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
#include <sstream>
#include <string>
#include <vector>

#include <project_pipeline/import_pipeline.h>
#include <project_pipeline/project.h>

#include <gltf_import/asset_export.h>

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

TestSource WriteGridGltf(const fs::path& dir, const std::string& stem,
	int n, uint32_t seed = 0) {
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
		"{\"POSITION\":0,\"NORMAL\":1},\"indices\":2}]}],"
		"\"accessors\":["
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

} // namespace

int main() {
	TestProjectModel();
	TestImportLifecycle();
	TestFingerprint();
	std::cout << "project_pipeline_tests: "
		<< (g_checks - g_failures) << "/" << g_checks
		<< " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
