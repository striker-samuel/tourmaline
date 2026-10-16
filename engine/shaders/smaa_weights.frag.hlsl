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
    float2 pixcoord : TEXCOORD4;
};

// Subsample indices come from the push constant (T2x phase selection;
// zeros for SMAA 1x - see @SUBSAMPLE_INDICES in smaa.hlsli).
float4 mainPS(PSIn IN) : SV_Target0
{
    float4 offset[3] = { IN.offset0, IN.offset1, IN.offset2 };
    Texture2D edgesTex = textures[NonUniformResourceIndex(sp.edgesTexIndex)];
    Texture2D areaTex = textures[NonUniformResourceIndex(sp.areaTexIndex)];
    Texture2D searchTex = textures[NonUniformResourceIndex(sp.searchTexIndex)];
    float4 subsampleIndices = float4(sp.subsampleIdxX, sp.subsampleIdxY,
        sp.subsampleIdxZ, sp.subsampleIdxW);
    return SMAABlendingWeightCalculationPS(IN.texcoord, IN.pixcoord, offset,
        edgesTex, areaTex, searchTex, subsampleIndices);
}
