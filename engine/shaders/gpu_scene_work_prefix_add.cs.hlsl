// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Production work scan 3/3: add each block's exclusive base to its
// 256 local offsets and publish offsets[candidateCount]=GPU work count.
#include "gpu_platform.hlsli"
#define GS_MAX_CANDIDATES 1048576u
#define GS_MAX_GROUPS 4096u
struct Offsets { uint values[GS_MAX_CANDIDATES+1u]; };
struct Groups { uint values[GS_MAX_GROUPS]; };
struct Push {
    GPU_PTR_WRITE(Offsets) offsets;
    GPU_PTR_READ(Groups) sums;
    GPU_PTR_READ(Groups) bases;
    uint candidateCount;
    uint groupCount;
};
GPU_PUSH(Push, pc);
[numthreads(256,1,1)]
void mainCS(uint3 tid: SV_DispatchThreadID) {
    const uint i=tid.x;
    if (pc.groupCount == 0u || pc.groupCount > GS_MAX_GROUPS ||
        pc.candidateCount > GS_MAX_CANDIDATES)
        return;
    if (i < pc.candidateCount) {
        const uint group=i/256u;
        pc.offsets.Get().values[i] += pc.bases.Get().values[group];
    }
    if (i == 0u) {
        const uint last=pc.groupCount-1u;
        pc.offsets.Get().values[pc.candidateCount] =
            pc.bases.Get().values[last]+pc.sums.Get().values[last];
    }
}
