// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/cluster_serializer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/asset_registry.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/texture_streamer.h>
#include <Engine/Utils/ktx_error_texture.h>
#include <Engine/Utils/texture_loader.h>
#include <Engine/Utils/thread_pool.h>

#include "ktx.h"

namespace smvk {
namespace {

// Little-endian binary helpers (pak format is LE; hosts are LE — see pak.cpp).
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + 4);
}
void PutI32(std::vector<uint8_t>& out, int32_t v) {
	PutU32(out, static_cast<uint32_t>(v));
}
void PutU64(std::vector<uint8_t>& out, uint64_t v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + 8);
}
void PutF32(std::vector<uint8_t>& out, float v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + 4);
}
void PutStr(std::vector<uint8_t>& out, StringViewT s) {
	PutU32(out, static_cast<uint32_t>(s.size()));
	out.insert(out.end(), s.begin(), s.end());
}
void PutVec3(std::vector<uint8_t>& out, const glm::vec3& v) {
	PutF32(out, v.x);
	PutF32(out, v.y);
	PutF32(out, v.z);
}
size_t AlignUp16(size_t v) {
	return (v + 15) & ~size_t(15);
}

// One texture's v4 cook job (thread-safe: ktx objects + digest + blob
// build are all local). Returns the payload blob + whether it carries a
// digest (digested payloads store uncompressed for tail direct
// addressing). On digest failure the payload stays legacy (the load path
// fails it per-texture, same as today); `error` is only set for hard
// cook failures (missing/invalid payload metadata).
struct TexCookJob {
	std::string uri;
	std::string filePart;
	TextureSamplerDesc samp;
	const std::vector<uint8_t>* src = nullptr;
	// A material's normal slot is DATA, never color: the cook forces the
	// linear transfer even when the source KTX2 claims sRGB (a toktx run
	// without --normal_mode defaults to sRGB; sRGB-decoded normal XY
	// warps the shading). See CookTexturePayload.
	bool forceLinear = false;
};
struct TexCookResult {
	std::string uri;
	std::string filePart;
	std::vector<uint8_t> blob;
	bool digested = false;
	size_t srcBytes = 0;
	std::string error;
};

// Alpha-use probe for 4-component Basis sources (a second parse: a
// ktxTexture2 can transcode only once). Decodes the top level to RGBA32
// and walks a subsampled grid: all-opaque alpha -> BC1 is safe (the
// transcoder's BC1_RGB is opaque-only); any used alpha -> BC3 keeps it
// (BC1-quality RGB + BC4-quality A: the color+AO-in-alpha packing).
// Best-effort: parse/transcode failure reads conservative (alpha used).
bool SourceAlphaAllOpaque(const std::vector<uint8_t>& src) {
	ktxTexture2* tex = nullptr;
	if (ktxTexture2_CreateFromMemory(
			static_cast<const ktx_uint8_t*>(src.data()),
			static_cast<ktx_size_t>(src.size()), 0, &tex) != KTX_SUCCESS
		|| !tex)
		return true; // unparseable here: caller re-parses for real work
	if (!ktxTexture2_NeedsTranscoding(tex)) {
		ktxTexture2_Destroy(tex);
		return true; // not Basis: the cook transcode path is skipped anyway
	}
	const KTX_error_code tc = ktxTexture2_TranscodeBasis(tex, KTX_TTF_RGBA32, 0);
	if (tc != KTX_SUCCESS) {
		ktxTexture2_Destroy(tex);
		return false;
	}
	ktx_size_t off = 0;
	if (ktxTexture2_GetImageOffset(tex, 0, 0, 0, &off) != KTX_SUCCESS) {
		ktxTexture2_Destroy(tex);
		return false;
	}
	const uint8_t* data = ktxTexture_GetData(ktxTexture(tex));
	const uint32_t w = std::max(1u, tex->baseWidth);
	const uint32_t h = std::max(1u, tex->baseHeight);
	// Subsampled grid over the TOP level (dominates the alpha character);
	// <250 (not just !=255) tolerates UASTC round-trip noise on opaque fills.
	constexpr uint32_t kStride = 16;
	bool allOpaque = data != nullptr;
	if (allOpaque)
		for (uint32_t y = 0; y < h && allOpaque; y += kStride)
			for (uint32_t x = 0; x < w; x += kStride)
				if (data[off + (size_t(y) * w + x) * 4 + 3] < 250) {
					allOpaque = false;
					break;
				}
	ktxTexture2_Destroy(tex);
	return allOpaque;
}

// Cook-time BCn selection for Basis-family (UASTC/ETC1S) sources — the
// per-semantic target table (not blanket BC7):
//   1 component  -> BC4_R   (single-channel data/AO)
//   2 components  -> BC5_RG  (normal XY pairs)
//   3 components  -> BC1_RGB (opaque RGB)
//   4 components  -> alpha probe: all-opaque -> BC1_RGB, used alpha ->
//                    BC3_RGBA
// sRGB never lands on BC4/BC5 (no sRGB variants of those formats) — a
// 1/2-component sRGB source falls back to BC3_RGBA. The default is BC7
// (unreachable in practice via the component ladder, kept as the safe
// catch-all for 4-component alpha probes that cannot decide).
ktx_transcode_fmt_e CookBcnTarget(const std::vector<uint8_t>& src,
	uint32_t comps, bool srgb) {
	if (comps <= 1)
		return srgb ? KTX_TTF_BC3_RGBA : KTX_TTF_BC4_R;
	if (comps == 2)
		return srgb ? KTX_TTF_BC3_RGBA : KTX_TTF_BC5_RG;
	if (comps == 3)
		return KTX_TTF_BC1_RGB;
	return SourceAlphaAllOpaque(src) ? KTX_TTF_BC1_RGB : KTX_TTF_BC3_RGBA;
}

	TexCookResult CookTexturePayload(const TexCookJob& job) {
	TexCookResult r;
	r.uri = job.uri;
	r.filePart = job.filePart;
	const std::vector<uint8_t>& src = *job.src;
	r.srcBytes = src.size();
	// v4 tail digest + cook-time BCn: parse the ktx (cheap at cook), stamp
	// the resident tail split, and — for Basis-family sources (foreign
	// toktx sidecars trusted at import) — transcode ONCE here to the
	// per-semantic BCn target and embed the transcoded bytes. The pak then
	// always ships GPU-ready payloads (the documented v4 contract, see
	// pak.h's ParseTexturePayload note: "the embedded ktx is always
	// GPU-ready (transcoded at cook)") — the runtime's retarget path
	// (LOD/texture promotion) never pays a synchronous transcode on the
	// render thread, and first load skips it too. Transcode/write-back
	// failure embeds the solid-pink ERROR TEXTURE (structure-mirrored,
	// GPU-ready — WritePinkKtx2Blob) instead of the raw Basis bytes: a
	// streaming pak must never ship a Basis payload (the runtime retarget
	// REFUSES those — pink + pinned, never a hitch and never fatal). The
	// pink surface is the visible "this texture failed to cook" flag;
	// recook with a fixed source. Only a pink-mint failure falls all the
	// way back to the raw embed (the runtime's pink refusal catches it).
	TextureStreamMeta meta{};
	std::vector<uint8_t> digestBytes;
	bool haveDigest = false;
	// Cooked GPU-ready bytes (transcoded BCn or the pink error texture):
	// must outlive the r.blob build below, so it lives here — embedBytes
	// points into it.
	std::vector<uint8_t> cooked;
	const uint8_t* embedBytes = src.data();
	size_t embedSize = src.size();
	ktxTexture2* ktxTex = nullptr;
	if (ktxTexture2_CreateFromMemory(
			static_cast<const ktx_uint8_t*>(src.data()),
			static_cast<ktx_size_t>(src.size()),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
			&ktxTex) != KTX_SUCCESS || !ktxTex
		|| ktxTex->numDimensions < 1 || ktxTex->numDimensions > 3) {
		if (ktxTex)
			ktxTexture2_Destroy(ktxTex);
	}
	else {
		// Cook-time BCn (Basis-family sources only; native BCn/uncompressed
		// payloads pass through untouched).
		if (ktxTexture2_NeedsTranscoding(ktxTex)) {
			const uint32_t comps = ktxTexture2_GetNumComponents(ktxTex);
			bool srgb =
				ktxTexture2_GetTransferFunction_e(ktxTex) == KHR_DF_TRANSFER_SRGB;
			if (job.forceLinear && srgb) {
				// Normal maps are data (X/Y are directions, not color).
				// An sRGB DFD here is an authoring slip (toktx without
				// --normal_mode); re-stamp linear so the BCn target and
				// the runtime view both stop gamma-decoding it.
				std::fprintf(stderr,
					"[cook] texture '%s': normal map source is sRGB — forcing linear (use toktx --normal_mode)\n",
					job.uri.c_str());
				ktxTexture2_SetOETF(ktxTex, KHR_DF_TRANSFER_LINEAR);
				srgb = false;
			}
			ktx_transcode_fmt_e target = CookBcnTarget(src, comps, srgb);
			// Do NOT force BC5 for 3-component normal KTX2s. These files are
			// plain UASTC RGB (DFD channelType RRRG is only written for the
			// 2-component X+Y --normal_mode layout); KTX-Software's BC5
			// transcode of a plain RGB UASTC writes the R axis but leaves the
			// green block constant 255, which rotates every mapped normal and
			// visibly darkens the surface. BC1 keeps both axes (5:6:5) and is
			// the correct target until the sources are re-encoded as X+Y.
			if (ktxTexture2_TranscodeBasis(ktxTex, target, 0) != KTX_SUCCESS) {
				std::fprintf(stderr,
					"[cook] texture '%s': BCn transcode failed (%u comps%s, vkFormat %u) — embedding the pink error texture (recook with a fixed source)\n",
					job.uri.c_str(), comps, srgb ? " sRGB" : "", ktxTex->vkFormat);
				if (!WritePinkKtx2Blob(*ktxTex, cooked)) {
					std::fprintf(stderr,
						"[cook] texture '%s': pink error mint ALSO failed — embedding raw (the runtime retarget will refuse it and pink-pin the entry)\n",
						job.uri.c_str());
				}
				else {
					embedBytes = cooked.data();
					embedSize = cooked.size();
					r.srcBytes = cooked.size();
				}
			}
			else {
				ktx_uint8_t* out = nullptr;
				ktx_size_t outSize = 0;
				if (ktxTexture2_WriteToMemory(ktxTex, &out, &outSize)
						== KTX_SUCCESS && out && outSize > 0) {
					cooked.assign(out, out + outSize);
					free(out); // KTX-Software documents free() for this buffer
					std::fprintf(stderr,
						"[cook] texture '%s': %u comps%s -> BCn (vkFormat %u, %.2f -> %.2f MB)\n",
						job.uri.c_str(), comps, srgb ? " sRGB" : "",
						ktxTex->vkFormat, src.size() / (1024.0 * 1024.0),
						cooked.size() / (1024.0 * 1024.0));
					embedBytes = cooked.data();
					embedSize = cooked.size();
					r.srcBytes = cooked.size();
				}
				else {
					free(out); // defensive: partial allocation
					std::fprintf(stderr,
						"[cook] texture '%s': BCn write-back failed — embedding the pink error texture (recook with a fixed source)\n",
						job.uri.c_str());
					if (!WritePinkKtx2Blob(*ktxTex, cooked)) {
						std::fprintf(stderr,
							"[cook] texture '%s': pink error mint ALSO failed — embedding raw (the runtime retarget will refuse it and pink-pin the entry)\n",
							job.uri.c_str());
					}
					else {
						embedBytes = cooked.data();
						embedSize = cooked.size();
						r.srcBytes = cooked.size();
					}
				}
			}
		}
		meta.tailFirstMip = TextureStreamer::InitialResidentBase(
			std::max(1u, ktxTex->numLevels),
			std::max(std::max(1u, ktxTex->baseWidth),
				ktxTex->numDimensions == 1 ? 1u
					: std::max(1u, ktxTex->baseHeight)),
			/*minResidentMips=*/2, /*dimCap=*/1024);
		ktxTexture2_Destroy(ktxTex);
		haveDigest = BuildTextureDigest(meta, digestBytes);
	}
	r.blob = haveDigest
		? BuildTexturePayloadV4(job.uri, job.samp.magFilter, job.samp.minFilter,
			job.samp.wrapS, job.samp.wrapT,
			std::span<const uint8_t>(embedBytes, embedSize),
			std::span<const uint8_t>(digestBytes))
		: BuildTexturePayload(job.uri, job.samp.magFilter, job.samp.minFilter,
			job.samp.wrapS, job.samp.wrapT,
			std::span<const uint8_t>(embedBytes, embedSize));
	r.digested = haveDigest;
	return r;
}
// Authored-environment record (see SceneEnv): 16 floats in fixed order +
// the skybox URI string. Shared by the resolved look, the default block
// and the per-node blocks.
void PutSceneEnv(std::vector<uint8_t>& out, const SceneEnv& e) {
	PutVec3(out, e.skyColor);
	PutF32(out, e.skyIntensity);
	PutVec3(out, e.sunColor);
	PutF32(out, e.sunAzimuth);
	PutF32(out, e.sunElevation);
	PutF32(out, e.sunIntensity);
	PutF32(out, e.exposure);
	PutF32(out, e.fogDensity);
	PutVec3(out, e.fogColor);
	PutF32(out, e.volumetrics);
	PutStr(out, e.skyboxUri);
	// Fog quality tiers + the volumetric fog settings (SceneEnv — the
	// App's pass seeds from the resolved env). Appended AFTER the v10
	// record: older paks fail the reader loudly (the sanctioned break —
	// old format, no conversions).
	PutU32(out, uint32_t(e.fogQuality));
	PutU32(out, e.volumetricSteps);
	PutF32(out, e.volumetricMaxDistance);
	PutF32(out, e.volumetricDensity);
	PutF32(out, e.volumetricAnisotropy);
	PutF32(out, e.volumetricSun);
	PutF32(out, e.volumetricGi);
	PutU32(out, e.volumetricUseGi ? 1u : 0u);
	PutU32(out, e.volumetricUseSun ? 1u : 0u);
	PutF32(out, e.volumetricJitter);
	PutF32(out, e.volumetricHeightFalloff);
	PutF32(out, e.volumetricHeightBase);
	// Postfx look: SPLIT OUT — the env record ends at the volumetric
	// tail + zonePriority; the look rides its own scene-level record
	// (PutSceneRenderSettings below). Older paks fail the reader loudly
	// (the sanctioned break — old format, no conversions).
	// Zone-entity tail (the zonePriority break): older paks fail the
	// reader loudly (sanctioned — no conversions).
	PutF32(out, e.zonePriority);
}

// Render-settings record (see SceneRenderSettings): the global postfx
// look — 28 words, written ONCE per scene (never per env block).
void PutSceneRenderSettings(std::vector<uint8_t>& out,
	const SceneRenderSettings& r) {
	PutU32(out, r.bloomEnabled ? 1u : 0u);
	PutF32(out, r.bloomIntensity);
	PutF32(out, r.bloomThreshold);
	PutU32(out, r.tonemapMode);
	PutU32(out, r.autoExposure ? 1u : 0u);
	PutF32(out, r.manualExposure);
	PutF32(out, r.exposureEvBias);
	PutF32(out, r.exposureSpeedToDark);
	PutF32(out, r.exposureSpeedToBright);
	PutF32(out, r.exposureMin);
	PutF32(out, r.exposureMax);
	PutU32(out, uint32_t(r.aoMode));
	PutF32(out, r.aoRadius);
	PutF32(out, r.aoBias);
	PutF32(out, r.aoIntensity);
	PutF32(out, r.aoMaxDistance);
	PutU32(out, uint32_t(r.aoSsaoSamples));
	PutF32(out, r.aoBlurSharpness);
	PutU32(out, uint32_t(r.aoSaoSamples));
	PutU32(out, uint32_t(r.aoSaoSpiralTurns));
	PutU32(out, r.ssrEnabled ? 1u : 0u);
	PutF32(out, r.ssrIntensity);
	PutU32(out, r.motionBlurEnabled ? 1u : 0u);
	PutU32(out, r.dofEnabled ? 1u : 0u);
	PutU32(out, uint32_t(r.aaMode));
	PutU32(out, r.cubemapEnabled ? 1u : 0u);
	PutF32(out, r.cubemapIntensity);
}

struct Cursor {
	std::span<const uint8_t> img;
	size_t off = 0;
	bool ok = true;
	uint32_t u32() {
		uint32_t v = 0;
		if (off + 4 > img.size())
			ok = false;
		else {
			memcpy(&v, img.data() + off, 4);
			off += 4;
		}
		return v;
	}
	int32_t i32() { return static_cast<int32_t>(u32()); }
	uint64_t u64() {
		uint64_t v = 0;
		if (off + 8 > img.size())
			ok = false;
		else {
			memcpy(&v, img.data() + off, 8);
			off += 8;
		}
		return v;
	}
	float f32() {
		float v = 0.0f;
		if (off + 4 > img.size())
			ok = false;
		else {
			memcpy(&v, img.data() + off, 4);
			off += 4;
		}
		return v;
	}
	std::string str() {
		const uint32_t n = u32();
		std::string s;
		if (!ok)
			return s;
		if (n > 16 * 1024 * 1024 || off + n > img.size()) {
			ok = false;
			return s;
		}
		s.assign(reinterpret_cast<const char*>(img.data() + off), n);
		off += n;
		return s;
	}
	glm::vec3 vec3() {
		const float x = f32(), y = f32(), z = f32();
		return glm::vec3(x, y, z);
	}
	// Authored-environment record (mirrors PutSceneEnv): 16 floats in
	// fixed order + the skybox URI. Sets ok=false on truncation (the
	// caller keeps defaults).
	SceneEnv sceneEnv() {
		SceneEnv e{};
		e.skyColor = vec3();
		e.skyIntensity = f32();
		e.sunColor = vec3();
		e.sunAzimuth = f32();
		e.sunElevation = f32();
		e.sunIntensity = f32();
		e.exposure = f32();
		e.fogDensity = f32();
		e.fogColor = vec3();
		e.volumetrics = f32();
		e.skyboxUri = str();
		// Fog quality tiers + the volumetric fog settings (mirrors
		// PutSceneEnv's appended tail; older blobs truncate here — the
		// scene-env section fails loudly per the sanctioned
		// no-conversions break).
		e.fogQuality = int(u32());
		e.volumetricSteps = u32();
		e.volumetricMaxDistance = f32();
		e.volumetricDensity = f32();
		e.volumetricAnisotropy = f32();
		e.volumetricSun = f32();
		e.volumetricGi = f32();
		e.volumetricUseGi = u32() != 0;
		e.volumetricUseSun = u32() != 0;
		e.volumetricJitter = f32();
		e.volumetricHeightFalloff = f32();
		e.volumetricHeightBase = f32();
		// Postfx look: SPLIT OUT — the env record ends at zonePriority;
		// the look reads from the scene-level render-settings record
		// (sceneRenderSettings below). Older blobs truncate here — the
		// scene-env section fails loudly per the sanctioned
		// no-conversions break.
		e.zonePriority = f32();
		return e;
	}
	// Render-settings record (mirrors PutSceneRenderSettings): the
	// global postfx look. Truncation keeps defaults (ok=false).
	SceneRenderSettings sceneRenderSettings() {
		SceneRenderSettings r{};
		r.bloomEnabled = u32() != 0;
		r.bloomIntensity = f32();
		r.bloomThreshold = f32();
		r.tonemapMode = u32();
		r.autoExposure = u32() != 0;
		r.manualExposure = f32();
		r.exposureEvBias = f32();
		r.exposureSpeedToDark = f32();
		r.exposureSpeedToBright = f32();
		r.exposureMin = f32();
		r.exposureMax = f32();
		r.aoMode = int(u32());
		r.aoRadius = f32();
		r.aoBias = f32();
		r.aoIntensity = f32();
		r.aoMaxDistance = f32();
		r.aoSsaoSamples = int(u32());
		r.aoBlurSharpness = f32();
		r.aoSaoSamples = int(u32());
		r.aoSaoSpiralTurns = int(u32());
		r.ssrEnabled = u32() != 0;
		r.ssrIntensity = f32();
		r.motionBlurEnabled = u32() != 0;
		r.dofEnabled = u32() != 0;
		r.aaMode = int(u32());
		r.cubemapEnabled = u32() != 0;
		r.cubemapIntensity = f32();
		return r;
	}
};

float SphereRadiusForAabb(const glm::vec3& mn, const glm::vec3& mx) {
	return glm::length(mx - mn) * 0.5f;
}

// Scene blob TOC path: index 0 keeps the historic path so single-scene
// paks are byte-stable; further scenes append their index.
std::string SceneTocPath(size_t index, bool multi) {
	if (!multi || index == 0)
		return "<cluster-local>#scene";
	return "<cluster-local>#scene." + std::to_string(index);
}

// Index encoded in a scene TOC path ("#scene" -> 0, "#scene.N" -> N;
// anything else sorts last and fails loudly on load, never silently).
uint32_t SceneBlobIndex(const std::string& tocPath) {
	const size_t hash = tocPath.find_last_of('#');
	const std::string leaf = (hash == std::string::npos)
		? tocPath : tocPath.substr(hash + 1);
	if (leaf == "scene")
		return 0;
	if (leaf.rfind("scene.", 0) == 0) {
		uint32_t n = 0;
		bool digits = false;
		for (size_t i = 6; i < leaf.size(); ++i) {
			if (leaf[i] < '0' || leaf[i] > '9')
				return 0xFFFFFFFFu;
			digits = true;
			n = n * 10 + uint32_t(leaf[i] - '0');
		}
		return digits ? n : 0xFFFFFFFFu;
	}
	return 0xFFFFFFFFu;
}

} // namespace

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------

bool SerializeClusters(
	const std::vector<const Cluster*>& clusters,
	const std::vector<std::string>& sceneNames,
	PakWriter& writer,
	ClusterSerializeStats* stats,
	std::string* err) {
	auto fail = [&](const std::string& msg) {
		if (err)
			*err = msg;
		std::cerr << "SerializeCluster: " << msg << std::endl;
		return false;
	};
	if (clusters.empty())
		return fail("no clusters to serialize");
	// The trailing skin-section writer's static fallback (a null retained
	// upload writes count 0).
	static const std::vector<VertexSkin> kEmptySkinStream{};
	for (size_t ci = 0; ci < clusters.size(); ++ci)
		if (!clusters[ci])
			return fail("null cluster #" + std::to_string(ci));
	const bool multi = clusters.size() > 1;

	// Shared blob pool: asset URIs are stable across per-scene clusters
	// of one model, so each unique blob is written once and every scene
	// references the shared pak ids.
	std::unordered_map<std::string, uint32_t> meshIdByUri, matIdByUri,
		lightIdByUri, camIdByUri, decalIdByUri;
	std::unordered_set<std::string> texturesDone;
	uint64_t textureBytes = 0;
	struct LocalMaps {
		std::unordered_map<uint32_t, uint32_t> mesh, mat, light, camera,
			decal;
	};
	std::vector<LocalMaps> locals(clusters.size());

	for (size_t ci = 0; ci < clusters.size(); ++ci) {
	const Cluster& cluster = *clusters[ci];
	LocalMaps& local = locals[ci];

	// --- Meshes (pool index order via forEachHandle) ---------------------
	std::vector<ClusterStaticMeshHandle> meshHandles;
	{
		// const_cast: Pool::forEachHandle is non-const (historic). It does
		// not mutate when the functor only reads.
		const_cast<Cluster&>(cluster).staticMeshes.forEachHandle(
			[&](ClusterStaticMeshHandle h) { meshHandles.push_back(h); });
	}
	std::vector<std::string> meshUris;
	meshUris.reserve(meshHandles.size());
	for (uint32_t k = 0; k < meshHandles.size(); ++k) {
		const auto h = meshHandles[k];
		std::string uri = cluster.GetMeshUri(h).Str();
		if (uri.empty())
			uri = "<cluster-local>#mesh." + std::to_string(h.index());
		meshUris.push_back(uri);
		auto idIt = meshIdByUri.find(uri);
		if (idIt != meshIdByUri.end()) {
			local.mesh[h.index()] = idIt->second; // shared with a sibling scene
			continue;
		}
		const uint32_t pakId = static_cast<uint32_t>(meshIdByUri.size());
		meshIdByUri[uri] = pakId;
		local.mesh[h.index()] = pakId;

		const StaticMesh* mesh = const_cast<Cluster&>(cluster).staticMeshes.get(h);
		if (!mesh)
			return fail("stale mesh handle in pool");
		// Full vertex/index payloads only exist when the cluster was
		// imported with retained uploads (cooker path). Runtime clusters
		// hold GPU-side allocations only and cannot be serialized.
		const StaticMeshUpload* upload = cluster.GetRetainedUpload(h);
		if (!upload)
			return fail("no retained upload for mesh: " + uri +
				" (import with Cluster::SetRetainUploads(true) to cook paks)");

		std::vector<uint8_t> blob;
		PutStr(blob, uri);
		const Geometry& g = mesh->geometry;
		// NOTE: the GPU-side allocation (VMA offsets, firstIndex/baseVertex)
		// is session-local and never stored. The source vertex/index arrays,
		// AABB/sphere and topology are persisted; the upload path rebuilds
		// GPU surfaces on load.
		PutU32(blob, static_cast<uint32_t>(upload->vertices.size()));
		PutU32(blob, static_cast<uint32_t>(upload->indices.size()));
		PutU32(blob, static_cast<uint32_t>(upload->surfaces.size()));
		for (const Surface& s : upload->surfaces) {
			PutU32(blob, s.firstIndex);
			PutU32(blob, s.indexCount);
			PutI32(blob, s.baseVertex);
		}
		PutVec3(blob, upload->aabbMin);
		PutVec3(blob, upload->aabbMax);
		const float sphere = g.sphereRadius > 0.0f
			? g.sphereRadius
			: SphereRadiusForAabb(upload->aabbMin, upload->aabbMax);
		PutF32(blob, sphere);
		PutU32(blob, static_cast<uint32_t>(g.topology));
		// LOD marker (instrumentation only; renderer ignores it).
		LodInfo lod;
		if (auto hit = cluster.GetMeshLod(uri))
			lod = *hit;
		else {
			lod.groupId = h.index();
			lod.triCount = static_cast<uint32_t>(upload->indices.size() / 3);
		}
		PutU32(blob, lod.groupId);
		PutU32(blob, lod.lodIndex);
		PutU32(blob, lod.candidateCount);
		PutF32(blob, lod.simplifyError);
		PutF32(blob, lod.screenThreshold);
		PutU32(blob, lod.triCount);
		PutF32(blob, lod.acmr);
		// Lightmap chunk (cooked --lightmap; texCoord2 in the vertex
		// payload below carries the per-vertex CHART-LOCAL uvs + chartId,
		// and the per-chart atlas transforms follow here for the shader
		// unpack: atlasUv = chartUv * xform.xy + xform.zw).
		PutU32(blob, mesh->lmChunk);
		PutU32(blob, static_cast<uint32_t>(mesh->lmChartXforms.size()));
		for (const glm::vec4& xf : mesh->lmChartXforms) {
			PutF32(blob, xf.x);
			PutF32(blob, xf.y);
			PutF32(blob, xf.z);
			PutF32(blob, xf.w);
		}
		// Payload: PACKED SoA STREAMS (v4) — the exact GPU heap layout
		// (VertexPos[] + VertexShading[] + u32[]), one layout marker +
		// blob-relative offsets. Stored UNCOMPRESSED (AddBlob flag below)
		// so the payload bytes are direct pak-image subspans and the
		// loader stages them with pure offset math (no AoS parse, no
		// per-vertex conversion, BulkCopyBuffer content = the pak image).
		static_assert(sizeof(VertexPos) == 12, "VertexPos layout changed");
		static_assert(sizeof(VertexShading) == 24, "VertexShading layout changed");
		static_assert(sizeof(uint32_t) == 4, "index stream layout changed");
		std::vector<VertexPos> positions(upload->vertices.size());
		std::vector<VertexShading> shading(upload->vertices.size());
		for (size_t v = 0; v < upload->vertices.size(); ++v)
			PackStaticVertex(upload->vertices[v], positions[v], shading[v]);
		PutU32(blob, 1u); // layout: 1 = packed SoA (v4; v1-v3 blobs have no marker)
		// Align the stream start PAST the three u64 offsets still to be
		// written below (+24) — blob.size() here is header + marker only.
		const uint64_t posOff = AlignUp16(blob.size() + 24);
		PutU64(blob, posOff);
		const uint64_t shadeOff = posOff + positions.size() * sizeof(VertexPos);
		PutU64(blob, shadeOff);
		const uint64_t idxOff = shadeOff + shading.size() * sizeof(VertexShading);
		PutU64(blob, idxOff);
		{
			const size_t padA = size_t(posOff) - blob.size();
			blob.insert(blob.end(), padA, 0);
			blob.insert(blob.end(),
				reinterpret_cast<const uint8_t*>(positions.data()),
				reinterpret_cast<const uint8_t*>(positions.data()) + positions.size() * sizeof(VertexPos));
			blob.insert(blob.end(),
				reinterpret_cast<const uint8_t*>(shading.data()),
				reinterpret_cast<const uint8_t*>(shading.data()) + shading.size() * sizeof(VertexShading));
			blob.insert(blob.end(),
				reinterpret_cast<const uint8_t*>(upload->indices.data()),
				reinterpret_cast<const uint8_t*>(upload->indices.data()) +
					upload->indices.size() * sizeof(uint32_t));
		}
		// Trailing meshlet section (v2; v1 blobs end here). Always written
		// (possibly count 0) so the layout is uniform for new paks.
		{
			CookedMeshlets cooked;
			if (auto hit = cluster.GetMeshMeshlets(uri))
				cooked = *hit;
			PutU32(blob, static_cast<uint32_t>(cooked.meshlets.size()));
			for (const Meshlet& m : cooked.meshlets) {
				PutU32(blob, m.vertexOffset);
				PutU32(blob, m.vertexCount);
				PutU32(blob, m.triangleOffset);
				PutU32(blob, m.triangleCount);
				PutVec3(blob, m.boundCenter);
				PutF32(blob, m.boundRadius);
				PutVec3(blob, m.coneApex);
				PutVec3(blob, m.coneAxis);
				PutF32(blob, m.coneCutoff);
			}
			PutU32(blob, static_cast<uint32_t>(cooked.vertices.size()));
			for (uint32_t v : cooked.vertices)
				PutU32(blob, v);
			PutU32(blob, static_cast<uint32_t>(cooked.triangles.size()));
			blob.insert(blob.end(), cooked.triangles.begin(), cooked.triangles.end());
			if (stats && !cooked.meshlets.empty()) {
				stats->meshesWithMeshlets++;
				stats->meshletCount += static_cast<uint32_t>(cooked.meshlets.size());
			}
		}
		// Trailing skin-stream section (the Northlight data path): u32
		// count + count * 8 raw VertexSkin bytes (mesh.h binding 2: RGBA8
		// joint ids + RGBA8 weights, top-4 normalized). ALWAYS written
		// (possibly count 0) so the layout is uniform; the LOAD side
		// accepts an absent section (pre-skin paks) as a static mesh.
		// The stream rides the retained upload (the import post pass
		// fills it - GltfToSceneImporter::BuildSkinData).
		{
			const StaticMeshUpload* up = cluster.GetRetainedUpload(h);
			const std::vector<VertexSkin>& skin =
				up ? up->skin : kEmptySkinStream;
			PutU32(blob, static_cast<uint32_t>(skin.size()));
			if (!skin.empty())
				blob.insert(blob.end(),
					reinterpret_cast<const uint8_t*>(skin.data()),
					reinterpret_cast<const uint8_t*>(skin.data())
						+ skin.size() * sizeof(VertexSkin));
		}

		// v4 fast-load layout: packed-SoA mesh streams stored uncompressed
		// (direct pak-image subspans; see the payload section above).
		// Raises the pak version so older readers reject at Open.
		if (!writer.AddBlob(PakAssetKind::Mesh, uri, std::move(blob), true))
			return fail("invalid mesh uri: " + uri);
	}

	// --- Materials ---------------------------------------------------------
	std::vector<ClusterMaterialHandle> matHandles;
	const_cast<Cluster&>(cluster).materials.forEachHandle(
		[&](ClusterMaterialHandle h) { matHandles.push_back(h); });
	std::vector<std::string> matUris;
	matUris.reserve(matHandles.size());

	// Normal-slot texture URIs (see TexCookJob::forceLinear): collected
	// while the material blobs are written below, consumed by the texture
	// cook so a foreign sRGB normal KTX2 still lands on a linear BCn target.
	std::unordered_set<std::string> normalTexUris;

	// RHI texture index -> asset uri (materials store session-local RHI
	// indices; paks store VFS paths).
	std::unordered_map<uint32_t, std::string> rhiIndexToUri;
	for (const auto& [uri, rhi] : cluster.ListTextureUris())
		rhiIndexToUri[rhi.index()] = uri.Str();

	auto texUriForRhi = [&](int rhiIndex) -> std::string {
		if (rhiIndex < 0)
			return {};
		const auto it = rhiIndexToUri.find(static_cast<uint32_t>(rhiIndex));
		return it != rhiIndexToUri.end() ? it->second : std::string{};
	};

	for (uint32_t k = 0; k < matHandles.size(); ++k) {
		const auto h = matHandles[k];
		std::string uri = cluster.GetMaterialUri(h).Str();
		if (uri.empty())
			uri = "<cluster-local>#mat." + std::to_string(h.index());
		matUris.push_back(uri);
		auto idIt = matIdByUri.find(uri);
		if (idIt != matIdByUri.end()) {
			local.mat[h.index()] = idIt->second;
			continue;
		}
		const uint32_t pakId = static_cast<uint32_t>(matIdByUri.size());
		matIdByUri[uri] = pakId;
		local.mat[h.index()] = pakId;

		const Material* mat = const_cast<Cluster&>(cluster).materials.get(h);
		if (!mat)
			return fail("stale material handle in pool");

		std::vector<uint8_t> blob;
		PutStr(blob, uri);
		static_assert(sizeof(MaterialValues) == 128, "MaterialValues layout changed");
		const uint8_t* mv = reinterpret_cast<const uint8_t*>(&mat->values);
		blob.insert(blob.end(), mv, mv + sizeof(MaterialValues));
		PutU32(blob, static_cast<uint32_t>(mat->shadingModel));
		PutU32(blob, static_cast<uint32_t>(mat->alphaMode));
		PutF32(blob, mat->alphaCutoff);
		PutU32(blob, mat->doubleSided ? 1u : 0u);
		PutStr(blob, texUriForRhi(mat->values.textures[0])); // baseColor
		PutStr(blob, texUriForRhi(mat->values.textures[1])); // metallicRoughness
		PutStr(blob, texUriForRhi(mat->values.textures[2])); // normal
		PutStr(blob, texUriForRhi(mat->values.textures[3])); // occlusion
		PutStr(blob, texUriForRhi(mat->values.textures2[0])); // emissive
		{
			const std::string normalUri =
				texUriForRhi(mat->values.textures[2]);
			if (!normalUri.empty())
				normalTexUris.insert(normalUri);
		}
		// Trailing field (appended after the texture URIs so older
		// readers stop cleanly before it; pre-flag paks read as casting).
		PutU32(blob, mat->castsShadows ? 1u : 0u);
		// Trailing shader-LOD policy (see ShaderLodPolicy): per-material
		// far-level shading program. Same convention — appended last.
		PutU32(blob, mat->shaderLod.allowFarVertexLit ? 1u : 0u);
		// Trailing far-quality spec (see ShaderLodQuality): the quality
		// switches for farthest levels that stay on the lit program.
		PutU32(blob, mat->shaderLod.farQuality.specular ? 1u : 0u);
		PutU32(blob, mat->shaderLod.farQuality.normalMap ? 1u : 0u);

		if (!writer.AddBlob(PakAssetKind::Material, uri, std::move(blob)))
			return fail("invalid material uri: " + uri);
	}

	// --- Textures (embedded .ktx2) ------------------------------------------
	// Two phases: collect the (uri, sampler, source) list serially (dedup
	// + validation order stays stable), then cook every payload — ktx
	// parse, Basis -> per-semantic BCn pre-transcode (see CookTexturePayload),
	// tail digest, blob build — on a worker pool (transcode is the dominant
	// cook cost), then append blobs in uri order so paks stay deterministic.
	std::vector<TexCookJob> texCooks;
	for (const auto& [uri, rhi] : cluster.ListTextureUris()) {
		(void)rhi;
		if (!texturesDone.insert(uri.Str()).second)
			continue; // shared with a sibling scene
		const std::vector<uint8_t>* src = cluster.GetTextureSource(uri);
		if (!src || src->empty())
			return fail("missing embedded .ktx2 source for texture: " + uri.Str() +
				" (importer must stash source bytes via SetTextureSource)");
		TextureSamplerDesc samp;
		if (auto hit = cluster.GetTextureSampler(uri))
			samp = *hit;
		// TextureBlob TOC path = the file part (VFS path) so PakBackend can
		// serve the .ktx2 bytes through the VFS overlay.
		const std::string filePart = AssetFilePart(uri);
		if (filePart.empty())
			return fail("texture uri has no VFS file part: " + uri.Str());
		texCooks.push_back(TexCookJob{ uri.Str(), filePart, samp, src,
			normalTexUris.count(uri.Str()) != 0 });
	}
	{
		ThreadPool texPool;
		std::vector<std::future<TexCookResult>> texFuts;
		texFuts.reserve(texCooks.size());
		for (const TexCookJob& job : texCooks) {
			texFuts.push_back(texPool.enqueue([&job] {
				return CookTexturePayload(job);
			}));
		}
		for (size_t i = 0; i < texFuts.size(); ++i) {
			TexCookResult res = texFuts[i].get();
			if (!res.error.empty())
				return fail(res.error);
			textureBytes += res.srcBytes;
			if (res.digested && stats)
				stats->digestedTextures++;
			// Digested payloads stay uncompressed (tail-range direct
			// addressing into the pak image); legacy payloads follow the
			// writer's compression setting as before.
			if (!writer.AddBlob(PakAssetKind::Texture, res.filePart,
					std::move(res.blob), res.digested))
				return fail("invalid texture path: " + res.filePart);
		}
	}

	// --- Lights (one blob per cooked light, URI-keyed like meshes) --------
	std::vector<ClusterLightHandle> lightHandles;
	const_cast<Cluster&>(cluster).lights.forEachHandle(
		[&](ClusterLightHandle h) { lightHandles.push_back(h); });
	for (uint32_t k = 0; k < lightHandles.size(); ++k) {
		const auto h = lightHandles[k];
		std::string uri = cluster.GetLightUri(h).Str();
		if (uri.empty())
			uri = "<cluster-local>#light." + std::to_string(h.index());
		auto idIt = lightIdByUri.find(uri);
		if (idIt != lightIdByUri.end()) {
			local.light[h.index()] = idIt->second;
			continue;
		}
		const uint32_t pakId = static_cast<uint32_t>(lightIdByUri.size());
		lightIdByUri[uri] = pakId;
		local.light[h.index()] = pakId;

		const SceneLight* l = const_cast<Cluster&>(cluster).lights.get(h);
		if (!l)
			return fail("stale light handle in pool");

		std::vector<uint8_t> blob;
		PutStr(blob, uri);
		PutU32(blob, static_cast<uint32_t>(l->type));
		PutVec3(blob, l->color);
		PutF32(blob, l->intensity);
		PutF32(blob, l->range);
		PutF32(blob, l->innerConeAngle);
		PutF32(blob, l->outerConeAngle);
		PutStr(blob, l->name);
		// Trailing flags section (v5 light blobs; v4 blobs end after the
		// name and readers default to 0 — see the read side).
		PutU32(blob, l->flags);
		// Trailing gobo/function section (v6): the spot cookie URI +
		// the procedural light function (type/speed/scale). Old readers
		// fail v6 blobs on the trailing-bytes check (see the read side)
		// — the sanctioned break.
		PutStr(blob, l->cookieUri);
		PutU32(blob, l->functionType);
		PutF32(blob, l->functionSpeed);
		PutF32(blob, l->functionScale);

		if (!writer.AddBlob(PakAssetKind::Light, uri, std::move(blob)))
			return fail("invalid light uri: " + uri);
	}

	// --- Cameras (one blob per authored camera, URI-keyed like lights) --
	std::vector<ClusterCameraHandle> cameraHandles;
	{
		// const_cast: Pool::forEachHandle is non-const (historic). It does
		// not mutate when the functor only reads.
		const_cast<Cluster&>(cluster).cameras.forEachHandle(
			[&](ClusterCameraHandle h) { cameraHandles.push_back(h); });
	}
	for (uint32_t k = 0; k < cameraHandles.size(); ++k) {
		const auto h = cameraHandles[k];
		std::string uri = cluster.GetCameraUri(h).Str();
		if (uri.empty())
			uri = "<cluster-local>#camera." + std::to_string(h.index());
		auto idIt = camIdByUri.find(uri);
		if (idIt != camIdByUri.end()) {
			local.camera[h.index()] = idIt->second;
			continue;
		}
		const uint32_t pakId = static_cast<uint32_t>(camIdByUri.size());
		camIdByUri[uri] = pakId;
		local.camera[h.index()] = pakId;

		const SceneCamera* c = const_cast<Cluster&>(cluster).cameras.get(h);
		if (!c)
			return fail("stale camera handle in pool");

		std::vector<uint8_t> blob;
		PutStr(blob, uri);
		PutU32(blob, static_cast<uint32_t>(c->projection));
		PutF32(blob, c->fovYDeg);
		PutF32(blob, c->orthoHalfH);
		PutF32(blob, c->nearClip);
		PutF32(blob, c->farClip);
		PutStr(blob, c->name);

		if (!writer.AddBlob(PakAssetKind::Camera, uri, std::move(blob)))
			return fail("invalid camera uri: " + uri);
	}

	// --- Decals (one blob per authored/runtime decal, URI-keyed like
	// lights). The look only — the shape rides the node transform and
	// travels in the Scene blob's decal links section (see below).
	{
		std::vector<ClusterDecalHandle> decalHandles;
		// constcast: Pool::forEachHandle is non-const (historic).
		const_cast<Cluster&>(cluster).decals.forEachHandle(
			[&](ClusterDecalHandle h) { decalHandles.push_back(h); });
		for (uint32_t k = 0; k < decalHandles.size(); ++k) {
			const auto h = decalHandles[k];
			std::string uri = cluster.GetDecalUri(h).Str();
			if (uri.empty())
				uri = "<cluster-local>#decal." + std::to_string(h.index());
			auto idIt = decalIdByUri.find(uri);
			if (idIt != decalIdByUri.end()) {
				local.decal[h.index()] = idIt->second;
				continue;
			}
			const uint32_t pakId = static_cast<uint32_t>(decalIdByUri.size());
			decalIdByUri[uri] = pakId;
			local.decal[h.index()] = pakId;

			const SceneDecal* d = const_cast<Cluster&>(cluster).decals.get(h);
			if (!d)
				return fail("stale decal handle in pool");

			std::vector<uint8_t> blob;
			PutStr(blob, uri);
			PutStr(blob, d->albedoUri);
			PutStr(blob, d->normalUri);
			PutVec3(blob, d->tint);
			PutF32(blob, d->opacity);
			PutF32(blob, d->normalStrength);
			PutF32(blob, d->roughnessTarget);
			PutF32(blob, d->metalTarget);
			PutF32(blob, d->depthFadeStart);
			PutU32(blob, d->seed);
			PutStr(blob, d->name);

			if (!writer.AddBlob(PakAssetKind::Decal, uri, std::move(blob)))
				return fail("invalid decal uri: " + uri);
		}
	}

	// --- Scene ---------------------------------------------------------------
	{
		const Scene& scene = cluster.scene;
		std::vector<uint8_t> blob;
		const uint32_t nodeCount = static_cast<uint32_t>(scene.hierarchy.size());
		PutU32(blob, nodeCount);
		for (const Hierarchy& h : scene.hierarchy) {
			PutI32(blob, h.parent);
			PutI32(blob, h.firstChild);
			PutI32(blob, h.nextSibling);
			PutI32(blob, h.lastSibling);
			PutI32(blob, h.level);
		}
		for (const glm::mat4& m : scene.localTransform) {
			const float* p = glm::value_ptr(m);
			for (int i = 0; i < 16; ++i)
				PutF32(blob, p[i]);
		}
		// meshForNode/materialForNode reference pool handle indices;
		// translate to pak-local ids.
		PutU32(blob, static_cast<uint32_t>(scene.meshForNode.size()));
		// Deterministic order for byte-stable paks.
		std::vector<std::pair<uint32_t, uint32_t>> meshPairs(
			scene.meshForNode.begin(), scene.meshForNode.end());
		std::sort(meshPairs.begin(), meshPairs.end());
		for (const auto& [node, localIdx] : meshPairs) {
			const auto it = local.mesh.find(localIdx);
			if (it == local.mesh.end())
				return fail("scene references unknown mesh pool index " + std::to_string(localIdx));
			PutU32(blob, node);
			PutU32(blob, it->second);
		}
		PutU32(blob, static_cast<uint32_t>(scene.materialForNode.size()));
		std::vector<std::pair<uint32_t, uint32_t>> matPairs(
			scene.materialForNode.begin(), scene.materialForNode.end());
		std::sort(matPairs.begin(), matPairs.end());
		for (const auto& [node, localIdx] : matPairs) {
			const auto it = local.mat.find(localIdx);
			if (it == local.mat.end())
				return fail("scene references unknown material pool index " + std::to_string(localIdx));
			PutU32(blob, node);
			PutU32(blob, it->second);
		}
		PutU32(blob, static_cast<uint32_t>(scene.nameForNode.size()));
		std::vector<std::pair<uint32_t, uint32_t>> namePairs(
			scene.nameForNode.begin(), scene.nameForNode.end());
		std::sort(namePairs.begin(), namePairs.end());
		for (const auto& [node, nameIdx] : namePairs) {
			PutU32(blob, node);
			PutU32(blob, nameIdx);
		}
		PutU32(blob, static_cast<uint32_t>(scene.nodeNames.size()));
		for (const auto& n : scene.nodeNames)
			PutStr(blob, n);
		PutU32(blob, static_cast<uint32_t>(scene.materialNames.size()));
		for (const auto& n : scene.materialNames)
			PutStr(blob, n);
		// Trailing static-subtree section (v2; v1 blobs end here).
		{
			PutU32(blob, nodeCount);
			if (scene.staticRoot.size() == scene.hierarchy.size()) {
				blob.insert(blob.end(), scene.staticRoot.begin(), scene.staticRoot.end());
				if (stats) {
					for (uint8_t v : scene.staticRoot)
						if (v)
							stats->staticRoots++;
				}
			}
			else {
				blob.insert(blob.end(), nodeCount, 0);
			}
		}
		// Trailing lightmap-exclusion section (v3; v2 blobs end above).
		// All-zero when nothing is excluded; v2 readers reject v3 blobs
		// (trailing-bytes check) — App/Editor ship together, so paks and
		// readers always move in lockstep.
		{
			PutU32(blob, nodeCount);
			if (scene.noLightmap.size() == scene.hierarchy.size()) {
				blob.insert(blob.end(), scene.noLightmap.begin(), scene.noLightmap.end());
			}
			else {
				blob.insert(blob.end(), nodeCount, 0);
			}
		}
		// Trailing cooked-lights section (v4; v3 blobs end above).
		// Definitions are PakAssetKind::Light blobs; this section carries
		// only the node links as (node, lightPakId) pairs in node order —
		// the same pak-id indirection meshPairs/matPairs use. Always
		// written (possibly empty) so the layout is uniform; v3 paks end
		// after the lightmap section (no lights).
		{
			std::vector<std::pair<uint32_t, uint32_t>> lightPairs(
				scene.lightForNode.begin(), scene.lightForNode.end());
			std::sort(lightPairs.begin(), lightPairs.end());
			PutU32(blob, static_cast<uint32_t>(lightPairs.size()));
			for (const auto& [node, localIdx] : lightPairs) {
				const auto it = local.light.find(localIdx);
				if (it == local.light.end())
					return fail("scene references unknown light pool index " +
						std::to_string(localIdx));
				PutU32(blob, node);
				PutU32(blob, it->second);
			}
		}
		// Trailing scene-name section (v5; v4 blobs end after the lights
		// section with an exact-end check — see the read side). Always
		// written (possibly empty) so the layout is uniform; discovery
		// (ListPakScenes) reads it without building a cluster.
		PutStr(blob, ci < sceneNames.size() ? sceneNames[ci] : std::string{});
		// Per-node dither-on-closeup section (v6; v5 blobs end after the
		// name). All-zero when unused; missing == off on read.
		{
			PutU32(blob, nodeCount);
			if (scene.ditherCloseup.size() == scene.hierarchy.size()) {
				blob.insert(blob.end(), scene.ditherCloseup.begin(),
					scene.ditherCloseup.end());
			}
			else {
				blob.insert(blob.end(), nodeCount, 0);
			}
		}
		// Per-node visibility section (v7; v6 blobs end after dither).
		// hidden flags (1 = hidden subtree root); all-zero == visible.
		// Missing == visible on read.
		{
			PutU32(blob, nodeCount);
			if (scene.hidden.size() == scene.hierarchy.size()) {
				blob.insert(blob.end(), scene.hidden.begin(),
					scene.hidden.end());
			}
			else {
				blob.insert(blob.end(), nodeCount, 0);
			}
		}
		// Per-node camera links (v8; v7 blobs end after visibility).
		// Definitions are PakAssetKind::Camera blobs; this section
		// carries only the node links as (node, camPakId) pairs in node
		// order — the same pak-id indirection the lights section uses.
		// Always written (possibly empty) so the layout is uniform.
		{
			std::vector<std::pair<uint32_t, uint32_t>> camPairs(
				scene.cameraForNode.begin(), scene.cameraForNode.end());
			std::sort(camPairs.begin(), camPairs.end());
			PutU32(blob, static_cast<uint32_t>(camPairs.size()));
			for (const auto& [node, localIdx] : camPairs) {
				const auto it = local.camera.find(localIdx);
				if (it == local.camera.end())
					return fail("scene references unknown camera pool index " +
						std::to_string(localIdx));
				PutU32(blob, node);
				PutU32(blob, it->second);
			}
		}
		// Authored environment (v9; v8 blobs end after the camera links).
		// u32 authored flag + resolved-look record + default-block record
		// + per-node blocks (u32 count + node-ordered (node, record)
		// pairs). Always written so the layout is uniform; old readers
		// fail the trailing-bytes check on v9 blobs (same contract as
		// the v6/v7/v8 additions).
		{
			PutU32(blob, scene.envAuthored ? 1u : 0u);
			PutSceneEnv(blob, scene.environment);
			PutSceneEnv(blob, scene.defaultEnvironment);
			std::vector<std::pair<uint32_t, SceneEnv>> envPairs(
				scene.envForNode.begin(), scene.envForNode.end());
			std::sort(envPairs.begin(), envPairs.end(),
				[](const auto& a, const auto& b) { return a.first < b.first; });
			PutU32(blob, static_cast<uint32_t>(envPairs.size()));
			for (const auto& [node, env] : envPairs) {
				PutU32(blob, node);
				PutSceneEnv(blob, env);
			}
		}
		// Render settings (the postfx look — see SceneRenderSettings):
		// u32 authored flag + ONE record (the look is scene-global,
		// never per-node). Always written so the layout is uniform; old
		// readers fail the trailing-bytes check (the sanctioned break).
		{
			PutU32(blob, scene.hasRenderSettings ? 1u : 0u);
			PutSceneRenderSettings(blob, scene.renderSettings);
		}
		// Default camera (v10; v9 blobs end after the env section): the
		// authored camera node the runtime App spawns at (world settings'
		// typed camera.default field — see Scene::defaultCameraNode).
		// One u32 node id (UINT32_MAX = none). Always written so the
		// layout is uniform; old readers fail the trailing-bytes check
		// on v10 blobs (the sanctioned break — no conversions).
		{
			PutU32(blob, scene.defaultCameraNode);
		}
		// Per-node decal links (v11; v10 blobs end after the default
		// camera). Definitions are PakAssetKind::Decal blobs; this
		// section carries only the node links as (node, decalPakId)
		// pairs in node order — the same pak-id indirection the lights/
		// cameras sections use. Always written (possibly empty) so the
		// layout is uniform.
		{
			std::vector<std::pair<uint32_t, uint32_t>> decalPairs(
				scene.decalForNode.begin(), scene.decalForNode.end());
			std::sort(decalPairs.begin(), decalPairs.end());
			PutU32(blob, static_cast<uint32_t>(decalPairs.size()));
			for (const auto& [node, localIdx] : decalPairs) {
				const auto it = local.decal.find(localIdx);
				if (it == local.decal.end())
					return fail("scene references unknown decal pool index " +
						std::to_string(localIdx));
				PutU32(blob, node);
				PutU32(blob, it->second);
			}
		}
		// Active environment (v12; v11 blobs end after the decal links):
		// the selected env block node (see Scene::activeEnvironment). One
		// u32 node id (UINT32_MAX = none, the default block renders).
		// Always written so the layout is uniform; old readers fail the
		// trailing-bytes check on v12 blobs (the sanctioned break — no
		// conversions).
		{
			PutU32(blob, scene.activeEnvironment);
		}
		// Env-volume settings (v13; v12 blobs end after the active
		// environment): node-ordered (node, mask, priority, weight,
		// radius, enabled-as-u32) rows. Always written (possibly empty)
		// so the layout is uniform; old readers fail the trailing-bytes
		// check on v13 blobs (the sanctioned break - no conversions).
		{
			std::vector<uint32_t> volNodes;
			volNodes.reserve(scene.envVolumeForNode.size());
			for (const auto& [node, vol] : scene.envVolumeForNode)
				volNodes.push_back(node);
			std::sort(volNodes.begin(), volNodes.end());
			PutU32(blob, static_cast<uint32_t>(volNodes.size()));
			for (const uint32_t node : volNodes) {
				const EnvVolume& vol = scene.envVolumeForNode.at(node);
				PutU32(blob, node);
				PutU32(blob, vol.infMask);
				PutF32(blob, vol.priority);
				PutF32(blob, vol.blendWeight);
				PutF32(blob, vol.blendRadius);
				PutU32(blob, vol.enabled ? 1u : 0u);
			}
		}
		// Cluster skeleton (v14; v13 blobs end after the env-volume
		// settings): u32 hasSkeleton + (u32 jointCount + cluster node id
		// per joint + 16 floats per inverseBind + u32 root). Always
		// written (possibly hasSkeleton 0) so the layout is uniform; the
		// LOAD side accepts an absent section (pre-skin paks) as no
		// skeleton. The Skeleton's joint table is what the per-mesh
		// VertexSkin streams (the mesh blob's trailing skin section)
		// index.
		{
			PutU32(blob, cluster.hasSkeleton ? 1u : 0u);
			if (cluster.hasSkeleton) {
				const smvk::touranim::Skeleton& sk = cluster.skeleton;
				PutU32(blob, static_cast<uint32_t>(sk.joints.size()));
				for (uint32_t j : sk.joints)
					PutU32(blob, j);
				for (const glm::mat4& m : sk.inverseBind) {
					for (int c = 0; c < 4; ++c)
						for (int r = 0; r < 4; ++r)
							PutF32(blob, m[c][r]);
				}
				PutU32(blob, sk.root);
			}
		}
		// Analytic env-zone master switch (v15; v14 blobs end after the
		// skeleton): one u32 (1 = zones evaluate). Always written so the
		// layout is uniform; old readers fail the trailing-bytes check on
		// v15 blobs (the sanctioned break - no conversions). The LOAD
		// side treats an absent section as the default ON.
		{
			PutU32(blob, scene.envZonesEnabled ? 1u : 0u);
		}
		// Explicit animation references (v16; v15 blobs end after the
		// env-zone flag): the scene's recorded .tourAnim clip VFS paths
		// (Scene::animClipRefs — the self-contained anim pipeline; the
		// pak cooker stamps them + embeds the clip blobs at the same
		// paths). u32 count + PutStr each, always written (possibly
		// empty) so the layout is uniform; the LOAD side treats an
		// absent section as no refs (pre-v16 paks).
		{
			PutU32(blob, static_cast<uint32_t>(scene.animClipRefs.size()));
			for (const std::string& ref : scene.animClipRefs)
				PutStr(blob, ref);
		}

		if (!writer.AddBlob(PakAssetKind::Scene,
				SceneTocPath(ci, multi), std::move(blob)))
			return fail("scene blob rejected");
		if (stats)
			stats->nodes += nodeCount;
	}
	} // per-cluster loop

	// Cooked GI registry (pak-level, one blob): union of the clusters'
	// named bake bindings, first-seen order. Loaders enforce one
	// binding per kind for now (first wins + warn); the blob simply
	// lists what the cook registered.
	{
		std::vector<GiBinding> unionBindings;
		for (const Cluster* cp : clusters) {
			for (const GiBinding& b : cp->giBindings) {
				if (b.name.empty())
					continue;
				bool seen = false;
				for (const GiBinding& u : unionBindings)
					if (u.kind == b.kind && u.name == b.name) {
						seen = true;
						break;
					}
				if (!seen)
					unionBindings.push_back(b);
			}
		}
		if (!unionBindings.empty()) {
			std::vector<uint8_t> blob;
			PutU32(blob, static_cast<uint32_t>(unionBindings.size()));
			for (const GiBinding& b : unionBindings) {
				PutU32(blob, static_cast<uint32_t>(b.kind));
				PutStr(blob, b.name);
			}
			if (!writer.AddBlob(PakAssetKind::Gi, "<cluster-local>#gi",
					std::move(blob)))
				return fail("gi registry blob rejected");
			if (stats)
				stats->giBindings = static_cast<uint32_t>(unionBindings.size());
		}
	}

	if (stats) {
		stats->meshes = static_cast<uint32_t>(meshIdByUri.size());
		stats->materials = static_cast<uint32_t>(matIdByUri.size());
		stats->textures = static_cast<uint32_t>(texturesDone.size());
		stats->textureBytes = static_cast<uint32_t>(textureBytes);
		stats->lights = static_cast<uint32_t>(lightIdByUri.size());
		stats->cameras = static_cast<uint32_t>(camIdByUri.size());
	}
	return true;
}

bool SerializeCluster(
	const Cluster& cluster,
	PakWriter& writer,
	ClusterSerializeStats* stats,
	std::string* err) {
	return SerializeClusters({ &cluster }, {}, writer, stats, err);
}

// ---------------------------------------------------------------------------
// Deserialize
// ---------------------------------------------------------------------------

namespace {
// Pak-id remap tables for one scene parse (null = discovery listing:
// links are skipped but the cursor still walks + validates structure).
struct SceneLinkMaps {
	const std::vector<ClusterStaticMeshHandle>* meshes = nullptr;
	const std::vector<ClusterMaterialHandle>* materials = nullptr;
	const std::vector<ClusterLightHandle>* lights = nullptr;
	const std::vector<ClusterCameraHandle>* cameras = nullptr;
	const std::vector<ClusterDecalHandle>* decals = nullptr;
};

// Shared Scene-blob parse (see the v2/v3/v4/v5 trailing sections in the
// writer): hierarchy + transforms + node links + names + static/noLM/
// lights sections + v5 name. With maps, links resolve to fresh pool
// handles; without (ListPakScenes), only structure + names come out.
// skeletonOut/hasSkeletonOut (v14, appended last so existing callers keep
// compiling): the trailing CLUSTER-SKELETON section (the joint table the
// per-mesh VertexSkin streams index; the writer always writes it). Null =
// skip (structure-only callers). An ABSENT section (a pre-skin pak) leaves
// *hasSkeletonOut false.
bool ParseSceneBlob(std::span<const uint8_t> payload, Scene& scene,
	std::string& nameOut, const SceneLinkMaps* maps, std::string& err,
	smvk::touranim::Skeleton* skeletonOut = nullptr,
	bool* hasSkeletonOut = nullptr) {
	auto fail = [&](const std::string& msg) {
		err = msg;
		return false;
	};
	Cursor c{ payload, 0 };
	const uint32_t nodeCount = c.u32();
	if (!c.ok || nodeCount == 0 || nodeCount > 1024 * 1024)
		return fail("bad scene node count");
	// Stash the recorded linkage first (single parse), then grow the
	// scene in index order with addNode (sibling order preserved), then
	// restore the recorded linkage verbatim as authoritative.
	std::vector<Hierarchy> recorded;
	recorded.reserve(nodeCount);
	for (uint32_t i = 0; i < nodeCount; ++i) {
		Hierarchy hh;
		hh.parent = c.i32();
		hh.firstChild = c.i32();
		hh.nextSibling = c.i32();
		hh.lastSibling = c.i32();
		hh.level = c.i32();
		if (!c.ok)
			return fail("truncated scene hierarchy");
		recorded.push_back(hh);
	}
	for (uint32_t i = 0; i < nodeCount; ++i)
		addNode(scene, recorded[i].parent, recorded[i].level);
	for (uint32_t i = 0; i < nodeCount; ++i)
		scene.hierarchy[i] = recorded[i];
	// Transforms.
	for (uint32_t i = 0; i < nodeCount; ++i) {
		glm::mat4 m(1.0f);
		float* p = glm::value_ptr(m);
		for (int k = 0; k < 16; ++k)
			p[k] = c.f32();
		if (!c.ok)
			return fail("truncated scene transforms");
		scene.localTransform[i] = m;
		scene.globalTransform[i] = glm::mat4(1.0f);
	}
	const uint32_t meshPairCount = c.u32();
	for (uint32_t i = 0; i < meshPairCount; ++i) {
		const uint32_t node = c.u32();
		const uint32_t pakId = c.u32();
		if (!c.ok || node >= nodeCount)
			return fail("bad scene mesh mapping");
		if (maps && maps->meshes) {
			if (pakId >= maps->meshes->size())
				return fail("bad scene mesh mapping");
			const auto h = (*maps->meshes)[pakId];
			if (!h.valid())
				return fail("scene references invalid mesh pak id");
			scene.meshForNode[node] = h.index();
		}
	}
	const uint32_t matPairCount = c.u32();
	for (uint32_t i = 0; i < matPairCount; ++i) {
		const uint32_t node = c.u32();
		const uint32_t pakId = c.u32();
		if (!c.ok || node >= nodeCount)
			return fail("bad scene material mapping");
		if (maps && maps->materials) {
			if (pakId >= maps->materials->size())
				return fail("bad scene material mapping");
			scene.materialForNode[node] = (*maps->materials)[pakId].index();
		}
	}
	const uint32_t namePairCount = c.u32();
	for (uint32_t i = 0; i < namePairCount; ++i) {
		const uint32_t node = c.u32();
		const uint32_t nameIdx = c.u32();
		if (!c.ok)
			return fail("bad scene name mapping");
		scene.nameForNode[node] = nameIdx;
	}
	const uint32_t nodeNameCount = c.u32();
	for (uint32_t i = 0; i < nodeNameCount; ++i)
		scene.nodeNames.push_back(c.str());
	const uint32_t matNameCount = c.u32();
	for (uint32_t i = 0; i < matNameCount; ++i)
		scene.materialNames.push_back(c.str());
	if (!c.ok)
		return fail("truncated scene blob");
	// Trailing static-subtree section (v2): u32 count + count bytes.
	// v1 blobs end exactly here (no static nodes).
	if (c.off < c.img.size()) {
		const uint32_t staticCount = c.u32();
		if (!c.ok || staticCount != nodeCount)
			return fail("bad scene static section");
		if (c.off + staticCount > c.img.size())
			return fail("scene blob trailing bytes mismatch");
		scene.staticRoot.assign(
			c.img.data() + c.off, c.img.data() + c.off + staticCount);
		c.off += staticCount;
	}
	else {
		scene.staticRoot.assign(nodeCount, 0);
	}
	// Trailing lightmap-exclusion section (v3): u32 count + count
	// bytes. v2 blobs end after the static section — default to no
	// exclusions in that case. v4 blobs carry a lights section after
	// this one, so only require that the bytes FIT (not exact end).
	if (c.off < c.img.size()) {
		const uint32_t lmCount = c.u32();
		if (!c.ok || lmCount != nodeCount)
			return fail("bad scene lightmap section");
		if (c.off + lmCount > c.img.size())
			return fail("scene blob trailing bytes mismatch");
		scene.noLightmap.assign(
			c.img.data() + c.off, c.img.data() + c.off + lmCount);
		c.off += lmCount;
	}
	else {
		scene.noLightmap.assign(nodeCount, 0);
	}
	// Trailing cooked-lights section (v4): u32 pairCount + (node,
	// lightPakId) pairs, remapped to fresh pool indices exactly like
	// the mesh/material mappings above. v3 blobs end after the
	// lightmap section (no lights).
	if (c.off < c.img.size()) {
		const uint32_t lightPairCount = c.u32();
		if (!c.ok || lightPairCount > 1024 * 1024)
			return fail("bad scene light mapping");
		for (uint32_t i = 0; i < lightPairCount; ++i) {
			const uint32_t node = c.u32();
			const uint32_t pakId = c.u32();
			if (!c.ok || node >= nodeCount)
				return fail("bad scene light mapping");
			if (maps && maps->lights) {
				if (pakId >= maps->lights->size())
					return fail("bad scene light mapping");
				const ClusterLightHandle h = (*maps->lights)[pakId];
				if (!h.valid())
					return fail("scene references invalid light pak id");
				scene.lightForNode[node] = h.index();
			}
		}
		if (!c.ok)
			return fail("truncated scene lights section");
		// Trailing scene-name section (v5; v4 blobs end after the
		// lights section — see the writer). Discovery reads it without
		// building a cluster.
		if (c.off < c.img.size()) {
			nameOut = c.str();
			if (!c.ok)
				return fail("truncated scene name");
		}
		// Per-node dither-on-closeup section (v6): u32 count + count
		// bytes. v5 blobs end after the name — default to off in that
		// case (and for the discovery listing path, which shares this
		// parse with null link maps).
		if (c.off < c.img.size()) {
			const uint32_t ditherCount = c.u32();
			if (!c.ok || ditherCount != nodeCount)
				return fail("bad scene dither section");
			if (c.off + ditherCount > c.img.size())
				return fail("scene blob trailing bytes mismatch");
			scene.ditherCloseup.assign(
				c.img.data() + c.off, c.img.data() + c.off + ditherCount);
			c.off += ditherCount;
		}
		else {
			scene.ditherCloseup.assign(nodeCount, 0);
		}
		// Per-node visibility section (v7): u32 count + hidden bytes.
		// v6 blobs end after dither — default to visible in that case.
		if (c.off < c.img.size()) {
			const uint32_t hidCount = c.u32();
			if (!c.ok || hidCount != nodeCount)
				return fail("bad scene visibility section");
			if (c.off + hidCount > c.img.size())
				return fail("scene blob trailing bytes mismatch");
			scene.hidden.assign(
				c.img.data() + c.off, c.img.data() + c.off + hidCount);
			c.off += hidCount;
		}
		else {
			scene.hidden.assign(nodeCount, 0);
		}
		// Per-node camera links (v8): u32 pairCount + (node, camPakId)
		// pairs, remapped exactly like lights. v7 blobs end after the
		// visibility section (no cameras).
		if (c.off < c.img.size()) {
			const uint32_t camPairCount = c.u32();
			if (!c.ok || camPairCount > 1024 * 1024)
				return fail("bad scene camera mapping");
			for (uint32_t i = 0; i < camPairCount; ++i) {
				const uint32_t node = c.u32();
				const uint32_t pakId = c.u32();
				if (!c.ok || node >= nodeCount)
					return fail("bad scene camera mapping");
				if (maps && maps->cameras) {
					if (pakId >= maps->cameras->size())
						return fail("bad scene camera mapping");
					const ClusterCameraHandle h = (*maps->cameras)[pakId];
					if (!h.valid())
						return fail("scene references invalid camera pak id");
					scene.cameraForNode[node] = h.index();
				}
			}
			if (!c.ok)
				return fail("truncated scene cameras section");
		}
		// Authored environment (v9): u32 authored flag + resolved-look
		// record + default-block record + per-node blocks (u32 count +
		// (node, record) pairs). v8 blobs end after the cameras — the
		// struct defaults stand and envAuthored stays false.
		if (c.off < c.img.size()) {
			const uint32_t envFlag = c.u32();
			SceneEnv resolved = c.sceneEnv();
			SceneEnv def = c.sceneEnv();
			const uint32_t envPairCount = c.u32();
			if (!c.ok || envPairCount > 1024 * 1024)
				return fail("bad scene environment section");
			std::unordered_map<NodeId, SceneEnv> table;
			for (uint32_t i = 0; i < envPairCount; ++i) {
				const uint32_t node = c.u32();
				SceneEnv block = c.sceneEnv();
				if (!c.ok || node >= nodeCount)
					return fail("bad scene environment section");
				table[node] = block;
			}
			if (!c.ok)
				return fail("truncated scene environment section");
			scene.environment = resolved;
			scene.envAuthored = envFlag != 0;
			scene.defaultEnvironment = def;
			scene.envForNode = std::move(table);
		}
		// Render settings (the postfx look): u32 authored flag + ONE
		// record. Blobs from before the split end after the env section —
		// the struct defaults stand (the sanctioned break).
		if (c.off < c.img.size()) {
			const uint32_t renderFlag = c.u32();
			SceneRenderSettings rs = c.sceneRenderSettings();
			if (!c.ok)
				return fail("truncated scene render settings section");
			scene.renderSettings = rs;
			scene.defaultRenderSettings = rs;
			scene.hasRenderSettings = renderFlag != 0;
		}
		// Default camera (v10): the authored camera node the runtime App
		// spawns at (see the writer above). v9 blobs end after the env
		// section — defaultCameraNode stays UINT32_MAX (no default).
		if (c.off < c.img.size()) {
			scene.defaultCameraNode = c.u32();
			if (!c.ok)
				return fail("truncated scene default camera section");
			if (scene.defaultCameraNode >= nodeCount) {
				// Dangling target (foreign/hand-edited blob): none.
				scene.defaultCameraNode = UINT32_MAX;
			}
		}
		// Per-node decal links (v11): u32 pairCount + (node, decalPakId)
		// pairs, remapped exactly like lights/cameras. v10 blobs end
		// after the default camera (no decals).
		if (c.off < c.img.size()) {
			const uint32_t decPairCount = c.u32();
			if (!c.ok || decPairCount > 1024 * 1024)
				return fail("bad scene decal mapping");
			for (uint32_t i = 0; i < decPairCount; ++i) {
				const uint32_t node = c.u32();
				const uint32_t pakId = c.u32();
				if (!c.ok || node >= nodeCount)
					return fail("bad scene decal mapping");
				if (maps && maps->decals) {
					if (pakId >= maps->decals->size())
						return fail("bad scene decal mapping");
					const ClusterDecalHandle h = (*maps->decals)[pakId];
					if (!h.valid())
						return fail("scene references invalid decal pak id");
					scene.decalForNode[node] = h.index();
				}
			}
			if (!c.ok)
				return fail("truncated scene decals section");
		}
		// Active environment (v12): the selected env block node (see the
		// writer above). v11 blobs end after the decal links (none). A
		// dangling target clears to none like defaultCameraNode.
		if (c.off < c.img.size()) {
			scene.activeEnvironment = c.u32();
			if (!c.ok)
				return fail("truncated scene active environment section");
			if (scene.activeEnvironment >= nodeCount)
				scene.activeEnvironment = UINT32_MAX;
		}
		// Env-volume settings (v13): u32 rowCount + (node, mask,
		// priority, weight, radius, enabled-u32) rows. v12 blobs end
		// after the active environment (no volumes).
		if (c.off < c.img.size()) {
			const uint32_t volCount = c.u32();
			if (!c.ok || volCount > 1024 * 1024)
				return fail("bad scene env-volume mapping");
			for (uint32_t i = 0; i < volCount; ++i) {
				const uint32_t node = c.u32();
				const uint32_t mask = c.u32();
				const float priority = c.f32();
				const float weight = c.f32();
				const float radius = c.f32();
				const uint32_t enabled = c.u32();
				if (!c.ok || node >= nodeCount)
					return fail("bad scene env-volume mapping");
				EnvVolume vol{};
				vol.infMask = mask & 63u;
				vol.priority = priority;
				vol.blendWeight = std::clamp(weight, 0.0f, 1.0f);
				vol.blendRadius =
					(radius >= 0.0f && std::isfinite(radius)) ? radius : 0.0f;
				vol.enabled = enabled != 0u;
				scene.envVolumeForNode[node] = vol;
			}
			if (!c.ok)
				return fail("truncated scene env-volume section");
		}
		// Cluster skeleton (v14; v13 blobs end after the env-volume
		// settings): u32 hasSkeleton + (u32 jointCount + cluster node id
		// per joint + 16 floats per inverseBind + u32 root). ABSENT =
		// a pre-skin pak (no skeleton); present must end the blob
		// exactly (the shared trailing-bytes check below).
		if (c.off < c.img.size()) {
			const uint32_t has = c.u32();
			if (!c.ok)
				return fail("truncated scene skeleton section");
			if (has != 0u) {
				if (!skeletonOut || !hasSkeletonOut) {
					// Structure-only caller (ListPakScenes): validate the
					// section + skip (the real load - DeserializeCluster -
					// parses it with the out-params). Hard-failing here
					// made every pak listing reject skinned paks.
					smvk::touranim::Skeleton skip{};
					const uint32_t jointCount = c.u32();
					if (!c.ok || jointCount > 4096)
						return fail("bad scene skeleton joint count");
					for (uint32_t i = 0; i < jointCount; ++i)
						if (c.u32() >= nodeCount)
							return fail("bad scene skeleton joint id");
					for (uint32_t i = 0; i < jointCount; ++i)
						for (int k = 0; k < 16; ++k)
							(void)c.f32();
					if (!c.ok)
						return fail("truncated scene skeleton skip");
					(void)c.u32(); // root
					if (!c.ok)
						return fail("truncated scene skeleton skip");
				}
				else {
					smvk::touranim::Skeleton& sk = *skeletonOut;
					const uint32_t jointCount = c.u32();
					if (!c.ok || jointCount > 4096)
						return fail("bad scene skeleton joint count");
					sk.joints.clear();
					sk.inverseBind.clear();
					sk.joints.reserve(jointCount);
					sk.inverseBind.resize(jointCount, glm::mat4(1.0f));
					for (uint32_t i = 0; i < jointCount; ++i) {
						const uint32_t j = c.u32();
						if (!c.ok || j >= nodeCount)
							return fail("bad scene skeleton joint id");
						sk.joints.push_back(j);
					}
					for (uint32_t i = 0; i < jointCount; ++i) {
						glm::mat4 m(1.0f);
						for (int col = 0; col < 4; ++col)
							for (int row = 0; row < 4; ++row)
								m[col][row] = c.f32();
						if (!c.ok)
							return fail("truncated scene skeleton IBM");
						sk.inverseBind[i] = m;
					}
					sk.root = c.u32();
					if (!c.ok)
						return fail("truncated scene skeleton root");
					if (sk.root >= nodeCount)
						sk.root = 0u;
					*hasSkeletonOut = true;
				}
			}
		}
		// Analytic env-zone master switch (v15): one u32 on the wire.
		// ALWAYS ON since the zones rework (the default env packs as the
		// infinite base zone; the toggle is retired) — the read stays for
		// format compatibility, the parsed value is forced true.
		scene.envZonesEnabled = true;
		if (c.off < c.img.size()) {
			const uint32_t zonesOn = c.u32();
			if (!c.ok)
				return fail("truncated scene env-zone flag");
			(void)zonesOn; // forced on above
		}
		// Explicit animation references (v16; v15 blobs end after the
		// env-zone flag): u32 count + strings — the scene's recorded
		// .tourAnim clips (see the writer). ABSENT (a pre-v16 blob) =
		// no refs, the struct default.
		if (c.off < c.img.size()) {
			const uint32_t refCount = c.u32();
			if (!c.ok || refCount > 1024)
				return fail("bad scene anim refs section");
			scene.animClipRefs.clear();
			scene.animClipRefs.reserve(refCount);
			for (uint32_t i = 0; i < refCount; ++i) {
				std::string ref = c.str();
				if (!c.ok)
					return fail("truncated scene anim refs section");
				if (!ref.empty())
					scene.animClipRefs.push_back(std::move(ref));
			}
		}
		if (c.off != c.img.size())
			return fail("scene blob trailing bytes mismatch");
	}
	// Pre-v4 blobs skip the tail wholesale: normalize the v6/v7 defaults.
	if (scene.ditherCloseup.empty())
		scene.ditherCloseup.assign(nodeCount, 0);
	if (scene.hidden.empty())
		scene.hidden.assign(nodeCount, 0);
	RebuildStaticCache(scene);
	MarkAllRootsChanged(scene);
	recalculateGlobalTransforms(scene);
	return true;
}

// Scene blobs in index order ("#scene" first, then "#scene.N").
std::vector<const PakTocEntry*> OrderedSceneEntries(const PakReader& reader) {
	auto sceneEntries = reader.BlobsOfKind(PakAssetKind::Scene);
	std::sort(sceneEntries.begin(), sceneEntries.end(),
		[](const PakTocEntry* a, const PakTocEntry* b) {
			const uint32_t ia = SceneBlobIndex(a->path);
			const uint32_t ib = SceneBlobIndex(b->path);
			if (ia != ib)
				return ia < ib;
			return a->path < b->path;
		});
	return sceneEntries;
}

} // namespace

// Cooked GI registry blob (u32 count | u32 kind | str name). smvk scope
// to match the header: shared by DeserializeCluster (registers into the
// loaded cluster) and inspection tools (no cluster needed).
bool ParseGiRegistryBlob(
	std::span<const uint8_t> payload,
	std::vector<GiBinding>& out,
	std::string& err) {
	Cursor c{ payload, 0 };
	const uint32_t count = c.u32();
	if (!c.ok || count > 1024) {
		err = "bad gi registry count";
		return false;
	}
	out.clear();
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t kind = c.u32();
		const std::string name = c.str();
		if (!c.ok || kind > 5 || name.empty() ||
			name.size() > 512) {
			err = "bad gi registry entry";
			return false;
		}
		out.push_back(GiBinding{
			static_cast<GiBinding::Kind>(kind), name });
	}
	if (!c.ok || c.off != payload.size()) {
		err = "gi registry trailing bytes mismatch";
		return false;
	}
	return true;
}

std::vector<PakSceneInfo> ListPakScenes(
	const PakReader& reader,
	std::string* err) {
	std::vector<PakSceneInfo> out;
	for (const PakTocEntry* e : OrderedSceneEntries(reader)) {
		Scene tmp;
		std::string name, perr;
		// Structure-only listing: the skeleton out-params stay null, so a
		// v14 scene blob with a skeleton must not hard-fail here - the
		// listing only wants names + node counts (the real load happens
		// in DeserializeCluster WITH the skeleton params). ParseSceneBlob
		// tolerates skeletonOut == nullptr by VALIDATING-AND-SKIPPING the
		// section (the same absent-section semantics).
		if (!ParseSceneBlob(reader.Payload(*e), tmp, name, nullptr, perr,
				nullptr, nullptr)) {
			if (err)
				*err = e->path + ": " + perr;
			return {};
		}
		out.push_back(PakSceneInfo{ name, e->path,
			static_cast<uint32_t>(tmp.hierarchy.size()) });
	}
	return out;
}

std::unique_ptr<Cluster> DeserializeCluster(
	ResourceManager& ctx,
	const PakReader& reader,
	std::string* err,
	uint32_t sceneIndex,
	TextureStreamer* streamer,
	std::shared_ptr<const std::vector<uint8_t>> imagePin) {
	// Failure after textures/meshes were queued in the upload streamer
	// leaves transfer command buffers + graphics acquire barriers
	// referencing the partial cluster's VkImages/VkBuffers. The cluster
	// is destroyed on return, so drain the streamer first: flush submits
	// the batches while the images are still alive, wait retires them,
	// and only then may the holders be torn down. Without this the
	// deferred destroy runs immediately (no graphics submit yet at load
	// time) while the upload is still in flight -> VUID-vkDestroyImage-
	// 01000 + a driver AV in the next submitState's vkCmdPipelineBarrier2
	// on the dead VkImage (see Bistro pak fallback crash).
	auto fail = [&](const std::string& msg) -> std::unique_ptr<Cluster> {
		if (err)
			*err = msg;
		std::cerr << "DeserializeCluster: " << msg << std::endl;
		// Workers are joined inside LoadKtxTexturesParallelMemory, so no
		// concurrent streamer submits are in flight here; safe to drain.
		ctx.WaitForUploads();
		return nullptr;
	};

	auto cluster = std::make_unique<Cluster>(&ctx);

	// --- Textures first (materials resolve URIs against them) ---------------
	// Two lists: digest entries (Texture blobs carrying a tail digest —
	// objects + coarse-mip tail upload, streamer-registered when a
	// streamer is present) and legacy entries (digest-less blobs, e.g.
	// bakery-appended cubemaps — today's ktx-parse full-resident path).
	// Payload spans borrow the reader image, which outlives this call by
	// contract; the streamer's demand upgrades borrow PAST this call —
	// those payloads stay pinned on the cluster below.
	const auto texEntries = reader.BlobsOfKind(PakAssetKind::Texture);
	// --- Bake payloads (GI/cubemap content riding as Texture blobs) ------
	// sggi/sgrv/givolume/manifests/sglm + cubemap manifests/KTX2s ALL under
	// gi/ (the canonical bake-asset home — cubemaps at gi/cubemaps/<id>.*,
	// the id-keyed capture-set paths): CLUSTER-OWNED payload bytes (the
	// entity home — see Cluster::bakePayloads / World::ReadBakeBytes),
	// never RHI textures. Excluded from the texture passes below (the
	// KTX decoder would fail the non-KTX ones anyway; the
	// CubemapLibrary decodes the cubemap KTX2s itself, so the old
	// double-decode dies too).
	const auto isBakePath = [](const std::string& p) {
		return p.rfind("gi/", 0) == 0;
	};
	bool bakeBorrowedAny = false;
	for (const PakTocEntry* e : texEntries) {
		if (!isBakePath(e->path))
			continue;
		PakTexturePayload p;
		const bool v4 = ParseTexturePayloadV4(reader.Payload(*e), p);
		if (!v4 && !ParseTexturePayload(reader.Payload(*e), p)) {
			// Not a texture-envelope payload (foreign blob): skip.
			continue;
		}
		if ((e->flags & PakEntryFlag_Lz4) != 0 || !imagePin) {
			// LZ4 entries decompress into the READER's cache (reader
			// lifetime, not the pinned image) — and without an image pin
			// there is nothing to borrow from: own a copy either way.
			Cluster::BakePayload bp;
			bp.owned.assign(p.ktxBytes.begin(), p.ktxBytes.end());
			cluster->AddBakePayload(e->path, std::move(bp.owned));
		}
		else {
			// Uncompressed + pinned image: the payload span borrows the
			// pak IMAGE — pin it on the cluster (same contract as the
			// digest tails) and keep the span zero-copy.
			cluster->AddBakePayloadBorrowed(e->path, p.ktxBytes);
			bakeBorrowedAny = true;
		}
	}
	if (bakeBorrowedAny)
		cluster->PinPakImage(imagePin); // lvalue: shared copy, digest pass may still move
	struct TexLoad {
		std::string uri; // full asset uri (== file part for textures)
		PakTexturePayload payload;
		TextureStreamMeta meta; // valid when hasMeta
		bool hasMeta = false;
	};
	std::vector<TexLoad> digestLoads;
	std::vector<TexLoad> legacyLoads;
	digestLoads.reserve(texEntries.size());
	legacyLoads.reserve(texEntries.size());
	for (const PakTocEntry* e : texEntries) {
		if (isBakePath(e->path))
			continue; // bake payload, harvested above
		PakTexturePayload p;
		bool v4 = ParseTexturePayloadV4(reader.Payload(*e), p);
		if (!v4 && !ParseTexturePayload(reader.Payload(*e), p))
			return fail("malformed texture payload: " + e->path);
		TexLoad t{ p.uri.empty() ? e->path : p.uri, p, {}, false };
		if (v4 && !p.digest.empty()) {
			if (!ParseTextureDigest(p.digest, t.meta))
				return fail("malformed texture digest: " + e->path);
			t.hasMeta = true;
		}
		((streamer && t.hasMeta) ? digestLoads : legacyLoads)
			.push_back(std::move(t));
	}
	// Borrowed payloads need the cluster pin (demand upgrades run
	// post-load against the reader image).
	if (!digestLoads.empty()) {
		if (!imagePin)
			return fail("digest textures need the pak image pin");
		cluster->PinPakImage(std::move(imagePin));
	}
	std::cerr << "DeserializeCluster: textures: " << digestLoads.size()
		<< " digest (tail-only) + " << legacyLoads.size() << " legacy (full)"
		<< (streamer ? " [streamer]" : " [no streamer]") << std::endl;
	// Digest list: parallel tail-split creation — the worker parses +
	// transcodes (as the legacy path) but uploads ONLY the tail
	// [tailFirstMip, end) and registers with the streamer for demand
	// upgrades. Payload bytes stay borrowed (pinned above).
	ParallelTextureStats digestStats;
	std::unordered_map<int, Holder<TextureHandle>> digestHolders;
	if (!digestLoads.empty()) {
		std::vector<KtxTailJob> jobs;
		jobs.reserve(digestLoads.size());
		for (size_t i = 0; i < digestLoads.size(); ++i) {
			const auto& t = digestLoads[i];
			jobs.push_back(KtxTailJob{
				static_cast<int>(i),
				t.meta.tailFirstMip,
				t.payload.ktxBytes.data(),
				t.payload.ktxBytes.size(),
				t.uri,
			});
		}
		digestHolders = LoadStreamingTailTexturesParallel(
			ctx, jobs, streamer, {}, &digestStats);
		if (digestStats.failed > 0)
			std::cerr << "DeserializeCluster: " << digestStats.failed
				<< " digest texture(s) failed to load" << std::endl;
	}
	// Legacy list: parallel ktx decode + full upload (today's path).
	ParallelTextureStats texStats;
	std::unordered_map<int, Holder<TextureHandle>> holders;
	if (!legacyLoads.empty()) {
		std::vector<KtxMemoryJob> jobs;
		jobs.reserve(legacyLoads.size());
		for (size_t i = 0; i < legacyLoads.size(); ++i) {
			const auto& t = legacyLoads[i];
			jobs.push_back(KtxMemoryJob{
				static_cast<int>(i),
				t.payload.ktxBytes.data(),
				t.payload.ktxBytes.size(),
				t.uri,
			});
		}
		holders = LoadKtxTexturesParallelMemory(ctx, jobs, {}, &texStats);
		if (texStats.failed > 0)
			std::cerr << "DeserializeCluster: " << texStats.failed << " texture(s) failed to load" << std::endl;
	}
	// Digest + legacy holds land in the cluster pools identically (uri +
	// sampler registration). SetTextureSource copies survive only when no
	// streamer is present: the streamer path borrows the pinned pak image
	// instead of duplicating every payload.
	for (size_t i = 0; i < digestLoads.size(); ++i) {
		const auto it = digestHolders.find(static_cast<int>(i));
		if (it == digestHolders.end())
			continue; // failed; materials referencing it fall back to empty
		const auto& t = digestLoads[i];
		TextureHandle rhi = it->second;
		cluster->textures.create(std::move(it->second));
		cluster->RegisterTextureUri(t.uri, rhi);
		cluster->RegisterTextureSampler(t.uri, TextureSamplerDesc{
			t.payload.magFilter, t.payload.minFilter,
			t.payload.wrapS, t.payload.wrapT,
		});
	}
	for (size_t i = 0; i < legacyLoads.size(); ++i) {
		const auto it = holders.find(static_cast<int>(i));
		if (it == holders.end())
			continue; // failed; materials referencing it fall back to empty
		const auto& t = legacyLoads[i];
		TextureHandle rhi = it->second;
		cluster->textures.create(std::move(it->second));
		cluster->RegisterTextureUri(t.uri, rhi);
		cluster->RegisterTextureSampler(t.uri, TextureSamplerDesc{
			t.payload.magFilter, t.payload.minFilter,
			t.payload.wrapS, t.payload.wrapT,
		});
		if (!streamer) {
			cluster->SetTextureSource(t.uri,
				std::vector<uint8_t>(t.payload.ktxBytes.begin(), t.payload.ktxBytes.end()));
		}
	}

	// --- Materials ------------------------------------------------------------
	const auto matEntries = reader.BlobsOfKind(PakAssetKind::Material);
	std::vector<ClusterMaterialHandle> matByPakId;
	matByPakId.reserve(matEntries.size());
	for (const PakTocEntry* e : matEntries) {
		Cursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		// Material blob, strict: the CURRENT MaterialValues layout and
		// ALL trailing fields (texture URIs, castsShadows). Paks are
		// cooked artifacts of this build — older layouts fail loudly and
		// get recooked; no runtime tolerance of stale shapes.
		MaterialValues values{};
		if (c.off + sizeof(MaterialValues) > c.img.size())
			return fail("truncated material values (pak cooked with an "
				"older layout - recook the pak): " + e->path);
		memcpy(&values, c.img.data() + c.off, sizeof(MaterialValues));
		c.off += sizeof(MaterialValues);
		const uint32_t shading = c.u32();
		const uint32_t alpha = c.u32();
		const float cutoff = c.f32();
		const uint32_t ds = c.u32();
		std::string texUris[5];
		for (int k = 0; k < 5; ++k)
			texUris[k] = c.str();
		if (!c.ok)
			return fail("truncated material blob: " + e->path);
		// castsShadows trailer (after the texture URIs).
		const uint32_t cs = c.u32();
		if (!c.ok)
			return fail("truncated material blob (missing castsShadows; "
				"stale pak - recook the pak): " + e->path);
		const bool castsShadows = cs != 0;
		// Shader-LOD policy trailer (after castsShadows; see
		// ShaderLodPolicy). Strict like every other trailing field:
		// stale paks fail loudly and get recooked, no silent defaults.
		const uint32_t svl = c.u32();
		if (!c.ok)
			return fail("truncated material blob (missing shaderLod; "
				"stale pak - recook the pak): " + e->path);
		const bool allowFarVertexLit = svl != 0;
		// Far-quality trailer (after the policy; see ShaderLodQuality).
		// Same strictness: stale paks fail loudly, no silent defaults.
		ShaderLodQuality farQuality{};
		const uint32_t fqSpec = c.u32();
		const uint32_t fqNorm = c.u32();
		if (!c.ok)
			return fail("truncated material blob (missing farQuality; "
				"stale pak - recook the pak): " + e->path);
		farQuality.specular = fqSpec != 0;
		farQuality.normalMap = fqNorm != 0;

		// Exact MaterialValues round-trip: everything (factors, params,
		// ambient) is preserved verbatim except the texture slots, which
		// are session-local RHI indices at cook time and must be remapped
		// to the freshly-uploaded handles.
		auto texSlot = [&](const std::string& u) -> int32_t {
			if (u.empty())
				return -1;
			auto hit = cluster->FindTextureByUri(u);
			return (hit && !hit->empty()) ? static_cast<int32_t>(hit->index()) : -1;
		};
		values.textures[0] = texSlot(texUris[0]);
		values.textures[1] = texSlot(texUris[1]);
		values.textures[2] = texSlot(texUris[2]);
		values.textures[3] = texSlot(texUris[3]);
		values.textures2[0] = texSlot(texUris[4]);

		const ClusterMaterialHandle h = cluster->CreateMaterialFromValues(
			values,
			shading <= 2 ? static_cast<ShadingModel>(shading) : ShadingModel::DefaultLit,
			alpha <= 2 ? static_cast<AlphaMode>(alpha) : AlphaMode::Opaque,
			cutoff,
			ds != 0,
			castsShadows,
			allowFarVertexLit,
			farQuality);
		if (!h.valid())
			return fail("CreateMaterial failed for: " + uri);
		cluster->RegisterMaterialUri(uri, h);
		matByPakId.push_back(h);
	}

	// --- Meshes (vertex/index payloads re-uploaded via the standard path) --
	// Two phases: parse ALL blobs first (a malformed blob now fails before
	// ANY upload is staged — the old loop could fail mid-way with half the
	// meshes already queued in the streamer), then ONE batched upload:
	// UploadStaticMeshes packs a single SoA blob per heap and issues three
	// BulkCopyBuffer calls (table-driven; 512MB blobs internally — one
	// memcpy pass + one submit + one barrier set per blob instead of three
	// streamer calls per mesh). The caller issues ctx.FlushUploads().
	const auto meshEntries = reader.BlobsOfKind(PakAssetKind::Mesh);
	struct MeshMeta {
		std::string uri;
		float sphere = 0.0f;
		uint32_t lmChunk = 0;
		std::vector<glm::vec4> lmXforms;
		LodInfo lod;
		CookedMeshlets meshlets;
		std::vector<VertexSkin> skin;
	};
	std::vector<Cluster::StaticMeshDirectSource> directs;
	std::vector<MeshMeta> meshMetas;
	std::vector<ClusterStaticMeshHandle> meshByPakId;
	directs.reserve(meshEntries.size());
	meshMetas.reserve(meshEntries.size());
	meshByPakId.reserve(meshEntries.size());
	for (const PakTocEntry* e : meshEntries) {
		Cursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		const uint32_t vertexCount = c.u32();
		const uint32_t indexCount = c.u32();
		const uint32_t surfaceCount = c.u32();
		if (!c.ok || vertexCount == 0 || vertexCount > 16 * 1024 * 1024 ||
			indexCount > 64 * 1024 * 1024 || surfaceCount > 4096)
			return fail("truncated/insane mesh header: " + e->path);
		StaticMeshUpload upload;
		upload.surfaces.reserve(surfaceCount);
		for (uint32_t s = 0; s < surfaceCount; ++s) {
			Surface surf;
			surf.firstIndex = c.u32();
			surf.indexCount = c.u32();
			surf.baseVertex = c.i32();
			upload.surfaces.push_back(surf);
		}
		upload.aabbMin = c.vec3();
		upload.aabbMax = c.vec3();
		const float sphere = c.f32();
		const uint32_t topology = c.u32();
		if (topology != static_cast<uint32_t>(PrimitiveTopology::TriangleList))
			return fail("unsupported mesh topology in: " + e->path);
		LodInfo lod;
		lod.groupId = c.u32();
		lod.lodIndex = c.u32();
		lod.candidateCount = c.u32();
		lod.simplifyError = c.f32();
		lod.screenThreshold = c.f32();
		lod.triCount = c.u32();
		lod.acmr = c.f32();
		const uint32_t lmChunk = c.u32();
		const uint32_t lmXformCount = c.u32();
		std::vector<glm::vec4> lmXforms;
		if (!c.ok || lmXformCount > 1024 * 1024)
			return fail("truncated/corrupt mesh lightmap transforms: " + e->path);
		lmXforms.reserve(lmXformCount);
		for (uint32_t i = 0; i < lmXformCount; ++i) {
			glm::vec4 xf;
			xf.x = c.f32();
			xf.y = c.f32();
			xf.z = c.f32();
			xf.w = c.f32();
			if (!c.ok)
				return fail("truncated mesh lightmap transforms: " + e->path);
			lmXforms.push_back(xf);
		}
		if (!c.ok)
			return fail("truncated mesh metadata: " + e->path);
		// Packed-SoA payload: the exact GPU heap streams (VertexPos[] +
		// VertexShading[] + u32[]) addressed by blob-relative offsets. NO
		// vertices are materialized here — the upload phase passes the
		// offsets straight to UploadStaticMeshesDirect (pure offset math
		// into the pak image). Strict: a blob without the layout marker is
		// a stale pak — hard fail (magic-gated, recook from zero).
		const uint32_t layout = c.u32();
		if (!c.ok || layout != 1u)
			return fail("unsupported mesh blob layout (stale pak - "
				"recook from zero): " + e->path);
		const uint64_t posOff = c.u64();
		const uint64_t shadeOff = c.u64();
		const uint64_t idxOff = c.u64();
		// Direct addressing: the streams must be pak-image subspans —
		// the cooker stores mesh blobs uncompressed.
		if (e->flags & PakEntryFlag_Lz4)
			return fail("mesh blob compressed (recook from zero): "
				+ e->path);
		const size_t posBytes =
			static_cast<size_t>(vertexCount) * sizeof(VertexPos);
		const size_t shadeBytes =
			static_cast<size_t>(vertexCount) * sizeof(VertexShading);
		const size_t idxBytes =
			static_cast<size_t>(indexCount) * sizeof(uint32_t);
		if (posOff + posBytes > e->size || shadeOff + shadeBytes > e->size
			|| idxOff + idxBytes > e->size)
			return fail("mesh stream ranges overrun blob: " + e->path);
		// The meshlet trailer follows the index stream.
		c.off = static_cast<size_t>(idxOff) + idxBytes;
		directs.push_back(Cluster::StaticMeshDirectSource{
			e->offset + posOff,
			e->offset + shadeOff,
			e->offset + idxOff,
			vertexCount,
			indexCount,
			upload.aabbMin,
			upload.aabbMax,
		});
		// Trailing meshlet section. The cooker ALWAYS writes it (possibly
		// count 0), so a missing or malformed trailer is a stale/corrupt
		// pak: hard fail. Meshlets are a COOKED artifact — the runtime
		// never rebuilds them; a mesh without cooked meshlets draws direct
		// (unculled) until the pak is recooked.
		CookedMeshlets cooked;
		{
			if (c.off >= c.img.size())
				return fail("mesh blob has no meshlet trailer (stale pak - "
					"recook the pak): " + e->path);
			const uint32_t meshletCount = c.u32();
			if (!c.ok || meshletCount > 1024 * 1024)
				return fail("corrupt meshlet trailer: " + e->path);
			cooked.meshlets.reserve(meshletCount);
			for (uint32_t m = 0; m < meshletCount; ++m) {
				Meshlet ml;
				ml.vertexOffset = c.u32();
				ml.vertexCount = c.u32();
				ml.triangleOffset = c.u32();
				ml.triangleCount = c.u32();
				ml.boundCenter = c.vec3();
				ml.boundRadius = c.f32();
				ml.coneApex = c.vec3();
				ml.coneAxis = c.vec3();
				ml.coneCutoff = c.f32();
				if (!c.ok || ml.vertexCount > 255 || ml.triangleCount > 512)
					return fail("corrupt meshlet trailer: " + e->path);
				cooked.meshlets.push_back(ml);
			}
			const uint32_t vertCount = c.u32();
			if (!c.ok || vertCount > 64 * 1024 * 1024)
				return fail("corrupt meshlet trailer: " + e->path);
			cooked.vertices.reserve(vertCount);
			for (uint32_t v = 0; v < vertCount; ++v)
				cooked.vertices.push_back(c.u32());
			const uint32_t triBytes = c.u32();
			if (!c.ok || triBytes > 256 * 1024 * 1024
				|| c.off + triBytes > c.img.size())
				return fail("corrupt meshlet trailer: " + e->path);
			cooked.triangles.assign(
				c.img.data() + c.off, c.img.data() + c.off + triBytes);
			c.off += triBytes;
			// Cross-check windows against the payload sizes.
			for (const Meshlet& ml : cooked.meshlets) {
				if ((size_t)ml.vertexOffset + ml.vertexCount > cooked.vertices.size() ||
					(size_t)ml.triangleOffset + (size_t)ml.triangleCount * 3 > cooked.triangles.size()) {
					return fail("corrupt meshlet trailer: " + e->path);
				}
			}
		}
		// Trailing skin-stream section (the Northlight data path; the
		// writer ALWAYS writes it after the meshlet trailer): u32 count +
		// count * 8 raw VertexSkin bytes. An ABSENT section (a pre-skin
		// pak, blob ends right after the meshlets) = a static mesh; a
		// present section must end the blob exactly.
		std::vector<VertexSkin> skin;
		if (c.off < c.img.size()) {
			const uint32_t skinCount = c.u32();
			if (!c.ok || skinCount > 16 * 1024 * 1024
				|| c.off + uint64_t(skinCount) * sizeof(VertexSkin)
					> c.img.size())
				return fail("corrupt mesh skin section: " + e->path);
			skin.assign(
				reinterpret_cast<const VertexSkin*>(c.img.data() + c.off),
				reinterpret_cast<const VertexSkin*>(c.img.data() + c.off)
					+ skinCount);
			c.off += size_t(skinCount) * sizeof(VertexSkin);
			if (c.off != c.img.size())
				return fail("mesh skin section does not end the blob: "
					+ e->path);
		}
		meshMetas.push_back(MeshMeta{ std::move(uri), sphere, lmChunk,
			std::move(lmXforms), lod, std::move(cooked), std::move(skin) });
	}
	{
		std::vector<ClusterStaticMeshHandle> handles =
			cluster->UploadStaticMeshesDirect(reader.ImageData(),
				std::span<const Cluster::StaticMeshDirectSource>(directs));
		if (handles.empty() && !directs.empty())
			return fail("UploadStaticMeshesDirect failed (degenerate mesh "
				"or full heap): " + meshMetas.front().uri);
		for (size_t i = 0; i < handles.size(); ++i) {
			const ClusterStaticMeshHandle h = handles[i];
			MeshMeta& meta = meshMetas[i];
			// UploadStaticMeshes fills AABB only; restore the cooked sphere
			// and the lightmap chunk (rides the draw ring to the lit
			// fragment).
			if (StaticMesh* m = cluster->staticMeshes.get(h)) {
				m->geometry.sphereRadius = meta.sphere;
				m->lmChunk = meta.lmChunk;
				m->lmChartXforms = std::move(meta.lmXforms);
				if (!meta.meshlets.empty())
					m->meshlets = std::move(meta.meshlets);
				m->skinned = !meta.skin.empty();
			}
			cluster->RegisterMeshUri(meta.uri, h);
			cluster->RegisterMeshLod(meta.uri, meta.lod);
			if (StaticMesh* m = cluster->staticMeshes.get(h)) {
				if (m->meshlets && !m->meshlets->empty())
					cluster->RegisterMeshMeshlets(meta.uri, *m->meshlets);
			}
			// The skin stream (the Northlight data path): stored
			// runtime-side (the draw path's source until the binding-2
			// GPU allocation lands; the cluster skeleton loads from the
			// scene blob below).
			if (!meta.skin.empty())
				cluster->SetMeshSkinStream(h, std::move(meta.skin));
			meshByPakId.push_back(h);
		}
	}

	// --- Lights (definitions; the Scene blob links nodes to pak ids) -------
	// Loaded exactly like meshes: one pool entry per blob, URI
	// re-registered, and the scene's (node, pakId) pairs below remap to
	// fresh pool handle indices.
	const auto lightEntries = reader.BlobsOfKind(PakAssetKind::Light);
	std::vector<ClusterLightHandle> lightByPakId;
	lightByPakId.reserve(lightEntries.size());
	for (const PakTocEntry* e : lightEntries) {
		Cursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		const uint32_t type = c.u32();
		const glm::vec3 color = c.vec3();
		const float intensity = c.f32();
		const float range = c.f32();
		const float inner = c.f32();
		const float outer = c.f32();
		const std::string name = c.str();
		if (!c.ok)
			return fail("truncated light blob: " + e->path);
		if (type > 2)
			return fail("bad light type in: " + e->path);
		// Trailing flags section (v5): kSceneLightFlag_* (static = bake
		// source). v4 blobs end after the name — default to 0.
		uint32_t lightFlags = 0;
		if (c.off + 4 <= c.img.size())
			lightFlags = c.u32();
		if (!c.ok)
			return fail("truncated light flags: " + e->path);
		// Trailing gobo/function section (v6 light blobs; v5 blobs end
		// after the flags): the spot cookie URI (.texture path or image
		// URI — empty = none) + the procedural light function (type/
		// speed/scale). Old readers stop after the flags (exact-end
		// check) and fail v6 blobs loudly — the sanctioned break.
		std::string lightCookie;
		uint32_t lightFn = 0;
		float lightFnSpeed = 1.0f;
		float lightFnScale = 0.5f;
		if (c.off < c.img.size())
			lightCookie = c.str();
		if (c.off + 12 <= c.img.size()) {
			lightFn = c.u32();
			lightFnSpeed = c.f32();
			lightFnScale = c.f32();
		}
		if (!c.ok)
			return fail("truncated light gobo section: " + e->path);
		SceneLight l;
		l.type = static_cast<SceneLight::Type>(type);
		l.color = color;
		l.intensity = intensity;
		l.range = range;
		l.innerConeAngle = inner;
		l.outerConeAngle = outer;
		l.name = name;
		l.flags = lightFlags;
		l.cookieUri = std::move(lightCookie);
		l.functionType = lightFn;
		l.functionSpeed = lightFnSpeed;
		l.functionScale = lightFnScale;
		const ClusterLightHandle h = cluster->CreateLight(std::move(l));
		if (!h.valid())
			return fail("CreateLight failed for: " + uri);
		cluster->RegisterLightUri(uri, h);
		lightByPakId.push_back(h);
	}

	// --- Cameras (definitions; the Scene blob links nodes to pak ids) ----
	// Loaded exactly like lights: one pool entry per blob, URI
	// re-registered, and the scene's (node, pakId) pairs below remap to
	// fresh pool handle indices.
	const auto cameraEntries = reader.BlobsOfKind(PakAssetKind::Camera);
	std::vector<ClusterCameraHandle> camByPakId;
	camByPakId.reserve(cameraEntries.size());
	for (const PakTocEntry* e : cameraEntries) {
		Cursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		const uint32_t proj = c.u32();
		const float fovY = c.f32();
		const float halfH = c.f32();
		const float nearClip = c.f32();
		const float farClip = c.f32();
		const std::string name = c.str();
		if (!c.ok)
			return fail("truncated camera blob: " + e->path);
		if (proj > 1)
			return fail("bad camera projection in: " + e->path);
		if (c.off != c.img.size())
			return fail("trailing bytes in camera blob: " + e->path);
		SceneCamera cam;
		cam.projection = (proj == 1)
			? SceneCamera::Projection::Orthographic
			: SceneCamera::Projection::Perspective;
		cam.fovYDeg = fovY;
		cam.orthoHalfH = halfH;
		cam.nearClip = nearClip;
		cam.farClip = farClip;
		cam.name = name;
		const ClusterCameraHandle h = cluster->CreateCamera(std::move(cam));
		if (!h.valid())
			return fail("CreateCamera failed for: " + uri);
		cluster->RegisterCameraUri(uri, h);
		camByPakId.push_back(h);
	}

	// --- Decals (definitions; the Scene blob links nodes to pak ids) ----
	// Loaded exactly like lights: one pool entry per blob, URI
	// re-registered, and the scene's (node, pakId) pairs below remap to
	// fresh pool handle indices. The SHAPE rides the node transform.
	const auto decalEntries = reader.BlobsOfKind(PakAssetKind::Decal);
	std::vector<ClusterDecalHandle> decalByPakId;
	decalByPakId.reserve(decalEntries.size());
	for (const PakTocEntry* e : decalEntries) {
		Cursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		const std::string albedoUri = c.str();
		const std::string normalUri = c.str();
		const glm::vec3 tint = c.vec3();
		const float opacity = c.f32();
		const float normalStrength = c.f32();
		const float roughnessTarget = c.f32();
		const float metalTarget = c.f32();
		const float depthFadeStart = c.f32();
		const uint32_t seed = c.u32();
		const std::string name = c.str();
		if (!c.ok)
			return fail("truncated decal blob: " + e->path);
		if (c.off != c.img.size())
			return fail("trailing bytes in decal blob: " + e->path);
		SceneDecal d;
		d.albedoUri = std::move(albedoUri);
		d.normalUri = std::move(normalUri);
		d.tint = tint;
		d.opacity = opacity;
		d.normalStrength = normalStrength;
		d.roughnessTarget = roughnessTarget;
		d.metalTarget = metalTarget;
		d.depthFadeStart = depthFadeStart;
		d.seed = seed;
		d.name = std::move(name);
		const ClusterDecalHandle h = cluster->CreateDecal(std::move(d));
		if (!h.valid())
			return fail("CreateDecal failed for: " + uri);
		cluster->RegisterDecalUri(uri, h);
		decalByPakId.push_back(h);
	}

	// --- Cooked GI registry (named bake files; 0..1 blobs, first wins) -----
	for (const PakTocEntry* e : reader.BlobsOfKind(PakAssetKind::Gi)) {
		std::vector<GiBinding> bindings;
		std::string gerr;
		if (!ParseGiRegistryBlob(reader.Payload(*e), bindings, gerr))
			return fail("gi registry rejected: " + gerr);
		for (const GiBinding& b : bindings)
			cluster->RegisterGiBinding(b.kind, b.name);
		break; // first registry wins; the writer emits one
	}

	// --- Scene (indexed; 0 = default) --------------------------------------
	const auto sceneEntries = OrderedSceneEntries(reader);
	if (sceneEntries.empty())
		return fail("pak contains no Scene blob");
	if (sceneIndex >= sceneEntries.size())
		return fail("scene index " + std::to_string(sceneIndex) +
			" out of range (" + std::to_string(sceneEntries.size()) +
			" scene(s) in pak)");
	{
		SceneLinkMaps maps{ &meshByPakId, &matByPakId, &lightByPakId,
			&camByPakId, &decalByPakId };
		std::string sceneName, perr;
		if (!ParseSceneBlob(reader.Payload(*sceneEntries[sceneIndex]),
				cluster->scene, sceneName, &maps, perr,
				&cluster->skeleton, &cluster->hasSkeleton))
			return fail(perr);
		if (!sceneName.empty())
			std::cerr << "DeserializeCluster: scene[" << sceneIndex
				<< "] '" << sceneName << "'\n";
		// The skeleton (the knight's joint table + inverse binds) landed
		// on the cluster - the per-mesh VertexSkin streams (the mesh
		// blobs' trailing sections, loaded above into
		// SetMeshSkinStream) index it. The palette build
		// (touranim::BuildSkinPalette) evaluates it per frame.
		if (cluster->hasSkeleton)
			std::cerr << "DeserializeCluster: cluster skeleton: "
				<< cluster->skeleton.Count() << " joints\n";
	}
	return cluster;
}

} // namespace smvk
