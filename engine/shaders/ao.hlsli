#pragma once
// Shared SSAO helpers (raw pass, blur pass, and the lighting-pass consumer).
//
// Conventions (must match Engine/Rendergraph/ssao.h + forwardplus.cs.hlsl):
//   - infinite reversed-Z projection: depth 1 = near, 0 = far/sky (clear 0)
//   - view distance (-z) = near / max(d, eps)
//   - background classification: d <= kAoBackgroundEps (never a second epsilon)
//
// The invariant this header enforces everywhere: a background pixel can
// never receive AO. Depth is the authority; color is irrelevant.

static const float kAoBackgroundEps = 1e-6;
static const float kAoSkyDist = 1e6; // sentinel view distance for sky taps

bool AoIsBackground(float ndcDepth)
{
    return ndcDepth <= kAoBackgroundEps;
}

float AoViewDist(float ndcDepth, float nearPlane)
{
    // Infinite reversed-Z: view distance (-z) = near / d.
    return nearPlane / max(ndcDepth, kAoBackgroundEps);
}

// Relative-depth edge weight: 1 on the same surface, ~0 across a
// discontinuity. A geometry|sky step collapses to exp(-sharpness) at worst
// because sky taps carry the kAoSkyDist sentinel.
float AoEdgeWeight(float tapDist, float centerDist, float sharpness)
{
    float depthDiff = abs(tapDist - centerDist) / max(centerDist, 1e-3f);
    return exp(-depthDiff * max(sharpness, 0.0f));
}

// Depth-guided bilateral upsample of half-res AO to one full-res pixel.
//   aoTex      : half-res blurred AO (R8)
//   depthTex   : full-res prepass depth (D32; .Load only, no sampler)
//   fullPx     : full-res pixel coords (depth extent == pass extent)
//   halfExtent : half-res AO extent (max(1, floor(full * 0.5)))
//   nearPlane  : frame near plane (pre-clamped by the caller)
//   sharpness  : edge weight (SSAO blur sharpness knob)
// Background full-res depth returns 1.0 unconditionally; geometry pixels
// take the 2x2 nearest half-res AO taps weighted by AoEdgeWeight against
// the pixel's own depth, so sky-side AO (raw = 1.0 there) can never bleed
// across a silhouette. If every candidate tap disagrees in depth (all-sky
// neighborhood), the pixel stays unoccluded.
float AoUpsample(Texture2D aoTex, Texture2D depthTex,
    int2 fullPx, float2 halfExtent, float nearPlane, float sharpness)
{
    float d = depthTex.Load(int3(fullPx, 0)).r;
    if (AoIsBackground(d))
        return 1.0f;
    float centerDist = AoViewDist(d, nearPlane);

    // 2x2 nearest half-res texels around this full-res pixel.
    int2 halfMin = int2(floor((float2(fullPx) - 1.0f) * 0.5f));
    int2 halfMax = int2(halfExtent) - 1;
    float acc = 0.0f;
    float wSum = 0.0f;
    for (int ty = 0; ty < 2; ++ty)
    {
        for (int tx = 0; tx < 2; ++tx)
        {
            int2 hp = clamp(halfMin + int2(tx, ty), int2(0, 0), halfMax);
            float ao = aoTex.Load(int3(hp, 0)).r;
            // Representative depth of the tapped block: its corner full-res
            // texel. Sky/mixed blocks carry the sentinel distance -> the
            // edge weight kills them at silhouettes.
            float bd = depthTex.Load(int3(hp * 2, 0)).r;
            float bDist = AoIsBackground(bd) ? kAoSkyDist : AoViewDist(bd, nearPlane);
            float w = AoEdgeWeight(bDist, centerDist, sharpness);
            acc += ao * w;
            wSum += w;
        }
    }
    return (wSum > 1e-4f) ? acc / wSum : 1.0f;
}
