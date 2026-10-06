// Tiled terrain FULL-detail vertex shader (near tiles).
//
// VSIn locations follow TerrainVertexFormat's attribute order exactly
// (mesh.h: locations are assigned in attribute-list order):
//   0 = POSITION      (R32G32B32_FLOAT)
//   1 = NORMAL        (R16G16B16A16_FLOAT, half xyz, w pad)
//   2 = TANGENT_BFLIP (R16G16B16A16_FLOAT, half xyz + bitangent flip w)
//   3 = COLOR         (R8G8B8A8_UNORM, albedo tint)
// The terrain rides the SCENE push constants (common.hlsli PushConstants):
// vertices are world-space (model = identity, never read), viewProj comes
// from the RenderView system, the env block from FrameUniforms.
#include "common.hlsli"
#include "environment.hlsli"

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 normal : NORMAL;
    [[vk::location(2)]] float4 tangentBFlip : TANGENT;
    [[vk::location(3)]] float4 color : COLOR0;
};

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 tangentBFlip : TEXCOORD2;
    [[vk::location(3)]] float4 color : COLOR1;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pc.frameUniforms.Get();

    OUT.worldPos = IN.position;

    // Authoritative VP: the main camera's RenderView (see default.vert).
    float4x4 viewProj = frame.viewProj;
    if (pc.mainRenderView < MAX_RENDER_VIEWS)
        viewProj = pc.renderViews.Get().views[pc.mainRenderView]
            .m_viewProjectionMatrix;
    OUT.pos = mul(viewProj, float4(IN.position, 1.0));

    OUT.normal = IN.normal.xyz;
    OUT.tangentBFlip = IN.tangentBFlip;
    OUT.color = IN.color;
    return OUT;
}
