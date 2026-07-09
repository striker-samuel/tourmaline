#include <Engine/Vulkan/core.h>
#include <Engine/Renderer/vertex.h>

static inline bool is_aligned4(uint32_t v) { return (v & 3u) == 0u; }

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
};
VkCommandPool smvk::VulkanCore::GetCommandPool(smvk::QueuePreference preferredQueue)
{
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
};
VkFormat smvk::VulkanCore::GetDepthFormat() { return m_depthFormat; };
int smvk::Context::Initialize(
) {
	VmaAllocatorCreateInfo allocatorCreateInfo = {};
	allocatorCreateInfo.physicalDevice = core_.physicalDevice_;
	allocatorCreateInfo.device = core_.device_;
	allocatorCreateInfo.instance = core_.instance_;
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

	images_.objects_.reserve(MAX_IMAGES);
	buffers_.objects_.reserve(MAX_BUFFERS);
	//descriptorSets_.objects_.reserve(MAX_DESCRIPTORS);
	// default objs
	// from ChatGpt, may not be true: you can set a generic sampler with a big maxLod like 100, and reuse it for all your textures of any mip count.
	// The driver will clamp dynamically per texture.
	//samplers["default"] = CreateSampler(100);
	//samplers["default_shadow"] = CreateShadowSampler(1);
	m_nilTexture = CreateTexture("");
	m_defaultSampler = CreateSampler(SamplerCreateInfo());

	return 0; // no error

}
inline smvk::Context::Context(VulkanCore& core) :core_(core), m_submitter(core, QueuePreference::GRAPHICS) { Initialize(); };

smvk::Context::~Context() { CleanUp(); };

[[nodiscard]] smvk::Holder<smvk::BufferHandle> smvk::Context::CreateBuffer(
	BufferCreateInfo bufferCreateInfo
) {
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

	// getDeviceAddress
	vmaGetAllocationMemoryProperties(m_allocator, desc.alloc, &desc.memory_flags);
	desc.usage = bufferCreateInfo.usage_flags;
	return smvk::Holder<smvk::BufferHandle>(this, buffers_.create(std::move(desc)));
}

void smvk::Context::UploadDataToBuffer(
	BufferHandle buffer,
	const void* data,
	size_t dataSize,
	VkPipelineStageFlags dstStages,
	VkAccessFlags dstAccess,
	size_t src_offset,
	size_t dest_offset
) {
	Buffer* bufferDesc = buffers_.get(buffer);

	// Since Im not subclassing/specializing buffers I retired this.
	//const VkAccessFlags dstAccess =
	//	(bufferDesc->role & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? VK_ACCESS_UNIFORM_READ_BIT
	//	: VK_ACCESS_SHADER_READ_BIT;

	//VkCommandBuffer tmpBuf = beginSingleTimeCommands(core.device_, core.defaultCommandPool_);

	if (bufferDesc->is_mapable()) {
		// Direct copy into mapped allocation
		VkResult result = vmaCopyMemoryToAllocation(m_allocator_, data, bufferDesc->alloc, 0, dataSize);
		assert(result == VK_SUCCESS);

		VkBufferMemoryBarrier b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
		b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		b.dstAccessMask = dstAccess;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.buffer = bufferDesc->buffer;
		b.offset = 0;
		b.size = VK_WHOLE_SIZE;

		vkCmdPipelineBarrier(
			tmpBuf,
			VK_PIPELINE_STAGE_HOST_BIT,
			dstStages,
			0,
			0, nullptr,
			1, &b,
			0, nullptr);

		endSingleTimeCommands(core.device_, tmpBuf, core.graphicsQueue_, core.defaultCommandPool_);
		return;
	}

	// Device-local path: stage the caller's data
	VkBuffer stagingBuf; VmaAllocation stagingAlloc;
	VkBufferCreateInfo sInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = dataSize,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VmaAllocationCreateInfo sAlloc{};
	sAlloc.usage = VMA_MEMORY_USAGE_AUTO;
	sAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	sAlloc.pool = m_stagingPool;

	VK_CHECK(vmaCreateBuffer(m_allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr));

	// Fill the staging buffer with the *provided* data
	VkResult result = vmaCopyMemoryToAllocation(m_allocator_, data, stagingAlloc, 0, dataSize);
	assert(result == VK_SUCCESS);

	// Copy staging -> dst
	VkBufferCopy copyRegion{
		.srcOffset = src_offset,
		.dstOffset = dest_offset,
		.size = dataSize };
	vkCmdCopyBuffer(tmpBuf, stagingBuf, bufferDesc->buffer, 1, &copyRegion);

	// Make the copy visible to shaders
	VkBufferMemoryBarrier b2{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
	b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b2.dstAccessMask = dstAccess;
	b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b2.buffer = bufferDesc->buffer;
	//b2.offset = 0;
	//b2.size = VK_WHOLE_SIZE;
	b2.offset = dest_offset;
	b2.size = dataSize;

	vkCmdPipelineBarrier(
		tmpBuf,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		dstStages,
		0,
		0, nullptr,
		1, &b2,
		0, nullptr)
		;

	endSingleTimeCommands(core.device_, tmpBuf, core.graphicsQueue_, core.defaultCommandPool_);

	// we could maybe delay this
	vmaDestroyBuffer(m_allocator_, stagingBuf, stagingAlloc);
}

[[nodiscard]] smvk::Holder<smvk::DescriptorPoolHandle> smvk::Context::CreateDescriptorPool() {
	const uint32_t total_sampled_images = MAX_FRAMES_IN_FLIGHT * MAX_TEXTURES;
	const uint32_t total_samplers = MAX_FRAMES_IN_FLIGHT * MAX_TEXTURES;
	std::array<VkDescriptorPoolSize, 4> poolSizes = { {
			//{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,   0 }, //deviceAddres mapping.
			//{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   0 },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    total_sampled_images      },
			{ VK_DESCRIPTOR_TYPE_SAMPLER,          total_samplers            },
		  } };

	VkDescriptorPoolCreateInfo poolInfo{ };
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1000;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT; // required
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();

	VkDescriptorPool dpool;
	VK_CHECK(vkCreateDescriptorPool(core.device_, &poolInfo, nullptr, &dpool));
	return smvk::Holder(this, descriptorPools_.create(std::move(dpool)));
}

void smvk::ShaderProgram::CreatePipelineLayout(const PipelineCreateInfo::PipelineLayoutDesc& descIn, VkPipelineLayout& out)
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
	vkGetPhysicalDeviceProperties(ctx_.core_.physicalDevice_, &properties);
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
	VK_CHECK(vkCreatePipelineLayout(ctx_.core_.device_, &ci, nullptr, &out));
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

void smvk::Context::TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout
) {

	auto cmd = m_submitter.acquire();
	AddDeferredTask(
		[]{},
		handle
	);
	VkCommandBuffer commandBuffer = beginSingleTimeCommands(core.device_, core.defaultCommandPool_);

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

		if (hasStencilComponent(format)) {
			barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	}
	else {
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}

	VkPipelineStageFlags sourceStage;
	VkPipelineStageFlags destinationStage;

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
		newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
		newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
		newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {

		// Swapchain depth
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
		newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {

		// ShadowMaps
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}

	else {

		assert(false); // This so i have easy acces to call stack.
		//throw std::invalid_argument("unsupported layout transition!");
	}

	vkCmdPipelineBarrier(
		commandBuffer,
		sourceStage,
		destinationStage,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&barrier
	);

	endSingleTimeCommands(
		core.device_,
		commandBuffer,
		core.graphicsQueue_,
		core.defaultCommandPool_
	);
}
//
[[nodiscard]] smvk::Holder<smvk::ImageHandle> smvk::Context::CreateImage(ImageCreateInfo _imageCreateInfo)
{
	Image image = {};

	//VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT // sample as texture
	//	| VK_IMAGE_USAGE_TRANSFER_DST_BIT; // upload via copy // I dont need this for depth
	//VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT

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

	// Laout is transient. and depents on stage. 
	//	image.layout = VK_IMAGE_LAYOUT_UNDEFINED;

	image.format = _imageCreateInfo.format;
	image.levels = _imageCreateInfo.levels;
	image.extent.width = _imageCreateInfo.texWidth;
	image.extent.height = _imageCreateInfo.texHeight;
	image.usage = _imageCreateInfo.usage;
	image.arrayLayers = _imageCreateInfo.arrayLayers;

	//VmaAllocation alloc = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo{};

	VkResult res = vmaCreateImage(m_allocator, &imgCI, &allocation_create_info, &image.image, &image.alloc, &allocInfo);
	if (res != VK_SUCCESS) {
		throw std::runtime_error("vmaCreateImage (in pool) failed");
	}

	return smvk::Holder<ImageHandle>(this, images_.create(std::move(image)));

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
[[nodiscard]] smvk::Holder<smvk::BufferHandle> smvk::Context::CreateStagingBuffer(const void* srcData, VkDeviceSize size)
{
	auto handle = CreateStagingBuffer(size);
	auto* buf = buffers_.get(handle);
	void* mapped = nullptr;
	VK_CHECK(vmaMapMemory(m_allocator, buf->alloc, &mapped));
	std::memcpy(mapped, srcData, static_cast<size_t>(size));
	// HOST_COHERENT means we don't need vmaFlushAllocation.
	// If you drop HOST_COHERENT in the future, call vmaFlushAllocation here.
	vmaUnmapMemory(m_allocator, buf->alloc);
	return handle;
}
[[nodiscard]] smvk::Holder<smvk::ShaderModuleHandle> smvk::Context::CreateShaderModule(std::vector<char> code) {
	VkShaderModule shader;
	core.CreateShaderModule(
		std::move(code),
		shader
	);
	return smvk::Holder(this, shaderModules_.create(std::move(shader)));
};
//
void destroy(smvk::Context* ctx, smvk::ShaderModuleHandle handle) {
	assert(handle.valid());
	auto sm = ctx->shaderModules_.get(handle);
	vkDestroyShaderModule(ctx->core.device_, *sm, NULL);
	ctx->shaderModules_.destroy(handle);
}
void destroy(smvk::Context* ctx, smvk::DescriptorPoolHandle handle) {
	assert(handle.valid());
	vkDeviceWaitIdle(ctx->core.device_);
	auto obj_ = ctx->descriptorPools_.get(handle);
	vkDestroyDescriptorPool(ctx->core.device_, *obj_, nullptr);
}
void destroy(smvk::Context* ctx, smvk::DescriptorSetHandle handle) {
	//auto& core = ctx->core
	//assert(handle.valid());
	//vkDeviceWaitIdle(ctx->core.device_);
	//auto obj_ = ctx->descriptorSets_.get(handle);
	//vkFreeDescriptorSets(
	//	core.device_,
	//)
	//vkDescrip(ctx->core.device_, *obj_, nullptr);
}
void destroy(smvk::Context* ctx, smvk::ImageHandle handle)
{

	if (!handle.valid()) return;

	auto device_ = ctx->core.device_;
	smvk::Image& image = *ctx->images_.get(handle);

	if (image.renderView != VK_NULL_HANDLE) {
		vkDestroyImageView(device_, image.renderView, nullptr);
		image.renderView = VK_NULL_HANDLE;
	}

	if (image.sampleView != VK_NULL_HANDLE) {
		vkDestroyImageView(device_, image.sampleView, nullptr);
		image.sampleView = VK_NULL_HANDLE;
	}
	for (auto& view : image.arrayView) {
		vkDestroyImageView(device_, image.sampleView, nullptr);
		view = VK_NULL_HANDLE;
	}
	//vkDestroySampler(device_, image.sampler, nullptr); // not owning!
	if (image.image != VK_NULL_HANDLE) {
		vmaDestroyImage(ctx->m_allocator, image.image, image.alloc);
		image.image = VK_NULL_HANDLE;
		image.alloc = nullptr;
	}

	ctx->images_.destroy(handle);
}
void destroy(smvk::Context* ctx, smvk::BufferHandle handle) {
	assert(handle.valid());
	auto db = ctx->buffers_.get(handle);
	vmaDestroyBuffer(ctx->m_allocator, db->buffer, db->alloc);
	ctx->buffers_.destroy(handle);
}
//
void smvk::Context::AddDeferredTask(std::packaged_task<void()>&& task, smvk::CommandSubmitHandle handle)
{
	if (!handle.valid()) {
		handle = submitter_.acquire();
	}
	m_deferredTasks.emplace_back(std::move(task), handle);
}
void smvk::Context::ProcessDeferrredTasks()
{
	std::vector<DeferredTask>::iterator it = m_deferredTasks.begin();
	while (it != m_deferredTasks.end() && submitter_.isReady(it->handle_)) {
		(it++)->task_();
	}
	m_deferredTasks.erase(m_deferredTasks.begin(), it);
}
void smvk::Context::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset) {

	//AddDeferredTask()

	VkCommandBuffer commandBuffer = beginSingleTimeCommands(core.device_, core.defaultCommandPool_);
	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	copyRegion.srcOffset = srcOffset;
	copyRegion.dstOffset = dstOffset;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
	endSingleTimeCommands(core.device_, commandBuffer, core.graphicsQueue_, core.defaultCommandPool_);
}
void smvk::Context::CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler) {

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;

	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(core.physicalDevice_, &properties);

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

	VK_CHECK(vkCreateSampler(core.device_, &samplerInfo, nullptr, &sampler));

}
// This should be moved to core.
void smvk::Context::CreateImageView(
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	int levelCount,
	int baseArrayLayer,
	int layerCount,
	VkImageViewType viewType,
	VkImageView& view
)
{
	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = viewType;
	viewInfo.format = format;

	viewInfo.subresourceRange.aspectMask = aspectFlags;
	viewInfo.subresourceRange.baseMipLevel = 0;
	//viewInfo.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	// VK_REMAINING_MIP_LEVELS is a special constant value used for image views to indicate that all remaining mipmap levels in an image after the base level should be included in the view.
	viewInfo.subresourceRange.levelCount = levelCount;
	viewInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
	viewInfo.subresourceRange.layerCount = layerCount;

	// Lets default this like that ourselves.
	viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

	VK_CHECK(vkCreateImageView(core_.device_, &viewInfo, nullptr, &view));
}
void smvk::Context::CleanUp() {
	VK_CHECK(vkDeviceWaitIdle(core.device_));
	vmaDestroyPool(m_allocator, m_imagesPool);
	vmaDestroyPool(m_allocator, m_stagingPool);
}
//
void smvk::ShaderProgram::CreateDescriptorSetLayout(const smvk::PipelineCreateInfo::DescriptorSetDescription& descIn, VkDescriptorSetLayout& layout)
{
	PipelineCreateInfo::DescriptorSetDescription desc = descIn;

	std::vector<VkDescriptorSetLayoutBinding> vkBindings;
	vkBindings.reserve(desc.bindings.size());

	// IMPORTANT: pImmutableSamplers must point to memory that lives until vkCreateDescriptorSetLayout returns.
	// We can keep a local vector-of-vectors of VkSampler pointers to stable storage.
	std::vector<std::vector<VkSampler>> immutableSamplersStorage;
	immutableSamplersStorage.reserve(desc.bindings.size());

	std::vector<VkDescriptorBindingFlags> bindingFlags;
	bindingFlags.reserve(desc.bindings.size());

	for (const PipelineCreateInfo::BindingDescription& b : desc.bindings)
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

	VK_CHECK(vkCreateDescriptorSetLayout(ctx_.core.device_, &ci, nullptr, &layout));
}
