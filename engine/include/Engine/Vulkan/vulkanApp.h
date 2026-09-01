#pragma once

#include "impl.h"
#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/Rendergraph/rendergraph.h>

namespace smvk {
	class VulkanApp : public IApp {
	public:
		bool Initialize() override;
		void Shutdown() override;
		~VulkanApp() override;
		void OnFramebufferResized(int width, int height) override;

	protected:
		// Graphics members live in VulkanApp (derived) so they are destroyed
		// BEFORE the base IApp::window. This guarantees VkSurfaceKHR /
		// VkSwapchainKHR are torn down before the GLFW window.
		std::unique_ptr<ResourceManagerImpl> m_context;
		std::unique_ptr<SwapchainImpl> m_swapchain;
		std::unique_ptr<RenderGraph> m_rendergraph;
		std::unique_ptr<World> m_world;

		// Helper for derived apps to wait for GPU idle before releasing
		// Holders that reference the context. Tolerates DEVICE_LOST on shutdown.
		void WaitForDeviceIdle();
	};
}