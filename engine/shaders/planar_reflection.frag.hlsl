// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Full-resolution HDR planar mirror receiver composite.
// Receives a COLOR capture from a reflected camera, produced by the host.
// The main camera's depth decides whether the visible surface lies on the
// receiver plane; a nearby wall/object must not acquire the mirror image.
#define PLANAR_REFLECTION_PASS
#include "common.hlsli"
#include "gpu_platform.hlsli"

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// C++ twin: Engine/Rendergraph/planar_reflection.h (128 bytes).
struct PlanarReflectionPush
{
    GPU_PTR_READ(FrameUniforms) frameUniforms;  // 0
    uint2 reserved;                             // 8, align the matrix
    float4x4 reflectedViewProjection;           // 16
    float4 receiverPlane;                       // 80, normalized xyz
    uint hdrTextureIndex;                       // 96
    uint captureTextureIndex;
    uint depthTextureIndex;
    uint samplerIndex;
    float strength;                             // 112
    float thickness;
    float f0;
    float pad0;
};
GPU_PUSH(PlanarReflectionPush, pp);

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    Texture2D hdr = textures[NonUniformResourceIndex(pp.hdrTextureIndex)];
    Texture2D capture = textures[NonUniformResourceIndex(pp.captureTextureIndex)];
    Texture2D depthTex = textures[NonUniformResourceIndex(pp.depthTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(pp.samplerIndex)];

    float4 base = hdr.SampleLevel(smp, IN.uv, 0.0f);
    OUT.outColor = float4(base.rgb, 1.0f);

    // Reversed-Z background (0) / invalid depth is never a receiver.
    const float depth = depthTex.Load(int3(int2(IN.pos.xy), 0)).r;
    if (!(depth > 1e-6f && depth <= 1.0f))
        return OUT;

    FrameUniforms frame = pp.frameUniforms.Get();
    // Vulkan projection is already Y-flipped: framebuffer top = NDC -1.
    const float2 ndc = IN.uv * 2.0f - 1.0f;
    const float4 worldH = mul(frame.invViewProj, float4(ndc, depth, 1.0f));
    if (!(abs(worldH.w) > 1e-8f))
        return OUT;
    const float3 world = worldH.xyz / worldH.w;

    // Reject every pixel not on the authored receiver plane. The narrow
    // finite tolerance accounts for geometric tessellation and float depth
    // reconstruction; falloff hides hard quantization at the boundary.
    const float planeDistance =
        abs(dot(pp.receiverPlane.xyz, world) + pp.receiverPlane.w);
    const float thickness = max(pp.thickness, 0.001f);
    if (!(planeDistance < thickness))
        return OUT;
    const float planeWeight = 1.0f -
        smoothstep(0.5f * thickness, thickness, planeDistance);

    // Project through the SAME reflected VP used to produce the capture.
    const float4 clip = mul(pp.reflectedViewProjection, float4(world, 1.0f));
    if (!(clip.w > 1e-6f))
        return OUT;
    const float3 projected = clip.xyz / clip.w;
    // Reject out-of-frustum taps; never clamp a reflection to its edge.
    if (projected.x < -1.0f || projected.x > 1.0f ||
        projected.y < -1.0f || projected.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f)
        return OUT;
    const float2 captureUv = projected.xy * 0.5f + 0.5f;

    // Schlick reflectance for the plane, with a mirror F0=1 default.
    const float3 toEye = normalize(frame.cameraPos - world);
    const float cosTheta = saturate(abs(dot(pp.receiverPlane.xyz, toEye)));
    const float fresnel = pp.f0 +
        (1.0f - pp.f0) * pow(1.0f - cosTheta, 5.0f);
    const float weight = saturate(pp.strength * fresnel * planeWeight);
    const float3 reflection = capture.SampleLevel(smp, captureUv, 0.0f).rgb;
    OUT.outColor = float4(lerp(base.rgb, reflection, weight), 1.0f);
    return OUT;
}
