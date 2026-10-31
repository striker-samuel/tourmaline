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
#include "ProjectScenes.h"
#include "DeployCheck.h"

#include <artifact_cache/artifact_cache.h> // HashBytes (the name digests)

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>
#include <Engine/Vfs/vfs.h>

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

// The DEPLOYMENT VALIDATION GATE (DeployCheck.h): the content-address
// contract + existence + integrity, and the engine shader source.
void TestDeployCheck() {
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_deploycheck";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	// A minimal VALID pak.
	smvk::PakWriter writer;
	Check(writer.AddBlob(smvk::PakAssetKind::Scene, "scene/only",
			{ 1, 2, 3, 4 }), "deploycheck.writer-adds-blob");
	const std::vector<uint8_t> image = writer.Finalize();
	// The content-addressed name: <stem>-<fnv1a64>.pak (exactly what
	// the recipe build's publish pass writes).
	const std::string digest =
		smvk::cache::HashBytes(image.data(), image.size());
	const fs::path addressed = tmp / ("game-" + digest + ".pak");
	{
		std::ofstream f(addressed, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(image.data()),
			std::streamsize(image.size()));
	}
	{
		std::string err;
		Check(app::ValidatePakFile(addressed.generic_string(), err),
			"deploycheck.addressed-pak-passes");
	}
	// The SAME bytes under a WRONG digest name: the deployment
	// contract fails (a stale/wrong pak under the expected name).
	{
		const std::string wrong = digest == "0000000000000000"
			? "ffffffffffffffff" : "0000000000000000";
		const fs::path badName = tmp / ("game-" + wrong + ".pak");
		{
			std::ofstream f(badName, std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char*>(image.data()),
				std::streamsize(image.size()));
		} // close + flush BEFORE the gate reads the bytes
		std::string err;
		const bool ok = app::ValidatePakFile(badName.generic_string(), err);
		if (!(!ok && err.find("content-address mismatch")
				!= std::string::npos))
			std::cout << "  (wrong-digest: ok=" << ok
				<< " err='" << err << "')\n";
		Check(!ok && err.find("content-address mismatch")
				!= std::string::npos,
			"deploycheck.wrong-digest-fails");
	}
	// A PLAIN (legacy, non-addressed) name: passes on existence +
	// a valid image.
	{
		const fs::path plain = tmp / "plain.pak";
		{
			std::ofstream f(plain, std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char*>(image.data()),
				std::streamsize(image.size()));
		} // close + flush BEFORE the gate reads the bytes
		std::string err;
		const bool ok = app::ValidatePakFile(plain.generic_string(), err);
		if (!ok)
			std::cout << "  (plain: err='" << err << "')\n";
		Check(ok, "deploycheck.plain-name-passes");
	}
	// Missing file: the actionable missing-deployment error.
	{
		std::string err;
		Check(!app::ValidatePakFile(
				(tmp / "nope-0123456789abcdef.pak").generic_string(),
				err)
			&& err.find("missing") != std::string::npos,
			"deploycheck.missing-pak-fails");
	}
	// Corrupt bytes under the RIGHT name: the integrity check fails.
	{
		std::vector<uint8_t> torn = image;
		torn[torn.size() / 2] ^= 0xFF;
		const fs::path tornPath = tmp / ("game-" + digest + ".pak");
		// (overwrite the good file with torn bytes)
		{
			std::ofstream f(tornPath, std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char*>(torn.data()),
				std::streamsize(torn.size()));
		}
		std::string err;
		Check(!app::ValidatePakFile(tornPath.generic_string(), err)
			&& err.find("integrity") != std::string::npos,
			"deploycheck.torn-pak-fails");
	}
	// The engine shader source: the test exe runs beside the build
	// tree's dev content/shaders/ (+ the deployed engine-shaders.pak)
	// — a source is mounted.
	{
		std::string err;
		Check(app::ValidateEngineShaderSource(err),
			"deploycheck.shader-source-present");
	}
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

// ---- ProjectScenes (the scene-instance layer over the façade) --------
// GPU-less end-to-end: the pak->cluster seam is replaced with a
// hand-built offline cluster (the test_world_entities recipe), so the
// whole LoadScene/LoadSceneAdditive/RemoveScene + ownership-prefetch
// machinery runs headless — the id->scene join, the instance table's
// stale-handle guarantee, and the ownership registry's pin semantics
// are the contracts under test. The default clusterLoader (the real
// DeserializeCluster) needs a GPU and is exercised by every cooked
// project launch instead.

// A minimal scene blob carrying a NAME (the v5 trailing name section —
// see ParseSceneBlob's trailing-section grammar): the v1 core + the
// v2 static / v3 lightmap marker sections + a zero-pair v4 lights
// section + the name. This is what ListPakScenes reports for the
// id->scene join.
std::vector<uint8_t> NamedSceneBlob(const std::string& name) {
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
	u32(1); b.push_back(0); // static section (v2): nodeCount bytes
	u32(1); b.push_back(0); // lightmap section (v3): nodeCount bytes
	u32(0);                 // lights section (v4): zero pairs
	u32(uint32_t(name.size())); // scene name (v5)
	b.insert(b.end(), name.begin(), name.end());
	return b;
}

void TestProjectScenes() {
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pscenes";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	const std::string texPath = "content/models/s/stone.png";
	const std::vector<uint8_t> ktx = { 0xAB, 'K', 'T', 'X', ' ', '2',
		'0', 0x0D, 0x0A, 7, 7, 7, 7 };
	// A sound asset (the raw .wav bytes — Sound-kind blobs ride the
	// same VFS surface as textures; the façade's ReadAsset serves
	// them and the audio manager decodes them).
	const std::string wavPath = "content/audio/bells.wav";
	const std::vector<uint8_t> wav = { 'R', 'I', 'F', 'F', 0x18, 0, 0, 0,
		'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 0x10, 0, 0, 0, 1, 0,
		1, 0, 0x80, 0xBB, 0, 0, 0, 0x77, 0xA1, 0, 0, 2, 0, 0x10, 0,
		'd', 'a', 't', 'a', 4, 0, 0, 0, 1, 2, 3, 4 };
	// one.pak: a single named scene ("Church") + a texture + a sound.
	// two.pak: TWO scenes — the multi-scene refusal without a recorded
	// name.
	{
		smvk::PakWriter w;
		w.AddBlob(smvk::PakAssetKind::Scene, "recording/one",
			NamedSceneBlob("Church"));
		w.AddBlob(smvk::PakAssetKind::Texture, texPath,
			smvk::BuildTexturePayload(texPath, -1, -1, -1, -1,
				std::span<const uint8_t>(ktx)));
		w.AddBlob(smvk::PakAssetKind::Sound, wavPath, wav);
		const std::vector<uint8_t> image = w.Finalize();
		std::ofstream f(tmp / "one.pak", std::ios::binary
			| std::ios::trunc);
		f.write(reinterpret_cast<const char*>(image.data()),
			std::streamsize(image.size()));
	}
	{
		smvk::PakWriter w;
		w.AddBlob(smvk::PakAssetKind::Scene, "recording/two-a",
			NamedSceneBlob("Alpha"));
		w.AddBlob(smvk::PakAssetKind::Scene, "recording/two-b",
			NamedSceneBlob("Beta"));
		const std::vector<uint8_t> image = w.Finalize();
		std::ofstream f(tmp / "two.pak", std::ios::binary
			| std::ios::trunc);
		f.write(reinterpret_cast<const char*>(image.data()),
			std::streamsize(image.size()));
	}
	const fs::path tapp = tmp / "game.tapp";
	// The compiled-catalog contract's deployment half: the fingerprint
	// over the [ids] rows in emission order (scenes, textures, meshes,
	// sounds; id-sorted within - the fold the generated project.h
	// emits as CatalogFingerprint).
	const uint64_t fpIds[4] = { 0x1000, 0x1001, 0x2000, 0x4000 };
	const uint8_t fpKinds[4] = { smvk::kCatalogKindScene,
		smvk::kCatalogKindScene, smvk::kCatalogKindTexture,
		smvk::kCatalogKindSound };
	const uint32_t fpPaks[4] = { 0, 1, 0, 0 };
	const char* const fpPaths[4] = { "content/scenes/church.tour",
		"content/scenes/two.tour", "content/models/s/stone.png",
		"content/audio/bells.wav" };
	const uint64_t catalogFp = smvk::ComputeCatalogFingerprint(
		fpIds, fpKinds, fpPaks, fpPaths, 4);
	{
		std::ofstream f(tapp, std::ios::binary | std::ios::trunc);
		f << "# Tourmaline Runtime Project\n"
			<< "appmanifest 1\n"
			<< "entry one.pak Church\n";
		{
			char fp[32];
			std::snprintf(fp, sizeof(fp), "0x%llX",
				static_cast<unsigned long long>(catalogFp));
			f << "catalog " << fp << "\n";
		}
		f << "\n[paks]\none.pak\ntwo.pak\n"
			<< "\n[ids]\n"
			// The recorded cooked name: the id->scene join.
			<< "0x1000 scene one.pak content/scenes/church.tour Church\n"
			// No recorded name: a multi-scene pak must refuse loudly.
			<< "0x1001 scene two.pak content/scenes/two.tour\n"
			<< "0x2000 texture one.pak " << texPath << "\n"
			<< "0x4000 sound one.pak " << wavPath << "\n";
	}
	std::string err;
	auto rtOpt = app::ProjectRuntime::Load(tapp.generic_string(), err);
	Check(bool(rtOpt) && err.empty(), "pscenes.load-manifest");
	if (!rtOpt) {
		Check(false, "pscenes.ctor");
		Check(false, "pscenes.target-recorded-name");
		Check(false, "pscenes.load-additive");
		return;
	}
	app::ProjectRuntime& rt = *rtOpt;
	// ---- the compiled-catalog validation (the flattened project.h
	// contract): the game's Assets table + CatalogFingerprint prove
	// themselves against the deployment manifest.
	{
		using Desc = app::ProjectRuntime::CompiledAssetDesc;
		const Desc assets[4] = {
			{ 0x1000, smvk::kCatalogKindScene, 0,
				"content/scenes/church.tour", "" },
			{ 0x1001, smvk::kCatalogKindScene, 1,
				"content/scenes/two.tour", "" },
			{ 0x2000, smvk::kCatalogKindTexture, 0,
				"content/models/s/stone.png", "" },
			{ 0x4000, smvk::kCatalogKindSound, 0,
				"content/audio/bells.wav", "" },
		};
		std::string aErr;
		Check(rt.AttachCompiledCatalog(assets, 4, catalogFp, aErr)
			&& aErr.empty() && rt.CompiledCatalogAttached(),
			"pscenes.catalog-attach-validates");
		// A WRONG fingerprint fails loudly (the version-skew case).
		std::string fErr;
		Check(!rt.AttachCompiledCatalog(assets, 4, catalogFp + 1, fErr)
			&& fErr.find("mismatch") != std::string::npos,
			"pscenes.catalog-fingerprint-mismatch-fails");
		// A row the manifest does not carry fails loudly.
		{
			const Desc stray[1] = { { 0x5000,
				smvk::kCatalogKindSound, 0, "content/audio/x.wav",
				"" } };
			std::string sErr;
			Check(!rt.AttachCompiledCatalog(stray, 1, catalogFp, sErr)
				&& sErr.find("0x20480") != std::string::npos,
				"pscenes.catalog-unknown-row-fails");
		}
		// A pakIndex skew (the row names the wrong mount slot).
		{
			const Desc skewed[4] = { assets[0], assets[1],
				{ 0x2000, smvk::kCatalogKindTexture, 1,
					"content/models/s/stone.png", "" },
				assets[3] };
			std::string kErr;
			Check(!rt.AttachCompiledCatalog(skewed, 4, catalogFp,
					kErr)
				&& kErr.find("mount-order") != std::string::npos,
				"pscenes.catalog-pakindex-skew-fails");
		}
	}
	smvk::OverlayVfs vfs;
	app::ProjectScenes scenes(rt, vfs);
	Check(scenes.Valid() && vfs.Mounts().size() == 2,
		"pscenes.ctor");
	// The façade's reads serve through the SAME adopted backends (the
	// ctor's adopt-or-open pass — one image per pak whatever opens it).
	{
		std::string rErr;
		auto bytes = rt.ReadAsset(0x2000, rErr);
		Check(bytes && *bytes == ktx && rErr.empty(),
			"pscenes.facade-reads-through-shared-cache");
		// The SOUND kind: raw .wav bytes through the same channel (the
		// audio manager's PrefetchSound/PlaySound feed).
		std::string sErr;
		auto wavBytes = rt.ReadAsset(0x4000, sErr);
		Check(wavBytes && *wavBytes == wav && sErr.empty(),
			"pscenes.facade-reads-sound-bytes");
	}

	// --- the id->scene join ---
	{
		std::string tErr;
		auto t = scenes.ResolveSceneTarget(0x1000, tErr);
		Check(t && tErr.empty() && t->pak == "one.pak"
			&& t->sceneName == "Church" && t->sceneIndex == 0,
			"pscenes.target-recorded-name");
	}
	{
		std::string tErr;
		auto t = scenes.ResolveSceneTarget(0x2000, tErr);
		Check(!t && tErr.find("SCENE") != std::string::npos,
			"pscenes.target-kind-refusal");
	}
	{
		std::string tErr;
		Check(!scenes.ResolveSceneTarget(0x9999, tErr) && !tErr.empty(),
			"pscenes.target-unknown");
	}
	{
		// A multi-scene pak without a recorded name refuses loudly
		// (recook) — never "whatever scene sits at index 0".
		std::string tErr;
		auto t = scenes.ResolveSceneTarget(0x1001, tErr);
		Check(!t && tErr.find("recook") != std::string::npos,
			"pscenes.target-multiscene-refusal");
	}

	// --- loads (the GPU-less offline cluster seam) ---
	scenes.clusterLoader =
		[](smvk::World& world, smvk::ResourceManager* ctx,
			const smvk::PakReader& reader, uint32_t sceneIndex,
			smvk::TextureStreamer* streamer,
			std::shared_ptr<const std::vector<uint8_t>> imagePin,
			const smvk::ExternalTextureResolver* externalTexture,
			std::string& loadErr) -> smvk::Cluster* {
		(void)ctx; (void)reader; (void)sceneIndex; (void)streamer;
		(void)imagePin; (void)externalTexture; (void)loadErr;
		// The test_world_entities recipe: an offline cluster with one
		// root node + one light, registered into the world — the
		// resolve folds it into the merged scene and the light table
		// proves the fold happened.
		auto c = std::make_unique<smvk::Cluster>(nullptr);
		::SceneLight def{};
		def.type = ::SceneLight::Type::Point;
		def.intensity = 100.0f;
		def.name = "test_light";
		auto handle = c->CreateLight(def);
		const int node = addNode(c->scene, -1, 0);
		SetLight(c->scene, uint32_t(node), handle.index());
		smvk::Cluster* raw = c.get();
		world.RegisterCluster(std::move(c));
		return raw;
	};
	smvk::World world;
	app::SceneHandle h1 = scenes.LoadSceneAdditive(world, nullptr,
		nullptr, 0x1000, err);
	Check(bool(h1) && err.empty(), "pscenes.load-additive");
	Check(scenes.LoadedSceneCount() == 1, "pscenes.count-1");
	Check(scenes.SceneCluster(h1) != nullptr
		&& scenes.SceneContentId(h1) == 0x1000,
		"pscenes.instance-row");
	Check(world.lightLocations.size() == 1,
		"pscenes.world-light-folded");
	// Two instances of the SAME scene (two churches): distinct handles,
	// both alive.
	app::SceneHandle h2 = scenes.LoadSceneAdditive(world, nullptr,
		nullptr, 0x1000, err);
	Check(bool(h2) && h2 != h1 && scenes.LoadedSceneCount() == 2
		&& world.lightLocations.size() == 2,
		"pscenes.second-instance");

	// --- remove + the stale-handle guarantee ---
	{
		std::vector<uint64_t> released;
		std::string rErr;
		Check(scenes.RemoveScene(h1, world, rErr, &released)
			&& rErr.empty(), "pscenes.remove");
		Check(!scenes.IsLoaded(h1) && scenes.LoadedSceneCount() == 1
			&& world.lightLocations.size() == 1,
			"pscenes.removed-folds-away");
		std::string staleErr;
		Check(!scenes.RemoveScene(h1, world, staleErr)
			&& !staleErr.empty(),
			"pscenes.stale-remove-refused");
	}

	// --- ownership-aware prefetch ---
	scenes.SceneAcquireAsset(h2, 0x2000);
	scenes.SceneAcquireAsset(h2, 0x2000); // idempotent per scene
	Check(scenes.IsHeld(0x2000), "pscenes.held-by-scene");
	Check(scenes.Prefetch(0x2000, err), "pscenes.prefetch");
	Check(scenes.IsPinned(0x2000) && scenes.IsHeld(0x2000),
		"pscenes.pinned");
	{
		std::vector<uint64_t> released;
		std::string rErr;
		Check(scenes.RemoveScene(h2, world, rErr, &released),
			"pscenes.remove-2");
		// The pin survives the scene's removal: the texture id is NOT
		// reported as dropped and stays held.
		Check(released.empty() && scenes.IsHeld(0x2000),
			"pscenes.pin-survives-removal");
		Check(scenes.Release(0x2000) && !scenes.IsHeld(0x2000),
			"pscenes.fully-released");
		Check(!scenes.Release(0x2000),
			"pscenes.release-unpinned-refused");
	}
	{
		std::string pErr;
		Check(!scenes.Prefetch(0x9999, pErr) && !pErr.empty(),
			"pscenes.prefetch-unknown");
	}
	// The unpinned drop list: a scene-owned id with NO pin is reported
	// exactly once, on the LAST owner's removal.
	{
		app::SceneHandle a = scenes.LoadSceneAdditive(world, nullptr,
			nullptr, 0x1000, err);
		app::SceneHandle b = scenes.LoadSceneAdditive(world, nullptr,
			nullptr, 0x1000, err);
		Check(bool(a) && bool(b), "pscenes.reload-for-ownership");
		scenes.SceneAcquireAsset(a, 0x2000);
		scenes.SceneAcquireAsset(b, 0x2000);
		std::vector<uint64_t> releasedA;
		std::string rErrA;
		scenes.RemoveScene(a, world, rErrA, &releasedA);
		std::vector<uint64_t> releasedB;
		std::string rErrB;
		scenes.RemoveScene(b, world, rErrB, &releasedB);
		Check(releasedA.empty() && releasedB.size() == 1
			&& releasedB[0] == 0x2000,
			"pscenes.drop-reported-on-last-owner");
	}

	// --- replace semantics ---
	{
		app::SceneHandle a = scenes.LoadSceneAdditive(world, nullptr,
			nullptr, 0x1000, err);
		app::SceneHandle b = scenes.LoadSceneAdditive(world, nullptr,
			nullptr, 0x1000, err);
		Check(bool(a) && bool(b) && scenes.LoadedSceneCount() == 2,
			"pscenes.pre-replace");
		app::SceneHandle c = scenes.LoadScene(world, nullptr, nullptr,
			0x1000, err);
		Check(bool(c) && scenes.LoadedSceneCount() == 1
			&& !scenes.IsLoaded(a) && !scenes.IsLoaded(b),
			"pscenes.replace-unloads-all");
	}
}

} // namespace

int main() {
	TestParallelCrcFirstUse();
	TestCliParse();
	TestPakPathResolution();
	TestPakCorruption();
	TestDeployCheck();
	TestStaticFlagApplication();
	TestProjectRuntime();
	TestProjectScenes();
	std::cout << "pak_host_tests: " << (g_checks - g_failures) << "/"
		<< g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
