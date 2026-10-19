// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "common.hlsli"

// World-space normals debug view (combo/env only - no free F-key; see
// ViewMode::Normals): the interpolated GEOMETRIC normal as false color,
// (n * 0.5 + 0.5) -> RGB (up = green+..., +X red, +Y green, +Z blue).
// Geometry readout only - no material fetch, no normal-map perturbation
// (the POM view owns mapped normals), no cutout/dither: flat readouts
// stay solid on purpose (silhouette agreement is the unlit view's job;
// see default_unlit.frag.hlsl's note). Pairs with the family contract:
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
