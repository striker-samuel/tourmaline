#pragma once
//
#include <GLFW/glfw3.h>

#include "input.h"
#include <algorithm>
#include <cassert>
#include <future>

namespace smvk
{

	class IApp {
	public:
		Input m_input{};
		IApp() = default;
		virtual ~IApp();
		virtual bool Initialize() = 0;
		virtual void DrawFrame() = 0;
		// Called while object is still fully constructed, after MainLoop exits
		// and before any members are destroyed. Override to release Holders
		// that depend on the graphics context while GPU is still valid.
		virtual void Shutdown() {}
		bool Run();
		void MainLoop();
		void UpdateDeltaTime();
	protected:
#pragma region ===[ App / Core State ]===
		float delta_time_s = .0f;
		float time_since_start_s = .0f;
#pragma endregion

#pragma region ===[ GLFW / Window State ]===
		// Window is first member so derived graphics members (in VulkanApp)
		// are destroyed *before* the window (base members destroyed last).
		GLFWwindow * window = nullptr;
#pragma endregion

#pragma region Events
		virtual void EarlyUpdate() {};
		virtual void Update() {}
		virtual void LateUpdate() {}
		virtual void OnFramebufferResized(int width, int height) {}
#pragma endregion

#pragma region ===[ GlfwCallbacks ]===

		static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

		static void keyCallback(
			GLFWwindow* window,
			int key,
			int scancode,
			int action,
			int mods
		);

		static void mouseButtonCallback(
			GLFWwindow* window,
			int button,
			int action,
			int mods
		);

		static void cursorPositionCallback(
			GLFWwindow* window,
			double xpos,
			double ypos
		);

		static void scrollCallback(
			GLFWwindow* window,
			double xoffset,
			double yoffset
		);

#pragma endregion
	};
};