// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// pak.exe: offline ARTIFACT -> .pak cooker that works exclusively on the
// VFS and needs NO GPU (offline-mode Cluster, see Cluster::SetOfflineMode).
// The COOK-SPLIT (the clean separation): the cook inputs are the
// ALREADY-PROCESSED .geometry artifacts (import.exe / the MeshImporter /
// the editor's open-drop produce them - glTF is IMPORT-ONLY, never a
// cook input; see gltf_import/asset_export.h; the raw import lives in
// tools/import, ONE place).
//
//   pak.exe <vfs_path> <artifact...> [--options]
// pak.exe — the PACK step (pure assemble: serialize + embed + verify).
// Bakes live in cook.exe (artifact transforms, lightmaps, cubemaps,
// convex volumes) and GiBaker.exe (world octree, tuning bakes); pak
// runs NONE of them. Positional grammar:
//
//   pak.exe <vfs_path> <artifact...> --out-pak <host path>
//       [--tour <scene.tour>] [--verify on|off] [--dump-pak <pak path>]
//       [--dump-manifest on|off] [--dump-node <id> ...]
//       [--meshopt on|off] [--overdraw 1.05] [--auto-ktx on|off]
//       [--compress on|off] [--lz4-accel 1]
//       [--sun-azimuth 30 --sun-elevation 55 --sun-intensity 10000
//        --sky-intensity 600 --sun-color 1,0.95,0.9]
//       [--gi-probe <name> ...] [--terrain <dir>] [--level <name>]
//
// Inputs: ALREADY-PROCESSED .geometry artifacts (import.exe derives
// them; cook.exe bakes + stamps <stem>.cooked — pak refuses
// uncooked/stale inputs) + the LOOSE bake files the cookers wrote
// (gi/*.sggi + *.givolume, gi/cubemaps/<base>[_N].ktx2 + .cubemanifest),
// discovered by stable stem and embedded as Texture blobs. The tour,
// when given, replays read-only for serialization (no synthesis —
// WorldSun/env seeding moved to the cook); --level packs the scene +
// baked data into one levels/<name>.level blob.
//
// For bake-transport details see tools/cook (cubemap/lightmap/gi-volume
// stages) and tools/gi_baker (world octree).
//
//
// In artifact mode, reads a derived .geometry file and its dependencies
// through an OverlayVfs with a LooseBackend over --content-dir (the same
// lookup rules as the runtime). In tour mode, replays an editor scene
// definition. Both paths cook geometry and write a pak with embedded
// textures and metadata. There is intentionally no external pak manifest:
// --dump-manifest reads the data back through PakReader and prints it to
// stdout, so the report cannot drift from the written bytes.

#include <algorithm>
#include <set>
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
#include <Engine/Asset/level.h>
#include "terrain_cook.h"
#include "pak_tool.h"
#include <tool_log.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/bake_inputs.h> // the invalidation model (the pak gate)
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

namespace paktool {

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
		bool dumpManifest = false; // print pak contents to stdout (pak-sourced)
		bool verify = true; // re-open the written pak with PakReader (self-check)
		// SKIP the loose VFS bake-file discovery (-1 = the tour's
		// exportSettings node decides; 0/1 = CLI-pinned). On = the pak
		// embeds ONLY the scene's own content (meshes/materials/textures,
		// blobs by path) — no gi/ cubemap manifests + KTX rows, no .sggi,
		// no .givolumemanifest + .givolume rows, no bindings (see SceneExportSettings).
		int ignoreVfs = -1;
		// KTX auto-convert (on by default): non-KTX sources transcode
		// in-process (stb decode + libktx write, `.ktx2` sidecar cached next
		// to the source). Off restores warn-and-skip for non-KTX payloads.
		bool autoKtx = true;
		// Per-blob LZ4 compression (default on: only stores when it shrinks).
		bool compress = true;
		int lz4Accel = 1; // 1 = best ratio, higher = faster
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
		// Probe-volume bake files this pak's scenes sample (repeatable):
		// registers gi/<name>.sggi names into the clusters (bake the matching
		// volumes with GiBaker --out). Runtime resolves by name.
		std::vector<std::string> giProbes;
		// Read-only inspection: open an existing pak and print its manifest
		// (meshes + lmChunk, materials + texture URIs, textures). No cooking.
		std::string dumpPak;
		// Scene pinpoint: with --dump-pak, restrict the scene print to these
		// node ids (repeatable) and include their local translation.
		std::vector<uint32_t> dumpNodes;
		// Tiled terrain source dir (host path with terrain.json +
		// h_XX_ZZ.png + m_XX_ZZ.png; see Rendergraph/terrain_meta.h).
		// Terrain blobs ride the same pak (or a terrain-only pak with no
		// artifacts/tour); empty = no terrain.
		std::string terrainDir;
		// LEVEL blob (the exploration spike; see Engine/Asset/level.h):
		// when set, ONE composite levels/<name>.level blob packs the
		// scene payload + the baked data blobs this pak carries (the
		// cubemap set manifest + KTX2 rows, the GI payloads). The LEVEL
		// — not the scene stem — owns the data. Empty = no level blob.
		std::string levelName;
		// Editor registry root (optional acceleration, NEVER a
		// precondition): when set, the pack consults + feeds the artifact
		// registry there — texture transcodes copy hash-validated payloads
		// from the registry on hit and stage fresh ones on miss.
		// Absent/broken registry = plain processing, unchanged output.
		std::string registryDir;
	};

	// Bake/pak sun+sky source of record (see SceneEnv): the scene's LUX-NATIVE
	// env block when the tour carried one (the editor always writes env.default.*),
	// else the CLI sun defaults (bare cooks). Every stage — the synthesized
	// WorldSun, the lightmap / cubemap / GI-volume bakes — reads this, so ONE
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
	// bake-time sun rides the in-pak bakes (convex volumes); the runtime
	// still renders it as the analytic/CSM sun (static directionals are
	// exempt from the bake-only rule), so nothing double-counts.
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

	// Produce a filename-safe token for embedded GI-volume paths (manifest rows
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
		return out.empty() ? std::string("givolume") : out;
	}

	// Reads a loose bake file by its content-root-relative vfs path
	// ("gi/<scene>.givolumemanifest" -> <contentDir>/gi/<scene>.givolumemanifest). Empty
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

	// Bake-file name stems (the REGULAR-volume manifest discovery only
	// — the convex/probe families ride the scene's HARD giAsset
	// bindings now): the pak stem first (the legacy regular-manifest
	// discovery order), then the source scene stems (the artifact list
	// - every listed .geometry, or the tour reference identity). Bakes
	// key on the SCENE they ran over (GiBaker --scene, the .regmanifest
	// stem) — without the scene stems a differently-named out pak would
	// silently miss its own bakes.
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
			if (a == "--out-pak") {
				if (!need(out.outPak)) return false;
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
			else if (a == "--auto-ktx") {
				std::string v;
				if (!need(v)) return false;
				out.autoKtx = (v == "on" || v == "1" || v == "true");
				out.explicitSet.insert("autoKtx");
			}
			else if (a == "--ignore-vfs") {
				std::string v;
				if (!need(v)) return false;
				// CLI pin (wins over the exportSettings node's ignoreVfs):
				// skip the loose VFS bake-file discovery — everything else
				// embeds straight from the scene.
				out.ignoreVfs = (v == "on" || v == "1" || v == "true") ? 1 : 0;
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
			else if (a == "--terrain") {
				if (!need(out.terrainDir)) return false;
			}
			else if (a == "--level") {
				if (!need(out.levelName)) return false;
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
		tool::err() << "Required: pak.exe <vfs_path> <artifact...> [--options]"
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
	if (out.inVfsList.empty() && out.tour.empty() && out.terrainDir.empty()) {
		tool::err() << "Required: at least one artifact .geometry (or --tour"
		" <scene.tour> - the editor-authored definition), and"
		" --out-pak <host path> (--terrain <dir> alone cooks a"
		" terrain-only pak)" << std::endl;
		return false;
	}
	if (out.outPak.empty()) {
		tool::err() << "Required: --out-pak <host path>" << std::endl;
		return false;
	}
	// --tour is the DEFINITION (self-contained: the source glTF rides
	// as root reference #0) — no artifact inference needed.
	return true;
}

	void Usage() {
		tool::out() << "pak.exe <vfs_path> <artifact...> [--options]\n"
			<< "          pak.exe F:/content models/sponza/Sponza.geometry\n"
			<< "              --out-pak F:/content/paks/sponza.pak [...]\n"
			<< "          (ASSEMBLY ONLY: cooked artifacts + loose bake\n"
			<< "          files in, .pak out. import.exe derives the\n"
			<< "          artifacts, cook.exe runs the bakes (GI/cubemaps/\n"
			<< "          LODs/meshlets) + writes them back with <stem>.cooked.\n"
			<< "          Pak refuses uncooked/stale artifacts. No bakes run\n"
			<< "          here, so there are no bake flags.)\n"
			<< "          [--tour <scene.tour>] [--out-pak <host path>]\n"
			<< "          [--verify on|off] [--dump-pak <pak path>]\n"
			<< "          [--dump-manifest on|off] [--dump-node <id> ...]\n"
			<< "          [--meshopt on|off] [--overdraw 1.05]\n"
			<< "          [--ignore-vfs on|off] (skip the loose gi/ bake-file\n"
			<< "           discovery - the scene's own content only; the\n"
			<< "           tour's exportSettings node defaults it)\n"
			<< "          [--auto-ktx on|off]\n"
			<< "          [--sun-azimuth 30 --sun-elevation 55\n"
			<< "           --sun-intensity 10000 --sky-intensity 6000\n"
			<< "           --sun-color 1,0.95,0.9]\n"
			<< "          [--gi-probe <name> ...] [--compress on|off]\n"
			<< "          [--lz4-accel 1]\n"
			<< "          [--terrain <terrain_src dir>]\n";
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

		// Textures (embedded .ktx2 byte sizes; v4 digested payloads parse
		// through the v4 reader first — the legacy v3 parse alone reads
		// every digested blob as malformed).
		for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Texture)) {
			smvk::PakTexturePayload p;
			if (smvk::ParseTexturePayloadV4(reader.Payload(*e), p)
				|| smvk::ParseTexturePayload(reader.Payload(*e), p)) {
				tool::out() << "    tex  " << p.uri << " ("
					<< p.ktxBytes.size() << " B"
					<< (p.digest.empty() ? "" : " digested") << ")\n";
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
		// Local transforms (pinpoint checks print them; the full mat4
		// kept so the pinpoint can dump rotation/scale columns too).
		std::vector<float> localT(nodeCount * 3, 0.0f);
		std::vector<float> localM(nodeCount * 16, 0.0f);
		for (uint32_t i = 0; i < nodeCount; ++i) {
			float m[16];
			for (int k = 0; k < 16; ++k)
				m[k] = c.f32();
			localT[i * 3 + 0] = m[12];
			localT[i * 3 + 1] = m[13];
			localT[i * 3 + 2] = m[14];
			for (int k = 0; k < 16; ++k)
				localM[i * 16 + k] = m[k];
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
				if (pinpoint) {
					tool::out() << " t=(" << localT[i * 3 + 0] << ","
					<< localT[i * 3 + 1] << "," << localT[i * 3 + 2] << ")";
					// Column magnitudes (scale) + the first column pair
					// (rotation read): enough to spot a lost/identity
					// root local without a full matrix print.
					const float* m = &localM[i * 16];
					tool::out() << " cols=(" << m[0] << "," << m[1] << ","
						<< m[2] << "|" << m[4] << "," << m[5] << "," << m[6]
						<< "|" << m[8] << "," << m[9] << "," << m[10] << ")";
				}
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

// (namespace paktool stays open through Run below; the helpers above
// and Run share it, so Args resolves unqualified.)

// Runs the pak assembly; returns the process exit code (byte-identical
// to the CLI run). See pak_tool.h — PakArgs mirrors the full CLI
// surface (the original lossy 6-field map is what this replaces).
int Run(const PakArgs& c) {
	Args args;
	args.contentDir = c.contentDir;
	args.inVfsList = c.inVfsList;
	args.inVfs = c.inVfs;
	args.tour = c.tour;
	args.registryDir = c.registryDir;
	args.outPak = c.outPak;
	args.dumpPak = c.dumpPak;
	args.dumpNodes = c.dumpNodes;
	args.dumpManifest = c.dumpManifest;
	args.verify = c.verify;
	args.compress = c.compress;
	args.lz4Accel = c.lz4Accel;
	args.meshopt = c.meshopt;
	args.overdraw = c.overdraw;
	args.autoKtx = c.autoKtx;
	args.ignoreVfs = c.ignoreVfs;
	args.levelName = c.levelName;
	args.terrainDir = c.terrainDir;
	args.sunAzimuth = c.sunAzimuth;
	args.sunElevation = c.sunElevation;
	args.sunIntensity = c.sunIntensity;
	args.skyIntensity = c.skyIntensity;
	args.sunColor = c.sunColor;
	args.giProbes = c.giProbes;

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
			<< " terrain=" << reader->BlobsOfKind(smvk::PakAssetKind::Terrain).size()
			<< " levels=" << smvk::ListPakLevels(*reader).size()
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
		// Level detail: every level blob's name, scene key + sub-blob
		// list (kind + key + bytes) — the playable units this pak packs.
		for (const smvk::PakTocEntry* e : smvk::ListPakLevels(*reader)) {
			smvk::LevelBlob lv;
			if (!smvk::ParseLevelBlob(reader->Payload(*e), lv)) {
				tool::err() << "  level '" << e->path
					<< "' malformed\n";
				continue;
			}
			tool::out() << "  level '" << lv.name << "' ("
				<< e->path << ", " << lv.subs.size()
				<< " sub-blob(s), scene='"
				<< (lv.sceneKey.empty() ? "-" : lv.sceneKey)
				<< "'):\n";
			for (const smvk::LevelSubBlob& s : lv.subs)
				tool::out() << "    " << uint32_t(s.kind) << " "
					<< s.key << " (" << s.size << " B)\n";
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
	// Assembly NEVER builds meshlets (the architectural rule): the
	// cooked sets ride the .geometry — BuildFromMeshAsset registers
	// them verbatim (see gltf.cpp's meshlet registration) and the pak
	// serializes what it loaded. No meshlet flag here, no inline build:
	// the artifact owns the table, the pak assembles it. (The brief
	// importMeshletCfg=true experiment was wrong twice over — it only
	// affected glTF-source imports that this path never takes, and
	// assembly deciding bake policy is exactly the bug class the
	// artifact/pipeline split exists to kill.)
	smvk::MeshletConfig importMeshletCfg;

	// Project cache (convention, no flag): explicit programmatic roots
	// (the editor's store) arrive via args.registryDir; bare CLI packs
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
			<< "/.tourmaline — plain pack (no acceleration)\n";
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
		// Export settings (the cook-block REPLACEMENT): the scene's
		// exportSettings NODE. Read straight off the doc's command list
		// (ExportSettingsFromDoc) — the merge must land before the
		// definition build below; the replay consumes the SAME lines, so
		// the results agree by construction. Only the pak-relevant
		// fields merge here (meshopt/autoKtx); CLI flags win.
		{
			SceneExportSettings es{};
			std::string esWarn;
			const bool haveEs =
				smvk::tour::ExportSettingsFromDoc(tour, es, &esWarn);
			if (haveEs) {
				auto use = [&](const char* key, auto& dst, auto val) {
					if (!args.explicitSet.contains(key)) {
						dst = val;
						tool::out() << "Tour cook default: " << key << "\n";
					}
				};
				use("meshopt", args.meshopt, es.meshopt);
				use("autoKtx", args.autoKtx, es.autoKtx);
			}
			if (!esWarn.empty())
				tool::err() << "Tour exportSettings: " << esWarn
					<< std::endl;
			// ignoreVfs: CLI pin wins; else the node's default (-1 unset).
			// On = the loose VFS bake-file discovery below stays OFF (the
			// minimal-pak switch — everything else embeds by path).
			if (args.ignoreVfs < 0)
				args.ignoreVfs = es.ignoreVfs ? 1 : 0;
			// NOTE: no sun/sky keys — the env blocks (env.default.* /
			// env.n.*, applied by ApplyTourToCluster into the cluster
			// scene) are the sun/sky of record for every stage (see
			// EnvForBake).
		}
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
			// The cooked manifest (<stem>.cooked beside the blob): proof
			// cook.exe baked THIS artifact and wrote the cooked blobs
			// back. Missing = never cooked (run cook.exe); artifact
			// newer than the manifest = re-derived/touched after the
			// cook (import.exe --force invalidates cooking — rerun
			// cook.exe). Pak never bakes, so this is a hard error.
			{
				std::string stem = smvk::vfs::FileName(artPath);
				const size_t mdot = stem.rfind(".geometry");
				const size_t mdotLegacy = stem.rfind(".mesh");
				if (mdot != std::string::npos)
					stem = stem.substr(0, mdot);
				else if (mdotLegacy != std::string::npos)
					stem = stem.substr(0, mdotLegacy);
				const std::string manRel = smvk::vfs::Join(
					smvk::vfs::BaseDir(artPath), stem + ".cooked");
				if (auto manBytes = vfs.TryRead(manRel);
					!manBytes || manBytes->empty()) {
					tool::err() << "Artifact not cooked: '" << inVfs
						<< "' has no '" << manRel << "' (run cook.exe "
						"on the artifact first — pak.exe assembles "
						"cooked artifacts only)" << std::endl;
					return 1;
				}
				std::error_code ec1, ec2;
				const auto artTime = std::filesystem::last_write_time(
					std::filesystem::path(args.contentDir) / artPath, ec1);
				const auto manTime = std::filesystem::last_write_time(
					std::filesystem::path(args.contentDir) / manRel, ec2);
				if (!ec1 && !ec2 && artTime > manTime) {
					tool::err() << "Artifact stale: '" << inVfs
						<< "' is newer than its '" << manRel
						<< "' (re-derived after the cook — rerun cook.exe)"
						<< std::endl;
					return 1;
				}
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
	// Tour-bound artifacts carry the same cook contract (the artifact-loop
	// check above does not see them): every source artifact behind the
	// replayed cluster must have its <stem>.cooked manifest, and no
	// artifact may be newer than it. URIs key the check (file parts).
	if (!args.tour.empty()) {
		std::unordered_set<std::string> files;
		for (auto& sc : sceneClusters) {
			for (const auto& [node, meshIdx] : sc->scene.meshForNode) {
				const auto h = sc->staticMeshes.getHandle(meshIdx);
				if (!h.valid())
					continue;
				const auto uri = sc->GetMeshUri(h);
				if (!uri.empty())
					files.insert(smvk::AssetFilePart(uri));
			}
		}
		for (const std::string& file : files) {
			std::string stem = smvk::vfs::FileName(file);
			if (const size_t mdot = stem.rfind(".geometry");
				mdot != std::string::npos)
				stem = stem.substr(0, mdot);
			const std::string manRel = smvk::vfs::Join(
				smvk::vfs::BaseDir(file), stem + ".cooked");
			if (auto manBytes = vfs.TryRead(manRel);
				!manBytes || manBytes->empty()) {
				tool::err() << "Tour source not cooked: '" << file
					<< "' has no '" << manRel << "' (run cook.exe "
					"--tour first — pak.exe assembles cooked "
					"artifacts only)" << std::endl;
				return 1;
			}
			std::error_code ec1, ec2;
			const auto artTime = std::filesystem::last_write_time(
				std::filesystem::path(args.contentDir) / file, ec1);
			const auto manTime = std::filesystem::last_write_time(
				std::filesystem::path(args.contentDir) / manRel, ec2);
			if (!ec1 && !ec2 && artTime > manTime) {
				tool::err() << "Tour source stale: '" << file
					<< "' is newer than its '" << manRel
					<< "' (re-derived after the cook — rerun cook.exe "
					"--tour)" << std::endl;
				return 1;
			}
		}
	}
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
	// Null in terrain-only cooks (no clusters); every use below is
	// guarded by a sceneClusters loop, so no null flows anywhere.
	smvk::Cluster* cluster =
		sceneClusters.empty() ? nullptr : sceneClusters.front().get();

	std::vector<std::pair<std::string, std::vector<uint8_t>>> giBlobs;

	// THE PAK GATE + the record-bound GI embed (Engine/GI/bake_inputs.h —
	// the invalidation model). Every baked-GI artifact the scene binds
	// (the world field, each convex volume, the cubemap capture set) is
	// FRESHNESS-CHECKED against the CURRENTLY replayed scene before it
	// can enter the pak: the gate recomputes each unit's bake-input hash
	// (env + static lights + the geometry merkle + the unit def + the
	// canonical config) and REFUSES on a mismatch or a hashless record —
	// an unverifiable bake is a stale bake, and stale lighting must
	// NEVER ride into a pak silently (the incident: ten-day-old daylight
	// cubemaps embedding into a dusk-cooked pak). A refusal fails the
	// pak (exit 1): re-bake (the editor's Bake GI / the cook's --cubemap
	// stage) before packing.
	// ---------------------------------------------------------------------------
	if (args.ignoreVfs <= 0) {
		// The canonical config: the tour's exportSettings node (the
		// authoring surface — the gate re-derives exactly what an
		// un-flagged bake derives; CLI-overridden bakes mismatch by
		// design).
		::SceneExportSettings esNode{};
		smvk::tour::ExportSettingsFromDoc(tour, esNode, nullptr);
		// The FIRST scene cluster carries the replayed binding state
		// (multi-scene paks share the tour's records).
		smvk::Cluster* gateCluster =
			sceneClusters.empty() ? nullptr : sceneClusters.front().get();
		bool gateFailed = false;
		auto gateRefuse = [&](const std::string& what,
				const std::string& detail) {
			tool::err() << "STALE BAKE REFUSED: " << what << " — " << detail
				<< "\n  Re-bake (Bake GI / cook --cubemap) before "
					"packing; the pak is NOT written.\n";
			gateFailed = true;
		};
		// The gate's component hashes (the refuse lines name the
		// diverging side — env / lights / grid — so an asymmetry is
		// diagnosable from the log alone).
		auto diagParts = [&](const smvk::Cluster& c) {
			return "env=" + smvk::gi::BakeEnvHash(c.scene)
				+ " lights=" + smvk::gi::BakeLightsHash(c)
				+ " grid=" + smvk::gi::BuildBakeCellGrid(
					const_cast<smvk::Cluster&>(c)).root;
		};
		smvk::gi::BakeCellGrid grid;
		bool gridBuilt = false;
		auto ensureGrid = [&]() -> const smvk::gi::BakeCellGrid& {
			if (!gridBuilt) {
				if (gateCluster)
					grid = smvk::gi::BuildBakeCellGrid(*gateCluster);
				gridBuilt = true;
			}
			return grid;
		};

		// Bound payloads embed ONCE (multi-scene paks: every cluster's
		// scene carries the same tour bindings — the dedup keeps AddBlob
		// one entry per path), bindings register PER CLUSTER (the union
		// registry dedups again downstream).
		std::set<std::string> embeddedGi;
		auto embedGi = [&](const std::string& vfsPath,
			const char* what) -> bool {
			if (vfsPath.empty())
				return false;
			if (!embeddedGi.insert(vfsPath).second)
				return true; // already embedded: the dedup no-op
			std::vector<uint8_t> bytes =
				ReadLooseBakeFile(args.contentDir, vfsPath);
			if (bytes.empty()) {
				tool::err() << "bound " << what << " '" << vfsPath
					<< "' unreadable — not embedded\n";
				return false;
			}
			giBlobs.emplace_back(vfsPath, std::move(bytes));
			return true;
		};

		if (gateCluster) {
			const auto& sn = gateCluster->scene;
			// ---- World field (giAsset kind world) ----
			if (!sn.giWorldSource.empty()) {
				const std::string fresh = smvk::gi::WorldBakeUnitHash(
					ensureGrid(), *gateCluster,
					smvk::gi::CanonicalWorldCfgHash(
						esNode.maskTransmission));
				if (sn.giWorldInputHash.empty())
					gateRefuse("the world field '" + sn.giWorldSource + "'",
						"the record carries NO bake-input hash (a "
						"pre-invalidation bake)");
				else if (fresh != sn.giWorldInputHash)
					gateRefuse("the world field '" + sn.giWorldSource + "'",
						"the scene changed since the bake (env/lights/"
						"geometry or the bake config); recorded="
						+ sn.giWorldInputHash + " fresh=" + fresh
						+ "; gate " + diagParts(*gateCluster));
				else if (embedGi(sn.giWorldSource, "world-field bake"))
					for (auto& sc : sceneClusters)
						sc->RegisterGiBinding(
							smvk::GiBinding::Kind::Probe,
							sn.giWorldSource);
			}
			// ---- GI volumes (giAsset kind givolume) ----
			for (const auto& [node, def] : sn.giVolumeForNode) {
				if (def.bakeSource.empty())
					continue; // authored but never baked: embeds nothing
				const std::string fresh =
					node < sn.globalTransform.size()
					? smvk::gi::GiVolumeBakeUnitHash(ensureGrid(),
						*gateCluster, def, sn.globalTransform[node],
						smvk::gi::CanonicalGiVolumeCfgHash(
							esNode.maskTransmission))
					: std::string();
				if (def.bakeInputHash.empty())
					gateRefuse("the GI volume '" + def.bakeSource + "'",
						"the record carries NO bake-input hash (a "
						"pre-invalidation bake)");
				else if (fresh != def.bakeInputHash)
					gateRefuse("the GI volume '" + def.bakeSource + "'",
						"the scene changed since the bake; recorded="
						+ def.bakeInputHash + " fresh=" + fresh
						+ "; gate " + diagParts(*gateCluster));
				else if (embedGi(def.bakeSource, "GI-volume bake"))
					for (auto& sc : sceneClusters)
						sc->RegisterGiBinding(
							smvk::GiBinding::Kind::GiVolume, def.bakeSource);
			}
			// ---- The cubemap capture set (giAsset kind captures) ----
			if (!sn.giCapturesSource.empty()) {
				const std::string manifestVfs = sn.giCapturesSource;
				std::vector<uint8_t> manifestBytes =
					ReadLooseBakeFile(args.contentDir, manifestVfs);
				if (manifestBytes.empty()) {
					gateRefuse("the capture set '" + manifestVfs + "'",
						"the bound manifest is unreadable");
				}
				else {
					std::string mErr;
					std::vector<smvk::CubemapManifestEntry> rows;
					const std::string_view text(
						reinterpret_cast<const char*>(manifestBytes.data()),
						manifestBytes.size());
					if (!smvk::CubemapManifestParse(text, rows, mErr)) {
						gateRefuse("the capture set '" + manifestVfs + "'",
							"manifest rejected: " + mErr);
					}
					else {
						// The .reach sidecar (the observed-cell
						// bookkeeping; the path derives from the bound
						// manifest's uuid).
						const std::string reachVfs = manifestVfs.substr(
							0, manifestVfs.size() - 13) + "reach";
						const std::vector<uint8_t> reachBytes =
							ReadLooseBakeFile(args.contentDir, reachVfs);
						smvk::gi::CaptureReachFile reach;
						if (reachBytes.empty()
								|| !smvk::gi::ParseCaptureReach(
									reachBytes.data(), reachBytes.size(),
									reach)) {
							gateRefuse("the capture set '" + manifestVfs
								+ "'", "the .reach sidecar is missing or "
									"unreadable");
						}
						else {
							auto rowCells = [&](uint32_t row) {
								for (const auto& e : reach.entries)
									if (e.rowIndex == row)
										return &e.cells;
								return static_cast<
									const std::vector<
										smvk::gi::CellId>*>(nullptr);
							};
							const smvk::gi::CubemapBakeConfigHashInputs
								cfgHash = smvk::gi::CanonicalCubemapCfgHash(
									esNode);
							// The set-level fast path: the recorded fold
							// matches → every row is fresh.
							std::string folded;
							for (const auto& r : rows)
								folded += r.inputHash;
							const bool setFresh =
								!sn.giCapturesHash.empty()
								&& smvk::gi::BakeInputHash(folded)
									== sn.giCapturesHash;
							if (!setFresh) {
								for (size_t r = 0; r < rows.size(); ++r) {
									const auto& row = rows[r];
									if (row.inputHash.empty()) {
										gateRefuse("the capture '"
											+ row.vfsPath + "'",
											"the row carries NO "
											"bake-input hash (a "
											"pre-invalidation bake)");
										continue;
									}
									const auto* cells = rowCells(
										uint32_t(r));
									const std::string fresh =
										smvk::gi::CubemapBakeUnitHash(
											ensureGrid(), *gateCluster,
											row.position,
											row.influenceRadius,
											row.fadeRadius, row.boxMin,
											row.boxMax, row.pinned,
											cfgHash,
											cells ? *cells
												: std::vector<
													smvk::gi::CellId>{});
									if (fresh != row.inputHash)
										gateRefuse("the capture '"
											+ row.vfsPath + "'",
											"the scene changed since the "
											"bake (env/lights/geometry "
											"or the capture config)");
								}
							}
							if (!gateFailed) {
								embedGi(manifestVfs,
									"capture-set manifest");
								embedGi(reachVfs, "capture-reach sidecar");
								for (const auto& row : rows)
									embedGi(row.vfsPath, "cubemap capture");
								for (auto& sc : sceneClusters)
									sc->RegisterGiBinding(
										smvk::GiBinding::Kind::Cubemap,
										manifestVfs);
								tool::out()
									<< "Registered + embedded capture set '"
									<< manifestVfs << "' (" << rows.size()
									<< " captures, gate-verified"
									<< (setFresh ? " via the set fold"
										: " per-row")
									<< ")\n";
							}
						}
					}
				}
			}
		}
		if (gateFailed)
			return 1;
		// ---- The REGULAR-volume manifest (a different bake family,
		// not scene-bound yet): the legacy stem discovery stays ----
		{
			const std::vector<std::string> stems = BakeStems(args);
			auto findStemWith = [&](const char* suffix) {
				for (const std::string& stem : stems) {
					const std::string vfs =
						"gi/" + stem + "." + suffix;
					if (!ReadLooseBakeFile(args.contentDir, vfs).empty())
						return stem;
				}
				return std::string();
			};
			const std::string stem = findStemWith("regmanifest");
			if (!stem.empty()) {
				const std::string manifestVfs =
					"gi/" + stem + ".regmanifest";
				std::vector<uint8_t> manifest =
					ReadLooseBakeFile(args.contentDir, manifestVfs);
				if (manifest.empty()) {
					tool::err() << "volume manifest '" << manifestVfs
						<< "' unreadable — binding kept, not embedded\n";
				}
				else {
					giBlobs.emplace_back(manifestVfs, std::move(manifest));
					std::vector<std::string> rowList;
					std::string cur;
					for (char c : giBlobs.back().second) {
						if (c == '\n' || c == '\r') {
							const std::string p =
								smvk::bake::SplitManifestRow(cur);
							if (!p.empty())
								rowList.push_back(p);
							cur.clear();
						}
						else
							cur.push_back(c);
					}
					const std::string p =
						smvk::bake::SplitManifestRow(cur);
					if (!p.empty())
						rowList.push_back(p);
					for (const std::string& row : rowList) {
						std::vector<uint8_t> bytes =
							ReadLooseBakeFile(args.contentDir, row);
						if (bytes.empty()) {
							tool::err() << "volume bake file '" << row
								<< "' unreadable — not embedded\n";
							continue;
						}
						giBlobs.emplace_back(row, std::move(bytes));
					}
				}
				for (auto& sc : sceneClusters)
					sc->RegisterGiBinding(
						smvk::GiBinding::Kind::RegManifest, stem);
				tool::out() << "Registered regmanifest GI '" << stem
					<< "'\n";
			}
		}
	}
	else {
		tool::out() << "ignoreVfs: loose bake-file resolution skipped - "
			"the pak carries the scene's own content only (no gi/ "
			"bake payloads, no bindings)\n";
	}

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

	// GI blobs embedded into the pak (discovery only — pak bakes
	// nothing; cook + GiBaker own every bake): the discovered GiBaker
	// bake files (probe octree, regular/convex manifests + rows) and the
	// lightmap .sglm. Every entry is (vfsPath, bytes) — Texture blobs
	// at gi/... paths embedded after Serialize below (PakBackend serves
	// by path).


	// ---------------------------------------------------------------------------
	// Preconvolved specular cubemaps: the COOK baked + wrote them loose
	// (gi/cubemaps/<base>[_N].ktx2 + the .cubemanifest, via
	// CubemapVfsPath/CubemapManifestVfsPath — the ID-keyed capture-set
	// paths under the canonical gi/ bake-asset home); the loose
	// discovery below embeds them as Texture blobs after SerializeClusters
	// (PakBackend serves by path). Pak never captures — bake flags are
	// rejected above (run cook.exe).
	// ---------------------------------------------------------------------------
	std::vector<std::pair<std::string, std::vector<uint8_t>>> cubemapKtx;
	// ---------------------------------------------------------------------------


	// Probe names (--giProbes), registered on every cooked cluster (the
	// serializer unions them into one pak-level Gi registry). Names are
	// the STABLE bake stems now (GiBaker writes gi/<stem>.sggi directly):
	// the runtime resolves gi/<name>.sggi by path, pak or loose.
	for (const std::string& probe : args.giProbes) {
		for (auto& sc : sceneClusters)
			sc->RegisterGiBinding(smvk::GiBinding::Kind::Probe, probe);
		tool::out() << "Registered probe GI '" << probe << "'\n";
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
	if (cookPtrs.empty()) {
		// Terrain-only pak (no clusters): valid, terrain blobs carry it.
		// Anything else was rejected at ParseArgs (artifacts/tour empty
		// without --terrain never reaches here).
		if (args.terrainDir.empty()) {
			tool::err() << "Serialize failed: no clusters to serialize"
				<< std::endl;
			return 1;
		}
	}
	else if (!SerializeClusters(cookPtrs, sceneNames, writer, &stats, &err)) {
		tool::err() << "Serialize failed: " << err << std::endl;
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
			tool::err() << "Cubemap AddBlob failed: " << vfsPath << std::endl;
			return 1;
		}
		const bool isManifest = vfsPath.size() >= 13
			&& vfsPath.compare(vfsPath.size() - 13, 13, ".cubemanifest") == 0;
		tool::out() << (isManifest ? "Embedded cubemap manifest " : "Embedded cubemap ")
			<< vfsPath << " (" << ktx.size() << " B"
			<< (isManifest ? " text" : " ktx2") << ")\n";
	}
	// GI payloads embedded into the pak (the discovered GiBaker
	// bake files + the lightmap .sglm — pak bakes nothing): Texture
	// blobs at their gi/... paths — PakBackend serves by path, so the
	// runtime's binding-first loaders read everything straight out of
	// this pak (no loose bake files needed; loose files still override
	// through the overlay by the same paths). Same path-embed pattern
	// as the .cubemanifest text above.
	for (auto& [vfsPath, bytes] : giBlobs) {
		std::vector<uint8_t> payload = smvk::BuildTexturePayload(
			vfsPath, -1, -1, -1, -1, std::span<const uint8_t>(bytes));
		if (!writer.AddBlob(smvk::PakAssetKind::Texture, vfsPath,
			std::move(payload))) {
			tool::err() << "GI AddBlob failed: " << vfsPath << std::endl;
			return 1;
		}
		tool::out() << "Embedded GI " << vfsPath
			<< " (" << bytes.size() << " B)\n";
	}
	// Tiled terrain (optional, independent of clusters): terrain.json +
	// H/S PNGs -> Terrain blobs. Runs for mixed and terrain-only paks;
	// empty sceneClusters serialize to zero scene blobs (valid: the
	// runtime loads terrain straight from the Terrain blobs).
	if (!args.terrainDir.empty()) {
		std::string terrErr;
		if (!terraincook::CookTerrainBlobs(args.terrainDir, writer, terrErr)) {
			tool::err() << "Terrain cook failed: " << terrErr << std::endl;
			return 1;
		}
	}
	// ---- LEVEL blob (the exploration spike; see Engine/Asset/level.h) ----
	// ONE composite levels/<name>.level blob packing the playable unit:
	// the scene payload + the baked data this pak carries (the cubemap
	// set manifest + KTX2 rows from cubemapKtx + the GI payloads from
	// giBlobs). Sub-blobs are addressed by their canonical keys (the
	// vfs paths the standalone blobs ride), so loaders consume the
	// unpacked bytes unchanged — the LEVEL owns the data; the
	// scene-stem bake-file association dies at the pak level.
	// ORDERING: this runs AFTER the pak file write + AppendPakBlobs —
	// the scene sub-blob comes straight out of the WRITTEN pak (the
	// index-0 "#scene" entry), never a second serialize (the cluster
	// was consumed by the main SerializeClusters; serializing again
	// double-registers URIs — the access violation the first cut hit).
	std::vector<smvk::LevelSubInput> levelSubs;
	for (auto& [vfsPath, ktx] : cubemapKtx)
		levelSubs.push_back({ smvk::PakAssetKind::Texture, vfsPath,
			std::span<const uint8_t>(ktx) });
	for (auto& [vfsPath, bytes] : giBlobs)
		levelSubs.push_back({ smvk::PakAssetKind::Texture, vfsPath,
			std::span<const uint8_t>(bytes) });
	std::vector<uint8_t> pak = writer.Finalize();

	{
		std::filesystem::path out(args.outPak);
		std::error_code ec;
		std::filesystem::create_directories(out.parent_path(), ec);
		FILE* f = std::fopen(out.string().c_str(), "wb");
		if (!f) {
			tool::err() << "Cannot open output: " << args.outPak << std::endl;
			return 1;
		}
		fwrite(pak.data(), 1, pak.size(), f);
		fclose(f);
	}

	// Level append: read the scene blob back from the JUST-WRITTEN pak
	// (the exact bytes the standalone scene blob rides), pack it + the
	// data sub-blobs above, append the level blob to the pak file
	// (AppendPakBlobs keeps the image valid at every step).
	if (!args.levelName.empty()) {
		// Read the written pak (borrowed spans must outlive the image).
		std::vector<uint8_t> pakImage;
		{
			std::ifstream pf(args.outPak, std::ios::binary
				| std::ios::ate);
			if (!pf) {
				tool::err() << "Level read-back failed: " << args.outPak
					<< std::endl;
				return 1;
			}
			const auto sz = pf.tellg();
			pf.seekg(0);
			pakImage.resize(size_t(sz));
			if (!pf.read(reinterpret_cast<char*>(pakImage.data()), sz)) {
				tool::err() << "Level read-back failed: " << args.outPak
					<< std::endl;
				return 1;
			}
		}
		std::string lOpenErr;
		auto pakReader = smvk::PakReader::Open(
			std::span<const uint8_t>(pakImage), lOpenErr);
		const smvk::PakTocEntry* sceneEntry = nullptr;
		if (pakReader)
			for (const smvk::PakTocEntry* e :
				pakReader->BlobsOfKind(smvk::PakAssetKind::Scene))
				if (!sceneEntry || e->path < sceneEntry->path)
					sceneEntry = e; // "#scene" sorts first
		std::vector<smvk::LevelSubInput> subs;
		if (sceneEntry)
			subs.push_back({ smvk::PakAssetKind::Scene, "scene",
				pakReader->Payload(*sceneEntry) });
		else
			tool::err() << "Level: no scene blob in the pak - packing"
				" a data-only level\n";
		for (const smvk::LevelSubInput& s : levelSubs)
			subs.push_back(s);
		std::vector<uint8_t> levelBlob;
		if (!smvk::BuildLevelBlob(args.levelName, "scene", subs,
				levelBlob)) {
			tool::err() << "Level blob build failed" << std::endl;
			return 1;
		}
		const std::string levelPath =
			"levels/" + args.levelName + ".level";
		std::vector<std::pair<std::string,
			std::span<const uint8_t>>> append;
		append.emplace_back(levelPath,
			std::span<const uint8_t>(levelBlob));
		std::vector<smvk::PakAppendInfo> appendInfo;
		std::string appendErr;
		if (!smvk::AppendPakBlobs(args.outPak,
				smvk::PakAssetKind::Level, append, appendInfo, appendErr)) {
			tool::err() << "Level append failed: " << levelPath
				<< (appendErr.empty() ? "" : ": " + appendErr)
				<< std::endl;
			return 1;
		}
		tool::out() << "Level '" << args.levelName << "' packed: "
			<< subs.size() << " sub-blob(s) (" << (sceneEntry ? 1 : 0)
			<< " scene, " << cubemapKtx.size() << " cubemap row(s), "
			<< giBlobs.size() << " GI) -> " << levelPath << "\n";
	}

	const auto t1 = std::chrono::steady_clock::now();
	const double secs = std::chrono::duration<double>(t1 - t0).count();

	tool::out() << "Cooked " << (args.inVfs.empty() ? "(terrain-only)" : args.inVfs)
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
		<< " terrain=" << [&] {
			size_t n = 0;
			for (const auto& e : writer.Entries()) {
				if (e.kind == smvk::PakAssetKind::Terrain)
					++n;
			}
			return n;
		}()
		<< " pak=" << pak.size() << " B"
		<< " staticRoots=" << stats.staticRoots
		<< " meshleted=" << stats.meshesWithMeshlets << "/" << stats.meshes
		<< " meshes," << stats.meshletCount << " meshlets"
		<< " compressed=" << writer.CompressedEntryCount() << "/" << writer.EntryCount()
		<< (args.compress ? " (lz4)" : " (raw)")
		<< " in " << secs << "s\n";

	// Self-check: re-open the written file through the production reader
	// (header/footer/CRC/TOC validation + payload spot-checks).
	std::optional<smvk::PakReader> verifyReader;
	if (args.verify || args.dumpManifest) {
		std::ifstream rf(args.outPak, std::ios::binary | std::ios::ate);
		if (!rf) {
			tool::err() << "Verify: cannot re-open " << args.outPak << std::endl;
			return 1;
		}
		const auto size = rf.tellg();
		rf.seekg(0);
		std::vector<uint8_t> image(static_cast<size_t>(size));
		if (!rf.read(reinterpret_cast<char*>(image.data()), size)) {
			tool::err() << "Verify: read failed" << std::endl;
			return 1;
		}
		std::string verr;
		// The image must outlive the reader borrow; keep it static duration.
		static std::vector<uint8_t> keepAlive = std::move(image);
		auto reader = smvk::PakReader::Open(
			std::span<const uint8_t>(keepAlive), verr);
		if (!reader) {
			tool::err() << "Verify: PakReader rejected: " << verr << std::endl;
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
			// legacy/digest-less fallback second — mirrors the runtime
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
		tool::out() << "  verify: entries=" << reader.EntryCount()
			<< " meshes=" << reader.BlobsOfKind(smvk::PakAssetKind::Mesh).size()
			<< " materials=" << reader.BlobsOfKind(smvk::PakAssetKind::Material).size()
			<< " textures=" << reader.BlobsOfKind(smvk::PakAssetKind::Texture).size()
			<< " digested=" << digested
			<< " cameras=" << reader.BlobsOfKind(smvk::PakAssetKind::Camera).size()
			<< " scene=" << reader.BlobsOfKind(smvk::PakAssetKind::Scene).size()
			<< (badPayloads ? " BAD PAYLOADS!" : " OK") << "\n";
		if (badPayloads) {
			tool::err() << "Verify: " << badPayloads << " malformed texture payload(s)\n";
			return 1;
		}
	}

	// No external manifest: the dump is printed straight from the pak TOC/blobs.
	if (args.dumpManifest && verifyReader) {
		DumpManifestFromPak(*verifyReader);
	}

	return 0;
}

} // namespace paktool

// The CLI stays a thin interface over the library (same argv, same
// output, same exit codes). The lib build (PAK_LIB_BUILD, linked
// into the editor) excludes main.
#ifndef PAK_LIB_BUILD
int main(int argc, char** argv) {
	// Unbuffered pipe output: hosts tailing this process through a
	// pipe stream each line live. Without this, stdout stays
	// block-buffered when piped and no progress appears until the
	// cook finishes (looks stuck).
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;
	paktool::Args args;
	if (!paktool::ParseArgs(argc, argv, args)) {
		paktool::Usage();
		return 2;
	}
	paktool::PakArgs c;
	c.contentDir = args.contentDir;
	c.inVfsList = args.inVfsList;
	c.inVfs = args.inVfs;
	c.tour = args.tour;
	c.registryDir = args.registryDir;
	c.outPak = args.outPak;
	c.dumpPak = args.dumpPak;
	c.dumpNodes = args.dumpNodes;
	c.dumpManifest = args.dumpManifest;
	c.verify = args.verify;
	c.compress = args.compress;
	c.lz4Accel = args.lz4Accel;
	c.meshopt = args.meshopt;
	c.overdraw = args.overdraw;
	c.autoKtx = args.autoKtx;
	c.ignoreVfs = args.ignoreVfs;
	c.levelName = args.levelName;
	c.terrainDir = args.terrainDir;
	c.sunAzimuth = args.sunAzimuth;
	c.sunElevation = args.sunElevation;
	c.sunIntensity = args.sunIntensity;
	c.skyIntensity = args.skyIntensity;
	c.sunColor = args.sunColor;
	c.giProbes = args.giProbes;
	return paktool::Run(c);
}
#endif
