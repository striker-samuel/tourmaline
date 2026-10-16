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

    // Sky ambient (hemisphere blend of zenith/horizon/ground). The
    // environment ZONES own the sky where the map is on: the tint +
    // intensity blend per texel (EnvZoneIrradiance), so painting zones
    // paints the terrain's ambient.
    float4 zw = EnvZoneWeights(IN.worldPos.xz, frame);
    float3 skyE = EnvZoneIrradiance(N, zw, env, frame);

    float3 color = albedo * (sunE / PI + skyE);

    // Env fog (same factor/mode the scene pixels blend with). Zones own
    // the fog color + density: the zone blend replaces the env's.
    float viewDepth = length(IN.worldPos - frame.cameraPos.xyz);
    float fogHdrIntensity = env.fogColor.w;
    float3 zoneFogHdr = env.fogColor.rgb * fogHdrIntensity;
    float zoneFogDensity = env.fogDensityHeight.x;
    if (frame.envZoneMapIndex != 0xffffffffu)
        EnvZoneFog(zw, frame, zoneFogHdr, zoneFogDensity);
    float fog = ComputeFogFactorDensity(viewDepth, IN.worldPos.y, env,
        zoneFogDensity);
    color = lerp(color, zoneFogHdr, fog);

    return float4(color, 1.0);
}
