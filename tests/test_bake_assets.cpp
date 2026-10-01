// bake-asset tests: the bake-identity layer (Engine/Asset/bake_asset.h)
// that makes GI bakes + cubemap captures first-class assets — per-file
// .meta sidecars (fresh GUID at first bake, stable across rebakes),
// guid-derived VFS paths, and the cubemanifest guid rows. No GPU.
//
// What it pins down:
//   1. sidecar: write/read round-trip (guid/kind/logical); missing and
//      invalid sidecars read as absent.
//   2. identity: first bake mints; rebake reuses the guid by (kind,
//      logical) even after the payload file is renamed (sidecar travels
//      with it); different logicals mint different guids.
//   3. SplitManifestRow: "<guid> <path>  # <note>" rows, legacy bare
//      paths, comments/blanks.
//   4. cubemanifest: guid rows serialize/parse round-trip; legacy
//      6-token and 7-token pin rows still parse (no guid); unknown
//      trailing tokens still fail.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <Engine/Asset/bake_asset.h>
#include <Engine/Renderer/cubemap.h>

#include <filesystem>

namespace {

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

std::string TempDir() {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path dir = fs::temp_directory_path(ec)
		/ "vulkantoy_bakeassets";
	fs::create_directories(dir, ec);
	return dir.generic_string();
}

void TestSidecarRoundTrip() {
	namespace fs = std::filesystem;
	const std::string dir = TempDir() + "/sidecar";
	std::error_code ec;
	fs::create_directories(dir, ec);
	const std::string file = dir + "/abc123.sglm";
	{
		std::ofstream f(file, std::ios::binary | std::ios::trunc);
		f << "payload";
	}
	std::string err;
	const std::string guid = "0123456789abcdef0123456789abcdef";
	Check(smvk::bake::WriteBakeAssetSidecar(file, guid,
			smvk::bake::kBakeAssetKindGi, "lightmap:test", err),
		"bake.sidecar.write");
	smvk::bake::BakeAssetSidecar side;
	Check(smvk::bake::ReadBakeAssetSidecar(file, side, err)
		&& side.guid == guid && side.kind == "gi"
		&& side.logical == "lightmap:test", "bake.sidecar.read");
	Check(smvk::bake::PayloadBakeGuid(file) == guid,
		"bake.sidecar.payload-guid");
	// Missing sidecar reads absent; garbage guid reads invalid.
	smvk::bake::BakeAssetSidecar missing;
	Check(!smvk::bake::ReadBakeAssetSidecar(dir + "/nope.sglm",
			missing, err), "bake.sidecar.missing");
	{
		std::ofstream f(file + ".meta",
			std::ios::binary | std::ios::trunc);
		f << "guid=not-a-guid\nkind=gi\n";
	}
	smvk::bake::BakeAssetSidecar bad;
	Check(!smvk::bake::ReadBakeAssetSidecar(file, bad, err),
		"bake.sidecar.invalid-guid");
	Check(!smvk::bake::IsBakeAssetGuid("XYZ")
		&& smvk::bake::IsBakeAssetGuid(guid)
		&& !smvk::bake::IsBakeAssetGuid(
			"0123456789ABCDEF0123456789ABCDEF"),
		"bake.guid.valid");
	fs::remove_all(dir, ec);
}

void TestEnsureGuid() {
	namespace fs = std::filesystem;
	const std::string dir = TempDir() + "/ensure";
	std::error_code ec;
	fs::create_directories(dir, ec);
	std::string err;
	// First bake mints.
	const std::string g1 = smvk::bake::EnsureBakeAssetGuid(dir, "gi",
		"lightmap:test", err);
	Check(!g1.empty() && smvk::bake::IsBakeAssetGuid(g1),
		"bake.ensure.mint");
	{
		std::ofstream f(dir + "/" + g1 + ".sglm",
			std::ios::binary | std::ios::trunc);
		f << "payload";
	}
	Check(smvk::bake::WriteBakeAssetSidecar(dir + "/" + g1 + ".sglm",
			g1, "gi", "lightmap:test", err),
		"bake.ensure.sidecar");
	// Rebake reuses by (kind, logical).
	const std::string g2 = smvk::bake::EnsureBakeAssetGuid(dir, "gi",
		"lightmap:test", err);
	Check(g2 == g1, "bake.ensure.reuse");
	// A different logical mints independently.
	const std::string g3 = smvk::bake::EnsureBakeAssetGuid(dir, "gi",
		"probe:test", err);
	Check(!g3.empty() && g3 != g1, "bake.ensure.logical-distinct");
	// Rename heals: move payload + sidecar, the scan still matches.
	fs::rename(dir + "/" + g1 + ".sglm", dir + "/moved.sglm", ec);
	fs::rename(dir + "/" + g1 + ".sglm.meta",
		dir + "/moved.sglm.meta", ec);
	const std::string g4 = smvk::bake::EnsureBakeAssetGuid(dir, "gi",
		"lightmap:test", err);
	Check(g4 == g1, "bake.ensure.rename-heals");
	fs::remove_all(dir, ec);
}

void TestSplitManifestRow() {
	using smvk::bake::SplitManifestRow;
	Check(SplitManifestRow("gi/abc.sgrv")
			== "gi/abc.sgrv", "bake.row.legacy");
	Check(SplitManifestRow(
			"0123456789abcdef0123456789abcdef gi/abc.sgrv  # bar")
			== "gi/abc.sgrv", "bake.row.guid-note");
	Check(SplitManifestRow("0123456789abcdef0123456789abcdef gi/abc.sgrv")
			== "gi/abc.sgrv", "bake.row.guid-bare");
	Check(SplitManifestRow("  gi/abc.sgrv  ") == "gi/abc.sgrv",
		"bake.row.trim");
	Check(SplitManifestRow("") == "", "bake.row.blank");
	Check(SplitManifestRow("# convex GI volumes") == "",
		"bake.row.comment");
	Check(SplitManifestRow("not-a-guid-hex gi/abc.sgrv")
			== "not-a-guid-hex gi/abc.sgrv", "bake.row.nonhex-kept");
}

void TestCubemanifestGuid() {
	using smvk::CubemapManifestEntry;
	const std::string g1 = "0123456789abcdef0123456789abcdef";
	const std::string g2 = "fedcba9876543210fedcba9876543210";
	std::vector<CubemapManifestEntry> rows;
	{
		CubemapManifestEntry e{};
		e.vfsPath = "textures/cubemaps/" + g1 + ".ktx2";
		e.position = glm::vec3(1.0f, 2.0f, 3.0f);
		e.influenceRadius = 10.0f;
		e.fadeRadius = 5.0f;
		e.pinned = true;
		e.guid = g1;
		rows.push_back(e);
	}
	{
		CubemapManifestEntry e{};
		e.vfsPath = "textures/cubemaps/" + g2 + ".ktx2";
		e.position = glm::vec3(4.0f, 5.0f, 6.0f);
		e.influenceRadius = 8.0f;
		e.fadeRadius = 4.0f;
		e.guid = g2;
		rows.push_back(e);
	}
	const std::string text = smvk::CubemapManifestSerialize(rows);
	std::vector<CubemapManifestEntry> back;
	std::string err;
	Check(smvk::CubemapManifestParse(text, back, err)
		&& back.size() == 2, "bake.cubemanifest.guid-roundtrip");
	if (back.size() == 2) {
		Check(back[0].guid == g1 && back[0].pinned
			&& back[0].vfsPath == rows[0].vfsPath
			&& std::abs(back[0].position.x - 1.0f) < 1e-4f,
			"bake.cubemanifest.guid-row");
		Check(back[1].guid == g2 && !back[1].pinned,
			"bake.cubemanifest.guid-row-unpinned");
	}
	// Legacy rows (no guid) parse exactly as before.
	{
		std::vector<CubemapManifestEntry> legacy;
		Check(smvk::CubemapManifestParse(
				"textures/cubemaps/sponza_3.ktx2 1 2 3 10 5\n"
				"textures/cubemaps/sponza_4.ktx2 4 5 6 10 5 pin\n",
				legacy, err)
			&& legacy.size() == 2, "bake.cubemanifest.legacy-roundtrip");
		if (legacy.size() == 2)
			Check(legacy[0].guid.empty() && !legacy[0].pinned
				&& legacy[1].guid.empty() && legacy[1].pinned
				&& legacy[1].vfsPath
					== "textures/cubemaps/sponza_4.ktx2",
				"bake.cubemanifest.legacy-fields");
	}
	// Unknown trailing tokens still fail (forward strictness).
	{
		std::vector<CubemapManifestEntry> bad;
		Check(!smvk::CubemapManifestParse(
				"textures/cubemaps/a.ktx2 1 2 3 10 5 frobnicate\n",
				bad, err), "bake.cubemanifest.unknown-token-fails");
	}
	{
		std::vector<CubemapManifestEntry> bad;
		Check(!smvk::CubemapManifestParse(
				"textures/cubemaps/a.ktx2 1 2 3 10 5 guid nothex\n",
				bad, err), "bake.cubemanifest.bad-guid-fails");
	}
	std::error_code ec;
	std::filesystem::remove_all(TempDir(), ec);
}

} // namespace

int main() {
	TestSidecarRoundTrip();
	TestEnsureGuid();
	TestSplitManifestRow();
	TestCubemanifestGuid();
	std::cout << "checks=" << g_checks
		<< " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
