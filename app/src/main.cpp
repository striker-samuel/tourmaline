#pragma once


#define TINYGLTF_IMPLEMENTATION
#include <Engine/App/app.h>

// --------------APP--------------------

int main(int argc, const char* argv[]) {
	if (argc > 1) {
		std::cout << argv[0];
	}

	VulkanApp app = VulkanApp();
	try {
		app.Run();
	}
	catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
