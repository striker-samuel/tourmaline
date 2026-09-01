#include <iostream>
#include <chrono>

#include <Engine/core.h>
#include <Engine/App/app.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/platform.h>


smvk::IApp::~IApp() {
	if (window != nullptr) {
		glfwDestroyWindow(window);
		window = nullptr;
	}
	glfwTerminate();
}

bool smvk::IApp::Run() {
	bool result = Initialize();
	assert(result && "Failed to initialize");
	MainLoop();
	// Ensure GPU idle and derived Holders are released while object is
	// still fully constructed (all members alive). This avoids destroying
	// VkImage/Buffer-backed Holders after the device/allocator is gone.
	Shutdown();
	return true;
};

void smvk::IApp::UpdateDeltaTime() {

	static auto lastFrameTime = std::chrono::high_resolution_clock::now();
	static auto startTime = std::chrono::high_resolution_clock::now();
	auto currentTime = std::chrono::high_resolution_clock::now();

	time_since_start_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - startTime).count();

	delta_time_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - lastFrameTime).count();

	lastFrameTime = currentTime;
}

void smvk::IApp::MainLoop() {
	while (!glfwWindowShouldClose(window)) {
		m_input.beginFrame();
		if (m_input.isKeyHeld(GLFW_KEY_ESCAPE)) {
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}
		glfwPollEvents();
		EarlyUpdate();
		Update();
		LateUpdate();

		DrawFrame();
	}
}

#pragma region ===[ Callbacks ]===

void smvk::IApp::keyCallback(
	GLFWwindow* window,
	int key,
	int scancode,
	int action,
	int mods
) {
	auto* app = static_cast<smvk::IApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onKey(key, scancode, action, mods);
	}
}


void smvk::IApp::mouseButtonCallback(
	GLFWwindow* window,
	int button,
	int action,
	int mods
) {
	auto* app = static_cast<smvk::IApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onMouseButton(button, action, mods);
	}
}


void smvk::IApp::cursorPositionCallback(
	GLFWwindow* window,
	double xpos,
	double ypos
) {
	auto* app = static_cast<smvk::IApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onCursorMove(xpos, ypos);
	}
}


void smvk::IApp::scrollCallback(
	GLFWwindow* window,
	double xoffset,
	double yoffset
) {
	auto* app = static_cast<smvk::IApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onScroll(xoffset, yoffset);
	}
}

void smvk::IApp::framebufferResizeCallback(GLFWwindow* window, int width,
	int height) {
	auto app =
		reinterpret_cast<smvk::IApp*>(glfwGetWindowUserPointer(window));
	if (app) {
		app->OnFramebufferResized(width, height);
	}
}

#pragma endregion