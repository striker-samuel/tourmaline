// Filtered-path vertex shader: draws the per-view compacted index list
// produced by gfxfilter.cs.hlsl via DrawIndexedIndirect.
//
// Differences vs default.vert.hlsl:
//   - model matrix comes from the GfxDraws buffer indexed by SV_InstanceID.
//     The filter writes firstInstance = drawIndex into each indirect command
//     (see gfxfilter.cs.hlsl), so instanceID == drawIndex with instanceCount 1.
//   - viewProj resolves through the render-view slab (same as default.vert).
//   - Push: 24B FilteredVsMainPush at byte offset 192 (the main pass'
//     unused prevModel scratch), leaving the 256B scene push intact for the
//     fragment shader. All other shading (normal/tangent) is identical.
#define GFXFILTERED_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"

struct FilteredVsMainPush
{
    // Preserve the scene push bytes this VS does not use (model + frame
    // addresses, 192 bytes); the tail below is what the app pushes at
    // offset 192 (see smvk::gfx::kFilteredVsPushOffset).
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    vk::BufferPointer<GfxDraws> draws;
    vk::BufferPointer<RenderViewBuffer> renderViews;
    uint renderViewIndex;
    uint pad;
};

[[vk::push_constant]]
ConstantBuffer<FilteredVsMainPush> pcFV;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
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
    float4x4 model = pcFV.draws.Get().draws[IN.instanceID].model;

    float4 world = mul(model, float4(IN.position, 1.0));
    OUT.worldPos = world.xyz;
    float4x4 viewProj =
        pcFV.renderViews.Get().views[pcFV.renderViewIndex].m_viewProjectionMatrix;
    OUT.pos = mul(viewProj, world);

    OUT.color = float3(1.0f, 1.0f, 1.0f);
    OUT.uv = IN.uv;

    float3x3 M3 = (float3x3) model;
    float det = M3[0][0] * (M3[1][1] * M3[2][2] - M3[1][2] * M3[2][1])
              - M3[0][1] * (M3[1][0] * M3[2][2] - M3[1][2] * M3[2][0])
              + M3[0][2] * (M3[1][0] * M3[2][1] - M3[1][1] * M3[2][0]);

    float3 worldNormal;
    if (abs(det) > 1e-12f)
    {
        float3x3 adj;
        adj[0][0] =  (M3[1][1] * M3[2][2] - M3[1][2] * M3[2][1]);
        adj[0][1] = -(M3[1][0] * M3[2][2] - M3[1][2] * M3[2][0]);
        adj[0][2] =  (M3[1][0] * M3[2][1] - M3[1][1] * M3[2][0]);

        adj[1][0] = -(M3[0][1] * M3[2][2] - M3[0][2] * M3[2][1]);
        adj[1][1] =  (M3[0][0] * M3[2][2] - M3[0][2] * M3[2][0]);
        adj[1][2] = -(M3[0][0] * M3[2][1] - M3[0][1] * M3[2][0]);

        adj[2][0] =  (M3[0][1] * M3[1][2] - M3[0][2] * M3[1][1]);
        adj[2][1] = -(M3[0][0] * M3[1][2] - M3[0][2] * M3[1][0]);
        adj[2][2] =  (M3[0][0] * M3[1][1] - M3[0][1] * M3[1][0]);

        worldNormal = mul(adj / det, IN.normal);
    }
    else
    {
        worldNormal = mul(M3, IN.normal);
    }
    OUT.normal = normalize(worldNormal);

    float3 worldTangent = mul(M3, IN.tangent.xyz);
    float handedness = IN.tangent.w * (det < 0.0f ? -1.0f : 1.0f);
    OUT.tangent = float4(normalize(worldTangent), handedness);

    return OUT;
}
