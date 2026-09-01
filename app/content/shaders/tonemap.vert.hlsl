#pragma once
#define TONEMAP_PASS
#include "common.hlsli"

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut mainVS(uint id : SV_VertexID)
{
    VSOut o;
    // Fullscreen triangle: 3 verts covering entire screen
    // uv in [0,1], pos in clip space
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    // Vulkan Y is flipped vs D3D, but our UV already accounts via pos.y = -2*uv.y+1
    // Sample UV remains 0-1 with origin top-left matching texture
    return o;
}
