#define TINYGLTF_IMPLEMENTATION

#include "ktx.h"

#include <Engine/Utils/gltf.h>
#include <Engine/Utils/texture_loader.h>
#include <Engine/Renderer/texture_streamer.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/Asset/asset_registry.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/core.h>


#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <thread>
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
	static glm::vec3 readVec3ForientationFix(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i, int coordFix) {
		// Expect FLOAT VEC3
		const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
		if (coordFix == 1 || coordFix == 3) {
			// 1: skip engine fix (model already in engine space).
			// 3: same + RootNode/Level hierarchy is neutralized by the
			//    scene preTransform (see bistro SceneDesc).
			return glm::vec3(p[0], p[1], p[2]);
		}
		if (coordFix == 2) {
			return glm::vec3(p[0], -p[1], p[2]);
		}
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
	// COLOR_0 as RGBA floats: handles FLOAT vec3/vec4 and normalized
	// UNSIGNED_BYTE / UNSIGNED_SHORT vec3/vec4 (the common authored forms).
	// Anything else falls back to white.
	glm::vec4 readColorRGBA(const tinygltf::Model& m, const tinygltf::Accessor& a,
		size_t i, int compCount) {
		glm::vec4 c{ 1.0f, 1.0f, 1.0f, 1.0f };
		if (compCount < 3)
			return c;
		const uint8_t* p = gltfPtr(m, a, i);
		const int n = compCount >= 4 ? 4 : 3;
		if (a.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
			const float* f = reinterpret_cast<const float*>(p);
			for (int k = 0; k < n; ++k)
				c[k] = f[k];
		}
		else if (a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
			for (int k = 0; k < n; ++k)
				c[k] = static_cast<float>(p[k]) * (1.0f / 255.0f);
		}
		else if (a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
			const uint16_t* s = reinterpret_cast<const uint16_t*>(p);
			for (int k = 0; k < n; ++k)
				c[k] = static_cast<float>(s[k]) * (1.0f / 65535.0f);
		}
		return c;
	}
	// -------------------------------------------------------------------------
	// Tangent-frame synthesis lives in Engine/Asset/mesh_optimizer.h
	// (ComputeSmoothNormals / GenerateTangents) so the LOD cooker can share it.
	// -------------------------------------------------------------------------
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
	// Reads a whole host file into bytes (HostTextureSource backend).
	inline std::optional<std::vector<uint8_t>> ReadHostBytes(const std::filesystem::path& p) {
		std::ifstream f(p, std::ios::binary | std::ios::ate);
		if (!f)
			return std::nullopt;
		const auto size = f.tellg();
		if (size < 0)
			return std::nullopt;
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		f.seekg(0);
		if (!bytes.empty())
			f.read(reinterpret_cast<char*>(bytes.data()), size);
		if (!f && !bytes.empty())
			return std::nullopt;
		return bytes;
	}
}; // namespace

GltfToSceneImporter::GltfToSceneImporter(smvk::Cluster& inCluster, const tinygltf::Model& inModel, std::filesystem::path inGltfModelPath)
	: cluster(inCluster)
	, model(inModel)
	, gltfModelPath(std::move(inGltfModelPath))
	, textureSource(std::make_shared<HostTextureSource>(gltfModelPath.parent_path()))
	, modelUriBase(smvk::vfs::Normalize(gltfModelPath.generic_string()))
	, materialCache(inModel.materials.size())
{
	meshCache.resize(inModel.meshes.size());
	for (size_t i = 0; i < inModel.meshes.size(); ++i) {
		meshCache[i].resize(
			inModel.meshes[i].primitives.size()
		);
	}
}

void GltfToSceneImporter::setTextureSource(std::shared_ptr<TextureSource> source) {
	if (source)
		textureSource = std::move(source);
}

void GltfToSceneImporter::setModelUriBase(const std::string& vfsPath) {
	const smvk::StringViewT n = smvk::NormalizeAssetUri(vfsPath);
	if (!n.empty())
		modelUriBase = smvk::AssetFilePart(n);
}

void GltfToSceneImporter::setMeshOptConfig(const smvk::MeshOptConfig& config) {
	meshOptConfig = config;
}

void GltfToSceneImporter::setMeshletConfig(const smvk::MeshletConfig& config) {
	meshletConfig = config;
}

namespace {
// A glTF node opts into the frozen static set via authoring extras or by
// naming convention (checked in importNodeRecursive):
//   extras: { "smvk.static": true }  (or { "smvk": { "static": true } })
//   -or- name starts with "STATIC_".
bool GltfExtrasStatic(const tinygltf::Value& extras) {
	if (!extras.IsObject())
		return false;
	auto truthy = [](const tinygltf::Value& v) {
		if (v.IsBool())
			return v.Get<bool>();
		if (v.IsNumber())
			return v.GetNumberAsDouble() != 0.0;
		return false;
	};
	if (extras.Has("smvk.static") && truthy(extras.Get("smvk.static")))
		return true;
	if (extras.Has("static") && truthy(extras.Get("static")))
		return true;
	if (extras.Has("smvk")) {
		const tinygltf::Value& sub = extras.Get("smvk");
		if (sub.IsObject() && sub.Has("static") && truthy(sub.Get("static")))
			return true;
	}
	return false;
}
} // namespace

// KTX2 file identifier (first 12 bytes) for cook-time payload validation.
inline bool IsKtx2Payload(const std::vector<uint8_t>& bytes) {
	static constexpr uint8_t kMagic[12] = {
		0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
	return bytes.size() >= 12 && memcmp(bytes.data(), kMagic, 12) == 0;
}

// Single memory-decode texture path shared by preloadTextures and the
// per-material fallback: bytes come from the configured TextureSource
// (host files or VFS). Runtime clusters decode/upload via
// LoadKtxTextureMemory; cook-mode clusters skip the GPU entirely and mint
// fake handles (Handle::Mint ids) that only feed MaterialValues slots and
// the serializer's slot->uri remap. Either way the asset URI, sampler
// params and source bytes are registered/stashed on the Cluster.
smvk::TextureHandle GltfToSceneImporter::loadTextureForImage(int imageIndex, int textureIndex) {
	auto cached = textureCache.find(imageIndex);
	if (cached != textureCache.end())
		return cached->second;
	if (imageIndex < 0 || imageIndex >= (int)model.images.size())
		return {};
	const tinygltf::Image& img = model.images[imageIndex];
	if (img.uri.empty() || starts_with(img.uri, "data:")) {
		assert(false && "Internal textures not supported for gltf loading.");
		return {};
	}
	if (!textureSource)
		return {};
	auto bytes = textureSource->ReadBytes(img.uri);
	if (!bytes || bytes->empty()) {
		std::cerr << "Texture source missing image: " << img.uri << std::endl;
		return {};
	}
	const std::string identity = textureSource->ResolveIdentity(img.uri);

	smvk::TextureHandle h;
	if (cluster.IsCookMode()) {
		// GPU-free cook: validate the payload so broken references fail
		// loudly at cook time, then mint a stable cook id.
		if (!IsKtx2Payload(*bytes)) {
			std::cerr << "Cook: not a KTX2 payload: " << img.uri << std::endl;
			return {};
		}
		h = smvk::TextureHandle::Mint(cluster.AllocCookTextureId());
	}
	else {
		if (!cluster.ctx_)
			return {};
		if (textureStreamer_ != nullptr && !identity.empty())
		{
			// Streaming path: stash the payload in the cluster FIRST – the
			// streamer keeps a non-owning view of it for future upgrades –
			// then upload only the initial tail.
			cluster.SetTextureSource(identity, std::move(*bytes));
			const std::vector<uint8_t>* stored = cluster.GetTextureSource(identity);
			if (!stored || stored->empty())
				return {};
			auto holder = textureStreamer_->StreamTexture(
				stored->data(), stored->size(), identity);
			if (!holder.valid())
				return {};
			h = holder;
			cluster.textures.create(std::move(holder));
		}
		else
		{
			auto holder = smvk::LoadKtxTextureMemory(*cluster.ctx_, bytes->data(), bytes->size(), identity);
			if (!holder.valid())
				return {};
			h = holder;
			cluster.textures.create(std::move(holder));
		}
	}
	textureCache[imageIndex] = h;
	if (!identity.empty()) {
		cluster.RegisterTextureUri(identity, h);
		// Persist authoring sampler (renderer still uses its default;
		// the desc travels into paks for a future sampler pass).
		smvk::TextureSamplerDesc samp;
		if (textureIndex >= 0 && textureIndex < (int)model.textures.size()) {
			const int s = model.textures[textureIndex].sampler;
			if (s >= 0 && s < (int)model.samplers.size()) {
				const auto& ts = model.samplers[s];
				samp.magFilter = ts.magFilter;
				samp.minFilter = ts.minFilter;
				samp.wrapS = ts.wrapS;
				samp.wrapT = ts.wrapT;
			}
		}
		cluster.RegisterTextureSampler(identity, samp);
		// The streaming path already stashed the payload above (moved-from
		// `bytes` is empty there); the full-res path stashes it here.
		if (bytes.has_value() && !bytes->empty())
			cluster.SetTextureSource(identity, std::move(*bytes));
	}
	return h;
}
bool LoadModelFromAsciiFile(tinygltf::Model& model, const char* filename) {
	tinygltf::TinyGLTF loader;
	std::string err;
	std::string warn;

	bool res = loader.LoadASCIIFromFile(&model, &err, &warn, filename);
#ifndef NDEBUG
	if (!warn.empty()) {
		std::cout << "WARN: " << warn << std::endl;
	}
#endif

	if (!err.empty()) {
		std::cout << "ERR: " << err << std::endl;
	}

	if (!res)
		std::cout << "Failed to load glTF: " << filename << std::endl;
#ifndef NDEBUG
	else
		std::cout << "Loaded glTF: " << filename << std::endl;
#endif

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
// Resolves a raw texture index (used by material extensions that carry
// plain {index, texCoord} objects instead of TextureInfo, e.g.
// KHR_materials_pbrSpecularGlossiness diffuse/specular textures).
std::optional<GltfTextureRef> GltfResolveTextureIndex(const tinygltf::Model& model, int texIndex, int texCoord = 0)
{
	if (texIndex < 0 || texIndex >= (int)model.textures.size()) return std::nullopt;
	const tinygltf::Texture& tex = model.textures[texIndex];
	GltfTextureRef out{};
	out.textureIndex = texIndex;
	out.texCoord = texCoord;
	if (tex.source >= 0 && tex.source < (int)model.images.size())
		out.imageIndex = tex.source;
	if (tex.sampler >= 0 && tex.sampler < (int)model.samplers.size())
		out.samplerIndex = tex.sampler;
	if (out.imageIndex < 0) return std::nullopt;
	return out;
}
// Reads an extension sub-object integer (tinygltf::Value numbers are double).
int GltfExtensionTextureIndex(const tinygltf::Value::Object& obj, const char* key)
{
	auto f = obj.find(key);
	if (f == obj.end() || !f->second.IsObject()) return -1;
	const auto& t = f->second.Get<tinygltf::Value::Object>();
	auto fi = t.find("index");
	if (fi == t.end() || !fi->second.IsNumber()) return -1;
	int texCoord = 0;
	if (auto tc = t.find("texCoord"); tc != t.end() && tc->second.IsNumber())
		texCoord = static_cast<int>(tc->second.GetNumberAsDouble());
	(void)texCoord;
	return static_cast<int>(fi->second.GetNumberAsDouble());
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

	// KHR_materials_pbrSpecularGlossiness fallback (FBX2glTF assets such as
	// Bistro: 234/254 materials carry ONLY this extension, no core
	// pbrMetallicRoughness textures). diffuseTexture/diffuseFactor map onto
	// the metal-rough base color; glossinessFactor tunes roughness
	// (metallic forced to dielectric 0). The specular-glossiness packing
	// (specular RGB + glossiness A) has no metal-rough equivalent, so that
	// texture is intentionally skipped.
	{
		auto it = m.extensions.find("KHR_materials_pbrSpecularGlossiness");
		if (it != m.extensions.end() && it->second.IsObject()) {
			const auto& obj = it->second.Get<tinygltf::Value::Object>();
			const int diffuseIdx = GltfExtensionTextureIndex(obj, "diffuseTexture");
			if (!mat.baseColorTex) {
				if (auto t = GltfResolveTextureIndex(model, diffuseIdx))
					mat.baseColorTex = *t;
			}
			if (auto f = obj.find("diffuseFactor"); f != obj.end() && f->second.IsArray()) {
				const auto& arr = f->second.Get<tinygltf::Value::Array>();
				// tinygltf defaults baseColorFactor to [1,1,1,1] (non-empty),
				// so emptiness can never detect "no core color" — compare
				// against the default instead.
				const auto& bcf = m.pbrMetallicRoughness.baseColorFactor;
				const bool coreFactorDefault = bcf.size() < 4 ||
					(bcf[0] == 1.0 && bcf[1] == 1.0 && bcf[2] == 1.0 && bcf[3] == 1.0);
				const bool hasPbrColor =
					m.pbrMetallicRoughness.baseColorTexture.index >= 0 ||
					!coreFactorDefault;
				if (!hasPbrColor && arr.size() >= 3) {
					for (int k = 0; k < 3 && k < (int)arr.size(); ++k)
						if (arr[k].IsNumber())
							mat.baseColorFactor[k] = arr[k].GetNumberAsDouble();
					if (arr.size() >= 4 && arr[3].IsNumber())
						mat.baseColorFactor[3] = arr[3].GetNumberAsDouble();
				}
			}
			// Dielectric default for spec-gloss-only materials; glossiness
			// inverts to roughness when the core block is at its default.
			// LUMBERYARD BISTRO NOTE: the spec-gloss specularGlossinessTexture
			// is a near-constant specular COLOR (gray RGB, A=255) — FBX2glTF
			// wired the FBX SpecularColor slot; there is no ORM packing and
			// no per-pixel gloss (A is constant). Per-material gloss lives in
			// glossinessFactor only, so the texture is never sampled.
			const bool coreUntouched =
				m.pbrMetallicRoughness.metallicFactor == 1.0 &&
				m.pbrMetallicRoughness.roughnessFactor == 1.0;
			if (coreUntouched) {
				mat.metallicFactor = 0.0;
				if (auto f = obj.find("glossinessFactor"); f != obj.end() && f->second.IsNumber()) {
					const double gloss = f->second.GetNumberAsDouble();
					mat.roughnessFactor = std::clamp(1.0 - gloss, 0.05, 1.0);
				}
				else {
					mat.roughnessFactor = 0.6;
				}
			}
		}
	}

	// KHR_materials_transmission -> BLEND transparent. The engine has
	// no transmission path, so transmissive glass (e.g. Bistro windows)
	// is approximated with alpha blend. Alpha derives from
	// (1 - transmissionFactor) unless the material already carries an
	// alpha < 1. Double-sided so panes are visible from both sides
	// (pipeline cull None via doubleSided).
	// Downstream this yields AlphaMode::Blend -> IteratorFlagBit_Transparent
	// (back-to-front sorted transparent pass, no shadow/depth-prepass
	// caster, bypasses the opaque/masked gfx-filter draw sets) with
	// AlphaBlend + depth-test/no-write from MakePipelineStateForMaterial.
	double transmissionFactor = 0.0;
	if (auto it = m.extensions.find("KHR_materials_transmission");
		it != m.extensions.end() && it->second.IsObject()) {
		const auto& obj = it->second.Get<tinygltf::Value::Object>();
		if (auto f = obj.find("transmissionFactor");
			f != obj.end() && f->second.IsNumber())
			transmissionFactor = f->second.GetNumberAsDouble();
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

	// Transmissive override last so it wins over the raw copy above.
	// Metallic is forced dielectric: transmissive panes are glass, and
	// an absent core factor defaults to 1.0 (fully metallic) per spec.
	if (transmissionFactor > 0.001) {
		mat.alphaMode = GltfAlphaMode::Blend;
		mat.metallicFactor = 0.0;
		if (mat.baseColorFactor[3] >= 0.999)
			mat.baseColorFactor[3] =
				std::clamp(1.0 - transmissionFactor, 0.05, 1.0);
		mat.doubleSided = true;
	}

	return mat;
}
void GltfToSceneImporter::preloadTextures()
{
	// Collect every image referenced by any material (dedup by imageIndex).
	// Per-material creation later hits textureCache; anything missed here
	// still loads sequentially via loadTextureForImage.
	// Bytes come from the configured TextureSource (host or VFS) and are
	// decoded/uploaded in parallel from memory; registration (URI, sampler,
	// source stash) mirrors loadTextureForImage so both paths agree.
	std::vector<int> imageIndices;
	imageIndices.reserve(model.images.size());
	std::unordered_map<int, int> firstTextureForImage; // imageIndex -> textureIndex (sampler)
	{
		std::vector<bool> seen(model.images.size(), false);
		auto note = [&](const std::optional<GltfTextureRef>& ref) {
			if (!ref || ref->imageIndex < 0 ||
				ref->imageIndex >= (int)model.images.size())
				return;
			if (!seen[ref->imageIndex]) {
				seen[ref->imageIndex] = true;
				imageIndices.push_back(ref->imageIndex);
				firstTextureForImage[ref->imageIndex] = ref->textureIndex;
			}
		};
		for (size_t mi = 0; mi < model.materials.size(); ++mi) {
			const tinygltf::Material& m = model.materials[mi];
			note(GltfResolveTexture(model, m.pbrMetallicRoughness.baseColorTexture));
			note(GltfResolveTexture(model, m.pbrMetallicRoughness.metallicRoughnessTexture));
			note(GtlfResolveNormalTexture(model, m.normalTexture));
			note(GLtfResolveOcclusionTexture(model, m.occlusionTexture));
			note(GltfResolveTexture(model, m.emissiveTexture));
			// Spec-gloss diffuse fallback (see ExtractMaterial): preload so
			// the per-material path hits the cache. specularGlossinessTexture
			// is never sampled (its pixels are a specular color, not ORM),
			// so only the diffuse needs preloading here.
			if (auto it = m.extensions.find("KHR_materials_pbrSpecularGlossiness");
				it != m.extensions.end() && it->second.IsObject()) {
				const auto& obj = it->second.Get<tinygltf::Value::Object>();
				note(GltfResolveTextureIndex(model, GltfExtensionTextureIndex(obj, "diffuseTexture")));
			}
		}
	}

	if (!textureSource)
		return;

	struct Pending {
		int imageIndex = -1;
		int textureIndex = -1;
		std::string identity;
		std::vector<uint8_t> bytes;
	};
	std::vector<Pending> pendings;
	pendings.reserve(imageIndices.size());
	for (int imageIndex : imageIndices) {
		if (textureCache.find(imageIndex) != textureCache.end())
			continue;
		const tinygltf::Image& img = model.images[imageIndex];
		// Embedded / data-URI images are not supported (same policy as
		// loadTextureForImage); skip them here, the sequential path asserts.
		if (img.uri.empty() || starts_with(img.uri, "data:"))
			continue;
		auto bytes = textureSource->ReadBytes(img.uri);
		if (!bytes || bytes->empty()) {
			std::cerr << "Texture source missing image: " << img.uri << std::endl;
			continue;
		}
		Pending p;
		p.imageIndex = imageIndex;
		auto tf = firstTextureForImage.find(imageIndex);
		p.textureIndex = tf != firstTextureForImage.end() ? tf->second : -1;
		p.identity = textureSource->ResolveIdentity(img.uri);
		p.bytes = std::move(*bytes);
		pendings.push_back(std::move(p));
	}

	if (pendings.empty()) return;

	// Cook mode: no GPU at all — mint stable ids, register uri/sampler/
	// source, done. Everything here is CPU-only so no threads are needed.
	if (cluster.IsCookMode()) {
		for (Pending& p : pendings) {
			if (!IsKtx2Payload(p.bytes)) {
				std::cerr << "Cook: not a KTX2 payload: " << p.identity << std::endl;
				continue;
			}
			const smvk::TextureHandle h =
				smvk::TextureHandle::Mint(cluster.AllocCookTextureId());
			textureCache[p.imageIndex] = h;
			if (!p.identity.empty()) {
				cluster.RegisterTextureUri(p.identity, h);
				smvk::TextureSamplerDesc samp;
				if (p.textureIndex >= 0 && p.textureIndex < (int)model.textures.size()) {
					const int s = model.textures[p.textureIndex].sampler;
					if (s >= 0 && s < (int)model.samplers.size()) {
						const auto& ts = model.samplers[s];
						samp.magFilter = ts.magFilter;
						samp.minFilter = ts.minFilter;
						samp.wrapS = ts.wrapS;
						samp.wrapT = ts.wrapT;
					}
				}
				cluster.RegisterTextureSampler(p.identity, samp);
				cluster.SetTextureSource(p.identity, std::move(p.bytes));
			}
		}
#ifndef NDEBUG
		std::cout << "Cook-staged " << pendings.size() << " texture payloads" << std::endl;
#endif
		return;
	}

	// Streaming path: tail-resident uploads through the TextureStreamer.
	// Payloads are stashed in the cluster first (the streamer views
	// cluster-owned bytes for future upgrades); decode + tail upload runs
	// on workers, registration back on this thread. The pool joins before
	// return, so no frames can interleave (same contract as full-res).
	if (textureStreamer_ != nullptr && cluster.ctx_ != nullptr) {
		const auto t0 = std::chrono::steady_clock::now();
		for (Pending& p : pendings) {
			if (!p.identity.empty())
				cluster.SetTextureSource(p.identity, std::move(p.bytes));
		}
		size_t threadCount = std::thread::hardware_concurrency();
		if (threadCount == 0) threadCount = 4;
		if (threadCount > pendings.size()) threadCount = pendings.size();
		size_t succeeded = 0, failed = 0;
		{
			smvk::ThreadPool pool(threadCount);
			std::vector<std::future<std::pair<int, smvk::Holder<smvk::TextureHandle>>>> futures;
			futures.reserve(pendings.size());
			for (size_t i = 0; i < pendings.size(); ++i) {
				// Borrow the cluster-owned payload (stable after the stash
				// loop above; registration below only runs after join).
				const std::vector<uint8_t>* stored =
					cluster.GetTextureSource(pendings[i].identity);
				const std::string identity = pendings[i].identity;
				futures.push_back(pool.enqueue([this, stored, identity] {
					smvk::Holder<smvk::TextureHandle> holder;
					if (stored && !stored->empty())
						holder = textureStreamer_->StreamTexture(
							stored->data(), stored->size(), identity);
					return std::make_pair(0, std::move(holder));
				}));
			}
			for (size_t i = 0; i < futures.size(); ++i) {
				try {
					auto [unused, holder] = futures[i].get();
					(void)unused;
					if (!holder.valid()) { ++failed; continue; }
					++succeeded;
					Pending& p = pendings[i];
					smvk::TextureHandle h = holder;
					cluster.textures.create(std::move(holder));
					textureCache[p.imageIndex] = h;
					if (!p.identity.empty()) {
						cluster.RegisterTextureUri(p.identity, h);
						smvk::TextureSamplerDesc samp;
						if (p.textureIndex >= 0 && p.textureIndex < (int)model.textures.size()) {
							const int s = model.textures[p.textureIndex].sampler;
							if (s >= 0 && s < (int)model.samplers.size()) {
								const auto& ts = model.samplers[s];
								samp.magFilter = ts.magFilter;
								samp.minFilter = ts.minFilter;
								samp.wrapS = ts.wrapS;
								samp.wrapT = ts.wrapT;
							}
						}
						cluster.RegisterTextureSampler(p.identity, samp);
						// Payload already stashed above; nothing to move here.
					}
				}
				catch (const std::exception& e) {
					std::cerr << "Texture streaming worker failed: " << e.what() << std::endl;
					++failed;
				}
			}
		}
		const auto t1 = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration<double>(t1 - t0).count();
#ifndef NDEBUG
		std::cout << "Stream-preloaded " << succeeded << "/" << pendings.size()
			<< " textures in " << seconds << "s"
			<< (failed ? " (" + std::to_string(failed) + " failed)" : "")
			<< " (tail-resident)" << std::endl;
#else
		(void)seconds;
		(void)failed;
#endif
		return;
	}

	std::vector<smvk::KtxMemoryJob> jobs;
	jobs.reserve(pendings.size());
	for (size_t i = 0; i < pendings.size(); ++i) {
		jobs.push_back(smvk::KtxMemoryJob{
			static_cast<int>(i),
			pendings[i].bytes.data(),
			pendings[i].bytes.size(),
			pendings[i].identity,
		});
	}

	smvk::ParallelTextureStats stats{};
	auto holders = cluster.ctx_
		? smvk::LoadKtxTexturesParallelMemory(*cluster.ctx_, jobs, {}, &stats)
		: std::unordered_map<int, smvk::Holder<smvk::TextureHandle>>{};

	for (size_t i = 0; i < pendings.size(); ++i) {
		auto hit = holders.find(static_cast<int>(i));
		if (hit == holders.end())
			continue;
		Pending& p = pendings[i];
		smvk::TextureHandle h = hit->second;
		cluster.textures.create(std::move(hit->second));
		textureCache[p.imageIndex] = h;
		if (!p.identity.empty()) {
			cluster.RegisterTextureUri(p.identity, h);
			smvk::TextureSamplerDesc samp;
			if (p.textureIndex >= 0 && p.textureIndex < (int)model.textures.size()) {
				const int s = model.textures[p.textureIndex].sampler;
				if (s >= 0 && s < (int)model.samplers.size()) {
					const auto& ts = model.samplers[s];
					samp.magFilter = ts.magFilter;
					samp.minFilter = ts.minFilter;
					samp.wrapS = ts.wrapS;
					samp.wrapT = ts.wrapT;
				}
			}
			cluster.RegisterTextureSampler(p.identity, samp);
			cluster.SetTextureSource(p.identity, std::move(p.bytes));
		}
	}

#ifndef NDEBUG
	std::cout << "Preloaded " << stats.succeeded << "/" << stats.requested
		<< " textures in " << stats.seconds << "s ("
		<< (stats.failed ? std::to_string(stats.failed) + " failed, " : "")
		<< jobs.size() << " jobs)" << std::endl;
#endif
	// Uploads are batched into the streamer ring (256MB threshold);
	// GltfToCluster issues the final FlushUploads after scene import.
}

// Basis conversion C for node local transforms. Vertices are baked as
// v' = C * v, so every local matrix must be conjugated to stay consistent:
// M' = C * M * C^-1 (C is its own inverse). For TRS this is t' = C*t,
// R' = C*R*C, S unchanged (diagonal commutes). coordFix modes mirror
// readVec3ForientationFix / SurfaceDataFromGltfPrimitive:
//   0 = engine fix (180 deg Y: -X,+Y,-Z, det +1, winding preserved)
//   1/3 = skip (already in engine space)
//   2 = Y mirror (det -1; conjugates translations the same way, but note
//       mirrored winding is NOT reflipped here — same limitation as the
//       vertex path, which also leaves indices untouched).
inline glm::mat4 ConvertNodeLocalTransform(const glm::mat4& m, int coordFix) {
	if (coordFix == 1 || coordFix == 3)
		return m;
	glm::mat4 c(1.0f);
	if (coordFix == 2) {
		c[1][1] = -1.0f;
	} else {
		c[0][0] = -1.0f;
		c[2][2] = -1.0f;
	}
	return c * m * c;
}

glm::mat4 GetNodeLocalTransform(const tinygltf::Node& n, int coordFix)
{
	glm::mat4 m(1.0f);
	if (!n.matrix.empty() && n.matrix.size() >= 16) {
		// tinygltf stores matrices as doubles in column-major order;
		// convert element-wise (memcpy would reinterpret double bits).
		float* dst = glm::value_ptr(m);
		for (int i = 0; i < 16; ++i)
			dst[i] = static_cast<float>(n.matrix[i]);
	} else {
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

		m = glm::translate(glm::mat4(1.0f), t)
			* glm::mat4_cast(r)
			* glm::scale(glm::mat4(1.0f), s);
	}
	return ConvertNodeLocalTransform(m, coordFix);
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
		if (!modelUriBase.empty())
			cluster.RegisterMaterialUri(
				smvk::MakeSubUri(modelUriBase, "mat.default"), defaultMaterialHandle);
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

	auto load = [&](const std::optional<GltfTextureRef>& ref) -> smvk::TextureHandle {
		if (!ref) return {};
		return loadTextureForImage(ref->imageIndex, ref->textureIndex);
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
	if (!modelUriBase.empty())
		cluster.RegisterMaterialUri(
			smvk::MakeSubUri(modelUriBase,
				"mat." + std::to_string(gltfMaterialIndex)),
			slot);

	return slot;
}
static smvk::StaticMeshUpload SurfaceDataFromGltfPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive, int coordFix) {
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
			glm::vec3 c0, c1;
			if (coordFix == 1 || coordFix == 3) {
				c0 = aabbMin;
				c1 = aabbMax;
			} else if (coordFix == 2) {
				c0 = glm::vec3(aabbMin.x, -aabbMin.y, aabbMin.z);
				c1 = glm::vec3(aabbMax.x, -aabbMax.y, aabbMax.z);
			} else {
				c0 = glm::vec3(-aabbMin.x, aabbMin.y, -aabbMin.z);
				c1 = glm::vec3(-aabbMax.x, aabbMax.y, -aabbMax.z);
			}

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
	const bool hasColors = primitive.attributes.find("COLOR_0") != primitive.attributes.end();

	auto& posAcc = model.accessors[primitive.attributes.at("POSITION")];
	data.vertices.reserve(posAcc.count);
	for (size_t i = 0; i < posAcc.count; ++i) {
		smvk::StaticVertex v{};
		v.pos = readVec3ForientationFix(model, posAcc, i, coordFix);
		v.color = { 1.0f, 1.0f, 1.0f, 1.0f };
		v.texCoord = { 0.0f, 0.0f }; // will fill if UVs exist
		v.normal = { 0.0f, 1.0f, 0.0f }; // replaced below if normals provided/generated
		v.tangent = { 1.0f, 0.0f, 0.0f, 1.0f }; // replaced below if tangents provided/generated
		data.vertices.push_back(v);
	}

	// Vertex colors (COLOR_0): vec3 or vec4 floats, or normalized ubyte/
	// ushort. Carries layered-material blend weights (RGBA) or a plain
	// albedo tint; defaults to white when absent.
	if (hasColors) {
		auto& colAcc = model.accessors[primitive.attributes.at("COLOR_0")];
		const int compCount = tinygltf::GetNumComponentsInType(colAcc.type);
		for (size_t i = 0; i < colAcc.count && i < data.vertices.size(); ++i) {
			data.vertices[i].color = readColorRGBA(model, colAcc, i, compCount);
		}
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
			// tangent-space handedness w is preserved as-is. Modes that
			// skip the engine fix (1/3) leave tangents untouched; the
			// Y-mirror (2, det -1) flips handedness, hence w.
			if (coordFix == 1 || coordFix == 3) {
				// Model already in engine space: no conversion.
			} else if (coordFix == 2) {
				tangent.y *= -1.0f;
				tangent.w *= -1.0f;
			} else {
				tangent.x *= -1.0f;
				tangent.z *= -1.0f;
			}

			data.vertices[i].tangent = tangent;
		}
	}


	if (hasAuthoredNormals) {
		auto& nAcc = model.accessors[primitive.attributes.at("NORMAL")];
		for (size_t i = 0; i < nAcc.count && i < data.vertices.size(); ++i) {
			glm::vec3 n = readVec3ForientationFix(model, nAcc, i, coordFix);
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
		smvk::ComputeSmoothNormals(data);
	}
	if (!hasAuthoredTangents && hasUVs) {
		smvk::GenerateTangents(data);
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
			primitive,
			coordFix_
		);

	// Cook-time meshoptimizer pass (off by default; the cooker enables it
	// via setMeshOptConfig). Overdraw optimization is gated off for
	// non-opaque primitives — it can hurt cutout rendering.
	smvk::LodInfo lod;
	lod.groupId = static_cast<uint32_t>(gltfMeshIndex);
	if (meshOptConfig.enabled && !data.vertices.empty() && !data.indices.empty()) {
		bool opaque = true;
		if (primitive.material >= 0 &&
			primitive.material < (int)model.materials.size()) {
			const std::string& am = model.materials[primitive.material].alphaMode;
			opaque = am.empty() || am == "OPAQUE";
		}
		smvk::MeshOptConfig cfg = meshOptConfig;
		if (!opaque)
			cfg.overdraw = false;
		smvk::MeshOptStats optStats;
		const size_t inVerts = data.vertices.size();
		if (OptimizeStaticMeshUpload(data, cfg,
				static_cast<uint32_t>(gltfMeshIndex), &optStats, &lod)) {
#ifndef NDEBUG
			std::cout << "MeshOpt mesh." << gltfMeshIndex << "/prim." << primitiveIndex
				<< ": verts " << inVerts << "->" << data.vertices.size()
				<< " tris " << optStats.triangles
				<< " ACMR " << optStats.acmrBefore << "->" << optStats.acmrAfter
				<< " overdraw " << optStats.overdrawBefore << "->" << optStats.overdrawAfter
				<< std::endl;
#endif
		}
	}
	else {
		lod.triCount = static_cast<uint32_t>(data.indices.size() / 3);
	}

	// Meshlet cook path (opt-in): builds from the optimized upload so the
	// meshlet windows match the vertex-cache-friendly triangle order.
	smvk::CookedMeshlets cooked;
	smvk::MeshletStats meshletStats;
	bool haveMeshlets = false;
	if (meshletConfig.enabled && !data.vertices.empty() && !data.indices.empty()) {
		std::string merr;
		if (BuildMeshletsForUpload(data, meshletConfig, &cooked, &meshletStats, &merr)) {
			haveMeshlets = true;
#ifndef NDEBUG
			std::cout << "Meshlets mesh." << gltfMeshIndex << "/prim." << primitiveIndex
				<< ": count " << meshletStats.meshletCount
				<< " tris " << meshletStats.totalTriangles
				<< " maxVerts " << meshletStats.maxVertsUsed
				<< " maxTris " << meshletStats.maxTrisUsed
				<< std::endl;
#endif
		}
		else if (!merr.empty() && merr != "meshlets disabled") {
#ifndef NDEBUG
			std::cout << "Meshlets mesh." << gltfMeshIndex << "/prim." << primitiveIndex
				<< " skipped: " << merr << std::endl;
#endif
		}
	}

	// Cooked LOD chain (opt-in via MeshOptConfig::generateLods): simplify
	// LOD0 into sibling uploads. Local errors are recorded here; world
	// scaling by node scale happens in FinalizeLodWorldErrors once globals
	// are final (shared meshes use the max instanced scale).
	std::vector<smvk::CookedLod> lodChain;
	if (meshOptConfig.generateLods && !data.vertices.empty() && !data.indices.empty()) {
		// Alpha cutout (MASK) and blended foliage sample coverage from the
		// base-color alpha via the UVs: positions-only simplify collapses
		// quad borders and stretches UVs, flashing background through the
		// leaves on every LOD switch. Use the coverage-preserving path.
		bool preserveCoverage = false;
		if (primitive.material >= 0 &&
			primitive.material < (int)model.materials.size()) {
			const std::string& am = model.materials[primitive.material].alphaMode;
			preserveCoverage = (am == "MASK" || am == "BLEND");
		}
		if (smvk::BuildLodChainForUpload(data, meshOptConfig, lodChain, preserveCoverage)) {
#ifndef NDEBUG
			for (size_t li = 0; li < lodChain.size(); ++li) {
				std::cout << "LOD mesh." << gltfMeshIndex << "/prim." << primitiveIndex
					<< "/lod" << (li + 1)
					<< ": tris " << lodChain[li].triCount
					<< " localErr " << lodChain[li].localError
					<< std::endl;
			}
#endif
		}
	}

	slot =
		cluster.UploadStaticMesh(data);

	if (!modelUriBase.empty()) {
		const smvk::StringViewT uri = smvk::MakeSubUri(modelUriBase,
			"mesh." + std::to_string(gltfMeshIndex) +
			"/prim." + std::to_string(primitiveIndex));
		lod.candidateCount = static_cast<uint32_t>(1 + lodChain.size());
		lod.lodIndex = 0;
		lod.simplifyError = 0.0f;
		cluster.RegisterMeshUri(uri, slot);
		cluster.RegisterMeshLod(uri, lod);
		if (haveMeshlets)
			cluster.RegisterMeshMeshlets(uri, cooked);
		// Siblings share the base groupId; scene keeps referencing LOD0.
		// Each sibling gets its own meshlets (same config as LOD0) so the
		// GPU filter can register every LOD: without them siblings draw
		// direct, unculled by frustum/HZB (freeze-view leak).
		for (size_t li = 0; li < lodChain.size(); ++li) {
			const smvk::CookedLod& kept = lodChain[li];
			const auto lh = cluster.UploadStaticMesh(kept.upload);
			if (!lh.valid())
				continue;
			const smvk::StringViewT lodUri = smvk::LodSiblingUri(uri, static_cast<uint32_t>(li + 1));
			if (meshletConfig.enabled && !kept.upload.vertices.empty()
				&& !kept.upload.indices.empty()) {
				smvk::CookedMeshlets lodCooked;
				smvk::MeshletStats lodStats;
				std::string lodErr;
				if (BuildMeshletsForUpload(kept.upload, meshletConfig,
						&lodCooked, &lodStats, &lodErr)
					&& !lodCooked.empty()) {
					cluster.RegisterMeshMeshlets(lodUri, lodCooked);
				}
#ifndef NDEBUG
				else if (!lodErr.empty() && lodErr != "meshlets disabled") {
					std::cout << "Meshlets mesh." << gltfMeshIndex << "/prim." << primitiveIndex
						<< "/lod" << (li + 1) << " skipped: " << lodErr << std::endl;
				}
#endif
			}
			cluster.RegisterMeshUri(lodUri, lh);
			smvk::LodInfo llod;
			llod.groupId = lod.groupId;
			llod.lodIndex = static_cast<uint32_t>(li + 1);
			llod.candidateCount = lod.candidateCount;
			llod.simplifyError = kept.localError; // scaled to world in finalize
			llod.screenThreshold = -1.0f;
			llod.triCount = kept.triCount;
			llod.acmr = 0.0f;
			cluster.RegisterMeshLod(lodUri, llod);
		}
	}

	return slot;
}

void GltfToSceneImporter::applyPreTransform(Scene& scene, int rootNode) {
	// Identity fast path (exact float compare is fine for a default matrix).
	static const glm::mat4 kIdentity{ 1.0f };
	if (preTransform_ == kIdentity)
		return;
	if (rootNode < 0 || (size_t)rootNode >= scene.localTransform.size())
		return;
	scene.localTransform[(size_t)rootNode] = preTransform_ * scene.localTransform[(size_t)rootNode];
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

	cluster.scene.localTransform[groupNode] = GetNodeLocalTransform(gltfNode, coordFix_);
	cluster.scene.globalTransform[groupNode] = glm::mat4(1.0f); // resolved later

	// Static-subtree opt-in: authoring extras or STATIC_ name prefix.
	// Flag is stamped directly; the staticNode cache is rebuilt once at the
	// end of the import flow (see GltfToClusterFromVfs / GltfToCluster).
	{
		const bool wantStatic =
			GltfExtrasStatic(gltfNode.extras) ||
			(!gltfNode.name.empty() && gltfNode.name.rfind("STATIC_", 0) == 0);
		if (wantStatic && (size_t)groupNode < cluster.scene.staticRoot.size())
			cluster.scene.staticRoot[(size_t)groupNode] = 1;
	}

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

HostTextureSource::HostTextureSource(std::filesystem::path baseDir)
	: baseDir_(std::move(baseDir)) {
}

std::optional<std::vector<uint8_t>> HostTextureSource::ReadBytes(const std::string& imageUri) {
	if (imageUri.empty() || starts_with(imageUri, "data:"))
		return std::nullopt;
	const std::filesystem::path full =
		(baseDir_ / std::filesystem::path(imageUri)).lexically_normal();
	return ReadHostBytes(full);
}

std::string HostTextureSource::ResolveIdentity(const std::string& imageUri) const {
	if (imageUri.empty())
		return {};
	const std::string joined =
		(baseDir_.empty() ? std::filesystem::path(imageUri)
			: (baseDir_ / std::filesystem::path(imageUri))).generic_string();
	const smvk::StringViewT n = smvk::NormalizeAssetUri(joined);
	return n.empty() ? imageUri : n.Str();
}

VfsTextureSource::VfsTextureSource(const smvk::OverlayVfs* vfs, std::string vfsBaseDir)
	: vfs_(vfs)
	, vfsBaseDir_(std::move(vfsBaseDir)) {
}

std::optional<std::vector<uint8_t>> VfsTextureSource::ReadBytes(const std::string& imageUri) {
	if (!vfs_ || imageUri.empty() || starts_with(imageUri, "data:"))
		return std::nullopt;
	return vfs_->TryRead(smvk::vfs::Join(vfsBaseDir_, imageUri));
}

std::string VfsTextureSource::ResolveIdentity(const std::string& imageUri) const {
	const smvk::StringViewT n = smvk::NormalizeAssetUri(smvk::vfs::Join(vfsBaseDir_, imageUri));
	return n.empty() ? imageUri : n.Str();
}

void GltfToClusterFromVfs(
	smvk::ResourceManager* ctx,
	const smvk::OverlayVfs& vfs,
	const std::string& modelVfsPath,
	smvk::Cluster& out,
	const smvk::MeshOptConfig& meshOpt,
	const smvk::MeshletConfig& meshlets,
	int coordFix,
	const glm::mat4& preTransform) {
	const std::string modelPath = smvk::NormalizeAssetUri(modelVfsPath).Str();
	if (modelPath.empty())
		throw std::runtime_error("GltfToClusterFromVfs: invalid VFS path: " + modelVfsPath);
	auto bytes = vfs.TryRead(modelPath);
	if (!bytes)
		throw std::runtime_error("GltfToClusterFromVfs: not in VFS: " + modelPath);

	const std::string baseDir = smvk::vfs::BaseDir(modelPath);

	// Route tinygltf's external reads (.bin buffers) through the VFS so the
	// whole cook works on the overlay, never on raw host paths.
	struct GltfFsCtx {
		const smvk::OverlayVfs* vfs;
		std::string baseDir;
	} fsCtx{ &vfs, baseDir };
	tinygltf::Model model;
	{
		tinygltf::FsCallbacks cbs;
		cbs.user_data = &fsCtx;
		cbs.ExpandFilePath = [](const std::string& path, void*) { return path; };
		cbs.FileExists = [](const std::string& path, void* user) -> bool {
			const auto* c = static_cast<const GltfFsCtx*>(user);
			return c->vfs->Exists(path) ||
				c->vfs->Exists(smvk::vfs::Join(c->baseDir, path));
		};
		cbs.ReadWholeFile = [](std::vector<unsigned char>* out, std::string* err,
				const std::string& filepath, void* user) -> bool {
			const auto* c = static_cast<const GltfFsCtx*>(user);
			if (auto b = c->vfs->TryRead(filepath)) {
				*out = std::move(*b);
				return true;
			}
			if (auto b = c->vfs->TryRead(smvk::vfs::Join(c->baseDir, filepath))) {
				*out = std::move(*b);
				return true;
			}
			if (err)
				*err = "VFS read failed: " + filepath;
			return false;
		};
		cbs.WriteWholeFile = [](std::string*, const std::string&,
				const std::vector<unsigned char>&, void*) { return false; };
		cbs.GetFileSizeInBytes = [](size_t* sizeOut, std::string*,
				const std::string& filepath, void* user) -> bool {
			const auto* c = static_cast<const GltfFsCtx*>(user);
			auto stat = c->vfs->Stat(filepath);
			if (!stat)
				stat = c->vfs->Stat(smvk::vfs::Join(c->baseDir, filepath));
			if (!stat)
				return false;
			if (sizeOut)
				*sizeOut = static_cast<size_t>(stat->size);
			return true;
		};

		tinygltf::TinyGLTF loader;
		loader.SetFsCallbacks(cbs);
		std::string err, warn;
		const std::string json(reinterpret_cast<const char*>(bytes->data()), bytes->size());
		if (!loader.LoadASCIIFromString(&model, &err, &warn,
				json.c_str(), static_cast<unsigned int>(json.size()), baseDir)) {
			throw std::runtime_error("GltfToClusterFromVfs: parse failed: " + modelPath + ": " + err);
		}
		if (!warn.empty())
#ifndef NDEBUG
			std::cout << "WARN: " << warn << std::endl;
		std::cout << "Loaded glTF from VFS: " << modelPath << std::endl;
#else
			(void)warn;
#endif
	}

	GltfToSceneImporter importer{ out, model, std::filesystem::path(modelPath) };
	importer.setTextureSource(
		std::make_shared<VfsTextureSource>(&vfs, baseDir));
	importer.setModelUriBase(modelPath);
	importer.setMeshOptConfig(meshOpt);
	importer.setMeshletConfig(meshlets);
	importer.setCoordFix(coordFix);
	importer.setPreTransform(preTransform);

	// Same tail as GltfToCluster (Engine/Utils/gltf.h): cluster root,
	// top-level glTF nodes, preload, import, fixup, flush.
	assert(out.scene.hierarchy.empty());
	const int root = addNode(out.scene, -1, 0);
	out.scene.localTransform[root] = glm::mat4(1.0f);
	out.scene.globalTransform[root] = glm::mat4(1.0f);

	std::vector<int> gltfRoots;
	if (model.defaultScene >= 0 && model.defaultScene < (int)model.scenes.size()) {
		gltfRoots = model.scenes[model.defaultScene].nodes;
	}
	else if (!model.scenes.empty()) {
		gltfRoots = model.scenes.front().nodes;
	}
	else {
		std::vector<bool> referenced(model.nodes.size());
		for (const auto& n : model.nodes)
			for (int c : n.children)
				if (c >= 0 && c < (int)model.nodes.size())
					referenced[c] = true;
		for (int i = 0; i < (int)model.nodes.size(); ++i)
			if (!referenced[i]) gltfRoots.push_back(i);
	}

	importer.preloadTextures();

	for (int gltfNode : gltfRoots)
		importer.import(gltfNode);

	importer.applyPreTransform(out.scene, root);

	RebuildStaticCache(out.scene);
	markAsChanged(out.scene, root);
	recalculateGlobalTransforms(out.scene);
	// Bake node world scale into cooked LOD errors (max over instances).
	smvk::FinalizeLodWorldErrors(out);
	if (out.ctx_)
		out.ctx_->FlushUploads();
	(void)ctx;
}

