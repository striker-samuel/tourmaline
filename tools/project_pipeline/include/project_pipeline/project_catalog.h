// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE GENERATED project.h (content as code): the resolved BuildPlan +
// the ASSET DATABASE's content-id registry (assets.db — ONE AND THE
// SAME store as the asset directory, the owner's call: no separate
// .tourids file, identity lives with identity) become a pure constexpr
// C++ catalog. Game code programs against stable IDs (ids belong to
// ASSETS — GUIDs — never paths: renames keep the id + the original
// symbol); the paks carry the same content; the CMake layer
// (cmake/TourmalineProject.cmake) regenerates the header only-on-change
// so an unchanged catalog never recompiles its includers.
//
// The identity ladder per entity (the GUID the registry keys on):
//   scene  — the .tour's .meta sidecar (minted + adopted on first
//            generation when absent; the sidecar travels with the file,
//            so a renamed scene keeps its identity)
//   mesh   — the family manifest's source GUID (the .gltf.meta that
//            doubles as the imported family's sidecar — SourceAssetGuid)
//   texture— the file's .meta sidecar (minted + adopted when absent)
//
// Deleted assets TOMBSTONE (the registry row keeps id+symbol reserved,
// active=0, absent from the catalog). New assets allocate the kind
// range's next free id — monotonic, never reused. The catalog reflects
// the LAST generation; regenerating another recipe re-activates its
// rows (ids are never lost across recipes in one project).

#include <string>
#include <vector>

#include <artifact_cache/assetdb.h>

#include "build_plan.h"
#include "project.h"

namespace smvk::project {

struct CatalogEntry {
	uint64_t id = 0;
	std::string symbol; // the C++ identifier (stable from first assign)
	std::string path;   // project-relative (the plan's identity — the
	                    // runtime pairs it with the pak + the manifest)
	std::string pak;    // the owning pak id
};

struct ProjectCatalog {
	std::string projectName;
	uint64_t projectId = 0; // stable from the project identity
	// The namespaces (the registry's kind ranges): scenes 0x1xxx,
	// textures 0x2xxx, meshes 0x3xxx, sounds 0x4xxx — sorted by id,
	// deterministic.
	std::vector<CatalogEntry> scenes;
	std::vector<CatalogEntry> textures;
	std::vector<CatalogEntry> meshes;
	std::vector<CatalogEntry> sounds;
	// (id, file) in plan (mount) order — paks are named entities, not
	// id carriers: renaming a package updates this table; asset ids
	// stay.
	std::vector<std::pair<std::string, std::string>> paks;
	uint64_t entryScene = 0; // the scenes:: namespace id (0 = none)
};

// Builds the catalog from the resolved plan against the registry:
// ensures content ids for every scene + every owned mesh/texture asset
// (BakeFiles are bake plumbing, not game-loadable content — no ids in
// v1), tombstones registry rows whose assets vanished from the plan,
// records the pak table. `contentDeps` collects every content file the
// generation touched (the CMake DEPFILE feed — rebuild the header when
// the content set changes).
bool GenerateProjectCatalog(const Project& project, const BuildPlan& plan,
	smvk::cache::AssetDatabase& db, ProjectCatalog& out,
	std::vector<std::string>& contentDeps, std::string& err);

// The header text (pure — the caller decides where it lands). Byte
// deterministic for the same catalog.
[[nodiscard]] std::string EmitProjectHeader(const ProjectCatalog& c);

// Write-only-on-change: reads the existing file and writes only when
// the text differs (an unchanged catalog never touches the mtime, so
// nothing that includes the header recompiles). True + wrote=false
// when the file already matched.
bool WriteProjectHeaderIfChanged(const std::string& path,
	const std::string& text, bool& wrote, std::string& err);

} // namespace smvk::project
