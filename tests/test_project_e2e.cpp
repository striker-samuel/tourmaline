// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// project_e2e_tests: THE BUNDLED-PROJECT ACCEPTANCE GATE (CPU-only).
//
// The gap this test closes: nothing else in the suite builds the
// CHECKED-IN sample content end to end. test_project_pipeline's
// executor battery cooks SYNTHETIC fixtures; pak_host forges paks by
// hand. This test drives the real contributor flow over
// samples/04_project_game's checked-in sources:
//
//   1. COPY the sample project tree to a temp dir (a project dir is
//      SELF-CONTAINED — the copy IS the deployment test's subject).
//   2. COOK + PUBLISH via the real CLI: `Project.exe build --project
//      <copy> --recipe game.tourrecipe --publish <dir>` — the same
//      command the docs teach. The subprocess form is deliberate: the
//      stage adapter spawns the real Cook/GiBaker/Pak tools, exactly
//      what a contributor's machine runs.
//   3. VERIFY THE PUBLISHED PACKAGES (DeployCheck's contract, in
//      process): every manifest-listed pak exists, opens
//      (magic/CRC), passes the EXPLICIT full verify, and its
//      content-addressed name matches its actual bytes; an engine
//      shader source exists.
//   4. PROVE THE RUNTIME DEPLOYMENT IS USABLE (the launch contract,
//      headless): the manifest parses + validates, the deployed
//      catalog FINGERPRINT equals the runtime fold over the [ids]
//      rows (what GameProject.exe checks through its generated
//      project.h at boot), every texture/sound row serves its bytes
//      through its pak (the sound byte-compares against the checked-in
//      source), and the scene row resolves + LOADS through the public
//      clusterLoader seam (the GPU deserialize stays a dev-side
//      launch; every cooked project launch exercises it).
//
// What stays OUT (deliberate): compiling GameProject.exe — the samples
// tree is built by every top-level configure (ci.yml's build
// verification + the tests workflow both compile it), and launching a
// graphics host needs a GPU (see the five CI-excluded window tests +
// the dev-side golden runs, tools/golden_capture.py).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "DeployCheck.h"
#include "ProjectRuntime.h"
#include "ProjectScenes.h"

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/pak.h>
#include <Engine/Scene/scene.h>
#include <Engine/Vfs/vfs.h>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
int g_checks = 0;

const char* g_argv0 = "";

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

// Repo root: walk up from the test exe (it lives at the build root,
// <root>/out/build/<cfg>) until the sample marker is visible. Never
// CWD — ctest launches from anywhere.
fs::path FindRepoRoot() {
	fs::path dir = fs::path(g_argv0).parent_path();
	for (int i = 0; i < 10; ++i) {
		const fs::path probe = dir
			/ "samples/04_project_game/project/project.tourproj";
		std::error_code ec;
		if (fs::exists(probe, ec)) {
			return dir;
		}
		fs::path parent = dir.parent_path();
		if (parent == dir) {
			break;
		}
		dir = std::move(parent);
	}
	return {};
}

std::vector<uint8_t> ReadFileBytes(const fs::path& p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) {
		return {};
	}
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
		std::istreambuf_iterator<char>());
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// Run the CLI tool with stdout+stderr captured to `log`, WITHOUT the
// cmd.exe quoting minefield (std::system's leading-quote stripping
// mangles "C:\path with spaces\tool.exe" ... commands). Returns the
// process exit code (-1 on a launch failure, err filled).
int RunToolCapture(const fs::path& exe, const std::string& args,
	const fs::path& log, std::string& err) {
	const fs::path logAbs = fs::absolute(log);
	HANDLE out = CreateFileW(logAbs.wstring().c_str(), GENERIC_WRITE,
		0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (out == INVALID_HANDLE_VALUE) {
		err = "CreateFileW(log) failed";
		return -1;
	}
	SetHandleInformation(out, HANDLE_FLAG_INHERIT,
		HANDLE_FLAG_INHERIT);
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = out;
	si.hStdError = out;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION pi{};
	// lpCommandLine: quoted exe + args exactly as typed (no shell
	// reinterpretation — the whole reason this exists).
	std::wstring cmd = L"\"" + exe.wstring() + L"\" "
		+ std::wstring(args.begin(), args.end());
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
			0, nullptr, nullptr, &si, &pi)) {
		CloseHandle(out);
		err = "CreateProcessW failed";
		return -1;
	}
	CloseHandle(out); // the child holds its inherited copy
	WaitForSingleObject(pi.hProcess, 15 * 60 * 1000);
	DWORD code = static_cast<DWORD>(-1);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return int(code);
}
#else
#include <sys/wait.h>

// POSIX: /bin/sh handles the quoting correctly, so the plain system()
// capture form is fine (the command is fully quoted; output redirects
// to the log).
int RunToolCapture(const fs::path& exe, const std::string& args,
	const fs::path& log, std::string& err) {
	const std::string cmd = "\"" + exe.string() + "\" " + args
		+ " > \"" + fs::absolute(log).string() + "\" 2>&1";
	const int rc = std::system(cmd.c_str());
	if (rc == -1) {
		err = "system() failed";
		return -1;
	}
	return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
#endif

// The bucket order ComputeCatalogFingerprint's callers must emit in
// (the generated project.h's exact sequence): scenes, textures,
// meshes, sounds, shaders, gi — id-sorted within.
int KindBucket(const std::string& kind) {
	if (kind == "scene") return 0;
	if (kind == "texture") return 1;
	if (kind == "mesh") return 2;
	if (kind == "sound") return 3;
	if (kind == "shader") return 4;
	if (kind == "gi") return 5;
	return 6;
}

} // namespace

int main(int argc, char** argv) {
	if (argc > 0) {
		g_argv0 = argv[0];
	}

	// ---- 0) locate the checked-in sample + the CLI tool beside us ----
	const fs::path root = FindRepoRoot();
	Check(!root.empty(), "e2e.repo-root-found");
	const fs::path sample = root / "samples/04_project_game/project";
	Check(fs::exists(sample / "project.tourproj"), "e2e.sample-present");
#ifdef _WIN32
	const char* kProjectTool = "Project.exe";
#else
	const char* kProjectTool = "Project";
#endif
	const fs::path projectExe =
		fs::path(g_argv0).parent_path() / kProjectTool;
	Check(fs::exists(projectExe), "e2e.project-tool-built");
	if (root.empty() || !fs::exists(sample / "project.tourproj")
		|| !fs::exists(projectExe)) {
		std::cout << "checks=" << g_checks << " failures=" << g_failures
			<< "\n";
		return g_failures ? 1 : 0;
	}

	// ---- 1) the self-contained copy (the deployment subject) --------
	const fs::path tmp = fs::temp_directory_path() / "tourmaline_e2e";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	const fs::path proj = tmp / "project";
	fs::copy(sample, proj, fs::copy_options::recursive, ec);
	Check(!ec, "e2e.sample-copied");
	const fs::path publish = tmp / "publish";

	// ---- 2) the CONTRIBUTOR build: Project.exe build -----------------
	// (the real CLI + the same subprocess stage adapter a contributor's
	// machine runs — Cook/GiBaker/Pak spawn from the tool's own dir;
	// stdout lands in the log so a failure carries the tool's own
	// diagnostics into the ctest output.)
	const fs::path buildLog = tmp / "build.log";
	{
		const std::string args = "build --project \""
			+ proj.string() + "\" --recipe \""
			+ (proj / "game.tourrecipe").string()
			+ "\" --publish \"" + publish.string()
			+ "\" --manifest-name game";  // the tool appends .tapp
		std::string sysErr;
		const int rc = RunToolCapture(projectExe, args, buildLog,
			sysErr);
		Check(rc == 0, "e2e.build-exit-clean");
		if (rc != 0) {
			std::cout << "  build rc=" << rc;
			if (!sysErr.empty()) {
				std::cout << " (" << sysErr << ")";
			}
			std::cout << "\n";
		}
	}
	{
		// Echo the log tail regardless (the diagnostics ARE the value
		// when this leg fails).
		std::ifstream f(buildLog);
		std::string text((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());
		const size_t tail = text.size() > 2000 ? text.size() - 2000 : 0;
		std::cout << "[build.log tail]\n" << text.substr(tail)
			<< "[/build.log tail]\n";
	}

	// The published products: the .tapp manifest + the content-addressed
	// paks (<stem>-<fnv1a64>.pak — a digest in the name proves the
	// publish actually policed bytes, not just wrote files).
	std::vector<fs::path> paks;
	for (const auto& e : fs::directory_iterator(publish, ec)) {
		if (e.path().extension() == ".pak") {
			paks.push_back(e.path());
		}
	}
	std::sort(paks.begin(), paks.end());
	Check(paks.size() >= 2, "e2e.published-pak-count"); // game + shared
	int addressed = 0;
	for (const fs::path& p : paks) {
		const std::string stem = p.stem().string();
		// <stem>-<16 hex>: a dash + a 16-hex digest tail.
		const size_t dash = stem.rfind('-');
		if (dash != std::string::npos
			&& stem.size() - dash - 1 == 16) {
			bool hex = true;
			for (size_t i = dash + 1; i < stem.size(); ++i) {
				const char c = stem[i];
				if (!std::isxdigit(static_cast<unsigned char>(c))) {
					hex = false;
					break;
				}
			}
			if (hex) {
				++addressed;
			}
		}
	}
	Check(addressed == int(paks.size()) && !paks.empty(),
		"e2e.paks-content-addressed");
	const fs::path tapp = publish / "game.tapp";
	Check(fs::exists(tapp), "e2e.manifest-published");

	// ---- 3) the DEPLOYMENT VALIDATION GATE (DeployCheck, headless) --
	smvk::AppManifest manifest;
	std::string err;
	Check(smvk::LoadAppManifest(tapp.string(), manifest, err) && err.empty(),
		"e2e.manifest-parses");
	if (!err.empty()) {
		std::cout << "  manifest err: " << err << "\n";
	}
	Check(!manifest.paks.empty() && !manifest.entryPak.empty(),
		"e2e.manifest-has-entry");
	bool entryListed = false;
	for (const std::string& p : manifest.paks) {
		if (p == manifest.entryPak) {
			entryListed = true;
		}
	}
	Check(entryListed, "e2e.entry-pak-listed");
	int validated = 0;
	for (const std::string& pak : manifest.paks) {
		std::string vErr;
		if (app::ValidatePakFile((publish / pak).string(), vErr)) {
			++validated;
		}
		else {
			std::cout << "  pak " << pak << ": " << vErr << "\n";
		}
	}
	Check(!manifest.paks.empty()
		&& validated == int(manifest.paks.size()),
		"e2e.deploycheck-paks-valid");
	{
		std::string sErr;
		Check(app::ValidateEngineShaderSource(sErr) && sErr.empty(),
			"e2e.deploycheck-shader-source");
		if (!sErr.empty()) {
			std::cout << "  shader source err: " << sErr << "\n";
		}
	}

	// ---- 4) the RUNTIME half (the launch contract, GPU-less) ---------
	auto rtOpt = app::ProjectRuntime::Load(tapp.string(), err);
	Check(bool(rtOpt) && err.empty(), "e2e.runtime-loads-manifest");
	if (!rtOpt) {
		std::cout << "checks=" << g_checks << " failures=" << g_failures
			<< "\n";
		return 1;
	}
	app::ProjectRuntime& rt = *rtOpt;
	Check(!rt.Manifest().assets.empty(), "e2e.catalog-rows-present");

	// The compiled-catalog contract, reconstructed from the DEPLOYED
	// manifest (the row + fingerprint fold the generated project.h
	// emits; GameProject.exe attaches the same at boot). The deployed
	// fingerprint must equal the fold over the [ids] rows in the
	// generator's emission order.
	{
		std::vector<const smvk::AppManifestAsset*> rows;
		for (const auto& a : rt.Manifest().assets) {
			rows.push_back(&a);
		}
		std::stable_sort(rows.begin(), rows.end(),
			[](const smvk::AppManifestAsset* x,
				const smvk::AppManifestAsset* y) {
				const int bx = KindBucket(x->kind);
				const int by = KindBucket(y->kind);
				if (bx != by) {
					return bx < by;
				}
				return x->id < y->id;
			});
		std::vector<uint64_t> ids;
		std::vector<uint8_t> kinds;
		std::vector<uint32_t> pakIdx;
		std::vector<const char*> paths;
		std::vector<app::ProjectRuntime::CompiledAssetDesc> descs;
		for (const smvk::AppManifestAsset* row : rows) {
			uint8_t kindByte = 0;
			uint32_t idx = UINT32_MAX;
			for (size_t i = 0; i < rt.Manifest().paks.size(); ++i) {
				if (rt.Manifest().paks[i] == row->pak) {
					idx = uint32_t(i);
					break;
				}
			}
			if (!smvk::CatalogKindFromString(row->kind, kindByte)
				|| idx == UINT32_MAX) {
				Check(false, "e2e.row-identity-sound");
				continue;
			}
			ids.push_back(row->id);
			kinds.push_back(kindByte);
			pakIdx.push_back(idx);
			paths.push_back(row->path.c_str());
			descs.push_back({ row->id, kindByte, idx, row->path,
				row->name });
		}
		Check(descs.size() == rows.size(), "e2e.row-identity-sound");
		const uint64_t fold = smvk::ComputeCatalogFingerprint(
			ids.data(), kinds.data(), pakIdx.data(), paths.data(),
			ids.size());
		Check(fold != 0 && fold == rt.Manifest().catalogFingerprint,
			"e2e.catalog-fingerprint-matches-deployment");
		std::string aErr;
		Check(rt.AttachCompiledCatalog(descs.data(), descs.size(), fold,
				aErr)
			&& aErr.empty() && rt.CompiledCatalogAttached(),
			"e2e.compiled-catalog-attaches");
		if (!aErr.empty()) {
			std::cout << "  attach err: " << aErr << "\n";
		}
	}

	// Every VFS-servable row reads its bytes through its pak; the SOUND
	// row byte-compares against the checked-in source (raw .wav at the
	// VFS path — no container wrapping on that kind).
	{
		const std::vector<uint8_t> source =
			ReadFileBytes(proj / "content/audio/bells.wav");
		bool readAll = true, sawSound = false, sawTexture = false;
		bool soundMatches = false;
		for (const auto& row : rt.Manifest().assets) {
			if (row.kind != "texture" && row.kind != "sound"
				&& row.kind != "shader" && row.kind != "gi") {
				continue; // scenes/meshes load by identity, not ReadAsset
			}
			std::string rErr;
			auto bytes = rt.ReadAsset(row.id, rErr);
			if (!bytes || bytes->empty() || !rErr.empty()) {
				readAll = false;
				std::cout << "  read " << row.path << ": " << rErr
					<< "\n";
				continue;
			}
			if (row.kind == "sound") {
				sawSound = true;
				soundMatches = (*bytes == source);
			}
			if (row.kind == "texture") {
				sawTexture = true;
			}
		}
		Check(readAll, "e2e.assets-serve-through-paks");
		Check(sawTexture, "e2e.texture-row-served");
		Check(sawSound && soundMatches, "e2e.sound-bytes-match-source");
	}

	// The scene leg: the id->scene join over the DEPLOYED manifest, then
	// a headless LOAD through the public clusterLoader seam (the
	// GPU-less offline cluster the pak_host tests use; the real
	// deserialize runs on every cooked project launch).
	{
		smvk::OverlayVfs vfs;
		app::ProjectScenes scenes(rt, vfs);
		Check(scenes.Valid() && vfs.Mounts().size() >= paks.size(),
			"e2e.scenes-ctor-opens-paks");
		if (!scenes.Valid()) {
			std::cout << "  scenes err: " << scenes.OpenError() << "\n";
		}
		// The scene row (the recipe's entry: content/scenes/game.tour).
		uint64_t sceneId = 0;
		bool sawScene = false;
		for (const auto& row : rt.Manifest().assets) {
			if (row.kind == "scene") {
				sceneId = row.id;
				sawScene = true;
				break;
			}
		}
		Check(sawScene, "e2e.scene-row-present");
		std::string tErr;
		auto target = scenes.ResolveSceneTarget(sceneId, tErr);
		Check(bool(target) && tErr.empty() && !target->sceneName.empty(),
			"e2e.scene-target-recorded-name");
		if (target) {
			std::cout << "  scene " << target->pak << " / \""
				<< target->sceneName << "\" @ " << target->sceneIndex
				<< "\n";
		}
		// The offline loader seam: a hand-built cluster registered into
		// the world (the GPU deserialize is the dev-launch lane).
		scenes.clusterLoader =
			[](smvk::World& world, smvk::ResourceManager* ctx,
				const smvk::PakReader& reader, uint32_t sceneIndex,
				smvk::TextureStreamer* streamer,
				std::shared_ptr<const std::vector<uint8_t>> imagePin,
				const smvk::ExternalTextureResolver* externalTexture,
				std::string& loadErr) -> smvk::Cluster* {
			(void)ctx; (void)reader; (void)sceneIndex; (void)streamer;
			(void)imagePin; (void)externalTexture; (void)loadErr;
			auto c = std::make_unique<smvk::Cluster>(nullptr);
			const int node = addNode(c->scene, -1, 0);
			(void)node; // one root node: the fold witness
			smvk::Cluster* raw = c.get();
			world.RegisterCluster(std::move(c));
			return raw;
		};
		smvk::World world;
		std::string lErr;
		app::SceneHandle h = scenes.LoadSceneAdditive(world, nullptr,
			nullptr, sceneId, lErr);
		Check(bool(h) && lErr.empty(), "e2e.scene-loads");
		Check(scenes.IsLoaded(h) && scenes.FindScene(sceneId) == h
			&& scenes.SceneContentId(h) == sceneId,
			"e2e.scene-instance-tracked");
		std::string rErr;
		std::vector<uint64_t> released;
		Check(scenes.RemoveScene(h, world, rErr, &released) && rErr.empty(),
			"e2e.scene-removes");
		Check(!scenes.IsLoaded(h), "e2e.scene-handle-stale-after-remove");
	}

	fs::remove_all(tmp, ec);
	std::cout << "checks=" << g_checks << " failures=" << g_failures
		<< "\n";
	return g_failures ? 1 : 0;
}
