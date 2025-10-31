#pragma once
#include <Engine/Vulkan/vulkan_setup.h>
#include <Engine/Vulkan/vulkan_helpers.h>
#include <Engine/Vulkan/vulkan_funs.h>

#include <Engine/Renderer/primitives.h>
#include <Engine/Renderer/constants.h>

#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>

struct DescriptorSet {
	DescriptorLayoutHandle layout;
	VkDescriptorSet set;
};

// Materials abstractions area really PipelineObjets. Raster conf, shader objs, descriptorlayout. 
// MaterialAttachments must match desc layout, adn MaterialConstants pc layout
// So we bake the POS. VkPipelineCache not implemented now
// Another name fot his could be MatModel?
struct MaterialPipeline {

	// We need to hash these tree. but wont do it now
	RasterSettings raster_settings;
	ShaderModuleHandle vert;
	ShaderModuleHandle frag;

	// Lets not complitcate this for now. same footprint/layout for everyone. 
	//VkDescriptorSetLayout* layout;
	// 
	// Lets just hold this here for now
	VkPipelineLayout plyt;

	// Bindless, so material descriptors sets are at this level instead of binding per instnace, so;
	// Invalid initialization!
	DescriptorSetHandle ds;
	VkPipeline pipeline;
};

// MatModels or MatPipelines can have several instances with different vlaues for constans and textures.
// Raster state, vert, frag, blending and more stay the same.
struct MaterialInstance {
	MaterialPipelineHandle pipeline;
	MaterialPushConstants values = {};
};



// This style of handling things is lifted up from LightweightVk.
class Context {

	// 1 allocator per device. Device is implicit in allocator...
	VmaAllocator* allocator_;
	VkDevice* device_;
	VkPhysicalDevice* physicalDevice_;

	// These three things are primary citizens of Device Memory Residency
	VmaPool D_2DImagesPool;
	VmaPool D_ShaderPool;

	// Different pools for differnet uses: ...Skinned. Static. Dynamic
	VmaPool D_geo;
	VmaPool D_DynamicMeshPool;
	VmaPool D_StagingPool;

	// Im gonna need a custom str type... lets use std::string for now.
	std::unordered_map<std::string, VkSampler> samplers;

public:

	DescriptorPoolHandle default_descriptor_pool;
	DescriptorLayoutHandle default_descriptorLayout;

	VkCommandPool default_command_pool;
	VkQueue default_queue;

	// We neeed VkDescriptorPool pools here
	// We move from VkDp to another because its cheaper to reset them than to evict particular sets.
	// Same with CommandPools. I have a stupid simple design now with just 1 CommandPool.
	Pool<BufferDescription, BufferDescription> buffers_;
	Pool<Texture, Texture> textures_;
	Pool<DynamicMesh, DynamicMesh> meshes_;


	// These two have confusing names;
	Pool<DescriptorSet, DescriptorSet> descriptrorSetPool_;
	Pool<DescriptorPool, VkDescriptorPool> descriptorSetPoolPool_;

	Pool<DescriptorLayout, VkDescriptorSetLayout> descriptorSetLayoutPool_;
	Pool<MaterialPipeline, MaterialPipeline> materialPipelinePool_;
	Pool<MaterialInstance, MaterialInstance> materialInstancesPool_;

	Pool<ShaderModule, VkShaderModule> shaderModulePool_;

	Context(VmaAllocator* allocator, VkDevice* device, VkPhysicalDevice* physicalDevice, VkSurfaceKHR surface) {

		allocator_ = allocator;
		device_ = device;
		physicalDevice_ = physicalDevice;

		VkSampler default_sampler;
		createSampler(*physicalDevice_, *device_, default_sampler);
		// Samplers are driver objs. can be copied like this np.
		samplers["default"] = default_sampler;

		// Lets reserve upfront.
		textures_.objects_.reserve(MAX_TEXTURES);

		//materialPipelinePool_.objects_.reserve(1000);
		// This is a small reservation. just testing 
		buffers_.objects_.reserve(1000);

		InitializeDeviceVmaPools();

		// Lets have one dpool
		default_descriptor_pool = createDescriptorPool();
		default_descriptorLayout = createDescriptorLayout();

		QueueFamilyIndices indices = findQueueFamilies(*physicalDevice, surface);

		VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = indices.graphicsFamily.value()
		};

		vkGetDeviceQueue(*device_, indices.graphicsFamily.value(), 0, &default_queue);

		for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
			if (vkCreateCommandPool(*device_, &poolInfo, nullptr, &default_command_pool) !=
				VK_SUCCESS) {
				throw std::runtime_error("failed to create graphics command pool!");
			}
		}
	};
	// Maybe destructor should clear the vmaPools. Device side allocations are not managed as clear by the OS.
	~Context(


	) {
	};

	void InitializeDeviceVmaPools() {

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
			VK_CHECK(vmaFindMemoryTypeIndexForImageInfo(*allocator_, &imgCI, &aCI, &typeIndex));

			VmaPoolCreateInfo pci{};
			pci.memoryTypeIndex = typeIndex;
			pci.blockSize = 64 * 1024 * 1024;
			VK_CHECK(vmaCreatePool(*allocator_, &pci, &D_2DImagesPool));
		}

		// DEVICE-LOCAL pool for static geometry buffers
		{
			VkBufferCreateInfo bufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			bufCI.size = 64 * 1024; // dummy
			bufCI.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

			VmaAllocationCreateInfo aCI{};
			aCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

			uint32_t typeIndex = UINT32_MAX;
			VK_CHECK(vmaFindMemoryTypeIndexForBufferInfo(*allocator_, &bufCI, &aCI, &typeIndex));

			VmaPoolCreateInfo pci{};
			pci.memoryTypeIndex = typeIndex;
			pci.blockSize = 64 * 1024 * 1024;
			VK_CHECK(vmaCreatePool(*allocator_, &pci, &D_geo));
		}

		{
			VkBufferCreateInfo bufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			bufCI.size = 64 * 1024;
			bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

			VmaAllocationCreateInfo aCI{};
			aCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
				| VMA_ALLOCATION_CREATE_MAPPED_BIT;
			aCI.usage = VMA_MEMORY_USAGE_AUTO;

			uint32_t typeIndex = UINT32_MAX;
			vmaFindMemoryTypeIndexForBufferInfo(*allocator_, &bufCI, &aCI, &typeIndex);

			VmaPoolCreateInfo pci{};
			pci.memoryTypeIndex = typeIndex; // host-visible & host-coherent type
			pci.blockSize = 32 * 1024 * 1024;
			VK_CHECK(vmaCreatePool(*allocator_, &pci, &D_StagingPool));
		}


	}

	// Resource creation
	DeviceBufferDescriptionHandle CreateStagingBuffer(VkDeviceSize size);
	DeviceBufferDescriptionHandle CreateStagingBuffer(const void* srcData, VkDeviceSize size);

	ShaderModuleHandle createShaderModule(std::vector<char> code);
	MaterialPipelineHandle createMaterialPipeline(ShaderModuleHandle vert, ShaderModuleHandle frag, RasterSettings raster_settings, VkFormat depth_format, VkFormat swapchainImageFormat);
	MaterialInstanceHandle createMaterialInstance(MaterialPipelineHandle mat_pipeline);

	void getAppropiateSampler(Texture& texture);

	void createImage(int texWidth, int texHeight, VkFormat format, VkImage& image, VmaAllocation& alloc);
	void createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, VkImageView& imageView);

	void UploadToImage(VkImage& image, void* data);

	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);

	VkCommandBuffer GetCommandBuffer();
	void SubmitCommandBuffer(VkCommandBuffer cmdBuf, VkCommandPool pool, VkQueue queue);

	TextureHandle createTexture(int texWidth, int texHeight, int texChannels, VkFormat format, VkImageAspectFlagBits aspectFlags, void* data);

	DescriptorLayoutHandle createDescriptorLayout();
	DescriptorPoolHandle createDescriptorPool();
	DescriptorSetHandle createDescriptorSet(DescriptorLayoutHandle lyt);

	DeviceBufferDescriptionHandle createBuffer(uint32_t size, VmaMemoryUsage vma_usage, VkBufferUsageFlags usage_flags);

	// Ideally we would expose resource deletion to the app too right. 
	// LightweightVk goes with Holder construct to delete when no refs. I quite dont like that or RAII really.
	// Lets not do this 'yet'
	// 
	// Resource Manipulation
	void UploadDataToBuffer(DeviceBufferDescriptionHandle buffer, void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAcces);

};

// This maybe should be moved out of Context really;
void Context::createImageView(VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	//int mipLevels,
	VkImageView& imageView
) {
	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;


	viewInfo.subresourceRange.aspectMask = aspectFlags;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	//viewInfo.subresourceRange.levelCount = mipLevels;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	VK_CHECK(vkCreateImageView(*device_, &viewInfo, nullptr, &imageView));

}

DescriptorSetHandle Context::createDescriptorSet(
	DescriptorLayoutHandle lyt
) {
	// Lets make pool selection automatic
	auto descriptorPool = descriptorSetPoolPool_.get(default_descriptor_pool);
	auto layout = descriptorSetLayoutPool_.get(lyt);

	std::array<VkDescriptorSetLayout, 1> layouts = {
		*layout
	};

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	// This needs to be implemented using rotating pools.
	allocInfo.descriptorPool = *descriptorPool;
	allocInfo.descriptorSetCount = 1;
	//allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

	allocInfo.pSetLayouts = layouts.data();

	DescriptorSet ds;
	VkResult result = vkAllocateDescriptorSets(*device_, &allocInfo, &ds.set);
	ds.layout = lyt;

	if (result == VK_ERROR_OUT_OF_POOL_MEMORY) {
		// Rotate the descriptor pool. try again
		// Manage live dspools. defrag
	}

	if (result != VK_SUCCESS) {
		throw std::runtime_error("failed to allocate descriptor sets!");
	}

	return descriptrorSetPool_.create(std::move(ds));

}

DeviceBufferDescriptionHandle Context::createBuffer(
	uint32_t size,
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

	VK_CHECK(vmaCreateBuffer(*allocator_,
		&ci,
		&aci,
		&desc.buffer,
		&desc.alloc,
		&desc.allocInfo)
	);

	// Query memory properties to know if it’s host-visible:
	VkMemoryPropertyFlags props{};
	vmaGetAllocationMemoryProperties(*allocator_, desc.alloc, &props);
	desc.memory_flags = props;

	// Lets go plain usageflags here
	desc.usage = usage_flags;

	return buffers_.create(std::move(desc));
}

void Context::UploadDataToBuffer(
	DeviceBufferDescriptionHandle buffer,
	void* data,
	size_t dataSize,
	VkPipelineStageFlags dstStages,
	VkAccessFlags dstAccess
) {
	BufferDescription* bufferDesc = buffers_.get(buffer);

	// Since Im not subclassing/specializing buffers I retired this.
	//const VkAccessFlags dstAccess =
	//	(bufferDesc->role & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? VK_ACCESS_UNIFORM_READ_BIT
	//	: VK_ACCESS_SHADER_READ_BIT;

	VkCommandBuffer tmpBuf = beginSingleTimeCommands(*device_, default_command_pool);

	if (bufferDesc->is_mapapble()) {
		// Direct copy into mapped allocation
		VkResult result = vmaCopyMemoryToAllocation(*allocator_, data, bufferDesc->alloc, 0, dataSize);
		assert(result == VK_SUCCESS);

		// Host write -> shader read
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

		endSingleTimeCommands(*device_, tmpBuf, default_queue, default_command_pool);
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

	vmaCreateBuffer(*allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr);

	// Fill the staging buffer with the *provided* data
	VkResult result = vmaCopyMemoryToAllocation(*allocator_, data, stagingAlloc, 0, dataSize);
	assert(result == VK_SUCCESS);

	// Copy staging -> dst
	VkBufferCopy copyRegion{ .size = dataSize };
	vkCmdCopyBuffer(tmpBuf, stagingBuf, bufferDesc->buffer, 1, &copyRegion);

	// Make the copy visible to shaders
	VkBufferMemoryBarrier b2{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
	b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b2.dstAccessMask = dstAccess;
	b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b2.buffer = bufferDesc->buffer;
	b2.offset = 0;
	b2.size = VK_WHOLE_SIZE;

	vkCmdPipelineBarrier(
		tmpBuf,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		dstStages,
		0,
		0, nullptr,
		1, &b2,
		0, nullptr);


	endSingleTimeCommands(*device_, tmpBuf, default_queue, default_command_pool);
	vmaDestroyBuffer(*allocator_, stagingBuf, stagingAlloc);
}

// Lets just use 1 layout for now. We will need more shapes for shadows and others is my guess.
DescriptorLayoutHandle Context::createDescriptorLayout() {
	// INDEX DATA
	VkDescriptorSetLayoutBinding instanceIndexBinding{};
	instanceIndexBinding.binding = 0;
	instanceIndexBinding.descriptorCount = 1;
	instanceIndexBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	instanceIndexBinding.pImmutableSamplers = nullptr;
	instanceIndexBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutBinding instanceDataBinding{};
	instanceDataBinding.binding = 1;
	instanceDataBinding.descriptorCount = 1;
	instanceDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	instanceDataBinding.pImmutableSamplers = nullptr;
	instanceDataBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutBinding sampledImageBinding{};
	sampledImageBinding.binding = 2;
	sampledImageBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	sampledImageBinding.descriptorCount = MAX_TEXTURES;
	sampledImageBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutBinding sampBinding{};
	sampBinding.binding = 3;
	sampBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	sampBinding.descriptorCount = MAX_TEXTURES;
	sampBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	std::array<VkDescriptorSetLayoutBinding, 4> bindings = {
		instanceIndexBinding,
		instanceDataBinding,
		//
		sampledImageBinding,
		sampBinding,
	};

	// Per-binding flags — same size as bindings[]
	std::array<VkDescriptorBindingFlags, 4> bindingFlags = {
		0,
		0,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // bindless textures
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT // bindless samplers
	};

	// Chain flags struct
	VkDescriptorSetLayoutBindingFlagsCreateInfo bindless_flags{};
	bindless_flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	bindless_flags.pNext = nullptr;
	bindless_flags.bindingCount = static_cast<uint32_t>(bindingFlags.size());
	bindless_flags.pBindingFlags = bindingFlags.data();

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	layoutInfo.pNext = &bindless_flags;
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

	VkDescriptorSetLayout descriptorSetLayout;

	if (vkCreateDescriptorSetLayout(*device_, &layoutInfo, nullptr,
		&descriptorSetLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create descriptor set layout!");
	}

	auto r = descriptorSetLayoutPool_.create(std::move(descriptorSetLayout));
	return r;

}

DescriptorPoolHandle Context::createDescriptorPool() {
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
	VK_CHECK(vkCreateDescriptorPool(*device_, &poolInfo, nullptr, &dpool));
	return descriptorSetPoolPool_.create(std::move(dpool));
}

MaterialPipelineHandle Context::createMaterialPipeline(ShaderModuleHandle vert, ShaderModuleHandle frag, RasterSettings raster_settings, VkFormat depth_format, VkFormat swapchainImageFormat) {

	// We ure using native VkPipelineLayout to ease things for now so we dont have to formalize a way to describe that.
	MaterialPipeline mat_pipe = {};
	mat_pipe.raster_settings = default_raster_settings;
	mat_pipe.frag = frag;
	mat_pipe.vert = vert;

	auto v = shaderModulePool_.get(vert);
	auto f = shaderModulePool_.get(frag);

	// Lets use default
	auto def_lyt = descriptorSetLayoutPool_.get(default_descriptorLayout);

	std::array<VkDescriptorSetLayout, 1> lyts = {
		*def_lyt
		// Thats the default. we need at least that for Instance Data.
		// We could add a second lyt ot have different layoitus per MaterialModel/MaterialPipeline
		//mat_pipe.lyt
	};

	createGraphicsPipeline(
		device_,
		*v,
		*f,
		&mat_pipe.plyt,
		def_lyt,
		depth_format,
		swapchainImageFormat,
		mat_pipe.pipeline
	);

	return materialPipelinePool_.create(std::move(mat_pipe));

}

MaterialInstanceHandle Context::createMaterialInstance(MaterialPipelineHandle mat_pipeline)
{
	MaterialInstance mat_instance = {};
	mat_instance.pipeline = mat_pipeline;

	return materialInstancesPool_.create(std::move(mat_instance));
}


//void Context::GetStagingBuffer() {}
//

void Context::UploadToImage(VkImage& image, void* data) {

}

void Context::TransitionImageLayout(
	VkImage image,
	VkFormat format,
	VkImageLayout oldLayout,
	VkImageLayout newLayout
) {
	VkCommandBuffer commandBuffer = beginSingleTimeCommands(*device_, default_command_pool);

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

	if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
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
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else {
		throw std::invalid_argument("unsupported layout transition!");
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

	endSingleTimeCommands(*device_,
		commandBuffer,
		default_queue,
		default_command_pool
	);
}

VkCommandBuffer Context::GetCommandBuffer() {
	return beginSingleTimeCommands(*device_,
		default_command_pool);

}
void Context::SubmitCommandBuffer(VkCommandBuffer cmdBuf, VkCommandPool pool, VkQueue queue) {
	endSingleTimeCommands(
		*device_,
		cmdBuf,
		queue,
		pool
	);
}


TextureHandle Context::createTexture(
	int texWidth,
	int texHeight,
	int texChannels,
	VkFormat format,
	VkImageAspectFlagBits aspectFlags,
	void* data
) {


	// Textures need specialization for each usage... like diffuse, normal whatevs. 2D, 3D.....
	// Specialization should be achieved with an App defined data structure that exposes needed determining factors.
	// This will be slowly built while making a trye app.

	uint32_t mipLevels = static_cast<uint32_t>(
		std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;

	Texture tex{};
	tex.allocation = VK_NULL_HANDLE;
	tex.image = VK_NULL_HANDLE;
	tex.format = format;
	tex.mipLevels = mipLevels;

	createImage(texWidth, texHeight, format, tex.image, tex.allocation);
	createImageView(tex.image, format, aspectFlags, tex.imageView);

	// We could use texChannels=4 fixed
	VkDeviceSize imageSize = texWidth * texHeight * texChannels;

	auto staging_bfh = CreateStagingBuffer(
		imageSize
	);

	auto* staging = buffers_.get(staging_bfh);

	void* mapped = nullptr;
	VK_CHECK(vmaMapMemory(*allocator_, staging->alloc, &mapped));
	std::memcpy(mapped, data, static_cast<size_t>(imageSize));
	vmaUnmapMemory(*allocator_, staging->alloc);

	TransitionImageLayout(
		tex.image,
		VK_FORMAT_R8G8B8A8_SRGB,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	auto cmdBuf = beginSingleTimeCommands(*device_,
		default_command_pool);

	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = { static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), 1 };

	vkCmdCopyBufferToImage(cmdBuf, staging->buffer, tex.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	endSingleTimeCommands(
		*device_,
		cmdBuf,
		default_queue,
		default_command_pool
	);

	TransitionImageLayout(
		tex.image,
		VK_FORMAT_R8G8B8A8_SRGB,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		//VK_IMAGE_LAYOUT_GENERAL
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);


	getAppropiateSampler(tex);

	return textures_.create(std::move(tex));
}


void Context::getAppropiateSampler(Texture& texture) {


	//VkSampler _sampler;
	//createSampler(*physicalDevice_, *device_, _sampler);


	// We should try to define here which sampler would work for this texture. Sepecialization.
	texture.sampler = samplers["default"];

};

void Context::createImage(int texWidth, int texHeight, VkFormat format, VkImage& image, VmaAllocation& alloc) {

	//uint32_t mipLevels = static_cast<uint32_t>(
	//	std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;

	VkImageCreateInfo imgCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgCI.imageType = VK_IMAGE_TYPE_2D;
	imgCI.format = format;               // pick your format
	imgCI.extent = { static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), 1 };                      // width, height, depth
	//imgCI.mipLevels = mipLevels;                                      // >1 if you want mipmaps
	imgCI.mipLevels = 1;                                      // >1 if you want mipmaps
	imgCI.arrayLayers = 1;                                      // >1 for arrays/cubemaps
	imgCI.samples = VK_SAMPLE_COUNT_1_BIT;                  // MSAA? set >1
	imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;                // GPU-friendly
	imgCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT              // sample as texture
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT;        // upload via copy
	imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;              // we’ll transition later. Well jus use transition General always
	imgCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT
		| VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // for vkCmdBlitImage when building mips

	VmaAllocationCreateInfo aCI{};
	aCI.usage = VMA_MEMORY_USAGE_GPU_ONLY; // should be compatible with the pool’s memoryType
	aCI.pool = D_2DImagesPool;                      // <--- CRITICAL: place this allocation in the pool

	//VmaAllocation alloc = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo{};

	VkResult res = vmaCreateImage(*allocator_, &imgCI, &aCI, &image, &alloc, &allocInfo);
	if (res != VK_SUCCESS) {
		throw std::runtime_error("vmaCreateImage (in pool) failed");
	}

}

// Creates a host-visible, coherent staging buffer suitable for uploads.
// The buffer is created with VK_BUFFER_USAGE_TRANSFER_SRC_BIT so it can be
// copied into images/buffers via vkCmdCopy*.
//
// Overload A: just create the buffer
DeviceBufferDescriptionHandle Context::CreateStagingBuffer(VkDeviceSize size)
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
	VK_CHECK(vmaCreateBuffer(*allocator_, &ci, &aci,
		&desc.buffer, &desc.alloc, &desc.allocInfo));

	// 4) Cache memory properties for quick checks/debug
	VkMemoryPropertyFlags props{};
	vmaGetAllocationMemoryProperties(*allocator_, desc.alloc, &props);
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

// Overload B: create and upload data immediately
DeviceBufferDescriptionHandle Context::CreateStagingBuffer(const void* srcData,
	VkDeviceSize size)
{
	auto handle = CreateStagingBuffer(size);
	auto* buf = buffers_.get(handle);

	void* mapped = nullptr;
	VK_CHECK(vmaMapMemory(*allocator_, buf->alloc, &mapped));

	// NOTE: memcpy(dst <- src)
	std::memcpy(mapped, srcData, static_cast<size_t>(size));

	// HOST_COHERENT means we don't need vmaFlushAllocation.
	// If you drop HOST_COHERENT in the future, call vmaFlushAllocation here.
	vmaUnmapMemory(*allocator_, buf->alloc);

	return handle;
}


ShaderModuleHandle Context::createShaderModule(std::vector<char> code) {

	auto shader = createDeviceShaderModule(
		*device_,
		std::move(code)
	);

	return shaderModulePool_.create(std::move(shader));

};

// Need device allocation freeing
void destroy(Context* ctx, TextureHandle handle) {

	// We need to free from device pool.
	ctx->textures_.destroy(handle);
};

void destroy(Context* ctx, DeviceBufferDescriptionHandle handle) {

	// We need to freee device allocation
	ctx->buffers_.destroy(handle);

};

