// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// MATERIAL BALL PREVIEW (the sidecar .material thumbnails): a DEDICATED
// pass — no scene pc, its own push block (MatBallPush, mirrored by the C++
// struct in editor_app.h exactly + a static_assert there). The program
// draws TWO geometries per pass (the flag in worldPos.w tells them apart):
//   draw 1 — a fullscreen triangle: the DEFAULT SKY background (the
//     analytic EvaluateSkyColor over the pixel's view ray, the fixed
//     default env below — "a ball in a default sky setting")
//   draw 2 — the UV sphere: the material's PBR look (bindless texture
//     slots + scalar factors) under the SAME default env's analytic sun
//     + sky/ground hemisphere ambient + a fresnel sky reflection term.
// VSIn: attributes 0..2 (POSITION + NORMAL + TEXCOORD — the editor-side
// sphere builder packs exactly these; the matball needs no tangent).
#define MATBALL_PASS
#include "common.hlsli"
#include "environment.hlsli"

// Mirrors editor::MatBallPush (editor_app.h) exactly — layout-identical,
// check the static_assert(sizeof(...)) there.
struct MatBallPush
{
    float4x4 viewProj;     // 0   the fixed preview camera's VP (reversed-Z)
    float4 eye;            // 64  xyz camera, w unused
    float4 baseColorFactor; // 80  rgba (the material's factor)
    float4 emissiveFactor;  // 96  rgb, w = emissive strength
    float4 params;          // 112 x metallic, y perceptual roughness, zw unused
    uint baseTex;           // 128 bindless slot (0xffffffffu = unbound)
    uint mrTex;             // 132 (G = roughness, B = metallic — glTF)
    uint normalTex;         // 136 (unused yet — reserved for the tangent-free
                            //      perturbation follow-up)
    uint occlTex;           // 140 (applied as a diffuse multiplier)
};

[[vk::push_constant]]
ConstantBuffer<MatBallPush> mb;

struct VSIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    // The fullscreen-sky triangle reuses the same input slots (all zeros
    // except POSITION carrying NDC — the FS branches on the flag).
    [[vk::location(3)]] float flag : TEXCOORD1;
};

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0; // w carries the flag
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float flag : TEXCOORD3;
};

VSOut mainVS(VSIn IN)
{
    VSOut OUT;
    if (IN.flag > 0.5f) {
        // The sphere draw: world transform is identity (the camera is
        // framed around the unit ball at the origin — the push owns it).
        OUT.worldPos = IN.position;
        OUT.normal = IN.normal;
        OUT.uv = IN.uv;
        OUT.flag = 1.0f;
        OUT.pos = mul(mb.viewProj, float4(IN.position, 1.0));
    }
    else {
        // The fullscreen-sky draw: POSITION carries NDC xy; NORMAL
        // carries the CPU-built WORLD RAY at that corner (the FS
        // normalizes the interpolated ray — no inverse-VP push needed).
        OUT.worldPos = float3(0.0f, 0.0f, 0.0f);
        OUT.normal = IN.normal;
        OUT.uv = float2(0.0f, 0.0f);
        OUT.flag = 0.0f;
        OUT.pos = float4(IN.position.xy, 0.0f, 1.0f);
    }
    return OUT;
}
