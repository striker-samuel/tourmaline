// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU task-group producer, stage 3: expand scanned 32-meshlet group counts
// into actual task-group records. The existing three-stage work-prefix
// kernel supplies offsets[workCapacity] = exact group count.
// Host MUST prove taskGroupCapacity >= entire GPU total before ownership.
#include "gpu_scene_task_mesh.hlsli"
struct GsOffsets { uint values[GS_MAX_WORK+1u]; };
struct GsSceneOffsets { uint values[GS_MAX_WORK+1u]; };
struct GsMeshRange { uint first; uint count; };
struct GsMeshes { GsMeshRange rows[GS_MAX_GEO]; };
struct Push {
    GPU_PTR_READ(GsWork) work;
    GPU_PTR_READ(GsOffsets) taskOffsets;
    GPU_PTR_READ(GsSceneOffsets) sceneOffsets;
    GPU_PTR_READ(GsMeshes) meshes;
    GPU_PTR_WRITE(GsTaskGroups) groups;
    uint sceneCandidateCount;
    uint workCapacity;
    uint taskGroupCapacity;
    uint meshCount;
};
GPU_PUSH(Push,pc);
[numthreads(128,1,1)]
void mainCS(uint3 tid:SV_DispatchThreadID) {
    const uint i=tid.x;
    if (pc.sceneCandidateCount>GS_MAX_WORK ||
        pc.workCapacity>GS_MAX_WORK) return;
    const uint live=pc.sceneOffsets.Get().values[pc.sceneCandidateCount];
    if (live>pc.workCapacity) return;
    const uint total=pc.taskOffsets.Get().values[pc.workCapacity];
    if (total>pc.taskGroupCapacity || total>GS_MAX_TASK_GROUPS ||
        i>=total) return;
    // upper_bound on prefix offsets: skip all zero-count work rows.
    uint lo=0u,hi=pc.workCapacity;
    while (lo<hi) {
        const uint mid=lo+(hi-lo+1u)/2u;
        if (pc.taskOffsets.Get().values[mid]<=i) lo=mid;
        else hi=mid-1u;
    }
    if (lo>=live) return;
    const GsWorkRow work=pc.work.Get().rows[lo];
    const uint meshId=work.identity.z;
    if (work.identity.y==0u || work.detail.z>=4u ||
        meshId>=pc.meshCount || meshId>=GS_MAX_GEO) return;
    const GsMeshRange mesh=pc.meshes.Get().rows[meshId];
    const uint groupWithin=i-pc.taskOffsets.Get().values[lo];
    const uint firstOffset=groupWithin*GS_TASK_THREADS;
    if (firstOffset>=mesh.count) return;
    GsTaskGroup group;
    group.workSlot=lo;
    group.firstCluster=mesh.first+firstOffset;
    group.clusterCount=min(GS_TASK_THREADS,mesh.count-firstOffset);
    group.rasterClass=work.detail.z;
    pc.groups.Get().rows[i]=group;
}
