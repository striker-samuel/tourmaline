// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// CPU reference oracle for the P1 flat GPU multi-view leaf visibility
// pipeline (#372, docs/gpu_scene_visibility_hierarchy_proposal.md):
//
//   gpu_scene_leaf_classify.cs.hlsl  -> per-leaf view mask
//   gpu_scene_leaf_compact.cs.hlsl   -> active-leaf work queue
//   gpu_scene_fused_cull.cs.hlsl     -> per-instance fused view mask
//
// Mirrors the three shaders exactly so the GPU runtime's test-mode readback
// can compare against it (the RFC's "test mode only" parity contract). This is
// the reference path only — heap-allocated, never the hot per-frame path.

#include <Engine/Renderer/gpu_scene_candidates.h>
#include <Engine/Renderer/gpu_scene_fused_cull.h>
#include <Engine/Renderer/gpu_scene_leaf.h>
#include <Engine/Renderer/gpu_scene_leaf_compact.h>

#include <cstdint>
#include <span>
#include <vector>

namespace tour::gpu_scene {

// The shaders test (1u << 5) as the cast-shadow bit (World iterator flag
// territory, below the GPU-scene raster-classification bits); mirror it
// directly rather than inventing a parallel constant.
inline constexpr uint32_t CastShadowBit = 1u << 5;

// One result per dense leaf (mask), one entry per active leaf (queue), and one
// verdict per live instance slot (sparse — the fused cull writes by slot).
struct VisibilityReference {
    std::vector<uint32_t> leafMasks{};      // per dense leaf index
    std::vector<LeafWorkEntry> workQueue{}; // active leaves, dense order
    std::vector<FusedVerdict> verdicts{};   // per instance slot (sparse)
    uint32_t activeLeafCount = 0;
};

// ownerGenerations is REQUIRED and sized to db.instances.Size() — the fused
// cull shader reads it unconditionally (no null fast-path). Only an exact
// (slot, generation) match may emit a verdict.
[[nodiscard]] inline VisibilityReference ReferenceVisibility(
    const RenderDatabase& db,
    const DenseLeaves& dense,
    std::span<const RelativeFrustum> views,
    std::span<const uint32_t> ownerGenerations) {
    VisibilityReference out{};
    out.leafMasks.assign(dense.count, 0u);
    out.verdicts.assign(db.instances.Size(), FusedVerdict{});

    // classify: one mask bit per view, mirroring LeafInView.
    for (uint32_t i = 0; i < dense.count; ++i) {
        if (dense.rows[i].identity.y != 1u)
            continue;
        uint32_t mask = 0u;
        for (size_t v = 0; v < views.size(); ++v)
            if (PossibleLeafInFrustum(dense.rows[i].value, views[v]))
                mask |= (1u << v);
        out.leafMasks[i] = mask;
    }

    // compact: active leaves (mask != 0) in dense order, one entry each.
    for (uint32_t i = 0; i < dense.count; ++i) {
        if (out.leafMasks[i] == 0u)
            continue;
        const Leaf& leaf = dense.rows[i].value;
        LeafWorkEntry entry{};
        entry.value = glm::uvec4(i, out.leafMasks[i], leaf.range.x, leaf.range.y);
        out.workQueue.push_back(entry);
    }
    out.activeLeafCount = static_cast<uint32_t>(out.workQueue.size());

    // fused cull: walk each active leaf's member range once, fold per-view
    // visibility into a single verdict keyed by instance slot.
    for (const LeafWorkEntry& entry : out.workQueue) {
        const uint32_t leafViewMask = entry.value.y;
        const uint32_t firstMember = entry.value.z;
        const uint32_t memberCount = entry.value.w;
        for (uint32_t i = 0; i < memberCount; ++i) {
            const uint32_t memberIndex = firstMember + i;
            if (memberIndex >= db.leafMembers.Size())
                break;
            const auto& member = db.leafMembers.Rows()[memberIndex];
            if (member.identity.y != 1u)
                continue;
            const uint32_t slot = member.value.ref.x;
            const uint32_t gen = member.value.ref.y;
            if (slot >= db.instances.Size() || ownerGenerations.size() <= slot)
                continue;
            const Instance* inst = db.instances.Get({slot, gen});
            if (!inst)
                continue;
            if (ownerGenerations[slot] != gen)
                continue;
            const bool castsShadow = (inst->refs.w & CastShadowBit) != 0u;
            uint32_t viewMask = 0u;
            for (size_t v = 0; v < views.size(); ++v) {
                if ((leafViewMask & (1u << v)) == 0u)
                    continue;
                if (ReferenceInstanceVisible(db, {slot, gen}, views[v]))
                    viewMask |= (1u << v);
            }
            if (viewMask == 0u)
                continue;
            FusedVerdict verdict{};
            verdict.value = glm::uvec4(slot, gen, viewMask, castsShadow ? 1u : 0u);
            out.verdicts[slot] = verdict;
        }
    }
    return out;
}

} // namespace tour::gpu_scene
