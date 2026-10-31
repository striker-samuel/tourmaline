// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
#include "gpu_platform.hlsli"

struct DefaultVsPush
{
    float4x4 model;
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    GPU_PTR_READ(LightBuffer) bLights;
    GPU_PTR_READ(MaterialsBuffer) bMats;
    GPU_PTR_READ(DirectionalCascadeBuffer) cascadeBuffer;
    uint materialId;
    uint comparisonSamplerId;
    uint numCascades;
    uint cascadeIndex;
    GPU_PTR_READ(ForwardPlusLightBuffer) fpLights;
    GPU_PTR_READ(ForwardPlusTileCountBuffer) fpTileCounts;
    GPU_PTR_READ(ForwardPlusTileIndexBuffer) fpTileIndices;
    uint fpTileCountX;
    uint fpLightCount;
    uint fpTileSize;
    uint fpMaxPerTile;
    GPU_PTR_READ(SpotShadowBuffer) spotShadowBuffer;
    uint spotShadowMapIndex;
    uint spotShadowCount;
    uint spotIndex;
    uint pad_pc;
    GPU_PTR_READ(RenderViewBuffer) renderViews;
    uint mainRenderView;
    uint renderViewIndex;
    // Per-mesh chart tail (pushed at byte 192 per mesh; hasXforms == 0,
    // the zeroed-scratch default, selects the passthrough below).
    uint hasXforms;
    uint chartOffset;
    GPU_PTR_READ(GfxChartXforms) lmXforms;
    // Byte 240 = PushConstants::padEnd_.x (the per-draw overlay scratch's
    // last word; the motion tail 192..240 stops short of it): the DIRECT
    // legs' per-draw dither-on-closeup flag (Scene::ditherCloseup — the
    // legs push it per draw, 0/1). Forwarded flat at location 8 so the FS
    // family reads ONE channel on both paths (the filtered legs deliver
    // the ring's GFX_DRAW_DITHER_CLOSEUP at the same location).
    // 208..240: the direct-path SKIN tail (zeroed = static fetch). The
    // prepass/occluder legs push it per skinned draw so this VS can fetch
    // the frame's deformed words instead of the bind-pose vertex inputs.
    // Mirrors C++ DepthSkinTail (32 bytes, byte-exact).
    uint skinned;      // 208: 1 = fetch from the deformed heap
    uint deformedBase; // 212: the skinned span's heap-id base
    GPU_PTR_READ(GfxSkinDeformedHeap) vsDeformed; // 216: the frame's deformed heap
    uint4 _padSkin;    // 224..240: layout filler
    uint ditherCloseup; // 240
    // Byte 244 = PushConstants::padEnd_.y: the DIRECT legs' per-draw
    // RESOLVED material userdata (docs/shader_library_design.md Phase A
    // — the World's material-instance override or base default, pushed
    // per lit draw; 0 = none). Forwarded flat at location 9 like the
    // dither flag above, so the FS family reads ONE channel on both
    // paths (the filtered legs deliver the Draw ring's misc3.x at the
    // same location).
    // Scratch-ownership note: the FILTERED motion pass overlays the
    // whole 240..256 word with its own FilteredMotionPrevTail — that is
    // a different pipeline whose pass pushes its tail before its draws,
    // so a stale direct-pass 244 can never leak into it; and only lit
    // FSs read location 9, while every lit direct draw pushes its own
    // value here (the padEnd_.x anti-leak contract, scoped to the
    // readers that exist).
    uint userData;     // 244
};

GPU_PUSH(DefaultVsPush, pcDB);

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint normalPacked : NORMAL;
    [[vk::location(4)]] uint tangentPacked : TANGENT;
    // Lightmaps are deprecated: StaticLayout no longer binds the
    // second UV channel (TexCoord1/ChartId would be locations 5/6), so
    // this VS declares neither (a bound-format/pipeline mismatch would
    // fail validation). The gated-off StaticLightmappedLayout
    // keeps the option; a future chart-unpack VS pairs with it. The
    // lightmapUv output below is a constant zero and lmChunk stays the
    // unlightmapped sentinel, so the FS lightmap paths gate off.
    uint instanceID : SV_InstanceID;
    // The global heap vertex id (the SKINNED swap's deformed-heap index).
    uint vertexID : SV_VertexID;
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
    // The resolved per-draw material userdata (flat; byte 244 — the
    // direct legs' per-lit-draw push): the unified second per-draw
    // channel, consumed by the lit FS family's TM_ModifySurface /
    // TM_ModifyLitColor hooks (the filtered legs deliver the Draw
    // ring's misc3.x at the same location). FSs that do not declare it
    // simply do not fetch it.
    [[vk::location(9)]] nointerpolation uint userData : TEXCOORD8;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    FrameUniforms frame = pcDB.frameUniforms.Get();

    // Optional skin fetch (direct-path prepass/occluder legs): a skinned
    // draw swaps the bind-pose vertex inputs for the frame's deformed words
    // (5 words/vertex at (vertexID - deformedBase) * 5). The tail is zeroed
    // on every non-skinned direct draw, so the static fetch stays the
    // default.
    float3 position = IN.position;
    uint normalPacked = IN.normalPacked;
    uint tangentPacked = IN.tangentPacked;
    if (pcDB.skinned != 0u)
    {
        const uint d = (IN.vertexID - pcDB.deformedBase) * 5;
        position = float3(asfloat(pcDB.vsDeformed.Get().word[d + 0]),
            asfloat(pcDB.vsDeformed.Get().word[d + 1]),
            asfloat(pcDB.vsDeformed.Get().word[d + 2]));
        normalPacked = pcDB.vsDeformed.Get().word[d + 3];
        tangentPacked = pcDB.vsDeformed.Get().word[d + 4];
    }

    // World position
    float4 world = mul(pcDB.model, float4(position, 1.0));
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
    // Lightmaps deprecated: no chart UV rides this format (see VSIn) and
    // direct draws are never lightmapped — the constant sentinel pair
    // (uv 0, chunk none) keeps the FS interpolant contract alive for the
    // gated-off StaticLightmappedLayout option. The chart tail
    // words in the push (hasXforms/chartOffset/lmXforms at 192..208) stay
    // declared for layout identity with the C++ mirror; nothing reads
    // them here.
    OUT.lightmapUv = float2(0.0f, 0.0f);
    OUT.lmChunk = 0xFFFFFFFFu;

    // Dither-on-closeup per-draw flag (byte 240 of this push — see the
    // struct tail above; the direct legs push it per draw).
    OUT.ditherCloseup = pcDB.ditherCloseup;

    // The resolved material userdata (byte 244 — the direct legs push it
    // per lit draw; the resolve is the World's material runtime state).
    OUT.userData = pcDB.userData;

    // Properly transform normal/tangent to world space.
    // - Normal uses inverse-transpose of upper 3x3 to survive non-uniform scale.
    // - Tangent xyz uses (float3x3)model, handedness w is flipped if model determinant <0 (mirrored).
    // - Main view uses a Y-flipped proj (proj[1][1]*=-1); front-face winding is
    //   defined by the pipeline RasterSettings (FrontFace::CounterClockwise).
    // Packed shading stream: normal/tangent ride as R32_UInt oct words.
    float3 normal = OctDecodeNormal(normalPacked);
    float4 tangent = OctDecodeTangent(tangentPacked);

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
