// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
#include "gfxmesh.hlsli"

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
    // paramsValid (byte 212, the old pad): 1 = the vsDeformed BDA below is
    // live (the filler sets it with the BDA; the skinned fetch gates on it
    // - a plain pointer compare is illegal on vk::BufferPointer; 0 = static
    // fetches, a frame with no skinned cluster). MUST sit here (byte 212),
    // BEFORE lmXforms - the C++ FilteredVsPush mirror orders
    // draws/renderViews/renderViewIndex/paramsValid/lmXforms/vsDeformed/...
    // Putting this word after lmXforms shifted the whole skinned tail by 8
    // and read vsDeformed as vsDeformedBase|vsPad = 0x00000000FFFFFFFF,
    // a wild BDA the skinned draw fetched from -> GPU fault.
    uint paramsValid;
    vk::BufferPointer<GfxChartXforms> lmXforms;
    // VS-skin input (bytes 224..240), carried DIRECTLY (one modest
    // pointer - see the C++ FilteredVsPush comment for the wedge
    // history): the frame's deformed heap at (vertexID - vsDeformedBase)
    // * 5.
    vk::BufferPointer<GfxSkinDeformedHeap> vsDeformed;
    uint vsDeformedBase;
    uint vsPad;
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
    // Global heap vertex id (the index buffer carries GLOBAL ids, so the
    // fixed-function vertex fetch AND the storage fetch below address the
    // same vertex): the SKINNED swap's deformed-heap index.
    uint vertexID : SV_VertexID;
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

    // SKINNED draws (the Northlight compute path; misc.y bit 2 =
    // GFX_DRAW_SKINNED): the DEFORMED heap holds this frame's animated
    // pos (the skinning pass wrote it; 5 words per vertex at
    // (vertexID - vsDeformedBase) * 5). ONE storage fetch swap - no
    // per-vertex skin math here, the same contract as the mesh-shader leg
    // (gfxfilter.mesh.hlsl). Static draws keep the fixed-function inputs
    // (the StaticVertexFormat fetch of the bind-pose heaps); the stale
    // IN.position/normalPacked/tangentPacked of a skinned draw are
    // simply ignored (the fixed-function fetch still happens - the
    // bandwidth waste is the documented slice-2 trade; emptying the
    // vertex format per-draw is impossible with one indirect range).
    // paramsValid is the CPU-side null check (the flag itself only sets
    // when the frame's deformed buffer exists - the MMU-fault rule).
    const bool skinned = (draw.misc.y & GFX_DRAW_SKINNED) != 0;
    float3 position = IN.position;
    uint normalPacked = IN.normalPacked;
    uint tangentPacked = IN.tangentPacked;
    if (skinned && pcFV.paramsValid != 0u) {
        // Index the BDA IN PLACE - never bind the whole GfxSkinDeformedHeap
        // to a local: that struct is a 256MB uint array, so a by-value
        // `const GfxSkinDeformedHeap def = ...Get();` forces the compiler to
        // materialize a 256MB local and the NVIDIA pipeline compiler wedges
        // (never returns) creating the vertex stage. The mesh leg already
        // indexes .Get().word[] directly; mirror it.
        const uint d = (IN.vertexID - pcFV.vsDeformedBase) * 5;
        position = float3(asfloat(pcFV.vsDeformed.Get().word[d + 0]),
            asfloat(pcFV.vsDeformed.Get().word[d + 1]),
            asfloat(pcFV.vsDeformed.Get().word[d + 2]));
        normalPacked = pcFV.vsDeformed.Get().word[d + 3];
        tangentPacked = pcFV.vsDeformed.Get().word[d + 4];
    }

    float4 world = mul(model, float4(position, 1.0));
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

    // Packed shading stream: normal/tangent ride as R32_UInt oct words
    // (the skinned swap above replaces the packed words with the deformed
    // heap's - the SAME encoding, so one decode path serves both).
    float3 normal = OctDecodeNormal(normalPacked);
    float4 tangent = OctDecodeTangent(tangentPacked);

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
