// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Stage 3: stable compaction, preserving the candidate order of visible rows.
#include "gpu_platform.hlsli"
#define GS_MAX_CANDIDATES 1048576u
#define GS_MAX_GROUPS 16384u
struct Verdict { uint4 value; };
struct VerdictBuffer { Verdict rows[GS_MAX_CANDIDATES]; };
struct OffsetBuffer { uint values[GS_MAX_GROUPS + 1u]; };
struct OutputBuffer { Verdict rows[GS_MAX_CANDIDATES]; };
struct Push {
    GPU_PTR_READ(VerdictBuffer) verdicts;
    GPU_PTR_READ(OffsetBuffer) offsets;
    GPU_PTR_WRITE(OutputBuffer) output;
    uint candidateCount;
    uint groupCount;
};
GPU_PUSH(Push, pcS);
groupshared uint prefix[64];
[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID,
            uint3 lane : SV_GroupThreadID) {
    if (gid.x >= pcS.groupCount || gid.x >= GS_MAX_GROUPS) return;
    const uint index = tid.x;
    uint4 v = uint4(0xffffffffu, 0u, 0u, 0u);
    if (index < pcS.candidateCount && index < GS_MAX_CANDIDATES)
        v = pcS.verdicts.Get().rows[index].value;
    const uint active = (v.x != 0xffffffffu && v.y != 0u && v.z != 0u)
        ? 1u : 0u;
    prefix[lane.x] = active;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint stride = 1u; stride < 64u; stride *= 2u) {
        const uint prior = lane.x >= stride ? prefix[lane.x - stride] : 0u;
        GroupMemoryBarrierWithGroupSync();
        prefix[lane.x] += prior;
        GroupMemoryBarrierWithGroupSync();
    }
    if (active != 0u) {
        const uint slot = pcS.offsets.Get().values[gid.x] + prefix[lane.x] - 1u;
        if (slot < pcS.candidateCount && slot < GS_MAX_CANDIDATES)
            pcS.output.Get().rows[slot].value = v;
    }
}
