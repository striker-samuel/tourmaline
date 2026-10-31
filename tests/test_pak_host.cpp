// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// pak_host_tests: CPU-only regression tests for the pak-viewer HOST layer
// (app/src): CLI parsing (validated numerics, the --pak gate, the
// dedicated options object), pak path resolution (the exe-relative
// fallback for ordinary relative paths), and corruption handling
// (PakBackend::Open fail-closed on garbage / CRC-torn images). No GPU,
// no window: everything here runs headless in CI.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h> // GetModuleFileNameA (exe-relative resolution test)
#else
#include <unistd.h> // readlink (/proc/self/exe)
#endif

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

#include "Host.h"
#include "PakPreload.h"
#include "PakWorldLoader.h"
#include "ProjectRuntime.h"

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/pak.h>
#include <Engine/Scene/scene.h>

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

// ---- CLI parsing (app::ParsePakAppArgs -> app::ViewerOptions) ----

bool ParseOk(std::vector<const char*> argv, app::ViewerOptions& opts) {
	int exitCode = 0;
	return app::ParsePakAppArgs(int(argv.size()), argv.data(), opts, exitCode)
		&& exitCode == 0;
}

bool ParseFails(std::vector<const char*> argv, int& exitCode) {
	app::ViewerOptions opts;
	return !app::ParsePakAppArgs(int(argv.size()), argv.data(), opts,
			exitCode);
}

void TestCliParse() {
	app::ViewerOptions opts;
	Check(ParseOk({ "App", "--pak", "x.pak" }, opts)
		&& opts.pakPath == "x.pak",
		"cli.pak.required-value");
	Check(ParseOk({ "App", "--pak=y.pak", "--scene-index=3" }, opts)
		&& opts.sceneIndex == 3,
		"cli.scene-index.equals-form");
	// The required-flag gate: no --pak, no run.
	{
		int ec = 0;
		Check(ParseFails({ "App" }, ec) && ec != 0,
			"cli.no-pak-refused");
	}
	// Unknown args refuse with a failure exit.
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--nonsense" }, ec)
			&& ec != 0, "cli.unknown-arg-refused");
	}
	// --help exits SUCCESS (documented usage path).
	{
		int ec = 1;
		app::ViewerOptions opts;
		std::vector<const char*> helpArgv = { "App", "--help" };
		Check(!app::ParsePakAppArgs(int(helpArgv.size()), helpArgv.data(),
				opts, ec) && ec == 0,
			"cli.help-success-exit");
	}
	// The removed --gltf refuses loudly.
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--gltf" }, ec)
			&& ec != 0, "cli.gltf-removed-refused");
	}
	// ---- the --project launch (the appmanifest surface) -------------
	Check(ParseOk({ "App", "--project", "build/sponza.tapp" }, opts)
		&& opts.project == "build/sponza.tapp",
		"cli.project.value");
	Check(ParseOk({ "App", "--project=world.tapp", "--no-vfs" }, opts)
		&& opts.project == "world.tapp" && opts.noVfs,
		"cli.project.equals-form");
	// --project satisfies the entry-point gate on its own.
	Check(ParseOk({ "App", "--project", "sponza.tapp" }, opts),
		"cli.project.solo-runs");
	// Mutual exclusion: the appmanifest owns the entry point.
	{
		int ec = 0;
		Check(ParseFails({ "App", "--project", "s.tapp", "--pak",
				"x.pak" }, ec)
			&& ec != 0, "cli.project.pak-exclusive");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--project", "s.tapp",
				"--scene-index", "1" }, ec)
			&& ec != 0, "cli.project.scene-index-exclusive");
	}
	// ---- Validated numerics (finding #11): malformed values REFUSE the
	// launch instead of silently mapping to zero. The old atoi/atof
	// accepted all of these as 0 — and a zero bench budget silently
	// flipped the run to interactive (a bench script spinning forever).
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--bench", "12x" }, ec),
			"cli.bench.malformed-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--bench=0" }, ec),
			"cli.bench.zero-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak",
				"--scene-index=abc" }, ec),
			"cli.scene-index.malformed-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak",
				"--scene-index=-1" }, ec),
			"cli.scene-index.negative-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak",
				"--bench_move=fast" }, ec),
			"cli.bench_move.malformed-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak",
				"--bench_move=99999" }, ec),
			"cli.bench_move.out-of-range-refused");
	}
	// Well-formed values still parse.
	Check(ParseOk({ "App", "--pak", "x.pak", "--bench", "64",
			"--bench_move", "4.5", "--scene-index", "2" }, opts)
		&& opts.benchFrames == 64
		&& (opts.benchMoveSpeed > 4.49f && opts.benchMoveSpeed < 4.51f)
		&& opts.sceneIndex == 2,
		"cli.numerics-well-formed-accepted");
	// ---- The dedicated options object (finding #16): the parser cannot
	// erase host-supplied config. Pre-install an extension hook + a run
	// pin, parse, verify both survive untouched (the old wholesale
	// `cfg = PakAppConfig{}` reset silently wiped them).
	{
		app::PakAppConfig cfg;
		bool hookRan = false;
		cfg.extraElement = [&hookRan](smvk::App&, engine::WorldRenderer&,
				std::shared_ptr<smvk::World*>) {
			hookRan = true;
			return nullptr;
		};
		cfg.extraOwnsCamera = true;
		app::ViewerOptions& optsRef = cfg.cli;
		Check(ParseOk({ "App", "--pak", "x.pak" }, optsRef)
			&& cfg.extraElement && !hookRan
			&& cfg.extraOwnsCamera,
			"cli.parse.preserves-host-config");
	}
}

// ---- Pak path resolution (app::ResolvePakOpenPath) ----

void TestPakPathResolution() {
	namespace fs = std::filesystem;
	// Scratch tree under the temp dir: one "cwd" root and one "exe" root
	// (the TEST BINARY's directory — the exe-relative fallback's base).
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pak_host_tests";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	const fs::path exeDir =
		fs::absolute(fs::path([]() {
#ifdef _WIN32
			char buf[1024] = {};
			GetModuleFileNameA(nullptr, buf, DWORD(sizeof(buf)));
			return std::string(buf);
#else
			char buf[4096] = {};
			const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
			if (n > 0) buf[n] = '\0';
			return std::string(buf);
#endif
		}())).parent_path();
	const fs::path exePakDir = exeDir / "pakhost_test_content";
	fs::create_directories(exePakDir, ec);
	const fs::path exePak = exePakDir / "relative.pak";
	{
		std::ofstream f(exePak, std::ios::binary);
		f << "fake pak bytes";
	}
	// The finding (#12): an ORDINARY relative path whose file exists ONLY
	// beside the executable (not in the CWD) must resolve through the
	// exe-relative fallback. Run with the CWD parked in the scratch dir
	// where the pak does NOT exist.
	const fs::path cwdSave = fs::current_path(ec);
	fs::current_path(tmp, ec);
	{
		const std::string resolved =
			app::ResolvePakOpenPath("pakhost_test_content/relative.pak");
		Check(!resolved.empty()
			&& fs::path(resolved) == fs::absolute(exePak),
			"pakpath.ordinary-relative.exe-relative-fallback");
	}
	// Missing everywhere: empty result (the launch-failure path) — the
	// KickPakPreload future must report the error without throwing
	// (headless launch failures must EXIT, not crash or hang).
	{
		const std::string resolved =
			app::ResolvePakOpenPath("pakhost_test_content/absent.pak");
		Check(resolved.empty(), "pakpath.missing-empty");
		auto fut = app::KickPakPreload("pakhost_test_content/absent.pak");
		const app::PakPreload pre = fut.get();
		Check(!pre.backend && !pre.error.empty(),
			"pakpath.preload-missing-pak.errors-gracefully");
	}
	fs::current_path(cwdSave, ec);
	// Absolute paths resolve as-is (no fallback needed).
	{
		const std::string resolved =
			app::ResolvePakOpenPath(exePak.string());
		Check(!resolved.empty(), "pakpath.absolute-as-is");
	}
	fs::remove_all(tmp, ec);
}

// ---- Corruption handling (smvk::PakBackend::Open fail-closed) ----

void TestPakCorruption() {
	// A minimal VALID pak via PakWriter (a single Scene-kind blob).
	smvk::PakWriter writer;
	Check(writer.AddBlob(smvk::PakAssetKind::Scene, "scene/only",
			{ 1, 2, 3, 4 }), "pakcorruption.writer-adds-blob");
	const std::vector<uint8_t> image = writer.Finalize();
	{
		std::string err;
		auto backend = smvk::PakBackend::Open(
			std::make_shared<std::vector<uint8_t>>(image), "ok", err);
		Check(backend != nullptr, "pakcorruption.valid-pak-opens");
	}
	// Garbage bytes: rejected with a reason.
	{
		std::string err;
		const std::vector<uint8_t> garbage(64, 0xAB);
		auto backend = smvk::PakBackend::Open(
			std::make_shared<std::vector<uint8_t>>(garbage), "bad", err);
		Check(backend == nullptr && !err.empty(),
			"pakcorruption.garbage-rejected");
	}
	// A torn write (CRC mismatch): one byte flipped mid-image.
	{
		std::vector<uint8_t> torn = image;
		torn[torn.size() / 2] ^= 0xFF;
		std::string err;
		auto backend = smvk::PakBackend::Open(
			std::make_shared<std::vector<uint8_t>>(torn), "torn", err);
		Check(backend == nullptr && !err.empty(),
			"pakcorruption.crc-torn-rejected");
	}
	// Truncated image: rejected.
	{
		std::vector<uint8_t> trunc(image.begin(),
			image.begin() + std::ptrdiff_t(image.size() / 2));
		std::string err;
		auto backend = smvk::PakBackend::Open(
			std::make_shared<std::vector<uint8_t>>(trunc), "short", err);
		Check(backend == nullptr && !err.empty(),
			"pakcorruption.truncated-rejected");
	}
}

// ---- Static-flag application (app::ApplyCookedStaticFlags — the
// loader's decision seam, driven on hand-built value worlds; finding B:
// an authored ALL-DYNAMIC cooked scene must never be misread as a legacy
// pak and re-frozen viewer-side) ----

// One offline cluster with a two-node hierarchy, optionally carrying the
// serialized-static-section flag (the cooked discriminator) and/or a
// skeleton.
std::unique_ptr<smvk::World> WorldWithOneCluster(bool staticsSerialized,
	bool skeleton) {
	auto world = std::make_unique<smvk::World>();
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetOfflineMode(true);
	addNode(cluster->scene, -1, 0);
	addNode(cluster->scene, 0, 1);
	cluster->scene.staticRoot.assign(cluster->scene.hierarchy.size(), 0);
	cluster->scene.staticRootsSerialized = staticsSerialized;
	cluster->hasSkeleton = skeleton;
	world->RegisterCluster(std::move(cluster));
	return world;
}

bool AllNodesDynamic(const smvk::World& world) {
	const ::Scene& s = world.clusters.front()->scene;
	return s.staticRoot.size() == s.hierarchy.size()
		&& std::all_of(s.staticRoot.begin(), s.staticRoot.end(),
			[](uint8_t v) { return v == 0; });
}

void TestStaticFlagApplication() {
	// THE FINDING-B CASE: a COOKED ALL-DYNAMIC scene (the serialized static
	// section PRESENT, every mark false — an authored all-dynamic scene).
	// Honored verbatim: NO viewer-invented freeze (the old any_of form
	// misread exactly this scene as a legacy pak and blanket-froze it —
	// the regression the loader rework was supposed to remove).
	{
		auto world = WorldWithOneCluster(/*staticsSerialized=*/true,
			/*skeleton=*/false);
		app::ApplyCookedStaticFlags(*world, "alldynamic.pak");
		Check(AllNodesDynamic(*world),
			"pakhost.staticflags.alldynamic-cooked-honored");
	}
	// A cooked cluster with REAL marks stays untouched (nothing to do;
	// the marks are the cooked data).
	{
		auto world = WorldWithOneCluster(true, false);
		::Scene& s = world->clusters.front()->scene;
		MarkSubtreeStatic(s, 0);
		app::ApplyCookedStaticFlags(*world, "marked.pak");
		Check(s.staticRoot[0] == 1,
			"pakhost.staticflags.cooked-marks-preserved");
	}
	// LEGACY (pre-section) non-skinned cluster: the loud compat fallback
	// freezes the root — the documented old-pak behavior, now the ONLY
	// route to a viewer-side freeze.
	{
		auto world = WorldWithOneCluster(false, false);
		app::ApplyCookedStaticFlags(*world, "legacy.pak");
		Check(!AllNodesDynamic(*world)
			&& world->clusters.front()->scene.staticRoot[0] == 1,
			"pakhost.staticflags.legacy-fallback-freezes");
	}
	// Skinned legacy cluster: NEVER frozen (the animation driver owns
	// its transforms — the fallback skips skeletons even without the
	// serialized section).
	{
		auto world = WorldWithOneCluster(false, true);
		app::ApplyCookedStaticFlags(*world, "skinned-legacy.pak");
		Check(AllNodesDynamic(*world),
			"pakhost.staticflags.skeleton-never-frozen");
	}
}

// ---- Parallel pak CRC first-use (finding C: PakCrc32's table init is a
// magic static now — the old lazy bool + unsynchronized fill was a data
// race when the FIRST CRC was the parallel one, with every worker racing
// the same table) ----
// MUST RUN FIRST in main: this process's very first PakCrc32 calls happen
// inside these concurrent threads, so the static's initialization is
// exercised exactly-once-under-race rather than pre-warmed.
void TestParallelCrcFirstUse() {
	std::vector<uint8_t> blob(4 * 1024 * 1024);
	for (size_t i = 0; i < blob.size(); ++i)
		blob[i] = uint8_t(i * 31u + (i >> 9));
	const uint32_t serial = smvk::PakCrc32(blob.data(), blob.size());
	// One thread takes the PARALLEL path with small chunks (every worker
	// first-touches the table concurrently — the old race window); the
	// others first-touch it serially at the same time. All must agree
	// with the serial reference.
	std::atomic<uint32_t> mismatches{ 0 };
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; ++t)
		threads.emplace_back([&, t] {
			const uint32_t got = (t == 0)
				? smvk::PakCrc32Parallel(
					std::span<const uint8_t>(blob), 4096)
				: smvk::PakCrc32(blob.data(), blob.size());
			if (got != serial)
				mismatches.fetch_add(1);
		});
	for (auto& th : threads)
		th.join();
	Check(mismatches.load() == 0,
		"pakcorruption.parallel-crc-first-use-agrees");
}

// ---- ProjectRuntime (the apphost's project façade) -------------------
// A pak with one Texture blob + a manifest with the [ids] row beside
// it: the stable id resolves to its row and to REAL BYTES through the
// pak backend (the v1 form of "LoadTexture(project::textures::Stone)").
void TestProjectRuntime() {
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_prt";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	const std::string vfsPath = "content/models/s/stone.png";
	const std::vector<uint8_t> ktx = { 0xAB, 'K', 'T', 'X', ' ', '2',
		'0', 0x0D, 0x0A, 1, 2, 3, 4, 5 };
	{
		smvk::PakWriter w;
		w.AddBlob(smvk::PakAssetKind::Texture, vfsPath,
			smvk::BuildTexturePayload(vfsPath, -1, -1, -1, -1,
				std::span<const uint8_t>(ktx)));
		const std::vector<uint8_t> image = w.Finalize();
		std::ofstream f(tmp / "shared.pak",
			std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(image.data()),
			std::streamsize(image.size()));
	}
	const fs::path tapp = tmp / "game.tapp";
	{
		std::ofstream f(tapp, std::ios::binary | std::ios::trunc);
		f << "# Tourmaline Runtime Project\n"
			<< "appmanifest 1\n"
			<< "entry shared.pak scene\n"
			<< "\n[paks]\nshared.pak\n"
			<< "\n[ids]\n"
			<< "0x2000 texture shared.pak " << vfsPath << "\n"
			<< "0x3000 mesh shared.pak content/models/s/rock.geometry\n"
			<< "0x1000 scene shared.pak content/scenes/s.tour\n";
	}
	std::string err;
	auto rt = app::ProjectRuntime::Load(tapp.generic_string(), err);
	Check(bool(rt) && err.empty(), "prt.load");
	if (rt) {
		Check(rt->Manifest().paks.size() == 1
			&& rt->Manifest().assets.size() == 3,
			"prt.manifest-parsed");
		const smvk::AppManifestAsset* row = rt->Asset(0x2000);
		Check(row && row->kind == "texture"
			&& row->pak == "shared.pak" && row->path == vfsPath,
			"prt.asset-row");
		Check(rt->Asset(0x9999) == nullptr
			&& rt->Asset("texture", vfsPath) == row,
			"prt.asset-lookup");
		std::string rErr;
		auto bytes = rt->ReadAsset(0x2000, rErr);
		Check(bytes && *bytes == ktx && rErr.empty(),
			"prt.read-bytes");
		// Unknown ids + unserved paths fail LOUDLY (never a guess).
		std::string badErr;
		Check(!rt->ReadAsset(0x9999, badErr) && !badErr.empty(),
			"prt.unknown-id-fails");
		// Non-texture kinds refuse HONESTLY (the commit review's #4:
		// meshes/scenes are cluster-internal, not VFS bytes — the error
		// must say so, never the misleading "pak does not serve").
		{
			std::string meshErr;
			Check(!rt->ReadAsset(0x3000, meshErr)
				&& meshErr.find("cluster") != std::string::npos,
				"prt.mesh-read-honest-refusal");
			std::string sceneErr;
			Check(!rt->ReadAsset(0x1000, sceneErr)
				&& sceneErr.find("identity") != std::string::npos,
				"prt.scene-read-honest-refusal");
		}
		// A manifest-relative pak resolution: move the whole tree, the
		// runtime still resolves (RUN-04's contract through the façade).
		const fs::path moved = tmp / "moved";
		fs::create_directories(moved, ec);
		fs::copy(tmp / "shared.pak", moved / "shared.pak",
			fs::copy_options::overwrite_existing, ec);
		fs::copy(tapp, moved / "game.tapp",
			fs::copy_options::overwrite_existing, ec);
		std::string mErr;
		auto rt2 = app::ProjectRuntime::Load(
			(moved / "game.tapp").generic_string(), mErr);
		std::string rErr2;
		Check(bool(rt2)
			&& rt2->ReadAsset(0x2000, rErr2) && rErr2.empty(),
			"prt.moved-tree-reads");
	}
	else {
		Check(false, "prt.manifest-parsed");
		Check(false, "prt.asset-row");
		Check(false, "prt.asset-lookup");
		Check(false, "prt.read-bytes");
		Check(false, "prt.unknown-id-fails");
		Check(false, "prt.moved-tree-reads");
	}
}

} // namespace

int main() {
	TestParallelCrcFirstUse();
	TestCliParse();
	TestPakPathResolution();
	TestPakCorruption();
	TestStaticFlagApplication();
	TestProjectRuntime();
	std::cout << "pak_host_tests: " << (g_checks - g_failures) << "/"
		<< g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
