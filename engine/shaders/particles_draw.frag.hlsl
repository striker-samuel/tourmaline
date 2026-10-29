// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Particle shading: soft radial sprite + emissive base + Forward+ tiled
// light accumulation — the same per-tile fetch as default_lit_body.hlsli
// (tile index from SV_Position, min(count, perTile) entries, emitter-index
// guard), with a particle-appropriate evaluation: Frostbite windowed
// falloff, spot cone, wrap-around NdotL (soft volume, no hard backface),
// no shadows (particles shade above that detail level).
//
// When the tile lists are NOT fresh (deferred-shadow pipeline owns the
// lights that frame: fpTileCountX == 0), the shader falls back to a capped
// brute-force loop over the light buffer so particles stay lit in both
// pipeline states.
//
// The sun is the env block's directional, evaluated unconditionally
// (outside the F+ gating: sun-only scenes with fpLightCount == 0 light
// too). UpdateLights builds the PBR/CSM bLights[0] straight from the same
// frameEnv that fills FrameUniforms.env, so the env copy here IS the
// scene sun (sunDirection.xyz = dir TO sun, sunColor.rgb * sunColor.w =
// lux) — same wrap-around NdotL, same no-shadow policy as the F+ terms
// (particles shade above that detail level), so interiors keep lit
// particles even where surfaces sit in CSM shadow.
//
// Output is additive HDR (BlendSettings::Add in the app): premultiplied by
// the sprite falloff alpha, sanitized to half-float range like every other
// HDR writer.

#define PARTICLES_DRAW_TYPES
#define PARTICLES_PASS // keep the scene-wide pc push out of scope
#include "common.hlsli"
#include "particles.hlsli"

[[vk::push_constant]]
ConstantBuffer<ParticlesDrawPush> pcDraw;

static const float PI = 3.14159265359f;

struct ParticleVsOut
{
    float4 clipPos : SV_Position;
    float3 worldPos : WORLDPOS;
    float4 color : COLOR0;
    float2 quad : QUAD0;
    float4 params : PARAMS0; // x = life t, y = emitter idx
};

// One Forward+ light against a particle (see the struct doc in
// default_lit_body.hlsli EvaluateForwardPlusLight for the source of the
// falloff math; simplified here: no Cook-Torrance, no shadows, wrap NdotL).
float3 EvaluateParticleLight(ForwardPlusLight pl, float3 worldPos, float3 N)
{
    float3 toLight = pl.position - worldPos;
    float dist2 = dot(toLight, toLight);
    if (pl.range > 0.0f && dist2 > pl.range * pl.range)
        return float3(0.0, 0.0, 0.0);
    float dist = sqrt(dist2);
    float3 Lp = toLight / max(dist, 1e-6);

    // Wrap-around diffuse: particles are thin fuzzy volumes, not solid
    // surfaces — lights behind them still glow through.
    float NdotL = saturate(dot(N, Lp) * 0.5 + 0.5);
    if (NdotL <= 0.0)
        return float3(0.0, 0.0, 0.0);

    float angular = 1.0;
    if (pl.type == 1)
    {
        float3 axis = normalize(pl.direction);
        float cosA = dot(-Lp, axis);
        float cosInner = cos(pl.innerAngle);
        float cosOuter = cos(pl.outerAngle);
        if (cosA < cosOuter)
            return float3(0.0, 0.0, 0.0);
        angular = smoothstep(cosOuter, cosInner, cosA);
    }

    // Frostbite-style windowed falloff, candela convention.
    float window_ = 1.0;
    if (pl.range > 0.0f)
    {
        float x = dist / max(pl.range, 1e-6);
        window_ = pow(saturate(1.0 - x * x * x * x), 2.0);
    }
    float atten = window_ / max(4.0 * PI * dist * dist, 1e-2);
    return pl.color.rgb * (pl.intensity * atten * angular * NdotL);
}

float4 mainPS(ParticleVsOut IN) : SV_Target
{
    // Soft disc sprite: smooth falloff to the quad edge.
    float r2 = dot(IN.quad, IN.quad);
    if (r2 > 1.0)
        discard;
    float falloff = 1.0 - r2;
    falloff *= falloff;

    uint e = (uint)(IN.params.y + 0.5);
    if (e >= (uint)MAX_PARTICLE_EMITTERS)
        e = 0;
    ParticleEmitterGpu em = pcDraw.emitters.Get().emitters[e];

    // Fade in over fadeIn fraction of life, out over the last fadeOut.
    float t = saturate(IN.params.x);
    float fade = 1.0;
    if (em.fade.x > 0.0)
        fade *= saturate(t / em.fade.x);
    if (em.fade.y > 0.0)
        fade *= saturate((1.0 - t) / em.fade.y);

    FrameUniforms frame = pcDraw.frameUniforms.Get();
    float3 N = normalize(frame.cameraPos - IN.worldPos);

    // ---- Forward+ tiled lights (or brute-force fallback) ----
    float3 lit = float3(0.0, 0.0, 0.0);
    const uint nlights = min(pcDraw.fpLightCount, (uint)MAX_FP_LIGHTS);
    if (nlights > 0u)
    {
        uint first, count;
        if (pcDraw.fpTileCountX > 0u)
        {
            uint tileSize = max(pcDraw.fpTileSize, 1u);
            uint tx = (uint)(IN.clipPos.x / (float)tileSize);
            uint ty = (uint)(IN.clipPos.y / (float)tileSize);
            if (tx < pcDraw.fpTileCountX)
            {
                uint tileIdx = ty * pcDraw.fpTileCountX + tx;
                // Shared effective capacity (common.hlsli FpTileLightSlots):
                // the cull producer clamps identically, so both sides
                // address the same rows whatever the configured
                // maxLightsPerTile (see the contract comment there).
                uint perTile = FpTileLightSlots(pcDraw.fpMaxPerTile);
                uint stride = perTile + (uint)MAX_FP_PROBE_TILE_SLOTS;
                // counts packs lights (low 16) + probes (high 16, dedicated
                // tail slots) — particles shade lights only, so mask.
                uint tcount = min(pcDraw.fpTileCounts.Get().counts[tileIdx] & 0xFFFFu, perTile);
                for (uint s = 0; s < tcount; ++s)
                {
                    uint li = pcDraw.fpTileIndices.Get().indices[tileIdx * stride + s];
                    if (li >= nlights)
                        continue;
                    lit += EvaluateParticleLight(
                        pcDraw.fpLights.Get().lights[li], IN.worldPos, N);
                }
            }
        }
        else
        {
            // Tile lists stale (deferred shadows own the lights): capped
            // direct walk so the fallback cannot explode on light-crowd
            // scenes (max 32 lights; the demo grids carry 1000+).
            const uint cap = min(nlights, 32u);
            for (uint li = 0; li < cap; ++li)
                lit += EvaluateParticleLight(
                    pcDraw.fpLights.Get().lights[li], IN.worldPos, N);
        }
    }

    // ---- Sun directional (env block: the sun's single source of truth) ----
    // Unconditional on purpose: a sun-only scene (fpLightCount == 0) must
    // still light particles, so this stays outside the nlights gate above.
    // The +1e-9 normalize guard follows environment.hlsli's sun eval: a
    // zero sunIntensity (lux 0) must zero the term, not NaN it through
    // normalize(0).
    float3 sunDir = normalize(frame.env.sunDirection.xyz + 1e-9);
    float sunNdotL = saturate(dot(N, sunDir) * 0.5 + 0.5);
    lit += frame.env.sunColor.rgb
        * (max(frame.env.sunColor.w, 0.0) * sunNdotL);

    // Emissive base (self-lit embers) + lit response, premultiplied by the
    // sprite falloff + life fades; additive blend composites over Main.
    float alpha = falloff * saturate(fade);
    float3 radiance = IN.color.rgb * (em.color.w + lit);
    return float4(SanitizeHdr(radiance * alpha), alpha);
}
