// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0
// (the 04_project_game sample content — see LICENSE.md)

// THE PROJECT LIT FAMILY — alpha-to-coverage entry: the family twin
// of the engine's default_lit_a2c (same ALPHA_TO_COVERAGE cutout
// contract, project hooks instead of the no-op family). Bound for
// Masked ProjectLit materials in 2x MSAA Main; a missing module
// keeps those materials on this family's discard variant (never the
// stock A2C — the effect never silently drops, design §9).

#define ALPHA_TO_COVERAGE 1
#include "lit_dependencies.hlsli"
#include "lit_hooks_project.hlsli"
#include "default_lit_body.hlsli"
