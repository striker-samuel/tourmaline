// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "hzb.hlsli"

// SSR composite: depth-guided bilateral upsample of the half-res blurred
// reflection layer onto the FULL-res base HDR (additive).
// Runs in its own pass so the full-res depth is only ever a barriered
// sampled input here; downstream passes then sample plain HDR.
//
// SSGI shared-HZB rework: every depth read comes from the final-depth
// MAX-HZB via BDA scalars — the center depth from mip0 (the exact 1:1
// depth copy) and each tap block's representative from mip1
// (nearest-of-2x2, the EXACT value the old per-tap 4-load quad max
// derived). ZERO depth-texture loads (the old pass did 17: 1 center +
// 4 loads x 4 taps); the depth image leaves this pass's input set
// entirely (the HZB build's read barrier covers the buffer).

struct PSIn
{
    float2 uv : TEXCOORD0;
};

// SSR_DEBUG_CONTRIB (the .dbg variant, TOURMALINE_SSR_DEBUG): a SECOND
// attachment (SV_Target1) carrying the composite's EXACT add term -
// what the reflections actually contribute to the final HDR (the
// vignette-scaled (blurTex bilinear x confidence) add, = outColor -
// base after the vignette lerp). Wasteful by design (a second
// full-res RGBA16F write, armed only from the debug flag): lets the
// SSR contribution be SEEN directly (the ssrdump's ssr_contrib stage
// exports this target) instead of differenced against the base HDR,
// where the sub-0.1% coverage vanishes in the tone map.
#ifdef SSR_DEBUG_CONTRIB
struct PSOut
{
    float4 outColor : SV_Target0;   // RGBA16F HDR + reflections
    float4 outContrib : SV_Target1; // the added reflection term (debug)
};
#else
struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F HDR + reflections
};
#endif

// Must match Engine/Rendergraph/ssr.h SsrComposePush (64 bytes).
// uint64 BDAs lead (see SsrClassifyPush's note); scalars after.
struct SsrComposePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<HzbBuffer> hzb; // MAX-HZB: mip0 center, mip1 reps
    uint blurTextureIndex;  // half-res blurred reflection (rgb, confidence)
    uint hdrTextureIndex;   // full-res base HDR
    uint samplerIndex;      // linear clamp (HDR + reflection taps)
    // The env-specular export (Main's third MRT, the AO-compose-scaled
    // version): the EXACT IBL slice the base holds that the hit REPLACES
    // (the energy-conserving swap — see the formula at the bottom).
    // 0xffffffff = the additive fallback (no replacement).
    uint envSpecTextureIndex;
    float invDepthW; // 1 / full-res depth extent (NDC math + vignette)
    float invDepthH;
    float sharpness; // upsample edge weight
    uint fullW;    // full-res width (HZB mip0 row stride)
    uint fullH;
    uint hzbMip1Offset; // float offset of mip1 in the HZB buffer
    uint hzbMip1Stride; // mip1 row stride (floats)
    float vignetteWidth; // NDC edge fade width (outer % of NDC range)
};

[[vk::push_constant]]
ConstantBuffer<SsrComposePush> cp;

float SsrUpEdgeWeight(float tapDist, float centerDist, float sharpness)
{
    float depthDiff = abs(tapDist - centerDist) / max(centerDist, 1e-3f);
    return exp(-depthDiff * max(sharpness, 0.0f));
}

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = cp.frameUniforms.Get();
    Texture2D blurTex = textures[NonUniformResourceIndex(cp.blurTextureIndex)];
    Texture2D hdrTex = textures[NonUniformResourceIndex(cp.hdrTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(cp.samplerIndex)];
    float nearPlane = max(frame.nearPlane, 1e-4f);

    float3 base = SanitizeHdr(hdrTex.Sample(smp, IN.uv).rgb);

    int2 fullPx = int2(pos.xy);
    // Center depth from HZB mip0 (the exact 1:1 depth copy the build
    // pass made) — one BDA scalar, no depth image read anywhere here.
    float d = cp.hzb.Get().texels[(uint) fullPx.y * cp.fullW + (uint) fullPx.x];
    if (DofIsBackgroundOrGarbage(d))
    {
        // Sky: nothing to reflect off — keep the base color.
        OUT.outColor = float4(base, 1.0f);
#ifdef SSR_DEBUG_CONTRIB
        OUT.outContrib = float4(0.0f, 0.0f, 0.0f, 1.0f);
#endif
        return OUT;
    }
    float2 ndcC = float2((float(fullPx.x) + 0.5f) * cp.invDepthW * 2.0f - 1.0f,
                         (float(fullPx.y) + 0.5f) * cp.invDepthH * 2.0f - 1.0f);
    float centerDist = DofViewDist(d, ndcC, frame.invProj);

    // Half-res reflection extent derived from the full-res depth extent
    // (max(1, floor(n / 2)), same as Ssao::HalfExtentFor).
    float2 halfExtent = max(
        floor(float2(1.0f / cp.invDepthW, 1.0f / cp.invDepthH) * 0.5f),
        float2(1.0f, 1.0f));

    // 2x2 nearest half-res texels around this full-res pixel, weighted by
    // depth agreement with the pixel's own depth — the same discipline as
    // the SSGI AO compose, so reflection never bleeds across silhouettes.
    int2 halfMin = int2(floor((float2(fullPx) - 1.0f) * 0.5f));
    int2 halfMax = int2(halfExtent) - 1;
    float3 accRgb = float3(0.0f, 0.0f, 0.0f);
    float accA = 0.0f;
    float wSum = 0.0f;
    for (int ty = 0; ty < 2; ++ty)
    {
        for (int tx = 0; tx < 2; ++tx)
        {
            int2 hp = clamp(halfMin + int2(tx, ty), int2(0, 0), halfMax);
            // Representative depth of the tapped block: HZB mip1 = MAX
            // (nearest valid tap) of the block's 2x2 — the EXACT value
            // the old 4-load quad max derived, as one BDA scalar (sky
            // writes 0, so a sky-poisoned block collapses to its nearest
            // geometry; an all-background block reads 0 -> the sentinel).
            float bd = cp.hzb.Get().texels[
                cp.hzbMip1Offset + (uint) hp.y * cp.hzbMip1Stride + (uint) hp.x];
            // Infinite reversed-Z: view distance = near / d (exact for
            // this projection; the old code unprojected the winning
            // tap's texel center — a sub-texel NDC placement difference).
            float bDist = DofIsBackgroundOrGarbage(bd)
                ? 1e5
                : nearPlane / max(bd, 1e-6f);
            float2 halfUv = (float2(hp) + 0.5f) / max(halfExtent, float2(1.0f, 1.0f));
            float4 tap = blurTex.Sample(smp, halfUv);
            float w = SsrUpEdgeWeight(bDist, centerDist, cp.sharpness);
            accRgb += tap.rgb * w;
            accA += tap.a * w;
            wSum += w;
        }
    }
    // The reflection term AND the environment term it replaces:
    //     result = base + conf * (layer - envSpec)
    // The layer (the blurred radiance x its confidence alpha, BRDF-
    // weighted by the trace's Fresnel x fade) REPLACES the env-specular
    // slice the base already holds (Main's IBL, the same Fresnel
    // domain — like-for-like). conf=0 keeps the base's IBL untouched;
    // conf=1 swaps the IBL for the true screen-space reflection — the
    // energy-conserving replacement (the old additive composite stacked
    // the true reflection ON TOP of the IBL approximation of the same
    // integral — a mirror read cubemap-IBL + screen-trace stacked).
    // envSpecTextureIndex = 0xffffffff = the additive fallback (the
    // export absent — the old behavior).
    const bool envSpecOn = cp.envSpecTextureIndex != 0xffffffffu;
    float3 envSpec = float3(0.0f, 0.0f, 0.0f);
    if (envSpecOn) {
        Texture2D envSpecTex =
            textures[NonUniformResourceIndex(cp.envSpecTextureIndex)];
        envSpec = envSpecTex.Load(int3(fullPx, 0)).rgb;
    }
    float3 outColor = base;
    if (wSum > 1e-4f) {
        const float3 layer = (accRgb / wSum);
        const float conf = (accA / wSum);
        outColor += envSpecOn
            ? conf * (layer - envSpec)
            : layer * conf; // the additive fallback (no export bound)
    }
    // NDC vignette: fade reflections near screen edges using the target
    // pixel's NDC coordinates. max(|ndc|) ramps from 0 (center) to 1
    // (border); linear ramp over the outer vignetteWidth of the NDC range.
    float edge = max(abs(ndcC.x), abs(ndcC.y));
    float vignette = saturate((1.0f - edge) / max(cp.vignetteWidth, 0.01f));
    outColor = lerp(base, outColor, vignette);
    OUT.outColor = float4(SanitizeHdr(outColor), 1.0f);
#ifdef SSR_DEBUG_CONTRIB
    // The exact add term: post-vignette outColor - base = the reflection
    // contribution actually landing in the HDR (0 at the vignette edge,
    // 0 on a miss by construction - the add only ever raised outColor).
    OUT.outContrib = float4(SanitizeHdr(outColor - base), 1.0f);
#endif
    return OUT;
}
