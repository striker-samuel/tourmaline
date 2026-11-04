// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define TONEMAP_PASS
#include "common.hlsli"
#include "gpu_platform.hlsli"

struct PSIn { float2 uv : TEXCOORD0; };
struct PSOut { float4 outColor : SV_Target0; };

// Keep byte-for-byte layout in sync with DistortionPush in distortion.h (64 bytes).
struct DistortionPush
{
    uint sceneIndex;
    uint fieldIndex;
    uint samplerIndex;
    uint mode;
    float strengthPx;
    float time;
    float speed;
    float frequency;
    float2 texelSize;
    float2 center;
    float2 radius;
    float edgeSoftness;
    float fieldScale;
};
GPU_PUSH(DistortionPush, dp);

// A localized, smooth elliptical influence with an optional feathered rim.
// radius is normalized screen UV; (1,1) at (0.5,0.5) covers the whole screen.
float Influence(float2 uv)
{
    float2 r = max(abs(dp.radius), float2(1e-4f, 1e-4f));
    float dist = length((uv - dp.center) / r);
    float feather = max(dp.edgeSoftness, 1e-4f);
    return 1.0f - smoothstep(max(0.0f, 1.0f - feather), 1.0f, dist);
}

PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    Texture2D scene = textures[NonUniformResourceIndex(dp.sceneIndex)];
    SamplerState smp = samplers[NonUniformResourceIndex(dp.samplerIndex)];

    float2 offset = float2(0.0f, 0.0f);
    if (dp.mode == 0u && dp.fieldIndex != 0xffffffffu)
    {
        // User-authored RG flow map: 0.5 = neutral, 0..1 = negative/positive.
        // B is an optional per-pixel strength mask (white = full strength).
        Texture2D field = textures[NonUniformResourceIndex(dp.fieldIndex)];
        float3 flow = field.Sample(smp, IN.uv).rgb;
        offset = (flow.rg * 2.0f - 1.0f) * saturate(flow.b) * dp.fieldScale;
    }
    else if (dp.mode == 1u)
    {
        // Two drifting wave bands give heat haze without an external texture.
        // Convert UV into approximately isotropic pixel-space coordinates
        // so animation does not stretch when the viewport aspect ratio changes.
        float2 pixel = IN.uv / max(dp.texelSize, float2(1e-6f, 1e-6f));
        float phase = dp.time * dp.speed;
        float a = sin(pixel.y * dp.frequency + phase +
                      sin(pixel.x * dp.frequency * 0.41f - phase * 0.73f));
        float b = sin(pixel.x * dp.frequency * 0.67f - phase * 0.62f +
                      sin(pixel.y * dp.frequency * 0.29f + phase));
        offset = float2(a, b) * 0.5f;
    }

    // Strength is measured in source pixels, not resolution-dependent UV.
    float2 uvOffset = offset * max(dp.strengthPx, 0.0f) * dp.texelSize *
                      Influence(IN.uv);
    // Clamp to source texel centers; wrap samplers must not bleed opposite edges.
    float2 sampleUv = clamp(IN.uv + uvOffset, dp.texelSize * 0.5f,
                           1.0f - dp.texelSize * 0.5f);
    OUT.outColor = float4(SanitizeHdr(scene.Sample(smp, sampleUv).rgb), 1.0f);
    return OUT;
}
