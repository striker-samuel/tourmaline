// Selection gizmo: screen-space axis handles drawn as debug lines +
// mouse interaction (translate/rotate/scale). Rotate/scale work in the
// node's LOCAL frame, translate in WORLD axes (noted in the overlay).

#include "editor_app.h"

#include <cfloat>
#include <cmath>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <Engine/Renderer/mesh.h>

namespace editor {
namespace {

struct LineBatch {
	glm::vec4 color{ 1, 1, 1, 1 };
	bool depthTest = false; // grid/flat wires: occluded by geometry
	std::vector<smvk::DebugLineVertex> verts;
	// Flat UV wire batches: drawn ONLY in viewport flatVp (whose view
	// mode is flatMode), pushed through an IDENTITY viewProj (verts are
	// already in NDC space — see BuildFlatUvWires + pane nav). 3D
	// batches skip flat panels.
	bool flat = false;
	ViewMode flatMode = ViewMode::Lit;
	int flatVp = -1; // viewport index for flat batches (per-pane nav)
};

glm::vec2 ToScreen(const glm::vec3& w, const glm::mat4& vp,
	smvk::Extent2D extent) {
	const glm::vec4 c = vp * glm::vec4(w, 1.0f);
	const float inv = (std::fabs(c.w) > 1e-9f) ? 1.0f / c.w : 0.0f;
	return glm::vec2((c.x * inv * 0.5f + 0.5f) * float(extent.width),
		(c.y * inv * 0.5f + 0.5f) * float(extent.height));
}

float DistPointSeg(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
	const glm::vec2 ab = b - a;
	const float len2 = glm::dot(ab, ab);
	const float t = (len2 > 1e-9f)
		? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
	return glm::length(p - (a + ab * t));
}

// Point in convex screen-space quad (same-side test, edge-inclusive
// so glancing-angle thin quads stay grabbable) — plane-handle picking.
bool PointInQuad(glm::vec2 p, glm::vec2 q0, glm::vec2 q1, glm::vec2 q2,
	glm::vec2 q3) {
	auto side = [](glm::vec2 a, glm::vec2 b, glm::vec2 c) {
		return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
	};
	const float s0 = side(q0, q1, p), s1 = side(q1, q2, p);
	const float s2 = side(q2, q3, p), s3 = side(q3, q0, p);
	return (s0 >= 0.0f && s1 >= 0.0f && s2 >= 0.0f && s3 >= 0.0f)
		|| (s0 <= 0.0f && s1 <= 0.0f && s2 <= 0.0f && s3 <= 0.0f);
}

void AddCircle(LineBatch& b, const glm::vec3& center, const glm::vec3& u,
	const glm::vec3& v, float r, int segs = 48) {
	for (int i = 0; i < segs; ++i) {
		const float a0 = float(i) / segs * 6.2831853f;
		const float a1 = float(i + 1) / segs * 6.2831853f;
		b.verts.push_back({ center + u * std::cos(a0) * r + v * std::sin(a0) * r });
		b.verts.push_back({ center + u * std::cos(a1) * r + v * std::sin(a1) * r });
	}
}

void AddBox(LineBatch& b, const glm::vec3& mn, const glm::vec3& mx) {
	const glm::vec3 c[8] = {
		{ mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z },
		{ mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
		{ mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z },
		{ mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z } };
	static const int e[12][2] = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},
		{6,7},{7,4},{0,4},{1,5},{2,6},{3,7} };
	for (auto& k : e) {
		b.verts.push_back({ c[k[0]] });
		b.verts.push_back({ c[k[1]] });
	}
}

// Wire diamond (octahedron edges): symbolic marker core for lights.
void AddDiamond(LineBatch& b, const glm::vec3& p, float s) {
	const glm::vec3 t = p + glm::vec3(0, s, 0);
	const glm::vec3 bo = p - glm::vec3(0, s, 0);
	const glm::vec3 e[4] = { p + glm::vec3(s, 0, 0), p + glm::vec3(0, 0, s),
		p - glm::vec3(s, 0, 0), p - glm::vec3(0, 0, s) };
	for (int i = 0; i < 4; ++i) {
		b.verts.push_back({ t });
		b.verts.push_back({ e[i] });
		b.verts.push_back({ bo });
		b.verts.push_back({ e[i] });
	}
}

// Wire arrowhead: 4-sided pyramid with apex at `tip`, axis `ax`
// (unit), base `len` behind the tip, base half-width `r`.
void AddArrowHead(LineBatch& b, const glm::vec3& tip, const glm::vec3& ax,
	float len, float r) {
	const glm::vec3 bc = tip - ax * len;
	glm::vec3 u = std::fabs(ax.y) < 0.9f ? glm::cross(ax, glm::vec3(0, 1, 0))
		: glm::cross(ax, glm::vec3(1, 0, 0));
	u = glm::normalize(u);
	const glm::vec3 v = glm::cross(ax, u);
	glm::vec3 c[4];
	for (int i = 0; i < 4; ++i) {
		const float a = float(i) / 4 * 6.2831853f + 0.7853982f;
		c[i] = bc + (u * std::cos(a) + v * std::sin(a)) * r;
	}
	for (int i = 0; i < 4; ++i) {
		b.verts.push_back({ tip });
		b.verts.push_back({ c[i] });
		b.verts.push_back({ c[i] });
		b.verts.push_back({ c[(i + 1) % 4] });
	}
}

namespace {
// Small plain-data bundle so the grid builder stays file-local (the
// editor app owns the actual settings).
struct EditorAppViewGizmo {
	float cellSize = 1.0f;
	int extentCells = 40;
};
} // namespace

// Ground grid (XZ plane at y = 0): minor lines every m_gridSize, a
// stronger major line every 5 cells, axis-colored X/Z through the
// origin. Vertices are cheap enough to rebuild per frame alongside the
// other gizmo batches.
static void AddGrid(LineBatch& minor, LineBatch& major,
	const EditorAppViewGizmo& cfg) {
	const float cell = cfg.cellSize;
	const int n = int(cfg.extentCells);
	// Minor grid (both directions), skipping the axis lines.
	for (int i = -n; i <= n; ++i) {
		const float v = float(i) * cell;
		if (i % 5 == 0)
			continue; // major lines below
		minor.verts.push_back({ glm::vec3(-float(n) * cell, 0.0f, v) });
		minor.verts.push_back({ glm::vec3(float(n) * cell, 0.0f, v) });
		minor.verts.push_back({ glm::vec3(v, 0.0f, -float(n) * cell) });
		minor.verts.push_back({ glm::vec3(v, 0.0f, float(n) * cell) });
	}
	for (int i = -n; i <= n; i += 5) {
		const float v = float(i) * cell;
		major.verts.push_back({ glm::vec3(-float(n) * cell, 0.0f, v) });
		major.verts.push_back({ glm::vec3(float(n) * cell, 0.0f, v) });
		major.verts.push_back({ glm::vec3(v, 0.0f, -float(n) * cell) });
		major.verts.push_back({ glm::vec3(v, 0.0f, float(n) * cell) });
	}
}

glm::vec3 GizmoOrigin(const Scene& scene, uint32_t node) {
	return glm::vec3(scene.globalTransform[node][3]);
}

} // namespace

void EditorApp::RecordGizmoPass(smvk::LogicalImageViewHandle swapView) {
	if (swapView.empty() || !m_world)
		return;
	if (m_lineProgram.shaderCount != 2)
		return;
	SetupDefaultCameras();
	std::vector<LineBatch> batches;
	// Ground grid (XZ at y = 0): minor + major cells; the translate snap
	// step equals the cell size (Shift-drag).
	if (m_showGrid) {
		EditorAppViewGizmo cfg{};
		cfg.cellSize = std::max(m_gridSize, 0.01f);
		cfg.extentCells = int(std::clamp<uint32_t>(m_gridExtent, 2, 512));
		LineBatch minor, major;
		minor.color = glm::vec4(0.32f, 0.34f, 0.38f, 1.0f);
		minor.depthTest = true; // grid: hidden behind geometry
		major.color = glm::vec4(0.45f, 0.47f, 0.52f, 1.0f);
		major.depthTest = true;
		AddGrid(minor, major, cfg);
		if (!minor.verts.empty())
			batches.push_back(std::move(minor));
		if (!major.verts.empty())
			batches.push_back(std::move(major));
	}
	// Light markers: one small distance-scaled diamond per light node
	// (sun = yellow + emission arrow, point = orange, spot = cyan +
	// cone), always visible — light nodes carry no mesh to click.
	// Flat-UV viewports filter these ranges out at record time.
	// Flat UV wires PER VIEWPORT (each flat pane has its own zoom/pan):
	// unwrapped shell edges in NDC space (identity viewProj; the pane
	// nav transform is baked in via UvToFlatNdc). Panel wires cover the
	// SELECTION's subtree (UV editor style); the PNG export wires the
	// whole scene instead. Selection highlights (area mode) draw on top
	// in yellow/cyan.
	for (int vpIdx = 0; vpIdx < ViewportCount(); ++vpIdx) {
		const ViewMode vm = m_viewports[vpIdx].viewMode;
		if (vm != ViewMode::FlatUv0 && vm != ViewMode::FlatUv1)
			continue;
		LineBatch wires;
		wires.flat = true;
		wires.flatMode = vm;
		wires.flatVp = vpIdx;
		wires.color = (vm == ViewMode::FlatUv0)
			? glm::vec4(0.15f, 0.95f, 0.55f, 1.0f)
			: glm::vec4(0.35f, 0.7f, 1.0f, 1.0f);
		wires.depthTest = true; // over the flat fill (z=1 vs 0.9999)
		BuildFlatUvWires(wires.verts, vm, m_selected, vpIdx);
		if (!wires.verts.empty())
			batches.push_back(std::move(wires));
		// Area-mode selection highlight: selected face (yellow loop) or
		// vertex (cyan cross) in this pane's UV space.
		if (m_selMode == SelMode::Face && m_selFaceNode != UINT32_MAX) {
			glm::vec2 fuv[3];
			if (SelectedFaceUvs(fuv, vm)) {
				LineBatch hl;
				hl.flat = true;
				hl.flatMode = vm;
				hl.flatVp = vpIdx;
				hl.color = glm::vec4(1.0f, 0.9f, 0.2f, 1.0f);
				hl.depthTest = false; // on top of the shell wires
				for (int k = 0; k < 3; ++k) {
					const glm::vec2 na = UvToFlatNdc(fuv[k], vpIdx);
					const glm::vec2 nb = UvToFlatNdc(fuv[(k + 1) % 3], vpIdx);
					hl.verts.push_back({ glm::vec3(na.x, na.y, 1.0f) });
					hl.verts.push_back({ glm::vec3(nb.x, nb.y, 1.0f) });
				}
				batches.push_back(std::move(hl));
			}
		}
		else if (m_selMode == SelMode::Vertex && m_selVertNode != UINT32_MAX) {
			glm::vec2 vuv;
			if (SelectedVertUv(vuv, vm)) {
				LineBatch hl;
				hl.flat = true;
				hl.flatMode = vm;
				hl.flatVp = vpIdx;
				hl.color = glm::vec4(0.3f, 1.0f, 1.0f, 1.0f);
				hl.depthTest = false;
				const glm::vec2 nc = UvToFlatNdc(vuv, vpIdx);
				const float s = 0.03f;
				hl.verts.push_back({ glm::vec3(nc.x - s, nc.y, 1.0f) });
				hl.verts.push_back({ glm::vec3(nc.x + s, nc.y, 1.0f) });
				hl.verts.push_back({ glm::vec3(nc.x, nc.y - s, 1.0f) });
				hl.verts.push_back({ glm::vec3(nc.x, nc.y + s, 1.0f) });
				batches.push_back(std::move(hl));
			}
		}
	}
	if (m_showLightGizmos && !m_world->scene.lightForNode.empty()) {
		const glm::vec3 eye = EditorCamEye(ActiveCam());
		for (const auto& [node, li] : m_world->scene.lightForNode) {
			if (node >= m_world->scene.hierarchy.size() ||
				node >= m_world->scene.globalTransform.size())
				continue;
			glm::vec3 aimWs(0.0f);
			const ::SceneLight* l = FindNodeLight(node, &aimWs);
			if (!l)
				continue;
			const glm::vec3 p =
				glm::vec3(m_world->scene.globalTransform[node][3]);
			float s = glm::distance(eye, p) * 0.045f;
			if (!(s > 1e-5f))
				continue;
			if (node == m_selected)
				s *= 1.6f; // selected marker reads bigger
			LineBatch b;
			if (l->type == ::SceneLight::Type::Directional) {
				b.color = glm::vec4(1.0f, 0.85f, 0.3f, 1.0f);
				AddDiamond(b, p, s);
				if (glm::length(aimWs) > 1e-6f) {
					const glm::vec3 tip = p + aimWs * s * 3.0f;
					b.verts.push_back({ p });
					b.verts.push_back({ tip - aimWs * s * 0.66f });
					AddArrowHead(b, tip, aimWs, s * 0.66f, s * 0.18f);
				}
			}
			else if (l->type == ::SceneLight::Type::Spot) {
				b.color = glm::vec4(0.3f, 0.85f, 1.0f, 1.0f);
				AddDiamond(b, p, s);
				glm::vec3 ax = glm::length(aimWs) > 1e-6f
					? aimWs : glm::vec3(0, -1, 0);
				const float outer = std::clamp(l->outerConeAngle,
					0.05f, 1.4f);
				glm::vec3 u = std::fabs(ax.y) < 0.9f
					? glm::cross(ax, glm::vec3(0, 1, 0))
					: glm::cross(ax, glm::vec3(1, 0, 0));
				u = glm::normalize(u);
				const glm::vec3 v = glm::cross(ax, u);
				const float len = s * 2.5f;
				for (int i = 0; i < 4; ++i) {
					const float a = float(i) / 4 * 6.2831853f;
					const glm::vec3 dir = glm::normalize(
						ax * std::cos(outer) +
						(u * std::cos(a) + v * std::sin(a)) * std::sin(outer));
					b.verts.push_back({ p });
					b.verts.push_back({ p + dir * len });
				}
				AddCircle(b, p + ax * len, u, v, len * std::sin(outer), 24);
			}
			else {
				b.color = glm::vec4(1.0f, 0.55f, 0.2f, 1.0f);
				AddDiamond(b, p, s);
			}
			if (node == m_selected && l->range > 0.0f &&
				l->type != ::SceneLight::Type::Directional)
				AddCircle(b, p, glm::vec3(1, 0, 0),
					glm::vec3(0, 0, 1), l->range, 48);
			if (!b.verts.empty())
				batches.push_back(std::move(b));
		}
	}
	// Convex-volume boxes: one X-ray wire box per cvolume node (shape =
	// node translate/scale), priority-hued like the GI preview overlay
	// so authored and baked volumes read the same; the selection draws
	// bright. Always visible — cvolume nodes carry no mesh to click.
	if (m_showCvolGizmos && !m_world->scene.cvolForNode.empty()) {
		const auto priorityRgb = [](uint32_t priority) {
			// HSV(h, 0.7, 1.0) with h = priority * 0.13 mod 1 (same
			// hue as DrawCvolPreviewOverlay's priorityColor).
			const float h = std::fmod(float(priority) * 0.13f, 1.0f);
			const float s = 0.7f, v = 1.0f;
			const float c = v * s;
			const float x = c * (1.0f - std::fabs(
				std::fmod(h * 6.0f, 2.0f) - 1.0f));
			const float m = v - c;
			float r = 0, g = 0, b = 0;
			const int sector = int(h * 6.0f) % 6;
			if (sector == 0) { r = c; g = x; }
			else if (sector == 1) { r = x; g = c; }
			else if (sector == 2) { g = c; b = x; }
			else if (sector == 3) { g = x; b = c; }
			else if (sector == 4) { r = x; b = c; }
			else { r = c; b = x; }
			return glm::vec3(r + m, g + m, b + m);
		};
		for (const auto& [node, def] : m_world->scene.cvolForNode) {
			glm::vec3 mn, mx;
			if (!CvolBox(node, mn, mx))
				continue;
			LineBatch b;
			b.depthTest = false; // X-ray: author shapes against walls
			if (node == m_selected)
				b.color = glm::vec4(1.0f, 0.9f, 0.3f, 1.0f);
			else
				b.color = glm::vec4(priorityRgb(def.priority), 1.0f);
			AddBox(b, mn, mx);
			// Center anchor diamond (the box itself has no mesh to
			// click; the marker reads at a glance like light nodes).
			const glm::vec3 c = (mn + mx) * 0.5f;
			const float s = std::max(
				glm::length(mx - mn) * 0.02f, 0.05f);
			AddDiamond(b, c, s);
			if (!b.verts.empty())
				batches.push_back(std::move(b));
		}
	}
	// Build batches: selection AABB (yellow) + gizmo axes.
	if (m_selected == UINT32_MAX ||
		m_selected >= m_world->scene.hierarchy.size()) {
		// No selection: markers (if any) still draw below.
	}
	else {
	{
		LineBatch box;
		box.color = glm::vec4(1.0f, 0.9f, 0.2f, 1.0f);
		// Union AABB over the selected subtree's mesh nodes (via drawItems).
		bool has = false;
		glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
		std::vector<uint32_t> stack{ m_selected };
		std::unordered_set<uint32_t> inSubtree;
		while (!stack.empty()) {
			const uint32_t n = stack.back();
			stack.pop_back();
			if (n >= m_world->scene.hierarchy.size() || !inSubtree.insert(n).second)
				continue;
			for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
				c = m_world->scene.hierarchy[c].nextSibling)
				stack.push_back((uint32_t)c);
		}
		for (const auto& item : m_world->drawItems) {
			if (!inSubtree.contains(item.node))
				continue;
			smvk::StaticMesh* m = m_world->GetMesh(item.mesh);
			if (!m)
				continue;
			const glm::mat4& model = m_world->scene.globalTransform[item.node];
			for (int k = 0; k < 8; ++k) {
				const glm::vec3 corner(
					(k & 1) ? m->geometry.aabbMax.x : m->geometry.aabbMin.x,
					(k & 2) ? m->geometry.aabbMax.y : m->geometry.aabbMin.y,
					(k & 4) ? m->geometry.aabbMax.z : m->geometry.aabbMin.z);
				const glm::vec3 w = glm::vec3(model * glm::vec4(corner, 1.0f));
				mn = glm::min(mn, w);
				mx = glm::max(mx, w);
				has = true;
			}
		}
		if (has) {
			AddBox(box, mn, mx);
			batches.push_back(std::move(box));
		}
	}

	const glm::vec3 O = GizmoOrigin(m_world->scene, m_selected);
	const float dist = glm::distance(EditorCamEye(ActiveCam()), O);
	// Configurable gizmo size (m_gizmoScale, default half the original
	// 0.14 factor); tiny floors keep the handles clickable.
	const float s = std::max(dist * m_gizmoScale, 1e-4f);
	const glm::mat4& global = m_world->scene.globalTransform[m_selected];
	const glm::vec3 axesW[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
	glm::vec3 axesL[3];
	for (int i = 0; i < 3; ++i) {
		axesL[i] = glm::normalize(glm::vec3(global * glm::vec4(axesW[i], 0.0f)));
		if (!std::isfinite(axesL[i].x))
			axesL[i] = axesW[i];
	}
	const glm::vec4 axisColors[3] = {
		{1, 0.25f, 0.25f, 1}, {0.3f, 1, 0.3f, 1}, {0.35f, 0.5f, 1, 1} };
	const bool useLocal = (m_gizmoMode != GizmoMode::Translate);
	for (int i = 0; i < 3; ++i) {
		const glm::vec3 ax = (useLocal ? axesL[i] : axesW[i]);
		LineBatch b;
		b.color = (m_gizmoDragging && m_gizmoAxis == i)
			? glm::vec4(1, 1, 0, 1) : axisColors[i];
		if (m_gizmoMode == GizmoMode::Rotate) {
			const glm::vec3 u = glm::normalize(
				std::fabs(ax.y) < 0.9f ? glm::cross(ax, glm::vec3(0, 1, 0))
				: glm::cross(ax, glm::vec3(1, 0, 0)));
			const glm::vec3 v = glm::cross(ax, u);
			AddCircle(b, O, u, v, s);
		}
		else if (m_gizmoMode == GizmoMode::Translate) {
			// Maya-style arrow: shaft + wire pyramid head at the tip.
			const glm::vec3 tip = O + ax * s;
			b.verts.push_back({ O });
			b.verts.push_back({ tip - ax * s * 0.22f });
			AddArrowHead(b, tip, ax, s * 0.22f, s * 0.06f);
		}
		else {
			// Maya-style scale: shaft + wire cube handle at the tip.
			const glm::vec3 tip = O + ax * s;
			b.verts.push_back({ O });
			b.verts.push_back({ tip });
			const glm::vec3 h(s * 0.055f);
			AddBox(b, tip - h, tip + h);
		}
		batches.push_back(std::move(b));
		}
		if (m_gizmoMode != GizmoMode::Rotate) {
			// Plane handles (two-axis drags): wire quads spanned by each
			// axis pair at 0.35*s, tinted by the FIXED (normal) axis —
			// the axis that does not move. Translate planes live in the
			// world frame, scale planes in the node's local frame.
			const float qd = s * 0.35f;
			for (int n = 0; n < 3; ++n) {
				const int a = (n + 1) % 3, b = (n + 2) % 3;
				const glm::vec3 axA =
					(m_gizmoMode == GizmoMode::Translate ? axesW[a] : axesL[a]);
				const glm::vec3 axB =
					(m_gizmoMode == GizmoMode::Translate ? axesW[b] : axesL[b]);
				LineBatch pb;
				pb.color = (m_gizmoDragging && m_gizmoAxis == 3 + n)
					? glm::vec4(1, 1, 0, 1) : axisColors[n];
				const glm::vec3 p00 = O;
				const glm::vec3 p10 = O + axA * qd;
				const glm::vec3 p11 = O + axA * qd + axB * qd;
				const glm::vec3 p01 = O + axB * qd;
				pb.verts.push_back({ p00 });
				pb.verts.push_back({ p10 });
				pb.verts.push_back({ p10 });
				pb.verts.push_back({ p11 });
				pb.verts.push_back({ p11 });
				pb.verts.push_back({ p01 });
				pb.verts.push_back({ p01 });
				pb.verts.push_back({ p00 });
				batches.push_back(std::move(pb));
			}
		}
		if (m_gizmoMode != GizmoMode::Rotate) {
			// Uniform center cube: all three axes at once (translate =
			// full 3D move, scale = uniform factor). Idle light gray,
			// yellow while dragged.
			LineBatch cb;
			cb.color = (m_gizmoDragging && m_gizmoAxis == 7)
				? glm::vec4(1, 1, 0, 1) : glm::vec4(0.75f, 0.75f, 0.8f, 1);
			const glm::vec3 ch(s * 0.055f);
			AddBox(cb, O - ch, O + ch);
			batches.push_back(std::move(cb));
		}
		// Sub-object highlight (3D viewports, wire mode included): the
		// selected triangle (yellow loop, on top) or vertex (cyan diamond
		// + cross, on top). Selection only — never edits geometry.
		if (m_selMode == SelMode::Face && m_selFaceNode != UINT32_MAX) {
			glm::vec3 w[3];
			if (SelectedFaceWorld(w)) {
				LineBatch hl;
				hl.color = glm::vec4(1.0f, 0.9f, 0.2f, 1.0f);
				hl.depthTest = false;
				for (int k = 0; k < 3; ++k) {
					hl.verts.push_back({ w[k] });
					hl.verts.push_back({ w[(k + 1) % 3] });
				}
				batches.push_back(std::move(hl));
			}
		}
		else if (m_selMode == SelMode::Vertex && m_selVertNode != UINT32_MAX) {
			glm::vec3 p;
			if (SelectedVertWorld(p)) {
				LineBatch hl;
				hl.color = glm::vec4(0.3f, 1.0f, 1.0f, 1.0f);
				hl.depthTest = false;
				const float s = glm::distance(EditorCamEye(ActiveCam()), p) *
					0.02f + 0.02f;
				AddDiamond(hl, p, s * 0.5f);
				hl.verts.push_back({ p + glm::vec3(-s, 0, 0) });
				hl.verts.push_back({ p + glm::vec3(s, 0, 0) });
				hl.verts.push_back({ p + glm::vec3(0, -s, 0) });
				hl.verts.push_back({ p + glm::vec3(0, s, 0) });
				hl.verts.push_back({ p + glm::vec3(0, 0, -s) });
				hl.verts.push_back({ p + glm::vec3(0, 0, s) });
				batches.push_back(std::move(hl));
			}
		}
	}

	// Flatten into one upload with per-batch draw ranges. Grid + flat-UV
	// wire ranges are DEPTH-TESTED against the viewport's depth (the
	// grid hides behind geometry; flat wires land over the UV fill);
	// every helper (gizmo axes, AABB, light markers, face/vertex
	// highlights) draws on top like a DCC overlay. Flat ranges render
	// through an identity viewProj and only in their OWN viewport (each
	// flat pane has its own zoom/pan).
	struct Range { uint32_t first, count; glm::vec4 color;
		bool depthTest; bool flat; ViewMode flatMode; int flatVp; };
	std::vector<Range> ranges;
	std::vector<smvk::DebugLineVertex> all;
	for (auto& b : batches) {
		if (b.verts.empty())
			continue;
		ranges.push_back({ uint32_t(all.size()), uint32_t(b.verts.size()),
			b.color, b.depthTest, b.flat, b.flatMode, b.flatVp });
		all.insert(all.end(), b.verts.begin(), b.verts.end());
	}
	if (all.empty())
		return;
	const size_t bytes = all.size() * sizeof(all[0]);
	if (m_gizmoLines.empty() || m_gizmoLinesCap < uint32_t(all.size())) {
		m_gizmoLines = m_context->CreateBuffer(bytes,
			smvk::BufferType_Vertex, "editor gizmo lines");
		m_gizmoLinesCap = uint32_t(all.size());
	}
	if (m_gizmoLines.empty())
		return;
	m_context->UploadDataToBuffer(m_gizmoLines, all.data(), bytes,
		smvk::PipelineStageBit_Vertex, smvk::AccessFlags::VertexAttributeRead);

	// One line pass PER VISIBLE VIEWPORT: same uploaded batches, each
	// viewport's own viewProj + rect. Flat-UV panels draw ONLY their
	// matching flat wire ranges through an IDENTITY viewProj; 3D panels
	// draw only the non-flat ranges with the viewport's viewProj.
	for (int vpIdx = 0; vpIdx < ViewportCount(); ++vpIdx) {
	const ViewportRect& vp = m_viewports[vpIdx].rect;
	const glm::mat4 vpViewProj = m_viewports[vpIdx].viewProj;
	const ViewMode vpMode = m_viewports[vpIdx].viewMode;
	const bool vpIsFlat =
		(vpMode == ViewMode::FlatUv0 || vpMode == ViewMode::FlatUv1);
	const glm::mat4 flatIdentity(1.0f);
	auto& pass = m_rendergraph->AddPass("EditorGizmo")
		.SetOffset(uint32_t(vp.x), uint32_t(vp.y))
		.SetExtent(smvk::Extent2D{ uint32_t(vp.w), uint32_t(vp.h) })
		.SetColor({ swapView },
			smvk::AttachmentLoadOp::Load, smvk::AttachmentStoreOp::Store,
			smvk::ClearColor(0, 0, 0, 1))
		.SetDepth(m_depth.logicalView,
			smvk::AttachmentLoadOp::Load, smvk::AttachmentStoreOp::Store,
			smvk::ClearDepthStencil(0.0f, 0));
	pass.SetRecordingFunc([this, ranges, vpViewProj, vpMode, vpIsFlat, vpIdx,
			flatIdentity](smvk::CommandBuffer& cmd) {
		if (m_gizmoLines.empty() || m_lineProgram.shaderCount != 2)
			return;
		const smvk::RasterSettings raster{
			.cullMode = smvk::CullMode::None };
		cmd.BindVertexBuffer(m_gizmoLines, 0, 0);
		bool depthPipeline = false;
		bool drewAny = false;
		for (const auto& r : ranges) {
			// Mode filter: flat ranges belong to their OWN flat panel
			// (per-pane zoom/pan), 3D ranges to the 3D panels.
			if (r.flat != vpIsFlat)
				continue;
			if (r.flat && (r.flatMode != vpMode || r.flatVp != vpIdx))
				continue;
			// Rebind the pipeline only when the depth mode changes
			// (grid/flat wires = depth-tested, helpers = on top).
			if (r.depthTest != depthPipeline || drewAny == false) {
				depthPipeline = r.depthTest;
				if (depthPipeline)
					cmd.BindRenderPipeline(m_lineProgram,
						smvk::VertexFormat::DebugLine,
						// Reversed-Z: GreaterEqual = closer-or-equal passes.
						smvk::DepthSettings{
							.depthFunc = smvk::CompareOp::GreaterEqual,
							.writeEnable = false, .depthTestEnable = true },
						smvk::StencilSettings{}, smvk::BlendSettings{},
						smvk::MSAASettings{}, raster,
						smvk::PrimitiveTopology::LineList);
				else
					cmd.BindRenderPipeline(m_lineProgram,
						smvk::VertexFormat::DebugLine,
						smvk::DepthSettings{
							.depthFunc = smvk::CompareOp::Always,
							.writeEnable = false, .depthTestEnable = false },
						smvk::StencilSettings{}, smvk::BlendSettings{},
						smvk::MSAASettings{}, raster,
						smvk::PrimitiveTopology::LineList);
			}
			const glm::mat4& vpForPush = r.flat ? flatIdentity : vpViewProj;
			app::LineDebugPush lp{};
			std::memcpy(lp.viewProj, glm::value_ptr(vpForPush),
				sizeof(lp.viewProj));
			lp.color[0] = r.color.x;
			lp.color[1] = r.color.y;
			lp.color[2] = r.color.z;
			lp.color[3] = r.color.w;
			cmd.PushConstants(0, sizeof(lp), &lp, smvk::ShaderStage::All);
			cmd.Draw(r.count, 1, r.first, 0);
			drewAny = true;
		}
		});
	} // per-viewport loop
}

bool EditorApp::GizmoHandleInput() {
	if (m_selected == UINT32_MAX || !m_world ||
		m_selected >= m_world->scene.hierarchy.size())
		return false;
	// ACTIVE viewport-local mouse + dims + viewProj for the hit tests.
	const ViewportRect& vp = m_viewports[m_activeVp].rect;
	const smvk::Extent2D extent{ uint32_t(vp.w), uint32_t(vp.h) };
	const glm::mat4 viewProj = m_viewports[m_activeVp].viewProj;
	const float mx = float(m_input.getMouseX()) - vp.x;
	const float my = float(m_input.getMouseY()) - vp.y;
	const bool shiftHeld = m_input.isKeyHeld(GLFW_KEY_LEFT_SHIFT) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_SHIFT);
	const glm::vec3 O = GizmoOrigin(m_world->scene, m_selected);
	const float dist = glm::distance(EditorCamEye(ActiveCam()), O);
	const float s = std::max(dist * m_gizmoScale, 1e-4f);
	const bool useLocal = (m_gizmoMode != GizmoMode::Translate);
	const glm::mat4& global = m_world->scene.globalTransform[m_selected];
	const glm::vec3 axesW[3] = { {1,0,0}, {0,1,0}, {0,0,1} };

	if (!m_gizmoDragging) {
		// Hit test. Rotate: the axis whose PROJECTED ring passes
		// closest to the mouse wins (48 samples, thick invisible grab);
		// clicks inside the sphere but off the rings grab the arcball
		// (free rotation). Translate/scale: plane quads first (two-axis
		// handles), then the axis shafts.
		int best = -1;
		const glm::vec2 oPx = ToScreen(O, viewProj, extent);
		if (m_gizmoMode == GizmoMode::Rotate) {
			float bestD = 15.0f;
			for (int i = 0; i < 3; ++i) {
				glm::vec3 ax = axesW[i];
				if (useLocal) {
					ax = glm::vec3(global * glm::vec4(axesW[i], 0.0f));
					if (glm::length(ax) < 1e-6f)
						continue;
					ax = glm::normalize(ax);
				}
				const glm::vec3 u = glm::normalize(
					std::fabs(ax.y) < 0.9f ? glm::cross(ax, glm::vec3(0, 1, 0))
					: glm::cross(ax, glm::vec3(1, 0, 0)));
				const glm::vec3 v = glm::cross(ax, u);
				for (int k = 0; k < 48; ++k) {
					const float a0 = float(k) / 48 * 6.2831853f;
					const float a1 = float(k + 1) / 48 * 6.2831853f;
					const glm::vec2 p0 = ToScreen(O + (u * std::cos(a0) + v * std::sin(a0)) * s,
						viewProj, extent);
					const glm::vec2 p1 = ToScreen(O + (u * std::cos(a1) + v * std::sin(a1)) * s,
						viewProj, extent);
					const float d = DistPointSeg({ mx, my }, p0, p1);
					if (d < bestD) {
						bestD = d;
						best = i;
					}
				}
			}
			if (best < 0) {
				// Arcball zone: inside the projected sphere, off the
				// rings — unconstrained rotation (DCC style).
				const glm::mat4 invVP = glm::inverse(viewProj);
				const glm::vec3 camRightW =
					glm::normalize(glm::vec3(invVP[0]));
				const float radiusPx = glm::length(
					ToScreen(O + camRightW * s, viewProj, extent) - oPx);
				if (radiusPx > 4.0f
					&& glm::length(glm::vec2(mx, my) - oPx) < radiusPx)
					best = 6;
			}
		}
		else {
			// Uniform center cube first (all three axes at once):
			// explicit click on the cube wins over the shafts.
			if (glm::length(glm::vec2(mx, my) - oPx) < 12.0f) {
				best = 7;
			}
			else {
			const float qd = s * 0.35f;
			float bestPlaneD = FLT_MAX;
			int bestPlane = -1;
			for (int n = 0; n < 3; ++n) {
				const int a = (n + 1) % 3, b = (n + 2) % 3;
				glm::vec3 axA = axesW[a], axB = axesW[b];
				if (useLocal) {
					axA = glm::vec3(global * glm::vec4(axesW[a], 0.0f));
					axB = glm::vec3(global * glm::vec4(axesW[b], 0.0f));
					if (glm::length(axA) < 1e-6f
						|| glm::length(axB) < 1e-6f)
						continue;
					axA = glm::normalize(axA);
					axB = glm::normalize(axB);
				}
				const glm::vec2 q0 = oPx;
				const glm::vec2 q1 = ToScreen(O + axA * qd, viewProj, extent);
				const glm::vec2 q2 = ToScreen(O + axA * qd + axB * qd,
					viewProj, extent);
				const glm::vec2 q3 = ToScreen(O + axB * qd, viewProj, extent);
				if (PointInQuad({ mx, my }, q0, q1, q2, q3)) {
					const float d = glm::length(glm::vec2(mx, my)
						- (q0 + q1 + q2 + q3) * 0.25f);
					if (d < bestPlaneD) {
						bestPlaneD = d;
						bestPlane = n;
					}
				}
			}
			if (bestPlane >= 0) {
				best = 3 + bestPlane;
			}
			else {
				float bestD = 10.0f;
				for (int i = 0; i < 3; ++i) {
					glm::vec3 ax = axesW[i];
					if (useLocal) {
						ax = glm::vec3(global * glm::vec4(axesW[i], 0.0f));
						if (glm::length(ax) < 1e-6f)
							continue;
						ax = glm::normalize(ax);
					}
					glm::vec3 end = O + ax * s;
					const glm::vec2 tipPx = ToScreen(end, viewProj, extent);
					float d = DistPointSeg({ mx, my },
						ToScreen(O, viewProj, extent), tipPx);
					// Generous grab on the head (arrow tip / scale cube).
					if (glm::length(glm::vec2(mx, my) - tipPx) < 16.0f)
						d = 0.0f;
					if (d < bestD) {
						bestD = d;
						best = i;
					}
				}
			}
			}
		}
		if (best < 0)
			return false;
		// Begin drag: snapshot the full baseline local matrix + grab data.
		// The drag writes live values; release pushes ONE undoable command.
		m_gizmoDragging = true;
		m_gizmoAxis = best;
		m_dragBaseMat = m_world->scene.localTransform[m_selected];
		ReadLocalTRS(m_selected, m_dragCurT, m_dragCurR, m_dragCurS);
		m_dragBaseT = m_dragCurT;
		m_dragBaseR = m_dragCurR;
		m_dragBaseS = m_dragCurS;
		m_dragGrabPoint = O;
		m_dragNodeOrigin = O;
		m_dragLastX = mx;
		m_dragLastY = my;
		m_dragStartDist = s;
		m_dragArcQ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		if (best == 6) {
			// Arcball: sphere-tracked below, no grab plane needed.
			m_dragBaseAngle = 0.0f;
			return true;
		}
		// Ray-plane for translate/scale: single axes drag in the plane
		// containing the axis and facing the camera (axis-plane drag);
		// plane handles drag in the axis-pair plane itself; the uniform
		// cube drags in the camera-facing plane through the origin.
		glm::vec3 origin;
		const glm::vec3 ray = MouseRay(mx, my, extent, viewProj, origin);
		if (best == 7) {
			const glm::vec3 camDir =
				glm::normalize(O - EditorCamEye(ActiveCam()));
			m_dragPlaneN = camDir;
		}
		else if (best >= 3) {
			const int n = best - 3;
			glm::vec3 nrm = axesW[n];
			if (useLocal) {
				nrm = glm::vec3(global * glm::vec4(axesW[n], 0.0f));
				if (glm::length(nrm) < 1e-6f)
					nrm = axesW[n];
				else
					nrm = glm::normalize(nrm);
			}
			m_dragPlaneN = nrm;
		}
		else {
			glm::vec3 ax = axesW[best];
			if (useLocal) {
				ax = glm::vec3(global * glm::vec4(axesW[best], 0.0f));
				if (glm::length(ax) < 1e-6f)
					ax = axesW[best];
				else
					ax = glm::normalize(ax);
			}
			const glm::vec3 camDir = glm::normalize(O - EditorCamEye(ActiveCam()));
			m_dragPlaneN = glm::normalize(glm::cross(ax, glm::cross(camDir, ax)));
		}
		const float denom = glm::dot(ray, m_dragPlaneN);
		if (std::fabs(denom) > 1e-9f) {
			const float t = glm::dot(O - origin, m_dragPlaneN) / denom;
			if (t > 0.0f)
				m_dragGrabPoint = origin + ray * t;
		}
		m_dragBaseAngle = 0.0f;
		return true;
	}

	// Continue drag (or finish on release).
	if (!m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT)) {
		m_gizmoDragging = false;
		// One undoable command for the whole gesture.
		EditCmd c{};
		c.node = m_selected;
		c.beforeMat = m_dragBaseMat;
		if (m_gizmoMode == GizmoMode::Translate) {
			c.verb = EditVerb::Translate;
			c.afterVec = m_dragCurT;
		}
		else if (m_gizmoMode == GizmoMode::Rotate) {
			c.verb = EditVerb::Rotate;
			c.afterVec = m_dragCurR;
		}
		else {
			c.verb = EditVerb::Scale;
			c.afterVec = m_dragCurS;
		}
		m_gizmoAxis = -1;
		PushDone(c);
		Log("Gizmo edit (" + std::string(
			m_gizmoMode == GizmoMode::Translate ? "move"
			: m_gizmoMode == GizmoMode::Rotate ? "rotate" : "scale") +
			" #" + std::to_string(m_selected) + ")");
		return true;
	}
	const int axIdx = m_gizmoAxis;
	if (axIdx < 0 || axIdx > 6)
		return true;
	glm::vec3 ax = useLocal
		? glm::vec3(global * glm::vec4(axesW[std::min(axIdx, 2)], 0.0f)) : axesW[std::min(axIdx, 2)];
	if (glm::length(ax) < 1e-9f)
		return true;
	ax = glm::normalize(ax);

	if (m_gizmoMode == GizmoMode::Translate) {
		glm::vec3 origin;
		const glm::vec3 ray = MouseRay(mx, my, extent, viewProj, origin);
		const float denom = glm::dot(ray, m_dragPlaneN);
		if (std::fabs(denom) > 1e-9f) {
			const float t = glm::dot(O - origin, m_dragPlaneN) / denom;
			if (t > 0.0f) {
				const glm::vec3 cur = origin + ray * t;
				// World delta -> parent-local delta (rotation part only).
				const int parent = m_world->scene.hierarchy[m_selected].parent;
				auto toLocal = [&](const glm::vec3& w) {
					if (parent < 0)
						return w;
					const glm::mat3 pw(m_world->scene.globalTransform[parent]);
					const float det = glm::determinant(pw);
					return (std::fabs(det) > 1e-12f)
						? glm::inverse(pw) * w : w;
				};
				if (axIdx == 7) {
					// Uniform cube: full 3D move (no axis projection).
					const glm::vec3 dW = cur - m_dragGrabPoint;
					m_dragCurT = m_dragBaseT + toLocal(dW);
				}
				else if (axIdx <= 2) {
					const float d = glm::dot(cur - m_dragGrabPoint, ax);
					m_dragCurT = m_dragBaseT + toLocal(ax * d);
				}
				else {
					// Plane handle: move on both tangent axes at once.
					const int n = axIdx - 3;
					const int a = (n + 1) % 3, b = (n + 2) % 3;
					const glm::vec3 axA = axesW[a], axB = axesW[b];
					const glm::vec3 dvec = cur - m_dragGrabPoint;
					const glm::vec3 dW = axA * glm::dot(dvec, axA)
						+ axB * glm::dot(dvec, axB);
					m_dragCurT = m_dragBaseT + toLocal(dW);
				}
				// Shift = grid snap: translate steps to the grid cell.
				if (shiftHeld && m_gridSize > 0.0f) {
					const float inv = 1.0f / m_gridSize;
					for (int k = 0; k < 3; ++k)
						m_dragCurT[k] = std::round(m_dragCurT[k] * inv) / inv;
				}
				WriteLocalTRS(m_selected, m_dragCurT, m_dragCurR, m_dragCurS);
			}
		}
	}
	else if (m_gizmoMode == GizmoMode::Rotate) {
		if (axIdx == 6) {
			// Arcball: map the mouse delta onto the unit sphere in
			// camera space (around the projected gizmo center) and
			// accumulate the rotation. The camera-space delta becomes
			// a local-space delta through the parent world orientation.
			const glm::mat4 invVP = glm::inverse(viewProj);
			const glm::vec3 camRightW =
				glm::normalize(glm::vec3(invVP[0]));
			const glm::vec3 camUpW =
				glm::normalize(glm::vec3(invVP[1]));
			const glm::vec2 centerPx = ToScreen(O, viewProj, extent);
			const float radiusPx = glm::max(glm::length(
				ToScreen(O + camRightW * s, viewProj, extent) - centerPx),
				4.0f);
			auto toSphere = [&](float x, float y) {
				glm::vec2 p((x - centerPx.x) / radiusPx,
					-(y - centerPx.y) / radiusPx);
				const float l2 = glm::dot(p, p);
				if (l2 <= 1.0f)
					return glm::vec3(p.x, p.y, std::sqrt(1.0f - l2));
				const float l = std::sqrt(l2);
				return glm::vec3(p.x / l, p.y / l, 0.0f); // rim clamp
			};
			const glm::vec3 v0 = toSphere(m_dragLastX, m_dragLastY);
			const glm::vec3 v1 = toSphere(mx, my);
			m_dragLastX = mx;
			m_dragLastY = my;
			const glm::vec3 cr = glm::cross(v0, v1);
			if (glm::length(cr) > 1e-9f) {
				const float ang = std::acos(std::clamp(
					glm::dot(v0, v1), -1.0f, 1.0f));
				const glm::vec3 axisW = glm::normalize(
					camRightW * cr.x + camUpW * cr.y
					+ glm::normalize(glm::cross(camRightW, camUpW)) * cr.z);
				// Camera-space delta -> parent-local delta: the node's
				// world orientation is qParentWorld * qLocal.
				const int parent =
					m_world->scene.hierarchy[m_selected].parent;
				const glm::mat4& pwM = (parent >= 0)
					? m_world->scene.globalTransform[parent]
					: glm::mat4(1.0f);
				const glm::quat qParentW = glm::quat_cast(glm::mat3(pwM));
				const glm::quat dq = glm::angleAxis(ang, axisW);
				m_dragArcQ = dq * m_dragArcQ;
				const glm::quat qLocal =
					glm::inverse(qParentW) * m_dragArcQ * qParentW
					* glm::quat_cast(glm::mat3(m_dragBaseMat));
				// Back to the editor's euler storage (same order
				// fidelity as ReadLocalTRS: eulerAngles XYZ read,
				// Rz*Ry*Rx write — see LocalMatrixFromTRS).
				m_dragCurR = glm::degrees(glm::eulerAngles(qLocal));
				m_dragCurT = m_dragBaseT;
				m_dragCurS = m_dragBaseS;
				WriteLocalTRS(m_selected, m_dragCurT, m_dragCurR,
					m_dragCurS);
			}
			return true;
		}
		const float dx = mx - m_dragLastX;
		m_dragLastX = mx;
		m_dragBaseAngle += dx * 0.008f; // radians per pixel
		float angleDeg = glm::degrees(m_dragBaseAngle);
		// Shift = angle snap (e.g. 15-degree steps).
		if (shiftHeld && m_snapRotateDeg > 0.0f)
			angleDeg = std::round(angleDeg / m_snapRotateDeg) * m_snapRotateDeg;
		m_dragCurR = m_dragBaseR;
		m_dragCurR[axIdx] += angleDeg;
		WriteLocalTRS(m_selected, m_dragCurT, m_dragCurR, m_dragCurS);
	}
	else { // Scale
		glm::vec3 origin;
		const glm::vec3 ray = MouseRay(mx, my, extent, viewProj, origin);
		const float denom = glm::dot(ray, m_dragPlaneN);
		if (std::fabs(denom) > 1e-9f) {
			const float t = glm::dot(O - origin, m_dragPlaneN) / denom;
			if (t > 0.0f) {
				const glm::vec3 cur = origin + ray * t;
				m_dragCurS = m_dragBaseS;
				if (axIdx == 7) {
					// Uniform cube: single factor from the screen-space
					// radial distance (drag outward to grow).
					const glm::vec2 centerPx =
						ToScreen(O, viewProj, extent);
					const glm::vec2 grabPx =
						ToScreen(m_dragGrabPoint, viewProj, extent);
					const float r0 = glm::max(
						glm::length(grabPx - centerPx), 8.0f);
					const float r1 = glm::length(
						glm::vec2(mx, my) - centerPx);
					float ratio = std::clamp(r1 / r0, 0.05f, 50.0f);
					if (shiftHeld && m_snapScaleStep > 0.0f)
						ratio = std::round(ratio / m_snapScaleStep)
							* m_snapScaleStep;
					ratio = std::clamp(ratio, 0.05f, 50.0f);
					m_dragCurS = m_dragBaseS * ratio;
				}
				else if (axIdx <= 2) {
					float ratio = 1.0f;
					const float d0 = glm::dot(m_dragGrabPoint - m_dragNodeOrigin, ax);
					const float d1 = glm::dot(cur - m_dragNodeOrigin, ax);
					if (std::fabs(d0) > 1e-6f)
						ratio = std::clamp(d1 / d0, 0.05f, 50.0f);
					else
						ratio = std::clamp(1.0f + (d1 / m_dragStartDist), 0.05f, 50.0f);
					// Shift = scale snap (e.g. 0.1 steps).
					if (shiftHeld && m_snapScaleStep > 0.0f)
						ratio = std::round(ratio / m_snapScaleStep) * m_snapScaleStep;
					ratio = std::clamp(ratio, 0.05f, 50.0f);
					m_dragCurS[axIdx] = m_dragBaseS[axIdx] * ratio;
				}
				else {
					// Plane handle: per-tangent-axis ratios (local
					// frame — scale planes are drawn local).
					const int n = axIdx - 3;
					const int comps[2] = { (n + 1) % 3, (n + 2) % 3 };
					for (int k = 0; k < 2; ++k) {
						const int ci = comps[k];
						glm::vec3 axK = glm::vec3(global
							* glm::vec4(axesW[ci], 0.0f));
						if (glm::length(axK) < 1e-6f)
							continue;
						axK = glm::normalize(axK);
						const float d0 = glm::dot(
							m_dragGrabPoint - m_dragNodeOrigin, axK);
						const float d1 = glm::dot(
							cur - m_dragNodeOrigin, axK);
						float ratio = 1.0f;
						if (std::fabs(d0) > 1e-6f)
							ratio = std::clamp(d1 / d0, 0.05f, 50.0f);
						else
							ratio = std::clamp(1.0f + (d1 / m_dragStartDist),
								0.05f, 50.0f);
						if (shiftHeld && m_snapScaleStep > 0.0f)
							ratio = std::round(ratio / m_snapScaleStep)
								* m_snapScaleStep;
						ratio = std::clamp(ratio, 0.05f, 50.0f);
						m_dragCurS[ci] = m_dragBaseS[ci] * ratio;
					}
				}
				WriteLocalTRS(m_selected, m_dragCurT, m_dragCurR, m_dragCurS);
			}
		}
	}
	return true;
}

} // namespace editor
