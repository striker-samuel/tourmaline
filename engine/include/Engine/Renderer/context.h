#pragma once
#include <Engine/Vulkan/vulkan_setup.h>
#include <Engine/Vulkan/vulkan_helpers.h>
#include <Engine/Vulkan/vulkan_funs.h>

#include <Engine/Renderer/primitives.h>
#include <Engine/Renderer/constants.h>

#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>

#include <ktx.h>
#include <ktxvulkan.h>

struct FrameUniforms
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewProj;

	glm::mat4 invView;
	glm::mat4 invProj;
	glm::mat4 invViewProj;

	glm::vec3 cameraPos;
	float     time;

	glm::vec2 viewportSize;
	glm::vec2 invViewportSize;

	float nearPlane;
	float farPlane;
	float exposure;
	float padding; // keep 16-byte alignment
};

static_assert(sizeof(FrameUniforms) % 16 == 0);


// This architecture is heavily inspired/ lifted from LightWeightVk.
// I used it as a base for my resource model. Will be changed as this evolves ;)
// 
// ----------------------------------------------------------
// specialized with dummy structs for type safety
using ShaderModuleHandle = Handle<struct ShaderModule>;
using TextureHandle = Handle<struct Texture>;
using ImageHandle = Handle<struct Image>;
using ImageViewHandle = Handle<struct ImageView>;
using ShadowMapHandle = Handle<struct ShadowMap>;
using SamplerHandle = Handle<struct Sampler>;

using MaterialInstanceHandle = Handle<struct MaterialInstance>;
using DescriptorPoolHandle = Handle<struct DescriptorPool>;
using DescriptorSetHandle = Handle<struct DescriptorSet>;
using DescriptorLayoutHandle = Handle<struct DescriptorLayout>;
using DeviceBufferHandle = Handle<struct BufferDescription>;
using PipelineHandle = Handle<struct Pipeline>;
using MeshDescHandle = Handle<struct GpuMeshDesc>;
using GeometryPageHandle = Handle <struct GeometryPage>;


void destroy(Context* ctx, ShaderModuleHandle handle);
void destroy(Context* ctx, DeviceBufferHandle handle);
void destroy(Context* ctx, TextureHandle handle);
void destroy(Context* ctx, ImageHandle handle);

// ----------------------------------------------------------

struct DescriptorSet {
	DescriptorLayoutHandle layout;
	VkDescriptorSet set;
};

// This would be input for GetAppropiateSampler. The ideea is that requirements can solve for a VkSampler easily.
// I don't know how worth is it tho, VkSamplers being driverside objs, maybe creating new ones is just simpler/faster/more convenient.
//struct SamplerRequirements {
//
//
//};

struct GeometryPage {

	DeviceBufferHandle indexBuffer;
	DeviceBufferHandle vertexPosBuffer;
	DeviceBufferHandle vertexShadingBuffer;

	//	uint32_t vertex_cursor = 0;   // per page
	uint32_t max_vertices = 0;    // per page capacity in vertices

	VmaVirtualBlock index_blk;         // bytes (as you already do)
	VmaVirtualBlock vertex_units_blk;  // vertices (NEW)
};

struct DrawRange {
	GeometryPageHandle page;

	uint32_t index_count;

	// Either:
	uint32_t first_index;   // if IB bound at 0 for that page
	int32_t  base_vertex;   // if VB bound at 0 for that page

	// OR if you allow rebinding offsets:
	VkDeviceSize ib_byte_offset;
	VkDeviceSize vb0_byte_offset;
	VkDeviceSize vb1_byte_offset;
};

struct GpuMeshDesc {

	glm::vec3 aabbMin;
	float     sphereRadius;
	glm::vec3 aabbMax;
	float     spherePad;

	uint32_t lods = 1;
	std::vector<uint32_t> lod_offsets;
	std::vector<DrawRange> sets;

};

// For Upload geometry context. we can distighuis between geo for eviction/streaming purposes. maybe other
typedef enum GeoFlag {
} GeoFlag;


typedef enum PipelineType {
	PIPELINE_UNDEFINED = 0,
	PIPELINE_MATERIAL = 1 << 0,
	PIPELINE_SHADOW = 1 << 2
} PipelineType;

struct Pipeline {
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

	PipelineType type = PIPELINE_UNDEFINED;

};
// MatModels or MatPipelines can have several instances with different vlaues for constans and textures.
// Raster state, vert, frag, blending and more stay the same.
struct MaterialInstance {
	PipelineHandle pipeline;
	MaterialPushConstants values = {};
};

// Lets forget about validation/ typing in any way. No depth/color, sample/write specialization yet in any shape or form.
struct Image {

	VkImage image;
	VmaAllocation alloc;
	VkFormat format;
	VkImageUsageFlags usage;
	uint32_t arrayLayers; // DepthImage will have each cascae in diff index.
	uint32_t levels; // levels is Mips.
	uint32_t width, height;
	uint32_t depth = 1; // lets just initialize this to 1. Used for volume textures. 
	//VK_IMAGE_TYPE_3D

	// Layout state cant be encoded here since it depends on the pipeline..
	// This is state, the rest is properties. Thats the core problem. 
	// Leaving this here to avoid future tentation.
	//VkImageLayout layout;

};

struct ShadowMapSlot {
	ShadowMapHandle perFrame[MAX_FRAMES_IN_FLIGHT];
};

struct ShadowMap {
	uint32_t cascades = 0;
	ImageHandle image;
	SamplerHandle sampler;
	bool owns_sampler = true;
	ImageViewHandle sampleView;
	ImageViewHandle renderView;
	// if image has layers, for render attachment you need unambiguous view. thats the why of the vector
//	std::vector<ImageViewHandle> renderViews; 

	bool is_atlas() {
		return this->cascades >= 1;
	}
};

// This style of handling things is lifted up from LightweightVk.
class Context {

	VkInstance* instance_;
	// This is for ktx texture operations.
	// We could lift the concept for context. instead of habing members for device, physDevice and else
	ktxVulkanDeviceInfo* ktx_vdi = NULL;

	// These three things are primary citizens of Device Memory Residency
	VmaPool D_2DImagesPool;
	VmaPool D_ShaderPool;

	// Different pools for differnet uses: ...Skinned. Static. Dynamic
	//VmaPool D_geo;
	//VmaPool D_DynamicMeshPool;

	VmaPool D_StagingPool;

public:


	// This will go in the fututre
	uint32_t vertex_cursor = 0;

	std::unordered_map<std::string, SamplerHandle> samplers;

	VmaAllocator allocator;
	VmaAllocator* allocator_;
	VkDevice* device_;
	VkPhysicalDevice* physicalDevice_;

	// lets just go monolithic with this for now. no skinned/ dynamic/ static distinctions yet
	VmaVirtualBlock vertex_blk;
	VmaVirtualBlock index_blk;
	VmaVirtualBlock vertex_shading_blk;

	// Default geo
	DeviceBufferHandle vertex_buffer;
	DeviceBufferHandle vertex_shading_buffer;
	DeviceBufferHandle index_buffer;

	// SKINNED 
	//DeviceBufferHandle skinned_vertex_buffer;
	//DeviceBufferHandle skinned_index_buffer;

	// MESHLETS
	DeviceBufferHandle meshlet_vertex_buffer;
	DeviceBufferHandle meshlet_triangle_buffer;
	DeviceBufferHandle meshlet_descriptions_buffer;

	// This is a cpu side manager for allocations so I dont have to manage the allocations myself. Im lazy :P
	//VmaVirtualBlock mehslet_vertex_blk;
	//VmaVirtualBlock meshlet_triangle_blk;
	//VmaVirtualBlock neshlet_descriptions_blk;

	// -------------------------------------

	DescriptorPoolHandle default_descriptor_pool;
	DescriptorPoolHandle imgui_descriptor_pool;
	DescriptorLayoutHandle default_descriptorLayout;

	// Added for shadowmaps.
	DescriptorLayoutHandle default_shadow_descriptorLayout;

	// Added for meshlets.
	DescriptorLayoutHandle default_meshlet_descriptorLayout;

	//PipelineHandle default_material_pipeline;

	VkCommandPool default_command_pool;
	VkQueue default_queue;
	VkQueue transfer_queue;

	Pool<BufferDescription, BufferDescription> buffers_;
	Pool<Texture, Texture> textures_;

	Pool<GpuMeshDesc, GpuMeshDesc> meshes_;
	Pool<Image, Image> images_;
	Pool<ImageView, VkImageView> imageViews_;
	Pool<ShadowMap, ShadowMap> shadowmaps_;
	Pool<Sampler, VkSampler> samplers_;

	Pool < GeometryPage, GeometryPage> geometry_pages;
	GeometryPageHandle current_geometry_page;

	Pool<DescriptorSet, DescriptorSet> descriptorSets_;
	Pool<DescriptorPool, VkDescriptorPool> descriptorSetPools_;

	Pool<DescriptorLayout, VkDescriptorSetLayout> descriptorSetLayouts_;
	Pool<Pipeline, Pipeline> pipelines_;
	Pool<MaterialInstance, MaterialInstance> materialInstances_;

	Pool<ShaderModule, VkShaderModule> shaderModules_;

	Context(VkInstance& instance, VkDevice* device, VkPhysicalDevice* physicalDevice, VkSurfaceKHR surface) {

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

		res = vmaCreateAllocator(&allocatorCreateInfo, &allocator);
		assert(res == VK_SUCCESS);

		allocator_ = &allocator;
		device_ = device;
		physicalDevice_ = physicalDevice;

		VkSampler default_sampler;

		// from ChatGpt, may not be true: you can set a generic sampler with a big maxLod like 100, and reuse it for all your textures of any mip count.
		// The driver will clamp dynamically per texture.
		samplers["default"] = CreateSampler(100);

		// Samplers are driver objs. can be copied like this np.
		samplers["default_shadow"] = CreateShadowSampler(1);

		// Lets reserve upfront.
		textures_.objects_.reserve(MAX_TEXTURES);

		//materialPipelinePool_.objects_.reserve(1000);
		// This is a small reservation. just testing 

		buffers_.objects_.reserve(MAX_BUFFERS);

		InitializeDeviceVmaPools();
		InitalizeGeoBuffers();

		// Lets have one dpool
		default_descriptor_pool = createDescriptorPool();
		imgui_descriptor_pool = createDescriptorPool();

		default_descriptorLayout = createMainViewDescriptorLayout();
		//default_shadow_descriptorLayout = CreateShadowMapsDescriptorSetLayout();
		//default_meshlet_descriptorLayout = CreateMeshletDescriptorLayout();

		QueueFamilyIndices indices = findQueueFamilies(*physicalDevice, surface);

		VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = indices.graphicsFamily.value()
		};

		vkGetDeviceQueue(*device_, indices.graphicsFamily.value(), 0, &default_queue);

		// This should pick transfer over graphics
		vkGetDeviceQueue(*device_, indices.graphicsFamily.value(), 0, &transfer_queue);

		for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
			if (vkCreateCommandPool(*device_, &poolInfo, nullptr, &default_command_pool) !=
				VK_SUCCESS) {
				throw std::runtime_error("failed to create graphics command pool!");
			}
		}

		ktx_vdi = ktxVulkanDeviceInfo_Create(
			*physicalDevice_,
			*device_,
			transfer_queue, // this maybe should be transfer queue. we only use ktx to upload tex. no blitting for mipmaps, which are generated offline.
			default_command_pool,
			NULL
		);

	};
	// Maybe destructor should clear the vmaPools. Device side allocations are not managed as clear by the OS.
	~Context(
	) {
		CleanUp();
	};

	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
		copyBuffer(srcBuffer, dstBuffer, size, 0, 0);
	}

	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset) {

		VkCommandBuffer commandBuffer = beginSingleTimeCommands(*device_, default_command_pool);

		VkBufferCopy copyRegion{};
		copyRegion.size = size;
		copyRegion.srcOffset = srcOffset;
		copyRegion.dstOffset = dstOffset;
		vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

		endSingleTimeCommands(*device_, commandBuffer, default_queue, default_command_pool);
	}


	void InitalizeGeoBuffers() {

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

		//{

		//	// this all should be calculated on a MAX_MESHLETS or something.
		//	uint32_t totalVertexBytes = 512u * 1024u * 1024u;
		//	uint32_t totalTriangleBytes = 512u * 1024u * 1024u;
		//	uint32_t totalDescriptionsBytes = 512u * 1024u * 1024u;

		//	this->meshlet_vertex_buffer = createBuffer(
		//		totalVertexBytes,
		//		VMA_MEMORY_USAGE_GPU_ONLY,
		//		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		//		VK_BUFFER_USAGE_TRANSFER_DST_BIT
		//		//				VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT   // optional, recommended
		//	);

		//	this->meshlet_triangle_buffer = createBuffer(
		//		totalTriangleBytes,
		//		VMA_MEMORY_USAGE_GPU_ONLY,
		//		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		//		VK_BUFFER_USAGE_TRANSFER_DST_BIT
		//		//				VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT   // optional, recommended
		//	);

		//	// We need the mesh descriptions here tooo.

		//	this->meshlet_triangle_buffer = createBuffer(
		//		totalTriangleBytes,
		//		VMA_MEMORY_USAGE_GPU_ONLY,
		//		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		//		VK_BUFFER_USAGE_TRANSFER_DST_BIT
		//		//				VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT   // optional, recommended
		//	);

		//	VmaVirtualBlockCreateInfo cv{};
		//	cv.size = totalVertexBytes;
		//	VK_CHECK(vmaCreateVirtualBlock(&cv, &meshlet_triangle_blk));

		//	VmaVirtualBlockCreateInfo ci{};
		//	ci.size = totalTriangleBytes;
		//	VK_CHECK(vmaCreateVirtualBlock(&ci, &mehslet_vertex_blk));

		//	VmaVirtualBlockCreateInfo cd{};
		//	cd.size = totalDescriptionsBytes;
		//	VK_CHECK(vmaCreateVirtualBlock(&cd, &neshlet_descriptions_blk));

		//}


	}

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

		//// DEVICE-LOCAL pool for static geometry buffers
		//{
		//	VkBufferCreateInfo bufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		//	bufCI.size = 64 * 1024; // dummy
		//	bufCI.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		//	VmaAllocationCreateInfo aCI{};
		//	aCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

		//	uint32_t typeIndex = UINT32_MAX;
		//	VK_CHECK(vmaFindMemoryTypeIndexForBufferInfo(*allocator_, &bufCI, &aCI, &typeIndex));

		//	VmaPoolCreateInfo pci{};
		//	pci.memoryTypeIndex = typeIndex;
		//	pci.blockSize = 64 * 1024 * 1024;
		//	VK_CHECK(vmaCreatePool(*allocator_, &pci, &D_geo));
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
			vmaFindMemoryTypeIndexForBufferInfo(*allocator_, &bufCI, &aCI, &typeIndex);

			VmaPoolCreateInfo pci{};
			pci.memoryTypeIndex = typeIndex; // host-visible & host-coherent type
			pci.blockSize = 32 * 1024 * 1024;
			VK_CHECK(vmaCreatePool(*allocator_, &pci, &D_StagingPool));
		}


	}

	// RESOURCE CREATION

	DeviceBufferHandle CreateStagingBuffer(VkDeviceSize size);
	DeviceBufferHandle CreateStagingBuffer(const void* srcData, VkDeviceSize size);
	ShaderModuleHandle CreateShaderModule(std::vector<char> code);

	PipelineHandle CreatePipeline(RasterSettings raster_settings, ShaderModuleHandle vert, ShaderModuleHandle frag, VkFormat depth_format, VkFormat swapchainImageFormat);
	PipelineHandle CreateShadowPipeline(ShaderModuleHandle vert);
	PipelineHandle CreateMaterialPipeline(ShaderModuleHandle vert, ShaderModuleHandle frag, RasterSettings raster_settings, VkFormat depth_format, VkFormat swapchainImageFormat);
	ImageHandle CreateShadowMapImage(int texWidth, int texHeight);

	ImageHandle CreateImage(int texWidth, int texHeight, VkFormat format, VkImageUsageFlags usage, uint32_t levels, uint32_t arrayLayers);
	SamplerHandle CreateSampler(uint32_t levelCount);
	SamplerHandle CreateShadowSampler(uint32_t levelCount);
	TextureHandle CreateTexture(ktxTexture2& texture);
	ShadowMapHandle CreateShadowMap(int width, int height);

	MaterialInstanceHandle CreateMaterialInstance(PipelineHandle mat_pipeline);


	// DescriptorLayouts
	DescriptorLayoutHandle CreateTextureTableLayout();
	DescriptorLayoutHandle createMainViewDescriptorLayout();
	//DescriptorLayoutHandle CreateShadowMapsDescriptorSetLayout();
	//DescriptorLayoutHandle CreateMeshletDescriptorLayout();

	DescriptorPoolHandle createDescriptorPool();

	ImageViewHandle CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, int levelCount, int baseArrayLayer, int layerCount, VkImageViewType viewType);
	ImageViewHandle CreateImageView(ImageHandle image, VkImageAspectFlags aspectFlags, int levelCount, int baseArrayLayer, int layerCount, VkImageViewType viewType);
	ImageViewHandle CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, int levelCount);

	DescriptorSetHandle createDescriptorSet(DescriptorLayoutHandle lyt);
	DeviceBufferHandle createBuffer(uint32_t size, VmaMemoryUsage vma_usage, VkBufferUsageFlags usage_flags);

	void UploadDataToBuffer(DeviceBufferHandle buffer, void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAcces);
	void UploadDataToBuffer(DeviceBufferHandle buffer, void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAccess, size_t src_offset, size_t dest_offset);

	MeshDescHandle UploadVertexData(MeshUploadInput& mesh_data);

	VkCommandBuffer GetCommandBuffer();
	void SubmitCommandBuffer(VkCommandBuffer cmdBuf, VkCommandPool pool, VkQueue queue);




	//
	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);


	void TransitionImageLayout(ImageHandle image_handle, VkImageLayout oldLayout, VkImageLayout newLayout);

	VkQueue GetGraphicsQueue();
	void GetAppropiateSampler(Texture& texture);

	// END
	void CleanUp();

};

[[nodiscard]] SamplerHandle Context::CreateSampler(uint32_t levelCount) {

	VkSampler sampler_;
	createSampler(*physicalDevice_, *device_, sampler_, levelCount, VK_COMPARE_OP_ALWAYS);

	return samplers_.create(std::move(sampler_));

}

[[nodiscard]] SamplerHandle Context::CreateShadowSampler(uint32_t levelCount) {

	VkSampler sampler_;

	VkSamplerCreateInfo sci{};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;

	sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

	sci.compareEnable = VK_TRUE;
	sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

	VkSampler shadowCmpSampler;
	vkCreateSampler(*device_, &sci, nullptr, &shadowCmpSampler);

	VK_CHECK(vkCreateSampler(*device_, &sci, nullptr, &sampler_));

	return samplers_.create(std::move(sampler_));

}

[[nodiscard]] ImageViewHandle  Context::CreateImageView(
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	int levelCount
) {

	return CreateImageView(
		image,
		format,
		aspectFlags,
		levelCount,
		0,
		1,
		VK_IMAGE_VIEW_TYPE_2D // Lets default to this. im only using 2D arrays for cascades in shadow perFrame for the moment
	);

}


[[nodiscard]] ImageViewHandle  Context::CreateImageView(
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	int levelCount,
	int baseArrayLayer,
	int layerCount,
	VkImageViewType viewType
) {

	VkImageView imageView;

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

	VK_CHECK(vkCreateImageView(*device_, &viewInfo, nullptr, &imageView));

	return imageViews_.create(std::move(imageView));


}

[[nodiscard]] ImageViewHandle  Context::CreateImageView(
	ImageHandle image,
	VkImageAspectFlags aspectFlags,
	int levelCount,
	int baseArrayLayer,
	int layerCount,
	VkImageViewType viewType
) {

	auto img = images_.get(image);

	return CreateImageView(
		img->image,
		img->format,
		aspectFlags,
		levelCount,
		baseArrayLayer,
		layerCount,
		viewType
	);

}


[[nodiscard]] DescriptorSetHandle Context::createDescriptorSet(
	DescriptorLayoutHandle lyt
) {
	// Pools should be rolled over on fail, get new pool. then discard FIFO
	auto descriptorPool = descriptorSetPools_.get(default_descriptor_pool);
	auto layout = descriptorSetLayouts_.get(lyt);

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
	DeviceBufferHandle buffer,
	void* data,
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
	void* data,
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


	endSingleTimeCommands(*device_, tmpBuf, default_queue, default_command_pool);

	// we could maybe delay this
	vmaDestroyBuffer(*allocator_, stagingBuf, stagingAlloc);
}


// I shold update this to not be a monolithic block instead resue the mainView framering buffers layout if useful 
//[[nodiscad]] DescriptorLayoutHandle Context::CreateShadowMapsDescriptorSetLayout() {
//	// INDEX DATA
//	VkDescriptorSetLayoutBinding instanceIndexBinding{};
//	instanceIndexBinding.binding = 0;
//	instanceIndexBinding.descriptorCount = 1;
//	instanceIndexBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
//	instanceIndexBinding.pImmutableSamplers = nullptr;
//	instanceIndexBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
//
//	VkDescriptorSetLayoutBinding instanceDataBinding{};
//	instanceDataBinding.binding = 1;
//	instanceDataBinding.descriptorCount = 1;
//	instanceDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
//	instanceDataBinding.pImmutableSamplers = nullptr;
//	instanceDataBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
//
//	VkDescriptorSetLayoutBinding lightBinding{};
//	lightBinding.binding = 2;
//	lightBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
//	lightBinding.descriptorCount = 1;
//	lightBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
//
//	// ShadowMetaUB
//	VkDescriptorSetLayoutBinding shadowMetaBinding{};
//	lightBinding.binding = 3;
//	lightBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
//	lightBinding.descriptorCount = 1;
//	lightBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
//
//	std::array<VkDescriptorSetLayoutBinding, 3> bindings = {
//		instanceIndexBinding,
//		instanceDataBinding,
//		lightBinding,
//		//		shadowMetaBinding
//	};
//
//	// Per-binding flags — same size as bindings[]
//	//std::array<VkDescriptorBindingFlags, 3> bindingFlags = {
//	//	0,// instanceUBO
//	//	0,// instanceSSBO
//	//	0 // lightData.
//	//};
//
//	//VkDescriptorSetLayoutBindingFlagsCreateInfo bindless_flags{};
//	//bindless_flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
//	//bindless_flags.pNext = nullptr;
//	//bindless_flags.bindingCount = static_cast<uint32_t>(bindingFlags.size());
//	//bindless_flags.pBindingFlags = bindingFlags.data();
//
//	VkDescriptorSetLayoutCreateInfo layoutInfo{};
//	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
//	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
//	layoutInfo.pBindings = bindings.data();
//	//layoutInfo.pNext = &bindless_flags;
//	//layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
//
//	VkDescriptorSetLayout descriptorSetLayout;
//
//	if (vkCreateDescriptorSetLayout(*device_, &layoutInfo, nullptr,
//		&descriptorSetLayout) != VK_SUCCESS) {
//		throw std::runtime_error("failed to create descriptor set layout!");
//	}
//
//	return descriptorSetLayouts_.create(std::move(descriptorSetLayout));
//}


// Could be a good model

//struct BindingDescription {
//	VkDescriptorType type;
//	VkDescriptorBindingFlags bindingFlags;
//	//std::vector<VkSampler> inmutableSamplers;
//};
//
//struct BindingDescriptionBound {
//	int layuout_n, bind_n = 0;
//};
//
//
//struct PipelineLayoutDescription {
//	//layouts;
//	std::vector<BindingDescription> bindings;
//
//};
//
//
//
//[[nodiscard]] DescriptorLayoutHandle Context::createDescriptorLayout(PipelineLayoutDescription& pld) {
//
//
//
//}



[[nodiscard]] DescriptorLayoutHandle Context::CreateTextureTableLayout() {



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


	std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
		sampledImageBinding,
		sampBinding,
	};

	// Per-binding flags — same size as bindings[]
	std::array<VkDescriptorBindingFlags, 2> bindingFlags = {
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // bindless textures
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // bindless samplers
	};

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

	return descriptorSetLayouts_.create(std::move(descriptorSetLayout));


}





[[nodiscard]] DescriptorLayoutHandle Context::createMainViewDescriptorLayout() {

	VkDescriptorSetLayoutBinding frameUniformsBinding{};
	frameUniformsBinding.binding = 0;
	frameUniformsBinding.descriptorCount = 1;
	frameUniformsBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	frameUniformsBinding.pImmutableSamplers = nullptr;
	frameUniformsBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutBinding instanceDataBinding{};
	instanceDataBinding.binding = 1;
	instanceDataBinding.descriptorCount = 1;
	instanceDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
	instanceDataBinding.pImmutableSamplers = nullptr;
	instanceDataBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
		frameUniformsBinding,
		instanceDataBinding,
		//sampledImageBinding,
		//sampBinding,
		//lightBinding,
		//shadowMapBinding,
		//shadowSamp
	};

	// Per-binding flags — same size as bindings[]
	std::array<VkDescriptorBindingFlags, 2> bindingFlags = {
		0,
		0,
		//VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		//VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // bindless textures
		//VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		//VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // bindless samplers
		//0, // lightData.
		//VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		//VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // bindless shadowmap_images
		//VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		//VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // bindless shadowmap_samplers
	};

	// Chain flags struct
	//VkDescriptorSetLayoutBindingFlagsCreateInfo bindless_flags{};
	//bindless_flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	//bindless_flags.pNext = nullptr;
	//bindless_flags.bindingCount = static_cast<uint32_t>(bindingFlags.size());
	//bindless_flags.pBindingFlags = bindingFlags.data();

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	layoutInfo.pNext = VK_NULL_HANDLE;
	//layoutInfo.pNext = &bindless_flags;
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

	VkDescriptorSetLayout descriptorSetLayout;

	if (vkCreateDescriptorSetLayout(*device_, &layoutInfo, nullptr,
		&descriptorSetLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create descriptor set layout!");
	}

	return descriptorSetLayouts_.create(std::move(descriptorSetLayout));

}

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
	VK_CHECK(vkCreateDescriptorPool(*device_, &poolInfo, nullptr, &dpool));
	return descriptorSetPools_.create(std::move(dpool));
}

[[nodiscard]] PipelineHandle Context::CreateShadowPipeline(ShaderModuleHandle vert) {

	// We ure using native VkPipelineLayout to ease things for now so we dont have to formalize a way to describe that.
	Pipeline pipeline = {};
	//pipeline.raster_settings = default_raster_settings;
	//pipeline.frag = ;
	pipeline.vert = vert;
	pipeline.type = PIPELINE_SHADOW;

	// 16 bit depth
	constexpr float depthBiasConstant = 0; // lets try  FFXVVI oriented depth bias
	constexpr float depthBiasSlope = 1.0f;

	auto v = shaderModules_.get(vert);
	VkPipelineShaderStageCreateInfo vertStage{};
	vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	vertStage.module = *v;
	vertStage.pName = "mainVS";

	VkPipelineShaderStageCreateInfo shaderStages[] = { vertStage };

	//VERTEX Duplicated from CreatePipeline... this needs updating too if it were to change.

	auto bindingDescription = Vertex::getBindingDescription();
	auto attributeDescriptions = Vertex::getAttributeDescriptions();

	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	vertexInput.vertexBindingDescriptionCount = bindingDescription.size();
	vertexInput.vertexAttributeDescriptionCount =
		static_cast<uint32_t>(attributeDescriptions.size());
	vertexInput.pVertexBindingDescriptions = bindingDescription.data();
	vertexInput.pVertexAttributeDescriptions = attributeDescriptions.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType =
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	// 3. Viewport & scissor (or mark as dynamic)
	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	//viewportState.pViewports = &viewport; // or null if dynamic // We are going dynamic
	viewportState.scissorCount = 1;
	//viewportState.pScissors = &scissor;  // or null if dynamic

	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_BACK_BIT; 
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

	raster.depthClampEnable = VK_FALSE; // Need to enable feature for VK_TRUE
	raster.rasterizerDiscardEnable = VK_FALSE;

	raster.depthBiasEnable = VK_TRUE;                // usually TRUE for shadow acne fix
	raster.depthBiasConstantFactor = depthBiasConstant;
	raster.depthBiasSlopeFactor = depthBiasSlope;
	raster.lineWidth = 1.0f;

	// 5. Multisampling
	VkPipelineMultisampleStateCreateInfo ms{};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	ms.sampleShadingEnable = VK_FALSE;

	// 6. Depth-stencil
	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;

	// Changed 
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	//depthStencil.depthTestEnable = VK_TRUE;
	//depthStencil.depthWriteEnable = VK_TRUE;
	//depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

	// 7. Color blend: **no color attachments**
	VkPipelineColorBlendStateCreateInfo colorBlend{};
	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = 0;
	colorBlend.pAttachments = nullptr;

	std::vector<VkDynamicState> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};

	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount =
		static_cast<uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();


	// DYNAMIC RENDERING INFO.

	// Provide information for dynamic rendering
	VkPipelineRenderingCreateInfo dynamic_rendering_pipeline_info{
		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	dynamic_rendering_pipeline_info.pNext = VK_NULL_HANDLE;

	dynamic_rendering_pipeline_info.depthAttachmentFormat = SHADOWMAP_DEPTH_FORMAT;
	dynamic_rendering_pipeline_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;


#pragma region ---[Push Constants]---


	VkPushConstantRange range{};
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	range.offset = 0;
	range.size = sizeof(MainViewPushConstants); // I dont need materialInstance pc for now. Maybe later ill do alphatested mats for non opaque geo

	auto shadow_def_lyt = descriptorSetLayouts_.get(default_shadow_descriptorLayout);
	auto descriptorSetLayout = shadow_def_lyt;

	VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = descriptorSetLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &range;

	if (vkCreatePipelineLayout(*device_, &layoutInfo, nullptr,
		&pipeline.plyt) != VK_SUCCESS) {
		throw std::runtime_error("failed to create pipeline layout!");
	}

#pragma endregion

	// 9. Finally, the pipeline create info
	VkGraphicsPipelineCreateInfo pipeCI{};
	pipeCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeCI.stageCount = 1;                          // <— ONLY vertex stage
	pipeCI.pStages = shaderStages;

	pipeCI.pVertexInputState = &vertexInput;
	pipeCI.pInputAssemblyState = &inputAssembly;
	pipeCI.pViewportState = &viewportState;
	pipeCI.pRasterizationState = &raster;
	pipeCI.pMultisampleState = &ms;
	pipeCI.pDepthStencilState = &depthStencil;
	pipeCI.pColorBlendState = &colorBlend;
	pipeCI.pDynamicState = &dynamicState;

	pipeCI.layout = pipeline.plyt;
	pipeCI.subpass = 0;
	pipeCI.pNext = &dynamic_rendering_pipeline_info;


	assert(pipeCI.pMultisampleState != nullptr);

	std::cout << "creating Shadow Pipeline" << std::endl;
	vkCreateGraphicsPipelines(*device_, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &pipeline.pipeline);
	std::cout << "end creating shadow pipeline" << std::endl;


	return pipelines_.create(std::move(pipeline));

}

//// Type specialization
////using uint32_t = BindingType;
//
//
//struct Bindign {
//	uint32_t type :
//};
//struct PipelineLayout {
//
//	// PC
//	std::vector<struct dummy> push_constants_block;
//
//	// SET LAYOUTS
//	// dynamics first
//};


[[nodiscard]] PipelineHandle Context::CreatePipeline(
	RasterSettings raster_settings,
	ShaderModuleHandle vert,
	ShaderModuleHandle frag,
	VkFormat depth_format,
	VkFormat swapchainImageFormat
) {

	// We ure using native VkPipelineLayout to ease things for now so we dont have to formalize a way to describe that.
	Pipeline pipeline = {};
	pipeline.raster_settings = default_raster_settings;
	pipeline.frag = frag;
	pipeline.vert = vert;
	pipeline.type = PIPELINE_MATERIAL;

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

	// VERTEX FORMAT

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	auto bindingDescription = Vertex::getBindingDescription();
	auto attributeDescriptions = Vertex::getAttributeDescriptions();

	vertexInputInfo.vertexBindingDescriptionCount = bindingDescription.size();
	vertexInputInfo.vertexAttributeDescriptionCount =
		static_cast<uint32_t>(attributeDescriptions.size());
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
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

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

#pragma region ---[Push Constants]---

	VkPushConstantRange range{};
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	range.offset = 0;
	range.size = sizeof(MainViewPushConstants) + sizeof(MaterialPushConstants);;

	std::array<VkDescriptorSetLayout, 2> layouts = {
		rings_layout, 
		texture_table_layout 
	};

	VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	
	layoutInfo.setLayoutCount = layouts.size();
	layoutInfo.pSetLayouts = layouts.data();

	//layoutInfo.pSetLayouts = descriptorSetLayout;

	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &range;

	if (vkCreatePipelineLayout(*device_, &layoutInfo, nullptr,
		&pipeline.plyt) != VK_SUCCESS) {
		throw std::runtime_error("failed to create pipeline layout!");
	}

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
	pipelineInfo.layout = pipeline.plyt;
	pipelineInfo.pNext = &dynamic_rendering_pipeline_info;

	if (vkCreateGraphicsPipelines(*device_, VK_NULL_HANDLE, 1, &pipelineInfo,
		nullptr, &pipeline.pipeline) != VK_SUCCESS)
	{
		throw std::runtime_error("failed to create graphics pipeline!");
	}

	return pipelines_.create(std::move(pipeline));
}

[[nodiscard]] PipelineHandle Context::CreateMaterialPipeline(
	ShaderModuleHandle vert,
	ShaderModuleHandle frag,
	RasterSettings raster_settings,
	VkFormat depth_format,
	VkFormat swapchainImageFormat
) {

	return CreatePipeline(raster_settings, vert, frag, depth_format, swapchainImageFormat);

}

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

void Context::GetAppropiateSampler(Texture& texture) {


	//VkSampler _sampler;
	//createSampler(*physicalDevice_, *device_, _sampler);
	// We should
	// uld try to define here which sampler would work for this texture. Sepecialization.

	auto sh = samplers["default"];
	texture.sampler = *samplers_.get(sh);

};

// At this moment the only texture input supported is ktxTextures. This is a big LIMITATION/HOLE
// 
// The complete reuqirements specification/nomenclatures are 
// not yet formalized for this toy renderer. That will need to come later.
[[nodiscard]] TextureHandle Context::CreateTexture(ktxTexture2& texture) {

	if (ktxTexture2_NeedsTranscoding(&texture)) {
		//auto target = pickTarget(*physicalDevice_);

		// This is hardcoded to BC7. it is not appropiate for other than diffuse.
		KTX_error_code tr = ktxTexture2_TranscodeBasis(&texture, KTX_TTF_BC7_RGBA, 0);
		if (tr != KTX_SUCCESS) throw std::runtime_error(ktxErrorString(tr));
	}

	// they should be generated with the py script
	texture.generateMipmaps = KTX_FALSE;

	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_SAMPLED_BIT |           // <-- required for sampling
		VK_IMAGE_USAGE_TRANSFER_DST_BIT |      // <-- required for the copy
		(texture.generateMipmaps ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0); // for blit chain. this needs linear format.

	ktxVulkanTexture vk_tex{};
	KTX_error_code kr = ktxTexture2_VkUploadEx(
		&texture,
		ktx_vdi,
		&vk_tex,
		VK_IMAGE_TILING_OPTIMAL,
		usage,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);
	if (kr != KTX_SUCCESS) {

		VkFormat fmt = ktxTexture2_GetVkFormat(&texture);
		char msg[256];
		snprintf(msg, sizeof(msg), "ktxTexture2_VkUploadEx failed (%s), VkFormat=%d, %ux%u, levels=%u, layers=%u",
			ktxErrorString(kr), (int)fmt, texture.baseWidth, texture.baseHeight,
			texture.numLevels, texture.numLayers);
		throw std::runtime_error(msg);
	}

	// 3) Build your view/sampler from the format actually created.
	Texture ctx_tex{};
	ctx_tex.image = vk_tex.image;

	auto aspect = (vk_tex.imageFormat == VK_FORMAT_D16_UNORM || vk_tex.imageFormat == VK_FORMAT_D32_SFLOAT ||
		vk_tex.imageFormat == VK_FORMAT_X8_D24_UNORM_PACK32)
		? VK_IMAGE_ASPECT_DEPTH_BIT
		: (vk_tex.imageFormat == VK_FORMAT_S8_UINT)
		? VK_IMAGE_ASPECT_STENCIL_BIT
		: (vk_tex.imageFormat == VK_FORMAT_D24_UNORM_S8_UINT || vk_tex.imageFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
		? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
		: VK_IMAGE_ASPECT_COLOR_BIT;

	// Lets do this for the moment. DEPTH is 
	assert(aspect == VK_IMAGE_ASPECT_COLOR_BIT);

	auto view_handle = CreateImageView(ctx_tex.image, vk_tex.imageFormat, aspect, texture.numLevels);
	ctx_tex.imageView = *imageViews_.get(view_handle);

	GetAppropiateSampler(ctx_tex);

	// Later, when destroying:
	// ktxVulkanTexture_Destruct(&vk_tex, device_, nullptr);

	return textures_.create(std::move(ctx_tex));
}

[[nodiscard]] ShadowMapHandle Context::CreateShadowMap(int width = 1024, int height = 1024)
{
	ShadowMap sm;
	sm.cascades = 0;
	auto image = CreateShadowMapImage(width, height);
	sm.image = image;

	auto arrayLayers = 1;
	VkImageViewType viewType = (arrayLayers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;

	auto view = CreateImageView(
		image,
		//VK_FORMAT_D32_SFLOAT, // Copied from CreateCascadedShadowMapsDepthImage. Need to checlk froamt availabilty and track it, probably...,
		VK_IMAGE_ASPECT_DEPTH_BIT,
		1,
		0,
		1,
		viewType

	);

	sm.renderView = view;
	sm.sampleView = view;

	// These are leftovers from 2D_ARRAY implementation for cascasdes.
	// Moved to atlas images instead for per-cascde resolution flexibility reasons. 

	//auto image = images_.get(sm.image);
	//// These are the individual views for use as depth render attachment in the pipeline.
	//for (int i = 0; i < sm.cascades; i++) {
	//	//sm.renderViews.push_back(
	//	//	view
	//	//);

	//}
	// We also need the sample view for shader use. 
	//	sm.sampleView = CreateImageView(
	//		image,
	//		VK_IMAGE_ASPECT_DEPTH_BIT,
	//		1, // levels = MIPS
	//		0,
	//		1,
	////		sm.cascades, // layerCount. Layers = ARRAY
	//		VK_IMAGE_VIEW_TYPE_2D_ARRAY
	//	);

	return shadowmaps_.create(std::move(sm));

}

[[nodiscard]] ImageHandle Context::CreateShadowMapImage(int texWidth, int texHeight /*int cascades*/) {

	//VkFormat depth_format = VK_FORMAT_D32_SFLOAT; // I dont need stencil, so lets hardcode this for the moment here. /*findDepthFormat(*physicalDevice_);*/
	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT // sample as texture
		| VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	// Levels just 1. No mipmaps for shadow perFrame images. 
	ImageHandle image_handle = CreateImage(texWidth,
		texHeight, SHADOWMAP_DEPTH_FORMAT, usage, 1, 1);

	auto image = images_.get(image_handle);

	// I dont really care beucase first op will be loadClear
	//TransitionImageLayout(
	//	image_handle,
	//	VK_IMAGE_LAYOUT_UNDEFINED,
	//	VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
	//);

	//image->layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

	return image_handle;
};

// I don't like texHeight and texWidth with subscript tex. Images can be non textures...
// Should be cleaned up later.
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

	//	image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	image.format = format;
	image.levels = levels;
	image.width = texWidth;
	image.height = texHeight;
	// Lets store usage. 
	image.usage = usage;
	image.arrayLayers = arrayLayers;


	//VmaAllocation alloc = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo{};

	VkResult res = vmaCreateImage(*allocator_, &imgCI, &allocation_create_info, &image.image, &image.alloc, &allocInfo);
	if (res != VK_SUCCESS) {
		throw std::runtime_error("vmaCreateImage (in pool) failed");
	}

	return images_.create(std::move(image));

}


[[nodiscard]] MeshDescHandle Context::UploadVertexData(MeshUploadInput& geometry_data)
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
	// 5) Draw range (bind-once style)
	// ------------------------------------------
	DrawRange ds{};
	ds.page = current_geometry_page;
	ds.index_count = static_cast<uint32_t>(geometry_data.indices.size());
	ds.first_index = static_cast<uint32_t>(index_offset_bytes / sizeof(IndexT));
	ds.base_vertex = static_cast<int32_t>(baseVertex);

	// IMPORTANT: store allocs somewhere if you want to free later.
	// e.g. ds.vertex_alloc = vtx_alloc; ds.index_alloc = index_alloc; (requires fields)

	GpuMeshDesc mesh_dec{};
	mesh_dec.lods = 1;
	mesh_dec.lod_offsets = { 0 };
	mesh_dec.sets = { ds };

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

[[nodiscard]] DeviceBufferHandle Context::CreateStagingBuffer(const void* srcData,
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

[[nodiscard]] ShaderModuleHandle Context::CreateShaderModule(std::vector<char> code) {

	auto shader = createDeviceShaderModule(
		*device_,
		std::move(code)
	);

	return shaderModules_.create(std::move(shader));

};

VkQueue Context::GetGraphicsQueue() {
	return default_queue;
}

// DESTRUCTORS

void destroy(Context* ctx, ShaderModuleHandle handle) {
	assert(handle.valid());
	auto sm = ctx->shaderModules_.get(handle);
	vkDestroyShaderModule(*ctx->device_, *sm, NULL);
	ctx->shaderModules_.destroy(handle);
};
void destroy(Context* ctx, TextureHandle handle) {
	assert(handle.valid());
	auto tex = ctx->textures_.get(handle);

	// Lets not manage samplers just this soon. we have a default one with use for every texture rnow anyways.
	//if (tex->owns_sampler) {
	//	vkDestroySampler(*ctx->device_, tex->sampler, nullptr);
	//	// this is not needed its just a pattern for preventing use after free.
	//	//tex->sampler = VK_NULL_HANDLE;
	//}

	// Im pairing imageViews 1 to 1 with iamges. so lets free that
	vkDestroyImageView(*ctx->device_, tex->imageView, nullptr);
	vmaDestroyImage(*ctx->allocator_, tex->image, tex->alloc);

	ctx->textures_.destroy(handle);
}
void destroy(Context* ctx, ImageHandle handle)
{
}
void destroy(Context* ctx, DeviceBufferHandle handle) {
	assert(handle.valid());
	auto db = ctx->buffers_.get(handle);
	vmaDestroyBuffer(*ctx->allocator_, db->buffer, db->alloc);
	ctx->buffers_.destroy(handle);
};

// END

void Context::CleanUp() {

	// Clear Pools.
	// Clear VmaPools.
	//vmaDestroyVirtualBlock(index_blk);
	//vmaDestroyVirtualBlock(vertex_blk);
	//vmaDestroyVirtualBlock(vertex_shading_blk);
	//vmaDestroyPool();//
}
