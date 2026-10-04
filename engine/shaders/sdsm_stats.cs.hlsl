// SDSM depth statistics (compute): decimated min/max VIEW-depth + a coarse
// log-space depth HISTOGRAM over the main render depth — the measured
// split range input for the cascades (Lauritzen & McCool 2011,
// "Sample Distribution Shadow Maps", distribution-based variant).
//
// Why a dedicated reduce instead of the existing depth pyramids: the
// occlusion HZB is MIN-only and sky-poisoned (one sky texel floors its
// mip to the far plane), and the MAX pyramid is the SSR trace's
// (conditional). One unconditional 256x256-thread decimated walk is
// hierarchy-equivalent cost (~65k samples ~= a 256^2 mip) and yields
// BOTH ends plus the distribution.
//
// The main depth is reversed-Z (1 = near, 0 = far; the clear is exactly
// 0.0 = sky). View depth is recovered with the reversed-Z ZO inversion
// viewZ = zNear / z; sky (z == 0) is skipped so the far end is measured
// over real geometry, and z is floored so far geometry cannot explode
// the ratio (the CPU clamps the range to the cascade far anyway).
//
// Output per group: {minV, maxV, bins[64]} — the bins are a log2(viewZ)
// histogram over [zNear, zFar] (uint counters, LDS atomics). No
// cross-group reduce: the CPU merges the 256 PARTIAL histograms (each
// group covers a different lane slice of the decimated samples) after
// the async readback (256*64 adds) and picks the robust percentile
// range — the point of the histogram: one tiny distant object must not
// dictate the whole split range.
//
// Decimation: one GLOBAL lane index (lane = group*256 + thread) walks
// lane*stride, +65536*stride, ... — the groups PARTITION the samples
// (gid must join the addressing, or all 256 groups sample the same
// pixels: 256 duplicate histograms + ~256x the loads). stride 1 = full
// coverage; the CPU's ceil(totalPixels / 65536) = ~1 sample per lane
// (~65k total, hierarchy-equivalent). The CPU consumes the stats ONE
// FRAME LATER (own async readback ticket, submitted every frame):
// current-frame measurement, next-frame fit.
//
// C++ mirror: WorldRenderer::SdsmStatsPush (40 bytes) + SdsmGroupStats
// (264 bytes) + kSdsmGroups/kSdsmBins.

#define SDSM_STATS_PASS
#include "common.hlsli"

// Must match kSdsmGroups / kSdsmBins on the C++ side.
#define SDSM_GROUPS 256
#define SDSM_BINS 64

struct SdsmGroupStats
{
    float minV;      // group min view depth (1e30 = nothing measured)
    float maxV;      // group max view depth (0 = nothing measured)
    uint bins[SDSM_BINS]; // log2(viewZ) histogram over [zNear, zFar]
};

struct SdsmStatsOut
{
    SdsmGroupStats stats[SDSM_GROUPS];
};

struct SdsmStatsPush
{
    vk::BufferPointer<SdsmStatsOut> stats; // BDA (C++ mirror: uint64_t)
    uint depthIndex;   // bindless textures[] index of the main render depth
    float zNear;       // main-view near clip (reversed-Z inversion)
    float zFar;        // histogram ceiling (the cascade far cap)
    uint width;        // render extent
    uint height;
    uint stride;       // per-thread hop in linear texels (decimation)
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<SdsmStatsPush> pcS;

groupshared float2 gsMinMax[256];
groupshared uint gsBins[SDSM_BINS];

[numthreads(256, 1, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint total = pcS.width * pcS.height;
    // Decimation addressing: one GLOBAL lane index over the whole dispatch
    // (lane = group * 256 + thread), so the 256 groups partition the samples
    // — gid must participate in the walk, or every group samples the SAME
    // pixels (256 duplicate histograms + ~256x the loads). Each lane walks
    // lane*stride, +laneCount*stride, ...: stride 1 = full coverage, the
    // CPU's ceil(totalPixels / laneCount) = ~1 sample per lane (~65k total).
    const uint stride = max(pcS.stride, 1u);
    const uint lane = gid.x * 256u + gtid.x;
    const uint laneCount = SDSM_GROUPS * 256u;
    const uint step = laneCount * stride;
    // Histogram interval, hoisted: log2(viewZ) over
    // [log2(zNear), log2(zFar)] -> t in [0, 1] -> bin in [0, 64).
    const float logNear = log2(pcS.zNear);
    const float logFar = log2(max(pcS.zFar, pcS.zNear * 2.0f));
    const float invLogRange = rcp(max(logFar - logNear, 1e-4f));
    float mn = 1e30f;
    float mx = 0.0f;
    if (gtid.x < SDSM_BINS)
        gsBins[gtid.x] = 0u;
    // Init visible before the atomics below (group-wide).
    GroupMemoryBarrierWithGroupSync();
    for (uint i = lane * stride; i < total; i += step)
    {
        const int2 p = int2(i % pcS.width, i / pcS.width);
        const float z = textures[pcS.depthIndex].Load(int3(p, 0)).r;
        if (z > 0.0f) // skip the sky clear (reversed-Z clear == 0.0 exactly)
        {
            const float viewZ = pcS.zNear / max(z, 1e-5f);
            mn = min(mn, viewZ);
            mx = max(mx, viewZ);
            const float t = saturate(
                (log2(max(viewZ, 1e-6f)) - logNear) * invLogRange);
            // saturate() INCLUDES 1.0 (viewZ at/beyond zFar), so the raw
            // t*64 index can be 64 — clamp to the last bin, or the
            // InterlockedAdd writes past gsBins[64].
            const uint bin = min(uint(t * float(SDSM_BINS)), SDSM_BINS - 1u);
            InterlockedAdd(gsBins[bin], 1u);
        }
    }
    gsMinMax[gtid.x] = float2(mn, mx);
    GroupMemoryBarrierWithGroupSync();
    // Publish: no intermediate SdsmGroupStats local — a dynamically indexed
    // local bins[64] promotes to private memory; write the destination
    // fields directly instead. Thread 0 does the 256-entry min/max (once
    // per group, trivial); the first 64 lanes each publish one bin in
    // parallel. Fields are disjoint, so no second barrier is needed.
    if (gtid.x == 0)
    {
        float2 acc = float2(1e30f, 0.0f);
        for (uint t = 0; t < 256; ++t)
        {
            acc.x = min(acc.x, gsMinMax[t].x);
            acc.y = max(acc.y, gsMinMax[t].y);
        }
        pcS.stats.Get().stats[gid.x].minV = acc.x;
        pcS.stats.Get().stats[gid.x].maxV = acc.y;
    }
    if (gtid.x < SDSM_BINS)
        pcS.stats.Get().stats[gid.x].bins[gtid.x] = gsBins[gtid.x];
}
