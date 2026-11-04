// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Cone-axis debug lines: pos-only stream, flat push color.
// Used with VertexLayoutId::DebugLine + LineList or PointList.
// C++ mirror: app::LineDebugPush (80 bytes).
#include "gpu_platform.hlsli"

struct LineDebugPush
{
    float4x4 viewProj;
    float4 color;
};

GPU_PUSH(LineDebugPush, pcL);

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    // Vulkan point rasterization requires an explicit size unless the
    // maintenance5 default-point-size feature is enabled.
    [[vk::builtin("PointSize")]] float pointSize : TEXCOORD1;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    OUT.pos = mul(pcL.viewProj, float4(IN.position, 1.0f));
    OUT.color = pcL.color;
    OUT.pointSize = 1.0f;
    return OUT;
}
