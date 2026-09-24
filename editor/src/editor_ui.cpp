// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"
#include "modal_bindings.h"

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iostream>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/pak.h>

#include <imgui.h>

#include <ktx.h>

#include <cmath>
#include <cstdint>

namespace editor {
// Panel widths/heights live in editor_app.h (kEditorLeftW/RightW/BottomH):
// the central viewport rect is computed from them in RenderFrame.

// Coalesced drag helpers (member functions for world access).
void EditorApp::FinalizePending() {
	if (!m_pending.active)
		return;
	EditCmd& c = m_pending.cmd;
	switch (c.verb) {
	case EditVerb::Translate:
	case EditVerb::Rotate:
	case EditVerb::Scale: {
		glm::vec3 t, r, s;
		ReadLocalTRS(c.node, t, r, s);
		if (c.verb == EditVerb::Translate)
			c.afterVec = t;
		else if (c.verb == EditVerb::Rotate)
			c.afterVec = r;
		else
			c.afterVec = s;
		break;
	}
	case EditVerb::SetMatAttr: {
		glm::vec4 v4;
		double num = 0.0;
		ReadSharedMatAttr(c.matWorld, c.attr, v4, num);
		c.vec4 = v4;
		c.number = num;
		break;
	}
	default:
		break;
	}
	PushDone(c);
	m_pending.active = false;
	m_pending.cmd = EditCmd{};
}

bool EditorApp::TrackDrag(EditVerb verb, uint32_t node,
	const glm::mat4& beforeMat) {
	if (ImGui::IsItemActivated()) {
		if (m_pending.active && (m_pending.cmd.verb != verb ||
			m_pending.cmd.node != node))
			FinalizePending(); // switching controls mid-gesture
		m_pending.active = true;
		m_pending.cmd = EditCmd{};
		m_pending.cmd.verb = verb;
		m_pending.cmd.node = node;
		m_pending.cmd.beforeMat = beforeMat;
		return true;
	}
	return m_pending.active && m_pending.cmd.verb == verb &&
		m_pending.cmd.node == node;
}

void EditorApp::FinishDrag() {
	PushDone(m_pending.cmd);
	m_pending.active = false;
	m_pending.cmd = EditCmd{};
}

void EditorApp::DrawUI(smvk::Extent2D targetExtent) {
	(void)targetExtent;
	if (ImGui::BeginMainMenuBar()) {
		// Tourmaline gem + name (branding; PNG lives as the window icon).
		{
			const float h = ImGui::GetTextLineHeight() + 2.0f;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 p = ImGui::GetCursorScreenPos();
			DrawTourmalineGem(dl, p, h);
			ImGui::Dummy(ImVec2(h * 0.62f + 6.0f, h));
			ImGui::TextUnformatted("Tourmaline Editor");
			ImGui::Separator();
		}
		DrawMenuBar();
		ImGui::EndMainMenuBar();
	}
	const float menuH = ImGui::GetFrameHeight();
	m_menuH = menuH; // cached for ComputeViewportRect (pre-NewFrame use)
	const float vh = float(ImGui::GetIO().DisplaySize.y);

	// Panels are siblings of the central viewport (never on top of it):
	// RenderFrame renders only the rect between them. I hides every
	// panel for a fullscreen viewport (the menu bar + overlay remain).
	if (m_uiMode) {
	ImGui::SetNextWindowPos(ImVec2(0, menuH), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(m_leftW, vh - menuH - m_bottomH),
		ImGuiCond_Always);
	if (ImGui::Begin("Outliner", nullptr,
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
		DrawOutliner();
	}
	ImGui::End();

	ImGui::SetNextWindowPos(
		ImVec2(float(ImGui::GetIO().DisplaySize.x) - m_rightW, menuH),
		ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(m_rightW, vh - menuH - m_bottomH),
		ImGuiCond_Always);
	if (ImGui::Begin("Inspector", nullptr,
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
		if (ImGui::BeginTabBar("##rightTabs")) {
			if (ImGui::BeginTabItem("Inspector")) {
				m_rightTab = 0;
				DrawInspector();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("UV")) {
				m_rightTab = 1;
				DrawUvPanel();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Cook")) {
				m_rightTab = 2;
				DrawCookPanel();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Bake")) {
				m_rightTab = 3;
				DrawLightmapPanel();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Textures")) {
				m_rightTab = 4;
				DrawTexturePanel();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();

	// Bottom dock (title-less, tabbed): the wide-override rule table
	// lives next to the Console so both stay full-width.
	ImGui::SetNextWindowPos(ImVec2(0, vh - m_bottomH), ImGuiCond_Always);
	ImGui::SetNextWindowSize(
		ImVec2(float(ImGui::GetIO().DisplaySize.x), m_bottomH),
		ImGuiCond_Always);
	if (ImGui::Begin("BottomDock", nullptr,
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoTitleBar)) {
		if (ImGui::BeginTabBar("##bottomTabs")) {
			if (ImGui::BeginTabItem("Overrides")) {
				m_bottomTab = 0;
				DrawOverridesPanel();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Console")) {
				m_bottomTab = 1;
				DrawConsolePanel();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
	} // m_uiMode

	DrawPanelSplitters(); // resize handles between panels and viewport
	DrawViewportOverlay(targetExtent);
	DrawViewportChrome(); // borders, camera chips, box-select rect
	DrawSelectionRadial(); // Q radial: Mesh/Face/Vertex highlight modes
	DrawPlayBar();
	DrawFlintbasePanel();
	DrawReferencesPanel(); // Maya-style reference list (View menu)
}

void EditorApp::DrawMenuBar() {
	if (ImGui::BeginMenu("File")) {
		// Single entry point: native dialog (.gltf/.pak/.tour, starts
		// in C:/), dispatched by extension (tours replay into the stack).
		if (ImGui::MenuItem("Open file...")) {
			std::string picked;
			if (NativeOpenFile(picked) && !picked.empty())
				OpenAnySceneFile(picked);
		}
		if (ImGui::MenuItem("Add scene reference...", nullptr, false,
				!m_gltfPath.empty() && !HasPakExtension(m_gltfPath))) {
			std::snprintf(m_refSourceBuf, sizeof(m_refSourceBuf), "%s", "");
			std::snprintf(m_refNameBuf, sizeof(m_refNameBuf), "%s", "");
			m_showRefDialog = true;
		}
		if (ImGui::MenuItem("Save tour...")) {
			std::snprintf(m_savePathBuf, sizeof(m_savePathBuf),
				"%s", TourTarget().c_str());
			m_showSaveDialog = true;
		}
		if (ImGui::MenuItem("Save tour incremental", "Ctrl+Alt+S"))
			SaveTourIncremental();
		if (ImGui::MenuItem("Cook to .pak"))
			CookToPak();
		if (ImGui::MenuItem("Run in App", nullptr, false,
			!m_runRunning && !RunArgs().empty()))
			RunInApp();
		ImGui::Separator();
		if (ImGui::MenuItem("Quit"))
			RequestClose();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Create")) {
		// Editor-authored punctual lights (point/spot): appended as world
		// nodes with pool definitions (undoable), persisted as
		// createLight/setLightAttr tour verbs so cooks replay them.
		const bool canCreate = m_world != nullptr;
		if (ImGui::MenuItem("Point light", nullptr, false, canCreate))
			DraftCreateLight(false);
		if (ImGui::MenuItem("Spot light", nullptr, false, canCreate))
			DraftCreateLight(true);
		if (!canCreate)
			ImGui::TextDisabled("open a scene first");
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Edit")) {
		const std::string ul = UndoLabel();
		const std::string rl = RedoLabel();
		if (ImGui::MenuItem(("Undo " + ul).c_str(), "Ctrl+Z", false,
			!ul.empty()))
			Undo();
		if (ImGui::MenuItem(("Redo " + rl).c_str(), "Ctrl+Shift+Z", false,
			!rl.empty()))
			Redo();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Mode")) {
		// vi-style modal operation: the ACTIVE mode's keymap interprets
		// keys (editor_modal.cpp); 1-4 switch modes from any mode.
		const struct {
			EditorMode m; const char* key; const char* hint;
		} modes[] = {
			{ EditorMode::Layout, "1", "object editing: gizmos, selection" },
			{ EditorMode::Lighting, "2", "sun/env + lightmap views" },
			{ EditorMode::Test, "3", "cook, run in App, debug views" },
			{ EditorMode::Profile, "4", "perf (stub)" },
		};
		for (const auto& e : modes)
			if (ImGui::MenuItem(EditorModeName(e.m), e.key, m_mode == e.m))
				SetMode(e.m);
		ImGui::Separator();
		ImGui::TextDisabled("Modal (vi-style): the active mode interprets");
		ImGui::TextDisabled("keys; the viewport legend shows the mode line.");
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("View")) {
		if (ImGui::MenuItem("Unlit (diffuse)", "F2",
			ActiveViewMode() == ViewMode::Unlit))
			ActiveViewMode() = ViewMode::Unlit;
		if (ImGui::MenuItem("Lit", "F1", ActiveViewMode() == ViewMode::Lit))
			ActiveViewMode() = ViewMode::Lit;
		if (ImGui::MenuItem("Lit + shadows", "F3",
			ActiveViewMode() == ViewMode::LitShadows))
			ActiveViewMode() = ViewMode::LitShadows;
		if (ImGui::MenuItem("Baked irradiance", "F8 (LIGHTING)",
			ActiveViewMode() == ViewMode::Baked, m_lmGpu))
			ActiveViewMode() = ViewMode::Baked;
		if (ImGui::MenuItem("Meshlets", "F4 (TEST)",
			ActiveViewMode() == ViewMode::Meshlets)) {
			ActiveViewMode() = ViewMode::Meshlets;
			if (!m_meshletDebugHasData && !EnsureMeshletDebug())
				Log("Meshlet view: no cooked meshlets in this scene "
					"(cook with --meshlets); meshes show as flat fallbacks.");
		}
		if (ActiveViewMode() == ViewMode::Meshlets) {
			const char* meshletModes[] = { "Random per meshlet",
				"Cone-cutoff heat" };
			ImGui::Combo("Meshlet color mode", &m_debugMeshletMode,
				meshletModes, 2);
		}
		ImGui::Separator();
		ImGui::MenuItem("Wireframe (all)", "Z (LAYOUT)", &m_wireframeAll);
		ImGui::Separator();
		if (ImGui::MenuItem("Single viewport", "V", m_vpLayout == 0))
			m_vpLayout = 0;
		if (ImGui::MenuItem("Quad viewports (4 panels)", "V",
				m_vpLayout == 1))
			m_vpLayout = 1;
		ImGui::Separator();
		ImGui::MenuItem("Ground grid", nullptr, &m_showGrid);
		{
			float cell = m_gridSize;
			if (ImGui::DragFloat("Grid cell (m)", &cell, 0.05f, 0.05f, 50.0f))
				m_gridSize = std::max(cell, 0.05f);
			ImGui::SetItemTooltip("Also the Shift-drag translate snap step.");
			int ext = int(m_gridExtent);
			if (ImGui::DragInt("Grid half-extent (cells)", &ext, 1, 2, 512))
				m_gridExtent = uint32_t(std::clamp(ext, 2, 512));
		}
		ImGui::Separator();
		{
			float gs = m_gizmoScale;
			if (ImGui::SliderFloat("Gizmo size", &gs, 0.02f, 0.3f, "%.3f"))
				m_gizmoScale = std::clamp(gs, 0.02f, 0.3f);
			ImGui::SetItemTooltip(
				"Screen-size factor of the camera distance (default 0.07, "
				"half the original 0.14).");
			ImGui::DragFloat("Rotate snap (deg)", &m_snapRotateDeg,
				1.0f, 1.0f, 90.0f);
			ImGui::DragFloat("Scale snap", &m_snapScaleStep, 0.01f, 0.01f, 1.0f);
			ImGui::TextDisabled("Shift while dragging = snap "
				"(translate: grid cell)");
		}
		ImGui::Separator();
		if (ImGui::MenuItem("UV0", "F9 (TEST)", ActiveViewMode() == ViewMode::Uv0))
			ActiveViewMode() = ViewMode::Uv0;
		if (ImGui::MenuItem("UV1 (lightmap)", "F10 (LIGHTING)", ActiveViewMode() == ViewMode::Uv1))
			ActiveViewMode() = ViewMode::Uv1;
		if (ImGui::MenuItem("Vertex colors", "F6 (TEST)",
			ActiveViewMode() == ViewMode::VertexColors))
			ActiveViewMode() = ViewMode::VertexColors;
		ImGui::Separator();
		if (ImGui::MenuItem(
				("UV inset: " + std::string(SecondViewName(m_secondView))).c_str(),
				"F5 (LIGHTING)"))
			m_secondView = SecondView((int(m_secondView) + 1) % 3);
		ImGui::Separator();
		if (ImGui::MenuItem("Translate gizmo", "W",
			m_gizmoMode == GizmoMode::Translate))
			m_gizmoMode = GizmoMode::Translate;
		if (ImGui::MenuItem("Rotate gizmo", "E",
			m_gizmoMode == GizmoMode::Rotate))
			m_gizmoMode = GizmoMode::Rotate;
		if (ImGui::MenuItem("Scale gizmo", "R",
			m_gizmoMode == GizmoMode::Scale))
			m_gizmoMode = GizmoMode::Scale;
		ImGui::Separator();
		if (ImGui::MenuItem("Selection radial...", "Q"))
			m_showSelRadial = true;
		if (ImGui::MenuItem("Select: Mesh", "Q,1",
			m_selMode == SelMode::Mesh)) {
			m_selMode = SelMode::Mesh;
			ClearSubSelection();
		}
		if (ImGui::MenuItem("Select: Face", "Q,2",
			m_selMode == SelMode::Face)) {
			m_selMode = SelMode::Face;
			ClearSubSelection();
		}
		if (ImGui::MenuItem("Select: Vertex", "Q,3",
			m_selMode == SelMode::Vertex)) {
			m_selMode = SelMode::Vertex;
			ClearSubSelection();
		}
		ImGui::TextDisabled("mode: %s (highlight only)", SelModeName(m_selMode));
		ImGui::Separator();
		if (ImGui::MenuItem("Frame selection", "F"))
			FrameSelection();
		if (ImGui::MenuItem("Frame all", "Shift+F"))
			FrameAll();
		if (IsFlatUvViewMode(ActiveViewMode()) &&
			ImGui::MenuItem("Reset UV pane (zoom 1)"))
			m_uvZoom[m_activeVp] = 1.0f,
			m_uvPan[m_activeVp] = glm::vec2(0.0f);
		ImGui::Separator();
		if (ImGui::MenuItem("Flintbase browser", nullptr, m_showFlintbasePanel))
			m_showFlintbasePanel = !m_showFlintbasePanel;
		if (ImGui::MenuItem("Reference editor", nullptr, m_showReferencesPanel))
			m_showReferencesPanel = !m_showReferencesPanel;
		ImGui::Separator();
		ImGui::MenuItem("Viewport legend", nullptr, &m_showOverlay);
		ImGui::MenuItem("Selection wire overlay", nullptr, &m_wireOverlay);
		if (ImGui::MenuItem("Light gizmos", "L (LIGHTING)", m_showLightGizmos))
			m_showLightGizmos = !m_showLightGizmos;
		{
			// Convex GI volume preview (LIGHTING 'J'): off/wire/solid.
			const char* cvNames[3] = { "Off", "Wireframe", "Solid" };
			if (ImGui::BeginCombo("GI volumes (J)",
					cvNames[m_cvolPreviewMode])) {
				for (int i = 0; i < 3; ++i)
					if (ImGui::Selectable(cvNames[i], m_cvolPreviewMode == i))
						m_cvolPreviewMode = i;
				ImGui::EndCombo();
			}
		}
		if (ImGui::MenuItem("Panels visible", nullptr, m_uiMode))
			m_uiMode = !m_uiMode;
		ImGui::EndMenu();
	}

	if (m_showRefDialog) {
		ImGui::OpenPopup("Add scene reference");
		m_showRefDialog = false;
	}
	DrawAddReferenceDialog();
	if (m_showSaveDialog) {
		ImGui::OpenPopup("Save tour as");
		m_showSaveDialog = false;
	}
	DrawSaveDialog();
}

void EditorApp::DrawSaveDialog() {
	namespace fs = std::filesystem;
	ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
	if (!ImGui::BeginPopupModal("Save tour as", nullptr,
		ImGuiWindowFlags_NoCollapse))
		return;
	auto closeDlg = [&]() {
		m_saveBrowseDir.clear();
		m_saveBrowseSel.clear();
		ImGui::CloseCurrentPopup();
	};
	if (m_saveBrowseDir.empty()) {
		// First frame: start from the target field's parent dir.
		std::error_code ec;
		fs::path d = fs::path(m_savePathBuf).parent_path();
		if (d.empty())
			d = fs::current_path(ec);
		if (!fs::is_directory(d, ec))
			d = ".";
		m_saveBrowseDir = d.string();
	}
	ImGui::InputText("##savepath", m_savePathBuf, sizeof(m_savePathBuf));
	ImGui::SameLine();
	if (ImGui::Button("Up")) {
		std::error_code ec;
		fs::path p(m_saveBrowseDir);
		if (p.has_parent_path()) {
			fs::path up = p.parent_path();
			if (fs::is_directory(up, ec))
				m_saveBrowseDir = up.string();
		}
		m_saveBrowseSel.clear();
	}
	ImGui::TextDisabled("%s", m_saveBrowseDir.c_str());
	ImGui::BeginChild("##savebrowse", ImVec2(0, -60), true);
	{
		std::error_code ec;
		std::vector<fs::directory_entry> dirs, files;
		for (fs::directory_iterator it(m_saveBrowseDir, ec), end;
			!ec && it != end; it.increment(ec)) {
			const fs::directory_entry& e = *it;
			if (e.is_directory(ec))
				dirs.push_back(e);
			else if (e.is_regular_file(ec) &&
				e.path().extension() == ".tour")
				files.push_back(e);
		}
		auto byName = [](const fs::directory_entry& a,
			const fs::directory_entry& b) {
			return a.path().filename().string() <
				b.path().filename().string();
		};
		std::sort(dirs.begin(), dirs.end(), byName);
		std::sort(files.begin(), files.end(), byName);
		if (ImGui::Selectable("[..]", false,
			ImGuiSelectableFlags_AllowDoubleClick)) {
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				fs::path p(m_saveBrowseDir);
				if (p.has_parent_path()) {
					fs::path up = p.parent_path();
					if (fs::is_directory(up, ec))
						m_saveBrowseDir = up.string();
				}
				m_saveBrowseSel.clear();
			}
		}
		char label[512];
		for (auto& d : dirs) {
			const std::string name = d.path().filename().string();
			std::snprintf(label, sizeof(label), "[%s]", name.c_str());
			if (ImGui::Selectable(label, false,
				ImGuiSelectableFlags_AllowDoubleClick)) {
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					m_saveBrowseDir = d.path().string();
					m_saveBrowseSel.clear();
				}
			}
		}
		for (auto& f : files) {
			const std::string full = f.path().string();
			const std::string name = f.path().filename().string();
			const bool sel = (m_saveBrowseSel == full);
			if (ImGui::Selectable(name.c_str(), sel,
				ImGuiSelectableFlags_AllowDoubleClick)) {
				m_saveBrowseSel = full;
				std::snprintf(m_savePathBuf, sizeof(m_savePathBuf),
					"%s", full.c_str());
			}
			if (sel)
				ImGui::SetItemDefaultFocus();
			if (ImGui::IsItemHovered() &&
				ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				if (SaveTourTo(full))
					Log("Tour saved: " + full);
				closeDlg();
				ImGui::EndChild();
				ImGui::EndPopup();
				return;
			}
		}
	}
	ImGui::EndChild();
	ImGui::TextDisabled("%s",
		m_saveBrowseSel.empty() ? "type a name or pick a file" : m_saveBrowseSel.c_str());
	if (ImGui::Button("Save")) {
		std::string target = m_saveBrowseSel.empty()
			? std::string(m_savePathBuf) : m_saveBrowseSel;
		if (!target.empty()) {
			if (fs::path(target).extension() != ".tour")
				target += ".tour";
			if (SaveTourTo(target))
				Log("Tour saved: " + target);
		}
		closeDlg();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
		closeDlg();
	ImGui::EndPopup();
}

std::string EditorApp::ResolveVfsRoot() const {
	// Prefer the content dir the current scene came from, else ./content.
	std::string posix = m_gltfPath;
	for (char& c : posix)
		if (c == '\\')
			c = '/';
	if (const size_t at = posix.find("content/");
		at != std::string::npos && !m_gltfPath.empty())
		return posix.substr(0, at + 7); // keep trailing "content/"
	std::error_code ec;
	if (std::filesystem::is_directory("content", ec))
		return "content";
	return ".";
}

bool EditorApp::PakSelected() const {
	// Pak options appear only for pak-related outliner selections: the
	// loaded-pak root / its TOC entries, or a .pak file on disk.
	if (m_outlinerTab != 1 || m_vfsSel.empty())
		return false;
	if (m_vfsSel == "pak:" || m_vfsSel.rfind("pak:", 0) == 0)
		return !m_pakPath.empty();
	if (m_vfsSel.rfind("disk:", 0) == 0) {
		const std::string p = m_vfsSel.substr(5);
		return p.size() >= 4 &&
			(p.compare(p.size() - 4, 4, ".pak") == 0 ||
				p.compare(p.size() - 4, 4, ".PAK") == 0);
	}
	return false;
}

void EditorApp::DrawOutliner() {
	if (ImGui::BeginTabBar("##outlinerTabs")) {
		if (ImGui::BeginTabItem("Scene")) {
			m_outlinerTab = 0;
			DrawSceneOutliner();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("VFS")) {
			m_outlinerTab = 1;
			DrawVfsOutliner();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void EditorApp::DrawSceneOutliner() {
	if (!m_world || m_world->scene.hierarchy.empty()) {
		ImGui::TextDisabled("No scene. File > Open scene... (.gltf/.pak)");
		return;
	}
	ImGui::InputTextWithHint("##filter", "filter nodes...",
		m_filterBuf, sizeof(m_filterBuf));
	const bool filtering = m_filterBuf[0] != '\0';
	ImGui::TextDisabled("nodes: %zu  selected: %s",
		m_world->scene.hierarchy.size(),
		m_selected == UINT32_MAX ? "-" : getNodeName(m_world->scene,
			int(m_selected)).c_str());
	ImGui::Separator();
	ImGui::Separator();
	ImGui::BeginChild("##tree");
	if (filtering) {
		for (uint32_t n = 0; n < uint32_t(m_world->scene.hierarchy.size()); ++n) {
			const std::string name = getNodeName(m_world->scene, int(n));
			if (name.find(m_filterBuf) == std::string::npos)
				continue;
			ImGui::PushID(int(n));
			const bool sel = IsSelected(n);
			char label[256];
			std::snprintf(label, sizeof(label), "%u: %s%s%s%s%s%s", n,
				name.empty() ? "(unnamed)" : name.c_str(),
				m_world->scene.meshForNode.contains(n) ? " [mesh]" : "",
				FindNodeLight(n) ? " [light]" : "",
				IsEnvNode(n) ? " [env]" : "",
				IsRefPlaceholder(n) ? " [ref]" : "",
				!IsVisibleNode(m_world->scene, int(n)) ? " [hidden]" : "");
			if (ImGui::Selectable(label, sel)) {
				// Click = replace selection; Ctrl/Shift-click = add.
				if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift)
					AddToSelection(n);
				else
					SetSelection(n);
				m_revealNode = n;
			}
			if (sel && n == m_revealNode)
				ImGui::SetScrollHereY(0.5f);
			ImGui::PopID();
		}
	}
	else {
		// Reveal a viewport pick: force-open the ancestor chain so the
		// selected node is visible, then scroll to it.
		std::unordered_set<uint32_t> expand;
		if (m_revealNode != UINT32_MAX) {
			uint32_t n = m_revealNode;
			while (n < uint32_t(m_world->scene.hierarchy.size())) {
				expand.insert(n);
				const int p = m_world->scene.hierarchy[n].parent;
				if (p < 0)
					break;
				n = uint32_t(p);
			}
		}
		for (int c = m_world->scene.hierarchy[0].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling)
			DrawOutlinerNode(uint32_t(c), expand.empty() ? nullptr : &expand);
	}
	ImGui::EndChild();
	m_revealNode = UINT32_MAX;
}

namespace {
ImVec4 VfsFileColor(const std::string& ext) {
	if (ext == ".gltf" || ext == ".GLTF")
		return ImVec4(0.45f, 0.9f, 1.0f, 1.0f);
	if (ext == ".pak" || ext == ".PAK")
		return ImVec4(1.0f, 0.65f, 0.25f, 1.0f);
	if (ext == ".sglm" || ext == ".sggi" || ext == ".sgrv")
		return ImVec4(0.8f, 0.55f, 1.0f, 1.0f);
	if (ext == ".tour")
		return ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
	return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
}
constexpr ImVec4 kVfsDirColor{ 1.0f, 0.85f, 0.4f, 1.0f };
constexpr ImVec4 kVfsInPakColor{ 1.0f, 0.65f, 0.25f, 1.0f };
} // namespace

void EditorApp::DrawVfsDir(const std::string& dir, int depth) {
	namespace fs = std::filesystem;
	if (depth > 6)
		return;
	std::error_code ec;
	std::vector<fs::directory_entry> dirs, files;
	for (fs::directory_iterator it(dir, ec), end;
		!ec && it != end; it.increment(ec)) {
		const fs::directory_entry& e = *it;
		if (e.is_directory(ec))
			dirs.push_back(e);
		else if (e.is_regular_file(ec))
			files.push_back(e);
	}
	auto byName = [](const fs::directory_entry& a,
		const fs::directory_entry& b) {
		return a.path().filename().string() <
			b.path().filename().string();
	};
	std::sort(dirs.begin(), dirs.end(), byName);
	std::sort(files.begin(), files.end(), byName);
	for (auto& d : dirs) {
		const std::string full = d.path().string();
		const std::string name = d.path().filename().string();
		ImGui::PushID(full.c_str());
		ImGui::PushStyleColor(ImGuiCol_Text, kVfsDirColor);
		const bool open = ImGui::TreeNodeEx(name.c_str(),
			ImGuiTreeNodeFlags_OpenOnArrow |
			ImGuiTreeNodeFlags_OpenOnDoubleClick);
		ImGui::PopStyleColor();
		if (open) {
			DrawVfsDir(full, depth + 1);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	size_t shown = 0;
	for (auto& f : files) {
		if (++shown > 500) {
			ImGui::TextDisabled("... (%zu more)", files.size() - 500);
			break;
		}
		const std::string full = f.path().string();
		const std::string name = f.path().filename().string();
		const std::string ext = f.path().extension().string();
		const bool openable = ext == ".gltf" || ext == ".GLTF" ||
			ext == ".pak" || ext == ".PAK";
		const bool sel = (m_vfsSel == "disk:" + full);
		ImGui::PushID(full.c_str());
		ImGui::PushStyleColor(ImGuiCol_Text, VfsFileColor(ext));
		if (ImGui::Selectable(name.c_str(), sel,
			ImGuiSelectableFlags_AllowDoubleClick))
			m_vfsSel = "disk:" + full;
		ImGui::PopStyleColor();
		if (sel)
			ImGui::SetItemDefaultFocus();
		if (openable && ImGui::IsItemHovered() &&
			ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			OpenScene(full);
		ImGui::PopID();
	}
}

void EditorApp::DrawVfsOutliner() {
	namespace fs = std::filesystem;
	if (m_vfsRoot.empty())
		m_vfsRoot = ResolveVfsRoot();
	ImGui::TextDisabled("legend:");
	ImGui::SameLine();
	ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "loose");
	ImGui::SameLine();
	ImGui::TextDisabled("/");
	ImGui::SameLine();
	ImGui::TextColored(kVfsInPakColor, "in pak");
	ImGui::TextDisabled("root: %s", m_vfsRoot.c_str());
	ImGui::BeginChild("##vfsTree", ImVec2(0, -110), true);
	// Loaded pak: virtual subtree from the cached reader (orange = inside;
	// never re-parsed per frame).
	if (!m_pakPath.empty() && m_pakReader) {
		const smvk::PakReader& reader = *m_pakReader;
		const std::string pakName =
			fs::path(m_pakPath).filename().string();
		ImGui::PushID("##loadedpak");
		ImGui::PushStyleColor(ImGuiCol_Text, kVfsInPakColor);
		const bool open = ImGui::TreeNodeEx(
			(" [pak] " + pakName).c_str(),
			ImGuiTreeNodeFlags_OpenOnArrow |
			ImGuiTreeNodeFlags_OpenOnDoubleClick);
		ImGui::PopStyleColor();
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
			!ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			m_vfsSel = "pak:";
		if (open) {
			// Scenes first: named, double-click loads that index.
			ImGui::PushID("##scenes");
			const bool sopen = ImGui::TreeNodeEx("Scenes",
				ImGuiTreeNodeFlags_OpenOnArrow);
			ImGui::SameLine();
			ImGui::TextDisabled("(%zu)", m_pakScenes.size());
			if (sopen) {
				for (size_t i = 0; i < m_pakScenes.size(); ++i) {
					const auto& si = m_pakScenes[i];
					char label[320];
					std::snprintf(label, sizeof(label), "[%zu] %s (%u nodes)%s",
						i, si.name.empty() ? "<unnamed>" : si.name.c_str(),
						si.nodes,
						i == m_pakSceneIndex ? "  [loaded]" : "");
					const bool sel = (m_vfsSel == "pak:" + si.tocPath);
					ImGui::PushID(si.tocPath.c_str());
					ImGui::PushStyleColor(ImGuiCol_Text, kVfsInPakColor);
					if (ImGui::Selectable(label, sel,
						ImGuiSelectableFlags_AllowDoubleClick))
						m_vfsSel = "pak:" + si.tocPath;
					ImGui::PopStyleColor();
					if (ImGui::IsItemHovered() &&
						ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
						uint32_t(i) != m_pakSceneIndex)
						LoadPakIntoWorld(m_pakPath, uint32_t(i));
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
			struct KindGroup {
				const char* label;
				smvk::PakAssetKind kind;
			};
			static const KindGroup kGroups[] = {
				{ "Meshes", smvk::PakAssetKind::Mesh },
				{ "Materials", smvk::PakAssetKind::Material },
				{ "Textures", smvk::PakAssetKind::Texture },
				{ "Lights", smvk::PakAssetKind::Light },
			};
			for (const auto& g : kGroups) {
				const auto blobs = reader.BlobsOfKind(g.kind);
				ImGui::PushID(g.label);
				const bool gopen = ImGui::TreeNodeEx(g.label,
					ImGuiTreeNodeFlags_OpenOnArrow);
				ImGui::SameLine();
				ImGui::TextDisabled("(%zu)", blobs.size());
				if (gopen) {
					size_t shown = 0;
					for (const smvk::PakTocEntry* e : blobs) {
						if (++shown > 200) {
							ImGui::TextDisabled("... (%zu more)",
								blobs.size() - 200);
							break;
						}
						const bool sel =
							(m_vfsSel == "pak:" + e->path);
						ImGui::PushID(e->path.c_str());
						ImGui::PushStyleColor(ImGuiCol_Text,
							kVfsInPakColor);
						if (ImGui::Selectable(
								fs::path(e->path).filename().string().c_str(),
								sel))
							m_vfsSel = "pak:" + e->path;
						ImGui::PopStyleColor();
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", e->path.c_str());
						ImGui::PopID();
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
		ImGui::Separator();
	}
	DrawVfsDir(m_vfsRoot, 0);
	ImGui::EndChild();
	DrawPakBox();
}

void EditorApp::DrawPakBox() {
	namespace fs = std::filesystem;
	if (!PakSelected())
		return; // pak options appear only on pak selection
	ImGui::Separator();
	ImGui::Text("Pak");
	if (m_vfsSel == "pak:") {
		ImGui::TextWrapped("%s", m_pakPath.c_str());
		if (ImGui::Button("Dump manifest"))
			DumpPakManifest();
		ImGui::SameLine();
		if (ImGui::Button("Reload sibling atlas"))
			TryLoadSiblingSglm(m_pakPath);
	}
	else if (m_vfsSel.rfind("pak:", 0) == 0) {
		ImGui::TextWrapped("in %s",
			fs::path(m_pakPath).filename().string().c_str());
		const std::string toc = m_vfsSel.substr(4);
		int sceneIdx = -1;
		for (size_t i = 0; i < m_pakScenes.size(); ++i)
			if (m_pakScenes[i].tocPath == toc)
				sceneIdx = int(i);
		if (sceneIdx >= 0) {
			ImGui::Text("scene[%d] '%s' (%u nodes)", sceneIdx,
				m_pakScenes[size_t(sceneIdx)].name.c_str(),
				m_pakScenes[size_t(sceneIdx)].nodes);
			if (uint32_t(sceneIdx) != m_pakSceneIndex &&
				ImGui::Button("Load scene"))
				LoadPakIntoWorld(m_pakPath, uint32_t(sceneIdx));
			else if (uint32_t(sceneIdx) == m_pakSceneIndex)
				ImGui::TextDisabled("currently loaded.");
		}
		else {
			ImGui::TextWrapped("%s", toc.c_str());
			ImGui::TextDisabled("entries inside a pak are read-only;");
			ImGui::TextDisabled("Dump manifest lists the full table.");
		}
		if (ImGui::Button("Dump manifest"))
			DumpPakManifest();
	}
	else if (m_vfsSel.rfind("disk:", 0) == 0) {
		const std::string full = m_vfsSel.substr(5);
		ImGui::TextWrapped("%s", full.c_str());
		std::error_code ec;
		const uint64_t bytes = fs::is_regular_file(full, ec)
			? uint64_t(fs::file_size(full, ec)) : 0;
		if (bytes != 0)
			ImGui::TextDisabled("%.1f MiB on disk",
				double(bytes) / (1024.0 * 1024.0));
		if (!m_pakPath.empty() &&
			fs::equivalent(full, m_pakPath, ec))
			ImGui::TextDisabled("already loaded.");
		else if (ImGui::Button("Load in editor"))
			OpenScene(full);
	}
}

void EditorApp::DrawOutlinerNode(uint32_t node,
	const std::unordered_set<uint32_t>* expandSet) {
	const Scene& scene = m_world->scene;
	const std::string name = getNodeName(scene, int(node));
	const bool hasMesh = scene.meshForNode.contains(node);
	const bool sel = IsSelected(node);
	ImGui::PushID(int(node));
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
		ImGuiTreeNodeFlags_OpenOnDoubleClick;
	if (sel)
		flags |= ImGuiTreeNodeFlags_Selected;
	if (scene.hierarchy[node].firstChild < 0)
		flags |= ImGuiTreeNodeFlags_Leaf;
	if (expandSet && expandSet->contains(node))
		ImGui::SetNextItemOpen(true);
	const ::SceneLight* nodeLight = FindNodeLight(node);
	char lightTag[16] = "";
	if (nodeLight) {
		std::snprintf(lightTag, sizeof(lightTag), " [%s]",
			nodeLight->type == ::SceneLight::Type::Directional ? "sun" :
			nodeLight->type == ::SceneLight::Type::Spot ? "spot" : "point");
	}
	char label[256];
	std::snprintf(label, sizeof(label), "%s%s%s%s%s%s%s%s", name.empty() ? "(node)" : name.c_str(),
		hasMesh ? " [mesh]" : "", lightTag,
		IsNoLightmapNode(scene, int(node)) ? " [noLM]" : "",
		IsDitherCloseupNode(scene, int(node)) ? " [dither]" : "",
		IsEnvNode(node) ? " [env]" : "",
		IsRefPlaceholder(node) ? " [ref]" : "",
		!IsVisibleNode(scene, int(node))
		? (IsHiddenNode(scene, int(node)) ? " [hidden]" : " [hidden*]") : "");
	const bool open = ImGui::TreeNodeEx(label, flags);
	// Drag source (reparent by drop).
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("EDITOR_NODE", &node, sizeof(node));
		ImGui::Text("%s", label);
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("EDITOR_NODE")) {
			const uint32_t src = *static_cast<const uint32_t*>(p->Data);
			EditCmd c{};
			c.verb = EditVerb::Parent;
			c.node = src;
			c.node2 = node;
			c.beforeParent = uint32_t(
				m_world->scene.hierarchy[src].parent);
			Do(c);
			Log("Reparented node " + std::to_string(src) + " under " +
				std::to_string(node));
		}
		ImGui::EndDragDropTarget();
	}
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
		if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift)
			AddToSelection(node);
		else
			SetSelection(node);
	}
	if (sel && node == m_revealNode)
		ImGui::SetScrollHereY(0.5f);
	if (open) {
		for (int c = scene.hierarchy[node].firstChild; c != -1;
			c = scene.hierarchy[c].nextSibling)
			DrawOutlinerNode(uint32_t(c), expandSet);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

// ---- Diffuse texture preview ------------------------------------------
// Resolves the material's baseColor slot (values.textures.x, an RHI pool
// index) back to its asset URI via the owning cluster's texture registry,
// then CPU-decodes the stashed .ktx2 source (same libktx path as the GI
// bakers) into a small sRGB thumbnail. Single-entry cache: selections
// change rarely, decodes cost a few ms once.
const EditorApp::TexPreview& EditorApp::ResolveDiffusePreview(uint32_t matWorld) {
	TexPreview& pv = m_texPreview;
	smvk::Material* mat = m_world ? m_world->GetMaterial(matWorld) : nullptr;
	smvk::Cluster* cluster = nullptr;
	int slot = -1;
	if (mat && matWorld < m_world->materialLocations.size()) {
		cluster = m_world->materialLocations[matWorld].cluster;
		slot = mat->values.textures.x;
	}
	std::string uri;
	const std::vector<uint8_t>* src = nullptr;
	if (cluster && slot >= 0) {
		for (const auto& [u, h] : cluster->ListTextureUris()) {
			if (int(h.index()) == slot) {
				uri = u.Str();
				break;
			}
		}
		if (!uri.empty())
			src = cluster->GetTextureSource(smvk::StringViewT(uri));
	}
	const size_t srcBytes = src ? src->size() : 0;
	if (pv.cluster == cluster && pv.slot == slot && pv.srcBytes == srcBytes &&
		(!pv.status.empty() || pv.ok))
		return pv; // cache hit (status is always set after a resolve)
	pv = TexPreview{};
	pv.cluster = cluster;
	pv.slot = slot;
	pv.srcBytes = srcBytes;
	if (!mat) {
		pv.status = "no material";
		return pv;
	}
	if (slot < 0) {
		pv.status = "no diffuse texture (factor only)";
		return pv;
	}
	if (!cluster) {
		pv.status = "no owning cluster";
		return pv;
	}
	if (uri.empty()) {
		pv.status = "slot has no registered texture";
		return pv;
	}
	pv.uri = uri;
	if (!src || src->empty()) {
		pv.status = "no stashed source bytes for '" + uri + "'";
		return pv;
	}
	ktxTexture2* raw = nullptr;
	if (ktxTexture2_CreateFromMemory(src->data(),
			static_cast<ktx_size_t>(src->size()),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw) != KTX_SUCCESS) {
		pv.status = "KTX parse failed";
		return pv;
	}
	struct Guard {
		ktxTexture2* t;
		~Guard() { if (t) ktxTexture2_Destroy(t); }
	} guard{ raw };
	if (ktxTexture2_NeedsTranscoding(raw)) {
		if (ktxTexture2_TranscodeBasis(raw, KTX_TTF_RGBA32, 0) != KTX_SUCCESS) {
			pv.status = "basis transcode failed";
			return pv;
		}
	}
	// 8-bit 1-4 channel data (transcoded RGBA32 lands here too: 4
	// comps, 4-byte texels). NOTE: GetElementSize is bytes per TEXEL,
	// not per component — RGBA8 reports 4.
	const ktx_uint32_t comps = ktxTexture2_GetNumComponents(raw);
	const ktx_size_t elemSize =
		ktxTexture_GetElementSize(reinterpret_cast<ktxTexture*>(raw));
	if (comps < 1 || comps > 4 || elemSize != ktx_size_t(comps)) {
		char fbuf[128];
		std::snprintf(fbuf, sizeof(fbuf),
			"unsupported pixel format (comps=%u elem=%u B)",
			comps, unsigned(elemSize));
		pv.status = fbuf;
		return pv;
	}
	// Smallest mip that still covers the thumbnail: less to downsample.
	ktx_uint32_t level = 0;
	while (level + 1 < raw->numLevels) {
		const ktx_uint32_t nw =
			std::max<ktx_uint32_t>(raw->baseWidth >> (level + 1), 1u);
		const ktx_uint32_t nh =
			std::max<ktx_uint32_t>(raw->baseHeight >> (level + 1), 1u);
		if (std::max(nw, nh) < 48)
			break;
		++level;
	}
	const ktx_uint32_t w =
		std::max<ktx_uint32_t>(raw->baseWidth >> level, 1u);
	const ktx_uint32_t h =
		std::max<ktx_uint32_t>(raw->baseHeight >> level, 1u);
	ktx_size_t offset = 0;
	if (ktxTexture2_GetImageOffset(raw, level, 0, 0, &offset) != KTX_SUCCESS) {
		pv.status = "mip offset failed";
		return pv;
	}
	const ktx_size_t rowPitch =
		ktxTexture_GetRowPitch(reinterpret_cast<ktxTexture*>(raw), level);
	if (rowPitch < ktx_size_t(w) * comps) {
		pv.status = "bad mip stride";
		return pv;
	}
	const uint8_t* base = raw->pData + offset;
	// Thumbnail grid: fit the long side into 44 texels, box-filtered.
	const float scale = std::min(1.0f, 44.0f / float(std::max(w, h)));
	pv.gridW = std::max<uint32_t>(uint32_t(std::round(float(w) * scale)), 1u);
	pv.gridH = std::max<uint32_t>(uint32_t(std::round(float(h) * scale)), 1u);
	pv.rgba.assign(size_t(pv.gridW) * pv.gridH, 0xFF000000u);
	for (uint32_t gy = 0; gy < pv.gridH; ++gy) {
		const uint32_t y0 = gy * h / pv.gridH, y1 = (gy + 1) * h / pv.gridH;
		for (uint32_t gx = 0; gx < pv.gridW; ++gx) {
			const uint32_t x0 = gx * w / pv.gridW, x1 = (gx + 1) * w / pv.gridW;
			uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
			for (uint32_t y = y0; y < y1; ++y) {
				const uint8_t* row = base + size_t(y) * size_t(rowPitch);
				for (uint32_t x = x0; x < x1; ++x) {
					const uint8_t* px = row + size_t(x) * comps;
					// Missing channels replicate sensibly (R/RG data shows
					// as grayscale-ish rather than black).
					r += px[0];
					g += (comps >= 2) ? px[1] : px[0];
					b += (comps >= 3) ? px[2] : px[0];
					a += (comps >= 4) ? px[3] : 255;
					++n;
				}
			}
			if (n == 0)
				n = 1;
			pv.rgba[size_t(gy) * pv.gridW + gx] =
				(uint32_t(r / n)) | (uint32_t(g / n) << 8) |
				(uint32_t(b / n) << 16) | (uint32_t(a / n) << 24);
		}
	}
	pv.imgW = w;
	pv.imgH = h;
	pv.ok = true;
	char buf[256];
	std::snprintf(buf, sizeof(buf), "%s (%ux%u mip %u)", uri.c_str(),
		raw->baseWidth, std::max<ktx_uint32_t>(raw->baseHeight, 1u),
		level);
	pv.status = buf;
	return pv;
}

void EditorApp::DrawDiffusePreview(uint32_t matWorld, const glm::vec4& tint) {
	const TexPreview& pv = ResolveDiffusePreview(matWorld);
	if (!pv.ok) {
		ImGui::ColorButton("##matPreview",
			ImVec4(tint.x, tint.y, tint.z, tint.w),
			ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip,
			ImVec2(64, 64));
		return;
	}
	// Aspect-kept thumbnail (long side 120 px) over a checkerboard so
	// alpha reads honestly; pixels are sRGB bytes, tinted by the
	// baseColor factor exactly like the lit shader multiplies map * factor.
	const float maxPx = 120.0f;
	const float dw = (pv.gridW >= pv.gridH)
		? maxPx : maxPx * float(pv.gridW) / float(pv.gridH);
	const float dh = (pv.gridH >= pv.gridW)
		? maxPx : maxPx * float(pv.gridH) / float(pv.gridW);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const float cw = dw / float(pv.gridW), ch = dh / float(pv.gridH);
	for (uint32_t y = 0; y < pv.gridH; ++y) {
		for (uint32_t x = 0; x < pv.gridW; ++x) {
			const uint32_t px = pv.rgba[size_t(y) * pv.gridW + x];
			const float a = float((px >> 24) & 255) / 255.0f;
			const float sr = float(px & 255) / 255.0f * tint.x;
			const float sg = float((px >> 8) & 255) / 255.0f * tint.y;
			const float sb = float((px >> 16) & 255) / 255.0f * tint.z;
			const bool light =
				((int(float(x) * cw / 12.0f) + int(float(y) * ch / 12.0f)) & 1) == 0;
			const float cb = light ? 0.65f : 0.3f;
			dl->AddRectFilled(
				ImVec2(p0.x + float(x) * cw, p0.y + float(y) * ch),
				ImVec2(p0.x + float(x + 1) * cw + 0.5f,
					p0.y + float(y + 1) * ch + 0.5f),
				IM_COL32(
					int((sr * a + cb * (1.0f - a)) * 255.0f),
					int((sg * a + cb * (1.0f - a)) * 255.0f),
					int((sb * a + cb * (1.0f - a)) * 255.0f), 255));
		}
	}
	ImGui::Dummy(ImVec2(dw, dh));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", pv.status.c_str());
}

static const char* AlphaModeName(smvk::AlphaMode m) {
	switch (m) {
	case smvk::AlphaMode::Opaque: return "Opaque";
	case smvk::AlphaMode::Mask: return "Mask (cutout)";
	case smvk::AlphaMode::Blend: return "Blend";
	}
	return "?";
}

// ---- material editor page (inspector travel) -------------------------
// m_materialFocus switches the Inspector to a material editor: full
// attribute set + layer slots + preview, with a back-trail to the node
// page ("travel between inspectors": layer slots jump focus too).
void EditorApp::DrawMaterialFocusPage() {
	if (!m_world || m_materialFocus >= m_world->materialLocations.size()) {
		m_materialFocus = UINT32_MAX;
		return;
	}
	const uint32_t mat = m_materialFocus;
	smvk::Material* m = m_world->GetMaterial(mat);
	if (!m) {
		m_materialFocus = UINT32_MAX;
		return;
	}
	// Breadcrumb: Back to node | trail of visited materials.
	if (ImGui::Button("< Back to node")) {
		m_materialFocus = UINT32_MAX;
		if (!m_materialTrail.empty()) {
			m_materialFocus = m_materialTrail.back();
			m_materialTrail.pop_back();
		}
	}
	ImGui::SameLine();
	ImGui::Text("Material mat.%u", mat);
	if (!m_materialTrail.empty()) {
		ImGui::TextDisabled("trail:");
		ImGui::SameLine();
		for (size_t i = 0; i < m_materialTrail.size() && i < 6; ++i) {
			ImGui::Text("mat.%u%s", m_materialTrail[i],
				i + 1 < m_materialTrail.size() ? " >" : "");
			ImGui::SameLine();
		}
		ImGui::Text("");
	}
	// Preview.
	ImGui::Separator();
	ImGui::Text("Preview (diffuse)");
	DrawDiffusePreview(mat, m->values.diffuse);
	// Attributes (shared: affects every node using this material).
	ImGui::Separator();
	ImGui::TextDisabled("Shared material: affects all users of mat.%u", mat);
	{
		glm::vec4 bc = m->values.diffuse;
		if (ImGui::ColorEdit4("Base color", glm::value_ptr(bc))) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
				m_pending.cmd.matWorld != mat || m_pending.cmd.attr != "baseColor"))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetMatAttr;
				m_pending.cmd.matWorld = mat;
				m_pending.cmd.attr = "baseColor";
				ReadSharedMatAttr(mat, "baseColor",
					m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
			}
			WriteSharedMatAttr(mat, "baseColor", bc, 0.0);
			m_pending.cmd.vec4 = bc;
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
			m_pending.cmd.attr == "baseColor") {
			FinishDrag();
		}
		float metallic = m->values.params.y;
		if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f)) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
				m_pending.cmd.matWorld != mat || m_pending.cmd.attr != "metallic"))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetMatAttr;
				m_pending.cmd.matWorld = mat;
				m_pending.cmd.attr = "metallic";
				ReadSharedMatAttr(mat, "metallic",
					m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
			}
			WriteSharedMatAttr(mat, "metallic", glm::vec4(0), metallic);
			m_pending.cmd.number = metallic;
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
			m_pending.cmd.attr == "metallic") {
			FinishDrag();
		}
		float rough = m->values.params.z;
		if (ImGui::SliderFloat("Roughness", &rough, 0.04f, 1.0f)) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
				m_pending.cmd.matWorld != mat || m_pending.cmd.attr != "roughness"))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetMatAttr;
				m_pending.cmd.matWorld = mat;
				m_pending.cmd.attr = "roughness";
				ReadSharedMatAttr(mat, "roughness",
					m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
			}
			WriteSharedMatAttr(mat, "roughness", glm::vec4(0), rough);
			m_pending.cmd.number = rough;
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
			m_pending.cmd.attr == "roughness") {
			FinishDrag();
		}
		glm::vec4 em(m->values.emissiveFactor.x, m->values.emissiveFactor.y,
			m->values.emissiveFactor.z, m->values.params2.z);
		if (ImGui::ColorEdit4("Emissive (w=strength)", glm::value_ptr(em))) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
				m_pending.cmd.matWorld != mat || m_pending.cmd.attr != "emissive"))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetMatAttr;
				m_pending.cmd.matWorld = mat;
				m_pending.cmd.attr = "emissive";
				ReadSharedMatAttr(mat, "emissive",
					m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
			}
			WriteSharedMatAttr(mat, "emissive", em, 0.0);
			m_pending.cmd.vec4 = em;
		}
		if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
			m_pending.cmd.attr == "emissive") {
			FinishDrag();
		}
	}
	// Layers (vertex-color blended): slots 1..3 + active count.
	ImGui::Separator();
	ImGui::Text("Layers (vertex-color blending)");
	ImGui::TextDisabled(
		"Layer 0 is this material; RGBA vertex weights blend 1..3. "
		"Count 0 = legacy tint.");
	{
		glm::ivec4 lm = m->values.layerMaterials;
		const uint32_t count = uint32_t(m_world->materialLocations.size());
		for (int slot = 0; slot < 3; ++slot) {
			ImGui::PushID(slot);
			int cur = lm[slot];
			std::string preview = (cur >= 0 && uint32_t(cur) < count)
				? ("mat." + std::to_string(cur)) : std::string("(none)");
			char comboLabel[32];
			std::snprintf(comboLabel, sizeof(comboLabel), "Layer %d", slot + 1);
			if (ImGui::BeginCombo(comboLabel, preview.c_str())) {
				if (ImGui::Selectable("(none)", cur < 0))
					cur = -1;
				for (uint32_t i = 0; i < count; ++i) {
					if (i == mat)
						continue; // this material is layer 0
					const auto& loc = m_world->materialLocations[i];
					std::string uri = loc.cluster
						? std::string(loc.cluster->GetMaterialUri(loc.handle).Str())
						: "";
					char item[320];
					std::snprintf(item, sizeof(item), "%u: %s", i,
						uri.empty() ? "(unnamed)" : uri.c_str());
					if (ImGui::Selectable(item, cur == int(i)))
						cur = int(i);
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
			if (cur != lm[slot]) {
				glm::ivec4 nl = lm;
				nl[slot] = cur;
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = mat;
				c.attr = "layers";
				glm::vec4 v4;
				double num = 0.0;
				ReadSharedMatAttr(mat, "layers", v4, num);
				c.beforeVec4 = v4;
				c.beforeNum = num;
				c.vec4 = glm::vec4(float(nl.x), float(nl.y), float(nl.z),
					float(nl.w));
				c.number = double(nl.w);
				Do(c);
			}
		}
		int layerCount = lm.w;
		if (ImGui::SliderInt("Active layers", &layerCount, 0, 4)) {
			glm::ivec4 nl = lm;
			nl.w = layerCount;
			EditCmd c{};
			c.verb = EditVerb::SetMatAttr;
			c.matWorld = mat;
			c.attr = "layers";
			glm::vec4 v4;
			double num = 0.0;
			ReadSharedMatAttr(mat, "layers", v4, num);
			c.beforeVec4 = v4;
			c.beforeNum = num;
			c.vec4 = glm::vec4(float(nl.x), float(nl.y), float(nl.z),
				float(nl.w));
			c.number = double(nl.w);
			Do(c);
		}
		// Travel: jump to a layer material (trail keeps the way back).
		for (int slot = 0; slot < 3; ++slot) {
			const int target = m->values.layerMaterials[slot];
			if (target < 0 || uint32_t(target) >=
				m_world->materialLocations.size())
				continue;
			ImGui::PushID(100 + slot);
			if (ImGui::SmallButton("Go to layer material >>")) {
				m_materialTrail.push_back(m_materialFocus);
				m_materialFocus = uint32_t(target);
			}
			ImGui::PopID();
		}
	}
	// Alpha / flags (same shared-edit path as the node page).
	ImGui::Separator();
	{
		bool ds = m->doubleSided;
		if (ImGui::Checkbox("Double sided", &ds)) {
			EditCmd c{};
			c.verb = EditVerb::SetMatAttr;
			c.matWorld = mat;
			c.attr = "doubleSided";
			c.number = ds ? 1.0 : 0.0;
			ReadSharedMatAttr(mat, "doubleSided", c.beforeVec4, c.beforeNum);
			Do(c);
		}
		bool cs = m->castsShadows;
		if (ImGui::Checkbox("Casts shadows", &cs)) {
			EditCmd c{};
			c.verb = EditVerb::SetMatAttr;
			c.matWorld = mat;
			c.attr = "castsShadows";
			c.number = cs ? 1.0 : 0.0;
			ReadSharedMatAttr(mat, "castsShadows", c.beforeVec4, c.beforeNum);
			Do(c);
		}
		int alpha = int(m->alphaMode);
		if (ImGui::Combo("Alpha", &alpha, "Opaque\0Mask (cutout)\0Blend\0")) {
			EditCmd c{};
			c.verb = EditVerb::SetMatAttr;
			c.matWorld = mat;
			c.attr = "alphaMode";
			c.number = double(alpha);
			ReadSharedMatAttr(mat, "alphaMode", c.beforeVec4, c.beforeNum);
			Do(c);
		}
		ImGui::TextDisabled("alpha: %s", AlphaModeName(m->alphaMode));
	}
}

void EditorApp::DrawInspector() {
	SetupDefaultCameras();
	// Material focus page: the Inspector is "traveling" in a material.
	if (m_world && m_materialFocus != UINT32_MAX) {
		DrawMaterialFocusPage();
		DrawEnvPanel();
		return;
	}
	if (!m_world || m_selected == UINT32_MAX ||
		m_selected >= m_world->scene.hierarchy.size()) {
		ImGui::TextDisabled("Nothing selected.");
		ImGui::TextWrapped("Click a mesh in the viewport or pick a node in the outliner. "
			"Drag nodes onto each other to reparent. All edits are undoable (Ctrl+Z).");
		DrawCameraSection();
		DrawEnvPanel();
		return;
	}
	Scene& scene = m_world->scene;
	const uint32_t node = m_selected;
	if (m_selection.size() > 1)
		ImGui::TextDisabled("multi-select: %zu nodes (primary #%u)",
			m_selection.size(), node);
	// Selection cycling (Up/Down arrows + Prev/Next option).
	{
		if (ImGui::Button("< Prev")) {
			SelectPrevNode();
			return;
		}
		ImGui::SameLine();
		if (ImGui::Button("Next >")) {
			SelectNextNode();
			return;
		}
		ImGui::SameLine();
		ImGui::Checkbox("Up/Down", &m_inspectorCycle);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"When on, the Up/Down arrow keys cycle the node selection.");
	}
	ImGui::Text("Node %u", node);
	// Sub-object highlight state (selection only, never edited).
	ImGui::TextDisabled("select mode: %s (Q radial)", SelModeName(m_selMode));
	if (m_selMode == SelMode::Face) {
		if (m_selFaceNode != UINT32_MAX && m_selFaceTri != UINT32_MAX)
			ImGui::Text("face: node %u tri %u (%.2f,%.2f,%.2f)",
				m_selFaceNode, m_selFaceTri,
				m_selFacePos.x, m_selFacePos.y, m_selFacePos.z);
		else
			ImGui::TextDisabled("face: none (click a triangle, Q for mesh)");
	}
	else if (m_selMode == SelMode::Vertex) {
		if (m_selVertNode != UINT32_MAX && m_selVertIdx != UINT32_MAX)
			ImGui::Text("vertex: node %u vert %u (%.2f,%.2f,%.2f)",
				m_selVertNode, m_selVertIdx,
				m_selVertPos.x, m_selVertPos.y, m_selVertPos.z);
		else
			ImGui::TextDisabled("vertex: none (click near a vertex)");
	}

	// Name.
	{
		char buf[256];
		const std::string cur = getNodeName(scene, int(node));
		std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
		if (ImGui::InputText("Name", buf, sizeof(buf),
			ImGuiInputTextFlags_EnterReturnsTrue)) {
			EditCmd c{};
			c.verb = EditVerb::Rename;
			c.node = node;
			c.afterText = buf;
			c.beforeText = cur;
			Do(c);
		}
	}

	// Transform (coalesced drags: one command per gesture).
	{
		glm::vec3 t, r, s;
		ReadLocalTRS(node, t, r, s);
		float ts[3] = { t.x, t.y, t.z };
		float rs[3] = { r.x, r.y, r.z };
		float ss[3] = { s.x, s.y, s.z };
		ImGui::DragFloat3("Translate", ts, 0.05f);
		const bool tOwn = 	TrackDrag( EditVerb::Translate, node,
			scene.localTransform[node]);
		if (tOwn) {
			WriteLocalTRS(node, glm::vec3(ts[0], ts[1], ts[2]), r, s);
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				m_pending.cmd.afterVec = glm::vec3(ts[0], ts[1], ts[2]);
				FinishDrag();
			}
		}
		ImGui::DragFloat3("Rotate (deg)", rs, 0.5f);
		const bool rOwn = 	TrackDrag( EditVerb::Rotate, node,
			scene.localTransform[node]);
		if (rOwn) {
			WriteLocalTRS(node, t, glm::vec3(rs[0], rs[1], rs[2]), s);
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				m_pending.cmd.afterVec = glm::vec3(rs[0], rs[1], rs[2]);
				FinishDrag();
			}
		}
		ImGui::DragFloat3("Scale", ss, 0.02f);
		const bool sOwn = 	TrackDrag( EditVerb::Scale, node,
			scene.localTransform[node]);
		if (sOwn) {
			WriteLocalTRS(node, t, r, glm::vec3(ss[0], ss[1], ss[2]));
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				m_pending.cmd.afterVec = glm::vec3(ss[0], ss[1], ss[2]);
				FinishDrag();
			}
		}
	}

	// Parent.
	{
		const int p = scene.hierarchy[node].parent;
		ImGui::Text("Parent: %d (%s)", p,
			p >= 0 ? getNodeName(scene, p).c_str() : "-");
		ImGui::SameLine();
		if (ImGui::Button("Detach to root") && p != int(m_clusterBase)) {
			if (m_clusterBase == UINT32_MAX) {
				Log("Detach: no cluster mapping (multi-cluster scene?)");
			}
			else {
				EditCmd c{};
				c.verb = EditVerb::Parent;
				c.node = node;
				c.node2 = m_clusterBase;
				c.beforeParent = uint32_t(p);
				Do(c);
			}
		}
	}

	// Mesh info.
	if (auto mit = scene.meshForNode.find(node); mit != scene.meshForNode.end()) {
		if (smvk::StaticMesh* mesh = m_world->GetMesh(mit->second)) {
			uint32_t tris = 0;
			for (auto& s : mesh->geometry.surfaces)
				tris += s.indexCount / 3;
			ImGui::Text("Mesh: %u verts, %u tris", mesh->geometry.vertexCount, tris);
			if (mesh->lmChunk == 0xFFFFFFFFu) {
				ImGui::Text("Lightmap: none (probe/sky lit)");
			}
			else {
				ImGui::Text("Lightmap: baked, %u lobes", mesh->lmChunk);
				if (m_hasSglm)
					ImGui::TextDisabled("atlas: %s (%ux%u)",
						m_sglmPath.c_str(), m_sglm.width, m_sglm.height);
				else
					ImGui::TextDisabled("atlas: not loaded (see panel below)");
			}
			ImGui::TextDisabled("UV1 view (F10) shows baked texCoord2");
			if (m_lmGpu)
				ImGui::TextDisabled("F8 shows baked irradiance in place");
		}
	}

	// Material reassignment.
	if (scene.materialForNode.contains(node)) {
		ImGui::Separator();
		ImGui::Text("Material");
		const uint32_t curMat = scene.materialForNode[node];
		std::string preview = "mat." + std::to_string(curMat);
		if (ImGui::BeginCombo("##mat", preview.c_str())) {
			const uint32_t count = uint32_t(m_world->materialLocations.size());
			for (uint32_t i = 0; i < count; ++i) {
				const auto& loc = m_world->materialLocations[i];
				std::string uri = loc.cluster
					? std::string(loc.cluster->GetMaterialUri(loc.handle).Str()) : "";
				char item[320];
				std::snprintf(item, sizeof(item), "%u: %s", i,
					uri.empty() ? "(unnamed)" : uri.c_str());
				const bool sel = (i == curMat);
				if (ImGui::Selectable(item, sel) && i != curMat) {
					EditCmd c{};
					c.verb = EditVerb::AssignMaterial;
					c.node = node;
					c.material = i;
					c.beforeMaterial = curMat;
					Do(c);
					Log("Node " + std::to_string(node) + " -> material " +
						std::to_string(i));
				}
				if (sel)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		// Travel: open this material in the material editor page.
		ImGui::SameLine();
		if (ImGui::SmallButton("Edit material >>")) {
			m_materialTrail.clear();
			m_materialFocus = curMat;
		}
		// 2D material preview: diffuse-map thumbnail (CPU KTX2 decode,
		// tinted by the baseColor factor) with an unlit swatch fallback
		// when the material carries no diffuse texture.
		if (smvk::Material* prevMat = m_world->GetMaterial(curMat)) {
			ImGui::Separator();
			ImGui::Text("Preview (diffuse)");
			const glm::vec4& bc = prevMat->values.diffuse;
			DrawDiffusePreview(curMat, bc);
			ImGui::SameLine();
			ImGui::BeginGroup();
			{
				const TexPreview& pv = ResolveDiffusePreview(curMat);
				if (pv.ok)
					ImGui::TextWrapped("map: %s", pv.uri.c_str());
				else
					ImGui::TextDisabled("map: %s", pv.status.c_str());
			}
			ImGui::Text("base %.2f %.2f %.2f a %.2f", bc.x, bc.y, bc.z, bc.w);
			ImGui::Text("metal %.2f  rough %.2f",
				prevMat->values.params.y, prevMat->values.params.z);
			ImGui::Text("emis %.2f %.2f %.2f x %.2f",
				prevMat->values.emissiveFactor.x,
				prevMat->values.emissiveFactor.y,
				prevMat->values.emissiveFactor.z,
				prevMat->values.params2.z);
			const glm::ivec4& t0 = prevMat->values.textures;
			const glm::ivec4& t1 = prevMat->values.textures2;
			ImGui::Text("tex base:%d mr:%d n:%d occ:%d e:%d",
				t0.x, t0.y, t0.z, t0.w, t1.x);
			ImGui::TextDisabled("alpha: %s cut %.2f%s%s",
				AlphaModeName(prevMat->alphaMode), prevMat->alphaCutoff,
				prevMat->doubleSided ? " DS" : "",
				prevMat->castsShadows ? "" : " noShadow");
			ImGui::EndGroup();
		}
		// Shared material quick edit (affects every node using it).
		if (smvk::Material* mat = m_world->GetMaterial(curMat)) {
			ImGui::TextDisabled("Shared edit: affects all users of mat.%u", curMat);
			glm::vec4 bc = mat->values.diffuse;
			if (ImGui::ColorEdit4("Base color", glm::value_ptr(bc))) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != curMat || m_pending.cmd.attr != "baseColor"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = curMat;
					m_pending.cmd.attr = "baseColor";
					ReadSharedMatAttr(curMat, "baseColor",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				WriteSharedMatAttr(curMat, "baseColor", bc, 0.0);
				m_pending.cmd.vec4 = bc;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "baseColor") {
				FinishDrag();
			}
			float metallic = mat->values.params.y;
			if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f)) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != curMat || m_pending.cmd.attr != "metallic"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = curMat;
					m_pending.cmd.attr = "metallic";
					ReadSharedMatAttr(curMat, "metallic",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				WriteSharedMatAttr(curMat, "metallic", glm::vec4(0), metallic);
				m_pending.cmd.number = metallic;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "metallic") {
				FinishDrag();
			}
			float rough = mat->values.params.z;
			if (ImGui::SliderFloat("Roughness", &rough, 0.04f, 1.0f)) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != curMat || m_pending.cmd.attr != "roughness"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = curMat;
					m_pending.cmd.attr = "roughness";
					ReadSharedMatAttr(curMat, "roughness",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				WriteSharedMatAttr(curMat, "roughness", glm::vec4(0), rough);
				m_pending.cmd.number = rough;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "roughness") {
				FinishDrag();
			}
			bool ds = mat->doubleSided;
			if (ImGui::Checkbox("Double sided", &ds)) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = curMat;
				c.attr = "doubleSided";
				c.number = ds ? 1.0 : 0.0;
				ReadSharedMatAttr(curMat, "doubleSided",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			bool cs = mat->castsShadows;
			if (ImGui::Checkbox("Casts shadows", &cs)) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = curMat;
				c.attr = "castsShadows";
				c.number = cs ? 1.0 : 0.0;
				ReadSharedMatAttr(curMat, "castsShadows",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			int alpha = int(mat->alphaMode);
			if (ImGui::Combo("Alpha", &alpha, "Opaque\0Mask (cutout)\0Blend\0")) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = curMat;
				c.attr = "alphaMode";
				c.number = double(alpha);
				ReadSharedMatAttr(curMat, "alphaMode",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			ImGui::TextDisabled("alpha: %s", AlphaModeName(mat->alphaMode));
		}
	}

	// Cooked light on this node (KHR punctual, import or pak).
	if (glm::vec3 aimWs(0.0f);
		const ::SceneLight* li = FindNodeLight(node, &aimWs)) {
		ImGui::Separator();
		const char* tname = li->type == ::SceneLight::Type::Directional
			? "Directional (sun)"
			: li->type == ::SceneLight::Type::Spot ? "Spot" : "Point";
		ImGui::Text("Light: %s '%s'", tname, li->name.c_str());
		ImGui::Text("color %.2f %.2f %.2f  I %.1f%s", li->color.x,
			li->color.y, li->color.z, li->intensity,
			li->type == ::SceneLight::Type::Directional ? " lux" : " cd");
		if (li->type == ::SceneLight::Type::Directional)
			ImGui::TextDisabled("toSun=(%.2f,%.2f,%.2f)%s", -aimWs.x,
				-aimWs.y, -aimWs.z,
				m_hasCookedSun && m_cookedSunName == li->name
				? " — seeds the editor IBL default" : "");
		else {
			const glm::vec3 p = node < scene.globalTransform.size()
				? glm::vec3(scene.globalTransform[node][3]) : glm::vec3(0.0f);
			ImGui::TextDisabled("pos=(%.2f,%.2f,%.2f) range=%.1f%s", p.x,
				p.y, p.z, li->range,
				li->range <= 0.0f ? " (infinite)" : "");
			if (li->type == ::SceneLight::Type::Spot)
				ImGui::TextDisabled("axis=(%.2f,%.2f,%.2f) inner=%.0fdeg outer=%.0fdeg",
					aimWs.x, aimWs.y, aimWs.z,
					glm::degrees(li->innerConeAngle),
					glm::degrees(li->outerConeAngle));
		}
		if ((li->flags & ::SceneLight::kSceneLightFlag_Static) != 0)
			ImGui::TextDisabled("STATIC: bake-only (excluded from runtime)");
		// ---- editable light properties (point/spot): SetLightAttr,
		// undoable, persisted as setLightAttr tour verbs so cooks replay
		// them. Sliders coalesce one command per drag (mattr pattern);
		// checkboxes commit immediately. ----
		if (li->type != ::SceneLight::Type::Directional) {
			const bool isSpot = li->type == ::SceneLight::Type::Spot;
			auto beginLightDrag = [&](const char* attr) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetLightAttr ||
					m_pending.cmd.node != node || m_pending.cmd.lightAttr != attr))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetLightAttr;
					m_pending.cmd.node = node;
					m_pending.cmd.lightAttr = attr;
					ReadNodeLightAttr(node, attr, m_pending.cmd.beforeVec4,
						m_pending.cmd.beforeNum, m_pending.cmd.beforeFlag,
						m_pending.cmd.beforeText);
				}
			};
			auto endLightDrag = [&](const char* attr) {
				if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
					m_pending.cmd.verb == EditVerb::SetLightAttr &&
					m_pending.cmd.node == node &&
					m_pending.cmd.lightAttr == attr)
					FinishDrag();
			};
			glm::vec3 lcol = li->color;
			if (ImGui::ColorEdit3("Light color", &lcol.x)) {
				beginLightDrag("color");
				WriteNodeLightAttr(node, "color", glm::vec4(lcol, 1.0f), 0, false, {});
				m_pending.cmd.vec4 = glm::vec4(lcol, 1.0f);
			}
			endLightDrag("color");
			float lint = li->intensity;
			if (ImGui::DragFloat("Intensity (cd)", &lint, 1.0f, 0.0f, 1e7f, "%.1f")) {
				beginLightDrag("intensity");
				WriteNodeLightAttr(node, "intensity", glm::vec4(0), lint, false, {});
				m_pending.cmd.number = lint;
			}
			endLightDrag("intensity");
			float lrange = li->range;
			if (ImGui::DragFloat("Range (m, 0 = inf)", &lrange, 0.1f, 0.0f, 500.0f, "%.1f")) {
				beginLightDrag("range");
				WriteNodeLightAttr(node, "range", glm::vec4(0), lrange, false, {});
				m_pending.cmd.number = lrange;
			}
			endLightDrag("range");
			if (isSpot) {
				float inner = glm::degrees(li->innerConeAngle);
				if (ImGui::SliderFloat("Inner cone (deg)", &inner, 0.0f, 89.0f, "%.1f")) {
					beginLightDrag("inner");
					WriteNodeLightAttr(node, "inner", glm::vec4(0), inner, false, {});
					m_pending.cmd.number = inner;
				}
				endLightDrag("inner");
				float outer = glm::degrees(li->outerConeAngle);
				if (ImGui::SliderFloat("Outer cone (deg)", &outer, 0.0f, 89.0f, "%.1f")) {
					beginLightDrag("outer");
					WriteNodeLightAttr(node, "outer", glm::vec4(0), outer, false, {});
					m_pending.cmd.number = outer;
				}
				endLightDrag("outer");
			}
			bool lstatic = (li->flags & ::SceneLight::kSceneLightFlag_Static) != 0;
			if (ImGui::Checkbox("Static (bake-only)", &lstatic)) {
				EditCmd c{};
				c.verb = EditVerb::SetLightAttr;
				c.node = node;
				c.lightAttr = "static";
				c.afterFlag = lstatic;
				ReadNodeLightAttr(node, "static", c.beforeVec4, c.beforeNum,
					c.beforeFlag, c.beforeText);
				Do(c);
			}
			if (isSpot) {
				bool lshadow = (li->flags & ::SceneLight::kSceneLightFlag_CastShadow) != 0;
				if (ImGui::Checkbox("Cast shadows", &lshadow)) {
					EditCmd c{};
					c.verb = EditVerb::SetLightAttr;
					c.node = node;
					c.lightAttr = "shadow";
					c.afterFlag = lshadow;
					ReadNodeLightAttr(node, "shadow", c.beforeVec4, c.beforeNum,
						c.beforeFlag, c.beforeText);
					Do(c);
				}
			} else {
				ImGui::TextDisabled("points cannot cast shadows (spot atlas only)");
			}
		}
	}

	// Lightmap participation.
	{
		ImGui::Separator();
		const bool inherited = IsNoLightmapNode(scene, int(node));
		bool self = node < scene.noLightmap.size() && scene.noLightmap[node] != 0;
		if (ImGui::Checkbox("Exclude from lightmaps (subtree)", &self)) {
			EditCmd c{};
			c.verb = EditVerb::SetNoLightmap;
			c.node = node;
			c.afterFlag = self;
			c.beforeFlag = !self;
			Do(c);
		}
		ImGui::TextDisabled("effective: %s (cook --lightmap skips it)",
			inherited ? "excluded" : "baked");
	}

	// Dither on camera closeup (subtree flag; GPU fade is TODO).
	{
		ImGui::Separator();
		const bool inherited = IsDitherCloseupNode(scene, int(node));
		bool self = node < scene.ditherCloseup.size() && scene.ditherCloseup[node] != 0;
		if (ImGui::Checkbox("Dither on camera closeup (subtree)", &self)) {
			EditCmd c{};
			c.verb = EditVerb::SetDitherCloseup;
			c.node = node;
			c.afterFlag = self;
			c.beforeFlag = !self;
			Do(c);
		}
		ImGui::TextDisabled("effective: %s", inherited ? "dithered" : "off");
	}

	// Clarisse-style override summary for this node (visibility control +
	// per-attr inherited/local/wide badges + wide-rule matches).
	DrawInspectorOverrides(node);

	// Environment node: mark/use this node as an IBL source (editor-only).
	{
		ImGui::Separator();
		ImGui::Text("Environment node");
		if (IsEnvNode(node)) {
			ImGui::TextDisabled("marked as Environment");
			if (ImGui::Button("Use as current IBL source"))
				m_envCurrent = node;
			ImGui::SameLine();
			if (ImGui::Button("Unmark##envnode"))
				UnmarkEnvNode(node);
		}
		else if (ImGui::Button("Mark as Environment node")) {
			MarkEnvNode(node);
			m_envCurrent = node;
		}
		ImGui::TextDisabled(
			"position = node translate, size = node scale "
			"(convex volume later; never cooked)");
	}

	// Scene reference block (placeholder nodes).
	DrawReferenceSection(node);

	// Editor camera (active viewport) + environment (IBL) settings.
	DrawCameraSection();
	DrawEnvPanel();
}

void EditorApp::DrawCameraSection() {
	ImGui::Separator();
	ImGui::Text("Editor camera (viewport %d)", m_activeVp);
	if (m_viewports[m_activeVp].camIdx >= int(m_editorCams.size()))
		return;
	EditorCam& cam = m_editorCams[m_viewports[m_activeVp].camIdx];
	int type = int(cam.type);
	if (ImGui::Combo("Projection", &type,
			"Perspective\0Orthographic\0")) {
		cam.type = EditorCam::Type(type);
		// Switching projection modes: give the ortho a sensible extent
		// from the current framing distance.
		if (cam.type == EditorCam::Type::Orthographic && cam.orthoHalfH < 0.1f)
			cam.orthoHalfH = std::max(cam.dist * 0.25f, 1.0f);
	}
	if (cam.type == EditorCam::Type::Perspective) {
		ImGui::SliderFloat("FOV (deg)", &cam.fovYDeg, 10.0f, 120.0f, "%.0f");
	}
	else {
		ImGui::DragFloat("Ortho half-height (m)", &cam.orthoHalfH,
			0.25f, 0.1f, 5000.0f);
	}
	ImGui::DragFloat("Near clip", &cam.nearClip, 0.005f, 0.001f, 50.0f, "%.3f");
	ImGui::DragFloat("Far clip", &cam.farClip, 5.0f, 10.0f, 100000.0f, "%.0f");
	if (cam.farClip <= cam.nearClip * 1.1f)
		cam.farClip = cam.nearClip * 1.1f + 10.0f;
}

void EditorApp::DrawCookPanel() {
	ImGui::Text("Cook to .pak (PakCooker --tour)");
	char pakBuf[512];
	std::snprintf(pakBuf, sizeof(pakBuf), "%s", m_cook.outPak.c_str());
	bool cookTouched = false;
	if (ImGui::InputText("Out pak", pakBuf, sizeof(pakBuf),
		ImGuiInputTextFlags_EnterReturnsTrue)) {
		m_cook.outPak = pakBuf;
		cookTouched = true;
	}
	if (ImGui::Checkbox("MeshOpt", &m_cook.meshopt))
		cookTouched = true;
	ImGui::SameLine();
	if (ImGui::Checkbox("LODs", &m_cook.lods))
		cookTouched = true;
	// Lightmaps are DEPRECATED (probes-only GI pivot): the cooker
	// force-disables the bake, so the checkbox is inert. Keep the row
	// visible for old tours (the flag persists in the cook block) but
	// grayed out.
	ImGui::BeginDisabled(true);
	ImGui::Checkbox("Lightmaps (bake)", &m_cook.lightmap);
	ImGui::EndDisabled();
	if (m_cook.lightmap) {
		m_cook.lightmap = false;
		cookTouched = true;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(deprecated: probes only - GiBaker .sggi + .cvol)");
	if (cookTouched)
		SaveTour(); // persist the cook block (not undoable)
	ImGui::TextDisabled("tour: %s (%zu commands)",
		TourTarget().empty() ? "-" : std::filesystem::path(TourTarget())
		.filename().string().c_str(),
		m_historyCursor);
	if (m_cookRunning) {
		ImGui::TextDisabled("Cooking... (see Console)");
	}
	else if (ImGui::Button("Cook")) {
		m_cook.outPak = pakBuf;
		CookToPak();
	}
}

void EditorApp::DrawLightmapPanel() {
	ImGui::Text("Lightmap bake (.pak + .sglm)");
	// Registry slots from the loaded world (first per kind wins, like App).
	if (m_world && !m_world->clusters.empty()) {
		for (const auto& clusterPtr : m_world->clusters) {
			if (!clusterPtr || clusterPtr->giBindings.empty())
				continue;
			for (const smvk::GiBinding& b : clusterPtr->giBindings)
				ImGui::TextDisabled("registry: %s '%s'",
					b.kind == smvk::GiBinding::Kind::Lightmap
					? "lightmap" : "probe", b.name.c_str());
			break; // first cluster carrying bindings (matches runtime)
		}
	}
	if (!m_pakPath.empty()) {
		ImGui::TextWrapped("pak: %s", m_pakPath.c_str());
	}
	else {
		ImGui::TextDisabled("No pak loaded (source is glTF: cook first).");
		ImGui::TextDisabled("Open a cooked .pak to inspect its bake.");
	}
	ImGui::TextWrapped("atlas: %s", m_sglmStatus.c_str());
	if (m_hasSglm) {
		uint32_t l5 = 0, l6 = 0, l9 = 0, l12 = 0;
		uint64_t texels = 0;
		for (size_t i = 0; i < m_sglm.chunks.size() && i < m_sglm.payloads.size(); ++i) {
			const uint32_t l = m_sglm.chunks[i].lobeCount;
			if (l == 5) ++l5;
			else if (l == 6) ++l6;
			else if (l == 9) ++l9;
			else ++l12;
			texels += uint64_t(m_sglm.payloads[i].width) * m_sglm.payloads[i].height;
		}
		ImGui::Text("chunks=%zu texels=%llu gpu=%.1f MiB",
			m_sglm.chunks.size(), (unsigned long long)texels,
			double(m_sglm.GpuBytes()) / (1024.0 * 1024.0));
		ImGui::Text("lobes: 5:%u 6:%u 9:%u 12:%u", l5, l6, l9, l12);
		// Per-selected-mesh atlas rects (meshKey == cook node id).
		if (m_selected != UINT32_MAX && m_world) {
			uint32_t rects = 0;
			for (const auto& c : m_sglm.chunks)
				if (c.meshKey == m_selected)
					++rects;
			ImGui::TextDisabled("selected node %u: %u atlas rects",
				m_selected, rects);
		}
		ImGui::TextDisabled("F10 = UV1 view (baked atlas uv: chart uv + id, unpacked in VS)");
		if (m_lmGpu && ImGui::Button("Baked irradiance view (F8)"))
			ActiveViewMode() = ViewMode::Baked;
		// Flat lightmap PNG export (whole scene, atlas resolution).
		ImGui::Separator();
		ImGui::Checkbox("Export with UV wireframe", &m_lmExportWires);
		ImGui::SetItemTooltip("Chart shell edges drawn over the export "
			"(whole scene, atlas uv2 layout).");
		ImGui::Checkbox("Export UV coordinates", &m_lmExportUv);
		ImGui::SetItemTooltip("Render the atlas uv itself (checker + "
			"gradient + stretch glow) instead of the baked content — "
			"the QA view of the parameterization.");
		if (!m_lmGpu)
			ImGui::BeginDisabled();
		if (ImGui::Button(m_lmExportUv ? "Export UV coords PNG"
			: "Export lightmap PNG")) {
			m_lmExportQueued = true;
			Log("Lightmap export queued: whole scene unwrapped by atlas "
				"uv2 (" + std::string(m_lmExportUv ? "uv coords"
					: "baked content")
				+ std::string(m_lmExportWires ? " + wireframe" : "") +
				") -> exports/<stem>.png");
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetItemTooltip(m_lmExportUv
				? "Whole scene unwrapped by its atlas uv2, uv rendered "
				"as color, saved at atlas resolution."
				: "Whole scene unwrapped by its atlas uv2 "
				"over the baked content, saved at atlas resolution.");
		if (!m_lmGpu) {
			ImGui::EndDisabled();
			ImGui::TextDisabled("export needs the atlas GPU layers "
				"(open the baked pak)");
		}
	}
	if (ImGui::InputText("sglm path", m_sglmBuf, sizeof(m_sglmBuf),
		ImGuiInputTextFlags_EnterReturnsTrue)) {
		if (m_sglmBuf[0] != '\0')
			LoadSglm(m_sglmBuf);
	}
	ImGui::SameLine();
	if (ImGui::Button("Load sglm") && m_sglmBuf[0] != '\0')
		LoadSglm(m_sglmBuf);
	if (!m_pakPath.empty())
		ImGui::TextDisabled(
			"Pak actions live on the pak selection (Outliner > VFS).");
}

void EditorApp::DumpPakManifest() {
	// Cached reader (parsed once at load; Open decompresses — never redo).
	if (!m_pakReader) {
		Log("Dump: no pak loaded.");
		return;
	}
	const smvk::PakReader& reader = *m_pakReader;
	for (size_t i = 0; i < m_pakScenes.size(); ++i)
		Log("  scene[" + std::to_string(i) + "] '" + m_pakScenes[i].name +
			"' nodes=" + std::to_string(m_pakScenes[i].nodes));
	if (m_world) {
		for (const auto& clusterPtr : m_world->clusters) {
			if (!clusterPtr || clusterPtr->giBindings.empty())
				continue;
			for (const smvk::GiBinding& b : clusterPtr->giBindings)
				Log(std::string("  gi ") +
					(b.kind == smvk::GiBinding::Kind::Lightmap
						? "lightmap '" : "probe '") + b.name + "'");
			break; // first cluster carrying bindings (matches runtime)
		}
	}
	const auto meshes = reader.BlobsOfKind(smvk::PakAssetKind::Mesh);
	const auto mats = reader.BlobsOfKind(smvk::PakAssetKind::Material);
	const auto texs = reader.BlobsOfKind(smvk::PakAssetKind::Texture);
	const auto lights = reader.BlobsOfKind(smvk::PakAssetKind::Light);
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		"Pak manifest '%s': entries=%zu meshes=%zu materials=%zu textures=%zu lights=%zu",
		m_pakPath.c_str(), reader.EntryCount(),
		meshes.size(), mats.size(), texs.size(), lights.size());
	Log(buf);
	// Light blobs, pak-sourced (same layout as the serializer writes:
	// str uri | u32 type | vec3 color | f32 I/range/inner/outer | str name
	// | u32 flags?). Verifies the sun decode the viewport uses.
	for (const smvk::PakTocEntry* e : lights) {
		const std::span<const uint8_t> img = reader.Payload(*e);
		size_t off = 0;
		auto u32 = [&]() -> uint32_t {
			uint32_t v = 0;
			if (off + 4 <= img.size()) {
				std::memcpy(&v, img.data() + off, 4);
				off += 4;
			}
			return v;
		};
		auto f32 = [&]() -> float {
			float v = 0.0f;
			if (off + 4 <= img.size()) {
				std::memcpy(&v, img.data() + off, 4);
				off += 4;
			}
			return v;
		};
		auto str = [&]() -> std::string {
			const uint32_t n = u32();
			if (off + n > img.size())
				return {};
			std::string s(reinterpret_cast<const char*>(img.data() + off), n);
			off += n;
			return s;
		};
		const std::string uri = str();
		const uint32_t type = u32();
		const float r = f32(), g = f32(), b = f32();
		const float inten = f32(), range = f32(), inner = f32(), outer = f32();
		const std::string name = str();
		const bool hasFlags = (off + 4 <= img.size());
		const uint32_t flags = hasFlags ? u32() : 0;
		char lbuf[384];
		std::snprintf(lbuf, sizeof(lbuf),
			"  light '%s' %s rgb=(%.2f,%.2f,%.2f) I=%.1f range=%.1f%s%s",
			name.empty() ? uri.c_str() : name.c_str(),
			type == 0 ? "dir" : type == 2 ? "spot" : "point",
			r, g, b, inten, range,
			type == 2 ? (" inner=" + std::to_string(glm::degrees(inner)) +
				"deg outer=" + std::to_string(glm::degrees(outer)) + "deg").c_str() : "",
			(flags & 1u) ? " STATIC" : "");
		Log(lbuf);
	}
	// lmChunk histogram straight from the mesh blobs (pak-sourced, same
	// cursor as PakCooker --dump-manifest). The editor never draws LODs,
	// so LOD0 only: "<base>/lodN" sibling blobs are runtime alternates
	// with their own per-LOD atlas rects — counting them would skew the
	// histogram away from what every editor lightmap view samples.
	uint32_t lmNone = 0, lm5 = 0, lm6 = 0, lm9 = 0, lm12 = 0, lmOther = 0;
	uint32_t lmLodSkipped = 0;
	for (const smvk::PakTocEntry* e : meshes) {
		const std::span<const uint8_t> img = reader.Payload(*e);
		size_t off = 0;
		auto u32 = [&]() -> uint32_t {
			uint32_t v = 0;
			if (off + 4 <= img.size()) {
				std::memcpy(&v, img.data() + off, 4);
				off += 4;
			}
			return v;
		};
		auto f32 = [&]() -> float {
			float v = 0.0f;
			if (off + 4 <= img.size()) {
				std::memcpy(&v, img.data() + off, 4);
				off += 4;
			}
			return v;
		};
		const uint32_t n = u32();
		if (off + n > img.size())
			continue;
		const std::string uri(reinterpret_cast<const char*>(img.data() + off), n);
		off += n; // uri
		// LOD sibling blob ("<base>/lodN"): not drawn by the editor,
		// not part of its lightmap sampling — skip (counted below).
		if (smvk::BaseMeshUriForLod(smvk::StringViewT(uri)) != smvk::StringViewT(uri)) {
			++lmLodSkipped;
			continue;
		}
		const uint32_t vc = u32(), ic = u32(), sc = u32();
		(void)vc;
		(void)ic;
		off += size_t(sc) * 12; // surfaces
		for (int k = 0; k < 6; ++k)
			(void)f32(); // aabb
		(void)f32(); // sphere
		(void)u32(); // topology
		(void)u32(); // groupId
		(void)u32(); // lodIndex
		(void)u32(); // candidates
		(void)f32(); // simplifyError
		(void)f32(); // screenThreshold
		(void)u32(); // tris
		(void)f32(); // acmr
		if (off + 4 > img.size())
			continue;
		const uint32_t lm = u32();
		if (lm == 0xFFFFFFFFu) ++lmNone;
		else if (lm == 5) ++lm5;
		else if (lm == 6) ++lm6;
		else if (lm == 9) ++lm9;
		else if (lm == 12) ++lm12;
		else ++lmOther;
	}
	std::snprintf(buf, sizeof(buf),
		"  lmChunk: none=%u 5=%u 6=%u 9=%u 12=%u other=%u (LOD0 only, %u siblings skipped)",
		lmNone, lm5, lm6, lm9, lm12, lmOther, lmLodSkipped);
	Log(buf);
	Log("For per-mesh lmChunk + texture URIs use: PakCooker --dump-pak <pak>");
}

void EditorApp::DrawTexturePanel() {
	ImGui::Text("Textures");
	if (!m_world || m_world->scene.hierarchy.empty()) {
		ImGui::TextDisabled("No scene loaded.");
		return;
	}
	if (!m_pakPath.empty()) {
		ImGui::TextDisabled("pak scene: full-resident (no streaming).");
		return;
	}
	int mode = int(m_texMode);
	if (ImGui::Combo("Mode", &mode,
			"Streaming (distance demand)\0Coarsest mip only\0Full (reimport)\0")) {
		m_texMode = TexMode(mode);
		ApplyTexMode(); // reimports the glTF scene in the new mode
	}
	if (!m_textureStreamer) {
		ImGui::TextDisabled("full-resident (no streaming).");
		return;
	}
	if (m_texMode == TexMode::Coarse)
		ImGui::TextDisabled("floor: single coarsest mip (flat colors).");
	const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
	ImGui::Text("%u tex  %.1f/%.1f MiB  full-res %u",
		stats.textureCount,
		double(stats.residentBytes) / (1024.0 * 1024.0),
		double(stats.fullBytes) / (1024.0 * 1024.0),
		stats.fullResidentCount);
	if (ImGui::Button("Dump residency"))
		DumpStreamerStats();
}

void EditorApp::DrawUvPanel() {
	ImGui::Text("UV (selection)");
	if (!m_world || m_selected == UINT32_MAX ||
		m_selected >= m_world->scene.hierarchy.size()) {
		ImGui::TextDisabled("Nothing selected.");
		ImGui::TextWrapped("Click a mesh in the viewport or pick a node "
			"(Scene tab). Baked views need a cooked pak + atlas.");
		return;
	}
	Scene& scene = m_world->scene;
	const uint32_t node = m_selected;
	ImGui::Text("Node %u %s", node,
		getNodeName(scene, int(node)).c_str());
	// Mesh lightmap state.
	if (auto mit = scene.meshForNode.find(node);
		mit != scene.meshForNode.end()) {
		if (smvk::StaticMesh* mesh = m_world->GetMesh(mit->second)) {
			uint32_t tris = 0;
			for (auto& s : mesh->geometry.surfaces)
				tris += s.indexCount / 3;
			ImGui::Text("Mesh: %u verts, %u tris",
				mesh->geometry.vertexCount, tris);
			if (mesh->lmChunk == 0xFFFFFFFFu)
				ImGui::Text("Lightmap: none (probe/sky lit)");
			else
				ImGui::Text("Lightmap: baked, %u lobes", mesh->lmChunk);
		}
	}
	else {
		ImGui::TextDisabled("No mesh on this node.");
	}
	// Atlas rects owned by this node (meshKey == cook node id).
	if (m_hasSglm) {
		uint32_t rects = 0;
		for (const auto& c : m_sglm.chunks)
			if (c.meshKey == node)
				++rects;
		ImGui::Text("Atlas rects: %u", rects);
		uint32_t shown = 0;
		for (size_t i = 0; i < m_sglm.chunks.size() && shown < 12; ++i) {
			const auto& c = m_sglm.chunks[i];
			if (c.meshKey != node)
				continue;
			ImGui::TextDisabled("  chunk %zu: (%.0f,%.0f)-(%.0f,%.0f) %u lobes",
				i, c.rectMin[0], c.rectMin[1], c.rectMax[0], c.rectMax[1],
				c.lobeCount);
			++shown;
		}
		if (rects > shown)
			ImGui::TextDisabled("  ... (%u more)", rects - shown);
	}
	else {
		ImGui::TextDisabled("No atlas loaded.");
	}
	// Material texture slots on this node.
	if (auto matIt = scene.materialForNode.find(node);
		matIt != scene.materialForNode.end()) {
		if (smvk::Material* mat = m_world->GetMaterial(matIt->second)) {
			const glm::ivec4& t0 = mat->values.textures;
			ImGui::TextDisabled("maps: base:%d mr:%d normal:%d occ:%d emis:%d",
				t0.x, t0.y, t0.z, t0.w, mat->values.textures2.x);
		}
	}
	ImGui::Separator();
	ImGui::Text("Flat pane navigation (area mode)");
	{
		const int vi = std::clamp(m_activeVp, 0, kMaxViewports - 1);
		float z = m_uvZoom[vi];
		if (ImGui::SliderFloat("Zoom", &z, 0.2f, 32.0f, "%.2f"))
			m_uvZoom[vi] = std::clamp(z, 0.2f, 32.0f);
		glm::vec2 p = m_uvPan[vi];
		float pp[2] = { p.x, p.y };
		if (ImGui::DragFloat2("Pan (UV)", pp, 0.01f, -4.0f, 4.0f))
			m_uvPan[vi] = glm::vec2(pp[0], pp[1]);
		if (ImGui::Button("Reset pane (F)")) {
			m_uvZoom[vi] = 1.0f;
			m_uvPan[vi] = glm::vec2(0.0f);
		}
		ImGui::TextDisabled("MMB drag = pan, wheel = zoom, click = pick.");
		ImGui::TextDisabled("Yellow = face, cyan = vertex highlight.");
	}
	ImGui::Separator();
	ImGui::Text("Views");
	if (ImGui::Button("UV0 fullscreen (F9)"))
		ActiveViewMode() = ViewMode::Uv0;
	ImGui::SameLine();
	if (ImGui::Button("UV1 fullscreen (F10)"))
		ActiveViewMode() = ViewMode::Uv1;
	if (ImGui::Button("Flat UV diffuse"))
		ActiveViewMode() = ViewMode::FlatUv0;
	ImGui::SameLine();
	if (ImGui::Button("Flat UV lightmap") && m_lmGpu)
		ActiveViewMode() = ViewMode::FlatUv1;
	if (ImGui::Button(
			("UV inset: " + std::string(SecondViewName(m_secondView)) +
				" (F5)").c_str()))
		m_secondView = SecondView((int(m_secondView) + 1) % 3);
	if (m_lmGpu && ImGui::Button("Baked irradiance (F8)"))
		ActiveViewMode() = ViewMode::Baked;
	ImGui::TextDisabled("UV0 = authored uvs; UV1 = atlas-space texCoord2.");
}

// Unity-style Play bar: centered under the menu, launches the current
// scene in the runtime App (registry scenes only). While App runs the
// button is disabled — close App to finish (its log streams to Console).
void EditorApp::DrawPlayBar() {
	const float menuH = ImGui::GetFrameHeight();
	// Centered over the central viewport rect (m_vp is set by RenderFrame).
	const float cx = m_vp.x + m_vp.w * 0.5f;
	ImGui::SetNextWindowPos(ImVec2(cx, menuH + 6), ImGuiCond_Always,
		ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha(1.0f); // fully opaque UI
	if (ImGui::Begin("##playbar", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
		if (m_runRunning) {
			ImGui::TextDisabled("Running in App... (close it)");
		}
		else {
			const std::string runArgs = RunArgs();
			if (runArgs.empty()) {
				ImGui::BeginDisabled();
				if (ImGui::Button("Run in App"))
					RunInApp();
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("Open a .pak or .gltf first.");
			}
			else {
				if (ImGui::Button("Run in App"))
					RunInApp();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("App %s", runArgs.c_str());
			}
		}
	}
	ImGui::End();
}

void EditorApp::DrawConsolePanel() {
	// Selectable log: readonly multiline text (mouse-drag selection +
	// Ctrl+C) with Clear/Copy buttons and stick-to-bottom autoscroll.
	if (ImGui::SmallButton("Clear"))
		m_consoleLines.clear();
	ImGui::SameLine();
	if (ImGui::SmallButton("Copy all")) {
		std::string all;
		size_t total = 0;
		for (auto& l : m_consoleLines)
			total += l.size() + 1;
		all.reserve(total);
		for (auto& l : m_consoleLines) {
			all += l;
			all += '\n';
		}
		ImGui::SetClipboardText(all.c_str());
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%zu lines (drag to select, Ctrl+C to copy)",
		m_consoleLines.size());
	std::string all;
	{
		size_t total = 0;
		for (auto& l : m_consoleLines)
			total += l.size() + 1;
		all.reserve(total + 1);
		for (auto& l : m_consoleLines) {
			all += l;
			all += '\n';
		}
	}
	// ImGui needs a mutable char buffer for InputTextMultiline; the
	// console caps at 400 lines so rebuilding each frame is cheap.
	static std::vector<char> buf;
	buf.assign(all.begin(), all.end());
	buf.push_back('\0');
	ImGui::BeginChild("##logscroll", ImVec2(0, 0), false,
		ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::InputTextMultiline("##consoletext", buf.data(), buf.size(),
		ImVec2(-1, -1),
		ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo);
	// Stick to bottom while new lines arrive (unless the user scrolled up).
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
}

void EditorApp::DrawViewportOverlay(smvk::Extent2D extent) {
	(void)extent;
	// Legend (view mode, hints, undo counts) on the ACTIVE panel only;
	// hideable via the View menu or the active panel's chip button.
	if (!m_showOverlay)
		return;
	const float menuH = ImGui::GetFrameHeight();
	// Top-left of the CENTRAL viewport rect (m_vp is set by RenderFrame).
	// Quad layout: sit BELOW the per-viewport camera chips.
	ImGui::SetNextWindowPos(ImVec2(m_vp.x + 12,
		menuH + 8 + (m_vpLayout == 1 ? 34.0f : 0.0f)), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(1.0f); // fully opaque UI
	if (ImGui::Begin("##overlay", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoInputs)) {
		// vi-style mode line (like vi's "-- INSERT --"), colored per
		// mode. Key hints are GENERATED from the central registry
		// (modal_bindings.h) so they can never drift from the real
		// bindings; the shared/global hints sit under the status lines.
		std::string modeKeys, globalKeys;
		for (size_t bi = 0; bi < ModalBindingCount(); ++bi) {
			const ModalBindingDef& b = ModalBindingAt(bi);
			if (!b.run || !ModalBindingApplies(b, m_mode))
				continue;
			std::string& dst = ((b.scopes & kScopeGlobal) != 0)
				? globalKeys : modeKeys;
			dst += std::string(b.keycap) + " " + b.label + "  ";
		}
		const ImVec4 modeCol =
			(m_mode == EditorMode::Layout) ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
			: (m_mode == EditorMode::Lighting) ? ImVec4(0.95f, 0.75f, 0.35f, 1.0f)
			: (m_mode == EditorMode::Test) ? ImVec4(0.40f, 0.80f, 0.95f, 1.0f)
			: ImVec4(0.95f, 0.45f, 0.65f, 1.0f);
		ImGui::TextColored(modeCol, "-- %s --  %s",
			EditorModeName(m_mode), modeKeys.c_str());
		ImGui::Text("%s%s  |  %s gizmo%s  |  %s  |  %s select",
			ViewModeName(ActiveViewMode()),
			m_wireframeAll ? " (wire)" : "",
			m_gizmoMode == GizmoMode::Translate ? "Translate" :
			m_gizmoMode == GizmoMode::Rotate ? "Rotate" : "Scale",
			m_gizmoMode == GizmoMode::Translate ? " (world)" : " (local)",
			m_gltfPath.empty() ? "no scene" : std::filesystem::path(
				m_gltfPath).filename().string().c_str(),
			SelModeName(m_selMode));
		if (m_selected != UINT32_MAX && m_world)
			ImGui::Text("sel: %u %s", m_selected,
				getNodeName(m_world->scene, int(m_selected)).c_str());
		if (m_selMode == SelMode::Face && m_selFaceNode != UINT32_MAX)
			ImGui::Text("face: node %u tri %u", m_selFaceNode, m_selFaceTri);
		else if (m_selMode == SelMode::Vertex && m_selVertNode != UINT32_MAX)
			ImGui::Text("vertex: node %u vert %u", m_selVertNode, m_selVertIdx);
		if (IsFlatUvViewMode(ActiveViewMode()))
			ImGui::Text("uv pane: zoom %.2f pan (%.2f,%.2f)",
				m_uvZoom[m_activeVp], m_uvPan[m_activeVp].x,
				m_uvPan[m_activeVp].y);
		if (ActiveViewMode() == ViewMode::Meshlets)
			ImGui::TextDisabled("meshlet color: %s",
				m_debugMeshletMode == 0 ? "random per meshlet"
				: "cone-cutoff heat");
		ImGui::TextDisabled("%s", globalKeys.c_str());
		ImGui::TextDisabled("click=select  Alt+LMB orbit  Alt+MMB pan  Alt+RMB dolly");
		ImGui::TextDisabled("flat pane: MMB pan  wheel zoom  click=face/vert");
		ImGui::TextDisabled("Ctrl+Z / Ctrl+Shift+Z: undo %zu / redo %zu"
			"  |  Ctrl+Alt+S save+",
			UndoDepth(), RedoDepth());
	}
	ImGui::End();

	// UV inset tag (top-left of the side strip, inside the viewport).
	if (SecondViewActive() && m_vpLayout == 0) {
		uint32_t stripX = 0;
		InsetStrip(m_vp, stripX);
		ImGui::SetNextWindowPos(ImVec2(float(stripX) + 8, menuH + 8),
			ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(1.0f); // fully opaque UI
		if (ImGui::Begin("##insetlabel", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoInputs)) {
			ImGui::Text("%s (F5)", SecondViewName(m_secondView));
		}
		ImGui::End();
	}
}

} // namespace editor
