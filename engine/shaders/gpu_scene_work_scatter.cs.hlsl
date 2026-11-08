// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Stable per-instance/per-submesh expansion. No atomics or draw calls.
#include "gpu_platform.hlsli"
#include "gpu_scene.hlsli"
#define GS_MAX_CANDIDATES 1048576u
#define GS_MAX_ROWS 4194304u
#define GS_MAX_WORK 1048576u
#define GS_INVALID 0xffffffffu
struct WorkChoice { uint4 identity; uint4 range; };
struct Choices { WorkChoice rows[GS_MAX_CANDIDATES]; };
struct Offsets { uint values[GS_MAX_CANDIDATES + 1u]; };
struct Submeshes { GpuSceneSubmeshRow rows[GS_MAX_ROWS]; };
struct WorkRow { uint4 identity; uint4 detail; };
// identity: instance slot, instance generation, registered geometry, material.
// detail: selected LOD index, persistent submesh slot, raster class, pass mask.
// raster class 0 = backface culling, 1 = double-sided/no culling.
// This remains passive until the matching PSO-bin dispatch is wired.
struct WorkOutput { WorkRow rows[GS_MAX_WORK]; };
struct Push {
    GPU_PTR_READ(Choices) choices;
    GPU_PTR_READ(Offsets) offsets;
    GPU_PTR_READ(Submeshes) submeshes;
    GPU_PTR_WRITE(WorkOutput) output;
    uint4 limits; // candidateCount, submeshSlots, workCapacity, reserved
};
GPU_PUSH(Push, pcW);
[numthreads(64,1,1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pcW.limits.x || i >= GS_MAX_CANDIDATES) return;
    const WorkChoice choice = pcW.choices.Get().rows[i];
    if (choice.identity.x == GS_INVALID || choice.range.y == 0u) return;
    const uint first = choice.identity.w;
    const uint count = choice.range.x;
    const uint slots = min(pcW.limits.y, GS_MAX_ROWS);
    if (first >= slots || count > slots - first || count > 16u) return;
    uint outputSlot = pcW.offsets.Get().values[i];
    for (uint j=0u; j<count; ++j) {
        const uint subSlot = first + j;
        const GpuSceneSubmeshRow sub = pcW.submeshes.Get().rows[subSlot];
        if (sub.identity.y != 1u || sub.value.geometryId == GS_INVALID) continue;
        if (outputSlot >= pcW.limits.z || outputSlot >= GS_MAX_WORK) return;
        WorkRow work;
        work.identity = uint4(choice.identity.x, choice.identity.y,
                              sub.value.geometryId, sub.value.materialId);
        work.detail = uint4(choice.identity.z, subSlot,
                            choice.range.z, sub.value.passMask);
        pcW.output.Get().rows[outputSlot++] = work;
    }
}
