// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <Engine/Scene/scene.h>

namespace tour {

// Registry of designated subtree roots in the ONE authoritative World scene.
// A region is an ownership / streaming boundary, NOT another Scene.
// Its root is a generation-checked node handle, so retiring and reusing a
// hierarchy slot cannot silently resurrect an old region.
//
// Nested registrations are permitted: the nearest registered ancestor wins.
// Region IDs are monotonic and are never recycled by this registry.
// This registry does NOT own GPU assets, pak images, or node memory;
// those lifetimes are tracked separately.
enum class RegionKind : uint8_t {
    Scene,
    PrefabInstance,
    Runtime
};

struct SceneRegion {
    uint64_t id = 0;
    NodeHandle root = kNodeInvalidHandle;
    RegionKind kind = RegionKind::Scene;
};

class RegionRegistry {
public:
    // Register an existing node as an ownership boundary. Returns 0 for an
    // invalid node or a root already registered with a live generation.
    // No scene topology is modified by this operation.
    uint64_t Register(const Scene& scene, NodeHandle root, RegionKind kind) {
        if (!NodeHandleValid(scene, root))
            return 0;
        const NodeId slot = NodeHandleSlot(root);
        auto it = byRoot_.find(slot);
        if (it != byRoot_.end()) {
            if (NodeHandleValid(scene, regions_[it->second].root))
                return 0;
            EraseAt(it->second); // stale registration from a retired slot
        }
        const uint64_t id = nextId_++;
        byRoot_[slot] = regions_.size();
        regions_.push_back({id, root, kind});
        return id;
    }

    // Unregister a boundary without deleting its subtree or releasing assets.
    bool Unregister(uint64_t id) {
        for (size_t i = 0; i < regions_.size(); ++i) {
            if (regions_[i].id == id) {
                EraseAt(i);
                return true;
            }
        }
        return false;
    }

    // Find the closest live region ancestor. Safe with retired slots:
    // a stale registered root is never reported for a reused node slot.
    const SceneRegion* RegionOf(const Scene& scene, NodeId node) const {
        if (!IsNodeAlive(scene, node))
            return nullptr;
        size_t guard = 0;
        while (IsNodeAlive(scene, node) && guard++ < scene.hierarchy.size()) {
            const auto it = byRoot_.find(node);
            if (it != byRoot_.end()) {
                const SceneRegion& region = regions_[it->second];
                if (NodeHandleValid(scene, region.root))
                    return &region;
            }
            const int parent = scene.hierarchy[node].parent;
            if (parent < 0)
                break;
            node = static_cast<NodeId>(parent);
        }
        return nullptr; // also handles malformed cyclic hierarchy
    }

    const SceneRegion* Find(uint64_t id, const Scene& scene) const {
        for (const SceneRegion& region : regions_) {
            if (region.id == id && NodeHandleValid(scene, region.root))
                return &region;
        }
        return nullptr;
    }

    // The editor may freely reorganize a subtree INSIDE one region.
    // Moving a registered root, or moving a subtree across ownership
    // boundaries, requires an explicit content transfer instead.
    bool CanReparent(const Scene& scene, NodeId node, NodeId newParent) const {
        if (!IsNodeAlive(scene, node) || !IsNodeAlive(scene, newParent))
            return false;
        if (byRoot_.contains(node)) {
            const auto& root = regions_[byRoot_.at(node)];
            if (NodeHandleValid(scene, root.root))
                return false;
        }
        const SceneRegion* source = RegionOf(scene, node);
        const SceneRegion* target = RegionOf(scene, newParent);
        return (source ? source->id : 0) == (target ? target->id : 0);
    }

    void PruneRetired(const Scene& scene) {
        for (size_t i = 0; i < regions_.size();) {
            if (!NodeHandleValid(scene, regions_[i].root))
                EraseAt(i);
            else
                ++i;
        }
    }

    const std::vector<SceneRegion>& All() const { return regions_; }
    bool Empty() const { return regions_.empty(); }

private:
    void EraseAt(size_t index) {
        byRoot_.erase(NodeHandleSlot(regions_[index].root));
        const size_t last = regions_.size() - 1;
        if (index != last) {
            regions_[index] = regions_[last];
            byRoot_[NodeHandleSlot(regions_[index].root)] = index;
        }
        regions_.pop_back();
    }

    uint64_t nextId_ = 1;
    std::vector<SceneRegion> regions_;
    std::unordered_map<NodeId, size_t> byRoot_;
};

} // namespace tour
