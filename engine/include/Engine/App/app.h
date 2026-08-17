#pragma once

#include <algorithm>
#include <cassert>
#include <future>
//
#include "volk.h"
#include "vk_mem_alloc.h"
//
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
//
#include "input.h"
#include <Engine/Vulkan/core.h>

namespace smvk {

	struct RenderGraph;

	class VulkanApp {
	public:
		Input m_input{};
		VulkanApp();
		virtual ~VulkanApp();
		virtual void DrawFrame() = 0;
		virtual void CleanUp();
		virtual bool Initialize();
		bool Run();
		void MainLoop();
		void UpdateDeltaTime();
	protected:
		std::unique_ptr<VulkanCore> core_;
		std::unique_ptr<VulkanResourceManager> m_context;
		std::unique_ptr<VulkanSwapchain> m_swapchain;
		std::unique_ptr<RenderGraph> m_rendergraph;
		ImGuiContext* m_imguiCtx = nullptr;
		//Holder<DescriptorPoolHandle> imgui_descriptorPool;
#pragma region ===[ App / Core State ]===
		float delta_time_s = .0f;
		float time_since_start_s = .0f;
#pragma endregion

#pragma region ===[ GLFW / Window State ]===
		GLFWwindow * window = nullptr;
#pragma endregion

#pragma region Initialization
		void InitializeImGui(); // This will be removed and made not app core
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