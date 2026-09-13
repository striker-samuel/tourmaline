#pragma once
// EVSM (Exponential Variance Shadow Maps) shared helpers.
//
// Moments (RG32F, see Rendergraph/evsm.h):
//   R = E[exp(+c * d)], G = E[exp(-c * d)], d = light-space depth in [0,1].
// The MSAA moment pass writes the warped depth per sample; the async
// prefilter (evsm_blur.cs.hlsl, separable Gaussian H+V on the compute queue)
// produces the filtered expectations. Visibility is the positive-channel
// ESM ratio vp = E[exp(c*d_occ)] * exp(-c*d_recv) (smooth falloff of width
// ~1/c — filterable, no Chebyshev step, soft penumbrae without moire),
// plus a far-plane gate for the clear color (see EvsmVisibility). The
// dual-channel min(vp, vn) of the negative first moment is NOT a
// visibility (see the EVSM_DUAL_CHANNEL note below) and is opt-in.
//
// c = kEvsmExponent (42): exp(42) ~ 1.7e18 fits fp32 with headroom;
// exp(-42) ~ 5.7e-19 stays normal. Larger c tightens bleeding but pushes
// the positive channel toward overflow under blur overshoot.

// Default warp coefficient. Must match C++ smvk::kEvsmExponent.
#ifndef EVSM_EXPONENT
#define EVSM_EXPONENT 42.0f
#endif

// Dual-channel visibility (min of the positive and negative ESM ratios).
// DEFAULT OFF. vn = E[exp(-c*d_occ)] * exp(+c*d_recv) is an upper bound on
// the SHADOW mass (how much of the blurred footprint sits in front of the
// receiver), not a visibility: it reads < 1 whenever the footprint holds
// mass DEEPER than the receiver. In a min() that darkened genuinely lit
// receivers — every contact edge (footprint mixes receiver + deeper
// geometry), every coverage boundary (footprint mixes receiver + the
// far-occluder clear value), and everywhere under perspective spots (depth
// is nonlinear in UV, so the blur footprint is depth-asymmetric). Combined
// with vp the pair had zero tolerance in either depth direction, which is
// the "heavy acne" signature. vp alone is the reference ESM: one-sided
// tolerance (map deeper than receiver = lit), same direction as the depth
// bias, with EVSM_BLEED_REDUCTION tightening the leak toe. Flip to 1 only
// to A/B the penumbra-into-deep-background leak control it provides.
#ifndef EVSM_DUAL_CHANNEL
#define EVSM_DUAL_CHANNEL 0
#endif

// Far-plane gate: a blurred footprint still at >= this fraction of the
// clear value's positive moment (exp(c), the "occluder at d=1" bake) holds
// no in-range caster -> fully lit. 0.98 leaves headroom for fp32
// resolve/blur rounding of identical taps while staying above anything a
// near clip-plane caster can produce (the zPadding slack means no real
// geometry lives in the gated slice of the depth range).
#ifndef EVSM_CLEAR_GATE
#define EVSM_CLEAR_GATE 0.98f
#endif

// Warp a [0,1] depth into the two positive exponential channels.
float2 EvsmWarp(float depth, float exponent)
{
    float d = clamp(depth, 0.0f, 1.0f);
    return float2(exp(exponent * d), exp(-exponent * d));
}

// ESM visibility over the filtered warped moments:
//   moments.x = E[exp(+c*d_occ)], moments.y = E[exp(-c*d_occ)].
// Visibility is the positive ESM ratio with a smooth exponential falloff
// of width ~1/c (filterable, no high-frequency step):
//   vp = E[exp(+c*d_occ)] * exp(-c*d_recv) = moments.x / wT.x
// (see EVSM_DUAL_CHANNEL above for why the negative ratio is opt-in).
// NOTE: the previous code used a single-moment "Chebyshev" bound with a
// faked variance (occl^2 * 1e-4). That bound is ~100x sharper than 1/c
// (width ~1/(100*c)): any sub-texel filtering/precision jitter flips the
// result binary, which reads as heavy moire, and the penumbra never forms
// (no soft transition). The ESM ratio restores the designed soft falloff.
#ifndef EVSM_BLEED_REDUCTION
#define EVSM_BLEED_REDUCTION 0.15f
#endif

// Oriented bias in warp space (boost-only; do NOT also subtract it from
// receiverDepth — the old code did both, netting e^{c*b*(S+1)} on vp and
// e^{c*b*(S-1)} on vn instead of the intended symmetric e^{c*b*S}).
// Multiplying both ratios by e^{+c*b*S} is equivalent to comparing the
// positive channel against dr - b*S (and the negative, when dual is on,
// against dr + b*S): a slope-scaled NDC push b amplified by S so the tiny
// PCF-sized bake (b ~ 1e-4 NDC vs the ESM falloff width 1/c ~ 0.024 NDC)
// still produces a visible lift. Clamped so a grazing receiver can't
// wash into peter-panning.
#ifndef EVSM_BIAS_SCALE
#define EVSM_BIAS_SCALE 4.0f
#endif
#ifndef EVSM_BOOST_MAX
#define EVSM_BOOST_MAX 4.0f
#endif
float EvsmWarpBoost(float exponent, float biasNDC)
{
    return clamp(exp(exponent * max(biasNDC, 0.0f) * EVSM_BIAS_SCALE), 1.0f, EVSM_BOOST_MAX);
}

float EvsmVisibility(float2 moments, float receiverDepth, float exponent, float warpBoost)
{
    // Sanitize blurred moments (see evsm_blur.cs.hlsl floors): the negative
    // channel legitimately spans e^-c .. 1, so the floor must stay ~e^-c.
    moments = clamp(moments, float2(1e-6f, 1e-20f), float2(3.0e37f, 1.0f));
    // Far-plane gate: the moment clear value bakes "occluder at d=1"
    // (EvsmClearColor). Any texel that never received a caster — open tile
    // area, casters clipped by the ortho z-range, MSAA-split edge texels
    // averaging clear + geometry — must read LIT (matching PCF's depth-1
    // clear), not as a solid occluder at the far plane. Gate on the
    // positive moment: only a ~fully-clear blurred footprint reaches
    // e^{+c}; a real caster anywhere in the window drops it far below.
    if (moments.x >= EVSM_CLEAR_GATE * exp(exponent))
        return 1.0f;
    float d = clamp(receiverDepth, 0.0f, 1.0f);
    float2 wT = EvsmWarp(d, exponent);
    // ESM ratio. Division form keeps the quotient O(1): both numerator and
    // denominator can be ~1e18 (c=42) but their ratio is the visibility.
    // warpBoost applies the oriented bias in warp space (see above).
    float vp = moments.x / max(wT.x, 1e-30f) * warpBoost;
#if EVSM_DUAL_CHANNEL
    float vn = moments.y / max(wT.y, 1e-30f) * warpBoost;
    float v = min(vp, vn);
#else
    float v = vp;
#endif
    // Light-bleeding reduction: rescale so weak bleed-through (v < bleed)
    // maps to 0 while fully-lit (>= 1) stays 1. Keeps the smooth falloff
    // shape, just tightens the toe.
    v = saturate((v - EVSM_BLEED_REDUCTION) / max(1.0f - EVSM_BLEED_REDUCTION, 1e-4f));
    return saturate(v);
}

// Clear color for the MSAA moment target, matching far-plane depth d=1:
// (exp(c), exp(-c)). Resolved uncleared texels read as "far occluder" for
// the blocker estimate (EvsmBlockerDepth) but are gated back to lit in
// EvsmVisibility (see the far-plane gate there).
float4 EvsmClearColor(float exponent)
{
    float2 w = EvsmWarp(1.0f, exponent);
    return float4(w.x, w.y, 0.0f, 1.0f);
}

float EvsmOrientedBias(
    float3 faceNormal,
    float3 lightDirection,
    float baseBiasNDC)
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

//// slope-scaled ndc bias in depth domain (same math as getorientedbias in
//// shadows.hlsli, duplicated here so this header stays self-contained for
//// standalone tus like shadow_evsm.frag.hlsl that never include shadows).
//float evsmorientedbias(float3 facenormal, float3 lightdirection, float basebiasndc)
//{
//    float ndotl = saturate(dot(facenormal, -lightdirection));
//    float slope = 1.0f - ndotl;
//    return -basebiasndc * (0.3f + 0.7f * slope);
//}

// Sample one EVSM cascade/spot tile. Mirrors SampleCascadeTile (shadows.hlsli)
// but reads the blurred RG32F moments with a linear sampler and applies the
// oriented bias in WARP space (EvsmWarpBoost lifts the ESM ratio by
// e^{c*b*S}, the warp-domain equivalent of a depth-domain push — applying
// it in depth domain AND as a boost double-counted it).
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
    // Oriented bias acts purely in warp space via EvsmWarpBoost (see the
    // EvsmWarpBoost note: subtracting it from receiverDepth as well used to
    // double-apply it). receiverDepth stays the TRUE light-space depth so
    // the soft-path blocker comparison (receiverDepth - blocker) is exact.
    float biasAmt = -EvsmOrientedBias(faceNormal, lightDirection, c.bias);
    float receiverDepth = ndc.z;
    float warpBoost = EvsmWarpBoost(exponent, biasAmt);
    float2 clampedUV = clamp(uvAtlas, c.uvClampMin, c.uvClampMax);
    float2 moments = textures[NonUniformResourceIndex(idx)]
        .SampleLevel(samplers[samplerId], clampedUV, 0).rg;
    valid = true;
    return EvsmVisibility(moments, receiverDepth, exponent, warpBoost);
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
    // Oriented bias purely in warp space (see SampleEvsmTile); receiverDepth
    // is the true depth so the blocker comparison below is exact.
    float biasAmt = -EvsmOrientedBias(faceNormal, lightDirection, c.bias);
    float receiverDepth = ndc.z;
    float warpBoost = EvsmWarpBoost(exponent, biasAmt);
    const float2 clampedUV = clamp(uvAtlas, c.uvClampMin, c.uvClampMax);
    Texture2D evsmTex = textures[NonUniformResourceIndex(idx)];
    float2 moments = evsmTex.SampleLevel(samplers[samplerId], clampedUV, 0).rg;
    valid = true;
    const float centerVis = EvsmVisibility(moments, receiverDepth, exponent, warpBoost);

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
        const float2 m = evsmTex.SampleLevel(samplers[samplerId], tapUV, 0).rg;
        sum += EvsmVisibility(m, receiverDepth, exponent, warpBoost);
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
// rotated 8-tap disk of moments (each an independent ESM eval).
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
    // Oriented bias purely in warp space (see SampleEvsmTile); receiverDepth
    // is the true depth so the eye-space blocker math below is exact.
    float biasAmt = -EvsmOrientedBias(faceNormal, spotDirection, t.bias);
    float receiverDepth = ndc.z;
    float warpBoost = EvsmWarpBoost(exponent, biasAmt);
    const float2 clampedUV = clamp(uvAtlas, t.uvClampMin, t.uvClampMax);
    Texture2D evsmTex = textures[NonUniformResourceIndex(idx)];
    float2 moments = evsmTex.SampleLevel(samplers[samplerId], clampedUV, 0).rg;
    valid = true;
    const float centerVis = EvsmVisibility(moments, receiverDepth, exponent, warpBoost);

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
        const float2 m = evsmTex.SampleLevel(samplers[samplerId], tapUV, 0).rg;
        sum += EvsmVisibility(m, receiverDepth, exponent, warpBoost);
    }
    return (centerVis + sum) / 9.0f;
}
