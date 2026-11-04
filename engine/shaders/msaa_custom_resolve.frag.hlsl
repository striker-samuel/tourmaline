// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Baseline programmable resolve: exactly two equally weighted linear HDR samples.
// Later edge reconstruction must preserve sample coverage at silhouettes.
#define TONEMAP_PASS
#include "common.hlsli"
#include "gpu_platform.hlsli"
struct MsaaResolvePush { uint colorIndex; };
GPU_PUSH(MsaaResolvePush, pc);
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 mainPS(PSIn input) : SV_Target0
{
    Texture2DMS<float4> src = texturesMS[pc.colorIndex];
    int2 pixel = int2(input.pos.xy);
    return 0.5f * (src.Load(pixel, 0) + src.Load(pixel, 1));
}
