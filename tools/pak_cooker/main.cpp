// PakCooker: offline glTF -> .pak cooker that works exclusively on the VFS
// and needs NO GPU (cook-mode Cluster, see Cluster::SetCookMode).
//
//   PakCooker --content-dir content --in-vfs models/Sponza/sponza.gltf
//             --out-pak content/paks/sponza.pak [--meshopt on|off]
//             [--overdraw 1.05] [--verify on|off] [--dump-manifest on|off]
//             [--lods on|off] [--lod-ratios 0.6,0.3,0.12] [--lod-error 0.02]
//             [--compress on|off] [--lz4-accel 1]
//             [--lightmap on|off --lm-density 32 --lm-lobes 9
//              --lm-atlas-width 2048 --lm-gutter 1 --lm-rays 128
//              --sun-azimuth 30 --sun-elevation 55 --sun-intensity 10000
//              --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]
//
// --lightmap on bakes 2D SG lightmaps DURING the cook (sun + static spot
// NEE, real materials, transmissive pass-through — see Engine/GI/bake.h),
// fills each mesh's texCoord2 with per-vertex lightmap UVs, stamps the
// mesh's atlas chunk id into the pak blob, and writes the atlas alongside
// the pak as <contentDir>/gi/<scene>.sglm. No bake-side work is left to
// the runtime: the pak + .sglm are the complete lightmap payload.
//
// Reads the model JSON, .bin buffers and all .ktx2 textures through an
// OverlayVfs with a LooseBackend over --content-dir (the same lookup rules
// as the runtime), runs the meshopt cook pass, and writes a self-contained
// pak with embedded textures. Everything about the pak is already inside
// the pak — there is intentionally no sidecar manifest. --dump-manifest
// prints the same data to stdout, read back from the pak via PakReader so
// it cannot drift from the written bytes.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Engine/App/cluster.h>
#include <Engine/App/static_merge.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Asset/pak.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/lightmap.h>
#include <Engine/Utils/gltf.h>
#include <Engine/Vfs/vfs.h>

namespace {

struct Args {
	std::string contentDir = "content";
	std::string inVfs;
	std::string outPak;
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
	uint32_t lmLobes = 9;        // 5|6|9|12
	uint32_t lmAtlasWidth = 2048;
	uint32_t lmGutter = 1;
	uint32_t lmRays = 128;       // radiance samples per texel
	uint32_t lmBounces = 3;
	float sunAzimuth = 30.0f;    // mirrors the runtime environment defaults
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	// Static spots for the bake: "px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela"
	std::vector<std::string> spots;
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
		else if (a == "--meshopt") {
			std::string v;
			if (!need(v)) return false;
			out.meshopt = (v == "on" || v == "1" || v == "true");
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
		}
		else if (a == "--lm-density") {
			std::string v;
			if (!need(v)) return false;
			out.lmDensity = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--lm-lobes") {
			std::string v;
			if (!need(v)) return false;
			out.lmLobes = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--lm-atlas-width") {
			std::string v;
			if (!need(v)) return false;
			out.lmAtlasWidth = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--lm-gutter") {
			std::string v;
			if (!need(v)) return false;
			out.lmGutter = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--lm-rays") {
			std::string v;
			if (!need(v)) return false;
			out.lmRays = static_cast<uint32_t>(std::stoul(v));
		}
		else if (a == "--lm-bounces") {
			std::string v;
			if (!need(v)) return false;
			out.lmBounces = static_cast<uint32_t>(std::stoul(v));
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
		else if (a == "--spot") {
			std::string v;
			if (!need(v)) return false;
			out.spots.push_back(v);
		}
		else if (a == "--help" || a == "-h") {
			return false;
		}
		else {
			std::cerr << "Unknown arg: " << a << std::endl;
			return false;
		}
	}
	if (out.inVfs.empty() || out.outPak.empty()) {
		std::cerr << "Required: --in-vfs <model vfs path> --out-pak <host path>" << std::endl;
		return false;
	}
	return true;
}

void Usage() {
	std::cout << "PakCooker --content-dir content --in-vfs models/Sponza/sponza.gltf\n"
		<< "          --out-pak content/paks/sponza.pak [--meshopt on|off]\n"
		<< "          [--overdraw 1.05] [--verify on|off] [--dump-manifest on|off]\n"
		<< "          [--lods on|off] [--lod-ratios 0.6,0.3,0.12] [--lod-error 0.02]\n"
		<< "          [--meshlets on|off] [--max-verts 64] [--max-tris 124]\n"
		<< "          [--cone-weight 0] [--collapse-static on|off]\n"
		<< "          [--compress on|off] [--lz4-accel 1]\n"
		<< "          [--lightmap on|off --lm-density 32 --lm-lobes 9\n"
		<< "           --lm-atlas-width 2048 --lm-gutter 1 --lm-rays 128\n"
		<< "           --lm-bounces 3 --sun-azimuth 30 --sun-elevation 55\n"
		<< "           --sun-intensity 10000\n"
		<< "           --spot px,py,pz,dx,dy,dz,innerDeg,outerDeg,range,r,g,b,candela]\n"
		<< "--lightmap on: bakes 2D SG lightmaps into the pak (per-vertex\n"
		<< "lightmap UVs + per-mesh chunk id; atlas written to\n"
		<< "<contentDir>/gi/<scene>.sglm). Sources: sun + static spots + real\n"
		<< "materials; transmissive (Blend) meshes pass light through.\n";
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
void DumpManifestFromPak(const smvk::PakReader& reader) {
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
			<< " lmChunk=" << (lmChunk == 0xFFFFFFFFu ? -1 : int(lmChunk)) << "\n";
	}

	// Materials.
	for (const smvk::PakTocEntry* e : reader.BlobsOfKind(smvk::PakAssetKind::Material)) {
		DumpCursor c{ reader.Payload(*e), 0 };
		const std::string uri = c.str();
		std::cout << "    mat  " << uri << "\n";
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
}

} // namespace

int main(int argc, char** argv) {
	Args args;
	if (!ParseArgs(argc, argv, args)) {
		Usage();
		return 2;
	}

	const auto t0 = std::chrono::steady_clock::now();

	// VFS: cooker reads inputs through the same overlay rules as runtime.
	smvk::OverlayVfs vfs;
	vfs.Mount(std::make_shared<smvk::LooseBackend>(args.contentDir), 0, "content");

	smvk::MeshOptConfig opt;
	opt.enabled = args.meshopt;
	opt.overdrawThreshold = args.overdraw;
	opt.generateLods = args.lods;
	opt.lodRatios = ParseLodRatios(args.lodRatios);
	opt.lodTargetError = std::clamp(args.lodError, 0.0f, 1.0f);

	smvk::MeshletConfig meshletCfg;
	meshletCfg.enabled = args.meshlets;
	meshletCfg.maxVertices = std::clamp(args.maxVerts, 3u, 255u);
	meshletCfg.maxTriangles = std::clamp(args.maxTris, 1u, 512u);
	meshletCfg.coneWeight = args.coneWeight;

	// NOTE: heap-allocated — Cluster embeds multi-MB pool arrays and must
	// never live on the (1MB) thread stack. Cook mode = GPU-free: CPU
	// geometry + retained uploads only, textures are validated .ktx2
	// payloads with minted slot ids.
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);

	try {
		GltfToClusterFromVfs(nullptr, vfs, args.inVfs, *cluster, opt, meshletCfg);
	}
	catch (const std::exception& e) {
		std::cerr << "Import failed: " << e.what() << std::endl;
		return 1;
	}

	// Static collapse: bake frozen subtrees into per-material mega-mesh
	// blocks (world-baked verts, identity nodes). Runs before Serialize so
	// the pak holds the collapsed scene.
	if (args.collapseStatic) {
		smvk::StaticMergeOptions mopts;
		smvk::StaticMergeResult mr = smvk::CollapseStaticSubtrees(*cluster, mopts);
		if (!mr.ok) {
			std::cerr << "CollapseStaticSubtrees failed: " << mr.err << std::endl;
			return 1;
		}
		std::cout << "Collapsed " << mr.srcNodesConsumed << " static nodes into "
			<< mr.blocks.size() << " block(s)\n";
		// Merged blocks skip the importer meshlet path; cook them here so
		// the single-block layout also feeds the indirect future.
		if (meshletCfg.enabled) {
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
	// through, tinted (no charts for them). Only 1:1 node->mesh mappings
	// get chunks: instanced meshes need per-instance coefficients.
	// ---------------------------------------------------------------------------
	if (args.lightmap) {
		using namespace smvk::gi;
		const auto lmT0 = std::chrono::steady_clock::now();

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
			std::cout << "  [LM] textures averaged: " << averaged
				<< " / " << cluster->ListTextureUris().size() << "\n";
		}
		buildOpts.env.sunAzimuthDeg = args.sunAzimuth;
		buildOpts.env.sunElevationDeg = args.sunElevation;
		buildOpts.env.sunIntensity = args.sunIntensity;

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
		BuildTraceSceneFromCluster(*cluster, buildOpts, traceScene, terr);
		if (!terr.empty()) {
			std::cerr << "[LM] trace scene failed: " << terr << std::endl;
			return 1;
		}
		traceScene.spots = std::move(spots);

		// Charts per eligible mesh (1:1 node->mesh, non-Blend material),
		// with budget-driven density fallback (see lightmap.h).
		struct LmWork {
			uint32_t node = 0;
			smvk::ClusterStaticMeshHandle handle{};
			uint32_t density = 0;
			SgLightmapChartResult chart;
		};
		SgLightmapChartOptions chartOpts{};
		chartOpts.texelsPerMeter = std::max<uint32_t>(args.lmDensity, 2);
		chartOpts.gutterTexels = std::clamp<uint32_t>(args.lmGutter, 1, 4);
		std::vector<LmWork> work;
		for (const auto& [node, meshIdx] : cluster->scene.meshForNode) {
			auto matIt = cluster->scene.materialForNode.find(node);
			if (matIt == cluster->scene.materialForNode.end())
				continue;
			const smvk::ClusterMaterialHandle matHandle =
				cluster->materials.getHandle(matIt->second);
			if (!matHandle.valid())
				continue;
			const smvk::Material* mat = cluster->materials.get(matHandle);
			if (!mat || mat->alphaMode == smvk::AlphaMode::Blend)
				continue; // transmissive: no charts (pass-through only)
			// Instancing check: a mesh shared by several nodes has one
			// lightmap position per instance — unsupported in v1.
			uint32_t refs = 0;
			for (const auto& [n2, m2] : cluster->scene.meshForNode)
				if (m2 == meshIdx)
					++refs;
			if (refs != 1)
				continue;
			const smvk::ClusterStaticMeshHandle handle =
				cluster->staticMeshes.getHandle(meshIdx);
			const smvk::StaticMeshUpload* upload = cluster->GetRetainedUpload(handle);
			if (!upload || upload->vertices.empty() || upload->indices.empty())
				continue;
			LmWork w{};
			w.node = node;
			w.handle = handle;
			uint32_t density = chartOpts.texelsPerMeter;
			bool ok = false;
			while (density >= 1) {
				SgLightmapChartOptions o = chartOpts;
				o.texelsPerMeter = density;
				std::string cerr_;
				ok = GenerateLightmapCharts(*upload, o, w.chart, cerr_);
				if (ok)
					break;
				if (density <= 1)
					break;
				density /= 2;
			}
			if (!ok)
				continue; // over-budget even at floor: probe/sky ambient
			w.density = density;
			work.push_back(std::move(w));
		}

		SgLightmapAtlas atlas;
		if (!work.empty()) {
			// Atlas pack with whole-atlas budget fallback (drop chunks that
			// cannot fit the shrinking budget; the app tolerates gaps).
			constexpr uint32_t kMaxAtlasH = 8192;
			constexpr uint64_t kAtlasTexelBudget = 4ull * 1024 * 1024;
			uint64_t rectBudget = chartOpts.maxRectTexels;
			uint32_t round = 0;
			while (true) {
				std::vector<SgLightmapAtlas::RectRequest> reqs;
				reqs.reserve(work.size());
				for (const LmWork& w : work)
					reqs.push_back({ glm::uvec2(w.chart.rectWidth, w.chart.rectHeight),
						args.lmLobes, w.node });
				if (atlas.PackRects(reqs, std::max<uint32_t>(args.lmAtlasWidth, 256),
						kMaxAtlasH)
					&& uint64_t(atlas.width) * atlas.height <= kAtlasTexelBudget)
					break;
				if (++round > 6 || work.empty()) {
					std::cerr << "[LM] atlas overflow (" << work.size()
						<< " chunks; lower --lm-density)\n";
					return 1;
				}
				rectBudget /= 2;
				std::vector<LmWork> kept;
				kept.reserve(work.size());
				for (LmWork& w : work) {
					std::string cerr_;
					uint32_t density = w.density;
					bool ok = false;
					while (density >= 1) {
						SgLightmapChartOptions o = chartOpts;
						o.texelsPerMeter = density;
						o.maxRectTexels = rectBudget;
						ok = GenerateLightmapCharts(*cluster->GetRetainedUpload(w.handle),
							o, w.chart, cerr_);
						if (ok)
							break;
						if (density <= 1)
							break;
						density /= 2;
					}
					if (ok) {
						w.density = density;
						kept.push_back(std::move(w));
					}
				}
				work = std::move(kept);
			}
		}

		// Bake every chunk (sun + spots + materials + pass-through glass).
		SgLightmapBakeConfig lcfg{};
		lcfg.raysPerTexel = std::max<uint32_t>(args.lmRays, 16);
		lcfg.maxBounces = std::min<uint32_t>(std::max<uint32_t>(args.lmBounces, 1), 8);
		SgLightmapBakeStats lstats{};
		uint32_t lmMeshes = 0;
		for (size_t ci = 0; ci < work.size(); ++ci) {
			LmWork& w = work[ci];
			// World-space texels: chart positions are node-local; the trace
			// scene is world-space.
			std::vector<SgLightmapTexel> texels;
			texels.reserve(w.chart.texels.size());
			const glm::mat4& g = cluster->scene.globalTransform[w.node];
			const glm::mat3 g3(g);
			for (const SgLightmapTexel& t : w.chart.texels) {
				SgLightmapTexel wt = t;
				wt.position = glm::vec3(g * glm::vec4(t.position, 1.0f));
				wt.normal = glm::normalize(g3 * t.normal);
				texels.push_back(wt);
			}
			SgLightmapBakeStats cs{};
			if (!BakeSgLightmapChunk(traceScene, args.lmLobes, texels, lcfg,
					atlas.payloads[ci], cs,
					[](const char* line) { std::cout << line; })) {
				std::cerr << "[LM] bake failed for node " << w.node << "\n";
				return 1;
			}
			lstats.texelsBaked += cs.texelsBaked;
			lstats.pathsTraced += cs.pathsTraced;
			lstats.samplesClamped += cs.samplesClamped;
			// Stamp the cook result: per-vertex texCoord2 + mesh chunk id.
			smvk::StaticMeshUpload* upload = nullptr;
			{
				// Retained uploads back the serialized blobs; write the
				// lightmap UVs into them (the only mutation the cook makes).
				if (smvk::StaticMeshUpload* up = const_cast<smvk::StaticMeshUpload*>(
						cluster->GetRetainedUpload(w.handle)))
					upload = up;
			}
			if (!upload || w.chart.uv2.size() != upload->vertices.size()) {
				std::cerr << "[LM] vertex count mismatch for node " << w.node
					<< " - chunk dropped\n";
				continue;
			}
			for (size_t v = 0; v < upload->vertices.size(); ++v)
				upload->vertices[v].texCoord2 = w.chart.uv2[v];
			if (smvk::StaticMesh* m = cluster->staticMeshes.get(w.handle))
				m->lmChunk = uint32_t(ci);
			++lmMeshes;
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
			std::cout << "  [LM] atlas " << sglmPath.generic_string()
				<< " (" << std::filesystem::file_size(sglmPath) << " B)\n";
		}
		std::cout << "  [LM] lightmaps: meshes=" << lmMeshes
			<< " chunks=" << atlas.chunks.size()
			<< " atlas=" << atlas.width << "x" << atlas.height
			<< " texels=" << lstats.texelsBaked
			<< " paths=" << lstats.pathsTraced
			<< " clamped=" << lstats.samplesClamped
			<< " seconds=" << lstats.seconds << "\n";
	}

	smvk::PakWriter writer;
	writer.SetCompression(
		args.compress ? smvk::PakCompression::Lz4 : smvk::PakCompression::None,
		args.lz4Accel);
	smvk::ClusterSerializeStats stats;
	std::string err;
	if (!SerializeCluster(*cluster, writer, &stats, &err)) {
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
		<< "  meshes=" << stats.meshes << " materials=" << stats.materials
		<< " textures=" << stats.textures << " (" << stats.textureBytes << " B)"
		<< " nodes=" << stats.nodes << " pak=" << pak.size() << " B"
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
