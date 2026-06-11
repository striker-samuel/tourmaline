#pragma once

#include <algorithm>
#include <array>

#include <Engine/Vulkan/core.h>
#include <Engine/Vulkan/vulkan_helpers.h>

#include <Engine/Renderer/constants.h>
#include <Engine/Renderer/resources.h>

#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>


// textures upload
#include <map>

// Texture upload 
// this should be refactore  to something like a GetImageData fucntion arg, maybe...
#include <ktx.h>
#include <ktxvulkan.h>


struct PipelineCreateInfo {};

// These may be redundant but im still going with them
struct SamplerCreateInfo {
};

// These also may be used for Keys? dont know yet.
//using SamplerKey = SamplerCreateInfo;

struct ImageCreateInfo {
	int texWidth;
	int texHeight;
	VkFormat format;
	VkImageUsageFlags usage;
	uint32_t levels;
	uint32_t arrayLayers;
};

struct BufferCreateInfo {
	VkDeviceSize size;
	VmaMemoryUsage vma_usage;
	VkBufferUsageFlags usage_flags;
};

class Context {
	
	VulkanCore& core;
	VmaAllocator allocator_;

	VmaPool D_2DImagesPool;
	VmaPool D_StagingPool;

public:

	int Initialize();
	Context(VulkanCore _core) : core(_core) { Initialize(); };
	~Context();

	Pool<Image, Image> images_;
	Pool<Texture, Image> textures_; // tdont like this specialization 
	Pool<DeviceBuffer, DeviceBuffer> buffers_; // neither this monolithic buffer setup.
	Pool<DescriptorSet, DescriptorSet> descriptorSets_;
	Pool<ShaderModule, VkShaderModule> shaderModules_;
	Pool<Pipeline, Pipeline> pipelines_;

	Pool<DescriptorPool, VkDescriptorPool> descriptorSetPools_;
	DescriptorPoolHandle default_descriptor_pool;
	DescriptorPoolHandle imgui_descriptor_pool;

	//Pool<Sampler, VkSampler> samplers_;
	std::map<std::string, TextureHandle> textures_uploaded;

	void InitializeDeviceVmaPools();


	// BUFFERS
	DeviceBufferHandle CreateStagingBuffer(const void* srcData, VkDeviceSize size);
	DeviceBufferHandle CreateStagingBuffer(VkDeviceSize size);

	DeviceBufferHandle CreateBuffer(BufferCreateInfo _bufferCreateInfo);

	// IMAGES
	ImageHandle CreateImage(ImageCreateInfo _imageCreateInfo);
	ImageHandle CreateImage(int texWidth, int texHeight, VkFormat format, VkImageUsageFlags usage, uint32_t levels, uint32_t arrayLayers);

	// TEXTURES
	// These need to be heavily reworked
	TextureHandle GetOrCreateTexure(std::string uri, ktxTexture2& texture);
	TextureHandle CreateTexture(ktxTexture2& texture);

	// SHADERS
	ShaderModuleHandle CreateShaderModule(std::vector<char> code);

	// DESCRIPTOR SETS
	DescriptorSetHandle CreateDescriptorSet(VkDescriptorSetLayout& layout);

	// PIPELINES
	PipelineHandle CreatePipeline(RasterSettings raster_settings, PipelineLayoutDesc layoutDesc, ShaderModuleHandle vert, ShaderModuleHandle frag, VkFormat depth_format, VkFormat swapchainImageFormat);


	void CreatePipelineLayout(const PipelineLayoutDesc& descIn, Pipeline::Layout& out);
	void CreateDescriptorLayout(const DescriptorSetLayoutDesc& descIn, VkDescriptorSetLayout& layout);


	//SamplerHandle CreateSampler(uint32_t levelCount);
	void CreateSampler(uint32_t levelCount, VkSampler& sampler);

	DescriptorPoolHandle createDescriptorPool();

	void CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, int levelCount, int baseArrayLayer, int layerCount, VkImageViewType viewType, VkImageView& view);

	void UploadDataToBuffer(DeviceBufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAcces);
	void UploadDataToBuffer(DeviceBufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAccess, size_t src_offset, size_t dest_offset);

	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset);

	//
	VkCommandBuffer GetCommandBuffer();
	void SubmitCommandBuffer(VkCommandBuffer cmdBuf, VkCommandPool pool, VkQueue queue);



	// 
	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
	void TransitionImageLayout(ImageHandle image_handle, VkImageLayout oldLayout, VkImageLayout newLayout);

	void CleanUp();

};

void destroy(Context* ctx, ImageHandle handle);
void destroy(Context* ctx, ShaderModuleHandle handle);
void destroy(Context* ctx, DescriptorPoolHandle handle);
void destroy(Context* ctx, DeviceBufferHandle handle);
void destroy(Context* ctx, PipelineHandle handle);



