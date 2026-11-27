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

// Ungated hemispherical sky irradiance for a surface normal (the analytic
// sky the DDGI field is computed against). Mirrors EvaluateIrradiance but
// WITHOUT the fogEndAndFlags.w diffuse-IBL-isolation gate, which is 0 while
// DDGI owns the ambient.
float3 DdgiSkyIrradiance(float3 N, FrameUniforms frame) {
    const float ibl = max(frame.env.ground.w, 0.0f);
    const float exponent = max(frame.env.skyHorizon.w, 0.01f);
    const float up = saturate(N.y);
    const float dn = saturate(-N.y);
    const float3 skyPart =
        lerp(frame.env.skyHorizon.rgb, frame.env.skyZenith.rgb, pow(up, exponent));
    const float3 gndPart =
        lerp(frame.env.skyHorizon.rgb, frame.env.ground.rgb, saturate(dn * 1.5f));
    const float3 hemi = lerp(gndPart, skyPart, saturate(N.y * 0.5f + 0.5f));
    return max(0, hemi * ibl);
}

// Multi-bounce feedback: sample the CURRENT volume's PREVIOUS-frame
// irradiance field at a surface hit point toward its normal, returning
// irradiance E (0 when the point is outside the volume or no probe is
// valid). Self-volume only — cross-volume propagation is a later step.
// Mirrors the lit shader's DdgiSampleVolume walk (trilinear + bilinear
// octahedral taps + Chebyshev visibility + normal-facing cosine).
float3 DdgiSampleEIndirect(GPU_PTR_READ(DdgiIrrBuffer) oldIrr,
                           GPU_PTR_READ(DdgiDistanceBuffer) oldDistance,
                           float3 origin, float spacing, uint3 dims,
                           float3 worldPos, float3 normal)
{
    const uint irrStride = DDGI_IRR_RES * DDGI_IRR_RES;
    const uint distStride = DDGI_DIST_RES * DDGI_DIST_RES;
    const float3 biased = worldPos + normal * (0.25f * spacing);
    const float3 local = (biased - origin) / spacing;
    const int3 base = int3(floor(local));
    const float3 frac = local - float3(base);

    uint irrBin[4];
    float irrW[4];
    DdgiOctBilinearTaps(normal, DDGI_IRR_RES, irrBin, irrW);

    float3 acc = float3(0.0, 0.0, 0.0);
    float wSum = 0.0;
    [unroll]
    for (uint c = 0; c < 8; ++c)
    {
        const int3 off = int3(c & 1u, (c >> 1) & 1u, (c >> 2) & 1u);
        const int3 coord = base + off;
        if (coord.x < 0 || coord.y < 0 || coord.z < 0 ||
            coord.x >= int(dims.x) || coord.y >= int(dims.y) || coord.z >= int(dims.z))
            continue;
        const uint probe = uint(coord.x) + uint(coord.y) * dims.x
            + uint(coord.z) * dims.x * dims.y;

        const float3 w = lerp(1.0f - frac, frac, float3(off));
        const float tri = w.x * w.y * w.z;
        if (tri <= 0.0)
            continue;

        float4 irr = float4(0.0, 0.0, 0.0, 0.0);
        [unroll]
        for (uint t = 0; t < 4; ++t)
            irr += oldIrr.Get().values[probe * irrStride + irrBin[t]] * irrW[t];
        if (irr.w <= 0.0)
            continue;

        const float3 probePos = origin + float3(coord) * spacing;
        const float3 toReceiver = biased - probePos;
        const float dist = length(toReceiver);
        const float3 recDir = dist > 1e-5f ? toReceiver / dist : normal;

        uint distBin[4];
        float distW[4];
        DdgiOctBilinearTaps(recDir, DDGI_DIST_RES, distBin, distW);
        float2 moments = float2(0.0, 0.0);
        [unroll]
        for (uint t = 0; t < 4; ++t)
            moments += oldDistance.Get().values[probe * distStride + distBin[t]] * distW[t];
        const float visibility = DdgiVisibility(moments, dist);
        const float facing = max(dot(normal, -recDir), 0.0f);

        const float weight = tri * facing * visibility;
        acc += irr.rgb * weight;
        wSum += weight;
    }
    return wSum > 1e-5f ? acc / wSum : float3(0.0, 0.0, 0.0);
}
