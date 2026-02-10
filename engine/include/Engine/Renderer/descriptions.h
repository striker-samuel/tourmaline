#pragma once

#include <Engine/Shared/common.h> // Needed for vec3
#include <Engine/Renderer/vertex.h>

struct DrawSetData {
	// Identity Matrix initialization requires this ugly ass shit without goign into macros or else. 
	//alignas(16)
	float transform[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	glm::vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
};

static_assert(sizeof(DrawSetData) % 16 == 0);

/// --------------------------------------


//// Specialized descirptios like these may go the way of the dodo or move to App 
struct MainViewPushConstants {
	glm::mat4 model;
	//glm::mat4 view;
	//glm::mat4 proj;
	int material_index;
	uint32_t     _pad0[3];
};

struct MaterialPushConstants {

	// Color *=
	glm::vec4 ambient = glm::vec4(0.06f);
	glm::vec4 diffuse = glm::vec4(1.0f);
	int tex[4] = { -1, -1, -1, -1 }; //diffuse ,ambient, alpha // unused 

} default_mat_pc;

static_assert(sizeof(MainViewPushConstants) % 16 == 0);
static_assert(sizeof(MaterialPushConstants) % 16 == 0);


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

} default_raster_settings;

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
