// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include "gpu_platform.hlsli"

// Mirrors tour::GameUiRectInstance (72 B) and GameUiRenderer::Push (24 B).
struct GameUiRectInstance {
	float2 origin;
	float2 size;
	float2 uvMin;
	float2 uvMax;
	float4 tint;
	float2 clipMin;
	float2 clipMax;
	uint textureIndex;
	uint pad;
};
struct GameUiInstances {
	GameUiRectInstance rects[];
};
struct GameUiPush {
	GPU_PTR_READ(GameUiInstances) instances;
	float2 viewport;
	uint samplerIndex;
	uint pad;
};
GPU_PUSH(GameUiPush, gameUiPush);
