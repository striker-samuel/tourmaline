// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSGI_DIFFUSE_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "hzb.hlsli"
#include "ssr_receiver.hlsli"
#include "gpu_platform.hlsli"

// Diffuse SSGI blur: edge-aware bilateral of the half-res raw GI layer
// (rgb = raw incoming radiance, a = hit confidence), structured on
// ssr_blur.frag.hlsl but WITHOUT the roughness-scaled radius buckets:
// diffuse indirect spreads by GEOMETRIC similarity (depth + normal),
// not by mirror roughness — one fixed 5x5 kernel (radius 2, the
// {6,4,1} axis table) for every receiver.
//
// Depth weights: the shared final-depth MAX-HZB mip1 (nearest-of-2x2
// under reversed-Z) via BDA scalars — zero depth-texture loads, the
// exact value the center quad max derived (the SSGI shared-HZB
// rework's discipline). Normal weights: the trace's packed receiver
// metadata (one BDA scalar per tap — the SELECTED receiver of each
// block, not a corner texel).
//
// Confidence-aware accumulation (the blur's convention, verbatim from
// ssr_blur): rgb weighted by the tap's CONFIDENCE x the bilateral
// weight, normalized by the confidence mass — a miss (conf 0) never
// dilutes its neighbors' valid radiance, and low-hit-rate pixels keep
// a smooth path to the baked-GI fallback in the composite (the
// confidence channel rides separately from radiance all the way to
// the compose).

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F blurred GI layer
};

// Must match Engine/Rendergraph/ssgi_diffuse.h SsgiDiffuseBlurPush
// (64 bytes). All-scalar layout on purpose (the spirv-val
// "improperly straddling vector" hazard — see SsrBlurPush's note).
struct SsgiDiffuseBlurPush
{
    GPU_PTR_READ(FrameUniforms) frameUniforms; // nearPlane
    GPU_PTR_READ(HzbBuffer) hzb; // MAX-HZB: mip1 (nearest-of-2x2) reads
    GPU_PTR_READ(SsrReceiverData) receiverData; // packed receiver per half-res pixel
    uint giTextureIndex;   // half-res raw GI layer
    uint samplerIndex;     // linear clamp (radiance taps only)
    float sharpness;       // depth edge weight (higher = tighter)
    float texelX;          // 1 / half-res width
    float texelY;          // 1 / half-res height
    uint hzbMip1Offset;    // float offset of mip1 in the HZB buffer
    uint hzbMip1Stride;    // mip1 row stride (floats)
    uint halfW;            // half-res extent (BDA tap clamp domain)
    uint halfH;
    uint pad0;
};

GPU_PUSH(SsgiDiffuseBlurPush, bp);

// Relative-depth edge weight (ssr_blur's twin): 1 on the same surface,
// ~0 across a discontinuity; background taps carry the sky sentinel.
float SsgiEdgeWeight(float tapDist, float centerDist, float sharpness)
{
    float depthDiff = abs(tapDist - centerDist) / max(centerDist, 1e-3f);
    return exp(-depthDiff * max(sharpness, 0.0f));
}

// The 5x5 {6,4,1} axis table (radius 2 — fixed for diffuse).
float SsgiAxisWeight(int k)
{
    const int a = abs(k);
    if (a == 0) return 6.0f;
    if (a == 1) return 4.0f;
    return 1.0f;
}

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = bp.frameUniforms.Get();
    Texture2D giTex = textures[NonUniformResourceIndex(bp.giTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(bp.samplerIndex)];
    float nearPlane = max(frame.nearPlane, 1e-4f);

    int2 halfPx = int2(pos.xy);

    // Center block depth: HZB mip1 IS the block's quad MAX (the
    // nearest valid tap; an all-background block reads 0).
    float dCenter = bp.hzb.Get().texels[
        bp.hzbMip1Offset + (uint) halfPx.y * bp.hzbMip1Stride + (uint) halfPx.x];
    if (DofIsBackgroundOrGarbage(dCenter))
    {
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }
    float centerDist = nearPlane / max(dCenter, 1e-6f);

    // Center receiver (the trace's packed metadata): world oct normal
    // for the tap-normality weights. The sentinel decodes +Z neutral.
    float4 centerGb = SsrUnpackReceiver(bp.receiverData.Get().v[
        (uint) halfPx.y * bp.halfW + (uint) halfPx.x]);
    float3 centerN = SsrOctDecode(centerGb.xy);

    const int radius = 2; // fixed 5x5: diffuse spread is geometric, not material
    float3 rgbAcc = float3(0.0f, 0.0f, 0.0f);
    float confAcc = 0.0f;
    float wSum = 0.0f;
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            float2 tapUv = IN.uv + float2(float(x) * bp.texelX,
                                           float(y) * bp.texelY);
            float4 tap = giTex.Sample(smp, tapUv);

            // Tap rep depth: mip1 at the tap's half-res coords, clamped
            // to the extent (the edge-tap pairing contract from ssr_blur).
            int2 thp = clamp(
                halfPx + int2(x, y),
                int2(0, 0),
                int2((int) bp.halfW - 1, (int) bp.halfH - 1));
            float tapD = bp.hzb.Get().texels[
                bp.hzbMip1Offset + (uint) thp.y * bp.hzbMip1Stride + (uint) thp.x];
            float tapDist = DofIsBackgroundOrGarbage(tapD)
                ? 1e5 // sky sentinel (matches dof.hlsli scale)
                : nearPlane / max(tapD, 1e-6f);

            float w = SsgiAxisWeight(x) * SsgiAxisWeight(y)
                * SsgiEdgeWeight(tapDist, centerDist, bp.sharpness);
            // Normal agreement (world oct both sides — the receiver
            // metadata's domain): a wall meeting a floor stays
            // separated where the depth weight cannot tell them apart.
            float4 tapGb = SsrUnpackReceiver(bp.receiverData.Get().v[
                (uint) thp.y * bp.halfW + (uint) thp.x]);
            w *= SsrNormalWeight(centerN, SsrOctDecode(tapGb.xy));

            float confidence = saturate(tap.a);
            rgbAcc += tap.rgb * confidence * w;
            confAcc += confidence * w;
            wSum += w;
        }
    }
    float outConf = (wSum > 1e-5f) ? saturate(confAcc / wSum) : 0.0f;
    float3 outRgb = (confAcc > 1e-5f)
        ? rgbAcc / confAcc
        : float3(0.0f, 0.0f, 0.0f);
    OUT.outColor = float4(SanitizeHdr(outRgb), outConf);
    return OUT;
}
