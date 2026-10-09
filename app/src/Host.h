#pragma once
// Pak-viewer host: the APP's own host layer. Takes a cooked pak (+ optional
// scene index), loads it into a World and runs it through the engine's
// WorldRenderer (Engine/Renderer/WorldRenderer.h). The engine knows nothing
// about paks-as-host-content — it renders whatever world the host hands it
// through SetWorldSource; pak preload/parse, the sponza check-scene extras
// and the demo-content seeding all live here.
//
// Usage:
//   App [--pak <path.pak>] [--scene-index N]
//   [--minimal] [--filter-only] [--with <csv>] [--without <csv>]
//   [--bench <frames>] [--bench_move <m/s>] [--present <fifo|mailbox|immediate>]
//   [--disable_control] [--no-dlss] [--use_optick] [--use_optick_gpu]

#include <string>

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
	// Scene id (GI sidecar stem + check-scene gating). Empty = pak stem.
	std::string sceneId;
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

// ---- Demo decal assets (DecalAssets.cpp) ------------------------------
// Procedural blood-splat textures (deterministic Pcg32 field) registered
// into the world's DYNAMIC cluster under the kDecalBlood*Uri contract —
// decal entities resolve them through the cluster pool like cooked
// content. Idempotent per world. False only on texture-creation failure.
bool InitDemoDecalTextures(smvk::ResourceManager& ctx, smvk::World& world);
// Sponza check-scene hero splats (floor pools + wall smear + arcade
// spray) as WORLD entities (CreateDecalAt into the dynamic cluster).
// No-op when the world already carries decal content.
void SpawnSponzaDecalPresets(smvk::World& world);

} // namespace app
