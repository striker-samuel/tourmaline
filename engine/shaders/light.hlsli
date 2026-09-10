#pragma once

struct Light
{
    float4 color; // 16B

    float3 position; // 12B
    int flags;
    //float _pad0; // 4B = pad to 16B

    float3 direction; // 12B
    float intensity; // 4B

    int type; // 4B
    float innerAngle;
    float outerAngle;
    int shadow_map_index;
    float4x4 lightVP;
};

struct SpotLight
{
    float4 color; // 16B

    float3 position; // 12B
    int flags;
    //float _pad0; // 4B = pad to 16B

    float3 direction; // 12B
    float intensity; // 4B

    int type; // 4B
    float innerAngle;
    float outerAngle;
    int shadow_map_index;
    float4x4 lightVP;
};

// ---------------------------------------------------------------------------
// Light cookies & light functions helpers.
// Require common.hlsli symbols (ForwardPlusLight) -> include after it; the
// helpers below only use scalar builtins so they are safe anywhere.
// ---------------------------------------------------------------------------

float LF_Hash21(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

// Cone UV for a spot light: maps a receiver direction (light -> receiver,
// normalized) into [0,1]^2 with the cone edge at 1. Matches the CPU spot
// projection GetSpotLightVP (glm::lookAt basis + perspective fov = 2*outer,
// no Y-flip): uv = ndc * 0.5 + 0.5 with
//   ndc = (dot(s, dir), dot(u, dir)) / (dot(axis, dir) * tan(outer)),
// s/u the lookAt right/up basis, so cookies line up with spot shadows.
float2 SpotConeUV(float3 axis, float3 toReceiver, float outerAngle)
{
    float3 upRef = abs(axis.y) > 0.99f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(0.0f, 1.0f, 0.0f);
    float3 s = normalize(cross(axis, upRef));
    float3 u = cross(s, axis);
    float z = dot(axis, toReceiver);
    float r = max(z * tan(min(outerAngle, 1.5f)), 1e-4f);
    return float2(dot(s, toReceiver) / r, dot(u, toReceiver) / r) * 0.5f + 0.5f;
}

// Procedural light function: per-pixel intensity modulation for a light.
// Types (must match C++ kForwardPlusFunction_*):
//   1 flicker - stepped candle-like time noise (mostly holds, quick jumps)
//   2 pulse   - sine breathing over time
//   3 bands   - moving diagonal bands across the spot cone (spatial; spots
//               only - points fall back to pulse)
// speed scales the time axis; scale is the modulation depth (1 = dips to
// black, 0 = flat). Returns a scalar in [1 - scale, 1].
float EvaluateLightFunction(int type, float speed, float scale,
    float time, float2 spotUV, bool isSpot)
{
    if (type <= 0)
        return 1.0f;

    const float spd = max(speed, 0.01f);
    const float amp = saturate(scale);
    if (amp <= 0.0f)
        return 1.0f;

    if (type == 1) // flicker
    {
        float t = time * spd * 24.0f;
        float n0 = LF_Hash21(float2(floor(t), 7.0f));
        float n1 = LF_Hash21(float2(floor(t) + 1.0f, 7.0f));
        // Hold the current step, jump quickly near the end of it.
        float f = lerp(n0, n1, smoothstep(0.7f, 1.0f, frac(t)));
        return lerp(1.0f - amp, 1.0f, f);
    }

    if (type == 2) // pulse
    {
        float f = 0.5f + 0.5f * sin(time * spd * 6.2831853f);
        return lerp(1.0f - amp, 1.0f, f);
    }

    if (type == 3) // bands (spatial on the cone; temporal fallback)
    {
        float f;
        if (isSpot)
        {
            float phase = (spotUV.x + spotUV.y) * max(scale, 1.0f) * 12.566371f
                + time * spd * 6.2831853f;
            f = 0.5f + 0.5f * sin(phase);
        }
        else
        {
            f = 0.5f + 0.5f * sin(time * spd * 6.2831853f);
        }
        return lerp(1.0f - amp, 1.0f, f);
    }

    return 1.0f;
}




