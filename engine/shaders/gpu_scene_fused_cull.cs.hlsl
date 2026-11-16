// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Fused multi-view instance cull (#372,
// docs/gpu_scene_visibility_hierarchy_proposal.md). Consumes the leaf work
// queue produced by gpu_scene_leaf_compact and reads each of an active leaf's
// instances ONCE, validating generation + ownership + CastShadow once and
// producing ONE fused verdict per instance carrying a per-view visibility
// mask. This replaces the four independent per-cascade cull passes that load
// the same instances four times — the 5x verdict-write amplification the
// proposal calls out.
//
// Threading (first version, correctness-first): ONE THREAD PER ACTIVE LEAF,
// walking its instance range sequentially. This is load-imbalanced for uneven
// leaves but correct; the workgroup-per-leaf-chunk model with subgroup
// ballots and block-aggregated per-view output is the follow-up optimization.
//
// The output is a fused verdict stream (instance slot + generation + per-view
// mask), NOT five per-view candidate arrays. The downstream work-select +
// draw-build shaders consume it and expand LOD/submesh work per view.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"

#define GPU_SCENE_MAX_ROW_SLOTS 4194304u
#define GPU_SCENE_MAX_VIEWS 32u

struct GpuSceneMembers {
    GpuSceneLeafMemberRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneInstances {
    GpuSceneInstanceRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneOwnerGenerations {
    uint generations[GPU_SCENE_MAX_ROW_SLOTS];
};
// Leaf work entry (mirrors gpu_scene_leaf_compact's GpuLeafWorkEntry; a shared
// hlsli is the follow-up cleanup).
struct GpuLeafWorkEntry {
    uint4 value; // x leafIndex, y viewMask, z firstMember, w memberCount
};
struct GpuLeafWorkQueue {
    GpuLeafWorkEntry entries[GPU_SCENE_MAX_ROW_SLOTS];
};
// Per-view frustum planes, origin-relative (mirrors GpuSceneCullView).
struct GpuSceneFusedView {
    DoubleFloat3 origin;
    float4 inwardPlanes[6];
};
struct GpuSceneFusedViews {
    GpuSceneFusedView views[GPU_SCENE_MAX_VIEWS];
};
// One fused verdict per visible instance: x slot, y generation, z view mask,
// w flags (bit 0 = CastShadow, mirroring the cull shader's convention).
struct GpuFusedVerdict {
    uint4 value;
};
struct GpuFusedVerdicts {
    GpuFusedVerdict values[GPU_SCENE_MAX_ROW_SLOTS];
};

struct GpuSceneFusedCullPush {
    GPU_PTR_READ(GpuLeafWorkQueue) queue;
    GPU_PTR_READ(GpuSceneMembers) members;
    GPU_PTR_READ(GpuSceneInstances) instances;
    GPU_PTR_READ(GpuSceneOwnerGenerations) ownerGenerations;
    GPU_PTR_READ(GpuSceneFusedViews) views;
    GPU_PTR_WRITE(GpuFusedVerdicts) verdicts;
    uint4 counts; // x activeLeafCount, y viewCount, z/w reserved
};
GPU_PUSH(GpuSceneFusedCullPush, pcS);

// Mirrors the CPU ReferenceInstanceVisible sphere test, but per-view.
bool InstanceInView(GpuSceneInstance inst, GpuSceneFusedView view) {
    const float3 translation = DoubleFloatRelative(inst.worldPosition, view.origin);
    const float3 bx = inst.basisX.xyz;
    const float3 by = inst.basisY.xyz;
    const float3 bz = inst.basisZ.xyz;
    const float3 local = inst.localSphere.xyz;
    const float3 center = translation + bx * local.x + by * local.y + bz * local.z;
    const float basisSquared = dot(bx, bx) + dot(by, by) + dot(bz, bz);
    const float radius = sqrt(max(basisSquared, 0.0f)) * inst.localSphere.w;
    if (!all(isfinite(center)) || !isfinite(radius) || radius < 0.0f)
        return true;
    [unroll]
    for (uint p = 0u; p < 6u; ++p) {
        const float4 plane = view.inwardPlanes[p];
        const float d = dot(plane.xyz, center) + plane.w;
        if (!isfinite(d) || !all(isfinite(plane)))
            return true;
        const float slack = 0.02f + 2e-6f * (abs(d) + radius + 1.0f);
        if (d < -(radius + slack))
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint workIndex = tid.x;
    if (workIndex >= pcS.counts.x || workIndex >= GPU_SCENE_MAX_ROW_SLOTS)
        return;
    const GpuLeafWorkEntry entry = pcS.queue.Get().entries[workIndex];
    const uint leafViewMask = entry.value.y;
    if (leafViewMask == 0u)
        return;
    const uint viewCount = min(pcS.counts.y, GPU_SCENE_MAX_VIEWS);

    // Walk the leaf's instance range; each instance is read ONCE and its
    // per-view visibility is folded into a single fused verdict.
    for (uint i = 0u; i < entry.value.w; ++i) {
        const uint memberIndex = entry.value.z + i;
        if (memberIndex >= GPU_SCENE_MAX_ROW_SLOTS)
            return;
        const GpuSceneLeafMemberRow member = pcS.members.Get().rows[memberIndex];
        if (member.identity.y != 1u)
            continue;
        const uint instanceId = member.value.ref.x;
        const uint instanceGen = member.value.ref.y;
        if (instanceId >= GPU_SCENE_MAX_ROW_SLOTS)
            continue;
        const GpuSceneInstanceRow row = pcS.instances.Get().rows[instanceId];
        if (row.identity.y != 1u || row.identity.x != instanceGen)
            continue;
        // Ownership: only this flight's exact generation may emit work.
        if (pcS.ownerGenerations.Get().generations[instanceId] != instanceGen)
            continue;
        const GpuSceneInstance inst = row.value;
        const bool castsShadow = (inst.refs.w & (1u << 5)) != 0u;

        // Fold the leaf's active views through the per-instance test.
        uint viewMask = 0u;
        for (uint v = 0u; v < viewCount; ++v) {
            if ((leafViewMask & (1u << v)) == 0u)
                continue;
            if (InstanceInView(inst, pcS.views.Get().views[v]))
                viewMask |= (1u << v);
        }
        if (viewMask == 0u)
            continue;

        GpuFusedVerdict verdict;
        verdict.value = uint4(instanceId, instanceGen, viewMask,
                              castsShadow ? 1u : 0u);
        pcS.verdicts.Get().values[instanceId] = verdict;
    }
}
