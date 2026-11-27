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
    uint updateIndex;
    GPU_PTR_READ(DdgiRelocationBuffer) relocation;
};
GPU_PUSH(DdgiBlendPush, sp);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint bin = tid.x;
    const uint probe = tid.y;
    if (probe >= sp.probes || bin >= DDGI_DIST_RES * DDGI_DIST_RES || sp.rays == 0)
        return;

    const uint3 dims = uint3(sp.dimX, sp.dimY, sp.dimZ);
    uint oldProbe = 0u;
    const bool carry = DdgiProbeCarried(int3(DdgiGridCoord(probe, dims)),
                                       int3(sp.deltaX, sp.deltaY, sp.deltaZ), dims, oldProbe);
    const float3 offset = sp.relocation.Get().values[probe].xyz;
    bool sameOrigin = false;
    if (carry && sp.reset == 0u)
        sameOrigin = DdgiSameOrigin(offset, DdgiHistoryOffset(sp.oldIrr, oldProbe));

    const bool writeIrr = bin < DDGI_IRR_BINS;
    const uint index = probe * DDGI_IRR_STRIDE + bin;
    const uint oldIndex = oldProbe * DDGI_IRR_STRIDE + bin;
    const uint distIndex = probe * DDGI_DIST_RES * DDGI_DIST_RES + bin;
    const uint oldDistIndex = oldProbe * DDGI_DIST_RES * DDGI_DIST_RES + bin;
    // Publish exactly the ray origin associated with this history slot.
    if (bin == 0u)
        sp.newIrr.Get().values[probe * DDGI_IRR_STRIDE + DDGI_IRR_BINS] = float4(offset, 0.0f);

    // Keep the trace and blend skip conditions identical.
    if (sameOrigin && !DdgiProbeTraced(probe, sp.updateIndex)) {
        if (writeIrr)
            sp.newIrr.Get().values[index] = sp.oldIrr.Get().values[oldIndex];
        sp.newDistance.Get().values[distIndex] = sp.oldDistance.Get().values[oldDistIndex];
        return;
    }

    const float3 irrDir = DdgiBinDirection(min(bin, DDGI_IRR_BINS - 1u), DDGI_IRR_RES);
    const float3 distDir = DdgiBinDirection(bin, DDGI_DIST_RES);
    float3 sumIrr = 0.0f;
    float sumCos = 0.0f;
    float2 sumMoments = 0.0f;
    float sumWeight = 0.0f;
    uint completed = 0u;
    uint backfaces = 0u;

    for (uint r = 0; r < sp.rays; ++r) {
        const DdgiRay ray = sp.scratch.Get().values[probe * sp.rays + r];
        // Unknown rays are neither sky nor visibility measurements.
        if (ray.flags >= 3u || !isfinite(ray.distance) || !all(isfinite(ray.direction)) ||
            (ray.flags < 2u && !all(isfinite(ray.radiance))))
            continue;
        ++completed;
        if (ray.flags == 2u)
            ++backfaces;
        if (writeIrr) {
            const float cosine = max(dot(irrDir, ray.direction), 0.0f);
            sumCos += cosine;
            if (ray.flags < 2u)
                sumIrr += max(ray.radiance, 0.0f) * cosine;
        }
        const float w = pow(saturate(dot(distDir, ray.direction)), 16.0f);
        const float d = clamp(ray.distance, 0.0f, sp.maxDistance);
        sumMoments += float2(d, d * d) * w;
        sumWeight += w;
    }

    const bool embedded = completed > 0u && backfaces * 4u >= completed;
    const bool complete = completed * 4u >= sp.rays * 3u;
    bool oldValid = false;
    if (sameOrigin)
        oldValid = sp.oldIrr.Get().values[oldProbe * DDGI_IRR_STRIDE].w > 0.0f;

    // Never keep lighting from an origin now classified inside geometry.
    // A temporarily incomplete trace may keep history only at the same origin.
    if (embedded || !complete) {
        const bool keep = !embedded && oldValid;
        float2 moments = 0.0f;
        if (keep)
            moments = sp.oldDistance.Get().values[oldDistIndex];
        sp.newDistance.Get().values[distIndex] = moments;
        if (writeIrr) {
            float4 irr = 0.0f;
            if (keep)
                irr = sp.oldIrr.Get().values[oldIndex];
            sp.newIrr.Get().values[index] = irr;
        }
        return;
    }

    const float h = oldValid ? saturate(sp.hysteresis) : 0.0f;
    // An unsupported directional bin is not evidence of an open ray.
    float2 moments = 0.0f;
    if (sumWeight > 1e-8f) {
        moments = sumMoments / sumWeight;
        if (oldValid)
            moments = lerp(moments, sp.oldDistance.Get().values[oldDistIndex], h);
    } else if (oldValid) {
        moments = sp.oldDistance.Get().values[oldDistIndex];
    }
    moments.y = max(moments.y, moments.x * moments.x);
    sp.newDistance.Get().values[distIndex] = moments;

    if (writeIrr) {
        // Cosine-weighted average times pi is irradiance, not radiance.
        // Constant incident radiance integrates exactly even with few rays.
        float3 E = 0.0f;
        if (sumCos > 1e-8f)
            E = sumIrr * (DDGI_PI / sumCos);
        if (oldValid) {
            const float3 old = sp.oldIrr.Get().values[oldIndex].rgb;
            E = sumCos > 1e-8f ? lerp(E, old, h) : old;
        }
        // Scrolled-in or relocated probes publish their own fresh measurements.
        sp.newIrr.Get().values[index] = float4(E, 1.0f);
    }
}
