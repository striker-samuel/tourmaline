// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// P1 production bridge: expand the per-instance fused verdict stream into the
// dense per-candidate InstanceCullVerdict stream the existing compact ->
// work-select -> raster chain consumes. One thread per instance slot; the
// target view bit selects which instances are visible in this view, so the
// same fused verdict stream feeds Main and each cascade independently.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"

#define GPU_SCENE_MAX_ROW_SLOTS 4194304u

// Fused verdict stream (x slot, y generation, z per-view mask, w flags).
struct GpuFusedVerdictArray {
    uint4 values[GPU_SCENE_MAX_ROW_SLOTS];
};
// Dense cull-verdict stream (x slot, y generation, z visible, w flags); the
// rejected sentinel is slot = InvalidIndex (0xFFFFFFFF).
struct GpuCullVerdictArray {
    uint4 values[GPU_SCENE_MAX_ROW_SLOTS];
};

struct GpuSceneFusedExpandPush {
    GPU_PTR_READ(GpuFusedVerdictArray) fused;
    GPU_PTR_WRITE(GpuCullVerdictArray) verdicts;
    uint4 counts; // x instanceCount, y viewBit (1 << viewIndex), z/w reserved
};
GPU_PUSH(GpuSceneFusedExpandPush, pcS);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint slot = tid.x;
    if (slot >= pcS.counts.x || slot >= GPU_SCENE_MAX_ROW_SLOTS)
        return;
    const uint viewBit = pcS.counts.y;
    const uint4 fused = pcS.fused.Get().values[slot];
    const bool visible = (fused.z & viewBit) != 0u;
    const uint4 outVerdict = visible ? uint4(fused.x, fused.y, 1u, 0u)
                                     : uint4(0xFFFFFFFFu, 0u, 0u, 0u);
    pcS.verdicts.Get().values[slot] = outVerdict;
}
