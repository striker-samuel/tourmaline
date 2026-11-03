// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Stable, PROJECT-FACING lit-fragment compile-time helpers.
//
// Include through lit_dependencies.hlsli (or explicitly from a project
// fragment wrapper). These macros describe the COMPILED fragment variant
// and already-published fragment inputs, not the GPU draw/cull backend.
// In particular, ordinary direct VS and GPU-filtered indexed VS share
// fragment programs: there is deliberately no TM_GPU_DRIVEN macro.
//
// TM_LIT_MATERIAL_ID(IN) is only valid inside the lit fragment entry
// (where both PSIn IN and the scene push pc are in scope). Project hook
// functions must use TMLitInput.materialId instead.
#if defined(GFXMESH_FS_PASS)
#define TM_LIT_MESH_FRAGMENT 1
#define TM_LIT_MATERIAL_ID(input) ((input).meshMaterial)
#else
#define TM_LIT_MESH_FRAGMENT 0
#define TM_LIT_MATERIAL_ID(input) (pc.materialId)
#endif

#if defined(ALPHA_TO_COVERAGE)
#define TM_LIT_ALPHA_TO_COVERAGE 1
#else
#define TM_LIT_ALPHA_TO_COVERAGE 0
#endif

// This is a compile-time capability: 0 means the MAT_STANDARD
// specialization removed the *layering machinery*. It does not report
// whether the current material happens to have extra layers.
#if defined(MAT_STANDARD)
#define TM_LIT_LAYERING_SUPPORTED 0
#else
#define TM_LIT_LAYERING_SUPPORTED 1
#endif

#define TM_LM_NONE 0xFFFFFFFFu

// A fixed SG variant's compile-time lobe count; 0 means the generic,
// runtime-count implementation, NOT that the draw has no lightmap.
#if defined(SG_LM_LOBE_COUNT)
#if SG_LM_LOBE_COUNT != 5 && SG_LM_LOBE_COUNT != 6 && SG_LM_LOBE_COUNT != 9 && SG_LM_LOBE_COUNT != 12
#error "SG_LM_LOBE_COUNT must be 5, 6, 9 or 12"
#endif
#define TM_LM_COMPILED_LOBES SG_LM_LOBE_COUNT
#else
#define TM_LM_COMPILED_LOBES 0
#endif

// Evaluate the expression only once, including when it is not literal.
bool TM_LmIsValidLobeCount(uint lobeCount)
{
    return lobeCount == 5u || lobeCount == 6u
        || lobeCount == 9u || lobeCount == 12u;
}
#define TM_LM_VALID_LOBE_COUNT(n) TM_LmIsValidLobeCount((n))
