#pragma once

#include <algorithm>
#include <array>

#include <Engine/Vulkan/core.h>
#include <Engine/Vulkan/vulkan_helpers.h>
#include <Engine/Vulkan/VulkanSwapchain.h>

#include <Engine/Renderer/constants.h>
#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>

#include <tuple>
#include <map>
#include <future>

#include "vk_mem_alloc.h"
#include <ktx.h>
#include <ktxvulkan.h>

namespace SamToy {};
struct VulkanSwapchain;

struct Image {
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo{};

	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageUsageFlags usage = 0;

	VkExtent3D extent{};
	uint32_t arrayLayers = 0;
	uint32_t levels = 0;

	VkImageView sampleView = VK_NULL_HANDLE; // you can sample multiple levels
	VkImageView renderView = VK_NULL_HANDLE; // but can only render to one. Views are created with this baked in. Thats the why to separate them
	VkImageView arrayView[6]{
		VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
		VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE
	};
	// not owning
	VkSampler sampler = VK_NULL_HANDLE;
	//SamplerKey samplerKey{};
};

struct DeviceBuffer {
	VkBuffer buffer;
	VmaAllocation alloc;
	VmaAllocationInfo allocInfo;
	VkBufferUsageFlags usage;
	VkMemoryPropertyFlags memory_flags;

	bool is_mapable() const {
		return this->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	}
	bool operator()(const DeviceBuffer& a, const DeviceBuffer& b) const {
		return std::tie(a.buffer, a.alloc, a.usage, a.memory_flags)
			< std::tie(b.buffer, b.alloc, b.usage, b.memory_flags);
	}
};


/// <summary>
///  One could argue this is just redundant provided that VkImageCreateInfo has this same thing typed out...
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
struct PipelineCreateInfo {
	//struct VertexFormatDesc {
	//};
	struct RasterSettings {
		uint32_t depthClampEnable = VK_FALSE;
		uint32_t rasterizerDiscardEnable = VK_FALSE;
		VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
		float lineWidth = 1.0f;
		uint32_t cullMode = VK_CULL_MODE_BACK_BIT;
		VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		uint32_t depthBiasEnable = VK_FALSE;
		// Depth bias for shadowmaps.
		float depthBiasConstant = 0.0f;
		float depthBiasSlope = 1.0f; // This should be PCF radius https://mynameismjp.wordpress.com/2013/09/10/shadow-maps/

	};
	struct BindingDescription {
		uint32_t binding = 0; // If we want sparse bindings we need this
		VkDescriptorType type{};
		uint32_t count = 1;
		VkShaderStageFlags stages = 0;
		VkDescriptorBindingFlags flags = 0;
		std::vector<VkSampler> immutableSamplers;
	};
	struct DescriptorSetDescription {
		VkDescriptorSetLayoutCreateFlags createFlags = 0; // e.g. UPDATE_AFTER_BIND_POOL_BIT
		std::vector<BindingDescription> bindings;
	};
	struct PushConstantDescription {
		VkShaderStageFlags stages = 0;
		uint32_t offset = 0;
		uint32_t size = 0;
	};
	struct PipelineLayoutDesc {
		std::vector<DescriptorSetDescription> setDescriptions; // each has set index
		std::vector<PushConstantDescription> pushConstants;
	};
	PipelineLayoutDesc layoutDescription{};
	RasterSettings rasterSettings{};
	ShaderModuleHandle vert{};
	ShaderModuleHandle frag{};
};
using PipelineKey = PipelineCreateInfo;

struct BufferCreateInfo {
	VkDeviceSize size;
	VmaMemoryUsage vma_usage;
	VkBufferUsageFlags usage_flags;
};

struct PipelineBuilder {
	PipelineBuilder(Context& ctx) : ctx_(ctx) {
		dps_.push_back(Holder(&ctx_, ctx_.createDescriptorPool()));
	};
	~PipelineBuilder() { dps_.clear(); sets_.clear(); };
	Context& ctx_;

	// 
	ShaderModuleHandle vert_{};
	ShaderModuleHandle frag_{};
	//hmmm
	std::vector<Holder<DescriptorPoolHandle>> dps_;
	std::vector<Holder<DescriptorSetHandle>> sets_;


	// Pass will give us this
	PipelineCreateInfo::RasterSettings rasterSettings_;
	// Graph will give us this
	VkPipelineRenderingCreateInfo dynamicRenderingInfo_;

	//using PipelineLayoutKey = PipelineCreateInfo::PipelineLayoutDesc;
	//std::map<PipelineLayoutKey, VkPipelineLayout> pipelineCache_;

public:

	PipelineBuilder& setVert();
	PipelineBuilder& setFrag();
	//PipelineBuilder& setVertexFormat();

	PipelineBuilder& setRasterSettings(PipelineCreateInfo::RasterSettings rasterSettings) { this->rasterSettings_ = rasterSettings; };
	PipelineBuilder& setDynamicRenderingInfo(VkPipelineRenderingCreateInfo dynamicRenderingInfo) { this->dynamicRenderingInfo_ = dynamicRenderingInfo; };
	//PipelineBuilder& setRasterSettings(PipelineCreateInfo::RasterSettings rasterSettings) { this->rasterSettings_ = rasterSettings; };
	PipelineHandle CreateGraphicsPipeline(PipelineCreateInfo pipelineCreateInfo) {};
	VkPipeline Build() {

		rasterSettings_ = {};
		dynamicRenderingInfo_ = {};
	};

private:
	void CreatePipelineLayout(const PipelineCreateInfo::PipelineLayoutDesc& descIn, VkPipelineLayout& out);
	void CreateDescriptorSetLayout(const PipelineCreateInfo::DescriptorSetDescription& descIn, VkDescriptorSetLayout& layout);

};


// 4.6gb is good amount to reserver for 8gb vram system
// 2gb for Windows is good practice. And a little headroom for aux apps
struct Context {
	//std::vector<std::packaged_task<void()>> deferredTasks;
	VulkanCore& core;
	//VulkanCore& core_;

	int Initialize();
	Context(VulkanCore& _core) : core(_core) { Initialize(); };
	~Context();

	Context(const Context&) = delete;
	Context& operator=(const Context&) = delete;
	Context(Context&&) = delete;
	Context& operator=(Context&&) = delete;
	//

	Pool<Image, Image> images_;
	Pool<DeviceBuffer, DeviceBuffer> buffers_;
	Pool<DescriptorSet, VkDescriptorSet> descriptorSets_;
	Pool<ShaderModule, VkShaderModule> shaderModules_;
	Pool<DescriptorPool, VkDescriptorPool> descriptorPools_;
	Pool<Pipeline, Pipeline> graphicPipelines_;
	//
	DeviceBufferHandle CreateStagingBuffer(const void* srcData, VkDeviceSize size);
	DeviceBufferHandle CreateStagingBuffer(VkDeviceSize size);
	DeviceBufferHandle CreateBuffer(BufferCreateInfo bufferCreateInfo);

	ImageHandle CreateImage(ImageCreateInfo imageCreateInfo);
	ShaderModuleHandle CreateShaderModule(std::vector<char> code);

	DescriptorPoolHandle createDescriptorPool();
	void UploadDataToBuffer(DeviceBufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAcces);
	void UploadDataToBuffer(DeviceBufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAccess, size_t src_offset, size_t dest_offset);
	//
	void CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler);
	void CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, int levelCount, int baseArrayLayer, int layerCount, VkImageViewType viewType, VkImageView& view);
	// 
	VkCommandBuffer GetCommandBuffer();
	void SubmitCommandBuffer(VkCommandBuffer cmdBuf, VkCommandPool pool, VkQueue queue);
	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
	void TransitionImageLayout(ImageHandle image_handle, VkImageLayout oldLayout, VkImageLayout newLayout);
	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset);
	//
	void CleanUp();

	VmaAllocator allocator_;
private:
	VmaPool D_2DImagesPool;
	VmaPool D_StagingPool;

	// We need a default texture for the nil slot incase missing textures are used.
	// The image needs to exist to be in the descriptorSet so it must be a real image
	TextureHandle nil_texture{}; // Default texture.
};

void destroy(Context* ctx, ImageHandle handle);
void destroy(Context* ctx, ShaderModuleHandle handle);
void destroy(Context* ctx, DescriptorPoolHandle handle);
void destroy(Context* ctx, DeviceBufferHandle handle);
void destroy(Context* ctx, PipelineHandle handle);
