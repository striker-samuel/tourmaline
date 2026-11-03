// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Hardware ray-query directional shadows: triangle intersections against
// the RHI-owned Vulkan KHR TLAS. Unlike the contact march, casters need
// not be visible in screen-space depth. CSM/EVSM remains the fallback.
#define CONTACT_SHADOW_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "gpu_platform.hlsli"

// Same storage format / visibility index / push ABI as contact shadows;
// the renderer selects exactly one producer of this image per frame.
[[vk::binding(2, 0)]]
RWTexture2D<float> contactStorage[];

[[vk::binding(8, 0)]]
RaytracingAccelerationStructure rayScene;

// Must stay byte-identical to ContactShadowPush in contact_shadow.h.
// Fields unused by hardware tracing are kept for the shared pass.
struct ContactShadowPush
{
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    uint depthTextureIndex;
    uint outImageIndex;
    uint outWidth;
    uint outHeight;
    float invDepthW;
    float invDepthH;
    float maxDistance;
    float thickness;
    float startBias;
    uint steps;
    uint samplerIndex;
    uint pad0;
};
GPU_PUSH(ContactShadowPush, sp);

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= sp.outWidth || tid.y >= sp.outHeight)
        return;
    const FrameUniforms frame = sp.frameUniforms.Get();
    const float2 uv = (float2(tid.xy) + 0.5f) /
        float2(sp.outWidth, sp.outHeight);
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    SamplerState hardSampler = samplers[NonUniformResourceIndex(sp.samplerIndex)];
    const float depth = depthTex.SampleLevel(hardSampler, uv, 0).r;

    // Skip sky, invalid depth, and empty receivers. They are always lit.
    float visibility = 1.0f;
    if (!DofIsBackgroundOrGarbage(depth))
    {
        const float2 ndc = uv * 2.0f - 1.0f;
        const float4 world = mul(frame.invViewProj, float4(ndc, depth, 1.0f));
        const float3 worldPos = world.xyz / max(world.w, 1e-9f);
        const float3 toSun = normalize(frame.env.sunDirection.xyz + 1e-9f);

        // Hardware traversal uses the complete scene TLAS. Avoid a
        // zero-distance intersection with the receiver's own triangle.
        RayDesc ray;
        ray.Origin = worldPos + toSun * max(sp.startBias, 0.001f);
        ray.Direction = toSun;
        ray.TMin = 0.001f;
        ray.TMax = max(sp.maxDistance, 0.01f);

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(rayScene, RAY_FLAG_NONE, 0xff, ray);
        while (q.Proceed()) { }
        visibility = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
    }

    contactStorage[sp.outImageIndex][tid.xy] = visibility;
}
