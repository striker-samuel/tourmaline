#include "common.hlsli"

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;

    Light light = pc.bLights.Get().lights[0];

    float4 world =
        mul(pc.model, float4(IN.position, 1.0f));

    float4 clip =
        mul(light.lightVP, world);

    clip.z = max(clip.z, 0.0f);

    OUT.pos = clip;

    return OUT;
}