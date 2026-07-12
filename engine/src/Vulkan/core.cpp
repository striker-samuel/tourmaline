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

};

namespace {
	static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);
	[[nodiscard]] std::vector<const char*> getRequiredExtensionsForGlfw();
	[[nodiscard]] smvk::SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);
	[[nodiscard]] smvk::QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface);
	bool checkDeviceExtensionSupport(VkPhysicalDevice device, const std::vector<const char*> deviceExtensions);
	bool checkValidationLayerSupport(const std::vector<const char*> validationLayers);
	bool isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface);
	VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
	VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
	VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window);
	VkFormat findSupportedFormat(VkPhysicalDevice physicalDevice, const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
	VkFormat findDepthFormat(VkPhysicalDevice physicalDevice);

};
//
static VkInstance instance_ = VK_NULL_HANDLE;
static VkDevice device_ = VK_NULL_HANDLE;
static VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
//
static VkQueue graphicsQueue_ = VK_NULL_HANDLE;
static VkQueue presentQueue_ = VK_NULL_HANDLE;
static VkQueue transferQueue_ = VK_NULL_HANDLE;
//
static VkCommandPool defaultCommandPool_ = VK_NULL_HANDLE;
static VkCommandPool transferCommandPool_ = VK_NULL_HANDLE;
//
static inline bool is_aligned4(uint32_t v) { return (v & 3u) == 0u; }
//
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
bool smvk::VulkanCore::Init(VkSurfaceKHR* surface) {
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
			if (isDeviceSuitable(device, m_surface)) {
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
			QueueFamilyIndices indices = findQueueFamilies(physicalDevice_, m_surface);
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
void smvk::VulkanCore::CreateFence(const VkFenceCreateInfo createInfo, VkFence& out) {
	VK_CHECK(vkCreateFence(device_, &createInfo, nullptr, &out));
};
void smvk::VulkanCore::CreateSemaphore(VkSemaphoreCreateInfo createInfo, VkSemaphore& out) {
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
inline void smvk::VulkanCore::CreateCommandPool(QueuePreference queue, VkCommandPoolCreateFlags flags, VkCommandPool& out) {
	QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice_, surface_);
	VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value(),
	};

	for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
		if (vkCreateCommandPool(vk_core.device_, &poolInfo, nullptr, &perFrameCommandPools[fn]) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create graphics command pool!");
		}
	}
}
inline VkInstance& smvk::VulkanCore::GetInstance() { return instance_; };
VkFormat smvk::VulkanCore::GetDepthFormat() { return m_depthFormat; };
int smvk::Context::Initialize(
) {
	VmaAllocatorCreateInfo allocatorCreateInfo = {};
	allocatorCreateInfo.physicalDevice = physicalDevice_;
	allocatorCreateInfo.device = device_;
	allocatorCreateInfo.instance = instance_;
	allocatorCreateInfo.vulkanApiVersion = core_.config_.version_ == VK_1_3 ? VK_API_VERSION_1_3 : UINT32_MAX;
	allocatorCreateInfo.flags =
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
	// VMA_ALLOCATOR_CREATE_KHR_EXTERNAL_MEMORY_WIN32_BIT // We wont use this. Just keeeping this here to remember it exists.
	if (allocatorCreateInfo.vulkanApiVersion == UINT32_MAX) { assert(false); };

	VmaVulkanFunctions vulkanFunctions;
	VK_CHECK(vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vulkanFunctions));
	allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;
	VK_CHECK(vmaCreateAllocator(&allocatorCreateInfo, &m_allocator));

	m_nilTexture = CreateTexture("");
	m_defaultSampler = core_.CreateSampler(SamplerCreateInfo());

	return 0; // no error

}
inline smvk::Context::Context(VulkanCore& core) :core_(core), m_submitter(core, QueuePreference::GRAPHICS) { Initialize(); }
inline smvk::Context::~Context() { CleanUp(); };
void smvk::Context::UploadDataToBuffer(
	BufferHandle buffer,
	const void* data,
	size_t dataSize,
	VkPipelineStageFlags dstStages,
	VkAccessFlags dstAccess,
	size_t src_offset,
	size_t dest_offset
) {
	Buffer* buf = buffers_.get(buffer);
	auto han = m_submitter.acquire();
	auto cmd = m_submitter.GetCommandBuffer(han).cmd;

	if (buf->is_mapable()) {
		VK_CHECK(vmaCopyMemoryToAllocation(m_allocator, data, buf->alloc, 0, dataSize));
		BufferBarrier(
			cmd,
			buf->buffer,
			dest_offset,
			dataSize,
			VK_PIPELINE_STAGE_2_HOST_BIT,
			VK_ACCESS_2_HOST_WRITE_BIT,
			dstStages,
			dstAccess
		);
		return;
	}

	VkBuffer stagingBuf;
	VmaAllocation stagingAlloc;

	VkBufferCreateInfo sInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = dataSize,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VmaAllocationCreateInfo sAlloc{};
	sAlloc.usage = VMA_MEMORY_USAGE_AUTO;
	sAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	sAlloc.pool = m_stagingPool;

	// Fill the staging buffer with the *provided* data
	VK_CHECK(vmaCreateBuffer(m_allocator, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr));
	VK_CHECK(vmaCopyMemoryToAllocation(m_allocator, data, stagingAlloc, 0, dataSize));

	// Copy staging -> dst
	VkBufferCopy copyRegion{
		.srcOffset = 0,	// we have the staging buf created for this so just from 0.
		.dstOffset = dest_offset,
		.size = dataSize
	};
	vkCmdCopyBuffer(cmd, stagingBuf, buf->buffer, 1, &copyRegion);

	BufferBarrier(
		cmd,
		buf->buffer,
		dest_offset,
		dataSize,
		VK_PIPELINE_STAGE_2_HOST_BIT,
		VK_ACCESS_2_HOST_WRITE_BIT,
		dstStages,
		dstAccess
	);

	AddDeferredTask(
		std::packaged_task<void()>([this, stagingBuf, stagingAlloc] {vmaDestroyBuffer(m_allocator, stagingBuf, stagingAlloc); })
		, han
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
//	[[nodiscard]] Pipeline ShaderProgram::CreateGraphicsPipeline(
//		PipelineCreateInfo pipelineCreateInfo
//	) {
//
//		Pipeline pipeline;
//		auto v = ctx_.shaderModules_.get(pipelineCreateInfo.vert);
//		auto f = ctx_.shaderModules_.get(pipelineCreateInfo.frag);
//
//		// SHADER
//		VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
//		vertShaderStageInfo.sType =
//			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
//		vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
//		vertShaderStageInfo.module = *v;
//		vertShaderStageInfo.pName = "mainVS";
//
//		VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
//		fragShaderStageInfo.sType =
//			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
//		fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
//		fragShaderStageInfo.module = *f;
//		fragShaderStageInfo.pName = "mainPS";
//
//		VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo,
//														  fragShaderStageInfo };
//
//		VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
//		vertexInputInfo.sType =
//			VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
//		auto bindingDescription = Vertex::getBindingDescription();
//		auto attributeDescriptions = Vertex::getAttributeDescriptions();
//		vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescription.size());
//		vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
//		vertexInputInfo.pVertexBindingDescriptions = bindingDescription.data();
//		vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
//
//		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
//		inputAssembly.sType =
//			VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
//		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
//		inputAssembly.primitiveRestartEnable = VK_FALSE;
//
//		// VIEWPORT
//		VkPipelineViewportStateCreateInfo viewportState{};
//		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
//		viewportState.viewportCount = 1;
//		viewportState.scissorCount = 1;
//
//		// RASTERIZER
//		VkPipelineRasterizationStateCreateInfo rasterizer{};
//		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
//		rasterizer.depthClampEnable = pipelineCreateInfo.rasterSettings.depthClampEnable;
//		rasterizer.rasterizerDiscardEnable = pipelineCreateInfo.rasterSettings.rasterizerDiscardEnable;
//		rasterizer.polygonMode = pipelineCreateInfo.rasterSettings.polygonMode;
//		rasterizer.lineWidth = pipelineCreateInfo.rasterSettings.lineWidth;
//		rasterizer.cullMode = pipelineCreateInfo.rasterSettings.cullMode;
//		rasterizer.frontFace = pipelineCreateInfo.rasterSettings.frontFace;
//		rasterizer.depthBiasEnable = pipelineCreateInfo.rasterSettings.depthBiasEnable;
//
//		// MULTISAMPLING
//		VkPipelineMultisampleStateCreateInfo multisampling{};
//		multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
//		multisampling.sampleShadingEnable = VK_FALSE;
//		multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
//
//		// STENCIL
//		VkPipelineDepthStencilStateCreateInfo depthStencil{};
//		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
//		depthStencil.depthTestEnable = VK_TRUE;
//		depthStencil.depthWriteEnable = VK_TRUE;
//		depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER;
//		depthStencil.depthBoundsTestEnable = VK_FALSE;
//		depthStencil.stencilTestEnable = VK_FALSE;
//
//		// COLORBLEND
//		VkPipelineColorBlendAttachmentState colorBlendAttachment{};
//		colorBlendAttachment.colorWriteMask =
//			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
//			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
//		colorBlendAttachment.blendEnable = VK_FALSE;
//
//		VkPipelineColorBlendStateCreateInfo colorBlending{};
//		colorBlending.sType =
//			VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
//		colorBlending.logicOpEnable = VK_FALSE;
//		colorBlending.logicOp = VK_LOGIC_OP_COPY;
//		colorBlending.attachmentCount = 1;
//		colorBlending.pAttachments = &colorBlendAttachment;
//		colorBlending.blendConstants[0] = 0.0f;
//		colorBlending.blendConstants[1] = 0.0f;
//		colorBlending.blendConstants[2] = 0.0f;
//		colorBlending.blendConstants[3] = 0.0f;
//
//		// DYNAMIC STATES
//		// These two are elemental. There are extensions for almost anything do. Maybe it is woth to go all in dynamic state.
//		std::vector<VkDynamicState> dynamicStates = {
//			VK_DYNAMIC_STATE_VIEWPORT,
//			VK_DYNAMIC_STATE_SCISSOR
//		};
//
//		VkPipelineDynamicStateCreateInfo dynamicState{};
//		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
//		dynamicState.dynamicStateCount =
//			static_cast<uint32_t>(dynamicStates.size());
//		dynamicState.pDynamicStates = dynamicStates.data();
//
//#pragma region ---[LayoutDescription]---
//		VkPipelineLayout lyt;
//		CreatePipelineLayout(pipelineCreateInfo.layoutDescription, lyt);
//
//#pragma endregion
//
//		VkGraphicsPipelineCreateInfo pipelineInfo{};
//		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
//		pipelineInfo.stageCount = 2;
//		pipelineInfo.pStages = shaderStages;
//		pipelineInfo.pVertexInputState = &vertexInputInfo;
//		pipelineInfo.pInputAssemblyState = &inputAssembly;
//		pipelineInfo.pViewportState = &viewportState;
//		pipelineInfo.pRasterizationState = &rasterizer;
//		pipelineInfo.pMultisampleState = &multisampling;
//		pipelineInfo.pColorBlendState = &colorBlending;
//		pipelineInfo.pDynamicState = &dynamicState;
//		pipelineInfo.pDepthStencilState = &depthStencil;
//		pipelineInfo.layout = pipelineLayput;
//		pipelineInfo.pNext = &dynamicRenderingInfo_;
//
//		if (vkCreateGraphicsPipelines(ctx_.core.device_, VK_NULL_HANDLE, 1, &pipelineInfo,
//			nullptr, &pipeline.pipeline) != VK_SUCCESS)
//		{
//			throw std::runtime_error("failed to create graphics pipeline!");
//		}
//
//		return ctx_.graphicPipelines_.create(std::move(pipeline));
//
//	}

[[nodiscard]] smvk::Holder<smvk::ImageHandle> smvk::Context::CreateImage(ImageCreateInfo _imageCreateInfo, void* data, size_t dataSize)
{
	Image image{};
	VkImageCreateInfo imgCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgCI.imageType = VK_IMAGE_TYPE_2D;
	imgCI.format = _imageCreateInfo.format;               // pick your format
	imgCI.extent = { static_cast<uint32_t>(_imageCreateInfo.texWidth), static_cast<uint32_t>(_imageCreateInfo.texHeight), 1 }; // width, height, depth
	imgCI.mipLevels = _imageCreateInfo.levels;                                      // >1 if you want mipmaps
	imgCI.usage = _imageCreateInfo.usage;
	imgCI.arrayLayers = _imageCreateInfo.arrayLayers;                                      // >1 for arrays/cubemaps
	imgCI.samples = VK_SAMPLE_COUNT_1_BIT;                  // MSAA? set >1
	imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;                // GPU-friendly
	// Queue exclusivity on by default.
	imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;              // we’ll transition later. Well jus use transition General always

	VmaAllocationCreateInfo allocation_create_info{};
	allocation_create_info.usage = VMA_MEMORY_USAGE_GPU_ONLY; // should be compatible with the pool’s memoryType
	allocation_create_info.pool = m_imagesPool;
	image.format = _imageCreateInfo.format;
	image.levels = _imageCreateInfo.levels;
	image.extent.width = _imageCreateInfo.texWidth;
	image.extent.height = _imageCreateInfo.texHeight;
	image.usage = _imageCreateInfo.usage;
	image.arrayLayers = _imageCreateInfo.arrayLayers;
	image.sampler_ = m_defaultSampler;
	VK_CHECK(vmaCreateImage(m_allocator, &imgCI, &allocation_create_info, &image.image, &image.alloc, nullptr));

	return smvk::Holder<ImageHandle>(this, images_.create(std::move(image)));

}
[[nodiscard]] smvk::Holder<smvk::BufferHandle> smvk::Context::CreateBuffer(BufferCreateInfo bufferCreateInfo) {
	Buffer desc{};
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = bufferCreateInfo.size;
	ci.usage = bufferCreateInfo.usage_flags;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo aci{};
	aci.usage = bufferCreateInfo.vma_usage;

	// Optionally choose pools here based on usage_flags/usage
	VK_CHECK(vmaCreateBuffer(
		m_allocator,
		&ci,
		&aci,
		&desc.buffer,
		&desc.alloc,
		nullptr
		//&desc.allocInfo
	)
	);

	const VkBufferDeviceAddressInfo ai = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = desc.buffer
	};
	desc.deviceAdress = vkGetBufferDeviceAddress(device_, &ai);
	vmaGetAllocationMemoryProperties(m_allocator, desc.alloc, &desc.memory_flags);
	desc.usage = bufferCreateInfo.usage_flags;
	return smvk::Holder<smvk::BufferHandle>(this, buffers_.create(std::move(desc)));
}
[[nodiscard]] smvk::Holder<smvk::BufferHandle> smvk::Context::CreateStagingBuffer(VkDeviceSize size)
{
	Buffer desc{};
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size;
	ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO; // modern VMA: let it pick a good type
	aci.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	aci.requiredFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VK_CHECK(vmaCreateBuffer(m_allocator, &ci, &aci, &desc.buffer, &desc.alloc, nullptr));
	// 4) Cache memory properties for quick checks/debug
	VkMemoryPropertyFlags props{};
	vmaGetAllocationMemoryProperties(m_allocator, desc.alloc, &props);
	desc.memory_flags = props;
	desc.usage = ci.usage;

	if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
		assert(false); // just hard crush for now
	}

	return smvk::Holder<smvk::BufferHandle>(this, buffers_.create(std::move(desc)));
}
[[nodiscard]] smvk::Holder<smvk::ShaderModuleHandle> smvk::Context::CreateShaderModule(std::vector<char> code) {
	VkShaderModule shader;
	core_.CreateShaderModule(
		std::move(code),
		shader
	);
	return smvk::Holder(this, shaderModules_.create(std::move(shader)));
}

//[[nodiscard]] smvk::Holder<smvk::TextureHandle> smvk::Context::CreateTexture(void* data, size_t dataSize)
//{
//	ImageCreateInfo createInfo;
//	createInfo.format = VK
//		CreateImage();
//	return Holder<TextureHandle>(this, images_.create());
//};
void destroy(smvk::Context* ctx, smvk::BufferHandle handle) {
	assert(handle.valid());
	auto db = ctx->buffers_.get(handle);
	vmaDestroyBuffer(ctx->m_allocator, db->buffer, db->alloc);
	ctx->buffers_.destroy(handle);
}
void destroy(smvk::Context* ctx, smvk::ShaderModuleHandle handle) {
	assert(handle.valid());
	auto sm = ctx->shaderModules_.get(handle);
	vkDestroyShaderModule(device_, *sm, NULL);
	ctx->shaderModules_.destroy(handle);
}
//
void smvk::Context::AddDeferredTask(std::packaged_task<void()>&& task, smvk::CommandSubmitHandle handle)
{
	if (m_submitter.validHandle(handle)) {
		handle = m_submitter.acquire();
	}
	m_deferredTasks.emplace_back(std::move(task), handle);
}
void smvk::Context::ProcessDeferrredTasks()
{
	std::vector<DeferredTask>::iterator it = m_deferredTasks.begin();
	while (it != m_deferredTasks.end() && m_submitter.isReady(it->handle_)) {
		(it++)->task_();
	}
	m_deferredTasks.erase(m_deferredTasks.begin(), it);
}
void smvk::Context::CleanUp() {
	VK_CHECK(vkDeviceWaitIdle(device_));
	vmaDestroyPool(m_allocator, m_imagesPool);
	vmaDestroyPool(m_allocator, m_stagingPool);
}
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
inline void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageSubresourceRange subresourceRange, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
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

		.image = image,
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
inline smvk::CommandBufferManager::CommandBufferManager(QueuePreference queue)
	:queue_(queue)
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

	VkSemaphoreTypeCreateInfo semaphoreTypeInfo{};
	semaphoreTypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	semaphoreTypeInfo.pNext = nullptr;
	semaphoreTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	semaphoreTypeInfo.initialValue = 0; // starting counter value

	for (int i = 0; i < kMax; i++) {
		commandBuffers_[i].allocatedCmd_ = m_buffers_[i];
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

	VkCommandPool pool = core_.GetCommandPool(queue_);
	if (pool != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(device_, pool, kMax, m_buffers_);
	}
}
[[nodiscard]] inline smvk::CommandSubmitHandle smvk::CommandBufferManager::acquire(uint32_t goal)
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
}
[[nodiscard]] inline smvk::CommandBufferManager::CommandBuffer& smvk::CommandBufferManager::GetCommandBuffer(CommandSubmitHandle handle) {
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
	vkQueueWaitIdle(queue);

	wrapper_.isEncoding_ = false;
	m_lastSubmit = handle;
}
inline bool smvk::CommandBufferManager::validHandle(CommandSubmitHandle handle) const { return handle.gen_ == commandBuffers_[handle.index_].gen_; }
inline bool smvk::CommandBufferManager::isReady(CommandSubmitHandle handle) { return true; } // TODO:: needs implementation
inline void smvk::CommandBufferManager::purge() {
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
			device_,
			&waitInfo,
			UINT64_MAX
		);
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
	vkWaitSemaphores(device_,
		&waitInfo,
		timeout
	);
}
inline void smvk::CommandBufferManager::waitAll() {
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
		device_,
		&waitInfo,
		UINT64_MAX
	);
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
void smvk::CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler) {

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
void smvk::CreateImageView(ImageViewCreateInfo imageViewCreateInfo, VkImageView& view)
{
	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = imageViewCreateInfo.image;
	viewInfo.viewType = imageViewCreateInfo.viewType;
	viewInfo.format = imageViewCreateInfo.format;
	viewInfo.subresourceRange = imageViewCreateInfo.subResourceRange;
	//.aspectMask = imageViewCreateInfo.aspectFlags,
	//.baseMipLevel = 0,
	////.levelCount = VK_REMAINING_MIP_LEVELS,
	//.levelCount = imageViewCreateInfo.levelCount,
	//.baseArrayLayer = imageViewCreatInfo.baseArrayLayer,
	//.layerCount = imageViewCreateInfo.layerCount,
// Lets default this like that ourselves.
	viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

	VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &view));
}
// 
inline smvk::VulkanSwapchain::VulkanSwapchain(Context& context, GLFWwindow* window, uint32_t framesInFlight, uint32_t desiredImageCount)
	: ctx_(context),
	window_(window),
	framesInFlight_(framesInFlight),
	desiredImageCount_(desiredImageCount) {

	Create();
}
inline smvk::VulkanSwapchain::~VulkanSwapchain() {
	DestroySyncObjects();
	CleanupSwapchain();
}
inline void smvk::VulkanSwapchain::Recreate() {
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
inline void smvk::VulkanSwapchain::CreateSwapchain() {

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
	VkResult result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);
	if (result != VK_SUCCESS) {
		throw std::runtime_error("Failed to create Vulkan swapchain.");
	}
	uint32_t actualImageCount = 0;
	vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, nullptr);
	images_.resize(actualImageCount);
	vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, images_.data());
	imageViews_.resize(actualImageCount);
	renderFinishedSemaphores_.resize(actualImageCount, VK_NULL_HANDLE);
	surfaceFormat_ = surfaceFormat.format;
	extent_ = swapExtent;
	glfwGetWindowSize(window_, &windowWidth, &windowHeight);
}
inline void smvk::VulkanSwapchain::CreateImageViews() {
	for (size_t i = 0; i < images_.size(); ++i) {
		auto viewCreateInfo = ImageViewCreateInfo{
			.flags = VK_IMAGE_ASPECT_COLOR_BIT,
			.image = images_[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = surfaceFormat_,
			.subResourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};
		CreateImageView(
			viewCreateInfo,
			imageViews_[i]
		);
	}
}
inline void smvk::VulkanSwapchain::CreateDepthResources() {
	depthImage_ = ctx_.CreateImage(
		ImageCreateInfo{
			.texWidth = static_cast<int>(extent_.width),
			.texHeight = static_cast<int>(extent_.height),
			.format = depthFormat_,
			.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			.levels = 1,
			.arrayLayers = 0,
		}
		);

	auto obj_ = ctx_.images_.get(depthImage_);

	auto viewCreateInfo =
		ImageViewCreateInfo{
		.image = obj_->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = obj_->format,
		.subResourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
				.baseMipLevel = 1,
				//.levelCount,
	},
	};

	CreateImageView(
		viewCreateInfo,
		obj_->renderView
	);
	obj_->sampleView = obj_->renderView;

	auto wrapper = ctx_.m_submitter.GetCommandBuffer(ctx_.m_submitter.acquire());
	ImageBarrier(
		wrapper.cmd,
		obj_->image,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	);
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

	if (swapchain_ != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(device_, swapchain_, nullptr);
		swapchain_ = VK_NULL_HANDLE;
	}
}
//
// TODO :: Remove this. leaking implementation.
VkQueue smvk::GetGraphicsQueue() { return graphicsQueue_; };
VkQueue smvk::GetPresentQueue() { return presentQueue_; };
