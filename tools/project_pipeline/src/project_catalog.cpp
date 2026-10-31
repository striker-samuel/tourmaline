// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The project.h generator (see project_catalog.h). Three passes:
// identity (every plan entity's GUID — sidecar read or mint+adopt),
// registry (EnsureContentId + the tombstone sweep), emission (the
// constexpr header text, byte-deterministic for the same catalog).

#include <project_pipeline/project_catalog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#include <Engine/Asset/tour.h>

namespace smvk::project {

namespace {

namespace fs = std::filesystem;

// FNV-1a 64 (the repo's stable hash shape).
uint64_t Fnv(const char* s) {
	uint64_t h = 1469598103934665603ULL;
	while (*s) {
		h ^= uint64_t(uint8_t(*s++));
		h *= 1099511628211ULL;
	}
	return h;
}

// The entity's GUID: the sidecar when one exists, else a FRESH mint
// written beside the file (the sidecar travels with it — renames keep
// the identity) + adopted into the asset directory.
bool GuidForAsset(const std::string& absPath, const std::string& kind,
	std::string& outGuid, std::string& err) {
	std::string metaErr;
	if (smvk::cache::ReadAssetMeta(absPath, outGuid, metaErr))
		return true;
	outGuid = smvk::tour::GenerateUuid();
	if (!smvk::cache::WriteAssetMeta(absPath, outGuid, "content-id",
			err))
		return false;
	return true;
}

} // namespace

bool GenerateProjectCatalog(const Project& project, const BuildPlan& plan,
	smvk::cache::AssetDatabase& db, ProjectCatalog& out,
	std::vector<std::string>& contentDeps, std::string& err) {
	err.clear();
	out = ProjectCatalog{};
	contentDeps.clear();
	if (!project.Valid()) {
		err = "invalid project (open/init first)";
		return false;
	}
	if (plan.paks.empty()) {
		err = "the plan is empty (resolve a recipe first)";
		return false;
	}
	out.projectName = plan.projectName.empty()
		? project.name : plan.projectName;
	out.projectId = 0xA1000000ULL | (Fnv(out.projectName.c_str()) & 0xFFFF);
	for (const PlanPak& pak : plan.paks)
		out.paks.emplace_back(pak.id, pak.output);

	// Every ensured (guid -> where it landed) for the tombstone sweep.
	std::set<std::string> liveGuids;
	const auto ensureAsset =
		[&](const std::string& projectRel, const std::string& kind,
			const std::string& pakId, const std::string& guidKindOverride,
			std::vector<CatalogEntry>& bucket) -> bool {
		const fs::path abs = fs::path(project.root) / fs::path(projectRel);
		std::string guid;
		if (!guidKindOverride.empty())
			guid = guidKindOverride;
		else if (!GuidForAsset(abs.generic_string(), kind, guid, err))
			return false;
		uint64_t id = 0;
		std::string symbol;
		if (!db.EnsureContentId(guid, kind, projectRel, id, symbol, err))
			return false;
		liveGuids.insert(guid);
		bucket.push_back(CatalogEntry{ id, symbol, projectRel, pakId });
		contentDeps.push_back(abs.lexically_normal().generic_string());
		return true;
	};

	// Scenes: every scene pak's tour (meshes' families below).
	std::map<std::string, std::string> meshGuidByPath;
	for (const PlanPak& pak : plan.paks) {
		if (pak.scene.empty())
			continue;
		if (!ensureAsset(pak.scene, "scene", pak.id, {}, out.scenes))
			return false;
		if (pak.id == plan.entryPak)
			out.entryScene = out.scenes.back().id;
		// The scene's geometry families: the SIBLING .gltf's manifest
		// GUID (SourceAssetGuid — the family's stable identity, the
		// importer's own sidecar doubles as it).
		for (const std::string& artifact : pak.sceneArtifacts) {
			const std::string ext =
				fs::path(artifact).extension().generic_string();
			std::string familySource;
			if (ext == ".gltf" || ext == ".glb")
				familySource = artifact;
			else {
				fs::path src = fs::path(artifact);
				for (const char* e : { ".gltf", ".glb" }) {
					src.replace_extension(e);
					if (fs::is_regular_file(
							fs::path(project.root) / src))
						break;
					src = fs::path(artifact);
				}
				familySource = src.generic_string();
			}
			std::string guid;
			std::string mErr;
			if (smvk::cache::ReadAssetMeta(
					(fs::path(project.root) / fs::path(familySource))
						.generic_string(),
					guid, mErr))
				meshGuidByPath[artifact] = guid;
			// (No manifest: the mesh pass below mints its own sidecar.)
		}
	}
	// Owned assets: meshes (geometry) + textures; BakeFiles are bake
	// plumbing (no game-loadable identity in v1 — documented).
	for (const PlanPak& pak : plan.paks) {
		for (const PlanAsset& asset : pak.assets) {
			if (asset.kind == PlanAssetKind::Geometry) {
				if (!ensureAsset(asset.path, "mesh", pak.id,
						meshGuidByPath.count(asset.path)
							? meshGuidByPath[asset.path] : std::string(),
						out.meshes))
					return false;
			}
			else if (asset.kind == PlanAssetKind::Texture) {
				if (!ensureAsset(asset.path, "texture", pak.id, {},
						out.textures))
					return false;
			}
		}
	}

	// The tombstone sweep: active registry rows whose guid the plan no
	// longer carries drop to active=0 (id+symbol reserved forever).
	{
		std::vector<smvk::cache::ContentIdRecord> rows;
		std::string lErr;
		if (!db.ListContentIds(rows, lErr)) {
			err = "registry list failed: " + lErr;
			return false;
		}
		for (const auto& row : rows) {
			if (!row.active || liveGuids.count(row.guid))
				continue;
			std::string sErr;
			if (!db.SetContentIdActive(row.guid, false, sErr)) {
				err = "tombstone failed: " + sErr;
				return false;
			}
		}
	}

	// Deterministic order: id-sorted buckets (EnsureContentId allocated
	// in plan order; the sort makes the emission stable against plan
	// re-orderings).
	const auto byId = [](const CatalogEntry& a, const CatalogEntry& b) {
		return a.id < b.id;
	};
	std::sort(out.scenes.begin(), out.scenes.end(), byId);
	std::sort(out.textures.begin(), out.textures.end(), byId);
	std::sort(out.meshes.begin(), out.meshes.end(), byId);
	return true;
}

std::string EmitProjectHeader(const ProjectCatalog& c) {
	std::ostringstream os;
	os << "// Generated by Tourmaline (Project.exe ids). Do not edit.\n"
		<< "// The content-as-code catalog: stable ids from the project's\n"
		<< "// asset database (assets.db - the one identity store); the\n"
		<< "// same content ships in the paks below. Regenerated by the\n"
		<< "// build only-on-change.\n"
		<< "#pragma once\n\n"
		<< "#include <array>\n#include <cstdint>\n"
			"#include <string_view>\n\n"
		<< "namespace project {\n\n"
		<< "using Id = std::uint64_t;\n\n"
		<< "inline constexpr Id ProjectId = 0x" << std::hex
		<< c.projectId << std::dec << "ULL;\n"
		<< "inline constexpr std::string_view Name = \""
		<< c.projectName << "\";\n\n";
	const auto emitNamespace = [&](const char* name,
			const std::vector<CatalogEntry>& entries) {
		os << "namespace " << name << " {\n";
		for (const CatalogEntry& e : entries)
			os << "    inline constexpr Id " << e.symbol << " = 0x"
				<< std::hex << e.id << std::dec << "ULL;\n";
		os << "}\n\n";
	};
	emitNamespace("scenes", c.scenes);
	emitNamespace("textures", c.textures);
	emitNamespace("meshes", c.meshes);
	os << "// Runtime packages (plan/mount order).\n"
		<< "struct Pak {\n"
		<< "    std::string_view name;\n"
		<< "    std::string_view file;\n"
		<< "};\n\n"
		<< "inline constexpr std::array Paks = {\n";
	for (const auto& [id, file] : c.paks)
		os << "    Pak{\"" << id << "\", \"" << file << "\"},\n";
	os << "};\n\n"
		<< "// The complete cooked content catalog (id -> path -> owner).\n"
		<< "struct Asset {\n"
		<< "    Id id;\n"
		<< "    std::string_view path;\n"
		<< "    std::string_view pak;\n"
		<< "};\n\n"
		<< "inline constexpr std::array Catalog = {\n";
	const auto emitRows = [&](const std::vector<CatalogEntry>& entries,
			const char* ns) {
		for (const CatalogEntry& e : entries)
			os << "    Asset{" << ns << "::" << e.symbol << ", \""
				<< e.path << "\", \"" << e.pak << "\"},\n";
	};
	emitRows(c.scenes, "scenes");
	emitRows(c.textures, "textures");
	emitRows(c.meshes, "meshes");
	os << "};\n\n";
	if (c.entryScene) {
		// The entry scene's symbol (the catalog's scenes bucket is
		// id-sorted; find it back).
		std::string entrySymbol;
		for (const CatalogEntry& e : c.scenes)
			if (e.id == c.entryScene) {
				entrySymbol = e.symbol;
				break;
			}
		os << "inline constexpr Id EntryScene = scenes::" << entrySymbol
			<< ";\n\n";
	}
	os << "// Compile-time content validation.\n"
		<< "constexpr bool Contains(Id id) {\n"
		<< "    for (const auto& asset : Catalog)\n"
		<< "        if (asset.id == id)\n"
		<< "            return true;\n"
		<< "    return false;\n"
		<< "}\n\n"
		<< "} // namespace project\n";
	return os.str();
}

bool WriteProjectHeaderIfChanged(const std::string& path,
	const std::string& text, bool& wrote, std::string& err) {
	wrote = false;
	{
		std::ifstream in(path, std::ios::binary);
		if (in) {
			std::string existing((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());
			if (existing == text)
				return true; // unchanged — the mtime never moves
		}
	}
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot write the project header: " + path;
		return false;
	}
	f << text;
	if (!f.good()) {
		err = "failed writing the project header: " + path;
		return false;
	}
	wrote = true;
	return true;
}

} // namespace smvk::project
