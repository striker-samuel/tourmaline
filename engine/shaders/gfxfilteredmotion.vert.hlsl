// Filtered-path motion-vector vertex shader: draws the per-view compacted
// index list produced by gfxfilter.cs.hlsl via DrawIndexedIndirect, so the
// motion pass reuses the GPU HZB cull (occluded clusters were zeroed to
// indexCount 0 by the filter CS and are skipped by the indirect draw).
//
// Coarse whole-mesh strategy (option b): static geometry only. prevWorld is
// assumed to equal the current model matrix, which is exact for static nodes
// (the dominant case: camera moves, geometry doesn't) and matches the
// direct path's prev==curr fallback on the first frame / scene rebuild.
// Nodes whose model matrix moved since last frame are drawn by the direct
// fallback in main.cpp (correct prevModel push) and skipped here by the
// caller — dynamic objects never rely on this shader.
//
// Push layout: bytes 0..192 are the scene push (preserved as pads, intact
// for the motion fragment shader which reads materialId/frameUniforms from
// offset 0 in its own module); bytes 192..216 are the 24B filtered VS tail
// (draws + renderViews + renderViewIndex, see
// smvk::gfx::kFilteredVsPushOffset, pushed by PushViewConstants); bytes
// 216..224 carry the FrameUniforms BDA (pushed by PushMotionViewConstants
// into the free prevModel scratch area). The FS never reads prevModel, so
// overwriting it is safe. viewProj resolves through the render-view slab
// (same as gfxfiltered.vert.hlsl); prevViewProj comes from FrameUniforms.
#define GFXFILTERED_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"

struct FilteredMotionPush
{
    // Preserve the scene push bytes this VS does not use (model + frame
    // addresses, 192 bytes); the tail below is what the app pushes at
    // offset 192 (see smvk::gfx::kFilteredVsPushOffset).
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    vk::BufferPointer<GfxDraws> draws;
    vk::BufferPointer<RenderViewBuffer> renderViews;
    uint renderViewIndex;
    uint pad;
    // Extra: FrameUniforms BDA pushed at byte offset 216 (free prevModel
    // scratch; the motion FS never reads prevModel).
    vk::BufferPointer<FrameUniforms> frameUniforms;
    // Remaining 224..256 unused.
    float4 _pad3;
    float4 _pad4;
};

[[vk::push_constant]]
ConstantBuffer<FilteredMotionPush> pcFM;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    uint instanceID : SV_InstanceID;
};
struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 prevClip : PREVCLIP;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    // firstInstance == drawIndex (filter writes it per indirect command),
    // so instanceID indexes the draws ring directly.
    float4x4 model = pcFM.draws.Get().draws[IN.instanceID].model;

    float4 world = mul(model, float4(IN.position, 1.0));
    float4x4 viewProj =
        pcFM.renderViews.Get().views[pcFM.renderViewIndex].m_viewProjectionMatrix;
    OUT.pos = mul(viewProj, world);

    // Static assumption: prevWorld == world. Camera-only motion falls out
    // of prevViewProj; moved nodes are drawn direct by the caller.
    FrameUniforms frame = pcFM.frameUniforms.Get();
    OUT.prevClip = mul(frame.prevViewProj, world);

    OUT.uv = IN.uv;
    return OUT;
}
