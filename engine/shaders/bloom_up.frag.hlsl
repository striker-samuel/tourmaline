// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define BLOOM_PASS
#include "common.hlsli"
#include "gpu_platform.hlsli"

struct PSIn
{
    float2 uv : TEXCOORD0;
};

struct PSOut
{
    float4 outColor : SV_Target0;
};

// Must match Engine/Rendergraph/bloom.h BloomUpPush (32 bytes).
struct BloomUpPush
{
    uint srcTextureIndex;
    uint samplerIndex;
    float radius;
    float pad0;
    float2 srcTexelSize;
    uint pad1;
    uint pad2;
};

GPU_PUSH(BloomUpPush, bp);

// 9-tap tent upsample of the coarser mip. Output is ADDITIVELY blended
// onto the finer mip (Blend::Add), so alpha stays 0.
float3 TentUpsample(Texture2D src, SamplerState smp, float2 uv, float2 texel, float radius)
{
    float x = radius * texel.x;
    float y = radius * texel.y;

    float3 a = src.Sample(smp, uv + float2(-x, -y)).rgb;
    float3 b = src.Sample(smp, uv + float2( 0.0f, -y)).rgb;
    float3 c = src.Sample(smp, uv + float2( x, -y)).rgb;
    float3 d = src.Sample(smp, uv + float2(-x,  0.0f)).rgb;
    float3 e = src.Sample(smp, uv).rgb;
    float3 f = src.Sample(smp, uv + float2( x,  0.0f)).rgb;
    float3 g = src.Sample(smp, uv + float2(-x,  y)).rgb;
    float3 h = src.Sample(smp, uv + float2( 0.0f,  y)).rgb;
    float3 k = src.Sample(smp, uv + float2( x,  y)).rgb;

    // 1-2-1 tent weights / 16.
    return (a * 1.0f + b * 2.0f + c * 1.0f
          + d * 2.0f + e * 4.0f + f * 2.0f
          + g * 1.0f + h * 2.0f + k * 1.0f) * (1.0f / 16.0f);
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    Texture2D src = textures[NonUniformResourceIndex(bp.srcTextureIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(bp.samplerIndex)];

    float3 c = TentUpsample(src, smp, IN.uv, bp.srcTexelSize, bp.radius);
    OUT.outColor = float4(c, 0.0f);
    return OUT;
}
