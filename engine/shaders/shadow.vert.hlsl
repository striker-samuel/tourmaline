#include "common.hlsli"

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 pos : SV_Position;

    // Consumed only by shadow.frag (alpha-tested cutout depth). The
    // depth-only program has no FS; extra varyings are legal there
    // (same pattern as default.vert with the Forward+ depth program).
    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;

    float4 world = mul(pc.model, float4(IN.position, 1.0f));

    OUT.color = IN.color.rgb;
    OUT.uv = IN.uv;

    // RenderView path: every shadow pass owns an allocated view (one per
    // directional cascade / spot tile); its VP is the single source of
    // truth. The legacy descriptor paths below stay as fallbacks for
    // pipelines driven without the RenderView system.
    float4x4 lightVP;
    if (pc.renderViewIndex < MAX_RENDER_VIEWS)
    {
        lightVP = pc.renderViews.Get().views[pc.renderViewIndex].m_viewProjectionMatrix;
    }
    else if (pc.spotIndex != 0xFFFFFFFFu && pc.spotShadowCount > 0u && pc.spotIndex < pc.spotShadowCount)
    {
        lightVP = pc.spotShadowBuffer.Get().tiles[pc.spotIndex].lightVP;
    }
    else if (pc.numCascades > 0 && pc.cascadeIndex < (uint)pc.numCascades)
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