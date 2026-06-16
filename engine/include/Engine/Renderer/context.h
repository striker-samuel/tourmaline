#pragma once

#include <algorithm>
#include <array>

#include <Engine/Vulkan/core.h>
#include <Engine/Vulkan/vulkan_helpers.h>

#include <Engine/Renderer/constants.h>
#include <Engine/Renderer/resources.h>

#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>


#include <tuple>
// textures upload
#include <map>

// Texture upload 
// this should be refactore  to something like a GetImageData lambda, maybe...
#include <ktx.h>
#include <ktxvulkan.h>


/// <summary>
///  One could argue this is just redundant provided that vulkanHpp has this same thing typed out...
/// </summary>
struct ImageCreateInfo {
	int texWidth;
	int texHeight;
	VkFormat format;
	VkImageUsageFlags usage;
	uint32_t levels;
	uint32_t arrayLayers;

	bool operator<(const ImageCreateInfo& other) const {
        return std::tie(texWidth, texHeight, format, usage, levels, arrayLayers) <
               std::tie(other.texWidth, other.texHeight, other.format,
                        other.usage, other.levels, other.arrayLayers);
    }
};

struct SamplerCreateInfo {
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
};
using SamplerKey = SamplerCreateInfo;

// TODO :: This neeeds heaby doses of elbow grease really
struct PipelineCreateInfo{
	
	// Raster
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
		float depthBiasSlope = 1.0f; // This should be PCF radius https://mynameismjp.wordpress.com/2013/09/10/shadow-maps/

	} rasterSettings;

	// TODO: HEABY WORK HEER
	// This should be creation/key also. dont quite like the stae of inmutable samplers and binding.
	struct BindingDesc {
		uint32_t binding = 0; // state 
		VkDescriptorType type{};
		uint32_t count = 1;
		VkShaderStageFlags stages = 0;
		VkDescriptorBindingFlags flags = 0;
		std::vector<VkSampler> immutableSamplers; // state, should be a list of SamplerKey/desc/createInfo really
	};

	//struct Binding {
	//	size_t bindPoint = 0;
	//	std::vector<VkSampler> immutableSamplers;
	//	BindingDesc desc;
	//};

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
	} layoutDesc;

	// I dont like this 
	ShaderModuleHandle vert;
	ShaderModuleHandle frag;

	// Swapchain Setup.
	VkFormat depth_format;
	VkFormat swapchainImageFormat;
};

struct DeviceBufferCreateInfo {
	VkDeviceSize size;
	VmaMemoryUsage vma_usage;
	VkBufferUsageFlags usage_flags;
};


class Context {
	
	VmaPool D_2DImagesPool;
	VmaPool D_StagingPool;

public:

	VmaAllocator allocator_;
	VulkanCore& core;
	//VulkanCore& core_;

	int Initialize();
	Context(VulkanCore& _core) : core(_core) {  Initialize(); };
	~Context();

	// 
	struct ToyCommandBuffer {
		VkCommandBuffer cmdH;
		size_t index;
		bool recording;
	};
	std::vector<ToyCommandBuffer> commamds;
	// 

	Pool<Image, Image> images_;
	Pool<Texture, Image> textures_; // dont like this specialization 
	Pool<DeviceBuffer, DeviceBuffer> buffers_; // neither this monolithic buffer setup.
	Pool<DescriptorSet, DescriptorSet> descriptorSets_;
	Pool<ShaderModule, VkShaderModule> shaderModules_;
	Pool<Pipeline, Pipeline> pipelines_;
	Pool<DescriptorPool, VkDescriptorPool> descriptorSetPools_;
	//Pool<Sampler, VkSampler> samplers_;

	std::map<std::string, TextureHandle> textures_uploaded;

	DescriptorPoolHandle default_descriptor_pool;
	DescriptorPoolHandle imgui_descriptor_pool;

	void InitializeDeviceVmaPools();

	// Create resources

	// BUFFERS
	DeviceBufferHandle CreateStagingBuffer(const void* srcData, VkDeviceSize size);
	DeviceBufferHandle CreateStagingBuffer(VkDeviceSize size);
	DeviceBufferHandle CreateBuffer(DeviceBufferCreateInfo bufferCreateInfo);
	//Images
	ImageHandle CreateImage(ImageCreateInfo imageCreateInfo);
	//ImageHandle CreateImage(int texWidth, int texHeight, VkFormat format, VkImageUsageFlags usage, uint32_t levels, uint32_t arrayLayers);
	// PIPELINES
	PipelineHandle CreatePipeline(PipelineCreateInfo pipelineCreateInfo);


	// SHADERS
	ShaderModuleHandle CreateShaderModule(std::vector<char> code);
	// DESCRIPTOR SETS
	DescriptorSetHandle CreateDescriptorSet(VkDescriptorSetLayout& layout);

	// TEXTURES
	// These need to be heavily reworked
	TextureHandle GetOrCreateTexure(std::string uri, ktxTexture2& texture);
	//TextureHandle CreateTexture(ktxTexture2& texture);

	void CreatePipelineLayout(const PipelineCreateInfo::PipelineLayoutDesc& descIn, Pipeline::Layout& out);
	void CreateDescriptorLayout(const PipelineCreateInfo::DescriptorSetLayoutDesc& descIn, VkDescriptorSetLayout& layout);

	void CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler);

	DescriptorPoolHandle createDescriptorPool();

	void CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, int levelCount, int baseArrayLayer, int layerCount, VkImageViewType viewType, VkImageView& view);


	// 
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


class ResourceMan{
	ResourceMan(Context& context) : ctx(context){};
	Context& ctx;
	Pool<Texture, Image> textures_; // dont like this specialization 
};
void destroy(ResourceMan* man, TextureHandle handle);
