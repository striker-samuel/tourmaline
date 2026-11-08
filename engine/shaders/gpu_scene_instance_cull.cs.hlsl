// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPUIR GPU instance frustum test — opt-in, not dispatched by WorldRenderer.
// CPU produces a small, sorted sequence of PassedInstanceRange records
// after COARSE leaf selection. One thread owns one candidate output slot;
// it binary-searches the range prefix, validates generational references,
// and writes a DETERMINISTIC verdict. No unsafe atomic compaction here.
//
// All view planes are render-origin-relative. Absolute scene translations
// are stored as DoubleFloat3 and rebased BEFORE any float32 cull math.
// This compute stage feeds future visibility/LOD expansion, not GfxDraw.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"

#define GPU_SCENE_MAX_PASSED_RANGES   65536u
#define GPU_SCENE_MAX_CANDIDATES      1048576u
#define GPU_SCENE_MAX_ROW_SLOTS       4194304u

struct GpuScenePassedRange {
    uint4 members; // x member row first, y count, z candidate prefix
    uint4 leaf;    // x leaf slot, y leaf generation
};
struct GpuSceneCullVerdict {
    uint4 value;   // x instance slot, y generation, z visible, w flags
};
struct GpuScenePassedRanges {
    GpuScenePassedRange ranges[GPU_SCENE_MAX_PASSED_RANGES];
};
struct GpuSceneCullVerdicts {
    GpuSceneCullVerdict values[GPU_SCENE_MAX_CANDIDATES];
};
struct GpuSceneOwnerGenerations {
    uint generations[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneLeaves {
    GpuSceneLeafRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneMembers {
    GpuSceneLeafMemberRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneInstances {
    GpuSceneInstanceRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneMeshes {
    GpuSceneMeshRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
// Explicit per-view data buffer instead of 128+ bytes of push constants.
struct GpuSceneCullView {
    DoubleFloat3 origin;
    float4 inwardPlanes[6]; // normal.xyz, d; relative to origin
};
struct GpuSceneInstanceCullPush {
    GPU_PTR_READ(GpuScenePassedRanges) ranges;
    GPU_PTR_READ(GpuSceneLeaves) leaves;
    GPU_PTR_READ(GpuSceneMembers) members;
    GPU_PTR_READ(GpuSceneInstances) instances;
    GPU_PTR_READ(GpuSceneMeshes) meshes;
    GPU_PTR_READ(GpuSceneCullView) view;
    GPU_PTR_WRITE(GpuSceneCullVerdicts) verdicts;
    uint4 counts; // x rangeCount, y candidateCount, z memberSlots, w instanceSlots
    uint4 counts2; // x leafSlots, y meshSlots, z viewValid (0=passthrough), w spare
    GPU_PTR_READ(GpuSceneOwnerGenerations) ownerGenerations; // null in CPU oracle mode
};
GPU_PUSH(GpuSceneInstanceCullPush, pcS);

bool GpuSceneSphereVisible(GpuSceneInstance instance, GpuSceneCullView view) {
    const float3 translation = DoubleFloatRelative(
        instance.worldPosition, view.origin);
    const float3 bx = instance.basisX.xyz;
    const float3 by = instance.basisY.xyz;
    const float3 bz = instance.basisZ.xyz;
    const float3 local = instance.localSphere.xyz;
    const float3 center = translation + bx * local.x
        + by * local.y + bz * local.z;
    const float basisSquared = dot(bx, bx) + dot(by, by) + dot(bz, bz);
    const float radius = sqrt(max(basisSquared, 0.0f))
                       * instance.localSphere.w;
    // Non-finite or unknown bounds must never prove invisibility.
    if (!all(isfinite(center)) || !isfinite(radius) || radius < 0.0f)
        return true;
    [unroll]
    for (uint plane = 0; plane < 6u; ++plane) {
        float4 p = view.inwardPlanes[plane];
        const float d = dot(p.xyz, center) + p.w;
        if (!isfinite(d) || !all(isfinite(p)))
            return true;
        // Conservative numeric slack. Deliberately larger for faraway
        // candidates: large relative float32 positions lose precision.
        const float slack = 0.02f + 2e-6f * (abs(d) + radius + 1.0f);
        if (d < -(radius + slack))
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint candidate = tid.x;
    if (candidate >= pcS.counts.y || candidate >= GPU_SCENE_MAX_CANDIDATES)
        return;

    // Always initialize; no previous-frame verdict can leak into this slot.
    GpuSceneCullVerdict result;
    result.value = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
    pcS.verdicts.Get().values[candidate] = result;

    const uint rangeCount = min(pcS.counts.x, GPU_SCENE_MAX_PASSED_RANGES);
    if (rangeCount == 0u)
        return;

    // Upper_bound(range.members.z, candidate). Candidate prefix offsets
    // are monotonically increasing, generated and capped by the CPU.
    uint lo = 0u, hi = rangeCount;
    while (lo < hi) {
        const uint mid = lo + (hi - lo) / 2u;
        const uint prefix = pcS.ranges.Get().ranges[mid].members.z;
        if (prefix <= candidate) lo = mid + 1u;
        else hi = mid;
    }
    if (lo == 0u)
        return;
    const GpuScenePassedRange range =
        pcS.ranges.Get().ranges[lo - 1u];
    const uint local = candidate - range.members.z;
    if (local >= range.members.y)
        return;

    // Validate the entire leaf-to-member relationship against this flight
    // snapshot, including its generation and current contiguous span.
    if (range.leaf.x >= min(pcS.counts2.x, GPU_SCENE_MAX_ROW_SLOTS))
        return;
    const GpuSceneLeafRow leaf = pcS.leaves.Get().rows[range.leaf.x];
    if (leaf.identity.y != 1u || leaf.identity.x != range.leaf.y ||
        leaf.value.range.x != range.members.x ||
        leaf.value.range.y != range.members.y)
        return;
    if (range.members.x >= min(pcS.counts.z, GPU_SCENE_MAX_ROW_SLOTS) ||
        local >= min(pcS.counts.z, GPU_SCENE_MAX_ROW_SLOTS) - range.members.x)
        return;

    const uint memberIndex = range.members.x + local;
    const GpuSceneLeafMemberRow member =
        pcS.members.Get().rows[memberIndex];
    if (member.identity.y != 1u)
        return;
    const uint instanceId = member.value.ref.x;
    const uint instanceGen = member.value.ref.y;
    if (instanceId >= min(pcS.counts.w, GPU_SCENE_MAX_ROW_SLOTS))
        return;
    const GpuSceneInstanceRow instance =
        pcS.instances.Get().rows[instanceId];
    if (instance.identity.y != 1u ||
        instance.identity.x != instanceGen)
        return;

    // Production GPU Main only: a slot is admitted if and only if this
    // flight's mask records its exact generation. A retired/recycled slot
    // or a legacy-owned (including skinned) source emits no GPU work.
    // Validation mode has a null owner pointer and retains its full oracle.
    if (pcS.ownerGenerations != 0 &&
        pcS.ownerGenerations.Get().generations[instanceId] != instanceGen)
        return;

    // Unknown/incomplete mesh bounds: fail OPEN, not invisible.
    bool bypass = true;
    if (instance.value.refs.x < min(pcS.counts2.y, GPU_SCENE_MAX_ROW_SLOTS)) {
        const GpuSceneMeshRow mesh =
            pcS.meshes.Get().rows[instance.value.refs.x];
        bypass = mesh.identity.y != 1u ||
                 mesh.identity.x != instance.value.refs.y ||
                 (mesh.value.flags & 1u) == 0u; // MeshFlagCompleteBounds
    }
    const bool visible = bypass || pcS.counts2.z == 0u ||
        GpuSceneSphereVisible(instance.value, pcS.view.Get());
    result.value = uint4(instanceId, instanceGen, visible ? 1u : 0u,
                         bypass ? 1u : 0u);
    pcS.verdicts.Get().values[candidate] = result;
}
