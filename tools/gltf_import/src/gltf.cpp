// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define TINYGLTF_IMPLEMENTATION

#include <gltf_import/gltf.h>
#include <gltf_import/asset_export.h>
#include <ktx.h>
#include <Engine/Utils/texture_loader.h>
#include <Engine/Renderer/texture_streamer.h>
#include <Engine/Renderer/vertex_pack.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/Asset/asset_registry.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/core.h>


#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <map>
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

		// SMVK_materials_shadows.castShadow (stamped by content cooks,
		// e.g. decal overlays must not darken the geometry under them).
		bool castsShadows = true;
		// SMVK_materials_contactShadows.receive (default receive):
		// materials that must not pick up the sun contact march.
		bool receiveContactShadows = true;

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
	// VFS-style asset URI base for a model path. Relative/VFS paths
	// normalize directly. Absolute host paths (drive-letter or
	// posix-absolute) are rejected by NormalizeAssetUri — without a
	// fallback modelUriBase stays empty and the importer silently drops
	// mesh/material URIs, and with them LOD chains + meshlets (all
	// gated on a non-empty base). Fall back to the `content/`-relative
	// suffix when present (matches CLI cooks of the same file), else
	// the bare filename. Deterministic for a given path, so the editor
	// and the cooker agree.
	inline std::string ResolveUriBase(const std::string& path) {
		const smvk::StringViewT n =
			smvk::NormalizeAssetUri(smvk::StringViewT(path));
		if (!n.empty())
			return smvk::AssetFilePart(n);
		if (path.empty())
			return {};
		std::string posix = path;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		const std::string marker = "content/";
		std::string fallback;
		if (const size_t at = posix.find(marker); at != std::string::npos)
			fallback = posix.substr(at + marker.size());
		else
			fallback = std::filesystem::path(posix).filename().generic_string();
		const smvk::StringViewT fn =
			smvk::NormalizeAssetUri(smvk::StringViewT(fallback));
		if (fn.empty())
			return {};
		return smvk::AssetFilePart(fn);
	}
	// KTX2 file identifier (first 12 bytes) for payload validation.
	inline bool IsKtx2Payload(const std::vector<uint8_t>& bytes) {
		static constexpr uint8_t kMagic[12] = {
			0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
		return bytes.size() >= 12 && memcmp(bytes.data(), kMagic, 12) == 0;
	}
	// Texture-conversion cache key: FNV-1a 64 over the SOURCE bytes
	// with the transfer mixed in, then the same length/avalanche
	// finish as smvk::cache::HashBytes. The ONLY producer of these
	// keys is this TU; stores treat them as opaque strings.
	inline std::string TextureConversionHashImpl(const void* data,
		size_t size, bool srgb) {
		const auto* bytes = static_cast<const uint8_t*>(data);
		uint64_t h = 14695981039346656037ull;
		for (size_t i = 0; i < size; ++i) {
			h ^= bytes[i];
			h *= 1099511628211ull;
		}
		h ^= (srgb ? 0x9E3779B97F4A7C15ull : 0xC2B2AE3D27D4EB4Full);
		h ^= uint64_t(size) * 0x9E3779B97F4A7C15ull;
		h *= 0xFF51AFD7ED558CCDull;
		h ^= h >> 32;
		char hex[32];
		std::snprintf(hex, sizeof(hex), "%016llx",
			static_cast<unsigned long long>(h));
		return hex;
	}
	// ---- KTX auto-convert (file nodes + `.ktx2` sidecar cache) ----
	// Lowercase basename / stem for file-node matching (editor image defs
	// carry absolute host paths; glTF images carry relative URIs — the
	// basename bridges them, like the staging scripts' stem keys).
	inline std::string LowerStr(std::string s) {
		for (char& c : s)
			c = char(std::tolower(uint8_t(c)));
		return s;
	}
	inline std::string BaseNameOf(const std::string& p) {
		std::string posix = p;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		const size_t s = posix.find_last_of('/');
		const std::string base =
			(s == std::string::npos) ? posix : posix.substr(s + 1);
		return LowerStr(base);
	}
	inline std::string StemOf(const std::string& p) {
		const std::string base = BaseNameOf(p);
		const size_t d = base.find_last_of('.');
		return (d == std::string::npos) ? base : base.substr(0, d);
	}
	inline std::string ExtOf(const std::string& p) {
		const size_t d = p.find_last_of('.');
		const size_t s = p.find_last_of("/\\");
		if (d == std::string::npos || (s != std::string::npos && d < s))
			return {};
		return LowerStr(p.substr(d));
	}
	// HDR sources (.hdr/.exr) need float pipelines the in-process
	// transcoder doesn't have yet — stb would silently mangle them to
	// LDR, so they stay skipped (status quo) until an RGBA16F path lands.
	inline bool IsHdrImagePath(const std::string& p) {
		const std::string e = ExtOf(p);
		return e == ".hdr" || e == ".exr";
	}
	// True when sidecar KTX2 bytes carry a VTXfer stamp disagreeing with
	// the requested transfer (or fail to parse at all — a torn sidecar
	// must retranscode, never promote). Foreign sidecars (toktx, ...) carry
	// no stamp and are trusted by mtime.
	inline bool SidecarTransferMismatch(const std::vector<uint8_t>& ktx,
		bool wantSrgb) {
		ktxTexture2* tex = nullptr;
		if (ktxTexture2_CreateFromMemory(ktx.data(), ktx.size(), 0, &tex)
				!= KTX_SUCCESS
			|| !tex) {
			if (tex)
				ktxTexture2_Destroy(tex);
			return true;
		}
		unsigned int len = 0;
		void* val = nullptr;
		const bool found = ktxHashList_FindValue(&tex->kvDataHead, "VTXfer",
			&len, &val) == KTX_SUCCESS;
		bool mismatch = false;
		if (found) {
			const char* want = wantSrgb ? "srgb" : "linear";
			const size_t wantLen = std::strlen(want);
			mismatch = (len != wantLen)
				|| std::memcmp(val, want, wantLen) != 0;
		}
		ktxTexture2_Destroy(tex);
		return mismatch;
	}
	// GL sampler enums (core, stable): the file-node wrap/filter intent
	// maps onto the stashed TextureSamplerDesc in the same vocabulary
	// the glTF sampler path uses. Min filter assumes the generated chain.
	inline void ApplyFileNodeSampler(smvk::TextureSamplerDesc& samp,
		const smvk::tour::FileNode& fn) {
		samp.wrapS = samp.wrapT = (fn.wrap == 1) ? 33071 // CLAMP_TO_EDGE
			: (fn.wrap == 2) ? 33648 // MIRRORED_REPEAT
			: 10497; // REPEAT
		if (fn.filter != 0) {
			samp.magFilter = 9728; // NEAREST
			samp.minFilter = 9728; // NEAREST (no mips sampled)
		}
		else {
			samp.magFilter = 9729; // LINEAR
			samp.minFilter = 9987; // LINEAR_MIPMAP_LINEAR
		}
	}
}; // namespace

GltfToSceneImporter::GltfToSceneImporter(smvk::Cluster& inCluster, const tinygltf::Model& inModel, std::filesystem::path inGltfModelPath)
	: cluster(inCluster)
	, model(inModel)
	, gltfModelPath(std::move(inGltfModelPath))
	, textureSource(std::make_shared<HostTextureSource>(gltfModelPath.parent_path()))
	, modelUriBase(ResolveUriBase(gltfModelPath.generic_string()))
	, materialCache(inModel.materials.size())
{
	meshCache.resize(inModel.meshes.size());
	for (size_t i = 0; i < inModel.meshes.size(); ++i) {
		meshCache[i].resize(
			inModel.meshes[i].primitives.size()
		);
	}
	lightCache.resize(inModel.lights.size());
}

void GltfToSceneImporter::setTextureSource(std::shared_ptr<TextureSource> source) {
	if (source)
		textureSource = std::move(source);
}

void GltfToSceneImporter::setModelUriBase(const std::string& vfsPath) {
	const std::string base = ResolveUriBase(vfsPath);
	if (!base.empty())
		modelUriBase = base;
}

void GltfToSceneImporter::setMeshOptConfig(const smvk::MeshOptConfig& config) {
	meshOptConfig = config;
}

void GltfToSceneImporter::setMeshletConfig(const smvk::MeshletConfig& config) {
	meshletConfig = config;
}

void GltfToSceneImporter::setFileNodes(
	const std::vector<smvk::tour::FileNode>& nodes) {
	fileNodes_ = nodes;
}

const smvk::tour::FileNode* GltfToSceneImporter::matchFileNode(
	const std::string& imageUri) const {
	if (fileNodes_.empty() || imageUri.empty())
		return nullptr;
	const std::string base = BaseNameOf(imageUri);
	const std::string stem = StemOf(imageUri);
	for (const auto& fn : fileNodes_) { // exact basename first
		if (!fn.path.empty() && BaseNameOf(fn.path) == base)
			return &fn;
	}
	for (const auto& fn : fileNodes_) { // then stem / display name
		if (!fn.path.empty() && StemOf(fn.path) == stem)
			return &fn;
		if (!fn.name.empty() && LowerStr(fn.name) == stem)
			return &fn;
	}
	return nullptr;
}

bool GltfToSceneImporter::inferSrgb(int imageIndex) const {
	if (imageIndex < 0)
		return false;
	auto uses = [&](int texIdx) {
		return texIdx >= 0 && texIdx < int(model.textures.size())
			&& model.textures[size_t(texIdx)].source == imageIndex;
	};
	for (const auto& m : model.materials) {
		if (uses(m.pbrMetallicRoughness.baseColorTexture.index))
			return true;
		if (uses(m.emissiveTexture.index))
			return true;
	}
	return false; // data maps (and unknown uses) stay linear
}

bool GltfToSceneImporter::resolveKtxBytes(const std::string& imageUri,
	std::vector<uint8_t> srcBytes, int imageIndex, std::vector<uint8_t>& out,
	std::string& err) const {
	out.clear();
	if (IsKtx2Payload(srcBytes)) {
		out = std::move(srcBytes);
		return true;
	}
	// Off restores the old warn-and-skip wholesale (no promotion either:
	// a stale sidecar must never override an explicit opt-out).
	if (!autoKtx_) {
		err = "not a KTX2 payload (auto-ktx off): " + imageUri;
		return false;
	}
	// Conversion intent first: it validates the sidecar below (a
	// colorspace change must invalidate the cache, not silently reuse it).
	const smvk::tour::FileNode* fn = matchFileNode(imageUri);
	const bool srgb = fn ? (fn->colorspace == 0) : inferSrgb(imageIndex);
	// Host-backed sources can persist the conversion: `.ktx2` next to the
	// source, reused while fresher (avoids recooks; the editor promotes
	// the same sidecars at load).
	std::optional<std::filesystem::path> hostPath;
	if (textureSource)
		hostPath = textureSource->HostPath(imageUri);
	std::optional<std::filesystem::path> sidecar;
	if (hostPath && !hostPath->empty()) {
		std::filesystem::path sc = *hostPath;
		sc.replace_extension(".ktx2");
		sidecar = sc;
		std::error_code ec;
		if (std::filesystem::exists(sc, ec)
			&& std::filesystem::exists(*hostPath, ec)) {
			std::error_code ec2;
			const auto tSide = std::filesystem::last_write_time(sc, ec2);
			const auto tSrc = std::filesystem::last_write_time(*hostPath, ec2);
			if (!ec2 && tSide >= tSrc) {
				if (auto sb = ReadHostBytes(sc); sb && IsKtx2Payload(*sb)) {
					if (SidecarTransferMismatch(*sb, srgb)) {
						std::cerr << "KTX sidecar stale transfer, reconverting: "
							<< sc.generic_string() << std::endl;
					}
					else {
						std::cerr << "KTX promoted sidecar: "
							<< sc.generic_string() << std::endl;
						out = std::move(*sb);
						return true;
					}
				}
			}
		}
	}
	if (IsHdrImagePath(imageUri)) {
		err = "HDR source skipped (no float transcode yet): " + imageUri;
		return false;
	}
	// Scratch-disk store (second-chance cache, hash-validated): consult
	// BEFORE transcoding — the source-hash keys the SOURCE bytes +
	// transfer, so a hit is byte-identical to a fresh conversion. The
	// payload magic re-validates (a torn/corrupt entry reprocesses).
	// Never required: a miss (or no hook) just transcodes below, and
	// the shared tail stages the result so the next import/cook of the
	// same source hits (the owner flushes).
	std::string pendingStoreHash;
	if (artifactCache_) {
		const std::string sourceHash = TextureConversionHash(
			srcBytes.data(), srcBytes.size(), srgb);
		std::vector<uint8_t> cached;
		// Has BEFORE Get (registry-validated hit): the hash gates the
		// uri — a DIFFERENT source under the same uri (edited file, or
		// another model's identically-named texture) never serves. The
		// payload magic re-validates a torn entry. Adapters without a
		// registry (default Has=true) keep the legacy accept-any-hit.
		if (artifactCache_->Has("texture", imageUri, sourceHash)
			&& artifactCache_->Get("texture", imageUri, cached)
			&& IsKtx2Payload(cached)) {
			std::cerr << "KTX cache hit (scratch): " << imageUri
				<< " (" << cached.size() << " B)" << std::endl;
			out = std::move(cached);
			return true;
		}
		pendingStoreHash = sourceHash;
	}
	std::vector<uint8_t> ktx;
	std::string terr;
	if (!TranscodeImageToKtx2(srcBytes.data(), srcBytes.size(), srgb,
			imageUri, ktx, terr)) {
		err = terr;
		return false;
	}
	std::cerr << "KTX converted (" << (srgb ? "srgb" : "linear") << " "
		<< srcBytes.size() << " -> " << ktx.size() << " B): " << imageUri
		<< std::endl;
	if (!pendingStoreHash.empty()) {
		if (artifactCache_->Put("texture", imageUri, ktx.data(), ktx.size(),
				pendingStoreHash,
				srgb ? "{\"srgb\":1}" : "{\"srgb\":0}"))
			std::cerr << "KTX cache staged (scratch): " << imageUri
				<< " (" << ktx.size() << " B)" << std::endl;
		else
			std::cerr << "KTX cache stage failed (transcode stands): "
				<< imageUri << std::endl;
	}
	if (sidecar) { // best-effort cache; the bytes are used regardless
		std::error_code ec;
		std::filesystem::create_directories(sidecar->parent_path(), ec);
		std::ofstream f(*sidecar, std::ios::binary | std::ios::trunc);
		if (f) {
			f.write(reinterpret_cast<const char*>(ktx.data()),
				std::streamsize(ktx.size()));
			if (f.good())
				std::cerr << "KTX sidecar cached: "
					<< sidecar->generic_string() << std::endl;
			else
				std::cerr << "KTX sidecar write failed: "
					<< sidecar->generic_string() << std::endl;
		}
		else {
			std::cerr << "KTX sidecar write failed: "
				<< sidecar->generic_string() << std::endl;
		}
	}
	out = std::move(ktx);
	return true;
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

// KHR_lights_punctual -> SceneLight (node-local definition; world
// position/direction resolve from the node's global transform at use
// time, so coordFix + parents apply like meshes). Units are passed
// through verbatim: directional = lux, point/spot = candela (both the
// glTF and the engine shader convention, see Renderer/light.h).
// staticLight stamps kSceneLightFlag_Static (bake source; see
// scene.h). Definitions are deduplicated per glTF light index, so the
// FIRST instance's flag wins when several nodes reference the same
// light definition with different extras.
// Returns false for unknown types (caller skips the node link).
bool GltfLightToSceneLight(const tinygltf::Light& src, bool staticLight, SceneLight& dst) {
	dst.name = src.name;
	dst.color = glm::vec3(1.0f);
	if (src.color.size() >= 3) {
		dst.color = glm::vec3(
			static_cast<float>(src.color[0]),
			static_cast<float>(src.color[1]),
			static_cast<float>(src.color[2]));
	}
	dst.intensity = static_cast<float>(src.intensity);
	dst.range = static_cast<float>(src.range); // 0 = infinite
	dst.innerConeAngle = 0.0f;
	dst.outerConeAngle = 0.0f;
	dst.flags = 0;
	if (staticLight ||
		(!src.name.empty() && src.name.rfind("STATIC_", 0) == 0))
		dst.flags |= SceneLight::kSceneLightFlag_Static;
	if (src.type == "directional") {
		dst.type = SceneLight::Type::Directional;
	}
	else if (src.type == "point") {
		dst.type = SceneLight::Type::Point;
	}
	else if (src.type == "spot") {
		dst.type = SceneLight::Type::Spot;
		dst.innerConeAngle = static_cast<float>(src.spot.innerConeAngle);
		dst.outerConeAngle = static_cast<float>(src.spot.outerConeAngle);
	}
	else {
		return false;
	}
	return true;
}

// A glTF light node opts its light INTO the static bake set via
// authoring extras (checked in importNodeRecursive; the light NAME
// prefix "STATIC_" is handled in GltfLightToSceneLight):
//   extras: { "smvk.staticLight": true }
//     (also "smvk.static_light", "smvk.static-light",
//      { "smvk": { "staticLight": true } }, plain "staticLight")
// Static lights feed the lightmap/probe bakes and are excluded from
// the runtime forward+ set (see SceneLight::kSceneLightFlag_Static).
// KHR_lights_punctual node-level index: tinygltf v2.8.x parses the
// top-level lights array into model.lights but no longer surfaces the
// NODE's light reference as Node::light - it only lands in the node's
// raw extension object. Read it there (extension shape:
// "KHR_lights_punctual": { "light": <index> }).
int GltfNodeLightIndex(const tinygltf::Node& node) {
	auto it = node.extensions.find("KHR_lights_punctual");
	if (it == node.extensions.end())
		return -1;
	const tinygltf::Value& ext = it->second;
	if (!ext.IsObject() || !ext.Has("light"))
		return -1;
	const tinygltf::Value& idx = ext.Get("light");
	if (!idx.IsInt() && !idx.IsNumber())
		return -1;
	return idx.GetNumberAsInt();
}

bool GltfExtrasStaticLight(const tinygltf::Value& extras) {
	if (!extras.IsObject())
		return false;
	auto truthy = [](const tinygltf::Value& v) {
		if (v.IsBool())
			return v.Get<bool>();
		if (v.IsNumber())
			return v.GetNumberAsDouble() != 0.0;
		return false;
	};
	if (extras.Has("smvk.staticLight") && truthy(extras.Get("smvk.staticLight")))
		return true;
	if (extras.Has("smvk.static_light") && truthy(extras.Get("smvk.static_light")))
		return true;
	if (extras.Has("smvk.static-light") && truthy(extras.Get("smvk.static-light")))
		return true;
	if (extras.Has("staticLight") && truthy(extras.Get("staticLight")))
		return true;
	if (extras.Has("smvk")) {
		const tinygltf::Value& sub = extras.Get("smvk");
		if (sub.IsObject()) {
			if (sub.Has("staticLight") && truthy(sub.Get("staticLight")))
				return true;
			if (sub.Has("static_light") && truthy(sub.Get("static_light")))
				return true;
		}
	}
	return false;
}

// A glTF node opts OUT of lightmap charting/baking via authoring extras or
// naming convention (checked in importNodeRecursive):
//   extras: { "smvk.noLightmap": true }
//     (also "smvk.no_lightmap", "smvk.no-lightmap", bare "noLightmap",
//      { "smvk": { "noLightmap"/"no_lightmap": true } })
//   -or- name starts with "NOLM_".
// The flag stamps Scene::noLightmap on the group node; exclusion is subtree
// scoped (see IsNoLightmapNode). The editor writes these extras into its
// cook-time patched glTF; PakCooker skips flagged subtrees in the chart loop.
// NOTE: engine lightmap opt-in lives in the EDITOR (tour setLightmap),
// never in glTF extras — sources stay engine-flag-free.
bool GltfExtrasNoLightmap(const tinygltf::Value& extras) {
	if (!extras.IsObject())
		return false;
	auto truthy = [](const tinygltf::Value& v) {
		if (v.IsBool())
			return v.Get<bool>();
		if (v.IsNumber())
			return v.GetNumberAsDouble() != 0.0;
		return false;
	};
	if (extras.Has("smvk.noLightmap") && truthy(extras.Get("smvk.noLightmap")))
		return true;
	if (extras.Has("smvk.no_lightmap") && truthy(extras.Get("smvk.no_lightmap")))
		return true;
	if (extras.Has("smvk.no-lightmap") && truthy(extras.Get("smvk.no-lightmap")))
		return true;
	if (extras.Has("noLightmap") && truthy(extras.Get("noLightmap")))
		return true;
	if (extras.Has("smvk")) {
		const tinygltf::Value& sub = extras.Get("smvk");
		if (sub.IsObject()) {
			if (sub.Has("noLightmap") && truthy(sub.Get("noLightmap")))
				return true;
			if (sub.Has("no_lightmap") && truthy(sub.Get("no_lightmap")))
				return true;
		}
	}
	return false;
}

// A glTF node opts into dither-on-camera-closeup via authoring extras or
// naming convention (checked in importNodeRecursive):
//   extras: { "smvk.ditherCloseup": true }
//     (also "smvk.dither_closeup", { "smvk": { "ditherCloseup": true } })
//   -or- name starts with "DITHER_".
// Stamps Scene::ditherCloseup on the group node (subtree scoped).
bool GltfExtrasDitherCloseup(const tinygltf::Value& extras) {
	if (!extras.IsObject())
		return false;
	auto truthy = [](const tinygltf::Value& v) {
		if (v.IsBool())
			return v.Get<bool>();
		if (v.IsNumber())
			return v.GetNumberAsDouble() != 0.0;
		return false;
	};
	if (extras.Has("smvk.ditherCloseup") && truthy(extras.Get("smvk.ditherCloseup")))
		return true;
	if (extras.Has("smvk.dither_closeup") && truthy(extras.Get("smvk.dither_closeup")))
		return true;
	if (extras.Has("ditherCloseup") && truthy(extras.Get("ditherCloseup")))
		return true;
	if (extras.Has("smvk")) {
		const tinygltf::Value& sub = extras.Get("smvk");
		if (sub.IsObject()) {
			if (sub.Has("ditherCloseup") && truthy(sub.Get("ditherCloseup")))
				return true;
			if (sub.Has("dither_closeup") && truthy(sub.Get("dither_closeup")))
				return true;
		}
	}
	return false;
}
} // namespace

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
	// Input plugs (texture relink; see GltfTextureRemap): an EXACT oldUri
	// match redirects the byte read BEFORE the source is consulted — the
	// registered identity stays the ORIGINAL image's below (the import
	// stays addressable at its authored uri; the plug is a pure input).
	// Read order: the configured source first (relative/VFS-friendly),
	// then the raw host path (the plug is authored as a host path).
	std::optional<std::vector<uint8_t>> bytes;
	if (textureRemaps_) {
		for (const GltfTextureRemap& r : *textureRemaps_) {
			if (r.oldUri != img.uri || r.newPath.empty())
				continue;
			bytes = textureSource->ReadBytes(r.newPath);
			if (!bytes || bytes->empty())
				bytes = ReadHostBytes(std::filesystem::path(r.newPath));
			break;
		}
	}
	if (!bytes)
		bytes = textureSource->ReadBytes(img.uri);
	if (!bytes || bytes->empty()) {
		std::cerr << "Texture source missing image: " << img.uri << std::endl;
		noteImage(imageIndex, "", false);
		return {};
	}
	const std::string identity = textureSource->ResolveIdentity(img.uri);
	// File-node intent for this image (colorspace for conversion, sampler
	// override below); null when no file node covers it.
	const smvk::tour::FileNode* fn = matchFileNode(img.uri);
	// KTX2 passes through; anything else promotes a fresh `.ktx2`
	// sidecar or transcodes in-process (+caches the sidecar when
	// host-backed). Unconvertible sources fail loudly here in both
	// modes (cook mints below, runtime uploads below).
	{
		std::vector<uint8_t> ktx;
		std::string kerr;
		if (!resolveKtxBytes(img.uri, std::move(*bytes), imageIndex, ktx,
				kerr)) {
				std::cerr << (cluster.IsOfflineMode() ? "Offline: " : "Texture: ")
				<< kerr << std::endl;
			noteImage(imageIndex, "", false);
			return {};
		}
		bytes = std::move(ktx);
	}

	smvk::TextureHandle h;
	if (cluster.IsOfflineMode()) {
		h = smvk::TextureHandle::Mint(cluster.AllocOfflineTextureId());
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
			if (!holder.valid()) {
				noteImage(imageIndex, identity, false);
				return {};
			}
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
		// A covering file node wins over the glTF sampler (authored
		// tiling/filter intent travels with the image definition).
		if (fn)
			ApplyFileNodeSampler(samp, *fn);
		cluster.RegisterTextureSampler(identity, samp);
		// The streaming path already stashed the payload above (moved-from
		// `bytes` is empty there); the full-res path stashes it here.
		if (bytes.has_value() && !bytes->empty())
			cluster.SetTextureSource(identity, std::move(*bytes));
	}
	noteImage(imageIndex, identity, true);
	return h;
}

// ---- importer data rows (see setImportImages) --------------------------
GltfImportImage* GltfToSceneImporter::importImageRow(int imageIndex) {
	if (!importImages_ || imageIndex < 0
		|| imageIndex >= (int)model.images.size())
		return nullptr;
	auto it = importImageRows_.find(imageIndex);
	if (it != importImageRows_.end())
		return &(*importImages_)[it->second];
	GltfImportImage row{};
	row.source = gltfModelPath.generic_string();
	row.uri = model.images[size_t(imageIndex)].uri;
	importImages_->push_back(std::move(row));
	importImageRows_[imageIndex] = importImages_->size() - 1;
	return &importImages_->back();
}

void GltfToSceneImporter::noteImage(int imageIndex,
	const std::string& identity, bool loaded) {
	if (GltfImportImage* row = importImageRow(imageIndex)) {
		row->identity = identity;
		row->loaded = loaded;
	}
}

void GltfToSceneImporter::noteImageUser(int imageIndex,
	const std::string& materialUri, const char* slot) {
	if (GltfImportImage* row = importImageRow(imageIndex)) {
		GltfImportImage::User u{};
		u.materialUri = materialUri;
		u.slot = slot;
		row->users.push_back(u);
	}
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

	// SMVK_materials_shadows: cook-stamped shadow participation
	// ({"castShadow": false} on decal overlay materials).
	if (auto it = m.extensions.find("SMVK_materials_shadows");
		it != m.extensions.end() && it->second.IsObject()) {
		const auto& obj = it->second.Get<tinygltf::Value::Object>();
		if (auto f = obj.find("castShadow");
			f != obj.end() && (f->second.IsBool() || f->second.IsNumber())) {
			mat.castsShadows = f->second.IsBool()
				? f->second.Get<bool>()
				: f->second.GetNumberAsDouble() != 0.0;
		}
	}

	// SMVK_materials_contactShadows: receive participation for the sun
	// contact march ({"receive": false} on mirror-like / emissive
	// materials). Default receive.
	if (auto it = m.extensions.find("SMVK_materials_contactShadows");
		it != m.extensions.end() && it->second.IsObject()) {
		const auto& obj = it->second.Get<tinygltf::Value::Object>();
		if (auto f = obj.find("receive");
			f != obj.end() && (f->second.IsBool() || f->second.IsNumber())) {
			mat.receiveContactShadows = f->second.IsBool()
				? f->second.Get<bool>()
				: f->second.GetNumberAsDouble() != 0.0;
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

	// Untouched core PBR block (no metallicRoughness texture, spec-default
	// metallic=1/roughness=1): tinygltf cannot tell "omitted" from
	// "authored 1.0", and the spec default prints every such mesh as metal
	// (F0 = albedo kills diffuse; SSR/specular fire everywhere). Forced
	// dielectric 0 like every other default path here (spec-gloss
	// fallback, transmission, default material). Materials WITH an MR
	// texture keep factor 1.0 (it multiplies the texel, per spec), and an
	// authored roughness < 1.0 keeps its metallic as written.
	if (!mat.metallicRoughnessTex &&
		mat.metallicFactor == 1.0 && mat.roughnessFactor == 1.0)
		mat.metallicFactor = 0.0;

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
		std::string uri; // glTF-relative image URI (sidecar + file-node key)
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
		// Input plugs (texture relink): EXACT oldUri match redirects the
		// read (mirrors loadTextureForImage; the identity stays the
		// original's below).
		std::optional<std::vector<uint8_t>> bytes;
		if (textureRemaps_) {
			for (const GltfTextureRemap& r : *textureRemaps_) {
				if (r.oldUri != img.uri || r.newPath.empty())
					continue;
				bytes = textureSource->ReadBytes(r.newPath);
				if (!bytes || bytes->empty())
					bytes = ReadHostBytes(std::filesystem::path(r.newPath));
				break;
			}
		}
		if (!bytes)
			bytes = textureSource->ReadBytes(img.uri);
		if (!bytes || bytes->empty()) {
			std::cerr << "Texture source missing image: " << img.uri << std::endl;
			continue;
		}
		Pending p;
		p.imageIndex = imageIndex;
		auto tf = firstTextureForImage.find(imageIndex);
		p.textureIndex = tf != firstTextureForImage.end() ? tf->second : -1;
		p.uri = img.uri;
		p.identity = textureSource->ResolveIdentity(img.uri);
		p.bytes = std::move(*bytes);
		pendings.push_back(std::move(p));
	}

	if (pendings.empty()) return;

	// Cook mode: no GPU at all — KTX2 passes through, anything else
	// promotes a fresh `.ktx2` sidecar or transcodes in-process; then mint
	// stable ids, register uri/sampler/source, done. Everything here is
	// CPU-only so no threads are needed.
	if (cluster.IsOfflineMode()) {
		for (Pending& p : pendings) {
			std::vector<uint8_t> ktx;
			std::string kerr;
			if (!resolveKtxBytes(p.uri, std::move(p.bytes), p.imageIndex,
					ktx, kerr)) {
				std::cerr << "Offline: " << kerr << std::endl;
				continue;
			}
			p.bytes = std::move(ktx);
			const smvk::TextureHandle h =
				smvk::TextureHandle::Mint(cluster.AllocOfflineTextureId());
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
				// A covering file node wins over the glTF sampler.
				if (const smvk::tour::FileNode* fn = matchFileNode(p.uri))
					ApplyFileNodeSampler(samp, *fn);
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

// Basis conversion for the IMPORTED CHAIN TOP (see importUnder): the
// coordFix basis applies ONCE at the top of every imported chain
// (C * M_top) instead of baking vertices + conjugating every node's
// local. Node locals stay RAW (exactly as the glTF author wrote them:
// identity nodes stay identity, rotations/translations read as
// written) and vertices/normals/tangents/AABBs stay raw — the world
// basis of every node is C * gltfWorld, so the render lands exactly
// where the per-node conjugation put it, but the authoring frame is
// honest. Modes:
//   0 = engine fix (180 deg Y: -X,+Y,-Z, det +1, winding preserved)
//   1/3 = skip (already in engine space)
//   2 = Y mirror (det -1; mirrored winding is NOT reflipped — same
//       limitation as before)
inline glm::mat4 CoordFixBasis(int coordFix) {
	glm::mat4 c(1.0f);
	if (coordFix == 2)
		c[1][1] = -1.0f;
	else if (coordFix == 0) {
		c[0][0] = -1.0f;
		c[2][2] = -1.0f;
	}
	return c;
}

glm::mat4 GetNodeLocalTransform(const tinygltf::Node& n)
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
	// RAW as authored: the coordFix basis rides the chain top
	// (importUnder), never the node locals.
	return m;
}

std::string TextureConversionHash(const void* data, size_t size,
	bool srgb) {
	return TextureConversionHashImpl(data, size, srgb);
}

glm::vec3 GltfPunctualAimAxis(int) {
	// glTF punctual lights aim along their node's -Z axis. Raw locals
	// (the basis rides the chain top): the world aim reads straight off
	// the node's global transform — globalTransform * (0,0,-1) — in
	// every coordFix mode. (The old per-node conjugation needed the
	// coordFix-0 special case +Z read; the chain-top basis kills it.)
	return glm::vec3(0.0f, 0.0f, -1.0f);
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

	// Importer data users (see setImportImages): every (material, slot)
	// pair that references an image gets noted — loaded AND failed rows
	// alike (the failed ones are exactly what the relink flow targets).
	// The material uri matches the registration below ("<base>#mat.N").
	const std::string matUri = modelUriBase.empty()
		? std::string()
		: smvk::MakeSubUri(modelUriBase,
			"mat." + std::to_string(gltfMaterialIndex)).Str();
	auto load = [&](const std::optional<GltfTextureRef>& ref,
			const char* slotName) -> smvk::TextureHandle {
		if (!ref) return {};
		noteImageUser(ref->imageIndex, matUri, slotName);
		const smvk::TextureHandle h =
			loadTextureForImage(ref->imageIndex, ref->textureIndex);
		if (h.valid()) {
			// The PRELOAD path registered this texture without passing
			// here's recording (textureCache short-circuits above); the
			// sequential path already noted the identical row inside
			// loadTextureForImage — this note is idempotent for it and
			// fills the preloaded case.
			noteImage(ref->imageIndex,
				textureSource
					? textureSource->ResolveIdentity(
						model.images[size_t(ref->imageIndex)].uri)
					: std::string(),
				true);
		}
		return h;
	};

	smvk::TextureHandle baseColorHandle = load(mat.baseColorTex, "baseColorTex");
	smvk::TextureHandle metallicRoughHandle = load(mat.metallicRoughnessTex, "metallicRoughTex");
	smvk::TextureHandle normalHandle = load(mat.normalTex, "normalTex");
	smvk::TextureHandle occlusionHandle = load(mat.occlusionTex, "occlusionTex");
	smvk::TextureHandle emissiveHandle = load(mat.emissiveTex, "emissiveTex");

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
	ci.castsShadows = mat.castsShadows;
	ci.receiveContactShadows = mat.receiveContactShadows;
	// Far-LOD shading program (see ShaderLodPolicy): DEFAULT OFF — the
	// derivation only runs when the derive config opts in
	// (DeriveConfig::shaderLods / setDeriveShaderLods). The cheap
	// vertex-lit program has no alpha-texture cutout and no blending, so
	// only opaque materials may downgrade at distance - masked/blend stay
	// fully lit regardless. Tour authors can still program any material
	// either way (farVertexLit).
	ci.allowFarVertexLit = deriveShaderLods_
		&& (ci.alphaMode == smvk::AlphaMode::Opaque);

	slot = cluster.CreateMaterial(ci);
	if (!modelUriBase.empty())
		cluster.RegisterMaterialUri(
			smvk::MakeSubUri(modelUriBase,
				"mat." + std::to_string(gltfMaterialIndex)),
			slot);

	return slot;
}

smvk::ClusterLightHandle GltfToSceneImporter::getOrCreateLight(int gltfLightIndex, bool staticLight) {
	assert(gltfLightIndex >= 0);
	assert(
		static_cast<size_t>(gltfLightIndex) <
		model.lights.size()
	);

	auto& slot = lightCache[gltfLightIndex];
	if (!slot.empty()) {
		return slot;
	}

	SceneLight dst;
	if (!GltfLightToSceneLight(model.lights[gltfLightIndex], staticLight, dst))
		return {}; // unknown type: skip the node link

	slot = cluster.CreateLight(std::move(dst));
	if (!slot.valid())
		return {};
	if (!modelUriBase.empty())
		cluster.RegisterLightUri(
			smvk::MakeSubUri(modelUriBase,
				"light." + std::to_string(gltfLightIndex)),
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

			// glTF stores AABB in the accessor; RAW as authored (the
			// coordFix basis rides the chain top, never the geometry).
			const std::vector<double>& min = accessor.minValues;
			const std::vector<double>& max = accessor.maxValues;

			data.aabbMin = glm::vec3(min[0], min[1], min[2]);
			data.aabbMax = glm::vec3(max[0], max[1], max[2]);
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
	const bool hasUV2s = primitive.attributes.find("TEXCOORD_1") != primitive.attributes.end();
	const bool hasAuthoredTangents = primitive.attributes.find("TANGENT") != primitive.attributes.end();
	const bool hasAuthoredNormals = primitive.attributes.find("NORMAL") != primitive.attributes.end();
	const bool hasColors = primitive.attributes.find("COLOR_0") != primitive.attributes.end();

	auto& posAcc = model.accessors[primitive.attributes.at("POSITION")];
	data.vertices.reserve(posAcc.count);
	for (size_t i = 0; i < posAcc.count; ++i) {
		smvk::ImportStaticVertex v{};
			v.pos = readVec3F(model, posAcc, i);
		v.color = { 1.0f, 1.0f, 1.0f, 1.0f };
		v.texCoord = { 0.0f, 0.0f }; // will fill if UVs exist
		v.texCoord2 = { 0.0f, 0.0f }; // lightmap UV (TEXCOORD_1), if present
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

	// Lightmap UV channel (TEXCOORD_1): authored lightmap UVs, or zeros
	// when the scene carries none (the lightmap cook overwrites these
	// with chart-local UVs + chart ids at bake time; the shader gates on
	// the per-draw flag, never on the value).
	if (hasUV2s) {
		auto& uv2Acc = model.accessors[primitive.attributes.at("TEXCOORD_1")];
		for (size_t i = 0; i < uv2Acc.count && i < data.vertices.size(); ++i) {
			data.vertices[i].texCoord2 = readVec2F(model, uv2Acc, i);
		}
	}


	if (hasAuthoredTangents) {
		auto& tangentAcc = model.accessors[primitive.attributes.at("TANGENT")];
		for (size_t i = 0; i < tangentAcc.count && i < data.vertices.size(); ++i) {
			glm::vec4 tangent = readVec4F(model, tangentAcc, i);
			// RAW as authored (the basis rides the chain top): tangent
			// space — including the handedness w — stays exactly as
			// the glTF author wrote it.
			data.vertices[i].tangent = tangent;
		}
	}


	if (hasAuthoredNormals) {
		auto& nAcc = model.accessors[primitive.attributes.at("NORMAL")];
		for (size_t i = 0; i < nAcc.count && i < data.vertices.size(); ++i) {
			glm::vec3 n = readVec3F(model, nAcc, i);
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

// Packs a primitive's JOINTS_0/WEIGHTS_0 into the PackedSkinInfluences stream
// (top-4 renormalized RGBA8; joint ids in the glTF JOINTS_0 order = the
// cluster Skeleton joint table). Built in the POSITION-accessor order,
// parallel to the mesh upload's vertices, so OptimizeStaticMeshUpload can
// keep it in lockstep through the vertex remap. Returns false for an
// unskinned primitive (missing/invalid attributes or empty).
static bool PackVertexSkinStream(const tinygltf::Model& model,
	const tinygltf::Primitive& prim,
	std::vector<smvk::PackedSkinInfluences>& out)
{
	auto jt = prim.attributes.find("JOINTS_0");
	auto wt = prim.attributes.find("WEIGHTS_0");
	if (jt == prim.attributes.end() || wt == prim.attributes.end()
		|| jt->second < 0 || (size_t)jt->second >= model.accessors.size()
		|| wt->second < 0 || (size_t)wt->second >= model.accessors.size())
		return false;
	const tinygltf::Accessor& jAcc = model.accessors[size_t(jt->second)];
	const tinygltf::Accessor& wAcc = model.accessors[size_t(wt->second)];
	const size_t verts = size_t(jAcc.count);
	if (verts == 0 || size_t(wAcc.count) < verts)
		return false;
	out.resize(verts);
	for (size_t v = 0; v < verts; ++v) {
		uint32_t j[4] = { 0u, 0u, 0u, 0u };
		float w[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		{
			const uint8_t* p = gltfPtr(model, jAcc, v);
			if (p) {
				for (int k = 0; k < 4; ++k) {
					if (jAcc.componentType
						== TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
						j[k] = p[k];
					else if (jAcc.componentType
						== TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
						const uint16_t* s16 =
							reinterpret_cast<const uint16_t*>(p);
						j[k] = s16[k];
					}
					else if (jAcc.componentType
						== TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
						const uint32_t* s32 =
							reinterpret_cast<const uint32_t*>(p);
						j[k] = s32[k];
					}
				}
			}
		}
		{
			const uint8_t* p = gltfPtr(model, wAcc, v);
			if (p) {
				for (int k = 0; k < 4; ++k) {
					if (wAcc.componentType
						== TINYGLTF_COMPONENT_TYPE_FLOAT)
						w[k] = reinterpret_cast<const float*>(p)[k];
					else if (wAcc.componentType
						== TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
						w[k] = float(p[k]) * (1.0f / 255.0f);
					else if (wAcc.componentType
						== TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
						w[k] = float(reinterpret_cast<const uint16_t*>(
							p)[k]) * (1.0f / 65535.0f);
				}
			}
		}
		// Degenerate zero weights fall back to joint 0 at full weight
		// (never a NaN normal in the shader).
		float sum = w[0] + w[1] + w[2] + w[3];
		if (!(sum > 1e-6f)) {
			j[0] = j[1] = j[2] = j[3] = 0u;
			w[0] = 1.0f;
			w[1] = w[2] = w[3] = 0.0f;
			sum = 1.0f;
		}
		out[v].joints =
			((j[0] & 0xFFu))
			| ((j[1] & 0xFFu) << 8)
			| ((j[2] & 0xFFu) << 16)
			| ((j[3] & 0xFFu) << 24);
		out[v].weights = glm::packUnorm4x8(glm::vec4(
			w[0] / sum, w[1] / sum, w[2] / sum, w[3] / sum));
	}
	return true;
}

smvk::ClusterStaticMeshHandle GltfToSceneImporter::getOrCreateMesh(int gltfMeshIndex, int primitiveIndex, bool skinned)
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

	// Attach the skin stream BEFORE the meshopt pass so the weld +
	// vertex-fetch remaps carry it in lockstep with `data.vertices`
	// (OptimizeStaticMeshUpload remaps `skin` alongside). A stream
	// attached after optimization would be in ACCESSOR order while the
	// vertices are reordered - bind-pose masks it (all joints = bind) but
	// animation scatters the weights into spikes.
	if (skinned)
		PackVertexSkinStream(model, primitive, data.skin);

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
		if (haveMeshlets) {
			cluster.RegisterMeshMeshlets(uri, cooked);
			// Attach to the uploaded mesh object too (the pak
			// deserializer does the same on load): the GPU filter
			// registration and the meshlet debug view read
			// mesh->meshlets, not the URI map. Cooker-safe (the pak
			// serializer reads the map, so output is unchanged).
			if (smvk::StaticMesh* m = cluster.staticMeshes.get(slot))
				m->meshlets = cooked;
		}
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
					// Same attach as LOD0 above: the filter registers
					// every LOD from mesh->meshlets (without them
					// siblings draw direct, unculled).
					if (smvk::StaticMesh* lm = cluster.staticMeshes.get(lh))
						lm->meshlets = lodCooked;
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
void GltfToSceneImporter::importNodeRecursive(int gltfNodeIndex,
	int parentSceneNode, int level, bool topOfImport) {
	const tinygltf::Node& gltfNode = model.nodes[gltfNodeIndex];

	// glTF node index -> cluster id map (the asset derivation retargets
	// animations/material bindings onto the deterministic cluster ids).
	if (gltfNodeMap_.size() < model.nodes.size())
		gltfNodeMap_.resize(model.nodes.size(), -1);

	// Always create a "group" node that owns name + transform and preserves hierarchy.
	const int groupNode = addNode(cluster.scene, parentSceneNode, level);
	gltfNodeMap_[size_t(gltfNodeIndex)] = groupNode;

	{
		const int nameIndex = (int)cluster.scene.nodeNames.size();
		cluster.scene.nodeNames.push_back(gltfNode.name.empty() ? "Node" : gltfNode.name);
		SetNodeNameIndex(cluster.scene, groupNode, nameIndex);
	}

	// RAW local (as authored). The TOP node of each imported chain
	// (importUnder) additionally carries the coordFix basis on the
	// left (C * M_top): the whole chain lands exactly where the old
	// per-node conjugation put it, while every descendant's transform
	// stays the honest glTF TRS (identity nodes stay identity —
	// the local gizmo + the inspector read true).
	cluster.scene.localTransform[groupNode] =
		(topOfImport ? CoordFixBasis(coordFix_) : glm::mat4(1.0f))
		* GetNodeLocalTransform(gltfNode);
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

	// Lightmap opt-out: authoring extras or NOLM_ name prefix. Stored on
	// the group node; IsNoLightmapNode() resolves subtree membership.
	{
		const bool wantNoLm =
			GltfExtrasNoLightmap(gltfNode.extras) ||
			(!gltfNode.name.empty() && gltfNode.name.rfind("NOLM_", 0) == 0);
		if (wantNoLm) {
			if (cluster.scene.noLightmap.size() != cluster.scene.hierarchy.size())
				cluster.scene.noLightmap.resize(cluster.scene.hierarchy.size(), 0);
			if ((size_t)groupNode < cluster.scene.noLightmap.size())
				cluster.scene.noLightmap[(size_t)groupNode] = 1;
		}
	}

	// Dither on camera closeup: authoring extras or DITHER_ name prefix.
	// Stored on the group node; IsDitherCloseupNode() resolves subtree
	// membership. Cooked into the pak scene blob (v6 section).
	{
		const bool wantDither =
			GltfExtrasDitherCloseup(gltfNode.extras) ||
			(!gltfNode.name.empty() && gltfNode.name.rfind("DITHER_", 0) == 0);
		if (wantDither) {
			if (cluster.scene.ditherCloseup.size() != cluster.scene.hierarchy.size())
				cluster.scene.ditherCloseup.resize(cluster.scene.hierarchy.size(), 0);
			if ((size_t)groupNode < cluster.scene.ditherCloseup.size())
				cluster.scene.ditherCloseup[(size_t)groupNode] = 1;
		}
	}

	// KHR_lights_punctual: node.light indexes the top-level lights array
	// (tinygltf 2.8.x keeps it only in the node's raw extension object -
	// GltfNodeLightIndex reads it back). Definitions
	// are deduplicated into the Cluster light pool under "light.<gltf
	// index>" URIs (same registry as meshes); the group node stores the
	// pool index in lightForNode, and world position/direction resolve
	// from globalTransform at cook/runtime so parents + coordFix apply
	// exactly like meshes. Node extras "smvk.staticLight" (or a STATIC_
	// light name) flags the light as a bake source - first instance
	// wins when the definition is shared.
	const int gltfNodeLight = GltfNodeLightIndex(gltfNode);
	if (gltfNodeLight >= 0 && gltfNodeLight < (int)model.lights.size()) {
		const smvk::ClusterLightHandle h =
			getOrCreateLight(gltfNodeLight, GltfExtrasStaticLight(gltfNode.extras));
		if (h.valid())
			SetLight(cluster.scene, (uint32_t)groupNode, h.index());
	}

	// Unroll primitives into separate meshes (your current approach).
	if (gltfNode.mesh >= 0) {
		const tinygltf::Mesh& gltfMesh = model.meshes[gltfNode.mesh];
		const bool hasSkin = gltfNode.skin >= 0
			&& (size_t)gltfNode.skin < model.skins.size();

		for (int primIndex = 0; primIndex < (int)gltfMesh.primitives.size(); ++primIndex) {
			const tinygltf::Primitive& primitive = gltfMesh.primitives[primIndex];

			const int primNode = addNode(cluster.scene, groupNode, level + 1);
			cluster.scene.localTransform[primNode] = glm::mat4(1.0f);
			cluster.scene.globalTransform[primNode] = glm::mat4(1.0f);
			const smvk::ClusterStaticMeshHandle primMesh =
				getOrCreateMesh(
					gltfNode.mesh,
					primIndex,
					hasSkin
				);
			SetMesh(cluster.scene, primNode, primMesh.index());
			SetMaterial(cluster.scene, primNode,
				getOrCreateMaterial(
					primitive.material
				).index());
			markAsChanged(cluster.scene, primNode);
			// SKIN RECORD (the Northlight data path): node.skin indexes
			// the top-level skins array; the JOINTS_0/WEIGHTS_0 streams +
			// the Skeleton build in the POST pass (BuildSkinData - the
			// gltfNodeMap must be fully populated first; joint nodes may
			// import after this one). Recording here keeps the primitive's
			// own attributes available (the post pass re-reads them from
			// the recorded indices).
			if (hasSkin && primMesh.valid())
				skinRefs_.push_back(SkinRef{ primMesh,
					gltfNode.skin, gltfNode.mesh, primIndex });
		}
	}

	// Recurse children under the group node to preserve hierarchy
	// (never top-of-import: only the chain's first node carries the
	// coordFix basis).
	for (int child : gltfNode.children) {
		importNodeRecursive(child, groupNode, level + 1, false);
	}
}

// ---------------------------------------------------------------------------
// SKIN post pass (the Northlight data path). The node walk recorded every
// (mesh, node.skin) pair; this builds the cluster Skeleton (joints via the
// FULLY-POPULATED gltfNodeMap - joint nodes may import after the meshes
// that reference them, which is why this is a post pass) and patches every
// recorded mesh's retained upload with the packed PackedSkinInfluences stream
// (mesh.h, binding 2: RGBA8_UInt joint ids + RGBA8_UNorm weights, top-4
// influences renormalized). The stream rides the upload through the
// serializer (the mesh blob's trailing skin section); the skeleton lands in
// the scene blob's trailing section. Both load back at pak load.
//
// The cluster carries ONE Skeleton (the FIRST skin wins - the knight has
// exactly one; a per-mesh table is the multi-skin generalization,
// deliberately deferred). A joint node outside the scene graph (never
// imported) aborts the whole pass loudly: a half-skinned skeleton renders
// wrong in ways nothing downstream can detect.
// ---------------------------------------------------------------------------
int GltfToSceneImporter::BuildSkinData() {
	if (skinRefs_.empty())
		return 0;
	if (model.skins.empty()) {
		std::cerr << "[Skin] node.skin references exist but the glTF"
			" carries no skins (skinned as static)" << std::endl;
		skinRefs_.clear();
		return 0;
	}
	// The cluster Skeleton: the FIRST skin wins (documented above).
	if (!cluster.hasSkeleton) {
		const tinygltf::Skin& skin =
			model.skins[size_t(skinRefs_[0].skinIndex)];
		smvk::touranim::Skeleton& sk = cluster.skeleton;
		sk.joints.clear();
		sk.inverseBind.clear();
		sk.joints.reserve(skin.joints.size());
		sk.inverseBind.resize(skin.joints.size(), glm::mat4(1.0f));
		for (size_t i = 0; i < skin.joints.size(); ++i) {
			const int j = skin.joints[i];
			const int clusterNode = (j >= 0
				&& (size_t)j < gltfNodeMap_.size())
				? gltfNodeMap_[size_t(j)] : -1;
			if (clusterNode < 0) {
				std::cerr << "[Skin] joint node " << j << " ("
					<< (j >= 0 && (size_t)j < model.nodes.size()
						? model.nodes[size_t(j)].name : "?")
					<< ") was never imported - the joint hierarchy must"
					" be part of the scene graph (skin pass aborted,"
					" meshes render static)" << std::endl;
				skinRefs_.clear();
				return 0;
			}
			sk.joints.push_back(uint32_t(clusterNode));
		}
		// inverseBindMatrices: FLOAT MAT4 accessor (column-major 16
		// floats == glm's layout); identity when the skin omits it.
		if (skin.inverseBindMatrices >= 0
			&& (size_t)skin.inverseBindMatrices < model.accessors.size()) {
			const tinygltf::Accessor& acc =
				model.accessors[size_t(skin.inverseBindMatrices)];
			for (size_t i = 0; i < sk.joints.size()
				&& i < size_t(acc.count); ++i) {
				const uint8_t* raw = gltfPtr(model, acc, i);
				if (!raw)
					break;
				const float* p = reinterpret_cast<const float*>(raw);
				glm::mat4 m(1.0f);
				for (int c = 0; c < 4; ++c)
					for (int r = 0; r < 4; ++r)
						m[c][r] = p[c * 4 + r];
				sk.inverseBind[i] = m;
			}
		}
		// Skeleton root: the skin's authored `skeleton` node when
		// present; otherwise the glTF spec fallback - the joint
		// hierarchy's own root (the joint whose parent is not itself a
		// joint; the knight's skin omits `skeleton`). The old hardcoded
		// 0u fallback pointed at the cluster-root placeholder - wrong
		// data for every skin-authored-without-skeleton source.
		if (skin.skeleton >= 0
			&& (size_t)skin.skeleton < gltfNodeMap_.size()
			&& gltfNodeMap_[size_t(skin.skeleton)] >= 0) {
			sk.root = uint32_t(gltfNodeMap_[size_t(skin.skeleton)]);
		}
		else {
			// glTF nodes carry no parent field - derive it from the
			// children lists, then take the first joint (skin order)
			// whose parent is missing or not a joint.
			std::unordered_set<int> jointSet(skin.joints.begin(),
				skin.joints.end());
			std::unordered_map<int, int> parentOf;
			for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
				for (int child : model.nodes[ni].children)
					parentOf[child] = int(ni);
			}
			int rootGltf = -1;
			for (int j : skin.joints) {
				const auto pit = parentOf.find(j);
				if (pit == parentOf.end() || !jointSet.count(pit->second)) {
					rootGltf = j;
					break;
				}
			}
			sk.root = (rootGltf >= 0
				&& (size_t)rootGltf < gltfNodeMap_.size()
				&& gltfNodeMap_[size_t(rootGltf)] >= 0)
				? uint32_t(gltfNodeMap_[size_t(rootGltf)]) : 0u;
			std::cerr << "[Skin] skeleton root derived from the joint"
				" hierarchy: node " << sk.root << " ("
				<< (sk.root < cluster.scene.hierarchy.size()
					? getNodeName(cluster.scene, int(sk.root))
					: std::string("?"))
				<< ") - the skin omits 'skeleton'\n";
		}
		cluster.hasSkeleton = true;
	}
	// Per-recorded-mesh skin streams: JOINTS_0/WEIGHTS_0 -> packed
	// PackedSkinInfluences (top-4 renormalized RGBA8).
	int patched = 0;
	for (const SkinRef& r : skinRefs_) {
		if (r.skinIndex < 0 || (size_t)r.skinIndex >= model.skins.size()
			|| r.gltfMeshIndex < 0
			|| (size_t)r.gltfMeshIndex >= model.meshes.size())
			continue;
		const tinygltf::Mesh& gm = model.meshes[size_t(r.gltfMeshIndex)];
		if (r.primitiveIndex < 0
			|| (size_t)r.primitiveIndex >= gm.primitives.size())
			continue;
		const tinygltf::Primitive& prim =
			gm.primitives[size_t(r.primitiveIndex)];
		// The stream attached in getOrCreateMesh (BEFORE the meshopt pass)
		// already rode OptimizeStaticMeshUpload's vertex remap, so it is in
		// FINAL vertex order - prefer it. The accessor-order pack is the
		// fallback for an unoptimized mesh (no reorder => the orders agree).
		std::vector<smvk::PackedSkinInfluences> stream;
		if (const std::vector<smvk::PackedSkinInfluences>* existing =
				cluster.GetRetainedSkinStream(r.mesh);
			existing && !existing->empty()) {
			stream = *existing;
		}
		else if (!PackVertexSkinStream(model, prim, stream)) {
			std::cerr << "[Skin] mesh '" << gm.name
				<< "' has node.skin but no JOINTS_0/WEIGHTS_0"
				" (skinned as static)" << std::endl;
			continue;
		}
		cluster.PatchRetainedSkinStream(r.mesh, std::move(stream));
		++patched;
	}
	skinRefs_.clear();
	if (patched > 0)
		std::cout << "[Skin] skinned " << patched << " mesh(es); the"
			" cluster skeleton has " << cluster.skeleton.Count()
			<< " joints" << std::endl;
	return patched;
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

std::optional<std::filesystem::path> HostTextureSource::HostPath(
	const std::string& imageUri) const {
	if (imageUri.empty() || starts_with(imageUri, "data:"))
		return std::nullopt;
	return (baseDir_ / std::filesystem::path(imageUri)).lexically_normal();
}

VfsTextureSource::VfsTextureSource(const smvk::OverlayVfs* vfs, std::string vfsBaseDir)
	: vfs_(vfs)
	, vfsBaseDir_(std::move(vfsBaseDir)) {
}

namespace {
	// True when uri already lives under dir (a full VFS URI, not a bare
	// filename): joining it again would double the path (base + base/file)
	// and miss. Derivations store either form (the MeshImporter writes
	// full URIs; older derivations wrote bare names) - both must read.
	bool VfsUriUnderDir(const std::string& uri, const std::string& dir) {
		if (dir.empty() || uri.size() <= dir.size())
			return false;
		if (uri.compare(0, dir.size(), dir) != 0)
			return false;
		const char c = uri[dir.size()];
		return c == '/' || c == '\\';
	}
} // namespace

std::optional<std::vector<uint8_t>> VfsTextureSource::ReadBytes(const std::string& imageUri) {
	if (!vfs_ || imageUri.empty() || starts_with(imageUri, "data:"))
		return std::nullopt;
	if (VfsUriUnderDir(imageUri, vfsBaseDir_))
		return vfs_->TryRead(imageUri);
	return vfs_->TryRead(smvk::vfs::Join(vfsBaseDir_, imageUri));
}

std::string VfsTextureSource::ResolveIdentity(const std::string& imageUri) const {
	// Named local: NormalizeAssetUri returns a BORROWED view (into its
	// input's storage), so feeding it a temporary Join() result leaves the
	// view dangling at the end of the full expression — the UAF that
	// crashed every cook at preloadTextures (ASan: freed at :2119, read at
	// :2120). Mirror HostTextureSource::ResolveIdentity's named `joined`.
	// Full VFS URIs resolve as-is (see ReadBytes above); bare names join.
	const std::string joined = VfsUriUnderDir(imageUri, vfsBaseDir_)
		? imageUri
		: smvk::vfs::Join(vfsBaseDir_, imageUri);
	const smvk::StringViewT n = smvk::NormalizeAssetUri(joined);
	return n.empty() ? imageUri : n.Str();
}

std::vector<GltfTextureRemap> ReadTextureRemapPlugs(
	const std::string& resolvedSourcePath) {
	// INPUT PLUGS (see gltf.h's block): texture-relink rows serialized in
	// the SOURCE asset's .meta sidecar — "plug.texture.<n>.old=<uri>" +
	// "plug.texture.<n>.new=<path>" pairs. The sidecar FORMAT is owned by
	// artifact_cache (assetdb.h — its write side rewrites the whole file and
	// preserves these rows); this is the tolerant read every import entry
	// point shares. Missing sidecar / no plug rows = empty (the common case).
	std::vector<GltfTextureRemap> out;
	if (resolvedSourcePath.empty())
		return out;
	std::ifstream in(resolvedSourcePath + ".meta", std::ios::binary);
	if (!in)
		return out;
	// Index-keyed halves, paired below (an unpaired row skips — the writer
	// always writes pairs, so this only rejects hand edits).
	std::map<size_t, std::pair<std::string, std::string>> halves;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = line.substr(0, eq);
		const std::string val = line.substr(eq + 1);
		// key = plug.texture.<n>.old | plug.texture.<n>.new
		if (key.rfind("plug.texture.", 0) != 0)
			continue;
		const size_t lastDot = key.rfind('.');
		if (lastDot == std::string::npos)
			continue;
		const std::string idxStr = key.substr(
			sizeof("plug.texture.") - 1, lastDot - (sizeof("plug.texture.") - 1));
		const std::string half = key.substr(lastDot + 1);
		if (idxStr.empty() || idxStr.find_first_not_of("0123456789")
				!= std::string::npos)
			continue;
		size_t idx = 0;
		try {
			idx = std::stoull(idxStr);
		}
		catch (...) {
			continue;
		}
		auto& pair = halves[idx];
		if (half == "old")
			pair.first = val;
		else if (half == "new")
			pair.second = val;
	}
	for (auto& [idx, pair] : halves) {
		(void)idx;
		if (!pair.first.empty() && !pair.second.empty())
			out.push_back(GltfTextureRemap{ pair.first, pair.second });
	}
	return out;
}

// ---------------------------------------------------------------------------
// Nested scene references (see ApplySceneReferences in gltf.h).
// ---------------------------------------------------------------------------
// (The VFS glTF import entries — LoadGltfModelFromVfs /
// ListGltfScenesFromVfs / GltfToClusterFromVfs — were REMOVED: glTF is
// IMPORT-ONLY (the MeshImporter, asset_export.h ImportMeshSource, parses
// the HOST source once and writes the .geometry artifact family). The
// runtime/build side (the editor, PakCooker, GiBaker, this reference
// resolution) reads the ARTIFACTS through the VFS/host — the VFS stays the
// runtime abstraction (patching, overlays, pak-embedded artifacts later);
// it just never carries glTF. Raw --in-vfs model paths name the .geometry.)
// ---------------------------------------------------------------------------
namespace {

// <stem>.tour next to a scene source (editor convention).
std::string ReferenceTourPathFor(const std::filesystem::path& src) {
	auto t = src;
	t.replace_extension(".tour");
	return t.generic_string();
}

// ".tour" source (any case): an authored scene — its source gltf is
// imported, then its tour commands replay with id offsets.
bool IsTourSource(const std::string& source) {
	const size_t dot = source.find_last_of('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = source.substr(dot);
	for (char& c : ext)
		c = char(std::tolower((unsigned char)c));
	return ext == ".tour";
}

// Read a whole tour file (VFS when vfs != null, else host fs).
bool ReadTourTextAt(const std::string& tourPath,
	const smvk::OverlayVfs* vfs, std::string& out) {
	if (vfs) {
		if (auto b = vfs->TryRead(tourPath)) {
			out.assign(reinterpret_cast<const char*>(b->data()), b->size());
			return true;
		}
		return false;
	}
	std::ifstream tf(tourPath, std::ios::binary);
	if (!tf)
		return false;
	std::stringstream ss;
	ss << tf.rdbuf();
	out = ss.str();
	return true;
}

} // namespace

// (See the header decl for the contract.) The definition stays here beside
// the reference resolution (its primary consumer) + the helpers it shares
// (ReferenceTourPathFor / the anonymous-namespace block above).
size_t BuildFromMeshAsset(
	smvk::Cluster& cluster,
	smvk::TextureStreamer* streamer,
	const smvk::assets::MeshAsset& asset,
	const smvk::assets::AssetManifest& manifest,
	const std::string& meshBaseUri,
	int ph,
	const std::shared_ptr<GltfToSceneImporter::TextureSource>& textureSource,
	bool autoKtx,
	GltfArtifactCache* artifactCache,
	const std::function<std::optional<std::vector<uint8_t>>(
		const std::string&)>& readSibling)
{
	// 1) Geometry uploads (base blobs + LOD sibling blobs, AoS via the
	// inverse pack — UploadStaticMesh handles cook + GPU + streaming).
	auto makeUpload = [](const smvk::assets::MeshAsset::Geometry& g) {
		smvk::StaticMeshUpload up{};
		up.vertices.resize(g.positions.size());
		// Uncharted blobs carry NO lightmap stream (count-gated on disk);
		// the unpack uses an all-uncharted record for those (the inverse
		// pack's sentinel round-trips to chartId 0xFFFFFFFF anyway).
		const smvk::PackedVertexLightmap lmNone{};
		for (size_t v = 0; v < g.positions.size(); ++v)
			smvk::UnpackStaticVertex(g.positions[v], g.shading[v],
				!g.lightmap.empty() ? g.lightmap[v] : lmNone,
				up.vertices[v]);
		up.indices = g.indices;
		up.aabbMin = g.aabbMin;
		up.aabbMax = g.aabbMax;
		// The skin stream (the Northlight data path): the BASE blobs
		// carry it (the .geometry's trailing skin section); the upload
		// rides it to the retained upload (the pak serializer's source).
		up.skin = g.skin;
		return up;
	};
	std::vector<smvk::StaticMeshUpload> uploads;
	uploads.reserve(asset.geometry.size());
	for (const auto& g : asset.geometry)
		uploads.push_back(makeUpload(g));
	for (const auto& chain : asset.lodChains)
		for (const auto& s : chain)
			uploads.push_back(makeUpload(s.geo));
	if (uploads.empty())
		return 0;
	std::vector<smvk::ClusterStaticMeshHandle> handles =
		cluster.UploadStaticMeshes(uploads);
	if (handles.size() != uploads.size())
		return 0;

	// 2) Texture loader (loadTextureForImage's mirror, URI-driven from
	// the .material refs; the identity dedupes shared images).
	auto loadTexture = [&](const std::string& uri,
			bool srgb) -> smvk::TextureHandle {
		if (!textureSource || uri.empty())
			return {};
		const std::string identity =
			textureSource->ResolveIdentity(uri);
		if (identity.empty())
			return {};
		if (auto hit = cluster.FindTextureByUri(identity))
			return *hit;
		auto bytes = textureSource->ReadBytes(uri);
		if (!bytes || bytes->empty())
			return {};
		std::vector<uint8_t> ktx;
		if (IsKtx2Payload(*bytes)) {
			ktx = std::move(*bytes);
		}
		else {
			if (!autoKtx)
				return {};
			const std::string hash = TextureConversionHash(
				bytes->data(), bytes->size(), srgb);
			const bool hitCache = artifactCache
				&& artifactCache->Has("texture", identity, hash)
				&& artifactCache->Get("texture", identity, ktx)
				&& IsKtx2Payload(ktx);
			if (!hitCache) {
				std::string terr;
				if (!TranscodeImageToKtx2(bytes->data(),
						bytes->size(), srgb, uri, ktx, terr))
					return {};
				if (artifactCache)
					artifactCache->Put("texture", identity,
						ktx.data(), ktx.size(), hash,
						srgb ? "{\"srgb\":1}" : "{\"srgb\":0}");
			}
		}
		smvk::TextureHandle h;
		if (cluster.IsOfflineMode()) {
			h = smvk::TextureHandle::Mint(cluster.AllocOfflineTextureId());
		}
		else if (!cluster.ctx_) {
			return {};
		}
		else if (streamer && !identity.empty()) {
			cluster.SetTextureSource(identity, std::move(ktx));
			const std::vector<uint8_t>* stored =
				cluster.GetTextureSource(identity);
			if (!stored || stored->empty())
				return {};
			auto holder = streamer->StreamTexture(
				stored->data(), stored->size(), identity);
			if (!holder.valid())
				return {};
			h = holder;
			cluster.textures.create(std::move(holder));
		}
		else {
			auto holder = smvk::LoadKtxTextureMemory(
				*cluster.ctx_, ktx.data(), ktx.size(), identity);
			if (!holder.valid())
				return {};
			h = holder;
			cluster.textures.create(std::move(holder));
		}
		cluster.RegisterTextureUri(identity, h);
		// Stash unconditionally (offline mode included): the pak serializer
		// embeds these bytes, and the CLI offline cluster is built fresh from
		// the artifact - nothing else stashes them (the editor path has
		// them from its import session). Skipping the stash in offline mode
		// made every CLI cook fail at serialize with "missing embedded
		// .ktx2 source".
		cluster.SetTextureSource(identity, std::move(ktx));
		return h;
	};

	// 3) Materials in FIRST-USE order over the node-sorted instances
	// (matches the import's creation order; tour material indices stay
	// stable). GUID -> the manifest entry -> the .material sibling.
	std::unordered_map<std::string, smvk::ClusterMaterialHandle> matOf;
	auto materialFor =
			[&](const std::string& guid) -> smvk::ClusterMaterialHandle {
		if (guid.empty())
			return {};
		auto it = matOf.find(guid);
		if (it != matOf.end())
			return it->second;
		const smvk::assets::AssetEntry* ent = nullptr;
		for (const auto& e : manifest.entries)
			if (e.kind == smvk::assets::AssetKind::Material
				&& e.guid == guid) {
				ent = &e;
				break;
			}
		if (!ent)
			return {};
		auto bytes = readSibling(ent->file);
		if (!bytes || bytes->empty())
			return {};
		smvk::assets::MaterialAsset m;
		std::string merr;
		if (!smvk::assets::LoadMaterialAssetText(
				std::string(bytes->begin(), bytes->end()), m, merr))
			return {};
		smvk::MaterialCreateInfo info{};
		info.baseColorFactor = m.info.baseColorFactor;
		info.emissiveFactor = m.info.emissiveFactor;
		info.emissiveStrength = m.info.emissiveStrength;
		info.metallicFactor = m.info.metallicFactor;
		info.roughnessFactor = m.info.roughnessFactor;
		info.normalScale = m.info.normalScale;
		info.occlusionStrength = m.info.occlusionStrength;
		// Shading model: the asset round-trip historically lost Masked
		// here (the exporter wrote shading=lit for alphaMode=mask) - the
		// lit FS's cutout gates on ShadingModel::Masked (== 2.0f), so the
		// lost flag killed the alpha test and masked foliage rendered
		// fully opaque. Derive like the direct glTF import does (the
		// alphaMode mapping at getOrCreateMaterial): Mask + DefaultLit ->
		// Masked. An explicitly-authored Unlit/Masked shading wins.
		info.shadingModel = (m.info.alphaMode == smvk::AlphaMode::Mask
			&& m.info.shadingModel == smvk::ShadingModel::DefaultLit)
			? smvk::ShadingModel::Masked : m.info.shadingModel;
		info.alphaMode = m.info.alphaMode;
		info.alphaCutoff = m.info.alphaCutoff;
		info.doubleSided = m.info.doubleSided;
		info.castsShadows = m.info.castsShadows;
		info.receiveContactShadows = m.info.receiveContactShadows;
		auto loadTex = [&](const smvk::assets::MaterialAsset::TexRef& t,
				smvk::TextureHandle& slot) {
			if (t.path.empty())
				return;
			smvk::TextureHandle h = loadTexture(t.path, t.srgb);
			if (!h.empty())
				slot = h;
		};
		loadTex(m.baseColor, info.baseColorTexture);
		loadTex(m.metallicRoughness, info.metallicRoughnessTexture);
		loadTex(m.normalTex, info.normalTexture);
		loadTex(m.occlusion, info.occlusionTexture);
		loadTex(m.emissiveTex, info.emissiveTexture);
		info.diffuse = info.baseColorTexture; // legacy alias
		smvk::ClusterMaterialHandle h = cluster.CreateMaterial(info);
		if (!h.empty()) {
			cluster.RegisterMaterialUri("asset://" + guid, h);
			matOf.emplace(guid, h);
		}
		return h;
	};

	// 4) Registry: URIs (the derivation's fragments, re-based on the
	// .mesh path so cooks address identically) + meshlets + LOD chains.
	std::vector<smvk::ClusterStaticMeshHandle> baseHandles;
	{
		size_t up = 0;
		for (size_t gi = 0; gi < asset.geometry.size(); ++gi) {
			const smvk::ClusterStaticMeshHandle h = handles[up++];
			const smvk::assets::MeshAsset::Geometry& g =
				asset.geometry[gi];
			const std::string uri = smvk::MakeSubUri(meshBaseUri,
				g.uriFragment.empty()
				? "mesh." + std::to_string(gi) : g.uriFragment).Str();
			cluster.RegisterMeshUri(uri, h);
			cluster.RegisterMeshMeshlets(uri, g.meshlets);
			if (auto* m = cluster.staticMeshes.get(h)) {
				m->meshlets = g.meshlets;
				// Cooked lightmap binding (the COOK wrote it back;
				// import.exe leaves it uncharted). Stamping it here
				// restores the bake without running one.
				m->lmChunk = g.lmChunk;
				m->lmChartXforms = g.lmChartXforms;
			}
			smvk::LodInfo lod{};
			lod.lodIndex = 0;
			lod.candidateCount = uint32_t(1
				+ ((gi < asset.lodChains.size())
					? asset.lodChains[gi].size() : 0));
			cluster.RegisterMeshLod(uri, lod);
			baseHandles.push_back(h);
		}
		for (size_t gi = 0; gi < asset.lodChains.size(); ++gi) {
			for (const auto& s : asset.lodChains[gi]) {
				const smvk::ClusterStaticMeshHandle h = handles[up++];
				const std::string baseUri = smvk::MakeSubUri(
					meshBaseUri, asset.geometry[gi].uriFragment).Str();
				const std::string sibUri =
					smvk::LodSiblingUri(baseUri,
						s.lodIndex > 0 ? s.lodIndex : 1).Str();
				cluster.RegisterMeshUri(sibUri, h);
				cluster.RegisterMeshMeshlets(sibUri, s.geo.meshlets);
				if (auto* m = cluster.staticMeshes.get(h)) {
					m->meshlets = s.geo.meshlets;
					m->lmChunk = s.geo.lmChunk;
					m->lmChartXforms = s.geo.lmChartXforms;
				}
				smvk::LodInfo lod{};
				lod.lodIndex = s.lodIndex;
				lod.simplifyError = s.simplifyError;
				lod.screenThreshold = s.screenThreshold;
				lod.triCount = s.triCount;
				lod.acmr = s.acmr;
				cluster.RegisterMeshLod(sibUri, lod);
			}
		}
	}

	// 5) Scene graph: node 0 of the asset is the derivation's cluster
	// root (the placeholder takes its place); nodes 1.. build under the
	// mapped parents (parent 0 -> the placeholder), ids ph+i aligning
	// with the sibling-glTF import (the .tour replay contract).
	Scene& scene = cluster.scene;
	if (asset.nodes.empty())
		return 0;
	std::vector<int> mapped(asset.nodes.size(), -1);
	mapped[0] = ph;
	for (size_t i = 1; i < asset.nodes.size(); ++i) {
		const int ap = asset.nodes[i].parent;
		const int parent = (ap <= 0) ? ph : mapped[size_t(ap)];
		const int n = addNode(scene, parent, asset.nodes[i].level);
		mapped[i] = n;
		if (i < asset.locals.size())
			scene.localTransform[size_t(n)] = asset.locals[i];
		if (i < asset.names.size() && !asset.names[i].empty())
			setNodeName(scene, n, asset.names[i]);
		// Global resolution is lazy (changedAtThisFrame): mark EVERY
		// mapped node, not just the instanced ones below. Unmarked
		// ancestors keep identity globals, so no-tour bakes read the
		// raw (e.g. centimeter) geometry as meters and the bake
		// octree explodes (the tour replay marks via its own edits,
		// which is why tour bakes never saw this).
		markAsChanged(scene, n);
	}
	for (size_t i = 1; i < asset.nodes.size(); ++i) {
		if (i < asset.staticRoot.size() && asset.staticRoot[i])
			MarkSubtreeStatic(scene, mapped[i]);
		if (i < asset.noLightmap.size() && asset.noLightmap[i])
			MarkNoLightmap(scene, mapped[i]);
	}
	// 5b) SKIN restore (the Northlight data path): the skeleton's joint
	// ids are in the ASSET node space (the node list is a 1:1 copy of the
	// import cluster's scene) - remap them through `mapped`. The joint-0
	// placeholder (the asset root) is invalid for a joint: any joint id 0
	// aborts the skeleton loudly (a half-mapped skeleton renders wrong in
	// ways nothing downstream can detect). The per-BASE-blob skin streams
	// already rode the uploads (makeUpload); stamp the runtime-side map +
	// the skinned flags for the draw path.
	if (asset.hasSkeleton) {
		smvk::touranim::Skeleton& sk = cluster.skeleton;
		sk = asset.skeleton;
		bool badJoint = false;
		for (uint32_t& j : sk.joints) {
			if (j == 0u || j >= mapped.size() || mapped[j] < 0) {
				badJoint = true;
				break;
			}
			j = uint32_t(mapped[j]);
		}
		if (sk.root < mapped.size() && mapped[sk.root] >= 0)
			sk.root = uint32_t(mapped[sk.root]);
		else
			badJoint = true;
		if (badJoint) {
			cluster.skeleton = smvk::touranim::Skeleton{};
			cluster.hasSkeleton = false;
			std::fprintf(stderr,
				"[Skin] .geometry skeleton joint maps outside the"
				" node range (dropped; meshes render static)\n");
		}
		else {
			cluster.hasSkeleton = true;
			for (size_t gi = 0; gi < asset.geometry.size()
				&& gi < baseHandles.size(); ++gi) {
				if (!asset.geometry[gi].skin.empty())
					cluster.SetMeshSkinStream(baseHandles[gi],
						std::vector<smvk::PackedSkinInfluences>(
							asset.geometry[gi].skin));
			}
		}
	}
	// 6) Bindings: instances (geometry + material), lights, cameras.
	for (const auto& inst : asset.instances) {
		if (inst.node == 0 || inst.node >= asset.nodes.size()
			|| inst.geometry >= baseHandles.size())
			continue;
		const uint32_t n = uint32_t(mapped[inst.node]);
		SetMesh(scene, n, baseHandles[inst.geometry].index());
		smvk::ClusterMaterialHandle mh =
			materialFor(inst.materialGuid);
		if (!mh.empty())
			SetMaterial(scene, n, mh.index());
		markAsChanged(scene, int(n));
	}
	for (const auto& l : asset.lights) {
		if (l.node == 0 || l.node >= asset.nodes.size())
			continue;
		smvk::ClusterLightHandle h = cluster.CreateLight(l.def);
		if (!h.empty())
			SetLight(scene, uint32_t(mapped[l.node]), h.index());
	}
	for (const auto& c : asset.cameras) {
		if (c.node == 0 || c.node >= asset.nodes.size())
			continue;
		smvk::ClusterCameraHandle h = cluster.CreateCamera(c.def);
		if (!h.empty())
			SetCamera(scene, uint32_t(mapped[c.node]), h.index());
	}
	return asset.nodes.size() - 1;
}

size_t ApplySceneReferences(
	smvk::Cluster& cluster,
	const smvk::tour::Doc& doc,
	const std::string& ownerBaseDir,
	smvk::ResourceManager* ctx,
	smvk::TextureStreamer* streamer,
	const smvk::OverlayVfs* vfs,
	const smvk::MeshOptConfig& meshOpt,
	const smvk::MeshletConfig& meshlets,
	int coordFix,
	std::vector<int>* outPlaceholderNodes,
	std::vector<std::string>* outIssues,
	std::vector<std::string>* outResolvedSources,
	bool autoKtx,
	const std::vector<smvk::tour::FileNode>* fileNodes,
	GltfArtifactCache* artifactCache,
	GltfGuidResolver* guidResolver,
	std::vector<GltfImportImage>* outImages,
	const std::string& contentRoot) {
	(void)ctx;
	auto issue = [&](const std::string& msg) {
		if (outIssues)
			outIssues->push_back(msg);
	};
	// HOST-AWARE path utils: vfs::BaseDir/FileName are VFS-RELATIVE
	// utilities — vfs::Normalize REJECTS drive-letter absolutes (C:/...)
	// by design, so calling them on a resolved HOST path returns EMPTY
	// (the nested-level baseDir died there: relative nested sources
	// like "gear.tour" could never resolve on the host path). This
	// helper pair picks the right machinery per mode.
	auto baseDirOf = [&](const std::string& p) -> std::string {
		if (vfs)
			return smvk::vfs::BaseDir(p);
		return std::filesystem::path(p).parent_path()
			.generic_string();
	};
	auto fileNameOf = [&](const std::string& p) -> std::string {
		if (vfs)
			return smvk::vfs::FileName(p);
		return std::filesystem::path(p).filename()
			.generic_string();
	};
	if (doc.references.empty())
		return 0;
	struct Level {
		smvk::tour::Doc doc;        // owning copy (nested docs parsed fresh)
		std::string baseDir;        // resolve relative sources against this
		std::vector<std::string> chain; // normalized sources up the chain
		std::string nsPrefix;       // reference-namespace chain ("a:b:")
		                             // applied to THIS level's inlines
		int depth = 0;
		uint32_t nodeBase = 0; // owner-space offset of THIS level's scene
		                       // (0 for the owner itself): nested reference
		                       // parents address their own scene's ids
	};
	constexpr int kMaxRefDepth = 8;
	std::vector<Level> queue;
	{
		Level l{};
		l.doc = doc; // copy (cheap enough; references + commands vectors)
		l.baseDir = ownerBaseDir;
		l.depth = 0;
		queue.push_back(std::move(l));
	}
	size_t applied = 0;
	size_t warnings = 0;
	while (!queue.empty()) {
		Level cur = std::move(queue.front());
		queue.erase(queue.begin());
		for (const auto& r : cur.doc.references) {
			// One entry per top-level reference in OUT list order
			// (filled with the canonical path once resolution is
			// known; stays empty on every early exit): keeps the
			// editor's parallel list aligned by construction.
			size_t resolvedIdx = size_t(-1);
			if (cur.depth == 0 && outResolvedSources) {
				resolvedIdx = outResolvedSources->size();
				outResolvedSources->push_back({});
			}
			auto noteResolved = [&](const std::string& canon) {
				if (resolvedIdx != size_t(-1))
					(*outResolvedSources)[resolvedIdx] = canon;
			};
			// Disabled references INLINE ANYWAY (keep-in-scene
			// semantics): the placeholder + subtree always bind, so
			// every cluster id stays STABLE across enable/disable
			// toggles — edits made on the referenced nodes can never
			// be orphaned by a toggle. Disabled only marks the
			// placeholder subtree hidden (below).
			if (cur.depth >= kMaxRefDepth) {
				const std::string m = "[References] depth cap reached at '" +
					r.source + "' (skipped)";
				std::cerr << m << "\n";
				issue(m);
				++warnings;
				continue;
			}
			// Parent ids live in THIS level's scene space: level 0 IS
			// owner space; deeper levels carry the offset their scene
			// was inlined with (the old code treated every parent as an
			// owner id, silently misparenting nested references).
			const uint64_t pRaw = (cur.depth == 0)
				? uint64_t(r.parent)
				: uint64_t(cur.nodeBase) + uint64_t(r.parent);
			// Resolve the source (as-is, then owner-relative). A .tour
			// source is an authored scene: parse it now; its `source`
			// gltf (resolved against the tour's dir first) is what gets
			// imported below, then its commands replay with offsets.
			smvk::tour::Doc refTour;
			bool haveRefTour = false;
			std::string refTourDir;
			std::string modelSource = r.source;
			// GUID-FIRST resolution (asset identity survives moves/
			// renames): the injected resolver maps the source file's
			// .meta GUID to its CURRENT path. Unknown/invalid GUID or
			// no resolver (cooker/baker/tests) keeps the recorded path.
			if (!r.guid.empty() && guidResolver) {
				const std::string byGuid =
					guidResolver->PathForGuid(r.guid);
				if (!byGuid.empty()) {
					if (byGuid != modelSource)
						std::cerr << "[References] guid "
							<< r.guid << " -> '" << byGuid
							<< "' (recorded '" << modelSource
							<< "')\n";
					modelSource = byGuid;
				}
				else {
					std::cerr << "[References] guid " << r.guid
						<< " unknown (asset db closed / sidecar "
						"missing?) — using recorded path '"
						<< modelSource << "'\n";
				}
			}
			if (IsTourSource(modelSource)) {
				std::string tourResolved;
				if (vfs) {
					std::string p = smvk::NormalizeAssetUri(
						smvk::StringViewT(modelSource)).Str();
					if (!vfs->Exists(p) && !cur.baseDir.empty())
						p = smvk::vfs::Join(cur.baseDir, modelSource);
					if (vfs->Exists(p))
						tourResolved = p;
				}
				else {
					// HOST resolution: RELATIVE sources try the CONTENT
					// ROOT first (the project owns the content tree;
					// tours record project-relative sources), then the
					// CWD, then owner-relative. The old as-is(CWD)-first
					// order silently bound the TOOL'S OWN synced
					// content/ copy (the build dir carries one) instead
					// of the project's tree.
					std::filesystem::path hp;
					std::error_code ec;
					if (!contentRoot.empty()
						&& std::filesystem::path(modelSource)
							.is_relative()) {
						const std::filesystem::path cr =
							std::filesystem::path(contentRoot)
							/ modelSource;
						if (std::filesystem::exists(cr, ec)) {
							hp = cr;
						}
					}
					if (hp.empty()) {
						hp = std::filesystem::path(modelSource);
						if (!std::filesystem::exists(hp, ec)
							&& !cur.baseDir.empty())
							hp = std::filesystem::path(cur.baseDir)
							/ modelSource;
					}
					if (std::filesystem::exists(hp, ec))
						tourResolved = hp.generic_string();
				}
				if (tourResolved.empty()) {
					const std::string m = "[References] cannot resolve tour '" +
						modelSource + "' (skipped)";
					std::cerr << m << "\n";
					issue(m);
					++warnings;
					continue;
				}
				std::string tourText;
				if (!ReadTourTextAt(tourResolved, vfs, tourText)) {
					const std::string m = "[References] cannot read tour '" +
						tourResolved + "' (skipped)";
					std::cerr << m << "\n";
					issue(m);
					++warnings;
					continue;
				}
				std::string tourErr;
				if (!smvk::tour::LoadTourFromString(tourText, refTour, tourErr)) {
					const std::string m = "[References] tour parse failed '" +
						tourResolved + "': " + tourErr + " (skipped)";
					std::cerr << m << "\n";
					issue(m);
					++warnings;
					continue;
				}
				if (refTour.source.empty()) {
					const std::string m = "[References] tour '" + tourResolved +
						"' names no source scene (skipped)";
					std::cerr << m << "\n";
					issue(m);
					++warnings;
					continue;
				}
				haveRefTour = true;
				refTourDir = baseDirOf(tourResolved);
				modelSource = refTour.source;
			}
			std::string resolved;   // canonical path (host or vfs)
			std::string baseDir;   // source dir for ITS references
			bool ok = false;
			// THE MESH ARTIFACT (.geometry) — the ONLY model source kind
			// (the glTF privileges are gone): a reference resolves +
			// LOADS the preprocessed blobs here; the BUILD happens below
			// where the placeholder exists (pure preprocessed loading;
			// node ids are the derivation's = the import's, so
			// replays/overrides/cooks address the same nodes). A failed
			// artifact load is a LOUD skip — NEVER a glTF fallback: the
			// glTF is IMPORT-ONLY (the MeshImporter, asset_export.h
			// ImportMeshSource); re-import the source to regenerate the
			// artifacts. The pre-10/6 ".mesh" derived asset renamed to
			// ".geometry" (the data blob's name; ".mesh" is the higher
			// placeable asset's now) — legacy ".mesh" sources still
			// resolve here.
			smvk::assets::MeshAsset meshAsset;
			smvk::assets::AssetManifest meshManifest;
			bool meshLoaded = false;
			{
				const size_t dotGeo = modelSource.rfind(".geometry");
				const size_t dotMesh = modelSource.rfind(".mesh");
				const bool isGeometry = dotGeo != std::string::npos
					&& modelSource.size() - dotGeo == 9;
				const bool isLegacyMesh = !isGeometry
					&& dotMesh != std::string::npos
					&& modelSource.size() - dotMesh == 5;
				const size_t dotGltf = modelSource.rfind(".gltf");
				const size_t dotGlb = modelSource.rfind(".glb");
				const bool isGltf = (dotGltf != std::string::npos
						&& modelSource.size() - dotGltf == 5)
					|| (dotGlb != std::string::npos
						&& modelSource.size() - dotGlb == 4);
				if (isGltf) {
					// glTF sources are IMPORT-ONLY: the editor's open of
					// a .gltf routes through the MeshImporter (derive
					// beside the source) and records the .geometry; the
					// build processes (this resolution, the cooker, the
					// GiBaker) read artifacts ONLY. A tour still naming a
					// .gltf is pre-artifact — re-open the source in the
					// editor (derive + re-save) or run ImportMeshSource.
					const std::string m = "[References] glTF sources are "
						"import-only (the MeshImporter): '" + modelSource
						+ "' — reference the derived .geometry (re-open "
						"the source in the editor / import it) (skipped)";
					std::cerr << m << "\n";
					issue(m);
					++warnings;
					continue;
				}
				if (isGeometry || isLegacyMesh) {
					// Resolve + read the .mesh (vfs or host).
					std::string meshResolved;
					std::optional<std::vector<uint8_t>> meshBytes;
					if (vfs) {
						std::string p = smvk::NormalizeAssetUri(
							smvk::StringViewT(modelSource)).Str();
						if (!vfs->Exists(p)
							&& !cur.baseDir.empty())
							p = smvk::vfs::Join(cur.baseDir,
								modelSource);
						if (vfs->Exists(p)) {
							meshResolved = p;
							meshBytes = vfs->TryRead(p);
						}
					}
				else {
					// HOST resolution (content-root first for RELATIVE
					// sources — see the tour branch above for the why:
					// the project tree owns "content/..." sources; the
					// CWD-first order bound the tool's own synced
					// content/ copy and shipped a stale, meshlet-less
					// scene in every tour pak).
					std::filesystem::path hp;
					std::error_code ec;
					if (!contentRoot.empty()
						&& std::filesystem::path(modelSource)
							.is_relative()) {
						const std::filesystem::path cr =
							std::filesystem::path(contentRoot)
							/ modelSource;
						if (std::filesystem::exists(cr, ec))
							hp = cr;
					}
					if (hp.empty()) {
						hp = std::filesystem::path(modelSource);
						if (!std::filesystem::exists(hp, ec)
							&& !cur.baseDir.empty())
							hp = std::filesystem::path(cur.baseDir)
							/ modelSource;
					}
					if (std::filesystem::exists(hp, ec)) {
						meshResolved = hp.generic_string();
						std::ifstream in(hp,
							std::ios::binary);
						if (in)
							meshBytes = std::vector<uint8_t>(
								(std::istreambuf_iterator<char>(
									in)),
								std::istreambuf_iterator<char>());
					}
				}
					if (meshBytes && !meshBytes->empty()) {
						std::string meshErr;
						if (smvk::assets::LoadMeshAssetBytes(
								meshBytes->data(), meshBytes->size(),
								meshAsset, meshErr)) {
							meshLoaded = true;
							resolved = meshResolved;
							baseDir = baseDirOf(meshResolved);
							ok = true;
						}
					}
					if (!meshLoaded) {
						// LOUD skip — never a glTF fallback (the glTF is
						// import-only; see the header note above): a
						// missing/corrupt .geometry means the source was
						// never imported (or the derivation half-wrote):
						// re-import it (the editor's open / the asset
						// browser / ImportMeshSource).
						const std::string m = "[References] .geometry "
							"artifact '" + meshResolved + "' missing or "
							"unparseable — re-import the source (the "
							"MeshImporter) (skipped)";
						std::cerr << m << "\n";
						issue(m);
						++warnings;
						continue;
					}
				}
				else {
					// Not a .geometry/.mesh and not a .gltf (checked
					// above): an unknown source kind — loud skip.
					const std::string m = "[References] unknown source '"
						+ modelSource + "' (want the derived .geometry "
						"artifact) (skipped)";
					std::cerr << m << "\n";
					issue(m);
					++warnings;
					continue;
				}
			}
			if (!ok) {
				const std::string m = "[References] cannot resolve '" +
					modelSource + "' (skipped)";
				std::cerr << m << "\n";
				issue(m);
				++warnings;
				continue;
			}
			// Cycle guard: never re-enter a source already on this chain.
			// (NormalizeAssetUri is empty for absolute host paths — fall
			// back to the resolved path so the chain key stays unique.)
			const std::string norm = [&]() {
				const smvk::StringViewT n = smvk::NormalizeAssetUri(
					smvk::StringViewT(resolved));
				return (!n.empty()) ? n.Str() : resolved;
			}();
			if (std::find(cur.chain.begin(), cur.chain.end(), norm) !=
				cur.chain.end()) {
				std::cerr << "[References] cycle '" << resolved <<
					"' (skipped)\n";
				++warnings;
				continue;
			}
			// Record the canonical model path the raw source bound to.
			// (The raw tour string shows in the panel; this is the
			// resolved sibling for path-solver/symlink users.)
			noteResolved(resolved);
			// ROOT references (the scene's own source, reference #0 in
			// the definition doc): inlined DIRECTLY under the scene
			// root — no placeholder, no namespace (Maya: the master
			// file's own contents carry no prefix). Cluster ids match
			// a direct source import exactly, so editor and cook
			// builds are identical by construction.
			const bool isRoot = r.root;
			// Placeholder group node under the recorded parent (level-space
			// parent resolved to owner space above). Root references
			// anchor at the scene root instead (ph stays 0 — the
			// subtree offset math below keeps working).
			const int parentNode = isRoot ? 0
				: ((pRaw < uint64_t(cluster.scene.hierarchy.size()))
					? int(pRaw) : 0);
			const int level = cluster.scene.hierarchy[size_t(parentNode)].level + 1;
			int ph = 0; // root anchor: the scene root node
			std::string name = r.name.empty()
				? std::filesystem::path(resolved).stem().string()
				: r.name;
			if (!isRoot) {
				ph = addNode(cluster.scene, parentNode, level);
				setNodeName(cluster.scene, ph, "REF " + name);
				if (r.hasTranslate) {
					glm::mat4 m(1.0f);
					m[3] = glm::vec4(r.translate, 1.0f);
					cluster.scene.localTransform[size_t(ph)] = m;
				}
			}
			if (cur.depth == 0 && outPlaceholderNodes)
				outPlaceholderNodes->push_back(isRoot ? -1 : ph);
			// Pool bases for .tour replay (captured before the subtree
			// lands; the subtree's own tour ids translate by these).
			const uint32_t matBaseBefore = cluster.materials.numObjects();
			// Import the referenced scene beneath the placeholder:
			// the ARTIFACT (.geometry) build — the ONLY path (the
			// glTF import fallback is gone: glTF is import-only, see
			// the resolution head). A build failure is a LOUD skip.
			bool builtFromMesh = false;
			if (meshLoaded) {
				// The manifest (the .material file table) sits beside
				// the sibling glTF: <stem>.gltf.meta.
				{
					std::string stem = fileNameOf(resolved);
					// Strip the data-blob extension (".geometry"; the
					// legacy ".mesh" too) to the derivation stem.
					const size_t mdot = stem.rfind(".geometry");
					const size_t mdotLegacy = stem.rfind(".mesh");
					if (mdot != std::string::npos)
						stem = stem.substr(0, mdot);
					else if (mdotLegacy != std::string::npos)
						stem = stem.substr(0, mdotLegacy);
					const std::string meshDir = baseDirOf(resolved);
					const std::string metaRel =
						stem + ".gltf.meta";
					std::optional<std::vector<uint8_t>> metaBytes;
					if (vfs) {
						const std::string mp =
							smvk::vfs::Join(meshDir, metaRel);
						if (vfs->Exists(mp))
							metaBytes = vfs->TryRead(mp);
					}
					else {
						std::ifstream in(
							std::filesystem::path(meshDir) / metaRel,
							std::ios::binary);
						if (in)
							metaBytes = std::vector<uint8_t>(
								(std::istreambuf_iterator<char>(
									in)),
								std::istreambuf_iterator<char>());
					}
					if (metaBytes && !metaBytes->empty()) {
						std::string merr;
						smvk::assets::LoadAssetManifestText(
							std::string(metaBytes->begin(),
								metaBytes->end()),
							meshManifest, merr);
					}
				}
				// readSibling: .material files beside the .mesh (host
				// or vfs); the texture source mirrors the importer's.
				auto readSibling =
					[&](const std::string& file)
					-> std::optional<std::vector<uint8_t>> {
					if (vfs) {
						const std::string p =
							smvk::vfs::Join(
								smvk::vfs::BaseDir(resolved), file);
						if (vfs->Exists(p))
							return vfs->TryRead(p);
						return std::nullopt;
					}
					std::ifstream in(
						std::filesystem::path(resolved)
						.parent_path() / file, std::ios::binary);
					if (!in)
						return std::nullopt;
					return std::vector<uint8_t>(
						(std::istreambuf_iterator<char>(in)),
						std::istreambuf_iterator<char>());
				};
				std::shared_ptr<GltfToSceneImporter::TextureSource>
					texSrc;
				if (vfs)
					texSrc = std::make_shared<VfsTextureSource>(
						vfs, baseDir);
				else
					texSrc = std::make_shared<HostTextureSource>(
						std::filesystem::path(baseDir));
			// MESH BASE URI: the tour's RECORDED source when it is
			// relative (the reference identity — matches the
			// positional artifact path's URIs exactly), else the
			// resolved host path. The absolute resolved path as base
			// kills the URI machinery (NormalizeAssetUri rejects
			// drive-letter absolutes — gltf.h's own warning: mesh URIs,
			// and with them the LOD chains + meshlet registrations the
			// pak serializer reads, silently drop) — the pool objects
			// kept their sets but the serializer found nothing and
			// shipped meshleted=0.
			const std::string meshBaseUri =
				std::filesystem::path(modelSource).is_relative()
				? modelSource : resolved;
			const size_t built = BuildFromMeshAsset(cluster,
				streamer, meshAsset, meshManifest, meshBaseUri,
				ph, texSrc, autoKtx, artifactCache, readSibling);
			if (built > 0) {
				builtFromMesh = true;
				std::cerr << "[References] built '" << resolved
					<< "' from the .geometry artifact (" << built
					<< " nodes, preprocessed)\n";
			}
		}
		if (!builtFromMesh) {
			// The ONLY path is the artifact build (above): a zero node
			// count means the .geometry parsed but built nothing
			// (degenerate geometry / empty pool / heap full) — LOUD,
			// never a glTF fallback (the glTF import here is GONE: the
			// MeshImporter owns glTF parsing, asset_export.cpp).
			const std::string m = "[References] .geometry artifact '"
				+ resolved + "' built 0 nodes — degenerate/empty "
				"(re-import the source, the MeshImporter) (skipped)";
			std::cerr << m << "\n";
			issue(m);
			++warnings;
			continue;
		}
			// Id offsets for .tour replay: the import above is
			// deterministic (same traversal as the referenced scene's own
			// import), so its cluster ids land at base + id; materials
			// append in first-use order the same way. The replay's
			// name verification warns if the bake ever drifts.
			const uint32_t subtreeBase = uint32_t(ph) + 1u;
			const uint32_t matBase = matBaseBefore;
			if (haveRefTour && !refTour.commands.empty()) {
				smvk::tour::Doc sub;
				sub.commands.reserve(refTour.commands.size());
				for (const auto& c : refTour.commands) {
					smvk::tour::Command t = c;
					const bool refAddr =
						(t.verb == smvk::tour::Verb::SetLightAttr)
						&& ((t.node & smvk::tour::kCreateLightRefBase) != 0);
					if (!refAddr)
						t.node += subtreeBase;
					if (t.verb == smvk::tour::Verb::Parent)
						t.node2 += subtreeBase;
					// NOTE: CreateLight.node2 is the shadow bit, not a
					// node — only .node (the parent) shifts.
					if (t.verb == smvk::tour::Verb::AssignMaterial
						|| t.verb == smvk::tour::Verb::SetMatAttr)
						t.material += matBase;
					sub.commands.push_back(std::move(t));
				}
				sub.overrides.reserve(refTour.overrides.size());
				for (const auto& o : refTour.overrides) {
					smvk::tour::OverrideRule rule = o;
					if (rule.attr == smvk::tour::OverrideAttr::Material)
						rule.material += matBase;
					sub.overrides.push_back(std::move(rule));
				}
				// Non-strict replay: a failing command is reported and
				// skipped, never aborting the rest (a partially-broken
				// reference keeps its geometry + every valid edit).
				std::vector<std::string> replayWarns;
				std::string replayErr;
				if (!smvk::tour::ApplyTourToCluster(cluster, sub, &replayWarns,
						&replayErr, false)) {
					const std::string m = "[References] tour replay failed for '" +
						resolved + "': " + replayErr + " (geometry kept)";
					std::cerr << m << "\n";
					issue(m);
					++warnings;
				}
			for (const std::string& w : replayWarns) {
				const std::string m = "[References] '" + resolved + "': " + w;
				std::cerr << m << "\n";
				issue(m);
				++warnings;
			}
		}
		// Reference namespace (Maya): every node below the placeholder
		// takes '<chain><refName>:' so the master's overrides can target
		// a single reference with one name regex ('ns:.*'). The ROOT
		// namespace is the master tour itself (its own nodes carry no
		// prefix — root references skip this pass entirely). Nested
		// references queue with the extended chain, so their nodes read
		// 'outer:inner:name'. Runs AFTER the source tour's replay: its
		// verify-names and its own wide overrides matched the
		// un-prefixed names.
		if (!isRoot) {
			const std::string ns = cur.nsPrefix + name + ":";
			std::vector<uint32_t> stack;
			for (int ch = cluster.scene.hierarchy[size_t(ph)].firstChild;
				ch >= 0;
				ch = cluster.scene.hierarchy[size_t(ch)].nextSibling)
				stack.push_back(uint32_t(ch));
			while (!stack.empty()) {
				const uint32_t v = stack.back();
				stack.pop_back();
				const std::string nm = getNodeName(cluster.scene, int(v));
				if (!nm.empty())
					setNodeName(cluster.scene, int(v), ns + nm);
				for (int ch = cluster.scene.hierarchy[v].firstChild;
					ch >= 0;
					ch = cluster.scene.hierarchy[size_t(ch)].nextSibling)
					stack.push_back(uint32_t(ch));
			}
			// NOTE: the disabled flag NEVER touches the import here —
			// the runtime world build excludes hidden nodes from its
			// draw list at resolve time, so an import-side hide would
			// permanently drop the reference's meshes (invisible AND
			// un-restorable on re-enable). The EDITOR applies the
			// disabled-hide on the resolved world instead (its passes
			// filter visibility per frame, so toggling is instant).
		}
		++applied;
			std::cout << "[References] inlined '" << resolved << "' "
				<< (isRoot
					? "as the scene source (root)"
					: ("under node " + std::to_string(parentNode)
						+ " (placeholder " + std::to_string(ph) + ")"))
				<< "\n";
			// Nesting: follow further references with the subtree's
			// offset. .tour sources carry their own doc; .gltf sources
			// use the sibling <stem>.tour (existing behavior — its
			// commands stay ignored, only its references are followed).
			// ROOT references skip the sibling lookup: their sibling
			// tour IS the master doc being applied (self-cycle).
			const smvk::tour::Doc* nestedDoc = nullptr;
			smvk::tour::Doc siblingTour;
			if (haveRefTour) {
				nestedDoc = &refTour;
			}
			else if (!isRoot) {
				const std::string tourPath = ReferenceTourPathFor(
					std::filesystem::path(resolved));
				std::string tourText;
				if (ReadTourTextAt(tourPath, vfs, tourText)) {
					std::string err;
					if (smvk::tour::LoadTourFromString(tourText, siblingTour,
							err)
						&& !siblingTour.references.empty()) {
						nestedDoc = &siblingTour;
					}
					else if (!err.empty()) {
						std::cerr << "[References] sibling tour parse failed '"
							<< tourPath << "': " << err << "\n";
					}
				}
			}
		if (nestedDoc && !nestedDoc->references.empty()) {
			Level next{};
			next.doc = *nestedDoc;
			next.baseDir = baseDir;
			next.chain = cur.chain;
			next.chain.push_back(norm);
			// Root references never extend the namespace chain (the
			// master's own contents stay unprefixed at every depth).
			next.nsPrefix = isRoot ? cur.nsPrefix
				: cur.nsPrefix + name + ":";
			next.depth = cur.depth + 1;
			next.nodeBase = subtreeBase;
			queue.push_back(std::move(next));
		}
		}
	}
	if (applied != 0) {
		RebuildStaticCache(cluster.scene);
		markAsChanged(cluster.scene, 0);
		recalculateGlobalTransforms(cluster.scene);
		if (cluster.ctx_)
			cluster.ctx_->FlushUploads();
	}
	return applied;
}
