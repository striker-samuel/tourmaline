#include <Engine/Renderer/context.h>

static inline bool is_aligned4(uint32_t v) { return (v & 3u) == 0u; }

Context::Context(VkInstance& instance, VkDevice* device, VkPhysicalDevice* physicalDevice, VkSurfaceKHR surface
) {

	VmaAllocatorCreateInfo allocatorCreateInfo = {};
	allocatorCreateInfo.physicalDevice = *physicalDevice;
	allocatorCreateInfo.device = *device;
	allocatorCreateInfo.instance = instance;
	allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
	allocatorCreateInfo.flags =
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;

	// This next flag is to get handles. To interporate resources between instances, or even between Vulkan and Dx or things like that.
	// VMA_ALLOCATOR_CREATE_KHR_EXTERNAL_MEMORY_WIN32_BIT

	VmaVulkanFunctions vulkanFunctions;
	VkResult res = vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vulkanFunctions);
	allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;

	res = vmaCreateAllocator(&allocatorCreateInfo, &allocator_);
	assert(res == VK_SUCCESS);

	// This is ok because VkObjects are just opaque handles for the driver.
	device_ = *device;
	physicalDevice_ = *physicalDevice;

	// from ChatGpt, may not be true: you can set a generic sampler with a big maxLod like 100, and reuse it for all your textures of any mip count.
	// The driver will clamp dynamically per texture.
	samplers["default"] = CreateSampler(100);
	//samplers["default_shadow"] = CreateShadowSampler(1);

	images_.objects_.reserve(MAX_IMAGES);
	buffers_.objects_.reserve(MAX_BUFFERS);

	InitializeDeviceVmaPools();

	// This initialization is not the same as the others...
	// Also im making the context have ownership of the geometry buffers. may not be the cleanest or better.
	// Do I want the context to manage memory resources or to provide core app functionality like meshUpload too?
	InitalizeGeoBuffers();

	// Lets have one dpool
	default_descriptor_pool = createDescriptorPool();
	imgui_descriptor_pool = createDescriptorPool();

	// These are pipeline specific. 
	//default_descriptorLayout = createMainViewDescriptorLayout();
	//default_shadow_descriptorLayout = CreateShadowMapsDescriptorSetLayout();
	//default_meshlet_descriptorLayout = CreateMeshletDescriptorLayout();

	QueueFamilyIndices indices = findQueueFamilies(*physicalDevice, surface);

	VkCommandPoolCreateInfo poolInfo{
	.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	.queueFamilyIndex = indices.graphicsFamily.value()
	};

	vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &default_queue);

	// This should pick transfer over graphics
	vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &transfer_queue);

	for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
		if (vkCreateCommandPool(device_, &poolInfo, nullptr, &default_command_pool) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create graphics command pool!");
		}
	}

	ktx_vdi = ktxVulkanDeviceInfo_Create(
		physicalDevice_,
		device_,
		transfer_queue,
		// this maybe should be transfer queue. we only use ktx to upload tex.
		// no blitting for mipmaps, which are generated offline
		// runtime genreation for mipmaps in ktx seems to be not up to par with SOA
		default_command_pool,
		NULL
	);

};

Context::~Context(
) {
	CleanUp();
};

void Context::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	copyBuffer(srcBuffer, dstBuffer, size, 0, 0);
}

void Context::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset) {

	VkCommandBuffer commandBuffer = beginSingleTimeCommands(device_, default_command_pool);

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	copyRegion.srcOffset = srcOffset;
	copyRegion.dstOffset = dstOffset;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	endSingleTimeCommands(device_, commandBuffer, default_queue, default_command_pool);
}

void Context::InitalizeGeoBuffers() {

	{

		uint32_t totalVertexBytes = 512u * 1024u * 1024u;
		uint32_t totalIndexBytes = 512u * 1024u * 1024u;

		// Create the actual GPU buffers (these are the page's buffers)
		DeviceBufferHandle vb_pos = createBuffer(
			totalVertexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		);

		DeviceBufferHandle vb_shd = createBuffer(
			totalVertexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		);

		DeviceBufferHandle ib = createBuffer(
			totalIndexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		);

		// Build a page that OWNS allocators for those buffers
		GeometryPage page{};
		page.vertexPosBuffer = vb_pos;
		page.vertexShadingBuffer = vb_shd;
		page.indexBuffer = ib;

		// Capacity in vertices must satisfy both streams
		page.max_vertices = std::min(
			totalVertexBytes / uint32_t(sizeof(VertexPos)),
			totalVertexBytes / uint32_t(sizeof(VertexShading))
		);

		// Index allocator (bytes)
		{
			VmaVirtualBlockCreateInfo ci{};
			ci.size = totalIndexBytes;
			VK_CHECK(vmaCreateVirtualBlock(&ci, &page.index_blk));
		}

		// Vertex allocator (VERTEX UNITS!)
		{
			VmaVirtualBlockCreateInfo vu{};
			vu.size = page.max_vertices; // vertices, not bytes
			VK_CHECK(vmaCreateVirtualBlock(&vu, &page.vertex_units_blk));
		}


		current_geometry_page = geometry_pages.create(std::move(page));

	}

}

void Context::InitializeDeviceVmaPools() {

	// DEVICE-LOCAL pool for 2D images
	{
		VkImageCreateInfo imgCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imgCI.imageType = VK_IMAGE_TYPE_2D;
		imgCI.format = VK_FORMAT_R8G8B8A8_UNORM;
		imgCI.extent = { 4, 4, 1 };
		imgCI.mipLevels = 1;
		imgCI.arrayLayers = 1;
		imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
		imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
		imgCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT
			| VK_IMAGE_USAGE_TRANSFER_DST_BIT
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // for vkCmdBlitImage when building mips

		VmaAllocationCreateInfo aCI{};
		aCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

		uint32_t typeIndex = UINT32_MAX;
		VK_CHECK(vmaFindMemoryTypeIndexForImageInfo(allocator_, &imgCI, &aCI, &typeIndex));

		VmaPoolCreateInfo pci{};
		pci.memoryTypeIndex = typeIndex;
		pci.blockSize = 64 * 1024 * 1024;
		VK_CHECK(vmaCreatePool(allocator_, &pci, &D_2DImagesPool));
	}

	//// DEVICE-LOCAL pool for static geometry buffers
	//{
	//	VkBufferCreateInfo bufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	//	bufCI.size = 64 * 1024; // dummy
	//	bufCI.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	//	VmaAllocationCreateInfo aCI{};
	//	aCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	//	uint32_t typeIndex = UINT32_MAX;
	//	VK_CHECK(vmaFindMemoryTypeIndexForBufferInfo(allocator_, &bufCI, &aCI, &typeIndex));

	//	VmaPoolCreateInfo pci{};
	//	pci.memoryTypeIndex = typeIndex;
	//	pci.blockSize = 64 * 1024 * 1024;
	//	VK_CHECK(vmaCreatePool(allocator_, &pci, &D_geo));
	//}

	{
		VkBufferCreateInfo bufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bufCI.size = 64 * 1024;
		bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

		VmaAllocationCreateInfo aCI{};
		aCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
			| VMA_ALLOCATION_CREATE_MAPPED_BIT;
		aCI.usage = VMA_MEMORY_USAGE_AUTO;

		uint32_t typeIndex = UINT32_MAX;
		vmaFindMemoryTypeIndexForBufferInfo(allocator_, &bufCI, &aCI, &typeIndex);

		VmaPoolCreateInfo pci{};
		pci.memoryTypeIndex = typeIndex; // host-visible & host-coherent type
		pci.blockSize = 32 * 1024 * 1024;
		VK_CHECK(vmaCreatePool(allocator_, &pci, &D_StagingPool));
	}


}

[[nodiscard]] SamplerHandle Context::CreateSampler(uint32_t levelCount) {

	VkSampler sampler_;
	createSampler(physicalDevice_, device_, sampler_, levelCount, VK_COMPARE_OP_ALWAYS);

	return samplers_.create(std::move(sampler_));

}

//[[nodiscard]] SamplerHandle Context::CreateShadowSampler(uint32_t levelCount) {
//
//	VkSampler sampler_;
//
//	VkSamplerCreateInfo sci{};
//	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
//
//	sci.magFilter = VK_FILTER_LINEAR;
//	sci.minFilter = VK_FILTER_LINEAR;
//	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
//
//	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
//	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
//	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
//
//	sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
//
//	sci.compareEnable = VK_TRUE;
//	sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
//
//	VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &sampler_));
//	return samplers_.create(std::move(sampler_));
//
//}

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

	VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &view));
}

[[nodiscard]] DescriptorSetHandle Context::CreateDescriptorSet(
	VkDescriptorSetLayout& layout
	//VkDescriptorSet& set
) {
	// Pools should be rolled over on fail, get new pool. then discard FIFO
	auto descriptorPool = descriptorSetPools_.get(default_descriptor_pool);
	//auto layout = descriptorSetLayouts_.get(lyt);

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
	VkResult result = vkAllocateDescriptorSets(device_, &allocInfo, &ds.set);
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

[[nodiscard]] DeviceBufferHandle Context::createBuffer(
	VkDeviceSize size,
	VmaMemoryUsage vma_usage,
	VkBufferUsageFlags usage_flags
) {
	BufferDescription desc{};

	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size;
	ci.usage = usage_flags;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo aci{};
	aci.usage = vma_usage;
	// Optionally choose pools here based on usage_flags/usage

	VK_CHECK(vmaCreateBuffer(allocator_,
		&ci,
		&aci,
		&desc.buffer,
		&desc.alloc,
		&desc.allocInfo)
	);

	// Query memory properties to know if it’s host-visible:
	VkMemoryPropertyFlags props{};
	vmaGetAllocationMemoryProperties(allocator_, desc.alloc, &props);
	desc.memory_flags = props;

	// Lets go plain usageflags here
	desc.usage = usage_flags;

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
	BufferDescription* bufferDesc = buffers_.get(buffer);

	// Since Im not subclassing/specializing buffers I retired this.
	//const VkAccessFlags dstAccess =
	//	(bufferDesc->role & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? VK_ACCESS_UNIFORM_READ_BIT
	//	: VK_ACCESS_SHADER_READ_BIT;

	VkCommandBuffer tmpBuf = beginSingleTimeCommands(device_, default_command_pool);

	if (bufferDesc->is_mapapble()) {
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

		endSingleTimeCommands(device_, tmpBuf, default_queue, default_command_pool);
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

	vmaCreateBuffer(allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr);

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


	endSingleTimeCommands(device_, tmpBuf, default_queue, default_command_pool);

	// we could maybe delay this
	vmaDestroyBuffer(allocator_, stagingBuf, stagingAlloc);
}


// I shold update this to not be a monolithic block in
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
	VK_CHECK(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &dpool));
	return descriptorSetPools_.create(std::move(dpool));
}

//[[nodiscard]] PipelineHandle Context::CreateShadowPipeline(ShaderModuleHandle vert) {
//
//	// We ure using native VkPipelineLayout to ease things for now so we dont have to formalize a way to describe that.
//	Pipeline pipeline = {};
//	//pipeline.raster_settings = default_raster_settings;
//	//pipeline.frag = ;
//	pipeline.vert = vert;
//	pipeline.type = PIPELINE_SHADOW;
//
//	// 16 bit depth
//	constexpr float depthBiasConstant = 0; // lets try  FFXVVI oriented depth bias
//	constexpr float depthBiasSlope = 1.0f;
//
//	auto v = shaderModules_.get(vert);
//	VkPipelineShaderStageCreateInfo vertStage{};
//	vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
//	vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
//	vertStage.module = *v;
//	vertStage.pName = "mainVS";
//
//	VkPipelineShaderStageCreateInfo shaderStages[] = { vertStage };
//
//	//VERTEX Duplicated from CreatePipeline... this needs updating too if it were to change.
//
//	auto bindingDescription = Vertex::getBindingDescription();
//	auto attributeDescriptions = Vertex::getAttributeDescriptions();
//
//	VkPipelineVertexInputStateCreateInfo vertexInput{};
//	vertexInput.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
//
//	vertexInput.vertexBindingDescriptionCount = bindingDescription.size();
//	vertexInput.vertexAttributeDescriptionCount =
//		static_cast<uint32_t>(attributeDescriptions.size());
//	vertexInput.pVertexBindingDescriptions = bindingDescription.data();
//	vertexInput.pVertexAttributeDescriptions = attributeDescriptions.data();
//
//	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
//	inputAssembly.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
//	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
//	inputAssembly.primitiveRestartEnable = VK_FALSE;
//
//	// 3. Viewport & scissor (or mark as dynamic)
//	VkPipelineViewportStateCreateInfo viewportState{};
//	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
//	viewportState.viewportCount = 1;
//	//viewportState.pViewports = &viewport; // or null if dynamic // We are going dynamic
//	viewportState.scissorCount = 1;
//	//viewportState.pScissors = &scissor;  // or null if dynamic
//
//	VkPipelineRasterizationStateCreateInfo raster{};
//	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
//	raster.polygonMode = VK_POLYGON_MODE_FILL;
//	raster.cullMode = VK_CULL_MODE_BACK_BIT; 
//	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
//
//	raster.depthClampEnable = VK_FALSE; // Need to enable feature for VK_TRUE
//	raster.rasterizerDiscardEnable = VK_FALSE;
//
//	raster.depthBiasEnable = VK_TRUE;                // usually TRUE for shadow acne fix
//	raster.depthBiasConstantFactor = depthBiasConstant;
//	raster.depthBiasSlopeFactor = depthBiasSlope;
//	raster.lineWidth = 1.0f;
//
//	// 5. Multisampling
//	VkPipelineMultisampleStateCreateInfo ms{};
//	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
//	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
//	ms.sampleShadingEnable = VK_FALSE;
//
//	// 6. Depth-stencil
//	VkPipelineDepthStencilStateCreateInfo depthStencil{};
//	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
//	depthStencil.depthTestEnable = VK_TRUE;
//	depthStencil.depthWriteEnable = VK_TRUE;
//
//	// Changed 
//	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
//	depthStencil.depthBoundsTestEnable = VK_FALSE;
//	depthStencil.stencilTestEnable = VK_FALSE;
//
//	//depthStencil.depthTestEnable = VK_TRUE;
//	//depthStencil.depthWriteEnable = VK_TRUE;
//	//depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
//
//	// 7. Color blend: **no color attachments**
//	VkPipelineColorBlendStateCreateInfo colorBlend{};
//	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
//	colorBlend.attachmentCount = 0;
//	colorBlend.pAttachments = nullptr;
//
//	std::vector<VkDynamicState> dynamicStates = {
//		VK_DYNAMIC_STATE_VIEWPORT,
//		VK_DYNAMIC_STATE_SCISSOR
//	};
//
//	VkPipelineDynamicStateCreateInfo dynamicState{};
//	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
//	dynamicState.dynamicStateCount =
//		static_cast<uint32_t>(dynamicStates.size());
//	dynamicState.pDynamicStates = dynamicStates.data();
//
//
//	// DYNAMIC RENDERING INFO.
//
//	// Provide information for dynamic rendering
//	VkPipelineRenderingCreateInfo dynamic_rendering_pipeline_info{
//		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
//	dynamic_rendering_pipeline_info.pNext = VK_NULL_HANDLE;
//
//	dynamic_rendering_pipeline_info.depthAttachmentFormat = SHADOWMAP_DEPTH_FORMAT;
//	dynamic_rendering_pipeline_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
//
//
//#pragma region ---[Push Constants]---
//
//
//	VkPushConstantRange range{};
//	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
//	range.offset = 0;
//	range.size = sizeof(MainViewPushConstants); // I dont need materialInstance pc for now. Maybe later ill do alphatested mats for non opaque geo
//
//	auto shadow_def_lyt = descriptorSetLayouts_.get(default_shadow_descriptorLayout);
//	auto descriptorSetLayout = shadow_def_lyt;
//
//	VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
//	layoutInfo.setLayoutCount = 1;
//	layoutInfo.pSetLayouts = descriptorSetLayout;
//	layoutInfo.pushConstantRangeCount = 1;
//	layoutInfo.pPushConstantRanges = &range;
//
//	if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
//		&pipeline.plyt) != VK_SUCCESS) {
//		throw std::runtime_error("failed to create pipeline layout!");
//	}
//
//#pragma endregion
//
//	// 9. Finally, the pipeline create info
//	VkGraphicsPipelineCreateInfo pipeCI{};
//	pipeCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
//	pipeCI.stageCount = 1;                          // <— ONLY vertex stage
//	pipeCI.pStages = shaderStages;
//
//	pipeCI.pVertexInputState = &vertexInput;
//	pipeCI.pInputAssemblyState = &inputAssembly;
//	pipeCI.pViewportState = &viewportState;
//	pipeCI.pRasterizationState = &raster;
//	pipeCI.pMultisampleState = &ms;
//	pipeCI.pDepthStencilState = &depthStencil;
//	pipeCI.pColorBlendState = &colorBlend;
//	pipeCI.pDynamicState = &dynamicState;
//
//	pipeCI.layout = pipeline.plyt;
//	pipeCI.subpass = 0;
//	pipeCI.pNext = &dynamic_rendering_pipeline_info;
//
//
//	assert(pipeCI.pMultisampleState != nullptr);
//
//	std::cout << "creating Shadow Pipeline" << std::endl;
//	vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &pipeline.pipeline);
//	std::cout << "end creating shadow pipeline" << std::endl;
//
//
//	return pipelines_.create(std::move(pipeline));
//
//}


void Context::CreatePipelineLayout(const PipelineLayoutDesc& descIn, PipelineLayout& out)
{

	// ---- 1) Canonicalize sets (sort by set index)
	std::vector<DescriptorSetLayoutDesc> sets = descIn.sets;
	std::sort(sets.begin(), sets.end(),
		[](const auto& a, const auto& b) { return a.set < b.set; });

	// ---- 2) Determine max set index and build VkDescriptorSetLayout array
	uint32_t maxSet = 0;
	for (const auto& s : sets) maxSet = std::max(maxSet, s.set);

	// You can pick your own limit. Vulkan guarantees at least 4, often 8+.
	// If you want "max 8 sets" like you did for push constants:
	assert(maxSet < 8 && "Too many descriptor sets (max set index must be < 8)");

	const uint32_t setLayoutCount = (sets.empty()) ? 0u : (maxSet + 1);

	std::array<VkDescriptorSetLayout, 8> vkSetLayouts{};
	vkSetLayouts.fill(VK_NULL_HANDLE);

	//out.setLayouts.clear();
	//out.setLayouts.reserve(sets.size());

	for (const auto& s : sets)
	{
		//DescriptorLayoutHandle lytHandle = createDescriptorLayout(s);
		//VkDescriptorSetLayout* vkLyt = descriptorSetLayouts_.get(lytHandle);
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		CreateDescriptorLayout(s, layout);
		vkSetLayouts[s.set] = layout;
	}

	// Optional policy: forbid gaps
	 for (uint32_t i = 0; i < setLayoutCount; ++i)
	     assert(vkSetLayouts[i] != VK_NULL_HANDLE && "Descriptor set gap not allowed");

	// ---- 3) Push constant ranges (max 8)
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

	// ---- 4) Create VkPipelineLayout
	VkPipelineLayoutCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = setLayoutCount,
		.pSetLayouts = setLayoutCount ? vkSetLayouts.data() : nullptr,
		.pushConstantRangeCount = rangeCount,
		.pPushConstantRanges = rangeCount ? ranges : nullptr,
	};

	VK_CHECK(
		vkCreatePipelineLayout(device_, &ci, nullptr, &out.layout)
	);
	out.sets = vkSetLayouts;

	//return pipelineLayouts_.create(std::move(out));
}


//[[nodiscard]] PipelineHandle Context::CreatePipeline(
//	PipelineDesc/Key key,
//) {} Same but with key -> pipeline 
[[nodiscard]] PipelineHandle Context::CreatePipeline(
	RasterSettings raster_settings,
	PipelineLayoutDesc layoutDesc,
	ShaderModuleHandle vert,
	ShaderModuleHandle frag,
	VkFormat depth_format,
	VkFormat swapchainImageFormat
) {


	Pipeline pipeline;
	auto v = shaderModules_.get(vert);
	auto f = shaderModules_.get(frag);

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

	// VERTEX FORMAT. This is fixed and set beforehand

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

	rasterizer.sType =
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;


	// This may be pretty sytupid, to encode raster_settings in a custom strutct with same members.
	// May be good later for defining a common interface RHI like if i do a DirectX backend?. Should I keep it?
	rasterizer.depthClampEnable = raster_settings.depthClampEnable;
	rasterizer.rasterizerDiscardEnable = raster_settings.rasterizerDiscardEnable;
	rasterizer.polygonMode = raster_settings.polygonMode;
	rasterizer.lineWidth = raster_settings.lineWidth;
	rasterizer.cullMode = raster_settings.cullMode;
	rasterizer.frontFace = raster_settings.frontFace;
	rasterizer.depthBiasEnable = raster_settings.depthBiasEnable;

	// MULTISAMPLING
	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType =
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// STENCIL

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType =
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
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

	//PipelineLayoutHandle plytHandle = createPipelineLayout(layoutDesc);
	//PipelineLayout* plytObj = pipelineLayouts_.get(plytHandle);
	//pipeline.plyt = plytHandle;
	PipelineLayout lyt;
	CreatePipelineLayout(layoutDesc, lyt);
	pipeline.layout = lyt;

#pragma endregion


	// Provide information for dynamic rendering
	VkPipelineRenderingCreateInfo dynamic_rendering_pipeline_info{
		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	dynamic_rendering_pipeline_info.pNext = VK_NULL_HANDLE;

	// We are using the single color buffer out.
	dynamic_rendering_pipeline_info.colorAttachmentCount = 1;
	dynamic_rendering_pipeline_info.pColorAttachmentFormats = &swapchainImageFormat;

	dynamic_rendering_pipeline_info.depthAttachmentFormat = depth_format;
	dynamic_rendering_pipeline_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

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
	pipelineInfo.layout = pipeline.layout.layout;
	pipelineInfo.pNext = &dynamic_rendering_pipeline_info;

	if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
		nullptr, &pipeline.pipeline) != VK_SUCCESS)
	{
		throw std::runtime_error("failed to create graphics pipeline!");
	}

	return pipelines_.create(std::move(pipeline));

}

//
//[[nodiscard]] PipelineHandle Context::CreatePipeline(
//	RasterSettings raster_settings,
//	ShaderModuleHandle vert,
//	ShaderModuleHandle frag,
//	VkFormat depth_format,
//	VkFormat swapchainImageFormat
//) {
//
//	// We ure using native VkPipelineLayout to ease things for now so we dont have to formalize a way to describe that.
//	Pipeline pipeline = {};
//	pipeline.raster_settings = default_raster_settings;
//	pipeline.frag = frag;
//	pipeline.vert = vert;
//	pipeline.type = PIPELINE_MATERIAL;
//
//	auto v = shaderModules_.get(vert);
//	auto f = shaderModules_.get(frag);
//
//	// SHADER
//	VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
//	vertShaderStageInfo.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
//	vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
//	vertShaderStageInfo.module = *v;
//	vertShaderStageInfo.pName = "mainVS";
//
//	VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
//	fragShaderStageInfo.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
//	fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
//	fragShaderStageInfo.module = *f;
//	fragShaderStageInfo.pName = "mainPS";
//
//	VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo,
//													  fragShaderStageInfo };
//
//	// VERTEX FORMAT
//
//	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
//	vertexInputInfo.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
//
//	auto bindingDescription = Vertex::getBindingDescription();
//	auto attributeDescriptions = Vertex::getAttributeDescriptions();
//
//	vertexInputInfo.vertexBindingDescriptionCount = bindingDescription.size();
//	vertexInputInfo.vertexAttributeDescriptionCount =
//		static_cast<uint32_t>(attributeDescriptions.size());
//	vertexInputInfo.pVertexBindingDescriptions = bindingDescription.data();
//	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
//
//	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
//	inputAssembly.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
//	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
//	inputAssembly.primitiveRestartEnable = VK_FALSE;
//
//	// VIEWPORT
//
//	VkPipelineViewportStateCreateInfo viewportState{};
//	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
//	viewportState.viewportCount = 1;
//	viewportState.scissorCount = 1;
//
//	// RASTERIZER
//
//	VkPipelineRasterizationStateCreateInfo rasterizer{};
//	rasterizer.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
//	rasterizer.depthClampEnable = VK_FALSE;
//	rasterizer.rasterizerDiscardEnable = VK_FALSE;
//	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
//	rasterizer.lineWidth = 1.0f;
//	rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
//	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
//	rasterizer.depthBiasEnable = VK_FALSE;
//
//	// MULTISAMPLING
//
//	VkPipelineMultisampleStateCreateInfo multisampling{};
//	multisampling.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
//	multisampling.sampleShadingEnable = VK_FALSE;
//	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
//
//	// STENCIL
//
//	VkPipelineDepthStencilStateCreateInfo depthStencil{};
//	depthStencil.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
//	depthStencil.depthTestEnable = VK_TRUE;
//	depthStencil.depthWriteEnable = VK_TRUE;
//	depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER;
//	depthStencil.depthBoundsTestEnable = VK_FALSE;
//	depthStencil.stencilTestEnable = VK_FALSE;
//
//	// COLORBLEND
//
//	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
//	colorBlendAttachment.colorWriteMask =
//		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
//		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
//	colorBlendAttachment.blendEnable = VK_FALSE;
//
//	VkPipelineColorBlendStateCreateInfo colorBlending{};
//	colorBlending.sType =
//		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
//	colorBlending.logicOpEnable = VK_FALSE;
//	colorBlending.logicOp = VK_LOGIC_OP_COPY;
//	colorBlending.attachmentCount = 1;
//	colorBlending.pAttachments = &colorBlendAttachment;
//	colorBlending.blendConstants[0] = 0.0f;
//	colorBlending.blendConstants[1] = 0.0f;
//	colorBlending.blendConstants[2] = 0.0f;
//	colorBlending.blendConstants[3] = 0.0f;
//
//	// DYNAMIC STATES
//
//	// These two are elemental. There are extensions for almost anything do. Maybe it is woth to go all in dynamic state.
//	std::vector<VkDynamicState> dynamicStates = {
//		VK_DYNAMIC_STATE_VIEWPORT,
//		VK_DYNAMIC_STATE_SCISSOR
//	};
//
//	VkPipelineDynamicStateCreateInfo dynamicState{};
//	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
//	dynamicState.dynamicStateCount =
//		static_cast<uint32_t>(dynamicStates.size());
//	dynamicState.pDynamicStates = dynamicStates.data();
//
//#pragma region ---[Push Constants]---
//
	//VkPushConstantRange range{};
	//range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	//range.offset = 0;
	//range.size = sizeof(MainViewPushConstants) + sizeof(MaterialPushConstants);;

	//std::array<VkDescriptorSetLayout, 2> layouts = {
	//	rings_layout, 
	//	texture_table_layout 
	//};

	//VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	//
	//layoutInfo.setLayoutCount = layouts.size();
	//layoutInfo.pSetLayouts = layouts.data();

	////layoutInfo.pSetLayouts = descriptorSetLayout;

	//layoutInfo.pushConstantRangeCount = 1;
	//layoutInfo.pPushConstantRanges = &range;
//
//	if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
//		&pipeline.plyt) != VK_SUCCESS) {
//		throw std::runtime_error("failed to create pipeline layout!");
//	}
//
//#pragma endregion
//
//	// Provide information for dynamic rendering
//	VkPipelineRenderingCreateInfo dynamic_rendering_pipeline_info{
//		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
//	dynamic_rendering_pipeline_info.pNext = VK_NULL_HANDLE;
//
//	// We are using the single color buffer out.
//	dynamic_rendering_pipeline_info.colorAttachmentCount = 1;
//	dynamic_rendering_pipeline_info.pColorAttachmentFormats = &swapchainImageFormat;
//
//	dynamic_rendering_pipeline_info.depthAttachmentFormat = depth_format;
//	dynamic_rendering_pipeline_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
//
//	VkGraphicsPipelineCreateInfo pipelineInfo{};
//	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
//	pipelineInfo.stageCount = 2;
//	pipelineInfo.pStages = shaderStages;
//	pipelineInfo.pVertexInputState = &vertexInputInfo;
//	pipelineInfo.pInputAssemblyState = &inputAssembly;
//	pipelineInfo.pViewportState = &viewportState;
//	pipelineInfo.pRasterizationState = &rasterizer;
//	pipelineInfo.pMultisampleState = &multisampling;
//	pipelineInfo.pColorBlendState = &colorBlending;
//	pipelineInfo.pDynamicState = &dynamicState;
//	pipelineInfo.pDepthStencilState = &depthStencil;
//	pipelineInfo.layout = pipeline.plyt;
//	pipelineInfo.pNext = &dynamic_rendering_pipeline_info;
//
//	if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
//		nullptr, &pipeline.pipeline) != VK_SUCCESS)
//	{
//		throw std::runtime_error("failed to create graphics pipeline!");
//	}
//
//	return pipelines_.create(std::move(pipeline));
//}

//[[nodiscard]] PipelineHandle Context::CreateMaterialPipeline(
//	ShaderModuleHandle vert,
//	ShaderModuleHandle frag,
//	RasterSettings raster_settings,
//	VkFormat depth_format,
//	VkFormat swapchainImageFormat
//) {
//
//	return CreatePipeline(raster_settings, vert, frag, depth_format, swapchainImageFormat);
//
//}

//[[nodiscard]] PipelineHandle Context::GetAppropiatePipeline(/* reqs*/) {
//	return default_material_pipeline;
//}

[[nodiscard]] MaterialInstanceHandle Context::CreateMaterialInstance(PipelineHandle mat_pipeline)
{
	MaterialInstance mat_instance = {};
	mat_instance.pipeline = mat_pipeline;

	return materialInstances_.create(std::move(mat_instance));
}

void Context::TransitionImageLayout(
	ImageHandle image_handle,
	//VkFormat format,
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
	// Atm format is only being used for stencil comp check. 
	VkFormat format,
	VkImageLayout oldLayout,
	VkImageLayout newLayout
) {
	VkCommandBuffer commandBuffer = beginSingleTimeCommands(device_, default_command_pool);

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

	endSingleTimeCommands(device_,
		commandBuffer,
		default_queue,
		default_command_pool
	);
}

VkCommandBuffer Context::GetCommandBuffer() {
	return beginSingleTimeCommands(device_,
		default_command_pool);

}

void Context::SubmitCommandBuffer(VkCommandBuffer cmdBuf, VkCommandPool pool, VkQueue queue) {
	endSingleTimeCommands(
		device_,
		cmdBuf,
		queue,
		pool
	);
}

ImageHandle Context::CreateTexture(ktxTexture2& texture) {

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


	// TODO: This is really ugly. Revise 
	auto* base = reinterpret_cast<ktxTexture*>(&texture);

	// Should this be compressed or uncompressed size?? My guess is uncompressed.
	ktx_size_t ktxDataSize = ktxTexture_GetDataSizeUncompressed(base);
	ktx_uint8_t* ktxData = ktxTexture_GetData(base);

	if (!ktxData || ktxDataSize == 0) {
		vmaDestroyImage(allocator_, image, imageAllocation);
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

	// -------------------------------------------------------------------------
	// Record upload
	// -------------------------------------------------------------------------
	VkCommandBuffer cmd = beginSingleTimeCommands(device_, default_command_pool); // your helper

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

	endSingleTimeCommands(device_, cmd, default_queue, default_command_pool); // your helper; submit + wait or submit to upload queue

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
	image_obj.is_tex = true;
	image_obj.alloc = imageAllocation;
	image_obj.allocInfo = imageAllocationInfo;
	image_obj.format = format;
	image_obj.usage = usage;
	image_obj.extent = { width, height, depth };
	image_obj.arrayLayers = imageInfo.arrayLayers;
	image_obj.levels = mipLevels;
	image_obj.sampleView = VK_NULL_HANDLE;
	image_obj.renderView = VK_NULL_HANDLE;


	// TODO: fix this crap
	auto smp = samplers_.get(samplers.at("default"));
	image_obj.sampler = *smp;

	for (auto& v : image_obj.arrayView) {
		v = VK_NULL_HANDLE;
	}

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

	return images_.create(std::move(image_obj));
}


//[[nodiscard]] ShadowMapHandle Context::CreateShadowMap(int width, int height)
//{
//	auto image = CreateShadowMapImage(width, height);
//
//	auto arrayLayers = 1;
//	VkImageViewType viewType = (arrayLayers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
//
//	auto view = CreateImageView(
//		image,
//		//VK_FORMAT_D32_SFLOAT, // Copied from CreateCascadedShadowMapsDepthImage. Need to checlk froamt availabilty and track it, probably...,
//		VK_IMAGE_ASPECT_DEPTH_BIT,
//		1,
//		0,
//		1,
//		viewType
//
//	);
//
//	ShadowMap sm{
//	.image = image,
//	.sampleView = view,
//	.renderView = view,
//	};
//
//	return shadowmaps_.create(std::move(sm));
//
//}

//[[nodiscard]] ImageHandle Context::CreateShadowMapImage(int texWidth, int texHeight /*int cascades*/) {
//
//	//VkFormat depth_format = VK_FORMAT_D32_SFLOAT; // I dont need stencil, so lets hardcode this for the moment here. /*findDepthFormat(*physicalDevice_);*/
//	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT // sample as texture
//		| VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
//
//	// Levels just 1. No mipmaps for shadow perFrame images. 
//	ImageHandle image_handle = CreateImage(texWidth,
//		texHeight, SHADOWMAP_DEPTH_FORMAT, usage, 1, 1);
//
//	auto image = images_.get(image_handle);
//
//	// I dont really care beucase first op will be loadClear
//	//TransitionImageLayout(
//	//	image_handle,
//	//	VK_IMAGE_LAYOUT_UNDEFINED,
//	//	VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
//	//);
//
//	//image->layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
//
//	return image_handle;
//};

[[nodiscard]] ImageHandle Context::CreateImage(int texWidth, int texHeight, VkFormat format, VkImageUsageFlags usage, uint32_t levels, uint32_t arrayLayers) {

	Image image = {};

	//VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT // sample as texture
	//	| VK_IMAGE_USAGE_TRANSFER_DST_BIT; // upload via copy // I dont need this for depth
	//VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT

	VkImageCreateInfo imgCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgCI.imageType = VK_IMAGE_TYPE_2D;
	imgCI.format = format;               // pick your format
	imgCI.extent = { static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), 1 }; // width, height, depth
	imgCI.mipLevels = levels;                                      // >1 if you want mipmaps
	imgCI.usage = usage;
	imgCI.arrayLayers = arrayLayers;                                      // >1 for arrays/cubemaps

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
	image.format = format;
	image.levels = levels;

	image.extent.width = texWidth;
	image.extent.height = texHeight;

	image.usage = usage;
	image.arrayLayers = arrayLayers;

	//VmaAllocation alloc = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo{};

	VkResult res = vmaCreateImage(allocator_, &imgCI, &allocation_create_info, &image.image, &image.alloc, &allocInfo);
	if (res != VK_SUCCESS) {
		throw std::runtime_error("vmaCreateImage (in pool) failed");
	}

	return images_.create(std::move(image));

}

[[nodiscard]] GeometryHandle Context::UploadVertexData(MeshUploadInput& geometry_data)
{
	const uint32_t vcount = static_cast<uint32_t>(geometry_data.vertices.size());
	auto page = geometry_pages.get(current_geometry_page);

	// ------------------------------------------
	// 0) Allocate vertices from ONE vertex-units block
	// ------------------------------------------
	VmaVirtualAllocation vtx_alloc{};
	VkDeviceSize baseVertex = 0; // <-- returned "offset" in VERTEX UNITS (not bytes)

	{
		VmaVirtualAllocationCreateInfo aV{};
		aV.size = vcount;     // vertex units
		aV.alignment = 1;     // vertex units

		if (vmaVirtualAllocate(page->vertex_units_blk, &aV, &vtx_alloc, &baseVertex) != VK_SUCCESS) {
			// Not enough contiguous vertex space in this page (or fragmented)
			return {};
		}
	}

	// Derived byte offsets for each stream (for copy destinations)
	const VkDeviceSize pos_byte_offset = baseVertex * sizeof(VertexPos);
	const VkDeviceSize shd_byte_offset = baseVertex * sizeof(VertexShading);

	// ------------------------------------------
	// 1) INDICES (same idea as before)
	// ------------------------------------------
	using IndexT = uint32_t;
	const VkDeviceSize indexBytes =
		sizeof(IndexT) * static_cast<VkDeviceSize>(geometry_data.indices.size());

	VmaVirtualAllocation index_alloc{};
	VkDeviceSize index_offset_bytes = 0;

	{
		VmaVirtualAllocationCreateInfo aIdx{};
		aIdx.size = indexBytes;
		aIdx.alignment = sizeof(IndexT);

		if (vmaVirtualAllocate(page->index_blk /*or index_blk*/, &aIdx, &index_alloc, &index_offset_bytes) != VK_SUCCESS) {
			// Roll back vertex allocation
			vmaVirtualFree(page->vertex_units_blk, vtx_alloc);
			return {};
		}

		auto staging_h = CreateStagingBuffer(geometry_data.indices.data(), indexBytes);
		auto staging = buffers_.get(staging_h);

		copyBuffer(staging->buffer, buffers_.get(page->indexBuffer)->buffer, indexBytes, 0, index_offset_bytes);
		destroy(this, staging_h);
	}

	// ------------------------------------------
	// 2) Split vertices into two CPU arrays
	// ------------------------------------------
	std::vector<VertexPos> pos(vcount);
	std::vector<VertexShading> shd(vcount);

	for (uint32_t i = 0; i < vcount; ++i) {
		const Vertex& v = geometry_data.vertices[i];

		pos[i].pos = v.pos;

		shd[i].color = v.color;
		shd[i].texCoord = v.texCoord;
		shd[i].normal = v.normal;
		shd[i].tangent = v.tangent;
	}

	// ------------------------------------------
	// 3) Upload positions to binding 0 buffer at derived offset
	// ------------------------------------------
	{
		const VkDeviceSize posBytes = sizeof(VertexPos) * static_cast<VkDeviceSize>(vcount);

		auto staging_h = CreateStagingBuffer(pos.data(), posBytes);
		auto staging = buffers_.get(staging_h);

		copyBuffer(
			staging->buffer,
			buffers_.get(page->vertexPosBuffer /*or vertex_buffer*/)->buffer,
			posBytes,
			0,
			pos_byte_offset
		);

		destroy(this, staging_h);
	}

	// ------------------------------------------
	// 4) Upload shading to binding 1 buffer at derived offset
	// ------------------------------------------
	{
		const VkDeviceSize shdBytes = sizeof(VertexShading) * static_cast<VkDeviceSize>(vcount);

		auto staging_h = CreateStagingBuffer(shd.data(), shdBytes);
		auto staging = buffers_.get(staging_h);

		copyBuffer(
			staging->buffer,
			buffers_.get(page->vertexShadingBuffer /*or vertex_shading_buffer*/)->buffer,
			shdBytes,
			0,
			shd_byte_offset
		);

		destroy(this, staging_h);
	}

	// ------------------------------------------
	// 5) Surface (bind-once style)
	// ------------------------------------------
	Surface ds{};
	ds.page = current_geometry_page;
	ds.index_count = static_cast<uint32_t>(geometry_data.indices.size());
	ds.first_index = static_cast<uint32_t>(index_offset_bytes / sizeof(IndexT));
	ds.base_vertex = static_cast<int32_t>(baseVertex);

	// IMPORTANT: store allocs somewhere if you want to free later.
	// e.g. ds.vertex_alloc = vtx_alloc; ds.index_alloc = index_alloc; (requires fields)

	Geometry mesh_dec{};
	mesh_dec.lods = 1;
	mesh_dec.lod_offsets = { 0 };
	mesh_dec.surfaces = { ds };
	mesh_dec.aabbMin = geometry_data.aabb_min;
	mesh_dec.aabbMax = geometry_data.aabb_max;

	return meshes_.create(std::move(mesh_dec));
}

[[nodiscard]] DeviceBufferHandle Context::CreateStagingBuffer(VkDeviceSize size)
{
	BufferDescription desc{};

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
		device_,
		std::move(code)
	);

	return shaderModules_.create(std::move(shader));

};

void Context::CreateDescriptorLayout(const DescriptorSetLayoutDesc& descIn, VkDescriptorSetLayout& layout)
{
	// 1) Canonicalize (copy then sort by binding)
	DescriptorSetLayoutDesc desc = descIn;
	//std::sort(desc.bindings.begin(), desc.bindings.end(),
	//	[](auto& a, auto& b) { return a.binding < b.binding; });

	// 2) Compute a cache key (pseudo)
	// LayoutKey key = Hash(desc);
	// if (auto it = layoutCache.find(key); it != layoutCache.end()) return it->second;

	// 3) Build VkDescriptorSetLayoutBinding array
	std::vector<VkDescriptorSetLayoutBinding> vkBindings;
	vkBindings.reserve(desc.bindings.size());

	// IMPORTANT: pImmutableSamplers must point to memory that lives until vkCreateDescriptorSetLayout returns.
	// We can keep a local vector-of-vectors of VkSampler pointers to stable storage.
	std::vector<std::vector<VkSampler>> immutableSamplersStorage;
	immutableSamplersStorage.reserve(desc.bindings.size());

	std::vector<VkDescriptorBindingFlags> bindingFlags;
	bindingFlags.reserve(desc.bindings.size());

	for (const BindingDesc& b : desc.bindings)
	{
		VkDescriptorSetLayoutBinding vkB{};
		vkB.binding = b.binding;
		vkB.descriptorType = b.type;
		vkB.descriptorCount = b.count;
		vkB.stageFlags = b.stages;

		// Immutable samplers: only valid for SAMPLER / COMBINED_IMAGE_SAMPLER
		if (!b.immutableSamplers.empty())
		{
			// Validate sizes early (debug assert)
			assert(b.immutableSamplers.size() == b.count);

			immutableSamplersStorage.emplace_back();
			auto& store = immutableSamplersStorage.back();
			store.reserve(b.immutableSamplers.size());

			for (SamplerHandle sh : b.immutableSamplers)
				store.push_back(*samplers_.get(sh)); // you store VkSampler in pool

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

	//VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	VK_CHECK(vkCreateDescriptorSetLayout(device_, &ci, nullptr, &layout));
	// 5) Cache it (pseudo)
	// layoutCache[key] = h;

	//DescriptorLayoutHandle h = descriptorSetLayouts_.create(std::move(layout));
	//return h;

}

VkQueue Context::GetGraphicsQueue() {
	return default_queue;
}

// TO-DO
// Just a placeholder for now to remember pipeline caching scheme needs inplementation
// Doom 2016 had a redudec number of PSOs in the 400
// Keeping this here as goal and to have in mind the orders of magintude we are talking about
PipelineHandle Context::GetAppropiatePipeline(PipelineKey key)
{
	return pipelines_.getHandle(0);
}

void destroy(Context* ctx, ShaderModuleHandle handle) {
	assert(handle.valid());
	auto sm = ctx->shaderModules_.get(handle);
	vkDestroyShaderModule(ctx->device_, *sm, NULL);
	ctx->shaderModules_.destroy(handle);
}
void destroy(Context* ctx, DescriptorPoolHandle handle)
{}
void destroy(Context * ctx, DescriptorSetHandle handle)
{}
void destroy(Context * ctx, DescriptorLayoutHandle handle)
{}
;

void destroy(Context* ctx, SamplerHandle handle)
{}

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



//void destroy(Context* ctx, PipelineLayoutHandle h)
//{
//	assert(h.valid());
//	auto* pl = ctx->pipelineLayouts_.get(h);
//	if (pl->layout) vkDestroyPipelineLayout(ctx->device_, pl->layout, nullptr);
//	ctx->pipelineLayouts_.destroy(h);
//}

void destroy(Context* ctx, GeometryHandle handle)
{}

void destroy(Context * ctx, GeometryPageHandle handle)
{}

void destroy(Context * ctx, MaterialInstanceHandle handle)
{}

// END

void Context::CleanUp(
) {}
