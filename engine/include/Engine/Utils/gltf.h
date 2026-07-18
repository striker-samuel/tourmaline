#pragma once
#include <array>
#include <unordered_map>
#include <vector>
#include <string>
#include <iostream>
#include <filesystem>
#include <tiny_gltf.h>

#include <Engine/Shared/common.h>

enum class AlphaMode { Opaque, Mask, Blend };
struct GltfTextureRef {
	int textureIndex = -1;   // index into model.textures
	int imageIndex = -1;   // index into model.images  (model.textures[textureIndex].source)
	int samplerIndex = -1;   // index into model.samplers (model.textures[textureIndex].sampler)
	int texCoord = 0;    // which UV set
	float scaleOrStrength = 1.0f; // normal scale or occlusion strength (when applicable)
	bool valid() const { return textureIndex >= 0 && imageIndex >= 0; }
};

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

bool LoadModelFromAsciiFile(tinygltf::Model& model, const char* filename) {
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
std::filesystem::path UriFromTexture(const tinygltf::Model& model, GltfTextureRef& texture_ref);
static std::optional<GltfTextureRef> GltfResolveTexture(
	const tinygltf::Model& model,
	const tinygltf::TextureInfo& texInfo);
static std::optional<GltfTextureRef> GtlfResolveNormalTexture(
	const tinygltf::Model& model,
	const tinygltf::NormalTextureInfo& nrm);
static std::optional<GltfTextureRef> GLtfResolveOcclusionTexture(
	const tinygltf::Model& model,
	const tinygltf::OcclusionTextureInfo& occ);
static AlphaMode ParseAlphaMode(const std::string& s) {
	if (s == "MASK")  return AlphaMode::Mask;
	if (s == "BLEND") return AlphaMode::Blend;
	return AlphaMode::Opaque; // default or "OPAQUE"
}

//
std::vector<GltfMaterial> ExtractMaterials(const tinygltf::Model& model);
glm::mat4 GetNodeLocalTransform(const tinygltf::Node& n);
