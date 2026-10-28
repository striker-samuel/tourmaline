// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// meshparticles_scatter.cs.hlsl — the visible-index scatter (PR06): one
// thread per pool slot; mask-set slots atomically claim their mesh
// bin's next position and write their SOURCE slot into the visible
// list — the PR03 scatter's form (bounded by construction: exactly
// counts[bin] mask-set slots of that bin exist). Within-bin order is
// arbitrary per frame (atomics) — documented: the buffer position is
// NOT an identity.

#include "meshparticles.hlsli"

[[vk::push_constant]]
ConstantBuffer<MeshParticlesScatterPush> pc;

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const uint slot = groupId.x * 64 + threadId.x;
    if (slot >= pc.maxParticles)
        return;

    const uint w = pc.mask.Get().w[slot / PLACEMENT_MASK_BITS];
    if ((w & (1u << (slot % PLACEMENT_MASK_BITS))) == 0)
        return;

    const MeshParticleGpu p = pc.particles.Get().p[slot];
    const uint bin = min((uint)(p.params.y + 0.5),
        (uint)MESHPARTICLE_MAX_BINS - 1u);
    uint n;
    InterlockedAdd(pc.bins.Get().v[PLACEMENT_BINS_APPEND_BASE + bin], 1u, n);
    pc.visible.Get().v[pc.bins.Get().v[PLACEMENT_BINS_OFFSET_BASE + bin] + n]
        = slot;
}
