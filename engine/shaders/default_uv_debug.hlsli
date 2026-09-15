// UV debug view body, shared by default_uv0_debug.frag.hlsl (base UV,
// TEXCOORD0) and default_uv1_debug.frag.hlsl (lightmap atlas UV, uv2).
// UVDBG_CHANNEL must be defined by the including entry file.
//
// Displays the selected UV stream as a 16x16 checker + gradient tint so
// continuity, orientation, tiling and (for uv2) chart-island layout read
// at a glance:
//   - checker + red/green uv gradient: base pattern;
//   - thin grid lines: cell borders (fwidth AA);
//   - magenta glow: |duv| screen-derivative — a triangle sweeping a huge
//     uv extent (e.g. a lightmap seam corner interpolated across two
//     atlas rects) lights up as a neon streak even where the checker
//     aliases into a plausible pattern;
//   - channel 1 only: uv outside [0,1] stripes magenta/black (atlas-space
//     uv2 is UNORM-packed and must stay in range).
// Pairs with default.vert.hlsl (same PSIn as default_lit). Alpha cutout +
// dithered Blend discard match the lit path so silhouettes agree.
//
// Like the other debug views the output is pre-scaled by 1/exposure: it
// travels through HDR + tonemap and a raw 0..1 color would crush to
// black under physical-units exposure. No fog on purpose.

#include "common.hlsli"

#ifndef UVDBG_CHANNEL
#define UVDBG_CHANNEL 0
#endif

struct PSIn
{
    float4 clipPos : SV_Position;
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

PSOut mainPS(PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];

#if UVDBG_CHANNEL == 1
    const float2 dbgUv = IN.lightmapUv;
#else
    const float2 dbgUv = IN.uv;
#endif

    // Alpha cutout so holes match the lit view (color itself is debug).
    float alphaCutoff = mat.params.x;
    float shadingModelF = mat.params.w;
    float baseAlpha = mat.diffuse.a;
    int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        baseAlpha *= baseTex.Sample(samplers[0], IN.uv).a;
        TS_ReportUsage(slot, baseTex, samplers[0], IN.uv, IN.clipPos.xy);
    }
    if (shadingModelF == 2.0f && baseAlpha < alphaCutoff)
        discard;

    float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (isBlendMat && ditheredTransp && !DitherKeep(baseAlpha, IN.clipPos.xy))
        discard;

    // Checker + gradient + grid lines.
    const float2 cell = dbgUv * 16.0f;
    const float2 fl = floor(cell);
    const float checker = (fmod(fl.x + fl.y, 2.0f) < 1.0f) ? 1.0f : 0.0f;
    float3 color = lerp(float3(0.16f, 0.18f, 0.22f),
        float3(0.72f, 0.74f, 0.78f), checker);
    color += float3(saturate(dbgUv.x), saturate(dbgUv.y), 0.12f) * 0.28f;
    const float2 grid = abs(frac(cell) - 0.5f)
        / max(fwidth(cell), float2(1e-4f, 1e-4f));
    const float gridLine = 1.0f - saturate(min(grid.x, grid.y));
    color += gridLine * 0.30f;

    // Derivative glow: triangles whose uv spans a huge atlas distance per
    // pixel (lightmap seam corners interpolating across chart rects) show
    // up as magenta streaks. Channel 0 tilings legitimately vary, so its
    // threshold is looser than the atlas-space channel.
    const float2 duvx = ddx(dbgUv);
    const float2 duvy = ddy(dbgUv);
    const float kStretch = (UVDBG_CHANNEL == 1) ? 2.0f : 8.0f;
    const float stretch = saturate((abs(duvx.x) + abs(duvx.y)
        + abs(duvy.x) + abs(duvy.y)) * kStretch);
    color = lerp(color, float3(1.0f, 0.10f, 1.0f), stretch);

    // Channel 1 is atlas-space UNORM: out-of-range uv2 is a bug, stripe it.
#if UVDBG_CHANNEL == 1
    if (dbgUv.x < 0.0f || dbgUv.x > 1.0f || dbgUv.y < 0.0f || dbgUv.y > 1.0f)
    {
        const float oob = fmod(floor(IN.clipPos.x * 0.25f)
            + floor(IN.clipPos.y * 0.25f), 2.0f) < 1.0f ? 1.0f : 0.0f;
        color = oob ? float3(1.0f, 0.05f, 1.0f) : float3(0.02f, 0.02f, 0.02f);
    }
#endif

    FrameUniforms frame = pc.frameUniforms.Get();
    const float exp = max(frame.exposure, 1e-6f);
    color /= exp;

    OUT.outColor = float4(SanitizeHdr(color),
        (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
