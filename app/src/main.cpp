// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// App host entry: CLI parse -> pak-viewer run. The pak loading (preload,
// world source, sponza check-scene extras, demo-content seeding) lives in
// this target — PakApp.cpp / PakWorldLoader.cpp / PakAppArgs.cpp /
// PakPreload.cpp / Host.h — NOT in the engine; the engine's WorldRenderer
// renders whatever world the host hands it.
#include "Host.h"

#include <filesystem>
#include <iostream>

#include <Engine/App/RunConfig.h>
#include <Engine/Utils/shader_hot_reload.h>
#include <Engine/Utils/shader_library.h>

int main(int argc, const char* argv[]) {
	// Central run config: parse ALL TOURMALINE_* once, log one line, warn
	// on unknown names (typos silently no-op'd before). Subsystems read
	// their slab from here; legacy getenv sites migrate incrementally.
	const engine::RunConfig runCfg = engine::RunConfig::Parse();
	runCfg.Log();
	engine::RunConfig::WarnUnknown();
	// The engine shader library mounts BEFORE any graphics
	// initialization: the exe-relative engine-shaders.pak (the
	// mandatory production source) + the dev content/shaders/
	// override. Every later shader read (renderer, render-graph,
	// debug views) serves through it.
	smvk::shaders::Init();
	// TOURMALINE_SHADER_RELOAD=1 (dev): arm the watched recompile
	// service — engine/shaders/ edits recompile in-process and land
	// in the dev override tree; the renderer's frame tick consumes
	// the change log. Never on by default (production never pays).
	if (std::getenv("TOURMALINE_SHADER_RELOAD")) {
		namespace fs = std::filesystem;
		// (the dev-tree source: the standard repo layout —
		// out/build/<cfg>/ -> <repo>/engine/shaders; an explicit
		// override for unusual trees)
		fs::path srcDir;
		if (const char* src =
				std::getenv("TOURMALINE_SHADER_RELOAD_SRC"))
			srcDir = fs::path(src);
		else
			srcDir = (fs::current_path() / ".." / ".." / ".."
				/ "engine" / "shaders").lexically_normal();
		const std::string outDir = (fs::current_path() / "content"
			/ "shaders").lexically_normal().generic_string();
		std::string rErr;
		if (!smvk::shader_reload::Start(
				srcDir.lexically_normal().generic_string(), outDir,
				rErr))
			std::cerr << "[shader-reload] not armed: " << rErr
				<< "\n";
	}
	// The CLI parse fills the dedicated options object ONLY — the
	// host-supplied state below is assembled after it and can never be
	// reset or erased by the parser.
	app::PakAppConfig cfg;
	cfg.run = runCfg;
	int exitCode = 0;
	if (!app::ParsePakAppArgs(argc, argv, cfg.cli, exitCode))
		return exitCode;
	return app::RunPakApp(cfg);
}
