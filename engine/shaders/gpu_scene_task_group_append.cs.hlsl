// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Candidate E — wave-aggregated atomic append for the task/mesh group
// producer. Replaces the five-dispatch count -> block-scan -> group-scan ->
// add -> scatter chain (gpu_scene_task_group_count + the three work-prefix
// scans + gpu_scene_task_group_scatter) with ONE counter clear + ONE append.
//
// Each work row reserves its task-group range with a single wave-aggregated
// InterlockedAdd on taskOffsets[workCapacity], so that word ends up equal to
// the total the task_dispatch kernel reads for DrawMeshTasksIndirect. Per-row
// group records are written in place; cross-row order is non-deterministic
// (stable within a row), which is harmless for the depth-tested mesh-shader
// draws. The per-row group count and every group record must match the retired
// count + scatter EXACTLY (the parity gate).
#include "gpu_scene_task_mesh.hlsli"
#define GS_MAX_MESHES 65535u
struct GsSceneOffsets { uint values[GS_MAX_WORK + 1u]; };
struct GsMeshRange { uint first; uint count; };
struct GsMeshes { GsMeshRange rows[GS_MAX_MESHES]; };
struct Push {
    GPU_PTR_READ(GsWork) work;
    GPU_PTR_READ(GsSceneOffsets) sceneOffsets;
    GPU_PTR_READ(GsMeshes) meshes;
    GPU_PTR_READ(GsAliases) aliases;
    GPU_PTR_READ(GsGeometry) geometry;
    GPU_PTR_WRITE(GsTaskGroups) groups;
    GPU_PTR(GsTaskOffsets) taskOffsets; // RW: taskOffsets[workCapacity] = running total
    uint sceneCandidateCount;
    uint workCapacity;
    uint taskGroupCapacity;
    uint meshCount;
    uint aliasCount;
    uint geometryCount;
    uint clusterCount;
    uint reserved;
};
GPU_PUSH(Push, pc);
[numthreads(256, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    uint n = 0u;
    uint meshId = 0u, meshFirst = 0u, clusters = 0u, rasterClass = 0u;
    if (i < pc.workCapacity && i < GS_MAX_WORK &&
        pc.sceneCandidateCount <= GS_MAX_WORK) {
        const uint live = pc.sceneOffsets.Get().values[pc.sceneCandidateCount];
        if (live <= pc.workCapacity && i < live) {
            const GsWorkRow work = pc.work.Get().rows[i];
            meshId = work.identity.z;
            rasterClass = work.detail.z;
            // The count shader's exact predicate (n must match it bit-for-bit).
            if (work.identity.y != 0u && rasterClass < 4u &&
                meshId < pc.meshCount && meshId < pc.aliasCount &&
                meshId < GS_MAX_MESHES) {
                const uint geo = pc.aliases.Get().slots[meshId];
                if (geo < pc.geometryCount && geo < GS_MAX_MESHES &&
                    pc.geometry.Get().rows[geo].indexCount != 0u) {
                    const GsMeshRange m = pc.meshes.Get().rows[meshId];
                    meshFirst = m.first;
                    clusters = m.count;
                    if (m.count != 0u && m.first <= pc.clusterCount &&
                        m.count <= pc.clusterCount - m.first) {
                        n = m.count / GS_TASK_THREADS +
                            ((m.count % GS_TASK_THREADS) != 0u ? 1u : 0u);
                    }
                }
            }
        }
    }
    // Wave-aggregated reservation: one InterlockedAdd per active wave, then
    // distribute the wave's reserved range by the lane prefix sum.
    const uint waveSum = WaveActiveSum(n);
    const uint wavePrefix = WavePrefixSum(n);
    uint waveBase = 0u;
    if (WaveIsFirstLane() && waveSum != 0u)
        InterlockedAdd(pc.taskOffsets.Get().values[pc.workCapacity], waveSum, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);
    if (n == 0u)
        return;
    const uint base = waveBase + wavePrefix;
    // Defensive only: the CPU conservative bound makes a real overflow
    // impossible, and the host never submits truncated draws (no-dropped-draws).
    if (base > pc.taskGroupCapacity || n > pc.taskGroupCapacity - base ||
        base + n > GS_MAX_TASK_GROUPS)
        return;
    // The scatter's per-group expansion, written to this row's reserved range.
    for (uint g = 0u; g < n; ++g) {
        const uint firstOffset = g * GS_TASK_THREADS;
        if (firstOffset >= clusters)
            break;
        GsTaskGroup group;
        group.workSlot = i;
        group.firstCluster = meshFirst + firstOffset;
        group.clusterCount = min(GS_TASK_THREADS, clusters - firstOffset);
        group.rasterClass = rasterClass;
        pc.groups.Get().rows[base + g] = group;
    }
}
