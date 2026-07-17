#pragma once
#include <array>
#include <unordered_map>
#include <vector>
#include <string>
#include <iostream>
#include <filesystem>
#include <tiny_gltf.h>

#include <Engine/Shared/common.h>

namespace {
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

	};
};

static inline bool starts_with(const std::string& s, const char* prefix) {
	return s.rfind(prefix, 0) == 0;
}
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
static inline const uint8_t* gltfPtr(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t index)
{
	const auto& bv = m.bufferViews[a.bufferView];
	const auto& buf = m.buffers[bv.buffer];
	size_t elemSize = gltfElementSize(a);
	size_t stride = bv.byteStride ? bv.byteStride : elemSize;

	size_t base = (bv.byteOffset ? bv.byteOffset : 0) +
		(a.byteOffset ? a.byteOffset : 0);
	return buf.data.data() + base + index * stride;
}
static inline glm::vec3 readVec3ForientationFix(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC3
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec3(p[0] * -1, p[1], p[2] * -1);
}
static inline glm::vec4 readVec4F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC4
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec4(p[0], p[1], p[2], p[3]);
}
static inline glm::vec3 readVec3F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC3
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec3(p[0], p[1], p[2]);
}
static inline glm::vec2 readVec2F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC2
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec2(p[0], p[1]);
}
static inline uint32_t readIndex(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	using namespace tinygltf;
	const uint8_t* p = gltfPtr(m, a, i);
	switch (a.componentType) {
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return *reinterpret_cast<const uint8_t*>(p);
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return *reinterpret_cast<const uint16_t*>(p);
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return *reinterpret_cast<const uint32_t*>(p);
	default: throw std::runtime_error("Unsupported index componentType");
	}
}
static inline bool LoadModelFromAsciiFile(tinygltf::Model& model, const char* filename) {
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
//
std::filesystem::path UriFromTexture(const tinygltf::Model& model,GltfTextureRef& texture_ref) {
	const tinygltf::Image& img = model.images[texture_ref.imageIndex];

	// Case 1: External image via URI
	if (!img.uri.empty() && !starts_with(img.uri, "data:")) {

		// this duty falls on the caller as of now .
		//std::filesystem::path baseDir = gltfPath.parent_path();
		//std::filesystem::path imagePath = baseDir / img.uri;
		std::filesystem::path imagePath = img.uri;

		return imagePath.lexically_normal();
	}
	else {
		// No internal texures!
		assert(false && "Internal textures not supported for gld loading.");
		return "";
	}
}
static std::optional<GltfTextureRef> GltfResolveTexture(
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
static std::optional<GltfTextureRef> GtlfResolveNormalTexture(
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
static std::optional<GltfTextureRef> GLtfResolveOcclusionTexture(
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

//
std::vector<GltfMaterial> ExtractMaterials(const tinygltf::Model& model);
glm::mat4 GetNodeLocalTransform(const tinygltf::Node& n);
