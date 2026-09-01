#pragma once

#if defined(ENGINE_VULKAN)
namespace smvk {

	class VulkanApp;
	struct VulkanSwapchain;
	struct VulkanResourceManager;
	struct PipelineCache;
	struct FrameBuffer;

	using ResourceManagerImpl = VulkanResourceManager;
	using SwapchainImpl = VulkanSwapchain;
	using AppImpl = VulkanApp;

};
#else
#error "No rendering backend selected"
#endif

