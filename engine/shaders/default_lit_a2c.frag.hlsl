// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Main forward lighting FS, alpha-to-coverage variant for Masked (cutout)
// materials in 2x MSAA Main. Opt-in (TOURMALINE_A2C=1): cutout comes from the
// coverage remap of the output alpha instead of a discard, so foliage
// silhouettes resolve AA'd — but coverage needs the FS output, which moves
// the per-sample depth test after the shader (overlapping layers lose
// early-reject; +6ms on the bistro foliage bench). Pipelines pair this with
// MSAASettings::Count2A2C().
#define ALPHA_TO_COVERAGE 1
#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"
#include "sggi.hlsli"
#include "sg_lightmap.hlsli"
#include "sg_regular.hlsli"
#include "sg_cvol.hlsli"
#include "default_lit_body.hlsli"
