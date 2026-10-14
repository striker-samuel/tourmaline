// pak.exe: offline ARTIFACT -> .pak cooker that works exclusively on the
// VFS and needs NO GPU (cook-mode Cluster, see Cluster::SetCookMode).
// The COOK-SPLIT (the clean separation): the cook inputs are the
// ALREADY-PROCESSED .geometry artifacts (import.exe / the MeshImporter /
// the editor's open-drop produce them - glTF is IMPORT-ONLY, never a
// cook input; see gltf_import/asset_export.h; the raw import lives in
// tools/import, ONE place).
//
//   pak.exe <vfs_path> <artifact...> [--options]
//             pak.exe F:/content models/sponza/Sponza.geometry
//                 --out-pak F:/content/paks/sponza.pak [--meshopt on|off]
//                 [--overdraw 1.05] [--verify on|off] [--dump-manifest on|off]
//                 [--lods on|off] [--lod-ratios 0.6,0.3,0.12] [--lod-error 0.02]
//                 [--compress on|off] [--lz4-accel 1]
//                 [--lightmap on|off --lm-meters-per-texel 0.25 --lm-lobes 12
//                  --lm-atlas-width 1024 --lm-gutter 1
//                  --lm-rays 1024
//                  --sun-azimuth 30 --sun-elevation 55 --sun-intensity 10000
//                  --lm-lod-lock attr|hard
//                  --lm-denoise on|off (DEPRECATED, off by default)
//                  --lm-denoise-quality high|balanced (DEPRECATED)
//                  --lm-curve-target 7.0 --lm-curve-max 2.5
//                  --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]
//
// --cubemap on bakes preconvolved SPECULAR cubemaps DURING the cook (same
// CPU path-tracer capture as tools/cubemap_baker, GGX importance-sampled
// mip chains via Engine/Renderer/cubemap_prefilter.h â€” roughness = mip /
// (levels-1)) and embeds them as Texture blobs at
// textures/cubemaps/<name>[_N].ktx2 (PakBackend serves them by VFS path,
// so CubemapLibrary::Load finds them with no code change). Probes and
// lightmaps carry all diffuse + low-frequency specular; these cubes own
// the high-frequency specular path only.
//
// --lightmap on bakes 2D SG lightmaps DURING the cook (sun + static spot
// NEE + analytic sky on open-sky rays, real materials, transmissive
// pass-through â€” see Engine/GI/bake.h). It stamps chart-local UVs and
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
// Denoising (DEPRECATED, off by default): with --lm-denoise on, the
// fitted per-lobe amplitude layers are denoised with Intel Open Image
// Denoise's RTLightmap filter (prebuilt release in external/oidn, linked
// into PakCooker only â€” engine and App never see OIDN). Denoised
// payloads are written into the .sglm; the runtime path is unchanged.
// --lm-rays stays independent: denoising recovers low-sample bakes, so
// try e.g. --lm-rays 128 for a ~8x faster trace.
//
// Curvature-weighted density (default on): --lm-meters-per-texel is the
// BASE request; smoothly bending meshes chart at up to --lm-curve-max
// times the base density so their normal-turn per texel stays under
// --lm-curve-target degrees (creases over 45 deg do not count â€” density
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
// textures and metadata. There is intentionally no sidecar pak manifest:
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
#include <artifact_cache/store_adapter.h>
#include <Engine/Vfs/vfs.h>
#include <meshoptimizer.h>

namespace {

	struct Args {
		std::string contentDir = "content";
		// POSITIONAL CLI (pak.exe <vfs_path> <artifact...> [--options]):
		// argv[1] = the VFS content root (was --content-dir), argv[2..]
		// until the first --option = the ALREADY-PROCESSED artifact list
		// (.geometry files - import.exe/the editor's MeshImporter
		// produces them; raw glTF NEVER cooks here). inVfsList holds the
		// artifacts; inVfs keeps the IDENTITY (the first artifact, or the
		// tour's root reference) for the logs/sidecar stems.
		std::vector<std::string> inVfsList;
		std::string inVfs; // the primary identity (first artifact / tour ref)
		std::string outPak;
		// Legacy glTF-era scene-selection flag. A .geometry artifact carries
		// one derived scene, and the artifact cook rejects --scene-index.
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
		// Size limits default to the ENGINE's kMeshletMax constants (64v/64t:
		// one mesh-shader workgroup covers a whole meshlet â€” see meshlet.h);
		// BuildMeshletsForUpload clamps over-limit configs (stale authored cooks)
		// down to them.
		bool meshlets = false;
		uint32_t maxVerts = smvk::kMeshletMaxVertices;
		uint32_t maxTris = smvk::kMeshletMaxTriangles;
		float coneWeight = 0.0f;
		// KTX auto-convert (on by default): non-KTX sources transcode
		// in-process (stb decode + libktx write, `.ktx2` sidecar cached next
		// to the source). Off restores warn-and-skip for non-KTX payloads.
		bool autoKtx = true;
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
		uint32_t lmLobes = 12;       // 5|6|9|12 â€” PREFERRED max; the bake
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
		// texture per lobe (8 B/texel per lobe) â€” ~6x smaller GPU footprint
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
		// anyway â€” probes serve them better, and the atlas keeps its texels
		// for walls and facades. 0 = no gating.
		float lmExcludeSmall = 0.0f;
		float sunAzimuth = 30.0f;    // mirrors the runtime environment defaults
		float sunElevation = 55.0f;
		float sunIntensity = 10000.0f;
		// World-sun color (linear RGB): with the angles/intensity above it IS
		// the cooked world sun â€” the bake env AND the pak runtime sun (a
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
		// packet/SIMD traversal â€” the fast default) or "builtin" (the
		// deterministic binned-SAH BVH reference; agrees within
		// Monte-Carlo noise).
		std::string backend = "embree";
		// Probe-volume sidecars this pak's scenes sample (repeatable):
		// registers gi/<name>.sggi names into the clusters (bake the matching
		// volumes with GiBaker --out). Runtime resolves by name.
		std::vector<std::string> giProbes;
		// ---- Preconvolved specular cubemaps (--cubemap on) ----
		// Captures the scene with the CPU path tracer at one or more points
		// (same transport as CubemapBaker), GGX-prefilters each capture for
		// the HIGH-frequency specular path (probes/lightmaps already carry
		// all diffuse + low-frequency specular), and embeds the KTX2 cubes
		// as Texture blobs at textures/cubemaps/<name>.ktx2 â€” served by
		// PakBackend through the VFS, so CubemapLibrary::Load finds them
		// with no code change. Empty cubemapName defaults to the out-pak
		// stem (sponza.pak -> "sponza", mirroring gi/<scene>.sglm).
		bool cubemap = false;
		std::string cubemapName;
		uint32_t cubemapSize = 128;   // base face width (HDR RGBA32F)
		uint32_t cubemapRays = 128;   // GGX importance samples per output texel
		uint32_t cubemapBounces = 5;  // diffuse path depth after the first hit
		// Transport samples per capture texel: the NEE sun/spot term is
		// analytic (clean at 1 spp), but the bounce field is a Monte-Carlo
		// path estimate â€” 1 spp reads as speckle, not GI, and the capture
		// stops representing the (multi-ray, denoised) lightmapped scene.
		// 8 spp smooths the bounce term at ~8x capture cost.
		uint32_t cubemapSpp = 8;
		std::vector<std::string> cubemapPos; // repeatable "x,y,z" (empty =
		// bounds center at cubemapHeight)
		float cubemapHeight = 1.5f;
		float cubemapGrid = 8.0f;     // grid spacing in meters (0 = single probe at center)
		// Sparse adaptive octree placement (Engine/Renderer/cubemap_placement):
		// occupancy-driven subdivision instead of a uniform lattice â€” open
		// volumes stay coarse, near-surface cells refine, embedded centers
		// drop, growth stops at the 32-probe runtime budget. Wins over --grid
		// when set (explicit --cubemap-pos still wins over both).
		bool cubemapOctree = false;
		float cubemapOctreeMax = 8.0f;   // open cells larger than this subdivide (coverage)
		float cubemapOctreeMin = 2.0f;   // detail floor: never subdivide below this
		uint32_t cubemapOctreeDepth = 4; // octree depth cap (root = 0)
		// Artist-pinned extra probes (repeatable "x,y,z"): appended AFTER
		// grid/octree placement and marked 'pin' in the manifest so the
		// runtime sorts them first â€” a pinned probe wins weight ties, e.g. a
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
		// Loose files under <contentDir>/textures/cubemaps/, never in the pak.
		bool cubemapPng = true;
		bool cubemapPngMips = true;
		// NOTE: captures store TRUE radiance (physical units); the runtime's
		// baked path scales by the cubemap panel intensity only, matching the
		// direct sun view. No bake-time intensity knob by design.
		// Authored convex GI volumes (tour cvolume nodes) baked INTO the pak
		// (the --cvolume stage: in-process BakeCvolume, embedded as Texture
		// blobs under gi/ â€” the runtime App resolves the manifest + volumes
		// straight out of the pak, no loose sidecars). Default on: authoring
		// the nodes IS the intent to bake them; --cvolume off skips (GiBaker
		// loose sidecars stay the on-demand/hand-tuning path).
		bool cvolume = true;
		// Quick-bakes mode (the editor's "Update bakes now" button): run the
		// capture stage ONLY and STOP â€” no pak write, no lightmap/LOD/collapse
		// stages. The loose sidecars the stage writes
		// (textures/cubemaps/<base>[_N].ktx2 + <base>.cubemanifest) are the
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
		// Editor scratch-disk root (optional acceleration, NEVER a
		// precondition): when set, the cook consults + feeds the artifact
		// registry there â€” texture transcodes copy hash-validated payloads
		// from scratch on hit and stage fresh ones on miss. Absent/broken
		// scratch = plain processing, unchanged output.
		std::string scratchDir;
	};

	// Bake/pak sun+sky source of record (see SceneEnv): the scene's LUX-NATIVE
	// env block when the tour carried one (the editor always writes env.default.*),
	// else the CLI sun defaults (bare cooks). Every stage â€” the synthesized
	// WorldSun, the lightmap / cubemap / cvolume bakes â€” reads this, so ONE
	// authored sun/sky drives the editor viewport, the bakes and the pak.
	struct BakeEnv {
		float sunAzimuth = 30.0f, sunElevation = 55.0f;
		float sunIntensity = 10000.0f, skyIntensity = 600.0f;
		glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f };
	};
	BakeEnv EnvForBake(const smvk::Cluster& c, const Args& args) {
		BakeEnv e{};
		if (c.scene.hasEnvironment) {
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
	// EnvForBake â€” one authored sun everywhere) so the editor's environment
	// settings travel pak -> App. Non-static by design: the bake harvests
	// static lights only, so the runtime sun never double-counts into
	// lightmaps.
	void EnsureWorldSun(smvk::Cluster& cluster, const BakeEnv& be) {
		bool hasDir = false;
		cluster.lights.forEachHandle([&](smvk::ClusterLightHandle h) {
			if (const SceneLight* l = cluster.lights.get(h))
				if (l->type == SceneLight::Type::Directional)
					hasDir = true;
		});
		if (hasDir || cluster.scene.hierarchy.empty())
			return;
		// toSun from compass azimuth (0 = +Z) + elevation â€” the same formula
		// as EnvironmentSettings::SunDirectionToSun (unit by construction).
		const float az = be.sunAzimuth * 0.01745329252f;
		const float el = be.sunElevation * 0.01745329252f;
		const float ce = std::cos(el);
		const glm::vec3 toSun{ std::sin(az) * ce, std::sin(el),
			std::cos(az) * ce };
		// Node frame: the aim reads G*(0,0,-1) in EVERY coordFix mode
		// (GltfPunctualAimAxis; raw locals, the basis rides the chain top),
		// so local -Z maps to the emission direction -toSun â€” local +Z =
		// toSun (orthonormal basis, det +1). Under the conjugated Sponza
		// root the plain +Z read flips (az/el inverted on the seed â€” the
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
		const auto h = cluster.CreateLight(std::move(sun));
		if (!h.valid())
			return;
		cluster.RegisterLightUri("sun/world", h);
		cluster.scene.lightForNode[uint32_t(node)] = h.index();
		std::cout << "World sun synthesized (env block): az=" << be.sunAzimuth
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

	// Reads a loose sidecar file by its content-root-relative vfs path
	// ("gi/<guid>.cvmanifest" -> <contentDir>/gi/<guid>.cvmanifest). Empty
	// on any failure (logged by the caller â€” a missing sidecar is "not
	// baked", never fatal).
	std::vector<uint8_t> ReadSidecarFile(const std::string& contentDir,
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

	// Sidecar name stems (see the GI-sidecar discovery in main): the pak
	// stem first (the runtime's fallback identity), then the source scene
	// stems (the artifact list - every listed .geometry, or the tour
	// reference identity). Bakes key on the SCENE they ran over
	// (GiBaker --scene, the .sggi stem) â€” without the scene stems a
	// differently-named out pak would silently miss its own bakes.
	std::vector<std::string> SidecarStems(const Args& args) {
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

	// Authored convex GI volumes baked INTO the pak (the --cvolume stage):
	// the same engine core as GiBaker (BuildTraceSceneFromCluster +
	// BakeCvVolume over the tour's cvolume nodes â€” identical bake inputs:
	// no collapse, no LODs, the definition cluster exactly as GiBaker sees
	// it). Output = (vfsPath, bytes) pairs for pak embedding: one .cvol
	// per volume at gi/<pak-stem>_<name>.cvol + the runtime manifest text
	// at gi/<pak-stem>.cvmanifest (bare-path rows: the runtime's
	// SplitManifestRow + vfs.ReadBinary resolve them straight out of the
	// pak through PakBackend's serve-Texture-by-path â€” the proven
	// cubemap-manifest embed pattern). Returns false on a bake failure â€”
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
			// axis, the env-block sun/sky record (EnvForBake â€” ONE authored
			// sun/sky everywhere), static light harvest) so the volume
			// matches what the rest of the cook bakes.
			const BakeEnv be = EnvForBake(*sc, args);
			GiSceneBuildOptions buildOpts{};
			uint32_t averaged = 0;
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
			}
			buildOpts.punctualAimAxis = GltfPunctualAimAxis(0);
			buildOpts.env.sunAzimuthDeg = be.sunAzimuth;
			buildOpts.env.sunElevationDeg = be.sunElevation;
			buildOpts.env.sunIntensity = be.sunIntensity;
			buildOpts.env.skyIntensity = be.skyIntensity;
			buildOpts.traceBackendEmbree = (args.backend == "embree");
			GiTraceScene traceScene;
			std::string terr;
			BuildTraceSceneFromCluster(*sc, buildOpts, traceScene, terr);
			if (!terr.empty()) {
				std::cerr << "[CV] trace scene failed: " << terr << std::endl;
				return false;
			}
			std::cout << "  [CV] trace scene: "
				<< traceScene.Bvh().TriangleCount() << " tris ("
				<< averaged << " averaged textures)\n" << std::flush;
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
				// axes (an OBB â€” six planes, CvBoxPlanesRotated).
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
				std::cout << "  [CV] volume '" << def.name << "': priority="
					<< def.priority << " spacing=" << cv.spacing
					<< " planes=" << def.planes.size() << "\n" << std::flush;
				// Engine defaults (1536 capture dirs, 3 bounces, auto
				// capture lattice, HW threads): GiBaker stays the tuning
				// tool for anything finer.
				CvBakeConfig cfg{};
				CvVolume vol;
				CvBakeStats stats;
				if (!BakeCvVolume(traceScene, def, cv.spacing, cfg, vol,
					stats, [](const char* line) { std::cout << line; })) {
					std::cerr << "[CV] bake failed for '" << def.name
						<< "'\n";
					return false;
				}
				std::vector<uint8_t> bytes;
				std::string werr;
				if (!vol.WriteToMemory(bytes, werr)) {
					std::cerr << "[CV] serialize failed for '" << def.name
						<< "': " << werr << "\n";
					return false;
				}
				const std::string vfsPath =
					"gi/" + sceneId + "_" + def.name + ".cvol";
				out.emplace_back(vfsPath, std::move(bytes));
				manifestRows.push_back(vfsPath + "  # " + def.name);
				std::cout << "  [CV] baked '" << def.name << "': dims="
					<< vol.dims[0] << "x" << vol.dims[1] << "x" << vol.dims[2]
					<< " captures=" << stats.captures
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
					std::cerr << "Missing value for " << a << std::endl;
					return false;
				}
				dst = argv[++i];
				return true;
			};
			if (a == "--content-dir" || a == "--in-vfs") {
				std::cerr << "'" << a << "' is gone: positional grammar now"
					" - pak.exe <vfs_path> <artifact...> [--options]"
					<< std::endl;
				return false;
			}
			else if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
				// The flag chain below parses this argv (the positionals
				// end at the first flag).
				sawFlag = true;
			}
			else if (!sawFlag) {
				positional.push_back(a);
				continue;
			}
			else {
				std::cerr << "Artifact '" << a << "' after flags: all"
					" artifacts come before the first --option"
					<< std::endl;
				return false;
			}
			if (a == "--out-pak") {
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
			else if (a == "--max-verts") {
				std::string v;
				if (!need(v)) return false;
				out.maxVerts = static_cast<uint32_t>(std::stoul(v));
				out.explicitSet.insert("maxVerts");
			}
			else if (a == "--max-tris") {
				std::string v;
				if (!need(v)) return false;
				out.maxTris = static_cast<uint32_t>(std::stoul(v));
				out.explicitSet.insert("maxTris");
			}
			else if (a == "--cone-weight") {
				std::string v;
				if (!need(v)) return false;
				out.coneWeight = std::stof(v);
				out.explicitSet.insert("coneWeight");
			}
	else if (a == "--collapse-static") {
		std::string v;
		if (!need(v)) return false;
		out.collapseStatic = (v == "on" || v == "1" || v == "true");
		out.explicitSet.insert("collapseStatic");
	}
			else if (a == "--auto-ktx") {
				std::string v;
				if (!need(v)) return false;
				out.autoKtx = (v == "on" || v == "1" || v == "true");
				out.explicitSet.insert("autoKtx");
			}
			else if (a == "--scratch-dir") {
				std::string v;
				if (!need(v)) return false;
				out.scratchDir = v;
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
			else if (a == "--backend") {
				std::string v;
				if (!need(v)) return false;
				if (v != "builtin" && v != "embree") {
					std::cerr << "--backend must be builtin|embree" << std::endl;
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
					std::cerr << "Bad --sun-color (want r,g,b): " << v << std::endl;
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
				std::string v;
				if (!need(v)) return false;
				out.giProbes.push_back(v);
			}
	else if (a == "--cvolume") {
		std::string v;
		if (!need(v)) return false;
		out.cvolume = (v == "on" || v == "1" || v == "true");
		out.explicitSet.insert("cvolume");
	}
	else if (a == "--quick-bakes") {
		std::string v;
		if (!need(v)) return false;
		out.quickBakes = (v == "on" || v == "1" || v == "true");
		out.explicitSet.insert("quickBakes");
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
	// Positionals: [0] = the VFS root, [1..] = the artifact list.
	if (!out.dumpPak.empty())
		return true; // inspection mode needs no cook inputs
	if (positional.empty()) {
		std::cerr << "Required: pak.exe <vfs_path> <artifact...> [--options]"
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
		std::cerr << "Required: at least one artifact .geometry (or --tour"
			" <scene.tour> - the editor-authored definition), and"
			" --out-pak <host path>" << std::endl;
		return false;
	}
	if (out.outPak.empty()) {
		std::cerr << "Required: --out-pak <host path>" << std::endl;
		return false;
	}
	// --tour is the DEFINITION (self-contained: the source glTF rides
	// as root reference #0) â€” no artifact inference needed.
	return true;
}

	void Usage() {
		std::cout << "pak.exe <vfs_path> <artifact...> [--options]\n"
			<< "          pak.exe F:/content models/sponza/Sponza.geometry\n"
			<< "              --out-pak F:/content/paks/sponza.pak [...]\n"
			<< "          (positional grammar: the VFS content root + the\n"
			<< "          ALREADY-PROCESSED .geometry artifact list â€” glTF is\n"
			<< "          IMPORT-ONLY (import.exe <input> / the editor's\n"
			<< "          MeshImporter derives the artifacts); the artifacts\n"
			<< "          carry the scenes; the pak cooks ALL of them.)\n"
			<< "          [--tour <scene.tour>] [--overdraw 1.05] [--verify on|off]"
			<< " [--dump-manifest on|off]\n"
			<< "          [--lods on|off] [--lod-ratios 0.6,0.3,0.12] [--lod-error 0.02]\n"
			<< "          [--meshlets on|off] [--max-verts 64] [--max-tris 64]\n"
			<< "          [--auto-ktx on|off]\n"
			<< "          [--scratch-dir <dir>]  (editor cache; acceleration only,\n"
			<< "                                  never required)\n"
			<< "          [--cone-weight 0] [--collapse-static on|off]\n"
			<< "          [--compress on|off] [--lz4-accel 1]\n"
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
			<< "           --lm-denoise on|off (DEPRECATED)"
			<< " --lm-denoise-quality high|balanced (DEPRECATED)\n"
			<< "           --lm-debug-pink on|off\n"
			<< "           --lm-stitch on|off\n"
			<< "           --lm-curve-target 7.0 --lm-curve-max 2.5\n"
			<< "           --lm-exclude tree,linden,ivy,glass --lm-exclude-small 0.5\n"
			<< "           --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]\n"
			<< "          (world sun: when the scene carries no directional light,\n"
			<< "           one is synthesized from the sun settings above, so the\n"
			<< "           editor world sun travels pak -> App)\n"
			<< "          [--gi-probe <name> ...]: registers probe sidecar names\n"
			<< "          (gi/<name>.sggi, baked separately) into the pak; the\n"
			<< "          lightmap atlas registers itself on --lightmap. Runtime\n"
			<< "          resolves GI by these names (one per kind for now).\n"
			<< "          [--cvolume on|off (default on)]: bakes the tour's\n"
			<< "          authored convex GI volumes (cvolume nodes) in-process\n"
			<< "          (same engine core as GiBaker) and embeds them into\n"
			<< "          the pak as Texture blobs under gi/ (volumes at\n"
			<< "          gi/<pak-stem>_<name>.cvol, the runtime manifest at\n"
			<< "          gi/<pak-stem>.cvmanifest) â€” the pak is self-contained\n"
			<< "          GI for the App; loose GiBaker sidecars still override\n"
			<< "          through the overlay when present.\n"
			<< "          [--cubemap on|off --cubemap-name <name>\n"
			<< "           --cubemap-size 128 --cubemap-rays 128 --cubemap-bounces 5\n"
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
			<< "          textures/cubemaps/<name>.cubemanifest (positions +\n"
			<< "          radii) next to the KTX blobs; embedded as Texture\n"
			<< "          blobs at textures/cubemaps/<name>[_N].ktx2 (name\n"
			<< "          defaults to the out-pak stem). Debug PNGs\n"
			<< "          (<name>_N.png faces, <name>_N_mips.png prefilter stack,\n"
			<< "          <name>_probes.png top-down placement map) land loose\n"
			<< "          in <contentDir>/textures/cubemaps/. Needs exactly\n"
			<< "          one cooked scene, like --lightmap.\n"
			<< "--lightmap on: bakes 2D SG lightmaps into the pak (per-vertex\n"
			<< "lightmap UVs + per-mesh chunk id; atlas written to\n"
			<< "<contentDir>/gi/<scene>.sglm). Sources: sun + static spots +\n"
			<< "real materials; transmissive (Blend) meshes pass light through.\n"
			<< "--lm-meters-per-texel is a WORLD-space request (m/texel; 0.25 =\n"
			<< "one texel per 25 cm = 4 texels/m; scaled nodes like sponza's\n"
			<< "0.008 root convert through their world scale. --lm-density\n"
			<< "<texels/m> stays as a deprecated converting alias). The atlas\n"
			<< "WIDTH is fixed at --lm-atlas-width; the HEIGHT grows with the\n"
			<< "content (rows appended at the bottom) up to --lm-max-height â€”\n"
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
			<< "the loose textures/cubemaps/<base>[_N].ktx2 + <base>.cubemanifest\n"
			<< "sidecars ARE the output (they override the last pak's blobs\n"
			<< "through the runtime VFS overlay). No pak is written; the\n"
			<< "lightmap/LOD/collapse/cvolume stages are forced off. The out-pak\n"
			<< "stem still names the capture set.\n"
			<< "--tour <scene.tour>: replay the editor's ASCII command overlay\n"
			<< "(renames, transforms, materials, no-lightmap flags, reparents)\n"
			<< "onto the built cluster before cooking. Tour cook settings\n"
			<< "fill flags the CLI did not set (CLI wins). Deterministic:\n"
			<< "the tour (or --in-vfs names the derived .geometry â€” glTF is\n"
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
					std::cout << "    [truncated mesh blob: " << e->path << "]\n";
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
			// samples another mesh's transform (or OOB garbage) â€” isolated
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
			//               referencing a pre-seam-dup vertex layout â€” the
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

		// Textures (embedded .ktx2 byte sizes).
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
				std::cout << "    [truncated camera blob: " << e->path << "]\n";
				continue;
			}
			std::cout << "    camera " << uri << " '" << name << "' "
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
				std::cout << "    [truncated scene blob]\n";
				continue;
			}
			std::cout << "    scene nodes=" << nodeCount
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
				if (hasCam)
					std::cout << " cam=" << cit->second;
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


int main(int argc, char** argv) {
	// Unbuffered pipe output: the editor tails this process through a
	// _popen pipe and streams each line live to its Console. Without
	// this, stdout stays block-buffered when piped and no progress
	// appears until the cook finishes (looks stuck).
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;
	Args args;
	if (!ParseArgs(argc, argv, args)) {
		Usage();
		return 2;
	}

	// Bakes moved to cook.exe: pak assembles COOKED artifacts only
	// (import.exe derives, cook.exe bakes + writes back, pak serializes).
	// Any explicitly-set bake flag is a hard error — silent ignore would
	// produce an unbaked pak that looks cooked. Prefix match covers the
	// detail flags (lm-*, cubemap-*) without enumerating every key.
	for (const std::string& k : args.explicitSet) {
		const bool bake =
			(k.rfind("lm", 0) == 0 || k.rfind("cubemap", 0) == 0
				|| k.rfind("lod", 0) == 0 || k.rfind("max", 0) == 0
				|| k == "meshlets" || k == "coneWeight"
				|| k == "cvolume" || k == "collapseStatic"
				|| k == "quickBakes");
		if (bake) {
			std::cerr << "--" << k << " is a cook.exe flag: pak.exe takes "
				"cooked artifacts only (run cook.exe first)" << std::endl;
			return 2;
		}
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
			<< " cameras=" << reader->BlobsOfKind(smvk::PakAssetKind::Camera).size()
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
	meshletCfg.maxVertices = std::clamp(args.maxVerts, 3u, smvk::kMeshletMaxVertices);
	meshletCfg.maxTriangles = std::clamp(args.maxTris, 1u, smvk::kMeshletMaxTriangles);
	meshletCfg.coneWeight = args.coneWeight;
	// Meshlet DECOUPLING (--lightmap on): the lightmap stage below
	// re-parameterizes meshes (chart-uv + chart-id stamping + seam vertex
	// duplication + index remap), so meshlets must NOT be built before
	// it â€” an import-time meshlet set references the PRE-stamp vertex
	// layout and draws the wrong chart uv per corner at runtime
	// (stale-window GI corruption). With lightmaps on the importer and
	// merged-block paths get a DISABLED config, and the cooker builds
	// meshlets ONCE after the bake from the FINAL stamped topology
	// (see the "Meshlet restore" stage below the lightmap block). The
	// rule that keeps GI whole: meshlet generation may partition
	// triangles, but it must never take part in â€” or see â€” the
	// parameterization. With --lightmap off nothing mutates uploads
	// after import, so import-time meshlets stay valid and the pipeline
	// is unchanged.
	smvk::MeshletConfig importMeshletCfg = meshletCfg;

	// Scratch cache (--scratch-dir, optional acceleration, NEVER a
	// precondition): open the editor's artifact registry so texture
	// transcodes serve hash-validated payloads from the scratch on hit
	// and stage fresh ones on miss. A missing/locked/broken scratch
	// degrades to plain processing â€” the pak output is byte-identical.
	std::unique_ptr<smvk::cache::CacheDb> scratchDb;
	std::unique_ptr<smvk::cache::ArtifactStore> scratchStore;
	std::unique_ptr<StoreArtifactCacheAdapter> scratchAdapter;
	if (!args.scratchDir.empty()) {
		const std::string root =
			smvk::cache::EnsureScratchRoot(args.scratchDir);
		if (!root.empty()) {
			scratchDb = std::make_unique<smvk::cache::CacheDb>();
			std::string dbErr;
			if (scratchDb->Open((std::filesystem::path(root) / "cache.db")
				.generic_string(), dbErr)) {
				scratchStore =
					std::make_unique<smvk::cache::ArtifactStore>(
						root, *scratchDb);
				scratchAdapter =
					std::make_unique<StoreArtifactCacheAdapter>(
						*scratchStore);
				std::cout << "Scratch cache: " << root << "\n";
			}
			else {
				std::cerr << "Scratch cache db open failed (" << dbErr
					<< ") â€” cooking without acceleration\n";
				scratchDb.reset();
			}
		}
		else {
			std::cerr << "Scratch root unusable: " << args.scratchDir
				<< " â€” cooking without acceleration\n";
		}
	}
	GltfArtifactCache* artifactCache =
		scratchAdapter ? scratchAdapter.get() : nullptr;

	// Tour path: the doc IS the definition (root reference #0 = the
	// source glTF, inlined under the scene root exactly like the
	// editor binds it). No source resolution, no side imports â€” the
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
			std::cerr << "Tour load failed: " << terr << std::endl;
			return 1;
		}
		haveTour = true;
		for (auto& w : twarn)
			std::cerr << "Tour warning: " << w << std::endl;
		// The tour's root reference doubles as the artifact identity when
		// the positional list is empty (the "Cooked <identity> -> ..." log
		// + SidecarStems: bake-asset logicals key on the SCENE stem, so
		// scene-keyed sidecar bakes survive differently-named out paks).
		if (args.inVfs.empty() && !tour.references.empty())
			args.inVfs = tour.references[0].source;
		// Apply tour cook defaults (CLI wins)
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
			use("lmProxyRes", args.lmProxyRes, k.lmProxyRes);
			use("lmExcludeSmall", args.lmExcludeSmall, k.lmExcludeSmall);
			// NOTE: no sun/sky keys here â€” TourCook dropped them; the env
			// blocks (env.default.* / env.n.*, applied by ApplyTourToCluster
			// into the cluster scene) are the sun/sky of record for every
			// stage (see EnvForBake). Legacy tour cook lines carrying the
			// old keys ignore them (the tour-side parse skips them).
			use("cubemap", args.cubemap, k.cubemap);
			use("cubemapName", args.cubemapName, k.cubemapName);
			use("cubemapSize", args.cubemapSize, k.cubemapSize);
			use("cubemapRays", args.cubemapRays, k.cubemapRays);
			// LOD detail + meshlet settings ride the tour cook block
			// the same way (CLI flags win when explicitly given).
			use("lodRatios", args.lodRatios, k.lodRatios);
			use("lodError", args.lodError, k.lodError);
			use("meshlets", args.meshlets, k.meshlets);
			use("maxVerts", args.maxVerts, k.meshletMaxVerts);
			use("maxTris", args.maxTris, k.meshletMaxTris);
			use("coneWeight", args.coneWeight, k.meshletConeWeight);
			use("autoKtx", args.autoKtx, k.autoKtx);
			if (!k.lmExcludeKw.empty() &&
				!args.explicitSet.contains("lmExclude")) {
				if (!args.lmExclude.empty())
					args.lmExclude += ",";
				args.lmExclude += k.lmExcludeKw;
				std::cout << "Tour cook default: lmExclude\n";
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
		meshletCfg.maxVertices = std::clamp(args.maxVerts, 3u, 255u);
		meshletCfg.maxTriangles = std::clamp(args.maxTris, 1u, 512u);
		meshletCfg.coneWeight = args.coneWeight;
		importMeshletCfg = meshletCfg;
		// DEFINITION BUILD â€” identical to the editor: empty scene root,
		// then the reference bind (root reference #0 imports the source
		// glTF under the scene root; the rest inline beneath it), then
		// the command replay. HOST-path resolution (vfs = nullptr) so
		// absolute and CWD-relative sources resolve exactly like the
		// editor session that authored the tour (the cooker inherits
		// the editor's working directory).
		{
			auto cluster = std::make_unique<smvk::Cluster>(nullptr);
			cluster->SetCookMode(true);
			addNode(cluster->scene, -1, 0); // empty scene root
			if (!tour.references.empty()) {
				const std::string ownerBase =
					smvk::vfs::BaseDir(tour.references[0].source);
				const size_t applied = ApplySceneReferences(*cluster, tour,
					ownerBase, nullptr, nullptr,
					nullptr /* HOST bind â€” editor parity */,
					opt, importMeshletCfg, 0, nullptr, nullptr, nullptr,
					args.autoKtx, tour.fileNodes.empty() ? nullptr : &tour.fileNodes,
					artifactCache);
				std::cout << "Definition: bound " << applied
					<< " reference subtree(s) ("
					<< tour.references.size() << " top-level)\n";
			}
			// Texture-asset POOL imports (ordered; see the tour's
			// textureAsset records): replayed BEFORE the material records
			// (a material tex ref or a light cookie naming a .texture must
			// find its pool entry â€” the shared ImportTextureAssetToCluster,
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
					std::cerr << "Tour textureAsset not found: '"
						<< ta.path << "' (under --content-dir '"
						<< args.contentDir << "', then as-is)\n";
					return 1;
				}
				std::string imErr;
				if (smvk::assets::ImportTextureAssetToCluster(
					*cluster, hp, nullptr, imErr).empty()) {
					std::cerr << "Tour textureAsset import failed: '"
						<< ta.path << "': " << imErr << "\n";
					return 1;
				}
			}
			if (!tour.textureAssets.empty())
				std::cout << "Definition: imported "
				<< tour.textureAssets.size()
				<< " texture asset(s)\n";
			// Material-asset POOL imports (ordered; see the tour's
			// materialAsset records): the editor session minted these into
			// the cluster pool beyond the reference-bound materials, and the
			// tour's assignMaterial/-m indices were recorded against that
			// pool â€” replay the records in order (the record order IS the
			// pool order) so the indices resolve exactly as authored. The
			// shared import is the same path the editor ran
			// (ImportMaterialAssetToCluster, cook mode: no GPU). A failed
			// import shifts every later index â€” FAIL the cook loudly
			// instead of cooking a wrong-material pak.
			for (const auto& ma : tour.materialAssets) {
				if (ma.editorDefault) {
					smvk::assets::EnsureClusterDefaultMaterial(*cluster);
					continue;
				}
				// Resolve: project-relative records against --content-dir
				// FIRST (the record's in-project convention â€” as-is/CWD would
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
					std::cerr << "Tour materialAsset not found: '"
						<< ma.path << "' (under --content-dir '"
						<< args.contentDir << "', then as-is)\n";
					return 1;
				}
				std::string imErr;
				if (smvk::assets::ImportMaterialAssetToCluster(
					*cluster, hp, nullptr, imErr).empty()) {
					std::cerr << "Tour materialAsset import failed: '"
						<< ma.path << "': " << imErr << "\n";
					return 1;
				}
			}
			if (!tour.materialAssets.empty())
				std::cout << "Definition: imported "
				<< tour.materialAssets.size()
				<< " material asset(s)\n";
			std::vector<std::string> rwarn;
			std::string rerr;
			if (!smvk::tour::ApplyTourToCluster(*cluster, tour, &rwarn,
				&rerr)) {
				std::cerr << "Tour replay failed: " << rerr << std::endl;
				return 1;
			}
			for (auto& w : rwarn)
				std::cerr << "Tour replay warning: " << w << std::endl;
			std::cout << "Tour replayed: " << tour.commands.size()
				<< " commands from " << args.tour << "\n";
			sceneNames.push_back("scene");
			sceneClusters.push_back(std::move(cluster));
		}
	}
	else {
		// No tour: ARTIFACT cook â€” the POSITIONAL artifact list (the
		// ALREADY-PROCESSED .geometry blobs; the MeshImporter/import.exe
		// output). glTF is IMPORT-ONLY and never cooks (the old
		// GltfToClusterFromVfs in-cook glTF import is GONE): open/drop
		// the .gltf in the editor (derive-if-stale) or run import.exe,
		// then cook the .geometry. Each artifact carries the ONE derived
		// scene (the source import's default scene) â€” no multi-scene
		// fan-out, no --scene-index; the pak cooks ALL listed artifacts.
		if (args.sceneIndex >= 0) {
			std::cerr << "--scene-index is a glTF-era flag: the .geometry "
				"artifact carries the one derived scene (dropped)"
				<< std::endl;
		}
		// Resolve + read each .geometry through the VFS (the runtime
		// content abstraction â€” loose files today; pak-embedded /
		// patched artifacts land through the same reads later).
		for (const std::string& inVfs : args.inVfsList) {
			std::string artPath = smvk::NormalizeAssetUri(
				inVfs).Str();
			if (artPath.empty())
				artPath = inVfs; // host-style path (as-is)
			auto artBytes = vfs.TryRead(artPath);
			if (!artBytes || artBytes->empty()) {
				std::cerr << "Artifact read failed: '" << inVfs
					<< "' not in VFS (want the derived .geometry â€” import "
					"the source glTF first: import.exe / the editor's "
					"MeshImporter)" << std::endl;
				return 1;
			}
			smvk::assets::MeshAsset meshAsset;
			std::string aerr;
			if (!smvk::assets::LoadMeshAssetBytes(artBytes->data(),
				artBytes->size(), meshAsset, aerr)) {
				std::cerr << "Artifact parse failed: '" << inVfs
					<< "': " << aerr << " (re-import the source glTF â€” "
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
			// NOTE: heap-allocated â€” Cluster embeds multi-MB pool arrays
			// and must never live on the (1MB) thread stack. Cook mode =
			// GPU-free: CPU geometry + retained uploads only, textures
			// are minted cook ids.
			auto cluster = std::make_unique<smvk::Cluster>(nullptr);
			cluster->SetCookMode(true);
			addNode(cluster->scene, -1, 0); // empty scene root (editor parity)
			const std::string baseDir = smvk::vfs::BaseDir(artPath);
			auto texSrc = std::make_shared<VfsTextureSource>(&vfs, baseDir);
			const size_t built = BuildFromMeshAsset(*cluster, nullptr,
				meshAsset, meshManifest, artPath, 0, texSrc, args.autoKtx,
				artifactCache, readSibling);
			if (built == 0) {
				std::cerr << "Artifact build failed: '" << inVfs
					<< "' built 0 nodes (degenerate/empty â€” re-import "
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
				std::cout << "Artifact built: " << built << " nodes, "
					<< cluster->lights.numObjects() << " lights"
					<< " (dir=" << nDir << " point=" << nPoint
					<< " spot=" << nSpot << ", links="
					<< cluster->scene.lightForNode.size() << ")\n";
			}
			std::cout << "Cooking artifact: '" << inVfs << "'\n";
			sceneNames.push_back("");
			sceneClusters.push_back(std::move(cluster));
		}
	} // close else block for the artifact path
	// Import phase finished: commit any staged scratch artifacts (one
	// pak grow per touched class). Flush failure loses acceleration
	// only â€” staged bytes were already used in the import.
	if (scratchStore && scratchStore->StagedCount() > 0) {
		const size_t staged = scratchStore->StagedCount();
		std::string flushErr;
		if (scratchStore->Flush(flushErr))
			std::cout << "Scratch cache: " << staged
			<< " texture artifact(s) flushed\n";
		else
			std::cerr << "Scratch flush failed: " << flushErr
			<< " (staged stands; the pak output is unaffected)\n";
	}
	// Single-scene alias for the tour/collapse/bake stages below (written
	// against one cluster; multi-scene loops reuse them per scene). Both
	// cluster-> and *cluster forms keep working through the pointer.
	smvk::Cluster* cluster = sceneClusters.front().get();

	// World sun per scene (App parity: first directional wins as the
	// runtime sun). Authored directionals always survive; sunless scenes
	// synthesize one from the ENV BLOCK (EnvForBake â€” one authored sun).
	for (auto& sc : sceneClusters)
		EnsureWorldSun(*sc, EnvForBake(*sc, args));

	// Sun of record -> the scene's look env (sun fields only): an
	// AUTHORED directional (imported KHR punctual or tour-created) is
	// scene CONTENT â€” it wins over the env block's sun fields so exactly
	// ONE sun reaches the App (the env keeps its sky/fog/exposure). The
	// env block is lux-native, so the light's lux copies straight in (no
	// conversion). A synthesized WorldSun already carries the env's own
	// values â€” the seed is a no-op there (float-exact round trip).
	for (auto& sc : sceneClusters) {
		if (!sc->scene.hasEnvironment)
			continue; // runtime seeds from the light at load
		const ::Scene& scene = sc->scene;
		glm::vec3 toSun(0.0f);
		glm::vec3 sunColor(0.0f);
		float sunLux = 0.0f;
		bool hasSun = false;
		// Deterministic node order (hash-map iteration is not); the
		// aim axis after the coordFix basis change (the same plain
		// read as BuildCookedLightsFromWorld / CollectStaticBakeLights).
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
		std::cout << "Env sun resolved from the sun light: az="
			<< e.sunAzimuth << " el=" << e.sunElevation
			<< " I=" << e.sunIntensity << " (lux)\n";
	}

	// GI blobs embedded into the pak (see the cvolume stage + the loose-
	// sidecar discovery below): one list â€” in-process-baked cvolumes (the
	// no-sidecar fallback), the discovered GiBaker sidecars (probe octree,
	// regular/convex manifests + rows) and the lightmap .sglm. Every
	// entry is (vfsPath, bytes) â€” Texture blobs at gi/... paths embedded
	// after Serialize below (PakBackend serves by path).
	std::vector<std::pair<std::string, std::vector<uint8_t>>> giBlobs;


	// ---------------------------------------------------------------------------
	// Preconvolved specular cubemaps: the COOK baked + wrote them loose
	// (textures/cubemaps/<base>[_N].ktx2 + the .cubemanifest); the loose
	// discovery below embeds them as Texture blobs after SerializeClusters
	// (PakBackend serves by path). Pak never captures â€” bake flags are
	// rejected above (run cook.exe).
	// ---------------------------------------------------------------------------
	std::vector<std::pair<std::string, std::vector<uint8_t>>> cubemapKtx;
	std::string cubemapManifestBase;



	// Probe names (--giProbes), registered on every cooked cluster (the
	// serializer unions them into one pak-level Gi registry). Names are
	// the STABLE bake stems now (GiBaker writes gi/<stem>.sggi directly):
	// the runtime resolves gi/<name>.sggi by path, pak or loose.
	for (const std::string& probe : args.giProbes) {
		for (auto& sc : sceneClusters)
			sc->RegisterGiBinding(smvk::GiBinding::Kind::Probe, probe);
		std::cout << "Registered probe GI '" << probe << "'\n";
	}
	// Cubemap manifest (the stage above minted it when the set baked
	// > 1 capture): registered by its stable base name â€” the runtime
	// loads textures/cubemaps/<base>.cubemanifest by path.
	if (!cubemapManifestBase.empty()) {
		for (auto& sc : sceneClusters)
			sc->RegisterGiBinding(smvk::GiBinding::Kind::Cubemap,
				cubemapManifestBase);
		std::cout << "Registered cubemap manifest GI '"
			<< cubemapManifestBase << "'\n";
	}
	// LOOSE cubemap bakes (an earlier --cubemap run / the editor's
	// quick-bake left them at their stable paths): when the bake stage
	// did not run, discover textures/cubemaps/<stem>.cubemanifest by the
	// same stable stems and EMBED it + every row's KTX â€” no re-bake, the
	// payloads ARE the bake output. Registered by its base like above;
	// the embed loop below carries the cubemapKtx rows into the pak.
	if (cubemapManifestBase.empty()) {
		for (const std::string& stem : SidecarStems(args)) {
			const std::string mrel =
				"textures/cubemaps/" + stem + ".cubemanifest";
			std::vector<uint8_t> manifest =
				ReadSidecarFile(args.contentDir, mrel);
			if (manifest.empty())
				continue;
			std::string mErr;
			std::vector<smvk::CubemapManifestEntry> rows;
			const std::string_view text(
				reinterpret_cast<const char*>(manifest.data()),
				manifest.size());
			if (!smvk::CubemapManifestParse(text, rows, mErr)) {
				std::cerr << "loose cubemap manifest '" << mrel
					<< "' rejected: " << mErr << " â€” not embedded\n";
				continue;
			}
			cubemapKtx.emplace_back(mrel, std::move(manifest));
			for (const auto& row : rows) {
				std::vector<uint8_t> ktx =
					ReadSidecarFile(args.contentDir, row.vfsPath);
				if (ktx.empty()) {
					std::cerr << "loose cubemap row '" << row.vfsPath
						<< "' unreadable â€” not embedded\n";
					continue;
				}
				cubemapKtx.emplace_back(row.vfsPath, std::move(ktx));
			}
			for (auto& sc : sceneClusters)
				sc->RegisterGiBinding(smvk::GiBinding::Kind::Cubemap,
					stem);
			std::cout << "Registered + embedded loose cubemap manifest '"
				<< stem << "' (" << rows.size() << " captures)\n";
			break;
		}
	}
	// Loose GI bakes (GiBaker writes these â€” the editor cook flow runs
	// GiBaker from 0 BEFORE PakCooker): discover by STABLE PATH in the
	// content's gi/ dir (the stems: pak stem first, then the source scene
	// stem), REGISTER the stem bindings AND EMBED the payloads into the
	// pak (Texture blobs at their gi/ paths â€” PakBackend serves by path,
	// so the runtime's binding-first loaders resolve straight out of the
	// pak; loose files still override through the overlay by the same
	// paths). Missing = unbaked (the runtime falls back to the scene-id
	// stem, then to "not baked yet").
	{
		std::filesystem::path pakPath(args.outPak);
		const std::string pakStem = pakPath.stem().generic_string();
		const std::vector<std::string> stems = SidecarStems(args);
		// First stem that has a readable file (the probe octree is a
		// single payload; the manifests carry row payloads).
		auto findStemWith = [&](const char* suffix) {
			for (const std::string& stem : stems) {
				const std::string vfs =
					"gi/" + stem + "." + suffix;
				if (!ReadSidecarFile(args.contentDir, vfs).empty())
					return stem;
			}
			return std::string();
		};
		// Probe octree (the world field, GiBaker --out): a single
		// payload, no manifest rows â€” embed gi/<stem>.sggi.
		{
			const std::string stem = findStemWith("sggi");
			if (!stem.empty()) {
				const std::string vfsPath = "gi/" + stem + ".sggi";
				std::vector<uint8_t> bytes =
					ReadSidecarFile(args.contentDir, vfsPath);
				if (bytes.empty()) {
					std::cerr << "probe bake '" << vfsPath
						<< "' unreadable â€” not embedded\n";
				}
				else {
					giBlobs.emplace_back(vfsPath, std::move(bytes));
					for (auto& sc : sceneClusters)
						sc->RegisterGiBinding(
							smvk::GiBinding::Kind::Probe, stem);
					std::cout << "Registered + embedded probe GI '"
						<< stem << "' (scene '" << pakStem << "')\n";
				}
			}
		}
		// Volume manifests (regular + convex): embed the manifest + every
		// row's payload file, register the stem binding. Rows are bare
		// VFS paths (legacy "<guid> <path>" rows still parse â€”
		// SplitManifestRow takes the path column either way).
		struct ManifestKind {
			smvk::GiBinding::Kind kind;
			const char* ext; // the manifest file extension
		};
		const ManifestKind manifestKinds[2] = {
			{ smvk::GiBinding::Kind::RegManifest, "regmanifest" },
			{ smvk::GiBinding::Kind::CvolManifest, "cvmanifest" },
		};
		for (const auto& [kind, ext] : manifestKinds) {
			const std::string stem = findStemWith(ext);
			if (stem.empty()) {
				// No loose manifest: cvolumes cooked INTO this pak (the
				// --cvolume stage above) bind by the pak stem â€” the
				// runtime resolves gi/<pakStem>.cvmanifest straight
				// out of the pak (PakBackend serves the gi/... Texture
				// blobs by path). Loose stem-named files override by
				// the same path through the overlay (dev override).
				if (kind == smvk::GiBinding::Kind::CvolManifest
					&& !giBlobs.empty()) {
					for (auto& sc : sceneClusters)
						sc->RegisterGiBinding(kind, pakStem);
					std::cout << "Registered embedded cvolmanifest GI '"
						<< pakStem << "' (cooked into this pak)\n";
				}
				continue;
			}
			const std::string manifestVfs =
				"gi/" + stem + "." + ext;
			std::vector<uint8_t> manifest =
				ReadSidecarFile(args.contentDir, manifestVfs);
			if (manifest.empty()) {
				std::cerr << "volume manifest '" << manifestVfs
					<< "' unreadable â€” binding kept, not embedded\n";
			}
			else {
				giBlobs.emplace_back(manifestVfs, std::move(manifest));
				// Row payloads (SplitManifestRow takes the path column).
				std::vector<std::string> rows;
				std::string cur;
				for (char c : giBlobs.back().second) {
					if (c == '\n' || c == '\r') {
						const std::string p =
							smvk::bake::SplitManifestRow(cur);
						if (!p.empty())
							rows.push_back(p);
						cur.clear();
					}
					else
						cur.push_back(c);
				}
				const std::string p =
					smvk::bake::SplitManifestRow(cur);
				if (!p.empty())
					rows.push_back(p);
				for (const std::string& row : rows) {
					std::vector<uint8_t> bytes =
						ReadSidecarFile(args.contentDir, row);
					if (bytes.empty()) {
						std::cerr << "sidecar volume '" << row
							<< "' unreadable â€” not embedded\n";
						continue;
					}
					giBlobs.emplace_back(row, std::move(bytes));
				}
			}
			for (auto& sc : sceneClusters)
				sc->RegisterGiBinding(kind, stem);
			std::cout << "Registered " << ext << " GI '" << stem
				<< "' (scene '" << pakStem << "')\n";
		}
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
	// text inside a Texture payload â€” served by path, decoded by the
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
	// GI payloads embedded into the pak (the in-process cvolume bake
	// above + the loose GiBaker sidecars + the lightmap .sglm): Texture
	// blobs at their gi/... paths â€” PakBackend serves by path, so the
	// runtime's binding-first loaders read everything straight out of
	// this pak (no loose sidecars needed; loose files still override
	// through the overlay by the same paths). Same path-embed pattern
	// as the .cubemanifest text above.
	for (auto& [vfsPath, bytes] : giBlobs) {
		std::vector<uint8_t> payload = smvk::BuildTexturePayload(
			vfsPath, -1, -1, -1, -1, std::span<const uint8_t>(bytes));
		if (!writer.AddBlob(smvk::PakAssetKind::Texture, vfsPath,
			std::move(payload))) {
			std::cerr << "GI AddBlob failed: " << vfsPath << std::endl;
			return 1;
		}
		std::cout << "Embedded GI " << vfsPath
			<< " (" << bytes.size() << " B)\n";
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

	std::cout << "Cooked " << args.inVfs
		<< (args.inVfsList.size() > 1
			? " +" + std::to_string(args.inVfsList.size() - 1)
				+ " more artifact(s)"
			: (!args.tour.empty() ? " (tour definition)" : ""))
		<< " -> " << args.outPak << "\n"
		<< "  scenes=" << sceneNames.size()
		<< " meshes=" << stats.meshes << " materials=" << stats.materials
		<< " textures=" << stats.textures << " (" << stats.textureBytes << " B)"
		<< " digested=" << stats.digestedTextures
		<< " nodes=" << stats.nodes << " lights=" << stats.lights
		<< " cameras=" << stats.cameras
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
		size_t digested = 0;
		for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Texture)) {
			smvk::PakTexturePayload p;
			// v4 digested shape first (validates digest + tail section),
			// legacy/digest-less fallback second â€” mirrors the runtime
			// split in DeserializeCluster.
			if (smvk::ParseTexturePayloadV4(reader.Payload(*e), p)) {
				++digested;
				// Digest sanity: kind + tail split must parse.
				smvk::TextureStreamMeta meta{};
				if (!smvk::ParseTextureDigest(p.digest, meta))
					++badPayloads;
			}
			else if (!smvk::ParseTexturePayload(reader.Payload(*e), p))
				++badPayloads;
		}
		std::cout << "  verify: entries=" << reader.EntryCount()
			<< " meshes=" << reader.BlobsOfKind(smvk::PakAssetKind::Mesh).size()
			<< " materials=" << reader.BlobsOfKind(smvk::PakAssetKind::Material).size()
			<< " textures=" << reader.BlobsOfKind(smvk::PakAssetKind::Texture).size()
			<< " digested=" << digested
			<< " cameras=" << reader.BlobsOfKind(smvk::PakAssetKind::Camera).size()
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
