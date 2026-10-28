// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GI volumes (Engine/GI/cubemap_volume.h): shaped local probe
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
// (frame.giVolumeTileMasks) so pixels only walk volumes that can reach
// their tile.
//
// CPU parity: the PACKED form is algebraically equal to GiVolumePlaneWeight /
// GiVolumeDef::Weight (cubemap_volume.h): min-of-smoothsteps ==
// smoothstep-of-min (the cubic is monotone on [0,1]) and
// saturate(dot(weightEquation.xyz, pos) + weightEquation.w) ==
// saturate((inside + outsideFade) / span) with
// weightEquation = (-n * fadeScale, fadeBias - d * fadeScale).
// Degenerate spans (insideFade + outsideFade < 1e-5) pack as a 1e-5 m
// ramp — the authored CPU step branch (GiVolumeSmoothStep) collapses to the
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
float SgGiVolumeWeight(FrameUniforms frame, GiVolumeGpu v, float3 pos)
{
    // Broad-phase: the fade-expanded hull AABB (the CPU packer grew the
    // generous Bounds() box by the widest outsideFade per axis — a
    // superset of the weight>0 region, so a reject here is always exact).
    if (any(pos < v.aabbMin.xyz) || any(pos > v.aabbMax.xyz))
        return 0.0;
    const uint count = min(v.planeCountBase.x, (uint)SG_GI_VOLUME_MAX_PLANES);
    float minT = 1.0;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        // One 16 B slot: the folded ramp — one dot3 + FMA + saturate.
        // Early out: once one plane is fully outside, the min cannot
        // recover.
        const float4 p =
            frame.giVolumes.Get().planes[v.planeCountBase.y + i].weightEquation;
        const float t = saturate(dot(p.xyz, pos) + p.w);
        minT = min(minT, t);
        if (minT <= 0.0)
            return 0.0;
    }
    return minT * minT * (3.0 - 2.0 * minT);
}

// Per-tile volume bitmask for a pixel (see GiVolumeTileMaskBuffer).
// Returns the all-volumes mask when no tile cull refreshed the masks
// for this view/frame (giVolumeTileCountX == 0 — the app publishes address
// + grid together, zeros together) or the pixel lands outside the tile
// grid: the mask is only ever allowed to NARROW the candidate set
// (correctness lives in the per-pixel AABB + plane tests of
// SgGiVolumeWeight), never invent or drop volumes on its own.
uint SgGiVolumeTileMask(FrameUniforms frame, float2 pix)
{
    const uint volCount = min(frame.giVolumeCount, (uint)SG_GI_VOLUME_MAX);
    const uint all = (1u << volCount) - 1u; // volCount <= 8 -> fits
    if (frame.giVolumeTileCountX == 0u)
        return all;
    const uint ts = max(frame.giVolumeTileSize, 1u);
    const uint tx = (uint)(pix.x / (float)ts);
    if (tx >= frame.giVolumeTileCountX)
        return all;
    const uint ty = (uint)(pix.y / (float)ts);
    const uint tileIdx = ty * frame.giVolumeTileCountX + tx;
    if (tileIdx >= (uint)MAX_FP_TILES)
        return all;
    return frame.giVolumeTileMasks.Get().masks[tileIdx] & all;
}

// ---- Through-wall rejection (the visibility bake) ----
// The dense-grid rebirth of the retired .sggi octree's per-probe
// 6-axis wall-awareness (SgGiVisibilityFactor): each cell corner's 6
// obstacle distances (spacing units, [0, 15] full-scale unorm8 —
// PackGpuVisLayers) gate that corner's tap by the tap OFFSET projected
// on each axis. dist 0 = BLOCKED (the probe is inside solid); the
// 0.35-spacing-unit slack keeps a point AT an obstacle surface reading
// its own surface probe (the surface we shade lives at the recorded
// distance); the rejection ramps to zero over a 25%-of-dist margin so
// thin obstacles do not nuke open-air coverage. Per-axis product — the
// CPU twin is GiVolume::Sample; the bake's same-side gates
// (mutuallyOpen) use the SAME slack (0.35 spacing — keep in sync).
float SgGiVolumeVisAxis(float along, float dist)
{
    // dist = 0 = obstacle at distance 0 (blocked); a miss at bake time
    // records the cap, which no in-cell offset can exceed = open.
    // The ramp denominator 0.25*dist + 0.5 (was 0.02 — the octree's
    // near-instant rejection) is the DARK-SQUARE FIX: blocked probes
    // (dist=0) now reject GRADUALLY across the cell (0.5 spacing
    // units of ramp instead of 0.02), so the transition between
    // clean and non-clean cells is smooth instead of a lattice-
    // aligned cliff. The old 0.02 made the factor essentially binary
    // (0 or 1), and the renormalization at the cell boundary produced
    // the visible grid (the sponza_3 flat-irradiance report).
    [branch]
    // HYBRID REACH (the black-shadow fix): proportional for large
    // dist (tight isolation), floor-clamped for small dist (wall-
    // pixel coverage). The pure proportional (dist * 1.5) went to
    // ~zero for near-surface probes — wall-adjacent pixels lost
    // their nearest probes (all 8 corners rejected, wSum < the
    // threshold, amps = 0) and rendered as hard black shadows
    // along walls (the sponza report: one-sided, the side whose
    // cell corners were mostly inside the wall or near-surface).
    // The floor (max(dist * 1.5, 0.5)) ensures near-surface probes
    // always reach at least 0.5 spacing units (0.25 m at 0.5 m
    // spacing) — enough for same-side wall pixels to read them.
    // Through-thin-wall leak: a probe 0.1 m from a thin wall
    // reaches 0.25 m; a pixel on the other side at 0.05 m from the
    // wall has offset 0.25 m — right at the reach boundary, the
    // ramp midpoint (~50% leak, a smooth transition, vs the old
    // 100% absolute-slack leak and the 0% pure-proportional black).
    // The RAMP width is also floor-clamped: max(0.5 * dist, 0.3)
    // — near-surface probes ramp over 0.3 units (smooth), open
    // probes proportionally.
    const float reach = max(dist * 1.5, 0.5);
    if (along > reach)
        return max(0.0, 1.0 - (along - reach) / max(0.5 * dist, 0.3));
    return 1.0;
    return 1.0;
}

float SgGiVolumeVisFactor(float3 dRel, float4 visA, float4 visB)
{
    // visA = +x,-x,+y,-y; visB = +z,-z (the .zw tail is the flag pool).
    // dRel is the tap offset in SPACING UNITS (pos - corner, /spacing).
    const float kFull = 15.0;
    float w = 1.0;
    w *= SgGiVolumeVisAxis(+dRel.x, visA.x * kFull);
    w *= SgGiVolumeVisAxis(-dRel.x, visA.y * kFull);
    w *= SgGiVolumeVisAxis(+dRel.y, visA.z * kFull);
    w *= SgGiVolumeVisAxis(-dRel.y, visA.w * kFull);
    w *= SgGiVolumeVisAxis(+dRel.z, visB.x * kFull);
    w *= SgGiVolumeVisAxis(-dRel.z, visB.y * kFull);
    return w;
}

// ---- Debug: the nearest probe's minimum axis distance ----
// The coplanarity check for the splotchy-field diagnosis: for a pixel's
// cell in the given volume, the base corner's minimum of the 6 stored
// obstacle distances (spacing units). Dark = the probe is very close
// to a surface in at least one axis (coplanar / inside geometry — the
// rejection ramp is active); bright = the probe is in open space (the
// cap = fully open). Clusters of dark probes near surfaces are the
// ones whose rejection modifies the blend — the splotchiness source.
// Returns the min distance normalized to [0, 1] over the 15-unit
// full-scale range (0 = blocked/coplanar, 1 = fully open).
float SgGiVolumeDebugProbeDistance(FrameUniforms frame, uint vol, float3 pos)
{
    const GiVolumeGpu v = frame.giVolumes.Get().volumes[vol];
    const float3 uvw = saturate(pos * v.uvwScale.xyz + v.uvwBias.xyz);
    const uint visSlotA = frame.giVolumeVisLayers[vol * 2u];
    const uint visSlotB = frame.giVolumeVisLayers[vol * 2u + 1u];
    if (visSlotA == 0xffffffffu || visSlotB == 0xffffffffu)
        return 1.0; // no vis data (the pre-visibility behavior)
    const float3 dims = float3(v.uvwScale.w, v.uvwBias.w, v.aabbMax.w);
    const float3 rel = uvw * dims;
    const int3 b = clamp(int3(floor(rel - 0.5)), 0, int3(dims) - 2);
    const float4 visA = volumeTextures[visSlotA].Load(int4(b, 0));
    const float4 visB = volumeTextures[visSlotB].Load(int4(b, 0));
    const float kFull = 15.0;
    const float d0 = visA.x * kFull;
    const float d1 = visA.y * kFull;
    const float d2 = visA.z * kFull;
    const float d3 = visA.w * kFull;
    const float d4 = visB.x * kFull;
    const float d5 = visB.y * kFull;
    const float mn = min(min(min(d0, d1), min(d2, d3)), min(d4, d5));
    return saturate(mn / kFull);
}
// Samples one volume's dense grid at world pos. Two paths:
//  - FAST (the common cell): the bake's per-voxel clean flag
//    (visB.b, point-fetched at the cell's base corner) proves every
//    corner of this cell sees every in-cell point — no tap can reject,
//    so the plain 7-fetch hardware trilerp below IS the
//    visibility-weighted blend. Absent vis layers (a pre-visibility
//    bake / a failed vis upload) also take this path — the
//    no-rejection contract, exactly the old behavior.
//  - WEIGHTED (cells near geometry): the 8 corners each weighted by
//    VisFactor(offset, that corner's distances) x the trilerp weight,
//    renormalized; surviving corners fetch their 7 lobe layers
//    point-sampled. ~72 loads in the wall band only — the clean cells
//    around it stay at 8.
// uvw = saturate(pos * uvwScale + uvwBias) — the CPU packed the
// lattice transform; the grid dims ride the header's spare w slots
// (dims.x = uvwScale.w, dims.y = uvwBias.w, dims.z = aabbMax.w) so the
// lattice-space math below needs no extra fields. The layer index
// rides a wave-UNIFORM loop counter (never a per-pixel value), so the
// plain bindless index keeps the descriptor fetch scalar — no
// NonUniformResourceIndex divergence on the taps.
void SgGiVolumeSample(FrameUniforms frame, uint vol, float3 pos,
    out float3 amps[SG_GI_LOBES])
{
    const GiVolumeGpu v = frame.giVolumes.Get().volumes[vol];
    const float3 uvw = saturate(pos * v.uvwScale.xyz + v.uvwBias.xyz);

    // ---- the cell + the clean-flag fast path ----
    const uint visSlotB = frame.giVolumeVisLayers[vol * 2u + 1u];
    if (visSlotB != 0xffffffffu)
    {
        // Lattice coords: rel = uvw * dims, the voxel-center
        // convention (texel c's center at uvw (c+0.5)/dims); the
        // sample's cell = [b, b+1]^3, b = floor(rel - 0.5) clamped to
        // dims-2; f = the in-cell fraction (the trilerp weights).
        const float3 dims = float3(v.uvwScale.w, v.uvwBias.w, v.aabbMax.w);
        const float3 rel = uvw * dims;
        const int3 b = clamp(int3(floor(rel - 0.5)), 0, int3(dims) - 2);
        // The forward cell's clean bit rides voxel b's visB.b channel
        // (Load = OpImageFetch, the exact-texel read the vis math
        // needs — no sampler filtering; DXC's Texture3D Load takes an
        // int4 = xyz + the mip slot 0).
        if (volumeTextures[visSlotB].Load(int4(b, 0)).z < 0.5)
        {
            const uint visSlotA = frame.giVolumeVisLayers[vol * 2u];
            const float3 f = saturate(rel - 0.5 - float3(b));
            float3 acc[SG_GI_LOBES] = { float3(0, 0, 0), float3(0, 0, 0),
                float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0),
                float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0),
                float3(0, 0, 0) };
            float wSum = 0.0;
            [unroll]
            for (uint c = 0; c < 8; ++c)
            {
                const int3 cc = int3(b) + int3(c & 1 ? 1 : 0,
                    c & 2 ? 1 : 0, c & 4 ? 1 : 0);
                const float3 dRel = rel - (float3(cc) + 0.5);
                const float4 visA = volumeTextures[visSlotA].Load(int4(cc, 0));
                const float4 visB = volumeTextures[visSlotB].Load(int4(cc, 0));
                const float wv = SgGiVolumeVisFactor(dRel, visA, visB);
                const float wvEff = wv;
                const float3 t3 = float3(c & 1 ? f.x : 1.0 - f.x,
                    c & 2 ? f.y : 1.0 - f.y, c & 4 ? f.z : 1.0 - f.z);
                const float wt = wvEff * (t3.x * t3.y * t3.z);
                if (wt <= 0.0)
                    continue;
                float4 layers[SG_GI_VOLUME_LAYERS];
                [unroll]
                for (uint t = 0; t < SG_GI_VOLUME_LAYERS; ++t)
                    layers[t] = volumeTextures[
                        frame.giVolumeLayers[vol * 7u + t]]
                        .Load(int4(cc, 0));
                float3 corner[SG_GI_LOBES];
                SgRegularUnpack(layers, corner);
                [unroll]
                for (uint l = 0; l < SG_GI_LOBES; ++l)
                    acc[l] += corner[l] * wt;
                wSum += wt;
            }
            if (wSum > 1e-4)
            {
                [unroll]
                for (uint l = 0; l < SG_GI_LOBES; ++l)
                    amps[l] = acc[l] / wSum;
            }
            else
            {
                // All 8 corners blocked (a point deep inside a wall —
                // no visible pixel should land here): read zero; the
                // hull weight still owns the pixel and zero beats the
                // old both-sides blend (the leak).
                [unroll]
                for (uint l = 0; l < SG_GI_LOBES; ++l)
                    amps[l] = float3(0.0, 0.0, 0.0);
            }
            return;
        }
    }

    // FAST PATH: the 7 hardware-trilinear 3D reads + the shared
    // 27-half unpack (the cell is clean — or no vis data: same shape).
    float4 layers[SG_GI_VOLUME_LAYERS];
    [unroll]
    for (uint t = 0; t < SG_GI_VOLUME_LAYERS; ++t)
    {
        layers[t] = volumeTextures[frame.giVolumeLayers[vol * 7u + t]]
            .Sample(samplers[2], uvw);
    }
    SgRegularUnpack(layers, amps);
}

// ---- Volumetric mean path (the fog twin of the sampling/compositing
// above): the fog consumes only the isotropic mean, which is linear in
// the lobe amps (VolGiMean = 0.124959 * Σ amps), so the mean rides a
// CPU-BAKED per-volume RGB texture (GiVolume::PackGpuMeanLayer —
// trilerp(Σ amps) == Σ trilerp(amps), packed from the same coeffs) and
// the walk never materializes the SG lobe arrays: ONE trilinear fetch
// per volume instead of 7 + the 27-half unpack + the 9-lobe blend —
// the surface representation (54 coefficient floats alive per lookup)
// was pure register-pressure poison at up to vp.steps lookups per
// pixel. Same header/AABB/plane weighting as SgGiVolumeWeight. ----

// Samples one volume's baked mean texture at world pos (one trilinear
// RGB fetch; uvw from the same packed header transform).
void SgGiVolumeSampleMean(FrameUniforms frame, uint vol, float3 pos,
    out float3 mean)
{
    const GiVolumeGpu v = frame.giVolumes.Get().volumes[vol];
    const float3 uvw = saturate(pos * v.uvwScale.xyz + v.uvwBias.xyz);
    mean = volumeTextures[frame.giVolumeMeanLayers[vol]]
        .Sample(samplers[2], uvw).rgb;
}

// ---- Fog ray-space volume table (per-pixel prep, SgGiVolumeRayPrep) ----
// The march ray is pos(t) = camPos + rayDir * t, so each plane ramp
//   t_p = saturate(dot(p.xyz, pos(t)) + p.w)
//       = saturate(dot(p.xyz, camPos) + p.w + dot(p.xyz, rayDir) * t)
//       = saturate(base_p + slope_p * t)
// with base/slope computed ONCE per pixel per candidate volume: the
// march's per-segment weight test becomes FMA + saturate + min — no
// plane-buffer loads, no dot3s per segment. Volumes whose
// fade-expanded-AABB interval misses the ray never enter the table at
// all (2 interval compares per segment, no loads). Register-bounded:
// the first SG_GI_VOLUME_MAX_RAY interval-overlapping volumes
// (priority order) ride the table; any excess (pathological-only)
// evaluates per segment straight from the pool, exactly
// (SgGiVolumeWeight + one mean fetch).
struct GiRayVolume
{
    float enter;                        // ray interval [enter, exit]
    float exit;                         // (clipped to [0, rayLen])
    uint volume;                        // upload index (priority order)
    uint planeCount;                    // planes in base/slope below
    float base[SG_GI_VOLUME_MAX_PLANES];       // dot(p.xyz, camPos) + p.w
    float slope[SG_GI_VOLUME_MAX_PLANES];      // dot(p.xyz, rayDir)
};

struct GiVolumeRayPrep
{
    uint rayCount;                                   // table entries used
    uint tailCount;                                  // pool-fallback entries used
    GiRayVolume rays[SG_GI_VOLUME_MAX_RAY];        // priority ordered
    uint tailVolumes[SG_GI_VOLUME_MAX];             // priority ordered
    float tailEnter[SG_GI_VOLUME_MAX];              // segment skip intervals
    float tailExit[SG_GI_VOLUME_MAX];
    float3 rayMean[SG_GI_VOLUME_MAX_RAY];           // held means (lazy fetch)
    bool rayMeanValid[SG_GI_VOLUME_MAX_RAY];
};

// Per-pixel ray-space prep for the fog march: tile mask -> per-volume
// fade-expanded-AABB slab test vs the march ray (headers only, no plane
// traffic) -> ray-interval table for the overlapping volumes (priority
// order) with their planes transformed to base/slope form. Called ONCE
// per pixel; the march then walks the table, never the pool.
void SgGiVolumeRayPrep(FrameUniforms frame, uint volMask, float3 camPos,
    float3 rayDir, float rayLen, out GiVolumeRayPrep prep)
{
    prep.rayCount = 0u;
    prep.tailCount = 0u;
    if (frame.giVolumesEnabled == 0u)
        return;
    const uint volCount = min(frame.giVolumeCount, (uint)SG_GI_VOLUME_MAX);
    [loop]
    for (int v = int(volCount) - 1; v >= 0; --v) // HIGH priority first
    {
        if ((volMask & (1u << uint(v))) == 0u)
            continue;
        const GiVolumeGpu vol = frame.giVolumes.Get().volumes[uint(v)];
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
        if (prep.rayCount < (uint)SG_GI_VOLUME_MAX_RAY)
        {
            const uint s = prep.rayCount;
            prep.rays[s].enter = enter;
            prep.rays[s].exit = exit;
            prep.rays[s].volume = uint(v);
            const uint count = min(vol.planeCountBase.x, (uint)SG_GI_VOLUME_MAX_PLANES);
            prep.rays[s].planeCount = count;
            [loop]
            for (uint p = 0; p < count; ++p)
            {
                const float4 wEq = frame.giVolumes.Get()
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
            if (k < (uint)SG_GI_VOLUME_MAX)
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
// true mean. Same ordered transmittance math as SgGiVolumeComposite — the
// result is the composite AT this t with exact weights.
void SgGiVolumeRayComposite(FrameUniforms frame, inout GiVolumeRayPrep prep, float t,
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
            SgGiVolumeSampleMean(frame, prep.rays[s].volume, wpos,
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
        const GiVolumeGpu vol =
            frame.giVolumes.Get().volumes[prep.tailVolumes[k]];
        const float w = SgGiVolumeWeight(frame, vol, wpos);
        [branch]
        if (w <= 0.0)
            continue;
        float3 vMean;
        SgGiVolumeSampleMean(frame, prep.tailVolumes[k], wpos, vMean);
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
// from SgGiVolumeTileMask (hoistable per pixel; the volumetric march reuses
// one mask for all its segments).
void SgGiVolumeComposite(FrameUniforms frame, uint volMask, float3 pos,
    out float3 amps[SG_GI_LOBES], out float transmission, out bool hit)
{
    [unroll]
    for (uint l = 0; l < SG_GI_LOBES; ++l)
        amps[l] = float3(0.0, 0.0, 0.0);
    transmission = 1.0;
    hit = false;
    if (frame.giVolumesEnabled == 0u)
        return;
    const uint volCount = min(frame.giVolumeCount, (uint)SG_GI_VOLUME_MAX);
    [loop]
    for (int v = int(volCount) - 1; v >= 0; --v) // HIGH priority first
    {
        if ((volMask & (1u << uint(v))) == 0u)
            continue;
        const GiVolumeGpu vol = frame.giVolumes.Get().volumes[uint(v)];
        const float w = SgGiVolumeWeight(frame, vol, pos);
        [branch]
        if (w <= 0.0)
            continue;
        float3 vAmps[SG_GI_LOBES];
        SgGiVolumeSample(frame, uint(v), pos, vAmps);
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
