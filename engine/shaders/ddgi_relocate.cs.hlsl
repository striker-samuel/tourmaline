// SPDX-License-Identifier: MIT
#include "ddgi_common.hlsli"

// TLAS probe relocation (no SDF dependency): cast six axis rays from the
// probe; if even the largest gap is tight (the probe is enclosed by geometry,
// e.g. embedded in a roof/wall), push it toward that gap so the trace's rays
// sample open space instead of the enclosing surface. Runs per volume, before
// the trace, using the same acceleration structure the trace uses.

[[vk::binding(8, 0)]]
RaytracingAccelerationStructure rayScene;

struct DdgiRelocatePush {
    GPU_PTR_WRITE(DdgiRelocationBuffer) relocation;
    float originX, originY, originZ, spacing;
    uint dimX, dimY, dimZ, probes;
    float maxDistance;
    uint pad;
};
GPU_PUSH(DdgiRelocatePush, sp);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint probe = tid.x;
    if (probe >= sp.probes)
        return;
    const uint3 dims = uint3(sp.dimX, sp.dimY, sp.dimZ);
    const float3 origin = float3(sp.originX, sp.originY, sp.originZ);
    const float3 worldPos = origin + float3(DdgiGridCoord(probe, dims)) * sp.spacing;

    const float3 axes[6] = { float3(1, 0, 0),  float3(-1, 0, 0), float3(0, 1, 0),
                             float3(0, -1, 0), float3(0, 0, 1),  float3(0, 0, -1) };
    float largestGap = 0.0;
    int bestAxis = -1;
    [unroll]
    for (uint a = 0; a < 6; ++a) {
        RayDesc ray;
        ray.Origin = worldPos;
        ray.Direction = axes[a];
        ray.TMin = 0.001;
        ray.TMax = sp.maxDistance;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(rayScene, RAY_FLAG_NONE, 0xff, ray);
        while (q.Proceed()) {}
        const float d = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            ? q.CommittedRayT() : sp.maxDistance;
        if (d > largestGap) {
            largestGap = d;
            bestAxis = int(a);
        }
    }

    // Buried when even the largest of the six gaps is tight (the probe is
    // enclosed by geometry). Otherwise keep the grid position.
    if (bestAxis < 0 || largestGap > sp.spacing * 0.5f) {
        sp.relocation.Get().values[probe] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    // Push toward the largest gap, one spacing past the surface, clamped to
    // one spacing (the trace adds this offset to its ray origin).
    const float move = min(largestGap + sp.spacing * 0.5f, sp.spacing);
    sp.relocation.Get().values[probe] = float4(axes[bestAxis] * move, 0.0);
}
