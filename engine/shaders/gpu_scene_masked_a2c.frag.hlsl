// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Optional 2x MSAA alpha-to-coverage Main twin for GPU-scene masked work.
// Same per-work material varying and lit implementation as gfxmesh_lit,
// but output alpha controls hardware coverage, matching default_lit_a2c.
#define ALPHA_TO_COVERAGE 1
#define GFXMESH_FS_PASS 1
#include "lit_dependencies.hlsli"
#include "gfxmesh_lit_decl.hlsli"
#include "lit_hooks_default.hlsli"
#include "default_lit_body.hlsli"
