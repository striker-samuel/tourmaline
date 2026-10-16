#pragma once
// Pak-viewer host: the APP's own host layer. Takes a cooked pak (+ optional
// scene index), loads it into a World and runs it through the engine's
// WorldRenderer (Engine/Renderer/WorldRenderer.h). The engine knows nothing
// about paks-as-host-content — it renders whatever world the host hands it
// through SetWorldSource; pak preload/parse, the sponza check-scene extras
// and the demo-content seeding all live here.
//
// Usage:
//   App [--pak <path.pak>] [--scene-index N] [--no-vfs]
//   [--minimal] [--filter-only] [--with <csv>] [--without <csv>]
//   [--bench <frames>] [--bench_move <m/s>] [--present <fifo|mailbox|immediate>]
//   [--disable_control] [--no-dlss] [--use_optick] [--use_optick_gpu]

#include <string>

#include <Engine/App/RunConfig.h>

namespace smvk {
	class ResourceManager;
	struct World;
}

namespace app {

// Host configuration (one field per CLI flag; ParsePakAppArgs fills it).
struct PakAppConfig {
	std::string pakPath = "content/paks/sponza.pak";
	int sceneIndex = 0;
	bool minimal = false;
	bool filterOnly = false;
	std::string withCsv;
	std::string withoutCsv;
	int benchFrames = 0;
	float benchMoveSpeed = 0.0f;
	std::string presentMode; // empty = default; fifo|mailbox|immediate
	bool disableControl = false;
	bool useOptick = false;
	bool useOptickGpu = false;
	bool noDlss = false;
	// --no-vfs: mute the loose VFS discovery mounts (project root +
	// content/) so the pak backend is the ONLY scene-content source.
	// Proves the pak is self-contained: anything the pak lacks falls
	// back loudly (procedural cubemaps, missing GI) instead of being
	// silently covered by loose sidecars. Engine resources (shaders,
	// fonts, LUTs) load exe-relative, never through the VFS.
	bool noVfs = false;
	// Scene id (GI sidecar stem + check-scene gating). Empty = pak stem.
	std::string sceneId;
	// Central run config (parsed once in main; Log/WarnUnknown there).
	// Carried by value so the renderer consumes the same snapshot.
	engine::RunConfig run;
};

// Parse argv into cfg. Returns true on success; on `--help`/unknown args
// (or the removed `--gltf`) prints to stdout/stderr and returns false with
// exitCode set (EXIT_SUCCESS for --help, EXIT_FAILURE otherwise).
bool ParsePakAppArgs(int argc, const char* argv[], PakAppConfig& cfg,
	int& exitCode);

// One-call run: InitMemory, background pak preload overlapped with renderer
// init, world-source install, renderer wiring (passes/bench/DLSS/control,
// demo-content opt-in), Run(), ShutdownMemory. Returns the process exit
// code.
int RunPakApp(PakAppConfig cfg);

} // namespace app
