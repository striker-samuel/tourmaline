// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Production work scan 2/3: exclusive scan of <=2048 block totals for
// the 524288 candidate capacity. Serial over BLOCKS, never instances.
// Also publishes the terminal total (workOffsets[candidateCount]) so the
// fused prefix-add can be dropped from the work-expansion chain.
#include "gpu_platform.hlsli"
#define GS_MAX_GROUPS 4096u
#define GS_MAX_CANDIDATES 1048576u
struct Groups { uint values[GS_MAX_GROUPS]; };
struct Offsets { uint values[GS_MAX_CANDIDATES + 1u]; };
struct Push {
    GPU_PTR_READ(Groups) sums;
    GPU_PTR_WRITE(Groups) bases;
    GPU_PTR_WRITE(Offsets) offsets;
    uint groupCount;
    uint candidateCount;
};
GPU_PUSH(Push, pc);
[numthreads(1,1,1)]
void mainCS(uint3 tid: SV_DispatchThreadID) {
    if (tid.x != 0u || pc.groupCount > GS_MAX_GROUPS) return;
    uint total=0u;
    for (uint i=0u; i<pc.groupCount; ++i) {
        pc.bases.Get().values[i]=total;
        total+=pc.sums.Get().values[i];
    }
    // Only the fused work-expansion path publishes the terminal here (the
    // task/mesh + meshlet + non-fused paths set candidateCount=0xFFFFFFFF).
    if (pc.candidateCount <= GS_MAX_CANDIDATES)
        pc.offsets.Get().values[pc.candidateCount] = total;
}
