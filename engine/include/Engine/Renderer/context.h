#pragma once

#include <array>
#include <tuple>
#include <map>
#include <future>
#include <algorithm>
//
#include <Engine/Vulkan/core.h>
#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>
//
#include "vk_mem_alloc.h"
#include <ktx.h>
#include <ktxvulkan.h>

// 
using ImageHandle = Handle<struct Image>; // Attachments
using PipelineHandle = Handle<struct Pipeline>;
using ToyBufferHandle = Handle<struct ToyBuffer>;
using SamplerHandle = Handle<struct Sampler>;
using ShaderModuleHandle = Handle<struct ShaderModule>;
using DescriptorPoolHandle = Handle<struct DescriptorPool>;
using DescriptorSetHandle = Handle<struct DescriptorSet>;
//
// Specialization for textures.
using TextureHandle = Handle<struct Image>;
//
struct Pipeline {
	VkPipeline pipeline;
	VkPipelineLayout layout;
};

struct Image {
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
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
	VkSampler sampler_ = VK_NULL_HANDLE;
	//Image& setSampler(VkSampler sampler) { this->sampler_ = sampler; return *this; };
};

struct ToyBuffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc{};
	//VmaAllocationInfo allocInfo{}; // this is just an info return. the info contained maybe altered during refrag so makes no sense storing it
	VkBufferUsageFlags usage;
	VkMemoryPropertyFlags memory_flags;
	bool is_mapable() const {
		return this->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	}
	bool operator()(const ToyBuffer& a, const ToyBuffer& b) const {
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
using ImageViewCreateInfo = VkImageViewCreateInfo; // hacky
//struct ImageViewCreateInfo {};

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
		bool valid = 0;
		std::vector<DescriptorSetDescription> setDescriptions; // each has set index
		std::vector<PushConstantDescription> pushConstants;
	};
	PipelineLayoutDesc layoutDescription{};
	RasterSettings rasterSettings{};
};

using PipelineKey = PipelineCreateInfo;
struct BufferCreateInfo {
	VkDeviceSize size;
	VmaMemoryUsage vma_usage;
	VkBufferUsageFlags usage_flags;
};

//struct TextureCache {
//	TextureCache(Context& ctx) :ctx_(ctx) {};
//	Context& ctx_;
//	std::map<std::string, Holder<ImageHandle>> texturesUploaded_;
//	TextureHandle GetOrCreateTexture(std::string uri, ktxTexture2& texture);
//};

// 4.6gb is good amount to reserver for 8gb vram system
// 2gb for Windows is good practice. And a little headroom for aux apps
struct Context {
	VulkanCore& core;
	// 
	ktxVulkanDeviceInfo ktxInfo_;
	std::vector<DeferredCommandBuffer> deferredTasks_;

	int Initialize();
	Context(VulkanCore& _core) : core(_core) { Initialize(); };
	~Context();

	Context(const Context&) = delete;
	Context& operator=(const Context&) = delete;
	Context(Context&&) = delete;
	Context& operator=(Context&&) = delete;

	//
	Holder<ToyBufferHandle> CreateBuffer(BufferCreateInfo bufferCreateInfo);
	Holder<ToyBufferHandle> CreateStagingBuffer(VkDeviceSize size);
	//
	Holder<ImageHandle> CreateImage(ImageCreateInfo imageCreateInfo);
	Holder<ShaderModuleHandle> CreateShaderModule(std::vector<char> code);
	Holder<DescriptorPoolHandle> CreateDescriptorPool(); // just hardcoded defaults for pool. 
	Holder<DescriptorSetHandle> CreateDescriptorSet(DescriptorPoolHandle pool, VkDescriptorSetLayout& layout);
	//
	void UploadDataToBuffer(ToyBufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAcces);
	void UploadDataToBuffer(ToyBufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAccess, size_t src_offset, size_t dest_offset);
	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
	void TransitionImageLayout(ImageHandle image_handle, VkImageLayout oldLayout, VkImageLayout newLayout);
	//
	void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset);
	//
	void CleanUp();
	//
	void AddDeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle);
	void ProcessDeferrredTasks(); //void Flush();
	//
	void GetKtxDeviceInfo() {
		ktxInfo_ = *ktxVulkanDeviceInfo_Create(
			core.physicalDevice_,
			core.device_,
			/*core.GetTransferQueue()*/ VK_NULL_HANDLE,
			// this maybe should be transfer queue. we only use ktx to upload tex.
			// no blitting for mipmaps, which are generated offline
			// runtime genreation for mipmaps in ktx seems to be not up to par with SOA
			core.defaultCommandPool_,
			NULL
		);
	}
	//SamplerHandle GetSampler(SamplerKey key) {
	//	auto it = samplerMap_.find(key);
	//	if (it != samplerMap_.end()) {
	//		//return it->second().get().handle();
	//	}
	//	else {
	//		auto handle = core.CreateSampler(key);
	//		samplerMap_[key] = Holder(&this, handle);
	//		return handle;

	//	}
	//};

//private:
	Pool<Image, Image> images_;
	Pool<ToyBuffer, ToyBuffer> buffers_;
	Pool<Sampler, VkSampler> samplers_;
	Pool<ShaderModule, VkShaderModule> shaderModules_;
	Pool<DescriptorPool, VkDescriptorPool> descriptorPools_;
	//
	VmaAllocator allocator_;
	VmaPool D_2DImagesPool;
	VmaPool D_StagingPool;
	//
	TextureHandle nil_texture{};
	SamplerHandle default_sampler{};
};

void destroy(Context* ctx, ImageHandle handle);
void destroy(Context* ctx, ToyBufferHandle handle);
void destroy(Context* ctx, ShaderModuleHandle handle);
void destroy(Context* ctx, DescriptorPoolHandle handle);


struct PipelineCache {
	std::map<PipelineKey, Holder<PipelineHandle>> pipelines_;
};

struct ShaderProgram {
	Context& ctx_;
	ShaderProgram(Context& ctx) : ctx_(ctx) {
		//dps_.push_back(Holder(&ctx_, ctx_.CreateDescriptorPool()));
	};
	~ShaderProgram() { dps_.clear(); sets_.clear(); };
	// 
	//Holder<ShaderModuleHandle> vert_{};
	//Holder<ShaderModuleHandle> frag_{};
	ShaderModuleHandle vert_{};
	ShaderModuleHandle frag_{};

	std::vector<DescriptorPoolHandle> dps_;
	std::vector<DescriptorSetHandle> sets_;

	// Pass will give us this
	PipelineCreateInfo::RasterSettings rasterSettings_{};
	PipelineCreateInfo::PipelineLayoutDesc pipelineLayoutDescription_{};
	// Graph will give us this
	VkPipelineRenderingCreateInfo dynamicRenderingInfo_;
	//std::map<PipelineCreateInfo, Holder<PipelineHandle>> pipelineCache_;

public:

	ShaderProgram& setRasterSettings(PipelineCreateInfo::RasterSettings rasterSettings) { this->rasterSettings_ = rasterSettings; };
	ShaderProgram& setDynamicRenderingInfo(VkPipelineRenderingCreateInfo dynamicRenderingInfo) { this->dynamicRenderingInfo_ = dynamicRenderingInfo; };
	ShaderProgram& setLayoutDescription();
	ShaderProgram& setVert(ShaderModuleHandle vert) { this->vert_ = Holder<ShaderModuleHandle>(&ctx_, vert); return *this; };
	ShaderProgram& setFrag(ShaderModuleHandle frag) { this->frag_ = Holder<ShaderModuleHandle>(&ctx_, frag); return *this; };

	VkPipeline Build() {
		PipelineCreateInfo info;
		info.rasterSettings = rasterSettings_;
		info.vert = vert_;
		info.frag = frag_;
		info.layoutDescription;
	};

private:
	void CreatePipelineLayout(const PipelineCreateInfo::PipelineLayoutDesc& descIn, VkPipelineLayout& out);
	void CreateDescriptorSetLayout(const PipelineCreateInfo::DescriptorSetDescription& descIn, VkDescriptorSetLayout& layout);

};

