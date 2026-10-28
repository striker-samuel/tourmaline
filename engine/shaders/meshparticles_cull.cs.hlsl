// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// meshparticles_cull.cs.hlsl — the MESH-particle visibility pass (PR06):
// one thread per pool slot up to maxParticles (the CPU-known dispatch
// bound). Live slots sit ANYWHERE in the pool (the free-list is a
// stack, not a compaction), so the walk covers the whole pool and dead
// slots early-out on posLife.w <= 0 (one load + branch — the billboard
// draw's dead-slot cost model).
//
// Alive + visible slots set their mask bit (InterlockedOr) and bump
// their MESH bin's count (atomics) — the PR03 chain's shapes.

#include "meshparticles.hlsli"

[[vk::push_constant]]
ConstantBuffer<MeshParticlesCullPush> pc;

[numthreads(64, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const uint slot = groupId.x * 64 + threadId.x;
    if (slot >= pc.maxParticles)
        return;
    const MeshParticleGpu p = pc.particles.Get().p[slot];
    if (p.posLife.w <= 0.0f)
        return; // dead: no mask bit, no bin count

    const uint bin = min((uint)(p.params.y + 0.5),
        (uint)MESHPARTICLE_MAX_BINS - 1u);
    // The conservative sphere: the per-MESH bound scaled by the
    // particle's CURRENT size (the envelope), at the particle position
    // — PlacementSphereVisible (the shared PR03 helper; particles carry
    // no species distance in v1).
    const bool visible = PlacementSphereVisible(pc.renderViews,
        pc.renderViewIndex, pc.cullFlags, pc.meshBounds[bin],
        0.0f, pc.camPos, p.posLife.xyz,
        float3(p.velSize.w, p.velSize.w, p.velSize.w));
    if (!visible)
        return;

    InterlockedOr(pc.mask.Get().w[slot / PLACEMENT_MASK_BITS],
        1u << (slot % PLACEMENT_MASK_BITS));
    uint binCount; // unused — the prefix pass reads counts[]
    InterlockedAdd(pc.bins.Get().v[PLACEMENT_BINS_COUNT_BASE + bin],
        1u, binCount);
}
