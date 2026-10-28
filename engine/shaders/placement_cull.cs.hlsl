// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_cull.cs.hlsl — PR03 chain 1/4: per-instance visibility.
// One thread per live instance slot (dispatch over liveCount, the
// CPU-known clamped bound — no count readback). Tests the per-proto
// cooked-mesh sphere against the view frustum, the constraint-alive
// flag and the species draw distance; writes the bitpack mask
// (InterlockedOr) and bumps the per-proto bin counts (atomics).
// All outputs are order-free; within-bin order is finalized by the
// scatter pass (documented: the buffer position is NOT an identity —
// overrides key by StableID).

#include "placement_common.hlsli"
#include "placement_cull.hlsli"

[[vk::push_constant]]
ConstantBuffer<PlacementCullPush> pc;

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    // Self-bounded: the LIVE count is GPU-side (min of the raw count and
    // the capacity) — the dispatch width is the CPU-known capacity upper
    // bound; threads past the live count exit. No count readback on the
    // frame path.
    const uint liveCount = min(pc.count.Get().count, pc.count.Get().capacity);
    const uint slot = groupId.x * 64 + threadId.x;
    if (slot >= liveCount)
        return;

    const PlacementRenderInstance inst = pc.instances.Get().items[slot];
    const uint proto = min(inst.prototypeId, PLACEMENT_MAX_PROTOS - 1);

    bool visible = PlacementSphereVisible(pc.renderViews, pc.renderViewIndex,
        pc.cullFlags, pc.protoBounds[proto], pc.protoDist[proto], pc.camPos,
        inst.position, inst.scale);
    if (visible && (pc.cullFlags & PLACEMENT_CULL_ALIVE) != 0
        && pc.alive.Get().v[slot].y == 0)
        visible = false; // constraint-killed instance

    if (visible)
    {
        InterlockedOr(pc.mask.Get().w[slot / PLACEMENT_MASK_BITS],
            1u << (slot % PLACEMENT_MASK_BITS));
        uint binCount; // unused by design — the prefix pass reads counts[]
        InterlockedAdd(pc.bins.Get().v[PLACEMENT_BINS_COUNT_BASE + proto],
            1u, binCount);
    }
}
