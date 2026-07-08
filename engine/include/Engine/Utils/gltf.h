#pragma once

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
#include <filesystem>
#include <tiny_gltf.h>

#include <Engine/Vulkan/core.h>
#include <Engine/Renderer/vertex.h>
#include <Engine/Renderer/ToyResourceManager.h> // woudl be better to get rid of this

#define DEFAULT_MATERIAL_INDEX 0;

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

// HELPERS

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

inline glm::vec3 readVec3ForientationFix(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC3
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec3(p[0] * -1, p[1], p[2] * -1);
}

inline glm::vec4 readVec4F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC4
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec4(p[0], p[1], p[2], p[3]);
}

inline glm::vec3 readVec3F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
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

std::filesystem::path UriFromTexture(
	const tinygltf::Model& model,
	GltfTextureRef& texture_ref
) {
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
		assert(false);
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


MeshUploadInput SurfaceDataFromGltfPrimitive(
	const tinygltf::Model& model,
	const tinygltf::Primitive& primitive
) {
	MeshUploadInput data;

	// AABB. for culling	
	{
		auto it = primitive.attributes.find("POSITION");
		if (it == primitive.attributes.end())
			return data;

		int accessorIndex = it->second;
		const tinygltf::Accessor& accessor = model.accessors[accessorIndex];

		if (accessor.minValues.size() >= 3 && accessor.maxValues.size() >= 3) {

			// glTF stores AABB in the accessor
			const std::vector<double>& min = accessor.minValues;
			const std::vector<double>& max = accessor.maxValues;

			glm::vec3 aabbMin(min[0], min[1], min[2]);
			glm::vec3 aabbMax(max[0], max[1], max[2]);

			// Apply same conversion as positions
			glm::vec3 c0(-aabbMin.x, aabbMin.y, -aabbMin.z);
			glm::vec3 c1(-aabbMax.x, aabbMax.y, -aabbMax.z);

			glm::vec3 mn = glm::min(c0, c1);
			glm::vec3 mx = glm::max(c0, c1);

			data.aabb_min = mn;
			data.aabb_max = mx;


		}
		else {

			// I wont handle this. just assert and fail. 
			assert(false && "failed to get bb ");

		}

	}

	// Extract vertices
	auto& posAcc = model.accessors[primitive.attributes.at("POSITION")];
	data.vertices.reserve(posAcc.count);
	for (size_t i = 0; i < posAcc.count; ++i) {
		Vertex v{};
		v.pos = readVec3ForientationFix(model, posAcc, i);
		v.color = { 1.0f, 1.0f, 1.0f };
		v.texCoord = { 0.0f, 0.0f }; // will fill if UVs exist
		v.normal = { 0.0f, 1.0f, 0.0f }; // will replace if normals provided
		v.tangent = { 1.0f, 0.0f,0.0f, 1.0f }; // zero out tangent by default too ;)
		data.vertices.push_back(v);
	}

	if (auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end()) {
		auto& uvAcc = model.accessors[it->second];
		for (size_t i = 0; i < uvAcc.count && i < data.vertices.size(); ++i) {
			glm::vec2 uv = readVec2F(model, uvAcc, i);
			data.vertices[i].texCoord = uv; // flip V if your shader expects it: uv.y = 1.0f - uv.y;
		}
	}

	if (auto it = primitive.attributes.find("TANGENT"); it != primitive.attributes.end()) {
		auto& tangentAcc = model.accessors[it->second];
		for (size_t i = 0; i < tangentAcc.count && i < data.vertices.size(); ++i) {
			glm::vec4 tangent = readVec4F(model, tangentAcc, i);

			// Space conversion to my engine. Same as pos
			tangent.x *= -1.0f;
			tangent.z *= -1.0f;

			data.vertices[i].tangent = tangent;
		}
	}


	if (auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end()) {
		auto& nAcc = model.accessors[it->second];
		for (size_t i = 0; i < nAcc.count && i < data.vertices.size(); ++i) {
			glm::vec3 n = readVec3ForientationFix(model, nAcc, i);
			data.vertices[i].normal = glm::normalize(n);
		}
	}

	if (primitive.indices >= 0) {
		auto& idxAcc = model.accessors[primitive.indices];
		for (size_t i = 0; i < idxAcc.count; ++i) {
			data.indices.push_back(readIndex(model, idxAcc, i));
		}
	}

	return data;

}

std::vector<GltfMaterial> ExtractMaterials(const tinygltf::Model& model)
{
	std::vector<GltfMaterial> out;
	out.reserve(model.materials.size());

	for (int i = 0; i < (int)model.materials.size(); ++i) {
		const tinygltf::Material& m = model.materials[i];
		GltfMaterial mat{};
		mat.name = m.name;

		// Core PBR factors
		if (m.pbrMetallicRoughness.baseColorFactor.size() == 4) {
			for (int k = 0; k < 4; ++k) mat.baseColorFactor[k] = m.pbrMetallicRoughness.baseColorFactor[k];
		}
		mat.metallicFactor = m.pbrMetallicRoughness.metallicFactor;
		mat.roughnessFactor = m.pbrMetallicRoughness.roughnessFactor;

		// Textures
		if (auto t = GltfResolveTexture(model, m.pbrMetallicRoughness.baseColorTexture))
			mat.baseColorTex = *t;

		if (auto t = GltfResolveTexture(model, m.pbrMetallicRoughness.metallicRoughnessTexture))
			mat.metallicRoughnessTex = *t;

		if (auto t = GtlfResolveNormalTexture(model, m.normalTexture))
			mat.normalTex = *t;

		if (auto t = GLtfResolveOcclusionTexture(model, m.occlusionTexture))
			mat.occlusionTex = *t;

		if (auto t = GltfResolveTexture(model, m.emissiveTexture))
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

glm::mat4 GetNodeLocalTransform(const tinygltf::Node& n)
{
	if (!n.matrix.empty()) {
		glm::mat4 m(1.0f);
		memcpy(glm::value_ptr(m), n.matrix.data(), sizeof(float) * 16);
		return m;
	}

	glm::vec3 t(0.0f);
	glm::quat r(1, 0, 0, 0);
	glm::vec3 s(1.0f);

	if (!n.translation.empty())
		t = glm::vec3(n.translation[0], n.translation[1], n.translation[2]);

	if (!n.rotation.empty())
		r = glm::quat(
			n.rotation[3],
			n.rotation[0],
			n.rotation[1],
			n.rotation[2]
		);

	if (!n.scale.empty())
		s = glm::vec3(n.scale[0], n.scale[1], n.scale[2]);

	return glm::translate(glm::mat4(1.0f), t)
		* glm::mat4_cast(r)
		* glm::scale(glm::mat4(1.0f), s);
}



struct GltfToSceneImporter {
	Scene& scene;
	const tinygltf::Model& model;
	std::filesystem::path gltfModelPath; // full path to .gltf/.glb on disk
	static constexpr uint32_t kInvalidMat = 0xFFFFFFFFu;
	std::vector<uint32_t> materialCache;
	std::function<uint32_t(MeshUploadInput& data)> createMeshCallback_;
	std::function<uint32_t()> createMaterialCallback_;

	GltfToSceneImporter(
		ToyMeshManager& inMan,
		Scene& inScene,
		const tinygltf::Model& inModel,
		std::filesystem::path inGltfModelPath,
		std::function<uint32_t()> createMeshCallback,
		std::function<uint32_t()> createMaterialCallback
	)
		: scene(inScene)
		, model(inModel)
		, gltfModelPath(std::move(inGltfModelPath))
		, materialCache(inModel.materials.size(), kInvalidMat)
		, createMeshCallback_(createMeshCallback)
		, createMaterialCallback_(createMaterialCallback)
	{}

	uint32_t getOrCreateMaterial(int gltfMaterialIndex) {
		if (gltfMaterialIndex < 0) {
			return DEFAULT_MATERIAL_INDEX;
		}

		// glTF guarantees material indices are within model.materials
		// (assuming the file is valid). If you want belt-and-suspenders:
		if ((size_t)gltfMaterialIndex >= materialCache.size()) {
			return DEFAULT_MATERIAL_INDEX;
		}

		uint32_t& slot = materialCache[(size_t)gltfMaterialIndex];
		if (slot != kInvalidMat) {
			return slot;
		}

		// NOTE: Avoid storing baseDir as a dangling c_str(). Build it here.
		const std::string baseDir = gltfModelPath.parent_path().string();

		// TODO : This should be done onlt once instead of here!
		auto mat = ExtractMaterials(model)[gltfMaterialIndex];

		TextureHandle colorTexHandle{};

		// TODO : Upload all textures
		{

			auto color_uri = UriFromTexture(model, *mat.baseColorTex);

			ktxTexture2* ktxTex = nullptr;
			KTX_error_code res = ktxTexture2_CreateFromNamedFile(
				(baseDir.c_str() / color_uri).string().c_str(),
				KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, // <<–– correct flags
				&ktxTex
			);

			if (res != KTX_SUCCESS) {
				throw std::runtime_error("KTX load failed");
			}

			// awful
			colorTexHandle = man.ctx->GetOrCreateTexure(color_uri.string(), *ktxTex);
			ktxTexture2_Destroy(ktxTex);
		}

		return man.CreateMaterial(
			{
				.diffuse = colorTexHandle
			}
		).index();

	}

	uint32_t getOrCreateMesh(tinygltf::Primitive primitive) {
		auto mdata = SurfaceDataFromGltfPrimitive(model, primitive);
		return createMeshCallback_(mdata).index();
	}

	void importNodeRecursive(int gltfNodeIndex, int parentSceneNode, int level) {
		const tinygltf::Node& gltfNode = model.nodes[gltfNodeIndex];

		// Always create a "group" node that owns name + transform and preserves hierarchy.
		const int groupNode = addNode(scene, parentSceneNode, level);

		{
			const int nameIndex = (int)scene.nodeNames.size();
			scene.nodeNames.push_back(gltfNode.name.empty() ? "Node" : gltfNode.name);
			scene.nameForNode[groupNode] = nameIndex;
		}

		scene.localTransform[groupNode] = GetNodeLocalTransform(gltfNode);
		scene.globalTransform[groupNode] = glm::mat4(1.0f); // resolved later

		// Unroll primitives into separate meshes (your current approach).
		if (gltfNode.mesh >= 0) {
			const tinygltf::Mesh& gltfMesh = model.meshes[gltfNode.mesh];

			for (int primIndex = 0; primIndex < (int)gltfMesh.primitives.size(); ++primIndex) {
				const tinygltf::Primitive& primitive = gltfMesh.primitives[primIndex];

				const int primNode = addNode(scene, groupNode, level + 1);

				scene.localTransform[primNode] = glm::mat4(1.0f);
				scene.globalTransform[primNode] = glm::mat4(1.0f);

				scene.meshForNode[primNode] = getOrCreateMesh(primitive);
				scene.materialForNode[primNode] = getOrCreateMaterial(primitive.material);

				markAsChanged(scene, primNode);
			}
		}

		// Recurse children under the group node to preserve hierarchy.
		for (int child : gltfNode.children) {
			importNodeRecursive(child, groupNode, level + 1);
		}
	}
};
