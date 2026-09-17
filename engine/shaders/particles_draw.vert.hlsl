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
    // Degenerate parking spot: NDC (4, 4) with w = 1 — outside every clip
    // plane, covers no viewport pixel, no NaNs.
    const float4 kDegenerate = float4(4.0, 4.0, 0.5, 1.0);

    if (instance >= pcDraw.maxParticles || instance >= (uint)MAX_PARTICLES)
    {
        o.clipPos = kDegenerate;
        o.worldPos = float3(0.0, 0.0, 0.0);
        o.color = float4(0.0, 0.0, 0.0, 0.0);
        o.quad = float2(0.0, 0.0);
        o.params = float4(0.0, 0.0, 0.0, 0.0);
        return o;
    }

    ParticleGpu p = pcDraw.particles.Get().particles[instance];
    if (p.posLife.w <= 0.0f)
    {
        o.clipPos = kDegenerate;
        o.worldPos = float3(0.0, 0.0, 0.0);
        o.color = float4(0.0, 0.0, 0.0, 0.0);
        o.quad = float2(0.0, 0.0);
        o.params = float4(0.0, 0.0, 0.0, 0.0);
        return o;
    }

    FrameUniforms frame = pcDraw.frameUniforms.Get();
    uint e = (uint)(p.params.y + 0.5);
    if (e >= (uint)MAX_PARTICLE_EMITTERS)
        e = 0;
    ParticleEmitterGpu em = pcDraw.emitters.Get().emitters[e];

    // Life fraction 0..1 (x = 1/lifeMax) drives size lerp + fades.
    float t = saturate(1.0 - p.posLife.w * p.params.x);
    float size = lerp(max(em.size.x, 1e-4), max(em.size.y, 1e-4), t);

    // Quad corner, spun by the particle rotation around the view axis.
    float2 corner = kQuadCorners[vertId < 6u ? vertId : 0u];
    float s, c;
    sincos(p.colorRot.w, s, c);
    float2 spun = float2(
        corner.x * c - corner.y * s,
        corner.x * s + corner.y * c);

    // View-space billboard: offset (right, up, 0), then project.
    float3 centerView = mul(frame.view, float4(p.posLife.xyz, 1.0)).xyz;
    float3 posView = centerView + float3(spun.x, spun.y, 0.0) * size;
    o.clipPos = mul(frame.proj, float4(posView, 1.0));
    o.worldPos = mul(frame.invView, float4(posView, 1.0)).xyz;
    o.color = float4(p.colorRot.rgb, 1.0);
    o.quad = corner;
    o.params = float4(t, (float)e, 0.0, 0.0);
    return o;
}
