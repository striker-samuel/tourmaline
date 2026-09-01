#pragma once


#if defined(ENGINE_VULKAN)
#include <volk.h>
namespace smvk {

	class VulkanApp;
	struct VulkanSwapchain;
	struct VulkanResourceManager;
	struct VulkanPipelineCache;
	struct VulkanFrameBuffer;
	struct VulkanPipeline;

	using ResourceManagerImpl = VulkanResourceManager;
	using SwapchainImpl = VulkanSwapchain;
	using AppImpl = VulkanApp;
	using FrameBufferImpl = VulkanFrameBuffer;
	using PipelineCacheImpl = VulkanPipelineCache;
	using PipelineImpl = VulkanPipeline;
	using CommandBufferImpl = VkCommandBuffer;

};
#endif
