// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"
#include "material_evaluation.hlsli"

// Must be defined after common.hlsli symbols
#include "shadows.hlsli"

// NAIVE hair preview (the PBR debugger scaffolding's shading-model test
// bed): Kajiya-Kay anisotropic strand shading (Scheuermann, "Human Hair
// Rendering" — the 2004 classic) over the shared material evaluator.
// NO Marschner R/TT/TRT lobes, no depth-of-fiber tessellation, no
// anti-aliased highlight (the specular AA variance trick), no
// shadow-attenuated root color. Deliberately naive: a development
// surface, not a shipping model.
//
// What it does per pixel:
//   - material inputs via EvaluatePbrMaterial (base color = strand tint)
//   - the STRAND direction from the vertex tangent (the flow direction —
//     the mesh must author tangents along the strands, guarded like the
//     lit body's TBN build)
//   - diffuse: Kajiya-Kay's sqrt(1 - (T.L)^2) mixed with a wrapped
//     Lambert floor so the underside of strands stays readable
//   - specular: the primary lobe sin(T,H)^exp + a secondary lobe over
//     the normal-shifted tangent (the classic dual highlight)
//   - sun + cascaded shadow + sky ambient + fog, real HDR like F1/F11
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

// Kajiya-Kay knobs (demo constants — the development surface).
static const float kHairDiffuseMix   = 0.5f;  // sin(T,L) vs wrapped Lambert
static const float kHairWrap         = 0.35f; // Lambert wrap width
static const float kHairSpecExp1     = 120.0f; // primary lobe tightness
static const float kHairSpecExp2     = 24.0f;  // secondary (shifted) lobe
static const float kHairSpecShift    = 0.15f;  // tangent shift toward N
static const float kHairSpecStrength = 0.35f;  // global spec scale

// sin(theta) between a unit tangent and a unit direction — the Kajiya-Kay
// anisotropy term (theta measured off the STRAND axis, not the normal).
float StrandSin(float3 T, float3 D)
{
    float c = saturate(dot(T, D));
    return sqrt(1.0f - c * c);
}

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    FrameUniforms frame = pc.frameUniforms.Get();
    FramePSHot hot = frame.frameHot.Get();

    // Shared material evaluation (strand tint = base color, the lit
    // body's math — see material_evaluation.hlsli).
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
    // Surface frame: N (guarded + flipped) and the STRAND tangent (the
    // vertex tangent = fiber flow direction; the lit body's degenerate
    // guards apply — a zero tangent is a fallback axis, not a NaN).
    // ---------------------------------------------------------------
    float3 N = normalize(IN.normal);
    if (!(dot(N, N) >= 0.5f))
        N = float3(0.0f, 1.0f, 0.0f);
    if (!IN.isFrontFace)
        N = -N;
    float3 geomN = N;

    float3 T = normalize(IN.tangent.xyz + 1e-5f * N); // degenerate guard
    {
        float3 Tg = T - N * dot(N, T);
        float tgLen = length(Tg);
        if (tgLen > 1e-6f)
            T = Tg / tgLen;
        else {
            float3 axis = abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            T = normalize(cross(axis, N));
        }
    }
    // Secondary-lobe tangent: the primary shifted toward the normal
    // (the classic dual-highlight trick).
    float3 T2 = normalize(T + N * kHairSpecShift);

    // Optional normal-map perturbation of N (the lit body's two-channel
    // discipline; the strand tangent stays flow-aligned regardless).
    {
        float3 Tb = cross(N, T) * ((IN.tangent.w == 0.0f || isnan(IN.tangent.w))
            ? 1.0f : sign(IN.tangent.w));
        const int normalIdx = mat.tex.z;
        if (normalIdx >= 0)
        {
            float2 mapXY = textures[NonUniformResourceIndex((uint) normalIdx)]
                .Sample(samplers[0], IN.uv).xy * 2.0f - 1.0f;
            mapXY *= mat.params2.y;
            float mapZ = sqrt(saturate(1.0f - dot(mapXY, mapXY)));
            float3 mapN = normalize(float3(mapXY, mapZ));
            float3 Nc = T * mapN.x + Tb * mapN.y + N * mapN.z;
            float ncLen = length(Nc);
            if (ncLen > 1e-12f)
                N = Nc / ncLen;
        }
    }

    // ---------------------------------------------------------------
    // Sun + cascaded shadow (the family convention: PCF or EVSM per
    // frame.evsmMode; regular sampler 0).
    // ---------------------------------------------------------------
    Light light = pc.bLights.Get().lights[0];
    float3 L = normalize(-light.direction);
    float3 V = normalize(hot.cameraPos - IN.worldPos);
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0f);

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
    // NAIVE Kajiya-Kay: strand diffuse + the dual anisotropic lobes.
    // ---------------------------------------------------------------
    // Diffuse: sin(T,L) is the fiber-cylinder's projected lit fraction;
    // mixed with a wrapped Lambert floor so gap interiors stay readable.
    float sinTL = StrandSin(T, L);
    float wrapT = saturate((NdotL + kHairWrap) / (1.0f + kHairWrap));
    float diffuseTerm = lerp(wrapT, sinTL, kHairDiffuseMix);
    float3 diffuse = (1.0f - e.metallic) * e.baseColorLinear / PI;

    // Specular: the primary lobe on the strand tangent, the secondary on
    // the shifted twin; both tinted by the strand color's lighter half
    // (colored highlights on dark hair are the Marschner TT tell — the
    // naive proxy lightens the tint instead).
    float sinTH1 = StrandSin(T, H);
    float sinTH2 = StrandSin(T2, H);
    float spec1 = pow(sinTH1, kHairSpecExp1);
    float spec2 = pow(sinTH2, kHairSpecExp2) * 0.5f;
    float3 specTint = lerp(e.baseColorLinear, float3(1.0f, 1.0f, 1.0f), 0.5f);
    float3 specular = (spec1 + spec2) * kHairSpecStrength * specTint;

    float3 direct = (diffuse * diffuseTerm + specular * NdotL) * lightColor * shadow;

    // ---------------------------------------------------------------
    // Ambient: analytic sky irradiance (the POM view's isolated-ambient
    // convention; baked GI stays off on purpose — no root shadowing yet).
    // ---------------------------------------------------------------
    float3 irradiance = EvaluateIrradiance(N, frame.env);
    float3 ambient = diffuse * irradiance * 2.0f;

    float3 color = direct + ambient;

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
