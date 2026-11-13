// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Flat UV (diffuse) fragment: sample the material's baseColor texture
// at the unwrapped uv, tinted by the baseColor factor — the "what does
// the diffuse layout look like over its texture" view. Alpha cutout +
// dithered Blend discard match the lit path so shells agree.
#include "common.hlsli"
#include "material_coverage.hlsli"

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    [[vk::location(7)]] nointerpolation uint meshMaterial : TEXCOORD5;
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

    float3 c = mat.diffuse.rgb;
    float alpha = mat.diffuse.a * IN.color.a;
    const int baseColorIdx = mat.tex.x;
    if (baseColorIdx >= 0)
    {
        const uint slot = (uint)baseColorIdx;
        Texture2D baseTex = textures[NonUniformResourceIndex(slot)];
        const float4 t = baseTex.Sample(samplers[0], IN.uv);
        c *= t.rgb;
        alpha *= t.a;
    }

    // Alpha cutout + dithered Blend (matches lit): the ONE keep test.
    const float alphaCutoff = mat.params.x;
    const float shadingModelF = mat.params.w;
    const float alphaModeF = mat.params2.w;
    const bool ditheredTransp = (pc.pad_pc == 0);
    if (!TM_CoverageKeep(alpha, alphaCutoff, shadingModelF, alphaModeF,
                         ditheredTransp, clipPos.xy))
        discard;

    OUT.outColor = float4(SanitizeHdr(c), 1.0);
    return OUT;
}
