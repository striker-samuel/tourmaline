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

struct MeshData {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	uint32_t materialIndex = 0;       // primitive.material (or -1)
};

MeshData Quad = MeshData(quad_vertices, quad_indices_ccw);
MeshData Triangle = MeshData(triangle_vertices, triangle_indices_ccw);

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
	//uint32_t meshindex;
	//uint32_t indexoffset;
	//uint32_t materialindex; // this is the device side for instacnce abstaction. material is cpu side. this souldnt be added back
	//uint32_t lod;
};

static_assert(sizeof(InstanceData) % 16 == 0);

// Lighting
typedef enum LightType {
	LIGHT_TYPE_POINT = 0,
	LIGHT_TYPE_SPOT = 1,
} LightType;

struct alignas(16) Light {
	glm::vec4 color = glm::vec4(1.0f);       // offset 0,  size 16

	float position[3] = { 0.0f,0.0f,0.0f };     // offset 16, size 12
	int32_t flags = 0;
	//float _pad0;           // offset 28, size 4  -> total 32

	float direction[3] = { 0.0f,0.0f,0.0f };    // offset 32, size 12
	float intensity = 1.0f;       // offset 44, size 4  -> total 48

	int32_t type = LIGHT_TYPE_POINT;
	// in radians!
	float innerAngle = glm::radians(10.0f);
	float outerAngle = glm::radians(20.0f);
	// -1 no shadow map. else context->shadowmaps.id.
	int shadow_map_index = -1;
	float lightVP[16] = {};
};

static_assert(sizeof(Light) % 16 == 0);

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
	int material_instance_index;
	uint32_t     _pad0[3];
};

// Lets just assume offsets into the static index/vertex buffers for now :)
struct DrawSet {
	//static doesn't go here- Instead static sets are baked into large draw sets. static is just to skip hierarchy transform updates.
	// Maybe for coherency for optimization

	uint32_t first_index = 0;

	// this is size
	uint32_t vert_offset = 0;
	uint32_t index_count = 0;

	// This is handled scene side. This is just temp
	uint32_t material_index = 0;
};


std::unordered_map<uint32_t, std::string> mesh_names;

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

// Initialized with crude defaults;
struct RasterSettings {
	uint32_t depthClampEnable = VK_FALSE;
	uint32_t rasterizerDiscardEnable = VK_FALSE;
	uint32_t polygonMode = VK_POLYGON_MODE_FILL;
	float lineWidth = 1.0f;
	uint32_t cullMode = VK_CULL_MODE_BACK_BIT;
	uint32_t frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	uint32_t depthBiasEnable = VK_FALSE;
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


template<typename T>
inline float length2(const T& v) {
	return glm::dot(v, v);
}

//#include <glm/glm.hpp>
//#include <glm/gtc/matrix_transform.hpp>
//
// Convert your Light to a view matrix (for spot / "directional-like" usage)
inline glm::mat4 GetLightView(const Light& l)
{
	glm::vec3 pos(l.position[0], l.position[1], l.position[2]);
	glm::vec3 dir(l.direction[0], l.direction[1], l.direction[2]);

	// Avoid zero-length direction
	if (length2(dir) < 1e-6f)
		dir = glm::vec3(0.0f, -1.0f, 0.0f); // default downwards

	dir = glm::normalize(dir);

	// Choose an up vector that isn't parallel to dir
	glm::vec3 up(0.0f, 1.0f, 0.0f);
	if (glm::abs(glm::dot(dir, up)) > 0.99f)
		up = glm::vec3(0.0f, 0.0f, 1.0f);

	return glm::lookAt(pos, pos + dir, up);
}

// Perspective projection for the light (spot light shadow map)
inline glm::mat4 GetLightProj(
	const Light& l,
	float aspect = 1.0f,   // usually shadow map is square, so 1.0
	float nearPlane = 0.1f,
	float farPlane = 100.0f  // tweak per scene
)
{
	// For spot lights, use outerAngle to define the FOV
	if (l.type == LIGHT_TYPE_SPOT) {
		// outerAngle is in degrees in your struct; FOV = 2 * outerAngle
		float fovRadians = l.outerAngle * 2.0f;
		return glm::perspective(fovRadians, aspect, nearPlane, farPlane);
	}

	// For point lights, this is only a placeholder if you *don't* do cubemap shadows.
	// Proper point light shadows need 6 views + same projection.
	float fovRadians = glm::radians(90.0f); // 90° for each cube face, if you go that route
	return glm::perspective(fovRadians, 1.0f, nearPlane, farPlane);
}

