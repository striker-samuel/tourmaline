// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define SMAA_PASS
#include "common.hlsli"

#include "smaa_push.hlsli"

#define SMAA_PRESET_HIGH
#define SMAA_VULKAN_BINDLESS
#define SMAA_INCLUDE_VS 0
#include "smaa.hlsli"

struct PSIn
{
    float2 texcoord : TEXCOORD0;
    float4 offset0 : TEXCOORD1;
    float4 offset1 : TEXCOORD2;
    float4 offset2 : TEXCOORD3;
};

struct PSOut
{
    float4 target : SV_Target0;
};

// Luma edge detection (official default). The color input is the
// tonemapper's manual-gamma LDR output (UNORM) - exactly the
// "gamma-corrected, non-sRGB" input the official code requires.
// Discards non-edge pixels; the target is cleared to (0,0,0,0) each frame.
PSOut mainPS(PSIn IN)
{
    PSOut OUT;
    float4 offset[3] = { IN.offset0, IN.offset1, IN.offset2 };
    Texture2D colorTex = textures[NonUniformResourceIndex(sp.colorTexIndex)];
    float2 edges = SMAALumaEdgeDetectionPS(IN.texcoord, offset, colorTex);
    OUT.target = float4(edges, 0.0, 1.0);
    return OUT;
}
