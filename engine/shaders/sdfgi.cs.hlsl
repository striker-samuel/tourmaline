// SPDX-License-Identifier: MIT
#define CONTACT_SHADOW_PASS
#include "ddgi_trace_shared.hlsli"
#include "static_sdf_trace.hlsli"
#include "dof.hlsli"

// Screen-adaptive SDF surface irradiance cache (#410, one-bounce minimal):
// for each screen tile, reconstruct the visible surface from depth, then
// trace cosine-weighted hemisphere rays through the SDF with material hit
// lighting and accumulate the diffuse irradiance into a screen-space image
// the lit body samples. This is the surface-cache prototype (no persistent
// voxel cache / feedback yet), comparable to the DDGI diffuse path.

[[vk::binding(2, 0)]]
RWTexture2D<float4> sdfGiStorage[];

struct SdfGiPush {
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    GPU_PTR_READ(StaticSdfSceneHeader) scene;
    uint depthTextureIndex;
    uint outImageIndex; // bindless storage index of the irradiance image
    uint outWidth;      // tile grid width
    uint outHeight;     // tile grid height
    float invDepthW;    // 1 / full-res depth width
    float invDepthH;
    uint rays;          // hemisphere ray count per probe
    uint updateIndex;
    uint samplerIndex;
    uint pad0;
};
GPU_PUSH(SdfGiPush, sp);

// Cosine-weighted hemisphere direction around +Z (the tangent-frame normal):
// z = sqrt(u) so the sample density is proportional to cos(theta), which the
// diffuse irradiance estimate integrates with E = (pi / N) * sum(L_i) (the
// cosine is absorbed into the sampling density, so no per-ray cos factor).
float3 SdfGiRayDirection(uint ray, uint count, uint updateIndex) {
    const float u = (float(ray) + 0.5f) / float(count);
    const float v = frac(float(ray) * 0.61803398875f +
                         float(updateIndex % 4096u) * 0.754877666f);
    const float z = sqrt(saturate(u)); // cos(theta)
    const float radius = sqrt(max(0.0f, 1.0f - z * z));
    const float phi = 2.0f * 3.14159265358979f * v;
    return float3(radius * cos(phi), radius * sin(phi), z);
}

// Build an orthonormal basis from a unit normal (Z = normal).
void SdfGiBasis(float3 n, out float3 t, out float3 b) {
    const float3 up = abs(n.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= sp.outWidth || tid.y >= sp.outHeight)
        return;

    const FrameUniforms frame = sp.frameUniforms.Get();
    const StaticSdfSceneHeader scene = sp.scene.Get();

    // Receiver: tile center -> full-res uv -> y-down NDC -> world.
    const float2 tileUV = (float2(tid.xy) + 0.5f) /
                          float2(float(sp.outWidth), float(sp.outHeight));
    // The tile spans tilePx pixels; sample the depth at its center.
    const float2 uv = tileUV;
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];
    const float ndcD = depthTex.SampleLevel(smp, uv, 0).r;

    // Sky / background tiles: no surface to cache. Store the up-facing sky
    // irradiance (pi * L_sky) so any stray sample reads a sky-lit surface.
    const float3 missColor = DdgiSkyRadiance(frame, float3(0, 1, 0)) * 3.14159265358979f;
    if (DofIsBackgroundOrGarbage(ndcD)) {
        sdfGiStorage[sp.outImageIndex][tid.xy] = float4(missColor, 1.0f);
        return;
    }

    const float2 ndc = uv * 2.0f - 1.0f;
    float4 world = mul(frame.invViewProj, float4(ndc, ndcD, 1.0f));
    const float3 worldPos = world.xyz / max(world.w, 1e-9f);

    // Surface normal from the SDF gradient (central difference).
    const float h = 0.25f; // world-space finite-difference step
    const float dx = StaticSdfSceneDistance(scene, worldPos + float3(h, 0, 0)) -
                     StaticSdfSceneDistance(scene, worldPos - float3(h, 0, 0));
    const float dy = StaticSdfSceneDistance(scene, worldPos + float3(0, h, 0)) -
                     StaticSdfSceneDistance(scene, worldPos - float3(0, h, 0));
    const float dz = StaticSdfSceneDistance(scene, worldPos + float3(0, 0, h)) -
                     StaticSdfSceneDistance(scene, worldPos - float3(0, 0, h));
    // The SDF gradient points toward INCREASING distance (outward from the
    // surface, negative = inside), so normalize(dx,dy,dz) is the OUTWARD
    // surface normal — the hemisphere must gather light from that side.
    float3 normal = float3(dx, dy, dz);
    const float n2 = dot(normal, normal);
    normal = n2 > 1e-12f ? normal * rsqrt(n2) : float3(0, 1, 0);

    float3 tangent, bitangent;
    SdfGiBasis(normal, tangent, bitangent);

    // One-bounce cosine-weighted hemisphere trace with material lighting.
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    const float bias = 0.1f; // self-hit guard
    [loop] for (uint r = 0; r < sp.rays; ++r) {
        const float3 localDir = SdfGiRayDirection(r, sp.rays, sp.updateIndex);
        const float3 worldDir = tangent * localDir.x + bitangent * localDir.y +
                                normal * localDir.z;

        const StaticSdfRayHit hit = StaticSdfTrace(
            worldPos + normal * bias, worldDir, 12.0f, scene.epsilon, scene.maxSteps,
            scene.instanceCount, scene.materialCount, scene.instances, scene.nodes,
            scene.children, scene.leaves, scene.materials
#ifdef STATIC_SDF_DDGI
            , true
#endif
        );

        float3 L;
        if (!hit.valid) {
            L = DdgiSkyRadiance(frame, worldDir);
        } else {
            // Material hit lighting (the DDGI hardware-RT form, but with the
            // per-hit SDF normal): albedo * (emissive + (1-metallic) * sky).
            const float3 baseColor = max(hit.baseColor, 0.0f);
            const float3 skyAmbient = DdgiSkyIrradiance(hit.normal, frame);
            L = baseColor * (hit.emissive + (1.0f - saturate(hit.metallic)) * skyAmbient);
        }
        acc += L; // cosine absorbed into the cosine-weighted sampling density
    }

    // E = (pi / N) * sum(L_i) for cosine-weighted hemisphere sampling.
    const float3 irradiance =
        (sp.rays > 0u) ? (acc * (3.14159265358979f / float(sp.rays))) : float3(0, 0, 0);
    sdfGiStorage[sp.outImageIndex][tid.xy] = float4(irradiance, 1.0f);
}
