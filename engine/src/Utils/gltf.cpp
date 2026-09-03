#define TINYGLTF_IMPLEMENTATION

#include "ktx.h"

#include <Engine/Utils/gltf.h>
#include <Engine/Utils/texture_loader.h>
#include <Engine/core.h>


#include <array>
#include <unordered_map>
#include <vector>
#include <string>
#include <iostream>
#include <filesystem>
#include <cmath>

namespace {
	constexpr uint32_t DEFAULT_MATERIAL_INDEX = 0;
	// Local glTF parse enum; maps to smvk::AlphaMode on material creation.
	enum class GltfAlphaMode { Opaque, Mask, Blend };
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
		double emissiveStrength = 1.0;

		// Extensions
		bool unlit = false;

		// Rasterization rules
		GltfAlphaMode alphaMode = GltfAlphaMode::Opaque;
		double alphaCutoff = 0.5;
		bool doubleSided = false;

	};
	bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
	size_t gltfElementSize(const tinygltf::Accessor& a) {
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
	const uint8_t* gltfPtr(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t index)
	{
		const auto& bv = m.bufferViews[a.bufferView];
		const auto& buf = m.buffers[bv.buffer];
		size_t elemSize = gltfElementSize(a);
		size_t stride = bv.byteStride ? bv.byteStride : elemSize;

		size_t base = (bv.byteOffset ? bv.byteOffset : 0) +
			(a.byteOffset ? a.byteOffset : 0);
		return buf.data.data() + base + index * stride;
	}
	GltfAlphaMode ParseAlphaMode(const std::string& s) {
		if (s == "MASK")  return GltfAlphaMode::Mask;
		if (s == "BLEND") return GltfAlphaMode::Blend;
		return GltfAlphaMode::Opaque; // default or "OPAQUE"
	}
	glm::vec3 readVec3ForientationFix(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
		// Expect FLOAT VEC3
		const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
		return glm::vec3(p[0] * -1, p[1], p[2] * -1);
	}
	glm::vec4 readVec4F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
		// Expect FLOAT VEC4
		const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
		return glm::vec4(p[0], p[1], p[2], p[3]);
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
	// -------------------------------------------------------------------------
	// Tangent-frame synthesis for primitives that lack authoring data.
	//
	// Normal maps need a per-vertex tangent frame (N, T, w). Many glTF assets
	// omit TANGENT (and some omit NORMAL); leaving the loader default of
	// T=(1,0,0,1) in place produces visibly broken normal-mapped lighting.
	// These helpers derive smooth normals (area-weighted) and tangents
	// (UV-gradient accumulation a la Lengyel, Gram-Schmidt orthogonalized
	// against N with a handedness sign) in the already-converted engine
	// space, so the result matches the shader's TBN construction.
	// -------------------------------------------------------------------------
	void computeSmoothNormals(smvk::StaticMeshUpload& data) {
		const size_t vcount = data.vertices.size();
		if (vcount == 0) return;
		for (auto& v : data.vertices) v.normal = glm::vec3(0.0f);
		auto accumTri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
			if (i0 >= vcount || i1 >= vcount || i2 >= vcount) return;
			const glm::vec3& p0 = data.vertices[i0].pos;
			const glm::vec3& p1 = data.vertices[i1].pos;
			const glm::vec3& p2 = data.vertices[i2].pos;
			const glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
			if (glm::dot(fn, fn) <= 1e-24f) return; // degenerate
			data.vertices[i0].normal += fn;
			data.vertices[i1].normal += fn;
			data.vertices[i2].normal += fn;
		};
		if (!data.indices.empty()) {
			for (size_t t = 0; t + 2 < data.indices.size() + 1; t += 3)
				accumTri(data.indices[t], data.indices[t + 1], data.indices[t + 2]);
		}
		else {
			for (uint32_t t = 0; t + 2 < vcount + 1; t += 3)
				accumTri(t, t + 1, t + 2);
		}
		for (auto& v : data.vertices) {
			const float l2 = glm::dot(v.normal, v.normal);
			v.normal = (l2 > 1e-24f) ? glm::normalize(v.normal) : glm::vec3(0.0f, 1.0f, 0.0f);
		}
	}
	void generateTangents(smvk::StaticMeshUpload& data) {
		const size_t vcount = data.vertices.size();
		if (vcount == 0) return;
		std::vector<glm::vec3> tan1(vcount, glm::vec3(0.0f));
		std::vector<glm::vec3> tan2(vcount, glm::vec3(0.0f));
		auto accumTri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
			if (i0 >= vcount || i1 >= vcount || i2 >= vcount) return;
			const glm::vec3& p0 = data.vertices[i0].pos;
			const glm::vec3& p1 = data.vertices[i1].pos;
			const glm::vec3& p2 = data.vertices[i2].pos;
			const glm::vec2& w0 = data.vertices[i0].texCoord;
			const glm::vec2& w1 = data.vertices[i1].texCoord;
			const glm::vec2& w2 = data.vertices[i2].texCoord;
			const glm::vec3 x1 = p1 - p0;
			const glm::vec3 x2 = p2 - p0;
			const glm::vec2 s1 = w1 - w0;
			const glm::vec2 s2 = w2 - w0;
			const float denom = s1.x * s2.y - s2.x * s1.y;
			if (fabsf(denom) <= 1e-12f) return; // degenerate UVs: no tangent info
			const float r = 1.0f / denom;
			const glm::vec3 sdir = (x1 * s2.y - x2 * s1.y) * r;
			const glm::vec3 tdir = (x2 * s1.x - x1 * s2.x) * r;
			tan1[i0] += sdir; tan1[i1] += sdir; tan1[i2] += sdir;
			tan2[i0] += tdir; tan2[i1] += tdir; tan2[i2] += tdir;
		};
		if (!data.indices.empty()) {
			for (size_t t = 0; t + 2 < data.indices.size() + 1; t += 3)
				accumTri(data.indices[t], data.indices[t + 1], data.indices[t + 2]);
		}
		else {
			for (uint32_t t = 0; t + 2 < vcount + 1; t += 3)
				accumTri(t, t + 1, t + 2);
		}
		for (size_t i = 0; i < vcount; ++i) {
			const glm::vec3& n = data.vertices[i].normal;
			// Gram-Schmidt: make accumulated tangent orthogonal to N.
			glm::vec3 t = tan1[i] - n * glm::dot(n, tan1[i]);
			if (glm::dot(t, t) > 1e-24f) {
				t = glm::normalize(t);
			}
			else {
				// No usable UV gradient: build an arbitrary frame orthogonal to N.
				const glm::vec3 axis =
					(fabsf(n.y) < 0.99f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
				t = glm::normalize(glm::cross(axis, n));
				if (glm::dot(t, t) <= 1e-24f) t = glm::vec3(1.0f, 0.0f, 0.0f);
			}
			const float w = (glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f;
			data.vertices[i].tangent = glm::vec4(t, w);
		}
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
	std::filesystem::path UriFromTexture(
		const tinygltf::Model& model,
		const GltfTextureRef& texture_ref)
	{
		const tinygltf::Image& img = model.images[texture_ref.imageIndex];

		// Case 1: External image via URI
		if (!img.uri.empty() && !starts_with(img.uri, "data:")) {
			std::filesystem::path imagePath = img.uri;
			return imagePath.lexically_normal();
		}
		else {
			assert(false && "Internal textures not supported for gltf loading.");
			return "";
		}
	}

	inline smvk::TextureHandle LoadKtxTextureCached(
		smvk::Cluster& cluster,
		const std::filesystem::path& baseDir,
		const tinygltf::Model& model,
		const GltfTextureRef& ref,
		std::unordered_map<int, smvk::TextureHandle>& cache)
	{
		auto it = cache.find(ref.imageIndex);
		if (it != cache.end()) return it->second;
		std::filesystem::path uri = UriFromTexture(model, ref);
		if (uri.empty()) return {};
		std::filesystem::path fullPath = (baseDir / uri).lexically_normal();
		auto holder = smvk::LoadKtxTextureFile(
			cluster.ctx_, fullPath.string());
		if (!holder.valid()) return {};
		smvk::TextureHandle h = holder;
		cluster.textures.create(std::move(holder));
		cache[ref.imageIndex] = h;
		return h;
	}
};

GltfToSceneImporter::GltfToSceneImporter(smvk::Cluster& inCluster, const tinygltf::Model& inModel, std::filesystem::path inGltfModelPath)
	: cluster(inCluster)
	, model(inModel)
	, gltfModelPath(std::move(inGltfModelPath))
	, materialCache(inModel.materials.size())
{
	meshCache.resize(inModel.meshes.size());
	for (size_t i = 0; i < inModel.meshes.size(); ++i) {
		meshCache[i].resize(
			inModel.meshes[i].primitives.size()
		);
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
std::optional<GltfTextureRef> GltfResolveTexture(const tinygltf::Model& model, const tinygltf::TextureInfo& texInfo)
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
std::optional<GltfTextureRef> GtlfResolveNormalTexture(const tinygltf::Model& model, const tinygltf::NormalTextureInfo& nrm)
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
std::optional<GltfTextureRef> GLtfResolveOcclusionTexture(const tinygltf::Model& model, const tinygltf::OcclusionTextureInfo& occ)
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
static GltfMaterial ExtractMaterial(const tinygltf::Model& model, int materialIndex)
{
	assert(materialIndex >= 0);
	assert(
		static_cast<size_t>(materialIndex) <
		model.materials.size()
	);

	const tinygltf::Material& m =
		model.materials[materialIndex];

	GltfMaterial mat{};

	mat.name = m.name;

	// -------------------------------------------------------------------------
	// PBR Metallic-Roughness
	// -------------------------------------------------------------------------

	if (m.pbrMetallicRoughness.baseColorFactor.size() == 4) {
		for (int k = 0; k < 4; ++k) {
			mat.baseColorFactor[k] =
				m.pbrMetallicRoughness.baseColorFactor[k];
		}
	}

	mat.metallicFactor =
		m.pbrMetallicRoughness.metallicFactor;

	mat.roughnessFactor =
		m.pbrMetallicRoughness.roughnessFactor;

	// -------------------------------------------------------------------------
	// Textures
	// -------------------------------------------------------------------------

	if (auto t = GltfResolveTexture(
		model,
		m.pbrMetallicRoughness.baseColorTexture))
	{
		mat.baseColorTex = *t;
	}

	if (auto t = GltfResolveTexture(
		model,
		m.pbrMetallicRoughness.metallicRoughnessTexture))
	{
		mat.metallicRoughnessTex = *t;
	}

	if (auto t = GtlfResolveNormalTexture(
		model,
		m.normalTexture))
	{
		mat.normalTex = *t;
	}

	if (auto t = GLtfResolveOcclusionTexture(
		model,
		m.occlusionTexture))
	{
		mat.occlusionTex = *t;
	}

	if (auto t = GltfResolveTexture(
		model,
		m.emissiveTexture))
	{
		mat.emissiveTex = *t;
	}

	if (m.emissiveFactor.size() == 3) {
		for (int k = 0; k < 3; ++k) {
			mat.emissiveFactor[k] =
				m.emissiveFactor[k];
		}
	}

	// Extensions: KHR_materials_unlit, KHR_materials_emissive_strength
	{
		auto it = m.extensions.find("KHR_materials_unlit");
		if (it != m.extensions.end()) {
			mat.unlit = true;
		}
		auto it2 = m.extensions.find("KHR_materials_emissive_strength");
		if (it2 != m.extensions.end() && it2->second.IsObject()) {
			const auto& obj = it2->second.Get<tinygltf::Value::Object>();
			auto f = obj.find("emissiveStrength");
			if (f != obj.end() && f->second.IsNumber()) {
				mat.emissiveStrength = f->second.GetNumberAsDouble();
			}
		}
	}

	// -------------------------------------------------------------------------
	// Rasterization
	// -------------------------------------------------------------------------

	mat.alphaMode =
		ParseAlphaMode(m.alphaMode);

	mat.alphaCutoff =
		m.alphaCutoff;

	mat.doubleSided =
		m.doubleSided;

	return mat;
}
void GltfToSceneImporter::preloadTextures()
{
	// Collect every image referenced by any material (dedup by imageIndex).
	// Per-material creation later hits textureCache; anything missed here
	// still loads sequentially via LoadKtxTextureCached.
	std::vector<int> imageIndices;
	imageIndices.reserve(model.images.size());
	{
		std::vector<bool> seen(model.images.size(), false);
		auto note = [&](const std::optional<GltfTextureRef>& ref) {
			if (!ref || ref->imageIndex < 0 ||
				ref->imageIndex >= (int)model.images.size())
				return;
			if (!seen[ref->imageIndex]) {
				seen[ref->imageIndex] = true;
				imageIndices.push_back(ref->imageIndex);
			}
		};
		for (size_t mi = 0; mi < model.materials.size(); ++mi) {
			const tinygltf::Material& m = model.materials[mi];
			note(GltfResolveTexture(model, m.pbrMetallicRoughness.baseColorTexture));
			note(GltfResolveTexture(model, m.pbrMetallicRoughness.metallicRoughnessTexture));
			note(GtlfResolveNormalTexture(model, m.normalTexture));
			note(GLtfResolveOcclusionTexture(model, m.occlusionTexture));
			note(GltfResolveTexture(model, m.emissiveTexture));
		}
	}

	const std::filesystem::path baseDir = gltfModelPath.parent_path();

	std::vector<smvk::KtxLoadJob> jobs;
	jobs.reserve(imageIndices.size());
	for (int imageIndex : imageIndices) {
		if (textureCache.find(imageIndex) != textureCache.end())
			continue;
		const tinygltf::Image& img = model.images[imageIndex];
		// Embedded / data-URI images are not supported (same policy as
		// UriFromTexture); skip them here, the sequential path asserts.
		if (img.uri.empty() || starts_with(img.uri, "data:"))
			continue;
		std::filesystem::path fullPath =
			(baseDir / std::filesystem::path(img.uri)).lexically_normal();
		jobs.push_back({ imageIndex, fullPath.string() });
	}

	if (jobs.empty()) return;

	smvk::ParallelTextureStats stats{};
	auto holders = smvk::LoadKtxTexturesParallel(
		cluster.ctx_, jobs, {}, &stats);

	for (auto& [imageIndex, holder] : holders) {
		smvk::TextureHandle h = holder;
		cluster.textures.create(std::move(holder));
		textureCache[imageIndex] = h;
	}

	std::cout << "Preloaded " << stats.succeeded << "/" << stats.requested
		<< " textures in " << stats.seconds << "s ("
		<< (stats.failed ? std::to_string(stats.failed) + " failed, " : "")
		<< jobs.size() << " jobs)" << std::endl;
	// Uploads are batched into the streamer ring (256MB threshold);
	// GltfToCluster issues the final FlushUploads after scene import.
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

smvk::ClusterMaterialHandle GltfToSceneImporter::getOrCreateMaterial(int gltfMaterialIndex)
{
	if (gltfMaterialIndex < 0) {
		if (!defaultMaterialHandle.empty()) return defaultMaterialHandle;
		smvk::MaterialCreateInfo def{};
		def.baseColorFactor = glm::vec4(1.0f);
		def.metallicFactor = 0.0f;
		def.roughnessFactor = 0.9f;
		def.emissiveFactor = glm::vec3(0.0f);
		def.emissiveStrength = 1.0f;
		def.occlusionStrength = 1.0f;
		def.normalScale = 1.0f;
		def.alphaMode = smvk::AlphaMode::Opaque;
		def.shadingModel = smvk::ShadingModel::DefaultLit;
		def.alphaCutoff = 0.5f;
		def.doubleSided = false;
		defaultMaterialHandle = cluster.CreateMaterial(def);
		return defaultMaterialHandle;
	}

	assert(
		static_cast<size_t>(gltfMaterialIndex) <
		materialCache.size()
	);

	auto& slot =
		materialCache[gltfMaterialIndex];

	if (!slot.empty()) {
		return slot;
	}

	const GltfMaterial mat =
		ExtractMaterial(
			model,
			gltfMaterialIndex
		);

	const std::filesystem::path baseDir =
		gltfModelPath.parent_path();

	auto load = [&](const std::optional<GltfTextureRef>& ref) -> smvk::TextureHandle {
		if (!ref) return {};
		return LoadKtxTextureCached(cluster, baseDir, model, *ref, textureCache);
	};

	smvk::TextureHandle baseColorHandle = load(mat.baseColorTex);
	smvk::TextureHandle metallicRoughHandle = load(mat.metallicRoughnessTex);
	smvk::TextureHandle normalHandle = load(mat.normalTex);
	smvk::TextureHandle occlusionHandle = load(mat.occlusionTex);
	smvk::TextureHandle emissiveHandle = load(mat.emissiveTex);

	smvk::MaterialCreateInfo ci{};
	ci.baseColorTexture = baseColorHandle;
	ci.metallicRoughnessTexture = metallicRoughHandle;
	ci.normalTexture = normalHandle;
	ci.occlusionTexture = occlusionHandle;
	ci.emissiveTexture = emissiveHandle;
	// legacy alias
	ci.diffuse = baseColorHandle;
	ci.baseColorFactor = glm::vec4(
		static_cast<float>(mat.baseColorFactor[0]),
		static_cast<float>(mat.baseColorFactor[1]),
		static_cast<float>(mat.baseColorFactor[2]),
		static_cast<float>(mat.baseColorFactor[3])
	);
	ci.metallicFactor = static_cast<float>(mat.metallicFactor);
	ci.roughnessFactor = static_cast<float>(mat.roughnessFactor);
	ci.emissiveFactor = glm::vec3(
		static_cast<float>(mat.emissiveFactor[0]),
		static_cast<float>(mat.emissiveFactor[1]),
		static_cast<float>(mat.emissiveFactor[2])
	);
	ci.emissiveStrength = static_cast<float>(mat.emissiveStrength);
	ci.occlusionStrength = mat.occlusionTex ? mat.occlusionTex->scaleOrStrength : 1.0f;
	ci.normalScale = mat.normalTex ? mat.normalTex->scaleOrStrength : 1.0f;
	// KHR_materials_unlit overrides shading model
	if (mat.unlit) {
		ci.shadingModel = smvk::ShadingModel::Unlit;
		// Alpha still handled via alphaMode
		switch (mat.alphaMode) {
			case GltfAlphaMode::Opaque: ci.alphaMode = smvk::AlphaMode::Opaque; break;
			case GltfAlphaMode::Mask:   ci.alphaMode = smvk::AlphaMode::Mask; break;
			case GltfAlphaMode::Blend:  ci.alphaMode = smvk::AlphaMode::Blend; break;
		}
	} else {
		switch (mat.alphaMode) {
			case GltfAlphaMode::Opaque: ci.alphaMode = smvk::AlphaMode::Opaque; ci.shadingModel = smvk::ShadingModel::DefaultLit; break;
			case GltfAlphaMode::Mask:   ci.alphaMode = smvk::AlphaMode::Mask;   ci.shadingModel = smvk::ShadingModel::Masked;   break;
			case GltfAlphaMode::Blend:  ci.alphaMode = smvk::AlphaMode::Blend;  ci.shadingModel = smvk::ShadingModel::DefaultLit; break;
		}
	}
	ci.alphaCutoff = static_cast<float>(mat.alphaCutoff);
	ci.doubleSided = mat.doubleSided;

	slot = cluster.CreateMaterial(ci);

	return slot;
}
smvk::StaticMeshUpload SurfaceDataFromGltfPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive) {
	smvk::StaticMeshUpload data;
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

			data.aabbMin = mn;
			data.aabbMax = mx;


		}
		else {

			// I wont handle this. just assert and fail. 
			assert(false && "failed to get bb ");

		}

	}


	// Extract vertices. Presence flags decide which frames must be synthesized
	// below: a missing NORMAL/TANGENT must never keep a placeholder, or
	// normal-mapped lighting breaks on that primitive.
	const bool hasUVs = primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end();
	const bool hasAuthoredTangents = primitive.attributes.find("TANGENT") != primitive.attributes.end();
	const bool hasAuthoredNormals = primitive.attributes.find("NORMAL") != primitive.attributes.end();

	auto& posAcc = model.accessors[primitive.attributes.at("POSITION")];
	data.vertices.reserve(posAcc.count);
	for (size_t i = 0; i < posAcc.count; ++i) {
		smvk::StaticVertex v{};
		v.pos = readVec3ForientationFix(model, posAcc, i);
		v.color = { 1.0f, 1.0f, 1.0f };
		v.texCoord = { 0.0f, 0.0f }; // will fill if UVs exist
		v.normal = { 0.0f, 1.0f, 0.0f }; // replaced below if normals provided/generated
		v.tangent = { 1.0f, 0.0f, 0.0f, 1.0f }; // replaced below if tangents provided/generated
		data.vertices.push_back(v);
	}


	if (hasUVs) {
		auto& uvAcc = model.accessors[primitive.attributes.at("TEXCOORD_0")];
		for (size_t i = 0; i < uvAcc.count && i < data.vertices.size(); ++i) {
			glm::vec2 uv = readVec2F(model, uvAcc, i);
			data.vertices[i].texCoord = uv; // flip V if your shader expects it: uv.y = 1.0f - uv.y;
		}
	}


	if (hasAuthoredTangents) {
		auto& tangentAcc = model.accessors[primitive.attributes.at("TANGENT")];
		for (size_t i = 0; i < tangentAcc.count && i < data.vertices.size(); ++i) {
			glm::vec4 tangent = readVec4F(model, tangentAcc, i);

			// Space conversion to my engine. Same as pos.
			// (-x, +y, -z) is a 180-degree Y rotation (det +1), so the
			// tangent-space handedness w is preserved as-is.
			tangent.x *= -1.0f;
			tangent.z *= -1.0f;

			data.vertices[i].tangent = tangent;
		}
	}


	if (hasAuthoredNormals) {
		auto& nAcc = model.accessors[primitive.attributes.at("NORMAL")];
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

	if (!hasAuthoredNormals) {
		computeSmoothNormals(data);
	}
	if (!hasAuthoredTangents && hasUVs) {
		generateTangents(data);
	}


	return data;

}
smvk::ClusterStaticMeshHandle GltfToSceneImporter::getOrCreateMesh(int gltfMeshIndex, int primitiveIndex)
{
	assert(gltfMeshIndex >= 0);
	assert(
		static_cast<size_t>(gltfMeshIndex) <
		model.meshes.size()
	);

	const tinygltf::Mesh& gltfMesh =
		model.meshes[gltfMeshIndex];

	assert(primitiveIndex >= 0);
	assert(
		static_cast<size_t>(primitiveIndex) <
		gltfMesh.primitives.size()
	);

	auto& slot =
		meshCache[gltfMeshIndex][primitiveIndex];

	if (!slot.empty()) {
		return slot;
	}

	const tinygltf::Primitive& primitive =
		gltfMesh.primitives[primitiveIndex];

	auto data =
		SurfaceDataFromGltfPrimitive(
			model,
			primitive
		);

	slot =
		cluster.UploadStaticMesh(data);

	return slot;
}
void GltfToSceneImporter::importNodeRecursive(int gltfNodeIndex, int parentSceneNode, int level) {
	const tinygltf::Node& gltfNode = model.nodes[gltfNodeIndex];

	// Always create a "group" node that owns name + transform and preserves hierarchy.
	const int groupNode = addNode(cluster.scene, parentSceneNode, level);

	{
		const int nameIndex = (int)cluster.scene.nodeNames.size();
		cluster.scene.nodeNames.push_back(gltfNode.name.empty() ? "Node" : gltfNode.name);
		cluster.scene.nameForNode[groupNode] = nameIndex;
	}

	cluster.scene.localTransform[groupNode] = GetNodeLocalTransform(gltfNode);
	cluster.scene.globalTransform[groupNode] = glm::mat4(1.0f); // resolved later

	// Unroll primitives into separate meshes (your current approach).
	if (gltfNode.mesh >= 0) {
		const tinygltf::Mesh& gltfMesh = model.meshes[gltfNode.mesh];

		for (int primIndex = 0; primIndex < (int)gltfMesh.primitives.size(); ++primIndex) {
			const tinygltf::Primitive& primitive = gltfMesh.primitives[primIndex];

			const int primNode = addNode(cluster.scene, groupNode, level + 1);
			cluster.scene.localTransform[primNode] = glm::mat4(1.0f);
			cluster.scene.globalTransform[primNode] = glm::mat4(1.0f);
			cluster.scene.meshForNode[primNode] =
				getOrCreateMesh(
					gltfNode.mesh,
					primIndex
				).index();
			cluster.scene.materialForNode[primNode] =
				getOrCreateMaterial(
					primitive.material
				).index();
			markAsChanged(cluster.scene, primNode);
		}
	}

	// Recurse children under the group node to preserve hierarchy.
	for (int child : gltfNode.children) {
		importNodeRecursive(child, groupNode, level + 1);
	}
}

