#pragma once
//
#include <GLFW/glfw3.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>

#include "input.h"
#include <algorithm>
#include <cassert>
#include <future>
#include <memory>

namespace smvk
{
	class World;
	class RenderGraph;

	class App {
	public:
		Input m_input{};
		App() = default;
		virtual ~App();
		virtual bool Initialize() = 0;
		virtual void DrawFrame() = 0;
		// Called while object is still fully constructed, after MainLoop exits
		// and before any members are destroyed. Override to release Holders
		// that depend on the graphics context while GPU is still valid.
		virtual void Shutdown();
		bool Run();
		void MainLoop();
		void UpdateDeltaTime();
	protected:
#pragma region ===[ App / Core State ]===
		float delta_time_s = .0f;
		float time_since_start_s = .0f;
#pragma endregion

#pragma region ===[ Graphics State ]===
		// Graphics members live in App so they are destroyed BEFORE window.
		std::unique_ptr<ResourceManager> m_context;
		std::unique_ptr<Swapchain> m_swapchain;
		std::unique_ptr<RenderGraph> m_rendergraph;
		std::unique_ptr<World> m_world;
		void WaitForDeviceIdle();
#pragma endregion

#pragma region ===[ GLFW / Window State ]===
		GLFWwindow * window = nullptr;
#pragma endregion

#pragma region Events
		virtual void EarlyUpdate() {}
		virtual void Update() {}
		virtual void LateUpdate() {}
		virtual void OnFramebufferResized(int width, int height);
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

	// Backward-compat aliases – old code used IApp / AppImpl (VulkanApp)
	using IApp = App;
	using AppImpl = App;

};