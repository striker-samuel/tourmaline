#include "light.hlsli"
#include "common.hlsli"
#include "vertex_pack.hlsli"

// Vertex-lit shader-LOD program (see World::UseVertexLitShaderAt): the
// whole lighting response is computed PER VERTEX and interpolated
// (Gouraud shading); the paired fragment shader is a fog/compositing
// pass-through. Selected for the FARTHEST level of a cooked LOD chain
// by the Main pass when the material programs it (see ShaderLodPolicy)
// — the level keeps the coarsest geometry sibling and only the shading
// program downgrades.
//
// Deliberately cheap (that is the point of a shader LOD): no shadow-map
// sampling, no forward+ point/spot lights, no GI/IBL, no specular, no
// texture fetches. Albedo is the material diffuse factor * vertex-color
// tint (legacy) or the per-layer blend of the diffuse factors (layered
// materials, same weight convention as default_unlit/default_lit).
//
// Radiance convention matches default_lit so the HDR + tonemap chain is
// unchanged: direct = albedo/PI * NdotL * sunE (lambert, unshadowed),
// ambient = albedo * mat.ambient (legacy constant sky-ambient factor).
// COLOR0.a carries the blended alpha for the FS cutout/dither tests.

static const float PI = 3.14159265359f;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    uint instanceID : SV_InstanceID;
};
struct VSOut
{
    float4 pos : SV_Position;

    // rgb = per-vertex lit HDR radiance, a = blended alpha.
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float3 worldPos : TEXCOORD0;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pc.frameUniforms.Get();

    // World position
    float4 world = mul(pc.model, float4(IN.position, 1.0));
    OUT.worldPos = world.xyz;

    // View / projection: the main camera's RenderView is the authoritative
    // VP for scene passes (see default.vert.hlsl).
    float4x4 viewProj = frame.viewProj;
    if (pc.mainRenderView < MAX_RENDER_VIEWS)
    {
        viewProj = pc.renderViews.Get().views[pc.mainRenderView].m_viewProjectionMatrix;
    }
    OUT.pos = mul(viewProj, world);

    // World normal via inverse-transpose (same path as default.vert).
    float3 normal = OctDecodeNormal(IN.normalPacked);

    float3x3 M3 = (float3x3) pc.model;

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

        // inverse-transpose = cofactor / det (see default.vert.hlsl).
        float3x3 invTrans = adj / det;
        worldNormal = mul(invTrans, normal);
    }
    else
    {
        worldNormal = mul(M3, normal);
    }
    float3 N = normalize(worldNormal);

    // Albedo + alpha: material diffuse factor * vertex tint (legacy), or
    // the normalized vertex-color weights blending up to four layer
    // diffuse factors (layered — same convention as default_unlit).
    MaterialValues mat = pc.bMats.Get().mats[pc.materialId];
    int layerMode = mat.layerMaterials.w;
    const bool layered = (layerMode > 1);
    float4 layerW = layered
        ? IN.color / max(IN.color.x + IN.color.y + IN.color.z + IN.color.w, 1e-6f)
        : float4(1.0f, 0.0f, 0.0f, 0.0f);
    MaterialValues ml0 = mat;
    MaterialValues ml1 = mat;
    MaterialValues ml2 = mat;
    MaterialValues ml3 = mat;
    if (layered)
    {
        int id1 = mat.layerMaterials.x;
        int id2 = mat.layerMaterials.y;
        int id3 = mat.layerMaterials.z;
        if (layerMode > 1 && id1 >= 0 && id1 < MAX_MATERIALS)
            ml1 = pc.bMats.Get().mats[id1];
        if (layerMode > 2 && id2 >= 0 && id2 < MAX_MATERIALS)
            ml2 = pc.bMats.Get().mats[id2];
        if (layerMode > 3 && id3 >= 0 && id3 < MAX_MATERIALS)
            ml3 = pc.bMats.Get().mats[id3];
    }
    float3 albedo;
    float alpha;
    if (layered)
    {
        albedo = ml0.diffuse.rgb * layerW.x + ml1.diffuse.rgb * layerW.y
               + ml2.diffuse.rgb * layerW.z + ml3.diffuse.rgb * layerW.w;
        alpha = ml0.diffuse.a * layerW.x + ml1.diffuse.a * layerW.y
              + ml2.diffuse.a * layerW.z + ml3.diffuse.a * layerW.w;
    }
    else
    {
        albedo = mat.diffuse.rgb * IN.color.rgb;
        alpha = mat.diffuse.a;
    }

    // Per-vertex lighting: directional sun (lights[0], unshadowed) +
    // constant sky ambient. Matches the default_lit direct/ambient terms
    // (lambert diffuse, mat.ambient albedo multiplier).
    Light light = pc.bLights.Get().lights[0];
    float3 L = normalize(-light.direction);
    float NdotL = saturate(dot(N, L));
    float3 sunE = light.color.rgb * light.intensity;
    float3 direct = albedo / PI * NdotL * sunE;
    float3 ambient = albedo * mat.ambient.rgb;

    OUT.color = float4(direct + ambient, alpha);
    return OUT;
}
