// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The STANDARD no-op hook family (StandardLit): the engine's own entry
// wrappers (default_lit.frag.hlsl / default_lit_a2c.frag.hlsl /
// gfxmesh_lit.frag.hlsl) include this before default_lit_body.hlsli.
//
// A project lit family (Phase B of docs/shader_library_design.md)
// defines the same two function names/signatures from its own library
// and its entry wrappers include THAT instead — same body, same seam.

#ifndef TM_LIT_HOOKS_DEFAULT_HLSLI
#define TM_LIT_HOOKS_DEFAULT_HLSLI

#include "lit_hooks_api.hlsli"

void TM_ModifySurface(inout TMSurface surface, TMLitInput input)
{
    // StandardLit: the authored material is the whole truth.
    (void) surface;
    (void) input;
}

void TM_ModifyLitColor(inout float3 hdrColor, TMLitInput input)
{
    // StandardLit: the assembled lit color is the whole truth.
    (void) hdrColor;
    (void) input;
}

#endif // TM_LIT_HOOKS_DEFAULT_HLSLI
