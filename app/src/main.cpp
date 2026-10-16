// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// App host entry: CLI parse -> pak-viewer run. The pak loading (preload,
// world source, sponza check-scene extras, demo-content seeding) lives in
// this target — PakApp.cpp/PakPreload.cpp/Host.h — NOT in the engine; the
// engine's WorldRenderer renders whatever world the host hands it.
#include "Host.h"

#include <Engine/App/RunConfig.h>

int main(int argc, const char* argv[]) {
	// Central run config: parse ALL TOURMALINE_* once, log one line, warn
	// on unknown names (typos silently no-op'd before). Subsystems read
	// their slab from here; legacy getenv sites migrate incrementally.
	const engine::RunConfig runCfg = engine::RunConfig::Parse();
	runCfg.Log();
	engine::RunConfig::WarnUnknown();
	app::PakAppConfig cfg;
	cfg.run = runCfg;
	int exitCode = 0;
	if (!app::ParsePakAppArgs(argc, argv, cfg, exitCode))
		return exitCode;
	return app::RunPakApp(cfg);
}
