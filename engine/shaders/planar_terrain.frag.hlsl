// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#define TERRAIN_NOPC
#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"
#include "terrain_push.hlsli"

struct VSOut {
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 tangentBFlip : TEXCOORD2;
    [[vk::location(3)]] float4 color : COLOR1;
};
float4 mainPS(VSOut IN) : SV_Target0 {
    clip(IN.color.a - 0.5f);
    FrameUniforms frame = pcT.frameUniforms.Get();
    EnvironmentFrameData env = frame.env;
    float3 N = normalize(IN.normal);
    float3 albedo = lerp(float3(0.16,0.20,0.11), float3(0.55,0.56,0.50), IN.color.r);
    albedo = lerp(albedo, float3(0.36,0.33,0.30), saturate((1.0-N.y)*3.2));
    float3 L = normalize(env.sunDirection.xyz + 1e-9);
    float3 sunE = env.sunColor.rgb * max(env.sunColor.w,0.0) * saturate(dot(N,L));
    return float4(SanitizeHdr(albedo * (sunE / 3.14159265 + EvaluateIrradiance(N,env))),1);
}
