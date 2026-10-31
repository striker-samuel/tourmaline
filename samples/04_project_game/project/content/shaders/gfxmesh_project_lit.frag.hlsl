// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0
// (the 04_project_game sample content — see LICENSE.md)

// THE PROJECT LIT FAMILY — mesh-path entry: the family twin of the
// engine's gfxmesh_lit (material from the flat mesh varying, project
// hooks). The PSIn/interface comes from the engine's extracted
// gfxmesh_lit_decl.hlsli so the linked locations can never drift from
// the mesh shader's. A missing family mesh twin makes the range
// NOT mesh-eligible — the indexed leg draws it with the family's
// indexed program (the design §9 fallback; the effect is never lost).

#define GFXMESH_FS_PASS
#include "lit_dependencies.hlsli"
#include "gfxmesh_lit_decl.hlsli"
#include "lit_hooks_project.hlsli"
#include "default_lit_body.hlsli"
