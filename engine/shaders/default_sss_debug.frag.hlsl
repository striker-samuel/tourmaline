// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"
#include "material_evaluation.hlsli"

// Must be defined after common.hlsli symbols
#include "shadows.hlsli"

// NAIVE subsurface-scattering preview (the PBR debugger scaffolding's
// shading-model test bed): the classic single-layer transmission hack
// (d'Eon, "Approximating Translucency", GPU Gems 3 ch. 16) over the
// shared material evaluator — NO screen-space diffusion, NO depth-based
// thickness, NO separable profile. Deliberately naive: this exists to
// develop against (owner: "Ill develop them further"), not to ship.
//
// What it does per pixel:
//   - material inputs via EvaluatePbrMaterial (the lit body's math)
//   - normal mapping (the lit body's TBN + two-channel map discipline)
//   - THICKNESS proxy: the occlusion map as thickness (the POM view's
//     AO-as-height pairing — cavities read thick, open surfaces thin;
//     0.5 flat when the material has no map)
//   - direct sun: foliage-style wrap diffuse (the lit body's MASK wrap)
//     PLUS a view-dependent transmission lobe pow(dot(V', -L), power)
//     scaled by thickness, tinted by a saturation-boosted base color
//   - ambient sky irradiance + occlusion + fog, real HDR like F1/F11
//
// Pairs with default.vert.hlsl / gfxfiltered.vert.hlsl (the scene-pc FS
// family PSIn). Alpha cutout + dithered Blend + dither-on-closeup match
// the lit path so silhouettes agree.

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

// Transmission knobs (demo constants — the development surface).
static const float kSssDistort    = 0.35f; // N pull into the light for V'
static const float kSssPower      = 2.0f;  // lobe tightness (higher = tighter rim)
static const float kSssScale      = 1.2f;  // transmission strength
static const float kSssWrapDiffuse = 0.5f; // wrap width (lit body's foliage value)

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    FrameUniforms frame = pc.frameUniforms.Get();
    FramePSHot hot = frame.frameHot.Get();

    // Shared material evaluation (layered + glTF packing, the lit body's
    // math — see material_evaluation.hlsli).
    PSInBridge bridge = { IN.color, IN.uv };
    EvaluatedPbrMaterial e = EvaluatePbrMaterial(pc.materialId, bridge);

    const float alphaCutoff = mat.params.x;
    const float shadingModelF = mat.params.w;
    const float alphaModeF = mat.params2.w;
    const bool isBlendMat = (alphaModeF == 2.0f);
    const bool ditheredTransp = (pc.pad_pc == 0);

    // Lit-path visibility decisions (same order as the unlit/PBR family).
    if (IN.ditherCloseup != 0u
        && !CloseupDitherKeep(IN.worldPos, hot.cameraPos, clipPos.xy))
        discard;
    if (shadingModelF == 2.0f && e.alpha < alphaCutoff)
        discard;
    if (isBlendMat && ditheredTransp && !DitherKeep(e.alpha, clipPos.xy))
        discard;

    // ---------------------------------------------------------------
    // Normal + TBN (the lit body's guarded build: degenerate-interpolation
    // NaN guard, Gram-Schmidt, handedness sanitize, two-channel maps).
    // ---------------------------------------------------------------
    float3 N = normalize(IN.normal);
    if (!(dot(N, N) >= 0.5f))
        N = float3(0.0f, 1.0f, 0.0f);
    if (!IN.isFrontFace)
        N = -N;
    float3 geomN = N;

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
        mapXY *= mat.params2.y; // glTF normalTexture.scale on XY
        float mapZ = sqrt(saturate(1.0f - dot(mapXY, mapXY)));
        float3 mapN = normalize(float3(mapXY, mapZ));
        float3 Nc = T * mapN.x + B * mapN.y + N * mapN.z;
        float ncLen = length(Nc);
        if (ncLen > 1e-12f)
            N = Nc / ncLen;
    }

    // Thickness proxy (the POM view's AO-as-height pairing): cavity =
    // thick, open surface = thin. Flat 0.5 when the material has no map.
    float thickness = 0.5f;
    const int occIdx = mat.tex.w;
    if (occIdx >= 0)
    {
        float ao = textures[NonUniformResourceIndex((uint) occIdx)]
            .Sample(samplers[0], IN.uv).r;
        thickness = lerp(1.0f, ao, mat.params2.x); // occlusion_strength blend
        thickness = 1.0f - thickness * 0.5f; // AO 0 -> thick 1, AO 1 -> thin 0.5
    }

    // ---------------------------------------------------------------
    // Sun + cascaded shadow (POM's convention: PCF depth atlas or EVSM
    // moments per frame.evsmMode; regular sampler 0).
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
    // NAIVE SSS: wrap diffuse + the view-dependent transmission lobe.
    // ---------------------------------------------------------------
    // Wrap diffuse (the lit body's MASK foliage wrap, applied to every
    // material in this preview — the SSS test bed's whole point).
    float t = saturate((dot(N, L) + kSssWrapDiffuse) / (1.0f + kSssWrapDiffuse));
    float NdotLWrap = t * t;
    float3 diffuse = (1.0f - e.metallic) * e.baseColorLinear / PI;

    // Transmission lobe: light entering the FAR side, exiting toward the
    // eye. V' pulls the view vector toward the light by the distortion so
    // the lobe blooms past the terminator; thickness scales the magnitude.
    float3 Vp = normalize(V + N * kSssDistort);
    float backDot = pow(saturate(dot(Vp, -L)), kSssPower);
    // Tint: base color pushed toward saturation (skin/wax transmission is
    // more chromatic than the surface — the naive constant-free proxy).
    float tLuma = dot(e.baseColorLinear, float3(0.2126f, 0.7152f, 0.0722f));
    float3 transTint = saturate(lerp(tLuma.xxx, e.baseColorLinear, 1.5f));
    float3 transmission = transTint * backDot * kSssScale * thickness * shadow;

    float3 direct = (diffuse * NdotLWrap + transmission) * lightColor;

    // ---------------------------------------------------------------
    // Ambient: analytic sky irradiance + occlusion (the POM view's
    // isolated-ambient convention; baked GI stays off on purpose).
    // ---------------------------------------------------------------
    float3 irradiance = EvaluateIrradiance(N, frame.env);
    float occ = 1.0f;
    if (occIdx >= 0)
    {
        float aoS = textures[NonUniformResourceIndex((uint) occIdx)]
            .Sample(samplers[0], IN.uv).r;
        occ = lerp(1.0f, aoS, mat.params2.x);
    }
    float3 ambient = diffuse * irradiance * occ * 2.0f;

    float3 color = direct + ambient;

    // Emissive (material authored, the evaluator skips it — the shading
    // consumers own it, matching the lit body's split).
    {
        float3 emissive = mat.emissiveFactor.rgb * mat.params2.z;
        const int emissiveIdx = mat.tex2.x;
        if (emissiveIdx >= 0)
            emissive *= textures[NonUniformResourceIndex((uint) emissiveIdx)]
                .Sample(samplers[0], IN.uv).rgb;
        color += emissive;
    }

    // Fog in HDR, same as the lit path (this is a SHADED view, not a flat
    // readout — the POM convention, depth cues match F1).
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
