// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Shadow-atlas seed VS, opaque phase (VS-only pipeline).
// Same cluster-heap indirect path as gfxseed.vert, but resolves the view
// from the shadow tile's RenderView (cascade/spot slab index) and clamps
// clip z like shadow.vert so casters behind the light's near plane squash
// onto it instead of clipping (leak-free tiles; see shadow.vert.hlsl).
#define GFXSEED_PASS
#include "common.hlsli"
#include "gfxseed.hlsli"

[[vk::push_constant]]
ConstantBuffer<SeedVsMainPush> pcSeed;

SeedVSOut mainVS(SeedVSIn IN)
{
    SeedVSOut OUT;
    OUT.pos = SeedClipPos(IN.position, IN.instanceID, IN.vertexID,
        pcSeed.draws, pcSeed.renderViews, pcSeed.renderViewIndex,
        pcSeed.vsDeformed, pcSeed.vsDeformedBase, pcSeed.paramsValid);
    OUT.pos.z = max(OUT.pos.z, 0.0f);
    OUT.color = float3(1.0f, 1.0f, 1.0f);
    OUT.uv = float2(0.0f, 0.0f);
    // No FS in this pipeline (opaque atlas seed, depth-only): the
    // dither-closeup outputs stay inert zeros (the MASKED twin is the one
    // that fills them for shadow.frag's screen-door).
    OUT.worldPos = float3(0.0f, 0.0f, 0.0f);
    OUT.ditherCloseup = 0u;
    return OUT;
}
