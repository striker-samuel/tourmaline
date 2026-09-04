// PakCooker: offline glTF -> .pak cooker that works exclusively on the VFS
// and needs NO GPU (cook-mode Cluster, see Cluster::SetCookMode).
//
//   PakCooker --content-dir content --in-vfs models/Sponza/sponza.gltf
//             --out-pak content/paks/sponza.pak [--meshopt on|off]
//             [--overdraw 1.05] [--verify on|off] [--dump-manifest on|off]
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
	// Meshlet cook path (off by default; indirect-draw foundation only).
	bool meshlets = false;
	uint32_t maxVerts = 64;
	uint32_t maxTris = 124;
	float coneWeight = 0.0f;
	// Collapse static subtrees into per-material mega-mesh blocks.
	bool collapseStatic = false;
};

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
		<< "          [--meshlets on|off] [--max-verts 64] [--max-tris 124]\n"
		<< "          [--cone-weight 0] [--collapse-static on|off]\n";
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
		(void)c.u32(); // candidateCount
		(void)c.f32(); // simplifyError
		(void)c.f32(); // screenThreshold
		const uint32_t tris = c.u32();
		const float acmr = c.f32();
		if (!c.ok) {
			std::cout << "    [truncated mesh blob: " << e->path << "]\n";
			continue;
		}
		// Vertex/index payloads (60 B per vert, 4 B per index).
		c.off += (size_t)vc * 60 + (size_t)ic * 4;
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
			<< " lod{g=" << groupId << " i=" << lodIndex << " tris=" << tris
			<< " acmr=" << acmr << "}"
			<< " meshlets=" << meshlets << "\n";
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

	smvk::PakWriter writer;
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
