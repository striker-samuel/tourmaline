// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include "light.hlsli"
#include "common.hlsli"
#include "material_coverage.hlsli"
#include "shadows.hlsli"

// Static-SDF reflection scene debug view (ViewMode::SdfScene, combo/env
// only): a per-fragment sphere trace of the uploaded static SDF scene
// (static_sdf_trace.hlsli) from the camera, shaded with the SDF instance's
// constant base color + a simple sun term. This shows the SDF
// representation independently of the rasterized mesh (the SDF surface is
// reconstructed from the sidecar volume, not the triangles). MAGENTA = the
// SDF residency not published (frame.staticSdfScene == 0) or empty.
//
// Pairs with default.vert.hlsl (same PSIn as default_lit — the family
// contract). Mask cutout + Blend dither + the closeup dissolve match the
// lit path so silhouettes agree. Exposure-compensated (the readout travels
// through HDR + tonemap); no fog (flat debug views stay unfogged).

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(7)]] nointerpolation uint meshMaterial : TEXCOORD5;
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    bool isFrontFace : SV_IsFrontFace;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(float4 clipPos : SV_Position, PSIn IN)
{
    PSOut OUT;

    MaterialValues mat = pc.bMats.Get().mats[IN.meshMaterial];

    // Match the lit path's closeup dissolve (default_unlit's block).
    if (IN.ditherCloseup != 0u)
    {
        FrameUniforms frameEarly = pc.frameUniforms.Get();
        FramePSHot hotEarly = frameEarly.frameHot.Get();
        if (!CloseupDitherKeep(IN.worldPos, hotEarly.cameraPos, clipPos.xy))
            discard;
    }

    // Masked cutout + dithered Blend (matches lit/cascade debug).
    float alphaCutoff = mat.params.x;
    float shadingModelF = mat.params.w;
    float baseAlpha = mat.diffuse.a * IN.color.a;
    int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        baseAlpha *= baseTex.Sample(samplers[0], IN.uv).a;
    }
    const float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    const bool isBlendMat = (alphaModeF == 2.0f);
    if (!TM_CoverageKeep(baseAlpha, alphaCutoff, shadingModelF, alphaModeF,
                         ditheredTransp, clipPos.xy))
        discard;

    FrameUniforms frame = pc.frameUniforms.Get();
    FramePSHot hot = frame.frameHot.Get();
    Light light = pc.bLights.Get().lights[0];

    const float3 missColor = float3(0.05f, 0.05f, 0.10f);

    // No SDF residency published / empty scene: LOUD magenta, never a
    // silent black (the bring-up lesson).
    if (frame.staticSdfEnabled == 0u)
    {
        float3 color = float3(1.0f, 0.05f, 1.0f);
        color /= max(frame.exposure, 1e-6f);
        OUT.outColor = float4(color, (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
        return OUT;
    }

    StaticSdfSceneHeader sdfScene = frame.staticSdfScene.Get();
    if (sdfScene.instanceCount == 0u)
    {
        float3 color = float3(1.0f, 0.05f, 1.0f);
        color /= max(frame.exposure, 1e-6f);
        OUT.outColor = float4(color, (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
        return OUT;
    }

    // Per-fragment view ray: from the camera through this pixel (the lit
    // fragment's world position gives the ray direction for its pixel).
    float3 rayOrigin = hot.cameraPos;
    float3 rayDir = IN.worldPos - rayOrigin;
    const float rayLen = length(rayDir);
    if (rayLen < 1e-5f)
    {
        float3 color = missColor;
        color /= max(frame.exposure, 1e-6f);
        OUT.outColor = float4(color, (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
        return OUT;
    }
    rayDir /= rayLen;

    // Trace at least to the rasterized fragment, and never less than a
    // modest world distance so nearby SDF surfaces past the fragment resolve.
    const float maxTrace = max(rayLen, 10.0f);

    StaticSdfRayHit hit = StaticSdfTrace(rayOrigin, rayDir, maxTrace,
                                         sdfScene.epsilon, sdfScene.maxSteps,
                                         sdfScene.instanceCount, sdfScene.materialCount,
                                         sdfScene.instances, sdfScene.samples,
                                         sdfScene.materials);

    float3 color;
    if (hit.valid)
    {
        const float3 L = normalize(-light.direction);
        const float NdotL = saturate(dot(hit.normal, L));
        const float3 albedo = max(hit.baseColor, 0.0f);
        color = albedo * (0.15f + 0.85f * NdotL) + albedo * hit.emissive;
    }
    else
    {
        color = missColor; // no SDF surface along this ray
    }

    // Exposure compensation (readout -> HDR): pre-scale by 1/exposure so the
    // SDF readout survives the hdr * exposure -> ACES path.
    color /= max(frame.exposure, 1e-6f);

    OUT.outColor = float4(color, (isBlendMat && ditheredTransp) ? 1.0f : baseAlpha);
    return OUT;
}
