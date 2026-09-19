// PakCooker: offline glTF -> .pak cooker that works exclusively on the VFS
// and needs NO GPU (cook-mode Cluster, see Cluster::SetCookMode).
//
//   PakCooker --content-dir content --in-vfs models/Sponza/sponza.gltf
//             --out-pak content/paks/sponza.pak [--meshopt on|off]
//             [--overdraw 1.05] [--verify on|off] [--dump-manifest on|off]
//             [--lods on|off] [--lod-ratios 0.6,0.3,0.12] [--lod-error 0.02]
//             [--compress on|off] [--lz4-accel 1]
//             [--lightmap on|off --lm-density 32 --lm-lobes 12
//              --lm-atlas-width 2048 --lm-gutter 1 --lm-budget 4194304
//              --lm-rays 1024
//              --sun-azimuth 30 --sun-elevation 55 --sun-intensity 10000
//              --lm-lod-lock attr|hard --lm-lod-audit on|off
//              --lm-lod-audit-tx 2.0 --lm-lod-audit-gate on|off
//              --lm-denoise on|off --lm-denoise-quality high|balanced
//              --lm-curve-target 7.0 --lm-curve-max 2.5
//              --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]
//
// --lightmap on bakes 2D SG lightmaps DURING the cook (sun + static spot
// NEE + analytic sky on open-sky rays, real materials, transmissive
// pass-through — see Engine/GI/bake.h), fills each mesh's texCoord2 with
// per-vertex lightmap UVs, stamps the mesh's atlas chunk id into the pak
// blob, and writes the atlas alongside the pak as
// <contentDir>/gi/<scene>.sglm (plus a raw-payload debug PNG
// <contentDir>/gi/<scene>_atlas.png — lobe 0 at the editor flat view's
// display mapping, see lm_png.h). No bake-side work is left to the
// runtime: the pak + .sglm are the complete lightmap payload. Baked
// pixels take the lightmap ALONE at runtime (the analytic sky IBL is
// the fallback for uncovered pixels only — the ambient sum starts
// empty, no blind ambient added on covered pixels).
//
// LOD + lightmap interaction: with --lods on, siblings are cooked AFTER
// the bake (CookLodSiblings), so lightmapped meshes simplify from their
// STAMPED uploads — surviving vertices keep exact LOD0 atlas UVs, chart
// ids ride the simplifier's error metric so collapses stay inside one
// lightmap chart, and every sibling shares LOD0's lobe count. The
// per-level audit (--lm-lod-audit, default on with --lightmap) projects
// sampled interior points of each sibling's triangles onto the LOD0
// surface and reports the uv drift in atlas texels (log-only; flip
// --lm-lod-audit-gate on to drop failing levels).
//   With --lm-charts xatlas the model is per-LOD atlases instead: LOD0
// bakes from the full-scene xatlas brute-force pack, then each sibling
// level charts + bakes its OWN rects (exact UVs for its simplified
// topology) stacked into the shared atlas — the audit is skipped
// (nothing to audit) and levels that overflow fall back to shared LOD0
// UVs. Either way meshlets are built once, after ALL stamping, from the
// final topology.
//
// Denoising: with --lightmap on (and --lm-denoise on, the default), the
// fitted per-lobe amplitude layers are denoised with Intel Open Image
// Denoise's RTLightmap filter (prebuilt release in external/oidn, linked
// into PakCooker only — engine and App never see OIDN). Denoised
// payloads are written into the .sglm; the runtime path is unchanged.
// --lm-rays stays independent: denoising recovers low-sample bakes, so
// try e.g. --lm-rays 128 for a ~8x faster trace.
//
// Curvature-weighted density (default on): --lm-density is the BASE
// request; smoothly-bending meshes chart at up to --lm-curve-max times
// the base so their normal-turn per texel stays under --lm-curve-target
// degrees (creases over 45 deg do not count — density cannot fix an
// authored edge). The atlas convergence takes the budget back from flat
// meshes, so the total stays inside --lm-budget: same texels, spent
// where normals turn fast. Uniform density reads as faceted/hard-normal
// shading on curved geometry (columns/arches at ~18 deg per texel).
//
// Reads the model JSON, .bin buffers and all .ktx2 textures through an
// OverlayVfs with a LooseBackend over --content-dir (the same lookup rules
// as the runtime), runs the meshopt cook pass, and writes a self-contained
// pak with embedded textures. Everything about the pak is already inside
// the pak — there is intentionally no sidecar manifest. --dump-manifest
// prints the same data to stdout, read back from the pak via PakReader so
// it cannot drift from the written bytes.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Engine/App/cluster.h>
#include <Engine/App/static_merge.h>
#include <Engine/Asset/tour.h>
#include <Engine/Scene/scene.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Asset/pak.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/lightmap.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <gltf_import/gltf.h>
#include <Engine/Vfs/vfs.h>

#include "lm_denoise.h"
#include "lm_png.h"

namespace {

struct Args {
	std::string contentDir = "content";
	std::string inVfs;
	std::string outPak;
	// glTF scene selection: -1 = all scenes (one Scene blob each, shared
	// asset pool); N = only that scene. --lightmap/--tour need exactly
	// one cooked scene (fail otherwise).
	int sceneIndex = -1;
	// .tour replay (editor scene overlay): applied to the cook cluster
	// right after import, before collapse/LOD/bake/serialize. CLI flags
	// win over tour cook defaults (tracked via explicitSet).
	std::string tour;
	std::unordered_set<std::string> explicitSet;
	bool meshopt = true;
	float overdraw = 1.05f;
	bool dumpManifest = false; // print pak contents to stdout (pak-sourced)
	bool verify = true; // re-open the written pak with PakReader (self-check)
	// Cooked LOD chain (meshopt_simplify, sibling blobs "<base>/lodN").
	bool lods = false;
	std::string lodRatios = "0.6,0.3,0.12";
	float lodError = 0.02f; // relative error budget per simplify call
	// Meshlet cook path (off by default; indirect-draw foundation only).
	bool meshlets = false;
	uint32_t maxVerts = 64;
	uint32_t maxTris = 124;
	float coneWeight = 0.0f;
	// Collapse static subtrees into per-material mega-mesh blocks.
	bool collapseStatic = false;
	// Per-blob LZ4 compression (default on: only stores when it shrinks).
	bool compress = true;
	int lz4Accel = 1; // 1 = best ratio, higher = faster
	// ---- Lightmap cook (--lightmap on) ----
	bool lightmap = false;
	uint32_t lmDensity = 32;     // texels per meter
	uint32_t lmLobes = 12;       // 5|6|9|12 — PREFERRED max; the bake
	                             // selects the smallest count per mesh
	                             // whose fit residual fits the budget
	                             // (SgLightmapBakeConfig::lobeRelError)
	uint32_t lmAtlasWidth = 2048;
	// Per-chart inner margin texels. Island-to-island padding is this +
	// the 2-texel pack gap + the neighbor's margin (4 total at 1); see
	// SgLightmapAtlas::PackRects. Large gutters are paid per CHART, and
	// ornamented scenes carry tens of thousands of tiny charts, so keep
	// this at 1 and let the packer provide the separation.
	uint32_t lmGutter = 1;
	// Atlas texel budget (width*height): the GPU cost is budget * 72 B
	// (9 RGBA16F layers). 4M -> ~288 MB. The density convergence
	// binary-searches the largest density that fits this budget.
	uint64_t lmBudget = 4ull * 1024 * 1024;
	uint32_t lmRays = 1024;      // radiance samples per texel
	uint32_t lmBounces = 3;
	// Per-mesh lobe selection budget: relative RMS degradation from the
	// preferred-count reconstruction the mesh may cost by dropping
	// lobes. 0.15 keeps contrast-heavy meshes at 9-12 and lets smooth
	// ones drop lower; 0 pins everything at --lm-lobes.
	float lmLobeRelErr = 0.15f;
	// ---- Lightmapped LOD cooking (siblings simplify after the bake) --
	// Chart-seam protection during sibling simplify: "attr" = chart id +
	// atlas uv as simplifier error terms (default), "hard" = vertex-lock
	// every seam pair (less reduction).
	std::string lmLodLock = "attr";
	// Per-level lightmap-safety audit (texel drift of the simplified
	// parameterization vs the LOD0 surface). Log-only unless the gate.
	bool lmLodAudit = true;
	float lmLodAuditTx = 2.0f;  // texel threshold for over-count/gate
	bool lmLodAuditGate = false;
	// ---- OIDN denoising (fitted atlas amplitudes, cook-time only) ------
	// Denoise the baked per-lobe amplitude layers with Intel Open Image
	// Denoise's RTLightmap filter (see lm_denoise.cpp). On by default
	// with --lightmap. Independent of --lm-rays (see the cook-time hint).
	bool lmDenoise = true;
	std::string lmDenoiseQuality = "high"; // high | balanced
	// ---- Curvature-weighted density (banding fix on curved shapes) ----
	// Target normal-turn per texel on smoothly bending surfaces: meshes
	// bending faster than the target chart at up to --lm-curve-max times
	// the base density; the atlas convergence takes the budget back from
	// flat meshes, so the total texel count stays inside --lm-budget.
	// Uniform density reads as faceted/hard-normal shading on curved
	// geometry (a column at ~20 texels/m turns ~18 deg per texel).
	// 0 disables.
	float lmCurveTarget = 7.0f;  // degrees of normal turn per texel
	float lmCurveMax = 2.5f;     // max per-mesh density multiplier
	// ---- Chart source -------------------------------------------------
	// "auto"/"authored" chart from the imported texCoord2 (external
	// unwrapper, e.g. tools/rizom_uv2.py packing TEXCOORD_1) with a
	// per-mesh planar fallback when a mesh carries no authored layout
	// (GenerateLightmapCharts checks the spread and falls through);
	// "authored-pack" additionally treats each mesh's whole [0,1]^2
	// layout as ONE chart (pre-packed unwrapper layouts: RizomUV Pack —
	// per-island rects pay ~4 gutter texels per side per island, and
	// rizom-dense scenes carry millions of islands);
	// "planar" forces the built-in unwrapper for every mesh.
	// "xatlas" runs the full external-xatlas chart + brute-force pack
	// path (one xatlas atlas over all meshes, ComputeCharts + PackCharts
	// with bruteForce=true always) with per-LOD atlas levels stacked
	// into the shared atlas (see the lightmap block below).
	std::string lmCharts = "auto";
	// Comma-separated node-name keywords whose SUBTREES are excluded from
	// charts/bake entirely (word-boundary match, case-insensitive: "tree"
	// hits Linden_Tree, not StreetSign; "glass" hits Wine_Glass). Excluded
	// geometry stays in the trace scene (occludes/casts) and renders
	// probe/sky-lit at runtime (the FS falls back to probe/grid ambient
	// for draws with no lightmap chunk). Empty = no exclusion.
	std::string lmExclude;
	std::vector<std::string> lmExcludeKeywords;
	// World-space size gate (meters, bbox diagonal): meshes smaller than
	// this skip charts/bake entirely (probe/sky-lit at runtime). Bistro
	// glassware/cutlery bake to a handful of texels at scene density
	// anyway — probes serve them better, and the atlas keeps its texels
	// for walls and facades. 0 = no gating.
	float lmExcludeSmall = 0.0f;
	float sunAzimuth = 30.0f;    // mirrors the runtime environment defaults
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	// Analytic sky IBL baked into the lightmap on open-sky rays
	// (gradient only, no sun disc). Default mirrors the runtime
	// EnvironmentSettings so covered pixels match the sky fallback
	// pixels see. 0 = sky-less field (old behavior).
	float skyIntensity = 600.0f;
	// Static spots for the bake: "px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela"
	std::vector<std::string> spots;
	// Probe-volume sidecars this pak's scenes sample (repeatable):
	// registers gi/<name>.sggi names into the clusters (bake the matching
	// volumes with GiBaker --out). Runtime resolves by name.
	std::vector<std::string> giProbes;
	// Read-only inspection: open an existing pak and print its manifest
	// (meshes + lmChunk, materials + texture URIs, textures). No cooking.
	std::string dumpPak;
	// Scene pinpoint: with --dump-pak, restrict the scene print to these
	// node ids (repeatable) and include their local translation.
	std::vector<uint32_t> dumpNodes;
};

std::vector<float> ParseLodRatios(const std::string& s) {
	std::vector<float> out;
	size_t pos = 0;
	while (pos <= s.size()) {
		size_t comma = s.find(',', pos);
		const std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		try {
			const float v = std::stof(tok);
			if (v > 0.0f && v < 1.0f)
				out.push_back(v);
			else
				std::cerr << "Ignoring out-of-range lod ratio: " << tok << std::endl;
		}
		catch (...) {
			std::cerr << "Ignoring bad lod ratio: " << tok << std::endl;
		}
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}
	return out;
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
		else if (a == "--in-vfs") {
			if (!need(out.inVfs)) return false;
		}
		else if (a == "--out-pak") {
			if (!need(out.outPak)) return false;
		}
		else if (a == "--scene-index") {
			std::string v;
			if (!need(v)) return false;
			out.sceneIndex = std::stoi(v);
		}
		else if (a == "--tour") {
			if (!need(out.tour)) return false;
		}
		else if (a == "--meshopt") {
			std::string v;
			if (!need(v)) return false;
			out.meshopt = (v == "on" || v == "1" || v == "true");
			out.explicitSet.insert("meshopt");
		}
		else if (a == "--overdraw") {
			std::string v;
			if (!need(v)) return false;
			out.overdraw = std::stof(v);
		}
		else if (a == "--dump-manifest") {
			std::string v;
			if (!need(v)) return false;
			out.dumpManifest = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--verify") {
			std::string v;
			if (!need(v)) return false;
			out.verify = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--lods") {
			std::string v;
			if (!need(v)) return false;
			out.lods = (v == "on" || v == "1" || v == "true");
			out.explicitSet.insert("lods");
		}
		else if (a == "--lod-ratios") {
			if (!need(out.lodRatios)) return false;
		}
		else if (a == "--lod-error") {
			std::string v;
			if (!need(v)) return false;
			out.lodError = std::stof(v);
		}
		else if (a == "--meshlets") {
			std::string v;
			if (!need(v)) return false;
			out.meshlets = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--max-verts") {
			std::string v;
			if (!need(v)) return false;
			out.maxVerts = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--max-tris") {
			std::string v;
			if (!need(v)) return false;
			out.maxTris = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--cone-weight") {
			std::string v;
			if (!need(v)) return false;
			out.coneWeight = std::stof(v);
		}
		else if (a == "--collapse-static") {
			std::string v;
			if (!need(v)) return false;
			out.collapseStatic = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--compress") {
			std::string v;
			if (!need(v)) return false;
			out.compress = (v == "on" || v == "1" || v == "true" || v == "lz4");
		}
		else if (a == "--lz4-accel") {
			std::string v;
			if (!need(v)) return false;
			out.lz4Accel = std::max(1, std::stoi(v));
		}
		else if (a == "--lightmap") {
			std::string v;
			if (!need(v)) return false;
			out.lightmap = (v == "on" || v == "1" || v == "true");
			out.explicitSet.insert("lightmap");
		}
		else if (a == "--lm-density") {
			std::string v;
			if (!need(v)) return false;
			out.lmDensity = static_cast<uint32_t>(std::stoul(v));
			out.explicitSet.insert("lmDensity");
		}
		else if (a == "--lm-lobes") {
			std::string v;
			if (!need(v)) return false;
			out.lmLobes = static_cast<uint32_t>(std::stoul(v));
			out.explicitSet.insert("lmLobes");
		}
		else if (a == "--lm-atlas-width") {
			std::string v;
			if (!need(v)) return false;
			out.lmAtlasWidth = static_cast<uint32_t>(std::stoul(v));
			out.explicitSet.insert("lmAtlasWidth");
		}
		else if (a == "--lm-gutter") {
			std::string v;
			if (!need(v)) return false;
			out.lmGutter = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--lm-budget") {
			std::string v;
			if (!need(v)) return false;
			out.lmBudget = std::stoull(v);
			out.explicitSet.insert("lmBudget");
		}
		else if (a == "--lm-rays") {
			std::string v;
			if (!need(v)) return false;
			out.lmRays = static_cast<uint32_t>(std::stoul(v));
			out.explicitSet.insert("lmRays");
		}
		else if (a == "--lm-bounces") {
			std::string v;
			if (!need(v)) return false;
			out.lmBounces = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--lm-lobe-relerr") {
			std::string v;
			if (!need(v)) return false;
			out.lmLobeRelErr = std::stof(v);
		}
		else if (a == "--lm-lod-lock") {
			std::string v;
			if (!need(v)) return false;
			out.lmLodLock = (v == "hard" || v == "border") ? "hard" : "attr";
		}
		else if (a == "--lm-lod-audit") {
			std::string v;
			if (!need(v)) return false;
			out.lmLodAudit = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--lm-lod-audit-tx") {
			std::string v;
			if (!need(v)) return false;
			out.lmLodAuditTx = std::stof(v);
		}
		else if (a == "--lm-lod-audit-gate") {
			std::string v;
			if (!need(v)) return false;
			out.lmLodAuditGate = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--lm-denoise") {
			std::string v;
			if (!need(v)) return false;
			out.lmDenoise = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--lm-denoise-quality") {
			std::string v;
			if (!need(v)) return false;
			out.lmDenoiseQuality = (v == "balanced") ? "balanced" : "high";
		}
		else if (a == "--lm-curve-target") {
			std::string v;
			if (!need(v)) return false;
			out.lmCurveTarget = std::stof(v);
		}
		else if (a == "--lm-curve-max") {
			std::string v;
			if (!need(v)) return false;
			out.lmCurveMax = std::max(std::stof(v), 1.0f);
		}
		else if (a == "--lm-charts") {
			std::string v;
			if (!need(v)) return false;
			// "planar" forces the built-in unwrapper; "authored-pack"
			// = authored UV2 with one chart per pre-packed layout;
			// "xatlas" = external-xatlas charts + brute-force pack
			// (per-LOD atlas levels); anything else ("auto", "authored")
			// = authored UV2 with per-island charts.
			out.lmCharts = (v == "planar") ? "planar"
				: (v == "authored-pack" ? "authored-pack"
				: (v == "xatlas" ? "xatlas" : "auto"));
		}
		else if (a == "--lm-exclude" || a == "--lm-exclude-veg") {
			std::string v;
			if (!need(v)) return false;
			out.explicitSet.insert("lmExclude");
			if (!out.lmExclude.empty() && !v.empty())
				out.lmExclude += ",";
			out.lmExclude += v;
			out.lmExcludeKeywords.clear();
			std::string low;
			for (char ch : out.lmExclude)
				low.push_back(char(std::tolower(uint8_t(ch))));
			size_t pos = 0;
			while (pos < low.size()) {
				const size_t end = low.find(',', pos);
				const std::string kw =
					low.substr(pos, end == std::string::npos
						? std::string::npos : end - pos);
				if (!kw.empty())
					out.lmExcludeKeywords.push_back(kw);
				pos = (end == std::string::npos)
					? low.size() : end + 1;
			}
		}
		else if (a == "--lm-exclude-small") {
			std::string v;
			if (!need(v)) return false;
			out.lmExcludeSmall = std::max(std::stof(v), 0.0f);
			out.explicitSet.insert("lmExcludeSmall");
		}
		else if (a == "--sun-azimuth") {
			std::string v;
			if (!need(v)) return false;
			out.sunAzimuth = std::stof(v);
			out.explicitSet.insert("sunAzimuth");
		}
		else if (a == "--sun-elevation") {
			std::string v;
			if (!need(v)) return false;
			out.sunElevation = std::stof(v);
			out.explicitSet.insert("sunElevation");
		}
		else if (a == "--sun-intensity") {
			std::string v;
			if (!need(v)) return false;
			out.sunIntensity = std::stof(v);
			out.explicitSet.insert("sunIntensity");
		}
		else if (a == "--sky-intensity") {
			std::string v;
			if (!need(v)) return false;
			out.skyIntensity = std::stof(v);
			out.explicitSet.insert("skyIntensity");
		}
		else if (a == "--spot") {
			std::string v;
			if (!need(v)) return false;
			out.spots.push_back(v);
		}
		else if (a == "--gi-probe") {
			std::string v;
			if (!need(v)) return false;
			out.giProbes.push_back(v);
		}
		else if (a == "--dump-pak") {
			if (!need(out.dumpPak)) return false;
		}
		else if (a == "--dump-node") {
			std::string v;
			if (!need(v)) return false;
			out.dumpNodes.push_back(static_cast<uint32_t>(std::stoul(v)));
		}
		else if (a == "--help" || a == "-h") {
			return false;
		}
		else {
			std::cerr << "Unknown arg: " << a << std::endl;
			return false;
		}
	}
	if (!out.dumpPak.empty())
		return true; // inspection mode needs no cook inputs
	if (out.inVfs.empty() || out.outPak.empty()) {
		std::cerr << "Required: --in-vfs <model vfs path> --out-pak <host path>" << std::endl;
		return false;
	}
	return true;
}

void Usage() {
	std::cout << "PakCooker --content-dir content --in-vfs models/Sponza/sponza.gltf\n"
		<< "          --out-pak content/paks/sponza.pak [--scene-index N] [--meshopt on|off]\n"
		<< "          (default cooks ALL glTF scenes: one Scene blob each\n"
		<< "          (#scene, #scene.1, ...) sharing one asset pool; the\n"
		<< "          runtime loads index 0 unless told otherwise.\n"
		<< "          --lightmap/--tour need exactly one cooked scene.)\n"
		<< "          [--tour <scene.tour>] [--overdraw 1.05] [--verify on|off]"
		<< " [--dump-manifest on|off]\n"
		<< "          [--lods on|off] [--lod-ratios 0.6,0.3,0.12] [--lod-error 0.02]\n"
		<< "          [--meshlets on|off] [--max-verts 64] [--max-tris 124]\n"
		<< "          [--cone-weight 0] [--collapse-static on|off]\n"
		<< "          [--compress on|off] [--lz4-accel 1]\n"
		<< "          [--lightmap on|off --lm-density 32 --lm-lobes 12\n"
		<< "           --lm-atlas-width 2048 --lm-gutter 1 --lm-rays 1024\n"
		<< "           --lm-budget 4194304 --lm-bounces 3\n"
		<< "           --sun-azimuth 30 --sun-elevation 55\n"
		<< "           --sun-intensity 10000 --sky-intensity 600\n"
		<< "           --lm-lod-lock attr|hard --lm-lod-audit on|off\n"
		<< "           --lm-lod-audit-tx 2.0 --lm-lod-audit-gate on|off\n"
		<< "           --lm-denoise on|off --lm-denoise-quality high|balanced\n"
		<< "           --lm-curve-target 7.0 --lm-curve-max 2.5\n"
		<< "           --lm-charts auto|authored|authored-pack|planar|xatlas\n"
		<< "           --lm-exclude tree,linden,ivy,glass --lm-exclude-small 0.5\n"
		<< "           --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]\n"
		<< "          [--gi-probe <name> ...]: registers probe sidecar names\n"
		<< "          (gi/<name>.sggi, baked separately) into the pak; the\n"
		<< "          lightmap atlas registers itself on --lightmap. Runtime\n"
		<< "          resolves GI by these names (one per kind for now).\n"
		<< "--lightmap on: bakes 2D SG lightmaps into the pak (per-vertex\n"
		<< "lightmap UVs + per-mesh chunk id; atlas written to\n"
		<< "<contentDir>/gi/<scene>.sglm). Sources: sun + static spots + real\n"
		<< "materials; transmissive (Blend) meshes pass light through.\n"
		<< "--lm-density is a WORLD-space request (texels/m; scaled nodes like\n"
		<< "sponza's 0.008 root convert through their world scale). The cook\n"
		<< "binary-searches the largest density whose atlas fits --lm-budget\n"
		<< "texels (GPU cost = budget * 72 B for the 9 RGBA16F layers) and\n"
		<< "logs the converged texels/m + texels/m^2.\n"
		<< "--lm-lobes is the PREFERRED (max) SG lobe count: the bake fits\n"
		<< "5/6/9/12 bases per texel and keeps the smallest count per MESH\n"
		<< "whose fitted irradiance degrades less than --lm-lobe-relerr\n"
		<< "(default 0.15) from the preferred-count reconstruction\n"
		<< "(contrast-heavy meshes keep 9-12, smooth ones drop lower; the\n"
		<< "runtime fetches only the layers the count needs).\n"
		<< "--dump-pak <pak path>: read-only inspection of an existing pak\n"
		<< "(no cooking): prints meshes + lmChunk, materials + texture URIs,\n"
		<< "and embedded textures.\n"
		<< "--tour <scene.tour>: replay the editor's ASCII command overlay\n"
		<< "(renames, transforms, materials, no-lightmap flags, reparents)\n"
		<< "onto the imported cluster before cooking. Tour cook settings\n"
		<< "fill flags the CLI did not set (CLI wins). Deterministic:\n"
		<< "source glTF + tour in, identical pak out.\n";
}

// Little-endian cursor over a MeshBlob payload for manifest printing. Mesh
// blob layout is defined by cluster_serializer.cpp (see the MeshBlob block):
//   str uri | u32 vertCount | u32 indexCount | u32 surfCount | surfaces |
//   vec3 aabbMin | vec3 aabbMax | f32 sphere | u32 topology |
//   LodInfo{groupId,lodIndex,candidateCount,f32 simplifyError,
//           f32 screenThreshold,triCount,f32 acmr} | vertex/index payloads
struct DumpCursor {
	std::span<const uint8_t> img;
	size_t off = 0;
	bool ok = true;
	uint32_t u32() {
		uint32_t v = 0;
		if (off + 4 > img.size()) ok = false; else { memcpy(&v, img.data() + off, 4); off += 4; }
		return v;
	}
	int32_t i32() { return static_cast<int32_t>(u32()); }
	float f32() {
		float v = 0.0f;
		if (off + 4 > img.size()) ok = false; else { memcpy(&v, img.data() + off, 4); off += 4; }
		return v;
	}
	std::string str() {
		const uint32_t n = u32();
		if (!ok || off + n > img.size()) { ok = false; return {}; }
		std::string s(reinterpret_cast<const char*>(img.data() + off), n);
		off += n;
		return s;
	}
};

// Prints meshes/materials/textures read back from the written pak (never
// from the in-memory Cluster, so the dump reflects the actual bytes).
void DumpManifestFromPak(const smvk::PakReader& reader,
	const std::vector<uint32_t>& onlyNodes = {}) {
	std::cout << "  manifest (pak-sourced):\n";

	// Meshes.
	for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Mesh)) {
		DumpCursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		const uint32_t vc = c.u32();
		const uint32_t ic = c.u32();
		const uint32_t sc = c.u32();
		for (uint32_t s = 0; s < sc; ++s) { (void)c.u32(); (void)c.u32(); (void)c.i32(); }
		for (int k = 0; k < 6; ++k) (void)c.f32(); // aabb min/max
		(void)c.f32(); // sphere
		(void)c.u32(); // topology
		const uint32_t groupId = c.u32();
		const uint32_t lodIndex = c.u32();
		const uint32_t candidates = c.u32();
		const float simplifyError = c.f32();
		(void)c.f32(); // screenThreshold
		const uint32_t tris = c.u32();
		const float acmr = c.f32();
		const uint32_t lmChunk = c.u32();
		if (!c.ok) {
			std::cout << "    [truncated mesh blob: " << e->path << "]\n";
			continue;
		}
		// ---- uv2 sweep audit (lightmap seam detector) ----
		// StaticVertex (AoS, 72 B): pos 12 | color 16 | texCoord 8 |
		// texCoord2 (the atlas uv) at byte 36 | normal 12 | tangent 16.
		// A chart-seam triangle whose corners map into DIFFERENT atlas
		// rects sweeps the whole atlas between them — the cross-island
		// streak artifact. Within one chart rect a triangle spans at most
		// 256/atlasW + 256/atlasH (charts are capped at 256x256 texels;
		// tall/short atlases make that aspect-dependent, e.g. 2047x871
		// -> 0.42), so 0.45 flags only genuine cross-rect sweeps.
		float maxUv2Span = 0.0f;
		uint32_t bigUv2Tris = 0;
		const size_t vertBase = c.off;
		const size_t idxBase = vertBase + (size_t)vc * sizeof(smvk::StaticVertex);
		if (lmChunk != 0xFFFFFFFFu && idxBase + (size_t)ic * 4 <= c.img.size()) {
			auto uv2At = [&](size_t v) -> std::pair<float, float> {
				const uint8_t* p = c.img.data() + vertBase
					+ v * sizeof(smvk::StaticVertex) + 36;
				float u, v2;
				memcpy(&u, p, 4);
				memcpy(&v2, p + 4, 4);
				return { u, v2 };
			};
			for (uint32_t t = 0; t + 2 < ic / 3; ++t) {
				uint32_t ix[3] = {};
				memcpy(ix, c.img.data() + idxBase + (size_t)t * 12, 12);
				const auto a = uv2At(ix[0]);
				const auto b = uv2At(ix[1]);
				const auto d = uv2At(ix[2]);
				const float duv0 = std::abs(a.first - b.first) + std::abs(a.second - b.second);
				const float duv1 = std::abs(a.first - d.first) + std::abs(a.second - d.second);
				const float duv2 = std::abs(b.first - d.first) + std::abs(b.second - d.second);
				const float span = std::max({ duv0, duv1, duv2 });
				if (span > maxUv2Span)
					maxUv2Span = span;
				if (span > 0.45f)
					++bigUv2Tris;
			}
		}
		// Vertex/index payloads (sizeof(StaticVertex) per vert, 4 B per index).
		c.off += (size_t)vc * sizeof(smvk::StaticVertex) + (size_t)ic * 4;
		uint32_t meshlets = 0;
		if (c.off < c.img.size()) {
			if (c.off + 4 <= c.img.size())
				meshlets = c.u32();
			else
				c.ok = false;
		}
		if (!c.ok) {
			std::cout << "    [truncated mesh blob: " << e->path << "]\n";
			continue;
		}
		std::cout << "    mesh " << uri
			<< " verts=" << vc << " idx=" << ic
			<< " lod{g=" << groupId << " i=" << lodIndex << " n=" << candidates
			<< " tris=" << tris << " err=" << simplifyError
			<< " acmr=" << acmr << "}"
			<< " meshlets=" << meshlets
			<< " lmChunk=" << (lmChunk == 0xFFFFFFFFu ? -1 : int(lmChunk));
		if (lmChunk != 0xFFFFFFFFu)
			std::cout << " uv2span=" << maxUv2Span
				<< " bigtris=" << bigUv2Tris;
		std::cout << "\n";
	}

	// Materials (blob: str uri | 128B MaterialValues | u32 shading |
	// u32 alpha | f32 cutoff | u32 doubleSided | 5x str texUris
	// (baseColor, metallicRoughness, normal, occlusion, emissive) |
	// u32 castsShadows).
	for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Material)) {
		DumpCursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		if (c.off + 128 > c.img.size()) {
			std::cout << "    mat  " << uri << " [truncated values]\n";
			continue;
		}
		// Decode the texture-slot indices baked into MaterialValues
		// (textures ivec4 at byte 32, textures2 ivec4 at byte 48;
		// see Engine/Renderer/mesh.h) alongside the URI strings the
		// runtime remaps them through. Metallic/roughness ride params
		// (byte 64: x=cutoff, y=metallic, z=roughness).
		int32_t slots[5] = { -9,-9,-9,-9,-9 };
		for (int k = 0; k < 4; ++k) {
			int32_t v = 0;
			memcpy(&v, c.img.data() + c.off + 32 + k * 4, 4);
			slots[k] = v;
		}
		{
			int32_t v = 0;
			memcpy(&v, c.img.data() + c.off + 48, 4);
			slots[4] = v;
		}
		float metal = 0.0f, rough = 0.0f;
		memcpy(&metal, c.img.data() + c.off + 68, 4);
		memcpy(&rough, c.img.data() + c.off + 72, 4);
		c.off += 128;
		const uint32_t shading = c.u32();
		const uint32_t alpha = c.u32();
		const float cutoff = c.f32();
		const uint32_t ds = c.u32();
		std::string texUris[5];
		for (int k = 0; k < 5; ++k)
			texUris[k] = c.str();
		const uint32_t cs = c.u32();
		if (!c.ok) {
			std::cout << "    mat  " << uri << " [truncated blob]\n";
			continue;
		}
		std::cout << "    mat  " << uri
			<< " shading=" << shading << " alpha=" << alpha
			<< " cutoff=" << cutoff << " ds=" << ds
			<< " cast=" << cs
			<< " metal=" << metal << " rough=" << rough << "\n";
		static const char* kNames[5] = { "bc", "mr", "n", "occ", "e" };
		for (int k = 0; k < 5; ++k) {
			std::cout << "      " << kNames[k] << " slot=" << slots[k]
				<< " uri=" << (texUris[k].empty() ? "<none>" : texUris[k]) << "\n";
		}
	}

	// GI registry (named sidecars: kind + stem).
	for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Gi)) {
		std::vector<smvk::GiBinding> bindings;
		std::string gerr;
		if (!smvk::ParseGiRegistryBlob(reader.Payload(*e), bindings, gerr)) {
			std::cout << "    [malformed gi registry blob: " << e->path << "]\n";
			continue;
		}
		for (const auto& b : bindings) {
			std::cout << "    gi   "
				<< (b.kind == smvk::GiBinding::Kind::Lightmap ? "lightmap" : "probe")
				<< " '" << b.name << "'\n";
		}
	}

	// Textures (embedded .ktx2 byte size).
	for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Texture)) {
		smvk::PakTexturePayload p;
		if (smvk::ParseTexturePayload(reader.Payload(*e), p)) {
			std::cout << "    tex  " << p.uri << " (" << p.ktxBytes.size() << " B)\n";
		}
		else {
			std::cout << "    [malformed texture blob: " << e->path << "]\n";
		}
	}

	// Lights (blob: str uri | u32 type | vec3 color | f32 intensity |
	// f32 range | f32 inner | f32 outer | str name).
	for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Light)) {
		DumpCursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		const uint32_t type = c.u32();
		const float r = c.f32(), g = c.f32(), b = c.f32();
		const float intensity = c.f32(), range = c.f32();
		const float inner = c.f32(), outer = c.f32();
		const std::string name = c.str();
		if (!c.ok) {
			std::cout << "    [truncated light blob: " << e->path << "]\n";
			continue;
		}
		const char* tname = type == 0 ? "dir" : (type == 2 ? "spot" : "point");
		std::cout << "    light " << uri << " '" << name << "' " << tname
			<< " rgb=(" << r << "," << g << "," << b << ")"
			<< " I=" << intensity << " range=" << range;
		if (type == 2)
			std::cout << " inner=" << inner << " outer=" << outer;
		std::cout << "\n";
	}

	// Scene graph (names + parent links + mesh/material/light pak ids + noLM).
	// Layout mirrors cluster_serializer.cpp: u32 nodes | hierarchy (5xi32)
	// | localTransforms (16xf32) | meshPairs | matPairs | namePairs |
	// nodeNames | matNames | static section | noLM section (v3) |
	// light pairs (v4; definitions are Light blobs) | name (v5).
	// Scene blobs iterate in TOC order; the index + name come from the
	// discovery table so multi-scene paks read per scene.
	std::unordered_map<std::string, std::pair<size_t, std::string>> sceneInfo;
	{
		std::string lerr;
		auto infos = smvk::ListPakScenes(reader, &lerr);
		for (size_t i = 0; i < infos.size(); ++i)
			sceneInfo[infos[i].tocPath] = { i, infos[i].name };
	}
	size_t sceneIdx = 0;
	for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Scene)) {
		DumpCursor c{ reader.Payload(*e), 0 };
		const uint32_t nodeCount = c.u32();
		if (!c.ok || nodeCount == 0 || nodeCount > 1024 * 1024) {
			std::cout << "    [bad scene blob]\n";
			continue;
		}
		const auto infoIt = sceneInfo.find(e->path);
		const size_t dispIdx = infoIt != sceneInfo.end()
			? infoIt->second.first : sceneIdx;
		const std::string dispName = infoIt != sceneInfo.end()
			? infoIt->second.second : std::string{};
		++sceneIdx;
		std::cout << "    scene[" << dispIdx << "] '" << dispName
			<< "' (" << e->path << "):\n";
		std::vector<int32_t> parent(nodeCount, -1);
		for (uint32_t i = 0; i < nodeCount; ++i) {
			parent[i] = c.i32();
			(void)c.i32();
			(void)c.i32();
			(void)c.i32();
			(void)c.i32();
		}
		// Local translations (pinpoint checks print them).
		std::vector<float> localT(nodeCount * 3, 0.0f);
		for (uint32_t i = 0; i < nodeCount; ++i) {
			float m[16];
			for (int k = 0; k < 16; ++k)
				m[k] = c.f32();
			localT[i * 3 + 0] = m[12];
			localT[i * 3 + 1] = m[13];
			localT[i * 3 + 2] = m[14];
		}
		std::unordered_map<uint32_t, uint32_t> meshMap, matMap, nameMap;
		const uint32_t meshPairs = c.u32();
		for (uint32_t i = 0; i < meshPairs; ++i) {
			const uint32_t n = c.u32(), id = c.u32();
			meshMap[n] = id;
		}
		const uint32_t matPairs = c.u32();
		for (uint32_t i = 0; i < matPairs; ++i) {
			const uint32_t n = c.u32(), id = c.u32();
			matMap[n] = id;
		}
		const uint32_t namePairs = c.u32();
		for (uint32_t i = 0; i < namePairs; ++i) {
			const uint32_t n = c.u32(), id = c.u32();
			nameMap[n] = id;
		}
		const uint32_t nameCount = c.u32();
		std::vector<std::string> names;
		for (uint32_t i = 0; i < nameCount; ++i)
			names.push_back(c.str());
		const uint32_t matNameCount = c.u32();
		for (uint32_t i = 0; i < matNameCount; ++i)
			(void)c.str();
		std::vector<uint8_t> noLm, dither;
		std::unordered_map<uint32_t, uint32_t> lightMap; // node -> light pak id
		if (c.off < c.img.size()) {
			const uint32_t sc = c.u32();
			if (c.ok && sc == nodeCount) {
				// static section bytes (skip) ...
				if (c.off + sc > c.img.size())
					c.ok = false;
				else
					c.off += sc;
				// ... then the v3 noLM section.
				if (c.ok && c.off < c.img.size()) {
					const uint32_t lc = c.u32();
					if (c.ok && lc == nodeCount && c.off + lc <= c.img.size()) {
						noLm.assign(c.img.data() + c.off,
							c.img.data() + c.off + lc);
						c.off += lc;
					}
				}
				// ... then the v4 light pairs (node, lightPakId); the
				// definitions are the Light blobs printed above.
				if (c.ok && c.off < c.img.size()) {
					const uint32_t lightPairs = c.u32();
					if (!c.ok || lightPairs > 1024 * 1024)
						c.ok = false;
					else {
						for (uint32_t li = 0; li < lightPairs && c.ok; ++li) {
							const uint32_t n = c.u32(), id = c.u32();
							lightMap[n] = id;
						}
					}
				}
				// ... then the v5 name (skip) and the v6 dither section.
				if (c.ok && c.off < c.img.size()) {
					(void)c.str();
					if (c.ok && c.off < c.img.size()) {
						const uint32_t dc = c.u32();
						if (c.ok && dc == nodeCount &&
							c.off + dc <= c.img.size()) {
							dither.assign(c.img.data() + c.off,
								c.img.data() + c.off + dc);
							c.off += dc;
						}
					}
				}
			}
		}
		if (!c.ok) {
			std::cout << "    [truncated scene blob]\n";
			continue;
		}
		std::cout << "    scene nodes=" << nodeCount
			<< " lightLinks=" << lightMap.size() << ":\n";
		// Print mesh-bearing nodes + renamed/noLM/interesting ones. Full
		// 9k-node dumps drown the signal; the tour verify path greps this.
		// --dump-node <id> (repeatable) restricts to pinpoint ids and adds
		// the local translation.
		const bool pinpoint = !onlyNodes.empty();
		uint32_t shown = 0;
		for (uint32_t i = 0; i < nodeCount; ++i) {
			if (pinpoint &&
				std::find(onlyNodes.begin(), onlyNodes.end(), i) ==
				onlyNodes.end())
				continue;
			std::string nm;
			auto nit = nameMap.find(i);
			if (nit != nameMap.end() && nit->second < names.size())
				nm = names[nit->second];
			const bool hasMesh = meshMap.contains(i);
			const bool noLmFlag = i < noLm.size() && noLm[i] != 0;
			const bool ditherFlag = i < dither.size() && dither[i] != 0;
			auto lit = lightMap.find(i);
			const bool hasLight = lit != lightMap.end();
			if (!pinpoint && !hasMesh && !noLmFlag && !hasLight && !ditherFlag)
				continue;
			std::cout << "      node " << i << " '" << nm << "' parent=" << parent[i];
			if (pinpoint)
				std::cout << " t=(" << localT[i * 3 + 0] << ","
					<< localT[i * 3 + 1] << "," << localT[i * 3 + 2] << ")";
			if (hasMesh)
				std::cout << " mesh=" << meshMap[i];
			auto mit = matMap.find(i);
			if (mit != matMap.end())
				std::cout << " mat=" << mit->second;
			if (hasLight)
				std::cout << " light=" << lit->second;
			if (noLmFlag)
				std::cout << " noLM=1";
			if (ditherFlag)
				std::cout << " dither=1";
			std::cout << "\n";
			if (!pinpoint && ++shown > 200) {
				std::cout << "      ... (truncated)\n";
				break;
			}
		}
	}
}

} // namespace

// Lightmap exclusion by node ancestry: true when any name on the node's
// parent chain matches one of the --lm-exclude keywords with word
// boundaries (case-insensitive; "tree" hits Linden_Tree_Mesh, not
// StreetSign_mesh). Excluded subtrees still trace/cast — they only skip
// charting and baked coefficients.
bool NodeChainMatchesKeywords(const Scene& scene, uint32_t node,
	const std::vector<std::string>& kws)
{
	for (int n = int(node); n >= 0 && n < int(scene.hierarchy.size()); ) {
		const std::string name = getNodeName(scene, n);
		if (!name.empty() && !kws.empty()) {
			std::string low;
			low.reserve(name.size());
			for (char ch : name)
				low.push_back(char(std::tolower(uint8_t(ch))));
			for (const std::string& kw : kws) {
				size_t pos = 0;
				while ((pos = low.find(kw, pos)) != std::string::npos) {
					const size_t end = pos + kw.size();
					const bool leftOk =
						pos == 0 || !std::isalpha(uint8_t(low[pos - 1]));
					const bool rightOk =
						end == low.size() || !std::isalpha(uint8_t(low[end]));
					if (leftOk && rightOk)
						return true;
					++pos;
				}
			}
		}
		n = scene.hierarchy[size_t(n)].parent;
	}
	return false;
}

// Cook-time de-instancing for the lightmap bake: meshes referenced by
// several nodes share one upload, but baked coefficients are per
// instance (each node's world transform feeds the trace), so shared
// meshes used to be skipped wholesale (bistro: ~2300 skipped pairs).
// With --lightmap on, every extra instance gets its own mesh: the
// retained upload is cloned through UploadStaticMesh (cook mode builds
// the same single-surface shape the importer made), the node's mapping
// is repointed, and a unique URI (original + "/inst<node>") plus copied
// LOD/meshlet markers are registered. The lowest node keeps the
// original; materials stay shared (the per-node material mapping is
// untouched). Deterministic: nodes processed in ascending order.
// Downstream stages (charts, bake, LOD siblings, meshlets, serializer)
// treat clones as ordinary meshes. Pool overflow degrades gracefully:
// a failed clone keeps the shared mapping (bake skips it as before).
uint32_t DeinstanceSharedMeshes(smvk::Cluster& cluster) {
	std::map<uint32_t, std::vector<uint32_t>> refs;
	for (const auto& [node, meshIdx] : cluster.scene.meshForNode)
		refs[meshIdx].push_back(node);
	uint32_t clones = 0;
	for (auto& [meshIdx, nodes] : refs) {
		if (nodes.size() < 2)
			continue;
		std::sort(nodes.begin(), nodes.end());
		const smvk::ClusterStaticMeshHandle origH =
			cluster.staticMeshes.getHandle(meshIdx);
		const smvk::StaticMeshUpload* origUp =
			cluster.GetRetainedUpload(origH);
		if (!origH.valid() || !origUp || origUp->vertices.empty()
			|| origUp->indices.empty())
			continue; // degenerate: keep shared (bake skips it anyway)
		const std::string origUri = cluster.GetMeshUri(origH).Str();
		const std::optional<smvk::LodInfo> origLod =
			origUri.empty() ? std::nullopt
			                : cluster.GetMeshLod(smvk::StringViewT(origUri));
		const std::optional<smvk::CookedMeshlets> origMeshlets =
			origUri.empty() ? std::nullopt
			                : cluster.GetMeshMeshlets(smvk::StringViewT(origUri));
		for (size_t i = 1; i < nodes.size(); ++i) {
			const uint32_t node = nodes[i];
			const smvk::ClusterStaticMeshHandle ch =
				cluster.UploadStaticMesh(*origUp);
			if (!ch.valid())
				continue;
			cluster.scene.meshForNode[node] = ch.index();
			if (!origUri.empty()) {
				const std::string cloneUri =
					origUri + "/inst" + std::to_string(node);
				cluster.RegisterMeshUri(smvk::StringViewT(cloneUri), ch);
				smvk::LodInfo lod;
				if (origLod)
					lod = *origLod;
				else
					lod.triCount = static_cast<uint32_t>(
						origUp->indices.size() / 3);
				// Pool indices are unique per cluster and the first clone
				// lands past every gltfMeshIndex-based group, so the
				// pool index is a collision-free LOD grouping key.
				lod.groupId = ch.index();
				lod.lodIndex = 0;
				lod.candidateCount = 1;
				lod.simplifyError = 0.0f;
				cluster.RegisterMeshLod(smvk::StringViewT(cloneUri), lod);
				if (origMeshlets)
					cluster.RegisterMeshMeshlets(
						smvk::StringViewT(cloneUri), *origMeshlets);
			}
			++clones;
		}
	}
	return clones;
}

// Per-corner atlas uv2 stamping (seam-correct), shared by the LOD0 bake
// and every per-LOD atlas level (xatlas path): a vertex shared by two
// charts has no single valid atlas uv, so every triangle corner is
// stamped through the TRIANGLE's chart — original vertices keep their
// slots 1:1 (first chart to use a vertex owns its uv2), corners needed
// by a different chart get positional duplicates appended, and the index
// buffer is rebuilt in the same triangle order (surface ranges stay
// valid). Fills outChartOf (per-vertex chart over the STAMPED layout —
// the simplifier's error-metric provenance) and counts seam duplicates.
// Returns the mesh's lobe count (first chunk, written by the bake's
// per-mesh selection; fallbackLobes when the mesh has no chunks).
uint32_t StampLightmapUvs(smvk::StaticMeshUpload& upload,
	const smvk::gi::SgLightmapChartResult& chartRes, uint32_t chunkBase,
	const smvk::gi::SgLightmapAtlas& atlas, uint32_t gutter,
	uint32_t fallbackLobes, std::vector<uint32_t>& outChartOf,
	uint64_t& seamDups) {
	const size_t triCount = upload.indices.size() / 3;
	const std::vector<uint32_t>& triChart = chartRes.triChart;
	const std::vector<glm::vec2>& cornerUv = chartRes.cornerChartUv;
	const uint32_t origVerts = uint32_t(upload.vertices.size());
	std::vector<uint32_t> ownerChart(origVerts, 0xFFFFFFFFu);
	// Chart of each appended seam duplicate (creation order).
	std::vector<uint32_t> dupChart;
	// (original vertex, chart) -> stamped index (orig or duplicate)
	std::unordered_map<uint64_t, uint32_t> cornerSlot;
	cornerSlot.reserve(size_t(origVerts) * 2);
	std::vector<uint32_t> newIndices;
	newIndices.reserve(upload.indices.size());
	for (size_t t = 0; t < triCount; ++t) {
		const uint32_t chartIdx = t < triChart.size()
			? triChart[t] : 0xFFFFFFFFu;
		for (int k = 0; k < 3; ++k) {
			const uint32_t orig = upload.indices[t * 3 + k];
			if (chartIdx == 0xFFFFFFFFu) {
				// Degenerate/uncharted triangle: keep the original
				// corner (uv2 stays whatever the vertex carries).
				newIndices.push_back(orig);
				continue;
			}
			const uint64_t key = uint64_t(orig) << 32 | chartIdx;
			auto it = cornerSlot.find(key);
			if (it != cornerSlot.end()) {
				newIndices.push_back(it->second);
				continue;
			}
			const glm::vec2 atlasUv = atlas.AtlasUv(
				chunkBase + chartIdx, gutter,
				cornerUv[t * 3 + k]);
			uint32_t slot;
			if (ownerChart[orig] == 0xFFFFFFFFu) {
				// First chart to stamp this vertex: in-place.
				ownerChart[orig] = chartIdx;
				slot = orig;
			}
			else if (ownerChart[orig] == chartIdx) {
				// Same chart re-uses the original (uv identical:
				// chart uv is a pure position projection).
				slot = orig;
			}
			else {
				// Seam corner: duplicate for THIS chart.
				smvk::StaticVertex dup = upload.vertices[orig];
				dup.texCoord2 = atlasUv;
				upload.vertices.push_back(dup);
				slot = uint32_t(upload.vertices.size() - 1);
				dupChart.push_back(chartIdx);
				++seamDups;
			}
			upload.vertices[slot].texCoord2 = atlasUv;
			cornerSlot.emplace(key, slot);
			newIndices.push_back(slot);
		}
	}
	upload.indices = std::move(newIndices);
	outChartOf.assign(upload.vertices.size(), 0xFFFFFFFFu);
	for (uint32_t v = 0; v < origVerts && v < outChartOf.size(); ++v)
		outChartOf[v] = ownerChart[v];
	for (size_t d = 0; d < dupChart.size()
		&& origVerts + d < outChartOf.size(); ++d)
		outChartOf[origVerts + d] = dupChart[d];
	if (!chartRes.charts.empty() && chunkBase < atlas.chunks.size())
		return atlas.chunks[chunkBase].lobeCount;
	return fallbackLobes;
}

int main(int argc, char** argv) {
	Args args;
	if (!ParseArgs(argc, argv, args)) {
		Usage();
		return 2;
	}

	// Read-only inspection path: no VFS, no cooking, no writes.
	if (!args.dumpPak.empty()) {
		std::ifstream rf(args.dumpPak, std::ios::binary | std::ios::ate);
		if (!rf) {
			std::cerr << "Cannot open pak: " << args.dumpPak << std::endl;
			return 1;
		}
		const auto size = rf.tellg();
		rf.seekg(0);
		std::vector<uint8_t> image(static_cast<size_t>(size));
		if (!rf.read(reinterpret_cast<char*>(image.data()), size)) {
			std::cerr << "Read failed: " << args.dumpPak << std::endl;
			return 1;
		}
		std::string verr;
		auto reader = smvk::PakReader::Open(
			std::span<const uint8_t>(image), verr);
		if (!reader) {
			std::cerr << "PakReader rejected: " << verr << std::endl;
			return 1;
		}
		std::cout << "Pak " << args.dumpPak << ": entries=" << reader->EntryCount()
			<< " meshes=" << reader->BlobsOfKind(smvk::PakAssetKind::Mesh).size()
			<< " materials=" << reader->BlobsOfKind(smvk::PakAssetKind::Material).size()
			<< " textures=" << reader->BlobsOfKind(smvk::PakAssetKind::Texture).size()
			<< " lights=" << reader->BlobsOfKind(smvk::PakAssetKind::Light).size()
			<< " scenes=" << reader->BlobsOfKind(smvk::PakAssetKind::Scene).size()
			<< " gi=" << reader->BlobsOfKind(smvk::PakAssetKind::Gi).size()
			<< "\n";
		{
			std::string lerr;
			auto scenes = smvk::ListPakScenes(*reader, &lerr);
			if (scenes.empty() && !lerr.empty())
				std::cerr << "Scene list failed: " << lerr << std::endl;
			for (size_t i = 0; i < scenes.size(); ++i)
				std::cout << "  scene[" << i << "] '" << scenes[i].name
					<< "' nodes=" << scenes[i].nodes << "\n";
		}
		DumpManifestFromPak(*reader, args.dumpNodes);
		return 0;
	}

	const auto t0 = std::chrono::steady_clock::now();

	// VFS: cooker reads inputs through the same overlay rules as runtime
	// (plus the Flintbase scheme for //flintbase/ inputs).
	smvk::OverlayVfs vfs;
	vfs.Mount(std::make_shared<smvk::LooseBackend>(args.contentDir), 0, "content");
	vfs.Mount(std::make_shared<smvk::FlintbaseBackend>(), 50, "flintbase");

	smvk::MeshOptConfig opt;
	opt.enabled = args.meshopt;
	opt.overdrawThreshold = args.overdraw;
	// Siblings are cooked AFTER the lightmap bake (CookLodSiblings below)
	// so lightmapped meshes simplify from their STAMPED uploads; the
	// importer's in-pass chain generation stays off here. With --lightmap
	// off the outcome is identical to the old import-time chain.
	opt.generateLods = false;
	opt.lodRatios = ParseLodRatios(args.lodRatios);
	opt.lodTargetError = std::clamp(args.lodError, 0.0f, 1.0f);
	opt.lodSeamLock = (args.lmLodLock == "hard")
		? smvk::LodSeamLock::Hard : smvk::LodSeamLock::Attribute;

	smvk::MeshletConfig meshletCfg;
	meshletCfg.enabled = args.meshlets;
	meshletCfg.maxVertices = std::clamp(args.maxVerts, 3u, 255u);
	meshletCfg.maxTriangles = std::clamp(args.maxTris, 1u, 512u);
	meshletCfg.coneWeight = args.coneWeight;
	// Meshlet DECOUPLING (--lightmap on): the lightmap stage below
	// re-parameterizes meshes (atlas uv2 stamping + seam vertex
	// duplication + index remap), so meshlets must NOT be built before
	// it — an import-time meshlet set references the PRE-stamp vertex
	// layout and draws the wrong texCoord2 per corner at runtime
	// (stale-window GI corruption). With lightmaps on the importer and
	// merged-block paths get a DISABLED config, and the cooker builds
	// meshlets ONCE after the bake from the FINAL stamped topology
	// (see the "Meshlet restore" stage below the lightmap block). The
	// rule that keeps GI whole: meshlet generation may partition
	// triangles, but it must never take part in — or see — the
	// parameterization. With --lightmap off nothing mutates uploads
	// after import, so import-time meshlets stay valid and the pipeline
	// is unchanged.
	smvk::MeshletConfig importMeshletCfg = meshletCfg;
	if (args.lightmap)
		importMeshletCfg.enabled = false;

	// Scene selection (multi-scene paks: one Scene blob per glTF scene,
	// shared asset pool; the runtime loads index 0 unless told otherwise).
	struct SceneCook { int gltfIndex; std::string name; };
	std::vector<SceneCook> toCook;
	{
		std::vector<std::string> gltfScenes;
		std::string lerr;
		if (!ListGltfScenesFromVfs(vfs, args.inVfs, gltfScenes, &lerr)) {
			std::cerr << "Scene list failed: " << lerr << std::endl;
			return 1;
		}
		if (gltfScenes.empty()) {
			toCook.push_back({ -1, "" }); // sceneless: legacy roots fallback
		}
		else if (args.sceneIndex >= 0) {
			if (args.sceneIndex >= int(gltfScenes.size())) {
				std::cerr << "--scene-index " << args.sceneIndex
					<< " out of range (" << gltfScenes.size()
					<< " scenes)" << std::endl;
				return 2;
			}
			toCook.push_back(
				{ args.sceneIndex, gltfScenes[size_t(args.sceneIndex)] });
		}
		else {
			for (int i = 0; i < int(gltfScenes.size()); ++i)
				toCook.push_back({ i, gltfScenes[size_t(i)] });
		}
		std::cout << "Cooking " << toCook.size() << " scene(s):";
		for (size_t i = 0; i < toCook.size(); ++i)
			std::cout << " [" << i << "]'" << toCook[i].name << "'";
		std::cout << "\n";
		if (toCook.size() > 1 && args.lightmap) {
			std::cerr << "--lightmap needs exactly one cooked scene "
				"(bake is per-scene): pass --scene-index N" << std::endl;
			return 2;
		}
		if (toCook.size() > 1 && !args.tour.empty()) {
			std::cerr << "--tour needs exactly one cooked scene "
				"(tour ids are scene-local): pass --scene-index N" << std::endl;
			return 2;
		}
	}

	// NOTE: heap-allocated — Cluster embeds multi-MB pool arrays and must
	// never live on the (1MB) thread stack. Cook mode = GPU-free: CPU
	// geometry + retained uploads only, textures are validated .ktx2
	// payloads with minted slot ids.
	std::vector<std::unique_ptr<smvk::Cluster>> sceneClusters;
	std::vector<std::string> sceneNames;
	for (const SceneCook& sc : toCook) {
		auto cluster = std::make_unique<smvk::Cluster>(nullptr);
		cluster->SetCookMode(true);

		try {
			GltfToClusterFromVfs(nullptr, vfs, args.inVfs, *cluster,
				opt, importMeshletCfg, 0, glm::mat4(1.0f), sc.gltfIndex);
		}
		catch (const std::exception& e) {
			std::cerr << "Import failed (scene '" << sc.name
				<< "'): " << e.what() << std::endl;
			return 1;
		}
		{
			size_t nDir = 0, nPoint = 0, nSpot = 0;
			cluster->lights.forEachHandle([&](smvk::ClusterLightHandle h) {
				const SceneLight* l = cluster->lights.get(h);
				if (!l)
					return;
				if (l->type == SceneLight::Type::Directional) ++nDir;
				else if (l->type == SceneLight::Type::Spot) ++nSpot;
				else ++nPoint;
			});
			std::cout << "Imported lights '" << sc.name << "': "
				<< cluster->lights.numObjects()
				<< " (dir=" << nDir << " point=" << nPoint << " spot=" << nSpot
				<< ", links=" << cluster->scene.lightForNode.size() << ")\n";
		}
		sceneNames.push_back(sc.name);
		sceneClusters.push_back(std::move(cluster));
	}
	// Single-scene alias for the tour/collapse/bake stages below (written
	// against one cluster; multi-scene loops reuse them per scene). Both
	// cluster-> and *cluster forms keep working through the pointer.
	smvk::Cluster* cluster = sceneClusters.front().get();

	// .tour replay (editor overlay): deterministic CRUD commands applied to
	// the freshly imported cook cluster — same ids, same importer — before
	// collapse/LOD/bake/serialize. Tour cook settings fill any flag the CLI
	// did not set explicitly (CLI wins). This is the mediaMolecule-style
	// bake: source asset + edit log in, deterministic pak out.
	if (!args.tour.empty()) {
		smvk::tour::Doc tour;
		std::string terr;
		std::vector<std::string> twarn;
		if (!smvk::tour::LoadTour(args.tour, tour, terr, &twarn)) {
			std::cerr << "Tour load failed: " << terr << std::endl;
			return 1;
		}
		for (auto& w : twarn)
			std::cerr << "Tour warning: " << w << std::endl;
		// Source sanity: the tour names the glTF it was authored against.
		if (!tour.source.empty()) {
			const auto base = [](const std::string& p) {
				const size_t s = p.find_last_of("/\\");
				return (s == std::string::npos) ? p : p.substr(s + 1);
			};
			if (base(tour.source) != base(args.inVfs))
				std::cerr << "Tour warning: authored for '" << tour.source
					<< "' but cooking '" << args.inVfs << "'\n";
			const std::string fpNow =
				smvk::tour::Fingerprint(args.contentDir + "/" + tour.source);
			if (!tour.fingerprint.empty() && !fpNow.empty() &&
				fpNow != tour.fingerprint)
				std::cerr << "Tour warning: source file changed since the tour "
					"was saved (ids may have shifted)\n";
		}
		if (tour.hasCook) {
			const auto& k = tour.cook;
			auto use = [&](const char* key, auto& dst, auto val) {
				if (!args.explicitSet.contains(key)) {
					dst = val;
					std::cout << "Tour cook default: " << key << "\n";
				}
			};
			use("meshopt", args.meshopt, k.meshopt);
			use("lods", args.lods, k.lods);
			use("lightmap", args.lightmap, k.lightmap);
			use("lmDensity", args.lmDensity, k.lmDensity);
			use("lmLobes", args.lmLobes, k.lmLobes);
			use("lmAtlasWidth", args.lmAtlasWidth, k.lmAtlasWidth);
			use("lmBudget", args.lmBudget, k.lmBudget);
			use("lmRays", args.lmRays, k.lmRays);
			use("lmExcludeSmall", args.lmExcludeSmall, k.lmExcludeSmall);
			use("sunAzimuth", args.sunAzimuth, k.sunAzimuth);
			use("sunElevation", args.sunElevation, k.sunElevation);
			use("sunIntensity", args.sunIntensity, k.sunIntensity);
			use("skyIntensity", args.skyIntensity, k.skyIntensity);
			if (!k.lmExcludeKw.empty() &&
				!args.explicitSet.contains("lmExclude")) {
				if (!args.lmExclude.empty())
					args.lmExclude += ",";
				args.lmExclude += k.lmExcludeKw;
				std::cout << "Tour cook default: lmExclude\n";
			}
			// Re-derive keyword list after the merge above.
			args.lmExcludeKeywords.clear();
			{
				std::string low;
				for (char ch : args.lmExclude)
					low.push_back(char(std::tolower(uint8_t(ch))));
				size_t pos = 0;
				while (pos < low.size()) {
					const size_t end = low.find(',', pos);
					const std::string kw = low.substr(pos, end == std::string::npos
						? std::string::npos : end - pos);
					if (!kw.empty())
						args.lmExcludeKeywords.push_back(kw);
					pos = (end == std::string::npos) ? low.size() : end + 1;
				}
			}
			opt.enabled = args.meshopt;
		}
	std::vector<std::string> rwarn;
	std::string rerr;
	// Clarisse-style nested scene references: inline the tour's
	// `reference` lines (placeholder + referenced source subtrees,
	// recursing through each source's own tour) BEFORE the command
	// replay so its cluster-space ids address the assembled scene.
	if (!tour.references.empty()) {
		// Sources resolve as-is or relative to the master scene's VFS dir.
		const size_t applied = ApplySceneReferences(*cluster, tour,
			smvk::vfs::BaseDir(args.inVfs),
			nullptr, nullptr, &vfs, opt, importMeshletCfg, 0, nullptr);
		std::cout << "References inlined: " << applied << " subtree(s) ("
			<< tour.references.size() << " top-level)\n";
	}
	if (!smvk::tour::ApplyTourToCluster(*cluster, tour, &rwarn, &rerr)) {
			std::cerr << "Tour replay failed: " << rerr << std::endl;
			return 1;
		}
		for (auto& w : rwarn)
			std::cerr << "Tour replay warning: " << w << std::endl;
		std::cout << "Tour replayed: " << tour.commands.size()
			<< " commands from " << args.tour << "\n";
	}

	// Static collapse: bake frozen subtrees into per-material mega-mesh
	// blocks (world-baked verts, identity nodes). Runs before Serialize so
	// the pak holds the collapsed scene. Per scene (shared nothing).
	if (args.collapseStatic) for (auto& sc : sceneClusters) {
		smvk::Cluster* cluster = sc.get();
		smvk::StaticMergeOptions mopts;
		smvk::StaticMergeResult mr = smvk::CollapseStaticSubtrees(*cluster, mopts);
		if (!mr.ok) {
			std::cerr << "CollapseStaticSubtrees failed: " << mr.err << std::endl;
			return 1;
		}
		std::cout << "Collapsed " << mr.srcNodesConsumed << " static nodes into "
			<< mr.blocks.size() << " block(s)\n";
		// Merged blocks skip the importer meshlet path; cook them here so
		// the single-block layout also feeds the indirect future. With
		// --lightmap on this is deferred too (importMeshletCfg disabled):
		// the lightmap stage re-stamps the block uploads, so their
		// meshlets are built by the post-bake restore stage below from
		// the final stamped topology.
		if (importMeshletCfg.enabled) {
			for (const auto& b : mr.blocks) {
				const auto h = cluster->staticMeshes.getHandle(b.meshPoolIndex);
				const smvk::StaticMeshUpload* up = cluster->GetRetainedUpload(h);
				if (!up)
					continue;
				smvk::CookedMeshlets cooked;
				smvk::MeshletStats st;
				std::string merr;
				if (smvk::BuildMeshletsForUpload(*up, meshletCfg, &cooked, &st, &merr)) {
					cluster->RegisterMeshMeshlets(b.uri, cooked);
					if (smvk::StaticMesh* m = cluster->staticMeshes.get(h))
						m->meshlets = std::move(cooked);
					std::cout << "Meshlets " << b.uri << ": count " << st.meshletCount
						<< " tris " << st.totalTriangles << std::endl;
				}
				else if (!merr.empty()) {
					std::cout << "Meshlets " << b.uri << " skipped: " << merr << std::endl;
				}
			}
		}
	}

	// ---------------------------------------------------------------------------
	// Lightmap cook (--lightmap on): bake 2D SG lightmaps INTO the pak —
	// per-vertex texCoord2 + per-mesh atlas chunk — and write the atlas
	// (<contentDir>/gi/<scene>.sglm) next to the pak. No bake-side work is
	// left to the runtime. Sources: sun + static spots (NEE) + real
	// materials (texture-averaged); transmissive (Blend) meshes pass rays
	// through, tinted (no charts for them). Shared (instanced) meshes are
	// de-instanced above so every instance bakes its own coefficients.
	// Decal overlays (cook-stamped no-cast on non-MASK materials) are out
	// of both charts and trace (see GiSceneBuildOptions::excludeDecalOverlays).
	//
	// lmVertexCharts (filled by the stamping below) carries each stamped
	// mesh's per-vertex chart ids out of the block for CookLodSiblings;
	// atlas stays alive for the audit's texel scale.
	// ---------------------------------------------------------------------------
	smvk::gi::SgLightmapAtlas atlas;
	std::unordered_map<uint32_t, std::vector<uint32_t>> lmVertexCharts;
	// Xatlas brute-force path (--lm-charts xatlas): full-scene xatlas
	// charts + brute-force pack for LOD0, per-LOD atlas levels stacked
	// into the shared atlas afterwards (meshlets stay deferred throughout
	// and are rebuilt once from the final stamped topology).
	const bool lmXatlas = (args.lmCharts == "xatlas");
	// Converged LOD0 world density, reused by the per-LOD levels.
	float lmFitDensityWorld = 0.0f;
	uint32_t lmLodLevelsBaked = 0;
	if (args.lightmap) {
		using namespace smvk::gi;
		const auto lmT0 = std::chrono::steady_clock::now();

		// Shared meshes carry one upload for many nodes, but baked
		// coefficients are per instance: clone the extras so every
		// instance charts and bakes independently (see DeinstanceSharedMeshes).
		{
			const uint32_t n = DeinstanceSharedMeshes(*cluster);
			std::cerr << "  [LM] de-instanced " << n << " shared meshes\n"
				<< std::flush;
		}

		// Texture averages so baked materials include texture content.
		GiSceneBuildOptions buildOpts{};
		{
			uint32_t averaged = 0;
			for (const auto& [uri, handle] : cluster->ListTextureUris()) {
				const std::vector<uint8_t>* src = cluster->GetTextureSource(uri);
				if (!src || src->empty())
					continue;
				std::optional<glm::vec3> avg =
					GiAverageKtxTexel(std::span<const uint8_t>(*src));
				if (!avg)
					continue;
				buildOpts.texelAvg[handle.index()] = *avg;
				++averaged;
			}
			std::cerr << "  [LM] textures averaged: " << averaged
				<< " / " << cluster->ListTextureUris().size() << "\n" << std::flush;
		}
		buildOpts.env.sunAzimuthDeg = args.sunAzimuth;
		buildOpts.env.sunElevationDeg = args.sunElevation;
		buildOpts.env.sunIntensity = args.sunIntensity;
		buildOpts.env.skyIntensity = args.skyIntensity;

		// Static spot list ("px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela").
		std::vector<GiTraceSpot> spots;
		for (const std::string& spec : args.spots) {
			GiTraceSpot spot{};
			float innerDeg = 0.0f, outerDeg = 0.0f, candela = 0.0f;
			glm::vec3 col(1.0f);
			if (std::sscanf(spec.c_str(),
					"%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f",
					&spot.position.x, &spot.position.y, &spot.position.z,
					&spot.direction.x, &spot.direction.y, &spot.direction.z,
					&innerDeg, &outerDeg, &spot.range,
					&col.r, &col.g, &col.b, &candela) != 13) {
				std::cerr << "Bad --spot (want px,py,pz,dx,dy,dz,innerDeg,"
					"outerDeg,range,r,g,b,candela): " << spec << "\n";
				return 2;
			}
			spot.color = col * candela;
			spot.cosInner = std::cos(glm::radians(std::min(innerDeg, outerDeg)));
			spot.cosOuter = std::cos(glm::radians(std::max(innerDeg, outerDeg)));
			spots.push_back(spot);
		}

		GiTraceScene traceScene;
		std::string terr;
		// buildOpts.staticLights (default on) harvests the scene's
		// STATIC-flagged cooked lights into traceScene.spots/sun here.
		BuildTraceSceneFromCluster(*cluster, buildOpts, traceScene, terr);
		if (!terr.empty()) {
			std::cerr << "[LM] trace scene failed: " << terr << std::endl;
			return 1;
		}
		// CLI --spot specs APPEND to the harvested static scene lights
		// (assignment would wipe them — both sets bake together).
		traceScene.spots.insert(
			traceScene.spots.end(), spots.begin(), spots.end());
		std::cerr << "  [LM] trace scene: " << traceScene.Bvh().TriangleCount()
			<< " tris, " << traceScene.materials.size() << " materials, "
			<< traceScene.spots.size() << " spots ("
			<< traceScene.spots.size() - spots.size() << " static scene lights)\n"
			<< std::flush;

		// Charts per eligible mesh (1:1 node->mesh, non-Blend material).
		// Charts are the ATLAS packing unit (one mesh owns many chart
		// rects, every rect small), so charting has no failure modes —
		// the ATLAS round below lowers the density globally until all
		// charts of ALL meshes fit the budget.
		struct LmWork {
			uint32_t node = 0;
			smvk::ClusterStaticMeshHandle handle{};
			float density = 0.0f;   // chart-space density actually used
			float worldScale = 1.0f; // node world scale (chart/world unit ratio)
			float curveScale = 1.0f; // curvature density boost (>= 1)
			SgLightmapChartResult chart;
			// chunk index base of this mesh's charts in the atlas
			// (chunk == chart rect; the mesh stamps its lobe count).
			uint32_t chunkBase = 0;
		};
		// Chart density lives in the UPLOAD's position space, but
		// --lm-density is world-space texels/meter: nodes with a world
		// scale != 1 (Sponza's 0.008 root) must scale it, or charts come
		// out 1/scale too dense and the budgets drop the biggest meshes
		// (walls/floor fell through to the grids). Per-node scaling
		// happens below; chartOpts carries the world-space request.
		auto lmWorldScale = [&](uint32_t node) {
			float s = 1.0f;
			if (node < cluster->scene.globalTransform.size()) {
				const glm::mat4& g = cluster->scene.globalTransform[node];
				s = std::max({ glm::length(glm::vec3(g[0])),
					glm::length(glm::vec3(g[1])),
					glm::length(glm::vec3(g[2])) });
			}
			return (s > 1e-6f) ? s : 1.0f;
		};
		// Charts read node-local positions while the trace scene is
		// world-space: make sure the global transforms are current
		// before either use.
		::recalculateGlobalTransforms(cluster->scene);
		SgLightmapChartOptions chartOpts{};
		chartOpts.texelsPerMeter = std::max<float>(float(args.lmDensity), 2.0f);
		chartOpts.gutterTexels = std::clamp<uint32_t>(args.lmGutter, 1, 4);
		// "auto" IS authored-with-fallback (GenerateLightmapCharts
		// checks texCoord2 spread per mesh and falls through to the
		// planar unwrapper); "planar" forces the built-in unwrap;
		// "authored-pack" = authored, one chart per pre-packed layout
		// (RizomUV: per-island rects would pay millions of gutters).
		chartOpts.authoredUv2 = (args.lmCharts != "planar");
		chartOpts.authoredOneChartPerLayout =
			(args.lmCharts == "authored-pack");
		if (!args.lmExcludeKeywords.empty() || args.lmExcludeSmall > 0.0f)
			std::cerr << "  [LM] chart source: " << args.lmCharts
				<< " | name exclusion: "
				<< (args.lmExclude.empty() ? "-" : args.lmExclude)
				<< " | small exclusion: "
				<< (args.lmExcludeSmall > 0.0f
					? std::to_string(args.lmExcludeSmall) + " m" : "-")
				<< "\n" << std::flush;
		std::vector<LmWork> work;
		uint32_t skipBlend = 0, skipInstanced = 0, skipNoMat = 0, skipDecal = 0, skipExcluded = 0, skipSmall = 0, skipNoLm = 0, skipHidden = 0;
		for (const auto& [node, meshIdx] : cluster->scene.meshForNode) {
			// Visibility overrides: hidden subtrees are not rendered, so
			// they take no lightmap charts (they still occlude/cast in
			// the trace scene like noLM exclusions).
			if (!IsVisibleNode(cluster->scene, int(node))) {
				++skipHidden;
				continue;
			}
			auto matIt = cluster->scene.materialForNode.find(node);
			if (matIt == cluster->scene.materialForNode.end()) {
				++skipNoMat;
				continue;
			}
			const smvk::ClusterMaterialHandle matHandle =
				cluster->materials.getHandle(matIt->second);
			if (!matHandle.valid())
				continue;
			const smvk::Material* mat = cluster->materials.get(matHandle);
			if (!mat || mat->alphaMode == smvk::AlphaMode::Blend) {
				++skipBlend;
				continue; // transmissive: no charts (pass-through only)
			}
			if (!mat->castsShadows && mat->alphaMode == smvk::AlphaMode::Opaque) {
				++skipDecal;
				continue; // decal overlay quad (cook-stamped no-cast):
				          // out of the atlas entirely (probes at runtime),
				          // so it steals no texels and shadows nothing.
				          // MASK no-cast (cutout on a decal atlas) still charts.
			}
			// Instancing safety net: de-instancing above leaves every mesh
			// 1:1, but degenerate clones keep the shared mapping — one
			// lightmap position per instance is still unsupported, skip.
			uint32_t refs = 0;
			for (const auto& [n2, m2] : cluster->scene.meshForNode)
				if (m2 == meshIdx)
					++refs;
			if (refs != 1) {
				++skipInstanced;
				continue;
			}
			// Name exclusion (--lm-exclude): vegetation leaf cards, glass
			// props, ... — the worst lightmap targets in the scene. They
			// keep their analytic/probe lighting and stay in the trace
			// scene (still occlude/cast).
			if (!args.lmExcludeKeywords.empty()
				&& NodeChainMatchesKeywords(cluster->scene, node,
					args.lmExcludeKeywords)) {
				++skipExcluded;
				continue;
			}
			// Authored exclusion (Scene::noLightmap, subtree-scoped): the
			// editor's per-object "bake lightmap" toggle, stamped from
			// glTF "smvk.noLightmap" node extras (or the NOLM_ prefix).
			// Same treatment as name exclusion: trace/cast, probe-lit.
			if (IsNoLightmapNode(cluster->scene, int(node))) {
				++skipNoLm;
				continue;
			}
			const smvk::ClusterStaticMeshHandle handle =
				cluster->staticMeshes.getHandle(meshIdx);
			const smvk::StaticMeshUpload* upload = cluster->GetRetainedUpload(handle);
			if (!upload || upload->vertices.empty() || upload->indices.empty()) {
				++skipNoMat;
				continue;
			}
			LmWork w{};
			w.node = node;
			w.handle = handle;
			w.worldScale = lmWorldScale(node);
			// Small-prop exclusion (--lm-exclude-small): meshes whose
			// world-space bbox diagonal is under the gate skip charts
			// (probe/sky-lit at runtime). Cheap scan over the upload.
			if (args.lmExcludeSmall > 0.0f) {
				glm::vec3 bbMin(std::numeric_limits<float>::max());
				glm::vec3 bbMax(std::numeric_limits<float>::lowest());
				for (const smvk::StaticVertex& v : upload->vertices) {
					bbMin = glm::min(bbMin, v.pos);
					bbMax = glm::max(bbMax, v.pos);
				}
				if (glm::length(bbMax - bbMin) * w.worldScale
					< args.lmExcludeSmall) {
					++skipSmall;
					continue;
				}
			}
			std::string cerr_;
			// Per-mesh density fallback: the mesh-total texel budget
			// (maxChartTexelsTotal) bounds the bake-point count; halve
			// the density until the mesh fits (floor 1 texel/m WORLD,
			// i.e. worldScale in chart space).
			const float floorD = std::max(w.worldScale, 1e-3f);
			const float localBase = std::max(
				chartOpts.texelsPerMeter * w.worldScale, floorD);
			// Curvature boost: smoothly-bending meshes chart denser so
			// the normal-turn per texel stays under the target; the
			// atlas convergence below takes the budget back from flat
			// meshes (same total texels, spent where normals turn fast).
			w.curveScale = args.lmCurveTarget > 0.0f
				? smvk::gi::LightmapCurvatureScale(*upload, localBase,
					args.lmCurveTarget, std::max(args.lmCurveMax, 1.0f))
				: 1.0f;
			if (lmXatlas) {
				// Xatlas path charts the whole scene in ONE batch during
				// the pack convergence below (world-prescaled positions
				// under a single world texelsPerUnit); nothing per mesh
				// here beyond the eligibility filters above.
				w.density = std::max(localBase * w.curveScale, floorD);
				std::cerr << "  [LM] xatlas node " << node
					<< " (curve x" << w.curveScale << ")\n" << std::flush;
				work.push_back(std::move(w));
				continue;
			}
			float density = std::max(localBase * w.curveScale, floorD);
			bool charted = false;
			while (density >= floorD) {
				SgLightmapChartOptions o = chartOpts;
				o.texelsPerMeter = density;
				try {
					charted = GenerateLightmapCharts(*upload, o, w.chart, cerr_);
				} catch (const std::exception& e) {
					std::cerr << "  [LM] chart THREW node " << node << ": "
						<< e.what() << "\n";
					return 1;
				}
				if (charted)
					break;
				if (density <= floorD)
					break;
				density = std::max(density * 0.5f, floorD);
			}
			if (!charted) {
				std::cerr << "  [LM] chart fail node " << node << " (over "
					<< "budget even at density " << density << "): "
					<< cerr_ << "\n";
				continue;
			}
			w.density = density;
			std::cerr << "  [LM] charted node " << node << " charts="
				<< w.chart.charts.size() << " density=" << density
				<< (w.chart.usedAuthoredUv2 ? " [authored]" : " [planar]");
			if (w.curveScale > 1.01f)
				std::cerr << " (curve x" << w.curveScale << ")";
			std::cerr << "\n" << std::flush;
			work.push_back(std::move(w));
		}

		if (!work.empty() && !lmXatlas) {
			// Atlas pack (planar/authored charts): ALL charts of all
			// meshes as individual rects.
			// Convergence (densities are WORLD texels/m; each mesh
			// converts through its own world scale):
			//   1. geometric binary search for the largest density whose
			//      pack fits --lm-budget (halving finds the feasible low
			//      bound, then ~6 probes between it and the failing high
			//      bound; the old halving-only rounds regularly skipped
			//      2x of usable budget);
			//   2. at the 1 texel/m floor, drop the most texel-hungry
			//      mesh per round until the pack fits.
			constexpr uint32_t kMaxAtlasH = 8192;
			auto tryPack = [&]() {
				uint32_t chartTotal = 0;
				for (const LmWork& w : work)
					chartTotal += uint32_t(w.chart.charts.size());
				std::vector<SgLightmapAtlas::RectRequest> reqs;
				reqs.reserve(chartTotal);
				for (const LmWork& w : work)
					for (const auto& ch : w.chart.charts)
						reqs.push_back({ glm::uvec2(ch.width, ch.height),
							args.lmLobes, w.node });
				return atlas.PackRects(reqs,
					std::max<uint32_t>(args.lmAtlasWidth, 256), kMaxAtlasH)
					&& uint64_t(atlas.width) * atlas.height <= args.lmBudget;
			};
			auto rechartAll = [&](float targetWorldDensity) {
				// Re-chart every mesh at targetWorldDensity (WORLD
				// texels/m — RAISES or lowers: the binary search probes
				// both sides) times its own curvature scale. Per-mesh
				// retry + drop on budget overflow at the floor. Each
				// mesh converts through its own world scale, so
				// mixed-scale scenes converge uniformly (relative
				// curvature allocation preserved across the search).
				std::vector<LmWork> kept;
				kept.reserve(work.size());
				for (LmWork& w : work) {
					const float floorD = std::max(w.worldScale, 1e-3f);
					float density = std::max(
						targetWorldDensity * w.worldScale * w.curveScale, floorD);
					if (std::abs(density - w.density)
						< 1e-4f * std::max(1.0f, w.density)) {
						kept.push_back(std::move(w));
						continue;
					}
					const smvk::StaticMeshUpload* up =
						cluster->GetRetainedUpload(w.handle);
					bool ok = false;
					while (density >= floorD && up) {
						SgLightmapChartOptions o = chartOpts;
						o.texelsPerMeter = density;
						std::string cerr_;
						ok = GenerateLightmapCharts(*up, o, w.chart, cerr_);
						if (ok)
							break;
						if (density <= floorD)
							break;
						density = std::max(density * 0.5f, floorD);
					}
					if (ok) {
						w.density = density;
						kept.push_back(std::move(w));
					} else {
						std::cerr << "  [LM] dropped node " << w.node
							<< " (cannot fit the texel budget)\n" << std::flush;
					}
				}
				work = std::move(kept);
			};
			uint32_t round = 0;
			float fitDensity = 0.0f;   // best (highest) fitting so far
			float failDensity = chartOpts.texelsPerMeter; // lowest failure
			// Initial probe at the requested density.
			if (tryPack()) {
				fitDensity = chartOpts.texelsPerMeter;
			} else {
				// Phase 1: halve until a pack fits (feasible lower bound).
				float probe = chartOpts.texelsPerMeter;
				while (probe > 1.0f) {
					if (++round > 32) {
						std::cerr << "[LM] atlas overflow\n";
						return 1;
					}
					probe = std::max(probe * 0.5f, 1.0f);
					rechartAll(probe);
					if (tryPack()) {
						fitDensity = probe;
						break;
					}
					failDensity = probe;
				}
				if (fitDensity <= 0.0f) {
					// Nothing fit even at 1 texel/m: drop the most
					// texel-hungry mesh per round until the pack fits.
					rechartAll(1.0f);
					while (!tryPack()) {
						if (++round > 32 || work.empty()) {
							std::cerr << "[LM] atlas overflow (" << work.size()
								<< " meshes remain; lower --lm-density or raise --lm-budget)\n";
							return 1;
						}
						size_t worst = 0;
						uint64_t worstTexels = 0;
						for (size_t i = 0; i < work.size(); ++i) {
							uint64_t texels = 0;
							for (const auto& ch : work[i].chart.charts)
								texels += uint64_t(ch.width) * ch.height;
							if (texels > worstTexels) {
								worstTexels = texels;
								worst = i;
							}
						}
						std::cerr << "  [LM] atlas round " << round
							<< ": drop node " << work[worst].node
							<< " (" << worstTexels << " texels)\n" << std::flush;
						work.erase(work.begin() + ptrdiff_t(worst));
					}
					fitDensity = 1.0f;
				} else {
					// Phase 2: geometric binary search in (fit, fail).
					// ~6 probes narrow the bracket to ~10%, then the
					// final rechartAll+tryPack leaves the atlas at the
					// fitting side (identical charts -> identical pack).
					while (failDensity / fitDensity > 1.10f) {
						if (++round > 32)
							break;
						const float mid = std::sqrt(fitDensity * failDensity);
						rechartAll(mid);
						if (tryPack()) {
							fitDensity = mid;
						} else {
							failDensity = mid;
						}
					}
					rechartAll(fitDensity);
					if (!tryPack()) {
						std::cerr << "[LM] atlas convergence regressed (round "
							<< round << ") - retrying at half density\n" << std::flush;
						fitDensity = std::max(fitDensity * 0.5f, 1.0f);
						rechartAll(fitDensity);
						if (!tryPack()) {
							std::cerr << "[LM] atlas overflow\n";
							return 1;
						}
					}
				}
			}
			std::cerr << "  [LM] density converged at " << fitDensity
				<< " texels/m world base (" << fitDensity * fitDensity
				<< " texels/m^2); atlas=" << atlas.width << "x" << atlas.height
				<< " (" << (uint64_t(atlas.width) * atlas.height * 72 / (1024 * 1024))
				<< " MiB gpu)\n" << std::flush;
			{
				uint32_t boosted = 0;
				float maxScale = 1.0f;
				for (const LmWork& w : work) {
					if (w.curveScale > 1.01f)
						++boosted;
					maxScale = std::max(maxScale, w.curveScale);
				}
				if (boosted != 0)
					std::cerr << "  [LM] curvature boost: " << boosted
						<< "/" << work.size() << " meshes denser, max x"
						<< maxScale << " (effective up to "
						<< fitDensity * maxScale << " texels/m)\n"
						<< std::flush;
			}
			// Chunk bases: atlas chunks are in request order (mesh-major,
			// chart-minor within each mesh).
			uint32_t next = 0;
			for (LmWork& w : work) {
				w.chunkBase = next;
				next += uint32_t(w.chart.charts.size());
			}
		}

		if (!work.empty() && lmXatlas) {
			// Xatlas brute-force pack convergence: one xatlas chart+pack
			// over ALL meshes per probe (world-prescaled positions under
			// a single world texelsPerUnit, bruteForce always on),
			// stacked into a fresh atlas. Same policy as the planar
			// search (halve to feasible, geometric binary search to
			// ~10%, drop hungriest meshes at the 1 texel/m floor), but
			// the WINNING probe's level+atlas are kept verbatim —
			// xatlas threading makes regeneration non-identical, so the
			// winner is never re-probed. fitDensity feeds the per-LOD
			// levels after the bake.
			constexpr uint32_t kMaxAtlasH = 8192;
			const uint32_t atlasW =
				std::max<uint32_t>(args.lmAtlasWidth, 256);
			SgLightmapXatlasOptions xopts;
			xopts.gutterTexels = chartOpts.gutterTexels;
			auto buildXinputs = [&]() {
				std::vector<SgLightmapXatlasMeshInput> ins;
				ins.reserve(work.size());
				for (const LmWork& w : work) {
					SgLightmapXatlasMeshInput in;
					in.upload = cluster->GetRetainedUpload(w.handle);
					in.meshKey = w.node;
					in.densityScale = w.worldScale * w.curveScale;
					ins.push_back(in);
				}
				return ins;
			};
			auto xatlasTexelsOf = [](const SgLightmapXatlasLevel& lv,
				size_t mi) {
				uint64_t t = 0;
				if (mi < lv.meshes.size())
					for (const auto& ch : lv.meshes[mi].chart.charts)
						t += uint64_t(ch.width) * ch.height;
				return t;
			};
			auto xatlasDropUnchartable = [&](SgLightmapXatlasLevel& lv) {
				for (size_t i = 0; i < lv.meshes.size();) {
					if (!lv.meshes[i].ok) {
						std::cerr << "  [LM] xatlas dropped node "
							<< work[i].node << " (unchartable)\n"
							<< std::flush;
						work.erase(work.begin() + ptrdiff_t(i));
						lv.meshes.erase(lv.meshes.begin() + ptrdiff_t(i));
					}
					else {
						++i;
					}
				}
			};
			auto xatlasProbeFits = [&](SgLightmapXatlasLevel& lv) {
				// Stacks the probe level into a scratch atlas: width cap
				// + texel budget decide fit (Append reports height
				// overflow itself).
				SgLightmapAtlas probe;
				std::vector<uint32_t> keys;
				keys.reserve(work.size());
				for (const LmWork& w : work)
					keys.push_back(w.node);
				std::vector<uint32_t> dummy;
				std::string aerr;
				if (!AppendXatlasLevelToAtlas(probe, lv, keys,
						args.lmLobes, atlasW, kMaxAtlasH, dummy, aerr))
					return false;
				return uint64_t(probe.width) * probe.height
					<= args.lmBudget;
			};
			uint32_t round = 0;
			float fitDensity = 0.0f;   // best (highest) fitting so far
			float failDensity = chartOpts.texelsPerMeter; // lowest failure
			SgLightmapXatlasLevel bestLevel;
			auto tryXatlasPack = [&](float worldDensity) {
				xopts.texelsPerMeter = std::max(worldDensity, 1e-3f);
				SgLightmapXatlasLevel lv;
				std::string xerr;
				if (!GenerateLightmapChartsXatlas(buildXinputs(), xopts,
						lv, xerr)) {
					std::cerr << "  [LM] xatlas probe " << worldDensity
						<< " texels/m failed: " << xerr << "\n" << std::flush;
					return false;
				}
				// Membership changes (drops) are only allowed before the
				// first fit: afterwards lv must stay parallel to the
				// stored winner, so an unchartable mesh just fails the
				// probe (halving/the floor path resolve it).
				if (fitDensity <= 0.0f) {
					xatlasDropUnchartable(lv);
				} else {
					for (const auto& m : lv.meshes) {
						if (!m.ok)
							return false;
					}
				}
				if (work.empty() || !xatlasProbeFits(lv))
					return false;
				bestLevel = std::move(lv);
				return true;
			};
			// Initial probe at the requested density.
			if (tryXatlasPack(chartOpts.texelsPerMeter)) {
				fitDensity = chartOpts.texelsPerMeter;
			} else {
				// Phase 1: halve until a pack fits (feasible lower bound).
				float probeD = chartOpts.texelsPerMeter;
				while (probeD > 1.0f) {
					if (++round > 32) {
						std::cerr << "[LM] atlas overflow\n";
						return 1;
					}
					probeD = std::max(probeD * 0.5f, 1.0f);
					if (tryXatlasPack(probeD)) {
						fitDensity = probeD;
						break;
					}
					failDensity = probeD;
				}
				if (fitDensity <= 0.0f) {
					// Nothing fit even at 1 texel/m: drop the most
					// texel-hungry mesh per round until the pack fits.
					xopts.texelsPerMeter = 1.0f;
					std::string xerr;
					if (!GenerateLightmapChartsXatlas(buildXinputs(),
							xopts, bestLevel, xerr)) {
						std::cerr << "[LM] atlas overflow (" << xerr
							<< "; lower --lm-density or raise --lm-budget)\n";
						return 1;
					}
					xatlasDropUnchartable(bestLevel);
					while (!xatlasProbeFits(bestLevel)) {
						if (++round > 32 || work.empty()) {
							std::cerr << "[LM] atlas overflow ("
								<< work.size()
								<< " meshes remain; lower --lm-density or raise --lm-budget)\n";
							return 1;
						}
						size_t worst = 0;
						uint64_t worstTexels = 0;
						for (size_t i = 0; i < work.size(); ++i) {
							const uint64_t texels =
								xatlasTexelsOf(bestLevel, i);
							if (texels > worstTexels) {
								worstTexels = texels;
								worst = i;
							}
						}
						std::cerr << "  [LM] atlas round " << round
							<< ": drop node " << work[worst].node
							<< " (" << worstTexels << " texels)\n" << std::flush;
						work.erase(work.begin() + ptrdiff_t(worst));
						bestLevel.meshes.erase(
							bestLevel.meshes.begin() + ptrdiff_t(worst));
					}
					fitDensity = 1.0f;
				} else {
					// Phase 2: geometric binary search in (fit, fail).
					// Winners are kept verbatim (no regen — see above).
					while (failDensity / fitDensity > 1.10f) {
						if (++round > 32)
							break;
						const float mid =
							std::sqrt(fitDensity * failDensity);
						if (tryXatlasPack(mid)) {
							fitDensity = mid;
						} else {
							failDensity = mid;
						}
					}
				}
			}
			// Land the winning level in the real atlas (deterministic
			// re-stack of the kept rects) and hand charts to the bake.
			{
				atlas = SgLightmapAtlas{};
				std::vector<uint32_t> keys;
				keys.reserve(work.size());
				for (const LmWork& w : work)
					keys.push_back(w.node);
				std::vector<uint32_t> bases;
				std::string aerr;
				if (!AppendXatlasLevelToAtlas(atlas, bestLevel, keys,
						args.lmLobes, atlasW, kMaxAtlasH, bases, aerr)) {
					std::cerr << "[LM] atlas convergence regressed: "
						<< aerr << "\n";
					return 1;
				}
				for (size_t i = 0; i < work.size(); ++i) {
					work[i].chart =
						std::move(bestLevel.meshes[i].chart);
					work[i].chunkBase = bases[i];
					work[i].density = fitDensity * work[i].worldScale
						* work[i].curveScale;
				}
			}
			lmFitDensityWorld = fitDensity;
			std::cerr << "  [LM] xatlas density converged at " << fitDensity
				<< " texels/m world base (" << fitDensity * fitDensity
				<< " texels/m^2); meshes=" << work.size()
				<< " atlas=" << atlas.width << "x" << atlas.height
				<< " (" << (uint64_t(atlas.width) * atlas.height * 72 / (1024 * 1024))
				<< " MiB gpu, bruteForce)\n" << std::flush;
			{
				uint32_t boosted = 0;
				float maxScale = 1.0f;
				for (const LmWork& w : work) {
					if (w.curveScale > 1.01f)
						++boosted;
					maxScale = std::max(maxScale, w.curveScale);
				}
				if (boosted != 0)
					std::cerr << "  [LM] curvature boost: " << boosted
						<< "/" << work.size() << " meshes denser, max x"
						<< maxScale << " (effective up to "
						<< fitDensity * maxScale << " texels/m)\n"
						<< std::flush;
			}
		}

		// Bake every chart (sun + spots + materials + pass-through glass)
		// in ONE shared-pool batch, then stamp the results into the
		// retained uploads: per-corner atlas-space texCoord2 + the mesh's
		// lobe count (lmChunk).
		SgLightmapBakeConfig lcfg{};
		lcfg.raysPerTexel = std::max<uint32_t>(args.lmRays, 16);
		lcfg.maxBounces = std::min<uint32_t>(std::max<uint32_t>(args.lmBounces, 1), 8);
		lcfg.lobeRelError = std::max(args.lmLobeRelErr, 0.0f);
		SgLightmapBakeStats lstats{};
		uint32_t lmMeshes = 0;
		uint64_t seamDups = 0;
		{
			// World-space texels per chart (chart positions are
			// node-local; the trace scene is world-space).
			std::vector<std::vector<SgLightmapTexel>> chartTexels;
			std::vector<SgLightmapChunkBatch> batches;
			chartTexels.reserve(atlas.chunks.size());
			batches.reserve(atlas.chunks.size());
			for (const LmWork& w : work) {
				const glm::mat4& g = cluster->scene.globalTransform[w.node];
				const glm::mat3 g3(g);
				for (uint32_t ci = 0; ci < w.chart.charts.size(); ++ci) {
					const auto& ch = w.chart.charts[ci];
					std::vector<SgLightmapTexel>& tx = chartTexels.emplace_back();
					tx.reserve(ch.texels.size());
					for (const SgLightmapTexel& t : ch.texels) {
						SgLightmapTexel wt = t;
						wt.position = glm::vec3(g * glm::vec4(t.position, 1.0f));
						wt.normal = glm::normalize(g3 * t.normal);
						tx.push_back(wt);
					}
				SgLightmapChunkBatch b{};
				b.payload = &atlas.payloads[w.chunkBase + ci];
				b.texels = &tx;
				// Per-mesh lobe selection: chunks group by meshKey and
				// the bake writes the chosen count into the chunk
				// (all of a mesh's chunks share it; the draw ring
				// carries one count per draw).
				b.meshKey = w.node;
				b.chunkMeta = &atlas.chunks[w.chunkBase + ci];
				batches.push_back(b);
				}
			}
			if (!BakeSgLightmapChunks(traceScene, args.lmLobes,
					std::span<const SgLightmapChunkBatch>(batches), lcfg,
					lstats, [](const char* line) { std::cout << line; })) {
				std::cerr << "[LM] bake failed\n";
				return 1;
			}
		}

		// NOTE: the OIDN denoise pass lives AFTER the stamp loop and the
		// per-LOD atlas levels (xatlas path), so it covers every baked
		// level's payloads exactly once (see below).
		for (LmWork& w : work) {
			smvk::StaticMeshUpload* upload = const_cast<smvk::StaticMeshUpload*>(
				cluster->GetRetainedUpload(w.handle));
			const size_t triCount = upload ? upload->indices.size() / 3 : 0;
			if (!upload || w.chart.triChart.size() != triCount) {
				std::cerr << "[LM] triangle count mismatch for node " << w.node
					<< " - mesh skipped\n";
				continue;
			}
			// Per-corner atlas uv2 stamping (seam-correct; see
			// StampLightmapUvs): every triangle corner through the
			// triangle's own chart, seam duplicates appended, index
			// buffer rebuilt in the same triangle order.
			std::vector<uint32_t> chartOf;
			const uint32_t lobes = StampLightmapUvs(*upload, w.chart,
				w.chunkBase, atlas, chartOpts.gutterTexels, args.lmLobes,
				chartOf, seamDups);
			// Chart provenance for the LOD cook (CookLodSiblings): one
			// chart id per vertex over the STAMPED layout. This is what
			// lets the simplifier keep collapses inside one lightmap
			// chart (chart id rides the error metric).
			lmVertexCharts.emplace(w.handle.index(), std::move(chartOf));
			// The draw ring carries the LOBE COUNT (5/6/9/12) chosen by
			// the bake for this MESH (read from any of its chunks —
			// the per-mesh selection wrote them all); 0xFFFFFFFF = no
			// lightmap stays the pak default.
			if (smvk::StaticMesh* m = cluster->staticMeshes.get(w.handle))
				m->lmChunk = lobes;
			// No meshlet work in the bake stage (decoupled by design):
			// meshlets are built once, after ALL lightmap stamping
			// finishes, by the "Meshlet restore" stage below — from
			// this final stamped topology, never a pre-stamp layout.
			++lmMeshes;
		}

		// ---- Per-LOD atlas levels (xatlas path with --lods on) ----
		// Siblings are cooked here (no meshlets, no audit — every kept
		// level gets exact UVs below, so there is nothing to audit),
		// then each sibling level charts + bakes its OWN rects (exact
		// UVs for its simplified topology) stacked into the shared
		// atlas. Levels that fail (chart/pack/bake) keep the simplified
		// LOD0 UVs they inherited and share LOD0's lobe count (the
		// shared-UV behavior the audit used to measure). The generic
		// LOD block below stays authoritative for --lightmap off; with
		// xatlas on it no-ops on these meshes (idempotent skip).
		if (lmXatlas && args.lods && !work.empty()
			&& lmFitDensityWorld > 0.0f) {
			{
				smvk::MeshOptConfig lodOpt = opt;
				lodOpt.generateLods = true;
				smvk::CookLodSiblingsOptions sibOpts;
				sibOpts.vertexCharts = &lmVertexCharts;
				sibOpts.audit = false;
				smvk::MeshletConfig noMeshlets = meshletCfg;
				noMeshlets.enabled = false;
				for (auto& sc : sceneClusters)
					smvk::CookLodSiblings(*sc, lodOpt, noMeshlets,
						sibOpts);
				std::cerr << "  [LM] xatlas: siblings cooked "
					"(per-LOD bake follows; audit skipped)\n" << std::flush;
			}
			// Sibling lookup: base uri -> lodIndex -> sibling uri
			// (from the markers CookLodSiblings just registered).
			std::map<std::string, std::map<uint32_t, std::string>> lodUris;
			uint32_t maxLod = 0;
			for (const auto& [uri, lod] : cluster->ListMeshLods()) {
				if (lod.lodIndex == 0)
					continue;
				maxLod = std::max(maxLod, lod.lodIndex);
				lodUris[std::string(
					smvk::BaseMeshUriForLod(uri).Str())][lod.lodIndex] =
					std::string(uri.Str());
			}
			SgLightmapXatlasOptions xlodOpts;
			xlodOpts.texelsPerMeter = lmFitDensityWorld;
			xlodOpts.gutterTexels = chartOpts.gutterTexels;
			const uint32_t lodAtlasW =
				std::max<uint32_t>(args.lmAtlasWidth, 256);
			constexpr uint32_t kLodMaxAtlasH = 8192;
			for (uint32_t level = 1; level <= maxLod; ++level) {
				struct SibJob {
					uint32_t node = 0;
					smvk::ClusterStaticMeshHandle sib{};
					float densityScale = 1.0f;
					uint32_t baseLobes = 9;
					SgLightmapChartResult chart;
					uint32_t chunkBase = 0xFFFFFFFFu;
				};
				std::vector<SibJob> jobs;
				for (const LmWork& w : work) {
					const std::string baseUri =
						cluster->GetMeshUri(w.handle).Str();
					if (baseUri.empty())
						continue;
					const auto bit = lodUris.find(baseUri);
					if (bit == lodUris.end())
						continue;
					const auto lit = bit->second.find(level);
					if (lit == bit->second.end())
						continue;
					const auto h = cluster->FindMeshByUri(
						smvk::StringViewT(lit->second));
					if (!h.has_value() || !h->valid())
						continue;
					const smvk::StaticMeshUpload* up =
						cluster->GetRetainedUpload(*h);
					if (!up || up->vertices.empty() || up->indices.empty())
						continue;
					uint32_t baseLobes = args.lmLobes;
					if (w.chunkBase < atlas.chunks.size())
						baseLobes = atlas.chunks[w.chunkBase].lobeCount;
					SibJob j;
					j.node = w.node;
					j.sib = *h;
					j.densityScale = w.worldScale * w.curveScale;
					j.baseLobes = baseLobes;
					jobs.push_back(std::move(j));
				}
				if (jobs.empty())
					continue;
				std::cerr << "  [LM] xatlas lod" << level << ": charting "
					<< jobs.size() << " siblings at " << lmFitDensityWorld
					<< " texels/m\n" << std::flush;
				std::vector<SgLightmapXatlasMeshInput> ins;
				ins.reserve(jobs.size());
				for (const SibJob& j : jobs) {
					SgLightmapXatlasMeshInput in;
					in.upload = cluster->GetRetainedUpload(j.sib);
					in.meshKey = j.node;
					in.densityScale = j.densityScale;
					ins.push_back(in);
				}
				SgLightmapXatlasLevel xlevel;
				std::string xerr;
				bool okGen = GenerateLightmapChartsXatlas(ins, xlodOpts,
					xlevel, xerr);
				std::vector<uint32_t> keys, bases;
				keys.reserve(jobs.size());
				for (const SibJob& j : jobs)
					keys.push_back(j.node);
				bool okAppend = false;
				if (okGen) {
					std::string aerr;
					okAppend = AppendXatlasLevelToAtlas(atlas, xlevel,
						keys, args.lmLobes, lodAtlasW, kLodMaxAtlasH,
						bases, aerr);
					if (!okAppend)
						std::cerr << "  [LM] xatlas lod" << level
							<< " pack overflow: " << aerr
							<< " (shared-UV fallback)\n" << std::flush;
				}
				else {
					std::cerr << "  [LM] xatlas lod" << level
						<< " chart failed: " << xerr
						<< " (shared-UV fallback)\n" << std::flush;
				}
				if (!okGen || !okAppend) {
					// Shared-UV fallback: keep the inherited LOD0 UVs,
					// share LOD0's lobe count (probe the same chunks).
					for (const SibJob& j : jobs) {
						if (smvk::StaticMesh* m =
								cluster->staticMeshes.get(j.sib))
							m->lmChunk = j.baseLobes;
					}
					continue;
				}
				for (size_t ji = 0; ji < jobs.size(); ++ji) {
					if (ji < xlevel.meshes.size()
						&& xlevel.meshes[ji].ok) {
						jobs[ji].chart =
							std::move(xlevel.meshes[ji].chart);
						jobs[ji].chunkBase = ji < bases.size()
							? bases[ji] : 0xFFFFFFFFu;
					}
				}
				// Bake the level (world-space texels through the base
				// node's global transform — siblings render in place of
				// the base under the same node).
				{
					// Exact batch count up front: batches hold POINTERS
					// into texelBuf, so texelBuf must never reallocate
					// after the first batch is taken (reserve guesses
					// caused use-after-realloc crashes in the bake pool).
					size_t lodBatches = 0;
					for (const SibJob& j : jobs) {
						if (!j.chart.charts.empty()
							&& j.chunkBase != 0xFFFFFFFFu)
							lodBatches += j.chart.charts.size();
					}
					std::vector<std::vector<SgLightmapTexel>> texelBuf;
					std::vector<SgLightmapChunkBatch> batches;
					texelBuf.reserve(lodBatches);
					batches.reserve(lodBatches);
					for (SibJob& j : jobs) {
						if (j.chart.charts.empty()
							|| j.chunkBase == 0xFFFFFFFFu)
							continue;
						const glm::mat4& g =
							cluster->scene.globalTransform[j.node];
						const glm::mat3 g3(g);
						for (uint32_t ci = 0;
							ci < j.chart.charts.size(); ++ci) {
							const auto& ch = j.chart.charts[ci];
							std::vector<SgLightmapTexel>& tx =
								texelBuf.emplace_back();
							tx.reserve(ch.texels.size());
							for (const SgLightmapTexel& t : ch.texels) {
								SgLightmapTexel wt = t;
								wt.position = glm::vec3(
									g * glm::vec4(t.position, 1.0f));
								wt.normal = glm::normalize(g3 * t.normal);
								tx.push_back(wt);
							}
							SgLightmapChunkBatch b{};
							b.payload =
								&atlas.payloads[j.chunkBase + ci];
							b.texels = &tx;
							b.meshKey = j.node;
							b.chunkMeta =
								&atlas.chunks[j.chunkBase + ci];
							batches.push_back(b);
						}
					}
					if (!batches.empty()) {
						SgLightmapBakeStats ls{};
						if (!BakeSgLightmapChunks(traceScene, args.lmLobes,
								std::span<const SgLightmapChunkBatch>(
									batches), lcfg, ls,
								[](const char* line) {
									std::cout << line;
								})) {
							std::cerr << "  [LM] xatlas lod" << level
								<< " bake failed (shared-UV fallback)\n"
								<< std::flush;
							for (const SibJob& j : jobs) {
								if (smvk::StaticMesh* m =
										cluster->staticMeshes.get(j.sib))
									m->lmChunk = j.baseLobes;
							}
							continue;
						}
						lstats.texelsBaked += ls.texelsBaked;
						lstats.pathsTraced += ls.pathsTraced;
						lstats.samplesClamped += ls.samplesClamped;
						lstats.lobePicks.insert(lstats.lobePicks.end(),
							ls.lobePicks.begin(), ls.lobePicks.end());
					}
				}
				// Stamp the siblings (seam-correct, same helper).
				uint32_t bakedSibs = 0;
				for (SibJob& j : jobs) {
					if (j.chart.charts.empty()
						|| j.chunkBase == 0xFFFFFFFFu) {
						if (smvk::StaticMesh* m =
								cluster->staticMeshes.get(j.sib))
							m->lmChunk = j.baseLobes;
						continue;
					}
					smvk::StaticMeshUpload* up =
						const_cast<smvk::StaticMeshUpload*>(
							cluster->GetRetainedUpload(j.sib));
					if (!up
						|| up->indices.size() / 3
							!= j.chart.triChart.size()) {
						if (smvk::StaticMesh* m =
								cluster->staticMeshes.get(j.sib))
							m->lmChunk = j.baseLobes;
						continue;
					}
					std::vector<uint32_t> dummy;
					const uint32_t lobes = StampLightmapUvs(*up, j.chart,
						j.chunkBase, atlas, chartOpts.gutterTexels,
						j.baseLobes, dummy, seamDups);
					if (smvk::StaticMesh* m =
							cluster->staticMeshes.get(j.sib))
						m->lmChunk = lobes;
					++bakedSibs;
				}
				++lmLodLevelsBaked;
				std::cerr << "  [LM] xatlas lod" << level << ": baked "
					<< bakedSibs << "/" << jobs.size()
					<< " siblings (atlas now " << atlas.width << "x"
					<< atlas.height << ")\n" << std::flush;
			}
		}

		// OIDN denoise pass: RTLightmap over the fitted per-lobe
		// amplitude layers, in place (see lm_denoise.cpp). Runs after ALL
		// bakes (LOD0 + every per-LOD level) and before the atlas write,
		// so the pak + .sglm carry denoised data. Failure is non-fatal:
		// the cook continues on the (possibly partially denoised)
		// amplitudes.
		if (args.lmDenoise && !work.empty()) {
			if (!pakcook::DenoiseAvailable()) {
				std::cerr << "[LM-DN] --lm-denoise on but this PakCooker "
					"was built without OIDN (extract the prebuilt release "
					"into external/oidn, reconfigure with VULKANTOY_OIDN) "
					"- denoise skipped\n" << std::flush;
			}
			else {
				const auto dnT0 = std::chrono::steady_clock::now();
				const pakcook::DenoiseQuality dq =
					args.lmDenoiseQuality == "balanced"
					? pakcook::DenoiseQuality::Balanced
					: pakcook::DenoiseQuality::High;
				if (pakcook::DenoiseLightmapAtlas(atlas, dq)) {
					std::cerr << "  [LM-DN] atlas denoised in "
						<< std::chrono::duration<double>(
							std::chrono::steady_clock::now() - dnT0).count()
						<< "s\n" << std::flush;
				}
				else {
					std::cerr << "[LM-DN] denoise FAILED - continuing with "
						"the current amplitudes\n" << std::flush;
				}
				if (args.lmRays >= 512)
					std::cerr << "  [LM-DN] hint: --lm-rays " << args.lmRays
						<< " is wasted with denoising on - try --lm-rays 128 "
						"for a ~" << (args.lmRays / 128) << "x faster trace\n"
						<< std::flush;
			}
		}
		lstats.seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - lmT0).count();
		// Write the atlas next to the pak: <contentDir>/gi/<scene>.sglm.
		if (!work.empty()) {
			std::filesystem::path pakPath(args.outPak);
			const std::string sceneId = pakPath.stem().generic_string();
			std::filesystem::path sglmPath =
				std::filesystem::path(args.contentDir) / "gi"
				/ (sceneId + ".sglm");
			std::error_code ec;
			std::filesystem::create_directories(sglmPath.parent_path(), ec);
			std::string werr;
			if (!atlas.WriteFile(sglmPath.generic_string(), werr)) {
				std::cerr << "[LM] atlas write failed: " << werr << std::endl;
				return 1;
			}
			std::cerr << "  [LM] atlas " << sglmPath.generic_string()
				<< " (" << std::filesystem::file_size(sglmPath) << " B)\n" << std::flush;
			// Raw-atlas debug PNG next to the atlas (<scene>_atlas.png):
			// the baked payload itself (lobe 0, editor flat-view display
			// mapping), NOT the uv2-unwrapped geometry draw the editor
			// exports — comparing the two localizes corruption to the
			// bake/payload vs the stamping/draw path. Non-fatal.
			{
				std::filesystem::path pngPath =
					std::filesystem::path(args.contentDir) / "gi"
					/ (sceneId + "_atlas.png");
				std::string perr;
				if (pakcook::WriteLightmapAtlasPng(atlas,
						pngPath.generic_string(), perr)) {
					std::cerr << "  [LM] atlas png " << pngPath.generic_string()
						<< " (" << std::filesystem::file_size(pngPath)
						<< " B)\n" << std::flush;
				} else {
					std::cerr << "[LM] atlas png write failed: " << perr
						<< "\n" << std::flush;
				}
			}
			// Name the atlas inside the (single) cooked cluster so the
			// runtime resolves gi/<name>.sglm from the pak, not the id.
			cluster->RegisterGiBinding(smvk::GiBinding::Kind::Lightmap,
				sceneId);
		}
		// Lobe selection diagnostics: per mesh, the candidate counts'
		// relative irradiance errors and the kept count. "Nothing passed"
		// (all errors over budget, chosen = preferred) is distinguishable
		// from "the preferred count genuinely won" here — the chunk
		// histogram alone cannot tell them apart. rel = vs the traced
		// probes (Monte-Carlo noise floor, diagnostics); deg = vs the
		// preferred-count reconstruction (the selection criterion).
		for (const auto& pick : lstats.lobePicks) {
			std::cerr << "  [LM] lobe pick mesh " << pick.meshKey
				<< " chosen=" << pick.chosen;
			for (uint32_t s = 0; s < pick.candCount; ++s)
				std::cerr << " " << pick.candCounts[s]
					<< "(rel " << (pick.relErr[s] * 100.0) << "%"
					<< ", deg " << (pick.degErr[s] * 100.0) << "%)";
			std::cerr << "\n" << std::flush;
		}
		// Lobe histogram: how many chunks the per-mesh selection kept at
		// each count (flat-lit geometry drops to 5-6, contrast-heavy
		// areas keep 9-12).
		{
			uint32_t lobes5 = 0, lobes6 = 0, lobes9 = 0, lobes12 = 0;
			for (const SgLightmapChunk& c : atlas.chunks) {
				if (c.lobeCount == 5) ++lobes5;
				else if (c.lobeCount == 6) ++lobes6;
				else if (c.lobeCount == 9) ++lobes9;
				else ++lobes12;
			}
			std::cerr << "  [LM] lobe chunks: 5=" << lobes5
				<< " 6=" << lobes6
				<< " 9=" << lobes9
				<< " 12=" << lobes12 << "\n" << std::flush;
		}
		std::cerr << "  [LM] lightmaps: meshes=" << lmMeshes
			<< " chunks=" << atlas.chunks.size()
			<< " atlas=" << atlas.width << "x" << atlas.height
			<< " texels=" << lstats.texelsBaked
			<< " paths=" << lstats.pathsTraced
			<< " clamped=" << lstats.samplesClamped
			<< " seamVerts=" << seamDups
			<< " seconds=" << lstats.seconds
			<< (lmXatlas ? " charts=xatlas-bruteforce" : " charts=built-in")
			<< (lmLodLevelsBaked ? " lodLevels=" + std::to_string(
				lmLodLevelsBaked) : "")
			<< " (skipped: blend=" << skipBlend
			<< " instanced=" << skipInstanced
			<< " nomat=" << skipNoMat
			<< " decal=" << skipDecal
			<< " excluded=" << skipExcluded
			<< " nolm=" << skipNoLm
			<< " hidden=" << skipHidden
			<< " small=" << skipSmall << ")\n" << std::flush;
	}

	// ---------------------------------------------------------------------------
	// Meshlet restore (post-lightmap): with --lightmap on, meshlets were
	// deliberately NOT built anywhere earlier in this cook
	// (importMeshletCfg disabled the importer and merged-block paths);
	// they are built HERE, once, from the FINAL topology — after the
	// atlas uv2 stamping and seam vertex duplication finished. Every
	// meshlet vertex window references already-final texCoord2: the
	// parameterization is complete before meshletization begins, so
	// meshlet generation can partition triangles but never changes
	// the lightmap parameterization. A failed build leaves the mesh
	// WITHOUT meshlets (the runtime draws the classic indexed path) —
	// never stale ones referencing a pre-stamp vertex layout. Covers
	// every scene cluster and every drawable mesh (de-instanced clones
	// and merged blocks rebind their nodes in meshForNode); meshes
	// still shared by several nodes are deduped by pool index (one
	// meshlet set per upload). Sibling meshes ("<base>/lodN") are NOT
	// node-referenced, so the sibling pass below covers them from the
	// LOD markers (in xatlas mode siblings stamp AFTER the sibling cook
	// and need meshlets here; otherwise they already carry the inline
	// build and the pass skips them). With --lightmap off this stage is
	// skipped and the import-time meshlets stay in force.
	// ---------------------------------------------------------------------------
	if (args.lightmap && meshletCfg.enabled) {
		uint32_t restoredMeshes = 0;
		uint32_t restoredMeshlets = 0;
		for (auto& sc : sceneClusters) {
			smvk::Cluster& c = *sc;
			std::unordered_set<uint32_t> seen;
			seen.reserve(c.scene.meshForNode.size() * 2);
			for (const auto& nodeMesh : c.scene.meshForNode) {
				const uint32_t meshIdx = nodeMesh.second;
				if (!seen.insert(meshIdx).second)
					continue; // still-shared mesh: one meshlet set per upload
				const smvk::ClusterStaticMeshHandle h =
					c.staticMeshes.getHandle(meshIdx);
				const smvk::StaticMeshUpload* up = c.GetRetainedUpload(h);
				if (!h.valid() || !up || up->vertices.empty()
					|| up->indices.empty())
					continue;
				smvk::CookedMeshlets cooked;
				smvk::MeshletStats mst;
				std::string merr;
				if (!smvk::BuildMeshletsForUpload(*up, meshletCfg,
						&cooked, &mst, &merr)) {
					if (!merr.empty() && merr != "meshlets disabled")
						std::cerr << "  [LM] meshlet build failed mesh "
							<< meshIdx << ": " << merr
							<< " (mesh ships without meshlets)\n" << std::flush;
					continue;
				}
				const auto uri = c.GetMeshUri(h);
				if (!uri.empty())
					c.RegisterMeshMeshlets(uri, cooked);
				++restoredMeshes;
				restoredMeshlets += mst.meshletCount;
			}
			// Sibling pass (LOD markers, lodIndex >= 1): meshes without a
			// set get one from their FINAL stamped upload. Skips siblings
			// the sibling cook already built inline (non-xatlas path).
			for (const auto& [uri, lod] : c.ListMeshLods()) {
				if (lod.lodIndex == 0)
					continue;
				if (c.GetMeshMeshlets(uri).has_value())
					continue; // inline build already covers it
				const auto h = c.FindMeshByUri(uri);
				if (!h.has_value() || !h->valid())
					continue;
				const smvk::StaticMeshUpload* up = c.GetRetainedUpload(*h);
				if (!up || up->vertices.empty() || up->indices.empty())
					continue;
				smvk::CookedMeshlets cooked;
				smvk::MeshletStats mst;
				std::string merr;
				if (!smvk::BuildMeshletsForUpload(*up, meshletCfg,
						&cooked, &mst, &merr)) {
					if (!merr.empty() && merr != "meshlets disabled")
						std::cerr << "  [LM] meshlet build failed sibling "
							<< uri.Str() << ": " << merr << "\n" << std::flush;
					continue;
				}
				c.RegisterMeshMeshlets(uri, cooked);
				++restoredMeshes;
				restoredMeshlets += mst.meshletCount;
			}
		}
		std::cerr << "  [LM] meshlets restored: meshes=" << restoredMeshes
			<< " meshlets=" << restoredMeshlets
			<< " (post-bake, final stamped topology)\n" << std::flush;
	}

	// ---------------------------------------------------------------------------
	// LOD siblings (post-bake): lightmapped meshes simplify from their
	// STAMPED uploads — texCoord2 rides the vertex remap, chart ids ride
	// the simplifier's error metric so collapses stay inside one lightmap
	// chart, siblings share LOD0's lobe count, and the per-level audit
	// measures the parameterization drift in atlas texels (log-only; the
	// gate flag drops failing levels). With --lightmap off this is exactly
	// the old import-time chain, just later in the pipeline. With
	// --lm-charts xatlas the sibling cook already ran inside the lightmap
	// block (per-LOD atlas levels need the siblings first); this call then
	// no-ops on those meshes (idempotent skip) and the audit stays off
	// (every kept level carries exact UVs — nothing to audit).
	// ---------------------------------------------------------------------------
	{
		smvk::MeshOptConfig lodOpt = opt;
		lodOpt.generateLods = args.lods;
		smvk::CookLodSiblingsOptions sibOpts;
		sibOpts.vertexCharts = &lmVertexCharts;
		sibOpts.audit = args.lightmap && args.lmLodAudit
			&& !lmVertexCharts.empty() && !lmXatlas;
		sibOpts.atlasWidth = atlas.width;
		sibOpts.atlasHeight = atlas.height;
		sibOpts.auditThresholdTexels = args.lmLodAuditTx;
		sibOpts.auditGate = args.lmLodAuditGate;
		// Per scene (the audit/charts only exist for the baked scene;
		// the gate above already requires them non-empty).
		for (auto& sc : sceneClusters)
			smvk::CookLodSiblings(*sc, lodOpt, meshletCfg, sibOpts);
	}

	// Probe sidecar names, registered on every cooked cluster (the
	// serializer unions them into one pak-level Gi registry).
	for (const std::string& probe : args.giProbes) {
		for (auto& sc : sceneClusters)
			sc->RegisterGiBinding(smvk::GiBinding::Kind::Probe, probe);
		std::cout << "Registered probe GI '" << probe << "'\n";
	}

	smvk::PakWriter writer;
	writer.SetCompression(
		args.compress ? smvk::PakCompression::Lz4 : smvk::PakCompression::None,
		args.lz4Accel);
	smvk::ClusterSerializeStats stats;
	std::string err;
	std::vector<const smvk::Cluster*> cookPtrs;
	for (auto& sc : sceneClusters)
		cookPtrs.push_back(sc.get());
	if (!SerializeClusters(cookPtrs, sceneNames, writer, &stats, &err)) {
		std::cerr << "Serialize failed: " << err << std::endl;
		return 1;
	}
	std::vector<uint8_t> pak = writer.Finalize();

	{
		std::filesystem::path out(args.outPak);
		std::error_code ec;
		std::filesystem::create_directories(out.parent_path(), ec);
		FILE* f = std::fopen(out.string().c_str(), "wb");
		if (!f) {
			std::cerr << "Cannot open output: " << args.outPak << std::endl;
			return 1;
		}
		fwrite(pak.data(), 1, pak.size(), f);
		fclose(f);
	}

	const auto t1 = std::chrono::steady_clock::now();
	const double secs = std::chrono::duration<double>(t1 - t0).count();

	std::cout << "Cooked " << args.inVfs << " -> " << args.outPak << "\n"
		<< "  scenes=" << sceneNames.size()
		<< " meshes=" << stats.meshes << " materials=" << stats.materials
		<< " textures=" << stats.textures << " (" << stats.textureBytes << " B)"
		<< " nodes=" << stats.nodes << " lights=" << stats.lights
		<< " gi=" << stats.giBindings
		<< " pak=" << pak.size() << " B"
		<< " staticRoots=" << stats.staticRoots
		<< " meshleted=" << stats.meshesWithMeshlets << "/" << stats.meshletCount
		<< " compressed=" << writer.CompressedEntryCount() << "/" << writer.EntryCount()
		<< (args.compress ? " (lz4)" : " (raw)")
		<< " in " << secs << "s\n";

	// Self-check: re-open the written file through the production reader
	// (header/footer/CRC/TOC validation + payload spot-checks).
	std::optional<smvk::PakReader> verifyReader;
	if (args.verify || args.dumpManifest) {
		std::ifstream rf(args.outPak, std::ios::binary | std::ios::ate);
		if (!rf) {
			std::cerr << "Verify: cannot re-open " << args.outPak << std::endl;
			return 1;
		}
		const auto size = rf.tellg();
		rf.seekg(0);
		std::vector<uint8_t> image(static_cast<size_t>(size));
		if (!rf.read(reinterpret_cast<char*>(image.data()), size)) {
			std::cerr << "Verify: read failed" << std::endl;
			return 1;
		}
		std::string verr;
		// The image must outlive the reader borrow; keep it static duration.
		static std::vector<uint8_t> keepAlive = std::move(image);
		auto reader = smvk::PakReader::Open(
			std::span<const uint8_t>(keepAlive), verr);
		if (!reader) {
			std::cerr << "Verify: PakReader rejected: " << verr << std::endl;
			return 1;
		}
		verifyReader = std::move(reader);
	}
	if (args.verify && verifyReader) {
		const auto& reader = *verifyReader;
		size_t badPayloads = 0;
		for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Texture)) {
			smvk::PakTexturePayload p;
			if (!smvk::ParseTexturePayload(reader.Payload(*e), p))
				++badPayloads;
		}
		std::cout << "  verify: entries=" << reader.EntryCount()
			<< " meshes=" << reader.BlobsOfKind(smvk::PakAssetKind::Mesh).size()
			<< " materials=" << reader.BlobsOfKind(smvk::PakAssetKind::Material).size()
			<< " textures=" << reader.BlobsOfKind(smvk::PakAssetKind::Texture).size()
			<< " scene=" << reader.BlobsOfKind(smvk::PakAssetKind::Scene).size()
			<< (badPayloads ? " BAD PAYLOADS!" : " OK") << "\n";
		if (badPayloads) {
			std::cerr << "Verify: " << badPayloads << " malformed texture payload(s)\n";
			return 1;
		}
	}

	// No sidecar: the dump is printed straight from the pak TOC/blobs.
	if (args.dumpManifest && verifyReader) {
		DumpManifestFromPak(*verifyReader);
	}

	return 0;
}
