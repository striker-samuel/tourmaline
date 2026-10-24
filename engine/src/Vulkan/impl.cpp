// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <span>
#include <optional>
#include <unordered_set>

//#define DEBUG_COMMAND_SUBMISSION

#define VMA_IMPLEMENTATION
#define VMA_VULKAN_VERSION 1003800
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

// Win32 surface creation must go through the volk-resolved entry point so
// the Streamline interposer's eVulkan_CreateWin32SurfaceKHR proxy sees the
// surface its (proxied) swapchain calls are created against.
#ifdef _WIN32
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#endif

#define VOLK_IMPLEMENTATION

#include "volk.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/utility/vk_format_utils.h>

#include "../Vulkan/detail/vulkan_impl.h"
#include <Engine/App/app.h>
#include <Engine/App/input_sdl3.h>
#include <Engine/Memory/memory.h>
#include <Engine/Memory/linear_vector.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Rendergraph/dlss.h>
#include <Engine/RHI/swapchain.h>
#include <Engine/App/world.h>
#include <Engine/Profile/profile.h>

// Debug-object-name setter (null-guarded): vkSetDebugUtilsObjectNameEXT
// is a volk-resolved function pointer that stays NULL when the
// VK_EXT_debug_utils instance extension is not enabled — which happens on
// TOURMALINE_NO_VALIDATION=1 runs (the extension rides the validation
// layer's instance chain). Calling it unguarded crashed with a call
// through null (pc=0). A missing debug name is cosmetic — skip instead.
static inline void SetDebugObjectName(
	VkDevice device, const VkDebugUtilsObjectNameInfoEXT* info)
{
	if (vkSetDebugUtilsObjectNameEXT)
		vkSetDebugUtilsObjectNameEXT(device, info);
}

using smvk::VulkanResourceManager;
using smvk::VulkanSwapchain;
using smvk::VulkanFrameBuffer;

#if 0 // TMP-DIAG: force validation on in Release (disabled, remove later)
bool enableValidationLayers = true;
#else
#ifdef NDEBUG
bool enableValidationLayers = false;
#else
bool enableValidationLayers = true;
#endif
#endif
//
static VkInstance instance_ = VK_NULL_HANDLE;
static VkDevice device_ = VK_NULL_HANDLE;
static VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
static VkSurfaceKHR surface_ = VK_NULL_HANDLE;

static VkQueue graphicsQueue_ = VK_NULL_HANDLE;
static VkQueue presentQueue_ = VK_NULL_HANDLE;
static VkQueue transferQueue_ = VK_NULL_HANDLE;
static VkQueue computeQueue_ = VK_NULL_HANDLE;
// Set at device creation (see VulkanCore init): true when the device
// supports VK multiDrawIndirect (drawCount > 1 in a single indirect call).
static bool g_multiDrawIndirect = false;
// True when the device supports non-zero firstInstance in indirect draws
// (the gfx filter packs cluster ids there). Without it the filter's
// commands are invalid — see the feature gate in VulkanCore init.
static bool g_multiDrawIndirectFirstInstance = false;
// True when the device supports shaderDemoteToHelperInvocation (the MASK
// alpha-test shaders compile clip/discard to OpDemoteToHelperInvocation).
static bool g_shaderDemoteToHelperInvocation = false;
// True when the device was created with fillModeNonSolid enabled
// (PolygonMode::Line/Point in graphics pipelines). BuildGraphicsPipeline
// clamps non-solid modes back to Fill when this is false.
static bool g_fillModeNonSolid = false;
// True when VK_EXT_device_fault was enabled at device creation (probed,
// never required). Gates DumpDeviceFaultInfo below.
static bool g_deviceFaultSupported = false;
// True when VK_EXT_mesh_shader was enabled at device creation (probed,
// never required): taskShader + meshShader features. Gates the
// mesh-shader gfx filter path (DrawMeshTasks); the compute+indexed CPU
// fallback stays otherwise.
static bool g_meshShaderSupported = false;
struct QueueMutex { VkQueue queue = VK_NULL_HANDLE; std::mutex mutex; };
static QueueMutex graphicsQueueMutex;
static QueueMutex presentQueueMutex;
static QueueMutex transferQueueMutex;
static QueueMutex computeQueueMutex;
static std::mutex& GetQueueMutex(VkQueue q) {
	if (q == graphicsQueue_) return graphicsQueueMutex.mutex;
	if (q == presentQueue_) return presentQueueMutex.mutex;
	if (q == transferQueue_) return transferQueueMutex.mutex;
	if (q != VK_NULL_HANDLE && q == computeQueue_) return computeQueueMutex.mutex;
	static std::mutex fallback; return fallback;
}
namespace smvk {
	thread_local std::unordered_map<PoolKey, CommandBufferPool*, PoolKeyHash> t_commandPools;
	std::mutex g_allPoolsMutex;
	std::unordered_map<PoolKey, std::vector<CommandBufferPool*>, PoolKeyHash> g_allPools;
	thread_local int t_threadIdLocal = 0;
#ifdef __GNUC__
	__thread int t_threadIdGcc = 0;
#endif

	void CommandBufferPool::init(VkDevice dev, uint32_t qfi) {
		device = dev;
		queueFamilyIndex = qfi;
		VkCommandPoolCreateInfo ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		ci.queueFamilyIndex = qfi;
		VK_CHECK(vkCreateCommandPool(dev, &ci, nullptr, &pool));
		VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, kMax };
		VkCommandBuffer bufs[kMax];
		VK_CHECK(vkAllocateCommandBuffers(dev, &ai, bufs));
		for (uint32_t i = 0; i < kMax; ++i) { slots[i].allocatedCmd_ = bufs[i]; slots[i].cmd_ = VK_NULL_HANDLE; slots[i].state_ = WrapperSlot::CommandState::Free; slots[i].gen_ = 1; slots[i].retireValue_ = 0; }
		availableSlots = kMax;
	}
	void CommandBufferPool::destroy() {
		if (pool != VK_NULL_HANDLE) { vkDestroyCommandPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
	}
	CommandBufferPool* GetThreadLocalPool(uint32_t qfi, VkDevice dev, const Submitter* owner) {
		PoolKey key{ dev, qfi, owner };
		auto it = t_commandPools.find(key);
		if (it != t_commandPools.end()) return it->second;
		auto* p = mem::ProcessAllocator().Create<CommandBufferPool>();
		p->init(dev, qfi);
		t_commandPools[key] = p;
		{
			std::lock_guard<std::mutex> lock(g_allPoolsMutex);
			g_allPools[key].push_back(p);
		}
		return p;
	}
}

namespace {

	// Frame-scoped scratch vectors: allocate from the frame linear allocator
	// (rewound wholesale at the next DrawFrame via ResetFrameArena). The
	// vectors die at scope end — never live across the frame boundary —
	// so the rewind is always safe (see mem::memory.h reset discipline).
	template<typename T>
	smvk::mem::FrameVector<T> MakeFrameScratchVec() {
		return smvk::mem::FrameVector<T>{
			smvk::mem::LinearAllocatorSTL<T>{&smvk::mem::FrameAllocator()}};
	}

	inline bool is_aligned4(uint32_t v) { return (v & 3u) == 0u; }


	struct FormatInfo {
		uint32_t blockWidth;
		uint32_t blockHeight;
		uint32_t bytesPerBlock;
	};

	FormatInfo getFormatInfo(VkFormat format) {
		const VkExtent3D extent = vkuFormatTexelBlockExtent(format);
		const uint32_t size = vkuFormatTexelBlockSize(format);

		assert(size != 0 && "Unsupported/undefined VkFormat");
		assert(extent.depth == 1 && "3D block-compressed format requires blockDepth");

		return {
			extent.width,
			extent.height,
			size
		};
	}

	bool IsHostTransferSupported(VkFormat format) {
		if (physicalDevice_ == VK_NULL_HANDLE) return false;
		VkFormatProperties3 props3{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3, nullptr };
		VkFormatProperties2 props2{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &props3 };
		vkGetPhysicalDeviceFormatProperties2(physicalDevice_, format, &props2);
#ifdef VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT_EXT
		return (props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT_EXT) != 0;
#else
		return (props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT) != 0;
#endif
	}

	struct SwapChainSupportDetails {
		VkSurfaceCapabilitiesKHR capabilities{};
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> presentModes;
	};

	smvk::QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface)
	{
		smvk::QueueFamilyIndices indices;

		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(
			device,
			&queueFamilyCount,
			nullptr);

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);

		vkGetPhysicalDeviceQueueFamilyProperties(
			device,
			&queueFamilyCount,
			queueFamilies.data());


		//for (uint32_t i = 0; i < queueFamilyCount; ++i) {
		//	VkBool32 present = VK_FALSE;
		//	vkGetPhysicalDeviceSurfaceSupportKHR(
		//		device, i, surface, &present);

		//	const auto flags = queueFamilies[i].queueFlags;

		//	printf(
		//		"Queue family %u: "
		//		"count=%u G=%d C=%d T=%d S=%d P=%d\n",
		//		i,
		//		queueFamilies[i].queueCount,
		//		!!(flags & VK_QUEUE_GRAPHICS_BIT),
		//		!!(flags & VK_QUEUE_COMPUTE_BIT),
		//		!!(flags & VK_QUEUE_TRANSFER_BIT),
		//		!!(flags & VK_QUEUE_SPARSE_BINDING_BIT),
		//		!!present);
		//}

		// First pass: prefer one family that supports BOTH graphics + present.
		for (uint32_t i = 0; i < queueFamilyCount; ++i) {
			const auto& q = queueFamilies[i];

			VkBool32 presentSupport = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(
				device,
				i,
				surface,
				&presentSupport);

			const bool graphics =
				(q.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

			if (graphics && presentSupport) {
				indices.graphicsFamily = i;
				indices.presentFamily = i;
				break;
			}
		}

		// If no combined family exists, find them separately.
		if (!indices.graphicsFamily || !indices.presentFamily) {
			for (uint32_t i = 0; i < queueFamilyCount; ++i) {
				const auto& q = queueFamilies[i];

				if (!indices.graphicsFamily &&
					(q.queueFlags & VK_QUEUE_GRAPHICS_BIT))
				{
					indices.graphicsFamily = i;
				}

				VkBool32 presentSupport = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(
					device,
					i,
					surface,
					&presentSupport);

				if (!indices.presentFamily && presentSupport) {
					indices.presentFamily = i;
				}

				if (indices.graphicsFamily &&
					indices.presentFamily)
				{
					break;
				}
			}
		}

		// Prefer a truly dedicated transfer queue.
		for (uint32_t i = 0; i < queueFamilyCount; ++i) {
			const auto flags = queueFamilies[i].queueFlags;

			const bool transfer =
				(flags & VK_QUEUE_TRANSFER_BIT) != 0;
			const bool graphics =
				(flags & VK_QUEUE_GRAPHICS_BIT) != 0;
			const bool compute =
				(flags & VK_QUEUE_COMPUTE_BIT) != 0;

			if (transfer && !graphics && !compute) {
				indices.transferFamily = i;
				break;
			}
		}

		// Otherwise prefer non-graphics.
		if (!indices.transferFamily) {
			for (uint32_t i = 0; i < queueFamilyCount; ++i) {
				const auto flags = queueFamilies[i].queueFlags;

				const bool transfer =
					(flags & VK_QUEUE_TRANSFER_BIT) != 0;
				const bool graphics =
					(flags & VK_QUEUE_GRAPHICS_BIT) != 0;

				if (transfer && !graphics) {
					indices.transferFamily = i;
					break;
				}
			}
		}

		if (!indices.transferFamily) {
			indices.transferFamily = indices.graphicsFamily;
		}

		// Dedicated async-compute queue for EVSM prefiltering (and future
		// compute overlap). Prefer a compute-only family (no graphics);
		// otherwise any non-graphics compute family; otherwise alias to
		// graphics (emulated async: same-queue compute, still pipelined).
		for (uint32_t i = 0; i < queueFamilyCount; ++i) {
			const auto flags = queueFamilies[i].queueFlags;
			const bool compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
			const bool graphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
			const bool transfer = (flags & VK_QUEUE_TRANSFER_BIT) != 0;
			if (compute && !graphics && !transfer) {
				indices.computeFamily = i;
				break;
			}
		}
		if (!indices.computeFamily) {
			for (uint32_t i = 0; i < queueFamilyCount; ++i) {
				const auto flags = queueFamilies[i].queueFlags;
				const bool compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
				const bool graphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
				if (compute && !graphics) {
					indices.computeFamily = i;
					break;
				}
			}
		}
		if (!indices.computeFamily) {
			indices.computeFamily = indices.graphicsFamily;
		}

		// TOURMALINE_MONOQUEUE=1 (diag): collapse transfer + compute onto
		// the graphics family - ONE queue for everything (uploads, async
		// passes, the frame). The bisect probe for cross-queue ordering
		// races: a bug that only reproduces on the split queues (and
		// vanishes here) lives in the cross-queue sync chains, not the
		// resource lifetimes (SERIALIZE=all already proved submission
		// serialization fixes the editor churn death).
		static const bool monoQueue = [] {
			const char* e = std::getenv("TOURMALINE_MONOQUEUE");
			return e && std::atoi(e) != 0;
		}();
		if (monoQueue) {
			indices.transferFamily = indices.graphicsFamily;
			indices.computeFamily = indices.graphicsFamily;
			std::fprintf(stderr,
				"[MONOQUEUE] transfer+compute collapsed onto graphics "
				"family %u\n",
				indices.graphicsFamily.value_or(0));
		}

		return indices;
	}
	VkMemoryRequirements2 getDeviceImageMemoryRequirements(VkDevice device, const VkImageCreateInfo& imageCreateInfo) {
		VkDeviceImageMemoryRequirements queryInfo{
			.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
			.pNext = nullptr,
			.pCreateInfo = &imageCreateInfo,
			.planeAspect = VK_IMAGE_ASPECT_NONE,
		};

		VkMemoryRequirements2 requirements{
			.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
			.pNext = nullptr,
		};

		vkGetDeviceImageMemoryRequirements(
			device,
			&queryInfo,
			&requirements
		);

		return requirements;
	}
	void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
		auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")
			);

		if (func != nullptr) {
			func(instance, debugMessenger, pAllocator);
		}
	}
	VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
		std::cerr << "---------------- Vulkan Validation ----------------\n";
		std::cerr << "Message: " << pCallbackData->pMessage << "\n";
		std::cerr << "\n";  // <--- Blank line for separation

		//if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		//{
		//	__debugbreak();
		//}

		return VK_FALSE;
	}
	bool checkValidationLayerSupport(const std::vector<const char*> validationLayers) {
		uint32_t layerCount;
		vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

		std::vector<VkLayerProperties> availableLayers(layerCount);
		vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

		for (const char* layerName : validationLayers) {
			bool layerFound = false;

			for (const auto& layerProperties : availableLayers) {
				if (strcmp(layerName, layerProperties.layerName) == 0) {
					layerFound = true;
					break;
				}
			}

			if (!layerFound) {
				return false;
			}
		}

		return true;
	}
	bool IsFormatSupported(VkFormat format, VkFormatFeatureFlags requiredFeatures, VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL)
	{
		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(
			physicalDevice_,
			format,
			&props);

		const VkFormatFeatureFlags availableFeatures =
			tiling == VK_IMAGE_TILING_OPTIMAL
			? props.optimalTilingFeatures
			: props.linearTilingFeatures;

		return (availableFeatures & requiredFeatures) == requiredFeatures;
	}
	void CheckTextureFormatSupport()
	{
		constexpr VkFormatFeatureFlags required =
			VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

		assert(IsFormatSupported(
			VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
			required));

		assert(IsFormatSupported(
			VK_FORMAT_BC5_UNORM_BLOCK,
			required));

		assert(IsFormatSupported(
			VK_FORMAT_BC7_SRGB_BLOCK,
			required));
	}
	VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
		for (VkFormat vk_format : candidates) {
			VkFormatProperties props;
			vkGetPhysicalDeviceFormatProperties(physicalDevice_, vk_format, &props);
			if (tiling == VK_IMAGE_TILING_LINEAR &&
				(props.linearTilingFeatures & features) == features) {
				return vk_format;
			}
			else if (tiling == VK_IMAGE_TILING_OPTIMAL &&
				(props.optimalTilingFeatures & features) == features) {
				return vk_format;
			}
		}
		throw std::runtime_error("failed to find supported format!");
	}
	VkFormat findDepthFormat() {
		return findSupportedFormat(
			{
				//VK_FORMAT_D32_SFLOAT // no stencil. Lets just stick to stencil formats as a minimum
				VK_FORMAT_D32_SFLOAT_S8_UINT,
				VK_FORMAT_D24_UNORM_S8_UINT
			},
			VK_IMAGE_TILING_OPTIMAL,
			VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
		);
	}
	SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {

		SwapChainSupportDetails details;

		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface,
			&details.capabilities);

		uint32_t formatCount;
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
			nullptr);

		if (formatCount != 0) {
			details.formats.resize(formatCount);
			vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
				details.formats.data());
		}

		uint32_t presentModeCount;
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface,
			&presentModeCount, nullptr);

		if (presentModeCount != 0) {
			details.presentModes.resize(presentModeCount);
			vkGetPhysicalDeviceSurfacePresentModesKHR(
				device, surface, &presentModeCount, details.presentModes.data());
		}

		return details;
	}
	bool checkDeviceExtensionSupport(VkPhysicalDevice device, const std::vector<const char*> deviceExtensions) {
		uint32_t extensionCount;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
			nullptr);

		std::vector<VkExtensionProperties> availableExtensions(extensionCount);
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
			availableExtensions.data());

		std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
		for (const auto& extension : availableExtensions) {
			requiredExtensions.erase(extension.extensionName);
		}

		return requiredExtensions.empty();
	}
	bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface, const std::vector<const char*> deviceExtensions) {
		smvk::QueueFamilyIndices indices = findQueueFamilies(device, surface);
		bool extensionsSupported = checkDeviceExtensionSupport(device, deviceExtensions);

		bool swapChainAdequate = false;
		if (extensionsSupported) {
			SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device, surface);
			swapChainAdequate = !swapChainSupport.formats.empty() &&
				!swapChainSupport.presentModes.empty();
		}
		return indices.isComplete() && extensionsSupported && swapChainAdequate;
	}
	VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
		for (const auto& availableFormat : availableFormats) {
			if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
				availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
				return availableFormat;
			}
		}

		return availableFormats[0];
	}
	VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, SDL_Window* window) {

		VkExtent2D extent{};
		if (capabilities.currentExtent.width !=
			std::numeric_limits<uint32_t>::max()) {
			extent = capabilities.currentExtent;
		}
		else {
			int width = 0, height = 0;
			SDL_GetWindowSizeInPixels(window, &width, &height);

			VkExtent2D actualExtent = { static_cast<uint32_t>(width),
									   static_cast<uint32_t>(height) };

			actualExtent.width =
				std::clamp(actualExtent.width, capabilities.minImageExtent.width,
					capabilities.maxImageExtent.width);
			actualExtent.height =
				std::clamp(actualExtent.height, capabilities.minImageExtent.height,
					capabilities.maxImageExtent.height);

			extent = actualExtent;
		}
		// Defense in depth against a 0x0 extent (minimized window whose
		// surface caps zero out between the WaitForNonMinimizedExtent gate
		// and this query, or platforms that report 0 without the sentinel):
		// a 0-sized swapchain/depth image is illegal (the driver returns
		// VK_ERROR_INITIALIZATION_FAILED) - 1x1 keeps it merely useless and
		// the next resize/OUT_OF_DATE triggers Recreate to the real size.
		if (extent.width == 0) extent.width = 1;
		if (extent.height == 0) extent.height = 1;
		return extent;
	}
	std::vector<const char*> getRequiredExtensionsForSDL(bool enableValidationLayers) {
		uint32_t sdlExtensionCount = 0;
		char const* const* sdlExtensions =
			SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);

		std::vector<const char*> extensions;
		if (sdlExtensions && sdlExtensionCount > 0) {
			extensions.assign(sdlExtensions,
				sdlExtensions + sdlExtensionCount);
		}

		if (enableValidationLayers) {
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}

		return extensions;
	}


} // static ns

//VkInstance smvk::VulkanCore::GetInstance() { return instance_; }
//VkDevice smvk::VulkanCore::GetDevice() { return device_; }
//VkPhysicalDevice smvk::VulkanCore::GetPhysicalDevice() { return physicalDevice_; }
//VkQueue smvk::VulkanCore::GetGraphicsQueue() { return graphicsQueue_; }

const char* vkResultToString(VkResult result) {
	switch (result) {
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_NOT_READY: return "VK_NOT_READY";
	case VK_TIMEOUT: return "VK_TIMEOUT";
	case VK_EVENT_SET: return "VK_EVENT_SET";
	case VK_EVENT_RESET: return "VK_EVENT_RESET";
	case VK_INCOMPLETE: return "VK_INCOMPLETE";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
	case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
	case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
	default: return "VK_ERROR_<unrecognized>";
	}
}

// RHI to vulkan
constexpr VkBufferUsageFlags bufferTypeToUsageFlags[9] = {
	VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Index (addressable: GPU-driven filter/index reads via BDA)
	VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Vertex (addressable: filter CS reads the static pos heap via BDA; null BDA = MMU fault + device lost)
	VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Uniform
	VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, // BufferType_Storage (also hosts GeometryFX-style filtered index lists + compute-written debug line streams — VERTEX so those bind as vertex buffers)
	VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Indirect (also written by filter compute)
	VK_BUFFER_USAGE_TRANSFER_SRC_BIT,              // BufferType_TransferSrc
	VK_BUFFER_USAGE_TRANSFER_DST_BIT,              // BufferType_TransferDst
	VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, // BufferType_Staging: host-visible scratch, both directions (uploads read from it, readback copies into it)
	0,                                             // BufferType_Count sentinel
};

constexpr VmaMemoryUsage bufferTypeToVmaUsage[9] = {
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Index: device-local only
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Vertex: device-local only
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Uniform: device-local only (with SHADER_DEVICE_ADDRESS_BIT)
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Storage: device-local only (with SHADER_DEVICE_ADDRESS_BIT)
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Indirect: device-local only
	VMA_MEMORY_USAGE_AUTO,               // BufferType_TransferSrc: auto-detect memory properties
	VMA_MEMORY_USAGE_AUTO,               // BufferType_TransferDst: auto-detect memory properties
	VMA_MEMORY_USAGE_AUTO_PREFER_HOST, // BufferType_Staging: auto-prefer host for CPU mapping
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Count sentinel
};

constexpr VkImageUsageFlags     smvk::toVkImageUsageFlags(ImageUsage usage)
{
	VkImageUsageFlags flags = 0;

	const auto value = static_cast<uint32_t>(usage);

	if (value & static_cast<uint32_t>(ImageUsage::TransferSrc))
		flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	if (value & static_cast<uint32_t>(ImageUsage::TransferDst))
		flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	if (value & static_cast<uint32_t>(ImageUsage::Sampled))
		flags |= VK_IMAGE_USAGE_SAMPLED_BIT;

	if (value & static_cast<uint32_t>(ImageUsage::Storage))
		flags |= VK_IMAGE_USAGE_STORAGE_BIT;

	if (value & static_cast<uint32_t>(ImageUsage::ColorAttachment))
		flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	if (value & static_cast<uint32_t>(ImageUsage::DepthStencilAttachment))
		flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	if (value & static_cast<uint32_t>(ImageUsage::InputAttachment))
		flags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

	return flags;
}
constexpr VkImageType           smvk::toVkImageType(ImageType type)
{
	switch (type)
	{
	case ImageType::Image1D: return VK_IMAGE_TYPE_1D;
	case ImageType::Image2D: return VK_IMAGE_TYPE_2D;
	case ImageType::Image3D: return VK_IMAGE_TYPE_3D;
	}

	return VK_IMAGE_TYPE_2D; // unreachable for valid ImageType values
}
constexpr VkImageViewType       smvk::toVkImageViewType(ImageViewType viewType)
{
	switch (viewType)
	{
	case ImageViewType::View1D:
		return VK_IMAGE_VIEW_TYPE_1D;
	case ImageViewType::View2D:
		return VK_IMAGE_VIEW_TYPE_2D;
	case ImageViewType::View3D:
		return VK_IMAGE_VIEW_TYPE_3D;
	case ImageViewType::ViewCube:
		return VK_IMAGE_VIEW_TYPE_CUBE;
	case ImageViewType::View1DArray:
		return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
	case ImageViewType::View2DArray:
		return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	case ImageViewType::ViewCubeArray:
		return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
	default:
		assert(false && "Unsupported ImageViewType");
		return VK_IMAGE_VIEW_TYPE_2D;
	}
}
constexpr VkClearValue          smvk::toVkClearValue(ClearColor color)
{
	VkClearValue result{};
	result.color.float32[0] = color.r;
	result.color.float32[1] = color.g;
	result.color.float32[2] = color.b;
	result.color.float32[3] = color.a;
	return result;
}
constexpr VkClearValue          smvk::toVkClearValue(ClearDepthStencil clearDepth)
{
	VkClearValue result{};
	result.depthStencil = {
		clearDepth.depth,
		clearDepth.stencil
	};
	return result;
}
constexpr VkAttachmentLoadOp    smvk::toVkAttachmentLoadOp(AttachmentLoadOp op)
{
	switch (op)
	{
	case AttachmentLoadOp::Load:
		return VK_ATTACHMENT_LOAD_OP_LOAD;

	case AttachmentLoadOp::Clear:
		return VK_ATTACHMENT_LOAD_OP_CLEAR;

	case AttachmentLoadOp::DontCare:
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	}

	assert(false && "Unsupported AttachmentLoadOp");
	return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}
constexpr VkAttachmentStoreOp   smvk::toVkAttachmentStoreOp(AttachmentStoreOp op)
{
	switch (op)
	{
	case AttachmentStoreOp::Store:
		return VK_ATTACHMENT_STORE_OP_STORE;

	case AttachmentStoreOp::DontCare:
		return VK_ATTACHMENT_STORE_OP_DONT_CARE;
	}

	assert(false && "Unsupported AttachmentStoreOp");
	return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}
constexpr VkStencilOp           smvk::toVkStencilOp(uint8_t op)
{
	switch (op) {
	case STENCIL_OP_KEEP:                return VK_STENCIL_OP_KEEP;
	case STENCIL_OP_ZERO:                return VK_STENCIL_OP_ZERO;
	case STENCIL_OP_REPLACE:             return VK_STENCIL_OP_REPLACE;
	case STENCIL_OP_INCREMENT_AND_CLAMP: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	case STENCIL_OP_DECREMENT_AND_CLAMP: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
	case STENCIL_OP_INVERT:              return VK_STENCIL_OP_INVERT;
	case STENCIL_OP_INCREMENT_AND_WRAP:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
	case STENCIL_OP_DECREMENT_AND_WRAP:  return VK_STENCIL_OP_DECREMENT_AND_WRAP;
	default:
		assert(false && "Unsupported StencilOp");
		return VK_STENCIL_OP_KEEP;
	}
}
constexpr VkShaderStageFlagBits smvk::toVkShaderStageBit(ShaderStage stage)
{
	assert(
		stage != ShaderStage::None &&
		stage != ShaderStage::AllGraphics &&
		stage != ShaderStage::All
	);

	return static_cast<VkShaderStageFlagBits>(
		static_cast<uint32_t>(stage)
		);
}
constexpr VkShaderStageFlags    smvk::toVkShaderStageFlags(ShaderStage stages)
{
	return static_cast<VkShaderStageFlags>(
		static_cast<uint32_t>(stages)
		);
}
constexpr VkBlendFactor         smvk::toVkSrcBlendFactor(BlendAttachmentState::SrcColorBlendFactor factor)
{
	using Factor =
		BlendAttachmentState::SrcColorBlendFactor;

	switch (factor)
	{
	case Factor::Zero:
		return VK_BLEND_FACTOR_ZERO;

	case Factor::One:
		return VK_BLEND_FACTOR_ONE;

	case Factor::SrcAlpha:
		return VK_BLEND_FACTOR_SRC_ALPHA;

	case Factor::OneMinusSrcAlpha:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	}

	assert(false);
	return VK_BLEND_FACTOR_ONE;
}
constexpr VkBlendFactor         smvk::toVkDstBlendFactor(BlendAttachmentState::DstColorBlendFactor factor)
{
	using Factor =
		BlendAttachmentState::DstColorBlendFactor;

	switch (factor)
	{
	case Factor::Zero:
		return VK_BLEND_FACTOR_ZERO;

	case Factor::One:
		return VK_BLEND_FACTOR_ONE;

	case Factor::OneMinusSrcAlpha:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	}

	assert(false);
	return VK_BLEND_FACTOR_ONE;
}
constexpr VkBlendOp             smvk::toVkBlendOp(BlendAttachmentState::ColorBlendOp op)
{
	using Op =
		BlendAttachmentState::ColorBlendOp;

	switch (op)
	{
	case Op::Add:
		return VK_BLEND_OP_ADD;

	case Op::Subtract:
		return VK_BLEND_OP_SUBTRACT;

	case Op::ReverseSubtract:
		return VK_BLEND_OP_REVERSE_SUBTRACT;

	case Op::Min:
		return VK_BLEND_OP_MIN;

	case Op::Max:
		return VK_BLEND_OP_MAX;
	}

	assert(false);
	return VK_BLEND_OP_ADD;
}
constexpr VkBlendOp             smvk::toVkBlendOp(BlendAttachmentState::AlphaBlendOp op)
{
	using Op =
		BlendAttachmentState::AlphaBlendOp;

	switch (op)
	{
	case Op::Add:
		return VK_BLEND_OP_ADD;

	case Op::Subtract:
		return VK_BLEND_OP_SUBTRACT;

	case Op::ReverseSubtract:
		return VK_BLEND_OP_REVERSE_SUBTRACT;

	case Op::Min:
		return VK_BLEND_OP_MIN;

	case Op::Max:
		return VK_BLEND_OP_MAX;
	}

	assert(false);
	return VK_BLEND_OP_ADD;
}
constexpr VkColorComponentFlags smvk::toVkColorWriteMask(uint8_t mask)
{
	using Flag =
		BlendAttachmentState::ColorComponentFlag;

	VkColorComponentFlags result = 0;

	if (mask & static_cast<uint8_t>(Flag::R))
		result |= VK_COLOR_COMPONENT_R_BIT;

	if (mask & static_cast<uint8_t>(Flag::G))
		result |= VK_COLOR_COMPONENT_G_BIT;

	if (mask & static_cast<uint8_t>(Flag::B))
		result |= VK_COLOR_COMPONENT_B_BIT;

	if (mask & static_cast<uint8_t>(Flag::A))
		result |= VK_COLOR_COMPONENT_A_BIT;

	return result;
}
constexpr VkPrimitiveTopology   smvk::toVkPrimitiveTopology(PrimitiveTopology topology)
{
	switch (topology)
	{
	case PrimitiveTopology::PointList:
		return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

	case PrimitiveTopology::LineList:
		return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

	case PrimitiveTopology::LineStrip:
		return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;

	case PrimitiveTopology::TriangleList:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	case PrimitiveTopology::TriangleStrip:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

	case PrimitiveTopology::TriangleFan:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	}

	assert(false);
	return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}
constexpr VkPolygonMode         smvk::toVkPolygonMode(PolygonMode mode)
{
	switch (mode)
	{
	case PolygonMode::Fill:
		return VK_POLYGON_MODE_FILL;

	case PolygonMode::Line:
		return VK_POLYGON_MODE_LINE;

	case PolygonMode::Point:
		return VK_POLYGON_MODE_POINT;
	}

	assert(false);
	return VK_POLYGON_MODE_FILL;
}
constexpr VkCullModeFlags       smvk::toVkCullMode(CullMode mode)
{
	switch (mode)
	{
	case CullMode::None:
		return VK_CULL_MODE_NONE;

	case CullMode::Front:
		return VK_CULL_MODE_FRONT_BIT;

	case CullMode::Back:
		return VK_CULL_MODE_BACK_BIT;

	case CullMode::FrontAndBack:
		return VK_CULL_MODE_FRONT_AND_BACK;
	}

	assert(false);
	return VK_CULL_MODE_NONE;
}
constexpr VkFrontFace           smvk::toVkFrontFace(FrontFace face)
{
	switch (face)
	{
	case FrontFace::CounterClockwise:
		return VK_FRONT_FACE_COUNTER_CLOCKWISE;

	case FrontFace::Clockwise:
		return VK_FRONT_FACE_CLOCKWISE;
	}

	assert(false);
	return VK_FRONT_FACE_COUNTER_CLOCKWISE;
}
constexpr VkSampleCountFlagBits smvk::toVkSampleCount(SampleCount samples)
{
	switch (samples)
	{
	case SampleCount::Count1:
		return VK_SAMPLE_COUNT_1_BIT;

	case SampleCount::Count2:
		return VK_SAMPLE_COUNT_2_BIT;

	case SampleCount::Count4:
		return VK_SAMPLE_COUNT_4_BIT;

	case SampleCount::Count8:
		return VK_SAMPLE_COUNT_8_BIT;

	case SampleCount::Count16:
		return VK_SAMPLE_COUNT_16_BIT;

	case SampleCount::Count32:
		return VK_SAMPLE_COUNT_32_BIT;

	case SampleCount::Count64:
		return VK_SAMPLE_COUNT_64_BIT;
	}

	assert(false);
	return VK_SAMPLE_COUNT_1_BIT;
}
constexpr VkCompareOp           smvk::toVkCompareOp(smvk::CompareOp op)
{
	switch (op)
	{
	case smvk::CompareOp::Never:        return VK_COMPARE_OP_NEVER;
	case smvk::CompareOp::Less:         return VK_COMPARE_OP_LESS;
	case smvk::CompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
	case smvk::CompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
	case smvk::CompareOp::Greater:      return VK_COMPARE_OP_GREATER;
	case smvk::CompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
	case smvk::CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
	case smvk::CompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
	default:                            return VK_COMPARE_OP_ALWAYS;
	}
}
constexpr VkFormat              smvk::toVkFormat(smvk::Format format)
{
	switch (format)
	{
	case smvk::Format::Undefined:           return VK_FORMAT_UNDEFINED;

		// 8-bit
	case smvk::Format::R8_UNorm:            return VK_FORMAT_R8_UNORM;
	case smvk::Format::R8_SNorm:            return VK_FORMAT_R8_SNORM;
	case smvk::Format::R8_UInt:             return VK_FORMAT_R8_UINT;
	case smvk::Format::R8_SInt:             return VK_FORMAT_R8_SINT;

	case smvk::Format::RG8_UNorm:           return VK_FORMAT_R8G8_UNORM;
	case smvk::Format::RG8_SNorm:           return VK_FORMAT_R8G8_SNORM;
	case smvk::Format::RG8_UInt:            return VK_FORMAT_R8G8_UINT;
	case smvk::Format::RG8_SInt:            return VK_FORMAT_R8G8_SINT;

	case smvk::Format::RGBA8_UNorm:         return VK_FORMAT_R8G8B8A8_UNORM;
	case smvk::Format::RGBA8_SNorm:         return VK_FORMAT_R8G8B8A8_SNORM;
	case smvk::Format::RGBA8_UInt:          return VK_FORMAT_R8G8B8A8_UINT;
	case smvk::Format::RGBA8_SInt:          return VK_FORMAT_R8G8B8A8_SINT;
	case smvk::Format::RGBA8_SRGB:          return VK_FORMAT_R8G8B8A8_SRGB;

	case smvk::Format::BGRA8_UNorm:         return VK_FORMAT_B8G8R8A8_UNORM;
	case smvk::Format::BGRA8_SRGB:          return VK_FORMAT_B8G8R8A8_SRGB;

		// 16-bit
	case smvk::Format::R16_UNorm:           return VK_FORMAT_R16_UNORM;
	case smvk::Format::R16_SNorm:           return VK_FORMAT_R16_SNORM;
	case smvk::Format::R16_UInt:            return VK_FORMAT_R16_UINT;
	case smvk::Format::R16_SInt:            return VK_FORMAT_R16_SINT;
	case smvk::Format::R16_Float:           return VK_FORMAT_R16_SFLOAT;

	case smvk::Format::RG16_UNorm:          return VK_FORMAT_R16G16_UNORM;
	case smvk::Format::RG16_SNorm:          return VK_FORMAT_R16G16_SNORM;
	case smvk::Format::RG16_UInt:           return VK_FORMAT_R16G16_UINT;
	case smvk::Format::RG16_SInt:           return VK_FORMAT_R16G16_SINT;
	case smvk::Format::RG16_Float:          return VK_FORMAT_R16G16_SFLOAT;

	case smvk::Format::RGBA16_UNorm:        return VK_FORMAT_R16G16B16A16_UNORM;
	case smvk::Format::RGBA16_SNorm:        return VK_FORMAT_R16G16B16A16_SNORM;
	case smvk::Format::RGBA16_UInt:         return VK_FORMAT_R16G16B16A16_UINT;
	case smvk::Format::RGBA16_SInt:         return VK_FORMAT_R16G16B16A16_SINT;
	case smvk::Format::RGBA16_Float:        return VK_FORMAT_R16G16B16A16_SFLOAT;

		// 32-bit
	case smvk::Format::R32_UInt:            return VK_FORMAT_R32_UINT;
	case smvk::Format::R32_SInt:            return VK_FORMAT_R32_SINT;
	case smvk::Format::R32_Float:           return VK_FORMAT_R32_SFLOAT;

	case smvk::Format::RG32_UInt:           return VK_FORMAT_R32G32_UINT;
	case smvk::Format::RG32_SInt:           return VK_FORMAT_R32G32_SINT;
	case smvk::Format::RG32_Float:          return VK_FORMAT_R32G32_SFLOAT;

	case smvk::Format::RGB32_UInt:          return VK_FORMAT_R32G32B32_UINT;
	case smvk::Format::RGB32_SInt:          return VK_FORMAT_R32G32B32_SINT;
	case smvk::Format::RGB32_Float:         return VK_FORMAT_R32G32B32_SFLOAT;

	case smvk::Format::RGBA32_UInt:         return VK_FORMAT_R32G32B32A32_UINT;
	case smvk::Format::RGBA32_SInt:         return VK_FORMAT_R32G32B32A32_SINT;
	case smvk::Format::RGBA32_Float:        return VK_FORMAT_R32G32B32A32_SFLOAT;

		// packed
	case smvk::Format::RGB10A2_UNorm:       return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	case smvk::Format::RGB10A2_UInt:        return VK_FORMAT_A2B10G10R10_UINT_PACK32;
	case smvk::Format::R11G11B10_Float:     return VK_FORMAT_B10G11R11_UFLOAT_PACK32;

		// depth / stencil
	case smvk::Format::D16_UNorm:           return VK_FORMAT_D16_UNORM;
	case smvk::Format::D24_UNorm_S8_UInt:   return VK_FORMAT_D24_UNORM_S8_UINT;
	case smvk::Format::D32_Float:           return VK_FORMAT_D32_SFLOAT;
	case smvk::Format::D32_Float_S8_UInt:   return VK_FORMAT_D32_SFLOAT_S8_UINT;
		// Stencil-only view of a combined depth/stencil image.
	case smvk::Format::S8_UInt:             return VK_FORMAT_S8_UINT;

		// compressed
	case smvk::Format::BC1_RGB_UNorm:       return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
	case smvk::Format::BC1_RGB_SRGB:        return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
	case smvk::Format::BC1_RGBA_UNorm:      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case smvk::Format::BC1_RGBA_SRGB:       return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
	case smvk::Format::BC2_UNorm:           return VK_FORMAT_BC2_UNORM_BLOCK;
	case smvk::Format::BC2_SRGB:            return VK_FORMAT_BC2_SRGB_BLOCK;
	case smvk::Format::BC3_UNorm:           return VK_FORMAT_BC3_UNORM_BLOCK;
	case smvk::Format::BC3_SRGB:            return VK_FORMAT_BC3_SRGB_BLOCK;
	case smvk::Format::BC4_UNorm:           return VK_FORMAT_BC4_UNORM_BLOCK;
	case smvk::Format::BC4_SNorm:           return VK_FORMAT_BC4_SNORM_BLOCK;
	case smvk::Format::BC5_UNorm:           return VK_FORMAT_BC5_UNORM_BLOCK;
	case smvk::Format::BC5_SNorm:           return VK_FORMAT_BC5_SNORM_BLOCK;
	case smvk::Format::BC6H_UFloat:         return VK_FORMAT_BC6H_UFLOAT_BLOCK;
	case smvk::Format::BC6H_SFloat:         return VK_FORMAT_BC6H_SFLOAT_BLOCK;
	case smvk::Format::BC7_UNorm:           return VK_FORMAT_BC7_UNORM_BLOCK;
	case smvk::Format::BC7_SRGB:            return VK_FORMAT_BC7_SRGB_BLOCK;
	}

	return VK_FORMAT_UNDEFINED;
}
constexpr VkBlendFactor         smvk::toVkBlendFactor(smvk::BlendAttachmentState::SrcColorBlendFactor factor)
{
	switch (factor)
	{
	case smvk::BlendAttachmentState::SrcColorBlendFactor::Zero:       return VK_BLEND_FACTOR_ZERO;
	case smvk::BlendAttachmentState::SrcColorBlendFactor::One:        return VK_BLEND_FACTOR_ONE;
	case smvk::BlendAttachmentState::SrcColorBlendFactor::SrcAlpha:   return VK_BLEND_FACTOR_SRC_ALPHA;
	case smvk::BlendAttachmentState::SrcColorBlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	default: return VK_BLEND_FACTOR_ONE;
	}
}
constexpr VkBlendFactor         smvk::toVkBlendFactor(smvk::BlendAttachmentState::DstColorBlendFactor factor)
{
	switch (factor)
	{
	case smvk::BlendAttachmentState::DstColorBlendFactor::Zero:       return VK_BLEND_FACTOR_ZERO;
	case smvk::BlendAttachmentState::DstColorBlendFactor::One:        return VK_BLEND_FACTOR_ONE;
	case smvk::BlendAttachmentState::DstColorBlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	default: return VK_BLEND_FACTOR_ONE;
	}
}
constexpr uint8_t               smvk::toRhiColorWriteMask(VkColorComponentFlags flags)
{
	uint8_t mask = 0;
	if (flags & VK_COLOR_COMPONENT_R_BIT) mask |= static_cast<uint8_t>(smvk::BlendAttachmentState::ColorComponentFlag::R);
	if (flags & VK_COLOR_COMPONENT_G_BIT) mask |= static_cast<uint8_t>(smvk::BlendAttachmentState::ColorComponentFlag::G);
	if (flags & VK_COLOR_COMPONENT_B_BIT) mask |= static_cast<uint8_t>(smvk::BlendAttachmentState::ColorComponentFlag::B);
	if (flags & VK_COLOR_COMPONENT_A_BIT) mask |= static_cast<uint8_t>(smvk::BlendAttachmentState::ColorComponentFlag::A);
	return mask;
}
constexpr VkColorSpaceKHR       smvk::ToVkColorSpace(ColorSpace colorSpace)
{
	switch (colorSpace)
	{
	case smvk::ColorSpace::SRGB_Nonlinear:
		return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

	case smvk::ColorSpace::DisplayP3_Nonlinear:
		return VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT;

	case smvk::ColorSpace::DisplayP3_Linear:
		return VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT;

	case smvk::ColorSpace::ExtendedSRGB_Linear:
		return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;

	case smvk::ColorSpace::ExtendedSRGB_Nonlinear:
		return VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT;

	case smvk::ColorSpace::DCI_P3_Nonlinear:
		return VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT;

	case smvk::ColorSpace::BT709_Linear:
		return VK_COLOR_SPACE_BT709_LINEAR_EXT;

	case smvk::ColorSpace::BT709_Nonlinear:
		return VK_COLOR_SPACE_BT709_NONLINEAR_EXT;

	case smvk::ColorSpace::BT2020_Linear:
		return VK_COLOR_SPACE_BT2020_LINEAR_EXT;

	case smvk::ColorSpace::HDR10_ST2084:
		return VK_COLOR_SPACE_HDR10_ST2084_EXT;

	case smvk::ColorSpace::HDR10_HLG:
		return VK_COLOR_SPACE_HDR10_HLG_EXT;

	case smvk::ColorSpace::DolbyVision:
		return VK_COLOR_SPACE_DOLBYVISION_EXT;

	case smvk::ColorSpace::AdobeRGB_Linear:
		return VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT;

	case smvk::ColorSpace::AdobeRGB_Nonlinear:
		return VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT;

	case smvk::ColorSpace::PassThrough:
		return VK_COLOR_SPACE_PASS_THROUGH_EXT;

	default:
		return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	}
}
constexpr VkImageAspectFlags    smvk::toVkImageAspectFlags(ImageAspect aspect)
{
	VkImageAspectFlags flags = 0;

	if ((aspect & ImageAspect::Color) != ImageAspect::None)
		flags |= VK_IMAGE_ASPECT_COLOR_BIT;

	if ((aspect & ImageAspect::Depth) != ImageAspect::None)
		flags |= VK_IMAGE_ASPECT_DEPTH_BIT;

	if ((aspect & ImageAspect::Stencil) != ImageAspect::None)
		flags |= VK_IMAGE_ASPECT_STENCIL_BIT;

	return flags;
}
static constexpr                smvk::Format fromVkFormat(VkFormat format) // dont like to have this reverse mapping too. choose swapsurface and depthFormat needs refacotring
{
	switch (format)
	{
	case VK_FORMAT_R8_UNORM:                 return smvk::Format::R8_UNorm;
	case VK_FORMAT_R8_SNORM:                 return smvk::Format::R8_SNorm;
	case VK_FORMAT_R8_UINT:                  return smvk::Format::R8_UInt;
	case VK_FORMAT_R8_SINT:                  return smvk::Format::R8_SInt;

	case VK_FORMAT_R8G8_UNORM:               return smvk::Format::RG8_UNorm;
	case VK_FORMAT_R8G8_SNORM:               return smvk::Format::RG8_SNorm;
	case VK_FORMAT_R8G8_UINT:                return smvk::Format::RG8_UInt;
	case VK_FORMAT_R8G8_SINT:                return smvk::Format::RG8_SInt;

	case VK_FORMAT_R8G8B8A8_UNORM:           return smvk::Format::RGBA8_UNorm;
	case VK_FORMAT_R8G8B8A8_SNORM:           return smvk::Format::RGBA8_SNorm;
	case VK_FORMAT_R8G8B8A8_UINT:            return smvk::Format::RGBA8_UInt;
	case VK_FORMAT_R8G8B8A8_SINT:            return smvk::Format::RGBA8_SInt;
	case VK_FORMAT_R8G8B8A8_SRGB:            return smvk::Format::RGBA8_SRGB;

	case VK_FORMAT_B8G8R8A8_UNORM:           return smvk::Format::BGRA8_UNorm;
	case VK_FORMAT_B8G8R8A8_SRGB:            return smvk::Format::BGRA8_SRGB;

	case VK_FORMAT_R16_UNORM:                return smvk::Format::R16_UNorm;
	case VK_FORMAT_R16_SNORM:                return smvk::Format::R16_SNorm;
	case VK_FORMAT_R16_UINT:                 return smvk::Format::R16_UInt;
	case VK_FORMAT_R16_SINT:                 return smvk::Format::R16_SInt;
	case VK_FORMAT_R16_SFLOAT:               return smvk::Format::R16_Float;

	case VK_FORMAT_R16G16_UNORM:             return smvk::Format::RG16_UNorm;
	case VK_FORMAT_R16G16_SNORM:             return smvk::Format::RG16_SNorm;
	case VK_FORMAT_R16G16_UINT:              return smvk::Format::RG16_UInt;
	case VK_FORMAT_R16G16_SINT:              return smvk::Format::RG16_SInt;
	case VK_FORMAT_R16G16_SFLOAT:            return smvk::Format::RG16_Float;

	case VK_FORMAT_R16G16B16A16_UNORM:       return smvk::Format::RGBA16_UNorm;
	case VK_FORMAT_R16G16B16A16_SNORM:       return smvk::Format::RGBA16_SNorm;
	case VK_FORMAT_R16G16B16A16_UINT:        return smvk::Format::RGBA16_UInt;
	case VK_FORMAT_R16G16B16A16_SINT:        return smvk::Format::RGBA16_SInt;
	case VK_FORMAT_R16G16B16A16_SFLOAT:      return smvk::Format::RGBA16_Float;

	case VK_FORMAT_R32_UINT:                 return smvk::Format::R32_UInt;
	case VK_FORMAT_R32_SINT:                 return smvk::Format::R32_SInt;
	case VK_FORMAT_R32_SFLOAT:               return smvk::Format::R32_Float;

	case VK_FORMAT_R32G32_UINT:              return smvk::Format::RG32_UInt;
	case VK_FORMAT_R32G32_SINT:              return smvk::Format::RG32_SInt;
	case VK_FORMAT_R32G32_SFLOAT:            return smvk::Format::RG32_Float;

	case VK_FORMAT_R32G32B32_UINT:           return smvk::Format::RGB32_UInt;
	case VK_FORMAT_R32G32B32_SINT:           return smvk::Format::RGB32_SInt;
	case VK_FORMAT_R32G32B32_SFLOAT:         return smvk::Format::RGB32_Float;

	case VK_FORMAT_R32G32B32A32_UINT:        return smvk::Format::RGBA32_UInt;
	case VK_FORMAT_R32G32B32A32_SINT:        return smvk::Format::RGBA32_SInt;
	case VK_FORMAT_R32G32B32A32_SFLOAT:      return smvk::Format::RGBA32_Float;

	case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return smvk::Format::RGB10A2_UNorm;
	case VK_FORMAT_A2R10G10B10_UINT_PACK32:  return smvk::Format::RGB10A2_UInt;

	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:  return smvk::Format::R11G11B10_Float;

	case VK_FORMAT_D16_UNORM:                return smvk::Format::D16_UNorm;
	case VK_FORMAT_D24_UNORM_S8_UINT:        return smvk::Format::D24_UNorm_S8_UInt;
	case VK_FORMAT_D32_SFLOAT:               return smvk::Format::D32_Float;
	case VK_FORMAT_D32_SFLOAT_S8_UINT:       return smvk::Format::D32_Float_S8_UInt;
	case VK_FORMAT_S8_UINT:                  return smvk::Format::S8_UInt;

		// BC1_RGB is what libktx's KTX_TTF_BC1_RGB transcode emits (opaque-only);
		// BC1_RGBA is the 1-bit punchthrough-alpha variant. Distinct VkFormats
		// require distinct image/view formats, so keep them distinct here too.
	case VK_FORMAT_BC1_RGB_UNORM_BLOCK:        return smvk::Format::BC1_RGB_UNorm;
	case VK_FORMAT_BC1_RGB_SRGB_BLOCK:         return smvk::Format::BC1_RGB_SRGB;
	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:      return smvk::Format::BC1_RGBA_UNorm;
	case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:       return smvk::Format::BC1_RGBA_SRGB;
	case VK_FORMAT_BC2_UNORM_BLOCK:          return smvk::Format::BC2_UNorm;
	case VK_FORMAT_BC2_SRGB_BLOCK:           return smvk::Format::BC2_SRGB;
	case VK_FORMAT_BC3_UNORM_BLOCK:          return smvk::Format::BC3_UNorm;
	case VK_FORMAT_BC3_SRGB_BLOCK:           return smvk::Format::BC3_SRGB;
	case VK_FORMAT_BC4_UNORM_BLOCK:          return smvk::Format::BC4_UNorm;
	case VK_FORMAT_BC4_SNORM_BLOCK:          return smvk::Format::BC4_SNorm;
	case VK_FORMAT_BC5_UNORM_BLOCK:          return smvk::Format::BC5_UNorm;
	case VK_FORMAT_BC5_SNORM_BLOCK:          return smvk::Format::BC5_SNorm;
	case VK_FORMAT_BC6H_UFLOAT_BLOCK:        return smvk::Format::BC6H_UFloat;
	case VK_FORMAT_BC6H_SFLOAT_BLOCK:        return smvk::Format::BC6H_SFloat;
	case VK_FORMAT_BC7_UNORM_BLOCK:          return smvk::Format::BC7_UNorm;
	case VK_FORMAT_BC7_SRGB_BLOCK:           return smvk::Format::BC7_SRGB;

	default:                                  return smvk::Format::Undefined;
	}
}


// Helpers
static constexpr bool                     FormatHasStencil(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_S8_UINT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;

	default:
		return false;
	}
}
static constexpr bool                     FormatHasDepth(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;

	default:
		return false;
	}
}
static constexpr VkImageAspectFlags       VkAspectMaskFromFormat(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
		return VK_IMAGE_ASPECT_DEPTH_BIT;

	case VK_FORMAT_S8_UINT:
		return VK_IMAGE_ASPECT_STENCIL_BIT;

	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

	default:
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}
static constexpr VkDescriptorSetLayoutBinding       getDSLBinding(uint32_t binding,
	VkDescriptorType descriptorType,
	uint32_t descriptorCount,
	VkShaderStageFlags stageFlags,
	const VkSampler* immutableSamplers = nullptr
) {
	return VkDescriptorSetLayoutBinding{
		.binding = binding,
		.descriptorType = descriptorType,
		.descriptorCount = descriptorCount,
		.stageFlags = stageFlags,
		.pImmutableSamplers = immutableSamplers,
	};
}
[[nodiscard]] smvk::ImageAspect           DefaultSampleAspect(smvk::Format format)
{
	const VkImageAspectFlags aspects =
		VkAspectMaskFromFormat(toVkFormat(format));
	// For depth/stencil images, sampled views should normally expose depth only.
	if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
	{
		return smvk::ImageAspect::Depth;
	}
	return smvk::ImageAspect::Color;
}
//


// ---------------------------------------------------------------------------
// Sync helpers – Vk <-> RHI conversions (kept internal for threading safety)
// ---------------------------------------------------------------------------
static smvk::ImageLayout FromVkImageLayout(VkImageLayout l) {
	switch (l) {
	case VK_IMAGE_LAYOUT_UNDEFINED: return smvk::ImageLayout::Undefined;
	case VK_IMAGE_LAYOUT_GENERAL: return smvk::ImageLayout::General;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return smvk::ImageLayout::ColorAttachmentOptimal;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return smvk::ImageLayout::DepthStencilAttachmentOptimal;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return smvk::ImageLayout::DepthStencilReadOnlyOptimal;
	case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL: return smvk::ImageLayout::DepthReadOnlyOptimal;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return smvk::ImageLayout::ShaderReadOnlyOptimal;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return smvk::ImageLayout::TransferSrcOptimal;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return smvk::ImageLayout::TransferDstOptimal;
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return smvk::ImageLayout::PresentSrc;
	default: return smvk::ImageLayout::Undefined;
	}
}
static smvk::PipelineStage FromVkPipelineStage(VkPipelineStageFlags2 flags) {
	if (flags == VK_PIPELINE_STAGE_2_NONE) return smvk::PipelineStageBit_None;
	smvk::PipelineStage out = smvk::PipelineStageBit_None;
	if (flags & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Vertex);
	// Vertex covers vertex-input fetch (see ToVkStage): fold both input
	// bits back so stage masks round-trip instead of collapsing to None.
	if (flags & (VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT)) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Vertex);
	if (flags & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Fragment);
	if (flags & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Compute);
	if (flags & VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Geometry);
	if (flags & VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_TessellationControl);
	if (flags & VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_TessellationEvaluation);
	if (flags & VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Task);
	if (flags & VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Mesh);
	if (flags & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR)
		out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_RayGeneration | smvk::PipelineStageBit_AnyHit | smvk::PipelineStageBit_ClosestHit | smvk::PipelineStageBit_Miss | smvk::PipelineStageBit_Intersection | smvk::PipelineStageBit_Callable);
	if (flags & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Fragment);
	if (flags & (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Fragment);
	if (flags & VK_PIPELINE_STAGE_2_TRANSFER_BIT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_Transfer);
	if (flags & VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT) out = static_cast<smvk::PipelineStage>(out | smvk::PipelineStageBit_DrawIndirect);
	return out;
}
static smvk::AccessFlags FromVkAccess(VkAccessFlags2 flags) {
	return static_cast<smvk::AccessFlags>(static_cast<uint32_t>(flags));
}
static VkImageLayout ToVkImageLayout(smvk::ImageLayout layout) {
	switch (layout) {
	case smvk::ImageLayout::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
	case smvk::ImageLayout::General: return VK_IMAGE_LAYOUT_GENERAL;
	case smvk::ImageLayout::ColorAttachmentOptimal: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	case smvk::ImageLayout::DepthStencilAttachmentOptimal: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	case smvk::ImageLayout::DepthStencilReadOnlyOptimal: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	case smvk::ImageLayout::DepthReadOnlyOptimal: return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
	case smvk::ImageLayout::ShaderReadOnlyOptimal: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	case smvk::ImageLayout::TransferSrcOptimal: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	case smvk::ImageLayout::TransferDstOptimal: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	case smvk::ImageLayout::PresentSrc: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	default: return VK_IMAGE_LAYOUT_UNDEFINED;
	}
}
static VkPipelineStageFlags2 ToVkStage(smvk::PipelineStage stage) {
	if (stage == smvk::PipelineStageBit_None) return VK_PIPELINE_STAGE_2_NONE;
	VkPipelineStageFlags2 out = 0;
	// Vertex covers vertex-input fetch as well as the shader stage: the
	// RHI has no separate input bit, and VERTEX_ATTRIBUTE_READ is only
	// valid with VERTEX_INPUT/ALL_GRAPHICS/ALL_COMMANDS in the mask.
	if (stage & smvk::PipelineStageBit_Vertex) out |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
	if (stage & smvk::PipelineStageBit_Fragment) out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	if (stage & smvk::PipelineStageBit_Compute) out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	if (stage & smvk::PipelineStageBit_Geometry) out |= VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
	if (stage & smvk::PipelineStageBit_TessellationControl) out |= VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
	if (stage & smvk::PipelineStageBit_TessellationEvaluation) out |= VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
	if (stage & smvk::PipelineStageBit_Task) out |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
	if (stage & smvk::PipelineStageBit_Mesh) out |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
	if (stage & (smvk::PipelineStageBit_RayGeneration | smvk::PipelineStageBit_AnyHit | smvk::PipelineStageBit_ClosestHit | smvk::PipelineStageBit_Miss | smvk::PipelineStageBit_Intersection | smvk::PipelineStageBit_Callable))
		out |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
	if (stage & smvk::PipelineStageBit_Transfer) out |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	if (stage & smvk::PipelineStageBit_DrawIndirect) out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
	// RHI Fragment maps to color attachment + depth test in Vk; expand for safety
	if (stage & smvk::PipelineStageBit_Fragment) out |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
	return out ? out : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}
static VkAccessFlags2 ToVkAccessLocal(smvk::AccessFlags access) { return static_cast<VkAccessFlags2>(static_cast<uint32_t>(access)); }

// TOURMALINE_HOST_COPY=0: kill switch for VK_EXT_host_image_copy — the
// extension is never enabled on the device AND the upload streamer is kept
// off the host-copy path. RenderDoc's support for the extension is
// incomplete and can crash captures; set this in RenderDoc launch configs.
static bool HostCopyKilledByEnv()
{
	static const bool killed = [] {
		const char* e = std::getenv("TOURMALINE_HOST_COPY");
		return e && *e && std::string_view{ e } == "0";
	}();
	return killed;
}

// TOURMALINE_MESH=0: kill switch for VK_EXT_mesh_shader — the extension
// is never enabled on the device and the mesh-shader gfx filter path
// stays off (compute+indexed CPU fallback). Mirrors HostCopyKilledByEnv.
static bool MeshKilledByEnv()
{
	static const bool killed = [] {
		const char* e = std::getenv("TOURMALINE_MESH");
		return e && *e && std::string_view{ e } == "0";
	}();
	return killed;
}

// Nsight Aftermath shader debug tables (VK_NV_device_diagnostics_config):
// the second half of shader-crash symbolication. DXC's -Zi /
// -fspv-debug=vulkan only embeds the HLSL -> SPIR-V line info in the module
// this tree loads; Aftermath crash dumps additionally need the
// NVIDIA-driver-generated mapping from GPU machine code back to that SPIR-V
// (the .nvdbg line tables). The device-init code below requests exactly
// that when this build's SPIR-V itself carries debug info
// (TOURMALINE_SHADER_DEBUG_INFO: Debug trees + the x64-Release-ShaderDebug
// preset) and the driver exposes the extension.
//   TOURMALINE_AFTERMATH=0  kill switch — skip the driver-side tables (they
//                          cost pipeline-compile time + memory), e.g. when
//                          bisecting a hang that never reaches Aftermath
//   unset                 default: on iff the SPIR-V carries debug info
// No GFSDK_Aftermath_EnableGpuCrashDumps call exists in this codebase (it
// would take .nvdbg writing away from the Aftermath Monitor), so the
// Monitor's "Generate Shader Debug Information" + dump-dir settings stay
// authoritative for writing the .nvdbg files.
static bool AftermathShaderDebugTablesEnabled()
{
	static const bool enabled = [] {
#if defined(TOURMALINE_SHADER_DEBUG_INFO)
		const bool def = true;
#else
		const bool def = false;
#endif
		const char* e = std::getenv("TOURMALINE_AFTERMATH");
		if (e && *e && std::string_view{ e } == "0")
			return false;
		return def;
	}();
	return enabled;
}

// ---------------------------------------------------------------------------
// TOURMALINE_SERIALIZE — submission-level serialization bisect knob
// ---------------------------------------------------------------------------
// Nsight Graphics' "serialize context" forces every GPU workload to
// complete before the next begins; bugs that only reproduce WITHOUT it
// live in submission-level ordering that validation layers cannot model.
// This knob reproduces the same serialization in-process so it can be
// bisected per submission class (vkQueueWaitIdle after each gated submit):
//   TOURMALINE_SERIALIZE=all      every submission class below (Nsight-like)
//   TOURMALINE_SERIALIZE=split    after the G1 and C submits (async split)
//   TOURMALINE_SERIALIZE=main     after the G2/main frame submit
//   TOURMALINE_SERIALIZE=frame    alias for main
//   TOURMALINE_SERIALIZE=uploads  after every Submitter submit (upload
//                                batches, acquire barriers, readback
//                                copies, misc engine submits)
//   unset / 0 / off              no serialization (default)
// Comma lists combine classes ("split,uploads"). If 'all' fixes the image
// but narrower classes do not, the race spans multiple classes; if a
// single class fixes it, the missing sync is in that class's chain.
struct SerializeMode {
	bool all = false, split = false, main = false, uploads = false;
};
static const SerializeMode& RgSerializeMode()
{
	static const SerializeMode m = [] {
		SerializeMode s;
		const char* e = std::getenv("TOURMALINE_SERIALIZE");
		if (!e || !*e)
			return s;
		const std::string_view v{ e };
		if (v == "0" || v == "off")
			return s;
		auto has = [&](std::string_view tok) {
			for (size_t pos = 0; pos <= v.size();) {
				size_t end = v.find(',', pos);
				if (end == std::string_view::npos)
					end = v.size();
				std::string_view t = v.substr(pos, end - pos);
				while (!t.empty() && t.front() == ' ')
					t.remove_prefix(1);
				while (!t.empty() && t.back() == ' ')
					t.remove_suffix(1);
				if (t == tok)
					return true;
				pos = end + 1;
			}
			return false;
		};
		s.all = has("all");
		s.split = s.all || has("split");
		s.main = s.all || has("main") || has("frame");
		s.uploads = s.all || has("uploads");
		return s;
	}();
	return m;
}
// The gate's queue wait holds the queue mutex like every queue call:
// vkQueueWaitIdle is not internally synchronized and can race a
// concurrent Submitter::submit() on the same queue from another thread.
static void RgSerializeGate(bool on, VkQueue queue, std::mutex& queueMutex)
{
	if (!on || queue == VK_NULL_HANDLE)
		return;
	static std::atomic<bool> s_noticed{ false };
	if (!s_noticed.exchange(true))
		std::fprintf(stderr,
			"[Serialize] TOURMALINE_SERIALIZE active: queue idle after gated submits\n");
	std::lock_guard<std::mutex> lock(queueMutex);
	vkQueueWaitIdle(queue);
}

// ---------------------------------------------------------------------------
// RenderGraph barrier debug tracing (TOURMALINE_BARRIER_TRACE)
// ---------------------------------------------------------------------------
// Env knob, evaluated once per process:
//   TOURMALINE_BARRIER_TRACE=0      off (also the default when unset)
//   TOURMALINE_BARRIER_TRACE=on     trace every frame (any non-numeric value)
//   TOURMALINE_BARRIER_TRACE=N      trace only the frame with frameIndex == N
// When enabled, RenderGraph::Execute prints, per pass in SORTED order:
//   - the pass name, its sorted position vs insertion index (exposes
//     topological reordering: "MOVED" marks a pass the sort displaced),
//   - the command-buffer stream it records into (G1/C/G2 when the
//     async-compute split is active, MAIN otherwise),
//   - per hinted image/buffer: the tracked src sync state -> the pass's
//     request and the needBarrier verdict (need=0 lines included — a
//     read whose src layout is still COLOR_ATTACHMENT / DEPTH_ATTACHMENT
//     means no barrier covered the producer's write),
//   - the emitted VkImageMemoryBarrier2 / VkBufferMemoryBarrier2
//     contents, plus the cross-queue rewrite markers on async passes.
// The two loudest bug signatures to grep for:
//   "SKIPPED-BARRIER"  — a hint that resolved to no backing image, so a
//                        needed barrier was silently dropped,
//   "need=0" on a line whose src layout differs from dst — impossible by
//                        construction, so seeing one means state corruption.
static const char* RgTraceImageLayoutName(smvk::ImageLayout l)
{
	switch (l) {
	case smvk::ImageLayout::Undefined: return "UNDEFINED";
	case smvk::ImageLayout::General: return "GENERAL";
	case smvk::ImageLayout::ColorAttachmentOptimal: return "COLOR_ATTACHMENT";
	case smvk::ImageLayout::DepthStencilAttachmentOptimal: return "DEPTH_ATTACHMENT";
	case smvk::ImageLayout::DepthStencilReadOnlyOptimal: return "DEPTH_READ_ONLY";
	case smvk::ImageLayout::DepthReadOnlyOptimal: return "DEPTH_READ_ONLY_STRICT";
	case smvk::ImageLayout::ShaderReadOnlyOptimal: return "SHADER_READ";
	case smvk::ImageLayout::TransferSrcOptimal: return "TRANSFER_SRC";
	case smvk::ImageLayout::TransferDstOptimal: return "TRANSFER_DST";
	case smvk::ImageLayout::PresentSrc: return "PRESENT";
	}
	return "OTHER";
}
static const char* RgTraceVkImageLayoutName(VkImageLayout l)
{
	switch (l) {
	case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
	case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT";
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_ATTACHMENT";
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_READ_ONLY";
	case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL: return "DEPTH_READ_ONLY_STRICT";
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ";
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC";
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST";
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT";
	}
	return "OTHER";
}
static const char* RgTracePassName(const char* n)
{
	return n ? n : "UnnamedRenderPass";
}
// -1 = off, 0 = every frame, N > 0 = only frameIndex N.
static bool RgTraceEnabled(uint32_t frameIndex)
{
	static const int64_t filter = []() -> int64_t {
		const char* e = std::getenv("TOURMALINE_BARRIER_TRACE");
		if (!e || !*e || (*e == '0' && !e[1]))
			return -1;
		bool digits = true;
		for (const char* p = e; *p; ++p)
			if (*p < '0' || *p > '9') { digits = false; break; }
		if (!digits)
			return 0;
		const unsigned long long v = std::strtoull(e, nullptr, 10);
		return v == 0 ? -1 : static_cast<int64_t>(v);
	}();
	return filter >= 0 &&
		(filter == 0 || static_cast<uint64_t>(filter) == frameIndex);
}
// TOURMALINE_ASYNC_COMPUTE=0 (or "off"): process-wide kill switch for the
// true-async G1/C/G2 split. EVSM itself stays on — the blur passes simply
// record inline on the main graphics command buffer (emulated async, one
// submission per frame, same-queue in-order execution). A/B tool for
// isolating split-only breakage without changing the shadow algorithm
// (unlike TOURMALINE_EVSM=0, which falls back to PCF).
static bool RgAsyncSplitKilled()
{
	static const bool killed = [] {
		const char* e = std::getenv("TOURMALINE_ASYNC_COMPUTE");
		if (!e || !*e)
			return false;
		const std::string_view v{ e };
		return v == "0" || v == "off";
	}();
	return killed;
}
// TOURMALINE_RB_DIAG=1: the readback-lifetime probe (the churn-death hunt's
// readback collision detector — see SubmitAsyncDownloadDataFromBuffers +
// DestroyBufferNow). Off by default: the hunt closed on the SSR ray
// counter's missing creation zero-fill (ssr.h), the readback path was
// exonerated across the whole battery.
static bool RbDiagEnabled()
{
	static const bool on = [] {
		const char* e = std::getenv("TOURMALINE_RB_DIAG");
		return e && std::atoi(e) != 0;
	}();
	return on;
}

// ---- VK_NV_device_diagnostic_checkpoints: the pass-level fault pinpoint ----
// Every RenderGraph pass records a checkpoint marker (its registry slot's
// address); at DEVICE_LOST the driver returns the LAST markers each queue
// reached — DumpDeviceFaultInfo prints them, naming the pass executing at
// the fault (an MMU fault lands on the pass whose marker is last). The
// registry is a fixed ring (cycled, never freed): the driver hands back
// marker POINTERS at death, so the entries must outlive every in-flight
// command buffer — a freed registry would turn the death dump into a
// use-after-free of its own. TOURMALINE_CHECKPOINTS=0 kills the emission
// (the extension stays enabled — the query at death is harmless).
namespace {
	struct CheckpointMarker {
		uint32_t frame = 0;
		uint32_t pos = 0;      // sorted position in the Execute walk
		const char* passName = "UnnamedRenderPass";
	};
	constexpr size_t kCheckpointRing = 16384; // ~100 frames of 3-graph history
	CheckpointMarker g_checkpointRing[kCheckpointRing];
	std::atomic<size_t> g_checkpointNext{ 0 };
	std::atomic<uint32_t> g_checkpointSeq{ 0 };
	bool RgCheckpointsEnabled()
	{
		static const bool enabled = [] {
			const char* e = std::getenv("TOURMALINE_CHECKPOINTS");
			return !(e && *e && *e == '0' && !e[1]); // default ON
		}();
		return enabled;
	}
	// Records one marker into the ring + emits vkCmdSetCheckpointNV into the
	// pass's destination command buffer. Returns nullptr when the extension
	// is absent (the fn is probed once) or the knob is off — the caller skips.
	PFN_vkCmdSetCheckpointNV g_fnVkCmdSetCheckpointNV = nullptr;
	PFN_vkGetQueueCheckpointDataNV g_fnVkGetQueueCheckpointDataNV = nullptr;
	void RgEmitPassCheckpoint(VkCommandBuffer cmd, uint32_t frame, uint32_t pos, const char* passName)
	{
		if (!g_fnVkCmdSetCheckpointNV || !RgCheckpointsEnabled())
			return;
		const size_t idx = g_checkpointNext.fetch_add(1) % kCheckpointRing;
		CheckpointMarker& m = g_checkpointRing[idx];
		m.frame = frame;
		m.pos = pos;
		m.passName = passName ? passName : "UnnamedRenderPass";
		g_checkpointSeq.fetch_add(1, std::memory_order_release);
		g_fnVkCmdSetCheckpointNV(cmd, &m);
	}
} // namespace

// Synchronization
inline void BufferBarrier(
	VkCommandBuffer cmd,
	VkBuffer buffer,
	VkDeviceSize offset,
	VkDeviceSize size,
	VkPipelineStageFlags2 srcStage,
	VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage,
	VkAccessFlags2 dstAccess
);

// The engine's ONE image-barrier helper, in namespace smvk (declared in
// an explicit block so the qualified out-of-line definition below is a
// legal redeclaration) + a global using-declaration so unqualified calls
// from both scope forms (explicit `namespace smvk {}` bodies and
// globally-qualified member definitions) bind the same function. No
// `inline`: the body must link from every TU (see the NOTE at the
// definition).
namespace smvk {
void ImageBarrier(
	VkCommandBuffer cmd,
	VkImage vk_image,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	VkImageSubresourceRange subresourceRange,
	VkPipelineStageFlags2 srcStage,
	VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage,
	VkAccessFlags2 dstAccess,
	uint32_t srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	uint32_t dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED
);
} // namespace smvk
using smvk::ImageBarrier;


// VulkanCore
inline void CreateTimelineSemaphore(uint64_t initialValue, VkSemaphore& out) {
	VkSemaphoreTypeCreateInfo semaphoreTypeInfo{};
	semaphoreTypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	semaphoreTypeInfo.pNext = nullptr;
	semaphoreTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	semaphoreTypeInfo.initialValue = initialValue;
	VkSemaphoreCreateInfo createInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &semaphoreTypeInfo,
		.flags = 0 };
	VK_CHECK(vkCreateSemaphore(device_, &createInfo, nullptr, &out));
};
inline void CreateShaderModuleIn(const std::vector<char>& code, VkShaderModule& out) {
	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = code.size();
	createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

	if (vkCreateShaderModule(device_, &createInfo, nullptr, &out) !=
		VK_SUCCESS) {
		throw std::runtime_error("failed to create shader module!");
	}
}
// ============================================================
// Robust disk pipeline cache (the "Robust pipeline cache
// serialization" scheme): one device-level VkPipelineCache,
// seeded from a validated file and saved via temp-file + rename.
//
// The engine's in-process VulkanPipelineCache dedupes pipeline
// OBJECTS; the driver's VkPipelineCache dedupes the COMPILES —
// both are needed (the object cache alone meant every process
// paid the full shader-microcode compiles, which hitch the
// first traversal of a scene hard: each unseen material/LOD
// bind mints a pipeline at ~ms each, hundreds of them on
// Bistro). With the disk seed, only genuinely new pipelines
// compile.
//
// Robustness (all of it application-side — drivers accept
// arbitrary blobs badly):
//   - Prefix header: magic, dataSize, dataHash (FNV-1a 64 over
//     the blob), vendorID, deviceID, driverVersion, driverABI
//     (sizeof(void*) — 32/64-bit driver builds share UUIDs),
//     pipelineCacheUUID. Anything stale/corrupt -> empty cache.
//   - vkCreatePipelineCache failing on valid-looking initial
//     data (drivers add checks the spec doesn't) -> retried
//     with NO initial data, never a crash and never a run
//     without the in-process cache.
//   - Saves: temp file + rename (no torn files), skipped when
//     the blob is unchanged (size + hash), triggered by every
//     >=64 compiles (abrupt-exit safety) and forced at device
//     teardown.
// TOURMALINE_PIPELINE_CACHE_DIR (replaces the retired TOURMALINE_
// PIPELINE_CACHE file-path contract — that one's unset default
// littered a pipeline_cache.bin into EVERY working dir the hosts
// ran from): unset = F:/pipelinecache (the dedicated scratch disk,
// created when missing); a path = that dir; "off"/"0" = no disk
// seed or save (the in-process cache still runs — it dedupes
// compiles across pipeline objects even without a file). The blob
// itself is always <dir>/pipeline_cache.bin, never the working dir.
// One shared file is safe across GPUs/drivers: the identity header
// above seeds empty on any mismatch, never a wrong blob.
// ============================================================
namespace {

	struct PipelineCachePrefixHeader {
		uint32_t magic;        // kPipelineCacheFileMagic
		uint32_t dataSize;     // == the vkGetPipelineCacheData size
		uint64_t dataHash;     // FNV-1a 64 over the data blob
		uint32_t vendorID;     // VkPhysicalDeviceProperties::vendorID
		uint32_t deviceID;     // VkPhysicalDeviceProperties::deviceID
		uint32_t driverVersion; // VkPhysicalDeviceProperties::driverVersion
		uint32_t driverABI;    // sizeof(void*)
		uint8_t uuid[VK_UUID_SIZE]; // pipelineCacheUUID
	};
	static_assert(sizeof(PipelineCachePrefixHeader) == 48);
	constexpr uint32_t kPipelineCacheFileMagic = 0x43504D53; // "SMPC" LE
	constexpr uint32_t kPipelineCacheHeaderVersion = 1;
	constexpr size_t kPipelineCacheSaveBatch = 16;

	VkPipelineCache g_diskPipelineCache = VK_NULL_HANDLE;
	std::mutex g_diskPipelineCacheMutex;
	bool g_diskPipelineCacheDiskEnabled = true;
	size_t g_diskPipelineCacheLastSavedSize = 0;
	uint64_t g_diskPipelineCacheLastSavedHash = 0;
	size_t g_diskPipelineCacheCompilesSinceSave = 0;

	// The disk home: always <scratchDir>/pipeline_cache.bin (the dir is
	// created when missing — an unusable dir degrades to cache-off with
	// a log line, never a crash; the load/save sites keep their own
	// torn-file tolerance on top). Resolved once at first use.
	const char* PipelineCacheFilePath()
	{
		static const char* sPath = []() -> const char* {
			static std::string storage;
			const char* env = std::getenv("TOURMALINE_PIPELINE_CACHE_DIR");
			if (env && (std::strcmp(env, "off") == 0
				|| std::strcmp(env, "0") == 0))
				return nullptr;
			const std::string dir = (env && *env)
				? std::string(env) : std::string("F:/pipelinecache");
			std::error_code mk;
			std::filesystem::create_directories(dir, mk);
			std::error_code probe;
			if (!std::filesystem::is_directory(dir, probe)) {
				fprintf(stderr,
					"[PipelineCache] scratch dir '%s' unusable (%s) — "
					"running without the disk cache\n",
					dir.c_str(),
					mk ? mk.message().c_str() : "not a directory");
				return nullptr;
			}
			storage = (std::filesystem::path(dir) / "pipeline_cache.bin")
				.generic_string();
			return storage.c_str();
		}();
		return sPath;
	}

	uint64_t PipelineCacheFnv1a64(const uint8_t* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		for (size_t i = 0; i < size; ++i) {
			hash ^= data[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	// Device-init hook: create the VkPipelineCache, seeded from the
	// validated disk file when possible.
	void CreateDiskPipelineCache()
	{
		const char* path = PipelineCacheFilePath();
		g_diskPipelineCacheDiskEnabled = (path != nullptr);

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(physicalDevice_, &props);

		VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
		std::vector<uint8_t> blob;
		if (g_diskPipelineCacheDiskEnabled && path) {
			// Load + validate (header, size, hash, device identity). Any
			// mismatch (driver update, GPU swap, ABI flip, torn write)
			// falls through to an empty cache — the driver never sees a
			// blob it did not itself produce for this exact configuration.
			std::FILE* f = std::fopen(path, "rb");
			if (f) {
				PipelineCachePrefixHeader header{};
				bool ok = std::fread(&header, sizeof(header), 1, f) == 1
					&& header.magic == kPipelineCacheFileMagic
					&& header.dataSize > 0
					&& header.dataSize <= (256u << 20);
				if (ok) {
					blob.resize(header.dataSize);
					ok = std::fread(blob.data(), header.dataSize, 1, f) == 1;
					if (ok)
						ok = PipelineCacheFnv1a64(blob.data(), blob.size()) == header.dataHash;
					if (ok)
						ok = header.vendorID == props.vendorID
						&& header.deviceID == props.deviceID
						&& header.driverVersion == props.driverVersion
						&& header.driverABI == sizeof(void*)
						&& std::memcmp(header.uuid, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
				}
				std::fclose(f);
				if (ok) {
					ci.initialDataSize = blob.size();
					ci.pInitialData = blob.data();
					// Seed the skip state so the first growth-triggered save of a
					// fully-cached run writes nothing (nothing new compiled).
					g_diskPipelineCacheLastSavedSize = blob.size();
					g_diskPipelineCacheLastSavedHash =
						PipelineCacheFnv1a64(blob.data(), blob.size());
					fprintf(stderr,
						"[PipelineCache] loaded %zu bytes from %s\n",
						blob.size(), path);
				}
				else {
					blob.clear();
					fprintf(stderr,
						"[PipelineCache] %s stale or corrupt, starting empty\n", path);
				}
			}
		}
		if (vkCreatePipelineCache(device_, &ci, nullptr, &g_diskPipelineCache) != VK_SUCCESS
			&& ci.pInitialData != nullptr) {
			// The driver rejected data our validation liked (it implements
			// checks the spec does not describe): retry with no initial
			// data — never run without the cache, never crash.
			ci.initialDataSize = 0;
			ci.pInitialData = nullptr;
			if (vkCreatePipelineCache(device_, &ci, nullptr, &g_diskPipelineCache) != VK_SUCCESS) {
				fprintf(stderr, "[PipelineCache] creation failed, running without\n");
				g_diskPipelineCache = VK_NULL_HANDLE;
				return;
			}
			fprintf(stderr, "[PipelineCache] driver rejected initial data, started empty\n");
		}
	}

	// Fetch the current blob; skip the write when unchanged (size +
	// hash); temp file + rename so a crash mid-write can never tear
	// the stored file.
	void SaveDiskPipelineCache(bool force)
	{
		if (g_diskPipelineCache == VK_NULL_HANDLE)
			return;
		const char* path = PipelineCacheFilePath();
		if (!path)
			return;

		size_t size = 0;
		if (vkGetPipelineCacheData(device_, g_diskPipelineCache, &size, nullptr) != VK_SUCCESS || size == 0)
			return;
		std::vector<uint8_t> data(size);
		if (vkGetPipelineCacheData(device_, g_diskPipelineCache, &size, data.data()) != VK_SUCCESS || size == 0)
			return;

		const uint64_t hash = PipelineCacheFnv1a64(data.data(), data.size());
		if (!force && size == g_diskPipelineCacheLastSavedSize && hash == g_diskPipelineCacheLastSavedHash)
			return; // nothing new compiled since the last save

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(physicalDevice_, &props);
		PipelineCachePrefixHeader header{};
		header.magic = kPipelineCacheFileMagic;
		header.dataSize = static_cast<uint32_t>(size);
		header.dataHash = hash;
		header.vendorID = props.vendorID;
		header.deviceID = props.deviceID;
		header.driverVersion = props.driverVersion;
		header.driverABI = static_cast<uint32_t>(sizeof(void*));
		std::memcpy(header.uuid, props.pipelineCacheUUID, VK_UUID_SIZE);

		std::filesystem::path fsPath(path);
		std::filesystem::path tmpPath = fsPath;
		tmpPath += ".tmp";
		bool ok = false;
		if (std::FILE* f = std::fopen(tmpPath.string().c_str(), "wb")) {
			ok = std::fwrite(&header, sizeof(header), 1, f) == 1
				&& std::fwrite(data.data(), data.size(), 1, f) == 1;
			ok = (std::fflush(f) == 0) && ok;
			std::fclose(f);
		}
		else {
			fprintf(stderr, "[PipelineCache] cannot open %s for writing\n",
				tmpPath.string().c_str());
			return;
		}
		if (!ok) {
			fprintf(stderr, "[PipelineCache] write to %s failed\n",
				tmpPath.string().c_str());
			std::error_code ec;
			std::filesystem::remove(tmpPath, ec);
			return;
		}
		std::error_code ec;
		std::filesystem::rename(tmpPath, fsPath, ec);
		if (ec) {
			fprintf(stderr, "[PipelineCache] rename to %s failed (%s)\n",
				path, ec.message().c_str());
			std::filesystem::remove(tmpPath, ec);
			return;
		}
		g_diskPipelineCacheLastSavedSize = size;
		g_diskPipelineCacheLastSavedHash = hash;
		fprintf(stderr, "[PipelineCache] saved %zu bytes to %s\n", size, path);
	}

	// Teardown hook: final forced save, then destroy the cache object
	// (before vkDestroyDevice).
	void DestroyDiskPipelineCache()
	{
		if (g_diskPipelineCache == VK_NULL_HANDLE)
			return;
		{
			std::lock_guard<std::mutex> lock(g_diskPipelineCacheMutex);
			SaveDiskPipelineCache(/*force=*/true);
		}
		vkDestroyPipelineCache(device_, g_diskPipelineCache, nullptr);
		g_diskPipelineCache = VK_NULL_HANDLE;
	}

	// Compile counter (called on every cache-miss pipeline build):
	// batches of >= kPipelineCacheSaveBatch compiles trigger a save, so
	// an abrupt exit loses at most the last batch, and a traversal
	// hitch frame that mints a batch of pipelines persists them for
	// the next run immediately.
	void NotePipelineCompiled()
	{
		if (g_diskPipelineCache == VK_NULL_HANDLE)
			return;
		std::lock_guard<std::mutex> lock(g_diskPipelineCacheMutex);
		if (++g_diskPipelineCacheCompilesSinceSave >= kPipelineCacheSaveBatch) {
			g_diskPipelineCacheCompilesSinceSave = 0;
			SaveDiskPipelineCache(/*force=*/false);
		}
	}

} // namespace

void static CreateGraphicsPipeline(VkGraphicsPipelineCreateInfo createInfo, VkPipeline& pipeline) {
	assert(!pipeline);
	if (vkCreateGraphicsPipelines(device_, g_diskPipelineCache, 1, &createInfo,
		nullptr, &pipeline) != VK_SUCCESS)
	{
		throw std::runtime_error("failed to create graphics pipeline!");
	}

}
void static CreateComputePipeline(VkComputePipelineCreateInfo createInfo, VkPipeline& pipeline)
{
	assert(!pipeline);
	if (vkCreateComputePipelines(device_, g_diskPipelineCache, 1, &createInfo,
		nullptr, &pipeline) != VK_SUCCESS)
	{
		throw std::runtime_error("failed to create graphics pipeline!");
	}
}
void static DestroyGraphicsPipeline(VkPipeline pipeline) {
	vkDestroyPipeline(device_, pipeline, nullptr);
}
constexpr VkFilter toVkFilter(smvk::Filter f) {
	return f == smvk::Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}
constexpr VkSamplerMipmapMode toVkSamplerMipmapMode(smvk::MipmapMode m) {
	return m == smvk::MipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}
constexpr VkSamplerAddressMode toVkSamplerAddressMode(smvk::AddressMode a) {
	switch (a) {
	case smvk::AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	case smvk::AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case smvk::AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
}
void static CreateSampler(const smvk::SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler) {

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = toVkFilter(samplerCreateInfo.magFilter);
	samplerInfo.minFilter = toVkFilter(samplerCreateInfo.minFilter);
	samplerInfo.mipmapMode = toVkSamplerMipmapMode(samplerCreateInfo.mipmapMode);
	samplerInfo.addressModeU = toVkSamplerAddressMode(samplerCreateInfo.addressU);
	samplerInfo.addressModeV = toVkSamplerAddressMode(samplerCreateInfo.addressV);
	samplerInfo.addressModeW = toVkSamplerAddressMode(samplerCreateInfo.addressW);
	samplerInfo.mipLodBias = samplerCreateInfo.mipLodBias;
	samplerInfo.minLod = samplerCreateInfo.minLod;
	samplerInfo.maxLod = samplerCreateInfo.maxLod;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = samplerCreateInfo.compareEnable ? VK_TRUE : VK_FALSE;
	samplerInfo.compareOp = toVkCompareOp(samplerCreateInfo.compareOp);

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
	const bool wantAniso = samplerCreateInfo.maxAnisotropy > 1.0f;
	samplerInfo.anisotropyEnable = wantAniso ? VK_TRUE : VK_FALSE;
	samplerInfo.maxAnisotropy = wantAniso
		? std::min(samplerCreateInfo.maxAnisotropy, properties.limits.maxSamplerAnisotropy)
		: 1.0f;

	VK_CHECK(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler));
}

int smvk::VulkanCore::CreateInstance() {

	if (enableValidationLayers && !checkValidationLayerSupport(config_.validationLayers)) {
		// Diagnostic escape hatch (TOURMALINE_NO_VALIDATION=1): the ASan
		// watchpoint build runs with the validation layer disabled (the
		// uninstrumented layer DLL's intercepted-memcpy interop with the
		// instrumented debug-messenger callback is a false-positive source
		// that drowns the real report) — proceed without the layer instead
		// of refusing to boot.
		const char* noVal = std::getenv("TOURMALINE_NO_VALIDATION");
		if (noVal && std::atoi(noVal) != 0) {
			std::fprintf(stderr, "[Vulkan] validation layer unavailable — continuing WITHOUT it (TOURMALINE_NO_VALIDATION)\n");
			enableValidationLayers = false;
		}
		else {
			throw std::runtime_error(
				"validation layers requested, but not available!");
		}
	}

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "default_smvk_name1";
	//appInfo.applicationVersion = VK_MAKE_API_VERSION(1, 0, 0);
	appInfo.pEngineName = "SamEngine";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_3;

	VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo
	{
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity =
		//VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
	.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
	.pfnUserCallback = debugCallback
	};

	auto extensions = getRequiredExtensionsForSDL(enableValidationLayers);

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();

	VkValidationFeatureDisableEXT disabledValidationFeatures[1] = {
		/*VK_VALIDATION_FEATURE_DISABLE_UNIQUE_HANDLES_EXT*/ // with this on debugNames break
	};

	VkValidationFeatureEnableEXT enabledValidationFeatures[] = {
		VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
#if 0 // TMP-DIAG: GPU-assisted validation (crashes the layer under the
		// editor churn - GPU-AV's own instrumentation faults before the
		// interesting errors surface; core+sync checks carry the hunt)
		, VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT
#endif
	};

	VkValidationFeaturesEXT validationFeatures{};
	validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
	validationFeatures.pNext = &debugCreateInfo;
	validationFeatures.enabledValidationFeatureCount = 1;
	validationFeatures.pEnabledValidationFeatures = enabledValidationFeatures;
	validationFeatures.disabledValidationFeatureCount = 0;
	validationFeatures.pDisabledValidationFeatures = nullptr;

	if (enableValidationLayers) {
		createInfo.enabledLayerCount =
			static_cast<uint32_t>(config_.validationLayers.size());

		createInfo.ppEnabledLayerNames =
			config_.validationLayers.data();

		// validationFeatures -> debugCreateInfo
		createInfo.pNext = &validationFeatures;
	}
	else {
		createInfo.enabledLayerCount = 0;
		createInfo.pNext = nullptr;
	}

	if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
		throw std::runtime_error("failed to create instance!");
	}

	volkLoadInstance(instance_);

	if (enableValidationLayers) {
		if (vkCreateDebugUtilsMessengerEXT(
			instance_,
			&debugCreateInfo,
			nullptr,
			&debugMessenger_) != VK_SUCCESS)
		{
			throw std::runtime_error("failed to set up debug messenger!");
		}
	}

	return 0;
};
bool smvk::VulkanCore::Init() {
	{
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);

		if (deviceCount == 0) {
			throw std::runtime_error("failed to find GPUs with Vulkan support!");
		}

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

		// Prefer DISCRETE GPUs over integrated ones (hybrid laptops put
		// the desktop on the iGPU, and enumeration order often lists it
		// first — "first suitable" would then render on the weak GPU and
		// locks out vendor features like Streamline/NGX that need the
		// dGPU). Ties keep the loader's order.
		{
			VkPhysicalDevice best = VK_NULL_HANDLE;
			int bestScore = -1;
			for (const auto& device : devices) {
				if (!isDeviceSuitable(device, surface_, config_.deviceExtensions))
					continue;
				VkPhysicalDeviceProperties props{};
				vkGetPhysicalDeviceProperties(device, &props);
				const int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
					? 2 : (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 1 : 0;
				if (score > bestScore) {
					bestScore = score;
					best = device;
				}
			}
			physicalDevice_ = best;
		}

		if (physicalDevice_ == VK_NULL_HANDLE) {
			throw std::runtime_error("failed to find a suitable GPU!");
		}

		CheckTextureFormatSupport(); // Check block format support

		vkGetPhysicalDeviceFeatures2(physicalDevice_, &vkFeatures10_);
		vkGetPhysicalDeviceProperties2(physicalDevice_, &vkPhysicalDeviceProperties2_);

		// Create Logical Device
		{
			indices = findQueueFamilies(physicalDevice_, surface_);
			std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
			std::set<uint32_t> uniqueQueueFamilies = {
				indices.graphicsFamily.value(),
				indices.presentFamily.value(),
				indices.transferFamily.value(),
				indices.computeFamily.value_or(indices.graphicsFamily.value())
			};

			float queuePriority = 1.0f;
			for (uint32_t queueFamily : uniqueQueueFamilies) {
				VkDeviceQueueCreateInfo queueCreateInfo{};
				queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				queueCreateInfo.queueFamilyIndex = queueFamily;
				queueCreateInfo.queueCount = 1;
				queueCreateInfo.pQueuePriorities = &queuePriority;
				queueCreateInfos.push_back(queueCreateInfo);
			}

			// Check for VK_EXT_host_image_copy support
			bool hostImageCopyAvailable = false;
			{
				uint32_t extCount = 0;
				vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr);
				std::vector<VkExtensionProperties> exts(extCount);
				vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, exts.data());
				for (auto& e : exts) if (strcmp(e.extensionName, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME) == 0) { hostImageCopyAvailable = true; break; }
			}
			// TOURMALINE_HOST_COPY=0: keep the extension off the device
			// entirely (uploads fall back to the transfer ring). RenderDoc's
			// support for VK_EXT_host_image_copy is incomplete and can
			// crash captures — set this in RenderDoc launch configs.
			// Mirrored by the VulkanResourceManager feature check (see
			// HostCopyKilledByEnv) so the streamer never calls into an
			// extension that is not enabled on the device.
			const bool hostCopyKilled = HostCopyKilledByEnv();
			bool enableHostCopy = false;
			if (hostImageCopyAvailable && !hostCopyKilled) {
				VkPhysicalDeviceHostImageCopyFeaturesEXT queryHostCopy{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT, nullptr };
				VkPhysicalDeviceFeatures2 queryFeats2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &queryHostCopy };
				vkGetPhysicalDeviceFeatures2(physicalDevice_, &queryFeats2);
				if (queryHostCopy.hostImageCopy == VK_TRUE) enableHostCopy = true;
			}
			if (hostImageCopyAvailable && hostCopyKilled)
				printf("[UploadStreamer] VK_EXT_host_image_copy disabled (TOURMALINE_HOST_COPY=0)\n");

			// VK_EXT_mesh_shader (task/mesh pipelines for the mesh-shader gfx
			// filter path): strictly optional. When present + the taskShader
			// and meshShader features are supported, the extension is enabled
			// and the features chained below; otherwise the engine keeps the
			// compute+indexed CPU-fallback path.
			bool meshExtPresent = false;
			{
				uint32_t extCount = 0;
				if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr) == VK_SUCCESS
					&& extCount != 0) {
					std::vector<VkExtensionProperties> exts(extCount);
					if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, exts.data()) == VK_SUCCESS) {
						for (const auto& e : exts) {
							if (std::strcmp(e.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0) {
								meshExtPresent = true;
								break;
							}
						}
					}
				}
			}
			const bool meshKilled = MeshKilledByEnv();
			bool enableMeshShader = false;
			VkPhysicalDeviceMeshShaderFeaturesEXT deviceMeshFeatures{};
			deviceMeshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
			if (meshExtPresent && !meshKilled) {
				VkPhysicalDeviceMeshShaderFeaturesEXT supportedMesh{
					VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, nullptr };
				VkPhysicalDeviceFeatures2 supported2{
					VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supportedMesh };
				vkGetPhysicalDeviceFeatures2(physicalDevice_, &supported2);
				if (supportedMesh.taskShader == VK_TRUE && supportedMesh.meshShader == VK_TRUE) {
					enableMeshShader = true;
					deviceMeshFeatures.taskShader = VK_TRUE;
					deviceMeshFeatures.meshShader = VK_TRUE;
				}
			}
			if (meshExtPresent && meshKilled)
				printf("[Vulkan] VK_EXT_mesh_shader disabled (TOURMALINE_MESH=0)\n");

			VkPhysicalDeviceHostImageCopyFeaturesEXT deviceHostCopyFeatures{
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT,
				enableMeshShader ? (void*)&deviceMeshFeatures : nullptr,
				enableHostCopy ? VK_TRUE : VK_FALSE
			};

			VkPhysicalDeviceVulkan11Features deviceFeatures11
			{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
				.pNext = enableHostCopy ? (void*)&deviceHostCopyFeatures : (enableMeshShader ? (void*)&deviceMeshFeatures : nullptr),
				.storageBuffer16BitAccess = VK_TRUE,
				.uniformAndStorageBuffer16BitAccess = VK_TRUE,
			};
			VkPhysicalDeviceVulkan12Features deviceFeatures12{};
			deviceFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			deviceFeatures12.pNext = &deviceFeatures11;
			deviceFeatures12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
			deviceFeatures12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
			deviceFeatures12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
			// engine globals binding (kBinding_EngineGlobals): the
			// storage-buffer descriptors are written once at init (per
			// flight set) and never touched again, but the layout's
			// UPDATE_AFTER_BIND bindings (and every deferred null-write /
			// bindless refresh on the UAB pool's sets) need the feature
			// enabled at device creation.
			deviceFeatures12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
			deviceFeatures12.descriptorBindingPartiallyBound = VK_TRUE;
			deviceFeatures12.descriptorBindingVariableDescriptorCount = VK_TRUE;
			deviceFeatures12.runtimeDescriptorArray = VK_TRUE;
			deviceFeatures12.scalarBlockLayout = VK_TRUE;
			deviceFeatures12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
			deviceFeatures12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
			deviceFeatures12.timelineSemaphore = VK_TRUE;
			deviceFeatures12.bufferDeviceAddress = VK_TRUE;
			VkPhysicalDeviceVulkan13Features deviceFeatures13
			{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
				.pNext = &deviceFeatures12,
				// privateData rides the 13 struct — the old individually
				// chained VkPhysicalDevicePrivateDataFeatures beside it
				// was a pNext duplication violation
				// (VUID-VkDeviceCreateInfo-pNext-06532). Member order:
				// privateData precedes synchronization2 in the struct.
				.privateData = VK_TRUE,
				.synchronization2 = VK_TRUE,
				.dynamicRendering = VK_TRUE,
				.maintenance4 = VK_TRUE,
			};
			// NOTE: plain assignments, no mixed designated/positional
			// initializer (MSVC C7556 rejects the mix).
			VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR computeDerivativesFeatures{};
			computeDerivativesFeatures.sType =
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR;
			computeDerivativesFeatures.computeDerivativeGroupQuads = VK_TRUE;

			VkPhysicalDeviceFeatures2 feats2{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
				// Only structs NOT promoted into the 1.2/1.3 feature structs
				// chain here (computeDerivatives is a KHR extension struct);
				// every promoted feature rides its version struct or the
				// validation flags the duplication (see the privateData note
				// above).
				.pNext = &computeDerivativesFeatures,
				.features = {
					.samplerAnisotropy = VK_TRUE,
					.textureCompressionBC = VK_TRUE,
				},
			};
			computeDerivativesFeatures.pNext = &deviceFeatures13;

			// Indirect multi-draw (per-view filter emits one command per
			// surviving cluster, drawn with a single call per material range).
			// Core since 1.0 but still optional: enable when present, emulate
			// one-by-one with a startup warning when absent.
			{
				VkPhysicalDeviceFeatures supported{};
				vkGetPhysicalDeviceFeatures(physicalDevice_, &supported);
				g_multiDrawIndirect = (supported.multiDrawIndirect == VK_TRUE);
				feats2.features.multiDrawIndirect =
					g_multiDrawIndirect ? VK_TRUE : VK_FALSE;
				// Fragment-stage storage writes (texture-streaming feedback
				// atomics in the materials, set 0 binding 4): core-optional
				// since 1.0, universal on desktop. No emulation exists, so a
				// device without it cannot run the materials — fail loudly.
				feats2.features.fragmentStoresAndAtomics = VK_TRUE;
				if (supported.fragmentStoresAndAtomics != VK_TRUE) {
					throw std::runtime_error(
						"device lacks fragmentStoresAndAtomics (required for "
						"texture-streaming GPU feedback)");
				}
				// 64-bit integer arithmetic (the BDA push-constant shaders —
				// vk::BufferPointer lowers to uint64 math, so the modules
				// declare the Int64 capability; validation fails the module
				// create without the feature even where desktop drivers
				// tolerate it — a validation-Debug run surfaced the spam).
				feats2.features.shaderInt64 = VK_TRUE;
				if (supported.shaderInt64 != VK_TRUE) {
					throw std::runtime_error(
						"device lacks shaderInt64 (required for the buffer-"
						"device-address push-constant shaders)");
				}
				// Non-solid fill modes (PolygonMode::Line, the wireframe debug
				// view). Core-optional since 1.0, universal on desktop; when
				// absent BuildGraphicsPipeline clamps pipelines back to Fill so
				// the app never rasterizes with an unsupported mode.
				g_fillModeNonSolid = (supported.fillModeNonSolid == VK_TRUE);
				feats2.features.fillModeNonSolid =
					g_fillModeNonSolid ? VK_TRUE : VK_FALSE;
				// The gfx filter packs a cluster id into firstInstance of every
				// indirect draw — that needs the core-optional
				// drawIndirectFirstInstance, or validation (and possibly the
				// driver) rejects the non-zero values the filter emits.
				g_multiDrawIndirectFirstInstance =
					(supported.drawIndirectFirstInstance == VK_TRUE);
				feats2.features.drawIndirectFirstInstance =
					g_multiDrawIndirectFirstInstance ? VK_TRUE : VK_FALSE;
				// shaderDemoteToHelperInvocation (clip/discard in the MASK
				// alpha-test shaders compiles to OpDemoteToHelperInvocation):
				// without the feature, the modules fail validation and pipeline
				// execution is undefined. Core since 1.3, universal on desktop.
				{
					VkPhysicalDeviceVulkan13Features supported13{
						VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, nullptr };
					VkPhysicalDeviceFeatures2 supported2{
						VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported13 };
					vkGetPhysicalDeviceFeatures2(physicalDevice_, &supported2);
					g_shaderDemoteToHelperInvocation =
						(supported13.shaderDemoteToHelperInvocation == VK_TRUE);
					deviceFeatures13.shaderDemoteToHelperInvocation =
						g_shaderDemoteToHelperInvocation ? VK_TRUE : VK_FALSE;
				}
			}

			// VK_NV_low_latency2 (NVIDIA Reflex / Streamline PCL latency path):
			// strictly optional — enables sl.common's VkNvLowLatency2 init
			// ("Failed to init VkNvLowLatency2: 2" when absent) and the PCL
			// marker timings. Probed, never required: non-NVIDIA devices
			// simply skip it (isDeviceSuitable above only gates the required
			// config_.deviceExtensions list). No feature struct exists for
			// this extension — the name alone switches it on.
			// GATED on the Streamline integration actually running: the
			// extension exists ONLY for the Reflex/PCL path, so enabling it
			// without sl.common driving it (the editor never loads
			// Streamline — see Editor's main.cpp) leaves the device's
			// low-latency queue state initialized-but-undrained. An editor
			// editor's first heavy frame wedged the GPU past the Windows TDR
			// window in exactly that state (bistro glass, VK_ERROR_DEVICE_LOST
			// + nvlddmkm 4101 reset); with the extension gated off the same
			// frame runs in tens of ms.
			bool enableLowLatency2 = false;
			{
				uint32_t extCount = 0;
				if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr) == VK_SUCCESS
					&& extCount != 0) {
					std::vector<VkExtensionProperties> exts(extCount);
					if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, exts.data()) == VK_SUCCESS) {
						for (const auto& e : exts) {
							if (std::strcmp(e.extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0) {
								enableLowLatency2 = true;
								break;
							}
						}
					}
				}
				// DlssStartup() ran before any Vulkan object exists (the
				// interposer bootstrap above), so its result is authoritative
				// here: no Streamline -> no low-latency extension either.
				if (enableLowLatency2 && !DlssAvailable()) {
					enableLowLatency2 = false;
					std::fprintf(stderr, "[Vulkan] VK_NV_low_latency2 supported"
						" - NOT enabling (Streamline not loaded; Reflex/PCL"
						" has no driver here)\n");
				}
				else {
					std::fprintf(stderr, "[Vulkan] VK_NV_low_latency2 %s\n",
						enableLowLatency2 ? "supported - enabling (Reflex/PCL latency path)" : "not supported - PCL timings off");
				}
			}

			// VK_EXT_device_fault (post-mortem GPU fault reports): strictly
			// optional. When present + the deviceFault feature is supported,
			// the extension is enabled and the feature chained so a device
			// loss can be diagnosed (faulting address/stage) instead of
			// surfacing as a bare vkWaitSemaphores/device-lost failure.
			bool enableDeviceFault = false;
			VkPhysicalDeviceFaultFeaturesEXT faultFeatures{
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, nullptr, VK_FALSE };
			{
				bool extPresent = false;
				uint32_t extCount = 0;
				if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr) == VK_SUCCESS
					&& extCount != 0) {
					std::vector<VkExtensionProperties> exts(extCount);
					if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, exts.data()) == VK_SUCCESS) {
						for (const auto& e : exts) {
							if (std::strcmp(e.extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0) {
								extPresent = true;
								break;
							}
						}
					}
				}
				if (extPresent) {
					VkPhysicalDeviceFaultFeaturesEXT supportedFault{
						VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, nullptr };
					VkPhysicalDeviceFeatures2 supported2{
						VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supportedFault };
					vkGetPhysicalDeviceFeatures2(physicalDevice_, &supported2);
					if (supportedFault.deviceFault == VK_TRUE) {
						enableDeviceFault = true;
						faultFeatures.deviceFault = VK_TRUE;
						faultFeatures.pNext = feats2.pNext;
						feats2.pNext = &faultFeatures;
					}
				}
				g_deviceFaultSupported = enableDeviceFault;
				std::fprintf(stderr, "[Vulkan] VK_EXT_device_fault %s\n",
					enableDeviceFault ? "supported - enabling (loss diagnostics)" : "not supported - loss will be opaque");
			}

			// VK_NV_device_diagnostics_config (NVIDIA Nsight Aftermath shader
			// debug tables): the driver-side half of crash symbolication. DXC's
			// -Zi only puts the HLSL -> SPIR-V line info into the modules this
			// tree ships; Aftermath crash dumps ALSO need the
			// driver-generated mapping from GPU machine code back to that
			// SPIR-V (the .nvdbg line tables). Chaining this struct with the
			// shader-debug-info bit makes the NVIDIA driver generate those
			// tables at pipeline compile time — the Vulkan equivalent of D3D's
			// GFSDK_Aftermath_FeatureFlags_GenerateShaderDebugInfo. Only
			// meaningful when the loaded SPIR-V itself carries debug info
			// (AftermathShaderDebugTablesEnabled above: Debug trees + the
			// x64-Release-ShaderDebug preset), and only present on NVIDIA —
			// probed, never required (same pattern as low_latency2 /
			// device_fault above). The Aftermath Monitor writes the matching
			// .nvdbg files on crash; nothing here calls
			// GFSDK_Aftermath_EnableGpuCrashDumps, so the Monitor's settings
			// stay authoritative for dump writing.
			bool enableDiagnosticsConfig = false;
			VkDeviceDiagnosticsConfigCreateInfoNV diagnosticsConfigInfo{
				VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV, nullptr,
				VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV };
			if (AftermathShaderDebugTablesEnabled()) {
				uint32_t extCount = 0;
				if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr) == VK_SUCCESS
					&& extCount != 0) {
					std::vector<VkExtensionProperties> exts(extCount);
					if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, exts.data()) == VK_SUCCESS) {
						for (const auto& e : exts) {
							if (std::strcmp(e.extensionName, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME) == 0) {
								enableDiagnosticsConfig = true;
								break;
							}
						}
					}
				}
			}
			std::fprintf(stderr, "[Vulkan] VK_NV_device_diagnostics_config %s\n",
				enableDiagnosticsConfig
				? "supported - enabling (Aftermath shader debug tables: GPU code -> SPIR-V -> HLSL lines)"
				: AftermathShaderDebugTablesEnabled()
				? "not supported - Aftermath crash dumps stay without shader line maps (non-NVIDIA driver?)"
				: "off (TOURMALINE_AFTERMATH=0, or this tree's SPIR-V carries no -Zi debug info)");

		std::vector<const char*> enabledExtensions = config_.deviceExtensions;
		if (enableHostCopy) enabledExtensions.push_back(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);
		if (enableMeshShader) enabledExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
		if (enableLowLatency2) enabledExtensions.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
		if (enableDeviceFault) enabledExtensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
		if (enableDiagnosticsConfig) enabledExtensions.push_back(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
		// VK_NV_device_diagnostic_checkpoints (the pass-level fault pinpoint,
		// complements the fault/diagnostics extensions above): every RenderGraph
		// pass records a vkCmdSetCheckpointNV marker (its registry slot —
		// frame + sorted position + pass name); at DEVICE_LOST,
		// vkGetQueueCheckpointDataNV hands back the LAST markers the GPU
		// reached per queue — DumpDeviceFaultInfo prints them, naming the
		// exact pass executing at the fault. Valid to query on a lost
		// device (that is the extension's whole purpose). NVIDIA exposes it;
		// probed, never required.
		bool enableDiagCheckpoints = false;
		{
			uint32_t extCount = 0;
			if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr) == VK_SUCCESS
				&& extCount != 0) {
				std::vector<VkExtensionProperties> exts(extCount);
				if (vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, exts.data()) == VK_SUCCESS) {
					for (const auto& e : exts) {
						if (std::strcmp(e.extensionName, "VK_NV_device_diagnostic_checkpoints") == 0) {
							enableDiagCheckpoints = true;
							break;
						}
					}
				}
			}
		}
		if (enableDiagCheckpoints) enabledExtensions.push_back("VK_NV_device_diagnostic_checkpoints");
		std::fprintf(stderr, "[Vulkan] VK_NV_device_diagnostic_checkpoints %s\n",
			enableDiagCheckpoints
			? "supported - enabling (per-pass fault pinpoint: the last-reached pass at DEVICE_LOST)"
			: "not supported (faults won't name the executing pass)");

			VkDeviceCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
			createInfo.pQueueCreateInfos = queueCreateInfos.data();
			createInfo.pEnabledFeatures = nullptr;
			createInfo.pNext = &feats2;
			createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
			createInfo.ppEnabledExtensionNames = enabledExtensions.data();
			createInfo.enabledLayerCount = 0;
			// Aftermath head-splice: chain the diagnostics config in front
			// of the feats2 -> faultFeatures feature chain below it — this
			// struct is not a feature, it rides pNext directly under
			// VkDeviceCreateInfo, and the driver walks the full chain.
			if (enableDiagnosticsConfig) {
				diagnosticsConfigInfo.pNext = createInfo.pNext; // = &feats2 (+faultFeatures)
				createInfo.pNext = &diagnosticsConfigInfo;
			}

			if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
				throw std::runtime_error("failed to create logical device!");
			}
			g_meshShaderSupported = enableMeshShader;
			std::fprintf(stderr, "[Vulkan] VK_EXT_mesh_shader %s\n",
				enableMeshShader ? "supported - enabling (task+mesh gfx filter path)"
				: meshKilled ? "off (TOURMALINE_MESH=0, compute+indexed CPU fallback)"
				: "not supported - compute+indexed CPU fallback");
			if (!g_multiDrawIndirect) {
				fprintf(stderr,
					"[Vulkan Warning] multiDrawIndirect not supported by this device; "
					"vkCmdDrawIndexedIndirect with drawCount > 1 will be emulated "
					"one draw at a time (slow path, expect lower throughput)\n");
			}
			if (!g_fillModeNonSolid) {
				fprintf(stderr,
					"[Vulkan Warning] fillModeNonSolid not supported by this device; "
					"wireframe (PolygonMode::Line) pipelines clamp to Fill\n");
			}
			volkLoadDevice(device_);
		// Diagnostic-checkpoint entry points (the extension is enabled
		// above when present; the fn pointers gate the emission + the
		// death-dump query — see RgEmitPassCheckpoint / DumpDeviceFaultInfo).
		g_fnVkCmdSetCheckpointNV = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(
			vkGetDeviceProcAddr(device_, "vkCmdSetCheckpointNV"));
		g_fnVkGetQueueCheckpointDataNV = reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(
			vkGetDeviceProcAddr(device_, "vkGetQueueCheckpointDataNV"));
		if (enableDiagCheckpoints && (!g_fnVkCmdSetCheckpointNV || !g_fnVkGetQueueCheckpointDataNV)) {
			// Present-but-not-loadable: treat as absent (emission skips).
			g_fnVkCmdSetCheckpointNV = nullptr;
			g_fnVkGetQueueCheckpointDataNV = nullptr;
			std::fprintf(stderr, "[Vulkan] VK_NV_device_diagnostic_checkpoints present but entry points missing - pinpoint disabled\n");
		}
			// Disk pipeline cache (see the CreateDiskPipelineCache block):
			// created right after the device so every pipeline below compiles
			// through it.
			CreateDiskPipelineCache();

		}

		QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
		vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
		vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
		{
			QueueFamilyIndices q2 = findQueueFamilies(physicalDevice_, surface_);
			if (q2.transferFamily) vkGetDeviceQueue(device_, q2.transferFamily.value(), 0, &transferQueue_);
			else transferQueue_ = graphicsQueue_;
			if (q2.computeFamily) vkGetDeviceQueue(device_, q2.computeFamily.value(), 0, &computeQueue_);
			else computeQueue_ = graphicsQueue_;
		}

		// Optick GPU timelines need the device + graphics queue + volk dispatch.
		smvk::profiler::InitBackendGpuTimelinesImpl();
	};

	return true;

}
inline smvk::VulkanCore::~VulkanCore() {
	if (device_ != VK_NULL_HANDLE) {
		VkResult r = vkDeviceWaitIdle(device_);
		if (r == VK_ERROR_DEVICE_LOST) {
			fprintf(stderr, "[Vulkan Warning] vkDeviceWaitIdle returned VK_ERROR_DEVICE_LOST during ~VulkanCore, proceeding with teardown\n");
		}
	}
	smvk::profiler::Shutdown();
	if (device_ != VK_NULL_HANDLE) {
		std::lock_guard<std::mutex> lock(g_allPoolsMutex);
		for (auto& [key, pools] : g_allPools) {
			for (auto* pool : pools) {
				if (pool) {
					pool->destroy();
					pool->~CommandBufferPool(); // linear-owned: dtor only, never freed
				}
			}
		}
		g_allPools.clear();
		t_commandPools.clear();
	}
	if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(instance_, surface_, nullptr);
		surface_ = VK_NULL_HANDLE;
	}
	if (instance_ != VK_NULL_HANDLE && debugMessenger_ != VK_NULL_HANDLE) {
		DestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
		debugMessenger_ = VK_NULL_HANDLE;
	}
	if (device_ != VK_NULL_HANDLE) {
		// Disk pipeline cache: final forced save before the device dies
		// (the blob is only complete now; also destroys the cache object).
		DestroyDiskPipelineCache();
		vkDestroyDevice(device_, nullptr);
		device_ = VK_NULL_HANDLE;
	}
	if (instance_ != VK_NULL_HANDLE) {
		vkDestroyInstance(instance_, nullptr);
		instance_ = VK_NULL_HANDLE;
	}
	physicalDevice_ = VK_NULL_HANDLE;
	graphicsQueue_ = VK_NULL_HANDLE;
	presentQueue_ = VK_NULL_HANDLE;
	transferQueue_ = VK_NULL_HANDLE;
	// vkDestroyInstance above was the final Vulkan call of the process:
	// volk's tables and VMA's imported functions still point into the
	// Streamline interposer (DlssShutdown only ran slShutdown), so only
	// now is it safe to unmap it. No-op when Streamline is absent.
	DlssRelease();
}
smvk::mem::LinearPtr<smvk::VulkanCore> smvk::CreateVulkanCoreWithWindow(smvk::VulkanCore::CoreConfiguration config, SDL_Window* window) {
	assert(window != nullptr && "window was nullptr!");

	auto unique = smvk::mem::MakeProcess<VulkanCore>(config);
	auto core = unique.get();

	core->CreateInstance();
#ifdef _WIN32
	// Win32 surface created through the volk-resolved function pointer —
	// the Streamline proxy when DLSS is active, SDL's equivalent native
	// call otherwise (SDL_Vulkan_CreateSurface would resolve its own
	// vkCreateWin32SurfaceKHR from vulkan-1.dll and bypass the hook).
	{
		SDL_PropertiesID windowProps = SDL_GetWindowProperties(window);
		HWND hwnd = reinterpret_cast<HWND>(SDL_GetPointerProperty(
			windowProps, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
		HINSTANCE hinstance = reinterpret_cast<HINSTANCE>(SDL_GetPointerProperty(
			windowProps, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr));
		if (!hinstance)
			hinstance = GetModuleHandleW(nullptr);
		if (!hwnd || !hinstance)
			throw std::runtime_error("failed to get Win32 window handles for surface creation");
		VkWin32SurfaceCreateInfoKHR surfaceInfo{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
		surfaceInfo.hinstance = hinstance;
		surfaceInfo.hwnd = hwnd;
		if (vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) {
			throw std::runtime_error("failed to create window surface");
		}
	}
#else
	// Linux (xcb/wayland via SDL): no Streamline interposer to route
	// through on this platform - SDL_Vulkan_CreateSurface resolves the
	// platform-appropriate vkCreate*SurfaceKHR from the same loader volk
	// resolved (libvulkan), which is the plain path we want.
	{
		if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
			throw std::runtime_error("failed to create window surface (SDL_Vulkan_CreateSurface)");
		}
	}
#endif
	core->Init();
	return unique;
}


// ResourceContext
smvk::VulkanResourceManager::VulkanResourceManager(
	VulkanCore::CoreConfiguration configuration,
	SDL_Window* window)
	: core_(CreateVulkanCoreWithWindow(configuration, window))
{
	//assert(window != nullptr && "headless not implemented yet");
	VmaAllocatorCreateInfo allocatorCreateInfo = {};
	allocatorCreateInfo.physicalDevice = physicalDevice_;
	allocatorCreateInfo.device = device_;
	allocatorCreateInfo.instance = instance_;
	allocatorCreateInfo.vulkanApiVersion = core_->config_.version_ == VK_1_3 ? VK_API_VERSION_1_3 : UINT32_MAX;
	allocatorCreateInfo.flags =
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT |
		VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (allocatorCreateInfo.vulkanApiVersion == UINT32_MAX) { assert(false); };

	VmaVulkanFunctions vulkanFunctions;
	VK_CHECK(vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vulkanFunctions));
	allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;
	VK_CHECK(vmaCreateAllocator(&allocatorCreateInfo, &m_allocator));

	m_submitter = smvk::mem::ProcessAllocator().Create<Submitter>(*core_, core_->indices.graphicsFamily.value());
	if (core_->indices.transferFamily && core_->indices.transferFamily.value() != core_->indices.graphicsFamily.value()) {
		m_transferSubmitter = smvk::mem::ProcessAllocator().Create<Submitter>(*core_, core_->indices.transferFamily.value());
	}
	else {
		// Single-queue devices: uploads ride the graphics queue. All
		// cross-queue paths below degrade to same-queue FIFO (family
		// indices match, ownership barriers collapse, waits are no-ops).
		m_transferSubmitter = m_submitter;
	}
	{
		const uint32_t computeFam = core_->indices.computeFamily.value_or(core_->indices.graphicsFamily.value());
		if (computeFam != core_->indices.graphicsFamily.value()) {
			m_computeSubmitter = smvk::mem::ProcessAllocator().Create<Submitter>(*core_, computeFam);
		}
		else {
			// No dedicated compute queue: blur still records as compute
			// work, submitted on graphics (emulated async, still pipelined).
			m_computeSubmitter = m_submitter;
		}
	}
	{
		VkPhysicalDeviceHostImageCopyFeaturesEXT hostFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT, nullptr };
		VkPhysicalDeviceFeatures2 feats2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &hostFeat };
		vkGetPhysicalDeviceFeatures2(physicalDevice_, &feats2);
		// Reflect the device's ACTUAL enabled state, not the physical
		// device's capability: with TOURMALINE_HOST_COPY=0 the extension is
		// absent from the device, and the streamer must not call it.
		m_hostImageCopySupported =
			hostFeat.hostImageCopy == VK_TRUE && !HostCopyKilledByEnv();
		if (m_hostImageCopySupported) {
			// Two-stage query: first obtain counts, then allocate and retrieve layouts.
			// pCopySrcLayouts / pCopyDstLayouts are not universally containing
			// SHADER_READ_ONLY_OPTIMAL – only GENERAL is guaranteed.
			VkPhysicalDeviceHostImageCopyPropertiesEXT props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES_EXT, nullptr };
			VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &props };
			vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);
			m_hostCopySrcLayouts.resize(props.copySrcLayoutCount);
			m_hostCopyDstLayouts.resize(props.copyDstLayoutCount);
			props.pCopySrcLayouts = m_hostCopySrcLayouts.empty() ? nullptr : m_hostCopySrcLayouts.data();
			props.pCopyDstLayouts = m_hostCopyDstLayouts.empty() ? nullptr : m_hostCopyDstLayouts.data();
			vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);
			m_hostCopyProps = props;
			// m_hostCopyProps was shallow-copied; restore pointers to persistent storage
			m_hostCopyProps.pCopySrcLayouts = m_hostCopySrcLayouts.empty() ? nullptr : m_hostCopySrcLayouts.data();
			m_hostCopyProps.pCopyDstLayouts = m_hostCopyDstLayouts.empty() ? nullptr : m_hostCopyDstLayouts.data();
		}
		if (m_hostImageCopySupported) {
			printf("[UploadStreamer] VK_EXT_host_image_copy enabled (optimalTilingLayoutUUID supported)\n");
		}
		else {
			printf("[UploadStreamer] VK_EXT_host_image_copy not available, using transfer queue\n");
		}
	}
#if true
	//m_hostImageCopySupported = false;
#endif
	m_streamer = smvk::mem::MakeProcess<UploadStreamer>(this, m_transferSubmitter, m_submitter);
	m_streamer->init(smvk::mem::config::Runtime().streamerTotalBudget);

	//this->m_hostImageCopySupported = false;

	// SamplerCache – owns up to MAX_SAMPLERS deduplicated VkSamplers.
	// Like VulkanPipelineCache, key (SamplerCreateInfo) → stable index.
	samplerCache_ = smvk::mem::MakeProcess<VulkanSamplerCache>(this);
	// Populate default slots: 0 = regular, 1 = shadow comparison,
	// 2 = linear clamp-to-edge (lightmap atlases + 3D probe volumes:
	// both interpolate amplitudes that must never wrap across borders).
	// Keep legacy handles in sync for existing code paths.
	{
		uint32_t idx0 = samplerCache_->GetOrCreate(SamplerCreateInfo{});
		m_defaultSampler = samplerCache_->at(idx0);
		uint32_t idx1 = samplerCache_->GetOrCreate(SamplerCreateInfo{
			.compareOp = smvk::CompareOp::LessEqual,
			.compareEnable = true
			});
		m_defaultShadowSampler = samplerCache_->at(idx1);
		SamplerCreateInfo clampCi{};
		clampCi.addressU = smvk::AddressMode::ClampToEdge;
		clampCi.addressV = smvk::AddressMode::ClampToEdge;
		clampCi.addressW = smvk::AddressMode::ClampToEdge;
		uint32_t idx2 = samplerCache_->GetOrCreate(clampCi);
		assert(idx0 == 0 && idx1 == 1 && "sampler cache must allocate defaults at 0,1");
		assert(idx2 < MAX_SAMPLERS && "clamp sampler must stay in the array");
		assert(idx2 == 2 && "clamp sampler expected at index 2 (shaders hardcode samplers[2])");
	}
	const uint32_t pixel = 0xFF000000;
	m_nilTexture = CreateTexture(
		ImageCreateInfo
		{
			.extent = { 1,1,1 },
			.format = smvk::Format::RGBA8_SRGB,
			.name = "nilTexture"
		},
		&pixel
	).release();
	// Cache the nil view NOW (see the member comment): freed bindless
	// slots retire their descriptor writes with this view +
	// ShaderReadOnlyOptimal (the layout UploadDataToImage2D leaves it in
	// for the fragment sampled-read usage above, the same layout every
	// CreateTexture bindless entry declares).
	m_nullImageView = GetTextureVkImageView(m_nilTexture);
	assert(m_nullImageView != VK_NULL_HANDLE && "nilTexture view missing");

	// Engine globals buffer: one EngineGlobals slice per frame in flight,
	// read by every shader through set 0 / kBinding_EngineGlobals. Each
	// flight's descriptor set permanently points at its OWN slice (baked
	// once at init - see the set allocation below). Values (time /
	// deltaTime / frameIndex / fps) are computed engine-side from the
	// acquire rhythm and flushed by VulkanSwapchain::AcquireFramebuffer
	// after the in-flight slot retires. No app-side plumbing.
	m_engineGlobalsBuffer = CreateBuffer(
		sizeof(smvk::EngineGlobals) * MAX_FRAMES_IN_FLIGHT,
		BufferType_Storage,
		"engine globals"
	).release();
	m_globalsStartTime = std::chrono::steady_clock::now();

	// ============================================================================
	// Hardcoded Bindless Setup
	// ============================================================================

	VkShaderStageFlags stageFlags =
		VK_SHADER_STAGE_VERTEX_BIT
		| VK_SHADER_STAGE_FRAGMENT_BIT
		| VK_SHADER_STAGE_COMPUTE_BIT;

	const VkDescriptorSetLayoutBinding bindings[kBinding_NumBindings] = {
		getDSLBinding(kBinding_Textures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_TEXTURES, stageFlags),
		getDSLBinding(kBinding_Samplers, VK_DESCRIPTOR_TYPE_SAMPLER, MAX_SAMPLERS, stageFlags),
		getDSLBinding(kBinding_StorageImages, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_TEXTURES, stageFlags),
		// Engine globals: single read-only storage buffer, one slice per frame
		// in flight. Each flight's OWN set (see the set allocation below)
		// points at its own slice, written ONCE at init; FlushEngineGlobals
		// only uploads values - never rewrites the descriptor.
		getDSLBinding(kBinding_EngineGlobals, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stageFlags),
		// 3D volume textures (SG probe grids): same slot space as
		// kBinding_Textures; UpdateDescriptors mirrors 3D views here.
		getDSLBinding(kBinding_VolumeTextures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_VOLUME_TEXTURES, stageFlags),
		// Reflection cubemaps for specular IBL: same slot space as
		// kBinding_Textures; UpdateDescriptors mirrors cube views here.
		getDSLBinding(kBinding_Cubemaps, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_TEXTURES, stageFlags),
		// Multisampled 2D textures (EVSM per-sample depth resolve): same
		// slot space as kBinding_Textures; UpdateDescriptors mirrors MSAA
		// views here (Texture2DMS-typed shaders only reach those slots).
		getDSLBinding(kBinding_MsaaTextures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_TEXTURES, stageFlags),
	};

	const uint32_t flags =
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
		//| VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
		| VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

	VkDescriptorBindingFlags bindingFlags[kBinding_NumBindings];
	for (int i = 0; i < kBinding_NumBindings; ++i) {
		bindingFlags[i] = flags;
	}
	const VkDescriptorSetLayoutBindingFlagsCreateInfo setLayoutBindingFlagsCI = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT,
		.bindingCount = kBinding_NumBindings,
		.pBindingFlags = bindingFlags,
	};
	const VkDescriptorSetLayoutCreateInfo dslci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = &setLayoutBindingFlagsCI,
		.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT,
		.bindingCount = kBinding_NumBindings,
		.pBindings = bindings,
	};

	VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &defaultSetLayout));

	// Engine Wide PC block (single global range; see kEnginePushConstantSize).
	// The Vulkan guarantee is only 128 bytes; 256 needs a capable device.
	{
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(physicalDevice_, &props);
		const uint32_t devMax = props.limits.maxPushConstantsSize;
		printf("[Vulkan] maxPushConstantsSize=%u, engine requires %u\n",
			devMax, (uint32_t)kEnginePushConstantSize);
		if (devMax < kEnginePushConstantSize) {
			throw std::runtime_error(
				"unsupported push-constant size: engine requires " +
				std::to_string(kEnginePushConstantSize) + " bytes but device supports only " +
				std::to_string(devMax) + " (Vulkan minimum is 128)");
		}
	}
	VkPushConstantRange range{
		.stageFlags = VK_SHADER_STAGE_ALL,
		.offset = 0,
		.size = kEnginePushConstantSize,
	};

	VkPipelineLayoutCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &defaultSetLayout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range
	};
	VK_CHECK(vkCreatePipelineLayout(device_, &ci, nullptr, &defaultPipeLayout));

	// Pool sizing: an update-after-bind pool charges the LAYOUT's maximum
	// counts per allocated set (not the descriptors actually written - the
	// partially-bound bindings do not shrink the allocation charge). The
	// layout carries FOUR sampled-image arrays (Textures, VolumeTextures,
	// Cubemaps, MsaaTextures): 3 * MAX_TEXTURES + MAX_VOLUME_TEXTURES
	// sampled slots PER SET, and the bindless table is REPLICATED PER FRAME
	// (one set per flight - see the allocation below), so every entry
	// scales by MAX_FRAMES_IN_FLIGHT. The old MAX_TEXTURES +
	// MAX_VOLUME_TEXTURES reservation was HALF the single-set requirement -
	// this driver's allocation happened to succeed anyway, but the spec
	// lets it fail (OUT_OF_POOL_MEMORY), and the unchecked call below would
	// have kept NULL sets silently (every frame skips its descriptor
	// updates - UpdateDescriptors skips null sets - and draws bind garbage).
	std::vector<VkDescriptorPoolSize> poolSizes = {
	  {
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    (3 * MAX_TEXTURES + MAX_VOLUME_TEXTURES) * MAX_FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_SAMPLER,          MAX_SAMPLERS * MAX_FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,    MAX_TEXTURES * MAX_FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   2 * MAX_FRAMES_IN_FLIGHT * MAX_FRAMES_IN_FLIGHT }, // engine globals + texture-feedback slices, per set
	  }
	};
	// Device limits for update-after-bind sets: the plain per-set limits
	// do NOT govern UAB pools - the descriptor-indexing properties'
	// update-after-bind variants do (fetched through the properties2
	// pNext chain, like the Vulkan samples do).
	{
		VkPhysicalDeviceDescriptorIndexingPropertiesEXT indexingProps{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES_EXT };
		VkPhysicalDeviceProperties2 props2{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &indexingProps };
		vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);
		const uint32_t needSampled = 3 * MAX_TEXTURES + MAX_VOLUME_TEXTURES;
		if (indexingProps.maxDescriptorSetUpdateAfterBindSampledImages < needSampled
			|| indexingProps.maxDescriptorSetUpdateAfterBindSamplers < MAX_SAMPLERS
			|| indexingProps.maxDescriptorSetUpdateAfterBindStorageImages < MAX_TEXTURES) {
			throw std::runtime_error(
				"device update-after-bind descriptor limits are below the "
				"bindless table requirements (sampled " +
				std::to_string(needSampled) + ", storage " +
				std::to_string(MAX_TEXTURES) + ", samplers " +
				std::to_string(MAX_SAMPLERS) + ")");
		}
		// Pool AGGREGATE: a UAB pool charges every allocated set's layout
		// maxima up front, and the per-set limits above say nothing about
		// the pool total - maxUpdateAfterBindDescriptorsInAllPools is the
		// device-wide budget the replicated table now has to fit.
		const uint64_t needPoolTotal =
			uint64_t(needSampled) + MAX_TEXTURES + MAX_SAMPLERS
			+ 2 * MAX_FRAMES_IN_FLIGHT;
		if (indexingProps.maxUpdateAfterBindDescriptorsInAllPools
			< needPoolTotal * MAX_FRAMES_IN_FLIGHT) {
			throw std::runtime_error(
				"device maxUpdateAfterBindDescriptorsInAllPools (" +
				std::to_string(indexingProps.maxUpdateAfterBindDescriptorsInAllPools)
				+ ") is below the replicated bindless pool requirement ("
				+ std::to_string(needPoolTotal * MAX_FRAMES_IN_FLIGHT) + ")");
		}
	}
	defaultDescriptorPool = CreateDescriptorPool(poolSizes);

	VkDescriptorPool pool = *descriptorPools_.get(defaultDescriptorPool);
	// ONE set PER FLIGHT: the bindless table is frame-invariant, and each
	// set's kBinding_EngineGlobals permanently points at ITS OWN slice of
	// m_engineGlobalsBuffer (written once below). The old single shared
	// set REWROTE the binding every frame - but UPDATE_AFTER_BIND makes
	// post-submit descriptor writes visible to ALREADY-SUBMITTED command
	// buffers, so every in-flight frame ended up reading the NEWEST
	// frame's globals (time / deltaTime / frameIndex) instead of its own
	// slice. Per-flight sets make the binding write-once: a flight's
	// slice address is baked into the set its command buffers bind, and
	// no sibling flight can rewrite it mid-flight.
	VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f)
		layouts[f] = defaultSetLayout;

	VkDescriptorSetAllocateInfo info
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.pNext = nullptr,
		.descriptorPool = pool,
		.descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
		.pSetLayouts = layouts,
	};
	VkResult allocResult = vkAllocateDescriptorSets(
		device_,
		&info,
		defaultDescriptorSets_
	);
	if (allocResult != VK_SUCCESS) {
		fprintf(stderr, "[Vulkan Error] bindless descriptor set allocation failed: %s\n",
			vkResultToString(allocResult));
		throw std::runtime_error("bindless descriptor set allocation failed");
	}

	// Bake each flight's kBinding_EngineGlobals to its own slice, ONCE.
	// FlushEngineGlobals afterwards only uploads fresh values into the
	// (retired) slice - the descriptor side never changes again, so
	// sibling flights still executing keep reading their own slices.
	{
		const Buffer* globalsBuffer = buffers_.get(m_engineGlobalsBuffer);
		assert(globalsBuffer && globalsBuffer->vkBuffer != VK_NULL_HANDLE);
		VkDescriptorBufferInfo bufferInfos[MAX_FRAMES_IN_FLIGHT];
		VkWriteDescriptorSet writes[MAX_FRAMES_IN_FLIGHT];
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			bufferInfos[f].buffer = globalsBuffer->vkBuffer;
			bufferInfos[f].offset = f * sizeof(smvk::EngineGlobals);
			bufferInfos[f].range = sizeof(smvk::EngineGlobals);
			VkWriteDescriptorSet& write = writes[f];
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.pNext = nullptr;
			write.dstSet = defaultDescriptorSets_[f];
			write.dstBinding = smvk::Bindings::kBinding_EngineGlobals;
			write.dstArrayElement = 0;
			write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			write.descriptorCount = 1;
			write.pBufferInfo = &bufferInfos[f];
		}
		vkUpdateDescriptorSets(device_, MAX_FRAMES_IN_FLIGHT, writes, 0, nullptr);
	}

	// ============================================================================


}
smvk::VulkanResourceManager::~VulkanResourceManager() {
	if (device_ != VK_NULL_HANDLE) {
		VkResult waitResult = vkDeviceWaitIdle(device_);
		if (waitResult != VK_SUCCESS && waitResult != VK_ERROR_DEVICE_LOST) {
			fprintf(stderr, "[Vulkan Error] %s:%d: %s\n",
				__FILE__, __LINE__, vkResultToString(waitResult));
			abort();
		}
		if (waitResult == VK_ERROR_DEVICE_LOST) {
			fprintf(stderr, "[Vulkan Warning] vkDeviceWaitIdle returned VK_ERROR_DEVICE_LOST during ~VulkanResourceManager, proceeding with teardown\n");
		}
	}

	destroy(this, m_nilTexture);
	// The nil VIEW died with the nil texture above (its ImageView destroy
	// runs inline here — DeferDestroy has no semaphore left to key on;
	// the nil entry's own free inside destroy() above still wrote with the
	// then-live view, which is harmless: the sets die with the pool
	// below). Clear the cache so all LATER teardown-time frees (leaked
	// Holders in the member teardown below) skip the freed-slot descriptor
	// write instead of writing the now-dead view.
	m_nullImageView = VK_NULL_HANDLE;
	destroy(this, m_engineGlobalsBuffer);
	destroy(this, m_readbackStaging);
	destroy(this, m_asyncStaging);

	// Flush, not Process: queued tasks may reference the swapchain timeline
	// semaphore, which ~VulkanSwapchain already destroyed (member order).
	// The device is idle, so every retire point is trivially satisfied and no
	// readiness query (which would touch the dead semaphore) may run.
	FlushDeferredTasks();
	// StaticVertexFmt
	destroy(this, staticVertexFmtHeap.m_staticVertexPosBuffer);
	destroy(this, staticVertexFmtHeap.m_staticVertexShadingBuffer);
	destroy(this, staticVertexFmtHeap.m_staticIndexBuffer);
	// The arenas (the VmaVirtualBlock replacement) are lifetime-global CPU
	// state: drop the bookkeeping, nothing device-side remains.
	staticVertexFmtHeap.m_staticVertexBlock.Initialize(0);
	staticVertexFmtHeap.m_staticIndexBlock.Initialize(0);
	//
	if (m_streamer) { m_streamer->waitAll(); m_streamer->destroy(); m_streamer.reset(); }
	// Unconditional drain: resources freed during Shutdown (after the last
	// ProcessDeferrredTasks) plus the static buffers below must still be
	// released. Device is idle, and Flush never queries semaphores, so this is
	// safe even though the swapchain timeline is already gone (member order).
	FlushDeferredTasks();
	if (m_transferSubmitter && m_transferSubmitter != m_submitter) { m_transferSubmitter->~Submitter(); } // linear-owned: dtor only
	m_transferSubmitter = nullptr;
	if (m_computeSubmitter && m_computeSubmitter != m_submitter) { m_computeSubmitter->~Submitter(); } // linear-owned: dtor only
	m_computeSubmitter = nullptr;
	m_submitter->~Submitter(); // linear-owned: dtor only, never freed
	m_submitter = nullptr;

#ifndef N_DEBUG
	bool leakHeaderPrinted = false;

	auto printLeakHeader = [&]() {
		if (!leakHeaderPrinted)
		{
			std::cout << "[ResourceContext::~ResourceContext] Leaking objects:\n";
			leakHeaderPrinted = true;
		}
	};

	shaderModules_.forEachHandle([&](ShaderModuleHandle handle) {
		printLeakHeader();

		auto sm = shaderModules_.get(handle);
		std::cout << "  [LEAK] ShaderModule: handle index=" << handle.index()
			<< ", gen=" << handle.gen() << ", module=" << sm->module << "\n";
	});

	buffers_.forEachHandle([&](BufferHandle handle) {
		printLeakHeader();

		auto buf = buffers_.get(handle);
		std::cout << "  [LEAK] Buffer: handle index=" << handle.index()
			<< ", gen=" << handle.gen() << ", buffer=" << buf->vkBuffer
			<< ", allocation=" << buf->vma_alloc << "\n";
	});

	samplers_.forEachHandle([&](SamplerHandle handle) {
		printLeakHeader();

		auto s = samplers_.get(handle);
		std::cout << "  [LEAK] Sampler: handle index=" << handle.index()
			<< ", gen=" << handle.gen() << ", sampler=" << *s << "\n";
	});

	descriptorPools_.forEachHandle([&](DescriptorPoolHandle handle) {
		if (handle != defaultDescriptorPool)
		{
			printLeakHeader();

			auto dp = descriptorPools_.get(handle);
			std::cout << "  [LEAK] DescriptorPool: handle index=" << handle.index()
				<< ", gen=" << handle.gen() << ", pool=" << *dp << "\n";
		}
	});

	images_.forEachHandle([&](ImageHandle handle) {
		printLeakHeader();

		auto img = images_.get(handle);
		std::cout << "  [LEAK] Image: handle index=" << handle.index()
			<< ", gen=" << handle.gen() << ", image=" << img->vkImage
			<< ", allocation=" << img->allocation << ", views="
			<< img->liveViewCount << "\n";
	});

	imageViews_.forEachHandle([&](ImageViewHandle handle)
	{
		printLeakHeader();

		auto* view = imageViews_.get(handle);

		std::cout
			<< "  [LEAK] ImageView: index="
			<< handle.index()
			<< ", gen=" << handle.gen()
			<< ", VkImageView=" << view->vkView
			<< "\n";
	});

	textures_.forEachHandle([&](TextureHandle handle) {
		printLeakHeader();

		auto tex = textures_.get(handle);
		std::cout << "  [LEAK] Texture: handle index=" << handle.index()
			<< ", gen=" << handle.gen() << ", image=" << tex->image.empty()
			<< "\n";
	});
#endif

	vkDestroyPipelineLayout(device_, defaultPipeLayout, nullptr);
	vkDestroyDescriptorSetLayout(device_, defaultSetLayout, nullptr);
	// Samplers are owned by samplerCache – clear it and avoid double-free
	if (samplerCache_) {
		samplerCache_->Clear();
		samplerCache_.reset();
		m_defaultSampler = VK_NULL_HANDLE;
		m_defaultShadowSampler = VK_NULL_HANDLE;
	}
	if (m_defaultSampler != VK_NULL_HANDLE) {
		vkDestroySampler(device_, m_defaultSampler, nullptr);
	}
	if (m_defaultShadowSampler != VK_NULL_HANDLE) {
		vkDestroySampler(device_, m_defaultShadowSampler, nullptr);
	}
	memset(defaultDescriptorSets_, 0, sizeof(defaultDescriptorSets_));
	//
	vmaDestroyAllocator(m_allocator);
};

// ===============================================================
// UploadStreamer implementation
// ===============================================================
smvk::UploadStreamer::UploadStreamer(VulkanResourceManager* rm, Submitter* transfer, Submitter* graphics)
	: m_rm(rm), m_transfer(transfer), m_graphics(graphics) {
	if (rm) m_hostCopySupported = rm->m_hostImageCopySupported;
}
smvk::UploadStreamer::~UploadStreamer() { destroy(); }
bool smvk::UploadStreamer::init(VkDeviceSize ringCapacity) {
	// Total budget split across shards; each thread lazily creates its own
	// ring on first upload so unused shards cost nothing.
	ringCapacityPerThread_ = ringCapacity / 8;
	if (ringCapacityPerThread_ < smvk::mem::config::Runtime().stagingRingPerThread)
		ringCapacityPerThread_ = smvk::mem::config::Runtime().stagingRingPerThread;
	return ensureShardRing(states_[0]);
}
bool smvk::UploadStreamer::ensureRing(ThreadUploadState& ust, VkDeviceSize capacity, const char* debugName) {
	if (ust.ring.buffer != VK_NULL_HANDLE) return true;
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = capacity;
	ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	VmaAllocationInfo info{};
	VkResult r = vmaCreateBuffer(m_rm->m_allocator, &ci, &aci, &ust.ring.buffer, &ust.ring.allocation, &info);
	// OOM policy: VMA staging failure crashes with a diagnostic — no silent
	// fallback (a "successful" upload path with no ring would corrupt data).
	if (r != VK_SUCCESS) {
		std::fprintf(stderr,
			"[OOM] VMA staging ring '%s' (%llu MB) creation failed (VkResult %d). "
			"No recovery — aborting.\n",
			debugName, (unsigned long long)(capacity / (1024 * 1024)), (int)r);
		std::fflush(stderr);
		std::abort();
	}
	ust.ring.mapped = info.pMappedData;
	ust.ring.capacity = capacity;
	ust.ring.offset = 0;
#if defined(_DEBUG)
	{
		VkDebugUtilsObjectNameInfoEXT ni{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
		ni.objectType = VK_OBJECT_TYPE_BUFFER;
		ni.objectHandle = reinterpret_cast<uint64_t>(ust.ring.buffer);
		ni.pObjectName = debugName;
		SetDebugObjectName(device_, &ni);
	}
#endif
	return true;
}
bool smvk::UploadStreamer::ensureShardRing(ThreadUploadState& ust) {
	return ensureRing(ust, ringCapacityPerThread_, "UploadStreamer Ring (thread shard)");
}
void smvk::UploadStreamer::destroy() {
	if (bulk_.curCmd != VK_NULL_HANDLE && bulk_.hasPending) {
		submitState(bulk_, false);
	}
	for (auto& ust : states_) {
		if (ust.curCmd != VK_NULL_HANDLE && ust.hasPending) {
			// submit any pending work before destroy
			submitState(ust, false);
		}
	}
	if (m_transfer) m_transfer->waitAll();
	if (m_graphics && m_graphics != m_transfer) m_graphics->waitAll();
	if (bulk_.ring.buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(m_rm->m_allocator, bulk_.ring.buffer, bulk_.ring.allocation);
		bulk_.ring.buffer = VK_NULL_HANDLE;
		bulk_.ring.allocation = VK_NULL_HANDLE;
		bulk_.ring.mapped = nullptr;
		bulk_.ring.capacity = 0;
		bulk_.ring.offset = 0;
	}
	bulk_.curCmd = VK_NULL_HANDLE;
	bulk_.curHandle = {};
	bulk_.hasPending = false;
	for (auto& ust : states_) {
		if (ust.ring.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(m_rm->m_allocator, ust.ring.buffer, ust.ring.allocation);
			ust.ring.buffer = VK_NULL_HANDLE;
			ust.ring.allocation = VK_NULL_HANDLE;
			ust.ring.mapped = nullptr;
			ust.ring.capacity = 0;
			ust.ring.offset = 0;
		}
		ust.curCmd = VK_NULL_HANDLE;
		ust.curHandle = {};
		ust.hasPending = false;
	}
}
VkDeviceSize smvk::UploadStreamer::allocFromRing(ThreadUploadState& ust, VkDeviceSize size, VkDeviceSize alignment) {
	// st is owned by the calling thread – no lock needed.
	if (!ensureShardRing(ust)) return VK_WHOLE_SIZE;
	VkDeviceSize aligned = (ust.ring.offset + alignment - 1) & ~(alignment - 1);
	if (aligned + size > ust.ring.capacity) {
		// need to flush current batch and wrap
		if (ust.hasPending) submitState(ust, true);
		if (size > ust.ring.capacity) return VK_WHOLE_SIZE; // signal fallback to temp staging
		ust.ring.offset = 0;
		aligned = 0;
		if (aligned + size > ust.ring.capacity) return VK_WHOLE_SIZE;
	}
	// Precise region sync (was: m_transfer->waitAll() at every wrap).
	// The CPU memcpy below overwrites [aligned, aligned+size) of the
	// host-mapped ring, so exactly the in-flight batches READING that
	// range must have executed first. Retired batches (the common case —
	// ranges cover recent ring bytes and retire ~one frame after submit)
	// drop out via isReady; the batch just flushed at a wrap reads the
	// ring TAIL, which only overlaps when the rewound region reaches it.
	// The old waitAll() blocked the render thread for a full frame of
	// graphics latency at every wrap, because the just-submitted batch
	// GPU-waits on the previous frame's retirement.
	if (!ust.pendingRanges.empty()) {
		SMVK_PROFILE_STREAM("UploadRing_InFlightSync");
		SMVK_PROFILE_TAG("ranges", ust.pendingRanges.size());
		SMVK_PROFILE_TAG("stagingKB", (size + 1023) / 1024);
		const bool ringTrace = std::getenv("TOURMALINE_TEXSTREAM_TRACE") != nullptr;
		const auto ringT0 = std::chrono::steady_clock::now();
		double waitMs = 0.0;
		// FrameScratch: scope-local filter temp (dies before return —
		// safe under the DrawFrame rewind; must NOT be UploadInFlight).
		auto live = MakeFrameScratchVec<InFlightRingRange>();
		live.reserve(ust.pendingRanges.size());
		for (const InFlightRingRange& r : ust.pendingRanges) {
			const bool overlaps = (r.start < aligned + size) && (r.end > aligned);
			const bool ready = m_transfer->isReady(r.handle);
			if (overlaps && !ready) {
				const auto waitT0 = std::chrono::steady_clock::now();
				m_transfer->wait(r.handle);
				waitMs += std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - waitT0).count();
			}
			if (!ready)
				live.push_back(r);
		}
		if (ringTrace && (std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - ringT0).count() > 0.5)) {
			std::fprintf(stderr,
				"[TexStream]   ringSync %zu ranges, staged %.1f KB: sync %.2f ms (GPU wait %.2f ms)\n",
				ust.pendingRanges.size(), size / 1024.0,
				std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - ringT0).count(),
				waitMs);
			std::fflush(stderr);
		}
		ust.pendingRanges.clear();
		for (const auto& r : live)
			ust.pendingRanges.push_back(r);
	}
	if (ust.batchRingStart == VK_WHOLE_SIZE)
		ust.batchRingStart = aligned;
	ust.ring.offset = aligned + size;
	return aligned;
}
void smvk::UploadStreamer::ensureCmd(ThreadUploadState& ust) {
	// st is owned by the calling thread – no lock needed.
	if (ust.curCmd != VK_NULL_HANDLE) return;
	// purge already done in acquire; acquire uses the thread-local command
	// pool (GetThreadLocalPool) so concurrent acquires are lock-free – only
	// the Submitter timeline counter takes a brief lock inside.
	ust.curHandle = m_transfer->acquire();
	ust.curCmd = m_transfer->GetCommandBuffer(ust.curHandle);
	ust.hasPending = false;
}
void smvk::UploadStreamer::recordSubmittedRange(ThreadUploadState& ust) {
	// Called right after ust.curHandle was submitted: remember which ring
	// bytes the batch reads so allocFromRing can wait exactly this batch
	// when a future alloc overlaps them (instead of a full waitAll()).
	if (ust.batchRingStart == VK_WHOLE_SIZE || ust.ring.offset <= ust.batchRingStart)
		ust.batchRingStart = VK_WHOLE_SIZE; // empty batch: nothing to track
	else {
		ust.pendingRanges.push_back(InFlightRingRange{
			ust.batchRingStart, ust.ring.offset, ust.curHandle });
		ust.batchRingStart = VK_WHOLE_SIZE;
	}
}
smvk::TimelinePoint smvk::UploadStreamer::submitTransfer(CommandSubmitHandle h) {
	// Serial chain: wait every earlier submission on this Submitter's
	// timeline (upload batches + chained external points such as the
	// async readback copy). Same-queue submissions only respect
	// submission (begin) order per spec and may overlap — without this,
	// a zero-fill batch can overwrite a histogram/ds-counter/feedback
	// slice while an earlier-submitted readback copy is still reading it
	// (the WRITE_AFTER_READ / WRITE_AFTER_WRITE hazards sync validation
	// reports on those buffers). Values on one timeline are monotonic,
	// so waiting the newest point orders the whole chain.
	const TimelinePoint chainPrev = lastTransferPoint();
	TimelinePoint tp{};
	if (m_transfer != m_graphics
		&& m_rm->m_lastGraphicsSemaphore != VK_NULL_HANDLE
		&& m_rm->m_lastGraphicsValue != 0) {
		const TimelineWait waits[2]{
			{ TimelinePoint{ m_rm->m_lastGraphicsSemaphore, m_rm->m_lastGraphicsValue },
			  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
			{ chainPrev, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT }
		};
		const uint32_t waitCount = chainPrev.valid() ? 2 : 1;
		tp = m_transfer->submit(h, std::span<const TimelineWait>(waits, waitCount));
	}
	else if (chainPrev.valid()) {
		const TimelineWait waits[1]{
			{ chainPrev, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT }
		};
		tp = m_transfer->submit(h, std::span<const TimelineWait>(waits, 1));
	}
	else {
		tp = m_transfer->submit(h);
	}
	// Publish the newest transfer point for lastTransferPoint() — the
	// swapchain's G1/G2/main submits wait it so uploaded data is a
	// semaphore-ordered dependency (same-queue submissions may overlap).
	if (tp.valid()) {
		m_lastTransferSemaphore = tp.semaphore;
		uint64_t observed = m_lastTransferValue.load(std::memory_order_relaxed);
		while (tp.value > observed
			&& !m_lastTransferValue.compare_exchange_weak(
				observed, tp.value, std::memory_order_release)) {
		}
	}
	return tp;
}
void smvk::UploadStreamer::submitState(ThreadUploadState& ust, bool waitOldestIfFull) {
	// st is owned by the calling thread – no streamer lock. The Submitter
	// and queue take their own brief locks internally. Cross-queue safety:
	// see submitTransfer (the batch waits for every submitted graphics
	// frame to retire before overwriting upload targets).
	if (!ust.hasPending || ust.curCmd == VK_NULL_HANDLE) {
		if (ust.curCmd != VK_NULL_HANDLE) {
			auto tp = submitTransfer(ust.curHandle);
			recordSubmittedRange(ust);
			for (auto& task : ust.pendingCleanups) m_rm->AddDeferredTask(std::move(task), tp);
			ust.pendingCleanups.clear();
		}
		ust.curCmd = VK_NULL_HANDLE;
		ust.curHandle = {};
		ust.hasPending = false;
		ust.pendingBufferAcquires.clear();
		ust.pendingImageAcquires.clear();
		ust.stagedBytesSinceFlush = 0;
		return;
	}
	auto tp = submitTransfer(ust.curHandle);
	recordSubmittedRange(ust);
	for (auto& task : ust.pendingCleanups) m_rm->AddDeferredTask(std::move(task), tp);
	ust.pendingCleanups.clear();
	// EXCLUSIVE ownership: submit acquire barriers on graphics queue waiting on transfer timeline
	if (m_transfer != m_graphics && (!ust.pendingBufferAcquires.empty() || !ust.pendingImageAcquires.empty())) {
		auto gHan = m_graphics->acquire();
		auto gCmd = m_graphics->GetCommandBuffer(gHan);
		VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, (uint32_t)ust.pendingBufferAcquires.size(), ust.pendingBufferAcquires.empty() ? nullptr : ust.pendingBufferAcquires.data(), (uint32_t)ust.pendingImageAcquires.size(), ust.pendingImageAcquires.empty() ? nullptr : ust.pendingImageAcquires.data() };
		vkCmdPipelineBarrier2(gCmd, &dep);
		TimelineWait wait{ tp, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
		m_graphics->submit(gHan, std::span<const TimelineWait>(&wait, 1));
	}
	ust.pendingBufferAcquires.clear();
	ust.pendingImageAcquires.clear();
	ust.curCmd = VK_NULL_HANDLE;
	ust.curHandle = {};
	ust.hasPending = false;
	ust.stagedBytesSinceFlush = 0;
}
bool smvk::UploadStreamer::hasAnyPending() const noexcept {
	if (bulk_.hasPending) return true;
	for (auto& ust : states_) if (ust.hasPending) return true;
	return false;
}
// Flush-contract producer guard (see ThreadUploadState::active): marks the
// shard ACTIVE for the whole recording; the cross-shard drains assert
// quiescence. The exchange assert doubles as the same-shard-two-threads
// check (a pool mis-assignment handing two live threads one shard).
// Debug builds only (assert): the Release contract is documentation +
// call-site discipline (workers joined / quiescent before flush()).
struct UploadShardActiveGuard {
	std::atomic<bool>& a;
	explicit UploadShardActiveGuard(std::atomic<bool>& a_) : a(a_) {
		assert(!a.exchange(true, std::memory_order_acq_rel)
			&& "upload shard entered by two threads (pool shard mis-assignment?)");
	}
	~UploadShardActiveGuard() { a.store(false, std::memory_order_release); }
};
void smvk::UploadStreamer::uploadBuffer(BufferHandle dst, const void* data, size_t size, size_t dstOffset,
	VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
	if (!data || size == 0 || dst.empty()) return;
	// fast path for host-visible buffers: direct write + barrier on graphics queue
	{
		Buffer* b = m_rm->buffers_.get(dst);
		if (b && b->isMappable()) {
			VK_CHECK(vmaCopyMemoryToAllocation(m_rm->m_allocator, data, b->vma_alloc, dstOffset, size));
			// barrier on graphics submitter (or transfer if shared, but graphics is fine)
			auto han = m_graphics->acquire();
			auto cmd = m_graphics->GetCommandBuffer(han);
			// BufferBarrier forward
			BufferBarrier(cmd, b->vkBuffer, dstOffset, size, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, dstStages, dstAccess);
			m_graphics->submit(han);
			VulkanResourceManager::BufferGlobalSyncState st{};
			st.masks.stageMask = FromVkPipelineStage(dstStages);
			if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess));
			st.masks.accessMask = FromVkAccess(dstAccess);
			m_rm->SetBufferSyncState(dst, st);
			return;
		}
	}
	ThreadUploadState& ust = states_[CurrentPoolShard()];
	UploadShardActiveGuard shardGuard{ ust.active };
	Buffer* dstBuf = m_rm->buffers_.get(dst);
	if (!dstBuf) return;
	// allocate from ring
	VkDeviceSize ringOff = allocFromRing(ust, size, 16);
	if (ringOff == VK_WHOLE_SIZE) {
		// fallback temporary staging (ring too small)
		VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		ci.size = size;
		ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		VkBuffer tmpBuf; VmaAllocation tmpAlloc; VmaAllocationInfo tmpInfo{};
		VK_CHECK(vmaCreateBuffer(m_rm->m_allocator, &ci, &aci, &tmpBuf, &tmpAlloc, &tmpInfo));
		memcpy(tmpInfo.pMappedData, data, size);
		vmaFlushAllocation(m_rm->m_allocator, tmpAlloc, 0, VK_WHOLE_SIZE);
		ensureCmd(ust);
		VkBufferCopy region{ 0, dstOffset, size };
		vkCmdCopyBuffer(ust.curCmd, tmpBuf, dstBuf->vkBuffer, 1, &region);
		// BufferBarrier forward
		uint32_t srcFamily = m_transfer->queueFamilyIndex();
		uint32_t dstFamily = m_graphics->queueFamilyIndex();
		if (srcFamily != dstFamily) {
			// Queue-family ownership transfer for EXCLUSIVE targets; CONCURRENT
			// targets must use IGNORED families (specialisation barriers are
			// only legal on queue-family-exclusive resources).
			const uint32_t relSrc = dstBuf->isConcurrent ? VK_QUEUE_FAMILY_IGNORED : srcFamily;
			const uint32_t acqDst = dstBuf->isConcurrent ? VK_QUEUE_FAMILY_IGNORED : dstFamily;
			VkBufferMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, relSrc, acqDst, dstBuf->vkBuffer, dstOffset, size };
			VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,0,nullptr,1,&rel,0,nullptr };
			vkCmdPipelineBarrier2(ust.curCmd, &dep);
			VkBufferMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, 0, 0, dstStages, dstAccess, relSrc, acqDst, dstBuf->vkBuffer, dstOffset, size };
			ust.pendingBufferAcquires.push_back(acq);
		}
		else {
			BufferBarrier(ust.curCmd, dstBuf->vkBuffer, dstOffset, size, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStages, dstAccess);
		}
		ust.hasPending = true;
		// defer staging destruction until transfer batch is submitted (use TimelinePoint)
		ust.pendingCleanups.push_back(std::packaged_task<void()>([rm = m_rm, buf = tmpBuf, alloc = tmpAlloc] { vmaDestroyBuffer(rm->m_allocator, buf, alloc); }));
		VulkanResourceManager::BufferGlobalSyncState st{}; st.masks.stageMask = FromVkPipelineStage(dstStages); if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess)); st.masks.accessMask = FromVkAccess(dstAccess); m_rm->SetBufferSyncState(dst, st);
		ust.stagedBytesSinceFlush += size;
		if (ust.stagedBytesSinceFlush >= flushThreshold_) {
			submitState(ust, false);
		}
		return;
	}
	// normal ring path
	memcpy(static_cast<char*>(ust.ring.mapped) + ringOff, data, size);
	// flush if non-coherent (ring is host_visible, likely coherent; still flush)
	vmaFlushAllocation(m_rm->m_allocator, ust.ring.allocation, ringOff, size);
	ensureCmd(ust);
	VkBufferCopy region{ ringOff, dstOffset, size };
	vkCmdCopyBuffer(ust.curCmd, ust.ring.buffer, dstBuf->vkBuffer, 1, &region);
	// BufferBarrier forward
	uint32_t srcFamily = m_transfer->queueFamilyIndex();
	uint32_t dstFamily = m_graphics->queueFamilyIndex();
	if (srcFamily != dstFamily) {
		// Queue-family ownership transfer for EXCLUSIVE targets; CONCURRENT
		// targets must use IGNORED families (specialisation barriers are
		// only legal on queue-family-exclusive resources).
		const uint32_t relSrc = dstBuf->isConcurrent ? VK_QUEUE_FAMILY_IGNORED : srcFamily;
		const uint32_t acqDst = dstBuf->isConcurrent ? VK_QUEUE_FAMILY_IGNORED : dstFamily;
		VkBufferMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, relSrc, acqDst, dstBuf->vkBuffer, dstOffset, size };
		VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,0,nullptr,1,&rel,0,nullptr };
		vkCmdPipelineBarrier2(ust.curCmd, &dep);
		VkBufferMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, 0, 0, dstStages, dstAccess, relSrc, acqDst, dstBuf->vkBuffer, dstOffset, size };
		ust.pendingBufferAcquires.push_back(acq);
	}
	else {
		BufferBarrier(ust.curCmd, dstBuf->vkBuffer, dstOffset, size, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStages, dstAccess);
	}
	ust.hasPending = true;
	VulkanResourceManager::BufferGlobalSyncState st{}; st.masks.stageMask = FromVkPipelineStage(dstStages); if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess)); st.masks.accessMask = FromVkAccess(dstAccess); m_rm->SetBufferSyncState(dst, st);
	ust.stagedBytesSinceFlush += size;
	if (ust.stagedBytesSinceFlush >= flushThreshold_) {
		submitState(ust, false);
	}
	// if we are batching many small uploads, don't submit yet – caller will flush
}
void smvk::UploadStreamer::uploadBufferBulk(BufferHandle dst, const void* content,
	std::span<const BufferBulkCopy> copies, VkPipelineStageFlags2 dstStages,
	VkAccessFlags2 dstAccess) {
	Buffer* dstBuf = m_rm->buffers_.get(dst);
	if (!dstBuf || !content || copies.empty()) return;
	// Host-mappable targets never take the bulk ring: per-entry fast path
	// (the heaps this serves are device-local, but stay correct for any dst).
	if (dstBuf->isMappable()) {
		for (const BufferBulkCopy& c : copies)
			if (c.size != 0)
				uploadBuffer(dst, static_cast<const char*>(content) + c.srcOffset,
					c.size, c.dstOffset, dstStages, dstAccess);
		return;
	}
	if (!ensureBulkRing()) return;
	ThreadUploadState& ust = bulk_;
	UploadShardActiveGuard shardGuard{ ust.active };
	VkDeviceSize chunkBytes = 0; // staged bytes in the current blob
	VkDeviceSize chunkDstMin = std::numeric_limits<VkDeviceSize>::max();
	VkDeviceSize chunkDstMax = 0; // exclusive end of the chunk's dst span
	VkDeviceSize chunkRingMin = std::numeric_limits<VkDeviceSize>::max();
	VkDeviceSize chunkRingMax = 0; // exclusive end of the chunk's ring span
	auto closeChunk = [&]() {
		if (!ust.hasPending || ust.curCmd == VK_NULL_HANDLE) return;
		// A chunk can also hold only temp-staging fallback entries (those
		// carry self-contained barriers); the chunk barrier + flush apply
		// only when ring entries are present.
		const bool haveChunk = chunkDstMin != std::numeric_limits<VkDeviceSize>::max()
			&& chunkDstMax > chunkDstMin;
		// ONE barrier set per blob: a single release barrier covering the
		// chunk's whole dst span (gaps included — legal, and one barrier
		// instead of one per entry) recorded in the transfer batch, plus
		// the matching acquire queued for the graphics follow-up submit.
		if (haveChunk) {
			const VkDeviceSize dstSpan = chunkDstMax - chunkDstMin;
			const uint32_t srcFamily = m_transfer->queueFamilyIndex();
			const uint32_t dstFamily = m_graphics->queueFamilyIndex();
			if (srcFamily != dstFamily) {
				const uint32_t relSrc = dstBuf->isConcurrent ? VK_QUEUE_FAMILY_IGNORED : srcFamily;
				const uint32_t acqDst = dstBuf->isConcurrent ? VK_QUEUE_FAMILY_IGNORED : dstFamily;
				VkBufferMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr,
					VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0,
					relSrc, acqDst, dstBuf->vkBuffer, chunkDstMin, dstSpan };
				VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 1, &rel, 0, nullptr };
				vkCmdPipelineBarrier2(ust.curCmd, &dep);
				VkBufferMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr,
					0, 0, dstStages, dstAccess, relSrc, acqDst,
					dstBuf->vkBuffer, chunkDstMin, dstSpan };
				ust.pendingBufferAcquires.push_back(acq);
			}
			else {
				BufferBarrier(ust.curCmd, dstBuf->vkBuffer, chunkDstMin, dstSpan,
					VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
					dstStages, dstAccess);
			}
			// One non-coherent flush covering the chunk's ring span instead
			// of one per entry.
			if (chunkRingMin != std::numeric_limits<VkDeviceSize>::max())
				vmaFlushAllocation(m_rm->m_allocator, ust.ring.allocation,
					chunkRingMin, chunkRingMax - chunkRingMin);
		}
		submitState(ust, false);
		chunkBytes = 0;
		chunkDstMin = std::numeric_limits<VkDeviceSize>::max();
		chunkDstMax = 0;
		chunkRingMin = std::numeric_limits<VkDeviceSize>::max();
		chunkRingMax = 0;
	};
	for (const BufferBulkCopy& c : copies) {
		if (c.size == 0) continue;
		// Blob boundary: when the chunk is full (512 MB) or the next entry
		// would not fit the remaining ring space, close it FIRST — the
		// barrier must be recorded before whatever submits this batch, and
		// closing here keeps chunk boundary == submit boundary (allocFromRing's
		// wrap then finds hasPending == false and only rewinds).
		const VkDeviceSize aligned = (ust.ring.offset + 15) & ~VkDeviceSize(15);
		if (ust.hasPending
			&& (chunkBytes >= bulkRingCapacity_
				|| aligned + c.size > ust.ring.capacity))
			closeChunk();
		const VkDeviceSize ringOff = allocFromRing(ust, c.size, 16);
		if (ringOff == VK_WHOLE_SIZE) {
			// Entry bigger than the whole ring: per-entry temp staging
			// (self-contained barrier + deferred destroy; mirrors
			// uploadBuffer's fallback). Rare — a single mesh payload over
			// 512 MB.
			VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			ci.size = c.size;
			ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo aci{};
			aci.usage = VMA_MEMORY_USAGE_AUTO;
			aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
			VkBuffer tmpBuf; VmaAllocation tmpAlloc; VmaAllocationInfo tmpInfo{};
			VK_CHECK(vmaCreateBuffer(m_rm->m_allocator, &ci, &aci, &tmpBuf, &tmpAlloc, &tmpInfo));
			memcpy(tmpInfo.pMappedData, static_cast<const char*>(content) + c.srcOffset, c.size);
			vmaFlushAllocation(m_rm->m_allocator, tmpAlloc, 0, VK_WHOLE_SIZE);
			ensureCmd(ust);
			VkBufferCopy region{ 0, c.dstOffset, c.size };
			vkCmdCopyBuffer(ust.curCmd, tmpBuf, dstBuf->vkBuffer, 1, &region);
			const uint32_t srcFamily = m_transfer->queueFamilyIndex();
			const uint32_t dstFamily = m_graphics->queueFamilyIndex();
			if (srcFamily != dstFamily) {
				const uint32_t relSrc = dstBuf->isConcurrent ? VK_QUEUE_FAMILY_IGNORED : srcFamily;
				const uint32_t acqDst = dstBuf->isConcurrent ? VK_QUEUE_FAMILY_IGNORED : dstFamily;
				VkBufferMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, relSrc, acqDst, dstBuf->vkBuffer, c.dstOffset, c.size };
				VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,0,nullptr,1,&rel,0,nullptr };
				vkCmdPipelineBarrier2(ust.curCmd, &dep);
				VkBufferMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, 0, 0, dstStages, dstAccess, relSrc, acqDst, dstBuf->vkBuffer, c.dstOffset, c.size };
				ust.pendingBufferAcquires.push_back(acq);
			}
			else {
				BufferBarrier(ust.curCmd, dstBuf->vkBuffer, c.dstOffset, c.size, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStages, dstAccess);
			}
			ust.hasPending = true;
			ust.pendingCleanups.push_back(std::packaged_task<void()>([rm = m_rm, buf = tmpBuf, alloc = tmpAlloc] { vmaDestroyBuffer(rm->m_allocator, buf, alloc); }));
			continue;
		}
		memcpy(static_cast<char*>(ust.ring.mapped) + ringOff,
			static_cast<const char*>(content) + c.srcOffset, c.size);
		ensureCmd(ust);
		VkBufferCopy region{ ringOff, c.dstOffset, c.size };
		vkCmdCopyBuffer(ust.curCmd, ust.ring.buffer, dstBuf->vkBuffer, 1, &region);
		ust.hasPending = true;
		chunkBytes += c.size;
		chunkDstMin = std::min(chunkDstMin, VkDeviceSize(c.dstOffset));
		chunkDstMax = std::max(chunkDstMax, VkDeviceSize(c.dstOffset) + VkDeviceSize(c.size));
		chunkRingMin = std::min(chunkRingMin, ringOff);
		chunkRingMax = std::max(chunkRingMax, ringOff + VkDeviceSize(c.size));
	}
	closeChunk();
	VulkanResourceManager::BufferGlobalSyncState st{};
	st.masks.stageMask = FromVkPipelineStage(dstStages);
	if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess));
	st.masks.accessMask = FromVkAccess(dstAccess);
	m_rm->SetBufferSyncState(dst, st);
}
bool smvk::UploadStreamer::tryHostImageCopy(VkImage image, VkFormat format, VkExtent3D extent, uint32_t mipLevels, uint32_t arrayLayers, const void* data, size_t dataSize, const std::vector<VkBufferImageCopy>& regions, VkImageLayout finalLayout) {
	if (!m_hostCopySupported) return false;
	if (!vkCopyMemoryToImageEXT || !vkTransitionImageLayoutEXT) return false;
	if (!IsHostTransferSupported(format)) return false;
	if (extent.depth != 1) return false; // fallback for 3D for now
	// Spec: newLayout must be in VkPhysicalDeviceHostImageCopyPropertiesEXT::pCopyDstLayouts.
	// Only GENERAL is guaranteed; e.g. SHADER_READ_ONLY_OPTIMAL must be checked.
	if (m_rm && !m_rm->IsHostCopyDstLayoutSupported(finalLayout)) return false;
	// Transition UNDEFINED -> GENERAL
	VkHostImageLayoutTransitionInfoEXT trans1{
		VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT,
		nullptr,
		image,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_GENERAL,
		VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, arrayLayers }
	};
	VkResult r = vkTransitionImageLayoutEXT(device_, 1, &trans1);
	if (r != VK_SUCCESS) return false;

	const FormatInfo fmt = getFormatInfo(format);
	std::vector<VkMemoryToImageCopyEXT> copies;
	copies.reserve((size_t)mipLevels * arrayLayers);
	VkDeviceSize offset = 0;
	for (uint32_t mip = 0; mip < mipLevels; ++mip) {
		uint32_t w = std::max(1u, extent.width >> mip);
		uint32_t h = std::max(1u, extent.height >> mip);
		uint32_t blocksX = (w + fmt.blockWidth - 1) / fmt.blockWidth;
		uint32_t blocksY = (h + fmt.blockHeight - 1) / fmt.blockHeight;
		VkDeviceSize layerSize = (VkDeviceSize)blocksX * blocksY * fmt.bytesPerBlock;
		for (uint32_t layer = 0; layer < arrayLayers; ++layer) {
			if (offset + layerSize > dataSize) return false; // bounds check
			VkMemoryToImageCopyEXT cpy{};
			cpy.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT;
			cpy.pHostPointer = static_cast<const char*>(data) + offset;
			cpy.memoryRowLength = 0;
			cpy.memoryImageHeight = 0;
			cpy.imageSubresource = VkImageSubresourceLayers{ VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 };
			cpy.imageOffset = VkOffset3D{ 0,0,0 };
			cpy.imageExtent = VkExtent3D{ w,h,1 };
			copies.push_back(cpy);
			offset += layerSize;
		}
	}
	if (copies.empty()) return false;
	VkCopyMemoryToImageInfoEXT copyInfo{
		VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT,
		nullptr,
		//VK_HOST_IMAGE_COPY_MEMCPY_BIT_EXT,
		0,
		image,
		VK_IMAGE_LAYOUT_GENERAL,
		(uint32_t)copies.size(),
		copies.data()
	};
	r = vkCopyMemoryToImageEXT(device_, &copyInfo);
	if (r != VK_SUCCESS) {
		// try without MEMCPY flag
		copyInfo.flags = 0;
		r = vkCopyMemoryToImageEXT(device_, &copyInfo);
		if (r != VK_SUCCESS) return false;
	}
	// Transition GENERAL -> finalLayout
	VkHostImageLayoutTransitionInfoEXT trans2{
		VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT,
		nullptr,
		image,
		VK_IMAGE_LAYOUT_GENERAL,
		finalLayout,
		VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, arrayLayers }
	};
	r = vkTransitionImageLayoutEXT(device_, 1, &trans2);
	return r == VK_SUCCESS;
}
void smvk::UploadStreamer::uploadImage(ImageHandle imageHandle, const void* data, size_t dataSize, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
	if (!data || imageHandle.empty()) return;
	Image* img = m_rm->images_.get(imageHandle);
	if (!img || img->vkImage == VK_NULL_HANDLE) return;
	// try host copy if supported and image has HOST_TRANSFER usage
	if (
		false
		&&
		m_hostCopySupported && (img->usage & ImageUsage::TransferDst) != ImageUsage::None) {
		VkFormat vkFmtTry = toVkFormat(img->format);
		if (IsHostTransferSupported(vkFmtTry)) {
			VkExtent3D ext{ img->extent.width, img->extent.height, img->extent.depth };
			std::vector<VkBufferImageCopy> dummy;
			bool remainTransferTry = (dstStages == VK_PIPELINE_STAGE_2_TRANSFER_BIT && dstAccess == VK_ACCESS_2_TRANSFER_WRITE_BIT);
			VkImageLayout finalLayoutTry = remainTransferTry ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			if (tryHostImageCopy(img->vkImage, vkFmtTry, ext, img->mipLevels, img->arrayLayers, data, dataSize, dummy, finalLayoutTry)) {
				bool remainTransfer = remainTransferTry;
				VulkanResourceManager::ImageGlobalSyncState st{};
				st.masks.stageMask = FromVkPipelineStage(dstStages);
				if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess));
				st.masks.accessMask = FromVkAccess(dstAccess);
				st.layout = remainTransfer ? ImageLayout::TransferDstOptimal : ImageLayout::ShaderReadOnlyOptimal;
				if (remainTransfer) st.masks.stageMask = PipelineStageBit_Transfer;
				m_rm->SetImageSyncState(imageHandle, st);
				return;
			}
		}
	}
	// fallback to staging ring copy path
	ThreadUploadState& ust = states_[CurrentPoolShard()];
	UploadShardActiveGuard shardGuard{ ust.active };
	// allocate from ring
	VkDeviceSize ringOff = allocFromRing(ust, dataSize, 16);
	VmaAllocation tmpAlloc = VK_NULL_HANDLE; VkBuffer tmpBuf = VK_NULL_HANDLE; bool useTmp = false;
	void* dstPtr = nullptr;
	VkBuffer srcBuf = ust.ring.buffer;
	VkDeviceSize srcOff = ringOff;
	if (ringOff == VK_WHOLE_SIZE) {
		// fallback temp
		VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		ci.size = dataSize;
		ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO; aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT; aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		VmaAllocationInfo info{}; VK_CHECK(vmaCreateBuffer(m_rm->m_allocator, &ci, &aci, &tmpBuf, &tmpAlloc, &info));
		dstPtr = info.pMappedData;
		srcBuf = tmpBuf;
		srcOff = 0;
		useTmp = true;
	}
	else {
		dstPtr = static_cast<char*>(ust.ring.mapped) + ringOff;
	}
	memcpy(dstPtr, data, dataSize);
	vmaFlushAllocation(m_rm->m_allocator, useTmp ? tmpAlloc : ust.ring.allocation, srcOff, dataSize);
	ensureCmd(ust);
	// barriers and copy – we need to handle full mip chain; caller provided dataSize covering all mips, but we need regions
	// Recompute regions similarly to UploadDataToImage2D
	VkFormat vkFmt = toVkFormat(img->format);
	// Use helper getFormatInfo (local to impl.cpp) – we forward declare via lambda? For now compute via switch minimal
	// We'll call a helper function defined in impl.cpp: getFormatInfo – need to expose or recompute inline
	// Instead reuse logic: for simplicity, assume tightly packed and use bufferRowLength=0; create one region per mip/layer if needed
	// For generic upload, we will use the same logic as original UploadDataToImage2D but with our ring buffer
	// To avoid duplication, we will record a single barrier UNDEFINED->TRANSFER_DST, then vkCmdCopyBufferToImage2 for all mips, then TRANSFER_DST->SHADER_READ_ONLY
	// Build regions on the fly
	// We need fmt info – replicate getFormatInfo minimal for uncompressed; fallback to 4 bytes
	// toVkFormat forward
	// We'll approximate: assume format is 4 bytes per pixel for most, but we have correct calculation via dataSize – just push one region per mip/layer with computed offsets? Simpler: if mipLevels==1 && arrayLayers==1, one region
	std::vector<VkBufferImageCopy2> regions2;
	// if we have detailed mip info, we should loop; for now if image is 2D and we have dataSize, we can create one region covering base mip
	// We'll treat data as tightly packed for all mips sequentially – need offsets. We'll reuse same logic as original but with srcOff base
	// For correctness, handle multi-mip by computing layer sizes using FormatInfo – we need FormatInfo helper. We'll define a local lambda that mimics getFormatInfo via vkGetPhysicalDeviceFormatProperties? Simpler: compute bytesPerPixel approximation 4 – but compressed formats will be wrong. Alternative: call original UploadDataToImage2D path for multi-mip? Instead we will delegate multi-mip to the legacy path.
	// For streamer, we support single-mip images (most textures are single) – multi-mip KTX already handled separately.
	VkImageSubresourceRange fullRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, img->mipLevels, 0, img->arrayLayers };
	uint32_t srcF = m_transfer->queueFamilyIndex();
	uint32_t dstF = m_graphics->queueFamilyIndex();
	const bool crossQueue = srcF != dstF;
	// EXCLUSIVE images need a release/acquire ownership pair. CONCURRENT
	// images (TransferDst streamer targets) skip the transfer-side release
	// (the timeline wait on the graphics-side submit provides availability)
	// and use IGNORED families on the acquire-side transition.
	const bool needOwnership = crossQueue && !img->isConcurrent;
	const uint32_t relSrcF = needOwnership ? srcF : VK_QUEUE_FAMILY_IGNORED;
	const uint32_t acqDstF = needOwnership ? dstF : VK_QUEUE_FAMILY_IGNORED;
	// UNDEFINED -> TRANSFER_DST
	ImageBarrier(ust.curCmd, img->vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fullRange, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
	// copy
	if (img->mipLevels == 1 && img->arrayLayers == 1) {
		// extent.depth serves 3D uploads (SG probe volumes); 2D images
		// carry depth == 1 so this is the same copy as before for them.
		VkBufferImageCopy2 r{ VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, nullptr, srcOff, 0,0,{ VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 },{ 0,0,0 },{ img->extent.width, img->extent.height, img->extent.depth } };
		VkCopyBufferToImageInfo2 copyInfo{ VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, nullptr, srcBuf, img->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r };
		vkCmdCopyBufferToImage2(ust.curCmd, &copyInfo);
	}
	else {
		const FormatInfo fmt = getFormatInfo(vkFmt);
		std::vector<VkBufferImageCopy2> regs;
		VkDeviceSize off = srcOff;
		for (uint32_t mip = 0; mip < img->mipLevels; ++mip) {
			uint32_t w = std::max(1u, img->extent.width >> mip);
			uint32_t h = std::max(1u, img->extent.height >> mip);
			uint32_t blocksX = (w + fmt.blockWidth - 1) / fmt.blockWidth;
			uint32_t blocksY = (h + fmt.blockHeight - 1) / fmt.blockHeight;
			VkDeviceSize layerSize = (VkDeviceSize)blocksX * blocksY * fmt.bytesPerBlock;
			for (uint32_t layer = 0; layer < img->arrayLayers; ++layer) {
				VkBufferImageCopy2 r{ VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, nullptr, off, 0,0,{ VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },{ 0,0,0 },{ w,h,1 } };
				regs.push_back(r);
				off += layerSize;
			}
		}
		VkCopyBufferToImageInfo2 copyInfo{ VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, nullptr, srcBuf, img->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regs.size(), regs.data() };
		vkCmdCopyBufferToImage2(ust.curCmd, &copyInfo);
	}
	bool remainTransfer = (dstStages == VK_PIPELINE_STAGE_2_TRANSFER_BIT && dstAccess == VK_ACCESS_2_TRANSFER_WRITE_BIT);
	if (!remainTransfer) {
		if (crossQueue) {
			if (needOwnership) {
				VkImageMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
				VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &rel };
				vkCmdPipelineBarrier2(ust.curCmd, &dep);
			}
			VkImageMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, 0, 0, dstStages, dstAccess, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, relSrcF, acqDstF, img->vkImage, fullRange };
			ust.pendingImageAcquires.push_back(acq);
		}
		else {
			ImageBarrier(ust.curCmd, img->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, fullRange, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStages, dstAccess, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		}
	}
	else {
		if (crossQueue) {
			if (needOwnership) {
				VkImageMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
				VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &rel }; vkCmdPipelineBarrier2(ust.curCmd, &dep);
			}
			VkImageMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, 0, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, relSrcF, acqDstF, img->vkImage, fullRange };
			ust.pendingImageAcquires.push_back(acq);
		}
	}
	ust.hasPending = true;
	// Defer staging destruction until the transfer batch retires
	// (TimelinePoint). Registered BEFORE the threshold check:
	// submitState CONSUMES pendingCleanups and resets the batch, so
	// registering AFTER it would orphan the cleanup in a shard that may
	// never flush again (a leaked tmp staging buffer).
	if (useTmp) {
		ust.pendingCleanups.push_back(std::packaged_task<void()>([rm = m_rm, buf = tmpBuf, alloc = tmpAlloc] { vmaDestroyBuffer(rm->m_allocator, buf, alloc); }));
	}
	ust.stagedBytesSinceFlush += dataSize;
	if (ust.stagedBytesSinceFlush >= flushThreshold_) {
		submitState(ust, false);
	}
	// update global sync
	VulkanResourceManager::ImageGlobalSyncState st{}; st.masks.stageMask = FromVkPipelineStage(dstStages); if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess)); st.masks.accessMask = FromVkAccess(dstAccess); st.layout = remainTransfer ? ImageLayout::TransferDstOptimal : ImageLayout::ShaderReadOnlyOptimal; if (remainTransfer) st.masks.stageMask = PipelineStageBit_Transfer; m_rm->SetImageSyncState(imageHandle, st);
}
void smvk::UploadStreamer::uploadImageKtx(ImageHandle imageHandle, const void* data, size_t dataSize, const std::vector<VkBufferImageCopy>& regions,
	VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
	if (!data || imageHandle.empty() || regions.empty()) return;
	Image* img = m_rm->images_.get(imageHandle);
	if (!img || img->vkImage == VK_NULL_HANDLE) return;
	bool remainTransfer = (dstStages == VK_PIPELINE_STAGE_2_TRANSFER_BIT && dstAccess == VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkImageLayout finalLayout = remainTransfer ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	// Try host copy first for KTX (upload once, long-lived)
	// Spec: finalLayout must be in pCopyDstLayouts; only GENERAL is guaranteed.
	if (false && m_hostCopySupported && (img->usage & ImageUsage::TransferDst) != ImageUsage::None
		&& m_rm->IsHostCopyDstLayoutSupported(finalLayout)) {
		VkFormat vkFmtTry = toVkFormat(img->format);
		if (IsHostTransferSupported(vkFmtTry) && vkCopyMemoryToImageEXT && vkTransitionImageLayoutEXT && img->extent.depth == 1) {
			VkHostImageLayoutTransitionInfoEXT trans1{ VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT, nullptr, img->vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, img->mipLevels, 0, img->arrayLayers } };
			if (vkTransitionImageLayoutEXT(device_, 1, &trans1) == VK_SUCCESS) {
				std::vector<VkMemoryToImageCopyEXT> hostCopies;
				hostCopies.reserve(regions.size());
				for (auto& r : regions) {
					VkMemoryToImageCopyEXT hc{};
					hc.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT;
					hc.pHostPointer = static_cast<const char*>(data) + r.bufferOffset;
					hc.memoryRowLength = 0;
					hc.memoryImageHeight = 0;
					hc.imageSubresource = VkImageSubresourceLayers{ r.imageSubresource.aspectMask, r.imageSubresource.mipLevel, r.imageSubresource.baseArrayLayer, r.imageSubresource.layerCount };
					hc.imageOffset = r.imageOffset;
					hc.imageExtent = r.imageExtent;
					hostCopies.push_back(hc);
				}
				VkCopyMemoryToImageInfoEXT copyInfo{ VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT, nullptr, /*VK_HOST_IMAGE_COPY_MEMCPY_BIT_EXT*/0, img->vkImage, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)hostCopies.size(), hostCopies.data() };
				VkResult cr = vkCopyMemoryToImageEXT(device_, &copyInfo);
				if (cr != VK_SUCCESS) {
					copyInfo.flags = 0;
					cr = vkCopyMemoryToImageEXT(device_, &copyInfo);
				}
				if (cr == VK_SUCCESS) {
					VkHostImageLayoutTransitionInfoEXT trans2{ VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT, nullptr, img->vkImage, VK_IMAGE_LAYOUT_GENERAL, finalLayout, VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, img->mipLevels, 0, img->arrayLayers } };
					if (vkTransitionImageLayoutEXT(device_, 1, &trans2) == VK_SUCCESS) {
						VulkanResourceManager::ImageGlobalSyncState st2{};
						st2.masks.stageMask = FromVkPipelineStage(dstStages);
						if (st2.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st2.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess));
						st2.masks.accessMask = FromVkAccess(dstAccess);
						st2.layout = remainTransfer ? ImageLayout::TransferDstOptimal : ImageLayout::ShaderReadOnlyOptimal;
						if (remainTransfer) st2.masks.stageMask = PipelineStageBit_Transfer;
						m_rm->SetImageSyncState(imageHandle, st2);
						return;
					}
				}
				// fallback: transition GENERAL -> TRANSFER_DST for staging path
				// Only if that target is also a supported host dst layout (GENERAL->TRANSFER_DST is also host-controlled).
				if (m_rm->IsHostCopyDstLayoutSupported(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)) {
					VkHostImageLayoutTransitionInfoEXT transFallback{ VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT, nullptr, img->vkImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, img->mipLevels, 0, img->arrayLayers } };
					vkTransitionImageLayoutEXT(device_, 1, &transFallback);
				}
			}
		}
	}
	// Fallback to staging ring
	// Hitch-path tracing (TOURMALINE_TEXSTREAM_TRACE): splits the upload
	// cost — ring alloc (possible GPU wait on an overlapping in-flight
	// batch) vs mapped write+flush vs command record/submit.
	const bool texTrace = std::getenv("TOURMALINE_TEXSTREAM_TRACE") != nullptr;
	const auto texT0 = std::chrono::steady_clock::now();
	ThreadUploadState& ust = states_[CurrentPoolShard()];
	UploadShardActiveGuard shardGuard{ ust.active };
	VkDeviceSize ringOff = allocFromRing(ust, dataSize, 16);
	const auto texT1 = std::chrono::steady_clock::now();
	VmaAllocation tmpAlloc = VK_NULL_HANDLE; VkBuffer tmpBuf = VK_NULL_HANDLE; bool useTmp = false;
	void* dstPtr = nullptr;
	VkBuffer srcBuf = ust.ring.buffer;
	VkDeviceSize srcOff = ringOff;
	if (ringOff == VK_WHOLE_SIZE) {
		VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		ci.size = dataSize;
		ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO; aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT; aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		VmaAllocationInfo info{}; VK_CHECK(vmaCreateBuffer(m_rm->m_allocator, &ci, &aci, &tmpBuf, &tmpAlloc, &info));
		dstPtr = info.pMappedData;
		srcBuf = tmpBuf;
		srcOff = 0;
		useTmp = true;
	}
	else {
		dstPtr = static_cast<char*>(ust.ring.mapped) + ringOff;
	}
	memcpy(dstPtr, data, dataSize);
	vmaFlushAllocation(m_rm->m_allocator, useTmp ? tmpAlloc : ust.ring.allocation, srcOff, dataSize);
	const auto texT2 = std::chrono::steady_clock::now();
	ensureCmd(ust);
	VkImageSubresourceRange fullRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, img->mipLevels, 0, img->arrayLayers };
	uint32_t srcF = m_transfer->queueFamilyIndex();
	uint32_t dstF = m_graphics->queueFamilyIndex();
	const bool crossQueue = srcF != dstF;
	// EXCLUSIVE images need a release/acquire ownership pair. CONCURRENT
	// images (TransferDst streamer targets) skip the transfer-side release
	// (the timeline wait on the graphics-side submit provides availability)
	// and use IGNORED families on the acquire-side transition.
	const bool needOwnership = crossQueue && !img->isConcurrent;
	const uint32_t relSrcF = needOwnership ? srcF : VK_QUEUE_FAMILY_IGNORED;
	const uint32_t acqDstF = needOwnership ? dstF : VK_QUEUE_FAMILY_IGNORED;
	// UNDEFINED or TRANSFER_DST -> TRANSFER_DST (handle host fallback case where image already in TRANSFER_DST)
	// If host fallback transitioned to TRANSFER_DST, oldLayout should be TRANSFER_DST, not UNDEFINED. Check current layout via host transition? For simplicity, use UNDEFINED->TRANSFER_DST with IGNORED, driver will handle if already TRANSFER_DST (validation may warn but not fault for fallback)
	ImageBarrier(ust.curCmd, img->vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fullRange, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
	std::vector<VkBufferImageCopy2> regs2;
	regs2.reserve(regions.size());
	for (auto& r : regions) {
		VkBufferImageCopy2 r2{ VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, nullptr, srcOff + r.bufferOffset, 0,0,{ r.imageSubresource.aspectMask, r.imageSubresource.mipLevel, r.imageSubresource.baseArrayLayer, r.imageSubresource.layerCount }, r.imageOffset, r.imageExtent };
		regs2.push_back(r2);
	}
	VkCopyBufferToImageInfo2 copyInfo{ VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, nullptr, srcBuf, img->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regs2.size(), regs2.data() };
	vkCmdCopyBufferToImage2(ust.curCmd, &copyInfo);
	if (!remainTransfer) {
		if (crossQueue) {
			if (needOwnership) {
				VkImageMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
				VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &rel };
				vkCmdPipelineBarrier2(ust.curCmd, &dep);
			}
			VkImageMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, 0, 0, dstStages, dstAccess, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, relSrcF, acqDstF, img->vkImage, fullRange };
			ust.pendingImageAcquires.push_back(acq);
		}
		else {
			ImageBarrier(ust.curCmd, img->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, fullRange, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStages, dstAccess, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		}
	}
	else {
		if (crossQueue) {
			if (needOwnership) {
				VkImageMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
				VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &rel }; vkCmdPipelineBarrier2(ust.curCmd, &dep);
			}
			VkImageMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, 0, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, relSrcF, acqDstF, img->vkImage, fullRange };
			ust.pendingImageAcquires.push_back(acq);
		}
	}
	ust.hasPending = true;
	// Defer staging destruction until the transfer batch retires
	// (TimelinePoint). Registered BEFORE the threshold check for the same
	// reason as uploadImage's tail: submitState consumes pendingCleanups,
	// so a cleanup registered after it can be orphaned in a shard that
	// never flushes again (a leaked tmp staging buffer).
	if (useTmp) {
		ust.pendingCleanups.push_back(std::packaged_task<void()>([rm = m_rm, buf = tmpBuf, alloc = tmpAlloc] { vmaDestroyBuffer(rm->m_allocator, buf, alloc); }));
	}
	ust.stagedBytesSinceFlush += dataSize;
	if (ust.stagedBytesSinceFlush >= flushThreshold_) {
		submitState(ust, false);
	}
	VulkanResourceManager::ImageGlobalSyncState st{}; st.masks.stageMask = FromVkPipelineStage(dstStages); if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess)); st.masks.accessMask = FromVkAccess(dstAccess); st.layout = remainTransfer ? ImageLayout::TransferDstOptimal : ImageLayout::ShaderReadOnlyOptimal; if (remainTransfer) st.masks.stageMask = PipelineStageBit_Transfer; m_rm->SetImageSyncState(imageHandle, st);
	if (texTrace) {
		using msd = std::chrono::duration<double, std::milli>;
		const auto texT3 = std::chrono::steady_clock::now();
		std::fprintf(stderr,
			"[TexStream]   uploadKtx %ux%u mips %u (%.1f MB staged): ring %.1f write+flush %.1f record+submit %.1f ms\n",
			img->extent.width, img->extent.height, img->mipLevels,
			static_cast<double>(dataSize) / (1024.0 * 1024.0),
			msd(texT1 - texT0).count(), msd(texT2 - texT1).count(),
			msd(texT3 - texT2).count());
		std::fflush(stderr);
	}
}
void smvk::UploadStreamer::flush() {
	SMVK_PROFILE_STREAM("UploadFlush");
	// Flush contract (see ThreadUploadState::active): cross-shard drains
	// require QUIESCENT producers - a worker recording into its shard while
	// this loop touches it is a data race, so the assert makes the contract
	// debug-visible. (flushLocal is the same-thread escape hatch.)
	for (const auto& ust : states_)
		assert(!ust.active.load(std::memory_order_acquire)
			&& "flush() during an active upload recording (producers must be quiescent)");
	assert(!bulk_.active.load(std::memory_order_acquire)
		&& "flush() during an active bulk upload recording");
	if (bulk_.hasPending) submitState(bulk_, true);
	for (auto& ust : states_) if (ust.hasPending) submitState(ust, true); if (m_transfer) m_transfer->purge();
}
void smvk::UploadStreamer::flushLocal() {
	SMVK_PROFILE_STREAM("UploadFlushLocal");
	// Same-thread shard only: the producer's own sequential drain (the
	// caller IS the shard's owner - no quiescence needed).
	ThreadUploadState& ust = states_[CurrentPoolShard()]; if (ust.hasPending) submitState(ust, true); if (m_transfer) m_transfer->purge();
}
smvk::TimelinePoint smvk::UploadStreamer::flushAndGetTimeline() {
	// Drains all shards (quiescent: call after workers joined). Returns the
	// last timeline point for callers that need to chain. Same flush
	// contract as flush() - the assert makes a mid-recording drain
	// debug-visible instead of a silent race.
	TimelinePoint last{};
	for (const auto& ust : states_)
		assert(!ust.active.load(std::memory_order_acquire)
			&& "flushAndGetTimeline() during an active upload recording (producers must be quiescent)");
	assert(!bulk_.active.load(std::memory_order_acquire)
		&& "flushAndGetTimeline() during an active bulk upload recording");
	if (bulk_.hasPending && bulk_.curCmd != VK_NULL_HANDLE) {
		submitState(bulk_, true);
		last = lastTransferPoint();
	}
	for (auto& ust : states_) {
		if (!ust.hasPending || ust.curCmd == VK_NULL_HANDLE) continue;
		auto tp = submitTransfer(ust.curHandle);
		recordSubmittedRange(ust);
		for (auto& task : ust.pendingCleanups) m_rm->AddDeferredTask(std::move(task), tp);
		ust.pendingCleanups.clear();
		if (m_transfer != m_graphics && (!ust.pendingBufferAcquires.empty() || !ust.pendingImageAcquires.empty())) {
			auto gHan = m_graphics->acquire();
			auto gCmd = m_graphics->GetCommandBuffer(gHan);
			VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, (uint32_t)ust.pendingBufferAcquires.size(), ust.pendingBufferAcquires.empty() ? nullptr : ust.pendingBufferAcquires.data(), (uint32_t)ust.pendingImageAcquires.size(), ust.pendingImageAcquires.empty() ? nullptr : ust.pendingImageAcquires.data() };
			vkCmdPipelineBarrier2(gCmd, &dep);
			TimelineWait wait{ tp, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
			m_graphics->submit(gHan, std::span<const TimelineWait>(&wait, 1));
		}
		ust.pendingBufferAcquires.clear();
		ust.pendingImageAcquires.clear();
		ust.curCmd = VK_NULL_HANDLE;
		ust.curHandle = {};
		ust.hasPending = false;
		ust.stagedBytesSinceFlush = 0;
		last = tp;
	}
	return last;
}
void smvk::UploadStreamer::waitAll() {
	SMVK_PROFILE_STREAM("UploadWaitAll");
	flush(); if (m_transfer) m_transfer->waitAll(); if (m_graphics && m_graphics != m_transfer) m_graphics->waitAll();
}

bool smvk::VulkanResourceManager::BulkCopyBuffer(BufferHandle dst, const void* content,
	std::span<const BufferBulkCopy> copies, VkPipelineStageFlags2 dstStages,
	VkAccessFlags2 dstAccess) {
	if (copies.empty()) return true;
	Buffer* b = buffers_.get(dst);
	if (!b || !content) return false;
	if (m_streamer) {
		m_streamer->uploadBufferBulk(dst, content, copies, dstStages, dstAccess);
		return true;
	}
	// No streamer (headless configs): per-entry single-copy path. Same
	// semantics, no batching.
	for (const BufferBulkCopy& c : copies) {
		if (c.size == 0) continue;
		UploadDataToBuffer(dst, static_cast<const char*>(content) + c.srcOffset,
			c.size, dstStages, dstAccess, 0, c.dstOffset);
	}
	return true;
}

void smvk::VulkanResourceManager::FlushUploads() { if (m_streamer) m_streamer->flush(); }
void smvk::VulkanResourceManager::WaitForUploads() { if (m_streamer) m_streamer->waitAll(); }
void smvk::VulkanResourceManager::UploadDataToBuffer(
	BufferHandle buffer,
	const void* data,
	size_t dataSize,
	VkPipelineStageFlags2 dstStages,
	VkAccessFlags2 dstAccess,
	size_t src_offset, // unused
	size_t dest_offset
) {
	// Hot path via UploadStreamer (transfer queue + ring) if available.
	// NEVER flushed per call: each flush is a queue submit, and the old
	// submit-per-upload heuristic exhausted the 16-slot submitter pool
	// every frame, blocking the render thread for a full frame of GPU
	// latency mid-record (Optick showed it as multi-ms UploadSeed/
	// Exposure scopes). Uploads accumulate in the calling thread's ring
	// shard and are submitted once per frame by the swapchain right
	// before the main submit (EndAndPresentFramebuffer), plus explicit
	// FlushUploads() at load boundaries. The sync download paths flush
	// on their own so uploaded-then-downloaded data stays ordered.
	if (m_streamer) {
		m_streamer->uploadBuffer(buffer, data, dataSize, dest_offset, dstStages, dstAccess);
		return;
	}
	auto buf = buffers_.get(buffer);
	assert(buffer.valid());
	assert(buf->vkBuffer != VK_NULL_HANDLE);

	auto han = m_submitter->acquire();
	auto cmd = m_submitter->GetCommandBuffer(han);
	if (buf->isMappable()) {
		VK_CHECK(vmaCopyMemoryToAllocation(m_allocator, data, buf->vma_alloc, dest_offset, dataSize));
		BufferBarrier(
			cmd,
			buf->vkBuffer,
			dest_offset,
			dataSize,
			VK_PIPELINE_STAGE_2_HOST_BIT,
			VK_ACCESS_2_HOST_WRITE_BIT,
			dstStages,
			dstAccess
		);
		m_submitter->submit(han);
		{
			VulkanResourceManager::BufferGlobalSyncState st{};
			st.masks.stageMask = FromVkPipelineStage(dstStages);
			if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE)
				st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess));
			st.masks.accessMask = FromVkAccess(dstAccess);
			SetBufferSyncState(buffer, st);
		}
		return;
	}

	auto stageHan = CreateBuffer(dataSize, smvk::BufferType_Staging, "staging buffer").release();
	buf = buffers_.get(buffer);
	auto stagingBuf = buffers_.get(stageHan);
	assert(stagingBuf->vkBuffer != VK_NULL_HANDLE);

	VK_CHECK(vmaCopyMemoryToAllocation(m_allocator, data, stagingBuf->vma_alloc, 0, dataSize));
	VkBufferCopy copyRegion{ 0, dest_offset, dataSize };
	vkCmdCopyBuffer(cmd, stagingBuf->vkBuffer, buf->vkBuffer, 1, &copyRegion);
	BufferBarrier(cmd, buf->vkBuffer, dest_offset, dataSize, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStages, dstAccess);
	m_submitter->submit(han);
	{
		VulkanResourceManager::BufferGlobalSyncState st{};
		st.masks.stageMask = FromVkPipelineStage(dstStages);
		if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE)
			st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess));
		st.masks.accessMask = FromVkAccess(dstAccess);
		SetBufferSyncState(buffer, st);
	}
	// 3-arg AddDeferredTask WITH the submitter: the destroy retires when
	// this submit completes (the 2-arg legacy path treats the handle as
	// always-ready, which could destroy the staging while the copy was
	// still in flight).
	AddDeferredTask(std::packaged_task<void()>([this, staging = std::move(stageHan)] {destroy(this, staging); }), han, m_submitter);
}
bool smvk::VulkanResourceManager::DownloadDataFromBuffers(std::span<const smvk::BufferDownload> downloads) {
	if (downloads.empty())
		return false;
	// Validate everything up front: all-or-nothing, no partial copies.
	uint64_t total = 0;
	for (const smvk::BufferDownload& d : downloads) {
		if (d.size == 0 || d.out == nullptr || d.src.empty())
			return false;
		auto srcBuf = buffers_.get(d.src);
		if (!srcBuf || srcBuf->vkBuffer == VK_NULL_HANDLE)
			return false;
		total += static_cast<uint64_t>(d.size);
	}
	// Same persistent staging as the single-copy path (see comment there).
	if (!EnsureReadbackStaging(static_cast<VkDeviceSize>(total)))
		return false;
	auto stagingBuf = buffers_.get(m_readbackStaging);
	if (!stagingBuf || stagingBuf->vkBuffer == VK_NULL_HANDLE)
		return false;
	// Pending uploads must be submitted before the copy can read their
	// targets (UploadDataToBuffer no longer flushes per call). The upload
	// batch (and its acquire-barrier submit on this queue when the
	// transfer queue is distinct) orders ahead of the copy below.
	FlushUploads();
	auto han = m_submitter->acquire();
	auto cmd = m_submitter->GetCommandBuffer(han);
	uint64_t stagingOffset = 0;
	for (const smvk::BufferDownload& d : downloads) {
		auto srcBuf = buffers_.get(d.src);
		// Prior writers -> transfer read. ALL_COMMANDS / MEMORY_ALL is the
		// conservative src: the last writer is not tracked here.
		BufferBarrier(cmd, srcBuf->vkBuffer, d.srcOffset, d.size,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferCopy copyRegion{
			static_cast<VkDeviceSize>(d.srcOffset),
			static_cast<VkDeviceSize>(stagingOffset),
			static_cast<VkDeviceSize>(d.size) };
		vkCmdCopyBuffer(cmd, srcBuf->vkBuffer, stagingBuf->vkBuffer, 1, &copyRegion);
		stagingOffset += static_cast<uint64_t>(d.size);
	}
	BufferBarrier(cmd, stagingBuf->vkBuffer, 0, static_cast<VkDeviceSize>(total),
		VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_HOST_BIT,
		VK_ACCESS_2_HOST_READ_BIT);
	m_submitter->submit(han);
	m_submitter->wait(han, UINT64_MAX);
	// Safe to reuse on the next call: every download waits for its own
	// copy before returning, so the staging is never read in flight.
	// GPU done != CPU-visible on non-coherent staging: invalidate the
	// copied range before reading it (see InvalidateStagingRange).
	InvalidateStagingRange(m_readbackStaging, 0, static_cast<VkDeviceSize>(total));
	void* mapped = MapBuffer(m_readbackStaging);
	if (!mapped)
		return false;
	const uint8_t* base = static_cast<const uint8_t*>(mapped);
	uint64_t offset = 0;
	for (const smvk::BufferDownload& d : downloads) {
		std::memcpy(d.out, base + offset, d.size);
		offset += static_cast<uint64_t>(d.size);
	}
	UnmapBuffer(m_readbackStaging);
	return true;
}

uint64_t smvk::VulkanResourceManager::SubmitAsyncDownloadDataFromBuffers(
	std::span<const smvk::BufferDownload> downloads) {
	if (downloads.empty() || m_asyncReadbackTicket != 0)
		return 0; // no work, or the single slot is still in flight
	// Validate everything up front (all-or-nothing, like the sync path).
	uint64_t total = 0;
	for (const smvk::BufferDownload& d : downloads) {
		if (d.size == 0 || d.out == nullptr || d.src.empty())
			return 0;
		auto srcBuf = buffers_.get(d.src);
		if (!srcBuf || srcBuf->vkBuffer == VK_NULL_HANDLE)
			return 0;
		total += static_cast<uint64_t>(d.size);
	}
	if (!EnsureAsyncStaging(static_cast<VkDeviceSize>(total)))
		return 0;
	auto stagingBuf = buffers_.get(m_asyncStaging);
	if (!stagingBuf || stagingBuf->vkBuffer == VK_NULL_HANDLE)
		return 0;
	// Submit on the transfer queue when it is distinct from graphics: the
	// ticket copy then shares FIFO with the streamer's upload batches,
	// preserving the download-before-zero/clear ordering the exposure
	// histogram + texture feedback rely on, and it stops queueing behind
	// in-flight main frames on the graphics timeline (polls complete a
	// frame or two earlier). The wait on the latest graphics retirement
	// point orders the copy after every submitted frame that wrote the
	// sources (retired slices only) – the cross-queue acquire the old
	// same-queue FIFO used to provide for free.
	Submitter* queue = (m_transferSubmitter && m_transferSubmitter != m_submitter)
		? m_transferSubmitter : m_submitter;
	auto han = queue->acquire();
	auto cmd = queue->GetCommandBuffer(han);
	uint64_t stagingOffset = 0;
	for (const smvk::BufferDownload& d : downloads) {
		auto srcBuf = buffers_.get(d.src);
		// Prior writers -> transfer read (conservative ALL_COMMANDS src;
		// the rendergraph tracked the real last writer for its own passes).
		BufferBarrier(cmd, srcBuf->vkBuffer, d.srcOffset, d.size,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferCopy copyRegion{
			static_cast<VkDeviceSize>(d.srcOffset),
			static_cast<VkDeviceSize>(stagingOffset),
			static_cast<VkDeviceSize>(d.size) };
		vkCmdCopyBuffer(cmd, srcBuf->vkBuffer, stagingBuf->vkBuffer, 1, &copyRegion);
		stagingOffset += static_cast<uint64_t>(d.size);
	}
	BufferBarrier(cmd, stagingBuf->vkBuffer, 0, static_cast<VkDeviceSize>(total),
		VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_HOST_BIT,
		VK_ACCESS_2_HOST_READ_BIT);
	// No wait: the ticket is the timeline value the copy signals. The
	// caller polls it non-blocking (PollAsyncDownloadDataFromBuffers).
	// Cross-queue case: wait the latest graphics retirement point so the
	// copy happens-after all submitted frames that wrote the sources.
	// AND join the transfer-queue serial chain: the copy must be ordered
	// after every earlier upload batch (they may still be executing) and
	// — via chainExternalPoint — every LATER batch (the per-frame
	// zero-fill of the same slices) is ordered after the copy. The old
	// "shares FIFO with the streamer's upload batches" reasoning was a
	// same-queue-ordering assumption the spec does not make (submissions
	// may overlap) and sync validation flagged the resulting
	// WRITE_AFTER_READ / WRITE_AFTER_WRITE hazards.
	const TimelinePoint chainPrev =
		m_streamer ? m_streamer->lastTransferPoint() : TimelinePoint{};
	TimelinePoint tp{};
	{
		const bool waitGraphics = queue != m_submitter
			&& m_lastGraphicsSemaphore != VK_NULL_HANDLE
			&& m_lastGraphicsValue != 0;
		TimelineWait waits[2]{};
		uint32_t waitCount = 0;
		if (waitGraphics) {
			waits[waitCount++] = TimelineWait{
				TimelinePoint{ m_lastGraphicsSemaphore, m_lastGraphicsValue },
				VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
		}
		if (chainPrev.valid()) {
			waits[waitCount++] = TimelineWait{
				chainPrev, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
		}
		tp = waitCount
			? queue->submit(han, std::span<const TimelineWait>(waits, waitCount))
			: queue->submit(han);
	}
	if (!tp.valid()) {
		return 0;
	}
	// TOURMALINE_RB_DIAG=1 (the churn DEVICE_LOST hunt's readback-collision
	// probe, env-gated like DEFER-DIAG): records the ticket's SOURCE buffers
	// so the deferred buffer destroy (DestroyBufferNow) can flag a
	// copy-still-in-flight destroy. The hunt's verdict: NO collision ever
	// fired across the whole battery (the death was the SSR ray counter's
	// missing creation zero-fill — see ssr.h); the probe stays as the
	// cheap guard for future readback-lifetime questions. Also the
	// single-slot serialization check below (a submit while a prior
	// ticket is in flight clobbers the slot — should be impossible).
	if (RbDiagEnabled()) {
		if (m_asyncReadbackTicket != 0) {
			std::fprintf(stderr,
				"[RB-DIAG] SLOT CLOBBER: submit while ticket %llu is still "
				"in flight (the single-slot serialization is broken)\n",
				static_cast<unsigned long long>(m_asyncReadbackTicket));
		}
		m_asyncReadbackSources.assign(downloads.size(), smvk::BufferHandle{});
		for (size_t di = 0; di < downloads.size(); ++di)
			m_asyncReadbackSources[di] = downloads[di].src;
	}
	// Same timeline as the upload chain (queue IS the streamer's
	// Submitter — dedicated transfer or the shared graphics fallback), so
	// merge: the next zero-fill batch waits this copy.
	if (m_streamer)
		m_streamer->chainExternalPoint(tp);
	m_asyncReadbackTicket = tp.value;
	m_asyncReadbackSemaphore = tp.semaphore;
	m_asyncReadbackBytes = static_cast<VkDeviceSize>(total);
	return tp.value;
}

bool smvk::VulkanResourceManager::PollAsyncDownloadDataFromBuffers(
	uint64_t ticket, std::span<const smvk::BufferDownload> downloads) {
	if (ticket == 0 || ticket != m_asyncReadbackTicket || m_asyncStaging.empty())
		return false;
	// NON-BLOCKING timeline check: the copy (and everything submitted
	// before it) has completed once the counter reaches the ticket.
	uint64_t completed = 0;
	VkResult r = vkGetSemaphoreCounterValue(device_, m_asyncReadbackSemaphore, &completed);
	if (r != VK_SUCCESS)
		return false;
	if (completed < ticket)
		return false; // still in flight; try again next frame
	// Non-coherent staging: invalidate the submitted range before the
	// reads (see InvalidateStagingRange - the timeline completion says the
	// GPU wrote the bytes, not that the CPU can SEE them yet).
	InvalidateStagingRange(m_asyncStaging, 0,
		static_cast<VkDeviceSize>(m_asyncReadbackBytes));
	void* mapped = MapBuffer(m_asyncStaging);
	if (!mapped)
		return false;
	const uint8_t* base = static_cast<const uint8_t*>(mapped);
	uint64_t offset = 0;
	for (const smvk::BufferDownload& d : downloads) {
		if (offset + d.size <= static_cast<uint64_t>(m_asyncReadbackBytes))
			std::memcpy(d.out, base + offset, d.size);
		offset += static_cast<uint64_t>(d.size);
	}
	UnmapBuffer(m_asyncStaging);
	m_asyncReadbackTicket = 0; // slot free for the next submit
	m_asyncReadbackSemaphore = VK_NULL_HANDLE;
	// TMP-DIAG: the copy landed — the sources are safe to destroy now.
	m_asyncReadbackSources.clear();
	return true;
}

bool smvk::VulkanResourceManager::EnsureAsyncStaging(VkDeviceSize size) {
	static constexpr VkDeviceSize kMinBytes = 256ull * 1024;         // 256 KiB floor
	static constexpr VkDeviceSize kMaxBytes = 64ull * 1024 * 1024;   // sanity cap (small periodic payloads)
	if (!m_asyncStaging.empty() && m_asyncStagingSize >= size)
		return true;
	if (size > kMaxBytes)
		return false;
	VkDeviceSize want = kMinBytes;
	while (want < size)
		want *= 2;
	// HOST_ACCESS_RANDOM: the CPU reads the copy results (cached memory).
	Holder<BufferHandle> grown = CreateBuffer(want, smvk::BufferType_Staging,
		"async readback staging", VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
	if (grown.empty())
		return false;
	if (!m_asyncStaging.empty())
		destroy(this, m_asyncStaging); // old staging retires via DeferDestroy
	m_asyncStaging = grown.release();
	m_asyncStagingSize = want;
	return true;
}

bool smvk::VulkanResourceManager::DownloadDataFromBuffer(BufferHandle src, void* out, size_t size, size_t srcOffset) {
	if (size == 0 || out == nullptr || src.empty())
		return false;
	const smvk::BufferDownload single{ src, out, size, srcOffset };
	return DownloadDataFromBuffers(std::span<const smvk::BufferDownload>(&single, 1));
}

bool smvk::VulkanResourceManager::DownloadDataFromImage(ImageHandle src, ImageLayout currentLayout, void* out, size_t outSize) {
	if (outSize == 0 || out == nullptr || src.empty())
		return false;
	auto srcImg = images_.get(src);
	if (!srcImg || srcImg->vkImage == VK_NULL_HANDLE)
		return false;
	// vkCmdCopyImageToBuffer needs TRANSFER_SRC usage on the image: without
	// it the copy is invalid (UB — the staging keeps garbage) and there is
	// no fallback here, so fail loudly instead of returning junk.
	if ((srcImg->usage & ImageUsage::TransferSrc) == ImageUsage::None) {
		std::fprintf(stderr,
			"[DownloadDataFromImage] image '%s' lacks TransferSrc usage — cannot copy\n",
			srcImg->name.valid() ? srcImg->name.data() : "<anon>");
		return false;
	}
	if (!EnsureReadbackStaging(static_cast<VkDeviceSize>(outSize)))
		return false;
	auto stagingBuf = buffers_.get(m_readbackStaging);
	if (!stagingBuf || stagingBuf->vkBuffer == VK_NULL_HANDLE)
		return false;

	// See DownloadDataFromBuffers: submit pending uploads before reading.
	FlushUploads();

	// vkCmdCopyImageToBuffer sources are only GENERAL / TRANSFER_SRC_OPTIMAL:
	// transition out of the caller-declared ACTUAL layout for the copy and
	// back afterwards. Never pass Undefined here - that discards contents.
	const VkImageLayout tracked = ToVkImageLayout(currentLayout);
	// Depth/stencil images copy through their own aspect — a COLOR-aspect
	// copy from a D32F image is invalid and yields garbage.
	const VkFormat srcFmt = toVkFormat(srcImg->format);
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	if (srcFmt == VK_FORMAT_D16_UNORM || srcFmt == VK_FORMAT_D32_SFLOAT)
		aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
	else if (srcFmt == VK_FORMAT_D24_UNORM_S8_UINT || srcFmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
		aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	else if (srcFmt == VK_FORMAT_S8_UINT)
		aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
	const VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
	const bool needTransition =
		tracked != VK_IMAGE_LAYOUT_GENERAL && tracked != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	auto han = m_submitter->acquire();
	auto cmd = m_submitter->GetCommandBuffer(han);
	if (needTransition) {
		ImageBarrier(cmd, srcImg->vkImage, tracked, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, range,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_READ_BIT,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
	}
	VkBufferImageCopy region{};
	region.imageSubresource = { aspect, 0, 0, 1 };
	region.imageExtent = { srcImg->extent.width, srcImg->extent.height, srcImg->extent.depth };
	vkCmdCopyImageToBuffer(cmd, srcImg->vkImage,
		needTransition ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : tracked,
		stagingBuf->vkBuffer, 1, &region);
	if (needTransition) {
		ImageBarrier(cmd, srcImg->vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tracked, range,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
	}
	BufferBarrier(cmd, stagingBuf->vkBuffer, 0, outSize,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_HOST_BIT,
		VK_ACCESS_2_HOST_READ_BIT);
	m_submitter->submit(han);
	m_submitter->wait(han, UINT64_MAX);
	// Non-coherent staging: invalidate the copied range before the read
	// (see InvalidateStagingRange - GPU completion alone is not enough).
	InvalidateStagingRange(m_readbackStaging, 0, static_cast<VkDeviceSize>(outSize));
	void* mapped = MapBuffer(m_readbackStaging);
	if (!mapped)
		return false;
	std::memcpy(out, mapped, outSize);
	UnmapBuffer(m_readbackStaging);
	return true;
}

bool smvk::VulkanResourceManager::EnsureReadbackStaging(VkDeviceSize size) {
	// Sizes centralized in mem::config (single place to resize). Growth
	// failures crash per the OOM policy — a download that silently returns
	// false would hand uninitialized pixels to the caller.
	if (size > smvk::mem::config::Runtime().readbackStagingMax) {
		std::fprintf(stderr,
			"[OOM] readback staging request %llu bytes exceeds cap %llu bytes. "
			"No recovery — aborting.\n",
			(unsigned long long)size,
			(unsigned long long)smvk::mem::config::Runtime().readbackStagingMax);
		std::fflush(stderr);
		std::abort();
	}
	if (!m_readbackStaging.empty() && m_readbackStagingSize >= size)
		return true;
	VkDeviceSize want = smvk::mem::config::Runtime().readbackStagingMin;
	while (want < size)
		want *= 2;
	// HOST_ACCESS_RANDOM: the CPU READS this buffer (copy results), so
	// cached memory beats the default sequential-write (write-combined).
	Holder<BufferHandle> grown = CreateBuffer(want, smvk::BufferType_Staging,
		"readback staging", VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
	if (grown.empty()) {
		std::fprintf(stderr,
			"[OOM] readback staging VMA allocation of %llu bytes failed. "
			"No recovery — aborting.\n",
			(unsigned long long)want);
		std::fflush(stderr);
		std::abort();
	}
	if (!m_readbackStaging.empty())
		destroy(this, m_readbackStaging); // old staging retires via DeferDestroy
	m_readbackStaging = grown.release();
	m_readbackStagingSize = want;
	return true;
}
void smvk::VulkanResourceManager::UploadDataToImage2D(
	const Image& image,
	const void* data,
	VkPipelineStageFlags2 dstStages,
	VkAccessFlags2 dstAccess,
	size_t srcOffset
)
{
	assert(data != nullptr);
	assert(image.vkImage != VK_NULL_HANDLE);
	// 3D uploads (SG probe volumes): single-mip/single-layer, tightly
	// packed w*h*depth slices. The streamer's single-mip region path
	// carries extent.depth (see uploadImage); the 2D fallback below
	// (mip chains + array layers) does not apply.
	if (image.type == ImageType::Image3D) {
		assert(image.mipLevels == 1 && image.arrayLayers == 1
			&& "3D uploads support single-mip, single-layer images");
		const FormatInfo fmt3 = getFormatInfo(toVkFormat(image.format));
		// Block-aware size (matches the 2D path below): bytesPerBlock is
		// per blockWidth x blockHeight block, NOT per texel. The old
		// w*h*d*bytesPerBlock math was right for the uncompressed SG
		// volumes (1x1 blocks) but read 16x past the real data for the
		// BC6H dense-GI layers (4x4 blocks) — an OOB memcpy in the
		// streamer ring.
		const uint32_t blocksX =
			(image.extent.width + fmt3.blockWidth - 1) / fmt3.blockWidth;
		const uint32_t blocksY =
			(image.extent.height + fmt3.blockHeight - 1) / fmt3.blockHeight;
		const VkDeviceSize size3 = static_cast<VkDeviceSize>(blocksX)
			* blocksY * image.extent.depth * fmt3.bytesPerBlock;
		assert(m_streamer && "3D uploads require the upload streamer");
		if (m_streamer) {
			ImageHandle found{};
			images_.forEachHandle([&](ImageHandle h) {
				auto* im = images_.get(h);
				if (im && im->vkImage == image.vkImage)
					found = h;
			});
			assert(!found.empty() && "3D upload: image not in the pool");
			if (!found.empty()) {
				m_streamer->uploadImage(found,
					static_cast<const std::byte*>(data) + srcOffset,
					static_cast<size_t>(size3), dstStages, dstAccess);
				m_streamer->flushLocal();
				return;
			}
		}
		return;
	}
	assert(image.type == ImageType::Image2D);
	assert((image.usage & ImageUsage::TransferDst) != ImageUsage::None);

	if (m_streamer) {
		// Use streamer hot-path (transfer queue + ring) if available
		ImageHandle found{};
		images_.forEachHandle([&](ImageHandle h) { auto* im = images_.get(h); if (im && im->vkImage == image.vkImage) found = h; });
		if (!found.empty()) {
			// compute upload size
			const VkFormat vkFmt2 = toVkFormat(image.format);
			const FormatInfo fmt2 = getFormatInfo(vkFmt2);
			VkDeviceSize sz = 0;
			for (uint32_t mip = 0; mip < image.mipLevels; ++mip) {
				uint32_t w = std::max(1u, image.extent.width >> mip);
				uint32_t h = std::max(1u, image.extent.height >> mip);
				uint32_t bx = (w + fmt2.blockWidth - 1) / fmt2.blockWidth;
				uint32_t by = (h + fmt2.blockHeight - 1) / fmt2.blockHeight;
				sz += (VkDeviceSize)bx * by * fmt2.bytesPerBlock * image.arrayLayers;
			}
			m_streamer->uploadImage(found, static_cast<const std::byte*>(data) + srcOffset, (size_t)sz, dstStages, dstAccess);
			m_streamer->flushLocal();
			return;
		}
	}

	const VkFormat vkFormat = toVkFormat(image.format);
	const FormatInfo fmt = getFormatInfo(vkFormat);

	const uint32_t mipLevels = image.mipLevels;
	const uint32_t arrayLayers = image.arrayLayers;

	// -------------------------------------------------------------------------
	// Calculate upload size
	// -------------------------------------------------------------------------

	VkDeviceSize stagingBufferSize = 0;

	for (uint32_t mip = 0; mip < mipLevels; ++mip)
	{
		const uint32_t mipWidth =
			std::max(1u, image.extent.width >> mip);

		const uint32_t mipHeight =
			std::max(1u, image.extent.height >> mip);

		const uint32_t blocksX =
			(mipWidth + fmt.blockWidth - 1) /
			fmt.blockWidth;

		const uint32_t blocksY =
			(mipHeight + fmt.blockHeight - 1) /
			fmt.blockHeight;

		const VkDeviceSize layerSize =
			static_cast<VkDeviceSize>(blocksX) *
			static_cast<VkDeviceSize>(blocksY) *
			static_cast<VkDeviceSize>(fmt.bytesPerBlock);

		stagingBufferSize +=
			layerSize *
			static_cast<VkDeviceSize>(arrayLayers);
	}

	assert(stagingBufferSize > 0);


	// -------------------------------------------------------------------------
	// Staging buffer
	// -------------------------------------------------------------------------

	auto stagingBufferHandle =
		CreateBuffer(
			stagingBufferSize,
			smvk::BufferType_Staging,
			"image upload staging buffer"
		).release();

	void* mapped =
		MapBuffer(stagingBufferHandle);

	assert(mapped != nullptr);

	std::memcpy(
		mapped,
		static_cast<const std::byte*>(data) + srcOffset,
		static_cast<size_t>(stagingBufferSize)
	);

	UnmapBuffer(stagingBufferHandle);


	// -------------------------------------------------------------------------
	// Command buffer
	// -------------------------------------------------------------------------

	auto submitHandle =
		m_submitter->acquire();

	VkCommandBuffer cmd =
		m_submitter
		->GetCommandBuffer(submitHandle);


	// -------------------------------------------------------------------------
	// UNDEFINED -> TRANSFER_DST
	// -------------------------------------------------------------------------

	const VkImageSubresourceRange fullRange{
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = mipLevels,
		.baseArrayLayer = 0,
		.layerCount = arrayLayers
	};

	ImageBarrier(
		cmd,
		image.vkImage,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		fullRange,

		VK_PIPELINE_STAGE_2_NONE,
		VK_ACCESS_2_NONE,

		VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		VK_ACCESS_2_TRANSFER_WRITE_BIT
	);


	// -------------------------------------------------------------------------
	// Copy regions
	// -------------------------------------------------------------------------

	std::vector<VkBufferImageCopy2> regions;

	regions.reserve(
		static_cast<size_t>(mipLevels) *
		static_cast<size_t>(arrayLayers)
	);

	VkDeviceSize bufferOffset = 0;

	for (uint32_t mip = 0; mip < mipLevels; ++mip)
	{
		const uint32_t mipWidth =
			std::max(1u, image.extent.width >> mip);

		const uint32_t mipHeight =
			std::max(1u, image.extent.height >> mip);

		const uint32_t blocksX =
			(mipWidth + fmt.blockWidth - 1) /
			fmt.blockWidth;

		const uint32_t blocksY =
			(mipHeight + fmt.blockHeight - 1) /
			fmt.blockHeight;

		const VkDeviceSize layerSize =
			static_cast<VkDeviceSize>(blocksX) *
			static_cast<VkDeviceSize>(blocksY) *
			static_cast<VkDeviceSize>(fmt.bytesPerBlock);

		for (uint32_t layer = 0;
			layer < arrayLayers;
			++layer)
		{
			VkBufferImageCopy2 region{
				.sType =
					VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,

				.pNext =
					nullptr,

				.bufferOffset =
					bufferOffset,

				.bufferRowLength =
					0,

				.bufferImageHeight =
					0,

				.imageSubresource = {
					.aspectMask =
						VK_IMAGE_ASPECT_COLOR_BIT,

					.mipLevel =
						mip,

					.baseArrayLayer =
						layer,

					.layerCount =
						1
				},

				.imageOffset = {
					0,
					0,
					0
				},

				.imageExtent = {
					mipWidth,
					mipHeight,
					1
				}
			};

			regions.push_back(region);

			bufferOffset +=
				layerSize;
		}
	}

	assert(bufferOffset == stagingBufferSize);


	// -------------------------------------------------------------------------
	// Copy staging -> image
	// -------------------------------------------------------------------------

	Buffer* stagingBuffer =
		buffers_.get(stagingBufferHandle);

	assert(stagingBuffer != nullptr);

	VkCopyBufferToImageInfo2 copyInfo{
		.sType =
			VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,

		.pNext =
			nullptr,

		.srcBuffer =
			stagingBuffer->vkBuffer,

		.dstImage =
			image.vkImage,

		.dstImageLayout =
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,

		.regionCount =
			static_cast<uint32_t>(regions.size()),

		.pRegions =
			regions.data()
	};

	vkCmdCopyBufferToImage2(
		cmd,
		&copyInfo
	);


	// -------------------------------------------------------------------------
	// TRANSFER_DST -> final usage
	//
	// If the caller wants to keep using it as a transfer destination,
	// leave the image in TRANSFER_DST_OPTIMAL.
	// -------------------------------------------------------------------------

	const bool remainTransferDst =
		dstStages == VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
		dstAccess == VK_ACCESS_2_TRANSFER_WRITE_BIT;

	if (!remainTransferDst)
	{
		ImageBarrier(
			cmd,
			image.vkImage,

			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,

			fullRange,

			VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT,

			dstStages,
			dstAccess
		);
	}


	// -------------------------------------------------------------------------
	// Submit
	// -------------------------------------------------------------------------

	m_submitter->submit(
		submitHandle
	);


	// Update global sync table for image
	{
		ImageHandle found{};
		images_.forEachHandle([&](ImageHandle h) {
			auto* img = images_.get(h);
			if (img && img->vkImage == image.vkImage) found = h;
		});
		if (!found.empty()) {
			VulkanResourceManager::ImageGlobalSyncState st{};
			st.masks.stageMask = FromVkPipelineStage(dstStages);
			if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE)
				st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess));
			st.masks.accessMask = FromVkAccess(dstAccess);
			st.layout = remainTransferDst ? ImageLayout::TransferDstOptimal : ImageLayout::ShaderReadOnlyOptimal;
			// For image uploads that stay in TransferDst, stage is TRANSFER
			if (remainTransferDst) st.masks.stageMask = PipelineStageBit_Transfer;
			SetImageSyncState(found, st);
		}
	}
	// Staging buffer must remain alive until this submission finishes.
	AddDeferredTask(
		std::packaged_task<void()>(
			[
				this,
				staging = stagingBufferHandle
			]
	{
		destroy(
			this,
			staging
		);
	}
		),
		submitHandle
	);
}
[[nodiscard]] smvk::Image smvk::VulkanResourceManager::CreateImageImpl(const ImageCreateInfo& ci)
{
	assert(ci.format != Format::Undefined);
	assert(ci.extent.width > 0);
	assert(ci.extent.height > 0);
	assert(ci.extent.depth > 0);
	assert(ci.mipLevels > 0);
	assert(ci.arrayLayers > 0);
	if (ci.isCubemap) {
		// Vulkan cube rules (VUID-VkImageCreateInfo-flags-00949 etc):
		// 2D, square, 6 (or 6*N) layers, no multisampling.
		assert(ci.type == ImageType::Image2D);
		assert(ci.extent.width == ci.extent.height);
		assert((ci.arrayLayers % 6u) == 0u);
		assert(ci.samples == SampleCount::Count1);
	}

	Image image{};

	image.type = ci.type;
	image.extent = ci.extent;
	image.format = ci.format;
	image.usage = ci.usage;
	image.samples = ci.samples;
	image.mipLevels = ci.mipLevels;
	image.arrayLayers = ci.arrayLayers;
	//image.flags = ci.flags;
	image.ownership = ResourceOwnership::Owned;
	image.isCubemap = ci.isCubemap;
	// Arena-owned copy of the caller's name (caller strings carry no
	// lifetime contract - literals, c_str()s, stack buffers): the pool
	// entry keeps a stable view for ListTextureInfos/debug prints.
	// Arena-owned copy of the caller's name (caller strings carry no
		// lifetime contract - literals, c_str()s, stack buffers): the pool
		// entry keeps a stable view for ListTextureInfos/debug prints.
	// CopyResourceName: the mutex-guarded arena copy (image/texture creation
	// is cross-thread - render + pak-preload workers).
	image.name = CopyResourceName(ci.name);

	VkImageCreateInfo vkCI{
		VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
	};

	//vkCI.flags = toVkImageCreateFlags(ci.flags);
	if (ci.isCubemap)
		vkCI.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	vkCI.imageType = toVkImageType(ci.type);

	vkCI.extent = {
		ci.extent.width,
		ci.extent.height,
		ci.extent.depth
	};

	vkCI.format = toVkFormat(ci.format);
	vkCI.mipLevels = ci.mipLevels;
	vkCI.arrayLayers = ci.arrayLayers;
	vkCI.samples = toVkSampleCount(ci.samples);

	vkCI.tiling = VK_IMAGE_TILING_OPTIMAL;
	vkCI.usage = toVkImageUsageFlags(ci.usage);
	if (ci.hostTransfer)
	{
		assert(m_hostImageCopySupported);
		const VkFormat format = toVkFormat(ci.format);
		assert(IsHostTransferSupported(format));
		vkCI.usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
	}
	vkCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	// EVSM-style async resources: CONCURRENT graphics+compute avoids the
	// per-frame ownership ping-pong (events + barriers order the queues).
	uint32_t concurrentFamilies[3] = {};
	if (ci.concurrentCompute && core_ && core_->indices.computeFamily
		&& core_->indices.graphicsFamily
		&& *core_->indices.computeFamily != *core_->indices.graphicsFamily) {
		concurrentFamilies[0] = *core_->indices.graphicsFamily;
		concurrentFamilies[1] = *core_->indices.computeFamily;
		uint32_t familyCount = 2;
		// The upload streamer copies into TransferDst images from the
		// DEDICATED TRANSFER family (core_->indices.transferFamily - the
		// streamer's queue is created on it): a CONCURRENT image shared
		// with graphics+compute only would be accessed by a family it was
		// NOT shared with. Include the transfer family when distinct.
		if (core_->indices.transferFamily
			&& *core_->indices.transferFamily != *core_->indices.graphicsFamily
			&& *core_->indices.transferFamily != *core_->indices.computeFamily) {
			concurrentFamilies[2] = *core_->indices.transferFamily;
			familyCount = 3;
		}
		vkCI.sharingMode = VK_SHARING_MODE_CONCURRENT;
		vkCI.queueFamilyIndexCount = familyCount;
		vkCI.pQueueFamilyIndices = concurrentFamilies;
		// Metadata MUST match the actual sharing mode (the old code never
		// set it here - the upload streamer's cross-queue ownership
		// decision keys on isConcurrent: a CONCURRENT image with a stale
		// false flag got release/acquire barriers for families it was not
		// shared with, an invalid barrier set).
		image.isConcurrent = true;
	}
	vkCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocCI{};
	allocCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	allocCI.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	// Allocation ledger: capture the EXACT committed bytes (the out-info
	// vmaCreateImage already writes; one extra stack struct, no query).
	VmaAllocationInfo allocInfo{};
	VK_CHECK(vmaCreateImage(
		m_allocator,
		&vkCI,
		&allocCI,
		&image.vkImage,
		&image.allocation,
		&allocInfo
	));
	image.allocBytes = allocInfo.size;

#if defined(_DEBUG)
	if (ci.name)
	{
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};

		nameInfo.objectType = VK_OBJECT_TYPE_IMAGE;
		nameInfo.objectHandle =
			reinterpret_cast<uint64_t>(image.vkImage);
		nameInfo.pObjectName = ci.name;

		SetDebugObjectName(
			device_,
			&nameInfo
		);
	}
#endif

	return image;
}

// Transient-heap image support (design B - see the header contract): the
// raw vkCreateImage mirror of CreateImageImpl WITHOUT the VmaAllocation -
// the object exists unbound for the rendergraph's compile-time handle/view
// resolution; BindHeapImage places it at a packed range later.
[[nodiscard]] smvk::Holder<smvk::ImageHandle> smvk::VulkanResourceManager::CreateHeapImage(
	const ImageCreateInfo& ci)
{
	// Heap placement binds manually - host-transfer images carry binding
	// requirements that path does not honor (they stay on the VMA path).
	assert(!ci.hostTransfer && "hostTransfer images cannot heap-place");

	// Cube rules mirror CreateImageImpl (the shape must be alias-legal for
	// bucket sharers regardless).
	assert(ci.type == ImageType::Image2D || !ci.isCubemap);
	if (ci.isCubemap) {
		assert(ci.extent.width == ci.extent.height);
		assert((ci.arrayLayers % 6u) == 0u);
		assert(ci.samples == SampleCount::Count1);
	}

	VkImageCreateInfo vkCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	// ALIAS_BIT on every heap image: packed sharers overlap ranges by
	// construction (identical parameters + disjoint use intervals), and
	// the flag is harmless for solo-placed ranges (it only opts into the
	// aliasing rules).
	vkCI.flags |= VK_IMAGE_CREATE_ALIAS_BIT;
	if (ci.isCubemap)
		vkCI.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	vkCI.imageType = toVkImageType(ci.type);
	vkCI.extent = { ci.extent.width, ci.extent.height, ci.extent.depth };
	vkCI.format = toVkFormat(ci.format);
	vkCI.mipLevels = ci.mipLevels;
	vkCI.arrayLayers = ci.arrayLayers;
	vkCI.samples = toVkSampleCount(ci.samples);
	vkCI.tiling = VK_IMAGE_TILING_OPTIMAL;
	vkCI.usage = toVkImageUsageFlags(ci.usage);
	vkCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	vkCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	Image image{};
	image.type = ci.type;
	image.extent = ci.extent;
	image.format = ci.format;
	image.usage = ci.usage;
	image.samples = ci.samples;
	image.mipLevels = ci.mipLevels;
	image.arrayLayers = ci.arrayLayers;
	image.ownership = ResourceOwnership::Owned;
	image.isCubemap = ci.isCubemap;
	image.transientHeap = true;
	image.name = CopyResourceName(ci.name);

	VK_CHECK(vkCreateImage(device_, &vkCI, nullptr, &image.vkImage));

#if defined(_DEBUG)
	if (ci.name)
	{
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};
		nameInfo.objectType = VK_OBJECT_TYPE_IMAGE;
		nameInfo.objectHandle = reinterpret_cast<uint64_t>(image.vkImage);
		nameInfo.pObjectName = ci.name;
		SetDebugObjectName(device_, &nameInfo);
	}
#endif

	ImageHandle handle = images_.create(std::move(image));
	SetImageSyncState(handle, ImageGlobalSyncState{});
	return { this, handle };
}

void smvk::VulkanResourceManager::BindHeapImage(ImageHandle handle,
	VkDeviceMemory memory, VkDeviceSize offset)
{
	auto* image = images_.get(handle);
	if (!image || image->vkImage == VK_NULL_HANDLE) {
		assert(false && "BindHeapImage on a dead/uncreated image");
		return;
	}
	assert(image->transientHeap && "BindHeapImage on a non-heap image");
	VkBindImageMemoryInfo bind{
		VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, nullptr,
		image->vkImage, memory, offset };
	VK_CHECK(vkBindImageMemory2(device_, 1, &bind));
}

// ---- Target heap (design-A interval-aliased target memory; the system
// reference is docs/target_heap_packing.md) ----
// The master gate's single storage (see smvk.h's
// TargetHeapAliasingEnabled contract): default OFF -
// TOURMALINE_ALIASING=1 opts in. The gate forces every window to the
// full frame so no range ever shares (disjoint-range placement only,
// always sound).
namespace {
	bool& TargetHeapAliasingGate() {
		static bool value = [] {
			const char* e = std::getenv("TOURMALINE_ALIASING");
			return e != nullptr && std::atoi(e) == 1;
		}();
		return value;
	}
}
bool smvk::TargetHeapAliasingEnabled() {
	return TargetHeapAliasingGate();
}
void smvk::SetTargetHeapAliasing(bool on) {
	TargetHeapAliasingGate() = on;
}

// See the resource_manager.h contract + the state block in
// vulkan_impl.h: the same packing as the rendergraph's
// CompileTransientHeap (shape buckets + interval slots + VMA virtual
// blocks over big device-local blocks), with slot live-user refcounts -
// graph-EXTERNAL targets claim by declared [firstPhase, lastPhase]
// windows (canonical TargetPhase order).
[[nodiscard]] smvk::Holder<smvk::TargetHeapClaimHandle>
smvk::VulkanResourceManager::ClaimTargetHeapRange(
	ImageHandle image, TargetPhase firstPhase, TargetPhase lastPhase)
{
	auto* img = images_.get(image);
	if (!img || img->vkImage == VK_NULL_HANDLE || !img->transientHeap) {
		assert(false && "ClaimTargetHeapRange needs an unbound heap image");
		return {};
	}
	VkMemoryRequirements reqs{};
	vkGetImageMemoryRequirements(device_, img->vkImage, &reqs);
	const VkDeviceSize need = (reqs.size + reqs.alignment - 1)
		/ reqs.alignment * reqs.alignment;
	// Shape identity off the Image's own fields (the aliasing parameter
	// set: everything CreateHeapImage received except the debug name).
	uint64_t shape = 1469598103934665603ull;
	{
		const auto mix = [&shape](uint64_t v) {
			shape = (shape ^ v) * 1099511628211ull;
		};
		mix(static_cast<uint64_t>(img->type));
		mix(img->extent.width); mix(img->extent.height);
		mix(img->extent.depth);
		mix(static_cast<uint64_t>(img->format));
		mix(static_cast<uint64_t>(img->usage));
		mix(static_cast<uint64_t>(img->samples));
		mix(img->mipLevels); mix(img->arrayLayers);
		mix(img->isCubemap ? 1u : 0u);
	}
	// The master gate (see the gate block above): OFF forces the full
	// frame - the span-disjointness check fails against every live slot,
	// so every claim packs to a DISJOINT range (plain multi-bind
	// placement, no aliasing, always sound). The declared windows are
	// honored only when opted in.
	if (!TargetHeapAliasingEnabled()) {
		firstPhase = TargetPhase::Shadows;
		lastPhase = TargetPhase::Composite;
	}
	const uint32_t first = static_cast<uint32_t>(firstPhase);
	const uint32_t last = static_cast<uint32_t>(lastPhase);

	// Join a live slot: same shape + the newcomer's window is DISJOINT
	// from every live user's span (arrival-order-independent: a wide
	// first claimer never blocks an entirely-earlier newcomer, and vice
	// versa). Tombstones (liveUsers == 0, the range retiring) never join -
	// their ranges are freeing.
	for (uint32_t s = 0; s < m_targetHeap.slots.size(); ++s) {
		TargetHeapState::Slot& slot = m_targetHeap.slots[s];
		if (slot.liveUsers == 0 || slot.shape != shape)
			continue;
		bool disjoint = true;
		for (const auto& span : slot.liveSpans)
			if (!(last < span.first || first > span.second)) {
				disjoint = false;
				break;
			}
		if (!disjoint)
			continue;
		++slot.liveUsers;
		slot.liveSpans.push_back({ first, last });
		BindHeapImage(image,
			m_targetHeap.blocks[slot.block].memory, slot.offset);
		m_targetHeap.aliasedBytes += need;
		++m_targetHeap.claimCount;
		std::fprintf(stderr,
			"[TargetHeap] claim #%u: shape 0x%llx window [%u..%u] -> "
			"JOIN slot %u (offset %llu, %.1f MiB shared)\n",
			m_targetHeap.claimCount,
			(unsigned long long)shape, first, last, s,
			(unsigned long long)slot.offset,
			double(need) / 1048576.0);
		return { this, targetHeapClaims_.create(
			{ s, shape, firstPhase, lastPhase }) };
	}

	// New slot: first-fit into an existing compatible block, else grow
	// one (the size ladder honors the OOM policy: loud abort).
	uint32_t block = ~0u;
	VkDeviceSize offset = 0;
	VmaVirtualAllocation alloc = VK_NULL_HANDLE;
	for (uint32_t bi = 0; bi < m_targetHeap.blocks.size(); ++bi) {
		if (!((reqs.memoryTypeBits
			>> m_targetHeap.blocks[bi].memoryTypeIndex) & 1u))
			continue;
		VmaVirtualAllocationCreateInfo vi{};
		vi.size = need;
		vi.alignment = reqs.alignment;
		VkDeviceSize off = 0;
		VmaVirtualAllocation a = VK_NULL_HANDLE;
		if (vmaVirtualAllocate(m_targetHeap.blocks[bi].virtualBlock,
			&vi, &a, &off) == VK_SUCCESS) {
			block = bi;
			offset = off;
			alloc = a;
			break;
		}
	}
	if (block == ~0u) {
		// Grow: lowest set memoryTypeBit that is device-local.
		uint32_t typeIndex = ~0u;
		{
			VkPhysicalDeviceMemoryProperties memProps{};
			vkGetPhysicalDeviceMemoryProperties(physicalDevice_,
				&memProps);
			for (uint32_t t = 0; t < memProps.memoryTypeCount; ++t) {
				if (!((reqs.memoryTypeBits >> t) & 1u))
					continue;
				if (!(memProps.memoryTypes[t].propertyFlags
					& VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
					continue;
				typeIndex = t;
				break;
			}
			if (typeIndex == ~0u) {
				std::fprintf(stderr,
					"[TargetHeap] no device-local memory type in mask "
					"0x%x - aborting\n", reqs.memoryTypeBits);
				std::fflush(stderr);
				std::abort();
			}
		}
		VkDeviceSize size = smvk::mem::config::Runtime().targetHeapBlock; // ladder: halves down to the request
		VkDeviceSize allocSize = 0;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		while (true) {
			allocSize = std::max(size, need);
			VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				nullptr, allocSize, typeIndex };
			if (vkAllocateMemory(device_, &ai, nullptr, &memory)
				== VK_SUCCESS)
				break;
			if (size <= need) {
				std::fprintf(stderr,
					"[TargetHeap] heap block allocation of %llu bytes "
					"failed. No recovery - aborting.\n",
					(unsigned long long)allocSize);
				std::fflush(stderr);
				std::abort();
			}
			size /= 2;
		}
		VmaVirtualBlockCreateInfo vbci{};
		vbci.size = allocSize;
		VmaVirtualBlock vb = VK_NULL_HANDLE;
		VK_CHECK(vmaCreateVirtualBlock(&vbci, &vb));
		m_targetHeap.blocks.push_back({ memory, allocSize, typeIndex, vb });
		VmaVirtualAllocationCreateInfo vi{};
		vi.size = need;
		vi.alignment = reqs.alignment;
		VkDeviceSize off = 0;
		VK_CHECK(vmaVirtualAllocate(vb, &vi, &alloc, &off));
		block = static_cast<uint32_t>(m_targetHeap.blocks.size() - 1);
		offset = off;
	}
	m_targetHeap.slots.push_back({ shape, block, alloc, offset,
		need, 1,{ { first, last } } });
	m_targetHeap.rangeBytes += need;
	++m_targetHeap.claimCount;
	BindHeapImage(image, m_targetHeap.blocks[block].memory, offset);
	std::fprintf(stderr,
		"[TargetHeap] claim #%u: shape 0x%llx window [%u..%u] -> NEW slot "
		"%zu (block %u, offset %llu, %.1f MiB; ranges %.1f MiB, shared "
		"%.1f MiB)\n",
		m_targetHeap.claimCount,
		(unsigned long long)shape, first, last,
		m_targetHeap.slots.size() - 1, block,
		(unsigned long long)offset,
		double(need) / 1048576.0,
		double(m_targetHeap.rangeBytes) / 1048576.0,
		double(m_targetHeap.aliasedBytes) / 1048576.0);
	return { this, targetHeapClaims_.create({ static_cast<uint32_t>(
		m_targetHeap.slots.size() - 1), shape, firstPhase, lastPhase }) };
}

void smvk::VulkanResourceManager::ReleaseTargetHeapClaim(
	TargetHeapClaimHandle claim)
{
	if (claim.empty())
		return;
	auto* c = targetHeapClaims_.get(claim);
	if (!c)
		return;
	// Capture everything off the claim BEFORE the pool destroy (the entry
	// dies below; the span fields must not be read after).
	const uint32_t s = c->slot;
	const uint32_t f = static_cast<uint32_t>(c->firstPhase);
	const uint32_t l = static_cast<uint32_t>(c->lastPhase);
	targetHeapClaims_.destroy(claim);
	if (s >= m_targetHeap.slots.size())
		return;
	TargetHeapState::Slot& slot = m_targetHeap.slots[s];
	if (slot.liveUsers == 0)
		return; // stale (defensive)
	// Remove this claim's span from the live-span set (the remaining
	// live users' spans keep the slot's sharing state correct).
	for (auto it = slot.liveSpans.begin(); it != slot.liveSpans.end();
		++it)
		if (it->first == f && it->second == l) {
			slot.liveSpans.erase(it);
			break;
		}
	if (--slot.liveUsers > 0)
		return;
	// The LAST user: the shared range frees RETIREMENT-ORDERED -
	// DeferDestroy appends at the current point (the caller already
	// destroyed the image, so this free lands BEHIND that destroy in the
	// FIFO; a future claim can never re-bind the range while a previous
	// binding's image object still exists). The slot stays as a
	// tombstone (liveUsers == 0): never joined, its index stays stable
	// for the remaining live claims.
	const VmaVirtualBlock vb =
		m_targetHeap.blocks[slot.block].virtualBlock;
	const VmaVirtualAllocation alloc = slot.allocation;
	// TOURMALINE_DEFER_DIAG: the range-free execution census — pairs
	// with the enqueue diag (the editor's marching offsets vs 03_fuzz's
	// cycling: do the frees RUN?).
	static const bool releaseDiag = [] {
		const char* e = std::getenv("TOURMALINE_DEFER_DIAG");
		return e && std::atoi(e) != 0;
	}();
	int* freeCount = nullptr;
	if (releaseDiag) {
		static int frees = 0;
		freeCount = &frees;
	}
	DeferDestroy(std::packaged_task<void()>([vb, alloc, freeCount,
		block = slot.block, off = slot.offset]() {
		// TOURMALINE_DEFER_DIAG: the freed identity — pairs with the
		// claim line's (block, offset) to see reuse (or the lack of it).
		static const bool frDiag = [] {
			const char* e = std::getenv("TOURMALINE_DEFER_DIAG");
			return e && std::atoi(e) != 0;
		}();
		if (frDiag)
			std::fprintf(stderr,
				"[DEFER-DIAG] FREE block=%u offset=%llu\n",
				block, (unsigned long long)off);
		vmaVirtualFree(vb, alloc);
		if (freeCount && ++*freeCount % 10 == 1)
			std::fprintf(stderr,
				"[DEFER-DIAG] vmaVirtualFree RAN (frees~%d)\n",
				*freeCount);
	}));
}

[[nodiscard]] smvk::Holder<smvk::ImageHandle> smvk::VulkanResourceManager::CreateImage(ImageCreateInfo ci, const void* data)
{
	Image image = CreateImageImpl(ci);

	ImageHandle handle =
		images_.create(std::move(image));
	SetImageSyncState(handle, ImageGlobalSyncState{});

	Image* imagePtr = images_.get(handle);
	if (data)
	{

		assert(
			((imagePtr->usage & ImageUsage::TransferDst) != ImageUsage::None) &&
			"Initial image data requires TransferDst"
		);

		UploadDataToImage2D(
			*imagePtr,
			data,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT,
			0
		);
	}
	else {


		// this is only a good solution if we go full GENERAL layout.
		// havent decided yet

		//VkImageSubresourceRange fullRange{
		//	.aspectMask = VkAspectMaskFromFormat(toVkFormat(imagePtr->format)),
		//	.baseMipLevel = 0,
		//	.levelCount = VK_REMAINING_MIP_LEVELS,
		//	.baseArrayLayer = 0,
		//	.layerCount = VK_REMAINING_ARRAY_LAYERS
		//};

		//auto han = m_submitter->acquire();
		//auto cmd = m_submitter->GetCommandBuffer(han);
		//// This should be added to a current command, instead of just spawning one submit!.
		//ImageBarrier(
		//	cmd.cmd,
		//	imagePtr->vkImage,

		//	VK_IMAGE_LAYOUT_UNDEFINED,
		//	VK_IMAGE_LAYOUT_GENERAL,
		//	fullRange,
		//	VK_PIPELINE_STAGE_2_NONE,
		//	0,

		//	VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
		//	VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
		//	VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
		//	VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
		//	VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
		//);
		//m_submitter->submit(han);
		//m_submitter->wait(han);
	}

	return Holder<ImageHandle>{
		this,
			handle
	};
}
[[nodiscard]] smvk::ImageSubresourceRange smvk::VulkanResourceManager::ResolveImageSubresourceRange(const Image& image, ImageSubresourceRange range) const
{
	if (range.mipLevelCount == RemainingSubresources)
	{
		range.mipLevelCount =
			image.mipLevels - range.baseMipLevel;
	}

	if (range.arrayLayerCount == RemainingSubresources)
	{
		range.arrayLayerCount =
			image.arrayLayers - range.baseArrayLayer;
	}

	assert(
		range.baseMipLevel + range.mipLevelCount <=
		image.mipLevels
	);

	assert(
		range.baseArrayLayer + range.arrayLayerCount <=
		image.arrayLayers
	);

	return range;
}
// ---------------------------------------------------------------------------
// STRICT RESOURCES (see smvk.h): --strict / TOURMALINE_STRICT=1. Default is
// the graceful degradation (editor sessions survive budget pressure);
// strict turns exhaustion into a LOUD abort — silent skips hide leaks.
// Device-level knob: getenv at first use (the RunConfig.h migration
// discipline for device/debug knobs).
// ---------------------------------------------------------------------------
// Fatal-diagnostics helpers (defined further down, next to the device-fault
// post-mortem): forward-declared here so the strict gate can use them.
namespace {
	void FatalMirrorOpen();
	void FatalPrintf(const char* fmt, ...);
	void FatalFlush();
	void FatalMirrorClose();
}
namespace smvk {
	static bool g_strictResources = false;
	void SetStrictResources(bool on) { g_strictResources = on; }
	bool StrictResources() {
		static const bool envOn = [] {
			const char* e = std::getenv("TOURMALINE_STRICT");
			return e && *e && std::strcmp(e, "0") != 0;
		}();
		return g_strictResources || envOn;
	}
	// CALL-SITE exhaustion gate (the RHI stays PURE — see smvk.h). No-op
	// unless strict; aborts with the fatal diagnostic when it is.
	void OnResourceExhaustion(const char* site) {
		if (!StrictResources())
			return;
		FatalMirrorOpen();
		FatalPrintf("[STRICT] resource exhaustion at '%s' — a pool/budget "
			"refused a creation. Raise the budget, fix the leak, or rerun "
			"without --strict to degrade gracefully.\n",
			site ? site : "?");
		FatalFlush();
		FatalMirrorClose();
		std::abort();
	}
} // namespace smvk

[[nodiscard]] smvk::Holder<smvk::BindlessImageHandle> smvk::VulkanResourceManager::CreateBindlessImage(ImageViewHandle viewHandle, VkImageLayout readLayout)
{
	ImageView* view = imageViews_.get(viewHandle);
	assert(view);

	BindlessImage bindless{};
	bindless.view = viewHandle;
	bindless.imageLayout = readLayout;

	BindlessImageHandle handle =
		bindlessImages_.create(std::move(bindless));

	// Pool-full (or any create failure) returns an EMPTY handle — the
	// RHI's error signal. The RHI itself stays PURE: no aborts here, no
	// strict checks — handling is the CALLER's responsibility (see
	// smvk.h + AGENTS.md: call sites gate with OnResourceExhaustion;
	// asserts inside the RHI are for invariants only).
	if (handle.empty())
		return {};

	BindlessImage* resource =
		bindlessImages_.get(handle);
	assert(resource);

	// The bindless pool is intentionally also our descriptor-slot allocator.
	// NOTE (sharded pools): handle.index() is the global sharded index
	// (shard * kShardCapacity + local). It doubles as the descriptor-array
	// slot, so it must stay below MAX_TEXTURES – guaranteed by the
	// static_assert in Memory/pool.h (MAX_TEXTURES >= kShardedMaxSize);
	// this runtime check stays as a loud failure instead of corrupting writes.
	resource->descriptorIndex = handle.index();
	assert(resource->descriptorIndex < MAX_TEXTURES && "bindless descriptor budget exhausted (sharded indices are sparse – raise MAX_TEXTURES or pack slots)");

	m_needToUpdateTextures = true;

	return Holder<BindlessImageHandle>{
		this,
			handle
	};
}
[[nodiscard]] smvk::Holder<smvk::TextureHandle>smvk::VulkanResourceManager::CreateTexture(ImageCreateInfo imageCreateInfo, const void* data)
{
	assert(
		((imageCreateInfo.usage & ImageUsage::Sampled) != ImageUsage::None) &&
		"CreateTexture requires ImageUsage::Sampled"
	);

	// ---------------------------------------------------------------------
	// Image
	// ---------------------------------------------------------------------

	auto image = CreateImage(
		imageCreateInfo,
		nullptr
	);

	const Image* imageResource =
		images_.get(image);

	assert(imageResource);
	if (data)
	{
		UploadDataToImage2D(
			*imageResource,
			data,

			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,

			0
		);
	}


	// ---------------------------------------------------------------------
	// Default sampled view
	// ---------------------------------------------------------------------

	ImageViewCreateInfo viewCI{};

	if (imageResource->isCubemap)
	{
		assert(imageResource->type == ImageType::Image2D);
		assert((imageResource->arrayLayers % 6u) == 0u);
		viewCI.viewType = imageResource->arrayLayers > 6u
			? ImageViewType::ViewCubeArray
			: ImageViewType::ViewCube;
	}
	else if (imageResource->type == ImageType::Image3D)
	{
		viewCI.viewType = ImageViewType::View3D;
	}
	else if (imageResource->arrayLayers > 1)
	{
		viewCI.viewType = ImageViewType::View2DArray;
	}
	else
	{
		viewCI.viewType = ImageViewType::View2D;
	}

	// Inherit underlying image format.
	viewCI.format = Format::Undefined;

	viewCI.subresourceRange = {
		.aspect = DefaultSampleAspect(imageResource->format),

		.baseMipLevel = 0,
		.mipLevelCount = imageResource->mipLevels,

		.baseArrayLayer = 0,
		.arrayLayerCount = imageResource->arrayLayers
	};

	viewCI.name = imageCreateInfo.name;
	// maybe append "view"

	auto view = CreateImageView(
		image,
		viewCI
	);

	const ImageViewHandle viewHandle = view;

	// ---------------------------------------------------------------------
	// Bindless descriptor
	// ---------------------------------------------------------------------

	auto bindless =
		CreateBindlessImage(viewHandle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// ---------------------------------------------------------------------
	// Texture owns all three resources.
	// ---------------------------------------------------------------------

	Texture texture{
		.image = image.release(),
		.view = view.release(),
		.bindless = bindless.release()
	};

	return Holder<TextureHandle>{
		this,
			textures_.create(std::move(texture))
	};
}
static VkFormat toSRGBVkFormat(VkFormat format)
{
	// Returns the sRGB variant of a UNORM color format (so sampling auto-linearizes),
	// or the same format if it has no sRGB counterpart / is already sRGB.
	// Returns the sRGB Vulkan variant of a UNORM color format.
	// Returns VK_FORMAT_UNDEFINED if there is no supported sRGB counterpart.
	switch (format)
	{
	case VK_FORMAT_R8G8B8A8_UNORM:
		return VK_FORMAT_R8G8B8A8_SRGB;

	case VK_FORMAT_B8G8R8A8_UNORM:
		return VK_FORMAT_B8G8R8A8_SRGB;

	case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
		return VK_FORMAT_BC1_RGB_SRGB_BLOCK;

	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
		return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;

	case VK_FORMAT_BC2_UNORM_BLOCK:
		return VK_FORMAT_BC2_SRGB_BLOCK;

	case VK_FORMAT_BC3_UNORM_BLOCK:
		return VK_FORMAT_BC3_SRGB_BLOCK;

	case VK_FORMAT_BC7_UNORM_BLOCK:
		return VK_FORMAT_BC7_SRGB_BLOCK;


		// Already sRGB.
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
	case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
	case VK_FORMAT_BC2_SRGB_BLOCK:
	case VK_FORMAT_BC3_SRGB_BLOCK:
	case VK_FORMAT_BC7_SRGB_BLOCK:
		return format;

	default:
		return VK_FORMAT_UNDEFINED;
	}
}
[[nodiscard]] smvk::Holder<smvk::TextureHandle> smvk::VulkanResourceManager::CreateTexture(ktxTexture2& texture, const char* debugName, ImageCreateInfo imageCreateInfo)
{
	// -------------------------------------------------------------------------
	// KTX metadata
	//
	// `srgb` is an override:
	//
	//   true  -> force sRGB
	//   false -> use KTX transfer-function metadata
	//
	// Read this before transcoding so the decision reflects the source asset.
	// -------------------------------------------------------------------------

	const khr_df_transfer_e transferFunction =
		ktxTexture2_GetTransferFunction_e(
			&texture
		);

	const bool ktxIsSRGB =
		transferFunction == KHR_DF_TRANSFER_SRGB;

	const bool useSRGB =
		/*	srgb || */ktxIsSRGB;


	// -------------------------------------------------------------------------
	// Validate dimensions
	// -------------------------------------------------------------------------

	if (texture.numDimensions < 1 ||
		texture.numDimensions > 3)
	{
		throw std::runtime_error(
			"KTX texture has invalid dimensionality."
		);
	}

	const bool is1D =
		texture.numDimensions == 1;

	const bool is2D =
		texture.numDimensions == 2;

	const bool is3D =
		texture.numDimensions == 3;

	const bool isCubemap =
		texture.isCubemap == KTX_TRUE;

	if (isCubemap && !is2D)
	{
		throw std::runtime_error(
			"KTX cubemap must be a 2D texture."
		);
	}


	// -------------------------------------------------------------------------
	// Transcode
	// -------------------------------------------------------------------------

	if (ktxTexture2_NeedsTranscoding(&texture))
	{
		const KTX_error_code result =
			ktxTexture2_TranscodeBasis(
				&texture,
				KTX_TTF_BC7_RGBA,
				0
			);

		if (result != KTX_SUCCESS)
		{
			throw std::runtime_error(
				ktxErrorString(result)
			);
		}
	}


	// We currently expect pre-generated mipmaps.
	texture.generateMipmaps =
		KTX_FALSE;


	// -------------------------------------------------------------------------
	// Format
	// -------------------------------------------------------------------------

	const VkFormat sourceVkFormat =
		ktxTexture2_GetVkFormat(
			&texture
		);

	if (sourceVkFormat == VK_FORMAT_UNDEFINED)
	{
		throw std::runtime_error(
			"KTX texture has undefined VkFormat after transcoding."
		);
	}


	VkFormat imageVkFormat =
		sourceVkFormat;

	if (useSRGB)
	{
		imageVkFormat =
			toSRGBVkFormat(
				sourceVkFormat
			);

		if (imageVkFormat == VK_FORMAT_UNDEFINED)
		{
			throw std::runtime_error(
				"KTX texture requires sRGB sampling, "
				"but its Vulkan format has no supported sRGB variant."
			);
		}
	}


	const Format format =
		fromVkFormat(
			imageVkFormat
		);

	if (format == Format::Undefined)
	{
		throw std::runtime_error(
			"KTX texture uses a format unsupported by smvk::Format."
		);
	}


	// -------------------------------------------------------------------------
	// Description
	//
	// Vulkan requires unused dimensions to be exactly 1:
	//
	// 1D -> width x 1 x 1
	// 2D -> width x height x 1
	// 3D -> width x height x depth
	// -------------------------------------------------------------------------

	const uint32_t width =
		std::max(
			1u,
			texture.baseWidth
		);

	const uint32_t height =
		is1D
		? 1u
		: std::max(
			1u,
			texture.baseHeight
		);

	const uint32_t depth =
		is3D
		? std::max(
			1u,
			texture.baseDepth
		)
		: 1u;


	const uint32_t mipLevels =
		std::max(
			1u,
			texture.numLevels
		);

	const uint32_t ktxLayers =
		std::max(
			1u,
			texture.numLayers
		);

	const uint32_t faces =
		std::max(
			1u,
			texture.numFaces
		);

	const uint32_t arrayLayers =
		ktxLayers * faces;


	ImageUsage usage =
		ImageUsage::Sampled |
		ImageUsage::TransferDst;

	if (texture.generateMipmaps)
	{
		usage |=
			ImageUsage::TransferSrc;
	}


	// -------------------------------------------------------------------------
	// Build copy regions FIRST
	// -------------------------------------------------------------------------

	auto* base =
		reinterpret_cast<ktxTexture*>(
			&texture
			);

	const ktx_size_t ktxDataSize =
		ktxTexture_GetDataSizeUncompressed(
			base
		);

	ktx_uint8_t* ktxData =
		ktxTexture_GetData(
			base
		);

	if (!ktxData ||
		ktxDataSize == 0)
	{
		throw std::runtime_error(
			"KTX texture contains no image data."
		);
	}


	std::vector<VkBufferImageCopy> regions;

	regions.reserve(
		mipLevels *
		ktxLayers *
		faces
	);


	for (uint32_t mip = 0;
		mip < mipLevels;
		++mip)
	{
		const uint32_t mipWidth =
			std::max(
				1u,
				width >> mip
			);

		const uint32_t mipHeight =
			is1D
			? 1u
			: std::max(
				1u,
				height >> mip
			);

		const uint32_t mipDepth =
			is3D
			? std::max(
				1u,
				depth >> mip
			)
			: 1u;


		for (uint32_t layer = 0;
			layer < ktxLayers;
			++layer)
		{
			for (uint32_t face = 0;
				face < faces;
				++face)
			{
				ktx_size_t offset =
					0;

				const KTX_error_code result =
					ktxTexture2_GetImageOffset(
						&texture,
						mip,
						layer,
						face,
						&offset
					);

				if (result != KTX_SUCCESS)
				{
					throw std::runtime_error(
						"ktxTexture2_GetImageOffset failed."
					);
				}


				VkBufferImageCopy region{};

				region.bufferOffset =
					static_cast<VkDeviceSize>(
						offset
						);

				region.bufferRowLength =
					0;

				region.bufferImageHeight =
					0;


				region.imageSubresource.aspectMask =
					VK_IMAGE_ASPECT_COLOR_BIT;

				region.imageSubresource.mipLevel =
					mip;

				region.imageSubresource.baseArrayLayer =
					is3D
					? 0
					: layer * faces + face;

				region.imageSubresource.layerCount =
					1;


				region.imageOffset = {
					0,
					0,
					0
				};

				region.imageExtent = {
					mipWidth,
					mipHeight,
					mipDepth
				};


				regions.push_back(
					region
				);
			}
		}
	}


	// -------------------------------------------------------------------------
	// VkImage
	// -------------------------------------------------------------------------

	VkImageCreateInfo imageCI{
		VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
	};

	imageCI.flags =
		isCubemap
		? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
		: 0;


	if (is1D)
	{
		imageCI.imageType =
			VK_IMAGE_TYPE_1D;
	}
	else if (is2D)
	{
		imageCI.imageType =
			VK_IMAGE_TYPE_2D;
	}
	else
	{
		imageCI.imageType =
			VK_IMAGE_TYPE_3D;
	}


	// Create the image in its actual sampled color space.
	imageCI.format =
		imageVkFormat;

	imageCI.extent = {
		width,
		height,
		depth
	};

	imageCI.mipLevels =
		mipLevels;

	imageCI.arrayLayers =
		is3D
		? 1u
		: arrayLayers;

	imageCI.samples =
		VK_SAMPLE_COUNT_1_BIT;

	imageCI.tiling =
		VK_IMAGE_TILING_OPTIMAL;

	imageCI.usage =
		toVkImageUsageFlags(
			usage
		);
	if (imageCreateInfo.hostTransfer)
	{
		assert(m_hostImageCopySupported);
		assert(IsHostTransferSupported(imageCI.format));
		imageCI.usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
	}
	// Explicit per-image opt-in for host transfer (KTX). Keep disabled
	// initially; set to true for exactly one test texture when re-enabling.
	const bool useHostCopyForThisTexture = false;
	if (useHostCopyForThisTexture)
	{
		assert(m_hostImageCopySupported);
		assert(IsHostTransferSupported(imageCI.format));
		imageCI.usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
	}

	// Streamer targets (TransferDst) are uploaded on the transfer queue and
	// re-uploaded by streaming retargets while the graphics queue may still
	// sample them; CONCURRENT avoids per-upload queue-family ownership
	// ping-pong (same argument as CreateBuffer's vertex/index/storage path).
	bool concurrentSharing = m_transferSubmitter && m_transferSubmitter != m_submitter &&
		(usage & ImageUsage::TransferDst) != ImageUsage::None;
	imageCI.sharingMode =
		concurrentSharing ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
	if (concurrentSharing) {
		uint32_t families[2] = {};
		families[0] = m_submitter->queueFamilyIndex();
		families[1] = m_transferSubmitter->queueFamilyIndex();
		imageCI.queueFamilyIndexCount = 2;
		imageCI.pQueueFamilyIndices = families;
	}

	imageCI.initialLayout =
		VK_IMAGE_LAYOUT_UNDEFINED;


	// -------------------------------------------------------------------------
	// Allocation
	// -------------------------------------------------------------------------

	VmaAllocationCreateInfo allocCI{};

	allocCI.usage =
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	allocCI.requiredFlags =
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;


	VkImage vkImage =
		VK_NULL_HANDLE;

	VmaAllocation allocation =
		VK_NULL_HANDLE;


	VmaAllocationInfo ledgerInfo{};
	VK_CHECK(
		vmaCreateImage(
			m_allocator,
			&imageCI,
			&allocCI,
			&vkImage,
			&allocation,
			&ledgerInfo
		)
	);


	// -------------------------------------------------------------------------
	// Image resource
	// -------------------------------------------------------------------------

	Image image{};

	image.vkImage =
		vkImage;

	image.allocation =
		allocation;

	// Allocation ledger: exact committed bytes (see the smvk.h block).
	image.allocBytes = ledgerInfo.size;


	if (is1D)
	{
		image.type =
			ImageType::Image1D;
	}
	else if (is2D)
	{
		image.type =
			ImageType::Image2D;
	}
	else
	{
		image.type =
			ImageType::Image3D;
	}


	image.extent = {
		width,
		height,
		depth
	};

	image.format =
		format;

	image.usage =
		usage;

	image.samples =
		SampleCount::Count1;

	image.mipLevels =
		mipLevels;

	image.arrayLayers =
		imageCI.arrayLayers;

	image.liveViewCount =
		0;

	image.ownership =
		ResourceOwnership::Owned;

	image.isConcurrent = concurrentSharing;
	image.isCubemap = isCubemap;
	// Arena-owned copy (same contract as CreateImageImpl:
	// CopyResourceName - cross-thread mutex-guarded arena copy).
	image.name = CopyResourceName(debugName);


	const ImageHandle imageHandle =
		images_.create(
			std::move(
				image
			)
		);

	Holder<ImageHandle> imageHolder{
		this,
		imageHandle
	};


	// -------------------------------------------------------------------------
	// Debug name
	// -------------------------------------------------------------------------

#if defined(_DEBUG)

	if (debugName)
	{
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};

		nameInfo.objectType =
			VK_OBJECT_TYPE_IMAGE;

		nameInfo.objectHandle =
			reinterpret_cast<uint64_t>(
				vkImage
				);

		nameInfo.pObjectName =
			debugName;

		SetDebugObjectName(
			device_,
			&nameInfo
		);
	}

#else

	(void)debugName;

#endif


	// -------------------------------------------------------------------------
	// Default sampled ImageView
	// -------------------------------------------------------------------------

	ImageViewCreateInfo viewCI{};


	if (isCubemap)
	{
		viewCI.viewType =
			ktxLayers > 1
			? ImageViewType::ViewCubeArray
			: ImageViewType::ViewCube;
	}
	else if (is1D)
	{
		viewCI.viewType =
			ktxLayers > 1
			? ImageViewType::View1DArray
			: ImageViewType::View1D;
	}
	else if (is3D)
	{
		viewCI.viewType =
			ImageViewType::View3D;
	}
	else if (ktxLayers > 1)
	{
		viewCI.viewType =
			ImageViewType::View2DArray;
	}
	else
	{
		viewCI.viewType =
			ImageViewType::View2D;
	}


	// VkImage already has the final UNORM/sRGB format.
	viewCI.format =
		Format::Undefined;

	viewCI.subresourceRange = {
		.aspect =
			ImageAspect::Color,

		.baseMipLevel =
			0,

		.mipLevelCount =
			mipLevels,

		.baseArrayLayer =
			0,

		.arrayLayerCount =
			imageCI.arrayLayers
	};

	viewCI.name =
		debugName;


	auto viewHolder =
		CreateImageView(
			imageHandle,
			viewCI
		);


	// -------------------------------------------------------------------------
	// Bindless sampled image
	// -------------------------------------------------------------------------

	auto bindlessHolder =
		CreateBindlessImage(
			viewHolder,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);


	// -------------------------------------------------------------------------
	// Staging buffer
	// -------------------------------------------------------------------------

	// Use UploadStreamer for KTX (batched, 256MB threshold, host-copy first, EXCLUSIVE ownership)
	{
		// KTX images are sampled, final layout SHADER_READ_ONLY (keep TRANSFER_DST false to save transition, global sync will handle)
		m_streamer->uploadImageKtx(imageHandle, ktxData, ktxDataSize, regions, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	}
	// -------------------------------------------------------------------------
	// Texture
	// -------------------------------------------------------------------------

	Texture textureResource{
		.image =
			imageHolder.release(),

		.view =
			viewHolder.release(),

		.bindless =
			bindlessHolder.release()
	};


	return Holder<TextureHandle>{
		this,
			textures_.create(
				std::move(
					textureResource
				)
			)
	};
}

namespace {
	// -------------------------------------------------------------------------
	// Texture-streaming helpers: KTX2 mip-subrange upload plan.
	//
	// A streamed texture keeps the full mip chain conceptually but only the
	// contiguous tail [firstMip, firstMip + residentMips) is backed by VRAM.
	// The VkImage is allocated at the subrange resolution (base extent >>
	// firstMip) with residentMips levels; copy regions address full-chain mips
	// but are remapped (mipLevel = globalMip - firstMip) so level 0 of the
	// image is full-chain mip firstMip. Normalized-UV sampling is unaffected.
	// -------------------------------------------------------------------------
	struct KtxStreamPlan {
		VkFormat vkFormat = VK_FORMAT_UNDEFINED;
		smvk::Format format = smvk::Format::Undefined;
		smvk::ImageUsage usage =
			smvk::ImageUsage::Sampled |
			smvk::ImageUsage::TransferDst;
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;
		uint32_t firstMip = 0;
		uint32_t residentMips = 1;
		uint32_t fullMips = 1;
		uint32_t arrayLayers = 1;
		uint32_t ktxLayers = 1;
		uint32_t faces = 1;
		bool is1D = false;
		bool is2D = true;
		bool is3D = false;
		bool isCubemap = false;
		ktx_uint8_t* data = nullptr;
		ktx_size_t dataSize = 0;
		std::vector<VkBufferImageCopy> regions;
	};

	// Transcodes (Basis -> BC7 when needed) and builds the copy regions for
	// [firstMip, firstMip + mipCount). Throws std::runtime_error on bad input.
	// Mirrors the validation/transcode/format path of CreateTexture(ktx...).
	KtxStreamPlan PrepareKtxStreamPlan(ktxTexture2& texture, uint32_t firstMip, uint32_t mipCount)
	{
		KtxStreamPlan plan{};

		const khr_df_transfer_e transferFunction =
			ktxTexture2_GetTransferFunction_e(&texture);
		const bool useSRGB =
			transferFunction == KHR_DF_TRANSFER_SRGB;

		if (texture.numDimensions < 1 ||
			texture.numDimensions > 3)
		{
			throw std::runtime_error(
				"KTX texture has invalid dimensionality."
			);
		}

		plan.is1D = texture.numDimensions == 1;
		plan.is2D = texture.numDimensions == 2;
		plan.is3D = texture.numDimensions == 3;
		plan.isCubemap = texture.isCubemap == KTX_TRUE;

		if (plan.isCubemap && !plan.is2D)
		{
			throw std::runtime_error(
				"KTX cubemap must be a 2D texture."
			);
		}

		if (ktxTexture2_NeedsTranscoding(&texture))
		{
			const KTX_error_code result =
				ktxTexture2_TranscodeBasis(
					&texture,
					KTX_TTF_BC7_RGBA,
					0
				);

			if (result != KTX_SUCCESS)
			{
				throw std::runtime_error(
					ktxErrorString(result)
				);
			}
		}

		texture.generateMipmaps = KTX_FALSE;

		const VkFormat sourceVkFormat =
			ktxTexture2_GetVkFormat(&texture);

		if (sourceVkFormat == VK_FORMAT_UNDEFINED)
		{
			throw std::runtime_error(
				"KTX texture has undefined VkFormat after transcoding."
			);
		}

		VkFormat imageVkFormat = sourceVkFormat;

		if (useSRGB)
		{
			imageVkFormat = toSRGBVkFormat(sourceVkFormat);

			if (imageVkFormat == VK_FORMAT_UNDEFINED)
			{
				throw std::runtime_error(
					"KTX texture requires sRGB sampling, "
					"but its Vulkan format has no supported sRGB variant."
				);
			}
		}

		const smvk::Format format = fromVkFormat(imageVkFormat);

		if (format == smvk::Format::Undefined)
		{
			throw std::runtime_error(
				"KTX texture uses a format unsupported by smvk::Format."
			);
		}

		const uint32_t fullMips =
			std::max(1u, texture.numLevels);

		if (firstMip >= fullMips)
		{
			throw std::runtime_error(
				"Streaming firstMip is past the end of the KTX mip chain."
			);
		}

		uint32_t residentMips = mipCount == 0
			? 1u
			: std::min(mipCount, fullMips - firstMip);

		if (residentMips == 0)
		{
			throw std::runtime_error(
				"Streaming mip subrange is empty."
			);
		}

		const uint32_t baseWidth = std::max(1u, texture.baseWidth);
		const uint32_t baseHeight = plan.is1D
			? 1u
			: std::max(1u, texture.baseHeight);
		const uint32_t baseDepth = plan.is3D
			? std::max(1u, texture.baseDepth)
			: 1u;

		plan.vkFormat = imageVkFormat;
		plan.format = format;
		plan.width = std::max(1u, baseWidth >> firstMip);
		plan.height = plan.is1D ? 1u : std::max(1u, baseHeight >> firstMip);
		plan.depth = plan.is3D ? std::max(1u, baseDepth >> firstMip) : 1u;
		plan.firstMip = firstMip;
		plan.residentMips = residentMips;
		plan.fullMips = fullMips;

		plan.ktxLayers = std::max(1u, texture.numLayers);
		plan.faces = std::max(1u, texture.numFaces);
		plan.arrayLayers = plan.is3D ? 1u : plan.ktxLayers * plan.faces;

		auto* base = reinterpret_cast<ktxTexture*>(&texture);

		plan.dataSize = ktxTexture_GetDataSizeUncompressed(base);
		plan.data = ktxTexture_GetData(base);

		if (!plan.data || plan.dataSize == 0)
		{
			throw std::runtime_error(
				"KTX texture contains no image data."
			);
		}

		plan.regions.reserve(
			(size_t)residentMips *
			plan.ktxLayers *
			plan.faces
		);

		for (uint32_t r = 0; r < residentMips; ++r)
		{
			const uint32_t mip = firstMip + r;

			const uint32_t mipWidth = std::max(1u, baseWidth >> mip);
			const uint32_t mipHeight = plan.is1D
				? 1u
				: std::max(1u, baseHeight >> mip);
			const uint32_t mipDepth = plan.is3D
				? std::max(1u, baseDepth >> mip)
				: 1u;

			for (uint32_t layer = 0; layer < plan.ktxLayers; ++layer)
			{
				for (uint32_t face = 0; face < plan.faces; ++face)
				{
					ktx_size_t offset = 0;

					const KTX_error_code result =
						ktxTexture2_GetImageOffset(
							&texture,
							mip,
							layer,
							face,
							&offset
						);

					if (result != KTX_SUCCESS)
					{
						throw std::runtime_error(
							"ktxTexture2_GetImageOffset failed."
						);
					}

					VkBufferImageCopy region{};
					region.bufferOffset = static_cast<VkDeviceSize>(offset);
					region.bufferRowLength = 0;
					region.bufferImageHeight = 0;
					region.imageSubresource.aspectMask =
						VK_IMAGE_ASPECT_COLOR_BIT;
					// Remap: image level 0 == full-chain mip firstMip.
					region.imageSubresource.mipLevel = r;
					region.imageSubresource.baseArrayLayer =
						plan.is3D ? 0 : layer * plan.faces + face;
					region.imageSubresource.layerCount = 1;
					region.imageOffset = { 0, 0, 0 };
					region.imageExtent = { mipWidth, mipHeight, mipDepth };

					plan.regions.push_back(region);
				}
			}
		}

		return plan;
	}

	// VkImageCreateInfo shared by the streaming create + retarget paths.
	// Matches the fixed-function choices of CreateTexture(ktx...).
	VkImageCreateInfo KtxStreamVkImageCI(const KtxStreamPlan& plan, bool hostTransfer)
	{
		VkImageCreateInfo imageCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };

		imageCI.flags = plan.isCubemap
			? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
			: 0;

		// Note: cubemaps are 2D images (cube-compatible), like the
		// non-streamed CreateTexture(ktx...) path above.
		imageCI.imageType = plan.is1D
			? VK_IMAGE_TYPE_1D
			: (plan.is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D);

		imageCI.format = plan.vkFormat;
		imageCI.extent = { plan.width, plan.height, plan.depth };
		imageCI.mipLevels = plan.residentMips;
		imageCI.arrayLayers = plan.arrayLayers;
		imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageCI.usage = toVkImageUsageFlags(plan.usage);
		if (hostTransfer)
		{
			imageCI.usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
		}
		imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		return imageCI;
	}

	smvk::ImageType KtxStreamRhiType(const KtxStreamPlan& plan)
	{
		if (plan.is1D) return smvk::ImageType::Image1D;
		if (plan.is3D) return smvk::ImageType::Image3D;
		return smvk::ImageType::Image2D;
	}


} // namespace

[[nodiscard]] smvk::Holder<smvk::TextureHandle> smvk::VulkanResourceManager::CreateStreamingTexture(ktxTexture2& texture, uint32_t firstMip, uint32_t mipCount, const char* debugName, ImageCreateInfo imageCreateInfo)
{
	KtxStreamPlan plan = PrepareKtxStreamPlan(texture, firstMip, mipCount);

	const bool hostTransfer = imageCreateInfo.hostTransfer;
	if (hostTransfer)
	{
		assert(m_hostImageCopySupported);
		assert(IsHostTransferSupported(plan.vkFormat));
	}

	VkImageCreateInfo imageCI = KtxStreamVkImageCI(plan, hostTransfer);

	VmaAllocationCreateInfo allocCI{};
	allocCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	allocCI.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	VkImage vkImage = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;

	// Allocation ledger: capture the exact committed bytes (the out-info
	// vmaCreateImage already writes).
	VmaAllocationInfo ledgerInfo{};
	VK_CHECK(vmaCreateImage(
		m_allocator, &imageCI, &allocCI,
		&vkImage, &allocation, &ledgerInfo));

	Image image{};
	image.vkImage = vkImage;
	image.allocation = allocation;
	image.allocBytes = ledgerInfo.size;
	image.type = KtxStreamRhiType(plan);
	image.extent = { plan.width, plan.height, plan.depth };
	image.format = plan.format;
	image.usage = plan.usage;
	image.samples = SampleCount::Count1;
	image.mipLevels = plan.residentMips;
	image.arrayLayers = imageCI.arrayLayers;
	image.liveViewCount = 0;
	image.ownership = ResourceOwnership::Owned;
	image.isCubemap = plan.isCubemap;
	// Arena-owned copy (same contract as CreateImageImpl:
	// CopyResourceName - cross-thread mutex-guarded arena copy).
	image.name = CopyResourceName(debugName);

	const ImageHandle imageHandle =
		images_.create(std::move(image));

	Holder<ImageHandle> imageHolder{ this, imageHandle };

#if defined(_DEBUG)
	if (debugName)
	{
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};
		nameInfo.objectType = VK_OBJECT_TYPE_IMAGE;
		nameInfo.objectHandle = reinterpret_cast<uint64_t>(vkImage);
		nameInfo.pObjectName = debugName;
		SetDebugObjectName(device_, &nameInfo);
	}
#else
	(void)debugName;
#endif

	ImageViewCreateInfo viewCI{};

	if (plan.isCubemap)
	{
		viewCI.viewType = plan.ktxLayers > 1
			? ImageViewType::ViewCubeArray
			: ImageViewType::ViewCube;
	}
	else if (plan.is1D)
	{
		viewCI.viewType = plan.ktxLayers > 1
			? ImageViewType::View1DArray
			: ImageViewType::View1D;
	}
	else if (plan.is3D)
	{
		viewCI.viewType = ImageViewType::View3D;
	}
	else if (plan.ktxLayers > 1)
	{
		viewCI.viewType = ImageViewType::View2DArray;
	}
	else
	{
		viewCI.viewType = ImageViewType::View2D;
	}

	viewCI.format = Format::Undefined;
	viewCI.subresourceRange = {
		.aspect = ImageAspect::Color,
		.baseMipLevel = 0,
		.mipLevelCount = plan.residentMips,
		.baseArrayLayer = 0,
		.arrayLayerCount = imageCI.arrayLayers
	};
	viewCI.name = debugName;

	auto viewHolder = CreateImageView(imageHandle, viewCI);
	auto bindlessHolder = CreateBindlessImage(
		viewHolder, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	{
		m_streamer->uploadImageKtx(
			imageHandle, plan.data, plan.dataSize, plan.regions,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_READ_BIT);
	}

	Texture textureResource{
		.image = imageHolder.release(),
		.view = viewHolder.release(),
		.bindless = bindlessHolder.release()
	};

	return Holder<TextureHandle>{
		this,
			textures_.create(std::move(textureResource))
	};
}

void smvk::VulkanResourceManager::RetargetStreamingTexture(TextureHandle handle, ktxTexture2& texture, uint32_t firstMip, uint32_t mipCount, const char* debugName)
{
	// Retarget is a RENDER-THREAD hot path (streaming demand upgrades +
	// budget downgrades, per frame). A Basis-family payload here pays a
	// full synchronous transcode (tens to ~150 ms for 2-4K textures) —
	// the exact hitch this pipeline is built to never have, so the v4
	// cook contract guarantees GPU-ready BCn payloads in SHIPPED paks
	// (see CookTexturePayload: per-semantic BCn pre-transcode at cook,
	// pink error texture on failure). The TextureStreamer ENFORCES the
	// policy (see its config): cooked runs refuse Basis payloads up
	// front (pink error texture + pinned entry), authoring hosts (the
	// editor — clusters stash SOURCE bytes by design) allow the
	// in-place transcode below and get the real content. Reaching here
	// with Basis means either the authoring path (transcoded by
	// PrepareKtxStreamPlan, like the first load) or a direct caller
	// with a stale/foreign payload (same transcode; recook the pak).
	Texture* rec = textures_.get(handle);
	if (!rec)
	{
		throw std::runtime_error(
			"RetargetStreamingTexture: invalid texture handle."
		);
	}

	Image* img = images_.get(rec->image);
	ImageView* view = imageViews_.get(rec->view);
	if (!img || !view)
	{
		throw std::runtime_error(
			"RetargetStreamingTexture: texture references are stale."
		);
	}

	KtxStreamPlan plan = PrepareKtxStreamPlan(texture, firstMip, mipCount);

	// Streaming textures always come through the KTX path with host
	// transfer enabled (see texture_loader.cpp); keep the usage bit when
	// the device/format supports it so the fast host-copy upload stays
	// available after the retarget.
	const bool hostTransfer =
		m_hostImageCopySupported &&
		IsHostTransferSupported(plan.vkFormat);

	VkImageCreateInfo imageCI = KtxStreamVkImageCI(plan, hostTransfer);

	VmaAllocationCreateInfo allocCI{};
	allocCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	allocCI.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	VkImage newImage = VK_NULL_HANDLE;
	VmaAllocation newAlloc = VK_NULL_HANDLE;

	const bool texTrace = std::getenv("TOURMALINE_TEXSTREAM_TRACE") != nullptr;
	const auto texT0 = std::chrono::steady_clock::now();
	// Allocation ledger: the retarget swaps the backing - capture the NEW
	// allocation's exact bytes for the entry update below.
	VmaAllocationInfo ledgerInfo{};
	VK_CHECK(vmaCreateImage(
		m_allocator, &imageCI, &allocCI,
		&newImage, &newAlloc, &ledgerInfo));
	const auto texT1 = std::chrono::steady_clock::now();

	// Retire the old backing lazily: in-flight frames may still sample
	// it, so destruction waits for the latest graphics submit to retire.
	// Must be called from the render thread between frames (same contract
	// as DeferDestroy).
	{
		VkImage oldImage = img->vkImage;
		VmaAllocation oldAlloc = img->allocation;
		VmaAllocator allocator = m_allocator;
		DeferDestroy(std::packaged_task<void()>([
			allocator, oldImage, oldAlloc]
		{
			if (oldImage != VK_NULL_HANDLE)
				vmaDestroyImage(allocator, oldImage, oldAlloc);
		}));
	}

	img->vkImage = newImage;
	img->allocation = newAlloc;
	// Ledger: the entry's exact bytes follow the swap (the old backing's
	// bytes retire with its deferred destroy - the entry only ever
	// reports its CURRENT allocation).
	img->allocBytes = ledgerInfo.size;
	img->type = KtxStreamRhiType(plan);
	img->extent = { plan.width, plan.height, plan.depth };
	img->format = plan.format;
	img->usage = plan.usage;
	img->samples = SampleCount::Count1;
	img->mipLevels = plan.residentMips;
	img->arrayLayers = imageCI.arrayLayers;
	img->isCubemap = plan.isCubemap;
	SetImageSyncState(rec->image, ImageGlobalSyncState{});

#if defined(_DEBUG)
	if (debugName)
	{
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};
		nameInfo.objectType = VK_OBJECT_TYPE_IMAGE;
		nameInfo.objectHandle = reinterpret_cast<uint64_t>(newImage);
		nameInfo.pObjectName = debugName;
		SetDebugObjectName(device_, &nameInfo);
	}
#else
	(void)debugName;
#endif

	// Rebuild the sampled view in place (same handle, same bindless slot):
	// keep the original view type (cube/array/3D), span the new range. The
	// previous VkImageView retires through the deferred queue – in-flight
	// frames may still reference it through their descriptors (which pick
	// up the new view on the next UpdateDescriptors, one frame later; that
	// stale frame keeps sampling the OLD, still-valid image).
	{
		VkImageView oldView = view->vkView;

		VkImageViewCreateInfo vkCI{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vkCI.image = newImage;
		vkCI.viewType = toVkImageViewType(view->viewType);
		vkCI.format = toVkFormat(img->format);
		vkCI.components = {
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY
		};
		vkCI.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = plan.residentMips,
			.baseArrayLayer = 0,
			.layerCount = imageCI.arrayLayers
		};

		VkImageView newView = VK_NULL_HANDLE;
		VK_CHECK(vkCreateImageView(device_, &vkCI, nullptr, &newView));

		view->vkView = newView;
		view->format = img->format;
		view->subresourceRange = {
			.aspect = ImageAspect::Color,
			.baseMipLevel = 0,
			.mipLevelCount = plan.residentMips,
			.baseArrayLayer = 0,
			.arrayLayerCount = imageCI.arrayLayers
		};
		// liveViewCount unchanged: the same image handle keeps one live view.

#if defined(_DEBUG)
		if (debugName)
		{
			VkDebugUtilsObjectNameInfoEXT viewNameInfo{
				VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
			};
			viewNameInfo.objectType = VK_OBJECT_TYPE_IMAGE_VIEW;
			viewNameInfo.objectHandle = reinterpret_cast<uint64_t>(newView);
			viewNameInfo.pObjectName = debugName;
			SetDebugObjectName(device_, &viewNameInfo);
		}
#endif

		VkDevice dev = device_;
		DeferDestroy(std::packaged_task<void()>([dev, oldView]
		{
			if (oldView != VK_NULL_HANDLE)
				vkDestroyImageView(dev, oldView, nullptr);
		}));
	}
	const auto texT2 = std::chrono::steady_clock::now();

	m_streamer->uploadImageKtx(
		rec->image, plan.data, plan.dataSize, plan.regions,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		VK_ACCESS_2_SHADER_READ_BIT);
	const auto texT3 = std::chrono::steady_clock::now();
	if (texTrace) {
		using msd = std::chrono::duration<double, std::milli>;
		std::fprintf(stderr,
			"[TexStream]   retargetPlan %ux%u mips %u: createImage %.1f view %.1f upload %.1f ms\n",
			plan.width, plan.height, plan.residentMips,
			msd(texT1 - texT0).count(), msd(texT2 - texT1).count(),
			msd(texT3 - texT2).count());
		std::fflush(stderr);
	}

	// Bindless slot content changed (new VkImageView): refresh the
	// descriptor arrays on the next UpdateDescriptors() before use.
	m_needToUpdateTextures = true;
}
[[nodiscard]] smvk::Holder<smvk::ImageViewHandle> smvk::VulkanResourceManager::CreateImageView(ImageHandle imageHandle, const ImageViewCreateInfo& ci)
{
	Image* image = images_.get(imageHandle);
	assert(image);

	const Format resolvedFormat =
		ci.format == Format::Undefined
		? image->format
		: ci.format;

	const ImageSubresourceRange resolvedRange =
		ResolveImageSubresourceRange(
			*image,
			ci.subresourceRange
		);

	VkImageViewCreateInfo vkCI{
		VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
	};

	vkCI.image = image->vkImage;
	vkCI.viewType = toVkImageViewType(ci.viewType);
	vkCI.format = toVkFormat(resolvedFormat);

	vkCI.components = {
		VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY
	};

	vkCI.subresourceRange = {
		.aspectMask =
			toVkImageAspectFlags(resolvedRange.aspect),

		.baseMipLevel =
			resolvedRange.baseMipLevel,

		.levelCount =
			resolvedRange.mipLevelCount,

		.baseArrayLayer =
			resolvedRange.baseArrayLayer,

		.layerCount =
			resolvedRange.arrayLayerCount
	};

	ImageView view{};

	VK_CHECK(vkCreateImageView(
		device_,
		&vkCI,
		nullptr,
		&view.vkView
	));

	view.image = imageHandle;
	view.viewType = ci.viewType;
	view.format = resolvedFormat;
	view.subresourceRange = resolvedRange;

#if defined(_DEBUG)
	if (ci.name)
	{
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};

		nameInfo.objectType = VK_OBJECT_TYPE_IMAGE_VIEW;
		nameInfo.objectHandle =
			reinterpret_cast<uint64_t>(view.vkView);
		nameInfo.pObjectName = ci.name;

		SetDebugObjectName(
			device_,
			&nameInfo
		);
	}
#endif

	++image->liveViewCount;


	return Holder<ImageViewHandle>{
		this,
			imageViews_.create(std::move(view))
	};
}
[[nodiscard]] smvk::ImageViewHandle smvk::VulkanResourceManager::RegisterImageView(VkImageView vkView, Format format, const char* name)
{
	assert(vkView != VK_NULL_HANDLE);

	ImageView view{};
	view.vkView = vkView;
	view.format = format;
	view.ownership = ResourceOwnership::External;

	return imageViews_.create(std::move(view));
}
void smvk::VulkanResourceManager::SetImageViewVkView(ImageViewHandle handle, VkImageView vkView)
{
	auto* view = imageViews_.get(handle);
	assert(view != nullptr);
	view->vkView = vkView;
}
smvk::ImageViewHandle smvk::VulkanResourceManager::RecreateImageView(ImageViewHandle handle, ImageHandle imageHandle, const ImageViewCreateInfo& ci)
{
	ImageView* view = imageViews_.get(handle);
	assert(view != nullptr);

	const Format resolvedFormat =
		ci.format == Format::Undefined
		? images_.get(imageHandle)->format
		: ci.format;

	const ImageSubresourceRange resolvedRange =
		ResolveImageSubresourceRange(
			*images_.get(imageHandle),
			ci.subresourceRange
		);

	// Release the previous view and its parent reference.
	if (view->image.valid())
	{
		auto* oldImage = images_.get(view->image);
		assert(oldImage != nullptr);
		assert(oldImage->liveViewCount > 0);
		--oldImage->liveViewCount;
	}

	if (view->vkView != VK_NULL_HANDLE)
	{
		vkDestroyImageView(device_, view->vkView, nullptr);
		view->vkView = VK_NULL_HANDLE;
	}

	VkImageViewCreateInfo vkCI{
		VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
	};

	vkCI.image = images_.get(imageHandle)->vkImage;
	vkCI.viewType = toVkImageViewType(ci.viewType);
	vkCI.format = toVkFormat(resolvedFormat);

	vkCI.components = {
		VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY
	};

	vkCI.subresourceRange = {
		.aspectMask = toVkImageAspectFlags(resolvedRange.aspect),
		.baseMipLevel = resolvedRange.baseMipLevel,
		.levelCount = resolvedRange.mipLevelCount,
		.baseArrayLayer = resolvedRange.baseArrayLayer,
		.layerCount = resolvedRange.arrayLayerCount
	};

	VK_CHECK(vkCreateImageView(
		device_,
		&vkCI,
		nullptr,
		&view->vkView
	));

	view->image = imageHandle;
	view->viewType = ci.viewType;
	view->format = resolvedFormat;
	view->subresourceRange = resolvedRange;

	++images_.get(imageHandle)->liveViewCount;

	return handle;
}
smvk::ImageHandle smvk::VulkanResourceManager::RecreateImage(ImageHandle handle, const ImageCreateInfo& ci, const void* data)
{
	Image* image = images_.get(handle);
	assert(image != nullptr);

	if (image->ownership == ResourceOwnership::Owned &&
		image->vkImage != VK_NULL_HANDLE)
	{
		vmaDestroyImage(
			m_allocator,
			image->vkImage,
			image->allocation
		);
		image->vkImage = VK_NULL_HANDLE;
		image->allocation = VK_NULL_HANDLE;
	}

	// Build a fresh image but preserve scalar state (liveViewCount, ownership,
	// type) so views referencing this handle keep their accounting intact.
	Image fresh = CreateImageImpl(ci);

	image->vkImage = fresh.vkImage;
	image->allocation = fresh.allocation;
	image->type = fresh.type;
	image->extent = fresh.extent;
	image->format = fresh.format;
	image->usage = fresh.usage;
	image->samples = fresh.samples;
	image->mipLevels = fresh.mipLevels;
	image->arrayLayers = fresh.arrayLayers;
	image->isCubemap = fresh.isCubemap;
	// Name: prefer the fresh ci's name, but the recreate callers often pass
	// a bare ImageCreateInfo (no name) - keep the OLD name then (the
	// arena view stays valid; streaming recreates keep their identity).
	if (fresh.name.valid())
		image->name = fresh.name;
	SetImageSyncState(handle, ImageGlobalSyncState{});

	return handle;
}

[[nodiscard]] smvk::Holder<smvk::BufferHandle> smvk::VulkanResourceManager::CreateBuffer(VkDeviceSize size, BufferType bufferType, const char* debugName, VmaAllocationCreateFlags vmaAllocFlags) {
	Buffer desc{};
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size;
	ci.usage = bufferTypeToUsageFlags[static_cast<size_t>(bufferType)];
	// For frequently updated resources (vertex/index) use CONCURRENT when dedicated transfer exists to avoid ownership ping-pong
	bool concurrentSharing = m_transferSubmitter && m_transferSubmitter != m_submitter &&
		(bufferType == BufferType_Vertex || bufferType == BufferType_Index || bufferType == BufferType_Storage);
	if (concurrentSharing) {
		ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
		// Stack storage: vmaCreateBuffer copies the indices during the
		// call, and concurrent buffer creation from worker threads must
		// not share a static array.
		uint32_t families[2] = {};
		families[0] = m_submitter->queueFamilyIndex();
		families[1] = m_transferSubmitter->queueFamilyIndex();
		ci.queueFamilyIndexCount = 2;
		ci.pQueueFamilyIndices = families;
	}
	else {
		ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	VmaAllocationCreateInfo aci{};
	aci.usage = bufferTypeToVmaUsage[static_cast<size_t>(bufferType)];
	if (bufferType == BufferType_Staging) {
		// CPU-read staging (readback) opts into HOST_ACCESS_RANDOM so VMA
		// picks cached memory; CPU-write staging (uploads) keeps
		// SEQUENTIAL_WRITE. Passing both to VMA is invalid, so it's either/or.
		if (vmaAllocFlags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)
			aci.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		else
			aci.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		// Optional/redundant if you want to strictly require it.
		aci.requiredFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	}
	if (bufferType == BufferType_Uniform) {
		aci.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		aci.requiredFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	}

	// Optionally choose pools here based on usage_flags/usage
	// Allocation ledger: capture the EXACT committed bytes (the out-info
	// vmaCreateBuffer already writes) + the creation type + the
	// arena-owned name (buffers now name-track like images - see
	// GetMemoryAllocInfos).
	VmaAllocationInfo ledgerInfo{};
	VK_CHECK(vmaCreateBuffer(
		m_allocator,
		&ci,
		&aci,
		&desc.vkBuffer,
		&desc.vma_alloc,
		&ledgerInfo
	)
	);
	desc.allocBytes = ledgerInfo.size;
	desc.type = bufferType;
	desc.name = CopyResourceName(debugName);

	const VkBufferDeviceAddressInfo ai = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = desc.vkBuffer
	};
	desc.deviceAddress = (bufferType == BufferType_Uniform || bufferType == BufferType_Storage ||
		bufferType == BufferType_Indirect || bufferType == BufferType_Vertex ||
		bufferType == BufferType_Index)
		? vkGetBufferDeviceAddress(device_, &ai) : 0;
	vmaGetAllocationMemoryProperties(m_allocator, desc.vma_alloc, &desc.memoryFlags);
	desc.usage = bufferTypeToUsageFlags[static_cast<size_t>(bufferType)];
	desc.isConcurrent = concurrentSharing;
#if defined(_DEBUG)
	if (debugName) {
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};
		nameInfo.objectType = VK_OBJECT_TYPE_BUFFER;
		nameInfo.objectHandle = reinterpret_cast<uint64_t>(desc.vkBuffer);
		nameInfo.pObjectName = debugName;
		SetDebugObjectName(device_, &nameInfo);
	}
#endif
	auto handle = buffers_.create(std::move(desc));
	// Init global sync state: Undefined / None until first use.
	SetBufferSyncState(handle, BufferGlobalSyncState{});
	return { this, handle };
}
[[nodiscard]] smvk::Holder<smvk::ShaderModuleHandle> smvk::VulkanResourceManager::CreateShaderModule(std::vector<char> code, const char* debugName) {
	VkShaderModule shader = VK_NULL_HANDLE;
	CreateShaderModuleIn(
		code,
		shader
	);
#if defined(_DEBUG)
	if (debugName) {
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};
		nameInfo.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
		nameInfo.objectHandle = reinterpret_cast<uint64_t>(shader);
		nameInfo.pObjectName = debugName;
		SetDebugObjectName(device_, &nameInfo);
	}
#else
	(void)debugName;
#endif
	ShaderModule entry{};
	entry.module = shader;
	entry.code = std::move(code);
	return { this, shaderModules_.create(std::move(entry)) };
}

std::span<const char> smvk::VulkanResourceManager::GetShaderModuleCode(ShaderModuleHandle handle) const {
	auto* sm = shaderModules_.get(handle);
	if (!sm)
		return {};
	return std::span<const char>(sm->code.data(), sm->code.size());
}

smvk::SpirvReflection smvk::VulkanResourceManager::ReflectShaderModule(ShaderModuleHandle handle) const {
	auto* sm = shaderModules_.get(handle);
	if (!sm || sm->code.empty()) {
		SpirvReflection out;
		out.error = "unknown shader module";
		return out;
	}
	return ReflectSpirvBytes(std::span<const char>(sm->code.data(), sm->code.size()));
}
[[nodiscard]] smvk::Holder<smvk::DescriptorPoolHandle> smvk::VulkanResourceManager::CreateDescriptorPool(std::vector<VkDescriptorPoolSize> poolSizes, const char* debugName)
{
	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1000;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT; // required
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();

	VkDescriptorPool dpool;
	VK_CHECK(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &dpool));
#if defined(_DEBUG)
	if (debugName) {
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};
		nameInfo.objectType = VK_OBJECT_TYPE_DESCRIPTOR_POOL;
		nameInfo.objectHandle = reinterpret_cast<uint64_t>(dpool);
		nameInfo.pObjectName = debugName;
		SetDebugObjectName(device_, &nameInfo);
	}
#else
	(void)debugName;
#endif
	return { this, descriptorPools_.create(std::move(dpool)) };
}
smvk::StringViewT smvk::VulkanResourceManager::CopyResourceName(const char* name) {
	// See the member block: image/texture/buffer creation runs on the
	// render thread AND worker threads (pak preload, streaming) - the
	// arena is not synchronized, so the copy is mutex-guarded. The
	// empty-view return for a null name IS the unnamed contract
	// (displays read "(unnamed)").
	if (!name)
		return StringViewT{};
	std::lock_guard<std::mutex> lock(nameArenaMutex_);
	return nameArena_.AllocateString(StringViewT(name));
}

// ---------------------------------------------------------------------------
// Allocation ledger (see the smvk.h block): immediate-mode pool walks.
// Exact bytes live ON the entries (captured at create); destroy needs no
// hooks. Render thread only (the ListTextureInfos forEach contract).
// ---------------------------------------------------------------------------
namespace smvk {
	static MemoryCategory ImageLedgerCategory(const ImageUsage& usage) {
		// Attachment-usage images are TARGETS (rendergraph color/depth, the
		// thin G-buffer Storage images); everything else (sampled content,
		// transfer-only) is CONTENT. TransferSrc/Dst never implies a target
		// (content images keep their upload flags after staging).
		const bool target = (usage & (ImageUsage::ColorAttachment
			| ImageUsage::DepthStencilAttachment
			| ImageUsage::Storage
			| ImageUsage::InputAttachment)) != ImageUsage::None;
		return target ? MemoryCategory::ImageTarget : MemoryCategory::ImageContent;
	}

	std::array<uint64_t, kMemoryCategoryCount>
		VulkanResourceManager::GetMemoryLedger() {
		std::array<uint64_t, kMemoryCategoryCount> out{};
		out.fill(0);
		images_.forEachHandle([&](ImageHandle handle) {
			const Image* image = images_.get(handle);
			if (!image || image->allocBytes == 0)
				return; // untracked (heap-placed/external) - the remainder row
			out[size_t(ImageLedgerCategory(image->usage))]
				+= image->allocBytes;
		});
		buffers_.forEachHandle([&](BufferHandle handle) {
			const Buffer* buffer = buffers_.get(handle);
			if (!buffer || buffer->allocBytes == 0)
				return;
			// BufferType maps 1:1 onto the ledger categories (the enum orders
			// match: Index, Vertex, Uniform, Storage, Indirect, Transfer*,
			// Staging -> Staging).
			size_t cat = size_t(MemoryCategory::Staging);
			switch (buffer->type) {
			case BufferType_Index: cat = size_t(MemoryCategory::Index); break;
			case BufferType_Vertex: cat = size_t(MemoryCategory::Vertex); break;
			case BufferType_Uniform: cat = size_t(MemoryCategory::Uniform); break;
			case BufferType_Storage: cat = size_t(MemoryCategory::Storage); break;
			case BufferType_Indirect: cat = size_t(MemoryCategory::Indirect); break;
			default: break; // TransferSrc/Dst/Staging
			}
			out[cat] += buffer->allocBytes;
		});
		return out;
	}

	std::vector<MemoryAllocInfo> VulkanResourceManager::GetMemoryAllocInfos() {
		std::vector<MemoryAllocInfo> out;
		// ALL images, tracked or not: bytes == 0 marks an untracked backing
		// (heap-placed design-B target / external) — the display computes an
		// estimate from the geometry. The LEDGER sums (GetMemoryLedger)
		// count tracked bytes only; this list is the complete MAP.
		images_.forEachHandle([&](ImageHandle handle) {
			const Image* image = images_.get(handle);
			if (!image)
				return;
			MemoryAllocInfo info{};
			info.name = image->name;
			info.category = ImageLedgerCategory(image->usage);
			info.bytes = image->allocBytes;
			info.isImage = true;
			info.extent = image->extent;
			info.format = image->format;
			info.samples = image->samples;
			info.mipLevels = image->mipLevels;
			info.arrayLayers = image->arrayLayers;
			out.push_back(info);
		});
		buffers_.forEachHandle([&](BufferHandle handle) {
			const Buffer* buffer = buffers_.get(handle);
			if (!buffer || buffer->allocBytes == 0)
				return;
			MemoryAllocInfo info{};
			info.name = buffer->name;
			info.bytes = buffer->allocBytes;
			info.isImage = false;
			// Category: same mapping as GetMemoryLedger.
			switch (buffer->type) {
			case BufferType_Index:
				info.category = MemoryCategory::Index; break;
			case BufferType_Vertex:
				info.category = MemoryCategory::Vertex; break;
			case BufferType_Uniform:
				info.category = MemoryCategory::Uniform; break;
			case BufferType_Storage:
				info.category = MemoryCategory::Storage; break;
			case BufferType_Indirect:
				info.category = MemoryCategory::Indirect; break;
			default:
				info.category = MemoryCategory::Staging; break;
			}
			out.push_back(info);
		});
		return out;
	}
} // namespace smvk

[[nodiscard]] void* smvk::VulkanResourceManager::MapBuffer(BufferHandle  handle) {
	// Hardened: in Release the asserts below compile out, and an empty /
	// retired handle would reach vmaMapMemory with a null allocation
	// (nullptr deref). Return null instead; callers treat it as failure.
	if (handle.empty())
		return nullptr;
	auto buf = this->buffers_.get(handle);
	if (!buf || buf->vma_alloc == VK_NULL_HANDLE)
		return nullptr;
	void* mapped = nullptr;
	if (vmaMapMemory(m_allocator, buf->vma_alloc, &mapped) != VK_SUCCESS)
		return nullptr;
	return mapped;
}
void smvk::VulkanResourceManager::UnmapBuffer(BufferHandle handle) {
	if (handle.empty())
		return;
	auto buf = this->buffers_.get(handle);
	if (!buf || buf->vma_alloc == VK_NULL_HANDLE)
		return;
	// Not required for HOST_COHERENT memory, but harmless if you want to support
	// non-coherent staging buffers later.
	// vmaFlushAllocation(m_allocator, staging->vma_alloc, 0, stagingBufferSize);
	vmaUnmapMemory(m_allocator, buf->vma_alloc);
}
void smvk::VulkanResourceManager::InvalidateStagingRange(BufferHandle handle,
	VkDeviceSize offset, VkDeviceSize size) {
	// See the header contract: the readback staging is host-visible but
	// not guaranteed host-COHERENT (VMA may pick a non-coherent cached
	// type); without this the CPU can read STALE bytes after the GPU copy
	// completed (the spec requires the explicit invalidate). Coherent
	// memory makes this a no-op that the spec still allows.
	auto buf = this->buffers_.get(handle);
	if (!buf || buf->vma_alloc == VK_NULL_HANDLE)
		return;
	VkResult r = vmaInvalidateAllocation(m_allocator, buf->vma_alloc, offset, size);
	if (r != VK_SUCCESS)
		fprintf(stderr, "[Vulkan Warning] vmaInvalidateAllocation (%llu..+%llu) failed: %s\n",
			(unsigned long long)offset, (unsigned long long)size, vkResultToString(r));
}
std::vector<smvk::GpuMemoryHeap> smvk::VulkanResourceManager::GetGpuMemoryInfo() const {
	std::vector<smvk::GpuMemoryHeap> out;
	if (m_allocator == VK_NULL_HANDLE || physicalDevice_ == VK_NULL_HANDLE)
		return out;
	VkPhysicalDeviceMemoryProperties memProps{};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
	if (memProps.memoryHeapCount == 0)
		return out;
	std::vector<VmaBudget> budgets(memProps.memoryHeapCount);
	vmaGetHeapBudgets(m_allocator, budgets.data());
	out.reserve(memProps.memoryHeapCount);
	for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
		smvk::GpuMemoryHeap h{};
		h.heapSizeBytes = memProps.memoryHeaps[i].size;
		h.budgetBytes = budgets[i].budget;
		h.usageBytes = budgets[i].usage;
		h.allocationBytes = budgets[i].statistics.allocationBytes;
		h.blockBytes = budgets[i].statistics.blockBytes;
		h.deviceLocal = (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
		out.push_back(h);
	}
	return out;
}
bool smvk::DeferredTask::isReady() const {
	if (retirePoint_.semaphore != VK_NULL_HANDLE && retirePoint_.value != 0) {
		uint64_t v = 0;
		VkResult r = vkGetSemaphoreCounterValue(device_, retirePoint_.semaphore, &v);
		if (r == VK_ERROR_DEVICE_LOST) return true;
		if (r != VK_SUCCESS) return false;
		return v >= retirePoint_.value;
	}
	if (!handle_.empty() && submitter_ != nullptr) {
		return submitter_->isReady(handle_);
	}
	if (!handle_.empty()) {
		// Legacy handle without submitter – check via global VulkanResourceManager submitters if possible
		// For now, treat as ready to avoid leak, but proper code should use TimelinePoint
		return true;
	}
	return true; // empty handle/point means immediate
}
void smvk::VulkanResourceManager::AddDeferredTask(std::packaged_task<void()>&& task, smvk::CommandSubmitHandle handle)
{
	std::lock_guard<std::mutex> lock(m_deferredMutex);
	if (handle.empty()) {
		m_deferredTasks.emplace_back(std::move(task), TimelinePoint{});
		return;
	}
	m_deferredTasks.emplace_back(std::move(task), handle, nullptr);
}
void smvk::VulkanResourceManager::AddDeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle, Submitter* submitter)
{
	std::lock_guard<std::mutex> lock(m_deferredMutex);
	if (handle.empty()) {
		m_deferredTasks.emplace_back(std::move(task), TimelinePoint{});
		return;
	}
	m_deferredTasks.emplace_back(std::move(task), handle, submitter);
}
void smvk::VulkanResourceManager::AddDeferredTask(std::packaged_task<void()>&& task, TimelinePoint point)
{
	std::lock_guard<std::mutex> lock(m_deferredMutex);
	m_deferredTasks.emplace_back(std::move(task), point);
}
void smvk::VulkanResourceManager::ProcessDeferrredTasks()
{
	std::vector<DeferredTask> ready;
	{
		std::lock_guard<std::mutex> lock(m_deferredMutex);
		// Drain EVERY ready task, not just the ready PREFIX: tasks key on
		// different timelines (the graphics point, per-submitter command
		// handles), so an unready EARLIER task must not hold back ready
		// LATER ones (the old prefix drain retained memory behind a
		// stalled head for no reason). Relative order is PRESERVED among
		// the drained tasks: a same-timeline pair queued as destroy-then-
		// free (the FIFO pairing the range frees rely on — see
		// ReleaseTargetHeapClaim / ReleaseTransientPlacement / the mesh
		// heap frees in Cluster::DestroyMesh) can never invert, because
		// the retire values are monotonic (the free's point being ready
		// implies the earlier destroy's point was ready too) and the
		// partition below is stable.
		ready.reserve(m_deferredTasks.size());
		auto keep = m_deferredTasks.begin();
		for (auto it = m_deferredTasks.begin(); it != m_deferredTasks.end(); ++it) {
			if (it->isReady()) {
				ready.push_back(std::move(*it));
			} else {
				// keep != it guard: a self move-assign of the move-only
				// packaged_task is not guaranteed valid for self-move, and
				// the prefix of survivors before the first ready task
				// would do exactly that (keep == it there).
				if (keep != it)
					*keep = std::move(*it);
				++keep;
			}
		}
		m_deferredTasks.erase(keep, m_deferredTasks.end());
	}
	// Run ready tasks OUTSIDE the lock: tasks may enqueue more deferred
	// work (destroy -> DeferDestroy -> AddDeferredTask), which previously
	// re-entered this non-recursive mutex from inside the drain loop (UB:
	// deadlock or fast-fail depending on the CRT). Appends made during the
	// run land behind the not-ready remainder, so the FIFO-by-retire-
	// point order is preserved.
	// TOURMALINE_DEFER_DIAG=1: the drain side of the census.
	{
		static const bool deferDiag = [] {
			const char* e = std::getenv("TOURMALINE_DEFER_DIAG");
			return e && std::atoi(e) != 0;
		}();
		static uint32_t drainCalls = 0;
		static uint32_t drainN = 0;
		if (deferDiag) {
			++drainCalls;
			if (drainCalls % 30 == 1)
				std::fprintf(stderr,
					"[DEFER-DIAG] call#%u: ready=%zu pending=%zu "
					"(drainsRun=%u)\n",
					drainCalls, ready.size(), m_deferredTasks.size(),
					drainN);
		}
		if (!ready.empty()) {
			++drainN;
			if (deferDiag && drainN % 10 == 1)
				std::fprintf(stderr,
					"[DEFER-DIAG] drain #%u: ran=%zu keptPending=%zu\n",
					drainN, ready.size(), m_deferredTasks.size());
		}
	}
	for (auto& task : ready)
		task.task_();
}
void smvk::VulkanResourceManager::FlushDeferredTasks()
{
	// Unconditional: caller guarantees the device is idle (or lost/teardown),
	// so every retire point is trivially satisfied. Never queries semaphores,
	// which matters because the timeline semaphore may already be destroyed
	// (member teardown order: swapchain before context).
	for (;;) {
		std::vector<DeferredTask> batch;
		{
			std::lock_guard<std::mutex> lock(m_deferredMutex);
			if (m_deferredTasks.empty())
				break;
			batch = std::move(m_deferredTasks);
			m_deferredTasks.clear();
		}
		// Outside the lock, same reason as ProcessDeferrredTasks: tasks may
		// re-enqueue (destroy chains); the loop drains those next iteration.
		for (auto& task : batch)
			task.task_();
	}
}
void smvk::VulkanResourceManager::NotifyGraphicsSubmit(VkSemaphore semaphore, uint64_t value)
{
	m_lastGraphicsSemaphore = semaphore;
	m_lastGraphicsValue = value;
}
VkDevice smvk::VulkanResourceManager::VkDeviceHandle() const
{
	// Test support only (see the header contract): the device handle itself
	// stays file-internal; the retirement fuzzer is the sole consumer.
	return device_;
}
void smvk::VulkanResourceManager::DeferDestroy(std::packaged_task<void()>&& task)
{
	// ---------------------------------------------------------------------------
	// RETIRE MARGIN: the retire point below is the last published GRAPHICS
	// timeline point + MAX_FRAMES_IN_FLIGHT. The swapchain's timeline signals
	// exactly once per completed frame, so the margin is literally "this many
	// frames later" - and that span structurally covers every producer that
	// can still reference the destroyed resource, WITHOUT the exact per-queue
	// join or the destroy-timing API contract the old exact-point key needed:
	//
	//   (a) ASYNC COMPUTE legs issued around the published point: a C chain's
	//       C2G signal is waited by the NEXT main submit, so every C work item
	//       that could reference the resource completes strictly before the
	//       NEXT frame's timeline point - well inside the margin.
	//   (b) TRANSFER batches flushed after the published point: the swapchain
	//       waits the NEWEST transfer point before the next frame's
	//       G1/G2/main - again inside the margin. (The streamer drain below
	//       covers the already-pending batches synchronously regardless.)
	//   (c) Command buffers still RECORDING at destroy time: a recording
	//       submits at the next point and completes inside the margin, so
	//       even a MID-FRAME destroy (previously forbidden by the
	//       "destroy only between frames" API contract) stays safe.
	//
	// The margin buys this by holding every deferred resource (buffers,
	// images/views, mesh heap ranges, bindless slots) up to MAX_FRAMES_IN_FLIGHT
	// extra frames - bounded, and negligible against the 512 MiB mesh heaps.
	// The alternative (the exact join: max over per-queue last-use points plus
	// a per-submitter drain of still-recording buffers) frees sooner but
	// needs per-queue tracking at every submit site; take it only if the
	// retention ever shows up as a real cost.
	// ---------------------------------------------------------------------------
	// Unflushed streamer batches still RECORD (transfer cmd) or hold
	// pending acquire barriers referencing the resource being destroyed.
	// They have no timeline point yet, so no retire point could cover
	// them. Drain synchronously first; this only triggers when destroying
	// with pending uploads (load / failure paths), never in steady state
	// where uploads are flushed every frame.
	if (m_streamer && m_streamer->hasAnyPending()) {
		m_streamer->waitAll();
	}
	// No graphics submit ever recorded -> after the streamer drain above
	// nothing can be in flight.
	if (m_lastGraphicsSemaphore == VK_NULL_HANDLE || m_lastGraphicsValue == 0) {
		task();
		return;
	}
	// Retire values are monotonic, so the FIFO pairing (destroy-then-free
	// at the same keyed point - see ReleaseTargetHeapClaim /
	// ReleaseTransientPlacement / the mesh heap frees) stays correct.
	AddDeferredTask(std::move(task), TimelinePoint{
		m_lastGraphicsSemaphore,
		m_lastGraphicsValue + MAX_FRAMES_IN_FLIGHT });
	// TOURMALINE_DEFER_DIAG=1: the deferred-queue drain census (why do
	// the editor's target-heap range frees never land while 03_fuzz's
	// claims mode cycles? the marching-offsets signature).
	{
		static const bool deferDiag = [] {
			const char* e = std::getenv("TOURMALINE_DEFER_DIAG");
			return e && std::atoi(e) != 0;
		}();
		if (deferDiag) {
			static uint32_t enq = 0;
			++enq;
			if (enq % 10 == 1) {
				uint64_t counterNow = 0;
				const VkResult rq = m_lastGraphicsSemaphore
					!= VK_NULL_HANDLE
					? vkGetSemaphoreCounterValue(device_,
						m_lastGraphicsSemaphore, &counterNow)
					: VK_ERROR_INITIALIZATION_FAILED;
				std::fprintf(stderr,
					"[DEFER-DIAG] enqueue #%u keyed at value %llu "
					"(lastGraphics=%llu sem=%p counterNow=%llu rq=%d)\n",
					enq,
					(unsigned long long)(m_lastGraphicsValue
						+ MAX_FRAMES_IN_FLIGHT),
					(unsigned long long)m_lastGraphicsValue,
					(void*)m_lastGraphicsSemaphore,
					(unsigned long long)counterNow, int(rq));
			}
		}
	}
}
void smvk::VulkanResourceManager::UpdateDescriptors(uint32_t frameIndex)
{
	if (!(m_needToUpdateTextures || m_needToUpdateSamplers))
		return;

	// check for these missing.
	//maxDescriptorSetUpdateAfterBindSampledImages
	//maxDescriptorSetUpdateAfterBindSamplers
	struct PendingImageWrite
	{
		uint32_t slot;
		VkDescriptorImageInfo info;
		bool storageCapable = false;
		bool isVolume = false; // 3D view: mirror write into binding 5
		bool isCubemap = false; // Cube view: mirror write into binding 6
		bool isMultisample = false; // MSAA image: mirror write into binding 7
	};

	std::vector<PendingImageWrite> imageWrites;
	bindlessImages_.forEach(
		[&](BindlessImage bindless)
	{
		ImageView* view =
			imageViews_.get(bindless.view);

		assert(view);

		PendingImageWrite pending{};

		pending.slot =
			bindless.descriptorIndex;

		pending.info.sampler =
			VK_NULL_HANDLE;

		pending.info.imageView =
			view->vkView;

		pending.info.imageLayout =
			bindless.imageLayout;

		// 3D views get a second descriptor write at the same slot in
		// kBinding_VolumeTextures so Texture3D-typed shaders can reach
		// them (Texture2D[] code simply never gets that slot bound).
		pending.isVolume =
			view->viewType == ImageViewType::View3D;

		// Cube views get a second descriptor write at the same slot in
		// kBinding_Cubemaps so TextureCube-typed shaders can reach
		// them (Texture2D[] code simply never gets that slot bound).
		pending.isCubemap =
			view->viewType == ImageViewType::ViewCube
			|| view->viewType == ImageViewType::ViewCubeArray;

		// Multisampled images (EVSM 2x MSAA depth) get a second descriptor
		// write at the same slot in kBinding_MsaaTextures so
		// Texture2DMS-typed shaders (the per-sample resolve) can reach
		// them (Texture2D[] code cannot fetch MSAA views by type).
		if (!view->image.empty()) {
			if (const Image* img = images_.get(view->image))
				pending.isMultisample =
				img->samples != SampleCount::Count1;
		}

		// STORAGE descriptors require STORAGE usage on the image
		// (VUID-00339): only Storage-capable images get the binding-2
		// mirror write below (EVSM blur targets et al.).
		if (!view->image.empty()) {
			if (const Image* img = images_.get(view->image))
				pending.storageCapable =
				(img->usage & ImageUsage::Storage) != ImageUsage::None;
		}

		imageWrites.push_back(pending);
	}
	);

	// Sampler descriptor array – driven by VulkanSamplerCache.
	// Index 0 = default (Linear Repeat), 1 = shadow comparison, etc.
	// Cache guarantees stable indices; like PipelineCache, same
	// SamplerCreateInfo → same descriptor slot.
	std::vector<VkDescriptorImageInfo> samplerInfos;
	if (samplerCache_) {
		samplerInfos.reserve(samplerCache_->size());
		for (uint32_t i = 0; i < samplerCache_->size(); ++i) {
			VkDescriptorImageInfo info{};
			info.sampler = samplerCache_->at(i);
			samplerInfos.push_back(info);
		}
	}
	else {
		// Fallback for early init / headless without cache
		samplerInfos.resize(2);
		samplerInfos[0].sampler = m_defaultSampler;
		samplerInfos[1].sampler = m_defaultShadowSampler;
	}

	//
	// Update every descriptor set that contains the global bindless table.
	//
	for (uint32_t frame = 0;
		frame < MAX_FRAMES_IN_FLIGHT /*static_cast<uint32_t>(defaultDescriptorSets_.size())*/;
		++frame)
	{

		if (defaultDescriptorSets_[frame] == VK_NULL_HANDLE) {
			continue;
		}

		std::vector<VkWriteDescriptorSet> writes;

		writes.reserve(
			imageWrites.size() + 1
		);

		//
		// Bindless sampled images.
		//
		for (const auto& pending : imageWrites)
		{
			VkWriteDescriptorSet write{
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
			};

			write.dstSet =
				defaultDescriptorSets_[frame];

			write.dstBinding =
				smvk::Bindings::kBinding_Textures;

			write.dstArrayElement =
				pending.slot;

			write.descriptorType =
				VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;

			write.descriptorCount =
				1;

			write.pImageInfo =
				&pending.info;

			writes.push_back(write);
		}
		// Bindless storage images (EVSM async prefilter + future compute).
		// Same slot as the sampled view so a compute shader can read
		// textures[i] and write evsmStorage[i] without extra plumbing.
		// Layout is GENERAL (storage); the sampled binding keeps its own
		// read layout above. vkUpdateDescriptorSets copies synchronously,
		// so the local vector outlives the call (same pattern as samplers).
		std::vector<VkDescriptorImageInfo> storageInfos;
		storageInfos.reserve(imageWrites.size());
		for (const auto& pending : imageWrites) {
			if (!pending.storageCapable)
				continue;
			VkDescriptorImageInfo si{};
			si.sampler = VK_NULL_HANDLE;
			si.imageView = pending.info.imageView;
			si.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			storageInfos.push_back(si);
			VkWriteDescriptorSet write{
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
			};
			write.dstSet = defaultDescriptorSets_[frame];
			write.dstBinding = smvk::Bindings::kBinding_StorageImages;
			write.dstArrayElement = pending.slot;
			write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			write.descriptorCount = 1;
			write.pImageInfo = &storageInfos.back();
			writes.push_back(write);
		}

		// 3D volume textures (SG probe grids): same slot, binding 5. See
		// kBinding_VolumeTextures — Texture3D-typed consumers read these.
		for (const auto& pending : imageWrites) {
			if (!pending.isVolume)
				continue;
			VkWriteDescriptorSet write{
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
			};
			write.dstSet = defaultDescriptorSets_[frame];
			write.dstBinding = smvk::Bindings::kBinding_VolumeTextures;
			write.dstArrayElement = pending.slot;
			write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			write.descriptorCount = 1;
			write.pImageInfo = &pending.info;
			writes.push_back(write);
		}

		// Reflection cubemaps for specular IBL: same slot, binding 6. See
		// kBinding_Cubemaps — TextureCube-typed consumers read these.
		for (const auto& pending : imageWrites) {
			if (!pending.isCubemap)
				continue;
			VkWriteDescriptorSet write{
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
			};
			write.dstSet = defaultDescriptorSets_[frame];
			write.dstBinding = smvk::Bindings::kBinding_Cubemaps;
			write.dstArrayElement = pending.slot;
			write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			write.descriptorCount = 1;
			write.pImageInfo = &pending.info;
			writes.push_back(write);
		}

		// Multisampled 2D textures (EVSM per-sample depth resolve): same
		// slot, kBinding_MsaaTextures — Texture2DMS-typed consumers
		// (evsm_resolve.cs.hlsl) read these.
		for (const auto& pending : imageWrites) {
			if (!pending.isMultisample)
				continue;
			VkWriteDescriptorSet write{
				VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
			};
			write.dstSet = defaultDescriptorSets_[frame];
			write.dstBinding = smvk::Bindings::kBinding_MsaaTextures;
			write.dstArrayElement = pending.slot;
			write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			write.descriptorCount = 1;
			write.pImageInfo = &pending.info;
			writes.push_back(write);
		}

		//
		// Global samplers.
		//
		VkWriteDescriptorSet samplerWrite{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
		};

		samplerWrite.dstSet =
			defaultDescriptorSets_[frame];

		samplerWrite.dstBinding =
			smvk::Bindings::kBinding_Samplers;

		samplerWrite.dstArrayElement =
			0;

		samplerWrite.descriptorType =
			VK_DESCRIPTOR_TYPE_SAMPLER;

		samplerWrite.descriptorCount =
			static_cast<uint32_t>(samplerInfos.size());

		samplerWrite.pImageInfo =
			samplerInfos.data();

		if (!samplerInfos.empty())
			writes.push_back(samplerWrite);

		if (!writes.empty())
		{
			vkUpdateDescriptorSets(
				device_,
				static_cast<uint32_t>(writes.size()),
				writes.data(),
				0,
				nullptr
			);
		}
	}

	m_needToUpdateTextures = false;
	m_needToUpdateSamplers = false;
}

// Freed-slot retirement (see the header + destroy(BindlessImageHandle)
// above). A stale sample now reads the 1x1 opaque-black nil texture
// instead of a destroyed VkImageView: wrong-colors, never device loss.
void smvk::VulkanResourceManager::NullBindlessSlotWrites(uint32_t slot)
{
	if (m_nullImageView == VK_NULL_HANDLE)
		return; // pre-init / teardown: nothing to write with
	if (slot >= MAX_TEXTURES)
		return; // outside the Textures array: nothing was written here
	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
	{
		if (defaultDescriptorSets_[frame] == VK_NULL_HANDLE)
			continue;
		VkDescriptorImageInfo info{};
		info.sampler = VK_NULL_HANDLE; // SAMPLED_IMAGE descriptors
		info.imageView = m_nullImageView;
		// Matches the layout every CreateTexture bindless entry declares
		// and the layout the nil image's own sampled upload left it in;
		// nothing else ever transitions the nil image.
		info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkWriteDescriptorSet writes[4]{};
		uint32_t n = 0;
		auto pushWrite = [&](uint32_t binding) {
			VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet = defaultDescriptorSets_[frame];
			write.dstBinding = binding;
			write.dstArrayElement = slot;
			write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			write.descriptorCount = 1;
			write.pImageInfo = &info;
			writes[n++] = write;
		};
		pushWrite(smvk::Bindings::kBinding_Textures);
		// Mirror bindings keep the same slot-space reads live: 3D views
		// (SG/dense-GI layers — freed on EVERY live-view relaunch), cube
		// views and MSAA views all had their old writes retired here too.
		// The 2D nil view in a typed (3D/cube) array slot is only
		// view-type-mismatched IF a stale index ever reaches a typed read
		// — still strictly safer than a use-after-free of a destroyed
		// view. VolumeTextures declares only MAX_VOLUME_TEXTURES entries.
		if (slot < MAX_VOLUME_TEXTURES)
			pushWrite(smvk::Bindings::kBinding_VolumeTextures);
		pushWrite(smvk::Bindings::kBinding_Cubemaps);
		pushWrite(smvk::Bindings::kBinding_MsaaTextures);
		// kBinding_StorageImages deliberately NOT nulled: the nil texture
		// carries no Storage usage (VUID-00339 forbids the write), and the
		// only consumers (evsm blur/resolve dstIndex) push per-frame from
		// live slots, so no data-side stale slot can reach that binding.
		vkUpdateDescriptorSets(device_, n, writes, 0, nullptr);
	}
}

// Compute this frame's EngineGlobals from the acquire rhythm and upload them
// into the frame's slice. Called by the swapchain after the in-flight slot
// retires (timeline wait), before any pass records, so the slice is safe to
// overwrite (previous use completed). The DESCRIPTOR side never changes: this
// flight's set's kBinding_EngineGlobals permanently points at this slice
// (baked once at init - see the set allocation), so sibling flights still
// executing keep reading THEIR OWN slices - no update-after-bind rewrite
// mid-flight. (The old shared-set rewrite was visible to already-submitted
// command buffers, so every in-flight frame read the newest frame's globals
// instead of its own.)
void smvk::VulkanResourceManager::FlushEngineGlobals(uint32_t frameIndex)
{
	assert(!m_engineGlobalsBuffer.empty());
	assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

	const auto now = std::chrono::steady_clock::now();
	const float time = std::chrono::duration<float>(now - m_globalsStartTime).count();
	// First acquire has no predecessor: deltaTime stays 0.
	const float deltaTime = m_globalsLastAcquireTime.time_since_epoch().count() != 0
		? std::chrono::duration<float>(now - m_globalsLastAcquireTime).count()
		: 0.0f;
	m_globalsLastAcquireTime = now;
	++m_globalsFrameCounter;
	if (deltaTime > 0.0f) {
		m_globalsSmoothedFps += (1.0f / deltaTime - m_globalsSmoothedFps) * 0.05f;
	}

	const smvk::EngineGlobals globals{
		.time = time,
		.deltaTime = deltaTime,
		.frameIndex = m_globalsFrameCounter,
		.fps = m_globalsSmoothedFps,
	};

	const size_t sliceOffset = frameIndex * sizeof(smvk::EngineGlobals);

	// Same upload path as all per-frame buffers (streamer/staging + barrier +
	// sync-state tracking), so the rendergraph barriers apply on first use.
	UploadDataToBuffer(
		m_engineGlobalsBuffer,
		&globals,
		sizeof(smvk::EngineGlobals),
		VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
		| VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
		| VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		VK_ACCESS_2_SHADER_READ_BIT,
		0,
		sliceOffset
	);
}

[[nodiscard]] uint32_t smvk::VulkanResourceManager::GetBindlessIndex(BindlessImageHandle handle) const
{
	assert(!handle.empty());
	return handle.index();
}
// Bindless descriptor-array slot of a TextureHandle's sampled view. Unlike
// the texture pool index (which material slots happen to share by creation
// order), this is the authoritative slot for e.g. per-light cookie textures.
[[nodiscard]] uint32_t smvk::VulkanResourceManager::GetTextureBindlessIndex(TextureHandle handle) const
{
	const Texture* texture = textures_.get(handle);
	assert(texture && !texture->bindless.empty());
	return GetBindlessIndex(texture->bindless);
}
bool smvk::VulkanResourceManager::IsCubemapTexture(TextureHandle handle) const
{
	const Texture* texture = textures_.get(handle);
	if (!texture)
		return false;
	const Image* image = images_.get(texture->image);
	const ImageView* view = imageViews_.get(texture->view);
	if (!image || !view)
		return false;
	if (!image->isCubemap)
		return false;
	return view->viewType == ImageViewType::ViewCube
		|| view->viewType == ImageViewType::ViewCubeArray;
}

// Live-texture listing (see the header contract): pool order, one row per
// live texture. Render thread only (the ShardedPool walk is
// unsynchronized — the documented forEach contract).
std::vector<smvk::TextureInfo> smvk::VulkanResourceManager::ListTextureInfos()
{
	std::vector<smvk::TextureInfo> out;
	textures_.forEachHandle([&](TextureHandle handle) {
		const Texture* texture = textures_.get(handle);
		if (!texture)
			return;
		const Image* image = images_.get(texture->image);
		if (!image)
			return;
		smvk::TextureInfo info{};
		info.handle = handle;
		info.name = image->name;
		info.format = image->format;
		info.type = image->type;
		info.extent = image->extent;
		info.mipLevels = image->mipLevels;
		info.arrayLayers = image->arrayLayers;
		info.isCubemap = image->isCubemap;
		info.bindless = texture->bindless.empty()
			? -1 : int32_t(GetBindlessIndex(texture->bindless));
		// Allocation ledger: the backing image's exact committed bytes
		// (0 = untracked external/heap-placed backing).
		info.allocBytes = image->allocBytes;
		out.push_back(info);
	});
	return out;
}
uint32_t smvk::VulkanResourceManager::GetTextureArrayLayers(TextureHandle handle) const
{
	const Texture* texture = textures_.get(handle);
	if (!texture)
		return 0;
	const Image* image = images_.get(texture->image);
	return image ? image->arrayLayers : 0;
}
smvk::ImageHandle smvk::VulkanResourceManager::GetTextureImageHandle(TextureHandle handle) const
{
	// The texture's underlying image (for standalone view creation on
	// it). Dead texture handles (generation bumped) return an empty
	// handle - CreateImageView's assert never sees a stale entry.
	const Texture* texture = textures_.get(handle);
	return texture ? texture->image : smvk::ImageHandle{};
}
uint32_t smvk::VulkanResourceManager::GetTextureCubemapSize(TextureHandle handle) const
{
	const Texture* texture = textures_.get(handle);
	if (!texture)
		return 0;
	const Image* image = images_.get(texture->image);
	if (!image || !image->isCubemap)
		return 0;
	return image->extent.width;
}
uint32_t smvk::VulkanResourceManager::GetTextureMipLevels(TextureHandle handle) const
{
	const Texture* texture = textures_.get(handle);
	if (!texture)
		return 0;
	const Image* image = images_.get(texture->image);
	return image ? image->mipLevels : 0;
}
// Raw Vulkan interop (see vulkan_impl.h): the texture's sampled
// VkImageView + a cached sampler's VkSampler, as opaque pointers.
VkImageView smvk::VulkanResourceManager::GetTextureVkImageView(TextureHandle handle) const
{
	const Texture* texture = textures_.get(handle);
	if (!texture)
		return VK_NULL_HANDLE;
	const ImageView* view = imageViews_.get(texture->view);
	return view ? view->vkView : VK_NULL_HANDLE;
}
VkImageView smvk::VulkanResourceManager::GetImageViewVkImageView(ImageViewHandle handle) const
{
	// Same interop for a standalone (caller-created) view: pool-get
	// returns null for dead handles (generation bumped), so a freed
	// view can never slip into a descriptor.
	const ImageView* view = imageViews_.get(handle);
	return view ? view->vkView : VK_NULL_HANDLE;
}
const void* smvk::VulkanResourceManager::GetVkSamplerOpaque(uint32_t samplerIndex) const
{
	if (samplerIndex >= GetSamplerCount())
		return nullptr;
	return GetVkSampler(samplerIndex);
}
uint32_t smvk::VulkanResourceManager::GetOrCreateSampler(const SamplerCreateInfo& ci) {
	assert(samplerCache_ && "samplerCache not initialized");
	return samplerCache_->GetOrCreate(ci);
}
VkSampler smvk::VulkanResourceManager::GetVkSampler(uint32_t index) const {
	assert(samplerCache_ && "samplerCache not initialized");
	return samplerCache_->at(index);
}
uint32_t smvk::VulkanResourceManager::GetSamplerCount() const {
	return samplerCache_ ? samplerCache_->size() : 0;
}
bool smvk::VulkanResourceManager::RecreateSampler(uint32_t index, const SamplerCreateInfo& ci) {
	if (!samplerCache_)
		return false;
	return samplerCache_->Recreate(index, ci);
}
// ============================================================================
// Sync bookkeeping – per-thread shards, lock-free hot path.
// Shard is decoded from the handle index so each thread only touches its
// own map. Snapshot/Commit merge all shards and run quiescent (render
// thread after worker join / rendergraph setup).
// ============================================================================
smvk::VulkanResourceManager::ImageGlobalSyncState smvk::VulkanResourceManager::GetImageSyncState(ImageHandle h) const {
	if (h.empty()) return {};
	uint32_t shard = SyncShard(h.index());
	assert(shard < kNumPoolShards);
	auto it = imageSyncShards_[shard].find(SyncKey(h));
	if (it != imageSyncShards_[shard].end()) return it->second;
	return {};
}
smvk::VulkanResourceManager::BufferGlobalSyncState smvk::VulkanResourceManager::GetBufferSyncState(BufferHandle h) const {
	if (h.empty()) return {};
	uint32_t shard = SyncShard(h.index());
	assert(shard < kNumPoolShards);
	auto it = bufferSyncShards_[shard].find(SyncKey(h));
	if (it != bufferSyncShards_[shard].end()) return it->second;
	return {};
}
void smvk::VulkanResourceManager::SetImageSyncState(ImageHandle h, ImageGlobalSyncState state) {
	if (h.empty()) return;
	uint32_t shard = SyncShard(h.index());
	assert(shard < kNumPoolShards);
	imageSyncShards_[shard][SyncKey(h)] = state;
}
void smvk::VulkanResourceManager::SetBufferSyncState(BufferHandle h, BufferGlobalSyncState state) {
	if (h.empty()) return;
	uint32_t shard = SyncShard(h.index());
	assert(shard < kNumPoolShards);
	bufferSyncShards_[shard][SyncKey(h)] = state;
}
void smvk::VulkanResourceManager::RemoveImageSyncState(ImageHandle h) {
	if (h.empty()) return;
	uint32_t shard = SyncShard(h.index());
	assert(shard < kNumPoolShards);
	imageSyncShards_[shard].erase(SyncKey(h));
}
void smvk::VulkanResourceManager::RemoveBufferSyncState(BufferHandle h) {
	if (h.empty()) return;
	uint32_t shard = SyncShard(h.index());
	assert(shard < kNumPoolShards);
	bufferSyncShards_[shard].erase(SyncKey(h));
}
std::unordered_map<uint64_t, smvk::VulkanResourceManager::ImageGlobalSyncState> smvk::VulkanResourceManager::SnapshotImageSyncStates() const {
	std::unordered_map<uint64_t, smvk::VulkanResourceManager::ImageGlobalSyncState> out;
	for (uint32_t s = 0; s < kNumPoolShards; ++s)
		for (auto& kv : imageSyncShards_[s]) out[kv.first] = kv.second;
	return out;
}
std::unordered_map<uint64_t, smvk::VulkanResourceManager::BufferGlobalSyncState> smvk::VulkanResourceManager::SnapshotBufferSyncStates() const {
	std::unordered_map<uint64_t, smvk::VulkanResourceManager::BufferGlobalSyncState> out;
	for (uint32_t s = 0; s < kNumPoolShards; ++s)
		for (auto& kv : bufferSyncShards_[s]) out[kv.first] = kv.second;
	return out;
}
void smvk::VulkanResourceManager::CommitImageSyncStates(const std::unordered_map<uint64_t, ImageGlobalSyncState>& states) {
	for (auto& kv : states) {
		uint32_t idx = static_cast<uint32_t>(kv.first & 0xffffffffull);
		uint32_t shard = SyncShard(idx);
		assert(shard < kNumPoolShards);
		imageSyncShards_[shard][kv.first] = kv.second;
	}
}
bool smvk::VulkanSwapchain::EnsureAsyncSplit(uint32_t flight) {
	if (flight >= MAX_FRAMES_IN_FLIGHT)
		return false;
	if (asyncSplitReady_[flight])
		return true;
	QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
	if (!indices.graphicsFamily || !indices.computeFamily
		|| *indices.computeFamily == *indices.graphicsFamily)
		return false; // single queue: emulated async (single submit)
	VkCommandPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolCI.queueFamilyIndex = *indices.graphicsFamily;
	if (vkCreateCommandPool(device_, &poolCI, nullptr, &splitPools[flight]) != VK_SUCCESS)
		return false;
	VkCommandBufferAllocateInfo allocCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocCI.commandPool = splitPools[flight];
	allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocCI.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(device_, &allocCI, &splitBuffers[flight]) != VK_SUCCESS)
		return false;
	poolCI.queueFamilyIndex = *indices.computeFamily;
	if (vkCreateCommandPool(device_, &poolCI, nullptr, &asyncComputePools[flight]) != VK_SUCCESS)
		return false;
	allocCI.commandPool = asyncComputePools[flight];
	if (vkAllocateCommandBuffers(device_, &allocCI, &asyncComputeBuffers[flight]) != VK_SUCCESS)
		return false;
	VkSemaphoreTypeCreateInfo typeCI{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
	typeCI.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	typeCI.initialValue = 0;
	VkSemaphoreCreateInfo semCI{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &typeCI };
	if (vkCreateSemaphore(device_, &semCI, nullptr, &asyncG2C_[flight]) != VK_SUCCESS)
		return false;
	if (vkCreateSemaphore(device_, &semCI, nullptr, &asyncC2G_[flight]) != VK_SUCCESS)
		return false;
	asyncSplitReady_[flight] = true;
	{
		static bool logged = false;
		if (!logged) {
			logged = true;
			std::fprintf(stderr,
				"[AsyncCompute] true-async split ready: graphics family %u, compute family %u "
				"(G1/C/G2 timeline chain per flight)\n",
				(unsigned)*indices.graphicsFamily, (unsigned)*indices.computeFamily);
			fflush(stderr);
		}
	}
	return true;
}
// Grows the per-flight async-pass CB pool to at least `count` (one CB per
// async pass per frame — timeline-chained per-pass submits in Execute).
// Handles persist across frames; the per-flight pool reset in Acquire
// returns them to recordable state alongside the split CBs.
bool smvk::VulkanSwapchain::EnsureAsyncPassBuffers(uint32_t flight, uint32_t count) {
	if (flight >= MAX_FRAMES_IN_FLIGHT || !asyncSplitReady_[flight])
		return false;
	if (asyncPassBuffers[flight].size() >= count)
		return true;
	const size_t oldSize = asyncPassBuffers[flight].size();
	asyncPassBuffers[flight].resize(count, VK_NULL_HANDLE);
	VkCommandBufferAllocateInfo allocCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocCI.commandPool = asyncComputePools[flight];
	allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocCI.commandBufferCount = count - static_cast<uint32_t>(oldSize);
	if (vkAllocateCommandBuffers(device_, &allocCI, asyncPassBuffers[flight].data() + oldSize) != VK_SUCCESS) {
		asyncPassBuffers[flight].resize(oldSize);
		return false;
	}
	return true;
}
void smvk::VulkanResourceManager::CommitBufferSyncStates(const std::unordered_map<uint64_t, BufferGlobalSyncState>& states) {
	for (auto& kv : states) {
		uint32_t idx = static_cast<uint32_t>(kv.first & 0xffffffffull);
		uint32_t shard = SyncShard(idx);
		assert(shard < kNumPoolShards);
		bufferSyncShards_[shard][kv.first] = kv.second;
	}
}
// 
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::BindlessImageHandle handle)
{
	if (handle.empty())
		return;

	auto* bindless =
		ctx->bindlessImages_.get(handle);

	assert(bindless);

	const uint32_t slot =
		bindless->descriptorIndex;

	// RETIRE the whole slot release — the pool entry (the slot HOLD), the
	// nil-image null write and the update flag — on the graphics timeline,
	// not in place. An in-flight frame can still be sampling this slot:
	// until the retire point completes, the entry stays LIVE so
	// UpdateDescriptors keeps the slot pointing at the STILL-ALIVE old
	// view (the view's own destroy queues right behind this task in the
	// same FIFO batch — see destroy(TextureHandle)), and every in-flight
	// frame keeps sampling the texture it was recorded with. The old
	// immediate release let the next CreateBindlessImage claim the slot
	// while old frames were still reading it (an in-flight frame sampled
	// the NEW texture or the nil image for a frame - the bindless-slot
	// ABA hazard, a one-frame flicker under churn).
	//
	// The null write itself (see the method comment on
	// NullBindlessSlotWrites): UpdateDescriptors only rewrites slots with
	// LIVE entries, so without it the freed slot would keep pointing at
	// the view being destroyed by the task queued right after this one —
	// any stale slot reference (material texture slots, editor live-view
	// relaunch churn, one-frame-late pushes) sampled a destroyed
	// VkImageView and took the device down (the [MATSLOT]-class device
	// death; the editor's relaunch path frees ~dozens of slots per boot,
	// so the window grew with every resize until it fired). Retiring the
	// write (not just the entry) keeps that ordering: the nil write lands
	// BEFORE the companion view destroy runs in the same FIFO drain, so
	// no window ever exposes a dying VkImageView through the table.
	std::packaged_task<void()> task([ctx, handle, slot] {
		ctx->bindlessImages_.destroy(handle);
		ctx->NullBindlessSlotWrites(slot);
		// If descriptors are replicated per-frame.
		ctx->m_needToUpdateTextures = true;
	});
	ctx->DeferDestroy(std::move(task));
}
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::TextureHandle handle)
{
	if (handle.empty())
		return;

	auto* texture =
		ctx->textures_.get(handle);

	assert(texture);

	// Descriptor references view.
	destroy(ctx, texture->bindless);

	// View references image.
	destroy(ctx, texture->view);

	// Image is now independent.
	destroy(ctx, texture->image);

	ctx->textures_.destroy(handle);
}
namespace smvk {
	namespace {
	void DestroyBufferNow(VulkanResourceManager* ctx, BufferHandle handle) {
		// TOURMALINE_RB_DIAG=1 (env-gated, the churn-death hunt's readback
		// collision detector): this destroy runs from the DEFERRED retire
		// (the swapchain's MAX_FRAMES_IN_FLIGHT margin), but the async
		// readback ticket rides the Submitter's PRIVATE timeline — unordered
		// from the swapchain's. If the buffer destroyed here is a source of
		// a ticket still in flight, the copy reads a destroyed VkBuffer.
		// The hunt's verdict: never fired (the death was the SSR ray
		// counter's missing creation zero-fill — ssr.h); the guard stays for
		// future readback-lifetime questions.
		if (ctx->m_asyncReadbackTicket != 0 && !ctx->m_asyncReadbackSources.empty()) {
			for (const auto& rbSrc : ctx->m_asyncReadbackSources) {
				if (rbSrc == handle) {
					std::fprintf(stderr,
						"[RB-DIAG] READBACK COLLISION: destroying buffer "
						"idx=%u gen=%u while readback ticket %llu is still "
						"in flight (the copy's source dies under the "
						"ticket)\n",
						handle.index(), handle.gen(),
						static_cast<unsigned long long>(
							ctx->m_asyncReadbackTicket));
					break;
				}
			}
		}
		Buffer db = *ctx->buffers_.get(handle);
		ctx->RemoveBufferSyncState(handle);
		vmaDestroyBuffer(ctx->m_allocator, db.vkBuffer, db.vma_alloc);
		ctx->buffers_.destroy(handle);
	}
	}
} // namespace smvk::<anonymous>
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::BufferHandle handle) {
	if (handle.empty()) { return; };
	// Buffers are referenced by in-flight draws/indirect args; retire on the
	// graphics timeline instead of destroying under the GPU.
	std::packaged_task<void()> task([ctx, handle] { DestroyBufferNow(ctx, handle); });
	ctx->DeferDestroy(std::move(task));
}
// Target-heap claim release (the Holder deleter - see
// ClaimTargetHeapRange's contract): drops the slot's live-user count; the
// LAST user's release queues the range free retirement-ordered. In target
// structs the claim Holder is declared BEFORE the image Holder so the
// claim releases BEHIND the image's deferred destroy (the free must land
// behind the destroy in the FIFO).
void smvk::destroy(smvk::VulkanResourceManager* ctx,
	smvk::TargetHeapClaimHandle handle) {
	if (handle.empty())
		return;
	ctx->ReleaseTargetHeapClaim(handle);
}
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::ShaderModuleHandle handle) {
	if (handle.empty()) { return; };
	auto sm = ctx->shaderModules_.get(handle);
	vkDestroyShaderModule(device_, sm->module, NULL);
	ctx->shaderModules_.destroy(handle);
}
namespace smvk {
	namespace {
		void DestroyImageNow(VulkanResourceManager* ctx, ImageHandle handle)
		{
			ctx->RemoveImageSyncState(handle);
			auto* image = ctx->images_.get(handle);
			assert(image);

			// ImageViews must be destroyed first. Holds because view teardown is
			// always queued before image teardown with the same retire point and the
			// queue drains FIFO (see destroy(TextureHandle), FreeBlurTarget, etc.).
			assert(
				image->liveViewCount == 0 &&
				"Destroying Image while ImageViews still reference it"
			);

			if (image->ownership == ResourceOwnership::Owned &&
				image->vkImage != VK_NULL_HANDLE)
			{
				if (image->transientHeap) {
					// Heap-placed (rendergraph design B): no VmaAllocation - the
					// packed-range memory is the graph's heap block (frees
					// wholesale with the graph). Plain vkDestroyImage only.
					assert(image->allocation == VK_NULL_HANDLE);
					vkDestroyImage(device_, image->vkImage, nullptr);
				}
				else {
					vmaDestroyImage(
						ctx->m_allocator,
						image->vkImage,
						image->allocation
					);
				}
			}

			ctx->images_.destroy(handle);
		}
		void DestroyImageViewNow(smvk::VulkanResourceManager* ctx, smvk::ImageViewHandle handle)
		{
			auto* view = ctx->imageViews_.get(handle);
			assert(view);

			if (view->vkView != VK_NULL_HANDLE &&
				view->ownership != ResourceOwnership::External)
			{
				vkDestroyImageView(
					device_,
					view->vkView,
					nullptr
				);
			}

			// Maintain parent dependency count.
			if (!view->image.empty())
			{
				auto* image =
					ctx->images_.get(view->image);

				assert(image);
				assert(image->liveViewCount > 0);

				--image->liveViewCount;
			}

			ctx->imageViews_.destroy(handle);
		}
	}
} // namespace smvk::<anonymous>
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::ImageHandle handle)
{
	if (handle.empty())
		return;
	// Images are referenced by in-flight command buffers (attachments and
	// descriptors). vkDestroyImage while in use is VUID-vkDestroyImage-01000
	// and ends in DEVICE_LOST; retire on the graphics timeline instead.
	// Pool slot stays occupied until retirement, so no aliasing in between.
	std::packaged_task<void()> task([ctx, handle] { DestroyImageNow(ctx, handle); });
	ctx->DeferDestroy(std::move(task));
}
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::ImageViewHandle handle)
{
	if (handle.empty())
		return;
	// Same use-after-free class as images (descriptors reference the view).
	std::packaged_task<void()> task([ctx, handle] { DestroyImageViewNow(ctx, handle); });
	ctx->DeferDestroy(std::move(task));
}
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::SamplerHandle handle) {
	if (handle.empty()) { return; };
	auto s = ctx->samplers_.get(handle);
	if (*s != VK_NULL_HANDLE) {
		vkDestroySampler(device_, *s, nullptr);
	}
	ctx->samplers_.destroy(handle);
}
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::DescriptorPoolHandle handle) {
	if (handle.empty()) { return; };
	auto dp = ctx->descriptorPools_.get(handle);
	if (*dp != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(device_, *dp, nullptr);
	}
	ctx->descriptorPools_.destroy(handle);
}

// Barriers
inline void BufferBarrier(
	VkCommandBuffer cmd,
	VkBuffer buffer,
	VkDeviceSize offset,
	VkDeviceSize size,
	VkPipelineStageFlags2 srcStage,
	VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage,
	VkAccessFlags2 dstAccess
) {

	assert(dstAccess == 0 || dstStage != 0);
	VkBufferMemoryBarrier2 barrier{
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
		.pNext = nullptr,
		.srcStageMask = srcStage,
		.srcAccessMask = srcAccess,
		.dstStageMask = dstStage,
		.dstAccessMask = dstAccess,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = buffer,
		.offset = offset,
		.size = size,
	};
	VkDependencyInfo dep{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.pNext = nullptr,
		.dependencyFlags = 0,
		.memoryBarrierCount = 0,
		.pMemoryBarriers = nullptr,
		.bufferMemoryBarrierCount = 1,
		.pBufferMemoryBarriers = &barrier,
		.imageMemoryBarrierCount = 0,
		.pImageMemoryBarriers = nullptr,
	};
	vkCmdPipelineBarrier2(cmd, &dep);
}


// (smvk-qualified: this sat at GLOBAL scope (after `} // namespace
// smvk::<anonymous>`), defining ::ImageBarrier while the callers bound to
// the extern-declared smvk::ImageBarrier - an undefined-reference at link
// on GCC/Clang. Qualified out-of-line definition, no `inline` - the
// single body every TU links. See the NOTE at the forward declaration.)
void smvk::ImageBarrier(
	VkCommandBuffer cmd,
	VkImage vk_image,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	VkImageSubresourceRange subresourceRange,
	VkPipelineStageFlags2 srcStage,
	VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage,
	VkAccessFlags2 dstAccess,
	uint32_t srcQueueFamilyIndex,
	uint32_t dstQueueFamilyIndex
)
{
	VkImageMemoryBarrier2 imageBarrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.pNext = nullptr,

		.srcStageMask = srcStage,
		.srcAccessMask = srcAccess,
		.dstStageMask = dstStage,
		.dstAccessMask = dstAccess,

		.oldLayout = oldLayout,
		.newLayout = newLayout,

		.srcQueueFamilyIndex = srcQueueFamilyIndex,
		.dstQueueFamilyIndex = dstQueueFamilyIndex,

		.image = vk_image,
		.subresourceRange = subresourceRange,
	};

	VkDependencyInfo dependencyInfo{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.pNext = nullptr,
		.dependencyFlags = 0,
		.memoryBarrierCount = 0,
		.pMemoryBarriers = nullptr,
		.bufferMemoryBarrierCount = 0,
		.pBufferMemoryBarriers = nullptr,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &imageBarrier,
	};

	vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

// Submitter – now uses thread-local CommandBufferPool
namespace {
	// Device-loss post-mortem (VK_EXT_device_fault) — defined below, called
	// from the acquire-loss path and the loud aborts in Submitter::purge.
	void DumpDeviceFaultInfo();
}
inline smvk::Submitter::Submitter(VulkanCore& core, uint32_t queueFamilyIndex)
	: core_(core), m_queueFamilyIndex(queueFamilyIndex)
{
	// Resolve the VkQueue for this family: prefer the cached globals
	// (transfer / compute / graphics / present), fall back to a fresh
	// family query for exotic configurations.
	QueueFamilyIndices q = findQueueFamilies(physicalDevice_, surface_);
	if (q.computeFamily && *q.computeFamily == queueFamilyIndex && computeQueue_ != VK_NULL_HANDLE) m_queue = computeQueue_;
	else if (q.transferFamily && *q.transferFamily == queueFamilyIndex && transferQueue_ != VK_NULL_HANDLE) m_queue = transferQueue_;
	else if (q.graphicsFamily && *q.graphicsFamily == queueFamilyIndex) m_queue = graphicsQueue_;
	else if (q.presentFamily && *q.presentFamily == queueFamilyIndex) m_queue = presentQueue_;
	else {
		if (queueFamilyIndex == core.indices.transferFamily.value_or(queueFamilyIndex))
			m_queue = transferQueue_ != VK_NULL_HANDLE ? transferQueue_ : graphicsQueue_;
		else
			m_queue = graphicsQueue_;
	}
	if (m_queue == VK_NULL_HANDLE) m_queue = graphicsQueue_;
	// Command pool is now thread-local – no pool creation here
	CreateTimelineSemaphore(0, m_timelineSemaphore);
	assert(m_timelineSemaphore != VK_NULL_HANDLE);
	m_timelineValue = 1;
	m_lastTimelineValue = 0;
}
inline smvk::Submitter::~Submitter()
{
	if (device_ == VK_NULL_HANDLE)
		return;
	waitAll();
	if (m_timelineSemaphore != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(device_, m_timelineSemaphore, nullptr);
		m_timelineSemaphore = VK_NULL_HANDLE;
	}
	// Thread-local pools are destroyed per-thread on exit, not here
}
inline smvk::CommandBufferPool* smvk::Submitter::getPool() const {
	return GetThreadLocalPool(m_queueFamilyIndex, device_, this);
}
smvk::TimelinePoint smvk::Submitter::nextSignalPoint() const
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	return TimelinePoint{
		.semaphore = m_timelineSemaphore,
		.value = m_timelineValue
	};
}
VkResult smvk::Submitter::waitTimeline(uint64_t value, uint64_t timeout) const
{
	assert(value != 0);
	assert(m_timelineSemaphore != VK_NULL_HANDLE);

	// Host timeline-wait trace (TOURMALINE_TRACE_WAITS=1): the documented
	// driver poison recipe is a HOST wait on a presented-signaled timeline
	// (see AcquireFramebuffer) — every wait goes through here, so this
	// names the offender when a later counter query reads UINT64_MAX.
	static const bool sTrace = []() {
		const char* e = std::getenv("TOURMALINE_TRACE_WAITS");
		return e && std::atoi(e) != 0;
	}();
	if (sTrace) {
		std::fprintf(stderr, "[WAIT] host waitTimeline qfi=%u value=%llu (next=%llu)\n",
			m_queueFamilyIndex,
			(unsigned long long)value,
			(unsigned long long)m_timelineValue);
		std::fflush(stderr);
	}

	VkSemaphore semaphore =
		m_timelineSemaphore;

	VkSemaphoreWaitInfo waitInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &semaphore,
		.pValues = &value
	};

	return vkWaitSemaphores(
		device_,
		&waitInfo,
		timeout);
}
[[nodiscard]] inline smvk::CommandSubmitHandle smvk::Submitter::acquire()
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);
	purge();
	auto* pool = getPool();
	if (pool->availableSlots == 0)
	{
		uint64_t oldest = UINT64_MAX;
		for (uint32_t i = 0; i < CommandBufferPool::kMax; ++i) {
			const auto& slot = pool->slots[i];
			if (slot.state_ == WrapperSlot::CommandState::Submitted) oldest = std::min(oldest, slot.retireValue_);
		}
		if (oldest != UINT64_MAX)
		{
			lock.unlock();
			VK_CHECK(waitTimeline(oldest, UINT64_MAX));
			lock.lock();
			purge();
			pool = getPool();
			if (pool->availableSlots == 0) { assert(false && "still no slot after wait"); return {}; }
		}
		else { assert(false && "All command buffers are currently recording"); return {}; }
	}
	uint32_t pickedIndex = UINT32_MAX;
	for (uint32_t i = 0; i < CommandBufferPool::kMax; ++i) if (pool->slots[i].state_ == WrapperSlot::CommandState::Free) { pickedIndex = i; break; }
	assert(pickedIndex != UINT32_MAX);
	WrapperSlot& slot = pool->slots[pickedIndex];
	slot.cmd_ = slot.allocatedCmd_;
	slot.state_ = WrapperSlot::CommandState::Recording;
	slot.retireValue_ = 0;
	VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VK_CHECK(vkBeginCommandBuffer(slot.cmd_, &beginInfo));
	--pool->availableSlots;
	return CommandSubmitHandle{ pool, pickedIndex, slot.gen_ };
}
[[nodiscard]] VkCommandBuffer smvk::Submitter::GetCommandBuffer(CommandSubmitHandle handle)
{
	assert(validHandle(handle));
	auto* pool = handle.pool; assert(pool);
	WrapperSlot& slot = pool->slots[handle.index_];
	assert(slot.state_ == WrapperSlot::CommandState::Recording);
	return slot.cmd_;
}

// TMP-DIAG forward decl (the submit fault-dump below needs the later
// definition at DumpDeviceFaultInfo).
namespace { void DumpDeviceFaultInfo(); }

smvk::TimelinePoint smvk::Submitter::submit(
	CommandSubmitHandle handle,
	std::span<const TimelineWait> waits,
	std::source_location loc)
{
	std::unique_lock<std::recursive_mutex> lock(m_mutex);

	assert(validHandle(handle));

	CommandBufferPool* pool = handle.pool;   // <-- important
	assert(pool);

	WrapperSlot& slot = pool->slots[handle.index_];

	assert(slot.state_ == WrapperSlot::CommandState::Recording);
	assert(slot.cmd_ != VK_NULL_HANDLE);

	VK_CHECK(vkEndCommandBuffer(slot.cmd_));

	std::vector<VkSemaphoreSubmitInfo> waitInfos;
	waitInfos.reserve(waits.size());

	for (const TimelineWait& wait : waits)
	{
		assert(wait.point.valid());

		waitInfos.push_back(
			VkSemaphoreSubmitInfo{
				.sType =
					VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,

				.semaphore =
					wait.point.semaphore,

				.value =
					wait.point.value,

				.stageMask =
					wait.stageMask,

				.deviceIndex = 0
			});
	}

	const uint64_t signalValue =
		m_timelineValue++;

	assert(signalValue != 0);

	// source_location patch: log caller for debugging timeline/queue issues
#ifdef DEBUG_COMMAND_SUBMISSION
	fprintf(stderr, "[Submit] qfi=%u signal=%llu from %s:%u %s\n",
		m_queueFamilyIndex,
		(unsigned long long)signalValue,
		loc.file_name(), (unsigned)loc.line(), loc.function_name());
#endif

	VkSemaphoreSubmitInfo signalInfo{
		.sType =
			VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,

		.semaphore =
			m_timelineSemaphore,

		.value =
			signalValue,

		.stageMask =
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,

		.deviceIndex = 0
	};

	VkCommandBufferSubmitInfo cmdInfo{
		.sType =
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,

		.commandBuffer =
			slot.cmd_,

		.deviceMask = 0
	};

	VkSubmitInfo2 submitInfo{
		.sType =
			VK_STRUCTURE_TYPE_SUBMIT_INFO_2,

		.waitSemaphoreInfoCount =
			static_cast<uint32_t>(waitInfos.size()),

		.pWaitSemaphoreInfos =
			waitInfos.empty()
				? nullptr
				: waitInfos.data(),

		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &cmdInfo,

		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &signalInfo
	};

	{
		std::lock_guard<std::mutex> qlock(GetQueueMutex(m_queue));
		// TMP-DIAG (the editor churn DEVICE_LOST hunt): the submit's
		// failure path dumps the VK_EXT_device_fault post-mortem BEFORE
		// the abort (VK_CHECK dies silently; the fault info names the
		// faulting addresses/commands).
		const VkResult submitErr = vkQueueSubmit2(
			m_queue,
			1,
			&submitInfo,
			VK_NULL_HANDLE);
		if (submitErr != VK_SUCCESS) {
			std::fprintf(stderr,
				"[Vulkan Error] %s:%d: %s\n", __FILE__, __LINE__,
				vkResultToString(submitErr));
			DumpDeviceFaultInfo();
			std::fflush(stderr);
			std::abort();
		}
	}

	slot.state_ = CommandState::Submitted;
	slot.retireValue_ = signalValue;

	m_lastTimelineValue = signalValue;
	m_lastSubmit = handle;

	// TOURMALINE_SERIALIZE=uploads (see RgSerializeGate): every engine-side
	// submit funnels through here — upload batches, acquire barriers,
	// readback copies, one-off dumps.
	RgSerializeGate(RgSerializeMode().uploads, m_queue, GetQueueMutex(m_queue));

	return {
		.semaphore = m_timelineSemaphore,
		.value = signalValue
	};
}
inline bool smvk::Submitter::validHandle(CommandSubmitHandle handle) const
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	if (!handle.pool)
		return false;
	if (handle.index_ >= CommandBufferPool::kMax)
		return false;
	if (handle.gen_ == 0)
		return false;
	const WrapperSlot& slot =
		handle.pool->slots[handle.index_];
	return handle.gen_ == slot.gen_;
}
//inline bool smvk::Submitter::validHandle(CommandSubmitHandle handle) const
//{
//	std::lock_guard<std::recursive_mutex> lock(m_mutex);
//	if (handle.index_ >= CommandBufferPool::kMax) return false;
//	if (handle.gen_ == 0) return false;
//	if (handle.pool == nullptr) return false;
//	return handle.gen_ == handle.pool->slots[handle.index_].gen_;
//}
bool smvk::Submitter::isReady(CommandSubmitHandle handle)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	if (handle.index_ >= CommandBufferPool::kMax) return false;
	if (handle.pool == nullptr) return false;
	auto* pool = handle.pool;
	WrapperSlot& slot = pool->slots[handle.index_];

	// Old generation already recycled.
	if (handle.gen_ < slot.gen_)
		return true;

	if (handle.gen_ != slot.gen_)
		return false;

	if (slot.state_ == WrapperSlot::CommandState::Recording) return false;
	if (slot.state_ == WrapperSlot::CommandState::Free) return true;
	uint64_t completed = 0;

	VkResult r = vkGetSemaphoreCounterValue(
		device_,
		m_timelineSemaphore,
		&completed);
	if (r == VK_ERROR_DEVICE_LOST) {
		// Device lost during shutdown - treat as ready to allow teardown
		return true;
	}
	VK_CHECK(r);

	return
		completed >=
		slot.retireValue_;
}
namespace { void DumpDeviceFaultInfo(); }
// Fatal-path mirror: GUI/editor runs often have no visible console, so the
// deliberate aborts below write to stderr AND append to
// tourmaline_fatal.log in the working directory. FatalPrintf behaves exactly
// like fprintf(stderr, ...) until FatalMirrorOpen() runs (fatal paths
// only — never hot). The mirror is fclose()d before abort() because abort
// does not flush stdio buffers (an unflushed file would stay empty).
namespace {
	static FILE* g_fatalMirror = nullptr;
	void FatalMirrorOpen() {
		if (!g_fatalMirror)
			g_fatalMirror = std::fopen("tourmaline_fatal.log", "a");
	}
	void FatalMirrorClose() {
		if (g_fatalMirror) {
			std::fclose(g_fatalMirror);
			g_fatalMirror = nullptr;
		}
	}
	void FatalPrintf(const char* fmt, ...) {
		va_list a1, a2;
		va_start(a1, fmt);
		va_copy(a2, a1);
		std::vfprintf(stderr, fmt, a1);
		if (g_fatalMirror)
			std::vfprintf(g_fatalMirror, fmt, a2);
		va_end(a1);
		va_end(a2);
	}
	void FatalFlush() {
		std::fflush(stderr);
		if (g_fatalMirror)
			std::fflush(g_fatalMirror);
	}
}
void smvk::Submitter::purge()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);

	uint64_t completedValue = 0;

	VkResult r = vkGetSemaphoreCounterValue(
		device_,
		m_timelineSemaphore,
		&completedValue);

	if (r == VK_ERROR_DEVICE_LOST) {
		PoolKey key{ device_, m_queueFamilyIndex, this };
		std::vector<CommandBufferPool*> pools;
		{
			std::lock_guard<std::mutex> g(g_allPoolsMutex);
			auto it = g_allPools.find(key);
			if (it != g_allPools.end()) pools = it->second;
		}
		if (pools.empty()) {
			auto* single = getPool();
			if (single) pools.push_back(single);
		}
		for (auto* pool : pools) {
			for (uint32_t i = 0; i < CommandBufferPool::kMax; ++i) {
				WrapperSlot& slot = pool->slots[i];
				if (slot.state_ == WrapperSlot::CommandState::Submitted) {
					slot.cmd_ = VK_NULL_HANDLE;
					slot.state_ = WrapperSlot::CommandState::Free;
					slot.retireValue_ = 0;
					++slot.gen_; if (slot.gen_ == 0) slot.gen_ = 1;
					++pool->availableSlots;
				}
			}
		}
		return;
	}
	VK_CHECK(r);


#ifdef DEBUG_COMMAND_SUBMISSION
	fprintf(stderr,
		"[PURGE] this=%p qfi=%u semaphore=%p "
		"completed=%llu next=%llu last=%llu\n",
		this,
		m_queueFamilyIndex,
		(void*)m_timelineSemaphore,
		(unsigned long long)completedValue,
		(unsigned long long)m_timelineValue,
		(unsigned long long)m_lastTimelineValue);
#endif

	if (completedValue == UINT64_MAX) {

		// Never abort silently: this exact abort (previously message-less,
		// surfacing as a bare 0xC0000409 ucrtbase fastfail) hid the frame-1
		// bench death for weeks. A UINT64_MAX counter means the driver's
		// semaphore-query path is poisoned (historically triggered by the
		// host timeline-wait in AcquireFramebuffer — now fence-based; see
		// the comment there). Dump the device-fault post-mortem while the
		// device still exists, then die loudly.
		// Probe whether the device is already lost: some drivers answer
		// the counter query with SUCCESS + garbage (all-ones) instead of
		// VK_ERROR_DEVICE_LOST. Distinguishing "lost device" from "poisoned
		// query on a live device" decides the whole investigation, so ask
		// the device directly (nothing is pending on this path at startup;
		// on a live device this returns immediately).
		VkResult idleResult = vkDeviceWaitIdle(device_);
		FatalMirrorOpen();
		FatalPrintf(
			"[Vulkan FATAL] timeline semaphore counter reads UINT64_MAX "
			"(qfi=%u next=%llu sem=%p queryResult=%s deviceIdle=%s) — %s; aborting\n",
			m_queueFamilyIndex,
			(unsigned long long)m_timelineValue,
			(void*)m_timelineSemaphore,
			vkResultToString(r),
			vkResultToString(idleResult),
			idleResult == VK_ERROR_DEVICE_LOST
			? "device already lost before first use"
			: "driver query path poisoned on a live device");
		FatalFlush();
		DumpDeviceFaultInfo();
		FatalMirrorClose();
		abort();
	}

	if (completedValue >= m_timelineValue) {

		// Same rule: never silent-abort in Release.
		FatalMirrorOpen();
		FatalPrintf(
			"[Vulkan FATAL] timeline ahead of Submitter: "
			"completed=%llu next=%llu (qfi=%u) — aborting\n",
			(unsigned long long)completedValue,
			(unsigned long long)m_timelineValue,
			m_queueFamilyIndex);
		FatalFlush();
		FatalMirrorClose();

		abort();
	}

	PoolKey key{ device_, m_queueFamilyIndex, this };
	std::vector<CommandBufferPool*> pools;
	{
		std::lock_guard<std::mutex> g(g_allPoolsMutex);
		auto it = g_allPools.find(key);
		if (it != g_allPools.end()) pools = it->second;
	}
	if (pools.empty()) {
		auto* single = getPool();
		if (single) pools.push_back(single);
	}
	for (auto* pool : pools) {
		for (uint32_t i = 0; i < CommandBufferPool::kMax; ++i) {
			WrapperSlot& slot = pool->slots[i];
			if (slot.state_ != WrapperSlot::CommandState::Submitted) continue;
			if (slot.retireValue_ <= completedValue) {
				recycleSlot(pool, i);
			}
		}
	}
}
inline void smvk::Submitter::wait(CommandSubmitHandle handle, uint64_t timeout)
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	if (handle.pool == nullptr || handle.index_ >= CommandBufferPool::kMax)
		return;
	CommandBufferPool* pool = handle.pool;
	WrapperSlot& slot = pool->slots[handle.index_];
	// Already recycled: the batch this handle refers to has completed (a slot
	// only recycles once its retire value has been reached), so there is
	// nothing to wait for. This also closes the isReady()->wait() window —
	// purge() recycles retired slots from ANY thread, so the slot can be
	// recycled out from under this caller between its isReady() check and
	// this lock; without the generation early-out that races into the
	// assert below (observed under RenderDoc timing during upload storms).
	if (handle.gen_ != slot.gen_)
		return;
	assert(slot.state_ == WrapperSlot::CommandState::Submitted && "Cannot wait on command buffer before submit");

	VkResult result = waitTimeline(slot.retireValue_, timeout);
	if (result == VK_TIMEOUT) { assert(false && "timeout reached"); return; }
	if (result == VK_ERROR_DEVICE_LOST) {
		fprintf(stderr, "[Vulkan Warning] waitTimeline returned DEVICE_LOST in Submitter::wait, treating as complete\n");
		slot.cmd_ = VK_NULL_HANDLE;
		slot.state_ = WrapperSlot::CommandState::Free;
		slot.retireValue_ = 0;
		++slot.gen_; if (slot.gen_ == 0) slot.gen_ = 1;
		++pool->availableSlots;
		return;
	}

	VK_CHECK(result);

	recycleSlot(pool,
		handle.index_);
}
void smvk::Submitter::waitAll()
{
	std::lock_guard<std::recursive_mutex> lock(m_mutex);
	if (m_lastTimelineValue == 0)
		return;

	VkResult r = waitTimeline(m_lastTimelineValue, UINT64_MAX);
	if (r == VK_ERROR_DEVICE_LOST) {
		fprintf(stderr, "[Vulkan Warning] waitTimeline returned DEVICE_LOST in Submitter::waitAll, proceeding with teardown\n");
		PoolKey key{ device_, m_queueFamilyIndex, this };
		std::vector<CommandBufferPool*> pools;
		{
			std::lock_guard<std::mutex> g(g_allPoolsMutex);
			auto it = g_allPools.find(key);
			if (it != g_allPools.end()) pools = it->second;
		}
		if (pools.empty()) {
			auto* single = getPool();
			if (single) pools.push_back(single);
		}
		for (auto* pool2 : pools) {
			for (uint32_t i = 0; i < CommandBufferPool::kMax; ++i) {
				WrapperSlot& slot = pool2->slots[i];
				if (slot.state_ == WrapperSlot::CommandState::Submitted) {
					slot.cmd_ = VK_NULL_HANDLE;
					slot.state_ = WrapperSlot::CommandState::Free;
					slot.retireValue_ = 0;
					++slot.gen_; if (slot.gen_ == 0) slot.gen_ = 1;
					++pool2->availableSlots;
				}
			}
		}
		return;
	}
	VK_CHECK(r);

	purge();
}
void smvk::Submitter::recycleSlot(uint32_t index)
{
	assert(index < CommandBufferPool::kMax);
	auto* pool = getPool();
	recycleSlot(pool, index);
}
void smvk::Submitter::recycleSlot(
	CommandBufferPool* pool,
	uint32_t index)
{
	assert(pool);
	assert(index < CommandBufferPool::kMax);

	WrapperSlot& slot = pool->slots[index];

	assert(slot.state_ == WrapperSlot::CommandState::Submitted);
	assert(slot.cmd_ != VK_NULL_HANDLE);

	VK_CHECK(vkResetCommandBuffer(slot.cmd_, 0));

	slot.cmd_ = VK_NULL_HANDLE;
	slot.state_ = WrapperSlot::CommandState::Free;
	slot.retireValue_ = 0;

	++slot.gen_;
	if (slot.gen_ == 0)
		slot.gen_ = 1;

	++pool->availableSlots;
}

// VulkanSwapchain
smvk::VulkanSwapchain::VulkanSwapchain(VulkanResourceManager& context, SDL_Window* window/*, uint32_t framesInFlight, uint32_t desiredImageCount*/)
	: ctx_(context)
	, window_(window)
{

	depthFormat = fromVkFormat(
		findDepthFormat()
	);
	Create();

	//FRAME COMMAND 
	//We do pool per frame, because reset pool is faster than reseting indiviual commandBuffers.
	//Also frame is a good lifetime unit/boundary of course!
	QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
	InitFrameCommands(indices.graphicsFamily.value());
	CreateTimestampPools();
}

void smvk::VulkanSwapchain::InitFrameCommands(uint32_t graphicsFamily) {
	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolInfo.queueFamilyIndex = graphicsFamily;
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		VkCommandPoolCreateInfo perPool = poolInfo;
		VK_CHECK(vkCreateCommandPool(
			device_,
			&perPool,
			nullptr,
			&perFrameCommandPools[i]
		));

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = perFrameCommandPools[i];
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffers[i]));
	}
}
smvk::VulkanSwapchain::~VulkanSwapchain() {

	vkDeviceWaitIdle(device_);
	DestroyTimestampPools();
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		vkDestroyCommandPool(device_, perFrameCommandPools[i], nullptr);
		// this freeds all the commands too
		if (splitPools[i] != VK_NULL_HANDLE) { vkDestroyCommandPool(device_, splitPools[i], nullptr); splitPools[i] = VK_NULL_HANDLE; }
		if (asyncComputePools[i] != VK_NULL_HANDLE) { vkDestroyCommandPool(device_, asyncComputePools[i], nullptr); asyncComputePools[i] = VK_NULL_HANDLE; }
		if (asyncG2C_[i] != VK_NULL_HANDLE) { vkDestroySemaphore(device_, asyncG2C_[i], nullptr); asyncG2C_[i] = VK_NULL_HANDLE; }
		if (asyncC2G_[i] != VK_NULL_HANDLE) { vkDestroySemaphore(device_, asyncC2G_[i], nullptr); asyncC2G_[i] = VK_NULL_HANDLE; }
		splitBuffers[i] = VK_NULL_HANDLE;
		asyncComputeBuffers[i] = VK_NULL_HANDLE;
		asyncPassBuffers[i].clear(); // handles freed with the pool above
		asyncSplitReady_[i] = false;
	};

	DestroySyncObjects();
	CleanupSwapchain(true);
}
void smvk::VulkanSwapchain::WaitForNonMinimizedExtent() {
	// Two minimized signatures must both gate here:
	// - platforms that zero the client size while minimized (0x0 extent,
	//   the original gate)
	// - Windows, where SDL keeps reporting the RESTORED window size while
	//   minimized (GetClientRect stays at the pre-minimize size), so the
	//   0x0 check never trips and acquire/present keep failing with
	//   OUT_OF_DATE/SUBOPTIMAL - without the SDL_WINDOW_MINIMIZED flag
	//   check, Recreate() would tear down and rebuild the whole swapchain
	//   every single frame of the minimize (the "Recreate #N" spam).
	SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
	int width = 0;
	int height = 0;
	SDL_GetWindowSizeInPixels(window_, &width, &height);
	while ((flags & SDL_WINDOW_MINIMIZED) != 0 || width == 0 || height == 0) {
		// Pump + polite sleep until restore. NOT SDL_WaitEvent(NULL):
		// measured against this exact SDL build, the NULL-event wait
		// does NOT pump the OS queue - a cross-thread ShowWindow(SW_MINIMIZE)
		// blocked 11 seconds and no window flag ever flipped, i.e. exactly
		// the "hangs, does not respond on deminimize" wedge (the sent
		// WM_WINDOWPOSCHANGING never gets processed without a pump, so
		// the window cannot even complete its own restore). SDL_PumpEvents()
		// processes OS messages without consuming SDL events - the app's
		// own poll loop still sees them later.
		SDL_PumpEvents();
		SDL_Delay(50);
		flags = SDL_GetWindowFlags(window_);
		SDL_GetWindowSizeInPixels(window_, &width, &height);
	}
}

void smvk::VulkanSwapchain::Recreate() {
	// No new sync objects. Should investigate, can iamge# change? on OOD
	static uint64_t recreateCount = 0;
	fprintf(stderr, "[swapchain] Recreate #%llu\n", (unsigned long long)++recreateCount);
	WaitForNonMinimizedExtent();

	int width = 0;
	int height = 0;
	SDL_GetWindowSizeInPixels(window_, &width, &height);
	SDL_GetWindowSize(window_, &windowWidth, &windowHeight);

	extent_.width = width;
	extent_.height = height;

	vkDeviceWaitIdle(device_);
	// Idle => every retire point is satisfied; run queued destroys now so no
	// task can outlive objects Recreate is about to touch. (The graphics
	// timeline semaphore itself survives Recreate, but flushing here keeps the
	// invariant "no pending retire across Recreate" regardless.)
	ctx_.FlushDeferredTasks();

	CleanupSwapchain(false);

	CreateSwapchain();
	CreateImageViews();
	// Per-image sync objects: rebuild when the swapchain's image count
	// changed. Every OLD semaphore is destroyed BEFORE the vector is
	// touched (resize() must never be the destroy path: truncation would
	// leak handles, growth would leave null slots). CreateSwapchain no
	// longer resizes this vector for exactly that reason - the old code's
	// size check always matched (the resize had already run), so the
	// rebuild fired only when imageTimelineValues_ happened to mismatch
	// (an accidental policy, not a lifecycle).
	if (renderFinishedSemaphores_.size() != images_.size()
		|| imageTimelineValues_.size() != images_.size()) {
		for (VkSemaphore s : renderFinishedSemaphores_) {
			if (s != VK_NULL_HANDLE)
				vkDestroySemaphore(device_, s, nullptr);
		}
		renderFinishedSemaphores_.clear();
		renderFinishedSemaphores_.resize(images_.size(), VK_NULL_HANDLE);
		VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		for (size_t i = 0; i < images_.size(); ++i) {
			if (vkCreateSemaphore(device_, &info, nullptr,
				&renderFinishedSemaphores_[i]) != VK_SUCCESS) {
				throw std::runtime_error("Failed to recreate render-finished semaphore.");
			}
		}
		// Fresh semaphores => fresh timeline payload values.
		imageTimelineValues_.assign(images_.size(), 0);
	}
	// Per-frame sync objects: rebuild when the frames-in-flight count
	// changed (Recreate already vkDeviceWaitIdle'd + flushed the deferred
	// queue, so nothing is in flight - safe to destroy). The old path
	// CLEARED inFlightFences_ without destroying: a VkFence leak on every
	// frames-in-flight change.
	if (frameTimelineValues_.size() != framesInFlight_
		|| imageAvailableSemaphores_.size() != framesInFlight_
		|| inFlightFences_.size() != framesInFlight_) {
		frameTimelineValues_.assign(framesInFlight_, 0);
		for (VkSemaphore s : imageAvailableSemaphores_) {
			if (s != VK_NULL_HANDLE)
				vkDestroySemaphore(device_, s, nullptr);
		}
		imageAvailableSemaphores_.clear();
		for (VkFence f : inFlightFences_) {
			if (f != VK_NULL_HANDLE)
				vkDestroyFence(device_, f, nullptr);
		}
		inFlightFences_.clear();
		VkSemaphoreCreateInfo sinfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkFenceCreateInfo finfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
		imageAvailableSemaphores_.resize(framesInFlight_, VK_NULL_HANDLE);
		inFlightFences_.resize(framesInFlight_, VK_NULL_HANDLE);
		for (size_t i = 0; i < framesInFlight_; ++i) {
			if (vkCreateSemaphore(device_, &sinfo, nullptr,
				&imageAvailableSemaphores_[i]) != VK_SUCCESS
				|| vkCreateFence(device_, &finfo, nullptr,
					&inFlightFences_[i]) != VK_SUCCESS) {
				throw std::runtime_error("Failed to recreate per-frame sync objects.");
			}
		}
	}
	CreateDepthResources();

	currentImageIndex_ = 0;
}
void smvk::VulkanSwapchain::CreateSwapchain() {

	SwapChainSupportDetails support = querySwapChainSupport(physicalDevice_, surface_);
	VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
	VkExtent2D swapExtent = chooseSwapExtent(support.capabilities, window_);
	// Requested mode wins when supported; otherwise fall back without the
	// strict-FIFO 30fps lock first: mailbox (tear-free, no halving), then
	// relaxed (vsync on time, immediate next blank when late), then
	// immediate, and only then spec-guaranteed FIFO.
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
	{
		const auto& modes = support.presentModes;
		auto has = [&](VkPresentModeKHR m) {
			return std::find(modes.begin(), modes.end(), m) != modes.end();
		};
		if (has(requestedPresentMode_))
			presentMode = requestedPresentMode_;
		else if (has(VK_PRESENT_MODE_MAILBOX_KHR))
			presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
		else if (has(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
			presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
		else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
			presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	}
	activePresentMode_ = presentMode;

	uint32_t imageCount = DESIRED_SWAPCHAIN_IMAGE_COUNT;
	minImageCount_ = support.capabilities.minImageCount;
	if (imageCount < support.capabilities.minImageCount) {
		imageCount = support.capabilities.minImageCount;
	}

	if (support.capabilities.maxImageCount > 0 &&
		imageCount > support.capabilities.maxImageCount) {
		imageCount = support.capabilities.maxImageCount;
	}
	framesInFlight_ = imageCount + 2u;
	framesInFlight_ = std::min(MAX_FRAMES_IN_FLIGHT, framesInFlight_);

	QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
	uint32_t queueFamilyIndices[] = {
		indices.graphicsFamily.value(),
		indices.presentFamily.value()
	};

	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = surface_;
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = swapExtent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	createInfo.preTransform = support.capabilities.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	if (indices.graphicsFamily != indices.presentFamily) {
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilyIndices;
	}
	else {
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}
	VkResult result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &vk_swapchain_);
	if (result != VK_SUCCESS) {
		throw std::runtime_error("Failed to create Vulkan swapchain.");
	}

	uint32_t actualImageCount = 0;


	vkGetSwapchainImagesKHR(device_, vk_swapchain_, &actualImageCount, nullptr);
	images_.resize(actualImageCount);
	imageViews_.resize(actualImageCount);
	// (No sync-object resize here - renderFinishedSemaphores_ is owned by
	// the sync-object lifecycle: CreateSyncObjects at init, the count-
	// change block in Recreate(). Resizing here made the recreate check
	// size-match ALWAYS (null slots left live on growth) and leaked the
	// truncated-away handles on shrink.)
	vkGetSwapchainImagesKHR(device_, vk_swapchain_, &actualImageCount, images_.data());


	this->surfaceFormat = fromVkFormat(surfaceFormat.format);
	this->extent_ = swapExtent;

	SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
}
std::vector<VkPresentModeKHR> smvk::VulkanSwapchain::GetAvailablePresentModes() const {
	return querySwapChainSupport(physicalDevice_, surface_).presentModes;
}
void smvk::VulkanSwapchain::CreateTimestampPools() {
	DestroyTimestampPools();
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(physicalDevice_, &props);
	timestampPeriod_ = props.limits.timestampPeriod;
	// Stage timers need timestamps on ALL queues (the async blur stream
	// writes from the compute queue) and the wider pools.
	stageTimersSupported_ = (props.limits.timestampComputeAndGraphics == VK_TRUE)
		&& (timestampPeriod_ > 0.0f);
	VkQueryPoolCreateInfo info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	info.queryCount = 2 + 2 * kGpuStageCount;
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		if (vkCreateQueryPool(device_, &info, nullptr, &timestampPools_[i]) != VK_SUCCESS)
			timestampPools_[i] = VK_NULL_HANDLE;
		timestampWritten_[i] = false;
	}
	gpuTimingValid_ = false;
	lastGpuMs_ = 0.0;
	for (uint32_t s = 0; s < kGpuStageCount; ++s) {
		stageTimingValid_[s] = false;
		lastStageGpuMs_[s] = 0.0;
	}
}
void smvk::VulkanSwapchain::DestroyTimestampPools() {
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		if (timestampPools_[i] != VK_NULL_HANDLE) {
			vkDestroyQueryPool(device_, timestampPools_[i], nullptr);
			timestampPools_[i] = VK_NULL_HANDLE;
		}
		timestampWritten_[i] = false;
	}
	gpuTimingValid_ = false;
}
void smvk::VulkanSwapchain::CreateImageViews() {
	const uint32_t count = static_cast<uint32_t>(images_.size());

	// Keep ImageViewHandles stable across recreation so external owners (the
	// rendergraph) never see a stale handle. Trim handles beyond the new
	// image count; the underlying VkImageView is swapped in place otherwise.
	if (imageViewHandles_.size() > count) {
		for (uint32_t i = count; i < imageViewHandles_.size(); ++i) {
			if (!imageViewHandles_[i].empty())
				destroy(&ctx_, imageViewHandles_[i]);
		}
		imageViewHandles_.resize(count);
		imageViews_.resize(count);
	}

	for (uint32_t i = 0; i < count; ++i) {
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = images_[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = toVkFormat(surfaceFormat);
		viewInfo.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1
		};

		VkImageView newView = VK_NULL_HANDLE;
		VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &newView));

		if (i < imageViewHandles_.size() && !imageViewHandles_[i].empty()) {
			// Previous raw view already destroyed in CleanupSwapchain; just
			// point the stable handle at the new VkImageView.
			imageViews_[i] = newView;
			ctx_.SetImageViewVkView(imageViewHandles_[i], newView);
		}
		else {
			imageViews_[i] = newView;
			imageViewHandles_.push_back(
				ctx_.RegisterImageView(
					newView,
					surfaceFormat,
					"swapchain view"
				)
			);
		}
	}
}
void smvk::VulkanSwapchain::CreateDepthResources() {

	assert(depthFormat != smvk::Format::Undefined && "core depthFormat not set. failed to create swapchain depth Image");

	ImageCreateInfo depthImageCreateInfo{};
	depthImageCreateInfo.extent = { extent_.width, extent_.height };
	depthImageCreateInfo.format = depthFormat;
	// Sampled bit is required for Forward+ tile culling (compute reads
	// depth as a bindless texture for per-tile min/max).
	depthImageCreateInfo.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled;
	depthImageCreateInfo.mipLevels = 1;
	depthImageCreateInfo.arrayLayers = 1;
	depthImageCreateInfo.name = "swapchain depth image";

	// Recreate in place so the ImageViewHandle stays stable across resize and
	// any rendergraph views referencing it remain valid.
	if (depthImage_.empty()) {
		depthImage_ = ctx_.CreateImage(
			depthImageCreateInfo,
			nullptr
		);
	}
	else {
		ctx_.RecreateImage(depthImage_, depthImageCreateInfo, nullptr);
	}

	auto* depthImage = ctx_.images_.get(depthImage_);
	assert(depthImage != nullptr);

	const ImageViewCreateInfo depthViewCI{
		.viewType = ImageViewType::View2D,
		.format = depthFormat,
		.subresourceRange = {
			.aspect = ImageAspect::Depth,
			.baseMipLevel = 0,
			.mipLevelCount = 1,
			.baseArrayLayer = 0,
			.arrayLayerCount = 1
		}
	};

	if (depthImageView_.empty()) {
		depthImageView_ = ctx_.CreateImageView(depthImage_, depthViewCI);
	}
	else {
		ctx_.RecreateImageView(depthImageView_, depthImage_, depthViewCI);
	}

	// maybe toVkImageSubresourceRange
	const VkImageSubresourceRange depthRange{
		.aspectMask = VkAspectMaskFromFormat(toVkFormat(depthImage->format)),
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};

	// Transition the image into the layout expected by depth attachment rendering.
	auto handle = ctx_.m_submitter->acquire();
	auto cmd = ctx_.m_submitter->GetCommandBuffer(handle);

	ImageBarrier(
		cmd,
		depthImage->vkImage,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		depthRange,
		VK_PIPELINE_STAGE_2_NONE,
		VK_ACCESS_2_NONE,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
		VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
		VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
	);

	ctx_.m_submitter->submit(handle);
}
// (No `inline`: single out-of-line definition - an inline member
// definition in one TU is only emitted where ODR-used, leaving other TUs'
// calls unresolved on GCC/Clang; MSVC happened to emit it.)
void smvk::VulkanSwapchain::CreateSyncObjects() {
	renderFinishedSemaphores_.resize(images_.size(), VK_NULL_HANDLE);
	imageAvailableSemaphores_.resize(framesInFlight_, VK_NULL_HANDLE);
	inFlightFences_.resize(framesInFlight_, VK_NULL_HANDLE);
	frameTimelineValues_.assign(framesInFlight_, 0);
	imageTimelineValues_.assign(images_.size(), 0);

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for (size_t i = 0; i < images_.size(); ++i) {
		if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create render-finished semaphore.");
		}
	}

	for (size_t i = 0; i < framesInFlight_; ++i) {
		if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
			vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create per-frame sync objects.");
		}
	}
	// Timeline for in-flight frames (hybrid: 1 binary for acquire + timeline for inFlight)
	if (timelineSemaphore_ == VK_NULL_HANDLE) {
		VkSemaphoreTypeCreateInfo typeInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
		VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &typeInfo, 0 };
		VK_CHECK(vkCreateSemaphore(device_, &info, nullptr, &timelineSemaphore_));
		timelineValue_ = 0;
	}
}
inline void smvk::VulkanSwapchain::DestroySyncObjects() {

	for (VkSemaphore semaphore : renderFinishedSemaphores_) {
		if (semaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(device_, semaphore, nullptr);
		}
	}
	renderFinishedSemaphores_.clear();

	for (VkSemaphore semaphore : imageAvailableSemaphores_) {
		if (semaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(device_, semaphore, nullptr);
		}
	}
	imageAvailableSemaphores_.clear();

	for (VkFence fence : inFlightFences_) {
		if (fence != VK_NULL_HANDLE) {
			vkDestroyFence(device_, fence, nullptr);
		}
	}
	inFlightFences_.clear();
	frameTimelineValues_.clear();
	imageTimelineValues_.clear();
	if (timelineSemaphore_ != VK_NULL_HANDLE) {
		vkDestroySemaphore(device_, timelineSemaphore_, nullptr);
		timelineSemaphore_ = VK_NULL_HANDLE;
		timelineValue_ = 0;
		// The stored retirement point dies with the semaphore. Null it so
		// later destroys run immediately instead of keying tasks on a dead
		// semaphore (only called from ~VulkanSwapchain, which waited idle).
		ctx_.NotifyGraphicsSubmit(VK_NULL_HANDLE, 0);
	}
}
inline void smvk::VulkanSwapchain::CleanupSwapchain(bool fullDestroy) {

	for (VkImageView view : imageViews_) {
		if (view != VK_NULL_HANDLE) {
			vkDestroyImageView(device_, view, nullptr);
		}
	}

	if (fullDestroy) {
		for (ImageViewHandle handle : imageViewHandles_) {
			if (!handle.empty())
				destroy(&ctx_, handle);
		}
		imageViewHandles_.clear();
	}

	imageViews_.clear();
	images_.clear();

	if (vk_swapchain_ != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(device_, vk_swapchain_, nullptr);
		vk_swapchain_ = VK_NULL_HANDLE;
	}
}
namespace {
	const char* DeviceFaultAddressTypeName(VkDeviceFaultAddressTypeEXT type) {
		switch (type) {
		case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "NONE";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "READ_INVALID";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "WRITE_INVALID";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "EXECUTE_INVALID";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "IP_UNKNOWN";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "IP_INVALID";
		case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "IP_FAULT";
		default: return "???";
		}
	}
	// Dumps VK_EXT_device_fault info for a lost device. Must run before the
	// device is destroyed (fault state dies with it); safe to call when the
	// extension is absent (no-op). Called from the acquire-loss path below.
	// FatalPrintf routes every line to stderr AND the tourmaline_fatal.log
	// mirror (GUI runs have no visible console — stderr alone is blind).
	void DumpDeviceFaultInfo() {
		FatalPrintf("[DeviceFault] enter (supported=%d device=%p fn=%p)\n",
			g_deviceFaultSupported ? 1 : 0, (void*)device_,
#ifdef VK_EXT_device_fault
			(void*)vkGetDeviceFaultInfoEXT);
#else
			(void*)nullptr);
#endif
		FatalFlush();
		if (!g_deviceFaultSupported || device_ == VK_NULL_HANDLE)
			return;
#ifdef VK_EXT_device_fault
		if (!vkGetDeviceFaultInfoEXT)
			return;
		// Single-shot query sized by maxDeviceFaultCount (physical-device
		// property, safe on a lost device): avoids the counts-only call with
		// pFaultInfo=null, which AVs on this driver.
		VkPhysicalDeviceFaultPropertiesKHR faultProps{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_PROPERTIES_KHR, nullptr };
		{
			VkPhysicalDeviceProperties2 props2{
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &faultProps };
			vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);
		}
		const uint32_t maxInfos = faultProps.maxDeviceFaultCount != 0
			? faultProps.maxDeviceFaultCount : 8u;
		FatalPrintf("[DeviceFault] maxDeviceFaultCount=%u\n", maxInfos);
		FatalFlush();
		VkDeviceFaultCountsEXT counts{
			VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT, nullptr };
		std::vector<VkDeviceFaultAddressInfoEXT> addrInfos(maxInfos);
		std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(maxInfos);
		VkDeviceFaultInfoEXT info{
			VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, nullptr };
		info.pAddressInfos = addrInfos.data();
		info.pVendorInfos = vendorInfos.data();
		info.pVendorBinaryData = nullptr; // opaque driver blob, not fetched
		VkResult r = vkGetDeviceFaultInfoEXT(device_, &counts, &info);
		// VK_INCOMPLETE (positive) is expected: pVendorBinaryData is null, so
		// the vendor binary portion is "too small" — the description + address
		// + vendor infos are still filled. Only negative results are failures.
		if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
			FatalPrintf("[DeviceFault] detail query failed (VkResult %d)\n", (int)r);
			FatalFlush();
			return;
		}
		const uint32_t nAddr = counts.addressInfoCount < maxInfos
			? counts.addressInfoCount : maxInfos;
		const uint32_t nVendor = counts.vendorInfoCount < maxInfos
			? counts.vendorInfoCount : maxInfos;
		FatalPrintf("[DeviceFault] desc='%s' %u address infos, %u vendor infos:",
			info.description, nAddr, nVendor);
		for (uint32_t i = 0; i < nAddr; ++i) {
			FatalPrintf("[DeviceFault]   addr[%u] type=%s reported=0x%llx precision=0x%llx\n",
				i, DeviceFaultAddressTypeName(addrInfos[i].addressType),
				(unsigned long long)addrInfos[i].reportedAddress,
				(unsigned long long)addrInfos[i].addressPrecision);
		}
		for (uint32_t i = 0; i < nVendor; ++i) {
			FatalPrintf("[DeviceFault]   vendor[%u] desc='%s' faultCode=0x%llx faultData=0x%llx\n",
				i, vendorInfos[i].description,
				(unsigned long long)vendorInfos[i].vendorFaultCode,
				(unsigned long long)vendorInfos[i].vendorFaultData);
		}
		FatalFlush();
#else
		(void)0;
#endif
		// ---- Checkpoint pinpoint (VK_NV_device_diagnostic_checkpoints) ----
		// The last-reached pass markers per queue — THE answer to "which pass
		// was executing at the fault". Valid on a lost device (the extension
		// exists for exactly this query). Markers outside the registry ring
		// (a pre-init fault) print raw.
		if (g_fnVkGetQueueCheckpointDataNV) {
			struct Q { VkQueue q; const char* name; };
			const Q queues[] = {
				{ graphicsQueue_, "graphics" },
				{ computeQueue_, "compute" },
				{ transferQueue_, "transfer" },
			};
			const CheckpointMarker* ringBegin = g_checkpointRing;
			const CheckpointMarker* ringEnd = g_checkpointRing + kCheckpointRing;
			for (const Q& q : queues) {
				if (q.q == VK_NULL_HANDLE)
					continue;
				uint32_t cpCount = 0;
				g_fnVkGetQueueCheckpointDataNV(q.q, &cpCount, nullptr);
				if (cpCount == 0)
					continue;
				std::vector<VkCheckpointDataNV> cps(cpCount,
					VkCheckpointDataNV{ VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV });
				g_fnVkGetQueueCheckpointDataNV(q.q, &cpCount, cps.data());
				FatalPrintf("[DeviceFault] %s queue: %u last checkpoints (NEWEST LAST):\n",
					q.name, cpCount);
				for (uint32_t c = 0; c < cpCount; ++c) {
					const void* marker = cps[c].pCheckpointMarker;
					if (marker >= ringBegin && marker < ringEnd) {
						const CheckpointMarker& m =
							*static_cast<const CheckpointMarker*>(marker);
						FatalPrintf("[DeviceFault]   stage=%llu frame=%u sortedPos=%u pass='%s'\n",
							(unsigned long long)cps[c].stage,
							m.frame, m.pos, m.passName);
					}
					else {
						FatalPrintf("[DeviceFault]   stage=%llu marker=%p (outside registry - pre-init?)\n",
							(unsigned long long)cps[c].stage, marker);
					}
				}
				FatalFlush();
			}
		}
	}
} // namespace
smvk::VulkanFrameBuffer smvk::VulkanSwapchain::AcquireFramebuffer()
{
	SMVK_PROFILE_SCOPE_COLOR("AcquireFramebuffer", Optick::Color::SteelBlue);
	// Frame-boundary host sync: FENCE wait, not a timeline-semaphore host
	// wait. History: this used vkWaitSemaphores on timelineSemaphore_ with
	// frameTimelineValues_[currentFrame_] (the previous frame's retirement
	// value). On this driver that FIRST host wait after a present poisons
	// the device: every subsequent vkGetSemaphoreCounterValue returns
	// SUCCESS with UINT64_MAX (then VK_ERROR_INITIALIZATION_FAILED, and
	// vkGetDeviceFaultInfoEXT returns garbage error codes) — the silent
	// abort in Submitter::purge() that killed every headless bench at
	// frame 1 for weeks (0xC0000409 in ucrtbase, misdiagnosed as a crash:
	// it was our own abort on the poisoned counter read). Fresh-semaphore
	// selftests showed the plain wait is fine; the poison needs the
	// swapchain's signaled-across-a-present timeline specifically. The
	// classic fence pattern sidesteps the host timeline-wait entirely:
	// the tail submit signals inFlightFences_[currentFrame_] (see the
	// three vkQueueSubmit2 call sites in EndAndPresentFramebuffer), this
	// wait retires exactly that submit — which already waited the async
	// split (C2G) and the upload chain, so G1/C/G2 + uploads all retire
	// with it. The swapchain timeline stays for DEVICE-side waits only
	// (upload-batch chaining, deferred-destroy retirement via submits);
	// the host never waits it again. The fences are created signaled
	// (CreateSyncObjects), so the first frame passes instantly.
	{
		VkResult waitResult = vkWaitForFences(
			device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);
		if (waitResult != VK_SUCCESS)
		{
			FatalMirrorOpen();
			FatalPrintf(
				"vkWaitForFences failed: VkResult=%d frame=%u\n",
				(int)waitResult, currentFrame_);
			FatalFlush();
			// Device loss post-mortem while the device still exists.
			DumpDeviceFaultInfo();
			FatalMirrorClose();

			throw std::runtime_error("vkWaitForFences failed");
		}
		// NOTE: the fence RESET moved below the acquire, on purpose. The
		// acquire-OUT_OF_DATE path below recurses into AcquireFramebuffer
		// after Recreate(); if this pass had reset the fence BEFORE the
		// acquire, the retry would vkWaitForFences(UINT64_MAX) on the very
		// fence this call just unsignaled — no submit can re-signal it (the
		// frame that would is stuck in this wait) and the app wedges
		// forever ("hangs, does not respond on deminimize": any acquire
		// OUT_OF_DATE - minimize landing between frames, some resizes -
		// deadlocks deterministically). With the reset after a successful
		// acquire, the retry's wait passes on the still-signaled fence and
		// resets only once the acquire actually succeeded.
	}

	// Frame retired => its start/end timestamps are ready. Pull the GPU
	// time for the CPU/GPU split overlay (non-blocking; the timeline wait
	// above already guarantees completion, first frames just have no data).
	if (timestampWritten_[currentFrame_] && timestampPools_[currentFrame_] != VK_NULL_HANDLE) {
		uint64_t ticks[2] = { 0, 0 };
		VkResult qr = vkGetQueryPoolResults(device_, timestampPools_[currentFrame_],
			0, 2, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
		if (qr == VK_SUCCESS && timestampPeriod_ > 0.0f && ticks[1] > ticks[0]) {
			lastGpuMs_ = double(ticks[1] - ticks[0]) * double(timestampPeriod_) / 1e6;
			gpuTimingValid_ = true;
		}
		// Stage pairs (shadow-path instrumentation, see GpuStageTimer):
		// per-pair pulls — a stage that never ran this frame leaves its
		// pair unwritten (reset but never written = NOT_READY), and the
		// previous frame's value survives. The async-stream pair was
		// written on the COMPUTE queue; the present submit's C2G wait
		// guarantees those completed with the frame.
		for (uint32_t s = 0; s < kGpuStageCount; ++s) {
			if (!stageTimersSupported_)
				break;
			uint64_t st[2] = { 0, 0 };
			VkResult sr = vkGetQueryPoolResults(device_, timestampPools_[currentFrame_],
				2 + 2 * s, 2, sizeof(st), st, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
			if (sr == VK_SUCCESS && st[1] > st[0]) {
				lastStageGpuMs_[s] = double(st[1] - st[0]) * double(timestampPeriod_) / 1e6;
				stageTimingValid_[s] = true;
			}
		}
	}

	uint32_t imageIndex;
	VkResult acquireResult = VK_SUCCESS;
	{
		acquireResult = vkAcquireNextImageKHR(
			device_,
			vk_swapchain_,
			UINT64_MAX,
			imageAvailableSemaphores_[currentFrame_],
			VK_NULL_HANDLE,
			&imageIndex
		);

		if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
			Recreate();
			return AcquireFramebuffer();
		}
		else if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
			throw std::runtime_error("failed to acquire swap chain image!");
		}
	}

	// Acquire succeeded => this frame's submit WILL run and re-signal this
	// slot's fence; safe to unsignal it here (see the NOTE above the fence
	// wait for why this must not happen before a successful acquire).
	VK_CHECK(vkResetFences(device_, 1, &inFlightFences_[currentFrame_]));

	SetCurrentImageIndex(imageIndex);

	// Engine globals: the fence wait above guarantees this frame's slice
	// retired, so it can be rewritten before any pass records. The
	// update-after-bind descriptor rewrite is visible to commands recorded
	// from now on; previously recorded frames keep their own slice.
	ctx_.FlushEngineGlobals(currentFrame_);

	vkResetCommandPool(device_, perFrameCommandPools[currentFrame_], 0);
	// Async-split pools retire with the slot (the fence wait above covers
	// the tail submit, which waited the compute timeline — so C is done).
	if (asyncSplitReady_[currentFrame_]) {
		if (splitPools[currentFrame_] != VK_NULL_HANDLE)
			vkResetCommandPool(device_, splitPools[currentFrame_], 0);
		if (asyncComputePools[currentFrame_] != VK_NULL_HANDLE)
			vkResetCommandPool(device_, asyncComputePools[currentFrame_], 0);
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	if (vkBeginCommandBuffer(commandBuffers[currentFrame_], &beginInfo) != VK_SUCCESS) {
		throw std::runtime_error("failed to begin recording command buffer!");
	}

	VkImageMemoryBarrier2 barrierToColorAttachment{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	barrierToColorAttachment.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	barrierToColorAttachment.srcAccessMask = VK_ACCESS_2_NONE;
	barrierToColorAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	barrierToColorAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	barrierToColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrierToColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrierToColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrierToColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrierToColorAttachment.image = images_[imageIndex];
	barrierToColorAttachment.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &barrierToColorAttachment;
	vkCmdPipelineBarrier2(commandBuffers[currentFrame_], &dep);

	// GPU frame timer: whole command buffer (all rendergraph passes).
	if (timestampPools_[currentFrame_] != VK_NULL_HANDLE) {
		vkCmdResetQueryPool(commandBuffers[currentFrame_], timestampPools_[currentFrame_], 0,
			2 + 2 * kGpuStageCount);
		vkCmdWriteTimestamp(commandBuffers[currentFrame_], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			timestampPools_[currentFrame_], 0);
	}

	VulkanFrameBuffer frameBufferContext_{
		.frameIndex = currentFrame_,
		.imageIndex = currentImageIndex_,
		.commandBuffer = commandBuffers[currentFrame_],
		.swapchain = *this

	};
	return frameBufferContext_;
}
void smvk::VulkanSwapchain::EndAndPresentFramebuffer(VulkanFrameBuffer& fb)
{
	SMVK_PROFILE_SCOPE_COLOR("Present", Optick::Color::SteelBlue);
	SMVK_PROFILE_GPU_FLIP(fb.swapchain.vk_swapchain_); // GPU profiling for present
	// Async-split frames (G1/C/G2): G1 was submitted by Execute; the tail
	// (G2) records here. Single-submit frames use commandBuffer as before.
	VkCommandBuffer tailCmd = (fb.splitCommandBuffer != VK_NULL_HANDLE)
		? fb.splitCommandBuffer : fb.commandBuffer;
	ImageBarrier(
		tailCmd,
		GetCurrentImage(),
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_NONE,
		VK_ACCESS_2_NONE
		//ctx_.core_.indices.graphicsFamily.value(), //nasty
		//ctx_.core_.indices.presentFamily.value()
	);

	//
	if (timestampPools_[fb.frameIndex] != VK_NULL_HANDLE) {
		vkCmdWriteTimestamp(tailCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			timestampPools_[fb.frameIndex], 1);
		timestampWritten_[fb.frameIndex] = true;
	}
	if (vkEndCommandBuffer(tailCmd) != VK_SUCCESS) {
		throw std::runtime_error("failed to record command buffer!");
	}

	// Per-frame upload batch point: submit the pending streamer uploads
	// (seed/visible/indirect slices, exposure + DS zero-fills, engine
	// globals, ...) BEFORE the main command buffer. UploadDataToBuffer no
	// longer flushes per call, so this is the one submit per frame that
	// kicks the batch.
	// (Split frames flushed ahead of G1 inside Execute; this covers
	// anything recorded after — e.g. the ImGui overlay — and is a no-op
	// when nothing is pending.)
	ctx_.FlushUploads();
	// Uploaded data -> this submit must be a semaphore-ordered memory
	// dependency: per spec, submissions to one queue only respect
	// submission (begin) order and may overlap, so queue FIFO alone does
	// not order the copies ahead of the passes that consume them. The
	// newest transfer-batch timeline point covers every earlier batch on
	// that timeline (values on one Submitter timeline are monotonic).
	const TimelinePoint uploadTp =
		ctx_.GetStreamer() ? ctx_.GetStreamer()->lastTransferPoint()
		: TimelinePoint{};

	// Split frames: G2 waits the compute blur (C -> G2 timeline), G1's own
	// signal (G1 -> G2: the acquire wait + swapchain layout barrier live
	// in G1's CB, and G2 must be ordered after them explicitly — same
	// queue submissions may overlap, so "transitively through submission
	// order" is not a spec guarantee), and the upload batch. G1 already
	// waited the previous frame's timeline (cross-frame image reuse),
	// which G2 inherits through the G1 -> G2 wait. The binary
	// swapchain-acquire wait already ran on the G1 submit (a binary
	// semaphore can only be waited once).
	// Single-submit frames: acquire wait + upload batch + previous frame
	// (same cross-frame hazard: Main writes hdr/depth while the previous
	// frame's Tonemap/bloom may still be reading them).
	VkSemaphoreSubmitInfo waitInfos[4]{};
	uint32_t waitCount = 0;
	if (fb.splitCommandBuffer != VK_NULL_HANDLE && fb.splitWaitSemaphore != VK_NULL_HANDLE) {
		waitInfos[waitCount] = VkSemaphoreSubmitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, fb.splitWaitSemaphore, fb.splitWaitValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
		++waitCount;
		if (fb.splitWaitSemaphore2 != VK_NULL_HANDLE && fb.splitWaitValue2 != 0) {
			waitInfos[waitCount] = VkSemaphoreSubmitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, fb.splitWaitSemaphore2, fb.splitWaitValue2, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
			++waitCount;
		}
	}
	else {
		waitInfos[waitCount] = VkSemaphoreSubmitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, imageAvailableSemaphores_[currentFrame_], 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0 };
		++waitCount;
		if (timelineValue_ != 0) {
			waitInfos[waitCount] = VkSemaphoreSubmitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, timelineSemaphore_, timelineValue_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
			++waitCount;
		}
	}
	if (uploadTp.valid()) {
		waitInfos[waitCount] = VkSemaphoreSubmitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, uploadTp.semaphore, uploadTp.value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
		++waitCount;
	}
	if (RgTraceEnabled(fb.frameIndex) && waitCount)
		std::fprintf(stderr, "[RG] G2/main submitted (waits=%u%s, upload value=%llu)\n",
			waitCount,
			(fb.splitCommandBuffer == VK_NULL_HANDLE && timelineValue_ != 0) ? " incl.prevFrame" : "",
			static_cast<unsigned long long>(uploadTp.value));
	VkSemaphoreSubmitInfo signalInfos[2]{};
	signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signalInfos[0].semaphore = renderFinishedSemaphores_[currentImageIndex_];
	signalInfos[0].value = 0;
	signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	signalInfos[0].deviceIndex = 0;
	signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signalInfos[1].semaphore = timelineSemaphore_;
	// Frame-retirement value: recorded into the per-slot wait table only
	// AFTER the submit that signals it was accepted (both branches below).
	// Publishing before the submit would leave a slot waiting forever on a
	// value no submit ever signals if anything fails between the two points
	// (queue-mutex throw, future early-return, non-fatal VK_CHECK). Skipped
	// values are harmless: the next frame takes a fresh ++, and timeline
	// waits are strictly increasing.
	const uint64_t signalValue = ++timelineValue_;
	signalInfos[1].value = signalValue;
	signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	signalInfos[1].deviceIndex = 0;
	VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, tailCmd, 0 };
	VkSubmitInfo2 submitInfo2{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, waitCount, waitInfos, 1, &cmdInfo,
		2, signalInfos };
	// PCL latency markers (no-ops unless DlssBeginFrame opened a token and
	// the PCL plugin is loaded): CPU sim work ends here, the render
	// submit starts.
	DlssPclMarker(PclMarker::SimulationEnd);
	DlssPclMarker(PclMarker::RenderSubmitStart);
	{
		// Single-threaded: all queue ops on render thread, but use per-queue mutex for correctness when transferQueue is distinct
		std::mutex& mg = GetQueueMutex(graphicsQueue_);
		std::mutex& mp = GetQueueMutex(presentQueue_);
		std::unique_lock<std::mutex> lockG(mg, std::defer_lock);
		std::unique_lock<std::mutex> lockP(mp, std::defer_lock);
		if (&mg == &mp) {
			lockG.lock();
			VK_CHECK(vkQueueSubmit2(graphicsQueue_, 1, &submitInfo2, inFlightFences_[currentFrame_]));
			DlssPclMarker(PclMarker::RenderSubmitEnd);
			DlssPclMarker(PclMarker::PresentStart);
			// Submit accepted: the value it signals is now reachable, so the
			// slot's wait target can be published (AcquireFramebuffer waits
			// on frameTimelineValues_ with an infinite timeout).
			frameTimelineValues_[currentFrame_] = signalValue;
			imageTimelineValues_[currentImageIndex_] = signalValue;
			// Publish the retirement point for deferred destroys: any resource
			// freed from here on retires once this submit (and all before it)
			// has completed.
			ctx_.NotifyGraphicsSubmit(timelineSemaphore_, signalValue);
			// Drop the queue lock before present/Recreate: Recreate starts
			// with vkDeviceWaitIdle plus preservation submits that re-lock
			// this same mutex (swapchain depth copy) — holding it across
			// the call deadlocks with EDEADLK on present-driven recreates.
			lockG.unlock();
			// TOURMALINE_SERIALIZE=main (see RgSerializeGate): the whole
			// frame's tail waits queue idle before present.
			RgSerializeGate(RgSerializeMode().main, graphicsQueue_, mg);
			VkPresentInfoKHR presentInfo{};
			presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = &renderFinishedSemaphores_[currentImageIndex_];
			VkSwapchainKHR swapChains[] = { vk_swapchain_ };
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = swapChains;
			presentInfo.pImageIndices = &currentImageIndex_;
			// The present call itself holds the queue mutex:
			// vkQueuePresentKHR is not internally synchronized, and
			// Submitter::submit() (the upload streamer's acquire-barrier
			// submits) runs on other threads against this same queue.
			// Released immediately after the call, BEFORE Recreate — the
			// present-vs-Recreate unlock note above still applies.
			VkResult result;
			{
				std::lock_guard<std::mutex> presentLock(mg);
				// FIFO VSync throttles inside vkQueuePresentKHR (vertical-blank
				// wait). Time it so the caller can report it as wait, not CPU.
				const auto presentStart = std::chrono::steady_clock::now();
				result = vkQueuePresentKHR(presentQueue_, &presentInfo);
				lastPresentWaitMs_ = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - presentStart).count();
			}
			if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
				framebufferResized) {
				framebufferResized = false;
				Recreate();
			}
			else if (result != VK_SUCCESS) {
				throw std::runtime_error("failed to present swap chain image!");
			}
			DlssPclMarker(PclMarker::PresentEnd);
			AdvanceFrame();
			DlssEndFrame();
			return;
		}
		else {
			{
				std::lock_guard<std::mutex> lock(mg);
				VK_CHECK(vkQueueSubmit2(graphicsQueue_, 1, &submitInfo2, inFlightFences_[currentFrame_]));
				DlssPclMarker(PclMarker::RenderSubmitEnd);
				DlssPclMarker(PclMarker::PresentStart);
				// Same publish-on-success as the single-mutex path above.
				frameTimelineValues_[currentFrame_] = signalValue;
				imageTimelineValues_[currentImageIndex_] = signalValue;
				ctx_.NotifyGraphicsSubmit(timelineSemaphore_, signalValue);
			}
			// TOURMALINE_SERIALIZE=main (see RgSerializeGate): the whole
			// frame's tail waits queue idle before present (gate holds the
			// graphics queue mutex; the present below locks mp).
			RgSerializeGate(RgSerializeMode().main, graphicsQueue_, mg);
			{
				std::lock_guard<std::mutex> lock(mp);
				VkPresentInfoKHR presentInfo{};
				presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
				presentInfo.waitSemaphoreCount = 1;
				presentInfo.pWaitSemaphores = &renderFinishedSemaphores_[currentImageIndex_];
				VkSwapchainKHR swapChains[] = { vk_swapchain_ };
				presentInfo.swapchainCount = 1;
				presentInfo.pSwapchains = swapChains;
				presentInfo.pImageIndices = &currentImageIndex_;
				const auto presentStart = std::chrono::steady_clock::now();
				VkResult result = vkQueuePresentKHR(presentQueue_, &presentInfo);
				lastPresentWaitMs_ = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - presentStart).count();
				if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
					framebufferResized) {
					framebufferResized = false;
					Recreate();
				}
				else if (result != VK_SUCCESS) {
					throw std::runtime_error("failed to present swap chain image!");
				}
				DlssPclMarker(PclMarker::PresentEnd);
			}
			AdvanceFrame();
			DlssEndFrame();
			return;
		}
	}
	AdvanceFrame();

}
bool smvk::StencilSettings::operator<(const StencilSettings& other) const noexcept
{
	return std::tie(
		frontCompareOp, backCompareOp,
		frontFailOp, backFailOp,
		frontPassOp, backPassOp,
		frontFace, backFace,
		frontTestEnable, backTestEnable,
		frontWriteEnable, backWriteEnable) <
		std::tie(
			other.frontCompareOp, other.backCompareOp,
			other.frontFailOp, other.backFailOp,
			other.frontPassOp, other.backPassOp,
			other.frontFace, other.backFace,
			other.frontTestEnable, other.backTestEnable,
			other.frontWriteEnable, other.backWriteEnable);
}
//
std::vector<VkVertexInputBindingDescription>
smvk::getVkVertexInputBindingDescriptions(const smvk::VertexFormatDescription& format)
{
	std::vector<VkVertexInputBindingDescription> descriptions;
	descriptions.reserve(format.bindings.size());

	for (const VertexBinding& binding : format.bindings) {
		descriptions.push_back({
			.binding = binding.binding,
			.stride = binding.stride,
			.inputRate = binding.inputRate == VertexInputRate::Instance
				? VK_VERTEX_INPUT_RATE_INSTANCE
				: VK_VERTEX_INPUT_RATE_VERTEX,
			});
	}

	return descriptions;
}
std::vector<VkVertexInputAttributeDescription>
smvk::getVkVertexAttributeDescriptions(const smvk::VertexFormatDescription& format)
{
	std::vector<VkVertexInputAttributeDescription> descriptions;

	size_t attributeCount = 0;

	for (const VertexBinding& binding : format.bindings) {
		attributeCount += binding.attributes.size();
	}

	descriptions.reserve(attributeCount);

	uint32_t location = 0;

	for (const VertexBinding& binding : format.bindings) {
		for (const VertexAttribute& attribute : binding.attributes) {
			descriptions.push_back({
				.location = location++,
				.binding = binding.binding,
				.format = toVkFormat(attribute.format),
				.offset = attribute.offset,
				});
		}
	}

	return descriptions;
}
//
smvk::PipelineBundle smvk::VulkanPipelineCache::BuildGraphicsPipeline(const smvk::PipelineKey& key)
{
	// ========================================================
	// SHADERS
	// ========================================================

	std::vector<VkPipelineShaderStageCreateInfo>shaderStages;
	shaderStages.reserve(key.program.shaderCount);

	// Specialization payloads must outlive vkCreateGraphicsPipelines below.
	// stageSpecs is reserved upfront so &stageSpecs[i].info stays stable.
	struct StageSpec {
		std::vector<VkSpecializationMapEntry> entries;
		VkSpecializationInfo info{};
	};
	std::vector<StageSpec> stageSpecs;
	stageSpecs.reserve(key.program.shaderCount);

	for (size_t i = 0;
		i < key.program.shaderCount;
		++i)
	{
		const Shader& shader =
			key.program.shaders[i];

		auto shaderModule =
			ctx_->shaderModules_.get(
				shader.module);

		VkPipelineShaderStageCreateInfo stageInfo{
			.sType =
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = toVkShaderStageBit(shader.stage),
			.module = shaderModule ? shaderModule->module : VK_NULL_HANDLE,
			.pName = shader.entryPoint,
		};
		if (shader.specialization && !shader.specialization->entries.empty()) {
			const ShaderSpecialization& spec = *shader.specialization;
			StageSpec& ss = stageSpecs.emplace_back();
			ss.entries.reserve(spec.entries.size());
			for (const auto& e : spec.entries) {
				ss.entries.push_back(VkSpecializationMapEntry{
					.constantID = e.constantID,
					.offset = e.offset,
					.size = static_cast<size_t>(e.size),
					});
			}
			ss.info.mapEntryCount = static_cast<uint32_t>(ss.entries.size());
			ss.info.pMapEntries = ss.entries.data();
			ss.info.dataSize = spec.data.size();
			ss.info.pData = spec.data.data();
			stageInfo.pSpecializationInfo = &ss.info;
		}
		shaderStages.push_back(stageInfo);
	}


	// ========================================================
	// VERTEX INPUT
	// ========================================================

	auto bindingDescriptions =
		getVkVertexInputBindingDescriptions(
			key.vertexFormat);

	auto attributeDescriptions =
		getVkVertexAttributeDescriptions(
			key.vertexFormat);

	// Consume-only attribute filter (validation-clean pipelines): the
	// format describes EVERY attribute the geometry carries; the vertex
	// shader consumes a subset (DXC dead-code-strips unused inputs, so
	// the VS's reflected stage inputs are exactly the consumed
	// locations). Binding the full set with an unconsumed VS is
	// legal-but-noisy ("Vertex attribute at location N not consumed by
	// vertex shader" validation spam) and can waste fetch bandwidth.
	// Filter to the VS's reflected locations; a failed/missing
	// reflection keeps the full set (fail-open, the old behavior). A VS
	// with no real inputs (fullscreen triangles) filters everything —
	// correct: nothing is consumed; the binding descriptions stay (a
	// stride-only binding is legal).
	if (!attributeDescriptions.empty()) {
		const Shader* vs = nullptr;
		for (uint8_t i = 0; i < key.program.shaderCount; ++i) {
			if (key.program.shaders[i].stage == ShaderStage::Vertex) {
				vs = &key.program.shaders[i];
				break;
			}
		}
		if (vs) {
			const SpirvReflection refl =
				ctx_->ReflectShaderModule(vs->module);
			if (refl.valid) {
				std::erase_if(attributeDescriptions,
					[&refl](const VkVertexInputAttributeDescription& a) {
					for (const SpirvStageInput& in
						: refl.stageInputs)
						if (in.location == a.location)
							return false;
					return true;
				});
			}
		}
	}

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,

		.vertexBindingDescriptionCount =
			static_cast<uint32_t>(
				bindingDescriptions.size()),

		.pVertexBindingDescriptions =
			bindingDescriptions.data(),

		.vertexAttributeDescriptionCount =
			static_cast<uint32_t>(
				attributeDescriptions.size()),

		.pVertexAttributeDescriptions =
			attributeDescriptions.data(),
	};


	// ========================================================
	// INPUT ASSEMBLY
	// ========================================================

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,

		.topology =
			toVkPrimitiveTopology(
				key.topology),

		.primitiveRestartEnable =
			VK_FALSE,
	};


	// ========================================================
	// VIEWPORT
	// ========================================================

	VkPipelineViewportStateCreateInfo viewportState{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,

		.viewportCount = 1,
		.scissorCount = 1,
	};


	// ========================================================
	// RASTERIZATION
	// ========================================================

	VkPipelineRasterizationStateCreateInfo rasterization{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,

		.depthClampEnable =
			key.raster.depthClampEnable,

		.rasterizerDiscardEnable =
			key.raster.rasterizerDiscardEnable,

		.polygonMode =
		// Line/Point need the fillModeNonSolid feature at device
		// creation; clamp to Fill when it is unavailable instead of
		// rasterizing with an unsupported mode.
		(key.raster.polygonMode != PolygonMode::Fill && !g_fillModeNonSolid)
		? VK_POLYGON_MODE_FILL
		: toVkPolygonMode(
			key.raster.polygonMode),

	.cullMode =
		toVkCullMode(
			key.raster.cullMode),

	.frontFace =
		toVkFrontFace(
			key.raster.frontFace),

	.depthBiasEnable =
		key.depth.depthBiasEnable,

	.depthBiasConstantFactor =
		key.depth.depthBiasConstantFactor,

	.depthBiasClamp =
		key.depth.depthBiasClamp,

	.depthBiasSlopeFactor =
		key.depth.depthBiasSlopeFactor,

	.lineWidth =
		key.raster.lineWidth,
	};


	// ========================================================
	// MULTISAMPLING
	// ========================================================

	VkPipelineMultisampleStateCreateInfo multisampling{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,

		.rasterizationSamples =
			toVkSampleCount(
				key.msaa.samples),

		.sampleShadingEnable =
			VK_FALSE,

		// Alpha-to-coverage: coverage mask from FS alpha output instead
		// of a shader discard (masked cutout, MSAA mode only).
		.alphaToCoverageEnable =
			key.msaa.alphaToCoverage
				? VK_TRUE
				: VK_FALSE,
	};


	// ========================================================
	// DEPTH / STENCIL
	// ========================================================

	VkPipelineDepthStencilStateCreateInfo depthStencil{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,

		.depthTestEnable =
			key.depth.depthTestEnable,

		.depthWriteEnable =
			key.depth.writeEnable,

		.depthCompareOp =
			toVkCompareOp(
				key.depth.depthFunc),

		.depthBoundsTestEnable =
			VK_FALSE,

		.stencilTestEnable =
			key.stencil.frontTestEnable ||
			key.stencil.backTestEnable,
	};


	// --------------------------------------------------------
	// Front stencil
	// --------------------------------------------------------

	depthStencil.front.compareOp =
		toVkCompareOp(
			key.stencil.frontCompareOp);

	depthStencil.front.failOp =
		toVkStencilOp(
			key.stencil.frontFailOp);

	depthStencil.front.passOp =
		toVkStencilOp(
			key.stencil.frontPassOp);

	depthStencil.front.depthFailOp =
		VK_STENCIL_OP_KEEP;

	depthStencil.front.compareMask =
		0xff;

	depthStencil.front.writeMask =
		key.stencil.frontWriteEnable
		? 0xff
		: 0;

	depthStencil.front.reference = 0;


	// --------------------------------------------------------
	// Back stencil
	// --------------------------------------------------------

	depthStencil.back.compareOp =
		toVkCompareOp(
			key.stencil.backCompareOp);

	depthStencil.back.failOp =
		toVkStencilOp(
			key.stencil.backFailOp);

	depthStencil.back.passOp =
		toVkStencilOp(
			key.stencil.backPassOp);

	depthStencil.back.depthFailOp =
		VK_STENCIL_OP_KEEP;

	depthStencil.back.compareMask =
		0xff;

	depthStencil.back.writeMask =
		key.stencil.backWriteEnable
		? 0xff
		: 0;

	depthStencil.back.reference = 0;


	// ========================================================
	// COLOR BLENDING
	// ========================================================

	std::vector<VkPipelineColorBlendAttachmentState>
		colorBlendAttachments;

	colorBlendAttachments.resize(
		key.colorFormats.size());

	for (size_t i = 0;
		i < colorBlendAttachments.size();
		++i)
	{
		const size_t stateIndex =
			std::min<size_t>(
				i,
				std::size(
					key.blend.blendState.attachment) - 1);

		const BlendAttachmentState& src =
			key.blend.blendState
			.attachment[stateIndex];

		auto& dst =
			colorBlendAttachments[i];

		dst.blendEnable =
			key.blend.blendState.blendEnable;

		dst.srcColorBlendFactor =
			toVkSrcBlendFactor(
				src.srcColorBlendFactor);

		dst.dstColorBlendFactor =
			toVkDstBlendFactor(
				src.dstColorBlendFactor);

		dst.colorBlendOp =
			toVkBlendOp(
				src.colorBlendOp);

		// Your RHI currently doesn't have separate
		// alpha src/dst factors.
		//
		// For now this mirrors color factors.
		dst.srcAlphaBlendFactor =
			dst.srcColorBlendFactor;

		dst.dstAlphaBlendFactor =
			dst.dstColorBlendFactor;

		dst.alphaBlendOp =
			toVkBlendOp(
				src.alphaBlendOp);

		dst.colorWriteMask =
			toVkColorWriteMask(
				src.colorWriteMask);
	}

	VkPipelineColorBlendStateCreateInfo colorBlending{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,

		.logicOpEnable =
			VK_FALSE,

		.logicOp =
			VK_LOGIC_OP_COPY,

		.attachmentCount =
			static_cast<uint32_t>(
				colorBlendAttachments.size()),

		.pAttachments =
			colorBlendAttachments.data(),
	};


	// ========================================================
	// DYNAMIC STATE
	// ========================================================

	// Stencil reference is dynamic everywhere so the reference value
	// NEVER enters the pipeline cache key (per-material references
	// would otherwise mint a pipeline per distinct ref — see the SSR
	// receiver-material stencil writers). The static pipeline state
	// below carries 0; recorders override via CommandBuffer::
	// SetStencilReference, which records into the command stream.
	constexpr VkDynamicState dynamicStates[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_STENCIL_REFERENCE,
	};

	VkPipelineDynamicStateCreateInfo dynamicState{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,

		.dynamicStateCount =
			static_cast<uint32_t>(
				std::size(dynamicStates)),

		.pDynamicStates =
			dynamicStates,
	};


	// ========================================================
	// DYNAMIC RENDERING FORMATS
	// ========================================================

	std::vector<VkFormat> vkColorFormats;
	vkColorFormats.reserve(
		key.colorFormats.size());

	for (Format format : key.colorFormats)
	{
		vkColorFormats.push_back(
			toVkFormat(format));
	}

	const VkFormat vkDepthFormat =
		toVkFormat(key.depthFormat);

	const VkFormat vkStencilFormat =
		toVkFormat(key.stencilFormat);

	VkPipelineRenderingCreateInfo renderingInfo{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,

		.colorAttachmentCount =
			static_cast<uint32_t>(
				vkColorFormats.size()),

		.pColorAttachmentFormats =
			vkColorFormats.data(),

		.depthAttachmentFormat =
			vkDepthFormat,

		.stencilAttachmentFormat =
			vkStencilFormat,
	};


	// ========================================================
	// PIPELINE
	// ========================================================

	const VkPipelineLayout layout = ctx_->defaultPipeLayout;
	VkGraphicsPipelineCreateInfo pipelineInfo{
		.sType =
			VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext =
			&renderingInfo,
		.stageCount =
			static_cast<uint32_t>(
				shaderStages.size()),
		.pStages =
			shaderStages.data(),
		.pVertexInputState =
			&vertexInputInfo,
		.pInputAssemblyState =
			&inputAssembly,
		.pViewportState =
			&viewportState,
		.pRasterizationState =
			&rasterization,
		.pMultisampleState =
			&multisampling,
		.pDepthStencilState =
			&depthStencil,
		.pColorBlendState =
			&colorBlending,
		.pDynamicState =
			&dynamicState,
		.layout =
			layout,
	};

	PipelineBundle bundle{};
	CreateGraphicsPipeline(
		pipelineInfo,
		bundle.pipeline);

	bundle.layout =
		layout;

	return bundle;
}
smvk::PipelineBundle smvk::VulkanPipelineCache::GetRenderPipeline(
	ShaderProgram& program,
	const VertexFormatDescription& vertexFormat,
	const Format* colorFormats,
	uint32_t colorAttachmentCount,
	Format depthFormat,
	Format stencilFormat,
	PrimitiveTopology topology,
	const RasterSettings& raster,
	const DepthSettings& depth,
	const StencilSettings& stencil,
	const BlendSettings& blend,
	const MSAASettings& msaa
)
{
	PipelineKey key{
		.program = program,
		.vertexFormat = vertexFormat,
		.colorFormats = std::vector<Format>(
			colorFormats,
			colorFormats + colorAttachmentCount),
		.depthFormat = depthFormat,
		.stencilFormat = stencilFormat,
		.topology = topology,
		.raster = raster,
		.depth = depth,
		.stencil = stencil,
		.blend = blend,
		.msaa = msaa,
	};

	if (auto it = pipelines_.find(key);
		it != pipelines_.end())
	{
		return it->second;
	}

	PipelineBundle bundle =
		BuildGraphicsPipeline(key);

	auto [it, inserted] =
		pipelines_.emplace(
			std::move(key),
			bundle);

	// Disk pipeline cache: a fresh compile happened — count it toward
	// the next batched save (see NotePipelineCompiled).
	NotePipelineCompiled();

	return it->second;
}
void smvk::VulkanPipelineCache::Clear() {
	for (auto& [key, bundle] : pipelines_)
	{
		if (bundle.pipeline != VK_NULL_HANDLE)
		{
			DestroyGraphicsPipeline(
				bundle.pipeline);
		}
	}
	pipelines_.clear();
	for (auto& [key, bundle] : computePipelines_)
	{
		if (bundle.pipeline != VK_NULL_HANDLE)
		{
			DestroyGraphicsPipeline(
				bundle.pipeline);
		}
	}
	computePipelines_.clear();
}

smvk::PipelineBundle smvk::VulkanPipelineCache::GetComputePipeline(
	ShaderProgram& program)
{
	assert(program.shaderCount == 1);
	const Shader& shader = program.shaders[0];
	assert(shader.stage == ShaderStage::Compute);
	ComputeKey key{ shader.module, shader.entryPoint ? shader.entryPoint : "" };
	if (shader.specialization) {
		key.specEntries = shader.specialization->entries;
		key.specData = shader.specialization->data;
	}
	if (auto it = computePipelines_.find(key); it != computePipelines_.end())
		return it->second;
	PipelineBundle bundle = BuildComputePipeline(program);
	computePipelines_.emplace(std::move(key), bundle);
	// Disk pipeline cache: a fresh compile happened (see NotePipelineCompiled).
	NotePipelineCompiled();
	return bundle;
}

smvk::PipelineBundle smvk::VulkanPipelineCache::BuildComputePipeline(
	ShaderProgram& program)
{
	assert(program.shaderCount == 1);
	const Shader& shader = program.shaders[0];
	auto shaderModule = ctx_->shaderModules_.get(shader.module);
	assert(shaderModule && shaderModule->module != VK_NULL_HANDLE);

	std::vector<VkSpecializationMapEntry> specEntries;
	VkSpecializationInfo specInfo{};

	VkPipelineShaderStageCreateInfo stageInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = shaderModule->module,
		.pName = shader.entryPoint,
	};
	if (shader.specialization && !shader.specialization->entries.empty()) {
		const ShaderSpecialization& spec = *shader.specialization;
		specEntries.reserve(spec.entries.size());
		for (const auto& e : spec.entries) {
			specEntries.push_back(VkSpecializationMapEntry{
				.constantID = e.constantID,
				.offset = e.offset,
				.size = static_cast<size_t>(e.size),
				});
		}
		specInfo.mapEntryCount = static_cast<uint32_t>(specEntries.size());
		specInfo.pMapEntries = specEntries.data();
		specInfo.dataSize = spec.data.size();
		specInfo.pData = spec.data.data();
		stageInfo.pSpecializationInfo = &specInfo;
	}

	const VkPipelineLayout layout = ctx_->defaultPipeLayout;
	VkComputePipelineCreateInfo pipelineInfo{
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = stageInfo,
		.layout = layout,
	};

	PipelineBundle bundle{};
	// Crash diagnostic: vkCreateComputePipelines enters the driver shader
	// compiler (AVs inside nvgpucomp64.dll have been observed there), so
	// log the module identity BEFORE the call (flushed) — the last such
	// line in stderr names the crashing shader.
	{
		const auto& code = shaderModule->code;
		const uint64_t hash = code.empty() ? 0 : PipelineCacheFnv1a64(
			reinterpret_cast<const uint8_t*>(code.data()), code.size());
		std::fprintf(stderr,
			"[PipelineCache] vkCreateComputePipelines entry='%s' module=[idx=%u gen=%u] spirvBytes=%zu spirvHash=0x%016llx specEntries=%zu\n",
			shader.entryPoint ? shader.entryPoint : "",
			shader.module.index(), shader.module.gen(),
			code.size(), static_cast<unsigned long long>(hash),
			specEntries.size());
		std::fflush(stderr);
	}
	CreateComputePipeline(pipelineInfo, bundle.pipeline);
	bundle.layout = layout;
	return bundle;
}

// ------------------------------------------------------------------
// VulkanSamplerCache
// ------------------------------------------------------------------
smvk::VulkanSamplerCache::VulkanSamplerCache(VulkanResourceManager* rm) : rm_(rm) {}
smvk::VulkanSamplerCache::~VulkanSamplerCache() { Clear(); }
uint32_t smvk::VulkanSamplerCache::GetOrCreate(const SamplerCreateInfo& ci) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = map_.find(ci);
	if (it != map_.end()) return it->second;
	if (samplers_.size() >= MAX_SAMPLERS) {
		fprintf(stderr, "[VulkanSamplerCache] MAX_SAMPLERS (%u) exceeded – cannot create new sampler\n", (unsigned)MAX_SAMPLERS);
		assert(false && "MAX_SAMPLERS exceeded");
		return 0;
	}
	VkSampler sampler = VK_NULL_HANDLE;
	CreateSampler(ci, sampler);
	uint32_t index = static_cast<uint32_t>(samplers_.size());
	samplers_.push_back(sampler);
	infos_.push_back(ci);
	map_.emplace(ci, index);
	if (rm_) rm_->m_needToUpdateSamplers = true;
	return index;
}
VkSampler smvk::VulkanSamplerCache::at(uint32_t index) const {
	std::lock_guard<std::mutex> lock(mutex_);
	assert(index < samplers_.size());
	return samplers_[index];
}
bool smvk::VulkanSamplerCache::Recreate(uint32_t index, const SamplerCreateInfo& ci) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (index >= samplers_.size())
		return false;
	if (infos_[index] == ci)
		return true; // already the requested filtering
	VkSampler sampler = VK_NULL_HANDLE;
	CreateSampler(ci, sampler);
	if (sampler == VK_NULL_HANDLE)
		return false;
	if (samplers_[index] != VK_NULL_HANDLE)
		vkDestroySampler(device_, samplers_[index], nullptr);
	samplers_[index] = sampler;
	// Point the new CI at this slot WITHOUT erasing the previous CI
	// mapping for it: many passes call GetOrCreateSampler(defaultCI) every
	// frame and must keep deduping to this stable slot (no new slots, no
	// MAX_SAMPLERS leak across toggles). The slot holds "the current mode"
	// — every CI ever installed here resolves to it, so postfx taps follow
	// the toggle exactly like the hardcoded samplers[0]/[2] material paths.
	infos_[index] = ci;
	map_[ci] = index;
	if (rm_) rm_->m_needToUpdateSamplers = true;
	return true;
}
void smvk::VulkanSamplerCache::Clear() {
	std::lock_guard<std::mutex> lock(mutex_);
	for (VkSampler s : samplers_) if (s != VK_NULL_HANDLE) vkDestroySampler(device_, s, nullptr);
	samplers_.clear();
	infos_.clear();
	map_.clear();
}
//void smvk::App::InitializeImGui() {
//
//	// Setup Dear ImGui context
//	IMGUI_CHECKVERSION();
//	ImGui::CreateContext();
//	ImGuiIO& io = ImGui::GetIO(); (void)io;
//	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
//	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Control	ImGui::SetCurrentContext(imgui_ctx);
//	//io.SetClipboardTextFn = [](void*, const char* text) { glfwSetClipboardString(nullptr, text); };
//	//io.GetClipboardTextFn = [](void*) { return glfwGetClipboardString(nullptr); };
//
//	VkPipelineRenderingCreateInfo pipeline_create{
//		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
//	pipeline_create.pNext = VK_NULL_HANDLE;
//	// We are using the single color buffer out.
//	pipeline_create.colorAttachmentCount = 1;
//	pipeline_create.pColorAttachmentFormats = &m_swapchain->vk_surfaceFormat_;
//	pipeline_create.depthAttachmentFormat = m_swapchain->vk_depthFormat_;
//	pipeline_create.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
//
//	ImGui_ImplGlfw_InitForVulkan(window, /*install callbacks*/ false);
//
//	imgui_descriptorPool = m_context->CreateDescriptorPool();
//	auto dp = m_context->descriptorPools_.get(imgui_descriptorPool);
//
//	ImGui_ImplVulkan_InitInfo info{};
//	info.Instance = core_->GetInstance();
//	info.ApiVersion = m_context->core_.config_.version_;
//	info.PhysicalDevice = core_->GetPhysicalDevice();
//	info.Device = core_->GetDevice();
//	info.Queue = core_->GetGraphicsQueue();
//	info.DescriptorPool = *dp;
//	info.MinImageCount = m_swapchain->GetMinImageCount();
//	info.ImageCount = m_swapchain->GetImageCount();
//	info.UseDynamicRendering = true,
//	info.PipelineRenderingCreateInfo = pipeline_create;
//	ImGui_ImplVulkan_Init(&info  /* oUseDynamicRendering=true */);
//
//	// I think this is no longer needed
//	//auto han = m_context->m_submitter->acquire();
//	//auto cmd = m_context->m_submitter->GetCommandBuffer(han);
//	//ImGui_ImplVulkan_CreateFontsTexture(cmd.cmd);
//	//ImGui_ImplVulkan_DestroyFontUploadObjects(cmd.cmd);
//	//m_context->m_submitter->submit(han);
//
//}
VkImageView GetImageView(smvk::RenderGraph* rg, smvk::LogicalImageViewHandle logicalView) {
	assert(!logicalView.empty());
	auto view = rg->imageViews_.get(logicalView);
	assert(view != nullptr);
	smvk::VulkanResourceManager& vctx = *static_cast<smvk::VulkanResourceManager*>(rg->ctx_.GetVulkanOpaque());

	if (view->type == smvk::LogicalImageView::Type::ExternalPhysical)
	{
		assert(!view->externalViewHandle.empty());
		return vctx.imageViews_.get(view->externalViewHandle)->vkView;
	}

	assert(!view->resolvedViewHandle.empty());
	return vctx.imageViews_.get(view->resolvedViewHandle)->vkView;
}

bool smvk::App::Initialize() {
	// NVIDIA Streamline (DLSS) — regular (non-manual) integration: slInit
	// runs before any Vulkan call and volk is pointed at the interposer's
	// vkGetInstanceProcAddr, so instance/device/swapchain/acquire/present
	// resolve to SL proxies (native passthrough for everything else). When
	// sl.interposer.dll is absent (or non-NVIDIA systems) the plain
	// vulkan-1.dll path is used unchanged.
	const void* slGpa = nullptr;
	if (DlssStartup())
		slGpa = DlssVulkanGetInstanceProcAddr();
	if (slGpa) {
		// This vendored volk's volkInitializeCustom returns void: probe a
		// global-level entry point afterwards to detect a dead SL loader.
		volkInitializeCustom(
			reinterpret_cast<PFN_vkGetInstanceProcAddr>(const_cast<void*>(slGpa)));
		if (vkCreateInstance == nullptr) {
			std::cerr << "Streamline loader failed to bootstrap - "
				"falling back to vulkan-1.dll\n";
			DlssShutdown();
			// No Vulkan objects exist yet, so unloading is safe here;
			// volkInitialize() below re-points the (possibly interposer-
			// resolved) global functions at the native loader.
			DlssRelease();
			slGpa = nullptr;
		}
	}
	if (!slGpa && volkInitialize() != VK_SUCCESS) {
		std::cerr << "Failed to initialize Volk\n";
		return false;
	}

	// SDL3: video + gamepad + audio subsystems. Joystick events are needed
	// for SDL_EVENT_GAMEPAD_* hotplug; audio init is lazy (AudioEngine inits
	// SDL_INIT_AUDIO itself if the app enables sound).
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
		std::cerr << "Failed to initialize SDL3: " << SDL_GetError() << "\n";
		return false;
	}

	// Input bridge: converts SDL events to engine events and owns the
	// opened gamepad handles (destroyed before SDL_Quit).
	m_sdlInput = std::make_unique<smvk::SdlInputBackend>();

	int width_ = DEFAULT_WIDTH;
	int height_ = DEFAULT_HEIGHT;
	window = SDL_CreateWindow("Vulkan", width_, height_,
		SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

	if (!window) {
		std::cerr << "Failed to create SDL3 window: " << SDL_GetError() << "\n";
		SDL_Quit();
		return false;
	}
	SDL_SetWindowTitle(window, /*ENGINE_NAME*/ "samuelmartin.1999@outlok.com");
	// Capture the mouse like the old GLFW_CURSOR_DISABLED default; scenes
	// that need a cursor (image viewers, editors) call SetRelativeMouseMode(false).
	SetRelativeMouseMode(true);

	// Audio: best-effort, never fatal. Mixer thread pins to the dedicated core.
	audio::Config audioCfg;
	if (!m_audio.Init(audioCfg)) {
		std::cerr << "[Audio] disabled: " << m_audio.LastError() << "\n";
	}

	m_context = smvk::mem::MakeProcess<ResourceManager>(window);
	m_swapchain = smvk::mem::MakeProcess<Swapchain>(
		*m_context,
		window
	);

	m_rendergraph = smvk::mem::MakeProcess<RenderGraph>(*m_context);
	m_world = smvk::mem::MakeProcess<World>();

	// Init imgui?s
	return true;

}
void smvk::App::WaitForDeviceIdle() {
	if (device_ == VK_NULL_HANDLE) return;
	VkResult r = vkDeviceWaitIdle(device_);
	if (r != VK_SUCCESS && r != VK_ERROR_DEVICE_LOST) {
		fprintf(stderr, "[Vulkan Error] %s:%d: %s\n",
			__FILE__, __LINE__, vkResultToString(r));
		abort();
	}
	if (r == VK_ERROR_DEVICE_LOST) {
		fprintf(stderr, "[Vulkan Warning] vkDeviceWaitIdle returned VK_ERROR_DEVICE_LOST, proceeding with teardown\n");
		// FIRST-surface fault dump (the editor's churn deaths land HERE —
		// the boundary quiesce / teardown waits): the device-fault query +
		// the diagnostic-checkpoint pinpoint (the last-reached pass per
		// queue). Once per process: every later surface (the Submitter
		// teardown waits, ~VulkanCore) sees the SAME dead device — one
		// dump, taken at the freshest point.
		static bool s_faultDumped = false;
		if (!s_faultDumped) {
			s_faultDumped = true;
			DumpDeviceFaultInfo();
		}
	}
}

void smvk::App::OnFramebufferResized(int width, int height) {
	if (m_swapchain) {
		m_swapchain->SetFramebufferResized(true);
	}
}

void smvk::App::Shutdown() {
	// Called from IApp::Run while object is still fully constructed.
	// This ensures derived TestApp Holders (e.g. ShadowMap, Buffers) are
	// still alive and can be destroyed *after* the wait, before the
	// Vulkan context is torn down. Without this, Holders in TestApp would
	// be destroyed after the Vulkan context (after Run returns, during
	// destructor unwinding), which is safe only if we waited here first.
	WaitForDeviceIdle();
	// Detach the element stack FIRST (reverse attach order), while every
	// member is still alive: elements release their Holders/pipelines
	// against the live context here, before any teardown below.
	for (auto it = m_elements.rbegin(); it != m_elements.rend(); ++it)
		if (*it)
			(*it)->onDetach();
	m_elements.clear();
	// Derived Shutdown (TestApp::Shutdown) will run first via virtual
	// dispatch then this base Shutdown. TestApp should release its Holders
	// that reference m_context before we reset the context below.
	// We do not reset m_* here to allow TestApp::Shutdown to still use
	// m_context if needed; actual destruction happens in ~App via\r
	// member order (derived members -> App members -> window).
	// Close gamepads while SDL is still initialized (the backend owns the
	// SDL_Gamepad handles; SDL_Quit happens in ~App).
	m_sdlInput.reset();
	// Streamline teardown: slShutdown must run before the VkInstance /
	// VkDevice are destroyed (~App tears down m_context). The interposer
	// DLL itself stays mapped until ~VulkanCore unloads it (DlssRelease)
	// because volk/VMA still call through its proxies during teardown.
	DlssShutdown();
}

// Out-of-line: the unique_ptr<SdlInputBackend> member needs the complete
// backend type for its deleter (the header only forward-declares it).
smvk::App::App() = default;

smvk::App::~App()
{
	// The pasted-pattern contract: Shutdown detached + destroyed the
	// element stack while the graphics members were alive. Anything
	// left here missed Shutdown (its Holders would release against a
	// dead context) — the debug assert catches the forgotten call.
	assert(m_elements.empty());
	WaitForDeviceIdle();
	m_audio.Shutdown();
	// Members m_world/m_rendergraph/m_swapchain/m_context destroyed automatically
	// (App holds them directly; window is last member so destroyed after Vulkan).
	// The input bridge must also outlive Input but die before SDL_Quit.
	m_sdlInput.reset();
	if (window) {
		SDL_DestroyWindow(window);
		window = nullptr;
	}
	SDL_Quit();
}


struct smvk::RenderGraph::PipelineCacheOpaque {
	VulkanPipelineCache cache;
	explicit PipelineCacheOpaque(VulkanResourceManager* rm) : cache(rm) {}
};

// ---------------------------------------------------------------------------
// Transient heap (design B - see the RenderGraph header contract): the
// opaque state behind CompileTransientHeap.
// ---------------------------------------------------------------------------
// Logical-image handle key (index | gen<<32): stable pool identity for the
// placement map (handles themselves churn on repacks).
static uint64_t RgLogicalImageKey(smvk::LogicalImageHandle h) {
	return static_cast<uint64_t>(h.index())
		| (static_cast<uint64_t>(h.gen()) << 32);
}
// RMI image handle key (the interval map's key).
static uint64_t RgPhysicalImageKey(smvk::ImageHandle h) {
	return static_cast<uint64_t>(h.index())
		| (static_cast<uint64_t>(h.gen()) << 32);
}
// SHAPE identity (the spec's aliasing parameter-identity requirement): two
// images may share a packed range only when EVERY creation parameter
// matches except the debug name. Anything else packs to a disjoint range.
static uint64_t RgTransientShapeHash(const smvk::ImageCreateInfo& k) {
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&h](uint64_t v) {
		h = (h ^ v) * 1099511628211ull;
	};
	mix(static_cast<uint64_t>(k.type));
	mix(k.extent.width); mix(k.extent.height); mix(k.extent.depth);
	mix(static_cast<uint64_t>(k.format));
	mix(static_cast<uint64_t>(k.usage));
	mix(static_cast<uint64_t>(k.samples));
	mix(k.mipLevels); mix(k.arrayLayers);
	mix(k.isCubemap ? 1u : 0u);
	mix(k.concurrentCompute ? 1u : 0u);
	// hostTransfer candidates never reach the heap (asserted at creation);
	// mixed in anyway so the hash stays total.
	mix(k.hostTransfer ? 1u : 0u);
	return h;
}

struct smvk::RenderGraph::TransientHeapOpaque {
	// One device-local block per growth; the VMA virtual block does the
	// placement bookkeeping (offset + alignment honoring), the real
	// VkDeviceMemory is graph-owned and frees wholesale at teardown.
	struct Block {
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		uint32_t memoryTypeIndex = ~0u;
		VmaVirtualBlock virtualBlock = VK_NULL_HANDLE;
	};
	// Logical-image-handle key -> its packed range. The virtual allocation
	// is set on the SLOT OWNER only (disjoint-interval sharers carry null -
	// the shared range frees once, retirement-ordered with the owner).
	struct Placement {
		uint32_t block = ~0u;
		VmaVirtualAllocation allocation = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		VkDeviceSize size = 0;        // aligned memory requirements
		uint32_t bucket = ~0u;        // shape bucket id (diagnostics)
		uint32_t firstUse = ~0u, lastUse = 0;
	};
	std::vector<Block> blocks;
	std::unordered_map<uint64_t, Placement> placements;
	// Layout fingerprint of the packed set: unchanged -> steady state
	// (zero churn), changed -> detonate + repack once.
	uint64_t packedEpoch = ~0ull;
	// Pack diagnostics (one stderr line per pack - rare: toggles).
	uint32_t packCount = 0;
	VkDeviceSize rangeBytes = 0;    // total packed ranges (the heap footprint)
	VkDeviceSize aliasedBytes = 0;  // union bytes saved by interval sharing
};

smvk::RenderGraph::RenderGraph(ResourceManager& ctx) : ctx_(ctx), pipelineCacheOpaque_(smvk::mem::ProcessAllocator().Create<PipelineCacheOpaque>(static_cast<VulkanResourceManager*>(ctx.GetVulkanOpaque()))), transientHeapOpaque_(smvk::mem::ProcessAllocator().Create<TransientHeapOpaque>()) {}
smvk::RenderGraph::~RenderGraph() {
	ReleaseResolvedResources();
	// The heap blocks free retirement-ordered BEHIND every image + placement
	// retire (same-point FIFO: queued after them all). The placement-free
	// tasks already queued reference the block handles by value; they run
	// before these teardown tasks (queued earlier, FIFO), so the virtual
	// block dies after its last free and the memory after its last image.
	{
		VulkanResourceManager& vctx =
			*static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
		for (const auto& block : transientHeapOpaque_->blocks) {
			const VkDeviceMemory memory = block.memory;
			const VmaVirtualBlock vb = block.virtualBlock;
			if (memory == VK_NULL_HANDLE && vb == VK_NULL_HANDLE)
				continue;
			vctx.DeferDestroy(std::packaged_task<void()>(
				[dev = device_, memory, vb] {
				vmaDestroyVirtualBlock(vb);
				vkFreeMemory(dev, memory, nullptr);
			}));
		}
		transientHeapOpaque_->blocks.clear();
	}
}

size_t smvk::RenderGraph::PipelineCacheCount() const
{
	return pipelineCacheOpaque_->cache.Size();
}

void smvk::RenderGraph::ResolveImageViews()
{
	imageViews_.forEach(
		[this](LogicalImageView& logicalView)
	{
		if (logicalView.type ==
			LogicalImageView::Type::ExternalPhysical)
		{
			return;
		}

		if (!logicalView.resolvedViewHandle.empty())
		{
			return;
		}

		auto* logicalImage =
			images_.get(logicalView.key.image);

		assert(logicalImage);
		assert(!logicalImage->resolvedImageHandle.empty());

		ImageViewCreateInfo ci{};

		ci.viewType =
			logicalView.key.viewType;

		ci.format =
			logicalView.key.format;

		ci.subresourceRange =
			logicalView.key.subresourceRange;

		ci.name =
			logicalView.name;

		VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
		logicalView.resolvedViewHandle =
			vctx.CreateImageView(
				logicalImage->resolvedImageHandle,
				ci
			).release();
	}
	);
}
[[nodiscard]] smvk::LogicalImageHandle smvk::RenderGraph::AddImage(const ImageCreateInfo& description, const char* name)
{
	LogicalImage image{
		.type =
			ResourceType::Transient,

		.key =
			description,

		.resolvedImageHandle = {},

		.name =
			name,
	};

	return images_.create(
		std::move(image));
}
[[nodiscard]] smvk::LogicalImageHandle smvk::RenderGraph::AddExternalImage(smvk::ImageHandle imageHandle, const char* name)
{
	assert(!imageHandle.empty());
	VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
	const Image* image = vctx.images_.get(imageHandle);
	assert(image != nullptr);
	LogicalImage logicalImage{
		.type = ResourceType::External,

		.key = {
			.type = image->type,
			.extent = image->extent,
			.format = image->format,
			.usage = image->usage,
			.samples = image->samples,
			.mipLevels = image->mipLevels,
			.arrayLayers = image->arrayLayers,
			.name = name,
		},
		.resolvedImageHandle = imageHandle,
		.name = name,
	};
	return images_.create(
		std::move(logicalImage)
	);
}
void smvk::RenderGraph::ReleaseResolvedResources()
{
	VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
	imageViews_.forEach(
		[this, &vctx](LogicalImageView& view)
	{
		if (view.type ==
			LogicalImageView::Type::ExternalPhysical)
		{
			return;
		}

		if (!view.resolvedViewHandle.empty())
		{
			destroy(&vctx, view.resolvedViewHandle);
		}
	}
	);

	images_.forEachHandle(
		[this, &vctx](LogicalImageHandle h)
	{
		LogicalImage* image = images_.get(h);
		if (!image || image->type == ResourceType::External)
		{
			return;
		}

		if (image->resolvedImageHandle.empty())
		{
			return;
		}

		// Heap-placed transients route through the placement-aware
		// release (the packed range frees retirement-ordered with the
		// image); plain VMA-path transients (hostTransfer) destroy
		// directly.
		if (transientHeapOpaque_->placements.count(RgLogicalImageKey(h)))
			ReleaseTransientPlacement(h);
		else
			destroy(&vctx, image->resolvedImageHandle);
	}
	);
}
void smvk::RenderGraph::FreeImageView(
	LogicalImageViewHandle handle)
{
	if (handle.empty())
		return;

	auto* view = imageViews_.get(handle);
	assert(view != nullptr);

	if (view->type !=
		LogicalImageView::Type::ExternalPhysical &&
		!view->resolvedViewHandle.empty())
	{
		VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
		destroy(&vctx, view->resolvedViewHandle);
	}

	imageViews_.destroy(handle);
}

void smvk::RenderGraph::FreeImage(
	LogicalImageHandle handle)
{
	if (handle.empty())
		return;

	auto* image = images_.get(handle);
	assert(image != nullptr);

	if (image->type != ResourceType::External &&
		!image->resolvedImageHandle.empty())
	{
		VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
		// Heap-placed transients release their packed range with the
		// image (retirement-ordered - see ReleaseTransientPlacement);
		// plain VMA-path transients destroy directly.
		if (transientHeapOpaque_->placements.count(RgLogicalImageKey(handle)))
			ReleaseTransientPlacement(handle);
		else
			destroy(&vctx, image->resolvedImageHandle);
	}

	images_.destroy(handle);
}

void smvk::RenderGraph::FreeBuffer(
	LogicalBufferHandle handle)
{
	if (handle.empty())
		return;
	// External buffers are owned by the caller (ForwardPlus tile buffers,
	// app light buffers) – just drop the logical alias, never the physical.
	buffers_.destroy(handle);
}
void smvk::RenderGraph::ResolveImages() {
	VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
	images_.forEach(
		[this, &vctx](smvk::LogicalImage& logicalImage) {
		if (logicalImage.resolvedImageHandle.empty()) {
			if (logicalImage.type == smvk::ResourceType::External) { assert(false && "external image unresolved. shouldnt be possible"); };
			// Design B (the transient heap): transients create UNBOUND
			// (handles + views exist for the compile-time interval
			// analysis), memory binds at CompileTransientHeap's packed
			// ranges. hostTransfer images stay on the plain VMA path -
			// host-copy binding requirements are not honored by the
			// manual bind.
			if (logicalImage.key.hostTransfer)
				logicalImage.resolvedImageHandle = vctx.CreateImage(logicalImage.key, nullptr).release();
			else
				logicalImage.resolvedImageHandle = vctx.CreateHeapImage(logicalImage.key).release();
		};
	}
	);
};

// Releases one heap-placed transient (FreeImage + the repack detonation):
// destroys the physical (deferred, retire-ordered) and queues the packed
// range's virtual free BEHIND the destroy (DeferDestroy appends at the
// current point, FIFO - the range can never be re-bound while its previous
// binding's object still exists).
void smvk::RenderGraph::ReleaseTransientPlacement(LogicalImageHandle handle)
{
	VulkanResourceManager& vctx =
		*static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
	TransientHeapOpaque& heap = *transientHeapOpaque_;
	auto it = heap.placements.find(RgLogicalImageKey(handle));
	if (it == heap.placements.end())
		return; // not heap-placed (plain VMA path or not yet packed)
	const TransientHeapOpaque::Placement p = it->second;
	heap.placements.erase(it);
	LogicalImage* li = images_.get(handle);
	if (li && !li->resolvedImageHandle.empty())
		smvk::destroy(&vctx, li->resolvedImageHandle);
	if (li)
		li->resolvedImageHandle = {};
	if (p.allocation != VK_NULL_HANDLE) {
		// The slot owner frees the shared range (sharers carry null).
		const VmaVirtualBlock vb = heap.blocks[p.block].virtualBlock;
		const VmaVirtualAllocation alloc = p.allocation;
		vctx.DeferDestroy(std::packaged_task<void()>([vb, alloc] {
			vmaVirtualFree(vb, alloc);
		}));
	}
}

// Design B compile step (see the header contract): interval-packed memory
// aliasing for the graph's transients. The system reference — candidates,
// intervals, shape buckets, the epoch fingerprint, detonation/repack,
// the bind-before-view contract, the retirement FIFO — is
// docs/target_heap_packing.md (§4 design B).
bool smvk::RenderGraph::CompileTransientHeap(
	const std::vector<PassUsage>& usages,
	const std::vector<size_t>& sorted)
{
	VulkanResourceManager& vctx =
		*static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
	TransientHeapOpaque& heap = *transientHeapOpaque_;

	// ---- 1. Heap candidates: live logical transients (hostTransfer stays
	// on the plain VMA path - see ResolveImages). ----
	std::vector<LogicalImageHandle> candidates;
	images_.forEachHandle([&](LogicalImageHandle h) {
		const LogicalImage* li = images_.get(h);
		if (!li || li->type != ResourceType::Transient)
			return;
		if (li->key.hostTransfer)
			return;
		candidates.push_back(h);
	});
	if (candidates.empty())
		return false;

	// ---- 2. Use intervals: per physical image, the [first, last] span
	// of SORTED positions whose passes touch it. The positions are stable
	// across a detonation+re-sort by construction (TopologicalSort builds
	// edges in pass-index order over the same logical structure and runs
	// a stable min-heap Kahn), so intervals computed from THIS sort stay
	// valid for the recomputed one after a detonation. ----
	std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> physInterval;
	for (size_t pos = 0; pos < sorted.size(); ++pos) {
		const PassUsage& pu = usages[sorted[pos]];
		for (const auto& pr : pu.images) {
			const uint64_t k = RgPhysicalImageKey(pr.first);
			auto [it, inserted] = physInterval.try_emplace(k,
				static_cast<uint32_t>(pos), static_cast<uint32_t>(pos));
			if (!inserted) {
				it->second.first = std::min(it->second.first,
					static_cast<uint32_t>(pos));
				it->second.second = std::max(it->second.second,
					static_cast<uint32_t>(pos));
			}
		}
	}

	struct CandInfo {
		LogicalImageHandle handle;
		LogicalImage* image = nullptr;
		uint64_t shape = 0;
		uint32_t firstUse = 0, lastUse = 0;
		VkImage vkImage = VK_NULL_HANDLE;
	};
	std::vector<CandInfo> infos;
	infos.reserve(candidates.size());
	const uint32_t unusedPos = static_cast<uint32_t>(sorted.size());
	for (LogicalImageHandle h : candidates) {
		LogicalImage* li = images_.get(h);
		if (!li)
			continue;
		uint32_t first = unusedPos, last = unusedPos;
		if (!li->resolvedImageHandle.empty()) {
			auto it = physInterval.find(
				RgPhysicalImageKey(li->resolvedImageHandle));
			if (it != physInterval.end()) {
				first = it->second.first;
				last = it->second.second;
			}
		}
		const Image* phys = li->resolvedImageHandle.empty()
			? nullptr : vctx.images_.get(li->resolvedImageHandle);
		infos.push_back({ h, li, RgTransientShapeHash(li->key),
			first, last, phys ? phys->vkImage : VK_NULL_HANDLE });
	}

	// ---- 3. Layout fingerprint: the pass count + every candidate's
	// (shape, interval). Unchanged -> steady state: the packed images +
	// placements stand, zero churn. ----
	uint64_t epoch = 1099511628211ull * (candidates.size() + 1)
		+ sorted.size();
	for (const CandInfo& ci : infos) {
		epoch = (epoch ^ ci.shape) * 1099511628211ull;
		epoch = (epoch ^ ci.firstUse) * 1099511628211ull;
		epoch = (epoch ^ ci.lastUse) * 1099511628211ull;
	}
	if (!heap.placements.empty() && epoch == heap.packedEpoch)
		return false;

	// ---- 4. Layout change (or first pack). Detonate the packed set: the
	// views first (FIFO retire: views before their images), then every
	// placed image + range through ReleaseTransientPlacement. ----
	const bool detonated = !heap.placements.empty();
	if (detonated) {
		// Views on placed transients die + clear (fresh views recreate
		// after ResolveImages below).
		imageViews_.forEach([&](LogicalImageView& view) {
			if (view.type == LogicalImageView::Type::ExternalPhysical)
				return;
			if (view.resolvedViewHandle.empty())
				return;
			const LogicalImage* li = images_.get(view.key.image);
			if (!li || li->type != ResourceType::Transient)
				return;
			if (li->key.hostTransfer)
				return;
			if (!heap.placements.count(RgLogicalImageKey(view.key.image)))
				return;
			smvk::destroy(&vctx, view.resolvedViewHandle);
			view.resolvedViewHandle = {};
		});
		std::vector<LogicalImageHandle> placed;
		images_.forEachHandle([&](LogicalImageHandle h) {
			if (heap.placements.count(RgLogicalImageKey(h)))
				placed.push_back(h);
		});
		for (LogicalImageHandle h : placed)
			ReleaseTransientPlacement(h);
	}

	// ---- 5. (Re)create unresolved transients (unbound heap images) +
	// their views (fresh handles after a detonation). ----
	ResolveImages();
	if (detonated)
		ResolveImageViews();
	for (CandInfo& ci : infos) {
		ci.image = images_.get(ci.handle);
		if (!ci.image || ci.image->resolvedImageHandle.empty())
			continue;
		const Image* phys = vctx.images_.get(ci.image->resolvedImageHandle);
		ci.vkImage = phys ? phys->vkImage : VK_NULL_HANDLE;
		// The interval stays: the positions are logical (see step 2).
	}

	// ---- 6. Shape buckets (alias-identity; see RgTransientShapeHash). ----
	std::vector<uint64_t> bucketShape;
	std::vector<std::vector<uint32_t>> bucketMembers;
	for (uint32_t i = 0; i < infos.size(); ++i) {
		uint32_t b = 0;
		for (; b < bucketShape.size(); ++b)
			if (bucketShape[b] == infos[i].shape)
				break;
		if (b == bucketShape.size()) {
			bucketShape.push_back(infos[i].shape);
			bucketMembers.emplace_back();
		}
		bucketMembers[b].push_back(i);
	}

	// ---- 7. Per bucket: interval scheduling into shared range slots. ----
	// First-fit over slots whose occupant finished before this image
	// starts (classic interval scheduling - not optimal, good enough for
	// a debug-grade packer; the heap is correct regardless of the fit
	// quality: every non-shared range is disjoint by construction).
	heap.rangeBytes = 0;
	heap.aliasedBytes = 0;
	VkDeviceSize packedUnionBytes = 0; // the no-aliasing footprint
	for (uint32_t b = 0; b < bucketShape.size(); ++b) {
		std::vector<uint32_t>& members = bucketMembers[b];
		std::sort(members.begin(), members.end(),
			[&](uint32_t a, uint32_t c) {
			return infos[a].firstUse < infos[c].firstUse;
		});
		struct Slot {
			uint32_t lastUse = 0;
			uint32_t block = ~0u;
			VmaVirtualAllocation alloc = VK_NULL_HANDLE;
			VkDeviceSize offset = 0, size = 0;
		};
		std::vector<Slot> slots;
		for (uint32_t m : members) {
			CandInfo& ci = infos[m];
			if (!ci.image || ci.image->resolvedImageHandle.empty()
				|| ci.vkImage == VK_NULL_HANDLE)
				continue;
			VkMemoryRequirements reqs{};
			vkGetImageMemoryRequirements(device_, ci.vkImage, &reqs);
			const VkDeviceSize need = (reqs.size + reqs.alignment - 1)
				/ reqs.alignment * reqs.alignment;
			packedUnionBytes += need;
			// Join a free slot?
			int64_t join = -1;
			for (size_t s = 0; s < slots.size(); ++s) {
				if (slots[s].lastUse < ci.firstUse) {
					join = static_cast<int64_t>(s);
					break;
				}
			}
			uint32_t block = ~0u;
			VkDeviceSize offset = 0;
			VmaVirtualAllocation alloc = VK_NULL_HANDLE;
			if (join >= 0) {
				// Shared range (identical shape + disjoint use interval:
				// the aliasing identity holds; the sharer's first barrier
				// transitions from UNDEFINED, discarding the previous
				// occupant's contents).
				block = slots[join].block;
				offset = slots[join].offset;
				slots[join].lastUse = std::max(slots[join].lastUse,
					ci.lastUse);
				heap.aliasedBytes += need;
			}
			else {
				// New slot: first-fit into an existing compatible block,
				// else grow one (the size ladder honors the OOM policy:
				// loud abort, like EnsureReadbackStaging).
				bool placed = false;
				for (uint32_t bi = 0; bi < heap.blocks.size() && !placed;
					++bi) {
					if (!((reqs.memoryTypeBits
						>> heap.blocks[bi].memoryTypeIndex) & 1u))
						continue;
					VmaVirtualAllocationCreateInfo vi{};
					vi.size = need;
					vi.alignment = reqs.alignment;
					VkDeviceSize off = 0;
					VmaVirtualAllocation a = VK_NULL_HANDLE;
					if (vmaVirtualAllocate(heap.blocks[bi].virtualBlock,
						&vi, &a, &off) == VK_SUCCESS) {
						block = bi;
						offset = off;
						alloc = a;
						placed = true;
					}
				}
				if (!placed) {
					// Grow: lowest set memoryTypeBit that is device-local.
					uint32_t typeIndex = ~0u;
					{
						VkPhysicalDeviceMemoryProperties memProps{};
						vkGetPhysicalDeviceMemoryProperties(
							physicalDevice_, &memProps);
						for (uint32_t t = 0;
							t < memProps.memoryTypeCount; ++t) {
							if (!((reqs.memoryTypeBits >> t) & 1u))
								continue;
							if (!(memProps.memoryTypes[t].propertyFlags
								& VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
								continue;
							typeIndex = t;
							break;
						}
						if (typeIndex == ~0u) {
							std::fprintf(stderr,
								"[TransientHeap] no device-local memory "
								"type in mask 0x%x - aborting\n",
								reqs.memoryTypeBits);
							std::fflush(stderr);
							std::abort();
						}
					}
					VkDeviceSize size = smvk::mem::config::Runtime().targetHeapBlock; // ladder: halves down to the request
					VkDeviceSize allocSize = 0;
					VkDeviceMemory memory = VK_NULL_HANDLE;
					while (true) {
						allocSize = std::max(size, need);
						VkMemoryAllocateInfo ai{
							VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
							allocSize, typeIndex };
						if (vkAllocateMemory(device_, &ai, nullptr,
							&memory) == VK_SUCCESS)
							break;
						if (size <= need) {
							std::fprintf(stderr,
								"[TransientHeap] heap block allocation of "
								"%llu bytes failed. No recovery - "
								"aborting.\n",
								(unsigned long long)allocSize);
							std::fflush(stderr);
							std::abort();
						}
						size /= 2;
					}
					VmaVirtualBlockCreateInfo vbci{};
					vbci.size = allocSize;
					VmaVirtualBlock vb = VK_NULL_HANDLE;
					VK_CHECK(vmaCreateVirtualBlock(&vbci, &vb));
					heap.blocks.push_back({ memory, allocSize, typeIndex,
						vb });
					VmaVirtualAllocationCreateInfo vi{};
					vi.size = need;
					vi.alignment = reqs.alignment;
					VkDeviceSize off = 0;
					VK_CHECK(vmaVirtualAllocate(vb, &vi, &alloc, &off));
					block = static_cast<uint32_t>(heap.blocks.size() - 1);
					offset = off;
				}
				slots.push_back({ ci.lastUse, block, alloc, offset, need });
				heap.rangeBytes += need;
			}
			// Bind + record the placement (the slot owner carries the
			// virtual allocation; sharers free through it).
			vctx.BindHeapImage(ci.image->resolvedImageHandle,
				heap.blocks[block].memory, offset);
			TransientHeapOpaque::Placement p{};
			p.block = block;
			p.allocation = (join >= 0) ? VK_NULL_HANDLE : alloc;
			p.offset = offset;
			p.size = need;
			p.bucket = b;
			p.firstUse = ci.firstUse;
			p.lastUse = ci.lastUse;
			heap.placements[RgLogicalImageKey(ci.handle)] = p;
		}
	}

	heap.packedEpoch = epoch;
	++heap.packCount;
	std::fprintf(stderr,
		"[TransientHeap] pack #%u: %zu images / %zu buckets / %zu blocks, "
		"%.1f MiB ranges vs %.1f MiB union (%.1f MiB interval-shared)%s\n",
		heap.packCount, infos.size(), bucketShape.size(),
		heap.blocks.size(),
		double(heap.rangeBytes) / 1048576.0,
		double(packedUnionBytes) / 1048576.0,
		double(heap.aliasedBytes) / 1048576.0,
		detonated ? " (repacked: layout changed)" : "");
	return detonated;
}
void smvk::RenderGraph::ResolveBuffers()
{
	//	buffers.forEach(
	//		[this](LogicalBuffer& logicalBuffer) {
	//		if (logicalBuffer.resolvedBufferHandle.empty()) {
	//			const auto& key = logicalBuffer.key;
	//			int ringMultiplier = key.ring ? 4 : 1;
	//			logicalBuffer.resolvedBufferHandle = ctx_.CreateBuffer(
	//				key.elementCount * key.elementSize * ringMultiplier,
	//				smvk::BufferType_Storage,
	//				"resolved buffer"
	//			).release();
	//		}
	//	}
	//	);
}
// ============================================================================
// Sync helpers – collect usages, topological sort, barrier emission
// ============================================================================
std::vector<smvk::RenderGraph::PassUsage> smvk::RenderGraph::CollectPassUsages() const
{
	VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
	std::vector<PassUsage> out;
	out.reserve(passes_.size());
	// Logical-view -> physical-image resolution (external views resolve to
	// the RMI view's backing image; transient views to the resolved image).
	// EMPTY result = no backing image (swapchain external - sync stays in
	// Acquire/Present) or a dead view - the caller skips THAT ONE sync
	// entry. NEVER skip the rest of the pass on an empty result: `out` must
	// stay exactly ONE entry per `passes_` entry (TopologicalSort + Execute
	// index them 1:1) - the old depth-attachment `continue`s skipped the
	// image/buffer hints AND the push_back, corrupting the pass mapping.
	const auto resolveViewImage =
		[&](LogicalImageViewHandle view) -> ImageHandle {
		if (view.empty())
			return {};
		auto* v = imageViews_.get(view);
		if (!v)
			return {};
		if (v->type == LogicalImageView::Type::ExternalPhysical) {
			auto* ext = vctx.imageViews_.get(v->externalViewHandle);
			if (!ext)
				return {};
			return ext->image; // empty = swapchain - external sync
		}
		auto* li = images_.get(v->key.image);
		if (!li)
			return {};
		return li->resolvedImageHandle;
	};
	for (const auto& pass : passes_) {
		PassUsage pu;
		// Color attachments – writes
		for (auto& att : pass.colorAttachments_) {
			const ImageHandle phys = resolveViewImage(att.view);
			if (phys.empty()) continue;
			ImageSyncRequest req{};
			req.barrierMasks.accessMask = AccessFlags::ColorAttachmentWrite;
			// Preserve read if Load
			if (att.loadOp == AttachmentLoadOp::Load) req.barrierMasks.accessMask = req.barrierMasks.accessMask | AccessFlags::ColorAttachmentRead;
			req.barrierMasks.stageMask = PipelineStageBit_Fragment; // mapped to COLOR_ATTACHMENT_OUTPUT
			req.layout = ImageLayout::ColorAttachmentOptimal;
			pu.images.emplace_back(phys, req);
			// MSAA resolve destination – also a color-attachment write.
			if (!att.resolveView.empty()) {
				const ImageHandle rphys = resolveViewImage(att.resolveView);
				if (!rphys.empty() && rphys != phys) {
					ImageSyncRequest rreq{};
					rreq.barrierMasks.accessMask = AccessFlags::ColorAttachmentWrite;
					rreq.barrierMasks.stageMask = PipelineStageBit_Fragment;
					rreq.layout = ImageLayout::ColorAttachmentOptimal;
					pu.images.emplace_back(rphys, rreq);
				}
			}
		}
		// Depth attachment: an unresolvable view skips ONLY the depth sync
		// entry (see the helper contract above) - never the pass.
		if (pass.hasDepthAttachment_) {
			auto& att = pass.depthAttachment_;
			const ImageHandle phys = resolveViewImage(att.view);
			if (!phys.empty()) {
				ImageSyncRequest req{};
				req.barrierMasks.accessMask = AccessFlags::DepthStencilAttachmentWrite | AccessFlags::DepthStencilAttachmentRead;
				req.barrierMasks.stageMask = PipelineStageBit_Fragment;
				req.layout = ImageLayout::DepthStencilAttachmentOptimal;
				pu.images.emplace_back(phys, req);
			}
		}
		// Explicit image hints (includes SetInput/Storage derived). The
		// continues below skip ONE hint (inner loop) - the pass entry still
		// lands in `out`.
		for (auto& h : pass.imageHints_) {
			const ImageHandle phys = resolveViewImage(h.view);
			if (phys.empty()) continue;
			// deduplicate if already covered by attachment (attachment takes precedence)
			bool dup = false;
			for (auto& e : pu.images) if (e.first == phys) { dup = true; break; }
			if (dup) continue;
			ImageSyncRequest req = h.request;
			if (req.barrierMasks.stageMask == PipelineStageBit_None)
				req.barrierMasks.stageMask = DerivePipelineStage(req.barrierMasks.accessMask);
			// Depth sampled fixup: if image is depth format and hint requests ShaderReadOnly, promote to DepthReadOnly for correctness
			{
				auto* img = vctx.images_.get(phys);
				if (img) {
					VkFormat fmt = toVkFormat(img->format);
					bool isDepth = (fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT);
					if (isDepth && req.layout == ImageLayout::ShaderReadOnlyOptimal)
						req.layout = ImageLayout::DepthStencilReadOnlyOptimal;
				}
			}
			pu.images.emplace_back(phys, req);
		}
		// Buffer hints
		for (auto& h : pass.bufferHints_) {
			if (h.buffer.empty()) continue;
			auto* lb = buffers_.get(h.buffer);
			if (!lb) continue;
			BufferHandle phys = lb->resolvedBufferHandle;
			if (phys.empty()) {
				// External buffer – use resolved directly
				phys = lb->resolvedBufferHandle;
				// If external and empty, skip (not resolved)
				if (phys.empty()) continue;
			}
			// For transient buffers not yet created, skip barrier (no physical)
			if (phys.empty()) continue;
			BufferSyncRequest req = h.request;
			if (req.barrierMasks.stageMask == PipelineStageBit_None)
				req.barrierMasks.stageMask = DerivePipelineStage(req.barrierMasks.accessMask);
			pu.buffers.emplace_back(phys, req);
		}
		out.push_back(std::move(pu));
	}
	// The 1:1 invariant the sort + Execute rely on (an inner skip must
	// never cost a pass entry - see the resolveViewImage contract).
	assert(out.size() == passes_.size());
	return out;
}

const std::vector<size_t>& smvk::RenderGraph::TopologicalSort(const std::vector<PassUsage>& usages) const
{
	size_t n = usages.size();
	topoSorted_.clear();
	if (n <= 1) {
		topoSorted_.reserve(n);
		for (size_t i = 0; i < n; ++i) topoSorted_.push_back(i);
		return topoSorted_;
	}
	// Reused scratch (mutable members): adjacency/indegree/frontier and
	// the resource bookkeeping maps used to be rebuilt from scratch every
	// Execute. clear() keeps the maps' bucket arrays and the adjacency
	// rows' capacity.
	if (topoAdj_.size() < n) topoAdj_.resize(n);
	for (size_t i = 0; i < n; ++i) topoAdj_[i].clear();
	topoIndeg_.assign(n, 0);
	topoLastWriterImage_.clear();
	topoLastReadersImage_.clear();
	topoLastWriterBuffer_.clear();
	topoLastReadersBuffer_.clear();
	// Map resource -> last writer / readers tracking for edge generation
	// We preserve insertion order hazard: iterate passes in order, for each resource add edge from previous access that hazards
	auto& lastWriterImage = topoLastWriterImage_;
	auto& lastReadersImage = topoLastReadersImage_;
	auto& lastWriterBuffer = topoLastWriterBuffer_;
	auto& lastReadersBuffer = topoLastReadersBuffer_;
	auto keyFor = [](auto h) { return (static_cast<uint64_t>(h.index()) | (static_cast<uint64_t>(h.gen()) << 32)); };
	for (size_t i = 0; i < n; ++i) {
		const auto& pu = usages[i];
		for (auto& p : pu.images) {
			uint64_t k = keyFor(p.first);
			bool isWrite = IsWriteAccess(p.second.barrierMasks.accessMask);
			if (isWrite) {
				// WAW: last writer -> this
				auto itW = lastWriterImage.find(k);
				if (itW != lastWriterImage.end()) {
					size_t src = itW->second;
					if (src != i) { topoAdj_[src].push_back(i); topoIndeg_[i]++; }
				}
				// WAR: last readers -> this
				auto itR = lastReadersImage.find(k);
				if (itR != lastReadersImage.end()) {
					for (size_t r : itR->second) { topoAdj_[r].push_back(i); topoIndeg_[i]++; }
					itR->second.clear();
				}
				lastWriterImage[k] = i;
			}
			else {
				// RAW: last writer -> this
				auto itW = lastWriterImage.find(k);
				if (itW != lastWriterImage.end()) {
					size_t src = itW->second;
					topoAdj_[src].push_back(i); topoIndeg_[i]++;
				}
				lastReadersImage[k].push_back(i);
			}
		}
		for (auto& p : pu.buffers) {
			uint64_t k = keyFor(p.first);
			bool isWrite = IsWriteAccess(p.second.barrierMasks.accessMask);
			if (isWrite) {
				auto itW = lastWriterBuffer.find(k);
				if (itW != lastWriterBuffer.end()) { topoAdj_[itW->second].push_back(i); topoIndeg_[i]++; }
				auto itR = lastReadersBuffer.find(k);
				if (itR != lastReadersBuffer.end()) {
					for (size_t r : itR->second) { topoAdj_[r].push_back(i); topoIndeg_[i]++; }
					itR->second.clear();
				}
				lastWriterBuffer[k] = i;
			}
			else {
				auto itW = lastWriterBuffer.find(k);
				if (itW != lastWriterBuffer.end()) { topoAdj_[itW->second].push_back(i); topoIndeg_[i]++; }
				lastReadersBuffer[k].push_back(i);
			}
		}
	}
	// Kahn over a min-heap (stable: prefer lower original index). The
	// old code kept the ready set in a vector and re-sorted ALL of it
	// after every release — O(n^2 log n) per Execute.
	auto& zero = topoHeap_;
	zero.clear();
	topoSorted_.reserve(n);
	for (size_t i = 0; i < n; ++i) if (topoIndeg_[i] == 0) zero.push_back(i);
	std::make_heap(zero.begin(), zero.end(), std::greater<size_t>());
	while (!zero.empty()) {
		std::pop_heap(zero.begin(), zero.end(), std::greater<size_t>());
		size_t u = zero.back(); zero.pop_back();
		topoSorted_.push_back(u);
		for (size_t v : topoAdj_[u]) {
			if (--topoIndeg_[v] == 0) {
				zero.push_back(v);
				std::push_heap(zero.begin(), zero.end(), std::greater<size_t>());
			}
		}
	}
	if (topoSorted_.size() != n) {
		// Cycle – fall back to insertion order and assert
		assert(false && "RenderGraph cycle detected in pass dependencies");
		topoSorted_.clear();
		for (size_t i = 0; i < n; ++i) topoSorted_.push_back(i);
	}
	return topoSorted_;
}

void smvk::RenderGraph::EmitBarriers(void* cmdOpaque, const std::vector<size_t>& sortedOrder, const std::vector<PassUsage>& usages)
{
	if (sortedOrder.empty() || usages.empty()) return;
	VkCommandBuffer vkCmd = reinterpret_cast<VkCommandBuffer>(cmdOpaque);
	VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
	// Snapshot global states under shared lock – local bookkeeping starts from these
	auto globalImageStates = vctx.SnapshotImageSyncStates();
	auto globalBufferStates = vctx.SnapshotBufferSyncStates();
	// Local states – seeded from global, with transient default Undefined/None if missing
	std::unordered_map<uint64_t, VulkanResourceManager::ImageGlobalSyncState> localImage = globalImageStates;
	std::unordered_map<uint64_t, VulkanResourceManager::BufferGlobalSyncState> localBuffer = globalBufferStates;

	auto imgBarriers = MakeFrameScratchVec<VkImageMemoryBarrier2>();
	auto bufBarriers = MakeFrameScratchVec<VkBufferMemoryBarrier2>();
	imgBarriers.reserve(32);
	bufBarriers.reserve(16);
	auto keyFor = [](auto h) { return (static_cast<uint64_t>(h.index()) | (static_cast<uint64_t>(h.gen()) << 32)); };

	for (size_t idx : sortedOrder) {
		const auto& pu = usages[idx];
		for (auto& pr : pu.images) {
			ImageHandle h = pr.first;
			ImageSyncRequest req = pr.second;
			uint64_t k = keyFor(h);
			auto it = localImage.find(k);
			VulkanResourceManager::ImageGlobalSyncState src{};
			if (it != localImage.end()) src = it->second;
			else { src.masks = { AccessFlags::None, PipelineStageBit_None }; src.layout = ImageLayout::Undefined; }

			bool srcIsWrite = IsWriteAccess(src.masks.accessMask);
			bool dstIsWrite = IsWriteAccess(req.barrierMasks.accessMask);
			bool needBarrier = false;
			if (src.layout != req.layout) needBarrier = true;
			else if (src.masks.accessMask != req.barrierMasks.accessMask || src.masks.stageMask != req.barrierMasks.stageMask) {
				// Read->Read with same layout needs no barrier
				if (!(!srcIsWrite && !dstIsWrite)) needBarrier = true;
			}
			// WAW with same layout/access still needs barrier for ordering/visibility (depth atlas tiles)
			else if (srcIsWrite && dstIsWrite) {
				needBarrier = true;
			}
			// Also if src is Undefined, we need barrier for layout transition
			if (src.layout == ImageLayout::Undefined && req.layout != ImageLayout::Undefined) needBarrier = true;

			if (needBarrier) {
				auto* img = vctx.images_.get(h);
				if (!img || img->vkImage == VK_NULL_HANDLE) continue;
				VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
				b.srcStageMask = ToVkStage(src.masks.stageMask);
				// For first transition from Undefined, src stage is NONE
				if (src.layout == ImageLayout::Undefined) b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
				b.srcAccessMask = ToVkAccessLocal(src.masks.accessMask);
				if (src.layout == ImageLayout::Undefined) b.srcAccessMask = VK_ACCESS_2_NONE;
				b.dstStageMask = ToVkStage(req.barrierMasks.stageMask);
				// Map RHI fragment to proper Vk stage for attachments
				if (req.layout == ImageLayout::ColorAttachmentOptimal) b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
				else if (req.layout == ImageLayout::DepthStencilAttachmentOptimal) b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
				b.dstAccessMask = ToVkAccessLocal(req.barrierMasks.accessMask);
				b.oldLayout = ToVkImageLayout(src.layout);
				b.newLayout = ToVkImageLayout(req.layout);
				b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = img->vkImage;
				// Use full subresource range – derive aspect from format
				VkFormat fmt = toVkFormat(img->format);
				VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
				// For depth formats, choose depth (and stencil if needed)
				if (fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
				else if (fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
				else if (fmt == VK_FORMAT_S8_UINT) aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
				b.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
				imgBarriers.push_back(b);
			}
			// Update local state to dst
			VulkanResourceManager::ImageGlobalSyncState dstState{};
			dstState.masks = req.barrierMasks;
			dstState.layout = req.layout;
			localImage[k] = dstState;
		}
		for (auto& pr : pu.buffers) {
			BufferHandle h = pr.first;
			BufferSyncRequest req = pr.second;
			uint64_t k = keyFor(h);
			auto it = localBuffer.find(k);
			VulkanResourceManager::BufferGlobalSyncState src{};
			if (it != localBuffer.end()) src = it->second;
			bool srcIsWrite = IsWriteAccess(src.masks.accessMask);
			bool dstIsWrite = IsWriteAccess(req.barrierMasks.accessMask);
			bool needBarrier = false;
			if (src.masks.accessMask != req.barrierMasks.accessMask || src.masks.stageMask != req.barrierMasks.stageMask) {
				if (!(!srcIsWrite && !dstIsWrite)) needBarrier = true;
			}
			// WAW / RAW on the same buffer with identical masks still needs
			// an execution + memory barrier for ordering/visibility (e.g.
			// the HZB pyramid: every mip pass uses ShaderRead|ShaderWrite
			// @Compute on the same VkBuffer; without this, mip N+1 reads
			// unfinished mip N -> stale near -> over-cull on fast camera
			// moves, exposed in Release where validation adds no
			// serialization). Mirrors the image path's WAW clause above.
			else if (srcIsWrite && dstIsWrite) {
				needBarrier = true;
			}
			// First use from None with no access – still may need barrier if dst is read after write globally, but local None -> need barrier if dst is read? For buffer from Undefined, we still need barrier if dst access != None
			if (src.masks.accessMask == AccessFlags::None && req.barrierMasks.accessMask != AccessFlags::None) needBarrier = true;

			if (needBarrier) {
				auto* buf = vctx.buffers_.get(h);
				if (!buf || buf->vkBuffer == VK_NULL_HANDLE) continue;
				VkBufferMemoryBarrier2 b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
				b.srcStageMask = ToVkStage(src.masks.stageMask);
				b.srcAccessMask = ToVkAccessLocal(src.masks.accessMask);
				b.dstStageMask = ToVkStage(req.barrierMasks.stageMask);
				b.dstAccessMask = ToVkAccessLocal(req.barrierMasks.accessMask);
				b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.buffer = buf->vkBuffer;
				b.offset = 0;
				b.size = VK_WHOLE_SIZE;
				bufBarriers.push_back(b);
			}
			VulkanResourceManager::BufferGlobalSyncState dstState{};
			dstState.masks = req.barrierMasks;
			localBuffer[k] = dstState;
		}
	}
	if (!imgBarriers.empty() || !bufBarriers.empty()) {
		VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dep.imageMemoryBarrierCount = static_cast<uint32_t>(imgBarriers.size());
		dep.pImageMemoryBarriers = imgBarriers.empty() ? nullptr : imgBarriers.data();
		dep.bufferMemoryBarrierCount = static_cast<uint32_t>(bufBarriers.size());
		dep.pBufferMemoryBarriers = bufBarriers.empty() ? nullptr : bufBarriers.data();
		vkCmdPipelineBarrier2(vkCmd, &dep);
	}
	// Commit local states back to global tables (under unique lock)
	vctx.CommitImageSyncStates(localImage);
	vctx.CommitBufferSyncStates(localBuffer);
}
// ---------------------------------------------------------------------------
// RenderGraph barrier debug tracing — see the helpers next to ToVkStage.
// ---------------------------------------------------------------------------
// GPU stage-timer context (see GpuStageTimer in smvk.h): built per frame
// from the swapchain's frame-timer pools (the stage pairs ride queries
// 2+2s / 3+2s of the SAME pools), handed to every per-pass CommandBuffer
// (raster stages write through CommandBuffer::WriteStageTimestamp) and
// used natively for the async-stream stage around the C chain. Null =
// timestamps unsupported (the stage timers stay inert all frame).
struct StageTimerCtx {
	VkQueryPool pool = VK_NULL_HANDLE;
	bool supported = false;
};
void smvk::RenderGraph::Execute(Framebuffer& fb)
{
	// The flight this Execute records for: every pass's CommandBuffer binds
	// defaultDescriptorSets_[flightSlot_] (see BindRenderPipeline /
	// BindComputePipeline - the bindless sets are replicated per frame, each
	// permanently pointing at its own EngineGlobals slice).
	const uint32_t flight = fb.frameIndex % MAX_FRAMES_IN_FLIGHT;
	flightSlot_ = flight;

	ResolveImages();
	ResolveBuffers();

	// Design B compile (see the transient-heap contract on the class):
	// the FIRST usages+sort feed the interval analysis; a layout change
	// detonates + recreates the packed set (fresh handles), so the
	// usages/sort/snapshot the frame proceeds on are recomputed after.
	// Steady state (unchanged fingerprint): the analysis pass only - the
	// second collect+sort is skipped.
	auto usages = CollectPassUsages();
	const std::vector<size_t>* sortedPtr = &TopologicalSort(usages);
	if (CompileTransientHeap(usages, *sortedPtr)) {
		usages = CollectPassUsages();
		sortedPtr = &TopologicalSort(usages);
	}
	const std::vector<size_t>& sorted = *sortedPtr;

	// Views resolve AFTER the transient pack (the 03_fuzz DEVICE_LOST
	// fix): CompileTransientHeap BINDS the heap images to their packed
	// ranges here, and the depth-target contract (depth.h, the working
	// heap path since the memory rework) creates every view only AFTER
	// the bind. The old order resolved views over UNBOUND heap images
	// during the first pack (ResolveImageViews above) — a render pass
	// then touched the pre-bind view and the device lost on the first
	// submit (deterministic, 03_fuzz FUZZ_MODE=graph). CollectPassUsages
	// only reads the logical views' backing IMAGES (never the resolved
	// view handles), so the usages+sort above do not need views; the
	// recording below does — and now gets post-bind views only.
	ResolveImageViews();

	VkCommandBuffer cmd =
		reinterpret_cast<VkCommandBuffer>(fb.commandBufferOpaque);
	VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
	auto imageStates = vctx.SnapshotImageSyncStates();
	auto bufferStates = vctx.SnapshotBufferSyncStates();

	StageTimerCtx stageTimerCtx{};
	if (fb.swapchain) {
		auto* stSwap = static_cast<VulkanSwapchain*>(fb.swapchain->GetVulkanOpaque());
		if (stSwap && stSwap->StageTimersSupported()) {
			stageTimerCtx.pool = stSwap->TimestampPool(fb.frameIndex);
			stageTimerCtx.supported = stageTimerCtx.pool != VK_NULL_HANDLE;
		}
	}
	void* const stageTimerCtxPtr =
		stageTimerCtx.supported ? &stageTimerCtx : nullptr;

	// One GPU context per command buffer: every pass scope below (and any
	// GPU event emitted inside a recording lambda) writes its timestamp
	// pairs into this context.
	SMVK_PROFILE_GPU_CONTEXT(cmd);

	// ---- True-async-compute split (EVSM prefilter, SameFrame) ----
	// AsyncCompute-marked passes run on the dedicated compute queue while
	// graphics continues: the frame submits G1 (graphics, through the
	// moment passes) -> C (compute blur) -> G2 (graphics, lighting onward),
	// chained with per-flight timeline semaphores (submission order makes
	// every wait signal-before-wait legal). Shared images are CONCURRENT
	// (see concurrentCompute), so no ownership transfers — plain barriers
	// on each CB plus semaphore stages order the queues.
	// Fallback (single queue, no async passes, or interleaved graphics
	// writes — see the guard below) keeps today's exact behavior: one
	// graphics submit with async passes inline (emulated).
	const uint32_t graphicsFam = vctx.core_->indices.graphicsFamily.value_or(0);
	const uint32_t computeFam = vctx.core_->indices.computeFamily.value_or(graphicsFam);
	const bool hasDedicatedCompute = (computeFam != graphicsFam);
	size_t firstAsyncPos = SIZE_MAX, lastAsyncPos = 0;
	bool anyAsync = false;
	uint32_t asyncPassCount = 0;
	for (size_t pos = 0; pos < sorted.size(); ++pos) {
		if (passes_[sorted[pos]].IsAsyncCompute()) {
			anyAsync = true;
			++asyncPassCount;
			firstAsyncPos = std::min(firstAsyncPos, pos);
			lastAsyncPos = std::max(lastAsyncPos, pos);
		}
	}
	bool useAsyncCompute = false;
	VulkanSwapchain* vkSwap = nullptr;
	if (RgAsyncSplitKilled()) {
		// One-time notice: the split stays off for the whole process, but
		// the trace header below still reports emulatedAsync per frame.
		static bool s_killNoticed = false;
		if (!s_killNoticed) {
			s_killNoticed = true;
			std::fprintf(stderr, "[AsyncCompute] split disabled (TOURMALINE_ASYNC_COMPUTE=0): async passes record inline on the graphics queue\n");
		}
	}
	else if (hasDedicatedCompute && anyAsync && fb.swapchain)
		vkSwap = static_cast<VulkanSwapchain*>(fb.swapchain->GetVulkanOpaque());
	if (vkSwap && !vkSwap->EnsureAsyncSplit(flight)) {
		// No dedicated compute after all (or resource failure): emulate.
		vkSwap = nullptr;
	}
	if (vkSwap) {
		// Guard: a graphics pass strictly inside the async window must not
		// WRITE a resource the async passes touch (it would record into G2
		// but execute... G2 executes after C, while the async reader may
		// need the pre-write contents — unrepresentable in 3 submits).
		// Reads are safe (topo sorts readers after all writers). Images AND
		// buffers: the async pass set may read either kind, and a same-frame
		// graphics writer inside the window would be ordered after C by
		// submission but before it by the sort — a contradiction, so the
		// split is refused. EVSM satisfies this (blur window touches
		// moments/temp/blurred images and no buffers).
		auto keyForGuard = [](auto h) { return (static_cast<uint64_t>(h.index()) | (static_cast<uint64_t>(h.gen()) << 32)); };
		std::unordered_set<uint64_t> asyncImages;
		std::unordered_set<uint64_t> asyncBuffers;
		for (size_t pos = firstAsyncPos; pos <= lastAsyncPos; ++pos) {
			if (!passes_[sorted[pos]].IsAsyncCompute())
				continue;
			for (auto& pr : usages[sorted[pos]].images)
				asyncImages.insert(keyForGuard(pr.first));
			for (auto& pr : usages[sorted[pos]].buffers)
				asyncBuffers.insert(keyForGuard(pr.first));
		}
		bool overlap = false;
		for (size_t pos = firstAsyncPos + 1; pos < lastAsyncPos && !overlap; ++pos) {
			const RenderPass& p = passes_[sorted[pos]];
			// Skip only the ASYNC passes themselves (they are the reference
			// set recorded into C). ORDINARY compute passes inside the
			// window record into G2 exactly like graphics passes - a write
			// of an async-touched resource is the same unrepresentable
			// ordering (sort says before-C, G2 submission says after-C), so
			// they must be checked too. The old `|| p.IsCompute()` let
			// those writers slip through the guard.
			if (p.IsAsyncCompute())
				continue;
			for (auto& pr : usages[sorted[pos]].images) {
				if (asyncImages.count(keyForGuard(pr.first))
					&& IsWriteAccess(pr.second.barrierMasks.accessMask)) {
					overlap = true;
					break;
				}
			}
			if (overlap)
				break;
			for (auto& pr : usages[sorted[pos]].buffers) {
				if (asyncBuffers.count(keyForGuard(pr.first))
					&& IsWriteAccess(pr.second.barrierMasks.accessMask)) {
					overlap = true;
					break;
				}
			}
		}
		useAsyncCompute = !overlap;
		if (!useAsyncCompute)
			vkSwap = nullptr;
	}
	// Per-pass C command buffers (timeline-chained submits — see the
	// compute-pass arm below): grow the per-flight pool to this frame's
	// async pass count. Growth failure degrades this frame to the emulated
	// single-queue path (correct, just not overlapped).
	if (useAsyncCompute
		&& !vkSwap->EnsureAsyncPassBuffers(flight, asyncPassCount)) {
		vkSwap = nullptr;
		useAsyncCompute = false;
	}
	// ---- Barrier debug tracing (TOURMALINE_BARRIER_TRACE, see helpers) ----
	const bool traceRg = RgTraceEnabled(fb.frameIndex);
	if (traceRg) {
		std::fprintf(stderr, "[RG] Execute frame=%u flight=%u passes=%zu asyncSplit=%s",
			fb.frameIndex, flight, sorted.size(), useAsyncCompute ? "on" : "off");
		if (useAsyncCompute)
			std::fprintf(stderr, " window=[%zu..%zu]", firstAsyncPos, lastAsyncPos);
		if (anyAsync && !useAsyncCompute)
			std::fprintf(stderr, " emulatedAsync=1");
		bool reordered = false;
		for (size_t i = 0; i < sorted.size(); ++i)
			if (sorted[i] != i) { reordered = true; break; }
		if (reordered) {
			std::fprintf(stderr, " SORTED!=INSERTION\n[RG] sorted order:");
			for (size_t i = 0; i < sorted.size(); ++i)
				std::fprintf(stderr, " %zu->%zu", i, sorted[i]);
		}
		std::fprintf(stderr, "\n");
	}
	VkCommandBuffer splitCB = VK_NULL_HANDLE; // G2
	uint64_t g2cVal = 0, c2gVal = 0;
	uint32_t asyncChainIdx = 0; // async pass index -> per-pass CB + timeline link
	if (useAsyncCompute) {
		assert(vkSwap);
		splitCB = vkSwap->splitBuffers[flight];
		assert(splitCB != VK_NULL_HANDLE);
		// The C stream is one CB + submit PER ASYNC PASS, begun in the walk
		// below and timeline-chained (see the compute-pass arm) — no single
		// whole-C CB anymore. The LAST async pass carries the C2G handoff,
		// so hoist its value here.
		c2gVal = ++vkSwap->asyncC2GValue_[flight];
		VkCommandBufferBeginInfo beginCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginCI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(splitCB, &beginCI));
		// NOTE: the Optick GPU context is rebound per pass inside the walk
		// (SMVK_PROFILE_GPU_CONTEXT(dstCmd)) so pass scopes timestamp the
		// CB they record into — without it, C/G2 scopes would WriteTimestamp
		// into the already-submitted G1 CB.
	}

	for (size_t pos = 0; pos < sorted.size(); ++pos) {
		const size_t sIdx = sorted[pos];
		// Split routing (computed before the profiler scope: Optick GPU
		// events emit WriteTimestamp into the thread-local context CB, so
		// the context must follow the recording CB — after G1 is ended +
		// submitted, scopes for C/G2 passes would otherwise timestamp the
		// dead G1 CB).
		const bool passIsAsync = useAsyncCompute && passes_[sIdx].IsAsyncCompute();
		VkCommandBuffer dstCmd = cmd;
		if (useAsyncCompute) {
			if (pos < firstAsyncPos)
				dstCmd = cmd;
			else if (passIsAsync) {
				// This pass's own C-stream CB (begun here, ended + submitted
				// timeline-chained in the compute-pass arm below).
				assert(vkSwap && asyncChainIdx < vkSwap->asyncPassBuffers[flight].size());
				dstCmd = vkSwap->asyncPassBuffers[flight][asyncChainIdx];
				assert(dstCmd != VK_NULL_HANDLE);
				VkCommandBufferBeginInfo beginCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
				beginCI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				VK_CHECK(vkBeginCommandBuffer(dstCmd, &beginCI));
				// GPU stage timer, async-stream stage (GpuStageTimer): begin
				// at the FIRST C-stream CB's start — the whole chained blur
				// stream is one stage (the timeline chain serializes the
				// passes, so first-CB-start..last-CB-end IS its GPU cost).
				if (asyncChainIdx == 0 && stageTimerCtxPtr != nullptr)
					vkCmdWriteTimestamp(dstCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
						stageTimerCtx.pool, 2 + 2 * kGpuStageEvsmBlur);
			}
			else
				dstCmd = splitCB;
		}
		SMVK_PROFILE_GPU_CONTEXT(dstCmd);
		// Pass-level diagnostic checkpoint (VK_NV_device_diagnostic_checkpoints,
		// see RgEmitPassCheckpoint): marks this pass's slot in dstCmd so a
		// DEVICE_LOST dump names the last-reached pass per queue — the fault
		// pinpoint (fires ahead of the barriers + the pass recording; its
		// sorted pos + stream make the returned marker unambiguous).
		RgEmitPassCheckpoint(dstCmd, fb.frameIndex, static_cast<uint32_t>(pos),
			passes_[sIdx].name_);
		// Per-pass CPU+GPU scope covering barriers + recording. RenderPass
		// names may be null (default ctor); Optick hashes the name and a
		// null pointer crashes the profiler, so substitute a stable literal.
		// The Optick Vulkan backend timestamps GPU events with
		// ALL_GRAPHICS stage, which is illegal on the compute queue —
		// async passes get a CPU-only scope (record time still profiled).
#if USE_OPTICK
		const char* passScopeName =
			passes_[sIdx].name_ ? passes_[sIdx].name_ : "UnnamedRenderPass";
		std::optional<::smvk::profiler::GpuPassScope> passGpuScope;
		std::optional<::Optick::Event> passCpuOnlyScope;
		if (passIsAsync)
			passCpuOnlyScope.emplace(
				*::Optick::EventDescription::CreateShared(passScopeName, __FILE__, __LINE__));
		else
			passGpuScope.emplace(passScopeName, __FILE__, __LINE__);
#else
		::smvk::profiler::GpuPassScope passGpuScope(
			passes_[sIdx].name_ ? passes_[sIdx].name_ : "UnnamedRenderPass",
			__FILE__, __LINE__);
#endif
		const uint32_t passQFam = passIsAsync ? computeFam : graphicsFam;
		// G1 -> C boundary: G1 is fully recorded here. Flush uploads first,
		// then end + submit G1 signaling the G1->C timeline. C and G2 chain
		// from it. G1 carries the swapchain-image acquire wait (its CB holds
		// the Acquire layout barrier) and the upload-batch timeline wait:
		// per spec, submissions to one queue only respect submission (begin)
		// order and may overlap, so both dependencies must be explicit
		// semaphore waits — G1's passes must not read upload targets or the
		// swapchain image while an earlier submission is still in flight.
		// G2 waits G1's G2C signal explicitly (splitWaitSemaphore2, set
		// below) for the same reason; EndAndPresent must NOT wait on the
		// binary acquire semaphore again (single wait per signal).
		if (useAsyncCompute && pos == firstAsyncPos) {
			vctx.FlushUploads();
			VK_CHECK(vkEndCommandBuffer(cmd));
			assert(vkSwap);
			g2cVal = ++vkSwap->asyncG2CValue_[flight];
			VkSemaphoreSubmitInfo g1wait[3]{};
			uint32_t g1WaitCount = 0;
			{
				g1wait[g1WaitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
				g1wait[g1WaitCount].semaphore = vkSwap->imageAvailableSemaphores_[flight];
				g1wait[g1WaitCount].value = 0;
				g1wait[g1WaitCount].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
				++g1WaitCount;
			}
			// Upload batches (frame uniforms, zero fills, seeds) -> G1.
			const TimelinePoint g1Uploads =
				vctx.GetStreamer() ? vctx.GetStreamer()->lastTransferPoint()
				: TimelinePoint{};
			if (g1Uploads.valid()) {
				g1wait[g1WaitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
				g1wait[g1WaitCount].semaphore = g1Uploads.semaphore;
				g1wait[g1WaitCount].value = g1Uploads.value;
				g1wait[g1WaitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				++g1WaitCount;
			}
			// Previous frame -> this frame. Consecutive frames share every
			// transient image (hdr, depth, SSR/volumetric targets): without
			// this wait, frame N's Main writing hdr overlaps frame N-1's
			// Tonemap reading it (per spec, same-queue submissions only
			// respect submission BEGIN order). The per-slot host wait in
			// Acquire covers N vs N-2 only; this closes N vs N-1 by
			// construction. GPU frames serialize; CPU-side recording and
			// submission still pipeline (MAX_FRAMES_IN_FLIGHT keeps its CPU
			// overlap value). timelineValue_ here still holds the previous
			// EndAndPresent's signal — this frame has not incremented it.
			if (vkSwap->timelineValue_ != 0) {
				g1wait[g1WaitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
				g1wait[g1WaitCount].semaphore = vkSwap->timelineSemaphore_;
				g1wait[g1WaitCount].value = vkSwap->timelineValue_;
				g1wait[g1WaitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				++g1WaitCount;
			}
			VkSemaphoreSubmitInfo g1sig{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
			g1sig.semaphore = vkSwap->asyncG2C_[flight];
			g1sig.value = g2cVal;
			g1sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			VkCommandBufferSubmitInfo g1cb{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
			g1cb.commandBuffer = cmd;
			VkSubmitInfo2 g1si{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
			g1si.waitSemaphoreInfoCount = g1WaitCount;
			g1si.pWaitSemaphoreInfos = g1wait;
			g1si.commandBufferInfoCount = 1;
			g1si.pCommandBufferInfos = &g1cb;
			g1si.signalSemaphoreInfoCount = 1;
			g1si.pSignalSemaphoreInfos = &g1sig;
			{
				std::mutex& mg = GetQueueMutex(graphicsQueue_);
				std::unique_lock<std::mutex> lockG(mg);
				VK_CHECK(vkQueueSubmit2(graphicsQueue_, 1, &g1si, VK_NULL_HANDLE));
			}
			if (traceRg)
				std::fprintf(stderr, "[RG] G1 submitted (waits acquire%s%s, signals G2C val=%llu)\n",
					g1Uploads.valid() ? " +uploadTimeline" : "",
					vkSwap->timelineValue_ != 0 ? " +prevFrame" : "",
					(unsigned long long)g2cVal);
			RgSerializeGate(RgSerializeMode().split, graphicsQueue_, GetQueueMutex(graphicsQueue_));
		}
		// Per-pass barriers (fix #1) – update imageStates/bufferStates as we go
		{
			if (traceRg) {
				const char* streamName = !useAsyncCompute ? "MAIN"
					: (passIsAsync ? "C" : (dstCmd == splitCB ? "G2" : "G1"));
				std::fprintf(stderr, "[RG] pass pos=%zu idx=%zu '%s' stream=%s%s%s\n",
					pos, sIdx, RgTracePassName(passes_[sIdx].name_), streamName,
					passes_[sIdx].IsCompute() ? " compute" : "",
					pos != sIdx ? " MOVED" : "");
			}
			const auto& pu = usages[sIdx];
			auto imgBarriers = MakeFrameScratchVec<VkImageMemoryBarrier2>();
			auto bufBarriers = MakeFrameScratchVec<VkBufferMemoryBarrier2>();
			auto imgBarrierSrcQ = MakeFrameScratchVec<uint32_t>();
			auto bufBarrierSrcQ = MakeFrameScratchVec<uint32_t>();
			imgBarriers.reserve(4);
			bufBarriers.reserve(4);
			imgBarrierSrcQ.reserve(4);
			bufBarrierSrcQ.reserve(4);
			auto keyFor = [](auto h) { return (static_cast<uint64_t>(h.index()) | (static_cast<uint64_t>(h.gen()) << 32)); };
			for (auto& pr : pu.images) {
				ImageHandle h = pr.first;
				ImageSyncRequest req = pr.second;
				uint64_t k = keyFor(h);
				auto it = imageStates.find(k);
				VulkanResourceManager::ImageGlobalSyncState src{};
				if (it != imageStates.end()) src = it->second;
				else { src.masks = { AccessFlags::None, PipelineStageBit_None }; src.layout = ImageLayout::Undefined; }
				bool srcIsWrite = IsWriteAccess(src.masks.accessMask);
				bool dstIsWrite = IsWriteAccess(req.barrierMasks.accessMask);
				bool needBarrier = false;
				if (src.layout != req.layout) needBarrier = true;
				else if (src.masks.accessMask != req.barrierMasks.accessMask || src.masks.stageMask != req.barrierMasks.stageMask) {
					if (!(!srcIsWrite && !dstIsWrite)) needBarrier = true;
				}
				else if (srcIsWrite && dstIsWrite) needBarrier = true;
				if (src.layout == ImageLayout::Undefined && req.layout != ImageLayout::Undefined) needBarrier = true;
				if (traceRg) {
					auto* dbgImg = vctx.images_.get(h);
					std::fprintf(stderr,
						"[RG]   img '%s' need=%d src[%s acc=%06x stage=%06x qfam=%u] dst[%s acc=%06x stage=%06x]\n",
						(dbgImg && dbgImg->name.valid()) ? dbgImg->name.data() : "<anon>",
						needBarrier ? 1 : 0,
						RgTraceImageLayoutName(src.layout),
						static_cast<uint32_t>(src.masks.accessMask),
						static_cast<uint32_t>(src.masks.stageMask),
						src.queueFamily,
						RgTraceImageLayoutName(req.layout),
						static_cast<uint32_t>(req.barrierMasks.accessMask),
						static_cast<uint32_t>(req.barrierMasks.stageMask));
				}
				if (needBarrier) {
					auto* img = vctx.images_.get(h);
					if (!img || img->vkImage == VK_NULL_HANDLE) {
						if (traceRg)
							std::fprintf(stderr, "[RG]   img '%s' SKIPPED-BARRIER (no VkImage backing)\n",
								(img && img->name.valid()) ? img->name.data() : "<anon>");
						continue;
					}
					VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
					b.srcStageMask = ToVkStage(src.masks.stageMask);
					if (src.layout == ImageLayout::Undefined) b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
					b.srcAccessMask = ToVkAccessLocal(src.masks.accessMask);
					if (src.layout == ImageLayout::Undefined) b.srcAccessMask = VK_ACCESS_2_NONE;
					b.dstStageMask = ToVkStage(req.barrierMasks.stageMask);
					if (req.layout == ImageLayout::ColorAttachmentOptimal) b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
					else if (req.layout == ImageLayout::DepthStencilAttachmentOptimal) b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
					b.dstAccessMask = ToVkAccessLocal(req.barrierMasks.accessMask);
					b.oldLayout = ToVkImageLayout(src.layout);
					b.newLayout = ToVkImageLayout(req.layout);
					b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					b.image = img->vkImage;
					VkFormat fmt = toVkFormat(img->format);
					VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
					if (fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
					else if (fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
					else if (fmt == VK_FORMAT_S8_UINT) aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
					b.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
					imgBarriers.push_back(b);
					if (useAsyncCompute) {
						const uint32_t srcQ = (src.queueFamily == UINT32_MAX)
							? graphicsFam : src.queueFamily;
						imgBarrierSrcQ.push_back(srcQ);
					}
					if (traceRg) {
						const uint32_t traceSrcQ = (src.queueFamily == UINT32_MAX)
							? graphicsFam : src.queueFamily;
						const bool crossQ = useAsyncCompute && traceSrcQ != passQFam;
						std::fprintf(stderr,
							"[RG]     barrier img '%s' old=%s new=%s srcStage=%llx srcAcc=%llx dstStage=%llx dstAcc=%llx%s\n",
							img->name.valid() ? img->name.data() : "<anon>",
							RgTraceVkImageLayoutName(b.oldLayout),
							RgTraceVkImageLayoutName(b.newLayout),
							(unsigned long long)b.srcStageMask,
							(unsigned long long)b.srcAccessMask,
							(unsigned long long)b.dstStageMask,
							(unsigned long long)b.dstAccessMask,
							crossQ ? " (crossQ: srcStage rewritten to ALL_COMMANDS at emit)" : "");
					}
				}
				VulkanResourceManager::ImageGlobalSyncState dstState{};
				dstState.masks = req.barrierMasks;
				dstState.layout = req.layout;
				dstState.queueFamily = passQFam;
				imageStates[k] = dstState;
			}
			for (auto& pr : pu.buffers) {
				BufferHandle h = pr.first;
				BufferSyncRequest req = pr.second;
				uint64_t k = keyFor(h);
				auto it = bufferStates.find(k);
				VulkanResourceManager::BufferGlobalSyncState src{};
				if (it != bufferStates.end()) src = it->second;
				bool srcIsWrite = IsWriteAccess(src.masks.accessMask);
				bool dstIsWrite = IsWriteAccess(req.barrierMasks.accessMask);
				bool needBarrier = false;
				if (src.masks.accessMask != req.barrierMasks.accessMask || src.masks.stageMask != req.barrierMasks.stageMask) {
					if (!(!srcIsWrite && !dstIsWrite)) needBarrier = true;
				}
				// Same-mask WAW still needs a barrier (see EmitBarriers
				// above: HZB mip chain reuses ShaderRead|ShaderWrite
				// @Compute on one buffer; mip N+1 must see mip N).
				else if (srcIsWrite && dstIsWrite) {
					needBarrier = true;
				}
				if (src.masks.accessMask == AccessFlags::None && req.barrierMasks.accessMask != AccessFlags::None) needBarrier = true;
				if (traceRg)
					std::fprintf(stderr,
						"[RG]   buf h=%u:%u need=%d src[acc=%06x stage=%06x] dst[acc=%06x stage=%06x]\n",
						h.index(), h.gen(), needBarrier ? 1 : 0,
						static_cast<uint32_t>(src.masks.accessMask),
						static_cast<uint32_t>(src.masks.stageMask),
						static_cast<uint32_t>(req.barrierMasks.accessMask),
						static_cast<uint32_t>(req.barrierMasks.stageMask));
				if (needBarrier) {
					auto* buf = vctx.buffers_.get(h);
					if (!buf || buf->vkBuffer == VK_NULL_HANDLE) {
						if (traceRg)
							std::fprintf(stderr, "[RG]   buf h=%u:%u SKIPPED-BARRIER (no VkBuffer backing)\n",
								h.index(), h.gen());
						continue;
					}
					VkBufferMemoryBarrier2 b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
					b.srcStageMask = ToVkStage(src.masks.stageMask);
					b.srcAccessMask = ToVkAccessLocal(src.masks.accessMask);
					b.dstStageMask = ToVkStage(req.barrierMasks.stageMask);
					b.dstAccessMask = ToVkAccessLocal(req.barrierMasks.accessMask);
					b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					b.buffer = buf->vkBuffer;
					b.offset = 0;
					b.size = VK_WHOLE_SIZE;
					bufBarriers.push_back(b);
					if (useAsyncCompute) {
						const uint32_t srcQ = (src.queueFamily == UINT32_MAX)
							? graphicsFam : src.queueFamily;
						bufBarrierSrcQ.push_back(srcQ);
					}
					if (traceRg)
						std::fprintf(stderr,
							"[RG]     barrier buf h=%u:%u srcStage=%llx srcAcc=%llx dstStage=%llx dstAcc=%llx\n",
							h.index(), h.gen(),
							(unsigned long long)b.srcStageMask,
							(unsigned long long)b.srcAccessMask,
							(unsigned long long)b.dstStageMask,
							(unsigned long long)b.dstAccessMask);
				}
				VulkanResourceManager::BufferGlobalSyncState dstState{};
				dstState.masks = req.barrierMasks;
				dstState.queueFamily = passQFam;
				bufferStates[k] = dstState;
			}
			if (!imgBarriers.empty() || !bufBarriers.empty()) {
				// Cross-CB ordering comes from the submit chain
				// (G1 -> C -> G2 timeline semaphores); each CB needs only
				// its plain same-stream barriers here, with src scopes
				// matching the chained signal stages for visibility.
				// Cross-queue legality, BOTH directions: a barrier handed
				// off between queues carries the writer's src stages
				// (fragment/attachment output from graphics; compute shader
				// from the async family) in the reader's command buffer.
				// Those stage bits may be unsupported by the reading queue's
				// family, and the writer's commands are not in this CB
				// anyway — the cross-queue semaphore wait already ordered
				// execution and made the writes available, so such barriers
				// degrade to availability-free acquires: srcStage=
				// ALL_COMMANDS, srcAccess=NONE, dst = consumer scope.
				// Same-stream barriers (G2 compute->G2 fragment chains like
				// the HZB mips, or async-pass internal chains) keep their
				// precise scopes.
				if (useAsyncCompute) {
					assert(imgBarrierSrcQ.size() == imgBarriers.size());
					for (size_t bi = 0; bi < imgBarriers.size(); ++bi) {
						if (imgBarrierSrcQ[bi] != passQFam) {
							imgBarriers[bi].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
							imgBarriers[bi].srcAccessMask = VK_ACCESS_2_NONE;
							if (traceRg)
								std::fprintf(stderr,
									"[RG]     rewrite img barrier[%zu]: srcStage->ALL_COMMANDS srcAcc->NONE (qfam %u -> %u handoff)\n",
									bi, imgBarrierSrcQ[bi], passQFam);
						}
					}
					assert(bufBarrierSrcQ.size() == bufBarriers.size());
					for (size_t bi = 0; bi < bufBarriers.size(); ++bi) {
						if (bufBarrierSrcQ[bi] != passQFam) {
							bufBarriers[bi].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
							bufBarriers[bi].srcAccessMask = VK_ACCESS_2_NONE;
							if (traceRg)
								std::fprintf(stderr,
									"[RG]     rewrite buf barrier[%zu]: srcStage->ALL_COMMANDS srcAcc->NONE (qfam %u -> %u handoff)\n",
									bi, bufBarrierSrcQ[bi], passQFam);
						}
					}
				}
				VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				dep.imageMemoryBarrierCount = static_cast<uint32_t>(imgBarriers.size());
				dep.pImageMemoryBarriers = imgBarriers.empty() ? nullptr : imgBarriers.data();
				dep.bufferMemoryBarrierCount = static_cast<uint32_t>(bufBarriers.size());
				dep.pBufferMemoryBarriers = bufBarriers.empty() ? nullptr : bufBarriers.data();
				vkCmdPipelineBarrier2(dstCmd, &dep);
			}
		}
		auto& pass = passes_[sIdx];
		std::vector<VkRenderingAttachmentInfo> colorInfos;
		colorInfos.reserve(pass.colorAttachments_.size());
		for (auto& att : pass.colorAttachments_)
		{
			VkRenderingAttachmentInfo info{ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, };
			info.imageView = GetImageView(this, att.view);
			info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			info.loadOp = toVkAttachmentLoadOp(att.loadOp);
			info.storeOp = toVkAttachmentStoreOp(att.storeOp);
			info.clearValue = toVkClearValue(att.clearColor);
			if (!att.resolveView.empty()) {
				info.resolveImageView = GetImageView(this, att.resolveView);
				info.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
				info.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			}
			colorInfos.push_back(info);
		}
		VkRenderingAttachmentInfo depthInfo{ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, };
		const bool hasDepth = pass.hasDepthAttachment_;
		bool hasStencil = false;
		if (hasDepth)
		{
			const auto depthFormat = GetResolvedImageViewFormat(pass.depthAttachment_.view);
			hasStencil = FormatHasStencil(toVkFormat(depthFormat));
			depthInfo.imageView = GetImageView(this, pass.depthAttachment_.view);
			depthInfo.imageLayout = hasStencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
			depthInfo.loadOp = toVkAttachmentLoadOp(pass.depthAttachment_.loadOp);
			depthInfo.storeOp = toVkAttachmentStoreOp(pass.depthAttachment_.storeOp);
			depthInfo.clearValue = toVkClearValue(pass.depthAttachment_.clear);
		}
		VkOffset2D renderOffset{ pass.offsetX_, pass.offsetY_ };
		VkExtent2D renderExtent{ pass.extent_.width, pass.extent_.height };
		// Depth-only tile passes (shadow atlases) declare a sub-rect pass
		// area but must not run with a sub-rect *render area*: many drivers
		// take a severe slow path when the render area does not cover the
		// whole attachment (measured ~10 ms/pass on RTX 5070 Ti). Instead
		// run the full attachment extent, downgrade the implicit loadOp
		// clear to LOAD, and clear just the tile explicitly with
		// vkCmdClearAttachments (its rect is clipped to the render area +
		// current scissor, both of which cover exactly the tile).
		// Tile-local clear, full-area render.
		bool depthTileClear = false;
		if (hasDepth && colorInfos.empty() &&
			pass.depthAttachment_.loadOp == AttachmentLoadOp::Clear)
		{
			ImageHandle phys{};
			auto* view = imageViews_.get(pass.depthAttachment_.view);
			if (view)
			{
				if (view->type == LogicalImageView::Type::ExternalPhysical)
				{
					auto* ext = vctx.imageViews_.get(view->externalViewHandle);
					if (ext) phys = ext->image;
				}
				else
				{
					auto* li = images_.get(view->key.image);
					if (li) phys = li->resolvedImageHandle;
				}
			}
			auto* img = phys.empty() ? nullptr : vctx.images_.get(phys);
			const bool strictSubRect = img == nullptr ||
				renderOffset.x != 0 || renderOffset.y != 0 ||
				renderExtent.width != img->extent.width ||
				renderExtent.height != img->extent.height;
			if (strictSubRect && img && img->extent.width != 0 && img->extent.height != 0 &&
				renderOffset.x >= 0 && renderOffset.y >= 0 &&
				static_cast<uint64_t>(renderOffset.x) + renderExtent.width <= img->extent.width &&
				static_cast<uint64_t>(renderOffset.y) + renderExtent.height <= img->extent.height)
			{
				depthTileClear = true;
				renderOffset = { 0, 0 };
				renderExtent = { img->extent.width, img->extent.height };
				depthInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			}
			// Full-extent passes keep the implicit loadOp clear (fast path).
			// Unresolvable extents keep the pass-area render + implicit clear
			// (correct, only potentially slow on tile passes).
		}
		VkRenderingInfo renderInfo{ .sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, .renderArea = { .offset = renderOffset, .extent = renderExtent, }, .layerCount = 1, .colorAttachmentCount = static_cast<uint32_t>(colorInfos.size()), .pColorAttachments = colorInfos.data(), };
		renderInfo.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
		renderInfo.pStencilAttachment = hasStencil ? &depthInfo : nullptr;
		if (pass.IsCompute()) {
			// Compute pass: no render targets, run recording directly.
			// Barriers above already transitioned hinted images/buffers.
			// Async-marked passes land in the compute-queue CB (true
			// async, split mode); the rest stay on the graphics CBs.
			CommandBuffer cb{ reinterpret_cast<void*>(dstCmd), *this, pass, stageTimerCtxPtr };
			if (pass.recordingFunction_) pass.recordingFunction_(cb);
			if (passIsAsync) {
				// ---- Per-pass timeline-chained C submit ----
				// Simple hard dependency for chained compute tasks (the
				// EVSM blur's H -> temp -> V chain): EVERY async pass
				// ends + submits its own CB, linked on the asyncG2C_
				// timeline — pass k waits g2cVal+k (G1's signal for k=0,
				// pass k-1's chain value after) and signals g2cVal+k+1
				// for pass k+1. The sync2 chain (signal ALL_COMMANDS ->
				// wait COMPUTE_SHADER) is a full execution+availability+
				// visibility dependency, so pass k's storage writes are
				// complete and visible before pass k+1's fetches — the
				// dispatch-adjacent RAW hazard the intra-CB hint
				// barriers could leave uncovered (a V leg racing its H
				// leg's temp stores read stale moments). NOT a
				// rendergraph-wide dependency solve: the per-pass hints
				// keep carrying the layout transitions inside each CB;
				// this chain just makes adjacent async passes strictly
				// sequential, which is all these task chains need. The
				// LAST pass signals the C2G handoff instead of a chain
				// value — identical C2G semantics to the old
				// single-whole-C-C submit, so G2/retirement ordering is
				// unchanged. Submission order (G1, then each C pass in
				// sorted order, same thread) keeps every wait
				// signal-before-wait legal.
				const bool lastAsync = (asyncChainIdx + 1 == asyncPassCount);
				// GPU stage timer, async-stream stage: end at the LAST
				// C-stream CB's end (see the begin at the first CB above).
				if (lastAsync && stageTimerCtxPtr != nullptr)
					vkCmdWriteTimestamp(dstCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
						stageTimerCtx.pool, 3 + 2 * kGpuStageEvsmBlur);
				VK_CHECK(vkEndCommandBuffer(dstCmd));
				VkSemaphoreSubmitInfo pWait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
				pWait.semaphore = vkSwap->asyncG2C_[flight];
				pWait.value = g2cVal + asyncChainIdx;
				pWait.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
				VkSemaphoreSubmitInfo pSig{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
				if (lastAsync) {
					pSig.semaphore = vkSwap->asyncC2G_[flight];
					pSig.value = c2gVal;
				}
				else {
					pSig.semaphore = vkSwap->asyncG2C_[flight];
					pSig.value = g2cVal + asyncChainIdx + 1;
				}
				pSig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				VkCommandBufferSubmitInfo pcb{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
				pcb.commandBuffer = dstCmd;
				VkSubmitInfo2 psi{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
				psi.waitSemaphoreInfoCount = 1;
				psi.pWaitSemaphoreInfos = &pWait;
				psi.commandBufferInfoCount = 1;
				psi.pCommandBufferInfos = &pcb;
				psi.signalSemaphoreInfoCount = 1;
				psi.pSignalSemaphoreInfos = &pSig;
				{
					std::mutex& mc = GetQueueMutex(computeQueue_);
					std::unique_lock<std::mutex> lockC(mc);
					VK_CHECK(vkQueueSubmit2(computeQueue_, 1, &psi, VK_NULL_HANDLE));
				}
				if (traceRg)
					std::fprintf(stderr,
						"[RG] C pass %u/%u '%s' submitted (waits G2C=%llu, signals %s=%llu)\n",
						asyncChainIdx + 1, asyncPassCount,
						RgTracePassName(passes_[sIdx].name_),
						(unsigned long long)pWait.value,
						lastAsync ? "C2G" : "G2C",
						(unsigned long long)pSig.value);
				RgSerializeGate(RgSerializeMode().split, computeQueue_, GetQueueMutex(computeQueue_));
				++asyncChainIdx;
			}
			continue;
		}
		VkViewport vp{}; vp.x = static_cast<float>(pass.offsetX_); vp.y = static_cast<float>(pass.offsetY_); vp.width = static_cast<float>(pass.extent_.width); vp.height = static_cast<float>(pass.extent_.height); vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
		VkRect2D scissor{}; scissor.offset = { pass.offsetX_, pass.offsetY_ }; scissor.extent = { pass.extent_.width, pass.extent_.height };
		vkCmdSetViewport(dstCmd, 0, 1, &vp);
		vkCmdSetScissor(dstCmd, 0, 1, &scissor);
		vkCmdBeginRendering(dstCmd, &renderInfo);
		if (depthTileClear)
		{
			VkClearAttachment clear{};
			clear.aspectMask = hasStencil
				? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
				: VK_IMAGE_ASPECT_DEPTH_BIT;
			clear.clearValue = depthInfo.clearValue;
			VkClearRect rect{};
			rect.rect.offset = { pass.offsetX_, pass.offsetY_ };
			rect.rect.extent = { pass.extent_.width, pass.extent_.height };
			rect.baseArrayLayer = 0;
			rect.layerCount = 1;
			vkCmdClearAttachments(dstCmd, 1, &clear, 1, &rect);
		}
		CommandBuffer cb{ reinterpret_cast<void*>(dstCmd), *this, pass, stageTimerCtxPtr };
		if (pass.recordingFunction_) pass.recordingFunction_(cb);
		vkCmdEndRendering(dstCmd);
	}

	// C-stream bookkeeping: the per-pass submits already happened in the
	// walk above (each async pass's CB ends + submits timeline-chained
	// there; the LAST pass carries the C2G signal G2 waits — identical
	// C2G semantics to the old single whole-C submit). Roll the G2C value
	// counter forward to the highest chain value this frame so next
	// frame's G1 signal stays strictly monotonic, then hand G2 its split
	// CB + wait values. G2 (splitCB) stays open — EndAndPresent ends +
	// submits it (waiting both C2G and G1's G2C). Retirement is
	// structural: G2's submit signals the slot's frame-retirement timeline
	// only after C (waited — transitively the whole chain) and G1
	// (waited) complete, so the Acquire-side timeline wait retires G1, C
	// and G2 together and no host waits are needed here.
	if (useAsyncCompute) {
		assert(vkSwap && splitCB != VK_NULL_HANDLE);
		if (asyncPassCount > 1)
			vkSwap->asyncG2CValue_[flight] = g2cVal + asyncPassCount - 1;
		if (traceRg)
			std::fprintf(stderr, "[RG] C stream done: %u chained pass submits (C2G=%llu)\n",
				asyncPassCount, (unsigned long long)c2gVal);
		fb.splitCommandBufferOpaque = reinterpret_cast<void*>(splitCB);
		fb.splitWaitSemaphoreOpaque = reinterpret_cast<void*>(vkSwap->asyncC2G_[flight]);
		fb.splitWaitValue = c2gVal;
		// G2 also waits G1's own signal: submissions to one queue may
		// overlap per spec, so G2 (Tonemap writing the swapchain image)
		// must be ordered after G1's acquire wait + layout barrier
		// explicitly. Timeline waits don't consume — C waits the same
		// semaphore at its own value, which is legal.
		fb.splitWaitSemaphore2Opaque = reinterpret_cast<void*>(vkSwap->asyncG2C_[flight]);
		fb.splitWaitValue2 = g2cVal;
	}

	vctx.CommitImageSyncStates(imageStates);
	vctx.CommitBufferSyncStates(bufferStates);
}
// CommandBuffer – opaque handle firewall: public header stores void* but impl casts to VkCommandBuffer
smvk::CommandBuffer::CommandBuffer(void* cmdOpaque, smvk::RenderGraph& graph, smvk::RenderPass& pass,
	void* stageTimerOpaque)
	: cmdOpaque_(cmdOpaque)
	, graph_(&graph)
	, renderPass_(&pass)
	, stageTimerOpaque_(stageTimerOpaque)
{}
void smvk::CommandBuffer::WriteStageTimestamp(uint32_t stageTimer, bool begin)
{
	if (stageTimerOpaque_ == nullptr || stageTimer >= kGpuStageCount)
		return;
	auto* ctx = static_cast<StageTimerCtx*>(stageTimerOpaque_);
	if (!ctx || !ctx->supported || ctx->pool == VK_NULL_HANDLE)
		return;
	// BOTTOM for both: begin fires after prior commands (the stage's
	// start), end after the stage's work — the delta is the stage's GPU
	// cost on a serialized frame.
	vkCmdWriteTimestamp(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		ctx->pool,
		2 + 2 * stageTimer + (begin ? 0 : 1));
}
void smvk::CommandBuffer::BindRenderPipeline(
	ShaderProgram& shaderProgram,
	VertexFormat vertexFormat,
	const DepthSettings& depthSettings,
	const StencilSettings& stencilSettings,
	const BlendSettings& blendSettings,
	const MSAASettings& msaaSettings,
	const RasterSettings& rasterSettings,
	PrimitiveTopology topology
)
{
	assert(graph_ != nullptr);
	assert(renderPass_ != nullptr);

	// Fast path: identical input state already bound in this pass.
	// Attachment formats are pass-invariants (CommandBuffer is
	// per-pass), so they are not part of the comparison. Skips the
	// pipeline-cache key build (vector allocs + ~100-way hash) and the
	// redundant vkCmdBindPipeline call that even the cache's own
	// current-pipeline dedup used to pay. The vertex buffer bind below
	// is NOT skipped: with the async-compute split (G1/C/G2) the pass
	// records into the split tail command buffer while this per-pass
	// state was established against earlier binds, so a binding issued
	// once cannot be assumed live at draw time (observed: vertex fetches
	// past binding 0 read zero in split frames without the rebind).
	// Re-issuing two buffer binds is negligible next to the lookup saved.
	if (graphicsBound_
		&& vertexFormat == boundVertexFormat_
		&& topology == boundTopology_
		&& msaaSettings.samples == boundMsaa_.samples
		&& msaaSettings.alphaToCoverage == boundMsaa_.alphaToCoverage
		&& !(depthSettings < boundDepth_) && !(boundDepth_ < depthSettings)
		&& !(stencilSettings < boundStencil_) && !(boundStencil_ < stencilSettings)
		&& !(blendSettings < boundBlend_) && !(boundBlend_ < blendSettings)
		&& !(rasterSettings < boundRaster_) && !(boundRaster_ < rasterSettings)
		&& shaderProgram == boundProgram_)
	{
		currentPipeline_ = graphicsPipeline_;
		currentLayout_ = graphicsLayout_;
		if (vertexFormat == VertexFormat::StaticVertexFormat)
			BindStaticVertexFmt();
		return;
	}

	switch (vertexFormat)
	{
	case VertexFormat::StaticVertexFormat:
		BindStaticVertexFmt();
		break;
	case VertexFormat::Empty:
		break;
	default:
		break;
	}
	const VertexFormatDescription& formatDescription =
		GetVertexFormatDescription(vertexFormat);

	// Attachment formats feed the pipeline cache key. Small fixed
	// storage (passes have few attachments); heap fallback keeps a wide
	// pass correct.
	Format colorFormatsStack[8];
	std::vector<Format> colorFormatsHeap;
	const Format* colorFormats = colorFormatsStack;
	if (renderPass_->colorAttachments_.size() <= 8) {
		uint32_t i = 0;
		for (const auto& attachment : renderPass_->colorAttachments_)
		{
			assert(!attachment.view.empty());
			colorFormatsStack[i++] =
				graph_->GetResolvedImageViewFormat(attachment.view);
		}
	}
	else {
		colorFormatsHeap.reserve(renderPass_->colorAttachments_.size());
		for (const auto& attachment : renderPass_->colorAttachments_)
		{
			colorFormatsHeap.push_back(
				graph_->GetResolvedImageViewFormat(attachment.view));
		}
		colorFormats = colorFormatsHeap.data();
	}
	smvk::Format depthFormat =
		smvk::Format::Undefined;

	smvk::Format stencilFormat =
		smvk::Format::Undefined;

	if (renderPass_->hasDepthAttachment_)
	{
		const auto format =
			graph_->GetResolvedImageViewFormat(
				renderPass_->depthAttachment_.view);

		if (FormatHasDepth(toVkFormat(format)))
			depthFormat = format;

		if (FormatHasStencil(toVkFormat(format)))
			stencilFormat = format;
	}

	VulkanResourceManager& vulkanCtx = *static_cast<VulkanResourceManager*>(graph_->ctx_.GetVulkanOpaque());
	auto* cacheOpaque = static_cast<RenderGraph::PipelineCacheOpaque*>(graph_->pipelineCacheOpaque_.get());
	auto bundle = cacheOpaque->cache.GetRenderPipeline(
		shaderProgram,
		formatDescription,
		colorFormats,
		static_cast<uint32_t>(renderPass_->colorAttachments_.size()),
		depthFormat,
		stencilFormat,
		topology,
		rasterSettings,
		depthSettings,
		stencilSettings,
		blendSettings,
		msaaSettings
	);

	assert(bundle.pipeline != VK_NULL_HANDLE && "Invalid pipeline handle");

	// Remember the input state so later redundant binds take the fast
	// path above.
	graphicsBound_ = true;
	boundProgram_ = shaderProgram;
	boundVertexFormat_ = vertexFormat;
	boundDepth_ = depthSettings;
	boundStencil_ = stencilSettings;
	boundBlend_ = blendSettings;
	boundMsaa_ = msaaSettings;
	boundRaster_ = rasterSettings;
	boundTopology_ = topology;
	graphicsPipeline_ = reinterpret_cast<void*>(bundle.pipeline);
	graphicsLayout_ = reinterpret_cast<void*>(bundle.layout);

	if (bundle.pipeline == reinterpret_cast<VkPipeline>(currentPipeline_)) {
		return;
	}

	currentPipeline_ = reinterpret_cast<void*>(bundle.pipeline);
	currentLayout_ = reinterpret_cast<void*>(bundle.layout);

	vkCmdBindPipeline(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		bundle.pipeline
	);

	// THIS flight's set (see RenderGraph::flightSlot_): the bindless sets are
	// replicated per frame, each permanently pointing at its own
	// EngineGlobals slice - binding by flight keeps in-flight siblings from
	// seeing each other's slice (the old shared set let every in-flight
	// frame read the newest rewrite).
	vkCmdBindDescriptorSets(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		vulkanCtx.defaultPipeLayout,
		0,
		1,
		&vulkanCtx.defaultDescriptorSets_[graph_->flightSlot_],
		0,
		nullptr);
}
void smvk::CommandBuffer::BindStaticVertexFmt()
{
#ifndef N_DEBUG
	assert(staticVertexFmtHeap.m_staticVertexPosBuffer.valid());
	assert(staticVertexFmtHeap.m_staticVertexShadingBuffer.valid());
	assert(staticVertexFmtHeap.m_staticIndexBuffer.valid());
#endif
	VulkanResourceManager& vulkanCtx = *static_cast<VulkanResourceManager*>(graph_->ctx_.GetVulkanOpaque());
	VkBuffer vertexBuffers[] = {
		vulkanCtx.buffers_.get(staticVertexFmtHeap.m_staticVertexPosBuffer)->vkBuffer,
		vulkanCtx.buffers_.get(staticVertexFmtHeap.m_staticVertexShadingBuffer)->vkBuffer
	};

	VkDeviceSize offsets[] = {
		0,
		0
	};

	vkCmdBindVertexBuffers(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		0,
		2,
		vertexBuffers,
		offsets
	);

	vkCmdBindIndexBuffer(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		vulkanCtx.buffers_.get(staticVertexFmtHeap.m_staticIndexBuffer)->vkBuffer,
		0,
		VK_INDEX_TYPE_UINT32
	);
#ifndef NDEBUG
	boundIndexBuffer_ = staticVertexFmtHeap.m_staticIndexBuffer;
#endif
}
void smvk::CommandBuffer::BindVertexBuffer(BufferHandle buffer, uint32_t binding, uint64_t offset)
{
	// Manual vertex rebind: the graphics-bind fast path skips the
	// BindStaticVertexFmt() that BindRenderPipeline would re-issue, so
	// drop the cached state to stay conservative.
	graphicsBound_ = false;
	VulkanResourceManager& vulkanCtx = *static_cast<VulkanResourceManager*>(graph_->ctx_.GetVulkanOpaque());
	auto* b = vulkanCtx.buffers_.get(buffer);
	assert(b && b->vkBuffer != VK_NULL_HANDLE);
	if (!b || b->vkBuffer == VK_NULL_HANDLE)
		return;
	VkBuffer vertexBuffers[] = { b->vkBuffer };
	VkDeviceSize offsets[] = { static_cast<VkDeviceSize>(offset) };
	vkCmdBindVertexBuffers(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		binding,
		1,
		vertexBuffers,
		offsets
	);
}
void smvk::CommandBuffer::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
	vkCmdDraw(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		vertexCount,
		instanceCount,
		firstVertex,
		firstInstance
	);
};
void smvk::CommandBuffer::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
#ifndef NDEBUG
	assert(!boundIndexBuffer_.empty() && "DrawIndexed with no index buffer bound (stale binding across passes?)");
#endif
	vkCmdDrawIndexed(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		indexCount,
		instanceCount,
		firstIndex,
		vertexOffset,
		firstInstance
	);
}
void smvk::CommandBuffer::DrawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	assert(vkCmdDrawMeshTasksEXT != nullptr && "DrawMeshTasks without VK_EXT_mesh_shader (gate on SupportsMeshShaders)");
	vkCmdDrawMeshTasksEXT(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		groupCountX,
		groupCountY,
		groupCountZ
	);
}
void smvk::CommandBuffer::BindIndexBuffer(BufferHandle buffer, uint64_t offset)
{
	// Manual index rebind: see BindVertexBuffer.
	graphicsBound_ = false;
	VulkanResourceManager& vulkanCtx = *static_cast<VulkanResourceManager*>(graph_->ctx_.GetVulkanOpaque());
	const Buffer* desc = vulkanCtx.buffers_.get(buffer);
	assert(desc != nullptr && desc->vkBuffer != VK_NULL_HANDLE);
#ifndef NDEBUG
	boundIndexBuffer_ = buffer;
#endif
	vkCmdBindIndexBuffer(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		desc->vkBuffer,
		static_cast<VkDeviceSize>(offset),
		VK_INDEX_TYPE_UINT32
	);
}
void smvk::CommandBuffer::DispatchIndirect(BufferHandle buffer, uint64_t offset)
{
	VulkanResourceManager& vulkanCtx =
		*static_cast<VulkanResourceManager*>(graph_->ctx_.GetVulkanOpaque());
	const Buffer* desc = vulkanCtx.buffers_.get(buffer);
	assert(desc != nullptr && desc->vkBuffer != VK_NULL_HANDLE);
	VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(cmdOpaque_);
	// Indirect dispatches consume one VkDispatchIndirectCommand (x/y/z
	// group counts); the graph's IndirectCommandRead barrier (declared
	// by the consumer pass) orders the producing compute writes.
	vkCmdDispatchIndirect(
		cmd,
		desc->vkBuffer,
		static_cast<VkDeviceSize>(offset)
	);
}
void smvk::CommandBuffer::EmitGlobalMemoryBarrier()
{
	// The engine-side seam for resources the graph's per-resource sync
	// cannot see (BDA-only heaps). Vulkan: one all-commands barrier —
	// every prior write visible to every later access.
	VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(cmdOpaque_);
	VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT
		| VK_ACCESS_2_MEMORY_WRITE_BIT;
	VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(cmd, &dep);
}
void smvk::CommandBuffer::DrawIndexedIndirect(BufferHandle buffer, uint64_t offset,
	uint32_t drawCount, uint32_t stride)
{
	VulkanResourceManager& vulkanCtx = *static_cast<VulkanResourceManager*>(graph_->ctx_.GetVulkanOpaque());
	const Buffer* desc = vulkanCtx.buffers_.get(buffer);
	assert(desc != nullptr && desc->vkBuffer != VK_NULL_HANDLE);
#ifndef NDEBUG
	assert(!boundIndexBuffer_.empty() && "DrawIndexedIndirect with no index buffer bound (stale binding across passes?)");
#endif
	VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(cmdOpaque_);
	if (g_multiDrawIndirect) {
		vkCmdDrawIndexedIndirect(
			cmd,
			desc->vkBuffer,
			static_cast<VkDeviceSize>(offset),
			drawCount,
			stride
		);
	}
	else {
		// Slow-path emulation: without multiDrawIndirect, drawCount must
		// be 0 or 1 per call (VUID-vkCmdDrawIndexedIndirect-drawCount-02718).
		for (uint32_t i = 0; i < drawCount; ++i) {
			vkCmdDrawIndexedIndirect(
				cmd,
				desc->vkBuffer,
				static_cast<VkDeviceSize>(offset) + (VkDeviceSize)i * stride,
				1,
				stride
			);
		}
	}
}
void smvk::CommandBuffer::BindComputePipeline(
	ShaderProgram& shaderProgram)
{
	assert(graph_ != nullptr);
	assert(renderPass_ != nullptr);

	VulkanResourceManager& vulkanCtx = *static_cast<VulkanResourceManager*>(graph_->ctx_.GetVulkanOpaque());
	auto* cacheOpaque = static_cast<RenderGraph::PipelineCacheOpaque*>(graph_->pipelineCacheOpaque_.get());
	auto bundle = cacheOpaque->cache.GetComputePipeline(shaderProgram);

	assert(bundle.pipeline != VK_NULL_HANDLE && "Invalid compute pipeline handle");

	if (bundle.pipeline == reinterpret_cast<VkPipeline>(currentPipeline_)) {
		return;
	}

	currentPipeline_ = reinterpret_cast<void*>(bundle.pipeline);
	currentLayout_ = reinterpret_cast<void*>(bundle.layout);

	vkCmdBindPipeline(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		VK_PIPELINE_BIND_POINT_COMPUTE,
		bundle.pipeline
	);

	// THIS flight's set (see BindRenderPipeline + RenderGraph::flightSlot_):
	// per-flight bindless sets, each permanently pointing at its own
	// EngineGlobals slice.
	vkCmdBindDescriptorSets(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		VK_PIPELINE_BIND_POINT_COMPUTE,
		vulkanCtx.defaultPipeLayout,
		0,
		1,
		&vulkanCtx.defaultDescriptorSets_[graph_->flightSlot_],
		0,
		nullptr);
}

void smvk::CommandBuffer::Dispatch(
	uint32_t groupCountX,
	uint32_t groupCountY,
	uint32_t groupCountZ)
{
	vkCmdDispatch(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		groupCountX,
		groupCountY,
		groupCountZ
	);
}

void smvk::CommandBuffer::PushConstants(uint32_t offset, uint32_t size, const void* data, ShaderStage stageMask)
{
	assert(currentLayout_ != nullptr);
	assert(size > 0);
	assert(offset + size <= kEnginePushConstantSize);
	// Release builds have no asserts: close with a message instead of
	// UB / a validation crash. Covers e.g. 126-byte (non-multiple-of-4)
	// pushes: VUID-vkCmdPushConstants-offset-01795 requires multiples of 4.
	if (currentLayout_ == nullptr)
		throw std::runtime_error("unsupported push-constants: no pipeline layout bound");
	if (size == 0 || (offset % 4) != 0 || (size % 4) != 0)
		throw std::runtime_error(
			"unsupported push-constant range [" + std::to_string(offset) + ", " +
			std::to_string(offset + size) + "): offset/size must be multiples of 4");
	if (offset + size < offset || offset + size > kEnginePushConstantSize)
		throw std::runtime_error(
			"unsupported push-constant range [" + std::to_string(offset) + ", " +
			std::to_string(offset + size) + "): exceeds engine limit " +
			std::to_string(kEnginePushConstantSize));
	if (data == nullptr)
		throw std::runtime_error("unsupported push-constants: null data pointer");

	// The global pipeline layout declares a single range [0, 256) with ALL
	// stages. VUID-vkCmdPushConstants-offset-01796 requires the call's
	// stageFlags to include every stage of each overlapping layout range,
	// so a narrow caller mask (e.g. COMPUTE only) must be expanded.
	const PushConstantRange engineRange{ .offset = 0, .size = kEnginePushConstantSize, .stages = ShaderStage::All };
	const ShaderStage expanded = ExpandPushStagesForLayout(
		stageMask, std::span<const PushConstantRange>(&engineRange, 1), offset, size);

	const VkShaderStageFlags vkStages =
		toVkShaderStageFlags(expanded);

	assert(vkStages != 0);

	vkCmdPushConstants(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		reinterpret_cast<VkPipelineLayout>(currentLayout_),
		vkStages,
		offset,
		size,
		data
	);
}

void smvk::CommandBuffer::SetStencilReference(uint32_t frontAndBackReference)
{
	// Dynamic state (see BuildGraphicsPipeline): pipelines never bake the
	// reference, so this records straight into the command stream like
	// PushConstants. Initial state is 0, so passes that never call this
	// keep the reference 0 they were built around.
	vkCmdSetStencilReference(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		VK_STENCIL_FACE_FRONT_AND_BACK,
		frontAndBackReference
	);
}

VkInstance smvk::GetVkInstance() { return instance_; }
VkDevice smvk::GetVkDevice() { return device_; }
bool smvk::SupportsMultiDrawIndirect() { return g_multiDrawIndirect; }
bool smvk::SupportsMeshShaders() { return g_meshShaderSupported; }
VkPhysicalDevice smvk::GetVkPhysicalDevice() { return physicalDevice_; }
VkQueue smvk::GetVkGraphicsQueue() { return graphicsQueue_; }
VkQueue smvk::GetVkPresentQueue() { return presentQueue_; }
VkQueue smvk::GetVkTransferQueue() { return transferQueue_; }
VkQueue smvk::GetVkComputeQueue() { return computeQueue_ != VK_NULL_HANDLE ? computeQueue_ : graphicsQueue_; }
VkSurfaceKHR smvk::GetVkSurface() { return surface_; }
uint32_t smvk::GetVkGraphicsQueueFamily() {
	if (physicalDevice_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE) {
		auto indices = findQueueFamilies(physicalDevice_, surface_);
		if (indices.graphicsFamily) return *indices.graphicsFamily;
	}
	return 0;
}
uint32_t smvk::GetVkTransferQueueFamily() {
	if (physicalDevice_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE) {
		auto indices = findQueueFamilies(physicalDevice_, surface_);
		if (indices.transferFamily) return *indices.transferFamily;
	}
	return GetVkGraphicsQueueFamily();
}
uint32_t smvk::GetVkComputeQueueFamily() {
	if (physicalDevice_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE) {
		auto indices = findQueueFamilies(physicalDevice_, surface_);
		if (indices.computeFamily) return *indices.computeFamily;
	}
	return GetVkGraphicsQueueFamily();
}
bool smvk::HasDedicatedComputeQueue() {
	if (physicalDevice_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE) {
		return findQueueFamilies(physicalDevice_, surface_).HasDedicatedCompute();
	}
	return false;
}

// The profiler backend seam (profile.h): the Vulkan GPU timeline
// machinery — device/queue getters + Optick's Vulkan dispatch table +
// OPTICK_GPU_INIT_VULKAN. The gates (GpuEnabled, one-shot) live in the
// neutral profile.cpp wrapper.
void smvk::profiler::InitBackendGpuTimelinesImpl() {
#if USE_OPTICK
	if (!GpuEnabled())
		return;
	static bool initialized = false;
	if (initialized)
		return;

	VkDevice device = smvk::GetVkDevice();
	VkPhysicalDevice physicalDevice = smvk::GetVkPhysicalDevice();
	VkQueue queue = smvk::GetVkGraphicsQueue();
	if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || queue == VK_NULL_HANDLE)
		return;
	uint32_t family = smvk::GetVkGraphicsQueueFamily();

	// Fill Optick's dispatch table from volk-loaded entry points.
	Optick::VulkanFunctions functions{};
	functions.vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties_>(vkGetPhysicalDeviceProperties);
	functions.vkCreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool_>(vkCreateQueryPool);
	functions.vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool_>(vkCreateCommandPool);
	functions.vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers_>(vkAllocateCommandBuffers);
	functions.vkCreateFence = reinterpret_cast<PFN_vkCreateFence_>(vkCreateFence);
	functions.vkCmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool_>(vkCmdResetQueryPool);
	functions.vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit_>(vkQueueSubmit);
	functions.vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences_>(vkWaitForFences);
	functions.vkResetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer_>(vkResetCommandBuffer);
	functions.vkCmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp_>(vkCmdWriteTimestamp);
	functions.vkGetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults_>(vkGetQueryPoolResults);
	functions.vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer_>(vkBeginCommandBuffer);
	functions.vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer_>(vkEndCommandBuffer);
	functions.vkResetFences = reinterpret_cast<PFN_vkResetFences_>(vkResetFences);
	functions.vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool_>(vkDestroyCommandPool);
	functions.vkDestroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool_>(vkDestroyQueryPool);
	functions.vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence_>(vkDestroyFence);
	functions.vkFreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers_>(vkFreeCommandBuffers);

	OPTICK_GPU_INIT_VULKAN(&device, &physicalDevice, &queue, &family, 1, &functions);
	initialized = true;
#else
	(void)0;
#endif
}



// ---------------------------------------------------------------------------
// The UI overlay's backend seam (imgui_overlay.h): the Vulkan side.
// Everything ImGui-overlay graphics-specific lives HERE — the descriptor
// pool, the ImGui_ImplVulkan init (dynamic rendering, the resolved color
// format), the paired ImGui_ImplSDL3_InitForVulkan, the record, the
// user-texture descriptor sets, and the wait-idle teardown — so the
// overlay TU stays backend-neutral (the D3D12 form slots into the same
// seam when the imgui_impl_dx12 port lands).
// ---------------------------------------------------------------------------

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

namespace smvk::ui {

namespace {

// The overlay's descriptor pool (owned here, the seam's private state).
VkDescriptorPool g_imguiDescriptorPool = VK_NULL_HANDLE;

} // namespace

bool InitBackend(ResourceManager& ctx, Swapchain& swapchain,
                 const UiBackendDesc& desc)
{
	(void)ctx; // the device/queue getters are process-globals here
	VkDevice device = GetVkDevice();
	if (device == VK_NULL_HANDLE)
	{
		std::fprintf(stderr, "[ui] InitBackend: no Vulkan device\n");
		return false;
	}

	// The paired SDL3 platform init. The window MUST flow through here:
	// ImGui_ImplSDL3_NewFrame derives io.DisplaySize from it every frame
	// (SDL_GetWindowSize on a null window fails without writing the
	// out-params — DisplaySize would read uninitialized stack ints; see
	// UiBackendDesc::window).
	if (!ImGui_ImplSDL3_InitForVulkan(desc.window))
	{
		std::fprintf(stderr, "[ui] ImGui_ImplSDL3_InitForVulkan failed\n");
		return false;
	}

	VkDescriptorPoolSize poolSizes[] = {
		{ VK_DESCRIPTOR_TYPE_SAMPLER, 100 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 100 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 100 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 100 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 100 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 100 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 100 },
	};
	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolInfo.maxSets = 100;
	poolInfo.poolSizeCount = (uint32_t)(sizeof(poolSizes)/sizeof(poolSizes[0]));
	poolInfo.pPoolSizes = poolSizes;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
	{
		std::fprintf(stderr, "[ui] vkCreateDescriptorPool failed\n");
		return false;
	}
	g_imguiDescriptorPool = pool;

	VkFormat colorFormat = toVkFormat(desc.colorFormat);
	VkPipelineRenderingCreateInfo renderingCreateInfo{};
	renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingCreateInfo.colorAttachmentCount = 1;
	renderingCreateInfo.pColorAttachmentFormats = &colorFormat;
	renderingCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED; // UI has no depth
	renderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

	ImGui_ImplVulkan_InitInfo vulkanInit{};
	vulkanInit.Instance = GetVkInstance();
	vulkanInit.PhysicalDevice = GetVkPhysicalDevice();
	vulkanInit.Device = device;
	vulkanInit.QueueFamily = GetVkGraphicsQueueFamily();
	vulkanInit.Queue = GetVkGraphicsQueue();
	vulkanInit.DescriptorPool = pool;
	vulkanInit.MinImageCount = desc.minImageCount;
	vulkanInit.ImageCount = desc.imageCount;
	vulkanInit.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	vulkanInit.UseDynamicRendering = true;
	vulkanInit.PipelineRenderingCreateInfo = renderingCreateInfo;
	// Dynamic rendering requires ApiVersion >= 1.3 (volk-loaded entry points).
	vulkanInit.ApiVersion = VK_API_VERSION_1_3;

	if (!ImGui_ImplVulkan_Init(&vulkanInit))
	{
		std::fprintf(stderr, "[ui] ImGui_ImplVulkan_Init failed\n");
		vkDestroyDescriptorPool(device, pool, nullptr);
		g_imguiDescriptorPool = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

void NewFrameBackend()
{
	ImGui_ImplVulkan_NewFrame();
}

void RenderDrawData(void* nativeCmd, ImDrawData* drawData)
{
	ImGui_ImplVulkan_RenderDrawData(drawData,
		reinterpret_cast<VkCommandBuffer>(nativeCmd));
}

ImTextureID AddTexture(ResourceManager& ctx, TextureHandle texture,
                       uint32_t samplerIndex)
{
	// The overlay's liveness check (the LIVE texture listing) already ran;
	// the view + cached sampler come from the Vulkan facade's backend-
	// specific getters.
	const void* view = ctx.GetTextureVkImageView(texture);
	const void* sampler = ctx.GetVkSamplerOpaque(samplerIndex);
	if (!view || !sampler)
		return ImTextureID(0);
	VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
		reinterpret_cast<VkSampler>(const_cast<void*>(sampler)),
		reinterpret_cast<VkImageView>(const_cast<void*>(view)),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return ImTextureID(reinterpret_cast<uint64_t>(set));
}

ImTextureID AddTextureView(ResourceManager& ctx, ImageViewHandle viewHandle,
                           uint32_t samplerIndex)
{
	const void* vkView = ctx.GetImageViewVkImageView(viewHandle);
	const void* sampler = ctx.GetVkSamplerOpaque(samplerIndex);
	if (!vkView || !sampler)
		return ImTextureID(0);
	VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
		reinterpret_cast<VkSampler>(const_cast<void*>(sampler)),
		reinterpret_cast<VkImageView>(const_cast<void*>(vkView)),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return ImTextureID(reinterpret_cast<uint64_t>(set));
}

void RemoveTexture(ImTextureID id)
{
	if (id == ImTextureID(0))
		return;
	ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(id));
}

void ShutdownBackend(ResourceManager& ctx)
{
	(void)ctx;
	VkDevice device = GetVkDevice();
	// The graphics backend first (its pipelines reference the pool), then
	// the paired SDL3 platform shutdown, then the pool itself — and the
	// GPU must be idle before any of the teardown.
	if (device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(device);
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	if (device != VK_NULL_HANDLE && g_imguiDescriptorPool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(device, g_imguiDescriptorPool, nullptr);
		g_imguiDescriptorPool = VK_NULL_HANDLE;
	}
}

} // namespace smvk::ui
