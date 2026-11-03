// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// 05_grass ground mesh. A one-time CPU-authored grid that shares the
// world height sampler with the GPU-scattered grass. The one-million
// grass instances themselves are NEVER created on the CPU.
#include "gpu_platform.hlsli"

struct GrassGroundPush { float4x4 viewProj; };
GPU_PUSH(GrassGroundPush, pc);

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};
struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
};
VSOut mainVS(VSIn IN)
{
    VSOut outV;
    outV.pos = mul(pc.viewProj, float4(IN.position, 1.0f));
    outV.color = IN.color.rgb;
    outV.uv = IN.uv;
    outV.worldPos = IN.position;
    outV.normal = float3(0.0f, 1.0f, 0.0f);
    return outV;
}
