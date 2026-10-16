// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU emitter pass: one [1,1,1] thread group per emitter (same
// one-thread-per-thing shape as the Forward+ tile cull).
//
// Each thread:
//   1. turns its rate * dt into an integer spawn quota using the GPU-OWNED
//      fractional carry (emitterState buffer — persistent, never uploaded
//      by the CPU, so parameter re-uploads can never clobber scheduling),
//   2. pops quota pool slots off the free-list stack (atomic; a failed pop
//      means the pool is exhausted this frame — spawn drops, never blocks),
//   3. initializes each slot: spawn position/velocity by shape (cone /
//      sphere / point), life, rotation, per-channel tint jitter, all from a
//      per-spawn PCG stream salted with the emitter seed + frame seed.
//
// Runs before the sim pass over the SAME buffers; the rendergraph emits the
// emit -> sim barrier from the pass hints (particles + freeList RW).

#include "particles.hlsli"

[[vk::push_constant]]
ConstantBuffer<ParticlesEmitPush> pcEmit;

// Pop one slot off the free-list stack; false when the pool is exhausted.
// The transient count underflow is benign: every failing thread restores
// its decrement, and only a return value of old >= 1 hands a slot out.
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

// Cone sample: spawn point on the base disc (radius r, axis-orthogonal)
// and a velocity direction inside the spread cone around the axis,
// solid-angle weighted (theta = spread * sqrt(u)).
void SampleCone(float3 center, float3 axis, float radius, float spread,
    float speed, inout uint rng, out float3 pos, out float3 vel)
{
    // Stable axis-orthogonal basis (axis is normalized at upload).
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
    float3 dir = axis * ct
        + tangent * (st * cp) + bitangent * (st * sp);
    vel = dir * speed;
}

[numthreads(1, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID)
{
    uint e = groupId.x;
    if (e >= pcEmit.emitterCount || e >= (uint)MAX_PARTICLE_EMITTERS)
        return;

    ParticleEmitterGpu em = pcEmit.emitters.Get().emitters[e];
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

    for (uint k = 0; k < quota; ++k)
    {
        uint slot;
        if (!PopFreeSlot(pcEmit.maxParticles, slot))
            break; // pool exhausted: remaining quota drops this frame

        // RNG stream: emitter seed + frame salt + spawn index.
        uint rng = ParticleHash((uint)em.size.w + pcEmit.frameSeed * 7919u
            + k * 104729u + e * 31u);

        float life = lerp(em.life.x, em.life.y, ParticleRand01(rng));
        life = max(life, 1e-3); // a zero life would die (and recycle) on spawn

        float speed = lerp(em.velocity.x, em.velocity.y, ParticleRand01(rng));

        float3 pos, vel;
        const uint shape = (uint)(em.life.w + 0.5);
        if (shape == (uint)PARTICLE_SHAPE_SPHERE)
        {
            pos = em.position.xyz + em.position.w * ParticleRandInBall(rng);
            vel = ParticleRandUnit(rng) * speed;
        }
        else if (shape == (uint)PARTICLE_SHAPE_POINT)
        {
            SampleCone(em.position.xyz, axis, 0.0, em.direction.w,
                speed, rng, pos, vel);
        }
        else // PARTICLE_SHAPE_CONE (default)
        {
            SampleCone(em.position.xyz, axis, em.position.w, em.direction.w,
                speed, rng, pos, vel);
        }

        // Per-channel tint jitter around the emitter base color.
        float jitter = em.fade.z;
        float3 tint = em.color.rgb;
        if (jitter > 0.0)
        {
            tint *= float3(
                1.0 + (ParticleRand01(rng) * 2.0 - 1.0) * jitter,
                1.0 + (ParticleRand01(rng) * 2.0 - 1.0) * jitter,
                1.0 + (ParticleRand01(rng) * 2.0 - 1.0) * jitter);
            tint = max(tint, float3(0.0, 0.0, 0.0));
        }

        ParticleGpu p;
        p.posLife = float4(pos, life);
        p.velSize = float4(vel, 0.0);
        p.colorRot = float4(tint, ParticleRand01(rng) * 6.2831853);
        p.params = float4(1.0 / life, (float)e,
            em.size.z * (ParticleRand01(rng) < 0.5 ? -1.0 : 1.0),
            float(rng));
        pcEmit.particles.Get().particles[slot] = p;
    }
}
