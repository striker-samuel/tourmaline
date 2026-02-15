#pragma once





#include <Engine/Vulkan/vulkan_helpers.h>
#include <Engine/Renderer/constants.h>
#include <Engine/Renderer/primitives.h>
#include <Engine/Renderer/descriptions.h>

#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>

#include <ktx.h>
#include <ktxvulkan.h>

#include <algorithm>
#include <array>

static inline bool is_aligned4(uint32_t v) { return (v & 3u) == 0u; }


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
using PipelineLayoutHandle = Handle<struct PipelineLayout>;

void destroy(Context* ctx, ShaderModuleHandle handle);
void destroy(Context* ctx, DeviceBufferHandle handle);
void destroy(Context* ctx, TextureHandle handle);
void destroy(Context* ctx, ImageHandle handle);

#pragma region [--Resources--]

// This structs should be named less generic, somethign like ToyPipeline could help to notice
// this are part of my implementation surface and not Vulkans or other. 
struct Pipeline {
	ShaderModuleHandle vert;
	ShaderModuleHandle frag;
	DescriptorSetHandle ds;
	PipelineLayoutHandle plyt;

	VkPipeline pipeline;
	//maybe name

};

struct DescriptorSet {
	DescriptorLayoutHandle layout;
	VkDescriptorSet set;
};

struct PipelineLayout {
	VkPipelineLayout layout = VK_NULL_HANDLE;
	std::vector<DescriptorLayoutHandle> setLayouts; // keep handles for lifetime tracking
};

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


#pragma endregion

struct Texture {
// Mem	
	VmaAllocation alloc;
	VmaAllocationInfo allocInfo;

	VkDeviceMemory deviceMemory;
	VkImage image;
	VkImageView imageView;
	VkSampler sampler;
	VkFormat format;

	// Texture entry may own sampler. 
	bool owns_sampler;

};

// This ideally shouldnt use VkTypes and be the RHI interface.
// Lets roll with this for now
struct RasterSettings {
	uint32_t depthClampEnable = VK_FALSE;
	uint32_t rasterizerDiscardEnable = VK_FALSE;
	VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
	float lineWidth = 1.0f;
	uint32_t cullMode = VK_CULL_MODE_BACK_BIT;
	VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

	// Depth bias for shadowmaps.
	uint32_t depthBiasEnable = VK_FALSE;
	float depthBiasConstant = 0.0f;
	float depthBiasSlope = 1.0f; // This shoul be PCF radiues

};

// Buffer state.
struct BufferDescription {
	//uint32_t device_n = 0;
	VkBuffer buffer;
	VmaAllocation alloc;
	VmaAllocationInfo allocInfo;
	VkBufferUsageFlags usage;

	// Did it end in mappable memory? This is important to know for proper upload. Discard the bool in the future use 
	VkMemoryPropertyFlags memory_flags;
	// Helpers to interrogate buffer
	bool is_mapapble() const {
		return this->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	}

};

// 
struct SamplerKey {
	// Enough to dedupe VkSampler objects in your cache
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
	// + whatever else you actually use

	bool operator==(const SamplerKey&) const = default;
};

struct BindingDesc {
	uint32_t binding = 0;                 // binding number inside the set

	VkDescriptorType type{};
	uint32_t count = 1;                   // descriptorCount
	VkShaderStageFlags stages = 0;
	VkDescriptorBindingFlags flags = 0;   // PARTIALLY_BOUND, UPDATE_AFTER_BIND, etc.

	//std::vector<SamplerKey> immutableSamplers; // size must be == count if non-empty
	std::vector<SamplerHandle> immutableSamplers; // size must be == count if non-empty
};

struct DescriptorSetLayoutDesc {
	uint32_t set = 0;                     // set number in pipeline layout
	VkDescriptorSetLayoutCreateFlags createFlags = 0; // e.g. UPDATE_AFTER_BIND_POOL_BIT
	std::vector<BindingDesc> bindings;
};

struct PushConstantDesc {
	VkShaderStageFlags stages = 0;
	uint32_t offset = 0;
	uint32_t size = 0;
};

struct PipelineLayoutDesc {
	std::vector<DescriptorSetLayoutDesc> sets; // each has set index
	std::vector<PushConstantDesc> pushConstants;
};


struct GeometryPageRef {
	uint16_t page;      // which arena/page
	uint16_t flags;     // optional (layout, etc.)
};



#pragma region [--Descriptions/Keys--]


#pragma endregion

// This style of handling things is lifted up from LightweightVk.
class Context {

	VkInstance* instance_;
	ktxVulkanDeviceInfo* ktx_vdi = NULL;

	VmaPool D_2DImagesPool;
	VmaPool D_ShaderPool;
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

	// --------------------------------------

	DescriptorPoolHandle default_descriptor_pool;
	DescriptorPoolHandle imgui_descriptor_pool;

	VkCommandPool default_command_pool;

	// --------------------------------------

	VkQueue default_queue;
	VkQueue transfer_queue;

	// --------------------------------------

	Pool<BufferDescription, BufferDescription> buffers_;
	Pool<Texture, Texture> textures_;

	Pool<GpuMeshDesc, GpuMeshDesc> meshes_;
	Pool<Image, Image> images_;
	Pool<ImageView, VkImageView> imageViews_;
	Pool<ShadowMap, ShadowMap> shadowmaps_;
	Pool<Sampler, VkSampler> samplers_;
	Pool<PipelineLayout, PipelineLayout> pipelineLayouts_;

	Pool <GeometryPage, GeometryPage> geometry_pages;
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

	PipelineLayoutHandle createPipelineLayout(const PipelineLayoutDesc& descIn);
	PipelineHandle CreatePipeline(RasterSettings raster_settings, PipelineLayoutDesc layoutDesc, ShaderModuleHandle vert, ShaderModuleHandle frag, VkFormat depth_format, VkFormat swapchainImageFormat);

	ImageHandle CreateShadowMapImage(int texWidth, int texHeight);
	ImageHandle CreateImage(int texWidth, int texHeight, VkFormat format, VkImageUsageFlags usage, uint32_t levels, uint32_t arrayLayers);

	SamplerHandle CreateSampler(uint32_t levelCount);
	SamplerHandle CreateShadowSampler(uint32_t levelCount);


	TextureHandle CreateTexture(ktxTexture2& texture); // TextureSlotTable implementeation pendinde. 
	ShadowMapHandle CreateShadowMap(int width = 1024, int height = 1024);

	MaterialInstanceHandle CreateMaterialInstance(PipelineHandle mat_pipeline);
	DescriptorLayoutHandle createDescriptorLayout(const DescriptorSetLayoutDesc& descIn);

	DescriptorPoolHandle createDescriptorPool();
	ImageViewHandle CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, int levelCount, int baseArrayLayer, int layerCount, VkImageViewType viewType);
	ImageViewHandle CreateImageView(ImageHandle image, VkImageAspectFlags aspectFlags, int levelCount, int baseArrayLayer, int layerCount, VkImageViewType viewType);
	ImageViewHandle CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, int levelCount);

	DescriptorSetHandle createDescriptorSet(DescriptorLayoutHandle lyt);
	DeviceBufferHandle createBuffer(VkDeviceSize size, VmaMemoryUsage vma_usage, VkBufferUsageFlags usage_flags);

	void UploadDataToBuffer(DeviceBufferHandle buffer, void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAcces);
	void UploadDataToBuffer(DeviceBufferHandle buffer, void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAccess, size_t src_offset, size_t dest_offset);

	MeshDescHandle UploadVertexData(MeshUploadInput& mesh_data);

	VkCommandBuffer GetCommandBuffer();
	void SubmitCommandBuffer(VkCommandBuffer cmdBuf, VkCommandPool pool, VkQueue queue);

	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
	void TransitionImageLayout(ImageHandle image_handle, VkImageLayout oldLayout, VkImageLayout newLayout);

	SamplerHandle GetSampler(SamplerKey key);
	VkQueue GetGraphicsQueue();

	void CleanUp();

};
