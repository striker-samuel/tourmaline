// Tiled terrain LOWER-detail vertex shader (far tiles).
//
// VSIn locations follow TerrainLowVertexFormat's attribute order exactly
// (mesh.h): 0 = POSITION, 1 = COLOR — the SAME 32-byte records the full
// format binds, with the NORMAL/TANGENT_BFLIP attributes simply not
// fetched (the far pipeline skips two attribute loads per vertex).
// World-space vertices; viewProj from the RenderView system (see
// terrain.vert.hlsl).
#include "common.hlsli"
#include "environment.hlsli"

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
};

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR1;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pc.frameUniforms.Get();

    OUT.worldPos = IN.position;

    float4x4 viewProj = frame.viewProj;
    if (pc.mainRenderView < MAX_RENDER_VIEWS)
        viewProj = pc.renderViews.Get().views[pc.mainRenderView]
            .m_viewProjectionMatrix;
    OUT.pos = mul(viewProj, float4(IN.position, 1.0));

    OUT.color = IN.color;
    return OUT;
}
