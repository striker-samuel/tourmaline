#include "common.hlsli"

// Motion vector VS: outputs the current clip position (SV_Position) and the
// previous-frame clip position as a separate interpolated attribute.
// The FS computes the UV delta from both; computing it per-vertex would be
// screen-space linear interpolation and wrong under perspective (the
// rasterizer perspective-corrects each attribute independently).
// prevViewProj comes from FrameUniforms, prevModel from push constants.

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};
struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 prevClip : PREVCLIP;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pc.frameUniforms.Get();

    float4 world = mul(pc.model, float4(IN.position, 1.0));
    OUT.pos = mul(frame.viewProj, world);

    float4 prevWorld = mul(pc.prevModel, float4(IN.position, 1.0));
    OUT.prevClip = mul(frame.prevViewProj, prevWorld);

    // uv passes through for the FS alpha test (MASK cutout).
    OUT.uv = IN.uv;
    return OUT;
}
