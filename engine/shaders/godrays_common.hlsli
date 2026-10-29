// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Screen-space god rays - the shared helpers (the sun's screen
// projection + the dither), included by godrays_mask.frag.hlsl and
// godrays_radial.frag.hlsl. The composite/filter passes need neither.
//
// Coordinate convention (the repo's, see ssr.h's audit block): the
// main view bakes proj[1][1] *= -1 for Vulkan, so NDC is y-down and
// uv = ndc * 0.5 + 0.5 lands top-left = (0,0) - the SAME space the
// fullscreen passes' IN.uv covers, so mask/radial/composite all agree
// with no flips.

#define GODRAYS_PASS

// The sun's screen UV (a WORLD DIRECTION - the sun is at infinity, so
// the projection point is the camera pushed far along the direction).
// out valid = the sun is in front of the camera (clip.w > 0); callers
// blank the mask/rays when it is not (an off-screen or behind-camera
// sun produces no screen-space shafts - the mask is empty and the
// radial march gathers nothing, no special casing needed downstream).
void GodraysSunUv(FrameUniforms frame, out float2 sunUv, out bool valid)
{
    const float3 sunDir =
        normalize(frame.env.sunDirection.xyz + 1e-9);
    // A large finite distance: any value >> the far plane projects to
    // the same screen point (a direction, not a position).
    const float4 clip = mul(frame.viewProj,
        float4(frame.cameraPos + sunDir * 1.0e5, 1.0));
    valid = clip.w > 0.0;
    sunUv = valid ? (clip.xy / clip.w * 0.5 + 0.5) : float2(-1.0, -1.0);
}

// Interleaved gradient noise (the VolIgn form - the radial march's
// per-pixel start dither: without it the 32 discrete samples band into
// concentric rings around the sun).
float GodraysIgn(float2 px)
{
    return frac(52.9829189 * frac(dot(px,
        float2(0.06711056, 0.00583715))));
}
