// App entry point: picks a scene sample and runs it through the shared
// SceneRenderer framegraph (see App/scene_renderer.h). Adding a sample is a
// SceneDesc row in SceneRegistry — never a copy of the framegraph.
//
// Usage:
//   App [--scene sponza|bistro] [--minimal]
// --minimal runs the reduced pass set (no shadows/filter/blur/AO/bloom).

#include <App/scene_renderer.h>

#include <cstring>
#include <iostream>

namespace {

void PrintUsage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "App")
		<< " [--scene <id>] [--minimal]\n";
	std::cout << "Available scenes:\n";
	for (const app::SceneDesc& s : app::SceneRegistry::All()) {
		std::cout << "  " << s.id << " — " << s.label
			<< " (" << s.gltfPath << ")\n";
	}
}

} // namespace

int main(int argc, const char* argv[]) {
	const char* sceneId = "sponza";
	bool minimal = false;
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
	renderer.Configure(*scene,
		minimal ? app::ScenePassConfig::Minimal() : app::ScenePassConfig::All());
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
