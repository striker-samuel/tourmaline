// SPDX-License-Identifier: MIT
#include "ddgi_common.hlsli"

// One thread per directional bin. History validity is per irradiance bin;
// reset must be set whenever either previous buffer is uninitialized.
struct DdgiBlendPush {
    GPU_PTR_READ(DdgiRayBuffer) scratch;
    GPU_PTR_READ(DdgiIrrBuffer) oldIrr;
    GPU_PTR_READ(DdgiDistanceBuffer) oldDistance;
    GPU_PTR_WRITE(DdgiIrrBuffer) newIrr;
    GPU_PTR_WRITE(DdgiDistanceBuffer) newDistance;
    uint probes;
    uint rays;
    uint reset;
    float hysteresis;
    float maxDistance;
    int deltaX;
    int deltaY;
    int deltaZ;
    uint dimX;
    uint dimY;
    uint dimZ;
    uint pad;
};
GPU_PUSH(DdgiBlendPush, sp);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint bin = tid.x;
    const uint probe = tid.y;
    if (probe >= sp.probes || bin >= DDGI_DIST_RES * DDGI_DIST_RES || sp.rays == 0)
        return;

    // Toroidal history carry (camera-following scroll): probe `probe` sits at
    // grid coord c = DdgiGridCoord(probe). Last frame that world position was
    // logical coord c + delta. When c+delta is inside [0, dims) the probe's
    // history carried across the scroll (blend hysteresis); otherwise it
    // scrolled into fresh territory and resets (hysteresis 0).
    const uint3 dims = uint3(sp.dimX, sp.dimY, sp.dimZ);
    const uint3 coord = DdgiGridCoord(probe, dims);
    const int3 oldCoord = int3(coord) + int3(sp.deltaX, sp.deltaY, sp.deltaZ);
    const bool carry = all(oldCoord >= int3(0, 0, 0)) && all(oldCoord < int3(dims));
    const uint oldProbe = carry
        ? uint(oldCoord.x) + uint(oldCoord.y) * sp.dimX + uint(oldCoord.z) * sp.dimX * sp.dimY
        : 0u;

    const bool writeIrr = bin < DDGI_IRR_RES * DDGI_IRR_RES;
    float3 irrDir = DdgiBinDirection(min(bin, DDGI_IRR_RES * DDGI_IRR_RES - 1), DDGI_IRR_RES);
    float3 distDir = DdgiBinDirection(bin, DDGI_DIST_RES);
    float3 sumIrr = 0;
    float2 sumMoments = 0;
    float sumWeight = 0;
    float sumDist = 0;
    uint incompleteCount = 0;

    for (uint r = 0; r < sp.rays; ++r) {
        DdgiRay ray = sp.scratch.Get().values[probe * sp.rays + r];
        if (ray.flags == 3)
            ++incompleteCount;
        if (writeIrr && ray.flags < 2 && all(isfinite(ray.radiance))) {
            sumIrr += max(ray.radiance, 0) * max(dot(irrDir, ray.direction), 0);
        }
        float w = pow(saturate(dot(distDir, ray.direction)), 16.0);
        float d = isfinite(ray.distance) ? clamp(ray.distance, 0, sp.maxDistance) : 0;
        sumMoments += float2(d, d*d) * w;
        sumWeight += w;
        sumDist += d;
    }

    // Probe validity: a probe is "inside geometry" only when its mean ray
    // distance collapses (surrounded on all sides). The old backface-count
    // test (backfaces*4 <= rays) wrongly rejected interior room probes — a
    // ceiling is a legitimate back-face hit from below, not proof the probe
    // is embedded. Distance-based validity keeps room probes valid while
    // rejecting probes buried in walls/floors (mean dist ~0).
    const float meanDist = sumDist / float(max(sp.rays, 1u));
    // Tolerate a few unresolved rays: the SDF march occasionally hits its
    // step budget in enclosed spaces, and requiring ZERO incomplete rays
    // made tunnel probes flicker invalid on the per-frame ray rotation (the
    // field visibly flashing frame-to-frame). Require < 1/4 unresolved.
    const bool valid =
        incompleteCount < (sp.rays / 4u + 1u) && meanDist > sp.maxDistance * 0.025f;
    float h = (sp.reset == 0 && carry) ? saturate(sp.hysteresis) : 0.0;
    uint distIndex = probe * DDGI_DIST_RES * DDGI_DIST_RES + bin;
    uint oldDistIndex = oldProbe * DDGI_DIST_RES * DDGI_DIST_RES + bin;
    float2 moments = sumWeight > 1e-8 ? sumMoments / sumWeight :
        float2(sp.maxDistance, sp.maxDistance * sp.maxDistance);
    // Use the first irradiance bin as the probe-wide validity flag, read at
    // the toroidal old index so the carried history matches this world pos.
    if (sp.reset == 0 && carry &&
        sp.oldIrr.Get().values[oldProbe * DDGI_IRR_RES * DDGI_IRR_RES].w > 0
        && valid) {
        moments = lerp(moments, sp.oldDistance.Get().values[oldDistIndex], h);
    }
    moments.y = max(moments.y, moments.x * moments.x);
    sp.newDistance.Get().values[distIndex] = moments;

    if (writeIrr) {
        uint index = probe * DDGI_IRR_RES * DDGI_IRR_RES + bin;
        uint oldIndex = oldProbe * DDGI_IRR_RES * DDGI_IRR_RES + bin;
        float3 E = sumIrr * (4 * DDGI_PI / float(sp.rays));
        if (sp.reset == 0 && carry && valid) {
            float4 old = sp.oldIrr.Get().values[oldIndex];
            if (old.w > 0) E = lerp(E, old.rgb, h);
        }
        sp.newIrr.Get().values[index] = valid ? float4(E, 1) : float4(0, 0, 0, 0);
    }
}
