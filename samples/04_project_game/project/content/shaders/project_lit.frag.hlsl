// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0
// (the 04_project_game sample content — see LICENSE.md)

// THE PROJECT LIT FAMILY — classic alpha-test cutout entry
// (docs/shader_library_design.md Phase B). Same shared body as the
// engine's default_lit; the ONLY difference is the hook family: this
// wrapper includes the PROJECT's lit_hooks_project.hlsli (resolved
// from this directory, which rides the include path FIRST) instead of
// the engine's lit_hooks_default.hlsli. The compiled output
// (project_lit.frag.hlsl.spv + its variant twins) is a DISTINCT
// namespace the WorldRenderer loads as the ProjectLit family —
// materials opt in through World::SetMaterialShaderFamily.

#include "lit_dependencies.hlsli"
#include "lit_hooks_project.hlsli"
#include "default_lit_body.hlsli"
