#pragma once

#include "volk.h"
#include "vk_mem_alloc.h"

#include <Engine/Memory/handle.h>

#include <ktxvulkan.h>


struct Pipeline {
	struct Layout {
		VkPipelineLayout layout = VK_NULL_HANDLE;
		std::array<VkDescriptorSetLayout, 8> sets{};
	} layout;

	ShaderModuleHandle vert;
	ShaderModuleHandle frag;

	VkPipeline pipeline = VK_NULL_HANDLE;
};

struct DescriptorSet {
	//DescriptorLayoutHandle layout = {};
	VkDescriptorSetLayout layout = {};
	VkDescriptorSet set = VK_NULL_HANDLE;
};


struct Image {
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo{};

	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageUsageFlags usage = 0;

	VkExtent3D extent{};
	uint32_t arrayLayers = 0;
	uint32_t levels = 0;

	VkImageView sampleView = VK_NULL_HANDLE;
	VkImageView renderView = VK_NULL_HANDLE;
	VkImageView arrayView[6]{
		VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
		VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE
	};


	VkSampler sampler = VK_NULL_HANDLE;

	// This is unused now
	//bool is_tex = false;


	// THIS IS WRONG. THIS IS ONLY HERE TO REMIND MYSELF 
	// ON LAYOUT TRACKING. LACYOUT TRACKING IS NOT NEEDED ANYMORE JUST RAWDOW UNDEFINED ANYWAYS
	// 
	//https://www.youtube.com/watch?v=0rqWe1M2HiE&t=433s
	// 
	//VkLayout current_layout;
	// Lets do rfc so the reource mangare can free unused textures
	// this is only applicable to texutes tho
	uint32_t reference_counter = 0;

	// Maybe to stream out?
	//uint32_t last_frame_seen;

	// TODO: remove this hacky way!
	bool owns_sampler = false;
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

struct DeviceBuffer {
	// 
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

	//bool operator < (const BufferDescription& other) const {
	//	return std::tie(buffer, alloc,  usage, memory_flags) < std::tie(
	//	other.buffer, other.alloc, other.usage, other.memory_flags);
	//}

};

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
	std::vector<VkSampler> immutableSamplers; // size must be == count if non-empty
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

//




