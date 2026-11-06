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
#include <limits>
#include <stdexcept>
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
#include "Runtime.h"
#include "AssetStore.h"
#include "AssetManager.h"
#include "DeployCheck.h"

#include <Engine/Asset/asset_plan.h>

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
		"cli.pak.required-value");	Check(ParseOk({ "App", "--pak=y.pak", "--scene-index=3" }, opts)
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
	// THE REMOVED --gltf (the App never touches glTF — import-only
	// content, the editor/import.exe own it): with the special-case
	// handler gone, the flag is exactly any other unknown argument.
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--gltf" }, ec)
			&& ec != 0, "cli.gltf-is-unknown-arg");
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
		cfg.extraElements.push_back(
			[&hookRan](smvk::App&, engine::WorldRenderer&,
				std::shared_ptr<smvk::World*>,
				const std::shared_ptr<app::ProjectRuntime>&) {
			hookRan = true;
			return nullptr;
		});
		cfg.extraOwnsCamera = true;
		app::ViewerOptions& optsRef = cfg.cli;
		Check(ParseOk({ "App", "--pak", "x.pak" }, optsRef)
			&& !cfg.extraElements.empty() && !hookRan
			&& cfg.extraOwnsCamera,
			"cli.parse.preserves-host-config");
	}
	// ---- The multi-pak + --scene surface (the discovery lane's CLI):
	// --pak is REPEATABLE (one legacy lane, several = the multi-pak
	// discovery lane), --scene names the entry by its cooked identity,
	// and the addressing forms never mix.
	{
		app::ViewerOptions opts;
		Check(ParseOk({ "App", "--pak", "a.pak", "--pak", "b.pak",
				"--scene", "Crypt" }, opts)
			&& opts.pakPaths.size() == 2 && opts.pakPaths[1] == "b.pak"
			&& opts.pakPath == "a.pak" && opts.sceneName == "Crypt"
			&& opts.sceneIndex == 0,
			"cli.multi-pak-scene-parse");
		app::ViewerOptions eq;
		Check(ParseOk({ "App", "--pak=a.pak", "--scene=Crypt" }, eq)
			&& eq.pakPaths.size() == 1 && eq.sceneName == "Crypt",
			"cli.scene-equals-form");
		// Index vs identity never mixes.
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--scene", "S",
				"--scene-index", "1" }, ec) && ec != 0,
			"cli.scene-vs-index-exclusive");
		// --scene has no effect with --project (the manifest names the
		// entry by identity).
		Check(ParseFails({ "App", "--project", "g.tapp", "--scene",
				"S" }, ec) && ec != 0,
			"cli.scene-with-project-refused");
	}
}

// ---- THE APPLICATION-FACING RUNTIME (app::RunGame's pieces) ----------
// Headless: the packaged-default entry resolution (the pure half).
// The game element's hook pump + the context's services ride a real
// graphics host — the golden/e2e lanes own those (the GameProject
// sample's BUILD instantiates RunGame + the typed gate end to end).

void TestRuntime() {
	// ArgsWithDefaultEntry: the packaged appmanifest beside the EXE
	// becomes the entry point unless the caller named one; an empty
	// default disables the whole lane.
	{
		const char* argv[2] = { "Game", "--irrelevant-flag" };
		std::vector<const char*> args = app::ArgsWithDefaultEntry(2,
			argv, "game.tapp");
		Check(args.size() == 4 && std::string(args[2]) == "--project"
			&& std::string(args[3]).find("game.tapp")
				!= std::string::npos,
			"rt.default-entry-appended");
		// A caller-named entry (either form) wins: nothing appended.
		const char* argvPak[3] = { "Game", "--pak", "x.pak" };
		std::vector<const char*> withPak = app::ArgsWithDefaultEntry(3,
			argvPak, "game.tapp");
		Check(withPak.size() == 3, "rt.explicit-pak-wins");
		const char* argvProject[3] = { "Game", "--project", "y.tapp" };
		std::vector<const char*> withProject =
			app::ArgsWithDefaultEntry(3, argvProject, "game.tapp");
		Check(withProject.size() == 3, "rt.explicit-project-wins");
		// The empty default disables the lane (the caller MUST name
		// an entry — the parse gate below refuses otherwise).
		std::vector<const char*> disabled = app::ArgsWithDefaultEntry(
			2, argv, "");
		Check(disabled.size() == 2, "rt.empty-default-disabled");
	}
	// The GameLaunchOptions defaults: one service-less camera-free
	// launch with the game.tapp default.
	{
		app::GameLaunchOptions opts;
		Check(opts.services.empty() && !opts.extraOwnsCamera
			&& opts.defaultManifest == "game.tapp",
			"rt.launch-options-defaults");
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
	// Payload must reject copied and foreign TOC entries before touching
	// the per-reader verification/decompression caches.
	{
		std::string err;
		auto first = smvk::PakReader::Open(image, err);
		auto second = smvk::PakReader::Open(image, err);
		Check(first.has_value() && second.has_value(),
			"pakcorruption.provenance-readers-open");
		if (first && second) {
			const auto* owned = first->Find(smvk::PakAssetKind::Scene, "scene/only");
			const auto* foreign = second->Find(smvk::PakAssetKind::Scene, "scene/only");
			bool valid = owned && first->Payload(*owned).size() == 4;
			bool copiedRejected = false, foreignRejected = false;
			if (owned) {
				const smvk::PakTocEntry copy = *owned;
				try { (void)first->Payload(copy); }
				catch (const std::invalid_argument&) { copiedRejected = true; }
			}
			if (foreign) {
				try { (void)first->Payload(*foreign); }
				catch (const std::invalid_argument&) { foreignRejected = true; }
			}
			Check(valid && copiedRejected && foreignRejected,
				"pakcorruption.payload-rejects-unowned-entry");
		}
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
	// A torn page write (SMP8): the small mount still opens (the TOC
	// metadata is intact) - the corrupt PAGE fails at its fetch and
	// VerifyAll names it (the lazy per-page contract; the deployment
	// gate calls VerifyAll, the runtime never hashes at boot).
	{
		std::vector<uint8_t> torn = image;
		torn[32] ^= 0xFF; // the scene blob's first payload byte
		std::string err;
		auto backend = smvk::PakBackend::Open(
			std::make_shared<std::vector<uint8_t>>(torn), "torn", err);
		Check(backend != nullptr, "pakcorruption.torn-mount-tolerates");
		bool caught = false;
		if (backend) {
			try {
				const auto* e = backend->Reader().Find(
					smvk::PakAssetKind::Scene, "scene/only");
				if (e)
					(void)backend->Reader().Payload(*e);
			}
			catch (const std::runtime_error&) {
				caught = true;
			}
			std::string verr;
			Check(caught
				&& !backend->Reader().VerifyAll(verr),
				"pakcorruption.torn-page-detected");
		}
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
		// THE PER-PAK IDENTITY TABLE (the "#ids" registry blob): the
		// runtime reads the records from THE PAK (the discovery lane —
		// the manifest's [ids] mirror is a vestigial compatibility
		// field the runtime ignores).
		const std::vector<tour::asset::PakAssetIdRow> idRows = {
			{ 0x2000, smvk::kCatalogKindTexture, vfsPath, "" },
			{ 0x3000, smvk::kCatalogKindMesh,
				"content/models/s/rock.geometry", "" },
			{ 0x1000, smvk::kCatalogKindScene,
				"content/scenes/s.tour", "" },
		};
		w.AddBlob(smvk::PakAssetKind::Identity,
			tour::asset::kPakAssetIdsPath,
			tour::asset::WriteAssetIdsBlob(idRows));
		const std::vector<uint8_t> image = w.Finalize();
		std::ofstream f(tmp / "shared.pak",
			std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(image.data()),
			std::streamsize(image.size()));
	}
	const fs::path tapp = tmp / "game.tapp";
	{
		std::ofstream f(tapp, std::ios::binary | std::ios::trunc);
		// appmanifest 2 (the RELEASE POINTER): entry + [paks] — no
		// [ids] mirror (the paks' own tables are the catalog), no
		// `catalog` line (the registry's record fold is the deployed
		// fingerprint).
		f << "# Tourmaline Runtime Project\n"
			<< "appmanifest 2\n"
			<< "entry shared.pak scene\n"
			<< "\n[paks]\nshared.pak\n";
	}
	std::string err;
	auto rt = app::ProjectRuntime::Load(tapp.generic_string(), err);
	Check(bool(rt) && err.empty(), "prt.load");
	if (rt) {
		Check(rt->Manifest().paks.size() == 1
			&& rt->Registry().RecordCount() == 3,
			"prt.manifest-parsed");
		const tour::asset::ContentRecord* record = rt->Asset(0x2000);
		Check(record && record->kind == smvk::kCatalogKindTexture
			&& record->pak == "shared.pak" && record->path == vfsPath,
			"prt.asset-row");
		// The convergent reverse lookup (the registry's (kind, path)
		// join — the same record the id form returns).
		Check(rt->Asset(0x9999) == nullptr
			&& rt->Registry().Find(smvk::kCatalogKindTexture, vfsPath)
				== record,
			"prt.asset-lookup");
		std::string rErr;
		auto bytes = rt->ReadAsset(0x2000, rErr);
		Check(bytes && *bytes == ktx && rErr.empty(),
			"prt.read-bytes");
		// THE SELECTIVE LANE (SMP8 + the pager): an asset read on a pak
		// NOBODY preloaded does ONE positional page read — no backend
		// was ever created, no whole-image preload ran (the small-load
		// acceptance contract). The load-time DISCOVERY consumed one
		// page first (the pak's own "#ids" identity table — the records
		// come from THE PAK now), so the counters start at 1: the asset
		// read is exactly ONE more.
		{
			Check(!rt->HasPak("shared.pak"),
				"prt.pager-no-backend-preloaded");
			std::string perr;
			std::shared_ptr<smvk::PakPager> pager =
				rt->OpenPakPager("shared.pak", perr);
			Check(pager && perr.empty(), "prt.pager-opens-lazily");
			if (pager) {
				const auto stats = pager->PageStats();
				const auto fileSize = fs::file_size(
					fs::path(rt->PakPath("shared.pak")));
				Check(stats.diskReads == 2 // discovery's #ids + the asset
					&& stats.bytesRead > 0
					&& stats.bytesRead < fileSize,
					"prt.pager-selective-bytes");
				// The cached pager is THE one the read used (the lazy
				// cache): a second read attaches, no new disk read
				// (the count stays at the discovery + first-read pair).
				std::string rErr2;
				auto again = rt->ReadAsset(0x2000, rErr2);
				Check(again && *again == ktx
					&& pager->PageStats().diskReads == 2,
					"prt.pager-second-read-attaches");
			}
		}
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
		// ---- THE CONVERGENCE CONTRACT through the façade (the
		// consolidated content architecture's first acceptance
		// contract): a mod pak serving the record's path overrides
		// BOTH read forms — the id form (the compiled game) and the
		// path form (the serialized-content lane) resolve through the
		// SAME provider selection, and a toggle reveals the base
		// provider beneath.
		{
			std::vector<uint8_t> ktxMod = ktx;
			ktxMod.push_back(0xCCu);
			const fs::path modPak = tmp / "mod.pak";
			{
				smvk::PakWriter w;
				w.AddBlob(smvk::PakAssetKind::Texture, vfsPath,
					smvk::BuildTexturePayload(vfsPath, -1, -1, -1,
						-1, std::span<const uint8_t>(ktxMod)));
				const std::vector<uint8_t> image = w.Finalize();
				std::ofstream f(modPak, std::ios::binary
					| std::ios::trunc);
				f.write(reinterpret_cast<const char*>(image.data()),
					std::streamsize(image.size()));
			}
			std::string oErr;
			Check(rt->AddPakOverride("mod",
					modPak.generic_string(), oErr) && oErr.empty(),
				"prt.override-registers");
			std::string idErr;
			auto idBytes = rt->ReadAsset(0x2000, idErr);
			std::string pErr;
			auto pathBytes = rt->ReadAssetByPath(
				smvk::kCatalogKindTexture, vfsPath, pErr);
			Check(idBytes && *idBytes == ktxMod && idErr.empty(),
				"prt.override-wins-id-form");
			Check(pathBytes && *pathBytes == ktxMod && pErr.empty(),
				"prt.override-wins-path-form");
			Check(idBytes && pathBytes && *idBytes == *pathBytes,
				"prt.forms-agree");
			// The between-launches toggle: disabling the override
			// reveals the base provider beneath (the shared.pak
			// pager lane — no backend was ever adopted here).
			rt->Registry().SetProviderEnabled("mod", false);
			std::string baseErr;
			auto baseBytes = rt->ReadAsset(0x2000, baseErr);
			Check(baseBytes && *baseBytes == ktx && baseErr.empty(),
				"prt.toggle-reveals-base");
		}
	}
	else {
		Check(false, "prt.manifest-parsed");
		Check(false, "prt.asset-row");
		Check(false, "prt.asset-lookup");
		Check(false, "prt.read-bytes");
		Check(false, "prt.pager-no-backend-preloaded");
		Check(false, "prt.pager-selective-bytes");
		Check(false, "prt.override-registers");
		Check(false, "prt.override-wins-id-form");
		Check(false, "prt.override-wins-path-form");
		Check(false, "prt.forms-agree");
		Check(false, "prt.toggle-reveals-base");
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
		// THE PER-PAK IDENTITY TABLE (the records the runtime reads —
		// the manifest's [ids] mirror is vestigial): the scene row
		// carries the cooked NAME (the id->scene join).
		const std::vector<tour::asset::PakAssetIdRow> idRows = {
			{ 0x1000, smvk::kCatalogKindScene,
				"content/scenes/church.tour", "Church" },
			{ 0x2000, smvk::kCatalogKindTexture, texPath, "" },
			{ 0x4000, smvk::kCatalogKindSound, wavPath, "" },
		};
		w.AddBlob(smvk::PakAssetKind::Identity,
			tour::asset::kPakAssetIdsPath,
			tour::asset::WriteAssetIdsBlob(idRows));
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
		// The scene row WITHOUT a name: the multi-scene pak refuses
		// loudly at load time (the single-scene inference has no
		// ground here — the exact refusal the battery pins).
		const std::vector<tour::asset::PakAssetIdRow> idRows = {
			{ 0x1001, smvk::kCatalogKindScene,
				"content/scenes/two.tour", "" },
		};
		w.AddBlob(smvk::PakAssetKind::Identity,
			tour::asset::kPakAssetIdsPath,
			tour::asset::WriteAssetIdsBlob(idRows));
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
	// emits as CatalogFingerprint). IDENTITY ONLY (id, kind, path) —
	// no package addressing in the fold.
	const uint64_t fpIds[4] = { 0x1000, 0x1001, 0x2000, 0x4000 };
	const uint8_t fpKinds[4] = { smvk::kCatalogKindScene,
		smvk::kCatalogKindScene, smvk::kCatalogKindTexture,
		smvk::kCatalogKindSound };
	const char* const fpPaths[4] = { "content/scenes/church.tour",
		"content/scenes/two.tour", "content/models/s/stone.png",
		"content/audio/bells.wav" };
	const uint64_t catalogFp = smvk::ComputeCatalogFingerprint(
		fpIds, fpKinds, fpPaths, 4);
	{
		std::ofstream f(tapp, std::ios::binary | std::ios::trunc);
		// appmanifest 2 (the RELEASE POINTER): entry + [paks] — the
		// records come from the paks' own "#ids" tables (added above),
		// the deployed fingerprint from the registry's record fold.
		f << "# Tourmaline Runtime Project\n"
			<< "appmanifest 2\n"
			<< "entry one.pak Church\n"
			<< "\n[paks]\none.pak\ntwo.pak\n";
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
			{ 0x1000, smvk::kCatalogKindScene,
				"content/scenes/church.tour", "" },
			{ 0x1001, smvk::kCatalogKindScene,
				"content/scenes/two.tour", "" },
			{ 0x2000, smvk::kCatalogKindTexture,
				"content/models/s/stone.png", "" },
			{ 0x4000, smvk::kCatalogKindSound,
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
				smvk::kCatalogKindSound, "content/audio/x.wav",
				"" } };
			std::string sErr;
			Check(!rt.AttachCompiledCatalog(stray, 1, catalogFp, sErr)
				&& sErr.find("0x20480") != std::string::npos,
				"pscenes.catalog-unknown-row-fails");
		}
		// A moved path fails loudly (identity never drifts silently:
		// the compiled row and the deployed record must agree on the
		// LOGICAL PATH, the one field beyond id + kind).
		{
			const Desc moved[4] = { assets[0], assets[1],
				{ 0x2000, smvk::kCatalogKindTexture,
					"content/models/s/moved.png", "" },
				assets[3] };
			std::string mErr;
			Check(!rt.AttachCompiledCatalog(moved, 4, catalogFp,
					mErr)
				&& mErr.find("moved") != std::string::npos,
				"pscenes.catalog-path-drift-fails");
		}
		// THE TYPED HELPER (the sample's bootstrap gate): a FOREIGN
		// project.h-shaped Desc proves its layout at instantiation
		// (static_asserts) and forwards — no reinterpret_cast in game
		// code, the pointer cast lives in ONE audited place.
		{
			struct GeneratedDesc {
				uint64_t id;
				uint8_t kind;
				std::string_view path;
				std::string_view sceneName;
			};
			const GeneratedDesc gen[4] = {
				{ 0x1000, smvk::kCatalogKindScene,
					"content/scenes/church.tour", "" },
				{ 0x1001, smvk::kCatalogKindScene,
					"content/scenes/two.tour", "" },
				{ 0x2000, smvk::kCatalogKindTexture,
					"content/models/s/stone.png", "" },
				{ 0x4000, smvk::kCatalogKindSound,
					"content/audio/bells.wav", "" },
			};
			std::string gErr;
			Check(app::AttachCompiledCatalog(rt, gen, 4, catalogFp,
					gErr)
				&& gErr.empty(),
				"pscenes.typed-helper-attaches");
			// The null/count convention: nullptr valid ONLY with
			// count 0.
			const GeneratedDesc* none = nullptr;
			std::string nErr;
			Check(!app::AttachCompiledCatalog(rt, none, 2, catalogFp,
					nErr)
				&& nErr.find("null pointer") != std::string::npos,
				"pscenes.typed-helper-null-nonzero-refused");
			std::string zErr;
			Check(app::AttachCompiledCatalog(rt, none, 0, catalogFp,
					zErr)
				&& zErr.empty(),
				"pscenes.typed-helper-null-zero-ok");
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

	// ---- Prefabs are independent scene instances (no new asset kind) --
	// Each .tour scene occurrence gets its own cluster root/frame.
	{
		glm::mat4 aFrame(1.0f);
		aFrame[3] = glm::vec4(10.0f, 2.0f, 3.0f, 1.0f);
		glm::mat4 bFrame(1.0f);
		bFrame[3] = glm::vec4(-7.0f, 0.0f, 9.0f, 1.0f);
		app::SceneHandle prefabA = scenes.InstantiatePrefab(
			world, nullptr, nullptr, 0x1000, aFrame, err);
		app::SceneHandle prefabB = scenes.InstantiatePrefab(
			world, nullptr, nullptr, 0x1000, bFrame, err);
		Check(prefabA && prefabB && prefabA != prefabB
			&& world.lightLocations.size() == 4, "prefab.two-occurrences");
		const auto* ca = scenes.SceneCluster(prefabA);
		const auto* cb = scenes.SceneCluster(prefabB);
		Check(ca && cb && ca != cb
			&& ca->scene.localTransform[0][3].x == 10.0f
			&& cb->scene.localTransform[0][3].x == -7.0f,
			"prefab.placement-isolated");
		glm::mat4 moved(1.0f);
		moved[3] = glm::vec4(22.0f, 0.0f, 0.0f, 1.0f);
		Check(scenes.SetPrefabTransform(prefabA, world, moved, err)
			&& scenes.SetPrefabTransform(prefabA, world, moved, err)
			&& scenes.SceneCluster(prefabA)->scene.localTransform[0][3].x == 22.0f
			&& scenes.SceneCluster(prefabB)->scene.localTransform[0][3].x == -7.0f,
			"prefab.absolute-move-no-drift");
		Check(scenes.PrefabTransform(prefabA).has_value()
			&& (*scenes.PrefabTransform(prefabA))[3].x == 22.0f,
			"prefab.query-transform");
		glm::mat4 invalid(1.0f);
		invalid[0][0] = std::numeric_limits<float>::infinity();
		const auto previous = scenes.SceneCluster(prefabA)->scene.localTransform[0];
		Check(!scenes.SetPrefabTransform(prefabA, world, invalid, err)
			&& scenes.SceneCluster(prefabA)->scene.localTransform[0][3].x
				== previous[3].x, "prefab.nonfinite-rejected");
		Check(!scenes.SetPrefabTransform(h1, world, moved, err),
			"prefab.normal-scene-refused");
		Check(scenes.RemoveScene(prefabA, world, err)
			&& !scenes.PrefabTransform(prefabA).has_value()
			&& !scenes.SetPrefabTransform(prefabA, world, moved, err)
			&& scenes.IsLoaded(prefabB) && world.lightLocations.size() == 3,
			"prefab.destroy-one-doesnt-touch-other");
		Check(scenes.RemoveScene(prefabB, world, err)
			&& scenes.LoadedSceneCount() == 2
			&& world.lightLocations.size() == 2, "prefab.cleanup");
	}
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

	// ---- THE DISCOVERY SESSION (the .tapp-free form) -------------------
	// ProjectRuntime::OpenDiscovered over LEGACY paks (no identity
	// blobs): the paks join as path providers, PakNames reflects them
	// (the manifest stays empty), the convergent path lane serves
	// bytes, and id reads refuse honestly (no identity rows — the
	// strict discovery lane's recook refusal is pinned in the
	// content-registry tests).
	{
		std::string dErr;
		const std::string pakArgs[2] = {
			(tmp / "one.pak").generic_string(),
			(tmp / "two.pak").generic_string() };
		auto rtD = app::ProjectRuntime::OpenDiscovered(
			{ pakArgs[0], pakArgs[1] }, dErr);
		Check(bool(rtD) && dErr.empty()
			&& rtD->PakNames().size() == 2
			&& rtD->PakNames()[0] == "one.pak"
			&& rtD->Manifest().paks.empty(),
			"dsc.session-opens-legacy-paks");
		if (rtD) {
			// The path lane serves through the discovered providers
			// (the pager lane — no backends adopted).
			std::string rErr;
			auto bytes = rtD->ReadAssetByPath(
				smvk::kCatalogKindTexture, texPath, rErr);
			Check(bytes && *bytes == ktx && rErr.empty(),
				"dsc.path-read-serves-legacy");
			// The manifest-relative PakPath consults the provider's
			// own source (the given pak path).
			Check(rtD->PakPath("one.pak") == pakArgs[0],
				"dsc.pakpath-uses-provider-source");
			// The discovery session's paks CARRY identity tables now
			// (the records come from the paks): ReadAsset by id serves
			// through the DISCOVERED records (the manifest mirror is
			// ignored — vestigial).
			std::string iErr;
			auto byId = rtD->ReadAsset(0x2000, iErr);
			Check(byId && *byId == ktx && iErr.empty(),
				"dsc.identity-rows-read-through-paks");
			// ProjectScenes over the discovered session: the ctor's
			// adopt-or-open walks PakNames (the session's providers,
			// manifest or not).
			smvk::OverlayVfs dVfs;
			app::ProjectScenes dScenes(*rtD, dVfs);
			Check(dScenes.Valid() && dVfs.Mounts().size() == 2,
				"dsc.scenes-ctor-adopts-discovered");
		}
	}
}

// ---------------------------------------------------------------------------
// THE ASSET MANAGER FACADE (app/src/AssetManager.h - the content-as-
// code load/blend surface, docs/content_as_code.md). The GI manifest
// parse + the generational handle registry + the kind refusals are
// direct; the MESH lane runs GPU-LESS through the AssetStore's
// cluster-source seam (the ProjectScenes clusterLoader discipline):
// hand-built mesh-carrying clusters stand in for the real deserialize
// (the production lane stays pinned by every cooked project launch),
// driving the shared-asset retention contract end to end - adoption,
// park-on-remove, no-recarve reloads, the session ref, the last-ref
// release. Only the renderer publish still needs a GPU.
// ---------------------------------------------------------------------------

// A hand-built offline cluster carrying ONE mesh family row (the
// GPU-less mesh-lane recipe: a pool mesh + its URI registration + a
// scene node row so the world resolve folds it into meshLocations -
// no GPU, no deserialize, the exact seam contract).
std::unique_ptr<smvk::Cluster> MeshFamilyCluster(const std::string& uri)
{
	auto c = std::make_unique<smvk::Cluster>(nullptr);
	smvk::StaticMesh m{};
	m.geometry.vertexCount = 3;
	m.geometry.indexCount = 3;
	m.geometry.aabbMin = glm::vec3(-1.0f);
	m.geometry.aabbMax = glm::vec3(1.0f);
	m.geometry.surfaces.push_back(smvk::Surface{0, 3, 0});
	const auto h = c->staticMeshes.create(std::move(m));
	c->RegisterMeshUri(uri, h);
	const int node = addNode(c->scene, -1, 0);
	SetMesh(c->scene, uint32_t(node), h.index());
	return c;
}

void TestAssetManager() {
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_am";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	// A pak carrying a GI manifest (a Texture-kind blob at a gi/ VFS
	// path - the payload channel the cooker uses), a named scene (the
	// mesh lane's carrier source), and TWO mesh families (the [ids]
	// join's Mesh-kind TOC blobs under the row paths' "#"-prefixes).
	const std::string manifestText =
		"# day set\n"
		"gi/scene_day_atrium.givolume\n"
		"gi/scene_day_hall.givolume\n"
		"# a legacy guid row (still parses)\n"
		"abcdef0123456789abcdef0123456789 gi/scene_day_cellar.givolume\n";
	const std::string texPath = "content/models/s/stone.png";
	const std::string treeFamily = "content/models/tree.geometry";
	const std::string rockFamily = "content/models/rock.geometry";
	{
		// A minimal KTX2 payload wrapped through BuildTexturePayload
		// (the TestProjectRuntime recipe - the texture channel's
		// envelope).
		const std::vector<uint8_t> ktx = { 0xAB, 'K', 'T', 'X', ' ',
			'2', '0', 0x0D, 0x0A, 1, 2, 3, 4, 5 };
		smvk::PakWriter w;
		w.AddBlob(smvk::PakAssetKind::Texture, texPath,
			smvk::BuildTexturePayload(texPath, -1, -1, -1, -1,
				std::span<const uint8_t>(ktx)));
		w.AddBlob(smvk::PakAssetKind::Texture,
			"content/gi/day.givolumemanifest",
			smvk::BuildTexturePayload("content/gi/day.givolumemanifest",
				-1, -1, -1, -1,
				std::span<const uint8_t>(
					reinterpret_cast<const uint8_t*>(
						manifestText.data()),
					manifestText.size())));
		// The mesh lane: a named scene + the two families' blobs (the
		// tree family carries a /lod1 sibling too - the base-mesh
		// filter). The blob CONTENT is irrelevant on the seam path
		// (the hand-built clusters stand in for the deserialize); the
		// TOC paths are the join. The GHOST family carries blobs + an
		// [ids] row but NO plan entry - the join must refuse loudly
		// (a version skew - recook), never a silent empty.
		w.AddBlob(smvk::PakAssetKind::Scene, "recording/one",
			NamedSceneBlob("Meshes"));
		w.AddBlob(smvk::PakAssetKind::Mesh,
			treeFamily + "#mesh.0/prim.0", { 1, 2, 3, 4 });
		w.AddBlob(smvk::PakAssetKind::Mesh,
			treeFamily + "#mesh.0/prim.0/lod1", { 5, 6, 7, 8 });
		w.AddBlob(smvk::PakAssetKind::Mesh,
			rockFamily + "#mesh.0/prim.0", { 9, 10, 11, 12 });
		w.AddBlob(smvk::PakAssetKind::Mesh,
			"content/models/ghost.geometry#mesh.0/prim.0", { 13, 14 });
		// SMP8: the cooked mesh plan (the family -> member join the
		// runtime reads - never a TOC string scan).
		{
			std::vector<smvk::PakMeshPlanFamily> plan = {
				{ treeFamily,
					{ { treeFamily + "#mesh.0/prim.0", 0 },
					  { treeFamily + "#mesh.0/prim.0/lod1", 1 } } },
				{ rockFamily,
					{ { rockFamily + "#mesh.0/prim.0", 0 } } },
			};
			w.AddBlob(smvk::PakAssetKind::Plan, smvk::kPakMeshPlanPath,
				smvk::WriteMeshPlanBlob(plan));
		}
		// THE PER-PAK IDENTITY TABLE (the records the runtime reads —
		// the manifest's [ids] mirror is vestigial): the scene row
		// carries the cooked NAME, the ghost family carries an
		// identity but NO plan entry (the join's loud refusal).
		const std::vector<tour::asset::PakAssetIdRow> idRows = {
			{ 0x1001, smvk::kCatalogKindScene,
				"content/scenes/meshes.tour", "Meshes" },
			{ 0x2000, smvk::kCatalogKindTexture, texPath, "" },
			{ 0x3000, smvk::kCatalogKindMesh, treeFamily, "" },
			{ 0x3001, smvk::kCatalogKindMesh, rockFamily, "" },
			{ 0x3002, smvk::kCatalogKindMesh,
				"content/models/ghost.geometry", "" },
			{ 0x6000, smvk::kCatalogKindGi,
				"content/gi/day.givolumemanifest", "" },
		};
		w.AddBlob(smvk::PakAssetKind::Identity,
			tour::asset::kPakAssetIdsPath,
			tour::asset::WriteAssetIdsBlob(idRows));
		const std::vector<uint8_t> image = w.Finalize();
		std::ofstream f(tmp / "one.pak", std::ios::binary
			| std::ios::trunc);
		f.write(reinterpret_cast<const char*>(image.data()),
			std::streamsize(image.size()));
	}
	const fs::path tapp = tmp / "game.tapp";
	{
		std::ofstream f(tapp, std::ios::binary | std::ios::trunc);
		// appmanifest 2 (the RELEASE POINTER): entry + [paks] — the
		// records come from one.pak's own "#ids" table (added above).
		f << "# Tourmaline Runtime Project\n"
			<< "appmanifest 2\n"
			<< "entry one.pak x\n"
			<< "\n[paks]\none.pak\n";
	}
	std::string err;
	auto rtOpt = app::ProjectRuntime::Load(tapp.generic_string(), err);
	Check(bool(rtOpt) && err.empty(), "am.load-manifest");
	if (!rtOpt) {
		Check(false, "am.gi-load-parse");
		Check(false, "am.gi-dedup");
		Check(false, "am.gi-kind-refusal");
		Check(false, "am.mesh-unknown-refused");
		Check(false, "am.mesh-null-ctx-refused");
		Check(false, "am.mesh-two-instances-folded");
		Check(false, "am.mesh-adopts-scene-cluster");
		Check(false, "am.mesh-remove-preserves-asset");
		Check(false, "am.mesh-last-ref-releases");
		Check(false, "am.mesh-seam-load");
		return;
	}
	smvk::World world;
	app::AssetManager assets(*rtOpt, world, nullptr, nullptr);
	// ---- the GI lane ----
	{
		app::GiHandle day = assets.LoadGI(0x6000, err);
		Check(bool(day) && err.empty(), "am.gi-load-parse");
		// The dedup: the same key returns the SAME handle.
		std::string dErr2;
		app::GiHandle day2 = assets.LoadGI(0x6000, dErr2);
		Check(bool(day2) && day2 == day && dErr2.empty(),
			"am.gi-dedup");
		Check(assets.LoadedGiCount() == 1, "am.gi-count");
		// A wrong-kind key refuses loudly (the typed-key contract).
		std::string kErr;
		Check(!assets.LoadGI(0x2000, kErr)
			&& kErr.find("MESH") == std::string::npos
			&& kErr.find("GI ids") != std::string::npos,
			"am.gi-kind-refusal");
		// An unknown id refuses.
		std::string uErr;
		Check(!assets.LoadGI(0x9999, uErr) && !uErr.empty(),
			"am.gi-unknown-refused");
		// SetGI/SetGIBlend without the renderer seam refuse (the
		// GPU-less test seam - the publish needs the renderer).
		Check(!assets.SetGI(day), "am.am.setgi-no-renderer-refused");
		Check(!assets.SetGIBlend(day, day, 0.5f),
			"am.setgiblend-no-renderer-refused");
		// Unload + the stale-handle guarantee.
		Check(assets.UnloadGI(day), "am.gi-unload");
		Check(!assets.UnloadGI(day), "am.gi-stale-unload-refused");
		Check(assets.LoadedGiCount() == 0, "am.gi-count-after-unload");
		// A re-load after the unload re-registers (a fresh handle).
		std::string rErr;
		app::GiHandle day3 = assets.LoadGI(0x6000, rErr);
		Check(bool(day3) && day3 != day, "am.gi-reload-after-unload");
	}
	// ---- the MESH lane's CPU-refused legs ----
	{
		std::string mErr;
		Check(!assets.LoadMesh(0x9999, mErr) && !mErr.empty(),
			"am.mesh-unknown-refused");
		// LoadMesh without a GPU context AND without a cluster-source
		// seam refuses loudly (the real deserialize is the DEFAULT
		// source); the manifest still resolves the id (the refusal is
		// the CONTEXT, not the key).
		std::string cErr;
		Check(!assets.LoadMesh(0x3000, cErr)
			&& cErr.find("GPU context") != std::string::npos,
			"am.mesh-null-ctx-refused");
		// The plan-less family refuses loudly (the cooked mesh plan is
		// the ONLY join - a family missing from it is a version skew,
		// recook; never a silent empty, never a TOC string scan).
		std::string gErr;
		Check(!assets.LoadMesh(0x3002, gErr)
			&& gErr.find("recook") != std::string::npos,
			"am.mesh-no-plan-refused");
		// The null-pointer batch convention: nullptr is valid ONLY
		// with count == 0 (a zero batch succeeds trivially).
		std::string zErr;
		const app::MeshBatch zero =
			assets.LoadMeshes(nullptr, 0, zErr);
		Check(zero.count == 0 && zErr.empty(),
			"am.mesh-batch-null-zero-ok");
		const app::MeshBatch bad =
			assets.LoadMeshes(nullptr, 2, zErr);
		Check(bad.count == 0 && !zErr.empty(),
			"am.mesh-batch-null-nonzero-refused");
	}

	// ---- the SHARED-ASSET RETENTION battery (the park-on-unload
	// bridge, end to end through the facades): LoadMesh adopts a
	// scene's carrier, RemoveScene parks it while the handle lives,
	// the other scene instance is untouched, a reload shares the
	// parked carrier (no re-carve), and the last reference releases.
	{
		smvk::OverlayVfs vfs;
		app::ProjectScenes scenes(*rtOpt, vfs);
		Check(scenes.Valid(), "am.mesh-scenes-ctor");
		// The scene loader builds a mesh-carrying cluster per instance
		// (the pscenes GPU-less seam recipe + one mesh family row).
		scenes.clusterLoader =
			[](smvk::World& w, smvk::ResourceManager* ctx,
				const smvk::PakReader& reader, uint32_t sceneIndex,
				smvk::TextureStreamer* streamer,
				std::shared_ptr<const std::vector<uint8_t>> imagePin,
				const smvk::ExternalTextureResolver* externalTexture,
				std::string& loadErr) -> smvk::Cluster* {
			(void)ctx; (void)reader; (void)sceneIndex; (void)streamer;
			(void)imagePin; (void)externalTexture; (void)loadErr;
			auto c = MeshFamilyCluster(
				"content/models/tree.geometry#mesh.0/prim.0");
			smvk::Cluster* raw = c.get();
			w.RegisterCluster(std::move(c));
			return raw;
		};
		app::SceneHandle s1 = scenes.LoadSceneAdditive(world, nullptr,
			nullptr, 0x1001, err);
		app::SceneHandle s2 = scenes.LoadSceneAdditive(world, nullptr,
			nullptr, 0x1001, err);
		Check(bool(s1) && bool(s2) && scenes.LoadedSceneCount() == 2
			&& world.meshLocations.size() == 2,
			"am.mesh-two-instances-folded");
		// LoadMesh ADOPTS the scene's carrier (the store's
		// world-cluster search - the load seam must never run: an
		// already-loaded scene IS the carrier).
		bool seamRan = false;
		assets.Store().clusterSource =
			[&seamRan](smvk::World&, const smvk::PakReader&, uint32_t,
				std::string&) -> smvk::Cluster* {
			seamRan = true;
			return nullptr;
		};
		std::string mErr;
		const app::MeshHandle tree = assets.LoadMesh(0x3000, mErr);
		Check(bool(tree) && mErr.empty() && !seamRan,
			"am.mesh-adopts-scene-cluster");
		Check(assets.MeshWorldIndex(tree) != UINT32_MAX,
			"am.mesh-worldindex");
		// Native World gameplay path: the key is loaded explicitly,
		// then the LOADED MeshHandle attaches to a standalone node.
		// No CatalogWorld wrapper, and never two meshes on one node.
		std::string wErr;
		Check(world.LoadMesh(0x3000, wErr) == tree && wErr.empty(),
			"am.world-loadmesh-catalog-key");
		const NodeHandle gameNode = world.AddNode();
		const NodeId gameSlot = NodeHandleSlot(gameNode);
		Check(world.IsGameNode(gameNode) && !FindMesh(world.scene, gameSlot),
			"am.world-addnode-empty");
		Check(world[gameNode].SetMesh(tree)
			&& FindMesh(world.scene, gameSlot)
			&& FindMesh(world.scene, gameSlot)->mesh
				== assets.MeshWorldIndex(tree),
			"am.world-setmesh-loaded-handle");
		Check(!world[gameNode].SetMesh(app::MeshHandle{})
			&& FindMesh(world.scene, gameSlot) != nullptr,
			"am.world-invalid-handle-preserves-existing-mesh");
		Check(world[gameNode].SetPhysicsBody({})
			&& world[gameNode].SetAudioRoom({})
			&& world[gameNode].SetTriggerVolume({}),
			"am.world-node-inline-default-components");
		uint32_t gameMeshRows = 0;
		for (const MeshRow& row : world.scene.tables.meshes)
			if (row.node == gameSlot) ++gameMeshRows;
		Check(gameMeshRows == 1,
			"am.world-one-mesh-per-transform");
		Check(!assets.UnloadMesh(tree),
			"am.world-mesh-handle-cannot-unload-while-attached");
		Check(world[gameNode].RemoveMesh()
			&& FindMesh(world.scene, gameSlot) == nullptr,
			"am.world-remove-mesh-keeps-node");
		Check(world.RemoveNode(gameNode) && !world.IsGameNode(gameNode),
			"am.world-remove-node-invalidates-handle");
		// The typed-key spawn lane (the gameplay surface): the loaded
		// handle instances through its CURRENT row - the entity is
		// world-owned, DestroyMesh kills it.
		const smvk::MeshEntityHandle ent = assets.SpawnMesh(tree,
			glm::mat4(1.0f), UINT32_MAX);
		Check(bool(ent) && world.IsMeshEntityAlive(ent),
			"am.mesh-spawn-typed-key");
		Check(world.DestroyMesh(ent)
			&& !world.IsMeshEntityAlive(ent),
			"am.mesh-spawn-destroy");
		std::string dErr;
		Check(assets.LoadMesh(0x3000, dErr) == tree && dErr.empty(),
			"am.mesh-dedup");
		// REMOVE one scene instance: its carrier PARKS (the asset refs
		// are live) - the removed scene stays gone, the handle stays
		// valid, the OTHER instance's rows stay folded, and the
		// asset's world mapping reads honestly stale.
		std::vector<uint64_t> released;
		std::string rErr;
		Check(scenes.RemoveScene(s1, world, rErr, &released)
			&& !scenes.IsLoaded(s1) && scenes.IsLoaded(s2),
			"am.mesh-scene-gone");
		Check(assets.MeshValid(tree)
			&& world.retainedClusters.size() == 1,
			"am.mesh-remove-preserves-asset");
		Check(world.meshLocations.size() == 1,
			"am.mesh-other-instance-unaffected");
		Check(assets.MeshWorldIndex(tree) == UINT32_MAX,
			"am.mesh-worldindex-stale-after-remove");
		// The spawn refuses while the carrier is parked (no CURRENT
		// row - never a stale-row instance); the ASSET itself stays
		// valid (the resource vs instance split).
		Check(!assets.SpawnMesh(tree, glm::mat4(1.0f), UINT32_MAX)
			&& assets.MeshValid(tree),
			"am.mesh-spawn-refused-while-parked");
		// A re-load shares the PARKED carrier: the same handle, no
		// fresh cluster, no re-carve of GPU ranges.
		std::string r2Err;
		Check(assets.LoadMesh(0x3000, r2Err) == tree
			&& world.retainedClusters.size() == 1,
			"am.mesh-reload-shared-no-recarve");
		// Unload the handle: keepResident's SESSION ref keeps the
		// parked carrier alive.
		Check(assets.UnloadMesh(tree), "am.mesh-unload");
		Check(!assets.UnloadMesh(tree),
			"am.mesh-stale-unload-refused");
		Check(world.retainedClusters.size() == 1,
			"am.mesh-resident-survives-unload");
		// Teardown drops the session ref: the LAST reference releases
		// the parked carrier for destruction.
		assets.UnloadAllMeshes();
		Check(world.retainedClusters.empty(),
			"am.mesh-last-ref-releases");
		// The surviving scene instance still removes cleanly (nothing
		// the asset lane did pins it).
		std::string r3Err;
		std::vector<uint64_t> released2;
		Check(scenes.RemoveScene(s2, world, r3Err, &released2)
			&& world.retainedClusters.empty(),
			"am.mesh-other-instance-removes-clean");
	}

	// ---- the CLUSTER-SOURCE SEAM battery (the load lane, no scene):
	// the store's walk resolves the family through the seam, the
	// world index resolves FRESH across rebuilds, the batch keeps its
	// per-slot shape, and a transient (non-resident) acquire releases
	// with the handle.
	{
		int seamCalls = 0;
		assets.Store().clusterSource =
			[&seamCalls](smvk::World& w, const smvk::PakReader& reader,
				uint32_t sceneIndex, std::string& seamErr)
			-> smvk::Cluster* {
			(void)reader; (void)sceneIndex; (void)seamErr;
			++seamCalls;
			auto c = MeshFamilyCluster(
				"content/models/rock.geometry#mesh.0/prim.0");
			smvk::Cluster* raw = c.get();
			w.RegisterCluster(std::move(c));
			return raw;
		};
		// A TRANSIENT acquire (keepResident=false): only the handle's
		// reference, no session ref.
		app::MeshLoadOptions transient;
		transient.keepResident = false;
		std::string mErr;
		const app::MeshHandle rock =
			assets.LoadMesh(0x3001, mErr, transient);
		Check(bool(rock) && mErr.empty() && seamCalls == 1,
			"am.mesh-seam-load");
		Check(assets.MeshWorldIndex(rock) != UINT32_MAX,
			"am.mesh-seam-worldindex");
		// The typed-key spawn through the seam-loaded carrier (the
		// end-to-end lane: key -> load -> spawn -> destroy).
		const smvk::MeshEntityHandle rent = assets.SpawnMesh(
			rock, glm::mat4(1.0f), UINT32_MAX);
		Check(bool(rent) && world.IsMeshEntityAlive(rent)
			&& world.DestroyMesh(rent),
			"am.mesh-seam-spawn-destroy");
		// The FRESH resolution: a world rebuild never stales the
		// query (the location tables are re-scanned per call).
		world.ResolveAndFixup_clusters();
		Check(assets.MeshWorldIndex(rock) != UINT32_MAX,
			"am.mesh-worldindex-fresh-across-resolve");
		// The batch: mixed valid/unknown keys keep the batch shape
		// (failed slots read as invalid handles - per-slot
		// inspection), and duplicates dedup to the same handle. The
		// TRANSIENT options ride the batch too (no session ref).
		const std::uint64_t keys[3] = { 0x3001, 0x3001, 0x9999 };
		std::string bErr;
		const app::MeshBatch batch =
			assets.LoadMeshes(keys, 3, bErr, transient);
		Check(batch.count == 3 && bool(batch[0])
			&& batch[1] == batch[0] && !bool(batch[2])
			&& !bErr.empty(),
			"am.mesh-batch-mixed");
		// The residency UPGRADE: a default-options load over the live
		// transient handle takes the store's SESSION ref (the slot
		// flag and the store's ledger stay in lockstep).
		std::string uErr;
		Check(assets.LoadMesh(0x3001, uErr) == rock && uErr.empty()
			&& assets.Store().HeldClusterCount() == 1,
			"am.mesh-residency-upgrade");
		// The unload keeps the carrier alive now (the session ref):
		// the hold survives the transient handle's death.
		Check(assets.UnloadMesh(rock), "am.mesh-transient-unload");
		Check(assets.Store().HeldClusterCount() == 1,
			"am.mesh-resident-survives-transient-unload");
		// Teardown releases the session ref: the hold drops entirely.
		assets.UnloadAllMeshes();
		Check(assets.Store().HeldClusterCount() == 0
			&& assets.LoadedMeshCount() == 0,
			"am.mesh-transient-released");
		// A re-load after the release acquires FRESH (a new handle)
		// but still SHARES the registered carrier (no seam call, no
		// re-carve - repeated requests share asset resources).
		std::string rErr;
		const app::MeshHandle rock2 =
			assets.LoadMesh(0x3001, rErr, transient);
		Check(bool(rock2) && rock2 != rock && seamCalls == 1,
			"am.mesh-reload-shares-registered-carrier");
		assets.UnloadAllMeshes();
	}

	// ---- THE COMPILED-PLAN LANE + THE FAIL-CLOSED GATE ----------------
	// The private project_plans.h constants' runtime surface: the
	// slot-keyed load (no manifest row, no mesh-plan parse, no TOC
	// scan) + the boot gate (the deployed paks' layout vs the
	// compiled hash - a mismatch is a LOUD abort, never a dynamic
	// fallback).
	{
		app::ProjectRuntime& rt = *rtOpt;
		// The test stands in for the generator: fold the deployed
		// one.pak the same way EmitPlansHeader does.
		std::string perr;
		std::shared_ptr<smvk::PakPager> pager =
			rt.OpenPakPager("one.pak", perr);
		Check(pager != nullptr, "planlane.pager-opens");
		const std::string names[1] = { "one.pak" };
		const std::string fps[1] = {
			smvk::PakLayoutFingerprint(pager->Reader())
		};
		const std::string hash =
			smvk::FoldPakLayouts(names, fps, 1);
		std::string gerr;
		Check(rt.AttachCompiledPlans(1, hash.c_str(), gerr)
			&& gerr.empty(), "planlane.gate-matches");
		Check(!rt.AttachCompiledPlans(2, hash.c_str(), gerr)
			&& gerr.find("never a dynamic fallback")
				!= std::string::npos,
			"planlane.gate-count-mismatch-fails-closed");
		Check(!rt.AttachCompiledPlans(1, "deadbeefdeadbeef", gerr)
			&& gerr.find("LAYOUT does not match")
				!= std::string::npos,
			"planlane.gate-hash-mismatch-fails-closed");
		// The compiled plan: the tree family's base slot (resolved
		// here the way the generator does - through the pak's mesh
		// plan; the CONSTANT carries it in the real game).
		uint32_t treeBaseSlot = UINT32_MAX;
		{
			std::vector<smvk::PakMeshPlanMember> members;
			std::string merr;
			Check(pager->Reader().MeshPlanFamily(treeFamily,
					members, merr)
				&& !members.empty(),
				"planlane.family-resolves");
			for (const smvk::PakMeshPlanMember& m : members)
				if (m.lodIndex == 0) {
					if (const auto* e = pager->Reader().Find(
							smvk::PakAssetKind::Mesh, m.tocPath))
						treeBaseSlot = e->slot;
					break;
				}
			Check(treeBaseSlot != UINT32_MAX,
				"planlane.base-slot-found");
		}
		if (treeBaseSlot != UINT32_MAX) {
			// The cluster-source seam: the plan lane's carrier load
			// goes through the SAME seam as the catalog lane (this
			// block's source builds the TREE family — the seam
			// battery's rock source is spent).
			assets.Store().clusterSource =
				[](smvk::World& w, const smvk::PakReader&,
					uint32_t, std::string&) -> smvk::Cluster* {
				auto c = MeshFamilyCluster(
					"content/models/tree.geometry#mesh.0/prim.0");
				smvk::Cluster* raw = c.get();
				w.RegisterCluster(std::move(c));
				return raw;
			};
			const smvk::MeshPlanRef treePlan{
				0u, treeBaseSlot, treeFamily.c_str() };
			std::string lerr;
			const app::MeshHandle byPlan =
				assets.LoadMeshByPlan(treePlan, lerr);
			Check(bool(byPlan) && lerr.empty(),
				"planlane.load-by-plan");
			Check(assets.MeshWorldIndex(byPlan) != UINT32_MAX,
				"planlane.worldindex");
			// The dedup by PLAN IDENTITY (the compiled address).
			std::string lerr2;
			Check(assets.LoadMeshByPlan(treePlan, lerr2) == byPlan,
				"planlane.dedup-by-plan-identity");
			// The full gameplay lane from a plan-loaded handle.
			const smvk::MeshEntityHandle pe = assets.SpawnMesh(
				byPlan, glm::mat4(1.0f), UINT32_MAX);
			Check(bool(pe) && world.IsMeshEntityAlive(pe)
				&& world.DestroyMesh(pe),
				"planlane.spawn-destroy");
			// THE WRONG SLOT fails closed: the ghost family's base
			// entry (a MESH slot, but not the plan's family) - the
			// per-asset fail-closed contract.
			uint32_t ghostSlot = UINT32_MAX;
			if (const auto* e = pager->Reader().Find(
					smvk::PakAssetKind::Mesh,
					"content/models/ghost.geometry#mesh.0/prim.0"))
				ghostSlot = e->slot;
			if (ghostSlot != UINT32_MAX) {
				const smvk::MeshPlanRef wrong{
					0u, ghostSlot, treeFamily.c_str() };
				std::string werr;
				Check(!assets.LoadMeshByPlan(wrong, werr)
					&& werr.find("never a dynamic fallback")
						!= std::string::npos,
					"planlane.wrong-slot-fails-closed");
			}
			// A bad pak index refuses loudly.
			const smvk::MeshPlanRef badPak{
				9u, treeBaseSlot, treeFamily.c_str() };
			std::string berr;
			Check(!assets.LoadMeshByPlan(badPak, berr)
				&& berr.find("version skew") != std::string::npos,
				"planlane.bad-pak-index-refused");
		}
	}
	fs::remove_all(tmp, ec);
}

// ---------------------------------------------------------------------------
// WORLD SHARED-ASSET RETENTION (Cluster::assetRefs + World's park/
// release) - the engine primitives under the AssetStore bridge, pinned
// directly: retain -> unload PARKS -> the last release destroys; a
// re-parenting UnregisterCluster pulls a parked cluster back out.
// ---------------------------------------------------------------------------
void TestWorldRetention() {
	smvk::World world;
	const auto mkCluster = [] {
		auto c = std::make_unique<smvk::Cluster>(nullptr);
		::SceneLight def{};
		def.type = ::SceneLight::Type::Point;
		def.intensity = 100.0f;
		def.name = "test_light";
		const auto h = c->CreateLight(def);
		const int node = addNode(c->scene, -1, 0);
		SetLight(c->scene, uint32_t(node), h.index());
		return c;
	};
	smvk::Cluster* a = nullptr;
	smvk::Cluster* b = nullptr;
	{
		auto ca = mkCluster();
		auto cb = mkCluster();
		a = ca.get();
		b = cb.get();
		world.RegisterCluster(std::move(ca));
		world.RegisterCluster(std::move(cb));
	}
	world.ResolveAndFixup_clusters();
	Check(world.lightLocations.size() == 2, "wretain.folded");
	// Two external refs on a.
	Check(world.RetainCluster(*a) && world.RetainCluster(*a)
		&& a->assetRefs == 2, "wretain.retain");
	// Releasing a cluster that was never retained is refused (a
	// no-op, never an underflow).
	Check(!world.ReleaseCluster(*b) && b->assetRefs == 0,
		"wretain.release-unreffed-refused");
	// Unload with live refs PARKS: the fold loses a's light exactly
	// like a destroy (removal never resurrects scene content), the
	// cluster stays alive in the retained list.
	world.UnloadCluster(*a);
	Check(world.clusters.size() == 1
		&& world.retainedClusters.size() == 1
		&& world.retainedClusters[0].get() == a
		&& world.lightLocations.size() == 1,
		"wretain.unload-parks");
	// Retain works on a PARKED cluster too.
	Check(world.RetainCluster(*a) && a->assetRefs == 3,
		"wretain.retain-parked");
	Check(world.ReleaseCluster(*a) && world.retainedClusters.size() == 1,
		"wretain.release-keeps-while-reffed");
	Check(world.ReleaseCluster(*a) && world.retainedClusters.size() == 1,
		"wretain.release-keeps-while-reffed-2");
	// The LAST release destroys the parked cluster (no resolve needed
	// - it is outside the fold).
	Check(world.ReleaseCluster(*a) && world.retainedClusters.empty(),
		"wretain.last-release-destroys");
	// UnregisterCluster pulls a PARKED cluster out for re-parenting
	// (the existing re-parenting use case, retained-list side).
	Check(world.RetainCluster(*b) && b->assetRefs == 1,
		"wretain.retain-for-reparent");
	world.UnloadCluster(*b);
	Check(world.retainedClusters.size() == 1,
		"wretain.park-for-reparent");
	std::unique_ptr<smvk::Cluster> detached =
		world.UnregisterCluster(*b);
	Check(bool(detached) && detached.get() == b
		&& world.retainedClusters.empty(),
		"wretain.unregister-pulled-from-retained");
	// The re-parenting holder owns the detached cluster now; its ref
	// travels with it (the count is the NEW owner's to drop).
}

} // namespace

int main() {
	// CTest captures stdout through a pipe: flush each check so a Linux
	// crash identifies the last completed assertion in the job log.
	std::cout << std::unitbuf;
	TestParallelCrcFirstUse();
	TestCliParse();
	TestRuntime();
	TestPakPathResolution();
	TestPakCorruption();
	TestDeployCheck();
	TestStaticFlagApplication();
	TestProjectRuntime();
	TestProjectScenes();
	TestAssetManager();
	TestWorldRetention();
	std::cout << "pak_host_tests: " << (g_checks - g_failures) << "/"
		<< g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
