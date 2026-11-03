// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Directional CSM receiver-plane bias (normal-Z, positive depth away from
// the sun). The old 1-NdotL approximation bounded the entire grazing
// correction to one base unit even as the depth gradient approached a
// near-parallel receiver plane. tan(theta) tracks that gradient; cap it
// to prevent extreme peter-panning at silhouettes and shadow terminators.
// This is ONLY for directional cascades: spots/points retain their own
// perspective-calibrated receiver bias.
float CascadeOrientedDepthBias(float3 faceNormal, float3 lightDirection,
    float baseBiasNDC)
{
    const float3 N = normalize(faceNormal);
    const float3 L = normalize(-lightDirection);
    const float cosTheta = saturate(dot(N, L));
    const float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    const float slope = min(sinTheta / max(cosTheta, 0.2f), 4.0f);
    // A half-base constant handles facing-plane precision; the bounded
    // receiver-plane slope handles the dominant grazing-angle acne.
    return -max(baseBiasNDC, 0.0f) * (0.5f + slope);
}

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
