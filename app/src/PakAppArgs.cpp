// CLI parsing for the pak-viewer host (the APP's own host layer; fills
// PakAppConfig from argv — see Host.h).

#include "Host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace app {
namespace {

void PrintUsage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "App")
		<< " [--pak <path.pak>] [--scene-index N]\n"
		<< "  [--minimal] [--filter-only] [--with <csv>] [--without <csv>]\n"
		<< "  [--bench <frames>] [--bench_move <m/s>] [--present <fifo|mailbox|immediate>] [--disable_control] [--no-dlss]\n"
		<< "  [--use_optick] [--use_optick_gpu] [--no-vfs]\n"
		<< "  --pak runs a cooked pak (default content/paks/sponza.pak);\n"
		<< "  scenes inside are discovered at startup, index 0 loads unless\n"
		<< "  --scene-index picks another. The App loads OUR format only:\n"
		<< "  preprocess glTF through the editor (import -> cook).\n"
		<< "  --disable_control freezes camera input for deterministic benching\n"
		<< "  --bench_move flies forward at constant velocity (default 4 m/s) past warmup\n"
		<< "  --bench also pins fixed manual exposure (no auto-exposure)\n"
		<< "  --no-dlss keeps the Streamline interposer out of the Vulkan chain\n"
		<< "  (RenderDoc captures crash through the SL layer + NGX cubins)\n"
		<< "  --use_optick enables the Optick CPU capture session (bench.opt)\n"
		<< "  --use_optick_gpu also brings up Optick GPU timelines (implies --use_optick)\n"
		<< "  --no-vfs mutes the loose VFS discovery mounts: only the pak\n"
		<< "  backend serves scene content (proves the pak is self-contained)\n";
}

} // namespace

bool ParsePakAppArgs(int argc, const char* argv[], PakAppConfig& cfg,
	int& exitCode) {
	// Clean slate — EXCEPT cfg.run: main() pre-installs the central
	// RunConfig (parsed once, logged, unknowns warned) BEFORE this parse.
	// The old wholesale reset silently wiped it, so every RunConfig-carried
	// env pin no-op'd: VULKANTOY_CAM_POS/FWD/YAW above all (the documented
	// bench pins) never reached the renderer — headless A/B cameras sat at
	// the hard-coded (4,3,6) default pose no matter the pin (observed:
	// three different pinned poses rendered byte-identical captures).
	const engine::RunConfig runPreserve = cfg.run;
	cfg = PakAppConfig{};
	cfg.run = runPreserve;
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if ((std::strcmp(a, "--pak") == 0) && i + 1 < argc) {
			cfg.pakPath = argv[++i];
		}
		else if (std::strncmp(a, "--pak=", 6) == 0) {
			cfg.pakPath = a + 6;
		}
		else if (std::strcmp(a, "--gltf") == 0
			|| std::strncmp(a, "--gltf=", 7) == 0) {
			std::cerr << "--gltf was removed: the App loads OUR format "
				"only (cooked paks). Preprocess glTF through the "
				"editor (import -> cook), then --pak the result.\n";
			exitCode = EXIT_FAILURE;
			return false;
		}
		else if ((std::strcmp(a, "--scene-index") == 0) && i + 1 < argc) {
			cfg.sceneIndex = std::atoi(argv[++i]);
		}
		else if (std::strncmp(a, "--scene-index=", 14) == 0) {
			cfg.sceneIndex = std::atoi(a + 14);
		}
		else if (std::strcmp(a, "--minimal") == 0) {
			cfg.minimal = true;
		}
		else if (std::strcmp(a, "--filter-only") == 0) {
			cfg.filterOnly = true;
		}
		else if ((std::strcmp(a, "--with") == 0) && i + 1 < argc) {
			cfg.withCsv = argv[++i];
		}
		else if (std::strncmp(a, "--with=", 7) == 0) {
			cfg.withCsv = a + 7;
		}
		else if ((std::strcmp(a, "--without") == 0) && i + 1 < argc) {
			cfg.withoutCsv = argv[++i];
		}
		else if (std::strncmp(a, "--without=", 10) == 0) {
			cfg.withoutCsv = a + 10;
		}
		else if (std::strcmp(a, "--bench") == 0 && i + 1 < argc) {
			cfg.benchFrames = std::atoi(argv[++i]);
		}
		else if (std::strncmp(a, "--bench=", 8) == 0) {
			cfg.benchFrames = std::atoi(a + 8);
		}
		else if (std::strcmp(a, "--bench_move") == 0
			|| std::strcmp(a, "--bench-move") == 0) {
			// Bare flag: default flight speed (4 m/s walks the bistro street
			// at a good LOD/streaming churn rate). Optional value follows.
			cfg.benchMoveSpeed = 4.0f;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				cfg.benchMoveSpeed = static_cast<float>(std::atof(argv[++i]));
		}
		else if (std::strncmp(a, "--bench_move=", 14) == 0
			|| std::strncmp(a, "--bench-move=", 13) == 0) {
			const char* v = a + (std::strncmp(a, "--bench_move=", 14) == 0 ? 14 : 13);
			cfg.benchMoveSpeed = static_cast<float>(std::atof(v));
		}
		else if ((std::strcmp(a, "--present") == 0) && i + 1 < argc) {
			cfg.presentMode = argv[++i];
		}
		else if (std::strncmp(a, "--present=", 10) == 0) {
			cfg.presentMode = a + 10;
		}
		else if (std::strcmp(a, "--disable_control") == 0 || std::strcmp(a, "--disable-control") == 0) {
			cfg.disableControl = true;
		}
		else if (std::strcmp(a, "--use_optick") == 0) {
			cfg.useOptick = true;
		}
		else if (std::strcmp(a, "--use_optick_gpu") == 0) {
			cfg.useOptick = true;
			cfg.useOptickGpu = true;
		}
		else if (std::strcmp(a, "--no-dlss") == 0 || std::strcmp(a, "--no_dlss") == 0) {
			cfg.noDlss = true;
		}
		else if (std::strcmp(a, "--no-vfs") == 0 || std::strcmp(a, "--no_vfs") == 0) {
			cfg.noVfs = true;
		}
		else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
			PrintUsage(argv[0]);
			exitCode = EXIT_SUCCESS;
			return false;
		}
		else {
			std::cerr << "Unknown argument: " << a << "\n";
			PrintUsage(argv[0]);
			exitCode = EXIT_FAILURE;
			return false;
		}
	}
	exitCode = EXIT_SUCCESS;
	return true;
}

} // namespace app
