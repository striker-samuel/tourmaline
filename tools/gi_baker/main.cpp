// GiBaker: offline SG-GI probe-volume baker (GPU-free). Bakes a scene's
// indirect lighting into an adaptive spherical-Gaussian probe volume
// (Engine/GI: path tracer + adaptive brick placement + error-driven
// refinement) and writes the LZ4-compressed .sggi the runtime App loads
// from content/gi/<scene>.sggi.
//
//   GiBaker --content-dir F:/content --scene sponza
//           [--out F:/content/gi/sponza.sggi]
//           [--rays 128] [--bounces 3] [--error-rays 32]
//           [--root 8.0] [--min-brick 0.25] [--max-probes 262144]
//           [--tolerance 0.12] [--waves 8] [--threads 0]
//           [--sun-azimuth 30] [--sun-elevation 55] [--sun-intensity 10000]
//           [--sky-intensity 600] [--textures on|off] [--verify on|off]
//
// Scene resolution mirrors the runtime SceneRegistry: --scene sponza |
// bistro | bistro-niagara maps to the same glTF the App imports (the bake
// must see the exact runtime geometry), or pass --in-vfs <model.gltf> for
// arbitrary scenes. Light setup defaults match the runtime environment
// defaults; the bake captures sky + sun bounce + emissive geometry, and
// EXCLUDES direct sun (the runtime's cascaded shadow maps own it) and the
// dynamic forward+ point/spot lights.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/compression.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/probe_volume.h>
#include <Engine/Utils/gltf.h>
#include <Engine/Vfs/vfs.h>

#include "ktx.h"

namespace {

struct Args {
	std::string contentDir = "content";
	std::string scene;    // sponza | bistro | bistro-niagara
	std::string inVfs;    // arbitrary model (overrides --scene input)
	std::string out;      // output host path (default <content>/gi/<scene>.sggi)
	uint32_t rays = 128;
	uint32_t bounces = 3;
	uint32_t errorRays = 32;
	float root = 8.0f;
	float minBrick = 0.25f;
	uint32_t maxProbes = 262144;
	float tolerance = 0.12f;
	float nearFactor = 1.0f;
	uint32_t waves = 8;
	uint32_t threads = 0; // 0 = hardware concurrency
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
	bool textures = true;
	bool emissive = true;
	bool verify = true;
};

bool ParseArgs(int argc, char** argv, Args& out) {
	auto need = [&](std::string& dst, int& i) {
		if (i + 1 >= argc) {
			std::cerr << "Missing value for " << argv[i] << std::endl;
			return false;
		}
		dst = argv[++i];
		return true;
	};
	auto needF = [&](float& dst, int& i) {
		std::string v;
		if (!need(v, i)) return false;
		dst = std::stof(v);
		return true;
	};
	auto needU = [&](uint32_t& dst, int& i) {
		std::string v;
		if (!need(v, i)) return false;
		dst = static_cast<uint32_t>(std::stoul(v));
		return true;
	};
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--content-dir") { if (!need(out.contentDir, i)) return false; }
		else if (a == "--scene") { if (!need(out.scene, i)) return false; }
		else if (a == "--in-vfs") { if (!need(out.inVfs, i)) return false; }
		else if (a == "--out") { if (!need(out.out, i)) return false; }
		else if (a == "--rays") { if (!needU(out.rays, i)) return false; }
		else if (a == "--bounces") { if (!needU(out.bounces, i)) return false; }
		else if (a == "--error-rays") { if (!needU(out.errorRays, i)) return false; }
		else if (a == "--root") { if (!needF(out.root, i)) return false; }
		else if (a == "--min-brick") { if (!needF(out.minBrick, i)) return false; }
		else if (a == "--max-probes") { if (!needU(out.maxProbes, i)) return false; }
		else if (a == "--tolerance") { if (!needF(out.tolerance, i)) return false; }
		else if (a == "--near-factor") { if (!needF(out.nearFactor, i)) return false; }
		else if (a == "--waves") { if (!needU(out.waves, i)) return false; }
		else if (a == "--threads") { if (!needU(out.threads, i)) return false; }
		else if (a == "--sun-azimuth") { if (!needF(out.sunAzimuth, i)) return false; }
		else if (a == "--sun-elevation") { if (!needF(out.sunElevation, i)) return false; }
		else if (a == "--sun-intensity") { if (!needF(out.sunIntensity, i)) return false; }
		else if (a == "--sky-intensity") { if (!needF(out.skyIntensity, i)) return false; }
		else if (a == "--textures") {
			std::string v;
			if (!need(v, i)) return false;
			out.textures = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--emissive") {
			std::string v;
			if (!need(v, i)) return false;
			out.emissive = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--verify") {
			std::string v;
			if (!need(v, i)) return false;
			out.verify = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--help" || a == "-h") {
			return false;
		}
		else {
			std::cerr << "Unknown arg: " << a << std::endl;
			return false;
		}
	}
	if (out.inVfs.empty() && out.scene.empty()) {
		std::cerr << "Required: --scene <id> or --in-vfs <model.gltf>" << std::endl;
		return false;
	}
	return true;
}

void Usage() {
	std::cout << "GiBaker --content-dir content --scene sponza\n"
		<< "         [--out content/gi/sponza.sggi] [--in-vfs models/X/x.gltf]\n"
		<< "         [--rays 128] [--bounces 3] [--error-rays 32]\n"
		<< "         [--root 8.0] [--min-brick 0.25] [--max-probes 262144]\n"
		<< "         [--tolerance 0.12] [--near-factor 1.0] [--waves 8] [--threads 0]\n"
		<< "         [--sun-azimuth 30] [--sun-elevation 55] [--sun-intensity 10000]\n"
		<< "         [--sky-intensity 600] [--textures on|off] [--emissive on|off]\n"
		<< "         [--verify on|off]\n";
}

// Scene-id -> glTF vfs path (mirrors app::SceneRegistry).
std::optional<std::string> SceneGltf(const std::string& id) {
	if (id == "sponza") return std::string("models/Sponza/sponza.gltf");
	if (id == "bistro") return std::string("models/Bistro/bistro.gltf");
	if (id == "bistro-niagara") return std::string("models/BistroNiagara/bistro.gltf");
	if (id == "scanlands") return std::string("models/Scanlands/scanlands.gltf");
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// KTX2 texel averaging: decode each texture on the CPU (libktx), average a
// small mip in linear space and feed the result to the bake as a per-
// material albedo/emissive tint (scenes carry most of their color in
// textures; factors are often white).
// ---------------------------------------------------------------------------

struct KtxGuard {
	ktxTexture2* tex = nullptr;
	explicit KtxGuard(ktxTexture2* t) : tex(t) {}
	~KtxGuard() { if (tex) ktxTexture2_Destroy(tex); }
	KtxGuard(const KtxGuard&) = delete;
	KtxGuard& operator=(const KtxGuard&) = delete;
};

float SrgbToLinear(float c) {
	return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::optional<glm::vec3> AverageKtxLinear(const std::vector<uint8_t>& bytes) {
	ktxTexture2* raw = nullptr;
	if (ktxTexture2_CreateFromMemory(
			reinterpret_cast<const ktx_uint8_t*>(bytes.data()),
			static_cast<ktx_size_t>(bytes.size()),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw) != KTX_SUCCESS)
		return std::nullopt;
	KtxGuard guard(raw);

	if (ktxTexture2_NeedsTranscoding(raw)) {
		if (ktxTexture2_TranscodeBasis(raw, KTX_TTF_RGBA32, 0) != KTX_SUCCESS)
			return std::nullopt;
	}
	const ktx_uint32_t baseWidth = raw->baseWidth;
	const ktx_uint32_t baseHeight = std::max<ktx_uint32_t>(raw->baseHeight, 1);
	const ktx_uint32_t levels = std::max<ktx_uint32_t>(raw->numLevels, 1);
	// Pick a small level (<= 64 px) so averaging is cheap.
	ktx_uint32_t level = 0;
	while (level + 1 < levels && (baseWidth >> (level + 1)) > 64)
		++level;
	const ktx_uint32_t w = std::max<ktx_uint32_t>(baseWidth >> level, 1);
	const ktx_uint32_t h = std::max<ktx_uint32_t>(baseHeight >> level, 1);

	ktx_size_t offset = 0;
	if (ktxTexture2_GetImageOffset(raw, level, 0, 0, &offset) != KTX_SUCCESS)
		return std::nullopt;
	const ktx_size_t rowPitch = ktxTexture_GetRowPitch(
		reinterpret_cast<ktxTexture*>(raw), level);
	const ktx_uint32_t components = ktxTexture2_GetNumComponents(raw);
	if (components < 3 || rowPitch < ktx_size_t(w) * components)
		return std::nullopt;

	const bool srgb = ktxTexture2_GetTransferFunction_e(raw) == KHR_DF_TRANSFER_SRGB;
	const uint8_t* base = reinterpret_cast<const uint8_t*>(raw->pData) + offset;
	glm::dvec3 sum(0.0);
	uint64_t count = 0;
	for (ktx_uint32_t y = 0; y < h; ++y) {
		const uint8_t* row = base + y * rowPitch;
		for (ktx_uint32_t x = 0; x < w; ++x) {
			const uint8_t* px = row + x * components;
			auto chan = [&](uint32_t c) {
				const float v = float(px[c]) / 255.0f;
				return srgb ? SrgbToLinear(v) : v;
			};
			sum += glm::dvec3(chan(0), chan(1), chan(2));
			++count;
		}
	}
	if (count == 0)
		return std::nullopt;
	return glm::vec3(sum / double(count));
}

} // namespace

int main(int argc, char** argv) {
	Args args;
	if (!ParseArgs(argc, argv, args)) {
		Usage();
		return 2;
	}

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
				<< "' (sponza | bistro | bistro-niagara) or use --in-vfs" << std::endl;
			return 2;
		}
		inVfs = *mapped;
	}
	if (args.out.empty()) {
		if (sceneId.empty())
			sceneId = "scene";
		args.out = (std::filesystem::path(args.contentDir) / "gi"
			/ (sceneId + ".sggi")).generic_string();
	}

	std::cout << "GiBaker: scene '" << sceneId << "' from " << inVfs
		<< " -> " << args.out << "\n";

	// GPU-free cook-mode Cluster (same import as PakCooker / the runtime's
	// glTF fallback; full-res geometry, retained CPU uploads).
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	try {
		GltfToClusterFromVfs(nullptr, vfs, inVfs, *cluster,
			smvk::MeshOptConfig{}, smvk::MeshletConfig{});
	} catch (const std::exception& e) {
		std::cerr << "Import failed: " << e.what() << std::endl;
		return 1;
	}

	// Optional texture averaging (role-agnostic per-texture average:
	// baseColor tints albedo, emissive tints emission, metallicRoughness
	// supplies the effective metallic like the runtime shader).
	smvk::gi::GiSceneBuildOptions buildOpts{};
	if (args.textures) {
		uint32_t averaged = 0;
		for (const auto& [uri, handle] : cluster->ListTextureUris()) {
			const std::vector<uint8_t>* src = cluster->GetTextureSource(uri);
			if (!src || src->empty())
				continue;
			std::optional<glm::vec3> avg = AverageKtxLinear(*src);
			if (!avg)
				continue;
			buildOpts.texelAvg[handle.index()] = *avg;
			++averaged;
		}
		std::cout << "  textures: averaged " << averaged << " ("
			<< cluster->ListTextureUris().size() << " uris)\n";
	}

	// Environment defaults mirror the runtime EnvironmentSettings so the
	// bake matches what the app shows when the GI volume is loaded.
	buildOpts.env.sunAzimuthDeg = args.sunAzimuth;
	buildOpts.env.sunElevationDeg = args.sunElevation;
	buildOpts.env.sunIntensity = args.sunIntensity;
	buildOpts.env.skyIntensity = args.skyIntensity;
	buildOpts.includeEmissive = args.emissive;

	smvk::gi::GiTraceScene traceScene;
	std::string err;
	smvk::gi::BuildTraceSceneFromCluster(*cluster, buildOpts, traceScene, err);
	if (!err.empty()) {
		std::cerr << "Scene build failed: " << err << std::endl;
		return 1;
	}
	std::cout << "  trace scene: " << traceScene.Bvh().TriangleCount()
		<< " tris, " << traceScene.materials.size() << " materials\n";

	smvk::gi::SgGiBakeConfig cfg{};
	cfg.raysPerProbe = args.rays;
	cfg.maxBounces = args.bounces;
	cfg.errorRaysPerProbe = args.errorRays;
	cfg.rootBrickSize = args.root;
	cfg.minBrickSize = args.minBrick;
	cfg.maxProbes = args.maxProbes;
	cfg.errorTolerance = args.tolerance;
	cfg.nearSurfaceFactor = args.nearFactor;
	cfg.maxRefineWaves = args.waves;
	cfg.threadCount = args.threads;

	smvk::gi::SgGiVolume volume;
	smvk::gi::SgGiBakeStats stats;
	if (!smvk::gi::BakeSgGiVolume(traceScene, cfg, volume, stats,
			[](const char* line) { std::cout << line; })) {
		std::cerr << "Bake failed" << std::endl;
		return 1;
	}

	{
		std::filesystem::path out(args.out);
		std::error_code ec;
		std::filesystem::create_directories(out.parent_path(), ec);
	}
	std::string writeErr;
	if (!volume.WriteFile(args.out, writeErr)) {
		std::cerr << "Write failed: " << writeErr << std::endl;
		return 1;
	}

	std::cout << "Wrote " << args.out << " ("
		<< std::filesystem::file_size(args.out) << " B on disk)\n";
	std::cout << "  probes=" << stats.probeCount
		<< " waves=" << stats.refineWaves
		<< " paths=" << stats.pathsTraced
		<< " seconds=" << stats.seconds << "\n";
	for (uint32_t lvl = 0; lvl < smvk::gi::kSgGiMaxLevels; ++lvl)
		if (stats.probesPerLevel[lvl])
			std::cout << "  level " << lvl << ": " << stats.probesPerLevel[lvl] << " probes\n";

	// Self-check: reload through the production reader and spot-check.
	if (args.verify) {
		smvk::gi::SgGiVolume reloaded;
		std::string verr;
		if (!smvk::gi::SgGiVolume::ReadFile(args.out, reloaded, verr)) {
			std::cerr << "Verify: reload failed: " << verr << std::endl;
			return 1;
		}
		if (reloaded.probes.size() != volume.probes.size()
			|| reloaded.levelCount != volume.levelCount) {
			std::cerr << "Verify: round-trip mismatch\n";
			return 1;
		}
		size_t mismatches = 0;
		for (size_t i = 0; i < volume.probes.size(); ++i) {
			const auto& a = volume.probes[i];
			const auto& b = reloaded.probes[i];
			if (glm::length(a.position - b.position) > 1e-4f
				|| std::abs(a.radius - b.radius) > 1e-3f
				|| a.level != b.level)
				++mismatches;
		}
		std::cout << "  verify: " << reloaded.probes.size()
			<< " probes reloaded, " << mismatches << " mismatches"
			<< (mismatches ? " BAD!" : " OK") << "\n";
		if (mismatches)
			return 1;
	}

	const auto t1 = std::chrono::steady_clock::now();
	std::cout << "GiBaker total: "
		<< std::chrono::duration<double>(t1 - t0).count() << "s\n";
	return 0;
}
// (set include)
