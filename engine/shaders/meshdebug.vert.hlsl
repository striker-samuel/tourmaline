// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Meshlet debug view: flat per-cluster color (see GfxRenderer debug UI).
// Standalone push (shares the global [0,256) range): model + viewProj +
// color. C++ mirror: app::MeshDebugPush (144 bytes).
struct MeshDebugPush
{
    float4x4 model;
    float4x4 viewProj;
    float4 color;
};

[[vk::push_constant]]
ConstantBuffer<MeshDebugPush> pcD;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    float4 world = mul(pcD.model, float4(IN.position, 1.0f));
    OUT.pos = mul(pcD.viewProj, world);
    OUT.color = pcD.color;
    return OUT;
}
