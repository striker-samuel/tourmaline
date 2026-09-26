// Editor viewports: multisection state + rubber-band box select +
// viewport chrome (borders, camera combos, box rectangle) drawn with
// ImGui over the rendered viewports.

#include "editor_app.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

namespace editor {

bool EditorApp::IsSelected(uint32_t node) const {
	return std::find(m_selection.begin(), m_selection.end(), node) !=
		m_selection.end();
}

void EditorApp::SetSelection(uint32_t node) {
	m_worldSelected = false;
	ClearNonDagSelection();
	m_selection.clear();
	if (node != UINT32_MAX) {
		m_selection.push_back(node);
		m_selected = node;
	}
	else {
		m_selected = UINT32_MAX;
	}
	// Node change invalidates sub-object highlights that pointed elsewhere.
	if (m_selFaceNode != node && m_selFaceNode != UINT32_MAX &&
		m_selMode == SelMode::Face) {
		m_selFaceNode = UINT32_MAX;
		m_selFaceTri = UINT32_MAX;
	}
	if (m_selVertNode != node && m_selVertNode != UINT32_MAX &&
		m_selMode == SelMode::Vertex) {
		m_selVertNode = UINT32_MAX;
		m_selVertIdx = UINT32_MAX;
	}
	if (node == UINT32_MAX)
		ClearSubSelection();
}

void EditorApp::ClearSubSelection() {
	m_selFaceNode = UINT32_MAX;
	m_selFaceTri = UINT32_MAX;
	m_selVertNode = UINT32_MAX;
	m_selVertIdx = UINT32_MAX;
}

void EditorApp::SelectPrevNode() {
	if (!m_world || m_world->scene.hierarchy.empty())
		return;
	const uint32_t n = uint32_t(m_world->scene.hierarchy.size());
	uint32_t cur = (m_selected == UINT32_MAX || m_selected >= n)
		? 0 : m_selected;
	cur = (cur == 0) ? n - 1 : cur - 1;
	SetSelection(cur);
	m_revealNode = cur;
}

void EditorApp::SelectNextNode() {
	if (!m_world || m_world->scene.hierarchy.empty())
		return;
	const uint32_t n = uint32_t(m_world->scene.hierarchy.size());
	uint32_t cur = (m_selected == UINT32_MAX || m_selected >= n)
		? 0 : m_selected;
	cur = (cur + 1 >= n) ? 0 : cur + 1;
	SetSelection(cur);
	m_revealNode = cur;
}

void EditorApp::UpdateUvPaneInput() {
	// Flat-UV pane navigation (pan/zoom in UV units, centered):
	// MMB-drag pans, wheel zooms at the cursor, F resets (the frame
	// bindings in editor_modal.cpp). 3D cameras are untouched.
	SetupDefaultCameras();
	const bool uiEats = m_imgui && m_imgui->WantsCaptureMouse();
	if (uiEats)
		return;
	const float wheel = float(m_input.getScrollDeltaY());
	for (int i = 0; i < ViewportCount(); ++i) {
		if (!IsFlatUvViewMode(m_viewports[i].viewMode))
			continue;
		const ViewportRect& vp = m_viewports[i].rect;
		const float mx = float(m_input.getMouseX());
		const float my = float(m_input.getMouseY());
		const bool hover = mx >= vp.x && my >= vp.y && mx < vp.x + vp.w &&
			my < vp.y + vp.h;
		if (!hover)
			continue;
		if (wheel != 0.0f) {
			float& z = m_uvZoom[i];
			z = std::clamp(z * (wheel > 0 ? 1.15f : 1.0f / 1.15f),
				0.2f, 32.0f);
		}
		if (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_MIDDLE)) {
			glm::vec2& p = m_uvPan[i];
			const float z = (m_uvZoom[i] > 1e-6f) ? m_uvZoom[i] : 1.0f;
			p.x -= float(m_input.getMouseDeltaX()) / (vp.w > 0 ? vp.w : 1.0f) * z;
			p.y -= float(m_input.getMouseDeltaY()) / (vp.h > 0 ? vp.h : 1.0f) * z;
			p.x = std::clamp(p.x, -4.0f, 4.0f);
			p.y = std::clamp(p.y, -4.0f, 4.0f);
		}
	}
}

void EditorApp::DrawSelectionRadial() {
	// Q radial: Mesh / Face / Vertex highlight modes (selection only).
	if (m_showSelRadial) {
		ImGui::OpenPopup("##selRadial");
		m_selRadialPos = ImGui::GetIO().MousePos;
		ImGui::SetNextWindowPos(m_selRadialPos, ImGuiCond_Always);
		m_showSelRadial = false;
	}
	const bool radialOpen = ImGui::BeginPopup("##selRadial");
	m_selRadialOpen = radialOpen; // modal keys pause while it owns 1/2/3
	if (!radialOpen)
		return;
	ImGui::TextDisabled("Select mode (Q)");
	ImGui::Separator();
	const SelMode modes[3] = { SelMode::Mesh, SelMode::Face, SelMode::Vertex };
	const char* hints[3] = { "object nodes", "one triangle", "one vertex" };
	for (int i = 0; i < 3; ++i) {
		char label[64];
		std::snprintf(label, sizeof(label), "%s  (%d)%s", SelModeName(modes[i]),
			i + 1, m_selMode == modes[i] ? "  *" : "");
		if (ImGui::Selectable(label, m_selMode == modes[i])) {
			m_selMode = modes[i];
			ClearSubSelection();
			Log(std::string("Selection mode: ") + SelModeName(m_selMode) +
				" (" + hints[i] + ", highlight only).");
			ImGui::CloseCurrentPopup();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s highlight; gizmo stays on the node.", hints[i]);
	}
	ImGui::Separator();
	ImGui::TextDisabled("Q reopens  |  1/2/3 picks while open");
	// Number shortcuts while the popup is open.
	for (int i = 0; i < 3; ++i) {
		if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_1 + i))) {
			m_selMode = modes[i];
			ClearSubSelection();
			Log(std::string("Selection mode: ") + SelModeName(m_selMode));
			ImGui::CloseCurrentPopup();
			break;
		}
	}
	ImGui::EndPopup();
}

void EditorApp::AddToSelection(uint32_t node) {
	if (node == UINT32_MAX || IsSelected(node))
		return;
	m_worldSelected = false;
	ClearNonDagSelection();
	m_selection.push_back(node);
	m_selected = node; // newest = primary
}

void EditorApp::ToggleSelection(uint32_t node) {
	if (node == UINT32_MAX)
		return;
	auto it = std::find(m_selection.begin(), m_selection.end(), node);
	if (it != m_selection.end()) {
		m_selection.erase(it);
		if (m_selected == node)
			m_selected = m_selection.empty()
				? UINT32_MAX : m_selection.back();
	}
	else {
		m_worldSelected = false;
		ClearNonDagSelection();
		m_selection.push_back(node);
		m_selected = node;
	}
}

void EditorApp::SelectRange(uint32_t node) {
	// Shift-click: contiguous VISIBLE range between the primary and the
	// clicked row (outliner order); falls back to single select when the
	// anchor is gone (deleted/collapsed away).
	m_worldSelected = false;
	ClearNonDagSelection();
	ClearSubSelection();
	if (node < m_world->scene.hierarchy.size()) {
		const auto it = std::find(m_outlinerOrder.begin(),
			m_outlinerOrder.end(), node);
		const auto anchor = std::find(m_outlinerOrder.begin(),
			m_outlinerOrder.end(), m_selected);
		if (it != m_outlinerOrder.end()
			&& anchor != m_outlinerOrder.end()
			&& m_selected != UINT32_MAX) {
			size_t a = size_t(anchor - m_outlinerOrder.begin());
			size_t b = size_t(it - m_outlinerOrder.begin());
			if (a > b)
				std::swap(a, b);
			m_selection.clear();
			for (size_t i = a; i <= b; ++i)
				m_selection.push_back(m_outlinerOrder[i]);
			m_selected = node;
			return;
		}
	}
	SetSelection(node);
}

void EditorApp::ClearSelection() {
	m_worldSelected = false;
	ClearNonDagSelection();
	m_selection.clear();
	m_selected = UINT32_MAX;
	ClearSubSelection();
}

void EditorApp::SelectWorldNode() {
	// Pseudo-selection (see m_worldSelected): clears the node selection
	// without touching the hierarchy. m_selected stays UINT32_MAX so
	// every consumer keeps its existing no-selection behavior.
	m_selection.clear();
	m_selected = UINT32_MAX;
	ClearSubSelection();
	ClearNonDagSelection();
	m_worldSelected = true;
}

uint32_t EditorApp::FindSunNode() const {
	if (!m_world)
		return UINT32_MAX;
	for (const auto& [node, li] : m_world->scene.lightForNode) {
		(void)li;
		if (const ::SceneLight* L = FindNodeLight(node))
			if (L->type == ::SceneLight::Type::Directional)
				return node;
	}
	return UINT32_MAX;
}

void EditorApp::FinishBoxSelect() {
	if (!m_world)
		return;
	const EditorViewport& vp = m_viewports[std::clamp(m_vpBoxVp, 0,
		kMaxViewports - 1)];
	const float x0 = std::min(m_vpBoxX0, m_vpBoxX1);
	const float x1 = std::max(m_vpBoxX0, m_vpBoxX1);
	const float y0 = std::min(m_vpBoxY0, m_vpBoxY1);
	const float y1 = std::max(m_vpBoxY0, m_vpBoxY1);
	const smvk::Extent2D ext{ uint32_t(vp.rect.w), uint32_t(vp.rect.h) };
	// A mesh node matches when its projected AABB intersects the box.
	std::vector<uint32_t> hits;
	const smvk::IteratorFlags req = smvk::IteratorFlags(
		smvk::IteratorFlagBit_Renderable |
		smvk::IteratorFlagBit_StaticVertexFmt);
	m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
		if (!IsVisibleNode(m_world->scene, int(item.node)))
			return;
		const smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
		if (!mesh)
			return;
		const glm::mat4& model = m_world->scene.globalTransform[item.node];
		float sx0 = FLT_MAX, sy0 = FLT_MAX, sx1 = -FLT_MAX, sy1 = -FLT_MAX;
		for (int k = 0; k < 8; ++k) {
			const glm::vec3 corner(
				(k & 1) ? mesh->geometry.aabbMax.x : mesh->geometry.aabbMin.x,
				(k & 2) ? mesh->geometry.aabbMax.y : mesh->geometry.aabbMin.y,
				(k & 4) ? mesh->geometry.aabbMax.z : mesh->geometry.aabbMin.z);
			const glm::vec4 c = vp.viewProj * model * glm::vec4(corner, 1.0f);
			if (c.w <= 1e-6f)
				return; // behind the camera: skip
			const float px = (c.x / c.w * 0.5f + 0.5f) * float(ext.width);
			const float py = (c.y / c.w * 0.5f + 0.5f) * float(ext.height);
			sx0 = std::min(sx0, px); sx1 = std::max(sx1, px);
			sy0 = std::min(sy0, py); sy1 = std::max(sy1, py);
		}
		if (sx1 < x0 || sx0 > x1 || sy1 < y0 || sy0 > y1)
			return;
		hits.push_back(item.node);
	});
	if (!m_vpBoxAdditive)
		m_selection.clear();
	for (uint32_t n : hits)
		if (!IsSelected(n))
			m_selection.push_back(n);
	// Primary: keep the previous primary when still selected, else first.
	if (!IsSelected(m_selected))
		m_selected = m_selection.empty() ? UINT32_MAX : m_selection.back();
	ClearSubSelection(); // box select is node-level (mesh mode)
	if (!m_selection.empty())
		Log("Box select: " + std::to_string(m_selection.size()) +
			" node(s) selected.");
}

void EditorApp::DrawViewportChrome() {
	// Borders + per-viewport label (camera combo) + the rubber-band box.
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	const int count = ViewportCount();
	for (int i = 0; i < count; ++i) {
		const EditorViewport& vp = m_viewports[i];
		const bool active = (i == m_activeVp);
		dl->AddRect(ImVec2(vp.rect.x, vp.rect.y),
			ImVec2(vp.rect.x + vp.rect.w, vp.rect.y + vp.rect.h),
			active ? IM_COL32(120, 200, 255, 220) : IM_COL32(70, 70, 76, 180),
			0.0f, 0, active ? 2.0f : 1.0f);
	}
	// Rubber-band box (window pixels).
	if (m_vpBoxDragging && m_vpBoxVp >= 0 && m_vpBoxVp < kMaxViewports) {
		const ViewportRect& vp = m_viewports[m_vpBoxVp].rect;
		const float bx0 = vp.x + std::min(m_vpBoxX0, m_vpBoxX1);
		const float by0 = vp.y + std::min(m_vpBoxY0, m_vpBoxY1);
		const float bx1 = vp.x + std::max(m_vpBoxX0, m_vpBoxX1);
		const float by1 = vp.y + std::max(m_vpBoxY0, m_vpBoxY1);
		dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1),
			IM_COL32(120, 200, 255, 28));
		dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1),
			IM_COL32(120, 200, 255, 200), 0.0f, 0, 1.5f);
	}
	// Per-viewport camera picker + VIEW TYPE chip (UV views are panel
	// types: any panel can show UV0/UV1/lightmap debug like a camera
	// view). Top-left inside each rect.
	for (int i = 0; i < count; ++i) {
		EditorViewport& vp = m_viewports[i];
		ImGui::SetNextWindowPos(
			ImVec2(vp.rect.x + 8.0f, vp.rect.y + 8.0f), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(1.0f);
		ImGui::PushID(i);
		char label[32];
		std::snprintf(label, sizeof(label), "##vpchip%d", i);
		if (ImGui::Begin(label, nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoNav)) {
			// Camera combo: free orbit cameras, or a locked look-through
			// binding to a scene camera node (viewport follows the node
			// until released back to an orbit camera).
			std::string bindLabel;
			bool bound = false;
			uint32_t boundNode = UINT32_MAX;
			if (vp.boundNode >= 0 && m_world
				&& FindNodeCamera(uint32_t(vp.boundNode)) != nullptr) {
				bound = true;
				boundNode = uint32_t(vp.boundNode);
				bindLabel = "Cam: "
					+ getNodeName(m_world->scene, int(boundNode));
			}
			else {
				bindLabel = m_editorCams[vp.camIdx].name;
			}
			if (ImGui::BeginCombo("##cam", bindLabel.c_str())) {
				for (size_t c = 0; c < m_editorCams.size(); ++c)
					if (ImGui::Selectable(m_editorCams[c].name.c_str(),
							!bound && int(c) == vp.camIdx)) {
						vp.camIdx = int(c);
						vp.boundNode = -1;
					}
				// Scene cameras (hierarchy order, stable list).
				if (m_world && !m_world->scene.cameraForNode.empty()) {
					ImGui::Separator();
					std::vector<uint32_t> camNodes;
					for (const auto& [n, ci] : m_world->scene.cameraForNode) {
						(void)ci;
						if (n < m_world->scene.hierarchy.size()
							&& !m_deletedNodes.contains(n))
							camNodes.push_back(n);
					}
					std::sort(camNodes.begin(), camNodes.end());
					for (uint32_t n : camNodes) {
						const std::string item = "Cam: "
							+ getNodeName(m_world->scene, int(n));
						if (ImGui::Selectable(item.c_str(),
								bound && boundNode == n)) {
							vp.boundNode = int(n);
							Log("Viewport " + std::to_string(i)
								+ " look-through: '"
								+ getNodeName(m_world->scene, int(n))
								+ "' (#" + std::to_string(n) + ").");
						}
					}
				}
				else {
					ImGui::TextDisabled("no scene cameras (Create > Camera)");
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(bound
					? "Locked look-through: viewport follows the camera "
					  "node (orbit nav parked; move the node instead). "
					  "Pick an orbit camera to release."
					: "Free orbit. Bind a scene camera node for locked "
					  "look-through.");
			if (bound) {
				ImGui::SameLine();
				ImGui::TextDisabled("[locked]");
			}
			ImGui::SameLine();
			// View type combo (this panel's shading/debug view).
			if (ImGui::BeginCombo("##view", ViewModeName(vp.viewMode))) {
				for (int m = 0; m <= int(ViewMode::FlatUv1); ++m) {
					const ViewMode vm = ViewMode(m);
					// Atlas views need the loaded lightmap GPU layers.
					if (ViewModeNeedsAtlas(vm) && !m_lmGpu)
						continue;
					if (ImGui::Selectable(ViewModeName(vm),
							int(vp.viewMode) == m)) {
						vp.viewMode = vm;
						if (vp.viewMode == ViewMode::Meshlets &&
							!m_meshletDebugHasData && !EnsureMeshletDebug())
							Log("Meshlet view: no cooked meshlets in this "
								"scene; meshes show as flat fallbacks.");
					}
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			// Selection-mode combo (Mesh/Face/Vertex highlight, Q radial).
			if (ImGui::BeginCombo("##selmode", SelModeName(m_selMode))) {
				for (int m = 0; m <= int(SelMode::Vertex); ++m) {
					if (ImGui::Selectable(SelModeName(SelMode(m)),
							int(m_selMode) == m)) {
						m_selMode = SelMode(m);
						ClearSubSelection();
						Log(std::string("Selection mode: ") +
							SelModeName(m_selMode) + " (highlight only).");
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Q opens the selection radial.");
			// Flat-pane nav readout (area mode): zoom/pan + reset.
			if (IsFlatUvViewMode(vp.viewMode)) {
				ImGui::SameLine();
				char uvLabel[64];
				std::snprintf(uvLabel, sizeof(uvLabel), "uv %.2fx",
					m_uvZoom[i]);
				if (ImGui::SmallButton(uvLabel)) {
					m_uvZoom[i] = 1.0f;
					m_uvPan[i] = glm::vec2(0.0f);
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Pane zoom (click resets). MMB drag = pan, wheel = zoom.");
			}
			ImGui::SameLine();
			// Legend toggle: the overlay/legend only ever draws on the
			// ACTIVE panel, so its switch lives in the active chip.
			if (i == m_activeVp) {
				if (ImGui::SmallButton(m_showOverlay ? "legend on"
						: "legend off"))
					m_showOverlay = !m_showOverlay;
			}
			else {
				ImGui::TextDisabled("%s", i == m_activeVp ? "active" : "");
			}
		}
		ImGui::End();
		ImGui::PopID();
	}
	// Convex GI volume preview (LIGHTING 'V': off/wireframe/solid).
	DrawCvolPreviewOverlay();
	DrawPlacementOverlay();
}

// X-ray preview of the convex GI volumes (probes-only pivot): each
// volume's hull (bake AABB clipped by its plane set, built in
// TryLoadCvolumes) drawn in the ImGui FOREGROUND per viewport —
// wireframe at mode 1, + translucent priority-coded fill at mode 2.
// Foreground = no depth, so the volumes read through geometry (the
// point of the view: authoring shapes against walls/roofs).
// Authored cvolume nodes label here too (their boxes ride the 3D
// gizmo batches instead — depth-tested scene objects, all view
// modes): name + priority at the box center, same priority hue.
void EditorApp::DrawCvolPreviewOverlay() {
	const bool showBaked = m_cvolPreviewMode != 0
		&& !m_cvolPreviews.empty();
	const bool showAuthored = m_showCvolGizmos && m_world
		&& !m_world->scene.cvolForNode.empty();
	if (!showBaked && !showAuthored)
		return;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	// Priority -> stable hue (stacked override volumes read at a glance).
	const auto priorityColor = [](uint32_t priority, int alpha) {
		const float hue = std::fmod(float(priority) * 0.13f, 1.0f);
		ImColor c = ImColor::HSV(hue, 0.7f, 1.0f);
		c.Value.w = float(alpha) / 255.0f;
		return (ImU32)c;
	};
	for (int i = 0; i < ViewportCount(); ++i) {
		const EditorViewport& vp = m_viewports[i];
		if (IsFlatUvViewMode(vp.viewMode))
			continue;
		const glm::mat4 vpm = vp.viewProj;
		const auto project = [&](const glm::vec3& w, ImVec2& out) {
			const glm::vec4 c = vpm * glm::vec4(w, 1.0f);
			if (c.w <= 1e-6f)
				return false; // behind the camera
			out.x = vp.rect.x + (c.x / c.w * 0.5f + 0.5f) * float(vp.rect.w);
			out.y = vp.rect.y + (c.y / c.w * 0.5f + 0.5f) * float(vp.rect.h);
			return true;
		};
		dl->PushClipRect(ImVec2(vp.rect.x, vp.rect.y),
			ImVec2(vp.rect.x + vp.rect.w, vp.rect.y + vp.rect.h), true);
		if (showBaked)
		for (const CvolPreview& cv : m_cvolPreviews) {
			const ImU32 wire = priorityColor(cv.priority, 220);
			const ImU32 fill = priorityColor(cv.priority, 42);
			for (const auto& poly : cv.polys) {
				if (poly.size() < 3)
					continue;
				ImVec2 a, b;
				for (size_t k = 0; k < poly.size(); ++k)
					if (project(poly[k], a)
						&& project(poly[(k + 1) % poly.size()], b))
						dl->AddLine(a, b, wire, 1.5f);
				if (m_cvolPreviewMode >= 2) {
					ImVec2 c0, c1, c2;
					if (project(poly[0], c0))
						for (size_t k = 1; k + 1 < poly.size(); ++k)
							if (project(poly[k], c1) && project(poly[k + 1], c2))
								dl->AddTriangleFilled(c0, c1, c2, fill);
				}
			}
			ImVec2 c;
			if (project(cv.center, c)) {
				char label[96];
				std::snprintf(label, sizeof(label), "%s  p%u",
					cv.name.c_str(), cv.priority);
				dl->AddText(ImVec2(c.x + 4.0f, c.y - 8.0f), wire, label);
			}
		}
		if (showAuthored && m_world) {
			// Authored boxes already draw as 3D gizmo wires; the
			// overlay contributes the name/priority tag at the box
			// center (same priority hue both places).
			const Scene& scene = m_world->scene;
			for (const auto& [node, def] : scene.cvolForNode) {
				glm::vec3 mn, mx;
				if (!CvolBox(node, mn, mx))
					continue;
				ImVec2 c;
				if (project((mn + mx) * 0.5f, c)) {
					char label[128];
					std::snprintf(label, sizeof(label), "%s  p%u",
						getNodeName(scene, int(node)).c_str(),
						def.priority);
					dl->AddText(ImVec2(c.x + 4.0f, c.y - 8.0f),
						priorityColor(def.priority, 220), label);
				}
			}
		}
		dl->PopClipRect();
	}
}

// ---- panel resize splitters ---------------------------------------------
namespace {

// One splitter handle: a 6px grab strip. `flip` inverts the drag (the
// right/bottom panels grow toward the screen edge). Returns true on
// the release frame (deactivated) so the caller can commit (prefs).
bool SplitterHandle(const char* id, ImVec2 pos, ImVec2 sz, bool horizontal,
	float* value, float minV, float maxV, bool flip) {
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
	// API-sized strip: beat the style's 32x32 window minimum (it made
	// the 6px strips ~32px tall, swallowing the bottom dock's tab strip
	// — tab clicks hit the splitter instead) and kill the padding.
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	bool released = false;
	if (ImGui::Begin(id, nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoBackground)) {
		ImGui::InvisibleButton("##grab", sz);
		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(horizontal ? ImGuiMouseCursor_ResizeNS
				: ImGuiMouseCursor_ResizeEW);
		// Explicit held-state drag: the press belongs to THIS strip
		// (active id), the delta accumulates per frame while held, and
		// the release ends the gesture exactly once. The strip window
		// repositions from the value every frame, so it tracks the
		// mouse 1:1 through the whole gesture — no jumps on grab or
		// snap-back on release.
		if (ImGui::IsItemActive()
			&& ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
			const ImVec2 d = ImGui::GetIO().MouseDelta;
			const float delta = horizontal ? d.y : d.x;
			*value = std::clamp(*value + (flip ? -delta : delta),
				minV, maxV);
		}
		released = ImGui::IsItemDeactivatedAfterEdit();
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
	return released;
}

} // namespace

void EditorApp::DrawPanelSplitters() {
	// Thin grab strips between the panels and the viewport (drag to
	// resize; the constants in editor_app.h are the defaults). The
	// sizes commit to prefs on release, so a restart keeps the layout.
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	// m_menuH = menu bar + tool topbar (the panels start below both).
	const float menuH = m_menuH;
	// Column splitters stop at the bottom dock (drag there resizes the
	// side panel without fighting the dock's own handle).
	const float colH = ds.y - menuH - m_bottomDockH;
	bool committed = false;
	committed |= SplitterHandle("##splitL",
		ImVec2(m_outlinerW - 3.0f, menuH),
		ImVec2(6.0f, colH), false, &m_outlinerW, 200.0f, 720.0f, false);
	committed |= SplitterHandle("##splitR",
		ImVec2(ds.x - m_inspectorW - 3.0f, menuH),
		ImVec2(6.0f, colH), false, &m_inspectorW, 200.0f, 720.0f, true);
	committed |= SplitterHandle("##splitB",
		ImVec2(0.0f, ds.y - m_bottomDockH - 3.0f),
		ImVec2(ds.x, 6.0f), true, &m_bottomDockH, 120.0f, 600.0f, true);
	if (committed)
		SavePrefs();
}

} // namespace editor
