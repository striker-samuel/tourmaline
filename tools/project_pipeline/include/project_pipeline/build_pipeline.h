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

#include <memory>
#include <string>

#include "build_plan.h"
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

// ---- the shared build-plan executor (the recipe spec) ----------------
//
// ONE plan, TWO environments: the executor sequences the resolved
// BuildPlan; a STAGE ADAPTER decides how each stage physically runs.
// The CLI adapter shells out to the stage EXEs (the exact legacy
// BuildProject invocations); the editor adapter calls the linked-in
// cooktool::Run / gibaker::Run / paktool::Run (the same Run() the exes'
// own mains call). The adapter is the invocation mechanism ONLY — never
// a second cooking implementation.
//
// Stage flags come from the RECIPE (bake gi / bake cubemaps): the
// recipe build's authority. The legacy BuildProject keeps reading
// .tourmaline/build-settings.txt — that file stays the LEGACY path's
// home and never leaks into plan builds.

struct CookStageArgs {
	std::string contentDir; // the project root
	std::string tour;       // the scene tour (absolute)
	bool cubemap = true;
	bool lightmap = false;
	// The artifact-cache store root (the editor's registry; empty = the
	// CLI's derived convention or off). Acceleration only — the output
	// is byte-identical either way (the cache's own contract).
	std::string registryDir;
};

struct GiBakerStageArgs {
	std::string contentDir;
	std::string tour;
};

struct PakStageArgs {
	std::string contentDir;
	std::string inVfs;     // the root artifact (the primary identity)
	std::vector<std::string> extraArtifacts; // additional positional artifacts
	std::string tour;      // "" = an asset-only pak (no scene blob)
	std::string outPak;    // absolute destination
	bool verify = true;
	// The playable-unit convention (the editor's cooks pack one): packs
	// levels/<levelName>.level into the pak. Empty = no level blob.
	std::string levelName;
	// The artifact-cache store root (see CookStageArgs::registryDir).
	std::string registryDir;
	// The multi-pak ownership surface (the resolved plan's tables):
	// contentSet = the VFS paths an ASSET-ONLY pak embeds directly as
	// Texture blobs (no scene, no clusters — the shared-texture pak);
	// excludeVfs = VFS texture paths a DEPENDENCY pak owns (this pak's
	// serializer skips them; the materials keep their refs and resolve
	// cross-pak through the mounted backends at runtime).
	std::vector<std::string> contentSet;
	std::vector<std::string> excludeVfs;
};

struct StageAdapter {
	virtual ~StageAdapter() = default;
	// Each returns the stage's exit code (0 = success). err carries the
	// LAUNCH failure only (a missing exe, a dead fork) — the stage's own
	// diagnostics stream through the adapter's output channel (the
	// subprocess console; the editor's tool::SetLineSink feed).
	virtual int Cook(const CookStageArgs& args, std::string& err) = 0;
	virtual int GiBaker(const GiBakerStageArgs& args, std::string& err) = 0;
	virtual int Pak(const PakStageArgs& args, std::string& err) = 0;
};

// The CLI default: the stage exes as subprocesses (flag-for-flag the
// legacy BuildProject invocations — the parity contract).
[[nodiscard]] std::unique_ptr<StageAdapter> MakeSubprocessStageAdapter();

struct BuildPlanOptions {
	// Where the paks (and the appmanifest) publish. Absolute or
	// project-relative; empty = <root>/build.
	std::string publishDir;
	// The manifest's name (sponza.tourrecipe -> "sponza" publishes
	// sponza.tapp). Empty = the entry pak's stem.
	std::string manifestName;
	// The playable-unit convention: packs levels/<levelName>.level into
	// every SCENE pak (the editor sets this to the pak stem; the CLI
	// default is empty). EDT-01 equivalence holds when both
	// environments pass the same options.
	std::string levelName;
	// The artifact-cache store root (the editor's registry; empty = the
	// CLI convention — derived or off). Acceleration ONLY: the stage
	// output is byte-identical either way.
	std::string registryDir;
	// THE UNSAVED-SCENE BUILD (the editor's EDT-02 form): the plan
	// RESOLVES against the authored .tour on disk (the saved truth),
	// but every STAGE runs against this snapshot tour instead — the
	// authored document is never touched. Empty = run the authored doc.
	std::string sceneSnapshot;
};

struct BuildPlanReport {
	struct Pak {
		std::string id;
		std::string output; // absolute
		std::string scene;  // project-relative ("" = asset-only)
		bool reimported = false;
	};
	std::vector<Pak> paks; // plan order (topological — mount order)
	// The published appmanifest (.tapp; empty until M4's writer ran).
	// Written LAST: every pak verified + policed first, so a failed
	// build leaves the previous manifest + its paks intact.
	std::string manifest;
};

// Execute a resolved plan end to end:
//   import freshness (in-process, every family the plan packs)
//   -> per scene pak: Cook + GiBaker (bake gi gates the GiBaker stage)
//   -> per pak, PLAN ORDER: Pak (assembly + the bake-freshness gate +
//      verify)
// A failed stage aborts the plan (the failing stage is named) and never
// reaches the next one; the Pak stage writes only on success, so the
// previous build's outputs stand (a failed build never replaces the
// last valid anything).
bool ExecuteBuildPlan(const Project& project, const BuildPlan& plan,
	StageAdapter& adapter, const BuildPlanOptions& options,
	BuildPlanReport& out, std::string& err);

} // namespace smvk::project
