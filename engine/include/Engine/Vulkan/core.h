#pragma once

#include <cassert>
#include "volk.h"
#include <Engine/Renderer/constants.h>
#include <Engine/Vulkan/vulkan_helpers.h>

#define APPLICATION_NAME "Sam2025Vulkan_one_three"
#define ENGINE_NAME "SamEngine"

// DEFAULT INITIALIZATION

static constexpr uint32_t DEFAULT_WIDTH = 1280;
static constexpr uint32_t DEFAULT_HEIGHT = 720;
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t DESIRED_SWAPCHAIN_IMAGE_COUNT = 2;

static constexpr uint32_t MAX_BUFFERS = 4096;
static constexpr uint32_t MAX_DESCRIPTORS = 4096;
static constexpr uint32_t MAX_DRAWS = 1024;
static constexpr uint32_t MAX_INSTANCES = 1000;
static constexpr uint32_t MAX_MESHES = 1000;
static constexpr uint32_t MAX_LIGHTS = 10;
static constexpr uint32_t MAX_TEXTURES = 4096;
static constexpr uint32_t MAX_IMAGES = 4096;
static constexpr uint32_t MAX_PUSH_CONSTANTS = 8;

struct /*class*/ VulkanCore {

	VkInstance instance_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
	//
	VkQueue graphicsQueue_ = VK_NULL_HANDLE;
	VkQueue presentQueue_ = VK_NULL_HANDLE;
	VkQueue transferQueue_ = VK_NULL_HANDLE;
	// 
	VkSurfaceKHR surface_;
	VkFormat viewportDepthFormat;


	const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation", };
	const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

	// Dont like having a commandPool default. // also dont like the name
	VkCommandPool defaultCommandPool_ = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;

	// Enviroment Descriptions
	VkPhysicalDeviceDriverProperties vkPhysicalDeviceDriverProperties_ = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, nullptr };
	VkPhysicalDeviceVulkan13Properties vkPhysicalDeviceVulkan13Properties_ = {
		 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
		 &vkPhysicalDeviceDriverProperties_,
	};
	// provided by Vulkan 1.2
	VkPhysicalDeviceVulkan12Properties vkPhysicalDeviceVulkan12Properties_ = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
		&vkPhysicalDeviceVulkan13Properties_,
	};
	VkPhysicalDeviceDepthStencilResolveProperties vkPhysicalDeviceDepthStencilResolveProperties_ = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
		&vkPhysicalDeviceVulkan12Properties_,
	};
	// provided by Vulkan 1.1
	VkPhysicalDeviceVulkan11Properties vkPhysicalDeviceVulkan11Properties_ = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
		&vkPhysicalDeviceDepthStencilResolveProperties_,
	};
	VkPhysicalDeviceProperties2 vkPhysicalDeviceProperties2_ = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		&vkPhysicalDeviceVulkan11Properties_,
		VkPhysicalDeviceProperties{},
	};
	VkPhysicalDeviceProperties2& GetPhysicalDeviceProperties() { return this->vkPhysicalDeviceProperties2_; };

	// missing 1.4 .. dont care yet
	VkPhysicalDeviceVulkan13Features vkFeatures13_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	VkPhysicalDeviceVulkan12Features vkFeatures12_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &vkFeatures13_ };
	VkPhysicalDeviceVulkan11Features vkFeatures11_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &vkFeatures12_ };
	VkPhysicalDeviceFeatures2 vkFeatures10_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vkFeatures11_ };
	// 

public:

	VulkanCore() {};
	//VulkanCore(GLFWwindow* window) {};
	VulkanCore(const VulkanCore&) = delete;
	VulkanCore& operator=(const VulkanCore&) = delete;

	VulkanCore(VulkanCore&&) = delete;
	VulkanCore& operator=(VulkanCore&&) = delete;

	// hacky way. Dont want to deal with finding and setting up queue. not needed to develop the rest of the API
	VkQueue GetTransferQueue() { return transferQueue_ != VK_NULL_HANDLE ? transferQueue_ : graphicsQueue_; };
	//const ktxVulkanDeviceInfo GetKtxPayloadInfo() { return ktxInfo_; };

	void Init() {

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

		if (enableValidationLayers) {

			VkDebugUtilsMessengerCreateInfoEXT createInfo;
			populateDebugMessengerCreateInfo(createInfo);

			if (vkCreateDebugUtilsMessengerEXT(instance_, &createInfo, nullptr,
				&debugMessenger_) != VK_SUCCESS) {
				throw std::runtime_error("failed to set up debug messenger!");
			}
		};

	};
	void Init2() {
		{

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

			vkGetPhysicalDeviceProperties2(physicalDevice_, &vkPhysicalDeviceProperties2_);

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

				// this is really hacky 
				// Assert for BC7
				VkFormatProperties p{};
				vkGetPhysicalDeviceFormatProperties(physicalDevice_, VK_FORMAT_BC7_SRGB_BLOCK, &p);
				bool sampleOK = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
				assert(sampleOK);

			}

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
			viewportDepthFormat = findDepthFormat(physicalDevice_);

		};

	};

	void CreateFence(const VkFenceCreateInfo createInfo, VkFence& out) {
		VK_CHECK(vkCreateFence(device_, &createInfo, nullptr, &out));
	};
	void CreateSemaphore(VkSemaphoreCreateInfo createInfo, VkSemaphore& out) {
		VK_CHECK(vkCreateSemaphore(device_, &createInfo, nullptr, &out));
	};
};

struct CommandSubmitter {

	struct ToyCommandBuffer {
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence_ = VK_NULL_HANDLE;
		VkSemaphore semaphore_ = VK_NULL_HANDLE;
		bool isEncoding_ = false;
		//QueuePreference queue_ = QueuePreference::GRAPHICS; // Dont know yet how to properly resolve this. will come with real use
	};

	struct Slot {
		ToyCommandBuffer cmd{};
		uint32_t gen_ = 1;
	};

	struct CommandSubmitHandle {
		uint32_t index_ = 0;
		uint32_t gen_ = 1;
	};
	const VkSemaphoreCreateInfo createSemaphoreInfo{
	  .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	  .flags = 0,
	};
	const VkFenceCreateInfo createFenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = 0,
	};
	enum struct QueuePreference {
		TRANSFER,
		GRAPHICS
	};
	CommandSubmitter(VulkanCore& core) : core_(core) {};
	~CommandSubmitter() {};
	bool isReady(CommandSubmitHandle handle) { return true; };

	CommandSubmitHandle acquire() {
		//auto cmd = beginSingleTimeCommands(core_.device_, core_.defaultCommandPool_);
		ToyCommandBuffer wrapper = {
			.cmd = cmd,
			.fence_ = VK_NULL_HANDLE,
			.semaphore_ = VK_NULL_HANDLE,
			.isEncoding_ = true
		};
		core_.CreateFence(createFenceInfo, wrapper.fence_);
		core_.CreateSemaphore(createSemaphoreInfo, wrapper.semaphore_);

		auto gen = commandBuffers_[nextFree].gen_;
		commandBuffers_[nextFree] = { .cmd = wrapper, .gen_ = gen };
	};

	void submit(CommandSubmitHandle handle) {
		auto slot = commandBuffers_[handle.index_];

		//endSingleTimeCommands(
		//	core_.device_,
		//	slot.cmd.cmd,
		//	core_.graphicsQueue_,
		//	//cmd->queue_ == QueuePreference::GRAPHICS ? core_.graphicsQueue_ : core_.transferQueue_,
		//	core_.defaultCommandPool_
		//);
	}

	CommandSubmitter(VulkanCore& core) : core_(core) {};
	~CommandSubmitter() {};
private:
	static constexpr uint32_t kMax = 64;
	uint32_t nextFree = kMax;
	uint32_t commandBuffersAmmount_ = 0;
	Slot commandBuffers_[kMax];
	VulkanCore& core_;
};

struct DeferredCommandBuffer {
	DeferredCommandBuffer(std::packaged_task<void()>&& task, CommandSubmitter::CommandSubmitHandle handle) : task_(std::move(task)), handle_(handle) {}
	std::packaged_task<void()> task_;
	CommandSubmitter::CommandSubmitHandle handle_;
};


