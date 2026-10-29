// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "hzb.hlsli"
#include "ssr_receiver.hlsli"

// SSR composite: joint-bilateral upsample of the half-res blurred
// reflection layer onto the FULL-res base HDR (additive) — depth
// agreement (HZB mip0 center vs mip1 block reps) x normal agreement
// (the pixel's own G-buffer normal vs the tap's receiver normal) x a
// tent spatial factor (the tap block's center distance), with
// confidence-aware accumulation (rgb weighted by the tap confidence,
// normalized by the confidence mass).
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

// Must match Engine/Rendergraph/ssr.h SsrComposePush (88 bytes).
// uint64 BDAs lead (see SsrClassifyPush's note); scalars after.
// receiverData: the classify's packed receiver metadata (the half-res
// tap normals for the upsample's normal agreement); gbufferTextureIndex:
// the FULL-res thin G-buffer (the center normal's + the output pixel's
// OWN material's source — the center must be the pixel's OWN normal,
// not a block representative, or edge detection weakens exactly where
// upsampling needs it). glossThreshold: the classify's ray gate, reused
// as the composite's MATERIAL ELIGIBILITY threshold (a matte dielectric
// output pixel must not receive its reflective neighbor's layer even
// when depth + normal agree — coplanar different-material surfaces).
// 0xffffffff on gbufferTextureIndex = depth-only (the MSAA/no-G-buffer
// fallback; eligibility stays 1).
struct SsrComposePush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<HzbBuffer> hzb; // MAX-HZB: mip0 center, mip1 reps
    vk::BufferPointer<SsrReceiverData> receiverData; // packed receiver per half-res pixel
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
    float glossThreshold; // the classify's ray gate (eligibility mask)
    uint gbufferTextureIndex; // full-res thin G-buffer (normal + material)
    uint pad0;
    uint pad1;
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

    // Center normal + material: the pixel's OWN full-res G-buffer texel
    // (not a half-res block representative — a representative would
    // weaken edge detection exactly where the upsample needs it).
    // World-space oct, same domain as the receiver metadata's tap
    // normals. The SAME load carries the eligibility material (below).
    bool haveNormals = cp.gbufferTextureIndex != 0xffffffffu;
    float3 centerN = float3(0.0f, 0.0f, 1.0f);
    float eligibility = 1.0f;
    if (haveNormals)
    {
        Texture2D gbTex = textures[NonUniformResourceIndex(cp.gbufferTextureIndex)];
        float4 gb = gbTex.Load(int3(fullPx, 0));
        centerN = SsrOctDecode(gb.xy);
        // MATERIAL ELIGIBILITY (the floating-patch fix): the output
        // pixel's own material must be an SSR-classified reflector.
        // The classify shoots rays only for roughness < glossThreshold
        // or metal, so a matte dielectric's own radiance slot is
        // silence — but the UPSAMPLE could still carry a neighboring
        // reflective metal's layer across their shared plane (depth +
        // normal agree on coplanar different-material surfaces; the
        // half-res blur can blur it in too). Gate the ADD by the
        // pixel's eligibility — the classify's own rule, with a smooth
        // 0.1 band below the threshold so the cut does not ring. No
        // G-buffer = eligibility 1 (the legacy path: no material data,
        // no mask).
        const bool metal = gb.a >= 0.5f;
        const float roughness = saturate(gb.b);
        eligibility = metal
            ? 1.0f
            : 1.0f - smoothstep(cp.glossThreshold - 0.1f, cp.glossThreshold,
                                roughness);
    }

    // Half-res reflection extent: EXACT integer math from the push's
    // uint dimensions — max(1, n/2), identical to HalfExtentFor (which
    // sized the targets AND the classify's receiver-buffer stride).
    //
    // THE DARK-TRIANGLE ROOT CAUSE (the "fixed itself on window
    // maximize" report): the old form reconstructed the extent in
    // floats, floor((1/invDepthW) * 0.5) — the reciprocal of a
    // non-power-of-two-friendly width rounds LOW (1920 -> 959.9999 ->
    // floor 959; 960 and 3840 likewise, while 1280/1600/2560 round
    // exact), and the value doubled as the RECEIVER BUFFER'S ROW
    // STRIDE (hp.y * halfExtent.x + hp.x). A 959 stride reading a
    // 960-stride buffer shifts every row's receiver entries by y: the
    // eligibility + normal gates consumed wrong-pixel material data in
    // a growing per-row shear — a large diagonal cutoff through the
    // gates that vanished the moment the window hit a width whose
    // reciprocal rounds exact (maximize). The blur never had it (its
    // push carries the exact uint halfW).
    const uint2 halfExtentU = max(uint2(1u, 1u),
        uint2(cp.fullW / 2u, cp.fullH / 2u));
    const int2 halfExtentI = int2(halfExtentU);
    const float2 halfExtentF = float2(halfExtentU);

    // 2x2 nearest half-res texels around this full-res pixel, weighted
    // by depth agreement (the same discipline as the SSGI AO compose,
    // so reflection never bleeds across silhouettes) + normal
    // agreement + a TENT SPATIAL factor favoring the blocks whose
    // centers sit nearer this pixel (the old depth-only weights treated
    // the DIAGONAL neighbor block exactly like the block containing
    // the pixel — the blocky half-res transition, sharpest on diagonal
    // edges and subpixel detail).
    //
    // BORDER: out-of-range candidates are SKIPPED before the fetch —
    // the old clamp duplicated a border texel into multiple candidates
    // (a double-counted far tap under the spatial factor would outvote
    // the one near tap); the weight normalization absorbs the skips.
    // The block CONTAINING the pixel is always a candidate and always
    // in range, so a valid pixel never loses every tap; an
    // all-rejected neighborhood (every depth/normal weight ~0) falls to
    // the wSum branch below — base HDR + env reflection untouched
    // rather than an unrelated reflection injected.
    int2 halfMin = int2(floor((float2(fullPx) - 1.0f) * 0.5f));
    float2 fullCenter = float2(fullPx) + 0.5f;
    float3 accRgb = float3(0.0f, 0.0f, 0.0f);
    float accA = 0.0f;
    float wSum = 0.0f;
    [loop]
    for (int ty = 0; ty < 2; ++ty)
    {
        [loop]
        for (int tx = 0; tx < 2; ++tx)
        {
            int2 hp = halfMin + int2(tx, ty);
            if (hp.x < 0 || hp.y < 0 || hp.x >= halfExtentI.x || hp.y >= halfExtentI.y)
                continue; // border candidate: skip, the normalization absorbs it
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
            float2 halfUv = (float2(hp) + 0.5f) / halfExtentF;
            float4 tap = blurTex.Sample(smp, halfUv);
            // Tent/bilinear spatial factor: the tap block's center (in
            // full-res units) against this pixel's center, scaled to
            // half-res pixel units — the containing block carries ~0.56,
            // edge neighbors ~0.19, the diagonal ~0.06.
            float2 tapCenterFull = float2(hp) * 2.0f + 1.0f;
            float2 delta = abs(fullCenter - tapCenterFull) * 0.5f;
            float2 axisWeight = saturate(1.0f - delta);
            float spatialWeight = axisWeight.x * axisWeight.y;
            float w = spatialWeight
                * SsrUpEdgeWeight(bDist, centerDist, cp.sharpness);
            // Normal agreement: the tap's RECEIVER normal (the block's
            // selected surface) against the pixel's own full-res
            // normal — same-depth surfaces with different orientations
            // (wall meets floor) stop bleeding halos into each other.
            // Gated: no G-buffer = the old depth-only weights.
            if (haveNormals)
            {
                // Receiver index: the EXACT uint extent's stride (the
                // classify's write stride — see the halfExtentU note for
                // the off-by-one the old float reconstruction caused).
                float4 repGb = SsrUnpackReceiver(cp.receiverData.Get().v[
                    (uint) hp.y * halfExtentU.x + (uint) hp.x]);
                w *= SsrNormalWeight(centerN, SsrOctDecode(repGb.xy));
            }
            // Confidence-aware accumulation (the blur's convention): rgb
            // weighted by the tap's confidence x upsample weight, the
            // confidence mass accumulated separately — a low-conf tap
            // stops diluting its neighbors' valid reflection rgb.
            float confidence = saturate(tap.a);
            accRgb += tap.rgb * confidence * w;
            accA += confidence * w;
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
        // Confidence-aware reconstruction (the blur's convention):
        // conf = the confidence mass's weighted mean, layer = the
        // confidence-weighted radiance mean — a valid reflection keeps
        // its radiance under a partially confident neighborhood instead
        // of being averaged toward the misses' zero.
        const float conf = saturate(accA / wSum);
        const float3 layer = (accA > 1e-5f)
            ? accRgb / accA
            : float3(0.0f, 0.0f, 0.0f);
        // eligibility gates the WHOLE add symmetrically: an ineligible
        // (matte) pixel neither gains the layer nor loses its envSpec —
        // the classify never traced it, so its base IBL stands exactly
        // as the base authored it.
        const float add = conf * eligibility;
        outColor += envSpecOn
            ? add * (layer - envSpec)
            : add * layer; // the additive fallback (no export bound)
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
