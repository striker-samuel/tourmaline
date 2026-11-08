// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU-scene depth prepass G-buffer variant: same receiver and F0-chroma
// outputs as the direct prepass, but the material comes from the
// generation-checked GPU-scene instance (flat VS location 7).
#define GPU_SCENE_GBUFFER 1
#include "gbuffer.frag.hlsl"
