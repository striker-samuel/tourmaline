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

#include <gltf_import/asset_export.h>

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

// C++ string-literal escaping for the emitted header (the generator is
// a compiler front end: a path or pak name carrying a quote or a
// backslash must never break the emitted code).
std::string CppStr(const std::string& s) {
	std::string o;
	for (char c : s) {
		if (c == '\\' || c == '"')
			o += '\\';
		o += c;
	}
	return o;
}

// THE ONE identity-acquisition point (every catalog entity goes through
// here, in this order):
//   1. a VALID sidecar GUID wins — and the row is ADOPTED into the
//      asset directory (the `assets` table: GUID -> path/kind), so the
//      source-asset registry and the content-id registry NEVER
//      disagree about an entity (one database, both tables, one
//      acquisition path);
//   2. a MISSING sidecar ("no .meta") mints a fresh GUID + writes it
//      (the sidecar travels with the file — renames keep the identity)
//      + adopts;
//   3. a MALFORMED/UNREADABLE sidecar FAILS THE GENERATION WITHOUT
//      TOUCHING IT. Auto-re-minting would silently REPLACE a stable
//      identity (every content id keyed on the old guid orphaned) —
//      identity repair is a SEPARATE, EXPLICIT operation: the error
//      tells the author to delete or fix the sidecar by hand (a
//      deletion re-enters the missing path on the next run).
// `sidecarOut` (optional) receives the sidecar path (the depfile feed —
// identity inputs belong with the content deps).
bool AcquireAssetIdentity(smvk::cache::AssetDatabase& db,
	const std::string& absPath, const std::string& kind,
	std::string& outGuid, std::string& sidecarOut, std::string& note,
	std::string& err) {
	outGuid.clear();
	note.clear();
	sidecarOut = smvk::cache::MetaPathFor(absPath);
	std::string guid;
	std::string metaErr;
	if (smvk::cache::ReadAssetMeta(absPath, guid, metaErr)) {
		if (!db.AdoptFile(absPath, guid, kind, err))
			return false;
		outGuid = guid;
		return true;
	}
	if (metaErr != "no .meta") {
		// Malformed/unreadable — never auto-repair a stable identity.
		err = "the identity sidecar '" + sidecarOut + "' is unreadable"
			" (" + metaErr + ") — REFUSING to auto-replace it (every"
				" content id keyed on that guid would silently orphan)."
				" Identity repair is explicit: fix or delete the sidecar"
				" by hand (a deleted sidecar re-mints on the next run)";
		return false;
	}
	outGuid = smvk::tour::GenerateUuid();
	if (!smvk::cache::WriteAssetMeta(absPath, outGuid, "content-id",
			err))
		return false;
	if (!db.AdoptFile(absPath, outGuid, kind, err))
		return false;
	note = "sidecar was missing — identity MINTED at '"
		+ sidecarOut + "'";
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
			const std::string& pakId, const std::string& guidOverride,
			std::vector<CatalogEntry>& bucket) -> bool {
		const fs::path abs = fs::path(project.root) / fs::path(projectRel);
		std::string guid;
		if (!guidOverride.empty()) {
			// The identity was resolved elsewhere (the mesh's DERIVED
			// manifest GUID): ADOPT it into the asset directory so both
			// tables agree, then ensure the content id.
			guid = guidOverride;
			std::string aErr;
			if (!db.AdoptFile(abs.lexically_normal().generic_string(),
					guid, kind, aErr)) {
				err = "adopt failed for '" + projectRel + "': " + aErr;
				return false;
			}
		}
		else {
			std::string sidecar, note;
			if (!AcquireAssetIdentity(db,
					abs.lexically_normal().generic_string(), kind, guid,
					sidecar, note, err))
				return false;
			if (!note.empty())
				std::printf("[ids] %s\n", note.c_str());
			// Identity inputs belong with the content deps: a sidecar
			// GUID change regenerates (the depfile feed).
			contentDeps.push_back(sidecar);
		}
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
		// The scene's geometry families: the manifest's DERIVED
		// .geometry entry GUID is the MESH's canonical identity — the
		// SAME GUID the asset directory registers the derived blob
		// under, so the runtime content id, the source-asset registry
		// and the import manifest agree on ONE identity (the source
		// glTF's own GUID stays the SOURCE model's identity; keying the
		// mesh row on it was the identity split this fixes).
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
			smvk::assets::AssetManifest man;
			std::string mErr;
			if (smvk::assets::LoadAssetManifest(
					(fs::path(project.root) / fs::path(familySource))
						.lexically_normal().generic_string(),
					man, mErr)) {
				const std::string want =
					fs::path(artifact).filename().generic_string();
				std::string derived;
				for (const auto& e : man.entries)
					if (e.file == want) {
						derived = e.guid;
						break;
					}
				if (derived.empty())
					for (const auto& e : man.entries)
						if (e.kind == smvk::assets::AssetKind::Geometry) {
							derived = e.guid;
							break;
						}
				if (!derived.empty())
					meshGuidByPath[artifact] = derived;
				// The manifest IS an identity input — a dep.
				contentDeps.push_back(
					smvk::cache::MetaPathFor(
						(fs::path(project.root)
							/ fs::path(familySource))
							.lexically_normal().generic_string()));
			}
			// (No manifest: the mesh pass below mints its own sidecar
			// identity through AcquireAssetIdentity.)
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
		<< CppStr(c.projectName) << "\";\n\n";
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
		os << "    Pak{\"" << CppStr(id) << "\", \""
			<< CppStr(file) << "\"},\n";
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
				<< CppStr(e.path) << "\", \"" << CppStr(e.pak)
				<< "\"},\n";
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
