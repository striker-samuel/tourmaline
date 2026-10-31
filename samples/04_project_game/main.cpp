// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The CONTENT-AS-CODE sample: a standalone project exe. The generated
// project.h (the content ids from the project's asset database —
// samples/04_project_game/project) is a pure constexpr catalog: this
// code programs against stable ids, never paths, never scene indices.
// The GameProject_paks target cooks the paks + the appmanifest next to
// the exe on demand (cmake --build --target GameProject_paks); the
// apphost resolves them exe-relative at run time.
//
//   cmake --build out/build/x64-Release --target GameProject       (exe + project.h)
//   cmake --build out/build/x64-Release --target GameProject_paks  (content)
//   out/build/x64-Release/GameProject.exe                          (runs the entry scene)

#include "Host.h"
#include "ProjectRuntime.h"
#include "AudioRuntime.h"

#include "project.h"

#include <Engine/App/RunConfig.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h> // GetModuleFileNameA (the exe-relative manifest)
#else
#include <unistd.h> // readlink (/proc/self/exe)
#endif

int main(int argc, const char* argv[]) {
	// COMPILE-TIME content proofs: the catalog is real C++ — the entry
	// scene exists, the pak table is populated, the ids are constexpr.
	static_assert(project::Contains(project::EntryScene),
		"the catalog must carry the entry scene");
	static_assert(!project::Paks.empty(),
		"the catalog must carry at least one pak");
	static_assert(project::Contains(project::textures::stone),
		"the catalog must carry the shared texture");

	// The catalog-driven launch banner (every id the header carries).
	std::printf("[GameProject] content-as-code: project 0x%llX '%s', %zu"
		" catalog asset(s)\n",
		(unsigned long long)project::ProjectId,
		std::string(project::Name).c_str(),
		project::Assets.size());
	for (const auto& pak : project::Paks)
		std::printf("[GameProject] pak '%s' -> %s\n",
			std::string(pak.name).c_str(),
			std::string(pak.file).c_str());
	std::printf("[GameProject] entry scene id 0x%llx (identity launch,"
		" never an index)\n",
		(unsigned long long)project::EntryScene);

	// Central run config (the TOURMALINE_* pins) + the host assembly:
	// same host as the plain viewer, but the ENTRY POINT comes from the
	// PROJECT (the appmanifest beside the exe) unless the caller names
	// one explicitly (--pak/--project still work for debugging).
	const engine::RunConfig runCfg = engine::RunConfig::Parse();
	runCfg.Log();
	app::PakAppConfig cfg;
	cfg.run = runCfg;

	std::vector<const char*> args(argv, argv + argc);
	std::string manifestPath;
	bool hasEntry = false;
	for (int i = 1; i < argc; ++i)
		if (std::strncmp(argv[i], "--pak", 5) == 0
				|| std::strncmp(argv[i], "--project", 9) == 0)
			hasEntry = true;
	if (!hasEntry) {
		// The packaged launch: the appmanifest next to this exe (the
		// GameProject_paks target publishes it here; its paks resolve
		// manifest-relative so the exe+content pair moves as one).
		namespace fs = std::filesystem;
		char exePath[1024] = {};
#ifdef _WIN32
		GetModuleFileNameA(nullptr, exePath, DWORD(sizeof(exePath)));
#else
		const ssize_t n = readlink("/proc/self/exe", exePath,
			sizeof(exePath) - 1);
		if (n > 0)
			exePath[n] = '\0';
#endif
		manifestPath = (fs::path(exePath).parent_path() / "game.tapp")
			.lexically_normal()
			.generic_string();
		args.push_back("--project");
		args.push_back(manifestPath.c_str());
	}
	int exitCode = 0;
	if (!app::ParsePakAppArgs(int(args.size()), args.data(), cfg.cli,
			exitCode))
		return exitCode;

	// ---- THE RUNTIME FAÇADE: stable ids resolve to real bytes -------
	// The compiled header's project::textures::stone (a constexpr id
	// from the project's asset db) resolves through the appmanifest's
	// [ids] table to its pak + path, and the bytes come back through
	// the pak backend — game code never types a path or an index.
	// (Cross-pak by construction: the texture lives in shared.pak while
	// the entry scene rides game.pak.)
	if (!cfg.cli.project.empty()) {
		std::string rtErr;
		if (auto runtime = app::ProjectRuntime::Load(cfg.cli.project,
				rtErr)) {
			// ---- THE COMPILED-CATALOG CONTRACT (the flattened
			// project.h): the game proves ITSELF against the deployed
			// content up front — the header's flat Assets table (the
			// compile-time POD records) + its CatalogFingerprint
			// validate against the .tapp's [ids] rows + `catalog` line.
			// After this, the game KNOWS the ids it was compiled with
			// are the ids the content ships — no runtime discovery, no
			// .tapp row ever reaches gameplay code.
			static_assert(sizeof(project::AssetDesc)
					== sizeof(app::ProjectRuntime::CompiledAssetDesc)
				&& offsetof(project::AssetDesc, id)
					== offsetof(app::ProjectRuntime::CompiledAssetDesc,
						id)
				&& offsetof(project::AssetDesc, pakIndex)
					== offsetof(app::ProjectRuntime::CompiledAssetDesc,
						pakIndex)
				&& offsetof(project::AssetDesc, path)
					== offsetof(app::ProjectRuntime::CompiledAssetDesc,
						path),
				"the generated AssetDesc and the runtime's mirror must"
				" stay layout-identical (member order + types)");
			if (runtime->AttachCompiledCatalog(
					reinterpret_cast<
						const app::ProjectRuntime::CompiledAssetDesc*>(
						project::Assets.data()),
					project::Assets.size(),
					project::CatalogFingerprint, rtErr))
				std::printf("[GameProject] compiled catalog attached +"
					" validated against the deployment manifest"
					" (fingerprint 0x%llx, %zu asset(s))\n",
					(unsigned long long)project::CatalogFingerprint,
					project::Assets.size());
			else
				std::printf("[GameProject] compiled catalog REJECTED:"
					" %s\n", rtErr.c_str());
			// The flat table is a first-class compile-time surface:
			// kind + pakIndex typed queries, no strings.
			if (const project::AssetDesc* tex =
					project::Find(project::textures::stone))
				std::printf("[GameProject] flat table: stone -> kind %u"
					" pak[%u] '%.*s'\n",
					unsigned(tex->kind), tex->pakIndex,
					int(tex->path.size()), tex->path.data());
			if (const smvk::AppManifestAsset* row =
					runtime->Asset(project::textures::stone)) {
				std::printf("[GameProject] id 0x%llx -> %s '%s'\n",
					(unsigned long long)project::textures::stone,
					row->pak.c_str(), row->path.c_str());
				if (auto bytes = runtime->ReadAsset(
						project::textures::stone, rtErr))
					std::printf("[GameProject] read %zu bytes of"
						" ktx2 through %s\n", bytes->size(),
						row->pak.c_str());
				else
					std::printf("[GameProject] texture read failed:"
						" %s\n", rtErr.c_str());
			}
			else {
				std::printf("[GameProject] the id table does not carry"
					" the texture (rebuild GameProject_paks)\n");
			}
		}
	}
	// ---- THE AUDIO LAYER: the scene-aware runtime over the mixer ----
	// The AudioRuntime element stacks over the renderer (the host
	// extension hook): it owns the clip cache (sound ids decoded
	// through the project's pak), the emitter states (the authored
	// audio-source nodes — this scene's BellTower, with its playOnLoad
	// + loop ring on load), and the spatialization (attenuation + pan
	// against the active camera, per frame). Gameplay code drives it
	// through the imperative surface: PrefetchSound /
	// PlaySound(world, node, sound) / StopSound / SetListener.
	if (!cfg.cli.project.empty()) {
		std::string audioTapp = cfg.cli.project;
		cfg.extraElement =
			[audioTapp](smvk::App& host,
				engine::WorldRenderer& renderer,
				std::shared_ptr<smvk::World*> worldHandle)
			-> std::shared_ptr<engine::IAppElement> {
			return app::MakeAudioRuntime(host, audioTapp,
				std::move(worldHandle), &renderer);
		};
	}
	return app::RunPakApp(cfg);
}
