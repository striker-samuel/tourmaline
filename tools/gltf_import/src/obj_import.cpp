// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier-Identifier: MIT

// The OBJ source adapter (see LoadObjFileAsModel in gltf.h): parses a
// Wavefront .obj (+ its mtllib .mtl files) with the vendored
// tinyobjloader and builds the tinygltf::Model INTERCHANGE FORM in
// memory — so the entire downstream (GltfToSceneImporter,
// DeriveGltfAssets: LOD chains, meshlets, .material/.anim artifacts,
// the .meta identity ledger) runs UNCHANGED over OBJ sources. No
// runtime, cooker or .geometry-format knowledge lives here; this file
// is a pure source-format adapter, exactly like the tinygltf load is
// for glTF/GLB.
//
// Mapping decisions (the honest v1 set — every deviation is loud or
// documented, never a silently wrong bind):
//   - one glTF node per OBJ shape ('o'/'g' groups), name preserved;
//     faces grouped into primitives BY MATERIAL (first-appearance
//     order, deterministic)
//   - the glTF vertex is the unique (v, vn, vt) ATTRIBUTE COMBINATION
//     (OBJ splits positions/normals/uvs across three index streams;
//     welding them is the standard glTF-ification)
//   - vt's V axis is FLIPPED (1 - v): OBJ's texture origin is
//     bottom-left, glTF's is top-left
//   - a shape with no authored normals gets smooth (area-weighted,
//     accumulated over the welded positions) normals
//   - Kd/map_Kd -> baseColor (+ dissolve as alpha), Ke/map_Ke ->
//     emissive, `norm` -> normalTexture, Pm/Pr PBR-extension scalars
//     when authored, roughness otherwise inverted from Ns
//     (0..1000 in every DCC dialect), d < 1 or map_d -> BLEND
//   - map_bump is deliberately NOT bound (a HEIGHT field is not a
//     tangent-space normal map — binding it shades garbage); map_Ks /
//     map_Ns / map_Pr / map_Pm are skipped too (specular color is not
//     the metallic-roughness ORM G/B packing). Both cases log once.
//   - OBJ and glTF share the Y-up right-handed basis: no correction
//     transform is applied (the importer's coordFix treats this model
//     exactly like a glTF).

#define TINYOBJLOADER_IMPLEMENTATION

#include <tiny_obj_loader.h>

#include <gltf_import/gltf.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// MTL map paths are OS-native in the wild: backslashes -> POSIX, a
// leading "./" stripped. Absolute paths pass through — the digest and
// staging machinery handles them exactly like glTF's absolute uris.
std::string NormalizeMtlPath(const std::string& authored) {
	std::string p;
	p.reserve(authored.size());
	for (char c : authored)
		p += (c == '\\') ? '/' : c;
	while (p.rfind("./", 0) == 0)
		p.erase(0, 2);
	return p;
}

// The mtllib lines (one line may name several files; names may be
// quoted). Returned AS AUTHORED (posix-normalized, relative to the
// .obj) — the same shape as a glTF dependency uri, so the digest and
// the project import staging resolve them identically.
std::vector<std::string> ParseMtlLibs(const std::string& objText) {
	std::vector<std::string> out;
	std::istringstream lines(objText);
	std::string line;
	while (std::getline(lines, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.compare(0, 7, "mtllib ") != 0)
			continue;
		std::istringstream toks(line.substr(7));
		std::string tok;
		while (toks >> tok) {
			if (tok.size() >= 2 && tok.front() == '"'
					&& tok.back() == '"')
				tok = tok.substr(1, tok.size() - 2);
			const std::string norm = NormalizeMtlPath(tok);
			if (!norm.empty())
				out.push_back(norm);
		}
	}
	return out;
}

// The single binary buffer every accessor slices (glTF's own shape:
// one buffer, tightly-packed 4-byte-aligned bufferViews).
struct BlobBuilder {
	std::vector<uint8_t> data;
	size_t append(const void* bytes, size_t n) {
		while (data.size() % 4)
			data.push_back(0);
		const size_t off = data.size();
		const uint8_t* p = static_cast<const uint8_t*>(bytes);
		data.insert(data.end(), p, p + n);
		return off;
	}
};

int AddBufferView(tinygltf::Model& m, BlobBuilder& blob,
	const void* bytes, size_t n, int target) {
	tinygltf::BufferView bv{};
	bv.buffer = 0;
	bv.byteOffset = blob.append(bytes, n);
	bv.byteLength = n;
	bv.target = target;
	m.bufferViews.push_back(std::move(bv));
	return int(m.bufferViews.size()) - 1;
}

int AddAccessor(tinygltf::Model& m, int bufferView, int componentType,
	int type, size_t count, const std::vector<double>& minValues = {},
	const std::vector<double>& maxValues = {}) {
	tinygltf::Accessor a{};
	a.bufferView = bufferView;
	a.byteOffset = 0;
	a.componentType = componentType;
	a.count = count;
	a.type = type;
	a.minValues = minValues;
	a.maxValues = maxValues;
	m.accessors.push_back(std::move(a));
	return int(m.accessors.size()) - 1;
}

// Smooth normals for shapes authored without any: area-weighted face
// normals accumulated over the WELDED positions (per unique vertex),
// then normalized. Degenerate vertices (zero accumulation) fall back
// to +Y so a numerically broken face never poisons the attribute with
// NaNs.
void ComputeSmoothNormals(std::vector<float>& nrm,
	const std::vector<float>& pos, const std::vector<uint32_t>& idx) {
	nrm.assign(pos.size(), 0.0f);
	for (size_t t = 0; t + 2 < idx.size(); t += 3) {
		const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
		const float* pa = &pos[size_t(a) * 3];
		const float* pb = &pos[size_t(b) * 3];
		const float* pc = &pos[size_t(c) * 3];
		const float e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
		const float e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
		const float n[3] = {
			e1[1] * e2[2] - e1[2] * e2[1],
			e1[2] * e2[0] - e1[0] * e2[2],
			e1[0] * e2[1] - e1[1] * e2[0]
		};
		for (const uint32_t v : { a, b, c }) {
			nrm[size_t(v) * 3 + 0] += n[0];
			nrm[size_t(v) * 3 + 1] += n[1];
			nrm[size_t(v) * 3 + 2] += n[2];
		}
	}
	for (size_t v = 0; v + 2 < nrm.size(); v += 3) {
		const float len = std::sqrt(nrm[v] * nrm[v]
			+ nrm[v + 1] * nrm[v + 1] + nrm[v + 2] * nrm[v + 2]);
		if (len > 1e-12f) {
			nrm[v + 0] /= len;
			nrm[v + 1] /= len;
			nrm[v + 2] /= len;
		}
		else {
			nrm[v + 0] = 0.0f;
			nrm[v + 1] = 1.0f;
			nrm[v + 2] = 0.0f;
		}
	}
}

} // namespace

bool LoadObjFileAsModel(tinygltf::Model& model,
	const std::filesystem::path& objPath, std::vector<std::string>& mtlDeps) {
	mtlDeps.clear();
	model = tinygltf::Model{};

	// The .obj text is read once up front: the mtllib lines name SOURCE
	// DEPENDENCIES the synthetic model cannot express (they ride
	// mtlDeps instead), and a MISSING library is a LOUD failure —
	// tinyobj would silently continue with default materials, but the
	// glTF lane's "missing dependency" policy never lets an import
	// half-succeed.
	std::string objText;
	{
		std::ifstream f(objPath, std::ios::binary);
		if (!f) {
			std::cout << "Failed to open obj: "
				<< objPath.generic_string() << std::endl;
			return false;
		}
		std::ostringstream ss;
		ss << f.rdbuf();
		objText = ss.str();
	}
	const std::filesystem::path baseDir = objPath.parent_path();
	for (const std::string& mtl : ParseMtlLibs(objText)) {
		const std::filesystem::path full =
			std::filesystem::path(mtl).is_absolute()
			? std::filesystem::path(mtl) : baseDir / std::filesystem::path(mtl);
		std::error_code ec;
		if (!std::filesystem::is_regular_file(full, ec)) {
			std::cout << "ERR: obj material library missing: '" << mtl
				<< "' (beside " << objPath.generic_string() << ")"
				<< std::endl;
			return false;
		}
		mtlDeps.push_back(mtl);
	}

	tinyobj::ObjReaderConfig cfg{};
	cfg.triangulate = true;
	cfg.vertex_color = false; // unused by the interchange form
	tinyobj::ObjReader reader;
	if (!reader.ParseFromFile(objPath.string(), cfg)) {
		std::cout << "ERR: " << reader.Error() << std::endl;
		std::cout << "Failed to load obj: "
			<< objPath.generic_string() << std::endl;
		return false;
	}
	if (!reader.Warning().empty())
		std::cout << "WARN: " << reader.Warning() << std::endl;

	const tinyobj::attrib_t& attrib = reader.GetAttrib();
	const std::vector<tinyobj::shape_t>& shapes = reader.GetShapes();
	const std::vector<tinyobj::material_t>& materials =
		reader.GetMaterials();

	// ---- images + materials (deduped image rows, POSIX uris) --------
	// Image/textures are registered by their MTL map path (relative to
	// the .obj): the HostTextureSource's baseDir read, the .material
	// TexRef, the digest and the project staging then all treat them
	// exactly like glTF-authored external textures.
	std::map<std::string, int> imageOf;
	auto imageFor = [&](const std::string& texname) -> int {
		const std::string uri = NormalizeMtlPath(texname);
		if (uri.empty())
			return -1;
		auto it = imageOf.find(uri);
		if (it != imageOf.end())
			return it->second;
		tinygltf::Image img{};
		img.uri = uri;
		model.images.push_back(std::move(img));
		const int idx = int(model.images.size()) - 1;
		imageOf.emplace(uri, idx);
		return idx;
	};
	// One texture row per USAGE (image deduped above) — the glTF shape
	// when samplers differ; the importer resolves texture -> image.
	auto textureFor = [&](const std::string& texname) -> int {
		const int img = imageFor(texname);
		if (img < 0)
			return -1;
		tinygltf::Texture tex{};
		tex.source = img;
		tex.sampler = -1;
		model.textures.push_back(std::move(tex));
		return int(model.textures.size()) - 1;
	};

	for (const tinyobj::material_t& om : materials) {
		tinygltf::Material m{};
		m.name = om.name;
		// Kd -> baseColor. tinyobj defaults diffuse to (0,0,0) — the
		// parser cannot distinguish an authored "Kd 0 0 0" from nothing,
		// and an authored pure black is vanishingly rare, so the
		// no-color case keeps glTF's white default (a map_Kd covers it
		// anyway in real assets).
		const bool authoredKd = om.diffuse[0] != 0.0f
			|| om.diffuse[1] != 0.0f || om.diffuse[2] != 0.0f
			|| !om.diffuse_texname.empty();
		if (authoredKd)
			for (int k = 0; k < 3; ++k)
				m.pbrMetallicRoughness.baseColorFactor[size_t(k)] =
					om.diffuse[k];
		// PBR extension scalars when authored (Pr/Pm; the metallic
		// default 0 is the honest dielectric default too, and a "Pr 0"
		// cannot be told apart from absent — accepted, rare).
		m.pbrMetallicRoughness.metallicFactor =
			std::clamp(double(om.metallic), 0.0, 1.0);
		if (om.roughness > 0.0f)
			m.pbrMetallicRoughness.roughnessFactor =
				std::clamp(double(om.roughness), 0.0, 1.0);
		else
			// Ns (specular exponent, 0..1000 in every DCC dialect)
			// inverted to roughness; tinyobj's default Ns=1 lands matte.
			// The 0.05 floor keeps a "mirror" Ns from fireflying bakes.
			m.pbrMetallicRoughness.roughnessFactor = std::clamp(
				1.0 - double(om.shininess) / 1000.0, 0.05, 1.0);
		if (int t = textureFor(om.diffuse_texname); t >= 0)
			m.pbrMetallicRoughness.baseColorTexture.index = t;
		if (int t = textureFor(om.emissive_texname); t >= 0)
			m.emissiveTexture.index = t;
		if (int t = textureFor(om.normal_texname); t >= 0)
			m.normalTexture.index = t;
		// Ke -> emissive (runtime-only in the engine; baked never).
		// ASSIGN, never index: tinygltf's Material() default-constructs
		// emissiveFactor EMPTY (the JSON parser is what fills it), so an
		// indexed write here is an out-of-bounds store.
		m.emissiveFactor = { double(om.emission[0]),
			double(om.emission[1]), double(om.emission[2]) };
		// Transparency: d < 1 (or Tr) or a map_d -> BLEND (MTL carries
		// no cutoff semantics, so MASK would invent one).
		if (om.dissolve < 1.0f || !om.alpha_texname.empty()) {
			m.alphaMode = "BLEND";
			m.pbrMetallicRoughness.baseColorFactor[3] =
				std::clamp(double(om.dissolve), 0.0, 1.0);
		}
		// Deliberately NOT bound (see the file header): a bump map is a
		// height field, and the Ks/Ns/Pr/Pm maps are not the ORM packing.
		if (!om.bump_texname.empty() || !om.specular_texname.empty()
				|| !om.specular_highlight_texname.empty()
				|| !om.roughness_texname.empty()
				|| !om.metallic_texname.empty())
			std::cout << "OBJ material '" << om.name << "': bump/specular/"
				<< "Pr/Pm maps are not representable in the metallic-"
				"roughness model and are skipped (bake a normal map / "
				"ORM texture instead)" << std::endl;
		model.materials.push_back(std::move(m));
	}

	// ---- geometry: one mesh per shape, primitives by material -------
	BlobBuilder blob;
	std::vector<int> shapeNodes;
	for (const tinyobj::shape_t& shape : shapes) {
		const tinyobj::mesh_t& mesh = shape.mesh;
		if (mesh.indices.empty()) {
			std::cout << "OBJ shape '" << shape.name << "' has no faces"
				" — skipped" << std::endl;
			continue;
		}
		// INVARIANT (tinyobj's triangulating flatten): one material_ids
		// entry + 3 consecutive corners per emitted triangle (a quad
		// pushes TWO entries; degenerate <3-vertex faces push none).
		// Guarded, never assumed — a torn parse fails loudly here.
		if (mesh.indices.size() / 3 < mesh.material_ids.size()) {
			std::cout << "ERR: OBJ shape '" << shape.name << "' has a"
				" torn face index stream (corners " << mesh.indices.size()
				<< " < 3 x faces " << mesh.material_ids.size() << ")"
				<< std::endl;
			return false;
		}
		// Faces are TRIANGLES (triangulate=true): indices.size() is
		// 3 x face count. Grouped BY MATERIAL in ascending material-id
		// order (deterministic; -1 = unassigned sorts first).
		std::map<int, std::vector<uint32_t>> facesByMat;
		for (uint32_t f = 0; f < mesh.material_ids.size(); ++f)
			facesByMat[mesh.material_ids[size_t(f)]].push_back(f);

		tinygltf::Mesh gm{};
		gm.name = shape.name;
		for (const auto& [matId, faces] : facesByMat) {
			// Weld the per-corner (v, vn, vt) triples — the glTF vertex
			// is the unique ATTRIBUTE COMBINATION.
			std::map<std::array<int, 3>, uint32_t> weld;
			std::vector<float> pos, nrm, uv;
			std::vector<uint32_t> idx;
			bool anyNormal = false, anyUv = false;
			bool badIndex = false;
			for (uint32_t f : faces) {
				for (int k = 0; k < 3; ++k) {
					const tinyobj::index_t& ix =
						mesh.indices[size_t(f) * 3 + size_t(k)];
					const int v = ix.vertex_index;
					if (v < 0 || size_t(v) * 3 + 2 >= attrib.vertices.size()) {
						badIndex = true;
						break;
					}
					const std::array<int, 3> key{ v, ix.normal_index,
						ix.texcoord_index };
					auto it = weld.find(key);
					if (it != weld.end()) {
						idx.push_back(it->second);
						continue;
					}
					const uint32_t ni = uint32_t(pos.size() / 3);
					weld.emplace(key, ni);
					idx.push_back(ni);
					pos.push_back(attrib.vertices[size_t(v) * 3 + 0]);
					pos.push_back(attrib.vertices[size_t(v) * 3 + 1]);
					pos.push_back(attrib.vertices[size_t(v) * 3 + 2]);
					if (ix.normal_index >= 0
							&& size_t(ix.normal_index) * 3 + 2
								< attrib.normals.size()) {
						anyNormal = true;
						nrm.push_back(attrib.normals[size_t(ix.normal_index) * 3 + 0]);
						nrm.push_back(attrib.normals[size_t(ix.normal_index) * 3 + 1]);
						nrm.push_back(attrib.normals[size_t(ix.normal_index) * 3 + 2]);
					}
					else {
						nrm.push_back(0.0f);
						nrm.push_back(0.0f);
						nrm.push_back(0.0f);
					}
					if (ix.texcoord_index >= 0
							&& size_t(ix.texcoord_index) * 2 + 1
								< attrib.texcoords.size()) {
						anyUv = true;
						// OBJ's V origin is bottom-left, glTF's top-left.
						uv.push_back(attrib.texcoords[size_t(ix.texcoord_index) * 2 + 0]);
						uv.push_back(1.0f
							- attrib.texcoords[size_t(ix.texcoord_index) * 2 + 1]);
					}
					else {
						uv.push_back(0.0f);
						uv.push_back(0.0f);
					}
				}
				if (badIndex)
					break;
			}
			if (badIndex) {
				std::cout << "ERR: OBJ shape '" << shape.name
					<< "' has out-of-range vertex indices" << std::endl;
				return false;
			}
			if (!anyNormal)
				ComputeSmoothNormals(nrm, pos, idx);

			tinygltf::Primitive prim{};
			prim.material = matId; // -1 = the importer's default material
			prim.mode = TINYGLTF_MODE_TRIANGLES;
			{
				std::vector<double> mn(3, 0.0), mx(3, 0.0);
				for (size_t p = 0; p + 2 < pos.size(); p += 3)
					for (int k = 0; k < 3; ++k) {
						mn[size_t(k)] = std::min(mn[size_t(k)],
							double(pos[p + size_t(k)]));
						mx[size_t(k)] = std::max(mx[size_t(k)],
							double(pos[p + size_t(k)]));
					}
				const int bv = AddBufferView(model, blob, pos.data(),
					pos.size() * sizeof(float),
					TINYGLTF_TARGET_ARRAY_BUFFER);
				prim.attributes["POSITION"] = AddAccessor(model, bv,
					TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3,
					pos.size() / 3, mn, mx);
			}
			{
				const int bv = AddBufferView(model, blob, nrm.data(),
					nrm.size() * sizeof(float),
					TINYGLTF_TARGET_ARRAY_BUFFER);
				prim.attributes["NORMAL"] = AddAccessor(model, bv,
					TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3,
					nrm.size() / 3);
			}
			if (anyUv) {
				const int bv = AddBufferView(model, blob, uv.data(),
					uv.size() * sizeof(float),
					TINYGLTF_TARGET_ARRAY_BUFFER);
				prim.attributes["TEXCOORD_0"] = AddAccessor(model, bv,
					TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2,
					uv.size() / 2);
			}
			{
				const int bv = AddBufferView(model, blob, idx.data(),
					idx.size() * sizeof(uint32_t),
					TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
				prim.indices = AddAccessor(model, bv,
					TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
					TINYGLTF_TYPE_SCALAR, idx.size());
			}
			gm.primitives.push_back(std::move(prim));
		}
		if (gm.primitives.empty())
			continue; // everything about this shape was skipped above
		model.meshes.push_back(std::move(gm));
		tinygltf::Node node{};
		node.name = shape.name;
		node.mesh = int(model.meshes.size()) - 1;
		model.nodes.push_back(std::move(node));
		shapeNodes.push_back(int(model.nodes.size()) - 1);
	}
	if (model.meshes.empty()) {
		std::cout << "Failed to load obj (no triangulable geometry): "
			<< objPath.generic_string() << std::endl;
		return false;
	}

	tinygltf::Buffer buffer{};
	buffer.name = "obj";
	buffer.data = std::move(blob.data);
	model.buffers.push_back(std::move(buffer));

	tinygltf::Scene scene{};
	scene.name = objPath.stem().generic_string();
	scene.nodes = std::move(shapeNodes);
	model.scenes.push_back(std::move(scene));
	model.defaultScene = 0;
	model.asset.version = "2.0";
	model.asset.generator = "Tourmaline OBJ adapter (tinyobjloader v2.0.0rc13)";
#ifndef NDEBUG
	std::cout << "Loaded obj: " << objPath.generic_string()
		<< " (shapes " << model.meshes.size()
		<< " materials " << model.materials.size()
		<< " images " << model.images.size() << ")" << std::endl;
#endif
	return true;
}
