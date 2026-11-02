// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define UI_PASS
#include "common.hlsli"
#include "gameui.hlsli"

struct UiPsIn {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 tint : COLOR0;
	nointerpolation float4 clipRect : TEXCOORD1;
	nointerpolation uint textureIndex : TEXCOORD2;
};

float4 mainPS(UiPsIn IN) : SV_Target0 {
	// Local framebuffer pixels (top-left origin); nested clipping is
	// per instance, so a mixed-texture UI stays ONE instanced draw.
	const float2 p = IN.position.xy;
	if (any(p < IN.clipRect.xy) || any(p >= IN.clipRect.zw))
		discard;
	if (IN.textureIndex == 0xffffffffu)
		return IN.tint;
	return textures[NonUniformResourceIndex(IN.textureIndex)]
		.Sample(samplers[NonUniformResourceIndex(gameUiPush.samplerIndex)],
			IN.uv) * IN.tint;
}
