// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Flat UV coordinate export: the mesh unwrapped by its atlas UV
// (chart-local TEXCOORD1 + chart id, unpacked in the VS via the per-mesh
// chart tail) with the PARAMETERIZATION ITSELF as content — no atlas
// sampling (unlike uvflat1.frag, this needs no loaded .sglm). Same QA
// language as the F10 UV1 debug view (default_uv_debug.hlsli):
// checker + uv gradient + derivative stretch glow + out-of-range
// striping, so chart-island layout, gutter bleed and seam-corner
// streaks read directly off the PNG. LDR direct (the export target is
// sRGB, like the swapchain).
#include "common.hlsli"

struct PSIn
{
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 tangent : TANGENT;
    float2 lightmapUv : TEXCOORD3;
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;
    const float2 dbgUv = IN.lightmapUv;

    // Checker + gradient + grid lines (atlas-normalized space: 256
    // cells land ~6 texels on a 1600-wide atlas — chart granularity).
    const float2 cell = dbgUv * 256.0f;
    const float2 fl = floor(cell);
    const float checker = (fmod(fl.x + fl.y, 2.0f) < 1.0f) ? 1.0f : 0.0f;
    float3 color = lerp(float3(0.16f, 0.18f, 0.22f),
        float3(0.72f, 0.74f, 0.78f), checker);
    color += float3(saturate(dbgUv.x), saturate(dbgUv.y), 0.12f) * 0.28f;
    const float2 grid = abs(frac(cell) - 0.5f)
        / max(fwidth(cell), float2(1e-4f, 1e-4f));
    const float gridLine = 1.0f - saturate(min(grid.x, grid.y));
    color += gridLine * 0.30f;

    // Derivative glow: triangles whose uv spans a huge atlas distance
    // per pixel (seam corners interpolating across chart rects) show up
    // as magenta streaks.
    const float2 duvx = ddx(dbgUv);
    const float2 duvy = ddy(dbgUv);
    const float stretch = saturate((abs(duvx.x) + abs(duvx.y)
        + abs(duvy.x) + abs(duvy.y)) * 2.0f);
    color = lerp(color, float3(1.0f, 0.10f, 1.0f), stretch);

    // Atlas-space uv2 must stay in [0,1]: stripe out-of-range pixels.
    if (dbgUv.x < 0.0f || dbgUv.x > 1.0f || dbgUv.y < 0.0f || dbgUv.y > 1.0f)
    {
        const float oob = fmod(floor(clipPos.x * 0.25f)
            + floor(clipPos.y * 0.25f), 2.0f) < 1.0f ? 1.0f : 0.0f;
        color = oob ? float3(1.0f, 0.05f, 1.0f) : float3(0.02f, 0.02f, 0.02f);
    }

    OUT.outColor = float4(SanitizeHdr(color), 1.0);
    return OUT;
}
