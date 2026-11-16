// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Depth-only alpha-cutout for GPUScene casters. The shared shadow body uses
// per-instance material IDs, but retains the main-camera anchored dither
// test rather than treating the shadow RenderView as the viewer.
//
// Unlike the VS (which owns the RasterVsPush 'pcS' and suppresses 'pc'), the
// FS reads the scene PushConstants ('pc': frameUniforms / bMats / pad_pc) for
// the dither fade and the material alpha test, so 'pc' must stay declared.
#define TM_SHADOW_CASTER_PASS 1
#define SHADOW_NORMAL_Z 1
#define GPU_SCENE_SHADOW_MASKED 1
#include "common.hlsli"
#include "shadow_body.hlsli"
