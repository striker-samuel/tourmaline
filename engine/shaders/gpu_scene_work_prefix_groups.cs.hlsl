// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Production work scan 2/3: exclusive scan of <=2048 block totals for
// the 524288 candidate capacity. Serial over BLOCKS, never instances.
#include "gpu_platform.hlsli"
#define GS_MAX_GROUPS 4096u
struct Groups { uint values[GS_MAX_GROUPS]; };
struct Push {
    GPU_PTR_READ(Groups) sums;
    GPU_PTR_WRITE(Groups) bases;
    uint groupCount;
    uint reserved;
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
}
