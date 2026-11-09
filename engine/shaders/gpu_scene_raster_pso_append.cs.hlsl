// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Experimental GPU-scene PSO append builder. NOT wired into the raster
// runtime until CPU bounds, indirect-count submission, and temporal/Hi-Z
// consumers are updated together. The old fixed-stride builder remains live.
//
// One wave-level reservation per nonempty PSO class; commands preserve the
// original work index as firstInstance. Per-bin offsets/capacities come from
// the CPU's conservative snapshot. Counters must be zeroed before dispatch.
#include "gpu_platform.hlsli"

#define GS_MAX_WORK 1048576u
#define GS_MAX_GEO 65535u
#define GS_MAX_CANDIDATES 1048576u
#define GS_INVALID 0xffffffffu

struct SceneSubmeshWork { uint4 identity; uint4 detail; };
struct IndexedGeometry {
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint flags;
};
struct RasterIndirectCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
struct WorkBuffer { SceneSubmeshWork rows[GS_MAX_WORK]; };
struct OffsetBuffer { uint values[GS_MAX_CANDIDATES + 1u]; };
struct GeometryBuffer { IndexedGeometry rows[GS_MAX_GEO]; };
struct IndirectBuffer { RasterIndirectCmd rows[GS_MAX_WORK]; };
struct AliasBuffer { uint slots[GS_MAX_GEO]; };
struct ClassCounters {
    uint counts[4];
    uint overflow[4];
};

struct PsoAppendPush {
    GPU_PTR_READ(WorkBuffer) work;
    GPU_PTR_READ(OffsetBuffer) workOffsets;
    GPU_PTR_READ(GeometryBuffer) geometry;
    GPU_PTR_WRITE(IndirectBuffer) indirect;
    GPU_PTR(ClassCounters) counters;
    GPU_PTR_READ(AliasBuffer) aliases;
    uint candidateCount;
    uint geometryCount;
    uint workCapacity;
    uint commandCapacity;
    uint aliasCount;
    uint padding0;
    uint padding1;
    uint padding2;
    uint4 binOffsets;
    uint4 binCapacities;
};
GPU_PUSH(PsoAppendPush, pc);

uint FirstLane(uint4 mask) {
    if (mask.x != 0u) return firstbitlow(mask.x);
    if (mask.y != 0u) return 32u + firstbitlow(mask.y);
    if (mask.z != 0u) return 64u + firstbitlow(mask.z);
    if (mask.w != 0u) return 96u + firstbitlow(mask.w);
    return 0u;
}

[numthreads(128, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint i = tid.x;
    const uint count = pc.candidateCount <= GS_MAX_CANDIDATES
        ? pc.workOffsets.Get().values[pc.candidateCount] : 0u;
    const bool inRange = i < pc.workCapacity && i < GS_MAX_WORK &&
                         count <= pc.workCapacity && i < count;
    SceneSubmeshWork w = (SceneSubmeshWork)0;
    IndexedGeometry g = (IndexedGeometry)0;
    if (inRange)
        w = pc.work.Get().rows[i];
    // Work identities carry revision-local registrations; master now
    // resolves those through a per-flight mapping to persistent descriptors.
    const uint transientId = w.identity.z;
    const bool hasAlias = inRange && transientId < pc.aliasCount &&
                          transientId < GS_MAX_GEO;
    uint geometryIndex = GS_INVALID;
    if (hasAlias)
        geometryIndex = pc.aliases.Get().slots[transientId];
    const bool hasGeometry = hasAlias && geometryIndex < pc.geometryCount &&
                             geometryIndex < GS_MAX_GEO;
    if (hasGeometry)
        g = pc.geometry.Get().rows[geometryIndex];

    const uint cls = w.detail.z;
    const bool valid = hasGeometry && w.identity.x != GS_INVALID &&
                       w.identity.y != 0u && cls < 4u && g.indexCount != 0u;

    // Every lane participates in every wave operation, including invalid
    // lanes and the last partial work group (no early return here).
    [unroll]
    for (uint bin = 0u; bin < 4u; ++bin) {
        const bool selected = valid && cls == bin;
        const uint participating = WaveActiveCountBits(selected);
        const uint prefix = WavePrefixCountBits(selected);
        const uint leader = FirstLane(WaveActiveBallot(selected));
        uint base = 0u;
        uint allocated = 0u;

        if (participating != 0u && WaveGetLaneIndex() == leader) {
            const uint limit = pc.binCapacities[bin];
            uint previous = pc.counters.Get().counts[bin];
            // Bounded CAS: do not increment the count beyond the allocated
            // command region, even under concurrent wave reservations.
            [loop]
            while (previous <= limit && participating <= limit - previous) {
                uint observed;
                InterlockedCompareExchange(pc.counters.Get().counts[bin],
                                           previous, previous + participating,
                                           observed);
                if (observed == previous) {
                    base = previous;
                    allocated = 1u;
                    break;
                }
                previous = observed;
            }
            if (allocated == 0u)
                InterlockedOr(pc.counters.Get().overflow[bin], 1u);
        }
        base = WaveReadLaneAt(base, leader);
        allocated = WaveReadLaneAt(allocated, leader);
        if (selected && allocated != 0u) {
            const uint slot = pc.binOffsets[bin] + base + prefix;
            // Reject inconsistent host layouts without corrupting memory.
            if (slot < pc.commandCapacity && slot < GS_MAX_WORK) {
                RasterIndirectCmd cmd;
                cmd.indexCount = g.indexCount;
                cmd.instanceCount = 1u;
                cmd.firstIndex = g.firstIndex;
                cmd.vertexOffset = g.vertexOffset;
                cmd.firstInstance = i;
                pc.indirect.Get().rows[slot] = cmd;
            } else {
                InterlockedOr(pc.counters.Get().overflow[bin], 2u);
            }
        }
    }
}
