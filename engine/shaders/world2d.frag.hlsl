// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define UI_PASS
#include "common.hlsli"
#include "world2d.hlsli"

float4 mainPS(float4 position : SV_Position, float2 uv : TEXCOORD0,
              float4 color : COLOR0) : SV_Target0 {
	return textures[NonUniformResourceIndex(spritePush.textureIndex)]
		.Sample(samplers[NonUniformResourceIndex(spritePush.samplerIndex)], uv)
		* color;
}
