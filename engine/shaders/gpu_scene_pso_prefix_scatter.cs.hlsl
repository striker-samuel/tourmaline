// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Atomics-free PSO compaction, pass 3/3. Unique destination:
// binOffset[class] + exclusiveBlockBase + priorWaveCount + waveLocalRank.
// Work order is stable inside each PSO. No global counter atomics.
// IMPORTANT: do not enable until host indirect-count, lifetime/barriers,
// overflow fallback and temporal/Hi-Z consumers are migrated together.
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
struct BlockTotals { uint values[PSO_MAX_BLOCKS * PSO_CLASSES]; };
struct ClassCounts { uint counts[4]; uint overflow[4]; };
struct RasterIndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct IndirectRows { RasterIndirectCmd rows[PSO_MAX_WORK]; };
struct Push {
    GPU_PTR_READ(WorkRows) work;
    GPU_PTR_READ(WorkOffsets) workOffsets;
    GPU_PTR_READ(GeometryRows) geometry;
    GPU_PTR_READ(Aliases) aliases;
    GPU_PTR_READ(BlockTotals) blockBases;
    GPU_PTR(ClassCounts) classCounts;
    GPU_PTR_WRITE(IndirectRows) indirect;
    uint candidateCount;
    uint workCapacity;
    uint geometryCount;
    uint aliasCount;
    uint blockCount;
    uint commandCapacity;
    uint reserved0;
    uint reserved1;
    uint4 binOffsets;
    uint4 binCapacities;
};
GPU_PUSH(Push, pc);

groupshared uint waveCounts[PSO_CLASSES * PSO_GROUP];

[numthreads(256,1,1)]
void mainCS(uint3 gid : SV_GroupID, uint3 lane : SV_GroupThreadID) {
    if (gid.x >= pc.blockCount || pc.blockCount > PSO_MAX_BLOCKS)
        return; // group uniform
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
    IndexedGeometry g = (IndexedGeometry)0;
    if (hasGeometry)
        g = pc.geometry.Get().rows[geometryId];
    const uint cls = work.detail.z;
    const bool valid = hasGeometry && g.indexCount != 0u &&
                       work.identity.x != PSO_INVALID && work.identity.y != 0u &&
                       cls < PSO_CLASSES;

    const uint waveSize = WaveGetLaneCount();
    const uint wave = lane.x / waveSize;
    uint localRank = 0u;
    [unroll]
    for (uint c = 0u; c < PSO_CLASSES; ++c) {
        const bool selected = valid && cls == c;
        const uint countInWave = WaveActiveCountBits(selected);
        const uint rankInWave = WavePrefixCountBits(selected);
        if (WaveIsFirstLane())
            waveCounts[c * PSO_GROUP + wave] = countInWave;
        if (selected)
            localRank = rankInWave;
    }
    GroupMemoryBarrierWithGroupSync();
    if (!valid)
        return; // barrier is behind us; no other workgroup barriers follow

    // Earlier-wave totals are shared only inside this workgroup.
    for (uint w = 0u; w < wave; ++w)
        localRank += waveCounts[cls * PSO_GROUP + w];
    const uint blockBase = pc.blockBases.Get().values[gid.x * PSO_CLASSES + cls];
    const uint offset = pc.binOffsets[cls];
    const uint capacity = pc.binCapacities[cls];

    // Guards are diagnostic only. The host MUST handle the overflow flag
    // without dropping any scene draw before enabling this path.
    const bool validLayout = offset <= pc.commandCapacity &&
                             capacity <= pc.commandCapacity - offset;
    if (!validLayout || blockBase >= capacity ||
        localRank >= capacity - blockBase ||
        offset + blockBase + localRank >= PSO_MAX_WORK) {
        InterlockedOr(pc.classCounts.Get().overflow[cls], 2u);
        return;
    }
    RasterIndirectCmd cmd;
    cmd.indexCount = g.indexCount;
    cmd.instanceCount = 1u;
    cmd.firstIndex = g.firstIndex;
    cmd.vertexOffset = g.vertexOffset;
    cmd.firstInstance = i; // preserve original persistent instance validation
    pc.indirect.Get().rows[offset + blockBase + localRank] = cmd;
}
