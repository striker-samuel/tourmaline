#pragma once
// Shared depth-of-field helpers (low single-pass + HQ CoC/bokeh).
//
// Conventions (must match ssao.h / ao.hlsli):
//   - reversed-Z projection, clear 0 (far), GreaterEqual.
//   - view positions unprojected EXACTLY via FrameUniforms.invProj
//     (flip / finite-far agnostic), never via near/d formulas.
//   - background classification: d <= 1e-6 (same epsilon as ao.hlsli).
//   - background view distance sentinel: 1e5 m (far plane scale), so the
//     CoC treats sky as far-blur-max instead of smearing toward the camera.

static const float kDofBackgroundEps = 1e-6;
static const float kDofSkyDist = 1e5;

bool DofIsBackground(float ndcDepth)
{
    return ndcDepth <= kDofBackgroundEps;
}

// NaN-safe background test for depth CONSUMERS (SSR trace/blur/compose,
// volumetric fog): true for sky (reverse-Z ~0) AND for garbage depth
// texels — NaN (degenerate vertex z written by the rasterizer) or
// out-of-range values — that would poison marches and unprojections.
// Valid reversed-Z depth is (eps, 1]: near = 1, far/sky = 0. NaN fails
// both range comparisons, so garbage texels classify as background
// instead of marching as phantom geometry.
bool DofIsBackgroundOrGarbage(float ndcDepth)
{
    return !(ndcDepth > kDofBackgroundEps && ndcDepth <= 1.0);
}

// NDC -> view space (exact, flip/reversed-Z agnostic; mirrors ssao.frag).
float3 DofUnprojectView(float2 ndcXY, float ndcZ, float4x4 invProj)
{
    float4 v = mul(invProj, float4(ndcXY, ndcZ, 1.0f));
    return v.xyz / max(v.w, 1e-9f);
}

// View distance (-z) of a full-res depth tap. Background OR garbage
// depth -> sky sentinel (garbage must never reach the unprojection:
// NaN depths would produce NaN distances and corrupt every consumer).
float DofViewDist(float ndcDepth, float2 ndcXY, float4x4 invProj)
{
    if (DofIsBackgroundOrGarbage(ndcDepth))
        return kDofSkyDist;
    return max(-DofUnprojectView(ndcXY, ndcDepth, invProj).z, 1e-3f);
}

// 0..1 blur amount: 0 inside [focus-range, focus+range], ramping to 1 at
// ~1.5x focus distance away on either side (artist-friendly thin-lens fit).
float DofCoc01(float dist, float focusDist, float focusRange)
{
    float denom = max(focusDist * 0.5f + focusRange, 1e-3f);
    return saturate((abs(dist - focusDist) - focusRange) / denom);
}

// Packed signed CoC: 0.5 in focus, 0 near-blur max, 1 far-blur/sky max.
float DofPackCoc(float dist, float focusDist, float focusRange)
{
    float c = DofCoc01(dist, focusDist, focusRange);
    float s = (dist < focusDist) ? -1.0f : 1.0f;
    return saturate(0.5f + 0.5f * s * c);
}

float DofUnpackRadius01(float packedCoc)
{
    return saturate(abs(packedCoc - 0.5f) * 2.0f);
}

// Hexagonal aperture outline radius for angle theta (bokeh disc shaping):
// r_hex(t) = cos(30deg) / cos((t mod 60deg) - 30deg), in [0.866, 1].
float DofHexRadius(float theta)
{
    float sector = fmod(theta, 1.0471976f) - 0.5235988f; // pi/3, pi/6
    return 0.8660254f / max(cos(sector), 1e-3f);
}
