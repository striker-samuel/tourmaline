// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Tiled terrain fragment shader (near tiles + far L3).
//
// Lambert sun + EvaluateIrradiance sky ambient + env fog — the SAME env
// block the scene reads (FrameUniforms.env), so the terrain composites
// with the world's lighting/exposure.
//
// Shadows (T3): shadowmap RECEIVE (lights[0] sun convention + PCF/EVSM
// cascade helpers, same as default_pom) MINNED with the heightfield
// RAYMARCHED distant self-shadow (terrain_shadow.hlsli, HZD-style: covers
// inter-ridge shadows out to 1.5 km, including beyond the last cascade).
// Mode/steps ride the 32B tail (C++ TerrainBlockPush mirror).
//
// Push: ONE block (DXC single-push rule) — the scene mirror + terrain tail
// in terrain_push.hlsli (TERRAIN_NOPC excludes the scene pc from this
// stage). Shadow helpers use the direct (non-FromPC) variants on pcT.
#define TERRAIN_NOPC
#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"

// Must be defined after common.hlsli symbols.
#include "shadows.hlsli"
#include "terrain_shadow.hlsli"
#include "terrain_push.hlsli"

static const float PI = 3.14159265359f;

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 tangentBFlip : TEXCOORD2;
    [[vk::location(3)]] float4 color : COLOR1;
};

float4 mainPS(VSOut IN) : SV_Target0
{
    FrameUniforms frame = pcT.frameUniforms.Get();
    EnvironmentFrameData env = frame.env;

    float3 N = normalize(IN.normal);
    float3 albedo = IN.color.rgb;

    // Direct sun (Lambert 1/PI, the scene convention).
    float3 L = normalize(env.sunDirection.xyz + 1e-9);
    float NdotL = saturate(dot(N, L));
    float3 sunE = env.sunColor.rgb * max(env.sunColor.w, 0.0) * NdotL;

    // Shadow: shadowmap receive (lights[0] sun convention; the numCascades
    // guard is the pass-off switch, same as the cascade debug view).
    float shadow = 1.0f;
    if (pcT.hfShadow >= 1)
    {
        Light light = pcT.bLights.Get().lights[0];
        if (light.shadow_map_index >= 0 && pcT.numCascades > 0)
        {
            uint evsm = pcT.frameUniforms.Get().evsmMode;
            if (evsm != 0u)
                shadow = GetShadowFactorEvsmCascaded(0, IN.worldPos, N, -L,
                    light.shadow_map_index, pcT.numCascades,
                    frame.view, pcT.cascadeBuffer,
                    (uint)light.shadow_map_index, EVSM_EXPONENT, IN.pos.xy);
            else
                shadow = GetShadowFactorCascaded(0, IN.worldPos, N, -L,
                    light.shadow_map_index, pcT.numCascades,
                    frame.view, pcT.cascadeBuffer);
        }
        // Heightfield raymarch: min() keeps the darkest occluder without
        // double-darkening where both agree.
        if (pcT.hfShadow >= 2)
        {
            float hf = TerrainRaymarchShadow(pcT.atlasSlot,
                float2(pcT.originX, pcT.originZ), pcT.invWorldSize,
                pcT.baseY, pcT.range, IN.worldPos, L, pcT.hfSteps);
            shadow = min(shadow, hf);
        }
    }
    sunE *= shadow;

    // Sky ambient (hemisphere blend of zenith/horizon/ground). Analytic
    // env zones override it per pixel: the covering OBB's tint +
    // intensity blend with the plain look by blendWeight * falloff.
    float3 skyE = EvaluateIrradiance(N, env);
    float zoneWeight = 0.0;
    EnvZoneLook zoneLook;
    if (EnvZoneAt(IN.worldPos, frame, zoneWeight, zoneLook))
        skyE = lerp(skyE, EnvZoneIrradiance(zoneLook, N, env), zoneWeight);

    float3 color = albedo * (sunE / PI + skyE);

    // Env fog (same factor/mode the scene pixels blend with). Zones own
    // the fog color + density: the zone look lerps with the plain env by
    // the same weight.
    float viewDepth = length(IN.worldPos - frame.cameraPos.xyz);
    float3 fogHdr = env.fogColor.rgb * env.fogColor.w;
    float fogDensity = env.fogDensityHeight.x;
    if (zoneWeight > 0.0)
    {
        float3 zoneFogHdr;
        float zoneFogDensity;
        EnvZoneFog(zoneLook, frame, zoneFogHdr, zoneFogDensity);
        fogHdr = lerp(fogHdr, zoneFogHdr, zoneWeight);
        fogDensity = lerp(fogDensity, zoneFogDensity, zoneWeight);
    }
    float fog = ComputeFogFactorDensity(viewDepth, IN.worldPos.y, env,
        fogDensity);
    color = lerp(color, fogHdr, fog);

    return float4(color, 1.0);
}
