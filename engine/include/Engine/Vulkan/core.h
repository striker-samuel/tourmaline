#pragma once

#include <cassert>
#include <array>
#include <tuple>
#include <map>
#include <future>
#include <algorithm>
#include <optional>
#include <vector>
#include <set>
#include <future>
//
#include "vk_mem_alloc.h"
//
#include "volk.h"
//
#include "Engine/Shared/common.h"
#include <Engine/Memory/pool.h>
//
#include <ktx.h>
#include <ktxvulkan.h>
//
#define APPLICATION_NAME "Sam2025Vulkan_one_three"
#define ENGINE_NAME "SamEngine"
//
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t DESIRED_SWAPCHAIN_IMAGE_COUNT = 2;
//
static constexpr uint32_t DEFAULT_WIDTH = 1280;
static constexpr uint32_t DEFAULT_HEIGHT = 720;
//
static constexpr uint32_t MAX_BUFFERS = 4096;
static constexpr uint32_t MAX_DRAWS = 1024;
static constexpr uint32_t MAX_LIGHTS = 4096;
static constexpr uint32_t MAX_TEXTURES = 4096;
//
static constexpr uint32_t MAX_PUSH_CONSTANTS = 8;
//

inline const char* vkResultToString(VkResult result);

#define VK_ASSERT_RETURN(func)                                     \
  {                                                                \
    const VkResult vk_assert_result = func;                        \
    if (vk_assert_result != VK_SUCCESS) {                          \
      LLOGW("Vulkan API call failed: %s:%i\n  %s\n  %s\n", \
                    __FILE__,                                      \
                    __LINE__,                                      \
                    #func,                                         \
                    lvk::getVulkanResultString(vk_assert_result)); \
      assert(false);                                               \
      return getResultFromVkResult(vk_assert_result);              \
    }                                                              \
  }

#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = (x);                                             \
        if (err != VK_SUCCESS) {                                        \
            fprintf(stderr, "[Vulkan Error] %s:%d: %s\n",               \
                    __FILE__, __LINE__, vkResultToString(err));         \
            abort();                                                    \
        }                                                               \
    } while (0)


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
	std::optional<uint32_t> transferFamily;

	bool isComplete() {
		return graphicsFamily.has_value() && presentFamily.has_value();
	}
};

struct SwapChainSupportDetails {
	VkSurfaceCapabilitiesKHR capabilities{};
	std::vector<VkSurfaceFormatKHR> formats;
	std::vector<VkPresentModeKHR> presentModes;
};


// This are mostly lifted from the vulkan tutorial page and really should be refactored into core.
//
std::vector<const char*> getRequiredExtensionsForGlfw();
bool checkDeviceExtensionSupport(VkPhysicalDevice device, const std::vector<const char*> deviceExtensions);
bool checkValidationLayerSupport(const std::vector<const char*> validationLayers);
bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface);
//
SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);
QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface);
//
VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window);
//
VkFormat findSupportedFormat(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
VkFormat findDepthFormat(VkPhysicalDevice physicalDevice);
//

namespace SamToy {

	// Just VK_1_3 for the moment
	// https://asawicki.info/articles/state_of_gpu_hardware_2025.php
	// Should be more than ok
	enum VulkanVersion {
		VK_1_3
	};

	enum struct QueuePreference {
		GRAPHICS,
		TRANSFER,
		COMPUTE
	};

	struct VulkanCore {

		struct CoreConfiguration {
			VulkanVersion version_ = VK_1_3;
			const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation", };
			const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
			const std::vector<const char*> instanceExtensions = {};
		} config_;
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
		//
		VkCommandPool defaultCommandPool_ = VK_NULL_HANDLE;
		VkCommandPool transferCommandPool_ = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
		//

		// Enviroment Descriptions
		VkPhysicalDeviceDriverProperties vkPhysicalDeviceDriverProperties_ = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, nullptr };
		VkPhysicalDeviceDepthStencilResolveProperties vkPhysicalDeviceDepthStencilResolveProperties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
			&vkPhysicalDeviceVulkan12Properties_,
		};
		VkPhysicalDeviceVulkan13Properties vkPhysicalDeviceVulkan13Properties_ = {
			 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
			 &vkPhysicalDeviceDriverProperties_,
		};
		// provided by Vulkan 1.2
		VkPhysicalDeviceVulkan12Properties vkPhysicalDeviceVulkan12Properties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
			&vkPhysicalDeviceVulkan13Properties_,
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

		VulkanCore(CoreConfiguration config) :config_(config) {};
		VulkanCore(const VulkanCore&) = delete;
		VulkanCore& operator=(const VulkanCore&) = delete;
		VulkanCore(VulkanCore&&) = delete;
		VulkanCore& operator=(VulkanCore&&) = delete;


		bool Init(
			VkSurfaceKHR* surface = nullptr
		) {
			if (enableValidationLayers && !checkValidationLayerSupport(config_.validationLayers)) {
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

			auto extensions = getRequiredExtensionsForGlfw();

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

					VkPhysicalDeviceVulkan11Features deviceFeatures11{
						.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
						.pNext = nullptr,
						.storageBuffer16BitAccess = VK_TRUE,
						.uniformAndStorageBuffer16BitAccess = VK_TRUE,
						.storagePushConstant16 = VK_TRUE,
						.storageInputOutput16 = VK_TRUE
					};
					VkPhysicalDeviceVulkan12Features deviceFeatures12{
						.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
						.pNext = &deviceFeatures11,
						.shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
						.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
						.descriptorBindingPartiallyBound = VK_TRUE,
						.descriptorBindingVariableDescriptorCount = VK_TRUE,
						.runtimeDescriptorArray = VK_TRUE,
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

		};

		//
		VkCommandPool GetCommandPool(QueuePreference preferredQueue) {
			switch (preferredQueue) {
			case QueuePreference::GRAPHICS:
				return defaultCommandPool_;

			case QueuePreference::TRANSFER:
				return transferCommandPool_;

			case QueuePreference::COMPUTE:
				assert(false && "Compute queue command pool not implemented");
				return VK_NULL_HANDLE;
			}

			assert(false && "Unknown QueuePreference");
			return VK_NULL_HANDLE;
		}
		// All these utilities and the whole core struct just does single device operation!
		void CreateFence(const VkFenceCreateInfo createInfo, VkFence& out) {
			VK_CHECK(vkCreateFence(device_, &createInfo, nullptr, &out));
		};
		void CreateSemaphore(VkSemaphoreCreateInfo createInfo, VkSemaphore& out) {
			VK_CHECK(vkCreateSemaphore(device_, &createInfo, nullptr, &out));
		};
		void CreateShaderModule(const std::vector<char>& code, VkShaderModule& out) {

			VkShaderModuleCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			createInfo.codeSize = code.size();
			createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

			if (vkCreateShaderModule(device_, &createInfo, nullptr, &out) !=
				VK_SUCCESS) {
				throw std::runtime_error("failed to create shader module!");
			}

		};
		VkFormat GetDepthFormat();
	};


	struct CommandSubmitHandle {
		uint32_t index_ = 0;
		uint32_t gen_ = 0; // gen 0 means no slot. alas empty. 
		bool empty() { return gen_ == 0; };
	};

	struct CommandBufferManager {

		struct TimelineSemaphore {
			VkSemaphore semaphore_ = VK_NULL_HANDLE;
			uint64_t initialValue_ = 0;
			uint64_t goal_ = 1; //defautl to 1
		};

		struct ToyCommandBuffer {
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			//VkFence fence_ = VK_NULL_HANDLE;
			//VkSemaphore semaphore_ = VK_NULL_HANDLE;
			TimelineSemaphore finishedSignalSemaphore{};
			bool isEncoding_ = false;
			QueuePreference queue_ = QueuePreference::GRAPHICS; // Dont know yet how to properly resolve this. will come with real use
		};

		//
		struct WrapperSlot {
			ToyCommandBuffer wrapper_{};
			VkCommandBuffer allocatedCmd_;
			uint32_t gen_ = 1;
		};
		//

		CommandBufferManager(VulkanCore& core, QueuePreference queue)
			:
			core_(core)
			, queue_(queue)
		{
			VkCommandPool pool = core.GetCommandPool(queue);
			if (pool == VK_NULL_HANDLE) { assert(false && "no commandPool"); };
			VkCommandBufferAllocateInfo info{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					.pNext = nullptr,
					.commandPool = pool,
					.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, //yes, just primary
					.commandBufferCount = kMax,
			};
			VkCommandBuffer buffers[kMax];
			VK_CHECK(vkAllocateCommandBuffers(core_.device_, &info, buffers));

			VkSemaphoreTypeCreateInfo semaphoreTypeInfo{};
			semaphoreTypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
			semaphoreTypeInfo.pNext = nullptr;
			semaphoreTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
			semaphoreTypeInfo.initialValue = 0; // starting counter value

			for (int i = 0; i < kMax; i++) {
				commandBuffers_[i].allocatedCmd_ = buffers[i];
				commandBuffers_[i].wrapper_.isEncoding_ = false;
				//core_.CreateFence(VkFenceCreateInfo{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = 0 }, commandBuffers_[i].wrapper_.fence_);
				core_.CreateSemaphore(VkSemaphoreCreateInfo
					{
						.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
						.pNext = &semaphoreTypeInfo,
						.flags = 0,
					},
					commandBuffers_[i].wrapper_.finishedSignalSemaphore.semaphore_
					);
			}
		};
		~CommandBufferManager() {
			if (core_.device_ == VK_NULL_HANDLE) {
				return;
			}

			// Conservative. You can remove this if the owner guarantees GPU work is complete.
			vkDeviceWaitIdle(core_.device_);

			VkCommandBuffer buffers[kMax]{};

			for (uint32_t i = 0; i < kMax; ++i) {
				auto& wrapper = commandBuffers_[i].wrapper_;
				assert(wrapper.isEncoding_ == false && "still encoding");

				//if (wrapper.fence_ != VK_NULL_HANDLE) {
				//	vkDestroyFence(core_.device_, wrapper.fence_, nullptr);
				//	wrapper.fence_ = VK_NULL_HANDLE;
				//}

				if (wrapper.finishedSignalSemaphore.semaphore_ != VK_NULL_HANDLE) {
					vkDestroySemaphore(core_.device_, wrapper.finishedSignalSemaphore.semaphore_, nullptr);
					wrapper.finishedSignalSemaphore.semaphore_ = VK_NULL_HANDLE;
				}

				buffers[i] = commandBuffers_[i].allocatedCmd_;
				commandBuffers_[i].allocatedCmd_ = VK_NULL_HANDLE;
				wrapper.isEncoding_ = false;
			}

			VkCommandPool pool = core_.GetCommandPool(queue_);
			if (pool != VK_NULL_HANDLE) {
				vkFreeCommandBuffers(core_.device_, pool, kMax, buffers);
			}
		}


		void purge() {
			for (uint32_t i = 0; i << kMax; i++) {
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

				vkWaitSemaphores(
					core_.device_,
					&waitInfo,
					UINT64_MAX
				);
				reset({ .index_ = i, .gen_ = commandBuffers_[i].gen_ });
			}
		}

		void wait(CommandSubmitHandle handle) {
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
			vkWaitSemaphores(core_.device_,
				&waitInfo,
				UINT64_MAX
			);
		};

		void waitAll() {
			VkSemaphore sems[kMax];
			uint64_t goals[kMax];
			uint32_t numSems = 0;

			for (uint32_t i = 0; i << kMax; i++) {
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
			vkWaitSemaphores(
				core_.device_,
				&waitInfo,
				UINT64_MAX
			);
		};
		CommandSubmitHandle acquire(uint32_t goal = 0)
		{
			WrapperSlot picked{};
			uint32_t pickedIndex;

			if (availableSlots == 0) { wait(); };
			// just iterate to find first free
			for (auto slot : commandBuffers_) {
				if (slot.wrapper_.cmd == VK_NULL_HANDLE) {
					picked = slot;
				}
				pickedIndex++;
			}
			picked.wrapper_.cmd = picked.allocatedCmd_;
			picked.wrapper_.finishedSignalSemaphore.goal_ = goal;
			availableSlots--;

			VkCommandBufferBeginInfo beginInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
			};

			vkBeginCommandBuffer(picked.wrapper_.cmd, &beginInfo);
			return CommandSubmitHandle{ .index_ = pickedIndex , .gen_ = picked.gen_ };
		};

		ToyCommandBuffer& get(CommandSubmitHandle handle) {
			assert(validHandle(handle));
			return commandBuffers_[handle.index_].wrapper_;
		};
		void reset(CommandSubmitHandle handle) {
			auto& wrapper = get(handle);
			wrapper.isEncoding_ = false;
			//vkResetFences(core_.device_, 1, &wrapper.fence_);
			vkResetCommandBuffer(wrapper.cmd, VkCommandBufferResetFlags{ 0 });
			wrapper.cmd = VK_NULL_HANDLE;

			auto& gen = commandBuffers_[handle.index_].gen_;
			gen++;
			// if overflowing set to 1. 
			if (gen == 0) {
				gen = 1;
			}
			availableSlots++;
		};

		void submit(CommandSubmitHandle handle) {
			if (!validHandle(handle)) { assert(false && "invalid handle"); }
			auto& wrapper_ = commandBuffers_[handle.index_].wrapper_;
			vkEndCommandBuffer(wrapper_.cmd);
			//VkQueue queue = wrapper.queue_ == QueuePreference::TRANSFER ? core_.transferQueue_ : core_.graphicsQueue_;
			VkQueue queue = core_.graphicsQueue_; // defaultComamndPool_ routes to this queue so...


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
			vkQueueWaitIdle(queue);

			wrapper_.isEncoding_ = false;
			m_lastSubmit = handle;
		};

		bool validHandle(CommandSubmitHandle handle) {
			return handle.gen_ == commandBuffers_[handle.index_].gen_;
		};

		ToyCommandBuffer GetCommandBuffer(CommandSubmitHandle handle) {
			assert(handle.gen_ == commandBuffers_[handle.index_].gen_);
			return commandBuffers_[handle.index_].wrapper_;
		};
	private:
		VulkanCore& core_;
		static const uint32_t kMax = 64;
		uint32_t availableSlots = kMax;
		WrapperSlot commandBuffers_[kMax];
		CommandSubmitHandle m_lastSubmit{};
		QueuePreference queue_;
	};

	struct DeferredTask {
		DeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle) : task_(std::move(task)), handle_(handle) {}
		std::packaged_task<void()> task_;
		CommandSubmitHandle handle_;
	};

	// 
	using ImageHandle = Handle<struct Image>; // Attachments
	using PipelineHandle = Handle<struct Pipeline>;
	using ToyBufferHandle = Handle<struct Buffer>;
	using SamplerHandle = Handle<struct Sampler>;
	using ShaderModuleHandle = Handle<struct ShaderModule>;
	using DescriptorPoolHandle = Handle<struct DescriptorPool>;
	using DescriptorSetHandle = Handle<struct DescriptorSet>;
	//
	// Specialization for textures.
	using TextureHandle = Handle<struct Image>;
	//
	struct Pipeline {
		VkPipeline pipeline;
		VkPipelineLayout layout;
	};

	struct Image {
		VkImage image = VK_NULL_HANDLE;
		VmaAllocation alloc = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags usage = 0;
		VkExtent3D extent{};
		uint32_t arrayLayers = 0;
		uint32_t levels = 0;
		VkImageView sampleView = VK_NULL_HANDLE; // you can sample multiple levels
		VkImageView renderView = VK_NULL_HANDLE; // but can only render to one. Views are created with this baked in. Thats the why to separate them
		VkImageView arrayView[6]{
			VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
			VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE
		};
		// not owning
		VkSampler sampler_ = VK_NULL_HANDLE;
		//Image& setSampler(VkSampler sampler) { this->sampler_ = sampler; return *this; };
	};

	struct Buffer {
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation alloc{};
		//VmaAllocationInfo allocInfo{}; // this is just an info return. the info contained maybe altered during refrag so makes no sense storing it
		VkBufferUsageFlags usage;
		VkMemoryPropertyFlags memory_flags;
		bool is_mapable() const {
			return this->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		}
		bool operator()(const Buffer& a, const Buffer& b) const {
			return std::tie(a.buffer, a.alloc, a.usage, a.memory_flags)
				< std::tie(b.buffer, b.alloc, b.usage, b.memory_flags);
		}
	};

	/// <summary>
	///  One could argue this is just redundant provided that VkImageCreateInfo has this same thing typed out...
	/// </summary>
	struct ImageCreateInfo {
		int texWidth;
		int texHeight;
		VkFormat format;
		VkImageUsageFlags usage;
		uint32_t levels;
		uint32_t arrayLayers;
		bool operator<(const ImageCreateInfo& other) const {
			return std::tie(texWidth, texHeight, format, usage, levels, arrayLayers) <
				std::tie(other.texWidth, other.texHeight, other.format,
					other.usage, other.levels, other.arrayLayers);
		}
	};
	using ImageViewCreateInfo = VkImageViewCreateInfo; // hacky
	//struct ImageViewCreateInfo {};

	struct SamplerCreateInfo {
		VkFilter mag = VK_FILTER_LINEAR;
		VkFilter min = VK_FILTER_LINEAR;
		VkSamplerMipmapMode mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		VkSamplerAddressMode u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		VkSamplerAddressMode v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		VkSamplerAddressMode w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		float maxLod = 1000.0f;
		float maxAniso = 1.0f;
		VkCompareOp compareOp = VK_COMPARE_OP_ALWAYS;
		bool compareEnable = false;
	};
	using SamplerKey = SamplerCreateInfo;

	struct PipelineCreateInfo {

		//struct VertexFormatDesc {
		//};
		struct RasterSettings {
			uint32_t depthClampEnable = VK_FALSE;
			uint32_t rasterizerDiscardEnable = VK_FALSE;
			VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
			float lineWidth = 1.0f;
			uint32_t cullMode = VK_CULL_MODE_BACK_BIT;
			VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			uint32_t depthBiasEnable = VK_FALSE;
			// Depth bias for shadowmaps.
			float depthBiasConstant = 0.0f;
			float depthBiasSlope = 1.0f; // This should be PCF radius https://mynameismjp.wordpress.com/2013/09/10/shadow-maps/

		};
		struct BindingDescription {
			uint32_t binding = 0; // If we want sparse bindings we need this
			VkDescriptorType type{};
			uint32_t count = 1;
			VkShaderStageFlags stages = 0;
			VkDescriptorBindingFlags flags = 0;
			std::vector<VkSampler> immutableSamplers;
		};
		struct DescriptorSetDescription {
			VkDescriptorSetLayoutCreateFlags createFlags = 0; // e.g. UPDATE_AFTER_BIND_POOL_BIT
			std::vector<BindingDescription> bindings;
		};
		struct PushConstantDescription {
			VkShaderStageFlags stages = 0;
			uint32_t offset = 0;
			uint32_t size = 0;
		};
		struct PipelineLayoutDesc {
			bool valid = 0;
			std::vector<DescriptorSetDescription> setDescriptions; // each has set index
			std::vector<PushConstantDescription> pushConstants;
		};
		PipelineLayoutDesc layoutDescription{};
		RasterSettings rasterSettings{};
	};

	using PipelineKey = PipelineCreateInfo;
	struct BufferCreateInfo {
		VkDeviceSize size;
		VmaMemoryUsage vma_usage;
		VkBufferUsageFlags usage_flags;
	};

	// 4.6gb is good amount to reserver for 8gb vram system
	// 2gb for Windows is good practice. And a little headroom for aux apps
	struct Context {
		VulkanCore& core;
		// 
		ktxVulkanDeviceInfo ktxInfo_;

		int Initialize();
		Context(VulkanCore& _core) : core(_core) { Initialize(); };
		~Context();

		Context(const Context&) = delete;
		Context& operator=(const Context&) = delete;
		Context(Context&&) = delete;
		Context& operator=(Context&&) = delete;

//

		void AddDeferredTask(std::packaged_task<void()>&& task,  CommandSubmitHandle handle = CommandSubmitHandle{});
		void ProcessDeferrredTasks(); //void Flush();

		//
		Holder<ToyBufferHandle> CreateBuffer(BufferCreateInfo bufferCreateInfo);
		Holder<ToyBufferHandle> CreateStagingBuffer(VkDeviceSize size);
		//
		Holder<ImageHandle> CreateImage(ImageCreateInfo imageCreateInfo);
		Holder<ShaderModuleHandle> CreateShaderModule(std::vector<char> code);
		Holder<DescriptorPoolHandle> CreateDescriptorPool(); // just hardcoded defaults for pool. 
		Holder<DescriptorSetHandle> CreateDescriptorSet(DescriptorPoolHandle pool, VkDescriptorSetLayout& layout);
		//
		void UploadDataToBuffer(ToyBufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAcces);
		void UploadDataToBuffer(ToyBufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAccess, size_t src_offset, size_t dest_offset);
		void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
		void TransitionImageLayout(ImageHandle image_handle, VkImageLayout oldLayout, VkImageLayout newLayout);
		//
		void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset);
		//
		void CleanUp();
		//
		void GetKtxDeviceInfo() {
			ktxInfo_ = *ktxVulkanDeviceInfo_Create(
				core.physicalDevice_,
				core.device_,
				/*core.GetTransferQueue()*/ VK_NULL_HANDLE,
				// this maybe should be transfer queue. we only use ktx to upload tex.
				// no blitting for mipmaps, which are generated offline
				// runtime genreation for mipmaps in ktx seems to be not up to par with SOA
				core.defaultCommandPool_,
				NULL
			);
		}

		//private:
		Pool<Image, Image> images_;
		Pool<Buffer, Buffer> buffers_;
		Pool<Sampler, VkSampler> samplers_;
		Pool<ShaderModule, VkShaderModule> shaderModules_;
		Pool<DescriptorPool, VkDescriptorPool> descriptorPools_;
		//
		VmaAllocator m_allocator_;
		VmaPool m_imagesPool;
		VmaPool m_stagingPool;
		std::vector<DeferredTask> m_deferredTasks_;
		//
		TextureHandle m_nilTexture_{};
		SamplerHandle m_defaultSampler_{};
	};

	void destroy(Context* ctx, ImageHandle handle);
	void destroy(Context* ctx, ToyBufferHandle handle);
	void destroy(Context* ctx, ShaderModuleHandle handle);
	void destroy(Context* ctx, DescriptorPoolHandle handle);

	struct PipelineCache {
		std::map<PipelineKey, Holder<PipelineHandle>> pipelines_;
	};

	struct ShaderProgram {
		Context& ctx_;
		ShaderProgram(Context& ctx) : ctx_(ctx) {
			//dps_.push_back(Holder(&ctx_, ctx_.CreateDescriptorPool()));
		};
		~ShaderProgram() { dps_.clear(); sets_.clear(); };
		// 
		//Holder<ShaderModuleHandle> vert_{};
		//Holder<ShaderModuleHandle> frag_{};
		ShaderModuleHandle vert_{};
		ShaderModuleHandle frag_{};

		std::vector<DescriptorPoolHandle> dps_;
		std::vector<DescriptorSetHandle> sets_;

		// Pass will give us this
		PipelineCreateInfo::RasterSettings rasterSettings_{};
		PipelineCreateInfo::PipelineLayoutDesc pipelineLayoutDescription_{};
		// Graph will give us this
		VkPipelineRenderingCreateInfo dynamicRenderingInfo_;
		//std::map<PipelineCreateInfo, Holder<PipelineHandle>> pipelineCache_;

	public:

		ShaderProgram& setRasterSettings(PipelineCreateInfo::RasterSettings rasterSettings) { this->rasterSettings_ = rasterSettings; };
		ShaderProgram& setDynamicRenderingInfo(VkPipelineRenderingCreateInfo dynamicRenderingInfo) { this->dynamicRenderingInfo_ = dynamicRenderingInfo; };
		ShaderProgram& setLayoutDescription();
		ShaderProgram& setVert(ShaderModuleHandle vert) { this->vert_ = Holder<ShaderModuleHandle>(&ctx_, vert); return *this; };
		ShaderProgram& setFrag(ShaderModuleHandle frag) { this->frag_ = Holder<ShaderModuleHandle>(&ctx_, frag); return *this; };

		VkPipeline Build() {
			PipelineCreateInfo info;
			info.rasterSettings = rasterSettings_;
			//info.vert = vert_;
			//info.frag = frag_;
			info.layoutDescription;
		};

	private:
		void CreatePipelineLayout(const PipelineCreateInfo::PipelineLayoutDesc& descIn, VkPipelineLayout& out);
		void CreateDescriptorSetLayout(const PipelineCreateInfo::DescriptorSetDescription& descIn, VkDescriptorSetLayout& layout);
	};

};
