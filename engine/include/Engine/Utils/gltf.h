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
#include <string>

#include <tiny_gltf.h>

#include <Engine/Renderer/vertex.h>


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

// Got this from ChatGpt
struct GltfTextureRef {
	int textureIndex = -1;   // index into model.textures
	int imageIndex = -1;   // index into model.images  (model.textures[textureIndex].source)
	int samplerIndex = -1;   // index into model.samplers (model.textures[textureIndex].sampler)
	int texCoord = 0;    // which UV set
	float scaleOrStrength = 1.0f; // normal scale or occlusion strength (when applicable)
	bool valid() const { return textureIndex >= 0 && imageIndex >= 0; }
};

enum class AlphaMode { Opaque, Mask, Blend };

struct GltfMaterial {
	std::string name;

	// PBR Metallic-Roughness core
	std::array<double, 4> baseColorFactor{ 1.0,1.0,1.0,1.0 };
	double metallicFactor = 1.0;
	double roughnessFactor = 1.0;

	std::optional<GltfTextureRef> baseColorTex;
	std::optional<GltfTextureRef> metallicRoughnessTex;

	// Other PBR textures/params
	std::optional<GltfTextureRef> normalTex;     // scale in scaleOrStrength
	std::optional<GltfTextureRef> occlusionTex;  // strength in scaleOrStrength
	std::optional<GltfTextureRef> emissiveTex;
	std::array<double, 3> emissiveFactor{ 0.0,0.0,0.0 };

	// Rasterization rules
	AlphaMode alphaMode = AlphaMode::Opaque;
	double alphaCutoff = 0.5;
	bool doubleSided = false;

	// For app-specific uses
	int gltfMaterialIndex = -1;
};

static std::optional<GltfTextureRef> ResolveTexture(
	const tinygltf::Model& model,
	const tinygltf::TextureInfo& texInfo)
{
	if (texInfo.index < 0 || texInfo.index >= (int)model.textures.size()) return std::nullopt;
	const tinygltf::Texture& tex = model.textures[texInfo.index];
	GltfTextureRef out{};
	out.textureIndex = texInfo.index;
	out.texCoord = texInfo.texCoord;

	if (tex.source >= 0 && tex.source < (int)model.images.size())
		out.imageIndex = tex.source;
	if (tex.sampler >= 0 && tex.sampler < (int)model.samplers.size())
		out.samplerIndex = tex.sampler;

	if (out.imageIndex < 0) return std::nullopt; // some files might omit source if using extensions
	return out;
}

static std::optional<GltfTextureRef> ResolveNormalTexture(
	const tinygltf::Model& model,
	const tinygltf::NormalTextureInfo& nrm)
{
	if (nrm.index < 0 || nrm.index >= (int)model.textures.size()) return std::nullopt;
	const tinygltf::Texture& tex = model.textures[nrm.index];
	GltfTextureRef out{};
	out.textureIndex = nrm.index;
	out.texCoord = nrm.texCoord;
	out.scaleOrStrength = (float)nrm.scale;

	if (tex.source >= 0 && tex.source < (int)model.images.size())
		out.imageIndex = tex.source;
	if (tex.sampler >= 0 && tex.sampler < (int)model.samplers.size())
		out.samplerIndex = tex.sampler;

	if (out.imageIndex < 0) return std::nullopt;
	return out;
}

static std::optional<GltfTextureRef> ResolveOcclusionTexture(
	const tinygltf::Model& model,
	const tinygltf::OcclusionTextureInfo& occ)
{
	if (occ.index < 0 || occ.index >= (int)model.textures.size()) return std::nullopt;
	const tinygltf::Texture& tex = model.textures[occ.index];
	GltfTextureRef out{};
	out.textureIndex = occ.index;
	out.texCoord = occ.texCoord;
	out.scaleOrStrength = (float)occ.strength;

	if (tex.source >= 0 && tex.source < (int)model.images.size())
		out.imageIndex = tex.source;
	if (tex.sampler >= 0 && tex.sampler < (int)model.samplers.size())
		out.samplerIndex = tex.sampler;

	if (out.imageIndex < 0) return std::nullopt;
	return out;
}

static AlphaMode ParseAlphaMode(const std::string& s) {
	if (s == "MASK")  return AlphaMode::Mask;
	if (s == "BLEND") return AlphaMode::Blend;
	return AlphaMode::Opaque; // default or "OPAQUE"
}

std::vector<GltfMaterial> ExtractMaterials(const tinygltf::Model& model)
{
	std::vector<GltfMaterial> out;
	out.reserve(model.materials.size());

	for (int i = 0; i < (int)model.materials.size(); ++i) {
		const tinygltf::Material& m = model.materials[i];
		GltfMaterial mat{};
		mat.gltfMaterialIndex = i;
		mat.name = m.name;

		// Core PBR factors
		if (m.pbrMetallicRoughness.baseColorFactor.size() == 4) {
			for (int k = 0; k < 4; ++k) mat.baseColorFactor[k] = m.pbrMetallicRoughness.baseColorFactor[k];
		}
		mat.metallicFactor = m.pbrMetallicRoughness.metallicFactor;
		mat.roughnessFactor = m.pbrMetallicRoughness.roughnessFactor;

		// Textures
		if (auto t = ResolveTexture(model, m.pbrMetallicRoughness.baseColorTexture))
			mat.baseColorTex = *t;

		if (auto t = ResolveTexture(model, m.pbrMetallicRoughness.metallicRoughnessTexture))
			mat.metallicRoughnessTex = *t;

		if (auto t = ResolveNormalTexture(model, m.normalTexture))
			mat.normalTex = *t;

		if (auto t = ResolveOcclusionTexture(model, m.occlusionTexture))
			mat.occlusionTex = *t;

		if (auto t = ResolveTexture(model, m.emissiveTexture))
			mat.emissiveTex = *t;

		if (m.emissiveFactor.size() == 3) {
			for (int k = 0; k < 3; ++k) mat.emissiveFactor[k] = m.emissiveFactor[k];
		}

		// Alpha + sidedness
		mat.alphaMode = ParseAlphaMode(m.alphaMode);
		mat.alphaCutoff = m.alphaCutoff;     // only meaningful for MASK
		mat.doubleSided = m.doubleSided;

		// (Optional) Example: read KHR_materials_unlit flag
		// if (m.additionalValues.count("unlit")) { ... } // or check m.extensions

		// (Optional) Example: KHR_texture_transform on baseColorTexture
		// auto it = m.extensions.find("KHR_texture_transform");
		// If present, read offset/scale/rotation and apply in your UV pipeline.

		out.emplace_back(std::move(mat));
	}

	return out;
}

struct MeshData {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	int materialIndex = -1;       // primitive.material (or -1)
	//int mode = TINYGLTF_MODE_TRIANGLES; // primitive.mode
	//std::string name;             // optional
};

// Models contains scenes, scenes contain meshes, and meshes: primitives.
class GltfModel {

private:

	tinygltf::Model gltfModel;

public:

	std::vector<tinygltf::Material> mats_;
	std::vector<MeshData> mesh_data;

	GltfModel(std::string model_path) {
		std::string warn, err;

		if (!loadModel(gltfModel, model_path.c_str())) {
			throw std::runtime_error(warn + err);
		}

		std::cout << gltfModel.meshes.size() << std::endl;
		// GltfMeshses are split into primitves. primitives are what would be faceSets in maya. Collections of primitives like tris, faces...
		// that share material, mode, geo

		for (const auto& mesh : gltfModel.meshes) {
			for (const auto& primitive : mesh.primitives) {


				if (primitive.mode == TINYGLTF_MODE_TRIANGLES) {
					continue;
				}

				MeshData data_;
				data_.materialIndex = primitive.material;

				// Extract vertices
				auto& posAcc = gltfModel.accessors[primitive.attributes.at("POSITION")];
				data_.vertices.reserve(posAcc.count);
				for (size_t i = 0; i < posAcc.count; ++i) {
					Vertex v{};
					v.pos = readVec3F(gltfModel, posAcc, i);


					// We need to do this because my coords system differs from gltf
					v.pos[0] *= -1;
					v.pos[2] *= -1; 
					
					
					v.color = { 1.0f, 1.0f, 1.0f };
					v.texCoord = { 0.0f, 0.0f }; // will fill if UVs exist
					data_.vertices.push_back(v);
				}

				if (auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end()) {
					auto& uvAcc = gltfModel.accessors[it->second];
					for (size_t i = 0; i < uvAcc.count && i < data_.vertices.size(); ++i) {
						glm::vec2 uv = readVec2F(gltfModel, uvAcc, i);
						data_.vertices[i].texCoord = uv; // flip V if your shader expects it: uv.y = 1.0f - uv.y;
					}
				}

				// Normals not implemented yet. Sadly :(
				//if (auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end()) {
				//	auto& nAcc = gltfModel.accessors[it->second];
				//	normals.reserve(nAcc.count);
				//	for (size_t i = 0; i < nAcc.count; ++i) {
				//		//vertices[i].
				//		//normals.push_back(readVec3F(gltfModel, nAcc, i));
				//	}
				//}

				if (primitive.indices >= 0) {
					auto& idxAcc = gltfModel.accessors[primitive.indices];
					for (size_t i = 0; i < idxAcc.count; ++i) {
						data_.indices.push_back(readIndex(gltfModel, idxAcc, i));
					}
				}

				mesh_data.push_back(data_);

			}
		}
	}
};
