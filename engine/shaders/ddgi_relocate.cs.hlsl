// SPDX-License-Identifier: MIT
#include "ddgi_common.hlsli"

// Escape an embedded origin through the closest observed backface exit.
// Keep the offset within its probe cell; classification handles probes that
// cannot be relocated safely.

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
    float closestExit = sp.maxDistance;
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
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT &&
            !q.CommittedTriangleFrontFace() && q.CommittedRayT() < closestExit) {
            closestExit = q.CommittedRayT();
            bestAxis = int(a);
        }
    }

    const float move = closestExit + max(0.002f, sp.spacing * 0.05f);
    float3 offset = 0.0f;
    if (bestAxis >= 0 && move <= sp.spacing * 0.45f)
        offset = axes[bestAxis] * move;
    sp.relocation.Get().values[probe] = float4(offset, 0.0f);
}
