// MULTIVIEW direct-path VS (the quad-viewport lean secondary views):
// default.vert.hlsl's twin with ONE change — the view/projection is
// selected PER VIEW LAYER via SV_ViewID instead of the single pushed
// render-view index. The recording pass carries a view mask (bit i =
// array layer i); pc.renderViewIndex is the BASE index of the
// secondary views' contiguous RenderView slab entries, so view i of the
// mask reads views[base + i]. Everything else (vertex streams, normal
// unpack, lightmap/chart passthrough, push layout) mirrors
// default.vert.hlsl exactly so the same fragment shaders link
// (default_unlit.frag for the lean unlit view; shadow's conservative
// FS for the wireframe depth-fill prepass).
#define DEFAULT_DIRECT_PASS
#include "common.hlsli"
#include "vertex_pack.hlsli"
#include "gfxfilter.hlsli"

struct MViewVsPush
{
    float4x4 model;
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<LightBuffer> bLights;
    vk::BufferPointer<MaterialsBuffer> bMats;
    vk::BufferPointer<DirectionalCascadeBuffer> cascadeBuffer;
    uint materialId;
    uint comparisonSamplerId;
    uint numCascades;
    uint cascadeIndex;
    vk::BufferPointer<ForwardPlusLightBuffer> fpLights;
    vk::BufferPointer<ForwardPlusTileCountBuffer> fpTileCounts;
    vk::BufferPointer<ForwardPlusTileIndexBuffer> fpTileIndices;
    uint fpTileCountX;
    uint fpLightCount;
    uint fpTileSize;
    uint fpMaxPerTile;
    vk::BufferPointer<SpotShadowBuffer> spotShadowBuffer;
    uint spotShadowMapIndex;
    uint spotShadowCount;
    uint spotIndex;
    uint pad_pc;
    vk::BufferPointer<RenderViewBuffer> renderViews;
    uint mainRenderView;
    // BASE RenderView index for this multiview set (see the header
    // comment); view layer i reads views[renderViewIndex + i].
    uint renderViewIndex;
    uint hasXforms;
    uint chartOffset;
    vk::BufferPointer<GfxChartXforms> lmXforms;
    float4 _padTail[2];
    uint ditherCloseup;
};

[[vk::push_constant]]
ConstantBuffer<MViewVsPush> pcMV;

struct MVIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    [[vk::location(5)]] float2 chartUv : TEXCOORD1;
    [[vk::location(6)]] uint chartId : TEXCOORD5;
    uint instanceID : SV_InstanceID;
    // MULTIVIEW: the layer being rasterized (valid because the pipeline
    // carries the pass's nonzero view mask).
    uint viewID : SV_ViewID;
};
struct VSOut
{
    float4 pos : SV_Position;

    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
};

VSOut mainVS(MVIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pcMV.frameUniforms.Get();

    float4 world = mul(pcMV.model, float4(IN.position, 1.0));
    OUT.worldPos = world.xyz;

    // Per-view VP: base + layer index (contiguous slab entries,
    // registered in view order each frame — see EnsureMViewTargets'
    // caller). Falls back to the frame slab only when the base is
    // invalid (defensive; the recorder always pushes a valid base).
    float4x4 viewProj = frame.viewProj;
    if (pcMV.renderViewIndex < MAX_RENDER_VIEWS)
    {
        uint vi = pcMV.renderViewIndex + IN.viewID;
        if (vi < MAX_RENDER_VIEWS)
            viewProj = pcMV.renderViews.Get().views[vi].m_viewProjectionMatrix;
    }
    float4 clip = mul(viewProj, world);
    OUT.pos = clip;

    OUT.color = IN.color;
    OUT.uv = IN.uv;
    OUT.lightmapUv = (pcMV.hasXforms == 0u)
        ? IN.chartUv
        : GfxLightmapUv(IN.chartUv, IN.chartId, pcMV.chartOffset, pcMV.lmXforms);
    OUT.lmChunk = 0xFFFFFFFFu;
    OUT.ditherCloseup = pcMV.ditherCloseup;

    float3 normal = OctDecodeNormal(IN.normalPacked);
    float4 tangent = OctDecodeTangent(IN.tangentPacked);

    float3x3 M3 = (float3x3) pcMV.model;

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

        float3x3 invTrans = adj / det;
        worldNormal = mul(invTrans, normal);
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
