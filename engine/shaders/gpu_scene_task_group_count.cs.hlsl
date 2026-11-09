// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU task-group producer, stage 1: meshlet range -> 32-cluster group count.
// Reuse gpu_scene_work_prefix_{block,groups,add} to scan these counts.
// This producer has NO 1024-meshlet-per-submesh limit.
// Invalid/unregistered geometry must be covered by the indexed fallback
// ownership mask before this producer can replace raster submissions.
#include "gpu_platform.hlsli"
#define GS_MAX_WORK 1048576u
#define GS_MAX_MESHES 65535u
#define GS_TASK_SIZE 32u
struct WorkRow { uint4 identity; uint4 detail; };
struct Work { WorkRow rows[GS_MAX_WORK]; };
struct SceneOffsets { uint values[GS_MAX_WORK+1u]; };
struct Mesh { uint first; uint count; };
struct Meshes { Mesh rows[GS_MAX_MESHES]; };
struct Aliases { uint slots[GS_MAX_MESHES]; };
struct Indexed { uint indexCount; uint firstIndex; int vertexOffset; uint flags; };
struct Geometry { Indexed rows[GS_MAX_MESHES]; };
struct Counts { uint values[GS_MAX_WORK]; };
struct Push {
    GPU_PTR_READ(Work) work;
    GPU_PTR_READ(SceneOffsets) sceneOffsets;
    GPU_PTR_READ(Meshes) meshes;
    GPU_PTR_READ(Aliases) aliases;
    GPU_PTR_READ(Geometry) geometry;
    GPU_PTR_WRITE(Counts) counts;
    uint sceneCandidateCount;
    uint workCapacity;
    uint meshCount;
    uint aliasCount;
    uint geometryCount;
    uint clusterCount;
};
GPU_PUSH(Push,pc);
[numthreads(256,1,1)]
void mainCS(uint3 tid:SV_DispatchThreadID) {
    const uint i=tid.x;
    if (i>=pc.workCapacity || i>=GS_MAX_WORK) return;
    pc.counts.Get().values[i]=0u;
    if (pc.sceneCandidateCount>GS_MAX_WORK) return;
    const uint live=pc.sceneOffsets.Get().values[pc.sceneCandidateCount];
    if (live>pc.workCapacity || i>=live) return;
    const WorkRow work=pc.work.Get().rows[i];
    const uint id=work.identity.z;
    if (work.identity.y==0u || work.detail.z>=4u ||
        id>=pc.meshCount || id>=pc.aliasCount ||
        id>=GS_MAX_MESHES) return;
    const uint geo=pc.aliases.Get().slots[id];
    if (geo>=pc.geometryCount || geo>=GS_MAX_MESHES ||
        pc.geometry.Get().rows[geo].indexCount==0u) return;
    const Mesh m=pc.meshes.Get().rows[id];
    if (m.count==0u || m.first>pc.clusterCount ||
        m.count>pc.clusterCount-m.first) return;
    // Overflow-free ceil(count / 32) for any uint32 range.
    pc.counts.Get().values[i]=m.count/GS_TASK_SIZE +
        ((m.count%GS_TASK_SIZE)!=0u ? 1u : 0u);
}
