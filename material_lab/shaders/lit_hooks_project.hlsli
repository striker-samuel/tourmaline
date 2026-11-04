// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Approximate toon shading through HDR luminance bands, not a per-light BRDF.
// Post-lit hooks cannot replace individual sun / point / spot contributions.
#include "lit_hooks_api.hlsli"

void TM_ModifySurface(inout TMSurface surface, TMLitInput input)
{
    surface.metallic = 0.0f;
    surface.roughness = 1.0f;
}

void TM_ModifyLitColor(inout float3 hdrColor, TMLitInput input)
{
    const float3 positive = max(hdrColor, float3(0.0f, 0.0f, 0.0f));
    const float luminance = dot(positive, float3(0.2126f, 0.7152f, 0.0722f));
    if (luminance < 1e-5f)
        return;

    const float bandsPerStop = 2.0f;
    const float bandedLog =
        floor(log2(luminance) * bandsPerStop + 0.5f) / bandsPerStop;
    hdrColor = positive * (exp2(bandedLog) / luminance);
}
