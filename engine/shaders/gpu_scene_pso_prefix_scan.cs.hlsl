// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Atomics-free PSO compaction, pass 2/3. Four independent block scans in
// four lanes; publishes per-class exact GPU draw counts for indirect-count.
// No inter-class ordering dependence, no global atomic append.
#include "gpu_platform.hlsli"

#define PSO_MAX_BLOCKS 4096u
#define PSO_CLASSES 4u

struct BlockTotals { uint values[PSO_MAX_BLOCKS * PSO_CLASSES]; };
struct ClassCounts { uint counts[4]; uint overflow[4]; };
struct Push {
    GPU_PTR_READ(BlockTotals) blockCounts;
    GPU_PTR_WRITE(BlockTotals) blockBases;
    GPU_PTR_WRITE(ClassCounts) classCounts;
    uint blockCount;
    uint commandCapacity;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
    uint4 binOffsets;
    uint4 binCapacities;
};
GPU_PUSH(Push, pc);

[numthreads(4,1,1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint cls = tid.x;
    if (cls >= PSO_CLASSES)
        return;
    uint total = 0u;
    if (pc.blockCount <= PSO_MAX_BLOCKS) {
        for (uint block = 0u; block < pc.blockCount; ++block) {
            const uint index = block * PSO_CLASSES + cls;
            pc.blockBases.Get().values[index] = total;
            total += pc.blockCounts.Get().values[index];
        }
    }
    const uint offset = pc.binOffsets[cls];
    const uint capacity = pc.binCapacities[cls];
    const bool validLayout = pc.blockCount <= PSO_MAX_BLOCKS &&
                             offset <= pc.commandCapacity &&
                             capacity <= pc.commandCapacity - offset;
    // Only valid layouts / sufficient capacity may be submitted. The host
    // MUST abort compact ownership on overflow, not render truncated draws.
    pc.classCounts.Get().counts[cls] = validLayout ? min(total, capacity) : 0u;
    pc.classCounts.Get().overflow[cls] =
        (!validLayout || total > capacity) ? 1u : 0u;
}
