// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Host-side bridge: drives real ProjectScenes instances from one active Map's
// paged SMP8 tile content. Deliberately explicit about its temporary use of
// ProjectScenes' legacy Cluster loader; replacing that loader with incremental
// World mutation is the Epoch-5 cutover, NOT accomplished by this bridge.
#include "ProjectScenes.h"

#include <Engine/App/world.h>
#include <Engine/WorldPartition/map_tile_pak.h>
#include <Engine/WorldPartition/SceneTileProvider.h>

#include <glm/glm.hpp>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace app {

class MapSceneStreaming {
public:
    MapSceneStreaming(ProjectScenes& scenes, tour::World& world,
                      smvk::ResourceManager* context,
                      tour::TextureStreamer* streamer)
        : scenes_(scenes), world_(world), context_(context), streamer_(streamer) {
        provider_.SetCallbacks(
            [this](const engine::worldpartition::SceneTileDescriptor& desc,
                   engine::worldpartition::DesiredResidency, uint64_t ticket) {
                Load(desc, ticket);
            },
            [this](const engine::worldpartition::SceneTileDescriptor& desc) {
                Retire(desc.tile);
            });
    }

    // Borrowed provider: host calls WorldPartition::AddProvider(&Provider())
    // and must ClearProviders before destruction. All methods run on the
    // render/update thread; no background Scene mutation is permitted.
    engine::worldpartition::SceneTileProvider& Provider() { return provider_; }
    engine::worldpartition::WorldId ActiveMap() const {
        return provider_.ActiveWorld();
    }
    const std::string& LastError() const { return error_; }

    // Bind a different Map only after deactivating the previous one.
    // The pak reader must outlive this streaming service.
    bool BindMap(const smvk::PakReader& reader,
                 engine::worldpartition::WorldId worldId) {
        error_.clear();
        if (!worldId) { error_ = "Map identity must be nonzero"; return false; }
        if (provider_.ResidentCount() || provider_.PendingCount()
            || !active_.empty()) {
            error_ = "retire the active Map before binding a replacement";
            return false;
        }
        std::vector<engine::worldpartition::SceneTileDescriptor> descriptors;
        if (!tour::mapcook::DiscoverMapTiles(reader, worldId, descriptors)) {
            error_ = "invalid MapTile manifest";
            return false;
        }
        if (!provider_.SetManifest(std::move(descriptors))) {
            error_ = "failed to install MapTile manifest";
            return false;
        }
        reader_ = &reader;
        return true;
    }

    // Switch-off contract: remove all scene instances before swapping the
    // source reader. The caller must ensure GPU submission retirement using
    // the renderer's existing synchronization before releasing assets.
    bool UnbindMap() {
        error_.clear();
        provider_.OnStreamingInterests(nullptr, 0);
        provider_.RetireFrame(0);
        if (!active_.empty()) {
            error_ = "Map retirement left active scene instances: " +
                std::to_string(active_.size());
            return false;
        }
        if (!provider_.SetManifest({})) {
            error_ = "Map provider still has pending or resident tiles";
            return false;
        }
        reader_ = nullptr;
        return true;
    }

private:
    void Load(const engine::worldpartition::SceneTileDescriptor& descriptor,
              uint64_t ticket) {
        if (!reader_) {
            error_ = "MapTile reader not bound";
            provider_.ResetPending(descriptor.tile, ticket);
            return;
        }
        std::vector<tour::CookedMapPlacement> records;
        if (!tour::mapcook::ReadMapTile(*reader_, descriptor.tile, records)) {
            error_ = "MapTile missing or corrupt";
            provider_.ResetPending(descriptor.tile, ticket);
            return;
        }
        std::vector<SceneHandle> created;
        created.reserve(records.size());
        for (const auto& record : records) {
            // Current world Scene transforms are float matrices; reject
            // overflow rather than silently losing an unbounded map pose.
            glm::mat4 frame(1.0f);
            bool valid = true;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) {
                    const double value = record.worldTransform[c][r];
                    if (!std::isfinite(value)
                        || std::abs(value) > double(3.402823466e38f))
                        valid = false;
                    frame[c][r] = static_cast<float>(value);
                }
            if (!valid) {
                error_ = "Map placement transform exceeds runtime precision";
                break;
            }
            std::string err;
            const SceneHandle scene = scenes_.InstantiatePrefab(
                world_, context_, streamer_, record.sceneAssetId, frame, err);
            if (!scene) {
                error_ = "failed to instantiate placed Scene " +
                    std::to_string(record.sceneAssetId) + ": " + err;
                break;
            }
            created.push_back(scene);
        }
        if (created.size() != records.size()) {
            for (auto it = created.rbegin(); it != created.rend(); ++it) {
                std::string err;
                scenes_.RemoveScene(*it, world_, err);
            }
            provider_.ResetPending(descriptor.tile, ticket);
            return;
        }
        if (!provider_.Activate(descriptor.tile, ticket)) {
            for (auto it = created.rbegin(); it != created.rend(); ++it) {
                std::string err;
                scenes_.RemoveScene(*it, world_, err);
            }
            return;
        }
        active_.emplace(descriptor.tile, std::move(created));
    }

    void Retire(engine::worldpartition::WorldTileKey tile) {
        const auto it = active_.find(tile);
        if (it == active_.end()) return;
        for (auto scene = it->second.rbegin(); scene != it->second.rend(); ++scene) {
            std::string err;
            if (!scenes_.RemoveScene(*scene, world_, err)) {
                error_ = "MapTile scene retirement failed: " + err;
                return;
            }
        }
        active_.erase(it);
    }

    ProjectScenes& scenes_;
    tour::World& world_;
    smvk::ResourceManager* context_ = nullptr;
    tour::TextureStreamer* streamer_ = nullptr;
    const smvk::PakReader* reader_ = nullptr;
    engine::worldpartition::SceneTileProvider provider_;
    std::map<engine::worldpartition::WorldTileKey,
             std::vector<SceneHandle>,
             engine::worldpartition::WorldTileKeyLess> active_;
    std::string error_;
};

} // namespace app
