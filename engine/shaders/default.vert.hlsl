#include "common.hlsli"
#include "vertex_pack.hlsli"

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    // Location 5 = StaticVertexFormat's TexCoord1 (lightmap UV; appended
    // last so 0..4 stay stable). Shaders that don't declare it just
    // ignore the attribute.
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD1;
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
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pc.frameUniforms.Get();

    // World position
    float4 world = mul(pc.model, float4(IN.position, 1.0));
    OUT.worldPos = world.xyz;

    // View / projection: the main camera's RenderView is the authoritative
    // VP for scene passes (allocated per frame, see renderview.hlsli).
    // FrameUniforms stays as the fallback for pipelines without the system.
    float4x4 viewProj = frame.viewProj;
    if (pc.mainRenderView < MAX_RENDER_VIEWS)
    {
        viewProj = pc.renderViews.Get().views[pc.mainRenderView].m_viewProjectionMatrix;
    }
    //float4 viewPos = mul(frame_res.view, world);
    float4 clip = mul(viewProj, world);
    OUT.pos = clip;

    // Vertex color (RGBA8): plain albedo tint in legacy materials, or
    // per-layer blend weights (normalized in the fragment) when the
    // material carries layerMaterials (see default_lit.frag.hlsl).
    OUT.color = IN.color;
    OUT.uv = IN.uv;
    OUT.lightmapUv = IN.lightmapUv;
    OUT.lmChunk = 0xFFFFFFFFu;

    // Properly transform normal/tangent to world space.
    // - Normal uses inverse-transpose of upper 3x3 to survive non-uniform scale.
    // - Tangent xyz uses (float3x3)model, handedness w is flipped if model determinant <0 (mirrored).
    // - Main view uses a Y-flipped proj (proj[1][1]*=-1); front-face winding is
    //   defined by the pipeline RasterSettings (FrontFace::CounterClockwise).
    // Packed shading stream: normal/tangent ride as R32_UInt oct words.
    float3 normal = OctDecodeNormal(IN.normalPacked);
    float4 tangent = OctDecodeTangent(IN.tangentPacked);

    float3x3 M3 = (float3x3) pc.model;

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
