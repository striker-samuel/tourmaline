// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"
#include "material_evaluation.hlsli"

// Must be defined after common.hlsli symbols
#include "shadows.hlsli"

// NAIVE water preview (the PBR debugger scaffolding's shading-model test
// bed): Fresnel sky reflection over procedural sum-of-sines wave normals,
// a GGX sun glint and a flat "body color" refraction stand-in. NO
// refraction pass, NO depth-based absorption/Beer-Lambert, NO foam, NO
// flow maps, NO SSR hookup (the env sample IS the reflection). Deliber-
// ately naive: a development surface, not a shipping model.
//
// What it does per pixel:
//   - material inputs via EvaluatePbrMaterial (base color = the water
//     volume/scatter tint; metallic/roughness IGNORED — water's F0 is
//     fixed at 0.02 and the glint roughness is a constant)
//   - the wave normal: three directional sines summed over WORLD xz
//     (frame-time animated), perturbing the (normal-mapped) surface
//     normal — slopes accumulated analytically, no texture fetches
//   - reflection: EvaluateSpecularEnv (the POM view's analytic sky IBL
//     + probe convention) over the wave normal, Fresnel-weighted
//   - refraction stand-in: bodyColor * sky irradiance (flat, no depth)
//   - sun glint: Cook-Torrance GGX at a fixed tight roughness, cascaded
//     shadowed
//   - fog, real HDR like F1/F11
//
// Pairs with default.vert.hlsl / gfxfiltered.vert.hlsl (the scene-pc FS
// family PSIn). Alpha cutout + dithered Blend + dither-on-closeup match
// the lit path (shoreline cards / masked water planes agree with F1).

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

static const float PI = 3.14159265359f;

// Water knobs (demo constants — the development surface).
static const float  kWaterF0        = 0.02f;  // the physical water dielectric
static const float  kWaterGlintRough = 0.06f; // sun glint lobe width
static const float2 kWaterDirs[3] = {
    float2(1.0f, 0.30f), float2(-0.70f, 0.70f), float2(0.20f, -1.0f)
};
static const float kWaterFreq[3]  = { 0.90f, 1.70f, 3.10f }; // rad / m
static const float kWaterAmp[3]   = { 0.060f, 0.030f, 0.015f }; // slope scale
static const float kWaterSpeed[3] = { 0.80f, 1.20f, 1.60f };  // rad / s

// Cook-Torrance helpers (the lit body's formulas, verbatim).
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0f);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = PI * denom * denom;
    return a2 / max(denom, 1e-7f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float denom = NdotV * (1.0f - k) + k;
    return NdotV / max(denom, 1e-7f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

// Wave normal over WORLD xz: the analytic slope of a sum of directional
// sines, accumulated in the two horizontal derivatives and folded into a
// unit normal. Phase = dot(dir, p) * freq + t * speed.
float3 WaterWaveNormal(float2 p, float t)
{
    float dhx = 0.0f;
    float dhz = 0.0f;
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        float2 d = normalize(kWaterDirs[i]);
        float phase = dot(d, p) * kWaterFreq[i] + t * kWaterSpeed[i];
        float s = cos(phase) * kWaterAmp[i] * kWaterFreq[i];
        dhx += d.x * s;
        dhz += d.y * s;
    }
    return normalize(float3(-dhx, 1.0f, -dhz));
}

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    FrameUniforms frame = pc.frameUniforms.Get();
    FramePSHot hot = frame.frameHot.Get();

    // Shared material evaluation. Metallic/roughness are IGNORED below
    // (water fixes F0/glint width) — the evaluator still runs so the
    // Material Health contract stays one evaluation site and the view
    // flags a water material authored with garbage inputs (the Health
    // view reads the SAME evaluation).
    PSInBridge bridge = { IN.color, IN.uv };
    EvaluatedPbrMaterial e = EvaluatePbrMaterial(pc.materialId, bridge);

    const float alphaCutoff = mat.params.x;
    const float shadingModelF = mat.params.w;
    const float alphaModeF = mat.params2.w;
    const bool isBlendMat = (alphaModeF == 2.0f);
    const bool ditheredTransp = (pc.pad_pc == 0);

    // Lit-path visibility decisions (same order as the family).
    if (IN.ditherCloseup != 0u
        && !CloseupDitherKeep(IN.worldPos, hot.cameraPos, clipPos.xy))
        discard;
    if (shadingModelF == 2.0f && e.alpha < alphaCutoff)
        discard;
    if (isBlendMat && ditheredTransp && !DitherKeep(e.alpha, clipPos.xy))
        discard;

    // ---------------------------------------------------------------
    // Base normal: guarded vertex normal (flipped), optional normal-map
    // perturbation (the lit body's two-channel discipline), then the
    // PROCEDURAL wave perturbation layered on top in world space.
    // ---------------------------------------------------------------
    float3 N = normalize(IN.normal);
    if (!(dot(N, N) >= 0.5f))
        N = float3(0.0f, 1.0f, 0.0f);
    if (!IN.isFrontFace)
        N = -N;
    float3 geomN = N;

    {
        float3 T = IN.tangent.xyz;
        float3 Tg = T - N * dot(N, T);
        float tgLen = length(Tg);
        if (tgLen > 1e-6f) { T = Tg / tgLen; }
        else {
            float3 axis = abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            T = normalize(cross(axis, N));
        }
        float tw = IN.tangent.w;
        tw = (tw == 0.0f || isnan(tw)) ? 1.0f : sign(tw);
        float3 B = cross(N, T) * tw;

        const int normalIdx = mat.tex.z;
        if (normalIdx >= 0)
        {
            float2 mapXY = textures[NonUniformResourceIndex((uint) normalIdx)]
                .Sample(samplers[0], IN.uv).xy * 2.0f - 1.0f;
            mapXY *= mat.params2.y;
            float mapZ = sqrt(saturate(1.0f - dot(mapXY, mapXY)));
            float3 mapN = normalize(float3(mapXY, mapZ));
            float3 Nc = T * mapN.x + B * mapN.y + N * mapN.z;
            float ncLen = length(Nc);
            if (ncLen > 1e-12f)
                N = Nc / ncLen;
        }
    }

    // Wave layer: perturb along the wave normal's horizontal delta (the
    // naive composition — renormalized once, so a steep map and a steep
    // wave cannot produce a non-unit shading normal).
    {
        float3 waveN = WaterWaveNormal(IN.worldPos.xz, hot.time);
        float3 Nc = N + float3(waveN.x, 0.0f, waveN.z);
        float ncLen = length(Nc);
        if (ncLen > 1e-12f)
            N = Nc / ncLen;
    }

    // ---------------------------------------------------------------
    // Sun + cascaded shadow (the family convention).
    // ---------------------------------------------------------------
    Light light = pc.bLights.Get().lights[0];
    float3 L = normalize(-light.direction);
    float3 V = normalize(hot.cameraPos - IN.worldPos);
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0f);
    float NdotV = max(dot(N, V), 0.0f);

    float shadow = 1.0f;
    if (light.shadow_map_index >= 0 && pc.numCascades > 0)
    {
        uint evsm = frame.evsmMode;
        if (evsm != 0u)
            shadow = GetShadowFactorEvsmCascadedFromPC(0, IN.worldPos, geomN, light,
                (uint)light.shadow_map_index, EVSM_EXPONENT, clipPos.xy);
        else
            shadow = GetShadowFactorCascadedFromPC(0, IN.worldPos, geomN, light);
    }

    float3 lightColor = light.color.rgb * light.intensity;

    // ---------------------------------------------------------------
    // NAIVE water: Fresnel split between the sky reflection and the flat
    // body color, plus the GGX sun glint.
    // ---------------------------------------------------------------
    float3 F0 = float3(kWaterF0, kWaterF0, kWaterF0);
    float3 F = FresnelSchlick(NdotV, F0);

    // Reflection: the analytic sky IBL / probe stack over the wave
    // normal (the POM view's EvaluateSpecularEnv convention, tile index
    // and all). Fresnel-weighted — grazing angles go mirror.
    float3 R = reflect(-V, N);
    uint specTileIdx = 0xffffffffu;
    {
        uint specTs = max(pc.fpTileSize, 1u);
        uint stx = (uint)(clipPos.x / (float)specTs);
        uint sty = (uint)(clipPos.y / (float)specTs);
        if (stx < pc.fpTileCountX)
            specTileIdx = sty * pc.fpTileCountX + stx;
    }
    float3 reflection = EvaluateSpecularEnv(R, F, kWaterGlintRough,
        frame.env, IN.worldPos, frame, specTileIdx,
        pc.fpLightCount, pc.fpMaxPerTile, pc.fpTileCounts, pc.fpTileIndices,
        pc.fpLights, 0.0f);

    // Refraction stand-in: the body color under flat sky irradiance —
    // NO depth-based absorption (the develop-further slot: Beer-Lambert
    // over the scene depth is the obvious next step).
    float3 irradiance = EvaluateIrradiance(N, frame.env);
    float3 bodyColor = e.baseColorLinear;
    float3 refracted = bodyColor * irradiance;

    // Sun glint: Cook-Torrance at the fixed tight roughness (the wave
    // normal carries ALL the sparkle — SpecularAA is deliberately off in
    // the naive preview so the raw lobe is legible while developing).
    float D = DistributionGGX(N, H, kWaterGlintRough);
    float G = GeometrySmith(N, V, L, kWaterGlintRough);
    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-7f);

    float3 color = lerp(refracted, reflection, F)
        + specular * NdotL * lightColor * shadow;

    // Emissive (authored material; the shading consumers own it).
    {
        float3 emissive = mat.emissiveFactor.rgb * mat.params2.z;
        const int emissiveIdx = mat.tex2.x;
        if (emissiveIdx >= 0)
            emissive *= textures[NonUniformResourceIndex((uint) emissiveIdx)]
                .Sample(samplers[0], IN.uv).rgb;
        color += emissive;
    }

    // Fog in HDR (the shaded-view convention; depth cues match F1).
    float viewDist = length(hot.cameraPos - IN.worldPos);
    float fogF = ComputeFogFactor(viewDist, IN.worldPos.y, frame.env);
    if (fogF > 0.0f)
    {
        float3 fogHdr = frame.env.fogColor.rgb * max(frame.env.fogColor.w, 0.0f);
        color = lerp(color, fogHdr, fogF);
    }

    OUT.outColor = float4(SanitizeHdr(color),
        (isBlendMat && ditheredTransp) ? 1.0f : e.alpha);
    return OUT;
}
