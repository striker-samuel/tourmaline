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

// RT hit-normal reconstruction inputs (see WorldRenderer + ray_tracing_world):
// per-instance row-major 3x4 transform + geometry offsets, and the shared
// static position/index monoblocks as raw uint words.
struct RtInstanceData {
    float4 transformRow0; // row-major 3x4: [m00 m01 m02 tx]
    float4 transformRow1; //                     [m10 m11 m12 ty]
    float4 transformRow2; //                     [m20 m21 m22 tz]
    uint4 geometry;       // x = firstIndex, y = baseVertex, z = vertexCount, w = pad
};
struct RtInstanceBuffer { RtInstanceData items[1]; };
struct StaticVertexPosWords { uint words[1]; }; // 3 words (R32G32B32) per vertex
struct StaticIndexWords { uint words[1]; };     // 1 word (UINT32) per index

#ifdef SSPROBES_RT
[[vk::binding(8, 0)]]
RaytracingAccelerationStructure rayScene;

// Hardware RT backend: inline ray query against the shared TLAS/BLAS with
// the per-TLAS-instance compact material. The geometric hit normal is
// reconstructed from the triangle's three vertices (fetched via the static
// monoblocks + the instance transform) — inline ray query exposes the
// primitive index but not the committed vertex positions.
SsProbeHit SsProbeTrace(GPU_PTR_READ(RayMaterialBuffer) rayMaterials,
                        GPU_PTR_READ(RtInstanceBuffer) instances,
                        GPU_PTR_READ(StaticVertexPosWords) vertexPos,
                        GPU_PTR_READ(StaticIndexWords) indices,
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
        const uint instanceId = q.CommittedInstanceIndex();
        const RayMaterial m = rayMaterials.Get().items[instanceId];
        result.valid = true;
        result.baseColor = max(m.albedoEmissive.rgb, 0.0f);
        result.emissive = max(m.albedoEmissive.w, 0.0f);
        result.metallic = saturate(m.pbr.x);

        // Reconstruct the geometric normal: pull the three indices, fetch the
        // positions, transform to world, and take the world-space edge cross
        // product (after normalization this is the correct transformed normal
        // for any invertible transform, non-uniform scale included).
        const RtInstanceData inst = instances.Get().items[instanceId];
        const uint firstIndex = inst.geometry.x;
        const uint baseVertex = inst.geometry.y;
        const uint tri = q.CommittedPrimitiveIndex();
        const uint i0 = indices.Get().words[firstIndex + tri * 3u + 0u];
        const uint i1 = indices.Get().words[firstIndex + tri * 3u + 1u];
        const uint i2 = indices.Get().words[firstIndex + tri * 3u + 2u];
        const float3 v0 = asfloat(uint3(vertexPos.Get().words[(baseVertex + i0) * 3u + 0u],
                                        vertexPos.Get().words[(baseVertex + i0) * 3u + 1u],
                                        vertexPos.Get().words[(baseVertex + i0) * 3u + 2u]));
        const float3 v1 = asfloat(uint3(vertexPos.Get().words[(baseVertex + i1) * 3u + 0u],
                                        vertexPos.Get().words[(baseVertex + i1) * 3u + 1u],
                                        vertexPos.Get().words[(baseVertex + i1) * 3u + 2u]));
        const float3 v2 = asfloat(uint3(vertexPos.Get().words[(baseVertex + i2) * 3u + 0u],
                                        vertexPos.Get().words[(baseVertex + i2) * 3u + 1u],
                                        vertexPos.Get().words[(baseVertex + i2) * 3u + 2u]));
        const float3 w0 = float3(dot(inst.transformRow0.xyz, v0) + inst.transformRow0.w,
                                 dot(inst.transformRow1.xyz, v0) + inst.transformRow1.w,
                                 dot(inst.transformRow2.xyz, v0) + inst.transformRow2.w);
        const float3 w1 = float3(dot(inst.transformRow0.xyz, v1) + inst.transformRow0.w,
                                 dot(inst.transformRow1.xyz, v1) + inst.transformRow1.w,
                                 dot(inst.transformRow2.xyz, v1) + inst.transformRow2.w);
        const float3 w2 = float3(dot(inst.transformRow0.xyz, v2) + inst.transformRow0.w,
                                 dot(inst.transformRow1.xyz, v2) + inst.transformRow1.w,
                                 dot(inst.transformRow2.xyz, v2) + inst.transformRow2.w);
        float3 n = cross(w1 - w0, w2 - w0);
        const float n2 = dot(n, n);
        n = n2 > 1e-15f ? n * rsqrt(n2) : -dir;
        if (dot(n, dir) > 0.0f)
            n = -n; // face the probe (against the ray), the SDF convention
        result.normal = n;
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
