// SPDX-License-Identifier: MIT
#pragma once
#include "static_sdf_trace.hlsli"
#include "ddgi_common.hlsli"

// Screen-space probe trace adapter: one unified hit result, two swappable
// backends. Selected at compile time by SSPROBES_RT so the SDF octree trace
// and the hardware TLAS/BLAS ray query are immediately interchangeable
// without touching the surrounding cache/reprojection/resolve logic.

struct SsProbeHit {
    bool valid;
    float3 baseColor;
    float emissive;
    float metallic;
    float3 normal;
};

#ifdef SSPROBES_RT
[[vk::binding(8, 0)]]
RaytracingAccelerationStructure rayScene;

// Hardware RT backend: inline ray query against the shared TLAS/BLAS with
// the per-TLAS-instance compact material. No per-hit normal is exposed by
// the inline query (CommittedTriangleVertexPositions is unavailable), so
// the up-facing sky is the ambient normal proxy — the DDGI hardware-RT
// convention.
SsProbeHit SsProbeTrace(GPU_PTR_READ(RayMaterialBuffer) rayMaterials,
                        float3 origin, float3 dir, float maxDist) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.001f;
    ray.TMax = maxDist;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(rayScene, RAY_FLAG_NONE, 0xff, ray);
    while (q.Proceed()) {}
    SsProbeHit result = (SsProbeHit)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        const RayMaterial m = rayMaterials.Get().items[q.CommittedInstanceIndex()];
        result.valid = true;
        result.baseColor = max(m.albedoEmissive.rgb, 0.0f);
        result.emissive = max(m.albedoEmissive.w, 0.0f);
        result.metallic = saturate(m.pbr.x);
        result.normal = float3(0.0f, 1.0f, 0.0f);
    }
    return result;
}
#else
// SDF backend: sphere-trace the static octree (a per-hit normal is available
// from the field gradient, so the hit lighting uses it directly).
SsProbeHit SsProbeTrace(StaticSdfSceneHeader scene, float3 origin, float3 dir, float maxDist) {
    StaticSdfRayHit h = StaticSdfTrace(origin, dir, maxDist, scene.epsilon,
                                       min(scene.maxSteps, 32u), scene.instanceCount,
                                       scene.materialCount, scene.instances, scene.nodes,
                                       scene.children, scene.leaves, scene.materials);
    SsProbeHit result = (SsProbeHit)0;
    result.valid = h.valid;
    result.baseColor = max(h.baseColor, 0.0f);
    result.emissive = h.emissive;
    result.metallic = saturate(h.metallic);
    result.normal = h.normal;
    return result;
}
#endif
