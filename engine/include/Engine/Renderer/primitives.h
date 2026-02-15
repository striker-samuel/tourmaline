#pragma once

#include <Engine/Shared/common.h>
#include <Engine/Renderer/vertex.h>

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

MeshUploadInput Quad = MeshUploadInput(quad_vertices, quad_indices_ccw);
MeshUploadInput Triangle = MeshUploadInput(triangle_vertices, triangle_indices_ccw);

