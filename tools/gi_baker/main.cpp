// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
// Scene resolution mirrors the App runtime: --scene sponza | bistro |
// bistro-niagara maps to the SAME derived .geometry artifact the runtime
// builds from (the bake must see the exact runtime geometry; glTF is
// IMPORT-ONLY — open/drop the .gltf in the editor to derive the artifacts),
// or pass --in-vfs <model.geometry> for arbitrary scenes. Light setup
// defaults match the runtime environment
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
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/compression.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/bake_inputs.h> // the invalidation model (unit hashes)
#include <Engine/GI/cubemap_volume.h>
#include <Engine/GI/lightmap.h> // lightmap-chain diagnostic (--lm-probe)
#include <Engine/GI/probe_volume.h>
#include <Engine/GI/regular_volume.h>
#include <Engine/Asset/tour.h>
#include <Engine/Scene/scene.h>
#include <gltf_import/gltf.h>
#include <Engine/Vfs/vfs.h>

#include "gi_baker.h"
#include <tool_log.h>

namespace gibaker {

// Args lives in gi_baker.h (embedders fill it directly).

bool ParseArgs(int argc, char** argv, Args& out) {
	auto need = [&](std::string& dst, int& i) {
		if (i + 1 >= argc) {
			tool::err() << "Missing value for " << argv[i] << std::endl;
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
		else if (a == "--tour") { if (!need(out.tour, i)) return false; }
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
		else if (a == "--backend") {
			std::string v;
			if (!need(v, i)) return false;
			if (v != "builtin" && v != "embree") {
				tool::err() << "--backend must be builtin|embree" << std::endl;
				return false;
			}
			out.backend = v;
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
		else if (a == "--gi-volume-spec") { if (!need(out.giVolumeSpec, i)) return false; }
		else if (a == "--gi-volume-dirs") { if (!needU(out.giVolumeDirs, i)) return false; }
		else if (a == "--gi-volume-smoothing") { if (!needU(out.giVolumeSmoothing, i)) return false; }
		else if (a == "--mask-transmission") {
			std::string v;
			if (!need(v, i)) return false;
			out.maskTransmission = (v == "on" || v == "1" || v == "true");
			out.maskTransmissionSet = true;
		}
		else if (a == "--gi-volume-capture-spacing") {
			std::string v;
			if (!need(v, i)) return false;
			out.giVolumeCaptureSpacing = std::stof(v);
		}
		else if (a == "--lm-probe") { if (!needU(out.lmProbe, i)) return false; }
		else if (a == "--help" || a == "-h") {
			return false;
		}
		else {
			tool::err() << "Unknown arg: " << a << std::endl;
			return false;
		}
	}
	if (out.analyze.empty() && out.inVfs.empty() && out.scene.empty()
		&& out.tour.empty()) {
		tool::err() << "Required: --scene <id>, --in-vfs <model.gltf>,"
			<< " --tour <def.tour>, or --analyze <file.sggi>" << std::endl;
		return false;
	}
	return true;
}

void Usage() {
	tool::out() << "GiBaker --content-dir content --scene sponza\n"
		<< "         [--out content/gi/sponza.sggi] [--in-vfs models/X/x.geometry]\n"
		<< "         (bake outputs ride STABLE VFS paths like mesh blobs:\n"
		<< "         gi/<scene>.sggi, gi/<scene>_<name>.givolume + the manifests\n"
		<< "         gi/<scene>.givolumemanifest / gi/<scene>.regmanifest (rows\n"
		<< "         are bare VFS paths))\n"
		<< "         [--rays 1024] [--bounces 3] [--error-rays 32]\n"
		<< "         [--root 8.0] [--min-brick 0.25] [--max-probes 1048576]\n"
		<< "         [--tolerance 0.12] [--near-factor 1.0] [--waves 8] [--threads 0]\n"
		<< "         [--diffusion 0] [--smoothing-passes 0] [--support 1.25]\n"
		<< "         [--sun-azimuth 30] [--sun-elevation 55] [--sun-intensity 10000]\n"
		<< "         [--sky-intensity 600] [--textures on|off] [--emissive on|off]\n"
		<< "         [--verify on|off] [--uniform-albedo off|0.7|r,g,b]\n"
		<< "         [--backend builtin|embree]\n"
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
		<< "         scene AABB / enclosure map to place bar/street boxes.\n"
		<< "GiBaker --scene bistro --gi-volume-spec volumes.txt\n"
		<< "         [--gi-volume-dirs 1536]: bake STACKED CONVEX GI volumes\n"
		<< "         (probes-only pivot; a DIRECT per-probe path trace per\n"
		<< "         dense grid node — the BO3 capture/reprojection era\n"
		<< "         is retired; --gi-volume-capture-spacing parses but is inert).\n"
		<< "         Spec file, one `volume <name>` block each:\n"
		<< "           volume bar\n"
		<< "             box 9.4,-1,0.6 28.4,5.5,18.6 [0.1,0.1]\n"
		<< "             plane 0,0,1,-3.2 2.0 2.0     # doorway fade\n"
		<< "             face-fade 2 0.1 0.1          # retune plane #2\n"
		<< "             priority 10\n"
		<< "             spacing 0.5\n"
		<< "         Writes gi/<scene>_<name>.givolume + updates the runtime\n"
		<< "         manifest gi/<scene>.givolumemanifest (rows are bare VFS paths).\n"
		<< "         Volumes blend over the .sggi world field by priority.\n"
		<< "GiBaker --scene bistro --lm-probe N --uniform-albedo off\n"
		<< "         [--sun-... --sky-intensity ...]: lightmap-chain diagnostic\n"
		<< "         (authored-pack charts -> rasterize -> bake) on N meshes;\n"
		<< "         prints chunk stats + first-texel trace (cook faults without\n"
		<< "         a pak cook).\n";
}

// Regular-volume inspector (.sgrv): coefficient stats + a trilinear
// irradiance transect, mirroring AnalyzeVolume for the adaptive format.
int AnalyzeRegularVolume(const std::string& path) {
	using namespace smvk::gi;
	SgRegularVolume vol;
	std::string err;
	if (!SgRegularVolume::ReadFile(path, vol, err)) {
		tool::err() << "Analyze: load failed: " << err << std::endl;
		return 1;
	}
	auto lum = [](const glm::vec3& c) {
		return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
	};
	const uint64_t n = vol.VoxelCount();
	tool::out() << "analyze-regular: " << path << "\n  voxels=" << n
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
	tool::out() << "  meanLum=" << (n ? sumLum / double(n) : 0.0)
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
	tool::out() << "  transect steps=" << steps << " uncovered=" << uncovered
		<< " maxAdjacentRelStep=" << maxStep << " at x=" << worstX
		<< " (" << worstA << " -> " << worstB << ")"
		<< " minLum=" << minLum << " maxLum=" << maxLumT << "\n";
	return 0;
}

// Offline volume inspector: amplitude distribution (NaN/Inf/extremes per
// level) plus a CPU Sample() irradiance transect across the volume so
// blotchy runtime output can be bisected into bake-side (extreme or
// disagreeing probes) vs GPU-side (transfer/sampling) faults.
int AnalyzeGiVolume(const std::string& path) {
	using namespace smvk::gi;
	GiVolume vol;
	std::string err;
	if (!GiVolume::ReadFile(path, vol, err)) {
		tool::err() << "Analyze: load failed: " << err << std::endl;
		return 1;
	}
	auto lum = [](const glm::vec3& c) {
		return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
	};
	const uint32_t nx = vol.dims[0], ny = vol.dims[1], nz = vol.dims[2];
	const uint64_t n = vol.VoxelCount();
	auto probeLum = [&](uint64_t v) {
		double l = 0.0;
		for (uint32_t i = 0; i < kSgLobeCount; ++i)
			l += lum(vol.coeffs[size_t(v) * kSgLobeCount + i]);
		return l / kSgLobeCount;
	};
	tool::out() << "analyze: " << path << "\n  voxels=" << n
		<< " dims=" << nx << "x" << ny << "x" << nz
		<< " spacing=" << vol.spacing
		<< " gridMin=(" << vol.gridMin.x << "," << vol.gridMin.y
		<< "," << vol.gridMin.z << ")\n";
	size_t nanV = 0;
	double sum = 0.0, mx = 0.0;
	for (uint64_t v = 0; v < n; ++v) {
		bool nan = false;
		for (uint32_t i = 0; i < kSgLobeCount; ++i) {
			const glm::vec3& a = vol.coeffs[size_t(v) * kSgLobeCount + i];
			if (std::isnan(a.x + a.y + a.z) || std::isinf(a.x + a.y + a.z))
				nan = true;
		}
		if (nan) {
			++nanV;
			continue;
		}
		const double l = probeLum(v);
		sum += l;
		mx = std::max(mx, l);
	}
	tool::out() << "  meanLum=" << (n ? sum / double(n) : 0.0)
		<< " maxLum=" << mx << " nanVoxels=" << nanV << "\n";
	// Neighbor variation per axis: the field's own high-frequency
	// content (the trilerp exposes exactly this at runtime). High
	// values here read as a mask/texture pattern on every surface.
	struct NborExtreme { double d; glm::vec3 a, b; };
	std::vector<NborExtreme> extremes;
	for (int axis = 0; axis < 3; ++axis) {
		const uint64_t stride = axis == 0 ? 1
			: axis == 1 ? uint64_t(nx) : uint64_t(nx) * ny;
		const uint64_t axisDim = axis == 0 ? nx : axis == 1 ? ny : nz;
		double dSum = 0.0, dMax = 0.0;
		uint64_t pairs = 0;
		for (uint64_t z = 0; z < nz; ++z)
			for (uint64_t y = 0; y < ny; ++y)
				for (uint64_t x = 0; x < nx; ++x) {
					const uint64_t v = (z * ny + y) * nx + x;
					const uint64_t c[3] = { x, y, z };
					if (c[axis] + 1 >= axisDim)
						continue;
					const double d = std::abs(probeLum(v + stride)
						- probeLum(v));
					dSum += d;
					dMax = std::max(dMax, d);
					++pairs;
					if (extremes.size() < 4096)
						extremes.push_back({ d,
							vol.gridMin + glm::vec3(float(x), float(y),
								float(z)) * vol.spacing,
							vol.gridMin + glm::vec3(float(x), float(y),
								float(z)) * vol.spacing
							+ glm::vec3(axis == 0 ? 1.0f : 0.0f,
								axis == 1 ? 1.0f : 0.0f,
								axis == 2 ? 1.0f : 0.0f) * vol.spacing });
					else if (d > extremes.back().d) {
						extremes.back() = { d,
							vol.gridMin + glm::vec3(float(x), float(y),
								float(z)) * vol.spacing,
							vol.gridMin + glm::vec3(float(x), float(y),
								float(z)) * vol.spacing
							+ glm::vec3(axis == 0 ? 1.0f : 0.0f,
								axis == 1 ? 1.0f : 0.0f,
								axis == 2 ? 1.0f : 0.0f) * vol.spacing };
					}
				}
		tool::out() << "  neighbor|" << (axis == 0 ? 'x' : axis == 1 ? 'y' : 'z')
			<< "|: meanDelta=" << (pairs ? dSum / double(pairs) : 0.0)
			<< " maxDelta=" << dMax
			<< " (relative " << (pairs && sum ? 100.0 * dSum / double(pairs)
				/ (sum / double(n)) : 0.0) << "% of meanLum)\n";
	}
	std::sort(extremes.begin(), extremes.end(),
		[](const NborExtreme& a, const NborExtreme& b) {
			return a.d > b.d;
		});
	tool::out() << "  top neighbor extremes (delta, posA -> posB):\n";
	for (size_t i = 0; i < extremes.size() && i < 12; ++i)
		tool::out() << "    " << extremes[i].d << "  ("
			<< extremes[i].a.x << "," << extremes[i].a.y << ","
			<< extremes[i].a.z << ") -> (" << extremes[i].b.x << ","
			<< extremes[i].b.y << "," << extremes[i].b.z << ")\n";
	return 0;
}

int AnalyzeVolume(const std::string& path) {
	using namespace smvk::gi;
	SgGiVolume vol;
	std::string err;
	if (!SgGiVolume::ReadFile(path, vol, err)) {
		tool::err() << "Analyze: load failed: " << err << std::endl;
		return 1;
	}
	auto lum = [](const glm::vec3& c) {
		return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
	};
	tool::out() << "analyze: " << path << "\n  probes=" << vol.probes.size()
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
	tool::out() << "  meanLum=" << (vol.probes.empty() ? 0.0 : sumLum / vol.probes.size())
		<< " minLum=" << (minLum == std::numeric_limits<float>::max() ? -1.0 : minLum)
		<< " maxLum=" << maxLum
		<< " nanProbes=" << nanProbes << " infProbes=" << infProbes << "\n";
	for (uint32_t l = 0; l < vol.levelCount; ++l)
		tool::out() << "  level " << l << ": n=" << levelCount[l]
			<< " mean=" << (levelCount[l] ? levelSum[l] / levelCount[l] : 0.0)
			<< " max=" << levelMax[l] << "\n";
	std::sort(extremes.begin(), extremes.end(),
		[](const Extreme& a, const Extreme& b) { return a.l > b.l; });
	tool::out() << "  top extremes (lum, level, pos):\n";
	for (size_t i = 0; i < std::min<size_t>(extremes.size(), 10); ++i)
		tool::out() << "    " << extremes[i].l << " L" << extremes[i].level
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
		tool::out() << "  isolation histogram (same-level neighbors within 1.5R):\n";
		for (int n = 0; n < 8; ++n)
			tool::out() << "    n=" << n << ": " << isoHist[n] << "\n";
		tool::out() << "  fine-level isolated (0 same-level neighbors): "
			<< fineIso << "/" << fineTotal << "\n";
	}
	// Zero census: probes whose mean amplitude is exactly (or near) zero
	// cannot be honest in sunlit open air (a downward ray always finds a
	// bounce). They are NaN-sanitized casualties or fit failures, and
	// their wide supports dilute (darken) large neighborhoods — suspects
	// for dark patches. Dump positions to see where they live.
	{
		uint32_t zeroCount = 0;
		tool::out() << "  zero probes (meanLum < 1e-6), first 25 with level+pos:\n";
		for (const SgProbe& p : vol.probes) {
			glm::vec3 m(0.0f);
			for (uint32_t i = 0; i < kSgLobeCount; ++i)
				m += p.amps[i];
			const double pl = (0.2126 * m.x + 0.7152 * m.y + 0.0722 * m.z) / kSgLobeCount;
			if (pl < 1e-6) {
				++zeroCount;
				if (zeroCount <= 25)
					tool::out() << "    L" << p.level << " (" << p.position.x << ","
						<< p.position.y << "," << p.position.z << ") r=" << p.radius << "\n";
			}
		}
		tool::out() << "  zero probes total: " << zeroCount << "/" << vol.probes.size() << "\n";
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
		tool::out() << "  hue: ratio>2: " << over2 << " ratio>5: " << over5
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
	tool::out() << "  transect y=" << y << " z=" << z << " (x, r, g, b, covered):\n";
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
		tool::out() << "    " << x << " " << e.r << " " << e.g << " " << e.b << " 1\n";
	}
	tool::out() << "  transect steps=" << steps << " uncovered=" << uncovered
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
		tool::out() << "  level census (point, then wv share % per level L1..L5, dominant):\n";
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
				tool::out() << "    (" << x << "," << yA << "," << zA
					<< ") LEAK? dom=L" << dom << " shares:";
			} else if (dom <= 2) {
				tool::out() << "    (" << x << "," << yA << "," << zA
					<< ") coarse-only dom=L" << dom << " shares:";
			} else
				continue;
			for (uint32_t l = 1; l < std::min<uint32_t>(vol.levelCount, 6u); ++l)
				tool::out() << " L" << l << "="
					<< int(100.0f * wByLevel[l] / (total + 1e-6f)) << "%";
			tool::out() << "\n";
				}
			}
		}
		tool::out() << "  census: " << censusPts << " pts, coarse-dominant "
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
			tool::out() << "  speckle census (|self-nbMean|/nbMean, finest levels): n="
				<< devs.size() << " median=" << q(0.5)
				<< " p75=" << q(0.75) << " p90=" << q(0.9)
				<< " p99=" << q(0.99) << "\n";
		}
	}
	return 0;
}

// Scene-id -> glTF vfs path (mirrors the App pak loader (main.cpp)).
std::optional<std::string> SceneGltf(const std::string& id) {
	// Scene presets name the DERIVED .geometry artifact (the MeshImporter's
	// output — the bake must see the exact runtime geometry, and the runtime
	// builds from the artifacts; glTF is import-only).
	if (id == "sponza") return std::string("models/Sponza/Sponza.geometry");
	if (id == "bistro") return std::string("models/Bistro/bistro.geometry");
	if (id == "bistro-niagara") return std::string("models/BistroNiagara/bistro.geometry");
	if (id == "scanlands") return std::string("models/Scanlands/scanlands.geometry");
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// Convex-volume spec (--gi-volume-spec): authoring format for stacked probe
// volumes. One `volume <name>` block per volume; keys inside a block:
//   box <min> <max> [inside,outside]   six planes from an AABB (fades
//                                      default 0.1/0.1; min/max "x,y,z")
//   plane <n.x,n.y,n.z,d> [in [out]]    extra clipping plane (inside:
//                                      n.P + d <= 0), own fades
//   face-fade <planeIndex> <in> <out>  retune one plane's fades
//   priority <n>                        higher wins (composited later)
//   spacing <m>                        dense probe grid spacing
//   capture-spacing <m>                INERT (the per-probe bake has no
//                                      capture lattice); still parsed
// ---------------------------------------------------------------------------
struct GiVolumeSpecVolume {
	smvk::gi::GiVolumeDef def;
	float spacing = 0.5f;
	float captureSpacing = 0.0f;
	// The authored GI volume NODE (tour GI volume nodes; UINT32_MAX = a
	// hand-written --gi-volume-spec entry). The stamp binds baked volumes to
	// their definitions by node — hand-tuning extras carry no node and
	// stay unbound (diagnostic-only under the scene-carried binding
	// model: the pak embeds exactly what the scene binds).
	uint32_t node = UINT32_MAX;
};

bool ParseGiVolumeSpec(const std::string& hostPath, std::vector<GiVolumeSpecVolume>& out,
	std::string& err) {
	std::ifstream in(hostPath);
	if (!in) {
		err = "cannot open spec " + hostPath;
		return false;
	}
	auto parseVec3 = [](const std::string& s, glm::vec3& v) {
		return std::sscanf(s.c_str(), "%f,%f,%f", &v.x, &v.y, &v.z) == 3;
	};
	int unnamed = 0;
	std::string line;
	int lineNo = 0;
	std::optional<size_t> curIdx;
	while (std::getline(in, line)) {
		++lineNo;
		const size_t hash = line.find('#');
		if (hash != std::string::npos)
			line = line.substr(0, hash);
		std::istringstream ss(line);
		std::string key;
		if (!(ss >> key) || key.empty())
			continue;
		auto fail = [&](const std::string& why) {
			err = "gi-volume-spec:" + std::to_string(lineNo) + ": " + why;
			return false;
		};
		if (key == "volume") {
			GiVolumeSpecVolume v{};
			if (!(ss >> v.def.name) || v.def.name.empty()) {
				v.def.name = "vol" + std::to_string(unnamed++);
				ss.clear();
			}
			out.push_back(std::move(v));
			curIdx = out.size() - 1;
			continue;
		}
		if (!curIdx)
			return fail("key '" + key + "' outside a volume block");
		GiVolumeSpecVolume& v = out[*curIdx];
		if (key == "box") {
			std::string mn, mx, fades;
			glm::vec3 bmn, bmx;
			if (!(ss >> mn >> mx) || !parseVec3(mn, bmn) || !parseVec3(mx, bmx))
				return fail("box wants <x,y,z> <x,y,z> [in,out]");
			float inF = 0.1f, outF = 0.1f;
			if ((ss >> fades) && std::sscanf(fades.c_str(), "%f,%f",
					&inF, &outF) != 2)
				return fail("box fades want <in,out>");
			auto planes = smvk::gi::GiVolumeBoxPlanes(bmn, bmx, inF, outF);
			v.def.planes.insert(v.def.planes.end(),
				planes.begin(), planes.end());
		} else if (key == "plane") {
			std::string spec, sIn, sOut;
			if (!(ss >> spec))
				return fail("plane wants <nx,ny,nz,d> [in [out]]");
			smvk::gi::GiVolumePlane p{};
			glm::vec3 n;
			if (!parseVec3(spec, n) || !(ss >> p.d))
				return fail("plane wants <nx,ny,nz,d> [in [out]]");
			if (std::abs(glm::length(n)) < 1e-3f)
				return fail("plane normal is zero");
			p.normal = glm::normalize(n);
			p.insideFade = 0.1f;
			p.outsideFade = 0.1f;
			if ((ss >> sIn))
				p.insideFade = std::stof(sIn);
			if ((ss >> sOut))
				p.outsideFade = std::stof(sOut);
			v.def.planes.push_back(p);
		} else if (key == "face-fade") {
			int idx = -1;
			std::string sIn, sOut;
			if (!(ss >> idx >> sIn >> sOut))
				return fail("face-fade wants <planeIndex> <in> <out>");
			if (idx < 0 || idx >= int(v.def.planes.size()))
				return fail("face-fade plane index out of range");
			v.def.planes[size_t(idx)].insideFade = std::stof(sIn);
			v.def.planes[size_t(idx)].outsideFade = std::stof(sOut);
		} else if (key == "priority") {
			if (!(ss >> v.def.priority))
				return fail("priority wants <n>");
		} else if (key == "spacing") {
			if (!(ss >> v.spacing) || v.spacing <= 0.0f)
				return fail("spacing wants <meters>");
		} else if (key == "capture-spacing") {
			if (!(ss >> v.captureSpacing) || v.captureSpacing <= 0.0f)
				return fail("capture-spacing wants <meters>");
		} else {
			return fail("unknown key '" + key + "'");
		}
	}
	if (out.empty()) {
		err = "cv-spec has no volume blocks";
		return false;
	}
	for (const GiVolumeSpecVolume& v : out) {
		if (v.def.planes.empty()) {
			err = "volume '" + v.def.name + "' has no planes";
			return false;
		}
		if (v.def.planes.size() > smvk::gi::kGiVolumeMaxPlanes) {
			err = "volume '" + v.def.name + "' exceeds "
				+ std::to_string(smvk::gi::kGiVolumeMaxPlanes) + " planes";
			return false;
		}
		glm::vec3 mn, mx;
		if (!v.def.Bounds(mn, mx)) {
			err = "volume '" + v.def.name
				+ "' planes do not bound a closed region";
			return false;
		}
	}
	return true;
}

// Content-relative VFS path for a host path under the content root
// ("F:/x/content/gi/<g>.sgrv" -> "gi/<g>.sgrv"); pass-through otherwise
// (the runtime can only resolve content-relative rows — callers warn).
std::string VfsRelPath(const std::string& contentDir,
	const std::string& hostPath) {
	namespace fs = std::filesystem;
	std::error_code ec;
	const std::string absHost = fs::absolute(fs::path(hostPath), ec)
		.generic_string();
	const std::string absRoot = fs::absolute(fs::path(contentDir), ec)
		.generic_string();
	if (!absHost.empty() && !absRoot.empty()
		&& absHost.size() > absRoot.size()
		&& absHost.compare(0, absRoot.size(), absRoot) == 0
		&& (absHost[absRoot.size()] == '/' || absHost[absRoot.size()] == '\\')) {
		std::string rel = absHost.substr(absRoot.size() + 1);
		for (char& c : rel)
			if (c == '\\')
				c = '/';
		return rel;
	}
	return hostPath;
}

// Merge one bake row into a volume manifest (shared by the reg/givolume
// writers): reads the manifest when it exists; keeps every existing
// line except a row carrying the same "# <note>" (the rebaked volume
// replaces its own row — same volume, possibly a new path); appends the
// new row; hand rows (no recognizable note) pass through verbatim so
// hand edits survive rebakes. Returns the merged lines (no header — the
// caller writes its own).
std::vector<std::string> MergeManifestRows(
	const std::string& manifestPath,
	const std::string& newRow,
	const std::string& newNote) {
	auto readLines = [](const std::string& path,
			std::vector<std::string>& lines) {
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return;
		std::string cur;
		while (std::getline(in, cur)) {
			while (!cur.empty()
				&& (cur.back() == '\r' || cur.back() == ' '
					|| cur.back() == '\t'))
				cur.pop_back();
			if (!cur.empty())
				lines.push_back(cur);
		}
	};
	auto rowNote = [](const std::string& line) {
		const size_t hash = line.find('#');
		if (hash == std::string::npos)
			return std::string();
		std::string note = line.substr(hash + 1);
		size_t b = 0;
		while (b < note.size() && (note[b] == ' ' || note[b] == '\t'))
			++b;
		size_t e = note.size();
		while (e > b && (note[e - 1] == ' ' || note[e - 1] == '\t'))
			--e;
		return note.substr(b, e - b);
	};
	std::vector<std::string> lines;
	readLines(manifestPath, lines);
	std::vector<std::string> merged;
	merged.reserve(lines.size() + 1);
	bool replaced = false;
	for (const std::string& l : lines) {
		if (!newNote.empty() && !replaced && rowNote(l) == newNote) {
			merged.push_back(newRow);
			replaced = true;
			continue;
		}
		if (std::find(merged.begin(), merged.end(), l) == merged.end())
			merged.push_back(l);
	}
	if (!replaced)
		merged.push_back(newRow);
	return merged;
}

} // (helpers above; the namespace stays open through Run below)

namespace gibaker {
// Runs the bake; returns the process exit code (byte-identical to the
// CLI run). See gi_baker.h.
int Run(Args& args) {
	// A non-empty out IS the world-field request. The CLI parse sets
	// outExplicit together with --out; in-process callers (the editor's
	// cook stage 1 / Bake GI) fill Args directly and were silently
	// missing the parse-only flag: GiBaker baked the authored convex
	// volumes, took the GI-volume-only early exit, returned 0 WITHOUT ever
	// writing the .sggi, and the editor's temp publish then failed
	// "file not found" — the three-day "Cook FAILED (see log above)"
	// ghost. Sanitize once here so the gate can never disagree with
	// the field it gates (same field-copy trap class as the Run(c)
	// rebuild comment in cook/src/main.cpp).
	if (!args.out.empty())
		args.outExplicit = true;
	if (!args.analyze.empty()) {
		if (args.analyze.size() >= 5
			&& args.analyze.compare(args.analyze.size() - 5, 5, ".sgrv") == 0)
			return AnalyzeRegularVolume(args.analyze);
		if (args.analyze.size() >= 5
			&& args.analyze.compare(args.analyze.size() - 9, 9, ".givolume") == 0)
			return AnalyzeGiVolume(args.analyze);
		return AnalyzeVolume(args.analyze);
	}

	const auto t0 = std::chrono::steady_clock::now();

	// Input scene (glTF via the VFS; same lookup rules as the runtime).
	smvk::OverlayVfs vfs;
	vfs.Mount(std::make_shared<smvk::LooseBackend>(args.contentDir), 0, "content");

	std::string inVfs = args.inVfs;
	std::string sceneId = args.scene;
	// --tour: the DEFINITION is the input (root reference #0 = the
	// source glTF, GI volume nodes = the convex bake inputs). The
	// cluster builds exactly like the editor / PakCooker do it; the
	// scene id defaults to the tour stem (gi/<id>_* outputs). --in-vfs
	// / --scene are ignored for the import.
	const bool haveTour = !args.tour.empty();
	if (haveTour) {
		if (sceneId.empty()) {
			const std::filesystem::path tp(args.tour);
			sceneId = tp.stem().string();
			if (sceneId.empty())
				sceneId = "scene";
		}
	} else if (inVfs.empty()) {
		auto mapped = SceneGltf(sceneId);
		if (!mapped) {
			tool::err() << "Unknown --scene '" << sceneId
				<< "' (sponza | bistro | bistro-niagara) or use --in-vfs"
				<< " or --tour" << std::endl;
			return 2;
		}
		inVfs = *mapped;
	}
	if (args.out.empty()) {
		// Default output NAME: RANDOM MINTED (a fresh guid). Note the
		// name alone does not request the world bake — --out explicit
		// is the request flag (outExplicit); a bare --tour run bakes
		// GI volumes only and exits at the GI-volume block. When the world does
		// bake (an explicit --out, or the editor's minted gargs.out),
		// the stamp carries the artifact path in the tour's giAsset
		// records — no stem-keyed default anywhere (that convention is
		// what the records retired).
		args.out = (std::filesystem::path(args.contentDir) / "gi"
			/ (smvk::tour::GenerateUuid() + ".sggi")).generic_string();
	}

	tool::out() << "GiBaker: scene '" << sceneId << "' from "
		<< (haveTour ? args.tour : inVfs)
		<< " -> " << args.out << "\n";

	// GPU-free offline-mode Cluster. --tour builds the DEFINITION (empty
	// root + reference bind + command replay, host-path resolution —
	// identical to the editor / PakCooker, so authored GI volume nodes
	// land on the same deterministic ids); otherwise the legacy VFS
	// glTF import.
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetOfflineMode(true);
	smvk::tour::Doc tour;
	std::vector<uint32_t> createdGiVolumes; // family refs (the stamp's map)
	if (haveTour) {
		std::string terr;
		std::vector<std::string> twarn;
		if (!smvk::tour::LoadTour(args.tour, tour, terr, &twarn)) {
			tool::err() << "Tour load failed: " << terr << std::endl;
			return 1;
		}
		for (const std::string& w : twarn)
			tool::err() << "Tour warning: " << w << std::endl;
		try {
			addNode(cluster->scene, -1, 0); // empty scene root
			if (!tour.references.empty())
				ApplySceneReferences(*cluster, tour,
					smvk::vfs::BaseDir(tour.references[0].source),
					nullptr, nullptr,
					nullptr /* HOST bind — editor parity */,
					smvk::MeshOptConfig{}, smvk::MeshletConfig{},
					0, nullptr, nullptr, nullptr,
					true, nullptr, nullptr, nullptr, nullptr,
					/*contentRoot=*/args.contentDir);
			std::vector<std::string> rwarn;
			std::string rerr;
			if (!smvk::tour::ApplyTourToCluster(*cluster, tour, &rwarn,
					&rerr, true, &createdGiVolumes)) {
				tool::err() << "Tour replay failed: " << rerr << std::endl;
				return 1;
			}
			for (const std::string& w : rwarn)
				tool::err() << "Tour replay warning: " << w << std::endl;
			tool::out() << "Tour replayed: " << tour.commands.size()
				<< " commands\n";
		} catch (const std::exception& e) {
			tool::err() << "Definition build failed: " << e.what()
				<< std::endl;
			return 1;
		}
	} else {
		// ARTIFACT build (glTF is IMPORT-ONLY — the GltfToClusterFromVfs
		// in-bake glTF import is GONE): --in-vfs names the DERIVED
		// .geometry blob; the manifest + the .material siblings resolve
		// beside it through the VFS. Open/drop the .gltf in the editor (or
		// run the MeshImporter) to derive the artifacts beside the source.
		try {
			std::string artPath = smvk::NormalizeAssetUri(inVfs).Str();
			if (artPath.empty())
				artPath = inVfs; // host-style path (as-is)
			auto artBytes = vfs.TryRead(artPath);
			if (!artBytes || artBytes->empty()) {
				tool::err() << "Artifact read failed: '" << inVfs
					<< "' not in VFS (want the derived .geometry — import "
					"the source glTF first)" << std::endl;
				return 1;
			}
			smvk::assets::MeshAsset meshAsset;
			std::string aerr;
			if (!smvk::assets::LoadMeshAssetBytes(artBytes->data(),
					artBytes->size(), meshAsset, aerr)) {
				tool::err() << "Artifact parse failed: '" << inVfs
					<< "': " << aerr << " (re-import the source glTF)"
					<< std::endl;
				return 1;
			}
			smvk::assets::AssetManifest meshManifest;
			{
				std::string stem = smvk::vfs::FileName(artPath);
				const size_t mdot = stem.rfind(".geometry");
				const size_t mdotLegacy = stem.rfind(".mesh");
				if (mdot != std::string::npos)
					stem = stem.substr(0, mdot);
				else if (mdotLegacy != std::string::npos)
					stem = stem.substr(0, mdotLegacy);
				if (auto metaBytes = vfs.TryRead(smvk::vfs::Join(
						smvk::vfs::BaseDir(artPath), stem + ".gltf.meta"))) {
					if (!metaBytes->empty()) {
						std::string merr;
						smvk::assets::LoadAssetManifestText(
							std::string(metaBytes->begin(),
								metaBytes->end()),
							meshManifest, merr);
					}
				}
			}
			auto readSibling =
				[&](const std::string& file)
				-> std::optional<std::vector<uint8_t>> {
				const std::string p = smvk::vfs::Join(
					smvk::vfs::BaseDir(artPath), file);
				if (vfs.Exists(p))
					return vfs.TryRead(p);
				return std::nullopt;
			};
			addNode(cluster->scene, -1, 0); // empty scene root (editor parity)
			auto texSrc = std::make_shared<VfsTextureSource>(&vfs, smvk::vfs::BaseDir(artPath));
			const size_t built = BuildFromMeshAsset(*cluster, nullptr,
				meshAsset, meshManifest, artPath, 0, texSrc, true,
				nullptr, readSibling);
			if (built == 0) {
				tool::err() << "Artifact build failed: '" << inVfs
					<< "' built 0 nodes (re-import the source glTF)"
					<< std::endl;
				return 1;
			}
			tool::out() << "Artifact built: " << built << " nodes\n";
		} catch (const std::exception& e) {
			tool::err() << "Artifact build failed: " << e.what() << std::endl;
			return 1;
		}
	}

	// The tour stamp (the bake owner's write-back): mirror the replayed
	// scene's bindings onto the tour's giAsset records, re-save, and
	// DELETE the superseded artifacts (record-exact — bake outputs carry
	// RANDOM minted names, so the old set-difference is the only exact
	// list; a kept binding keeps its file by construction). Runs at
	// every SUCCESS exit when --tour was given; a failed bake never
	// stamps (the old records stand, the artifacts on disk are the last
	// good bake).
	auto stampTour = [&]() -> bool {
		if (!haveTour)
			return true;
		std::string serr;
		std::vector<std::string> superseded;
		if (!smvk::tour::StampGiAssets(tour, cluster->scene,
				createdGiVolumes, &serr, &superseded)) {
			tool::err() << "Tour giAsset stamp failed: " << serr
				<< std::endl;
			return false;
		}
		std::string saveErr;
		if (!smvk::tour::SaveTour(args.tour, tour, saveErr)) {
			tool::err() << "Tour re-save failed: " << saveErr
				<< std::endl;
			return false;
		}
		int deleted = 0;
		for (const std::string& vfs : superseded) {
			const std::filesystem::path host =
				std::filesystem::path(args.contentDir) / vfs;
			std::error_code ec;
			if (std::filesystem::remove(host, ec))
				++deleted;
		}
		tool::out() << "Tour stamped: " << tour.giAssets.size()
			<< " giAsset binding(s) -> " << args.tour
			<< (superseded.empty() ? std::string()
				: " (deleted " + std::to_string(deleted)
					+ "/" + std::to_string(superseded.size())
					+ " superseded artifact(s))")
			<< "\n";
		return true;
	};

	// Authored convex volumes (editor Create > Convex volume, tour
	// GI volume nodes): the definition's bake inputs. The node's WORLD
	// transform carries the box (translate = center, basis lengths =
	// full extents, the NORMALIZED basis = the box axes — ROTATION
	// supported: an OBB's six planes, GiVolumeBoxPlanesRotated); the node-local
	// def carries the bake params (priority/spacing/fades). The dense
	// probe lattice stays axis-aligned over the OBB's AABB (Bounds);
	// the plane weights carve the rotated hull.
	std::vector<GiVolumeSpecVolume> tourGiVolumes;
	for (const auto& [node, def] : cluster->scene.giVolumeForNode) {
		if (node >= cluster->scene.globalTransform.size())
			continue;
		GiVolumeSpecVolume v{};
		const glm::mat4& g = cluster->scene.globalTransform[node];
		const glm::vec3 c(g[3]);
		glm::vec3 half;
		glm::mat3 axes;
		for (int i = 0; i < 3; ++i) {
			const float len = glm::length(g[i]);
			half[i] = 0.5f * len;
			axes[i] = (len > 1e-6f) ? glm::vec3(g[i]) / len
				: glm::vec3(0.0f);
		}
		v.def.name = getNodeName(cluster->scene, int(node));
		if (v.def.name.empty())
			v.def.name = "givolume" + std::to_string(node);
		v.def.priority = def.priority;
		v.spacing = def.spacing;
		v.node = node;
		v.def.planes = smvk::gi::GiVolumeBoxPlanesRotated(c, axes, half,
			def.insideFade, def.outsideFade);
		tourGiVolumes.push_back(std::move(v));
	}
	if (!tourGiVolumes.empty())
		tool::out() << "  authored GI volumes: " << tourGiVolumes.size()
			<< " (tour nodes)\n";

	// ---- The invalidation model's inputs (Engine/GI/bake_inputs.h):
	// the node-authored bake config (the exportSettings node is the
	// authoring surface; the CLI flag is an explicit override whose
	// records then mismatch the pak gate BY DESIGN) + the geometry
	// merkle (the 1 m cell grid over the traced static content). The
	// per-unit freshness checks below skip re-bakes whose recorded
	// input hash still matches.
	::SceneExportSettings esNode{};
	const bool hasEsNode = haveTour
		&& smvk::tour::ExportSettingsFromDoc(tour, esNode, nullptr);
	const bool maskTransmission = args.maskTransmissionSet
		? args.maskTransmission
		: (hasEsNode ? esNode.maskTransmission : false);
	smvk::gi::BakeCellGrid bakeGrid;
	if (haveTour) {
		bakeGrid = smvk::gi::BuildBakeCellGrid(*cluster);
		tool::out() << "  bake-input grid: " << bakeGrid.meshCount
			<< " meshes, " << bakeGrid.cells.size() << " cells (1 m)\n";
	}

	// Optional texture averaging (role-agnostic per-texture average:
	// baseColor tints albedo, emissive tints emission, metallicRoughness
	// supplies the effective metallic like the runtime shader).
	smvk::gi::GiSceneBuildOptions buildOpts{};
	if (args.textures) {
		uint32_t averaged = 0;
		uint32_t textured = 0;
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
			// Full decoded texel data: the trace samples the baseColor
			// texture at the interpolated hit UV (AlbedoAt). Irrelevant
			// in white-world .sggi bakes (uniformAlbedo overrides it),
			// live for the textured path + the GI-volume bakes.
			if (auto data = smvk::gi::GiDecodeKtxTexelData(
					std::span<const uint8_t>(*src),
					smvk::gi::kGiBakeTexelMaxDim)) {
				buildOpts.texelData[handle.index()] = std::move(*data);
				++textured;
			}
		}
		tool::out() << "  textures: averaged " << averaged << " ("
			<< cluster->ListTextureUris().size() << " uris, decoded "
			<< textured << ")\n";
	}

	// Environment: the TOUR'S ENV BLOCK when one replayed (the editor
	// always writes env.default.* — SceneEnv, lux-native) so the bake sees
	// exactly the sun/sky the pak and every other bake use (ONE authored
	// sun); the CLI --sun-*/--sky-intensity defaults stay the no-tour
	// fallback. The analytic sky is baked on open-sky rays (TraceIncoming),
	// so the sky intensity is meaningful; emissive stays runtime-only.
	if (!args.emissive)
		tool::out() << "  note: --emissive off (emissive is runtime-only)\n";
	if (cluster->scene.envAuthored) {
		const SceneEnv& se = cluster->scene.environment;
		buildOpts.env.sunAzimuthDeg = se.sunAzimuth;
		buildOpts.env.sunElevationDeg = se.sunElevation;
		buildOpts.env.sunIntensity = se.sunIntensity;
		buildOpts.env.skyIntensity = se.skyIntensity;
		tool::out() << "  env block (tour): az=" << se.sunAzimuth
			<< " el=" << se.sunElevation << " I=" << se.sunIntensity
			<< " lux sky=" << se.skyIntensity << "\n";
	}
	else {
		buildOpts.env.sunAzimuthDeg = args.sunAzimuth;
		buildOpts.env.sunElevationDeg = args.sunElevation;
		buildOpts.env.sunIntensity = args.sunIntensity;
		buildOpts.env.skyIntensity = args.skyIntensity;
		tool::out() << "  env defaults (no tour block): az=" << args.sunAzimuth
			<< " el=" << args.sunElevation << " I=" << args.sunIntensity
			<< " lux sky=" << args.skyIntensity << "\n";
	}
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
		tool::out() << "  uniform albedo: (" << uv.x << "," << uv.y << "," << uv.z << ")\n";
	}

	smvk::gi::GiTraceScene traceScene;
	std::string err;
	buildOpts.traceBackendEmbree = (args.backend == "embree");
	// Weave-transmissive cloth (the effective maskTransmission — the
	// node-authored value unless the CLI overrode it, resolved above):
	// the GI field sees the tinted light pools behind the banners; the
	// tint stays REAL in the white-world bake (the tint through the
	// cloth is not its diffuse albedo - see GiSceneBuildOptions::
	// maskTransmissive).
	buildOpts.maskTransmissive = maskTransmission;
	tool::out() << "  mask transmission: "
		<< (maskTransmission ? "on" : "off")
		<< (args.maskTransmissionSet && hasEsNode
			&& args.maskTransmission != esNode.maskTransmission
			? " (CLI override - the records will mismatch the pak gate; "
				"author the exportSettings node instead)"
			: "") << "\n";
	tool::out() << "  trace backend: "
		<< (buildOpts.traceBackendEmbree ? "embree" : "builtin") << "\n";
	smvk::gi::BuildTraceSceneFromCluster(*cluster, buildOpts, traceScene, err);
	if (!err.empty()) {
		tool::err() << "Scene build failed: " << err << std::endl;
		return 1;
	}
	// CLI --spot specs APPEND to the scene's STATIC-flagged lights already
	// harvested into traceScene by BuildTraceSceneFromCluster above
	// (GiSceneBuildOptions::staticLights, default on — both sets bake).
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
			tool::err() << "Bad --spot spec: " << spec << "\n";
			return 2;
		}
		spot.color = col * candela;
		spot.cosInner = std::cos(glm::radians(std::min(innerDeg, outerDeg)));
		spot.cosOuter = std::cos(glm::radians(std::max(innerDeg, outerDeg)));
		traceScene.spots.push_back(spot);
	}
	tool::out() << "  trace scene: " << traceScene.Bvh().TriangleCount()
		<< " tris, " << traceScene.materials.size() << " materials, "
		<< traceScene.spots.size() << " spots\n";

	// Trace-scene world AABB (bake source of truth for box placement).
	glm::vec3 sceneMin(1e30f), sceneMax(-1e30f);
	for (const glm::vec3& v : traceScene.positions) {
		sceneMin = glm::min(sceneMin, v);
		sceneMax = glm::max(sceneMax, v);
	}
	tool::out() << "  scene bounds: (" << sceneMin.x << "," << sceneMin.y << "," << sceneMin.z
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
		tool::out() << "  interior scan y=" << yEye << " step=" << step
			<< " ('#' solid, '.' open, 's' canyon, 'I' enclosed):\n";
		for (int iz = nz - 1; iz >= 0; --iz) {
			std::string row;
			row.reserve(size_t(nx));
			for (int ix = 0; ix < nx; ++ix) {
				const int c = cls[size_t(iz) * nx + ix];
				row.push_back(c < 0 ? '#' : (c == 0 ? '.' : (c == 1 ? 's' : 'I')));
			}
			tool::out() << "    z=" << (sceneMin.z + (iz + 0.5f) * step) << " " << row << "\n";
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
		tool::out() << "  enclosed components: " << nComp << "\n";
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
			tool::out() << "    comp " << c << " cells=" << count
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
		// Bake-asset identity for the regular volume (see
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
				tool::err() << "Unknown --reg-preset '" << args.regPreset
					<< "' (bistro-bar | bistro-street | scene)\n";
				return 2;
			}
		}
		if (!haveBox) {
			tool::err() << "Regular bake needs --reg-min/--reg-max or --reg-preset\n";
			return 2;
		}
		obb.center = (regMin + regMax) * 0.5f;
		obb.halfExtents = (regMax - regMin) * 0.5f;
		if (!parseDims(args.regDims, obb.dims)) {
			tool::err() << "Bad --reg-dims (want nx,ny,nz)\n";
			return 2;
		}
		if (args.regSpacing > 0.0f) {
			const glm::vec3 ext = regMax - regMin;
			for (int a = 0; a < 3; ++a)
				obb.dims[a] = std::max(1u, uint32_t(std::ceil(ext[a] / args.regSpacing)));
		}
		tool::out() << "  regular OBB: center=(" << obb.center.x << "," << obb.center.y
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
				[](const char* line) { tool::out() << line; })) {
			tool::err() << "Regular bake failed\n";
			return 1;
		}
		{
			std::filesystem::path out(args.regularOut);
			std::error_code ec;
			std::filesystem::create_directories(out.parent_path(), ec);
		}
		std::string rerr;
		if (!rvol.WriteFile(args.regularOut, rerr)) {
			tool::err() << "Regular write failed: " << rerr << std::endl;
			return 1;
		}
		tool::out() << "Wrote " << args.regularOut << " ("
			<< std::filesystem::file_size(args.regularOut) << " B on disk)\n";
		if (args.verify) {
			smvk::gi::SgRegularVolume reloaded;
			std::string verr;
			if (!smvk::gi::SgRegularVolume::ReadFile(args.regularOut, reloaded, verr)) {
				tool::err() << "Regular verify: reload failed: " << verr << std::endl;
				return 1;
			}
			tool::out() << "  verify: " << reloaded.VoxelCount() << " voxels reloaded OK\n";
			if (reloaded.VoxelCount() != rvol.VoxelCount())
				return 1;
		}
		// Runtime manifest (stable path gi/<scene>.regmanifest): rows are
		// bare VFS paths (legacy "<guid> <path>" rows from old bakes are
		// still read by SplitManifestRow); existing rows merge by content
		// so hand edits survive rebakes.
		{
			const std::filesystem::path giDir =
				std::filesystem::path(args.contentDir) / "gi";
			const std::filesystem::path manifestPath =
				giDir / (sceneId + ".regmanifest");
			const std::string rowVfs = VfsRelPath(args.contentDir,
				args.regularOut);
			if (rowVfs.compare(0, 3, "gi/") != 0)
				tool::out() << "Regular manifest row '" << rowVfs
					<< "' is outside the content tree - the runtime can "
					"only resolve content-relative rows\n";
			const std::string row = rowVfs;
			std::vector<std::string> merged = MergeManifestRows(
				manifestPath.generic_string(), row, "");
			std::error_code ec;
			std::filesystem::create_directories(
				manifestPath.parent_path(), ec);
			{
				std::ofstream out(manifestPath,
					std::ios::binary | std::ios::trunc);
				if (!out) {
					tool::err() << "Regular manifest write failed: "
						<< manifestPath.generic_string() << std::endl;
					return 1;
				}
				out << "# regular GI volumes (GiBaker --regular-out; "
					"<vfsPath> rows, hand lines kept)\n";
				for (const std::string& l : merged)
					out << l << "\n";
			}
			tool::out() << "Manifest " << manifestPath.generic_string()
				<< ": " << merged.size() << " volume(s)\n";
		}
		// Fall through only when another stage was requested explicitly
		// (--out adaptive A/B or a convex bake pending below: --gi-volume-spec
		// or authored tour GI volumes). A regular-only run passes no --out.
		if (!args.outExplicit && args.giVolumeSpec.empty() && tourGiVolumes.empty()) {
			const auto t1 = std::chrono::steady_clock::now();
			tool::out() << "GiBaker total: "
				<< std::chrono::duration<double>(t1 - t0).count() << "s\n";
			return 0;
		}
	}

	// Convex-volume bake (probes-only pivot). Sources stack: authored
	// tour GI volume nodes (--tour, the definition) + hand-written
	// --gi-volume-spec entries. Bake each volume's dense probe grid by the
	// per-probe direct trace, write gi/<scene>_<name>.givolume and BIND the
	// artifact to its authored definition (the replayed scene's
	// SceneGiVolume::bakeSource — the stamp at the end of Run mirrors the
	// bindings onto the tour's giAsset records). Runs in addition to
	// whatever else was asked for; like --regular-out, a GI-volume-only run
	// (no --out) exits after this block.
	if (!args.giVolumeSpec.empty() || !tourGiVolumes.empty()) {
		std::vector<GiVolumeSpecVolume> specVols;
		std::string specErr;
		if (!args.giVolumeSpec.empty()
			&& !ParseGiVolumeSpec(args.giVolumeSpec, specVols, specErr)) {
			tool::err() << "Bad --gi-volume-spec: " << specErr << std::endl;
			return 2;
		}
		// Authored tour volumes lead (the definition is the source of
		// truth); --gi-volume-spec entries append for hand-tuning extras.
		specVols.insert(specVols.begin(),
			tourGiVolumes.begin(), tourGiVolumes.end());
		if (specVols.size() > 8) {
			tool::err() << "convex bake lists " << specVols.size()
				<< " volumes; the runtime manifest caps at 8\n";
			return 2;
		}
		const std::filesystem::path giVolumeDir =
			std::filesystem::path(args.contentDir) / "gi";
		for (GiVolumeSpecVolume& spec : specVols) {
			smvk::gi::GiVolumeBakeConfig cfg{};
			cfg.captureDirs = std::max(args.giVolumeDirs, 64u);
			cfg.maxBounces = args.bounces;
			cfg.captureSpacing = args.giVolumeCaptureSpacing > 0.0f
				? args.giVolumeCaptureSpacing : spec.captureSpacing;
			cfg.smoothingPasses = args.giVolumeSmoothing;
			cfg.threadCount = args.threads;
			// The per-volume FRESHNESS CHECK (the invalidation model):
			// when the recorded bake-input hash matches the freshly
			// computed one, the re-trace is SKIPPED — the bound artifact
			// + record stand (the stamp re-issues them unchanged). This
			// is the editor's partial-bake path: an edit far outside a
			// volume's reach never re-traces it.
			if (spec.node != UINT32_MAX) {
				auto cvIt = cluster->scene.giVolumeForNode.find(spec.node);
				if (cvIt != cluster->scene.giVolumeForNode.end()
						&& !cvIt->second.bakeSource.empty()
						&& spec.node
							< cluster->scene.globalTransform.size()) {
					smvk::gi::GiVolumeBakeConfigHashInputs hc{};
					hc.captureDirs = cfg.captureDirs;
					hc.maxBounces = cfg.maxBounces;
					hc.captureSpacing = 0.0f; // the auto lattice (inert CLI)
					hc.smoothingPasses = cfg.smoothingPasses;
					hc.maskTransmission = maskTransmission;
					const std::string fresh = smvk::gi::GiVolumeBakeUnitHash(
						bakeGrid, *cluster, cvIt->second,
						cluster->scene.globalTransform[spec.node], hc);
					if (fresh == cvIt->second.bakeInputHash) {
						tool::out() << "  [GIV] volume '" << spec.def.name
							<< "': inputs unchanged — SKIPPED (the bound "
							"bake stands)\n";
						continue;
					}
				}
			}
			tool::out() << "  [GIV] volume '" << spec.def.name << "': priority="
				<< spec.def.priority << " spacing=" << spec.spacing
				<< " planes=" << spec.def.planes.size() << "\n";
			smvk::gi::GiVolume vol;
			smvk::gi::GiVolumeBakeStats stats;
			if (!smvk::gi::BakeGiVolume(traceScene, spec.def, spec.spacing,
					cfg, vol, stats,
					[](const char* line) { tool::out() << line; })) {
				tool::err() << "Convex bake failed for '" << spec.def.name
					<< "'\n";
				return 1;
			}
			// The artifact name: RANDOM MINTED (a fresh guid per
			// bake). The binding record IS the identity — the file
			// name carries no scene/stem information to go stale (a
			// rebake never collides with or overwrites the old
			// artifact; the stamp's supersede list retires it).
			const std::string outPath = (giVolumeDir
				/ (smvk::tour::GenerateUuid() + ".givolume"))
				.generic_string();
			{
				std::filesystem::path out(outPath);
				std::error_code ec;
				std::filesystem::create_directories(out.parent_path(), ec);
			}
			std::string werr;
			if (!vol.WriteFile(outPath, werr)) {
				tool::err() << "Convex write failed: " << werr << std::endl;
				return 1;
			}
			tool::out() << "Wrote " << outPath << " ("
				<< std::filesystem::file_size(outPath) << " B on disk)\n";
			// The HARD identification (as meshes do): the baked volume
			// binds to ITS authored definition on the replayed scene —
			// the stamp at the end of Run mirrors the scene onto the
			// tour's giAsset records and retires the superseded old
			// artifact BY RECORD (the path is RANDOM MINTED, so the
			// rebake never overwrote anything). Hand-tuning --gi-volume-spec
			// extras carry no node and stay unbound (the pak embeds
			// exactly what the scene binds).
			if (spec.node != UINT32_MAX) {
				auto cvIt = cluster->scene.giVolumeForNode.find(spec.node);
				if (cvIt == cluster->scene.giVolumeForNode.end()) {
					tool::err() << "baked volume's node "
						<< spec.node << " not in the replayed scene — "
						"binding dropped\n";
					return 1;
				}
				cvIt->second.bakeSource =
					VfsRelPath(args.contentDir, outPath);
				cvIt->second.bakeGuid.clear();
				// The bake-input hash AT BAKE TIME (the pak gate's
				// freshness check + the next run's skip).
				if (spec.node < cluster->scene.globalTransform.size()) {
					smvk::gi::GiVolumeBakeConfigHashInputs hc{};
					hc.captureDirs = cfg.captureDirs;
					hc.maxBounces = cfg.maxBounces;
					hc.captureSpacing = 0.0f;
					hc.smoothingPasses = cfg.smoothingPasses;
					hc.maskTransmission = maskTransmission;
					cvIt->second.bakeInputHash =
						smvk::gi::GiVolumeBakeUnitHash(bakeGrid, *cluster,
							cvIt->second,
							cluster->scene.globalTransform[spec.node],
							hc);
				}
				else
					cvIt->second.bakeInputHash.clear();
			}
			else {
				tool::out() << "  (unbound: no authored node for '"
					<< spec.def.name
					<< "' — a --gi-volume-spec extra, diagnostic-only)\n";
			}
			if (args.verify) {
				smvk::gi::GiVolume reloaded;
				std::string verr;
				if (!smvk::gi::GiVolume::ReadFile(outPath, reloaded, verr)) {
					tool::err() << "Convex verify: reload failed: " << verr
						<< std::endl;
					return 1;
				}
				tool::out() << "  verify: " << reloaded.VoxelCount()
					<< " voxels, " << reloaded.def.planes.size()
					<< " planes reloaded OK\n";
				if (reloaded.VoxelCount() != vol.VoxelCount())
					return 1;
			}
		}
		// NO givolumemanifest anymore: the scene's HARD per-volume bindings
		// (updated above, mirrored onto the tour by the stamp at the
		// end of Run) REPLACE the manifest round-trip — the pak cooker
		// embeds exactly what the scene binds, the runtime loads by
		// binding. The old gi/<scene>.givolumemanifest channels (loose
		// discovery + legacy paks) are retired with the stem
		// convention.
		// A GI-volume-only run (no --out) is done; --out falls through to the
		// adaptive world-field bake (the .sggi and .givolume files pair up).
		if (!args.outExplicit) {
			if (!stampTour())
				return 1;
			const auto t1 = std::chrono::steady_clock::now();
			tool::out() << "GiBaker total: "
				<< std::chrono::duration<double>(t1 - t0).count() << "s\n";
			return 0;
		}
	}

	// Lightmap-chain diagnostic: run the xatlas chart + bake chain
	// (GenerateLightmapChartsXatlas -> AppendXatlasLevelToAtlas ->
	// BakeSgLightmapChunks) on the first N chartable meshes and print
	// per-mesh stats (texel counts, world positions, incoming radiance).
	// Reproduces cook-side lightmap faults without a pak cook.
	if (args.lmProbe > 0) {
		using namespace smvk::gi;
		::recalculateGlobalTransforms(cluster->scene);
		SgLightmapXatlasOptions xopts{};
		xopts.metersPerTexel = 1.0f / 5.66f; // converged night cook density (world)
		xopts.gutterTexels = 1;
		SgLightmapAtlas atlas{};
		struct LmWork { std::string name; glm::mat4 g; const smvk::StaticMeshUpload* up; };
		std::vector<LmWork> works;
		std::vector<SgLightmapXatlasMeshInput> xins;
		std::vector<uint32_t> keys;
		uint32_t count = 0;
		for (const auto& [node, meshIdx] : cluster->scene.meshForNode) {
			if (count >= args.lmProbe)
				break;
			if (!IsVisibleNode(cluster->scene, int(node)))
				continue; // hidden override: not charted
			auto matIt = cluster->scene.materialForNode.find(node);
			if (matIt == cluster->scene.materialForNode.end())
				continue;
			const smvk::ClusterMaterialHandle mh = cluster->materials.getHandle(matIt->second);
			if (!mh.valid())
				continue;
			const smvk::Material* mat = cluster->materials.get(mh);
			if (!mat || mat->alphaMode == smvk::AlphaMode::Blend)
				continue;
			if (!mat->castsShadows && mat->alphaMode != smvk::AlphaMode::Mask)
				continue;
			const smvk::ClusterStaticMeshHandle handle = cluster->staticMeshes.getHandle(meshIdx);
			const smvk::StaticMeshUpload* up = cluster->GetRetainedUpload(handle);
			if (!up || up->vertices.empty())
				continue;
			const glm::mat4 g = cluster->scene.globalTransform[node];
			const float ws = std::max({ glm::length(glm::vec3(g[0])),
				glm::length(glm::vec3(g[1])), glm::length(glm::vec3(g[2])) });
			LmWork w{};
			w.name = getNodeName(cluster->scene, int(node));
			w.up = up;
			w.g = g;
			works.push_back(std::move(w));
			SgLightmapXatlasMeshInput in{};
			in.upload = up;
			in.meshKey = node;
			in.densityScale = (ws > 1e-6f) ? ws : 1.0f;
			xins.push_back(in);
			keys.push_back(node);
			++count;
		}
		tool::out() << "  [lm-probe] charting " << works.size() << " meshes\n";
		SgLightmapXatlasLevel lvl;
		std::string xerr;
		if (!GenerateLightmapChartsXatlas(xins, xopts, lvl, xerr)) {
			tool::out() << "  [lm-probe] xatlas chart failed: " << xerr << "\n";
			return 1;
		}
		// Post-shrink AABBs can overlap (concave charts); repack
		// big->small into disjoint placements before Append. An
		// InternalError here is a packer bug: report it as such
		// instead of a generic overflow.
		if (RepackXatlasLevel(lvl, 4096, 4096, xerr)
			== RepackStatus::InternalError) {
			tool::out() << "  [lm-probe] repack INTERNAL ERROR (packer "
				"bug, please report): " << xerr << "\n";
			return 1;
		}
		if (xerr.empty() == false && lvl.meshes.empty() == false
			&& lvl.atlasWidth == 0) {
			// Infeasible at these caps: report and stop (no density
			// convergence in the probe tool).
			tool::out() << "  [lm-probe] repack failed: " << xerr << "\n";
			return 1;
		}
		std::vector<uint32_t> bases;
		if (!AppendXatlasLevelToAtlas(atlas, lvl, keys, 12u,
				4096, 4096, bases, xerr)) {
			tool::out() << "  [lm-probe] atlas append failed: " << xerr << "\n";
			return 1;
		}
		tool::out() << "  [lm-probe] charted " << works.size() << " meshes (atlas "
			<< atlas.width << "x" << atlas.height << ")\n";
		// Bake every mesh's FIRST chunk; print stats.
		std::vector<std::vector<SgLightmapTexel>> texels(works.size());
		std::vector<SgLightmapChunkBatch> batches(works.size());
		bool anyBatch = false;
		for (size_t wi = 0; wi < works.size(); ++wi) {
			if (!lvl.meshes[wi].ok || lvl.meshes[wi].chart.charts.empty()) {
				tool::out() << "  [lm-probe] '" << works[wi].name
					<< "': unchartable\n";
				continue;
			}
			const glm::mat3 g3(works[wi].g);
			for (const auto& t : lvl.meshes[wi].chart.charts[0].texels) {
				SgLightmapTexel wt = t;
				wt.position = glm::vec3(works[wi].g * glm::vec4(t.position, 1.0f));
				wt.normal = glm::normalize(g3 * t.normal);
				texels[wi].push_back(wt);
			}
			SgLightmapChunkBatch b{};
			b.payload = &atlas.payloads[bases[wi]];
			b.texels = &texels[wi];
			b.meshKey = uint32_t(wi) + 1;
			b.chunkMeta = &atlas.chunks[bases[wi]];
			batches[wi] = b;
			anyBatch = true;
		}
		SgLightmapBakeConfig lcfg{};
		lcfg.raysPerTexel = 128;
		lcfg.maxBounces = 3;
		SgLightmapBakeStats ls{};
		if (anyBatch) {
			std::vector<SgLightmapChunkBatch> live;
			for (const auto& b : batches)
				if (b.payload)
					live.push_back(b);
			if (!BakeSgLightmapChunks(traceScene, 12u,
					std::span<const SgLightmapChunkBatch>(live), lcfg, ls, nullptr)) {
				tool::out() << "  [lm-probe] bake failed\n";
				return 1;
			}
		}
		for (size_t wi = 0; wi < works.size(); ++wi) {
			if (!lvl.meshes[wi].ok || lvl.meshes[wi].chart.charts.empty())
				continue;
			const auto& p = atlas.payloads[bases[wi]];
			const uint32_t lobes = std::min<uint32_t>(
				atlas.chunks[bases[wi]].lobeCount, 12u);
			double meanL0 = 0.0, maxL0 = 0.0;
			uint64_t nz = 0;
			for (size_t i = 0; i < size_t(p.width) * p.height; ++i) {
				const double l = 0.2126 * p.coeffs[i * lobes + 0].x
					+ 0.7152 * p.coeffs[i * lobes + 0].y
					+ 0.0722 * p.coeffs[i * lobes + 0].z;
				meanL0 += l;
				maxL0 = std::max(maxL0, l);
				if (l > 1e-6)
					++nz;
			}
			const size_t total = size_t(p.width) * p.height;
			const LmWork& w = works[wi];
			const auto& ch0 = lvl.meshes[wi].chart.charts[0];
			const glm::vec3 firstPos = ch0.texels.empty()
				? glm::vec3(0.0f)
				: ch0.texels.front().position;
			const glm::vec3 wpos = glm::vec3(w.g * glm::vec4(firstPos, 1.0f));
			tool::out() << "  [lm-probe] '" << w.name << "' chunk "
				<< p.width << "x" << p.height
				<< " baked=" << ch0.texels.size()
				<< " nz=" << nz
				<< " meanL0=" << (meanL0 / double(std::max(total, size_t(1))))
				<< " maxL0=" << maxL0
				<< " firstTexelWorld=(" << wpos.x << "," << wpos.y << "," << wpos.z << ")"
				<< "\n";
			// Trace a few of the first texel's fib directions and report
			// what comes back (sky / geometry / zero).
			if (!ch0.texels.empty()) {
				const SgLightmapTexel& t0 = ch0.texels.front();
				const glm::vec3 nrm = glm::normalize(glm::vec3(w.g * glm::vec4(t0.normal, 0.0f)));
				const glm::vec3 origin = glm::vec3(w.g * glm::vec4(t0.position, 1.0f))
					+ nrm * 1e-3f;
				int misses = 0;
				glm::vec3 acc(0.0f);
				for (uint32_t i = 0; i < 64; ++i) {
					const float y = 1.0f - 2.0f * (float(i) + 0.5f) / 64.0f;
					const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
					const float phi = float(i) * 2.39996323f;
					const glm::vec3 d(std::cos(phi) * r, y, std::sin(phi) * r);
					smvk::gi::Pcg32 rng(1234u + i);
					acc += traceScene.TraceIncoming(origin, d, rng, 3);
					smvk::gi::TriangleBvh::Hit h;
					if (!traceScene.Bvh().Raycast(origin, d, 1e9f, h))
						++misses;
				}
				tool::out() << "    first texel: 64-ray mean lum="
					<< ((0.2126 * acc.x + 0.7152 * acc.y + 0.0722 * acc.z) / 64.0)
					<< " open-sky rays=" << misses << "/64\n";
			}
		}
		return 0;
	}

	// The world-field FRESHNESS CHECK (the invalidation model): when the
	// recorded bake-input hash matches the freshly computed one, the
	// whole octree bake is SKIPPED — the bound artifact + record stand
	// (the stamp re-issues them unchanged; nothing is written, so the
	// editor's minted --out path never even materializes as an orphan).
	// The editor's partial-bake path: a GI-volume-only change skips the
	// minutes-long world bake entirely.
	if (haveTour && !cluster->scene.giWorldSource.empty()) {
		const smvk::gi::WorldBakeConfigHashInputs wc =
			smvk::gi::CanonicalWorldCfgHash(maskTransmission);
		const std::string fresh = smvk::gi::WorldBakeUnitHash(
			bakeGrid, *cluster, wc);
		if (fresh == cluster->scene.giWorldInputHash
				&& !cluster->scene.giWorldInputHash.empty()) {
			tool::out() << "world-field bake SKIPPED (inputs unchanged; "
				"the bound artifact stands)\n";
			if (!stampTour())
				return 1;
			const auto t1 = std::chrono::steady_clock::now();
			tool::out() << "GiBaker total: "
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
			[](const char* line) { tool::out() << line; })) {
		tool::err() << "Bake failed" << std::endl;
		return 1;
	}

	// Dense runtime layers (the BC6H Texture3D path): bake the
	// visibility-weighted partition-of-unity blend + BC6H-compress it
	// HERE, at cook time — the .sggi ships finished payloads and the
	// runtime only creates images (no encoder, no half-float
	// intermediates, no repeated allocations at load; the exact baked
	// result is deterministic).
	if (!volume.BakeDenseLayers()) {
		tool::err() << "Dense layer bake failed (no usable levels)" << std::endl;
		return 1;
	}
	{
		uint32_t denseLvls = 0;
		size_t denseBytes = 0;
		for (const auto& dl : volume.denseLevels)
			if (dl.present) {
				++denseLvls;
				denseBytes += dl.Bytes();
			}
		tool::out() << "Dense layers: " << denseLvls << " levels ("
			<< (denseBytes / 1024) << " KiB BC6H+R16F)\n";
		for (uint32_t lvl = 0; lvl < volume.denseLevels.size(); ++lvl)
			if (volume.denseLevels[lvl].present)
				tool::out() << "  dense level " << lvl << ": "
					<< volume.denseLevels[lvl].dims[0] << "x"
					<< volume.denseLevels[lvl].dims[1] << "x"
					<< volume.denseLevels[lvl].dims[2] << " voxels\n";
	}

	{
		std::filesystem::path out(args.out);
		std::error_code ec;
		std::filesystem::create_directories(out.parent_path(), ec);
	}
	std::string writeErr;
	if (!volume.WriteFile(args.out, writeErr)) {
		tool::err() << "Write failed: " << writeErr << std::endl;
		return 1;
	}

	tool::out() << "Wrote " << args.out << " ("
		<< std::filesystem::file_size(args.out) << " B on disk)\n";
	// The world-field binding (the same hard-identification stamp as the
	// GI volumes): the baked octree binds to the scene on the replayed
	// state — the tour stamp at the exit mirrors it into the giAsset
	// records. VfsRelPath is the content-relative form the records carry.
	cluster->scene.giWorldSource = VfsRelPath(args.contentDir, args.out);
	cluster->scene.giWorldGuid.clear();
	// The bake-input hash AT BAKE TIME (the pak gate's freshness check +
	// the next run's skip). The canonical config derivation matches the
	// gate's (the CLI-overridden knobs mismatch BY DESIGN).
	cluster->scene.giWorldInputHash = smvk::gi::WorldBakeUnitHash(
		bakeGrid, *cluster,
		smvk::gi::CanonicalWorldCfgHash(maskTransmission));
	tool::out() << "  world bake-input hash: "
		<< cluster->scene.giWorldInputHash << " (env="
		<< smvk::gi::BakeEnvHash(cluster->scene) << " lights="
		<< smvk::gi::BakeLightsHash(*cluster) << " grid="
		<< bakeGrid.root << ")\n";
	tool::out() << "  probes=" << stats.probeCount
		<< " waves=" << stats.refineWaves
		<< " paths=" << stats.pathsTraced
		<< " seconds=" << stats.seconds << "\n";
	if (stats.probeBudgetHits > 0)
		tool::out() << "  budget: " << stats.probeBudgetHits
			<< " leaves not subdivided (cap " << args.maxProbes
			<< ") — raise --max-probes for finer coverage\n";
	for (uint32_t lvl = 0; lvl < smvk::gi::kSgGiMaxLevels; ++lvl)
		if (stats.probesPerLevel[lvl])
			tool::out() << "  level " << lvl << ": " << stats.probesPerLevel[lvl] << " probes\n";

	// Self-check: reload through the production reader and spot-check.
	if (args.verify) {
		smvk::gi::SgGiVolume reloaded;
		std::string verr;
		if (!smvk::gi::SgGiVolume::ReadFile(args.out, reloaded, verr)) {
			tool::err() << "Verify: reload failed: " << verr << std::endl;
			return 1;
		}
		if (reloaded.probes.size() != volume.probes.size()
			|| reloaded.levelCount != volume.levelCount) {
			tool::err() << "Verify: round-trip mismatch\n";
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
		tool::out() << "  verify: " << reloaded.probes.size()
			<< " probes reloaded, " << mismatches << " mismatches"
			<< (mismatches ? " BAD!" : " OK") << "\n";
		// Dense round-trip: the cooked payloads must survive byte-exact
		// (BC6H blocks + R16F halves — the runtime uploads them as-is).
		size_t denseMismatch = 0;
		if (reloaded.denseLevels.size() != volume.denseLevels.size()) {
			++denseMismatch;
		} else {
			for (size_t l = 0; l < volume.denseLevels.size(); ++l) {
				const auto& a = volume.denseLevels[l];
				const auto& b = reloaded.denseLevels[l];
				if (a.present != b.present) {
					++denseMismatch;
					continue;
				}
				if (!a.present)
					continue;
				if (a.dims[0] != b.dims[0] || a.dims[1] != b.dims[1]
					|| a.dims[2] != b.dims[2]) {
					++denseMismatch;
					continue;
				}
				for (uint32_t lb = 0; lb < smvk::gi::kSgLobeCount; ++lb)
					if (a.lobeBlocks[lb] != b.lobeBlocks[lb]) {
						++denseMismatch;
						break;
					}
				if (a.weight != b.weight)
					++denseMismatch;
			}
		}
		tool::out() << "  verify: dense levels byte-exact, "
			<< denseMismatch << " mismatches"
			<< (denseMismatch ? " BAD!" : " OK") << "\n";
		if (mismatches || denseMismatch)
			return 1;
	}

	// The stamp before the exit (see stampTour): the world field just
	// bound + every baked volume's record.
	if (!stampTour())
		return 1;
	const auto t1 = std::chrono::steady_clock::now();
	tool::out() << "GiBaker total: "
		<< std::chrono::duration<double>(t1 - t0).count() << "s\n";
	return 0;
}

} // namespace gibaker

// The CLI stays a thin interface over the library (same argv, same
// output, same exit codes). The lib build (GIBAKER_LIB_BUILD, linked
// into the editor) excludes main.
#ifndef GIBAKER_LIB_BUILD
int main(int argc, char** argv) {
	// Same live-pipe contract as before: editors that still spawn the
	// CLI stream this output line-by-line while the bake runs.
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;
	gibaker::Args args;
	if (!gibaker::ParseArgs(argc, argv, args)) {
		gibaker::Usage();
		return 2;
	}
	return gibaker::Run(args);
}
#endif
