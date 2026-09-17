// GltfEditor entry point: inspect + cook glTF scenes.
//
// Usage:
//   Editor [--gltf <path>] [--out-pak <path>]
//
// With no --gltf the editor opens empty (File > Open glTF...).
// Camera: orbit with left-drag on empty space / WASD+mouse in FPS mode (I).
// Selection: left-click a mesh, or pick it in the outliner.

#include "editor_app.h"

#include <cstring>
#include <iostream>

namespace {

void PrintUsage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "Editor")
		<< " [--gltf <path.gltf>] [--out-pak <path.pak>]\n";
}

} // namespace

int main(int argc, const char* argv[]) {
	const char* gltfPath = nullptr;
	const char* outPak = nullptr;
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if (std::strcmp(a, "--gltf") == 0 && i + 1 < argc) {
			gltfPath = argv[++i];
		}
		else if (std::strncmp(a, "--gltf=", 7) == 0) {
			gltfPath = a + 7;
		}
		else if (std::strcmp(a, "--out-pak") == 0 && i + 1 < argc) {
			outPak = argv[++i];
		}
		else if (std::strncmp(a, "--out-pak=", 10) == 0) {
			outPak = a + 10;
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

	editor::EditorApp app;
	if (gltfPath)
		if (!app.OpenScene(gltfPath)) {
			std::cerr << "Editor: failed to queue scene '" << gltfPath << "'\n";
			return EXIT_FAILURE;
		}
	if (outPak)
		app.SetOutPak(outPak);
	try {
		if (!app.Run())
			return EXIT_FAILURE;
	}
	catch (const std::exception& e) {
		std::cerr << "\nERROR: " << e.what() << std::endl;
		std::cerr << "\nPress Enter to exit...";
		std::cin.get();
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
