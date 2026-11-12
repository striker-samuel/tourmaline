// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// World -> GPU scene database bridge. No raster or GPU cull consumers yet.
// Source keys refer to cluster registrations and cluster-LOCAL nodes, not
// the unstable merged World NodeId or material-sorted drawItems index.

#include <Engine/App/world.h>
#include <Engine/Memory/arena.h>
#include <Engine/Renderer/gpu_scene_leaf.h>
#include <Engine/Renderer/gpu_scene_upload.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <tuple>
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
        uint64_t h = k.registration ^ (uint64_t(k.slot) << 17) ^ (uint64_t(k.generation) << 39);
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 27;
        return size_t(h ^ (h >> 31));
    }
};
struct SourceRecord {
    SourceKey instance{};
    SourceKey mesh{};
    // Preserve authoritative double translation at the CPU -> GPU boundary.
    // The current World synchronizer can still supply glm::mat4 (converted
    // explicitly), but callers with doubles never round before splitting.
    glm::dmat4 globalModel{1.0};
    glm::vec4 localSphere{0.0f};
    uint32_t material = InvalidIndex;
    uint32_t flags = 0;
};
struct LevelSource {
    float simplifyError = 0.0f;
    uint32_t gfxMesh = InvalidIndex; // transient registration for THIS revision
    bool resident = false;           // physical payload is in GPU memory
    bool boundsKnown = false;        // including deferred metadata
};

struct SyncStats {
    uint32_t created = 0, updated = 0, retired = 0;
    uint32_t meshesCreated = 0, meshesRetired = 0;
    bool leafPublicationOk = true;
    bool topologyReconciled = false;
    bool geometryRefreshed = false;
    uint32_t geometryMeshesVisited = 0;
};

// Conservative readiness gate for *future* draw consumers. An alive
// instance alone is not enough: topology, bounds and a physically resident
// filterable submesh must also be published. This does NOT activate the
// experimental GPU-scene draw path; existing renderer fallback is unchanged.
// Row generations are checked for instance->mesh; LOD/submesh rows are
// currently addressed by contiguous slot only, so this is not yet an
// end-to-end generation/flight publication proof.
[[nodiscard]] inline bool InstanceDrawReady(const RenderDatabase& db, Handle<Instance> id) {
    const Instance* instance = db.instances.Get(id);
    if (!instance)
        return false;
    const Handle<Mesh> meshId{instance->refs.x, instance->refs.y};
    const Mesh* mesh = db.meshes.Get(meshId);
    if (!mesh || !(mesh->flags & MeshFlagCompleteBounds) || mesh->lodCount == 0 ||
        mesh->lodCount > 16 || mesh->firstLod >= db.lods.Size() ||
        mesh->lodCount > db.lods.Size() - mesh->firstLod)
        return false;
    bool ready = false;
    for (uint32_t level = 0; level < mesh->lodCount; ++level) {
        const auto& lodRow = db.lods.Rows()[mesh->firstLod + level];
        if (lodRow.identity.y != 1 || !(lodRow.value.flags & LodFlagBoundsKnown))
            return false;
        const bool resident = (mesh->residentMask & (1u << level)) != 0;
        if (resident != ((lodRow.value.flags & LodFlagResident) != 0))
            return false;
        if (!resident)
            continue;
        if (!(lodRow.value.flags & LodFlagFilterable) || lodRow.value.submeshCount != 1 ||
            lodRow.value.firstSubmesh >= db.submeshes.Size())
            return false;
        const auto& sub = db.submeshes.Rows()[lodRow.value.firstSubmesh];
        if (sub.identity.y != 1 || sub.value.geometryId == InvalidIndex)
            return false;
        ready = true;
    }
    return ready;
}

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
    // Return delta batches in the order "retire old identities, upsert
    // live identities"; the same slot may reappear with a new generation.
    struct Deltas {
        std::vector<InstanceId> changed;
        std::vector<InstanceId> retired;
    };
    [[nodiscard]] Deltas TakeDeltas() {
        Deltas result{};
        result.changed.swap(changed_);
        result.retired.swap(retired_);
        return result;
    }
    void Clear(RenderDatabase& db) {
        for (const auto& kv : instances_) {
            retired_.push_back(kv.second);
            (void)db.instances.Erase(kv.second);
        }
        for (auto& kv : meshes_)
            EraseMesh(kv.second, db);
        instances_.clear();
        meshes_.clear();
    }

    [[nodiscard]] Handle<Mesh> FindMesh(SourceKey key) const {
        const auto it = meshes_.find(key);
        return it == meshes_.end() ? Handle<Mesh>{} : it->second.id;
    }

    // Stable mesh slot, contiguous LOD span, and one submesh per LOD.
    // Publishing a streamed LOD changes residency/geometry fields IN PLACE;
    // chain reshaping replaces the dependent rows but retains mesh identity.
    // Returns false on invalid metadata / allocation failure. No GPU reader
    // can use these rows until the standard per-flight uploader completes.
    [[nodiscard]] bool PublishMesh(SourceKey key,
                                   std::span<const LevelSource> levels,
                                   bool allBoundsKnown,
                                   RenderDatabase& db) {
        auto it = meshes_.find(key);
        if (it == meshes_.end() || levels.empty() || levels.size() > 16)
            return false;
        MeshState& state = it->second;
        const size_t count = levels.size();
        if (state.count != count) {
            // Max LOD chain is 16, so both payloads and handles fit in a
            // fixed stack array. No heap vector or per-update allocation.
            std::array<Submesh, 16> subValues{};
            std::array<Lod, 16> lodValues{};
            std::array<Handle<Submesh>, 16> subs{};
            std::array<Handle<Lod>, 16> lods{};
            const auto subsIn = std::span<const Submesh>(subValues.data(), count);
            const auto subsOut = std::span<Handle<Submesh>>(subs.data(), count);
            if (!db.submeshes.InsertContiguous(subsIn, subsOut))
                return false;
            const auto lodsIn = std::span<const Lod>(lodValues.data(), count);
            const auto lodsOut = std::span<Handle<Lod>>(lods.data(), count);
            if (!db.lods.InsertContiguous(lodsIn, lodsOut)) {
                for (size_t i = 0; i < count; ++i)
                    (void)db.submeshes.Erase(subs[i]);
                return false;
            }
            for (size_t i = 0; i < state.count; ++i) {
                (void)db.lods.Erase(state.lods[i]);
                (void)db.submeshes.Erase(state.submeshes[i]);
            }
            state.lods = lods;
            state.submeshes = subs;
            state.count = static_cast<uint32_t>(count);
        }
        uint32_t residentMask = 0;
        for (uint32_t level = 0; level < count; ++level) {
            const LevelSource& src = levels[level];
            const bool filterable = src.resident && src.gfxMesh != InvalidIndex;
            if (src.resident)
                residentMask |= 1u << level;
            const Submesh sub{
                filterable ? src.gfxMesh : InvalidIndex, InvalidIndex, InvalidIndex, 0u};
            const Lod lod{state.submeshes[level].slot,
                          1u,
                          src.simplifyError,
                          (src.resident ? LodFlagResident : 0u) |
                              (src.boundsKnown ? LodFlagBoundsKnown : 0u) |
                              (filterable ? LodFlagFilterable : 0u)};
            const Submesh* prevSub = db.submeshes.Get(state.submeshes[level]);
            if (!prevSub || std::memcmp(prevSub, &sub, sizeof(sub)) != 0)
                (void)db.submeshes.Update(state.submeshes[level], sub);
            const Lod* prevLod = db.lods.Get(state.lods[level]);
            if (!prevLod || std::memcmp(prevLod, &lod, sizeof(lod)) != 0)
                (void)db.lods.Update(state.lods[level], lod);
        }
        const Mesh desc{state.lods.front().slot,
                        uint32_t(count),
                        residentMask,
                        allBoundsKnown ? MeshFlagCompleteBounds : 0u};
        const Mesh* old = db.meshes.Get(state.id);
        if (!old || std::memcmp(old, &desc, sizeof(desc)) != 0)
            (void)db.meshes.Update(state.id, desc);
        return true;
    }

    static void SetTransform(Instance& out, const glm::dmat4& model) {
        // The translation stays double until SplitDoubleFloat; only the
        // affine basis is rounded. Never convert the entire dmat4 to mat4.
        out.worldPosition = tour::math::SplitDoubleFloat(glm::dvec3(model[3]));
        out.basisX = glm::vec4(glm::vec3(model[0]), 0.0f);
        out.basisY = glm::vec4(glm::vec3(model[1]), 0.0f);
        out.basisZ = glm::vec4(glm::vec3(model[2]), 0.0f);
    }

    [[nodiscard]] bool UpdateGlobal(SourceKey key, const glm::dmat4& model, RenderDatabase& db) {
        auto it = instances_.find(key);
        if (it == instances_.end())
            return false;
        const Instance* existing = db.instances.Get(it->second);
        if (!existing)
            return false;
        Instance next = *existing;
        SetTransform(next, model);
        if (std::memcmp(existing, &next, sizeof(Instance)) == 0)
            return false;
        const bool updated = db.instances.Update(it->second, next);
        if (updated)
            changed_.push_back(it->second);
        return updated;
    }

    // Current Scene journals still publish float matrices. This adapter
    // is deliberate and isolated; the dmat4 overload is authoritative
    // when a double transform source is available.
    [[nodiscard]] bool UpdateGlobal(SourceKey key, const glm::mat4& model, RenderDatabase& db) {
        return UpdateGlobal(key, glm::dmat4(model), db);
    }

    SyncStats Reconcile(std::span<const SourceRecord> sources, RenderDatabase& db) {
        SyncStats stats{};
        std::unordered_set<SourceKey, SourceKeyHash> seen, usedMeshes;
        seen.reserve(sources.size());
        usedMeshes.reserve(sources.size());
        for (const SourceRecord& r : sources) {
            if (!r.instance.registration || !r.mesh.registration || !seen.insert(r.instance).second)
                continue; // stale or duplicate source mapping
            usedMeshes.insert(r.mesh);
            auto mesh = meshes_.find(r.mesh);
            if (mesh == meshes_.end()) {
                // Placeholder deliberately advertises NO resident LOD.
                // Geometry and LOD tables are populated in a later PR.
                auto handle = db.meshes.Insert(Mesh{});
                if (!handle)
                    continue;
                mesh = meshes_.emplace(r.mesh, MeshState{handle, {}, {}, 0}).first;
                ++stats.meshesCreated;
            }
            Instance data{};
            SetTransform(data, r.globalModel);
            data.localSphere = r.localSphere; // LOD0 sphere only
            data.refs =
                glm::uvec4(mesh->second.id.slot, mesh->second.id.generation, r.material, r.flags);
            auto it = instances_.find(r.instance);
            if (it == instances_.end()) {
                auto handle = db.instances.Insert(data);
                if (handle) {
                    instances_.emplace(r.instance, handle);
                    changed_.push_back(handle);
                    ++stats.created;
                }
            } else {
                const Instance* old = db.instances.Get(it->second);
                if (old && std::memcmp(old, &data, sizeof(data)) != 0 &&
                    db.instances.Update(it->second, data)) {
                    changed_.push_back(it->second);
                    ++stats.updated;
                }
            }
        }
        for (auto it = instances_.begin(); it != instances_.end();) {
            if (!seen.contains(it->first)) {
                retired_.push_back(it->second);
                (void)db.instances.Erase(it->second);
                it = instances_.erase(it);
                ++stats.retired;
            } else
                ++it;
        }
        for (auto it = meshes_.begin(); it != meshes_.end();) {
            if (!usedMeshes.contains(it->first)) {
                EraseMesh(it->second, db);
                it = meshes_.erase(it);
                ++stats.meshesRetired;
            } else
                ++it;
        }
        return stats;
    }

  private:
    struct MeshState {
        Handle<Mesh> id{};
        std::array<Handle<Lod>, 16> lods{};
        std::array<Handle<Submesh>, 16> submeshes{};
        uint32_t count = 0;
    };
    static void EraseMesh(MeshState& state, RenderDatabase& db) {
        for (uint32_t i = 0; i < state.count; ++i) {
            (void)db.lods.Erase(state.lods[i]);
            (void)db.submeshes.Erase(state.submeshes[i]);
        }
        (void)db.meshes.Erase(state.id);
        state.count = 0;
    }
    std::vector<InstanceId> changed_;
    std::vector<InstanceId> retired_;
    std::unordered_map<SourceKey, InstanceId, SourceKeyHash> instances_;
    std::unordered_map<SourceKey, MeshState, SourceKeyHash> meshes_;
};

// Per-renderer adapter: one O(renderables + source nodes) pass per topology
// revision, then O(changed nodes) in steady state. Scene's change journal is
// consumed after global transform recalculation, not before.
class WorldSynchronizer {
  public:
    using GeometryLookup = std::function<uint32_t(uint32_t, uint32_t)>;
    SyncStats Synchronize(World& world,
                          RenderDatabase& db,
                          uint64_t geometryRevision = 0,
                          const GeometryLookup& geometryLookup = {}) {
        if (world_ != &world) {
            Reset(db);
            world_ = &world;
        }
        SyncStats stats{};
        if (revision_ != world.gpuSceneTopologyRevision) {
            // Persistent source identity mapping: AppArena, not a heap vector.
            const size_t nodeCount = world.scene.hierarchy.size();
            if (nodeCount > nodeKeyCapacity_) {
                nodeKeys_ = static_cast<SourceKey*>(
                    smvk::AppArena().Allocate(nodeCount * sizeof(SourceKey), alignof(SourceKey)));
                std::uninitialized_value_construct_n(nodeKeys_, nodeCount);
                nodeKeyCapacity_ = nodeCount;
            }
            nodeKeyCount_ = nodeCount;
            if (nodeKeyCount_)
                std::fill_n(nodeKeys_, nodeKeyCount_, SourceKey{});
            for (size_t i = 0; i < world.clusters.size() && i < world.clusterMergedNodes.size();
                 ++i) {
                const Cluster* c = world.clusters[i].get();
                if (!c)
                    continue;
                const auto owner = world.gpuSceneClusterIds.find(c);
                if (owner == world.gpuSceneClusterIds.end())
                    continue;
                const auto& nodes = world.clusterMergedNodes[i];
                for (uint32_t local = 0; local < nodes.size(); ++local) {
                    const uint32_t merged = nodes[local];
                    if (merged == InvalidIndex || merged >= nodeKeyCount_)
                        continue;
                    const uint32_t gen = local < c->scene.nodeGeneration.size()
                                             ? (c->scene.nodeGeneration[local] & kNodeGenMask)
                                             : 0;
                    nodeKeys_[merged] = {owner->second, local, gen};
                }
            }
            // Direct gameplay nodes are World-owned, not Cluster-local. Give them
            // a distinct registration allocated from the same namespace as
            // Cluster registrations; the generational node handle prevents ABA
            // when a removed gameplay slot is reused. The mesh below still
            // resolves through its actual Cluster-backed resource owner.
            for (NodeHandle handle : world.gameNodes_) {
                if (!world.IsGameNode(handle))
                    continue;
                const uint32_t slot = NodeHandleSlot(handle);
                if (slot < nodeKeyCount_ && !nodeKeys_[slot].registration)
                    nodeKeys_[slot] = {world.gpuSceneGameNodeRegistration, slot,
                                       NodeGenerationOf(world.scene, slot)};
            }
            // The source-record list is only consumed by Reconcile below.
            // Allocate it from the current FrameArena; no persistent heap vector.
            const size_t maxRecords = world.drawItems.size();
            auto* records = maxRecords
                ? static_cast<SourceRecord*>(smvk::FrameArena().Allocate(
                      maxRecords * sizeof(SourceRecord), alignof(SourceRecord)))
                : nullptr;
            auto* scratchMeshes = maxRecords
                ? static_cast<MeshSourceEntry*>(smvk::FrameArena().Allocate(
                      maxRecords * sizeof(MeshSourceEntry), alignof(MeshSourceEntry)))
                : nullptr;
            size_t recordCount = 0;
            size_t meshCount = 0;
            for (const World::RenderItem& item : world.drawItems) {
                if (item.node >= nodeKeyCount_ ||
                    item.node >= world.scene.globalTransform.size() ||
                    item.mesh >= world.meshLocations.size())
                    continue;
                const SourceKey key = nodeKeys_[item.node];
                if (!key.registration)
                    continue;
                const World::MeshLocation& loc = world.meshLocations[item.mesh];
                if (!loc.cluster || !loc.handle.valid())
                    continue;
                const auto owner = world.gpuSceneClusterIds.find(loc.cluster);
                if (owner == world.gpuSceneClusterIds.end())
                    continue;
                SourceRecord record{};
                record.instance = key;
                record.mesh = {owner->second, loc.handle.index(), loc.handle.gen()};
                std::construct_at(scratchMeshes + meshCount++, MeshSourceEntry{record.mesh, item.mesh});
                record.globalModel = tour::scene::GlobalTransformDouble(world.scene, item.node);
                record.material = item.material; // interim World index
                // Closeup fades need a discarding MASK depth pass, even if the
                // authored material is opaque. The FS applies the distance
                // factor every frame, so no per-camera CPU publication is
                // required for the fade band.
                record.flags = WithCloseupDitherRasterFlags(
                    static_cast<uint32_t>(item.flags),
                    tour::scene::IsDitherCloseupNode(world.scene, int(item.node)));
                // Per-instance pipeline classification, independent of
                // mesh sharing. Keep legacy admission until both raster
                // and depth PSO bins are wired end-to-end.
                if (const auto* material = world.GetMaterial(item.material);
                    material && material->doubleSided)
                    record.flags |= InstanceFlagDoubleSided;
                // Union the bounds of EVERY cooked LOD, including payloads
                // that are currently deferred. Bounds must not shrink on
                // eviction or the GPU would cull geometry after promotion.
                if (item.mesh < world.lodChains.size()) {
                    const auto& levels = world.lodChains[item.mesh].levels;
                    bool anyBounds = false;
                    glm::vec3 bmin(0.0f), bmax(0.0f);
                    for (const auto& level : levels) {
                        if (!level.boundsKnown)
                            continue;
                        if (!anyBounds) {
                            bmin = level.boundsMin;
                            bmax = level.boundsMax;
                            anyBounds = true;
                        } else {
                            bmin = glm::min(bmin, level.boundsMin);
                            bmax = glm::max(bmax, level.boundsMax);
                        }
                    }
                    if (anyBounds) {
                        const glm::vec3 center = bmin + (bmax - bmin) * 0.5f;
                        // Conservative across float32 rounding: the sphere
                        // must contain every AABB corner, including after
                        // the center was rounded to float32. Round the
                        // resulting radius up rather than letting streamed
                        // LOD admission spuriously fail by one ULP.
                        const glm::dvec3 c(center);
                        const glm::dvec3 lo(bmin), hi(bmax);
                        const glm::dvec3 reach =
                            glm::max(glm::abs(lo - c), glm::abs(hi - c));
                        const float radius = std::nextafter(
                            static_cast<float>(glm::length(reach)),
                            std::numeric_limits<float>::infinity());
                        record.localSphere = glm::vec4(center, radius);
                    }
                }
                new (records + recordCount++) SourceRecord(record);
            }
            stats = registry_.Reconcile(std::span<const SourceRecord>(records, recordCount), db);
            std::destroy_n(records, recordCount);
            stats.topologyReconciled = true;
            // Retain a sorted, de-duplicated mesh source map in AppArena.
            // Geometry-only revisions must never rebuild it from drawItems.
            auto meshLess = [](const MeshSourceEntry& a, const MeshSourceEntry& b) {
                return std::tie(a.source.registration, a.source.slot, a.source.generation) <
                       std::tie(b.source.registration, b.source.slot, b.source.generation);
            };
            if (meshCount > 1)
                std::sort(scratchMeshes, scratchMeshes + meshCount, meshLess);
            size_t uniqueCount = 0;
            for (size_t i = 0; i < meshCount; ++i) {
                if (uniqueCount == 0 || !(scratchMeshes[i].source == scratchMeshes[uniqueCount - 1].source))
                    scratchMeshes[uniqueCount++] = scratchMeshes[i];
            }
            if (uniqueCount > meshSourceCapacity_) {
                meshSources_ = static_cast<MeshSourceEntry*>(
                    smvk::AppArena().Allocate(uniqueCount * sizeof(MeshSourceEntry),
                                              alignof(MeshSourceEntry)));
                std::uninitialized_value_construct_n(meshSources_, uniqueCount);
                meshSourceCapacity_ = uniqueCount;
            }
            meshSourceCount_ = uniqueCount;
            if (uniqueCount)
                std::copy_n(scratchMeshes, uniqueCount, meshSources_);
            std::destroy_n(scratchMeshes, meshCount);
            stats.geometryMeshesVisited = PublishGeometry(world, db, geometryLookup);
            stats.geometryRefreshed = true;
            const bool meshPublicationChanged = geometryRevision_ != geometryRevision;
            revision_ = world.gpuSceneTopologyRevision;
            geometryRevision_ = geometryRevision;
            stats.leafPublicationOk = SyncLeaves(db, meshPublicationChanged);
            world.scene.gpuSceneGlobalChanges.clear();
            std::fill(world.scene.gpuSceneGlobalQueued.begin(),
                      world.scene.gpuSceneGlobalQueued.end(),
                      uint8_t(0));
            return stats;
        }
        // A geometry-only revision updates shared mesh/LOD/submesh rows without
        // scanning drawItems, rebuilding nodeKeys, or reconciling instances.
        const bool meshPublicationChanged = geometryRevision_ != geometryRevision;
        if (meshPublicationChanged) {
            stats.geometryMeshesVisited = PublishGeometry(world, db, geometryLookup);
            stats.geometryRefreshed = true;
            geometryRevision_ = geometryRevision;
        }
        // Recalc can append the same node multiple times before a frame.
        auto& dirty = world.scene.gpuSceneGlobalChanges;
        std::sort(dirty.begin(), dirty.end());
        dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
        for (NodeId node : dirty) {
            if (node < nodeKeyCount_ && node < world.scene.globalTransform.size() &&
                registry_.UpdateGlobal(
                    nodeKeys_[node], tour::scene::GlobalTransformDouble(world.scene, node), db))
                ++stats.updated;
        }
        for (NodeId node : dirty)
            if (node < world.scene.gpuSceneGlobalQueued.size())
                world.scene.gpuSceneGlobalQueued[node] = 0;
        dirty.clear();
        stats.leafPublicationOk = SyncLeaves(db, meshPublicationChanged);
        return stats;
    }
    void Reset(RenderDatabase& db) {
        leafRegistry_.Clear(db);
        registry_.Clear(db);
        (void)registry_.TakeDeltas();
        nodeKeyCount_ = 0;
        meshSourceCount_ = 0; // Retain AppArena capacity for subsequent Worlds.
        revision_ = ~uint64_t(0);
        geometryRevision_ = ~uint64_t(0);
        world_ = nullptr;
    }
    [[nodiscard]] std::vector<LeafTicket> LeafTickets(const RenderDatabase& db) const {
        return leafRegistry_.Tickets(db);
    }
    [[nodiscard]] std::span<const LeafTicket> LeafTicketsFrame(
        const RenderDatabase& db) const {
        return leafRegistry_.TicketsFrame(db);
    }
    [[nodiscard]] size_t LeafCount() const {
        return leafRegistry_.LeafCount();
    }
    [[nodiscard]] SceneRegistry::InstanceId Find(SourceKey k) const {
        return registry_.Find(k);
    }
    // Canonical cluster-local source identity for a merged World node.
    // Only valid for the current topology revision; callers must never
    // persist the merged NodeId itself as an ownership key.
    [[nodiscard]] SourceKey SourceForNode(NodeId node) const noexcept {
        return node < nodeKeyCount_ ? nodeKeys_[node] : SourceKey{};
    }

  private:
    uint32_t PublishGeometry(World& world, RenderDatabase& db, const GeometryLookup& geometryLookup) {
        uint32_t visited = 0;
        // The source-to-mesh mapping is refreshed by topology reconciliation.
        // Geometry revisions must not force another O(instances) scan.
        for (size_t meshIdx = 0; meshIdx < meshSourceCount_; ++meshIdx) {
            const auto& [source, worldIndex] = meshSources_[meshIdx];
            if (worldIndex >= world.lodChains.size())
                continue;
            const auto& chain = world.lodChains[worldIndex].levels;
            if (chain.empty() || chain.size() > 16)
                continue;
            // LOD chains are capped at 16; keep this scratch on the stack.
            std::array<LevelSource, 16> levels{};
            bool complete = true;
            // Animated vertex positions can leave the static cooked
            // mesh bounds: until skinning publishes dynamic bounds,
            // never reject the whole leaf using the bind-pose sphere.
            if (const StaticMesh* base = world.GetMesh(worldIndex))
                complete = !base->skinned;
            for (uint32_t i = 0; i < chain.size(); ++i) {
                const auto& l = chain[i];
                complete &= l.boundsKnown;
                const uint32_t gfxId =
                    geometryLookup ? geometryLookup(worldIndex, i) : InvalidIndex;
                levels[i] = LevelSource{l.error, gfxId, l.resident, l.boundsKnown};
            }
            (void)registry_.PublishMesh(source,
                                        std::span<const LevelSource>(levels.data(), chain.size()),
                                        complete, db);
            ++visited;
        }
        return visited;
    }
    [[nodiscard]] bool SyncLeaves(RenderDatabase& db, bool meshChanged) {
        SceneRegistry::Deltas changes = registry_.TakeDeltas();
        for (auto id : changes.retired)
            leafRegistry_.Remove(id);
        for (auto id : changes.changed)
            leafRegistry_.Upsert(id, db);
        if (meshChanged)
            leafRegistry_.RefreshSafety();
        return leafRegistry_.Flush(db);
    }

    LeafRegistry leafRegistry_;
    const World* world_ = nullptr;
    uint64_t revision_ = ~uint64_t(0);
    uint64_t geometryRevision_ = ~uint64_t(0);
    struct MeshSourceEntry {
        SourceKey source{};
        uint32_t worldIndex = InvalidIndex;
    };
    SourceKey* nodeKeys_ = nullptr; // AppArena-owned, retained across topology refreshes
    size_t nodeKeyCount_ = 0, nodeKeyCapacity_ = 0;
    MeshSourceEntry* meshSources_ = nullptr; // AppArena-owned, persistent
    size_t meshSourceCount_ = 0, meshSourceCapacity_ = 0;
    SceneRegistry registry_;
};

} // namespace tour::gpu_scene
