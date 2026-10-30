// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// -------------------------------------------------------------------------
// Shared PBR material evaluator (the PBR Material Validation Debugger's
// Phase-2 "one evaluation site"): the EXACT material-input math of the lit
// body (default_lit_body.hlsli) — layered fetch, per-layer weight blending,
// the <0.004 insignificant-layer skip, glTF G/B metallic-roughness channel
// packing — lifted verbatim so the debugger reports the same effective
// values the lighting path consumes, instead of a disconnected texture
// inspector.
//
// Deliberate DIVERGENCE from the lit body (the point of the debugger):
// EvaluatePbrMaterial keeps the RAW pre-clamp metallic/roughness alongside
// the renderer-effective clamped pair (the lit body clamps roughness into
// [0.04, 1] and saturates metallic — an authored 0.01 roughness texture is
// invisible in the lit output but flags PBR_ROUGHNESS_CLAMPED here).
//
// Consumers (all pair through the DebugFs scene-pc FS family):
//   default_pbr_debug.frag.hlsl   — the six validation/readout views
//   default_sss_debug.frag.hlsl   — naive subsurface-scattering preview
//   default_hair_debug.frag.hlsl  — naive Kajiya-Kay hair preview
//   default_water_debug.frag.hlsl — naive water preview
//
// NOT included here (owned by the shading consumers, matching the split in
// the lit body): normal mapping, occlusion, emissive, decals — the lit
// body's ApplyForwardPlusDecals mutates the composed material AFTER this
// evaluation, so decal-modified values are Phase-2 follow-up work (the
// debugger reports the AUTHORED material today; documented limitation).
//
// Include AFTER common.hlsli (MaterialValues / textures / samplers / pc).
// -------------------------------------------------------------------------

// Validation flag bits (the inspector's error vocabulary; the C++ mirror
// lands with the Phase-3 pixel inspector — the shader owns the canonical
// bit order). ERROR bits mean the input is OUT OF RANGE for the renderer;
// WARNING bits mean suspicious-but-legal values worth an artist's review.
// A green Material Health pixel means "passes the configured checks", NOT
// "physically correct" — the renderer cannot know whether a surface was
// SUPPOSED to be copper or concrete without material classification.
#define PBR_VALID                (0u)
#define PBR_INVALID_BASECOLOR    (1u << 0)  // any channel outside [0,1]
#define PBR_INVALID_METALLIC     (1u << 1)  // raw resolved metallic outside [0,1]
#define PBR_INVALID_ROUGHNESS    (1u << 2)  // raw resolved roughness outside [0,1]
#define PBR_NONFINITE_INPUT      (1u << 3)  // NaN / Inf anywhere in the chain
#define PBR_ALBEDO_WARNING       (1u << 4)  // dielectric luminance < 0.02 / > 0.80
#define PBR_MIXED_METAL_WARNING  (1u << 5)  // metallic in the (0.05, 0.95) transition
#define PBR_ROUGHNESS_CLAMPED    (1u << 6)  // authored roughness below the 0.04 floor

// Artist-facing validation thresholds. Deliberately conservative PROJECT
// DEFAULTS (tunable later via push/uniform when the inspector lands) —
// these are heuristics worth reviewing, not universal physical laws.
#define PBR_ALBEDO_LUMA_MIN  0.02f  // near-black dielectric (charcoal legality edge)
#define PBR_ALBEDO_LUMA_MAX  0.80f  // near-white dielectric (snow legality edge)
#define PBR_METAL_DIELECTRIC 0.05f  // <= this reads as a dielectric
#define PBR_METAL_FULL       0.95f  // >= this reads as a metal
#define PBR_ROUGHNESS_FLOOR  0.04f  // the renderer's own clamp (lit body)

// The one-evaluation-site result shared by every debugger consumer.
struct EvaluatedPbrMaterial
{
    // Effective (renderer-convention) values — what the lit path shades.
    float3 baseColorLinear;      // factor * vertexTint * texture (glTF chain)
    float3 diffuseLinear;        // baseColor * (1 - metallic) — the METALLIC
                                 // workflow's diffuse material input (NOT the
                                 // final BRDF diffuse term: no Fresnel, no 1/PI)
    float3 F0;                   // lerp(0.04, baseColor, metallic)
    float  alpha;                // factor.a * texture.a (pre-cutout, pre-dither)
    float  metallic;             // saturate(raw) — the lit body's effective value
    float  perceptualRoughness;  // clamp(raw, 0.04, 1) — the lit body's value

    // Raw authored values BEFORE the renderer's clamps (the debugger's
    // whole reason to exist: which texture produced a clamped input).
    float  rawMetallic;
    float  rawRoughness;

    // PBR_* validation bits (errors | warnings; PBR_VALID = 0).
    uint   validationFlags;

    // Layered-material state (consumers downstream may want it: layer 0
    // is the draw's own material; w = layer mode 0..4, see MaterialValues).
    bool   layered;
    float4 layerWeights;         // normalized vertex-color RGBA weights
};

// Bridge over the FS input: every consumer FS declares the SAME locations
// (the scene-pc FS family shape — see default_unlit.frag.hlsl), but HLSL
// has no structural typing between PSIn declarations, so the evaluator
// takes the three fields it needs through this tiny carrier.
struct PSInBridge
{
    float4 color; // COLOR0 — layer weights (layered) or albedo tint (plain)
    float2 uv;    // TEXCOORD0
};

// Layered-material fetch (the lit body's convention verbatim): layer 0 is
// this material; layers 1..3 come from layerMaterials.xyz (-1 = reuse
// layer 0); vertex color RGBA are the weights (normalized); enum-like
// state (shading model, alpha mode, cutout) always comes from layer 0.
static void PbrDebugFetchLayers(int materialId, PSInBridge IN,
    out MaterialValues ml0, out MaterialValues ml1,
    out MaterialValues ml2, out MaterialValues ml3,
    out bool layered, out float4 layerW)
{
    MaterialValues mat = pc.bMats.Get().mats[materialId];
    int layerMode = mat.layerMaterials.w;
    layered = (layerMode > 1);
    layerW = layered
        ? IN.color / max(IN.color.x + IN.color.y + IN.color.z + IN.color.w, 1e-6f)
        : float4(1.0f, 0.0f, 0.0f, 0.0f);
    ml0 = mat;
    ml1 = mat;
    ml2 = mat;
    ml3 = mat;
    if (layered)
    {
        int id1 = mat.layerMaterials.x;
        int id2 = mat.layerMaterials.y;
        int id3 = mat.layerMaterials.z;
        if (layerMode > 1 && id1 >= 0 && id1 < MAX_MATERIALS)
            ml1 = pc.bMats.Get().mats[id1];
        if (layerMode > 2 && id2 >= 0 && id2 < MAX_MATERIALS)
            ml2 = pc.bMats.Get().mats[id2];
        if (layerMode > 3 && id3 >= 0 && id3 < MAX_MATERIALS)
            ml3 = pc.bMats.Get().mats[id3];
    }
}

// Shared evaluator (see the header note): mirrors default_lit_body's
// material-input math, keeps raw + clamped values, packs validation bits.
EvaluatedPbrMaterial EvaluatePbrMaterial(int materialId, PSInBridge IN)
{
    EvaluatedPbrMaterial e;

    MaterialValues ml0, ml1, ml2, ml3;
    bool layered;
    float4 layerW;
    PbrDebugFetchLayers(materialId, IN, ml0, ml1, ml2, ml3, layered, layerW);
    e.layered = layered;
    e.layerWeights = layerW;

    // ---------------------------------------------------------------
    // Base color / alpha (layered: per-layer factors + samples blend by
    // weight, the <0.004 skip matching the lit body's fetch discipline).
    // ---------------------------------------------------------------
    float3 baseColorFactor = layered
        ? ml0.diffuse.rgb * layerW.x + ml1.diffuse.rgb * layerW.y
        + ml2.diffuse.rgb * layerW.z + ml3.diffuse.rgb * layerW.w
        : ml0.diffuse.rgb;
    float baseAlphaFactor = layered
        ? ml0.diffuse.a * layerW.x + ml1.diffuse.a * layerW.y
        + ml2.diffuse.a * layerW.z + ml3.diffuse.a * layerW.w
        : ml0.diffuse.a;

    float4 baseSample = float4(1, 1, 1, 1);
    if (!layered && ml0.tex.x >= 0)
    {
        baseSample = textures[NonUniformResourceIndex((uint) ml0.tex.x)]
            .Sample(samplers[0], IN.uv);
    }
    if (layered)
    {
        baseSample = float4(0, 0, 0, 0);
        int4 bcIdx = int4(ml0.tex.x, ml1.tex.x, ml2.tex.x, ml3.tex.x);
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y
                : (li == 2) ? layerW.z : layerW.w;
            int idx = (li == 0) ? bcIdx.x : (li == 1) ? bcIdx.y
                : (li == 2) ? bcIdx.z : bcIdx.w;
            if (w < 0.004f)
                continue; // below RGBA8 quantization: skips the fetch (lit body)
            float4 s = float4(1, 1, 1, 1);
            if (idx >= 0)
                s = textures[NonUniformResourceIndex((uint) idx)]
                    .Sample(samplers[0], IN.uv);
            baseSample += s * w;
        }
    }

    // glTF chain: baseColor = factor * vertexColor * texture (layered:
    // the vertex color is consumed as weights, tint stays white).
    float3 vcolTint = layered ? float3(1.0f, 1.0f, 1.0f) : IN.color.rgb;
    e.baseColorLinear = baseColorFactor * vcolTint * baseSample.rgb;
    e.alpha = baseAlphaFactor * baseSample.a;

    // ---------------------------------------------------------------
    // Metallic-roughness (glTF packing: G = roughness, B = metallic;
    // layered: per-layer factors AND samples blend by weight — the lit
    // body's (Sum w*f) * (Sum w*s) form).
    // ---------------------------------------------------------------
    float metallicFactor = layered
        ? ml0.params.y * layerW.x + ml1.params.y * layerW.y
        + ml2.params.y * layerW.z + ml3.params.y * layerW.w
        : ml0.params.y;
    float roughnessFactor = layered
        ? ml0.params.z * layerW.x + ml1.params.z * layerW.y
        + ml2.params.z * layerW.z + ml3.params.z * layerW.w
        : ml0.params.z;

    float roughMult = 1.0f;
    float metalMult = 1.0f;
    if (!layered && ml0.tex.y >= 0)
    {
        float4 mrSample = textures[NonUniformResourceIndex((uint) ml0.tex.y)]
            .Sample(samplers[0], IN.uv);
        roughMult = mrSample.g;
        metalMult = mrSample.b;
    }
    if (layered)
    {
        roughMult = 0.0f;
        metalMult = 0.0f;
        int4 mIdx = int4(ml0.tex.y, ml1.tex.y, ml2.tex.y, ml3.tex.y);
        [unroll]
        for (int li = 0; li < 4; ++li)
        {
            float w = (li == 0) ? layerW.x : (li == 1) ? layerW.y
                : (li == 2) ? layerW.z : layerW.w;
            int idx = (li == 0) ? mIdx.x : (li == 1) ? mIdx.y
                : (li == 2) ? mIdx.z : mIdx.w;
            if (w < 0.004f)
                continue;
            float2 gb = float2(1.0f, 1.0f);
            if (idx >= 0)
                gb = textures[NonUniformResourceIndex((uint) idx)]
                    .Sample(samplers[0], IN.uv).gb;
            roughMult += gb.x * w;
            metalMult += gb.y * w;
        }
    }

    // RAW resolved values BEFORE the renderer's clamps.
    e.rawRoughness = roughnessFactor * roughMult;
    e.rawMetallic = metallicFactor * metalMult;

    // The lit body's effective clamps (mirrored, not invented).
    e.metallic = saturate(e.rawMetallic);
    e.perceptualRoughness = clamp(e.rawRoughness, PBR_ROUGHNESS_FLOOR, 1.0f);

    // Metallic workflow derivations (lit body conventions).
    e.diffuseLinear = e.baseColorLinear * (1.0f - e.metallic);
    e.F0 = lerp(float3(0.04f, 0.04f, 0.04f), e.baseColorLinear, e.metallic);

    // ---------------------------------------------------------------
    // Validation. Errors: the resolved input is outside what the renderer
    // considers legal (range / finiteness). Warnings: legal but suspicious
    // for a dielectric/metal classification. Non-finite is checked FIRST
    // (NaN comparisons are false — every range test below a NaN must not
    // accidentally pass it through).
    // ---------------------------------------------------------------
    e.validationFlags = PBR_VALID;

    const bool finiteInput =
        all(isfinite(e.baseColorLinear))
        && isfinite(e.rawMetallic) && isfinite(e.rawRoughness)
        && isfinite(e.alpha);
    if (!finiteInput)
        e.validationFlags |= PBR_NONFINITE_INPUT;

    if (any(e.baseColorLinear < 0.0f) || any(e.baseColorLinear > 1.0f))
        e.validationFlags |= PBR_INVALID_BASECOLOR;
    if (!(e.rawMetallic >= 0.0f && e.rawMetallic <= 1.0f))
        e.validationFlags |= PBR_INVALID_METALLIC;
    if (!(e.rawRoughness >= 0.0f && e.rawRoughness <= 1.0f))
        e.validationFlags |= PBR_INVALID_ROUGHNESS;

    const bool dielectric = (e.metallic <= PBR_METAL_DIELECTRIC);
    const bool intermediateMetal =
        (e.metallic > PBR_METAL_DIELECTRIC && e.metallic < PBR_METAL_FULL);
    if (intermediateMetal)
        e.validationFlags |= PBR_MIXED_METAL_WARNING;

    if (dielectric)
    {
        const float luma = dot(e.baseColorLinear,
            float3(0.2126f, 0.7152f, 0.0722f));
        if (luma < PBR_ALBEDO_LUMA_MIN || luma > PBR_ALBEDO_LUMA_MAX)
            e.validationFlags |= PBR_ALBEDO_WARNING;
    }

    // The renderer clamps authored roughness below 0.04 to 0.04 (the lit
    // body's singularity guard): the FINAL image cannot show which
    // textures authored smaller values — this bit is exactly that signal.
    if (e.rawRoughness < PBR_ROUGHNESS_FLOOR)
        e.validationFlags |= PBR_ROUGHNESS_CLAMPED;

    return e;
}

// True when any ERROR bit is set (the Material Health red class).
bool PbrHasError(uint flags)
{
    return (flags & (PBR_INVALID_BASECOLOR | PBR_INVALID_METALLIC
        | PBR_INVALID_ROUGHNESS | PBR_NONFINITE_INPUT)) != 0u;
}

// True when any WARNING bit is set (the Material Health yellow class).
bool PbrHasWarning(uint flags)
{
    return (flags & (PBR_ALBEDO_WARNING | PBR_MIXED_METAL_WARNING
        | PBR_ROUGHNESS_CLAMPED)) != 0u;
}
