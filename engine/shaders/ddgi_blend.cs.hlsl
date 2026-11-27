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
};
GPU_PUSH(DdgiBlendPush, sp);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint bin = tid.x;
    const uint probe = tid.y;
    if (probe >= sp.probes || bin >= DDGI_DIST_RES * DDGI_DIST_RES || sp.rays == 0)
        return;

    // Toroidal carry + incremental subset (see ddgi_common.hlsli): a probe at
    // grid coord c lived at c + delta last frame. Carried probes re-trace only
    // on their rotating-subset turn; scrolled-in probes and the reset frame
    // always trace. Carried-but-not-traced probes copy their old history.
    const uint3 dims = uint3(sp.dimX, sp.dimY, sp.dimZ);
    uint oldProbe = 0u;
    const bool carry = DdgiProbeCarried(int3(DdgiGridCoord(probe, dims)),
                                        int3(sp.deltaX, sp.deltaY, sp.deltaZ), dims, oldProbe);
    const bool traced = sp.reset != 0u || !carry || DdgiProbeTraced(probe, sp.updateIndex);

    const bool writeIrr = bin < DDGI_IRR_RES * DDGI_IRR_RES;
    const uint distIndex = probe * DDGI_DIST_RES * DDGI_DIST_RES + bin;

    // Carried + not traced this frame: pure scroll — copy the carried history
    // forward unchanged (the trace left this probe's scratch stale).
    if (carry && !traced) {
        if (writeIrr)
            sp.newIrr.Get().values[probe * DDGI_IRR_RES * DDGI_IRR_RES + bin] =
                sp.oldIrr.Get().values[oldProbe * DDGI_IRR_RES * DDGI_IRR_RES + bin];
        sp.newDistance.Get().values[distIndex] =
            sp.oldDistance.Get().values[oldProbe * DDGI_DIST_RES * DDGI_DIST_RES + bin];
        return;
    }

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
    // distance collapses (surrounded on all sides).
    const float meanDist = sumDist / float(max(sp.rays, 1u));
    const bool valid =
        incompleteCount < (sp.rays / 4u + 1u) && meanDist > sp.maxDistance * 0.025f;
    // Carried + traced: blend hysteresis against the shifted old history;
    // scrolled-in / reset probes start fresh (h = 0).
    const float h = (carry && sp.reset == 0) ? saturate(sp.hysteresis) : 0.0;
    uint oldDistIndex = oldProbe * DDGI_DIST_RES * DDGI_DIST_RES + bin;
    const bool oldValid = carry && sp.reset == 0 &&
        sp.oldIrr.Get().values[oldProbe * DDGI_IRR_RES * DDGI_IRR_RES].w > 0;
    float2 moments = sumWeight > 1e-8 ? sumMoments / sumWeight :
        float2(sp.maxDistance, sp.maxDistance * sp.maxDistance);
    if (valid) {
        if (oldValid)
            moments = lerp(moments, sp.oldDistance.Get().values[oldDistIndex], h);
    } else if (oldValid) {
        // A single-frame invalid trace (borderline meanDist under the rotating
        // rays) must not discard the probe's converged history — keep the old
        // moments so the probe reads as it did instead of flashing black.
        moments = sp.oldDistance.Get().values[oldDistIndex];
    }
    moments.y = max(moments.y, moments.x * moments.x);
    sp.newDistance.Get().values[distIndex] = moments;

    if (writeIrr) {
        uint index = probe * DDGI_IRR_RES * DDGI_IRR_RES + bin;
        uint oldIndex = oldProbe * DDGI_IRR_RES * DDGI_IRR_RES + bin;
        if (valid) {
            float3 E = sumIrr * (4 * DDGI_PI / float(sp.rays));
            if (oldValid) {
                float4 old = sp.oldIrr.Get().values[oldIndex];
                E = lerp(E, old.rgb, h);
            }
            sp.newIrr.Get().values[index] = float4(E, 1);
        } else if (oldValid) {
            sp.newIrr.Get().values[index] = sp.oldIrr.Get().values[oldIndex];
        } else {
            sp.newIrr.Get().values[index] = float4(0, 0, 0, 0);
        }
    }
}
