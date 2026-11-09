// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Atomics-free PSO compaction, pass 1/3. Count VALID indexed draws per
// raster class (0..3) for each 256-work block. A wave leader writes a
// private shared-memory count; a workgroup barrier publishes block totals.
// This shader is not used by the active renderer yet.
#include "gpu_platform.hlsli"

#define PSO_MAX_WORK 1048576u
#define PSO_MAX_GEO 65535u
#define PSO_MAX_CANDIDATES 1048576u
#define PSO_MAX_BLOCKS 4096u
#define PSO_CLASSES 4u
#define PSO_GROUP 256u
#define PSO_INVALID 0xffffffffu

struct Work { uint4 identity; uint4 detail; };
struct WorkRows { Work rows[PSO_MAX_WORK]; };
struct WorkOffsets { uint values[PSO_MAX_CANDIDATES + 1u]; };
struct IndexedGeometry { uint indexCount; uint firstIndex; int vertexOffset; uint flags; };
struct GeometryRows { IndexedGeometry rows[PSO_MAX_GEO]; };
struct Aliases { uint slots[PSO_MAX_GEO]; };
struct BlockCounts { uint values[PSO_MAX_BLOCKS * PSO_CLASSES]; };
struct Push {
    GPU_PTR_READ(WorkRows) work;
    GPU_PTR_READ(WorkOffsets) workOffsets;
    GPU_PTR_READ(GeometryRows) geometry;
    GPU_PTR_READ(Aliases) aliases;
    GPU_PTR_WRITE(BlockCounts) blockCounts;
    uint candidateCount;
    uint workCapacity;
    uint geometryCount;
    uint aliasCount;
    uint blockCount;
    uint reserved0;
};
GPU_PUSH(Push, pc);

groupshared uint waveCounts[PSO_CLASSES * PSO_GROUP];

[numthreads(256,1,1)]
void mainCS(uint3 gid : SV_GroupID, uint3 lane : SV_GroupThreadID) {
    if (gid.x >= pc.blockCount || pc.blockCount > PSO_MAX_BLOCKS)
        return; // group-uniform
    const uint i = gid.x * PSO_GROUP + lane.x;
    const uint total = pc.candidateCount <= PSO_MAX_CANDIDATES
        ? pc.workOffsets.Get().values[pc.candidateCount] : 0u;
    const bool inRange = total <= pc.workCapacity && i < total &&
                         i < pc.workCapacity && i < PSO_MAX_WORK;
    Work work = (Work)0;
    if (inRange)
        work = pc.work.Get().rows[i];

    const uint transientId = work.identity.z;
    const bool hasAlias = inRange && transientId < pc.aliasCount &&
                          transientId < PSO_MAX_GEO;
    uint geometryId = PSO_INVALID;
    if (hasAlias)
        geometryId = pc.aliases.Get().slots[transientId];
    const bool hasGeometry = hasAlias && geometryId < pc.geometryCount &&
                             geometryId < PSO_MAX_GEO;
    uint indexCount = 0u;
    if (hasGeometry)
        indexCount = pc.geometry.Get().rows[geometryId].indexCount;
    const uint cls = work.detail.z;
    const bool valid = hasGeometry && indexCount != 0u &&
                       work.identity.x != PSO_INVALID && work.identity.y != 0u &&
                       cls < PSO_CLASSES;

    const uint waveSize = WaveGetLaneCount();
    const uint wave = lane.x / waveSize;
    [unroll]
    for (uint c = 0u; c < PSO_CLASSES; ++c) {
        const uint countInWave = WaveActiveCountBits(valid && cls == c);
        if (WaveIsFirstLane())
            waveCounts[c * PSO_GROUP + wave] = countInWave;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane.x < PSO_CLASSES) {
        const uint waves = (PSO_GROUP + waveSize - 1u) / waveSize;
        uint sum = 0u;
        for (uint w = 0u; w < waves; ++w)
            sum += waveCounts[lane.x * PSO_GROUP + w];
        pc.blockCounts.Get().values[gid.x * PSO_CLASSES + lane.x] = sum;
    }
}
