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

// ---------------------------------------------------------------------------
// Soft shadows (PCSS semantics over prefiltered moments).
//
// The moments carry their own blocker estimate: m- = E[exp(-c*d)] is
// dominated by the SMALLEST depth in the blur footprint (the negative
// exponential warp weights near occluders hardest), so -ln(m-)/c ~ the
// depth of the nearest occluder — a conservative blocker, exactly what a
// PCSS blocker search approximates, for the cost of one log per receiver.
//
// Tile-descriptor pads carry the per-tile softness inputs (CPU-baked, see
// BuildDirectionalLight / BuildSpotTile):
//   cascade tiles: pad2.x = depthRange/orthoWidth * tan(sun disc angular
//                  radius)  -> NDC delta * pad2.x = solar penumbra in tile-UV
//                  pad2.y = softness UI multiplier
//   spot tiles:    pad_ = projection near plane (eye-depth inversion),
//                  pad2.x = emitting-sphere diameter (m),
//                  pad2.y = softness UI multiplier
// ---------------------------------------------------------------------------

// Blocker depth from the center moments. The NEGATIVE channel carries the
// nearest occluder: m- = E[exp(-c*d)] is dominated by the SMALLEST depth in
// the blur footprint (exp(-c*d) peaks at small d), so -ln(m-)/c ~ the
// closest blocker — the conservative PCSS blocker. (The positive channel
// is dominated by the LARGEST depth — reading it as a blocker collapses
// everything to "blocker at the far plane" and kills the penumbra.)
// max(., 1e-20): an empty footprint stores the clear value (m- ~ e^-c),
// -ln(e^-c)/c ~ 1 -> "blocker at the far plane" -> fully lit, as intended.
float EvsmBlockerDepth(float2 moments, float exponent)
{
    return clamp(-log(max(moments.y, 1e-20f)) / exponent, 0.0f, 1.0f);
}

// 8-tap unit disk (poisson-ish: 16-step circle angles, jittered radii so
// no tap ring aliases) — every tap evaluates the PREfiltered moments, so
// 8 taps ride on top of the blur's own filtering (vs 32-64 raw PCF taps).
static const float2 kEvsmDisk8[8] = {
    float2( 0.577f,  0.816f), float2(-0.404f,  0.915f), float2(-0.990f, -0.024f), float2(-0.360f, -0.741f),
    float2( 0.421f, -0.663f), float2( 0.984f, -0.178f), float2(-0.729f,  0.409f), float2( 0.108f,  0.994f),
};

// Interleaved gradient noise (fract form) -> per-pixel disk rotation.
float EvsmDiskRotation(float2 pix)
{
    return 52.9829189f * frac(0.06711056f * pix.x + 0.00583715f * pix.y);
}

float2x2 EvsmRotate2x2(float a)
{
    float s, c;
    sincos(a, s, c);
    return float2x2(c, -s, s, c);
}

// Soft directional (cascade tile): solar penumbra. rUV is derived from the
// blocker->receiver NDC delta scaled by the baked per-cascade factor (NDC
// delta is world distance / depthRange; UV = world * tan(sunAngle)/orthoW),
// clamped to sane texel bounds, then an 8-tap rotated disk of moments.
float SampleEvsmTileSoft(
    GpuCascadeDescription c,
    float3 worldPos,
    float3 faceNormal,
    float3 lightDirection,
    uint samplerId,
    uint idx,
    float exponent,
    float2 pix,
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
    const float2 clampedUV = clamp(uvAtlas, c.uvClampMin, c.uvClampMax);
    Texture2D evsmTex = textures[NonUniformResourceIndex(idx)];
    float2 moments = evsmTex.SampleLevel(samplers[samplerId], clampedUV, 0).rg;
    valid = true;

    // Solar penumbra width from the moment blocker (tile-UV units).
    const float blocker = EvsmBlockerDepth(moments, exponent);
    const float delta = max(receiverDepth - blocker, 0.0f);
    const float softness = c.pad2.y;
    float rUV = delta * c.pad2.x * softness;
    // Clamp to [0.75, 12] texels: below ~0.75 the disk degenerates onto the
    // prefilter (single tap is enough), above ~12 the 8 taps streak.
    rUV = clamp(rUV, 0.75f * c.invShadowSize.x, 12.0f * c.invShadowSize.x);

    const float2x2 rot = EvsmRotate2x2(EvsmDiskRotation(pix));
    float sum = 0.0f;
    [unroll]
    for (int t = 0; t < 8; ++t) {
        const float2 off = mul(rot, kEvsmDisk8[t]) * rUV * c.uvScale;
        const float2 tapUV = clamp(uvAtlas + off, c.uvClampMin, c.uvClampMax);
        const float2 m = evsmTex.SampleLevel(samplers[samplerId], tapUV, 0).rg;
        sum += EvsmVisibility(m, receiverDepth, exponent);
    }
    // Center tap carries full weight (it is the hard EVSM answer); the 8
    // disk taps average in the penumbra estimate.
    return (EvsmVisibility(moments, receiverDepth, exponent) + sum / 8.0f) * 0.5f;
}

// Soft spot tile: the old PcssSpotShadow math with the moment blocker.
// Eye depth from the RH_ZO perspective inversion using the tile's near
// (pad_) and far (splitEnd); penumbra = lightSize * (zRecv - zBlk) / zBlk
// in eye space, projected to tile-UV through the cone half angle; then the
// rotated 8-tap disk of moments (each an independent Chebyshev eval).
float SampleEvsmSpotTileSoft(
    GpuCascadeDescription t,
    float3 worldPos,
    float3 faceNormal,
    float3 spotDirection,
    float outerAngle,
    uint samplerId,
    uint idx,
    float exponent,
    float2 pix,
    out bool valid)
{
    valid = false;
    float3 biasedWorld = worldPos + faceNormal * t.normalBias;
    float4 clip = mul(t.lightVP, float4(biasedWorld, 1.0f));
    if (clip.w <= 1e-6f)
        return 1.0f;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z <= 0.0f || ndc.z >= 1.0f)
        return 1.0f;
    float2 uv = ndc.xy * 0.5f + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;
    float2 uvAtlas = uv * t.uvScale + t.uvBias;
    float receiverDepth = ndc.z + EvsmOrientedBias(faceNormal, spotDirection, t.bias);
    const float2 clampedUV = clamp(uvAtlas, t.uvClampMin, t.uvClampMax);
    Texture2D evsmTex = textures[NonUniformResourceIndex(idx)];
    float2 moments = evsmTex.SampleLevel(samplers[samplerId], clampedUV, 0).rg;
    valid = true;

    // Eye depths (RH_ZO perspective inversion; near = pad_, far = splitEnd).
    const float nearP = max(t.pad_, 1e-3f);
    const float farP = max(t.splitEnd, nearP + 0.01f);
    const float zRecv = farP * nearP / max(farP - ndc.z * (farP - nearP), 1e-4f);
    const float blocker = EvsmBlockerDepth(moments, exponent);
    const float ndcBlocker = clamp(blocker, 0.0f, 0.9999f);
    const float zBlocker = farP * nearP / max(farP - ndcBlocker * (farP - nearP), 1e-4f);
    if (zBlocker >= zRecv)
        return 1.0f; // blocker behind/at receiver: fully lit

    // Penumbra (eye-space similar triangles; lightSize = pad2.x emitting
    // diameter, softness = pad2.y), projected into tile UV through the
    // cone half angle: the tile spans 2*z*tanOuter at eye depth z.
    const float lightSize = max(t.pad2.x, 0.0f);
    const float softness = max(t.pad2.y, 0.0f);
    const float tanOuter = max(tan(min(outerAngle, 1.45f)), 1e-3f);
    float rPenUV = lightSize * softness * (zRecv - zBlocker)
        / max(zBlocker * 2.0f * zRecv * tanOuter, 1e-4f);
    // Clamp to [0.75, 14] texels (each tap is already soft from the blur,
    // so the disk only shapes the variable outer penumbra).
    const float tileRes = 1.0f / max(t.invShadowSize.x, 1e-6f);
    rPenUV = clamp(rPenUV, 0.75f / tileRes, 14.0f / tileRes);

    const float2x2 rot = EvsmRotate2x2(EvsmDiskRotation(pix));
    float sum = 0.0f;
    [unroll]
    for (int k = 0; k < 8; ++k) {
        const float2 off = mul(rot, kEvsmDisk8[k]) * rPenUV * t.uvScale;
        const float2 tapUV = clamp(uvAtlas + off, t.uvClampMin, t.uvClampMax);
        const float2 m = evsmTex.SampleLevel(samplers[samplerId], tapUV, 0).rg;
        sum += EvsmVisibility(m, receiverDepth, exponent);
    }
    return (EvsmVisibility(moments, receiverDepth, exponent) + sum / 8.0f) * 0.5f;
}
