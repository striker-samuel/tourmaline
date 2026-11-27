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
    uint prevImageIndex; // history[writeSlot ^ 1] (sampled read)
    uint outImageIndex;  // history[writeSlot] (storage write)
    uint outWidth;       // tile grid width
    uint outHeight;      // tile grid height
    float invDepthW;     // 1 / full-res depth width
    float invDepthH;
    uint rays;           // hemisphere ray count per probe (per update)
    uint updateIndex;
    uint samplerIndex;
    float blendAlpha;    // EMA blend factor (1 = full replace)
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

    // Tile center -> full-res uv -> y-down NDC -> world.
    const float2 uv = (float2(tid.xy) + 0.5f) /
                      float2(float(sp.outWidth), float(sp.outHeight));

    // Previous history value for this tile (the EMA feedback source). Each
    // thread reads/writes only its own texel, but the sampled read and the
    // storage write target DIFFERENT ping-pong images (no in-place hazard).
    Texture2D prevTex = textures[NonUniformResourceIndex(sp.prevImageIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(sp.samplerIndex)];
    // Nonfinite-history rejection: NaN/Inf would persist through the EMA
    // forever (lerp(NaN, x, t) == NaN for any t). Explicit zero-init should
    // prevent it, but guard anyway so a bad frame can always recover.
    const float3 prevRaw = prevTex.SampleLevel(smp, uv, 0).rgb;
    const float3 prevE = all(isfinite(prevRaw)) ? prevRaw : float3(0.0f, 0.0f, 0.0f);

    // Rotating subset: only 1/4 of tiles trace each frame (the amortized
    // surface-cache schedule); the rest copy their history unchanged. The
    // interleaved (x+y)&3 stride measured faster than warp-coalesced groups
    // (the march is memory-latency-bound, so spreading the traced threads
    // across every warp hides the octree-access latency better).
    if (((tid.x + tid.y) & 3u) != (sp.updateIndex & 3u)) {
        sdfGiStorage[sp.outImageIndex][tid.xy] = float4(prevE, 1.0f);
        return;
    }

    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    const float ndcD = depthTex.SampleLevel(smp, uv, 0).r;

    // Sky / background tiles: no surface to cache. Store the up-facing sky
    // irradiance (pi * L_sky) so any stray sample reads a sky-lit surface.
    const float3 missColor = DdgiSkyRadiance(frame, float3(0, 1, 0)) * 3.14159265358979f;
    if (DofIsBackgroundOrGarbage(ndcD)) {
        sdfGiStorage[sp.outImageIndex][tid.xy] =
            float4(lerp(prevE, missColor, sp.blendAlpha), 1.0f);
        return;
    }

    const float2 ndc = uv * 2.0f - 1.0f;
    float4 world = mul(frame.invViewProj, float4(ndc, ndcD, 1.0f));
    const float3 worldPos = world.xyz / max(world.w, 1e-9f);

    // Surface normal from the depth buffer's screen-space derivatives: a
    // smooth geometric normal (no SDF octree quantization noise — the SDF
    // gradient carries per-leaf stair-stepping that read as high-frequency
    // detail in the diffuse field). The derivative offset spans the tile
    // footprint (16px / 2) so the normal is a tile-scale average, not the
    // 1px micro-detail that a full-res derivative would pick up.
    //
    // A derivative tap is rejected when it is background OR crosses a depth
    // discontinuity (a silhouette / foreground-background step), so a thin
    // occluder's normal can't be stolen by the surface behind it. Each axis
    // then falls back to a one-sided derivative (or the up normal) instead
    // of mixing two unrelated depths.
    const float2 stepX = float2(8.0f * sp.invDepthW, 0.0f);
    const float2 stepY = float2(0.0f, 8.0f * sp.invDepthH);
    const float dL = depthTex.SampleLevel(smp, uv - stepX, 0).r;
    const float dR = depthTex.SampleLevel(smp, uv + stepX, 0).r;
    const float dU = depthTex.SampleLevel(smp, uv - stepY, 0).r;
    const float dD = depthTex.SampleLevel(smp, uv + stepY, 0).r;
    const float discThreshold = max(0.05f, ndcD * 0.2f);
    const bool dLok = !DofIsBackgroundOrGarbage(dL) && abs(dL - ndcD) < discThreshold;
    const bool dRok = !DofIsBackgroundOrGarbage(dR) && abs(dR - ndcD) < discThreshold;
    const bool dUok = !DofIsBackgroundOrGarbage(dU) && abs(dU - ndcD) < discThreshold;
    const bool dDok = !DofIsBackgroundOrGarbage(dD) && abs(dD - ndcD) < discThreshold;
    float3 normal = float3(0, 1, 0);
    if (dLok || dRok || dUok || dDok) {
        float3 pL = worldPos, pR = worldPos, pU = worldPos, pD = worldPos;
        if (dLok) {
            float4 w = mul(frame.invViewProj, float4((uv - stepX) * 2.0f - 1.0f, dL, 1.0f));
            pL = w.xyz / max(w.w, 1e-9f);
        }
        if (dRok) {
            float4 w = mul(frame.invViewProj, float4((uv + stepX) * 2.0f - 1.0f, dR, 1.0f));
            pR = w.xyz / max(w.w, 1e-9f);
        }
        if (dUok) {
            float4 w = mul(frame.invViewProj, float4((uv - stepY) * 2.0f - 1.0f, dU, 1.0f));
            pU = w.xyz / max(w.w, 1e-9f);
        }
        if (dDok) {
            float4 w = mul(frame.invViewProj, float4((uv + stepY) * 2.0f - 1.0f, dD, 1.0f));
            pD = w.xyz / max(w.w, 1e-9f);
        }
        // One-sided fallback: a rejected tap degenerates its axis's derivative
        // toward the centre (the valid side still contributes).
        const float3 tx = (dLok && dRok) ? (pR - pL) : (dRok ? (pR - worldPos)
                                                            : (pL - worldPos));
        const float3 ty = (dUok && dDok) ? (pD - pU) : (dDok ? (pD - worldPos)
                                                            : (pU - worldPos));
        normal = cross(tx, ty);
        const float n2 = dot(normal, normal);
        normal = n2 > 1e-12f ? normal * rsqrt(n2) : float3(0, 1, 0);
        // Orient toward the camera (the visible side of the surface).
        if (dot(normal, frame.cameraPos - worldPos) < 0.0f)
            normal = -normal;
    }

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
            worldPos + normal * bias, worldDir, 3.0f, scene.epsilon, min(scene.maxSteps, 32u),
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
    const float3 irradianceRaw =
        (sp.rays > 0u) ? (acc * (3.14159265358979f / float(sp.rays))) : float3(0, 0, 0);
    // Reject a non-finite sample (e.g. a degenerate hit normal) so a single
    // bad frame cannot poison the history through the EMA.
    const float3 irradiance = all(isfinite(irradianceRaw)) ? irradianceRaw : prevE;
    sdfGiStorage[sp.outImageIndex][tid.xy] =
        float4(lerp(prevE, irradiance, sp.blendAlpha), 1.0f);
}
