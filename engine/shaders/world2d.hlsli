// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include "gpu_platform.hlsli"

// Must match tour::SpriteInstance2D / World2DRenderer::Push.
struct World2DSprite {
	float2 position;
	float2 size;
	float2 uvMin;
	float2 uvMax;
	float4 color;
	float rotation;
	float z;
};
struct World2DSprites {
	World2DSprite sprites[];
};
struct World2DPush {
	GPU_PTR_READ(World2DSprites) instances;
	float2 camera;
	float2 viewport;
	float pixelsPerUnit;
	float minZ;
	float zRange;
	uint textureIndex;
	uint samplerIndex;
	uint pad;
};
GPU_PUSH(World2DPush, spritePush);
