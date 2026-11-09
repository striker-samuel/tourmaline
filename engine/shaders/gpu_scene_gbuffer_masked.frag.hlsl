// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU-scene MASK G-buffer, same shadow/depth alpha test and surface layout
// as the legacy gbuffer_masked twin. Flat material from GPU work varying.
#define GPU_SCENE_GBUFFER 1
#include "gbuffer_masked.frag.hlsl"
