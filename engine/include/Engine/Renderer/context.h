#pragma once

#include <algorithm>
#include <array>

#include <Engine/Vulkan/vulkan_helpers.h>

#include <Engine/Renderer/constants.h>
#include <Engine/Renderer/resources.h>

#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>

#include <ktx.h>
#include <ktxvulkan.h>


#define DEFAULT_MATERIAL_INDEX (0);

class Context {
	// This is a payload defining instance.
	ktxVulkanDeviceInfo* ktx_vdi = NULL;

	VmaPool D_2DImagesPool;
	VmaPool D_ShaderPool;
	VmaPool D_StagingPool;

public:

	VkInstance* instance_;
	VkDevice device_;
	VkPhysicalDevice physicalDevice_;

	//std::unordered_map<std::string, SamplerHandle> samplers;
	VmaAllocator allocator_;

	// --------------------------------------

	DescriptorPoolHandle default_descriptor_pool;
	DescriptorPoolHandle imgui_descriptor_pool;

	VkCommandPool default_command_pool;

	// --------------------------------------

	VkQueue default_queue;
	VkQueue transfer_queue;

	// --------------------------------------

	
	// images here should be for render targets and thinks like that. 
	// Dont like image name for this. maybea attachment would be better
	Pool<Image, Image> images_;
	Pool<Texture, Image> textures_;

	Pool<BufferDescription, BufferDescription> buffers_;

	//Pool<Sampler, VkSampler> samplers_;

	Pool<DescriptorSet, DescriptorSet> descriptorSets_;
	Pool<DescriptorPool, VkDescriptorPool> descriptorSetPools_;
	Pool<ShaderModule, VkShaderModule> shaderModules_;
	Pool<Pipeline, Pipeline> pipelines_;

	Context(VkInstance& instance, VkDevice* device, VkPhysicalDevice* physicalDevice, VkSurfaceKHR surface);

	~Context(
	);

	void InitializeDeviceVmaPools();

	DeviceBufferHandle CreateStagingBuffer(const void* srcData, VkDeviceSize size);
	DeviceBufferHandle CreateStagingBuffer(VkDeviceSize size);
	DeviceBufferHandle createBuffer(VkDeviceSize size, VmaMemoryUsage vma_usage, VkBufferUsageFlags usage_flags);

	TextureHandle CreateTexture(ktxTexture2& texture);
	ImageHandle CreateImage(int texWidth, int texHeight, VkFormat format, VkImageUsageFlags usage, uint32_t levels, uint32_t arrayLayers);

	ShaderModuleHandle CreateShaderModule(std::vector<char> code);

	void CreatePipelineLayout(const PipelineLayoutDesc& descIn, Pipeline::Layout& out);
	void CreateDescriptorLayout(const DescriptorSetLayoutDesc& descIn, VkDescriptorSetLayout& layout);
	DescriptorSetHandle CreateDescriptorSet(VkDescriptorSetLayout& layout);

	PipelineHandle CreatePipeline(RasterSettings raster_settings, PipelineLayoutDesc layoutDesc, ShaderModuleHandle vert, ShaderModuleHandle frag, VkFormat depth_format, VkFormat swapchainImageFormat);

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



