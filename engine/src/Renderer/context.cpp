#include <Engine/Renderer/context.h>
#include <Engine/Renderer/vertex.h>

static inline bool is_aligned4(uint32_t v) { return (v & 3u) == 0u; }

int Context::Initialize(
) {

	VmaAllocatorCreateInfo allocatorCreateInfo = {};
	allocatorCreateInfo.physicalDevice = core.physicalDevice_;
	allocatorCreateInfo.device = core.device_;
	allocatorCreateInfo.instance = core.instance_;

	// Hardcodede to 1.3 atm
	allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
	//allocatorCreateInfo.vulkanApiVersion = core.API_VERSION;

	allocatorCreateInfo.flags =
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
	// This next flag is to get handles, to interporate resources between instances, or even between Vulkan and Dx or things like that.
	// VMA_ALLOCATOR_CREATE_KHR_EXTERNAL_MEMORY_WIN32_BIT

	VmaVulkanFunctions vulkanFunctions;
	VkResult res = vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vulkanFunctions);
	allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;

	res = vmaCreateAllocator(&allocatorCreateInfo, &allocator_);
	assert(res == VK_SUCCESS);


	images_.objects_.reserve(MAX_IMAGES);
	buffers_.objects_.reserve(MAX_BUFFERS);
	descriptorSets_.objects_.reserve(MAX_DESCRIPTORS);


	// default objs
	// from ChatGpt, may not be true: you can set a generic sampler with a big maxLod like 100, and reuse it for all your textures of any mip count.
	// The driver will clamp dynamically per texture.
	//samplers["default"] = CreateSampler(100);
	//samplers["default_shadow"] = CreateShadowSampler(1);
	//nil_texture = CreateTexture("");

	return 0; // no error

};

Context::~Context(
) {
	CleanUp();
};

void Context::CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler) {

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

void Context::CreateImageView(
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

	VK_CHECK(vkCreateImageView(core.device_, &viewInfo, nullptr, &view));
}

[[nodiscard]] DescriptorSetHandle Context::CreateDescriptorSet(
	DescriptorPoolHandle pool,
	VkDescriptorSetLayout& layout
) {

	std::array<VkDescriptorSetLayout, 1> layouts = {
		layout
	};

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	// This needs to be implemented using rotating pools.
	allocInfo.descriptorPool = *descriptorPool;
	allocInfo.descriptorSetCount = 1;
	//allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

	allocInfo.pSetLayouts = layouts.data();

	DescriptorSet ds;
	VkResult result = vkAllocateDescriptorSets(core.device_, &allocInfo, &ds.set);
	ds.layout = layout;

	if (result == VK_ERROR_OUT_OF_POOL_MEMORY) {

		assert(false && "oom pool");

		// Rotate the descriptor pool. try again
		// Manage live dspools. defrag
	}

	if (result != VK_SUCCESS) {
		throw std::runtime_error("failed to allocate descriptor sets!");
	}

	return descriptorSets_.create(std::move(ds));

}

[[nodiscard]] DeviceBufferHandle Context::CreateBuffer(
	BufferCreateInfo bufferCreateInfo
) {
	DeviceBuffer desc{};

	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = bufferCreateInfo.size;
	ci.usage = bufferCreateInfo.usage_flags;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo aci{};
	aci.usage = bufferCreateInfo.vma_usage;

	// Optionally choose pools here based on usage_flags/usage
	VK_CHECK(vmaCreateBuffer(allocator_,
		&ci,
		&aci,
		&desc.buffer,
		&desc.alloc,
		&desc.allocInfo)
	);

	vmaGetAllocationMemoryProperties(allocator_, desc.alloc, &desc.memory_flags);
	desc.usage = bufferCreateInfo.usage_flags;

	return buffers_.create(std::move(desc));
}

void Context::UploadDataToBuffer(
	DeviceBufferHandle buffer,
	const void* data,
	size_t dataSize,
	VkPipelineStageFlags dstStages,
	VkAccessFlags dstAccess
) {

	return UploadDataToBuffer(
		buffer,
		data,
		dataSize,
		dstStages,
		dstAccess,
		0,
		0
	);
}

void Context::UploadDataToBuffer(
	DeviceBufferHandle buffer,
	const void* data,
	size_t dataSize,
	VkPipelineStageFlags dstStages,
	VkAccessFlags dstAccess,
	size_t src_offset,
	size_t dest_offset
) {
	DeviceBuffer* bufferDesc = buffers_.get(buffer);

	// Since Im not subclassing/specializing buffers I retired this.
	//const VkAccessFlags dstAccess =
	//	(bufferDesc->role & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? VK_ACCESS_UNIFORM_READ_BIT
	//	: VK_ACCESS_SHADER_READ_BIT;

	VkCommandBuffer tmpBuf = beginSingleTimeCommands(core.device_, core.defaultCommandPool_);

	if (bufferDesc->is_mapable()) {
		// Direct copy into mapped allocation
		VkResult result = vmaCopyMemoryToAllocation(allocator_, data, bufferDesc->alloc, 0, dataSize);
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
	sAlloc.pool = D_StagingPool;

	VK_CHECK(vmaCreateBuffer(allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr));

	// Fill the staging buffer with the *provided* data
	VkResult result = vmaCopyMemoryToAllocation(allocator_, data, stagingAlloc, 0, dataSize);
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
		0, nullptr);


	endSingleTimeCommands(core.device_, tmpBuf, core.graphicsQueue_, core.defaultCommandPool_);

	// we could maybe delay this
	vmaDestroyBuffer(allocator_, stagingBuf, stagingAlloc);
}

// This is functional but really crude, no descriptorPool rotation support, and despite good defualts it ideally should at least take
// framesInFlight count
[[nodiscard]] DescriptorPoolHandle Context::createDescriptorPool() {
	const uint32_t bindless_sets = MAX_FRAMES_IN_FLIGHT;
	const uint32_t max_textures = MAX_TEXTURES;

	const uint32_t total_sampled_images = bindless_sets * max_textures;
	const uint32_t total_samplers = bindless_sets * max_textures;

	std::array<VkDescriptorPoolSize, 4> poolSizes = { {
	  { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,   2u * MAX_FRAMES_IN_FLIGHT },
	  { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   2u * MAX_FRAMES_IN_FLIGHT },
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
	return descriptorPools_.create(std::move(dpool));
}


// This does not depend on context (jsut the core device payload and should not be part of it really) 
void PipelineBuilder::CreatePipelineLayout(
	const PipelineCreateInfo::PipelineLayoutDesc& descIn,
	Pipeline::Layout& out)
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

	// ---- 3) Push constant ranges (max 8)
	VkPushConstantRange ranges[8]{};
	const uint32_t rangeCount = static_cast<uint32_t>(descIn.pushConstants.size());

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(core.physicalDevice_, &properties);
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

	// ---- 4) Create VkPipelineLayout
	VkPipelineLayoutCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = setLayoutCount,
		.pSetLayouts = setLayoutCount ? vkSetLayouts.data() : nullptr,
		.pushConstantRangeCount = rangeCount,
		.pPushConstantRanges = rangeCount ? ranges : nullptr,
	};

	VK_CHECK(vkCreatePipelineLayout(core.device_, &ci, nullptr, &out.vk_layout));
	//out.vk_sets = vkSetLayouts;
}

[[nodiscard]] PipelineHandle PipelineBuilder::CreateGraphicsPipeline(
	PipelineCreateInfo pipelineCreateInfo
) {

	Pipeline pipeline;
	auto v = ctx_.shaderModules_.get(pipelineCreateInfo.vert);
	auto f = ctx_.shaderModules_.get(pipelineCreateInfo.frag);

	// SHADER
	VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
	vertShaderStageInfo.sType =
		VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
	vertShaderStageInfo.module = *v;
	vertShaderStageInfo.pName = "mainVS";

	VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
	fragShaderStageInfo.sType =
		VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	fragShaderStageInfo.module = *f;
	fragShaderStageInfo.pName = "mainPS";

	VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo,
													  fragShaderStageInfo };

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	auto bindingDescription = Vertex::getBindingDescription();
	auto attributeDescriptions = Vertex::getAttributeDescriptions();
	vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescription.size());
	vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
	vertexInputInfo.pVertexBindingDescriptions = bindingDescription.data();
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType =
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	// VIEWPORT
	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	// RASTERIZER
	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = pipelineCreateInfo.rasterSettings.depthClampEnable;
	rasterizer.rasterizerDiscardEnable = pipelineCreateInfo.rasterSettings.rasterizerDiscardEnable;
	rasterizer.polygonMode = pipelineCreateInfo.rasterSettings.polygonMode;
	rasterizer.lineWidth = pipelineCreateInfo.rasterSettings.lineWidth;
	rasterizer.cullMode = pipelineCreateInfo.rasterSettings.cullMode;
	rasterizer.frontFace = pipelineCreateInfo.rasterSettings.frontFace;
	rasterizer.depthBiasEnable = pipelineCreateInfo.rasterSettings.depthBiasEnable;

	// MULTISAMPLING
	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// STENCIL
	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	// COLORBLEND
	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	colorBlendAttachment.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType =
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.logicOp = VK_LOGIC_OP_COPY;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;
	colorBlending.blendConstants[0] = 0.0f;
	colorBlending.blendConstants[1] = 0.0f;
	colorBlending.blendConstants[2] = 0.0f;
	colorBlending.blendConstants[3] = 0.0f;

	// DYNAMIC STATES
	// These two are elemental. There are extensions for almost anything do. Maybe it is woth to go all in dynamic state.
	std::vector<VkDynamicState> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};

	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount =
		static_cast<uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

#pragma region ---[LayoutDescription]---
	VkPipelineLayout lyt;
	CreatePipelineLayout(pipelineCreateInfo.layoutDescription, lyt);

#pragma endregion


	//// This depends on RenderPass description too...
	//std::vector<VkFormat> colorAttachmentFormats;
	//colorAttachmentFormats.push_back(static_cast<VkFormat>(swapchain_->GetImageFormat()));
	//VkPipelineRenderingCreateInfo dynamic_rendering_pipeline_info{
	//	VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR
	//};
	//dynamic_rendering_pipeline_info.pNext = VK_NULL_HANDLE;
	//dynamic_rendering_pipeline_info.colorAttachmentCount = 1;
	//dynamic_rendering_pipeline_info.pColorAttachmentFormats = colorAttachmentFormats.data();
	//dynamic_rendering_pipeline_info.depthAttachmentFormat = static_cast<VkFormat>(swapchain_->GetDepthImageFormat());
	////dynamic_rendering_pipeline_info.pColorAttachmentFormats = &pipelineCreateInfo.swapchainImageFormat;
	////dynamic_rendering_pipeline_info.depthAttachmentFormat = pipelineCreateInfo.depth_format;
	//dynamic_rendering_pipeline_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.layout = pipelineLayput;
	pipelineInfo.pNext = &dynamicRenderingInfo_;

	if (vkCreateGraphicsPipelines(ctx_.core.device_, VK_NULL_HANDLE, 1, &pipelineInfo,
		nullptr, &pipeline.pipeline) != VK_SUCCESS)
	{
		throw std::runtime_error("failed to create graphics pipeline!");
	}

	return ctx_.graphicPipelines_.create(std::move(pipeline));

}

void Context::TransitionImageLayout(
	ImageHandle image_handle,
	VkImageLayout oldLayout,
	VkImageLayout newLayout

) {
	assert(image_handle.valid() && "Image handle was not valid");
	auto image = images_.get(image_handle);

	TransitionImageLayout(
		image->image,
		image->format,
		oldLayout,
		newLayout
	);

}

void Context::TransitionImageLayout(
	VkImage image,
	VkFormat format,
	VkImageLayout oldLayout,
	VkImageLayout newLayout
) {
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

VkCommandBuffer Context::GetCommandBuffer() {
	return beginSingleTimeCommands(
		core.device_,
		core.defaultCommandPool_
	);

}

void Context::SubmitCommandBuffer(VkCommandBuffer cmdBuf, VkCommandPool pool, VkQueue queue) {
	endSingleTimeCommands(
		core.device_,
		cmdBuf,
		queue,
		pool
	);
}

// This whole resource upload is too primitie and slow 
TextureHandle Context::GetOrCreateTexure(std::string uri, ktxTexture2& texture) {

	// 
	auto it = textures_uploaded.find(uri);
	if (it != textures_uploaded.end()) { return it->second; };
	//

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
		allocator_,
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

		vmaDestroyImage(allocator_, image, imageAllocation);

		// this is illegal and should just assert out really.
		// even on release build? ftm
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
		allocator_,
		&stagingBufferInfo,
		&stagingAllocInfo,
		&stagingBuffer,
		&stagingAllocation,
		&stagingAllocationInfo
	);

	if (vr != VK_SUCCESS) {
		vmaDestroyImage(allocator_, image, imageAllocation);
		throw std::runtime_error("vmaCreateBuffer failed for KTX staging buffer.");
	}

	// Guaranteed mapped due to chosen flags. Ignore warning
#pragma warning(suppress : 6387)
	std::memcpy(stagingAllocationInfo.pMappedData, ktxData, static_cast<size_t>(ktxDataSize));
	vmaFlushAllocation(allocator_, stagingAllocation, 0, VK_WHOLE_SIZE);

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
						vmaDestroyBuffer(allocator_, stagingBuffer, stagingAllocation);
						vmaDestroyImage(allocator_, image, imageAllocation);
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
				vmaDestroyBuffer(allocator_, stagingBuffer, stagingAllocation);
				vmaDestroyImage(allocator_, image, imageAllocation);
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




	// texture_copies.push_back()

	// Instead of this upload should be scheduled to do in abtch and recorded to the transferQueue! 
	// -------------------------------------------------------------------------
	// Record upload
	// -------------------------------------------------------------------------
	VkCommandBuffer cmd = beginSingleTimeCommands(core.device_, core.defaultCommandPool_); // your helper

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

	endSingleTimeCommands(core.device_, cmd, core.graphicsQueue_, core.defaultCommandPool_); // your helper; submit + wait or submit to upload queue

	vmaDestroyBuffer(allocator_, stagingBuffer, stagingAllocation);

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
	image_obj.image = image;
	//image_obj.is_tex = true;
	image_obj.alloc = imageAllocation;
	image_obj.allocInfo = imageAllocationInfo;
	image_obj.format = format;
	image_obj.usage = usage;
	image_obj.extent = { width, height, depth };
	image_obj.arrayLayers = imageInfo.arrayLayers;
	image_obj.levels = mipLevels;
	image_obj.sampleView = VK_NULL_HANDLE;
	image_obj.renderView = VK_NULL_HANDLE;

	// TODO: Rework this.
	//CreateSampler(0, image_obj.sampler);
	CreateSampler(SamplerCreateInfo(), image_obj.sampler);
	image_obj.owns_sampler = true;

	CreateImageView(
		image,
		format,
		aspect,
		mipLevels,
		0,
		arrayLayers,
		viewType,
		image_obj.renderView
	);

	image_obj.sampleView = image_obj.renderView;

	auto res = textures_.create(std::move(image_obj));

	textures_uploaded[uri] = res;

	return res;

}

//void Context::CreateImage(ImageCreateInfo _imageCreateInfo,VkImage& out) {}

[[nodiscard]] ImageHandle Context::CreateImage(ImageCreateInfo _imageCreateInfo)
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
	allocation_create_info.pool = D_2DImagesPool;

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

	VkResult res = vmaCreateImage(allocator_, &imgCI, &allocation_create_info, &image.image, &image.alloc, &allocInfo);
	if (res != VK_SUCCESS) {
		throw std::runtime_error("vmaCreateImage (in pool) failed");
	}

	return images_.create(std::move(image));

}

[[nodiscard]] DeviceBufferHandle Context::CreateStagingBuffer(VkDeviceSize size)
{
	DeviceBuffer desc{};

	// 1) Describe the buffer itself
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size;                          // VkDeviceSize
	ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; // staging = copy source
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	// 2) Describe how we want the allocation
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO; // modern VMA: let it pick a good type
	// Tell VMA we intend to write from the CPU sequentially
	aci.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	// Belt & suspenders: force host-visible + coherent
	aci.requiredFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	// 3) Create buffer + allocation
	VK_CHECK(vmaCreateBuffer(allocator_, &ci, &aci,
		&desc.buffer, &desc.alloc, &desc.allocInfo));

	// 4) Cache memory properties for quick checks/debug
	VkMemoryPropertyFlags props{};
	vmaGetAllocationMemoryProperties(allocator_, desc.alloc, &props);
	desc.memory_flags = props;
	desc.usage = ci.usage;

	// Safety: make sure we got what we asked for
	// (You can convert to your own assert/log system)
	if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
		// This should never happen given requiredFlags, but check anyway.
		// Handle error as appropriate for your engine:
		// throw/abort/log + destroy
	}

	return buffers_.create(std::move(desc));
}

[[nodiscard]] DeviceBufferHandle Context::CreateStagingBuffer(const void* srcData,
	VkDeviceSize size)
{
	auto handle = CreateStagingBuffer(size);
	auto* buf = buffers_.get(handle);
	void* mapped = nullptr;
	VK_CHECK(vmaMapMemory(allocator_, buf->alloc, &mapped));
	// NOTE: memcpy(dst <- src)
	std::memcpy(mapped, srcData, static_cast<size_t>(size));
	// HOST_COHERENT means we don't need vmaFlushAllocation.
	// If you drop HOST_COHERENT in the future, call vmaFlushAllocation here.
	vmaUnmapMemory(allocator_, buf->alloc);
	return handle;
}

[[nodiscard]] ShaderModuleHandle Context::CreateShaderModule(std::vector<char> code) {
	auto shader = createDeviceShaderModule(
		core.device_,
		std::move(code)
	);
	return shaderModules_.create(std::move(shader));
};

void PipelineBuilder::CreateDescriptorSetLayout(
	const PipelineCreateInfo::DescriptorSetDescription& descIn,
	VkDescriptorSetLayout& layout)
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

// 



void destroy(Context* ctx, ShaderModuleHandle handle) {
	assert(handle.valid());
	auto sm = ctx->shaderModules_.get(handle);
	vkDestroyShaderModule(ctx->core.device_, *sm, NULL);
	ctx->shaderModules_.destroy(handle);
}
void destroy(Context* ctx, DescriptorPoolHandle handle) {
	//vkDeviceWaitIdle(core_.device_);
	//vkDestroyDescriptorPool(ha)
}
void destroy(Context* ctx, DescriptorSetHandle handle){}
void destroy(Context* ctx, ImageHandle handle)
{

	if (!handle.valid()) return;

	//auto image = ctx->images_.get(handle);
	Image& image = *ctx->images_.get(handle);

	// Destroy any image views created from this image first.
	//if (image.view != VK_NULL_HANDLE) {
	//    vkDestroyImageView(device_, image.view, nullptr);
	//    image.view = VK_NULL_HANDLE;
	//}

	// If you created other views per mip/layer, destroy those too.
	// If you created a sampler, destroy that separately:
	// vkDestroySampler(device_, image.sampler, nullptr);

	if (image.image != VK_NULL_HANDLE) {
		vmaDestroyImage(ctx->allocator_, image.image, image.alloc);
		image.image = VK_NULL_HANDLE;
		image.alloc = nullptr;
	}

	ctx->images_.destroy(handle);

	//auto obj_ = ctx->images_.get(handle);
	//vmaDestroyImage(*ctx->allocator_, obj_->image, obj_->alloc);
	//ctx->images_.destroy(handle);
}

void destroy(Context* ctx, DeviceBufferHandle handle) {
	assert(handle.valid());
	auto db = ctx->buffers_.get(handle);
	vmaDestroyBuffer(ctx->allocator_, db->buffer, db->alloc);
	ctx->buffers_.destroy(handle);
}

void destroy(Context* ctx, PipelineHandle handle)
{};

void Context::CleanUp
() {

	VK_CHECK(vkDeviceWaitIdle(core.device_));
	vmaDestroyPool(allocator_, D_2DImagesPool);

	shaderModules_.clear();
	//vmaDestroyPool(allocator_, D_ShaderPool);

	// This is only used to staging buffers and they should be all destroyed on deviceIdle really.
	vmaDestroyPool(allocator_, D_StagingPool);
}

// All these operations on queus should pusehd to a stack of deferred ops and done threaded? 
void Context::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	copyBuffer(srcBuffer, dstBuffer, size, 0, 0);
}

void Context::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset) {

	VkCommandBuffer commandBuffer = beginSingleTimeCommands(core.device_, core.defaultCommandPool_);

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	copyRegion.srcOffset = srcOffset;
	copyRegion.dstOffset = dstOffset;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	endSingleTimeCommands(core.device_, commandBuffer, core.graphicsQueue_, core.defaultCommandPool_);
}
