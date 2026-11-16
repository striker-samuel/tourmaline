// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// P1 flat GPU multi-view leaf classifier (#372). One compute pass classifies
// every GPUScene leaf against every active RenderView (Main + C0..C3),
// writing a per-leaf view mask. This is the GPU replacement for the CPU
// SelectPassedInstanceRangesFrame leaf scan; it produces the mask only — the
// per-view prefix/scatter and compact range stream are the next stage. The
// mask is consumed on-GPU by the per-view compaction + instance cull, never
// read back to the CPU on a normal frame.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"

#define GPU_SCENE_MAX_ROW_SLOTS 4194304u
#define GPU_SCENE_MAX_VIEWS 32u

struct GpuSceneLeaves {
    GpuSceneLeafRow rows[GPU_SCENE_MAX_ROW_SLOTS];
};
struct GpuSceneCullView {
    DoubleFloat3 origin;
    float4 inwardPlanes[6];
};
struct GpuSceneCullViews {
    GpuSceneCullView views[GPU_SCENE_MAX_VIEWS];
};
struct GpuSceneLeafMasks {
    uint masks[GPU_SCENE_MAX_ROW_SLOTS];
};

struct GpuSceneLeafClassifyPush {
    GPU_PTR_READ(GpuSceneLeaves) leaves;
    GPU_PTR_READ(GpuSceneCullViews) views;
    GPU_PTR_WRITE(GpuSceneLeafMasks) masks;
    uint4 counts; // x leafCount, y viewCount, z/w reserved
};
GPU_PUSH(GpuSceneLeafClassifyPush, pcS);

// Mirrors the CPU PossibleLeafInFrustum: incomplete/nonfinite bounds fail
// OPEN (visible), never prove invisibility.
bool LeafInView(GpuSceneLeaf leaf, GpuSceneCullView view) {
    if ((leaf.range.z & 1u) == 0u) // LeafFlagBoundsComplete
        return true;
    const float3 center = DoubleFloatRelative(leaf.worldCenter, view.origin);
    const float3 extents = leaf.halfExtents.xyz;
    if (!all(isfinite(center)) || !all(isfinite(extents)) || any(extents < 0.0f))
        return true;
    [unroll]
    for (uint p = 0u; p < 6u; ++p) {
        const float4 plane = view.inwardPlanes[p];
        const float reach = dot(abs(plane.xyz), extents);
        const float signedDistance = dot(plane.xyz, center) + plane.w;
        if (!isfinite(signedDistance) || !isfinite(reach))
            return true;
        const float slack = 0.01f + 2e-6f * (abs(signedDistance) + reach + 1.0f);
        if (signedDistance + reach < -slack)
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint leafIndex = tid.x;
    if (leafIndex >= pcS.counts.x || leafIndex >= GPU_SCENE_MAX_ROW_SLOTS)
        return;
    const GpuSceneLeafRow row = pcS.leaves.Get().rows[leafIndex];
    if (row.identity.y != 1u) {
        pcS.masks.Get().masks[leafIndex] = 0u;
        return;
    }
    uint mask = 0u;
    const uint viewCount = min(pcS.counts.y, GPU_SCENE_MAX_VIEWS);
    for (uint v = 0u; v < viewCount; ++v) {
        if (LeafInView(row.value, pcS.views.Get().views[v]))
            mask |= (1u << v);
    }
    pcS.masks.Get().masks[leafIndex] = mask;
}
