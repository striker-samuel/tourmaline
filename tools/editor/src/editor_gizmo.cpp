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
	// Flat UV wire batches: drawn ONLY in viewports whose view mode is
	// flatMode, pushed through an IDENTITY viewProj (verts are already
	// in NDC space — see BuildFlatUvWires). 3D batches skip flat panels.
	bool flat = false;
	ViewMode flatMode = ViewMode::Lit;
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
	// Flat UV wires (per needed mode): unwrapped shell edges in NDC
	// space (identity viewProj; see BuildFlatUvWires).
	bool flatNeeded[2] = { false, false }; // [0]=FlatUv0, [1]=FlatUv1
	for (int vpIdx = 0; vpIdx < ViewportCount(); ++vpIdx) {
		const ViewMode vm = m_viewports[vpIdx].viewMode;
		if (vm == ViewMode::FlatUv0)
			flatNeeded[0] = true;
		else if (vm == ViewMode::FlatUv1)
			flatNeeded[1] = true;
	}
	for (int fm = 0; fm < 2; ++fm) {
		if (!flatNeeded[fm])
			continue;
		LineBatch wires;
		wires.flat = true;
		wires.flatMode = fm == 0 ? ViewMode::FlatUv0 : ViewMode::FlatUv1;
		wires.color = fm == 0 ? glm::vec4(0.15f, 0.95f, 0.55f, 1.0f)
			: glm::vec4(0.35f, 0.7f, 1.0f, 1.0f);
		wires.depthTest = true; // over the flat fill (z=1 vs 0.9999)
		// Panel wires = the SELECTION's subtree (UV editor style; the
		// PNG export wires the whole scene instead).
		BuildFlatUvWires(wires.verts, wires.flatMode, m_selected);
		if (!wires.verts.empty())
			batches.push_back(std::move(wires));
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
	}

	// Flatten into one upload with per-batch draw ranges. Grid + flat-UV
	// wire ranges are DEPTH-TESTED against the viewport's depth (the
	// grid hides behind geometry; flat wires land over the UV fill);
	// every helper (gizmo axes, AABB, light markers) draws on top like
	// a DCC overlay. Flat ranges render through an identity viewProj
	// and only in panels whose view mode matches.
	struct Range { uint32_t first, count; glm::vec4 color;
		bool depthTest; bool flat; ViewMode flatMode; };
	std::vector<Range> ranges;
	std::vector<smvk::DebugLineVertex> all;
	for (auto& b : batches) {
		if (b.verts.empty())
			continue;
		ranges.push_back({ uint32_t(all.size()), uint32_t(b.verts.size()),
			b.color, b.depthTest, b.flat, b.flatMode });
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
	pass.SetRecordingFunc([this, ranges, vpViewProj, vpMode, vpIsFlat,
			flatIdentity](smvk::CommandBuffer& cmd) {
		if (m_gizmoLines.empty() || m_lineProgram.shaderCount != 2)
			return;
		const smvk::RasterSettings raster{
			.cullMode = smvk::CullMode::None };
		cmd.BindVertexBuffer(m_gizmoLines, 0, 0);
		bool depthPipeline = false;
		bool drewAny = false;
		for (const auto& r : ranges) {
			// Mode filter: flat ranges belong to their flat panel,
			// 3D ranges to the 3D panels.
			if (r.flat != vpIsFlat)
				continue;
			if (r.flat && r.flatMode != vpMode)
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
		// Hit test: nearest axis segment in screen pixels.
		int best = -1;
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
			if (m_gizmoMode == GizmoMode::Rotate) {
				// Rings: accept grabs near the projected circle — sample it.
				const glm::vec3 u = glm::normalize(
					std::fabs(ax.y) < 0.9f ? glm::cross(ax, glm::vec3(0, 1, 0))
					: glm::cross(ax, glm::vec3(1, 0, 0)));
				const glm::vec3 v = glm::cross(ax, u);
				for (int k = 0; k < 24; ++k) {
					const float a0 = float(k) / 24 * 6.2831853f;
					const float a1 = float(k + 1) / 24 * 6.2831853f;
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
				continue;
			}
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
		m_dragStartDist = s;
		// Ray-plane for translate/scale: plane through O facing the camera
		// around the axis (axis-plane drag).
		glm::vec3 origin;
		const glm::vec3 ray = MouseRay(mx, my, extent, viewProj, origin);
		glm::vec3 ax = useLocal
			? glm::normalize(glm::vec3(global * glm::vec4(axesW[best], 0.0f))) : axesW[best];
		const glm::vec3 camDir = glm::normalize(O - EditorCamEye(ActiveCam()));
		m_dragPlaneN = glm::normalize(glm::cross(ax, glm::cross(camDir, ax)));
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
	if (axIdx < 0 || axIdx > 2)
		return true;
	glm::vec3 ax = useLocal
		? glm::vec3(global * glm::vec4(axesW[axIdx], 0.0f)) : axesW[axIdx];
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
				const float d = glm::dot(cur - m_dragGrabPoint, ax);
				// World delta -> parent-local delta (rotation part only).
				const int parent = m_world->scene.hierarchy[m_selected].parent;
				glm::vec3 dLocal = ax * d;
				if (parent >= 0) {
					const glm::mat3 pw(m_world->scene.globalTransform[parent]);
					const float det = glm::determinant(pw);
					if (std::fabs(det) > 1e-12f)
						dLocal = glm::inverse(pw) * (ax * d);
				}
				m_dragCurT = m_dragBaseT + dLocal;
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
		float ratio = 1.0f;
		if (std::fabs(denom) > 1e-9f) {
			const float t = glm::dot(O - origin, m_dragPlaneN) / denom;
			if (t > 0.0f) {
				const glm::vec3 cur = origin + ray * t;
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
			}
		}
		m_dragCurS = m_dragBaseS;
		m_dragCurS[axIdx] = m_dragBaseS[axIdx] * ratio;
		WriteLocalTRS(m_selected, m_dragCurT, m_dragCurR, m_dragCurS);
	}
	return true;
}

} // namespace editor
