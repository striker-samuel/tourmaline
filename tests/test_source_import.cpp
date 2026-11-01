// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier-Identifier: MIT

// source-import tests: the multi-format mesh-import lane
// (LoadModelFromSourceFile + ImportMeshSource) — the format adapters
// behind the shared gate, GPU-free (offline-mode derivations into temp
// dirs). What this pins:
//
//   1. the format gate itself (source_formats.h): .gltf/.glb/.obj are
//      mesh sources, everything else is not; the meta sidecar of every
//      source format is the ledger sidecar.
//   2. GLB loads (the old loader routed GLB through the ASCII JSON
//      parse — every binary .glb failed; the fix sniffs the magic).
//   3. EMBEDDED textures work: GLB bufferView images and glTF data:
//      URIs EXTRACT to <stem>_<srcext>_img<N>.<ext> sidecar files that
//      the material artifact references, the dependency enumeration
//      stages, and the digest covers (idempotently — a re-import of an
//      unchanged source is NOT stale).
//   4. OBJ imports end-to-end: geometry derives, the .material assets
//      carry the MTL map refs, the .mtl is a DIGEST dependency (an
//      .mtl edit re-derives, the family identity carries), and a
//      MISSING material library fails loudly.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <gltf_import/asset_export.h>
#include <gltf_import/gltf.h>
#include <gltf_import/source_formats.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	std::cout << (cond ? "PASS " : "FAIL ") << name << "\n";
	if (!cond)
		++g_failures;
}

namespace fs = std::filesystem;

// ---- scratch content builders -------------------------------------------

void WriteBytes(const fs::path& p, const void* data, size_t n) {
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f.write(static_cast<const char*>(data), std::streamsize(n));
}

void WriteText(const fs::path& p, const std::string& text) {
	WriteBytes(p, text.data(), text.size());
}

// A small valid PNG (4x4 RGBA), generated in-process through the
// vendored stb_image_write — the import's KTX transcode must accept it.
std::vector<uint8_t> MakePng() {
	unsigned char px[4 * 4 * 4];
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 4; ++x) {
			unsigned char* q = px + (size_t(y) * 4 + x) * 4;
			q[0] = static_cast<unsigned char>(40 + x * 50);
			q[1] = static_cast<unsigned char>(40 + y * 50);
			q[2] = 200;
			q[3] = 255;
		}
	int len = 0;
	unsigned char* enc = stbi_write_png_to_mem(px, 4 * 4, 4, 4, 4, &len);
	if (!enc)
		return {};
	std::vector<uint8_t> out(enc, enc + len);
	STBIW_FREE(enc);
	return out;
}

std::string Base64(const std::vector<uint8_t>& bytes) {
	static const char* tab =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	for (size_t i = 0; i < bytes.size(); i += 3) {
		const uint32_t b0 = bytes[i];
		const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
		const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
		const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
		out += tab[(v >> 18) & 63];
		out += tab[(v >> 12) & 63];
		out += (i + 1 < bytes.size()) ? tab[(v >> 6) & 63] : '=';
		out += (i + 2 < bytes.size()) ? tab[v & 63] : '=';
	}
	return out;
}

// The one-triangle BIN payload shared by the glTF/GLB builders:
// positions (3 x float3) then normals (3 x float3) then indices
// (3 x uint16). 78 bytes; the trailing PNG (when present) lands at the
// 4-aligned offset 76 — indices take 6 bytes, 72..78, so the image
// bufferView starts at 80 to keep every offset 4-aligned.
struct TriBin {
	std::vector<uint8_t> bytes;
	size_t imageOffset = 0;

	TriBin(const std::vector<uint8_t>* png) {
		const float pos[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
		const float nrm[9] = { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
		const uint16_t idx[3] = { 0, 1, 2 };
		auto put = [&](const void* p, size_t n) {
			const uint8_t* b = static_cast<const uint8_t*>(p);
			bytes.insert(bytes.end(), b, b + n);
		};
		put(pos, sizeof(pos));
		put(nrm, sizeof(nrm));
		put(idx, sizeof(idx));
		while (bytes.size() % 4)
			bytes.push_back(0);
		imageOffset = bytes.size();
		if (png) {
			bytes.insert(bytes.end(), png->begin(), png->end());
			while (bytes.size() % 4)
				bytes.push_back(0);
		}
	}
};

std::string TriJson(const TriBin& bin, const std::vector<uint8_t>& png) {
	std::ostringstream j;
	j << "{\"asset\":{\"version\":\"2.0\"},"
		"\"scene\":0,"
		"\"scenes\":[{\"nodes\":[0]}],"
		"\"nodes\":[{\"name\":\"Tri\",\"mesh\":0}],"
		"\"meshes\":[{\"primitives\":[{\"attributes\":"
		"{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"material\":0}]}],"
		"\"materials\":[{\"pbrMetallicRoughness\":"
		"{\"baseColorTexture\":{\"index\":0}}}],"
		"\"textures\":[{\"source\":0}],"
		"\"images\":[{\"bufferView\":3,\"mimeType\":\"image/png\"}],"
		"\"accessors\":["
		"{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
		"\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
		"{\"bufferView\":1,\"componentType\":5126,\"count\":3,"
		"\"type\":\"VEC3\",\"min\":[0,0,1],\"max\":[0,0,1]},"
		"{\"bufferView\":2,\"componentType\":5123,\"count\":3,"
		"\"type\":\"SCALAR\",\"min\":[0],\"max\":[2]}],"
		"\"bufferViews\":["
		"{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
		"{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
		"{\"buffer\":0,\"byteOffset\":72,\"byteLength\":6},"
		"{\"buffer\":0,\"byteOffset\":" << bin.imageOffset
		<< ",\"byteLength\":" << png.size() << "}],"
		"\"buffers\":[{\"byteLength\":" << bin.bytes.size() << "}]}";
	return j.str();
}

// A minimal binary GLB: 12-byte header + JSON chunk (space-padded) +
// BIN chunk (zero-padded).
void WriteGlb(const fs::path& p, const std::string& json,
	const std::vector<uint8_t>& bin) {
	auto push32 = [](std::vector<uint8_t>& v, uint32_t x) {
		for (int i = 0; i < 4; ++i)
			v.push_back(uint8_t(x >> (8 * i)));
	};
	auto chunk = [&](uint32_t type, const void* data, size_t n,
		uint8_t pad) {
		const size_t padded = (n + 3) & ~size_t(3);
		std::vector<uint8_t> c;
		push32(c, uint32_t(padded));
		push32(c, type);
		const uint8_t* b = static_cast<const uint8_t*>(data);
		c.insert(c.end(), b, b + n);
		c.resize(c.size() + (padded - n), pad);
		return c;
	};
	std::vector<uint8_t> out;
	out.insert(out.end(), { 'g', 'l', 'T', 'F' });
	push32(out, 2); // version
	const std::vector<uint8_t> jsonChunk =
		chunk(0x4E4F534A, json.data(), json.size(), 0x20);
	const std::vector<uint8_t> binChunk =
		chunk(0x004E4942, bin.data(), bin.size(), 0);
	const uint32_t total =
		uint32_t(12 + jsonChunk.size() + binChunk.size());
	push32(out, total);
	out.insert(out.end(), jsonChunk.begin(), jsonChunk.end());
	out.insert(out.end(), binChunk.begin(), binChunk.end());
	WriteBytes(p, out.data(), out.size());
}

// ---- 1) the shared format gate ------------------------------------------

void TestFormatGate() {
	using smvk::assets::IsMeshSourceExtension;
	using smvk::assets::IsMeshSourceFile;
	using smvk::assets::IsMeshSourceMetaSidecar;
	Check(IsMeshSourceExtension(".gltf") && IsMeshSourceExtension(".glb")
			&& IsMeshSourceExtension(".obj"), "formats.gate.sources");
	Check(IsMeshSourceExtension(".GLB") && IsMeshSourceExtension(".Obj"),
		"formats.gate.case-insensitive");
	Check(!IsMeshSourceExtension(".fbx") && !IsMeshSourceExtension(".dae")
			&& !IsMeshSourceExtension(".usd") && !IsMeshSourceExtension("")
			&& !IsMeshSourceExtension(".geometry")
			&& !IsMeshSourceExtension(".tour"),
		"formats.gate.non-sources");
	Check(IsMeshSourceFile(fs::path("content/models/a/bistro.GLB"))
			&& !IsMeshSourceFile(fs::path("x.png")),
		"formats.gate.path");
	Check(IsMeshSourceMetaSidecar("knight.gltf.meta")
			&& IsMeshSourceMetaSidecar("bistro.glb.meta")
			&& IsMeshSourceMetaSidecar("statue.obj.meta"),
		"formats.gate.meta-sidecars");
	Check(!IsMeshSourceMetaSidecar("knight.gltf")
			&& !IsMeshSourceMetaSidecar("brick.texture.meta")
			&& !IsMeshSourceMetaSidecar("foo.meta")
			&& !IsMeshSourceMetaSidecar("meta"),
		"formats.gate.meta-sidecars-reject");
}

// ---- 2) GLB (binary) ------------------------------------------------------

void TestGlbBinary() {
	std::error_code ec;
	const fs::path dir = fs::temp_directory_path(ec)
		/ "tourmaline_srcimport_glbbinary";
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const fs::path glb = dir / "tri.glb";
	{
		const TriBin bin(nullptr);
		// A textureless variant of TriJson: same builder minus the
		// material/texture/image rows.
		std::ostringstream j;
		j << "{\"asset\":{\"version\":\"2.0\"},"
			"\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
			"\"nodes\":[{\"name\":\"Tri\",\"mesh\":0}],"
			"\"meshes\":[{\"primitives\":[{\"attributes\":"
			"{\"POSITION\":0,\"NORMAL\":1},\"indices\":2}]}],"
			"\"accessors\":["
			"{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
			"\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
			"{\"bufferView\":1,\"componentType\":5126,\"count\":3,"
			"\"type\":\"VEC3\",\"min\":[0,0,1],\"max\":[0,0,1]},"
			"{\"bufferView\":2,\"componentType\":5123,\"count\":3,"
			"\"type\":\"SCALAR\",\"min\":[0],\"max\":[2]}],"
			"\"bufferViews\":["
			"{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
			"{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
			"{\"buffer\":0,\"byteOffset\":72,\"byteLength\":6}],"
			"\"buffers\":[{\"byteLength\":" << bin.bytes.size()
			<< "}]}";
		WriteGlb(glb, j.str(), bin.bytes);
	}
	// THE BUG this pins: the old single ASCII load path failed every
	// binary GLB outright.
	tinygltf::Model model;
	Check(LoadModelFromSourceFile(model, glb.string().c_str()),
		"import.glb-binary.parse");
	Check(model.meshes.size() == 1 && model.buffers.size() == 1,
		"import.glb-binary.model-shape");

	smvk::assets::AssetManifest man;
	std::string err;
	smvk::assets::DeriveConfig cfg{};
	smvk::assets::DeriveStats stats;
	Check(smvk::assets::ImportMeshSource(glb, "", nullptr, cfg, man,
			&stats, &err),
		"import.glb-binary.derive");
	Check(err.empty(), "import.glb-binary.no-err");
	smvk::assets::MeshAsset asset;
	std::string aerr;
	Check(smvk::assets::LoadMeshAsset(
			fs::path(glb).replace_extension(".geometry"), asset, aerr),
		"import.glb-binary.geometry-parses");
	Check(asset.geometry.size() == 1
			&& asset.geometry[0].indices.size() == 3,
		"import.glb-binary.geometry-triangle");
	Check(!smvk::assets::MeshArtifactStale(glb, cfg),
		"import.glb-binary.not-stale");
	// A source edit flips the content digest.
	{
		std::ofstream f(glb, std::ios::binary | std::ios::app);
		f.put(0);
	}
	Check(smvk::assets::MeshArtifactStale(glb, cfg),
		"import.glb-binary.stale-after-edit");
	fs::remove_all(dir, ec);
}

// ---- 3) embedded textures (GLB bufferView + glTF data: URI) --------------

// The shared body: import `source` (which must reference ONE embedded
// PNG image through its baseColorTexture), then verify the extraction
// contract — sidecar file present, the material artifact references
// it, the dependency enumeration stages it, re-import is idempotent.
void TestEmbeddedTexture(const fs::path& source, const char* stemTag) {
	std::error_code ec;
	smvk::assets::DeriveConfig cfg{};
	smvk::assets::AssetManifest man;
	std::string err;
	smvk::assets::DeriveStats stats;
	Check(smvk::assets::ImportMeshSource(source, "", nullptr, cfg, man,
			&stats, &err),
		(std::string("import.") + stemTag + ".derive").c_str());

	const std::string sidecarName =
		std::string("packed_") + stemTag + "_img0.png";
	const fs::path sidecar = source.parent_path() / sidecarName;
	Check(fs::is_regular_file(sidecar, ec),
		(std::string("import.") + stemTag + ".sidecar-extracted").c_str());

	// The material artifact binds the EXTRACTED sidecar as its
	// baseColor source (empty path = the old silent drop this fixes).
	std::string matFile;
	for (const auto& e : man.entries)
		if (e.kind == smvk::assets::AssetKind::Material) {
			matFile = e.file;
			break;
		}
	smvk::assets::MaterialAsset mat;
	std::string merr;
	Check(!matFile.empty()
			&& smvk::assets::LoadMaterialAsset(
				source.parent_path() / matFile, mat, merr),
		(std::string("import.") + stemTag + ".material-parses").c_str());
	Check(mat.baseColor.path == sidecarName,
		(std::string("import.") + stemTag
			+ ".material-references-extracted").c_str());

	// The project-import dependency enumeration carries the sidecar
	// (it stages exactly this set beside the project copy).
	std::vector<std::string> deps =
		smvk::assets::GltfExternalDependencies(source);
	Check(std::find(deps.begin(), deps.end(), sidecarName) != deps.end(),
		(std::string("import.") + stemTag + ".dependency-staged").c_str());

	// Idempotence: a re-import of the unchanged source keeps the family
	// identity and is NOT stale (write-if-changed + digest stability).
	smvk::assets::AssetManifest man2;
	std::string err2;
	Check(smvk::assets::ImportMeshSource(source, "", nullptr, cfg, man2,
			nullptr, &err2),
		(std::string("import.") + stemTag + ".reimport").c_str());
	Check(man2.guid == man.guid,
		(std::string("import.") + stemTag + ".identity-carries").c_str());
	Check(!smvk::assets::MeshArtifactStale(source, cfg),
		(std::string("import.") + stemTag + ".reimport-not-stale").c_str());
}

void TestGlbEmbeddedTexture() {
	std::error_code ec;
	const fs::path dir = fs::temp_directory_path(ec)
		/ "tourmaline_srcimport_glbtext";
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const std::vector<uint8_t> png = MakePng();
	Check(!png.empty(), "import.glb-texture.png-generated");
	const fs::path glb = dir / "packed.glb";
	{
		const TriBin bin(&png);
		WriteGlb(glb, TriJson(bin, png), bin.bytes);
	}
	TestEmbeddedTexture(glb, "glb");

	// Same payload as a data: URI in an ASCII .gltf (the base64
	// embedded-texture lane of the same fix).
	const fs::path gltf = dir / "packed.gltf";
	{
		const TriBin bin(nullptr);
		std::ostringstream j;
		j << "{\"asset\":{\"version\":\"2.0\"},"
			"\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
			"\"nodes\":[{\"name\":\"Tri\",\"mesh\":0}],"
			"\"meshes\":[{\"primitives\":[{\"attributes\":"
			"{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"material\":0}]}],"
			"\"materials\":[{\"pbrMetallicRoughness\":"
			"{\"baseColorTexture\":{\"index\":0}}}],"
			"\"textures\":[{\"source\":0}],"
			"\"images\":[{\"uri\":\"data:image/png;base64,"
			<< Base64(png) << "\"}],"
			"\"accessors\":["
			"{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
			"\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
			"{\"bufferView\":1,\"componentType\":5126,\"count\":3,"
			"\"type\":\"VEC3\",\"min\":[0,0,1],\"max\":[0,0,1]},"
			"{\"bufferView\":2,\"componentType\":5123,\"count\":3,"
			"\"type\":\"SCALAR\",\"min\":[0],\"max\":[2]}],"
			"\"bufferViews\":["
			"{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
			"{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
			"{\"buffer\":0,\"byteOffset\":72,\"byteLength\":6}],"
			"\"buffers\":[{\"uri\":\"data:application/octet-stream;"
			"base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"
			"AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA\","
			"\"byteLength\":78}]}";
		WriteText(gltf, j.str());
	}
	TestEmbeddedTexture(gltf, "gltf");
	fs::remove_all(dir, ec);
}

// ---- 4) OBJ ---------------------------------------------------------------

void WriteObjFamily(const fs::path& dir) {
	std::error_code ec;
	fs::create_directories(dir, ec);
	// Two shapes, two materials, one map_Kd onto a real PNG (the
	// import's KTX transcode must accept it — a broken ref fails the
	// import loudly, which is exactly what makes this a real test).
	const std::vector<uint8_t> png = MakePng();
	WriteBytes(dir / "check.png", png.data(), png.size());
	WriteText(dir / "shapes.mtl",
		"newmtl Red\n"
		"Kd 1 0 0\n"
		"map_Kd check.png\n"
		"newmtl Green\n"
		"Kd 0 1 0\n"
		"Ke 0.25 0 0\n");
	WriteText(dir / "shapes.obj",
		"mtllib shapes.mtl\n"
		"o TriA\n"
		"v 0 0 0\n"
		"v 1 0 0\n"
		"v 0 1 0\n"
		"vn 0 0 1\n"
		"vt 0 0\n"
		"vt 1 0\n"
		"vt 0 1\n"
		"usemtl Red\n"
		"f 1/1/1 2/2/1 3/3/1\n"
		"o TriB\n"
		"v 2 0 0\n"
		"v 3 0 0\n"
		"v 2 1 0\n"
		"vn 0 0 1\n"
		"vt 0 0\n"
		"vt 1 0\n"
		"vt 0 1\n"
		"usemtl Green\n"
		"f 4/4/2 5/5/2 6/6/2\n");
}

void TestObj() {
	std::error_code ec;
	const fs::path dir = fs::temp_directory_path(ec)
		/ "tourmaline_srcimport_obj";
	fs::remove_all(dir, ec);
	WriteObjFamily(dir);
	const fs::path obj = dir / "shapes.obj";
	smvk::assets::DeriveConfig cfg{};

	// The dependency enumeration: the .mtl + the map_Kd image (the
	// project import stages exactly this set beside the source).
	{
		const std::vector<std::string> deps =
			smvk::assets::GltfExternalDependencies(obj);
		Check(std::find(deps.begin(), deps.end(), "shapes.mtl")
				!= deps.end()
				&& std::find(deps.begin(), deps.end(), "check.png")
					!= deps.end(),
			"import.obj.dependencies-mtl-and-map");
	}

	smvk::assets::AssetManifest man;
	std::string err;
	smvk::assets::DeriveStats stats;
	Check(smvk::assets::ImportMeshSource(obj, "", nullptr, cfg, man,
			&stats, &err),
		"import.obj.derive");
	Check(stats.materials == 2, "import.obj.material-count");

	// Geometry: one blob per shape, the shape names survive.
	smvk::assets::MeshAsset asset;
	std::string aerr;
	Check(smvk::assets::LoadMeshAsset(
			fs::path(obj).replace_extension(".geometry"), asset, aerr),
		"import.obj.geometry-parses");
	Check(asset.geometry.size() == 2, "import.obj.geometry-two-shapes");
	Check(std::find(asset.names.begin(), asset.names.end(), "TriA")
			!= asset.names.end()
		&& std::find(asset.names.begin(), asset.names.end(), "TriB")
			!= asset.names.end(),
		"import.obj.shape-names-survive");

	// The Red material binds the MTL map (POSIX-relative, beside the
	// .obj — the HostTextureSource lane).
	bool redBindsMap = false;
	for (const auto& e : man.entries)
		if (e.kind == smvk::assets::AssetKind::Material) {
			smvk::assets::MaterialAsset m;
			std::string merr;
			if (smvk::assets::LoadMaterialAsset(
					dir / e.file, m, merr) && m.name == "Red")
				redBindsMap = m.baseColor.path == "check.png";
		}
	Check(redBindsMap, "import.obj.material-binds-map");

	Check(!smvk::assets::MeshArtifactStale(obj, cfg),
		"import.obj.not-stale");

	// An .mtl EDIT re-derives (the .mtl rides the content digest) and
	// the family identity CARRIES (freshness, not identity).
	{
		std::ofstream f(dir / "shapes.mtl", std::ios::binary | std::ios::app);
		f << "\n# edited\n";
	}
	Check(smvk::assets::MeshArtifactStale(obj, cfg),
		"import.obj.mtl-edit-rederives");
	smvk::assets::AssetManifest man2;
	std::string err2;
	Check(smvk::assets::ImportMeshSource(obj, "", nullptr, cfg, man2,
			nullptr, &err2),
		"import.obj.reimport-after-mtl-edit");
	Check(man2.guid == man.guid, "import.obj.identity-carries");
	fs::remove_all(dir, ec);
}

void TestObjMissingMtl() {
	std::error_code ec;
	const fs::path dir = fs::temp_directory_path(ec)
		/ "tourmaline_srcimport_objmissing";
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	WriteText(dir / "broken.obj",
		"mtllib absent.mtl\n"
		"v 0 0 0\nv 1 0 0\nv 0 1 0\n"
		"f 1 2 3\n");
	smvk::assets::AssetManifest man;
	std::string err;
	smvk::assets::DeriveConfig cfg{};
	Check(!smvk::assets::ImportMeshSource(dir / "broken.obj", "",
			nullptr, cfg, man, nullptr, &err),
		"import.obj.missing-mtl-fails");
	Check(err.find("parse failed") != std::string::npos,
		"import.obj.missing-mtl-err-names-parse");
	fs::remove_all(dir, ec);
}

} // namespace

int main() {
	TestFormatGate();
	TestGlbBinary();
	TestGlbEmbeddedTexture();
	TestObj();
	TestObjMissingMtl();
	std::cout << "checks=" << g_checks
		<< " failures=" << g_failures << std::endl;
	return g_failures == 0 ? 0 : 1;
}
