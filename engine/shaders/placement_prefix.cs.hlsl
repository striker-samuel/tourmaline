// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_prefix.cs.hlsl — PR03 chain 2/4: the bin prefix sums.
// ONE thread (the bin count is <= 8 — the prototype budget): counts ->
// exclusive offsets + total. Serial-in-one-thread on purpose: a scan
// over 8 elements is cheaper than any parallel scan's dispatch cost,
// and the result is deterministic by construction.

#include "placement_common.hlsli"
#include "placement_cull.hlsli"

[[vk::push_constant]]
ConstantBuffer<PlacementCullPush> pc;

[numthreads(1, 1, 1)]
void mainCS()
{
    uint acc = 0;
    [unroll]
    for (uint p = 0; p < PLACEMENT_MAX_PROTOS; ++p)
    {
        pc.bins.Get().v[PLACEMENT_BINS_OFFSET_BASE + p] = acc;
        acc += pc.bins.Get().v[PLACEMENT_BINS_COUNT_BASE + p];
    }
    pc.bins.Get().v[PLACEMENT_BINS_TOTAL] = acc;
}
