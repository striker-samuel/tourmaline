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

#include <Engine/common.h>
#include <Engine/Vulkan/vulkan_setup.h>
#include <Engine/Renderer/vertex.h>

struct StandardBRDFMatAttachment {
	//texture handles to imageview
};

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
// Translucent, needs blending
// Hair, needs blending/ own brdf
// Cloth, sheen.
// Maybe more...

struct GPUMaterial {

	int vert_shader_id;
	int frag_shader_id;
	//attachments?
	// or maybe descriptor format abstraction. and flags. like opaque transparent/else
};

CameraPositioner_FirstPerson cameraPositioner(
	glm::vec3(0.0f, 0.0f, -10.0f), // position
	glm::vec3(0.0f, 0.0f, 1.0f),  // target (looking in +Z direction)
	glm::vec3(0.0f, 1.0f, 0.0f)   // up vector
);

std::vector<Vertex> quad_vertices = {
	{{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
	{{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
	{{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
	{{-0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
};

std::vector<uint32_t> quad_indices = {
	0, 1, 2, 2, 3, 0
};

struct GPULight {
	// Maybe we dont need scale and can compact here.
	glm::mat4 transform;
	//type
	//cone
	//color
	//intensity
} DefaultLight;

struct Mesh {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
};

Mesh Quad = Mesh(quad_vertices, quad_indices);

struct MeshData {
	std::vector<glm::vec3> vertices;
	std::vector<glm::vec3> normals;
	std::vector<uint32_t> indices;
};

struct InstanceData {

	// Identity Matrix initialization
	float transform[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	//uint32_t meshIndex;
	//uint32_t indexOffset;
	//uint32_t materialIndex;
	//uint32_t LOD;
};

struct UniformBufferObject {
	alignas(16) glm::mat4 model;
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
};

//Down like to use char for this.
typedef struct Color4 {
	unsigned char r;  // Color red value
	unsigned char g;  // Color green value
	unsigned char b;  // Color blue value
	unsigned char a;  // Color alpha value
};

// ---------------------------------
