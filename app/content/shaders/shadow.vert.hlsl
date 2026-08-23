#include "common.hlsli"

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(4)]] float4 tangent : TANGENT;
    uint instanceID : SV_InstanceID;
};
struct VSOut
{
    float4 pos : SV_Position;

    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pc.frameUniforms.Get();
    Light firstLight = pc.bLights.Get().lights[0];

    // World position
    float4 world = mul(pc.model, float4(IN.position, 1.0));
    OUT.worldPos = world.xyz;

    float4 clip = mul(firstLight.lightVP, world);
    OUT.pos = clip;

    OUT.color = float3(1.0f, 1.0f, 1.0f);
    OUT.uv = IN.uv;

    // Properly transform normal to world space
    float3 worldNormal = mul((float3x3) pc.model, IN.normal);
    OUT.normal = normalize(worldNormal);

    return OUT;
}
