// App entry point: picks a scene sample and runs it through the shared
// SceneRenderer framegraph (see App/scene_renderer.h). Adding a sample is a
// SceneDesc row in SceneRegistry — never a copy of the framegraph.
//
// Usage:
//   App [--scene sponza|bistro] [--minimal] [--filter-only] [--with csv]
// --minimal runs the reduced pass set (no shadows/filter/blur/AO/bloom).

#include <App/scene_renderer.h>

#include <cstring>
#include <iostream>

namespace {

void PrintUsage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "App")
		<< " [--scene <id>] [--minimal] [--filter-only] [--with <csv>] [--without <csv>] [--no-pak]\n";
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
