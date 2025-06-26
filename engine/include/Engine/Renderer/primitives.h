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

#include <Engine/Vulkan/vulkan_setup.h>
#include <Engine/common.h>
#include <Engine/Renderer/vertex.h>



typedef glm::vec3 Vector3;
struct InstanceData {
	float transform[16];
	uint32_t meshIndex;
	uint32_t materialIndex;
	uint32_t LOD;
	uint32_t indexOffset;
};

struct UniformBufferObject {
	alignas(16) glm::mat4 model;
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
};


typedef struct Color4 {
	unsigned char r;  // Color red value
	unsigned char g;  // Color green value
	unsigned char b;  // Color blue value
	unsigned char a;  // Color alpha value
};


typedef struct Transform {
	glm::vec3 translation = { 0.0f, 0.0f, 0.0f };
	glm::mat4 rotation = glm::mat4(1.0f);  // Identity matrix
	glm::vec3 scale = { 1, 1, 1 };           // Scale
	Transform* parent = nullptr;
} Transform;

glm::mat4 getWorldTransform(const Transform& transform) {
	glm::mat4 model = glm::translate(glm::mat4(1.0f), transform.translation);
	model *= transform.rotation;
	model = glm::scale(model, transform.scale);

	if (transform.parent) {
		return getWorldTransform(*transform.parent) * model;
	}
	return model;
}

glm::mat4 getModelMatrix(Transform* t) {
	glm::mat4 model = glm::translate(glm::mat4(1.0f), t->translation);
	model *= t->rotation;
	model = glm::scale(model, t->scale);
	return model;
}

glm::vec3 extractVertexFromBuffer(const std::vector<uint8_t>& buffer,
	const tinygltf::Accessor& accessor,
	size_t index) {
	size_t byteOffset = accessor.byteOffset + accessor.bufferView;
	size_t componentSize =
		sizeof(float) * 3;  // Each vertex has 3 components: x, y, z

	// Calculate the position of the index in the buffer (each vertex takes 3
	// floats for a vec3)
	size_t byteIndex = byteOffset + (index * componentSize);

	// Extract the data as floats (x, y, z)
	const float* vertexData = reinterpret_cast<const float*>(&buffer[byteIndex]);

	return glm::vec3(vertexData[0], vertexData[1], vertexData[2]);
}

glm::vec3 extractNormalFromBuffer(const std::vector<uint8_t>& buffer,
	const tinygltf::Accessor& accessor,
	size_t index) {
	size_t byteOffset = accessor.byteOffset + accessor.bufferView;
	size_t componentSize = sizeof(float) * 3;  // Each normal is a vec3 (x, y, z)

	// Calculate the position of the index in the buffer
	size_t byteIndex = byteOffset + (index * componentSize);

	// Extract the data as floats (x, y, z)
	const float* normalData = reinterpret_cast<const float*>(&buffer[byteIndex]);

	return glm::vec3(normalData[0], normalData[1], normalData[2]);
}

uint32_t extractIndexFromBuffer(const std::vector<uint8_t>& buffer,
	const tinygltf::Accessor& accessor,
	size_t index) {
	size_t byteOffset = accessor.byteOffset + accessor.bufferView;
	size_t componentSize = sizeof(uint32_t);  // Assuming 32-bit indices (4 bytes)

	// Calculate the position of the index in the buffer
	size_t byteIndex = byteOffset + (index * componentSize);

	// Extract the data as a 32-bit unsigned integer
	const uint32_t* indexData =
		reinterpret_cast<const uint32_t*>(&buffer[byteIndex]);

	return indexData[0];
}

glm::vec2 extractUVFromBuffer(const std::vector<uint8_t>& buffer,
	const tinygltf::Accessor& accessor,
	size_t index) {
	// Calculate the byte offset for the UV data
	size_t byteOffset = accessor.byteOffset + accessor.bufferView;
	size_t componentSize =
		sizeof(glm::vec2);  // Assuming 32-bit indices (4 bytes)
	size_t byteIndex = byteOffset + (index * componentSize);

	// Extract the UV data as two floats
	const float* uvData = reinterpret_cast<const float*>(&buffer[byteIndex]);
	return glm::vec2(uvData[0], uvData[1]);
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

struct MeshData {
	std::vector<glm::vec3> vertices;
	std::vector<glm::vec3> normals;
	std::vector<uint32_t> indices;
};

void loadGltfToMesh(const tinygltf::Model& gltfModel, MeshData& meshData) {
	for (const auto& mesh : gltfModel.meshes) {
		for (const auto& primitive : mesh.primitives) {
			// Extract vertices
			auto& positionAccessor =
				gltfModel.accessors[primitive.attributes.at("POSITION")];
			auto& positionBufferView =
				gltfModel.bufferViews[positionAccessor.bufferView];
			auto& positionBuffer = gltfModel.buffers[positionBufferView.buffer];
			for (size_t i = 0; i < positionAccessor.count; ++i) {
				glm::vec3 vertex =
					extractVertexFromBuffer(positionBuffer.data, positionAccessor, i);
				meshData.vertices.push_back(vertex);
			}

			// Extract normals (if available)
			if (primitive.attributes.find("NORMAL") != primitive.attributes.end()) {
				auto& normalAccessor =
					gltfModel.accessors[primitive.attributes.at("NORMAL")];
				auto& normalBufferView =
					gltfModel.bufferViews[normalAccessor.bufferView];
				auto& normalBuffer = gltfModel.buffers[normalBufferView.buffer];
				for (size_t i = 0; i < normalAccessor.count; ++i) {
					glm::vec3 normal =
						extractNormalFromBuffer(normalBuffer.data, normalAccessor, i);
					meshData.normals.push_back(normal);
				}
			}

			// Extract indices
			if (primitive.indices >= 0) {
				auto& indexAccessor = gltfModel.accessors[primitive.indices];
				auto& indexBufferView = gltfModel.bufferViews[indexAccessor.bufferView];
				auto& indexBuffer = gltfModel.buffers[indexBufferView.buffer];
				for (size_t i = 0; i < indexAccessor.count; ++i) {
					uint32_t index =
						extractIndexFromBuffer(indexBuffer.data, indexAccessor, i);
					meshData.indices.push_back(index);
				}
			}
		}
	}
}

class Mesh {
public:
	Transform* transform{};

	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;


	Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices) {
		this->vertices = vertices;
		this->indices = indices;
	}

	Mesh(std::string model_path, Transform* parent = nullptr) {
		transform = parent;

		tinyobj::attrib_t attrib;

		std::vector<tinyobj::shape_t> shapes;
		std::vector<tinyobj::material_t> materials;

		std::string warn, err;

		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
			model_path.c_str())) {
			throw std::runtime_error(warn + err);
		}

		std::unordered_map<Vertex, uint32_t> uniqueVertices{};

		for (const auto& shape : shapes) {
			for (const auto& index : shape.mesh.indices) {
				Vertex vertex{};

				vertex.pos = { attrib.vertices[3 * index.vertex_index + 0],
							  attrib.vertices[3 * index.vertex_index + 1],
							  attrib.vertices[3 * index.vertex_index + 2] };

				vertex.texCoord = {
					attrib.texcoords[2 * index.texcoord_index + 0],
					1.0f - attrib.texcoords[2 * index.texcoord_index + 1] };

				vertex.color = { 1.0f, 1.0f, 1.0f };

				if (uniqueVertices.count(vertex) == 0) {
					uniqueVertices[vertex] = static_cast<uint32_t>(vertices.size());
					vertices.push_back(vertex);
				}

				indices.push_back(uniqueVertices[vertex]);
			}
		}
	}
};

class MeshTwo {
public:
	std::vector<Vertex> vertices;
	// std::vector<glm::vec3> vertices;
	std::vector<uint32_t> indices;
	std::vector<glm::vec3> normals;
	std::vector<glm::vec2> uvs;
	tinygltf::Model gltfModel;

	MeshTwo(std::string model_path) {
		std::string warn, err;

		if (!loadModel(gltfModel, model_path.c_str())) {
			throw std::runtime_error(warn + err);
		}

		// std::unordered_map<Vertex, uint32_t> uniqueVertices{};

		for (const auto& mesh : gltfModel.meshes) {
			for (const auto& primitive : mesh.primitives) {
				// Extract vertices
				auto& positionAccessor =
					gltfModel.accessors[primitive.attributes.at("POSITION")];
				auto& positionBufferView =
					gltfModel.bufferViews[positionAccessor.bufferView];
				auto& positionBuffer = gltfModel.buffers[positionBufferView.buffer];

				for (size_t i = 0; i < positionAccessor.count; ++i) {
					glm::vec3 vertex_extracted =
						extractVertexFromBuffer(positionBuffer.data, positionAccessor, i);

					Vertex vertex{};

					vertex.pos = vertex_extracted; /* {
							 attrib.vertices[3 * index.vertex_index + 0],
							 attrib.vertices[3 * index.vertex_index + 1],
							 attrib.vertices[3 * index.vertex_index + 2]
					 };*/

					 /*	vertex.texCoord = {
									 attrib.texcoords[2 * index.texcoord_index + 0],
									 1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
							 };*/

					vertex.texCoord = { 0, 0.01 * i };

					vertex.color = { 1.0f, 1.0f, 1.0f };

					vertices.push_back(vertex);
				}

				// Extract UV positions (if available)
				if (primitive.attributes.find("TEXCOORD_0") !=
					primitive.attributes.end()) {
					auto& uvAccessor =
						gltfModel.accessors[primitive.attributes.at("TEXCOORD_0")];
					auto& uvBufferView = gltfModel.bufferViews[uvAccessor.bufferView];
					auto& uvBuffer = gltfModel.buffers[uvBufferView.buffer];

					for (size_t i = 0; i < uvAccessor.count; ++i) {
						glm::vec2 uv =
							extractNormalFromBuffer(uvBuffer.data, uvAccessor, i);
						uvs.push_back(uv);

						vertices[i].texCoord = uv;
					}

					// Extract normals (if available)
					if (primitive.attributes.find("NORMAL") !=
						primitive.attributes.end()) {
						auto& normalAccessor =
							gltfModel.accessors[primitive.attributes.at("NORMAL")];
						auto& normalBufferView =
							gltfModel.bufferViews[normalAccessor.bufferView];
						auto& normalBuffer = gltfModel.buffers[normalBufferView.buffer];
						for (size_t i = 0; i < normalAccessor.count; ++i) {
							glm::vec3 normal =
								extractNormalFromBuffer(normalBuffer.data, normalAccessor, i);
							normals.push_back(normal);
						}
					}

					// Extract indices
					if (primitive.indices >= 0) {
						auto& indexAccessor = gltfModel.accessors[primitive.indices];
						auto& indexBufferView =
							gltfModel.bufferViews[indexAccessor.bufferView];
						auto& indexBuffer = gltfModel.buffers[indexBufferView.buffer];
						for (size_t i = 0; i < indexAccessor.count; ++i) {
							uint32_t index =
								extractIndexFromBuffer(indexBuffer.data, indexAccessor, i);
							indices.push_back(index);
						}
					}
				}
			}
		}
	}
};
