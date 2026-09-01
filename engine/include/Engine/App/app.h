#pragma once
//
#include <Engine/App/world.h>
#include <Engine/smvk.h>
#include <GLFW/glfw3.h>

#include "input.h"
#include <algorithm>
#include <cassert>
#include <future>

namespace smvk
{

	struct RenderGraph;
	class IApp {
	public:
		Input m_input{};
		IApp() = default;
		virtual ~IApp() = default;
		virtual bool Initialize() = 0;
		virtual void DrawFrame() = 0;
		//virtual ResourceManagerImpl* GetContext();
		//virtual SwapchainImpl* GetSwawphain();
		bool Run();
		void MainLoop();
		void UpdateDeltaTime();
	protected:
		std::unique_ptr<ResourceManagerImpl> m_context; // two should be reduced to one really
		std::unique_ptr<SwapchainImpl> m_swapchain;
		std::unique_ptr<RenderGraph> m_rendergraph;
		std::unique_ptr<World> m_world;
#pragma region ===[ App / Core State ]===
		float delta_time_s = .0f;
		float time_since_start_s = .0f;
#pragma endregion

#pragma region ===[ GLFW / Window State ]===
		GLFWwindow * window = nullptr;
#pragma endregion

#pragma region Events
		virtual void EarlyUpdate() {};
		virtual void Update() {}
		virtual void LateUpdate() {}
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