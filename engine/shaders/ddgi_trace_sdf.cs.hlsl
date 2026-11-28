// SPDX-License-Identifier: MIT
#define CONTACT_SHADOW_PASS
#define STATIC_SDF_DDGI
#include "ddgi_trace_shared.hlsli"
#include "static_sdf_trace.hlsli"

// Independent pipeline: no acceleration structure descriptor or RayQuery.
struct DdgiSdfTracePush {
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    GPU_PTR_WRITE(DdgiRayBuffer) scratch;
    float originX, originY, originZ, spacing;
    uint dimX, dimY, dimZ, rays;
    float maxDistance;
    uint updateIndex;
    GPU_PTR_READ(DdgiIrrBuffer) oldIrr;           // E_indirect feedback (prev frame)
    GPU_PTR_READ(DdgiDistanceBuffer) oldDistance; // E_indirect visibility moments
    GPU_PTR_READ(RayMaterialBuffer) rayMaterials; // unused (SDF leaf carries material)
    GPU_PTR_READ(DdgiRelocationBuffer) relocation; // per-probe relocation offset
    float oldOriginX, oldOriginY, oldOriginZ; // previous-frame origin (E_indirect)
    int deltaX, deltaY, deltaZ; // scroll delta (probe units) for the carry test
    uint reset; // 1 = trace everything (first frame / spacing change)
    uint pad;
    GPU_PTR_READ(GpuPositions) positions; // unused (SDF carries geometry)
    GPU_PTR_READ(GpuIndices) indices;     // unused
    GPU_PTR_READ(StaticSdfSceneHeader) scene;
};
GPU_PUSH(DdgiSdfTracePush, sp);

StaticSdfRayHit DdgiSdfTrace(StaticSdfSceneHeader scene, float3 origin,
                           float3 direction, float distance, bool occlusionOnly) {
    return StaticSdfTrace(origin, direction, distance, scene.epsilon,
        scene.maxSteps, scene.instanceCount, scene.materialCount,
        scene.instances, scene.nodes, scene.children, scene.leaves,
        scene.materials, occlusionOnly);
}

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    uint rayIndex = tid.x;
    uint probe = tid.y;
    uint3 dims = uint3(sp.dimX, sp.dimY, sp.dimZ);
    if (rayIndex >= sp.rays || probe >= dims.x * dims.y * dims.z)
        return;
    // Incremental tracing (see ddgi_trace.cs.hlsl): carried probes trace only
    // on their rotating-subset turn; the blend copies the rest.
    if (sp.reset == 0u) {
        uint oldProbe;
        const bool carry = DdgiProbeCarried(int3(DdgiGridCoord(probe, dims)),
                                            int3(sp.deltaX, sp.deltaY, sp.deltaZ), dims, oldProbe);
        if (carry && !DdgiProbeTraced(probe, sp.updateIndex))
            return;
    }
    float3 direction = DdgiRayDirection(rayIndex, sp.rays, sp.updateIndex);
    float3 origin = float3(sp.originX, sp.originY, sp.originZ)
        + float3(DdgiGridCoord(probe, dims)) * sp.spacing;
    DdgiRay result = DdgiEmptyRay(direction, sp.maxDistance);
    StaticSdfSceneHeader scene = sp.scene.Get();
    FrameUniforms frame = sp.frameUniforms.Get();
    // Caller must guarantee coverage, not merely a nonempty upload.
    if (scene.instanceCount == 0 || scene.materialCount == 0 || scene.maxSteps == 0) {
        result.flags = 3;
    } else {
        const float tMin = 0.001f;
        StaticSdfRayHit hit = DdgiSdfTrace(scene, origin + direction * tMin,
            direction, sp.maxDistance - tMin, false);
        if (hit.incomplete) {
            result.flags = 3; // unknown is not sky
        } else if (!hit.valid) {
            result.radiance = DdgiSkyRadiance(frame, direction);
        } else {
            result.distance = min(hit.t + tMin, sp.maxDistance);
            result.flags = hit.frontFace ? 1 : 2;
            if (hit.frontFace) {
                // Compact material: no UV or HDR emission.
                result.radiance = hit.baseColor * hit.emissive;
                const float3 diffuse = hit.baseColor * (1.0f - saturate(hit.metallic));
                // E_indirect: multi-bounce feedback from the previous frame's
                // irradiance field (the color-bleed + shadow-fill term).
                float3 eIndirect = 0.0f;
                if (sp.reset == 0u)
                    eIndirect = DdgiSampleEIndirect(
                    sp.oldIrr, sp.oldDistance,
                    float3(sp.oldOriginX, sp.oldOriginY, sp.oldOriginZ), sp.spacing,
                    uint3(sp.dimX, sp.dimY, sp.dimZ), hit.hitPoint, hit.normal);
                result.radiance += diffuse * (eIndirect / DDGI_PI);
                float3 sunDir = frame.env.sunDirection.xyz;
                float sunLength2 = dot(sunDir, sunDir);
                if (sunLength2 > 1e-8f && frame.env.sunColor.w > 0.0f) {
                    sunDir *= rsqrt(sunLength2);
                    float ndl = saturate(dot(hit.normal, sunDir));
                    if (ndl > 0.0f) {
                        float bias = max(scene.originBias, max(2.0f * scene.epsilon,
                            2.0f * hit.worldVoxel));
                        StaticSdfRayHit shadow = DdgiSdfTrace(scene,
                            hit.hitPoint + hit.normal * bias, sunDir,
                            sp.maxDistance, true);
                        if (!shadow.valid && shadow.incomplete) {
                            result.flags = 3;
                        } else if (!shadow.valid) {
                            result.radiance += diffuse * frame.env.sunColor.rgb *
                                (frame.env.sunColor.w * ndl / DDGI_PI);
                        }
                    }
                }
            }
        }
    }
    sp.scratch.Get().values[probe * sp.rays + rayIndex] = result;
}
