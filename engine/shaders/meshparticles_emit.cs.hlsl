// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// meshparticles_emit.cs.hlsl — the MESH-particle emitter pass (PR06):
// the billboard emit's exact scheduling shape (rate*dt + the GPU-OWNED
// fractional carry; free-list pops; per-spawn PCG) with the MESH-particle
// payload: a quaternion orientation, an angular velocity, the mesh bin,
// and a sizeStart->sizeEnd uniform-scale envelope.

#include "meshparticles.hlsli"
#include "gpu_platform.hlsli"

GPU_PUSH(MeshParticlesEmitPush, pcEmit);

// Pop one slot off the free-list stack; false when the pool is exhausted
// (the transient underflow restores itself — the billboard kernel's
// proven form).
bool PopFreeSlot(uint maxParticles, out uint slot)
{
    uint old;
    InterlockedAdd(pcEmit.freeList.Get().count, -1u, old);
    if (old < 1u || old > maxParticles)
    {
        InterlockedAdd(pcEmit.freeList.Get().count, 1u);
        return false;
    }
    slot = pcEmit.freeList.Get().slots[old - 1u];
    return slot < maxParticles;
}

// Cone sample (the billboard emit's form, verbatim).
void SampleCone(float3 center, float3 axis, float radius, float spread,
    float speed, inout uint rng, out float3 pos, out float3 vel)
{
    float3 up = abs(axis.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, axis));
    float3 bitangent = cross(axis, tangent);

    float ang = ParticleRand01(rng) * 2.0 * 3.14159265;
    float rr = radius * sqrt(ParticleRand01(rng));
    float3 disc = tangent * (cos(ang) * rr) + bitangent * (sin(ang) * rr);
    pos = center + disc;

    float theta = spread * sqrt(ParticleRand01(rng));
    float phi = ParticleRand01(rng) * 2.0 * 3.14159265;
    float st, ct, sp, cp;
    sincos(theta, st, ct);
    sincos(phi, sp, cp);
    float3 dir = axis * ct + tangent * (st * cp) + bitangent * (st * sp);
    vel = dir * speed;
}

[numthreads(1, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID)
{
    uint e = groupId.x;
    if (e >= pcEmit.emitterCount || e >= (uint)MESHPARTICLE_MAX_EMITTERS)
        return;

    MeshParticleEmitterGpu em = pcEmit.emitters.Get().e[e];
    float rate = em.life.z;
    if (rate <= 0.0f)
        return;

    // ---- Spawn quota: rate * dt + carry (GPU-owned accumulator). ----
    float acc = pcEmit.emitterState.Get().carry[e] + rate * pcEmit.dt;
    float quotaF = floor(acc);
    float carry = acc - quotaF;
    pcEmit.emitterState.Get().carry[e] = carry;
    uint quota = quotaF > 0.0f ? (uint)quotaF : 0u;
    if (quota == 0u)
        return;

    float3 axis = em.direction.xyz;
    float axisLen = length(axis);
    axis = axisLen > 1e-5 ? axis / axisLen : float3(0, 1, 0);

    const uint meshId = min((uint)(em.life.w + 0.5),
        (uint)MESHPARTICLE_MAX_BINS - 1u);

    for (uint k = 0; k < quota; ++k)
    {
        uint slot;
        if (!PopFreeSlot(pcEmit.maxParticles, slot))
            break; // pool exhausted: remaining quota drops this frame

        // RNG stream: emitter seed + frame salt + spawn index.
        uint rng = ParticleHash((uint)em.spin.z + pcEmit.frameSeed * 7919u
            + k * 104729u + e * 31u);

        float life = lerp(em.life.x, em.life.y, ParticleRand01(rng));
        life = max(life, 1e-3); // a zero life would die (and recycle) on spawn

        float speed = lerp(em.velocity.x, em.velocity.y, ParticleRand01(rng));

        // The spawn shapes: cone around the axis (the mesh default) —
        // the billboard's cone/sphere/point set applies unchanged.
        float3 pos, vel;
        SampleCone(em.position.xyz, axis, em.position.w, em.direction.w,
            speed, rng, pos, vel);

        // The MESH-particle specifics: a random ORIENTATION (a uniform
        // random quaternion via the Shoemake form: u0 = w, u1..u3 axes)
        // and an ANGULAR VELOCITY on a random axis with the emitter's
        // magnitude band.
        float u0 = ParticleRand01(rng);
        float u1 = ParticleRand01(rng);
        float u2 = ParticleRand01(rng);
        float u3 = ParticleRand01(rng);

        float a = sqrt(1.0 - u1);
        float b = sqrt(u1);

        float4 rot = float4(
            a * sin(6.2831853 * u2),
            a * cos(6.2831853 * u2),
            b * sin(6.2831853 * u3),
            b * cos(6.2831853 * u3));

        float3 spinAxis = ParticleRandUnit(rng);
        float angVel = lerp(em.spin.x, em.spin.y, ParticleRand01(rng));

        // Size envelope + scale jitter.
        float scaleJit = 1.0 + (ParticleRand01(rng) * 2.0 - 1.0)
            * em.spin.w;
        scaleJit = max(scaleJit, 0.05);
        float sizeStart = em.size.x * scaleJit;

        // Per-channel tint jitter around the emitter base color.
        float jitter = em.size.z;
        float3 tint = em.color.rgb;
        if (jitter > 0.0)
        {
            tint *= float3(
                1.0 + (ParticleRand01(rng) * 2.0 - 1.0) * jitter,
                1.0 + (ParticleRand01(rng) * 2.0 - 1.0) * jitter,
                1.0 + (ParticleRand01(rng) * 2.0 - 1.0) * jitter);
            tint = max(tint, float3(0.0, 0.0, 0.0));
        }

        MeshParticleGpu p;
        p.posLife = float4(pos, life);
        p.velSize = float4(vel, sizeStart);
        p.rot = rot;
        p.angVelGrav = float4(spinAxis * angVel, em.velocity.w);
        p.colorEmit = float4(tint, em.size.w);
        p.params = float4(1.0 / life, (float)meshId, sizeStart, em.size.y);
        pcEmit.particles.Get().p[slot] = p;
    }
}
