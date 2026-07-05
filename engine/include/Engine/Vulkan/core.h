#pragma once

#include <cassert>
#include <optional>
#include <vector>
#include <set>

#include "volk.h"
#include "Engine/Shadred/common.h"

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

bool checkValidationLayerSupport(const std::vector<const char*> validationLayers);
bool hasStencilComponent(VkFormat format);
VkFormat findSupportedFormat(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
VkFormat findDepthFormat(VkPhysicalDevice physicalDevice);
uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);
VkShaderModule createDeviceShaderModule(VkDevice device, const std::vector<char>& code);
SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);
QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface);
bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface);
void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);

bool checkDeviceExtensionSupport(VkPhysicalDevice device);
std::vector<const char*> getRequiredExtensions();

VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window);


namespace SamToy {

	enum VulkanVersion {
		VK_1_3
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

			VkInstanceCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			createInfo.pApplicationInfo = &appInfo;

			auto extensions = getRequiredExtensions();
			createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
			createInfo.ppEnabledExtensionNames = extensions.data();

			if (enableValidationLayers) {
				createInfo.enabledLayerCount = static_cast<uint32_t>(config.validationLayers.size());
				createInfo.ppEnabledLayerNames = config.validationLayers.data();
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
															  indices.presentFamily.value()
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
					createInfo.enabledExtensionCount = static_cast<uint32_t>(config.deviceExtensions.size());
					createInfo.ppEnabledExtensionNames = config.deviceExtensions.data();

					if (enableValidationLayers) {
						createInfo.enabledLayerCount = static_cast<uint32_t>(config.validationLayers.size());
						createInfo.ppEnabledLayerNames = config.validationLayers.data();
					}
					else {
						createInfo.enabledLayerCount = 0;
					}

					if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
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
				VkCommandPoolCreateInfo poolInfo
				{
					.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					.queueFamilyIndex = indices.graphicsFamily.value()
				};

				vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
				// transferqueue will demote to graphics if no dedicated transfer queue is exposed.
				vkGetDeviceQueue(device_, indices.transferFamily.value(), 0, &transferQueue_);

				for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
					if (vkCreateCommandPool(device_, &poolInfo, nullptr, &defaultCommandPool_) !=
						VK_SUCCESS) {
						throw std::runtime_error("failed to create graphics command pool!");
					}
				}
			};

		};

		void CreateFence(const VkFenceCreateInfo createInfo, VkFence& out) {
			VK_CHECK(vkCreateFence(device_, &createInfo, nullptr, &out));
		};
		void CreateSemaphore(VkSemaphoreCreateInfo createInfo, VkSemaphore& out) {
			VK_CHECK(vkCreateSemaphore(device_, &createInfo, nullptr, &out));
		};
		VkFormat GetDepthFormat();
	};

	struct CommandBufferPool {
		VulkanCore& core_;

		enum struct QueuePreference {
			TRANSFER,
			GRAPHICS,
			COMPUTE
		};
		struct ToyCommandBuffer {
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			VkFence fence_ = VK_NULL_HANDLE;
			VkSemaphore waitSemaphore_ = VK_NULL_HANDLE;
			VkSemaphore signalSemaphore_ = VK_NULL_HANDLE;
			bool isEncoding_ = false;
			QueuePreference queue_ = QueuePreference::GRAPHICS; // Dont know yet how to properly resolve this. will come with real use
		};

		//
		struct Slot {
			ToyCommandBuffer cmd{};
			uint32_t gen_ = 1;
		};
		struct CommandSubmitHandle {
			uint32_t index_ = 0;
			uint32_t gen_ = 1;
		};
		//

		CommandBufferPool(VulkanCore& core) : core_(core) {
			VkCommandBufferAllocateInfo info{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.pNext = nullptr,
				.commandPool = core_.defaultCommandPool_,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, //yes, just primary
				.commandBufferCount = kMax,
			};
			VkCommandBuffer buffers[kMax];
			VK_CHECK(vkAllocateCommandBuffers(core_.device_, &info, buffers));

			for (int i = 0; i < kMax; i++) {
				commandBuffers_[i].cmd.cmd = buffers[i];
				commandBuffers_[i].cmd.isEncoding_ = false;
				core_.CreateFence(createFenceInfo_, commandBuffers_[i].cmd.fence_);
				core_.CreateSemaphore(createSemaphoreInfo_, commandBuffers_[i].cmd.semaphore_);
			}
		};
		~CommandBufferPool() {};
		bool isReady(CommandSubmitHandle handle) { return true; };

		ToyCommandBuffer GetCommandBuffer(CommandSubmitHandle handle) {
			assert(handle.gen_ == commandBuffers_[handle.index_]);
			return commandBuffers_[handle.index_].cmd;
		};

		CommandSubmitHandle acquire() {
			ToyCommandBuffer wrapper = {
				.cmd = cmd,
				.fence_ = VK_NULL_HANDLE,
				.semaphore_ = VK_NULL_HANDLE,
				.isEncoding_ = true
			};
			auto gen = commandBuffers_[].gen_;
			commandBuffers_[nextFree] = { .cmd = wrapper, .gen_ = gen };
		};

		void submit(CommandSubmitHandle handle) {
			auto& slot = commandBuffers_[handle.index_];
			vkEndCommandBuffer(slot.cmd.cmd);


			VkSubmitInfo submitInfo{};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers = &slot.cmd.cmd;
			submitInfo.waitSemaphoreCount = slot.cmd.semaphore_ ? 0 : 1;
			submitInfo.pWaitSemaphores
			submitInfo.pCommandBuffers = &slot.cmd.cmd;


			VkQueue queue = core_.GetQueueFromPref(slot.cmd.queue_);
			vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
			vkQueueWaitIdle(queue);

			//endSingleTimeCommands(
			//	core_.device_,
			//	slot.cmd.cmd,
			//	core_.graphicsQueue_,
			//	//cmd->queue_ == QueuePreference::GRAPHICS ? core_.graphicsQueue_ : core_.transferQueue_,
			//	core_.defaultCommandPool_
			//);

			// this will burn gens
			nextFree_ = handle.index_;
			slot.gen_++;
		}

		CommandBufferPool(VulkanCore& core) : core_(core) {};
		~CommandBufferPool() {};
	private:
		static constexpr uint32_t kMax = 64;
		const VkSemaphoreCreateInfo createSemaphoreInfo_{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.flags = 0, };
		const VkFenceCreateInfo createFenceInfo_ = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.flags = 0, };
		uint32_t nextFree_ = 0;
		Slot commandBuffers_[kMax];
		VulkanCore& core_;
	};

	struct DeferredCommandBuffer {
		DeferredCommandBuffer(std::packaged_task<void()>&& task, CommandBufferPool::CommandSubmitHandle handle) : task_(std::move(task)), handle_(handle) {}
		std::packaged_task<void()> task_;
		CommandBufferPool::CommandSubmitHandle handle_;
	};

};
