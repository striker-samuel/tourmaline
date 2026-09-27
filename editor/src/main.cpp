// GltfEditor entry point: inspect + cook glTF scenes.
//
// Usage:
//   Editor [--gltf <path>] [--out-pak <path>]
//
// With no --gltf the editor opens empty (File > Open glTF...).
// Camera: orbit-only (Alt+LMB tumble, Alt+MMB track, Alt+RMB dolly,
// wheel zoom, F / Shift+F framing).
// Selection: left-click a mesh, or pick it in the outliner.

#include "editor_app.h"

#include <cstring>
#include <iostream>

namespace {

void PrintUsage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "Editor")
		<< " [--gltf <path.gltf> | --pak <path.pak>[:N]] [--out-pak <path.pak>]\n"
		<< "  --pak opens a cooked pak read-only (bake inspection: UV1 view,\n"
		<< "  lightmap panel, sibling gi/<stem>.sglm auto-load).\n"
		<< "  :N selects pak scene index N (default 0; scenes listed at load).\n"
		<< "  --wysiwyg-smoke: headless-ish crash repro — cooks the scene\n"
		<< "  minimal, exercises the auto-preview + the in-place rebind, then\n"
		<< "  exits (dev tool).\n"
		<< "  --project-smoke <dir>: with --gltf <asset>: create/open the\n"
		<< "  project at <dir>, import the asset, cook it (product step),\n"
		<< "  then run the wysiwyg-smoke stages over it (dev tool).\n";
}

} // namespace

	int main(int argc, const char* argv[]) {
	const char* scenePath = nullptr;
	const char* outPak = nullptr;
	const char* projectSmoke = nullptr;
	bool wysiwygSmoke = false;
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if (std::strcmp(a, "--gltf") == 0 && i + 1 < argc) {
			scenePath = argv[++i];
		}
		else if (std::strncmp(a, "--gltf=", 7) == 0) {
			scenePath = a + 7;
		}
		else if (std::strcmp(a, "--pak") == 0 && i + 1 < argc) {
			scenePath = argv[++i];
		}
		else if (std::strncmp(a, "--pak=", 6) == 0) {
			scenePath = a + 6;
		}
		else if (std::strcmp(a, "--out-pak") == 0 && i + 1 < argc) {
			outPak = argv[++i];
		}
		else if (std::strncmp(a, "--out-pak=", 10) == 0) {
			outPak = a + 10;
		}
		else if (std::strcmp(a, "--wysiwyg-smoke") == 0) {
			wysiwygSmoke = true;
		}
		else if (std::strcmp(a, "--project-smoke") == 0 && i + 1 < argc) {
			projectSmoke = argv[++i];
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
	if (projectSmoke) {
		// Project flow owns the --gltf path as its import source (it
		// is copied into the project, not opened in place).
		if (!scenePath) {
			std::cerr << "--project-smoke <dir> needs --gltf <asset>\n";
			return EXIT_FAILURE;
		}
		app.SetProjectSmoke(projectSmoke, scenePath);
	}
	else if (scenePath) {
		if (!app.OpenScene(scenePath)) {
			std::cerr << "Editor: failed to queue scene '" << scenePath << "'\n";
			return EXIT_FAILURE;
		}
	}
	if (outPak)
		app.SetOutPak(outPak);
	if (wysiwygSmoke)
		app.SetWysiwygSmoke();
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
