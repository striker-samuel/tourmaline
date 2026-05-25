#pragma once

#include "volk.h"
#include "vk_mem_alloc.h"

#include <Engine/Memory/handle.h>

#include <ktxvulkan.h>


struct PipelineLayout {
	VkPipelineLayout layout = VK_NULL_HANDLE;
	std::array<VkDescriptorSetLayout, 8> sets{};
};

//  
struct Pipeline {
	// Only caring for these stages for now.
	ShaderModuleHandle vert;
	ShaderModuleHandle frag;

	VkPipeline pipeline = VK_NULL_HANDLE;
	PipelineLayout layout = {};
};

struct DescriptorSet {
	//DescriptorLayoutHandle layout = {};
	VkDescriptorSetLayout layout = {};
	VkDescriptorSet set = VK_NULL_HANDLE;
};

// Surface is the 3d primitve and not to be taken as renderSurface.
struct Surface {
	GeometryPageHandle page;
	uint32_t first_index; 
	uint32_t index_count;
	int32_t  base_vertex;   
};


// 
// 
struct Geometry {

	glm::vec3 aabbMin;
	float     sphereRadius;
	glm::vec3 aabbMax;
	float     spherePad;

	uint32_t lods = 1;
	std::vector<uint32_t> lod_offsets;
	std::vector<Surface> surfaces;

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
	bool is_tex = false;
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
struct PipelineKey {
	//vert
	//frag
	//layout
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

// Pipeline blocks.

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

//




