// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Depth-only alpha-cutout for GPUScene casters. The shared shadow body uses
// per-instance material IDs, but retains the main-camera anchored dither
// test rather than treating the shadow RenderView as the viewer.
#define GFXFILTERED_PASS
#define TM_SHADOW_CASTER_PASS 1
#define SHADOW_NORMAL_Z 1
#define GPU_SCENE_SHADOW_MASKED 1
#include "common.hlsli"
#include "shadow_body.hlsli"
