// SPDX-License-Identifier: MIT
#pragma once
// DDGI sampling (Rendergraph/ddgi.h + docs/ddgi_static_rendergraph.md §6):
// the lit shader's diffuse-ambient reader. The world is divided into
// 64 m subcell probe VOLUMES; the lit shader finds the volume containing
// the pixel (via the DdgiParams slab's volume-descriptor list), walks
// the 8 neighboring grid probes, weights them by trilinear falloff +
// normal-facing cosine + Chebyshev visibility (from the published
// distance moments), and returns the directionally-sampled irradiance E
// (NOT outgoing radiance — the caller multiplies by kD*baseColor/pi).
//
// Two normals are taken: the GEOMETRIC normal (geomN) drives every
// spatial decision (receiver bias, probe selection, facing, visibility);
// the SHADING normal (N, after normal mapping) only selects the
// directional irradiance bin. Normal-map detail must not move the
// receiver or re-weight neighbors, or a brick wall stipples.

#include "ddgi_common.hlsli"

struct DdgiSampleResult
{
    float3 irradiance;
    float valid; // 0 = outside/invalid -> caller keeps the baked/sky slice
};

static const float DDGI_NORMAL_BIAS = 0.25; // probe-space bias along geomN (spacing units)

// Sample ONE volume at worldPos with geometric normal geomN and shading
// normal N (the 8-probe walk).
DdgiSampleResult DdgiSampleVolume(DdgiVolumeDesc desc, uint slot, float3 worldPos,
                                  float3 geomN, float3 N)
{
    DdgiSampleResult r;
    r.irradiance = float3(0.0, 0.0, 0.0);
    r.valid = 0.0;

    const float spacing = max(desc.spacing, 1e-4f);
    const float3 origin = float3(desc.originX, desc.originY, desc.originZ);
    const int3 dims = int3(desc.dimX, desc.dimY, desc.dimZ);
    // Bias the receiver along the GEOMETRIC normal and trilinear on the
    // BIASED position (the standard DDGI walk). The probes sit at the grid
    // CORNERS (origin + integer*spacing); a surface lying exactly on the
    // lattice would otherwise sample two embedded/same-side probes
    // (facing ~0) and read invalid, never reaching the open-air probe on
    // its normal side.
    const float3 biasedPos = worldPos + geomN * (DDGI_NORMAL_BIAS * spacing);
    const float3 local = clamp((biasedPos - origin) / spacing, 0.0f, float3(dims - 1));
    const int3 base = int3(floor(local));
    const float3 frac = local - float3(base);

    const uint irrStride = DDGI_IRR_STRIDE;
    const uint distStride = DDGI_DIST_RES * DDGI_DIST_RES;

    // Directional irradiance taps for the SHADING normal (computed once).
    uint irrBin[4];
    float irrW[4];
    DdgiOctBilinearTaps(N, DDGI_IRR_RES, irrBin, irrW);

    float3 acc = float3(0.0, 0.0, 0.0);
    float wSum = 0.0;

    [unroll]
    for (uint c = 0; c < 8; ++c)
    {
        const int3 off = int3(c & 1u, (c >> 1) & 1u, (c >> 2) & 1u);
        const int3 coord = base + off;
        if (coord.x < 0 || coord.y < 0 || coord.z < 0 ||
            coord.x >= dims.x || coord.y >= dims.y || coord.z >= dims.z)
            continue;

        const uint probe = uint(coord.x) + uint(coord.y) * uint(dims.x)
            + uint(coord.z) * uint(dims.x * dims.y);

        // Trilinear weight.
        const float3 w = lerp(1.0f - frac, frac, float3(off));
        const float tri = w.x * w.y * w.z;
        if (tri <= 0.0)
            continue;

        const float3 probePos = origin + float3(coord) * spacing
            + DdgiHistoryOffset(desc.irradiance[slot], probe);

        // Bilinear irradiance toward the surface normal; .w = validity.
        float4 irr = float4(0.0, 0.0, 0.0, 0.0);
        [unroll]
        for (uint t = 0; t < 4; ++t)
            irr += desc.irradiance[slot].Get().values[probe * irrStride + irrBin[t]] * irrW[t];
        if (irr.w <= 0.0)
            continue;

        // Chebyshev visibility toward the receiver (from the actual probe),
        // with bilinear distance moments.
        const float3 toReceiver = biasedPos - probePos;
        const float dist = length(toReceiver);
        const float3 recDir = dist > 1e-5f ? toReceiver / dist : geomN;
        uint distBin[4];
        float distW[4];
        DdgiOctBilinearTaps(recDir, DDGI_DIST_RES, distBin, distW);
        float2 moments = float2(0.0, 0.0);
        [unroll]
        for (uint t = 0; t < 4; ++t)
            moments += desc.distance[slot].Get().values[probe * distStride + distBin[t]] * distW[t];
        const float visibility = DdgiVisibility(moments, dist);

        // Facing uses the unbiased receiver; visibility alone uses the bias.
        const float facing = DdgiFacing(geomN, probePos - worldPos);

        const float weight = tri * facing * visibility;
        acc += irr.rgb * weight;
        wSum += weight;
    }

    if (wSum <= 1e-5f)
        return r;
    r.irradiance = acc / wSum;
    r.valid = 1.0;
    return r;
}

// Sample the DDGI probe field at worldPos with geometric normal geomN and
// shading normal N. Cascades are listed fine-first (both follow the camera;
// the fine volume is a subset of the coarse). Both are sampled and blended by
// camera distance — near the camera the fine cascade wins, far the coarse —
// so the density transition is a smooth cross-fade, not a hard seam. Returns
// valid == 0 when the point is outside every volume or the weights sum to ~0.
DdgiSampleResult DdgiSampleIrradiance(DdgiParams params, float3 cameraPos, float3 worldPos,
                                      float3 geomN, float3 N)
{
    DdgiSampleResult r;
    r.irradiance = float3(0.0, 0.0, 0.0);
    r.valid = 0.0;
    if (params.enabled == 0u || params.volumeCount == 0u)
        return r;

    const uint count = min(params.volumeCount, 128u);
    // Fine cascade = volume 0; its half-extent sets the cross-fade band.
    const DdgiVolumeDesc dFine = params.volumes.Get().values[0];
    const float3 halfExtent = max(0.5f * (float3(dFine.dimX, dFine.dimY, dFine.dimZ) - 1.0f) * dFine.spacing, 1e-4f);
    const float3 center = float3(dFine.originX, dFine.originY, dFine.originZ) + halfExtent;
    const float3 edge = abs(worldPos - center) / halfExtent;
    // Fade at every face, including the short vertical dimension.
    const float dist = max(length(worldPos - cameraPos) / halfExtent.x,
                           max(edge.x, max(edge.y, edge.z)));
    const float fadeStart = min(saturate(params.crossFade), 0.999f);
    const float fadeEnd = saturate(fadeStart + 0.4f);

    // Near the camera the fine cascade alone wins — skip the coarse sample
    // entirely (most pixels, so the smooth blend stays cheap).
    if (dist < fadeStart) {
        const float3 vmin = float3(dFine.originX, dFine.originY, dFine.originZ);
        const float3 vmax = vmin + (float3(dFine.dimX, dFine.dimY, dFine.dimZ) - 1.0f) * dFine.spacing;
        if (all(worldPos >= vmin) && all(worldPos <= vmax)) {
            const DdgiSampleResult nearSample = DdgiSampleVolume(dFine, params.slot, worldPos, geomN, N);
            if (nearSample.valid > 0.0f)
                return nearSample;
        }
        // Invalid fine history must not hide a valid coarse volume.
    }

    // Otherwise sample both cascades and cross-fade by distance (far = coarse,
    // the band in between blends).
    DdgiSampleResult fine;
    DdgiSampleResult coarse;
    fine.irradiance = float3(0.0, 0.0, 0.0);
    coarse.irradiance = float3(0.0, 0.0, 0.0);
    fine.valid = 0.0;
    coarse.valid = 0.0;
    [loop]
    for (uint v = 0; v < count; ++v)
    {
        const DdgiVolumeDesc d = params.volumes.Get().values[v];
        const float3 vmin = float3(d.originX, d.originY, d.originZ);
        const float3 vmax = vmin + (float3(d.dimX, d.dimY, d.dimZ) - 1.0f) * d.spacing;
        if (all(worldPos >= vmin) && all(worldPos <= vmax)) {
            DdgiSampleResult s = DdgiSampleVolume(d, params.slot, worldPos, geomN, N);
            if (s.valid > 0.0f) {
                if (v == 0u)
                    fine = s;
                else if (coarse.valid <= 0.0f)
                    coarse = s;
            }
        }
    }

    if (fine.valid > 0.0f && coarse.valid > 0.0f) {
        const float t = smoothstep(fadeStart, fadeEnd, dist);
        r.irradiance = lerp(fine.irradiance, coarse.irradiance, t);
        r.valid = 1.0f;
        return r;
    }
    if (fine.valid > 0.0f)
        return fine;
    if (coarse.valid > 0.0f)
        return coarse;
    return r;
}
