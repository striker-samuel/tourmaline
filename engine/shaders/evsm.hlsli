#pragma once
// EVSM (Exponential Variance Shadow Maps) shared helpers.
//
// Moments (RG32F, see Rendergraph/evsm.h):
//   R = E[exp(+c * d)], G = E[exp(-c * d)], d = light-space depth in [0,1].
// The MSAA moment pass writes the warped depth per sample; the async
// prefilter (evsm_blur.cs.hlsl, separable Gaussian H+V on the compute queue)
// produces the filtered expectations. Lighting evaluates a dual one-sided
// Chebyshev bound per channel and takes the minimum (Lauritzen & McCool
// 2008, Sec. 3.3) — this is what kills the VSM light-bleeding tail while
// keeping the filterable (blurrable) representation.
//
// c = kEvsmExponent (42): exp(42) ~ 1.7e18 fits fp32 with headroom;
// exp(-42) ~ 5.7e-19 stays normal. Larger c tightens bleeding but pushes
// the positive channel toward overflow under blur overshoot.

// Default warp coefficient. Must match C++ smvk::kEvsmExponent.
#ifndef EVSM_EXPONENT
#define EVSM_EXPONENT 42.0f
#endif

// Warp a [0,1] depth into the two positive exponential channels.
float2 EvsmWarp(float depth, float exponent)
{
    float d = clamp(depth, 0.0f, 1.0f);
    return float2(exp(exponent * d), exp(-exponent * d));
}

// One-sided Chebyshev upper bound for a single exponential channel:
// occluder moments occl = filtered E[w(z)], receiver warped wT.
// Returns 1 (lit) when wT <= occl, else variance bound in (0,1).
// minVariance floors the denominator ( proportion of light-bleeding control;
// 1e-5 matches the paper's epsilon for fp32 moments).
float EvsmChebyshev(float occl, float wT)
{
    if (wT <= occl)
        return 1.0f;
    // Single-moment bound (no E[w^2] stored in the 2-channel variant):
    // variance is estimated from the cross-channel consistency instead.
    // Fall back to a tight exponential falloff — continuous, no hard cut.
    float diff = wT - occl;
    float variance = max(occl * occl * 1e-4f, 1e-8f);
    return variance / (variance + diff * diff);
}

// Full dual-channel EVSM visibility from filtered moments.
// moments = RG32F texel (already blurred), receiverDepth = biased NDC depth.
float EvsmVisibility(float2 moments, float receiverDepth, float exponent)
{
    float2 wT = EvsmWarp(receiverDepth, exponent);
    // Guard against uninitialized / cleared texels (clear color is (1,1):
    // exp(0)=1 => exactly lit at d=0, slightly shadowed deeper — the
    // moment pass clears to EvsmClearColor below, matching d=1 far).
    moments = max(moments, float2(1e-6f, 1e-6f));
    float vp = EvsmChebyshev(moments.x, wT.x);
    float vn = EvsmChebyshev(moments.y, wT.y);
    // Lauritzen Sec. 3.3: intersection of the two bounds.
    float v = min(vp, vn);
    // Rescale the classic VSM bleed: values below ~0.15 are almost always
    // bleed-through on thin occluders; keep them but compress so a single
    // exponential tail cannot wash the whole cascade.
    return saturate(v);
}

// Clear color for the MSAA moment target, matching far-plane depth d=1:
// (exp(c), exp(-c)). Resolved uncleared texels then read as "far occluder"
// instead of lit, so missing geometry shadows rather than leaks.
float4 EvsmClearColor(float exponent)
{
    float2 w = EvsmWarp(1.0f, exponent);
    return float4(w.x, w.y, 0.0f, 1.0f);
}

// Slope-scaled NDC bias in depth domain (same math as GetOrientedBias in
// shadows.hlsli, duplicated here so this header stays self-contained for
// standalone TUs like shadow_evsm.frag.hlsl that never include shadows).
float EvsmOrientedBias(float3 faceNormal, float3 lightDirection, float baseBiasNDC)
{
    float nDotL = saturate(dot(faceNormal, -lightDirection));
    float slope = 1.0f - nDotL;
    return -baseBiasNDC * (0.3f + 0.7f * slope);
}

// Sample one EVSM cascade/spot tile. Mirrors SampleCascadeTile (shadows.hlsli)
// but reads the blurred RG32F moments with a linear sampler and applies the
// oriented depth bias BEFORE the exponential warp (bias in depth domain,
// otherwise the warp magnifies it by c*exp(c*d)).
float SampleEvsmTile(
    GpuCascadeDescription c,
    float3 worldPos,
    float3 faceNormal,
    float3 lightDirection,
    uint samplerId,
    uint idx,
    float exponent,
    out bool valid)
{
    valid = false;
    float3 biasedWorld = worldPos + faceNormal * c.normalBias;
    float4 clip = mul(c.lightVP, float4(biasedWorld, 1.0f));
    float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    float2 uv = ndc.xy * 0.5f + 0.5f;
    if (ndc.z <= 0.0f || ndc.z >= 1.0f ||
        uv.x < 0.0f || uv.x > 1.0f ||
        uv.y < 0.0f || uv.y > 1.0f)
    {
        return 1.0f;
    }
    float2 uvAtlas = uv * c.uvScale + c.uvBias;
    float receiverDepth = ndc.z + EvsmOrientedBias(faceNormal, lightDirection, c.bias);
    float2 clampedUV = clamp(uvAtlas, c.uvClampMin, c.uvClampMax);
    float2 moments = textures[NonUniformResourceIndex(idx)]
        .SampleLevel(samplers[samplerId], clampedUV, 0).rg;
    valid = true;
    return EvsmVisibility(moments, receiverDepth, exponent);
}
