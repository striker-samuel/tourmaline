// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Turn GPU-generated submesh work count into vkCmdDispatchIndirect arguments.
// The work count is the terminal prefix at offsets[candidateCount].
// Output is exactly three uints (VkDispatchIndirectCommand ABI).
#include "gpu_platform.hlsli"
#define GS_MAX_CANDIDATES 32768u
#define GS_MAX_WORK 524288u
struct Offsets { uint values[GS_MAX_CANDIDATES + 1u]; };
struct DispatchArgs { uint3 groups; };
struct Push {
    GPU_PTR_READ(Offsets) offsets;
    GPU_PTR_WRITE(DispatchArgs) args;
    uint candidateCount;
    uint workCapacity;
};
GPU_PUSH(Push, pcI);
[numthreads(1,1,1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    if (tid.x != 0u) return;
    // Keep the work consumer inside its allocated bounds. A malformed
    // prefix must produce a ZERO dispatch instead of an out-of-bounds read.
    uint workCount = 0u;
    if (pcI.candidateCount <= GS_MAX_CANDIDATES)
        workCount = pcI.offsets.Get().values[pcI.candidateCount];
    const uint cap = min(pcI.workCapacity, GS_MAX_WORK);
    const uint groups = workCount <= cap ? (workCount + 63u) / 64u : 0u;
    pcI.args.Get().groups = uint3(groups, 1u, 1u);
}
