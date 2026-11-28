// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Candidate E helper — zeroes the per-class append counters and overflow
// flags before the wave-aggregated atomic-append pass. The append's
// InterlockedAdd accumulates on the cleared `counts` words, so the final
// value IS the per-class draw count vkCmdDrawIndexedIndirectCount reads.
// MUST be separated from the append dispatch by an explicit compute
// dependency, and from any prior frame's device-read of the count words by
// the render-graph's write->read edge.
#include "gpu_platform.hlsli"
struct ClassCounts { uint counts[4]; uint overflow[4]; };
struct Push {
    GPU_PTR_WRITE(ClassCounts) classCounts;
};
GPU_PUSH(Push, pc);
[numthreads(8, 1, 1)]
void mainCS(uint3 lane : SV_GroupThreadID) {
    if (lane.x < 4u)
        pc.classCounts.Get().counts[lane.x] = 0u;
    else
        pc.classCounts.Get().overflow[lane.x - 4u] = 0u;
}
