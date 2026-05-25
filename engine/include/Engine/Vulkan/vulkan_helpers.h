#pragma once

#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <limits.h>
#include <optional>

#include <array>
#include <set>

#include <stdexcept>
#include <algorithm>
#include <cstring>

#include <Engine/Shared/common.h>
#include <Engine/Renderer/primitives.h>

/*inline*/ const char* vkResultToString(VkResult result);

#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = (x);                                             \
        if (err != VK_SUCCESS) {                                        \
            fprintf(stderr, "[Vulkan Error] %s:%d: %s\n",               \
                    __FILE__, __LINE__, vkResultToString(err));         \
            abort();                                                    \
        }                                                               \
    } while (0)


const std::vector<const char*> validationLayers = {
	"VK_LAYER_KHRONOS_validation",
};

const std::vector<const char*> deviceExtensions = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	// Not needed -- Vk1.3 promoted baybeeee
	//VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME
};


static VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData);

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

struct QueueFamilyIndices {
	std::optional<uint32_t> graphicsFamily;
	std::optional<uint32_t> presentFamily;

	bool isComplete() {
		return graphicsFamily.has_value() && presentFamily.has_value();
	}
};

struct SwapChainSupportDetails {
	VkSurfaceCapabilitiesKHR capabilities{};
	std::vector<VkSurfaceFormatKHR> formats;
	std::vector<VkPresentModeKHR> presentModes;
};

//VkResult CreateDebugUtilsMessengerEXT(
//	VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
//	const VkAllocationCallbacks* pAllocator,
//	VkDebugUtilsMessengerEXT* pDebugMessenger);
//
//void DestroyDebugUtilsMessengerEXT(VkInstance instance,	VkDebugUtilsMessengerEXT debugMessenger,const VkAllocationCallbacks* pAllocator);

bool checkValidationLayerSupport();

VkFormat findSupportedFormat(
	VkPhysicalDevice physicalDevice,
	const std::vector<VkFormat>& candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features
);

bool hasStencilComponent(VkFormat format);

VkFormat findDepthFormat(
	VkPhysicalDevice physicalDevice
);
uint32_t findMemoryType(
	VkPhysicalDevice physicalDevice,
	uint32_t typeFilter,
	VkMemoryPropertyFlags properties
);
VkShaderModule createDeviceShaderModule(VkDevice device, const std::vector<char>& code);
SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);
QueueFamilyIndices findQueueFamilies(
	VkPhysicalDevice device,
	VkSurfaceKHR surface
);

bool checkDeviceExtensionSupport(VkPhysicalDevice device);
bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface);
VkSurfaceFormatKHR chooseSwapSurfaceFormat(
	const std::vector<VkSurfaceFormatKHR>& availableFormats);

VkPresentModeKHR chooseSwapPresentMode(
	const std::vector<VkPresentModeKHR>& availablePresentModes);

VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window);
std::vector<const char*> getRequiredExtensions();
void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);
VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool commandPool);
void endSingleTimeCommands(VkDevice device, VkCommandBuffer commandBuffer, VkQueue queue, VkCommandPool commandPool);
