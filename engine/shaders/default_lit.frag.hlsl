// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Main forward lighting FS (classic alpha-test cutout variant).
// The shared body (default_lit_body.hlsli) holds everything; ALPHA_TO_COVERAGE
// is undefined here so Masked materials keep the discard alpha test.
// The STANDARD no-op hook family (StandardLit): a project lit family
// includes its own hook library here instead — same body, same seam
// (docs/shader_library_design.md).
#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"
#include "sggi.hlsli"
#include "sg_lightmap.hlsli"
#include "sg_regular.hlsli"
#include "sg_gi_volume.hlsli"
#include "lit_hooks_default.hlsli"
#include "default_lit_body.hlsli"
