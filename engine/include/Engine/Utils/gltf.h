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

#include <Engine/Renderer/vertex.h>

class GltfMesh {
public:
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<glm::vec3> normals;
	std::vector<glm::vec2> uvs;
	tinygltf::Model gltfModel;

	GltfMesh(std::string model_path) {
		std::string warn, err;

		if (!loadModel(gltfModel, model_path.c_str())) {
			throw std::runtime_error(warn + err);
		}

		for (const auto& mesh : gltfModel.meshes) {
			for (const auto& primitive : mesh.primitives) {

				const size_t baseVertex = vertices.size();
				// Extract vertices
				auto& posAcc = gltfModel.accessors[primitive.attributes.at("POSITION")];
				for (size_t i = 0; i < posAcc.count; ++i) {
					Vertex v{};
					v.pos = readVec3F(gltfModel, posAcc, i);
					v.color = { 1.0f, 1.0f, 1.0f };
					v.texCoord = { 0.0f, 0.0f }; // will fill if UVs exist
					vertices.push_back(v);
				}

				if (auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end()) {
					auto& uvAcc = gltfModel.accessors[it->second];
					for (size_t i = 0; i < uvAcc.count && i < vertices.size(); ++i) {
						glm::vec2 uv = readVec2F(gltfModel, uvAcc, i);
						vertices[i].texCoord = uv; // flip V if your shader expects it: uv.y = 1.0f - uv.y;
					}
				}

				if (auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end()) {
					auto& nAcc = gltfModel.accessors[it->second];
					normals.reserve(nAcc.count);
					for (size_t i = 0; i < nAcc.count; ++i) {
						normals.push_back(readVec3F(gltfModel, nAcc, i));
					}
				}

				if (primitive.indices >= 0) {
					auto& idxAcc = gltfModel.accessors[primitive.indices];
					for (size_t i = 0; i < idxAcc.count; ++i) {
						indices.push_back(baseVertex + readIndex(gltfModel, idxAcc, i));
					}
				}
				
			}
		}
	}
};
