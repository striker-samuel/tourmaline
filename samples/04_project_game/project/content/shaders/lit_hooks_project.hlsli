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

#include "lit_hooks_api.hlsli"

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
