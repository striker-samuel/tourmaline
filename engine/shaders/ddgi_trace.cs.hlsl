// SPDX-License-Identifier: MIT
#define CONTACT_SHADOW_PASS
#include "common.hlsli"
#include "ddgi_common.hlsli"

[[vk::binding(8, 0)]]
RaytracingAccelerationStructure rayScene;

// Diagnostic first stage: sky misses and opaque occlusion. Material hit
// lighting is deliberately not fabricated; do not enable in Main yet.
struct DdgiTracePush {
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    GPU_PTR_WRITE(DdgiRayBuffer) scratch;
    float originX, originY, originZ, spacing;
    uint dimX, dimY, dimZ, rays;
    float maxDistance;
    uint updateIndex;
};
GPU_PUSH(DdgiTracePush, sp);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    uint rayIndex = tid.x;
    uint probe = tid.y;
    uint3 dims = uint3(sp.dimX, sp.dimY, sp.dimZ);
    if (rayIndex >= sp.rays || probe >= dims.x * dims.y * dims.z)
        return;

    float3 direction = DdgiRayDirection(rayIndex, sp.rays, sp.updateIndex);
    float3 origin = float3(sp.originX, sp.originY, sp.originZ)
        + float3(DdgiGridCoord(probe, dims)) * sp.spacing;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = sp.maxDistance;
    // Nearest committed hit, not first hit: distance moments need nearest geometry.
    // Do not force opacity over the engine's opacity micromaps.
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(rayScene, RAY_FLAG_NONE, 0xff, ray);
    while (q.Proceed()) {}

    DdgiRay result;
    result.direction = direction;
    result.distance = sp.maxDistance;
    result.radiance = 0;
    result.flags = 0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        result.distance = q.CommittedRayT();
        result.flags = q.CommittedTriangleFrontFace() ? 1 : 2;
    } else {
        FrameUniforms frame = sp.frameUniforms.Get();
        float t = pow(saturate(direction.y), max(frame.env.skyHorizon.w, 0.001));
        // Diagnostic gradient; the final tracer must share Main's sky evaluation.
        result.radiance = max(0, lerp(frame.env.skyHorizon.rgb,
                                      frame.env.skyZenith.rgb, t) * frame.env.skyZenith.w);
    }
    sp.scratch.Get().values[probe * sp.rays + rayIndex] = result;
}
