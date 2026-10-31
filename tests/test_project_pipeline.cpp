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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <project_pipeline/build_plan.h>
#include <project_pipeline/build_pipeline.h>
#include <project_pipeline/import_pipeline.h>
#include <project_pipeline/project.h>
#include <project_pipeline/project_recipe.h>
#include <project_pipeline/recipe_resolver.h>

#include <gltf_import/asset_export.h>

#include <Engine/Asset/pak.h>
#include <Engine/Asset/tour.h>

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
	int n, uint32_t seed = 0, bool withMaterial = false) {
	fs::create_directories(dir);
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
	j << "{\"asset\":{\"version\":\"2.0\"},"
		"\"scene\":0,"
		"\"scenes\":[{\"nodes\":[0]}],"
		"\"nodes\":[{\"name\":\"Grid\",\"mesh\":0}],"
		"\"meshes\":[{\"primitives\":[{\"attributes\":"
		"{\"POSITION\":0,\"NORMAL\":1},\"indices\":2"
		<< (withMaterial ? ",\"material\":0" : "") << "}]}],"
		<< (withMaterial
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
		bool adopted = true;
		smvk::project::Project again = smvk::project::Project::Init(
			tmp.generic_string(), "Other", adopted, err);
		Check(again.Valid() && !adopted && again.name == "ModelTest",
			"proj.init-adopts-keeps-name");
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
	Check(smvk::assets::ImportMeshSource(src.gltf, "", a, man,
			nullptr, &err), "fingerprint.derive-ok");
	Check(!smvk::assets::MeshArtifactStale(src.gltf, a),
		"fingerprint.same-settings-current");
	Check(smvk::assets::MeshArtifactStale(src.gltf, b),
		"fingerprint.changed-settings-stale");
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
	// An include claiming geometry for a scene-less pak: meshes are
	// cluster-internal — dead content in an asset-only pak.
	resolveFails("geometry-without-scene",
		"recipe 1\nentry content/scenes/world.tour\n"
		"[pak hoard]\noutput h.pak\n"
		"include content/models/world/*.geometry\n"
		"[pak world]\noutput w.pak\nscene content/scenes/world.tour\n",
		"owns no scene");
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

// ---- the shared executor (M2: one plan, adapter-invoked stages) ------

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
		if (pakRc == 0) {
			// The executor verifies the stage's output EXISTS and
			// POLICES it through PakReader after a success — the
			// recording adapter fakes a REAL minimal pak (one Scene
			// blob; policing only inspects Texture entries, so a
			// scene-blob-only pak passes it by construction).
			std::error_code ec;
			fs::create_directories(fs::path(a.outPak).parent_path(), ec);
			smvk::PakWriter w;
			const std::vector<uint8_t> fake{ 1, 2, 3 };
			w.AddBlob(smvk::PakAssetKind::Scene, "recording/fake",
				std::vector<uint8_t>(fake));
			const std::vector<uint8_t> image = w.Finalize();
			std::ofstream f(a.outPak, std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char*>(image.data()),
				std::streamsize(image.size()));
		}
		return pakRc;
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
					== (tmp / "publish" / "grid.pak").lexically_normal()
						.generic_string()
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
	TestRecipeParsing();
	TestRecipeGlob();
	TestRecipeResolution();
	TestBuildPlanExecutor();
	std::cout << "project_pipeline_tests: "
		<< (g_checks - g_failures) << "/" << g_checks
		<< " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
