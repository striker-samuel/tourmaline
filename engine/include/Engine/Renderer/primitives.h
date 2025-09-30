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

struct ViewProjUniform {
	//alignas(16) glm::mat4 model;
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
};

//struct GPULight {
//	// Maybe we dont need scale and can compact here.
//	glm::mat4 transform;
//	//type
//	//cone
//	//color
//	//intensity
//} DefaultLight;

//Down like to use char for this.
typedef struct Color4 {
	unsigned char r;  // Color red value
	unsigned char g;  // Color green value
	unsigned char b;  // Color blue value
	unsigned char a;  // Color alpha value
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

// ------------------------------------------------------------------------------------------------
// 
// This is the neeeded Host side state to manage meshes.
// This is whats needed for the geometry part of the drawcall. Materials are codified per sceneNode.
struct DynamicMesh {

	VmaAllocation allocation;
	VmaAllocationInfo allocInfo;
	// Im gonna have a standard format so no propreties for selecting tri, fan, or else

	uint32_t first_index = 0;
	uint32_t vert_offset = 0;
	uint32_t index_count = 0;

	// LOD should choose first index and voffset but lets keep this like this.... its not that clear yet to me how to manage that properly
	uint32_t LOD;

	// I prefer to manage material + attachentes via MaterialInstanceInstead.
	// but maybe i could have a ref for ez acces here?
	//MaterialInstanceHandle material_instance;

	// Maybe name
	//uint32_t name;

};

// Lets go with uniform_layouts ext. GENERAL and UNDEFINED
struct Texture {

	// to be able to free. or queryu memory info. Could be moved really.
	VmaAllocation allocation;
	VmaAllocationInfo allocInfo;

	VkImage image;
	VkImageView imageView;
	// This is far from good.
	VkSampler sampler;
	// Other properties and attributes

	// I could use debug name
	// Srgb/lienar others
	VkFormat format;

};

// Initialized with crude defaults;
struct RasterSettings {
	uint32_t depthClampEnable = VK_FALSE;
	uint32_t rasterizerDiscardEnable = VK_FALSE;
	uint32_t polygonMode = VK_POLYGON_MODE_FILL;
	uint32_t lineWidth = 1.0f;
	uint32_t cullMode = VK_CULL_MODE_BACK_BIT;
	uint32_t frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	uint32_t depthBiasEnable = VK_FALSE;
} default_raster_settings;

// Materials aabstractions area really PipelineObjets. Raster conf, shader objs, descriptorlayout. 
// MaterialAttachments must match desc layout, adn MaterialConstants pc layout
// So we bake the POS. VkPipelineCache not implemented now
// Another name fot his could be MatModel?
struct MaterialPipeline {

	// We need to hash these tree. but wont do it now
	RasterSettings raster_settings;
	ShaderModuleHandle vert;
	ShaderModuleHandle frag;

	// Lets not complitcate this for now. same footprint/layout for everyone. 
	//VkDescriptorSetLayout* layout;
	// 
	// Lets just hold this here for now
	VkPipelineLayout plyt;

	// Bindless, so material descriptors sets are at this level instead of binding per instnace, so;
	// Invalid initialization!
	DescriptorSetHandle ds;
	VkPipeline pipeline;
};

// This gets uploaded to Device. stride contraints!
// This will go with pushcontants. May require specialization per shader model
struct MaterialPushConstants {

	// Color *=
	glm::vec4 ambient = glm::vec4(0.0f);
	glm::vec4 diffuse = glm::vec4(0.0f);


	// Index to bindless texture array. Doing this like so obviously makes it so we only have one def for material texture slots
	uint32_t texAmbient = 0;
	uint32_t texDiffuse = 0;
	uint32_t texAlpha = 0;
	uint32_t padding[1];

} default_mat_pc;

static_assert(sizeof(MaterialPushConstants) % 16 == 0);

// MatModels or MatPipelines can have several instances with different vlaues for constans and textures.
// Raster state, vert, frag, blending and more stay the same.
struct MaterialInstance {
	MaterialPipelineHandle pipeline;
	MaterialPushConstants values = {};
};


// Buffer state.
struct DeviceBufferDescription {

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

struct DescriptorSet {
	DescriptorLayoutHandle layout;
	VkDescriptorSet set;
};

