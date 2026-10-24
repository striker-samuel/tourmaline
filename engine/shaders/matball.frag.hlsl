// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// MATERIAL BALL PREVIEW — the FS half (see matball.vert.hlsl for the pass
// contract). One program, two draws, branched on the interpolated flag:
//   flag=0 — the DEFAULT SKY background: the analytic EvaluateSkyColor
//     over the CPU-built corner rays, the fixed default env below
//   flag=1 — the BALL: the material's PBR under that same env —
//     the analytic sun's GGX direct + the sky/ground hemisphere ambient
//     (EvaluateSkyGradient at N and at the fresnel reflection R, blended
//     by SchlickFresnel — a cheap stand-in for the 1-integral prefiltered
//     IBL the runtime chain bakes; honest for a thumbnail's glance read)
//     + the bindless texture slots (base/MR/occlusion, glTF channels).
// Everything is HDR (pre-exposure units like the runtime sky); the final
// color gets a fixed filmic tonemap + gamma so the PNG lands display-ready
// (the offscreen is RGBA8_SRGB, but the tonemap is the look — a raw
// 5.0-HDR sun would clip to flat white without it).
#define MATBALL_PASS
#include "common.hlsli"
#include "environment.hlsli"

// Mirrors editor::MatBallPush (editor_app.h) exactly.
struct MatBallPush
{
    float4x4 viewProj;      // 0
    float4 eye;             // 64
    float4 baseColorFactor; // 80
    float4 emissiveFactor;  // 96
    float4 params;          // 112 x metallic, y perceptual roughness
    uint baseTex;           // 128
    uint mrTex;             // 132
    uint normalTex;         // 136
    uint occlTex;           // 140
};

[[vk::push_constant]]
ConstantBuffer<MatBallPush> mb;

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float flag : TEXCOORD3;
};

// THE DEFAULT SKY SETTING (fixed — deliberately not the scene's env: the
// sidecar must look the same for every material, every project): a
// clear midday-ish sky, a warm sun at ~35deg elevation from +X. Values in
// the same linear/HDR units the runtime env block carries.
static const EnvironmentFrameData kDefaultEnv =
{
    /* skyZenith       */ float4(0.20f, 0.42f, 0.85f, 1.0f),
    /* skyHorizon      */ float4(0.62f, 0.74f, 0.90f, 0.9f),
    /* ground          */ float4(0.28f, 0.24f, 0.20f, 1.0f),
    /* sunDirection    */ float4(0.716f, 0.574f, 0.396f, 0.9997f),
    /* sunColor        */ float4(1.0f, 0.95f, 0.88f, 5.0f),
    /* fogColor        */ float4(0.6f, 0.7f, 0.85f, 0.0f),
    /* fogDensityHeight*/ float4(0.0f, 0.0f, 0.0f, 0.0f),
    /* fogEndAndFlags  */ float4(1000.0f, 1.0f, 0.0f, 0.0f) // y=1: sky ON
};

// GGX specular (the standard single-term cut of default_lit_body's
// chain — distribution + Smith geometry + Schlick F).
static float D_GGX(float NdotH, float a)
{
    const float a2 = a * a;
    const float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (3.14159265f * d * d);
}
static float G_Smith(float NdotV, float NdotL, float rough)
{
    const float k = (rough + 1.0f) * (rough + 1.0f) / 8.0f;
    return (NdotV / (NdotV * (1.0f - k) + k))
        * (NdotL / (NdotL * (1.0f - k) + k));
}

// Fixed filmic (Unreal-2-ish ACES approximation — the same family the
// runtime tonemap uses) + the sRGB transfer curve: a self-contained
// display path for the thumbnail bytes.
static float3 TonemapFilmic(float3 x)
{
    const float3 a = x * (2.51f * x + 0.03f);
    const float3 b = x * (2.43f * x + 0.59f) + 0.14f;
    return saturate(a / b);
}
static float3 ToSrgb(float3 c)
{
    return saturate(lerp(c * 12.92f,
        1.055f * pow(c, 1.0f / 2.4f) - 0.055f, step(0.0031308f, c)));
}

float4 mainPS(VSOut IN) : SV_Target0
{
    if (IN.flag < 0.5f) {
        // ---- The default-sky BACKGROUND ------------------------------
        const float3 dir = normalize(IN.normal); // the CPU-built ray
        return float4(ToSrgb(TonemapFilmic(
            EvaluateSkyColor(dir, kDefaultEnv))), 1.0f);
    }

    // ---- The BALL --------------------------------------------------
    const float3 N = normalize(IN.normal);
    const float3 V = normalize(mb.eye.xyz - IN.worldPos);
    const float3 L = normalize(kDefaultEnv.sunDirection.xyz);
    const float3 H = normalize(V + L);
    const float NdotV = max(dot(N, V), 1e-4f);
    const float NdotL = saturate(dot(N, L));
    const float NdotH = saturate(dot(N, H));

    // Material slots (bindless; unbound -> the factor alone).
    float4 base = mb.baseColorFactor;
    if (mb.baseTex != 0xffffffffu)
        base *= textures[NonUniformResourceIndex(mb.baseTex)]
            .SampleLevel(samplers[NonUniformResourceIndex(0)], IN.uv, 0.0f);
    float rough = mb.params.y;
    float metal = mb.params.x;
    if (mb.mrTex != 0xffffffffu) {
        const float4 mr = textures[NonUniformResourceIndex(mb.mrTex)]
            .SampleLevel(samplers[NonUniformResourceIndex(0)], IN.uv, 0.0f);
        rough *= mr.g;
        metal *= mr.b;
    }
    if (mb.occlTex != 0xffffffffu)
        base.rgb *= textures[NonUniformResourceIndex(mb.occlTex)]
            .SampleLevel(samplers[NonUniformResourceIndex(0)], IN.uv, 0.0f).r;
    rough = max(rough, 0.045f);

    // Direct sun (GGX).
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f),
        base.rgb, metal);
    const float a = rough * rough;
    const float3 F = F0
        + (1.0f - F0) * pow(1.0f - saturate(dot(V, H)), 5.0f);
    const float3 spec = (D_GGX(NdotH, a) * G_Smith(NdotV, NdotL, rough)
        * F * kDefaultEnv.sunColor.rgb * kDefaultEnv.sunColor.w)
        / (4.0f * NdotV * max(NdotL, 1e-4f) + 1e-4f);
    const float3 kd = (1.0f - F) * (1.0f - metal);
    const float3 diffuse = kd * base.rgb * NdotL
        * kDefaultEnv.sunColor.rgb * kDefaultEnv.sunColor.w / 3.14159265f;

    // Ambient: the sky/ground hemisphere (the gradient at N, split by
    // the up-facing ratio) + the fresnel-weighted sky reflection (the
    // gradient at R — the "chrome ball in a default sky" read).
    const float3 ambSky = EvaluateSkyGradient(N, kDefaultEnv);
    const float3 ambDiff = kd * base.rgb * ambSky * 0.5f
        + kd * base.rgb * kDefaultEnv.ground.rgb * 0.3f;
    const float3 R = reflect(-V, N);
    const float3 ambSpec = EvaluateSkyGradient(R, kDefaultEnv)
        * (F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f)) * 0.5f;

    const float3 emissive = mb.emissiveFactor.rgb
        * max(mb.emissiveFactor.w, 0.0f);

    const float3 hdr = diffuse + spec + ambDiff + ambSpec + emissive;
    return float4(ToSrgb(TonemapFilmic(hdr)), 1.0f);
}
