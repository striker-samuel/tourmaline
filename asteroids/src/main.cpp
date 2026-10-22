// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Asteroids.exe: the sponza invaders GAME host. The same pak-viewer
// pipeline as App (full WorldRenderer pass set — shadows, GI, SSR,
// volumetrics, particles, the works; see app/src/PakApp.cpp) plus the
// invaders game element stacked over the renderer through RunPakApp's
// extra-element hook (app/src/Host.h): fixed top-down camera over the
// atrium, green player ball vs the red invader grid, GPU particle
// bursts on every death/hit/fire (src/InvadersGame.cpp). The CLI
// surface is identical to App (same ParsePakAppArgs); the default pak
// is asteroids.pak — the arena assembled from the cooked sponza
// artifacts (see below).
#include "Host.h"
#include "InvadersGame.h"

#include <Engine/App/RunConfig.h>

#include <cstdlib>
#include <memory>

int main(int argc, const char* argv[]) {
	// Central run config: parse ALL TOURMALINE_* once, log one line,
	// warn on unknown names (same contract as App/main.cpp).
	const engine::RunConfig runCfg = engine::RunConfig::Parse();
	runCfg.Log();
	engine::RunConfig::WarnUnknown();
	app::PakAppConfig cfg;
	cfg.run = runCfg;
	int exitCode = 0;
	if (!app::ParsePakAppArgs(argc, argv, cfg, exitCode))
		return exitCode;
	// AFTER the parse: ParsePakAppArgs resets the config to defaults
	// (preserving only `run`), so host extension fields are set here.
	// The game pak: the arena assembled from the cooked sponza artifacts
	// (tools/cook + Pak.exe --out-pak asteroids.pak; the GI stack and
	// cubemaps are embedded, the field is laid out for its atrium). An
	// explicit --pak still wins — only the untouched struct default is
	// swapped.
	if (cfg.pakPath == "content/paks/sponza.pak")
		cfg.pakPath = "content/paks/asteroids.pak";
	// The game element factory: RunPakApp stacks whatever it returns
	// over the renderer (world already loaded). TOURMALINE_INVADERS=0
	// mutes the game (the plain viewer frame).
	cfg.extraElement =
		[](smvk::App& host, engine::WorldRenderer& renderer,
			std::shared_ptr<smvk::World*> worldHandle)
		-> std::shared_ptr<engine::IAppElement> {
		if (const char* e = std::getenv("TOURMALINE_INVADERS");
			e && *e == '0')
			return nullptr;
		return std::make_shared<app::InvadersElement>(
			host, renderer, std::move(worldHandle));
	};
	cfg.extraOwnsCamera = true; // the fixed top-down game camera
	return app::RunPakApp(cfg);
}
