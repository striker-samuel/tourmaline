// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Meshlet expansion stage 1: one count per GPU submesh work record.
// Run the existing gpu_scene_work_prefix_{block,groups,add} pipeline over
// these counts to obtain workOffsets[workCount] = exact candidate count.
// Per-record counts <= 1024 prevent 32-bit prefix overflow at 1M work slots.
// Oversized/unknown mesh registrations generate ONE whole-submesh fallback.
// This shader is staged; it is not yet dispatched by WorldRenderer.
#include "gpu_platform.hlsli"
#define MAX_WORK 1048576u
#define MAX_GEO 65535u
#define MAX_CANDIDATES 1048576u
#define MAX_CLUSTERS_PER_WORK 1024u
#define INVALID 0xffffffffu
struct WorkRow { uint4 identity; uint4 detail; };
struct Work { WorkRow rows[MAX_WORK]; };
struct WorkOffsets { uint values[MAX_CANDIDATES+1u]; };
struct Mesh { uint clusterStart; uint clusterCount; };
struct Meshes { Mesh rows[MAX_GEO]; };
struct Indexed { uint indexCount; uint firstIndex; int vertexOffset; uint flags; };
struct Geometry { Indexed rows[MAX_GEO]; };
struct Aliases { uint slots[MAX_GEO]; };
struct Counts { uint values[MAX_WORK]; };
struct Push {
    GPU_PTR_READ(Work) work;
    GPU_PTR_READ(WorkOffsets) workOffsets;
    GPU_PTR_READ(Meshes) meshes;
    GPU_PTR_READ(Geometry) geometry;
    GPU_PTR_READ(Aliases) aliases;
    GPU_PTR_WRITE(Counts) counts;
    uint selectedCount;
    uint workCapacity;
    uint meshCount;
    uint geometryCount;
    uint aliasCount;
    uint unused;
};
GPU_PUSH(Push, pc);
[numthreads(256,1,1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.workCapacity || i >= MAX_WORK) return;
    pc.counts.Get().values[i] = 0u;
    if (pc.selectedCount > MAX_CANDIDATES) return;
    const uint live = pc.workOffsets.Get().values[pc.selectedCount];
    if (live > pc.workCapacity || i >= live) return;
    const WorkRow w = pc.work.Get().rows[i];
    const uint id = w.identity.z;
    if (w.identity.x == INVALID || w.identity.y == 0u ||
        w.detail.z >= 4u || id >= pc.meshCount ||
        id >= pc.aliasCount || id >= MAX_GEO) return;
    const uint geometryId = pc.aliases.Get().slots[id];
    if (geometryId >= pc.geometryCount || geometryId >= MAX_GEO ||
        pc.geometry.Get().rows[geometryId].indexCount == 0u) return;
    const Mesh m = pc.meshes.Get().rows[id];
    const bool bounded = m.clusterCount != 0u &&
                         m.clusterCount <= MAX_CLUSTERS_PER_WORK &&
                         m.clusterStart <= 4194304u &&
                         m.clusterCount <= 4194304u - m.clusterStart;
    pc.counts.Get().values[i] = bounded ? m.clusterCount : 1u;
}
