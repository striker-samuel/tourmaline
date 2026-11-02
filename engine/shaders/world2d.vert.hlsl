// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define UI_PASS // suppress unrelated 3D scene push constants
#include "common.hlsli"
#include "world2d.hlsli"

struct VSOut {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

// Sprite center position; six procedural vertices (no quad/index buffer).
static const float2 corners[6] = {
	float2(0,0), float2(1,0), float2(1,1),
	float2(0,0), float2(1,1), float2(0,1)
};
VSOut mainVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
	World2DSprite s = spritePush.instances.Get().sprites[instance];
	float2 uv = corners[vertex];
	float2 local = (uv - 0.5f) * s.size;
	float sn, cs;
	sincos(s.rotation, sn, cs);
	float2 rotated = float2(local.x * cs - local.y * sn,
	                        local.x * sn + local.y * cs);
	float2 pixel = (s.position + rotated - spritePush.camera) *
		spritePush.pixelsPerUnit;
	float2 clip = pixel / spritePush.viewport * 2.0f - 1.0f;
	// Sort is authoritative for transparent sprites. Depth is encoded but
	// depth writes/tests are disabled, so equal-Z sprites retain submission
	// order and arbitrary world Z values are never clipped.
	float clipZ = saturate((s.z - spritePush.minZ) / spritePush.zRange);
	VSOut o;
	o.position = float4(clip.x, -clip.y, clipZ, 1.0f);
	o.uv = lerp(s.uvMin, s.uvMax, uv);
	o.color = s.color;
	return o;
}
