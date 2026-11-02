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
// DXC requires explicit struct-member array dimensions (the draw itself is
// bounded by the recorded instanceCount, never this constant). Mirrors
// World2DRenderer::kMaxSprites; the CPU side fails loudly past it.
#define WORLD2D_MAX_SPRITES 65536
struct World2DSprites {
	World2DSprite sprites[WORLD2D_MAX_SPRITES];
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
