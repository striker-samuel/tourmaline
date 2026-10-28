// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Placement instanced-path vertex shader: draws GPU-resident placement
// instances via DrawIndexedIndirect, one command per (tile, prototype)
// bin — the PR03 GPU-visible chain (cull -> prefix -> scatter ->
// drawbuild writes the visible-index list + the commands' dynamic
// halves; the CPU only coarse-culls whole tiles and records the
// indirect draws).
//
//   - INSTANCE FETCH: visible[SV_InstanceID] -> the SOURCE slot ->
//     the 40-byte instance. SV_InstanceID includes the indirect
//     command's firstInstance under SPIR-V, so bin p's draw walks
//     exactly its own visible range [offsets[p], offsets[p]+counts[p])
//     — no degenerate-collapse vertex waste, no per-instance VS test
//     on the hot path.
//   - model matrix is composed per instance: T(position) * RotY(yaw) *
//     S(uniform scale). Scale is always uniform (enforced at
//     generation), so normals rotate without the adjugate. Mirroring
//     never occurs (det = sc^3 > 0), so tangent handedness passes
//     through.
//   - cullFlags & PLACEMENT_CULL_DEBUG_RETEST: the LEGACY conservative
//     frustum re-test (the pre-chain degenerate collapse) kept as a
//     DEBUG compare switch while the chain is validated — the guard
//     factor form, never for production (the production bound is the
//     per-proto cooked sphere in the cull kernel).
//   - placement draws are dynamic: lmChunk none, lightmapUv zero.
//
// Push: 32B tail at byte offset 192 (the main pass' unused prevModel
// scratch — same overlay precedent as FilteredVsPush; different passes,
// no conflict). The 256B scene push stays intact for the fragment
// shader, so all existing fragment shaders work unchanged.
#define PLACEMENT_INSTANCED_PASS
#include "common.hlsli"
#include "vertex_pack.hlsli"
#include "placement_common.hlsli"
#include "placement_cull.hlsli"

// VS debug re-test bit (PlacementVsPush.cullFlags): re-run the legacy
// guard-based frustum test and collapse failures (compare switch only).
#define PLACEMENT_CULL_DEBUG_RETEST 8

struct PlacementVsMainPush
{
    // Preserve the scene push bytes this VS does not use (model + frame
    // addresses, 192 bytes); the tail below is what the app pushes at
    // offset 192 (see kPlacementVsPushOffset).
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    vk::BufferPointer<PlacementVisibleBuffer> visible;     // 192
    vk::BufferPointer<PlacementInstanceBuffer> instances;   // 200
    vk::BufferPointer<RenderViewBuffer> renderViews;       // 208
    uint renderViewIndex;  // 216
    uint cullFlags;        // 220 (debug re-test switch; 0 = trust the chain)
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
    // includes the indirect command's firstInstance (= the proto bin's
    // offset in the visible list). idx below IS the global visible-list
    // position — the chain's per-bin ranges make each draw walk exactly
    // its own instances (the nonzero-firstInstance contract, pinned by
    // PlacementGpuTests' bin-range identity checks).
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
    // Indirect firstInstance is pre-added to SV_InstanceID by hardware:
    // idx is the position inside the GLOBAL visible list — bin p's draw
    // walks exactly [offsets[p], offsets[p] + counts[p]).
    uint idx = IN.instanceID;
    // THE CHAIN FETCH: visible slot -> source instance. No per-instance
    // test on the hot path — the cull kernel already decided (the
    // degenerate-collapse waste of the pre-chain VS is gone).
    uint source = pcPV.visible.Get().v[idx];
    PlacementRenderInstance inst = pcPV.instances.Get().items[source];

    // DEBUG compare switch (PLACEMENT_CULL_DEBUG_RETEST): the legacy
    // guard-based conservative frustum re-test, kept while the chain is
    // validated. Production leaves this OFF (the production bound is
    // the per-proto cooked sphere in the cull kernel).
    if ((pcPV.cullFlags & PLACEMENT_CULL_DEBUG_RETEST) != 0)
    {
        bool culled = false;
        float boundR = 0.8660254f * max(inst.scale.x, max(inst.scale.y, inst.scale.z))
            * 4.0f; // the old guard factor — debug-only
        for (uint p = 0; p < 6; ++p)
        {
            float4 plane = pcPV.renderViews.Get().views[pcPV.renderViewIndex].m_viewPlanes[p];
            if (dot(plane.xyz, inst.position) + plane.w < -boundR)
            {
                culled = true;
                break;
            }
        }
        if (culled)
        {
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
