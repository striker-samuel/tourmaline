// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// cook.exe: offline ARTIFACT -> COOKED-ARTIFACT cooker that works
// exclusively on the VFS and needs NO GPU (offline-mode Cluster, see
// Cluster::SetOfflineMode). GI / cubemaps / LODs / meshlets / de-instance
// run here and write back into the artifacts (+ <stem>.cooked);
// pak.exe assembles the .pak from the cooked artifacts (pure
// serialization). Raw glTF never cooks (import.exe / the MeshImporter /
// the editor's open-drop produce the artifacts - glTF is IMPORT-ONLY).
//
//   cook.exe <vfs_path> <artifact...> [--options]
//             cook.exe F:/content models/sponza/Sponza.geometry
//                 [--meshopt on|off]
//                 [--overdraw 1.05]
//                 [--lods on|off] [--lod-ratios 0.75,0.5,0.3,0.15] [--lod-error 0.01]
//                 [--lightmap on|off --lm-meters-per-texel 0.25 --lm-lobes 12
//                  --lm-atlas-width 1024 --lm-gutter 1
//                  --lm-rays 1024
//                  --sun-azimuth 30 --sun-elevation 55 --sun-intensity 10000
//                  --lm-lod-lock attr|hard
//                  --lm-curve-target 7.0 --lm-curve-max 2.5
//                  --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]
//
// --cubemap on bakes preconvolved SPECULAR cubemaps DURING the cook (same
// CPU path-tracer capture as tools/cubemap_baker, GGX importance-sampled
// mip chains via Engine/Renderer/cubemap_prefilter.h — roughness = mip /
// (levels-1)) and embeds them as Texture blobs at
// gi/cubemaps/<name>[_N].ktx2 (PakBackend serves them by VFS path,
// so CubemapLibrary::Load finds them with no code change). Probes and
// lightmaps carry all diffuse + low-frequency specular; these cubes own
// the high-frequency specular path only.
//
// --lightmap on bakes 2D SG lightmaps DURING the cook (sun + static spot
// NEE + analytic sky on open-sky rays, real materials, transmissive
// pass-through — see Engine/GI/bake.h). It stamps chart-local UVs and
// chart IDs into the meshes, records per-chart atlas transforms for the
// shader, and stores each mesh's atlas chunk ID in the pak. The cooker
// writes a loose atlas at <contentDir>/gi/<scene>.sglm and also embeds
// its payload in the pak. Debug PNGs are written beside the loose atlas:
// <scene>_atlas.png shows the payload with the editor's flat-view display
// mapping; <scene>_charts.png shows chart IDs (see lm_png.h).
// No bake work remains at runtime. Covered pixels use ONLY the lightmap;
// analytic sky IBL is a fallback for uncovered pixels, not additional
// ambient lighting on covered pixels.
//
// LOD + lightmap interaction: with --lm-proxy-res > 0, the cooker
// attempts to chart and bake a voxel-remeshed scene proxy ONCE
// (meshopt_remesh solid). Real meshes inherit its layout through
// TransferLightmapCharts (closest-point projection onto the proxy).
// When that proxy bake succeeds, LOD siblings cook AFTER baking from
// the stamped uploads. If the proxy is disabled (--lm-proxy-res 0) or
// fails, the legacy per-mesh LOD0 path is used: siblings cook BEFORE
// baking and receive the LOD0 atlas layout through UV transfer. In
// either path, LOD levels share atlas rects and do not need new bakes.
//
//
// Curvature-weighted density (default on): --lm-meters-per-texel is the
// BASE request; smoothly bending meshes chart at up to --lm-curve-max
// times the base density so their normal-turn per texel stays under
// --lm-curve-target degrees (creases over 45 deg do not count — density
// cannot fix an authored edge). The atlas width is FIXED and the height
// grows with content (only a --lm-max-height overflow lowers the
// density), so curvature spends height, not a texel budget: same
// layout, spent where normals turn fast. Uniform density reads as
// faceted/hard-normal shading on curved geometry (columns/arches at
// ~18 deg per texel).
//
// In artifact mode, reads a derived .geometry file and its dependencies
// through an OverlayVfs with a LooseBackend over --content-dir (the same
// lookup rules as the runtime). In tour mode, replays an editor scene
// definition. Both paths cook geometry and write a pak with embedded
// textures and metadata. There is intentionally no external pak manifest:
// --dump-manifest reads the data back through PakReader and prints it to
// stdout, so the report cannot drift from the written bytes.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
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
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <volk.h> // VkFormat enums (the --cubemap KTX2 encode)

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
#include <Engine/GI/cubemap_volume.h>
#include <Engine/GI/lightmap.h>
#include <Engine/Renderer/cubemap.h>
#include <Engine/Renderer/cubemap_placement.h>
#include <Engine/Renderer/cubemap_prefilter.h>
#include <Engine/Utils/thread_pool.h>
#include <gltf_import/gltf.h>
#include <gltf_import/asset_export.h> // materialAsset replay (tour cooks)
#include <Engine/Asset/bake_asset.h> // bake-asset identity (bake GUIDs)
#include <artifact_cache/artifact_cache.h>
#include <artifact_cache/artifact_kind.h>
#include <artifact_cache/store_adapter.h>
#include <Engine/Vfs/vfs.h>
#include <meshoptimizer.h>

#include "lm_png.h"
#include "cube_png.h"

#include "cook_tool.h"
#include <tool_log.h>

namespace cooktool {

	struct Args {
		std::string contentDir = "content";
		// POSITIONAL CLI (pak.exe <vfs_path> <artifact...> [--options]):
		// argv[1] = the VFS content root (was --content-dir), argv[2..]
		// until the first --option = the ALREADY-PROCESSED artifact list
		// (.geometry files - import.exe/the editor's MeshImporter
		// produces them; raw glTF NEVER cooks here). inVfsList holds the
		// artifacts; inVfs keeps the IDENTITY (the first artifact, or the
		// tour's root reference) for the logs/bake stems.
		std::vector<std::string> inVfsList;
		std::string inVfs; // the primary identity (first artifact / tour ref)
		std::string outPak;
		// .tour replay (editor scene overlay): applied to the offline cluster
		// right after import, before collapse/LOD/bake/serialize. CLI flags
		// win over tour cook defaults (tracked via explicitSet).
		std::string tour;
		std::unordered_set<std::string> explicitSet;
		bool meshopt = true;
		float overdraw = 1.05f;
		// Cooked LOD chain (meshopt_simplify, sibling blobs "<base>/lodN").
		bool lods = false;
		std::string lodRatios = "0.75,0.5,0.3,0.15";
		float lodError = 0.01f; // relative error budget per simplify call
		// Meshlet cook path (SETUP-TIME on/off only): the engine canon is
		// fixed at 64 verts / 64 tris per meshlet (kMeshletMaxVertices /
		// kMeshletMaxTriangles - one mesh-shader workgroup covers a whole
		// meshlet, Northlight-style); there are no tuning flags.
		bool meshlets = false;
		// KTX auto-convert (on by default): non-KTX sources transcode
		// in-process (stb decode + libktx write, `.ktx2` sidecar cached next
		// to the source). Off restores warn-and-skip for non-KTX payloads.
		bool autoKtx = true;
		// Collapse static subtrees into per-material mega-mesh blocks.
		bool collapseStatic = false;
		// ---- Lightmap cook (--lightmap on) ----
		bool lightmap = false;
		// Chart density request, METERS PER TEXEL (world space): 0.25 = one
		// texel per 25 cm (4 texels/m). The engine chart options speak
		// texels/m; the cook converts (1/mpt, floored at 2 texels/m).
		float lmMetersPerTexel = 0.25f;
		uint32_t lmLobes = 12;       // 5|6|9|12 — PREFERRED max; the bake
		// selects the smallest count per mesh
		// whose fit residual fits the budget
		// (SgLightmapBakeConfig::lobeRelError)
// FIXED atlas width (texels). The atlas never shrinks below or grows
// past this width; content that does not fit the width lowers the
// density. The HEIGHT grows with content (rows appended at the
// bottom) up to --lm-max-height.
		uint32_t lmAtlasWidth = 1024;
		// Atlas height cap (texels): exceeding this makes density convergence
		// lower the density (or drop meshes at the floor). The default 8192
		// fits within typical 2D texture limits; test cooks can raise it
		// (uncompressed GPU cost: width * height * 72 B).
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
		// Voxel-remesh scene proxy (bake once on the proxy, UV-transfer to
		// the real meshes): merged world-space soup of all bake-eligible
		// meshes is remeshed with meshopt_remesh (solid, Solve) at this
		// voxel-grid resolution [4..256], then charted + baked once. Every
		// real mesh inherits the layout via TransferLightmapCharts
		// (closest-point projection onto the proxy). 0 = legacy per-mesh
		// LOD0 path (kept for A/B only).
		int lmProxyRes = 256;
		// Per-mesh lobe selection budget: relative RMS degradation from the
		// preferred-count reconstruction the mesh may cost by dropping
		// lobes. 0.15 keeps contrast-heavy meshes at 9-12 and lets smooth
		// ones drop lower; 0 pins everything at --lm-lobes.
		float lmLobeRelErr = 0.15f;
		// ---- LOD sibling cooking --------------------------------------------
		// Sibling simplification seam protection: "attr" uses UV attributes
		// as error terms (default); "hard" locks vertices along every seam
		// pair (less reduction). In the legacy per-mesh lightmap path, LOD
		// siblings cook before baking and inherit LOD0 charts via UV transfer.
		std::string lmLodLock = "attr";
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
		// World-sun color (linear RGB): with the angles/intensity above it IS
		// the cooked world sun — the bake env AND the pak runtime sun (a
		// directional is synthesized from it when the cluster has none).
		glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f };
		// Analytic sky IBL baked into the lightmap on open-sky rays
		// (gradient only, no sun disc). Default mirrors the runtime
		// EnvironmentSettings so covered pixels match the sky fallback
		// pixels see. 0 = sky-less field (old behavior).
		float skyIntensity = 600.0f;
		// Static spots for the bake: "px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela"
		std::vector<std::string> spots;
		// Ray-tracing backend for the in-pak bake transport (cvolumes,
		// cubemap captures, lightmaps): "embree" (vendored external/embree,
		// packet/SIMD traversal — the fast default) or "builtin" (the
		// deterministic binned-SAH BVH reference; agrees within
		// Monte-Carlo noise).
		std::string backend = "embree";
		// NOTE: no giProbes here (pak owns probe registration; a --gi-probe
		// CLI flag never took effect in cook and was removed).
		// ---- Preconvolved specular cubemaps (--cubemap on) ----
		// Captures the scene with the CPU path tracer at one or more points
		// (same transport as CubemapBaker), GGX-prefilters each capture for
		// the HIGH-frequency specular path (probes/lightmaps already carry
		// all diffuse + low-frequency specular), and embeds the KTX2 cubes
		// as Texture blobs at gi/cubemaps/<stem>.ktx2 — served by
		// PakBackend through the VFS, so CubemapLibrary::Load finds them
		// with no code change. The capture-set base is NAMELESS: derived
		// from the input artifact stem so pak.exe's side-car-stem
		// discovery (gi/cubemaps/<stem>.cubemanifest) always matches.
	bool cubemap = false;
		uint32_t cubemapSize = 128;   // base face width (HDR RGBA32F)
		uint32_t cubemapRays = 128;   // GGX importance samples per output texel
		uint32_t cubemapBounces = 5;  // diffuse path depth after the first hit
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
		// Pinned-probe influence (fade = half). Matches the SceneCubemap
		// authoring default (1.5m covered almost nothing - probes dropped
		// out of influence immediately); explicit --cubemap-extra-radius
		// still overrides per cook.
		float cubemapExtraRadius = 5.5f;
	// Debug PNGs next to the pak inputs (like the lightmap chart PNGs):
	// per-capture face strips (+ mip sheets) + a top-down probe map.
	// Loose files under <contentDir>/gi/cubemaps/, never in the pak.
	bool cubemapPng = true;
	bool cubemapPngMips = true;
	// Ambient closure (OFF by default): hit directions in the captures add
	// SkyAmbient * firstHitAlbedo on top of the traced radiance. That hack
	// matches the RUNTIME SKY-AMBIENT FALLBACK - the right compensation
	// for scenes WITHOUT GI volume data (the covered pixels' diffuse
	// ambient comes from the analytic sky there, and interior captures
	// would otherwise bake ~black with a probe-vs-fallback seam). With GI
	// volumes in the pak (the .sggi octree + cvols - the standard
	// pipeline: cook -> GiBaker -> pak) the runtime ambient comes from
	// the GI STACK, and the closure baked into the captures DOUBLE-COUNTS
	// it: every mirror-like surface (the chrome probe ball, gold trim)
	// reflects ~traced-bounce + closure ~= 2x the ambient-lit field. So
	// the closure is an explicit opt-in for stack-less cooks.
	bool cubemapAmbientClosure = false;
	// Weave transmission for MASK (cutout cloth) materials: the cloth
	// keeps its full lit albedo AND passes light through the weave,
	// tinted per texel by its baseColor texture (cutout holes pass
	// unobstructed; the tint stays real in white-world GI bakes - see
	// GiSceneBuildOptions::maskTransmissive + kMaskWeaveTransmittance).
	// Default OFF: MASK stays the opaque occluder (bistro foliage
	// approximates volume by its occlusion).
	bool maskTransmission = false;
		// NOTE: captures store TRUE radiance (physical units); the runtime's
		// baked path scales by the cubemap panel intensity only, matching the
		// direct sun view. No bake-time intensity knob by design.
		// Authored convex GI volumes (tour cvolume nodes) baked by THIS
		// cook (the --cvolume stage: in-process BakeCvolume, written
		// loose under gi/ + registered — pak embeds the same paths and
		// the runtime App resolves them out of the pak). Default on:
		// authoring the nodes IS the intent to bake them; --cvolume off
		// skips (GiBaker loose bakes stay the on-demand/hand-tuning
		// path).
		bool cvolume = true;
		// Quick-bakes mode (the editor's "Update bakes now" button): run the
		// capture stage ONLY and STOP — no pak write, no lightmap/LOD/collapse
		// stages. The loose bake files the stage writes
		// (gi/cubemaps/<base>[_N].ktx2 + <base>.cubemanifest) are the
		// output; they override the last pak's blobs through the runtime VFS
		// overlay by the same paths. Forces --cubemap on and the heavy stages
		// off; the pak from the last full cook stays untouched.
		bool quickBakes = false;
		// Read-only inspection: open an existing pak and print its manifest
		// (meshes + lmChunk, materials + texture URIs, textures). No cooking.
		std::string dumpPak;
		// Scene pinpoint: with --dump-pak, restrict the scene print to these
		// node ids (repeatable) and include their local translation.
		std::vector<uint32_t> dumpNodes;
		// Editor registry root (optional acceleration, NEVER a
		// precondition): when set, the cook consults + feeds the artifact
		// registry there — texture transcodes copy hash-validated payloads
		// from the registry on hit and stage fresh ones on miss.
		// Absent/broken registry = plain processing, unchanged output.
		std::string registryDir;
	};

	// Bake/pak sun+sky source of record (see SceneEnv): the scene's LUX-NATIVE
	// env block when the tour carried one (the editor always writes env.default.*),
	// else the CLI sun defaults (bare cooks). Every stage — the synthesized
	// WorldSun, the lightmap / cubemap / cvolume bakes — reads this, so ONE
	// authored sun/sky drives the editor viewport, the bakes and the pak.
	struct BakeEnv {
		float sunAzimuth = 30.0f, sunElevation = 55.0f;
		float sunIntensity = 10000.0f, skyIntensity = 600.0f;
		glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f };
	};
	BakeEnv EnvForBake(const smvk::Cluster& c, const Args& args) {
		BakeEnv e{};
		if (c.scene.envAuthored) {
			const SceneEnv& se = c.scene.environment;
			e.sunAzimuth = se.sunAzimuth;
			e.sunElevation = se.sunElevation;
			e.sunIntensity = se.sunIntensity;
			e.skyIntensity = se.skyIntensity;
			e.sunColor = se.sunColor;
		}
		else {
			e.sunAzimuth = args.sunAzimuth;
			e.sunElevation = args.sunElevation;
			e.sunIntensity = args.sunIntensity;
			e.skyIntensity = args.skyIntensity;
			e.sunColor = args.sunColor;
		}
		return e;
	}

	// World sun: the pak's first directional is the runtime sun (the App
	// seeds its env from it, first-wins). Clusters that already carry one
	// (imported KHR punctual or tour-created/edited) are untouched; sunless
	// scenes get a "WorldSun" node synthesized from the ENV BLOCK (see
	// EnvForBake — one authored sun everywhere) so the editor's environment
	// settings travel pak -> App. STATIC by design: the bake harvest
	// (CollectStaticBakeLights) picks up static lights only, so the
	// bake-time sun — direction from this node's -Z aim, color/intensity
	// from the env block — rides every bake transport (world field,
	// convex volumes, cubemap captures, lightmaps). The runtime still
	// renders it as the analytic/CSM sun: static-flagged DIRECTIONALS
	// are exempt from the bake-only rule (see ResolveSceneLights —
	// only punctual statics go bake-only), so nothing double-counts;
	// the old non-static choice starved every bake of the sun while
	// the 2D-lightmap double-count it feared is deprecated in-shader
	// and off in every shipped tour.
	void EnsureWorldSun(smvk::Cluster& cluster, const BakeEnv& be) {
		bool hasDir = false;
		cluster.lights.forEachHandle([&](smvk::ClusterLightHandle h) {
			if (const SceneLight* l = cluster.lights.get(h))
				if (l->type == SceneLight::Type::Directional)
					hasDir = true;
		});
		if (hasDir || cluster.scene.hierarchy.empty())
			return;
		// toSun from compass azimuth (0 = +Z) + elevation — the same formula
		// as EnvironmentSettings::SunDirectionToSun (unit by construction).
		const float az = be.sunAzimuth * 0.01745329252f;
		const float el = be.sunElevation * 0.01745329252f;
		const float ce = std::cos(el);
		const glm::vec3 toSun{ std::sin(az) * ce, std::sin(el),
			std::cos(az) * ce };
		// Node frame: the aim reads G*(0,0,-1) in EVERY coordFix mode
		// (GltfPunctualAimAxis; raw locals, the basis rides the chain top),
		// so local -Z maps to the emission direction -toSun — local +Z =
		// toSun (orthonormal basis, det +1). Under the conjugated Sponza
		// root the plain +Z read flips (az/el inverted on the seed — the
		// verify cook caught it); parent under the SCENE root (addNode -1)
		// instead, whose global frame is the raw identity, so the -Z read
		// is exact and the App decode matches this construction 1:1.
		const glm::vec3 z = toSun;
		glm::vec3 up(0.0f, 1.0f, 0.0f);
		const float facing = glm::dot(z, up);
		if (facing > 0.999f || facing < -0.999f)
			up = glm::vec3(1.0f, 0.0f, 0.0f);
		const glm::vec3 x = glm::normalize(glm::cross(up, z));
		const glm::vec3 y = glm::cross(z, x);
		glm::mat4 m(1.0f);
		m[0] = glm::vec4(x, 0.0f);
		m[1] = glm::vec4(y, 0.0f);
		m[2] = glm::vec4(z, 0.0f);
		const int node = addNode(cluster.scene, -1, 1);
		cluster.scene.localTransform[size_t(node)] = m;
		if (size_t(node) < cluster.scene.globalTransform.size())
			cluster.scene.globalTransform[size_t(node)] = m; // scene-root child: global == local
		setNodeName(cluster.scene, node, "WorldSun");
		SceneLight sun{};
		sun.type = SceneLight::Type::Directional;
		sun.color = be.sunColor;
		sun.intensity = be.sunIntensity;
		sun.name = "WorldSun";
		sun.flags |= SceneLight::kSceneLightFlag_Static;
		const auto h = cluster.CreateLight(std::move(sun));
		if (!h.valid())
			return;
		cluster.RegisterLightUri("sun/world", h);
		cluster.scene.lightForNode[uint32_t(node)] = h.index();
		tool::out() << "World sun synthesized (env block): az=" << be.sunAzimuth
			<< " el=" << be.sunElevation << " I=" << be.sunIntensity
			<< " rgb=(" << be.sunColor.x << "," << be.sunColor.y
			<< "," << be.sunColor.z << ")\n";
	}

	// Produce a filename-safe token for embedded cvol paths (manifest rows
	// use the first whitespace-delimited token as the path).
	std::string SanitizeVfsToken(const std::string& s) {
		std::string out;
		for (char c : s) {
			if (std::isalnum(static_cast<unsigned char>(c))
				|| c == '_' || c == '-' || c == '.')
				out.push_back(c);
			else
				out.push_back('_');
		}
		return out.empty() ? std::string("cvol") : out;
	}

	// Reads a loose bake file by its content-root-relative vfs path
	// ("gi/<scene>.cvmanifest" -> <contentDir>/gi/<scene>.cvmanifest). Empty
	// on any failure (logged by the caller — a missing bake file is "not
	// baked", never fatal).
	std::vector<uint8_t> ReadLooseBakeFile(const std::string& contentDir,
		const std::string& vfsPath) {
		std::error_code ec;
		const std::filesystem::path host =
			std::filesystem::path(contentDir) / vfsPath;
		if (!std::filesystem::is_regular_file(host, ec))
			return {};
		std::ifstream in(host, std::ios::binary);
		if (!in)
			return {};
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
	}

	// Bake-file name stems (see the GI bake-file discovery in main): the pak
	// stem first (the runtime's fallback identity), then the source scene
	// stems (the artifact list - every listed .geometry, or the tour
	// reference identity). Bakes key on the SCENE they ran over
	// (GiBaker --scene, the .sggi stem) — without the scene stems a
	// differently-named out pak would silently miss its own bakes.
	std::vector<std::string> BakeStems(const Args& args) {
		std::vector<std::string> stems;
		const std::string pakStem =
			std::filesystem::path(args.outPak).stem().generic_string();
		if (!pakStem.empty())
			stems.push_back(pakStem);
		auto push = [&](const std::string& src) {
			if (src.empty())
				return;
			const std::string srcStem =
				std::filesystem::path(src).stem().generic_string();
			if (!srcStem.empty()
				&& std::find(stems.begin(), stems.end(), srcStem)
					== stems.end())
				stems.push_back(srcStem);
		};
		for (const std::string& art : args.inVfsList)
			push(art);
		push(args.inVfs); // tour-reference identity (set in main)
		if (stems.empty())
			stems.push_back("pak");
		return stems;
	}

	// Authored convex GI volumes baked by THIS cook (the --cvolume stage):
	// the same engine core as GiBaker (BuildTraceSceneFromCluster +
	// BakeCvVolume over the tour's cvolume nodes — identical bake inputs:
	// no collapse, no LODs, the definition cluster exactly as GiBaker sees
	// it). Output = (vfsPath, bytes) pairs for loose write + registration:
	// one .cvol per volume at gi/<pak-stem>_<name>.cvol + the runtime
	// manifest text at gi/<pak-stem>.cvmanifest (bare-path rows: the
	// runtime's SplitManifestRow + vfs.ReadBinary resolve them out of the
	// pak through PakBackend's serve-Texture-by-path — the proven
	// cubemap-manifest embed pattern). Returns false on a bake failure —
	// the cook fails loudly (a silently missing GI volume is exactly the
	// "cooked cvolumes never reached the App" bug).
	bool BakeAuthoredCvolumes(
		std::vector<std::unique_ptr<smvk::Cluster>>& sceneClusters,
		const Args& args, const std::string& sceneId,
		std::vector<std::pair<std::string, std::vector<uint8_t>>>& out) {
		using namespace smvk::gi;
		std::vector<std::string> manifestRows;
		for (auto& sc : sceneClusters) {
			if (!sc || sc->scene.cvolForNode.empty())
				continue;
			// Trace scene per cluster: bake inputs mirror the lightmap/
			// cubemap stages (texture-averaged albedo, cooked-punctual aim
			// axis, the env-block sun/sky record (EnvForBake — ONE authored
			// sun/sky everywhere), static light harvest) so the volume
			// matches what the rest of the cook bakes.
			const BakeEnv be = EnvForBake(*sc, args);
			GiSceneBuildOptions buildOpts{};
			uint32_t averaged = 0;
			uint32_t textured = 0;
			for (const auto& [uri, handle] : sc->ListTextureUris()) {
				const std::vector<uint8_t>* src = sc->GetTextureSource(uri);
				if (!src || src->empty())
					continue;
				std::optional<glm::vec3> avg =
					GiAverageKtxTexel(std::span<const uint8_t>(*src));
				if (!avg)
					continue;
				buildOpts.texelAvg[handle.index()] = *avg;
				++averaged;
				// Full decoded texel data: the trace samples the baseColor
				// texture at the interpolated hit UV (AlbedoAt) — volume
				// bakes carry the texture detail, not flat averages.
				if (auto data = GiDecodeKtxTexelData(
						std::span<const uint8_t>(*src), kGiBakeTexelMaxDim)) {
					buildOpts.texelData[handle.index()] = std::move(*data);
					++textured;
				}
			}
			buildOpts.punctualAimAxis = GltfPunctualAimAxis(0);
			buildOpts.env.sunAzimuthDeg = be.sunAzimuth;
			buildOpts.env.sunElevationDeg = be.sunElevation;
			buildOpts.env.sunIntensity = be.sunIntensity;
			buildOpts.env.skyIntensity = be.skyIntensity;
			buildOpts.traceBackendEmbree = (args.backend == "embree");
			// Weave-transmissive cloth (--mask-transmission): the volume
			// bakes see the tinted light pools behind the banners.
			buildOpts.maskTransmissive = args.maskTransmission;
			GiTraceScene traceScene;
			std::string terr;
			BuildTraceSceneFromCluster(*sc, buildOpts, traceScene, terr);
			if (!terr.empty()) {
				tool::err() << "[CV] trace scene failed: " << terr << std::endl;
				return false;
			}
			tool::out() << "  [CV] trace scene: "
				<< traceScene.Bvh().TriangleCount() << " tris ("
				<< averaged << " averaged, " << textured
				<< " decoded textures)\n" << std::flush;
			// Deterministic node order (hash-map iteration is not).
			std::vector<uint32_t> nodes;
			nodes.reserve(sc->scene.cvolForNode.size());
			for (const auto& [node, def] : sc->scene.cvolForNode)
				nodes.push_back(node);
			std::sort(nodes.begin(), nodes.end());
			for (uint32_t node : nodes) {
				if (node >= sc->scene.globalTransform.size())
					continue;
				const SceneCvol& cv = sc->scene.cvolForNode[node];
				// World box decode (GiBaker parity): translation = center,
				// basis lengths = full extents, normalized basis = the box
				// axes (an OBB — six planes, CvBoxPlanesRotated).
				const glm::mat4& g = sc->scene.globalTransform[node];
				const glm::vec3 c(g[3]);
				glm::vec3 half;
				glm::mat3 axes;
				for (int i = 0; i < 3; ++i) {
					const float len = glm::length(g[i]);
					half[i] = 0.5f * len;
					axes[i] = (len > 1e-6f) ? glm::vec3(g[i]) / len
						: glm::vec3(0.0f);
				}
				std::string name = getNodeName(sc->scene, int(node));
				if (name.empty())
					name = "cvol" + std::to_string(node);
				name = SanitizeVfsToken(name);
				CvVolumeDef def{};
				def.name = name;
				def.priority = cv.priority;
				def.planes = CvBoxPlanesRotated(c, axes, half,
					cv.insideFade, cv.outsideFade);
				tool::out() << "  [CV] volume '" << def.name << "': priority="
					<< def.priority << " spacing=" << cv.spacing
					<< " planes=" << def.planes.size() << "\n" << std::flush;
				// Engine defaults (1536 capture dirs, 3 bounces, auto
				// capture lattice, HW threads): GiBaker stays the tuning
				// tool for anything finer.
				CvBakeConfig cfg{};
				CvVolume vol;
				CvBakeStats stats;
				if (!BakeCvVolume(traceScene, def, cv.spacing, cfg, vol,
					stats, [](const char* line) { tool::out() << line; })) {
					tool::err() << "[CV] bake failed for '" << def.name
						<< "'\n";
					return false;
				}
				std::vector<uint8_t> bytes;
				std::string werr;
				if (!vol.WriteToMemory(bytes, werr)) {
					tool::err() << "[CV] serialize failed for '" << def.name
						<< "': " << werr << "\n";
					return false;
				}
				const std::string vfsPath =
					"gi/" + sceneId + "_" + def.name + ".cvol";
				out.emplace_back(vfsPath, std::move(bytes));
				manifestRows.push_back(vfsPath + "  # " + def.name);
				tool::out() << "  [CV] baked '" << def.name << "': dims="
					<< vol.dims[0] << "x" << vol.dims[1] << "x" << vol.dims[2]
					<< " probes=" << stats.probes
					<< " (" << stats.seconds << "s)\n";
			}
		}
		if (manifestRows.empty())
			return true; // no authored cvolumes: nothing to embed
		// Runtime manifest: bare-path rows (SplitManifestRow takes the first
		// token; the '# name' notes are ignored by the parser, read by us).
		std::string manifest =
			"# convex GI volumes (cooked into this pak; pak vfs paths)\n";
		for (const std::string& row : manifestRows)
			manifest += row + "\n";
		out.emplace_back("gi/" + sceneId + ".cvmanifest",
			std::vector<uint8_t>(manifest.begin(), manifest.end()));
		return true;
	}

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
					tool::err() << "Ignoring out-of-range lod ratio: " << tok << std::endl;
			}
			catch (...) {
				tool::err() << "Ignoring bad lod ratio: " << tok << std::endl;
			}
			if (comma == std::string::npos)
				break;
			pos = comma + 1;
		}
		return out;
	}

	bool ParseArgs(int argc, char** argv, Args& out) {
		// POSITIONAL grammar (pak.exe <vfs_path> <artifact...> [--options]):
		// the first positional = the VFS content root (was --content-dir),
		// every positional after it until the first flag = one
		// ALREADY-PROCESSED .geometry artifact (was the single --in-vfs).
		// Raw glTF NEVER cooks here - import.exe (tools/import) / the
		// editor's MeshImporter produce the artifacts; this is the clean
		// split: ONE raw-import place, ONE artifact-consumer place.
		// --tour stays a flag (the editor-authored scene overlay); with a
		// tour the artifact list may be empty (the tour's references are
		// the definition).
		std::vector<std::string> positional;
		bool sawFlag = false;
		for (int i = 1; i < argc; ++i) {
			const std::string a = argv[i];
			auto need = [&](std::string& dst) {
				if (i + 1 >= argc) {
					tool::err() << "Missing value for " << a << std::endl;
					return false;
				}
				dst = argv[++i];
				return true;
			};
			if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
				// The flag chain below parses this argv (the positionals
				// end at the first flag).
				sawFlag = true;
			}
			else if (!sawFlag) {
				positional.push_back(a);
				continue;
			}
			else {
				tool::err() << "Artifact '" << a << "' after flags: all"
					" artifacts come before the first --option"
					<< std::endl;
				return false;
			}
			if (a == "--tour") {
				if (!need(out.tour)) return false;
			}
			else if (a == "--out-pak") {
				// Stem source for bake-file names (cvol manifest +
				// volumes, cubemap base fallback): the pak this cook
				// feeds. No pak is written (pak.exe assembles it) —
				// without it the cvolume fallback mints stemless
				// "pak.*" names no discovery will ever find.
				if (!need(out.outPak)) return false;
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
			else if (a == "--lods") {
				std::string v;
				if (!need(v)) return false;
				out.lods = (v == "on" || v == "1" || v == "true");
				out.explicitSet.insert("lods");
			}
			else if (a == "--lod-ratios") {
				if (!need(out.lodRatios)) return false;
				out.explicitSet.insert("lodRatios");
			}
			else if (a == "--lod-error") {
				std::string v;
				if (!need(v)) return false;
				out.lodError = std::stof(v);
				out.explicitSet.insert("lodError");
			}
			else if (a == "--meshlets") {
				std::string v;
				if (!need(v)) return false;
				out.meshlets = (v == "on" || v == "1" || v == "true");
				out.explicitSet.insert("meshlets");
			}
			else if (a == "--collapse-static") {
				std::string v;
				if (!need(v)) return false;
				out.collapseStatic = (v == "on" || v == "1" || v == "true");
			}
			else if (a == "--auto-ktx") {
				std::string v;
				if (!need(v)) return false;
				out.autoKtx = (v == "on" || v == "1" || v == "true");
				out.explicitSet.insert("autoKtx");
			}
			else if (a == "--scratch-dir") {
				// Removed: the project cache joins by convention now
				// (<contentDir>/.tourmaline when it carries a cache.db),
				// never by flag. Reject explicitly rather than silently
				// ignoring a stale script flag.
				tool::err() << "--scratch-dir was removed (project cache"
					" joins by content-root convention; the editor still"
					" passes its store programmatically)\n";
				return false;
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
			else if (a == "--lm-proxy-res") {
				std::string v;
				if (!need(v)) return false;
				out.lmProxyRes = std::clamp(std::stoi(v), 0, 256);
				out.explicitSet.insert("lmProxyRes");
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
			else if (a == "--backend") {
				std::string v;
				if (!need(v)) return false;
				if (v != "builtin" && v != "embree") {
					tool::err() << "--backend must be builtin|embree" << std::endl;
					return false;
				}
				out.backend = v;
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
			else if (a == "--sun-color") {
				std::string v;
				if (!need(v)) return false;
				for (char& c : v)
					if (c == ',')
						c = ' ';
				std::istringstream in(v);
				glm::vec3 c = out.sunColor;
				if (in >> c.x >> c.y >> c.z)
					out.sunColor = c;
				else {
					tool::err() << "Bad --sun-color (want r,g,b): " << v << std::endl;
					return false;
				}
				out.explicitSet.insert("sunColor");
			}
			else if (a == "--spot") {
				std::string v;
				if (!need(v)) return false;
				out.spots.push_back(v);
			}
			else if (a == "--gi-probe") {
				// Removed: never honored here (parsed but unread — pak
				// owns probe registration). Reject explicitly rather
				// than silently ignoring a stale script flag.
				tool::err() << "--gi-probe was removed from cook (it never"
					" took effect here; register probes with pak --gi-probe"
					" instead)\n";
				return false;
			}
			else if (a == "--cvolume") {
				std::string v;
				if (!need(v)) return false;
				out.cvolume = (v == "on" || v == "1" || v == "true");
			}
			else if (a == "--quick-bakes") {
				std::string v;
				if (!need(v)) return false;
				out.quickBakes = (v == "on" || v == "1" || v == "true");
			}
			else if (a == "--cubemap") {
				std::string v;
				if (!need(v)) return false;
				out.cubemap = (v == "on" || v == "1" || v == "true");
				out.explicitSet.insert("cubemap");
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
				out.explicitSet.insert("cubemapOctree");
			}
			else if (a == "--cubemap-octree-max") {
				std::string v;
				if (!need(v)) return false;
				out.cubemapOctreeMax = std::max(0.5f, std::stof(v));
				out.explicitSet.insert("cubemapOctreeMax");
			}
			else if (a == "--cubemap-octree-min") {
				std::string v;
				if (!need(v)) return false;
				out.cubemapOctreeMin = std::max(0.25f, std::stof(v));
				out.explicitSet.insert("cubemapOctreeMin");
			}
			else if (a == "--cubemap-octree-depth") {
				std::string v;
				if (!need(v)) return false;
				out.cubemapOctreeDepth = std::clamp<uint32_t>(
					static_cast<uint32_t>(std::stoul(v)), 1, 6);
				out.explicitSet.insert("cubemapOctreeDepth");
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
				out.explicitSet.insert("cubemapExtraRadius");
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
			else if (a == "--cubemap-ambient-closure") {
				std::string v;
				if (!need(v)) return false;
				out.cubemapAmbientClosure = (v == "on" || v == "1" || v == "true");
				out.explicitSet.insert("cubemapAmbientClosure");
			}
			else if (a == "--mask-transmission") {
				std::string v;
				if (!need(v)) return false;
				out.maskTransmission = (v == "on" || v == "1" || v == "true");
				out.explicitSet.insert("maskTransmission");
			}
			else if (a == "--cubemap-ibl-intensity") {
				// Removed: captures store true radiance by design (the runtime
				// scales with the cubemap panel intensity). Reject explicitly
				// rather than silently ignoring a stale script flag.
				tool::err() << "--cubemap-ibl-intensity was removed (captures are"
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
			tool::err() << "Unknown arg: " << a << std::endl;
			return false;
		}
	}
	// Positionals: [0] = the VFS root, [1..] = the artifact list.
	if (!out.dumpPak.empty())
		return true; // inspection mode needs no cook inputs
	if (positional.empty()) {
		tool::err() << "Required: cook.exe <vfs_path> <artifact...> [--options]"
			" - the VFS content root + the processed .geometry list"
			" (import raw glTF with import.exe / the editor first)"
			<< std::endl;
		return false;
	}
	out.contentDir = positional[0];
	if (positional.size() > 1) {
		out.inVfsList.assign(positional.begin() + 1, positional.end());
		out.inVfs = out.inVfsList.front();
	}
	if (out.inVfsList.empty() && out.tour.empty()) {
		tool::err() << "Required: at least one artifact .geometry (or --tour"
			" <scene.tour> - the editor-authored definition)" << std::endl;
		return false;
	}
	// Cook writes cooked artifacts in place, never paks (pak.exe owns
	// the pak write). --out-pak names the DESTINATION pak for bake
	// file stems only (cvol manifest/volumes, cubemap base).
	// --tour is the DEFINITION (self-contained: the source glTF rides
	// as root reference #0) — no artifact inference needed.
	return true;
}

	void Usage() {
	tool::out() << "cook.exe <vfs_path> <artifact...> [--options]\n"
		<< "          cook.exe F:/content models/sponza/Sponza.geometry\n"
		<< "              [--lods on|off] [...] (bakes write back in place;\n"
		<< "              no pak write — pak.exe assembles it;\n"
		<< "              --out-pak <path> names the destination pak\n"
		<< "              for bake file stems only)\n"
			<< "          (positional grammar: the VFS content root + the\n"
			<< "          ALREADY-PROCESSED .geometry artifact list — glTF is\n"
			<< "          IMPORT-ONLY (import.exe <input> / the editor's\n"
			<< "          MeshImporter derives the artifacts); the artifacts\n"
			<< "          carry the scenes; the pak cooks ALL of them.)\n"
			<< "          [--tour <scene.tour>] [--overdraw 1.05]\n"
			<< "          [--lods on|off] [--lod-ratios 0.75,0.5,0.3,0.15] [--lod-error 0.01]\n"
		<< "          [--meshlets on|off] (setup-time; fixed 64v/64t canon)\n"
		<< "          [--auto-ktx on|off]\n"
			<< "          [--collapse-static on|off]\n"
			<< "          [--lightmap on|off --lm-meters-per-texel 0.25 --lm-lobes 12\n"
			<< "           --lm-atlas-width 1024 --lm-max-height 8192\n"
			<< "           --lm-gutter 1 --lm-rays 1024\n"
			<< "           --lm-proxy-res 256 (voxel-remesh scene proxy; 0 = legacy per-mesh)\n"
			<< "           --lm-compress bc6hopt|bc6h|uncompressed\n"
			<< "           --lm-bounces 3\n"
			<< "           --sun-azimuth 30 --sun-elevation 55\n"
			<< "           --sun-intensity 10000 --sky-intensity 600\n"
			<< "           --backend builtin|embree (bake ray-tracing backend)\n"
			<< "           --sun-color 1,0.95,0.9\n"
			<< "           --lm-lod-lock attr|hard\n"
			<< "           --lm-debug-pink on|off\n"
			<< "           --lm-stitch on|off\n"
			<< "           --lm-curve-target 7.0 --lm-curve-max 2.5\n"
			<< "           --lm-exclude tree,linden,ivy,glass --lm-exclude-small 0.5\n"
			<< "           --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]\n"
			<< "          (world sun: when the scene carries no directional light,\n"
			<< "           one is synthesized from the sun settings above, so the\n"
			<< "           editor world sun travels pak -> App)\n"
			<< "          [--gi-probe <name> ...]: registers probe bake names\n"
			<< "          (gi/<name>.sggi, baked separately) into the pak; the\n"
			<< "          lightmap atlas registers itself on --lightmap. Runtime\n"
			<< "          resolves GI by these names (one per kind for now).\n"
			<< "          [--cvolume on|off (default on)]: bakes the tour's\n"
			<< "          authored convex GI volumes (cvolume nodes) in-process\n"
			<< "          (same engine core as GiBaker) and embeds them into\n"
			<< "          the pak as Texture blobs under gi/ (volumes at\n"
			<< "          gi/<pak-stem>_<name>.cvol, the runtime manifest at\n"
			<< "          gi/<pak-stem>.cvmanifest) — the pak is self-contained\n"
			<< "          GI for the App; loose GiBaker bakes still override\n"
			<< "          through the overlay when present.\n"
			<< "          [--cubemap on|off\n"
			<< "           --cubemap-size 128 --cubemap-rays 128 --cubemap-bounces 5\n"
			<< "           --cubemap-pos x,y,z (repeatable) --cubemap-height 1.5\n"
			<< "           --cubemap-grid 8.0 --cubemap-octree on\n"
			<< "           --cubemap-octree-max 8.0 --cubemap-octree-min 2.0\n"
			<< "           --cubemap-octree-depth 4\n"
			<< "           --cubemap-extra-pos x,y,z (repeatable, pinned)\n"
			<< "           --cubemap-extra-radius 1.5\n"
			<< "           --cubemap-spp 8\n"
			<< "           --cubemap-ambient-closure on|off (default OFF: the\n"
			<< "            GI stack (.sggi + cvols) owns the ambient; ON only\n"
			<< "            for stack-less cooks - it matches the runtime sky\n"
			<< "            fallback ambient, but double-counts with the stack)\n"
			<< "           --cubemap-png on|off --cubemap-png-mips on|off]:\n"
			<< "          preconvolved SPECULAR cubemaps baked into the pak\n"
			<< "          (GGX importance-sampled mip chains, roughness = mip /\n"
			<< "          (levels-1); probes/lightmaps own diffuse + low-freq\n"
			<< "          specular). Captures store true radiance (the runtime\n"
			<< "          scales them with the cubemap panel intensity, so a\n"
			<< "          mirror matches the sun-lit scene; no bake-time\n"
			<< "          intensity is frozen in). One capture per\n"
			<< "          --cubemap-pos (default: scene-bounds center at +height).\n"
			<< "          If --cubemap-grid > 0, captures a uniform 3D grid\n"
			<< "          of probes at that spacing (meters) within the scene\n"
			<< "          bounds; --cubemap-octree on replaces the lattice with\n"
			<< "          sparse adaptive placement (open volumes stay coarse,\n"
			<< "          near-surface cells refine to --cubemap-octree-min at\n"
			<< "          depth --cubemap-octree-depth, embedded centers drop,\n"
			<< "          growth stops at 32 probes). --cubemap-extra-pos\n"
			<< "          appends artist-PINNED probes after placement (manifest\n"
			<< "          'pin' rows sort first at load, so they win weight ties\n"
			<< "          -- e.g. a capture at the chrome ball; influence\n"
			<< "          --cubemap-extra-radius, fade half). --cubemap-spp\n"
			<< "          averages N transport paths per texel so the bounce\n"
			<< "          field is coherent GI (1 spp = speckle), matching the\n"
			<< "          multi-ray lightmap look. Multi captures write\n"
			<< "          gi/cubemaps/<stem>.cubemanifest (positions +\n"
			<< "          radii) next to the KTX blobs; embedded as Texture\n"
			<< "          blobs at gi/cubemaps/<stem>[_N].ktx2 (the base is\n"
			<< "          NAMELESS: the input artifact's stem - pak.exe\n"
			<< "          discovers + embeds by that stem). Debug PNGs\n"
			<< "          (<name>_N.png faces, <name>_N_mips.png prefilter stack,\n"
			<< "          <name>_probes.png top-down placement map) land loose\n"
			<< "          in <contentDir>/gi/cubemaps/. Needs exactly\n"
			<< "          one cooked scene, like --lightmap.\n"
			<< "--lightmap on: bakes 2D SG lightmaps into the pak (per-vertex\n"
			<< "lightmap UVs + per-mesh chunk id; atlas written to\n"
			<< "<contentDir>/gi/<scene>.sglm). Sources: sun + static spots +\n"
			<< "real materials; transmissive (Blend) meshes pass light through.\n"
			<< "--lm-meters-per-texel is a WORLD-space request (m/texel; 0.25 =\n"
			<< "one texel per 25 cm = 4 texels/m; scaled nodes like sponza's\n"
			<< "0.008 root convert through their world scale). The atlas\n"
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
			<< "--quick-bakes on: capture-only refresh (the editor's 'Update\n"
			<< "bakes now' button): runs the --cubemap capture stage and stops -\n"
			<< "the loose gi/cubemaps/<base>[_N].ktx2 + <base>.cubemanifest\n"
			<< "bake files ARE the output (they override the last pak's blobs\n"
			<< "through the runtime VFS overlay). No pak is written; the\n"
			<< "lightmap/LOD/collapse/cvolume stages are forced off. The\n"
			<< "capture set is NAMELESS - the artifact stem names it.\n"
			<< "--tour <scene.tour>: replay the editor's ASCII command overlay\n"
			<< "(renames, transforms, materials, no-lightmap flags, reparents)\n"
			<< "onto the built cluster before cooking. Tour cook settings\n"
			<< "fill flags the CLI did not set (CLI wins). Deterministic:\n"
			<< "the tour (or --in-vfs names the derived .geometry — glTF is\n"
			<< "IMPORT-ONLY, never a cook input; the editor's open/drop of a\n"
			<< ".gltf runs the MeshImporter and derives the artifacts beside\n"
			<< "the source) + identical pak out." << std::endl;
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
		uint64_t u64() {
			uint64_t v = 0;
			if (off + 8 > img.size()) ok = false; else { memcpy(&v, img.data() + off, 8); off += 8; }
			return v;
		}
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
		tool::out() << "  manifest (pak-sourced):\n";

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
				tool::out() << "    [truncated mesh blob: " << e->path << "]\n";
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
				tool::out() << "    [truncated mesh blob: " << e->path << "]\n";
				continue;
			}
			// ---- stream layout (packed-SoA; see cluster_serializer.cpp) ----
			// Mesh blobs: [u32 layout marker (=1) | u64 posOff | u64 shadeOff
			// | u64 idxOff | VertexPos[] | VertexShading[] | u32[]]. The
			// shading stream carries chartUv PACKED (RG16_UNORM at shade+8)
			// and chart (u32 at shade+20); positions are float3 at pos,
			// stride 12.
			size_t posBase = 0, shadeBase = 0, idxBase = 0;
			{
				const uint32_t layout = c.u32();
				const uint64_t posOff = c.u64();
				const uint64_t shadeOff = c.u64();
				const uint64_t idxOff = c.u64();
				if (!c.ok || layout != 1u
					|| (size_t)posOff + (size_t)vc * sizeof(smvk::VertexPos) > c.img.size()
					|| (size_t)shadeOff + (size_t)vc * sizeof(smvk::VertexShading) > c.img.size()
					|| (size_t)idxOff + (size_t)ic * 4 > c.img.size()) {
					tool::out() << "    [truncated mesh blob: " << e->path << "]\n";
					continue;
				}
				posBase = (size_t)posOff;
				shadeBase = (size_t)shadeOff;
				idxBase = (size_t)idxOff;
			}
			// chart-local uv + chart id probe (SoA shading stream).
			auto chartAt = [&](size_t v) -> std::tuple<float, float, uint32_t> {
				const uint8_t* p = c.img.data() + shadeBase
					+ v * sizeof(smvk::VertexShading);
				uint32_t cuv = 0, cid = 0;
				memcpy(&cuv, p + 8, 4);
				memcpy(&cid, p + 20, 4);
				return { float(cuv & 0xFFFFu) / 65535.0f,
					float(cuv >> 16) / 65535.0f, cid };
			};
			// Vertex-attribute probe shared by the classic-index walk and
			// the meshlet walk below: EVERY attribute must come from the
			// SAME final vertex (the meshlet audit's whole point).
			auto atlasAt = [&](size_t v) -> std::pair<float, float> {
				const auto [cu, cv, cid] = chartAt(v);
				if (cid != 0xFFFFFFFFu && (size_t)cid < lmXf.size() / 4) {
					const float* xf = lmXf.data() + (size_t)cid * 4;
					return { cu * xf[0] + xf[2], cv * xf[1] + xf[3] };
				}
				return { cu, cv };
			};
			// ---- atlas-uv sweep audit (lightmap seam detector) ----
			// Atlas uvs resolve on the fly as chartUv * xform.scale +
			// xform.offset (same math as the VS unpack). A triangle spanning
			// distant atlas regions flags a stamping bug; within one chart
			// rect a triangle spans at most 256/atlasW + 256/atlasH (charts
			// are capped at 256x256 texels; tall/short atlases make that
			// aspect-dependent, e.g. 2047x871 -> 0.42), so 0.45 flags only
			// genuine cross-rect sweeps.
			float maxUv2Span = 0.0f;
			uint32_t bigUv2Tris = 0;
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
			for (uint32_t v = 0; v < vc; ++v) {
				const auto [cu0, cv0, cid] = chartAt(v);
				(void)cu0; (void)cv0;
				if (cid != 0xFFFFFFFFu) {
					haveChartId = true;
					chartIdMax = std::max(chartIdMax, cid);
				}
			}
			if (lmChunk != 0xFFFFFFFFu && idxBase + (size_t)ic * 4 <= c.img.size()) {
				for (uint32_t t = 0; t * 3 + 3 <= ic; ++t) {
					uint32_t ix[3] = {};
					memcpy(ix, c.img.data() + idxBase + (size_t)t * 12, 12);
					for (int k = 0; k < 3; ++k) {
						if (ix[k] >= vc)
							continue;
						const auto [cu1, cv1, cid] = chartAt(ix[k]);
						(void)cu1; (void)cv1;
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
					memcpy(p, c.img.data() + posBase
						+ v * sizeof(smvk::VertexPos), 12);
					return glm::vec3(p[0], p[1], p[2]);
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
						const auto [cu, cv, cid] = chartAt(ix[k]);
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
			// Payload skip to the meshlet trailer (recorded stream offsets).
			c.off = idxBase + (size_t)ic * 4;
			uint32_t meshlets = 0;
			if (c.off < c.img.size()) {
				if (c.off + 4 <= c.img.size())
					meshlets = c.u32();
				else
					c.ok = false;
			}
			if (!c.ok) {
				tool::out() << "    [truncated mesh blob: " << e->path << "]\n";
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
					}
					else {
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
					}
					else {
						c.ok = false;
					}
				}
				if (!c.ok) {
					mlTruncated = true;
				}
				else {
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
				tool::out() << "    [truncated meshlet trailer: " << e->path
					<< "]\n";
				continue;
			}
			tool::out() << "    mesh " << uri
				<< " verts=" << vc << " idx=" << ic
				<< " lod{g=" << groupId << " i=" << lodIndex << " n=" << candidates
				<< " tris=" << tris << " err=" << simplifyError
				<< " acmr=" << acmr << "}"
				<< " meshlets=" << meshlets
				<< " lmChunk=" << (lmChunk == 0xFFFFFFFFu ? -1 : int(lmChunk));
			if (lmChunk != 0xFFFFFFFFu)
				tool::out() << " charts=" << lmXforms
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
				tool::out() << " mlbigtris=" << mlBigTris
				<< " mlsetdiff=" << mlSetDiff
				<< " mlbadlocal=" << mlBadLocal
				<< " mloob=" << mlOob;
			tool::out() << "\n";
		}

		// Materials (blob: str uri | 128B MaterialValues | u32 shading |
		// u32 alpha | f32 cutoff | u32 doubleSided | 5x str texUris
		// (baseColor, metallicRoughness, normal, occlusion, emissive) |
		// u32 castsShadows).
		for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Material)) {
			DumpCursor c{ reader.Payload(*e), 0 };
			const std::string uri = c.str();
			if (c.off + 128 > c.img.size()) {
				tool::out() << "    mat  " << uri << " [truncated values]\n";
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
				tool::out() << "    mat  " << uri << " [truncated blob]\n";
				continue;
			}
			tool::out() << "    mat  " << uri
				<< " shading=" << shading << " alpha=" << alpha
				<< " cutoff=" << cutoff << " ds=" << ds
				<< " cast=" << cs
				<< " metal=" << metal << " rough=" << rough << "\n";
			static const char* kNames[5] = { "bc", "mr", "n", "occ", "e" };
			for (int k = 0; k < 5; ++k) {
				tool::out() << "      " << kNames[k] << " slot=" << slots[k]
					<< " uri=" << (texUris[k].empty() ? "<none>" : texUris[k]) << "\n";
			}
		}

		// GI registry (named bake files: kind + stem).
		for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Gi)) {
			std::vector<smvk::GiBinding> bindings;
			std::string gerr;
			if (!smvk::ParseGiRegistryBlob(reader.Payload(*e), bindings, gerr)) {
				tool::out() << "    [malformed gi registry blob: " << e->path << "]\n";
				continue;
			}
			for (const auto& b : bindings) {
				tool::out() << "    gi   "
					<< (b.kind == smvk::GiBinding::Kind::Lightmap ? "lightmap" : "probe")
					<< " '" << b.name << "'\n";
			}
		}

		// Textures (embedded .ktx2 byte sizes).
		for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Texture)) {
			smvk::PakTexturePayload p;
			if (smvk::ParseTexturePayload(reader.Payload(*e), p)) {
				tool::out() << "    tex  " << p.uri << " (" << p.ktxBytes.size() << " B)\n";
			}
			else {
				tool::out() << "    [malformed texture blob: " << e->path << "]\n";
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
				tool::out() << "    [truncated light blob: " << e->path << "]\n";
				continue;
			}
			const char* tname = type == 0 ? "dir" : (type == 2 ? "spot" : "point");
			tool::out() << "    light " << uri << " '" << name << "' " << tname
				<< " rgb=(" << r << "," << g << "," << b << ")"
				<< " I=" << intensity << " range=" << range;
			if (type == 2)
				tool::out() << " inner=" << inner << " outer=" << outer;
			tool::out() << "\n";
		}

		// Cameras (blob: str uri | u32 projection | f32 fovY | f32 orthoHalfH
		// | f32 near | f32 far | str name).
		for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Camera)) {
			DumpCursor c{ reader.Payload(*e), 0 };
			const std::string uri = c.str();
			const uint32_t proj = c.u32();
			const float fovY = c.f32(), halfH = c.f32();
			const float nearClip = c.f32(), farClip = c.f32();
			const std::string name = c.str();
			if (!c.ok || proj > 1) {
				tool::out() << "    [truncated camera blob: " << e->path << "]\n";
				continue;
			}
			tool::out() << "    camera " << uri << " '" << name << "' "
				<< (proj == 1 ? "ortho" : "persp")
				<< " fovY=" << fovY << " halfH=" << halfH
				<< " near=" << nearClip << " far=" << farClip << "\n";
		}

		// Scene graph (names + parent links + mesh/material/light/camera pak
		// ids + noLM). Layout mirrors cluster_serializer.cpp: u32 nodes |
		// hierarchy (5xi32) | localTransforms (16xf32) | meshPairs | matPairs
		// | namePairs | nodeNames | matNames | static section | noLM (v3) |
		// light pairs (v4) | name (v5) | dither (v6) | visibility (v7) |
		// camera pairs (v8; definitions are Camera blobs).
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
				tool::out() << "    [bad scene blob]\n";
				continue;
			}
			const auto infoIt = sceneInfo.find(e->path);
			const size_t dispIdx = infoIt != sceneInfo.end()
				? infoIt->second.first : sceneIdx;
			const std::string dispName = infoIt != sceneInfo.end()
				? infoIt->second.second : std::string{};
			++sceneIdx;
			tool::out() << "    scene[" << dispIdx << "] '" << dispName
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
			std::unordered_map<uint32_t, uint32_t> camMap; // node -> camera pak id
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
						// ... then the v7 visibility section (skip: u32 + bytes).
						if (c.ok && c.off < c.img.size()) {
							const uint32_t hc = c.u32();
							if (c.ok && hc == nodeCount &&
								c.off + hc <= c.img.size())
								c.off += hc;
							else if (c.ok)
								c.ok = false;
						}
						// ... then the v8 camera pairs (node, camPakId).
						if (c.ok && c.off < c.img.size()) {
							const uint32_t camPairs = c.u32();
							if (!c.ok || camPairs > 1024 * 1024)
								c.ok = false;
							else {
								for (uint32_t ci = 0; ci < camPairs && c.ok; ++ci) {
									const uint32_t n = c.u32(), id = c.u32();
									camMap[n] = id;
								}
							}
						}
					}
				}
			}
			if (!c.ok) {
				tool::out() << "    [truncated scene blob]\n";
				continue;
			}
			tool::out() << "    scene nodes=" << nodeCount
				<< " lightLinks=" << lightMap.size()
				<< " camLinks=" << camMap.size() << ":\n";
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
				auto cit = camMap.find(i);
				const bool hasCam = cit != camMap.end();
				if (!pinpoint && !hasMesh && !noLmFlag && !hasLight && !hasCam && !ditherFlag)
					continue;
				tool::out() << "      node " << i << " '" << nm << "' parent=" << parent[i];
				if (pinpoint)
					tool::out() << " t=(" << localT[i * 3 + 0] << ","
					<< localT[i * 3 + 1] << "," << localT[i * 3 + 2] << ")";
				if (hasMesh)
					tool::out() << " mesh=" << meshMap[i];
				auto mit = matMap.find(i);
				if (mit != matMap.end())
					tool::out() << " mat=" << mit->second;
				if (hasLight)
					tool::out() << " light=" << lit->second;
				if (hasCam)
					tool::out() << " cam=" << cit->second;
				if (noLmFlag)
					tool::out() << " noLM=1";
				if (ditherFlag)
					tool::out() << " dither=1";
				tool::out() << "\n";
				if (!pinpoint && ++shown > 200) {
					tool::out() << "      ... (truncated)\n";
					break;
				}
			}
		}
	}

// (namespace cooktool stays open through Run below, so every helper
// resolves Args unqualified.)

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

// Seam-correct per-corner chart-UV stamping, shared by source charts
// and UV-transfer levels. A vertex shared by two charts has no single
// valid chart-local UV, so each triangle corner uses its triangle's
// chart. The first chart keeps the original vertex slot; subsequent
// charts receive positional duplicates. The index buffer is rebuilt
// in triangle order, preserving surface ranges. The function reports
// seam duplicates and uncharted (dropped or degenerate) triangles.
//
// outChartOf maps stamped vertices to chart IDs for the simplifier's
// error metric. outXforms contains one atlas transform per chart:
// xy = scale, zw = offset, so atlasUv = chartUv * xy + zw. Each vertex
// stores only its chart-local UV and chart ID; the vertex shader uses
// GfxDraw.misc2.x to select the correct atlas transform.
//
// When texelSpace is true, transforms are in atlas-texel coordinates
// (AtlasTexel) and the later normalization pass divides them by the
// final atlas dimensions. When false, transforms are already normalized
// (AtlasUv, planar path). Vertex UVs are never modified by that pass.
// The return value is the mesh's lobe count from its first chunk, or
// fallbackLobes when no chunks exist.
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
	uint64_t& seamDups, uint32_t& unchartedTris,
	const char* auditLabel = nullptr, bool isTransfer = false) {
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
	// Colocal-UV invariant audit (chart-LOCAL uvs, before stamping):
	// same chart + colocated positions => identical chart-local UV
	// (the per-chart rect map is one affine transform, so the atlas-UV
	// invariant from GenerateLightmapChartsXatlas carries through).
	// Tight colocality mirrors xatlas's own epsilon (MeshDecl default
	// 1.192092896e-07, per-component |da-db| <= eps*max(1,|a|,|b|)) in
	// WORLD space — conservative vs the cook's density-scaled xatlas
	// input (scale >= 1 only shrinks xatlas's world-space threshold,
	// so a pair xatlas saw as split can still read as tight here;
	// such residue, if any, is small-delta by construction). Pairs in
	// the (tight, 1e-4] gap are importer-slop candidates: counted as
	// slopSame/slopCross, never flagged. auditLabel distinguishes the
	// chart SOURCE ("src ...": a hit is a real chart/stamp bug) from
	// LOD-transfer stamps ("xfer ...": closest-point projection may
	// legitimately snap colocated target verts apart — reported as
	// XFER-DIVERGENT, informational). Case B (different chart) is
	// expected; counted with the stitcher context (dotN vs
	// kNormalGate 0.7): pairs the post-bake stitch refuses to
	// reconcile. dist2 prints on every sample line so loose-box
	// residue is identifiable without a recook.
	{
		constexpr float kXatlasEps = 1.192092896e-07f;
		constexpr float kCell = 1e-4f; // slop-band radius (world)
		constexpr float kCell2 = 1e-8f;
		constexpr float kUvEps2 = 1e-10f; // (1e-5)^2, chart-local [0,1]
		constexpr float kNormalGate = 0.7f; // mirrors StitchSgLightmapSeams
		constexpr uint32_t kMaxPrints = 8;
		const char* lbl = auditLabel ? auditLabel : "?";
		const bool isXfer = isTransfer;
		auto colocated = [&](const glm::vec3& a, const glm::vec3& b) {
			return std::fabs(a.x - b.x)
				<= kXatlasEps * std::max(1.0f,
					std::max(std::fabs(a.x), std::fabs(b.x)))
				&& std::fabs(a.y - b.y)
				<= kXatlasEps * std::max(1.0f,
					std::max(std::fabs(a.y), std::fabs(b.y)))
				&& std::fabs(a.z - b.z)
				<= kXatlasEps * std::max(1.0f,
					std::max(std::fabs(a.z), std::fabs(b.z)));
		};
		struct Cell {
			int x, y, z;
			bool operator==(const Cell& o) const {
				return x == o.x && y == o.y && z == o.z;
			}
		};
		struct CellHash {
			size_t operator()(const Cell& c) const noexcept {
				size_t h = std::hash<int>{}(c.x);
				h ^= std::hash<int>{}(c.y) + 0x9e3779b9u + (h << 6)
					+ (h >> 2);
				h ^= std::hash<int>{}(c.z) + 0x9e3779b9u + (h << 6)
					+ (h >> 2);
				return h;
			}
		};
		std::unordered_map<Cell, std::vector<uint32_t>, CellHash> grid;
		grid.reserve(triCount * 3 * 2 + 16);
		uint64_t badA = 0, xferDiv = 0, seamB = 0, seamBgated = 0;
		uint64_t slopSame = 0, slopCross = 0, tightPairs = 0;
		uint32_t printedA = 0, printedB = 0;
		// ---- Case C probe: SG coefficients immediately on each side ----
		// For each colocated cross-chart pair, sample both chunks through
		// SgLightmapAtlas::Sample (the hardware bilinear mirror, uv
		// converted chart-inner [0,1] -> payload-rect [0,1]) and
		// measure the rendered discontinuity: L2 over the full SG
		// amplitude vector. Void sampling is NOT this probe's domain:
		// in-rect unclaimed texels are dilated (margin ring), beyond-
		// rect bleed is the bc6hopt prefill's job — so each side is
		// flagged claimed/dilated (primary texel) and the deltas are
		// bucketed by dotN: gated (<= 0.7: the crease population the
		// OLD stitch gate refused) vs ungated (> 0.7: the stitched
		// control), plus the opp sub-bucket (<= 0: still refused by
		// the 0.0 stitch gate — the residual after the A/B).
		// Per-chart claimed-texel map (BuildTexelOf-equivalent), lazy.
		std::vector<std::vector<uint32_t>> chartCov(
			chartRes.charts.size());
		auto sampleSide = [&](uint32_t localChart, const glm::vec2& cUv,
			glm::vec3* amps, bool& claimed) -> bool {
			claimed = false;
			if (localChart >= chartRes.charts.size())
				return false;
			const uint32_t chunk = chunkBase + localChart;
			if (chunk >= atlas.chunks.size()
				|| chunk >= atlas.payloads.size())
				return false;
			const auto& pp = atlas.payloads[chunk];
			if (pp.width <= 2 * gutter || pp.height <= 2 * gutter)
				return false;
			const float iw = float(pp.width - 2 * gutter);
			const float ih = float(pp.height - 2 * gutter);
			const float cx = glm::clamp(cUv.x, 0.0f, 1.0f);
			const float cy = glm::clamp(cUv.y, 0.0f, 1.0f);
			const glm::vec2 full(
				(float(gutter) + cx * iw) / float(pp.width),
				(float(gutter) + cy * ih) / float(pp.height));
			if (!atlas.Sample(chunk, full, amps))
				return false;
			if (chartCov[localChart].empty()) {
				auto& cov = chartCov[localChart];
				cov.assign(size_t(pp.width) * pp.height, 0xFFFFFFFFu);
				const auto& txs = chartRes.charts[localChart].texels;
				for (uint32_t i = 0; i < txs.size(); ++i) {
					const int x = int(std::floor(txs[i].uv.x));
					const int y = int(std::floor(txs[i].uv.y));
					if (x < 0 || y < 0 || x >= int(pp.width)
						|| y >= int(pp.height))
						continue;
					cov[size_t(y) * pp.width + size_t(x)] = i;
				}
			}
			const int px = glm::clamp(
				int(std::floor(float(gutter) + cx * iw)),
				0, int(pp.width) - 1);
			const int py = glm::clamp(
				int(std::floor(float(gutter) + cy * ih)),
				0, int(pp.height) - 1);
			claimed = chartCov[localChart][size_t(py) * pp.width
				+ size_t(px)] != 0xFFFFFFFFu;
			return true;
		};
		uint64_t sgPairs = 0, sgBothClaimed = 0, sgDilated = 0,
			sgUnsampleable = 0;
		uint64_t sgGatedN = 0, sgUngatedN = 0, sgOppN = 0;
		double sgGatedSum = 0.0, sgUngatedSum = 0.0, sgOppSum = 0.0;
		float sgGatedMax = 0.0f, sgUngatedMax = 0.0f, sgOppMax = 0.0f;
		for (size_t t = 0; t < triCount; ++t) {
			if (t >= triChart.size() || triChart[t] == 0xFFFFFFFFu)
				continue;
			for (int k = 0; k < 3; ++k) {
				const size_t ci = t * 3 + k;
				if (ci >= cornerUv.size())
					continue;
				const uint32_t vi = upload.indices[ci];
				if (vi >= upload.vertices.size())
					continue;
				const glm::vec3& pa = upload.vertices[vi].pos;
				const glm::vec2& ua = cornerUv[ci];
				const Cell base{
					int(std::floor(pa.x / kCell)),
					int(std::floor(pa.y / kCell)),
					int(std::floor(pa.z / kCell)),
				};
				for (int dx = -1; dx <= 1; ++dx)
					for (int dy = -1; dy <= 1; ++dy)
						for (int dz = -1; dz <= 1; ++dz) {
							const Cell nb{ base.x + dx, base.y + dy,
								base.z + dz };
							const auto it = grid.find(nb);
							if (it == grid.end())
								continue;
							for (uint32_t prev : it->second) {
								const size_t tb = prev / 3;
								const int kb = int(prev % 3);
								if (triChart[tb] == 0xFFFFFFFFu)
									continue;
								const uint32_t vj =
									upload.indices[tb * 3 + kb];
								if (vj >= upload.vertices.size())
									continue;
								const glm::vec3& pb =
									upload.vertices[vj].pos;
								const glm::vec3 d = pa - pb;
								const float d2 = glm::dot(d, d);
								if (d2 >= kCell2)
									continue;
								const glm::vec2& ub = cornerUv[tb * 3 + kb];
								if (!colocated(pa, pb)) {
									if (triChart[tb] == triChart[t])
										++slopSame;
									else
										++slopCross;
									continue;
								}
								++tightPairs;
								if (triChart[tb] == triChart[t]) {
									const glm::vec2 du = ua - ub;
									if (glm::dot(du, du) > kUvEps2) {
										if (isXfer)
											++xferDiv;
										else
											++badA;
										if (printedA < kMaxPrints) {
											++printedA;
											std::fprintf(stderr,
												"[LM] %s SAME-CHART COLOCAL UV "
												"(%s): chart=%u dist2=%g "
												"uvA=(%f,%f) uvB=(%f,%f) "
												"P=(%f,%f,%f)\n",
												isXfer ? "XFER-DIVERGENT" : "BAD",
												lbl, triChart[t], double(d2),
												ua.x, ua.y, ub.x, ub.y,
												pa.x, pa.y, pa.z);
										}
									}
								}
								else {
									++seamB;
									const float dotN = glm::dot(
										upload.vertices[vi].normal,
										upload.vertices[vj].normal);
									if (dotN <= kNormalGate)
										++seamBgated;
									// Case C probe (see above).
									glm::vec3 ampsA[smvk::gi::kLightmapMaxLobes];
									glm::vec3 ampsB[smvk::gi::kLightmapMaxLobes];
									bool clA = false, clB = false;
									const bool okA = sampleSide(triChart[t], ua,
										ampsA, clA);
									const bool okB = sampleSide(triChart[tb],
										ub, ampsB, clB);
									float sgDelta = -1.0f, sgMag = 0.0f;
									if (okA && okB) {
										++sgPairs;
										if (clA && clB)
											++sgBothClaimed;
										else
											++sgDilated;
										float d2 = 0.0f, mA2 = 0.0f, mB2 = 0.0f;
										for (uint32_t l = 0;
											l < smvk::gi::kLightmapMaxLobes;
											++l) {
											const glm::vec3 d =
												ampsA[l] - ampsB[l];
											d2 += glm::dot(d, d);
											mA2 += glm::dot(ampsA[l],
												ampsA[l]);
											mB2 += glm::dot(ampsB[l],
												ampsB[l]);
										}
										sgDelta = std::sqrt(d2);
										sgMag = std::sqrt(std::max(mA2, mB2));
										if (dotN <= kNormalGate) {
											++sgGatedN;
											sgGatedSum += sgDelta;
											sgGatedMax = std::max(sgGatedMax,
												sgDelta);
											if (dotN <= 0.0f) {
												++sgOppN;
												sgOppSum += sgDelta;
												sgOppMax = std::max(sgOppMax,
													sgDelta);
											}
										}
										else {
											++sgUngatedN;
											sgUngatedSum += sgDelta;
											sgUngatedMax = std::max(
												sgUngatedMax, sgDelta);
										}
									}
									else {
										++sgUnsampleable;
									}
									if (printedB < kMaxPrints) {
										++printedB;
										std::fprintf(stderr,
											"[LM] seam (%s) P=(%f,%f,%f) "
											"chart %u/%u dist2=%g "
											"dotN=%f uvA=(%f,%f) uvB=(%f,%f)"
											"%s sgDelta=%g mag=%g "
											"claimed=%d/%d\n",
											lbl, pa.x, pa.y, pa.z,
											triChart[tb], triChart[t],
											double(d2),
											dotN, ua.x, ua.y, ub.x, ub.y,
											sgDelta >= 0.0f ? "" : " [unsampled]",
											double(sgDelta), double(sgMag),
											clA ? 1 : 0, clB ? 1 : 0);
									}
								}
							}
						}
				grid[base].push_back(uint32_t(ci));
			}
		}
		if (badA > 0 || xferDiv > 0 || seamB > 0 || slopSame > 0) {
			std::fprintf(stderr,
				"[LM] stamp colocal audit (%s): tris %zu tight %llu "
				"badA %llu xferDiv %llu seamB %llu (gated %llu) "
				"slopSame %llu slopCross %llu\n",
				lbl, triCount,
				(unsigned long long)tightPairs,
				(unsigned long long)badA,
				(unsigned long long)xferDiv,
				(unsigned long long)seamB,
				(unsigned long long)seamBgated,
				(unsigned long long)slopSame,
				(unsigned long long)slopCross);
		}
		if (sgPairs > 0) {
			std::fprintf(stderr,
				"[LM] seam sg audit (%s): measured %llu "
				"(bothClaimed %llu dilated %llu unsampleable %llu) "
				"gated %llu: delta mean %g max %g "
				"(opp %llu: mean %g max %g) | "
				"ungated %llu: mean %g max %g\n",
				lbl,
				(unsigned long long)sgPairs,
				(unsigned long long)sgBothClaimed,
				(unsigned long long)sgDilated,
				(unsigned long long)sgUnsampleable,
				(unsigned long long)sgGatedN,
				sgGatedN ? sgGatedSum / double(sgGatedN) : 0.0,
				double(sgGatedMax),
				(unsigned long long)sgOppN,
				sgOppN ? sgOppSum / double(sgOppN) : 0.0,
				double(sgOppMax),
				(unsigned long long)sgUngatedN,
				sgUngatedN ? sgUngatedSum / double(sgUngatedN) : 0.0,
				double(sgUngatedMax));
		}
	}
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
				// Same chart reuses the original (UV identical:
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
	std::vector<std::pair<std::string, std::vector<uint8_t>>>& out,
	std::string& outManifestBase) {
	outManifestBase.clear();
	using namespace smvk::gi;
	const auto t0 = std::chrono::steady_clock::now();

	// Texture averages so captures see textured albedo (what the skybox
	// shows), mirroring the lightmap block.
	GiSceneBuildOptions buildOpts{};
	{
		uint32_t averaged = 0;
		uint32_t textured = 0;
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
			// Full decoded texel data: the trace samples the baseColor
			// texture at the interpolated hit UV (AlbedoAt) — bakes
			// carry the texture detail, not flat averages.
			if (auto data = GiDecodeKtxTexelData(
					std::span<const uint8_t>(*src), kGiBakeTexelMaxDim)) {
				buildOpts.texelData[handle.index()] = std::move(*data);
				++textured;
			}
		}
		tool::err() << "  [CUBE] textures averaged: " << averaged << " / "
			<< cluster.ListTextureUris().size() << " (decoded " << textured
			<< ")\n" << std::flush;
	}
	// Same import basis change as CubemapBaker/GiBaker: cook imports at
	// coordFix 0 read punctual aims with the conjugated -Z axis. The
	// sun/sky record comes from the scene's env block (EnvForBake — ONE
	// authored sun/sky everywhere, matching every other bake stage).
	const BakeEnv be = EnvForBake(cluster, args);
	buildOpts.punctualAimAxis = GltfPunctualAimAxis(0);
	buildOpts.env.sunAzimuthDeg = be.sunAzimuth;
	buildOpts.env.sunElevationDeg = be.sunElevation;
	buildOpts.env.sunIntensity = be.sunIntensity;
	buildOpts.env.skyIntensity = be.skyIntensity;
	// uniformAlbedo stays off: captures show real materials. The backend
	// honors --backend like every other bake stage (the old gap: the
	// capture stage silently ran the builtin reference BVH while CV/LM
	// ran the CLI choice - same transport math, but the packet/SIMD
	// Embree traversal is the fast default everywhere else).
	buildOpts.traceBackendEmbree = (args.backend == "embree");
	// Weave-transmissive cloth (--mask-transmission): the captures see
	// the banners' lit look AND the tinted light pools behind them.
	buildOpts.maskTransmissive = args.maskTransmission;
	{
		// Diagnostic: the alphaMode histogram over the NODE-REFERENCED
		// cluster materials (what the trace soup can intern at all).
		uint32_t nOpaque = 0, nMask = 0, nBlend = 0, nNodes = 0;
		for (const auto& [node, matIdx] : cluster.scene.materialForNode) {
			const smvk::ClusterMaterialHandle mh =
				cluster.materials.getHandle(matIdx);
			if (!mh.valid())
				continue;
			const smvk::Material* mat = cluster.materials.get(mh);
			if (!mat)
				continue;
			++nNodes;
			if (mat->alphaMode == smvk::AlphaMode::Mask)
				++nMask;
			else if (mat->alphaMode == smvk::AlphaMode::Blend)
				++nBlend;
			else
				++nOpaque;
		}
		tool::err() << "  [CUBE] cluster materials: " << nNodes
			<< " node-materials (" << nOpaque << " opaque, " << nMask
			<< " mask, " << nBlend << " blend; maskTransmissive="
			<< (buildOpts.maskTransmissive ? "on" : "off") << ")\n"
			<< std::flush;
	}

	GiTraceScene traceScene;
	std::string terr;
	// buildOpts.staticLights (default on) harvests the scene's
	// STATIC-flagged cooked lights into traceScene.spots/sun here.
	BuildTraceSceneFromCluster(cluster, buildOpts, traceScene, terr);
	if (!terr.empty()) {
		tool::err() << "[CUBE] trace scene failed: " << terr << std::endl;
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
			tool::err() << "Bad --spot (want px,py,pz,dx,dy,dz,innerDeg,"
				"outerDeg,range,r,g,b,candela): " << spec << "\n";
			return false;
		}
		spot.color = col * candela;
		spot.cosInner = std::cos(glm::radians(std::min(innerDeg, outerDeg)));
		spot.cosOuter = std::cos(glm::radians(std::max(innerDeg, outerDeg)));
		traceScene.spots.push_back(spot);
	}
	{
		uint32_t weaveMats = 0, weaveTris = 0;
		for (size_t t = 0; t < traceScene.triMaterial.size(); ++t) {
			if (traceScene.materials[traceScene.triMaterial[t]].weave) {
				++weaveTris;
				if (weaveTris == 1)
					++weaveMats; // crude: at least one (per-tri count is the signal)
			}
		}
		uint32_t weaveUnique = 0;
		for (const auto& m : traceScene.materials)
			if (m.weave)
				++weaveUnique;
		tool::err() << "  [CUBE] trace scene: " << traceScene.Bvh().TriangleCount()
			<< " tris, " << traceScene.materials.size() << " materials, "
			<< traceScene.spots.size() << " spots"
			<< (weaveUnique > 0
				? (" (weave: " + std::to_string(weaveUnique)
					+ " mats / " + std::to_string(weaveTris) + " tris)")
				: std::string(" (NO WEAVE - check --mask-transmission)"))
			<< "\n" << std::flush;
	}
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
				tool::err() << "  [CUBE] mat[" << mi << "] albedo=("
					<< m.albedo.x << "," << m.albedo.y << "," << m.albedo.z
					<< ") emissive=(" << m.emissive.x << "," << m.emissive.y
					<< "," << m.emissive.z << ")\n";
			}
		}
		tool::err() << "  [CUBE] materials dark(<1e-4): " << darkMats << "/"
			<< traceScene.materials.size() << std::flush;
		std::vector<std::pair<uint32_t, glm::vec3>> avgs(
			buildOpts.texelAvg.begin(), buildOpts.texelAvg.end());
		std::sort(avgs.begin(), avgs.end(),
			[](const auto& a, const auto& b) { return a.first < b.first; });
		tool::err() << " texelAvg=" << avgs.size() << "\n" << std::flush;
		for (size_t i = 0; i < std::min<size_t>(avgs.size(), 5); ++i) {
			tool::err() << "  [CUBE] texelAvg[" << avgs[i].first << "]=("
				<< avgs[i].second.x << "," << avgs[i].second.y << ","
				<< avgs[i].second.z << ")\n" << std::flush;
		}
	}
	if (traceScene.Bvh().TriangleCount() == 0) {
		tool::err() << "[CUBE] no trace geometry (empty scene)" << std::endl;
		return false;
	}

	// Capture positions: the explicit --cubemap-pos list, else the
	// scene-bounds center raised by --cubemap-height (CubemapBaker rule).
	// If --cubemap-octree is on, sparse adaptive placement wins over the
	// uniform --cubemap-grid lattice (see cubemap_placement.h).
	std::vector<glm::vec3> positions;
	std::vector<glm::vec2> radii; // parallel to positions: x = influence, y = fade
	for (const std::string& spec : args.cubemapPos) {
		glm::vec3 p{};
		if (std::sscanf(spec.c_str(), "%f,%f,%f", &p.x, &p.y, &p.z) != 3) {
			tool::err() << "Bad --cubemap-pos (want x,y,z): " << spec << "\n";
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
			// 128 CELLS: the partition budget (the +7-per-split leaf
			// math needs headroom past uniform depth 2 to afford depth-3
			// refinement). VIABILITY gating at emission skips wall-slit
			// cells (black captures), so the probe count that survives
			// is well under the cell count. Per-pixel shading cost is
			// NOT this number - the tile lists bucket at most 4 probes
			// per tile (MAX_FP_PROBE_TILE_SLOTS) and the specular walk
			// blends the top 2. Density here only buys coverage/parallax
			// fit. The manifest/runtime resident caps do the rest.
			oo.maxProbes = 128;
			// The PLACEMENT geometry: the trace scene MINUS the weave
			// cloth. The cloth is a thin HANGING layer (the banners), not
			// structure - refining near it and accepting embedDist
			// clearances against it placed captures "almost colliding"
			// with the banners (the preview balls clipping them, the
			// parallax fits hugging the cloth). Only the PLACEMENT
			// ignores the cloth; the capture BAKE below still sees it
			// (and passes light through it - maskTransmissive).
			// Cloth marker: the EXPLICIT weave flag (the intern sets it
			// from the alpha mode; an albedo inference breaks exactly on
			// the metallic-1 cloth, whose albedo is 0). Blend glass
			// stays structure for the placement.
			auto isCloth = [&](uint32_t tri) {
				return traceScene.materials[
					traceScene.triMaterial[tri]].weave;
			};
			smvk::gi::TriangleBvh placementBvh;
			uint32_t clothTris = 0;
			{
				const auto& tbvh = traceScene.Bvh();
				bool anyCloth = false;
				for (uint32_t t = 0; t < tbvh.TriangleCount(); ++t)
					if (isCloth(t)) { anyCloth = true; break; }
				if (anyCloth) {
					std::vector<uint32_t> kept;
					kept.reserve(traceScene.indices.size());
					for (uint32_t t = 0; t < tbvh.TriangleCount(); ++t) {
						if (isCloth(t)) {
							++clothTris;
							continue;
						}
						kept.push_back(traceScene.indices[t * 3 + 0]);
						kept.push_back(traceScene.indices[t * 3 + 1]);
						kept.push_back(traceScene.indices[t * 3 + 2]);
					}
					placementBvh.Build(traceScene.positions, kept,
						traceScene.GetTraceBackend());
				}
				if (clothTris > 0)
					tool::err() << "  [CUBE] placement excludes " << clothTris
						<< " cloth tris (captures place against the"
						" structure; the bake still sees + passes the"
						" cloth)\n" << std::flush;
			}
			smvk::gi::TriangleBvh& bvh = (clothTris > 0)
				? placementBvh : const_cast<smvk::gi::TriangleBvh&>(
					traceScene.Bvh());
			smvk::CubemapOcclusionRay occ =
				[&](const glm::vec3& o, const glm::vec3& d, float m) {
				smvk::gi::TriangleBvh::Hit h{};
				return bvh.Raycast(o, d, m, h);
			};
			std::vector<smvk::CubemapPlacedProbe> placed;
			std::string olog;
			smvk::BuildOctreeProbePositions(bmin, bmax, oo, occ, placed, olog);
			tool::err() << olog << std::flush;
			positions.reserve(placed.size());
			radii.reserve(placed.size());
			for (const auto& p : placed) {
				positions.push_back(p.position);
				radii.emplace_back(p.influenceRadius, p.fadeRadius);
			}
			// CLOTH CLEARANCE: the exclusion above only stops the placement
			// from REFINING against the cloth - the emitted cell centers
			// still land next to the banners (the cells span across them),
			// and a capture a few decimeters from a hanging banner bakes it
			// at point-blank, the preview ball clips it and the parallax
			// fit hugs it ("almost collide with the cloths"). Push every
			// capture that sits closer than kClothClearance to the nearest
			// cloth triangle out along the away direction (a relocation,
			// not a drop: the coverage cell keeps its capture; the pushed
			// positions land in the open corridor next to the banner).
			if (clothTris > 0) {
				smvk::gi::TriangleBvh clothBvh;
				{
					std::vector<uint32_t> clothIdx;
					clothIdx.reserve(size_t(clothTris) * 3);
					for (uint32_t t = 0;
						t < traceScene.Bvh().TriangleCount(); ++t) {
						if (!isCloth(t))
							continue;
						clothIdx.push_back(traceScene.indices[t * 3 + 0]);
						clothIdx.push_back(traceScene.indices[t * 3 + 1]);
						clothIdx.push_back(traceScene.indices[t * 3 + 2]);
					}
					clothBvh.Build(traceScene.positions, clothIdx,
						traceScene.GetTraceBackend());
				}
				constexpr float kClothClearance = 1.0f;
				uint32_t pushedCount = 0;
				for (glm::vec3& pos : positions) {
					glm::vec3 cp;
					const float d = clothBvh.ClosestDistance(pos, &cp);
					if (!(d < kClothClearance) || !(d >= 0.0f))
						continue;
					glm::vec3 away = pos - cp;
					const float len = glm::length(away);
					if (len < 1e-4f)
						continue; // inside the cloth plane: leave it (the
						          // dead-drop filter below catches it)
					away /= len;
					pos += away * (kClothClearance - d);
					++pushedCount;
				}
				if (pushedCount > 0)
					tool::err() << "  [CUBE] cloth clearance: pushed "
						<< pushedCount << " capture(s) to " << kClothClearance
						<< "m from the banners\n" << std::flush;
			}
		}
		else if (args.cubemapGrid > 0.0f) {
			// Generate 3D grid of probe positions
			const float spacing = args.cubemapGrid;
			const float yBase = bmin.y + args.cubemapHeight;

			int nx = std::max(1, int(std::ceil((bmax.x - bmin.x) / spacing)));
			int ny = std::max(1, int(std::ceil((bmax.y - bmin.y) / spacing)));
			int nz = std::max(1, int(std::ceil((bmax.z - bmin.z) / spacing)));

			tool::err() << "  [CUBE] generating " << nx << "x" << ny << "x" << nz
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
					{ 1, 0, 0 },{ -1, 0, 0 },{ 0, 1, 0 },
					{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 },
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
						tool::err() << "  [CUBE] dropping embedded grid probe ("
							<< p.x << "," << p.y << "," << p.z << ")\n";
					}
					else {
						open.push_back(p);
						openRadii.push_back(radii[oi]);
					}
				}
				positions.swap(open);
				radii.swap(openRadii);
				tool::err() << "  [CUBE] grid occupancy: kept " << positions.size()
					<< ", dropped " << dropped << " embedded\n" << std::flush;
			}
		}
		else {
			// Single probe at bounds center (original behavior)
			const glm::vec3 c = (bmin + bmax) * 0.5f;
			positions.push_back(glm::vec3(c.x, bmin.y + args.cubemapHeight, c.z));
			tool::err() << "  [CUBE] no --cubemap-pos: capturing at bounds center ("
				<< positions.back().x << "," << positions.back().y << ","
				<< positions.back().z << ")\n" << std::flush;
		}
	}

	// NAMELESS capture-set base: the FIRST input artifact's stem
	// (Sponza.geometry -> "Sponza"). pak.exe discovers + embeds the
	// manifest by side-car stem (gi/cubemaps/<stem>.cubemanifest), so the
	// bake MUST land on that stem or the pak silently ships without GI
	// (observed: the old "sky" default baked fine but never embedded,
	// gi=0/cubemaps=0, no specular IBL at runtime). The runtime accepts
	// whatever base the pak registers, so no name exists anywhere else.
	std::string base;
	if (!args.inVfsList.empty())
		base = std::filesystem::path(args.inVfsList.front())
			.stem().generic_string();
	if (base.empty())
		base = "sky";

	// Stable paths (like mesh blobs): captures at
	// gi/cubemaps/<base>_<N>.ktx2 (the single capture keeps the
	// bare <base> name), the placement manifest at
	// gi/cubemaps/<base>.cubemanifest. Rebakes overwrite the same
	// paths; the manifest's optional trailing "guid <hex>" column is
	// vestigial (kept parseable; the runtime loads by path).
	const std::filesystem::path cubeAssetDir =
		std::filesystem::path(args.contentDir) / "textures" / "cubemaps";

	// Artist-pinned extra probes (--cubemap-extra-pos): appended AFTER
	// placement, flagged 'pin' in the manifest so the runtime sorts them
	// first — a pinned probe wins weight ties (e.g. the capture exactly at
	// the chrome ball). Radii are deliberately tight: a pin is a local
	// reflection source, not coverage. NOTE: the trace scene never
	// contains runtime-injected props (the chrome ball is an app-side
	// node), so a pin at the ball captures the ATRIUM AROUND it — exactly
	// what a mirror ball should reflect.
	std::vector<char> pinned;
	for (const std::string& spec : args.cubemapExtraPos) {
		glm::vec3 p{};
		if (std::sscanf(spec.c_str(), "%f,%f,%f",
			&p.x, &p.y, &p.z) != 3) {
			tool::err() << "Bad --cubemap-extra-pos (want x,y,z): "
				<< spec << "\n";
			return false;
		}
		if (!positions.empty() && radii.size() != positions.size())
			radii.resize(positions.size(), glm::vec2(10.0f, 5.0f));
		positions.push_back(p);
		radii.emplace_back(args.cubemapExtraRadius,
			args.cubemapExtraRadius * 0.5f);
		pinned.push_back(1);
		tool::err() << "  [CUBE] pinned extra probe (" << p.x << "," << p.y
			<< "," << p.z << ") r=" << args.cubemapExtraRadius << "\n"
			<< std::flush;
	}
	// Placement-derived rows come first and are never pinned.
	pinned.insert(pinned.begin(), positions.size() - pinned.size(), 0);

	// AUTHORED captures (tour createCubemap / editor cubemap nodes):
	// pinned probes harvested from the definition's cluster — position
	// = the node's world transform, influence = the node-local radius
	// (fade = radius/2, the CLI pin rule). Appended after the CLI
	// --cubemap-extra-pos pins, same manifest semantics.
	for (const auto& [node, def] : cluster.scene.cubemapForNode) {
		if (node >= cluster.scene.globalTransform.size())
			continue;
		const glm::vec3 p(cluster.scene.globalTransform[node][3]);
		if (!positions.empty() && radii.size() != positions.size())
			radii.resize(positions.size(), glm::vec2(10.0f, 5.0f));
		positions.push_back(p);
		radii.emplace_back(def.radius, def.radius * 0.5f);
		pinned.push_back(1);
		tool::err() << "  [CUBE] authored pinned probe ('"
			<< getNodeName(cluster.scene, int(node)) << "' @" << p.x
			<< "," << p.y << "," << p.z << ") r=" << def.radius << "\n"
			<< std::flush;
	}

	// Runtime budget: the tile lists bucket at most MAX_FP_PROBE_TILE_SLOTS
	// (4) probes per tile and the specular walk blends the top 2 - the
	// PER-PIXEL shading cost never scales with the capture count. The
	// 64-row cap below is the RESIDENT/upload budget (kForwardPlusMaxProbes
	// + the manifest cap; the runtime streams the nearest rows within it).
	// Captures beyond the cap still incur bake time and ~2 MB of pak data
	// each; manifest truncation preserves pinned rows and drops excess
	// unpinned rows.
	if (positions.empty()) {
		tool::err() << "  [CUBE] WARNING: no capture positions survived"
			" (all grid points embedded?) - no cubemaps baked\n" << std::flush;
		return true;
	}
	if (positions.size() > 64) {
		tool::err() << "  [CUBE] WARNING: " << positions.size()
			<< " captures exceed the 64-probe resident budget (manifest"
			<< " truncation keeps pinned rows) - increase --cubemap-grid"
			<< " spacing or --cubemap-octree-max to cover the scene with"
			<< " fewer, wider probes\n" << std::flush;
	}

	const uint32_t S = args.cubemapSize;
	const uint32_t levels = smvk::CubemapFullMipCount(S);
	smvk::ThreadPool pool(0);

	// Debug PNG outputs (loose, next to the pak inputs — never in the pak).
	// Face strips (+ mip sheets) per capture, then one top-down map.
	// Debug PNG outputs (loose, next to the KTX2 bake files — never in
	// the pak). Moved with the capture-set home (was textures/cubemaps).
	std::filesystem::path cubePngDir =
		std::filesystem::path(args.contentDir) / "gi/cubemaps";
	if (args.cubemap && args.cubemapPng) {
		std::error_code dcEc;
		std::filesystem::create_directories(cubePngDir, dcEc);
	}
	std::vector<glm::vec3> pngMeans;
	pngMeans.reserve(positions.size());
	// Depth-fitted parallax boxes, parallel to positions (see the fit
	// pass inside the loop): written into the manifest rows below.
	std::vector<std::pair<glm::vec3, glm::vec3>> fitBoxes;
	// Parallel to positions: 1 = dark capture (all fit planes < 0.8 m -
	// wall slit; the bake was skipped, the manifest row is dropped).
	std::vector<char> captureDead;
	fitBoxes.reserve(positions.size());

	// Machine-readable total for progress clients (the editor's cook
	// dialog parses it into TaskWindowStage fractions; per-capture
	// "baked" lines below mark completion).
	tool::err() << "  [CUBE] captures=" << positions.size() << "\n" << std::flush;

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
			1.0f / std::max(be.skyIntensity, 1e-4f);

		// DEPTH-FITTED PARALLAX BOX: analyze each cube face's primary-ray
		// depths (a coarse 16x16 grid per face) and keep the per-plane
		// MEDIAN hit distance (misses count at the analysis reach - the
		// median tracks the DOMINANT surface the face sees, not the
		// nearest speck or the one window). The six plane distances are
		// the probe's "wall scalars": the world AABB the runtime's box
		// parallax re-aims reflections against, replacing the symmetric
		// influence-sphere guess. The box's half-diagonal ALSO clamps the
		// placement influence (auto-fit coverage): a probe whose emission
		// relocated to the open sky just above a roof fits a thin
		// downward box and stops reaching THROUGH the roof into the room
		// below - the old cell-half-diagonal influence put full-white sky
		// captures over every interior pixel at weight 1.0 (the lit walk
		// could never reach the interior probes).
		glm::vec3 fitMin(0.0f), fitMax(0.0f);
		{
			constexpr uint32_t kFitGrid = 16;
			constexpr float kFitReach = 60.0f;
			float planeDist[smvk::kCubemapFaceCount] = {};
			for (uint32_t f = 0; f < smvk::kCubemapFaceCount; ++f) {
				std::vector<float> ds;
				ds.reserve(kFitGrid * kFitGrid);
				for (uint32_t y = 0; y < kFitGrid; ++y) {
					for (uint32_t x = 0; x < kFitGrid; ++x) {
						const float u = (float(x) + 0.5f) / float(kFitGrid);
						const float v = (float(y) + 0.5f) / float(kFitGrid);
						const smvk::CubemapDir d = smvk::FaceUvToDirection(
							smvk::CubemapFace(f), u, v);
						glm::vec3 dir(d.x, d.y, d.z);
						dir = glm::normalize(dir);
						smvk::gi::TriangleBvh::Hit h;
						ds.push_back(
							traceScene.Bvh().Raycast(origin, dir, kFitReach, h)
								? h.t : kFitReach);
					}
				}
				std::sort(ds.begin(), ds.end());
				planeDist[f] = ds[ds.size() / 2];
			}
			// CubemapFace order = Px, Nx, Py, Ny, Pz, Nz -> the six
			// world-axis planes around the capture point.
			fitMin = origin - glm::vec3(planeDist[1], planeDist[3], planeDist[5]);
			fitMax = origin + glm::vec3(planeDist[0], planeDist[2], planeDist[4]);
			// Floor every plane at 0.25m so a capture hugging a wall still
			// describes a thin-but-valid pocket (a zero plane would
			// degenerate the slab test at runtime).
			fitMin = glm::min(fitMin, origin - glm::vec3(0.25f));
			fitMax = glm::max(fitMax, origin + glm::vec3(0.25f));
			// Auto-fit the influence: the placement/cell radius clamped by
			// the fitted pocket's half-diagonal (floor 0.5m). The fade
			// stays cell-based so coverage keeps transitioning to the
			// neighbors.
			const float boxHalf = glm::length((fitMax - fitMin) * 0.5f);
			if (radii.size() == positions.size()) {
				radii[cap].x = std::max(
					std::min(radii[cap].x, boxHalf), 0.5f);
			}
			tool::err() << "  [CUBE] fit '" << captureName
				<< "' planes=(" << planeDist[0] << "," << planeDist[1]
				<< "," << planeDist[2] << "," << planeDist[3] << ","
				<< planeDist[4] << "," << planeDist[5]
				<< ") influence=" << (radii.size() == positions.size()
					? radii[cap].x : 0.0f) << "\n" << std::flush;

			// DARK-capture gate (EXACT - the bake-side twin of the
			// placement viability test): every face's median plane under
			// 0.8 m means the point sees NOTHING beyond a wall slit in
			// any axis direction - the capture bakes ~black. Skip the
			// whole bake for this row (the expensive GGX prefilter +
			// encode + loose write + pak blob all die here) and mark it
			// dead for the manifest writer. Neighbors cover the cell
			// (their influence + fade reach in). Observed on sponza:
			// 81 of 126 cells landed in wall slits (thin walls pass the
			// ray gates - rays escape ALONG the wall plane).
			const float maxPlane = std::max(std::max(
				std::max(planeDist[0], planeDist[1]),
				std::max(planeDist[2], planeDist[3])),
				std::max(planeDist[4], planeDist[5]));
			if (maxPlane < 0.8f) {
				tool::err() << "  [CUBE] skip dark capture '" << captureName
					<< "' (all planes < 0.8 m - wall slit)\n" << std::flush;
				captureDead.push_back(1);
				fitBoxes.emplace_back(fitMin, fitMax);
				pngMeans.push_back(glm::vec3(0.0f));
				continue;
			}
		}
		captureDead.push_back(0);
		fitBoxes.emplace_back(fitMin, fitMax);

		// Trace 6 HDR base faces. Per texel: --cubemap-spp transport paths,
		// averaged — the NEE sun/spot term is analytic (clean at 1 spp), but
		// the bounce field is a Monte-Carlo estimate and 1 spp reads as
		// speckle instead of the GI the lightmapped scene shows. Per-texel
		// seeds stay deterministic (spp continues the same rng stream).
		//
		// AMBIENT CLOSURE (--cubemap-ambient-closure, default OFF): hit
		// directions add SkyAmbient * firstHitAlbedo on top of the traced
		// radiance. That matches the RUNTIME SKY-AMBIENT FALLBACK - the
		// right compensation for scenes WITHOUT GI volume data (interior
		// probes whose paths never escape bake ~black, the probe-vs-
		// fallback seam reads as broken cubemaps; observed at sponza
		// interiors ~0.003 HDR). With GI volumes in the pak (the standard
		// cook -> GiBaker -> pak pipeline) the runtime ambient comes from
		// the GI STACK, and the closure baked into the captures
		// DOUBLE-COUNTS it: mirror-like surfaces (the chrome probe ball,
		// gold trim) reflect ~traced-bounce + closure ~= 2x the
		// ambient-lit field. Stack-less cooks opt in explicitly.
		const bool closure = args.cubemapAmbientClosure;
		const glm::vec3 skyAmb = closure ? traceScene.SkyAmbient()
			: glm::vec3(0.0f);
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
								glm::vec3 firstAlbedo(-1.0f);
								if (!traceScene.Bvh().Raycast(origin, dir, 1.0e5f, h)) {
									L += traceScene.SkyRadiance(dir);
								}
								else {
									L += traceScene.TraceIncoming(origin, dir, rng,
										args.cubemapBounces, nullptr, &firstAlbedo);
									// Ambient closure (opt-in, see the block
									// comment above): the visible surface's
									// one-bounce ambient (albedo < 0 = sky /
									// transmissive-only path: no closure).
									if (closure && firstAlbedo.x >= 0.0f)
										L += skyAmb * firstAlbedo;
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
			tool::err() << "[CUBE] encode failed '" << captureName << "': "
				<< werr << std::endl;
			return false;
		}
		// VFS path (stable, like the standalone CubemapBaker): the single
		// capture keeps the bare <base> name (the runtime's direct
		// <base>.ktx2 probe centers on the scene bounds — a 1-row
		// manifest would move it to the capture point, so singles stay
		// out of the manifest), multi-capture sets use <base>_<N>.
		const std::string vfsPath = smvk::CubemapVfsPath(captureName);
		tool::err() << "  [CUBE] baked " << vfsPath << "  " << S << "x" << S
			<< "x6 ggx-levels=" << levels << " rays=" << args.cubemapRays
			<< " origin=(" << origin.x << "," << origin.y << "," << origin.z
			<< ")  " << ktx.size() << " B\n" << std::flush;
		// Loose payload (the mesh-blob model: the loose file + the pak
		// blob ride the same path; the overlay lets the loose copy
		// override). Best-effort: the pak blob is the runtime channel.
		{
			const std::filesystem::path loosePath =
				std::filesystem::path(args.contentDir) / vfsPath;
			std::error_code ec;
			std::filesystem::create_directories(loosePath.parent_path(), ec);
			std::ofstream lf(loosePath, std::ios::binary | std::ios::trunc);
			if (!lf
				|| !lf.write(reinterpret_cast<const char*>(ktx.data()),
					std::streamsize(ktx.size()))) {
				tool::err() << "  [CUBE] loose payload write failed for "
					<< vfsPath << " - continuing with the pak blob only\n"
					<< std::flush;
			}
		}
		out.emplace_back(vfsPath, std::move(ktx));

		// Debug PNGs from the same preconvolved chains the KTX encodes
		// (comparing these against the runtime view separates bake/encode
		// faults from load/sample faults). Non-fatal by design.
		glm::vec3 capMean = pakcook::CubemapCaptureMean(chains, S);
		pngMeans.push_back(capMean);
		if (pakcook::CubemapMeanLuma(capMean) < 1e-6f) {
			tool::err() << "  [CUBE] WARNING baked '" << captureName
				<< "' mean ~0 (origin embedded or fully occluded?) - check "
				<< captureName << ".png\n" << std::flush;
		}
		if (args.cubemapPng) {
			std::string perr;
			const std::string facesPath =
				(cubePngDir / (captureName + ".png")).generic_string();
			if (pakcook::WriteCubemapFacesPng(chains, S, pngDisplayExposure, facesPath, perr)) {
				tool::err() << "  [CUBE] png " << facesPath
					<< " (mean luma "
					<< pakcook::CubemapMeanLuma(capMean) << ")\n" << std::flush;
			}
			else {
				tool::err() << "  [CUBE] png write failed: " << perr << "\n" << std::flush;
			}
			if (args.cubemapPngMips) {
				const std::string mipsPath =
					(cubePngDir / (captureName + "_mips.png")).generic_string();
				if (!pakcook::WriteCubemapMipsPng(chains, S, pngDisplayExposure, mipsPath, perr)) {
					tool::err() << "  [CUBE] mips png write failed: " << perr
						<< "\n" << std::flush;
				}
			}
		}
	}

	// Multi-probe placement manifest: the runtime cannot guess grid
	// positions from KTX blobs alone, so list every capture with its
	// cook/world-space origin + influence/fade radii. Served as a Texture
	// blob (PakBackend serves Texture blobs by VFS path) and consumed by
	// WorldRenderer::LoadCubemaps. Single captures need none (the runtime
	// centers a global probe on the scene bounds itself — a 1-row
	// manifest would move it to the capture point). Rows are bare
	// stable paths (the trailing "guid <hex>" column of bake-asset rows
	// stays parseable; the manifest itself rides the stable
	// <base>.cubemanifest path).
	if (positions.size() > 1) {
		// Radii ride the parallel radii[] vector (filled per branch:
		// explicit 10/5, grid tight to the spacing, octree per-cell,
		// pinned extras tight to --cubemap-extra-radius), and the pinned[]
		// flags mark the artist rows, so the manifest matches what the
		// placement chose.
		std::vector<smvk::CubemapManifestEntry> rows;
		rows.reserve(positions.size());
		size_t droppedBlack = 0;
		for (size_t cap = 0; cap < positions.size(); ++cap) {
			// Drop DARK captures from the manifest: rows whose bake was
			// skipped (the fit's all-planes-<0.8 m wall-slit gate - the
			// capture never ran) or whose baked mean is dead (luma <=
			// kCubemapDeadLuma: embedded/fully-occluded origins, or deep
			// interiors whose bounce tail died - the trace carries no
			// signal there; the old ambient closure MASKED these with fake
			// sky ambient, which double-counts with the GI stack - the
			// closure is opt-in now, so the dead ones must go). Such a row
			// serves nothing its pocket doesn't, wastes a resident slot
			// + ~2 MB of pak data, and darkens whatever tile lists it
			// lands in. The bake + loose bake file stay (inspection), only
			// the row goes - the zone falls to the live neighbors + the
			// (GI-occluded) sky fallback. Pinned rows are NEVER dropped
			// (artist intent).
			constexpr float kCubemapDeadLuma = 20.0f;
			const bool isPinned = cap < pinned.size() && pinned[cap] != 0;
			const bool dead = cap < captureDead.size() && captureDead[cap] != 0;
			const bool blackMean = cap < pngMeans.size()
				&& pakcook::CubemapMeanLuma(pngMeans[cap]) <= kCubemapDeadLuma;
			if (!isPinned && (dead || blackMean)) {
				++droppedBlack;
				continue;
			}
			smvk::CubemapManifestEntry e{};
			e.vfsPath = smvk::CubemapVfsPath(
				base + "_" + std::to_string(cap));
			e.position = positions[cap];
			e.influenceRadius = radii.size() == positions.size() ? radii[cap].x : 10.0f;
			e.fadeRadius = radii.size() == positions.size() ? radii[cap].y : 5.0f;
			// Depth-fitted parallax box (the fit pass above; the
			// influence already carries the clamped half-diagonal).
			if (cap < fitBoxes.size()) {
				e.boxMin = fitBoxes[cap].first;
				e.boxMax = fitBoxes[cap].second;
			} else {
				e.DeriveSymmetricBox();
			}
			e.pinned = isPinned;
			rows.push_back(std::move(e));
		}
		if (droppedBlack > 0)
			tool::err() << "  [CUBE] manifest dropped " << droppedBlack
				<< " black capture(s) (embedded emission points)\n"
				<< std::flush;
		const std::string text = smvk::CubemapManifestSerialize(rows);
		const std::string manifestVfs = smvk::CubemapManifestVfsPath(base);
		outManifestBase = base;
		out.emplace_back(manifestVfs,
			std::vector<uint8_t>(text.begin(), text.end()));
		// Loose manifest (the mesh-blob model: loose + pak copies at the
		// same path). Best-effort like the payloads.
		{
			const std::filesystem::path looseManifest =
				std::filesystem::path(args.contentDir) / manifestVfs;
			std::error_code ec;
			std::filesystem::create_directories(
				looseManifest.parent_path(), ec);
			std::ofstream mf(looseManifest,
				std::ios::binary | std::ios::trunc);
			if (!mf || !(mf << text)) {
				tool::err() << "  [CUBE] loose manifest write failed for "
					<< manifestVfs
					<< " - continuing with the pak blob only\n"
					<< std::flush;
			}
		}
		tool::err() << "  [CUBE] manifest " << manifestVfs << "  probes="
			<< rows.size() << " pinned="
			<< size_t(std::count(pinned.begin(), pinned.end(), 1))
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
			tool::err() << "  [CUBE] probes map " << mapPath << "\n" << std::flush;
		}
		else {
			tool::err() << "  [CUBE] probes map write failed: " << merr
				<< "\n" << std::flush;
		}
	}

	const double secs = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - t0).count();
	tool::err() << "  [CUBE] preconvolved cubemaps: captures=" << positions.size()
		<< " seconds=" << secs << "\n" << std::flush;
	return true;
}

// Cook write-back: persist the cooked cluster meshes into their source
// .geometry artifacts (MESH v2) + a <stem>.cooked manifest per artifact.
// Pak assembles the .pak from those without running any bake. Blob-local
// data only (stamped shading, meshlets, LOD siblings, the lm binding);
// nodes / instances / lights / materials ride through untouched from the
// import derivation. Meshes are grouped by their URI file part, so a
// multi-artifact tour replay writes each artifact back separately.
// A cluster mesh that maps to NO artifact blob (de-instance clones and
// collapse-merged blocks carry no source fragment) is a hard error —
// structural write-back is not implemented; cook without the structural
// stages (no --lightmap de-instance, no --collapse-static) for now.
struct WriteBackStats {
	size_t artifacts = 0;
	size_t blobs = 0;
	size_t siblings = 0;
	size_t meshleted = 0;
	size_t lmBound = 0;
};

static std::string CookFlagsFingerprint(const Args& args) {
	std::ostringstream f;
	f << "lightmap=" << args.lightmap
		<< ";cubemap=" << args.cubemap
		<< ";lods=" << args.lods
		<< ";meshlets=" << args.meshlets
		<< ";cvolume=" << args.cvolume
		<< ";collapse=" << args.collapseStatic
		<< ";cubemapSize=" << args.cubemapSize
		<< ";lmMetersPerTexel=" << args.lmMetersPerTexel
		<< ";lmLobes=" << args.lmLobes
		<< ";lmAtlasWidth=" << args.lmAtlasWidth;
	return f.str();
}

bool WriteCookedArtifactsBack(const Args& args,
	const std::vector<std::unique_ptr<smvk::Cluster>>& sceneClusters,
	WriteBackStats& st, std::string& err) {
	// 1) Group every cooked mesh by its source artifact (the URI file
	// part). Base meshes come from node references; LOD siblings (never
	// node-attached) come from the LOD registry.
	struct MeshRef {
		smvk::Cluster* cluster = nullptr;
		smvk::ClusterStaticMeshHandle handle;
		smvk::LodInfo lod; // lodIndex 0 = base
	};
	std::map<std::string, std::vector<MeshRef>> byFile;
	for (auto& sc : sceneClusters) {
		smvk::Cluster& c = *sc;
		std::unordered_set<uint32_t> seen;
		for (const auto& [node, meshIdx] : c.scene.meshForNode) {
			if (!seen.insert(meshIdx).second)
				continue; // still-shared: one set per upload
			const smvk::ClusterStaticMeshHandle h =
				c.staticMeshes.getHandle(meshIdx);
			if (!h.valid())
				continue;
			const smvk::StringViewT uri = c.GetMeshUri(h);
			if (uri.empty()) {
				err = "cooked mesh " + std::to_string(meshIdx)
					+ " carries no URI (cannot map back to an artifact)";
				return false;
			}
			MeshRef r;
			r.cluster = &c;
			r.handle = h;
			r.lod = smvk::LodInfo{};
			byFile[smvk::AssetFilePart(uri)].push_back(r);
		}
		for (const auto& [uriV, lod] : c.ListMeshLods()) {
			if (lod.lodIndex < 1)
				continue;
			auto h = c.FindMeshByUri(uriV);
			if (!h.has_value() || !h->valid())
				continue;
			MeshRef r;
			r.cluster = &c;
			r.handle = *h;
			r.lod = lod;
			byFile[smvk::AssetFilePart(uriV)].push_back(r);
		}
	}
	// 2) Per artifact: load the current derivation (preserves nodes /
	// instances / lights / materials), overwrite the cooked blobs,
	// save + stamp the manifest.
	for (auto& [file, refs] : byFile) {
		const std::filesystem::path hostPath =
			std::filesystem::path(args.contentDir) / file;
		smvk::assets::MeshAsset asset;
		std::string aerr;
		if (!smvk::assets::LoadMeshAsset(hostPath, asset, aerr)) {
			err = "write-back load failed '" + file + "': " + aerr;
			return false;
		}
		// Fragment -> blob index (mirrors the forward path: an empty
		// uriFragment addresses as "mesh.N").
		std::unordered_map<std::string, size_t> blobByFrag;
		for (size_t gi = 0; gi < asset.geometry.size(); ++gi) {
			const std::string& f = asset.geometry[gi].uriFragment;
			blobByFrag[f.empty() ? "mesh." + std::to_string(gi) : f] = gi;
		}
		if (asset.lodChains.size() < asset.geometry.size())
			asset.lodChains.resize(asset.geometry.size());
		for (const MeshRef& r : refs) {
			smvk::Cluster& c = *r.cluster;
			const smvk::StaticMeshUpload* up =
				c.GetRetainedUpload(r.handle);
			if (!up || up->vertices.empty() || up->indices.empty()) {
				err = "cooked mesh '" + c.GetMeshUri(r.handle).Str()
					+ "' has no retained upload (cook mode must retain)";
				return false;
			}
			const smvk::StringViewT uri = c.GetMeshUri(r.handle);
			const auto parts = smvk::SplitAssetUri(uri);
			std::string frag(parts.second.data(), parts.second.size());
			// Sibling fragments read "<base>/lodN" — strip the suffix
			// for the base match (the N must equal the LOD registry).
			uint32_t sibN = 0;
			std::string baseFrag = frag;
			if (r.lod.lodIndex >= 1) {
				const size_t slash = frag.rfind("/lod");
				if (slash == std::string::npos) {
					err = "sibling mesh '" + std::string(uri.data(), uri.size())
						+ "' is not a <base>/lodN fragment";
					return false;
				}
				sibN = uint32_t(std::stoul(frag.substr(slash + 4)));
				baseFrag = frag.substr(0, slash);
				if (sibN != r.lod.lodIndex) {
					err = "sibling LOD index disagrees with its fragment ('"
						+ std::string(uri.data(), uri.size()) + "')";
					return false;
				}
			}
			const auto bit = blobByFrag.find(baseFrag);
			if (bit == blobByFrag.end()) {
				err = "cooked mesh '" + std::string(uri.data(), uri.size())
					+ "' maps to no '" + file + "' blob (de-instance clones"
					" and collapse-merged blocks have no source fragment;"
					" cook without --lightmap/--collapse-static for now)";
				return false;
			}
			// Pack the FINAL upload back to the heap-stream layout
			// (PackStaticVertex: byte-stable round trip by design).
			smvk::assets::MeshAsset::Geometry geo;
			geo.positions.resize(up->vertices.size());
			geo.shading.resize(up->vertices.size());
			for (size_t vi = 0; vi < up->vertices.size(); ++vi)
				smvk::PackStaticVertex(up->vertices[vi],
					geo.positions[vi], geo.shading[vi]);
			geo.indices = up->indices;
			geo.surfaces = up->surfaces;
			geo.aabbMin = up->aabbMin;
			geo.aabbMax = up->aabbMax;
			// The skin stream (the Northlight data path): the cook's
			// write-back rebuilds every blob from the final upload - the
			// stream must ride it or the cooked .geometry silently drops
			// the skin (the skeleton fields on the asset survive, only
			// the per-blob streams were lost). The BASE blob carries it;
			// decimated LOD siblings stay static (the runtime skinning
			// slice owns per-LOD skin propagation).
			if (sibN < 1)
				geo.skin = up->skin;
			geo.uriFragment = asset.geometry[bit->second].uriFragment;
			if (auto ml = c.GetMeshMeshlets(uri); ml.has_value())
				geo.meshlets = *ml;
			else if (auto* m = c.staticMeshes.get(r.handle);
				m && m->meshlets.has_value())
				geo.meshlets = *m->meshlets;
			if (auto* m = c.staticMeshes.get(r.handle); m) {
				geo.lmChunk = m->lmChunk;
				geo.lmChartXforms = m->lmChartXforms;
			}
			if (!geo.meshlets.meshlets.empty())
				++st.meshleted;
			if (geo.lmChunk != 0xFFFFFFFFu)
				++st.lmBound;
			if (sibN >= 1) {
				auto& chain = asset.lodChains[bit->second];
				smvk::assets::MeshAsset::LodSibling sib;
				sib.geo = std::move(geo);
				sib.lodIndex = sibN;
				sib.simplifyError = r.lod.simplifyError;
				sib.screenThreshold = r.lod.screenThreshold;
				sib.triCount = r.lod.triCount;
				sib.acmr = r.lod.acmr;
				bool replaced = false;
				for (auto& e : chain) {
					if (e.lodIndex == sibN) {
						e = std::move(sib);
						replaced = true;
						break;
					}
				}
				if (!replaced)
					chain.push_back(std::move(sib));
				++st.siblings;
			}
			else {
				asset.geometry[bit->second] = std::move(geo);
				++st.blobs;
			}
		}
		std::string serr;
		if (!smvk::assets::SaveMeshAsset(hostPath, asset, serr)) {
			err = "write-back save failed '" + file + "': " + serr;
			return false;
		}
		// Manifest: existence + freshness is the pak contract (pak
		// refuses artifacts newer than their manifest = re-DERIVED
		// (import) or hand-touched after the cook).
		std::string stem = smvk::vfs::FileName(file);
		if (const size_t mdot = stem.rfind(".geometry");
			mdot != std::string::npos)
			stem = stem.substr(0, mdot);
		else if (const size_t mld = stem.rfind(".mesh");
			mld != std::string::npos)
			stem = stem.substr(0, mld);
		const std::filesystem::path manPath =
			hostPath.parent_path() / (stem + ".cooked");
		std::ofstream man(manPath, std::ios::binary | std::ios::trunc);
		if (!man) {
			err = "cannot write manifest '" + manPath.generic_string()
				+ "'";
			return false;
		}
		man << "cook=1\nflags=" << CookFlagsFingerprint(args) << "\n";
		man.close();
		if (!man) {
			err = "manifest write failed '" + manPath.generic_string()
				+ "'";
			return false;
		}
		// Reload proof: the saved artifact parses with the same blob
		// count (catches truncation, not content — content is proven
		// by the pak verify downstream).
		smvk::assets::MeshAsset proof;
		if (!smvk::assets::LoadMeshAsset(hostPath, proof, aerr)
			|| proof.geometry.size() != asset.geometry.size()) {
			err = "write-back reload mismatch '" + file + "': " + aerr;
			return false;
		}
		++st.artifacts;
		tool::out() << "Cooked '" << file << "' (blobs="
			<< asset.geometry.size() << ")\n";
	}
	return true;
}

// Runs the cook; returns the process exit code (byte-identical to the
// CLI run). See cook_tool.h.
// Funnel registration for one loose byproduct (see
// docs/artifact-db-funnel.md): upserts the (gi, uuid) identity row
// where uuid = content hash (deterministic bakes: identity ==
// content; the uri is already the canonical VFS path). Registration
// NEVER fails the cook (cache contract): false + one log line on any
// error. Null db (no registry root) skips silently — bare CLI cooks
// stay funnel-invisible.
bool RegisterByproduct(smvk::cache::CacheDb* db,
	const std::string& vfsPath, const uint8_t* bytes, size_t size,
	const char* producer) {
	if (!db || vfsPath.empty() || !bytes || size == 0)
		return true; // nothing to record (not an error)
	const std::string hash =
		smvk::cache::HashBytes(bytes, size);
	char meta[64];
	std::snprintf(meta, sizeof(meta), "{\"producer\":\"%s\"}",
		producer ? producer : "cook");
	std::string err;
	if (!db->Register(smvk::cache::kKindGi, hash, vfsPath, hash,
			meta, err)) {
		tool::err() << "  funnel: registration skipped for '"
			<< vfsPath << "': " << err << "\n" << std::flush;
		return false;
	}
	return true;
}

int Run(const CookArgs& c) {
	Args args;
	args.contentDir = c.contentDir;
	args.tour = c.tour;
	args.inVfsList = c.inVfsList;
	args.inVfs = c.inVfs;
	args.outPak = c.outPak;
	args.explicitSet = c.explicitSet;
	args.meshopt = c.meshopt;
	args.overdraw = c.overdraw;
	args.lods = c.lods;
	args.lodRatios = c.lodRatios;
	args.lodError = c.lodError;
	args.meshlets = c.meshlets;
	args.autoKtx = c.autoKtx;
	args.collapseStatic = c.collapseStatic;
	args.lightmap = c.lightmap;
	args.lmMetersPerTexel = c.lmMetersPerTexel;
	args.lmLobes = c.lmLobes;
	args.lmAtlasWidth = c.lmAtlasWidth;
	args.lmMaxHeight = c.lmMaxHeight;
	args.lmGutter = c.lmGutter;
	args.lmCompress = c.lmCompress;
	args.lmRays = c.lmRays;
	args.lmBounces = c.lmBounces;
	args.lmProxyRes = c.lmProxyRes;
	args.lmLobeRelErr = c.lmLobeRelErr;
	args.lmLodLock = c.lmLodLock;
	args.lmDebugPink = c.lmDebugPink;
	args.lmStitch = c.lmStitch;
	args.lmCurveTarget = c.lmCurveTarget;
	args.lmCurveMax = c.lmCurveMax;
	args.lmExclude = c.lmExclude;
	args.lmExcludeKeywords = c.lmExcludeKeywords;
	args.lmExcludeSmall = c.lmExcludeSmall;
	args.sunAzimuth = c.sunAzimuth;
	args.sunElevation = c.sunElevation;
	args.sunIntensity = c.sunIntensity;
	args.sunColor = c.sunColor;
	args.skyIntensity = c.skyIntensity;
	args.spots = c.spots;
	args.backend = c.backend;
			args.cubemap = c.cubemap;
			args.cubemapSize = c.cubemapSize;
	args.cubemapRays = c.cubemapRays;
	args.cubemapBounces = c.cubemapBounces;
	args.cubemapSpp = c.cubemapSpp;
	args.cubemapPos = c.cubemapPos;
	args.cubemapHeight = c.cubemapHeight;
	args.cubemapGrid = c.cubemapGrid;
	args.cubemapOctree = c.cubemapOctree;
	args.cubemapOctreeMax = c.cubemapOctreeMax;
	args.cubemapOctreeMin = c.cubemapOctreeMin;
	args.cubemapOctreeDepth = c.cubemapOctreeDepth;
	args.cubemapExtraPos = c.cubemapExtraPos;
	args.cubemapExtraRadius = c.cubemapExtraRadius;
	args.cubemapPng = c.cubemapPng;
	args.cubemapPngMips = c.cubemapPngMips;
	// (Both flags were MISSING here when they were added - the Run(c)
	// rebuild copies Args field-by-field from CookArgs, so a new flag
	// that only exists in the parse + the stages silently stays at its
	// default: the weave cloth never fired in the cook's stages, and
	// bistro's --cubemap-ambient-closure on never reached the captures.)
	args.cubemapAmbientClosure = c.cubemapAmbientClosure;
	args.maskTransmission = c.maskTransmission;
	args.cvolume = c.cvolume;
	args.quickBakes = c.quickBakes;
	args.dumpPak = c.dumpPak;
	args.dumpNodes = c.dumpNodes;
	args.registryDir = c.registryDir;
	// Quick-bakes mode (--quick-bakes on, the editor's "Update bakes now"
	// button): the capture stage ONLY, then stop. The heavy stages are
	// forced off (the cubemap stage needs just the imported trace scene);
	// the capture set runs --cubemap on regardless of the tour's cook
	// block. The loose bake files the stage writes are the whole output —
	// the early return below skips serialization + the pak write, so the
	// pak from the last full cook stays untouched (loose bake files override
	// its blobs through the runtime VFS overlay by the same paths).
	if (args.quickBakes) {
		args.lightmap = false;
		args.lods = false;
		args.collapseStatic = false;
		args.cvolume = false;
		args.cubemap = true;
		// Pin the forced gates in explicitSet so the tour cook-default
		// merge below ("CLI wins") cannot refill them from the tour's
		// cook block (a tour authored with cubemap off would otherwise
		// silently cancel the refresh).
		for (const char* k : { "lightmap", "lods", "cubemap" })
			args.explicitSet.insert(k);
		tool::out() << "quick-bakes: capture-only refresh (no pak write; "
			"loose cubemap bakes under gi/cubemaps/)\n";
	}

	// Lightmap bake (voxel-remesh proxy path): --lightmap on charts +
	// bakes a meshopt_remesh solid proxy of the scene once, then
	// UV-transfers the layout onto the real meshes. Needs exactly one
	// cooked scene (checked below).

	// Read-only inspection path: no VFS, no cooking, no writes.
	if (!args.dumpPak.empty()) {
		std::ifstream rf(args.dumpPak, std::ios::binary | std::ios::ate);
		if (!rf) {
			tool::err() << "Cannot open pak: " << args.dumpPak << std::endl;
			return 1;
		}
		const auto size = rf.tellg();
		rf.seekg(0);
		std::vector<uint8_t> image(static_cast<size_t>(size));
		if (!rf.read(reinterpret_cast<char*>(image.data()), size)) {
			tool::err() << "Read failed: " << args.dumpPak << std::endl;
			return 1;
		}
		std::string verr;
		auto reader = smvk::PakReader::Open(
			std::span<const uint8_t>(image), verr);
		if (!reader) {
			tool::err() << "PakReader rejected: " << verr << std::endl;
			return 1;
		}
		tool::out() << "Pak " << args.dumpPak << ": entries=" << reader->EntryCount()
			<< " meshes=" << reader->BlobsOfKind(smvk::PakAssetKind::Mesh).size()
			<< " materials=" << reader->BlobsOfKind(smvk::PakAssetKind::Material).size()
			<< " textures=" << reader->BlobsOfKind(smvk::PakAssetKind::Texture).size()
			<< " lights=" << reader->BlobsOfKind(smvk::PakAssetKind::Light).size()
			<< " cameras=" << reader->BlobsOfKind(smvk::PakAssetKind::Camera).size()
			<< " scenes=" << reader->BlobsOfKind(smvk::PakAssetKind::Scene).size()
			<< " gi=" << reader->BlobsOfKind(smvk::PakAssetKind::Gi).size()
			<< "\n";
		{
			std::string lerr;
			auto scenes = smvk::ListPakScenes(*reader, &lerr);
			if (scenes.empty() && !lerr.empty())
				tool::err() << "Scene list failed: " << lerr << std::endl;
			for (size_t i = 0; i < scenes.size(); ++i)
				tool::out() << "  scene[" << i << "] '" << scenes[i].name
				<< "' nodes=" << scenes[i].nodes << "\n";
		}
		DumpManifestFromPak(*reader, args.dumpNodes);
		return 0;
	}

	const auto t0 = std::chrono::steady_clock::now();

	// VFS: cooker reads inputs through the same overlay rules as runtime.
	smvk::OverlayVfs vfs;
	vfs.Mount(std::make_shared<smvk::LooseBackend>(args.contentDir), 0, "content");

	smvk::MeshOptConfig opt;
	opt.enabled = args.meshopt;
	opt.overdrawThreshold = args.overdraw;
	// Disable the importer's in-pass LOD generation. The legacy per-mesh
	// lightmap path cooks siblings before baking for UV transfer; the
	// successful proxy path cooks them afterward from stamped uploads.
	// Without lightmaps, the final stage produces the usual LOD chain.
	opt.generateLods = false;
	opt.lodRatios = ParseLodRatios(args.lodRatios);
	opt.lodTargetError = std::clamp(args.lodError, 0.0f, 1.0f);
	opt.lodSeamLock = (args.lmLodLock == "hard")
		? smvk::LodSeamLock::Hard : smvk::LodSeamLock::Attribute;

	smvk::MeshletConfig meshletCfg;
	meshletCfg.enabled = args.meshlets;
	// (canon sizes + cone pressure ride the MeshletConfig defaults: 64v/64t, coneWeight 0)
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

	// Project cache (convention, no flag): explicit programmatic roots
	// (the editor's store) arrive via args.registryDir; bare CLI cooks
	// join <contentDir>/.tourmaline when it already carries a cache.db
	// (never created here). Either way the registry accelerates texture
	// transcodes: hash-validated payloads serve from the registry on hit
	// and stage fresh ones on miss. A missing/locked/broken registry
	// degrades to plain processing — the pak output is byte-identical.
	std::unique_ptr<smvk::cache::CacheDb> registryDb;
	std::unique_ptr<smvk::cache::ArtifactStore> registryStore;
	std::unique_ptr<StoreArtifactCacheAdapter> registryAdapter;
	std::string registryRoot = args.registryDir; // explicit (editor/lib)
	bool registryDerived = false;
	if (registryRoot.empty()) {
		registryRoot = smvk::cache::ExistingRegistryRootFor(
			args.contentDir);
		registryDerived = !registryRoot.empty();
	}
	if (!registryRoot.empty()) {
		const std::string root =
			smvk::cache::EnsureRegistryRoot(registryRoot);
		if (!root.empty()) {
			registryDb = std::make_unique<smvk::cache::CacheDb>();
			std::string dbErr;
			if (registryDb->Open((std::filesystem::path(root) / "cache.db")
				.generic_string(), dbErr)) {
				registryStore =
					std::make_unique<smvk::cache::ArtifactStore>(
						root, *registryDb);
				registryAdapter =
					std::make_unique<StoreArtifactCacheAdapter>(
						*registryStore);
				tool::out() << "Asset registry: " << root
					<< (registryDerived ? " (derived)" : "") << "\n";
			}
			else {
				tool::err() << "Registry db open failed (" << dbErr
					<< ") — cooking without acceleration\n";
				registryDb.reset();
			}
		}
		else {
			tool::err() << "Registry root unusable: " << registryRoot
				<< " — cooking without acceleration\n";
		}
	}
	else {
		tool::out() << "No project cache under " << args.contentDir
			<< "/.tourmaline — plain cook (no acceleration, no funnel)\n";
	}
	GltfArtifactCache* artifactCache =
		registryAdapter ? registryAdapter.get() : nullptr;

	// Tour path: the doc IS the definition (root reference #0 = the
	// source glTF, inlined under the scene root exactly like the
	// editor binds it). No source resolution, no side imports — the
	// reference machinery builds the whole scene.
	std::vector<std::unique_ptr<smvk::Cluster>> sceneClusters;
	std::vector<std::string> sceneNames;
	smvk::tour::Doc tour;
	bool haveTour = false;

	if (!args.tour.empty()) {
		// Load tour: the DEFINITION (root reference = the source glTF,
		// other references inline beneath it, commands replay on top).
		std::string terr;
		std::vector<std::string> twarn;
		if (!smvk::tour::LoadTour(args.tour, tour, terr, &twarn)) {
			tool::err() << "Tour load failed: " << terr << std::endl;
			return 1;
		}
		haveTour = true;
		for (auto& w : twarn)
			tool::err() << "Tour warning: " << w << std::endl;
		// The tour's root reference doubles as the artifact identity when
		// the positional list is empty (the "Cooked <identity> -> ..." log
		// + BakeStems: bake logicals key on the SCENE stem, so
		// scene-keyed bake files survive differently-named out paks).
		if (args.inVfs.empty() && !tour.references.empty())
			args.inVfs = tour.references[0].source;
		// Export settings (the cook-block REPLACEMENT): authored on the
		// scene's exportSettings NODE (createNode -at exportSettings +
		// setExportAttr lines). The merge must land BEFORE the definition
		// build below (the reference bind consumes the meshopt/LOD/
		// meshlet configs), so read them straight off the doc's command
		// list (ExportSettingsFromDoc) — the replay consumes the SAME
		// lines through the SAME mapping, so the results agree by
		// construction. CLI flags still win (the explicitSet rule).
		SceneExportSettings es{};
		bool haveExportSettings = false;		{
			std::string esWarn;
			haveExportSettings = smvk::tour::ExportSettingsFromDoc(
				tour, es, &esWarn);
			if (!esWarn.empty())
				tool::err() << "Tour exportSettings: " << esWarn
					<< std::endl;
		}
		if (haveExportSettings)
			tool::out() << "Tour exportSettings node as CLI-losing "
				"defaults\n";
		// Apply tour export defaults (CLI wins)
		{
			auto use = [&](const char* key, auto& dst, auto val) {
				if (!args.explicitSet.contains(key)) {
					dst = val;
					tool::out() << "Tour cook default: " << key << "\n";
				}
			};
			use("meshopt", args.meshopt, es.meshopt);
			use("lods", args.lods, es.lods);
			use("lightmap", args.lightmap, es.lightmap);
			use("lmMetersPerTexel", args.lmMetersPerTexel, es.lmMetersPerTexel);
			use("lmLobes", args.lmLobes, es.lmLobes);
			use("lmAtlasWidth", args.lmAtlasWidth, es.lmAtlasWidth);
			use("lmRays", args.lmRays, es.lmRays);
			use("lmProxyRes", args.lmProxyRes, es.lmProxyRes);
			use("lmExcludeSmall", args.lmExcludeSmall, es.lmExcludeSmall);
			// NOTE: no sun/sky keys here — the env blocks (env.default.* /
			// env.n.*, applied by ApplyTourToCluster into the cluster
			// scene) are the sun/sky of record for every stage (see
			// EnvForBake).
			use("cubemap", args.cubemap, es.cubemap);
			use("cubemapSize", args.cubemapSize, es.cubemapSize);
			use("cubemapRays", args.cubemapRays, es.cubemapRays);
			// Placement density (the octree knobs — the inspector's
			// exportSettings section authors them; CLI flags win when
			// explicitly given).
			use("cubemapOctree", args.cubemapOctree, es.cubemapOctree);
			use("cubemapOctreeMax", args.cubemapOctreeMax, es.cubemapOctreeMax);
			use("cubemapOctreeMin", args.cubemapOctreeMin, es.cubemapOctreeMin);
			use("cubemapOctreeDepth", args.cubemapOctreeDepth, es.cubemapOctreeDepth);
			use("cubemapExtraRadius", args.cubemapExtraRadius, es.cubemapExtraRadius);
			// LOD detail + meshlet settings ride the exportSettings node
			// the same way (CLI flags win when explicitly given).
			use("lodRatios", args.lodRatios, es.lodRatios);
			use("lodError", args.lodError, es.lodError);
			use("meshlets", args.meshlets, es.meshlets);
			use("autoKtx", args.autoKtx, es.autoKtx);
			if (!es.lmExcludeKw.empty() &&
				!args.explicitSet.contains("lmExclude")) {
				if (!args.lmExclude.empty())
					args.lmExclude += ",";
				args.lmExclude += es.lmExcludeKw;
				tool::out() << "Tour cook default: lmExclude\n";
			}
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
		// Rebuild the meshopt/LOD/meshlet configs from the MERGED
		// args (the pre-branch build read the pre-merge values; the
		// tour block may have filled any of them).
		opt.enabled = args.meshopt;
		opt.lodRatios = ParseLodRatios(args.lodRatios);
		opt.lodTargetError = std::clamp(args.lodError, 0.0f, 1.0f);
	meshletCfg.enabled = args.meshlets;
	// (canon sizes + cone pressure ride the MeshletConfig defaults: 64v/64t, coneWeight 0)
	importMeshletCfg = meshletCfg;
		if (args.lightmap)
			importMeshletCfg.enabled = false;
		// DEFINITION BUILD — identical to the editor: empty scene root,
		// then the reference bind (root reference #0 imports the source
		// glTF under the scene root; the rest inline beneath it), then
		// the command replay. HOST-path resolution (vfs = nullptr) so
		// absolute and CWD-relative sources resolve exactly like the
		// editor session that authored the tour (the cooker inherits
		// the editor's working directory).
		{
			auto cluster = std::make_unique<smvk::Cluster>(nullptr);
			cluster->SetOfflineMode(true);
			addNode(cluster->scene, -1, 0); // empty scene root
			if (!tour.references.empty()) {
				const std::string ownerBase =
					smvk::vfs::BaseDir(tour.references[0].source);
				const size_t applied = ApplySceneReferences(*cluster, tour,
					ownerBase, nullptr, nullptr,
					nullptr /* HOST bind — editor parity */,
					opt, importMeshletCfg, 0, nullptr, nullptr, nullptr,
					args.autoKtx, tour.fileNodes.empty() ? nullptr : &tour.fileNodes,
					artifactCache, nullptr, nullptr,
					/*contentRoot=*/args.contentDir);
				tool::out() << "Definition: bound " << applied
					<< " reference subtree(s) ("
					<< tour.references.size() << " top-level)\n";
			}
			// Texture-asset POOL imports (ordered; see the tour's
			// textureAsset records): replayed BEFORE the material records
			// (a material tex ref or a light cookie naming a .texture must
			// find its pool entry — the shared ImportTextureAssetToCluster,
			// cook mode: no GPU). A failed import FAILS the cook loudly
			// (the cookie would otherwise resolve empty and cook a
			// wrong-texture pak).
			for (const auto& ta : tour.textureAssets) {
				// Resolve: project-relative records against --content-dir
				// FIRST (same convention as materialAsset records), then
				// as-is for absolute/out-of-project records.
				namespace fs = std::filesystem;
				fs::path hp;
				std::error_code ec;
				if (!args.contentDir.empty())
					hp = fs::path(args.contentDir) / ta.path;
				if (hp.empty() || !fs::exists(hp, ec))
					hp = fs::path(ta.path);
				if (!fs::exists(hp, ec)) {
					tool::err() << "Tour textureAsset not found: '"
						<< ta.path << "' (under --content-dir '"
						<< args.contentDir << "', then as-is)\n";
					return 1;
				}
				std::string imErr;
				if (smvk::assets::ImportTextureAssetToCluster(
					*cluster, hp, nullptr, imErr).empty()) {
					tool::err() << "Tour textureAsset import failed: '"
						<< ta.path << "': " << imErr << "\n";
					return 1;
				}
			}
			if (!tour.textureAssets.empty())
				tool::out() << "Definition: imported "
				<< tour.textureAssets.size()
				<< " texture asset(s)\n";
			// Material-asset POOL imports (ordered; see the tour's
			// materialAsset records): the editor session minted these into
			// the cluster pool beyond the reference-bound materials, and the
			// tour's assignMaterial/-m indices were recorded against that
			// pool — replay the records in order (the record order IS the
			// pool order) so the indices resolve exactly as authored. The
			// shared import is the same path the editor ran
			// (ImportMaterialAssetToCluster, cook mode: no GPU). A failed
			// import shifts every later index — FAIL the cook loudly
			// instead of cooking a wrong-material pak.
			for (const auto& ma : tour.materialAssets) {
				if (ma.editorDefault) {
					smvk::assets::EnsureClusterDefaultMaterial(*cluster);
					continue;
				}
				// Resolve: project-relative records against --content-dir
				// FIRST (the record's in-project convention — as-is/CWD would
				// hit the BUNDLED content tree over the project for same-named
				// sources), then as-is for absolute/out-of-project records.
				namespace fs = std::filesystem;
				fs::path hp;
				std::error_code ec;
				if (!args.contentDir.empty())
					hp = fs::path(args.contentDir) / ma.path;
				if (hp.empty() || !fs::exists(hp, ec))
					hp = fs::path(ma.path);
				if (!fs::exists(hp, ec)) {
					tool::err() << "Tour materialAsset not found: '"
						<< ma.path << "' (under --content-dir '"
						<< args.contentDir << "', then as-is)\n";
					return 1;
				}
				std::string imErr;
				if (smvk::assets::ImportMaterialAssetToCluster(
					*cluster, hp, nullptr, imErr).empty()) {
					tool::err() << "Tour materialAsset import failed: '"
						<< ma.path << "': " << imErr << "\n";
					return 1;
				}
			}
			if (!tour.materialAssets.empty())
				tool::out() << "Definition: imported "
				<< tour.materialAssets.size()
				<< " material asset(s)\n";
			std::vector<std::string> rwarn;
			std::string rerr;
			if (!smvk::tour::ApplyTourToCluster(*cluster, tour, &rwarn,
				&rerr)) {
				tool::err() << "Tour replay failed: " << rerr << std::endl;
				return 1;
			}
			for (auto& w : rwarn)
				tool::err() << "Tour replay warning: " << w << std::endl;
			tool::out() << "Tour replayed: " << tour.commands.size()
				<< " commands from " << args.tour << "\n";
			sceneNames.push_back("scene");
			sceneClusters.push_back(std::move(cluster));
		}
	}
	else {
		// No tour: ARTIFACT cook — the POSITIONAL artifact list (the
		// ALREADY-PROCESSED .geometry blobs; the MeshImporter/import.exe
		// output). glTF is IMPORT-ONLY and never cooks (the old
		// GltfToClusterFromVfs in-cook glTF import is GONE): open/drop
		// the .gltf in the editor (derive-if-stale) or run import.exe,
		// then cook the .geometry. Each artifact carries the ONE derived
		// scene (the source import's default scene) — no multi-scene
		// fan-out, no --scene-index; the pak cooks ALL listed artifacts.
		// Resolve + read each .geometry through the VFS (the runtime
		// content abstraction — loose files today; pak-embedded /
		// patched artifacts land through the same reads later).
		for (const std::string& inVfs : args.inVfsList) {
			std::string artPath = smvk::NormalizeAssetUri(
				inVfs).Str();
			if (artPath.empty())
				artPath = inVfs; // host-style path (as-is)
			auto artBytes = vfs.TryRead(artPath);
			if (!artBytes || artBytes->empty()) {
				tool::err() << "Artifact read failed: '" << inVfs
					<< "' not in VFS (want the derived .geometry — import "
					"the source glTF first: import.exe / the editor's "
					"MeshImporter)" << std::endl;
				return 1;
			}
			smvk::assets::MeshAsset meshAsset;
			std::string aerr;
			if (!smvk::assets::LoadMeshAssetBytes(artBytes->data(),
				artBytes->size(), meshAsset, aerr)) {
				tool::err() << "Artifact parse failed: '" << inVfs
					<< "': " << aerr << " (re-import the source glTF — "
					"the MeshImporter overwrites the artifacts)"
					<< std::endl;
				return 1;
			}
			// The manifest (<stem>.gltf.meta beside the blob): the
			// .material file table the artifact build resolves.
			smvk::assets::AssetManifest meshManifest;
			{
				std::string stem = smvk::vfs::FileName(artPath);
				const size_t mdot = stem.rfind(".geometry");
				const size_t mdotLegacy = stem.rfind(".mesh");
				if (mdot != std::string::npos)
					stem = stem.substr(0, mdot);
				else if (mdotLegacy != std::string::npos)
					stem = stem.substr(0, mdotLegacy);
				const std::string metaRel = stem + ".gltf.meta";
				if (auto metaBytes = vfs.TryRead(smvk::vfs::Join(
					smvk::vfs::BaseDir(artPath), metaRel))) {
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
			// NOTE: heap-allocated — Cluster embeds multi-MB pool arrays
			// and must never live on the (1MB) thread stack. Cook mode =
			// GPU-free: CPU geometry + retained uploads only, textures
			// are minted cook ids.
			auto cluster = std::make_unique<smvk::Cluster>(nullptr);
			cluster->SetOfflineMode(true);
			addNode(cluster->scene, -1, 0); // empty scene root (editor parity)
			const std::string baseDir = smvk::vfs::BaseDir(artPath);
			auto texSrc = std::make_shared<VfsTextureSource>(&vfs, baseDir);
			const size_t built = BuildFromMeshAsset(*cluster, nullptr,
				meshAsset, meshManifest, artPath, 0, texSrc, args.autoKtx,
				artifactCache, readSibling);
			if (built == 0) {
				tool::err() << "Artifact build failed: '" << inVfs
					<< "' built 0 nodes (degenerate/empty — re-import "
					"the source glTF)" << std::endl;
				return 1;
			}
			{
				size_t nDir = 0, nPoint = 0, nSpot = 0;
				cluster->lights.forEachHandle(
					[&](smvk::ClusterLightHandle h) {
					const SceneLight* l = cluster->lights.get(h);
					if (!l)
						return;
					if (l->type == SceneLight::Type::Directional) ++nDir;
					else if (l->type == SceneLight::Type::Spot) ++nSpot;
					else ++nPoint;
				});
				tool::out() << "Artifact built: " << built << " nodes, "
					<< cluster->lights.numObjects() << " lights"
					<< " (dir=" << nDir << " point=" << nPoint
					<< " spot=" << nSpot << ", links="
					<< cluster->scene.lightForNode.size() << ")\n";
			}
			tool::out() << "Cooking artifact: '" << inVfs << "'\n";
			sceneNames.push_back("");
			sceneClusters.push_back(std::move(cluster));
		}
	} // close else block for the artifact path
	// Import phase finished: commit any staged registry artifacts (one
	// pak grow per touched class). Flush failure loses acceleration
	// only — staged bytes were already used in the import.
	if (registryStore && registryStore->StagedCount() > 0) {
		const size_t staged = registryStore->StagedCount();
		std::string flushErr;
		if (registryStore->Flush(flushErr))
			tool::out() << "Asset registry: " << staged
			<< " texture artifact(s) flushed\n";
		else
			tool::err() << "Registry flush failed: " << flushErr
			<< " (staged stands; the pak output is unaffected)\n";
	}
	// Single-scene alias for the tour/collapse/bake stages below (written
	// against one cluster; multi-scene loops reuse them per scene). Both
	// cluster-> and *cluster forms keep working through the pointer.
	smvk::Cluster* cluster = sceneClusters.front().get();

	// World sun per scene (App parity: first directional wins as the
	// runtime sun). Authored directionals always survive; sunless scenes
	// synthesize one from the ENV BLOCK (EnvForBake — one authored sun).
	for (auto& sc : sceneClusters)
		EnsureWorldSun(*sc, EnvForBake(*sc, args));

	// Sun of record -> the scene's look env (sun fields only): an
	// AUTHORED directional (imported KHR punctual or tour-created) is
	// scene CONTENT — it wins over the env block's sun fields so exactly
	// ONE sun reaches the App (the env keeps its sky/fog/exposure). The
	// env block is lux-native, so the light's lux copies straight in (no
	// conversion). A synthesized WorldSun already carries the env's own
	// values — the seed is a no-op there (float-exact round trip).
	for (auto& sc : sceneClusters) {
		if (!sc->scene.envAuthored)
			continue; // runtime seeds from the light at load
		const ::Scene& scene = sc->scene;
		glm::vec3 toSun(0.0f);
		glm::vec3 sunColor(0.0f);
		float sunLux = 0.0f;
		bool hasSun = false;
		// Deterministic node order (hash-map iteration is not); the
		// aim axis after the coordFix basis change (the same plain
		// read as ResolveSceneLights / CollectStaticBakeLights).
		std::vector<uint32_t> nodes;
		nodes.reserve(scene.lightForNode.size());
		for (const auto& [node, lightIdx] : scene.lightForNode)
			nodes.push_back(node);
		std::sort(nodes.begin(), nodes.end());
		const glm::vec3 aimAxis = GltfPunctualAimAxis(0);
		for (uint32_t node : nodes) {
			if (node >= scene.globalTransform.size())
				continue;
			const auto lit = scene.lightForNode.find(node);
			if (lit == scene.lightForNode.end())
				continue;
			const smvk::ClusterLightHandle handle =
				sc->lights.getHandle(lit->second);
			if (!handle.valid())
				continue;
			const SceneLight* L = sc->lights.get(handle);
			if (!L || L->type != SceneLight::Type::Directional)
				continue;
			glm::vec3 emis = glm::vec3(
				scene.globalTransform[node] * glm::vec4(aimAxis, 0.0f));
			if (glm::length(emis) < 1e-6f)
				continue;
			toSun = -glm::normalize(emis);
			sunColor = L->color;
			sunLux = L->intensity;
			hasSun = true;
			break; // single-sun renderer: first directional wins
		}
		if (!hasSun)
			continue;
		SceneEnv& e = sc->scene.environment;
		e.sunElevation = glm::degrees(
			std::asin(glm::clamp(toSun.y, -1.0f, 1.0f)));
		e.sunAzimuth = glm::degrees(std::atan2(toSun.x, toSun.z));
		if (e.sunAzimuth < 0.0f)
			e.sunAzimuth += 360.0f;
		e.sunColor = sunColor;
		e.sunIntensity = sunLux; // lux-native env: straight in
		tool::out() << "Env sun resolved from the sun light: az="
			<< e.sunAzimuth << " el=" << e.sunElevation
			<< " I=" << e.sunIntensity << " (lux)\n";
	}

	// GI blobs cook assembles for the pack (see the cvolume stage + the
	// loose-bake handling below): one list — in-process-baked cvolumes
	// (the no-loose-bake fallback), the discovered GiBaker bake files (probe
	// octree, regular/convex manifests + rows) and the lightmap .sglm.
	// Every entry is (vfsPath, bytes) at canonical gi/... paths: cook
	// writes them LOOSE beside the sources and registers them (the
	// funnel); pak embeds the same paths (PakBackend serves by path).
	std::vector<std::pair<std::string, std::vector<uint8_t>>> giBlobs;
	// Authored convex GI volumes: bake IN-PROCESS only as the fallback —
	// when the GiBaker cvolmanifest (stable path, see the discovery below)
	// already exists (the editor cook flow runs GiBaker from 0 FIRST),
	// its fresh bake wins (same definition, hand-tunable) and the
	// discovery stage embeds it. Baked before collapse/LODs: identical
	// bake inputs to GiBaker's definition bake (the tour cluster as
	// replayed), and cvolForNode node ids are still valid (collapse
	// consumes static subtrees later).
	if (args.cvolume) {
		const std::string giDir =
			(std::filesystem::path(args.contentDir) / "gi")
			.generic_string();
		const std::string pakStem =
			std::filesystem::path(args.outPak).stem().generic_string();
		const std::string cvolSceneId = pakStem.empty()
			? std::string("pak") : pakStem;
		bool haveLooseManifest = false;
		for (const std::string& stem : BakeStems(args)) {
			std::error_code ec;
			if (std::filesystem::is_regular_file(
				std::filesystem::path(giDir)
				/ (stem + ".cvmanifest"), ec)) {
				haveLooseManifest = true;
				break;
			}
		}
		if (haveLooseManifest) {
			tool::out() << "cvolume manifest found (stable path)"
				" — in-process bake skipped, the loose bake "
				"embeds below\n";
		}
		else if (!BakeAuthoredCvolumes(sceneClusters, args, cvolSceneId,
			giBlobs))
			return 1;
	}

	// Static collapse: bake frozen subtrees into per-material mega-mesh
	// blocks (world-baked verts, identity nodes). Runs before Serialize so
	// the pak holds the collapsed scene. Per scene (shared nothing).
	if (args.collapseStatic) for (auto& sc : sceneClusters) {
		smvk::Cluster* cluster = sc.get();
		smvk::StaticMergeOptions mopts;
		smvk::StaticMergeResult mr = smvk::CollapseStaticSubtrees(*cluster, mopts);
		if (!mr.ok) {
			tool::err() << "CollapseStaticSubtrees failed: " << mr.err << std::endl;
			return 1;
		}
		tool::out() << "Collapsed " << mr.srcNodesConsumed << " static nodes into "
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
				tool::out() << "Meshlets " << b.uri << ": count " << st.meshletCount
					<< " tris " << st.totalTriangles
					<< " split " << st.splitMeshlets
					<< " maxR " << st.maxRadius
					<< " meanR " << st.meanRadius
					<< " meanCutoff " << st.meanCutoff
					<< (st.degenerateCones
						? " DEGENERATE-CONES " + std::to_string(st.degenerateCones)
						: "")
					<< std::endl;
				}
				else if (!merr.empty()) {
					tool::out() << "Meshlets " << b.uri << " skipped: " << merr << std::endl;
				}
			}
		}
	}

	// ---------------------------------------------------------------------------
	// Lightmap cook (--lightmap on): bake 2D SG lightmaps. Stamp
	// per-vertex chart UVs and IDs, mesh chart transforms, and atlas
	// chunk IDs; write <contentDir>/gi/<scene>.sglm and embed its bytes
	// in the pak later. No bake work remains at runtime.
	// Sources: sun, static spots (NEE), and real texture-averaged
	// materials. Transmissive (Blend) meshes pass rays
	// through, tinted (no charts for them). Shared (instanced) meshes are
	// de-instanced above so every instance bakes its own coefficients.
	// Decal overlays (cook-stamped no-cast on non-MASK materials) are out
	// of both charts and trace (see GiSceneBuildOptions::excludeDecalOverlays).
	// Charts come from xatlas ONLY (GenerateLightmapChartsXatlas, brute-force
	// pack); atlas stays alive past the block for the post-bake stages.
	// ---------------------------------------------------------------------------
	smvk::gi::SgLightmapAtlas atlas;
	// Pool indices stamped in ATLAS-TEXEL space. The normalize pass
	// divides only these meshes' transforms by the final dimensions;
	// unstamped uploads retain their imported [0,1] UVs.
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
			tool::err() << "  [LM] de-instanced " << n << " shared meshes\n"
				<< std::flush;
		}

		// Texture averages so baked materials include texture content.
		GiSceneBuildOptions buildOpts{};
		{
			uint32_t averaged = 0;
			uint32_t textured = 0;
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
				// Full decoded texel data: the trace samples the baseColor
				// texture at the interpolated hit UV (AlbedoAt) — bakes
				// carry the texture detail, not flat averages.
				if (auto data = GiDecodeKtxTexelData(
						std::span<const uint8_t>(*src), kGiBakeTexelMaxDim)) {
					buildOpts.texelData[handle.index()] = std::move(*data);
					++textured;
				}
			}
			tool::err() << "  [LM] textures averaged: " << averaged
				<< " / " << cluster->ListTextureUris().size()
				<< " (decoded " << textured << ")\n" << std::flush;
		}
		// Sun/sky record from the scene's env block (EnvForBake — ONE
		// authored sun/sky everywhere, matching every other bake stage).
		const BakeEnv be = EnvForBake(*cluster, args);
		buildOpts.env.sunAzimuthDeg = be.sunAzimuth;
		buildOpts.env.sunElevationDeg = be.sunElevation;
		buildOpts.env.sunIntensity = be.sunIntensity;
		buildOpts.env.skyIntensity = be.skyIntensity;
		buildOpts.traceBackendEmbree = (args.backend == "embree");

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
				tool::err() << "Bad --spot (want px,py,pz,dx,dy,dz,innerDeg,"
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
			tool::err() << "[LM] trace scene failed: " << terr << std::endl;
			return 1;
		}
		// CLI --spot specs APPEND to the harvested static scene lights
		// (assignment would wipe them — both sets bake together).
		traceScene.spots.insert(
			traceScene.spots.end(), spots.begin(), spots.end());
		tool::err() << "  [LM] trace scene: " << traceScene.Bvh().TriangleCount()
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
			tool::err() << "  [LM] name exclusion: "
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
			// Authored opt-in (Scene::lightmap, subtree-scoped, default
			// OFF): the editor's per-object "bake lightmap" toggle,
			// stamped from glTF "smvk.lightmap" node extras (or the LM_
			// prefix). Only opted-in meshes chart; everything else keeps
			// its analytic/probe lighting and stays in the trace scene
			// (still occludes/casts).
			if (!IsLightmapNode(cluster->scene, int(node))) {
				++skipNoLm;
				continue;
			}
			// Legacy opt-out (Scene::noLightmap, subtree-scoped): force
			// skip even when opted in above. Same treatment: trace/cast,
			// probe-lit.
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
			// Curvature boost: smoothly bending meshes chart denser so
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
			tool::err() << "  [LM] xatlas node " << node
				<< " (curve x" << w.curveScale << ")\n" << std::flush;
			work.push_back(std::move(w));
		}

		// ---- Voxel-remesh scene proxy (bake once, transfer to real meshes) ----
		// Merges all bake-eligible meshes in WORLD space, remeshes the soup
		// with meshopt_remesh (solid + Solve) at --lm-proxy-res, charts +
		// bakes the proxy ONCE (uniform lobe count: the atlas is shared, so
		// per-mesh lobe selection cannot apply), then every real mesh
		// inherits the layout via TransferLightmapCharts (closest-point
		// projection onto the proxy, world-space on both sides). LOD
		// siblings are NOT pre-cooked here; they are generated post-bake
		// from the stamped uploads by the shared sibling stage below.
		// --lm-proxy-res 0 keeps the legacy per-mesh LOD0 path for A/B.
		bool proxyBaked = false;
		SgLightmapChartResult proxyChart;
		uint32_t proxyChunkBase = 0;
		SgLightmapBakeStats lstats{};
		uint32_t lmMeshes = 0;
		uint64_t seamDups = 0;
		if (args.lmProxyRes > 0 && !work.empty()) {
			const int proxyRes = std::clamp(args.lmProxyRes, 4, 256);
			// Merge eligible uploads in world space.
			std::vector<float> soupPos;
			std::vector<uint32_t> soupIdx;
			{
				size_t nv = 0, ni = 0;
				for (const LmWork& w : work) {
					const smvk::StaticMeshUpload* up =
						cluster->GetRetainedUpload(w.handle);
					if (up) {
						nv += up->vertices.size();
						ni += up->indices.size();
					}
				}
				soupPos.reserve(nv * 3);
				soupIdx.reserve(ni);
				for (const LmWork& w : work) {
					const smvk::StaticMeshUpload* up =
						cluster->GetRetainedUpload(w.handle);
					if (!up || up->vertices.empty()
						|| up->indices.empty())
						continue;
					const glm::mat4& g =
						cluster->scene.globalTransform[w.node];
					const uint32_t base =
						uint32_t(soupPos.size() / 3);
					for (const smvk::StaticVertex& v : up->vertices) {
						const glm::vec3 wp =
							glm::vec3(g * glm::vec4(v.pos, 1.0f));
						soupPos.push_back(wp.x);
						soupPos.push_back(wp.y);
						soupPos.push_back(wp.z);
					}
					for (uint32_t ix : up->indices)
						soupIdx.push_back(base + ix);
				}
			}
			tool::err() << "  [LM] proxy: merged soup verts="
				<< (soupPos.size() / 3) << " tris="
				<< (soupIdx.size() / 3) << " res=" << proxyRes
				<< "\n" << std::flush;
			if (!soupPos.empty() && !soupIdx.empty()) {
				size_t cap = meshopt_remesh(nullptr, 0,
					soupIdx.data(), soupIdx.size(),
					soupPos.data(), soupPos.size() / 3, 12,
					proxyRes, meshopt_RemeshSolve);
				tool::err() << "  [LM] proxy: remesh capacity=" << cap
					<< "\n" << std::flush;
				if (cap > 0 && cap <= 4000000) {
					std::vector<float> triSoup(cap * 9, 0.0f);
					const size_t ntri = meshopt_remesh(triSoup.data(),
						cap, soupIdx.data(), soupIdx.size(),
						soupPos.data(), soupPos.size() / 3, 12,
						proxyRes, meshopt_RemeshSolve);
					tool::err() << "  [LM] proxy: remeshed tris=" << ntri
						<< "\n" << std::flush;
					if (ntri > 0) {
						smvk::StaticMeshUpload proxyUp;
						proxyUp.vertices.resize(ntri * 3);
						proxyUp.indices.resize(ntri * 3);
						for (size_t i = 0; i < ntri * 3; ++i) {
							smvk::StaticVertex v{};
							v.pos = glm::vec3(triSoup[i * 3 + 0],
								triSoup[i * 3 + 1], triSoup[i * 3 + 2]);
							v.color = glm::vec4(1.0f);
							proxyUp.vertices[i] = v;
							proxyUp.indices[i] = uint32_t(i);
						}
						smvk::ComputeSmoothNormals(proxyUp);
						// Chart the proxy (single input, already world
						// space): halve the density until the fixed-width
						// atlas + height cap fit.
						const uint32_t atlasW =
							(std::max<uint32_t>(args.lmAtlasWidth, 256)
								+ 3u) & ~3u;
						const uint32_t kMaxAtlasH = args.lmMaxHeight;
						SgLightmapXatlasOptions pxopts;
						pxopts.gutterTexels = lmGutter;
						pxopts.deferTexelRaster = true;
						SgLightmapXatlasMeshInput pin;
						pin.upload = &proxyUp;
						pin.meshKey = 0;
						pin.densityScale = 1.0f;
						SgLightmapXatlasLevel bestLv;
						float fitD = 0.0f;
						float probeD = baseDensity;
						for (int round = 0; round < 32; ++round) {
							pxopts.metersPerTexel =
								1.0f / std::max(probeD, 1e-3f);
							SgLightmapXatlasLevel lv;
							std::string xerr;
							const std::vector<
								SgLightmapXatlasMeshInput> pins{ pin };
							if (!GenerateLightmapChartsXatlas(pins,
								pxopts, lv, xerr)) {
								tool::err() << "  [LM] proxy xatlas "
									<< probeD << " texels/m failed: "
									<< xerr << "\n" << std::flush;
							}
							else {
								std::string rerr;
								const RepackStatus rs =
									RepackXatlasLevel(lv, atlasW,
										kMaxAtlasH, rerr);
								if (rs == RepackStatus::Ok) {
									SgLightmapAtlas probe;
									std::vector<uint32_t> dummy;
									std::string aerr;
									if (AppendXatlasLevelToAtlas(probe,
										lv, { 0u }, args.lmLobes,
										atlasW, kMaxAtlasH, dummy,
										aerr)) {
										bestLv = std::move(lv);
										fitD = probeD;
										break;
									}
									tool::err() << "  [LM] proxy probe "
										<< probeD << " overflow: " << aerr
										<< "\n" << std::flush;
								}
								else if (rs
									== RepackStatus::InternalError) {
									tool::err() << "[LM] proxy repack "
										"internal error: " << rerr << "\n"
										<< std::flush;
									break;
								}
								else {
									tool::err() << "  [LM] proxy repack "
										<< probeD << ": " << rerr << "\n"
										<< std::flush;
								}
							}
							if (probeD <= 1.0f)
								break;
							probeD = std::max(probeD * 0.5f, 1.0f);
						}
						if (fitD > 0.0f && !bestLv.meshes.empty()
							&& bestLv.meshes[0].ok) {
							std::string rerr;
							const std::vector<
								SgLightmapXatlasMeshInput> pins{ pin };
							if (!RasterizeLightmapPackTexels(pins,
								bestLv, lmGutter, rerr)) {
								tool::err() << "[LM] proxy rasterize "
									"failed: " << rerr << "\n" << std::flush;
							}
							else {
								atlas = SgLightmapAtlas{};
								std::vector<uint32_t> bases;
								std::string aerr;
								if (!AppendXatlasLevelToAtlas(atlas,
									bestLv, { 0u }, args.lmLobes,
									atlasW, kMaxAtlasH, bases,
									aerr)) {
									tool::err() << "[LM] proxy atlas "
										"append regressed: " << aerr << "\n"
										<< std::flush;
								}
								else {
									atlas.width = atlasW;
									proxyChart = std::move(
										bestLv.meshes[0].chart);
									proxyChunkBase = bases.empty()
										? 0 : bases[0];
									tool::err() << "  [LM] proxy charted: "
										<< "density=" << fitD << " texels/m "
										<< "charts="
										<< proxyChart.charts.size()
										<< " atlas=" << atlas.width << "x"
										<< atlas.height << "\n" << std::flush;
									// Bake the proxy charts (world space
									// already): uniform lobe count, one
									// shared pool batch.
									SgLightmapBakeConfig pcfg{};
									pcfg.raysPerTexel = std::max<uint32_t>(
										args.lmRays, 16);
									pcfg.maxBounces = std::min<uint32_t>(
										std::max<uint32_t>(args.lmBounces,
											1), 8);
									pcfg.lobeRelError = 0.0f;
									pcfg.chooseLobeCount = false;
									std::vector<std::vector<
										SgLightmapTexel>> pTex;
									std::vector<SgLightmapChunkBatch>
										pBatches;
									pTex.reserve(
										proxyChart.charts.size());
									pBatches.reserve(
										proxyChart.charts.size());
									for (uint32_t ci = 0;
										ci < proxyChart.charts.size();
										++ci) {
										std::vector<SgLightmapTexel>& tx =
											pTex.emplace_back(
												proxyChart.charts[ci]
												.texels);
										SgLightmapChunkBatch b{};
										b.payload = &atlas.payloads[
											proxyChunkBase + ci];
										b.texels = &tx;
										b.meshKey = 0;
										b.chunkMeta = &atlas.chunks[
											proxyChunkBase + ci];
										pBatches.push_back(b);
									}
									if (BakeSgLightmapChunks(traceScene,
										args.lmLobes,
										std::span<const
										SgLightmapChunkBatch>(
											pBatches), pcfg, lstats,
										[](const char* line) {
										tool::out() << line;
									})) {
										if (args.lmStitch) {
											uint64_t stitched = 0;
											std::string serr;
											if (!StitchSgLightmapSeams(
												std::span<const
												SgLightmapChunkBatch>(
													pBatches), 0.5f,
												&stitched, serr))
												tool::err() << "[LM] proxy "
												"stitch failed: "
												<< serr << "\n"
												<< std::flush;
											else
												tool::err() << "  [LM] proxy "
												"stitch: " << stitched
												<< " blended\n"
												<< std::flush;
										}
										// Transfer to every real mesh +
										// stamp (world-space both sides).
										uint32_t okMeshes = 0;
										for (LmWork& w : work) {
											smvk::StaticMeshUpload*
												realUp =
												const_cast<smvk::
												StaticMeshUpload*>(
													cluster->
													GetRetainedUpload(
														w.handle));
											if (!realUp
												|| realUp->vertices.empty()
												|| realUp->indices.empty())
												continue;
											const glm::mat4& g = cluster->
												scene.globalTransform[
													w.node];
											const glm::mat3 g3(g);
											smvk::StaticMeshUpload worldTgt;
											worldTgt.vertices =
												realUp->vertices;
											worldTgt.indices =
												realUp->indices;
											for (smvk::StaticVertex& v :
												worldTgt.vertices) {
												v.pos = glm::vec3(g *
													glm::vec4(v.pos,
														1.0f));
												v.normal = glm::normalize(
													g3 * v.normal);
											}
											SgLightmapChartResult xfer;
											std::string terr;
											SgLightmapTransferStats tstat;
											if (!TransferLightmapCharts(
												proxyUp, proxyChart,
												worldTgt, xfer, terr,
												&tstat)) {
												tool::err() << "  [LM] proxy "
													"xfer failed node "
													<< w.node << ": "
													<< terr << "\n"
													<< std::flush;
												continue;
											}
											std::vector<uint32_t> chartOf;
											std::vector<glm::vec4> xforms;
											uint32_t uncharted = 0;
											const uint32_t lobes =
												StampLightmapUvs(*realUp,
													xfer, proxyChunkBase,
													atlas, lmGutter,
													args.lmLobes, chartOf,
													xforms, seamDups,
													uncharted,
													("proxy node "
														+ std::to_string(
															w.node))
													.c_str(), true);
											lmTexelStamped.insert(
												w.handle.index());
											if (smvk::StaticMesh* m =
												cluster->staticMeshes
												.get(w.handle)) {
												m->lmChunk = lobes;
												m->lmChartXforms =
													std::move(xforms);
											}
											w.chart = std::move(xfer);
											w.chunkBase = proxyChunkBase;
											w.density = fitD;
											++okMeshes;
											++lmMeshes;
										}
										tool::err() << "  [LM] proxy "
											"transferred: " << okMeshes
											<< "/" << work.size()
											<< " meshes\n" << std::flush;
										proxyBaked = okMeshes != 0;
									}
									else {
										tool::err() << "[LM] proxy bake "
											"failed\n" << std::flush;
									}
								}
							}
						}
						else {
							tool::err() << "[LM] proxy charting failed "
								"(falling back to per-mesh path)\n"
								<< std::flush;
						}
					}
				}
				else {
					tool::err() << "[LM] proxy remesh produced no usable "
						"triangles (falling back to per-mesh path)\n"
						<< std::flush;
				}
			}
			else {
				tool::err() << "[LM] proxy soup empty (falling back to "
					"per-mesh path)\n" << std::flush;
			}
		}

		// ---- Legacy per-mesh LOD sources (--lods on) ----
		// With no successful proxy bake, siblings cook FIRST. Each mesh
		// charts and bakes its own LOD0 upload
		// (LOD0) exactly once — one bake, one atlas level — and every
		// sibling level receives the layout through UV transfer below.
		// All LODs sample the same rects, so LOD switches never
		// re-light and the atlas stops growing per level. LOD0's charts
		// are the transfer source for the whole chain; the simplifier
		// runs chart-free. Skipped when the proxy baked: siblings are
		// generated post-bake from the stamped uploads instead.
		if (!proxyBaked && args.lods && !work.empty()) {
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
			tool::err() << "  [LM] frostbite: siblings cooked, LOD0 sources "
				<< frostChains << "/" << work.size()
				<< " with transfers (bake once on LOD0, UV-transfer "
				<< "siblings)\n" << std::flush;
		}

		if (!proxyBaked && !work.empty()) {
			// Xatlas pack convergence (LEGACY per-mesh path; the proxy
			// path above charts + bakes the remeshed scene proxy once):
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
						tool::err() << "  [LM] xatlas dropped node "
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
					tool::err() << "[LM] repack internal error at " << where
						<< ": " << xerr
						<< "\n[LM] this is a packer bug - please report;"
						" aborting instead of converging around it\n"
						<< std::flush;
					repackBug = true;
					return false;
				}
				tool::err() << "  [LM] " << where << ": " << xerr << "\n"
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
					tool::err() << "  [LM] xatlas probe " << worldDensity
						<< " texels/m failed: " << xerr << "\n" << std::flush;
					return false;
				}
				// Membership changes (drops) are only allowed before the
				// first fit: afterwards lv must stay parallel to the
				// stored winner, so an unchartable mesh just fails the
				// probe (halving/the floor path resolve it).
				if (fitDensity <= 0.0f) {
					xatlasDropUnchartable(lv);
				}
				else {
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
						tool::err() << "  [LM] xatlas probe " << worldDensity
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
			}
			else {
				// Phase 1: halve until a pack fits (feasible lower bound).
				float probeD = baseDensity;
				while (probeD > 1.0f) {
					if (++round > 32) {
						tool::err() << "[LM] atlas overflow\n";
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
						tool::err() << "[LM] atlas overflow (" << xerr
							<< "; raise --lm-meters-per-texel, --lm-atlas-width"
							<< " or --lm-max-height)\n";
						return 1;
					}
					xatlasDropUnchartable(bestLevel);
					if (!repackOrAbort(bestLevel, "floor pack")) {
						if (repackBug)
							return 1;
						tool::err() << "[LM] atlas overflow (" << xerr
							<< "; raise --lm-meters-per-texel, --lm-atlas-width"
							<< " or --lm-max-height)\n";
						return 1;
					}
					while (!xatlasProbeFits(bestLevel)) {
						if (++round > 32 || work.empty()) {
							tool::err() << "[LM] atlas overflow ("
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
						tool::err() << "  [LM] atlas round " << round
							<< ": drop node " << work[worst].node
							<< " (" << worstTexels << " texels)\n" << std::flush;
						work.erase(work.begin() + ptrdiff_t(worst));
						bestLevel.meshes.erase(
							bestLevel.meshes.begin() + ptrdiff_t(worst));
						if (!repackOrAbort(bestLevel, "drop round repack")) {
							if (repackBug)
								return 1;
							tool::err() << "[LM] atlas overflow ("
								<< work.size()
								<< " meshes remain; raise --lm-meters-per-texel,"
								" --lm-atlas-width or --lm-max-height)\n";
							return 1;
						}
					}
					fitDensity = 1.0f;
				}
				else {
					// Phase 2: geometric binary search in (fit, fail).
					// Winners are kept verbatim (no regen — see above).
					while (failDensity / fitDensity > 1.10f) {
						if (++round > 32)
							break;
						const float mid =
							std::sqrt(fitDensity * failDensity);
						if (tryXatlasPack(mid)) {
							fitDensity = mid;
						}
						else {
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
					tool::err() << "[LM] winning level rasterize failed: "
						<< rerr << "\n";
					return 1;
				}
				std::string overlapErr;
				if (!CheckXatlasLevelRectsForOverlap(bestLevel,
					&overlapErr)) {
					tool::err() << "[LM] winning level rect overlap: "
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
					tool::err() << "[LM] atlas convergence regressed: "
						<< aerr << "\n";
					return 1;
				}
				if (!CheckAtlasChunksForOverlap(atlas, &overlapErr)) {
					tool::err() << "[LM] atlas chunk overlap after Append: "
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
			tool::err() << "  [LM] xatlas density converged at " << fitDensity
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
					tool::err() << "  [LM] curvature boost: " << boosted
					<< "/" << work.size() << " meshes denser, max x"
					<< maxScale << " (effective up to "
					<< fitDensity * maxScale << " texels/m)\n"
					<< std::flush;
			}
		}

		// Bake every chart (LEGACY per-mesh path; skipped when the proxy
		// baked above). Shared-pool batch, then stamp into the uploads.
		if (!proxyBaked) {
			SgLightmapBakeConfig lcfg{};
			lcfg.raysPerTexel = std::max<uint32_t>(args.lmRays, 16);
			lcfg.maxBounces = std::min<uint32_t>(std::max<uint32_t>(args.lmBounces, 1), 8);
			lcfg.lobeRelError = std::max(args.lmLobeRelErr, 0.0f);
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
					lstats, [](const char* line) { tool::out() << line; })) {
					tool::err() << "[LM] bake failed\n";
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
						tool::err() << "[LM] seam stitch failed: " << serr
							<< " (continuing on unstitched payloads)\n"
							<< std::flush;
					}
					else {
						tool::err() << "  [LM] seam stitch: " << stitched
							<< " boundary texels blended\n" << std::flush;
					}
				}
			}

			// (no denoise pass: embree path tracing at full spp carries
			// the bake; see below).
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
					tool::err() << "[LM] triangle count mismatch for node "
						<< w.node << " - mesh skipped\n";
					continue;
				}
				std::vector<uint32_t> chartOf;
				std::vector<glm::vec4> xforms;
				uint32_t uncharted = 0;
				const uint32_t lobes = StampLightmapUvs(*srcUpload, w.chart,
					w.chunkBase, atlas, lmGutter, args.lmLobes,
					chartOf, xforms, seamDups, uncharted,
					("src node " + std::to_string(w.node)).c_str(), false);
				lmTexelStamped.insert(w.handle.index());
				if (uncharted != 0)
					tool::err() << "  [LM] stamp node " << w.node
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
						tool::err() << "  [LM] transfer failed node " << w.node
							<< " pool " << th.index() << ": " << terr
							<< " (inherited chart uvs)\n" << std::flush;
						if (smvk::StaticMesh* m =
							cluster->staticMeshes.get(th))
							m->lmChunk = lobes;
						continue;
					}
					tool::err() << "  [LM] transfer node " << w.node
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
						dummy, xferXforms, seamDups, unch,
						("xfer node " + std::to_string(w.node)
							+ " pool " + std::to_string(th.index())).c_str(), true);
					lmTexelStamped.insert(th.index());
					if (unch != 0)
						tool::err() << "  [LM] transfer stamp node " << w.node
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
		} // end legacy per-mesh bake+stamp (skipped when proxy baked)





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
			tool::err() << "  [LM] texel-space transforms normalized: "
				<< order.size() << " meshes over " << atlas.width << "x"
				<< atlas.height << "\n" << std::flush;
		}

		lstats.seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - lmT0).count();
		// Write the loose lightmap atlas as gi/<scene>.sglm, using the
		// output-pak stem as the stable scene name. Embed these bytes in
		// the pak below; the matching Gi binding resolves the same path.
		if (!work.empty()) {
			std::filesystem::path pakPath(args.outPak);
			const std::string sceneId = pakPath.stem().generic_string();
			const std::filesystem::path giDir =
				std::filesystem::path(args.contentDir) / "gi";
			std::filesystem::path sglmPath = giDir / (sceneId + ".sglm");
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
					tool::err() << "[LM] bc6h compression failed: " << werr
						<< std::endl;
					return 1;
				}
				const size_t rawBytes = size_t(atlas.width) * atlas.height
					* kLightmapLayerCount * 8;
				const double bcSecs = std::chrono::duration<double>(
					std::chrono::steady_clock::now() - bcT0).count();
				tool::err() << "  [LM] bc6h: " << atlas.bc6hLobes
					<< " lobe image(s), " << (atlas.GpuBytes() / (1024 * 1024))
					<< " MiB gpu vs " << (rawBytes / (1024 * 1024))
					<< " MiB raw ("
					<< (args.lmCompress == SgLightmapCompression::Bc6HOpt
						? "opt prefill" : "plain") << ", " << bcSecs
					<< "s)\n" << std::flush;
			}
			if (!atlas.WriteFile(sglmPath.generic_string(), werr)) {
				tool::err() << "[LM] atlas write failed: " << werr << std::endl;
				return 1;
			}
			tool::err() << "  [LM] atlas " << sglmPath.generic_string()
				<< " (" << std::filesystem::file_size(sglmPath) << " B)\n" << std::flush;
		// Stage the .sglm payload for the pack (the atlas textures
		// already ride as Texture blobs; the binding below names the
		// scene stem) so the lightmap payload is registered like every
		// other GI blob (see the giBlobs write + register below).
		// Loose files still override by the same path.
			{
				const std::string vfsPath =
					"gi/" + sceneId + ".sglm";
				std::vector<uint8_t> bytes =
					ReadLooseBakeFile(args.contentDir, vfsPath);
				if (bytes.empty())
					tool::err() << "[LM] sglm embed skipped ('" << vfsPath
					<< "' unreadable)\n" << std::flush;
				else
					giBlobs.emplace_back(vfsPath, std::move(bytes));
			}
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
					tool::err() << "  [LM] atlas png " << pngPath.generic_string()
						<< " (" << std::filesystem::file_size(pngPath)
						<< " B)\n" << std::flush;
				}
				else {
					tool::err() << "[LM] atlas png write failed: " << perr
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
					tool::err() << "  [LM] charts png " << cPath.generic_string()
						<< " (" << std::filesystem::file_size(cPath)
						<< " B)\n" << std::flush;
				}
				else {
					tool::err() << "[LM] charts png write failed: " << perr
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
					tool::err() << "  [LM] pink png " << pinkPath.generic_string()
						<< " (" << std::filesystem::file_size(pinkPath)
						<< " B)\n" << std::flush;
				}
				else {
					tool::err() << "[LM] pink png write failed: " << perr
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
					tool::err() << "  [LM] chart rect audit: " << ringPairs
						<< " ring / " << contentPairs
						<< " content overlapping pairs; transforms in "
						<< dbgPath.generic_string() << "\n" << std::flush;
				}
				else {
					tool::err() << "[LM] chart debug dump open failed: "
						<< dbgPath.generic_string() << "\n" << std::flush;
				}
			}
			// Name the atlas inside the (single) cooked cluster so the
			// runtime resolves gi/<name>.sglm from the pak, not the id.
			// The name is the scene stem (stable, like the payload path).
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
			tool::err() << "  [LM] lobe pick mesh " << pick.meshKey
				<< " chosen=" << pick.chosen;
			for (uint32_t s = 0; s < pick.candCount; ++s)
				tool::err() << " " << pick.candCounts[s]
				<< "(rel " << (pick.relErr[s] * 100.0) << "%"
				<< ", deg " << (pick.degErr[s] * 100.0) << "%)";
			tool::err() << "\n" << std::flush;
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
			tool::err() << "  [LM] lobe chunks: 5=" << lobes5
				<< " 6=" << lobes6
				<< " 9=" << lobes9
				<< " 12=" << lobes12 << "\n" << std::flush;
		}
		tool::err() << "  [LM] lightmaps: meshes=" << lmMeshes
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
	std::string cubemapManifestBase;
	if (args.cubemap) {
		if (!BakePreconvolvedCubemaps(*cluster, args, cubemapKtx,
			cubemapManifestBase))
			return 1;
	}
	// Funnel: the loose bake files above (KTX2s + manifest, written
	// inside the bake) register their identity rows when a registry DB
	// is open. Covers quick-bakes too (the early return below).
	if (registryDb && !cubemapKtx.empty()) {
		size_t regged = 0;
		for (auto& [vfsPath, bytes] : cubemapKtx)
			if (RegisterByproduct(registryDb.get(), vfsPath,
					bytes.data(), bytes.size(), "cook:cubemap"))
				++regged;
		tool::out() << "Funnel: " << regged << "/" << cubemapKtx.size()
			<< " cubemap bake files registered\n";
	}

	// Quick-bakes stop HERE: the capture stage above wrote the loose
	// bake files (gi/cubemaps/<base>[_N].ktx2 + the .cubemanifest) —
	// that IS the quick-refresh output. Serialization, the pak write and
	// every post-capture stage below belong to the full cook; skipping
	// them keeps the last pak untouched (its blobs stay overridden by
	// these fresh loose files through the runtime VFS overlay).
	if (args.quickBakes) {
		tool::out() << "quick-bakes: cubemap captures updated (loose bake files"
			" under gi/cubemaps/) - pak write skipped\n";
		return 0;
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
	// build and the pass skips them).
	//
	// The gate is meshletCfg.enabled ALONE now (was args.lightmap &&):
	// with --lightmap off and a .geometry-source cook (the mesh-asset
	// import path - BuildFromMeshAsset, NOT the glTF importer), the
	// BASE meshes never got meshlets anywhere: the glTF importer
	// builds them inline, but the mesh-asset import does NOT, the
	// sibling cook only covers siblings, and this stage was the only
	// base-mesh build - so every base mesh shipped count 0 and ALL of
	// LOD0 (what draws up close) went through the unfiltered direct
	// path (observed at runtime: 103 bases direct, 170 siblings
	// filtered - "sponza stone has no meshlets"). With --lightmap on
	// nothing changes: import-time meshlets were disabled, so the same
	// meshes build here. The base pass SKIPS meshes that already carry
	// a set (the glTF-imported bases keep their inline build; a
	// rebuild from the same upload would be identical, but the skip
	// keeps the stage cheap and the provenance honest).
	// ---------------------------------------------------------------------------
	if (meshletCfg.enabled) {
		uint32_t restoredMeshes = 0;
		uint32_t restoredMeshlets = 0;
		// Shape-quality aggregates over the restored sets (the dashboard:
		// maxR/meanR in mesh-local model units, meanCutoff = meshopt's
		// cos(half-angle), split = facing-split repairs; see MeshletStats).
		float maxR = 0.0f;
		double radiusSum = 0.0;
		double cutoffSum = 0.0;
		uint32_t degenerate = 0;
		uint32_t splitsSum = 0;
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
				const auto uri = c.GetMeshUri(h);
				if (!uri.empty() && c.GetMeshMeshlets(uri).has_value())
					continue; // inline import build already covers it
				smvk::CookedMeshlets cooked;
				smvk::MeshletStats mst;
				std::string merr;
				if (!smvk::BuildMeshletsForUpload(*up, meshletCfg,
					&cooked, &mst, &merr)) {
					if (!merr.empty() && merr != "meshlets disabled")
						tool::err() << "  [LM] meshlet build failed mesh "
							<< meshIdx << ": " << merr
							<< " (mesh ships without meshlets)\n" << std::flush;
					continue;
				}
				if (!uri.empty())
					c.RegisterMeshMeshlets(uri, cooked);
				++restoredMeshes;
				restoredMeshlets += mst.meshletCount;
				maxR = std::max(maxR, mst.maxRadius);
				radiusSum += (double)mst.meanRadius * mst.meshletCount;
				cutoffSum += (double)mst.meanCutoff * mst.meshletCount;
				degenerate += mst.degenerateCones;
				splitsSum += mst.splitMeshlets;
				if (mst.degenerateCones > 0 || mst.meanCutoff < 0.5f)
					tool::err() << "  [LM] meshlet quality mesh " << meshIdx
						<< " uri=" << uri.Str()
						<< (up->skin.empty() ? "" : " skinned")
						<< " meshlets=" << mst.meshletCount
						<< " tris=" << mst.totalTriangles
						<< " cutoff=" << mst.meanCutoff
						<< " R=" << mst.meanRadius << "/" << mst.maxRadius
						<< " degenerate=" << mst.degenerateCones
						<< " split=" << mst.splitMeshlets << "\n" << std::flush;
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
						tool::err() << "  [LM] meshlet build failed sibling "
							<< uri.Str() << ": " << merr << "\n" << std::flush;
					continue;
				}
				c.RegisterMeshMeshlets(uri, cooked);
				++restoredMeshes;
				restoredMeshlets += mst.meshletCount;
				maxR = std::max(maxR, mst.maxRadius);
				radiusSum += (double)mst.meanRadius * mst.meshletCount;
				cutoffSum += (double)mst.meanCutoff * mst.meshletCount;
				degenerate += mst.degenerateCones;
				splitsSum += mst.splitMeshlets;
				if (mst.degenerateCones > 0 || mst.meanCutoff < 0.5f)
					tool::err() << "  [LM] meshlet quality sibling " << uri.Str()
						<< (up->skin.empty() ? "" : " skinned")
						<< " meshlets=" << mst.meshletCount
						<< " tris=" << mst.totalTriangles
						<< " cutoff=" << mst.meanCutoff
						<< " R=" << mst.meanRadius << "/" << mst.maxRadius
						<< " degenerate=" << mst.degenerateCones
						<< " split=" << mst.splitMeshlets << "\n" << std::flush;
			}
		}
		tool::err() << "  [LM] meshlets restored: meshes=" << restoredMeshes
			<< " meshlets=" << restoredMeshlets
			<< " (final topology; bases included)"
			<< " split=" << splitsSum
			<< " maxR=" << maxR
			<< " meanR=" << (restoredMeshlets
				? radiusSum / (double)restoredMeshlets : 0.0)
			<< " meanCutoff=" << (restoredMeshlets
				? cutoffSum / (double)restoredMeshlets : 0.0)
			<< (degenerate ? " DEGENERATE-CONES " : "")
			<< (degenerate ? std::to_string(degenerate) : "")
			<< "\n" << std::flush;
	}

	// ---------------------------------------------------------------------------
	// Final LOD sibling pass: the legacy per-mesh lightmap branch has
	// already cooked siblings for UV transfer, so this skips those meshes.
	// After a successful proxy bake, this pass cooks siblings from the
	// stamped base uploads. Without lightmaps, it builds the normal LOD
	// chain here rather than during import.
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

	// GI bake-file write-back + funnel registration (the giBlobs
	// verdict): the in-process cvolumes + re-read sglm collected above
	// have no other outlet (cook writes no pak) — without this they
	// vanish. Every entry lands loose at its canonical gi/... path
	// (fresh bake wins) and registers its identity row when a registry
	// DB is open. The cvolume fallback only fires with no loose
	// manifest present, so nothing hand-tuned is clobbered.
	if (!giBlobs.empty()) {
		size_t wrote = 0, regged = 0;
		for (auto& [vfsPath, bytes] : giBlobs) {
			const std::filesystem::path loose =
				std::filesystem::path(args.contentDir) / vfsPath;
			std::error_code ec;
			std::filesystem::create_directories(loose.parent_path(),
				ec);
			std::ofstream lf(loose,
				std::ios::binary | std::ios::trunc);
			if (lf && !bytes.empty()
				&& lf.write(
					reinterpret_cast<const char*>(bytes.data()),
					std::streamsize(bytes.size()))) {
				++wrote;
				if (registryDb && RegisterByproduct(registryDb.get(),
						vfsPath, bytes.data(), bytes.size(),
						"cook:gi"))
					++regged;
			}
			else {
				tool::err() << "  [GI] loose bake file write failed for '"
					<< vfsPath << "'\n" << std::flush;
			}
		}
		tool::out() << "GI bake files: " << wrote << "/"
			<< giBlobs.size() << " written loose, " << regged
			<< " registered"
			<< (registryDb ? "" : " (no registry db — skipped)")
			<< "\n";
	}

	// COOK WRITE-BACK (no pak write): persist the cooked clusters into
	// their source .geometry artifacts + <stem>.cooked manifests.
	// pak.exe assembles the .pak from those (pure serialization); the
	// loose bake files the stages above wrote (cubemap .ktx2 +
	// .cubemanifest, lightmap .sglm, GiBaker volumes) are discovered by
	// pak at assembly time.
	const auto t1 = std::chrono::steady_clock::now();
	WriteBackStats wb;
	std::string werr;
	if (!WriteCookedArtifactsBack(args, sceneClusters, wb, werr)) {
		tool::err() << "Write-back failed: " << werr << std::endl;
		return 1;
	}
	const double secs = std::chrono::duration<double>(t1 - t0).count();
	tool::out() << "Cooked " << args.inVfs
		<< (args.inVfsList.size() > 1
			? " +" + std::to_string(args.inVfsList.size() - 1)
				+ " more artifact(s)"
			: (!args.tour.empty() ? " (tour definition)" : ""))
		<< " -> cooked artifacts\n"
		<< "  scenes=" << sceneClusters.size()
		<< " artifacts=" << wb.artifacts
		<< " blobs=" << wb.blobs
		<< " siblings=" << wb.siblings
		 << " meshleted=" << wb.meshleted << "/" << wb.blobs << " blobs"
		 << " lmBound=" << wb.lmBound
		 << " in " << secs << "s\n";
	return 0;
}

} // namespace cooktool

// The CLI stays a thin interface over the library (same argv, same
// output, same exit codes). The lib build (COOK_LIB_BUILD, linked
// into the editor) excludes main.
#ifndef COOK_LIB_BUILD
int main(int argc, char** argv) {
	// Unbuffered pipe output: hosts tailing this process through a
	// pipe stream each line live. Without this, stdout stays
	// block-buffered when piped and no progress appears until the
	// cook finishes (looks stuck).
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;
	cooktool::Args args;
	if (!cooktool::ParseArgs(argc, argv, args)) {
		cooktool::Usage();
		return 2;
	}
	cooktool::CookArgs c;
	c.contentDir = args.contentDir;
	c.tour = args.tour;
	c.inVfsList = args.inVfsList;
	c.inVfs = args.inVfs;
	c.outPak = args.outPak;
	c.explicitSet = args.explicitSet;
	c.meshopt = args.meshopt;
	c.overdraw = args.overdraw;
	c.lods = args.lods;
	c.lodRatios = args.lodRatios;
	c.lodError = args.lodError;
	c.meshlets = args.meshlets;
	c.autoKtx = args.autoKtx;
	c.collapseStatic = args.collapseStatic;
	c.lightmap = args.lightmap;
	c.lmMetersPerTexel = args.lmMetersPerTexel;
	c.lmLobes = args.lmLobes;
	c.lmAtlasWidth = args.lmAtlasWidth;
	c.lmMaxHeight = args.lmMaxHeight;
	c.lmGutter = args.lmGutter;
	c.lmCompress = args.lmCompress;
	c.lmRays = args.lmRays;
	c.lmBounces = args.lmBounces;
	c.lmProxyRes = args.lmProxyRes;
	c.lmLobeRelErr = args.lmLobeRelErr;
	c.lmLodLock = args.lmLodLock;
	c.lmDebugPink = args.lmDebugPink;
	c.lmStitch = args.lmStitch;
	c.lmCurveTarget = args.lmCurveTarget;
	c.lmCurveMax = args.lmCurveMax;
	c.lmExclude = args.lmExclude;
	c.lmExcludeKeywords = args.lmExcludeKeywords;
	c.lmExcludeSmall = args.lmExcludeSmall;
	c.sunAzimuth = args.sunAzimuth;
	c.sunElevation = args.sunElevation;
	c.sunIntensity = args.sunIntensity;
	c.sunColor = args.sunColor;
	c.skyIntensity = args.skyIntensity;
	c.spots = args.spots;
	c.backend = args.backend;
	c.cubemap = args.cubemap;
	c.cubemapSize = args.cubemapSize;
	c.cubemapRays = args.cubemapRays;
	c.cubemapBounces = args.cubemapBounces;
	c.cubemapSpp = args.cubemapSpp;
	c.cubemapPos = args.cubemapPos;
	c.cubemapHeight = args.cubemapHeight;
	c.cubemapGrid = args.cubemapGrid;
	c.cubemapOctree = args.cubemapOctree;
	c.cubemapOctreeMax = args.cubemapOctreeMax;
	c.cubemapOctreeMin = args.cubemapOctreeMin;
	c.cubemapOctreeDepth = args.cubemapOctreeDepth;
	c.cubemapExtraPos = args.cubemapExtraPos;
	c.cubemapExtraRadius = args.cubemapExtraRadius;
	c.cubemapPng = args.cubemapPng;
	c.cubemapPngMips = args.cubemapPngMips;
	c.cubemapAmbientClosure = args.cubemapAmbientClosure;
	c.maskTransmission = args.maskTransmission;
	c.cvolume = args.cvolume;
	c.quickBakes = args.quickBakes;
	c.dumpPak = args.dumpPak;
	c.dumpNodes = args.dumpNodes;
	c.registryDir = args.registryDir;
	return cooktool::Run(c);
}
#endif
