// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0
// (the 04_project_game sample content — see LICENSE.md)

// THE PROJECT LIT HOOK FAMILY (docs/shader_library_design.md Phase B):
// the project's implementation of the two lit hooks. This file is the
// ONLY shader code the project authors — the entry wrappers
// (project_lit.frag.hlsl / project_lit_a2c.frag.hlsl /
// gfxmesh_project_lit.frag.hlsl beside this file) include it in place
// of the engine's lit_hooks_default.hlsli, and the shared lit body
// calls the functions below at the seam's two contract points:
//   TM_ModifySurface — after Forward+ decals, before F0/specular AA
//   TM_ModifyLitColor — after fog, before the HDR sanitize
//
// The userdata bit layout (the ENGINE never knows these names — the
// bits belong to gameplay code, packed through
// World::SetMaterialInstanceUserData):
//   0..7   = damage flash amount
//   8..15  = wetness amount
//   16..31 = available
//
// The hook input also exposes i.materialId, i.lightmapAtlasUv,
// i.lightmapLobes. The sample doesn't need to read those values, so it
// adds no per-pixel effect work or material permutations.

#include "lit_hooks_api.hlsli"
#include "tm_project_shader_macros.hlsli"

// These public compile-time switches are available to a project's hook
// library through lit_dependencies.hlsli. Prove that the wrapper's
// compile-time path agrees with the public macro without adding runtime
// branches or changing the wetness/damage examples.
#if defined(GFXMESH_FS_PASS) && !TM_LIT_MESH_FRAGMENT
#error "ProjectLit mesh fragment must advertise TM_LIT_MESH_FRAGMENT"
#endif
#if defined(MAT_STANDARD) && TM_LIT_LAYERING_SUPPORTED
#error "MAT_STANDARD variant must not advertise layering support"
#endif
#if defined(SG_LM_LOBE_COUNT) && TM_LM_COMPILED_LOBES == 0
#error "Fixed-lobe variant must advertise its lobe count"
#endif

#include "effects/damage.hlsli"
#include "effects/wetness.hlsli"

void TM_ModifySurface(inout TMSurface s, TMLitInput i)
{
    // Wetness rides the SURFACE hook: the roughness/baseColor edit
    // flows through the normal BRDF path (highlights, env response),
    // not a final-pixel tint.
    ApplyWetness(s, i);
}

void TM_ModifyLitColor(inout float3 color, TMLitInput i)
{
    // Damage flash rides the LIT-COLOR hook: surface-local artistic
    // tinting relative to the HDR lighting (outAoTerm/outEnvSpec and
    // alpha untouched by contract).
    ApplyDamageFlash(color, i);
}
