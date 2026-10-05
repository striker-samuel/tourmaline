#pragma once
// Convex GI volumes (Engine/GI/cubemap_volume.h): shaped local probe
// grids cooked by sparse cubemap-reprojection bakes, blended over the
// world field by ordered priority compositing.
//
// Blend model (TPP/HZD/BO3 lineage — volumes blend, grids never
// cross-interpolate):
//
//   E = SampleGlobalOctree(P, N)      // or the sky fallback
//   for (v : volumesSortedLowToHighPriority)
//       E = lerp(E, SampleVolume(v, P, N), v.Weight(P))
//
// Per-face fades make a doorway transition gradual while walls stay
// tight (FOX innerScales): the weight is the MIN over planes of a
// smoothstep across the plane (nearest face dominates, so a wide
// doorway fade survives next to a tight floor).
//
// COMPOSITING DIRECTION (perf): the ordered chain is evaluated
// HIGH->LOW priority with transmittance factors instead of the
// lerp chain low->high — algebraically the same blend,
//
//   E = sum_v irr(V_v) * w_v * PI_{u>v}(1-w_u)  +  base * PI_v(1-w_v)
//
// but a volume with weight 1 (deep inside every fade) zeroes the
// transmittance and the walk BREAKS before the world-field gather:
// the deep-inside pixel pays its 7 texture taps and nothing else. The
// old form ran a full per-pixel pre-scan of every volume's planes
// hunting a weight==1 "full owner", then the blend pass walked the
// volumes AGAIN (plus the octree for every non-owner pixel) — the
// profiler showed the bookkeeping outcosting the 3D texture fetches.
// Irradiance / specular / the fog mean are all linear in the lobe
// amplitudes, so accumulating blended AMPS and evaluating the lobe
// math once per pixel matches the per-volume lerps exactly.
//
// Runtime per volume: 7 hardware-trilinear RGBA16F 3D reads through the
// bindless volumeTextures mirror (samplers[2] = linear clamp) — the
// same GPU layout and unpack as the regular grids (SgRegularUnpack).
//
// GPU data (sg_lightmap_types.hlsli): 80 B headers (fade-expanded AABB
// for broad-phase rejection + the CPU-precomputed world->uvw transform
// + the plane span) and a shared packed plane pool (32 B/plane,
// fade scale/bias precomputed — no per-plane division, no 560 B
// aggregate copies). The tile cull (forwardplus.cs.hlsl) additionally
// writes a per-tile volume bitmask (frame.cvolTileMasks) so pixels only
// walk volumes that can reach their tile.
//
// CPU parity: the PACKED form is algebraically equal to CvPlaneWeight /
// CvVolumeDef::Weight (cubemap_volume.h): min-of-smoothsteps ==
// smoothstep-of-min (the cubic is monotone on [0,1]) and
// saturate(inside * fadeScale + fadeBias) == the old
// saturate((inside + outsideFade) / span). Degenerate spans
// (insideFade + outsideFade < 1e-5) pack as a 1e-5 m ramp — the
// authored CPU step branch (CvSmoothStep) collapses to the same values
// below any content-relevant epsilon; no authored content hits it.

// Volume weight at a world position: min over planes of the normalized
// fade position, ONE cubic at the end — smoothstep is monotone
// increasing on [0,1], so
//     min(smoothstep(t_0), ..., smoothstep(t_n))
//  == smoothstep(min(t_0, ..., t_n))
// exactly (six per-plane cubics collapsed into one). The header's
// fade-expanded AABB rejects far-off volumes before a single plane slot
// is fetched (the tile mask above it rejects whole volumes per TILE).
float SgCvWeight(FrameUniforms frame, CvVolumeGpu v, float3 pos)
{
    // Broad-phase: the fade-expanded hull AABB (the CPU packer grew the
    // generous Bounds() box by the widest outsideFade per axis — a
    // superset of the weight>0 region, so a reject here is always exact).
    if (any(pos < v.aabbMin.xyz) || any(pos > v.aabbMax.xyz))
        return 0.0;
    const uint count = min(v.planeCountBase.x, (uint)SG_CV_MAX_PLANES);
    float minT = 1.0;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const CvPlaneGpu p =
            frame.cvolVolumes.Get().planes[v.planeCountBase.y + i];
        // Signed distance INSIDE the plane; the normalized fade ramp
        // via the CPU-packed scale/bias (no per-plane division). Early
        // out: once one plane is fully outside, the min cannot recover.
        const float inside = -(dot(p.equation.xyz, pos) + p.equation.w);
        const float t = saturate(inside * p.fadeScaleBias.x + p.fadeScaleBias.y);
        minT = min(minT, t);
        if (minT <= 0.0)
            return 0.0;
    }
    return minT * minT * (3.0 - 2.0 * minT);
}

// Per-tile volume bitmask for a pixel (see CvVolumeTileMaskBuffer).
// Returns the all-volumes mask when no tile cull refreshed the masks
// for this view/frame (cvolTileCountX == 0 — the app publishes address
// + grid together, zeros together) or the pixel lands outside the tile
// grid: the mask is only ever allowed to NARROW the candidate set
// (correctness lives in the per-pixel AABB + plane tests of
// SgCvWeight), never invent or drop volumes on its own.
uint SgCvTileMask(FrameUniforms frame, float2 pix)
{
    const uint volCount = min(frame.cvolCount, (uint)SG_CV_MAX_VOLUMES);
    const uint all = (1u << volCount) - 1u; // volCount <= 8 -> fits
    if (frame.cvolTileCountX == 0u)
        return all;
    const uint ts = max(frame.cvolTileSize, 1u);
    const uint tx = (uint)(pix.x / (float)ts);
    if (tx >= frame.cvolTileCountX)
        return all;
    const uint ty = (uint)(pix.y / (float)ts);
    const uint tileIdx = ty * frame.cvolTileCountX + tx;
    if (tileIdx >= (uint)MAX_FP_TILES)
        return all;
    return frame.cvolTileMasks.Get().masks[tileIdx] & all;
}

// Samples one volume's dense grid at world pos: 7 hardware-trilinear
// 3D reads + the shared 27-half unpack. uvw = saturate(pos * uvwScale +
// uvwBias) — the CPU packed the lattice transform (dims/spacing/origin
// are load-time invariants; the shader used to rebuild them per tap).
// The layer index rides a wave-UNIFORM loop counter here (never a
// per-pixel value like the old full-owner index), so the plain bindless
// index keeps the descriptor fetch scalar — no NonUniformResourceIndex
// divergence on the taps.
void SgCvSampleVolume(FrameUniforms frame, uint vol, float3 pos,
    out float3 amps[SG_GI_LOBES])
{
    const CvVolumeGpu v = frame.cvolVolumes.Get().volumes[vol];
    const float3 uvw = saturate(pos * v.uvwScale.xyz + v.uvwBias.xyz);
    float4 layers[SG_CV_LAYERS];
    [unroll]
    for (uint t = 0; t < SG_CV_LAYERS; ++t)
    {
        layers[t] = volumeTextures[frame.cvolLayers[vol * 7u + t]]
            .Sample(samplers[2], uvw);
    }
    SgRegularUnpack(layers, amps);
}

// ---- Volumetric mean path (the fog twin of the sampling/compositing
// above): the fog consumes only the isotropic mean, which is linear in
// the lobe amps (VolGiMean = 0.124959 * Σ amps), so the mean rides a
// CPU-BAKED per-volume RGB texture (CvVolume::PackGpuMeanLayer —
// trilerp(Σ amps) == Σ trilerp(amps), packed from the same coeffs) and
// the walk never materializes the SG lobe arrays: ONE trilinear fetch
// per volume instead of 7 + the 27-half unpack + the 9-lobe blend —
// the surface representation (54 coefficient floats alive per lookup)
// was pure register-pressure poison at up to vp.steps lookups per
// pixel. Same header/AABB/plane weighting as SgCvWeight. ----

// Samples one volume's baked mean texture at world pos (one trilinear
// RGB fetch; uvw from the same packed header transform).
void SgCvSampleVolumeMean(FrameUniforms frame, uint vol, float3 pos,
    out float3 mean)
{
    const CvVolumeGpu v = frame.cvolVolumes.Get().volumes[vol];
    const float3 uvw = saturate(pos * v.uvwScale.xyz + v.uvwBias.xyz);
    mean = volumeTextures[frame.cvolMeanLayers[vol]]
        .Sample(samplers[2], uvw).rgb;
}

// Mean twin of SgCvComposite (the banner math, one mean fetch per
// volume): outMean = the blended volume mean
// (Σ mean_v * w_v * Π_{higher}(1-w_u)); outTransmission = the leftover
// base weight (0 when a deep-inside volume fully owns the position —
// the caller skips the octree gather entirely). volMask 0 (tile cull
// bypass / no volumes reaching the tile / the fog-GI quality bypass)
// walks nothing.
void SgCvCompositeMean(FrameUniforms frame, uint volMask, float3 pos,
    out float3 mean, out float transmission)
{
    mean = float3(0.0, 0.0, 0.0);
    transmission = 1.0;
    if (frame.cvolEnabled == 0u)
        return;
    const uint volCount = min(frame.cvolCount, (uint)SG_CV_MAX_VOLUMES);
    [loop]
    for (int v = int(volCount) - 1; v >= 0; --v) // HIGH priority first
    {
        if ((volMask & (1u << uint(v))) == 0u)
            continue;
        const CvVolumeGpu vol = frame.cvolVolumes.Get().volumes[uint(v)];
        const float w = SgCvWeight(frame, vol, pos);
        [branch]
        if (w <= 0.0)
            continue;
        float3 vMean;
        SgCvSampleVolumeMean(frame, uint(v), pos, vMean);
        mean += vMean * (transmission * w);
        transmission *= 1.0 - w;
        if (transmission <= 0.0)
            break; // deep inside: lower priorities + base contribute 0
    }
}

// Ordered high->low-priority transmittance compositing (the banner
// math). outAmps returns the blended lobe amplitudes
// (sum_v amps_v * w_v * PI_{higher}(1-w_u)); outTransmission the
// leftover base weight (0 when a deep-inside volume fully owns the
// pixel — the caller skips the octree/sky gather entirely);
// outHit whether any volume reached the pixel at all. volMask comes
// from SgCvTileMask (hoistable per pixel; the volumetric march reuses
// one mask for all its segments).
void SgCvComposite(FrameUniforms frame, uint volMask, float3 pos,
    out float3 amps[SG_GI_LOBES], out float transmission, out bool hit)
{
    [unroll]
    for (uint l = 0; l < SG_GI_LOBES; ++l)
        amps[l] = float3(0.0, 0.0, 0.0);
    transmission = 1.0;
    hit = false;
    if (frame.cvolEnabled == 0u)
        return;
    const uint volCount = min(frame.cvolCount, (uint)SG_CV_MAX_VOLUMES);
    [loop]
    for (int v = int(volCount) - 1; v >= 0; --v) // HIGH priority first
    {
        if ((volMask & (1u << uint(v))) == 0u)
            continue;
        const CvVolumeGpu vol = frame.cvolVolumes.Get().volumes[uint(v)];
        const float w = SgCvWeight(frame, vol, pos);
        [branch]
        if (w <= 0.0)
            continue;
        float3 vAmps[SG_GI_LOBES];
        SgCvSampleVolume(frame, uint(v), pos, vAmps);
        const float c = transmission * w;
        [unroll]
        for (uint l = 0; l < SG_GI_LOBES; ++l)
            amps[l] += vAmps[l] * c;
        hit = true;
        transmission *= 1.0 - w;
        if (transmission <= 0.0)
            break; // deep inside: lower priorities + base contribute 0
    }
}
