// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Receiver normal bias for a perspective shadow tile. At eye depth z,
// half a shadow texel spans z * tan(halfFov) / tileResolution world units.
// In PCF, PCSS, and EVSM, normalBias remains a world-space upper limit,
// not an unconditional displacement. Point faces use dominant-axis depth.
float PerspectiveShadowNormalBias(
    float eyeDepth,
    float tanHalfFov,
    float invTileResolution,
    float maxWorldBias)
{
    const float halfTexelWorld =
        max(eyeDepth, 0.0f) * max(tanHalfFov, 0.0f)
        * max(invTileResolution, 0.0f);
    return min(max(maxWorldBias, 0.0f), halfTexelWorld);
}
