// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE RESOLVED BUILD PLAN: the recipe resolver's output (see
// recipe_resolver.h) — everything the shared executor needs to run a
// build with ZERO further discovery. The plan is DATA: which paks, in
// which order, each owning exactly which assets, with which scene and
// which geometry dependencies. Execution (import/cook/bake/pack) lives
// in build_pipeline.h.
//
// DETERMINISM CONTRACT: the same recipe + the same project tree always
// produce an identical plan (sorted asset tables, stable topological
// pak order with authored order as the tiebreak) — the CLI and the
// editor resolving the same saved scene get the same plan, byte for
// byte (the EDT-01 equivalence precondition).

#include <cstdint>
#include <string>
#include <vector>

namespace smvk::project {

enum class PlanAssetKind : uint8_t {
	// A derived .geometry family. CLUSTER-INTERNAL in the pak: it must
	// ship in the pak of every scene that references it. A cross-pak
	// mesh reference is the v1 NON-GOAL — the runtime loads scene/mesh/
	// material blobs through ONE pak's PakReader; only Texture-kind
	// blobs are VFS-addressable across mounts (pak.h's interop
	// contract). The resolver REJECTS any plan that would need one.
	Geometry,
	// VFS-addressable cooked content (source images + cooked .ktx2):
	// shareable across paks — another pak's materials resolve it by
	// plain VFS path through the mounted pak backends.
	Texture,
	// VFS-addressable bake products (GI volume manifests + payloads,
	// cubemap captures): the same cross-pak contract as Texture.
	BakeFile,
};

struct PlanAsset {
	std::string path; // project-relative, forward slashes (the sort key)
	PlanAssetKind kind = PlanAssetKind::Texture;

	bool operator==(const PlanAsset& o) const {
		return path == o.path && kind == o.kind;
	}
};

struct PlanPak {
	std::string id;
	std::string output; // the authored output name (the executor places
	                    // it under the build/publish directory)
	std::string scene;  // project-relative tour; "" = asset-only pak
	bool transitive = false; // the recipe's 'dependencies transitive'
	std::vector<std::string> depends; // pak ids, authored order (the
	                                  // plan's order guarantees every
	                                  // one appears EARLIER in paks)
	// The scene's cooked geometry dependencies in TOUR order — index 0
	// is the ROOT artifact (the packer's primary VFS input; the same
	// contract BuildProject's refSource feeds Pak.exe). Empty for
	// asset-only paks. Nested .tour references are walked into this
	// list (they ship in the SAME pak — meshes are cluster-internal).
	std::vector<std::string> sceneArtifacts;
	// The pak's OWNED content, sorted by path: the sceneArtifacts (as
	// Geometry) plus every include-claimed asset. Exactly one pak owns
	// any given path (the resolver's core invariant).
	std::vector<PlanAsset> assets;
	size_t line = 0; // the recipe's [pak] line (diagnostics only)

	bool operator==(const PlanPak& o) const {
		return id == o.id && output == o.output && scene == o.scene
			&& transitive == o.transitive && depends == o.depends
			&& sceneArtifacts == o.sceneArtifacts && assets == o.assets;
	}
};

struct BuildPlan {
	std::string projectName;
	std::string entryScene; // project-relative .tour
	std::string entryPak;   // the pak owning the entry scene
	bool bakeGi = false;
	bool bakeCubemaps = false;
	// TOPOLOGICAL order: dependencies first, so mounting/packing in
	// plan order is always correct. Authored order is the deterministic
	// tiebreak (RES-03: same inputs -> same order, every time).
	std::vector<PlanPak> paks;

	[[nodiscard]] const PlanPak* FindPak(const std::string& id) const;

	bool operator==(const BuildPlan& o) const {
		return projectName == o.projectName
			&& entryScene == o.entryScene && entryPak == o.entryPak
			&& bakeGi == o.bakeGi && bakeCubemaps == o.bakeCubemaps
			&& paks == o.paks;
	}
};

} // namespace smvk::project
