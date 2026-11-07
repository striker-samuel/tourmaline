// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Cluster-free Map streaming. Content loading is a separate responsibility:
// the source must supply a decoded immutable Scene, stable resource leases,
// and valid World resource bindings before the hierarchy is published.
// Unsupported component types fail through World::InstantiateSceneHierarchy.
#include <Engine/App/world.h>
#include <Engine/WorldPartition/SceneTileProvider.h>
#include <Engine/WorldPartition/map_tile_pak.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine::worldpartition {

struct DirectScenePayload {
    std::shared_ptr<const Scene> scene;
    std::vector<tour::asset::AssetLease> assets;
    tour::scene::WorldSceneResourceMap resources;
};

// All callbacks run on the update thread. No GPU resource lifetime may end
// before the host's deferred retirement fence. The scene resolver must not
// mutate World; it prepares independent resource-backed scene data only.
class DirectMapSceneStreaming {
public:
    using ResolveScene = std::function<bool(uint64_t, DirectScenePayload&, std::string&)>;

    DirectMapSceneStreaming(tour::World& world, ResolveScene resolve)
        : world_(world), resolve_(std::move(resolve)) {
        provider_.SetCallbacks(
            [this](const SceneTileDescriptor& d, DesiredResidency, uint64_t ticket) {
                Load(d, ticket);
            },
            [this](const SceneTileDescriptor& d) { return Retire(d.tile); });
    }

    SceneTileProvider& Provider() { return provider_; }
    WorldId ActiveMap() const { return provider_.ActiveWorld(); }
    const std::string& LastError() const { return error_; }
    size_t ActiveTileCount() const { return active_.size(); }
    size_t PersistentInstanceCount() const { return persistent_.size(); }

    // Reader and resolver must outlive the binding; borrowed provider must
    // be unregistered from WorldPartition before this object is destroyed.
    bool BindMap(const smvk::PakReader& reader, WorldId mapId) {
        error_.clear();
        if (!mapId || !resolve_) {
            error_ = "nonzero Map ID and a direct Scene resolver are required";
            return false;
        }
        if (ActiveMap() || provider_.ResidentCount() || provider_.PendingCount() ||
            !active_.empty() || !persistent_.empty()) {
            error_ = "retire the prior Map before binding a replacement";
            return false;
        }
        std::vector<SceneTileDescriptor> descriptors;
        std::vector<tour::CookedMapPlacement> persistent;
        if (!tour::mapcook::DiscoverMapTiles(reader, mapId, descriptors) ||
            !tour::mapcook::ReadPersistentPlacements(reader, mapId, persistent)) {
            error_ = "missing or corrupt Map manifest/placement page";
            return false;
        }
        if (!world_.SetActiveMapIdentity(mapId)) {
            error_ = "World already owns a different active Map's instances";
            return false;
        }
        std::vector<uint64_t> created;
        if (!InstantiateRecords(persistent, created)) {
            Rollback(created);
            if (created.empty()) world_.SetActiveMapIdentity(0);
            return false;
        }
        if (!provider_.SetManifest(mapId, std::move(descriptors))) {
            error_ = "Scene tile manifest installation failed";
            Rollback(created);
            if (created.empty()) world_.SetActiveMapIdentity(0);
            return false;
        }
        persistent_ = std::move(created);
        reader_ = &reader;
        return true;
    }

    // Must be invoked only at a safe scene-retirement publication boundary;
    // the host continues to own GPU fences for all referenced assets.
    bool UnbindMap() {
        error_.clear();
        provider_.OnStreamingInterests(nullptr, 0);
        provider_.RetireFrame(0);
        if (!active_.empty()) {
            error_ = "streamed tiles could not all be retired";
            return false;
        }
        if (!Rollback(persistent_)) return false;
        if (!provider_.SetManifest(WorldId(0), {})) {
            error_ = "provider still has live tile requests";
            return false;
        }
        if (!world_.SetActiveMapIdentity(0)) {
            error_ = "World still contains direct scene instances";
            return false;
        }
        reader_ = nullptr;
        return true;
    }

private:
    bool Rollback(std::vector<uint64_t>& instances) {
        while (!instances.empty()) {
            if (!world_.RetireSceneHierarchy(instances.back())) {
                error_ = "World failed to retire direct scene instance";
                return false;
            }
            instances.pop_back();
        }
        return true;
    }

    bool InstantiateRecords(const std::vector<tour::CookedMapPlacement>& placements,
                            std::vector<uint64_t>& created) {
        for (const auto& placement : placements) {
            DirectScenePayload payload;
            std::string err;
            if (!resolve_(placement.sceneAssetId, payload, err) || !payload.scene) {
                error_ = "Scene resolver failed for " +
                    std::to_string(placement.sceneAssetId) + ": " + err;
                return false;
            }
            // World performs exact typed-resource and unsupported-component
            // validation; no placeholder resources or dropped nodes.
            const uint64_t instance = world_.InstantiateSceneHierarchy(
                *payload.scene, placement.sceneAssetId,
                placement.worldTransform, std::move(payload.assets), err, &payload.resources);
            if (!instance) {
                error_ = "direct Scene publication failed: " + err;
                return false;
            }
            created.push_back(instance);
        }
        return true;
    }

    void Load(const SceneTileDescriptor& descriptor, uint64_t ticket) {
        if (!reader_) {
            error_ = "Map tile reader is unbound";
            provider_.ResetPending(descriptor.tile, ticket);
            return;
        }
        std::vector<tour::CookedMapPlacement> records;
        if (!tour::mapcook::ReadMapTile(*reader_, descriptor.tile, records)) {
            error_ = "missing or corrupt Map tile";
            provider_.ResetPending(descriptor.tile, ticket);
            return;
        }
        std::vector<uint64_t> created;
        if (!InstantiateRecords(records, created)) {
            if (!Rollback(created)) return; // retain error + fail closed
            provider_.ResetPending(descriptor.tile, ticket);
            return;
        }
        if (!provider_.Activate(descriptor.tile, ticket)) {
            Rollback(created);
            return;
        }
        active_.emplace(descriptor.tile, std::move(created));
    }

    bool Retire(WorldTileKey tile) {
        auto it = active_.find(tile);
        if (it == active_.end()) return true;
        if (!Rollback(it->second)) return false;
        active_.erase(it);
        return true;
    }

    tour::World& world_;
    ResolveScene resolve_;
    const smvk::PakReader* reader_ = nullptr;
    SceneTileProvider provider_;
    std::map<WorldTileKey, std::vector<uint64_t>, WorldTileKeyLess> active_;
    std::vector<uint64_t> persistent_;
    std::string error_;
};

} // namespace engine::worldpartition
