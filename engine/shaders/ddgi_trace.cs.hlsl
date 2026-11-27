// SPDX-License-Identifier: MIT
#define CONTACT_SHADOW_PASS
#include "ddgi_trace_shared.hlsli"

[[vk::binding(8, 0)]]
RaytracingAccelerationStructure rayScene;

// Hardware-RT DDGI trace: sky misses + material hit lighting (compact
// per-instance albedo/emissive/metallic), plus distance moments for the
// Chebyshev visibility.
struct DdgiTracePush {
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    GPU_PTR_WRITE(DdgiRayBuffer) scratch;
    float originX, originY, originZ, spacing;
    uint dimX, dimY, dimZ, rays;
    float maxDistance;
    uint updateIndex;
    GPU_PTR_READ(DdgiIrrBuffer) oldIrr;           // E_indirect feedback (prev frame)
    GPU_PTR_READ(DdgiDistanceBuffer) oldDistance; // E_indirect visibility moments
    GPU_PTR_READ(RayMaterialBuffer) rayMaterials; // per-TLAS-instance material
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

    DdgiRay result = DdgiEmptyRay(direction, sp.maxDistance);
    FrameUniforms frame = sp.frameUniforms.Get();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        result.distance = q.CommittedRayT();
        const bool frontFace = q.CommittedTriangleFrontFace();
        result.flags = frontFace ? 1 : 2;
        if (frontFace) {
            // Compact per-instance material (flat baseColor, no texture).
            const uint instanceId = q.CommittedInstanceIndex();
            const RayMaterial mat = sp.rayMaterials.Get().items[instanceId];
            const float3 baseColor = max(mat.albedoEmissive.rgb, 0.0f);
            const float emissive = max(mat.albedoEmissive.w, 0.0f);
            const float metallic = saturate(mat.pbr.x);

            // One-bounce material hit lighting: emissive + diffuse sky
            // reflection. No per-hit normal in this shader, so the up-facing
            // sky is a constant proxy for the incoming sky irradiance.
            // (E_indirect feedback + direct sun are the P2 follow-on.)
            const float3 skyAmbient = DdgiSkyRadiance(frame, float3(0.0f, 1.0f, 0.0f));
            result.radiance =
                baseColor * (emissive + (1.0f - metallic) * skyAmbient);
        }
    } else {
        result.radiance = DdgiSkyRadiance(frame, direction);
    }
    sp.scratch.Get().values[probe * sp.rays + rayIndex] = result;
}
