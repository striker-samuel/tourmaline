#include "common.hlsli"

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

    OUT.color = float3(1.0f, 1.0f, 1.0f);
    OUT.uv = IN.uv;

    // Properly transform normal/tangent to world space.
    // - Normal uses inverse-transpose of upper 3x3 to survive non-uniform scale.
    // - Tangent xyz uses (float3x3)model, handedness w is flipped if model determinant <0 (mirrored).
    // - Main view uses a Y-flipped proj (proj[1][1]*=-1); front-face winding is
    //   defined by the pipeline RasterSettings (FrontFace::CounterClockwise).
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
        worldNormal = mul(invTrans, IN.normal);
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
