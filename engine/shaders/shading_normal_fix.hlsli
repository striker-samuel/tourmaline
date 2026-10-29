// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// JHFAA-INSPIRED SHADING-NORMAL VALIDITY CORRECTION (experimental).
//
// Johan Hammes' JHFAA targets a SOURCE of specular aliasing rather than
// filtering its symptoms post-hoc: the interpolated (smooth-shading)
// normal — further perturbed by normal maps — does not represent
// physically valid geometry at silhouette pixels. On a coarsely
// tessellated smooth-shaded mesh the interpolated normal can point AWAY
// from the camera while its triangle is still front-facing, and the
// reflection vector it produces can point THROUGH the polygon (below the
// geometric tangent plane), sampling bright unrelated environment
// radiance that sparkles as the camera or the geometry subpixel-shifts.
// Raising roughness or damping specular intensity does not fix the
// underlying DIRECTION — the lobe keeps pointing at the invalid sample.
//
// No public reference implementation exists (Hammes' complete
// normal-correction mathematics live in a document prepared for Epic and
// shared privately), so this file is a practical correction in the same
// SPIRIT, not a reproduction. Two validity conditions are enforced:
//
//   A. the shading normal faces the viewer (NoV above a small epsilon),
//   B. the reflection vector stays above the geometric face's tangent
//      plane ("the geometric horizon" — the actual rasterized triangle's
//      plane, NOT the interpolated vertex normal: the two are exactly
//      what disagree at a silhouette).
//
// Condition B is enforced on the REFLECTION direction and the normal is
// then RECONSTRUCTED from the half-vector identity
//
//     N = normalize(V + R)
//
// which round-trips exactly (reflect(-V, normalize(V+R)) == R for unit
// V, R), so ONE corrected normal feeds every downstream consumer — the
// sun lobe, the forward+ lights, the env/probe specular vector, the GI
// specular crossovers — and they all agree on a physically plausible
// direction. Simply clamping NoV and continuing would leave R invalid.
//
// Gating: FrameUniforms::flags bits (C++ mirror kFrameFlagNormalFix /
// kFrameFlagNormalFixDebug in WorldRenderer.h; pins TOURMALINE_NORMAL_FIX
// / TOURMALINE_NORMAL_FIX_DEBUG, default off while under evaluation —
// see docs/jhfaa_normal_fix.md for the A/B contract and the test matrix).
//
// Known limitations (deliberate, first-experiment scope):
//   - screen-space derivatives at a silhouette QUAD span the neighboring
//     triangle too (the helper quad straddles the edge); the horizon
//     plane is therefore an approximation for the 1-pixel edge band.
//   - near-dot silhouette pixels (|dot(Ng, V)| ~ 0) have an ambiguous
//     face orientation; the horizon constraint is weak there either way
//     (the plane is nearly edge-on to the view), so the sign flip is
//     harmless in practice.
//   - two-sided / alpha-tested edges and the POM debug view are out of
//     scope for the first experiment (see the doc).
//   - the corrected normal has a kink AT the correction threshold, so
//     SpecularAARoughness's screen-space variance sees a spike there —
//     the kernel clamp (0.20) bounds the roughness bump.

#ifndef SHADING_NORMAL_FIX_HLSLI
#define SHADING_NORMAL_FIX_HLSLI

// Prototype tolerances — NOT published JHFAA parameters (there are none
// publicly). NoV epsilon: small enough to keep grazing Fresnel alive
// (the lobe must still sharpen toward silhouettes), large enough that
// the grazing geometry term does not explode on the corrected normal.
// Horizon epsilon: just above the tangent plane so numerically-edge-on
// reflections are not left straddling it.
#define JHFAA_NOV_EPSILON    (0.001f)
#define JHFAA_HORIZON_EPSILON (0.002f)

// -------------------------------------------------------------------------
// Screen-space geometric face normal.
//
// The rasterized triangle's own plane from position derivatives — the
// "ground truth" macro-surface the shading normal is supposed to
// approximate. Oriented toward the viewer so the horizon test in
// CorrectShadingNormal is well-defined for front-facing geometry (the
// lit family flips the interpolated normal for back faces already; this
// orientation makes the face normal agree with that convention).
//
// fallbackN: the (already double-sided-flipped) interpolated vertex
// normal — used when the derivative plane degenerates (subpixel / helper
// quads). The NaN-reject idiom (!= (x >= lim)) matches the body's other
// guards: NaN comparisons are false, so a poisoned derivative collapses
// to the fallback instead of NaN-ing the HDR chain.
// -------------------------------------------------------------------------
float3 ScreenSpaceFaceNormal(float3 worldPos, float3 V, float3 fallbackN)
{
    float3 dpdx = ddx(worldPos);
    float3 dpdy = ddy(worldPos);
    float3 Ng = cross(dpdx, dpdy);
    float len2 = dot(Ng, Ng);
    if (!(len2 >= 1e-12f))
        return fallbackN;
    Ng = Ng * rsqrt(len2);
    // Orient toward the camera (see the header note on the ~0 band).
    if (dot(Ng, V) < 0.0f)
        Ng = -Ng;
    return Ng;
}

// -------------------------------------------------------------------------
// The correction itself (see the header for the derivation). Inputs may
// be un-normalized; all three are treated as directions. Returns a unit
// normal that satisfies A (NoV >= ~eps) and whose reflection vector
// satisfies B (dot(R, Ng) >= ~eps) whenever the inputs admit one.
// -------------------------------------------------------------------------
float3 CorrectShadingNormal(float3 N, float3 Ng, float3 V)
{
    N = normalize(N);
    Ng = normalize(Ng);
    V = normalize(V);

    // A. Face the viewer. N + a*V with a = eps - NoV lands dot(N', V)
    // exactly at eps by construction; the renormalize handles the length
    // growth (worst case N ~ -V collapses toward V — the only direction
    // a fully away-pointing normal can validly take).
    float NoV = dot(N, V);
    if (NoV < JHFAA_NOV_EPSILON)
        N = normalize(N + (JHFAA_NOV_EPSILON - NoV) * V);

    // B. The geometric horizon. Push the reflection back above the
    // triangle's tangent plane, then recover the shading normal that
    // PRODUCES that corrected direction (the half-vector identity —
    // clamping R alone would desync every other consumer of N).
    float3 R = reflect(-V, N);
    float horizon = dot(R, Ng);
    if (horizon < JHFAA_HORIZON_EPSILON)
    {
        R = normalize(R + (JHFAA_HORIZON_EPSILON - horizon) * Ng);
        float3 H = V + R;
        // H ~ 0 means V ~ -R (the view ray reversed): no meaningful
        // half-vector exists — the face plane itself is the only
        // defensible normal left.
        N = dot(H, H) > 1e-8f ? normalize(H) : Ng;
    }

    return N;
}

// -------------------------------------------------------------------------
// Violation palette for the debug overlay (TOURMALINE_NORMAL_FIX_DEBUG).
// Keys off the UN-corrected normal — the view shows the disease, not the
// medicine, and stays meaningful with the correction on (residue) or off
// (baseline extent):
//   red    = shading normal points away from the camera (NoV < 0)
//   orange = reflection below the geometric horizon (dot(R, Ng) < 0,
//            with or without the red violation — orange wins the pixel
//            when both fire, since the horizon case is the one that
//            actually sources wrong radiance)
//   black  = no violation
// -------------------------------------------------------------------------
float3 ShadingNormalViolationColor(float3 N, float3 Ng, float3 V)
{
    N = normalize(N);
    V = normalize(V);
    float NoV = dot(N, V);
    float3 R = reflect(-V, N);
    float RoNg = dot(R, Ng);
    return float3(
        (NoV < 0.0f || RoNg < 0.0f) ? 1.0f : 0.0f,
        (RoNg < 0.0f) ? 0.45f : 0.0f,
        0.0f);
}

#endif // SHADING_NORMAL_FIX_HLSLI
