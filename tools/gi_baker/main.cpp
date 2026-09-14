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
// defaults; the bake captures SUN BOUNCE ONLY (added on top of the
// analytic sky IBL at runtime), and EXCLUDES direct sun (the runtime's
// cascaded shadow maps own it), sky, emissive geometry, and the dynamic
// forward+ point/spot lights. --sky-intensity/--emissive are accepted
// but ignored (deprecated no-ops kept for old scripts). Materials bake
// white-world (uniform albedo 0.7) unless --uniform-albedo off requests
// the textured path: per-material albedo on contrasty scenes prints
// chromatic dots the 9-lobe fit cannot represent (verified on Sponza).

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
#include <Engine/Utils/gltf.h>
#include <Engine/Vfs/vfs.h>

#include "ktx.h"

namespace {

struct Args {
	std::string contentDir = "content";
	std::string scene;    // sponza | bistro | bistro-niagara
	std::string inVfs;    // arbitrary model (overrides --scene input)
	std::string out;      // output host path (default <content>/gi/<scene>.sggi)
	uint32_t rays = 256;
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
		<< "         [--rays 256] [--bounces 3] [--error-rays 32]\n"
		<< "         [--root 8.0] [--min-brick 0.25] [--max-probes 1048576]\n"
		<< "         [--tolerance 0.12] [--near-factor 1.0] [--waves 8] [--threads 0]\n"
		<< "         [--diffusion 0] [--smoothing-passes 0] [--support 1.25]\n"
		<< "         [--sun-azimuth 30] [--sun-elevation 55] [--sun-intensity 10000]\n"
		<< "         [--sky-intensity 600] [--textures on|off] [--emissive on|off]\n"
		<< "         [--verify on|off] [--uniform-albedo 0.7|r,g,b|off]\n"
		<< "         (default uniform 0.7 white-world; off = textured bake)\n"
		<< "GiBaker --analyze <file.sggi>: dump probe amplitude stats and a\n"
		<< "         CPU-sampled irradiance transect (bake diagnostics).\n";
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

	if (!args.analyze.empty())
		return AnalyzeVolume(args.analyze);

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
	// Sun-only field: sun params matter; sky/emissive flags are no-ops.
	if (args.skyIntensity != 600.0f)
		std::cout << "  note: --sky-intensity ignored (sun-only GI)\n";
	if (!args.emissive)
		std::cout << "  note: --emissive off (sun-only GI ignores it either way)\n";
	buildOpts.env.sunAzimuthDeg = args.sunAzimuth;
	buildOpts.env.sunElevationDeg = args.sunElevation;
	buildOpts.env.sunIntensity = args.sunIntensity;
	buildOpts.env.skyIntensity = args.skyIntensity;
	buildOpts.includeEmissive = args.emissive;
	// White-world mode: single known albedo everywhere (textures ignored),
	// so the transport is analytically checkable and texture cooking
	// cannot print through as dots.
	if (!args.uniformAlbedo.empty() && args.uniformAlbedo != "off") {
		glm::vec3 uv(0.7f);
		if (std::sscanf(args.uniformAlbedo.c_str(), "%f,%f,%f",
				&uv.x, &uv.y, &uv.z) != 3)
			uv = glm::vec3(std::stof(args.uniformAlbedo));
		buildOpts.uniformAlbedo = true;
		buildOpts.uniformAlbedoValue = uv;
		std::cout << "  uniform albedo: (" << uv.x << "," << uv.y << "," << uv.z << ")\n";
	} else if (args.uniformAlbedo == "off") {
		std::cout << "  textured albedo (experimental: chromatic dots on contrasty scenes)\n";
	}

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
