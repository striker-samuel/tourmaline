#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>

#include "volk.h"
#include <GLFW/glfw3.h>
#include <Engine/Vulkan/impl.h>

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
VkDevice smvk::VulkanCore::GetDevice() { return device_; }
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
	VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,              // BufferType_Index
	VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,             // BufferType_Vertex
	VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Uniform
	VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Storage
	VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,           // BufferType_Indirect
	VK_BUFFER_USAGE_TRANSFER_SRC_BIT,              // BufferType_TransferSrc
	VK_BUFFER_USAGE_TRANSFER_DST_BIT,              // BufferType_TransferDst
	VK_BUFFER_USAGE_TRANSFER_SRC_BIT,              // BufferType_Staging: transfer with host visibility
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

/*constexpr*/ bool              /*smvk::*/FormatHasStencil(VkFormat format)
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
/*constexpr*/ bool              /*smvk::*/FormatHasDepth(VkFormat format)
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
static constexpr VkImageAspectFlags VkAspectMaskFromFormat(VkFormat format)
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
[[nodiscard]] inline smvk::ImageAspect DefaultSampleAspect(smvk::Format format)
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

	VkValidationFeaturesEXT validationFeatures{};
	validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
	validationFeatures.pNext = &debugCreateInfo;
	validationFeatures.disabledValidationFeatureCount = 1;
	validationFeatures.pDisabledValidationFeatures = disabledValidationFeatures;

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

			VkPhysicalDeviceVulkan11Features deviceFeatures11
			{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
				.pNext = nullptr,
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

			// if version == 1.4
			//VkPhysicalDeviceVulkan14Features deviceFeatures14
			//{
			//	.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
			//	.pNext = &deviceFeatures13,
			//};

			VkPhysicalDeviceFeatures2 feats2{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
				.pNext = &deviceFeatures13,
				.features = {
					.samplerAnisotropy = VK_TRUE,
					.textureCompressionBC = VK_TRUE, // REQUIRED for BC1..BC7
				},
			};

			// Missing features macro, right now I dont want that much portability nor correctness do rawdogging extensions!
			//#define CHECK_VULKAN_FEATURE(reqFeatures, availFeatures, feature, version)     \
			//  if ((reqFeatures.feature == VK_TRUE) && (availFeatures.feature == VK_FALSE)) \
			//    missingFeatures.append("\n   " version " ." #feature);

			VkDeviceCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
			createInfo.pQueueCreateInfos = queueCreateInfos.data();
			createInfo.pEnabledFeatures = nullptr;
			createInfo.pNext = &feats2;
			createInfo.enabledExtensionCount = static_cast<uint32_t>(config_.deviceExtensions.size());
			createInfo.ppEnabledExtensionNames = config_.deviceExtensions.data();
			createInfo.enabledLayerCount = 0;

			if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
				throw std::runtime_error("failed to create logical device!");
			}
			volkLoadDevice(device_);

		}

		QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
		vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_); // this is fine like this
		vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_); // this maybe not 
	};

	return true;

}
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
void smvk::VulkanCore::CreateGraphicsPipeline(VkGraphicsPipelineCreateInfo createInfo, VkPipeline& pipeline) {
	assert(!pipeline);
	if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &createInfo,
		nullptr, &pipeline) != VK_SUCCESS)
	{
		throw std::runtime_error("failed to create graphics pipeline!");
	}

}
void smvk::VulkanCore::CreateComputePipeline(VkComputePipelineCreateInfo createInfo, VkPipeline& pipeline)
{
	assert(!pipeline);
	if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &createInfo,
		nullptr, &pipeline) != VK_SUCCESS)
	{
		throw std::runtime_error("failed to create graphics pipeline!");
	}
}
void smvk::VulkanCore::DestroyGraphicsPipeline(VkPipeline pipeline) {
	vkDestroyPipeline(device_, pipeline, nullptr);
}
void smvk::VulkanCore::CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler) {

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;

	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
	samplerInfo.anisotropyEnable = VK_TRUE;
	samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = toVkCompareOp(samplerCreateInfo.compareOp);

	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = samplerCreateInfo.maxLod;
	//samplerInfo.maxLod = float(samplerCreateInfo.levelCount - 1);

	VK_CHECK(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler));

}
inline smvk::VulkanCore::~VulkanCore() {
	vkDeviceWaitIdle(device_);
	//vkDestroyCommandPool(device_, defaultCommandPool_, nullptr);
	//vkDestroyCommandPool(device_, transferCommandPool_, nullptr);
	if (surface_ != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(instance_, surface_, nullptr);
	}
	DestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
	vkDestroyDevice(device_, nullptr);
	vkDestroyInstance(instance_, nullptr);
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
static inline VkDescriptorSetLayoutBinding getDSLBinding(uint32_t binding,
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

smvk::VulkanResourceManager::VulkanResourceManager(VulkanCore& core) :core_(core)
{
	VmaAllocatorCreateInfo allocatorCreateInfo = {};
	allocatorCreateInfo.physicalDevice = physicalDevice_;
	allocatorCreateInfo.device = device_;
	allocatorCreateInfo.instance = instance_;
	allocatorCreateInfo.vulkanApiVersion = core_.config_.version_ == VK_1_3 ? VK_API_VERSION_1_3 : UINT32_MAX;
	allocatorCreateInfo.flags =
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT |
		VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (allocatorCreateInfo.vulkanApiVersion == UINT32_MAX) { assert(false); };

	VmaVulkanFunctions vulkanFunctions;
	VK_CHECK(vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vulkanFunctions));
	allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;
	VK_CHECK(vmaCreateAllocator(&allocatorCreateInfo, &m_allocator));

	m_submitter = new Submitter(core_, core_.indices.graphicsFamily.value());

	// Urges creating the SamplerPool -> key mapping to samplers!
	core_.CreateSampler(SamplerCreateInfo{}, m_defaultSampler);
	core_.CreateSampler(
		SamplerCreateInfo{
		.compareOp = smvk::CompareOp::LessEqual,
		.compareEnable = true
		},
		m_defaultShadowSampler
	);
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

	// Engine Wide PC block
	//static constexpr uint32_t EnginePushConstantSize = 128;
	static constexpr uint32_t EnginePushConstantSize = 256;
	VkPushConstantRange range{
		.stageFlags = VK_SHADER_STAGE_ALL,
		.offset = 0,
		.size = EnginePushConstantSize,
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
	VK_CHECK(vkDeviceWaitIdle(device_));

	destroy(this, m_nilTexture);

	ProcessDeferrredTasks();
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
			<< ", gen=" << handle.gen() << ", module=" << *sm << "\n";
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
void smvk::VulkanResourceManager::UploadDataToBuffer(
	BufferHandle buffer,
	const void* data,
	size_t dataSize,
	VkPipelineStageFlags2 dstStages,
	VkAccessFlags2 dstAccess,
	size_t src_offset, // unused
	size_t dest_offset
) {

	auto buf = buffers_.get(buffer);
	assert(buffer.valid());
	assert(buf->vkBuffer != VK_NULL_HANDLE);

	auto han = m_submitter->acquire();
	auto cmd = m_submitter->GetCommandBuffer(han);
	if (buf->isMappable()) {
		VK_CHECK(vmaCopyMemoryToAllocation(m_allocator, data, buf->vma_alloc, dest_offset, dataSize));
		// this would be better served with a sync request pushed to a list.
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
		return;
	}

	auto stageHan = CreateBuffer(dataSize, smvk::BufferType_Staging, "staging buffer").release();
	// this invalidates buf*, so lets get it again.
	buf = buffers_.get(buffer);
	auto stagingBuf = buffers_.get(stageHan);
	assert(stagingBuf->vkBuffer != VK_NULL_HANDLE);

	VK_CHECK(vmaCopyMemoryToAllocation(m_allocator, data, stagingBuf->vma_alloc, 0, dataSize));

	// Copy staging -> dst
	VkBufferCopy copyRegion{
		.srcOffset = 0,	// we have the staging buf created for this so just from 0.
		.dstOffset = dest_offset,
		.size = dataSize
	};
	vkCmdCopyBuffer(cmd, stagingBuf->vkBuffer, buf->vkBuffer, 1, &copyRegion);

	BufferBarrier(
		cmd,
		buf->vkBuffer,
		dest_offset,
		dataSize,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		VK_ACCESS_2_TRANSFER_WRITE_BIT,
		dstStages,
		dstAccess
	);

	m_submitter->submit(han);
	AddDeferredTask(
		std::packaged_task<void()>([this, staging = std::move(stageHan)] {destroy(this, staging); })
		, han
	);
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
	resource->descriptorIndex = handle.index();

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
		data
	);

	const Image* imageResource =
		images_.get(image);

	assert(imageResource);


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
[[nodiscard]] smvk::Holder<smvk::TextureHandle> smvk::VulkanResourceManager::CreateTexture(ktxTexture2& texture, const char* debugName)
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

	VkBufferCreateInfo stagingCI{
		VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
	};

	stagingCI.size =
		static_cast<VkDeviceSize>(
			ktxDataSize
			);

	stagingCI.usage =
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

	stagingCI.sharingMode =
		VK_SHARING_MODE_EXCLUSIVE;


	VmaAllocationCreateInfo stagingAllocCI{};

	stagingAllocCI.usage =
		VMA_MEMORY_USAGE_AUTO;

	stagingAllocCI.flags =
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
		VMA_ALLOCATION_CREATE_MAPPED_BIT;


	VkBuffer stagingBuffer =
		VK_NULL_HANDLE;

	VmaAllocation stagingAllocation =
		VK_NULL_HANDLE;

	VmaAllocationInfo stagingAllocationInfo{};


	const VkResult stagingResult =
		vmaCreateBuffer(
			m_allocator,
			&stagingCI,
			&stagingAllocCI,
			&stagingBuffer,
			&stagingAllocation,
			&stagingAllocationInfo
		);

	if (stagingResult != VK_SUCCESS)
	{
		// Image/View/Bindless are automatically cleaned up by Holders.
		throw std::runtime_error(
			"vmaCreateBuffer failed for KTX staging buffer."
		);
	}


	// -------------------------------------------------------------------------
	// Upload CPU -> staging
	// -------------------------------------------------------------------------

	assert(
		stagingAllocationInfo.pMappedData
	);

	std::memcpy(
		stagingAllocationInfo.pMappedData,
		ktxData,
		static_cast<size_t>(
			ktxDataSize
			)
	);

	vmaFlushAllocation(
		m_allocator,
		stagingAllocation,
		0,
		VK_WHOLE_SIZE
	);


	// -------------------------------------------------------------------------
	// Record upload
	// -------------------------------------------------------------------------

	auto submitHandle =
		m_submitter->acquire();

	auto cmd =
		m_submitter
		->GetCommandBuffer(
			submitHandle
		);


	// -------------------------------------------------------------------------
	// UNDEFINED -> TRANSFER_DST
	// -------------------------------------------------------------------------

	{
		VkImageMemoryBarrier barrier{
			VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
		};

		barrier.srcAccessMask =
			0;

		barrier.dstAccessMask =
			VK_ACCESS_TRANSFER_WRITE_BIT;

		barrier.oldLayout =
			VK_IMAGE_LAYOUT_UNDEFINED;

		barrier.newLayout =
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

		barrier.srcQueueFamilyIndex =
			VK_QUEUE_FAMILY_IGNORED;

		barrier.dstQueueFamilyIndex =
			VK_QUEUE_FAMILY_IGNORED;

		barrier.image =
			vkImage;

		barrier.subresourceRange = {
			.aspectMask =
				VK_IMAGE_ASPECT_COLOR_BIT,

			.baseMipLevel =
				0,

			.levelCount =
				mipLevels,

			.baseArrayLayer =
				0,

			.layerCount =
				imageCI.arrayLayers
		};

		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier
		);
	}


	// -------------------------------------------------------------------------
	// Copy
	// -------------------------------------------------------------------------

	vkCmdCopyBufferToImage(
		cmd,
		stagingBuffer,
		vkImage,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		static_cast<uint32_t>(
			regions.size()
			),
		regions.data()
	);


	// -------------------------------------------------------------------------
	// TRANSFER_DST -> SHADER_READ_ONLY
	//
	// Intentionally fragment-only until the general transition API exists.
	// -------------------------------------------------------------------------

	{
		VkImageMemoryBarrier barrier{
			VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
		};

		barrier.srcAccessMask =
			VK_ACCESS_TRANSFER_WRITE_BIT;

		barrier.dstAccessMask =
			VK_ACCESS_SHADER_READ_BIT;

		barrier.oldLayout =
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

		barrier.newLayout =
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		barrier.srcQueueFamilyIndex =
			VK_QUEUE_FAMILY_IGNORED;

		barrier.dstQueueFamilyIndex =
			VK_QUEUE_FAMILY_IGNORED;

		barrier.image =
			vkImage;

		barrier.subresourceRange = {
			.aspectMask =
				VK_IMAGE_ASPECT_COLOR_BIT,

			.baseMipLevel =
				0,

			.levelCount =
				mipLevels,

			.baseArrayLayer =
				0,

			.layerCount =
				imageCI.arrayLayers
		};

		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier
		);
	}


	m_submitter->submit(
		submitHandle
	);


	// -------------------------------------------------------------------------
	// Deferred staging cleanup
	// -------------------------------------------------------------------------

	AddDeferredTask(
		std::packaged_task<void()>(
			[
				this,
				buffer = stagingBuffer,
				allocation = stagingAllocation
			]
	{
		vmaDestroyBuffer(
			m_allocator,
			buffer,
			allocation
		);
	}
		),
		submitHandle
	);


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
[[nodiscard]] smvk::ImageViewHandle smvk::VulkanResourceManager::RegisterImageView(
	VkImageView vkView,
	Format format,
	const char* name)
{
	assert(vkView != VK_NULL_HANDLE);

	ImageView view{};
	view.vkView = vkView;
	view.format = format;
	view.ownership = ResourceOwnership::External;

	return imageViews_.create(std::move(view));
}

void smvk::VulkanResourceManager::SetImageViewVkView(
	ImageViewHandle handle,
	VkImageView vkView)
{
	auto* view = imageViews_.get(handle);
	assert(view != nullptr);
	view->vkView = vkView;
}

smvk::ImageViewHandle smvk::VulkanResourceManager::RecreateImageView(
	ImageViewHandle handle,
	ImageHandle imageHandle,
	const ImageViewCreateInfo& ci)
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

smvk::ImageHandle smvk::VulkanResourceManager::RecreateImage(
	ImageHandle handle,
	const ImageCreateInfo& ci,
	const void* data)
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

	return handle;
}

[[nodiscard]] smvk::Holder<smvk::BufferHandle> smvk::VulkanResourceManager::CreateBuffer(VkDeviceSize size, BufferType bufferType, const char* debugName) {
	Buffer desc{};
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size;
	ci.usage = bufferTypeToUsageFlags[static_cast<size_t>(bufferType)];
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo aci{};
	aci.usage = bufferTypeToVmaUsage[static_cast<size_t>(bufferType)];
	if (bufferType == BufferType_Staging) {
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
		bufferType == BufferType_Indirect)
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
	return { this, buffers_.create(std::move(desc)) };
}
[[nodiscard]] smvk::Holder<smvk::ShaderModuleHandle> smvk::VulkanResourceManager::CreateShaderModule(std::vector<char> code, const char* debugName) {
	VkShaderModule shader;
	CreateShaderModuleIn(
		std::move(code),
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
	return { this, shaderModules_.create(std::move(shader)) };
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
	void* mapped = nullptr;
	assert(!handle.empty());
	auto buf = this->buffers_.get(handle);
	assert(buf != nullptr);
	VK_CHECK(vmaMapMemory(m_allocator, buf->vma_alloc, &mapped));
	return mapped;
}
void smvk::VulkanResourceManager::UnmapBuffer(BufferHandle handle) {
	assert(!handle.empty());
	auto buf = this->buffers_.get(handle);
	assert(buf != nullptr);
	// Not required for HOST_COHERENT memory, but harmless if you want to support
	// non-coherent staging buffers later.
	// vmaFlushAllocation(m_allocator, staging->vma_alloc, 0, stagingBufferSize);
	vmaUnmapMemory(m_allocator, buf->vma_alloc);
}
void smvk::VulkanResourceManager::AddDeferredTask(std::packaged_task<void()>&& task, smvk::CommandSubmitHandle handle)
{
	if (handle.gen_ == 0) //not valid acquire 
	{
		handle = m_submitter->acquire();
	}
	m_deferredTasks.emplace_back(std::move(task), handle);
}
void smvk::VulkanResourceManager::ProcessDeferrredTasks()
{
	std::vector<DeferredTask>::iterator it = m_deferredTasks.begin();
	while (it != m_deferredTasks.end() && m_submitter->isReady(it->handle_)) {
		(it++)->task_();
	}
	m_deferredTasks.erase(m_deferredTasks.begin(), it);
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

	//
		// Sampler descriptor array:
		//
		//   samplers[0] = default sampler
		//   samplers[1] = default shadow/comparison sampler
		//
	VkDescriptorImageInfo samplerInfos[2]{};
	samplerInfos[0].sampler =
		m_defaultSampler;
	samplerInfos[1].sampler =
		m_defaultShadowSampler;

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
			2;

		samplerWrite.pImageInfo =
			samplerInfos;

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
[[nodiscard]] uint32_t smvk::VulkanResourceManager::GetBindlessIndex(BindlessImageHandle handle) const
{
	assert(!handle.empty());
	return handle.index();
	//const BindlessImage* bindless =
	//	bindlessImages_.get(handle);
	//assert(bindless != nullptr);
	//assert(bindless->descriptorIndex != UINT32_MAX);
	//return bindless->descriptorIndex;
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
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::BufferHandle handle) {
	if (handle.empty()) { return; };
	smvk::Buffer db = *ctx->buffers_.get(handle);
	//if mapped, unmap first as vma requires.
	//if (db.mapped != nullptr) { ctx->UnmapBuffer(handle); }
	vmaDestroyBuffer(ctx->m_allocator, db.vkBuffer, db.vma_alloc);
	ctx->buffers_.destroy(handle);
}
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::ShaderModuleHandle handle) {
	if (handle.empty()) { return; };
	auto sm = ctx->shaderModules_.get(handle);
	vkDestroyShaderModule(device_, *sm, NULL);
	ctx->shaderModules_.destroy(handle);
}
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::ImageHandle handle)
{
	if (handle.empty())
		return;

	auto* image = ctx->images_.get(handle);
	assert(image);

	// ImageViews must be destroyed first.
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
void smvk::destroy(smvk::VulkanResourceManager* ctx, smvk::ImageViewHandle handle)
{
	if (handle.empty())
		return;

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

// Submitter

inline smvk::Submitter::Submitter(
	VulkanCore& core,
	uint32_t queueIndex)
	: core_(core)
{
	// Fix this according to however VulkanCore stores queues.
	m_queue = graphicsQueue_;

	VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = core.indices.graphicsFamily.value()
	};

	VK_CHECK(vkCreateCommandPool(
		device_,
		&poolInfo,
		nullptr,
		&pool_));

	assert(pool_ != VK_NULL_HANDLE);

	VkCommandBufferAllocateInfo info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool_,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = kMax
	};

	VkCommandBuffer buffers[kMax]{};

	VK_CHECK(vkAllocateCommandBuffers(
		device_,
		&info,
		buffers));

	for (uint32_t i = 0; i < kMax; ++i)
	{
		WrapperSlot& slot = commandBuffers_[i];

		slot.allocatedCmd_ = buffers[i];
		slot.cmd_ = VK_NULL_HANDLE;
		slot.state_ = CommandState::Free;
		slot.retireValue_ = 0;
		slot.gen_ = 1;
	}

	CreateTimelineSemaphore(
		0,
		m_timelineSemaphore);

	assert(m_timelineSemaphore != VK_NULL_HANDLE);

	m_timelineValue = 1;
	m_lastTimelineValue = 0;
}
inline smvk::Submitter::~Submitter()
{
	if (device_ == VK_NULL_HANDLE)
		return;

	waitAll();

#ifndef NDEBUG
	for (uint32_t i = 0; i < kMax; ++i)
	{
		assert(
			commandBuffers_[i].state_ != CommandState::Recording &&
			"Submitter destroyed while command buffer is recording");
	}
#endif

	if (m_timelineSemaphore != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(
			device_,
			m_timelineSemaphore,
			nullptr);

		m_timelineSemaphore = VK_NULL_HANDLE;
	}

	if (pool_ != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(
			device_,
			pool_,
			nullptr);

		pool_ = VK_NULL_HANDLE;
	}
}
smvk::TimelinePoint smvk::Submitter::nextSignalPoint() const
{
	return TimelinePoint{
		.semaphore = m_timelineSemaphore,
		.value = m_timelineValue
	};
}
VkResult smvk::Submitter::waitTimeline(
	uint64_t value,
	uint64_t timeout) const
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

[[nodiscard]]
inline smvk::CommandSubmitHandle
smvk::Submitter::acquire()
{
	purge();

	if (availableSlots == 0)
	{
		uint64_t oldest = UINT64_MAX;

		for (const WrapperSlot& slot : commandBuffers_)
		{
			if (slot.state_ == CommandState::Submitted)
			{
				oldest = std::min(
					oldest,
					slot.retireValue_);
			}
		}

		if (oldest != UINT64_MAX)
		{
			VK_CHECK(waitTimeline(
				oldest,
				UINT64_MAX));

			purge();
		}
		else
		{
			assert(
				false &&
				"All command buffers are currently recording");

			return {};
		}
	}

	uint32_t pickedIndex =
		UINT32_MAX;

	for (uint32_t i = 0; i < kMax; ++i)
	{
		if (commandBuffers_[i].state_ == CommandState::Free)
		{
			pickedIndex = i;
			break;
		}
	}

	assert(pickedIndex != UINT32_MAX);

	WrapperSlot& slot =
		commandBuffers_[pickedIndex];

	slot.cmd_ =
		slot.allocatedCmd_;

	slot.state_ =
		CommandState::Recording;

	slot.retireValue_ =
		0;

	VkCommandBufferBeginInfo beginInfo{
		.sType =
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,

		.flags =
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};

	VK_CHECK(vkBeginCommandBuffer(
		slot.cmd_,
		&beginInfo));

	--availableSlots;

	return CommandSubmitHandle{
		.index_ = pickedIndex,
		.gen_ = slot.gen_
	};
}
[[nodiscard]]
VkCommandBuffer
smvk::Submitter::GetCommandBuffer(
	CommandSubmitHandle handle)
{
	assert(validHandle(handle));

	WrapperSlot& slot =
		commandBuffers_[handle.index_];

	assert(
		slot.state_ == CommandState::Recording);

	return slot.cmd_;
}
smvk::TimelinePoint
smvk::Submitter::submit(
	CommandSubmitHandle handle,
	std::span<const TimelineWait> waits)
{
	assert(validHandle(handle));

	WrapperSlot& slot =
		commandBuffers_[handle.index_];

	assert(
		slot.state_ == CommandState::Recording);

	assert(
		slot.cmd_ != VK_NULL_HANDLE);

	VK_CHECK(vkEndCommandBuffer(
		slot.cmd_));

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

	VK_CHECK(vkQueueSubmit2(
		m_queue,
		1,
		&submitInfo,
		VK_NULL_HANDLE));

	slot.state_ =
		CommandState::Submitted;

	slot.retireValue_ =
		signalValue;

	m_lastTimelineValue =
		signalValue;

	m_lastSubmit =
		handle;

	return TimelinePoint{
		.semaphore =
			m_timelineSemaphore,

		.value =
			signalValue
	};
}
inline bool
smvk::Submitter::validHandle(
	CommandSubmitHandle handle) const
{
	if (handle.index_ >= kMax)
		return false;
	if (handle.gen_ == 0)
		return false;
	return
		handle.gen_ ==
		commandBuffers_[handle.index_].gen_;
}
bool smvk::Submitter::isReady(
	CommandSubmitHandle handle)
{
	if (handle.index_ >= kMax)
		return false;

	WrapperSlot& slot =
		commandBuffers_[handle.index_];

	// Old generation already recycled.
	if (handle.gen_ < slot.gen_)
		return true;

	if (handle.gen_ != slot.gen_)
		return false;

	if (slot.state_ == CommandState::Recording)
		return false;

	if (slot.state_ == CommandState::Free)
		return true;

	uint64_t completed = 0;

	VK_CHECK(vkGetSemaphoreCounterValue(
		device_,
		m_timelineSemaphore,
		&completed));

	return
		completed >=
		slot.retireValue_;
}
void smvk::Submitter::purge()
{
	uint64_t completedValue = 0;

	VK_CHECK(vkGetSemaphoreCounterValue(
		device_,
		m_timelineSemaphore,
		&completedValue));

	for (uint32_t i = 0; i < kMax; ++i)
	{
		WrapperSlot& slot = commandBuffers_[i];

		if (slot.state_ != CommandState::Submitted)
			continue;

		if (slot.retireValue_ <= completedValue)
		{
			recycleSlot(i);
		}
	}
}
inline void smvk::Submitter::wait(
	CommandSubmitHandle handle,
	uint64_t timeout)
{
	if (handle.index_ >= kMax)
	{
		assert(false && "invalid handle");
		return;
	}

	WrapperSlot& slot =
		commandBuffers_[handle.index_];

	// Already recycled.
	if (handle.gen_ < slot.gen_)
		return;

	assert(validHandle(handle));

	assert(
		slot.state_ == CommandState::Submitted &&
		"Cannot wait on command buffer before submit");

	VkResult result =
		waitTimeline(
			slot.retireValue_,
			timeout);

	if (result == VK_TIMEOUT)
	{
		assert(false && "timeout reached");
		return;
	}

	VK_CHECK(result);

	recycleSlot(
		handle.index_);
}
void smvk::Submitter::waitAll()
{
	if (m_lastTimelineValue == 0)
		return;

	VK_CHECK(waitTimeline(
		m_lastTimelineValue,
		UINT64_MAX));

	purge();
}
void smvk::Submitter::recycleSlot(
	uint32_t index)
{
	assert(index < kMax);

	WrapperSlot& slot =
		commandBuffers_[index];

	assert(
		slot.state_ == CommandState::Submitted);

	assert(
		slot.cmd_ != VK_NULL_HANDLE);

	VK_CHECK(vkResetCommandBuffer(
		slot.cmd_,
		0));

	slot.cmd_ =
		VK_NULL_HANDLE;

	slot.state_ =
		CommandState::Free;

	slot.retireValue_ =
		0;

	++slot.gen_;

	if (slot.gen_ == 0)
		slot.gen_ = 1;

	++availableSlots;
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
}
smvk::VulkanSwapchain::~VulkanSwapchain() {

	vkDeviceWaitIdle(device_);
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

	CleanupSwapchain(false);

	CreateSwapchain();
	CreateImageViews();
	CreateDepthResources();

	currentImageIndex_ = 0;
}
void smvk::VulkanSwapchain::CreateSwapchain() {

	SwapChainSupportDetails support = querySwapChainSupport(physicalDevice_, surface_);
	VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
	VkExtent2D swapExtent = chooseSwapExtent(support.capabilities, window_);
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

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
	depthImageCreateInfo.usage = ImageUsage::DepthStencilAttachment;
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
	renderFinishedSemaphores_.resize(imageViews_.size(), VK_NULL_HANDLE);
	imageAvailableSemaphores_.resize(framesInFlight_, VK_NULL_HANDLE);
	inFlightFences_.resize(framesInFlight_, VK_NULL_HANDLE);

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for (size_t i = 0; i < imageViews_.size(); ++i) {
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
smvk::FrameBuffer smvk::VulkanSwapchain::AcquireFramebuffer()
{
	auto f = GetInFlightFence();
	VkResult waitResult = vkWaitForFences(
		device_,
		1,
		&f,
		VK_TRUE,
		UINT64_MAX
	);
	if (waitResult != VK_SUCCESS && waitResult != VK_TIMEOUT) {
		throw std::runtime_error("Failed to wait for fence.");
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

	vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);
	vkResetCommandPool(device_, perFrameCommandPools[currentFrame_], 0);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	if (vkBeginCommandBuffer(commandBuffers[currentFrame_], &beginInfo) != VK_SUCCESS) {
		throw std::runtime_error("failed to begin recording command buffer!");
	}

	VkImageMemoryBarrier barrierToColorAttachment{};
	barrierToColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrierToColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrierToColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrierToColorAttachment.srcAccessMask = 0;
	barrierToColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	// We are suing grapgics and present capable queue. No need to transfer
	barrierToColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrierToColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	barrierToColorAttachment.image = images_[imageIndex];
	barrierToColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrierToColorAttachment.subresourceRange.baseMipLevel = 0;
	barrierToColorAttachment.subresourceRange.levelCount = 1;
	barrierToColorAttachment.subresourceRange.baseArrayLayer = 0;
	barrierToColorAttachment.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(
		commandBuffers[currentFrame_],
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrierToColorAttachment
	);

	FrameBuffer frameBufferContext_{
		.frameIndex = currentFrame_,
		.imageIndex = currentImageIndex_,
		.commandBuffer = commandBuffers[currentFrame_],
		.swapchain = *this

	};
	return frameBufferContext_;
}
void smvk::VulkanSwapchain::EndAndPresentFramebuffer(FrameBuffer& fb)
{
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
	if (vkEndCommandBuffer(fb.commandBuffer) != VK_SUCCESS) {
		throw std::runtime_error("failed to record command buffer!");
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	VkSemaphore waitSemaphores[] = { imageAvailableSemaphores_[currentFrame_] };
	VkPipelineStageFlags waitStages[] = {
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = waitSemaphores;
	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &fb.commandBuffer;
	VkSemaphore signalSemaphores[] = { renderFinishedSemaphores_[currentImageIndex_] };
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;


	VK_CHECK(
		vkQueueSubmit(
			graphicsQueue_,
			1,
			&submitInfo,
			GetInFlightFence()
		)
	);


	//
	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;
	VkSwapchainKHR swapChains[] = { vk_swapchain_ };
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &currentImageIndex_;

	VkResult result = vkQueuePresentKHR(presentQueue_, &presentInfo);

	// this is at eof, this should be earlier right? theres the glfw event.
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
		framebufferResized) {
		framebufferResized = false;
		Recreate();
	}
	else if (result != VK_SUCCESS) {
		throw std::runtime_error("failed to present swap chain image!");
	}
	AdvanceFrame();

}
smvk::StencilSettings smvk::StencilSettings::DefaultDisabled()
{
	StencilSettings s{};
	s.frontTestEnable = false;
	s.backTestEnable = false;
	s.frontWriteEnable = false;
	s.backWriteEnable = false;
	return s;
}
smvk::StencilSettings smvk::StencilSettings::DefaultEnabled()
{
	StencilSettings s{};
	s.frontTestEnable = true;
	s.backTestEnable = true;
	s.frontWriteEnable = true;
	s.backWriteEnable = true;
	return s;
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
smvk::PipelineBundle smvk::PipelineCache::BuildGraphicsPipeline(const smvk::PipelineKey& key)
{
	// ========================================================
	// SHADERS
	// ========================================================

	std::vector<VkPipelineShaderStageCreateInfo>shaderStages;
	shaderStages.reserve(key.program.shaderCount);

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
			.module = *shaderModule,
			.pName = shader.entryPoint,
		};
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
	ctx_->core_.CreateGraphicsPipeline(
		pipelineInfo,
		bundle.pipeline);

	bundle.layout =
		layout;

	return bundle;
}


// RENDERGRAPH
#include <Engine/Renderer/rendergraph.h>

VkImageView GetImageView(smvk::RenderGraph* rg, smvk::LogicalImageViewHandle logicalView) {
	assert(!logicalView.empty());
	auto view = rg->imageViews_.get(logicalView);
	assert(view != nullptr);

		if (view->type == smvk::LogicalImageView::Type::ExternalPhysical)
		{
			assert(!view->externalViewHandle.empty());
			return rg->ctx_.imageViews_.get(view->externalViewHandle)->vkView;
		}

	assert(!view->resolvedViewHandle.empty());
	return rg->ctx_.imageViews_.get(view->resolvedViewHandle)->vkView;
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

		logicalView.resolvedViewHandle =
			ctx_.CreateImageView(
				logicalImage->resolvedImageHandle,
				ci
			).release();
	}
	);
}
void smvk::RenderGraph::ResolveImages() {
	images_.forEach(
		[this](smvk::LogicalImage& logicalImage) {
		if (logicalImage.resolvedImageHandle.empty()) {
			if (logicalImage.type == smvk::ResourceType::External) { assert(false && "external image unresolved. shouldnt be possible"); };
			//CreateTransientImaeg(key, sync_req);
			logicalImage.resolvedImageHandle = ctx_.CreateImage(logicalImage.key, nullptr).release();
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
void smvk::RenderGraph::Execute(smvk::FrameBuffer& fb)
{
	ResolveImages();
	ResolveImageViews();
	ResolveBuffers();

	for (auto& pass : passes_) {

		std::vector<VkRenderingAttachmentInfo> colorInfos;
		colorInfos.reserve(pass.colorAttachments_.size());

		for (auto& att : pass.colorAttachments_)
		{
			VkRenderingAttachmentInfo info{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
			};

			info.imageView = GetImageView(this, att.view);
				info.imageLayout =
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			info.loadOp =
				toVkAttachmentLoadOp(att.loadOp);
			info.storeOp =
				toVkAttachmentStoreOp(att.storeOp);
			info.clearValue =
				toVkClearValue(att.clearColor);
			colorInfos.push_back(info);
		}

		VkRenderingAttachmentInfo depthInfo{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
		};

		const bool hasDepth =
			pass.hasDepthAttachment_;

		bool hasStencil = false;

		if (hasDepth)
		{
			const auto depthFormat =
				GetResolvedImageViewFormat(
					pass.depthAttachment_.view);
			hasStencil =
				FormatHasStencil(
					toVkFormat(depthFormat));
			depthInfo.imageView =
				GetImageView(this,
					pass.depthAttachment_.view);
			depthInfo.imageLayout =
				hasStencil
				? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
				: VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
			depthInfo.loadOp =
				toVkAttachmentLoadOp(
					pass.depthAttachment_.loadOp);
			depthInfo.storeOp =
				toVkAttachmentStoreOp(
					pass.depthAttachment_.storeOp);
			depthInfo.clearValue =
				toVkClearValue(
					pass.depthAttachment_.clear);
		}

		VkRenderingInfo renderInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
			.renderArea = {
				.offset = {pass.offsetX_, pass.offsetY_},
				.extent = {pass.extent_.width, pass.extent_.height},
			},
			.layerCount = 1,
			.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size()),
			.pColorAttachments = colorInfos.data(),
		};
		renderInfo.pDepthAttachment =
			hasDepth ? &depthInfo : nullptr;
		renderInfo.pStencilAttachment =
			hasStencil ? &depthInfo : nullptr;

		VkViewport vp{};
		vp.x = 0.0f;
		vp.y = 0.0f;
		vp.width = static_cast<float>(pass.extent_.width);
		vp.height = static_cast<float>(pass.extent_.height);
		vp.minDepth = 0.0f;
		vp.maxDepth = 1.0f;

		VkRect2D scissor{};
		scissor.offset = { 0, 0 };
		scissor.extent = { pass.extent_.width, pass.extent_.height };

		vkCmdSetViewport(fb.commandBuffer, 0, 1, &vp);
		vkCmdSetScissor(fb.commandBuffer, 0, 1, &scissor);

		vkCmdBeginRendering(fb.commandBuffer, &renderInfo);

		CommandBuffer cmd{
			fb.commandBuffer,
			*this,
			pass
		};

		if (pass.recordingFunction_) {
			pass.recordingFunction_(cmd);
		}

		vkCmdEndRendering(fb.commandBuffer);
	}
};
void smvk::CommandBuffer::BindRenderPipeline(
	ShaderProgram& shaderProgram,
	VertexFormat vertexFormat,
	const DepthSettings& depthSettings,
	const StencilSettings& stencilSettings,
	const BlendSettings& blendSettings,
	const MSAASettings& msaaSettings
)
{
	assert(graph_ != nullptr);
	assert(renderPass_ != nullptr);

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
	VertexFormatDescription formatDescription{};
	formatDescription = GetVertexFormatDescription(vertexFormat);

	std::vector<smvk::Format> colorAttachmentFormats;
	colorAttachmentFormats.reserve(renderPass_->colorAttachments_.size());
	for (auto& attachment : renderPass_->colorAttachments_)
	{
		colorAttachmentFormats.push_back(
			graph_->GetResolvedImageViewFormat(
				attachment.view)
		);
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

	auto bundle = graph_->pipelineCachePimpl->GetRenderPipeline(
		shaderProgram,
		formatDescription,
		colorAttachmentFormats.data(),
		static_cast<uint32_t>(colorAttachmentFormats.size()),
		depthFormat,
		stencilFormat,
		PrimitiveTopology::TriangleList,
		smvk::RasterSettings{},
		depthSettings,
		stencilSettings,
		blendSettings,
		msaaSettings
	);

	assert(bundle.pipeline != VK_NULL_HANDLE && "Invalid pipeline handle");

	if (bundle.pipeline == currentPipeline_) {
		return;
	}

	currentPipeline_ = bundle.pipeline;
	currentPipelineLayout_ = bundle.layout;

	vkCmdBindPipeline(
		cmd_,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		currentPipeline_);

	vkCmdBindDescriptorSets(
		cmd_,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		graph_->ctx_.defaultPipeLayout,
		0,
		1,
		graph_->ctx_.defaultDescriptorSets_,
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
	VkBuffer vertexBuffers[] = {
		graph_->ctx_.buffers_.get(staticVertexFmtHeap.m_staticVertexPosBuffer)->vkBuffer,
		graph_->ctx_.buffers_.get(staticVertexFmtHeap.m_staticVertexShadingBuffer)->vkBuffer
	};

	VkDeviceSize offsets[] = {
		0,
		0
	};

	vkCmdBindVertexBuffers(
		cmd_,
		0,
		2,
		vertexBuffers,
		offsets
	);

	vkCmdBindIndexBuffer(
		cmd_,
		graph_->ctx_.buffers_.get(staticVertexFmtHeap.m_staticIndexBuffer)->vkBuffer,
		0,
		VK_INDEX_TYPE_UINT32
	);
}
void smvk::CommandBuffer::PushConstants(uint32_t offset, uint32_t size, const void* data, ShaderStage stageMask)
{
	assert(currentPipelineLayout_ != VK_NULL_HANDLE);

	const VkShaderStageFlags vkStages =
		toVkShaderStageFlags(stageMask);

	assert(vkStages != 0);

	vkCmdPushConstants(
		cmd_,
		currentPipelineLayout_,
		vkStages,
		offset,
		size,
		data
	);
}
