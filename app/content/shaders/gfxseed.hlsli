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

struct SeedVsMainPush
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
};

struct SeedVSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    uint instanceID : SV_InstanceID;
};

struct SeedVSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

float4 SeedClipPos(float3 position, uint instanceID,
    vk::BufferPointer<GfxDraws> draws,
    vk::BufferPointer<RenderViewBuffer> renderViews,
    uint renderViewIndex)
{
    float4x4 model = draws.Get().draws[instanceID].model;
    float4 world = mul(model, float4(position, 1.0));
    float4x4 viewProj =
        renderViews.Get().views[renderViewIndex].m_viewProjectionMatrix;
    return mul(viewProj, world);
}
