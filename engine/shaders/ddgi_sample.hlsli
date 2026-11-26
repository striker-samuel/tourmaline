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

#include "ddgi_common.hlsli"

struct DdgiSampleResult
{
    float3 irradiance;
    float valid; // 0 = outside/invalid -> caller keeps the baked/sky slice
};

static const float DDGI_NORMAL_BIAS = 0.25; // probe-space bias along N (spacing units)

// Sample ONE volume at worldPos with surface normal N (the 8-probe walk).
DdgiSampleResult DdgiSampleVolume(DdgiVolumeDesc desc, uint slot, float3 worldPos, float3 N)
{
    DdgiSampleResult r;
    r.irradiance = float3(0.0, 0.0, 0.0);
    r.valid = 0.0;

    const float spacing = max(desc.spacing, 1e-4f);
    const float3 origin = float3(desc.originX, desc.originY, desc.originZ);
    const int3 dims = int3(desc.dimX, desc.dimY, desc.dimZ);
    // Bias the receiver along N and trilinear on the BIASED position (the
    // standard DDGI walk). The probes sit at the grid CORNERS (origin +
    // integer*spacing); a surface lying exactly on the lattice would
    // otherwise sample two embedded/same-side probes (facing ~0) and read
    // invalid, never reaching the open-air probe on its normal side. The old
    // code trilinear'd the unbiased point with a -0.5 center shift, a
    // different convention than the corner placement.
    const float3 biasedPos = worldPos + N * (DDGI_NORMAL_BIAS * spacing);
    const float3 local = (biasedPos - origin) / spacing;
    const int3 base = int3(floor(local));
    const float3 frac = local - float3(base);

    const uint irrStride = DDGI_IRR_RES * DDGI_IRR_RES;
    const uint distStride = DDGI_DIST_RES * DDGI_DIST_RES;

    // Directional oct bin for the surface normal (irradiance) and for the
    // probe->receiver direction (visibility moments).
    const float2 nUv = DdgiOctEncode(N);
    const uint irrBin = min(uint(nUv.x * float(DDGI_IRR_RES)), DDGI_IRR_RES - 1u)
        + min(uint(nUv.y * float(DDGI_IRR_RES)), DDGI_IRR_RES - 1u) * DDGI_IRR_RES;

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

        const float3 probePos = origin + float3(coord) * spacing;

        // Irradiance bin toward the surface normal; .w carries validity.
        const float4 irr = desc.irradiance[slot].Get().values[probe * irrStride + irrBin];
        if (irr.w <= 0.0)
            continue;

        // Chebyshev visibility toward the receiver (from the actual probe).
        const float3 toReceiver = biasedPos - probePos;
        const float dist = length(toReceiver);
        const float3 recDir = dist > 1e-5f ? toReceiver / dist : N;
        const float2 recUv = DdgiOctEncode(recDir);
        const uint distBin = min(uint(recUv.x * float(DDGI_DIST_RES)), DDGI_DIST_RES - 1u)
            + min(uint(recUv.y * float(DDGI_DIST_RES)), DDGI_DIST_RES - 1u) * DDGI_DIST_RES;
        const float2 moments = desc.distance[slot].Get().values[probe * distStride + distBin];
        const float visibility = DdgiVisibility(moments, dist);

        // Normal-facing cosine: probes behind the surface contribute 0.
        // Measure toward the BIASED receiver (not worldPos) so a probe lying
        // exactly on the surface (normalize(0) -> NaN) degrades to facing 0
        // instead of poisoning the weight.
        const float facing = max(dot(N, normalize(probePos - biasedPos)), 0.0f);

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

// Sample the DDGI probe field at worldPos with surface normal N: find the
// volume whose AABB contains the point and sample it. Returns valid == 0
// when the point is outside every volume or the weights sum to ~0.
DdgiSampleResult DdgiSampleIrradiance(DdgiParams params, float3 worldPos, float3 N)
{
    DdgiSampleResult r;
    r.irradiance = float3(0.0, 0.0, 0.0);
    r.valid = 0.0;
    if (params.enabled == 0u || params.volumeCount == 0u)
        return r;

    const uint count = min(params.volumeCount, 128u);
    // Index the BDA in place (values[1] is a flexible GPU array view).
    // Reject using spatial fields before fetching the history pointers.
    [loop]
    for (uint v = 0; v < count; ++v)
    {
        const float3 vmin = float3(params.volumes.Get().values[v].originX,
            params.volumes.Get().values[v].originY, params.volumes.Get().values[v].originZ);
        const float3 vmax = vmin + float3(params.volumes.Get().values[v].dimX,
            params.volumes.Get().values[v].dimY, params.volumes.Get().values[v].dimZ)
            * params.volumes.Get().values[v].spacing;
        if (all(worldPos >= vmin) && all(worldPos <= vmax))
            return DdgiSampleVolume(params.volumes.Get().values[v], params.slot, worldPos, N);
    }
    return r;
}
