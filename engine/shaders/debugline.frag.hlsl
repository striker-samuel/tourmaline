// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Cone-axis debug line fragment: flat push color.
// C++ mirror: app::LineDebugPush (80 bytes).
#include "gpu_platform.hlsli"

struct LineDebugPush
{
    float4x4 viewProj;
    float4 color;
};

GPU_PUSH(LineDebugPush, pcL);

struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
};

struct PSOut
{
    float4 outColor : SV_Target;
};

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    OUT.outColor = IN.color;
    return OUT;
}
