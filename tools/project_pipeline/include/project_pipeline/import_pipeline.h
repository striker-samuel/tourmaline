// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE PROJECT IMPORT PIPELINE: one canonical glTF -> project-asset flow
// (the spec's ImportGltf/ImportProjectAsset). Built ON the single
// existing importer — smvk::assets::ImportMeshSource (gltf_import, the
// only glTF-aware module in the tree); this layer adds the project
// ownership the spec requires:
//
//   - copy the external source + its dependencies INTO the project
//     (the original location kept as reimport metadata only — the
//     project relocates freely);
//   - derive with EXPLICIT settings (GeometryImportSettings — the LOD
//     ratios/error live with the import, recorded in the manifest's
//     import fingerprint);
//   - the derived .geometry is AUTHORITATIVE: LOD chains + meshlets are
//     an import product; cooks consume them and never re-decide;
//   - validate before publishing (the required preprocessing actually
//     happened — a requested-but-missing meshlet/LOD set is an error,
//     never a silent success);
//   - transactional publish: derive into a staging directory under
//     .tourmaline/, validate, THEN publish; a failed import leaves the
//     previous generation live;
//   - idempotent: re-importing unchanged content under unchanged
//     settings is a no-op (content digest + import fingerprint + the
//     registry row agree).
//
// The editor and any CLI wrap THIS layer (the same implementation —
// no second importer, no alternate derivation path).

#include <filesystem>
#include <string>
#include <vector>

#include <gltf_import/asset_export.h> // DeriveConfig: the settings surface

#include "project.h"

namespace smvk::project {

// The geometry import settings (the spec's GeometryImportSettings —
// expressed as the existing DeriveConfig, the ONE settings type; the
// editor's env gates and the CLI both fill this).
using GeometryImportSettings = smvk::assets::DeriveConfig;

struct ImportRequest {
	Project project;
	std::filesystem::path source;      // the external glTF/GLB
	GeometryImportSettings settings;
	bool force = false;                // re-derive even when current
};

struct ImportResult {
	// The project-owned source copy (absolute, inside ImportsDir()).
	std::filesystem::path projectSource;
	// The project-owned .geometry (the runtime/renderer-ready artifact).
	std::filesystem::path geometry;
	std::string guid;                  // the manifest's source GUID
	uint32_t geometryBlobs = 0;
	uint32_t lodLevels = 0;            // total sibling levels across blobs
	uint32_t meshletSets = 0;          // blobs carrying a cooked set
	bool derived = false;             // false = current, nothing written
};

// The canonical import. True on success (fresh derivation OR current
// skip); false + err on failure (the previous generation stays live).
bool ImportProjectAsset(const ImportRequest& request,
	ImportResult& out, std::string& err);

// ---- staleness report (the build --explain surface) ----------------------

struct ImportStatus {
	std::string source;      // project-relative glTF path
	std::string guid;
	std::string original;    // the original external path (metadata only)
	bool known = false;      // a registry row exists
	bool current = true;     // family + fingerprint agree
	std::string why;         // the decision explanation
	std::string fingerprint;
};

// One row per registry record, in file order. err set when the registry
// is unreadable (still returns what parsed).
std::vector<ImportStatus> ProjectImportStatuses(const Project& project,
	std::string* err = nullptr);

} // namespace smvk::project
