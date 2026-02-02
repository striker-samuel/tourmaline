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
#include <Engine/Renderer/light.h>

struct MeshUploadInput {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	glm::vec3 aabb_min; // im reading this directly from gltf.
	glm::vec3 aabb_max;
};

// DEFAULT GEOS

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

// 

MeshUploadInput Quad = MeshUploadInput(quad_vertices, quad_indices_ccw);
MeshUploadInput Triangle = MeshUploadInput(triangle_vertices, triangle_indices_ccw);

// Unused at this time
enum GPU_CULLING_MODE {
	CPU_CULLING,
	GPU_CULLING
};

// This was done before I read : https://www.sebastianaaltonen.com/blog/no-graphics-api
// It seeems going with SSBO by default might be not so bad.
// 
// We hit the masUBOsize limit fast. Be careful. We may need to encode better.
// Maybe we can get away with float12 with implied 0,0,0,1 last row.
// This is the exact footprint of the data uploaded to GPU. 
struct InstanceData {
	// Identity Matrix initialization
	//alignas(16)
	float transform[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	glm::vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
	//uint32_t materialindex; // this is the device side for instacnce abstaction. material is cpu side. this souldnt be added back
	//uint32_t lod;
};

static_assert(sizeof(InstanceData) % 16 == 0);

// 
/// --------------------------------------

struct ViewProjUniform {
	//alignas(16) glm::mat4 model;
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
};

// Dont like this. currently limited by the 16 byte stride.
// Maybe i could do away with it but that needs ext checking and enalbing and mods in shader code
struct alignas(16) IndexSlot {
	uint32_t idx;      // put the 16-bit value here (zero-extend)
	uint32_t pad[3];   // 16 bytes total per element
};
static_assert(sizeof(IndexSlot) % 16 == 0);

struct PushConstants {
	glm::mat4 view;
	glm::mat4 proj;
	// The MaterialPipeline should be binded to desired material. This only gets us bindless addresing of the instnanceSlots.
	// A single MaterialModel. shader code, layout , rastersettings, can have many instancs with individual image attachments and constants.
	int material_instance_index; // This is for the mateiral instance contants such as albedo intensity. speculat intensity and such. 
	uint32_t     _pad0[3];
};

struct GeometryPageRef {
	uint16_t page;      // which arena/page
	uint16_t flags;     // optional (layout, etc.)
};

struct GPUMeshletDesc
{
	uint32_t vertOffset;
	uint32_t vertCount;
	uint32_t triOffset;     // offset into triIndex buffer (in "triangles", not bytes)
	uint32_t triCount;

	// Culling data
	glm::vec3 aabbMin;
	float     sphereRadius;
	glm::vec3 aabbMax;
	float     spherePad;    // or sphereCenter.w if you want vec4 center
	glm::vec3 sphereCenter;
	float     coneCutoff;   // optional
	glm::vec3 coneAxis;     // optional
	float     conePad;
};

// Lets go with uniform_layouts ext. GENERAL and UNDEFINED
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

// Not really being used now really.
struct RasterSettings {
	uint32_t depthClampEnable = VK_FALSE;
	uint32_t rasterizerDiscardEnable = VK_FALSE;
	uint32_t polygonMode = VK_POLYGON_MODE_FILL;
	float lineWidth = 1.0f;
	uint32_t cullMode = VK_CULL_MODE_BACK_BIT;
	uint32_t frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

	// Depth bias for shadowmaps.
	uint32_t depthBiasEnable = VK_FALSE;
	uint32_t depthBiasConstant = 0.0f;
	uint32_t depthBiasSlope = 1.0f; // This shoul be PCF radiues

} default_raster_settings;

// This gets uploaded to Device. stride contraints!
// This will go with pushcontants. May require specialization per shader model
struct MaterialPushConstants {

	// Color *=
	glm::vec4 ambient = glm::vec4(0.06f);
	glm::vec4 diffuse = glm::vec4(1.0f);

	// Index to bindless texture array. Doing this like so obviously makes it so we only have one def for material texture slots
	uint32_t texAmbient = 1;
	uint32_t texDiffuse = 1;
	uint32_t texAlpha = 1;
	uint32_t padding[1];

} default_mat_pc;

static_assert(sizeof(MaterialPushConstants) % 16 == 0);

// Buffer state.
struct BufferDescription {

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
