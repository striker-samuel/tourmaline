// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Exclusive per-instance submesh-work prefix for validation-sized batches.
// Serial by design; production must replace with a hierarchical GPU scan.
#include "gpu_platform.hlsli"
#define GS_MAX_CANDIDATES 1048576u
struct Counts { uint values[GS_MAX_CANDIDATES]; };
struct Offsets { uint values[GS_MAX_CANDIDATES + 1u]; };
struct Push {
    GPU_PTR_READ(Counts) counts;
    GPU_PTR_WRITE(Offsets) offsets;
    uint candidateCount;
    uint reserved;
};
GPU_PUSH(Push, pcW);
[numthreads(1,1,1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    if (tid.x != 0u || pcW.candidateCount > GS_MAX_CANDIDATES) return;
    uint total = 0u;
    for (uint i=0u; i<pcW.candidateCount; ++i) {
        pcW.offsets.Get().values[i] = total;
        total += pcW.counts.Get().values[i];
    }
    pcW.offsets.Get().values[pcW.candidateCount] = total;
}
