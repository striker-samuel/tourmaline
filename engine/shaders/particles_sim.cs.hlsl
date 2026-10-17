// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU simulation pass: one thread per pool slot ([64,1,1], dispatch
// ceil(maxParticles / 64) — see smvk::Particles::SimGroupsFor).
//
// Alive slots integrate (per-emitter drag + gravityScale over the global
// gravity push), age, spin; slots whose life crosses zero this frame are
// marked dead and pushed back onto the free-list stack (atomic) for the
// NEXT frame's emit pass (emit runs before sim within a frame, so a slot
// freed here is only reused one frame later — no double-pop).
//
// Dead slots (life already <= 0) cost one load + branch: the pool is a
// fixed-size array, there is no compaction pass by design (the draw VS
// collapses dead slots to degenerate quads instead).

#include "particles.hlsli"

[[vk::push_constant]]
ConstantBuffer<ParticlesSimPush> pcSim;

[numthreads(64, 1, 1)]
void mainCS(uint3 dispatchId : SV_DispatchThreadID)
{
    uint i = dispatchId.x;
    if (i >= pcSim.maxParticles || i >= (uint)MAX_PARTICLES)
        return;

    ParticleGpu p = pcSim.particles.Get().particles[i];
    if (p.posLife.w <= 0.0f)
        return; // already dead (never spawned or recycled earlier)

    p.posLife.w -= pcSim.dt;
    if (p.posLife.w <= 0.0f)
    {
        // Dead this frame: mark and recycle the slot. Each alive slot can
        // die at most once per pass, so the stack can never overflow its
        // maxParticles + 1 capacity; the guard stays as a safety net.
        p.posLife.w = 0.0f;
        p.velSize.xyz = float3(0.0, 0.0, 0.0);
        pcSim.particles.Get().particles[i] = p;
        uint top;
        InterlockedAdd(pcSim.freeList.Get().count, 1u, top);
        if (top < pcSim.maxParticles)
            pcSim.freeList.Get().slots[top] = i;
        else
            InterlockedAdd(pcSim.freeList.Get().count, -1u); // unreachable guard
        return;
    }

    // Emitter-driven dynamics (drag + gravityScale are per-emitter rows;
    // the ring slice is the same frame the emit pass read).
    uint e = (uint)(p.params.y + 0.5);
    if (e >= (uint)MAX_PARTICLE_EMITTERS)
        e = 0;
    ParticleEmitterGpu em = pcSim.emitters.Get().emitters[e];

    float3 vel = p.velSize.xyz;
    vel += float3(pcSim.gravityX, pcSim.gravityY, pcSim.gravityZ)
        * em.velocity.w * pcSim.dt;
    // Exponential drag, clamped so a big dt can never flip the velocity.
    float damp = exp(-max(em.velocity.z, 0.0) * pcSim.dt);
    vel *= damp;

    p.posLife.xyz += vel * pcSim.dt;
    // AMBIENT (camera-box) emitters (fade.w = wrap half-extent > 0):
    // toroidally wrap the world position into the box around the camera.
    // What leaves through one side re-enters through the opposite side, so
    // the volume stays permanently populated with ZERO dependence on
    // respawn catching up with camera movement - rain falls out the
    // bottom and re-enters at the top, dust drifts forever. frac() keeps
    // the wrap total for negative deltas (fmod would mirror them).
    if (em.fade.w > 0.0)
    {
        const float h = max(em.fade.w, 0.25);
        const float span = 2.0 * h;
        float3 d = p.posLife.xyz - float3(pcSim.cameraX, pcSim.cameraY, pcSim.cameraZ);
        d = frac((d + h) / span) * span - h;
        p.posLife.xyz = float3(pcSim.cameraX, pcSim.cameraY, pcSim.cameraZ) + d;
    }
    p.velSize.xyz = vel;
    p.colorRot.w += p.params.z * pcSim.dt;
    pcSim.particles.Get().particles[i] = p;
}
