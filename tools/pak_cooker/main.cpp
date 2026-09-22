// PakCooker: offline glTF -> .pak cooker that works exclusively on the VFS
// and needs NO GPU (cook-mode Cluster, see Cluster::SetCookMode).
//
//   PakCooker --content-dir content --in-vfs models/Sponza/sponza.gltf
//             --out-pak content/paks/sponza.pak [--meshopt on|off]
//             [--overdraw 1.05] [--verify on|off] [--dump-manifest on|off]
//             [--lods on|off] [--lod-ratios 0.6,0.3,0.12] [--lod-error 0.02]
//             [--compress on|off] [--lz4-accel 1]
//             [--lightmap on|off --lm-meters-per-texel 0.25 --lm-lobes 12
//              --lm-atlas-width 2048 --lm-gutter 1
//              --lm-rays 1024
//              --sun-azimuth 30 --sun-elevation 55 --sun-intensity 10000
//              --lm-lod-lock attr|hard
//              --lm-denoise on|off (DEPRECATED, off by default)
//              --lm-denoise-quality high|balanced (DEPRECATED)
//              --lm-curve-target 7.0 --lm-curve-max 2.5
//              --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]
//
// --cubemap on bakes preconvolved SPECULAR cubemaps DURING the cook (same
// CPU path-tracer capture as tools/cubemap_baker, GGX importance-sampled
// mip chains via Engine/Renderer/cubemap_prefilter.h — roughness = mip /
// (levels-1)) and embeds them as Texture blobs at
// textures/cubemaps/<name>[_N].ktx2 (PakBackend serves them by VFS path,
// so CubemapLibrary::Load finds them with no code change). Probes and
// lightmaps carry all diffuse + low-frequency specular; these cubes own
// the high-frequency specular path only.
//
// --lightmap on bakes 2D SG lightmaps DURING the cook (sun + static spot
// NEE + analytic sky on open-sky rays, real materials, transmissive
// pass-through — see Engine/GI/bake.h), fills each mesh's texCoord2 with
// per-vertex chart-local UVs (+ chart ids), records the per-chart atlas
// transforms on the mesh for the shader unpack, stamps the mesh's atlas
// chunk id into the pak blob, and writes the atlas alongside the pak as
// <contentDir>/gi/<scene>.sglm (plus debug PNGs next to it:
// <scene>_atlas.png = the raw payload at the editor flat view's display
// mapping and <scene>_charts.png = the chart-id map, see lm_png.h). No
// bake-side work is left to the
// runtime: the pak + .sglm are the complete lightmap payload. Baked
// pixels take the lightmap ALONE at runtime (the analytic sky IBL is
// the fallback for uncovered pixels only — the ambient sum starts
// empty, no blind ambient added on covered pixels).
//
// LOD + lightmap interaction: with --lods on, the model is Frostbite-style:
// siblings cook FIRST, each mesh's LOD0 upload charts + bakes the WHOLE
// chain (one bake, one atlas level — the atlas stops growing per level),
// and every sibling level inherits the layout through UV transfer
// (TransferLightmapCharts: closest-point projection onto the LOD0
// surface). All LODs sample the same rects, so LOD switches never
// re-light. Either way meshlets are built once, after ALL stamping,
// from the final topology.
//
// Denoising (DEPRECATED, off by default): with --lm-denoise on, the
// fitted per-lobe amplitude layers are denoised with Intel Open Image
// Denoise's RTLightmap filter (prebuilt release in external/oidn, linked
// into PakCooker only — engine and App never see OIDN). Denoised
// payloads are written into the .sglm; the runtime path is unchanged.
// --lm-rays stays independent: denoising recovers low-sample bakes, so
// try e.g. --lm-rays 128 for a ~8x faster trace.
//
// Curvature-weighted density (default on): --lm-meters-per-texel is the
// BASE request; smoothly-bending meshes chart at up to --lm-curve-max
// times the base density so their normal-turn per texel stays under
// --lm-curve-target degrees (creases over 45 deg do not count — density
// cannot fix an authored edge). The atlas width is FIXED and the height
// grows with content (only a --lm-max-height overflow lowers the
// density), so curvature spends height, not a texel budget: same
// layout, spent where normals turn fast. Uniform density reads as
// faceted/hard-normal shading on curved geometry (columns/arches at
// ~18 deg per texel).
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
#include <cstdlib>
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

#include <volk.h>

#include "ktx.h"

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
#include <Engine/Renderer/cubemap.h>
#include <Engine/Renderer/cubemap_placement.h>
#include <Engine/Renderer/cubemap_prefilter.h>
#include <Engine/Utils/thread_pool.h>
#include <gltf_import/gltf.h>
#include <Engine/Vfs/vfs.h>

#include "lm_denoise.h"
#include "lm_png.h"
#include "cube_png.h"

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
	// Chart density request, METERS PER TEXEL (world space): 0.25 = one
	// texel per 25 cm (4 texels/m). The engine chart options speak
	// texels/m; the cook converts (1/mpt, floored at 2 texels/m).
	// --lm-density <texels/m> stays as a deprecated converting alias.
	float lmMetersPerTexel = 0.25f;
	uint32_t lmLobes = 12;       // 5|6|9|12 — PREFERRED max; the bake
	                             // selects the smallest count per mesh
	                             // whose fit residual fits the budget
	                             // (SgLightmapBakeConfig::lobeRelError)
	// FIXED atlas width (texels). The atlas never shrinks below or grows
	// past this width; content that does not fit the width lowers the
	// density. The HEIGHT grows with content (rows appended at the
	// bottom) up to --lm-max-height.
	uint32_t lmAtlasWidth = 2048;
// Atlas height cap (texels): past this the density convergence lowers
// the density (or drops meshes at the floor). 8192 keeps
// the 9 RGBA16F layers inside typical 2D texture limits; test cooks
// can raise it (GPU cost is width*height*72 B regardless).
	uint32_t lmMaxHeight = 8192;
	// Per-chart inner margin texels. Island-to-island padding is this +
	// the 2-texel pack gap + the neighbor's margin (4 total at 1); see
	// SgLightmapAtlas::PackRects. Large gutters are paid per CHART, and
	// ornamented scenes carry tens of thousands of tiny charts, so keep
	// this at 1 and let the packer provide the separation.
	uint32_t lmGutter = 1;
	// Atlas payload compression: "bc6hopt" (default, Frostbite-inspired:
	// BC6H with the invalid-texel block-average prefill so uncovered
	// zeros do not expand a block's endpoint box), "bc6h" (plain BC6H)
	// or "uncompressed" (9 RGBA16F layers, 72 B/texel). BC6H stores one
	// texture per lobe (8 B/texel per lobe) — ~6x smaller GPU footprint
	// at 12 lobes, block-compression artifacts on top of the bake.
	smvk::gi::SgLightmapCompression lmCompress =
		smvk::gi::SgLightmapCompression::Bc6HOpt;
	uint32_t lmRays = 1024;      // radiance samples per texel
	uint32_t lmBounces = 3;
	// Per-mesh lobe selection budget: relative RMS degradation from the
	// preferred-count reconstruction the mesh may cost by dropping
	// lobes. 0.15 keeps contrast-heavy meshes at 9-12 and lets smooth
	// ones drop lower; 0 pins everything at --lm-lobes.
	float lmLobeRelErr = 0.15f;
	// ---- LOD sibling cooking --------------------------------------------
	// Sibling simplify seam protection: "attr" = uv attributes as
	// simplifier error terms (default), "hard" = vertex-lock every seam
	// pair (less reduction). Siblings cook BEFORE the bake (frostbite:
	// LOD0 charts + bakes the whole chain, siblings UV-transfer).
	std::string lmLodLock = "attr";
	// ---- OIDN denoising (fitted atlas amplitudes, cook-time only) ------
	// Denoise the baked per-lobe amplitude layers with Intel Open Image
	// Denoise's RTLightmap filter (see lm_denoise.cpp). DEPRECATED: off
	// by default, kept functional for old scripts; the high-ray-count
	// bake stands on its own (the overlapping-rect guard rejects most
	// xatlas atlases anyway).
	bool lmDenoise = false;
	std::string lmDenoiseQuality = "high"; // high | balanced
	// Bright-pink fault map (<scene>_pink.png): chart-id layout with
	// empty/corrupt/overlapping chunk rects painted solid (255,0,255).
	// Off by default (extra full-atlas PNG write + O(n^2) overlap
	// pass on tens of thousands of chunks).
	bool lmDebugPink = false;
	// Cross-chart seam stitching (post-bake): boundary texels blend
	// toward same-normal sibling-chunk texels so duplicated seam
	// vertices sample one value on both sides; margins re-dilate after.
	// On by default (boundary-only, deterministic); off for A/B.
	bool lmStitch = true;
	// ---- Curvature-weighted density (banding fix on curved shapes) ----
	// Target normal-turn per texel on smoothly bending surfaces: meshes
	// bending faster than the target chart at up to --lm-curve-max times
	// the base density; the fixed-width atlas pays for it in HEIGHT (the
	// density search only runs when the height cap overflows).
	// Uniform density reads as faceted/hard-normal shading on curved
	// geometry (a column at ~20 texels/m turns ~18 deg per texel).
	// 0 disables.
	float lmCurveTarget = 7.0f;  // degrees of normal turn per texel
	float lmCurveMax = 2.5f;     // max per-mesh density multiplier
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
	// ---- Preconvolved specular cubemaps (--cubemap on) ----
	// Captures the scene with the CPU path tracer at one or more points
	// (same transport as CubemapBaker), GGX-prefilters each capture for
	// the HIGH-frequency specular path (probes/lightmaps already carry
	// all diffuse + low-frequency specular), and embeds the KTX2 cubes
	// as Texture blobs at textures/cubemaps/<name>.ktx2 — served by
	// PakBackend through the VFS, so CubemapLibrary::Load finds them
	// with no code change. Empty cubemapName defaults to the out-pak
	// stem (sponza.pak -> "sponza", mirroring gi/<scene>.sglm).
	bool cubemap = false;
	std::string cubemapName;
	uint32_t cubemapSize = 128;   // base face width (HDR RGBA32F)
	uint32_t cubemapRays = 128;   // GGX importance samples per output texel
	uint32_t cubemapBounces = 3;  // diffuse path depth after the first hit
	// Transport samples per capture texel: the NEE sun/spot term is
	// analytic (clean at 1 spp), but the bounce field is a Monte-Carlo
	// path estimate — 1 spp reads as speckle, not GI, and the capture
	// stops representing the (multi-ray, denoised) lightmapped scene.
	// 8 spp smooths the bounce term at ~8x capture cost.
	uint32_t cubemapSpp = 8;
	std::vector<std::string> cubemapPos; // repeatable "x,y,z" (empty =
	                                     // bounds center at cubemapHeight)
	float cubemapHeight = 1.5f;
	float cubemapGrid = 8.0f;     // grid spacing in meters (0 = single probe at center)
	// Sparse adaptive octree placement (Engine/Renderer/cubemap_placement):
	// occupancy-driven subdivision instead of a uniform lattice — open
	// volumes stay coarse, near-surface cells refine, embedded centers
	// drop, growth stops at the 32-probe runtime budget. Wins over --grid
	// when set (explicit --cubemap-pos still wins over both).
	bool cubemapOctree = false;
	float cubemapOctreeMax = 8.0f;   // open cells larger than this subdivide (coverage)
	float cubemapOctreeMin = 2.0f;   // detail floor: never subdivide below this
	uint32_t cubemapOctreeDepth = 4; // octree depth cap (root = 0)
	// Artist-pinned extra probes (repeatable "x,y,z"): appended AFTER
	// grid/octree placement and marked 'pin' in the manifest so the
	// runtime sorts them first — a pinned probe wins weight ties, e.g. a
	// capture exactly at the chrome ball. Unlike --cubemap-pos these never
	// replace the automatic placement, they add to it.
	std::vector<std::string> cubemapExtraPos;
	float cubemapExtraRadius = 1.5f; // pinned-probe influence (fade = half)
	// Debug PNGs next to the pak inputs (like the lightmap chart PNGs):
	// per-capture face strips (+ mip sheets) + a top-down probe map.
	// Loose files under <contentDir>/textures/cubemaps/, never in the pak.
	bool cubemapPng = true;
	bool cubemapPngMips = true;
	// NOTE: captures store TRUE radiance (physical units); the runtime's
	// baked path scales by the cubemap panel intensity only, matching the
	// direct sun view. No bake-time intensity knob by design.
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
		else if (a == "--lm-meters-per-texel") {
			std::string v;
			if (!need(v)) return false;
			out.lmMetersPerTexel = std::max(std::stof(v), 1e-4f);
			out.explicitSet.insert("lmMetersPerTexel");
		}
		else if (a == "--lm-density") {
			// Deprecated alias: value is old-semantics texels/meter.
			std::string v;
			if (!need(v)) return false;
			const float tpm = std::max(std::stof(v), 2.0f);
			out.lmMetersPerTexel = 1.0f / tpm;
			std::cerr << "warning: --lm-density (texels/m) is deprecated; "
				"use --lm-meters-per-texel (default 0.25 = 4 texels/m)\n";
			out.explicitSet.insert("lmMetersPerTexel");
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
		else if (a == "--lm-max-height") {
			std::string v;
			if (!need(v)) return false;
			out.lmMaxHeight = std::max<uint32_t>(
				static_cast<uint32_t>(std::stoul(v)), 256);
			out.explicitSet.insert("lmMaxHeight");
		}
		else if (a == "--lm-gutter") {
			std::string v;
			if (!need(v)) return false;
			out.lmGutter = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--lm-compress") {
			std::string v;
			if (!need(v)) return false;
			out.lmCompress = (v == "bc6h")
				? smvk::gi::SgLightmapCompression::Bc6H
				: (v == "uncompressed" || v == "none" || v == "off")
				? smvk::gi::SgLightmapCompression::Uncompressed
				: smvk::gi::SgLightmapCompression::Bc6HOpt;
			out.explicitSet.insert("lmCompress");
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
	else if (a == "--lm-denoise") {
		std::string v;
		if (!need(v)) return false;
		std::cerr << "warning: --lm-denoise is deprecated (off by "
			"default) and will be removed; the bake stands on its own\n";
		out.lmDenoise = (v == "on" || v == "1" || v == "true");
	}
	else if (a == "--lm-denoise-quality") {
		std::string v;
		if (!need(v)) return false;
		std::cerr << "warning: --lm-denoise-quality is deprecated "
			"(see --lm-denoise)\n";
		out.lmDenoiseQuality = (v == "balanced") ? "balanced" : "high";
	}
	else if (a == "--lm-debug-pink") {
		std::string v;
		if (!need(v)) return false;
		out.lmDebugPink = (v == "on" || v == "1" || v == "true");
	}
	else if (a == "--lm-stitch") {
		std::string v;
		if (!need(v)) return false;
		out.lmStitch = (v == "on" || v == "1" || v == "true");
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
		else if (a == "--cubemap") {
			std::string v;
			if (!need(v)) return false;
			out.cubemap = (v == "on" || v == "1" || v == "true");
			out.explicitSet.insert("cubemap");
		}
		else if (a == "--cubemap-name") {
			if (!need(out.cubemapName)) return false;
			out.explicitSet.insert("cubemapName");
		}
		else if (a == "--cubemap-size") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapSize = std::clamp<uint32_t>(
				static_cast<uint32_t>(std::stoul(v)), 4, 1024);
			out.explicitSet.insert("cubemapSize");
		}
		else if (a == "--cubemap-rays") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapRays = std::max<uint32_t>(
				static_cast<uint32_t>(std::stoul(v)), 1);
		}
		else if (a == "--cubemap-spp") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapSpp = std::clamp<uint32_t>(
				static_cast<uint32_t>(std::stoul(v)), 1, 64);
		}
		else if (a == "--cubemap-bounces") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapBounces = std::min<uint32_t>(
				static_cast<uint32_t>(std::stoul(v)), 8);
			out.explicitSet.insert("cubemapBounces");
		}
		else if (a == "--cubemap-pos") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapPos.push_back(v);
		}
		else if (a == "--cubemap-height") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapHeight = std::stof(v);
		}
		else if (a == "--cubemap-grid") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapGrid = std::stof(v);
		}
		else if (a == "--cubemap-octree") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapOctree = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--cubemap-octree-max") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapOctreeMax = std::max(0.5f, std::stof(v));
		}
		else if (a == "--cubemap-octree-min") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapOctreeMin = std::max(0.25f, std::stof(v));
		}
		else if (a == "--cubemap-octree-depth") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapOctreeDepth = std::clamp<uint32_t>(
				static_cast<uint32_t>(std::stoul(v)), 1, 6);
		}
		else if (a == "--cubemap-extra-pos") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapExtraPos.push_back(v);
		}
		else if (a == "--cubemap-extra-radius") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapExtraRadius = std::max(0.25f, std::stof(v));
		}
		else if (a == "--cubemap-png") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapPng = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--cubemap-png-mips") {
			std::string v;
			if (!need(v)) return false;
			out.cubemapPngMips = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--cubemap-ibl-intensity") {
			// Removed: captures store true radiance by design (the runtime
			// scales with the cubemap panel intensity). Reject explicitly
			// rather than silently ignoring a stale script flag.
			std::cerr << "--cubemap-ibl-intensity was removed (captures are"
				" true radiance; use the runtime cubemap intensity)\n";
			return false;
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
		<< "          [--lightmap on|off --lm-meters-per-texel 0.25 --lm-lobes 12\n"
		<< "           --lm-atlas-width 2048 --lm-max-height 8192\n"
		<< "           --lm-gutter 1 --lm-rays 1024\n"
		<< "           --lm-compress bc6hopt|bc6h|uncompressed\n"
		<< "           --lm-bounces 3\n"
		<< "           --sun-azimuth 30 --sun-elevation 55\n"
		<< "           --sun-intensity 10000 --sky-intensity 600\n"
		<< "           --lm-lod-lock attr|hard\n"
			<< "           --lm-denoise on|off (DEPRECATED)"
			<< " --lm-denoise-quality high|balanced (DEPRECATED)\n"
		<< "           --lm-debug-pink on|off\n"
		<< "           --lm-stitch on|off\n"
		<< "           --lm-curve-target 7.0 --lm-curve-max 2.5\n"
		<< "           --lm-exclude tree,linden,ivy,glass --lm-exclude-small 0.5\n"
		<< "           --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]\n"
		<< "          [--gi-probe <name> ...]: registers probe sidecar names\n"
		<< "          (gi/<name>.sggi, baked separately) into the pak; the\n"
		<< "          lightmap atlas registers itself on --lightmap. Runtime\n"
		<< "          resolves GI by these names (one per kind for now).\n"
		<< "          [--cubemap on|off --cubemap-name <name>\n"
		<< "           --cubemap-size 128 --cubemap-rays 128 --cubemap-bounces 3\n"
		<< "           --cubemap-pos x,y,z (repeatable) --cubemap-height 1.5\n"
		<< "           --cubemap-grid 8.0 --cubemap-octree on\n"
		<< "           --cubemap-octree-max 8.0 --cubemap-octree-min 2.0\n"
		<< "           --cubemap-octree-depth 4\n"
		<< "           --cubemap-extra-pos x,y,z (repeatable, pinned)\n"
		<< "           --cubemap-extra-radius 1.5\n"
		<< "           --cubemap-spp 8\n"
		<< "           --cubemap-png on|off --cubemap-png-mips on|off]:\n"
		<< "          preconvolved SPECULAR cubemaps baked into the pak\n"
		<< "          (GGX importance-sampled mip chains, roughness = mip /\n"
		<< "          (levels-1); probes/lightmaps own diffuse + low-freq\n"
		<< "          specular). Captures store true radiance (the runtime\n"
		<< "          scales them with the cubemap panel intensity, so a\n"
		<< "          mirror matches the sun-lit scene; no bake-time\n"
		<< "          intensity is frozen in). One capture per\n"
		<< "          --cubemap-pos (default:\n"
		<< "          scene-bounds center at +height). If --cubemap-grid > 0,\n"
		<< "          captures a uniform 3D grid of probes at that spacing\n"
		<< "          (meters) within the scene bounds; --cubemap-octree on\n"
		<< "          replaces the lattice with sparse adaptive placement\n"
		<< "          (open volumes stay coarse, near-surface cells refine\n"
		<< "          to --cubemap-octree-min at depth --cubemap-octree-depth,\n"
		<< "          embedded centers drop, growth stops at 32 probes).\n"
		<< "          --cubemap-extra-pos appends artist-PINNED probes after\n"
		<< "          placement (manifest 'pin' rows sort first at load, so\n"
		<< "          they win weight ties — e.g. a capture at the chrome\n"
		<< "          ball; influence --cubemap-extra-radius, fade half).\n"
		<< "          --cubemap-spp averages N transport paths per texel so\n"
		<< "          the bounce field is coherent GI (1 spp = speckle),\n"
		<< "          matching the multi-ray lightmap look.\n"
		<< "          Multi captures write textures/cubemaps/<name>.cubemanifest\n"
		<< "          (positions + radii) next to the KTX blobs; embedded as\n"
		<< "          Texture blobs at\n"
		<< "          textures/cubemaps/<name>[_N].ktx2 (name defaults to the\n"
		<< "          out-pak stem). Debug PNGs (<name>_N.png faces,\n"
		<< "          <name>_N_mips.png prefilter stack, <name>_probes.png\n"
		<< "          top-down placement map) land loose in\n"
		<< "          <contentDir>/textures/cubemaps/. Needs exactly one\n"
		<< "          cooked scene, like --lightmap.\n"
		<< "--lightmap on: bakes 2D SG lightmaps into the pak (per-vertex\n"
		<< "lightmap UVs + per-mesh chunk id; atlas written to\n"
		<< "<contentDir>/gi/<scene>.sglm). Sources: sun + static spots + real\n"
		<< "materials; transmissive (Blend) meshes pass light through.\n"
		<< "--lm-meters-per-texel is a WORLD-space request (m/texel; 0.25 =\n"
		<< "one texel per 25 cm = 4 texels/m; scaled nodes like sponza's\n"
		<< "0.008 root convert through their world scale. --lm-density\n"
		<< "<texels/m> stays as a deprecated converting alias). The atlas\n"
		<< "WIDTH is fixed at --lm-atlas-width; the HEIGHT grows with the\n"
		<< "content (rows appended at the bottom) up to --lm-max-height —\n"
		<< "only a height-cap overflow (or a chart wider than the atlas)\n"
		<< "triggers the density search, which then halves until the pack\n"
		<< "fits and logs the converged texels/m + texels/m^2.\n"
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
		// Same sanity bounds as the runtime reader (DeserializeCluster):
		// a stale (pre-lightmap-transform) or corrupt blob misaligns
		// every field below into vertex payload bytes. Without this the
		// uv-sweep audit walks garbage indices OOB and AVs instead of
		// reporting the blob.
		if (!c.ok || vc == 0 || vc > 16 * 1024 * 1024 || ic > 64 * 1024 * 1024 || sc > 4096) {
			std::cout << "    [truncated mesh blob: " << e->path << "]\n";
			continue;
		}
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
		const uint32_t lmXforms = c.u32();
		std::vector<float> lmXf;
		if (c.ok && lmXforms <= 1024 * 1024
			&& c.off + (size_t)lmXforms * 16 <= c.img.size()) {
			lmXf.resize((size_t)lmXforms * 4);
			memcpy(lmXf.data(), c.img.data() + c.off,
				(size_t)lmXforms * 16);
			c.off += (size_t)lmXforms * 16;
		}
		else {
			// Stale (pre-lightmap-transform) or corrupt blob: the count
			// field is vertex payload, not a count. Mark truncated so the
			// audit below is skipped instead of reading OOB (AV).
			c.ok = false;
		}
		if (!c.ok) {
			std::cout << "    [truncated mesh blob: " << e->path << "]\n";
			continue;
		}
		// ---- atlas-uv sweep audit (lightmap seam detector) ----
		// StaticVertex (AoS): pos 12 | color 16 | texCoord 8 |
		// texCoord2 (CHART-LOCAL uv) at byte 36 | normal 12 | tangent 16
		// | chartId at byte 72. Atlas uvs resolve on the fly as
		// chartUv * xform.scale + xform.offset (same math as the VS
		// unpack). A triangle spanning distant atlas regions flags a
		// stamping bug; within one chart rect a triangle spans at most
		// 256/atlasW + 256/atlasH (charts are capped at 256x256 texels;
		// tall/short atlases make that aspect-dependent, e.g. 2047x871
		// -> 0.42), so 0.45 flags only genuine cross-rect sweeps.
		float maxUv2Span = 0.0f;
		uint32_t bigUv2Tris = 0;
		const size_t vertBase = c.off;
		const size_t idxBase = vertBase + (size_t)vc * sizeof(smvk::StaticVertex);
		// Vertex-attribute probe shared by the classic-index walk and
		// the meshlet walk below: EVERY attribute must come from the
		// SAME final vertex (the meshlet audit's whole point).
		auto atlasAt = [&](size_t v) -> std::pair<float, float> {
			const uint8_t* p = c.img.data() + vertBase
				+ v * sizeof(smvk::StaticVertex);
			float cu, cv;
			uint32_t cid;
			memcpy(&cu, p + 36, 4);
			memcpy(&cv, p + 40, 4);
			memcpy(&cid, p + 72, 4);
			if (cid != 0xFFFFFFFFu && (size_t)cid < lmXf.size() / 4) {
				const float* xf = lmXf.data() + (size_t)cid * 4;
				return { cu * xf[0] + xf[2], cv * xf[1] + xf[3] };
			}
			return { cu, cv };
		};
		// Canonical (sorted) triangle key for set comparison.
		auto triKey = [](uint32_t a, uint32_t b, uint32_t d) {
			uint32_t mn = std::min({ a, b, d });
			uint32_t mx = std::max({ a, b, d });
			uint32_t mid = a + b + d - mn - mx;
			return uint64_t(mn) * 4294967296ull + uint64_t(mid) * 65536ull
				+ mx;
		};
		std::unordered_set<uint64_t> classicTris;
		// ---- chart-id slice audit (stale-id detector) ----
		// The runtime resolves xforms[chartOffset + chartId] with NO
		// bounds check (gfxfilter.hlsli): a vertex whose chartId is
		// neither 0xFFFFFFFF nor inside its own mesh's xform slice
		// samples another mesh's transform (or OOB garbage) — isolated
		// triangles in wrong rects + black regions, positions perfect.
		// The atlasAt probe above bounds-checks and silently falls
		// back, so this fault is invisible to the sweep/density
		// audits: count it explicitly. chartidmax = max non-FFFFFFFF
		// chartId over ALL vertices; oobslice = triangle corners with
		// a stamped-looking id past the mesh's own xform count.
		uint32_t chartIdMax = 0;
		bool haveChartId = false;
		uint32_t oobSlice = 0;
		if (vertBase + (size_t)vc * sizeof(smvk::StaticVertex)
			<= c.img.size()) {
			for (uint32_t v = 0; v < vc; ++v) {
				uint32_t cid;
				memcpy(&cid, c.img.data() + vertBase
					+ v * sizeof(smvk::StaticVertex) + 72, 4);
				if (cid != 0xFFFFFFFFu) {
					haveChartId = true;
					chartIdMax = std::max(chartIdMax, cid);
				}
			}
		}
		if (lmChunk != 0xFFFFFFFFu && idxBase + (size_t)ic * 4 <= c.img.size()) {
			for (uint32_t t = 0; t * 3 + 3 <= ic; ++t) {
				uint32_t ix[3] = {};
				memcpy(ix, c.img.data() + idxBase + (size_t)t * 12, 12);
				for (int k = 0; k < 3; ++k) {
					if (ix[k] >= vc)
						continue;
					uint32_t cid;
					memcpy(&cid, c.img.data() + vertBase
						+ ix[k] * sizeof(smvk::StaticVertex) + 72, 4);
					if (cid != 0xFFFFFFFFu
						&& (size_t)cid >= lmXf.size() / 4)
						++oobSlice;
				}
			}
		}
		// ---- texel-distance uniformity audit ----
		// Per triangle: atlas-uv area / world area (geometric-mean
		// scale, dimensionless per meter^2). Uniform charting gives one
		// scale for every triangle of the scene; sparse charts whose
		// footprint-shrink compresses distant geometry into a few
		// texels, and clamped seam corners, show up as outliers. Each
		// outlier triangle is attributed to its chart + whether any
		// corner sits on the chart-local [0,1] clamp boundary (the
		// shrink-rebuild clamp) so the mechanism reads directly:
		// clamped outliers = footprint compression, non-clamped = a
		// genuinely different-density chart (sliver inner region).
		struct DensTri {
			uint32_t chart;
			float scale;
			bool clamped;
		};
		std::vector<DensTri> densTris;
		uint32_t densDegenerateUv = 0, densClampVerts = 0;
		std::unordered_map<uint32_t, uint32_t> chartOut, chartDeg;
		if (lmChunk != 0xFFFFFFFFu && idxBase + (size_t)ic * 4 <= c.img.size()) {
			auto posAt = [&](size_t v) {
				float p[3];
				memcpy(p, c.img.data() + vertBase
					+ v * sizeof(smvk::StaticVertex), 12);
				return glm::vec3(p[0], p[1], p[2]);
			};
			auto chartLocalAt = [&](size_t v) {
				const uint8_t* p = c.img.data() + vertBase
					+ v * sizeof(smvk::StaticVertex);
				float cu, cv;
				uint32_t cid;
				memcpy(&cu, p + 36, 4);
				memcpy(&cv, p + 40, 4);
				memcpy(&cid, p + 72, 4);
				return std::tuple<float, float, uint32_t>(cu, cv, cid);
			};
			for (uint32_t t = 0; t * 3 + 3 <= ic; ++t) {
				uint32_t ix[3] = {};
				memcpy(ix, c.img.data() + idxBase + (size_t)t * 12, 12);
				const glm::vec3 p0 = posAt(ix[0]);
				const glm::vec3 p1 = posAt(ix[1]);
				const glm::vec3 p2 = posAt(ix[2]);
				const float worldA = 0.5f * glm::length(
					glm::cross(p1 - p0, p2 - p0));
				uint32_t chart = 0xFFFFFFFFu;
				bool clamped = false;
				for (int k = 0; k < 3; ++k) {
					const auto [cu, cv, cid] = chartLocalAt(ix[k]);
					if (cid != 0xFFFFFFFFu && chart == 0xFFFFFFFFu)
						chart = cid;
					if (cu <= 1e-4f || cu >= 1.0f - 1e-4f
						|| cv <= 1e-4f || cv >= 1.0f - 1e-4f) {
						clamped = true;
						++densClampVerts;
					}
				}
				const auto a0 = atlasAt(ix[0]);
				const auto a1 = atlasAt(ix[1]);
				const auto a2 = atlasAt(ix[2]);
				const float uvA = 0.5f * std::abs(
					(a1.first - a0.first) * (a2.second - a0.second)
					- (a2.first - a0.first) * (a1.second - a0.second));
				if (worldA < 1e-12f)
					continue;
				if (uvA <= 0.0f) {
					++densDegenerateUv; // fully-clamped triangle:
					                    // zero atlas area on real
					                    // geometry
					if (chart != 0xFFFFFFFFu)
						++chartDeg[chart];
					continue;
				}
				densTris.push_back({ chart,
					std::sqrt(uvA / worldA), clamped });
			}
		}
		float densMed = 0.0f, densP90 = 0.0f, densMax = 0.0f;
		uint32_t densOut = 0, densOutClamped = 0;
		uint32_t worstChart = 0xFFFFFFFFu, worstChartOut = 0;
		if (!densTris.empty()) {
			std::vector<float> densScales;
			densScales.reserve(densTris.size());
			for (const DensTri& d : densTris)
				densScales.push_back(d.scale);
			std::sort(densScales.begin(), densScales.end());
			densMed = densScales[densScales.size() / 2];
			densP90 = densScales[(densScales.size() * 9) / 10];
			densMax = densScales.back();
			std::unordered_map<uint32_t, uint32_t> chartOutAll;
			for (const DensTri& d : densTris) {
				if (d.scale > densMed * 8.0f || d.scale < densMed / 8.0f) {
					++densOut;
					if (d.clamped)
						++densOutClamped;
					if (d.chart != 0xFFFFFFFFu
						&& ++chartOutAll[d.chart] > worstChartOut) {
						worstChartOut = chartOutAll[d.chart];
						worstChart = d.chart;
					}
				}
			}
		}
		if (lmChunk != 0xFFFFFFFFu && idxBase + (size_t)ic * 4 <= c.img.size()) {
			for (uint32_t t = 0; t * 3 + 3 <= ic; ++t) {
				uint32_t ix[3] = {};
				memcpy(ix, c.img.data() + idxBase + (size_t)t * 12, 12);
				const auto a = atlasAt(ix[0]);
				const auto b = atlasAt(ix[1]);
				const auto d = atlasAt(ix[2]);
				const float duv0 = std::abs(a.first - b.first) + std::abs(a.second - b.second);
				const float duv1 = std::abs(a.first - d.first) + std::abs(a.second - d.second);
				const float duv2 = std::abs(b.first - d.first) + std::abs(b.second - d.second);
				const float span = std::max({ duv0, duv1, duv2 });
				if (span > maxUv2Span)
					maxUv2Span = span;
				if (span > 0.45f)
					++bigUv2Tris;
				classicTris.insert(triKey(ix[0], ix[1], ix[2]));
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
		// ---- meshlet-walk audit: localIndex -> window -> global ----
		// Reproduces the runtime indirection exactly (gfx_filter
		// AppendMeshlet / BuildMeshletIndirectCommands):
		//   local   = triangles[triangleOffset + k]
		//   global  = vertices[vertexOffset + local]
		// then EVERY attribute (uv1/chartId here) must come from that
		// same global vertex. Verifies the serialized trailer against
		// the vertex payload it ships with:
		//   mlbadlocal: meshlet-local id >= vertexCount (corrupt tri)
		//   mloob:      window id >= vertex count (STALE windows
		//               referencing a pre-seam-dup vertex layout — the
		//               "meshlets built before lightmap split" bug)
		//   mlbigtris:  atlas-uv2 sweep over the meshlet topology
		//               (>0.45 = a meshlet triangle sampling a distant
		//               atlas region)
		//   mlsetdiff:  symmetric triangle-set difference vs the
		//               classic index buffer (healthy: only degenerate
		//               classic tris dropped by the builder)
		uint32_t mlBadLocal = 0, mlOob = 0, mlBigTris = 0,
			mlSetDiff = 0;
		bool mlTruncated = false;
		if (meshlets > 0) {
			// Trailer layout mirrors the serializer: meshlet records
			// (4 u32 + bound vec3+f + cone vec3+vec3+f = 60 B), then
			// the u32 window, then the u8 triangle bytes.
			struct MlRec {
				uint32_t vertexOffset, vertexCount, triangleOffset,
					triangleCount;
			};
			std::vector<MlRec> recs(meshlets);
			for (uint32_t m = 0; m < meshlets && c.ok; ++m) {
				recs[m].vertexOffset = c.u32();
				recs[m].vertexCount = c.u32();
				recs[m].triangleOffset = c.u32();
				recs[m].triangleCount = c.u32();
				for (int f = 0; f < 3; ++f) (void)c.f32(); // center
				(void)c.f32();                            // radius
				for (int f = 0; f < 3; ++f) (void)c.f32(); // apex
				for (int f = 0; f < 3; ++f) (void)c.f32(); // axis
				(void)c.f32();                            // cutoff
			}
			std::vector<uint32_t> window;
			std::vector<uint8_t> mlTris;
			if (c.ok) {
				const uint32_t wc = c.u32();
				if (c.ok && wc <= 64 * 1024 * 1024
					&& c.off + (size_t)wc * 4 <= c.img.size()) {
					window.resize(wc);
					if (wc)
						memcpy(window.data(), c.img.data() + c.off,
							(size_t)wc * 4);
					c.off += (size_t)wc * 4;
				} else {
					c.ok = false;
				}
			}
			if (c.ok) {
				const uint32_t tc = c.u32();
				if (c.ok && tc <= 64 * 1024 * 1024
					&& c.off + tc <= c.img.size()) {
					mlTris.resize(tc);
					if (tc)
						memcpy(mlTris.data(), c.img.data() + c.off, tc);
					c.off += tc;
				} else {
					c.ok = false;
				}
			}
			if (!c.ok) {
				mlTruncated = true;
			} else {
				std::unordered_set<uint64_t> mlTriSet;
				for (const MlRec& m : recs) {
					for (uint32_t t = 0; t < m.triangleCount; ++t) {
						const size_t base =
							(size_t)m.triangleOffset + (size_t)t * 3;
						if (base + 3 > mlTris.size()) {
							++mlBadLocal;
							continue;
						}
						uint32_t g[3];
						bool bad = false;
						for (int k = 0; k < 3; ++k) {
							const uint32_t local = mlTris[base + k];
							if (local >= m.vertexCount) {
								bad = true;
								break;
							}
							const size_t w =
								(size_t)m.vertexOffset + local;
							if (w >= window.size()) {
								bad = true;
								break;
							}
							g[k] = window[w];
							if (g[k] >= vc) {
								bad = true;
								++mlOob; // window id past the vertex
								         // payload: stale windows
								break;
							}
						}
						if (bad) {
							++mlBadLocal;
							continue;
						}
						mlTriSet.insert(triKey(g[0], g[1], g[2]));
						if (lmChunk != 0xFFFFFFFFu) {
							const auto a = atlasAt(g[0]);
							const auto b = atlasAt(g[1]);
							const auto d = atlasAt(g[2]);
							const float duv0 = std::abs(a.first - b.first)
								+ std::abs(a.second - b.second);
							const float duv1 = std::abs(a.first - d.first)
								+ std::abs(a.second - d.second);
							const float duv2 = std::abs(b.first - d.first)
								+ std::abs(b.second - d.second);
							if (std::max({ duv0, duv1, duv2 }) > 0.45f)
								++mlBigTris;
						}
					}
				}
				// Symmetric set difference: classic-tris missing from
				// the meshlet walk + meshlet tris absent classically.
				if (lmChunk != 0xFFFFFFFFu) {
					for (uint64_t key : classicTris)
						if (mlTriSet.count(key) == 0)
							++mlSetDiff;
					for (uint64_t key : mlTriSet)
						if (classicTris.count(key) == 0)
							++mlSetDiff;
				}
			}
		}
		if (mlTruncated) {
			std::cout << "    [truncated meshlet trailer: " << e->path
				<< "]\n";
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
			std::cout << " charts=" << lmXforms
				<< " uv2span=" << maxUv2Span
				<< " bigtris=" << bigUv2Tris
				<< " densmed=" << densMed
				<< " densp90=" << densP90
				<< " densmax=" << densMax
				<< " densout=" << densOut
				<< " outclamp=" << densOutClamped
				<< " degenuv=" << densDegenerateUv
				<< " worstchart=" << (worstChart == 0xFFFFFFFFu
					? -1 : int(worstChart)) << "/" << worstChartOut
				<< " chartidmax=" << (haveChartId ? int(chartIdMax) : -1)
				<< " oobslice=" << oobSlice;
		if (meshlets > 0)
			std::cout << " mlbigtris=" << mlBigTris
				<< " mlsetdiff=" << mlSetDiff
				<< " mlbadlocal=" << mlBadLocal
				<< " mloob=" << mlOob;
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

// Per-corner chart-uv stamping (seam-correct), shared by every stamp
// (chart source + UV-transfer levels): a vertex shared by two charts
// has no single valid uv, so every triangle corner is stamped through
// the TRIANGLE's chart — original vertices keep their slots 1:1 (first
// chart to use a vertex owns its slot), corners needed by a different
// chart get positional duplicates appended, and the index buffer is
// rebuilt in the same triangle order (surface ranges stay valid). Fills
// outChartOf (per-vertex chart over the STAMPED layout — the
// simplifier's error-metric provenance) and outXforms (per-chart atlas
// transforms for the shader unpack), and counts seam duplicates and
// uncharted (dropped/degenerate, inherited-chart) triangles.
// texelSpace=true computes the transforms in ATLAS-TEXEL coords
// (AtlasTexel): the xatlas path divides exactly these meshes' transforms
// once by the final dimensions in
// Stamps CHART-LOCAL uvs + chart ids, and returns the per-chart
// atlas-space transforms (one float4 per chart: xy = scale, zw = offset,
// so atlasUv = chartUv * xy + zw). The vertex stream no longer carries
// atlas-space uv2 — the VS unpacks it via the draw's chart offset (see
// GfxDraw.misc2.x). texelSpace=true computes the transforms in
// ATLAS-TEXEL coords (AtlasTexel): the normalize pass below divides them
// by the final dims (vertices are never touched). false computes
// normalized transforms directly (AtlasUv, planar path). outXforms is
// parallel to the chart indices (chart i -> outXforms[i]). Returns the
// mesh's lobe count (first chunk, written by the bake's per-mesh
// selection; fallbackLobes when the mesh has no chunks).
//
// Cross-rect streaks are structurally impossible in this scheme: every
// triangle's corners land in ONE chart (seam corners are duplicated per
// chart) and reach the atlas through that chart's single affine map —
// so the old sweep-repair pass is gone.
uint32_t StampLightmapUvs(smvk::StaticMeshUpload& upload,
	const smvk::gi::SgLightmapChartResult& chartRes, uint32_t chunkBase,
	const smvk::gi::SgLightmapAtlas& atlas, uint32_t gutter,
	uint32_t fallbackLobes, std::vector<uint32_t>& outChartOf,
	std::vector<glm::vec4>& outXforms,
	uint64_t& seamDups, uint32_t& unchartedTris) {
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
	unchartedTris = 0;
	for (size_t t = 0; t < triCount; ++t) {
		const uint32_t chartIdx = t < triChart.size()
			? triChart[t] : 0xFFFFFFFFu;
		if (chartIdx == 0xFFFFFFFFu)
			++unchartedTris;
		for (int k = 0; k < 3; ++k) {
			const uint32_t orig = upload.indices[t * 3 + k];
			if (chartIdx == 0xFFFFFFFFu) {
				// Degenerate/uncharted triangle: keep the original
				// corner (chart uv/id stay whatever the vertex carries).
				newIndices.push_back(orig);
				continue;
			}
			const uint64_t key = uint64_t(orig) << 32 | chartIdx;
			auto it = cornerSlot.find(key);
			if (it != cornerSlot.end()) {
				newIndices.push_back(it->second);
				continue;
			}
			const glm::vec2 chartLocalUv = cornerUv[t * 3 + k];
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
				dup.texCoord2 = chartLocalUv;
				dup.chartId = chartIdx;
				upload.vertices.push_back(dup);
				slot = uint32_t(upload.vertices.size() - 1);
				dupChart.push_back(chartIdx);
				++seamDups;
			}
			upload.vertices[slot].texCoord2 = chartLocalUv;
			upload.vertices[slot].chartId = chartIdx;
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
	// Per-chart atlas transforms, decomposed exactly from the atlas map
	// (affine by construction: offset = map(0,0), scale = map(1,1)-offset).
	// ATLAS-TEXEL space: the normalize pass below divides every stamped
	// mesh's transforms once by the final atlas dimensions.
	outXforms.assign(chartRes.charts.size(), glm::vec4(0.0f));
	for (size_t ci = 0; ci < chartRes.charts.size(); ++ci) {
		const uint32_t chunk = chunkBase + uint32_t(ci);
		const glm::vec2 o =
			atlas.AtlasTexel(chunk, gutter, glm::vec2(0.0f, 0.0f));
		const glm::vec2 e =
			atlas.AtlasTexel(chunk, gutter, glm::vec2(1.0f, 1.0f));
		outXforms[ci] = glm::vec4(e.x - o.x, e.y - o.y, o.x, o.y);
	}
	if (!chartRes.charts.empty() && chunkBase < atlas.chunks.size())
		return atlas.chunks[chunkBase].lobeCount;
	return fallbackLobes;
}

// ---- Preconvolved specular cubemaps (--cubemap on) ------------------------
// KTX2 encode of one preconvolved chain into memory bytes (same layout the
// CubemapBaker writes to disk: RGBA32F cube, full mip chain, linear
// transfer — the runtime must sample it without an sRGB decode).
bool WriteCubemapKtx2ToMemory(const smvk::CubemapMipChains& chains,
	uint32_t w, uint32_t h, uint32_t levels, std::vector<uint8_t>& out,
	std::string& err) {
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
	ci.generateMipmaps = KTX_FALSE; // mips are pre-baked into the payload

	ktxTexture2* tex = nullptr;
	KTX_error_code res = ktxTexture2_Create(
		&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex);
	if (res != KTX_SUCCESS || !tex) {
		err = std::string("ktxTexture2_Create: ") + ktxErrorString(res);
		return false;
	}
	struct Guard {
		ktxTexture2* t;
		~Guard() { if (t) ktxTexture2_Destroy(t); }
	} guard{ tex };

	res = ktxTexture2_SetOETF(tex, KHR_DF_TRANSFER_LINEAR);
	if (res != KTX_SUCCESS) {
		err = std::string("ktxTexture2_SetOETF: ") + ktxErrorString(res);
		return false;
	}
	for (uint32_t level = 0; level < levels; ++level) {
		for (uint32_t face = 0; face < smvk::kCubemapFaceCount; ++face) {
			const auto* src = reinterpret_cast<const ktx_uint8_t*>(
				chains[face][level].data());
			const ktx_size_t srcLen = static_cast<ktx_size_t>(
				chains[face][level].size() * sizeof(float));
			res = ktxTexture_SetImageFromMemory(ktxTexture(tex),
				level, 0, face, src, srcLen);
			if (res != KTX_SUCCESS) {
				err = std::string("ktxTexture_SetImageFromMemory: ")
					+ ktxErrorString(res);
				return false;
			}
		}
	}
	ktx_uint8_t* mem = nullptr;
	ktx_size_t memLen = 0;
	res = ktxTexture_WriteToMemory(ktxTexture(tex), &mem, &memLen);
	if (res != KTX_SUCCESS || !mem || !memLen) {
		err = std::string("ktxTexture_WriteToMemory: ")
			+ ktxErrorString(res);
		return false;
	}
	out.assign(mem, mem + memLen);
	std::free(mem);
	return true;
}

// Bakes one GGX-preconvolved specular capture per position (same CPU
// path-tracer transport as tools/cubemap_baker: TraceIncoming on hits,
// analytic sky on primary misses, textured albedo) and returns
// (vfsPath, ktxBytes) pairs for pak embedding. False on error (logged).
bool BakePreconvolvedCubemaps(smvk::Cluster& cluster, Args& args,
	std::vector<std::pair<std::string, std::vector<uint8_t>>>& out) {
	using namespace smvk::gi;
	const auto t0 = std::chrono::steady_clock::now();

	// Texture averages so captures see textured albedo (what the skybox
	// shows), mirroring the lightmap block.
	GiSceneBuildOptions buildOpts{};
	{
		uint32_t averaged = 0;
		for (const auto& [uri, handle] : cluster.ListTextureUris()) {
			const std::vector<uint8_t>* src = cluster.GetTextureSource(uri);
			if (!src || src->empty())
				continue;
			std::optional<glm::vec3> avg =
				GiAverageKtxTexel(std::span<const uint8_t>(*src));
			if (!avg)
				continue;
			buildOpts.texelAvg[handle.index()] = *avg;
			++averaged;
		}
		std::cerr << "  [CUBE] textures averaged: " << averaged << " / "
			<< cluster.ListTextureUris().size() << "\n" << std::flush;
	}
	// Same import basis change as CubemapBaker/GiBaker: cook imports at
	// coordFix 0 read punctual aims with the conjugated -Z axis.
	buildOpts.punctualAimAxis = GltfPunctualAimAxis(0);
	buildOpts.env.sunAzimuthDeg = args.sunAzimuth;
	buildOpts.env.sunElevationDeg = args.sunElevation;
	buildOpts.env.sunIntensity = args.sunIntensity;
	buildOpts.env.skyIntensity = args.skyIntensity;
	// uniformAlbedo stays off: captures show real materials.

	GiTraceScene traceScene;
	std::string terr;
	// buildOpts.staticLights (default on) harvests the scene's
	// STATIC-flagged cooked lights into traceScene.spots/sun here.
	BuildTraceSceneFromCluster(cluster, buildOpts, traceScene, terr);
	if (!terr.empty()) {
		std::cerr << "[CUBE] trace scene failed: " << terr << std::endl;
		return false;
	}
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
			return false;
		}
		spot.color = col * candela;
		spot.cosInner = std::cos(glm::radians(std::min(innerDeg, outerDeg)));
		spot.cosOuter = std::cos(glm::radians(std::max(innerDeg, outerDeg)));
		traceScene.spots.push_back(spot);
	}
	std::cerr << "  [CUBE] trace scene: " << traceScene.Bvh().TriangleCount()
		<< " tris, " << traceScene.materials.size() << " materials, "
		<< traceScene.spots.size() << " spots\n" << std::flush;
	// Albedo audit: black captures almost always mean black trace albedo
	// (every bounce dies on arrival). Print the first materials + the dark
	// count so a broken average/import shows up here, not in the PNGs.
	{
		size_t darkMats = 0;
		for (size_t mi = 0; mi < traceScene.materials.size(); ++mi) {
			const auto& m = traceScene.materials[mi];
			const float luma = 0.2126f * m.albedo.x + 0.7152f * m.albedo.y
				+ 0.0722f * m.albedo.z;
			if (luma < 1e-4f)
				++darkMats;
			if (mi < 10) {
				std::cerr << "  [CUBE] mat[" << mi << "] albedo=("
					<< m.albedo.x << "," << m.albedo.y << "," << m.albedo.z
					<< ") emissive=(" << m.emissive.x << "," << m.emissive.y
					<< "," << m.emissive.z << ")\n";
			}
		}
		std::cerr << "  [CUBE] materials dark(<1e-4): " << darkMats << "/"
			<< traceScene.materials.size() << std::flush;
		std::vector<std::pair<uint32_t, glm::vec3>> avgs(
			buildOpts.texelAvg.begin(), buildOpts.texelAvg.end());
		std::sort(avgs.begin(), avgs.end(),
			[](const auto& a, const auto& b) { return a.first < b.first; });
		std::cerr << " texelAvg=" << avgs.size() << "\n" << std::flush;
		for (size_t i = 0; i < std::min<size_t>(avgs.size(), 5); ++i) {
			std::cerr << "  [CUBE] texelAvg[" << avgs[i].first << "]=("
				<< avgs[i].second.x << "," << avgs[i].second.y << ","
				<< avgs[i].second.z << ")\n" << std::flush;
		}
	}
	if (traceScene.Bvh().TriangleCount() == 0) {
		std::cerr << "[CUBE] no trace geometry (empty scene)" << std::endl;
		return false;
	}

	// Capture positions: explicit --cubemap-pos list, else the
	// scene-bounds center raised by --cubemap-height (CubemapBaker rule).
	// If --cubemap-octree is on, sparse adaptive placement wins over the
	// uniform --cubemap-grid lattice (see cubemap_placement.h).
	std::vector<glm::vec3> positions;
	std::vector<glm::vec2> radii; // parallel to positions: x = influence, y = fade
	for (const std::string& spec : args.cubemapPos) {
		glm::vec3 p{};
		if (std::sscanf(spec.c_str(), "%f,%f,%f", &p.x, &p.y, &p.z) != 3) {
			std::cerr << "Bad --cubemap-pos (want x,y,z): " << spec << "\n";
			return false;
		}
		positions.push_back(p);
		radii.emplace_back(10.0f, 5.0f);
	}
	if (positions.empty()) {
		glm::vec3 bmin(1e30f), bmax(-1e30f);
		for (uint32_t k = 0; k < traceScene.Bvh().TriangleCount(); ++k) {
			glm::vec3 v0, v1, v2;
			traceScene.Bvh().TriangleVertices(k, v0, v1, v2);
			bmin = glm::min(bmin, glm::min(v0, glm::min(v1, v2)));
			bmax = glm::max(bmax, glm::max(v0, glm::max(v1, v2)));
		}
		
		if (args.cubemapOctree) {
			// Sparse adaptive octree: coverage-bounded open cells +
			// surface-near refinement, embedded centers dropped, growth
			// capped at the runtime probe budget (cubemap_placement.h).
			smvk::CubemapOctreeOptions oo{};
			oo.maxCellSize = args.cubemapOctreeMax;
			oo.minCellSize = args.cubemapOctreeMin;
			oo.maxDepth = args.cubemapOctreeDepth;
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
			std::cerr << olog << std::flush;
			positions.reserve(placed.size());
			radii.reserve(placed.size());
			for (const auto& p : placed) {
				positions.push_back(p.position);
				radii.emplace_back(p.influenceRadius, p.fadeRadius);
			}
		}
		else if (args.cubemapGrid > 0.0f) {
			// Generate 3D grid of probe positions
			const float spacing = args.cubemapGrid;
			const float yBase = bmin.y + args.cubemapHeight;

			int nx = std::max(1, int(std::ceil((bmax.x - bmin.x) / spacing)));
			int ny = std::max(1, int(std::ceil((bmax.y - bmin.y) / spacing)));
			int nz = std::max(1, int(std::ceil((bmax.z - bmin.z) / spacing)));

			std::cerr << "  [CUBE] generating " << nx << "x" << ny << "x" << nz
				<< " grid at " << spacing << "m spacing = " << (nx * ny * nz) << " probes\n";

			for (int iz = 0; iz < nz; ++iz) {
				for (int iy = 0; iy < ny; ++iy) {
					for (int ix = 0; ix < nx; ++ix) {
						float x = bmin.x + (ix + 0.5f) * spacing;
						float y = yBase + iy * spacing;
						float z = bmin.z + (iz + 0.5f) * spacing;

						// Clamp to bounds
						x = std::clamp(x, bmin.x, bmax.x);
						y = std::clamp(y, bmin.y, bmax.y);
						z = std::clamp(z, bmin.z, bmax.z);

						positions.emplace_back(x, y, z);
						// Tight radii on purpose (see manifest block): fat
						// spheres make each pixel scan ~15 neighbors.
						radii.emplace_back(spacing * 0.5f, spacing * 0.25f);
					}
				}
			}

			// Occupancy cull: a grid point buried inside solid geometry
			// captures mostly wall interior (dark, wrong for its neighbors)
			// and can win nearest-probe ties over open-air probes. Drop a
			// grid point only when it is clearly embedded: all six axial
			// rays blocked within 0.3 m. Corridor/air probes always survive
			// (at least one axis escapes). Explicit --cubemap-pos points
			// are never culled (artist placed).
			{
				std::vector<glm::vec3> open;
				open.reserve(positions.size());
				std::vector<glm::vec2> openRadii;
				openRadii.reserve(radii.size());
				size_t dropped = 0;
				const glm::vec3 axes[6] = {
					{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
					{ 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
				};
				for (size_t oi = 0; oi < positions.size(); ++oi) {
					const glm::vec3& p = positions[oi];
					bool embedded = true;
					for (const glm::vec3& a : axes) {
						smvk::gi::TriangleBvh::Hit h{};
						if (!traceScene.Bvh().Raycast(p, a, 0.3f, h)) {
							embedded = false;
							break;
						}
					}
					if (embedded) {
						++dropped;
						std::cerr << "  [CUBE] dropping embedded grid probe ("
							<< p.x << "," << p.y << "," << p.z << ")\n";
					} else {
						open.push_back(p);
						openRadii.push_back(radii[oi]);
					}
				}
				positions.swap(open);
				radii.swap(openRadii);
				std::cerr << "  [CUBE] grid occupancy: kept " << positions.size()
					<< ", dropped " << dropped << " embedded\n" << std::flush;
			}
		} else {
			// Single probe at bounds center (original behavior)
			const glm::vec3 c = (bmin + bmax) * 0.5f;
			positions.push_back(glm::vec3(c.x, bmin.y + args.cubemapHeight, c.z));
			std::cerr << "  [CUBE] no --cubemap-pos: capturing at bounds center ("
				<< positions.back().x << "," << positions.back().y << ","
				<< positions.back().z << ")\n" << std::flush;
		}
	}

	std::string base = args.cubemapName;
	if (base.empty())
		base = std::filesystem::path(args.outPak).stem().generic_string();
	if (base.empty())
		base = "sky";

	// Artist-pinned extra probes (--cubemap-extra-pos): appended AFTER
	// placement, flagged 'pin' in the manifest so the runtime sorts them
	// first — a pinned probe wins weight ties (e.g. the capture exactly at
	// the chrome ball). Radii default tight: a pin is a deliberate local
	// reflection source, not coverage. NOTE: the trace scene never
	// contains runtime-injected props (the chrome ball is an app-side
	// node), so a pin at the ball captures the ATRIUM AROUND it — exactly
	// what a mirror ball should reflect.
	std::vector<char> pinned;
	for (const std::string& spec : args.cubemapExtraPos) {
		glm::vec3 p{};
		if (std::sscanf(spec.c_str(), "%f,%f,%f",
				&p.x, &p.y, &p.z) != 3) {
			std::cerr << "Bad --cubemap-extra-pos (want x,y,z): "
				<< spec << "\n";
			return false;
		}
		if (!positions.empty() && radii.size() != positions.size())
			radii.resize(positions.size(), glm::vec2(10.0f, 5.0f));
		positions.push_back(p);
		radii.emplace_back(args.cubemapExtraRadius,
			args.cubemapExtraRadius * 0.5f);
		pinned.push_back(1);
		std::cerr << "  [CUBE] pinned extra probe (" << p.x << "," << p.y
			<< "," << p.z << ") r=" << args.cubemapExtraRadius << "\n"
			<< std::flush;
	}
	// Placement-derived rows come first and are never pinned.
	pinned.insert(pinned.begin(), positions.size() - pinned.size(), 0);

	// Runtime budget: the lit shader walks at most 32 probe entries per
	// pixel (tile lists; see environment.hlsli). Captures past 32 still
	// cost bake time + ~2 MB pak each; the manifest truncation keeps the
	// pinned rows, the rest clamp.
	if (positions.empty()) {
		std::cerr << "  [CUBE] WARNING: no capture positions survived"
			" (all grid points embedded?) - no cubemaps baked\n" << std::flush;
		return true;
	}
	if (positions.size() > 32) {
		std::cerr << "  [CUBE] WARNING: " << positions.size()
			<< " captures exceed the 32-probe runtime budget (manifest"
			<< " truncation keeps pinned rows) - increase --cubemap-grid"
			<< " spacing or --cubemap-octree-max to cover the scene with"
			<< " fewer, wider probes\n" << std::flush;
	}

	const uint32_t S = args.cubemapSize;
	const uint32_t levels = smvk::CubemapFullMipCount(S);
	smvk::ThreadPool pool(0);

	// Debug PNG outputs (loose, next to the pak inputs — never in the pak).
	// Faces strips (+ mip sheets) per capture, then one top-down map.
	std::filesystem::path cubePngDir =
		std::filesystem::path(args.contentDir) / "textures/cubemaps";
	if (args.cubemap && args.cubemapPng) {
		std::error_code dcEc;
		std::filesystem::create_directories(cubePngDir, dcEc);
	}
	std::vector<glm::vec3> pngMeans;
	pngMeans.reserve(positions.size());

	for (size_t cap = 0; cap < positions.size(); ++cap) {
		const glm::vec3 origin = positions[cap];
		const std::string captureName = positions.size() == 1
			? base : base + "_" + std::to_string(cap);

		// Store TRUE radiance (no /skyIntensity): the runtime's baked path
		// scales only by the cubemap panel intensity, so a mirror
		// reflecting a sun-lit wall matches looking at that wall directly.
		// (The old /skyIntensity + ibl convention crushed sun-lit
		// reflections by skyIntensity/ibl, ~30x.) Debug PNGs divide by
		// skyIntensity at DISPLAY time so the strips keep the old look.
		const float cubemapScale = 1.0f;
		const float pngDisplayExposure =
			1.0f / std::max(args.skyIntensity, 1e-4f);

	// Trace 6 HDR base faces. Per texel: --cubemap-spp transport paths,
	// averaged — the NEE sun/spot term is analytic (clean at 1 spp), but
	// the bounce field is a Monte-Carlo estimate and 1 spp reads as
	// speckle instead of the GI the lightmapped scene shows. Per-texel
	// seeds stay deterministic (spp continues the same rng stream).
		smvk::CubemapBaseFaces faces;
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
						const uint64_t seed = (uint64_t(cap + 1) << 48)
							^ (uint64_t(f) << 40)
							^ (uint64_t(y) * 4099ull + uint64_t(x) * 130963ull + 1ull);
						smvk::gi::Pcg32 rng(seed);
						glm::vec3 L(0.0f);
						for (uint32_t s = 0; s < args.cubemapSpp; ++s) {
							smvk::gi::TriangleBvh::Hit h;
							if (!traceScene.Bvh().Raycast(origin, dir, 1.0e5f, h)) {
								L += traceScene.SkyRadiance(dir);
							} else {
								L += traceScene.TraceIncoming(origin, dir, rng,
									args.cubemapBounces);
							}
						}
						L *= cubemapScale / float(args.cubemapSpp);
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

		// GGX-preconvolve into the specular mip chain (one face per
		// worker; roughness = level / (levels - 1)).
		smvk::CubemapMipChains chains;
		chains.resize(smvk::kCubemapFaceCount);
		futures.clear();
		for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f) {
			futures.push_back(pool.enqueue([&, f] {
				smvk::PrefilterCubemapFaceGGX(faces, S,
					smvk::CubemapFace(f), levels, args.cubemapRays,
					chains[f]);
			}));
		}
		for (auto& fu : futures)
			fu.get();

		std::vector<uint8_t> ktx;
		std::string werr;
		if (!WriteCubemapKtx2ToMemory(chains, S, S, levels, ktx, werr)) {
			std::cerr << "[CUBE] encode failed '" << captureName << "': "
				<< werr << std::endl;
			return false;
		}
		const std::string vfsPath = smvk::CubemapVfsPath(captureName);
		std::cerr << "  [CUBE] baked " << vfsPath << "  " << S << "x" << S
			<< "x6 ggx-levels=" << levels << " rays=" << args.cubemapRays
			<< " origin=(" << origin.x << "," << origin.y << "," << origin.z
			<< ")  " << ktx.size() << " B\n" << std::flush;
		out.emplace_back(vfsPath, std::move(ktx));

		// Debug PNGs from the same preconvolved chains the KTX encodes
		// (comparing these against the runtime view separates bake/encode
		// faults from load/sample faults). Non-fatal by design.
		glm::vec3 capMean = pakcook::CubemapCaptureMean(chains, S);
		pngMeans.push_back(capMean);
		if (pakcook::CubemapMeanLuma(capMean) < 1e-6f) {
			std::cerr << "  [CUBE] WARNING baked '" << captureName
				<< "' mean ~0 (origin embedded or fully occluded?) - check "
				<< captureName << ".png\n" << std::flush;
		}
		if (args.cubemapPng) {
			std::string perr;
			const std::string facesPath =
				(cubePngDir / (captureName + ".png")).generic_string();
			if (pakcook::WriteCubemapFacesPng(chains, S, pngDisplayExposure, facesPath, perr)) {
				std::cerr << "  [CUBE] png " << facesPath
					<< " (mean luma "
					<< pakcook::CubemapMeanLuma(capMean) << ")\n" << std::flush;
			} else {
				std::cerr << "  [CUBE] png write failed: " << perr << "\n" << std::flush;
			}
			if (args.cubemapPngMips) {
				const std::string mipsPath =
					(cubePngDir / (captureName + "_mips.png")).generic_string();
				if (!pakcook::WriteCubemapMipsPng(chains, S, pngDisplayExposure, mipsPath, perr)) {
					std::cerr << "  [CUBE] mips png write failed: " << perr
						<< "\n" << std::flush;
				}
			}
		}
	}

	// Multi-probe placement manifest: the runtime cannot guess grid
	// positions from KTX blobs alone, so list every capture with its
	// cook/world-space origin + influence/fade radii. Served as a Texture
	// blob (PakBackend serves Texture blobs by VFS path) and consumed by
	// SceneRenderer::LoadCubemaps. Single captures need none (the runtime
	// centers a global probe on the scene bounds itself).
	if (positions.size() > 1) {
		// Radii ride the parallel radii[] vector (filled per branch:
		// explicit 10/5, grid tight to the spacing, octree per-cell,
		// pinned extras tight to --cubemap-extra-radius), and the pinned[]
		// flags mark the artist rows, so the manifest matches what the
		// placement chose.
		std::vector<smvk::CubemapManifestEntry> rows;
		rows.reserve(positions.size());
		for (size_t cap = 0; cap < positions.size(); ++cap) {
			const std::string captureName = base + "_" + std::to_string(cap);
			smvk::CubemapManifestEntry e{};
			e.vfsPath = smvk::CubemapVfsPath(captureName);
			e.position = positions[cap];
			e.influenceRadius = radii.size() == positions.size() ? radii[cap].x : 10.0f;
			e.fadeRadius = radii.size() == positions.size() ? radii[cap].y : 5.0f;
			e.pinned = cap < pinned.size() && pinned[cap] != 0;
			rows.push_back(std::move(e));
		}
		const std::string text = smvk::CubemapManifestSerialize(rows);
		const std::string manifestPath = smvk::CubemapManifestVfsPath(base);
		out.emplace_back(manifestPath,
			std::vector<uint8_t>(text.begin(), text.end()));
		std::cerr << "  [CUBE] manifest " << manifestPath << "  probes="
			<< rows.size() << " pinned=" << args.cubemapExtraPos.size()
			<< "\n" << std::flush;
	}

	// Top-down placement map (positions + mean colors + influence rings):
	// "is there a cubemap near X?" without launching the app. Non-fatal.
	if (args.cubemapPng && !positions.empty()
		&& pngMeans.size() == positions.size()) {
		glm::vec3 mmin(1e30f), mmax(-1e30f);
		for (const glm::vec3& p : positions) {
			mmin = glm::min(mmin, p);
			mmax = glm::max(mmax, p);
		}
		// Pad degenerate spans so single probes still map.
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
			(cubePngDir / (base + "_probes.png")).generic_string();
		std::string merr;
		if (pakcook::WriteCubemapProbesPng(positions, pngMeans, influences,
				mmin, mmax, mapPath, merr)) {
			std::cerr << "  [CUBE] probes map " << mapPath << "\n" << std::flush;
		} else {
			std::cerr << "  [CUBE] probes map write failed: " << merr
				<< "\n" << std::flush;
		}
	}

	const double secs = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - t0).count();
	std::cerr << "  [CUBE] preconvolved cubemaps: captures=" << positions.size()
		<< " seconds=" << secs << "\n" << std::flush;
	return true;
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
	// re-parameterizes meshes (chart-uv + chart-id stamping + seam vertex
	// duplication + index remap), so meshlets must NOT be built before
	// it — an import-time meshlet set references the PRE-stamp vertex
	// layout and draws the wrong chart uv per corner at runtime
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
		if (toCook.size() > 1 && args.cubemap) {
			std::cerr << "--cubemap needs exactly one cooked scene "
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
			use("lmMetersPerTexel", args.lmMetersPerTexel, k.lmMetersPerTexel);
			use("lmLobes", args.lmLobes, k.lmLobes);
			use("lmAtlasWidth", args.lmAtlasWidth, k.lmAtlasWidth);
			use("lmRays", args.lmRays, k.lmRays);
			use("lmExcludeSmall", args.lmExcludeSmall, k.lmExcludeSmall);
			use("sunAzimuth", args.sunAzimuth, k.sunAzimuth);
			use("sunElevation", args.sunElevation, k.sunElevation);
			use("sunIntensity", args.sunIntensity, k.sunIntensity);
			use("skyIntensity", args.skyIntensity, k.skyIntensity);
			use("cubemap", args.cubemap, k.cubemap);
			use("cubemapName", args.cubemapName, k.cubemapName);
			use("cubemapSize", args.cubemapSize, k.cubemapSize);
			use("cubemapRays", args.cubemapRays, k.cubemapRays);
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
	// per-vertex chart uvs + chart ids + per-mesh chart transforms and
	// atlas chunk — and write the atlas (<contentDir>/gi/<scene>.sglm)
	// next to the pak. No bake-side work is left to the runtime.
	// Sources: sun + static spots (NEE) + real
	// materials (texture-averaged); transmissive (Blend) meshes pass rays
	// through, tinted (no charts for them). Shared (instanced) meshes are
	// de-instanced above so every instance bakes its own coefficients.
	// Decal overlays (cook-stamped no-cast on non-MASK materials) are out
	// of both charts and trace (see GiSceneBuildOptions::excludeDecalOverlays).
	// Charts come from xatlas ONLY (GenerateLightmapChartsXatlas, brute-force
	// pack); atlas stays alive past the block for the post-bake stages.
	// ---------------------------------------------------------------------------
	smvk::gi::SgLightmapAtlas atlas;
// Pool indices stamped in ATLAS-TEXEL space. The normalize pass below
// divides exactly these meshes once by the final dimensions —
	// never anything else (unstamped uploads carry imported [0,1] UVs).
	std::unordered_set<uint32_t> lmTexelStamped;
	// Meshes stamped via Frostbite UV transfer (--lods on: the sibling
	// chain cooks first and transfers the LOD0 layout).
	uint32_t lmLodTransferred = 0;
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
		// charts of ALL meshes fit the fixed-width atlas and its height
		// cap.
		struct LmWork {
			uint32_t node = 0;
			smvk::ClusterStaticMeshHandle handle{};
			// Chain members that receive the layout through UV transfer
			// (every sibling level; empty when the mesh has no LOD
			// chain). The chart + bake source is always the mesh's own
			// upload (LOD0).
			std::vector<smvk::ClusterStaticMeshHandle> transfer;
			float density = 0.0f;   // chart-space density actually used
			float worldScale = 1.0f; // node world scale (chart/world unit ratio)
			float curveScale = 1.0f; // curvature density boost (>= 1)
			SgLightmapChartResult chart;
			// chunk index base of this mesh's charts in the atlas
			// (chunk == chart rect; the mesh stamps its lobe count).
			uint32_t chunkBase = 0;
		};
		// Chart density lives in the UPLOAD's position space, but
		// --lm-meters-per-texel is world-space: nodes with a world
		// scale != 1 (Sponza's 0.008 root) must scale it, or charts come
		// out 1/scale too dense and the atlas width/height caps drop the
		// biggest meshes (walls/floor fell through to the grids).
		// Per-node scaling happens below; baseDensity carries the
		// world-space request.
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
		// Meters-per-texel request -> world texels/m base for the xatlas
		// options (density floor 2 texels/m = mpt ceiling 0.5). The
		// convergence search below still speaks texels/m; the engine
		// option converts back per probe.
		const uint32_t lmGutter =
			std::clamp<uint32_t>(args.lmGutter, 1, 4);
		const float baseDensity = std::max(
			1.0f / std::max(args.lmMetersPerTexel, 1e-4f), 2.0f);
		if (!args.lmExcludeKeywords.empty() || args.lmExcludeSmall > 0.0f)
			std::cerr << "  [LM] name exclusion: "
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
		const float floorD = std::max(w.worldScale, 1e-3f);
		const float localBase = std::max(
			baseDensity * w.worldScale, floorD);
		// Curvature boost: smoothly-bending meshes chart denser so
		// the normal-turn per texel stays under the target; the
		// fixed-width atlas pays for it in height (the convergence
		// below only lowers density on height-cap overflow).
		w.curveScale = args.lmCurveTarget > 0.0f
			? smvk::gi::LightmapCurvatureScale(*upload, localBase,
				args.lmCurveTarget, std::max(args.lmCurveMax, 1.0f))
			: 1.0f;
		// xatlas charts the whole scene in ONE batch during the pack
		// convergence below (world-prescaled positions under a single
		// world texelsPerUnit); nothing per mesh here beyond the
		// eligibility filters above.
		w.density = std::max(localBase * w.curveScale, floorD);
		std::cerr << "  [LM] xatlas node " << node
			<< " (curve x" << w.curveScale << ")\n" << std::flush;
		work.push_back(std::move(w));
	}

		// ---- Frostbite LOD sources (--lods on) ----
		// Siblings cook FIRST: every mesh charts + bakes its OWN upload
		// (LOD0) exactly once — one bake, one atlas level — and every
		// sibling level receives the layout through UV transfer below.
		// All LODs sample the same rects, so LOD switches never
		// re-light and the atlas stops growing per level. LOD0's charts
		// are the transfer source for the whole chain; the simplifier
		// runs chart-free.
		if (args.lods && !work.empty()) {
			smvk::MeshOptConfig lodOpt = opt;
			lodOpt.generateLods = true;
			smvk::CookLodSiblingsOptions sibOpts;
			sibOpts.audit = false;
			smvk::MeshletConfig noMeshlets = meshletCfg;
			noMeshlets.enabled = false;
			for (auto& sc : sceneClusters)
				smvk::CookLodSiblings(*sc, lodOpt, noMeshlets, sibOpts);
			// Sibling lookup: base uri -> lodIndex -> sibling uri (from
			// the markers CookLodSiblings just registered). Every
			// sibling level (1..maxLod) transfers from the base upload.
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
			uint32_t frostChains = 0;
			for (LmWork& w : work) {
				const std::string baseUri =
					cluster->GetMeshUri(w.handle).Str();
				if (baseUri.empty())
					continue;
				const auto bit = lodUris.find(baseUri);
				if (bit == lodUris.end())
					continue; // no chain: nothing to transfer
				for (uint32_t level = 1; level <= maxLod; ++level) {
					const auto lit = bit->second.find(level);
					if (lit == bit->second.end())
						continue;
					const auto h = cluster->FindMeshByUri(
						smvk::StringViewT(lit->second));
					if (!h.has_value() || !h->valid())
						continue;
					const smvk::StaticMeshUpload* up =
						cluster->GetRetainedUpload(*h);
					if (!up || up->vertices.empty()
						|| up->indices.empty())
						continue;
					w.transfer.push_back(*h);
				}
				if (!w.transfer.empty())
					++frostChains;
			}
			std::cerr << "  [LM] frostbite: siblings cooked, LOD0 sources "
				<< frostChains << "/" << work.size()
				<< " with transfers (bake once on LOD0, UV-transfer "
				<< "siblings)\n" << std::flush;
		}

		if (!work.empty()) {
			// Xatlas pack convergence: one xatlas chart+pack over ALL
			// meshes per probe (world-prescaled positions under a single
			// world texelsPerUnit, bruteForce always on), stacked into a
			// fresh atlas of the FIXED cook width (xatlas packs its own
			// canvas; Append rejects charts wider than the request and
			// reports height overflow). Halve to feasible, geometric
			// binary search to ~10%, drop hungriest meshes at the
			// 1 texel/m floor — all driven by the height cap — but the
			// WINNING probe's level+atlas are kept verbatim — xatlas
			// threading makes regeneration non-identical, so the winner
			// is never re-probed. The winning level IS the bake level
			// (LOD0 uploads) — the single bake and every UV transfer
			// below consume it directly.
			const uint32_t kMaxAtlasH = args.lmMaxHeight;
			// Fixed atlas width, rounded to the BC6H 4-texel block
			// multiple (the atlas IS the GPU texture).
			const uint32_t atlasW = (std::max<uint32_t>(
				args.lmAtlasWidth, 256) + 3u) & ~3u;
			SgLightmapXatlasOptions xopts;
			xopts.gutterTexels = lmGutter;
			// Layout-first: convergence probes pack + shrink only
			// (rects decide fit). The winning level is rasterized once
			// below, before the expensive GI bake consumes its texels.
			xopts.deferTexelRaster = true;
			auto buildXinputs = [&]() {
				std::vector<SgLightmapXatlasMeshInput> ins;
				ins.reserve(work.size());
				for (const LmWork& w : work) {
					SgLightmapXatlasMeshInput in;
					// Chart source is always the mesh's own upload
					// (LOD0); siblings transfer below.
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
				// Stacks the probe level into a scratch atlas: the fixed
				// width cap + the height cap decide fit (Append rejects
				// wider charts and reports height overflow itself).
				SgLightmapAtlas probe;
				std::vector<uint32_t> keys;
				keys.reserve(work.size());
				for (const LmWork& w : work)
					keys.push_back(w.node);
				std::vector<uint32_t> dummy;
				std::string aerr;
				return AppendXatlasLevelToAtlas(probe, lv, keys,
					args.lmLobes, atlasW, kMaxAtlasH, dummy, aerr);
			};
			uint32_t round = 0;
			float fitDensity = 0.0f;   // best (highest) fitting so far
			float failDensity = baseDensity; // lowest failure
			SgLightmapXatlasLevel bestLevel;
			// Repack failures split by class: Infeasible (near-capacity
			// job) converges the density down; InternalError (the whole
			// sep-inflated job under HALF the atlas area with no
			// placement) is a packer deficiency — abort the cook loudly
			// instead of converging into the ground to route around it.
			bool repackBug = false;
			auto repackOrAbort = [&](SgLightmapXatlasLevel& lv,
					const char* where) {
				std::string xerr;
				const RepackStatus rs =
					RepackXatlasLevel(lv, atlasW, kMaxAtlasH, xerr);
				if (rs == RepackStatus::Ok)
					return true;
				if (rs == RepackStatus::InternalError) {
					std::cerr << "[LM] repack internal error at " << where
						<< ": " << xerr
						<< "\n[LM] this is a packer bug - please report;"
						" aborting instead of converging around it\n"
						<< std::flush;
					repackBug = true;
					return false;
				}
				std::cerr << "  [LM] " << where << ": " << xerr << "\n"
					<< std::flush;
				return false;
			};
			auto tryXatlasPack = [&](float worldDensity) {
				// The convergence search speaks world texels/m; the
				// engine option is meters-per-texel (converted inside).
				xopts.metersPerTexel = 1.0f / std::max(worldDensity, 1e-3f);
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
				// Post-shrink AABBs of concave charts are expected to
				// overlap (xatlas only promises dilated-coverage
				// disjointness): repack big->small into disjoint
				// placements before measuring fit. A repack bug aborts
				// the cook (repackOrAbort); Infeasible just fails the
				// probe so convergence lowers the density.
				if (!repackOrAbort(lv, "xatlas probe")) {
					if (repackBug)
						std::cerr << "  [LM] xatlas probe " << worldDensity
							<< " texels/m\n" << std::flush;
					return false;
				}
				if (work.empty() || !xatlasProbeFits(lv))
					return false;
				bestLevel = std::move(lv);
				return true;
			};
			// Initial probe at the requested density.
			if (tryXatlasPack(baseDensity)) {
				fitDensity = baseDensity;
			} else {
				// Phase 1: halve until a pack fits (feasible lower bound).
				float probeD = baseDensity;
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
				xopts.metersPerTexel = 1.0f;
					std::string xerr;
				if (!GenerateLightmapChartsXatlas(buildXinputs(),
						xopts, bestLevel, xerr)) {
					std::cerr << "[LM] atlas overflow (" << xerr
						<< "; raise --lm-meters-per-texel, --lm-atlas-width"
						<< " or --lm-max-height)\n";
					return 1;
				}
				xatlasDropUnchartable(bestLevel);
				if (!repackOrAbort(bestLevel, "floor pack")) {
					if (repackBug)
						return 1;
					std::cerr << "[LM] atlas overflow (" << xerr
						<< "; raise --lm-meters-per-texel, --lm-atlas-width"
						<< " or --lm-max-height)\n";
					return 1;
				}
				while (!xatlasProbeFits(bestLevel)) {
					if (++round > 32 || work.empty()) {
						std::cerr << "[LM] atlas overflow ("
							<< work.size()
							<< " meshes remain; raise --lm-meters-per-texel,"
							" --lm-atlas-width or --lm-max-height)\n";
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
					if (!repackOrAbort(bestLevel, "drop round repack")) {
						if (repackBug)
							return 1;
						std::cerr << "[LM] atlas overflow ("
							<< work.size()
							<< " meshes remain; raise --lm-meters-per-texel,"
							" --lm-atlas-width or --lm-max-height)\n";
						return 1;
					}
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
			// Finalize layout BEFORE the expensive GI work: the winning
			// (layout-only) level was already repacked big->small into
			// disjoint rects during probing. Rasterize it once into
			// those final rects, gate the repacked rects pairwise,
			// then land it in the real atlas (deterministic re-stack)
			// and hand charts to the bake.
			{
				std::string rerr;
				if (!RasterizeLightmapPackTexels(buildXinputs(), bestLevel,
						lmGutter, rerr)) {
					std::cerr << "[LM] winning level rasterize failed: "
						<< rerr << "\n";
					return 1;
				}
				std::string overlapErr;
				if (!CheckXatlasLevelRectsForOverlap(bestLevel,
						&overlapErr)) {
					std::cerr << "[LM] winning level rect overlap: "
						<< overlapErr << "\n";
					return 1;
				}
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
				if (!CheckAtlasChunksForOverlap(atlas, &overlapErr)) {
					std::cerr << "[LM] atlas chunk overlap after Append: "
						<< overlapErr << "\n";
					return 1;
				}
				// Fixed atlas width (xatlas's own canvas can be narrower;
				// the empty right columns stay zero — see the planar
				// block). Safe here: stamping below is texel-space and
				// the normalize pass divides by the FINAL dims.
				atlas.width = atlasW;
				for (size_t i = 0; i < work.size(); ++i) {
					work[i].chart =
						std::move(bestLevel.meshes[i].chart);
					work[i].chunkBase = bases[i];
					work[i].density = fitDensity * work[i].worldScale
						* work[i].curveScale;
				}
			}
			std::cerr << "  [LM] xatlas density converged at " << fitDensity
				<< " texels/m world base (" << fitDensity * fitDensity
				<< " texels/m^2, " << (1.0f / fitDensity) << " m/texel); meshes="
				<< work.size()
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
		// retained uploads: per-corner chart-local texCoord2 + chart ids
		// + per-chart atlas transforms + the mesh's lobe count (lmChunk).
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
			// Seam stitch (post-bake, payloads at final lobe counts):
			// chart-boundary texels blend toward same-normal baked
			// texels of sibling chunks so duplicated seam vertices
			// sample one value on both sides; margins re-dilate after.
			if (args.lmStitch) {
				uint64_t stitched = 0;
				std::string serr;
				if (!StitchSgLightmapSeams(
						std::span<const SgLightmapChunkBatch>(batches),
						0.5f, &stitched, serr)) {
					std::cerr << "[LM] seam stitch failed: " << serr
						<< " (continuing on unstitched payloads)\n"
						<< std::flush;
				} else {
					std::cerr << "  [LM] seam stitch: " << stitched
						<< " boundary texels blended\n" << std::flush;
				}
			}
		}

		// NOTE: the OIDN denoise pass lives AFTER the stamp + transfer
		// loop, so it covers the baked payload exactly once (see below).
		for (LmWork& w : work) {
			// Source stamp: the charted parameterization (always the
			// mesh's own upload = LOD0) stamps itself directly —
			// per-corner chart-uv + chart-id stamping, seam-correct (see
			// StampLightmapUvs) — with texel-space transforms (the
			// normalize pass below finishes the job).
			smvk::StaticMeshUpload* srcUpload =
				const_cast<smvk::StaticMeshUpload*>(
					cluster->GetRetainedUpload(w.handle));
			const size_t srcTris =
				srcUpload ? srcUpload->indices.size() / 3 : 0;
			if (!srcUpload || w.chart.triChart.size() != srcTris) {
				std::cerr << "[LM] triangle count mismatch for node "
					<< w.node << " - mesh skipped\n";
				continue;
			}
			std::vector<uint32_t> chartOf;
			std::vector<glm::vec4> xforms;
			uint32_t uncharted = 0;
			const uint32_t lobes = StampLightmapUvs(*srcUpload, w.chart,
				w.chunkBase, atlas, lmGutter, args.lmLobes,
				chartOf, xforms, seamDups, uncharted);
			lmTexelStamped.insert(w.handle.index());
			if (uncharted != 0)
				std::cerr << "  [LM] stamp node " << w.node
					<< ": uncharted=" << uncharted << "\n" << std::flush;
			// The draw ring carries the LOBE COUNT (5/6/9/12) chosen by
			// the bake for this MESH (read from any of its chunks —
			// the per-mesh selection wrote them all); 0xFFFFFFFF = no
			// lightmap stays the pak default. The per-chart atlas
			// transforms ride the mesh for the shader unpack.
			if (smvk::StaticMesh* m = cluster->staticMeshes.get(w.handle)) {
				m->lmChunk = lobes;
				m->lmChartXforms = std::move(xforms);
			}
			// Frostbite UV transfer: every sibling level inherits the
			// LOD0 atlas layout through closest-point projection onto
			// the charted base upload (TransferLightmapCharts). All
			// levels sample the same rects: no per-level bakes, no
			// LOD-switch re-lighting. Failure keeps the level's
			// inherited chart uvs (uncharted triangle semantics) and the
			// shared lobe count.
			for (const smvk::ClusterStaticMeshHandle th : w.transfer) {
				smvk::StaticMeshUpload* up =
					const_cast<smvk::StaticMeshUpload*>(
						cluster->GetRetainedUpload(th));
				if (!up || up->vertices.empty() || up->indices.empty()) {
					if (smvk::StaticMesh* m =
							cluster->staticMeshes.get(th))
						m->lmChunk = lobes;
					continue;
				}
				SgLightmapChartResult xfer;
				std::string terr;
				SgLightmapTransferStats tstat;
				if (!TransferLightmapCharts(*srcUpload, w.chart, *up,
						xfer, terr, &tstat)) {
					std::cerr << "  [LM] transfer failed node " << w.node
						<< " pool " << th.index() << ": " << terr
						<< " (inherited chart uvs)\n" << std::flush;
					if (smvk::StaticMesh* m =
							cluster->staticMeshes.get(th))
						m->lmChunk = lobes;
					continue;
				}
				std::cerr << "  [LM] transfer node " << w.node
					<< " pool " << th.index() << ": tris " << tstat.trisTotal
					<< " same " << tstat.trisSameChart
					<< " vote " << tstat.trisVote
					<< " rescued " << tstat.trisRescued
					<< " degenerate " << tstat.trisDegenerate
					<< " uncharted " << tstat.trisUncharted << "\n"
					<< std::flush;
			std::vector<uint32_t> dummy;
			std::vector<glm::vec4> xferXforms;
			uint32_t unch = 0;
			const uint32_t lobesT = StampLightmapUvs(*up, xfer,
				w.chunkBase, atlas, lmGutter, lobes,
				dummy, xferXforms, seamDups, unch);
			lmTexelStamped.insert(th.index());
				if (unch != 0)
					std::cerr << "  [LM] transfer stamp node " << w.node
						<< " pool " << th.index()
						<< ": uncharted=" << unch << "\n" << std::flush;
				if (smvk::StaticMesh* m = cluster->staticMeshes.get(th)) {
					m->lmChunk = lobesT;
					m->lmChartXforms = std::move(xferXforms);
				}
				++lmLodTransferred;
			}
			// No meshlet work in the bake stage (decoupled by design):
			// meshlets are built once, after ALL lightmap stamping
			// finishes, by the "Meshlet restore" stage below — from
			// this final stamped topology, never a pre-stamp layout.
			++lmMeshes;
		}





		// ---- Texel-space normalize pass ----
		// Stamps computed ATLAS-TEXEL transforms; with dimensions
		// final (single bake level, no post-stamp appends), divide the
		// stamped meshes' TRANSFORMS once by the dims (vertices carry
		// chart-local uvs and are never touched). Empty on the planar
		// path (its transforms are already normalized).
		if (!lmTexelStamped.empty() && atlas.width > 0 && atlas.height > 0) {
			const glm::vec2 dims(float(atlas.width), float(atlas.height));
			// Deterministic pool-index order (stable table layout).
			std::vector<uint32_t> order(lmTexelStamped.begin(),
				lmTexelStamped.end());
			std::sort(order.begin(), order.end());
			for (uint32_t meshIdx : order) {
				const smvk::ClusterStaticMeshHandle h =
					cluster->staticMeshes.getHandle(meshIdx);
				if (!h.valid())
					continue;
				if (smvk::StaticMesh* m = cluster->staticMeshes.get(h)) {
					for (glm::vec4& xf : m->lmChartXforms) {
						xf.x /= dims.x;
						xf.y /= dims.y;
						xf.z /= dims.x;
						xf.w /= dims.y;
					}
				}
			}
			std::cerr << "  [LM] texel-space transforms normalized: "
				<< order.size() << " meshes over " << atlas.width << "x"
				<< atlas.height << "\n" << std::flush;
		}

		// OIDN denoise pass: RTLightmap over the fitted per-lobe
		// amplitude layers, in place (see lm_denoise.cpp). Runs after
		// the (single) bake and all stamping, before the atlas write,
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
			// Payload compression (default bc6hopt): BC6H blocks with the
			// Frostbite invalid-texel prefill. Atlas dims are 4-aligned by
			// construction (AppendXatlasLevelToAtlas rounds the height,
			// the width cap is a power of two); WriteFile serializes the
			// blocks as sglm v3.
			if (args.lmCompress != SgLightmapCompression::Uncompressed) {
				const auto bcT0 = std::chrono::steady_clock::now();
				if (!atlas.CompressToBc6h(
						args.lmCompress == SgLightmapCompression::Bc6HOpt,
						werr)) {
					std::cerr << "[LM] bc6h compression failed: " << werr
						<< std::endl;
					return 1;
				}
				const size_t rawBytes = size_t(atlas.width) * atlas.height
					* kLightmapLayerCount * 8;
				const double bcSecs = std::chrono::duration<double>(
					std::chrono::steady_clock::now() - bcT0).count();
				std::cerr << "  [LM] bc6h: " << atlas.bc6hLobes
					<< " lobe image(s), " << (atlas.GpuBytes() / (1024 * 1024))
					<< " MiB gpu vs " << (rawBytes / (1024 * 1024))
					<< " MiB raw ("
					<< (args.lmCompress == SgLightmapCompression::Bc6HOpt
						? "opt prefill" : "plain") << ", " << bcSecs
					<< "s)\n" << std::flush;
			}
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
		// Chart-id map (<scene>_charts.png): chunk rects colored by the
		// stamped chart id, inner content bright / margin ring dimmed /
		// separator black — chart layout and padding-leak inspection at
		// a glance (see lm_png.h). Non-fatal.
		{
			std::filesystem::path cPath =
				std::filesystem::path(args.contentDir) / "gi"
				/ (sceneId + "_charts.png");
			std::string perr;
			if (pakcook::WriteLightmapChartsPng(atlas,
					lmGutter, cPath.generic_string(), perr)) {
				std::cerr << "  [LM] charts png " << cPath.generic_string()
					<< " (" << std::filesystem::file_size(cPath)
					<< " B)\n" << std::flush;
			} else {
				std::cerr << "[LM] charts png write failed: " << perr
					<< "\n" << std::flush;
			}
		}
		// Bright-pink fault map (<scene>_pink.png, --lm-debug-pink on):
		// the true loaded lightmap with unbaked/corrupt/overlapping
		// rects in solid (255,0,255). Baked-but-dark chunks (zero
		// traced light) are content, not faults: the mask carries the
		// per-chart bake-point counts. Non-fatal.
		if (args.lmDebugPink) {
			std::filesystem::path pinkPath =
				std::filesystem::path(args.contentDir) / "gi"
				/ (sceneId + "_pink.png");
			std::vector<char> bakedPoints(atlas.chunks.size(), 0);
			for (size_t i = 0; i < work.size(); ++i)
				for (uint32_t c = 0; c < work[i].chart.charts.size()
					&& work[i].chunkBase + c < bakedPoints.size(); ++c)
					bakedPoints[work[i].chunkBase + c] =
						work[i].chart.charts[c].texels.empty() ? 0 : 1;
			std::string perr;
			if (pakcook::WriteLightmapPinkPng(atlas,
					bakedPoints, pinkPath.generic_string(), perr)) {
				std::cerr << "  [LM] pink png " << pinkPath.generic_string()
					<< " (" << std::filesystem::file_size(pinkPath)
					<< " B)\n" << std::flush;
			} else {
				std::cerr << "[LM] pink png write failed: " << perr
					<< "\n" << std::flush;
			}
		}
		// Chart transform debug dump (<scene>_charts_debug.txt): every
		// chunk's texel-space transform (exactly what the VS applies
		// after the normalize pass) + bake-point/tri counts, plus an
		// overlap audit of the chunk rects — xatlas packs the per-chart
		// bitmaps disjoint, so rect intersections mean two payloads
		// write the same atlas texels (last writer wins, both charts
		// sample the shared region = the "overlapped charts" artifact).
		// Non-fatal; the console keeps only the summary line.
		{
			std::filesystem::path dbgPath =
				std::filesystem::path(args.contentDir) / "gi"
				/ (sceneId + "_charts_debug.txt");
			std::ofstream dbg(dbgPath);
			uint32_t ringPairs = 0, contentPairs = 0;
			if (dbg) {
				dbg << "# " << sceneId << " chart transform debug (cook)\n"
					<< "# atlas " << atlas.width << "x" << atlas.height
					<< " chunks " << atlas.chunks.size() << " gutter "
					<< lmGutter << "\n"
					<< "# atlas texel = chartUv * s + o (divide by the"
						" final dims for uv)\n";

				// chunk -> (mesh work idx, local chart, tri count).
				const uint32_t gw = lmGutter;
				std::vector<uint32_t> ownerMesh(atlas.chunks.size(),
					0xFFFFFFFFu);
				std::vector<uint32_t> ownerChart(atlas.chunks.size(), 0);
				std::vector<uint32_t> chartTris(atlas.chunks.size(), 0);
				for (size_t i = 0; i < work.size(); ++i) {
					const SgLightmapChartResult& cr = work[i].chart;
					std::vector<uint32_t> tris(cr.charts.size(), 0);
					for (uint32_t t : cr.triChart)
						if (t < tris.size())
							++tris[t];
					for (uint32_t c = 0; c < cr.charts.size(); ++c) {
						ownerMesh[work[i].chunkBase + c] = uint32_t(i);
						ownerChart[work[i].chunkBase + c] = c;
						chartTris[work[i].chunkBase + c] = tris[c];
					}
				}
				// Per-chunk world area from the owning mesh's charted
				// triangles (node-local positions through the world
				// transform): density = claimed texels per m^2 exposes
				// texel-density variance within and across meshes.
				std::vector<double> chartArea(atlas.chunks.size(), 0.0);
				for (size_t i = 0; i < work.size(); ++i) {
					const SgLightmapChartResult& cr = work[i].chart;
					const smvk::StaticMeshUpload* up =
						cluster->GetRetainedUpload(work[i].handle);
					if (!up || up->indices.size() < 3)
						continue;
					const glm::mat4& g =
						cluster->scene.globalTransform[work[i].node];
					const size_t triCount = up->indices.size() / 3;
					for (size_t t = 0; t < triCount
						&& t < cr.triChart.size(); ++t) {
						const uint32_t lc = cr.triChart[t];
						if (lc >= cr.charts.size())
							continue;
						const uint32_t chunk =
							work[i].chunkBase + lc;
						if (chunk >= chartArea.size())
							continue;
						const uint32_t i0 = up->indices[t * 3 + 0];
						const uint32_t i1 = up->indices[t * 3 + 1];
						const uint32_t i2 = up->indices[t * 3 + 2];
						if (i0 >= up->vertices.size()
							|| i1 >= up->vertices.size()
							|| i2 >= up->vertices.size())
							continue;
						const glm::vec3 a =
							glm::vec3(g * glm::vec4(
								up->vertices[i0].pos, 1.0f));
						const glm::vec3 b =
							glm::vec3(g * glm::vec4(
								up->vertices[i1].pos, 1.0f));
						const glm::vec3 cc =
							glm::vec3(g * glm::vec4(
								up->vertices[i2].pos, 1.0f));
						chartArea[chunk] += 0.5 * double(glm::length(
							glm::cross(b - a, cc - a)));
					}
				}
				auto line = [&](uint32_t ci) {
					const SgLightmapChunk& c = atlas.chunks[ci];
					const glm::vec2 o = atlas.AtlasTexel(ci, gw,
						glm::vec2(0.0f, 0.0f));
					const glm::vec2 e = atlas.AtlasTexel(ci, gw,
						glm::vec2(1.0f, 1.0f));
					const size_t texels = ownerMesh[ci] < work.size()
						? work[ownerMesh[ci]].chart
							.charts[ownerChart[ci]].texels.size()
						: 0;
					const double area = chartArea[ci];
					const double dens = area > 1e-9
						? double(texels) / area : 0.0;
					const double reserved = double(
						std::max(c.rectMax[0] - c.rectMin[0], 0.0f))
						* double(std::max(
							c.rectMax[1] - c.rectMin[1], 0.0f));
					const double fill = reserved > 0.0
						? double(texels) / reserved : 0.0;
					dbg << "chunk " << ci << " node " << c.meshKey
						<< " chart " << ownerChart[ci]
						<< " rect [" << c.rectMin[0] << "," << c.rectMin[1]
						<< " " << c.rectMax[0] << "," << c.rectMax[1] << "]"
						<< " s=(" << (e.x - o.x) << "," << (e.y - o.y)
						<< ") o=(" << o.x << "," << o.y << ")"
						<< " texels " << texels
						<< " tris " << chartTris[ci]
						<< " area_m2 " << area
						<< " dens_per_m2 " << dens
						<< " fill " << fill
						<< " lobes " << c.lobeCount;
				};

				// Sweep-line rect overlap audit. For each pair, also
				// intersect the charts' actual bake-point footprints
				// (rect-local uv -> atlas texel): rect overlaps with
				// ZERO shared texels mean the uvs are fine and only
				// the bbox-derived rects are fatter than the packed
				// footprints; shared texels mean real uv corruption.
				std::vector<uint32_t> byY(atlas.chunks.size());
				for (uint32_t i = 0; i < byY.size(); ++i)
					byY[i] = i;
				std::sort(byY.begin(), byY.end(),
					[&](uint32_t a, uint32_t b) {
						return atlas.chunks[a].rectMin[1]
							< atlas.chunks[b].rectMin[1];
					});
				auto texelSet = [&](uint32_t ci) {
					std::unordered_set<uint64_t> set;
					const SgLightmapChunk& c = atlas.chunks[ci];
					const uint32_t mi = ownerMesh[ci];
					if (mi >= work.size())
						return set;
					for (const SgLightmapTexel& t :
							work[mi].chart.charts[ownerChart[ci]].texels) {
						const int tx = int(c.rectMin[0]) + int(t.uv.x);
						const int ty = int(c.rectMin[1]) + int(t.uv.y);
						set.insert(uint64_t(uint32_t(tx))
							* atlas.height + uint32_t(ty));
					}
					return set;
				};
				std::vector<uint32_t> active;
				for (uint32_t ci : byY) {
					const SgLightmapChunk& c = atlas.chunks[ci];
					active.erase(std::remove_if(active.begin(),
							active.end(), [&](uint32_t a) {
								return atlas.chunks[a].rectMax[1]
									<= c.rectMin[1];
							}), active.end());
					for (uint32_t a : active) {
						const SgLightmapChunk& s = atlas.chunks[a];
						if (!(s.rectMin[0] < c.rectMax[0]
								&& c.rectMin[0] < s.rectMax[0]))
							continue;
						++ringPairs;
						const float ix0 = std::max(s.rectMin[0],
							c.rectMin[0]);
						const float iy0 = std::max(s.rectMin[1],
							c.rectMin[1]);
						const float ix1 = std::min(s.rectMax[0],
							c.rectMax[0]);
						const float iy1 = std::min(s.rectMax[1],
							c.rectMax[1]);
						const bool content = ix0 + float(gw) < ix1 - float(gw)
							&& iy0 + float(gw) < iy1 - float(gw);
						if (content)
							++contentPairs;
						uint64_t shared = 0;
						{
							const std::unordered_set<uint64_t> aSet =
								texelSet(a);
							const uint32_t mi = ownerMesh[ci];
							if (mi < work.size()) {
								const SgLightmapChunk& cc = atlas.chunks[ci];
								for (const SgLightmapTexel& t : work[mi]
										.chart.charts[ownerChart[ci]]
										.texels) {
									const int tx = int(cc.rectMin[0])
										+ int(t.uv.x);
									const int ty = int(cc.rectMin[1])
										+ int(t.uv.y);
									if (aSet.count(uint64_t(uint32_t(tx))
											* atlas.height
											+ uint32_t(ty)))
										++shared;
								}
							}
						}
						dbg << "pair " << a << " " << ci << " "
							<< (content ? "CONTENT" : "ring")
							<< " inter [" << ix0 << "," << iy0
							<< " " << ix1 << "," << iy1 << "]"
							<< " sharedTexels " << shared << "\n";
						dbg << "  A "; line(a); dbg << "\n";
						dbg << "  B "; line(ci); dbg << "\n";
					}
					active.push_back(ci);
				}
				dbg << "overlap summary: " << ringPairs << " ring "
					<< contentPairs << " content\n";
				dbg << "chunks:\n";
				for (uint32_t ci = 0; ci < atlas.chunks.size(); ++ci) {
					line(ci);
					dbg << "\n";
				}
				// Per-mesh density rollup: min/max/mean claimed-texel
				// density over the mesh's chunks. Uniform charting gives
				// one density per mesh (worldScale * curveScale path);
				// spread inside a mesh points at non-uniform node scale
				// (anisotropic world density), clamping, or chart-size
				// quantization on tiny charts.
				dbg << "mesh density:\n";
				for (size_t i = 0; i < work.size(); ++i) {
					double mn = std::numeric_limits<double>::max();
					double mx = 0.0, sum = 0.0, area = 0.0;
					uint64_t tex = 0;
					uint32_t n = 0;
					for (uint32_t c = 0;
						c < work[i].chart.charts.size(); ++c) {
						const uint32_t chunk = work[i].chunkBase + c;
						if (chunk >= chartArea.size())
							continue;
						const size_t t = work[i].chart.charts[c]
							.texels.size();
						const double a = chartArea[chunk];
						if (a <= 1e-9)
							continue;
						const double d = double(t) / a;
						mn = std::min(mn, d);
						mx = std::max(mx, d);
						sum += d;
						area += a;
						tex += t;
						++n;
					}
					dbg << "mesh node " << work[i].node
						<< " worldScale " << work[i].worldScale
						<< " curveScale " << work[i].curveScale
						<< " chunks " << n << " area_m2 " << area
						<< " texels " << tex;
					if (n > 0)
						dbg << " densMean " << (sum / double(n))
							<< " densMin " << mn << " densMax " << mx
							<< " spread "
							<< (mn > 0.0 ? mx / mn : 0.0);
					dbg << "\n";
				}
				std::cerr << "  [LM] chart rect audit: " << ringPairs
					<< " ring / " << contentPairs
					<< " content overlapping pairs; transforms in "
					<< dbgPath.generic_string() << "\n" << std::flush;
			} else {
				std::cerr << "[LM] chart debug dump open failed: "
					<< dbgPath.generic_string() << "\n" << std::flush;
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
			<< " charts=xatlas-bruteforce"
			<< (lmLodTransferred ? " lodUVTransfer=" + std::to_string(
				lmLodTransferred) : "")
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
	// Preconvolved specular cubemaps (--cubemap on): capture + GGX
	// prefilter into KTX2 cubes, embedded as Texture blobs below (after
	// SerializeClusters, before Finalize). Runs after the lightmap block
	// so the trace scene sees the same imported cluster; independent of
	// --lightmap (works on unstamped uploads either way).
	// ---------------------------------------------------------------------------
	std::vector<std::pair<std::string, std::vector<uint8_t>>> cubemapKtx;
	if (args.cubemap) {
		if (!BakePreconvolvedCubemaps(*cluster, args, cubemapKtx))
			return 1;
	}

	// ---------------------------------------------------------------------------
	// Meshlet restore (post-lightmap): with --lightmap on, meshlets were
	// deliberately NOT built anywhere earlier in this cook
	// (importMeshletCfg disabled the importer and merged-block paths);
	// they are built HERE, once, from the FINAL topology — after the
	// chart-uv stamping and seam vertex duplication finished. Every
	// meshlet vertex window references already-final chart uvs: the
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
	// LOD siblings (post-bake): with --lightmap on the sibling chain already
	// cooked inside the lightmap block (frostbite: the base chart + bake
	// needs the siblings first); this call then no-ops on those meshes
	// (idempotent skip). With --lightmap off this is exactly the old
	// import-time chain, just later in the pipeline.
	// ---------------------------------------------------------------------------
	{
		smvk::MeshOptConfig lodOpt = opt;
		lodOpt.generateLods = args.lods;
		smvk::CookLodSiblingsOptions sibOpts;
		// Chart-lock provenance + the parameterization audit only existed
		// on the removed post-bake simplify path; the frostbite transfer
		// shares one parameterization, so there is nothing to audit.
		sibOpts.audit = false;
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
	// Preconvolved cubemaps ride as Texture blobs under their VFS paths
	// (PakBackend serves them; CubemapLibrary::Load resolves them). Added
	// after the cluster serialize so the cook stages above never see
	// synthetic textures in the asset pool. The .cubemanifest (if any) is
	// text inside a Texture payload — served by path, decoded by the
	// cubemap manifest parser, never as an image.
	for (auto& [vfsPath, ktx] : cubemapKtx) {
		std::vector<uint8_t> payload = smvk::BuildTexturePayload(
			vfsPath, -1, -1, -1, -1, std::span<const uint8_t>(ktx));
		if (!writer.AddBlob(smvk::PakAssetKind::Texture, vfsPath,
				std::move(payload))) {
			std::cerr << "Cubemap AddBlob failed: " << vfsPath << std::endl;
			return 1;
		}
		const bool isManifest = vfsPath.size() >= 13
			&& vfsPath.compare(vfsPath.size() - 13, 13, ".cubemanifest") == 0;
		std::cout << (isManifest ? "Embedded cubemap manifest " : "Embedded cubemap ")
			<< vfsPath << " (" << ktx.size() << " B"
			<< (isManifest ? " text" : " ktx2") << ")\n";
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
		<< " cubemaps=" << cubemapKtx.size()
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
