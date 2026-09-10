#define SMAA_PASS
#include "common.hlsli"

#include "smaa_push.hlsli"

#define SMAA_PRESET_HIGH
#define SMAA_VULKAN_BINDLESS
#define SMAA_INCLUDE_PS 0
#include "smaa.hlsli"

// Fullscreen triangle + official neighborhood-blending offset
// (SMAANeighborhoodBlendingVS).
struct VSOut
{
    float4 pos : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 offset : TEXCOORD1;
};

VSOut mainVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    o.texcoord = uv;
    float4 offset;
    SMAANeighborhoodBlendingVS(uv, offset);
    o.offset = offset;
    return o;
}
