#pragma once
// SG-GI probe volume sampling + evaluation. Included after common.hlsli
// (FrameUniforms carries the volume pointers; types in sggi_types.hlsli).
//
// Sampling: for each octree level, find the containing cell, gather the
// cell's probe list and blend amplitudes with a Wendland partition-of-
// unity weight x baked 6-axis visibility. Finer probes dominate near
// geometry; coarse probes fill open space. Must match the CPU reference
// SgGiVolume::Sample (unit tested).

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
// obstacles do not nuke open-air coverage. Per-axis product; SgGiSample
// falls back to distance-only blending when every covering probe is
// visibility-rejected. Mirrors VisibilityFactor in probe_volume.cpp.
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

// Gathers + blends probe amplitudes at a world position. Primary blend:
// visibility-weighted partition of unity (anti-leak). Safety net: when
// every covering probe is visibility-rejected (deep clutter), fall back
// to a kernel-weighted blend of the kNearest COVERING probes (blending
// occluded far-side probes wholesale leaks their dark radiance; the
// nearest few are almost always same-side "own brick" probes, and the
// weighted blend keeps the patch smooth instead of one-probe dots).
// The debug outs feed the F8 diagnostics view (weight sum, contributor
// count, fallback flag; debugBestLevel = level of the strongest wv
// contributor for the dominant-level view). Mirrors SgGiVolume::Sample.
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

    float3 ampsVis[SG_GI_LOBES];
    [unroll]
    for (uint i = 0; i < SG_GI_LOBES; ++i)
        ampsVis[i] = float3(0, 0, 0);
    float weightVis = 0.0;
    // Per-LEVEL weight accumulation (dominant level = argmax over these
    // sums): robust vs "strongest single probe", which is noisy — the
    // question answered is "which octree level carries most of this
    // pixel's interpolation weight".
    float dbgLevelW[SG_GI_MAX_LEVELS];
    [unroll]
    for (uint l = 0; l < SG_GI_MAX_LEVELS; ++l)
        dbgLevelW[l] = 0.0;
    // Fallback bookkeeping: single NEAREST covering probe (idx + invsq
    // weight). The old 4-slot sorted insert cost ~20 ALU per probe for a
    // fallback that fires on ~0% of pixels; one compare is the same
    // information for a blending fallback that has to be smooth anyway
    // via the coverage kernel, and the fallback stays magenta-flagged in
    // the debug views when it does fire.
    float bestNearW = 0.0;
    uint bestNearIdx = 0;
    // HIERARCHICAL walk, finest level first, with an early-out: fine
    // probes SUPERSEDE coarse ones. Once the accumulated partition-of-
    // unity weight is dense enough (>= kGiCoverage), coarser levels are
    // skipped entirely — 1-2 levels walked for the typical pixel instead
    // of all of them (~4x less gather work; the per-level census showed
    // 99% single-level dominance, so pixels that stop early were never
    // meaningfully mixed anyway). Debug modes walk everything (mode 3
    // isolates one level, mode 4 needs every level's weight for argmax).
    [loop]
    for (int lvl = int(levelCount) - 1; lvl >= 0; --lvl)
    {
        // Debug level isolation (mode 3): gather ONLY the selected level
        // so per-level captures show what each octave contributes alone.
        [branch]
        if (dbgMode == 3u && uint(lvl) != dbgLvl)
            continue;
        const SgGiLevel lg = grid.levels[lvl];
        const float3 rel = pos - lg.volumeMin.xyz;
        const float3 cellF = floor(rel / lg.cellSize.x);
        const int3 c = clamp(int3(cellF), int3(0, 0, 0), int3(lg.dims.xyz) - 1);
        const uint cellIdx = uint(c.x)
            + lg.dims.x * (uint(c.y) + lg.dims.y * uint(c.z));
        const uint2 range = frame.giCells.Get().cells[lg.offsets.x + cellIdx];
        // Defensive clamp: a corrupt/oversized cell list would read
        // probe indices out of bounds (driver-fault class). The bake-side
        // cap is 64 probes/cell (kSgGiMaxProbesPerCell).
        const uint listCount = min(range.y, 64u);
        [loop]
        for (uint i = 0; i < listCount; ++i)
        {
            const uint probeIdx = frame.giIndices.Get().index[lg.offsets.y + range.x + i];
            const SgProbeGpu p = frame.giProbes.Get().probes[probeIdx];
            [branch]
            if ((p.vis.w & 1u) == 0u)
                continue; // invalid
            const float w = SgGiProbeWeight(pos, p);
            [branch]
            if (w <= 0.0)
                continue;
            const float3 d = pos - p.posRadius.xyz;
            const float dist2 = dot(d, d);
            // nearest-covering-probe bookkeeping (fallback blend)
            const float wn = 1.0 / max(dist2, 1e-4);
            [branch]
            if (wn > bestNearW)
            {
                bestNearW = wn;
                bestNearIdx = probeIdx;
            }
            const float wv = w * SgGiVisibilityFactor(pos, p);
            [branch]
            if (wv <= 0.0)
                continue;
            // Dominant contributor (mode 4): per-LEVEL weight sums; the
            // argmax after the walk picks the level carrying most of this
            // pixel's interpolation weight (robust vs single-probe max).
            dbgLevelW[lvl] += wv;
            weightVis += wv;
            ++debugContributors;
            [unroll]
            for (uint lobe = 0; lobe < SG_GI_LOBES; ++lobe)
                ampsVis[lobe] += wv * SgGiProbeAmp(p.amp, lobe);
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
    // Argmax over per-level weight sums (mode 4 bookkeeping).
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
    // Fallback: single nearest covering probe (rare: every covering
    // probe was visibility-rejected). Its amps ARE the fallback blend —
    // no list to normalize, and the debug views flag it magenta/violet.
    [branch]
    if (bestNearW > 0.0)
    {
        debugFellBack = true;
        const SgProbeGpu p = frame.giProbes.Get().probes[bestNearIdx];
        [unroll]
        for (uint lobe = 0; lobe < SG_GI_LOBES; ++lobe)
            amps[lobe] = SgGiProbeAmp(p.amp, lobe);
        return true;
    }
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
