// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// meshparticles_prefix.cs.hlsl — the bin prefix sums (PR06): ONE thread
// (<= 8 mesh bins — a parallel scan would cost more than the dispatch).
// counts -> exclusive offsets + total. The bin slab is the PR03 chain's
// PlacementBinsBuffer shape (flat words with named offsets).

#include "meshparticles.hlsli"
#include "gpu_platform.hlsli"

// Must match C++ MeshParticlesPrefixPush (32 bytes).
struct MeshParticlesPrefixPush
{
    GPU_PTR(PlacementBinsBuffer) bins; // 0
    uint binCount; // 8 <= MESHPARTICLE_MAX_BINS
    uint pad0;     // 12
    uint pad1;     // 16
    uint pad2;     // 20
    uint pad3;     // 24
    uint pad4;     // 28
};

GPU_PUSH(MeshParticlesPrefixPush, pc);

[numthreads(1, 1, 1)]
void mainCS()
{
    uint acc = 0;
    [unroll]
    for (uint b = 0; b < MESHPARTICLE_MAX_BINS; ++b)
    {
        pc.bins.Get().v[PLACEMENT_BINS_OFFSET_BASE + b] = acc;
        acc += pc.bins.Get().v[PLACEMENT_BINS_COUNT_BASE + b];
    }
    pc.bins.Get().v[PLACEMENT_BINS_TOTAL] = acc;
}
