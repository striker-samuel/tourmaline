// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// meshparticles_sim.cs.hlsl — the MESH-particle simulation pass (PR06):
// one thread per pool slot, the billboard sim's integration shape
// (gravity/aging + the free-list recycle) extended with:
//
//   * the QUATERNION SPIN: the EXACT half-angle increment
//     dq = (axis * sin(|w|*dt/2), cos(|w|*dt/2)), q' = dq * q,
//     re-normalized — constant omega per step, not the first-order
//     q + 0.5*dt*(omega-quaternion)*q form;
//   * the SIZE ENVELOPE: the uniform scale lerps sizeStart -> sizeEnd
//     over the life fraction (posLife.w * invLifeMax = the remaining
//     fraction; the lerp runs on the AGE fraction);
//   * the gravityScale lives ON THE PARTICLE (baked at spawn from its
//     emitter row) — no emitter ring read here, so a parameter
//     re-upload mid-life never changes in-flight dynamics. NO DRAG in
//     v1 (the debris/leaf motion envelope is dominated by gravity +
//     spin; a drag field is a refinement — see the dynamics section).
//
// Dead slots (life already <= 0) cost one load + branch — no compaction
// pass by design; the cull mask keeps them out of the bins, so the draw
// never fetches one.

#include "meshparticles.hlsli"
#include "gpu_platform.hlsli"

GPU_PUSH(MeshParticlesSimPush, pcSim);

[numthreads(64, 1, 1)]
void mainCS(uint3 dispatchId : SV_DispatchThreadID)
{
    uint i = dispatchId.x;
    if (i >= pcSim.maxParticles || i >= (uint)MESHPARTICLE_MAX)
        return;

    MeshParticleGpu p = pcSim.particles.Get().p[i];
    if (p.posLife.w <= 0.0f)
        return; // already dead (never spawned or recycled earlier)

    p.posLife.w -= pcSim.dt;
    if (p.posLife.w <= 0.0f)
    {
        // Dead this frame: mark and recycle the slot (each alive slot
        // can die at most once per pass — the stack cannot overflow).
        p.posLife.w = 0.0f;
        p.velSize.xyz = float3(0.0, 0.0, 0.0);
        pcSim.particles.Get().p[i] = p;
        uint top;
        InterlockedAdd(pcSim.freeList.Get().count, 1u, top);
        if (top < pcSim.maxParticles)
            pcSim.freeList.Get().slots[top] = i;
        else
            InterlockedAdd(pcSim.freeList.Get().count, -1u); // unreachable guard
        return;
    }

    // ---- Linear dynamics: gravity (the particle's baked gravityScale)
    // + exponential drag (baked at spawn). ----
    float3 vel = p.velSize.xyz;
    const float gravScale = p.angVelGrav.w;
    // v1 mesh particles carry no drag (the debris/leaf motion envelope
    // is dominated by gravity + spin; a drag field is a refinement).
    vel += float3(pcSim.gravityX, pcSim.gravityY, pcSim.gravityZ)
        * gravScale * pcSim.dt;
    p.velSize.xyz = vel;
    p.posLife.xyz += vel * pcSim.dt;

    // ---- The SPIN: the quaternion angular integration. ----
    float3 w = p.angVelGrav.xyz; // axis * rad/s
    const float wl = length(w);
    if (wl > 1e-6)
    {
        // The omega quaternion (half-angle form):
        // dq = (axis * sin(|w|*dt/2), cos(|w|*dt/2)); q' = dq * q.
        const float half = wl * pcSim.dt * 0.5;
        float4 dq = float4(normalize(w) * sin(half), cos(half));
        float4 q = p.rot;
        float4 qr = float4(
            dq.w * q.xyz + q.w * dq.xyz + cross(dq.xyz, q.xyz),
            dq.w * q.w - dot(dq.xyz, q.xyz));
        p.rot = normalize(qr);
    }

    // ---- The size envelope: the CLEAN lerp sizeStart -> sizeEnd over
    // the age fraction (both endpoints persist in params — the per-frame
    // size never drifts). ----
    const float ageFrac = saturate(1.0 - p.posLife.w * p.params.x);
    p.velSize.w = lerp(p.params.z, p.params.w, ageFrac);

    pcSim.particles.Get().p[i] = p;
}
