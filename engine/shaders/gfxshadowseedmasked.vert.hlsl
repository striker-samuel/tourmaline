// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Shadow-atlas seed VS, MASK cutout phase (pairs with shadow.frag
// alpha-test). Same VSOut subset as shadow.vert so it links with
// shadow.frag; z clamp parity with shadow.vert / gfxshadowseed.vert.
#define GFXSEED_PASS
#include "common.hlsli"
#include "gfxseed.hlsli"
#include "gpu_platform.hlsli"

GPU_PUSH(SeedVsMainPush, pcSeed);

SeedVSOut mainVS(SeedVSIn IN)
{
    SeedVSOut OUT;
    OUT.pos = SeedClipPos(IN.position, IN.instanceID, IN.vertexID,
        pcSeed.draws, pcSeed.renderViews, pcSeed.renderViewIndex,
        pcSeed.vsDeformed, pcSeed.vsDeformedBase, pcSeed.paramsValid);
    OUT.pos.z = max(OUT.pos.z, 0.0f);
    OUT.color = IN.color.rgb;
    OUT.uv = IN.uv;
    // Dither-on-closeup inputs for shadow.frag's screen-door (the
    // closeup-fade draws ride the MASKED seed bucket — see gfxseed.hlsli).
    OUT.worldPos = SeedWorldPos(IN.position, IN.instanceID, IN.vertexID,
        pcSeed.draws, pcSeed.vsDeformed, pcSeed.vsDeformedBase,
        pcSeed.paramsValid);
    OUT.ditherCloseup = SeedDitherCloseup(IN.instanceID, pcSeed.draws);
    return OUT;
}
