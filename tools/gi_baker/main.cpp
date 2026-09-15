// GiBaker: offline SG-GI probe-volume baker (GPU-free). Bakes a scene's
// indirect lighting into an adaptive spherical-Gaussian probe volume
// (Engine/GI: path tracer + adaptive brick placement + error-driven
// refinement) and writes the LZ4-compressed .sggi the runtime App loads
// from content/gi/<scene>.sggi.
//
//   GiBaker --content-dir F:/content --scene sponza
//           [--out F:/content/gi/sponza.sggi]
//           [--rays 1024] [--bounces 3] [--error-rays 32]
//           [--root 8.0] [--min-brick 0.25] [--max-probes 262144]
//           [--tolerance 0.12] [--waves 8] [--threads 0]
//           [--sun-azimuth 30] [--sun-elevation 55] [--sun-intensity 10000]
//           [--sky-intensity 600] [--textures on|off] [--verify on|off]
//
// Scene resolution mirrors the runtime SceneRegistry: --scene sponza |
// bistro | bistro-niagara maps to the same glTF the App imports (the bake
// must see the exact runtime geometry), or pass --in-vfs <model.gltf> for
// arbitrary scenes. Light setup defaults match the runtime environment
// defaults; the bake captures the COMPLETE ambient: bounced sun (NEE at
// every vertex, incl. the primary hit) + the analytic sky on open-sky
// rays (--sky-intensity), and EXCLUDES direct sun (the runtime's
// cascaded shadow maps own it), emissive geometry, and the dynamic
// forward+ point/spot lights. Covered pixels take the baked source
// ALONE at runtime (the sky IBL is the uncovered-pixel fallback).
// --emissive is accepted but ignored (emissive stays runtime-only).
// Materials bake white-world (uniform albedo 0.7) unless
// --uniform-albedo off requests the textured path: per-material albedo
// on contrasty scenes prints chromatic dots the 9-lobe fit cannot
// represent (verified on Sponza).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/compression.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/probe_volume.h>
#include <Engine/GI/regular_volume.h>
#include <Engine/Utils/gltf.h>
#include <Engine/Vfs/vfs.h>


namespace {

struct Args {
	std::string contentDir = "content";
	std::string scene;    // sponza | bistro | bistro-niagara
	std::string inVfs;    // arbitrary model (overrides --scene input)
	std::string out;      // output host path (default <content>/gi/<scene>.sggi)
	uint32_t rays = 1024;
	uint32_t bounces = 3;
	uint32_t errorRays = 32;
	float root = 8.0f;
	float minBrick = 0.5f;
	uint32_t maxProbes = 262144;
	float tolerance = 0.12f;
	float nearFactor = 1.0f;
	uint32_t waves = 8;
	uint32_t threads = 0; // 0 = hardware concurrency
	uint32_t diffusion = 0; // placement jitter+repulsion (keep 0: post-bake
	                      // moves invalidate baked radiance)
	uint32_t smoothingPasses = 0; // Laplacian radiance passes (keep 0: smoothing
	                              // without visibility leaks across walls)
	float support = 1.0f; // support radius = factor * brickSize (1.0 =
	                       // partition of unity + ~27 probes/cell; higher
	                       // stuffs cells to the cap and drops probes)
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
	bool textures = true;
	bool emissive = true;
	bool verify = true;
	std::string analyze; // .sggi host path: inspect instead of baking
	// White-world albedo: default "0.7" (single known albedo everywhere).
	// "off" restores the textured bake (per-material albedo from factors
	// + KTX averages; known to print chromatic dots on contrasty scenes).
	// "r,g,b" bakes a custom uniform tint instead of grey.
	std::string uniformAlbedo = "0.7";
	// Regular-lattice prototype (Ready-at-Dawn style, .sgrv alongside
	// the adaptive .sggi): fixed OBB grid, diagonal projection, hardware
	// trilinear at runtime. Empty regularOut = no regular bake.
	std::string regularOut;
	bool outExplicit = false;
	std::string regMin, regMax;   // "x,y,z" world AABB (axis-aligned OBB)
	std::string regDims = "32,16,32"; // "nx,ny,nz"
	float regSpacing = 0.0f;      // voxel spacing override (m); 0 = use regDims
	std::string regPreset;        // bistro-bar | bistro-street | scene
	bool printBounds = false;     // print trace-scene AABB and exit
	bool scanInterior = false;    // enclosure map + enclosed-component boxes
	// Static spot lights for the bake (same spec as PakCooker --spot).
	std::vector<std::string> spots;
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
		else if (a == "--out") { if (!need(out.out, i)) return false; out.outExplicit = true; }
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
		else if (a == "--diffusion") { if (!needU(out.diffusion, i)) return false; }
		else if (a == "--smoothing-passes") { if (!needU(out.smoothingPasses, i)) return false; }
		else if (a == "--support") { if (!needF(out.support, i)) return false; }
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
		else if (a == "--analyze") { if (!need(out.analyze, i)) return false; }
		else if (a == "--uniform-albedo") { if (!need(out.uniformAlbedo, i)) return false; }
		else if (a == "--regular-out") { if (!need(out.regularOut, i)) return false; }
		else if (a == "--reg-min") { if (!need(out.regMin, i)) return false; }
		else if (a == "--reg-max") { if (!need(out.regMax, i)) return false; }
		else if (a == "--reg-dims") { if (!need(out.regDims, i)) return false; }
		else if (a == "--reg-preset") { if (!need(out.regPreset, i)) return false; }
		else if (a == "--spot") { std::string v; if (!need(v, i)) return false; out.spots.push_back(v); }
		else if (a == "--reg-spacing") {
			std::string v;
			if (!need(v, i)) return false;
			out.regSpacing = std::stof(v);
		}
		else if (a == "--print-bounds") { out.printBounds = true; }
		else if (a == "--scan-interior") { out.scanInterior = true; }
		else if (a == "--help" || a == "-h") {
			return false;
		}
		else {
			std::cerr << "Unknown arg: " << a << std::endl;
			return false;
		}
	}
	if (out.analyze.empty() && out.inVfs.empty() && out.scene.empty()) {
		std::cerr << "Required: --scene <id>, --in-vfs <model.gltf>, or --analyze <file.sggi>" << std::endl;
		return false;
	}
	return true;
}

void Usage() {
	std::cout << "GiBaker --content-dir content --scene sponza\n"
		<< "         [--out content/gi/sponza.sggi] [--in-vfs models/X/x.gltf]\n"
		<< "         [--rays 1024] [--bounces 3] [--error-rays 32]\n"
		<< "         [--root 8.0] [--min-brick 0.25] [--max-probes 1048576]\n"
		<< "         [--tolerance 0.12] [--near-factor 1.0] [--waves 8] [--threads 0]\n"
		<< "         [--diffusion 0] [--smoothing-passes 0] [--support 1.25]\n"
		<< "         [--sun-azimuth 30] [--sun-elevation 55] [--sun-intensity 10000]\n"
		<< "         [--sky-intensity 600] [--textures on|off] [--emissive on|off]\n"
		<< "         [--verify on|off] [--uniform-albedo off|0.7|r,g,b]\n"
		<< "         [--spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]\n"
		<< "         (bakes use REAL materials by default; sun + static spots\n"
		<< "         via NEE; transmissive meshes pass rays through, tinted.\n"
		<< "         --uniform-albedo overrides with a known flat albedo)\n"
		<< "GiBaker --analyze <file.sggi>: dump probe amplitude stats and a\n"
		<< "         CPU-sampled irradiance transect (bake diagnostics).\n"
		<< "GiBaker --scene bistro --regular-out content/gi/bistro_bar.sgrv\n"
		<< "         --reg-preset bistro-bar|bistro-street|scene\n"
		<< "         [--reg-min x,y,z --reg-max x,y,z] [--reg-dims nx,ny,nz]\n"
		<< "         [--reg-spacing 0.25]: regular-lattice (.sgrv) prototype bake\n"
		<< "         (fixed OBB grid, diagonal SG projection, no relocation).\n"
		<< "         2D SG lightmaps are cooked with the pak: use\n"
		<< "         PakCooker --lightmap on (writes gi/<scene>.sglm itself).\n"
		<< "GiBaker --scene bistro --print-bounds | --scan-interior:\n"
		<< "         scene AABB / enclosure map to place bar/street boxes.\n";
}

// Regular-volume inspector (.sgrv): coefficient stats + a trilinear
// irradiance transect, mirroring AnalyzeVolume for the adaptive format.
int AnalyzeRegularVolume(const std::string& path) {
	using namespace smvk::gi;
	SgRegularVolume vol;
	std::string err;
	if (!SgRegularVolume::ReadFile(path, vol, err)) {
		std::cerr << "Analyze: load failed: " << err << std::endl;
		return 1;
	}
	auto lum = [](const glm::vec3& c) {
		return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
	};
	const uint64_t n = vol.VoxelCount();
	std::cout << "analyze-regular: " << path << "\n  voxels=" << n
		<< " dims=" << vol.obb.dims[0] << "x" << vol.obb.dims[1] << "x" << vol.obb.dims[2]
		<< " center=(" << vol.obb.center.x << "," << vol.obb.center.y << "," << vol.obb.center.z << ")"
		<< " half=(" << vol.obb.halfExtents.x << "," << vol.obb.halfExtents.y << "," << vol.obb.halfExtents.z << ")"
		<< " spacing=(" << vol.obb.VoxelSpacing().x << "," << vol.obb.VoxelSpacing().y
		<< "," << vol.obb.VoxelSpacing().z << ") gpuMiB="
		<< (double(vol.GpuBytes()) / (1024.0 * 1024.0)) << "\n";
	size_t nanVox = 0;
	double sumLum = 0.0, maxLum = 0.0;
	for (uint64_t v = 0; v < n; ++v) {
		double vl = 0.0;
		bool bad = false;
		for (uint32_t i = 0; i < kSgLobeCount; ++i) {
			const glm::vec3& a = vol.coeffs[size_t(v) * kSgLobeCount + i];
			for (int c = 0; c < 3; ++c)
				if (!std::isfinite(a[c]))
					bad = true;
			vl += lum(a);
		}
		vl /= kSgLobeCount;
		if (bad) { ++nanVox; continue; }
		sumLum += vl;
		maxLum = std::max(maxLum, vl);
	}
	std::cout << "  meanLum=" << (n ? sumLum / double(n) : 0.0)
		<< " maxLum=" << maxLum << " nonfiniteVoxels=" << nanVox << "\n";
	// Center transect along X at mid-Y/Z: adjacent samples share 7/8 of
	// their voxels, so cliffs here are bake faults, not interpolation.
	const glm::vec3 up(0, 1, 0);
	const glm::vec3 lo = vol.obb.center - vol.obb.halfExtents;
	const glm::vec3 hi = vol.obb.center + vol.obb.halfExtents;
	const float y = (lo.y + hi.y) * 0.5f, z = (lo.z + hi.z) * 0.5f;
	double prevLum = -1.0, maxStep = 0.0, prevX = lo.x, worstX = lo.x;
	double worstA = 0.0, worstB = 0.0, minLum = 1e30, maxLumT = 0.0;
	size_t steps = 0, uncovered = 0;
	for (float x = lo.x; x <= hi.x; x += 0.25f) {
		glm::vec3 amps[kSgLobeCount];
		++steps;
		if (!vol.Sample(glm::vec3(x, y, z), amps)) { ++uncovered; continue; }
		const double l = lum(SgDiffuseIrradiance(amps, up));
		minLum = std::min(minLum, l);
		maxLumT = std::max(maxLumT, l);
		if (prevLum >= 0.0) {
			const double step = std::abs(l - prevLum) / std::max(prevLum, 1e-3);
			if (step > maxStep) {
				maxStep = step;
				worstX = x;
				worstA = prevLum;
				worstB = l;
			}
		}
		prevLum = l;
		prevX = x;
	}
	(void)prevX;
	std::cout << "  transect steps=" << steps << " uncovered=" << uncovered
		<< " maxAdjacentRelStep=" << maxStep << " at x=" << worstX
		<< " (" << worstA << " -> " << worstB << ")"
		<< " minLum=" << minLum << " maxLum=" << maxLumT << "\n";
	return 0;
}

// Offline volume inspector: amplitude distribution (NaN/Inf/extremes per
// level) plus a CPU Sample() irradiance transect across the volume so
// blotchy runtime output can be bisected into bake-side (extreme or
// disagreeing probes) vs GPU-side (transfer/sampling) faults.
int AnalyzeVolume(const std::string& path) {
	using namespace smvk::gi;
	SgGiVolume vol;
	std::string err;
	if (!SgGiVolume::ReadFile(path, vol, err)) {
		std::cerr << "Analyze: load failed: " << err << std::endl;
		return 1;
	}
	auto lum = [](const glm::vec3& c) {
		return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
	};
	std::cout << "analyze: " << path << "\n  probes=" << vol.probes.size()
		<< " levels=" << vol.levelCount
		<< " boundsMin=(" << vol.boundsMin.x << "," << vol.boundsMin.y << "," << vol.boundsMin.z << ")"
		<< " boundsMax=(" << vol.boundsMax.x << "," << vol.boundsMax.y << "," << vol.boundsMax.z << ")\n";

	size_t nanProbes = 0, infProbes = 0;
	double sumLum = 0.0, maxLum = 0.0;
	float minLum = std::numeric_limits<float>::max();
	std::vector<double> levelSum(kSgGiMaxLevels, 0.0);
	std::vector<double> levelMax(kSgGiMaxLevels, 0.0);
	std::vector<uint32_t> levelCount(kSgGiMaxLevels, 0);
	struct Extreme { double l; uint32_t level; glm::vec3 pos; };
	std::vector<Extreme> extremes;
	for (const SgProbe& p : vol.probes) {
		double pl = 0.0;
		bool nan = false, inf = false;
		for (uint32_t i = 0; i < kSgLobeCount; ++i) {
			for (int c = 0; c < 3; ++c) {
				const float v = p.amps[i][c];
				if (std::isnan(v)) nan = true;
				if (std::isinf(v)) inf = true;
			}
			pl += lum(p.amps[i]);
		}
		pl /= kSgLobeCount;
		if (nan) ++nanProbes;
		if (inf) ++infProbes;
		if (!nan && !inf) {
			sumLum += pl;
			maxLum = std::max(maxLum, pl);
			minLum = std::min(minLum, float(pl));
			if (p.level < kSgGiMaxLevels) {
				levelSum[p.level] += pl;
				levelMax[p.level] = std::max(levelMax[p.level], pl);
				++levelCount[p.level];
			}
			extremes.push_back({ pl, p.level, p.position });
		}
	}
	std::cout << "  meanLum=" << (vol.probes.empty() ? 0.0 : sumLum / vol.probes.size())
		<< " minLum=" << (minLum == std::numeric_limits<float>::max() ? -1.0 : minLum)
		<< " maxLum=" << maxLum
		<< " nanProbes=" << nanProbes << " infProbes=" << infProbes << "\n";
	for (uint32_t l = 0; l < vol.levelCount; ++l)
		std::cout << "  level " << l << ": n=" << levelCount[l]
			<< " mean=" << (levelCount[l] ? levelSum[l] / levelCount[l] : 0.0)
			<< " max=" << levelMax[l] << "\n";
	std::sort(extremes.begin(), extremes.end(),
		[](const Extreme& a, const Extreme& b) { return a.l > b.l; });
	std::cout << "  top extremes (lum, level, pos):\n";
	for (size_t i = 0; i < std::min<size_t>(extremes.size(), 10); ++i)
		std::cout << "    " << extremes[i].l << " L" << extremes[i].level
			<< " (" << extremes[i].pos.x << "," << extremes[i].pos.y << "," << extremes[i].pos.z << ")\n";
	// Isolation census: for each probe, same-level neighbors within
	// 1.5x its support radius. Fine probes with ~0 fine neighbors are
	// speckle-subdivision orphans: their footprint shows as a lone dot
	// against a coarser neighborhood (and vice versa). Healthy dense
	// regions have 5+ same-level neighbors.
	{
		const float cell = 1.0f;
		glm::vec3 gmin(1e30f);
		for (const SgProbe& p : vol.probes)
			gmin = glm::min(gmin, p.position);
		std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
		auto key = [&](int x, int y, int z) {
			return (uint64_t(uint32_t(x) + 512) << 42)
				| (uint64_t(uint32_t(y) + 512) << 21)
				| uint64_t(uint32_t(z) + 512);
		};
		for (uint32_t i = 0; i < vol.probes.size(); ++i) {
			const glm::vec3 d = (vol.probes[i].position - gmin) / cell;
			grid[key(int(d.x), int(d.y), int(d.z))].push_back(i);
		}
		std::vector<uint32_t> isoHist(8, 0);
		uint32_t fineIso = 0, fineTotal = 0;
		for (uint32_t i = 0; i < vol.probes.size(); ++i) {
			const SgProbe& p = vol.probes[i];
			const float r = 1.5f * p.radius;
			const glm::vec3 lo = (p.position - glm::vec3(r) - gmin) / cell;
			const glm::vec3 hi = (p.position + glm::vec3(r) - gmin) / cell;
			uint32_t same = 0;
			for (int x = int(lo.x); x <= int(hi.x); ++x)
				for (int y = int(lo.y); y <= int(hi.y); ++y)
					for (int z = int(lo.z); z <= int(hi.z); ++z) {
						auto it = grid.find(key(x, y, z));
						if (it == grid.end())
							continue;
						for (uint32_t j : it->second) {
							if (j == i || vol.probes[j].level != p.level)
								continue;
							const glm::vec3 dd = p.position - vol.probes[j].position;
							if (glm::dot(dd, dd) < r * r)
								++same;
						}
					}
			isoHist[std::min(same, 7u)]++;
			if (p.level + 2 >= vol.levelCount) { // finest two levels
				++fineTotal;
				if (same == 0)
					++fineIso;
			}
		}
		std::cout << "  isolation histogram (same-level neighbors within 1.5R):\n";
		for (int n = 0; n < 8; ++n)
			std::cout << "    n=" << n << ": " << isoHist[n] << "\n";
		std::cout << "  fine-level isolated (0 same-level neighbors): "
			<< fineIso << "/" << fineTotal << "\n";
	}
	// Zero census: probes whose mean amplitude is exactly (or near) zero
	// cannot be honest in sunlit open air (a downward ray always finds a
	// bounce). They are NaN-sanitized casualties or fit failures, and
	// their wide supports dilute (darken) large neighborhoods — suspects
	// for dark patches. Dump positions to see where they live.
	{
		uint32_t zeroCount = 0;
		std::cout << "  zero probes (meanLum < 1e-6), first 25 with level+pos:\n";
		for (const SgProbe& p : vol.probes) {
			glm::vec3 m(0.0f);
			for (uint32_t i = 0; i < kSgLobeCount; ++i)
				m += p.amps[i];
			const double pl = (0.2126 * m.x + 0.7152 * m.y + 0.0722 * m.z) / kSgLobeCount;
			if (pl < 1e-6) {
				++zeroCount;
				if (zeroCount <= 25)
					std::cout << "    L" << p.level << " (" << p.position.x << ","
						<< p.position.y << "," << p.position.z << ") r=" << p.radius << "\n";
			}
		}
		std::cout << "  zero probes total: " << zeroCount << "/" << vol.probes.size() << "\n";
	}
	// Hue census: per-probe mean-amplitude RGB, max/min channel ratio.
	// A white-world bake must be ~sun-tinted everywhere (ratio ~= 1.1);
	// ratios >> 2 mean chromatic probes that read as colored dots in the
	// albedo-free debug view (which has no local albedo to blame).
	{
		size_t over2 = 0, over5 = 0;
		double worstRatio = 0.0;
		glm::vec3 worstMean(0.0f);
		glm::vec3 worstPos(0.0f);
		uint32_t worstLevel = 0;
		for (const SgProbe& p : vol.probes) {
			glm::vec3 m(0.0f);
			for (uint32_t i = 0; i < kSgLobeCount; ++i)
				m += p.amps[i];
			m /= float(kSgLobeCount);
			const float mx = std::max(m.x, std::max(m.y, m.z));
			const float mn = std::min(m.x, std::min(m.y, m.z));
			if (!(mx > 1e-6f))
				continue; // dark probe: no hue to speak of
			const double ratio = double(mx) / double(std::max(mn, 1e-6f));
			if (ratio > 2.0) ++over2;
			if (ratio > 5.0) ++over5;
			if (ratio > worstRatio) {
				worstRatio = ratio;
				worstMean = m;
				worstPos = p.position;
				worstLevel = p.level;
			}
		}
		std::cout << "  hue: ratio>2: " << over2 << " ratio>5: " << over5
			<< " worst=" << worstRatio << " mean=(" << worstMean.x << ","
			<< worstMean.y << "," << worstMean.z << ") L" << worstLevel
			<< " (" << worstPos.x << "," << worstPos.y << "," << worstPos.z << ")\n";
	}

	// Transect: CPU Sample() + up-normal irradiance across the volume at
	// standing eye height. Adjacent steps share almost all contributors,
	// so cliffs here mean the baked probes genuinely disagree (bake fault),
	// while a smooth transect with a blotchy GPU image means the fault is
	// downstream (pack/grid/shader).
	const glm::vec3 lo = vol.boundsMin, hi = vol.boundsMax;
	// Standing eye height (volume bounds are padded ~rootBrick/4 below
	// the actual floor, so lo.y + 1.5 can end up under the scene).
	const float y = std::clamp(1.0f, lo.y, hi.y);
	const float z = (lo.z + hi.z) * 0.5f;
	const glm::vec3 up(0, 1, 0);
	size_t uncovered = 0, steps = 0;
	double prevLum = -1.0, maxStep = 0.0;
	std::cout << "  transect y=" << y << " z=" << z << " (x, r, g, b, covered):\n";
	for (float x = lo.x; x <= hi.x; x += 0.25f) {
		glm::vec3 amps[kSgLobeCount];
		const bool ok = vol.Sample(glm::vec3(x, y, z), amps);
		++steps;
		if (!ok) { ++uncovered; continue; }
		const glm::vec3 e = SgDiffuseIrradiance(amps, up);
		const double l = lum(e);
		if (prevLum >= 0.0)
			maxStep = std::max(maxStep, std::abs(l - prevLum) / std::max(prevLum, 1e-3));
		prevLum = l;
		std::cout << "    " << x << " " << e.r << " " << e.g << " " << e.b << " 1\n";
	}
	std::cout << "  transect steps=" << steps << " uncovered=" << uncovered
		<< " maxAdjacentRelStep=" << maxStep << "\n";

	// Per-level weight census at the transect points: for each point,
	// the visibility-weighted (wv) share of each octree level in the
	// blended result, brute-forced over all probes (independent
	// reimplementation of ProbeWeight/VisibilityFactor from
	// probe_volume.cpp — also a cross-check of the runtime blend).
	// Reports where COARSE probes dominate a point that FINE probes also
	// cover (the coarse-leak signature: big soft blobs amid dense fine
	// probes).
	{
		auto wendland = [](const glm::vec3& pos, const SgProbe& p) {
			const glm::vec3 d = pos - p.position;
			const float dist2 = glm::dot(d, d);
			const float r2 = p.radius * p.radius;
			if (dist2 >= r2)
				return 0.0f;
			const float x2 = dist2 / r2;
			const float x2m = 1.0f - x2;
			return x2m * x2m * x2m;
		};
		static const glm::vec3 kAxes[6] = {
			glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
			glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
		};
		auto visFactor = [&](const glm::vec3& pos, const SgProbe& p) {
			const float slack = std::clamp(0.35f * p.radius, 0.05f, 0.3f);
			const glm::vec3 d = pos - p.position;
			float w = 1.0f;
			for (uint32_t k = 0; k < 6; ++k) {
				const float dist = p.visibility[k];
				if (dist <= 0.0f)
					continue;
				const float along = glm::dot(d, kAxes[k]);
				if (along > dist + slack) {
					w *= std::max(0.0f, 1.0f
						- (along - dist - slack) / (0.25f * dist + 0.02f));
					if (w <= 0.0f)
						return 0.0f;
				}
			}
			return w;
		};
		std::cout << "  level census (point, then wv share % per level L1..L5, dominant):\n";
		uint32_t coarseDominant = 0, fineDominant = 0, censusPts = 0, leakPts = 0;
		// 3D sweep: heights through the arches + z rows through the
		// recesses (the transect-only census can miss leak pockets).
		const float ys[5] = { 0.5f, 1.5f, 3.5f, 6.0f, 9.0f };
		const float span = hi.z - lo.z;
		const float zs[3] = { z - span * 0.25f, z, z + span * 0.25f };
		for (float yA : ys) {
			if (yA < lo.y || yA > hi.y)
				continue;
			for (float zA : zs) {
				for (float x = lo.x; x <= hi.x; x += 0.5f) {
					const glm::vec3 pos(x, yA, zA);
			float wByLevel[kSgGiMaxLevels] = {};
			for (const SgProbe& p : vol.probes) {
				if (!p.valid)
					continue;
				const float w = wendland(pos, p);
				if (w <= 0.0f)
					continue;
				const float wv = w * visFactor(pos, p);
				if (wv > 0.0f && p.level < kSgGiMaxLevels)
					wByLevel[p.level] += wv;
			}
			float total = 0.0f;
			for (uint32_t l = 0; l < vol.levelCount; ++l)
				total += wByLevel[l];
			if (total <= 1e-4f)
				continue;
			++censusPts;
			uint32_t dom = 0;
			float best = -1.0f;
			for (uint32_t l = 0; l < vol.levelCount; ++l)
				if (wByLevel[l] > best) { best = wByLevel[l]; dom = l; }
			const float fineShare = (vol.levelCount > 4)
				? (wByLevel[3] + wByLevel[4])
					/ (total + 1e-6f) : 0.0f;
			if (dom <= 2)
				++coarseDominant;
			else
				++fineDominant;
			// Flag the leak signature: coarse dominant while fine probes
			// hold a real share too (they were spent on this region!).
			if (dom <= 2 && fineShare > 0.15f) {
				++leakPts;
				std::cout << "    (" << x << "," << yA << "," << zA
					<< ") LEAK? dom=L" << dom << " shares:";
			} else if (dom <= 2) {
				std::cout << "    (" << x << "," << yA << "," << zA
					<< ") coarse-only dom=L" << dom << " shares:";
			} else
				continue;
			for (uint32_t l = 1; l < std::min<uint32_t>(vol.levelCount, 6u); ++l)
				std::cout << " L" << l << "="
					<< int(100.0f * wByLevel[l] / (total + 1e-6f)) << "%";
			std::cout << "\n";
				}
			}
		}
		std::cout << "  census: " << censusPts << " pts, coarse-dominant "
			<< coarseDominant << ", fine-dominant " << fineDominant
			<< ", leak-signature " << leakPts << "\n";
	}

	// Same-level neighbor disagreement (speckle census): for each probe of
	// the finest levels, compare its mean amplitude luminance against the
	// same-level neighbors inside 2x spacing. The relative deviation is
	// exactly what runtime blends turn into per-probe patches when no
	// smoothing runs: median ~5% reads smooth, median ~30%+ reads as
	// white/grey dots the size of a probe support.
	{
		const float cell = 1.0f;
		glm::vec3 gmin(1e30f);
		for (const SgProbe& p : vol.probes)
			gmin = glm::min(gmin, p.position);
		auto key = [&](int x, int y, int z) {
			return (uint64_t(uint32_t(x) + 512) << 42)
				| (uint64_t(uint32_t(y) + 512) << 21)
				| uint64_t(uint32_t(z) + 512);
		};
		std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
		for (uint32_t i = 0; i < vol.probes.size(); ++i) {
			const glm::vec3 d = (vol.probes[i].position - gmin) / cell;
			grid[key(int(d.x), int(d.y), int(d.z))].push_back(i);
		}
		auto meanLumOf = [&](const SgProbe& p) {
			glm::vec3 m(0.0f);
			for (uint32_t i = 0; i < kSgLobeCount; ++i)
				m += p.amps[i];
			m /= float(kSgLobeCount);
			return 0.2126f * m.x + 0.7152f * m.y + 0.0722f * m.z;
		};
		std::vector<double> devs;
		for (uint32_t i = 0; i < vol.probes.size(); ++i) {
			const SgProbe& p = vol.probes[i];
			if (p.level + 2 < vol.levelCount)
				continue; // finest two levels only
			const float r = 2.0f * p.radius;
			const glm::vec3 lo = (p.position - glm::vec3(r) - gmin) / cell;
			const glm::vec3 hi = (p.position + glm::vec3(r) - gmin) / cell;
			double nbSum = 0.0;
			uint32_t nb = 0;
			for (int x = int(lo.x); x <= int(hi.x); ++x)
				for (int y = int(lo.y); y <= int(hi.y); ++y)
					for (int z = int(lo.z); z <= int(hi.z); ++z) {
						auto it = grid.find(key(x, y, z));
						if (it == grid.end())
							continue;
						for (uint32_t j : it->second) {
							if (j == i || vol.probes[j].level != p.level)
								continue;
							const glm::vec3 dd = p.position - vol.probes[j].position;
							if (glm::dot(dd, dd) < r * r) {
								nbSum += meanLumOf(vol.probes[j]);
								++nb;
							}
						}
					}
			if (nb < 3)
				continue;
			const double self = meanLumOf(p);
			const double nbMean = nbSum / nb;
			if (nbMean > 1e-4 && self > 1e-4)
				devs.push_back(std::abs(self - nbMean) / nbMean);
		}
		if (!devs.empty()) {
			std::sort(devs.begin(), devs.end());
			const auto q = [&](double f) {
				return devs[size_t(f * double(devs.size() - 1))];
			};
			std::cout << "  speckle census (|self-nbMean|/nbMean, finest levels): n="
				<< devs.size() << " median=" << q(0.5)
				<< " p75=" << q(0.75) << " p90=" << q(0.9)
				<< " p99=" << q(0.99) << "\n";
		}
	}
	return 0;
}

// Scene-id -> glTF vfs path (mirrors app::SceneRegistry).
std::optional<std::string> SceneGltf(const std::string& id) {
	if (id == "sponza") return std::string("models/Sponza/sponza.gltf");
	if (id == "bistro") return std::string("models/Bistro/bistro.gltf");
	if (id == "bistro-niagara") return std::string("models/BistroNiagara/bistro.gltf");
	if (id == "scanlands") return std::string("models/Scanlands/scanlands.gltf");
	return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
	Args args;
	if (!ParseArgs(argc, argv, args)) {
		Usage();
		return 2;
	}

	if (!args.analyze.empty()) {
		if (args.analyze.size() >= 5
			&& args.analyze.compare(args.analyze.size() - 5, 5, ".sgrv") == 0)
			return AnalyzeRegularVolume(args.analyze);
		return AnalyzeVolume(args.analyze);
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

	// Environment defaults mirror the runtime EnvironmentSettings so the
	// bake matches what the app shows when the GI volume is loaded. The
	// analytic sky is baked on open-sky rays (TraceIncoming), so
	// --sky-intensity is meaningful again; emissive stays runtime-only.
	if (!args.emissive)
		std::cout << "  note: --emissive off (emissive is runtime-only)\n";
	buildOpts.env.sunAzimuthDeg = args.sunAzimuth;
	buildOpts.env.sunElevationDeg = args.sunElevation;
	buildOpts.env.sunIntensity = args.sunIntensity;
	buildOpts.env.skyIntensity = args.skyIntensity;
	buildOpts.includeEmissive = args.emissive;
	// Real materials by default (the production look: bounces carry each
	// surface's color). --uniform-albedo overrides with a known flat
	// albedo (analytic-transport debug mode).
	if (!args.uniformAlbedo.empty() && args.uniformAlbedo != "off") {
		glm::vec3 uv(0.7f);
		if (std::sscanf(args.uniformAlbedo.c_str(), "%f,%f,%f",
				&uv.x, &uv.y, &uv.z) != 3)
			uv = glm::vec3(std::stof(args.uniformAlbedo));
		buildOpts.uniformAlbedo = true;
		buildOpts.uniformAlbedoValue = uv;
		std::cout << "  uniform albedo: (" << uv.x << "," << uv.y << "," << uv.z << ")\n";
	}

	smvk::gi::GiTraceScene traceScene;
	std::string err;
	smvk::gi::BuildTraceSceneFromCluster(*cluster, buildOpts, traceScene, err);
	if (!err.empty()) {
		std::cerr << "Scene build failed: " << err << std::endl;
		return 1;
	}
	// Static spots for the bake (same spec as PakCooker).
	for (const std::string& spec : args.spots) {
		smvk::gi::GiTraceSpot spot{};
		float innerDeg = 0.0f, outerDeg = 0.0f, candela = 0.0f;
		glm::vec3 col(1.0f);
		if (std::sscanf(spec.c_str(),
				"%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f",
				&spot.position.x, &spot.position.y, &spot.position.z,
				&spot.direction.x, &spot.direction.y, &spot.direction.z,
				&innerDeg, &outerDeg, &spot.range,
				&col.r, &col.g, &col.b, &candela) != 13) {
			std::cerr << "Bad --spot spec: " << spec << "\n";
			return 2;
		}
		spot.color = col * candela;
		spot.cosInner = std::cos(glm::radians(std::min(innerDeg, outerDeg)));
		spot.cosOuter = std::cos(glm::radians(std::max(innerDeg, outerDeg)));
		traceScene.spots.push_back(spot);
	}
	std::cout << "  trace scene: " << traceScene.Bvh().TriangleCount()
		<< " tris, " << traceScene.materials.size() << " materials, "
		<< traceScene.spots.size() << " spots\n";

	// Trace-scene world AABB (bake source of truth for box placement).
	glm::vec3 sceneMin(1e30f), sceneMax(-1e30f);
	for (const glm::vec3& v : traceScene.positions) {
		sceneMin = glm::min(sceneMin, v);
		sceneMax = glm::max(sceneMax, v);
	}
	std::cout << "  scene bounds: (" << sceneMin.x << "," << sceneMin.y << "," << sceneMin.z
		<< ")..(" << sceneMax.x << "," << sceneMax.y << "," << sceneMax.z << ")\n";
	if (args.printBounds)
		return 0;

	if (args.scanInterior) {
		// Enclosure scan at standing eye height: classifies each 1 m
		// free-space cell as open / street-canyon / enclosed-interior
		// from 6-axis obstacle distances, then reports connected
		// enclosed components as --reg-min/--reg-max candidates.
		const float yEye = 1.6f;
		const float step = 1.0f;
		const float maxDist = 60.0f;
		const int nx = std::max(1, int(std::ceil((sceneMax.x - sceneMin.x) / step)));
		const int nz = std::max(1, int(std::ceil((sceneMax.z - sceneMin.z) / step)));
		std::vector<int> cls(size_t(nx) * size_t(nz), -1); // -1 solid, 0 open, 1 canyon, 2 enclosed
		static const glm::vec3 kAxes[6] = {
			{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
		};
		for (int iz = 0; iz < nz; ++iz) {
			for (int ix = 0; ix < nx; ++ix) {
				const glm::vec3 p(sceneMin.x + (ix + 0.5f) * step, yEye,
					sceneMin.z + (iz + 0.5f) * step);
				if (traceScene.Bvh().ClosestDistance(p) < 0.2f)
					continue; // solid
				float d[6] = {};
				for (int k = 0; k < 6; ++k) {
					smvk::gi::TriangleBvh::Hit h;
					d[k] = traceScene.Bvh().Raycast(p, kAxes[k], maxDist, h) ? h.t : maxDist;
				}
				const float hMax = std::max(std::max(d[0], d[1]), std::max(d[4], d[5]));
				const float hMin = std::min(std::min(d[0], d[1]), std::min(d[4], d[5]));
				int c = 0;
				if (hMax < 15.0f && d[2] < 10.0f)
					c = 2; // enclosed interior (all walls + ceiling near)
				else if (hMin < 8.0f && hMax > 20.0f)
					c = 1; // street canyon (walls near across, open along)
				cls[size_t(iz) * nx + ix] = c;
			}
		}
		std::cout << "  interior scan y=" << yEye << " step=" << step
			<< " ('#' solid, '.' open, 's' canyon, 'I' enclosed):\n";
		for (int iz = nz - 1; iz >= 0; --iz) {
			std::string row;
			row.reserve(size_t(nx));
			for (int ix = 0; ix < nx; ++ix) {
				const int c = cls[size_t(iz) * nx + ix];
				row.push_back(c < 0 ? '#' : (c == 0 ? '.' : (c == 1 ? 's' : 'I')));
			}
			std::cout << "    z=" << (sceneMin.z + (iz + 0.5f) * step) << " " << row << "\n";
		}
		// Connected enclosed components (4-neighbourhood).
		std::vector<int> comp(size_t(nx) * size_t(nz), -1);
		int nComp = 0;
		for (int iz = 0; iz < nz; ++iz)
			for (int ix = 0; ix < nx; ++ix) {
				if (cls[size_t(iz) * nx + ix] != 2 || comp[size_t(iz) * nx + ix] >= 0)
					continue;
				std::vector<std::pair<int, int>> stack{ { ix, iz } };
				comp[size_t(iz) * nx + ix] = nComp;
				size_t head = 0;
				while (head < stack.size()) {
					const auto [cx, cz] = stack[head++];
					static const int kDx[4] = { 1, -1, 0, 0 };
					static const int kDz[4] = { 0, 0, 1, -1 };
					for (int k = 0; k < 4; ++k) {
						const int nx2 = cx + kDx[k], nz2 = cz + kDz[k];
						if (nx2 < 0 || nz2 < 0 || nx2 >= nx || nz2 >= nz)
							continue;
						if (cls[size_t(nz2) * nx + nx2] != 2
							|| comp[size_t(nz2) * nx + nx2] >= 0)
							continue;
						comp[size_t(nz2) * nx + nx2] = nComp;
						stack.emplace_back(nx2, nz2);
					}
				}
				++nComp;
			}
		std::cout << "  enclosed components: " << nComp << "\n";
		for (int c = 0; c < nComp; ++c) {
			int x0 = nx, x1 = -1, z0 = nz, z1 = -1, count = 0;
			for (int iz = 0; iz < nz; ++iz)
				for (int ix = 0; ix < nx; ++ix)
					if (comp[size_t(iz) * nx + ix] == c) {
						x0 = std::min(x0, ix); x1 = std::max(x1, ix);
						z0 = std::min(z0, iz); z1 = std::max(z1, iz);
						++count;
					}
			if (count < 4)
				continue; // closet, skip
			const float ax0 = sceneMin.x + x0 * step, ax1 = sceneMin.x + (x1 + 1) * step;
			const float az0 = sceneMin.z + z0 * step, az1 = sceneMin.z + (z1 + 1) * step;
			std::cout << "    comp " << c << " cells=" << count
				<< " x=[" << ax0 << "," << ax1 << "] z=[" << az0 << "," << az1 << "]"
				<< "  suggest: --reg-min " << ax0 << "," << sceneMin.y << "," << az0
				<< " --reg-max " << ax1 << "," << (yEye + 3.0f) << "," << az1 << "\n";
		}
		return 0;
	}

	// Regular-lattice prototype bake (.sgrv). Runs INSTEAD of the
	// adaptive bake when only --regular-out is given, or IN ADDITION
	// when both --out and --regular-out are set (clean A/B). Writes one
	// .sgrv; append its vfs path to content/gi/<scene>.regmanifest to
	// activate the runtime dynamic-object grids (artist OBBs + density
	// via --reg-min/--reg-max/--reg-spacing; see --scan-interior).
	if (!args.regularOut.empty()) {
		auto parseVec3 = [](const std::string& s, glm::vec3& v) {
			return std::sscanf(s.c_str(), "%f,%f,%f", &v.x, &v.y, &v.z) == 3;
		};
		auto parseDims = [](const std::string& s, uint32_t (&d)[3]) {
			return std::sscanf(s.c_str(), "%u,%u,%u", &d[0], &d[1], &d[2]) == 3;
		};
		smvk::gi::SgRegularObb obb{};
		bool haveBox = false;
		glm::vec3 regMin, regMax;
		if (!args.regMin.empty() && !args.regMax.empty()
			&& parseVec3(args.regMin, regMin) && parseVec3(args.regMax, regMax)) {
			haveBox = true;
		} else if (!args.regPreset.empty()) {
			if (args.regPreset == "scene") {
				regMin = sceneMin; regMax = sceneMax; haveBox = true;
			} else if (args.regPreset == "bistro-street") {
				// Outdoor street canyon around the canon view
				// (27.61,2.26,-10.19): the 's' band from --scan-interior.
				regMin = glm::vec3(-10.0f, -1.0f, -32.0f);
				regMax = glm::vec3(40.0f, 10.0f, -2.0f);
				haveBox = true;
			} else if (args.regPreset == "bistro-bar") {
				// Bar/restaurant interior: largest enclosed component
				// from --scan-interior (comp 75, 146 cells).
				regMin = glm::vec3(9.4f, -1.0f, 0.6f);
				regMax = glm::vec3(28.4f, 5.5f, 18.6f);
				haveBox = true;
			} else {
				std::cerr << "Unknown --reg-preset '" << args.regPreset
					<< "' (bistro-bar | bistro-street | scene)\n";
				return 2;
			}
		}
		if (!haveBox) {
			std::cerr << "Regular bake needs --reg-min/--reg-max or --reg-preset\n";
			return 2;
		}
		obb.center = (regMin + regMax) * 0.5f;
		obb.halfExtents = (regMax - regMin) * 0.5f;
		if (!parseDims(args.regDims, obb.dims)) {
			std::cerr << "Bad --reg-dims (want nx,ny,nz)\n";
			return 2;
		}
		if (args.regSpacing > 0.0f) {
			const glm::vec3 ext = regMax - regMin;
			for (int a = 0; a < 3; ++a)
				obb.dims[a] = std::max(1u, uint32_t(std::ceil(ext[a] / args.regSpacing)));
		}
		std::cout << "  regular OBB: center=(" << obb.center.x << "," << obb.center.y
			<< "," << obb.center.z << ") half=(" << obb.halfExtents.x << ","
			<< obb.halfExtents.y << "," << obb.halfExtents.z << ") dims="
			<< obb.dims[0] << "x" << obb.dims[1] << "x" << obb.dims[2] << "\n";

		smvk::gi::SgRegularBakeConfig rcfg{};
		rcfg.raysPerProbe = args.rays;
		rcfg.maxBounces = args.bounces;
		rcfg.threadCount = args.threads;
		smvk::gi::SgRegularVolume rvol;
		smvk::gi::SgRegularBakeStats rstats;
		if (!smvk::gi::BakeSgRegularVolume(traceScene, obb, rcfg, rvol, rstats,
				[](const char* line) { std::cout << line; })) {
			std::cerr << "Regular bake failed\n";
			return 1;
		}
		{
			std::filesystem::path out(args.regularOut);
			std::error_code ec;
			std::filesystem::create_directories(out.parent_path(), ec);
		}
		std::string rerr;
		if (!rvol.WriteFile(args.regularOut, rerr)) {
			std::cerr << "Regular write failed: " << rerr << std::endl;
			return 1;
		}
		std::cout << "Wrote " << args.regularOut << " ("
			<< std::filesystem::file_size(args.regularOut) << " B on disk)\n";
		if (args.verify) {
			smvk::gi::SgRegularVolume reloaded;
			std::string verr;
			if (!smvk::gi::SgRegularVolume::ReadFile(args.regularOut, reloaded, verr)) {
				std::cerr << "Regular verify: reload failed: " << verr << std::endl;
				return 1;
			}
			std::cout << "  verify: " << reloaded.VoxelCount() << " voxels reloaded OK\n";
			if (reloaded.VoxelCount() != rvol.VoxelCount())
				return 1;
		}
		// Fall through to the adaptive bake only when the user asked
		// for it explicitly (both --out and --regular-out = A/B pair
		// in one invocation). A regular-only run passes no --out.
		if (!args.outExplicit) {
			const auto t1 = std::chrono::steady_clock::now();
			std::cout << "GiBaker total: "
				<< std::chrono::duration<double>(t1 - t0).count() << "s\n";
			return 0;
		}
	}

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
	cfg.diffusionIterations = args.diffusion;
	cfg.smoothingPasses = args.smoothingPasses;
	cfg.probeSupportFactor = args.support;

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
