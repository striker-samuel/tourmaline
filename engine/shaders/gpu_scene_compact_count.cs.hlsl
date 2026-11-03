// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Stage 1: count visible, generation-valid verdicts per 64-candidate group.
#include "gpu_platform.hlsli"
#define GS_MAX_CANDIDATES 1048576u
#define GS_MAX_GROUPS 16384u
struct Verdict { uint4 value; };
struct VerdictBuffer { Verdict rows[GS_MAX_CANDIDATES]; };
struct CountBuffer { uint values[GS_MAX_GROUPS]; };
struct Push {
    GPU_PTR_READ(VerdictBuffer) verdicts;
    GPU_PTR_WRITE(CountBuffer) counts;
    uint candidateCount;
    uint groupCount;
};
GPU_PUSH(Push, pcS);
[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    if (groupId.x >= pcS.groupCount || groupId.x >= GS_MAX_GROUPS ||
        groupThreadId.x != 0u) return;
    uint visible = 0u;
    const uint start = groupId.x * 64u;
    [unroll]
    for (uint i = 0u; i < 64u; ++i) {
        const uint index = start + i;
        if (index >= pcS.candidateCount || index >= GS_MAX_CANDIDATES) break;
        const uint4 v = pcS.verdicts.Get().rows[index].value;
        visible += (v.x != 0xffffffffu && v.y != 0u && v.z != 0u) ? 1u : 0u;
    }
    pcS.counts.Get().values[groupId.x] = visible;
}
