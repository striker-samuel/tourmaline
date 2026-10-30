// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Filtered-path motion-vector vertex shader: draws the per-view compacted
// index list produced by gfxfilter.cs.hlsl via DrawIndexedIndirect, so the
// motion pass reuses the GPU HZB cull (occluded clusters were zeroed to
// indexCount 0 by the filter CS and are skipped by the indirect draw).
//
// Static geometry: prevWorld == world (node transforms do not move; camera
// motion falls out of prevViewProj - see the per-draw SKINNED swap below
// for the one exception). Nodes whose model matrix moved since last frame
// are drawn by the direct fallback (correct prevModel push) and skipped
// here by the caller - dynamic transforms never rely on this shader.
//
// SKINNED draws (the Northlight compute path; misc.y bit 2 =
// GFX_DRAW_SKINNED): cur + PREV positions from the parity DOUBLE-buffered
// deformed heap - the true per-vertex mesh motion the temporal consumers
// (SMAA T2x resolve history blending, DLSS, motion blur) need. The old
// static-heap contract (prevPos == pos for everything) ghosted every
// animated mesh through those consumers: the T2x resolve reprojected the
// previous pose onto the current one at full history weight (a dimmed
// ghost copy next to the sharp one). vsPrevValid gates the prev half (the
// first animated frame / a heap rebuild / a skinning gap falls back to
// zero mesh motion - prevPos == curPos, the old behavior for one frame).
//
// Push layout: bytes 0..192 are the scene push (preserved as pads, intact
// for the motion fragment shader which reads materialId/frameUniforms from
// offset 0 in its own module); bytes 192..240 are the FilteredVsPush tail
// (PushViewConstants - the SAME tail every filtered VS reads: this VS
// re-declares the fields it consumes instead of padding over them,
// including paramsValid at 212 and the VS-skin cur pair at 224..236);
// bytes 216..224 carry the FrameUniforms BDA (overwriting the FilteredVsPush
// lmXforms slot the motion VS never reads); bytes 240..256 are the
// motion-only PREV tail (FilteredMotionPrevTail, pushed by
// PushMotionViewConstants; nothing else claims 240..256 in the motion
// pass). viewProj resolves through the render-view slab (same as
// gfxfiltered.vert.hlsl); prevViewProj comes from FrameUniforms (the jitter
// rides both sides - the resolve's exact reprojection).
#define GFXFILTERED_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"
#include "gfxmesh.hlsli"
#include "gpu_platform.hlsli"

struct FilteredMotionPush
{
    // Preserve the scene push bytes this VS does not use (model + frame
    // addresses, 192 bytes); the tail below is what the app pushes at
    // offset 192 (see smvk::gfx::kFilteredVsPushOffset).
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    // --- FilteredVsPush prefix (192..216, PushViewConstants) ---
    GPU_PTR_READ(GfxDraws) draws;
    GPU_PTR_READ(RenderViewBuffer) renderViews;
    uint renderViewIndex;
    // paramsValid (FilteredVsPush byte 212): 1 = the vsDeformed BDA below
    // is live (the filler sets it with the BDA; the skinned fetch gates on
    // it - a plain pointer compare is illegal on vk::BufferPointer).
    uint paramsValid;
    // Extra: FrameUniforms BDA pushed at byte offset 216 (the FilteredVsPush
    // lmXforms slot - the motion FS never reads prevModel, overwriting it
    // is safe).
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    // --- FilteredVsPush VS-skin tail (224..240, PushViewConstants) ---
    // The frame's cur deformed half (5 words per vertex at
    // (vertexID - vsDeformedBase) * 5).
    GPU_PTR_READ(GfxSkinDeformedHeap) vsDeformed;
    uint vsDeformedBase;
    uint vsPad;
    // --- Motion-only PREV tail (240..256, FilteredMotionPrevTail -
    // PushMotionViewConstants; C++ mirror gfx_filter.h) ---
    GPU_PTR_READ(GfxSkinDeformedHeap) vsPrevDeformed;
    uint vsPrevValid; // 1 = the prev half holds last frame's pose
    uint vsPad2;
};

GPU_PUSH(FilteredMotionPush, pcFM);

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    uint instanceID : SV_InstanceID;
    // Global heap vertex id (the index buffer carries GLOBAL ids - the
    // skinned fetch's deformed-heap index; see gfxfiltered.vert.hlsl).
    uint vertexID : SV_VertexID;
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
    GfxDraw draw = pcFM.draws.Get().draws[IN.instanceID];
    float4x4 model = draw.model;

    // Static default: prev == curr (camera-only motion). SKINNED draws
    // swap BOTH positions to the deformed halves (one storage fetch pair,
    // indexed IN PLACE - never bind the whole 256MB heap struct to a
    // local; see gfxfiltered.vert.hlsl's wedge note).
    float3 position = IN.position;
    float3 prevPosition = IN.position;
    const bool skinned = (draw.misc.y & GFX_DRAW_SKINNED) != 0;
    if (skinned && pcFM.paramsValid != 0u) {
        const uint d = (IN.vertexID - pcFM.vsDeformedBase) * 5;
        position = float3(asfloat(pcFM.vsDeformed.Get().word[d + 0]),
            asfloat(pcFM.vsDeformed.Get().word[d + 1]),
            asfloat(pcFM.vsDeformed.Get().word[d + 2]));
        if (pcFM.vsPrevValid != 0u) {
            prevPosition = float3(asfloat(pcFM.vsPrevDeformed.Get().word[d + 0]),
                asfloat(pcFM.vsPrevDeformed.Get().word[d + 1]),
                asfloat(pcFM.vsPrevDeformed.Get().word[d + 2]));
        } else {
            prevPosition = position; // no prev pose yet: zero mesh motion
        }
    }

    float4 world = mul(model, float4(position, 1.0));
    float4x4 viewProj =
        pcFM.renderViews.Get().views[pcFM.renderViewIndex].m_viewProjectionMatrix;
    OUT.pos = mul(viewProj, world);

    // The same (node-static) model on both sides - moved nodes draw
    // direct. Camera motion (incl. the previous frame's jitter) falls out
    // of prevViewProj; skinned mesh motion out of the prev half above.
    FrameUniforms frame = pcFM.frameUniforms.Get();
    float4 prevWorld = mul(model, float4(prevPosition, 1.0));
    OUT.prevClip = mul(frame.prevViewProj, prevWorld);

    OUT.uv = IN.uv;
    return OUT;
}
