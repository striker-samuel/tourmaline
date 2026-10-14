// import.exe - THE raw-3d -> engine-format converter (the asset
// creation). POSITIONAL grammar:
//
//   import.exe <input.glb|gltf> [--meshlets on|off] [--lods on|off]
//       [--shader-lods on|off] [--force] [--help]
//
// Parse + derive in ONE call (the MeshImporter: gltf_import's
// ImportMeshSource - the SAME import the editor's open/drop runs):
// the Tourmaline artifact family lands next to the source -
//   <stem>.geometry            the scene blob (hierarchy + SoA streams)
//   <stem>.mat<idx>_<name>.material (one per glTF material)
//   <stem>.anim<idx>_<name>.anim   (one per animation)
//   <stem>.gltf.meta            the manifest (stable GUIDs)
// Identity is DETERMINISTIC (SourceAssetGuid): a from-zero re-import
// reproduces byte-identical artifacts.
//
// The DERIVE settings mirror the editor's env pins
// (VULKANTOY_EDITOR_MESHLETS/LODS/SHADERLODS - see DeriveConfig).
// Default: derive-if-stale (MeshArtifactStale: missing artifacts or a
// changed source re-derives; a fresh import is a no-op when current).
// --force always re-derives.
//
// The SECOND half of the content pipeline is pak.exe (tools/pak):
// it consumes the ALREADY-PROCESSED artifacts + a VFS root and cooks
// the pak (cubemap bake, LOD/meshlet cook, serializer). Raw glTF
// NEVER cooks - this is the clean split: ONE raw-import place (this
// tool / the editor's MeshImporter), ONE artifact-consumer place.

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include <gltf_import/asset_export.h>

namespace {

bool ParseOnOff(const std::string& v) {
	return v == "on" || v == "1" || v == "true";
}

void Usage() {
	std::cout
		<< "import.exe <input.glb|gltf> [--meshlets on|off] [--lods on|off]\n"
		<< "    [--shader-lods on|off] [--force]\n"
		<< "  (the raw-3d -> engine-format converter: derives the .geometry\n"
		<< "   + .material/.anim + .gltf.meta artifact family next to the\n"
		<< "   source - the SAME import the editor's open/drop runs;\n"
		<< "   deterministic GUIDs, byte-identical from-zero re-imports.\n"
		<< "   Default derive-if-stale; --force always re-derives. Cook\n"
		<< "   the artifacts into a pak with pak.exe.)\n";
}

} // namespace

int main(int argc, char** argv) {
	std::string input;
	bool force = false;
	smvk::assets::DeriveConfig config{};
	bool seenFlag = false;
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
		const bool isFlag = a.size() >= 2 && a[0] == '-' && a[1] == '-';
		if (!isFlag) {
			if (seenFlag || !input.empty()) {
				std::cerr << "Multiple inputs / artifact after flags: '"
					<< a << "' (import.exe takes ONE input - the settings"
					" are flags)" << std::endl;
				return 1;
			}
			input = a;
			continue;
		}
		seenFlag = true;
		if (a == "--meshlets") {
			std::string v;
			if (!need(v)) return 1;
			config.meshlets = ParseOnOff(v);
		}
		else if (a == "--lods") {
			std::string v;
			if (!need(v)) return 1;
			config.lods = ParseOnOff(v);
		}
		else if (a == "--shader-lods") {
			std::string v;
			if (!need(v)) return 1;
			config.shaderLods = ParseOnOff(v);
		}
		else if (a == "--force") {
			force = true;
		}
		else if (a == "--help" || a == "-h") {
			Usage();
			return 0;
		}
		else {
			std::cerr << "Unknown arg: " << a << std::endl;
			Usage();
			return 1;
		}
	}
	if (input.empty()) {
		std::cerr << "Required: import.exe <input.glb|gltf>" << std::endl;
		Usage();
		return 1;
	}

	const std::filesystem::path inPath(input);
	if (!std::filesystem::exists(inPath)) {
		std::cerr << "Input not found: " << input << std::endl;
		return 1;
	}

	// Default: derive-if-stale (the editor's open/drop parity - a fresh
	// import is a no-op when the artifacts are current; --force always
	// re-derives).
	if (!force && !smvk::assets::MeshArtifactStale(inPath)) {
		std::cout << "Artifacts current (not stale): '" << input
			<< "' - nothing to do (--force always re-derives)\n";
		return 0;
	}

	smvk::assets::AssetManifest manifest;
	smvk::assets::DeriveStats stats;
	std::string err;
	// gltfGuid "" = the from-zero source identity (ImportMeshSource
	// derives it from the SOURCE FILE CONTENT - deterministic; the
	// editor passes "" by design so project sessions agree with
	// from-zero re-imports).
	if (!smvk::assets::ImportMeshSource(inPath, "", config, manifest,
			&stats, &err)) {
		std::cerr << "Import failed: " << (err.empty()
			? "unknown" : err) << std::endl;
		return 1;
	}
	std::cout << "Imported: " << input << "\n"
		<< "  materials=" << stats.materials
		<< " anims=" << stats.anims
		<< " geometryBlobs=" << stats.geometryBlobs
		<< " instances=" << stats.instances
		<< " lights=" << stats.lights
		<< " cameras=" << stats.cameras
		<< " nodes=" << (stats.instances + stats.lights + stats.cameras)
		<< "\n"
		<< "  (deterministic GUIDs: " << stats.newGuids
		<< " new; a from-zero re-import is byte-identical)\n"
		<< "  cook with: pak.exe <vfs_path> <stem>.geometry [--options]\n";
	return 0;
}
