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
// + the plane span) and a shared packed plane pool (16 B/plane — plane
// equation + fade ramp folded into ONE pre-normalized float4, no
// per-plane division, no 560 B aggregate copies). The tile cull
// (forwardplus.cs.hlsl) additionally writes a per-tile volume bitmask
// (frame.cvolTileMasks) so pixels only walk volumes that can reach
// their tile.
//
// CPU parity: the PACKED form is algebraically equal to CvPlaneWeight /
// CvVolumeDef::Weight (cubemap_volume.h): min-of-smoothsteps ==
// smoothstep-of-min (the cubic is monotone on [0,1]) and
// saturate(dot(weightEquation.xyz, pos) + weightEquation.w) ==
// saturate((inside + outsideFade) / span) with
// weightEquation = (-n * fadeScale, fadeBias - d * fadeScale).
// Degenerate spans (insideFade + outsideFade < 1e-5) pack as a 1e-5 m
// ramp — the authored CPU step branch (CvSmoothStep) collapses to the
// same values below any content-relevant epsilon; no authored content
// hits it.

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
        // One 16 B slot: the folded ramp — one dot3 + FMA + saturate.
        // Early out: once one plane is fully outside, the min cannot
        // recover.
        const float4 p =
            frame.cvolVolumes.Get().planes[v.planeCountBase.y + i].weightEquation;
        const float t = saturate(dot(p.xyz, pos) + p.w);
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

// ---- Fog ray-space volume table (per-pixel prep, SgCvRayPrep) ----
// The march ray is pos(t) = camPos + rayDir * t, so each plane ramp
//   t_p = saturate(dot(p.xyz, pos(t)) + p.w)
//       = saturate(dot(p.xyz, camPos) + p.w + dot(p.xyz, rayDir) * t)
//       = saturate(base_p + slope_p * t)
// with base/slope computed ONCE per pixel per candidate volume: the
// march's per-segment weight test becomes FMA + saturate + min — no
// plane-buffer loads, no dot3s per segment. Volumes whose
// fade-expanded-AABB interval misses the ray never enter the table at
// all (2 interval compares per segment, no loads). Register-bounded:
// the first SG_CV_MAX_RAY_VOLUMES interval-overlapping volumes
// (priority order) ride the table; any excess (pathological-only)
// evaluates per segment straight from the pool, exactly
// (SgCvWeight + one mean fetch).
struct CvRayVolume
{
    float enter;                        // ray interval [enter, exit]
    float exit;                         // (clipped to [0, rayLen])
    uint volume;                        // upload index (priority order)
    uint planeCount;                    // planes in base/slope below
    float base[SG_CV_MAX_PLANES];       // dot(p.xyz, camPos) + p.w
    float slope[SG_CV_MAX_PLANES];      // dot(p.xyz, rayDir)
};

struct CvRayPrep
{
    uint rayCount;                                   // table entries used
    uint tailCount;                                  // pool-fallback entries used
    CvRayVolume rays[SG_CV_MAX_RAY_VOLUMES];        // priority ordered
    uint tailVolumes[SG_CV_MAX_VOLUMES];             // priority ordered
    float tailEnter[SG_CV_MAX_VOLUMES];              // segment skip intervals
    float tailExit[SG_CV_MAX_VOLUMES];
    float3 rayMean[SG_CV_MAX_RAY_VOLUMES];           // held means (lazy fetch)
    bool rayMeanValid[SG_CV_MAX_RAY_VOLUMES];
};

// Per-pixel ray-space prep for the fog march: tile mask -> per-volume
// fade-expanded-AABB slab test vs the march ray (headers only, no plane
// traffic) -> ray-interval table for the overlapping volumes (priority
// order) with their planes transformed to base/slope form. Called ONCE
// per pixel; the march then walks the table, never the pool.
void SgCvRayPrep(FrameUniforms frame, uint volMask, float3 camPos,
    float3 rayDir, float rayLen, out CvRayPrep prep)
{
    prep.rayCount = 0u;
    prep.tailCount = 0u;
    if (frame.cvolEnabled == 0u)
        return;
    const uint volCount = min(frame.cvolCount, (uint)SG_CV_MAX_VOLUMES);
    [loop]
    for (int v = int(volCount) - 1; v >= 0; --v) // HIGH priority first
    {
        if ((volMask & (1u << uint(v))) == 0u)
            continue;
        const CvVolumeGpu vol = frame.cvolVolumes.Get().volumes[uint(v)];
        // Fade-expanded AABB slab test (a superset of the weight>0
        // region — a miss is an exact weight-0 verdict for the whole
        // ray). Division by a ~0 ray component yields ±inf, which the
        // min/max below propagate correctly (parallel axis: no
        // constraint when inside, reject when outside).
        const float3 t0 = (vol.aabbMin.xyz - camPos) / rayDir;
        const float3 t1 = (vol.aabbMax.xyz - camPos) / rayDir;
        const float3 tmin = min(t0, t1);
        const float3 tmax = max(t0, t1);
        const float enter = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
        const float exit = min(min(tmax.x, tmax.y), rayLen);
        if (exit < enter)
            continue; // ray misses the volume: zero weight everywhere
        if (prep.rayCount < (uint)SG_CV_MAX_RAY_VOLUMES)
        {
            const uint s = prep.rayCount;
            prep.rays[s].enter = enter;
            prep.rays[s].exit = exit;
            prep.rays[s].volume = uint(v);
            const uint count = min(vol.planeCountBase.x, (uint)SG_CV_MAX_PLANES);
            prep.rays[s].planeCount = count;
            [loop]
            for (uint p = 0; p < count; ++p)
            {
                const float4 wEq = frame.cvolVolumes.Get()
                    .planes[vol.planeCountBase.y + p].weightEquation;
                prep.rays[s].base[p] = dot(wEq.xyz, camPos) + wEq.w;
                prep.rays[s].slope[p] = dot(wEq.xyz, rayDir);
            }
            prep.rayMeanValid[s] = false;
            prep.rayCount = s + 1u;
        }
        else
        {
            // Pathological-only tail (5th+ overlapping volume): the
            // march evaluates these from the pool per segment, exactly.
            const uint k = prep.tailCount;
            if (k < (uint)SG_CV_MAX_VOLUMES)
            {
                prep.tailVolumes[k] = uint(v);
                prep.tailEnter[k] = enter;
                prep.tailExit[k] = exit;
                prep.tailCount = k + 1u;
            }
        }
    }
}

// Per-segment volume composite from the ray-space table: exact weights
// (FMA-only plane ramps, no loads, no dot3s) over the interval-active
// volumes, plus the pool-fallback tail. Means are fetched LAZILY once
// (the mean texture is static — a held mean stays valid), so a volume
// whose interval goes live between octree refreshes still blends its
// true mean. Same ordered transmittance math as SgCvComposite — the
// result is the composite AT this t with exact weights.
void SgCvRayComposite(FrameUniforms frame, inout CvRayPrep prep, float t,
    float3 wpos, out float3 volAcc, out float trans)
{
    volAcc = float3(0.0, 0.0, 0.0);
    trans = 1.0;
    [loop]
    for (uint s = 0; s < prep.rayCount; ++s)
    {
        if (t < prep.rays[s].enter || t > prep.rays[s].exit)
            continue;
        float minT = 1.0;
        [loop]
        for (uint p = 0; p < prep.rays[s].planeCount; ++p)
        {
            minT = min(minT, saturate(prep.rays[s].base[p]
                + prep.rays[s].slope[p] * t));
            if (minT <= 0.0)
                break;
        }
        const float w = minT * minT * (3.0 - 2.0 * minT);
        [branch]
        if (w <= 0.0)
            continue;
        if (!prep.rayMeanValid[s])
        {
            SgCvSampleVolumeMean(frame, prep.rays[s].volume, wpos,
                prep.rayMean[s]);
            prep.rayMeanValid[s] = true;
        }
        volAcc += prep.rayMean[s] * (trans * w);
        trans *= 1.0 - w;
        if (trans <= 0.0)
            return;
    }
    [loop]
    for (uint k = 0; k < prep.tailCount; ++k)
    {
        if (t < prep.tailEnter[k] || t > prep.tailExit[k])
            continue;
        const CvVolumeGpu vol =
            frame.cvolVolumes.Get().volumes[prep.tailVolumes[k]];
        const float w = SgCvWeight(frame, vol, wpos);
        [branch]
        if (w <= 0.0)
            continue;
        float3 vMean;
        SgCvSampleVolumeMean(frame, prep.tailVolumes[k], wpos, vMean);
        volAcc += vMean * (trans * w);
        trans *= 1.0 - w;
        if (trans <= 0.0)
            return;
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
