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
    GPU_PTR_READ(DdgiRelocationBuffer) relocation; // per-probe relocation offset
    float oldOriginX, oldOriginY, oldOriginZ; // previous-frame origin (E_indirect)
    int deltaX, deltaY, deltaZ; // scroll delta (probe units) for the carry test
    uint reset; // 1 = trace everything (first frame / spacing change)
    uint pad;
    GPU_PTR_READ(GpuPositions) positions; // static vertex positions (hit normal)
    GPU_PTR_READ(GpuIndices) indices;     // static triangle indices (hit normal)
};
GPU_PUSH(DdgiTracePush, sp);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    uint rayIndex = tid.x;
    uint probe = tid.y;
    uint3 dims = uint3(sp.dimX, sp.dimY, sp.dimZ);
    if (rayIndex >= sp.rays || probe >= dims.x * dims.y * dims.z)
        return;

    // Incremental tracing: carried probes (their world position existed last
    // frame) are re-traced only on their rotating-subset turn; scrolled-in
    // probes and the reset frame trace everything. Skipped probes leave the
    // scratch stale — the blend copies the carried history instead.
    const float3 offset = sp.relocation.Get().values[probe].xyz;
    if (sp.reset == 0u) {
        uint oldProbe;
        const bool carry = DdgiProbeCarried(int3(DdgiGridCoord(probe, dims)),
                                            int3(sp.deltaX, sp.deltaY, sp.deltaZ), dims, oldProbe);
        if (carry && !DdgiProbeTraced(probe, sp.updateIndex)) {
            if (DdgiSameOrigin(offset, DdgiHistoryOffset(sp.oldIrr, oldProbe)))
                return;
        }
    }

    float3 direction = DdgiRayDirection(rayIndex, sp.rays, sp.updateIndex);
    float3 origin = float3(sp.originX, sp.originY, sp.originZ)
        + float3(DdgiGridCoord(probe, dims)) * sp.spacing
        + offset;
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
            const float3 diffuse = baseColor * (1.0f - metallic);

            const float3 hitPoint = origin + direction * result.distance;

            // Hit normal from the triangle's three vertices (flat shading),
            // fetched through the same static position/index monoblocks the
            // BLAS references (firstIndex/baseVertex map the committed
            // primitive straight to its vertices).
            const uint primIndex = q.CommittedPrimitiveIndex();
            const uint i0 = sp.indices.Get().values[mat.firstIndex + primIndex * 3u + 0u];
            const uint i1 = sp.indices.Get().values[mat.firstIndex + primIndex * 3u + 1u];
            const uint i2 = sp.indices.Get().values[mat.firstIndex + primIndex * 3u + 2u];
            const float3 p0 = sp.positions.Get().values[mat.baseVertex + i0];
            const float3 p1 = sp.positions.Get().values[mat.baseVertex + i1];
            const float3 p2 = sp.positions.Get().values[mat.baseVertex + i2];
            float3 normal = cross(p1 - p0, p2 - p0);
            const float normalLen2 = dot(normal, normal);
            normal = normalLen2 > 1e-8f ? normal * rsqrt(normalLen2) : -direction;
            if (dot(normal, direction) > 0.0f)
                normal = -normal; // face the air side (toward the ray origin)

            // Hit lighting matches the SDF tracer (ddgi_trace_sdf.cs.hlsl):
            // emissive + multi-bounce E_indirect feedback + shadow-tested
            // direct sun. No constant sky term — interior surfaces stay dark
            // because the previous frame's field there is dark.
            result.radiance = baseColor * emissive;

            float3 eIndirect = 0.0f;
            if (sp.reset == 0u) {
                eIndirect = DdgiSampleEIndirect(sp.oldIrr, sp.oldDistance,
                                                float3(sp.oldOriginX, sp.oldOriginY, sp.oldOriginZ),
                                                sp.spacing,
                                                uint3(sp.dimX, sp.dimY, sp.dimZ),
                                                hitPoint, normal);
            }
            result.radiance += diffuse * (eIndirect / DDGI_PI);

            float3 sunDir = frame.env.sunDirection.xyz;
            const float sunLen2 = dot(sunDir, sunDir);
            if (sunLen2 > 1e-8f && frame.env.sunColor.w > 0.0f) {
                sunDir *= rsqrt(sunLen2);
                const float ndl = saturate(dot(normal, sunDir));
                if (ndl > 0.0f) {
                    RayDesc sunRay;
                    sunRay.Origin = hitPoint + normal * 0.05f;
                    sunRay.Direction = sunDir;
                    sunRay.TMin = 0.0f;
                    sunRay.TMax = sp.maxDistance;
                    RayQuery<RAY_FLAG_NONE> sunQ;
                    sunQ.TraceRayInline(rayScene, RAY_FLAG_NONE, 0xff, sunRay);
                    while (sunQ.Proceed()) {}
                    const bool occluded =
                        (sunQ.CommittedStatus() == COMMITTED_TRIANGLE_HIT);
                    if (!occluded)
                        result.radiance += diffuse * frame.env.sunColor.rgb *
                            (frame.env.sunColor.w * ndl / DDGI_PI);
                }
            }
        }
    } else {
        result.radiance = DdgiSkyRadiance(frame, direction);
    }
    sp.scratch.Get().values[probe * sp.rays + rayIndex] = result;
}
