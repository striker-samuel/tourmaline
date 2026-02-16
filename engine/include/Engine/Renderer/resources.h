#pragma once

#include "volk.h"
#include "vk_mem_alloc.h"

#include <Engine/Memory/handle.h>

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


