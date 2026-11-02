// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// World -> GPU scene database bridge. No raster or GPU cull consumers yet.
// Source keys refer to cluster registrations and cluster-LOCAL nodes, not
// the unstable merged World NodeId or material-sorted drawItems index.

#include <Engine/App/world.h>
#include <Engine/Renderer/gpu_scene_upload.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tour::gpu_scene {

struct SourceKey {
    uint64_t registration = 0;
    uint32_t slot = InvalidIndex;
    uint32_t generation = 0;
    bool operator==(const SourceKey&) const = default;
};
struct SourceKeyHash {
    size_t operator()(const SourceKey& k) const noexcept {
        uint64_t h = k.registration ^ (uint64_t(k.slot) << 17)
                                     ^ (uint64_t(k.generation) << 39);
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 27;
        return size_t(h ^ (h >> 31));
    }
};
struct SourceRecord {
    SourceKey instance{};
    SourceKey mesh{};
    glm::mat4 globalModel{1.0f};
    glm::vec4 localSphere{0.0f};
    uint32_t material = InvalidIndex;
    uint32_t flags = 0;
};
struct SyncStats {
    uint32_t created = 0, updated = 0, retired = 0;
    uint32_t meshesCreated = 0, meshesRetired = 0;
};

// Pure CPU reconciler; exposed independently so lifetime/dirty semantics
// can be tested without constructing a Vulkan renderer or a cooked pak.
class SceneRegistry {
public:
    using InstanceId = Handle<Instance>;
    [[nodiscard]] InstanceId Find(SourceKey key) const {
        auto it = instances_.find(key);
        return it == instances_.end() ? InstanceId{} : it->second;
    }
    [[nodiscard]] uint32_t LiveCount() const {
        return static_cast<uint32_t>(instances_.size());
    }
    void Clear(RenderDatabase& db) {
        for (const auto& kv : instances_) (void)db.instances.Erase(kv.second);
        for (const auto& kv : meshes_) (void)db.meshes.Erase(kv.second);
        instances_.clear();
        meshes_.clear();
    }

    static void SetTransform(Instance& out, const glm::mat4& model) {
        // Existing Scene authority is mat4(float); DoubleFloat cannot
        // reconstruct world precision already lost before this boundary.
        out.worldPosition = tour::math::SplitDoubleFloat(
            glm::dvec3(glm::vec3(model[3])));
        out.basisX = glm::vec4(glm::vec3(model[0]), 0.0f);
        out.basisY = glm::vec4(glm::vec3(model[1]), 0.0f);
        out.basisZ = glm::vec4(glm::vec3(model[2]), 0.0f);
    }

    [[nodiscard]] bool UpdateGlobal(SourceKey key, const glm::mat4& model,
                                     RenderDatabase& db) {
        auto it = instances_.find(key);
        if (it == instances_.end()) return false;
        const Instance* existing = db.instances.Get(it->second);
        if (!existing) return false;
        Instance next = *existing;
        SetTransform(next, model);
        if (std::memcmp(existing, &next, sizeof(Instance)) == 0)
            return false;
        return db.instances.Update(it->second, next);
    }

    SyncStats Reconcile(std::span<const SourceRecord> sources,
                         RenderDatabase& db) {
        SyncStats stats{};
        std::unordered_set<SourceKey, SourceKeyHash> seen, usedMeshes;
        seen.reserve(sources.size());
        usedMeshes.reserve(sources.size());
        for (const SourceRecord& r : sources) {
            if (!r.instance.registration || !r.mesh.registration ||
                !seen.insert(r.instance).second)
                continue; // stale or duplicate source mapping
            usedMeshes.insert(r.mesh);
            auto mesh = meshes_.find(r.mesh);
            if (mesh == meshes_.end()) {
                // Placeholder deliberately advertises NO resident LOD.
                // Geometry and LOD tables are populated in a later PR.
                auto handle = db.meshes.Insert(Mesh{});
                if (!handle) continue;
                mesh = meshes_.emplace(r.mesh, handle).first;
                ++stats.meshesCreated;
            }
            Instance data{};
            SetTransform(data, r.globalModel);
            data.localSphere = r.localSphere; // LOD0 sphere only
            data.refs = glm::uvec4(mesh->second.slot, r.material,
                                    InvalidIndex, r.flags);
            auto it = instances_.find(r.instance);
            if (it == instances_.end()) {
                auto handle = db.instances.Insert(data);
                if (handle) {
                    instances_.emplace(r.instance, handle);
                    ++stats.created;
                }
            } else {
                const Instance* old = db.instances.Get(it->second);
                if (old && std::memcmp(old, &data, sizeof(data)) != 0 &&
                    db.instances.Update(it->second, data))
                    ++stats.updated;
            }
        }
        for (auto it = instances_.begin(); it != instances_.end();) {
            if (!seen.contains(it->first)) {
                (void)db.instances.Erase(it->second);
                it = instances_.erase(it);
                ++stats.retired;
            } else ++it;
        }
        for (auto it = meshes_.begin(); it != meshes_.end();) {
            if (!usedMeshes.contains(it->first)) {
                (void)db.meshes.Erase(it->second);
                it = meshes_.erase(it);
                ++stats.meshesRetired;
            } else ++it;
        }
        return stats;
    }

private:
    std::unordered_map<SourceKey, InstanceId, SourceKeyHash> instances_;
    std::unordered_map<SourceKey, Handle<Mesh>, SourceKeyHash> meshes_;
};

// Per-renderer adapter: one O(renderables + source nodes) pass per topology
// revision, then O(changed nodes) in steady state. Scene's change journal is
// consumed after global transform recalculation, not before.
class WorldSynchronizer {
public:
    SyncStats Synchronize(World& world, RenderDatabase& db) {
        if (world_ != &world) {
            Reset(db);
            world_ = &world;
        }
        SyncStats stats{};
        if (revision_ != world.gpuSceneTopologyRevision) {
            nodeKeys_.assign(world.scene.hierarchy.size(), {});
            for (size_t i = 0; i < world.clusters.size() &&
                               i < world.clusterMergedNodes.size(); ++i) {
                const Cluster* c = world.clusters[i].get();
                if (!c) continue;
                const auto owner = world.gpuSceneClusterIds.find(c);
                if (owner == world.gpuSceneClusterIds.end()) continue;
                const auto& nodes = world.clusterMergedNodes[i];
                for (uint32_t local = 0; local < nodes.size(); ++local) {
                    const uint32_t merged = nodes[local];
                    if (merged == InvalidIndex || merged >= nodeKeys_.size())
                        continue;
                    const uint32_t gen =
                        local < c->scene.nodeGeneration.size()
                            ? (c->scene.nodeGeneration[local] & kNodeGenMask)
                            : 0;
                    nodeKeys_[merged] = {owner->second, local, gen};
                }
            }
            std::vector<SourceRecord> records;
            records.reserve(world.drawItems.size());
            for (const World::RenderItem& item : world.drawItems) {
                if (item.node >= nodeKeys_.size() ||
                    item.node >= world.scene.globalTransform.size() ||
                    item.mesh >= world.meshLocations.size())
                    continue;
                const SourceKey key = nodeKeys_[item.node];
                if (!key.registration) continue;
                const World::MeshLocation& loc = world.meshLocations[item.mesh];
                if (!loc.cluster || !loc.handle.valid()) continue;
                const auto owner = world.gpuSceneClusterIds.find(loc.cluster);
                if (owner == world.gpuSceneClusterIds.end()) continue;
                SourceRecord record{};
                record.instance = key;
                record.mesh = {owner->second, loc.handle.index(), loc.handle.gen()};
                record.globalModel = world.scene.globalTransform[item.node];
                record.material = item.material; // interim World index
                record.flags = static_cast<uint32_t>(item.flags);
                if (const StaticMesh* mesh = world.GetMesh(item.mesh)) {
                    const glm::vec3 lo = mesh->geometry.aabbMin;
                    const glm::vec3 hi = mesh->geometry.aabbMax;
                    const glm::vec3 center = lo + (hi - lo) * 0.5f;
                    record.localSphere = glm::vec4(center,
                                                   glm::length(hi - center));
                }
                records.push_back(record);
            }
            stats = registry_.Reconcile(records, db);
            revision_ = world.gpuSceneTopologyRevision;
            world.scene.gpuSceneGlobalChanges.clear();
            std::fill(world.scene.gpuSceneGlobalQueued.begin(),
                      world.scene.gpuSceneGlobalQueued.end(), uint8_t(0));
            return stats;
        }
        // Recalc can append the same node multiple times before a frame.
        auto& dirty = world.scene.gpuSceneGlobalChanges;
        std::sort(dirty.begin(), dirty.end());
        dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
        for (NodeId node : dirty) {
            if (node < nodeKeys_.size() &&
                node < world.scene.globalTransform.size() &&
                registry_.UpdateGlobal(nodeKeys_[node],
                                       world.scene.globalTransform[node], db))
                ++stats.updated;
        }
        for (NodeId node : dirty)
            if (node < world.scene.gpuSceneGlobalQueued.size())
                world.scene.gpuSceneGlobalQueued[node] = 0;
        dirty.clear();
        return stats;
    }
    void Reset(RenderDatabase& db) {
        registry_.Clear(db);
        nodeKeys_.clear();
        revision_ = ~uint64_t(0);
        world_ = nullptr;
    }
    [[nodiscard]] SceneRegistry::InstanceId Find(SourceKey k) const {
        return registry_.Find(k);
    }

private:
    const World* world_ = nullptr;
    uint64_t revision_ = ~uint64_t(0);
    std::vector<SourceKey> nodeKeys_;
    SceneRegistry registry_;
};

} // namespace tour::gpu_scene
