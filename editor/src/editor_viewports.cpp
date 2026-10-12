// Editor viewports: multisection state + rubber-band box select +
// viewport chrome (borders, camera combos, box rectangle) drawn with
// ImGui over the rendered viewports.

#include "editor_app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Renderer/cubemap.h>
#include <imgui.h>

namespace editor {

bool EditorApp::IsSelected(uint32_t node) const {
	return std::find(m_selection.begin(), m_selection.end(), node) !=
		m_selection.end();
}

void EditorApp::SetSelection(uint32_t node) {
	m_worldSelected = false;
	ClearNonDagSelection();
	// Node selection supersedes the ASSET-BROWSER selection (the
	// read-only asset inspector page shows only while an asset tile is
	// the current selection).
	m_assetsSel.clear();
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
	ImGui::TextDisabled("Gizmo space");
	{
		// Handle orientation (World default): World = scene axes, Local =
		// the node's rotated axes (scale is always local — no TRS form).
		const GizmoSpace spaces[2] = { GizmoSpace::World,
			GizmoSpace::Local };
		for (int i = 0; i < 2; ++i) {
			char label[64];
			std::snprintf(label, sizeof(label), "%s%s",
				GizmoSpaceName(spaces[i]),
				m_gizmoSpace == spaces[i] ? "  *" : "");
			if (ImGui::Selectable(label,
					m_gizmoSpace == spaces[i])) {
				m_gizmoSpace = spaces[i];
				Log(std::string("Gizmo space: ") + GizmoSpaceName(spaces[i])
					+ (spaces[i] == GizmoSpace::Local
						? " (node axes; scale stays local)" : ""));
				ImGui::CloseCurrentPopup();
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Translate/rotate align to the selected "
				"axes. Scale is always local (per-axis world scaling of "
				"a rotated node has no TRS form).");
	}
	ImGui::Separator();
	ImGui::TextDisabled("Gizmo pivot");
	{
		// Where the gizmo sits + the rotate/scale math origin (Node
		// default = as before; BoundsCenter = the selection's subtree
		// AABB center, rotate/scale act about it).
		const GizmoPivot pivots[2] = { GizmoPivot::Node,
			GizmoPivot::BoundsCenter };
		for (int i = 0; i < 2; ++i) {
			char label[64];
			std::snprintf(label, sizeof(label), "%s%s",
				GizmoPivotName(pivots[i]),
				m_gizmoPivot == pivots[i] ? "  *" : "");
			if (ImGui::Selectable(label, m_gizmoPivot == pivots[i])) {
				m_gizmoPivot = pivots[i];
				Log(std::string("Gizmo pivot: ") + GizmoPivotName(pivots[i]));
				ImGui::CloseCurrentPopup();
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Bounds center: the gizmo sits at the "
				"selection's subtree AABB center; rotate/scale act "
				"ABOUT it (translate compensates). Falls back to the "
				"node origin without mesh bounds.");
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
	m_assetsSel.clear(); // node/world selection supersedes the asset page
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
	// BACKGROUND list, not foreground: both composite above the Vulkan 3D
	// scene, but the foreground list renders above EVERY ImGui window —
	// the active viewport's (brighter, 2px) selection border painted over
	// panels, combo dropdowns and tooltips touching the viewport edge.
	// On the background list the chrome stays under all windows (a 1px
	// edge spill hides beneath the abutting sibling panel, which is
	// correct — panels are siblings, never covered by viewport chrome).
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const int count = ViewportCount();
	for (int i = 0; i < count; ++i) {
		const EditorViewport& vp = m_viewports[i];
		const bool active = (i == m_activeVp);
		dl->AddRect(ImVec2(vp.rect.x, vp.rect.y),
			ImVec2(vp.rect.x + vp.rect.w, vp.rect.y + vp.rect.h),
			active ? IM_COL32(120, 200, 255, 220) : IM_COL32(70, 70, 76, 180),
			0.0f, 0, active ? 2.0f : 1.0f);
	}
	// ASSET DROP TARGETS: while an asset drag is in flight (the browser's
	// mesh/material tiles), a transparent window covers each visible
	// viewport — drop a MESH asset to add a scene reference, a MATERIAL
	// asset to assign it to the selection. The overlay exists ONLY
	// during the drag (no clicks consumed otherwise; the highlight tint
	// shows the drop zone).
	if (const ImGuiPayload* payload = ImGui::GetDragDropPayload()) {
		const bool isMesh = payload->IsDataType("VT_ASSET_MESH");
		const bool isMat = payload->IsDataType("VT_ASSET_MAT");
		if (isMesh || isMat) {
			for (int i = 0; i < count; ++i) {
				const EditorViewport& vp = m_viewports[i];
				ImGui::SetNextWindowPos(
					ImVec2(vp.rect.x, vp.rect.y), ImGuiCond_Always);
				ImGui::SetNextWindowSize(
					ImVec2(vp.rect.w, vp.rect.h), ImGuiCond_Always);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
					ImVec2(0, 0));
				ImGui::PushStyleColor(ImGuiCol_WindowBg,
					(i == m_activeVp) ? ImVec4(0.25f, 0.45f, 0.7f, 0.25f)
					: ImVec4(0.2f, 0.2f, 0.25f, 0.2f));
				char wname[48];
				std::snprintf(wname, sizeof(wname), "##assetdrop%d", i);
				if (ImGui::Begin(wname, nullptr,
						ImGuiWindowFlags_NoDecoration |
						ImGuiWindowFlags_NoMove |
						ImGuiWindowFlags_NoSavedSettings |
						ImGuiWindowFlags_NoBringToFrontOnFocus)) {
					ImGui::InvisibleButton("##drop",
						ImVec2(vp.rect.w, vp.rect.h));
					if (ImGui::BeginDragDropTarget()) {
						if (const ImGuiPayload* accepted =
								ImGui::AcceptDragDropPayload(
									isMesh ? "VT_ASSET_MESH"
									: "VT_ASSET_MAT")) {
							const char* p =
								static_cast<const char*>(
									accepted->Data);
							const std::string asset(p,
								p + accepted->DataSize - 1);
							if (isMesh) {
								Log("Assets: dropping geometry asset '"
									+ asset + "' - placing its content "
									"(asset bind).");
								AddSceneReference(asset, {}, true);
							}
							else {
								AssignMaterialAsset(asset);
							}
						}
						ImGui::EndDragDropTarget();
					}
				}
				ImGui::End();
				ImGui::PopStyleColor();
				ImGui::PopStyleVar();
			}
		}
	}
	// Rubber-band box (window pixels), clipped to its viewport: a drag
	// past the viewport edge used to paint the selection tint over the
	// neighboring panels (same foreground-layer defect as the borders
	// above — now background + clipped).
	if (m_vpBoxDragging && m_vpBoxVp >= 0 && m_vpBoxVp < kMaxViewports) {
		const ViewportRect& vp = m_viewports[m_vpBoxVp].rect;
		dl->PushClipRect(ImVec2(vp.x, vp.y),
			ImVec2(vp.x + vp.w, vp.y + vp.h), true);
		const float bx0 = vp.x + std::min(m_vpBoxX0, m_vpBoxX1);
		const float by0 = vp.y + std::min(m_vpBoxY0, m_vpBoxY1);
		const float bx1 = vp.x + std::max(m_vpBoxX0, m_vpBoxX1);
		const float by1 = vp.y + std::max(m_vpBoxY0, m_vpBoxY1);
		dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1),
			IM_COL32(120, 200, 255, 28));
		dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1),
			IM_COL32(120, 200, 255, 200), 0.0f, 0, 1.5f);
		dl->PopClipRect();
	}
	//// Per-viewport camera picker + VIEW TYPE chip (UV views are panel
	//// types: any panel can show UV0/UV1/lightmap debug like a camera
	//// view). Top-left inside each rect.
	//for (int i = 0; i < count; ++i) {
	//	EditorViewport& vp = m_viewports[i];
	//	ImGui::SetNextWindowPos(
	//		ImVec2(vp.rect.x + 8.0f, vp.rect.y + 8.0f), ImGuiCond_Always);
	//	ImGui::SetNextWindowBgAlpha(1.0f);
	//	ImGui::PushID(i);
	//	char label[32];
	//	std::snprintf(label, sizeof(label), "##vpchip%d", i);
	//	if (ImGui::Begin(label, nullptr,
	//		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
	//		ImGuiWindowFlags_NoSavedSettings |
	//		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove |
	//		ImGuiWindowFlags_NoNav)) {
	//		// Camera combo: free orbit cameras, or a locked look-through
	//		// binding to a scene camera node (viewport follows the node
	//		// until released back to an orbit camera).
	//		std::string bindLabel;
	//		bool bound = false;
	//		uint32_t boundNode = UINT32_MAX;
	//		if (vp.boundNode >= 0 && m_world
	//			&& FindNodeCamera(uint32_t(vp.boundNode)) != nullptr) {
	//			bound = true;
	//			boundNode = uint32_t(vp.boundNode);
	//			bindLabel = "Cam: "
	//				+ getNodeName(m_world->scene, int(boundNode));
	//		}
	//		else {
	//			bindLabel = m_editorCams[vp.camIdx].name;
	//		}
	//		if (ImGui::BeginCombo("##cam", bindLabel.c_str())) {
	//			for (size_t c = 0; c < m_editorCams.size(); ++c)
	//				if (ImGui::Selectable(m_editorCams[c].name.c_str(),
	//						!bound && int(c) == vp.camIdx)) {
	//					vp.camIdx = int(c);
	//					vp.boundNode = -1;
	//				}
	//			// Scene cameras (hierarchy order, stable list).
	//			if (m_world && !m_world->scene.cameraForNode.empty()) {
	//				ImGui::Separator();
	//				std::vector<uint32_t> camNodes;
	//				for (const auto& [n, ci] : m_world->scene.cameraForNode) {
	//					(void)ci;
	//					if (n < m_world->scene.hierarchy.size()
	//						&& !m_deletedNodes.contains(n))
	//						camNodes.push_back(n);
	//				}
	//				std::sort(camNodes.begin(), camNodes.end());
	//				for (uint32_t n : camNodes) {
	//					const std::string item = "Cam: "
	//						+ getNodeName(m_world->scene, int(n));
	//					if (ImGui::Selectable(item.c_str(),
	//							bound && boundNode == n)) {
	//						vp.boundNode = int(n);
	//						Log("Viewport " + std::to_string(i)
	//							+ " look-through: '"
	//							+ getNodeName(m_world->scene, int(n))
	//							+ "' (#" + std::to_string(n) + ").");
	//					}
	//				}
	//			}
	//			else {
	//				ImGui::TextDisabled("no scene cameras (Create > Camera)");
	//			}
	//			ImGui::EndCombo();
	//		}
	//		if (ImGui::IsItemHovered())
	//			ImGui::SetTooltip(bound
	//				? "Locked look-through: viewport follows the camera "
	//				  "node (orbit nav parked; move the node instead). "
	//				  "Pick an orbit camera to release."
	//				: "Free orbit. Bind a scene camera node for locked "
	//				  "look-through.");
	//		if (bound) {
	//			ImGui::SameLine();
	//			ImGui::TextDisabled("[locked]");
	//		}
	//		ImGui::SameLine();
	//		// View type combo (this panel's shading/debug view). Flat UV
	//		// panes are not viewports anymore (separate UV editor
	//		// window) — 3D modes only here.
	//		if (ImGui::BeginCombo("##view", ViewModeName(vp.viewMode))) {
	//			for (int m = 0; m <= int(ViewMode::Pom); ++m) {
	//				const ViewMode vm = ViewMode(m);
	//				if (vm == ViewMode::FlatUv0 || vm == ViewMode::FlatUv1)
	//					continue;
	//				// Atlas views need the loaded lightmap GPU layers.
	//				if (ViewModeNeedsAtlas(vm) && !m_lmGpu)
	//					continue;
	//				if (ImGui::Selectable(ViewModeName(vm),
	//						int(vp.viewMode) == m)) {
	//					vp.viewMode = vm;
	//					if (vp.viewMode == ViewMode::Meshlets &&
	//						!m_meshletDebugHasData && !EnsureMeshletDebug())
	//						Log("Meshlet view: no cooked meshlets in this "
	//							"scene; meshes show as flat fallbacks.");
	//				}
	//			}
	//			ImGui::EndCombo();
	//		}
	//		ImGui::SameLine();
	//		// Selection-mode combo (Mesh/Face/Vertex highlight, Q radial).
	//		if (ImGui::BeginCombo("##selmode", SelModeName(m_selMode))) {
	//			for (int m = 0; m <= int(SelMode::Vertex); ++m) {
	//				if (ImGui::Selectable(SelModeName(SelMode(m)),
	//						int(m_selMode) == m)) {
	//					m_selMode = SelMode(m);
	//					ClearSubSelection();
	//					Log(std::string("Selection mode: ") +
	//						SelModeName(m_selMode) + " (highlight only).");
	//				}
	//			}
	//			ImGui::EndCombo();
	//		}
	//		if (ImGui::IsItemHovered())
	//			ImGui::SetTooltip("Q opens the selection radial.");
	//	}
	//	ImGui::End();
	//	ImGui::PopID();
	//}
	// Convex GI volume preview (LIGHTING 'V': off/wireframe/solid).
	DrawCvolPreviewOverlay();
	DrawCaptureBallOverlay();
	DrawOctreePreviewOverlay();
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
	// MODAL: GI authoring previews belong to LIGHTING mode.
	if (m_mode != EditorMode::Lighting)
		return;
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

// ---- octree PRECOOK visualization --------------------------------------
// In-process authored overlay (no baker subprocess): tiles the live
// scene AABB at the root-brick size, colors dirty tiles (need rebake)
// apart from clean ones, and draws cubemap captures as BLACK balls so
// the precook layout reads apart from the chrome bake preview.
// Partial-update model: UpdateOctreePreview() rebuilds the grid,
// MarkOctreeDirty() dirties every tile (called on scene edits + cooks).
void EditorApp::MarkOctreeDirty() {
	for (OctreeTile& t : m_octreeTiles)
		t.dirty = true;
}

void EditorApp::UpdateOctreePreview() {
	m_octreeTiles.clear();
	if (!m_world)
		return;
	// Scene AABB: node translations + authored cvol boxes (so empty
	// scenes still frame their volumes).
	glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
	bool any = false;
	const Scene& scene = m_world->scene;
	if (scene.globalTransform.size() == scene.hierarchy.size()) {
		for (size_t n = 0; n < scene.globalTransform.size(); ++n) {
			const glm::vec3 p(scene.globalTransform[n][3]);
			if (!std::isfinite(p.x + p.y + p.z))
				continue;
			mn = glm::min(mn, p);
			mx = glm::max(mx, p);
			any = true;
		}
	}
	for (const auto& [node, def] : scene.cvolForNode) {
		glm::vec3 bmn, bmx;
		if (!CvolBox(node, bmn, bmx))
			continue;
		mn = glm::min(mn, bmn);
		mx = glm::max(mx, bmx);
		any = true;
		(void)def;
	}
	if (!any) {
		mn = glm::vec3(-8.0f);
		mx = glm::vec3(8.0f);
	}
	// Pad one tile so captures on the boundary stay inside.
	const float pad = std::max(m_octreeTileSize, 1.0f);
	mn -= glm::vec3(pad);
	mx += glm::vec3(pad);
	m_octreeMn = mn;
	m_octreeMx = mx;
	const float ts = std::max(m_octreeTileSize, 0.5f);
	const int nx = std::clamp(int((mx.x - mn.x) / ts) + 1, 1, 32);
	const int ny = std::clamp(int((mx.y - mn.y) / ts) + 1, 1, 32);
	const int nz = std::clamp(int((mx.z - mn.z) / ts) + 1, 1, 32);
	if (nx * ny * nz > 4096) {
		// Scene too large for a per-tile overlay at this size: keep
		// the bounds box only (the GI panel says so).
		Log("Octree preview: grid capped ("
			+ std::to_string(nx * ny * nz) + " tiles) — bounds only.");
		return;
	}
	m_octreeTiles.reserve(size_t(nx * ny * nz));
	for (int ix = 0; ix < nx; ++ix)
		for (int iy = 0; iy < ny; ++iy)
			for (int iz = 0; iz < nz; ++iz) {
				OctreeTile t;
				t.mn = mn + glm::vec3(ix * ts, iy * ts, iz * ts);
				t.mx = glm::min(t.mn + glm::vec3(ts), mx);
				t.dirty = true; // fresh grid needs its bake
				m_octreeTiles.push_back(t);
			}
	Log("Octree preview: " + std::to_string(m_octreeTiles.size())
		+ " tiles (" + std::to_string(nx) + "x" + std::to_string(ny)
		+ "x" + std::to_string(nz) + " @ " + std::to_string(ts)
		+ "m) — all dirty.");
}

void EditorApp::DrawOctreePreviewOverlay() {
	// MODAL: GI authoring previews belong to LIGHTING mode.
	if (m_mode != EditorMode::Lighting)
		return;
	if (!m_octreePreviewOn || !m_world)
		return;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	for (int i = 0; i < ViewportCount(); ++i) {
		const EditorViewport& vp = m_viewports[i];
		const glm::mat4 vpm = vp.viewProj;
		const auto project = [&](const glm::vec3& w, ImVec2& out) {
			const glm::vec4 c = vpm * glm::vec4(w, 1.0f);
			if (c.w <= 1e-6f)
				return false;
			out.x = vp.rect.x + (c.x / c.w * 0.5f + 0.5f) * float(vp.rect.w);
			out.y = vp.rect.y + (c.y / c.w * 0.5f + 0.5f) * float(vp.rect.h);
			return true;
		};
		dl->PushClipRect(ImVec2(vp.rect.x, vp.rect.y),
			ImVec2(vp.rect.x + vp.rect.w, vp.rect.y + vp.rect.h), true);
		// Tiles: dirty = orange-red wire, clean = teal wire.
		for (const OctreeTile& t : m_octreeTiles) {
			const ImU32 col = t.dirty ? IM_COL32(255, 120, 40, 160)
				: IM_COL32(60, 220, 200, 120);
			const glm::vec3 c[8] = {
				{ t.mn.x, t.mn.y, t.mn.z }, { t.mx.x, t.mn.y, t.mn.z },
				{ t.mx.x, t.mx.y, t.mn.z }, { t.mn.x, t.mx.y, t.mn.z },
				{ t.mn.x, t.mn.y, t.mx.z }, { t.mx.x, t.mn.y, t.mx.z },
				{ t.mx.x, t.mx.y, t.mx.z }, { t.mn.x, t.mx.y, t.mx.z },
			};
			static const int kE[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 },
				{ 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
				{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
			for (const auto& e : kE) {
				ImVec2 a, b;
				if (project(c[e[0]], a) && project(c[e[1]], b))
					dl->AddLine(a, b, col, 1.0f);
			}
		}
		// World bounds: white wire.
		{
			const glm::vec3 mn = m_octreeMn, mx = m_octreeMx;
			const glm::vec3 c[8] = {
				{ mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z },
				{ mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
				{ mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z },
				{ mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
			};
			static const int kE[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 },
				{ 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
				{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
			for (const auto& e : kE) {
				ImVec2 a, b;
				if (project(c[e[0]], a) && project(c[e[1]], b))
					dl->AddLine(a, b, IM_COL32(255, 255, 255, 200), 2.0f);
			}
		}
		// Captures read through DrawCaptureBallOverlay (baked-aware balls,
		// always on with the cubemap gizmos) — the precook grid draws
		// tiles + bounds only so the two never double up.
		dl->PopClipRect();
	}
}

// Loaded cubemap probe rows parsed straight from the scene's manifest
// sidecar (see the header): the same rows the renderer loads at runtime.
bool EditorApp::CubemapProbeRowsFromFile(
	std::vector<CubemapProbeRow>& out) const {
	out.clear();
	if (m_gltfPath.empty() && m_gltfHostPath.empty())
		return false;
	namespace fs = std::filesystem;
	const std::string rel =
		smvk::CubemapManifestVfsPath(SceneIdForHost());
	std::vector<std::string> roots;
	if (!m_projectRoot.empty())
		roots.push_back(m_projectRoot);
	roots.push_back("content");
	for (const auto& root : roots) {
		const fs::path host = fs::path(root) / fs::path(rel);
		std::ifstream f(host, std::ios::binary | std::ios::ate);
		if (!f)
			continue;
		const auto end = f.tellg();
		if (end == std::ifstream::pos_type(-1) || end <= 0)
			return false;
		f.seekg(0);
		std::string text(static_cast<size_t>(end), '\0');
		if (!f.read(text.data(), static_cast<std::streamsize>(text.size())))
			return false;
		std::string mErr;
		std::vector<smvk::CubemapManifestEntry> rows;
		if (!smvk::CubemapManifestParse(text, rows, mErr))
			return false;
		out.reserve(rows.size());
		for (const auto& row : rows) {
			CubemapProbeRow r;
			r.center = row.position;
			r.influence = row.influenceRadius;
			r.fade = row.fadeRadius;
			out.push_back(r);
		}
		return !out.empty();
	}
	return false;
}

// Baked/unbaked census over the authored pins (overlay + cook dialog
// share it so the readout and the balls can never disagree).
void EditorApp::CaptureBakeState(size_t& bakedOut, size_t& totalOut) {
	bakedOut = 0;
	totalOut = 0;
	if (!m_world)
		return;
	totalOut = m_world->scene.cubemapForNode.size();
	if (totalOut == 0)
		return;
	std::vector<CubemapProbeRow> rows;
	if (!CubemapProbeRowsFromFile(rows))
		return;
	for (const auto& [node, def] : m_world->scene.cubemapForNode) {
		(void)def;
		if (node >= m_world->scene.globalTransform.size())
			continue;
		const glm::vec3 p(m_world->scene.globalTransform[node][3]);
		for (const auto& r : rows) {
			if (glm::distance(p, r.center) < kCaptureBakeMatchM) {
				++bakedOut;
				break;
			}
		}
	}
}

// Capture balls + App-parity probe overlay (LIGHTING mode only). Two
// halves sharing one viewport walk:
//   1. LOADED probes (the manifest sidecar rows - see
//      CubemapProbeRowsFromFile): center dot + C<idx> +
//      influence ring (solid) + fade ring (dim), camera-distance colored
//      (green = inside influence, yellow = in fade, gray = out of range),
//      capped at 64 within the overlay max distance. Answers "is there a
//      cubemap near X?" exactly like the App.
//   2. AUTHORED pins with no loaded probe behind them: black balls (the
//      editor-only half — "nothing rendered here yet"; bake to promote
//      them into half 1).
// The click twin lives in PickGizmoMarker.
void EditorApp::DrawCaptureBallOverlay() {
	// MODAL: the light-authoring set shows only in LIGHTING mode.
	if (m_mode != EditorMode::Lighting)
		return;
	if (!m_world)
		return;
	// Loaded probe rows (empty when no manifest sidecar - pins all read
	// unbaked, probes draw nothing).
	std::vector<CubemapProbeRow> rows;
	CubemapProbeRowsFromFile(rows);
	const bool showPins = m_showCubemapGizmos
		&& !m_world->scene.cubemapForNode.empty();
	if (!m_showProbeOverlay && !showPins)
		return;
	const auto pinBaked = [&](const glm::vec3& p) {
		for (const auto& r : rows)
			if (glm::distance(p, r.center) < kCaptureBakeMatchM)
				return true;
		return false;
	};
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	const Scene& scene = m_world->scene;
	const float maxDist = std::max(m_probeOverlayMaxDist, 1.0f);
	const float maxDist2 = maxDist * maxDist;
	for (int i = 0; i < ViewportCount(); ++i) {
		const EditorViewport& vp = m_viewports[i];
		const glm::mat4 vpm = vp.viewProj;
		glm::vec3 eye(0.0f);
		if (vp.camIdx >= 0
			&& size_t(vp.camIdx) < m_editorCams.size())
			eye = EditorCamEye(m_editorCams[size_t(vp.camIdx)]);
		const auto project = [&](const glm::vec3& w, ImVec2& out) {
			const glm::vec4 c = vpm * glm::vec4(w, 1.0f);
			if (c.w <= 1e-6f)
				return false; // behind the camera
			out.x = vp.rect.x + (c.x / c.w * 0.5f + 0.5f) * float(vp.rect.w);
			out.y = vp.rect.y + (c.y / c.w * 0.5f + 0.5f) * float(vp.rect.h);
			return true;
		};
		const auto projectInView = [&](const glm::vec3& w, ImVec2& out) {
			const glm::vec4 c = vpm * glm::vec4(w, 1.0f);
			if (c.w <= 1e-6f)
				return false;
			const glm::vec3 ndc = glm::vec3(c) / c.w;
			if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f)
				return false; // off-screen (the App culls the same way)
			out.x = vp.rect.x + (ndc.x * 0.5f + 0.5f) * float(vp.rect.w);
			out.y = vp.rect.y + (ndc.y * 0.5f + 0.5f) * float(vp.rect.h);
			return true;
		};
		dl->PushClipRect(ImVec2(vp.rect.x, vp.rect.y),
			ImVec2(vp.rect.x + vp.rect.w, vp.rect.y + vp.rect.h), true);
		// ---- half 1: App-parity probe overlay ----
		uint32_t drawn = 0;
		if (m_showProbeOverlay) {
			uint32_t ci = 0;
			for (const auto& pr : rows) {
				const uint32_t idx = ci++;
				if (drawn >= 64)
					break;
				const glm::vec3 dc = pr.center - eye;
				if (glm::dot(dc, dc) > maxDist2)
					continue;
				ImVec2 cpt;
				if (!projectInView(pr.center, cpt))
					continue;
				const float cdist = std::sqrt(glm::dot(dc, dc));
				ImU32 col;
				if (cdist <= pr.influence)
					col = IM_COL32(80, 255, 120, 230);
				else if (pr.fade > 0.0f
					&& cdist <= pr.influence + pr.fade)
					col = IM_COL32(255, 220, 80, 230);
				else
					col = IM_COL32(120, 140, 160, 160);
				// Influence ring: project a world-X offset of one radius
				// (the App's trick); fade ring scales from it.
				ImVec2 ept;
				if (projectInView(pr.center + glm::vec3(
						std::max(pr.influence, 0.01f), 0.0f, 0.0f), ept)) {
					const float dx = ept.x - cpt.x, dy = ept.y - cpt.y;
					const float rPx = std::sqrt(dx * dx + dy * dy);
					if (rPx > 2.0f && rPx < 4000.0f) {
						dl->AddCircle(cpt, rPx,
							col & IM_COL32(255, 255, 255, 90), 48);
						if (pr.fade > 0.0f) {
							const float fr = rPx * (pr.influence + pr.fade)
								/ std::max(pr.influence, 1e-3f);
							if (fr < 4000.0f)
								dl->AddCircle(cpt, fr,
									col & IM_COL32(255, 255, 255, 45), 48);
						}
					}
				}
				dl->AddCircleFilled(cpt, 3.5f, col);
				char clab[16];
				std::snprintf(clab, sizeof(clab), "C%u", idx);
				dl->AddText(ImVec2(cpt.x + 6.0f, cpt.y - 7.0f), col, clab);
				++drawn;
			}
			// Caption (the App draws it top-left of the full display;
			// per viewport here so quads stay readable).
			char ccap[96];
			std::snprintf(ccap, sizeof(ccap), "Cubemap probes: %u drawn",
				drawn);
			dl->AddText(ImVec2(vp.rect.x + 9.0f, vp.rect.y + 37.0f),
				IM_COL32(0, 0, 0, 200), ccap);
			dl->AddText(ImVec2(vp.rect.x + 8.0f, vp.rect.y + 36.0f),
				IM_COL32_WHITE, ccap);
		}
		// ---- half 2: authored pins with no loaded probe: black balls ----
		if (showPins) {
			for (const auto& [node, def] : scene.cubemapForNode) {
				(void)def;
				if (node >= scene.globalTransform.size())
					continue;
				const glm::vec3 p(scene.globalTransform[node][3]);
				if (pinBaked(p))
					continue; // the loaded-probe marker above owns it
				ImVec2 s;
				if (!project(p, s))
					continue;
				const bool selected = (node == m_selected);
				// Big readable balls: halo + thick ring so pins read on
				// any background.
				const float r = selected ? 17.0f : 13.0f;
				dl->AddCircleFilled(s, r + 5.0f,
					IM_COL32(255, 255, 255, 26));
				dl->AddCircleFilled(s, r, IM_COL32(0, 0, 0, 255));
				dl->AddCircle(s, r,
					selected ? IM_COL32(255, 220, 80, 255)
					: IM_COL32(255, 255, 255, 230), 0,
					selected ? 3.5f : 2.5f);
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

void EditorApp::DrawUvEditorWindow() {
	// UV EDITOR WINDOW (floating, separate from the viewports): chrome
	// here, canvas CONTENT from RecordUvWindowPass — which is added
	// right after DrawUI and before the imgui pass, so the fill sits
	// UNDER this window's chrome. Window/title backgrounds are pushed
	// alpha-0 and the canvas is an InvisibleButton (draws nothing):
	// the GPU clear in the canvas rect is the only background. Nav:
	// MMB drag pans, wheel zooms (centered), F resets (modal, hovered).
	// LMB click picks face/vertex in UV space (PickFlatUvWindow).
	m_uvWinHovered = false;
	if (!m_showUvEditor)
		return;
	ImGui::SetNextWindowSize(ImVec2(540.0f, 560.0f), ImGuiCond_FirstUseEver);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, ImVec4(0, 0, 0, 0));
	if (!ImGui::Begin("UV Editor", &m_showUvEditor)) {
		ImGui::End();
		ImGui::PopStyleColor(4);
		return;
	}
	// Top row: channel combo + zoom readout (click = reset).
	const char* chans[2] = { "TEXCOORD0 (diffuse)", "uv2 (lightmap)" };
	int cm = (m_uvWinMode == ViewMode::FlatUv1) ? 1 : 0;
	ImGui::SetNextItemWidth(170.0f);
	if (ImGui::Combo("##uvwinchan", &cm, chans, 2)) {
		if (cm == 1 && !m_lmGpu)
			Log("No lightmap atlas loaded - UV1 channel unavailable.");
		else
			m_uvWinMode = (cm == 1) ? ViewMode::FlatUv1 : ViewMode::FlatUv0;
	}
	ImGui::SameLine();
	char zl[48];
	std::snprintf(zl, sizeof(zl), "zoom %.2fx", m_uvWinZoom);
	if (ImGui::SmallButton(zl)) {
		m_uvWinZoom = 1.0f;
		m_uvWinPan = glm::vec2(0.0f);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Click resets (F). MMB drag = pan, wheel = zoom.");
	// Canvas: square centered in the remaining space (two hint rows are
	// reserved below). InvisibleButton draws NOTHING and yields the
	// hover/click state the nav + pick below consume.
	const float hintH = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 10.0f;
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float side = std::max(std::min(avail.x, avail.y - hintH), 0.0f);
	if (side >= 8.0f) {
		ImGui::InvisibleButton("##uvcanvas", ImVec2(side, side));
		if (ImGui::IsItemHovered()) {
			m_uvWinHovered = true;
			const ImGuiIO& io = ImGui::GetIO();
			if (io.MouseWheel != 0.0f) {
				m_uvWinZoom = std::clamp(
					m_uvWinZoom * (io.MouseWheel > 0.0f ? 1.15f : 1.0f / 1.15f),
					0.2f, 32.0f);
			}
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
				// Same units as the old flat pane: UV pan, mouse delta
				// scaled by zoom over the canvas side.
				const float z = (m_uvWinZoom > 1e-6f) ? m_uvWinZoom : 1.0f;
				m_uvWinPan.x -= io.MouseDelta.x / side * z;
				m_uvWinPan.y -= io.MouseDelta.y / side * z;
				m_uvWinPan.x = std::clamp(m_uvWinPan.x, -4.0f, 4.0f);
				m_uvWinPan.y = std::clamp(m_uvWinPan.y, -4.0f, 4.0f);
			}
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_world) {
				// Click = sub-object pick in UV space (canvas-local
				// mouse); shift-click on empty space keeps the selection.
				const ImVec2 c0 = ImGui::GetItemRectMin();
				const ImVec2 m = ImGui::GetMousePos();
				if (auto sub = PickFlatUvWindow(m.x - c0.x, m.y - c0.y))
					ApplySubSelection(*sub);
				else if (!ImGui::GetIO().KeyShift)
					ClearSelection();
			}
		}
		m_uvWinRectMin = ImGui::GetItemRectMin();
		m_uvWinRectMax = ImGui::GetItemRectMax();
	}
	else {
		m_uvWinRectMin = m_uvWinRectMax = ImVec2(0.0f, 0.0f);
	}
	ImGui::TextDisabled("wires: %s; yellow = face, cyan = vertex; no selection = empty",
		ViewModeName(m_uvWinMode));
	ImGui::TextDisabled("MMB drag = pan, wheel = zoom, F = reset, click = pick.");
	ImGui::End();
	ImGui::PopStyleColor(4);
}

} // namespace editor
