// Flat UV view vertex shader: the mesh is projected INTO UV space —
// the clip position comes straight from the selected UV channel, so the
// rasterized result is the unwrapped shell seen straight-on with its
// texture content (uvflat0/uvflat1.frag). Z is a constant just below
// the reversed-Z near plane (1): the fill stays behind depth-tested UV
// wires drawn by the editor line pass at z = 1.
// UVFLAT_CHANNEL is defined by the including entry file
// (uvflat0.vert.hlsl = base TEXCOORD0, uvflat1.vert.hlsl = lightmap
// uv2/TEXCOORD1).
#include "common.hlsli"
#include "vertex_pack.hlsli"

#ifndef UVFLAT_CHANNEL
#define UVFLAT_CHANNEL 0
#endif

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD1;
    uint instanceID : SV_InstanceID;
};
struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
#if UVFLAT_CHANNEL == 1
    const float2 uvFlat = IN.lightmapUv;
#else
    const float2 uvFlat = IN.uv;
#endif
    // glTF image space: (0,0) top-left; Vulkan NDC: -y is up, so uv.y=0
    // maps to -1 (screen top) — the texture reads with image orientation.
    OUT.pos = float4(uvFlat.x * 2.0 - 1.0, uvFlat.y * 2.0 - 1.0, 0.9999, 1.0);
    OUT.color = IN.color;
    OUT.uv = IN.uv;
    OUT.worldPos = float3(uvFlat, 0.0);
    OUT.normal = float3(0.0, 0.0, 1.0);
    OUT.tangent = float4(1.0, 0.0, 0.0, 1.0);
    OUT.lightmapUv = IN.lightmapUv;
    return OUT;
}
