// App entry point: picks a scene sample and runs it through the shared
// SceneRenderer framegraph (see App/scene_renderer.h). Adding a sample is a
// SceneDesc row in SceneRegistry — never a copy of the framegraph.
//
// Usage:
//   App [--scene sponza|bistro] [--minimal] [--filter-only] [--with csv]
// --minimal runs the reduced pass set (no shadows/filter/blur/AO/bloom).
// --disable_control freezes the camera (no keyboard/gamepad/mouselook):
// deterministic A/B benching at the scene's configured pose.

#include <App/scene_renderer.h>
#include <Engine/Profile/profile.h>

#include <cstring>
#include <iostream>

namespace {

void PrintUsage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "App")
		<< " [--scene <id>] [--minimal] [--filter-only] [--with <csv>] [--without <csv>] [--no-pak]\n"
		<< "  [--bench <frames>] [--present <fifo|mailbox|immediate>] [--disable_control]\n"
		<< "  [--use_optick] [--use_optick_gpu]\n"
		<< "  --disable_control freezes camera input for deterministic benching\n"
		<< "  --use_optick enables the Optick CPU capture session (bench.opt)\n"
		<< "  --use_optick_gpu also brings up Optick GPU timelines (implies --use_optick)\n";
	std::cout << "Available scenes:\n";
	for (const app::SceneDesc& s : app::SceneRegistry::All()) {
		std::cout << "  " << s.id << " — " << s.label
			<< " (" << s.gltfPath << ")\n";
	}
}

} // namespace

	int main(int argc, const char* argv[]) {
	const char* sceneId = "bistro";
	bool minimal = false;
	bool filterOnly = false;
	const char* withCsv = nullptr;
	const char* withoutCsv = nullptr;
	bool noPak = false;
	int benchFrames = 0;
	const char* presentMode = nullptr;
	bool disableControl = false;
	bool useOptick = false;
	bool useOptickGpu = false;
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if ((std::strcmp(a, "--scene") == 0 || std::strcmp(a, "-s") == 0) && i + 1 < argc) {
			sceneId = argv[++i];
		}
		else if (std::strncmp(a, "--scene=", 8) == 0) {
			sceneId = a + 8;
		}
		else if (std::strcmp(a, "--minimal") == 0) {
			minimal = true;
		}
		else if (std::strcmp(a, "--filter-only") == 0) {
			filterOnly = true;
		}
		else if ((std::strcmp(a, "--with") == 0) && i + 1 < argc) {
			withCsv = argv[++i];
		}
		else if (std::strncmp(a, "--with=", 7) == 0) {
			withCsv = a + 7;
		}
		else if ((std::strcmp(a, "--without") == 0) && i + 1 < argc) {
			withoutCsv = argv[++i];
		}
		else if (std::strncmp(a, "--without=", 10) == 0) {
			withoutCsv = a + 10;
		}
		else if (std::strcmp(a, "--no-pak") == 0 || std::strcmp(a, "--gltf") == 0) {
			noPak = true;
		}
		else if ((std::strcmp(a, "--bench") == 0) && i + 1 < argc) {
			benchFrames = std::atoi(argv[++i]);
		}
		else if (std::strncmp(a, "--bench=", 8) == 0) {
			benchFrames = std::atoi(a + 8);
		}
		else if ((std::strcmp(a, "--present") == 0) && i + 1 < argc) {
			presentMode = argv[++i];
		}
		else if (std::strncmp(a, "--present=", 10) == 0) {
			presentMode = a + 10;
		}
		else if (std::strcmp(a, "--disable_control") == 0 || std::strcmp(a, "--disable-control") == 0) {
			disableControl = true;
		}
		else if (std::strcmp(a, "--use_optick") == 0) {
			useOptick = true;
		}
		else if (std::strcmp(a, "--use_optick_gpu") == 0) {
			useOptick = true;
			useOptickGpu = true;
		}
		else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
			PrintUsage(argv[0]);
			return EXIT_SUCCESS;
		}
		else {
			std::cerr << "Unknown argument: " << a << "\n";
			PrintUsage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	auto scene = app::SceneRegistry::Find(sceneId ? sceneId : "");
	if (!scene) {
		std::cerr << "Unknown scene: '" << (sceneId ? sceneId : "") << "'\n";
		PrintUsage(argv[0]);
		return EXIT_FAILURE;
	}

	app::SceneRenderer renderer;
	app::ScenePassConfig passCfg = app::ScenePassConfig::All();
	if (filterOnly)
		passCfg = app::ScenePassConfig::FilterOnly();
	else if (minimal)
		passCfg = app::ScenePassConfig::Minimal();
	// Bisection: layer extra passes onto the base config
	// (csv: deferred,shadows,ssao,smaa,motion,bloom,exposure,msaa,skydome,ui,cull).
	auto applyCsv = [&](const char* csv, bool enable) {
		if (!csv) return;
		const std::string s = csv;
		size_t pos = 0;
		while (pos <= s.size()) {
			size_t end = s.find(',', pos);
			if (end == std::string::npos) end = s.size();
			const std::string tok = s.substr(pos, end - pos);
			bool* flag = nullptr;
			if (tok == "deferred") flag = &passCfg.deferredShadows;
			else if (tok == "shadows") { passCfg.directionalShadows = passCfg.spotShadows = enable; }
			else if (tok == "ssao") flag = &passCfg.ssao;
			else if (tok == "smaa") flag = &passCfg.smaa;
			else if (tok == "motion") { passCfg.motionVectors = passCfg.motionBlur = enable; }
			else if (tok == "bloom") flag = &passCfg.bloom;
			else if (tok == "exposure") flag = &passCfg.exposure;
			else if (tok == "msaa") flag = &passCfg.msaa;
			else if (tok == "skydome") flag = &passCfg.skydome;
			else if (tok == "ui") flag = &passCfg.ui;
			else if (tok == "cull") flag = &passCfg.forwardPlusCull;
			else if (tok == "prepass") flag = &passCfg.forwardPlusPrepass;
			else if (tok == "filter") flag = &passCfg.gfxFilter;
			else if (!tok.empty()) std::cerr << "Unknown pass token: '" << tok << "'\n";
			if (flag) *flag = enable;
			pos = end + 1;
		}
	};
	applyCsv(withCsv, true);
	applyCsv(withoutCsv, false);
	renderer.Configure(*scene, passCfg);
	renderer.SetForceGltf(noPak);
	if (disableControl) {
		renderer.SetDisableControl(true);
		std::cerr << "[Scene] camera control disabled (--disable_control)\n";
	}
	if (presentMode) {
		if (std::strcmp(presentMode, "fifo") == 0)
			renderer.SetPresentMode(smvk::PresentMode::Fifo);
		else if (std::strcmp(presentMode, "mailbox") == 0)
			renderer.SetPresentMode(smvk::PresentMode::Mailbox);
		else if (std::strcmp(presentMode, "immediate") == 0)
			renderer.SetPresentMode(smvk::PresentMode::Immediate);
		else {
			std::cerr << "Unknown present mode: '" << presentMode << "' (fifo|mailbox|immediate)\n";
			return EXIT_FAILURE;
		}
	}
	if (benchFrames > 0)
		renderer.SetFrameBudget(benchFrames);
	// Optick gates must be set before Run(): the engine initializes the GPU
	// profiler during VulkanCore init, and the bench session starts after the
	// warmup frames. --use_optick_gpu implies --use_optick.
	smvk::profiler::SetEnabled(useOptick);
	if (useOptickGpu)
		smvk::profiler::SetGpuEnabled(true);
	try {
		renderer.Run();
	}
	catch (const std::exception& e) {
		std::cerr << "\nERROR: " << e.what() << std::endl;
		std::cerr << "\nPress Enter to exit...";
		std::cin.get();
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
