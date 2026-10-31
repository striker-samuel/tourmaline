// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0
// (the 04_project_game sample content — see LICENSE.md)

// Runtime wetness (the design doc's worked example B): userdata bits
// 8..15 drive a physically meaningful roughness/baseColor edit on the
// evaluated surface — one instance of a stone/metal material looks
// wet through the NORMAL BRDF path (sharper highlights, darker
// albedo) without a new material asset, a texture or a reupload.
//
// The body's hook contract re-clamps metallic/roughness and guards
// non-finite values AFTER this returns, so the lerp targets stay
// simple.

#ifndef TM_FX_WETNESS_HLSLI
#define TM_FX_WETNESS_HLSLI

void ApplyWetness(inout TMSurface surface, TMLitInput input)
{
    const float wet = float((input.userData >> 8) & 0xFFu) / 255.0f;
    surface.roughness = lerp(surface.roughness, 0.08f, wet);
    surface.baseColor *= lerp(1.0f, 0.72f, wet);
}

#endif // TM_FX_WETNESS_HLSLI
