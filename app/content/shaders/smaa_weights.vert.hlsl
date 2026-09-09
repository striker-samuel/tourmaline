#define SMAA_PASS
#include "common.hlsli"

#include "smaa_push.hlsli"

#define SMAA_PRESET_HIGH
#define SMAA_VULKAN_BINDLESS
#define SMAA_INCLUDE_PS 0
#include "smaa.hlsli"

// Fullscreen triangle + official blending-weight offsets + pixel
// coordinates (SMAABlendingWeightCalculationVS).
struct VSOut
{
    float4 pos : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 offset0 : TEXCOORD1;
    float4 offset1 : TEXCOORD2;
    float4 offset2 : TEXCOORD3;
    float2 pixcoord : TEXCOORD4;
};

VSOut mainVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    o.texcoord = uv;
    float4 offset[3];
    float2 pixcoord;
    SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
    o.offset0 = offset[0];
    o.offset1 = offset[1];
    o.offset2 = offset[2];
    o.pixcoord = pixcoord;
    return o;
}
