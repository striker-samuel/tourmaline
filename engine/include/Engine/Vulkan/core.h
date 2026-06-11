#pragma once

#include "volk.h"
#include <Engine/Vulkan/vulkan_helpers.h>


#define APPLICATION_NAME "Sam2025Vulkan_one_three"
#define ENGINE_NAME "SamEngine"


struct CommandScope {};

struct VulkanCore {
	ktxVulkanDeviceInfo ktxInfo{};


	VkInstance instance_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;

	VkQueue graphicsQueue_ = VK_NULL_HANDLE;
	VkQueue presentQueue_ = VK_NULL_HANDLE;
	VkQueue transferQueue_ = VK_NULL_HANDLE;

	// not sure about this
	VkSurfaceKHR surface_ = VK_NULL_HANDLE;

	// Dont like having a commandPool default.
	VkCommandPool defaultCommandPool_ = VK_NULL_HANDLE;
	// ----

	VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;

	//
	VkPhysicalDeviceProperties properties{};

	// TODO
	//features1
	//features11
	//features12
	//features13

	void GetQueues() {

		QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);
		VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = indices.graphicsFamily.value()
		};

		vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);

		// This should pick transfer over graphics
		vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &transferQueue_);

		for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
			if (vkCreateCommandPool(device_, &poolInfo, nullptr, &defaultCommandPool_) !=
				VK_SUCCESS) {
				throw std::runtime_error("failed to create graphics command pool!");
			}
		}

		// may be wrong . 
		ktxInfo = *ktxVulkanDeviceInfo_Create(
			physicalDevice_,
			device_,
			transferQueue_,
			// this maybe should be transfer queue. we only use ktx to upload tex.
			// no blitting for mipmaps, which are generated offline
			// runtime genreation for mipmaps in ktx seems to be not up to par with SOA
			defaultCommandPool_,
			NULL
		);
	}

	ktxVulkanDeviceInfo GetKtxPayloadInfo() {
		return ktxInfo;
	}

	VulkanCore() {
		{
			if (enableValidationLayers && !checkValidationLayerSupport()) {
				throw std::runtime_error(
					"validation layers requested, but not available!");
			}

			VkApplicationInfo appInfo{};
			appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
			appInfo.pApplicationName = APPLICATION_NAME;
			appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
			appInfo.pEngineName = ENGINE_NAME;
			appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
			appInfo.apiVersion = VK_API_VERSION_1_3;

			VkInstanceCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			createInfo.pApplicationInfo = &appInfo;

			auto extensions = getRequiredExtensions();
			createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
			createInfo.ppEnabledExtensionNames = extensions.data();

			if (enableValidationLayers) {
				createInfo.enabledLayerCount =
					static_cast<uint32_t>(validationLayers.size());
				createInfo.ppEnabledLayerNames = validationLayers.data();

				VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo;
				populateDebugMessengerCreateInfo(debugCreateInfo);
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

		}

		if (enableValidationLayers) {

			VkDebugUtilsMessengerCreateInfoEXT createInfo;
			populateDebugMessengerCreateInfo(createInfo);

			if (vkCreateDebugUtilsMessengerEXT(instance_, &createInfo, nullptr,
				&debugMessenger_) != VK_SUCCESS) {
				throw std::runtime_error("failed to set up debug messenger!");
			}
		}


		// Pick Device
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);

		if (deviceCount == 0) {
			throw std::runtime_error("failed to find GPUs with Vulkan support!");
		}

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

		for (const auto& device : devices) {
			if (isDeviceSuitable(device, surface_)) {
				physicalDevice_ = device;
				break;
			}
		}

		if (physicalDevice_ == VK_NULL_HANDLE) {
			throw std::runtime_error("failed to find a suitable GPU!");
		}

		vkGetPhysicalDeviceProperties(physicalDevice_, &properties);

#ifndef DEBUG

		std::cout << "----DEVICE INFO-----" << std::endl;

		std::cout << "	maxUniformBufferRange = "
			<< properties.limits.maxUniformBufferRange << " bytes\n";

		std::cout << "	minUniformBufferOffsetAlignment = "
			<< properties.limits.minUniformBufferOffsetAlignment << " bytes\n";

		std::cout << "	maxStorageBufferRange = "
			<< properties.limits.maxStorageBufferRange << " bytes\n\n";
#endif

		// Create Logical Device
		{
			QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, surface_);

			std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
			std::set<uint32_t> uniqueQueueFamilies = { indices.graphicsFamily.value(),
													  indices.presentFamily.value() };

			float queuePriority = 1.0f;
			for (uint32_t queueFamily : uniqueQueueFamilies) {
				VkDeviceQueueCreateInfo queueCreateInfo{};
				queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				queueCreateInfo.queueFamilyIndex = queueFamily;
				queueCreateInfo.queueCount = 1;
				queueCreateInfo.pQueuePriorities = &queuePriority;
				queueCreateInfos.push_back(queueCreateInfo);
			}


			// Descriptor indexing (core, not EXT)
			VkPhysicalDeviceDescriptorIndexingFeatures descIdx{};
			descIdx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
			// Enable what you actually need:
			descIdx.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
			descIdx.descriptorBindingPartiallyBound = VK_TRUE;
			descIdx.runtimeDescriptorArray = VK_TRUE;
			descIdx.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

			// Maybe I dont want this one
			descIdx.descriptorBindingVariableDescriptorCount = VK_TRUE;

			VkPhysicalDeviceVulkan13Features vulkan13Features{};
			vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			vulkan13Features.dynamicRendering = VK_TRUE;
			vulkan13Features.pNext = &descIdx;

			// Core features (BC + anisotropy) go in Features2.features
			VkPhysicalDeviceFeatures2 feats2{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
				.pNext = &vulkan13Features,
				.features = {
					.samplerAnisotropy = VK_TRUE,
					.textureCompressionBC = VK_TRUE, // REQUIRED for BC1..BC7
				},
			};

			VkDeviceCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			createInfo.queueCreateInfoCount =
				static_cast<uint32_t>(queueCreateInfos.size());
			createInfo.pQueueCreateInfos = queueCreateInfos.data();

			// Using Features2
			createInfo.pEnabledFeatures = nullptr;
			createInfo.pNext = &feats2;

			createInfo.enabledExtensionCount =
				static_cast<uint32_t>(deviceExtensions.size());
			createInfo.ppEnabledExtensionNames = deviceExtensions.data();

			if (enableValidationLayers) {
				createInfo.enabledLayerCount =
					static_cast<uint32_t>(validationLayers.size());
				createInfo.ppEnabledLayerNames = validationLayers.data();
			}
			else {
				createInfo.enabledLayerCount = 0;
			}

			if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) !=
				VK_SUCCESS) {
				throw std::runtime_error("failed to create logical device!");
			}

			volkLoadDevice(device_);

			vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
			vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);

			// Assert for BC7
			VkFormatProperties p{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice_, VK_FORMAT_BC7_SRGB_BLOCK, &p);
			bool sampleOK = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
			assert(sampleOK);

		}


		GetQueues();

	}

};
