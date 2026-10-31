// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Pak-viewer host: the APP's own host layer. Takes a cooked pak (+ optional
// scene index), loads it into a World and runs it through the engine's
// WorldRenderer (Engine/Renderer/WorldRenderer.h). The engine knows nothing
// about paks-as-host-content — it renders whatever world the host hands it
// through SetWorldSource; pak preload/parse, the sponza check-scene extras
// and the demo-content seeding all live here.
//
// Layer split (the PakApp.cpp responsibilities, extracted):
//   - ViewerOptions (below): the PURE CLI surface — every --flag the
//     viewer parses. The parser fills EXACTLY this object; host-supplied
//     config (RunConfig pins, the extension hooks) lives on PakAppConfig
//     and is never reset or erased by a parse.
//   - PakWorldLoader (PakWorldLoader.h): pak preload -> a loaded World
//     (scene discovery, cluster load, anim clips, static flags, the
//     scene-prep diagnostics).
//   - RunPakApp (PakApp.cpp): the viewer HOST — renderer wiring, pass
//     config, bench/DLSS/control, lifecycle, exit codes.
//
// Usage:
//   App [--pak <path.pak>] [--scene-index N] [--no-vfs]
//   App --project <path.tapp> [--no-vfs]
//   [--minimal] [--filter-only] [--with <csv>] [--without <csv>]
//   [--bench <frames>] [--bench_move <m/s>] [--present <fifo|mailbox|immediate>]
//   [--disable_control] [--no-dlss] [--capture] [--use_optick] [--use_optick_gpu]

#include <functional>
#include <memory>
#include <string>

#include <Engine/App/RunConfig.h>

namespace tour { struct World; class App; }
namespace smvk {
	class ResourceManager;
	using ::tour::World;
	using ::tour::App;
}

namespace engine {
	class IAppElement;
	class WorldRenderer;
}

namespace app {

// The viewer's CLI options (one field per CLI flag; ParsePakAppArgs fills
// this object and nothing else — see the layer note above).
struct ViewerOptions {
	// The entry-point pak — REQUIRED, no default: a bare App.exe must
	// refuse instead of silently running whatever pak happens to sit at
	// some hard-coded path (the old default content/paks/sponza.pak
	// launched stale builds' content by accident). Empty after the
	// parse = the parse gate rejects (usage + EXIT_FAILURE).
	std::string pakPath;
	int sceneIndex = 0;
	bool minimal = false;
	bool filterOnly = false;
	std::string withCsv;
	std::string withoutCsv;
	int benchFrames = 0;
	float benchMoveSpeed = 0.0f;
	std::string presentMode; // empty = default; fifo|mailbox|immediate
	bool presentSet = false; // --present given explicitly (wins over the --bench vsync-off force)
	bool disableControl = false;
	bool useOptick = false;
	bool useOptickGpu = false;
	bool noDlss = false;
	// --capture: the one-flag RenderDoc launch configuration. Implies
	// --no-dlss (Streamline out of the Vulkan call chain — low latency /
	// Reflex / PCL ride that gate: VK_NV_low_latency2 is enabled only
	// while Streamline is loaded, and the PCL markers are no-ops without
	// the interposer) and force-disables Optick (CPU + GPU — the GPU
	// profiler's fence-based flip interleaves its own command-buffer
	// submits with the frame, which breaks captures). Wins over
	// --use_optick[_gpu] if both are given. Unrelated to the
	// TOURMALINE_CAPTURE env pin (PNG screenshot export at a frame).
	bool capture = false;
	// --no-vfs: mute the loose VFS discovery mounts (project root +
	// content/) so the pak backend is the ONLY scene-content source.
	// Proves the pak is self-contained: anything the pak lacks falls
	// back loudly (procedural cubemaps, missing GI) instead of being
	// silently covered by loose bake files. Engine resources (shaders,
	// fonts, LUTs) load exe-relative, never through the VFS.
	bool noVfs = false;
	// --project <path.tapp>: the GENERATED appmanifest launch — parse
	// + validate the manifest BEFORE Vulkan startup, mount every listed
	// pak (manifest-relative paths, listed order), and load the entry
	// scene BY IDENTITY (the manifest's entry pak + scene name — never
	// a scene index, never the source .tour). Mutually exclusive with
	// --pak and --scene-index (the legacy single-pak surface stands
	// unchanged beside it).
	std::string project;
	// Scene id (GI bake stem + check-scene gating). Empty = pak stem.
	std::string sceneId;
};

// The full host run configuration: the parsed CLI options + the
// host-supplied state the parser must NEVER touch. RunPakApp consumes
// this; main() assembles it (CLI parse -> ViewerOptions, then installs
// the RunConfig snapshot and any host extension hooks).
struct PakAppConfig {
	ViewerOptions cli;
	// Central run config (parsed once in main; Log/WarnUnknown there).
	// Carried by value so the renderer consumes the same snapshot.
	engine::RunConfig run;
	// ---- Host extension element (the game-host hook) ----
	// A host (Asteroids.exe — asteroids/src/main.cpp) installs a factory
	// that RunPakApp stacks OVER the renderer element (after its onAttach
	// loaded the world): the game layer owns whatever it wants (the
	// sponza invaders element drives the camera + ball entities through
	// the merged scene). Returning null from the factory skips stacking
	// (the env kill switch lives there). Bench runs never stack it
	// (capture determinism — RunPakApp gates). The plain App host leaves
	// it unset: the clean viewer.
	std::function<std::shared_ptr<engine::IAppElement>(
		smvk::App& host,
		engine::WorldRenderer& renderer,
		std::shared_ptr<smvk::World*> worldHandle)> extraElement;
	// True when the extra element owns the camera (RunPakApp then parks
	// the renderer element's FPS camera drive for the whole run).
	bool extraOwnsCamera = false;
};

// Parse argv into `opts` (the dedicated CLI options object — nothing else
// is read or reset). Returns true on success; on `--help`/unknown args/bad
// values (or the removed `--gltf`) prints to stdout/stderr and returns
// false with exitCode set (EXIT_SUCCESS for --help, EXIT_FAILURE
// otherwise).
bool ParsePakAppArgs(int argc, const char* argv[], ViewerOptions& opts,
	int& exitCode);

// One-call run: InitMemory, background pak preload overlapped with renderer
// init, world-source install, renderer wiring (passes/bench/DLSS/control,
// demo-content opt-in), Run(), ShutdownMemory. Returns the process exit
// code.
int RunPakApp(PakAppConfig cfg);

} // namespace app
