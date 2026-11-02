// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define UI_PASS
#include "common.hlsli"
#include "gameui.hlsli"

struct UiVsOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 tint : COLOR0;
	nointerpolation float4 clipRect : TEXCOORD1;
	nointerpolation uint textureIndex : TEXCOORD2;
};
static const float2 corners[6] = {
	float2(0, 0), float2(1, 0), float2(1, 1),
	float2(0, 0), float2(1, 1), float2(0, 1)
};

UiVsOut mainVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
	GameUiRectInstance rect = gameUiPush.instances.Get().rects[instance];
	const float2 corner = corners[vertex];
	const float2 px = rect.origin + rect.size * corner;
	const float2 ndc = px / gameUiPush.viewport * 2.0f - 1.0f;
	UiVsOut o;
	o.position = float4(ndc.x, -ndc.y, 0.0f, 1.0f);
	o.uv = lerp(rect.uvMin, rect.uvMax, corner);
	o.tint = rect.tint;
	o.clipRect = float4(rect.clipMin, rect.clipMax);
	o.textureIndex = rect.textureIndex;
	return o;
}
