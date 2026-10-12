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
#include "vertex_pack.hlsli"
#include "gfxfilter.hlsli"

struct FilteredVsMainPush
{
    // Preserve the scene push bytes this VS does not use (model + frame
    // addresses, 192 bytes); the tail below is what the app pushes at
    // offset 192 (see smvk::gfx::kFilteredVsPushOffset).
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    // Offsets mirror C++ smvk::gfx::FilteredVsPush exactly (32B tail;
    // the trailing lmXforms keeps the 24B prefix identical to the motion
    // VS tail, whose own FrameUniforms BDA lands at 216).
    vk::BufferPointer<GfxDraws> draws;
    vk::BufferPointer<RenderViewBuffer> renderViews;
    uint renderViewIndex;
    uint pad;
    vk::BufferPointer<GfxChartXforms> lmXforms;
};

[[vk::push_constant]]
ConstantBuffer<FilteredVsMainPush> pcFV;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    // Lightmaps are deprecated: StaticVertexFormat no longer binds the
    // second UV channel (TexCoord1/ChartId would be locations 5/6), so
    // this VS declares neither. The gated-off LightmappedStaticVertexFormat
    // keeps the option; the MESH leg (gfxfilter.mesh.hlsl) still reads the
    // chart words straight from the shading heap, untouched.
    uint instanceID : SV_InstanceID;
};
struct VSOut
{
    float4 pos : SV_Position;

    // COLOR0 must be a full float4 at Location 0: the lit FS reads a
    // vec4 (layer-weight normalization divides by the RGBA sum), and a
    // vec3 here leaves Location 0 component 3 undeclared — the
    // VUID-RuntimeSpirv-OpEntryPoint-08743 interface mismatch. Pass the
    // vertex color through (same StaticVertexFormat stream default.vert
    // forwards): the filter is transparent draw compaction, so tint +
    // layer weights must survive it exactly like the direct path.
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    // Lightmap chunk for this draw (flat: constant over the triangle);
    // 0xFFFFFFFF = no lightmap (dynamic / unbaked draws).
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
    // Dither-on-closeup flag (flat; Scene::ditherCloseup): the Draw ring's
    // per-draw state this frame (GFX_DRAW_DITHER_CLOSEUP) forwarded so the
    // lit FS family reads ONE channel on both paths (default.vert delivers
    // the direct legs' pc word at the same location).
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    GfxDraw draw = pcFV.draws.Get().draws[IN.instanceID];
    float4x4 model = draw.model;

    float4 world = mul(model, float4(IN.position, 1.0));
    OUT.worldPos = world.xyz;
    float4x4 viewProj =
        pcFV.renderViews.Get().views[pcFV.renderViewIndex].m_viewProjectionMatrix;
    OUT.pos = mul(viewProj, world);

    OUT.color = IN.color;
    OUT.uv = IN.uv;
    // Lightmaps deprecated: no chart UV rides this format (see VSIn) —
    // the atlas unpack the draw's lmChunk would gate stays a constant
    // zero (the FS gates on lmChunk none and never samples it). The
    // lmXforms BDA stays declared in the push tail for layout identity
    // with the C++ mirror; nothing reads it here.
    OUT.lightmapUv = float2(0.0f, 0.0f);

    // Lightmap chunk rides the draw record (misc.w; 0xFFFFFFFF = none).
    OUT.lmChunk = draw.misc.w;

    // Dither-on-closeup per-draw flag rides the ring (misc.y bit 1 —
    // GFX_DRAW_DITHER_CLOSEUP; set at PushDraw time together with the
    // MASKED bucket routing for the depth legs).
    OUT.ditherCloseup = (draw.misc.y & GFX_DRAW_DITHER_CLOSEUP) ? 1u : 0u;

    // Packed shading stream: normal/tangent ride as R32_UInt oct words.
    float3 normal = OctDecodeNormal(IN.normalPacked);
    float4 tangent = OctDecodeTangent(IN.tangentPacked);

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

        worldNormal = mul(adj / det, normal);
    }
    else
    {
        worldNormal = mul(M3, normal);
    }
    OUT.normal = normalize(worldNormal);

    float3 worldTangent = mul(M3, tangent.xyz);
    float handedness = tangent.w * (det < 0.0f ? -1.0f : 1.0f);
    OUT.tangent = float4(normalize(worldTangent), handedness);

    return OUT;
}
