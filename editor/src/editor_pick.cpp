// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU ray picking: mouse -> world ray -> closest mesh node.
//
// Mouse/extent are VIEWPORT-LOCAL (the central rect between the panels;
// the projection is built for that aspect). Exact triangle tests read the
// CLUSTER's retained uploads (Cluster::GetRetainedUpload — the same CPU
// geometry the cook/bake paths consume) through the world's asset
// registry (meshLocations); nodes whose cluster did not retain geometry
// fall back to AABB picking.

#include "editor_app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_set>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/renderview.h>

namespace editor {
namespace {

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

// ---- retained-geometry lookup (the editor's single CPU source) ---------
// The editor retains uploads on its world cluster (source imports), so
// the world's asset registry resolves node -> mesh -> CPU geometry with
// no importer-side state left alive.

const smvk::StaticMeshUpload* EditorApp::MeshUploadOf(
	uint32_t worldMeshIdx) const {
	if (!m_world || worldMeshIdx >= m_world->meshLocations.size())
		return nullptr;
	const smvk::World::MeshLocation& loc =
		m_world->meshLocations[worldMeshIdx];
	if (!loc.cluster || !loc.handle.valid())
		return nullptr;
	return loc.cluster->GetRetainedUpload(loc.handle);
}

const smvk::StaticMeshUpload* EditorApp::NodeMeshUpload(
	uint32_t node) const {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return nullptr;
	const auto it = m_world->scene.meshForNode.find(node);
	if (it == m_world->scene.meshForNode.end())
		return nullptr;
	return MeshUploadOf(it->second);
}

glm::vec2 EditorApp::UvToFlatNdc(glm::vec2 uv, float zoom, glm::vec2 pan) const {
	// Must match uvflat.vert.hlsli: uvT = (uv-0.5)*zoom + 0.5 + pan,
	// then NDC = uvT*2-1. glTF image space (0,0) top-left maps to NDC
	// (-1,-1) at screen top (Vulkan Y-flip handled by the viewport).
	const float z = (zoom > 1e-6f) ? zoom : 1.0f;
	const glm::vec2 t = (uv - glm::vec2(0.5f)) * z + glm::vec2(0.5f) + pan;
	return t * 2.0f - glm::vec2(1.0f);
}

glm::vec2 EditorApp::FlatNdcToUv(glm::vec2 ndc, float zoom, glm::vec2 pan) const {
	const float z = (zoom > 1e-6f) ? zoom : 1.0f;
	const glm::vec2 t = (ndc + glm::vec2(1.0f)) * 0.5f;
	return (t - glm::vec2(0.5f) - pan) / z + glm::vec2(0.5f);
}

// UV-window indexed shorthands RETIRED with the canvas (they read the
// commented m_uvWinZoom/m_uvWinPan nav state).
//glm::vec2 EditorApp::UvToFlatNdc(glm::vec2 uv) const {
//	return UvToFlatNdc(uv, m_uvWinZoom, m_uvWinPan);
//}
//glm::vec2 EditorApp::FlatNdcToUv(glm::vec2 ndc) const {
//	return FlatNdcToUv(ndc, m_uvWinZoom, m_uvWinPan);
//}

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
	// NOTE: locals are nearPt/farPt, not near/far — the latter collide
	// with macros pulled in through the Windows SDK headers.
	const glm::vec4 nearPt = glm::inverse(viewProj) * glm::vec4(nx, ny, 1.0f, 1.0f);
	const glm::vec4 farPt = glm::inverse(viewProj) * glm::vec4(nx, ny, 0.0f, 1.0f);
	const glm::vec3 n = glm::vec3(nearPt) / nearPt.w;
	const glm::vec3 f = glm::vec3(farPt) / farPt.w;
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
		const smvk::StaticMeshUpload* up = MeshUploadOf(item.mesh);
		if (up && !up->vertices.empty()) {
			// Exact triangle walk over the retained CPU geometry (the
			// same data the GPU draws — node-local positions mapped by
			// the node's global transform).
			const size_t vcount = up->vertices.size();
			glm::vec3 a, b, cc;
			auto vAt = [&](uint32_t idx, glm::vec3& out) {
				if (idx >= vcount)
					return false;
				out = glm::vec3(model * glm::vec4(up->vertices[idx].pos, 1.0f));
				return true;
			};
			if (!up->indices.empty()) {
				const size_t tcount = up->indices.size() / 3;
				for (size_t t = 0; t < tcount; ++t) {
					const uint32_t ix[3] = {
						up->indices[t * 3 + 0],
						up->indices[t * 3 + 1],
						up->indices[t * 3 + 2] };
					if (!vAt(ix[0], a) || !vAt(ix[1], b) || !vAt(ix[2], cc))
						continue;
					const float th = RayTri(origin, dir, a, b, cc);
					if (th > 0.0f && th < tHit)
						tHit = th;
				}
			}
			else {
				// Non-indexed: vertices ARE the triangle list.
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
		else {
			// AABB fallback (unretained geometry).
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

// ---- gizmo-marker picking (nodes with no mesh to raycast) -------------
// Lights, GI volume centers and cubemap pins draw as X-ray markers with no
// geometry behind them, so the triangle walk can never hit them. This
// projects each marker center to viewport pixels and takes the nearest
// one inside the threshold — the click twin of what the eye sees. Only
// markers whose View-menu toggle is on participate (hidden kinds are not
// clickable, exactly like they are not drawn).
std::optional<EditorApp::PickHit> EditorApp::PickGizmoMarker(int vpIdx,
	float mouseX, float mouseY, smvk::Extent2D extent) const {
	// MODAL: markers only draw (and only pick) in LIGHTING mode.
	if (m_mode != EditorMode::Lighting)
		return std::nullopt;
	if (!m_world || m_world->scene.hierarchy.empty())
		return std::nullopt;
	if (extent.width == 0 || extent.height == 0)
		return std::nullopt;
	const glm::mat4 viewProj =
		m_viewports[std::clamp(vpIdx, 0, kMaxViewports - 1)].viewProj;
	glm::vec3 origin;
	const glm::vec3 dir = MouseRay(mouseX, mouseY, extent, viewProj,
		origin);
	const Scene& scene = m_world->scene;
	PickHit best{};
	best.t = FLT_MAX;
	bool found = false;
	static constexpr float kPxThreshold = 14.0f;
	auto consider = [&](uint32_t node, const glm::vec3& center) {
		if (node >= scene.hierarchy.size()
			|| node >= scene.globalTransform.size())
			return;
		if (!IsVisibleNode(scene, int(node)))
			return; // hidden subtrees are not pickable
		const float t = glm::dot(center - origin, dir);
		if (!(t > 0.0f) || t >= best.t)
			return; // behind the camera, or farther than the best
		const glm::vec4 c = viewProj * glm::vec4(center, 1.0f);
		if (!(c.w > 1e-6f))
			return;
		// Same NDC->pixel convention as the overlay projection (no
		// Y-flip on either side, so the threshold test is consistent).
		const float px = (c.x / c.w * 0.5f + 0.5f) * float(extent.width);
		const float py = (c.y / c.w * 0.5f + 0.5f) * float(extent.height);
		const float dx = px - mouseX, dy = py - mouseY;
		if (dx * dx + dy * dy > kPxThreshold * kPxThreshold)
			return;
		best.node = node;
		best.t = t;
		found = true;
	};
	if (m_showLightGizmos) {
		for (const auto& [node, li] : scene.lightForNode) {
			(void)li;
			if (node >= scene.globalTransform.size())
				continue;
			consider(node, glm::vec3(scene.globalTransform[node][3]));
		}
	}
	if (m_showGiVolumeGizmos) {
		for (const auto& [node, def] : scene.giVolumeForNode) {
			(void)def;
			glm::vec3 mn, mx;
			if (!GiVolumeBox(node, mn, mx))
				continue;
			consider(node, (mn + mx) * 0.5f);
		}
	}
	if (m_showCubemapGizmos) {
		for (const auto& [node, def] : scene.cubemapForNode) {
			(void)def;
			if (node >= scene.globalTransform.size())
				continue;
			consider(node, glm::vec3(scene.globalTransform[node][3]));
		}
	}
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
		const smvk::StaticMeshUpload* up = MeshUploadOf(item.mesh);
		if (!up || up->vertices.empty() || up->indices.empty())
			return; // no exact geometry: no sub-object info
		const size_t vcount = up->vertices.size();
		auto vAt = [&](uint32_t idx, glm::vec3& out) {
			if (idx >= vcount)
				return false;
			out = glm::vec3(model * glm::vec4(up->vertices[idx].pos, 1.0f));
			return true;
		};
		const size_t tcount = up->indices.size() / 3;
		for (size_t t = 0; t < tcount; ++t) {
			const uint32_t ix[3] = {
				up->indices[t * 3 + 0],
				up->indices[t * 3 + 1],
				up->indices[t * 3 + 2] };
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
	});
	if (!found)
		return std::nullopt;
	return best;
}

#if 0 // Flat-UV window pick retired with the UV editor canvas
std::optional<EditorApp::SubHit> EditorApp::PickFlatUvWindow(float mouseX,
	float mouseY) const {
	// Flat-UV window pick (brute force, no accel): mouse -> NDC -> UV
	// (inverse zoom/pan) -> point-in-triangle over the selection subtree
	// (or the whole scene when nothing is selected). Mouse is
	// canvas-local (the caller subtracts the canvas origin).
	if (!m_world || !m_showUvEditor)
		return std::nullopt;
	const float w = m_uvWinRectMax.x - m_uvWinRectMin.x;
	const float h = m_uvWinRectMax.y - m_uvWinRectMin.y;
	if (w <= 0.0f || h <= 0.0f)
		return std::nullopt;
	const bool lightmap = (m_uvWinMode == ViewMode::FlatUv1);
	const glm::vec2 ndc(mouseX / w * 2.0f - 1.0f,
		mouseY / h * 2.0f - 1.0f);
	const glm::vec2 puv = FlatNdcToUv(ndc, m_uvWinZoom, m_uvWinPan);
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
	auto inside = [](glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
		const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
		if (std::fabs(d) < 1e-12f)
			return false;
		const float u = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / d;
		const float v = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / d;
		return u >= -1e-4f && v >= -1e-4f && (u + v) <= 1.0f + 1e-4f;
	};
	for (const auto& [node, meshIdx] : m_world->scene.meshForNode) {
		if (useSubtree && !subtree.contains(node))
			continue;
		const smvk::StaticMeshUpload* up = MeshUploadOf(meshIdx);
		if (!up || up->vertices.empty() || up->indices.empty())
			continue;
		const size_t tcount = up->indices.size() / 3;
		for (size_t t = 0; t < tcount; ++t) {
			const uint32_t ix[3] = {
				up->indices[t * 3 + 0],
				up->indices[t * 3 + 1],
				up->indices[t * 3 + 2] };
			glm::vec2 uv[3];
			for (int k = 0; k < 3; ++k) {
				if (ix[k] >= up->vertices.size())
					break;
				// UV channel: TEXCOORD_0 (diffuse) vs TEXCOORD_1 (the
				// lightmap chart-local uv).
				uv[k] = lightmap ? up->vertices[ix[k]].texCoord2
					: up->vertices[ix[k]].texCoord;
			}
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
	return std::nullopt;
}
#endif // Flat-UV window pick retired

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
	if (m_selFaceNode == UINT32_MAX || m_selFaceTri == UINT32_MAX)
		return false;
	const smvk::StaticMeshUpload* up = NodeMeshUpload(m_selFaceNode);
	if (!up || m_selFaceTri >= up->indices.size() / 3)
		return false;
	const bool lightmap = (mode == ViewMode::FlatUv1);
	const uint32_t ix[3] = {
		up->indices[size_t(m_selFaceTri) * 3 + 0],
		up->indices[size_t(m_selFaceTri) * 3 + 1],
		up->indices[size_t(m_selFaceTri) * 3 + 2] };
	for (int k = 0; k < 3; ++k) {
		if (ix[k] >= up->vertices.size())
			return false;
		outUv[k] = lightmap ? up->vertices[ix[k]].texCoord2
			: up->vertices[ix[k]].texCoord;
	}
	return true;
}

bool EditorApp::SelectedFaceWorld(glm::vec3 outW[3]) const {
	// World positions of the selected triangle (brute-force read, no
	// acceleration structures; selection only, never edited).
	if (m_selFaceNode == UINT32_MAX || m_selFaceTri == UINT32_MAX ||
		!m_world || m_selFaceNode >= m_world->scene.hierarchy.size())
		return false;
	const smvk::StaticMeshUpload* up = NodeMeshUpload(m_selFaceNode);
	if (!up || m_selFaceTri >= up->indices.size() / 3)
		return false;
	const uint32_t ix[3] = {
		up->indices[size_t(m_selFaceTri) * 3 + 0],
		up->indices[size_t(m_selFaceTri) * 3 + 1],
		up->indices[size_t(m_selFaceTri) * 3 + 2] };
	const glm::mat4& model = m_world->scene.globalTransform[m_selFaceNode];
	for (int k = 0; k < 3; ++k) {
		if (ix[k] >= up->vertices.size())
			return false;
		outW[k] = glm::vec3(model * glm::vec4(up->vertices[ix[k]].pos, 1.0f));
	}
	return true;
}

bool EditorApp::SelectedVertWorld(glm::vec3& outW) const {
	if (m_selVertNode == UINT32_MAX || m_selVertIdx == UINT32_MAX ||
		!m_world || m_selVertNode >= m_world->scene.hierarchy.size())
		return false;
	const smvk::StaticMeshUpload* up = NodeMeshUpload(m_selVertNode);
	if (!up || m_selVertIdx >= up->vertices.size())
		return false;
	outW = glm::vec3(
		m_world->scene.globalTransform[m_selVertNode]
			* glm::vec4(up->vertices[m_selVertIdx].pos, 1.0f));
	return true;
}

bool EditorApp::SelectedVertUv(glm::vec2& outUv, ViewMode mode) const {
	if (m_selVertNode == UINT32_MAX || m_selVertIdx == UINT32_MAX)
		return false;
	const smvk::StaticMeshUpload* up = NodeMeshUpload(m_selVertNode);
	if (!up || m_selVertIdx >= up->vertices.size())
		return false;
	outUv = (mode == ViewMode::FlatUv1)
		? up->vertices[m_selVertIdx].texCoord2
		: up->vertices[m_selVertIdx].texCoord;
	return true;
}

} // namespace editor
