// CubemapBaker: offline scene-capture cubemap baker (GPU-free).
//
//   CubemapBaker --content-dir F:/content --scene sponza
//                [--in-vfs models/X/x.gltf]
//                [--out-dir engine/resources] [--name sky]
//                [--position x,y,z]... [--height 1.5]
//                [--size 256] [--mips on|off] [--prefilter box|ggx]
//                [--prefilter-rays 128] [--bounces 3]
//                [--sun-azimuth 30] [--sun-elevation 55] [--sun-intensity 10000]
//                [--sky-intensity 600] [--test-pattern]
//                [--pak <host path>] [--verify on|off] [--threads 0]
//
// Captures environment cubemaps at points in the scene with the engine's
// CPU path tracer (Engine/GI: GiTraceScene, the same transport the GiBaker
// uses) and writes HDR RGBA32F (VK_FORMAT_R32G32B32A32_SFLOAT, linear OETF)
// KTX2 cubemaps (numFaces = 6) to
// <out-dir>/textures/cubemaps/<name>[_N].ktx2, e.g.
// engine/resources/textures/cubemaps/. The engine CMake step copies
// <out-dir> to ${CMAKE_BINARY_DIR}/engine_resources, which apps mount as
// the lowest-priority VFS backend; Engine/Renderer/cubemap.h
// (CubemapLibrary) loads them by the same VFS path.
//
// Per texel: the face UV maps to a sampling direction with the shared
// FaceUvToDirection math (cubemap.h — the runtime TextureCube samples the
// same mapping, so captures cannot drift from it). A primary-miss ray
// returns the analytic sky gradient (SkyRadiance); a primary hit returns
// the sun-bounce field (TraceIncoming, NEE sun at every vertex including
// the primary hit, --spp paths averaged). Captures store TRUE radiance
// (like PakCooker --cubemap): the runtime scales by the cubemap panel
// intensity, so a mirror reflecting a sun-lit wall matches looking at
// that wall directly. Secondary sky after bounces is not tracked (minor
// indoors; the sky through windows after one bounce is second-order).
//
// --test-pattern skips the scene import and writes an RGBA8 test cubemap
// with the shared GenerateCubemapFacePixels math (face order/orientation
// check end to end, no content needed).
//
// Optional --pak additionally packs the baked bytes into a texture-only
// .pak (PakWriter texture blobs; PakBackend serves them by VFS path, so
// mounting that pak replaces the loose files with no code change).
//
// --verify re-opens every written file through libktx (format / face
// count / level count / linear transfer) and checks both VFS serving
// paths (LooseBackend over the output dir, PakBackend over the pak when
// built).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <volk.h>

#include "ktx.h"
#include <ktxVulkan.h>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/pak.h>
#include <Engine/GI/bake.h>
#include <Engine/Renderer/cubemap.h>
#include <Engine/Renderer/cubemap_placement.h>
#include <Engine/Renderer/cubemap_prefilter.h>
#include "../pak_cooker/cube_png.h"
#include <gltf_import/gltf.h>
#include <Engine/Utils/thread_pool.h>
#include <Engine/Vfs/vfs.h>

namespace {

struct Args {
	std::string contentDir = "content";
	std::string scene;    // sponza | bistro | bistro-niagara | scanlands
	std::string inVfs;    // arbitrary model (overrides --scene input)
	std::string outDir = "engine/resources";
	std::string name = "sky";
	std::vector<glm::vec3> positions; // repeatable --position x,y,z
	float height = 1.5f;  // used when no --position: bbox center raised by height
	uint32_t size = smvk::kCubemapDefaultSize;
	bool mips = true;
	// Mip-chain filter for HDR captures: "box" (legacy 2x2 average) or
	// "ggx" (GGX importance-sampled specular prefilter, roughness =
	// level / (levels-1) — the high-frequency specular IBL path;
	// probes/lightmaps own diffuse + low-frequency specular).
	std::string prefilter = "box";
	uint32_t prefilterRays = 128; // GGX samples per output texel
	uint32_t bounces = 3;
	uint32_t spp = 8;             // transport paths per capture texel (bounce GI)
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
	bool testPattern = false;
	// Sparse adaptive octree placement (Engine/Renderer/cubemap_placement):
	// occupancy-driven subdivision instead of one center capture — open
	// volumes stay coarse, near-surface cells refine, embedded centers
	// drop, growth stops at the 32-probe runtime budget. Wins over the
	// single center capture when set (explicit --position still wins).
	bool octree = false;
	float octreeMax = 8.0f;   // open cells larger than this subdivide (coverage)
	float octreeMin = 2.0f;   // detail floor: never subdivide below this
	uint32_t octreeDepth = 4; // octree depth cap (root = 0)
	// Debug PNGs next to the KTX outputs (like the pak cooker's chart
	// PNGs): per-capture face strips (+ mip sheets) + a top-down probe map.
	bool png = true;
	bool pngMips = true;
	std::string pak; // empty = no pak
	bool verify = true;
	uint32_t threads = 0; // 0 = hardware concurrency
};

bool ParseVec3(const std::string& s, glm::vec3& out) {
	return std::sscanf(s.c_str(), "%f,%f,%f", &out.x, &out.y, &out.z) == 3;
}

bool ParseArgs(int argc, char** argv, Args& out) {
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto need = [&](std::string& dst) {
			if (i + 1 >= argc) {
				std::cerr << "Missing value for " << a << std::endl;
				return false;
			}
			dst = argv[++i];
			return true;
		};
		if (a == "--content-dir") {
			if (!need(out.contentDir)) return false;
		}
		else if (a == "--scene") {
			if (!need(out.scene)) return false;
		}
		else if (a == "--in-vfs") {
			if (!need(out.inVfs)) return false;
		}
		else if (a == "--out-dir") {
			if (!need(out.outDir)) return false;
		}
		else if (a == "--name") {
			if (!need(out.name)) return false;
		}
		else if (a == "--position") {
			std::string v;
			if (!need(v)) return false;
			glm::vec3 p{};
			if (!ParseVec3(v, p)) {
				std::cerr << "--position must be x,y,z" << std::endl;
				return false;
			}
			out.positions.push_back(p);
		}
		else if (a == "--height") {
			std::string v;
			if (!need(v)) return false;
			out.height = std::stof(v);
		}
		else if (a == "--size") {
			std::string v;
			if (!need(v)) return false;
			out.size = static_cast<uint32_t>(std::stoul(v));
			if (out.size < 4 || out.size > 1024) {
				std::cerr << "--size must be within [4, 1024]" << std::endl;
				return false;
			}
		}
		else if (a == "--mips") {
			std::string v;
			if (!need(v)) return false;
			out.mips = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--prefilter") {
			std::string v;
			if (!need(v)) return false;
			out.prefilter = (v == "ggx") ? "ggx" : "box";
		}
		else if (a == "--prefilter-rays") {
			std::string v;
			if (!need(v)) return false;
			out.prefilterRays = std::max<uint32_t>(
				static_cast<uint32_t>(std::stoul(v)), 1);
		}
		else if (a == "--bounces") {
			std::string v;
			if (!need(v)) return false;
			out.bounces = static_cast<uint32_t>(std::stoul(v));
			if (out.bounces > 8) {
				std::cerr << "--bounces must be within [0, 8]" << std::endl;
				return false;
			}
		}
		else if (a == "--spp") {
			std::string v;
			if (!need(v)) return false;
			out.spp = std::clamp<uint32_t>(
				static_cast<uint32_t>(std::stoul(v)), 1, 64);
		}
		else if (a == "--sun-azimuth") {
			std::string v;
			if (!need(v)) return false;
			out.sunAzimuth = std::stof(v);
		}
		else if (a == "--sun-elevation") {
			std::string v;
			if (!need(v)) return false;
			out.sunElevation = std::stof(v);
		}
		else if (a == "--sun-intensity") {
			std::string v;
			if (!need(v)) return false;
			out.sunIntensity = std::stof(v);
		}
		else if (a == "--sky-intensity") {
			std::string v;
			if (!need(v)) return false;
			out.skyIntensity = std::stof(v);
		}
		else if (a == "--test-pattern") {
			out.testPattern = true;
		}
		else if (a == "--octree") {
			out.octree = true;
		}
		else if (a == "--octree-max") {
			std::string v;
			if (!need(v)) return false;
			out.octreeMax = std::max(0.5f, std::stof(v));
		}
		else if (a == "--octree-min") {
			std::string v;
			if (!need(v)) return false;
			out.octreeMin = std::max(0.25f, std::stof(v));
		}
		else if (a == "--octree-depth") {
			std::string v;
			if (!need(v)) return false;
			out.octreeDepth = std::clamp<uint32_t>(
				static_cast<uint32_t>(std::stoul(v)), 1, 6);
		}
		else if (a == "--png") {
			std::string v;
			if (!need(v)) return false;
			out.png = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--png-mips") {
			std::string v;
			if (!need(v)) return false;
			out.pngMips = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--pak") {
			if (!need(out.pak)) return false;
		}
		else if (a == "--verify") {
			std::string v;
			if (!need(v)) return false;
			out.verify = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--threads") {
			std::string v;
			if (!need(v)) return false;
			out.threads = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--help" || a == "-h") {
			return false;
		}
		else {
			std::cerr << "Unknown arg: " << a << std::endl;
			return false;
		}
	}
	if (!out.testPattern && out.inVfs.empty() && out.scene.empty()) {
		std::cerr << "Required: --scene <id> or --in-vfs <model.gltf> (or --test-pattern)" << std::endl;
		return false;
	}
	if (out.name.empty()) {
		std::cerr << "--name must not be empty" << std::endl;
		return false;
	}
	return true;
}

void Usage() {
	std::cout << "CubemapBaker --content-dir content --scene sponza\n"
		<< "         [--in-vfs models/X/x.gltf] [--out-dir engine/resources]\n"
		<< "         [--name sky] [--position x,y,z]... [--height 1.5]\n"
		<< "         [--size 256] [--mips on|off] [--prefilter box|ggx]\n"
		<< "         [--prefilter-rays 128] [--bounces 3]\n"
		<< "         [--sun-azimuth 30] [--sun-elevation 55]\n"
		<< "         [--sun-intensity 10000] [--sky-intensity 600]\n"
		<< "         [--test-pattern] [--pak <host path>] [--verify on|off]\n"
		<< "         [--threads 0]\n"
		<< "         [--octree] [--octree-max 8.0] [--octree-min 2.0]\n"
		<< "         [--octree-depth 4]\n"
		<< "         [--spp 8]\n"
		<< "         [--png on|off] [--png-mips on|off]\n"
		<< "         --prefilter ggx preconvolves the mip chain for the\n"
		<< "         high-frequency specular path (GGX importance sampling,\n"
		<< "         roughness = level/(levels-1)); box keeps the legacy\n"
		<< "         2x2 average chain.\n";
}

std::optional<std::string> SceneGltf(const std::string& id) {
	if (id == "sponza") return std::string("models/Sponza/sponza.gltf");
	if (id == "bistro") return std::string("models/Bistro/bistro.gltf");
	if (id == "bistro-niagara") return std::string("models/BistroNiagara/bistro.gltf");
	if (id == "scanlands") return std::string("models/Scanlands/scanlands.gltf");
	return std::nullopt;
}

struct KtxGuard {
	ktxTexture2* tex = nullptr;
	explicit KtxGuard(ktxTexture2* t) : tex(t) {}
	~KtxGuard() { if (tex) ktxTexture2_Destroy(tex); }
	KtxGuard(const KtxGuard&) = delete;
	KtxGuard& operator=(const KtxGuard&) = delete;
};

// Float box-filtered mip chain per face: level 0 = base, then halved dims
// (2x2 average, odd edges sample the last texel) down to 1x1. RGBA32F.
std::vector<std::vector<float>> BuildFloatMipChain(
	const std::vector<float>& base, uint32_t w, uint32_t h) {
	std::vector<std::vector<float>> chain;
	chain.push_back(base);
	uint32_t pw = w, ph = h;
	while (pw > 1 || ph > 1) {
		const uint32_t cw = std::max(1u, pw / 2);
		const uint32_t ch = std::max(1u, ph / 2);
		std::vector<float> dst(size_t(cw) * ch * 4, 0.0f);
		for (uint32_t y = 0; y < ch; ++y) {
			for (uint32_t x = 0; x < cw; ++x) {
				float acc[4] = { 0, 0, 0, 0 };
				uint32_t n = 0;
				for (uint32_t dy = 0; dy < 2; ++dy) {
					for (uint32_t dx = 0; dx < 2; ++dx) {
						const uint32_t sx = std::min(x * 2 + dx, pw - 1);
						const uint32_t sy = std::min(y * 2 + dy, ph - 1);
						const float* s = &chain.back()[(size_t(sy) * pw + sx) * 4];
						for (int c = 0; c < 4; ++c)
							acc[c] += s[c];
						++n;
					}
				}
				float* d = &dst[(size_t(y) * cw + x) * 4];
				for (int c = 0; c < 4; ++c)
					d[c] = acc[c] / float(n);
			}
		}
		chain.push_back(std::move(dst));
		pw = cw;
		ph = ch;
	}
	return chain;
}

std::filesystem::path HostPath(const std::string& outDir, const std::string& vfsPath) {
	return std::filesystem::path(outDir) / std::filesystem::path(vfsPath);
}

bool WriteCubemapKtx2(const std::filesystem::path& file,
	const std::vector<std::vector<std::vector<float>>>& mipChains,
	uint32_t w, uint32_t h, uint32_t levels, std::string& err) {
	ktxTextureCreateInfo ci{};
	ci.vkFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
	ci.baseWidth = w;
	ci.baseHeight = h;
	ci.baseDepth = 1;
	ci.numDimensions = 2;
	ci.numLevels = levels;
	ci.numLayers = 1;
	ci.numFaces = smvk::kCubemapFaceCount;
	ci.isArray = KTX_FALSE;
	ci.generateMipmaps = KTX_FALSE; // mips are pre-baked into the file

	ktxTexture2* tex = nullptr;
	KTX_error_code res = ktxTexture2_Create(
		&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex);
	if (res != KTX_SUCCESS || !tex) {
		err = std::string("ktxTexture2_Create: ") + ktxErrorString(res);
		return false;
	}
	KtxGuard guard(tex);

	// Linear OETF: captures are linear HDR radiance, the runtime must not
	// sample them through an sRGB view.
	res = ktxTexture2_SetOETF(tex, KHR_DF_TRANSFER_LINEAR);
	if (res != KTX_SUCCESS) {
		err = std::string("ktxTexture2_SetOETF: ") + ktxErrorString(res);
		return false;
	}

	for (uint32_t level = 0; level < levels; ++level) {
		for (uint32_t face = 0; face < smvk::kCubemapFaceCount; ++face) {
			const auto* src = reinterpret_cast<const ktx_uint8_t*>(mipChains[face][level].data());
			const ktx_size_t srcLen = static_cast<ktx_size_t>(
				mipChains[face][level].size() * sizeof(float));
			res = ktxTexture_SetImageFromMemory(ktxTexture(tex),
				level, 0, face, src, srcLen);
			if (res != KTX_SUCCESS) {
				err = std::string("ktxTexture_SetImageFromMemory: ")
					+ ktxErrorString(res);
				return false;
			}
		}
	}

	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	res = ktxTexture2_WriteToNamedFile(tex, file.string().c_str());
	if (res != KTX_SUCCESS) {
		err = std::string("ktxTexture2_WriteToNamedFile: ") + ktxErrorString(res);
		return false;
	}
	return true;
}

bool WriteTestPatternKtx2(const std::filesystem::path& file, uint32_t size,
	uint32_t levels, std::string& err) {
	// RGBA8 test faces from the shared procedural math (same bytes the
	// runtime fallback uploads, so verify can compare directly).
	std::vector<std::vector<uint8_t>> baseFaces;
	baseFaces.reserve(smvk::kCubemapFaceCount);
	for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f)
		baseFaces.push_back(smvk::GenerateCubemapFacePixels(smvk::CubemapFace(f), size));

	ktxTextureCreateInfo ci{};
	ci.vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
	ci.baseWidth = size;
	ci.baseHeight = size;
	ci.baseDepth = 1;
	ci.numDimensions = 2;
	ci.numLevels = levels;
	ci.numLayers = 1;
	ci.numFaces = smvk::kCubemapFaceCount;
	ci.isArray = KTX_FALSE;
	ci.generateMipmaps = KTX_FALSE;

	ktxTexture2* tex = nullptr;
	KTX_error_code res = ktxTexture2_Create(
		&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex);
	if (res != KTX_SUCCESS || !tex) {
		err = std::string("ktxTexture2_Create: ") + ktxErrorString(res);
		return false;
	}
	KtxGuard guard(tex);

	res = ktxTexture2_SetOETF(tex, KHR_DF_TRANSFER_LINEAR);
	if (res != KTX_SUCCESS) {
		err = std::string("ktxTexture2_SetOETF: ") + ktxErrorString(res);
		return false;
	}

	// Per-face box-filtered RGBA8 chain (same math as CreateCubemapTexture).
	for (uint32_t face = 0; face < smvk::kCubemapFaceCount; ++face) {
		std::vector<std::vector<uint8_t>> chain;
		chain.push_back(baseFaces[face]);
		uint32_t pw = size, ph = size;
		while (pw > 1 || ph > 1) {
			const uint32_t cw = std::max(1u, pw / 2);
			const uint32_t ch = std::max(1u, ph / 2);
			std::vector<uint8_t> dst(size_t(cw) * ch * 4);
			for (uint32_t y = 0; y < ch; ++y) {
				for (uint32_t x = 0; x < cw; ++x) {
					uint32_t acc[4] = { 0, 0, 0, 0 };
					uint32_t n = 0;
					for (uint32_t dy = 0; dy < 2; ++dy) {
						for (uint32_t dx = 0; dx < 2; ++dx) {
							const uint32_t sx = std::min(x * 2 + dx, pw - 1);
							const uint32_t sy = std::min(y * 2 + dy, ph - 1);
							const uint8_t* s = &chain.back()[(size_t(sy) * pw + sx) * 4];
							for (int c = 0; c < 4; ++c)
								acc[c] += s[c];
							++n;
						}
					}
					uint8_t* d = &dst[(size_t(y) * cw + x) * 4];
					for (int c = 0; c < 4; ++c)
						d[c] = uint8_t((acc[c] + n / 2) / n);
				}
			}
			chain.push_back(std::move(dst));
			pw = cw;
			ph = ch;
		}
		for (uint32_t level = 0; level < levels && level < chain.size(); ++level) {
			res = ktxTexture_SetImageFromMemory(ktxTexture(tex),
				level, 0, face, chain[level].data(), chain[level].size());
			if (res != KTX_SUCCESS) {
				err = std::string("ktxTexture_SetImageFromMemory: ")
					+ ktxErrorString(res);
				return false;
			}
		}
	}

	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	res = ktxTexture2_WriteToNamedFile(tex, file.string().c_str());
	if (res != KTX_SUCCESS) {
		err = std::string("ktxTexture2_WriteToNamedFile: ") + ktxErrorString(res);
		return false;
	}
	return true;
}

std::optional<std::vector<uint8_t>> ReadFileBytes(const std::filesystem::path& file) {
	std::ifstream f(file, std::ios::binary | std::ios::ate);
	if (!f)
		return std::nullopt;
	const auto size = f.tellg();
	f.seekg(0);
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (!f.read(reinterpret_cast<char*>(bytes.data()), size))
		return std::nullopt;
	return bytes;
}

bool WriteFileBytes(const std::filesystem::path& file,
	const std::vector<uint8_t>& bytes) {
	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	FILE* f = std::fopen(file.string().c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	std::fclose(f);
	return ok;
}

// Re-opens one baked file and checks everything the runtime depends on:
// cube face count, expected level count, linear transfer function and the
// float format (HDR captures) or RGBA8 (test pattern).
bool VerifyCubemapKtx2(const std::filesystem::path& file,
	uint32_t expectedLevels, bool expectHdr, std::string& err) {
	ktxTexture2* tex = nullptr;
	KTX_error_code res = ktxTexture2_CreateFromNamedFile(
		file.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex);
	if (res != KTX_SUCCESS || !tex) {
		err = std::string("open: ") + ktxErrorString(res);
		return false;
	}
	KtxGuard guard(tex);

	const VkFormat fmt = ktxTexture2_GetVkFormat(tex);
	if (expectHdr) {
		if (fmt != VK_FORMAT_R32G32B32A32_SFLOAT) {
			err = "vkFormat is not VK_FORMAT_R32G32B32A32_SFLOAT";
			return false;
		}
	} else if (fmt != VK_FORMAT_R8G8B8A8_UNORM) {
		err = "vkFormat is not VK_FORMAT_R8G8B8A8_UNORM";
		return false;
	}
	if (tex->numFaces != smvk::kCubemapFaceCount) {
		err = "numFaces is " + std::to_string(tex->numFaces) + " (want 6)";
		return false;
	}
	if (tex->numLevels != expectedLevels) {
		err = "unexpected level count " + std::to_string(tex->numLevels)
			+ " (want " + std::to_string(expectedLevels) + ")";
		return false;
	}
	if (ktxTexture2_GetTransferFunction_e(tex) == KHR_DF_TRANSFER_SRGB) {
		err = "transfer function is sRGB (captures must stay linear)";
		return false;
	}
	if (tex->baseWidth != tex->baseHeight) {
		err = "cubemap faces must be square";
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char** argv) {
	Args args;
	if (!ParseArgs(argc, argv, args)) {
		Usage();
		return 2;
	}

	const uint32_t S = args.size;
	uint32_t expectedLevels = 1;
	if (args.mips) {
		uint32_t w = S;
		while (w > 1) {
			w = std::max(1u, w / 2);
			++expectedLevels;
		}
	}

	struct Baked {
		std::string vfsPath;
		std::filesystem::path file;
		size_t bytes = 0;
		bool isManifest = false; // text placement rows, not a KTX cube
	};
	std::vector<Baked> baked;

	if (args.testPattern) {
		const std::string vfsPath = smvk::CubemapVfsPath(args.name);
		const std::filesystem::path file = HostPath(args.outDir, vfsPath);
		std::string err;
		if (!WriteTestPatternKtx2(file, S, expectedLevels, err)) {
			std::cerr << "FAILED " << vfsPath << ": " << err << std::endl;
			return 1;
		}
		const auto sizeOnDisk = std::filesystem::file_size(file);
		baked.push_back(Baked{ vfsPath, file, sizeOnDisk });
		std::cout << "baked(test-pattern) " << vfsPath << "  " << S << "x" << S
			<< "x6  levels=" << expectedLevels << "  " << sizeOnDisk << " B" << std::endl;
	} else {
		const auto t0 = std::chrono::steady_clock::now();

		// Input scene (glTF via the VFS; same lookup rules as the runtime).
		smvk::OverlayVfs vfs;
		vfs.Mount(std::make_shared<smvk::LooseBackend>(args.contentDir), 0, "content");

		std::string inVfs = args.inVfs;
		std::string sceneId = args.scene;
		if (inVfs.empty()) {
			auto mapped = SceneGltf(sceneId);
			if (!mapped) {
				std::cerr << "Unknown --scene '" << sceneId
					<< "' (sponza | bistro | bistro-niagara | scanlands) or use --in-vfs" << std::endl;
				return 2;
			}
			inVfs = *mapped;
		}
		if (sceneId.empty())
			sceneId = "scene";

		std::cout << "CubemapBaker: scene '" << sceneId << "' from " << inVfs << "\n";

		// GPU-free cook-mode Cluster (same import as GiBaker / PakCooker).
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetCookMode(true);
		try {
			GltfToClusterFromVfs(nullptr, vfs, inVfs, *cluster,
				smvk::MeshOptConfig{}, smvk::MeshletConfig{});
		} catch (const std::exception& e) {
			std::cerr << "Import failed: " << e.what() << std::endl;
			return 1;
		}

		smvk::gi::GiSceneBuildOptions buildOpts{};
		// Same import convention as GiBaker/PakCooker: raw locals (the
		// coordFix basis rides the chain top), so punctual aims read
		// with the plain glTF -Z axis.
		buildOpts.punctualAimAxis = GltfPunctualAimAxis(0);
		// Texture averages (SAME block as GiBaker — REQUIRED here, not
		// optional: without it textured materials keep glTF-default
		// metallicFactor 1, which zeroes every albedo (the trace scene
		// goes black: no sun NEE, no bounce — captures see sky-miss
		// only). Captures see textured albedo by design (see above), so
		// this always runs (GiBaker gates the identical block on
		// --textures).
		{
			uint32_t averaged = 0;
			for (const auto& [uri, handle] : cluster->ListTextureUris()) {
				const std::vector<uint8_t>* src = cluster->GetTextureSource(uri);
				if (!src || src->empty())
					continue;
				std::optional<glm::vec3> avg =
					smvk::gi::GiAverageKtxTexel(std::span<const uint8_t>(*src));
				if (!avg)
					continue;
				buildOpts.texelAvg[handle.index()] = *avg;
				++averaged;
			}
			std::cout << "  textures: averaged " << averaged << " ("
				<< cluster->ListTextureUris().size() << " uris)\n";
		}
		buildOpts.env.sunAzimuthDeg = args.sunAzimuth;
		buildOpts.env.sunElevationDeg = args.sunElevation;
		buildOpts.env.sunIntensity = args.sunIntensity;
		buildOpts.env.skyIntensity = args.skyIntensity;
		// Captures see textured albedo (they are what the skybox shows);
		// uniformAlbedo stays off unlike the white-world GI bake.

		smvk::gi::GiTraceScene traceScene;
		std::string err;
		smvk::gi::BuildTraceSceneFromCluster(*cluster, buildOpts, traceScene, err);
		if (!err.empty()) {
			std::cerr << "Scene build failed: " << err << std::endl;
			return 1;
		}
		std::cout << "  trace scene: " << traceScene.Bvh().TriangleCount()
			<< " tris, " << traceScene.materials.size() << " materials\n";

		// Default capture point: scene-bounds center at mid height when no
		// --position was given. --octree replaces the single center with
		// sparse adaptive placement (cubemap_placement.h).
		std::vector<glm::vec3> positions = args.positions;
		std::vector<glm::vec2> radii; // parallel to positions when multi
		for (size_t ei = 0; ei < positions.size(); ++ei)
			radii.emplace_back(10.0f, 5.0f);
		if (positions.empty()) {
			glm::vec3 bmin(1e30f), bmax(-1e30f);
			for (uint32_t k = 0; k < traceScene.Bvh().TriangleCount(); ++k) {
				glm::vec3 v0, v1, v2;
				traceScene.Bvh().TriangleVertices(k, v0, v1, v2);
				bmin = glm::min(bmin, glm::min(v0, glm::min(v1, v2)));
				bmax = glm::max(bmax, glm::max(v0, glm::max(v1, v2)));
			}
			if (args.octree) {
				smvk::CubemapOctreeOptions oo{};
				oo.maxCellSize = args.octreeMax;
				oo.minCellSize = args.octreeMin;
				oo.maxDepth = args.octreeDepth;
				oo.embedDist = 0.3f;
				oo.maxProbes = 32;
				auto& bvh = traceScene.Bvh();
				smvk::CubemapOcclusionRay occ =
					[&](const glm::vec3& o, const glm::vec3& d, float m) {
						smvk::gi::TriangleBvh::Hit h{};
						return bvh.Raycast(o, d, m, h);
					};
				std::vector<smvk::CubemapPlacedProbe> placed;
				std::string olog;
				smvk::BuildOctreeProbePositions(bmin, bmax, oo, occ, placed, olog);
				std::cout << olog << std::flush;
				positions.reserve(placed.size());
				radii.reserve(placed.size());
				for (const auto& p : placed) {
					positions.push_back(p.position);
					radii.emplace_back(p.influenceRadius, p.fadeRadius);
				}
			} else {
				const glm::vec3 c = (bmin + bmax) * 0.5f;
				positions.push_back(glm::vec3(c.x, bmin.y + args.height, c.z));
				std::cout << "  no --position: capturing at bounds center ("
					<< positions.back().x << "," << positions.back().y << ","
					<< positions.back().z << ")\n";
			}
		}

		// Runtime budget: the lit shader scans at most 32 probe rows per
		// pixel (manifest parse clamps there). Extra captures still cost
		// bake time + disk and are never consumed.
		if (positions.size() > 32) {
			std::cerr << "WARNING: " << positions.size()
				<< " captures exceed the 32-probe runtime budget (only the"
				<< " first 32 manifest rows are consumed)\n";
		}

		smvk::ThreadPool pool(args.threads ? args.threads : std::thread::hardware_concurrency());

		// Debug PNG outputs next to the KTX files (like the pak cooker's
		// chart PNGs): per-capture face strips (+ mip sheets), then one
		// top-down probe map. Non-fatal by design.
		const std::filesystem::path cubePngDir =
			std::filesystem::path(args.outDir) / "textures/cubemaps";
		if (args.png) {
			std::error_code dcEc;
			std::filesystem::create_directories(cubePngDir, dcEc);
		}
		std::vector<glm::vec3> pngMeans;
		pngMeans.reserve(positions.size());

		baked.reserve(positions.size());
		for (size_t cap = 0; cap < positions.size(); ++cap) {
			const glm::vec3 origin = positions[cap];
			const std::string captureName = positions.size() == 1
				? args.name
				: args.name + "_" + std::to_string(cap);
			const std::string vfsPath = smvk::CubemapVfsPath(captureName);
			const std::filesystem::path file = HostPath(args.outDir, vfsPath);

			// Store TRUE radiance (same convention as PakCooker --cubemap):
			// the runtime's baked path scales by the cubemap panel
			// intensity, so a mirror reflecting a sun-lit wall matches
			// looking at that wall directly. Debug PNGs divide by
			// skyIntensity at display time to keep the readable look.
			const float cubemapScale = 1.0f;
			const float pngDisplayExposure =
				1.0f / std::max(args.skyIntensity, 1e-4f);

			// Trace 6 HDR faces (one deterministic sample per texel).
			std::vector<float> faces[smvk::kCubemapFaceCount];
			for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f)
				faces[f].assign(size_t(S) * S * 4, 0.0f);

			std::vector<std::future<void>> futures;
			for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f) {
				futures.push_back(pool.enqueue([&, f] {
					for (uint32_t y = 0; y < S; ++y) {
						for (uint32_t x = 0; x < S; ++x) {
							const float u = (float(x) + 0.5f) / float(S);
							const float v = (float(y) + 0.5f) / float(S);
							const smvk::CubemapDir d = smvk::FaceUvToDirection(
								smvk::CubemapFace(f), u, v);
							glm::vec3 dir(d.x, d.y, d.z);
							dir = glm::normalize(dir);
						// Deterministic per-texel seed: independent of
						// thread scheduling (same seed -> same pixels).
						const uint64_t seed = (uint64_t(cap + 1) << 48)
							^ (uint64_t(f) << 40)
							^ (uint64_t(y) * 4099ull + uint64_t(x) * 130963ull + 1ull);
						smvk::gi::Pcg32 rng(seed);
						// spp averaged transport paths: the NEE sun/spot
						// term is analytic, but the bounce field is a
						// Monte-Carlo estimate — 1 spp reads as speckle,
						// not the GI the lightmapped scene shows.
						glm::vec3 L(0.0f);
						for (uint32_t s = 0; s < args.spp; ++s) {
							smvk::gi::TriangleBvh::Hit h;
							if (!traceScene.Bvh().Raycast(origin, dir, 1.0e5f, h)) {
								L += traceScene.SkyRadiance(dir);
							} else {
								L += traceScene.TraceIncoming(origin, dir, rng, args.bounces);
							}
						}
						L *= cubemapScale / float(args.spp);
						float* px = &faces[f][(size_t(y) * S + x) * 4];
							px[0] = L.x;
							px[1] = L.y;
							px[2] = L.z;
							px[3] = 1.0f;
						}
					}
				}));
			}
			for (auto& fu : futures)
				fu.get();

			std::vector<std::vector<std::vector<float>>> chains;
			chains.reserve(smvk::kCubemapFaceCount);
			const bool ggx = args.mips && args.prefilter == "ggx";
			if (ggx) {
				// GGX specular prefilter (one face per worker; the
				// capture futures above already joined, so the pool
				// is free).
				smvk::CubemapBaseFaces base;
				for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f)
					base[f] = faces[f];
				chains.resize(smvk::kCubemapFaceCount);
				futures.clear();
				for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f) {
					futures.push_back(pool.enqueue([&, f] {
						smvk::PrefilterCubemapFaceGGX(base, S,
							smvk::CubemapFace(f), expectedLevels,
							args.prefilterRays, chains[f]);
					}));
				}
				for (auto& fu : futures)
					fu.get();
			}
			else {
				for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f) {
					if (args.mips)
						chains.push_back(BuildFloatMipChain(faces[f], S, S));
					else
						chains.push_back({ faces[f] });
				}
			}

			std::string werr;
			if (!WriteCubemapKtx2(file, chains, S, S, expectedLevels, werr)) {
				std::cerr << "FAILED " << vfsPath << ": " << werr << std::endl;
				return 1;
			}
			const auto sizeOnDisk = std::filesystem::file_size(file);
			baked.push_back(Baked{ vfsPath, file, sizeOnDisk });
			std::cout << "baked " << vfsPath << "  " << S << "x" << S
				<< "x6  levels=" << expectedLevels
				<< " filter=" << (ggx ? "ggx" : "box")
				<< "  origin=(" << origin.x << "," << origin.y << "," << origin.z << ")"
				<< "  " << sizeOnDisk << " B" << std::endl;

			// Debug PNGs from the same chains the KTX encodes (comparing
			// these against the runtime view separates bake/encode faults
			// from load/sample faults). Non-fatal by design.
			glm::vec3 capMean = pakcook::CubemapCaptureMean(chains, S);
			pngMeans.push_back(capMean);
			if (pakcook::CubemapMeanLuma(capMean) < 1e-6f) {
				std::cerr << "WARNING baked '" << captureName
					<< "' mean ~0 (origin embedded or fully occluded?) - check "
					<< captureName << ".png" << std::endl;
			}
			if (args.png) {
				std::string perr;
				const std::string facesPath =
					(cubePngDir / (captureName + ".png")).generic_string();
				if (pakcook::WriteCubemapFacesPng(chains, S, pngDisplayExposure, facesPath, perr)) {
					std::cout << "baked(png) " << facesPath
						<< " (mean luma "
						<< pakcook::CubemapMeanLuma(capMean) << ")" << std::endl;
				} else {
					std::cerr << "FAILED png write: " << perr << std::endl;
				}
				if (args.pngMips) {
					const std::string mipsPath =
						(cubePngDir / (captureName + "_mips.png")).generic_string();
					if (!pakcook::WriteCubemapMipsPng(chains, S, pngDisplayExposure, mipsPath, perr))
						std::cerr << "FAILED mips png write: " << perr << std::endl;
				}
			}
		}

		// Multi-probe placement manifest (grid mode): the runtime cannot
		// guess capture positions from KTX blobs alone. Same text format
		// the pak cook embeds; single captures need none (the runtime
		// centers a global probe itself). Positions are cook/world meters
		// (trace BVH space = World::ComputeSceneBounds space).
		if (positions.size() > 1) {
			std::vector<smvk::CubemapManifestEntry> rows;
			rows.reserve(positions.size());
			for (size_t cap = 0; cap < positions.size(); ++cap) {
				const std::string captureName = args.name + "_" + std::to_string(cap);
				smvk::CubemapManifestEntry e{};
				e.vfsPath = smvk::CubemapVfsPath(captureName);
				e.position = positions[cap];
				e.influenceRadius = radii.size() == positions.size() ? radii[cap].x : 10.0f;
				e.fadeRadius = radii.size() == positions.size() ? radii[cap].y : 5.0f;
				rows.push_back(std::move(e));
			}
			const std::string text = smvk::CubemapManifestSerialize(rows);
			const std::filesystem::path mfile =
				HostPath(args.outDir, smvk::CubemapManifestVfsPath(args.name));
			std::error_code mec;
			std::filesystem::create_directories(mfile.parent_path(), mec);
			std::ofstream mf(mfile, std::ios::binary | std::ios::trunc);
			if (!mf || !(mf << text)) {
				std::cerr << "FAILED manifest write: " << mfile << std::endl;
				return 1;
			}
			std::cout << "baked(manifest) "
				<< smvk::CubemapManifestVfsPath(args.name)
				<< "  probes=" << rows.size() << std::endl;
			baked.push_back(Baked{
				smvk::CubemapManifestVfsPath(args.name), mfile,
				text.size(), true });
		}

		// Top-down placement map (positions + mean colors + influence
		// rings): "is there a cubemap near X?" without launching the app.
		// Non-fatal by design.
		if (args.png && !positions.empty()
			&& pngMeans.size() == positions.size()) {
			glm::vec3 mmin(1e30f), mmax(-1e30f);
			for (const glm::vec3& p : positions) {
				mmin = glm::min(mmin, p);
				mmax = glm::max(mmax, p);
			}
			for (int a = 0; a < 3; ++a) {
				if (mmax[a] - mmin[a] < 2.0f) {
					mmin[a] -= 1.0f;
					mmax[a] += 1.0f;
				}
			}
			std::vector<float> influences;
			influences.reserve(positions.size());
			for (size_t i = 0; i < positions.size(); ++i)
				influences.push_back(
					radii.size() == positions.size() ? radii[i].x : 10.0f);
			const std::string mapPath =
				(cubePngDir / (args.name + "_probes.png")).generic_string();
			std::string merr;
			if (pakcook::WriteCubemapProbesPng(positions, pngMeans, influences,
					mmin, mmax, mapPath, merr)) {
				std::cout << "baked(probes-map) " << mapPath << std::endl;
			} else {
				std::cerr << "FAILED probes map write: " << merr << std::endl;
			}
		}

		const auto t1 = std::chrono::steady_clock::now();
		std::cout << "CubemapBaker capture total: "
			<< std::chrono::duration<double>(t1 - t0).count() << "s\n";
	}

	// Optional texture-only pak over the baked bytes (capability demo:
	// mount the pak at runtime and PakBackend serves the same VFS paths).
	if (!args.pak.empty()) {
		smvk::PakWriter writer;
		for (const Baked& b : baked) {
			auto bytes = ReadFileBytes(b.file);
			if (!bytes) {
				std::cerr << "FAILED pak read-back: " << b.file << std::endl;
				return 1;
			}
			std::vector<uint8_t> payload = smvk::BuildTexturePayload(
				b.vfsPath, -1, -1, -1, -1, std::span<const uint8_t>(*bytes));
			if (!writer.AddBlob(smvk::PakAssetKind::Texture, b.vfsPath,
				std::move(payload))) {
				std::cerr << "FAILED pak AddBlob: " << b.vfsPath << std::endl;
				return 1;
			}
		}
		const std::vector<uint8_t> pak = writer.Finalize();
		if (!WriteFileBytes(args.pak, pak)) {
			std::cerr << "FAILED writing pak: " << args.pak << std::endl;
			return 1;
		}
		std::cout << "paked " << args.pak << "  " << pak.size()
			<< " B (" << baked.size() << " textures)" << std::endl;
	}

	if (args.verify) {
		size_t failures = 0;

		// 1. Written files decode with the expected cube layout.
		// Manifest rows are text, not KTX: parse them instead.
		for (const Baked& b : baked) {
			if (b.isManifest) {
				auto disk = ReadFileBytes(b.file);
				std::string merr;
				std::vector<smvk::CubemapManifestEntry> rows;
				if (!disk || !smvk::CubemapManifestParse(
						std::string_view(reinterpret_cast<const char*>(disk->data()), disk->size()),
						rows, merr)) {
					std::cerr << "verify FAILED manifest " << b.vfsPath << ": "
						<< merr << std::endl;
					++failures;
				}
				continue;
			}
			std::string verr;
			if (!VerifyCubemapKtx2(b.file, expectedLevels, !args.testPattern, verr)) {
				std::cerr << "verify FAILED " << b.vfsPath << ": "
					<< verr << std::endl;
				++failures;
			}
		}

		// 2. Loose serving: the output dir mounted as the app mounts
		//    engine_resources must serve every cubemap VFS path.
		{
			smvk::OverlayVfs vfs;
			vfs.Mount(std::make_shared<smvk::LooseBackend>(args.outDir), 0, "baked");
			for (const Baked& b : baked) {
				auto bytes = vfs.TryRead(b.vfsPath);
				if (!bytes) {
					std::cerr << "verify FAILED loose read: " << b.vfsPath << std::endl;
					++failures;
					continue;
				}
				auto disk = ReadFileBytes(b.file);
				if (!disk || disk->size() != bytes->size() ||
					std::memcmp(disk->data(), bytes->data(), bytes->size()) != 0) {
					std::cerr << "verify FAILED loose bytes differ: "
						<< b.vfsPath << std::endl;
					++failures;
				}
			}
		}

		// 3. Pak serving: PakBackend must serve the same paths from the pak.
		if (!args.pak.empty()) {
			auto image = ReadFileBytes(args.pak);
			std::string pakErr;
			auto pakImage = std::make_shared<const std::vector<uint8_t>>(
				image ? std::move(*image) : std::vector<uint8_t>{});
			auto backend = smvk::PakBackend::Open(pakImage, args.pak, pakErr);
			if (!backend) {
				std::cerr << "verify FAILED pak open: " << pakErr << std::endl;
				++failures;
			}
			else {
				for (const Baked& b : baked) {
					auto bytes = backend->TryRead(b.vfsPath);
					if (!bytes) {
						std::cerr << "verify FAILED pak read: "
							<< b.vfsPath << std::endl;
						++failures;
					}
				}
			}
		}

		if (failures) {
			std::cerr << "verify: " << failures << " failure(s)" << std::endl;
			return 1;
		}
		std::cout << "verify: " << baked.size() << " file(s) OK"
			<< (args.pak.empty() ? "" : " + pak OK") << std::endl;
	}

	return 0;
}
