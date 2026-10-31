// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0
// (the 04_project_game sample content — see LICENSE.md)

// Damage flash (the design doc's worked example A): an independent
// per-instance hit flash driven by userdata bits 0..7. The tint scales
// the ASSEMBLED lit color (fog included), so the flash reads relative
// to the scene's HDR lighting instead of a fixed emissive value —
// dark rooms flash dim, bright rooms flash bright.
//
// NOT the player's full-screen damage redout: that is a view-scoped
// post effect, never a material instance (design §10).

#ifndef TM_FX_DAMAGE_HLSLI
#define TM_FX_DAMAGE_HLSLI

void ApplyDamageFlash(inout float3 hdrColor, TMLitInput input)
{
    const float damage = float(input.userData & 0xFFu) / 255.0f;
    // Relative to HDR scene lighting, instead of a fixed low HDR value.
    const float3 flashScale = float3(3.0f, 0.15f, 0.15f);
    hdrColor *= lerp(float3(1.0f, 1.0f, 1.0f), flashScale, damage);
}

#endif // TM_FX_DAMAGE_HLSLI
