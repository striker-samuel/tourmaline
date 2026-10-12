// Placement instanced-path vertex shader: draws GPU-resident placement
// instances via DrawIndexedIndirect, one command per (tile, prototype) run.
//
// Differences vs gfxfiltered.vert.hlsl:
//   - model matrix is composed per instance from the placement instance
//     buffer: T(position) * RotY(yaw) * S(uniform scale). Scale is always
//     uniform (enforced at generation), so normals rotate without the
//     adjugate: worldNormal = RotY * normal. Mirroring never occurs
//     (det = sc^3 > 0), so tangent handedness passes through.
//   - instance fetch index = SV_InstanceID, which under SPIR-V already
//     includes the indirect command's firstInstance (runs are sorted by
//     prototype at generation; see PlacementTile).
//   - per-instance culling in the VS (frustum sphere + alive flag), with
//     failures collapsed to out-of-clip degenerates — the particles_draw
//     precedent. Clip does the exact cull; the VS test only saves work.
//     Frustum radius is conservative (guard factor below); it may keep,
//     never wrongly culls.
//   - placement draws are dynamic: lmChunk none, lightmapUv zero.
//
// Push: 32B PlacementVsPush tail at byte offset 192 (the main pass'
// unused prevModel scratch — same overlay precedent as FilteredVsPush;
// different passes, no conflict). The 256B scene push stays intact for
// the fragment shader, so all existing fragment shaders work unchanged.
#define PLACEMENT_INSTANCED_PASS
#include "common.hlsli"
#include "vertex_pack.hlsli"
#include "placement_common.hlsli"
#include "placement_constraint.hlsli"

// Conservative bound: unit-cube half-diagonal (0.866) * max scale, times
// a guard for prototype extents beyond the unit cube. May keep culled
// instances (extra VS cost), never culls visible ones.
#define PLACEMENT_VS_BOUND_GUARD 4.0f

// Cull flag bits (PlacementVsPush.cullFlags).
#define PLACEMENT_CULL_FRUSTUM 1
#define PLACEMENT_CULL_ALIVE   2

struct PlacementVsMainPush
{
    // Preserve the scene push bytes this VS does not use (model + frame
    // addresses, 192 bytes); the tail below is what the app pushes at
    // offset 192 (see kPlacementVsPushOffset).
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    vk::BufferPointer<PlacementInstanceBuffer> instances; // 192
    vk::BufferPointer<PlacementFlag2Buffer> alive;        // 200 (0 = all alive)
    vk::BufferPointer<RenderViewBuffer> renderViews;      // 208
    uint renderViewIndex;  // 216
    uint cullFlags;        // 220
};

[[vk::push_constant]]
ConstantBuffer<PlacementVsMainPush> pcPV;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    // No TexCoord1/ChartId (locations 5/6): placement draws are dynamic and
    // never lightmapped (the outputs below carry the unlightmapped
    // sentinel), and StaticVertexFormat no longer binds the second UV
    // channel anyway (lightmaps deprecated — see mesh.h).
    // NOTE: via SPIR-V, SV_InstanceID maps to InstanceIndex, which already
    // includes the indirect command's firstInstance. No BaseInstance fetch
    // (and no shaderDrawParameters requirement): idx below IS the tile
    // buffer index for per-prototype runs.
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
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    // Indirect firstInstance is pre-added to SV_InstanceID by hardware.
    uint idx = IN.instanceID;
    PlacementRenderInstance inst = pcPV.instances.Get().items[idx];

    // ---- Per-instance cull (degenerate collapse on failure). ----
    bool culled = false;
    if ((pcPV.cullFlags & PLACEMENT_CULL_ALIVE) != 0 && pcPV.alive.Get().v[idx].y == 0)
        culled = true;
    if (!culled && (pcPV.cullFlags & PLACEMENT_CULL_FRUSTUM) != 0)
    {
        float boundR = 0.8660254f * max(inst.scale.x, max(inst.scale.y, inst.scale.z))
            * PLACEMENT_VS_BOUND_GUARD;
        for (uint p = 0; p < 6; ++p)
        {
            float4 plane = pcPV.renderViews.Get().views[pcPV.renderViewIndex].m_viewPlanes[p];
            if (dot(plane.xyz, inst.position) + plane.w < -boundR)
            {
                culled = true;
                break;
            }
        }
    }
    if (culled)
    {
        // Outside a common clip plane: the whole triangle clips away.
        OUT.pos = float4(2.0f, 2.0f, 2.0f, 1.0f);
        OUT.color = float3(0.0f, 0.0f, 0.0f);
        OUT.uv = float2(0.0f, 0.0f);
        OUT.worldPos = float3(0.0f, 0.0f, 0.0f);
        OUT.normal = float3(0.0f, 1.0f, 0.0f);
        OUT.tangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
        OUT.lightmapUv = float2(0.0f, 0.0f);
        OUT.lmChunk = 0xFFFFFFFFu;
        return OUT;
    }

    // ---- Compose T * RotY * S (column-major). ----
    float c, s;
    sincos(inst.yaw, s, c);
    float sx = inst.scale.x; // uniform by construction; x is representative
    float4x4 model;
    model[0] = float4(c * sx, 0.0f, -s * sx, 0.0f);
    model[1] = float4(0.0f, sx, 0.0f, 0.0f);
    model[2] = float4(s * sx, 0.0f, c * sx, 0.0f);
    model[3] = float4(inst.position, 1.0f);

    float4 world = mul(model, float4(IN.position, 1.0f));
    OUT.worldPos = world.xyz;
    float4x4 viewProj =
        pcPV.renderViews.Get().views[pcPV.renderViewIndex].m_viewProjectionMatrix;
    OUT.pos = mul(viewProj, world);

    OUT.color = float3(1.0f, 1.0f, 1.0f);
    OUT.uv = IN.uv;
    OUT.lightmapUv = float2(0.0f, 0.0f);
    OUT.lmChunk = 0xFFFFFFFFu;

    float3 normal = OctDecodeNormal(IN.normalPacked);
    float4 tangent = OctDecodeTangent(IN.tangentPacked);

    // Uniform positive scale: rotation only, no adjugate, no mirror fixup.
    float3x3 R;
    R[0] = float3(c, 0.0f, -s);
    R[1] = float3(0.0f, 1.0f, 0.0f);
    R[2] = float3(s, 0.0f, c);
    OUT.normal = normalize(mul(R, normal));
    OUT.tangent = float4(normalize(mul(R, tangent.xyz)), tangent.w);

    return OUT;
}
