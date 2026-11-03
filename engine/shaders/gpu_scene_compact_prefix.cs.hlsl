// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Stage 2: exclusive group offsets plus total count in offsets[groupCount].
// Validation-sized sets use <=512 groups. Larger sets retain correctness;
// a hierarchical scan can replace this sequential pass for production scale.
#include "gpu_platform.hlsli"
#define GS_MAX_GROUPS 16384u
struct CountBuffer { uint values[GS_MAX_GROUPS]; };
struct OffsetBuffer { uint values[GS_MAX_GROUPS + 1u]; };
struct Push {
    GPU_PTR_READ(CountBuffer) counts;
    GPU_PTR_WRITE(OffsetBuffer) offsets;
    uint groupCount;
    uint unused;
};
GPU_PUSH(Push, pcS);
[numthreads(1, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    if (tid.x != 0u || pcS.groupCount > GS_MAX_GROUPS) return;
    uint sum = 0u;
    for (uint i = 0u; i < pcS.groupCount; ++i) {
        pcS.offsets.Get().values[i] = sum;
        sum += pcS.counts.Get().values[i];
    }
    pcS.offsets.Get().values[pcS.groupCount] = sum;
}
