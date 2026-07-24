#include <algorithm>
#include <cstdint>
#include <iostream>

#include <Engine/Vulkan/core.h>

//
#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif
//

// anonymous namepsace is like static but can hold types
namespace {
	// Buffers will be pushed in pc block with deviceAddress
	// The rest will be bindless with fixed bindign#
	// This bindless model is lifted from lightweightVK aproach
	enum Bindings {
		kBinding_Textures = 0,
		kBinding_Samplers = 1,
	};
	struct FormatInfo {
		uint32_t blockWidth;
		uint32_t blockHeight;
		uint32_t bytesPerBlock;
	};
	static FormatInfo getFormatInfo(VkFormat format) {
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
			assert(false);
		}
	}
};

static VkInstance instance_ = VK_NULL_HANDLE;
static VkDevice device_ = VK_NULL_HANDLE;
static VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
static VkSurfaceKHR surface_ = VK_NULL_HANDLE;
static VkQueue graphicsQueue_ = VK_NULL_HANDLE;
static VkQueue presentQueue_ = VK_NULL_HANDLE;
static VkQueue transferQueue_ = VK_NULL_HANDLE;
static VkCommandPool defaultCommandPool_ = VK_NULL_HANDLE;
static VkCommandPool transferCommandPool_ = VK_NULL_HANDLE;



void DestroyDebugUtilsMessengerEXT(
	VkInstance instance,
	VkDebugUtilsMessengerEXT debugMessenger,
	const VkAllocationCallbacks* pAllocator
) {
	auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
		vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")
		);

	if (func != nullptr) {
		func(instance, debugMessenger, pAllocator);
	}
}

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

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData) {
	std::cerr << "---------------- Vulkan Validation ----------------\n";
	std::cerr << "Message: " << pCallbackData->pMessage << "\n";
	std::cerr << "\n";  // <--- Blank line for separation
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

VkFormat findSupportedFormat(
	VkPhysicalDevice physicalDevice,
	const std::vector<VkFormat>& candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features
) {
	for (VkFormat vk_format : candidates) {
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(physicalDevice, vk_format, &props);

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


// I should implement promotion other ways. Shadows may run other depthfmt also.
// Maybe should be const. just checked for availability at device, error out if not found.
VkFormat findDepthFormat(
) {
	return findSupportedFormat(
		physicalDevice_,
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

QueueFamilyIndices findQueueFamilies(
	VkPhysicalDevice device,
	VkSurfaceKHR surface
) {
	QueueFamilyIndices indices;

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(
		device,
		&queueFamilyCount,
		nullptr
	);

	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(
		device,
		&queueFamilyCount,
		queueFamilies.data()
	);

	int i = 0;

	for (const auto& queueFamily : queueFamilies) {
		if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			indices.graphicsFamily = i;
		}

		VkBool32 presentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(
			device,
			i,
			surface,
			&presentSupport
		);

		if (presentSupport) {
			indices.presentFamily = i;
		}

		i++;
	}

	// Look for a dedicated transfer queue.
	i = 0;
	for (const auto& queueFamily : queueFamilies) {
		bool supportsTransfer =
			queueFamily.queueFlags & VK_QUEUE_TRANSFER_BIT;

		bool supportsGraphics =
			queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT;

		bool supportsCompute =
			queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT;

		if (supportsTransfer && !supportsGraphics && !supportsCompute) {
			indices.transferFamily = i;
			break;
		}

		i++;
	}

	// If no fully dedicated transfer queue exists,
	// look for one that at least is not a graphics queue.
	if (!indices.transferFamily.has_value()) {
		i = 0;
		for (const auto& queueFamily : queueFamilies) {
			bool supportsTransfer =
				queueFamily.queueFlags & VK_QUEUE_TRANSFER_BIT;

			bool supportsGraphics =
				queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT;

			if (supportsTransfer && !supportsGraphics) {
				indices.transferFamily = i;
				break;
			}

			i++;
		}
	}

	// Final fallback: use graphics queue for transfers.
	if (!indices.transferFamily.has_value()) {
		indices.transferFamily = indices.graphicsFamily;
	}

	return indices;
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
	QueueFamilyIndices indices = findQueueFamilies(device, surface);

	bool extensionsSupported = checkDeviceExtensionSupport(device, deviceExtensions);

	bool swapChainAdequate = false;
	if (extensionsSupported) {
		SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device, surface);
		swapChainAdequate = !swapChainSupport.formats.empty() &&
			!swapChainSupport.presentModes.empty();
	}

	return indices.isComplete() && extensionsSupported && swapChainAdequate;
}

VkSurfaceFormatKHR chooseSwapSurfaceFormat(
	const std::vector<VkSurfaceFormatKHR>& availableFormats) {
	for (const auto& availableFormat : availableFormats) {
		if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
			availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return availableFormat;
		}
	}

	return availableFormats[0];
}

VkPresentModeKHR chooseSwapPresentMode(
	const std::vector<VkPresentModeKHR>& availablePresentModes) {
	for (const auto& availablePresentMode : availablePresentModes) {
		if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
			return availablePresentMode;
		}
	}

	return VK_PRESENT_MODE_FIFO_KHR;
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

static inline bool is_aligned4(uint32_t v) { return (v & 3u) == 0u; }
static uint64_t getImageSizeBytes(
	VkFormat vk_format,
	VkExtent3D vk_extent,
	uint32_t levels,
	uint32_t arrayLayers
) {
	const FormatInfo fmt = getFormatInfo(vk_format);

	uint64_t totalSize = 0;

	uint32_t width = static_cast<uint32_t>(vk_extent.width);
	uint32_t height = static_cast<uint32_t>(vk_extent.height);

	for (uint32_t level = 0; level < levels; ++level) {
		uint32_t mipWidth = std::max(1u, width >> level);
		uint32_t mipHeight = std::max(1u, height >> level);

		uint32_t blocksX = (mipWidth + fmt.blockWidth - 1) / fmt.blockWidth;
		uint32_t blocksY = (mipHeight + fmt.blockHeight - 1) / fmt.blockHeight;

		uint64_t mipSize =
			static_cast<uint64_t>(blocksX) *
			static_cast<uint64_t>(blocksY) *
			static_cast<uint64_t>(fmt.bytesPerBlock);

		totalSize += mipSize;
	}

	totalSize *= arrayLayers;

	return totalSize;
}
static uint64_t getImageSizeBytes(const smvk::ImageCreateInfo& info) {
	return getImageSizeBytes(
		info.vk_format,
		info.vk_extent,
		info.levels,
		info.arrayLayers
	);
}

VkCommandPool GetCommandPool(smvk::QueuePreference preferredQueue)
{
	switch (preferredQueue) {
	case smvk::QueuePreference::GRAPHICS:
		return defaultCommandPool_;
	case smvk::QueuePreference::TRANSFER:
		return transferCommandPool_;
	case smvk::QueuePreference::COMPUTE:
		assert(false && "Compute queue command pool not implemented");
		return VK_NULL_HANDLE;
	}
	assert(false && "Unknown QueuePreference");

	return VK_NULL_HANDLE;
};
void CreateDescriptorSetLayout(const smvk::PipelineCreateInfo::DescriptorSetDescription& descIn, VkDescriptorSetLayout& layout)
{
	smvk::PipelineCreateInfo::DescriptorSetDescription desc = descIn;
	std::vector<VkDescriptorSetLayoutBinding> vkBindings;
	vkBindings.reserve(desc.bindings.size());

	// IMPORTANT: pImmutableSamplers must point to memory that lives until vkCreateDescriptorSetLayout returns.
	// We can keep a local vector-of-vectors of VkSampler pointers to stable storage.
	std::vector<std::vector<VkSampler>> immutableSamplersStorage;
	immutableSamplersStorage.reserve(desc.bindings.size());

	std::vector<VkDescriptorBindingFlags> bindingFlags;
	bindingFlags.reserve(desc.bindings.size());

	for (const smvk::PipelineCreateInfo::BindingDescription& b : desc.bindings)
	{
		VkDescriptorSetLayoutBinding vkB{};
		vkB.binding = b.binding;
		vkB.descriptorType = b.type;
		vkB.descriptorCount = b.count;
		vkB.stageFlags = b.stages;
		if (!b.immutableSamplers.empty())
		{
			assert(b.immutableSamplers.size() == b.count);
			immutableSamplersStorage.emplace_back();
			auto& store = immutableSamplersStorage.back();
			store.reserve(b.immutableSamplers.size());
			store = b.immutableSamplers;
			vkB.pImmutableSamplers = store.data();
		}
		else
		{
			vkB.pImmutableSamplers = nullptr;
		}
		vkBindings.push_back(vkB);
		bindingFlags.push_back(b.flags);
	}

	// 4) Optional binding flags chain
	VkDescriptorSetLayoutBindingFlagsCreateInfo flagsCI{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
	};
	flagsCI.bindingCount = (uint32_t)bindingFlags.size();
	flagsCI.pBindingFlags = bindingFlags.data();

	VkDescriptorSetLayoutCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
	};
	ci.flags = desc.createFlags;
	ci.bindingCount = (uint32_t)vkBindings.size();
	ci.pBindings = vkBindings.data();

	// Only chain if you actually have non-zero flags; otherwise keep pNext null
	bool anyFlags = false;
	for (auto f : bindingFlags) if (f != 0) { anyFlags = true; break; }
	ci.pNext = anyFlags ? &flagsCI : nullptr;

	VK_CHECK(vkCreateDescriptorSetLayout(device_, &ci, nullptr, &layout));
}
//
int smvk::VulkanCore::CreateInstance() {

	if (enableValidationLayers && !checkValidationLayerSupport(config_.validationLayers)) {
		throw std::runtime_error(
			"validation layers requested, but not available!");
	}

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	//appInfo.pApplicationName = APPLICATION_NAME;
	appInfo.pApplicationName = "default_smvk_name1";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = ENGINE_NAME;
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

	if (enableValidationLayers) {
		createInfo.enabledLayerCount = static_cast<uint32_t>(config_.validationLayers.size());
		createInfo.ppEnabledLayerNames = config_.validationLayers.data();
		createInfo.pNext = (void*)&debugCreateInfo;
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
		if (vkCreateDebugUtilsMessengerEXT(instance_, &debugCreateInfo, nullptr,
			&debugMessenger_) != VK_SUCCESS) {
			throw std::runtime_error("failed to set up debug messenger!");
		}
	};

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

		vkGetPhysicalDeviceFeatures2(physicalDevice_, &vkFeatures10_);
		vkGetPhysicalDeviceProperties2(physicalDevice_, &vkPhysicalDeviceProperties2_);

		// Create Logical Device
		{
			QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
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
				//.storagePushConstant16 = VK_TRUE,
				//.storageInputOutput16 = VK_TRUE
			};
			VkPhysicalDeviceVulkan12Features deviceFeatures12
			{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
				.pNext = &deviceFeatures11,
				.shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
				.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
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
					.textureCompressionBC = VK_TRUE, // REQUIRED for BC1..BC7
				},
			};

			//s
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

			if (enableValidationLayers) {
				createInfo.enabledLayerCount = static_cast<uint32_t>(config_.validationLayers.size());
				createInfo.ppEnabledLayerNames = config_.validationLayers.data();
			}
			else {
				createInfo.enabledLayerCount = 0;
			}

			if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
				throw std::runtime_error("failed to create logical device!");
			}

			volkLoadDevice(device_);

			// this is really hacky 
			// Assert for BC7
			VkFormatProperties p{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice_, VK_FORMAT_BC7_SRGB_BLOCK, &p);
			bool sampleOK = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
			assert(sampleOK);

		}

		QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
		vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
		vkGetDeviceQueue(device_, indices.transferFamily.value(), 0, &transferQueue_);

		VkCommandPoolCreateInfo poolInfo
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, // this is needed to reset individual cmdbuf in the pool
			.queueFamilyIndex = indices.graphicsFamily.value()
		};
		VkCommandPoolCreateInfo transferPoolInfo
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, // this is needed to reset individual cmdbuf in the pool
			.queueFamilyIndex = indices.transferFamily.value()
		};
		if (vkCreateCommandPool(device_, &poolInfo, nullptr, &defaultCommandPool_) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create graphics command pool!");
		}
		if (vkCreateCommandPool(device_, &poolInfo, nullptr, &transferCommandPool_) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create transfer command pool!");
		}

	};

	return true;

};
//void smvk::VulkanCore::CreateFence(const VkFenceCreateInfo createInfo, VkFence& out) {
//	VK_CHECK(vkCreateFence(device_, &createInfo, nullptr, &out));
//};
void smvk::VulkanCore::CreateTimelineSemaphore(uint64_t initialValue, VkSemaphore& out) {
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

void smvk::VulkanCore::CreateShaderModule(const std::vector<char>& code, VkShaderModule& out) {
	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = code.size();
	createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

	if (vkCreateShaderModule(device_, &createInfo, nullptr, &out) !=
		VK_SUCCESS) {
		throw std::runtime_error("failed to create shader module!");
	}
}
inline smvk::VulkanCore::~VulkanCore() {
	vkDeviceWaitIdle(device_);
	vkDestroyCommandPool(device_, defaultCommandPool_, nullptr);
	vkDestroyCommandPool(device_, transferCommandPool_, nullptr);
	if (surface_ != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(instance_, surface_, nullptr);
	}
	DestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
	vkDestroyInstance(instance_, nullptr);
}

VkResult createStagingPool(
	VmaAllocator allocator,
	VmaPool& attachmentPool)
{
	// Representative attachment image.
	// Use a format supported by every image intended for this pool.
	VkImageCreateInfo imageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {
			.width = 1920,
			.height = 1080,
			.depth = 1,
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage =
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VmaAllocationCreateInfo allocationInfo{
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	};

	uint32_t memoryTypeIndex = 0;

	VkResult result = vmaFindMemoryTypeIndexForImageInfo(
		allocator,
		&imageInfo,
		&allocationInfo,
		&memoryTypeIndex);

	if (result != VK_SUCCESS)
		return result;

	VmaPoolCreateInfo poolInfo{
		.memoryTypeIndex = memoryTypeIndex,

		// Safe because this pool contains only optimal-tiled images.
		.flags = VMA_POOL_CREATE_IGNORE_BUFFER_IMAGE_GRANULARITY_BIT,

		// Tune these values based on attachment sizes and frames in flight.
		.blockSize = 128ull * 1024ull * 1024ull,
		.minBlockCount = 1,
		.maxBlockCount = 8,

		.priority = 1.0f,
		.minAllocationAlignment = 0,
		.pMemoryAllocateNext = nullptr,
	};

	return vmaCreatePool(
		allocator,
		&poolInfo,
		&attachmentPool);
}

VkResult createAttachmentPool(
	VmaAllocator allocator,
	VmaPool& attachmentPool)
{
	// Representative attachment image.
	// Use a format supported by every image intended for this pool.
	VkImageCreateInfo imageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {
			.width = 1920,
			.height = 1080,
			.depth = 1,
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage =
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VmaAllocationCreateInfo allocationInfo{
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	};

	uint32_t memoryTypeIndex = 0;

	VkResult result = vmaFindMemoryTypeIndexForImageInfo(
		allocator,
		&imageInfo,
		&allocationInfo,
		&memoryTypeIndex);

	if (result != VK_SUCCESS)
		return result;

	VmaPoolCreateInfo poolInfo{
		.memoryTypeIndex = memoryTypeIndex,

		// Safe because this pool contains only optimal-tiled images.
		.flags = VMA_POOL_CREATE_IGNORE_BUFFER_IMAGE_GRANULARITY_BIT,

		// Tune these values based on attachment sizes and frames in flight.
		.blockSize = 128ull * 1024ull * 1024ull,
		.minBlockCount = 1,
		.maxBlockCount = 8,

		.priority = 1.0f,
		.minAllocationAlignment = 0,
		.pMemoryAllocateNext = nullptr,
	};

	return vmaCreatePool(
		allocator,
		&poolInfo,
		&attachmentPool);
}
int smvk::Context::Initialize() {
	VmaAllocatorCreateInfo allocatorCreateInfo = {};
	allocatorCreateInfo.physicalDevice = physicalDevice_;
	allocatorCreateInfo.device = device_;
	allocatorCreateInfo.instance = instance_;
	allocatorCreateInfo.vulkanApiVersion = core_.config_.version_ == VK_1_3 ? VK_API_VERSION_1_3 : UINT32_MAX;
	allocatorCreateInfo.flags =
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT |
		VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	// VMA_ALLOCATOR_CREATE_KHR_EXTERNAL_MEMORY_WIN32_BIT // We wont use this. Just keeeping this here to remember it exists.
	if (allocatorCreateInfo.vulkanApiVersion == UINT32_MAX) { assert(false); };

	VmaVulkanFunctions vulkanFunctions;
	VK_CHECK(vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vulkanFunctions));
	allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;
	VK_CHECK(vmaCreateAllocator(&allocatorCreateInfo, &m_allocator));
	VK_CHECK(createAttachmentPool(m_allocator, m_imagesPool));
	VK_CHECK(createStagingPool(m_allocator, m_stagingPool));

	core_.CreateSampler(SamplerCreateInfo{}, m_defaultSampler);
	const uint32_t pixel = 0xFF000000;
	//m_nilTexture = CreateTexture(
	//	ImageCreateInfo{
	//	.vk_extent = {1,1,1},
	//	.vk_format = VK_FORMAT_R8G8B8A8_SRGB,
	//	},
	//	&pixel
	//	);

	// TODO: rework this
	m_submitter = new CommandBufferManager(core_, QueuePreference::GRAPHICS);
	return 0; // no error
}
smvk::Context::Context(VulkanCore& core) :core_(core) { Initialize(); }
smvk::Context::~Context() {
	// TODO:: implement this propertul
	delete(&m_submitter);
	m_submitter = nullptr;
	CleanUp();
};
void smvk::Context::UploadDataToBuffer(
	BufferHandle buffer,
	const void* data,
	size_t dataSize,
	VkPipelineStageFlags2 dstStages,
	VkAccessFlags2 dstAccess,
	size_t src_offset,
	size_t dest_offset
) {
	auto stageHan = CreateStagingBuffer(dataSize);

	auto buf = buffers_.get(buffer);
	assert(buffer.valid());
	assert(buf->vk_buffer != VK_NULL_HANDLE);

	auto stagingBuf = buffers_.get(stageHan);
	assert(stagingBuf->vk_buffer != VK_NULL_HANDLE);
	assert(stagingBuf->vk_buffer != VK_NULL_HANDLE);



	auto han = m_submitter->acquire();
	auto cmd = m_submitter->GetCommandBuffer(han).cmd;

	if (buf->is_mappable()) {
		VK_CHECK(vmaCopyMemoryToAllocation(m_allocator, data, buf->vma_alloc, dest_offset, dataSize));
		BufferBarrier(
			cmd,
			buf->vk_buffer,
			dest_offset,
			dataSize,
			VK_PIPELINE_STAGE_2_HOST_BIT,
			VK_ACCESS_2_HOST_WRITE_BIT,
			dstStages,
			dstAccess
		);
		return;
	}


	//VmaAllocationInfo info{};
	//vmaGetAllocationInfo(m_allocator, stagingBuf->vma_alloc, &info);
	//assert(dest_offset <= info.size);
	//assert(dataSize <= info.size - dest_offset);
	VK_CHECK(vmaCopyMemoryToAllocation(m_allocator, data, stagingBuf->vma_alloc, 0, dataSize));

	// Copy staging -> dst
	VkBufferCopy copyRegion{
		.srcOffset = 0,	// we have the staging buf created for this so just from 0.
		.dstOffset = dest_offset,
		.size = dataSize
	};
	vkCmdCopyBuffer(cmd, stagingBuf->vk_buffer, buf->vk_buffer, 1, &copyRegion);

	BufferBarrier(
		cmd,
		buf->vk_buffer,
		dest_offset,
		dataSize,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		VK_ACCESS_2_TRANSFER_WRITE_BIT,
		dstStages,
		dstAccess
	);

	m_submitter->submit(han);
	AddDeferredTask(
		std::packaged_task<void()>([this, stagingBuf] {vmaDestroyBuffer(m_allocator, stagingBuf->vk_buffer, stagingBuf->vma_alloc); })
		, han
	);
}

void smvk::Context::UploadDataToImage2D(Image image, const void* data, size_t dataSize, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset, size_t dest_offset)
{
	const VkDeviceSize stagingBufferSize = static_cast<VkDeviceSize>(getImageSizeBytes(
		{
			.vk_extent = image.vk_extent
		}
	));
	auto stagingBufferHandle = CreateStagingBuffer(stagingBufferSize);
	auto staginfBuffer = buffers_.get(stagingBufferHandle);
	void* mapped = MapBuffer(stagingBufferHandle);
	std::memcpy(mapped, data, static_cast<size_t>(stagingBufferSize));
	UnmapBuffer(stagingBufferHandle);
	//VK_CHECK(vmaMapMemory(m_allocator, stagingBuf->vma_alloc, &mapped));
	//// Not required for HOST_COHERENT memory, but harmless if you want to support
	//// non-coherent staging buffers later.
	//// vmaFlushAllocation(m_allocator, staging->vma_alloc, 0, stagingBufferSize);
	//vmaUnmapMemory(m_allocator, stagingBuf->vma_alloc);

	auto han = m_submitter->acquire();
	VkCommandBuffer cmd = m_submitter->GetCommandBuffer(han).cmd;
	{
		VkImageSubresourceRange range{};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.baseMipLevel = 0;
		range.levelCount = image.levels;
		range.baseArrayLayer = 0;
		range.layerCount = image.arrayLayers;

		ImageBarrier(
			cmd,
			image.vk_image,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			range,
			VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
			VK_ACCESS_2_NONE,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT
		);
	}

	// ---------------------------------------------------------------------
	// Copy staging buffer into image
	// Assumes `data` is packed as:
	// mip 0 layer 0
	// mip 0 layer 1
	// ...
	// mip 1 layer 0
	// mip 1 layer 1
	// ...
	// ---------------------------------------------------------------------
	std::vector<VkBufferImageCopy2> regions;
	regions.reserve(
		static_cast<size_t>(image.levels) *
		static_cast<size_t>(image.arrayLayers)
	);

	VkDeviceSize bufferOffset = 0;

	const FormatInfo fmt = getFormatInfo(image.vk_format);

	for (uint32_t level = 0; level < image.levels; ++level)
	{
		const uint32_t mipWidth =
			std::max(1u, static_cast<uint32_t>(image.vk_extent.width) >> level);

		const uint32_t mipHeight =
			std::max(1u, static_cast<uint32_t>(image.vk_extent.height) >> level);

		const uint32_t blocksX =
			(mipWidth + fmt.blockWidth - 1) / fmt.blockWidth;

		const uint32_t blocksY =
			(mipHeight + fmt.blockHeight - 1) / fmt.blockHeight;

		const VkDeviceSize singleLayerMipSize =
			static_cast<VkDeviceSize>(blocksX) *
			static_cast<VkDeviceSize>(blocksY) *
			static_cast<VkDeviceSize>(fmt.bytesPerBlock);

		for (uint32_t layer = 0; layer < image.arrayLayers; ++layer)
		{
			VkBufferImageCopy2 region{};
			region.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;

			region.bufferOffset = bufferOffset;

			// 0 means tightly packed.
			region.bufferRowLength = 0;
			region.bufferImageHeight = 0;

			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = level;
			region.imageSubresource.baseArrayLayer = layer;
			region.imageSubresource.layerCount = 1;

			region.imageOffset = { 0, 0, 0 };
			region.imageExtent = {
				mipWidth,
				mipHeight,
				1
			};

			regions.push_back(region);

			bufferOffset += singleLayerMipSize;
		}
	}

	VkCopyBufferToImageInfo2 copyInfo{};
	copyInfo.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
	copyInfo.srcBuffer = staginfBuffer->vk_buffer; // <-- replace with your staging buffer member
	copyInfo.dstImage = image.vk_image;
	copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	copyInfo.regionCount = static_cast<uint32_t>(regions.size());
	copyInfo.pRegions = regions.data();
	vkCmdCopyBufferToImage2(cmd, &copyInfo);

	// ---------------------------------------------------------------------
	// Transition whole image:
	// TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
	// ---------------------------------------------------------------------
	{
		VkImageSubresourceRange range{};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.baseMipLevel = 0;
		range.levelCount = image.levels;
		range.baseArrayLayer = 0;
		range.layerCount = image.arrayLayers;

		ImageBarrier(
			cmd,
			image.vk_image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			range,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT,
			// framgent is presummed to be first access!
			// this could lead to errors
			//VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
		);
	}

	m_submitter->submit(han);
	AddDeferredTask(
		std::packaged_task<void()>([this, staging = stagingBufferHandle] {destroy(this, staging); }),
		han // Delete stagingBuffer when copy ends.
	);

}
void CreatePipelineLayout(const smvk::PipelineCreateInfo::PipelineLayoutDesc& descIn, VkPipelineLayout& out)
{
	auto sets = descIn.setDescriptions;
	assert(sets.size() <= 8 && "Too many descriptor sets 8+");
	const uint32_t setLayoutCount = (sets.empty()) ? 0u : (sets.size());
	std::array<VkDescriptorSetLayout, 8> vkSetLayouts{};
	vkSetLayouts.fill(VK_NULL_HANDLE);

	int i = 0;
	for (const auto& s : sets)
	{
		VkDescriptorSetLayout vk_lyt = VK_NULL_HANDLE;
		CreateDescriptorSetLayout(s, vk_lyt);
		vkSetLayouts[i] = vk_lyt;
		i++;
	}

	VkPushConstantRange ranges[8]{};
	const uint32_t rangeCount = static_cast<uint32_t>(descIn.pushConstants.size());
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
	auto min_align = properties.limits.minUniformBufferOffsetAlignment;

	assert(rangeCount <= 8 && "Max 8 push constant ranges");

	// Decide explicit offsets vs packed offsets
	bool anyExplicitOffset = false;
	for (const auto& pc : descIn.pushConstants)
		if (pc.offset != 0) { anyExplicitOffset = true; break; }

	uint32_t curr_off = 0;

	for (uint32_t i = 0; i < rangeCount; ++i)
	{
		const auto& r = descIn.pushConstants[i];

		const uint32_t off = anyExplicitOffset ? r.offset : curr_off;

		assert(is_aligned4(off));
		assert(is_aligned4(r.size));
		assert(r.size > 0);

		ranges[i] = VkPushConstantRange{
			.stageFlags = r.stages,
			.offset = off,
			.size = r.size
		};

		if (!anyExplicitOffset)
			curr_off += r.size;
	}
	VkPipelineLayoutCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = setLayoutCount,
		.pSetLayouts = setLayoutCount ? vkSetLayouts.data() : nullptr,
		.pushConstantRangeCount = rangeCount,
		.pPushConstantRanges = rangeCount ? ranges : nullptr,
	};
	VK_CHECK(vkCreatePipelineLayout(device_, &ci, nullptr, &out));
}

[[nodiscard]] smvk::Holder<smvk::ImageHandle> smvk::Context::CreateImage(ImageCreateInfo imageCreateInfo, const void* data)
{
	Image img{};

	VkImageCreateInfo imgCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgCI.imageType = VK_IMAGE_TYPE_2D; // TODO, hardoded to 2D but could be 3D really
	imgCI.format = imageCreateInfo.vk_format;               // pick your format
	imgCI.extent = imageCreateInfo.vk_extent;// width, height, depth
	imgCI.mipLevels = imageCreateInfo.levels;                                      // >1 if you want mipmaps
	imgCI.usage = imageCreateInfo.vk_usage;
	imgCI.arrayLayers = imageCreateInfo.arrayLayers;                                      // >1 for arrays/cubemaps
	imgCI.samples = VK_SAMPLE_COUNT_1_BIT;                  // MSAA? set >1
	imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;                // GPU-friendly
	imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE; // Queue exclusivity on by default.
	imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;              // we’ll transition later. Well jus use transition General always

	VmaAllocationCreateInfo allocation_create_info{};
	allocation_create_info.usage = VMA_MEMORY_USAGE_GPU_ONLY; // should be compatible with the pool’s memoryType
	allocation_create_info.pool = VK_NULL_HANDLE; // Null handle draws from VmaDefaultPool based on usage
	img.vk_format = imageCreateInfo.vk_format;
	img.levels = imageCreateInfo.levels;
	img.vk_extent = imageCreateInfo.vk_extent;
	//img.vk_usageFlags = imageCreateInfo.usage;
	img.arrayLayers = imageCreateInfo.arrayLayers;
	VK_CHECK(vmaCreateImage(m_allocator, &imgCI, &allocation_create_info, &img.vk_image, &img.vma_alloc, nullptr));

	// TODO:: rework this
	img.vk_sampler = m_defaultSampler;
	//img.owns_sampler = false;

	if (data != nullptr) {}
	return smvk::Holder<ImageHandle>(this, images_.create(std::move(img)));

}

//smvk::Holder<smvk::TextureHandle> CreateTexture(smvk::ImageCreateInfo imageCreateInfo, const void* data = nullptr)
//{
//	smvk::Texture t;
//	return { this, textures_.create(std::move(t)) };
//
//}
smvk::Holder<smvk::TextureHandle> smvk::Context::CreateTexture(ktxTexture2& texture) {

	if (ktxTexture2_NeedsTranscoding(&texture)) {
		// Hardcoded for now, as in your original code.
		KTX_error_code tr = ktxTexture2_TranscodeBasis(&texture, KTX_TTF_BC7_RGBA, 0);
		if (tr != KTX_SUCCESS) {
			throw std::runtime_error(ktxErrorString(tr));
		}
	}

	// Force offline mip usage.
	texture.generateMipmaps = KTX_FALSE;

	const VkFormat format = ktxTexture2_GetVkFormat(&texture);
	if (format == VK_FORMAT_UNDEFINED) {
		throw std::runtime_error("KTX texture has undefined VkFormat after transcoding.");
	}

	const uint32_t width = texture.baseWidth;
	const uint32_t height = texture.baseHeight;
	const uint32_t depth = std::max(1u, texture.baseDepth);
	const uint32_t mipLevels = texture.numLevels;
	const uint32_t arrayLayers =
		std::max(1u, texture.numLayers) * std::max(1u, texture.numFaces);

	const bool isCubemap = texture.isCubemap == KTX_TRUE;

	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		(texture.generateMipmaps ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);

	const bool is3D = depth > 1;

	// -------------------------------------------------------------------------
	// Create VkImage with VMA
	// -------------------------------------------------------------------------
	VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.flags = isCubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;

	imageInfo.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	imageInfo.extent = { width, height, depth };
	imageInfo.arrayLayers = is3D ? 1 : arrayLayers;

	imageInfo.format = format;
	imageInfo.extent = { width, height, depth };
	imageInfo.mipLevels = mipLevels;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = usage;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	// Textures go on device local.
	VmaAllocationCreateInfo imageAllocInfo{};
	imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	imageAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	VkImage image = VK_NULL_HANDLE;
	VmaAllocation imageAllocation = VK_NULL_HANDLE;
	VmaAllocationInfo imageAllocationInfo{};

	VkResult vr = vmaCreateImage(
		m_allocator,
		&imageInfo,
		&imageAllocInfo,
		&image,
		&imageAllocation,
		&imageAllocationInfo
	);

	if (vr != VK_SUCCESS) {
		throw std::runtime_error("vmaCreateImage failed for KTX texture.");
	}

	auto* base = reinterpret_cast<ktxTexture*>(&texture);
	ktx_size_t ktxDataSize = ktxTexture_GetDataSizeUncompressed(base);
	ktx_uint8_t* ktxData = ktxTexture_GetData(base);

	if (!ktxData || ktxDataSize == 0) {
		vmaDestroyImage(m_allocator, image, imageAllocation);
		assert(false && "no image data");
		throw std::runtime_error("KTX texture has no image data.");
	}

	VkBufferCreateInfo stagingBufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	stagingBufferInfo.size = static_cast<VkDeviceSize>(ktxDataSize);
	stagingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	stagingBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo stagingAllocInfo{};
	stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	stagingAllocInfo.flags =
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
		VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VmaAllocation stagingAllocation = VK_NULL_HANDLE;
	VmaAllocationInfo stagingAllocationInfo{};

	vr = vmaCreateBuffer(
		m_allocator,
		&stagingBufferInfo,
		&stagingAllocInfo,
		&stagingBuffer,
		&stagingAllocation,
		&stagingAllocationInfo
	);

	if (vr != VK_SUCCESS) {
		vmaDestroyImage(m_allocator, image, imageAllocation);
		throw std::runtime_error("vmaCreateBuffer failed for KTX staging buffer.");
	}

	// Guaranteed mapped due to chosen flags. Ignore warning
#pragma warning(suppress : 6387)
	std::memcpy(stagingAllocationInfo.pMappedData, ktxData, static_cast<size_t>(ktxDataSize));
	vmaFlushAllocation(m_allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

	// -------------------------------------------------------------------------
	// Build VkBufferImageCopy regions from KTX offsets
	// -------------------------------------------------------------------------
	std::vector<VkBufferImageCopy> regions;
	regions.reserve(mipLevels * arrayLayers);

	for (uint32_t level = 0; level < mipLevels; ++level) {
		const uint32_t mipWidth = std::max(1u, width >> level);
		const uint32_t mipHeight = std::max(1u, height >> level);
		const uint32_t mipDepth = std::max(1u, depth >> level);

		if (texture.numLayers > 0) {
			// Array texture / cubemap array / cubemap
			for (uint32_t layer = 0; layer < std::max(1u, texture.numLayers); ++layer) {
				const uint32_t faces = std::max(1u, texture.numFaces);

				for (uint32_t face = 0; face < faces; ++face) {
					ktx_size_t offset = 0;
					KTX_error_code offRes = ktxTexture2_GetImageOffset(
						&texture,
						level,
						layer,
						face,
						&offset
					);
					if (offRes != KTX_SUCCESS) {
						vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
						vmaDestroyImage(m_allocator, image, imageAllocation);
						throw std::runtime_error("ktxTexture_GetImageOffset failed.");
					}

					VkBufferImageCopy region{};
					region.bufferOffset = static_cast<VkDeviceSize>(offset);
					region.bufferRowLength = 0;   // tightly packed
					region.bufferImageHeight = 0; // tightly packed

					region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					region.imageSubresource.mipLevel = level;
					region.imageSubresource.baseArrayLayer = layer * faces + face;
					region.imageSubresource.layerCount = 1;

					region.imageOffset = { 0, 0, 0 };
					region.imageExtent = { mipWidth, mipHeight, mipDepth };

					regions.push_back(region);
				}
			}
		}
		else {
			// Non-array texture
			ktx_size_t offset = 0;

			KTX_error_code offRes = ktxTexture_GetImageOffset(
				base,
				//ktxTexture(texture),
				level,
				0,
				0,
				&offset
			);
			if (offRes != KTX_SUCCESS) {
				vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
				vmaDestroyImage(m_allocator, image, imageAllocation);
				throw std::runtime_error("ktxTexture_GetImageOffset failed.");
			}

			VkBufferImageCopy region{};
			region.bufferOffset = static_cast<VkDeviceSize>(offset);
			region.bufferRowLength = 0;
			region.bufferImageHeight = 0;

			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = level;
			region.imageSubresource.baseArrayLayer = 0;
			region.imageSubresource.layerCount = 1;

			region.imageOffset = { 0, 0, 0 };
			region.imageExtent = { mipWidth, mipHeight, mipDepth };

			regions.push_back(region);
		}
	}

	// Instead of this upload should be scheduled to do in abtch and recorded to the transferQueue! 
	// -------------------------------------------------------------------------
	// Record upload
	// -------------------------------------------------------------------------
	//VkCommandBuffer cmd = beginSingleTimeCommands(core.device_, core.defaultCommandPool_); // your helper
	auto han = m_submitter->acquire();
	auto cmd = m_submitter->GetCommandBuffer(han).cmd;


	//ImageBarrier(
	//	cmd, 
	//	
	//)
	{
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = mipLevels;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = arrayLayers;

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

	vkCmdCopyBufferToImage(
		cmd,
		stagingBuffer,
		image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		static_cast<uint32_t>(regions.size()),
		regions.data()
	);

	{
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = mipLevels;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = arrayLayers;

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

	m_submitter->submit(han);
	AddDeferredTask(
		std::packaged_task<void()>([this, buf = stagingBuffer, alloc = stagingAllocation] {vmaDestroyBuffer(this->m_allocator, buf, alloc); }),
		han
	);

	// -------------------------------------------------------------------------
	// View
	// -------------------------------------------------------------------------
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

	VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
	if (isCubemap) {
		viewType = (arrayLayers > 6) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
	}
	else if (arrayLayers > 1) {
		viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	}
	else if (depth > 1) {
		viewType = VK_IMAGE_VIEW_TYPE_3D;
	}

	Image image_obj{};
	image_obj.vk_image = image;
	image_obj.vma_alloc = imageAllocation;
	image_obj.vk_format = format;
	image_obj.vk_extent = { width, height, depth };
	image_obj.arrayLayers = imageInfo.arrayLayers;
	image_obj.levels = mipLevels;
	image_obj.sampleView = VK_NULL_HANDLE;
	image_obj.renderView = VK_NULL_HANDLE;

	// TODO: Rework this.
	//CreateSampler(0, image_obj.sampler);
	//CreateSampler(SamplerCreateInfo(), image_obj.sampler);

	image_obj.vk_sampler = m_defaultSampler;
	//image_obj.owns_sampler = true;


	ImageViewCreateInfo createViewInfo{};
	createViewInfo.flags = {};
	createViewInfo.vk_image = image;
	createViewInfo.vk_format = format;
	createViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	createViewInfo.subResourceRange = {
		.aspectMask = aspect,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};

	core_.CreateImageView(
		createViewInfo,
		image_obj.renderView
	);

	image_obj.sampleView = image_obj.renderView;
	return { this, 	textures_.create(std::move(image_obj)) };

}

[[nodiscard]] smvk::BufferHandle smvk::Context::CreateBufferNoHolder(BufferCreateInfo bufferCreateInfo) {
	Buffer desc{};
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = bufferCreateInfo.size;
	ci.usage = bufferCreateInfo.usage_flags | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo aci{};
	aci.usage = bufferCreateInfo.vma_usage;

	// Optionally choose pools here based on usage_flags/usage
	VK_CHECK(vmaCreateBuffer(
		m_allocator,
		&ci,
		&aci,
		&desc.vk_buffer,
		&desc.vma_alloc,
		nullptr
		//&desc.allocInfo
	)
	);

	const VkBufferDeviceAddressInfo ai = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = desc.vk_buffer
	};
	desc.deviceAdress = vkGetBufferDeviceAddress(device_, &ai);
	vmaGetAllocationMemoryProperties(m_allocator, desc.vma_alloc, &desc.memory_flags);
	desc.usage = bufferCreateInfo.usage_flags;
	return buffers_.create(std::move(desc));
}
[[nodiscard]] smvk::Holder<smvk::BufferHandle> smvk::Context::CreateBuffer(BufferCreateInfo bufferCreateInfo) {
	Buffer desc{};
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = bufferCreateInfo.size;
	ci.usage = bufferCreateInfo.usage_flags | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo aci{};
	aci.usage = bufferCreateInfo.vma_usage;

	// Optionally choose pools here based on usage_flags/usage
	VK_CHECK(vmaCreateBuffer(
		m_allocator,
		&ci,
		&aci,
		&desc.vk_buffer,
		&desc.vma_alloc,
		nullptr
		//&desc.allocInfo
	)
	);

	const VkBufferDeviceAddressInfo ai = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = desc.vk_buffer
	};
	desc.deviceAdress = vkGetBufferDeviceAddress(device_, &ai);
	vmaGetAllocationMemoryProperties(m_allocator, desc.vma_alloc, &desc.memory_flags);
	desc.usage = bufferCreateInfo.usage_flags;
	return { this, buffers_.create(std::move(desc)) };
}
[[nodiscard]] smvk::BufferHandle smvk::Context::CreateStagingBuffer(VkDeviceSize size)
{
	smvk::Buffer desc{};
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size;
	ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO; // modern VMA: let it pick a good type
	aci.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	aci.requiredFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VK_CHECK(vmaCreateBuffer(m_allocator, &ci, &aci, &desc.vk_buffer, &desc.vma_alloc, nullptr));


	VkMemoryPropertyFlags props{}; //Cache memory properties for quick checks/debug
	vmaGetAllocationMemoryProperties(m_allocator, desc.vma_alloc, &props);
	desc.memory_flags = aci.requiredFlags;
	desc.usage = ci.usage;

	if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
		assert(false); // just hard crush for now
	}

	//return smvk::Holder<smvk::BufferHandle>(this, buffers_.create(std::move(desc)));
	return buffers_.create(std::move(desc));
}
//[[nodiscard]] smvk::Holder<smvk::SamplerHandle> smvk::Context::CreateSampler(SamplerCreateInfo samplerCreateInfo) {
//	VkSampler sampler;
//	core_.CreateSampler(samplerCreateInfo, sampler);
//	return smvk::Holder<SamplerHandle>(this, samplers_.create(std::move(sampler)));
//}
[[nodiscard]] smvk::Holder<smvk::ShaderModuleHandle> smvk::Context::CreateShaderModule(std::vector<char> code) {
	VkShaderModule shader;
	core_.CreateShaderModule(
		std::move(code),
		shader
	);
	return smvk::Holder(this, shaderModules_.create(std::move(shader)));
}
void* smvk::Context::MapBuffer(smvk::Handle<smvk::Buffer> handle) {
	void* mapped = nullptr;
	auto buf = this->buffers_.get(handle);
	VK_CHECK(vmaMapMemory(m_allocator, buf->vma_alloc, &mapped));
	return mapped;
}
void smvk::Context::UnmapBuffer(smvk::Handle<smvk::Buffer> handle) {
	void* mapped = nullptr;
	auto buf = this->buffers_.get(handle);
	// Not required for HOST_COHERENT memory, but harmless if you want to support
	// non-coherent staging buffers later.
	// vmaFlushAllocation(m_allocator, staging->vma_alloc, 0, stagingBufferSize);
	vmaUnmapMemory(m_allocator, buf->vma_alloc);
}
void smvk::Context::AddDeferredTask(std::packaged_task<void()>&& task, smvk::CommandSubmitHandle handle)
{
	//if (!m_submitter->validHandle(handle)) {
	if (handle.gen_ == 0) //not valid acquire 
	{
		handle = m_submitter->acquire();
	}
	m_deferredTasks.emplace_back(std::move(task), handle);
}
void smvk::Context::ProcessDeferrredTasks()
{
	std::vector<DeferredTask>::iterator it = m_deferredTasks.begin();
	while (it != m_deferredTasks.end() && m_submitter->isReady(it->handle_)) {
		(it++)->task_();
	}
	m_deferredTasks.erase(m_deferredTasks.begin(), it);
}
void smvk::Context::CleanUp() {
	VK_CHECK(vkDeviceWaitIdle(device_));
	vmaDestroyPool(m_allocator, m_imagesPool);
	vmaDestroyPool(m_allocator, m_stagingPool);
}

// 
void smvk::destroy(smvk::Context* ctx, smvk::BufferHandle handle) {
	if (handle.empty()) { return; };
	smvk::Buffer db = *ctx->buffers_.get(handle);
	vmaDestroyBuffer(ctx->m_allocator, db.vk_buffer, db.vma_alloc);
	ctx->buffers_.destroy(handle);
}
void smvk::destroy(smvk::Context* ctx, smvk::ShaderModuleHandle handle) {
	if (handle.empty()) { return; };
	auto sm = ctx->shaderModules_.get(handle);
	vkDestroyShaderModule(device_, *sm, NULL);
	ctx->shaderModules_.destroy(handle);
}
void smvk::destroy(smvk::Context* ctx, smvk::ImageHandle handle) {}
void smvk::destroy(smvk::Context* ctx, smvk::SamplerHandle handle) {}
void smvk::destroy(smvk::Context* ctx, smvk::DescriptorSetHandle handle) {}
void smvk::destroy(smvk::Context* ctx, smvk::DescriptorPoolHandle handle) {}
//

#ifdef USE_KTX
inline void smvk::Context::GetKtxDeviceInfo() {
	ktxInfo_ = *ktxVulkanDeviceInfo_Create(
		physicalDevice_,
		device_,
		/*core.GetTransferQueue()*/ graphicsQueue_,
		// this maybe should be transfer queue. we only use ktx to upload tex.
		// no blitting for mipmaps, which are generated offline
		// runtime genreation for mipmaps in ktx seems to be not up to par with SOA
		defaultCommandPool_,
		NULL
	);
}
#endif
//

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
	VkAccessFlags2 dstAccess
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

		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,

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
// 
inline smvk::CommandBufferManager::CommandBufferManager(VulkanCore& core, QueuePreference queue) : core_(core), queue_(queue)
{
	VkCommandPool pool = GetCommandPool(queue);
	if (pool == VK_NULL_HANDLE) { assert(false && "no commandPool"); };
	VkCommandBufferAllocateInfo info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = nullptr,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, //yes, just primary
		.commandBufferCount = kMax,
	};
	VkCommandBuffer m_buffers_[kMax];
	VK_CHECK(vkAllocateCommandBuffers(device_, &info, m_buffers_));

	for (int i = 0; i < kMax; i++) {
		commandBuffers_[i].allocatedCmd_ = m_buffers_[i];
		commandBuffers_[i].wrapper_.isEncoding_ = false;
		//core_.CreateFence(VkFenceCreateInfo{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = 0 }, commandBuffers_[i].wrapper_.fence_);
		core_.CreateTimelineSemaphore(0, commandBuffers_[i].wrapper_.finishedSignalSemaphore.semaphore_
		);
	}
}
inline smvk::CommandBufferManager::~CommandBufferManager() {
	if (device_ == VK_NULL_HANDLE) {
		return;
	}

	// Conservative. You can remove this if the owner guarantees GPU work is complete.
	vkDeviceWaitIdle(device_);

	VkCommandBuffer m_buffers_[kMax]{};

	for (uint32_t i = 0; i < kMax; ++i) {
		auto& wrapper = commandBuffers_[i].wrapper_;
		assert(wrapper.isEncoding_ == false && "still encoding");

		//if (wrapper.fence_ != VK_NULL_HANDLE) {
		//	vkDestroyFence(device_, wrapper.fence_, nullptr);
		//	wrapper.fence_ = VK_NULL_HANDLE;
		//}

		if (wrapper.finishedSignalSemaphore.semaphore_ != VK_NULL_HANDLE) {
			vkDestroySemaphore(device_, wrapper.finishedSignalSemaphore.semaphore_, nullptr);
			wrapper.finishedSignalSemaphore.semaphore_ = VK_NULL_HANDLE;
		}

		m_buffers_[i] = commandBuffers_[i].allocatedCmd_;
		commandBuffers_[i].allocatedCmd_ = VK_NULL_HANDLE;
		wrapper.isEncoding_ = false;
	}

	VkCommandPool pool = GetCommandPool(queue_);
	if (pool != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(device_, pool, kMax, m_buffers_);
	}
}
[[nodiscard]] inline smvk::CommandSubmitHandle smvk::CommandBufferManager::acquire(uint32_t goal)
{
	WrapperSlot* picked = nullptr;
	uint32_t pickedIndex = 0;

	if (availableSlots == 0) {
		//waitAny(); // MAybe this is more sensible.
		waitAll(); //obv not ok ! TODO::
	};
	// just iterate to find first free
	for (auto& slot : commandBuffers_) {
		if (slot.wrapper_.cmd == VK_NULL_HANDLE) {
			picked = &slot;
			break;
		}
		pickedIndex++;
	}
	if (picked == nullptr) { assert(false); }
	if (pickedIndex == kMax) {
		assert(false && "couldnt pick");
	}

	picked->wrapper_.cmd = picked->allocatedCmd_;
	picked->wrapper_.finishedSignalSemaphore.goal_ = goal + picked->gen_;
	availableSlots--;

	VkCommandBufferBeginInfo beginInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};

	vkBeginCommandBuffer(picked->wrapper_.cmd, &beginInfo);
	return CommandSubmitHandle{ .index_ = pickedIndex , .gen_ = picked->gen_ };
}
[[nodiscard]] smvk::CommandBufferManager::CommandBuffer& smvk::CommandBufferManager::GetCommandBuffer(CommandSubmitHandle handle) {
	assert(handle.gen_ == commandBuffers_[handle.index_].gen_);
	return commandBuffers_[handle.index_].wrapper_;
}
inline void smvk::CommandBufferManager::submit(CommandSubmitHandle handle) {
	if (!validHandle(handle)) { assert(false && "invalid handle"); }
	auto& wrapper_ = commandBuffers_[handle.index_].wrapper_;
	vkEndCommandBuffer(wrapper_.cmd);
	//VkQueue queue = wrapper.queue_ == QueuePreference::TRANSFER ? core_.transferQueue_ : core_.graphicsQueue_;
	VkQueue queue = graphicsQueue_; // defaultComamndPool_ routes to this queue so...
	//WAITING ON
	uint32_t waitSemaphoresCount = 0;
	VkSemaphoreSubmitInfo waitSemaphores[2];
	//
	VkSemaphoreSubmitInfo signalFinishedSemaphoresSubmitInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = wrapper_.finishedSignalSemaphore.semaphore_,
		.value = wrapper_.finishedSignalSemaphore.goal_
	};

	VkCommandBufferSubmitInfo commandsSubmitInfo
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.pNext = nullptr,
		.commandBuffer = wrapper_.cmd,
		.deviceMask = 0
	};

	VkSubmitInfo2 submitInfo{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.pNext = nullptr,
		.waitSemaphoreInfoCount = waitSemaphoresCount,
		.pWaitSemaphoreInfos = waitSemaphores,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &commandsSubmitInfo,
		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &signalFinishedSemaphoresSubmitInfo
	};
	vkQueueSubmit2(queue, 1, &submitInfo, VK_NULL_HANDLE);
	//vkQueueWaitIdle(queue);

	wrapper_.isEncoding_ = false;
	m_lastSubmit = handle;
}

inline bool smvk::CommandBufferManager::validHandle(CommandSubmitHandle handle) const { return handle.gen_ == commandBuffers_[handle.index_].gen_; }
inline bool smvk::CommandBufferManager::isReady(CommandSubmitHandle handle) {
	wait(handle); // crude
	return true;
} // TODO:: needs implementation
inline void smvk::CommandBufferManager::purge() {
	for (uint32_t i = 0; i < kMax; i++) {
		auto& wrp = commandBuffers_[i].wrapper_;
		if (wrp.cmd == VK_NULL_HANDLE || wrp.isEncoding_) { continue; }
		uint64_t waitValue = wrp.finishedSignalSemaphore.goal_;
		VkSemaphore semaphore = wrp.finishedSignalSemaphore.semaphore_;
		VkSemaphoreWaitInfo waitInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.pNext = nullptr,
			.flags = 0,
			.semaphoreCount = 1,
			.pSemaphores = &semaphore,
			.pValues = &waitValue,
		};

		VkResult result = vkWaitSemaphores(
			device_,
			&waitInfo,
			UINT64_MAX
		);
		if (result == VK_TIMEOUT) { assert(false); }
		reset({ .index_ = i, .gen_ = commandBuffers_[i].gen_ });
	}
}
inline void smvk::CommandBufferManager::wait(CommandSubmitHandle handle, uint64_t timeout) {
	auto wrp = GetCommandBuffer(handle);
	if (wrp.cmd == VK_NULL_HANDLE || wrp.isEncoding_) { assert(false); }
	uint64_t waitValue = wrp.finishedSignalSemaphore.goal_;
	VkSemaphore semaphore = wrp.finishedSignalSemaphore.semaphore_;

	VkSemaphoreWaitInfo waitInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.pNext = nullptr,
		.flags = 0,
		.semaphoreCount = 1,
		.pSemaphores = &semaphore,
		.pValues = &waitValue,
	};
	VkResult result = vkWaitSemaphores(device_,
		&waitInfo,
		timeout
	);

	if (result == VK_TIMEOUT) { assert(false && "timeout reached"); }
	availableSlots++;

}
inline void smvk::CommandBufferManager::waitAll() {
	VkSemaphore sems[kMax];
	uint64_t goals[kMax];
	uint32_t numSems = 0;

	for (uint32_t i = 0; i < kMax; i++) {
		auto& wrp = commandBuffers_[i].wrapper_;
		if (wrp.cmd == VK_NULL_HANDLE || wrp.isEncoding_) { continue; }
		goals[numSems] = wrp.finishedSignalSemaphore.goal_;
		sems[numSems] = wrp.finishedSignalSemaphore.semaphore_;
		numSems++;
	};

	VkSemaphoreWaitInfo waitInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.pNext = nullptr,
		.flags = 0,
		.semaphoreCount = numSems,
		.pSemaphores = sems,
		.pValues = goals,
	};

	VK_CHECK(vkWaitSemaphores(device_, &waitInfo, UINT64_MAX));

	for (uint32_t i = 0; i < kMax; i++) {
		auto& wrp = commandBuffers_[i].wrapper_;
		if (wrp.cmd == VK_NULL_HANDLE || wrp.isEncoding_) { continue; }
		reset({ i, commandBuffers_[i].gen_ });
	};

}
inline void smvk::CommandBufferManager::reset(CommandSubmitHandle handle) {
	auto& wrapper = GetCommandBuffer(handle);
	wrapper.isEncoding_ = false;
	vkResetCommandBuffer(wrapper.cmd, VkCommandBufferResetFlags{ 0 });
	wrapper.cmd = VK_NULL_HANDLE;
	auto& gen = commandBuffers_[handle.index_].gen_;
	gen++;
	// if overflowing set to 1. 
	if (gen == 0) {
		gen = 1;
	}
	availableSlots++;
}
//
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
	// This is a hardware/spec limit. Thats why im asserting here...
	// Maybe we can query gpu capabilities to give better paths to more powerful devices.
	assert(properties.limits.maxUniformBufferRange >= 1 << 16);

	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	//samplerInfo.compareOp = compareOp;

	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = samplerCreateInfo.maxLod;
	//samplerInfo.maxLod = float(samplerCreateInfo.levelCount - 1);

	VK_CHECK(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler));

}
void smvk::VulkanCore::CreateImageView(ImageViewCreateInfo imageViewCreateInfo, VkImageView& view)
{
	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = imageViewCreateInfo.vk_image;
	viewInfo.viewType = imageViewCreateInfo.viewType;
	viewInfo.format = imageViewCreateInfo.vk_format;
	viewInfo.subresourceRange = imageViewCreateInfo.subResourceRange;
	// Lets default this like that ourselves.
	viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

	VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &view));
}
// 
smvk::VulkanSwapchain::VulkanSwapchain(Context& context, GLFWwindow* window, uint32_t framesInFlight, uint32_t desiredImageCount)
	: ctx_(context),
	window_(window),
	framesInFlight_(framesInFlight),
	desiredImageCount_(desiredImageCount) {

	vk_depthFormat_ = findDepthFormat();
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

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		vkDestroyCommandPool(device_, perFrameCommandPools[i], nullptr);
	};

	DestroySyncObjects();
	CleanupSwapchain();
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

	CleanupSwapchain();

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
	//VkPresentModeKHR presentMode = chooseSwapPresentMode(support.presentModes);

	uint32_t imageCount = desiredImageCount_;
	if (imageCount < support.capabilities.minImageCount) {
		imageCount = support.capabilities.minImageCount;
	}

	if (support.capabilities.maxImageCount > 0 &&
		imageCount > support.capabilities.maxImageCount) {
		imageCount = support.capabilities.maxImageCount;
	}

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
	vkGetSwapchainImagesKHR(device_, vk_swapchain_, &actualImageCount, images_.data());
	imageViews_.resize(actualImageCount);
	renderFinishedSemaphores_.resize(actualImageCount, VK_NULL_HANDLE);
	vk_surfaceFormat_ = surfaceFormat.format;
	extent_ = swapExtent;
	glfwGetWindowSize(window_, &windowWidth, &windowHeight);
}
void smvk::VulkanSwapchain::CreateImageViews() {
	for (size_t i = 0; i < images_.size(); ++i) {
		auto viewCreateInfo = ImageViewCreateInfo{
			.flags = VK_IMAGE_ASPECT_COLOR_BIT,
			.vk_image = images_[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.vk_format = vk_surfaceFormat_,
			.subResourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};
		ctx_.core_.CreateImageView(
			viewCreateInfo,
			imageViews_[i]
		);
	}
}
void smvk::VulkanSwapchain::CreateDepthResources() {
	assert(vk_depthFormat_ != VK_FORMAT_UNDEFINED && "core depthFormat not set. failed to create swapchain depth Image");
	depthImage_ = ctx_.CreateImage(
		ImageCreateInfo{
			.vk_extent = {extent_.width, extent_.height, 1 },
			.vk_format = vk_depthFormat_,
			.vk_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			.levels = 1,
			.arrayLayers = 1
		}
	);

	auto obj_ = ctx_.images_.get(depthImage_);
	auto viewCreateInfo = ImageViewCreateInfo{
		.vk_image = obj_->vk_image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.vk_format = obj_->vk_format,
		.subResourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
			| VK_IMAGE_ASPECT_STENCIL_BIT,
			.baseMipLevel = 0,
			.levelCount = VK_REMAINING_MIP_LEVELS,
			.baseArrayLayer = 0,
			.layerCount = VK_REMAINING_ARRAY_LAYERS	},
	};

	ctx_.core_.CreateImageView(
		viewCreateInfo,
		obj_->renderView
	);
	obj_->sampleView = obj_->renderView;

	// acquire and submit to transitiobn image layout. could be more sofisticated really
	auto han = ctx_.m_submitter->acquire();
	auto wrapper = ctx_.m_submitter->GetCommandBuffer(han);
	//VkCommandBuffer cmd = commandBuffers[currentFrame_];
	ImageBarrier(
		wrapper.cmd,
		obj_->vk_image,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		{
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT ,
			.baseMipLevel = 0,
			.levelCount = VK_REMAINING_MIP_LEVELS, // there should be only 1 do
			.baseArrayLayer = 0,
			.layerCount = VK_REMAINING_ARRAY_LAYERS
		},
		VK_PIPELINE_STAGE_2_NONE,
		VK_ACCESS_2_NONE,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
		VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
		VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT

	);
	ctx_.m_submitter->submit(han);

}
inline void smvk::VulkanSwapchain::CreateSyncObjects() {
	imageAvailableSemaphores_.resize(framesInFlight_, VK_NULL_HANDLE);
	inFlightFences_.resize(framesInFlight_, VK_NULL_HANDLE);

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for (size_t i = 0; i < renderFinishedSemaphores_.size(); ++i) {
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
inline void smvk::VulkanSwapchain::CleanupSwapchain() {
	if (depthImage_.valid()) {
		destroy(&ctx_, depthImage_);
		depthImage_ = {};
	}

	for (VkImageView view : imageViews_) {
		if (view != VK_NULL_HANDLE) {
			vkDestroyImageView(device_, view, nullptr);
		}
	}

	imageViews_.clear();
	images_.clear();

	if (vk_swapchain_ != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(device_, vk_swapchain_, nullptr);
		vk_swapchain_ = VK_NULL_HANDLE;
	}
}
//
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
smvk::FrameBuffer smvk::VulkanSwapchain::AcquireFramebuffer()
{

	//if framebufferResized ??
	auto f = GetInFlightFence();
	vkWaitForFences(
		device_,
		1,
		&f,
		VK_TRUE,
		UINT64_MAX
	);

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
		assert(false && "not implemented yet");
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	SetCurrentImageIndex(imageIndex);

	vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);
	vkResetCommandPool(device_, perFrameCommandPools[currentFrame_], 0);

	//UpdateDeltaTime();

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
		{ // maybe the api surface should specialize subresoruces to avoid having to think about this
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
	VkSemaphore signalSemaphores[] = { renderFinishedSemaphores_[currentFrame_] };
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;


	if (
		vkQueueSubmit(
			graphicsQueue_,
			1,
			&submitInfo,
			GetInFlightFence()
		) != VK_SUCCESS
		) {
		assert(false && "failed to submit draw");
		throw std::runtime_error("failed to submit draw command buffer!");
	}

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

	//waitAll(); // maybe ?
	AdvanceFrame();

}

//
smvk::SamplerPool::SamplerPool(SamplerPoolCreateInfo createInfo) : device_(createInfo.device) {
	VkSampler defaultSampler;
	vkCreateSampler(device_, {}, nullptr, &defaultSampler);
	m_samplers[SamplerKey{}] = defaultSampler;
}

smvk::SamplerPool::~SamplerPool() {
	for (auto sampler : m_samplers)
	{
		vkDestroySampler(device_, sampler.second, nullptr);
	}
	m_samplers.clear();
}

VkSampler smvk::SamplerPool::GetSampler(SamplerKey key) {
	auto it = m_samplers.find(key);
	if (it == m_samplers.end()) {
		VkSampler newSampler;
		VkSamplerCreateInfo createInfo{};
		// populate
		vkCreateSampler(device_, &createInfo, nullptr, &newSampler);
		m_samplers[key] = newSampler;
		return newSampler;
	}
	else {
		return it->second;
	}

}

