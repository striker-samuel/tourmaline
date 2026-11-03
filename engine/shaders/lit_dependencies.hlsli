// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// THE LIT-ENTRY INCLUDE GLUE (docs/shader_library_design.md Phase B,
// plan item 6): the one-time shared-include list every lit fragment
// entry wrapper needs before its hook family + the body. Extracted so
// project-authored family wrappers (project_lit.frag.hlsl &
// gfxmesh_project_lit.frag.hlsl in the project's own shader tree) are
// THREE-LINE files instead of copies of the engine wrappers — the
// shared lit implementation stays INCLUDED ONCE, never duplicated.
//
// This header is include-order neutral: nothing here depends on the
// wrapper's defines (ALPHA_TO_COVERAGE / GFXMESH_FS_PASS land BEFORE
// it exactly like they did when the list was inline), and it declares
// nothing itself — it only forwards the body's environment.

#ifndef TM_LIT_DEPENDENCIES_HLSLI
#define TM_LIT_DEPENDENCIES_HLSLI

#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"
#include "sggi.hlsli"
#include "sg_lightmap.hlsli"
#include "sg_regular.hlsli"
#include "sg_gi_volume.hlsli"
// Public compile-time helpers for project fragment wrappers and hooks.
// All variant defines (GFXMESH_FS_PASS, MAT_STANDARD, SG_LM_LOBE_COUNT,
// ALPHA_TO_COVERAGE) must be set by the wrapper/build BEFORE this include.
#include "tm_project_shader_macros.hlsli"

#endif // TM_LIT_DEPENDENCIES_HLSLI
