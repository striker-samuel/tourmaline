// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SSGI_DIFFUSE_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "ssr_compute.hlsli"
#include "gpu_platform.hlsli"

// Diffuse SSGI temporal: motion-vector-reprojected accumulation of the
// half-res GI layer (rgb = raw incoming radiance, a = hit confidence).
// Structured on ssr_temporal.frag.hlsl (receiver-aligned reprojection,
// the raw-vs-dejittered motion split, the four-texel confidence-
// weighted history fetch, the current-neighborhood AABB clamp, the
// premultiplied confidence blend) with ONE deliberate strengthening:
// the history validation checks PREVIOUS DEPTH *AND* PREVIOUS NORMALS
// — diffuse GI is a wide neighborhood effect (a 5x5 blur + a full
// hemisphere per pixel), so a moving object inheriting another
// surface's GI reads far worse than a stale reflection streak. The
// previous-frame normal mirror (ssgi_diffuse_prev.frag.hlsl, the
// RGBA8 oct-normal twin of the SSR depth mirror) is the history side
// of that test; the current side is the trace's packed receiver
// metadata.
//
// Scoped to the GI layer only: the base HDR is never filtered here
// (the compose owns it), so a rejection simply falls back to the
// current frame's spatially-blurred tap.

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0; // RGBA16F temporally filtered GI layer
};

// Must match Engine/Rendergraph/ssgi_diffuse.h SsgiDiffuseTemporalPush
// (80 bytes). All-scalar layout on purpose (the spirv-val
// "improperly straddling vector" hazard).
struct SsgiDiffuseTemporalPush
{
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    uint currTextureIndex;      // half-res blurred GI (this frame)
    uint historyTextureIndex;   // half-res temporal history (previous record)
    uint motionTextureIndex;    // render-extent RG16F motion (prevUV - currUV)
    uint depthTextureIndex;     // full-res depth (authority + disocclusion)
    uint prevDepthTextureIndex; // previous record's R32F depth mirror
    uint prevNormalTextureIndex; // previous record's RGBA8 oct-normal mirror
    uint gbufferTextureIndex;   // thin G-buffer (the CURRENT receiver normal)
    uint samplerIndex;          // linear clamp
    uint historyValid;          // 0 = warmup passthrough of curr
    float jitterDeltaX;         // (prevJitter - currJitter) UV (attenuation only)
    float jitterDeltaY;
    float texelX;               // 1 / half-res width
    float texelY;               // 1 / half-res height
    float invDepthW;            // 1 / full-res depth width
    float invDepthH;            // 1 / full-res depth height
    float blend;                // history weight 0..0.98 (before attenuation)
    float sharpness;            // depth disocclusion edge weight
    uint pad0;
};

GPU_PUSH(SsgiDiffuseTemporalPush, tp);

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

    // Center 2x2 block from full-res depth: nearest valid tap (reverse-Z
    // max) is the authority — the SAME receiver the trace selected.
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
        // Sky: no receiver — carry nothing (never keep history).
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
    float2 receiverUv = (float2(fullPxC) + 0.5f) * invDepthSize;
    float2 ndcC = receiverUv * 2.0f - 1.0f;
    float3 viewPos = SsrUnprojectView(ndcC, dCenter, frame.invProj);

    // RECEIVER-ALIGNED motion (raw jitter-inclusive vector for the
    // reprojection — history was rendered under the previous jitter;
    // the dejittered vector drives only the blend attenuation below).
    float2 motionRaw = motionTex.Load(int3(fullPxC, 0)).rg;
    if (!isfinite(motionRaw.x + motionRaw.y))
        motionRaw = float2(0.0f, 0.0f);
    float2 prevUv = receiverUv + motionRaw;
    if (prevUv.x < 0.0f || prevUv.x > 1.0f || prevUv.y < 0.0f || prevUv.y > 1.0f)
    {
        OUT.outColor = curr;
        return OUT;
    }

    // History fetch: FOUR-TEXEL CONFIDENCE-WEIGHTED bilinear (the
    // ssr_temporal convention — an invalid texel's rgb never poisons
    // its valid neighbors' before the blend sees them).
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

    // ---- Disocclusion test 1: DEPTH, both sides in the PREVIOUS
    // frame's projection (the ssr_temporal fix, verbatim): project
    // this frame's receiver into the previous camera and compare the
    // expected previous depth against the previous record's depth
    // mirror — camera motion cancels exactly.
    if (tp.prevDepthTextureIndex == 0xffffffffu
        || tp.prevNormalTextureIndex == 0xffffffffu)
    {
        // No mirror bound (a misconfigured host — the chain always
        // creates + binds both): REJECT rather than approximate.
        OUT.outColor = curr;
        return OUT;
    }
    Texture2D prevDepthTex =
        textures[NonUniformResourceIndex(tp.prevDepthTextureIndex)];
    Texture2D prevNormalTex =
        textures[NonUniformResourceIndex(tp.prevNormalTextureIndex)];
    float4 worldPos = mul(frame.invView, float4(viewPos, 1.0f));
    float4 prevClip = mul(frame.prevViewProj, worldPos);
    if (prevClip.w <= 0.0f)
    {
        // The receiver was behind the previous camera: disoccluded.
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

    // ---- Disocclusion test 2: NORMALS (the diffuse strengthening):
    // the history pixel's surface orientation (the previous record's
    // oct-normal mirror at the reprojected coordinate) against THIS
    // receiver's normal (the trace's packed receiver metadata — the
    // same surface the ray marched from). Same-depth different-surface
    // inheritance (the classic GI ghost: a box sliding across a floor
    // of equal depth) survives the depth test but dies here. Both
    // sides are WORLD-space oct — decode both, never rotate one.
    // SsrNormalWeight's 0.75..0.98 smoothstep band: the diffuse
    // counterpart of the blur's normal gate, one tuning value.
    // The CURRENT side: the thin G-buffer at the selected receiver's
    // own full-res pixel (the same nearest-of-2x2 texel the trace
    // selected — the ray marched from this surface).
    Texture2D gbTex = textures[NonUniformResourceIndex(tp.gbufferTextureIndex)];
    float3 curN = SsrOctDecode(gbTex.Load(int3(fullPxC, 0)).xy);
    float3 prevN = SsrOctDecode(
        prevNormalTex.Load(int3(tapFull, 0)).xy);
    float normalW = SsrNormalWeight(curN, prevN);
    if (normalW < 1e-3f)
    {
        OUT.outColor = curr;
        return OUT;
    }

    // Anti-ghost clamp: fit history rgb into the 3x3 neighborhood AABB
    // of the current frame (classic color-AABB TAA; the one-ray
    // hemisphere estimate needs it — a moving lobe's mean walks).
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

    // Attenuate the history weight by TRUE (dejittered) motion: a
    // still camera converges the hemisphere mean, fast motion favors
    // the current tap (GI must not smear behind moving geometry).
    float2 vTrue = motionRaw - float2(tp.jitterDeltaX, tp.jitterDeltaY);
    float2 vHalfPx = vTrue / max(float2(tp.texelX, tp.texelY), float2(1e-9f, 1e-9f));
    float motionGate = exp(-length(vHalfPx) * 0.25f);
    float effBlend = clamp(tp.blend, 0.0f, 0.98f)
        * depthW * normalW * motionGate;
    // Miss pixels (a ~= 0) carry no signal: let any clamped history
    // decay instead of parking stale GI on a miss.
    if (curr.a <= 1e-4f)
        effBlend *= 0.35f;

    // Confidence-aware blend in the PREMULTIPLIED domain (rgb x conf),
    // output unpremultiplied (ssr_temporal's convention verbatim).
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
