// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU-scene indexed MASK depth pass, both temporal seed and remainder.
// Reuse the exact legacy reversed-Z conservative depth + alpha-cutout body.
// Unlike the per-material legacy pass, material ID is the flat GPU work
// varying (location 7) delivered by gpu_scene_raster.vert.hlsl.
#define GPU_SCENE_MASKED_DEPTH 1
#include "common.hlsli"
#include "shadow_body.hlsli"
