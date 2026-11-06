// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Incremental, World-owned local Scene HIERARCHY instances.
// Scene authoring assets are immutable inputs; only the one runtime Scene
// acquires nodes. NodeHandle generations survive unrelated tile retirement.
// AssetLease records are independent of the tree: removing one instance only
// releases its own references, not assets held by another instance.
//
// IMPORTANT: this first runtime path requires render/component row binding
// before it can accept a Scene with drawable/typed components. Rejecting
// unsupported associations is intentional: silently dropping meshes or
// emitting unbound indices would turn streaming into invisible/corrupt draws.

#include <Engine/Asset/asset_residency.h>
#include <Engine/Scene/region_registry.h>
#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace tour::scene {

struct WorldSceneInstance {
    uint64_t id = 0;
    uint64_t mapId = 0;
    uint64_t sceneAssetId = 0;
    uint64_t region = 0;
    NodeHandle root = kNodeInvalidHandle;
    // Dense source slots to live runtime handles; dead source slots = invalid.
    std::vector<NodeHandle> sourceNodes;
    std::vector<asset::AssetLease> assets;
};

class WorldSceneInstances {
public:
    // Map identity lives in the single active World, not another World.
    // Refuse to switch while any scene instance from the old Map survives.
    bool SetActiveMap(uint64_t id) {
        if (!instances_.empty()) return false;
        activeMap_ = id;
        return true;
    }
    uint64_t ActiveMap() const { return activeMap_; }
    size_t Count() const { return instances_.size(); }

    const WorldSceneInstance* Find(uint64_t id) const {
        const auto it = instances_.find(id);
        return it == instances_.end() ? nullptr : &it->second;
    }

    // The caller acquires all assets before this call (AssetResidency's
    // AcquireAll is atomic), and publishes render bindings separately after
    // the root is valid. No scene-level GPU resources are embedded here.
    uint64_t Instantiate(Scene& target, RegionRegistry& regions,
                         const Scene& source, uint64_t sceneAssetId,
                         const glm::dmat4& frame,
                         std::vector<asset::AssetLease> assets,
                         std::string& err) {
        err.clear();
        if (!activeMap_ || !sceneAssetId || nextId_ == 0) {
            err = "active Map and source Scene asset IDs are required";
            return 0;
        }
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(frame[c][r])) {
                    err = "nonfinite Scene placement";
                    return 0;
                }
        for (const auto& a : assets)
            if (!a || !a->contentId || !a->backing) {
                err = "invalid resource lease";
                return 0;
            }
        const auto& t = source.tables;
        if (!t.meshes.empty() || !t.materials.empty()
            || !t.lights.empty() || !t.cameras.empty()
            || !t.decals.empty() || !t.giVolumes.empty()
            || !t.cubemaps.empty() || !t.exports.empty()
            || !t.envs.empty() || !t.envVolumes.empty()
            || !t.audioSources.empty() || !t.audioRooms.empty()
            || !t.triggerVolumes.empty() || !t.physicsBodies.empty()
            || !t.schemaAssocs.empty()) {
            err = "Scene has typed bindings requiring the incremental resource/component publisher";
            return 0;
        }

        // Validate all parents, cycles, depths, and source transforms before
        // modifying target. The source can contain retired node slots.
        const size_t n = source.hierarchy.size();
        if (n >= kNodeSlotCountMax) {
            err = "Scene source slot count exceeds handle limit";
            return 0;
        }
        std::vector<int> depth(n, -1);
        std::vector<uint8_t> visit(n, 0);
        size_t live = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (!IsLiveSlot(source, i)) continue;
            ++live;
            if (i >= source.localTransform.size()) {
                err = "Scene transform missing for a live node";
                return 0;
            }
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (!std::isfinite(source.localTransform[i][c][r])) {
                        err = "nonfinite source transform";
                        return 0;
                    }
        }
        for (uint32_t i = 0; i < n; ++i) {
            if (!IsLiveSlot(source, i)) continue;
            std::vector<uint32_t> chain;
            int current = int(i);
            while (current >= 0 && depth[size_t(current)] < 0) {
                if (size_t(current) >= n
                    || !IsLiveSlot(source, uint32_t(current))) {
                    err = "Scene parent points outside live hierarchy";
                    return 0;
                }
                if (visit[size_t(current)] == 1) {
                    err = "Scene hierarchy cycle";
                    return 0;
                }
                visit[size_t(current)] = 1;
                chain.push_back(uint32_t(current));
                current = source.hierarchy[size_t(current)].parent;
            }
            while (!chain.empty()) {
                const uint32_t slot = chain.back();
                chain.pop_back();
                const int parent = source.hierarchy[slot].parent;
                depth[slot] = parent < 0 ? 1 : depth[size_t(parent)] + 1;
                if (depth[slot] >= MAX_NODE_LEVEL) {
                    err = "Scene hierarchy exceeds World level limit";
                    return 0;
                }
                visit[slot] = 2;
            }
        }
        const size_t needed = live + 1; // synthetic instance root
        const size_t size = target.hierarchy.size();
        const size_t reuse = target.nodeFreeList.size();
        const size_t growth = needed > reuse ? needed - reuse : 0;
        if (growth > kNodeSlotCountMax - 1 - size
            || (target.nodeSlotBudget
                && growth > target.nodeSlotBudget - std::min(size, target.nodeSlotBudget))) {
            err = "World instance would exceed scene node budget";
            return 0;
        }

        std::vector<uint32_t> order;
        order.reserve(live);
        for (uint32_t i = 0; i < n; ++i)
            if (IsLiveSlot(source, i)) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return depth[a] < depth[b] || (depth[a] == depth[b] && a < b);
        });

        WorldSceneInstance instance;
        instance.id = nextId_;
        instance.mapId = activeMap_;
        instance.sceneAssetId = sceneAssetId;
        instance.assets = std::move(assets);
        instance.sourceNodes.assign(n, kNodeInvalidHandle);
        const int root = addNode(target, -1, 0, false);
        instance.root = MakeNodeHandle(target, NodeId(root));
        target.localTransform[size_t(root)] = glm::mat4(frame);
        target.localTransformDouble[size_t(root)] = frame;
        setNodeName(target, root, "SceneInstance");
        const uint64_t region = regions.Register(
            target, instance.root, RegionKind::PrefabInstance);
        if (!region) {
            deleteSceneNodes(target, {uint32_t(root)});
            err = "failed to register incremental scene root";
            return 0;
        }
        instance.region = region;
        for (uint32_t src : order) {
            const int sourceParent = source.hierarchy[src].parent;
            const int parent = sourceParent < 0 ? root :
                int(NodeHandleSlot(instance.sourceNodes[size_t(sourceParent)]));
            const int dst = addNode(target, parent, depth[src], false);
            instance.sourceNodes[src] = MakeNodeHandle(target, NodeId(dst));
            target.localTransform[size_t(dst)] = source.localTransform[src];
            target.localTransformDouble[size_t(dst)] =
                src < source.localTransformDouble.size()
                ? source.localTransformDouble[src]
                : glm::dmat4(source.localTransform[src]);
            const std::string name = getNodeName(source, int(src));
            if (!name.empty()) setNodeName(target, dst, name);
            if (src < source.staticRoot.size())
                target.staticRoot[size_t(dst)] = source.staticRoot[src];
            if (src < source.noLightmap.size())
                target.noLightmap[size_t(dst)] = source.noLightmap[src];
            if (src < source.lightmap.size())
                target.lightmap[size_t(dst)] = source.lightmap[src];
            if (src < source.hidden.size())
                target.hidden[size_t(dst)] = source.hidden[src];
            if (src < source.ditherCloseup.size())
                target.ditherCloseup[size_t(dst)] = source.ditherCloseup[src];
            if (src < source.castsVolumetricShadow.size())
                target.castsVolumetricShadow[size_t(dst)] =
                    source.castsVolumetricShadow[src];
        }
        // Only the new subtree is marked; no ResolveAndFixup_clusters(),
        // source Scene copy, location-table rebuild, or unrelated ID remap.
        RebuildStaticCache(target);
        markAsChanged(target, root);
        recalculateGlobalTransforms(target);
        ++nextId_;
        const uint64_t id = instance.id;
        instances_.emplace(id, std::move(instance));
        return id;
    }

    bool Retire(Scene& target, RegionRegistry& regions, uint64_t id) {
        auto it = instances_.find(id);
        if (it == instances_.end()) return false;
        if (!NodeHandleValid(target, it->second.root)) return false;
        if (!regions.Unregister(it->second.region)) return false;
        // deleteSceneNodes only retires our root and descendants.
        // Surviving instance node slots (and their generations) never move.
        deleteSceneNodes(target, {NodeHandleSlot(it->second.root)});
        instances_.erase(it); // releases the instance's asset leases
        return true;
    }

private:
    uint64_t activeMap_ = 0;
    uint64_t nextId_ = 1;
    std::map<uint64_t, WorldSceneInstance> instances_;
};

} // namespace tour::scene
