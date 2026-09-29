// glTF -> project asset derivation (see asset_export.h): the writers +
// readers for .mesh / .material / .anim + the gltf.meta manifest.
//
// The derivation runs the SAME deterministic GPU-free cook-mode import the
// baker/cooker use (GltfToCluster on a null-RM Cluster), so node ids are
// identical to a live import Ã¢â‚¬â€ scenes built from the assets address the
// same nodes a .tour replay expects. The .mesh stores the RAW TRS (the
// coordFix basis applies at the scene root at load, the same contract as
// the live import) + the packed SoA GPU heap streams (the exact layout the
// pak cooker stores Ã¢â‚¬â€ direct upload, no per-vertex work at load).

#include <gltf_import/asset_export.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

#include <glm/gtc/type_ptr.hpp>

#include <gltf_import/gltf.h>
#include <Engine/Asset/tour.h>
#include <Engine/Asset/touranim.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Asset/mesh_optimizer.h>

namespace tinygltf { class Model; }
namespace smvk::assets {

namespace {

// ---------------------------------------------------------------------------
// Binary put/get (the .mesh format)
// ---------------------------------------------------------------------------

void PutU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	b.insert(b.end(), p, p + 4);
}
void PutI32(std::vector<uint8_t>& b, int32_t v) {
	PutU32(b, static_cast<uint32_t>(v));
}
void PutF32(std::vector<uint8_t>& b, float v) {
	uint32_t u;
	std::memcpy(&u, &v, 4);
	PutU32(b, u);
}
void PutVec3(std::vector<uint8_t>& b, const glm::vec3& v) {
	PutF32(b, v.x);
	PutF32(b, v.y);
	PutF32(b, v.z);
}
void PutMat4(std::vector<uint8_t>& b, const glm::mat4& m) {
	for (int i = 0; i < 16; ++i)
		PutF32(b, glm::value_ptr(m)[i]);
}
void PutStr(std::vector<uint8_t>& b, const std::string& s) {
	PutU32(b, uint32_t(s.size()));
	b.insert(b.end(), s.begin(), s.end());
}
void PutBytes(std::vector<uint8_t>& b, const void* p, size_t n) {
	const uint8_t* q = reinterpret_cast<const uint8_t*>(p);
	b.insert(b.end(), q, q + n);
}

struct BlobReader {
	const uint8_t* p = nullptr;
	size_t n = 0;
	size_t off = 0;
	bool ok = true;

	uint32_t U32() {
		if (!ok || off + 4 > n) { ok = false; return 0; }
		uint32_t v;
		std::memcpy(&v, p + off, 4);
		off += 4;
		return v;
	}
	uint8_t U8() {
		if (!ok || off + 1 > n) { ok = false; return 0; }
		return p[off++];
	}
	float F32() {
		const uint32_t u = U32();
		float v;
		std::memcpy(&v, &u, 4);
		return v;
	}
	glm::vec3 Vec3() { return glm::vec3(F32(), F32(), F32()); }
	glm::mat4 Mat4() {
		glm::mat4 m;
		for (int i = 0; i < 16; ++i)
			glm::value_ptr(m)[i] = F32();
		return m;
	}
	std::string Str() {
		const uint32_t len = U32();
		if (!ok || off + len > n) { ok = false; return {}; }
		std::string s(reinterpret_cast<const char*>(p + off), len);
		off += len;
		return s;
	}
	const void* Skip(size_t bytes) {
		if (!ok || off + bytes > n) { ok = false; return nullptr; }
		const void* r = p + off;
		off += bytes;
		return r;
	}
};

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

bool WriteFileBytes(const std::filesystem::path& path,
	const std::vector<uint8_t>& bytes, std::string& err) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "cannot write '" + path.generic_string() + "'";
		return false;
	}
	out.write(reinterpret_cast<const char*>(bytes.data()),
		std::streamsize(bytes.size()));
	if (!out.good()) {
		err = "write failed '" + path.generic_string() + "'";
		return false;
	}
	return true;
}

bool ReadFileBytes(const std::filesystem::path& path,
	std::vector<uint8_t>& out, std::string& err) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		err = "cannot open '" + path.generic_string() + "'";
		return false;
	}
	out.assign((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	return true;
}

// File-name-safe asset label (lower/upper/digits/_/-; the rest -> '_').
std::string SanitizeLabel(std::string s) {
	for (char& c : s)
		if (!std::isalnum(unsigned char(c)) && c != '_' && c != '-')
			c = '_';
	return s;
}

std::string MaterialFileName(const std::string& stem, int index,
	const std::string& name) {
	const std::string label =
		SanitizeLabel(name.empty() ? ("mat" + std::to_string(index)) : name);
	return stem + ".mat" + std::to_string(index) + "_" + label + ".material";
}

std::string AnimFileName(const std::string& stem, int index,
	const std::string& name) {
	const std::string label =
		SanitizeLabel(name.empty() ? ("anim" + std::to_string(index)) : name);
	return stem + ".anim" + std::to_string(index) + "_" + label + ".anim";
}

// Raw FLOAT element pointer of an accessor (null on a non-float/short
// read; animation samplers are float-or-double scalars/vec3s/vec4s —
// glTF animation accessors are componentType FLOAT by spec, but be
// tolerant of doubles).
template <typename T>
const T* AnimAccessorPtr(const tinygltf::Model& m,
	const tinygltf::Accessor& a, size_t i) {
	if (a.bufferView < 0 || a.bufferView >= int(m.bufferViews.size()))
		return nullptr;
	if (i >= size_t(a.count))
		return nullptr;
	const tinygltf::BufferView& bv = m.bufferViews[size_t(a.bufferView)];
	if (bv.buffer < 0 || bv.buffer >= int(m.buffers.size()))
		return nullptr;
	const tinygltf::Buffer& buf = m.buffers[size_t(bv.buffer)];
	const size_t stride = (a.ByteStride(bv) != 0)
		? size_t(a.ByteStride(bv)) : sizeof(T) * size_t(a.type);
	const size_t off = size_t(bv.byteOffset) + size_t(a.byteOffset)
		+ i * stride;
	if (off + sizeof(T) > buf.data.size())
		return nullptr;
	return reinterpret_cast<const T*>(buf.data.data() + off);
}

// .anim writer (the .tourAnim ASCII grammar — see touranim.h; the
// engine's SaveTourAnim is not implemented yet, so the derivation
// emits the documented format directly. Kept minimal: clip/track/key
// verbs, no comment decoration).
void WriteTourAnimAsset(const std::filesystem::path& path,
	const smvk::touranim::AnimDoc& doc, std::string& err) {
	std::ostringstream o;
	o << "// .tourAnim " << smvk::touranim::kTourAnimVersion
		<< " — " << (doc.source.empty() ? "derived" : doc.source) << "\n";
	o << "source -p \"" << doc.source << "\";\n";
	for (const smvk::touranim::Clip& clip : doc.clips) {
		o << "clip -name \"" << clip.name << "\" -duration "
			<< clip.duration << " -fps " << clip.fps << ";\n";
		for (const smvk::touranim::Track& t : clip.tracks) {
			const char* attr = (t.channel
				== smvk::touranim::Channel::Rotate) ? "rotate"
				: (t.channel == smvk::touranim::Channel::Scale)
				? "scale" : "translate";
			const char* interp = (t.spline
				== smvk::touranim::Spline::Step) ? "step"
				: (t.spline == smvk::touranim::Spline::CatmullRom)
				? "catmullrom" : "linear";
			o << "track -joint " << t.joint;
			if (!t.name.empty())
				o << " -name \"" << t.name << "\"";
			o << " -attr " << attr << " -interp " << interp << ";\n";
			for (const smvk::touranim::Key& k : t.keys)
				o << "  " << k.t << "  " << k.v.x << " " << k.v.y
					<< " " << k.v.z
					<< ((t.channel == smvk::touranim::Channel::Rotate)
						? std::string(" ") + std::to_string(k.v.w)
						: std::string()) << ";\n";
		}
	}
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "cannot write '" + path.generic_string() + "'";
		return;
	}
	out << o.str();
}

// ---------------------------------------------------------------------------
// gltf.meta manifest (ASCII, stable-GUID registry)
// ---------------------------------------------------------------------------

// Key=value lines; sections by kind:
//   guid=<32hex>            the glTF asset GUID
//   source=<file name>      the glTF (display/reimport)
//   mesh=<file> <guid> <name>
//   material.<idx>=<file> <guid> <name>
//   anim.<idx>=<file> <guid> <name>
bool ParseManifestLine(const std::string& key, const std::string& val,
	AssetManifest& m) {
	if (key == "guid") { m.guid = val; return true; }
	if (key == "source") { m.source = val; return true; }
	if (key == "stem") { m.stem = val; return true; }
	AssetEntry e{};
	bool have = false;
	if (key == "mesh") {
		e.kind = AssetKind::Mesh;
		have = true;
	}
	else if (key.rfind("material.", 0) == 0) {
		e.kind = AssetKind::Material;
		e.sourceIndex = std::atoi(key.c_str() + 9);
		have = true;
	}
	else if (key.rfind("anim.", 0) == 0) {
		e.kind = AssetKind::Anim;
		e.sourceIndex = std::atoi(key.c_str() + 5);
		have = true;
	}
	if (!have)
		return false; // unknown key: caller warns
	std::istringstream is(val);
	std::string file, guid, name;
	is >> file >> guid;
	std::getline(is, name);
	// trim the name
	size_t b = name.find_first_not_of(" \t");
	size_t epos = name.find_last_not_of(" \t\r\n");
	name = (b == std::string::npos) ? std::string()
		: name.substr(b, epos - b + 1);
	e.file = file;
	e.guid = guid;
	e.name = name;
	m.entries.push_back(std::move(e));
	return true;
}

} // namespace

bool LoadAssetManifest(const std::filesystem::path& gltfPath,
	AssetManifest& out, std::string& err) {
	out = AssetManifest{};
	const std::filesystem::path metaPath =
		gltfPath.string() + ".meta";
	std::ifstream in(metaPath);
	if (!in) {
		err = "no manifest '" + metaPath.generic_string() + "'";
		return false;
	}
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		ParseManifestLine(line.substr(0, eq), line.substr(eq + 1), out);
	}
	out.stem = std::filesystem::path(gltfPath).stem().string();
	if (out.guid.empty()) {
		err = "manifest carries no guid";
		return false;
	}
	return true;
}

bool SaveAssetManifest(const std::filesystem::path& gltfPath,
	const AssetManifest& m, std::string& err) {
	std::vector<std::string> lines;
	lines.push_back("# Tourmaline asset manifest (the derived assets of one"
		" glTF; GUIDs are stable across reimports)");
	lines.push_back("guid=" + m.guid);
	lines.push_back("source=" + (m.source.empty()
		? std::filesystem::path(gltfPath).filename().generic_string()
		: m.source));
	lines.push_back("stem=" + m.stem);
	for (const AssetEntry& e : m.entries) {
		std::string key;
		switch (e.kind) {
		case AssetKind::Mesh: key = "mesh"; break;
		case AssetKind::Material:
			key = "material." + std::to_string(e.sourceIndex);
			break;
		case AssetKind::Anim:
			key = "anim." + std::to_string(e.sourceIndex);
			break;
		}
		lines.push_back(key + "=" + e.file + " " + e.guid + " " + e.name);
	}
	const std::filesystem::path metaPath = gltfPath.string() + ".meta";
	std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "cannot write '" + metaPath.generic_string() + "'";
		return false;
	}
	for (const std::string& l : lines)
		out << l << "\n";
	return out.good();
}

// ---------------------------------------------------------------------------
// .material (ASCII)
// ---------------------------------------------------------------------------

namespace {

void WriteMaterialAsset(const std::filesystem::path& path,
	const MaterialAsset& m, std::string& err) {
	std::ostringstream o;
	o << "# Tourmaline material asset v1\n";
	o << "guid=" << m.guid << "\n";
	o << "name=" << m.name << "\n";
	const glm::vec4& bc = m.info.baseColorFactor;
	o << "baseColor=" << bc.x << " " << bc.y << " " << bc.z << " "
		<< bc.w << "\n";
	o << "metallic=" << m.info.metallicFactor << "\n";
	o << "roughness=" << m.info.roughnessFactor << "\n";
	const glm::vec3& em = m.info.emissiveFactor;
	o << "emissive=" << em.x << " " << em.y << " " << em.z << "\n";
	o << "emissiveStrength=" << m.info.emissiveStrength << "\n";
	const char* alpha = (m.info.alphaMode == smvk::AlphaMode::Mask)
		? "mask" : (m.info.alphaMode == smvk::AlphaMode::Blend)
		? "blend" : "opaque";
	o << "alphaMode=" << alpha << "\n";
	o << "alphaCutoff=" << m.info.alphaCutoff << "\n";
	o << "normalScale=" << m.info.normalScale << "\n";
	o << "occlusionStrength=" << m.info.occlusionStrength << "\n";
	o << "doubleSided=" << (m.info.doubleSided ? 1 : 0) << "\n";
	o << "castsShadows=" << (m.info.castsShadows ? 1 : 0) << "\n";
	const char* shading = (m.info.shadingModel
		== smvk::ShadingModel::Unlit) ? "unlit"
		: (m.info.shadingModel == smvk::ShadingModel::Masked) ? "masked"
		: "lit";
	o << "shading=" << shading << "\n";
	auto tex = [&](const char* slot, const MaterialAsset::TexRef& t) {
		if (!t.path.empty())
			o << "tex." << slot << "=" << t.path
				<< (t.srgb ? " srgb" : "") << "\n";
	};
	tex("baseColor", m.baseColor);
	tex("metallicRoughness", m.metallicRoughness);
	tex("normal", m.normalTex);
	tex("occlusion", m.occlusion);
	tex("emissive", m.emissiveTex);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "cannot write '" + path.generic_string() + "'";
		return;
	}
	out << o.str();
}

bool SplitPathFlag(const std::string& val, std::string& path, bool& srgb) {
	const size_t sp = val.rfind(' ');
	if (sp != std::string::npos && val.compare(sp + 1, 4, "srgb") == 0) {
		path = val.substr(0, sp);
		srgb = true;
		return true;
	}
	path = val;
	srgb = false;
	return true;
}

} // namespace

bool LoadMaterialAsset(const std::filesystem::path& path,
	MaterialAsset& out, std::string& err) {
	out = MaterialAsset{};
	std::ifstream in(path);
	if (!in) {
		err = "cannot open '" + path.generic_string() + "'";
		return false;
	}
	std::string line;
	glm::vec4 bc(1.0f);
	glm::vec3 em(0.0f);
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = line.substr(0, eq);
		const std::string val = line.substr(eq + 1);
		if (key == "guid") out.guid = val;
		else if (key == "name") out.name = val;
		else if (key == "baseColor")
			std::sscanf(val.c_str(), "%f %f %f %f", &bc.x, &bc.y, &bc.z,
				&bc.w);
		else if (key == "metallic")
			out.info.metallicFactor = float(std::atof(val.c_str()));
		else if (key == "roughness")
			out.info.roughnessFactor = float(std::atof(val.c_str()));
		else if (key == "emissive")
			std::sscanf(val.c_str(), "%f %f %f", &em.x, &em.y, &em.z);
		else if (key == "emissiveStrength")
			out.info.emissiveStrength = float(std::atof(val.c_str()));
		else if (key == "alphaMode") {
			if (val == "mask") out.info.alphaMode = smvk::AlphaMode::Mask;
			else if (val == "blend")
				out.info.alphaMode = smvk::AlphaMode::Blend;
			else out.info.alphaMode = smvk::AlphaMode::Opaque;
		}
		else if (key == "alphaCutoff")
			out.info.alphaCutoff = float(std::atof(val.c_str()));
		else if (key == "normalScale")
			out.info.normalScale = float(std::atof(val.c_str()));
		else if (key == "occlusionStrength")
			out.info.occlusionStrength = float(std::atof(val.c_str()));
		else if (key == "doubleSided")
			out.info.doubleSided = std::atoi(val.c_str()) != 0;
		else if (key == "castsShadows")
			out.info.castsShadows = std::atoi(val.c_str()) != 0;
		else if (key == "shading") {
			if (val == "unlit")
				out.info.shadingModel = smvk::ShadingModel::Unlit;
			else if (val == "masked")
				out.info.shadingModel = smvk::ShadingModel::Masked;
			else out.info.shadingModel = smvk::ShadingModel::DefaultLit;
		}
		else if (key == "tex.baseColor")
			SplitPathFlag(val, out.baseColor.path, out.baseColor.srgb);
		else if (key == "tex.metallicRoughness")
			SplitPathFlag(val, out.metallicRoughness.path,
				out.metallicRoughness.srgb);
		else if (key == "tex.normal")
			SplitPathFlag(val, out.normalTex.path, out.normalTex.srgb);
		else if (key == "tex.occlusion")
			SplitPathFlag(val, out.occlusion.path, out.occlusion.srgb);
		else if (key == "tex.emissive")
			SplitPathFlag(val, out.emissiveTex.path, out.emissiveTex.srgb);
	}
	out.info.baseColorFactor = bc;
	out.info.emissiveFactor = em;
	if (out.guid.empty()) {
		err = "material asset carries no guid: '" + path.generic_string()
			+ "'";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// .mesh (binary, MESH v1)
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kMeshMagic =
	('M' << 24) | ('E' << 16) | ('S' << 8) | 'H';
constexpr uint32_t kMeshVersion = 1;

void WriteMeshAsset(const std::filesystem::path& path, const MeshAsset& m,
	std::string& err) {
	std::vector<uint8_t> b;
	b.reserve(1 << 16);
	PutU32(b, kMeshMagic);
	PutU32(b, kMeshVersion);
	// Hierarchy (raw parents/levels; firstChild/next rebuilt at load).
	const uint32_t nodeCount = uint32_t(m.nodes.size());
	PutU32(b, nodeCount);
	for (const MeshAsset::Node& n : m.nodes) {
		PutI32(b, n.parent);
		PutI32(b, n.level);
	}
	PutU32(b, nodeCount);
	for (const std::string& s : m.names)
		PutStr(b, s);
	PutU32(b, nodeCount);
	for (const glm::mat4& t : m.locals)
		PutMat4(b, t);
	// Flags (staticRoot / noLightmap), nodeCount bytes each.
	PutU32(b, nodeCount);
	PutBytes(b, m.staticRoot.data(), m.staticRoot.size());
	PutU32(b, nodeCount);
	PutBytes(b, m.noLightmap.data(), m.noLightmap.size());
	// Geometry instances (node -> blob + material GUID).
	PutU32(b, uint32_t(m.instances.size()));
	for (const MeshAsset::Instance& i : m.instances) {
		PutU32(b, i.node);
		PutU32(b, i.geometry);
		PutStr(b, i.materialGuid);
	}
	// Lights + cameras (node-linked defs).
	PutU32(b, uint32_t(m.lights.size()));
	for (const MeshAsset::LightDef& l : m.lights) {
		PutU32(b, l.node);
		PutU32(b, uint32_t(l.def.type));
		PutVec3(b, l.def.color);
		PutF32(b, l.def.intensity);
		PutF32(b, l.def.range);
		PutF32(b, l.def.innerConeAngle);
		PutF32(b, l.def.outerConeAngle);
		PutU32(b, l.def.flags);
		PutStr(b, l.def.name);
	}
	PutU32(b, uint32_t(m.cameras.size()));
	for (const MeshAsset::CameraDef& c : m.cameras) {
		PutU32(b, c.node);
		PutU32(b, uint32_t(c.def.projection));
		PutF32(b, c.def.fovYDeg);
		PutF32(b, c.def.orthoHalfH);
		PutF32(b, c.def.nearClip);
		PutF32(b, c.def.farClip);
		PutStr(b, c.def.name);
	}
	// Geometry blobs: packed SoA streams (the GPU heap layout).
	PutU32(b, uint32_t(m.geometry.size()));
	for (const MeshAsset::Geometry& g : m.geometry) {
		PutU32(b, uint32_t(g.positions.size()));
		PutU32(b, uint32_t(g.indices.size()));
		PutU32(b, uint32_t(g.surfaces.size()));
		for (const smvk::Surface& s : g.surfaces) {
			PutU32(b, s.firstIndex);
			PutU32(b, s.indexCount);
			PutI32(b, s.baseVertex);
		}
		PutVec3(b, g.aabbMin);
		PutVec3(b, g.aabbMax);
		static_assert(sizeof(smvk::VertexPos) == 12, "layout drift");
		static_assert(sizeof(smvk::VertexShading) == 24, "layout drift");
		PutBytes(b, g.positions.data(),
			g.positions.size() * sizeof(smvk::VertexPos));
		PutBytes(b, g.shading.data(),
			g.shading.size() * sizeof(smvk::VertexShading));
		PutBytes(b, g.indices.data(),
			g.indices.size() * sizeof(uint32_t));
	}
	if (!WriteFileBytes(path, b, err))
		return;
}

} // namespace

bool LoadMeshAsset(const std::filesystem::path& path, MeshAsset& out,
	std::string& err) {
	out = MeshAsset{};
	std::vector<uint8_t> bytes;
	if (!ReadFileBytes(path, bytes, err))
		return false;
	BlobReader r{ bytes.data(), bytes.size(), 0, true };
	const uint32_t magic = r.U32();
	const uint32_t version = r.U32();
	if (!r.ok || magic != kMeshMagic) {
		err = "not a .mesh asset: '" + path.generic_string() + "'";
		return false;
	}
	if (version != kMeshVersion) {
		err = "unsupported .mesh version " + std::to_string(version);
		return false;
	}
	const uint32_t nodeCount = r.U32();
	out.nodes.resize(nodeCount);
	for (MeshAsset::Node& n : out.nodes) {
		n.parent = int32_t(r.U32());
		n.level = int32_t(r.U32());
	}
	out.names.resize(r.U32());
	for (std::string& s : out.names)
		s = r.Str();
	out.locals.resize(r.U32());
	for (glm::mat4& t : out.locals)
		t = r.Mat4();
	const uint32_t srCount = r.U32();
	out.staticRoot.resize(srCount);
	for (uint8_t& v : out.staticRoot)
		v = r.U8();
	const uint32_t nlmCount = r.U32();
	out.noLightmap.resize(nlmCount);
	for (uint8_t& v : out.noLightmap)
		v = r.U8();
	out.instances.resize(r.U32());
	for (MeshAsset::Instance& i : out.instances) {
		i.node = r.U32();
		i.geometry = r.U32();
		i.materialGuid = r.Str();
	}
	out.lights.resize(r.U32());
	for (MeshAsset::LightDef& l : out.lights) {
		l.node = r.U32();
		l.def.type = SceneLight::Type(r.U32());
		l.def.color = r.Vec3();
		l.def.intensity = r.F32();
		l.def.range = r.F32();
		l.def.innerConeAngle = r.F32();
		l.def.outerConeAngle = r.F32();
		l.def.flags = r.U32();
		l.def.name = r.Str();
	}
	out.cameras.resize(r.U32());
	for (MeshAsset::CameraDef& c : out.cameras) {
		c.node = r.U32();
		c.def.projection = SceneCamera::Projection(r.U32());
		c.def.fovYDeg = r.F32();
		c.def.orthoHalfH = r.F32();
		c.def.nearClip = r.F32();
		c.def.farClip = r.F32();
		c.def.name = r.Str();
	}
	const uint32_t blobCount = r.U32();
	out.geometry.resize(blobCount);
	for (MeshAsset::Geometry& g : out.geometry) {
		const uint32_t vcount = r.U32();
		const uint32_t icount = r.U32();
		g.surfaces.resize(r.U32());
		for (smvk::Surface& s : g.surfaces) {
			s.firstIndex = r.U32();
			s.indexCount = r.U32();
			s.baseVertex = int32_t(r.U32());
		}
		g.aabbMin = r.Vec3();
		g.aabbMax = r.Vec3();
		g.positions.resize(vcount);
		g.shading.resize(vcount);
		g.indices.resize(icount);
		const void* pos = r.Skip(size_t(vcount) * sizeof(smvk::VertexPos));
		const void* sha = r.Skip(size_t(vcount) * sizeof(smvk::VertexShading));
		const void* idx = r.Skip(size_t(icount) * sizeof(uint32_t));
		if (!r.ok) {
			err = "truncated .mesh geometry blob";
			return false;
		}
		std::memcpy(g.positions.data(), pos,
			size_t(vcount) * sizeof(smvk::VertexPos));
		std::memcpy(g.shading.data(), sha,
			size_t(vcount) * sizeof(smvk::VertexShading));
		std::memcpy(g.indices.data(), idx,
			size_t(icount) * sizeof(uint32_t));
	}
	if (!r.ok) {
		err = "corrupt .mesh asset: '" + path.generic_string() + "'";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Derivation
// ---------------------------------------------------------------------------

bool DeriveGltfAssets(const tinygltf::Model& model,
	const std::filesystem::path& gltfPath, const std::string& gltfGuid,
	const DeriveConfig& config, AssetManifest& outManifest,
	DeriveStats* outStats, std::string* err) {
	DeriveStats stats{};
	std::string fail;
	const std::string stem =
		std::filesystem::path(gltfPath).stem().string();
	const std::filesystem::path dir =
		std::filesystem::path(gltfPath).parent_path();
	auto giveUp = [&](const std::string& e) {
		if (err)
			*err = e;
		return false;
	};

	// 1) The deterministic cook-mode import (GPU-free like the baker;
	// node ids identical to the live import Ã¢â‚¬â€ the .tour replay contract).
	smvk::MeshOptConfig meshOpt{};
	meshOpt.enabled = config.lods;
	auto cluster = std::make_unique<smvk::Cluster>(nullptr);
	cluster->SetCookMode(true);
	std::vector<int> gltfNodeMap;
	{
		GltfToSceneImporter importer{ *cluster, model, gltfPath };
		importer.setMeshOptConfig(meshOpt);
		if (config.meshlets)
			importer.setMeshletConfig(smvk::MeshletConfig{});
		importer.setAutoKtx(true);
		importer.setModelUriBase(gltfPath.generic_string());
		// The classic GltfToCluster body (cook mode, no RM): cluster
		// scene root + the glTF scene roots imported underneath.
		const int root = addNode(cluster->scene, -1, 0);
		cluster->scene.localTransform[size_t(root)] = glm::mat4(1.0f);
		cluster->scene.globalTransform[size_t(root)] = glm::mat4(1.0f);
		std::vector<int> gltfRoots;
		if (model.defaultScene >= 0
			&& model.defaultScene < int(model.scenes.size())) {
			gltfRoots = model.scenes[size_t(model.defaultScene)].nodes;
		}
		else if (!model.scenes.empty()) {
			gltfRoots = model.scenes.front().nodes;
		}
		else {
			std::vector<bool> referenced(model.nodes.size());
			for (const auto& n : model.nodes)
				for (int c : n.children)
					if (c >= 0 && c < int(model.nodes.size()))
						referenced[size_t(c)] = true;
			for (int i = 0; i < int(model.nodes.size()); ++i)
				if (!referenced[size_t(i)])
					gltfRoots.push_back(i);
		}
	 importer.preloadTextures();
		for (int gltfNode : gltfRoots)
			importer.import(gltfNode);
		RebuildStaticCache(cluster->scene);
		recalculateGlobalTransforms(cluster->scene);
		gltfNodeMap = importer.gltfNodeMap();
	}

	// 2) Manifest stability: keep the GUIDs of an existing manifest
	// (matched by kind + source index), fresh GUIDs for new entries.
	AssetManifest manifest;
	manifest.guid = gltfGuid;
	manifest.source =
		std::filesystem::path(gltfPath).filename().generic_string();
	manifest.stem = stem;
	{
		AssetManifest old;
		if (LoadAssetManifest(gltfPath, old, fail) && old.guid == gltfGuid) {
			for (const AssetEntry& e : old.entries) {
				const bool match =
					(e.kind == AssetKind::Mesh)
					|| (e.kind == AssetKind::Material
						&& e.sourceIndex >= 0
						&& e.sourceIndex < int(model.materials.size()))
					|| (e.kind == AssetKind::Anim
						&& e.sourceIndex >= 0
						&& e.sourceIndex < int(model.animations.size()));
				if (match) {
					manifest.entries.push_back(e);
					++stats.keptGuids;
				}
				else
					++stats.dropped;
			}
		}
	}
	auto guidFor = [&](AssetKind kind, int sourceIndex) -> std::string {
		for (const AssetEntry& e : manifest.entries)
			if (e.kind == kind && e.sourceIndex == sourceIndex)
				return e.guid;
		AssetEntry e{};
		e.kind = kind;
		e.sourceIndex = sourceIndex;
		e.guid = smvk::tour::GenerateUuid();
		manifest.entries.push_back(e);
		++stats.newGuids;
		return e.guid;
	};

	// 3) Material assets (straight from the glTF model: the PBR values
	// + texture refs (as-authored image URIs; sRGB = baseColor/emissive,
	// the importer's inferSrgb convention)).
	for (int mi = 0; mi < int(model.materials.size()); ++mi) {
		const tinygltf::Material& gm = model.materials[size_t(mi)];
		MaterialAsset m{};
		m.guid = guidFor(AssetKind::Material, mi);
		m.name = gm.name.empty() ? ("mat" + std::to_string(mi)) : gm.name;
		m.info.baseColorFactor = glm::vec4(
			float(gm.pbrMetallicRoughness.baseColorFactor[0]),
			float(gm.pbrMetallicRoughness.baseColorFactor[1]),
			float(gm.pbrMetallicRoughness.baseColorFactor[2]),
			float(gm.pbrMetallicRoughness.baseColorFactor[3]));
		m.info.metallicFactor =
			float(gm.pbrMetallicRoughness.metallicFactor);
		m.info.roughnessFactor =
			float(gm.pbrMetallicRoughness.roughnessFactor);
		m.info.emissiveFactor = glm::vec3(float(gm.emissiveFactor[0]),
			float(gm.emissiveFactor[1]), float(gm.emissiveFactor[2]));
		m.info.alphaMode = (gm.alphaMode == "MASK")
			? smvk::AlphaMode::Mask : (gm.alphaMode == "BLEND")
			? smvk::AlphaMode::Blend : smvk::AlphaMode::Opaque;
		m.info.alphaCutoff = float(gm.alphaCutoff);
		m.info.doubleSided = gm.doubleSided;
		auto texRef = [&](int texIndex, bool srgb) {
			MaterialAsset::TexRef r;
			r.srgb = srgb;
			if (texIndex < 0 || texIndex >= int(model.textures.size()))
				return r;
			const int img = model.textures[size_t(texIndex)].source;
			if (img < 0 || img >= int(model.images.size()))
				return r;
			r.path = model.images[size_t(img)].uri;
			return r;
		};
		m.baseColor = texRef(
			gm.pbrMetallicRoughness.baseColorTexture.index, true);
		m.metallicRoughness = texRef(
			gm.pbrMetallicRoughness.metallicRoughnessTexture.index, false);
		m.normalTex = texRef(gm.normalTexture.index, false);
		m.occlusion = texRef(gm.occlusionTexture.index, false);
		m.emissiveTex = texRef(gm.emissiveTexture.index, true);
		// Manifest entry (file + name) + the file write.
		const std::string file = MaterialFileName(stem, mi, m.name);
		for (AssetEntry& e : manifest.entries)
			if (e.kind == AssetKind::Material && e.sourceIndex == mi) {
				e.file = file;
				e.name = m.name;
			}
		WriteMaterialAsset(dir / file, m, fail);
		++stats.materials;
	}

	// 4) The .mesh asset (from the imported cluster): the RAW hierarchy +
	// the packed SoA geometry blobs + the material GUID bindings (the
	// cluster material URIs carry "<...>#mat.<gltf index>").
	MeshAsset mesh;
	{
		const Scene& scene = cluster->scene;
		const size_t nodeCount = scene.hierarchy.size();
		mesh.nodes.resize(nodeCount);
		for (size_t i = 0; i < nodeCount; ++i) {
			mesh.nodes[i].parent = scene.hierarchy[i].parent;
			mesh.nodes[i].level = scene.hierarchy[i].level;
		}
		mesh.names.resize(nodeCount);
		for (size_t i = 0; i < nodeCount; ++i) {
			int id = scene.nameForNode.contains(int(i))
				? scene.nameForNode.at(int(i)) : -1;
			mesh.names[i] = (id >= 0 && id < int(scene.nodeNames.size()))
				? scene.nodeNames[size_t(id)] : std::string();
		}
		mesh.locals = scene.localTransform;
		mesh.staticRoot = scene.staticRoot;
		mesh.noLightmap = scene.noLightmap;
		// Geometry blobs: unique meshes in first-use order over the
		// node-sorted instance list (deterministic; the load order
		// matches the live import's pool order closely enough for the
		// .tour replay contract: node ids, not pool ids, address nodes).
		std::vector<std::pair<uint32_t, uint32_t>> meshPairs(
			scene.meshForNode.begin(), scene.meshForNode.end());
		std::sort(meshPairs.begin(), meshPairs.end());
		std::unordered_map<uint32_t, uint32_t> blobOf;
		for (const auto& [node, poolIdx] : meshPairs) {
			uint32_t blob = 0;
			const auto it = blobOf.find(poolIdx);
			if (it != blobOf.end()) {
				blob = it->second;
			}
			else {
				blob = uint32_t(mesh.geometry.size());
				blobOf.emplace(poolIdx, blob);
				MeshAsset::Geometry g{};
				const smvk::ClusterStaticMeshHandle h =
					smvk::ClusterStaticMeshHandle::Mint(poolIdx);
				if (const smvk::StaticMeshUpload* up =
						cluster->GetRetainedUpload(h)) {
					g.positions.resize(up->vertices.size());
					g.shading.resize(up->vertices.size());
					for (size_t v = 0; v < up->vertices.size(); ++v)
						smvk::PackStaticVertex(up->vertices[v],
							g.positions[v], g.shading[v]);
					g.indices = up->indices;
					g.surfaces = up->surfaces;
					g.aabbMin = up->aabbMin;
					g.aabbMax = up->aabbMax;
				}
				mesh.geometry.push_back(std::move(g));
				++stats.geometryBlobs;
			}
			MeshAsset::Instance inst{};
			inst.node = node;
			inst.geometry = blob;
			// Material GUID: the cluster material URI fragment
			// "#mat.<gltf index>" -> the manifest entry.
			const auto mIt = scene.materialForNode.find(node);
			if (mIt != scene.materialForNode.end()) {
				const smvk::StringViewT uri =
					cluster->GetMaterialUri(
						smvk::ClusterMaterialHandle::Mint(mIt->second));
				const std::string u = uri.Str();
				const size_t at = u.rfind("#mat.");
				if (at != std::string::npos) {
					const int mi = std::atoi(u.c_str() + at + 5);
					if (mi >= 0 && mi < int(model.materials.size()))
						inst.materialGuid =
							guidFor(AssetKind::Material, mi);
				}
			}
			mesh.instances.push_back(inst);
			++stats.instances;
		}
		// Lights (the pool defs + node links).
		for (const auto& [node, poolIdx] : scene.lightForNode) {
			if (auto* l = cluster->lights.get(
					smvk::ClusterLightHandle::Mint(poolIdx))) {
				MeshAsset::LightDef d{};
				d.node = node;
				d.def = *l;
				mesh.lights.push_back(d);
				++stats.lights;
			}
		}
	}
	const std::string meshFile = stem + ".mesh";
	for (AssetEntry& e : manifest.entries)
		if (e.kind == AssetKind::Mesh) {
			e.file = meshFile;
			e.name = stem;
		}
	{
		AssetEntry e{};
		e.kind = AssetKind::Mesh;
		e.sourceIndex = -1;
		e.guid = guidFor(AssetKind::Mesh, -1);
		e.file = meshFile;
		e.name = stem;
		for (AssetEntry& x : manifest.entries)
			if (x.kind == AssetKind::Mesh && &x != &e) {
				x.file = meshFile;
				x.name = stem;
			}
	}
	WriteMeshAsset(dir / meshFile, mesh, fail);

	// 5) Animation assets (the .tourAnim format; tracks retargeted onto
	// the .mesh's node ids via the import's gltf node map).
	for (int ai = 0; ai < int(model.animations.size()); ++ai) {
		const tinygltf::Animation& ga = model.animations[size_t(ai)];
		smvk::touranim::AnimDoc doc{};
		doc.source = std::filesystem::path(gltfPath).filename().string();
		smvk::touranim::Clip clip{};
		clip.name = ga.name.empty()
			? ("anim" + std::to_string(ai)) : ga.name;
		double lastT = 0.0;
		for (const tinygltf::AnimationChannel& ch : ga.channels) {
			if (ch.sampler < 0
				|| ch.sampler >= int(ga.samplers.size()))
				continue;
			const int target = ch.target_node;
			if (target < 0 || target >= int(gltfNodeMap.size()))
				continue;
			const int joint = gltfNodeMap[size_t(target)];
			if (joint < 0)
				continue;
			const tinygltf::AnimationSampler& s =
				ga.samplers[size_t(ch.sampler)];
			// Output values: a VEC3 (translate/scale) or a quat (rotate).
			if (s.output < 0 || s.output >= int(model.accessors.size()))
				continue;
			const tinygltf::Accessor& out =
				model.accessors[size_t(s.output)];
			const tinygltf::Accessor& in =
				(s.input >= 0 && s.input < int(model.accessors.size()))
				? model.accessors[size_t(s.input)]
				: tinygltf::Accessor{};
			const size_t keys = std::min(size_t(out.count),
				size_t(in.count));
			if (keys == 0)
				continue;
			smvk::touranim::Track t{};
			t.joint = uint32_t(joint);
			t.name = (target >= 0 && target < int(model.nodes.size()))
				? model.nodes[size_t(target)].name : std::string();
			t.channel = (ch.target_path == "rotation")
				? smvk::touranim::Channel::Rotate
				: (ch.target_path == "scale")
				? smvk::touranim::Channel::Scale
				: smvk::touranim::Channel::Translate;
			t.spline = (s.interpolation == "STEP")
				? smvk::touranim::Spline::Step
				: (s.interpolation == "CUBICSPLINE")
				? smvk::touranim::Spline::CatmullRom
				: smvk::touranim::Spline::Linear;
			for (size_t k = 0; k < keys; ++k) {
				smvk::touranim::Key key{};
				const double* tin = reinterpret_cast<const double*>(
					AnimAccessorPtr<double>(model, in, k));
				if (!tin)
					break;
				key.t = *tin;
				lastT = std::max(lastT, key.t);
				if (t.channel == smvk::touranim::Channel::Rotate) {
					const double* q = reinterpret_cast<const double*>(
						AnimAccessorPtr<double>(model, out, k));
					if (!q)
						break;
					// glTF quat (x,y,z,w) -> glm (w,x,y,z).
					key.v = glm::vec4(float(q[3]), float(q[0]),
						float(q[1]), float(q[2]));
				}
				else {
					const double* v3 = reinterpret_cast<const double*>(
						AnimAccessorPtr<double>(model, out, k));
					if (!v3)
						break;
					key.v = glm::vec4(float(v3[0]), float(v3[1]),
						float(v3[2]), 1.0f);
				}
				t.keys.push_back(key);
			}
			if (!t.keys.empty())
				clip.tracks.push_back(std::move(t));
		}
		clip.duration = lastT;
		doc.clips.push_back(std::move(clip));
		const std::string file = AnimFileName(stem, ai, ga.name);
		std::string aerr;
		WriteTourAnimAsset(dir / file, doc, aerr);
		for (AssetEntry& e : manifest.entries)
			if (e.kind == AssetKind::Anim && e.sourceIndex == ai) {
				e.file = file;
				e.name = ga.name;
			}
		++stats.anims;
	}

	// 6) The manifest.
	if (!SaveAssetManifest(gltfPath, manifest, fail))
		return giveUp("manifest write failed: " + fail);
	outManifest = std::move(manifest);
	if (outStats)
		*outStats = stats;
	return true;
}

} // namespace smvk::assets
