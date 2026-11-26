// SPDX-License-Identifier: MIT
#pragma once
#include "common.hlsli"
#include "ddgi_common.hlsli"

float3 DdgiSkyRadiance(FrameUniforms frame, float3 direction) {
    // Miss: evaluate the environment radiance in this direction. Sky up,
    // ground down, scaled by the UNGATED sky scale (ground.w =
    // iblIntensity = skyIntensity), never skyZenith.w (the specular sky
    // intensity) AND never fogEndAndFlags.w: that flag is the
    // diffuse-IBL-isolation gate, and it is 0 while DDGI owns the
    // ambient (IblWanted() is false) — folding it in would zero the
    // field this tracer is supposed to produce. EvaluateSpecularEnv
    // makes the same call (ungated ground.w). The DDGI field REPLACES
    // the gated analytic ambient, so it must capture the sky itself.
    const float ibl = max(frame.env.ground.w, 0.0f);
    const float exponent = max(frame.env.skyHorizon.w, 0.001f);
    const float up = saturate(direction.y);
    const float dn = saturate(-direction.y);
    const float3 skyPart =
        lerp(frame.env.skyHorizon.rgb, frame.env.skyZenith.rgb, pow(up, exponent));
    const float3 gndPart =
        lerp(frame.env.skyHorizon.rgb, frame.env.ground.rgb, saturate(dn * 1.5f));
    const float3 hemi = lerp(gndPart, skyPart, saturate(direction.y * 0.5f + 0.5f));
    return max(0, hemi * ibl);
}

DdgiRay DdgiEmptyRay(float3 direction, float maxDistance) {
    DdgiRay result;
    result.direction = direction;
    result.distance = maxDistance;
    result.radiance = 0;
    result.flags = 0;
    return result;
}
