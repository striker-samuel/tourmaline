#pragma once

#include <Engine/Shared/common.h>
#include <Engine/Renderer/vertex.h>

struct MeshUploadInput {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	glm::vec3 aabb_min; // im reading this directly from gltf.
	glm::vec3 aabb_max;
};

//// DEFAULT GEOS
//MeshUploadInput Quad;
//MeshUploadInput Triangle;
