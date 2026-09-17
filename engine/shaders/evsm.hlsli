#pragma once
// EVSM (Exponential Variance Shadow Maps) shared helpers.
//
// Moments (RGBA32F, see Rendergraph/evsm.h) — first AND second moments of
// both exponential warps (Lauritzen & McCool 2008):
//   R = E[exp(+c*d)]  G = E[exp(+2c*d)]   positive warp, M1/M2
//   B = E[exp(-c*d)]  A = E[exp(-2c*d)]   negative warp, M1/M2
// with d = light-space depth in [0,1]. The MSAA moment pass writes the
// warped depth per sample; because E[warp^2] = E[exp(+-2c*d)], the second
// moments are just squares of the first at bake time. The async prefilter
// (evsm_blur.cs.hlsl, separable Gaussian H+V) produces the filtered
// expectations.
//
// Visibility = min of two one-sided Cantelli (Chebyshev) bounds, each an
// UPPER bound on the lit probability P(d_occ >= d_recv):
//   v+ : upper tail of x = exp(+c*d)  — tight when the deep (lit) side of
//        the warp distribution is separated from the receiver threshold;
//   v- : lower tail of y = exp(-c*d)  — y is DECREASING in d, so lit is
//        P(y <= t'), tight when shallow (shadow) mass dominates.
// Each bound is vacuous (-> 1) exactly in the regime where the other is
// tight, so the min is tight on BOTH sides of every mixed footprint.
//
// Why the previous single-ratio form leaked: vp = E[exp(+c*d)] *
// exp(-c*d_recv) is only the positive warp's M1 bound. The clear value
// bakes "occluder at d=1" (exp(c)), and exp is convex, so any fraction w of
// clear/far mass in the filtered footprint contributes w*exp(c*(1-d_recv))
// — above 1 (full leak) for w > exp(-c*(1-d_recv)), which is ~1e-15 for a
// mid-range receiver. saturate clamps it to 1: every shadow edge adjacent
// to open space lit through, the bleed-reduction remap (which only
// rescales v in [bleed, 1]) could not touch v >> 1, and the clear gate
// only rescues ~fully-clear texels. In the soft path the leaky field made
// center taps flip between 0 and 1 (early-out or 8 leaky disk taps), so
// edges stippled instead of forming a gradient.
//
// Why the old dual-channel min(vp, vn) acned: vn from a SINGLE negative
// moment is a raw shadow-mass proxy — it reads < 1 whenever the footprint
// holds mass deeper than the receiver (every contact edge, every coverage
// boundary, everywhere under perspective spots), so min() darkened
// genuinely lit receivers. The Cantelli pair is different: variance is
// what separates "mass in front of the receiver" from "mass at/behind it"
// (a two-point footprint with the threshold at a support point bounds to
// ~the opposite mass weight — near-exact), and each bound only engages
// when its own tail direction says there is something to bound.
//
// c = kEvsmExponent (42): with second moments the fp32 ceiling is the
// positive M2: exp(2c) = exp(84) ~ 3.3e36 fits with headroom. Resolve and
// blur only average (never exceed the max input), and the Cantelli
// intermediates ((t-mu)^2, mu2) stay <= e^{2c} < FLT_MAX. Larger c
// tightens the bounds but pushes M2 toward overflow.

// Default warp coefficient. Must match C++ smvk::kEvsmExponent.
#ifndef EVSM_EXPONENT
#define EVSM_EXPONENT 42.0f
#endif

// Far-plane gate: a blurred footprint still at >= this fraction of the
// clear value's positive moment (exp(c), the "occluder at d=1" bake) holds
// no in-range caster -> fully lit. Both Cantelli bounds are vacuous for a
// pure-clear texel anyway (mu1 = exp(c) >= t, mu1' = exp(-c) <= t'), so
// this is a rounding guard: resolve/blur of identical clear taps can sit a
// few ulps below exp(c), which would otherwise let a receiver within those
// ulps of d=1 fall through to v+ = 0 (sigma^2 = 0). 0.98 leaves headroom
// for fp32 rounding while staying above anything a near clip-plane caster
// can produce (the zPadding slack means no real geometry lives in the
// gated slice of the depth range).
#ifndef EVSM_CLEAR_GATE
#define EVSM_CLEAR_GATE 0.98f
#endif

// Light-bleeding reduction: rescales the bound toe so weak over-lit
// (v < bleed, e.g. a contact edge where both Cantelli bounds stay loose)
// maps to 0 while fully-lit (>= 1) stays 1. Keeps the smooth falloff
// shape, just tightens the toe.
#ifndef EVSM_BLEED_REDUCTION
#define EVSM_BLEED_REDUCTION 0.15f
#endif

// Oriented bias: slope-scaled NDC push applied to the RECEIVER THRESHOLD
// (dThr = d_recv - push), the warp-domain equivalent of the old
// EvsmWarpBoost multiplication (comparing against d - b lifts the bounds
// by e^{c*b}, identical to the boost on the ESM ratio). Push is
// boost-only (never pushes the receiver deeper — that manufactured acne)
// and clamped so a grazing receiver can't wash into peter-panning; the
// clamp is the push-depth equivalent of the old e^{c*b*S} <= EVSM_BOOST_MAX
// boost cap.
#ifndef EVSM_BIAS_SCALE
#define EVSM_BIAS_SCALE 4.0f
#endif
#ifndef EVSM_BOOST_MAX
#define EVSM_BOOST_MAX 4.0f
#endif

// Warp a [0,1] depth into the four exponential moments. The second
// moments are squares: E[exp(+-c*d)^2] = E[exp(+-2c*d)] per sample, so the
// bake is two exp() + two multiplies.
float4 EvsmWarp(float depth, float exponent)
{
    float d = saturate(depth);
    float wp = exp(exponent * d);
    float wn = exp(-exponent * d);
    return float4(wp, wp * wp, wn, wn * wn);
}

// Cantelli visibility over the filtered warped moments (see the header):
//   moments.x = E[exp(+c*d_occ)]  moments.y = E[exp(+2c*d_occ)]
//   moments.z = E[exp(-c*d_occ)]  moments.w = E[exp(-2c*d_occ)]
// biasPushNDC is the positive oriented-bias push toward the light.
float EvsmVisibility(float4 moments, float receiverDepth, float exponent, float biasPushNDC)
{
    // Sanitize to the analytic moment ranges: every legit input lies in
    // [warp(0), warp(1)] and resolve/blur only average, so anything
    // outside is garbage (unclear tile / inf-poisoned tap). This also
    // caps the Cantelli intermediates at e^{2c} (< FLT_MAX for c=42).
    const float maxPos = exp(exponent);          // e^{+c}
    const float maxNeg = exp(-exponent);         // e^{-c}
    moments = clamp(moments,
        float4(1.0f, 1.0f, maxNeg, maxNeg * maxNeg),
        float4(maxPos, maxPos * maxPos, 1.0f, 1.0f));

    // Far-plane gate (see EVSM_CLEAR_GATE): clears read LIT, matching
    // PCF's depth-1 clear convention.
    if (moments.x >= EVSM_CLEAR_GATE * maxPos)
        return 1.0f;

    // Receiver threshold after the oriented-bias push (see EVSM_BIAS_SCALE).
    const float pushMax = log(EVSM_BOOST_MAX) / max(exponent, 1e-4f);
    const float push = min(max(biasPushNDC, 0.0f) * EVSM_BIAS_SCALE, pushMax);
    const float dThr = saturate(clamp(receiverDepth, 0.0f, 1.0f) - push);

    // Positive warp: lit = P(x >= t), x = exp(+c*d) increasing. Cantelli
    // upper tail; vacuous (bound >= 1) when t <= mu1, i.e. the receiver is
    // at/before the warp mean — the deep mass it would bound includes the
    // whole distribution, so there is nothing to prove.
    const float t = exp(exponent * dThr);
    const float mu1 = moments.x;
    const float mu2 = moments.y;
    float vPos = 1.0f;
    if (t > mu1) {
        const float varPos = max(mu2 - mu1 * mu1, 0.0f);
        const float gap = t - mu1;
        vPos = varPos / (varPos + gap * gap);
    }

    // Negative warp: y = exp(-c*d) is DECREASING, so lit = P(y <= t').
    // Cantelli lower tail; vacuous when t' >= mu1' (footprint mean at/before
    // the receiver — the shallow mass it would bound is nothing real).
    const float tn = exp(-exponent * dThr);
    const float nu1 = moments.z;
    const float nu2 = moments.w;
    float vNeg = 1.0f;
    if (tn < nu1) {
        const float varNeg = max(nu2 - nu1 * nu1, 0.0f);
        const float gapN = nu1 - tn;
        vNeg = varNeg / (varNeg + gapN * gapN);
    }

    float v = min(vPos, vNeg);
    // Light-bleeding reduction on the bound toe (see EVSM_BLEED_REDUCTION).
    v = saturate((v - EVSM_BLEED_REDUCTION) / max(1.0f - EVSM_BLEED_REDUCTION, 1e-4f));
    return saturate(v);
}

// Clear color for the MSAA moment target, matching far-plane depth d=1:
// (exp(c), exp(2c), exp(-c), exp(-2c)). Resolved uncleared texels read as
// "far occluder" for the blocker estimate (EvsmBlockerDepth) but are gated
// back to lit in EvsmVisibility (see the far-plane gate there).
float4 EvsmClearColor(float exponent)
{
    return EvsmWarp(1.0f, exponent);
}

float EvsmOrientedBias(
    float3 faceNormal,
    float3 lightDirection,
    float baseBiasNDC
)
{
    float3 N = normalize(faceNormal);
    float3 L = normalize(-lightDirection);

    float cosTheta = saturate(dot(N, L));

    // tan(theta) = sin(theta) / cos(theta)
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float tanTheta = sinTheta / max(cosTheta, 0.1f);

    // Clamp so near-perpendicular surfaces don't explode.
    float slopeScale = min(tanTheta, 4.0f);

    return -baseBiasNDC * (1.0f + slopeScale);
}

// Sample one EVSM cascade/spot tile. Mirrors SampleCascadeTile (shadows.hlsli)
// but reads the blurred RGBA32F moments with a linear sampler and applies the
// oriented bias as the receiver-threshold push inside EvsmVisibility.
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
    // Oriented bias acts purely as the receiver-threshold push in warp
    // space (see EvsmVisibility); receiverDepth stays the TRUE
    // light-space depth so the soft-path blocker comparison below is
    // exact.
    float biasAmt = -EvsmOrientedBias(faceNormal, lightDirection, c.bias);
    float receiverDepth = ndc.z;
    float2 clampedUV = clamp(uvAtlas, c.uvClampMin, c.uvClampMax);
    float4 moments = textures[NonUniformResourceIndex(idx)]
        .SampleLevel(samplers[samplerId], clampedUV, 0).rgba;
    valid = true;
    return EvsmVisibility(moments, receiverDepth, exponent, biasAmt);
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

// Blocker depth from the negative first moment (channel .z). m- =
// E[exp(-c*d)] is dominated by the SMALLEST depth in the blur footprint
// (exp(-c*d) peaks at small d), so -ln(m-)/c ~ the closest blocker — the
// conservative PCSS blocker. (The positive first moment is dominated by
// the LARGEST depth — reading it as a blocker collapses everything to
// "blocker at the far plane" and kills the penumbra.) An empty footprint
// stores the clear value (m- ~ e^-c), -ln(e^-c)/c ~ 1 -> "blocker at the
// far plane" -> fully lit, as intended.
float EvsmBlockerDepth(float4 moments, float exponent)
{
    return clamp(-log(max(moments.z, 1e-20f)) / exponent, 0.0f, 1.0f);
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
    // Oriented bias purely as the receiver-threshold push (see
    // SampleEvsmTile); receiverDepth is the true depth so the blocker
    // comparison below is exact.
    float biasAmt = -EvsmOrientedBias(faceNormal, lightDirection, c.bias);
    float receiverDepth = ndc.z;
    const float2 clampedUV = clamp(uvAtlas, c.uvClampMin, c.uvClampMax);
    Texture2D evsmTex = textures[NonUniformResourceIndex(idx)];
    float4 moments = evsmTex.SampleLevel(samplers[samplerId], clampedUV, 0).rgba;
    valid = true;
    const float centerVis = EvsmVisibility(moments, receiverDepth, exponent, biasAmt);

    // Fully-lit center: the disk can only manufacture self-shadow here.
    // On a grazing floor the blocker estimate (-ln(m-)/c, biased toward the
    // nearest texel of the receiver's OWN footprint) sits in front of the
    // receiver, so delta > 0 passes the guard below and the disk taps then
    // compare offset footprints against the unshifted receiverDepth — the
    // downslope taps read shadowed and stipple the lit floor with the
    // rotated-disk pattern. A receiver in a REAL penumbra always has a
    // transitional (< 1) center; a lit one returns without the 8 taps.
    if (centerVis >= 0.999f)
        return centerVis;

    // Solar penumbra width from the moment blocker (tile-UV units).
    const float blocker = EvsmBlockerDepth(moments, exponent);
    const float delta = receiverDepth - blocker;
    // Blocker at/behind the receiver: fully lit. Return the center tap
    // (which is already ~1) instead of running the disk: forcing a minimum
    // disk radius here samples neighbours on FLAT lit surfaces and injects
    // the rotated-disk pattern as moire.
    if (delta <= 1e-5f)
        return centerVis;
    const float softness = c.pad2.y;
    float rUV = delta * c.pad2.x * softness;
    const float texelU = max(c.invShadowSize.x, 1e-6f);
    // Below ~0.5 texel the disk degenerates onto the prefilter footprint:
    // a single (already blurred) tap is the correct, noise-free answer.
    // Forcing a minimum radius was a moire source on flat surfaces.
    if (rUV < 0.5f * texelU)
        return centerVis;
    // Clamp the outer penumbra so the 8 taps don't streak.
    rUV = min(rUV, 12.0f * texelU);

    const float2x2 rot = EvsmRotate2x2(EvsmDiskRotation(pix));
    float sum = 0.0f;
    [unroll]
    for (int t = 0; t < 8; ++t) {
        const float2 off = mul(rot, kEvsmDisk8[t]) * rUV * c.uvScale;
        const float2 tapUV = clamp(uvAtlas + off, c.uvClampMin, c.uvClampMax);
        const float4 m = evsmTex.SampleLevel(samplers[samplerId], tapUV, 0).rgba;
        sum += EvsmVisibility(m, receiverDepth, exponent, biasAmt);
    }
    // True 9-tap average (center + disk): the penumbra gradient comes from
    // the spatially-varying visibility field. The old 50%-center blend kept
    // a hard core and halved the penumbra contrast (weak soft transitions).
    return (centerVis + sum) / 9.0f;
}

// Soft spot tile: the old PcssSpotShadow math with the moment blocker.
// Eye depth from the RH_ZO perspective inversion using the tile's near
// (pad_) and far (splitEnd); penumbra = lightSize * (zRecv - zBlk) / zBlk
// in eye space, projected to tile-UV through the cone half angle; then the
// rotated 8-tap disk of moments (each an independent EVSM eval).
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
    // Oriented bias purely as the receiver-threshold push (see
    // SampleEvsmTile); receiverDepth is the true depth so the eye-space
    // blocker math below is exact.
    float biasAmt = -EvsmOrientedBias(faceNormal, spotDirection, t.bias);
    float receiverDepth = ndc.z;
    const float2 clampedUV = clamp(uvAtlas, t.uvClampMin, t.uvClampMax);
    Texture2D evsmTex = textures[NonUniformResourceIndex(idx)];
    float4 moments = evsmTex.SampleLevel(samplers[samplerId], clampedUV, 0).rgba;
    valid = true;
    const float centerVis = EvsmVisibility(moments, receiverDepth, exponent, biasAmt);

    // Fully-lit center: skip the disk (see SampleEvsmTileSoft — offset taps
    // against the unshifted receiverDepth only manufacture self-shadow on
    // lit surfaces; a real penumbra always has a transitional center).
    if (centerVis >= 0.999f)
        return centerVis;

    // Eye depths (RH_ZO perspective inversion; near = pad_, far = splitEnd).
    const float nearP = max(t.pad_, 1e-3f);
    const float farP = max(t.splitEnd, nearP + 0.01f);
    const float zRecv = farP * nearP / max(farP - ndc.z * (farP - nearP), 1e-4f);
    const float blocker = EvsmBlockerDepth(moments, exponent);
    const float ndcBlocker = clamp(blocker, 0.0f, 0.9999f);
    const float zBlocker = farP * nearP / max(farP - ndcBlocker * (farP - nearP), 1e-4f);
    if (zBlocker >= zRecv)
        return centerVis; // blocker behind/at receiver: fully lit, no disk

    // Penumbra (eye-space similar triangles; lightSize = pad2.x emitting
    // diameter, softness = pad2.y), projected into tile UV through the
    // cone half angle: the tile spans 2*z*tanOuter at eye depth z.
    const float lightSize = max(t.pad2.x, 0.0f);
    const float softness = max(t.pad2.y, 0.0f);
    const float tanOuter = max(tan(min(outerAngle, 1.45f)), 1e-3f);
    float rPenUV = lightSize * softness * (zRecv - zBlocker)
        / max(zBlocker * 2.0f * zRecv * tanOuter, 1e-4f);
    // Below ~0.5 texel the prefiltered center tap is already the soft
    // answer; forcing a minimum disk radius stipples flat lit surfaces
    // (moire). Clamp only the outer extent to avoid streaking.
    const float tileRes = 1.0f / max(t.invShadowSize.x, 1e-6f);
    if (rPenUV < 0.5f / tileRes)
        return centerVis;
    rPenUV = min(rPenUV, 14.0f / tileRes);

    const float2x2 rot = EvsmRotate2x2(EvsmDiskRotation(pix));
    float sum = 0.0f;
    [unroll]
    for (int k = 0; k < 8; ++k) {
        const float2 off = mul(rot, kEvsmDisk8[k]) * rPenUV * t.uvScale;
        const float2 tapUV = clamp(uvAtlas + off, t.uvClampMin, t.uvClampMax);
        const float4 m = evsmTex.SampleLevel(samplers[samplerId], tapUV, 0).rgba;
        sum += EvsmVisibility(m, receiverDepth, exponent, biasAmt);
    }
    return (centerVis + sum) / 9.0f;
}
