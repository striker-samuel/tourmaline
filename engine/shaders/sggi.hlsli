#pragma once
// SG-GI probe volume sampling + evaluation. Included after common.hlsli
// (FrameUniforms carries the volume pointers; types in sggi_types.hlsli).
//
// Sampling (the DENSE path): at load the app resamples every octree level
// into BC6H 3D textures (per level 9 SG-lobe RGB layers + 1 weight layer —
// the per-voxel visibility-weighted partition-of-unity blend, the SAME
// math the CPU reference SgGiVolume::Sample applies per cell, baked once
// because the probes never move). The walk below just trilinear-filters
// those layers per level, finest first, accumulating raw sums + early-out
// at dense coverage — no per-probe weight/visibility/lobe loops, no tile
// lists. Finer probes dominate near geometry; coarse fill open space. The
// reference walk survives as ResampleLevelDense (unit tested).

// Lobe basis value for a direction.
float SgGiBasisValue(uint lobe, float3 dir)
{
    return exp(SG_GI_SHARPNESS * (dot(SG_GI_AXES[lobe], dir) - 1.0));
}

// Closed-form product integral of basis lobe x an external SG.
float SgGiProductIntegral(uint lobe, float otherAmp, float otherSharp, float3 otherAxis)
{
    const float3 sum = SG_GI_SHARPNESS * SG_GI_AXES[lobe] + otherSharp * otherAxis;
    const float lp = length(sum);
    [branch]
    if (lp < 1e-6)
        return otherAmp * 12.566371 * exp(-(SG_GI_SHARPNESS + otherSharp));
    const float amp = otherAmp * exp(lp - SG_GI_SHARPNESS - otherSharp);
    return amp * (6.2831853 / lp) * (1.0 - exp(-2.0 * lp));
}

// fp16 half decode: half index h -> word h>>1, shift (h & 1) * 16.
float SgGiHalfAt(uint4 amp[4], uint h)
{
    const uint w = h >> 1;
    const uint word = amp[w >> 2][w & 3];
    return f16tof32(word >> ((h & 1) * 16));
}

float3 SgGiProbeAmp(uint4 amp[4], uint lobe)
{
    const uint h = lobe * 3;
    return float3(SgGiHalfAt(amp, h), SgGiHalfAt(amp, h + 1), SgGiHalfAt(amp, h + 2));
}

// Partition-of-unity probe weight (C2-smooth Wendland kernel, compact
// support). Mirrors ProbeWeight in probe_volume.cpp.
float SgGiProbeWeight(float3 pos, SgProbeGpu p)
{
    const float3 d = pos - p.posRadius.xyz;
    const float dist2 = dot(d, d);
    const float r2 = p.posRadius.w * p.posRadius.w;
    [branch]
    if (dist2 >= r2)
        return 0.0;
    const float x2 = dist2 / r2;
    const float x2m = 1.0 - x2;
    return x2m * x2m * x2m;
}

// Baked wall awareness: the shading point beyond a probe's recorded
// obstacle on an axis is behind a wall from the probe's viewpoint. The
// recorded distance gets a SLACK (surface points live AT the obstacle)
// and the rejection ramps to zero over a 25%-of-dist margin, so thin
// obstacles do not nuke open-air coverage. Per-axis product; points with
// no visibility-valid covering probe sample as uncovered (no fallback).
// Mirrors VisibilityFactor in probe_volume.cpp.
float SgGiVisibilityFactor(float3 pos, SgProbeGpu p)
{
    static const float3 kAxes[6] = {
        float3( 1, 0, 0), float3(-1, 0, 0),
        float3( 0, 1, 0), float3( 0,-1, 0),
        float3( 0, 0, 1), float3( 0, 0,-1)
    };
    const float slack = clamp(0.35 * p.posRadius.w, 0.05, 0.3);
    const float3 d = pos - p.posRadius.xyz;
    float w = 1.0;
    [unroll]
    for (uint k = 0; k < 6; ++k)
    {
        const float dist = f16tof32(p.vis[k >> 1] >> ((k & 1) * 16));
        [branch]
        if (dist > 0.0)
        {
            const float along = dot(d, kAxes[k]);
            [branch]
            if (along > dist + slack)
            {
                w *= max(0.0, 1.0 - (along - dist - slack) / (0.25 * dist + 0.02));
                [branch]
                if (w <= 0.0)
                    return 0.0;
            }
        }
    }
    return w;
}

// Gathers + blends probe amplitudes at a world position. Primary (and
// only) blend: visibility-weighted partition of unity (anti-leak). When
// no visibility-valid probe covers the point the sample returns false
// (uncovered reads black / magenta in the debug views) — there is NO
// nearest-probe fallback: it hides coverage holes behind leaked bright
// GI from across walls, which is exactly the white-blob failure.
// The debug outs feed the F8 diagnostics views (weight sum, contributor
// count, fallback flag, always false; debugBestLevel = level of the
// strongest wv contributor for the dominant-level view). Mirrors
// SgGiVolume::Sample.
// Debug level isolation: frame.giDebug packs mode (low byte) + level
// (byte 1); mode 3 gathers ONLY that level (level-leak bisect).
bool SgGiSample(FrameUniforms frame, float3 pos, out float3 amps[SG_GI_LOBES],
    out float debugWeightSum, out uint debugContributors, out bool debugFellBack,
    out uint debugBestLevel)
{
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        amps[i] = float3(0, 0, 0);

    debugWeightSum = 0.0;
    debugContributors = 0u;
    debugFellBack = false;
    debugBestLevel = 0u;

    const uint dbgMode = frame.giDebug & 0xffu;
    const uint dbgLvl = (frame.giDebug >> 8) & 0xffu;

    const SgGiGrid grid = frame.giGrid.Get();
    const uint levelCount = min(grid.misc.x, (uint)SG_GI_MAX_LEVELS);
    [branch]
    if (levelCount == 0)
        return false;

    // Debug-only arrays — only allocated when mode 4 is active to reduce
    // register pressure in production (dbgMode == 0) and modes 1-3.
    float dbgLevelW[SG_GI_MAX_LEVELS];
    [branch]
    if (dbgMode == 4u)
    {
        [unroll]
        for (uint l = 0; l < SG_GI_MAX_LEVELS; ++l)
            dbgLevelW[l] = 0.0;
    }

    float weightVis = 0.0;
    float3 ampsVis[SG_GI_LOBES];
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        ampsVis[i] = float3(0, 0, 0);
    // Fallback bookkeeping removed (coverage-test mode): with the
    // visibility-weighted blend finding nothing, returning false exposes
    // the hole (magenta/black) instead of leaking a wall-ignoring probe.
    // HIERARCHICAL walk, finest level first, with an early-out: fine
    // probes SUPERSEDE coarse ones. Once the accumulated partition-of-
    // unity weight is dense enough (>= kGiCoverage), coarser levels are
    // skipped entirely — 1-2 levels walked for the typical pixel instead
    // of all of them (~4x less gather work; the per-level census showed
    // 99% single-level dominance, so pixels that stop early were never
    // meaningfully mixed anyway). Debug modes walk everything (mode 3
    // isolates one level, mode 4 needs every level's weight for argmax).
    // DENSE TRILINEAR LEVELS (the BC6H Texture3D path — cooked payloads,
    // baked by GiBaker because the probes never move): per level, ONE
    // trilinear fetch of the baked amps + weight instead of the per-cell
    // probe-list gather the cook step precomputed. Same transport (the
    // bake evaluated the visibility-weighted partition-of-unity blend
    // per voxel), a fraction of the per-pixel cost — no per-probe
    // weight/visibility/lobe-MAD loops, no tile lists to maintain.
    [loop]
    for (int lvl = int(levelCount) - 1; lvl >= 0; --lvl)
    {
        // Debug level isolation (mode 3): gather ONLY the selected level
        // so per-level captures show what each octave contributes alone.
        [branch]
        if (dbgMode == 3u && uint(lvl) != dbgLvl)
            continue;
        // Absent level (sparse octree / a failed cook bake): the slot
        // stays UINT32_MAX — the level contributes nothing.
        if (frame.giLevelLayers[uint(lvl) * 10u] == 0xFFFFFFFFu)
            continue;
        const SgGiLevel lg = grid.levels[lvl];
        // Cell coordinate -> trilinear uvw over the REAL texture dims
        // (the images are exactly dims on every axis — BC6H blocks are
        // 4x4x1 and partial edge blocks are legal, so nothing is padded
        // anywhere: natural normalized coordinates, no special transform).
        // Alignment: the bake stored the blend evaluated at the cell
        // CENTER (cellF = i + 0.5) into texel i, so a continuous query at
        // cellF interpolates between those stored centers DIRECTLY:
        // uv = cellF/dims. (Adding a half-texel here would double-apply
        // the offset — voxel centers would land on texel EDGES.)
        // Clamp-to-edge sampling matches the walk's clamped cell lookup
        // (beyond-bounds queries read the edge texel, same as the clamp).
        const float3 cellF = (pos - lg.volumeMin.xyz) / lg.cellSize.x;
        const float3 uvw = cellF / float3(lg.dims.xyz);
        const float w_l = volumeTextures[NonUniformResourceIndex(
            frame.giLevelLayers[uint(lvl) * 10u + 9u])].Sample(
            samplers[2], uvw).r;
        // Dominant contributor (mode 4): per-LEVEL weight sums; the
        // argmax after the walk picks the level carrying most of this
        // pixel's interpolation weight (robust vs single-probe max).
        [branch]
        if (dbgMode == 4u)
            dbgLevelW[lvl] += w_l;
        weightVis += w_l;
        ++debugContributors;
        [unroll]
        for (uint lobe = 0; lobe < SG_GI_LOBES; ++lobe)
        {
            const float3 a_l = volumeTextures[NonUniformResourceIndex(
                frame.giLevelLayers[uint(lvl) * 10u + lobe])].Sample(
                samplers[2], uvw).rgb;
            ampsVis[lobe] += a_l;
        }
        // Early-out (production only): fine coverage is dense enough —
        // coarser octaves cannot meaningfully change this pixel. 0.5 with
        // R = spacing kernels (accumulated partition-of-unity sums run
        // ~1-2.5 at covered points; thinner kernels sum lower than the
        // old 1.25x supports, so the old 1.0 threshold rarely fired).
        [branch]
        if (dbgMode == 0u && weightVis >= 0.5)
            break;
    }
    // Argmax over per-level weight sums (mode 4 bookkeeping only — the
    // display consumes debugBestLevel solely in the dominant-level view;
    // never runs in production or on uninitialized dbgLevelW).
    [branch]
    if (dbgMode == 4u)
    {
        float bestW = -1.0;
        uint bestL = 0;
        [unroll]
        for (uint l = 0; l < SG_GI_MAX_LEVELS; ++l) {
            if (dbgLevelW[l] > bestW) {
                bestW = dbgLevelW[l];
                bestL = l;
            }
        }
        debugBestLevel = bestL;
    }
    debugWeightSum = weightVis;
    [branch]
    if (weightVis >= 1e-4)
    {
        const float inv = 1.0 / weightVis;
        [unroll]
        for (uint i = 0; i < SG_GI_LOBES; ++i)
            amps[i] = ampsVis[i] * inv;
        return true;
    }
    // No fallback: uncovered stays uncovered (returns false; the caller
    // keeps sky-IBL ambient and the debug views flag it magenta).
    // debugFellBack is kept as an output (always false) so existing
    // callers and the violet-fallback debug branch keep compiling.
    [unroll]
    for (uint lobe = 0; lobe < SG_GI_LOBES; ++lobe)
        amps[lobe] = float3(0, 0, 0);
    return false;
}

// Clamped-cosine kernel K(t) for a unit basis lobe: degree-6 Chebyshev
// fit (Clenshaw), tail clamped to >= 0. The true kernel is smooth and
// strictly positive on [-1, 1] (fat lobe tails), so no cutoff is needed.
// Mirrors SgCosKernel in Engine/GI/sg.cpp.
float SgGiCosKernel(float t)
{
    t = clamp(t, -1.0, 1.0);
    float b1 = 0.0;
    float b2 = 0.0;
    [unroll]
    for (int k = SG_GI_CHEB_TERMS - 1; k >= 1; --k)
    {
        const float next = 2.0 * t * b1 - b2 + SG_GI_COS_CHEB[k];
        b2 = b1;
        b1 = next;
    }
    const float kernel = SG_GI_COS_CHEB[0] + t * b1 - b2;
    return max(kernel, 0.0);
}

// Diffuse irradiance E(n) = int L(omega) (n.omega)+ domega via the
// precomputed clamped-cosine kernel (radiance units; caller applies
// albedo / PI).
float3 SgGiDiffuseIrradiance(float3 amps[SG_GI_LOBES], float3 n)
{
    float3 sum = float3(0, 0, 0);
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        sum += amps[i] * SgGiCosKernel(dot(SG_GI_AXES[i], n));
    return sum;
}

// Rough GGX specular response: the GGX reflection kernel K(w) = D(h(w))/4
// (view assumed along the normal, the standard ambient-specular
// simplification; integral == 1 for a constant environment), approximated
// by a normalized SG around the reflection vector with sharpness
// 1 / (4 alpha^2) (GGX is Gaussian-ish in tan(H-angle) with sigma = alpha
// and reflection doubles the angle). Each lobe integrates against it
// analytically. Caller supplies Fresnel; blend to the analytic env for
// smooth surfaces.
float3 SgGiSpecular(float3 amps[SG_GI_LOBES], float3 r, float roughness)
{
    const float a = clamp(roughness, 0.04, 1.0);
    const float alpha = a * a;
    const float sharp = 1.0 / (4.0 * alpha * alpha + 1e-4);
    const float norm = sharp / (6.2831853 * (1.0 - exp(-2.0 * sharp)));
    float3 sum = float3(0, 0, 0);
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        sum += amps[i] * SgGiProductIntegral(i, norm, sharp, r);
    return sum;
}
