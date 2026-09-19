// CPU ray picking: mouse -> world ray -> closest mesh node.
//
// Mouse/extent are VIEWPORT-LOCAL (the central rect between the panels;
// the projection is built for that aspect). Exact triangle tests use the
// kept tinygltf model (import is lossless for positions: importer only
// applies the coord-fix rigid transform, mirrored here). Scenes loaded
// from pak (no glTF kept) fall back to AABB picking.

#include "editor_app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_set>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/renderview.h>

#include <tiny_gltf.h>

namespace editor {
namespace {

// coordFix 0 (engine default): (-x, y, -z). Mirror of readVec3ForientationFix.
glm::vec3 FixPos(const double* p) {
	return glm::vec3(float(-p[0]), float(p[1]), float(-p[2]));
}

bool ReadPosition(const tinygltf::Model& model, int accessorIdx, size_t i,
	glm::vec3& out) {
	if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size())
		return false;
	const tinygltf::Accessor& acc = model.accessors[accessorIdx];
	if (i >= (size_t)acc.count)
		return false;
	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];
	const size_t stride = acc.ByteStride(bv);
	const size_t off = bv.byteOffset + acc.byteOffset + i * stride;
	if (off + 12 > buf.data.size())
		return false;
	const float* f = reinterpret_cast<const float*>(buf.data.data() + off);
	if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
		return false;
	const double d[3] = { f[0], f[1], f[2] };
	out = FixPos(d);
	return true;
}

bool ReadIndex(const tinygltf::Model& model, int accessorIdx, size_t i,
	uint32_t& out) {
	if (accessorIdx < 0)
		return false;
	const tinygltf::Accessor& acc = model.accessors[accessorIdx];
	if (i >= (size_t)acc.count)
		return false;
	const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
	const tinygltf::Buffer& buf = model.buffers[bv.buffer];
	const size_t stride = acc.ByteStride(bv);
	const size_t off = bv.byteOffset + acc.byteOffset + i * stride;
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
		if (off + 4 > buf.data.size())
			return false;
		out = *reinterpret_cast<const uint32_t*>(buf.data.data() + off);
		return true;
	}
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
		if (off + 2 > buf.data.size())
			return false;
		out = *reinterpret_cast<const uint16_t*>(buf.data.data() + off);
		return true;
	}
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
		if (off + 1 > buf.data.size())
			return false;
		out = buf.data[off];
		return true;
	}
	return false;
}

// Moller-Trumbore. Returns t or -1.
float RayTri(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a,
	const glm::vec3& b, const glm::vec3& c) {
	const glm::vec3 e1 = b - a, e2 = c - a;
	const glm::vec3 p = glm::cross(d, e2);
	const float det = glm::dot(e1, p);
	if (std::fabs(det) < 1e-12f)
		return -1.0f;
	const float inv = 1.0f / det;
	const glm::vec3 t = o - a;
	const float u = glm::dot(t, p) * inv;
	if (u < 0.0f || u > 1.0f)
		return -1.0f;
	const glm::vec3 q = glm::cross(t, e1);
	const float v = glm::dot(d, q) * inv;
	if (v < 0.0f || u + v > 1.0f)
		return -1.0f;
	const float tt = glm::dot(e2, q) * inv;
	return (tt > 1e-6f) ? tt : -1.0f;
}

bool RayAabb(const glm::vec3& o, const glm::vec3& inv, const glm::vec3& mn,
	const glm::vec3& mx, float& tOut) {
	float tmin = 0.0f, tmax = FLT_MAX;
	for (int i = 0; i < 3; ++i) {
		float t0 = (mn[i] - o[i]) * inv[i];
		float t1 = (mx[i] - o[i]) * inv[i];
		if (inv[i] < 0.0f)
			std::swap(t0, t1);
		tmin = std::max(tmin, t0);
		tmax = std::min(tmax, t1);
		if (tmax < tmin)
			return false;
	}
	tOut = tmin;
	return true;
}

} // namespace

glm::vec2 EditorApp::UvToFlatNdc(glm::vec2 uv, int vpIdx) const {
	// Must match uvflat.vert.hlsli: uvT = (uv-0.5)*zoom + 0.5 + pan,
	// then NDC = uvT*2-1. glTF image space (0,0) top-left maps to NDC
	// (-1,-1) at screen top (Vulkan Y-flip handled by the viewport).
	const int i = std::clamp(vpIdx, 0, kMaxViewports - 1);
	const float z = (m_uvZoom[i] > 1e-6f) ? m_uvZoom[i] : 1.0f;
	const glm::vec2 t = (uv - glm::vec2(0.5f)) * z + glm::vec2(0.5f) + m_uvPan[i];
	return t * 2.0f - glm::vec2(1.0f);
}

glm::vec2 EditorApp::FlatNdcToUv(glm::vec2 ndc, int vpIdx) const {
	const int i = std::clamp(vpIdx, 0, kMaxViewports - 1);
	const float z = (m_uvZoom[i] > 1e-6f) ? m_uvZoom[i] : 1.0f;
	const glm::vec2 t = (ndc + glm::vec2(1.0f)) * 0.5f;
	return (t - glm::vec2(0.5f) - m_uvPan[i]) / z + glm::vec2(0.5f);
}

glm::vec3 EditorApp::MouseRay(float mouseX, float mouseY, smvk::Extent2D extent,
	const glm::mat4& viewProj, glm::vec3& outOrigin) const {
	// Mouse pixels (top-left origin) -> NDC (Y-down after the Vulkan flip)
	// -> world ray via the inverse view-projection. Depth is REVERSED-Z:
	// NDC z=1 is the near plane, z=0 the far plane (see
	// MakeReversedZProjRH) — unprojecting z=0 as "near" builds a
	// backwards ray that hits geometry behind the camera instead of
	// what's under the cursor.
	const float nx = (extent.width > 0) ? (mouseX / float(extent.width)) * 2.0f - 1.0f : 0.0f;
	const float ny = (extent.height > 0) ? (mouseY / float(extent.height)) * 2.0f - 1.0f : 0.0f;
	const glm::vec4 near = glm::inverse(viewProj) * glm::vec4(nx, ny, 1.0f, 1.0f);
	const glm::vec4 far = glm::inverse(viewProj) * glm::vec4(nx, ny, 0.0f, 1.0f);
	const glm::vec3 n = glm::vec3(near) / near.w;
	const glm::vec3 f = glm::vec3(far) / far.w;
	outOrigin = n;
	return glm::normalize(f - n);
}

std::optional<EditorApp::PickHit> EditorApp::PickNode(int vpIdx, float mouseX,
	float mouseY, smvk::Extent2D extent) const {
	if (!m_world || m_world->scene.hierarchy.empty())
		return std::nullopt;
	glm::vec3 origin;
	const glm::mat4 viewProj = m_viewports[std::clamp(vpIdx, 0, kMaxViewports - 1)].viewProj;
	const glm::vec3 dir = MouseRay(mouseX, mouseY, extent, viewProj, origin);
	const glm::vec3 inv(1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z);

	PickHit best{};
	best.t = FLT_MAX;
	bool found = false;
	const smvk::IteratorFlags req = smvk::IteratorFlags(
		smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_StaticVertexFmt);
	m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
		if (!IsVisibleNode(m_world->scene, int(item.node)))
			return; // hidden subtrees are not pickable
		smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
		if (!mesh)
			return;
		const glm::mat4& model = m_world->scene.globalTransform[item.node];
		glm::vec3 c;
		float r = 0.0f;
		smvk::MeshBoundsToWorld(model, mesh->geometry.aabbMin,
			mesh->geometry.aabbMax, c, r);
		// Broadphase: sphere test.
		const glm::vec3 oc = c - origin;
		const float tca = glm::dot(oc, dir);
		if (tca < 0.0f && glm::dot(oc, oc) > r * r)
			return;
		const float d2 = glm::dot(oc, oc) - tca * tca;
		if (d2 > r * r)
			return;
		if (found && tca - r >= best.t)
			return; // cannot beat the current best

		float tHit = FLT_MAX;
		auto it = m_primRef.find(item.node);
		if (m_hasGltfModel && it != m_primRef.end() && it->second.mesh >= 0) {
			// Exact triangle walk over the glTF primitive.
			const tinygltf::Mesh& gm = m_gltfModel.meshes[it->second.mesh];
			if (it->second.prim < (int)gm.primitives.size()) {
				const tinygltf::Primitive& prim = gm.primitives[it->second.prim];
				auto pa = prim.attributes.find("POSITION");
				if (pa != prim.attributes.end()) {
					const size_t vcount =
						m_gltfModel.accessors[pa->second].count;
					glm::vec3 a, b, cc;
					auto vAt = [&](uint32_t idx, glm::vec3& out) {
						glm::vec3 local;
						if (!ReadPosition(m_gltfModel, pa->second, idx, local))
							return false;
						out = glm::vec3(model * glm::vec4(local, 1.0f));
						return true;
					};
					if (prim.indices >= 0) {
						const size_t tcount =
							m_gltfModel.accessors[prim.indices].count / 3;
						for (size_t t = 0; t < tcount; ++t) {
							uint32_t ix[3] = {};
							bool ok = true;
							for (int k = 0; k < 3; ++k)
								ok &= ReadIndex(m_gltfModel, prim.indices,
									t * 3 + k, ix[k]);
							if (!ok || ix[0] >= vcount || ix[1] >= vcount ||
								ix[2] >= vcount)
								continue;
							if (!vAt(ix[0], a) || !vAt(ix[1], b) || !vAt(ix[2], cc))
								continue;
							const float th = RayTri(origin, dir, a, b, cc);
							if (th > 0.0f && th < tHit)
								tHit = th;
						}
					}
					else {
						for (size_t t = 0; t + 2 < vcount; t += 3) {
							if (!vAt(uint32_t(t), a) || !vAt(uint32_t(t + 1), b) ||
								!vAt(uint32_t(t + 2), cc))
								continue;
							const float th = RayTri(origin, dir, a, b, cc);
							if (th > 0.0f && th < tHit)
								tHit = th;
						}
					}
				}
			}
		}
		else {
			// AABB fallback (pak-loaded scenes).
			glm::vec3 mn = glm::vec3(model * glm::vec4(mesh->geometry.aabbMin, 1.0f));
			glm::vec3 mx = glm::vec3(model * glm::vec4(mesh->geometry.aabbMax, 1.0f));
			const glm::vec3 lo = glm::min(mn, mx), hi = glm::max(mn, mx);
			float ta = 0.0f;
			if (RayAabb(origin, inv, lo, hi, ta))
				tHit = ta;
		}
		if (tHit < FLT_MAX && (!found || tHit < best.t)) {
			best.node = item.node;
			best.t = tHit;
			found = true;
		}
		});
	if (!found)
		return std::nullopt;
	return best;
}

std::optional<EditorApp::SubHit> EditorApp::PickSubObject(int vpIdx,
	float mouseX, float mouseY, smvk::Extent2D extent) const {
	// Brute-force triangle walk (no acceleration structures, per request):
	// closest triangle under the cursor + nearest of its 3 verts.
	if (!m_world || m_world->scene.hierarchy.empty())
		return std::nullopt;
	glm::vec3 origin;
	const glm::mat4 viewProj =
		m_viewports[std::clamp(vpIdx, 0, kMaxViewports - 1)].viewProj;
	const glm::vec3 dir = MouseRay(mouseX, mouseY, extent, viewProj, origin);
	SubHit best{};
	float bestT = FLT_MAX;
	bool found = false;
	const smvk::IteratorFlags req = smvk::IteratorFlags(
		smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_StaticVertexFmt);
	m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
		if (!IsVisibleNode(m_world->scene, int(item.node)))
			return;
		smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
		if (!mesh)
			return;
		const glm::mat4& model = m_world->scene.globalTransform[item.node];
		glm::vec3 c;
		float r = 0.0f;
		smvk::MeshBoundsToWorld(model, mesh->geometry.aabbMin,
			mesh->geometry.aabbMax, c, r);
		const glm::vec3 oc = c - origin;
		const float tca = glm::dot(oc, dir);
		if (tca < 0.0f && glm::dot(oc, oc) > r * r)
			return;
		if (glm::dot(oc, oc) - tca * tca > r * r)
			return;
		if (found && tca - r >= bestT)
			return;
		auto it = m_primRef.find(item.node);
		if (!m_hasGltfModel || it == m_primRef.end() || it->second.mesh < 0)
			return; // AABB-only nodes carry no sub-object info
		const tinygltf::Mesh& gm = m_gltfModel.meshes[it->second.mesh];
		if (it->second.prim >= int(gm.primitives.size()))
			return;
		const tinygltf::Primitive& prim = gm.primitives[it->second.prim];
		auto pa = prim.attributes.find("POSITION");
		if (pa == prim.attributes.end())
			return;
		const size_t vcount = m_gltfModel.accessors[pa->second].count;
		auto vAt = [&](uint32_t idx, glm::vec3& out) {
			glm::vec3 local;
			if (!ReadPosition(m_gltfModel, pa->second, idx, local))
				return false;
			out = glm::vec3(model * glm::vec4(local, 1.0f));
			return true;
		};
		if (prim.indices >= 0) {
			const size_t tcount = m_gltfModel.accessors[prim.indices].count / 3;
			for (size_t t = 0; t < tcount; ++t) {
				uint32_t ix[3] = {};
				bool ok = true;
				for (int k = 0; k < 3; ++k)
					ok &= ReadIndex(m_gltfModel, prim.indices, t * 3 + k, ix[k]);
				if (!ok || ix[0] >= vcount || ix[1] >= vcount || ix[2] >= vcount)
					continue;
				glm::vec3 a, b, cc;
				if (!vAt(ix[0], a) || !vAt(ix[1], b) || !vAt(ix[2], cc))
					continue;
				const float th = RayTri(origin, dir, a, b, cc);
				if (th > 0.0f && th < bestT) {
					bestT = th;
					best.node = item.node;
					best.tri = uint32_t(t);
					const glm::vec3 hit = origin + dir * th;
					float d0 = glm::distance(hit, a);
					float d1 = glm::distance(hit, b);
					float d2 = glm::distance(hit, cc);
					best.vert = (d0 <= d1 && d0 <= d2) ? ix[0]
						: (d1 <= d2) ? ix[1] : ix[2];
					best.pos = hit;
					found = true;
				}
			}
		}
	});
	if (!found)
		return std::nullopt;
	return best;
}

std::optional<EditorApp::SubHit> EditorApp::PickFlatUv(int vpIdx,
	float mouseX, float mouseY) const {
	// Flat-UV pane pick (brute force, no accel): mouse -> NDC -> UV
	// (inverse zoom/pan) -> point-in-triangle over the selection subtree
	// (or the whole scene when nothing is selected).
	if (!m_hasGltfModel || !m_world)
		return std::nullopt;
	const int vi = std::clamp(vpIdx, 0, kMaxViewports - 1);
	const ViewportRect& vp = m_viewports[vi].rect;
	if (vp.w <= 0.0f || vp.h <= 0.0f)
		return std::nullopt;
	const ViewMode mode = m_viewports[vi].viewMode;
	const bool lightmap = (mode == ViewMode::FlatUv1);
	const glm::vec2 ndc(mouseX / vp.w * 2.0f - 1.0f,
		mouseY / vp.h * 2.0f - 1.0f);
	const glm::vec2 puv = FlatNdcToUv(ndc, vi);
	// Candidate set: selection subtree when something is selected.
	std::unordered_set<uint32_t> subtree;
	bool useSubtree = (m_selected != UINT32_MAX &&
		m_selected < m_world->scene.hierarchy.size());
	if (useSubtree) {
		std::vector<uint32_t> stack{ m_selected };
		while (!stack.empty()) {
			const uint32_t n = stack.back();
			stack.pop_back();
			if (n >= m_world->scene.hierarchy.size() || !subtree.insert(n).second)
				continue;
			for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
				c = m_world->scene.hierarchy[c].nextSibling)
				stack.push_back(uint32_t(c));
		}
	}
	const tinygltf::Model& model = m_gltfModel;
	auto readUv = [&](int accessorIdx, size_t i, glm::vec2& out) -> bool {
		if (accessorIdx < 0 || accessorIdx >= int(model.accessors.size()))
			return false;
		const tinygltf::Accessor& acc = model.accessors[size_t(accessorIdx)];
		if (i >= size_t(acc.count) ||
			acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
			return false;
		const tinygltf::BufferView& bv = model.bufferViews[size_t(acc.bufferView)];
		const tinygltf::Buffer& buf = model.buffers[size_t(bv.buffer)];
		const size_t stride = acc.ByteStride(bv);
		const size_t off = bv.byteOffset + acc.byteOffset + i * stride;
		if (off + 8 > buf.data.size())
			return false;
		const float* f = reinterpret_cast<const float*>(buf.data.data() + off);
		out = glm::vec2(f[0], f[1]);
		return true;
	};
	auto inside = [](glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
		const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
		if (std::fabs(d) < 1e-12f)
			return false;
		const float u = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / d;
		const float v = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / d;
		return u >= -1e-4f && v >= -1e-4f && (u + v) <= 1.0f + 1e-4f;
	};
	for (const auto& [node, ref] : m_primRef) {
		if (useSubtree && !subtree.contains(node))
			continue;
		if (ref.mesh < 0 || ref.prim < 0)
			continue;
		const tinygltf::Mesh& gm = model.meshes[size_t(ref.mesh)];
		if (ref.prim >= int(gm.primitives.size()))
			continue;
		const tinygltf::Primitive& prim = gm.primitives[size_t(ref.prim)];
		const std::string attr = lightmap ? "TEXCOORD_1" : "TEXCOORD_0";
		auto it = prim.attributes.find(attr);
		if (it == prim.attributes.end())
			continue;
		const int uvAcc = it->second;
		if (prim.indices >= 0) {
			const size_t tcount = size_t(model.accessors[size_t(prim.indices)].count) / 3;
			for (size_t t = 0; t < tcount; ++t) {
				uint32_t ix[3] = {};
				bool ok = true;
				for (int k = 0; k < 3; ++k)
					ok &= ReadIndex(model, prim.indices, t * 3 + k, ix[k]);
				if (!ok)
					continue;
				glm::vec2 uv[3];
				if (!readUv(uvAcc, ix[0], uv[0]) || !readUv(uvAcc, ix[1], uv[1]) ||
					!readUv(uvAcc, ix[2], uv[2]))
					continue;
				if (inside(puv, uv[0], uv[1], uv[2])) {
					SubHit h{};
					h.node = node;
					h.tri = uint32_t(t);
					// Nearest vert in UV space.
					float d0 = glm::distance(puv, uv[0]);
					float d1 = glm::distance(puv, uv[1]);
					float d2 = glm::distance(puv, uv[2]);
					h.vert = (d0 <= d1 && d0 <= d2) ? ix[0] : (d1 <= d2) ? ix[1] : ix[2];
					h.pos = glm::vec3(puv, 0.0f);
					return h;
				}
			}
		}
	}
	return std::nullopt;
}

void EditorApp::ApplySubSelection(const SubHit& h) {
	if (h.node == UINT32_MAX)
		return;
	SetSelection(h.node);
	m_revealNode = h.node;
	if (m_selMode == SelMode::Face) {
		m_selFaceNode = h.node;
		m_selFaceTri = h.tri;
		m_selFacePos = h.pos;
		m_selVertNode = UINT32_MAX;
		m_selVertIdx = UINT32_MAX;
		Log("Face select: node " + std::to_string(h.node) + " tri " +
			std::to_string(h.tri));
	}
	else if (m_selMode == SelMode::Vertex) {
		m_selVertNode = h.node;
		m_selVertIdx = h.vert;
		m_selVertPos = h.pos;
		m_selFaceNode = UINT32_MAX;
		m_selFaceTri = UINT32_MAX;
		Log("Vertex select: node " + std::to_string(h.node) + " vert " +
			std::to_string(h.vert));
	}
	else {
		ClearSubSelection();
	}
}

bool EditorApp::SelectedFaceUvs(glm::vec2 outUv[3], ViewMode mode) const {
	if (m_selFaceNode == UINT32_MAX || m_selFaceTri == UINT32_MAX || !m_hasGltfModel)
		return false;
	auto it = m_primRef.find(m_selFaceNode);
	if (it == m_primRef.end() || it->second.mesh < 0)
		return false;
	const tinygltf::Mesh& gm = m_gltfModel.meshes[it->second.mesh];
	if (it->second.prim >= int(gm.primitives.size()))
		return false;
	const tinygltf::Primitive& prim = gm.primitives[it->second.prim];
	const std::string attr =
		(mode == ViewMode::FlatUv1) ? "TEXCOORD_1" : "TEXCOORD_0";
	auto pa = prim.attributes.find(attr);
	if (pa == prim.attributes.end())
		return false;
	const int uvAcc = pa->second;
	const tinygltf::Model& model = m_gltfModel;
	auto readUv = [&](size_t i, glm::vec2& out) -> bool {
		if (uvAcc < 0 || uvAcc >= int(model.accessors.size()))
			return false;
		const tinygltf::Accessor& acc = model.accessors[size_t(uvAcc)];
		if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
			return false;
		const tinygltf::BufferView& bv = model.bufferViews[size_t(acc.bufferView)];
		const tinygltf::Buffer& buf = model.buffers[size_t(bv.buffer)];
		const size_t stride = acc.ByteStride(bv);
		const size_t off = bv.byteOffset + acc.byteOffset + i * stride;
		if (off + 8 > buf.data.size())
			return false;
		const float* f = reinterpret_cast<const float*>(buf.data.data() + off);
		out = glm::vec2(f[0], f[1]);
		return true;
	};
	uint32_t ix[3] = {};
	if (prim.indices >= 0) {
		for (int k = 0; k < 3; ++k)
			if (!ReadIndex(model, prim.indices, size_t(m_selFaceTri) * 3 + k, ix[k]))
				return false;
	}
	else {
		ix[0] = m_selFaceTri * 3;
		ix[1] = ix[0] + 1;
		ix[2] = ix[0] + 2;
	}
	return readUv(ix[0], outUv[0]) && readUv(ix[1], outUv[1]) &&
		readUv(ix[2], outUv[2]);
}

bool EditorApp::SelectedFaceWorld(glm::vec3 outW[3]) const {
	// World positions of the selected triangle (brute-force read, no
	// acceleration structures; selection only, never edited).
	if (m_selFaceNode == UINT32_MAX || m_selFaceTri == UINT32_MAX ||
		!m_hasGltfModel || !m_world)
		return false;
	if (m_selFaceNode >= m_world->scene.hierarchy.size())
		return false;
	auto it = m_primRef.find(m_selFaceNode);
	if (it == m_primRef.end() || it->second.mesh < 0)
		return false;
	const tinygltf::Mesh& gm = m_gltfModel.meshes[it->second.mesh];
	if (it->second.prim >= int(gm.primitives.size()))
		return false;
	const tinygltf::Primitive& prim = gm.primitives[it->second.prim];
	auto pa = prim.attributes.find("POSITION");
	if (pa == prim.attributes.end())
		return false;
	const size_t vcount = m_gltfModel.accessors[pa->second].count;
	uint32_t ix[3] = {};
	if (prim.indices >= 0) {
		for (int k = 0; k < 3; ++k)
			if (!ReadIndex(m_gltfModel, prim.indices,
					size_t(m_selFaceTri) * 3 + k, ix[k]))
				return false;
	}
	else {
		ix[0] = m_selFaceTri * 3;
		ix[1] = ix[0] + 1;
		ix[2] = ix[0] + 2;
	}
	if (ix[0] >= vcount || ix[1] >= vcount || ix[2] >= vcount)
		return false;
	const glm::mat4& model = m_world->scene.globalTransform[m_selFaceNode];
	for (int k = 0; k < 3; ++k) {
		glm::vec3 local;
		if (!ReadPosition(m_gltfModel, pa->second, ix[k], local))
			return false;
		outW[k] = glm::vec3(model * glm::vec4(local, 1.0f));
	}
	return true;
}

bool EditorApp::SelectedVertWorld(glm::vec3& outW) const {
	if (m_selVertNode == UINT32_MAX || m_selVertIdx == UINT32_MAX ||
		!m_hasGltfModel || !m_world)
		return false;
	if (m_selVertNode >= m_world->scene.hierarchy.size())
		return false;
	auto it = m_primRef.find(m_selVertNode);
	if (it == m_primRef.end() || it->second.mesh < 0)
		return false;
	const tinygltf::Mesh& gm = m_gltfModel.meshes[it->second.mesh];
	if (it->second.prim >= int(gm.primitives.size()))
		return false;
	const tinygltf::Primitive& prim = gm.primitives[it->second.prim];
	auto pa = prim.attributes.find("POSITION");
	if (pa == prim.attributes.end())
		return false;
	if (size_t(m_selVertIdx) >= size_t(m_gltfModel.accessors[pa->second].count))
		return false;
	glm::vec3 local;
	if (!ReadPosition(m_gltfModel, pa->second, m_selVertIdx, local))
		return false;
	outW = glm::vec3(
		m_world->scene.globalTransform[m_selVertNode] * glm::vec4(local, 1.0f));
	return true;
}

bool EditorApp::SelectedVertUv(glm::vec2& outUv, ViewMode mode) const {
	if (m_selVertNode == UINT32_MAX || m_selVertIdx == UINT32_MAX || !m_hasGltfModel)
		return false;
	auto it = m_primRef.find(m_selVertNode);
	if (it == m_primRef.end() || it->second.mesh < 0)
		return false;
	const tinygltf::Mesh& gm = m_gltfModel.meshes[it->second.mesh];
	if (it->second.prim >= int(gm.primitives.size()))
		return false;
	const tinygltf::Primitive& prim = gm.primitives[it->second.prim];
	const std::string attr =
		(mode == ViewMode::FlatUv1) ? "TEXCOORD_1" : "TEXCOORD_0";
	auto pa = prim.attributes.find(attr);
	if (pa == prim.attributes.end())
		return false;
	const int uvAcc = pa->second;
	const tinygltf::Model& model = m_gltfModel;
	if (uvAcc < 0 || uvAcc >= int(model.accessors.size()))
		return false;
	const tinygltf::Accessor& acc = model.accessors[size_t(uvAcc)];
	if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
		return false;
	if (size_t(m_selVertIdx) >= size_t(acc.count))
		return false;
	const tinygltf::BufferView& bv = model.bufferViews[size_t(acc.bufferView)];
	const tinygltf::Buffer& buf = model.buffers[size_t(bv.buffer)];
	const size_t stride = acc.ByteStride(bv);
	const size_t off = bv.byteOffset + acc.byteOffset + size_t(m_selVertIdx) * stride;
	if (off + 8 > buf.data.size())
		return false;
	const float* f = reinterpret_cast<const float*>(buf.data.data() + off);
	outUv = glm::vec2(f[0], f[1]);
	return true;
}

} // namespace editor
