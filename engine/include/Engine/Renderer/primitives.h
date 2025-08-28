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


CameraPositioner_FirstPerson cameraPositioner(
	glm::vec3(0.0f, 0.0f, -1.0f), //pos
	glm::vec3(0.0f, 0.0f, 0.0f), //target
	glm::vec3(0.0f, 1.0f, 0.0f) //up
);


// Quad (two triangles), CCW
const std::vector<Vertex> quad_vertices = {
	{{7.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
	{{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
	{{ -0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
	{{0.5f,  0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
};

const std::vector<uint32_t> quad_indices_ccw = {
	0, 1, 2,
	2, 3, 0
};

// Triangle, CCW
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
	//float4 color = {1.0f, 1.0f, 1.0f, 1.0f};
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

static inline size_t gltfElementSize(const tinygltf::Accessor& a) {
	using namespace tinygltf;
	int numComps =
		(a.type == TINYGLTF_TYPE_SCALAR) ? 1 :
		(a.type == TINYGLTF_TYPE_VEC2) ? 2 :
		(a.type == TINYGLTF_TYPE_VEC3) ? 3 :
		(a.type == TINYGLTF_TYPE_VEC4) ? 4 :
		(a.type == TINYGLTF_TYPE_MAT2) ? 4 :
		(a.type == TINYGLTF_TYPE_MAT3) ? 9 :
		(a.type == TINYGLTF_TYPE_MAT4) ? 16 : 0;

	size_t compSize =
		(a.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
			a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) ? 1 :
		(a.componentType == TINYGLTF_COMPONENT_TYPE_SHORT ||
			a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) ? 2 :
		(a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT ||
			a.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) ? 4 : 0;

	return numComps * compSize;
}

static inline const uint8_t* gltfPtr(const tinygltf::Model& m,
	const tinygltf::Accessor& a,
	size_t index)
{
	const auto& bv = m.bufferViews[a.bufferView];
	const auto& buf = m.buffers[bv.buffer];
	size_t elemSize = gltfElementSize(a);
	size_t stride = bv.byteStride ? bv.byteStride : elemSize;

	size_t base = (bv.byteOffset ? bv.byteOffset : 0) +
		(a.byteOffset ? a.byteOffset : 0);
	return buf.data.data() + base + index * stride;
}

glm::vec3 readVec3F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC3
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec3(p[0], p[1], p[2]);
}

glm::vec2 readVec2F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC2
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec2(p[0], p[1]);
}

uint32_t readIndex(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	using namespace tinygltf;
	const uint8_t* p = gltfPtr(m, a, i);
	switch (a.componentType) {
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return *reinterpret_cast<const uint8_t*>(p);
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return *reinterpret_cast<const uint16_t*>(p);
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return *reinterpret_cast<const uint32_t*>(p);
	default: throw std::runtime_error("Unsupported index componentType");
	}
}

bool loadModel(tinygltf::Model& model, const char* filename) {
	tinygltf::TinyGLTF loader;
	std::string err;
	std::string warn;

	bool res = loader.LoadASCIIFromFile(&model, &err, &warn, filename);
	if (!warn.empty()) {
		std::cout << "WARN: " << warn << std::endl;
	}

	if (!err.empty()) {
		std::cout << "ERR: " << err << std::endl;
	}

	if (!res)
		std::cout << "Failed to load glTF: " << filename << std::endl;
	else
		std::cout << "Loaded glTF: " << filename << std::endl;

	return res;
}

