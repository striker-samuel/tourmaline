// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Seed-depth vertex shader shared code (Alan Wake 2 style temporal, T3).
// Included by gfxseed.vert.hlsl (opaque, position-only) and
// gfxseedmasked.vert.hlsl (+ color/uv for the shadow.frag alpha test).
//
// Draws the previous frame's surviving (cluster, draw) list straight from
// the cluster-ordered index heap to seed current-frame depth. One indirect
// command per visible cluster (built CPU-side by BuildSeedCommands):
//   indexCount    = triCount * 3
//   firstIndex    = cluster.indexOffset into GfxIndices (global vertex ids)
//   firstInstance = draw index into the visDraws snapshot (model matrices)
//
// Push: the 24B tail at byte offset 192 (same layout as the main filtered
// path: draws + renderViews + renderViewIndex), leaving the 256B scene
// push intact for the masked fragment shader (materialId).

#pragma once

#include "gfxfilter.hlsli"
#include "gfxmesh.hlsli"
#include "gpu_platform.hlsli"

struct SeedVsMainPush
{
    // Preserve the scene push bytes this VS does not use (model + frame
    // addresses, 192 bytes); the tail below is what the app pushes at
    // offset 192 (see smvk::gfx::kFilteredVsPushOffset). The layout mirrors
    // C++ smvk::gfx::FilteredVsPush byte-for-byte (PushSeedConstants pushes
    // that same 48B struct): draws/renderViews/renderViewIndex/paramsValid/
    // lmXforms/vsDeformed/vsDeformedBase/vsPad. Only draws/renderViews/
    // renderViewIndex + the skin pair are read here.
    float4x4 _pad0;
    float4x4 _pad1;
    float4x4 _pad2;
    GPU_PTR_READ(GfxDraws) draws;
    GPU_PTR_READ(RenderViewBuffer) renderViews;
    uint renderViewIndex;
    // paramsValid (byte 212): 1 = vsDeformed is live (the same null-check
    // flag the filtered VS reads - the skinned fetch gates on it).
    uint paramsValid;
    GPU_PTR_READ(GfxChartXforms) lmXforms;
    // The frame's DEFORMED heap (the skinning pass output): skinned draws
    // fetch pos at (vertexID - vsDeformedBase) * 5 so the depth legs write
    // the ANIMATED pose (not the bind pose) - the depth/Main parity the
    // Northlight order requires.
    GPU_PTR_READ(GfxSkinDeformedHeap) vsDeformed;
    uint vsDeformedBase;
    uint vsPad;
};

struct SeedVSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    uint instanceID : SV_InstanceID;
    // Global heap vertex id (the seed index commands carry GLOBAL ids, so
    // the fixed-function fetch AND the deformed-heap storage fetch below
    // address the same vertex): the SKINNED swap's deformed index.
    uint vertexID : SV_VertexID;
};

struct SeedVSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    // Dither-on-closeup inputs (the MASKED seed/remainder twins pair with
    // shadow.frag's screen-door — see common.hlsli CloseupDitherKeep):
    // the per-draw flag from the Draw ring + the world position for the
    // per-pixel camera-distance factor. The OPAQUE seed twins are VS-only
    // pipelines with no FS: they leave these unwritten and no stage
    // declares them (locations a VS never writes are fine when no FS
    // input consumes them — the same rule as the color/uv pair above).
    [[vk::location(2)]] float3 worldPos : TEXCOORD2;
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
};

// SKINNED draw fetch swap (the Northlight compute path): a draw carrying
// GFX_DRAW_SKINNED (misc.y bit 2) takes its position from the DEFORMED
// heap (5 words/vertex at (vertexID - vsDeformedBase) * 5, the skinning
// pass's output) instead of the fixed-function bind-pose fetch, so the
// depth legs write the ANIMATED pose and match Main's skinned draws
// exactly. paramsValid is the CPU null-check (the flag only sets when the
// frame's deformed buffer exists - the MMU-fault rule). Index the BDA IN
// PLACE (never bind the 256MB heap to a local - the NV pipeline compiler
// wedges; see gfxfiltered.vert.hlsl).
float3 SeedSkinPos(float3 position, uint instanceID, uint vertexID,
    GPU_PTR(GfxDraws) draws,
    GPU_PTR(GfxSkinDeformedHeap) vsDeformed,
    uint vsDeformedBase, uint paramsValid)
{
    if ((draws.Get().draws[instanceID].misc.y & GFX_DRAW_SKINNED) != 0
        && paramsValid != 0u)
    {
        const uint d = (vertexID - vsDeformedBase) * 5;
        return float3(asfloat(vsDeformed.Get().word[d + 0]),
            asfloat(vsDeformed.Get().word[d + 1]),
            asfloat(vsDeformed.Get().word[d + 2]));
    }
    return position;
}

float4 SeedClipPos(float3 position, uint instanceID, uint vertexID,
    GPU_PTR(GfxDraws) draws,
    GPU_PTR(RenderViewBuffer) renderViews,
    uint renderViewIndex,
    GPU_PTR(GfxSkinDeformedHeap) vsDeformed,
    uint vsDeformedBase, uint paramsValid)
{
    float3 p = SeedSkinPos(position, instanceID, vertexID, draws,
        vsDeformed, vsDeformedBase, paramsValid);
    float4x4 model = draws.Get().draws[instanceID].model;
    float4 world = mul(model, float4(p, 1.0));
    float4x4 viewProj =
        renderViews.Get().views[renderViewIndex].m_viewProjectionMatrix;
    return mul(viewProj, world);
}

// World-space seed position (the masked twins' dither-closeup distance
// term — the same ring fetch SeedClipPos pays, L1-resident by the time
// the draw record is re-read). Skinned draws use the deformed pose too so
// the screen-door distance tracks the animated position.
float3 SeedWorldPos(float3 position, uint instanceID, uint vertexID,
    GPU_PTR(GfxDraws) draws,
    GPU_PTR(GfxSkinDeformedHeap) vsDeformed,
    uint vsDeformedBase, uint paramsValid)
{
    float3 p = SeedSkinPos(position, instanceID, vertexID, draws,
        vsDeformed, vsDeformedBase, paramsValid);
    return mul(draws.Get().draws[instanceID].model,
        float4(p, 1.0)).xyz;
}

// Per-draw dither-on-closeup flag from the Draw ring (misc.y bit 1 —
// GFX_DRAW_DITHER_CLOSEUP; set at PushDraw time together with the MASKED
// bucket routing that lands the draw in these alpha-tested twins).
uint SeedDitherCloseup(uint instanceID, GPU_PTR(GfxDraws) draws)
{
    return (draws.Get().draws[instanceID].misc.y
        & GFX_DRAW_DITHER_CLOSEUP) ? 1u : 0u;
}
