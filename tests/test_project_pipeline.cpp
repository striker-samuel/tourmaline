// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// project_pipeline_tests: CPU-only regression tests for the canonical
// project import pipeline (tools/project_pipeline): the project model,
// the transactional copy-in + derive + validate + publish flow, the
// import fingerprint (settings/revision invalidation), idempotent
// reimport, source-change reimport, failure retention (the previous
// generation survives a failed import), project relocation, and the
// authoritative-preprocessing contract (LOD chains + meshlets are an
// import product).

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <project_pipeline/build_plan.h>
#include <project_pipeline/build_pipeline.h>
#include <project_pipeline/import_pipeline.h>
#include <project_pipeline/project.h>
#include <project_pipeline/project_catalog.h>
#include <project_pipeline/project_recipe.h>
#include <project_pipeline/recipe_resolver.h>

#include <artifact_cache/assetdb.h>
#include <artifact_cache/artifact_cache.h>

#include <gltf_import/asset_export.h>

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/tour.h>

#include "cook_tool.h"
#include "gi_baker.h"
#include "pak_tool.h"

// The in-process stage libs (CookLib/PakLib) reference the stb impls
// without DEFINING them (the CLI exes compile their own impl TUs; the
// editor provides them in editor_branding.cpp). This test drives the
// libs directly — it provides the impls the same way. (The engine's
// own stb copy is STB_*_STATIC, so no collision.)
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}


// ---- a chainable glTF source: an (N+1)^2 grid, EXTERNAL .bin buffer ----
// (2*N*N triangles — enough geometry for the LOD simplifier to keep a
// chain; the external buffer exercises the dependency-copy path.)
struct TestSource {
	fs::path gltf;
	fs::path bin;
};

// A minimal VALID 1x1 red RGBA PNG (93-byte spec shape with zeroed CRCs
// — stb_image decodes it and does not verify CRCs; GDI+ confirms the
// bytes decode). The fixture's only real texture: the multi-pak tests
// transcode it through the content-set path.
void WriteTinyPng(const fs::path& path) {
	static const unsigned char kPng[] = {
		0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
		0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
		0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
		0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54,
		0x78, 0x01, 0x01, 0x05, 0x00, 0xFA, 0xFF, 0x00,
		0xFF, 0x00, 0x00, 0xFF, 0x05, 0x00, 0x01, 0xFF,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
		0x00, 0x00, 0x00, 0x00
	};
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write(reinterpret_cast<const char*>(kPng), sizeof(kPng));
}

TestSource WriteGridGltf(const fs::path& dir, const std::string& stem,
	int n, uint32_t seed = 0, bool withMaterial = false,
	const std::vector<std::string>* namedMaterials = nullptr) {
	fs::create_directories(dir);
	// namedMaterials: an explicit, NAMED materials array (order under
	// the caller's control — the subasset-identity tests' reorder +
	// remap driver; plain PBR factors, no textures).
	const fs::path binPath = dir / (stem + ".bin");
	const int verts = (n + 1) * (n + 1);
	std::vector<float> pos, nrm;
	for (int y = 0; y <= n; ++y)
		for (int x = 0; x <= n; ++x) {
			// A gentle bump so the simplifier has real work (a perfectly
			// flat grid collapses under any error budget anyway).
			const float h = 0.25f * float((x * 31 + y * 17 + int(seed)) % 7);
			pos.push_back(float(x));
			pos.push_back(h);
			pos.push_back(float(y));
			nrm.push_back(0.0f);
			nrm.push_back(1.0f);
			nrm.push_back(0.0f);
		}
	std::vector<uint16_t> idx;
	for (int y = 0; y < n; ++y)
		for (int x = 0; x < n; ++x) {
			const int a = y * (n + 1) + x;
			idx.push_back(uint16_t(a));
			idx.push_back(uint16_t(a + 1));
			idx.push_back(uint16_t(a + n + 1));
			idx.push_back(uint16_t(a + 1));
			idx.push_back(uint16_t(a + n + 2));
			idx.push_back(uint16_t(a + n + 1));
		}
	std::ofstream bin(binPath, std::ios::binary | std::ios::trunc);
	bin.write(reinterpret_cast<const char*>(pos.data()),
		std::streamsize(pos.size() * sizeof(float)));
	bin.write(reinterpret_cast<const char*>(nrm.data()),
		std::streamsize(nrm.size() * sizeof(float)));
	bin.write(reinterpret_cast<const char*>(idx.data()),
		std::streamsize(idx.size() * sizeof(uint16_t)));
	bin.close();
	const size_t posEnd = pos.size() * sizeof(float);
	const size_t nrmEnd = posEnd + nrm.size() * sizeof(float);
	const size_t idxEnd = nrmEnd + idx.size() * sizeof(uint16_t);
	std::ostringstream j;
	std::string materialsJson;
	if (namedMaterials) {
		std::ostringstream mats;
		for (size_t i = 0; i < namedMaterials->size(); ++i) {
			if (i)
				mats << ",";
			mats << "{\"name\":\"" << (*namedMaterials)[i]
				<< "\",\"pbrMetallicRoughness\":{"
					"\"baseColorFactor\":[0.8,0.8,0.8,1],"
					"\"metallicFactor\":0,\"roughnessFactor\":1}}";
		}
		materialsJson = "\"materials\":[" + mats.str() + "],";
	}
	j << "{\"asset\":{\"version\":\"2.0\"},"
		"\"scene\":0,"
		"\"scenes\":[{\"nodes\":[0]}],"
		"\"nodes\":[{\"name\":\"Grid\",\"mesh\":0}],"
		"\"meshes\":[{\"primitives\":[{\"attributes\":"
		"{\"POSITION\":0,\"NORMAL\":1},\"indices\":2"
		<< (namedMaterials ? ",\"material\":0"
				: withMaterial ? ",\"material\":0" : "") << "}]}],"
		<< (namedMaterials ? materialsJson
			: withMaterial
				? "\"materials\":[{\"name\":\"Grid\","
					"\"pbrMetallicRoughness\":{"
					"\"baseColorFactor\":[0.8,0.8,0.8,1],"
					"\"metallicFactor\":0,\"roughnessFactor\":1},"
					"\"baseColorTexture\":{\"index\":0}}],"
					"\"textures\":[{\"sampler\":0,\"source\":0}],"
					"\"samplers\":[{}],"
					"\"images\":[{\"uri\":\"shared/stone.png\"}],"
				: "")
		<< "\"accessors\":["
		"{\"bufferView\":0,\"componentType\":5126,\"count\":" << verts
		<< ",\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[" << n
		<< ",1," << n << "]},"
		"{\"bufferView\":1,\"componentType\":5126,\"count\":" << verts
		<< ",\"type\":\"VEC3\",\"min\":[0,1,0],\"max\":[0,1,0]},"
		"{\"bufferView\":2,\"componentType\":5123,\"count\":"
		<< idx.size() << ",\"type\":\"SCALAR\",\"min\":[0],\"max\":["
		<< verts - 1 << "]}],"
		"\"bufferViews\":["
		"{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" << posEnd << "},"
		"{\"buffer\":0,\"byteOffset\":" << posEnd
		<< ",\"byteLength\":" << (nrmEnd - posEnd) << "},"
		"{\"buffer\":0,\"byteOffset\":" << nrmEnd
		<< ",\"byteLength\":" << (idxEnd - nrmEnd) << "}],"
		"\"buffers\":[{\"uri\":\"" << (stem + ".bin")
		<< "\",\"byteLength\":" << idxEnd << "}]}";
	const fs::path gltfPath = dir / (stem + ".gltf");
	if (withMaterial)
		WriteTinyPng(dir / "shared" / "stone.png"); // the material's image
	{
		std::ofstream f(gltfPath, std::ios::binary | std::ios::trunc);
		f << j.str();
	}
	return { gltfPath, binPath };
}

void TestProjectModel() {
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pp_model";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	// Open refuses a marker-less dir.
	{
		std::string err;
		smvk::project::Project p = smvk::project::Project::Open(
			tmp.generic_string(), err);
		Check(!p.Valid() && !err.empty(), "proj.open-requires-marker");
	}
	// Init creates the marker + the tree; twice = adoption.
	{
		bool created = false;
		std::string err;
		smvk::project::Project p = smvk::project::Project::Init(
			tmp.generic_string(), "ModelTest", created, err);
		Check(p.Valid() && created, "proj.init-creates");
		Check(err.empty(), "proj.init-clean");
		Check(smvk::cache::IsValidAssetGuid(p.guid),
			"proj.init-mints-identity");
		bool adopted = true;
		smvk::project::Project again = smvk::project::Project::Init(
			tmp.generic_string(), "Other", adopted, err);
		Check(again.Valid() && !adopted && again.name == "ModelTest",
			"proj.init-adopts-keeps-name");
		// Adoption NEVER re-mints: the identity is once-at-creation.
		Check(again.guid == p.guid, "proj.init-adopts-keeps-guid");
	}
	// Open now works; the paths + RelativeTo behave.
	{
		std::string err;
		smvk::project::Project p = smvk::project::Project::Open(
			tmp.generic_string(), err);
		Check(p.Valid() && p.name == "ModelTest", "proj.open-after-init");
		Check(p.RelativeTo(p.ImportsDir()) == "content/models",
			"proj.relative-to");
		Check(p.RelativeTo((fs::path(tmp) / "..").generic_string())
			.empty(), "proj.relative-to-rejects-escape");
	}
	// The persistent identity: a NAME change (or a dir move) never
	// moves the derived ProjectId — the id is GUID-keyed, not
	// name/path-keyed (the pre-identity behavior this replaces).
	{
		std::string err;
		const smvk::project::Project before = smvk::project::Project::Open(
			tmp.generic_string(), err);
		const uint64_t idBefore =
			smvk::project::DeriveProjectId(before.guid);
		{
			std::ofstream m(fs::path(tmp) / "project.tourproj",
				std::ios::binary | std::ios::trunc);
			m << "# renamed\nname=RenamedTest\nguid=" << before.guid
				<< "\nversion=2\n";
		}
		const smvk::project::Project after = smvk::project::Project::Open(
			tmp.generic_string(), err);
		Check(after.Valid() && after.name == "RenamedTest"
			&& after.guid == before.guid
			&& smvk::project::DeriveProjectId(after.guid) == idBefore,
			"proj.rename-keeps-guid-and-id");
		// A same-named DIFFERENT project is a different identity.
		const uint64_t otherId = smvk::project::DeriveProjectId(
			"0123456789abcdef0123456789abcdef");
		Check(otherId != idBefore, "proj.id-depends-on-guid-not-name");
	}
	// Current-format policy: a pre-identity marker (no guid row) is
	// INCOMPATIBLE — Open rejects with the actionable error; Init is
	// the one explicit mint-and-adopt path; Open works after.
	{
		const fs::path legacy = fs::temp_directory_path()
			/ "tourmaline_pp_model_legacy";
		fs::remove_all(legacy, ec);
		fs::create_directories(legacy, ec);
		{
			std::ofstream m(legacy / "project.tourproj",
				std::ios::binary | std::ios::trunc);
			m << "name=OldProject\nversion=1\n";
		}
		std::string err;
		smvk::project::Project p = smvk::project::Project::Open(
			legacy.generic_string(), err);
		Check(!p.Valid() && err.find("guid") != std::string::npos,
			"proj.open-rejects-identity-less-marker");
		bool created = true;
		smvk::project::Project adopted = smvk::project::Project::Init(
			legacy.generic_string(), "Whatever", created, err);
		Check(adopted.Valid() && !created && adopted.name == "OldProject"
			&& smvk::cache::IsValidAssetGuid(adopted.guid),
			"proj.init-adopts-and-mints-identity");
		smvk::project::Project ok = smvk::project::Project::Open(
			legacy.generic_string(), err);
		Check(ok.Valid() && ok.guid == adopted.guid,
			"proj.open-works-after-identity-mint");
	}
	// A MALFORMED guid row is rejected too (never auto-repaired).
	{
		const fs::path bad = fs::temp_directory_path()
			/ "tourmaline_pp_model_badguid";
		fs::remove_all(bad, ec);
		fs::create_directories(bad, ec);
		{
			std::ofstream m(bad / "project.tourproj",
				std::ios::binary | std::ios::trunc);
			m << "name=Bad\nguid=NOT-A-GUID\nversion=2\n";
		}
		std::string err;
		smvk::project::Project p = smvk::project::Project::Open(
			bad.generic_string(), err);
		Check(!p.Valid() && err.find("guid") != std::string::npos,
			"proj.open-rejects-malformed-guid");
	}
}

void TestImportLifecycle() {
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pp_import";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	const fs::path ext = tmp / "external";   // the ORIGINAL source home
	const fs::path projDir = tmp / "proj";   // the project
	fs::create_directories(ext, ec);
	{
		bool created = false;
		std::string err;
		smvk::project::Project p = smvk::project::Project::Init(
			projDir.generic_string(), "Sponza", created, err);
		Check(p.Valid() && created, "import.project-created");
	}
	const TestSource src = WriteGridGltf(ext, "grid", 12);

	smvk::project::Project project;
	{
		std::string err;
		project = smvk::project::Project::Open(
			projDir.generic_string(), err);
	}
	Check(project.Valid(), "import.project-open");

	// 1) The canonical import: copy-in + derive + validate + publish.
	smvk::project::ImportResult r1;
	{
		smvk::project::ImportRequest req;
		req.project = project;
		req.source = src.gltf;
		std::string err;
		Check(smvk::project::ImportProjectAsset(req, r1, err),
			"import.first-succeeds");
		if (!r1.derived && !err.empty())
			std::cout << "  (err: " << err << ")\n";
	}
	Check(r1.derived, "import.first-derived");
	Check(r1.geometryBlobs == 1, "import.one-blob");
	Check(r1.lodLevels > 0, "import.lod-chain-derived");
	Check(r1.meshletSets > 0, "import.meshlets-derived");
	// The project owns the family: the source copy + the .geometry +
	// the manifest + the registry row all live inside the project.
	const fs::path family =
		fs::path(project.ImportsDir()) / "grid";
	Check(fs::is_regular_file(family / "grid.gltf", ec)
		&& fs::is_regular_file(family / "grid.bin", ec),
		"import.source+deps-copied-in");
	Check(fs::is_regular_file(family / "grid.geometry", ec),
		"import.geometry-published");
	Check(fs::is_regular_file(fs::path(r1.geometry), ec),
		"import.geometry-path-resolves");
	// The manifest carries the import fingerprint.
	std::string familyGuid1, geometryGuid1, digest1;
	{
		smvk::assets::AssetManifest m;
		std::string merr;
		Check(smvk::assets::LoadAssetManifest(
				family / "grid.gltf", m, merr)
			&& !m.importFingerprint.empty()
			&& m.importFingerprint
				== smvk::assets::ImportFingerprintFor(
					smvk::project::GeometryImportSettings{}),
			"import.manifest-fingerprint-recorded");
		// Captured for the identity-ledger checks below (the re-derives
		// in 3 + 4 must CARRY these, not re-mint them).
		familyGuid1 = m.guid;
		digest1 = m.sourceDigest;
		for (const auto& e : m.entries)
			if (e.kind == smvk::assets::AssetKind::Geometry)
				geometryGuid1 = e.guid;
		Check(!familyGuid1.empty() && !geometryGuid1.empty()
			&& !digest1.empty(),
			"import.identity-captured");
	}

	// 2) Idempotence: unchanged source + unchanged settings = a no-op.
	{
		smvk::project::ImportRequest req;
		req.project = project;
		req.source = src.gltf; // the EXTERNAL source: unchanged
		smvk::project::ImportResult r2;
		std::string err;
		Check(smvk::project::ImportProjectAsset(req, r2, err)
			&& !r2.derived, "import.unchanged-skip");
	}
	// The status surface agrees.
	{
		std::string err;
		const std::vector<smvk::project::ImportStatus> rows =
			smvk::project::ProjectImportStatuses(project, &err);
		Check(rows.size() == 1 && rows[0].current
			&& rows[0].source == "content/models/grid/grid.gltf",
			"import.status-current");
	}

	// 3) Settings change re-derives (the fingerprint, not the clock): a
	// two-ratio chain under the same source bytes.
	{
		smvk::project::ImportRequest req;
		req.project = project;
		req.source = src.gltf;
		req.settings.lodRatios = { 0.5f, 0.25f };
		smvk::project::ImportResult r3;
		std::string err;
		Check(smvk::project::ImportProjectAsset(req, r3, err)
			&& r3.derived, "import.settings-change-rederives");
		// The published chain honors the new ratios: fewer levels.
		smvk::assets::MeshAsset asset;
		std::string aerr;
		if (smvk::assets::LoadMeshAsset(
				family / "grid.geometry", asset, aerr)) {
			uint32_t levels = 0;
			for (const auto& chain : asset.lodChains)
				levels += uint32_t(chain.size());
			Check(levels == 2, "import.custom-ratios-chain-length");
		}
	}

	// 4) Source change re-derives (content digest): a different bump seed
	// (seed 1: heights shift — a multiple of 7 would be a no-op mod 7).
	{
		WriteGridGltf(ext, "grid", 12, 1);
		smvk::project::ImportRequest req;
		req.project = project;
		req.source = ext / "grid.gltf";
		smvk::project::ImportResult r4;
		std::string err;
		Check(smvk::project::ImportProjectAsset(req, r4, err)
			&& r4.derived, "import.source-change-rederives");
		// The identity LEDGER carried across the changed content (the
		// staged derive resolved the PUBLISHED manifest up front): the
		// family + the geometry identity stand, only the digest moved.
		Check(r4.guid == familyGuid1, "import.source-change-keeps-family");
		smvk::assets::AssetManifest m4;
		std::string m4err;
		std::string geom4;
		if (smvk::assets::LoadAssetManifest(family / "grid.gltf", m4,
				m4err))
			for (const auto& e : m4.entries)
				if (e.kind == smvk::assets::AssetKind::Geometry)
					geom4 = e.guid;
		Check(geom4 == geometryGuid1 && m4.sourceDigest != digest1,
			"import.source-change-keeps-geometry");
	}

	// 5) FAILURE RETENTION: a corrupt source must NOT disturb the live
	// family. Corrupt the external copy, force-reimport, expect failure —
	// and the project's family still parses + the status stays current
	// against the project copy.
	{
		{
			std::ofstream f(ext / "grid.gltf",
				std::ios::binary | std::ios::trunc);
			f << "not a gltf";
		}
		smvk::project::ImportRequest req;
		req.project = project;
		req.source = ext / "grid.gltf";
		req.force = true;
		smvk::project::ImportResult r5;
		std::string err;
		Check(!smvk::project::ImportProjectAsset(req, r5, err)
			&& !err.empty(), "import.corrupt-source-fails");
		smvk::assets::MeshAsset asset;
		std::string aerr;
		Check(smvk::assets::LoadMeshAsset(
				family / "grid.geometry", asset, aerr),
			"import.failure-keeps-family");
		std::string serr;
		const std::vector<smvk::project::ImportStatus> rows =
			smvk::project::ProjectImportStatuses(project, &serr);
		Check(rows.size() == 1 && rows[0].current,
			"import.failure-registry-stands");
	}

	// 6) RELOCATION: the registry is project-relative — moving the whole
	// project root keeps the family current.
	{
		const fs::path moved = tmp / "proj-moved";
		fs::remove_all(moved, ec);
		fs::rename(projDir, moved, ec);
		if (ec) {
			Check(false, "import.relocation-rename");
		}
		else {
			std::string err;
			smvk::project::Project movedP =
				smvk::project::Project::Open(moved.generic_string(), err);
			const std::vector<smvk::project::ImportStatus> rows =
				smvk::project::ProjectImportStatuses(movedP, &err);
			Check(movedP.Valid() && rows.size() == 1 && rows[0].current,
				"import.relocation-stays-current");
			// A re-import after relocation is still a no-op.
			smvk::project::ImportRequest req;
			req.project = movedP;
			req.source = moved / "content" / "models" / "grid" / "grid.gltf";
			smvk::project::ImportResult r6;
			std::string ierr;
			Check(smvk::project::ImportProjectAsset(req, r6, ierr)
				&& !r6.derived, "import.relocation-reimport-noop");
		}
	}
}

void TestFingerprint() {
	smvk::project::GeometryImportSettings a{};
	smvk::project::GeometryImportSettings b{};
	Check(smvk::assets::ImportFingerprintFor(a)
			== smvk::assets::ImportFingerprintFor(b),
		"fingerprint.deterministic");
	b.lodRatios = { 0.5f, 0.25f };
	Check(smvk::assets::ImportFingerprintFor(a)
			!= smvk::assets::ImportFingerprintFor(b),
		"fingerprint.settings-sensitive");
	b = a;
	b.lods = false;
	Check(smvk::assets::ImportFingerprintFor(a)
			!= smvk::assets::ImportFingerprintFor(b),
		"fingerprint.gate-sensitive");
	// MeshArtifactStale's settings half: a current artifact under A is
	// stale under B (the recorded fingerprint disagrees).
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pp_fp";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	const TestSource src = WriteGridGltf(tmp, "fp", 12);
	smvk::assets::AssetManifest man;
	std::string err;
	Check(smvk::assets::ImportMeshSource(src.gltf, "", nullptr, a, man,
			nullptr, &err), "fingerprint.derive-ok");
	Check(!smvk::assets::MeshArtifactStale(src.gltf, a),
		"fingerprint.same-settings-current");
	Check(smvk::assets::MeshArtifactStale(src.gltf, b),
		"fingerprint.changed-settings-stale");
}

// The SUBASSET IDENTITY contract (the identity ledger): logical
// identity (the name key) is separate from the source-byte
// fingerprints; reimports carry identities across content edits and
// DCC reorders; ambiguous remaps FAIL rather than silently
// re-assigning; a corrupt ledger is a loud refusal, never a re-mint.
void TestSubassetIdentity() {
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pp_identity";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	const fs::path ext = tmp / "external";
	fs::create_directories(ext, ec);
	const fs::path gltf = ext / "knight.gltf";
	const smvk::assets::DeriveConfig cfg{};
	// Named-material sets (addressable locals — the helper takes a
	// pointer by design: identity tests control array ORDER).
	const std::vector<std::string> kGoldSteel{ "Gold", "Steel" };
	const std::vector<std::string> kSteelGold{ "Steel", "Gold" };
	const std::vector<std::string> kGoldOnly{ "Gold" };
	const std::vector<std::string> kGoldGold{ "Gold", "Gold" };
	const std::vector<std::string> kGoldSilver{ "Gold", "Silver" };
	auto guidOf = [](const smvk::assets::AssetManifest& m,
			smvk::assets::AssetKind k, const std::string& name) {
		for (const auto& e : m.entries)
			if (e.kind == k && e.name == name)
				return e.guid;
		return std::string();
	};
	auto derive = [&](smvk::assets::AssetManifest& m,
			smvk::assets::DeriveStats& s, std::string& err) {
		return smvk::assets::ImportMeshSource(gltf, "", nullptr, cfg,
			m, &s, &err);
	};

	// First import: two NAMED materials (the from-zero mint).
	WriteGridGltf(ext, "knight", 8, 0, false,
		&kGoldSteel);
	smvk::assets::AssetManifest m1;
	smvk::assets::DeriveStats s1;
	std::string err1;
	Check(derive(m1, s1, err1), "identity.first-derives");
	const std::string family1 = m1.guid;
	const std::string gold1 = guidOf(m1, smvk::assets::AssetKind::Material,
		"Gold");
	const std::string steel1 = guidOf(m1, smvk::assets::AssetKind::Material,
		"Steel");
	const std::string geom1 = guidOf(m1, smvk::assets::AssetKind::Geometry,
		"knight");
	Check(!family1.empty() && !gold1.empty() && !steel1.empty()
		&& !geom1.empty() && gold1 != steel1,
		"identity.first-rows");
	Check(s1.newGuids == 3 && s1.keptGuids == 0,
		"identity.first-mints"); // 2 materials + the geometry

	// REORDER (the DCC re-export shuffles the arrays): the ledger
	// (.gltf.meta beside the source) carries EVERY identity by name.
	WriteGridGltf(ext, "knight", 8, 0, false,
		&kSteelGold);
	{
		smvk::assets::AssetManifest m;
		smvk::assets::DeriveStats s;
		std::string derr;
		Check(derive(m, s, derr), "identity.reorder-derives");
		Check(m.guid == family1, "identity.reorder-keeps-family");
		Check(guidOf(m, smvk::assets::AssetKind::Material, "Gold")
				== gold1
			&& guidOf(m, smvk::assets::AssetKind::Material, "Steel")
				== steel1,
			"identity.reorder-keeps-subassets");
		Check(guidOf(m, smvk::assets::AssetKind::Geometry, "knight")
				== geom1,
			"identity.reorder-keeps-geometry");
		Check(s.keptGuids == 3 && s.newGuids == 0,
			"identity.reorder-all-carried");
	}

	// CONTENT EDIT (the heights shift): identity carries, the DIGEST
	// (the staleness fingerprint) moves — logical identity separate
	// from source-byte fingerprints.
	WriteGridGltf(ext, "knight", 8, 3, false,
		&kGoldSteel);
	{
		smvk::assets::AssetManifest m;
		smvk::assets::DeriveStats s;
		std::string derr;
		Check(derive(m, s, derr), "identity.edit-derives");
		Check(m.guid == family1
			&& guidOf(m, smvk::assets::AssetKind::Material, "Gold")
				== gold1
			&& guidOf(m, smvk::assets::AssetKind::Material, "Steel")
				== steel1
			&& guidOf(m, smvk::assets::AssetKind::Geometry, "knight")
				== geom1,
			"identity.edit-carries-all");
		Check(m.sourceDigest != m1.sourceDigest,
			"identity.edit-moves-digest");
	}

	// A REMOVED name retires (its GUID drops from the manifest, never
	// re-assigns); the survivor carries.
	WriteGridGltf(ext, "knight", 8, 3, false,
		&kGoldOnly);
	{
		smvk::assets::AssetManifest m;
		smvk::assets::DeriveStats s;
		std::string derr;
		Check(derive(m, s, derr), "identity.removed-derives");
		Check(m.guid == family1
			&& guidOf(m, smvk::assets::AssetKind::Material, "Gold")
				== gold1
			&& guidOf(m, smvk::assets::AssetKind::Material, "Steel")
				.empty(),
			"identity.removed-retires");
	}

	// AMBIGUOUS remap (a duplicate of a RECORDED name with a changed
	// count): a loud failure — identity is never silently re-assigned.
	WriteGridGltf(ext, "knight", 8, 3, false,
		&kGoldGold);
	{
		smvk::assets::AssetManifest m;
		smvk::assets::DeriveStats s;
		std::string derr;
		Check(!derive(m, s, derr)
			&& derr.find("ambiguous") != std::string::npos,
			"identity.ambiguous-fails");
	}

	// A NEW name beside a carried one: carried + freshly minted.
	WriteGridGltf(ext, "knight", 8, 3, false,
		&kGoldSilver);
	{
		smvk::assets::AssetManifest m;
		smvk::assets::DeriveStats s;
		std::string derr;
		Check(derive(m, s, derr), "identity.new-name-derives");
		Check(guidOf(m, smvk::assets::AssetKind::Material, "Gold")
				== gold1
			&& !guidOf(m, smvk::assets::AssetKind::Material, "Silver")
				.empty()
			&& guidOf(m, smvk::assets::AssetKind::Material, "Silver")
				!= gold1,
			"identity.new-name-mints");
		Check(s.keptGuids == 2 && s.newGuids == 1,
			"identity.new-name-counts"); // Gold + geometry carried
	}

	// A CORRUPT ledger refuses the derive — both shapes: unparseable
	// text, and parseable text carrying a MALFORMED guid row. Never a
	// silent identity re-mint.
	{
		const std::string meta = std::string(gltf.string()) + ".meta";
		{
			std::ofstream f(meta, std::ios::binary | std::ios::trunc);
			f << "source=whatever\nfingerprint=x\n"; // no guid row
		}
		smvk::assets::AssetManifest m;
		smvk::assets::DeriveStats s;
		std::string derr;
		Check(!derive(m, s, derr)
			&& derr.find("REFUSING") != std::string::npos,
			"identity.corrupt-ledger-refuses");
		{
			std::ofstream f(meta, std::ios::binary | std::ios::trunc);
			f << "guid=garbage!!! not a 32-hex row\n"; // malformed guid
		}
		std::string derr2;
		Check(!derive(m, s, derr2)
			&& derr2.find("REFUSING") != std::string::npos,
			"identity.malformed-guid-refuses");
		fs::remove(meta, ec);
	}
	// (Ledger deleted by hand: the next derive mints from zero — the
	// explicit identity-repair path.)
	{
		smvk::assets::AssetManifest m;
		smvk::assets::DeriveStats s;
		std::string derr;
		Check(derive(m, s, derr) && m.guid != family1,
			"identity.deleted-ledger-remints");
	}
}

// ---- the recipe layer (the .tourrecipe spec's M1: PAR + RES) ---------

// The spec's canonical valid recipe (three packages), as a string so
// every case below is self-contained.
const char* kValidRecipe =
	"# Tourmaline Project Recipe v1\n"
	"\n"
	"recipe 1\n"
	"project Sponza\n"
	"entry content/scenes/Sponza.tour\n"
	"\n"
	"bake gi\n"
	"bake cubemaps\n"
	"\n"
	"[pak shared]\n"
	"output shared.pak\n"
	"include content/models/shared/**\n"
	"\n"
	"[pak world]\n"
	"output world.pak\n"
	"scene content/scenes/Sponza.tour\n"
	"depends shared\n"
	"dependencies transitive\n"
	"\n"
	"[pak props]\n"
	"output props.pak\n"
	"include content/models/props/**\n"
	"depends shared\n";

// PAR-01: the valid three-package recipe parses, with every field
// landing in the typed structures exactly as authored.
void TestRecipeParsing() {
	smvk::project::ProjectRecipe r;
	smvk::project::RecipeDiagnostic diag;
	Check(smvk::project::ParseProjectRecipeText(kValidRecipe, r, diag)
			&& diag.message.empty(),
		"recipe.parse.valid-three-pak");
	if (!r.version) {
		std::cout << "  (diag: " << diag.Format("(text)") << ")\n";
		return;
	}
	Check(r.version == 1 && r.projectName == "Sponza"
			&& r.entry == "content/scenes/Sponza.tour",
		"recipe.parse.header-fields");
	Check(r.bakeGi && r.bakeCubemaps, "recipe.parse.bake-flags");
	Check(r.paks.size() == 3 && r.paks[0].id == "shared"
			&& r.paks[1].id == "world" && r.paks[2].id == "props",
		"recipe.parse.pak-ids-authored-order");
	Check(r.paks[0].output == "shared.pak"
			&& r.paks[0].includes.size() == 1
			&& r.paks[0].includes[0] == "content/models/shared/**"
			&& r.paks[0].scene.empty() && r.paks[0].depends.empty()
			&& !r.paks[0].dependenciesTransitive,
		"recipe.parse.shared-pak");
	Check(r.paks[1].scene == "content/scenes/Sponza.tour"
			&& r.paks[1].depends.size() == 1
			&& r.paks[1].depends[0] == "shared"
			&& r.paks[1].dependenciesTransitive,
		"recipe.parse.world-pak");
	Check(r.paks[2].depends[0] == "shared"
			&& r.paks[2].includes[0] == "content/models/props/**",
		"recipe.parse.props-pak");
	Check(r.FindPak("world") == &r.paks[1]
			&& r.FindPak("nope") == nullptr,
		"recipe.parse.find-pak");

	// Line numbers are 1-based physical lines (the [pak world] header
	// is line 14 in the string above).
	Check(r.paks[1].line == 14, "recipe.parse.line-numbers");

	// Robustness positives: CRLF, a UTF-8 BOM, quoted paths with
	// spaces + '#', mid-line comments, backslash normalization,
	// explicit bake switches, and a Windows-style path.
	{
		const std::string text =
			"\xEF\xBB\xBFrecipe 1\r\n"
			"project Quoted # the display name\r\n"
			"entry \"content/scenes/My #Scene.tour\"\r\n"
			"bake gi off\r\n"
			"bake cubemaps off\r\n"
			"[pak a]\r\n"
			"output a.pak\r\n"
			"scene \"content/scenes/My #Scene.tour\"\r\n"
			"include content\\models\\a\\**\r\n"
			"[pak b]\r\n"
			"output b.pak\r\n"
			"include content/models/b/**\r\n"
			"depends a\r\n"
			"dependencies none\r\n";
		smvk::project::ProjectRecipe q;
		smvk::project::RecipeDiagnostic d;
		Check(smvk::project::ParseProjectRecipeText(text, q, d),
			"recipe.parse.crlf-bom-quoted-comments");
		Check(q.projectName == "Quoted"
				&& q.entry == "content/scenes/My #Scene.tour"
				&& !q.bakeGi && !q.bakeCubemaps
				&& q.paks[0].includes[0] == "content/models/a/**",
			"recipe.parse.quoted-values-normalized");
	}

	// The file entry point agrees with the text entry point.
	{
		const fs::path tmp = fs::temp_directory_path()
			/ "tourmaline_pp_recipe";
		std::error_code ec;
		fs::create_directories(tmp, ec);
		const fs::path file = tmp / "valid.tourrecipe";
		{
			std::ofstream f(file, std::ios::binary | std::ios::trunc);
			f << kValidRecipe;
		}
		smvk::project::ProjectRecipe a, b;
		smvk::project::RecipeDiagnostic da, db;
		Check(smvk::project::ParseProjectRecipe(file.generic_string(), a,
				da)
			&& smvk::project::ParseProjectRecipeText(kValidRecipe, b, db)
			&& a.paks.size() == b.paks.size()
			&& a.entry == b.entry,
			"recipe.parse.file-matches-text");
		smvk::project::RecipeDiagnostic dmiss;
		smvk::project::ProjectRecipe rmiss;
		Check(!smvk::project::ParseProjectRecipe(
				(tmp / "missing.tourrecipe").generic_string(), rmiss,
				dmiss)
			&& dmiss.message.find("cannot open") != std::string::npos,
			"recipe.parse.missing-file");
	}

	// PAR-02: every malformed recipe fails with a line-specific
	// diagnostic (line 0 = whole-file complaints: missing directives).
	struct Bad {
		const char* name;
		std::string text;
		size_t expectLine; // 0 = the error must be line-0; 1 = any >0
	};
	const std::string head = "recipe 1\nentry c/s/a.tour\n";
	const Bad bad[] = {
		{ "empty", "", 0 },
		{ "version-not-first", "project X\nrecipe 1\n", 1 },
		{ "unsupported-version", "recipe 2\n" + head, 1 },
		{ "version-not-number", "recipe one\n" + head, 1 },
		{ "missing-entry", "recipe 1\n[pak a]\noutput a.pak\n"
			"include c/m/a/**\n", 0 },
		{ "entry-not-tour", "recipe 1\nentry c/s/a.scene\n"
			"[pak a]\noutput a.pak\nscene c/s/a.scene\n", 2 },
		{ "entry-not-owned", "recipe 1\nentry c/s/a.tour\n"
			"[pak a]\noutput a.pak\ninclude c/m/a/**\n", 0 },
		{ "duplicate-entry", "recipe 1\nentry c/s/a.tour\n"
			"entry c/s/b.tour\n", 3 },
		{ "unknown-directive", head + "frobnicate on\n", 3 },
		{ "unknown-section", head + "[widgets foo]\noutput w.pak\n", 3 },
		{ "malformed-section", head + "[pak a\noutput a.pak\n", 3 },
		{ "duplicate-pak-id", head + "[pak a]\noutput a.pak\n"
			"scene c/s/a.tour\n[pak a]\noutput b.pak\n"
			"include c/m/**\n", 6 },
		{ "pak-missing-output", head + "[pak a]\nscene c/s/a.tour\n", 3 },
		{ "pak-no-content", head + "[pak a]\noutput a.pak\n", 3 },
		{ "duplicate-output", head + "[pak a]\noutput a.pak\n"
			"scene c/s/a.tour\n[pak b]\noutput a.pak\n"
			"include c/m/**\n", 6 },
		{ "duplicate-scene-owner", head + "[pak a]\noutput a.pak\n"
			"scene c/s/a.tour\n[pak b]\noutput b.pak\n"
			"scene c/s/a.tour\n", 6 },
		{ "absolute-path", "recipe 1\nentry F:/abs/scene.tour\n", 2 },
		{ "rooted-path", "recipe 1\nentry /abs/scene.tour\n", 2 },
		{ "dotdot-escape", "recipe 1\nentry ../outside.tour\n", 2 },
		{ "empty-segment", "recipe 1\nentry c//s/a.tour\n", 2 },
		{ "wildcard-in-entry", "recipe 1\nentry c/s/*.tour\n", 2 },
		{ "unterminated-quote", "recipe 1\nentry \"c/s/a.tour\n", 2 },
		{ "junk-after-quote", "recipe 1\nentry \"c/s/a.tour\" x\n", 2 },
		{ "unknown-depends", head + "[pak a]\noutput a.pak\n"
			"scene c/s/a.tour\ndepends ghost\n", 6 },
		{ "self-depends", head + "[pak a]\noutput a.pak\n"
			"scene c/s/a.tour\ndepends a\n", 6 },
		{ "duplicate-depends", head + "[pak a]\noutput a.pak\n"
			"include c/m/a/**\n[pak b]\noutput b.pak\n"
			"include c/m/b/**\ndepends a\ndepends a\n", 10 },
		{ "dependency-cycle", head + "[pak a]\noutput a.pak\n"
			"scene c/s/a.tour\ndepends b\n[pak b]\noutput b.pak\n"
			"include c/m/b/**\ndepends a\n", 0 },
		{ "unknown-bake", "recipe 1\nbake lightmaps\n", 2 },
		{ "bad-bake-switch", "recipe 1\nbake gi maybe\n", 2 },
		{ "bad-dependencies-value", head + "[pak a]\noutput a.pak\n"
			"include c/m/a/**\ndependencies maybe\n", 6 },
		{ "header-directive-in-section", head + "[pak a]\noutput a.pak\n"
			"include c/m/a/**\nentry c/s/b.tour\n", 6 },
		{ "pak-directive-in-header", "recipe 1\noutput a.pak\n", 2 },
	};
	for (const Bad& b : bad) {
		smvk::project::ProjectRecipe r;
		smvk::project::RecipeDiagnostic d;
		const bool ok = smvk::project::ParseProjectRecipeText(b.text, r, d);
		const bool lineOk = b.expectLine == 0 ? d.line == 0 : d.line >= 1;
		Check(!ok && !d.message.empty() && !d.directive.empty()
				&& r.version == 0 && r.paks.empty() && lineOk,
			(std::string("recipe.parse.reject.") + b.name).c_str());
	}
}

// Glob matcher unit checks ('**' crosses directories; '*'/'?' do not).
void TestRecipeGlob() {
	using smvk::project::RecipeGlobMatch;
	Check(RecipeGlobMatch("content/a/**", "content/a/x.png"),
		"recipe.glob.double-star-direct-child");
	Check(RecipeGlobMatch("content/a/**", "content/a/b/c/x.png"),
		"recipe.glob.double-star-deep");
	Check(RecipeGlobMatch("content/**/*.png", "content/x/y.png"),
		"recipe.glob.double-star-mid-pattern");
	Check(!RecipeGlobMatch("content/a/*", "content/a/b/x.png"),
		"recipe.glob.single-star-stays-in-segment");
	Check(RecipeGlobMatch("content/a/*.png", "content/a/x.png"),
		"recipe.glob.single-star-suffix");
	Check(RecipeGlobMatch("content/m?del/**", "content/model/t.png"),
		"recipe.glob.question-mark");
	Check(RecipeGlobMatch("content/a/x.png", "content/a/x.png")
			&& !RecipeGlobMatch("content/a/x.png", "content/a/y.png"),
		"recipe.glob.exact-path");
	Check(!RecipeGlobMatch("content/a/**", "content/b/a/x.png"),
		"recipe.glob.no-prefix-bleed");
}

// A synthetic recipe project: two imported grid families, a shared
// texture directory, two authored scene tours. RES-01..04 run here.
void TestRecipeResolution() {
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pp_resolve";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	const fs::path ext = tmp / "external";
	const fs::path projDir = tmp / "proj";
	fs::create_directories(ext, ec);
	{
		bool created = false;
		std::string err;
		smvk::project::Project p = smvk::project::Project::Init(
			projDir.generic_string(), "ResolveTest", created, err);
		Check(p.Valid() && created, "resolve.project-created");
	}
	smvk::project::Project project;
	{
		std::string err;
		project = smvk::project::Project::Open(projDir.generic_string(),
			err);
	}
	Check(project.Valid(), "resolve.project-open");

	// Two imported families (the canonical import; grids big enough to
	// derive real LOD chains).
	const TestSource worldSrc = WriteGridGltf(ext, "world", 8);
	const TestSource propsSrc = WriteGridGltf(ext, "props", 6);
	for (const TestSource* src : { &worldSrc, &propsSrc }) {
		smvk::project::ImportRequest req;
		req.project = project;
		req.source = src->gltf;
		smvk::project::ImportResult res;
		std::string err;
		const std::string label = std::string("resolve.import.")
			+ (src == &worldSrc ? "world" : "props");
		Check(smvk::project::ImportProjectAsset(req, res, err)
			&& res.derived, label.c_str());
	}

	// The shared package's textures (content only — the resolver
	// classifies by extension, the bytes are irrelevant here). NOTE:
	// ContentRoot() IS the project root (the project is the content
	// tree) — "content/..." recipe paths are root-relative.
	const fs::path sharedTex = fs::path(project.ImportsDir())
		/ "shared" / "tex";
	fs::create_directories(sharedTex, ec);
	for (const char* name : { "stone.png", "wood.png" }) {
		std::ofstream f(sharedTex / name, std::ios::binary | std::ios::trunc);
		f << "\x89PNG-not-really-a-png";
	}

	// Authored scene tours: references[0] = the imported .geometry (the
	// root-reference contract BuildProject consumes).
	const auto writeTour = [](const fs::path& path,
			const std::string& artifact) {
		smvk::tour::Doc doc;
		smvk::tour::ReferenceCmd root;
		root.source = artifact;
		root.name = "ROOT";
		root.root = true;
		doc.references.push_back(root);
		std::string err;
		if (!smvk::tour::SaveTour(path.generic_string(), doc, err))
			std::cout << "  (tour save err: " << err << ")\n";
	};
	const fs::path scenes = fs::path(project.root) / "content" / "scenes";
	fs::create_directories(scenes, ec);
	writeTour(scenes / "world.tour", "content/models/world/world.geometry");
	writeTour(scenes / "props.tour", "content/models/props/props.geometry");

	const std::string recipeText =
		"recipe 1\n"
		"project ResolveTest\n"
		"entry content/scenes/world.tour\n"
		"\n"
		"bake gi\n"
		"bake cubemaps\n"
		"\n"
		"[pak world]\n"
		"output world.pak\n"
		"scene content/scenes/world.tour\n"
		"depends shared\n"
		"dependencies transitive\n"
		"\n"
		"[pak shared]\n"
		"output shared.pak\n"
		"include content/models/shared/**\n"
		"\n"
		"[pak props]\n"
		"output props.pak\n"
		"scene content/scenes/props.tour\n"
		"depends shared\n";
	smvk::project::ProjectRecipe recipe;
	smvk::project::RecipeDiagnostic diag;
	Check(smvk::project::ParseProjectRecipeText(recipeText, recipe, diag),
		"resolve.recipe-parses");

	smvk::project::BuildPlan plan;
	std::string err;
	Check(smvk::project::ResolveRecipe(project, recipe, plan, err)
			&& err.empty(),
		"resolve.valid-plan");
	if (!err.empty())
		std::cout << "  (resolve err: " << err << ")\n";
	if (!plan.entryPak.empty()) {
		// RES-01: the entry resolves to exactly one cooked scene owner.
		Check(plan.entryScene == "content/scenes/world.tour"
				&& plan.entryPak == "world",
			"resolve.entry-owned-once");
		// Topological order: shared lands BEFORE its dependents (the
		// authored order here is world, shared, props — resolution
		// reorders to shared, world, props; authored order is the
		// tiebreak between the independent world/props).
		Check(plan.paks.size() == 3 && plan.paks[0].id == "shared"
				&& plan.paks[1].id == "world"
				&& plan.paks[2].id == "props",
			"resolve.topological-order");
		const smvk::project::PlanPak* world = plan.FindPak("world");
		const smvk::project::PlanPak* shared = plan.FindPak("shared");
		const smvk::project::PlanPak* props = plan.FindPak("props");
		Check(world && world->scene == "content/scenes/world.tour"
				&& world->sceneArtifacts.size() == 1
				&& world->sceneArtifacts[0]
					== "content/models/world/world.geometry",
			"resolve.scene-artifacts-root-first");
		Check(shared && shared->scene.empty()
				&& shared->sceneArtifacts.empty()
				&& shared->assets.size() == 2
				&& shared->assets[0].path
					== "content/models/shared/tex/stone.png"
				&& shared->assets[0].kind
					== smvk::project::PlanAssetKind::Texture
				&& shared->assets[1].path
					== "content/models/shared/tex/wood.png",
			"resolve.asset-only-pak-owned-textures");
		Check(world && world->assets.size() == 1
				&& world->assets[0].kind
					== smvk::project::PlanAssetKind::Geometry,
			"resolve.scene-pak-owns-geometry");
		Check(props && props->sceneArtifacts[0]
				== "content/models/props/props.geometry",
			"resolve.second-scene-pak");
		// RES-04: no path is owned by two paks (the world pak does NOT
		// re-own the shared textures; shared does NOT own the geometry).
		{
			std::map<std::string, int> owners;
			bool unique = true;
			for (const auto& pak : plan.paks)
				for (const auto& asset : pak.assets)
					if (++owners[asset.path] > 1)
						unique = false;
			Check(unique, "resolve.one-owner-per-artifact");
			Check(owners.count("content/models/shared/tex/stone.png") == 1
				&& owners.count("content/models/world/world.geometry")
					== 1,
				"resolve.no-cross-pak-duplicates");
		}
		// RES-03: deterministic — the same inputs resolve identically.
		{
			smvk::project::BuildPlan again;
			std::string err2;
			Check(smvk::project::ResolveRecipe(project, recipe, again,
					err2)
				&& again == plan,
				"resolve.deterministic");
		}
	}

	// ---- RES-02: rejection cases (before any stage ever runs) -------
	const auto resolveFails = [&](const std::string& name,
			const std::string& text, const char* needle) {
		smvk::project::ProjectRecipe r;
		smvk::project::RecipeDiagnostic d;
		if (!smvk::project::ParseProjectRecipeText(text, r, d)) {
			Check(d.message.find(needle) != std::string::npos,
				(std::string("resolve.reject.") + name).c_str());
			return;
		}
		smvk::project::BuildPlan p;
		std::string e;
		const bool failed =
			!smvk::project::ResolveRecipe(project, r, p, e);
		if (failed && e.find(needle) == std::string::npos)
			std::cout << "  (" << name << " err: " << e << ")\n";
		Check(failed && e.find(needle) != std::string::npos,
			(std::string("resolve.reject.") + name).c_str());
	};
	// Missing scene file (entry owned by a pak, file absent).
	resolveFails("missing-scene",
		"recipe 1\nentry content/scenes/ghost.tour\n"
		"[pak a]\noutput a.pak\nscene content/scenes/ghost.tour\n",
		"not found");
	// The scene's referenced artifact is missing.
	{
		const fs::path gap = scenes / "gap.tour";
		writeTour(gap, "content/models/world/missing.geometry");
		resolveFails("missing-artifact",
			"recipe 1\nentry content/scenes/gap.tour\n"
			"[pak a]\noutput a.pak\nscene content/scenes/gap.tour\n",
			"artifact is missing");
	}
	// Two paks claiming the same include territory.
	resolveFails("overlapping-includes",
		"recipe 1\nentry content/scenes/world.tour\n"
		"[pak a]\noutput a.pak\n"
		"include content/models/shared/**\n"
		"[pak b]\noutput b.pak\n"
		"include content/models/shared/tex/*\n"
		"[pak world]\noutput w.pak\nscene content/scenes/world.tour\n",
		"already owned");
	// An include glob claiming another pak's scene geometry (the
	// cluster-internal boundary).
	resolveFails("include-vs-scene-conflict",
		"recipe 1\nentry content/scenes/world.tour\n"
		"[pak hoard]\noutput h.pak\n"
		"include content/models/world/*.geometry\n"
		"[pak world]\noutput w.pak\nscene content/scenes/world.tour\n",
		"cluster-internal");
	// Two scene paks referencing the same geometry.
	{
		const fs::path twin = scenes / "twin.tour";
		writeTour(twin, "content/models/world/world.geometry");
		resolveFails("two-scenes-one-geometry",
			"recipe 1\nentry content/scenes/world.tour\n"
			"[pak a]\noutput a.pak\nscene content/scenes/world.tour\n"
			"[pak b]\noutput b.pak\nscene content/scenes/twin.tour\n",
			"cluster-internal");
	}
	// A glob matching nothing (typo'd directory).
	resolveFails("empty-glob",
		"recipe 1\nentry content/scenes/world.tour\n"
		"[pak a]\noutput a.pak\ninclude content/models/nothere/**\n"
		"[pak world]\noutput w.pak\nscene content/scenes/world.tour\n",
		"matched no files");
	// An include hitting only family/support files, on an asset-only
	// pak (nothing classifiable was claimed).
	resolveFails("contentless-asset-pak",
		"recipe 1\nentry content/scenes/world.tour\n"
		"[pak a]\noutput a.pak\ninclude content/models/world/*.gltf\n"
		"[pak world]\noutput w.pak\nscene content/scenes/world.tour\n",
		"owns no content");
	// An include claiming geometry for a pak whose scene doesn't
	// reference it (or a scene-less pak): meshes are cluster-internal.
	resolveFails("geometry-without-scene",
		"recipe 1\nentry content/scenes/world.tour\n"
		"[pak hoard]\noutput h.pak\n"
		"include content/models/world/*.geometry\n"
		"[pak world]\noutput w.pak\nscene content/scenes/world.tour\n",
		"cluster-internal");
	// A SHARED nested subtree is a DAG, not a cycle: A -> B -> D and
	// A -> C -> D must resolve (the old never-cleared visited set
	// false-positived exactly this as a cycle — the commit review
	// caught it).
	{
		const auto writeTourWithRefs =
			[&](const fs::path& path,
				const std::vector<std::string>& nested) {
			smvk::tour::Doc doc;
			smvk::tour::ReferenceCmd root;
			root.source = "content/models/world/world.geometry";
			root.name = "ROOT";
			root.root = true;
			doc.references.push_back(root);
			for (const std::string& n : nested) {
				smvk::tour::ReferenceCmd ref;
				ref.source = n;
				ref.name = "REF " + n;
				doc.references.push_back(ref);
			}
			std::string sErr;
			smvk::tour::SaveTour(path.generic_string(), doc, sErr);
		};
		writeTourWithRefs(scenes / "dagD.tour", {});
		writeTourWithRefs(scenes / "dagB.tour",
			{ "content/scenes/dagD.tour" });
		writeTourWithRefs(scenes / "dagC.tour",
			{ "content/scenes/dagD.tour" });
		writeTourWithRefs(scenes / "dagA.tour",
			{ "content/scenes/dagB.tour", "content/scenes/dagC.tour" });
		smvk::project::ProjectRecipe r;
		smvk::project::RecipeDiagnostic d;
		smvk::project::BuildPlan p;
		std::string e;
		const std::string dag =
			"recipe 1\n"
			"entry content/scenes/dagA.tour\n"
			"[pak a]\noutput a.pak\nscene content/scenes/dagA.tour\n";
		Check(smvk::project::ParseProjectRecipeText(dag, r, d)
			&& smvk::project::ResolveRecipe(project, r, p, e),
			"resolve.shared-subtree-dag-ok");
		// The shared subtree's artifact appears ONCE (the dedupe), and
		// a real back-edge (D -> A) still rejects as a cycle.
		Check(e.empty() && p.paks[0].sceneArtifacts.size() == 1,
			"resolve.shared-subtree-walks-once");
		writeTourWithRefs(scenes / "dagD.tour",
			{ "content/scenes/dagA.tour" });
		smvk::project::BuildPlan p2;
		std::string e2;
		Check(!smvk::project::ResolveRecipe(project, r, p2, e2)
			&& e2.find("cycle") != std::string::npos,
			"resolve.real-back-edge-still-cycle");
	}
	// A nested scene-reference cycle (a -> b -> a).
	{
		writeTour(scenes / "loopA.tour", "content/models/world/world.geometry");
		smvk::tour::Doc doc;
		smvk::tour::ReferenceCmd root;
		root.source = "content/models/world/world.geometry";
		root.name = "ROOT";
		root.root = true;
		doc.references.push_back(root);
		smvk::tour::ReferenceCmd nested;
		nested.source = "content/scenes/loopB.tour";
		nested.name = "REF loopB";
		doc.references.push_back(nested);
		std::string saveErr;
		smvk::tour::SaveTour((scenes / "loopA.tour").generic_string(),
			doc, saveErr);
		doc.references[1].source = "content/scenes/loopA.tour";
		smvk::tour::SaveTour((scenes / "loopB.tour").generic_string(),
			doc, saveErr);
		resolveFails("nested-reference-cycle",
			"recipe 1\nentry content/scenes/loopA.tour\n"
			"[pak a]\noutput a.pak\nscene content/scenes/loopA.tour\n",
			"cycle");
	}
}

// ---- the appmanifest (.tapp) module + the executor's publish ---------

void TestAppManifest() {
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pp_tapp";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	// Round-trip: write -> read -> identical.
	{
		smvk::AppManifest m;
		m.version = smvk::kAppManifestVersion;
		m.entryPak = "world.pak";
		m.entryScene = "scene";
		m.paks = { "shared.pak", "world.pak" };
		const fs::path file = tmp / "sponza.tapp";
		std::string err;
		Check(smvk::SaveAppManifest(file.generic_string(), m, err),
			"tapp.save-ok");
		smvk::AppManifest back;
		Check(smvk::LoadAppManifest(file.generic_string(), back, err)
			&& back.version == m.version && back.entryPak == m.entryPak
			&& back.entryScene == m.entryScene && back.paks == m.paks,
			"tapp.round-trip");
		// The written text carries the format markers.
		{
			std::ifstream f(file, std::ios::binary);
			std::stringstream ss;
			ss << f.rdbuf();
			const std::string text = ss.str();
			Check(text.find("appmanifest 1") != std::string::npos
				&& text.find("entry world.pak scene")
					!= std::string::npos
				&& text.find("[paks]") != std::string::npos,
				"tapp.format-markers");
		}
	}
	// Validation rejections (both directions share ValidateAppManifest).
	{
		struct Bad {
			const char* name;
			std::string text;
		};
		const Bad bad[] = {
			{ "no-version", "# c\nentry a.pak scene\n[paks]\na.pak\n" },
			{ "bad-version", "appmanifest 2\nentry a.pak scene\n"
				"[paks]\na.pak\n" },
			{ "no-entry", "appmanifest 1\n[paks]\na.pak\n" },
			{ "entry-not-listed", "appmanifest 1\nentry b.pak scene\n"
				"[paks]\na.pak\n" },
			{ "no-paks", "appmanifest 1\nentry a.pak scene\n[paks]\n" },
			{ "absolute-pak", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\nC:/abs/a.pak\n" },
			{ "escaping-pak", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\n../a.pak\n" },
			{ "duplicate-pak", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\na.pak\na.pak\n" },
			{ "unknown-directive", "appmanifest 1\nfrobnicate x\n"
				"entry a.pak scene\n[paks]\na.pak\n" },
			{ "unknown-section", "appmanifest 1\nentry a.pak scene\n"
				"[recipes]\na.pak\n" },
			{ "bad-entry-arity", "appmanifest 1\nentry a.pak\n"
				"[paks]\na.pak\n" },
			{ "empty-scene-name", "appmanifest 1\nentry a.pak \"\"\n"
				"[paks]\na.pak\n" },
			// The [ids] table (the runtime catalog mirror).
			{ "id-bad-hex", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\na.pak\n[ids]\nzz scene a.pak c/s.tour\n" },
			{ "id-duplicate", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\na.pak\n[ids]\n0x10 scene a.pak c/a.tour\n"
				"0x10 texture a.pak c/b.png\n" },
			{ "id-bad-kind", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\na.pak\n[ids]\n0x10 widget a.pak c/a.tour\n" },
			{ "id-unlisted-pak", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\na.pak\n[ids]\n0x10 scene b.pak c/a.tour\n" },
			{ "id-bad-arity", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\na.pak\n[ids]\n0x10 scene a.pak\n" },
			{ "id-zero", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\na.pak\n[ids]\n0x0 scene a.pak c/a.tour\n" },
			// The scene-name token (the id->scene join): 5th token is
			// scene-rows-only, and whitespace in it is malformed.
			{ "id-name-on-nonscene", "appmanifest 1\nentry a.pak"
				" scene\n[paks]\na.pak\n[ids]\n0x10 texture a.pak"
				" c/a.png Church\n" },
			{ "id-name-whitespace", "appmanifest 1\nentry a.pak"
				" scene\n[paks]\na.pak\n[ids]\n0x10 scene a.pak"
				" c/a.tour \"Two Words\"\n" },
			{ "id-bad-arity-6", "appmanifest 1\nentry a.pak scene\n"
				"[paks]\na.pak\n[ids]\n0x10 scene a.pak c/a.tour x y\n" },
		};
		for (const Bad& b : bad) {
			smvk::AppManifest m;
			std::string err;
			Check(!smvk::LoadAppManifestText(b.text, m, err)
				&& !err.empty(),
			(std::string("tapp.reject.") + b.name).c_str());
		}
		// Save refuses an invalid manifest too.
		{
			smvk::AppManifest m;
			m.version = smvk::kAppManifestVersion;
			m.entryPak = "notlisted.pak";
			m.entryScene = "scene";
			m.paks = { "a.pak" };
			std::string err;
			Check(!smvk::SaveAppManifest((tmp / "bad.tapp").generic_string(),
					m, err)
				&& err.find("not listed") != std::string::npos,
				"tapp.save-rejects-invalid");
		}
	}
	// The [ids] round-trip: save -> load -> identical rows + FindAsset.
	{
		smvk::AppManifest m;
		m.version = smvk::kAppManifestVersion;
		m.entryPak = "world.pak";
		m.entryScene = "scene";
		m.paks = { "shared.pak", "world.pak" };
		m.assets = {
			{ 0x1000, "scene", "world.pak", "content/scenes/w.tour",
				"Church" },
			// A scene row WITHOUT a name (pre-name manifests) stays
			// 4-token — the runtime infers from a single-scene pak.
			{ 0x1001, "scene", "shared.pak", "content/scenes/o.tour" },
			{ 0x2000, "texture", "shared.pak",
				"content/models/s/stone.png" },
			{ 0x3000, "mesh", "world.pak", "content/models/w/w.geometry" },
			// The sound kind (0x4xxx rows — VFS-addressable .wav
			// assets, the second byte-readable kind).
			{ 0x4000, "sound", "shared.pak",
				"content/audio/bells.wav" },
		};
		const fs::path file = tmp / "ids.tapp";
		std::string err;
		Check(smvk::SaveAppManifest(file.generic_string(), m, err),
			"tapp.ids-save-ok");
		smvk::AppManifest back;
		Check(smvk::LoadAppManifest(file.generic_string(), back, err)
			&& back.assets.size() == 5
			&& back.assets[0].name == "Church"
			&& back.assets[1].name.empty()
			&& back.assets[2].id == 0x2000
			&& back.assets[2].kind == "texture"
			&& back.assets[2].pak == "shared.pak"
			&& back.assets[2].path == "content/models/s/stone.png"
			&& back.assets[2].name.empty()
			&& back.assets[3].id == 0x3000
			&& back.assets[3].kind == "mesh"
			&& back.assets[4].id == 0x4000
			&& back.assets[4].kind == "sound"
			&& back.assets[4].pak == "shared.pak"
			&& back.assets[4].path == "content/audio/bells.wav"
			&& back.FindAsset(0x3000) == &back.assets[3]
			&& back.FindAsset(0x4000) == &back.assets[4]
			&& back.FindAsset(0x9999) == nullptr,
			"tapp.ids-round-trip");
		// The written text carries the scene-name token only where it
		// exists (the 5th [ids] token — scene rows only).
		{
			std::ifstream f(file, std::ios::binary);
			std::stringstream ss;
			ss << f.rdbuf();
			const std::string text = ss.str();
			Check(text.find(
					"0x1000 scene world.pak content/scenes/w.tour Church")
					!= std::string::npos
				&& text.find(
					"0x1001 scene shared.pak content/scenes/o.tour\n")
					!= std::string::npos,
				"tapp.ids-scene-name-token");
		}
	}
	// ---- the compiled-catalog contract ---------------------------------
	// The fingerprint fold: shared by the generated header and the
	// manifest (one implementation - smvk::ComputeCatalogFingerprint).
	// Deterministic, order-sensitive, and INDEPENDENT of the published
	// pak filenames (content-addressed names change every cook).
	{
		const uint64_t ids[3] = { 0x1000, 0x2000, 0x4000 };
		const uint8_t kinds[3] = { smvk::kCatalogKindScene,
			smvk::kCatalogKindTexture, smvk::kCatalogKindSound };
		const uint32_t pakIdx[3] = { 1, 0, 0 };
		const char* const paths[3] = { "content/scenes/w.tour",
			"content/models/s/stone.png", "content/audio/bells.wav" };
		const uint64_t a = smvk::ComputeCatalogFingerprint(
			ids, kinds, pakIdx, paths, 3);
		const uint64_t b = smvk::ComputeCatalogFingerprint(
			ids, kinds, pakIdx, paths, 3);
		Check(a == b && a != 0, "tapp.fold-deterministic");
		// Order sensitivity (a swapped row folds differently).
		{
			const uint64_t ids2[3] = { ids[1], ids[0], ids[2] };
			const uint8_t kinds2[3] = { kinds[1], kinds[0], kinds[2] };
			const uint32_t pakIdx2[3] = { pakIdx[1], pakIdx[0],
				pakIdx[2] };
			const char* const paths2[3] = { paths[1], paths[0],
				paths[2] };
			Check(smvk::ComputeCatalogFingerprint(ids2, kinds2, pakIdx2,
					paths2, 3) != a,
				"tapp.fold-order-sensitive");
		}
		// Content sensitivity (a moved path folds differently).
		{
			const char* const paths2[3] = { "content/scenes/moved.tour",
				paths[1], paths[2] };
			Check(smvk::ComputeCatalogFingerprint(ids, kinds, pakIdx,
					paths2, 3) != a,
				"tapp.fold-path-sensitive");
		}
		// The kind-string <-> byte mapping (the ABI).
		{
			uint8_t k = 0xFF;
			Check(smvk::CatalogKindFromString("scene", k)
					&& k == smvk::kCatalogKindScene
					&& smvk::CatalogKindFromString("sound", k)
					&& k == smvk::kCatalogKindSound
					&& !smvk::CatalogKindFromString("widget", k)
					&& std::string(smvk::CatalogKindToString(
						smvk::kCatalogKindMesh)) == "mesh",
				"tapp.fold-kind-mapping");
		}
	}
	// The `catalog` line round-trips (and a bad hex value rejects).
	{
		smvk::AppManifest m;
		m.version = smvk::kAppManifestVersion;
		m.entryPak = "world.pak";
		m.entryScene = "scene";
		m.paks = { "world.pak" };
		m.catalogFingerprint = 0xDEADBEEF12345678ULL;
		const fs::path file = tmp / "fp.tapp";
		std::string err;
		Check(smvk::SaveAppManifest(file.generic_string(), m, err),
			"tapp.fingerprint-save-ok");
		smvk::AppManifest back;
		Check(smvk::LoadAppManifest(file.generic_string(), back, err)
			&& back.catalogFingerprint == 0xDEADBEEF12345678ULL,
			"tapp.fingerprint-round-trip");
		{
			std::ifstream f(file, std::ios::binary);
			std::stringstream ss;
			ss << f.rdbuf();
			Check(ss.str().find("catalog 0xdeadbeef12345678")
					!= std::string::npos,
				"tapp.fingerprint-line-written");
		}
		{
			const fs::path bad = tmp / "fp_bad.tapp";
			std::ofstream f(bad, std::ios::binary | std::ios::trunc);
			f << "appmanifest 1\nentry world.pak scene\n"
				<< "catalog nothex\n\n[paks]\nworld.pak\n";
			f.close();
			smvk::AppManifest b2;
			Check(!smvk::LoadAppManifest(bad.generic_string(), b2, err),
				"tapp.fingerprint-bad-hex-rejected");
		}
	}
}

// ---- the shared executor (M2: one plan, adapter-invoked stages) ------

// A minimal VALID scene blob (the v1 shape: one root node, identity
// transform, empty tables) so ListPakScenes accepts the recording
// adapter's fake paks — the executor's manifest step parses the entry
// pak's scenes after a successful stage.
std::vector<uint8_t> MinimalSceneBlob() {
	std::vector<uint8_t> b;
	const auto u32 = [&b](uint32_t v) {
		for (int i = 0; i < 4; ++i)
			b.push_back(uint8_t((v >> (8 * i)) & 0xFF));
	};
	const auto i32 = [&u32](int32_t v) { u32(uint32_t(v)); };
	u32(1); // node count
	i32(-1); i32(-1); i32(-1); i32(-1); i32(0); // hierarchy[0]
	for (float v : { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }) {
		uint32_t bits = 0;
		std::memcpy(&bits, &v, 4);
		u32(bits);
	}
	u32(0); // mesh pairs
	u32(0); // material pairs
	u32(0); // node-name pairs
	u32(0); // node names
	u32(0); // material names
	return b;
}

// A recording adapter: captures every stage invocation WITHOUT running
// anything — the sequencing + args contract, checked pure-CPU.
class RecordingAdapter final : public smvk::project::StageAdapter {
public:
	struct Call {
		const char* stage;
		std::vector<std::string> args;
	};
	std::vector<Call> calls;
	int cookRc = 0, giRc = 0, pakRc = 0;
	int failPakCall = -1;    // 0-based Pak-call index to fail (-1 never)
	int pakCallCount = 0;
	// Payload variant for the fake paks (the content-addressed
	// publication tests flip it to produce different pak bytes).
	std::string payloadTag;
	int Cook(const smvk::project::CookStageArgs& a,
		std::string&) override {
		calls.push_back({ "Cook", { a.contentDir, a.tour,
			a.cubemap ? "cubemap:on" : "cubemap:off",
			a.lightmap ? "lightmap:on" : "lightmap:off" } });
		return cookRc;
	}
	int GiBaker(const smvk::project::GiBakerStageArgs& a,
		std::string&) override {
		calls.push_back({ "GiBaker", { a.contentDir, a.tour } });
		return giRc;
	}
	int Pak(const smvk::project::PakStageArgs& a,
		std::string&) override {
		std::vector<std::string> v = { a.contentDir, a.inVfs, a.tour,
			a.outPak, a.verify ? "verify:on" : "verify:off" };
		for (const std::string& e : a.extraArtifacts)
			v.push_back("extra:" + e);
		for (const std::string& e : a.contentSet)
			v.push_back("content-set:" + e);
		for (const std::string& e : a.excludeVfs)
			v.push_back("exclude:" + e);
		calls.push_back({ "Pak", v });
		// The staged-publication regression control: fail ONE pak call
		// (0-based; -1 = never) so a mid-set failure can be exercised.
		if (failPakCall == int(pakCallCount++))
			return 1;
		if (pakRc == 0) {
			// The executor verifies the stage's output EXISTS, POLICES
			// it through PakReader, and lists the entry pak's SCENES for
			// the manifest — the recording adapter fakes a REAL minimal
			// pak (one valid v1 scene blob; policing only inspects
			// Texture entries, so a scene-blob-only pak passes it by
			// construction).
			std::error_code ec;
			fs::create_directories(fs::path(a.outPak).parent_path(), ec);
			smvk::PakWriter w;
			w.AddBlob(smvk::PakAssetKind::Scene, "recording/fake",
				MinimalSceneBlob());
			// The payload variant rides a SEPARATE blob (appending
			// bytes to the scene blob breaks ParseSceneBlob's trailing
			// sections): a distinct pak digest without a distinct
			// scene. Only the SCENE pak's call (non-empty tour) — the
			// variant models "one pak's content changed", not both.
			if (!payloadTag.empty() && !a.tour.empty()) {
				const std::vector<uint8_t> tag(payloadTag.begin(),
					payloadTag.end());
				w.AddBlob(smvk::PakAssetKind::Texture, "recording/tag",
					smvk::BuildTexturePayload("recording/tag", -1, -1,
						-1, -1, std::span<const uint8_t>(tag)));
			}
			const std::vector<uint8_t> image = w.Finalize();
			std::ofstream f(a.outPak, std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char*>(image.data()),
				std::streamsize(image.size()));
		}
		return pakRc;
	}
};

// The IN-PROCESS stage adapter — the editor's form (the linked
// Cook/GiBaker/Pak Runs). The EDT-01 parity leg builds the same plan
// through this and through the subprocess adapter and byte-compares
// the paks: the CLI and the editor drive ONE pipeline.
class InProcessAdapter final : public smvk::project::StageAdapter {
public:
	int Cook(const smvk::project::CookStageArgs& a,
		std::string&) override {
		cooktool::CookArgs args;
		args.contentDir = a.contentDir;
		args.tour = a.tour;
		args.cubemap = a.cubemap;
		args.lightmap = a.lightmap;
		args.registryDir = a.registryDir;
		return cooktool::Run(args);
	}
	int GiBaker(const smvk::project::GiBakerStageArgs& a,
		std::string&) override {
		gibaker::Args args;
		args.contentDir = a.contentDir;
		args.tour = a.tour;
		return gibaker::Run(args);
	}
	int Pak(const smvk::project::PakStageArgs& a,
		std::string&) override {
		paktool::PakArgs args;
		args.contentDir = a.contentDir;
		args.inVfs = a.inVfs;
		args.inVfsList = a.extraArtifacts;
		args.tour = a.tour;
		args.outPak = a.outPak;
		args.verify = a.verify;
		args.levelName = a.levelName;
		args.registryDir = a.registryDir;
		return paktool::Run(args);
	}
};

std::vector<char> ReadAllBytes(const fs::path& p) {
	std::ifstream f(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(f),
		std::istreambuf_iterator<char>() };
}

void TestBuildPlanExecutor() {
	// The fixture: a single-scene project (grid import + authored tour).
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pp_exec";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	const fs::path ext = tmp / "external";
	const fs::path projDir = tmp / "proj";
	fs::create_directories(ext, ec);
	{
		bool created = false;
		std::string err;
		smvk::project::Project p = smvk::project::Project::Init(
			projDir.generic_string(), "ExecTest", created, err);
		Check(p.Valid() && created, "executor.project-created");
	}
	smvk::project::Project project;
	{
		std::string err;
		project = smvk::project::Project::Open(projDir.generic_string(),
			err);
	}
	Check(project.Valid(), "executor.project-open");
	// A material on the primitive: the bake paths (bake-input grid +
	// BuildTraceSceneFromCluster) require a material ROW on the mesh
	// node — material-less geometry renders with the default but the
	// trace paths skip it ("no renderable geometry").
	const TestSource src = WriteGridGltf(ext, "grid", 8, 0, true);
	{
		smvk::project::ImportRequest req;
		req.project = project;
		req.source = src.gltf;
		smvk::project::ImportResult res;
		std::string err;
		Check(smvk::project::ImportProjectAsset(req, res, err)
			&& res.derived, "executor.import-grid");
	}
	const fs::path scenes = fs::path(project.root) / "content" / "scenes";
	fs::create_directories(scenes, ec);
	{
		smvk::tour::Doc doc;
		smvk::tour::ReferenceCmd root;
		root.source = "content/models/grid/grid.geometry";
		root.name = "ROOT";
		root.root = true;
		doc.references.push_back(root);
		// The default-material seed (materialAsset -default): the grid
		// glTF has no materials, so without the pool's default entry the
		// bake's FindMaterial skips every mesh ("no renderable
		// geometry") — the canonical tours all carry this line.
		smvk::tour::MaterialAssetCmd defMat;
		defMat.editorDefault = true;
		defMat.name = "default";
		doc.materialAssets.push_back(defMat);
		std::string err;
		Check(smvk::tour::SaveTour((scenes / "grid.tour").generic_string(),
				doc, err),
			"executor.tour-saved");
	}
	const std::string recipeText =
		"recipe 1\n"
		"project ExecTest\n"
		"entry content/scenes/grid.tour\n"
		"bake gi\n"
		"bake cubemaps off\n"
		"[pak game]\n"
		"output grid.pak\n"
		"scene content/scenes/grid.tour\n";
	smvk::project::ProjectRecipe recipe;
	smvk::project::RecipeDiagnostic diag;
	Check(smvk::project::ParseProjectRecipeText(recipeText, recipe,
			diag),
		"executor.recipe-parses");
	smvk::project::BuildPlan plan;
	std::string err;
	Check(smvk::project::ResolveRecipe(project, recipe, plan, err),
		"executor.plan-resolves");

	// Sequencing + args through the recording adapter (no stages run).
	{
		RecordingAdapter rec;
		smvk::project::BuildPlanOptions opts;
		opts.publishDir = (tmp / "publish").generic_string();
		smvk::project::BuildPlanReport rep;
		std::string e;
		Check(smvk::project::ExecuteBuildPlan(project, plan, rec, opts,
				rep, e)
				&& e.empty() && rep.paks.size() == 1,
			"executor.plan-runs");
		if (!e.empty())
			std::cout << "  (plan-runs err: " << e << ")\n";
		Check(rec.calls.size() == 3
			&& std::string(rec.calls[0].stage) == "Cook"
			&& std::string(rec.calls[1].stage) == "GiBaker"
			&& std::string(rec.calls[2].stage) == "Pak",
			"executor.stage-sequence");
		if (rec.calls.size() >= 3) {
			Check(rec.calls[0].args[0] == project.root
				&& rec.calls[0].args[2] == "cubemap:off"
				&& rec.calls[0].args[3] == "lightmap:off",
				"executor.cook-flags-from-recipe");
			Check(rec.calls[2].args[1]
					== "content/models/grid/grid.geometry"
				&& rec.calls[2].args[2]
					== (scenes / "grid.tour").lexically_normal()
						.generic_string()
				&& rec.calls[2].args[3]
					== (tmp / "publish" / ".staging" / "grid.pak")
						.lexically_normal().generic_string()
				&& rec.calls[2].args[4] == "verify:on",
				"executor.pak-args");
		}
		if (rep.paks.size() == 1) {
			Check(rep.paks[0].id == "game"
				&& rep.paks[0].scene == "content/scenes/grid.tour"
				&& !rep.paks[0].reimported,
				"executor.report");
		}
		else {
			Check(false, "executor.report");
		}
	}
	// 'bake gi off' skips the GiBaker stage entirely.
	{
		smvk::project::BuildPlan noGi = plan;
		noGi.bakeGi = false;
		RecordingAdapter rec;
		smvk::project::BuildPlanOptions opts;
		opts.publishDir = (tmp / "publish").generic_string();
		smvk::project::BuildPlanReport rep;
		std::string e;
		Check(smvk::project::ExecuteBuildPlan(project, noGi, rec, opts,
				rep, e),
			"executor.bake-gi-off-runs");
		Check(rec.calls.size() == 2
			&& std::string(rec.calls[1].stage) == "Pak",
			"executor.bake-gi-off-skips-gibaker");
	}
	// Failure propagation: a failing stage aborts the plan (the failing
	// stage is named) and NOTHING later runs.
	{
		RecordingAdapter rec;
		rec.cookRc = 1;
		smvk::project::BuildPlanOptions opts;
		smvk::project::BuildPlanReport rep;
		std::string e;
		Check(!smvk::project::ExecuteBuildPlan(project, plan, rec, opts,
				rep, e)
			&& e.find("the Cook stage failed") != std::string::npos
			&& rec.calls.size() == 1 && rep.paks.empty(),
			"executor.cook-failure-aborts");
	}
	{
		RecordingAdapter rec;
		rec.pakRc = 2;
		smvk::project::BuildPlanOptions opts;
		smvk::project::BuildPlanReport rep;
		std::string e;
		Check(!smvk::project::ExecuteBuildPlan(project, plan, rec, opts,
				rep, e)
			&& e.find("the Pak stage failed") != std::string::npos
			&& rec.calls.size() == 3 && rep.paks.empty(),
			"executor.pak-failure-aborts");
	}
	// ---- the multi-pak gate (M3): asset-only shared pak + scene pak -----
	// The grid glTF's material references shared/stone.png; the import
	// copies it into the family (content/models/grid/shared/stone.png);
	// the recipe gives it to an ASSET-ONLY shared pak and world depends
	// on shared — world's pak must NOT embed it (the cross-pak exclude;
	// its material keeps the ref) while shared's pak carries it as a
	// content-set Texture blob with no scene.
	{
		const fs::path sharedPng = fs::path(project.ImportsDir())
			/ "grid" / "shared" / "stone.png";
		Check(fs::is_regular_file(sharedPng, ec),
			"executor.texture-copied-in");
		const std::string two =
			"recipe 1\n"
			"entry content/scenes/grid.tour\n"
			"bake gi\n"
			"bake cubemaps off\n"
			"[pak world]\n"
			"output world.pak\n"
			"scene content/scenes/grid.tour\n"
			"depends shared\n"
			"[pak shared]\n"
			"output shared.pak\n"
			"include content/models/grid/shared/**\n";
		smvk::project::ProjectRecipe r2;
		smvk::project::RecipeDiagnostic d2;
		smvk::project::BuildPlan p2;
		std::string e;
		Check(smvk::project::ParseProjectRecipeText(two, r2, d2)
			&& smvk::project::ResolveRecipe(project, r2, p2, e),
			"executor.two-pak-plan-resolves");
		Check(p2.paks.size() == 2 && p2.paks[0].id == "shared"
			&& p2.paks[1].id == "world"
			&& p2.paks[0].assets.size() == 1
			&& p2.paks[0].assets[0].path
				== "content/models/grid/shared/stone.png",
			"executor.shared-owns-texture");
		// The recording leg: the stage args carry the content set and
		// the cross-pak exclude (calls: Cook, GiBaker, Pak(shared),
		// Pak(world) — scene bakes first, then paks in plan order).
		{
			RecordingAdapter rec;
			smvk::project::BuildPlanOptions opts;
			opts.publishDir = (tmp / "publish3").generic_string();
			smvk::project::BuildPlanReport rep;
			std::string e2;
			Check(smvk::project::ExecuteBuildPlan(project, p2, rec,
					opts, rep, e2)
				&& rep.paks.size() == 2,
				"executor.two-pak-runs");
			if (!e2.empty())
				std::cout << "  (two-pak-runs err: " << e2 << ")\n";
			Check(rec.calls.size() == 4
				&& std::string(rec.calls[2].stage) == "Pak"
				&& std::string(rec.calls[3].stage) == "Pak",
				"executor.two-pak-call-order");
			if (rec.calls.size() == 4) {
				const std::vector<std::string>& sharedArgs =
					rec.calls[2].args;
				const std::vector<std::string>& worldArgs =
					rec.calls[3].args;
				const auto hasArg = [](const std::vector<std::string>& v,
						const std::string& needle) {
					for (const std::string& a : v)
						if (a == needle)
							return true;
					return false;
				};
				Check(sharedArgs[2] == ""
					&& hasArg(sharedArgs,
						"content-set:content/models/grid/shared/"
						"stone.png")
					&& !hasArg(sharedArgs, "exclude:"),
					"executor.shared-content-set-args");
				Check(hasArg(worldArgs,
						"exclude:content/models/grid/shared/stone.png")
					&& !hasArg(worldArgs, "content-set:"),
					"executor.world-exclude-args");
			}
		}
		// ---- the staged-publication regression (the commit review's
		// #5): a pak failing MID-SET never touches the published
		// generation — the finals stand byte-identical and no staging
		// file lingers.
		std::string ok1Shared, ok1World; // the first generation's paths
		{
			smvk::project::BuildPlanOptions opts;
			opts.publishDir = (tmp / "publish5").generic_string();
			smvk::project::BuildPlanReport ok1;
			std::string e1;
			RecordingAdapter rec1;
			Check(smvk::project::ExecuteBuildPlan(project, p2, rec1,
					opts, ok1, e1)
				&& ok1.paks.size() == 2,
				"executor.staged-generation-builds");
			ok1Shared = ok1.paks[0].output;
			ok1World = ok1.paks[1].output;
			const std::vector<char> sharedBefore =
				ReadAllBytes(ok1.paks[0].output);
			const std::vector<char> worldBefore =
				ReadAllBytes(ok1.paks[1].output);
			// The rebuild: the SECOND pak call (world, plan order
			// shared->world) FAILS after shared staged successfully —
			// the whole generation must roll back.
			RecordingAdapter rec2;
			rec2.failPakCall = 1;
			smvk::project::BuildPlanReport fail;
			std::string e2;
			Check(!smvk::project::ExecuteBuildPlan(project, p2, rec2,
					opts, fail, e2)
				&& e2.find("the Pak stage failed") != std::string::npos
				&& fail.paks.empty(),
				"executor.staged-midset-failure-fails");
			Check(ReadAllBytes(ok1.paks[0].output) == sharedBefore
				&& ReadAllBytes(ok1.paks[1].output) == worldBefore,
				"executor.staged-previous-generation-untouched");
			// No staging leftovers (the .staging dir holds nothing).
			bool stagingLingers = false;
			if (fs::is_directory(tmp / "publish5" / ".staging", ec))
				for (const fs::directory_entry& e :
						fs::directory_iterator(
							tmp / "publish5" / ".staging", ec))
					stagingLingers = true;
			Check(!stagingLingers,
				"executor.staged-no-staging-leftovers");
		}
		// ---- the content-addressed publication contract (the commit
		// review's follow-up round): unchanged content DEDUPS (same
		// immutable name, mtime never moves, no duplicates); changed
		// content publishes a NEW immutable name and the stale sweep
		// retires the superseded generation after the manifest commits.
		{
			smvk::project::BuildPlanOptions opts;
			opts.publishDir = (tmp / "publish5").generic_string();
			// Unchanged rebuild: same bytes -> same digest -> the
			// published file is never touched (mtime stands), and the
			// publish dir carries exactly the two paks of one
			// generation.
			const auto pakCount = [&]() {
				size_t n = 0;
				for (const fs::directory_entry& e :
						fs::directory_iterator(tmp / "publish5", ec))
					if (e.is_regular_file(ec)
							&& e.path().extension() == ".pak")
						++n;
				return n;
			};
			const fs::file_time_type worldMtime =
				fs::last_write_time(ok1World, ec);
			ec.clear();
			{
				RecordingAdapter rec;
				smvk::project::BuildPlanReport rep;
				std::string e;
				Check(smvk::project::ExecuteBuildPlan(project, p2, rec,
						opts, rep, e)
					&& rep.paks.size() == 2,
					"executor.addressed-rebuild-ok");
				Check(rep.paks[0].output == ok1Shared
					&& rep.paks[1].output == ok1World,
					"executor.addressed-unchanged-same-names");
				Check(fs::last_write_time(ok1World, ec) == worldMtime,
					"executor.addressed-dedup-mtime-untouched");
				ec.clear();
				Check(pakCount() == 2,
					"executor.addressed-no-duplicates");
			}
			// Changed content (a different pak payload for world): a
			// NEW immutable name lands, the manifest commits to it, and
			// the sweep retires the superseded world pak while the
			// unchanged shared pak stays (dedup again).
			{
				RecordingAdapter rec;
				rec.payloadTag = "v2";
				smvk::project::BuildPlanReport rep;
				std::string e;
				Check(smvk::project::ExecuteBuildPlan(project, p2, rec,
						opts, rep, e)
					&& rep.paks.size() == 2,
					"executor.addressed-changed-rebuild-ok");
				if (!e.empty())
					std::cout << "  (addressed-changed err: " << e
						<< ")\n";
				Check(rep.paks[0].output == ok1Shared
					&& rep.paks[1].output != ok1World,
					"executor.addressed-changed-new-name");
				Check(!fs::is_regular_file(ok1World, ec),
					"executor.addressed-old-pak-swept");
				ec.clear();
				Check(pakCount() == 2,
					"executor.addressed-sweep-single-generation");
				// The manifest committed to the new generation.
				smvk::AppManifest m;
				std::string mErr;
				Check(smvk::LoadAppManifest(
						(tmp / "publish5" / "world.tapp")
							.generic_string(),
						m, mErr)
					&& m.paks.size() == 2
					&& m.paks[1] == fs::path(rep.paks[1].output)
						.filename()
						.generic_string(),
					"executor.addressed-manifest-committed");
			}
		}
		// The real leg: build through the subprocess adapter, then
		// inspect BOTH paks (BLD-02 verify + BLD-07 asset-only + the
		// ownership police's contract).
		{
			auto sub = smvk::project::MakeSubprocessStageAdapter();
			smvk::project::BuildPlanOptions opts;
			opts.publishDir = (tmp / "publish4").generic_string();
			smvk::project::BuildPlanReport rep;
			std::string e3;
			Check(smvk::project::ExecuteBuildPlan(project, p2, *sub,
					opts, rep, e3),
				"executor.two-pak-build-ok");
			if (!e3.empty())
				std::cout << "  (two-pak err: " << e3 << ")\n";
			if (rep.paks.size() == 2) {
				// The manifest published last: parses, names the entry
				// pak + cooked scene identity, lists the paks in plan
				// (mount) order, and every listed pak exists beside it.
				smvk::AppManifest manifest;
				std::string mErr;
				Check(!rep.manifest.empty()
					&& smvk::LoadAppManifest(rep.manifest, manifest,
						mErr),
					"executor.manifest-published");
				Check(manifest.entryPak
						== fs::path(rep.paks[1].output)
							.filename()
							.generic_string()
					&& !manifest.entryScene.empty()
					&& manifest.paks.size() == 2
					&& manifest.paks[0]
						== fs::path(rep.paks[0].output)
							.filename()
							.generic_string()
					&& manifest.paks[1]
						== fs::path(rep.paks[1].output)
							.filename()
							.generic_string(),
					"executor.manifest-content");
				// The published names are CONTENT-ADDRESSED
				// (<stem>-<16hex>.pak — immutable, atomic-generation
				// publication).
				{
					const auto isAddressed = [](const std::string& n) {
						if (n.size() < 6
								|| n.compare(n.size() - 4, 4, ".pak")
									!= 0)
							return false;
						const size_t dash = n.rfind('-');
						if (dash == std::string::npos)
							return false;
						const std::string h =
							n.substr(dash + 1, n.size() - dash - 5);
						if (h.size() != 16)
							return false;
						for (char c : h)
							if (!((c >= '0' && c <= '9')
									|| (c >= 'a' && c <= 'f')))
								return false;
						return true;
					};
					Check(std::all_of(manifest.paks.begin(),
							manifest.paks.end(),
							[&](const std::string& n) {
								return isAddressed(n);
							}),
						"executor.manifest-content-addressed-names");
				}
				// The content-id table (the runtime catalog mirror —
				// the same registry the ids command maintains): the
				// scene + mesh in the WORLD pak, the texture cross-pak
				// in the SHARED pak, ids from the kind ranges, paks
				// named by their published (content-addressed) files.
				{
					const smvk::AppManifestAsset* scene =
						manifest.FindAsset(0x1000);
					const smvk::AppManifestAsset* tex =
						manifest.FindAsset(0x2000);
					const smvk::AppManifestAsset* mesh =
						manifest.FindAsset(0x3000);
					Check(manifest.assets.size() == 3
						&& scene && scene->kind == "scene"
						&& scene->pak == fs::path(rep.paks[1].output)
							.filename()
							.generic_string()
						&& scene->path == "content/scenes/grid.tour"
						&& tex && tex->kind == "texture"
						&& tex->pak == fs::path(rep.paks[0].output)
							.filename()
							.generic_string()
						&& tex->path
							== "content/models/grid/shared/stone.png"
						&& mesh && mesh->kind == "mesh"
						&& mesh->pak == fs::path(rep.paks[1].output)
							.filename()
							.generic_string()
						&& mesh->path
							== "content/models/grid/grid.geometry",
						"executor.manifest-id-table");
				}
				// The mesh id keys the DERIVED .geometry manifest guid
				// (the one identity the asset directory registers the
				// blob under — NOT the source glTF's guid; the commit
				// review's identity-split finding).
				{
					smvk::cache::AssetDatabase db;
					std::string dErr;
					const std::string regRoot =
						smvk::cache::EnsureRegistryRoot(
							project.DerivedDataDir());
					Check(db.Open(regRoot, project.root, dErr),
						"executor.ids-db-opens");
					if (db.IsOpen()) {
						smvk::assets::AssetManifest man;
						std::string mErr;
						Check(smvk::assets::LoadAssetManifest(
								(fs::path(project.root) / "content"
									/ "models" / "grid" / "grid.gltf")
									.lexically_normal()
									.generic_string(),
								man, mErr),
							"executor.mesh-manifest-reads");
						std::string derived;
						for (const auto& e : man.entries)
							if (e.file == "grid.geometry") {
								derived = e.guid;
								break;
							}
						Check(!derived.empty(),
							"executor.mesh-derived-guid-found");
						if (!derived.empty()) {
							uint64_t id = 0;
							std::string sym;
							Check(db.EnsureContentId(derived, "mesh",
									"content/models/grid/grid.geometry",
									id, sym, dErr)
								&& manifest.FindAsset(0x3000)
								&& id == manifest.FindAsset(0x3000)->id,
								"executor.mesh-id-keys-derived-guid");
						}
					}
				}
				bool paksExist = true;
				for (const std::string& pak : manifest.paks)
					if (!fs::is_regular_file(
							fs::path(rep.manifest).parent_path()
								/ pak, ec))
						paksExist = false;
				Check(paksExist, "executor.manifest-paks-exist");
				// RUN-04: a MOVED build directory keeps resolving —
				// copy the whole publish tree elsewhere, reload the
				// manifest there, every pak still exists beside it.
				{
					const fs::path moved = tmp / "publish4-moved";
					fs::remove_all(moved, ec);
					fs::copy(fs::path(rep.manifest).parent_path(), moved,
						fs::copy_options::recursive, ec);
					smvk::AppManifest movedM;
					std::string movedErr;
					Check(smvk::LoadAppManifest(
							(moved / "world.tapp").generic_string(),
							movedM, movedErr),
						"executor.manifest-moved-loads");
					bool movedPaks = true;
					for (const std::string& pak : movedM.paks)
						if (!fs::is_regular_file(moved / pak, ec))
							movedPaks = false;
					Check(movedPaks,
						"executor.manifest-moved-paks-resolve");
				}
			}
			if (rep.paks.size() == 2) {
				const auto open = [](const std::string& p) {
					std::ifstream in(p, std::ios::binary);
					std::vector<uint8_t> image(
						(std::istreambuf_iterator<char>(in)),
						std::istreambuf_iterator<char>());
					std::string verr;
					return smvk::PakReader::Open(
						std::span<const uint8_t>(image), verr);
				};
				auto shared = open(rep.paks[0].output);
				auto world = open(rep.paks[1].output);
				Check(bool(shared) && bool(world),
					"executor.two-pak-open-verify");
				if (shared && world) {
					const auto texPath =
						[&](const std::optional<smvk::PakReader>& r,
							const char* path) {
							for (const smvk::PakTocEntry* e : r
									->BlobsOfKind(
										smvk::PakAssetKind::Texture))
								if (e->path == path)
									return true;
							return false;
						};
					const bool sharedHas = texPath(shared,
						"content/models/grid/shared/stone.png");
					if (!sharedHas) {
						std::cout << "  (shared.pak textures:";
						for (const smvk::PakTocEntry* e : shared
								->BlobsOfKind(
									smvk::PakAssetKind::Texture))
							std::cout << " " << e->path;
						std::cout << ")\n";
					}
					Check(sharedHas, "executor.shared-embeds-texture");
					Check(!world
							->BlobsOfKind(smvk::PakAssetKind::Scene)
							.empty()
						&& !texPath(world,
							"content/models/grid/shared/stone.png"),
						"executor.world-excludes-shared-texture");
					Check(shared
							->BlobsOfKind(smvk::PakAssetKind::Scene)
							.empty(),
						"executor.asset-only-no-scene");
				}
			}
		}
	}

	// ---- the M2 GATE: end-to-end parity with the legacy build ----------
	// Legacy BuildProject (its own subprocess path) vs ExecuteBuildPlan
	// through the subprocess adapter — same project, same stages, same
	// flags (build-settings cubemap=off matches the recipe's 'bake
	// cubemaps off'). The paks must be byte-identical.
	{
		const fs::path dd =
			fs::path(project.DerivedDataDir());
		fs::create_directories(dd, ec);
		{
			std::ofstream f(dd / "build-settings.txt",
				std::ios::binary | std::ios::trunc);
			f << "cubemap=off\n";
		}
		smvk::project::BuildOptions bo;
		bo.scene = "content/scenes/grid.tour";
		bo.outputPak = (tmp / "legacy" / "grid.pak").generic_string();
		smvk::project::BuildReport br;
		std::string e;
		Check(smvk::project::BuildProject(project, bo, br, e),
			"executor.legacy-build-ok");
		auto sub = smvk::project::MakeSubprocessStageAdapter();
		smvk::project::BuildPlanOptions opts;
		opts.publishDir = (tmp / "publish2").generic_string();
		smvk::project::BuildPlanReport rep;
		Check(smvk::project::ExecuteBuildPlan(project, plan, *sub, opts,
				rep, e),
			"executor.plan-build-ok");
		if (br.pak.empty() || rep.paks.empty()) {
			Check(false, "executor.parity-bytes");
			if (!e.empty())
				std::cout << "  (plan err: " << e << ")\n";
		}
		else {
			const std::vector<char> legacy = ReadAllBytes(br.pak);
			const std::vector<char> planned =
				ReadAllBytes(rep.paks[0].output);
			Check(!legacy.empty() && legacy == planned,
				"executor.parity-bytes");
		}
		// ---- EDT-01: the editor's form (the linked-in Runs) builds the
		// SAME plan byte-identically — one pipeline, two adapters.
		{
			smvk::project::BuildPlanOptions opts;
			opts.publishDir = (tmp / "publish-inproc").generic_string();
			opts.manifestName = "grid";
			smvk::project::BuildPlanReport inRep;
			std::string ie;
			InProcessAdapter inProc;
			Check(smvk::project::ExecuteBuildPlan(project, plan, inProc,
					opts, inRep, ie),
				"executor.inproc-build-ok");
			if (!ie.empty())
				std::cout << "  (inproc err: " << ie << ")\n";
			if (!inRep.paks.empty() && !rep.paks.empty()) {
				const std::vector<char> inPak =
					ReadAllBytes(inRep.paks[0].output);
				const std::vector<char> subPak =
					ReadAllBytes(rep.paks[0].output);
				Check(!inPak.empty() && inPak == subPak,
					"executor.inproc-parity-bytes");
			}
			else {
				Check(false, "executor.inproc-parity-bytes");
			}
		}
		// ---- EDT-02: the unsaved-scene build — the plan RESOLVES against
		// the authored doc; every STAGE runs the SNAPSHOT; the authored
		// document is never touched, and identical snapshot content
		// builds the identical pak.
		{
			const fs::path authored = scenes / "grid.tour";
			const std::vector<char> before = ReadAllBytes(authored);
			const fs::path snapshot = tmp / "grid-snapshot.tour";
			std::error_code cec;
			fs::copy_file(authored, snapshot,
				fs::copy_options::overwrite_existing, cec);
			smvk::project::BuildPlanOptions opts;
			opts.publishDir = (tmp / "publish-snap").generic_string();
			opts.sceneSnapshot = snapshot.generic_string();
			smvk::project::BuildPlanReport snapRep;
			std::string se;
			InProcessAdapter inProc;
			Check(smvk::project::ExecuteBuildPlan(project, plan, inProc,
					opts, snapRep, se),
				"executor.snapshot-build-ok");
			if (!se.empty())
				std::cout << "  (snapshot err: " << se << ")\n";
			Check(ReadAllBytes(authored) == before,
				"executor.snapshot-never-touches-authored");
			if (!snapRep.paks.empty() && !rep.paks.empty()) {
				Check(ReadAllBytes(snapRep.paks[0].output)
						== ReadAllBytes(rep.paks[0].output),
					"executor.snapshot-same-content-same-pak");
			}
			else {
				Check(false, "executor.snapshot-same-content-same-pak");
			}
		}
	}
}

// ---- the content-id registry + the generated project.h ---------------

void TestProjectCatalog() {
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pp_catalog";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);

	// The registry: assets.db under the project's derived-data home.
	smvk::cache::AssetDatabase db;
	{
		const fs::path dd = tmp / "proj" / ".tourmaline";
		bool created = false;
		std::string err;
		smvk::project::Project p = smvk::project::Project::Init(
			(tmp / "proj").generic_string(), "CatalogTest", created, err);
		Check(p.Valid() && created, "catalog.project-created");
		const std::string root =
			smvk::cache::EnsureRegistryRoot(dd.generic_string());
		Check(!root.empty() && db.Open(root, p.root, err),
			"catalog.db-opens");
	}
	// Stability: ensure -> re-ensure -> rename keeps id + symbol.
	{
		uint64_t id = 0, id2 = 0;
		std::string sym, sym2, err;
		Check(db.EnsureContentId(
				"11111111111111111111111111111111", "scene",
				"content/scenes/a.tour", id, sym, err)
			&& id >= 0x1000 && id <= 0x1FFF && sym == "a",
			"catalog.ensure-allocates");
		Check(db.EnsureContentId(
				"11111111111111111111111111111111", "scene",
				"content/scenes/a.tour", id2, sym2, err)
			&& id2 == id && sym2 == "a",
			"catalog.ensure-idempotent");
		Check(db.EnsureContentId(
				"11111111111111111111111111111111", "scene",
				"content/scenes/renamed.tour", id2, sym2, err)
			&& id2 == id && sym2 == "a",
			"catalog.rename-keeps-id-and-symbol");
	}
	// Collision: the same stem in one kind suffixes (_2), stably.
	{
		uint64_t id = 0, id2 = 0;
		std::string sym, sym2, err;
		Check(db.EnsureContentId(
				"22222222222222222222222222222222", "texture",
				"content/t/a.png", id, sym, err)
			&& sym == "a",
			"catalog.collision-first");
		Check(db.EnsureContentId(
				"33333333333333333333333333333333", "texture",
				"content/other/a.png", id2, sym2, err)
			&& sym2 == "a_2" && id2 > id,
			"catalog.collision-suffixed");
	}
	// Tombstones: deactivate -> the id is RESERVED (a new asset never
	// takes it) -> re-ensure re-activates with the same identity.
	{
		std::string err;
		Check(db.SetContentIdActive(
				"33333333333333333333333333333333", false, err),
			"catalog.tombstone");
		std::vector<smvk::cache::ContentIdRecord> rows;
		Check(db.ListContentIds(rows, err)
			&& std::any_of(rows.begin(), rows.end(),
				[](const smvk::cache::ContentIdRecord& r) {
					return r.guid == "33333333333333333333333333333333"
						&& !r.active;
				}),
			"catalog.tombstone-listed");
		uint64_t id = 0, idNew = 0;
		std::string s, sNew;
		Check(db.EnsureContentId(
				"33333333333333333333333333333333", "texture",
				"content/other/a.png", id, s, err)
			&& s == "a_2",
			"catalog.tombstone-id-kept");
		Check(db.EnsureContentId(
				"44444444444444444444444444444444", "texture",
				"content/t/b.png", idNew, sNew, err)
			&& idNew != id && idNew > id,
			"catalog.tombstone-id-never-reused");
	}

	// ---- the catalog generation on a mini project ---------------------
	smvk::project::Project project;
	{
		std::string err;
		project = smvk::project::Project::Open(
			(tmp / "proj").generic_string(), err);
	}
	const fs::path models = fs::path(project.root) / "content" / "models"
		/ "thing";
	const fs::path scenes = fs::path(project.root) / "content" / "scenes";
	fs::create_directories(models, ec);
	fs::create_directories(scenes, ec);
	{ // a dummy .geometry (existence is all the resolver needs) + a
	  // texture (content only — the ids path never decodes it)
		std::ofstream f(models / "thing.geometry",
			std::ios::binary | std::ios::trunc);
		f << "geo";
		std::ofstream t(models / "stone.png",
			std::ios::binary | std::ios::trunc);
		t << "png";
		smvk::tour::Doc doc;
		smvk::tour::ReferenceCmd root;
		root.source = "content/models/thing/thing.geometry";
		root.name = "ROOT";
		root.root = true;
		doc.references.push_back(root);
		std::string err;
		Check(smvk::tour::SaveTour(
				(scenes / "thing.tour").generic_string(), doc, err),
			"catalog.tour-saved");
	}
	const std::string recipeText =
		"recipe 1\n"
		"project CatalogTest\n"
		"entry content/scenes/thing.tour\n"
		"[pak world]\n"
		"output thing.pak\n"
		"scene content/scenes/thing.tour\n";
	smvk::project::ProjectRecipe recipe;
	smvk::project::RecipeDiagnostic diag;
	Check(smvk::project::ParseProjectRecipeText(recipeText, recipe, diag),
		"catalog.recipe-parses");
	smvk::project::BuildPlan plan;
	std::string err;
	Check(smvk::project::ResolveRecipe(project, recipe, plan, err),
		"catalog.plan-resolves");

	smvk::project::ProjectCatalog catalog;
	std::vector<std::string> deps;
	Check(smvk::project::GenerateProjectCatalog(project, plan, db,
			catalog, deps, err),
		"catalog.generates");
	Check(catalog.scenes.size() == 1 && catalog.meshes.size() == 1
			&& catalog.textures.empty() // no includes in this recipe
			&& catalog.entryScene == catalog.scenes[0].id
			&& catalog.scenes[0].id >= 0x1000
			&& catalog.meshes[0].id >= 0x3000,
		"catalog.namespaces");
	// (the texture namespace fills from the include-claiming plan below)

	smvk::project::GenerationStamp stamp;
	stamp.generation = 7;
	stamp.timestamp = 1793451600;
	stamp.contentHash =
		smvk::project::ComputeCatalogContentHash(catalog);
	const std::string header =
		smvk::project::EmitProjectHeader(catalog, stamp, {}, {});
	Check(header.find("namespace scenes") != std::string::npos
			&& header.find("EntryScene = scenes::thing")
				!= std::string::npos
			&& header.find("namespace meshes") != std::string::npos
			&& header.find("Assets = {") != std::string::npos
			&& header.find("AssetKind::Scene") != std::string::npos
			&& header.find("AssetKind::Mesh") != std::string::npos
			&& header.find("constexpr const AssetDesc* Find")
				!= std::string::npos
			&& header.find("CatalogFingerprint = 0x")
				!= std::string::npos
			&& header.find("thing.pak") != std::string::npos,
		"catalog.header-content");
	// Deterministic + write-only-on-change.
	{
		const std::string again =
			smvk::project::EmitProjectHeader(catalog, stamp, {}, {});
		Check(again == header, "catalog.header-deterministic");
		const fs::path file = tmp / "project.h";
		bool wrote = false;
		Check(smvk::project::WriteProjectHeaderIfChanged(
				file.generic_string(), header, wrote, err)
			&& wrote,
			"catalog.header-writes");
		wrote = true;
		Check(smvk::project::WriteProjectHeaderIfChanged(
				file.generic_string(), header, wrote, err)
			&& !wrote,
			"catalog.header-unchanged-skip");
	}
	// The tombstone sweep: a plan WITHOUT the mesh (the recipe drops
	// the scene) tombstones its row; re-adding re-activates the SAME
	// id (the header text returns identical).
	{
		smvk::project::ProjectCatalog shrunk;
		std::vector<std::string> deps2;
		// A texture-only recipe (the include claims the png; no scene
		// pak): the scene + mesh rows tombstone.
		const std::string textOnly =
			"recipe 1\n"
			"project CatalogTest\n"
			"entry content/scenes/thing.tour\n"
			"[pak shared]\n"
			"output shared.pak\n"
			"include content/models/thing/*.png\n"
			"[pak world]\n"
			"output thing.pak\n"
			"scene content/scenes/thing.tour\n";
		smvk::project::ProjectRecipe r2;
		smvk::project::RecipeDiagnostic d2;
		smvk::project::BuildPlan p2;
		Check(smvk::project::ParseProjectRecipeText(textOnly, r2, d2)
			&& smvk::project::ResolveRecipe(project, r2, p2, err),
			"catalog.texture-plan-resolves");
		Check(smvk::project::GenerateProjectCatalog(project, p2, db,
				shrunk, deps2, err),
			"catalog.texture-plan-generates");
		Check(shrunk.textures.size() == 1 && shrunk.meshes.size() == 1,
			"catalog.texture-plan-namespaces");
		// Regenerate the ORIGINAL plan (no includes): the texture row
		// tombstones, every other row re-activates with the SAME ids —
		// the header text returns identical.
		smvk::project::ProjectCatalog round;
		std::vector<std::string> deps3;
		Check(smvk::project::GenerateProjectCatalog(project, plan, db,
				round, deps3, err)
			&& smvk::project::EmitProjectHeader(round, stamp, {}, {})
				== header,
			"catalog.reactivate-same-ids");
	}

	// ---- the commit-review regressions (identity + symbol safety) ---
	// A guid identifying two kinds is an IDENTITY INVARIANT violation —
	// loud, never a silent cross-namespace id.
	{
		uint64_t id = 0;
		std::string sym, err2;
		Check(db.EnsureContentId(
				"55555555555555555555555555555555", "texture",
				"content/t/x.png", id, sym, err2),
			"catalog.kind-mismatch-setup");
		Check(!db.EnsureContentId(
				"55555555555555555555555555555555", "mesh",
				"content/m/x.geometry", id, sym, err2)
			&& err2.find("identity inconsistency")
				!= std::string::npos,
			"catalog.kind-mismatch-rejected");
	}
	// C++ keywords get a safe '_' prefix (class.png -> _class, never
	// the invalid project::textures::class) — the COMPLETE set now:
	// the digraph operators (compl, bitand) and char8_t that the first
	// cut missed.
	{
		uint64_t id = 0;
		std::string sym, err2;
		Check(db.EnsureContentId(
				"66666666666666666666666666666666", "texture",
				"content/t/class.png", id, sym, err2)
			&& sym == "_class",
			"catalog.keyword-symbol-safe");
		for (const char* name : { "compl", "bitand", "char8_t",
				"or_eq", "xor_eq", "not_eq", "and_eq", "bitor" }) {
			const std::string path =
				std::string("content/t/") + name + ".png";
			const std::string want = std::string("_") + name;
			// A DISTINCT guid per name (one guid = one asset; reusing
			// it returns the first row). 30 sevens + a 2-hex counter
			// = 32 hex chars.
			static int s_n = 0;
			++s_n;
			char guidBuf[33];
			std::snprintf(guidBuf, sizeof(guidBuf),
				"777777777777777777777777777777%02x", s_n);
			Check(db.EnsureContentId(guidBuf, "texture", path, id, sym,
					err2)
				&& sym == want,
				(std::string("catalog.keyword-digraph.") + name).c_str());
		}
	}
	// RESERVED identifiers normalize: '__' ANYWHERE folds to '_u_'
	// (std reserved), a leading '_'+uppercase inserts 'u'.
	{
		uint64_t id = 0;
		std::string sym, err2;
		Check(db.EnsureContentId(
				"88888888888888888888888888888888", "texture",
				"content/t/__stone.png", id, sym, err2)
			&& sym == "_u_stone"
			&& db.EnsureContentId(
				"88888888888888888888888888888889", "texture",
				"content/t/_Stone.png", id, sym, err2)
			&& sym == "_uStone",
			"catalog.reserved-normalized");
	}
	// Structural validation of the emitted header: every generated
	// symbol line is a valid, non-reserved, non-keyword identifier.
	// The entries carry DB-SANITIZED symbols (what SymbolFromPath
	// produces for class.png/__under.png/compl.png) — the lexical pass
	// must accept all of them.
	{
		smvk::project::ProjectCatalog c;
		c.projectName = "StructTest";
		c.scenes.push_back({ 0x1000, "_class", "content/s/a.tour",
			"world" });
		c.scenes.push_back({ 0x1001, "_u_under", "content/s/b.tour",
			"world" });
		c.textures.push_back({ 0x2000, "_compl", "content/t/c.png",
			"world" });
		smvk::project::GenerationStamp st;
		st.generation = 1;
		st.timestamp = 1793451600;
		st.contentHash = smvk::project::ComputeCatalogContentHash(c);
		const std::string text =
			smvk::project::EmitProjectHeader(c, st, {}, {});
		// The typed-key declarations (SceneKey/TextureKey/... beside
		// the raw Id ProjectId) — the symbol is the SECOND token.
		const std::regex symLine(
			"\\s*inline constexpr (?:Id|SceneKey|TextureKey|MeshKey|"
			"SoundKey) ([A-Za-z_][A-Za-z0-9_]*)[ ={]");
		std::smatch match;
		std::string::const_iterator at = text.begin();
		int symbols = 0, valid = 0;
		static const char* kKw[] = { "class", "compl", "int", "struct",
			"template", "char8_t", "bitand" };
		while (std::regex_search(at, text.end(), match, symLine)) {
			++symbols;
			const std::string sym = match[1].str();
			bool ok = sym.find("__") == std::string::npos
				&& !(sym.size() >= 2 && sym[0] == '_'
					&& sym[1] >= 'A' && sym[1] <= 'Z');
			for (const char* kw : kKw)
				if (sym == kw)
					ok = false;
			if (ok)
				++valid;
			at = match.suffix().first;
		}
		// ProjectId + the 3 catalog entries (EntryScene aliases an
		// entry symbol, not a fresh declaration of this shape).
		Check(symbols == 4 && valid == 4,
			"catalog.header-symbols-lexically-valid");
		if (symbols != 4 || valid != 4)
			std::cout << "  (lexical: symbols=" << symbols
				<< " valid=" << valid << ")\n";
		// The FLAT TYPED CATALOG (the compiled-catalog contract): the
		// AssetKind enum (the ABI values), AssetDesc rows carrying
		// kind + pakIndex (the LOGICAL package index - not the pak
		// NAME string the old Catalog array carried), the constexpr
		// Find, and CatalogFingerprint == the shared fold.
		{
			Check(text.find("enum class AssetKind")
					!= std::string::npos
				&& text.find("struct AssetDesc")
					!= std::string::npos
				&& text.find(
					"AssetDesc{scenes::_class, AssetKind::Scene, 0,")
					!= std::string::npos
				&& text.find(
					"AssetDesc{textures::_compl, AssetKind::Texture,")
					!= std::string::npos
				&& text.find(
					"constexpr const AssetDesc* Find(Id id)")
					!= std::string::npos,
				"catalog.header-flat-table");
			char fp[40];
			std::snprintf(fp, sizeof(fp), "0x%llx",
				static_cast<unsigned long long>(
					smvk::project::CatalogFingerprintOf(c)));
			const std::string want =
				"inline constexpr std::uint64_t CatalogFingerprint = "
				+ std::string(fp);
			Check(text.find(want) != std::string::npos,
				"catalog.header-fingerprint-emitted");
		}
	}
	// Identity acquisition ADOPTS: the guid has a row in the ASSET
	// DIRECTORY too (one database, both tables agree — the mint never
	// leaves content_ids without an assets row).
	{
		std::string found;
		Check(db.GuidForPath(
				(fs::path(project.root) / "content" / "models" / "thing"
					/ "thing.geometry").lexically_normal()
					.generic_string(),
				found)
			&& !found.empty(),
			"catalog.identity-adopted");
	}
	// A MALFORMED sidecar FAILS the generation WITHOUT touching it —
	// identity repair is explicit (auto-re-minting would orphan every
	// content id keyed on the old guid).
	{
		const fs::path evil = fs::path(project.ImportsDir())
			/ "thing" / "evil.png";
		{
			std::ofstream f(evil, std::ios::binary | std::ios::trunc);
			f << "png";
			std::ofstream m(fs::path(evil).string() + ".meta",
				std::ios::binary | std::ios::trunc);
			m << "guid=not-a-guid\n";
		}
		const std::string evilRecipe =
			"recipe 1\n"
			"project CatalogTest\n"
			"entry content/scenes/thing.tour\n"
			"[pak world]\n"
			"output thing.pak\n"
			"scene content/scenes/thing.tour\n"
			"[pak shared]\n"
			"output shared.pak\n"
			"include content/models/thing/evil.png\n";
		smvk::project::ProjectRecipe r3;
		smvk::project::RecipeDiagnostic d3;
		smvk::project::BuildPlan p3;
		std::string e3;
		Check(smvk::project::ParseProjectRecipeText(evilRecipe, r3, d3)
			&& smvk::project::ResolveRecipe(project, r3, p3, e3),
			"catalog.malformed-setup-resolves");
		smvk::project::ProjectCatalog c3;
		std::vector<std::string> deps3b;
		Check(!smvk::project::GenerateProjectCatalog(project, p3, db,
				c3, deps3b, e3)
			&& e3.find("REFUSING to auto-replace")
				!= std::string::npos,
			"catalog.malformed-sidecar-refuses");
		// The malformed sidecar is UNTOUCHED (repair stays explicit).
		{
			std::ifstream m(fs::path(evil).string() + ".meta");
			std::string text((std::istreambuf_iterator<char>(m)),
				std::istreambuf_iterator<char>());
			Check(text.find("not-a-guid") != std::string::npos,
				"catalog.malformed-sidecar-untouched");
		}
	}
	// String-literal escaping: quotes/backslashes in names/paths never
	// break the emitted header.
	{
		smvk::project::ProjectCatalog c;
		c.projectName = "We\"ird";
		c.paks = { { "wo\"rld", "wor\\ld.pak" } };
		smvk::project::GenerationStamp st;
		st.generation = 1;
		st.timestamp = 1793451600;
		st.contentHash = smvk::project::ComputeCatalogContentHash(c);
		const std::string text =
			smvk::project::EmitProjectHeader(c, st, {}, {});
		Check(text.find("Name = \"We\\\"ird\"") != std::string::npos
			&& text.find("Pak{\"wo\\\"rld\", \"wor\\\\ld.pak\"}")
				!= std::string::npos,
			"catalog.header-escapes-literals");
	}
}

// The GENERATION STAMP + the EOF metadata (project.h as its own build
// artifact): the stamp constants, the machine-readable comment's round
// trip, the bump policy (ComposeProjectHeader — the split-events
// contract: unchanged = byte-identical no-op, any change or --stamp =
// a new generation, fingerprint-only changes keep the content hash),
// FingerprintFile's VERIFIED content hashes (metadata never proves
// byte equality), and the pak cook-state rules.
void TestProjectHeaderStamp() {
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pp_stamp";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);

	// ---- a hand-built catalog: stamp + EOF round trip ---------------
	smvk::project::ProjectCatalog c;
	c.projectName = "StampTest";
	c.projectId = 0xA1007777;
	c.paks = { { "world", "thing.pak" } };
	c.scenes.push_back({ 0x1000, "thing", "content/scenes/thing.tour",
		"world" });
	c.entryScene = 0x1000;
	std::vector<smvk::project::AssetFingerprint> assets;
	{
		smvk::project::AssetFingerprint af;
		af.id = 0x1000;
		af.path = "content/scenes/thing.tour";
		af.pak = "world";
		af.mtime = 1793451000;
		af.size = 124892;
		af.hash = "fnv1a64:5ae71b30deadbee0";
		assets.push_back(af);
	}
	std::vector<smvk::project::PakFingerprint> paks;
	{
		smvk::project::PakFingerprint pf;
		pf.file = "thing-3ac9ef10deadbeef.pak";
		pf.mtime = 1793451500;
		pf.size = 48291824;
		pf.hash = "fnv1a64:3ac9ef10deadbeef";
		pf.state = "verified";
		paks.push_back(pf);
	}
	smvk::project::GenerationStamp stamp;
	stamp.generation = 42;
	stamp.timestamp = 1793451600;
	stamp.contentHash = smvk::project::ComputeCatalogContentHash(c);
	const std::string text =
		smvk::project::EmitProjectHeader(c, stamp, assets, paks);
	Check(text.find(
			"inline constexpr std::uint64_t Generation = 42ULL")
			!= std::string::npos
		&& text.find("GeneratedAt = 1793451600ULL") != std::string::npos
		&& text.find("ContentHash = \"" + stamp.contentHash + "\"")
			!= std::string::npos
		&& text.find("TOURMALINE-METADATA-BEGIN 1") != std::string::npos
		&& text.find("TOURMALINE-METADATA-END 1") != std::string::npos
		&& text.find("state=verified") != std::string::npos
		&& text.find("mtime_s=1793451000") != std::string::npos,
		"stamp.header-carries-stamp");
	Check(smvk::project::EmitProjectHeader(c, stamp, assets, paks)
			== text,
		"stamp.header-deterministic");
	{
		const fs::path file = tmp / "roundtrip.h";
		bool wrote = false;
		std::string err;
		Check(smvk::project::WriteProjectHeaderIfChanged(
				file.generic_string(), text, wrote, err)
			&& wrote,
			"stamp.roundtrip-writes");
		smvk::project::HeaderMetadata meta;
		Check(smvk::project::ReadProjectHeaderMetadata(
				file.generic_string(), meta, err)
			&& meta.present && meta.generation == 42
			&& meta.timestamp == 1793451600
			&& meta.contentHash == stamp.contentHash,
			"stamp.roundtrip-stamp");
		Check(meta.assets.size() == 1 && meta.assets.count(0x1000) == 1
			&& meta.assets.at(0x1000).hash == assets[0].hash
			&& meta.assets.at(0x1000).mtime == assets[0].mtime
			&& meta.assets.at(0x1000).size == assets[0].size
			&& meta.assets.at(0x1000).path == assets[0].path
			&& meta.assets.at(0x1000).pak == "world",
			"stamp.roundtrip-asset");
		Check(meta.paks.size() == 1
			&& meta.paks.count(paks[0].file) == 1
			&& meta.paks.at(paks[0].file).state == "verified"
			&& meta.paks.at(paks[0].file).hash == paks[0].hash
			&& meta.paks.at(paks[0].file).mtime == paks[0].mtime,
			"stamp.roundtrip-pak");
	}
	// An unknown metadata version (and a missing file) read as ABSENT —
	// a fresh scan, never a stale-cache reuse.
	{
		smvk::project::HeaderMetadata meta;
		std::string err;
		const fs::path v99 = tmp / "v99.h";
		{
			std::ofstream f(v99, std::ios::binary | std::ios::trunc);
			f << "// TOURMALINE-METADATA-BEGIN 99\n"
				<< "// generator generation=9 timestamp=9\n"
				<< "// TOURMALINE-METADATA-END 99\n";
		}
		Check(smvk::project::ReadProjectHeaderMetadata(
				v99.generic_string(), meta, err)
			&& !meta.present && meta.generation == 0,
			"stamp.unknown-version-reads-absent");
		Check(smvk::project::ReadProjectHeaderMetadata(
				(tmp / "none.h").generic_string(), meta, err)
			&& !meta.present,
			"stamp.missing-file-reads-absent");
	}

	// ---- the bump policy (ComposeProjectHeader) ---------------------
	const fs::path gen = tmp / "gen" / "project.h";
	fs::create_directories(gen.parent_path(), ec);
	std::string t;
	smvk::project::GenerationStamp st;
	bool changed = false;
	bool wrote = false;
	std::string err;
	// First generation (no previous header): generation 1.
	smvk::project::ComposeProjectHeader(c, assets, paks,
		gen.generic_string(), false, t, st, changed);
	Check(changed && st.generation == 1
			&& st.contentHash == stamp.contentHash,
		"stamp.first-generation");
	Check(smvk::project::WriteProjectHeaderIfChanged(
			gen.generic_string(), t, wrote, err)
		&& wrote,
		"stamp.first-writes");
	const std::string first = t;
	// Unchanged re-compose: byte-identical text, generation stays, the
	// file never moves (the build-incremental contract).
	smvk::project::ComposeProjectHeader(c, assets, paks,
		gen.generic_string(), false, t, st, changed);
	Check(!changed && t == first && st.generation == 1,
		"stamp.unchanged-noop");
	wrote = true;
	Check(smvk::project::WriteProjectHeaderIfChanged(
			gen.generic_string(), t, wrote, err)
		&& !wrote,
		"stamp.unchanged-skip");
	// Forced (--stamp / the <Name>_stamp target): a NEW generation
	// every run, changed content or not.
	smvk::project::ComposeProjectHeader(c, assets, paks,
		gen.generic_string(), true, t, st, changed);
	Check(changed && st.generation == 2
			&& st.contentHash == stamp.contentHash,
		"stamp.forced-bumps");
	Check(smvk::project::WriteProjectHeaderIfChanged(
			gen.generic_string(), t, wrote, err)
		&& wrote,
		"stamp.forced-writes");
	// A fingerprint-only change (the texture repixel): the generation
	// bumps (the EOF moved) but the CONTENT HASH stays — the catalog
	// semantics did not change.
	std::vector<smvk::project::AssetFingerprint> repixel = assets;
	repixel[0].hash = "fnv1a64:ffffffffffffffff";
	repixel[0].mtime = 1793451999;
	repixel[0].size = 124893;
	smvk::project::ComposeProjectHeader(c, repixel, paks,
		gen.generic_string(), false, t, st, changed);
	Check(changed && st.generation == 3
			&& st.contentHash == stamp.contentHash,
		"stamp.repixel-keeps-content-hash");
	Check(smvk::project::WriteProjectHeaderIfChanged(
			gen.generic_string(), t, wrote, err)
		&& wrote,
		"stamp.repixel-writes");
	// A catalog content change: the generation bumps AND the content
	// hash moves.
	smvk::project::ProjectCatalog c2 = c;
	c2.textures.push_back({ 0x2000, "stone", "content/t/stone.png",
		"world" });
	std::vector<smvk::project::AssetFingerprint> assets2 = repixel;
	{
		smvk::project::AssetFingerprint af2;
		af2.id = 0x2000;
		af2.path = "content/t/stone.png";
		af2.pak = "world";
		af2.mtime = 1793451700;
		af2.size = 819200;
		af2.hash = "fnv1a64:0123456789abcdef";
		assets2.push_back(af2);
	}
	smvk::project::ComposeProjectHeader(c2, assets2, paks,
		gen.generic_string(), false, t, st, changed);
	Check(changed && st.generation == 4
			&& st.contentHash != stamp.contentHash,
		"stamp.content-change-moves-hash");

	// ---- FingerprintFile: VERIFIED content hashes -------------------
	{
		const fs::path src = tmp / "fingerprint.bin";
		{
			std::ofstream f(src, std::ios::binary | std::ios::trunc);
			f << "hello stamp";
		}
		int64_t m = -1;
		uint64_t sz = 0;
		std::string h, fErr;
		Check(smvk::project::FingerprintFile(src.generic_string(),
				m, sz, h, fErr)
			&& m > 0 && sz == 11
			&& h.find("fnv1a64:") == 0,
			"stamp.fingerprint-hashes");
		const std::string h1 = h;
		const int64_t m1 = m;
		const uint64_t sz1 = sz;
		const auto t1 = fs::last_write_time(src, ec);
		// Unchanged bytes: the recomputed hash matches (determinism).
		std::string h2;
		Check(smvk::project::FingerprintFile(src.generic_string(),
				m, sz, h2, fErr)
			&& h2 == h1,
			"stamp.fingerprint-stable");
		// THE PRESERVED-TIMESTAMP SAME-SIZE EDIT: different bytes, the
		// SAME size, the mtime RESTORED to the byte — matching
		// metadata must NEVER read as unchanged bytes (the mtime+size
		// carry this replaces would have carried h1 forward here).
		{
			std::ofstream f(src, std::ios::binary | std::ios::trunc);
			f << "hellp stamp"; // 11 bytes again, one byte differs
		}
		fs::last_write_time(src, t1, ec);
		Check(smvk::project::FingerprintFile(src.generic_string(),
				m, sz, h2, fErr)
			&& m == m1 && sz == sz1 && h2 != h1,
			"stamp.fingerprint-detects-preserved-timestamp-edit");
		// A backdated different-size edit: detected too (trivially —
		// the hash is always from bytes).
		{
			std::ofstream f(src, std::ios::binary | std::ios::trunc);
			f << "hello stamp!";
		}
		fs::last_write_time(src,
			fs::file_time_type::clock::now()
				- std::chrono::seconds(10000), ec);
		Check(smvk::project::FingerprintFile(src.generic_string(),
				m, sz, h2, fErr)
			&& m < m1 && sz == 12 && h2 != h1,
			"stamp.fingerprint-detects-backdated-edit");
		Check(smvk::project::FingerprintFile(
				(tmp / "nope.bin").generic_string(), m, sz, h2, fErr)
			&& m == -1 && sz == 0 && h2.empty(),
			"stamp.fingerprint-missing");
	}

	// ---- DiscoverPakFingerprints: the cook-state rules --------------
	{
		const fs::path dir = tmp / "pakdir";
		fs::create_directories(dir, ec);
		smvk::project::ProjectCatalog pc;
		pc.projectName = "PakStateTest";
		pc.paks = { { "world", "thing.pak" } };
		pc.scenes.push_back({ 0x1000, "thing",
			"content/scenes/thing.tour", "world" });
		pc.entryScene = 0x1000;
		// The SOURCE: backdated far past the pak (rule (c)'s witness).
		const fs::path srcRoot = tmp / "srctest";
		fs::create_directories(srcRoot / "content" / "scenes", ec);
		const fs::path src =
			srcRoot / "content" / "scenes" / "thing.tour";
		{
			// (The stream CLOSES before the backdate — its destructor
			// flush would re-stamp the mtime to now.)
			std::ofstream f(src, std::ios::binary | std::ios::trunc);
			f << "tour";
		}
		fs::last_write_time(src,
			fs::file_time_type::clock::now()
				- std::chrono::seconds(10000), ec);
		smvk::project::AssetFingerprint cur;
		cur.id = 0x1000;
		cur.path = "content/scenes/thing.tour";
		cur.pak = "world";
		{
			std::string fErr;
			Check(smvk::project::FingerprintFile(
					src.generic_string(), cur.mtime, cur.size, cur.hash,
					fErr),
				"stamp.pak-source-fingerprinted");
		}
		const std::vector<smvk::project::AssetFingerprint> curs =
			{ cur };
		// Empty dir: nothing published -> missing.
		{
			std::vector<smvk::project::PakFingerprint> out;
			std::string dErr;
			Check(smvk::project::DiscoverPakFingerprints(
					dir.generic_string(), pc, curs,
					smvk::project::HeaderMetadata{}, out, dErr)
				&& out.size() == 1 && out[0].state == "missing",
				"stamp.pak-state-missing");
		}
		// Publish: the content-addressed pak (backdated, but NEWER
		// than the source) + the manifest whose [ids] row joins the
		// stable id to the pak FILE.
		const std::string pakName = "thing-0123456789abcdef.pak";
		{
			// (Close BEFORE the backdate — see the source above.)
			std::ofstream f(dir / pakName,
				std::ios::binary | std::ios::trunc);
			f << "pakbytes";
		}
		fs::last_write_time(dir / pakName,
			fs::file_time_type::clock::now()
				- std::chrono::seconds(5000), ec);
		{
			smvk::AppManifest m;
			m.version = smvk::kAppManifestVersion;
			m.entryPak = pakName;
			m.entryScene = "thing";
			m.paks.push_back(pakName);
			smvk::AppManifestAsset row;
			row.id = 0x1000;
			row.kind = "scene";
			row.pak = pakName;
			row.path = "content/scenes/thing.tour";
			row.name = "thing";
			m.assets.push_back(row);
			std::string mErr;
			Check(smvk::SaveAppManifest(
					(dir / "thing.tapp").generic_string(), m, mErr),
				"stamp.pak-manifest-saved");
		}
		// A first-ever run (no previous metadata): stale — it claims
		// nothing (only the recipe build's re-stamp verifies).
		{
			std::vector<smvk::project::PakFingerprint> out;
			std::string dErr;
			Check(smvk::project::DiscoverPakFingerprints(
					dir.generic_string(), pc, curs,
					smvk::project::HeaderMetadata{}, out, dErr)
				&& out.size() == 1 && out[0].state == "stale"
				&& out[0].file == pakName
				&& !out[0].hash.empty(),
				"stamp.pak-first-run-stale");
		}
		// The matching previous record: verified, preserved.
		{
			smvk::project::HeaderMetadata prev;
			prev.present = true;
			prev.assets[0x1000] = cur;
			std::vector<smvk::project::PakFingerprint> out;
			std::string dErr;
			Check(smvk::project::DiscoverPakFingerprints(
					dir.generic_string(), pc, curs, prev, out, dErr)
				&& out[0].state == "verified",
				"stamp.pak-verified-preserved");
		}
		// A source hash change vs the previous record: stale.
		{
			smvk::project::HeaderMetadata prev;
			prev.present = true;
			smvk::project::AssetFingerprint old = cur;
			old.hash = "fnv1a64:aaaaaaaaaaaaaaaa";
			prev.assets[0x1000] = old;
			std::vector<smvk::project::PakFingerprint> out;
			std::string dErr;
			Check(smvk::project::DiscoverPakFingerprints(
					dir.generic_string(), pc, curs, prev, out, dErr)
				&& out[0].state == "stale",
				"stamp.pak-source-change-stale");
		}
		// A source NEWER than the pak: stale (rule (c)) — even with a
		// MATCHING previous record (the mtime is the witness).
		{
			smvk::project::AssetFingerprint touched = cur;
			{
				std::ofstream f(src, std::ios::binary | std::ios::trunc);
				f << "tour2"; // mtime = now > the backdated pak
				std::string fErr;
				Check(smvk::project::FingerprintFile(
						src.generic_string(), touched.mtime,
						touched.size, touched.hash, fErr),
					"stamp.pak-touched-refingerprinted");
			}
			smvk::project::HeaderMetadata prev;
			prev.present = true;
			prev.assets[0x1000] = touched;
			const std::vector<smvk::project::AssetFingerprint> ts =
				{ touched };
			std::vector<smvk::project::PakFingerprint> out;
			std::string dErr;
			Check(smvk::project::DiscoverPakFingerprints(
					dir.generic_string(), pc, ts, prev, out, dErr)
				&& out[0].state == "stale",
				"stamp.pak-source-newer-stale");
		}
		// An UNPUBLISHED asset (no manifest row): stale — the pak
		// cannot contain it.
		{
			smvk::project::ProjectCatalog pc2 = pc;
			pc2.textures.push_back({ 0x2000, "stone",
				"content/t/stone.png", "world" });
			smvk::project::AssetFingerprint stone;
			stone.id = 0x2000;
			stone.path = "content/t/stone.png";
			stone.pak = "world";
			stone.mtime = 1000;
			stone.size = 8;
			stone.hash = "fnv1a64:0123456789abcdef";
			std::vector<smvk::project::AssetFingerprint> both = curs;
			both.push_back(stone);
			smvk::project::HeaderMetadata prev;
			prev.present = true;
			prev.assets[0x1000] = cur;
			prev.assets[0x2000] = stone;
			std::vector<smvk::project::PakFingerprint> out;
			std::string dErr;
			Check(smvk::project::DiscoverPakFingerprints(
					dir.generic_string(), pc2, both, prev, out, dErr)
				&& out.size() == 1 && out[0].state == "stale",
				"stamp.pak-unpublished-stale");
		}
		// The pak file vanished (the manifest still names it): missing.
		{
			fs::remove(dir / pakName, ec);
			smvk::project::HeaderMetadata prev;
			prev.present = true;
			prev.assets[0x1000] = cur;
			std::vector<smvk::project::PakFingerprint> out;
			std::string dErr;
			Check(smvk::project::DiscoverPakFingerprints(
					dir.generic_string(), pc, curs, prev, out, dErr)
				&& out.size() == 1 && out[0].state == "missing",
				"stamp.pak-file-vanished-missing");
		}
	}
}

} // namespace

int main() {
	// Unbuffered streams: printf and std::cout carry SEPARATE buffers, so
	// a crash would otherwise scramble the log order (the last output
	// before an AV is lost/deferred). Tests are small — pay the flush.
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	std::cout << std::unitbuf;
	TestProjectModel();
	TestImportLifecycle();
	TestFingerprint();
	TestSubassetIdentity();
	TestRecipeParsing();
	TestRecipeGlob();
	TestRecipeResolution();
	TestAppManifest();
	TestProjectCatalog();
	TestProjectHeaderStamp();
	TestBuildPlanExecutor();
	std::cout << "project_pipeline_tests: "
		<< (g_checks - g_failures) << "/" << g_checks
		<< " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
