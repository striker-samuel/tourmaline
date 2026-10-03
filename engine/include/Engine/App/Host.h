#pragma once
// Pak-viewer host: the reusable half of the old App/main.cpp.
// Takes a cooked pak (+ optional scene index), loads it into a World and
// runs it through the engine's WorldRenderer (Engine/Renderer/WorldRenderer.h).
//
// Usage (unchanged from the old App entry):
//   App [--pak <path.pak>] [--scene-index N]
//   [--minimal] [--filter-only] [--with <csv>] [--without <csv>]
//   [--bench <frames>] [--bench_move <m/s>] [--present <fifo|mailbox|immediate>]
//   [--disable_control] [--no-dlss] [--use_optick] [--use_optick_gpu]

#include <Engine/Renderer/WorldRenderer.h>

#include <string>

namespace engine {

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
	// Scene id (GI sidecar stem + check-scene gating). Empty = pak stem.
	std::string sceneId;
};

// Parse argv into cfg. Returns true on success; on `--help`/unknown args
// (or the removed `--gltf`) prints to stdout/stderr and returns false with
// exitCode set (EXIT_SUCCESS for --help, EXIT_FAILURE otherwise).
bool ParsePakAppArgs(int argc, const char* argv[], PakAppConfig& cfg,
	int& exitCode);

// One-call run: InitMemory, background pak preload overlapped with renderer
// init, world-source install, renderer wiring (passes/bench/DLSS/control),
// Run(), ShutdownMemory. Returns the process exit code.
int RunPakApp(PakAppConfig cfg);

// Parse + run (the thin-main entry point).
int RunPakApp(int argc, const char* argv[]);

} // namespace engine
