// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The canonical project import (see import_pipeline.h). Flow:
//   validate -> copy the source + deps into a STAGING dir under
//   .tourmaline/ -> derive (ImportMeshSource — the single glTF-aware
//   importer, with the request's settings) -> VALIDATE the staged family
//   (round-trip + the requested preprocessing actually happened) ->
//   PUBLISH into content/models/<stem>/ (remove the previous derived
//   family, move the staged one in; user-authored .texture files are
//   NEVER touched) -> update the registry. A failure at any point leaves
//   the previous live generation untouched.

#include <project_pipeline/import_pipeline.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <system_error>

#include <gltf_import/asset_export.h>

#include "registry_detail.h"

namespace smvk::project {

namespace {

bool IsModelSource(const std::filesystem::path& p) {
	std::string e = p.extension().string();
	for (char& c : e)
		c = char(std::tolower((unsigned char)c));
	return e == ".gltf" || e == ".glb";
}

// The derived-family file patterns (the publish step replaces exactly
// these; everything else in the destination — user-authored .texture
// settings, hand-placed files — survives a reimport).
bool IsDerivedFamilyFile(const std::string& name) {
	auto endsWith = [&](const char* suffix) {
		const size_t n = std::strlen(suffix);
		return name.size() >= n
			&& name.compare(name.size() - n, n, suffix) == 0;
	};
	return endsWith(".geometry") || endsWith(".gltf.meta")
		|| endsWith(".material") || endsWith(".anim");
}

bool CopyFileTo(const std::filesystem::path& from,
	const std::filesystem::path& to, std::string& err) {
	std::error_code ec;
	std::filesystem::create_directories(to.parent_path(), ec);
	std::filesystem::copy_file(from, to,
		std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		err = "copy failed '" + from.generic_string() + "' -> '"
			+ to.generic_string() + "': " + ec.message();
		return false;
	}
	return true;
}

} // namespace

bool ImportProjectAsset(const ImportRequest& request,
	ImportResult& out, std::string& err) {
	err.clear();
	out = ImportResult{};
	if (!request.project.Valid()) {
		err = "invalid project (open/init first)";
		return false;
	}
	if (request.source.empty() || !IsModelSource(request.source)) {
		err = "the source must be a .gltf/.glb: "
			+ request.source.generic_string();
		return false;
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path src = fs::absolute(request.source, ec);
	if (ec || !fs::is_regular_file(src, ec)) {
		err = "source not found: " + request.source.generic_string();
		return false;
	}
	const std::string stem = src.stem().string();
	const fs::path destDir =
		fs::path(request.project.ImportsDir()) / stem;
	const fs::path projectSource = destDir / src.filename();

	// ---- idempotence: unchanged content + unchanged settings = a no-op --
	// (The project copy is the import source of record; when it exists
	// and its family is current under THIS request's settings, there is
	// nothing to write. The external source may have changed — that only
	// matters when it differs from the project copy, checked below.)
	if (!request.force && fs::is_regular_file(projectSource, ec)) {
		if (!smvk::assets::MeshArtifactStale(projectSource,
				request.settings)) {
			out.projectSource = projectSource;
			out.geometry =
				fs::path(projectSource).replace_extension(".geometry");
			smvk::assets::AssetManifest m;
			std::string merr;
			if (smvk::assets::LoadAssetManifest(projectSource, m, merr))
				out.guid = m.guid;
			out.derived = false;
			return true; // current — nothing written
		}
	}

	// ---- stage: a full working copy under .tourmaline/stage/<stem>/ ----
	// (Deterministic derivation, so the staged family is byte-identical
	// to what a direct derivation beside the source would produce.)
	const fs::path stageRoot =
		fs::path(request.project.DerivedDataDir()) / "stage" / stem;
	std::filesystem::remove_all(stageRoot, ec);
	fs::create_directories(stageRoot, ec);
	const fs::path stageSource = stageRoot / src.filename();
	if (!CopyFileTo(src, stageSource, err))
		return false;
	// The dependency closure (the single glTF-aware enumeration — this
	// module parses through the importer's own tinygltf path).
	for (const std::string& dep :
			smvk::assets::GltfExternalDependencies(src)) {
		const fs::path depFrom = fs::path(dep).is_absolute()
			? fs::path(dep) : src.parent_path() / fs::path(dep);
		if (!fs::is_regular_file(depFrom, ec)) {
			err = "source dependency missing: '" + dep + "' (beside "
				+ src.generic_string() + ")";
			return false;
		}
		if (!CopyFileTo(depFrom, stageRoot / fs::path(dep), err))
			return false;
	}

	// ---- derive (the ONE importer; the request's settings) ---------------
	smvk::assets::AssetManifest manifest;
	smvk::assets::DeriveStats stats;
	if (!smvk::assets::ImportMeshSource(stageSource, "",
			request.settings, manifest, &stats, &err)) {
		err = "import failed: " + err;
		return false; // stage stays for diagnostics? no — cleaned below
	}

	// ---- validate the staged family BEFORE publishing ---------------------
	// (a) Round-trip: the written .geometry parses back.
	const fs::path stageGeometry =
		fs::path(stageSource).replace_extension(".geometry");
	smvk::assets::MeshAsset asset;
	std::string verr;
	if (!smvk::assets::LoadMeshAsset(stageGeometry, asset, verr)) {
		err = "validation failed — the staged .geometry does not parse: "
			+ verr;
		return false;
	}
	// (b) The requested preprocessing ACTUALLY happened: a requested
	// meshlet/LOD pass that produced nothing is an error, never a silent
	// success (the runtime would fall back to direct draws and the
	// renderer's mesh-shader path would silently vanish). Tiny meshes
	// legitimately produce no siblings (lodMinTriangles/minReduction) —
	// the LOD rule fires only when CHAINABLE geometry (enough triangles
	// to simplify) produced no chain at all.
	uint32_t blobsWithMeshlets = 0, lodLevels = 0, chainableBlobs = 0;
	for (const auto& g : asset.geometry) {
		if (!g.meshlets.empty())
			++blobsWithMeshlets;
		if (g.indices.size() / 3 > 100)
			++chainableBlobs;
	}
	for (const auto& chain : asset.lodChains)
		lodLevels += uint32_t(chain.size());
	if (request.settings.meshlets && !asset.geometry.empty()
			&& blobsWithMeshlets == 0) {
		err = "validation failed — meshlets were requested but no cooked"
			" set derived (the importer's meshlet pass produced nothing)";
		return false;
	}
	if (request.settings.lods && chainableBlobs > 0 && lodLevels == 0) {
		err = "validation failed — an LOD chain was requested but no"
			" sibling level derived despite chainable geometry (the"
			" simplifier produced nothing)";
		return false;
	}
	// (c) Manifest present + parseable beside the staged source.
	{
		smvk::assets::AssetManifest check;
		std::string cerr;
		if (!smvk::assets::LoadAssetManifest(stageSource, check, cerr)) {
			err = "validation failed — the staged manifest is missing or"
				" unparseable: " + cerr;
			return false;
		}
	}

	// ---- publish ------------------------------------------------------------
	// The derived family is REPLACED (a changed source may drop or rename
	// .material/.anim files — stale ones must not linger for cooks to
	// pick up); the source + dependency copies overwrite in place;
	// user-authored .texture files and anything unrecognized survive.
	fs::create_directories(destDir, ec);
	for (const fs::directory_entry& e :
			fs::directory_iterator(destDir, ec)) {
		if (ec || !e.is_regular_file(ec))
			continue;
		const std::string name = e.path().filename().generic_string();
		if (IsDerivedFamilyFile(name))
			fs::remove(e.path(), ec);
	}
	for (const fs::directory_entry& e :
			fs::directory_iterator(stageRoot, ec)) {
		if (ec || !e.is_regular_file(ec))
			continue;
		std::error_code moveEc;
		fs::rename(e.path(), destDir / e.path().filename(), moveEc);
		if (moveEc) {
			// Cross-device or locked: fall back to copy+remove (per-file;
			// the stage retains a complete copy until the loop ends).
			if (!CopyFileTo(e.path(), destDir / e.path().filename(), err))
				return false;
			fs::remove(e.path(), moveEc);
		}
	}
	std::filesystem::remove_all(stageRoot, ec);

	// ---- the registry (only after a successful publication) ---------------
	out.projectSource = projectSource;
	out.geometry = fs::path(projectSource).replace_extension(".geometry");
	out.guid = manifest.guid;
	out.geometryBlobs = stats.geometryBlobs;
	out.lodLevels = lodLevels;
	out.meshletSets = blobsWithMeshlets;
	out.derived = true;
	const std::string rel =
		request.project.RelativeTo(projectSource.generic_string());
	if (rel.empty()) {
		err = "internal: the published source is not project-relative: "
			+ projectSource.generic_string();
		return false;
	}
	if (!registry::UpdateRow(request.project, out, rel,
			src.generic_string(),
			smvk::assets::ImportFingerprintFor(request.settings))) {
		err = "import published but the registry write failed: "
			+ request.project.RegistryPath();
		return false;
	}
	return true;
}

} // namespace smvk::project
