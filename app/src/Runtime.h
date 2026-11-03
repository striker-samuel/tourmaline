// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: LicenseRef-Tourmaline-Community-1.0

#pragma once
// THE APPLICATION-FACING RUNTIME (the consolidated content
// architecture's public boundary — the proposal's "run<Game>"
// convenience path, in the repo's app:: namespace per the
// app-host/ownership split): ONE shared content session, the game's
// lifecycle hooks over the engine's element stack, and the FATAL
// compiled-catalog gate. Games compose THIS — never the pak viewer's
// config, never a manifest row, never a pak filename.
//
//   struct Game : app::GameBehaviour {      // the application
//       void onStart(app::Context& ctx) override {
//           ctx.scenes().LoadSceneAdditive(world, nullptr, nullptr,
//               project::EntryScene, err);
//       }
//       void onUpdate(app::Context& ctx, float dt) override {
//           app::PlaySound(*ctx.world(), node, project::sounds::bell);
//       }
//   };
//   int main(int argc, const char* argv[]) {
//       Game game;
//       return app::RunGame(&game, argc, argv,
//           project::Assets.data(), project::Assets.size(),
//           project::CatalogFingerprint);
//   }
//
// THE BRING-YOUR-OWN-LOOP PATH stays open (the proposal's contract):
// RunGame is a thin adapter over RunPakApp (the viewer host's
// lifecycle + window + bench/capture/control knobs) — a game needing
// its OWN loop drives RunPakApp (or lower) directly with the same
// session + element composition; the convenience runner must never
// become mandatory.
//
// ELEMENT COMPOSITION: the game element rides the ENGINE's element
// stack (AddElement's attach order + reverse detach — the lifecycle
// coordinator); a game's SERVICE elements (the scene-aware audio...)
// stack UNDER the game element through GameLaunchOptions::services
// (listed order), each receiving the shared session + the world
// handle. No nested lifecycle-forwarding wrappers — every element
// owns only its own concerns.

#include "Host.h"
#include "ProjectRuntime.h"
#include "ProjectScenes.h"

#include "AudioGraphs.h"

#include <Engine/App/Element.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace app {

// ---- THE APPLICATION CONTEXT ---------------------------------------------
// The borrowed view game code receives through the hooks: the launch's
// shared content session + the services over it. The game never
// reaches past this into viewer-config concerns.
class Context {
public:
	Context(app::ProjectRuntime& session, tour::App& host,
		engine::WorldRenderer& renderer,
		std::shared_ptr<tour::World*> worldHandle);

	// The launch's SHARED content session (the manifest or discovery
	// lane built it before any graphics work; the world load's
	// providers are its providers).
	[[nodiscard]] app::ProjectRuntime& session() {
		return m_session;
	}
	// The world the renderer loaded (null until the renderer attached
	// — the hooks run after, so it is live by then).
	[[nodiscard]] tour::World* world() const {
		return m_worldHandle ? *m_worldHandle : nullptr;
	}
	// The element stack's host + the renderer (a game stacking its
	// OWN elements during onStart — safe: the attach loop is past
	// the renderer, the additions append to the stack's tail).
	[[nodiscard]] tour::App& host() { return m_host; }
	[[nodiscard]] engine::WorldRenderer& renderer() {
		return m_renderer;
	}

	// The SCENE SERVICE (ProjectScenes over the session — scene
	// instances by id, additive loads, the ownership-aware
	// prefetch). Lazily built at first use (one per context).
	[[nodiscard]] app::ProjectScenes& scenes();

	// The AUDIO-GRAPH SERVICE (AudioGraphs over the session + the
	// engine's AudioEngine — docs/audio_graph.md): the typed-key
	// create() gameplay drives authored graph instances through.
	// Lazily built at first use (one per context).
	[[nodiscard]] app::AudioGraphs& audio();

	// Engine-owned, per-run gameplay random streams (seeded before onStart).
	[[nodiscard]] tour::RandomService& random() { return m_host.Random(); }

private:
	app::ProjectRuntime& m_session;
	tour::App& m_host;
	engine::WorldRenderer& m_renderer;
	std::shared_ptr<tour::World*> m_worldHandle;
	std::unique_ptr<app::ProjectScenes> m_scenes;
	std::unique_ptr<app::AudioGraphs> m_audio;
	smvk::OverlayVfs m_vfs; // protocol surface only (see content(8))
};

// ---- THE GAME BEHAVIOUR ----------------------------------------------------
// The application's lifecycle hooks. Games implement ANY subset —
// every default is a no-op (the runner composes whatever exists).
class GameBehaviour {
public:
	virtual ~GameBehaviour() = default;
	// After the renderer attached (the world + the session are live)
	// and after the service elements stacked (their state is ready).
	virtual void onStart(Context& ctx) { (void)ctx; }
	// Every frame after the start (the engine's pump drives it).
	virtual void onUpdate(Context& ctx, float dt) {
		(void)ctx;
		(void)dt;
	}
};

// ---- THE GAME ELEMENT -----------------------------------------------------
// The GameBehaviour's adapter onto the engine's element stack: the
// hooks run at the element's lifecycle points; everything else rides
// as a no-op. The behaviour is CALLER-OWNED (RunGame is synchronous —
// the game can live on the caller's stack for the whole run).
class GameElement final : public engine::IAppElement {
public:
	GameElement(GameBehaviour* game,
		std::shared_ptr<tour::World*> worldHandle,
		std::shared_ptr<app::ProjectRuntime> session,
		tour::App& host, engine::WorldRenderer& renderer);

	const char* ElementName() const override { return "Game"; }

	bool onAttach() override;
	void onDetach() override {}
	void onResize(smvk::Extent2D) override {}
	void onUpdate(float dt) override;
	void onUIMenu() override {}
	void onUIRender() override {}
	void onPreRender(engine::ElementFrame&) override {}
	void onRender(engine::ElementFrame&) override {}

private:
	GameBehaviour* m_game;
	std::shared_ptr<tour::World*> m_worldHandle;
	std::shared_ptr<app::ProjectRuntime> m_session;
	tour::App& m_host;
	engine::WorldRenderer& m_renderer;
	std::unique_ptr<Context> m_context;
};

// ---- THE LAUNCH OPTIONS ----------------------------------------------------
struct GameLaunchOptions {
	// SERVICE element factories stacked UNDER the game element
	// (listed order; each receives the shared session + the world
	// handle + the renderer). The scene-aware audio installs here.
	using ServiceFactory = std::function<
		std::shared_ptr<engine::IAppElement>(
			tour::App& host,
			engine::WorldRenderer& renderer,
			std::shared_ptr<tour::World*> worldHandle,
			const std::shared_ptr<app::ProjectRuntime>& session)>;
	std::vector<ServiceFactory> services;
	// The packaged appmanifest beside the exe (the default entry when
	// the caller names no --pak/--project). Empty disables the default
	// (the caller MUST then name an entry).
	std::string defaultManifest = "game.tapp";
	// True when a service element owns the camera (the renderer's FPS
	// drive parks for the whole run).
	bool extraOwnsCamera = false;
};

// ---- THE PACKAGED DEFAULT ENTRY --------------------------------------------
// The appmanifest beside the exe unless the caller named an entry
// (--pak/--project still work for debugging). Implementation in
// Runtime.cpp (the exe-relative resolution); an empty defaultManifest
// disables the default. Public for tests (the default-entry pin).
[[nodiscard]] std::vector<const char*> ArgsWithDefaultEntry(
	int argc, const char* argv[],
	const std::string& defaultManifest);

// ---- THE CONVENIENCE RUNNER -----------------------------------------------
// One call: parse the launch (the packaged manifest beside the exe
// unless the caller names an entry), build the session, run the
// FATAL compiled-catalog gate, stack the services + the game
// element, run the host. Returns the process exit code.
//
// `catalogAssets` is the generated project.h's flat Assets table
// (its OWN Desc type — the layout proof static-asserts at every
// instantiation, the same discipline as AttachCompiledCatalog;
// nullptr + count 0 = no compiled catalog: a game without
// project.h — the session still builds + resolves, the gate just
// does not run).
template <typename Desc>
int RunGame(GameBehaviour* game, int argc, const char* argv[],
	const Desc* catalogAssets, size_t catalogCount,
	uint64_t catalogFingerprint, GameLaunchOptions options = {}) {
	app::PakAppConfig cfg;
	cfg.run = engine::RunConfig::Parse();
	cfg.run.Log(); // the TOURMALINE_* pins (the sample's old block)
	std::vector<const char*> args = ArgsWithDefaultEntry(argc, argv,
		options.defaultManifest);
	int exitCode = 0;
	if (!app::ParsePakAppArgs(int(args.size()), args.data(), cfg.cli,
			exitCode))
		return exitCode;
	// THE FATAL COMPILED-CATALOG GATE: the game proves itself
	// against the deployed content before any graphics work (a
	// mismatch refuses the launch — the version-skew contract).
	if (catalogAssets || catalogCount != 0) {
		cfg.compiledCatalogGate =
			[catalogAssets, catalogCount, catalogFingerprint](
				app::ProjectRuntime& session, std::string& err) {
			return app::AttachCompiledCatalog(session, catalogAssets,
				catalogCount, catalogFingerprint, err);
		};
	}
	// The services under the game element (listed order), then the
	// game element itself last (its onStart sees the services'
	// stacked state).
	cfg.extraElements.assign(options.services.begin(),
		options.services.end());
	cfg.extraOwnsCamera = options.extraOwnsCamera;
	cfg.extraElements.push_back(
		[game](tour::App& host, engine::WorldRenderer& renderer,
			std::shared_ptr<tour::World*> worldHandle,
			const std::shared_ptr<app::ProjectRuntime>& session)
		-> std::shared_ptr<engine::IAppElement> {
		return std::make_shared<app::GameElement>(game,
			std::move(worldHandle), session, host, renderer);
	});
	return app::RunPakApp(std::move(cfg));
}

} // namespace app
