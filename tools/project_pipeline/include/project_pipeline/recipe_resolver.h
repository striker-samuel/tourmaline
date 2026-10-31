// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE RECIPE RESOLVER: parsed recipe + opened project -> the fully
// resolved, deterministic BuildPlan. NO stage runs here (no import,
// cook, bake, pack — the executor consumes the plan); this is where
// the filesystem finally gets involved.
//
// Resolution contract (the spec's PAR/RES invariants):
//  * the entry scene loads as a tour, carries a root reference whose
//    artifact exists, and is OWNED by exactly one [pak]'s 'scene'
//    (RES-01 — the same validation BuildProject applies, extended to
//    every scene-owning pak)
//  * include globs expand deterministically (sorted, project-relative;
//    RES-03) and every cooked artifact gets EXACTLY ONE owner — a
//    second claim on an owned path is an ERROR, never a silent dedup
//    (RES-04)
//  * a scene's geometry dependencies are FORCED into the scene's pak
//    (meshes/materials are cluster-internal: the pak format's
//    PakReader boundary — only Texture-kind blobs are VFS-addressable
//    cross-pak). Any cross-pak geometry claim is rejected with the
//    boundary named, never bypassed
//  * 'depends' is a real dependency edge: unknown targets and cycles
//    are rejected (the parser already did; defensive re-checks keep
//    hand-built recipes honest) and the plan's pak order is the
//    topological sort of them (mount order = plan order)
//  * missing scenes/artifacts, empty glob matches and contentless
//    paks reject BEFORE any stage runs (RES-02)
//
// V1 BOUNDARY (deliberate, see the spec's non-goals): a tour doc does
// not record its glTF-bound textures (doc.textureAssets is the cookie
// pool only — glTF textures are reference-bound), so material ->
// texture cross-pak references are NOT visible here. They surface at
// pack time, where the material replay discovers them; the executor +
// packer enforce the declared-'depends' rule there (M3 packaging).
// The resolver establishes OWNERSHIP; the packer polices USE.

#include <string>

#include "build_plan.h"
#include "project.h"
#include "project_recipe.h"

namespace smvk::project {

bool ResolveRecipe(const Project& project, const ProjectRecipe& recipe,
	BuildPlan& out, std::string& err);

// Recipe include-glob matching: '**' matches any number of path
// segments (including zero), '*'/'?' stay within one segment. Both
// sides are project-relative, forward-slashed. Exported for tests.
[[nodiscard]] bool RecipeGlobMatch(const std::string& pattern,
	const std::string& path);

} // namespace smvk::project
