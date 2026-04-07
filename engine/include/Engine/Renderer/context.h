#pragma once

#include <algorithm>
#include <array>

#include <Engine/Vulkan/vulkan_helpers.h>
#include <Engine/Renderer/constants.h>
#include <Engine/Renderer/primitives.h>
#include <Engine/Renderer/descriptions.h>
#include <Engine/Renderer/resources.h>

#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>

#include <ktx.h>
#include <ktxvulkan.h>



// This belongs here. 
// As it stands Context is also a geometry data manager.
struct GeometryPage {

	DeviceBufferHandle indexBuffer;
	DeviceBufferHandle vertexPosBuffer;
	DeviceBufferHandle vertexShadingBuffer;

	//	uint32_t vertex_cursor = 0;   // per page
	uint32_t max_vertices = 0;    // per page capacity in vertices

	VmaVirtualBlock index_blk;         // bytes (as you already do)
	VmaVirtualBlock vertex_units_blk;  // vertices (NEW)
};

struct GeometryPageRef {
	uint16_t page;      // which arena/page
	uint16_t flags;     // optional (layout, etc.)
};

// This resource management style is lifted from lightweightVk
class Context {

	VkInstance* instance_;
	ktxVulkanDeviceInfo* ktx_vdi = NULL;

	VmaPool D_2DImagesPool;
	VmaPool D_ShaderPool;
	VmaPool D_StagingPool;

	// This will go in the fututre
	uint32_t vertex_cursor = 0;


public:
	std::unordered_map<std::string, SamplerHandle> samplers;

	VmaAllocator allocator_;
	VkDevice device_;
	VkPhysicalDevice physicalDevice_;

	// lets just go monolithic with this for now. no skinned/ dynamic/ static distinctions yet
	VmaVirtualBlock vertex_blk;
	VmaVirtualBlock index_blk;
	VmaVirtualBlock vertex_shading_blk;

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

	GeometryPageHandle current_geometry_page;
	// --------------------------------------

	Pool<BufferDescription, BufferDescription> buffers_;
	Pool<Image, Image> images_;
	
	//Pool<ImageView, VkImageView> imageViews_; // We may not need this one.
	Pool<Sampler, VkSampler> samplers_;

	Pool<ShaderModule, VkShaderModule> shaderModules_;

	Pool<DescriptorSet, DescriptorSet> descriptorSets_;
	Pool<DescriptorLayout, VkDescriptorSetLayout> descriptorSetLayouts_;
	Pool<DescriptorPool, VkDescriptorPool> descriptorSetPools_;

	Pool<Pipeline, Pipeline> pipelines_;
	Pool<PipelineLayout, PipelineLayout> pipelineLayouts_;

	Pool<Geometry, Geometry> meshes_;
	Pool <GeometryPage, GeometryPage> geometry_pages;

	Pool<MaterialInstance, MaterialInstance> materialInstances_;

	Context(VkInstance& instance, VkDevice* device, VkPhysicalDevice* physicalDevice, VkSurfaceKHR surface);
	~Context(
	);
	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset);
	void InitalizeGeoBuffers();
	void InitializeDeviceVmaPools();

	//

	DeviceBufferHandle CreateStagingBuffer(VkDeviceSize size);
	DeviceBufferHandle CreateStagingBuffer(const void* srcData, VkDeviceSize size);
	ShaderModuleHandle CreateShaderModule(std::vector<char> code);

	PipelineLayoutHandle createPipelineLayout(const PipelineLayoutDesc& descIn);
	PipelineHandle CreatePipeline(RasterSettings raster_settings, PipelineLayoutDesc layoutDesc, ShaderModuleHandle vert, ShaderModuleHandle frag, VkFormat depth_format, VkFormat swapchainImageFormat);

	ImageHandle CreateShadowMapImage(int texWidth, int texHeight);
	ImageHandle CreateImage(int texWidth, int texHeight, VkFormat format, VkImageUsageFlags usage, uint32_t levels, uint32_t arrayLayers);



	// Instead of specializing with baked in defautls just expose 
	// sampler creation, maybe bassed on samplerKey
	SamplerHandle GetSampler(SamplerKey samplerKey);
	SamplerHandle CreateSampler(uint32_t levelCount);
	SamplerHandle CreateShadowSampler(uint32_t levelCount);

	//ImageHandle CreateTexture(filepath);
	ImageHandle CreateTexture(ktxTexture2& texture);
	// Same here, no specialization
	//ImageHandle CreateShadowMap(int width = 1024, int height = 1024);

	MaterialInstanceHandle CreateMaterialInstance(PipelineHandle mat_pipeline);
	DescriptorLayoutHandle createDescriptorLayout(const DescriptorSetLayoutDesc& descIn);

	DescriptorPoolHandle createDescriptorPool();

	void CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, int levelCount, int baseArrayLayer, int layerCount, VkImageViewType viewType, VkImageView& view);

	DescriptorSetHandle createDescriptorSet(DescriptorLayoutHandle lyt);
	DeviceBufferHandle createBuffer(VkDeviceSize size, VmaMemoryUsage vma_usage, VkBufferUsageFlags usage_flags);

	void UploadDataToBuffer(DeviceBufferHandle buffer,const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAcces);
	void UploadDataToBuffer(DeviceBufferHandle buffer,const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAccess, size_t src_offset, size_t dest_offset);

	GeometryHandle UploadVertexData(MeshUploadInput& mesh_data);

	VkCommandBuffer GetCommandBuffer();
	void SubmitCommandBuffer(VkCommandBuffer cmdBuf, VkCommandPool pool, VkQueue queue);

	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
	void TransitionImageLayout(ImageHandle image_handle, VkImageLayout oldLayout, VkImageLayout newLayout);

	//SamplerHandle GetSampler(SamplerKey key);
	VkQueue GetGraphicsQueue();
	PipelineHandle GetAppropiatePipeline(PipelineKey key);

	void CleanUp();

};

void destroy(Context* ctx, ImageHandle handle);
void destroy(Context* ctx, ImageViewHandle handle);
void destroy(Context* ctx, SamplerHandle handle);
void destroy(Context* ctx, ShaderModuleHandle handle);
void destroy(Context* ctx, DescriptorPoolHandle handle);
void destroy(Context* ctx, DescriptorSetHandle handle);
void destroy(Context* ctx, DescriptorLayoutHandle handle);
void destroy(Context* ctx, DeviceBufferHandle handle);
void destroy(Context* ctx, PipelineHandle handle);
void destroy(Context* ctx, PipelineLayoutHandle handle);
void destroy(Context* ctx, GeometryHandle handle);
void destroy(Context* ctx, GeometryPageHandle handle);
void destroy(Context* ctx, MaterialInstanceHandle handle);
