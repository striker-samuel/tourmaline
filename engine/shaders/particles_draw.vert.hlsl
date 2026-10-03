// Particle billboards: camera-facing quads expanded entirely in the vertex
// shader from SV_VertexID + SV_InstanceID — no vertex buffer, no CPU-side
// alive list (the app issues a fixed Draw(6, maxParticles)). Dead slots
// (life <= 0) collapse to a degenerate quad parked off-screen: every
// coordinate is outside the clip volume, so no pixels survive, but the
// values stay finite (w = 1) to keep rasterizer input well-formed.
//
// Billboard basis: work in VIEW space (camera looks down -Z, right +X, up
// +Y by construction), offset the quad there, then project. This sidesteps
// any matrix-layout ambiguity of extracting world-space camera axes.
// worldPos for lighting comes back through invView.

#define PARTICLES_DRAW_TYPES
#define PARTICLES_PASS // keep the scene-wide pc push out of scope
#include "common.hlsli"
#include "particles.hlsli"

[[vk::push_constant]]
ConstantBuffer<ParticlesDrawPush> pcDraw;

struct ParticleVsOut
{
    float4 clipPos : SV_Position;
    float3 worldPos : WORLDPOS;
    float4 color : COLOR0;   // rgb spawn tint, a unused
    float2 quad : QUAD0;     // -1..1 quad-space uv (pre-rotation)
    float4 params : PARAMS0; // x = life t (0 spawn .. 1 death), y = emitter idx
};

static const float2 kQuadCorners[6] =
{
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0),
};

ParticleVsOut mainVS(uint vertId : SV_VertexID, uint instance : SV_InstanceID)
{
    ParticleVsOut o;

    const float4 kDegenerate = float4(4.0, 4.0, 0.5, 1.0);

    if (instance >= pcDraw.maxParticles ||
        instance >= (uint) MAX_PARTICLES)
    {
        o.clipPos = kDegenerate;
        o.worldPos = 0.0f;
        o.color = 0.0f;
        o.quad = 0.0f;
        o.params = 0.0f;
        return o;
    }

    // Death test BEFORE the remaining field loads: dead slots cost one
    // 16-byte BDA read (posLife carries life in .w) instead of the full
    // 48 bytes the old `ParticleGpu p = ...` aggregate pulled first (DXC
    // scalarized the aggregate to the same three narrow loads, but issued
    // all of them above this branch). NOTE: -O3 is free to hoist
    // side-effect-free loads back above the branch — after rebuilding,
    // disassemble the .spv (spirv-dis) and confirm the params/colorRot
    // OpLoads sit below the early-out OpBranchConditional; if they moved
    // back up, this ordering bought nothing and should be revisited.
    float4 posLife =
        pcDraw.particles.Get().particles[instance].posLife;

    if (posLife.w <= 0.0f)
    {
        o.clipPos = kDegenerate;
        o.worldPos = 0.0f;
        o.color = 0.0f;
        o.quad = 0.0f;
        o.params = 0.0f;
        return o;
    }

    float4 pParams =
        pcDraw.particles.Get().particles[instance].params;
    float4 colorRot =
        pcDraw.particles.Get().particles[instance].colorRot;

    uint e = (uint) (pParams.y + 0.5f);
    if (e >= (uint) MAX_PARTICLE_EMITTERS)
        e = 0;

    ParticleEmitterGpu em =
        pcDraw.emitters.Get().emitters[e];

    float t =
        saturate(1.0f - posLife.w * pParams.x);

    float size =
        lerp(
            max(em.size.x, 1e-4f),
            max(em.size.y, 1e-4f),
            t);

    float2 corner =
        kQuadCorners[vertId < 6u ? vertId : 0u];

    float s, c;
    sincos(colorRot.w, s, c);

    float2 spun = float2(
        corner.x * c - corner.y * s,
        corner.x * s + corner.y * c);

    float2 offset = spun * size;

    float4 centerClip = mul(
        pcDraw.frameUniforms.Get().viewProj,
        float4(posLife.xyz, 1.0f));

    float4 clipOffset = mul(
        pcDraw.frameUniforms.Get().proj,
        float4(offset, 0.0f, 0.0f));

    o.clipPos = centerClip + clipOffset;

    float3 worldOffset = mul(
        pcDraw.frameUniforms.Get().invView,
        float4(offset, 0.0f, 0.0f)).xyz;

    o.worldPos = posLife.xyz + worldOffset;

    o.color = float4(colorRot.rgb, 1.0f);
    o.quad = corner;
    o.params = float4(t, (float) e, 0.0f, 0.0f);

    return o;
}