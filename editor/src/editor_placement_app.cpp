// PlacementGraph editor: EditorApp integration (persistence, mesh
// extraction, live preview). Model/compiler live in editor_placement.cpp.

#include "editor_app.h"
#include "editor_placement.h"

#include <chrono>
#include <cstdio>
#include <sstream>

#include <tiny_gltf.h>

namespace editor {
using namespace placement;
// Mesh extraction reads the kept glTF primitive for a world node (same
// lossless source the picker uses), transformed to world space.
namespace {
glm::vec3 FixGltfPos(const float* f) {
	return glm::vec3(-f[0], f[1], -f[2]); // coordFix 0 mirror (picking parity)
}
bool ReadVec3Attr(const tinygltf::Model& model, int accessorIdx, size_t i,
	glm::vec3& out) {
	if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size())
		return false;
	const tinygltf::Accessor& acc = model.accessors[accessorIdx];
	if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return false;
	if (i >= (size_t)acc.count) return false;
	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];
	const size_t stride = acc.ByteStride(bv);
	const size_t off = bv.byteOffset + acc.byteOffset + i * stride;
	if (off + 12 > buf.data.size()) return false;
	const float* f = reinterpret_cast<const float*>(buf.data.data() + off);
	out = FixGltfPos(f);
	return true;
}
bool ReadIdxAttr(const tinygltf::Model& model, int accessorIdx, size_t i,
	uint32_t& out) {
	if (accessorIdx < 0) return false;
	const tinygltf::Accessor& acc = model.accessors[accessorIdx];
	if (i >= (size_t)acc.count) return false;
	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];
	const size_t stride = acc.ByteStride(bv);
	const size_t off = bv.byteOffset + acc.byteOffset + i * stride;
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
		if (off + 4 > buf.data.size()) return false;
		out = *reinterpret_cast<const uint32_t*>(buf.data.data() + off);
		return true;
	}
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
		if (off + 2 > buf.data.size()) return false;
		out = *reinterpret_cast<const uint16_t*>(buf.data.data() + off);
		return true;
	}
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
		if (off + 1 > buf.data.size()) return false;
		out = buf.data[off];
		return true;
	}
	return false;
}
} // namespace

bool EditorApp::ExtractPlacementMesh(uint32_t worldNode,
	smvk::placement::PlacementMeshData& out) const {
	using namespace smvk::placement;
	out = PlacementMeshData{};
	if (!m_world || !m_hasGltfModel) return false;
	auto it = m_primRef.find(worldNode);
	if (it == m_primRef.end() || it->second.mesh < 0
		|| it->second.mesh >= (int)m_gltfModel.meshes.size())
		return false;
	const tinygltf::Mesh& gm = m_gltfModel.meshes[it->second.mesh];
	if (it->second.prim < 0 || it->second.prim >= (int)gm.primitives.size())
		return false;
	const tinygltf::Primitive& prim = gm.primitives[it->second.prim];
	auto pa = prim.attributes.find("POSITION");
	if (pa == prim.attributes.end()) return false;
	const size_t vcount = m_gltfModel.accessors[pa->second].count;
	if (vcount == 0 || vcount > 600000) return false;
	const glm::mat4& model =
		m_world->scene.globalTransform.size() > worldNode
		? m_world->scene.globalTransform[worldNode]
		: glm::mat4(1.0f);
	out.positions.reserve(vcount);
	for (size_t v = 0; v < vcount; ++v) {
		glm::vec3 local;
		if (!ReadVec3Attr(m_gltfModel, pa->second, v, local)) return false;
		out.positions.push_back(glm::vec3(model * glm::vec4(local, 1.0f)));
	}
	auto na = prim.attributes.find("NORMAL");
	if (na != prim.attributes.end()
		&& m_gltfModel.accessors[na->second].count == (int)vcount) {
		out.normals.reserve(vcount);
		glm::mat3 nrm(model); // coord fix is a proper rotation: same map
		for (size_t v = 0; v < vcount; ++v) {
			glm::vec3 n;
			if (!ReadVec3Attr(m_gltfModel, na->second, v, n)) break;
			out.normals.push_back(glm::normalize(nrm * n));
		}
		if (out.normals.size() != vcount) out.normals.clear();
	}
	if (prim.indices >= 0) {
		const size_t tcount = m_gltfModel.accessors[prim.indices].count / 3;
		if (tcount == 0 || tcount > 200000) return false;
		out.indices.reserve(tcount * 3);
		for (size_t t = 0; t < tcount; ++t)
			for (int k = 0; k < 3; ++k) {
				uint32_t ix = 0;
				if (!ReadIdxAttr(m_gltfModel, prim.indices, t * 3 + k, ix)
					|| ix >= vcount)
					return false;
				out.indices.push_back(ix);
			}
	}
	else {
		if (vcount % 3 != 0) return false;
		out.indices.reserve(vcount);
		for (uint32_t v = 0; v < (uint32_t)vcount; ++v) out.indices.push_back(v);
	}
	BuildMeshAliasTable(out);
	return out.triangleCount() > 0;
}

void EditorApp::MarkPlacementDirty() {
	m_placement.preview.dirty = true;
}

void EditorApp::RefreshPlacementPreview() {
	using namespace smvk::placement;
	PreviewCache& pv = m_placement.preview;
	pv.error.clear();
	pv.stats.clear();
	pv.accepted.clear();
	pv.rejected.clear();
	pv.picked = -1;
	pv.meshTris = 0;
	GraphDoc* g = m_placement.current();
	if (!g) {
		pv.ok = false;
		pv.error = "no placement graph (Graph + New)";
		pv.dirty = false;
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	CompileResult cr = CompileGraph(*g, m_placement.active);
	pv.compiled = cr;
	if (!cr.ok) {
		pv.ok = false;
		pv.error = cr.error;
		pv.dirty = false;
		return;
	}
	PlacementSystem::LayerBinding lb;
	lb.programId = 1;
	lb.program = cr.program;
	lb.prototypes = cr.prototypes;
	lb.config = cr.config;
	lb.domainKind = cr.domainKind;
	GridDomain gridStorage;
	PlacementMeshData meshStorage;
	HeightfieldData terrainStorage;
	HeightfieldDomain terrainDom;
	if (cr.domainKind == DomainKind::Grid) {
		gridStorage = cr.grid;
		lb.grid = &gridStorage;
	}
	else if (cr.domainKind == DomainKind::Heightfield) {
		terrainStorage = BuildTerrainHeights(cr.terrain);
		terrainDom.field = &terrainStorage;
		terrainDom.extentMin = cr.terrain.origin;
		terrainDom.extentMax = cr.terrain.origin + glm::vec2(cr.terrain.size);
		lb.heightfield = &terrainDom;
		pv.terrainField = terrainStorage;
		pv.terrainDom = terrainDom;
		pv.terrainDom.field = &pv.terrainField; // re-point at the cache copy
	}
	else {
		if (!ExtractPlacementMesh(cr.meshNode, meshStorage)) {
			pv.ok = false;
			pv.error = "Mesh node " + std::to_string(cr.meshNode) +
			 ": no kept glTF primitive (pak scenes have no CPU geometry)";
			pv.dirty = false;
			return;
		}
		pv.meshTris = (uint32_t)meshStorage.triangleCount();
		lb.mesh = &meshStorage;
	}
	const uint32_t want = cr.config.candidatesPerTile;
	const uint32_t n = std::min(want, m_placement.previewCap);
	lb.config.candidatesPerTile = n;
	pv.system.registerLayer(lb);
	pv.system.generateTile(1, TileID{ 0, 0, 0 }, DependencyVersions{});
	const PlacementTile* tile = pv.system.findTile(1, TileID{ 0, 0, 0 });
	if (tile) {
		for (const RenderInstance& in : tile->instances) {
			StableID id = MakeStableID(cr.program.graphSeed, cr.program.nodeId,
				TileID{ 0, 0, 0 }, 0, in.customB);
			pv.accepted.push_back(PreviewPoint{ in.position, in.prototypeId,
				in.customB, id });
		}
	}
	// Rejected overlay sample (accept stage only, no constraint; capped).
	uint32_t shown = 0;
	for (uint32_t i = 0; i < n && shown < 1500; ++i) {
		StableID id = MakeStableID(cr.program.graphSeed, cr.program.nodeId,
			TileID{ 0, 0, 0 }, 0, i);
		DomainSample s = (cr.domainKind == DomainKind::Grid)
			? gridStorage.sample(id, i)
			: (cr.domainKind == DomainKind::Heightfield
				? terrainDom.sample(id)
				: SampleMeshSurface(meshStorage, id));
		std::vector<float> values = EvaluateProgram(cr.program, s);
		if (!AcceptCandidate(values[cr.program.acceptIndex], id, cr.config.densityScale)) {
			pv.rejected.push_back(PreviewPoint{ s.p, 0, i, id });
			++shown;
		}
	}
	pv.grid = gridStorage;
	pv.mesh = meshStorage;
	pv.domainKind = cr.domainKind;
	const auto t1 = std::chrono::steady_clock::now();
	pv.genMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
	pv.candidateTotal = n;
	char buf[512];
	std::map<uint32_t, size_t> perProto;
	for (const PreviewPoint& p : pv.accepted) perProto[p.proto]++;
	std::snprintf(buf, sizeof(buf),
		"%u candidates -> %u accepted (%.1f%%), %u rejected-shown, %.1f ms",
		n, (unsigned)pv.accepted.size(),
		n ? 100.0 * (double)pv.accepted.size() / (double)n : 0.0,
		(unsigned)pv.rejected.size(), pv.genMs);
	pv.stats = buf;
	if (n < want) {
		pv.stats += "  [preview capped at " + std::to_string(n) + "]";
		if (pv.compiled.warning.empty()) pv.compiled.warning = "preview candidate cap active";
	}
	if (pv.meshTris > 0)
		pv.stats += "  [mesh: " + std::to_string(pv.meshTris) + " tris]";
	if (cr.domainKind == DomainKind::Heightfield)
		pv.stats += "  [terrain: " + std::to_string(cr.terrain.res) + "^2]";
	for (const auto& [proto, cnt] : perProto)
		pv.stats += "  p" + std::to_string(proto) + ":" + std::to_string(cnt);
	pv.ok = true;
	pv.dirty = false;
}

void EditorApp::SavePlacementToDoc(smvk::tour::Doc& doc) const {
	auto put = [&](const std::string& key, const std::string& val) {
		doc.editorSettings.emplace_back(key, val);
	};
	for (const auto& [gid, g] : m_placement.graphs) {
		const std::string base = "place.graph." + std::to_string(gid);
		put(base + ".name", g.name);
		for (const auto& [nid, n] : g.nodes) {
			put(base + ".node." + std::to_string(nid), SerializeNode(n));
			char pb[64];
			std::snprintf(pb, sizeof(pb), "%.6g %.6g",
				(double)n.pos[0], (double)n.pos[1]);
			put(base + ".pos." + std::to_string(nid), pb);
		}
		for (const auto& [lid, l] : g.links)
			put(base + ".link." + std::to_string(lid), SerializeLink(l));
	}
	put("place.ui.active", std::to_string(m_placement.active));
	put("place.ui.open", m_placement.showGraph ? "1" : "0");
}

void EditorApp::LoadPlacementFromDoc(const smvk::tour::Doc& doc) {
	m_placement.graphs.clear();
	m_placement.undoStack.clear();
	m_placement.undoPos = -1;
	struct Acc {
		GraphDoc g;
		std::unordered_map<uint32_t, std::pair<float, float>> pos;
	};
	std::map<uint64_t, Acc> acc;
	auto gidOf = [](const std::string& key, size_t prefix, uint64_t& gid,
		std::string& rest) {
		const size_t dot = key.find('.', prefix);
		if (dot == std::string::npos) return false;
		try { gid = std::stoull(key.substr(prefix, dot - prefix)); }
		catch (...) { return false; }
		rest = key.substr(dot + 1);
		return true;
	};
	for (const auto& [key, val] : doc.editorSettings) {
		if (key.rfind("place.graph.", 0) == 0) {
			uint64_t gid = 0;
			std::string rest;
			if (!gidOf(key, 12, gid, rest)) continue;
			Acc& a = acc[gid];
			if (rest == "name") { a.g.name = val; }
			else if (rest.rfind("node.", 0) == 0) {
				uint32_t nid = (uint32_t)detail::StrU64(rest.substr(5), UINT32_MAX);
				PNode n;
				if (nid != UINT32_MAX && ParseNode(val, n)) {
					n.id = nid;
					a.g.nodes[nid] = n;
				}
			}
			else if (rest.rfind("pos.", 0) == 0) {
				uint32_t nid = (uint32_t)detail::StrU64(rest.substr(4), UINT32_MAX);
				if (nid != UINT32_MAX) {
					std::istringstream in(val);
					float x = 0, y = 0;
					if (in >> x >> y) a.pos[nid] = { x, y };
				}
			}
			else if (rest.rfind("link.", 0) == 0) {
				uint32_t lid = (uint32_t)detail::StrU64(rest.substr(5), UINT32_MAX);
				PLink l;
				if (lid != UINT32_MAX && ParseLink(val, l)) {
					l.id = lid;
					a.g.links[lid] = l;
				}
			}
		}
		else if (key == "place.ui.active") {
			m_placement.active = detail::StrU64(val, 0);
		}
		else if (key == "place.ui.open") {
			m_placement.showGraph = (val == "1");
		}
	}
	for (auto& [gid, a] : acc) {
		for (const auto& [nid, p] : a.pos) {
			auto it = a.g.nodes.find(nid);
			if (it != a.g.nodes.end()) {
				it->second.pos[0] = p.first;
				it->second.pos[1] = p.second;
			}
		}
		uint32_t mx = 0;
		for (const auto& [nid, n] : a.g.nodes) mx = std::max(mx, nid);
		for (const auto& [lid, l] : a.g.links) mx = std::max(mx, lid);
		a.g.nextId = mx + 1;
		m_placement.graphs[gid] = a.g;
	}
	uint64_t mx = 0;
	for (const auto& [gid, g] : m_placement.graphs) mx = std::max(mx, gid);
	m_placement.nextGraph = mx + 1;
	if (!m_placement.graphs.contains(m_placement.active))
		m_placement.active = m_placement.graphs.empty() ? 0 : m_placement.graphs.begin()->first;
	m_placement.selected = -1;
	m_placement.selectedLink = -1;
	m_placement.linkFrom = -1;
	MarkPlacementDirty();
	m_placement.pushUndo(); // baseline for the session undo stack
	if (!m_placement.graphs.empty())
		Log("tour: restored " + std::to_string(m_placement.graphs.size()) +
			" placement graph(s).");
}

} // namespace editor
