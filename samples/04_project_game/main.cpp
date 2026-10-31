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

#include "project.h"

#include <Engine/App/RunConfig.h>

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

	// The catalog-driven launch banner (every id the header carries).
	std::printf("[GameProject] content-as-code: project 0x%llX '%s', %zu"
		" catalog asset(s)\n",
		(unsigned long long)project::ProjectId,
		std::string(project::Name).c_str(),
		project::Catalog.size());
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
	return app::RunPakApp(cfg);
}
