// PakCooker: offline glTF -> .pak cooker that works exclusively on the VFS
// and needs NO GPU (cook-mode Cluster, see Cluster::SetCookMode).
//
//   PakCooker --content-dir content --in-vfs models/Sponza/sponza.gltf
//             --out-pak content/paks/sponza.pak [--meshopt on|off]
//             [--overdraw 1.05] [--manifest on|off]
//
// Reads the model JSON, .bin buffers and all .ktx2 textures through an
// OverlayVfs with a LooseBackend over --content-dir (the same lookup rules
// as the runtime), runs the meshopt cook pass, and writes a self-contained
// pak with embedded textures plus a .pak.json manifest sidecar.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <Engine/App/cluster.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/mesh_optimizer.h>
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
	bool manifest = true;
	bool verify = true; // re-open the written pak with PakReader (self-check)
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
		else if (a == "--manifest") {
			std::string v;
			if (!need(v)) return false;
			out.manifest = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--verify") {
			std::string v;
			if (!need(v)) return false;
			out.verify = (v == "on" || v == "1" || v == "true");
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
		<< "          [--overdraw 1.05] [--manifest on|off]\n";
}

std::string JsonEscape(const std::string& s) {
	std::string o;
	for (char c : s) {
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		default: o.push_back(c); break;
		}
	}
	return o;
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

	// NOTE: heap-allocated — Cluster embeds multi-MB pool arrays and must
	// never live on the (1MB) thread stack. Cook mode = GPU-free: CPU
	// geometry + retained uploads only, textures are validated .ktx2
	// payloads with minted slot ids.
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);

	try {
		GltfToClusterFromVfs(nullptr, vfs, args.inVfs, *cluster, opt);
	}
	catch (const std::exception& e) {
		std::cerr << "Import failed: " << e.what() << std::endl;
		return 1;
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
		<< " in " << secs << "s\n";

	// Self-check: re-open the written file through the production reader
	// (header/footer/CRC/TOC validation + payload spot-checks).
	if (args.verify) {
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
		auto reader = smvk::PakReader::Open(std::span<const uint8_t>(image), verr);
		if (!reader) {
			std::cerr << "Verify: PakReader rejected: " << verr << std::endl;
			return 1;
		}
		size_t badPayloads = 0;
		for (const smvk::PakTocEntry* e : reader->BlobsOfKind(smvk::PakAssetKind::Texture)) {
			smvk::PakTexturePayload p;
			if (!smvk::ParseTexturePayload(reader->Payload(*e), p))
				++badPayloads;
		}
		std::cout << "  verify: entries=" << reader->EntryCount()
			<< " meshes=" << reader->BlobsOfKind(smvk::PakAssetKind::Mesh).size()
			<< " materials=" << reader->BlobsOfKind(smvk::PakAssetKind::Material).size()
			<< " textures=" << reader->BlobsOfKind(smvk::PakAssetKind::Texture).size()
			<< " scene=" << reader->BlobsOfKind(smvk::PakAssetKind::Scene).size()
			<< (badPayloads ? " BAD PAYLOADS!" : " OK") << "\n";
		if (badPayloads) {
			std::cerr << "Verify: " << badPayloads << " malformed texture payload(s)\n";
			return 1;
		}
	}

	if (args.manifest) {
		std::ostringstream m;
		m << "{\n  \"source\": \"" << JsonEscape(args.inVfs) << "\",\n";
		m << "  \"pakBytes\": " << pak.size() << ",\n";
		m << "  \"meshes\": [\n";
		bool first = true;
		cluster->staticMeshes.forEachHandle([&](smvk::ClusterStaticMeshHandle h) {
			const std::string uri = cluster->GetMeshUri(h);
			auto lod = cluster->GetMeshLod(uri.empty() ? "" : uri);
			if (!first) m << ",\n";
			first = false;
			m << "    {\"uri\": \"" << JsonEscape(uri) << "\"";
			if (lod) {
				m << ", \"lodGroup\": " << lod->groupId
					<< ", \"lodIndex\": " << lod->lodIndex
					<< ", \"tris\": " << lod->triCount
					<< ", \"acmr\": " << lod->acmr;
			}
			m << "}";
			});
		m << "\n  ],\n  \"materials\": [";
		first = true;
		cluster->materials.forEachHandle([&](smvk::ClusterMaterialHandle h) {
			if (!first) m << ", ";
			first = false;
			m << "\"" << JsonEscape(cluster->GetMaterialUri(h)) << "\"";
			});
		m << "],\n  \"textures\": [";
		first = true;
		for (const auto& [uri, rhi] : cluster->ListTextureUris()) {
			(void)rhi;
			const auto* src = cluster->GetTextureSource(uri);
			if (!first) m << ", ";
			first = false;
			m << "{\"uri\": \"" << JsonEscape(uri) << "\", \"bytes\": "
				<< (src ? src->size() : 0) << "}";
		}
		m << "]\n}\n";
		const std::string mpath = args.outPak + ".json";
		FILE* f = std::fopen(mpath.c_str(), "wb");
		if (f) {
			const std::string s = m.str();
			fwrite(s.data(), 1, s.size(), f);
			fclose(f);
			std::cout << "  manifest: " << mpath << "\n";
		}
		else {
			std::cerr << "Cannot write manifest: " << mpath << std::endl;
		}
	}

	return 0;
}
