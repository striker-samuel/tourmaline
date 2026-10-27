// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE PROJECT BUILD ORCHESTRATOR (the spec's BuildProject): the thin
// pipeline layer that chains the canonical stages for one project scene —
//
//   open + validate project
//     -> resolve the scene tour (the authored .tour under content/scenes)
//     -> resolve its imported .geometry (the tour's root reference)
//     -> reimport missing/stale sources (ImportProjectAsset — the
//        canonical import; unchanged content is a no-op)
//     -> Cook (scene-dependent bakes: cubemap captures + the baked data
//        the pak embeds; the settings come from the tour's own
//        exportSettings node, not the script)
//     -> GiBaker (the world field + the authored GI volumes; per-unit
//        freshness skips live inside — unchanged units stand)
//     -> Pak (assembly + the bake-freshness gate + verify)
//
// The stages are the EXISTING executables driven as subprocesses with
// explicit exit-code propagation (each is a complete, user-runnable tool;
// the orchestrator adds sequencing + the project resolution, never a
// second implementation of any stage). Stage FLAGS come from the
// project's persisted build settings (.tourmaline/build-settings.txt) —
// the batch script never duplicates them.

#include <string>

#include "project.h"

namespace smvk::project {

struct BuildOptions {
	// The scene tour, project-relative (e.g. "content/scenes/Sponza.tour").
	// Empty = discover content/scenes/*.tour — exactly one must exist.
	std::string scene;
	// The output pak (absolute or project-relative).
	std::string outputPak;
};

struct BuildReport {
	std::string scene;      // the resolved tour (project-relative)
	std::string artifact;   // the tour's imported .geometry (content-relative)
	bool reimported = false;
	std::string pak;        // the written pak (absolute)
};

// One build. True + report on success; false + err (the failing stage is
// named) on any failure — a failed stage never reaches the next one, and
// a failed build never replaces the last valid pak.
bool BuildProject(const Project& project, const BuildOptions& options,
	BuildReport& out, std::string& err);

} // namespace smvk::project
