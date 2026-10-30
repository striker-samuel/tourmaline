// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_scatter_visible.cs.hlsl — PR03 chain 3/4: fill the
// visible-index list. One thread per live instance: when my mask bit
// is set, atomically claim the next slot inside my proto's bin range
// (offsets[proto] + appendNext[proto]) and write my SOURCE slot index.
// Bounded by construction: exactly counts[proto] mask-set instances of
// proto exist, so appendNext never exceeds the bin's count — no
// capacity guard is needed here (the cull pass counted them).
// Within-bin order is arbitrary per frame (atomics) — documented: the
// buffer position is NOT an identity; overrides key by StableID. A
// stable candidate-index scatter is the refinement if deterministic
// within-bin order is ever required.

#include "placement_common.hlsli"
#include "placement_cull.hlsli"
#include "gpu_platform.hlsli"

// The scatter-visible push (48 bytes). Must match C++
// PlacementScatterVisPush. The kernels SELF-BOUND from the count
// buffer (min(raw, capacity)) — the dispatch width is the CPU-known
// capacity upper bound; no count readback on the frame path.
struct PlacementScatterVisPush
{
    GPU_PTR_READ(PlacementInstanceBuffer) instances; // 0
    GPU_PTR_READ(PlacementCountBuffer) count;        // 8
    GPU_PTR_READ(PlacementMaskBuffer) mask;          // 16
    GPU_PTR(PlacementBinsBuffer) bins;        // 24
    GPU_PTR_WRITE(PlacementVisibleBuffer) visible;   // 32
    uint protoCount;  // 40 (chain parity; unused here)
    uint pad0;        // 44
};

GPU_PUSH(PlacementScatterVisPush, pc);

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const uint liveCount = min(pc.count.Get().count, pc.count.Get().capacity);
    const uint slot = groupId.x * 64 + threadId.x;
    if (slot >= liveCount)
        return;

    const uint w = pc.mask.Get().w[slot / PLACEMENT_MASK_BITS];
    if ((w & (1u << (slot % PLACEMENT_MASK_BITS))) == 0)
        return;

    const uint proto = min(pc.instances.Get().items[slot].prototypeId,
        PLACEMENT_MAX_PROTOS - 1);
    uint n;
    InterlockedAdd(pc.bins.Get().v[PLACEMENT_BINS_APPEND_BASE + proto], 1u, n);
    pc.visible.Get().v[pc.bins.Get().v[PLACEMENT_BINS_OFFSET_BASE + proto] + n]
        = slot;
}
