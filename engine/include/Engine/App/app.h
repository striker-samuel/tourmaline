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
#include <Engine/Renderer/rendergraph.h>
//
#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>
//
#include <Engine/Utils/gltf.h> 
#include <Engine/Utils/bvh.h>
#include <Engine/platform.h>

namespace smvk {

	class VulkanApp {
	public:
		std::unique_ptr<VulkanCore> core_;
		Input input;
		VulkanApp();
		virtual ~VulkanApp();
		virtual void DrawFrame() = 0;
		virtual void CleanUp();
		virtual bool Initialize();
		bool Run() {};
		void MainLoop();
		void UpdateDeltaTime();

		VulkanCore& GetCore();
		const VulkanCore& GetCore() const;
	protected:
		Context* m_context = nullptr;
		std::unique_ptr<RenderGraph> m_rendergraph;
		ImGuiContext* m_imguiCtx = nullptr;
		VulkanSwapchain* m_swapchain = nullptr;
		VkSurfaceKHR m_surface;

#pragma region ===[ App / Core State ]===

		// This needs rework. naive float for elpased time leads to problems in the long run!
		float delta_time_s = .0f;
		float time_since_start_s = .0f;

		glm::mat4 frozenMainView = {};

#pragma endregion

#pragma region ===[ GLFW / Window State ]===
		GLFWwindow * window = nullptr;
#pragma endregion

#pragma region ===[ Frame Commands List ]===
		VkCommandPool perFrameCommandPools[MAX_FRAMES_IN_FLIGHT]{};
		VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT]{};
#pragma endregion

#pragma region ===[ Flags / Events ]===
		bool framebufferResized = false;
#pragma endregion

#pragma region Initialization
		void InitializeImGui();
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
		ToyFrameBuffer AcquireFramebuffer();
		void EndAndPresentFramebuffer(ToyFrameBuffer& cb);
	};
};