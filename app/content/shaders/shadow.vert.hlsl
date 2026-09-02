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

    float4 world = mul(pc.model, float4(IN.position, 1.0f));

    // Upgraded: fetch per-cascade VP from cascade buffer when cascades active
    float4x4 lightVP;
    if (pc.numCascades > 0 && pc.cascadeIndex < (uint)pc.numCascades)
    {
        GpuDirectionalLightDescription dir = pc.cascadeBuffer.Get().dir;
        lightVP = dir.cascades[pc.cascadeIndex].lightVP;
    }
    else
    {
        Light light = pc.bLights.Get().lights[0];
        lightVP = light.lightVP;
    }

    float4 clip = mul(lightVP, world);
    clip.z = max(clip.z, 0.0f);
    OUT.pos = clip;
    return OUT;
}