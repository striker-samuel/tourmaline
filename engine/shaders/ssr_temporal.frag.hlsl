// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSR_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "ssr_compute.hlsli"
#include "gpu_platform.hlsli"

// SSR temporal: motion-vector-reprojected accumulation of the half-res
// reflection layer (rgb = reflected x fresnel x intensity, a = confidence).
// Scoped to SSR only: the base HDR is never touched here (it is re-added
// later in the composite), so a rejection simply falls back to the current
// frame's spatially-blurred tap.
//
// Reprojection is RECEIVER-ALIGNED: motion is Loaded at the selected
// receiver's own full-res pixel (the block's nearest-valid tap — a
// block-center sample is a different surface at silhouettes) and applied
// to the receiver's own UV. The motion buffer holds prevUV - currUV in
// UV units and INCLUDES the T2x/DLSS jitter (prevViewProj is jittered),
// while the SSR trace runs on the same jittered proj — so the history
// fetch uses the RAW vector (prevUv = receiverUv + motion). The
// DEJITTERED vector (motion - jitterDelta) drives only the blend
// attenuation, same discipline as motionblur.frag.hlsl and the SMAA
// neighborhood pass.
//
// Disocclusion is a PREVIOUS-FRAME test on both sides: the receiver is
// projected into the previous camera (invView -> world ->
// prevViewProj) and its expected previous depth is compared against
// the previous record's depth mirror (ssr_depth_history.frag.hlsl) at
// the reprojected coordinate — camera motion cancels exactly instead
// of riding the tolerance (see the disocclusion block's note).

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F temporally filtered reflection layer
};

// Must match Engine/Rendergraph/ssr.h SsrTemporalPush (72 bytes).
// All-scalar layout on purpose: float2 members at non-8-byte-aligned
// offsets make spirv-val reject the module ("improperly straddling
// vector"), see SsrBlurPush in ssr_blur.frag.hlsl.
struct SsrTemporalPush
{
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    uint currTextureIndex;    // half-res blurred reflection (this frame)
    uint historyTextureIndex; // half-res temporal history (previous record)
    uint motionTextureIndex;  // render-extent RG16F motion (prevUV - currUV)
    uint depthTextureIndex;   // full-res depth (authority + disocclusion)
    uint prevDepthTextureIndex; // previous record's depth mirror (the
                                // disocclusion test's history side; see
                                // ssr_depth_history.frag.hlsl)
    uint samplerIndex;        // linear clamp
    uint historyValid;        // 0 = warmup passthrough of curr
    float jitterDeltaX;       // (prevJitter - currJitter) UV
    float jitterDeltaY;
    float texelX;             // 1 / half-res width
    float texelY;             // 1 / half-res height
    float invDepthW;          // 1 / full-res depth width
    float invDepthH;          // 1 / full-res depth height
    float blend;              // history weight 0..0.98 (before attenuation)
    float sharpness;          // depth edge weight (higher = tighter)
    uint pad0;
};

GPU_PUSH(SsrTemporalPush, tp);

PSOut mainPS(float4 pos : SV_Position, PSIn IN)
{
    PSOut OUT;
    FrameUniforms frame = tp.frameUniforms.Get();
    Texture2D currTex = textures[NonUniformResourceIndex(tp.currTextureIndex)];
    Texture2D histTex = textures[NonUniformResourceIndex(tp.historyTextureIndex)];
    Texture2D motionTex = textures[NonUniformResourceIndex(tp.motionTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(tp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(tp.samplerIndex)];

    float4 curr = currTex.Sample(smp, IN.uv);

    // Center 2x2 block from full-res depth (same discipline as the blur):
    // nearest valid tap (reverse-Z max depth) is the authority.
    int2 halfPx = int2(pos.xy);
    int2 fullBase = halfPx * 2;
    float d00 = depthTex.Load(int3(fullBase, 0)).r;
    float d10 = depthTex.Load(int3(fullBase + int2(1, 0), 0)).r;
    float d01 = depthTex.Load(int3(fullBase + int2(0, 1), 0)).r;
    float d11 = depthTex.Load(int3(fullBase + int2(1, 1), 0)).r;
    float dCenter = 0.0f;
    int2 fullPxC = fullBase;
    if (!DofIsBackgroundOrGarbage(d00) && d00 > dCenter) { dCenter = d00; fullPxC = fullBase; }
    if (!DofIsBackgroundOrGarbage(d10) && d10 > dCenter) { dCenter = d10; fullPxC = fullBase + int2(1, 0); }
    if (!DofIsBackgroundOrGarbage(d01) && d01 > dCenter) { dCenter = d01; fullPxC = fullBase + int2(0, 1); }
    if (!DofIsBackgroundOrGarbage(d11) && d11 > dCenter) { dCenter = d11; fullPxC = fullBase + int2(1, 1); }
    if (DofIsBackgroundOrGarbage(dCenter))
    {
        // Sky: no reflector here — carry nothing (never keep history).
        OUT.outColor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return OUT;
    }
    if (tp.historyValid == 0u)
    {
        // Warmup: fill the history slot with the current tap.
        OUT.outColor = curr;
        return OUT;
    }

    float2 invDepthSize = float2(tp.invDepthW, tp.invDepthH);
    float nearPlane = max(frame.nearPlane, 1e-4f);
    // The receiver's own UV + view position (the block's nearest-valid
    // tap — the same selection the classify/intersect use; the ray
    // chain is receiver-consistent, the temporal chain now is too).
    float2 receiverUv = (float2(fullPxC) + 0.5f) * invDepthSize;
    float2 ndcC = receiverUv * 2.0f - 1.0f;
    float3 viewPos = SsrUnprojectView(ndcC, dCenter, frame.invProj);

    // RECEIVER-ALIGNED MOTION (the silhouette fix): the motion comes
    // from the SELECTED receiver's own full-res pixel — the old
    // block-center Sample was a different surface at silhouettes, so
    // the reprojection followed the WRONG world point's trajectory.
    // RAW (jitter-inclusive) vector, same contract as before: history
    // was rendered under the previous record's jitter, current under
    // this record's, so the jitter component is part of the exact
    // mapping (the DEJITTERED vector still drives only the blend
    // attenuation below).
    float2 motionRaw = motionTex.Load(int3(fullPxC, 0)).rg;
    if (!isfinite(motionRaw.x + motionRaw.y))
        motionRaw = float2(0.0f, 0.0f);
    float2 prevUv = receiverUv + motionRaw;
    if (prevUv.x < 0.0f || prevUv.x > 1.0f || prevUv.y < 0.0f || prevUv.y > 1.0f)
    {
        OUT.outColor = curr;
        return OUT;
    }

    // History fetch: FOUR-TEXEL CONFIDENCE-WEIGHTED bilinear (the raw
    // Sample interpolated UNPREMULTIPLIED rgb across the footprint — an
    // invalid texel's rgb poisoned its valid neighbors' rgb before the
    // blend ever saw them). Same convention as the blur/compose: rgb
    // weighted by confidence x the bilinear factor, normalized by the
    // confidence mass; out-of-footprint border texels are SKIPPED (the
    // normalization absorbs them) instead of clamped into duplicates.
    float2 halfExtentF = float2(1.0f / max(tp.texelX, 1e-9f),
                                1.0f / max(tp.texelY, 1e-9f));
    int2 halfExtentI = int2(halfExtentF);
    float2 prevPx = prevUv * halfExtentF - 0.5f;
    int2 p0 = int2(floor(prevPx));
    float2 f = frac(prevPx);
    float3 hRgbAcc = float3(0.0f, 0.0f, 0.0f);
    float hConfAcc = 0.0f;
    float hW = 0.0f;
    [loop]
    for (int hy = 0; hy < 2; ++hy)
    {
        [loop]
        for (int hx = 0; hx < 2; ++hx)
        {
            int2 hp = p0 + int2(hx, hy);
            if (hp.x < 0 || hp.y < 0 || hp.x >= halfExtentI.x || hp.y >= halfExtentI.y)
                continue;
            float bw = (hx ? f.x : 1.0f - f.x) * (hy ? f.y : 1.0f - f.y);
            float2 hUv = (float2(hp) + 0.5f) / halfExtentF;
            float4 t = histTex.Sample(smp, hUv);
            float c = saturate(t.a);
            hRgbAcc += t.rgb * c * bw;
            hConfAcc += c * bw;
            hW += bw;
        }
    }
    float4 hist = float4(0.0f, 0.0f, 0.0f, 0.0f);
    hist.a = (hW > 1e-5f) ? saturate(hConfAcc / hW) : 0.0f;
    hist.rgb = (hConfAcc > 1e-5f)
        ? hRgbAcc / hConfAcc
        : float3(0.0f, 0.0f, 0.0f);

    // Disocclusion — BOTH SIDES IN THE PREVIOUS FRAME'S PROJECTION:
    // project THIS frame's receiver into the previous camera
    // (invView -> world, prevViewProj — the same matrix the motion
    // pass used) and compare that expected previous depth against the
    // depth the history's surface recorded (prevDepth at prevUv, the
    // previous record's mirror). The same surface matches; a
    // disocclusion (the history's surface other than / behind the
    // receiver) mismatches. The earlier forms mixed cameras: the
    // original compared the CURRENT depth at a previous-frame
    // coordinate, and the first mirror fix compared a current-frame
    // receiver distance against a prev-frame linearization — both
    // left camera motion inside the comparison. The near/d
    // linearization is projection-stable (near is frame-stable), so
    // the relative threshold keeps its scale, and a static scene's
    // expected-vs-sampled pair is EXACTLY equal (no tolerance eaten by
    // parallax).
    if (tp.prevDepthTextureIndex == 0xffffffffu)
    {
        // No mirror bound (a misconfigured host — the chain always
        // creates + binds it): REJECT the history rather than
        // approximate with the wrong frame's depth.
        OUT.outColor = curr;
        return OUT;
    }
    Texture2D prevDepthTex =
        textures[NonUniformResourceIndex(tp.prevDepthTextureIndex)];
    float4 worldPos = mul(frame.invView, float4(viewPos, 1.0f));
    float4 prevClip = mul(frame.prevViewProj, worldPos);
    if (prevClip.w <= 0.0f)
    {
        // The receiver was behind the previous camera (the camera
        // moved past it): no previous-frame visibility — disoccluded.
        OUT.outColor = curr;
        return OUT;
    }
    float expectedPrevD = prevClip.z / prevClip.w;
    float expectedDist = nearPlane / max(expectedPrevD, 1e-6f);
    float2 fullExtent = float2(1.0f / max(invDepthSize.x, 1e-9f),
                               1.0f / max(invDepthSize.y, 1e-9f));
    int2 tapFull = clamp(int2(prevUv * fullExtent),
                         int2(0, 0), int2(max(fullExtent - 1.0f, float2(1.0f, 1.0f))));
    float tapD = prevDepthTex.Load(int3(tapFull, 0)).r;
    float sampledDist = DofIsBackgroundOrGarbage(tapD)
        ? 1e5 // sky sentinel (matches dof.hlsli scale)
        : nearPlane / max(tapD, 1e-6f);
    float depthDiff = abs(sampledDist - expectedDist) / max(expectedDist, 1e-3f);
    float depthW = exp(-depthDiff * max(tp.sharpness, 0.0f));
    if (depthW < 1e-3f)
    {
        OUT.outColor = curr;
        return OUT;
    }

    // Anti-ghost clamp: fit history rgb into the 3x3 neighborhood AABB
    // of the current frame (same remedy as classic color-AABB TAA).
    float3 nMin = curr.rgb;
    float3 nMax = curr.rgb;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            float3 t = currTex.Sample(smp, IN.uv + float2(float(x) * tp.texelX,
                                                          float(y) * tp.texelY)).rgb;
            nMin = min(nMin, t);
            nMax = max(nMax, t);
        }
    }
    float3 histClamped = clamp(hist.rgb, nMin, nMax);

    // Attenuate the history weight by TRUE (dejittered) motion: a still
    // camera converges fully, fast motion favors the current tap so
    // reflections never smear behind moving geometry.
    float2 vTrue = motionRaw - float2(tp.jitterDeltaX, tp.jitterDeltaY);
    float2 vHalfPx = vTrue / max(float2(tp.texelX, tp.texelY), float2(1e-9f, 1e-9f));
    float motionGate = exp(-length(vHalfPx) * 0.25f);
    float effBlend = clamp(tp.blend, 0.0f, 0.98f) * depthW * motionGate;
    // Miss pixels (a ~= 0) carry no signal: let any clamped history decay
    // instead of parking stale reflections on a miss.
    if (curr.a <= 1e-4f)
        effBlend *= 0.35f;

    // Confidence-aware blend in the PREMULTIPLIED domain (rgb x conf),
    // output unpremultiplied: the old independent rgb/alpha lerps let a
    // stale history rgb survive under a faded confidence (and a fresh
    // rgb ride an inflated one) — the premultiplied lerp ties rgb to
    // the confidence that actually carried it.
    float3 currPremul = curr.rgb * saturate(curr.a);
    float3 histPremul = histClamped * saturate(hist.a);
    float3 outPremul = lerp(currPremul, histPremul, effBlend);
    float outA = lerp(saturate(curr.a), saturate(hist.a), effBlend);
    float3 outRgb = (outA > 1e-5f)
        ? outPremul / outA
        : float3(0.0f, 0.0f, 0.0f);
    OUT.outColor = float4(SanitizeHdr(outRgb), outA);
    return OUT;
}
