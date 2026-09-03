#include <iostream>
#include <chrono>

#include <Engine/core.h>
#include <Engine/App/app.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/platform.h>
#include <Engine/Profile/profile.h>


bool smvk::App::Run() {
	bool result = Initialize();
	assert(result && "Failed to initialize");
	MainLoop();
	// Ensure GPU idle and derived Holders are released while object is
	// still fully constructed (all members alive). This avoids destroying
	// VkImage/Buffer-backed Holders after the device/allocator is gone.
	Shutdown();
	return true;
};

void smvk::App::UpdateDeltaTime() {

	static auto lastFrameTime = std::chrono::high_resolution_clock::now();
	static auto startTime = std::chrono::high_resolution_clock::now();
	auto currentTime = std::chrono::high_resolution_clock::now();

	time_since_start_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - startTime).count();

	delta_time_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - lastFrameTime).count();

	lastFrameTime = currentTime;
}

void smvk::App::MainLoop() {
	SMVK_PROFILE_THREAD("MainThread");
	while (!glfwWindowShouldClose(window)) {
		SMVK_PROFILE_FRAME("MainThread");
		{
			SMVK_PROFILE_SCOPE_COLOR("PollEvents", Optick::Color::Ivory);
			m_input.beginFrame();
			if (m_input.isKeyHeld(GLFW_KEY_ESCAPE)) {
				glfwSetWindowShouldClose(window, GLFW_TRUE);
			}
			glfwPollEvents();
		}
		{
			SMVK_PROFILE_CATEGORY("EarlyUpdate", Optick::Category::GameLogic);
			EarlyUpdate();
		}
		{
			SMVK_PROFILE_CATEGORY("Update", Optick::Category::GameLogic);
			Update();
		}
		{
			SMVK_PROFILE_CATEGORY("LateUpdate", Optick::Category::GameLogic);
			LateUpdate();
		}
		{
			SMVK_PROFILE_CATEGORY("DrawFrame", Optick::Category::Rendering);
			DrawFrame();
		}
	}
}

#pragma region ===[ Callbacks ]===

void smvk::App::keyCallback(
	GLFWwindow* window,
	int key,
	int scancode,
	int action,
	int mods
) {
	auto* app = static_cast<smvk::App*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onKey(key, scancode, action, mods);
	}
}


void smvk::App::mouseButtonCallback(
	GLFWwindow* window,
	int button,
	int action,
	int mods
) {
	auto* app = static_cast<smvk::App*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onMouseButton(button, action, mods);
	}
}


void smvk::App::cursorPositionCallback(
	GLFWwindow* window,
	double xpos,
	double ypos
) {
	auto* app = static_cast<smvk::App*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onCursorMove(xpos, ypos);
	}
}


void smvk::App::scrollCallback(
	GLFWwindow* window,
	double xoffset,
	double yoffset
) {
	auto* app = static_cast<smvk::App*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onScroll(xoffset, yoffset);
	}
}

void smvk::App::framebufferResizeCallback(GLFWwindow* window, int width,
	int height) {
	auto app =
		reinterpret_cast<smvk::App*>(glfwGetWindowUserPointer(window));
	if (app) {
		app->OnFramebufferResized(width, height);
	}
}

#pragma endregion