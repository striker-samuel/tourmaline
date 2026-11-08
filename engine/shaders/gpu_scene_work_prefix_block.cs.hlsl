// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Production work scan 1/3: 256-thread exclusive prefix per block.
// Only blocks, not individual instances, require a later serial scan.
#include "gpu_platform.hlsli"
#define GS_MAX_CANDIDATES 1048576u
#define GS_WORK_SCAN_GROUP 256u
#define GS_MAX_GROUPS 4096u
struct Counts { uint values[GS_MAX_CANDIDATES]; };
struct Offsets { uint values[GS_MAX_CANDIDATES+1u]; };
struct Groups { uint values[GS_MAX_GROUPS]; };
struct Push {
    GPU_PTR_READ(Counts) counts;
    GPU_PTR_WRITE(Offsets) offsets;
    GPU_PTR_WRITE(Groups) sums;
    uint candidateCount;
    uint groupCount;
};
GPU_PUSH(Push, pc);
groupshared uint scratch[GS_WORK_SCAN_GROUP];

[numthreads(256,1,1)]
void mainCS(uint3 groupId: SV_GroupID, uint3 localId: SV_GroupThreadID) {
    const uint g=groupId.x, t=localId.x;
    if (g >= pc.groupCount || pc.groupCount > GS_MAX_GROUPS)
        return;
    const uint i = g*GS_WORK_SCAN_GROUP+t;
    const uint value = i < pc.candidateCount ? pc.counts.Get().values[i] : 0u;
    scratch[t]=value;
    GroupMemoryBarrierWithGroupSync();
    for (uint delta=1u; delta<GS_WORK_SCAN_GROUP; delta<<=1u) {
        uint addition = t >= delta ? scratch[t-delta] : 0u;
        GroupMemoryBarrierWithGroupSync();
        scratch[t] += addition;
        GroupMemoryBarrierWithGroupSync();
    }
    if (i < pc.candidateCount)
        pc.offsets.Get().values[i] = scratch[t]-value;
    if (t == GS_WORK_SCAN_GROUP-1u)
        pc.sums.Get().values[g] = scratch[t];
}
