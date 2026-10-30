// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "common.hlsli"
#include "material_evaluation.hlsli"

// PBR Material Validation Debugger (the Alan Wake 2 environment-art
// workflow): six flat readouts over the EXACT material inputs the lit
// path consumes (the shared evaluator — see material_evaluation.hlsli).
// One source, six compiled variants via PBR_MODE:
//
//   0 = Material Health     (base compile, no define needed)
//   1 = Base Color          (.base)   raw material color, no lighting
//   2 = Diffuse Reflectance (.diffuse) dielectric diffuse base + warnings
//   3 = Roughness           (.rough)  per-pixel perceptual roughness
//   4 = Metalness           (.metal)  dielectric / metal / transition
//   5 = F0 / Specular       (.f0)     normal-incidence specular
//
// Material Health palette (green = passes the CONFIGURED checks, not
// "guaranteed physically accurate" — the renderer cannot know whether a
// surface was supposed to be copper or concrete without classification):
//   GREEN  no validation issue detected
//   YELLOW suspicious values worth reviewing (see the PBR_*_WARNING bits)
//   RED    invalid input / severe material issue (the PBR error bits)
//   MAGENTA non-finite data (NaN/Inf) — visible in EVERY mode so a broken
//           texture cannot hide behind a plausible-looking readout
//   BLUE   (metalness view) the informational intermediate-metal marking
//
// Pairs with default.vert.hlsl / gfxfiltered.vert.hlsl (same PSIn shape
// as default_unlit — the scene-pc FS family). Alpha cutout + dithered
// Blend discard + dither-on-closeup match the lit path so silhouettes
// agree with F1 while the readout stays authored data.
//
// Debug colors travel through HDR + tonemap (hdr * exposure -> ACES) with
// a physical-units exposure (~0.001) that would crush albedo to black, so
// the output is pre-scaled by 1/exposure (the unlit/meshlet convention).
// NO fog: flat readouts never Beer-Lambert (DebugVisualizationActive also
// keeps the volumetric/SSR composites off these pixels). The 1/exposure
// pre-scale means the apparent intensity is a DISPLAY, not a numeric
// readout — the Phase-3 pixel inspector owns exact values.

#ifndef PBR_MODE
#define PBR_MODE 0
#endif

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    // Dither-on-closeup flag (flat, per-draw; see common.hlsli
    // CloseupDitherKeep + default_unlit's note): EXPLICIT locations on
    // every input (DXC rejects partial explicit assignment; a decoration
    // mismatch on this slot once made unflagged meshes dissolve).
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

    // The one shared evaluation site: raw + renderer-effective values +
    // validation bits, identical math to the lit body (layered blends,
    // the <0.004 skip, glTF G/B packing).
    PSInBridge bridge = { IN.color, IN.uv };
    EvaluatedPbrMaterial e = EvaluatePbrMaterial(pc.materialId, bridge);

    // Enum-like state from layer 0 (layered convention: factors blend,
    // state does not).
    const float alphaCutoff = mat.params.x;
    const float shadingModelF = mat.params.w;
    const float alphaModeF = mat.params2.w;
    const bool isBlendMat = (alphaModeF == 2.0f);
    const bool ditheredTransp = (pc.pad_pc == 0);

    // Preserve the lit path's visibility decisions so the readout covers
    // exactly the pixels F1 shades (same order as default_unlit).
    if (IN.ditherCloseup != 0u)
    {
        FrameUniforms frameEarly = pc.frameUniforms.Get();
        FramePSHot hotEarly = frameEarly.frameHot.Get();
        if (!CloseupDitherKeep(IN.worldPos, hotEarly.cameraPos, clipPos.xy))
            discard;
    }
    if (shadingModelF == 2.0f && e.alpha < alphaCutoff)
        discard; // Masked cutout (matches lit)
    if (isBlendMat && ditheredTransp && !DitherKeep(e.alpha, clipPos.xy))
        discard; // Dithered transparency for Blend (matches lit)

    const bool invalid = PbrHasError(e.validationFlags);
    const bool warning = PbrHasWarning(e.validationFlags);

    float3 debugColor;

#if PBR_MODE == 0
    // -------------------------------------------------------------
    // Material Health: the combined validation heatmap.
    // -------------------------------------------------------------
    debugColor = invalid
        ? float3(1.0f, 0.0f, 0.15f)   // RED — invalid input
        : warning
            ? float3(1.0f, 0.72f, 0.0f) // YELLOW — suspicious, review
            : float3(0.05f, 0.70f, 0.14); // GREEN — passes the checks

#elif PBR_MODE == 1
    // -------------------------------------------------------------
    // Base Color: the raw linear material color (factor * vertex tint *
    // texture), independent of lighting. What the artist authored.
    // -------------------------------------------------------------
    debugColor = e.baseColorLinear;

#elif PBR_MODE == 2
    // -------------------------------------------------------------
    // Diffuse Reflectance: the metallic workflow's diffuse material
    // input baseColor * (1 - metallic) — NOT the final BRDF diffuse term
    // (no Fresnel complement, no 1/PI, no lighting). Fully metallic
    // surfaces go black here BY DESIGN: their base color describes the
    // colored specular response, not diffuse reflection (see the F0 view).
    // -------------------------------------------------------------
    debugColor = e.diffuseLinear;

#elif PBR_MODE == 3
    // -------------------------------------------------------------
    // Roughness: the RENDERER-EFFECTIVE perceptual roughness (post the
    // 0.04 floor clamp). Authored values below the floor flag YELLOW in
    // the Health view — this view shows what the clamp produced.
    // -------------------------------------------------------------
    debugColor = e.perceptualRoughness.xxx;

#elif PBR_MODE == 4
    // -------------------------------------------------------------
    // Metalness: grayscale metallic with the BLUE informational marking
    // over the (0.05, 0.95) transition region — the antialiased
    // metal/dielectric boundary and material-layer blends land here and
    // are usually legitimate (the Health view still flags them yellow
    // until boundary exemption lands).
    // -------------------------------------------------------------
    {
        const bool intermediate =
            (e.metallic > PBR_METAL_DIELECTRIC && e.metallic < PBR_METAL_FULL);
        debugColor = e.metallic.xxx;
        if (intermediate)
            debugColor = lerp(debugColor, float3(0.15f, 0.35f, 1.0f), 0.65f);
    }

#elif PBR_MODE == 5
    // -------------------------------------------------------------
    // F0 / Specular: the normal-incidence specular reflectance,
    // lerp(0.04, baseColor, metallic) — dielectrics read as the flat
    // 0.04 gray unless the base color is out of range, metals read as
    // their tinted specular.
    // -------------------------------------------------------------
    debugColor = e.F0;

#else
    // Unknown PBR_MODE: unreachable (CMake pins every variant); keep the
    // health heatmap so a hand-rolled compile still shows something sane.
    debugColor = invalid
        ? float3(1.0f, 0.0f, 0.15f)
        : warning
            ? float3(1.0f, 0.72f, 0.0f)
            : float3(0.05f, 0.70f, 0.14);
#endif

    // Invalid marking (stays visible in EVERY mode so a broken texture
    // cannot hide behind a plausible-looking readout): the Health heatmap
    // keeps its palette — red for out-of-range, MAGENTA reserved for
    // non-finite so "the data is broken" stays distinguishable from "the
    // data is out of range" in the one view whose job is the difference.
    // The value views mark ANY invalid input magenta (a base color of
    // 1.4 would tonemap bright and read as legal).
#if PBR_MODE == 0
    if ((e.validationFlags & PBR_NONFINITE_INPUT) != 0u)
        debugColor = float3(1.0f, 0.0f, 1.0f);
#else
    if (invalid)
        debugColor = float3(1.0f, 0.0f, 1.0f);
#endif

    // Exposure compensation (albedo-domain readouts -> HDR; the unlit
    // convention). ACES still shapes the tonemap — hues stay distinct,
    // the numeric value does not survive (see the header note).
    FrameUniforms frame = pc.frameUniforms.Get();
    float exposure = max(frame.exposure, 1e-6f);
    debugColor /= exposure;

    OUT.outColor = float4(SanitizeHdr(debugColor),
        (isBlendMat && ditheredTransp) ? 1.0f : e.alpha);
    return OUT;
}
