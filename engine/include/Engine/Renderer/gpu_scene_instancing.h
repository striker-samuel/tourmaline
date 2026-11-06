// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// CPU reference planner for a future GPU-scene-driven instanced raster path.
// This is NOT a draw consumer and does not replace gfx_filter's indirect path.
// Candidates are selected per view; persistent identity lives ONLY in the DB.

#include <Engine/Renderer/gpu_scene_world.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace tour::gpu_scene {

struct InstanceDrawCandidate {
    Handle<Instance> instance{};
    uint32_t lod = 0; // selected per-instance LOD; never assume LOD0
};

struct InstanceDrawKey {
    uint32_t meshSlot = InvalidIndex;
    uint32_t meshGeneration = 0;
    uint32_t lod = 0;
    uint32_t geometry = InvalidIndex;
    uint32_t material = InvalidIndex;
    uint32_t submeshMaterial = InvalidIndex;
    uint32_t pipelineGroup = InvalidIndex;
    uint32_t passMask = 0;
    uint32_t instanceFlags = 0;
    [[nodiscard]] auto Tie() const {
        return std::tie(meshSlot, meshGeneration, lod, geometry, material,
                        submeshMaterial, pipelineGroup, passMask, instanceFlags);
    }
    [[nodiscard]] bool operator<(const InstanceDrawKey& rhs) const {
        return Tie() < rhs.Tie();
    }
    [[nodiscard]] bool operator==(const InstanceDrawKey& rhs) const {
        return Tie() == rhs.Tie();
    }
};

// The refs array is a transient, per-view/per-flight output; every element
// retains slot + generation, never a naked slot that can alias a recycled row.
// Range offsets index refs, not persistent DB tables.
struct InstanceDrawRange {
    InstanceDrawKey key{};
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
};
struct InstanceDrawPlan {
    std::vector<Handle<Instance>> refs;
    std::vector<InstanceDrawRange> ranges;
    uint32_t rejected = 0;
};

// Reference-only planner. Inputs must ALREADY have passed the desired
// per-instance visibility test; this function does no camera/GPU culling.
// A mesh with missing bounds, an unpublished LOD, or a stale generation
// fails closed and remains eligible for the EXISTING direct fallback.
// Note: DB has no complete lightmap/skin/raster binding contract yet; this
// plan MUST NOT be submitted as real indirect commands until those are
// either validated here or consumed as per-instance shader data.
[[nodiscard]] inline InstanceDrawPlan BuildInstanceDrawPlan(
    const RenderDatabase& db, std::span<const InstanceDrawCandidate> candidates) {
    InstanceDrawPlan plan{};
    std::map<InstanceDrawKey, std::vector<Handle<Instance>>> grouped;
    for (const auto& candidate : candidates) {
        const Instance* instance = db.instances.Get(candidate.instance);
        if (!instance || !InstanceDrawReady(db, candidate.instance)) {
            ++plan.rejected;
            continue;
        }
        const Handle<Mesh> meshId{instance->refs.x, instance->refs.y};
        const Mesh* mesh = db.meshes.Get(meshId);
        if (!mesh || candidate.lod >= mesh->lodCount ||
            (mesh->residentMask & (1u << candidate.lod)) == 0u) {
            ++plan.rejected;
            continue;
        }
        const auto& lodRow = db.lods.Rows()[mesh->firstLod + candidate.lod];
        const Lod& lod = lodRow.value;
        if (lodRow.identity.y != 1u ||
            !(lod.flags & LodFlagResident) ||
            !(lod.flags & LodFlagFilterable) ||
            lod.submeshCount != 1u ||
            lod.firstSubmesh >= db.submeshes.Size()) {
            ++plan.rejected;
            continue;
        }
        const auto& subRow = db.submeshes.Rows()[lod.firstSubmesh];
        const Submesh& sub = subRow.value;
        if (subRow.identity.y != 1u || sub.geometryId == InvalidIndex) {
            ++plan.rejected;
            continue;
        }
        const InstanceDrawKey key{meshId.slot, meshId.generation,
            candidate.lod, sub.geometryId, instance->refs.z,
            sub.materialId, sub.pipelineGroup, sub.passMask, instance->refs.w};
        grouped[key].push_back(candidate.instance);
    }
    for (const auto& [key, refs] : grouped) {
        plan.ranges.push_back({key, static_cast<uint32_t>(plan.refs.size()),
                               static_cast<uint32_t>(refs.size())});
        plan.refs.insert(plan.refs.end(), refs.begin(), refs.end());
    }
    return plan;
}

} // namespace tour::gpu_scene
