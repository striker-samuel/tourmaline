#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <Engine/Shared/common.h>
#include <Engine/Vulkan/vulkan_setup.h>
#include <Engine/Renderer/vertex.h>

// Statements 
// Forward+ . Maybe tiled deferred shadows in future
// Just static geo. Dont focus on streaming yet
// Dynamic Rendering
// Under 10 Materials Definitions. MAterial is stages, pipeline blend, depth conf, vert and shader. attachment slots desc. No more
// Mateials. 
// Simple standwardSurface (Default),
// Unlit, own frag
// Anisotropic, Own brdf
// SubSurface, Maybe needs depth bias.
// Translucent, needs blendin
// Hair, needs blending/ own brdf
// Cloth, sheen.
// Maybe more...


const std::vector<Vertex> quad_vertices = {
	{{7.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
	{{0.5f,  0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
	{{ -0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
	{{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
};

const std::vector<uint32_t> quad_indices_ccw = {
	0, 1, 2,
	2, 3, 0
};

const std::vector<Vertex> triangle_vertices = {
	{{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}}, // v0
	{{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}}, // v1
	{{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}}, // v2
};

const std::vector<uint32_t> triangle_indices_ccw = {
	0, 1, 2
};



struct Mesh {
	// Vertex is 3fpos 3fnormal 2fuv. no color.
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;

	uint32_t first_index = 0;
	uint32_t vert_offset = 0;

};

Mesh Quad = Mesh(quad_vertices, quad_indices_ccw, 0, 0);
Mesh Triangle = Mesh(triangle_vertices, triangle_indices_ccw, 0, 0);


// We hit the masUBOsize limit fast. Be careful. We may need to encode better.
// Maybe we can get away with float12 with implied 0,0,0,1 last row.
struct InstanceData {
	// Identity Matrix initialization
	//alignas(16)
	float transform[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	glm::float4 color = {1.0f, 1.0f, 1.0f, 1.0f};
	//uint32_t meshIndex;
	//uint32_t indexOffset;
	//uint32_t materialIndex;
	//uint32_t LOD;
};

static_assert(sizeof(InstanceData) % 16 == 0);

struct UniformBufferObject {
	//alignas(16) glm::mat4 model;
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
};

struct GPULight {
	// Maybe we dont need scale and can compact here.
	glm::mat4 transform;
	//type
	//cone
	//color
	//intensity
} DefaultLight;

//Down like to use char for this.
typedef struct Color4 {
	unsigned char r;  // Color red value
	unsigned char g;  // Color green value
	unsigned char b;  // Color blue value
	unsigned char a;  // Color alpha value
};

// ---------------------------------
//
enum PipelineSetting {
	DEPTH_ENABLED,
	BACK_CULLING_ENABLED
};


struct MaterialDefinition {

	VkShaderModule vert;
	VkShaderModule frag;

	// This is bad. Should be an index to a pool of VkImages.
	VkImage image;
	VkImageView imageView;
	VkSampler sampler;

	std::array<PipelineSetting, 1> pipeline_settings = {
		{
			DEPTH_ENABLED,
		}
	};
};

struct UniformBufferAllocation {
	VkBuffer buffer;
	VmaAllocation alloc;
	VmaAllocationInfo allocInfo;
	VkDeviceSize size;
	// Did it end in mappable memory? This is important to know for proper upload. Discard the bool in the future use 
	VkMemoryPropertyFlags memory_flags;


	bool is_mapapble() {
		return this->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	}

};
