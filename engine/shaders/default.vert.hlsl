// Direct-path VS: declares its own push block (DEFAULT_DIRECT_PASS
// excludes the scene pc from this stage — see common.hlsli). Bytes
// 0..192 mirror the scene PushConstants exactly (same field order as
// common.hlsli::PushConstants); bytes 192..208 carry the per-mesh chart
// tail (the scene push's unused prevModel scratch — same overlay
// precedent as FilteredVsPush). Passes that draw lightmapped content
// direct (editor Baked/Uv1 views) push the tail per mesh; every other
// pass pushes the zeroed scratch, which reads as BDA 0 and selects the
// raw chart-uv passthrough below (their fragments gate on lmChunk none
// and never sample lightmaps). The fragment stage keeps the scene pc —
// same split as the filtered pipe.
#define DEFAULT_DIRECT_PASS
#include "common.hlsli"
#include "vertex_pack.hlsli"
#include "gfxfilter.hlsli"

struct DefaultVsPush
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
    uint renderViewIndex;
    // Per-mesh chart tail (pushed at byte 192 per mesh; hasXforms == 0,
    // the zeroed-scratch default, selects the passthrough below).
    uint hasXforms;
    uint chartOffset;
    vk::BufferPointer<GfxChartXforms> lmXforms;
    // Byte 240 = PushConstants::padEnd_.x (the per-draw overlay scratch's
    // last word; the motion tail 192..240 stops short of it): the DIRECT
    // legs' per-draw dither-on-closeup flag (Scene::ditherCloseup — the
    // legs push it per draw, 0/1). Forwarded flat at location 8 so the FS
    // family reads ONE channel on both paths (the filtered legs deliver
    // the ring's GFX_DRAW_DITHER_CLOSEUP at the same location).
    float4 _padTail[2]; // 208..240: unused prevModel scratch tail
    uint ditherCloseup; // 240
};

[[vk::push_constant]]
ConstantBuffer<DefaultVsPush> pcDB;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    // Location 5 = StaticVertexFormat's TexCoord1 (chart-local lightmap
    // UV), location 6 = ChartId (appended last so 0..4 stay stable).
    // Shaders that don't declare them just ignore the attributes. The
    // direct path has no draw ring / transform buffer, so it forwards
    // the chart UV raw (debug views show chart space); direct draws are
    // never lightmapped (lmChunk none below).
    [[vk::location(5)]] float2 chartUv : TEXCOORD1;
    [[vk::location(6)]] uint chartId : TEXCOORD5;
    uint instanceID : SV_InstanceID;
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
    // Lightmap chunk (flat). The direct-draw path has no draws ring to
    // resolve a per-mesh chunk from, so direct draws are never
    // lightmapped (0xFFFFFFFF = none); the filtered path (gfxfiltered.
    // vert) carries the real per-draw chunk. Static/dynamic GI
    // separation keys off this + FrameUniforms.lmEnabled.
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
    // Dither-on-closeup flag (flat; Scene::ditherCloseup): the direct
    // legs' per-draw pc word (byte 240) forwarded so the FS family reads
    // one channel on both paths (the filtered legs deliver the Draw
    // ring's flag at the same location). FSs that do not declare it
    // simply do not fetch it.
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pcDB.frameUniforms.Get();

    // World position
    float4 world = mul(pcDB.model, float4(IN.position, 1.0));
    OUT.worldPos = world.xyz;

    // View / projection: the main camera's RenderView is the authoritative
    // VP for scene passes (allocated per frame, see renderview.hlsli).
    // FrameUniforms stays as the fallback for pipelines without the system.
    float4x4 viewProj = frame.viewProj;
    if (pcDB.mainRenderView < MAX_RENDER_VIEWS)
    {
        viewProj = pcDB.renderViews.Get().views[pcDB.mainRenderView].m_viewProjectionMatrix;
    }
    //float4 viewPos = mul(frame_res.view, world);
    float4 clip = mul(viewProj, world);
    OUT.pos = clip;

    // Vertex color (RGBA8): plain albedo tint in legacy materials, or
    // per-layer blend weights (normalized in the fragment) when the
    // material carries layerMaterials (see default_lit.frag.hlsl).
    OUT.color = IN.color;
    OUT.uv = IN.uv;
    // Chart unpack for direct draws that pushed a chart tail (editor
    // Baked/Uv1 views); zeroed scratch (hasXforms == 0) selects the raw
    // chart-uv passthrough — direct draws are never lightmapped (lmChunk
    // none). Gated per-vertex on chartId inside GfxLightmapUv
    // (uncharted = 0).
    OUT.lightmapUv = (pcDB.hasXforms == 0u)
        ? IN.chartUv
        : GfxLightmapUv(IN.chartUv, IN.chartId, pcDB.chartOffset, pcDB.lmXforms);
    OUT.lmChunk = 0xFFFFFFFFu;

    // Dither-on-closeup per-draw flag (byte 240 of this push — see the
    // struct tail above; the direct legs push it per draw).
    OUT.ditherCloseup = pcDB.ditherCloseup;

    // Properly transform normal/tangent to world space.
    // - Normal uses inverse-transpose of upper 3x3 to survive non-uniform scale.
    // - Tangent xyz uses (float3x3)model, handedness w is flipped if model determinant <0 (mirrored).
    // - Main view uses a Y-flipped proj (proj[1][1]*=-1); front-face winding is
    //   defined by the pipeline RasterSettings (FrontFace::CounterClockwise).
    // Packed shading stream: normal/tangent ride as R32_UInt oct words.
    float3 normal = OctDecodeNormal(IN.normalPacked);
    float4 tangent = OctDecodeTangent(IN.tangentPacked);

    float3x3 M3 = (float3x3) pcDB.model;

    // determinant of M3
    float det = M3[0][0] * (M3[1][1] * M3[2][2] - M3[1][2] * M3[2][1])
              - M3[0][1] * (M3[1][0] * M3[2][2] - M3[1][2] * M3[2][0])
              + M3[0][2] * (M3[1][0] * M3[2][1] - M3[1][1] * M3[2][0]);

    // inverse-transpose for normal
    float3 worldNormal;
    if (abs(det) > 1e-12f)
    {
        // cofactor matrix, transposed = adjugate
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

        // adj = cofactor matrix C (C[i][j]=(-1)^{i+j} minor).
        // inverse = C^T / det, inverse-transpose = C / det.
        // So for normal transform we need C / det (not transpose).
        float3x3 invTrans = adj / det;
        // Equivalent to transpose(inverse(M3)). Use mul(invTrans, normal) for column vector.
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
