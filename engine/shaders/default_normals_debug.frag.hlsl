// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "common.hlsli"

// World-space normals debug view (combo/env only - no free F-key; see
// ViewMode::Normals): the interpolated GEOMETRIC normal as false color,
// (n * 0.5 + 0.5) -> RGB (up = green+..., +X red, +Y green, +Z blue).
// Geometry readout only - no normal-map perturbation (the POM view owns
// mapped normals). MASK cutout + closeup dither DO run here (mirrored
// from default_unlit.frag.hlsl, the family's silhouette keeper): masked
// leaf cards otherwise draw their transparent texels as solid false
// color, which reads as flicker/overlapping polygons on authored masked
// geometry that lit and unlit discard. Pairs with the family contract:
// same PSIn as default_unlit over default.vert (direct fallback leg) and
// gfxfiltered.vert (GPU-driven leg) - see AppResources::debugPrograms.
//
// Debug colors travel through HDR + tonemap (hdr * exposure -> ACES) with
// a physical-units exposure (~0.001) that would crush a 0..1 readout to
// black, so the output is pre-scaled by 1/exposure (the family trick;
// ACES still shapes hues slightly). NO fog: debug views stay unfogged by
// discipline (flat readouts must not be Beer-Lamberted - see
// DebugVisualizationActive, which also keeps the volumetric/SSR composites
// off these pixels).

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    // ALL inputs carry EXPLICIT locations (DXC rejects partial explicit
    // assignment; see default_unlit.frag.hlsl's note). The locations the
    // FS does not consume (lightmapUv 5 / lmChunk 6) stay undeclared -
    // decorations of the declared set must match the VS exactly.
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

    // Match the lit path's closeup dissolve (default_unlit's block,
    // verbatim): a flagged subtree must screen-door away here too or it
    // renders SOLID in this view while it dissolves in lit.
    if (IN.ditherCloseup != 0u)
    {
        FrameUniforms frameEarly = pc.frameUniforms.Get();
        FramePSHot hotEarly = frameEarly.frameHot.Get();

        if (!CloseupDitherKeep(
                IN.worldPos, hotEarly.cameraPos, clipPos.xy))
            discard;
    }

    // Single-layer MASK material (matches lit/unlit): discard below the
    // alpha cutoff so masked cutouts keep their authored silhouette
    // instead of drawing transparent texels as false color.
    if (mat.params.w == 2.0f)
    {
        float alpha = mat.diffuse.a;
        int baseColorIdx = mat.tex.x;

        if (baseColorIdx >= 0)
        {
            alpha *= textures[
                NonUniformResourceIndex((uint)baseColorIdx)]
                .Sample(samplers[0], IN.uv).a;
        }

        if (alpha < mat.params.x)
            discard;
    }

    // Interpolated world-space geometric normal -> false color. The
    // pipeline is always back-face cull, so isFrontFace is constant true
    // here (declared for interface parity with the family, never read).
    float3 n = normalize(IN.normal);
    float3 color = n * 0.5f + 0.5f;

    // Exposure compensation (readout -> HDR): pre-scale by 1/exposure so
    // the 0..1 false color survives the hdr * exposure -> ACES path.
    FrameUniforms frame = pc.frameUniforms.Get();
    color /= max(frame.exposure, 1e-6f);

    OUT.outColor = float4(SanitizeHdr(color), 1.0f);
    return OUT;
}
