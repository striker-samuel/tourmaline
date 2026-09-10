#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <span>

//#define DEBUG_COMMAND_SUBMISSION

#define VMA_IMPLEMENTATION
#define VMA_VULKAN_VERSION 1003800
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#define VOLK_IMPLEMENTATION

#include "volk.h"
#include <GLFW/glfw3.h>

#include "../Vulkan/detail/vulkan_impl.h"
#include <Engine/Vulkan/vulkanApp.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/swapchain.h>
#include <Engine/App/world.h>
#include <Engine/Profile/profile.h>
using smvk::VulkanResourceManager;
using smvk::VulkanSwapchain;
using smvk::VulkanFrameBuffer;

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
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
// Set at device creation (see VulkanCore init): true when the device
// supports VK multiDrawIndirect (drawCount > 1 in a single indirect call).
static bool g_multiDrawIndirect = false;
struct QueueMutex { VkQueue queue = VK_NULL_HANDLE; std::mutex mutex; };
static QueueMutex graphicsQueueMutex;
static QueueMutex presentQueueMutex;
static QueueMutex transferQueueMutex;
static std::mutex& GetQueueMutex(VkQueue q) {
	if (q == graphicsQueue_) return graphicsQueueMutex.mutex;
	if (q == presentQueue_) return presentQueueMutex.mutex;
	if (q == transferQueue_) return transferQueueMutex.mutex;
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
		auto* p = new CommandBufferPool();
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

	inline bool is_aligned4(uint32_t v) { return (v & 3u) == 0u; }

	struct FormatInfo {
		uint32_t blockWidth;
		uint32_t blockHeight;
		uint32_t bytesPerBlock;
	};
	FormatInfo getFormatInfo(VkFormat format) {
		switch (format) {
			// 8-bit per pixel
		case VK_FORMAT_R8_UNORM:
		case VK_FORMAT_R8_SRGB:
			return { 1, 1, 1 };

			// 16-bit per pixel
		case VK_FORMAT_R8G8_UNORM:
		case VK_FORMAT_R8G8_SRGB:
		case VK_FORMAT_R16_UNORM:
		case VK_FORMAT_D16_UNORM:
			return { 1, 1, 2 };

			// 32-bit per pixel
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_R16G16_UNORM:
		case VK_FORMAT_R32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT:
			return { 1, 1, 4 };

			// 64-bit per pixel
		case VK_FORMAT_R16G16B16A16_SFLOAT:
		case VK_FORMAT_R32G32_SFLOAT:
			return { 1, 1, 8 };

			// 128-bit per pixel
		case VK_FORMAT_R32G32B32A32_SFLOAT:
			return { 1, 1, 16 };

			// BC compressed formats
		case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
		case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
		case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
		case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
			return { 4, 4, 8 };

		case VK_FORMAT_BC2_UNORM_BLOCK:
		case VK_FORMAT_BC2_SRGB_BLOCK:
		case VK_FORMAT_BC3_UNORM_BLOCK:
		case VK_FORMAT_BC3_SRGB_BLOCK:
		case VK_FORMAT_BC5_UNORM_BLOCK:
		case VK_FORMAT_BC5_SNORM_BLOCK:
		case VK_FORMAT_BC7_UNORM_BLOCK:
		case VK_FORMAT_BC7_SRGB_BLOCK:
			return { 4, 4, 16 };

		default:
			assert(false && "Unsupported VkFormat");
			return { 1, 1, 0 };
		}
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
	VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window) {

		if (capabilities.currentExtent.width !=
			std::numeric_limits<uint32_t>::max()) {
			return capabilities.currentExtent;
		}
		else {
			int width, height;
			glfwGetFramebufferSize(window, &width, &height);

			VkExtent2D actualExtent = { static_cast<uint32_t>(width),
									   static_cast<uint32_t>(height) };

			actualExtent.width =
				std::clamp(actualExtent.width, capabilities.minImageExtent.width,
					capabilities.maxImageExtent.width);
			actualExtent.height =
				std::clamp(actualExtent.height, capabilities.minImageExtent.height,
					capabilities.maxImageExtent.height);

			return actualExtent;
		}
	}
	std::vector<const char*> getRequiredExtensionsForGlfw(bool enableValidationLayers) {
		uint32_t glfwExtensionCount = 0;
		const char** glfwExtensions;
		glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

		std::vector<const char*> extensions(glfwExtensions,
			glfwExtensions + glfwExtensionCount);

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

//smvk::Format ChooseSwapSurfaceFormat(const std::vector<smvk::Format>& availableFormats) {
//	for (const auto& availableFormat : availableFormats) {
//		if (availableFormat.format == smvk::Format::BGRA8_SRGB &&
//			availableFormat.colorSpace == smvk::ColorSpace::SRGB_Nonlinear)
//		{
//			return availableFormat;
//		}
//	}
//	return availableFormats[0];
//}

// RHI to vulkan
constexpr VkBufferUsageFlags bufferTypeToUsageFlags[9] = {
	VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Index (addressable: GPU-driven filter/index reads via BDA)
	VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Vertex (addressable: filter CS reads the static pos heap via BDA; null BDA = MMU fault + device lost)
	VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Uniform
	VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, // BufferType_Storage (also hosts GeometryFX-style filtered index lists)
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

//constexpr bool HasAnyFlag(
//	smvk::ImageUsage value,
//	smvk::ImageUsage flags)
//{
//	return
//		(static_cast<uint32_t>(value) &
//			static_cast<uint32_t>(flags)) != 0;
//}
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

		// compressed
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

inline void ImageBarrier(
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
void static CreateGraphicsPipeline(VkGraphicsPipelineCreateInfo createInfo, VkPipeline& pipeline) {
	assert(!pipeline);
	if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &createInfo,
		nullptr, &pipeline) != VK_SUCCESS)
	{
		throw std::runtime_error("failed to create graphics pipeline!");
	}

}
void static CreateComputePipeline(VkComputePipelineCreateInfo createInfo, VkPipeline& pipeline)
{
	assert(!pipeline);
	if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &createInfo,
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
		throw std::runtime_error(
			"validation layers requested, but not available!");
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

	auto extensions = getRequiredExtensionsForGlfw(enableValidationLayers);

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();

	VkValidationFeatureDisableEXT disabledValidationFeatures[1] = {
		/*VK_VALIDATION_FEATURE_DISABLE_UNIQUE_HANDLES_EXT*/ // with this on debugNames break
	};

	VkValidationFeatureEnableEXT enabledValidationFeatures[1] = {
		VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
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

		for (const auto& device : devices) {
			if (isDeviceSuitable(device, surface_, config_.deviceExtensions)) {
				physicalDevice_ = device;
				break;
			}
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
				indices.transferFamily.value()
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
			bool enableHostCopy = false;
			if (hostImageCopyAvailable) {
				VkPhysicalDeviceHostImageCopyFeaturesEXT queryHostCopy{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT, nullptr };
				VkPhysicalDeviceFeatures2 queryFeats2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &queryHostCopy };
				vkGetPhysicalDeviceFeatures2(physicalDevice_, &queryFeats2);
				if (queryHostCopy.hostImageCopy == VK_TRUE) enableHostCopy = true;
			}

			VkPhysicalDeviceHostImageCopyFeaturesEXT deviceHostCopyFeatures{
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT,
				nullptr,
				enableHostCopy ? VK_TRUE : VK_FALSE
			};

			VkPhysicalDeviceVulkan11Features deviceFeatures11
			{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
				.pNext = enableHostCopy ? (void*)&deviceHostCopyFeatures : nullptr,
				.storageBuffer16BitAccess = VK_TRUE,
				.uniformAndStorageBuffer16BitAccess = VK_TRUE,
			};
			VkPhysicalDeviceVulkan12Features deviceFeatures12
			{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
				.pNext = &deviceFeatures11,
				.shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
				.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
				.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
				// engine globals binding (kBinding_EngineGlobals): the single
				// storage-buffer descriptor is rewritten every frame while
				// previous frames are still executing (update-after-bind).
				.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE,
				.descriptorBindingPartiallyBound = VK_TRUE,
				.descriptorBindingVariableDescriptorCount = VK_TRUE,
				.runtimeDescriptorArray = VK_TRUE,
				.timelineSemaphore = VK_TRUE,
				.bufferDeviceAddress = VK_TRUE
			};
			VkPhysicalDeviceVulkan13Features deviceFeatures13
			{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
				.pNext = &deviceFeatures12,
				.synchronization2 = VK_TRUE,
				.dynamicRendering = VK_TRUE,
				.maintenance4 = VK_TRUE,
			};

		VkPhysicalDeviceFeatures2 feats2{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			.pNext = &deviceFeatures13,
			.features = {
				.samplerAnisotropy = VK_TRUE,
				.textureCompressionBC = VK_TRUE,
			},
		};

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
		}

			std::vector<const char*> enabledExtensions = config_.deviceExtensions;
			if (enableHostCopy) enabledExtensions.push_back(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);

			VkDeviceCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
			createInfo.pQueueCreateInfos = queueCreateInfos.data();
			createInfo.pEnabledFeatures = nullptr;
			createInfo.pNext = &feats2;
			createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
			createInfo.ppEnabledExtensionNames = enabledExtensions.data();
			createInfo.enabledLayerCount = 0;

		if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
			throw std::runtime_error("failed to create logical device!");
		}
		if (!g_multiDrawIndirect) {
			fprintf(stderr,
				"[Vulkan Warning] multiDrawIndirect not supported by this device; "
				"vkCmdDrawIndexedIndirect with drawCount > 1 will be emulated "
				"one draw at a time (slow path, expect lower throughput)\n");
		}
		volkLoadDevice(device_);

		}

		QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
		vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
		vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
		{
			QueueFamilyIndices q2 = findQueueFamilies(physicalDevice_, surface_);
			if (q2.transferFamily) vkGetDeviceQueue(device_, q2.transferFamily.value(), 0, &transferQueue_);
			else transferQueue_ = graphicsQueue_;
		}

		// Optick GPU timelines need the device + graphics queue + volk dispatch.
		smvk::profiler::InitVulkanGPU();
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
					delete pool;
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
}
std::unique_ptr<smvk::VulkanCore> smvk::CreateVulkanCoreWithWindow(smvk::VulkanCore::CoreConfiguration config, GLFWwindow* window) {
	assert(window != nullptr && "window was nullptr!");

	auto unique = std::make_unique<VulkanCore>(config);
	auto core = unique.get();

	core->CreateInstance();
	if (glfwCreateWindowSurface(instance_, window, nullptr, &surface_) != VK_SUCCESS) {
		throw std::runtime_error("failed to create window surface!");
	}
	core->Init();
	return unique;
}


// ResourceContext
smvk::VulkanResourceManager::VulkanResourceManager(
	VulkanCore::CoreConfiguration configuration,
	GLFWwindow* window)
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

	m_submitter = new Submitter(*core_, core_->indices.graphicsFamily.value());
#if 0
	if (core_->indices.transferFamily && core_->indices.transferFamily.value() != core_->indices.graphicsFamily.value()) {
		m_transferSubmitter = new Submitter(*core_, core_->indices.transferFamily.value());
	}
	else
#endif
	{
		m_transferSubmitter = m_submitter;
	}
	{
		VkPhysicalDeviceHostImageCopyFeaturesEXT hostFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT, nullptr };
		VkPhysicalDeviceFeatures2 feats2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &hostFeat };
		vkGetPhysicalDeviceFeatures2(physicalDevice_, &feats2);
		m_hostImageCopySupported = hostFeat.hostImageCopy == VK_TRUE;
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
	m_streamer = std::make_unique<UploadStreamer>(this, m_transferSubmitter, m_submitter);
	m_streamer->init(256ull * 1024 * 1024);

	//this->m_hostImageCopySupported = false;

	// SamplerCache – owns up to MAX_SAMPLERS deduplicated VkSamplers.
	// Like VulkanPipelineCache, key (SamplerCreateInfo) → stable index.
	samplerCache_ = std::make_unique<VulkanSamplerCache>(this);
	// Populate default slots: 0 = regular, 1 = shadow comparison
	// Keep legacy handles in sync for existing code paths.
	{
		uint32_t idx0 = samplerCache_->GetOrCreate(SamplerCreateInfo{});
		m_defaultSampler = samplerCache_->at(idx0);
		uint32_t idx1 = samplerCache_->GetOrCreate(SamplerCreateInfo{
			.compareOp = smvk::CompareOp::LessEqual,
			.compareEnable = true
			});
		m_defaultShadowSampler = samplerCache_->at(idx1);
		assert(idx0 == 0 && idx1 == 1 && "sampler cache must allocate defaults at 0,1");
	}
	const uint32_t pixel = 0xFF000000;
	m_nilTexture = CreateTexture(
		ImageCreateInfo
		{
			.extent = {1,1,1},
			.format = smvk::Format::RGBA8_SRGB,
			.name = "nilTexture"
		},
		&pixel
	).release();

	// Engine globals buffer: one EngineGlobals slice per frame in flight,
	// read by every shader through set 0 / kBinding_EngineGlobals.
	// Values (time / deltaTime / frameIndex / fps) are computed engine-side
	// from the acquire rhythm and flushed by VulkanSwapchain::AcquireFramebuffer
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
		// in flight. The descriptor is rewritten every frame (update-after-bind)
		// to point at the current frame's slice; see FlushEngineGlobals.
		getDSLBinding(kBinding_EngineGlobals, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stageFlags),
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

	std::vector<VkDescriptorPoolSize> poolSizes = {
	  {
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    MAX_TEXTURES },
		{ VK_DESCRIPTOR_TYPE_SAMPLER,          MAX_SAMPLERS },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,    MAX_TEXTURES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   MAX_FRAMES_IN_FLIGHT }, // engine globals slices
	  }
	};
	defaultDescriptorPool = CreateDescriptorPool(poolSizes);

	VkDescriptorPool pool = *descriptorPools_.get(defaultDescriptorPool);
	VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT]{};
	for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		layouts[frame] = defaultSetLayout;
	}

	VkDescriptorSetAllocateInfo info
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.pNext = nullptr,
		.descriptorPool = pool,
		.descriptorSetCount = 1, // We are just allocating the first layout namespace
		.pSetLayouts = layouts
	};
	vkAllocateDescriptorSets(
		device_,
		&info,
		defaultDescriptorSets_
	);

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
	destroy(this, m_engineGlobalsBuffer);
	destroy(this, m_readbackStaging);

	// Flush, not Process: queued tasks may reference the swapchain timeline
	// semaphore, which ~VulkanSwapchain already destroyed (member order).
	// The device is idle, so every retire point is trivially satisfied and no
	// readiness query (which would touch the dead semaphore) may run.
	FlushDeferredTasks();
	// StaticVertexFmt
	destroy(this, staticVertexFmtHeap.m_staticVertexPosBuffer);
	destroy(this, staticVertexFmtHeap.m_staticVertexShadingBuffer);
	destroy(this, staticVertexFmtHeap.m_staticIndexBuffer);
	if (staticVertexFmtHeap.m_staticVertexBlock) {
		vmaDestroyVirtualBlock(staticVertexFmtHeap.m_staticVertexBlock);
		staticVertexFmtHeap.m_staticVertexBlock = VK_NULL_HANDLE;
	}
	if (staticVertexFmtHeap.m_staticIndexBlock) {
		vmaDestroyVirtualBlock(staticVertexFmtHeap.m_staticIndexBlock);
		staticVertexFmtHeap.m_staticIndexBlock = VK_NULL_HANDLE;
	}
	//
	if (m_streamer) { m_streamer->waitAll(); m_streamer->destroy(); m_streamer.reset(); }
	// Unconditional drain: resources freed during Shutdown (after the last
	// ProcessDeferrredTasks) plus the static buffers below must still be
	// released. Device is idle, and Flush never queries semaphores, so this is
	// safe even though the swapchain timeline is already gone (member order).
	FlushDeferredTasks();
	if (m_transferSubmitter && m_transferSubmitter != m_submitter) { delete m_transferSubmitter; }
	m_transferSubmitter = nullptr;
	delete m_submitter;
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
	if (ringCapacityPerThread_ < 32ull * 1024 * 1024) ringCapacityPerThread_ = 32ull * 1024 * 1024;
	return ensureShardRing(states_[0]);
}
bool smvk::UploadStreamer::ensureShardRing(ThreadUploadState& ust) {
	if (ust.ring.buffer != VK_NULL_HANDLE) return true;
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = ringCapacityPerThread_;
	ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	VmaAllocationInfo info{};
	VkResult r = vmaCreateBuffer(m_rm->m_allocator, &ci, &aci, &ust.ring.buffer, &ust.ring.allocation, &info);
	if (r != VK_SUCCESS) return false;
	ust.ring.mapped = info.pMappedData;
	ust.ring.capacity = ringCapacityPerThread_;
	ust.ring.offset = 0;
#if defined(_DEBUG)
	{
		VkDebugUtilsObjectNameInfoEXT ni{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
		ni.objectType = VK_OBJECT_TYPE_BUFFER;
		ni.objectHandle = reinterpret_cast<uint64_t>(ust.ring.buffer);
		ni.pObjectName = "UploadStreamer Ring (thread shard)";
		vkSetDebugUtilsObjectNameEXT(device_, &ni);
	}
#endif
	return true;
}
void smvk::UploadStreamer::destroy() {
	for (auto& ust : states_) {
		if (ust.curCmd != VK_NULL_HANDLE && ust.hasPending) {
			// submit any pending work before destroy
			submitState(ust, false);
		}
	}
	if (m_transfer) m_transfer->waitAll();
	if (m_graphics && m_graphics != m_transfer) m_graphics->waitAll();
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
		// need to flush current batch and wrap – must wait before reusing memory
		if (ust.hasPending) submitState(ust, true);
		if (size > ust.ring.capacity) return VK_WHOLE_SIZE; // signal fallback to temp staging
		// Correctness: wait for transfer queue to finish reading old ring contents before wrap
		m_transfer->waitAll();
		ust.ring.offset = 0;
		aligned = 0;
		if (aligned + size > ust.ring.capacity) return VK_WHOLE_SIZE;
	}
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
void smvk::UploadStreamer::submitState(ThreadUploadState& ust, bool waitOldestIfFull) {
	// st is owned by the calling thread – no streamer lock. The Submitter
	// and queue take their own brief locks internally.
	if (!ust.hasPending || ust.curCmd == VK_NULL_HANDLE) {
		if (ust.curCmd != VK_NULL_HANDLE) {
			auto tp = m_transfer->submit(ust.curHandle);
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
	auto tp = m_transfer->submit(ust.curHandle);
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
	for (auto& ust : states_) if (ust.hasPending) return true;
	return false;
}
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
			VkBufferMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, srcFamily, dstFamily, dstBuf->vkBuffer, dstOffset, size };
			VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,0,nullptr,1,&rel,0,nullptr };
			vkCmdPipelineBarrier2(ust.curCmd, &dep);
			VkBufferMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, 0, 0, dstStages, dstAccess, srcFamily, dstFamily, dstBuf->vkBuffer, dstOffset, size };
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
		if (ust.stagedBytesSinceFlush >= kFlushThreshold) {
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
		VkBufferMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, srcFamily, dstFamily, dstBuf->vkBuffer, dstOffset, size };
		VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,0,nullptr,1,&rel,0,nullptr };
		vkCmdPipelineBarrier2(ust.curCmd, &dep);
		VkBufferMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, 0, 0, dstStages, dstAccess, srcFamily, dstFamily, dstBuf->vkBuffer, dstOffset, size };
		ust.pendingBufferAcquires.push_back(acq);
	}
	else {
		BufferBarrier(ust.curCmd, dstBuf->vkBuffer, dstOffset, size, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStages, dstAccess);
	}
	ust.hasPending = true;
	VulkanResourceManager::BufferGlobalSyncState st{}; st.masks.stageMask = FromVkPipelineStage(dstStages); if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess)); st.masks.accessMask = FromVkAccess(dstAccess); m_rm->SetBufferSyncState(dst, st);
	ust.stagedBytesSinceFlush += size;
	if (ust.stagedBytesSinceFlush >= kFlushThreshold) {
		submitState(ust, false);
	}
	// if we are batching many small uploads, don't submit yet – caller will flush
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
	extern void ImageBarrier(VkCommandBuffer, VkImage, VkImageLayout, VkImageLayout, VkImageSubresourceRange, VkPipelineStageFlags2, VkAccessFlags2, VkPipelineStageFlags2, VkAccessFlags2, uint32_t, uint32_t);
	uint32_t srcF = m_transfer->queueFamilyIndex();
	uint32_t dstF = m_graphics->queueFamilyIndex();
	bool needOwnership = srcF != dstF;
	// UNDEFINED -> TRANSFER_DST
	ImageBarrier(ust.curCmd, img->vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fullRange, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
	// copy
	if (img->mipLevels == 1 && img->arrayLayers == 1) {
		VkBufferImageCopy2 r{ VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, nullptr, srcOff, 0,0, {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, {0,0,0}, {img->extent.width, img->extent.height, 1} };
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
				VkBufferImageCopy2 r{ VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, nullptr, off, 0,0, {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1}, {0,0,0}, {w,h,1} };
				regs.push_back(r);
				off += layerSize;
			}
		}
		VkCopyBufferToImageInfo2 copyInfo{ VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, nullptr, srcBuf, img->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regs.size(), regs.data() };
		vkCmdCopyBufferToImage2(ust.curCmd, &copyInfo);
	}
	bool remainTransfer = (dstStages == VK_PIPELINE_STAGE_2_TRANSFER_BIT && dstAccess == VK_ACCESS_2_TRANSFER_WRITE_BIT);
	if (!remainTransfer) {
		if (needOwnership) {
			VkImageMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
			VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &rel };
			vkCmdPipelineBarrier2(ust.curCmd, &dep);
			VkImageMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, 0, 0, dstStages, dstAccess, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
			ust.pendingImageAcquires.push_back(acq);
		}
		else {
			ImageBarrier(ust.curCmd, img->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, fullRange, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStages, dstAccess, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		}
	}
	else {
		if (needOwnership) {
			VkImageMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
			VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &rel }; vkCmdPipelineBarrier2(ust.curCmd, &dep);
			VkImageMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, 0, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
			ust.pendingImageAcquires.push_back(acq);
		}
	}
	ust.hasPending = true;
	ust.stagedBytesSinceFlush += dataSize;
	if (ust.stagedBytesSinceFlush >= kFlushThreshold) {
		submitState(ust, false);
	}
	// defer staging destruction until transfer batch is submitted (TimelinePoint)
	if (useTmp) {
		ust.pendingCleanups.push_back(std::packaged_task<void()>([rm = m_rm, buf = tmpBuf, alloc = tmpAlloc] { vmaDestroyBuffer(rm->m_allocator, buf, alloc); }));
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
	ThreadUploadState& ust = states_[CurrentPoolShard()];
	VkDeviceSize ringOff = allocFromRing(ust, dataSize, 16);
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
	ensureCmd(ust);
	VkImageSubresourceRange fullRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, img->mipLevels, 0, img->arrayLayers };
	uint32_t srcF = m_transfer->queueFamilyIndex();
	uint32_t dstF = m_graphics->queueFamilyIndex();
	bool needOwnership = srcF != dstF;
	// UNDEFINED or TRANSFER_DST -> TRANSFER_DST (handle host fallback case where image already in TRANSFER_DST)
	// If host fallback transitioned to TRANSFER_DST, oldLayout should be TRANSFER_DST, not UNDEFINED. Check current layout via host transition? For simplicity, use UNDEFINED->TRANSFER_DST with IGNORED, driver will handle if already TRANSFER_DST (validation may warn but not fault for fallback)
	ImageBarrier(ust.curCmd, img->vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fullRange, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
	std::vector<VkBufferImageCopy2> regs2;
	regs2.reserve(regions.size());
	for (auto& r : regions) {
		VkBufferImageCopy2 r2{ VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, nullptr, srcOff + r.bufferOffset, 0,0, {r.imageSubresource.aspectMask, r.imageSubresource.mipLevel, r.imageSubresource.baseArrayLayer, r.imageSubresource.layerCount}, r.imageOffset, r.imageExtent };
		regs2.push_back(r2);
	}
	VkCopyBufferToImageInfo2 copyInfo{ VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, nullptr, srcBuf, img->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regs2.size(), regs2.data() };
	vkCmdCopyBufferToImage2(ust.curCmd, &copyInfo);
	if (!remainTransfer) {
		if (needOwnership) {
			VkImageMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
			VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &rel };
			vkCmdPipelineBarrier2(ust.curCmd, &dep);
			VkImageMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, 0, 0, dstStages, dstAccess, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
			ust.pendingImageAcquires.push_back(acq);
		}
		else {
			ImageBarrier(ust.curCmd, img->vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, fullRange, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, dstStages, dstAccess, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		}
	}
	else {
		if (needOwnership) {
			VkImageMemoryBarrier2 rel{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
			VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 1, &rel }; vkCmdPipelineBarrier2(ust.curCmd, &dep);
			VkImageMemoryBarrier2 acq{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, 0, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcF, dstF, img->vkImage, fullRange };
			ust.pendingImageAcquires.push_back(acq);
		}
	}
	ust.hasPending = true;
	ust.stagedBytesSinceFlush += dataSize;
	if (ust.stagedBytesSinceFlush >= kFlushThreshold) {
		submitState(ust, false);
	}
	if (useTmp) {
		ust.pendingCleanups.push_back(std::packaged_task<void()>([rm = m_rm, buf = tmpBuf, alloc = tmpAlloc] { vmaDestroyBuffer(rm->m_allocator, buf, alloc); }));
	}
	VulkanResourceManager::ImageGlobalSyncState st{}; st.masks.stageMask = FromVkPipelineStage(dstStages); if (st.masks.stageMask == PipelineStageBit_None && dstAccess != VK_ACCESS_2_NONE) st.masks.stageMask = DerivePipelineStage(FromVkAccess(dstAccess)); st.masks.accessMask = FromVkAccess(dstAccess); st.layout = remainTransfer ? ImageLayout::TransferDstOptimal : ImageLayout::ShaderReadOnlyOptimal; if (remainTransfer) st.masks.stageMask = PipelineStageBit_Transfer; m_rm->SetImageSyncState(imageHandle, st);
}
void smvk::UploadStreamer::flush() {
	SMVK_PROFILE_STREAM("UploadFlush");
	for (auto& ust : states_) if (ust.hasPending) submitState(ust, true); if (m_transfer) m_transfer->purge(); }
void smvk::UploadStreamer::flushLocal() {
	SMVK_PROFILE_STREAM("UploadFlushLocal");
	ThreadUploadState& ust = states_[CurrentPoolShard()]; if (ust.hasPending) submitState(ust, true); if (m_transfer) m_transfer->purge(); }
smvk::TimelinePoint smvk::UploadStreamer::flushAndGetTimeline() {
	// Drains all shards (quiescent: call after workers joined). Returns the
	// last timeline point for callers that need to chain.
	TimelinePoint last{};
	for (auto& ust : states_) {
		if (!ust.hasPending || ust.curCmd == VK_NULL_HANDLE) continue;
		auto tp = m_transfer->submit(ust.curHandle);
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
	flush(); if (m_transfer) m_transfer->waitAll(); if (m_graphics && m_graphics != m_transfer) m_graphics->waitAll(); }

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
	// Hot path via UploadStreamer (transfer queue + ring) if available
	if (m_streamer) {
		m_streamer->uploadBuffer(buffer, data, dataSize, dest_offset, dstStages, dstAccess);
		// do not flush here – caller may batch. For single uploads outside batch, flush immediately
		// to preserve old "submit per call" semantics we flush if not in a batch – heuristic: flush if size < 1MiB? For now flush
		// To keep batching for Cluster, Cluster will call FlushUploads after 3 uploads.
		// Detect batch: if caller is Cluster (we can check if dest is static vertex heap) defer flush.
		// Simple: flush if the buffer is NOT the static heap buffers
		if (buffer != staticVertexFmtHeap.m_staticVertexPosBuffer &&
			buffer != staticVertexFmtHeap.m_staticVertexShadingBuffer &&
			buffer != staticVertexFmtHeap.m_staticIndexBuffer) {
			m_streamer->flushLocal();
		}
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
bool smvk::VulkanResourceManager::DownloadDataFromBuffer(BufferHandle src, void* out, size_t size, size_t srcOffset) {
	if (size == 0 || out == nullptr || src.empty())
		return false;
	auto srcBuf = buffers_.get(src);
	if (!srcBuf || srcBuf->vkBuffer == VK_NULL_HANDLE)
		return false;
	// Persistent staging: no per-call buffer create and no deferred
	// destroy (the old per-call staging destroy re-entered the deferred
	// task queue from inside ProcessDeferrredTasks' locked drain loop,
	// re-locking the non-recursive m_deferredMutex = UB).
	if (!EnsureReadbackStaging(static_cast<VkDeviceSize>(size)))
		return false;
	auto stagingBuf = buffers_.get(m_readbackStaging);
	if (!stagingBuf || stagingBuf->vkBuffer == VK_NULL_HANDLE)
		return false;
	auto han = m_submitter->acquire();
	auto cmd = m_submitter->GetCommandBuffer(han);
	// Prior writers (e.g. filter compute) -> transfer read. ALL_COMMANDS /
	// MEMORY_ALL is the conservative src: we don't track the last writer here.
	BufferBarrier(cmd, srcBuf->vkBuffer, srcOffset, size,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		VK_ACCESS_2_TRANSFER_READ_BIT);
	VkBufferCopy copyRegion{ static_cast<VkDeviceSize>(srcOffset), 0, static_cast<VkDeviceSize>(size) };
	vkCmdCopyBuffer(cmd, srcBuf->vkBuffer, stagingBuf->vkBuffer, 1, &copyRegion);
	BufferBarrier(cmd, stagingBuf->vkBuffer, 0, size,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_HOST_BIT,
		VK_ACCESS_2_HOST_READ_BIT);
	m_submitter->submit(han);
	m_submitter->wait(han, UINT64_MAX);
	// Safe to reuse on the next call: every download waits for its own
	// copy before returning, so the staging is never read in flight.
	void* mapped = MapBuffer(m_readbackStaging);
	if (!mapped)
		return false;
	std::memcpy(out, mapped, size);
	UnmapBuffer(m_readbackStaging);
	return true;
}

bool smvk::VulkanResourceManager::DownloadDataFromImage(ImageHandle src, ImageLayout currentLayout, void* out, size_t outSize) {
	if (outSize == 0 || out == nullptr || src.empty())
		return false;
	auto srcImg = images_.get(src);
	if (!srcImg || srcImg->vkImage == VK_NULL_HANDLE)
		return false;
	if (!EnsureReadbackStaging(static_cast<VkDeviceSize>(outSize)))
		return false;
	auto stagingBuf = buffers_.get(m_readbackStaging);
	if (!stagingBuf || stagingBuf->vkBuffer == VK_NULL_HANDLE)
		return false;

	// vkCmdCopyImageToBuffer sources are only GENERAL / TRANSFER_SRC_OPTIMAL:
	// transition out of the caller-declared ACTUAL layout for the copy and
	// back afterwards. Never pass Undefined here - that discards contents.
	const VkImageLayout tracked = ToVkImageLayout(currentLayout);
	const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
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
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
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
	void* mapped = MapBuffer(m_readbackStaging);
	if (!mapped)
		return false;
	std::memcpy(out, mapped, outSize);
	UnmapBuffer(m_readbackStaging);
	return true;
}

bool smvk::VulkanResourceManager::EnsureReadbackStaging(VkDeviceSize size) {
	static constexpr VkDeviceSize kMinBytes = 256ull * 1024;       // 256 KiB floor
	static constexpr VkDeviceSize kMaxBytes = 256ull * 1024 * 1024; // sanity cap
	if (!m_readbackStaging.empty() && m_readbackStagingSize >= size)
		return true;
	if (size > kMaxBytes)
		return false;
	VkDeviceSize want = kMinBytes;
	while (want < size)
		want *= 2;
	// HOST_ACCESS_RANDOM: the CPU READS this buffer (copy results), so
	// cached memory beats the default sequential-write (write-combined).
	Holder<BufferHandle> grown = CreateBuffer(want, smvk::BufferType_Staging,
		"readback staging", VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
	if (grown.empty())
		return false;
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

	VkImageCreateInfo vkCI{
		VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
	};

	//vkCI.flags = toVkImageCreateFlags(ci.flags);
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
	vkCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocCI{};
	allocCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	allocCI.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	VK_CHECK(vmaCreateImage(
		m_allocator,
		&vkCI,
		&allocCI,
		&image.vkImage,
		&image.allocation,
		nullptr
	));

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

		vkSetDebugUtilsObjectNameEXT(
			device_,
			&nameInfo
		);
	}
#endif

	return image;
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
[[nodiscard]] smvk::Holder<smvk::BindlessImageHandle> smvk::VulkanResourceManager::CreateBindlessImage(ImageViewHandle viewHandle, VkImageLayout readLayout)
{
	ImageView* view = imageViews_.get(viewHandle);
	assert(view);

	BindlessImage bindless{};
	bindless.view = viewHandle;
	bindless.imageLayout = readLayout;

	BindlessImageHandle handle =
		bindlessImages_.create(std::move(bindless));

	BindlessImage* resource =
		bindlessImages_.get(handle);

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

	if (imageResource->type == ImageType::Image3D)
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

	imageCI.sharingMode =
		VK_SHARING_MODE_EXCLUSIVE;

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


	VK_CHECK(
		vmaCreateImage(
			m_allocator,
			&imageCI,
			&allocCI,
			&vkImage,
			&allocation,
			nullptr
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

		vkSetDebugUtilsObjectNameEXT(
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

		vkSetDebugUtilsObjectNameEXT(
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
	if (m_transferSubmitter && m_transferSubmitter != m_submitter &&
		(bufferType == BufferType_Vertex || bufferType == BufferType_Index || bufferType == BufferType_Storage)) {
		ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
		static uint32_t families[2];
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
	VK_CHECK(vmaCreateBuffer(
		m_allocator,
		&ci,
		&aci,
		&desc.vkBuffer,
		&desc.vma_alloc,
		nullptr
		//&desc.allocInfo
	)
	);

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
#if defined(_DEBUG)
	if (debugName) {
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT
		};
		nameInfo.objectType = VK_OBJECT_TYPE_BUFFER;
		nameInfo.objectHandle = reinterpret_cast<uint64_t>(desc.vkBuffer);
		nameInfo.pObjectName = debugName;
		vkSetDebugUtilsObjectNameEXT(device_, &nameInfo);
	}
#else
	(void)debugName;
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
		vkSetDebugUtilsObjectNameEXT(device_, &nameInfo);
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
	VkDescriptorPoolCreateInfo poolInfo{ };
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
		vkSetDebugUtilsObjectNameEXT(device_, &nameInfo);
	}
#else
	(void)debugName;
#endif
	return { this, descriptorPools_.create(std::move(dpool)) };
}
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
		auto it = m_deferredTasks.begin();
		while (it != m_deferredTasks.end() && it->isReady())
			++it;
		ready.assign(std::make_move_iterator(m_deferredTasks.begin()),
			std::make_move_iterator(it));
		m_deferredTasks.erase(m_deferredTasks.begin(), it);
	}
	// Run ready tasks OUTSIDE the lock: tasks may enqueue more deferred
	// work (destroy -> DeferDestroy -> AddDeferredTask), which previously
	// re-entered this non-recursive mutex from inside the drain loop (UB:
	// deadlock or fast-fail depending on the CRT). Appends made during
	// the run land behind the not-ready remainder, so the FIFO-by-retire-
	// point order is preserved.
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
void smvk::VulkanResourceManager::DeferDestroy(std::packaged_task<void()>&& task)
{
	// No graphics submit ever recorded -> nothing can be in flight.
	if (m_lastGraphicsSemaphore == VK_NULL_HANDLE || m_lastGraphicsValue == 0) {
		task();
		return;
	}
	// Retire values are monotonic, so FIFO prefix-drain in
	// ProcessDeferrredTasks stays correct.
	AddDeferredTask(std::move(task), TimelinePoint{ m_lastGraphicsSemaphore, m_lastGraphicsValue });
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

// Compute this frame's EngineGlobals from the acquire rhythm, upload them
// into the frame's slice and point the engine-globals descriptor at it.
// Called by the swapchain after the in-flight slot retires (timeline wait),
// before any pass records, so:
//   - the slice is safe to overwrite (previous use completed), and
//   - the update-after-bind descriptor rewrite is visible to every command
//     recorded this frame, while already-recorded frames keep their slice.
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

	const Buffer* buffer = buffers_.get(m_engineGlobalsBuffer);
	assert(buffer && buffer->vkBuffer != VK_NULL_HANDLE);

	VkDescriptorBufferInfo info{};
	info.buffer = buffer->vkBuffer;
	info.offset = sliceOffset;
	info.range = sizeof(smvk::EngineGlobals);

	VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = defaultDescriptorSets_[0]; // single global bindless set instance
	write.dstBinding = smvk::Bindings::kBinding_EngineGlobals;
	write.dstArrayElement = 0;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.descriptorCount = 1;
	write.pBufferInfo = &info;
	vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
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

	// Ideally overwrite this descriptor with your null/default image.
	//ctx->ClearBindlessImageSlot(slot);

	ctx->bindlessImages_.destroy(handle);
	// If descriptors are replicated per-frame.
	ctx->m_needToUpdateTextures = true;
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
namespace smvk { namespace {
void DestroyBufferNow(VulkanResourceManager* ctx, BufferHandle handle) {
	Buffer db = *ctx->buffers_.get(handle);
	ctx->RemoveBufferSyncState(handle);
	vmaDestroyBuffer(ctx->m_allocator, db.vkBuffer, db.vma_alloc);
	ctx->buffers_.destroy(handle);
}
} } // namespace smvk::<anonymous>
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::BufferHandle handle) {
	if (handle.empty()) { return; };
	// Buffers are referenced by in-flight draws/indirect args; retire on the
	// graphics timeline instead of destroying under the GPU.
	std::packaged_task<void()> task([ctx, handle] { DestroyBufferNow(ctx, handle); });
	ctx->DeferDestroy(std::move(task));
}
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::ShaderModuleHandle handle) {
	if (handle.empty()) { return; };
	auto sm = ctx->shaderModules_.get(handle);
	vkDestroyShaderModule(device_, sm->module, NULL);
	ctx->shaderModules_.destroy(handle);
}
namespace smvk { namespace {
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
		vmaDestroyImage(
			ctx->m_allocator,
			image->vkImage,
			image->allocation
		);
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
} } // namespace smvk::<anonymous>
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


inline void ImageBarrier(
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
inline smvk::Submitter::Submitter(VulkanCore& core, uint32_t queueFamilyIndex)
	: core_(core), m_queueFamilyIndex(queueFamilyIndex)
{
	if (queueFamilyIndex == core.indices.transferFamily.value_or(queueFamilyIndex)) {
		m_queue = transferQueue_ != VK_NULL_HANDLE ? transferQueue_ : graphicsQueue_;
		if (queueFamilyIndex == core.indices.graphicsFamily.value()) m_queue = graphicsQueue_;
		QueueFamilyIndices q = findQueueFamilies(physicalDevice_, surface_);
		if (q.transferFamily && *q.transferFamily == queueFamilyIndex) m_queue = transferQueue_;
		else if (q.graphicsFamily && *q.graphicsFamily == queueFamilyIndex) m_queue = graphicsQueue_;
		else if (q.presentFamily && *q.presentFamily == queueFamilyIndex) m_queue = presentQueue_;
	}
	else {
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
		VK_CHECK(vkQueueSubmit2(
			m_queue,
			1,
			&submitInfo,
			VK_NULL_HANDLE));
	}

	slot.state_ = CommandState::Submitted;
	slot.retireValue_ = signalValue;

	m_lastTimelineValue = signalValue;
	m_lastSubmit = handle;

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

#ifdef DEBUG_COMMAND_SUBMISSION
		fprintf(stderr,
			"FATAL: timeline semaphore became UINT64_MAX\n");
#endif
		abort();
	}

	if (completedValue >= m_timelineValue) {

#ifdef DEBUG_COMMAND_SUBMISSION

		fprintf(stderr,
			"FATAL: timeline ahead of Submitter: "
			"completed=%llu next=%llu\n",
			(unsigned long long)completedValue,
			(unsigned long long)m_timelineValue);
#endif

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
	//if (handle.index_ >= CommandBufferPool::kMax) { assert(false && "invalid handle"); return; }
	//if (handle.pool == nullptr) { assert(false && "wait: null handle.pool"); return; }
	//auto* pool = handle.pool;
	//WrapperSlot& slot = pool->slots[handle.index_];

	//// Already recycled.
	//if (handle.gen_ < slot.gen_)
	//	return;

	//assert(validHandle(handle));

	CommandBufferPool* pool = handle.pool;
	WrapperSlot& slot = pool->slots[handle.index_];
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
smvk::VulkanSwapchain::VulkanSwapchain(VulkanResourceManager& context, GLFWwindow* window/*, uint32_t framesInFlight, uint32_t desiredImageCount*/)
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
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		poolInfo.queueFamilyIndex = *indices.graphicsFamily;
		VK_CHECK(vkCreateCommandPool(
			device_,
			&poolInfo,
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
	CreateTimestampPools();
}
smvk::VulkanSwapchain::~VulkanSwapchain() {

	vkDeviceWaitIdle(device_);
	DestroyTimestampPools();
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		vkDestroyCommandPool(device_, perFrameCommandPools[i], nullptr);
		// this freeds all the commands too
	};

	DestroySyncObjects();
	CleanupSwapchain(true);
}
void smvk::VulkanSwapchain::Recreate() {
	// No new sync objects. Should investigate, can iamge# change? on OOD
	int width = 0;
	int height = 0;

	glfwGetFramebufferSize(window_, &width, &height);
	while (width == 0 || height == 0) {
		glfwGetFramebufferSize(window_, &width, &height);
		glfwWaitEvents();
	}

	glfwGetWindowSize(window_, &windowWidth, &windowHeight);

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
	// Handle sync objects resize if imageCount or framesInFlight changed (hybrid timeline)
	if (renderFinishedSemaphores_.size() != images_.size() || imageTimelineValues_.size() != images_.size()) {
		for (auto s : renderFinishedSemaphores_) if (s) vkDestroySemaphore(device_, s, nullptr);
		renderFinishedSemaphores_.clear();
		imageTimelineValues_.assign(images_.size(), 0);
		VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		renderFinishedSemaphores_.resize(images_.size(), VK_NULL_HANDLE);
		for (size_t i = 0; i < images_.size(); ++i) vkCreateSemaphore(device_, &info, nullptr, &renderFinishedSemaphores_[i]);
	}
	if (frameTimelineValues_.size() != framesInFlight_) {
		frameTimelineValues_.assign(framesInFlight_, 0);
		for (auto s : imageAvailableSemaphores_) if (s) vkDestroySemaphore(device_, s, nullptr);
		imageAvailableSemaphores_.clear();
		inFlightFences_.clear();
		VkSemaphoreCreateInfo sinfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkFenceCreateInfo finfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
		imageAvailableSemaphores_.resize(framesInFlight_, VK_NULL_HANDLE);
		inFlightFences_.resize(framesInFlight_, VK_NULL_HANDLE);
		for (size_t i = 0; i < framesInFlight_; ++i) {
			vkCreateSemaphore(device_, &sinfo, nullptr, &imageAvailableSemaphores_[i]);
			vkCreateFence(device_, &finfo, nullptr, &inFlightFences_[i]);
		}
	}
	CreateDepthResources();

	currentImageIndex_ = 0;
}
void smvk::VulkanSwapchain::CreateSwapchain() {

	SwapChainSupportDetails support = querySwapChainSupport(physicalDevice_, surface_);
	VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
	VkExtent2D swapExtent = chooseSwapExtent(support.capabilities, window_);
	// Requested mode wins when supported; otherwise prefer no-VSync
	// fallbacks (mailbox, then immediate) before guaranteed FIFO.
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
	renderFinishedSemaphores_.resize(actualImageCount, VK_NULL_HANDLE);
	vkGetSwapchainImagesKHR(device_, vk_swapchain_, &actualImageCount, images_.data());


	this->surfaceFormat = fromVkFormat(surfaceFormat.format);
	this->extent_ = swapExtent;

	glfwGetWindowSize(window_, &windowWidth, &windowHeight);
}
std::vector<VkPresentModeKHR> smvk::VulkanSwapchain::GetAvailablePresentModes() const {
	return querySwapChainSupport(physicalDevice_, surface_).presentModes;
}
void smvk::VulkanSwapchain::CreateTimestampPools() {
	DestroyTimestampPools();
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(physicalDevice_, &props);
	timestampPeriod_ = props.limits.timestampPeriod;
	VkQueryPoolCreateInfo info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	info.queryCount = 2;
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		if (vkCreateQueryPool(device_, &info, nullptr, &timestampPools_[i]) != VK_SUCCESS)
			timestampPools_[i] = VK_NULL_HANDLE;
		timestampWritten_[i] = false;
	}
	gpuTimingValid_ = false;
	lastGpuMs_ = 0.0;
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
inline void smvk::VulkanSwapchain::CreateSyncObjects() {
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
smvk::VulkanFrameBuffer smvk::VulkanSwapchain::AcquireFramebuffer()
{
	SMVK_PROFILE_SCOPE_COLOR("AcquireFramebuffer", Optick::Color::SteelBlue);
	uint64_t waitVal = frameTimelineValues_[currentFrame_];
	if (waitVal != 0) {
		VkSemaphoreWaitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &timelineSemaphore_, &waitVal };
		VkResult waitResult = vkWaitSemaphores(device_, &waitInfo, UINT64_MAX);
		if (waitResult != VK_SUCCESS) {
			throw std::runtime_error("Failed to wait for timeline semaphore.");
		}
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
	}

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(
		device_,
		vk_swapchain_,
		UINT64_MAX,
		imageAvailableSemaphores_[currentFrame_],
		VK_NULL_HANDLE,
		&imageIndex
	);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		Recreate();
		return AcquireFramebuffer();
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	SetCurrentImageIndex(imageIndex);

	// Engine globals: the timeline wait above guarantees this frame's slice
	// retired, so it can be rewritten before any pass records. The
	// update-after-bind descriptor rewrite is visible to commands recorded
	// from now on; previously recorded frames keep their own slice.
	ctx_.FlushEngineGlobals(currentFrame_);

	vkResetCommandPool(device_, perFrameCommandPools[currentFrame_], 0);

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
		vkCmdResetQueryPool(commandBuffers[currentFrame_], timestampPools_[currentFrame_], 0, 2);
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
	ImageBarrier(
		fb.commandBuffer,
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
		vkCmdWriteTimestamp(fb.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			timestampPools_[fb.frameIndex], 1);
		timestampWritten_[fb.frameIndex] = true;
	}
	if (vkEndCommandBuffer(fb.commandBuffer) != VK_SUCCESS) {
		throw std::runtime_error("failed to record command buffer!");
	}

	VkSemaphoreSubmitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, imageAvailableSemaphores_[currentFrame_], 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0 };
	VkSemaphoreSubmitInfo signalInfos[2]{};
	signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signalInfos[0].semaphore = renderFinishedSemaphores_[currentImageIndex_];
	signalInfos[0].value = 0;
	signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	signalInfos[0].deviceIndex = 0;
	signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signalInfos[1].semaphore = timelineSemaphore_;
	signalInfos[1].value = ++timelineValue_;
	signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	signalInfos[1].deviceIndex = 0;
	VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, fb.commandBuffer, 0 };
	VkSubmitInfo2 submitInfo2{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, 1, &waitInfo, 1, &cmdInfo, 2, signalInfos };
	frameTimelineValues_[currentFrame_] = timelineValue_;
	imageTimelineValues_[currentImageIndex_] = timelineValue_;
	{
		// Single-threaded: all queue ops on render thread, but use per-queue mutex for correctness when transferQueue is distinct
		std::mutex& mg = GetQueueMutex(graphicsQueue_);
		std::mutex& mp = GetQueueMutex(presentQueue_);
		std::unique_lock<std::mutex> lockG(mg, std::defer_lock);
		std::unique_lock<std::mutex> lockP(mp, std::defer_lock);
		if (&mg == &mp) {
			lockG.lock();
			VK_CHECK(vkQueueSubmit2(graphicsQueue_, 1, &submitInfo2, VK_NULL_HANDLE));
			// Publish the retirement point for deferred destroys: any resource
			// freed from here on retires once this submit (and all before it)
			// has completed.
			ctx_.NotifyGraphicsSubmit(timelineSemaphore_, timelineValue_);
			VkPresentInfoKHR presentInfo{};
			presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = &renderFinishedSemaphores_[currentImageIndex_];
			VkSwapchainKHR swapChains[] = { vk_swapchain_ };
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = swapChains;
			presentInfo.pImageIndices = &currentImageIndex_;
			// FIFO VSync throttles inside vkQueuePresentKHR (vertical-blank
			// wait). Time it so the caller can report it as wait, not CPU.
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
			AdvanceFrame();
			return;
		}
		else {
			{
				std::lock_guard<std::mutex> lock(mg);
				VK_CHECK(vkQueueSubmit2(graphicsQueue_, 1, &submitInfo2, VK_NULL_HANDLE));
				// Same retirement publish as the single-mutex path above.
				ctx_.NotifyGraphicsSubmit(timelineSemaphore_, timelineValue_);
			}
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
			}
			AdvanceFrame();
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
			toVkPolygonMode(
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

	constexpr VkDynamicState dynamicStates[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
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

	if (volkInitialize() != VK_SUCCESS) {
		std::cerr << "Failed to initialize Volk\n";
		glfwTerminate();
		return false;
	};

	// GLFW;

	if (!glfwInit()) {
		std::cerr << "Failed to initialize GLFW\n";
		return false;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	int width_ = DEFAULT_WIDTH;
	int height_ = DEFAULT_HEIGHT;
	window = glfwCreateWindow(width_, height_, "Vulkan", nullptr, nullptr);

	if (!window) {
		std::cerr << "Failed to create GLFW window\n";
		glfwTerminate();
		return false;
	}
	glfwMakeContextCurrent(window);
	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	glfwSetWindowTitle(window, /*ENGINE_NAME*/ "samuelmartin.1999@outlok.com");

	glfwSetKeyCallback(window, keyCallback);
	glfwSetMouseButtonCallback(window, mouseButtonCallback);
	glfwSetCursorPosCallback(window, cursorPositionCallback);
	glfwSetScrollCallback(window, scrollCallback);

	glfwSetWindowUserPointer(window, this);
	glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

	m_context = std::make_unique<ResourceManager>(window);
	m_swapchain = std::make_unique<Swapchain>(
		*m_context,
		window
	);

	m_rendergraph = std::make_unique<RenderGraph>(*m_context);
	m_world = std::make_unique<World>();

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
	// Derived Shutdown (TestApp::Shutdown) will run first via virtual
	// dispatch then this base Shutdown. TestApp should release its Holders
	// that reference m_context before we reset the context below.
	// We do not reset m_* here to allow TestApp::Shutdown to still use
	// m_context if needed; actual destruction happens in ~VulkanApp via
	// member order (TestApp members -> VulkanApp members -> IApp window).
}

smvk::App::~App()
{
	WaitForDeviceIdle();
	// Members m_world/m_rendergraph/m_swapchain/m_context destroyed automatically
	// (App holds them directly; window is last member so destroyed after Vulkan).
	if (window) {
		glfwDestroyWindow(window);
		window = nullptr;
	}
	glfwTerminate();
}


struct smvk::RenderGraph::PipelineCacheOpaque {
	VulkanPipelineCache cache;
	explicit PipelineCacheOpaque(VulkanResourceManager* rm) : cache(rm) {}
};
smvk::RenderGraph::RenderGraph(ResourceManager& ctx) : ctx_(ctx), pipelineCacheOpaque_(std::make_unique<PipelineCacheOpaque>(static_cast<VulkanResourceManager*>(ctx.GetVulkanOpaque()))) {}
smvk::RenderGraph::~RenderGraph() { ReleaseResolvedResources(); }

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

	images_.forEach(
		[this, &vctx](LogicalImage& image)
	{
		if (image.type == ResourceType::External)
		{
			return;
		}

		if (!image.resolvedImageHandle.empty())
		{
			destroy(&vctx, image.resolvedImageHandle);
		}
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
			//CreateTransientImaeg(key, sync_req);
			logicalImage.resolvedImageHandle = vctx.CreateImage(logicalImage.key, nullptr).release();
		};
	}
	);
};
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
	for (const auto& pass : passes_) {
		PassUsage pu;
		// Color attachments – writes
		for (auto& att : pass.colorAttachments_) {
			if (att.view.empty()) continue;
			auto* v = imageViews_.get(att.view);
			if (!v) continue;
			ImageHandle phys{};
			if (v->type == LogicalImageView::Type::ExternalPhysical) {
				// Resolve external view to physical image; if no backing image (swapchain) skip – swapchain sync stays in Acquire/Present
				auto* ext = vctx.imageViews_.get(v->externalViewHandle);
				if (!ext) continue;
				phys = ext->image;
				if (phys.empty()) continue; // swapchain – external sync
			}
			else {
				auto* li = images_.get(v->key.image);
				if (!li) continue;
				phys = li->resolvedImageHandle;
			}
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
				auto* rv = imageViews_.get(att.resolveView);
				if (rv) {
					ImageHandle rphys{};
					if (rv->type == LogicalImageView::Type::ExternalPhysical) {
						auto* rext = vctx.imageViews_.get(rv->externalViewHandle);
						if (rext) rphys = rext->image;
						// swapchain resolve dest has no backing image – external sync
					}
					else {
						auto* rli = images_.get(rv->key.image);
						if (rli) rphys = rli->resolvedImageHandle;
					}
					if (!rphys.empty() && rphys != phys) {
						ImageSyncRequest rreq{};
						rreq.barrierMasks.accessMask = AccessFlags::ColorAttachmentWrite;
						rreq.barrierMasks.stageMask = PipelineStageBit_Fragment;
						rreq.layout = ImageLayout::ColorAttachmentOptimal;
						pu.images.emplace_back(rphys, rreq);
					}
				}
			}
		}
		// Depth attachment
		if (pass.hasDepthAttachment_) {
			auto& att = pass.depthAttachment_;
			if (!att.view.empty()) {
				auto* v = imageViews_.get(att.view);
				if (!v) continue;
				ImageHandle phys{};
				if (v->type == LogicalImageView::Type::ExternalPhysical) {
					auto* ext = vctx.imageViews_.get(v->externalViewHandle);
					if (!ext) continue;
					phys = ext->image;
					if (phys.empty()) continue;
				}
				else {
					auto* li = images_.get(v->key.image);
					if (!li) continue;
					phys = li->resolvedImageHandle;
				}
				if (phys.empty()) continue;
				ImageSyncRequest req{};
				req.barrierMasks.accessMask = AccessFlags::DepthStencilAttachmentWrite | AccessFlags::DepthStencilAttachmentRead;
				req.barrierMasks.stageMask = PipelineStageBit_Fragment;
				req.layout = ImageLayout::DepthStencilAttachmentOptimal;
				pu.images.emplace_back(phys, req);
			}
		}
		// Explicit image hints (includes SetInput/Storage derived)
		for (auto& h : pass.imageHints_) {
			if (h.view.empty()) continue;
			auto* v = imageViews_.get(h.view);
			if (!v) continue;
			ImageHandle phys{};
			if (v->type == LogicalImageView::Type::ExternalPhysical) {
				auto* ext = vctx.imageViews_.get(v->externalViewHandle);
				if (!ext) continue;
				phys = ext->image;
				if (phys.empty()) continue; // swapchain – skip
			}
			else {
				auto* li = images_.get(v->key.image);
				if (!li) continue;
				phys = li->resolvedImageHandle;
			}
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

	std::vector<VkImageMemoryBarrier2> imgBarriers;
	std::vector<VkBufferMemoryBarrier2> bufBarriers;
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
void smvk::RenderGraph::Execute(Framebuffer& fb)
{
	ResolveImages();
	ResolveImageViews();
	ResolveBuffers();

	auto usages = CollectPassUsages();
	const std::vector<size_t>& sorted = TopologicalSort(usages);

	VkCommandBuffer cmd =
		reinterpret_cast<VkCommandBuffer>(fb.commandBufferOpaque);
	VulkanResourceManager& vctx = *static_cast<VulkanResourceManager*>(ctx_.GetVulkanOpaque());
	auto imageStates = vctx.SnapshotImageSyncStates();
	auto bufferStates = vctx.SnapshotBufferSyncStates();

	// One GPU context per command buffer: every pass scope below (and any
	// GPU event emitted inside a recording lambda) writes its timestamp
	// pairs into this context.
	SMVK_PROFILE_GPU_CONTEXT(cmd);

	for (size_t sIdx : sorted) {
		// Per-pass CPU+GPU scope covering barriers + recording. RenderPass
		// names may be null (default ctor); Optick hashes the name and a
		// null pointer crashes the profiler, so substitute a stable literal.
		::smvk::profiler::GpuPassScope passGpuScope(
			passes_[sIdx].name_ ? passes_[sIdx].name_ : "UnnamedRenderPass",
			__FILE__, __LINE__);
		// Per-pass barriers (fix #1) – update imageStates/bufferStates as we go
		{
			const auto& pu = usages[sIdx];
			std::vector<VkImageMemoryBarrier2> imgBarriers;
			std::vector<VkBufferMemoryBarrier2> bufBarriers;
			imgBarriers.reserve(4);
			bufBarriers.reserve(4);
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
				if (needBarrier) {
					auto* img = vctx.images_.get(h);
					if (!img || img->vkImage == VK_NULL_HANDLE) continue;
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
				}
				VulkanResourceManager::ImageGlobalSyncState dstState{};
				dstState.masks = req.barrierMasks;
				dstState.layout = req.layout;
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
				bufferStates[k] = dstState;
			}
			if (!imgBarriers.empty() || !bufBarriers.empty()) {
				VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				dep.imageMemoryBarrierCount = static_cast<uint32_t>(imgBarriers.size());
				dep.pImageMemoryBarriers = imgBarriers.empty() ? nullptr : imgBarriers.data();
				dep.bufferMemoryBarrierCount = static_cast<uint32_t>(bufBarriers.size());
				dep.pBufferMemoryBarriers = bufBarriers.empty() ? nullptr : bufBarriers.data();
				vkCmdPipelineBarrier2(cmd, &dep);
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
		VkRenderingInfo renderInfo{ .sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, .renderArea = {.offset = renderOffset, .extent = renderExtent, }, .layerCount = 1, .colorAttachmentCount = static_cast<uint32_t>(colorInfos.size()), .pColorAttachments = colorInfos.data(), };
		renderInfo.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
		renderInfo.pStencilAttachment = hasStencil ? &depthInfo : nullptr;
		if (pass.IsCompute()) {
			// Compute pass: no render targets, run recording directly.
			// Barriers above already transitioned hinted images/buffers.
			CommandBuffer cb{ reinterpret_cast<void*>(cmd), *this, pass };
			if (pass.recordingFunction_) pass.recordingFunction_(cb);
			continue;
		}
		VkViewport vp{}; vp.x = static_cast<float>(pass.offsetX_); vp.y = static_cast<float>(pass.offsetY_); vp.width = static_cast<float>(pass.extent_.width); vp.height = static_cast<float>(pass.extent_.height); vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
		VkRect2D scissor{}; scissor.offset = { pass.offsetX_, pass.offsetY_ }; scissor.extent = { pass.extent_.width, pass.extent_.height };
		vkCmdSetViewport(cmd, 0, 1, &vp);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdBeginRendering(cmd, &renderInfo);
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
			vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
		}
		CommandBuffer cb{ reinterpret_cast<void*>(cmd), *this, pass };
		if (pass.recordingFunction_) pass.recordingFunction_(cb);
		vkCmdEndRendering(cmd);
	}

	vctx.CommitImageSyncStates(imageStates);
	vctx.CommitBufferSyncStates(bufferStates);
}
// CommandBuffer – opaque handle firewall: public header stores void* but impl casts to VkCommandBuffer
smvk::CommandBuffer::CommandBuffer(void* cmdOpaque, smvk::RenderGraph& graph, smvk::RenderPass& pass)
	: cmdOpaque_(cmdOpaque)
	, graph_(&graph)
	, renderPass_(&pass)
{}
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
	// redundant vkCmdBind* calls that even the cache's own
	// current-pipeline dedup used to pay.
	if (graphicsBound_
		&& vertexFormat == boundVertexFormat_
		&& topology == boundTopology_
		&& msaaSettings.samples == boundMsaa_.samples
		&& !(depthSettings < boundDepth_) && !(boundDepth_ < depthSettings)
		&& !(stencilSettings < boundStencil_) && !(boundStencil_ < stencilSettings)
		&& !(blendSettings < boundBlend_) && !(boundBlend_ < blendSettings)
		&& !(rasterSettings < boundRaster_) && !(boundRaster_ < rasterSettings)
		&& shaderProgram == boundProgram_)
	{
		currentPipeline_ = graphicsPipeline_;
		currentLayout_ = graphicsLayout_;
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

	vkCmdBindDescriptorSets(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		vulkanCtx.defaultPipeLayout,
		0,
		1,
		vulkanCtx.defaultDescriptorSets_,
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
void smvk::CommandBuffer::DrawIndexedIndirect(BufferHandle buffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
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

	vkCmdBindDescriptorSets(
		reinterpret_cast<VkCommandBuffer>(cmdOpaque_),
		VK_PIPELINE_BIND_POINT_COMPUTE,
		vulkanCtx.defaultPipeLayout,
		0,
		1,
		vulkanCtx.defaultDescriptorSets_,
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

VkInstance smvk::GetVkInstance() { return instance_; }
VkDevice smvk::GetVkDevice() { return device_; }
bool smvk::SupportsMultiDrawIndirect() { return g_multiDrawIndirect; }
VkPhysicalDevice smvk::GetVkPhysicalDevice() { return physicalDevice_; }
VkQueue smvk::GetVkGraphicsQueue() { return graphicsQueue_; }
VkQueue smvk::GetVkPresentQueue() { return presentQueue_; }
VkQueue smvk::GetVkTransferQueue() { return transferQueue_; }
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

