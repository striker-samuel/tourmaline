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
    uint pad;
};
GPU_PUSH(DdgiBlendPush, sp);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint bin = tid.x;
    const uint probe = tid.y;
    if (probe >= sp.probes || bin >= DDGI_DIST_RES * DDGI_DIST_RES || sp.rays == 0)
        return;

    const bool writeIrr = bin < DDGI_IRR_RES * DDGI_IRR_RES;
    float3 irrDir = DdgiBinDirection(min(bin, DDGI_IRR_RES * DDGI_IRR_RES - 1), DDGI_IRR_RES);
    float3 distDir = DdgiBinDirection(bin, DDGI_DIST_RES);
    float3 sumIrr = 0;
    float2 sumMoments = 0;
    float sumWeight = 0;
    uint backfaces = 0;

    for (uint r = 0; r < sp.rays; ++r) {
        DdgiRay ray = sp.scratch.Get().values[probe * sp.rays + r];
        if (ray.flags == 2) ++backfaces;
        if (writeIrr && ray.flags != 2 && all(isfinite(ray.radiance))) {
            sumIrr += max(ray.radiance, 0) * max(dot(irrDir, ray.direction), 0);
        }
        float w = pow(saturate(dot(distDir, ray.direction)), 16.0);
        float d = isfinite(ray.distance) ? clamp(ray.distance, 0, sp.maxDistance) : 0;
        sumMoments += float2(d, d*d) * w;
        sumWeight += w;
    }

    bool valid = backfaces * 4 <= sp.rays;
    float h = sp.reset != 0 ? 0 : saturate(sp.hysteresis);
    uint distIndex = probe * DDGI_DIST_RES * DDGI_DIST_RES + bin;
    float2 moments = sumWeight > 1e-8 ? sumMoments / sumWeight :
        float2(sp.maxDistance, sp.maxDistance * sp.maxDistance);
    // Use the first irradiance bin as the probe-wide validity flag.
    if (sp.reset == 0 && sp.oldIrr.Get().values[probe * DDGI_IRR_RES * DDGI_IRR_RES].w > 0
        && valid) {
        moments = lerp(moments, sp.oldDistance.Get().values[distIndex], h);
    }
    moments.y = max(moments.y, moments.x * moments.x);
    sp.newDistance.Get().values[distIndex] = moments;

    if (writeIrr) {
        uint index = probe * DDGI_IRR_RES * DDGI_IRR_RES + bin;
        float3 E = sumIrr * (4 * DDGI_PI / float(sp.rays));
        if (sp.reset == 0 && valid) {
            float4 old = sp.oldIrr.Get().values[index];
            if (old.w > 0) E = lerp(E, old.rgb, h);
        }
        sp.newIrr.Get().values[index] = valid ? float4(E, 1) : float4(0, 0, 0, 0);
    }
}
