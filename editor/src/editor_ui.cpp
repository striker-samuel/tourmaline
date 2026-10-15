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
#include <fstream>
#include <iostream>
#include <iterator>
#include <unordered_map>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Profile/profile.h>

#include <imgui.h>

#include <ktx.h>

#include <cmath>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h> // ShellExecuteA (reveal-in-explorer)
#endif

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
		// Tool topbar: tabbed toolbar directly under the menu bar (hidden
		// with the panels via m_uiMode). Everything below keys off the
		// combined menu+toolbar offset (m_menuH), so panels, splitters and
		// the rendered viewport rects stay in lockstep.
		const float menuBarH = ImGui::GetFrameHeight();
		const float toolbarH = m_uiMode ? DrawToolTabs(menuBarH) : 0.0f;
		m_menuH = menuBarH + toolbarH;
		const float menuH = m_menuH; // panels/viewport math use the combined offset
		const float vh = float(ImGui::GetIO().DisplaySize.y);
		// Del-key routing resets here: DrawPlacementGraph re-sets it when its
		// window hierarchy holds the keyboard focus (ModalDelete vs the
		// graph's own node/wire delete).
		m_imguiFocusInPlacementGraph = false;
		// Window title tracks the scene + dirty star (change-detected).
		UpdateWindowTitle();

		// Panels are siblings of the central viewport (never on top of it):
		// RenderFrame renders only the rect between them. I hides every
		// panel for a fullscreen viewport (the menu bar + overlay remain).
		if (m_uiMode) {
			ImGui::SetNextWindowPos(ImVec2(0, menuH), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(m_outlinerW, vh - menuH - m_bottomDockH),
				ImGuiCond_Always);
			if (ImGui::Begin("Outliner", nullptr,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
				ImGuiWindowFlags_NoResize)) {
				DrawOutliner();
			}
			ImGui::End();

			ImGui::SetNextWindowPos(
				ImVec2(float(ImGui::GetIO().DisplaySize.x) - m_inspectorW, menuH),
				ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(m_inspectorW, vh - menuH - m_bottomDockH),
				ImGuiCond_Always);
			if (ImGui::Begin("Inspector", nullptr,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
				ImGuiWindowFlags_NoResize)) {
				if (ImGui::BeginTabBar("##rightTabs")) {
					if (ImGui::BeginTabItem("Inspector")) {
						m_rightTab = 0;
						DrawInspector();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Render")) {
						m_rightTab = 2;
						DrawRenderSettingsPanel();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("UV")) {
						m_rightTab = 1;
						DrawUvPanel();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Material")) {
						m_rightTab = 5;
						DrawMaterialEditor();
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}
			}
			ImGui::End();

			// Bottom dock (title-less, tabbed): the wide-override rule table
			// lives next to the Console so both stay full-width.
			ImGui::SetNextWindowPos(ImVec2(0, vh - m_bottomDockH), ImGuiCond_Always);
			ImGui::SetNextWindowSize(
				ImVec2(float(ImGui::GetIO().DisplaySize.x), m_bottomDockH),
				ImGuiCond_Always);
			if (ImGui::Begin("BottomDock", nullptr,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
				ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
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
					if (ImGui::BeginTabItem("Assets")) {
						m_bottomTab = 2;
						DrawAssetsPanel();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Cook")) {
						m_bottomTab = 3;
						DrawCookPanel();
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}
			}
			ImGui::End();
		} // m_uiMode

		DrawPanelSplitters(); // resize handles between panels and viewport
		DrawInsetTag();
		DrawViewportChrome(); // borders, camera chips, box-select rect
		DrawUvEditorWindow(); // floating UV editor (View menu; canvas via GPU)
		DrawSelectionRadial(); // Q radial: Mesh/Face/Vertex highlight modes
		DrawReferencesPanel(); // Maya-style reference list (View menu)
		DrawPathsPanel(); // consolidated file/path view (File menu)
		if (m_placement.showGraph)
			DrawPlacementGraph();
		// Frame-time histogram (View menu / F12): a SEPARATE bottom strip —
		// full-width horizontal bucket graph pinned to the bottom of the
		// screen, drawn last so it sits on top of the panels/docks. Zero cost
		// while disabled: one flag check.
		if (smvk::profiler::FrameHistogramEnabled())
			smvk::profiler::DrawFrameHistogramUi();
	}

	void EditorApp::DrawMenuBar() {
		if (ImGui::BeginMenu("File")) {
			// Project first (Unity/Unreal style): a project folder owns
			// content/, paks/, gi/ and its own artifacts/ cache. Imports
			// land in it and the editor shows them
			// pipeline â€” no manual staging.
			if (ImGui::MenuItem("New / Open Project...")) {
				std::string picked;
				if (NativePickFolder(picked) && !picked.empty())
					NewProjectOrOpen(picked);
			}
			if (!m_projectRoot.empty()) {
				if (ImGui::MenuItem("Import asset...", nullptr, false,
					!m_cookRunning)) {
					std::string picked;
					if (NativeOpenFile(picked) && !picked.empty())
						ImportAsset(picked);
				}
				ImGui::TextDisabled("project: %s", m_projectName.c_str());
				ImGui::Separator();
			}
			// Single entry point: native dialog (.gltf/.glb/.tour, in C:/),
			// dispatched by extension (tours replay into the stack; paks are
			// refused — they run in the App).
			if (ImGui::MenuItem("Open file...")) {
				std::string picked;
				if (NativeOpenFile(picked) && !picked.empty())
					OpenAnySceneFile(picked);
			}
			if (ImGui::MenuItem("Add scene reference...", nullptr, false,
				!m_gltfPath.empty())) {
				// File browser pick (.gltf/.tour; the name defaults to the
				// path stem, the reference binds into its own namespace).
				std::string picked;
				if (NativeOpenFile(picked) && !picked.empty())
					AddSceneReference(picked, {});
			}
			if (ImGui::MenuItem("Save tour...")) {
				std::snprintf(m_savePathBuf, sizeof(m_savePathBuf),
					"%s", TourTarget().c_str());
				m_showSaveDialog = true;
			}
			if (ImGui::MenuItem("Save tour incremental", "Ctrl+Alt+S"))
				SaveTourIncremental();
			if (ImGui::MenuItem("Cook...", "Ctrl+Shift+C", false, m_world != nullptr)) {
				ShowCookPanel();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Close"))
				CloseScene();
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
			if (ImGui::MenuItem("Convex volume (GI)", nullptr, false,
				canCreate))
				DraftCreateCVolume();
			if (ImGui::MenuItem("Cubemap capture (pinned)", nullptr, false,
				canCreate))
				DraftCreateCubemap();
			if (ImGui::MenuItem("Camera", nullptr, false, canCreate))
				DraftCreateCamera();
			if (ImGui::MenuItem("Terrain...", nullptr, false, canCreate)) {
				// Seed the dialog defaults (name, material) once per open.
				std::snprintf(m_terrainNameBuf, sizeof(m_terrainNameBuf), "%s",
					UniqueTerrainName().c_str());
				m_terrainMat = UINT32_MAX;
				m_showTerrainDialog = true;
			}
			if (ImGui::MenuItem("Image...", nullptr, false, canCreate)) {
				// Maya file node: an image definition (path + sampling
				// settings) held OUTSIDE the DAG — editor-only state until
				// a material uses it.
				std::string picked;
				if (NativeOpenFile(picked, true) && !picked.empty())
					AddImageDef(picked);
			}
			// NOTE: cook assets (.gicook/.lmcook) are intentionally NOT
			// menu-creatable: they are an internal consequence of baking
			// (RecordBakeAssetsFromCook mints/updates them per scene).
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
			ImGui::Separator();
			// Del-key twin (modal registry 'Del'): deletes the editor-created
			// content in the selection (created lights, terrain groups);
			// imports/references are refused with a log (ids must stay
			// stable â€” hide those instead).
			if (ImGui::MenuItem("Delete selection", "Del", false,
				m_world && (m_selected != UINT32_MAX
					|| !m_selection.empty())))
				DeleteSelection();
			// Ctrl+D twin (modal registry 'Ctrl+D'): fresh Creates seeded
			// from the LIVE state of every selected duplicable node
			// (lights/cameras/cubemaps/cvolumes/transforms under the same
			// parent); imports/terrain are refused with a log.
			if (ImGui::MenuItem("Duplicate selection", "Ctrl+D", false,
				m_world && (m_selected != UINT32_MAX
					|| !m_selection.empty())))
				DuplicateSelection();
			ImGui::Separator();
			// Collapse the stack to the current state: superseded per-node /
			// per-attr edits drop (their LAST occurrence already carries the
			// live state), the redo tail clears, the .tour lightens. Creates
			// and terrain paint stamps stay (structure / ordered bake). NOT
			// undoable â€” same contract as Maya's delete-history.
			if (ImGui::MenuItem(
				("Coalesce history (" + std::to_string(UndoDepth())
					+ " cmds)").c_str(),
				nullptr, false, UndoDepth() > 1)) {
				const size_t dropped = CoalesceHistory();
				Log("Coalesced history: " + std::to_string(UndoDepth())
					+ " commands kept, " + std::to_string(dropped)
					+ " dropped (redo tail cleared).");
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Keeps only the last edit per node/attr "
					"channel â€” the surviving state â€” so the tour replays "
					"identically from fewer commands.");
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
			ImGui::TextDisabled("Debug views share the App's keys (F1..F6,"
				"F9..F11, P) â€” same view, same key, live view.");
			if (ImGui::MenuItem("Unlit", "F2",
				ActiveViewMode() == ViewMode::Unlit))
				ActiveViewMode() = ViewMode::Unlit;
			if (ImGui::MenuItem("Lit (shadows + full pipeline)", "F1",
				ActiveViewMode() == ViewMode::Lit))
				ActiveViewMode() = ViewMode::Lit;
			ImGui::Separator();
			// Lighting view (the GI tri-view, the bake workflow's three
			// looks): Unlit (albedo), Fully Lit (With GI) — the live view
			// renders the baked GI sources (world field + convex volumes +
			// cubemaps) — and Realtime Lit — the analytic look without the
			// bake (direct sun + sky IBL). The latter two both ride the Lit
			// frame; the difference is the ambient source (SetAmbientMode
			// All vs SkyIbl, driven every frame with the debug views).
			{
				const bool sel[3] = {
					ActiveViewMode() == ViewMode::Unlit,
					ActiveViewMode() == ViewMode::Lit && m_litView == 0,
					ActiveViewMode() == ViewMode::Lit && m_litView == 1
				};
				if (ImGui::MenuItem("Unlit (albedo)", nullptr, sel[0]))
					ActiveViewMode() = ViewMode::Unlit;
				if (ImGui::MenuItem("Fully Lit (With GI)", nullptr, sel[1])) {
					ActiveViewMode() = ViewMode::Lit;
					m_litView = 0;
				}
				if (ImGui::MenuItem("Realtime Lit", nullptr, sel[2])) {
					ActiveViewMode() = ViewMode::Lit;
					m_litView = 1;
				}
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Meshlets", "F3",
				ActiveViewMode() == ViewMode::Meshlets)) {
				ActiveViewMode() = ViewMode::Meshlets;
				if (!m_meshletDebugHasData && !EnsureMeshletDebug())
					Log("Meshlet view: no cooked meshlets in this scene "
						"(cook with --meshlets); meshes show as flat fallbacks.");
			}
			if (ImGui::MenuItem("Shadow cascade coverage", "F4",
				ActiveViewMode() == ViewMode::Cascades))
				ActiveViewMode() = ViewMode::Cascades;
			if (ImGui::MenuItem("LODs", "F5",
				ActiveViewMode() == ViewMode::Lods))
				ActiveViewMode() = ViewMode::Lods;
			if (ImGui::MenuItem("Vertex colors", "F6",
				ActiveViewMode() == ViewMode::VertexColors))
				ActiveViewMode() = ViewMode::VertexColors;
			if (ImGui::MenuItem("Parallax (POM)", "F11",
				ActiveViewMode() == ViewMode::Pom))
				ActiveViewMode() = ViewMode::Pom;
			if (ImGui::MenuItem("Baked irradiance", "F8",
				ActiveViewMode() == ViewMode::Baked, m_lmGpu))
				ActiveViewMode() = ViewMode::Baked;
			if (ActiveViewMode() == ViewMode::Meshlets) {
				const char* meshletModes[] = { "Random per meshlet",
					"Cone-cutoff heat" };
				ImGui::Combo("Meshlet color mode", &m_debugMeshletMode,
					meshletModes, 2);
			}
			ImGui::Separator();
			ImGui::MenuItem("Wireframe (hidden-line)", "P / Z", &m_wireframeAll);
			ImGui::SetItemTooltip("Line polygon mode over the Fill depth"
				"prepass — lines never draw over the objects in front.");
			ImGui::Separator();
			// Frame-time histogram (F12 — the kModalBindings GLOBAL row; this
			// menu item calls the SAME handler, so key + menu stay one
			// binding): the shared profiler state + a separate full-width
			// bottom strip drawn at the end of DrawUI. Zero cost while
			// disabled.
			if (ImGui::MenuItem("Frame-time histogram", "F12",
				smvk::profiler::FrameHistogramEnabled()))
				ModalFrameHistogramToggle();
			ImGui::SetItemTooltip("Live frame-time distribution (last 1024"
				" frames): a full-width horizontal bucket graph pinned to the"
				" bottom of the viewport, with avg/p50/p95/p99/max.");
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
			if (ImGui::MenuItem("UV0", "F9", ActiveViewMode() == ViewMode::Uv0))
				ActiveViewMode() = ViewMode::Uv0;
			if (ImGui::MenuItem("UV1 (lightmap)", "F10", ActiveViewMode() == ViewMode::Uv1))
				ActiveViewMode() = ViewMode::Uv1;
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
			if (m_showUvEditor && ImGui::MenuItem("Reset UV window (zoom 1)")) {
				m_uvWinZoom = 1.0f;
				m_uvWinPan = glm::vec2(0.0f);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Placement graph", nullptr, m_placement.showGraph))
				m_placement.showGraph = !m_placement.showGraph;
			if (ImGui::MenuItem("Reference editor...", nullptr,
				m_showReferencesPanel))
				m_showReferencesPanel = !m_showReferencesPanel;
			if (ImGui::MenuItem("Path editor...", nullptr,
				m_showPathsPanel))
				m_showPathsPanel = !m_showPathsPanel;
			ImGui::Separator();
			ImGui::MenuItem("Selection wire overlay", nullptr, &m_wireOverlay);
			if (ImGui::MenuItem("Light gizmos", "L", m_showLightGizmos))
				m_showLightGizmos = !m_showLightGizmos;
			if (ImGui::MenuItem("CVolume boxes", nullptr, m_showCvolGizmos))
				m_showCvolGizmos = !m_showCvolGizmos;
			if (ImGui::MenuItem("Cubemap capture markers", nullptr,
				m_showCubemapGizmos))
				m_showCubemapGizmos = !m_showCubemapGizmos;
			// App-parity probe overlay: the manifest sidecar's loaded rows
			// as dot + C<idx> + influence/fade rings (same table, same
			// colors as the App's probe-location overlay).
			if (ImGui::MenuItem("Probe locations overlay", nullptr,
				m_showProbeOverlay))
				m_showProbeOverlay = !m_showProbeOverlay;
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Loaded baked probes per viewport (dot +\n"
					"C<idx> + influence/fade rings, camera-distance\n"
					"colored) — the App's probe overlay, LIGHTING only.");
			if (ImGui::MenuItem("Camera frustums", nullptr,
				m_showCameraGizmos))
				m_showCameraGizmos = !m_showCameraGizmos;
			ImGui::Separator();
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
			if (ImGui::MenuItem("UV editor window", nullptr, m_showUvEditor))
				m_showUvEditor = !m_showUvEditor;
			ImGui::EndMenu();
		}

		// Update GI (top-bar bake entry, right-aligned): the click twin of the
		// cook dialog's "Bake GI now" — rebakes stay one click away instead
		// of hiding in a dialog. While a task runs the button reopens its
		// progress window (backgrounded or finished-unacknowledged). Guards
		// + logs live inside BakeGI.
		{
			const bool taskLive = m_bakeRunning || m_cookRunning;
			const char* label = m_bakeRunning ? "Baking GI..."
				: m_cookRunning ? "Cooking..." : "Update GI";
			const float w = ImGui::CalcTextSize(label).x
				+ ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
			ImGui::SameLine(ImGui::GetWindowWidth() - w
				- ImGui::GetStyle().WindowPadding.x);
			ImGui::BeginDisabled(!taskLive && SceneEmpty());
			if (ImGui::SmallButton(label)) {
				if (taskLive)
					ReopenTaskWindow();
				else
					BakeGI();
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(taskLive
					? "A task is running — click to reopen its progress."
					: "Rebake GI (world field + convex volumes)\n"
					"over the current definition — same as Bake GI now.");
		}

		if (m_showSaveDialog) {
			ImGui::OpenPopup("Save tour as");
			m_showSaveDialog = false;
		}
		DrawSaveDialog();
		if (m_showRecoverDialog) {
			ImGui::OpenPopup("Recover autosave");
			m_showRecoverDialog = false;
			m_recoverArmed = true;
		}
		DrawRecoverDialog();
		if (m_showTerrainDialog)
			ImGui::OpenPopup("Create Terrain");
		// m_showTerrainDialog stays true while the modal is up (the dialog
		// clears it on Create/Cancel); OpenPopup is idempotent.
		DrawCreateTerrainDialog();
		// Blocking task window (cook / bake / publish progress): drawn last
		// so its modal sits on top of every other popup.
		DrawTaskWindow();
		// Forced startup project selection (pending flag from Initialize,
		// cleared on the first frame; the dialog is a blocking modal).
		if (m_projectDialogPending) {
			ImGui::OpenPopup("Select project");
			m_projectDialogPending = false;
		}
		DrawProjectDialog();
	}

	// ---- tool topbar --------------------------------------------------------
	// Tab strip of tool types + one row of the active tab's controls. Returns
	// the toolbar's height (0 when the window fails to draw). The toolbar is
	// chrome: NoMove/NoBringToFront, fixed under the menu bar, hidden with
	// the panels (m_uiMode) so fullscreen viewports keep only the menu bar.
	float EditorApp::DrawToolTabs(float top) {
		const float frame = ImGui::GetFrameHeight();
		const float stripH = ImGui::GetFrameHeightWithSpacing();
		const float rowH = frame + 8.0f;
		const float h = stripH + rowH;
		ImGui::SetNextWindowPos(ImVec2(0.0f, top), ImGuiCond_Always);
		ImGui::SetNextWindowSize(
			ImVec2(float(ImGui::GetIO().DisplaySize.x), h), ImGuiCond_Always);
		if (!ImGui::Begin("##toptabs", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav)) {
			ImGui::End();
			return 0.0f;
		}
		// Radio helper: enum-class values over an int temp.
		auto radioEnum = [](const char* label, int* v, int id) {
			return ImGui::RadioButton(label, v, id);
		};
		if (ImGui::BeginTabBar("##toolTabs",
			ImGuiTabBarFlags_FittingPolicyScroll)) {
			// ---- Select ----------------------------------------------------
			if (ImGui::BeginTabItem("Select")) {
				m_toolTab = 0;
				int sel = int(m_selMode);
				if (radioEnum("Mesh", &sel, 0)) {}
				ImGui::SameLine();
				if (radioEnum("Face", &sel, 1)) {}
				ImGui::SameLine();
				if (radioEnum("Vertex", &sel, 2)) {}
				if (sel != int(m_selMode)) {
					m_selMode = SelMode(sel);
					ClearSubSelection();
				}
				ImGui::SameLine();
				ImGui::TextDisabled("Q radial |");
				ImGui::SameLine();
				if (ImGui::Button("Frame (F)"))
					FrameSelection();
				ImGui::SameLine();
				if (ImGui::Button("Clear##sel"))
					ClearSelection();
				ImGui::EndTabItem();
			}
			// ---- Transform -------------------------------------------------
			if (ImGui::BeginTabItem("Transform")) {
				m_toolTab = 1;
				int g = int(m_gizmoMode);
				if (radioEnum("Move", &g, 0)) m_gizmoMode = GizmoMode::Translate;
				ImGui::SameLine();
				if (radioEnum("Rotate", &g, 1)) m_gizmoMode = GizmoMode::Rotate;
				ImGui::SameLine();
				if (radioEnum("Scale", &g, 2)) m_gizmoMode = GizmoMode::Scale;
				ImGui::SameLine();
				ImGui::TextDisabled("click gizmo axes to drag |");
				ImGui::SameLine();
				ImGui::TextDisabled("sel: %s",
					(m_world && m_selected != UINT32_MAX)
					? getNodeName(m_world->scene, int(m_selected)).c_str()
					: "-");
				ImGui::EndTabItem();
			}
			// ---- Terrain ---------------------------------------------------
			if (ImGui::BeginTabItem("Terrain")) {
				m_toolTab = 2;
				const bool hasWorld = m_world != nullptr;
				if (!hasWorld)
					ImGui::BeginDisabled();
				if (ImGui::Button("Create...##terr")) {
					std::snprintf(m_terrainNameBuf, sizeof(m_terrainNameBuf),
						"%s", UniqueTerrainName().c_str());
					m_terrainMat = UINT32_MAX;
					m_showTerrainDialog = true;
				}
				ImGui::SameLine();
				const uint32_t group = hasWorld
					? TerrainGroupOf(m_selected) : UINT32_MAX;
				if (group == UINT32_MAX)
					ImGui::BeginDisabled();
				ImGui::Checkbox("Paint##terr", &m_paintTerrain);
				if (group == UINT32_MAX)
					ImGui::EndDisabled();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Paints the SELECTED terrain group "
						"(LMB in a 3D viewport).");
				if (!hasWorld)
					ImGui::EndDisabled();
				ImGui::SameLine();
				static const char* kOps[] = { "Raise", "Lower", "Flatten",
					"Smooth" };
				int op = std::clamp(m_paintOp, 0, 3);
				for (int k = 0; k < 4; ++k) {
					if (k > 0)
						ImGui::SameLine();
					if (radioEnum(kOps[k], &op, k))
						m_paintOp = k;
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(120.0f);
				ImGui::DragFloat("R##terr", &m_paintRadius, 0.25f, 0.5f, 256.0f,
					"%.1f m");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(120.0f);
				ImGui::DragFloat("S##terr", &m_paintStrength, 0.01f, 0.01f,
					64.0f, "%.2f");
				ImGui::SameLine();
				ImGui::TextDisabled("brush on %s",
					group != UINT32_MAX
					? ("group #" + std::to_string(group)).c_str()
					: "(select a terrain)");
				ImGui::EndTabItem();
			}
			// ---- Placement -------------------------------------------------
			if (ImGui::BeginTabItem("Placement")) {
				m_toolTab = 3;
				ImGui::Checkbox("Graph##tool", &m_placement.showGraph);
				ImGui::SameLine();
				ImGui::Checkbox("Overlay##tool", &m_placement.showOverlay);
				ImGui::SameLine();
				ImGui::Checkbox("Rejected##tool", &m_placement.showRejected);
				ImGui::SameLine();
				if (ImGui::Button("Refresh##tool"))
					MarkPlacementDirty();
				ImGui::SameLine();
				if (ImGui::Button("New graph##tool")) {
					m_placement.pushUndo();
					m_placement.addGraph("");
					MarkPlacementDirty();
				}
				ImGui::SameLine();
				ImGui::TextDisabled("%s",
					m_placement.preview.ok ? "compiled OK"
					: m_placement.preview.error.empty()
					? "no graph" : m_placement.preview.error.c_str());
				ImGui::EndTabItem();
			}
			// ---- Modes -----------------------------------------------------
			if (ImGui::BeginTabItem("Modes")) {
				m_toolTab = 4;
				int modeSel = int(m_mode);
				if (radioEnum("Layout", &modeSel, 0))
					SetMode(EditorMode::Layout);
				ImGui::SameLine();
				if (radioEnum("Lighting", &modeSel, 1))
					SetMode(EditorMode::Lighting);
				ImGui::SameLine();
				if (radioEnum("Test", &modeSel, 2))
					SetMode(EditorMode::Test);
				ImGui::SameLine();
				if (radioEnum("Profile", &modeSel, 3))
					SetMode(EditorMode::Profile);
				ImGui::SameLine();
				ImGui::TextDisabled("keys 1-4 anywhere");
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::End();
		return h;
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
		// The CANONICAL HOST PATH drives the marker walk (m_gltfHostPath — the
		// file that actually loaded, GUID-resolved on tour opens): the recorded
		// identity (m_gltfPath) can be a project-RELATIVE "content/models/..."
		// on tour-opened scenes, whose relative root then resolved against the
		// editor CWD (the build dir) instead of the project (same defect the
		// cook's contentDir walk had — see CookToPak).
		const std::string identity = !m_gltfHostPath.empty()
			? m_gltfHostPath : m_gltfPath;
		std::string posix = identity;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		if (const size_t at = posix.find("content/");
			at != std::string::npos && !identity.empty())
			return posix.substr(0, at + 7); // keep trailing "content/"
		std::error_code ec;
		if (std::filesystem::is_directory("content", ec))
			return "content";
		return ".";
	}

	void EditorApp::DrawOutliner() {
		// The scene graph. The old cooked-VFS/pak browsing tab is GONE:
		// paks run in the App (the editor is WYSIWYG authoring over source
		// assets); tools/pak_browser.py remains the standalone inspector.
		DrawSceneOutliner();
	}

	void EditorApp::ClearNonDagSelection() {
		m_nonDagSel = NonDagSel{};
		m_imageFocus = UINT32_MAX;
	}

	void EditorApp::SelectNonDag(NonDagKind kind, uint32_t index) {
		// Single selection, mutually exclusive with node/World picks
		// (SetSelection fans out to the non-DAG clear first).
		SetSelection(UINT32_MAX);
		m_nonDagSel.valid = true;
		m_nonDagSel.kind = kind;
		m_nonDagSel.index = index;
		if (kind == NonDagKind::Material && m_world
			&& index < m_world->materialLocations.size()) {
			m_matEditorSel = index;
			if (m_materialFocus != index) {
				if (m_materialFocus != UINT32_MAX)
					m_materialTrail.push_back(m_materialFocus);
				m_materialFocus = index; // opens the material page
			}
		}
		else if (kind == NonDagKind::Image) {
			m_imageFocus = index; // Images-tab highlight (sticky)
		}
	}

	void EditorApp::CollectNonDagEntries(std::vector<NonDagEntry>& out) const {
		out.clear();
		// Future node sets: add a NonDagKind value + one block here; the
		// section rendering and picking below stay kind-driven.
		for (size_t i = 0; i < m_images.size(); ++i) {
			NonDagEntry e;
			e.kind = NonDagKind::Image;
			e.index = uint32_t(i);
			const std::string& name = m_images[i].name;
			std::string base = m_images[i].path;
			const size_t s = base.find_last_of("/\\");
			if (s != std::string::npos)
				base = base.substr(s + 1);
			e.label = "[file] " + (name.empty() ? base : name);
			out.push_back(std::move(e));
		}
		if (m_world) {
			for (uint32_t i = 0;
				i < uint32_t(m_world->materialLocations.size()); ++i) {
				const auto& loc = m_world->materialLocations[i];
				if (!loc.cluster || !loc.handle.valid())
					continue;
			NonDagEntry e;
			e.kind = NonDagKind::Material;
			e.index = i;
			e.label = "[mat] mat." + std::to_string(i) + " "
				+ std::string(loc.cluster->GetMaterialUri(loc.handle).Str());
			out.push_back(std::move(e));
		}
		}
	}

	void EditorApp::DrawNonDagSection() {
		SetupDefaultCameras();
		std::vector<NonDagEntry> entries;
		CollectNonDagEntries(entries);
		const bool filtering = m_filterBuf[0] != '\0';
		// Editor orbit cameras (virtual rows): click binds the ACTIVE
		// viewport (same as its chip combo). These are session view state,
		// never scene nodes â€” they don't select, don't cook, and stay out
		// of the shift-range order.
		auto drawEditorCamRow = [&](size_t ci) {
			const EditorCam& ec = m_editorCams[ci];
			const std::string label = "[editor] " + ec.name;
			if (!filtering || label.find(m_filterBuf) != std::string::npos) {
				ImGui::PushID(int(2000000 + ci));
				const bool active = m_viewports[m_activeVp].camIdx == int(ci)
					&& m_viewports[m_activeVp].boundNode < 0;
				if (ImGui::Selectable(label.c_str(), active)) {
					m_viewports[m_activeVp].camIdx = int(ci);
					m_viewports[m_activeVp].boundNode = -1;
					Log("Viewport " + std::to_string(m_activeVp)
						+ " orbit camera: '" + ec.name + "'.");
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Editor orbit camera (session view "
						"state, never cooked). Click binds the active "
						"viewport; orbit with Alt+LMB/MMB/RMB.");
				ImGui::PopID();
			}
		};
		if (filtering) {
			// Flat rows (same child as the node hits).
			for (const NonDagEntry& e : entries) {
				if (e.label.find(m_filterBuf) == std::string::npos)
					continue;
				std::string label = e.label;
				if (e.kind == NonDagKind::Image) {
					std::error_code ec;
					if (!std::filesystem::exists(
						std::filesystem::path(m_images[size_t(e.index)].path), ec))
						label += " [missing]";
				}
				ImGui::PushID(int(1000000 + int(e.kind) * 100000 + e.index));
				const bool sel = m_nonDagSel.valid
					&& m_nonDagSel.kind == e.kind
					&& m_nonDagSel.index == e.index;
				if (ImGui::Selectable(label.c_str(), sel))
					SelectNonDag(e.kind, e.index);
				ImGui::PopID();
			}
			for (size_t ci = 0; ci < m_editorCams.size(); ++ci)
				drawEditorCamRow(ci);
			return;
		}
		ImGui::PushID("nondag");
		const bool open = ImGui::TreeNodeEx("##root",
			ImGuiTreeNodeFlags_DefaultOpen,
			"Non-DAG (%zu)", entries.size());
		if (!open) {
			ImGui::PopID();
			return;
		}
		auto drawGroup = [&](const char* title, NonDagKind kind) {
			size_t n = 0;
			for (const NonDagEntry& e : entries)
				if (e.kind == kind)
					++n;
			if (n == 0)
				return;
			ImGui::PushID(title);
			const bool gopen = ImGui::TreeNodeEx("##g",
				ImGuiTreeNodeFlags_DefaultOpen, "%s (%zu)", title, n);
			if (gopen) {
				for (const NonDagEntry& e : entries) {
					if (e.kind != kind)
						continue;
					std::string label = e.label;
					if (e.kind == NonDagKind::Image) {
						std::error_code ec;
						if (!std::filesystem::exists(std::filesystem::path(
							m_images[size_t(e.index)].path), ec))
							label += " [missing]";
					}
					ImGui::PushID(int(e.index));
					const bool sel = m_nonDagSel.valid
						&& m_nonDagSel.kind == e.kind
						&& m_nonDagSel.index == e.index;
					if (ImGui::Selectable(label.c_str(), sel))
						SelectNonDag(e.kind, e.index);
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		};
		drawGroup("Images", NonDagKind::Image);
		drawGroup("Materials", NonDagKind::Material);
		drawGroup("World dressing", NonDagKind::Terrain);
		ImGui::PushID("editorcams");
		const bool eopen = ImGui::TreeNodeEx("##g",
			ImGuiTreeNodeFlags_DefaultOpen, "Editor (%zu)",
			m_editorCams.size());
		if (eopen) {
			for (size_t ci = 0; ci < m_editorCams.size(); ++ci)
				drawEditorCamRow(ci);
			ImGui::TreePop();
		}
		ImGui::PopID();
		ImGui::TreePop();
		ImGui::PopID();
	}

	void EditorApp::DrawSceneOutliner() {
		if (!m_world || m_world->scene.hierarchy.empty()) {
			ImGui::TextDisabled("No scene. File > Open scene... (.gltf/.tour)");
			return;
		}
		ImGui::SetNextItemWidth(-90.0f);
		ImGui::InputTextWithHint("##filter", "filter nodes...",
			m_filterBuf, sizeof(m_filterBuf));
		ImGui::SameLine();
		ImGui::Checkbox("non-DAG", &m_showNonDag);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Show non-scene rows (file/image nodes, "
				"materials, editor orbit cameras) in this tree.");
		const bool filtering = m_filterBuf[0] != '\0';
		std::string selName = "-";
		if (m_worldSelected)
			selName = "World";
		else if (m_selected != UINT32_MAX)
			selName = getNodeName(m_world->scene, int(m_selected));
		ImGui::TextDisabled("nodes: %zu%s  selected: %s",
			m_world->scene.hierarchy.size(),
			m_deletedNodes.empty() ? ""
			: (" (" + std::to_string(m_deletedNodes.size()) + " deleted)").c_str(),
			selName.c_str());
		ImGui::Separator();
		ImGui::Separator();
		m_outlinerOrder.clear(); // rebuilt below (tree XOR filter rows)
		ImGui::BeginChild("##tree");
		// World pseudo-node: global render settings. Pinned above the tree,
		// never part of it â€” no children, no drag&drop, no rename/delete.
		// (Filtered out by the node filter like everything else.)
		if (!filtering || std::string("World").find(m_filterBuf) != std::string::npos) {
			ImGui::PushID("worldnode");
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.0f, 1.0f));
			if (ImGui::Selectable("World [globals]", m_worldSelected))
				SelectWorldNode();
			ImGui::PopStyleColor();
			ImGui::PopID();
			if (!filtering)
				ImGui::Separator();
		}
		if (m_showNonDag && !filtering)
			DrawNonDagSection();
		if (filtering) {
			for (uint32_t n = 0; n < uint32_t(m_world->scene.hierarchy.size()); ++n) {
				if (m_deletedNodes.contains(n))
					continue; // deleted (Maya delete): out of the live scene
				if (InDisabledRefSubtree(n))
					continue; // disabled reference tree: not shown, not read
				const std::string name = getNodeName(m_world->scene, int(n));
				if (name.find(m_filterBuf) == std::string::npos)
					continue;
				m_outlinerOrder.push_back(n); // visible-row order (shift-range)
				ImGui::PushID(int(n));
				const bool sel = IsSelected(n);
				char label[300];
				std::snprintf(label, sizeof(label), "%u: %s%s%s%s%s%s%s%s%s", n,
					name.empty() ? "(unnamed)" : name.c_str(),
					m_world->scene.meshForNode.contains(n) ? " [mesh]" : "",
					FindNodeLight(n) ? " [light]" : "",
					m_world->scene.cvolForNode.contains(n) ? " [cvol]" : "",
					m_world->scene.cubemapForNode.contains(n) ? " [cube]" : "",
					m_world->scene.cameraForNode.contains(n) ? " [camera]" : "",
					IsEnvNode(n) ? " [env]" : "",
					IsRefPlaceholder(n) ? " [ref]" : "",
					!IsVisibleNode(m_world->scene, int(n)) ? " [hidden]" : "");
				if (ImGui::Selectable(label, sel)) {
					// Click = replace; ctrl-click = toggle one;
					// shift-click = contiguous visible range.
					if (ImGui::GetIO().KeyShift)
						SelectRange(n);
					else if (ImGui::GetIO().KeyCtrl)
						ToggleSelection(n);
					else
						SetSelection(n);
					m_revealNode = n;
				}
				if (sel && n == m_revealNode)
					ImGui::SetScrollHereY(0.5f);
				ImGui::PopID();
			}
			if (m_showNonDag)
				DrawNonDagSection();
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

	void EditorApp::DrawOutlinerNode(uint32_t node,
		const std::unordered_set<uint32_t>* expandSet) {
		if (m_deletedNodes.contains(node))
			return; // deleted (Maya delete): out of the live scene
		m_outlinerOrder.push_back(node); // visible-row order (shift-range)
		const Scene& scene = m_world->scene;	const std::string name = getNodeName(scene, int(node));
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
		char label[300];
		std::snprintf(label, sizeof(label), "%s%s%s%s%s%s%s%s%s%s%s%s",
			name.empty() ? "(node)" : name.c_str(),
			hasMesh ? " [mesh]" : "", lightTag,
			scene.cvolForNode.contains(node) ? " [cvol]" : "",
			scene.cubemapForNode.contains(node) ? " [cube]" : "",
			scene.cameraForNode.contains(node) ? " [camera]" : "",
			IsNoLightmapNode(scene, int(node)) ? " [noLM]" : "",
			IsDitherCloseupNode(scene, int(node)) ? " [dither]" : "",
			IsStaticSubtreeRoot(scene, int(node)) ? " [static]" : "",
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
			// Click = replace; ctrl-click = toggle one; shift-click =
			// contiguous visible range from the primary.
			if (ImGui::GetIO().KeyShift)
				SelectRange(node);
			else if (ImGui::GetIO().KeyCtrl)
				ToggleSelection(node);
			else
				SetSelection(node);
		}
		if (sel && node == m_revealNode)
			ImGui::SetScrollHereY(0.5f);
		if (open) {
			// Disabled references keep their placeholder row (the anchor
			// for re-enabling) but the TREE is not shown â€” the nodes stay
			// in the scene (stable ids), just out of sight.
			if (m_disabledRefRoots.contains(node)) {
				ImGui::TextDisabled("    (reference disabled â€” %zu node(s) "
					"hidden)",
					RefSubtreeSizeForOutliner(node));
			}
			else {
				for (int c = scene.hierarchy[node].firstChild; c != -1;
					c = scene.hierarchy[c].nextSibling)
					DrawOutlinerNode(uint32_t(c), expandSet);
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

	// Whether `node` sits inside (or is the root of) a DISABLED reference
	// subtree: the placeholder row stays, the tree does not.
	bool EditorApp::InDisabledRefSubtree(uint32_t node) const {
		if (m_disabledRefRoots.empty() || !m_world
			|| node >= m_world->scene.hierarchy.size())
			return false;
		// Walk ancestors (the placeholder itself counts as inside).
		for (uint32_t n = node; n < m_world->scene.hierarchy.size();) {
			if (m_disabledRefRoots.contains(n))
				return true;
			const int p = m_world->scene.hierarchy[n].parent;
			if (p < 0)
				return false;
			n = uint32_t(p);
		}
		return false;
	}

	// Subtree size for the disabled-reference hint (child count below the
	// placeholder, excluding other deleted nodes).
	size_t EditorApp::RefSubtreeSizeForOutliner(uint32_t root) const {
		if (!m_world || root >= m_world->scene.hierarchy.size())
			return 0;
		size_t n = 0;
		std::vector<uint32_t> stack;
		for (int c = m_world->scene.hierarchy[root].firstChild; c >= 0;
			c = m_world->scene.hierarchy[size_t(c)].nextSibling)
			stack.push_back(uint32_t(c));
		while (!stack.empty()) {
			const uint32_t v = stack.back();
			stack.pop_back();
			if (!m_deletedNodes.contains(v))
				++n;
			for (int c = m_world->scene.hierarchy[v].firstChild; c >= 0;
				c = m_world->scene.hierarchy[size_t(c)].nextSibling)
				stack.push_back(uint32_t(c));
		}
		return n;
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
		// not per component â€” RGBA8 reports 4.
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

	void EditorApp::DrawCvolSection(uint32_t node) {
		// Authored convex GI volume on this node (editor cvolume node):
		// box shape from the node's TRS (translate = center, scale = full
		// extents), bake params below. Undoable SetCvolAttr edits persisted
		// as setCvolAttr tour verbs so cooks replay them.
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		auto it = scene.cvolForNode.find(node);
		if (it == scene.cvolForNode.end())
			return;
		::SceneCvol& def = it->second;
		ImGui::Separator();
		ImGui::Text("Convex volume: '%s'",
			getNodeName(scene, int(node)).c_str());
		glm::vec3 mn, mx;
		if (CvolBox(node, mn, mx))
			ImGui::TextDisabled("aabb (%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f) m",
				mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
		ImGui::TextDisabled("shape = node translate/scale/rotation (gizmo "
			"or transform; a rotated box — the bake rotates the planes)");
		auto beginCvolDrag = [&](const char* attr) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetCvolAttr ||
				m_pending.cmd.node != node || m_pending.cmd.cvolAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetCvolAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.cvolAttr = attr;
				ReadNodeCvolAttr(node, attr, m_pending.cmd.beforeVec4,
					m_pending.cmd.beforeNum, m_pending.cmd.beforeText);
			}
		};
		auto endCvolDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.verb == EditVerb::SetCvolAttr &&
				m_pending.cmd.node == node &&
				m_pending.cmd.cvolAttr == attr)
				FinishDrag();
		};
		int prio = int(def.priority);
		if (ImGui::DragInt("Priority (higher wins)", &prio, 0.2f, 0, 16)) {
			prio = std::clamp(prio, 0, 255);
			beginCvolDrag("priority");
			WriteNodeCvolAttr(node, "priority", glm::vec4(0), double(prio), {});
			m_pending.cmd.number = double(prio);
		}
		endCvolDrag("priority");
		float spacing = def.spacing;
		if (ImGui::DragFloat("Probe spacing (m)", &spacing, 0.01f, 0.05f,
			4.0f, "%.2f")) {
			beginCvolDrag("spacing");
			WriteNodeCvolAttr(node, "spacing", glm::vec4(0), double(spacing),
				{});
			m_pending.cmd.number = double(spacing);
		}
		endCvolDrag("spacing");
		float inside = def.insideFade;
		if (ImGui::DragFloat("Inside fade (m)", &inside, 0.01f, 0.0f,
			4.0f, "%.2f")) {
			beginCvolDrag("inside");
			WriteNodeCvolAttr(node, "inside", glm::vec4(0), double(inside),
				{});
			m_pending.cmd.number = double(inside);
		}
		endCvolDrag("inside");
		float outside = def.outsideFade;
		if (ImGui::DragFloat("Outside fade (m)", &outside, 0.01f, 0.0f,
			4.0f, "%.2f")) {
			beginCvolDrag("outside");
			WriteNodeCvolAttr(node, "outside", glm::vec4(0), double(outside),
				{});
			m_pending.cmd.number = double(outside);
		}
		endCvolDrag("outside");
		ImGui::TextDisabled("bake: a future gi_baker --tour pass harvests "
			"these (priority/spacing/fades) + the box");
	}

	void EditorApp::DrawCubemapSection(uint32_t node) {
		// Authored cubemap capture on this node (editor cubemap node): a
		// PINNED specular probe. Position = the node's translate (gizmo);
		// the influence radius below. Baked by the cook's --cubemap stage
		// as a 'pin' row (wins weight ties at load).
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		auto it = scene.cubemapForNode.find(node);
		if (it == scene.cubemapForNode.end())
			return;
		::SceneCubemap& def = it->second;
		ImGui::Separator();
		ImGui::Text("Cubemap capture: '%s'",
			getNodeName(scene, int(node)).c_str());
		ImGui::TextDisabled("pinned probe â€” wins weight ties at load");
		auto beginCubeDrag = [&](const char* attr) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetCubemapAttr ||
				m_pending.cmd.node != node || m_pending.cmd.cubemapAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetCubemapAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.cubemapAttr = attr;
				ReadNodeCubemapAttr(node, attr, m_pending.cmd.beforeVec4,
					m_pending.cmd.beforeNum, m_pending.cmd.beforeText);
			}
		};
		auto endCubeDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.verb == EditVerb::SetCubemapAttr &&
				m_pending.cmd.node == node &&
				m_pending.cmd.cubemapAttr == attr)
				FinishDrag();
		};
		float radius = def.radius;
		if (ImGui::DragFloat("Influence radius (m)", &radius, 0.05f,
			0.05f, 100.0f, "%.2f")) {
			beginCubeDrag("radius");
			WriteNodeCubemapAttr(node, "radius", glm::vec4(0),
				double(radius), {});
			m_pending.cmd.number = double(radius);
		}
		endCubeDrag("radius");
		ImGui::TextDisabled("fade = radius/2; position = node translate "
			"(gizmo); bake: Cook panel > Cubemap captures (pinned rows)");
	}

	void EditorApp::DrawCamSection(uint32_t node) {
		// Authored camera on this node (editor camera node): the LENS below;
		// the pose is the node's transform (gizmo), forward = node -Z in
		// world. Undoable SetCamAttr edits persisted as setCamAttr tour
		// verbs so cooks replay them.
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		if (!scene.cameraForNode.contains(node))
			return;
		ImGui::Separator();
		ImGui::Text("Camera: '%s'",
			getNodeName(scene, int(node)).c_str());
		ImGui::TextDisabled("pose = node transform (gizmo), forward = -Z");
		const ::SceneCamera* C = FindNodeCamera(node);
		if (!C)
			return;
		auto beginCamDrag = [&](const char* attr) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetCamAttr ||
				m_pending.cmd.node != node || m_pending.cmd.camAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetCamAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.camAttr = attr;
				ReadNodeCameraAttr(node, attr, m_pending.cmd.beforeVec4,
					m_pending.cmd.beforeNum, m_pending.cmd.beforeText);
			}
		};
		auto endCamDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.verb == EditVerb::SetCamAttr &&
				m_pending.cmd.node == node &&
				m_pending.cmd.camAttr == attr)
				FinishDrag();
		};
		int proj = (C->projection
			== ::SceneCamera::Projection::Orthographic) ? 1 : 0;
		if (ImGui::Combo("Projection", &proj,
			"Perspective\0Orthographic\0")) {
			EditCmd c{};
			c.verb = EditVerb::SetCamAttr;
			c.node = node;
			c.camAttr = "projection";
			c.number = double(proj);
			ReadNodeCameraAttr(node, "projection", c.beforeVec4,
				c.beforeNum, c.beforeText);
			Do(c);
		}
		if (proj == 0) {
			float fov = C->fovYDeg;
			if (ImGui::DragFloat("FOV (deg)", &fov, 1.0f, 1.0f, 179.0f,
				"%.0f")) {
				beginCamDrag("fovY");
				WriteNodeCameraAttr(node, "fovY", glm::vec4(0),
					double(fov), {});
				m_pending.cmd.number = double(fov);
			}
			endCamDrag("fovY");
		}
		else {
			float hh = C->orthoHalfH;
			if (ImGui::DragFloat("Ortho half-height (m)", &hh, 0.25f,
				0.01f, 100000.0f)) {
				beginCamDrag("orthoHalfH");
				WriteNodeCameraAttr(node, "orthoHalfH", glm::vec4(0),
					double(hh), {});
				m_pending.cmd.number = double(hh);
			}
			endCamDrag("orthoHalfH");
		}
		float nc = C->nearClip;
		if (ImGui::DragFloat("Near clip", &nc, 0.005f, 0.0001f, 50.0f,
			"%.4f")) {
			beginCamDrag("near");
			WriteNodeCameraAttr(node, "near", glm::vec4(0), double(nc), {});
			m_pending.cmd.number = double(nc);
		}
		endCamDrag("near");
		float fc = C->farClip;
		if (ImGui::DragFloat("Far clip", &fc, 5.0f, 0.01f, 1000000.0f,
			"%.0f")) {
			beginCamDrag("far");
			WriteNodeCameraAttr(node, "far", glm::vec4(0), double(fc), {});
			m_pending.cmd.number = double(fc);
		}
		endCamDrag("far");
	}

	void EditorApp::DrawInspector() {
		SetupDefaultCameras();
		// Material focus page: the Inspector is "traveling" in a material.
		if (m_world && m_materialFocus != UINT32_MAX) {
			DrawMaterialFocusPage();
			return;
		}
		// Browser-selected ASSET page: a .material/.mesh click in the asset
		// browser inspects here (view parameters only; edits happen on
		// scenes — Unity-style). A .texture click opens the TEXTURE-ASSET
		// page: its import settings ARE the authoring (editable for LDR
		// sources; a KTX2 source pre-decided them — read-only).
		if (!m_assetsSel.empty()) {
			std::string e =
				std::filesystem::path(m_assetsSel).extension().string();
			for (char& c : e)
				c = char(std::tolower((unsigned char)c));
			if (e == ".material" || e == ".geometry" || e == ".texture") {
				DrawAssetReadonlyPage();
				return;
			}
			// IMPORTABLE SOURCE page: a supported image format selected but
			// not yet imported as an asset — probe + the import button (the
			// .texture wrap picks the settings up from here on).
			if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga"
				|| e == ".bmp" || e == ".ktx2") {
				DrawImportableTexturePage();
				return;
			}
		}
		// World pseudo-node page (global render settings, not a scene node).
		if (m_worldSelected) {
			DrawWorldPanel();
			return;
		}
		if (!m_world || m_selected == UINT32_MAX ||
			m_selected >= m_world->scene.hierarchy.size()) {
			ImGui::TextDisabled("Nothing selected.");
			ImGui::TextWrapped("Click a mesh in the viewport or pick a node in the outliner. "
				"Drag nodes onto each other to reparent. All edits are undoable (Ctrl+Z). "
				"Global settings (sun, IBL, exposure) live on the World node at the "
				"top of the outliner.");
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
			const bool tOwn = TrackDrag(EditVerb::Translate, node,
				scene.localTransform[node]);
			if (tOwn) {
				WriteLocalTRS(node, glm::vec3(ts[0], ts[1], ts[2]), r, s);
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					m_pending.cmd.afterVec = glm::vec3(ts[0], ts[1], ts[2]);
					FinishDrag();
				}
			}
			ImGui::DragFloat3("Rotate (deg)", rs, 0.5f);
			const bool rOwn = TrackDrag(EditVerb::Rotate, node,
				scene.localTransform[node]);
			if (rOwn) {
				WriteLocalTRS(node, t, glm::vec3(rs[0], rs[1], rs[2]), s);
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					m_pending.cmd.afterVec = glm::vec3(rs[0], rs[1], rs[2]);
					FinishDrag();
				}
			}
			ImGui::DragFloat3("Scale", ss, 0.02f);
			const bool sOwn = TrackDrag(EditVerb::Scale, node,
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
			const ::SceneLight * li = FindNodeLight(node, &aimWs)) {
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
					? " â€” seeds the editor IBL default" : "");
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
					// Spot COOKIE (gobo): a .texture asset path (or image URI)
					// projected through the cone — the authored cookieUri the
					// light build resolves to the bindless index (empty = none).
					// Browse imports the picked image as a .texture beside the
					// asset tree in one click (see CreateTextureAsset).
					ImGui::Separator();
					ImGui::TextDisabled("Cookie (spot gobo)");
					char cookieBuf[512] = {};
					std::snprintf(cookieBuf, sizeof(cookieBuf), "%s",
						li->cookieUri.c_str());
					ImGui::SetNextItemWidth(-90.0f);
					if (ImGui::InputText("##lightcookie", cookieBuf,
						sizeof(cookieBuf),
						ImGuiInputTextFlags_EnterReturnsTrue)) {
						EditCmd c{};
						c.verb = EditVerb::SetLightAttr;
						c.node = node;
						c.lightAttr = "cookie";
						c.afterText = cookieBuf;
						ReadNodeLightAttr(node, "cookie", c.beforeVec4, c.beforeNum,
							c.beforeFlag, c.beforeText);
						ImportCookieTexture(cookieBuf);
						Do(c);
					}
					ImGui::SameLine();
					if (ImGui::Button("Browse...")) {
						std::string picked;
						if (NativeOpenFile(picked, true) && !picked.empty()) {
							// Full asset flow in one click: the picked image
							// becomes a .texture (import settings wrap it from
							// here on) and the cookie ref points at it.
							const std::string tex =
								CreateTextureAsset(picked, m_assetsDir);
							if (!tex.empty()) {
								std::string rel = ProjectRelativePath(tex);
								for (char& ch : rel)
									if (ch == '\\')
										ch = '/';
								EditCmd c{};
								c.verb = EditVerb::SetLightAttr;
								c.node = node;
								c.lightAttr = "cookie";
								c.afterText =
									rel.empty() ? tex : rel;
								ReadNodeLightAttr(node, "cookie", c.beforeVec4,
									c.beforeNum, c.beforeFlag, c.beforeText);
								ImportCookieTexture(c.afterText);
								Do(c);
							}
						}
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("The .texture (or image) projected "
							"through the cone as a gobo. Empty = none. "
							"Unresolvable refs warn at the light build and "
							"drop (the light renders unmodified).");
				}
				else {
					ImGui::TextDisabled("points cannot cast shadows (spot atlas only)");
				}
				// Procedural light FUNCTION (points + spots): per-pixel intensity
				// modulation — flicker (candle-like time noise), pulse (sine
				// breathing), bands (moving diagonal bands across the cone;
				// points fall back to pulse). Speed scales time, scale is the
				// modulation depth (1 = dips to black). See light.hlsli.
				ImGui::Separator();
				ImGui::TextDisabled("Light function (procedural)");
				{
					const char* items[] = { "none", "flicker", "pulse",
						"bands" };
					int fn = int(std::clamp(li->functionType, 0u, 3u));
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::Combo("##lightfunction", &fn, items, 4)) {
						EditCmd c{};
						c.verb = EditVerb::SetLightAttr;
						c.node = node;
						c.lightAttr = "function";
						c.number = double(fn);
						ReadNodeLightAttr(node, "function", c.beforeVec4,
							c.beforeNum, c.beforeFlag, c.beforeText);
						Do(c);
					}
					float fnspeed = li->functionSpeed;
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::DragFloat("speed##lightfunction", &fnspeed,
						0.01f, 0.01f, 10.0f, "%.2f")) {
						beginLightDrag("functionSpeed");
						WriteNodeLightAttr(node, "functionSpeed", glm::vec4(0),
							fnspeed, false, {});
						m_pending.cmd.number = fnspeed;
					}
					endLightDrag("functionSpeed");
					float fnscale = li->functionScale;
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::DragFloat("depth##lightfunction", &fnscale,
						0.01f, 0.0f, 1.0f, "%.2f")) {
						beginLightDrag("functionScale");
						WriteNodeLightAttr(node, "functionScale", glm::vec4(0),
							fnscale, false, {});
						m_pending.cmd.number = fnscale;
					}
					endLightDrag("functionScale");
				}
			}
		}

		// Authored convex GI volume on this node (cvolume inspector).
		DrawCvolSection(node);
		// Authored cubemap capture on this node (pinned probe inspector).
		DrawCubemapSection(node);
		// Authored camera on this node (lens inspector).
		DrawCamSection(node);

		// Static subtree (transform freeze): the root mark affects the WHOLE
		// tree (Scene::staticRoot) — frozen transforms after the initial
		// build (skip per-frame updates + BVH rebuilds) + mega-mesh collapse
		// eligibility at cook.
		{
			ImGui::Separator();
			const bool inherited = IsStaticNode(scene, int(node));
			bool self = node < scene.staticRoot.size()
				&& scene.staticRoot[node] != 0;
			if (ImGui::Checkbox("Static (whole subtree)", &self)) {
				EditCmd c{};
				c.verb = EditVerb::SetStatic;
				c.node = node;
				c.afterFlag = self;
				c.beforeFlag = !self;
				Do(c);
			}
			ImGui::TextDisabled("effective: %s (transforms freeze after the "
				"build; cook may collapse the subtree into one block)",
				inherited ? "static" : "dynamic");
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

		// Dither on camera closeup (subtree flag; the GPU fade is LIVE — per
		// frame, per draw, no rebake needed: the renderer re-resolves the flag
		// every frame, so a checkbox toggle reflects on the next frame).
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
			ImGui::TextDisabled("effective: %s (1 m fully hidden .. 8 m solid,\n"
				"screen-door fade from the subtree anchor; live per frame)",
				inherited ? "dithered" : "off");
		}

		// Terrain group/tile: height paint controls (editor paint mode).
		if (const uint32_t group = TerrainGroupOf(node); group != UINT32_MAX) {
			ImGui::Separator();
			auto it = m_terrainGroups.find(group);
			if (it != m_terrainGroups.end()) {
				ImGui::Text("Terrain '%s'  (%ux%u tiles)",
					getNodeName(scene, int(group)).c_str(),
					it->second.data.tilesX, it->second.data.tilesZ);
				ImGui::TextDisabled("%u x %u samples, %.0f m, group #%u",
					it->second.data.heights.width,
					it->second.data.heights.height,
					it->second.data.params.size, group);
				static const char* kOps[] = { "Raise", "Lower", "Flatten",
					"Smooth" };
				ImGui::Combo("Brush", &m_paintOp, kOps, 4);
				ImGui::DragFloat("Radius (m)", &m_paintRadius, 0.25f, 0.5f,
					256.0f);
				ImGui::DragFloat("Strength", &m_paintStrength, 0.01f, 0.01f,
					64.0f, "%.2f");
				if (ImGui::Checkbox("Paint mode (LMB paints this terrain)",
					&m_paintTerrain) && m_paintTerrain)
					m_toolTab = 2; // surface the Terrain toolbar tab
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("While on, LMB in a 3D viewport paints "
						"the selected terrain instead of picking. Each stamp "
						"is one undoable command; paint persists as tour "
						"terrainPaint verbs (deterministic replay).");
			}
		}

		// Clarisse-style override summary for this node (visibility control +
		// per-attr inherited/local/wide badges + wide-rule matches).
		DrawInspectorOverrides(node);

		// Environment node: mark/use this node as an IBL source (scene
		// definition â€” the block lives in the scene, the selection below
		// resolves into the rendered look).
		{
			ImGui::Separator();
			ImGui::Text("Environment node");
			if (IsEnvNode(node)) {
				ImGui::TextDisabled("marked as Environment");
				if (ImGui::Button("Use as current IBL source")) {
					m_envCurrent = node;
					ResolveSceneEnv();
				}
				ImGui::SameLine();
				if (ImGui::Button("Unmark##envnode"))
					UnmarkEnvNode(node);
			}
			else if (ImGui::Button("Mark as Environment node")) {
				MarkEnvNode(node);
				m_envCurrent = node;
				ResolveSceneEnv();
			}
			ImGui::TextDisabled(
				"position = node translate, size = node scale "
				"(convex volume later; cooked with the resolved look)");
		}

		// Scene reference block (placeholder nodes).
		DrawReferenceSection(node);
		// NOTE: the active-viewport orbit camera used to live here; it moved
		// to the World panel ("Viewport camera"). Node pages show authored
		// state only â€” camera nodes get the lens section above instead.
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

	void EditorApp::DrawLightmapPanel() {
		ImGui::Text("Lightmap bake (.sglm)");
		// Registry slots from the loaded world (first per kind wins, like App).
		if (m_world && !m_world->clusters.empty()) {
			for (const auto& clusterPtr : m_world->clusters) {
				if (!clusterPtr || clusterPtr->giBindings.empty())
					continue;
				for (const smvk::GiBinding& b : clusterPtr->giBindings) {
					const char* kindName = "?";
					switch (b.kind) {
					case smvk::GiBinding::Kind::Probe: kindName = "probe"; break;
					case smvk::GiBinding::Kind::Lightmap: kindName = "lightmap"; break;
					case smvk::GiBinding::Kind::RegManifest: kindName = "regmanifest"; break;
					case smvk::GiBinding::Kind::CvolManifest: kindName = "cvolmanifest"; break;
					case smvk::GiBinding::Kind::Cubemap: kindName = "cubemap"; break;
					}
					ImGui::TextDisabled("registry: %s '%s'",
						kindName, b.name.c_str());
				}
				break; // first cluster carrying bindings (matches runtime)
			}
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
				"gradient + stretch glow) instead of the baked content â€” "
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
	}

	void EditorApp::DrawTexturePanel() {
		ImGui::Text("Textures");
		if (!m_world || m_world->scene.hierarchy.empty()) {
			ImGui::TextDisabled("No scene loaded.");
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
		ImGui::Text("UV editor window (View menu)");
		{
			if (ImGui::Checkbox("Show UV editor", &m_showUvEditor)) {
				if (m_showUvEditor && m_uvWinMode == ViewMode::FlatUv1 && !m_lmGpu)
					m_uvWinMode = ViewMode::FlatUv0; // atlas not loaded
			}
			const char* chans[2] = { "TEXCOORD0 (diffuse)", "uv2 (lightmap)" };
			int cm = (m_uvWinMode == ViewMode::FlatUv1) ? 1 : 0;
			if (ImGui::Combo("Channel", &cm, chans, 2)) {
				if (cm == 1 && !m_lmGpu)
					Log("No lightmap atlas loaded - UV1 channel unavailable.");
				else
					m_uvWinMode = (cm == 1) ? ViewMode::FlatUv1 : ViewMode::FlatUv0;
			}
			float z = m_uvWinZoom;
			if (ImGui::SliderFloat("Zoom##uvwin", &z, 0.2f, 32.0f, "%.2f"))
				m_uvWinZoom = std::clamp(z, 0.2f, 32.0f);
			glm::vec2 p = m_uvWinPan;
			float pp[2] = { p.x, p.y };
			if (ImGui::DragFloat2("Pan (UV)##uvwin", pp, 0.01f, -4.0f, 4.0f))
				m_uvWinPan = glm::vec2(pp[0], pp[1]);
			if (ImGui::Button("Reset window (F)")) {
				m_uvWinZoom = 1.0f;
				m_uvWinPan = glm::vec2(0.0f);
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
		if (ImGui::Button("Flat UV diffuse")) {
			m_showUvEditor = true;
			m_uvWinMode = ViewMode::FlatUv0;
		}
		ImGui::SameLine();
		if (ImGui::Button("Flat UV lightmap") && m_lmGpu) {
			m_showUvEditor = true;
			m_uvWinMode = ViewMode::FlatUv1;
		}
		if (ImGui::Button(
			("UV inset: " + std::string(SecondViewName(m_secondView)) +
				" (F5)").c_str()))
			m_secondView = SecondView((int(m_secondView) + 1) % 3);
		if (m_lmGpu && ImGui::Button("Baked irradiance (F8)"))
			ActiveViewMode() = ViewMode::Baked;
		ImGui::TextDisabled("UV0 = authored uvs; UV1 = atlas-space texCoord2.");
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

	// ---- Images tab (scene image definitions, Maya file nodes) -------------
	// Image declarations OUTSIDE the DAG (like materials): a path +
	// sampling settings each. A material that uses one mints the cluster
	// texture resource at use time; until then these are authoring state.
	// Persisted as img.* editorSettings (legacy) AND cooked fileNode records
	// (path + colorspace + sampler intent â€” the cooker reads them for KTX
	// conversion); fileNodes win on load.
	void EditorApp::DrawImagesTab() {
		if (ImGui::Button("Add image...")) {
			std::string picked;
			if (NativeOpenFile(picked, true) && !picked.empty())
				AddImageDef(picked);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%zu image(s) â€” outside the DAG, editor-only",
			m_images.size());
		if (m_images.empty()) {
			ImGui::TextDisabled("Create > Image... adds one; a material that");
			ImGui::TextDisabled("uses an image mints its cluster texture.");
			return;
		}
		int pendingRemove = -1;
		ImGui::BeginChild("##imglist", ImVec2(0, 0), false);
		for (size_t i = 0; i < m_images.size(); ++i) {
			ImageDef& img = m_images[i];
			ImGui::PushID(int(i));
			ImGui::Separator();
			if (m_imageFocus == i) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.25f, 1.0f));
				ImGui::TextUnformatted(">> outliner selection");
				ImGui::PopStyleColor();
				ImGui::SetScrollHereY(0.5f);
			}
			ImGui::SetNextItemWidth(-120.0f);
			char nameBuf[128];
			std::snprintf(nameBuf, sizeof(nameBuf), "%s", img.name.c_str());
			if (ImGui::InputText("##imgname", nameBuf, sizeof(nameBuf))) {
				img.name = nameBuf;
				AutosaveTour();
			}
			ImGui::SameLine();
			if (ImGui::Button("Remove"))
				pendingRemove = int(i);
			{
				char pathBuf[512];
				std::snprintf(pathBuf, sizeof(pathBuf), "%s", img.path.c_str());
				ImGui::SetNextItemWidth(-120.0f);
				ImGui::InputText("##imgpath", pathBuf, sizeof(pathBuf),
					ImGuiInputTextFlags_ReadOnly);
				ImGui::SameLine();
				if (ImGui::Button("Browse...")) {
					std::string picked;
					if (NativeOpenFile(picked, true) && !picked.empty()
						&& picked != img.path) {
						img.path = picked;
						AutosaveTour();
					}
				}
			}
			static const char* kWrapNames[] = { "repeat", "clamp edge",
				"mirrored repeat" };
			static const char* kCsNames[] = { "sRGB", "linear" };
			static const char* kFilterNames[] = { "linear", "nearest" };
			ImGui::SetNextItemWidth(-4.0f);
			int w = img.wrap;
			if (ImGui::Combo("Tiling (wrap)##imgwrap", &w, kWrapNames, 3)
				&& w != img.wrap) {
				img.wrap = w;
				AutosaveTour();
			}
			ImGui::SetNextItemWidth(-4.0f);
			int cs = img.colorspace;
			if (ImGui::Combo("Colorspace##imgcs", &cs, kCsNames, 2)
				&& cs != img.colorspace) {
				img.colorspace = cs;
				AutosaveTour();
			}
			ImGui::SetNextItemWidth(-4.0f);
			int f = img.filter;
			if (ImGui::Combo("Filter##imgfilter", &f, kFilterNames, 2)
				&& f != img.filter) {
				img.filter = f;
				AutosaveTour();
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
		if (pendingRemove >= 0)
			RemoveImageDefAt(size_t(pendingRemove));
	}

	void EditorApp::DrawInsetTag() {
		// (Viewport legend removed â€” overlay window, menu item and chip
		// button deleted. The per-view refresh combos moved onto the
		// viewport chips, next to the per-panel view-mode combo.)

		// UV inset tag (top-left of the side strip, inside the viewport).
		if (SecondViewActive() && m_vpLayout == 0) {
			uint32_t stripX = 0;
			InsetStrip(m_vp, stripX);
			ImGui::SetNextWindowPos(ImVec2(float(stripX) + 8, m_menuH + 8),
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

	void EditorApp::DrawRecoverDialog() {
		// Autosave recovery is NEVER silent: a newer (or orphan) autosave
		// arms this modal at load; the user picks the document explicitly.
		if (!m_recoverArmed)
			return;
		if (!ImGui::BeginPopupModal("Recover autosave", nullptr,
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
			return;
		ImGui::Text("An autosave is newer than the tour file.");
		ImGui::TextDisabled("tour: %s", m_recoverTour.c_str());
		ImGui::TextDisabled("autosave (%s): %s",
			m_recoverStamp.empty() ? "unknown time" : m_recoverStamp.c_str(),
			m_recoverAutosave.c_str());
		if (ImGui::Button("Recover autosave")) {
			smvk::tour::Doc doc;
			std::string err;
			std::vector<std::string> warns;
			if (m_world && smvk::tour::LoadTour(m_recoverAutosave, doc,
				err, &warns)) {
				for (auto& w : warns)
					Log(w);
				if (ReplayTourDoc(doc, m_gltfPath)) {
					Log("Recovered from autosave: " + m_recoverAutosave);
					// The content is live now (and the next edit
					// re-autosaves): the stale file goes away.
					std::error_code ec;
					std::filesystem::remove(
						std::filesystem::path(m_recoverAutosave), ec);
				}
				else {
					Log("Autosave recovery failed (kept the tour file).");
				}
			}
			else {
				Log("Autosave recovery failed: " + err);
			}
			m_recoverArmed = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Keep tour file")) {
			// The stale autosave stays on disk: it will offer again on the
			// next open (until an explicit save clears it).
			Log("Kept the tour file; the autosave stays pending.");
			m_recoverArmed = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void EditorApp::DrawCreateTerrainDialog() {
		if (!ImGui::BeginPopupModal("Create Terrain", nullptr,
			ImGuiWindowFlags_NoCollapse))
			return;
		ImGui::Text("Create a procedural terrain mesh (heightfield grid).");
		ImGui::Separator();
		ImGui::SetNextItemWidth(300.0f);
		if (ImGui::InputText("Name", m_terrainNameBuf, sizeof(m_terrainNameBuf),
			ImGuiInputTextFlags_EnterReturnsTrue))
			;
		ImGui::DragFloat3("Origin (x, 0, z)", m_terrainOrigin, 0.5f);
		ImGui::DragFloat("Size (m)", &m_terrainSize, 0.5f, 1.0f, 1024.0f);
		int res = m_terrainRes;
		if (ImGui::DragInt("Resolution (samples/side)", &res, 1.0f, 8, 512))
			m_terrainRes = res;
		ImGui::DragFloat("Hill height (m)", &m_terrainHeight, 0.1f, 0.0f, 128.0f);
		ImGui::DragFloat("Hill frequency", &m_terrainFreq, 0.002f, 0.0005f, 1.0f, "%.4f");
		int seed = m_terrainSeed;
		if (ImGui::DragInt("Noise seed", &seed, 1, 1, 1000000))
			m_terrainSeed = seed;
		int tX = m_terrainTilesX, tZ = m_terrainTilesZ;
		if (ImGui::DragInt("Tiles X", &tX, 1.0f, 1, 16, "%.0f"))
			m_terrainTilesX = tX;
		if (ImGui::DragInt("Tiles Z", &tZ, 1.0f, 1, 16, "%.0f"))
			m_terrainTilesZ = tZ;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Tile split: each tile is its own mesh node "
				"(per-tile materials/texture layers); edges stay continuous.");
		// Material combo over the live world materials; the editor default
		// (editor://default) is the fallback and the dialog's default pick.
		if (m_world && !m_world->materialLocations.empty()) {
			const uint32_t def = EnsureDefaultMaterial();
			if (m_terrainMat == UINT32_MAX
				|| m_terrainMat >= m_world->materialLocations.size())
				m_terrainMat = def != UINT32_MAX ? def : 0;
			std::string preview = "mat." + std::to_string(m_terrainMat);
			if (auto* M = m_world->GetMaterial(m_terrainMat)) {
				const auto& loc = m_world->materialLocations[m_terrainMat];
				const std::string uri = loc.cluster
					? std::string(loc.cluster->GetMaterialUri(loc.handle).Str())
					: "";
				preview += " " + uri;
				(void)M;
			}
			if (ImGui::BeginCombo("Material", preview.c_str())) {
				for (uint32_t i = 0; i < m_world->materialLocations.size(); ++i) {
					const auto& loc = m_world->materialLocations[i];
					std::string uri = loc.cluster
						? std::string(loc.cluster->GetMaterialUri(loc.handle).Str())
						: "";
					char item[320];
					std::snprintf(item, sizeof(item), "%u: %s", i,
						uri.empty() ? "(unnamed)" : uri.c_str());
					if (ImGui::Selectable(item, m_terrainMat == i))
						m_terrainMat = i;
				}
				ImGui::EndCombo();
			}
		}
		else {
			ImGui::TextDisabled("Material: (scene has no materials)");
			m_terrainMat = UINT32_MAX;
		}
		ImGui::Separator();
		if (ImGui::Button("Create")) {
			TerrainDraft d{};
			d.name = m_terrainNameBuf;
			d.origin = glm::vec3(m_terrainOrigin[0], m_terrainOrigin[1],
				m_terrainOrigin[2]);
			d.size = m_terrainSize;
			d.res = uint32_t(m_terrainRes);
			d.height = m_terrainHeight;
			d.freq = m_terrainFreq;
			d.seed = m_terrainSeed;
			d.tilesX = uint32_t(m_terrainTilesX);
			d.tilesZ = uint32_t(m_terrainTilesZ);
			d.material = m_terrainMat;
			DraftCreateTerrain(d);
			m_showTerrainDialog = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			m_showTerrainDialog = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void EditorApp::ShowCookPanel() {
		if (!m_world) {
			Log("Cook: no scene open.");
			return;
		}
		if (m_cookOutPakBuf[0] == '\0') {
			std::filesystem::path p(m_gltfPath);
			std::string def = ProjectCookOutPak(p.stem().string());
			if (def.empty())
				def = ("content/paks/" + p.stem().string() + ".pak");
			std::snprintf(m_cookOutPakBuf, sizeof(m_cookOutPakBuf),
				"%s", def.c_str());
		}
		if (m_cookContentDirBuf[0] == '\0') {
			std::snprintf(m_cookContentDirBuf, sizeof(m_cookContentDirBuf),
				"content");
		}
		// Switch the bottom dock to the Cook tab (the panel lives
		// there now, not in a modal).
		m_bottomTab = 3;
	}

	void EditorApp::DrawCookPanel() {
		ImGui::Text("Export the current scene + tour edits to a cooked .pak.");
		ImGui::Separator();

		// Labels ride their OWN lines and fields take the full width â€”
		// shared label/widget lines collide in narrow modals.
		ImGui::Text("Output .pak path (relative to content/ or absolute):");
		{
			const float browseW = 90.0f;
			ImGui::SetNextItemWidth((std::max)(80.0f,
				ImGui::GetContentRegionAvail().x - browseW));
			ImGui::InputText("##outPak", m_cookOutPakBuf,
				sizeof(m_cookOutPakBuf));
			ImGui::SameLine();
			if (ImGui::Button("Browse...")) {
				std::string picked;
				if (NativeSaveFile(picked) && !picked.empty()) {
					std::snprintf(m_cookOutPakBuf, sizeof(m_cookOutPakBuf),
						"%s", picked.c_str());
				}
			}
		}

		ImGui::Text("Content root (for VFS resolution):");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##contentDir", m_cookContentDirBuf,
			sizeof(m_cookContentDirBuf));

		ImGui::Separator();
		ImGui::Text("Quality settings (CLI overrides tour defaults):");

		if (ImGui::Checkbox("MeshOpt (meshoptimizer)", &m_cook.meshopt))
			AutosaveTour();
		ImGui::SameLine();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Reorder vertices/indices for GPU cache efficiency.");

		if (ImGui::Checkbox("Generate LODs", &m_cook.lods))
			AutosaveTour();
		ImGui::SameLine();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Build mesh LOD chains via meshoptimizer.");
		if (m_cook.lods) {
			ImGui::Indent();
			ImGui::Text("LOD ratios (csv, e.g. \"0.6,0.3,0.12\"):");
			char ratios[64];
			std::snprintf(ratios, sizeof(ratios), "%s",
				m_cook.lodRatios.c_str());
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputText("##lodRatios", ratios,
				sizeof(ratios),
				ImGuiInputTextFlags_EnterReturnsTrue)) {
				m_cook.lodRatios = ratios;
				AutosaveTour();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Screen-size retention per level.");
			ImGui::Text("LOD error:");
			float lerr = m_cook.lodError;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragFloat("##lodError", &lerr, 0.001f, 0.0f,
				1.0f, "%.3f")) {
				m_cook.lodError = lerr;
				AutosaveTour();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Simplification target error (relative).");
			ImGui::Unindent();
		}

		if (ImGui::Checkbox("Meshlets", &m_cook.meshlets))
			AutosaveTour();
		ImGui::SameLine();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Per-meshlet build over the cooked topology "
				"(GPU mesh-shader friendly clusters).");
		if (m_cook.meshlets) {
			ImGui::Indent();
			ImGui::Text("Max verts:");
			int mv = int(m_cook.meshletMaxVerts);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragInt("##meshletVerts", &mv, 1.0f, 3, 255)) {
				m_cook.meshletMaxVerts = uint32_t(std::clamp(mv, 3, 255));
				AutosaveTour();
			}
			ImGui::Text("Max tris:");
			int mt = int(m_cook.meshletMaxTris);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragInt("##meshletTris", &mt, 1.0f, 1, 512)) {
				m_cook.meshletMaxTris = uint32_t(std::clamp(mt, 1, 512));
				AutosaveTour();
			}
			ImGui::Text("Cone weight:");
			float cw = m_cook.meshletConeWeight;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::SliderFloat("##meshletCone", &cw, 0.0f, 1.0f)) {
				m_cook.meshletConeWeight = cw;
				AutosaveTour();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Partitioning bias toward tighter cone "
					"bounds (0 = cluster shape only).");
			ImGui::Unindent();
		}

		if (ImGui::Checkbox("Auto-convert textures (KTX2)", &m_cook.autoKtx))
			AutosaveTour();
		ImGui::SameLine();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Non-KTX sources transcode in-process at cook "
				"(file-node colorspace honored, `.ktx2` sidecar cached next "
				"to the source). Off restores warn-and-skip.");

		if (ImGui::Checkbox("Lightmaps (bake)", &m_cook.lightmap))
			AutosaveTour();
		if (m_cook.lightmap) {
			ImGui::Indent();
			ImGui::Text("Voxel proxy resolution:");
			int pres = m_cook.lmProxyRes;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragInt("##lmProxyRes", &pres, 1.0f, 0, 256)) {
				m_cook.lmProxyRes = pres;
				AutosaveTour();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Voxel-remesh scene proxy grid (0 = legacy per-mesh path).");

			ImGui::Text("Meters per texel:");
			float mpt = m_cook.lmMetersPerTexel;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragFloat("##lmMpt", &mpt, 0.01f, 0.01f, 1.0f, "%.3f")) {
				m_cook.lmMetersPerTexel = mpt;
				AutosaveTour();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("World-space lightmap texel density (m/texel).");

			ImGui::Text("Max SG lobes:");
			int lobes = m_cook.lmLobes;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragInt("##lmLobes", &lobes, 1.0f, 5, 12)) {
				m_cook.lmLobes = std::clamp(lobes, 5, 12);
				AutosaveTour();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Preferred max SG bases per texel (5-12).");

			ImGui::Text("Atlas width (px):");
			int atlasW = m_cook.lmAtlasWidth;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragInt("##lmAtlasW", &atlasW, 64.0f, 256, 8192)) {
				m_cook.lmAtlasWidth = std::clamp(atlasW, 256, 8192);
				AutosaveTour();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Lightmap atlas width; height grows with content.");

			ImGui::Text("Rays per texel:");
			int rays = m_cook.lmRays;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragInt("##lmRays", &rays, 8.0f, 64, 8192)) {
				m_cook.lmRays = std::clamp(rays, 64, 8192);
				AutosaveTour();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Path-traced rays per lightmap texel (quality).");

			ImGui::Unindent();
		}

		if (ImGui::Checkbox("Cubemap captures", &m_cook.cubemap))
			AutosaveTour();
		if (m_cook.cubemap) {
			ImGui::Indent();
			ImGui::Text("Name (blank = pak stem):");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputText("##cubemapName", m_cookCubemapNameBuf, sizeof(m_cookCubemapNameBuf));
			if (ImGui::IsItemDeactivatedAfterEdit())
				m_cook.cubemapName = m_cookCubemapNameBuf;
			ImGui::Text("Size:");
			int size = m_cook.cubemapSize;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragInt("##cubemapSize", &size, 32.0f, 64, 2048)) {
				m_cook.cubemapSize = std::clamp(size, 64, 2048);
				AutosaveTour();
			}
			ImGui::Text("Rays per texel:");
			int rays = m_cook.cubemapRays;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragInt("##cubemapRays", &rays, 8.0f, 1, 4096)) {
				m_cook.cubemapRays = std::clamp(rays, 1, 4096);
				AutosaveTour();
			}
			// Placement density (the octree knobs — Cook.exe's
			// --cubemap-octree* set): adaptive placement instead of the
			// uniform lattice. Open cells over the max subdivide for
			// coverage; the min is the detail floor; the depth caps the
			// tree; growth stops at the 32-probe runtime budget.
			if (ImGui::Checkbox("Octree placement (adaptive)", &m_cook.cubemapOctree))
				AutosaveTour();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Sparse adaptive placement: open volumes stay coarse,\nnear-surface cells refine to the detail floor, embedded centers\ndrop, growth stops at the 32-probe runtime budget.");
			if (m_cook.cubemapOctree) {
				ImGui::Indent();
				if (ImGui::DragFloat("Max cell (m)##cubeoct",
						&m_cook.cubemapOctreeMax, 0.25f, 0.5f, 64.0f, "%.1f"))
					AutosaveTour();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Open cells larger than this subdivide (coverage).");
				if (ImGui::DragFloat("Min cell (m)##cubeoct",
						&m_cook.cubemapOctreeMin, 0.1f, 0.125f,
						std::max(m_cook.cubemapOctreeMax, 0.25f), "%.2f"))
					AutosaveTour();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Detail floor: never subdivide below this (probe DENSITY).");
				{
					int depth = int(m_cook.cubemapOctreeDepth);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::DragInt("Depth cap##cubeoct", &depth, 0.25f, 0, 8)) {
						m_cook.cubemapOctreeDepth = uint32_t(std::clamp(depth, 0, 8));
						AutosaveTour();
					}
				}
				if (ImGui::DragFloat("Pinned influence (m)##cubeoct",
						&m_cook.cubemapExtraRadius, 0.25f, 0.5f, 50.0f, "%.1f"))
					AutosaveTour();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Influence radius of pinned extra probes (fade = half).\nMatches the SceneCubemap authoring default.");
				ImGui::Unindent();
			}
			ImGui::Unindent();
		}

		ImGui::Separator();
		ImGui::Text("GI (probes) â€” baked ON DEMAND, never with the cook");
		ImGui::TextDisabled("%u cvolume node(s) authored in this scene",
			m_world ? uint32_t(m_world->scene.cvolForNode.size()) : 0u);
		if (ImGui::Checkbox("Convex volumes (bake authored)", &m_cook.giCvol))
			AutosaveTour();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Cvolume nodes bake via Bake GI below (GiBaker):\n"
				"gi/<pak-stem>_*.cvol + .cvmanifest sidecars.\n"
				"Skipped when the definition has no cvolumes.");
		if (ImGui::Checkbox("World field (adaptive octree)", &m_cook.giWorld))
			AutosaveTour();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("gi/<pak-stem>.sggi: the global fallback the\n"
				"runtime samples outside any convex volume.\n"
				"This is the slow stage (minutes on real scenes) â€” it\n"
				"runs only via Bake GI below. Unbaked scenes render\n"
				"sky IBL instead.");
		if (!m_giBakedHash.empty()) {
			const bool fresh = !m_cookDefHash.empty()
				&& m_giBakedHash == m_cookDefHash;
			ImGui::TextDisabled("GI baked (%s)", fresh ? "sidecars fresh"
				: "may predate this cook â€” rebake to refresh");
		}
		if (m_bakeRunning) {
			ImGui::Text("Baking GI... (detail in the task window)");
			ImGui::ProgressBar(-1.0f, ImVec2(-1, 0), "Baking GI..."); // indeterminate
		}
		else if (ImGui::Button("Bake GI now")) {
			BakeGI(); // guards + logs inside (needs a cook target)
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Run GiBaker over the current definition tour.\n"
				"Sidecars land next to the cook's out pak; the live view\n"
				"rebinds itself on completion. Cooks stay GI-free.");

		ImGui::Separator();
		ImGui::Text("Octree precook (in-process overlay, no baker run)");
		size_t dirtyTiles = 0;
		for (const OctreeTile& t : m_octreeTiles)
			if (t.dirty)
				++dirtyTiles;
		ImGui::TextDisabled("%zu tile(s), %zu dirty",
			m_octreeTiles.size(), dirtyTiles);
		ImGui::Checkbox("Show precook overlay", &m_octreePreviewOn);
		ImGui::Checkbox("Auto-update on edit/select", &m_octreeAutoUpdate);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Recompute the tile grid when the scene changes\n"
				"and refresh when a capture below is clicked.");
		float tileSize = m_octreeTileSize;
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::DragFloat("Tile size (m)", &tileSize, 0.5f, 1.0f, 32.0f)) {
			m_octreeTileSize = std::clamp(tileSize, 1.0f, 32.0f);
			if (m_octreeAutoUpdate)
				UpdateOctreePreview();
			else
				MarkOctreeDirty();
		}
		if (ImGui::Button("Update precook")) {
			UpdateOctreePreview();
		}
		ImGui::SameLine();
		if (ImGui::Button("New .cubemap")) {
			CreateCubemapAsset(m_assetsDir);
		}
		ImGui::SameLine();
		if (ImGui::Button("New .gicvolume")) {
			CreateGicvolumeAsset(m_assetsDir,
				m_selected != UINT32_MAX ? m_selected : UINT32_MAX);
		}
		// Clickable captures: selecting one also refreshes the overlay
		// when auto-update is on (the "automatic update" half). The census
		// shares the overlay's match, so the readout and the balls agree —
		// black balls with "0/N baked" means no baked cubemap is loaded for
		// these pins (bake + rebind first), not a display bug.
		if (m_world && !m_world->scene.cubemapForNode.empty()) {
			size_t bakedCount = 0, totalCount = 0;
			CaptureBakeState(bakedCount, totalCount);
			ImGui::TextDisabled("captures (click to select): %zu/%zu baked",
				bakedCount, totalCount);
			int shown = 0;
			for (const auto& [node, def] : m_world->scene.cubemapForNode) {
				if (shown++ >= 12)
					break;
				(void)def;
				char btn[128];
				std::snprintf(btn, sizeof(btn), "%s (#%u)##cube%u",
					getNodeName(m_world->scene, int(node)).c_str(),
					node, node);
				if (ImGui::SmallButton(btn)) {
					SetSelection(node);
					if (m_octreeAutoUpdate)
						UpdateOctreePreview();
				}
				if (shown % 3 != 0)
					ImGui::SameLine();
			}
		}
		// On-demand cubemap refresh (capture-only quick bake, GiBaker
		// skipped) + the live face strip below: finished captures
		// appear as KTX-decoded face strips while the cook runs.
		if (m_cookRunning && m_cubeRunActive && m_cubeTotal > 0) {
			ImGui::Text("Cubemaps %d/%d captures...",
				m_cubeDone, m_cubeTotal);
			ImGui::ProgressBar(
				float(m_cubeDone) / float(m_cubeTotal), ImVec2(-1, 0), "");
		}
		else if (!m_cookRunning && !m_bakeRunning) {
			if (ImGui::Button("Update cubemaps now")) {
				CookCubemapsNow();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Cubemap-only quick bake (GiBaker skipped): re-traces\n"
					"every capture in the set; faces land below as each\n"
					"capture finishes. Loose sidecars override the pak\n"
					"through the VFS overlay.");
		}
		DrawCubeFaceStrip();

		ImGui::Separator();
		ImGui::TextDisabled("Cook = PRODUCT step: a .pak for the game/App "
			"(Run in App loads it). The editor is unaffected by cooks.");
		if (m_cookRunning) {
			ImGui::Text("Cooking... (detail in the task window)");
			ImGui::ProgressBar(-1.0f, ImVec2(-1, 0), "Cooking..."); // indeterminate
		}
		else {
			if (m_cookJustFinished) {
				ImGui::TextColored(m_cookLastOk
					? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
					: ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
					"%s", m_cookLastOk ? "Last cook finished OK."
						: "Last cook FAILED (see Console).");
			}
			if (ImGui::Button("Cook")) {
				m_cook.outPak = m_cookOutPakBuf;
				m_cook.contentDir = m_cookContentDirBuf;
				CookToPak();
			}
		}
	}

	// ---- Material editor (right-dock tab) ---------------------------------
	// Texture-centric material authoring: pick a material, assign its PBR
	// texture slots from the cluster texture registry, manage layers, and add
	// textures as FILE PATH refs (KTX2 loads into the pool with the path as
	// the URI â€” cook re-embeds the stashed bytes). Deeper per-material pages
	// (emissive/alpha/flags/travel) live in the material focus page.
	void EditorApp::DrawMaterialEditor() {
		if (!m_world || m_world->materialLocations.empty()) {
			ImGui::TextDisabled("No materials (open a scene).");
			return;
		}
		smvk::Cluster* cl = m_world->clusters.empty()
			? nullptr : m_world->clusters[0].get();
		// Material picker.
		if (m_matEditorSel == UINT32_MAX
			|| m_matEditorSel >= m_world->materialLocations.size())
			m_matEditorSel = m_defaultMatWorld != UINT32_MAX
			? m_defaultMatWorld : 0;
		{
			const auto& loc = m_world->materialLocations[m_matEditorSel];
			std::string preview = "mat." + std::to_string(m_matEditorSel);
			if (loc.cluster) {
				const std::string uri =
					std::string(loc.cluster->GetMaterialUri(loc.handle).Str());
				if (!uri.empty())
					preview += " " + uri;
			}
			if (ImGui::BeginCombo("Material", preview.c_str())) {
				for (uint32_t i = 0; i < m_world->materialLocations.size(); ++i) {
					const auto& l2 = m_world->materialLocations[i];
					std::string uri = l2.cluster
						? std::string(l2.cluster->GetMaterialUri(l2.handle).Str())
						: "";
					char item[320];
					std::snprintf(item, sizeof(item), "%u: %s", i,
						uri.empty() ? "(unnamed)" : uri.c_str());
					if (ImGui::Selectable(item, m_matEditorSel == i))
						m_matEditorSel = i;
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Focus >>")) {
				m_materialTrail.push_back(m_materialFocus);
				m_materialFocus = m_matEditorSel;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Opens the full material page in the "
					"Inspector (emissive, alpha, flags, layer travel).");
		}
		const uint32_t mat = m_matEditorSel;
		smvk::Material* m = m_world->GetMaterial(mat);
		if (!m) {
			ImGui::TextDisabled("Material resolve failed.");
			return;
		}
		// ---- PBR texture slots -------------------------------------------
		// Slot attrs are pool-index scalars (-1 = none); the combo lists the
		// cluster's registered textures (file path refs included).
		ImGui::Separator();
		ImGui::Text("Texture slots");
		struct SlotDef { const char* label; const char* attr; int* slot; };
		// Pointer copies read live values per frame (post-Do refresh).
		int tBC = m->values.textures.x, tMR = m->values.textures.y;
		int tN = m->values.textures.z, tO = m->values.textures.w;
		int tE = m->values.textures2.x;
		const SlotDef slots[] = {
			{ "Base color", "baseColorTex", &tBC },
			{ "Metallic-rough", "metallicRoughTex", &tMR },
			{ "Normal", "normalTex", &tN },
			{ "Occlusion", "occlusionTex", &tO },
			{ "Emissive", "emissiveTex", &tE },
		};
		std::vector<std::pair<smvk::StringViewT, smvk::TextureHandle>> texList;
		if (cl)
			texList = cl->ListTextureUris();
		for (const SlotDef& s : slots) {
			ImGui::PushID(s.attr);
			std::string preview = *s.slot >= 0
				? ("tex." + std::to_string(*s.slot)) : std::string("(none)");
			// Reverse-lookup a friendly URI for the preview.
			if (*s.slot >= 0)
				for (const auto& [uri, th] : texList)
					if (int(th.index()) == *s.slot) {
						preview = uri.Str();
						break;
					}
			if (ImGui::BeginCombo(s.label, preview.c_str())) {
				if (ImGui::Selectable("(none)", *s.slot < 0)) {
					EditCmd c{};
					c.verb = EditVerb::SetMatAttr;
					c.matWorld = mat;
					c.attr = s.attr;
					c.number = -1.0;
					ReadSharedMatAttr(mat, s.attr, c.beforeVec4, c.beforeNum);
					Do(c);
				}
				for (const auto& [uri, th] : texList) {
					char item[420];
					std::snprintf(item, sizeof(item), "%u: %s",
						uint32_t(th.index()), uri.Str().c_str());
					if (ImGui::Selectable(item, *s.slot == int(th.index()))) {
						EditCmd c{};
						c.verb = EditVerb::SetMatAttr;
						c.matWorld = mat;
						c.attr = s.attr;
						c.number = double(th.index());
						ReadSharedMatAttr(mat, s.attr, c.beforeVec4, c.beforeNum);
						Do(c);
					}
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
		}
		// ---- Layers ------------------------------------------------------
		ImGui::Separator();
		ImGui::Text("Layers (vertex-color blending)");
		ImGui::TextDisabled("RGBA weights blend layers 1..3 over this "
			"material (layer 0).");
		{
			glm::ivec4 lm = m->values.layerMaterials;
			const uint32_t count = uint32_t(m_world->materialLocations.size());
			for (int slot = 0; slot < 3; ++slot) {
				ImGui::PushID(slot);
				int cur = lm[slot];
				std::string preview = (cur >= 0 && uint32_t(cur) < count)
					? ("mat." + std::to_string(cur)) : std::string("(none)");
				char comboLabel[32];
				std::snprintf(comboLabel, sizeof(comboLabel), "Layer %d",
					slot + 1);
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
		}
		// ---- Texture registry (file path refs) ----------------------------
		ImGui::Separator();
		ImGui::Text("Textures (%zu)", texList.size());
		for (const auto& [uri, th] : texList) {
			ImGui::BulletText("%u  %s", uint32_t(th.index()), uri.Str().c_str());
		}
		ImGui::SetNextItemWidth(-90.0f);
		ImGui::InputTextWithHint("##matTexPath", "Texture file path...",
			m_matTexPathBuf, sizeof(m_matTexPathBuf));
		ImGui::SameLine();
		if (ImGui::Button("Add...##tex")) {
			if (m_matTexPathBuf[0] != '\0') {
				// Native file dialog would be nicer; the path field is the
				// direct ref (also exactly what the URI becomes).
				if (AddTextureFromFile(m_matTexPathBuf) >= 0)
					m_matTexPathBuf[0] = '\0';
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Loads the texture into the texture pool with "
				"the file path as its URI (a filepath ref). KTX2 loads "
				"directly; PNG/JPG/... transcode to KTX2 (sRGB) on the way. "
				"Source bytes are stashed so cooks re-embed them.");
		ImGui::TextDisabled("Slots store pool indices; -1 clears a slot.");
	}

	void EditorApp::RevealInExplorer(const std::string& absPath) {
		// explorer /select needs backslashes on Windows.
#ifdef _WIN32
		std::string win = absPath;
		for (char& c : win)
			if (c == '/')
				c = '\\';
		// "explorer /select," fails for non-existing paths: open the
		// parent folder instead, plain-open the file's dir as the last
		// resort. A non-zero "success" HINSTANCE just means it launched.
		std::error_code ec;
		if (std::filesystem::is_regular_file(absPath, ec)) {
			// /select,<path> pre-selects the file in an Explorer window.
			if (reinterpret_cast<INT_PTR>(::ShellExecuteA(nullptr, "open",
				"explorer", ("/select," + win).c_str(), nullptr,
				SW_SHOWNORMAL)) <= 32)
				Log("Reveal failed: '" + absPath + "'.");
		}
		else {
			const std::string dir =
				std::filesystem::path(absPath).parent_path().string();
			if (reinterpret_cast<INT_PTR>(::ShellExecuteA(nullptr, "open",
				dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
				Log("Reveal failed: '" + dir + "'.");
		}
#else
		Log("Reveal in Explorer is Windows-only.");
#endif
	}

	void EditorApp::DrawAssetReadonlyPage() {
		// READ-ONLY asset inspector (a .material/.mesh/.gicook/.lmcook click
		// in the asset browser): view parameters only — edits happen on
		// SCENES (the tour-level overrides/assignments), never on the
		// derived/cook assets (derived assets regenerate on reimport; cook
		// assets rebake through the bake execution). The loaded asset caches
		// per selection (never per frame: .mesh blobs can be megabytes).
		// EXCEPTION — .texture (user-authored, not derived): its IMPORT
		// SETTINGS are the authoring, so LDR sources edit them here (a
		// KTX2 source pre-decided them in its header — read-only rows).
		const bool wantTex = m_assetsSel.size() > 8
			&& m_assetsSel.compare(m_assetsSel.size() - 8, 8, ".texture") == 0;
		const bool wantMat = !wantTex && m_assetsSel.size() > 9
			&& m_assetsSel.compare(m_assetsSel.size() - 9, 9, ".material") == 0;
		const bool wantMesh = !wantTex && !wantMat && m_assetsSel.size() > 9
			&& (m_assetsSel.compare(m_assetsSel.size() - 9, 9, ".geometry") == 0
				// legacy pre-10/6 blobs (read-only inspection keeps working
				// until the next re-import derives ".geometry")
				|| m_assetsSel.compare(m_assetsSel.size() - 5, 5, ".mesh") == 0);
		const bool wantGi = !wantTex && !wantMat && !wantMesh && m_assetsSel.size() > 7
			&& m_assetsSel.compare(m_assetsSel.size() - 7, 7, ".gicook") == 0;
		const bool wantLm = !wantTex && !wantMat && !wantMesh && !wantGi
			&& m_assetsSel.size() > 7
			&& m_assetsSel.compare(m_assetsSel.size() - 7, 7, ".lmcook") == 0;
		const bool wantCm = !wantTex && !wantMat && !wantMesh && !wantGi && !wantLm
			&& m_assetsSel.size() > 8
			&& m_assetsSel.compare(m_assetsSel.size() - 8, 8, ".cubemap") == 0;
		const bool wantCv = !wantTex && !wantMat && !wantMesh && !wantGi && !wantLm && !wantCm
			&& m_assetsSel.size() > 10
			&& m_assetsSel.compare(m_assetsSel.size() - 10, 10, ".gicvolume") == 0;
		if (!wantTex && !wantMat && !wantMesh && !wantGi && !wantLm && !wantCm && !wantCv)
			return;
		// Cache refresh on selection change (or a failed load retries never:
		// a re-derive/re-bake + re-click reloads).
		if (m_assetInspectPath != m_assetsSel) {
			m_assetInspectPath = m_assetsSel;
			m_assetInspectMat.reset();
			m_assetInspectMesh.reset();
			m_assetInspectGiCook.reset();
			m_assetInspectLmCook.reset();
			m_assetInspectCubemap.reset();
			m_assetInspectGicvolume.reset();
			m_assetInspectTex.reset();
			m_assetInspectTexInfoOk = false;
			std::string err;
			if (wantTex) {
				m_assetInspectTex.emplace();
				if (!smvk::assets::LoadTextureAsset(m_assetsSel,
					*m_assetInspectTex, err)) {
					Log("Assets: texture inspect failed: " + err);
					m_assetInspectTex.reset();
				}
				else {
					// Probe the SOURCE (beside the .texture): the read-only
					// header facts for KTX2, the dims for LDR.
					std::ifstream in(std::filesystem::path(m_assetsSel)
						.parent_path() / m_assetInspectTex->source,
						std::ios::binary);
					if (in) {
						std::vector<uint8_t> bytes(
							(std::istreambuf_iterator<char>(in)),
							std::istreambuf_iterator<char>());
						m_assetInspectTexInfoOk = ProbeTextureSource(
							bytes.data(), bytes.size(),
							m_assetInspectTexInfo);
					}
					if (!m_assetInspectTexInfoOk)
						Log("Assets: texture source not probeable: "
							+ m_assetInspectTex->source);
				}
			}
			else if (wantMat) {
				m_assetInspectMat.emplace();
				if (!smvk::assets::LoadMaterialAsset(m_assetsSel,
					*m_assetInspectMat, err)) {
					Log("Assets: material inspect failed: " + err);
					m_assetInspectMat.reset();
				}
			}
			else if (wantMesh) {
				m_assetInspectMesh.emplace();
				if (!smvk::assets::LoadMeshAsset(m_assetsSel,
					*m_assetInspectMesh, err)) {
					Log("Assets: mesh inspect failed: " + err);
					m_assetInspectMesh.reset();
				}
			}
			else if (wantGi) {
				m_assetInspectGiCook.emplace();
				if (!smvk::cook::LoadGiCookAsset(m_assetsSel,
					*m_assetInspectGiCook, err)) {
					Log("Assets: GI cook inspect failed: " + err);
					m_assetInspectGiCook.reset();
				}
			}
			else if (wantCm) {
				m_assetInspectCubemap.emplace();
				if (!smvk::cook::LoadCubemapAsset(m_assetsSel,
					*m_assetInspectCubemap, err)) {
					Log("Assets: cubemap inspect failed: " + err);
					m_assetInspectCubemap.reset();
				}
			}
			else if (wantCv) {
				m_assetInspectGicvolume.emplace();
				if (!smvk::cook::LoadGicvolumeAsset(m_assetsSel,
					*m_assetInspectGicvolume, err)) {
					Log("Assets: GI volume inspect failed: " + err);
					m_assetInspectGicvolume.reset();
				}
			}
			else {
				m_assetInspectLmCook.emplace();
				if (!smvk::cook::LoadLightmapCookAsset(m_assetsSel,
					*m_assetInspectLmCook, err)) {
					Log("Assets: lightmap cook inspect failed: " + err);
					m_assetInspectLmCook.reset();
				}
			}
		}
		// TEXTURE ASSET page: user-authored import settings (the authoring
		// surface, not read-only). KTX2 sources show the header-decided rows
		// READ-ONLY; LDR sources edit them (saved into the .texture; applied
		// at the next import/cook — materials referencing this asset pick the
		// settings up through ImportTextureAssetToCluster).
		if (m_assetInspectTex) {
			smvk::assets::TextureAsset& t = *m_assetInspectTex;
			const TextureSourceInfo& info = m_assetInspectTexInfo;
			const bool infoOk = m_assetInspectTexInfoOk;
			ImGui::Text("Texture asset: '%s'",
				t.name.empty()
				? std::filesystem::path(m_assetsSel).stem().string().c_str()
				: t.name.c_str());
			ImGui::TextDisabled("guid %s", t.guid.c_str());
			ImGui::TextDisabled("source %s (%s)", t.source.c_str(),
				infoOk ? (info.isKtx2 ? "KTX2" : "LDR image") : "not probeable");
			if (infoOk)
				ImGui::TextDisabled("%ux%u%s", info.width, info.height,
					info.isKtx2 ? "" : (info.components
						? (info.components == 4 ? " RGBA" : " RGB") : ""));
			ImGui::Separator();
			if (infoOk && info.isKtx2) {
				// The .ktx2 pre-decided these in its header — read-only rows.
				ImGui::TextDisabled("colorSpace: %s (from the .ktx2 header)",
					info.srgb ? "sRGB" : "linear");
				ImGui::TextDisabled("format: %s (vkFormat %u)",
					info.bcn ? "BCn block-compressed"
					: (info.vkFormat == 37 ? "rgba8"
						: (info.vkFormat == 43 ? "rgba8 sRGB" : "other")),
					info.vkFormat);
				if (info.basis)
					ImGui::TextDisabled("payload: Basis-family (the cook "
						"transcodes to per-semantic BCn)");
				ImGui::TextDisabled("mip levels: %u (the .ktx2 chain)",
					info.mipLevels);
				ImGui::Separator();
				ImGui::TextWrapped("The KTX2 source carries its own encoding: "
					"colorSpace, format and mip levels are read-only. The "
					"sampler rows still apply.");
			}
			else {
				// LDR source: the import settings ARE the authoring (the
				// sampler rows below are shared with KTX2 sources).
				bool changed = false;
				{
					const char* items[] = { "sRGB (color)", "linear (data)" };
					int cs = t.colorspace == 1 ? 1 : 0;
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::Combo("colorSpace##texasset", &cs, items, 2)) {
						t.colorspace = cs;
						changed = true;
					}
				}
				{
					const char* items[] = { "rgba8 (uncompressed)", "bc1 (RGB)",
						"bc3 (RGBA)", "bc4 (R)", "bc5 (RG)", "bc7 (RGBA hq)" };
					int fmt = std::clamp(t.format, 0, 5);
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::Combo("format##texasset", &fmt, items, 6)) {
						t.format = fmt;
						changed = true;
					}
				}
				{
					int mips = t.mipLevels;
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::InputInt("mip levels (0 = auto)##texasset",
						&mips, 1, 1)) {
						t.mipLevels = std::clamp(mips, 0, 32);
						changed = true;
					}
				}
				if (changed) {
					std::string serr;
					if (smvk::assets::SaveTextureAsset(m_assetsSel, t, serr))
						Log("Texture asset saved: "
							+ std::filesystem::path(m_assetsSel)
							.filename().string()
							+ " (applies at the next import/cook).");
					else
						Log("Texture asset save failed: " + serr);
				}
			}
			// Sampler rows apply to BOTH source kinds (never baked into a
			// KTX2 payload; they ride the pak's per-texture sampler record).
			{
				const char* items[] = { "linear", "nearest" };
				int f = t.filter != 0 ? 1 : 0;
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("filter##texassetsmp", &f, items, 2)) {
					t.filter = f;
					std::string serr;
					if (smvk::assets::SaveTextureAsset(m_assetsSel, t, serr))
						Log("Texture asset saved: "
							+ std::filesystem::path(m_assetsSel)
							.filename().string() + " (sampler).");
				}
				const char* wraps[] = { "repeat", "clamp", "mirror" };
				int w = std::clamp(t.wrap, 0, 2);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("wrap##texassetsmp", &w, wraps, 3)) {
					t.wrap = w;
					std::string serr;
					if (smvk::assets::SaveTextureAsset(m_assetsSel, t, serr))
						Log("Texture asset saved: "
							+ std::filesystem::path(m_assetsSel)
							.filename().string() + " (sampler).");
				}
			}
			ImGui::Separator();
			ImGui::TextWrapped("Materials reference this asset by file name: "
				"tex.baseColor=<name>.texture in a .material (the import "
				"routes through these settings). The cook embeds the "
				"converted payload as KTX2 in the pak.");
			return;
		}
		ImGui::TextDisabled("Asset (read-only) - edits happen on scenes");
		ImGui::Separator();
		if (m_assetInspectMat) {
			const smvk::assets::MaterialAsset& m = *m_assetInspectMat;
			ImGui::Text("Material: '%s'", m.name.c_str());
			ImGui::TextDisabled("guid %s", m.guid.c_str());
			const glm::vec4& bc = m.info.baseColorFactor;
			ImGui::ColorEdit4("Base color##ro", const_cast<float*>(&bc.x),
				ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker
				| ImGuiColorEditFlags_NoAlpha);
			ImGui::TextDisabled("metallic: %.2f  roughness: %.2f",
				m.info.metallicFactor, m.info.roughnessFactor);
			const glm::vec3& em = m.info.emissiveFactor;
			ImGui::ColorEdit3("Emissive##ro", const_cast<float*>(&em.x),
				ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker);
			const char* alpha = (m.info.alphaMode == smvk::AlphaMode::Mask)
				? "mask" : (m.info.alphaMode == smvk::AlphaMode::Blend)
				? "blend" : "opaque";
			ImGui::TextDisabled("alpha: %s (%.2f cutoff)  %s",
				alpha, m.info.alphaCutoff,
				m.info.doubleSided ? "double-sided" : "single-sided");
			ImGui::Separator();
			ImGui::Text("Textures");
			auto tex = [&](const char* slot,
				const smvk::assets::MaterialAsset::TexRef& t) {
				ImGui::TextDisabled("%-18s %s", slot,
					t.path.empty() ? "-" : t.path.c_str());
			};
			tex("baseColor", m.baseColor);
			tex("metallicRoughness", m.metallicRoughness);
			tex("normal", m.normalTex);
			tex("occlusion", m.occlusion);
			tex("emissive", m.emissiveTex);
			ImGui::Separator();
			ImGui::TextDisabled("Assign: drag onto a viewport (selection) or "
				"use a scene's material override.");
		}
		else if (m_assetInspectMesh) {
			const smvk::assets::MeshAsset& k = *m_assetInspectMesh;
			ImGui::Text("Mesh: '%s'",
				std::filesystem::path(m_assetsSel).stem().string().c_str());
			uint64_t tris = 0, verts = 0;
			for (const auto& g : k.geometry) {
				verts += g.positions.size();
				tris += g.indices.size() / 3;
			}
			ImGui::TextDisabled("%u nodes, %u instances, %zu blobs, "
				"%llu verts, %llu tris",
				uint32_t(k.nodes.size()), uint32_t(k.instances.size()),
				k.geometry.size(),
				(unsigned long long)verts, (unsigned long long)tris);
			uint32_t lights = uint32_t(k.lights.size());
			uint32_t cams = uint32_t(k.cameras.size());
			ImGui::TextDisabled("%u light(s), %u camera(s)", lights, cams);
			ImGui::Separator();
			ImGui::Text("Hierarchy (first 32)");
			ImGui::BeginChild("##meshnodes", ImVec2(0, 0), true);
			const size_t n = std::min(k.nodes.size(), size_t(32));
			for (size_t i = 0; i < n; ++i) {
				ImGui::TextDisabled("%2zu p%-4d %s", i, k.nodes[i].parent,
					k.names[i].empty() ? "(node)" : k.names[i].c_str());
			}
			if (k.nodes.size() > n)
				ImGui::TextDisabled("... (%zu more)", k.nodes.size() - n);
			ImGui::EndChild();
			ImGui::Separator();
			ImGui::TextDisabled("Drop onto a viewport to add a scene "
				"reference; the tour binds it by GUID.");
			if (m_world && !m_cookRunning && ImGui::Button("Add to scene")) {
				Log("Assets: adding geometry asset '" + m_assetsSel
					+ "' (asset bind).");
				AddSceneReference(m_assetsSel, {}, true);
			}
		}
		else if (m_assetInspectGiCook) {
			// GI cook asset (settings + blob result): the Bake GI execution
			// that fills the blob is the follow-up — this page only reports
			// WHAT would bake and WHERE the result points.
			const smvk::cook::GiCookAsset& c = *m_assetInspectGiCook;
			ImGui::Text("GI cook: '%s'", c.name.c_str());
			ImGui::TextDisabled("guid %s", c.guid.c_str());
			ImGui::TextDisabled("scene: %s",
				c.scene.empty() ? "(unbound)" : c.scene.c_str());
			ImGui::TextDisabled("convex volumes: %s   world field: %s",
				c.cvol ? "on" : "off", c.world ? "on" : "off");
			ImGui::Separator();
			if (c.blobGuid.empty()) {
				ImGui::TextDisabled("blob: unbaked (no result yet)");
			}
			else {
				const bool stale = smvk::cook::GiCookBlobStale(c);
				ImGui::TextDisabled("blob: %s (%s)%s", c.blobGuid.c_str(),
					c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
					stale ? "  STALE (settings changed)" : "  fresh");
			}
		}
		else if (m_assetInspectLmCook) {
			// Lightmap cook asset (settings + blob result): same contract —
			// the atlas bake that fills the blob is the follow-up.
			const smvk::cook::LightmapCookAsset& c = *m_assetInspectLmCook;
			ImGui::Text("Lightmap cook: '%s'", c.name.c_str());
			ImGui::TextDisabled("guid %s", c.guid.c_str());
			ImGui::TextDisabled("scene: %s",
				c.scene.empty() ? "(unbound)" : c.scene.c_str());
			ImGui::TextDisabled("density: %.3f m/texel  lobes: %u",
				c.lmMetersPerTexel, c.lmLobes);
			ImGui::TextDisabled("atlas width: %u  rays: %u  proxy: %d",
				c.lmAtlasWidth, c.lmRays, c.lmProxyRes);
			if (c.lmExcludeSmall > 0.0f || !c.lmExcludeKw.empty()) {
				ImGui::TextDisabled("exclude: small < %.3f  kw: '%s'",
					c.lmExcludeSmall, c.lmExcludeKw.c_str());
			}
			ImGui::TextDisabled("sun: az %.1f el %.1f int %.0f  sky %.0f",
				c.sunAzimuth, c.sunElevation, c.sunIntensity,
				c.skyIntensity);
			ImGui::Separator();
			if (c.blobGuid.empty()) {
				ImGui::TextDisabled("blob: unbaked (no result yet)");
			}
			else {
				const bool stale = smvk::cook::LightmapCookBlobStale(c);
				ImGui::TextDisabled("blob: %s (%s)%s", c.blobGuid.c_str(),
					c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
					stale ? "  STALE (settings changed)" : "  fresh");
			}
		}
		else if (m_assetInspectCubemap) {
			const smvk::cook::CubemapAsset& c = *m_assetInspectCubemap;
			ImGui::Text("Cubemap: '%s'", c.name.c_str());
			ImGui::TextDisabled("guid %s", c.guid.c_str());
			ImGui::TextDisabled("scene: %s",
				c.scene.empty() ? "(unbound)" : c.scene.c_str());
			ImGui::TextDisabled("capture: %s  size: %d  rays: %d",
				c.capture.empty() ? "(scene stem)" : c.capture.c_str(),
				c.size, c.rays);
			ImGui::Separator();
			if (c.blobGuid.empty()) {
				ImGui::TextDisabled("blob: unbaked (no result yet)");
			}
			else {
				const bool stale = smvk::cook::CubemapBlobStale(c);
				ImGui::TextDisabled("blob: %s (%s)%s", c.blobGuid.c_str(),
					c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
					stale ? "  STALE (settings changed)" : "  fresh");
			}
		}
		else if (m_assetInspectGicvolume) {
			const smvk::cook::GicvolumeAsset& c = *m_assetInspectGicvolume;
			ImGui::Text("GI volume: '%s'", c.name.c_str());
			ImGui::TextDisabled("guid %s", c.guid.c_str());
			ImGui::TextDisabled("scene: %s",
				c.scene.empty() ? "(unbound)" : c.scene.c_str());
			ImGui::TextDisabled("node: %u  priority: %u", c.node, c.priority);
			ImGui::Separator();
			if (c.blobGuid.empty()) {
				ImGui::TextDisabled("blob: unbaked (no result yet)");
			}
			else {
				const bool stale = smvk::cook::GicvolumeBlobStale(c);
				ImGui::TextDisabled("blob: %s (%s)%s", c.blobGuid.c_str(),
					c.blobPath.empty() ? "no path" : c.blobPath.c_str(),
					stale ? "  STALE (settings changed)" : "  fresh");
			}
		}
		else {
			ImGui::TextDisabled("(asset load failed - see the console)");
		}
	}

	void EditorApp::DrawImportableTexturePage() {
		// IMPORTABLE SOURCE page: a supported image format selected in the
		// browser but not yet imported as a .texture asset. Probes the file
		// (dims + kind) and offers the import button — the .texture wrap
		// carries the import settings (colorSpace/format/mipLevels/filter/
		// wrap) from here on. A project is required (the .texture lands
		// beside the source, adopted into the assetdb).
		namespace fs = std::filesystem;
		ImGui::Text("Texture source: '%s'",
			fs::path(m_assetsSel).filename().string().c_str());
		ImGui::TextDisabled("%s", m_assetsSel.c_str());
		ImGui::Separator();
		// Probe on selection change (cached like the asset pages).
		if (m_assetInspectPath != m_assetsSel) {
			m_assetInspectPath = m_assetsSel;
			m_assetInspectTex.reset();
			m_assetInspectTexInfoOk = false;
			std::ifstream in(m_assetsSel, std::ios::binary);
			if (in) {
				std::vector<uint8_t> bytes(
					(std::istreambuf_iterator<char>(in)),
					std::istreambuf_iterator<char>());
				m_assetInspectTexInfoOk = ProbeTextureSource(
					bytes.data(), bytes.size(), m_assetInspectTexInfo);
			}
			if (!m_assetInspectTexInfoOk)
				Log("Assets: texture source not probeable: " + m_assetsSel);
		}
		if (m_assetInspectTexInfoOk) {
			const TextureSourceInfo& info = m_assetInspectTexInfo;
			ImGui::TextDisabled("%ux%u %s", info.width, info.height,
				info.isKtx2 ? "KTX2 (settings read-only at import)"
				: (info.components == 4 ? "RGBA" : "RGB"));
			ImGui::TextWrapped("Importing wraps this source in a .texture "
				"asset: import settings (colorSpace, format, mip levels, "
				"filter, wrap) %s, identity (.meta + assetdb), and "
				"materials reference it as tex.<slot>=<name>.texture.",
				info.isKtx2
				? "read from the .ktx2 header (it pre-decided them)"
				: "you author in the Inspector after the import");
		}
		ImGui::Separator();
		ImGui::BeginDisabled(m_projectRoot.empty());
		if (ImGui::Button("Import as .texture")) {
			// The .texture lands beside the source (same dir — the copy in
			// CreateTextureAsset is a no-op for an in-project file).
			const std::string dir =
				fs::path(m_assetsSel).parent_path().generic_string();
			CreateTextureAsset(m_assetsSel, dir);
		}
		ImGui::EndDisabled();
		if (m_projectRoot.empty() && ImGui::IsItemHovered())
			ImGui::SetTooltip("Open or create a project first (File > New / "
				"Open Project...).");
	}

	void EditorApp::DrawProjectDialog() {
		// FORCED startup project selection: a blocking modal (no close â€”
		// the editor is project-bound; smoke flows never see it). Recents
		// first (one click; the current project sits on top), New/Browse
		// for anything else. Opened from DrawMenuBar's popup tail.
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing,
			ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Select project", nullptr,
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoSavedSettings))
			return;
		ImGui::Text("Select a project");
		ImGui::TextDisabled(
			"The editor is project-bound: scenes, imports, asset GUIDs "
			"and the artifact cache all live in the project folder. "
			"Pick a recent project below, or create/open one.");
		ImGui::Separator();
		ImGui::Text("Recent projects");
		bool opened = false;
		if (m_recentProjects.empty()) {
			ImGui::TextDisabled("(none yet - create or browse for one)");
		}
		for (size_t i = 0; i < m_recentProjects.size(); ++i) {
			const std::string& dir = m_recentProjects[i];
			ImGui::PushID(int(i));
			const std::string name =
				std::filesystem::path(dir).filename().string();
			const bool isProject = IsProjectDir(dir);
			const bool current = (dir == m_projectRoot);
			if (ImGui::Selectable(("##rec" + std::to_string(i)).c_str(),
				current, ImGuiSelectableFlags_SpanAllColumns,
				ImVec2(0, 0))) {
				// Row click (single) opens too â€” the dialog is one step.
				if (isProject) {
					opened = OpenProject(dir);
					if (!opened)
						Log("Project dialog: cannot open '" + dir
							+ "' (see log).");
				}
				else {
					Log("Project dialog: '" + dir + "' is not a project "
						"(no project.tourproj) - removed from recents.");
					m_recentProjects.erase(m_recentProjects.begin()
						+ std::ptrdiff_t(i));
					SavePrefs();
					ImGui::PopID();
					break; // iterator invalidated
				}
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("%s", dir.c_str());
			ImGui::SameLine();
			if (current)
				ImGui::TextDisabled("(open)");
			else if (!isProject)
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
					"(marker missing)");
			ImGui::PopID();
		}
		ImGui::Separator();
		if (ImGui::Button("New / Open Project...")) {
			std::string picked;
			if (NativePickFolder(picked) && !picked.empty()) {
				opened = NewProjectOrOpen(picked);
				if (!opened)
					Log("Project dialog: cannot open '" + picked
						+ "' (see log).");
			}
		}
		if (opened) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		ImGui::EndPopup();
	}

	void EditorApp::DrawAssetsPanel() {
		// Asset browser: a GRID directory explorer of the project folder
		// (Unity Project window style). Breadcrumb + Up; folders first;
		// SVG icon tiles (m_icons via ImDrawList::AddImage, drawn shapes
		// as the fallback); a green badge marks assets with a registered
		// GUID (assets.db); click selects, double-click enters folders /
		// opens scenes; Import + Rescan ride the header.
		if (m_projectRoot.empty()) {
			ImGui::TextDisabled("Assets: open a project first (File > New / "
				"Open Project...).");
			return;
		}
		namespace fs = std::filesystem;
		ImGui::BeginChild(
			"##assets_root",
			ImVec2(0, 0),
			false,
			ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse
		);

		// Navigation state heal: home = <root>/content.
		std::error_code ec;
		if (m_assetsDir.empty()
			|| !fs::is_directory(fs::path(m_assetsDir), ec)
			|| !InProjectDir(m_assetsDir)) {
			m_assetsDir =
				(fs::path(m_projectRoot) / "content").generic_string();
			m_assetsSel.clear();
		}

		// ---- OS drag-and-drop target (Windows Explorer -> browser) -------
		// The SDL hook (OnSDLEvent) collects SDL_EVENT_DROP_FILE batches;
		// route them HERE by the drop point against this panel's rect (the
		// only drop target in the editor): images become .texture assets in
		// the browsed dir, .gltf/.glb run the full import. A drop outside
		// the panel drops with a log (nothing silently swallowed).
		if (m_pendingDropX >= 0.0f) {
			const ImVec2 wp = ImGui::GetWindowPos();
			const ImVec2 ws = ImGui::GetWindowSize();
			const ImVec2 vp = ImGui::GetMainViewport()->Pos;
			const float dx = vp.x + m_pendingDropX;
			const float dy = vp.y + m_pendingDropY;
			if (dx >= wp.x && dx < wp.x + ws.x
				&& dy >= wp.y && dy < wp.y + ws.y)
				HandleDroppedFiles(m_pendingDropFiles);
			else if (!m_pendingDropFiles.empty())
				Log("Drop: landed outside the asset browser ("
					+ std::to_string(m_pendingDropFiles.size())
					+ " file(s) ignored — drop onto the Assets panel).");
			m_pendingDropX = -1.0f;
			m_pendingDropY = -1.0f;
			m_pendingDropFiles.clear();
		}

		// Header controls live in this non-scrolling root.  Wrap every
		// action when the dock gets narrow instead of spilling into the grid.
		auto SameLineIfFits = [](const char* nextLabel) {
			const ImGuiStyle& style = ImGui::GetStyle();
			const float width = ImGui::CalcTextSize(nextLabel, nullptr, true).x
				+ style.FramePadding.x * 2.0f;
			const float right = ImGui::GetWindowPos().x
				+ ImGui::GetWindowContentRegionMax().x;
			if (ImGui::GetItemRectMax().x + style.ItemSpacing.x
				+ width <= right)
				ImGui::SameLine();
		};
		// Wrap a group of neighboring widgets as one unit (breadcrumb arrow
		// and the button after it must stay together).
		auto SameLineIfWidthFits = [](float width) {
			const ImGuiStyle& style = ImGui::GetStyle();
			const float right = ImGui::GetWindowPos().x
				+ ImGui::GetWindowContentRegionMax().x;
			if (ImGui::GetItemRectMax().x + style.ItemSpacing.x
				+ width <= right)
				ImGui::SameLine();
		};
		// Truncate status text so paths/UUIDs cannot escape the dock's width.
		auto FitLabel = [](const std::string& text, float maxWidth) {
			if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
				return text;
			std::string shown = text;
			while (!shown.empty() && ImGui::CalcTextSize(
				("..." + shown).c_str()).x > maxWidth)
				shown.erase(shown.begin());
			return "..." + shown;
		};

		// ---- header row 1: navigation breadcrumb (wrapping) ------------
		ImGui::BeginDisabled(ProjectRelativePath(m_assetsDir).empty());
		if (ImGui::SmallButton("^ Up")) {
			const std::string rel = ProjectRelativePath(m_assetsDir);
			const size_t slash = rel.find_last_of('/');
			m_assetsDir = (slash == std::string::npos)
				? m_projectRoot
				: (fs::path(m_projectRoot) / rel.substr(0, slash))
				.generic_string();
			m_assetsSel.clear();
		}
		ImGui::EndDisabled();
		SameLineIfFits(m_projectName.c_str());
		if (ImGui::SmallButton(m_projectName.c_str())) {
			m_assetsDir = m_projectRoot;
			m_assetsSel.clear();
		}
		{
			fs::path acc = fs::path(m_projectRoot);
			std::string rel = ProjectRelativePath(m_assetsDir);
			int depth = 0;
			while (!rel.empty()) {
				const size_t slash = rel.find('/');
				const std::string comp = (slash == std::string::npos)
					? rel : rel.substr(0, slash);
				acc /= comp;
				const std::string shown = comp.size() > 22
					? comp.substr(0, 19) + "..." : comp;
				const ImGuiStyle& style = ImGui::GetStyle();
				const float width = ImGui::CalcTextSize(">").x
					+ style.ItemSpacing.x
					+ ImGui::CalcTextSize(shown.c_str()).x
					+ style.FramePadding.x * 2.0f;
				SameLineIfWidthFits(width);
				ImGui::TextDisabled(">");
				ImGui::SameLine();
				ImGui::PushID(depth++);
				if (ImGui::SmallButton(shown.c_str())) {
					m_assetsDir = acc.generic_string();
					m_assetsSel.clear();
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", acc.generic_string().c_str());
				ImGui::PopID();
				rel = (slash == std::string::npos)
					? std::string() : rel.substr(slash + 1);
			}
		}

		// Own line, NEVER SameLine with the filter or actions.
		const std::string currentPath = "Path: " + m_assetsDir;
		const std::string pathLabel = FitLabel(currentPath,
			ImGui::GetContentRegionAvail().x);
		ImGui::TextDisabled("%s", pathLabel.c_str());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", m_assetsDir.c_str());

		// ---- header row 2: collection filter and expanded views --------
		const char* filters[9] = { "All (files)", "Meshes", "Materials",
			"Animations", "GI bakes", "Cubemaps", "GI cooks",
			"Lightmap cooks", "GI volumes" };
		ImGui::SetNextItemWidth(std::min(180.0f,
			std::max(1.0f, ImGui::GetContentRegionAvail().x)));
		ImGui::Combo("##assetsfilter", &m_assetsFilter, filters, 9);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Collection views: derived assets across "
				"the project's content tree.");
		if (!m_assetsExpandGltf.empty()) {
			const std::string stem = fs::path(m_assetsExpandGltf).stem().string();
			const std::string btn = stem + " X##expand";
			SameLineIfFits(btn.c_str());
			if (ImGui::SmallButton(btn.c_str())) {
				m_assetsExpandGltf.clear();
				m_assetsSel.clear();
			}
		}
		if (!m_assetsExpandTour.empty()) {
			const std::string stem = fs::path(m_assetsExpandTour).stem().string();
			const std::string btn = stem + " X##expandtour";
			SameLineIfFits(btn.c_str());
			if (ImGui::SmallButton(btn.c_str())) {
				m_assetsExpandTour.clear();
				m_assetsSel.clear();
			}
		}

		// ---- header row 3: actions (responsive, not scrolling) ----------
		ImGui::Separator();
		if (ImGui::Button("Import asset...")) {
			std::string picked;
			if (NativeOpenFile(picked) && !picked.empty()
				&& ImportAsset(picked)) {
				// Navigate to where the import landed.
				m_assetsDir = (fs::path(m_projectRoot) / "content"
					/ "models").generic_string();
				m_assetsSel.clear();
			}
		}
		// Cook-asset creation (same helper as the Create menu — placement
		// dir is the browsed directory).
		SameLineIfFits("New...");
		if (ImGui::Button("New...")) {
			ImGui::OpenPopup("##newcookasset");
		}
		if (ImGui::BeginPopup("##newcookasset")) {
			// NOTE: .gicook/.lmcook are NOT listed here on purpose — they
			// are an internal consequence of baking (minted/updated per
			// scene by RecordBakeAssetsFromCook), never user-created.
			if (ImGui::MenuItem("Cubemap Asset (.cubemap)")) {
				CreateCubemapAsset(m_assetsDir);
			}
			if (ImGui::MenuItem("GI Volume Asset (.gicvolume)")) {
				CreateGicvolumeAsset(m_assetsDir);
			}
			// Texture asset (from a picked source image): the .texture
			// import-settings record + identity (see CreateTextureAsset).
			if (ImGui::MenuItem("Texture (from image)...")) {
				std::string picked;
				if (NativeOpenFile(picked, true) && !picked.empty())
					CreateTextureAsset(picked, m_assetsDir);
			}
			ImGui::EndPopup();
		}
		if (m_assetDb.IsOpen()) {
			SameLineIfFits("Rescan");
			if (ImGui::Button("Rescan")) {
				smvk::cache::RescanStats stats;
				std::string rerr;
				if (m_assetDb.RescanProject(stats, rerr))
					Log("Asset rescan: "
						+ std::to_string(stats.adopted) + " adopted, "
						+ std::to_string(stats.moved) + " re-pointed, "
						+ std::to_string(stats.derived) + " derived, "
						+ std::to_string(stats.missing) + " missing.");
				else
					Log("Asset rescan failed: " + rerr);
			}
		}
		// Selection actions (Open = scene files only; paks run in the App).
		static const auto isOpenable = [](const std::string& p) {
			std::string e = fs::path(p).extension().string();
			for (char& c : e)
				c = char(std::tolower((unsigned char)c));
			return e == ".gltf" || e == ".glb" || e == ".tour";
		};
		{
			const bool selFile = !m_assetsSel.empty()
				&& fs::is_regular_file(fs::path(m_assetsSel), ec);
			SameLineIfFits("Open");
			ImGui::BeginDisabled(!(selFile && isOpenable(m_assetsSel)));
			if (ImGui::SmallButton("Open##sel")) {
				if (!m_cookRunning)
					OpenAnySceneFile(m_assetsSel);
				else
					Log("Assets: a cook is running - wait for it to "
						"finish.");
			}
			ImGui::EndDisabled();
			SameLineIfFits("Reveal");
			ImGui::BeginDisabled(m_assetsSel.empty());
			if (ImGui::SmallButton("Reveal##sel"))
				RevealInExplorer(m_assetsSel);
			ImGui::EndDisabled();
			// Add-to-scene (mesh/geometry selection): the click twin of the
			// viewport drag-drop (AddSceneReference asset-bind). Imports
			// land in-project first (ImportAsset), so this only ever adds
			// in-project assets. A .gltf/.glb container adds its first
			// derived geometry (same as expanding it and dropping the tile).
			{
				auto isMeshAsset = [](const std::string& p) {
					std::string e = fs::path(p).extension().string();
					for (char& c : e)
						c = char(std::tolower((unsigned char)c));
					return e == ".geometry" || e == ".mesh";
				};
				auto isGltfContainer = [](const std::string& p) {
					std::string e = fs::path(p).extension().string();
					for (char& c : e)
						c = char(std::tolower((unsigned char)c));
					return e == ".gltf" || e == ".glb";
				};
				const bool canAdd = selFile && m_world != nullptr && !m_cookRunning
					&& (isMeshAsset(m_assetsSel) || isGltfContainer(m_assetsSel));
				SameLineIfFits("Add to scene");
				ImGui::BeginDisabled(!canAdd);
				if (ImGui::SmallButton("Add to scene##sel")) {
					std::string target = m_assetsSel;
					if (isGltfContainer(target) && !isMeshAsset(target)) {
						// Resolve the container's first derived geometry.
						smvk::assets::AssetManifest man;
						std::string merr;
						if (smvk::assets::LoadAssetManifest(target, man, merr)) {
							const std::string base = fs::path(target)
								.parent_path().generic_string();
							for (const auto& e : man.entries) {
								if (e.kind == smvk::assets::AssetKind::Geometry) {
									target = (fs::path(base) / e.file)
										.generic_string();
									break;
								}
							}
						}
					}
					if (isMeshAsset(target) || target != m_assetsSel) {
						Log("Assets: adding geometry asset '" + target
							+ "' (asset bind).");
						AddSceneReference(target, {}, true);
					}
					else {
						Log("Assets: '" + m_assetsSel
							+ "' has no derived mesh yet — reimport it first.");
					}
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Add the selected mesh to the scene\n"
						"(same as dropping its tile on a viewport).");
			}
			// Delete (selection action): plans + confirms through the modal
			// (refusals — open scene, derived assets, cache — log + no-op).
			SameLineIfFits("Delete");
			ImGui::BeginDisabled(m_assetsSel.empty());
			if (ImGui::SmallButton("Delete##sel"))
				RequestAssetDelete(m_assetsSel);
			ImGui::EndDisabled();
			// Selected file is informational and has its OWN line. Never
			// put a potentially long filename/UUID beside the action buttons.
			if (!m_assetsSel.empty()) {
				std::string selGuid;
				if (m_assetDb.IsOpen())
					m_assetDb.GuidForPath(m_assetsSel, selGuid);
				const std::string selected =
					fs::path(m_assetsSel).filename().string();
				const std::string label = FitLabel("Selected: " + selected,
					ImGui::GetContentRegionAvail().x);
				ImGui::TextDisabled("%s", label.c_str());
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", m_assetsSel.c_str());
				if (!selGuid.empty()) {
					SameLineIfFits("Copy UUID");
					if (ImGui::SmallButton("Copy UUID##sel"))
						ImGui::SetClipboardText(selGuid.c_str());
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", selGuid.c_str());
				}
			}
		}

		// ---- scrollable grid --------------------------------------------
		// Defer tile-context deletes until after the grid child has ended:
		// OpenPopup() and BeginPopupModal() must run at the same popup depth.
		std::string tileDeleteRequested;
		ImGui::Separator();
		// The grid alone fills the remaining height, so toolbar rows never
		// compete with tiles. IMPORTANT: obtain dl after BeginChild; using
		// the parent draw list causes custom labels/icons to bleed over the
		// toolbar when scrolling.
		ImGui::BeginChild("##assetgrid", ImVec2(0, 0), false,
			ImGuiWindowFlags_AlwaysVerticalScrollbar);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		// Background context menu (right-click the grid background): the
		// same creation as the header New... button (placement dir = the
		// browsed directory). .gicook/.lmcook are NOT listed — internal
		// bake consequences, never user-created.
		if (ImGui::BeginPopupContextWindow("##assetgrid_ctx",
			ImGuiPopupFlags_MouseButtonRight |
			ImGuiPopupFlags_NoOpenOverItems)) {
			if (ImGui::MenuItem("New Cubemap Asset (.cubemap)"))
				CreateCubemapAsset(m_assetsDir);
			if (ImGui::MenuItem("New GI Volume Asset (.gicvolume)"))
				CreateGicvolumeAsset(m_assetsDir);
			if (ImGui::MenuItem("New Texture (from image)...")) {
				std::string picked;
				if (NativeOpenFile(picked, true) && !picked.empty())
					CreateTextureAsset(picked, m_assetsDir);
			}
			ImGui::EndPopup();
		}
		// Registered GUID map (assets.db) for badges + tooltips. The rows
		// also feed the bake-asset collections below (kept in scope: the
		// collection branch reads them).
		std::unordered_map<std::string, std::string> guidByAbs;
		// The reverse map (guid -> abs path): the .tour folder view resolves
		// its bake refs (the tour's editorSet bake.*.guid keys) through the
		// assetdb — moves/renames heal through the rescan.
		std::unordered_map<std::string, std::string> absByGuid;
		std::vector<smvk::cache::AssetRecord> assetRows;
		if (m_assetDb.IsOpen()) {
			std::string derr;
			if (m_assetDb.All(assetRows, derr)) {
				for (const auto& r : assetRows) {
					const std::string abs = m_assetDb.ResolvePath(r.path);
					guidByAbs[abs] = r.guid;
					absByGuid[r.guid] = abs;
				}
			}
		}
		// The tour's bake-asset references (RecordBakeAssetsFromCook writes
		// them; SaveEnvToDoc persists them into the tour): the editorSet
		// guids the .tour folder view resolves through the assetdb. Tolerant
		// — an unreadable/unref'd tour presents as a plain openable file.
		auto tourBakeRefs = [](const std::string& tourAbs,
			std::vector<std::string>& out) {
			smvk::tour::Doc td;
			std::string terr;
			if (!smvk::tour::LoadTour(tourAbs, td, terr))
				return;
			for (const auto& [k, v] : td.editorSettings) {
				if (k != "bake.gicook.guid" && k != "bake.cubemap.guid")
					continue;
				std::string g = v;
				// The tokenizer glues the statement ';' onto the last
				// quoted -v token (see the tour tests); trim it.
				while (!g.empty()
					&& (g.back() == ';' || g.back() == ' '
						|| g.back() == '\r'))
					g.pop_back();
				if (!g.empty())
					out.push_back(g);
			}
		};
		// Entries: directories first, then files, both by name (skip
		// .meta sidecars + dotfiles).
		struct Entry {
			std::string name;
			std::string abs;
			bool dir = false;
			IconType icon{};
			bool gltf = false;  // .gltf with a manifest: presents as folder
			bool tour = false;  // .tour with bake refs: presents as folder
			bool back = false;  // the expanded view's ".." tile
			// Drag-drop payload kinds ("" = not draggable).
			bool dragMesh = false;
			bool dragMaterial = false;
		};
		std::vector<Entry> dirs, files;
		const auto iconForAsset = [&](smvk::assets::AssetKind k) {
			switch (k) {
			case smvk::assets::AssetKind::Geometry: return IconType::Model;
			case smvk::assets::AssetKind::Material:
				return IconType::Texture;
			case smvk::assets::AssetKind::Anim: return IconType::Animation;
			}
			return IconType::File;
		};
		// A glTF's derived assets (its manifest children), for BOTH the
		// expanded view and the collection filters.
		const std::string expandGltf = m_assetsExpandGltf;
		const std::string expandTour = m_assetsExpandTour;
		auto manifestChildren = [&](const std::string& gltfAbs,
			int kindFilter, std::vector<Entry>& out) {
			smvk::assets::AssetManifest man;
			std::string merr;
			if (!smvk::assets::LoadAssetManifest(gltfAbs, man, merr))
				return;
			const std::string base =
				fs::path(gltfAbs).parent_path().generic_string();
			for (const auto& e : man.entries) {
				if (kindFilter >= 0
					&& int(e.kind) != kindFilter)
					continue;
				Entry t{};
				t.name = e.name.empty() ? e.file : e.name;
				t.abs = (fs::path(base) / e.file).generic_string();
				t.icon = iconForAsset(e.kind);
				t.dragMesh = (e.kind == smvk::assets::AssetKind::Geometry);
				t.dragMaterial =
					(e.kind == smvk::assets::AssetKind::Material);
				out.push_back(std::move(t));
			}
		};
		// Recursive .gltf.meta scan under the current directory (the
		// collection views list every derived asset of the kind).
		std::vector<std::string> gltfMetas;
		if (m_assetsFilter != 0 || !expandGltf.empty()) {
			std::vector<fs::path> stack{ fs::path(m_assetsDir) };
			while (!stack.empty()) {
				const fs::path d = stack.back();
				stack.pop_back();
				std::error_code sec;
				for (fs::directory_iterator it(d, sec), end;
					!sec && it != end; it.increment(sec)) {
					if (sec)
						break;
					std::error_code fec;
					if (it->is_directory(fec) && !fec) {
						stack.push_back(it->path());
						continue;
					}
					if (!it->is_regular_file(fec) || fec)
						continue;
					const std::string n =
						it->path().filename().string();
					if (n.size() > 10
						&& n.compare(n.size() - 10, 10, ".gltf.meta") == 0)
						gltfMetas.push_back(it->path().string());
				}
			}
		}
		if (m_assetsFilter != 0) {
			if (m_assetsFilter <= 3) {
				// Collection view: every derived asset of the kind under the
				// current directory tree.
				for (const std::string& meta : gltfMetas) {
					const std::string gltf = meta.substr(
						0, meta.size() - 5); // strip ".meta"
					manifestChildren(gltf, m_assetsFilter - 1, files);
				}
			}
			else {
				// Bake/cook-asset collections (GI bakes / cubemap captures /
				// GI cooks / lightmap cooks): the assetdb rows adopted from
				// the .meta sidecars (see Engine/Asset/bake_asset.h for the
				// bake rows, Engine/Asset/cook_asset.h for the cook rows) —
				// moves/renames heal through the rescan, so these stay
				// correct where stem-named loose listings would rot. Rows
				// whose file is gone are skipped (the db keeps them as
				// retargets).
				const char* want = (m_assetsFilter == 4) ? "gi"
					: (m_assetsFilter == 5) ? "cubemap"
					: (m_assetsFilter == 6) ? "gicook"
					: (m_assetsFilter == 7) ? "lmcook" : "gicvolume";
				for (const auto& r : assetRows) {
					// The cubemap collection also lists legacy loose rows
					// (kind "cubemap" covers typed .cubemap assets AND the
					// migrated textures/cubemaps manifests).
					if (r.kind != want)
						continue;
					const std::string abs = m_assetDb.ResolvePath(r.path);
					std::error_code fec;
					if (abs.empty()
						|| !fs::is_regular_file(abs, fec) || fec)
						continue;
					Entry t{};
					t.name = fs::path(abs).filename().generic_string();
					t.abs = abs;
					t.icon = (m_assetsFilter == 5)
						? IconType::Env : IconType::GI;
					files.push_back(std::move(t));
				}
			}
		}
		else if (!expandGltf.empty()) {
			// Expanded glTF: its derived assets + a back tile (Unity-style
			// folder view; the glTF itself stays listed as the source).
			Entry back{};
			back.name = "..";
			back.abs = expandGltf;
			back.dir = true; // plain-directory styling
			back.back = true; // double-click = leave the folder
			back.icon = IconType::Folder;
			dirs.push_back(back);
			manifestChildren(expandGltf, -1, files);
			Entry src{};
			src.name = fs::path(expandGltf).filename().string();
			src.abs = expandGltf;
			src.icon = IconType::Model;
			src.gltf = false; // inside the folder, double-click opens the source
			files.push_back(src);
		}
		else if (!expandTour.empty()) {
			// Expanded .tour: the SAME folder presentation, over the tour's
			// BAKE ASSETS — the refs (RecordBakeAssetsFromCook) resolve
			// through the assetdb (moves/renames heal through the rescan).
			// The tour itself stays listed as the source tile: double-click
			// OPENS it (isOpenable — the tour remains openable from the
			// browser, nested inside its own folder view).
			Entry back{};
			back.name = "..";
			back.abs = expandTour;
			back.dir = true;
			back.back = true;
			back.icon = IconType::Folder;
			dirs.push_back(back);
			std::vector<std::string> bakeRefs;
			tourBakeRefs(expandTour, bakeRefs);
			for (const std::string& guid : bakeRefs) {
				const auto git = absByGuid.find(guid);
				if (git == absByGuid.end())
					continue; // not adopted (yet): the ref stays unlisted
				std::error_code fec;
				if (!fs::is_regular_file(git->second, fec) || fec)
					continue;
				Entry t{};
				t.name = fs::path(git->second).filename().generic_string();
				t.abs = git->second;
				t.icon = (t.name.size() > 13
					&& t.name.compare(t.name.size() - 13, 13,
						".cubemanifest") == 0)
					? IconType::Env : IconType::GI;
				files.push_back(std::move(t));
			}
			// The source itself: a plain openable .tour tile (double-click
			// opens the scene — see the tile handler's isOpenable branch).
			Entry src{};
			src.name = fs::path(expandTour).filename().string();
			src.abs = expandTour;
			src.icon = IconTypeFor(src.name, false);
			files.push_back(src);
		}
		else {
			for (fs::directory_iterator it(m_assetsDir, ec), end;
				!ec && it != end; it.increment(ec)) {
				if (ec)
					break;
				std::error_code fec;
				const bool isDir = it->is_directory(fec) && !fec;
				if (!isDir && (!it->is_regular_file(fec) || fec))
					continue;
				const fs::path& p = it->path();
				std::string name = p.filename().string();
				if (name.empty() || name[0] == '.')
					continue;
				bool isGltf = false;
				bool isTour = false;
				bool dragMesh = false;
				bool dragMaterial = false;
				IconType icon = IconTypeFor(name, isDir);
				if (!isDir) {
					std::string e = p.extension().string();
					for (char& c : e)
						c = char(std::tolower((unsigned char)c));
					if (e == ".meta")
						continue; // the asset's own identity sidecar
					if (e == ".gltf" || e == ".glb") {
						// With a manifest the glTF presents as a FOLDER of its
						// derived assets; without one it opens as a scene
						// (the legacy path).
						std::string merr;
						smvk::assets::AssetManifest man;
						isGltf = smvk::assets::LoadAssetManifest(
							p.generic_string(), man, merr);
						if (isGltf)
							icon = IconType::Folder;
					}
					else if (e == ".tour") {
						// With bake-asset refs (a cook ran —
						// RecordBakeAssetsFromCook) the tour presents as a
						// FOLDER of its bakes, mirroring the glTF-with-manifest
						// presentation; without refs it stays a plain openable
						// file (the legacy behavior).
						std::vector<std::string> refs;
						tourBakeRefs(p.generic_string(), refs);
						isTour = !refs.empty();
						if (isTour)
							icon = IconType::Folder;
					}
					else if (e == ".geometry" || e == ".mesh")
						dragMesh = true;
					else if (e == ".material")
						dragMaterial = true;
				}
				(isDir ? dirs : files).push_back({
					name,
					p.generic_string(),
					isDir,
					icon,
					isGltf,
					isTour,
					false,          // back
					dragMesh,
					dragMaterial
					});
			}
		}
		const auto byName = [](const Entry& a, const Entry& b) {
			std::string la = a.name, lb = b.name;
			for (char& c : la)
				c = char(std::tolower((unsigned char)c));
			for (char& c : lb)
				c = char(std::tolower((unsigned char)c));
			return la < lb;
		};
		std::sort(dirs.begin(), dirs.end(), byName);
		std::sort(files.begin(), files.end(), byName);

		const float tileW = 96.0f, tileH = 104.0f, iconS = 56.0f;
		int cols = std::max(1,
			int(ImGui::GetContentRegionAvail().x / tileW));
		int col = 0;
		auto tile = [&](const Entry& e) {
			ImGui::PushID(e.abs.c_str());
			ImGui::PushID(e.back ? 1 : 0);
			const bool sel = (m_assetsSel == e.abs);
			const ImVec2 tileSz(tileW - 8.0f, tileH);
			(void)ImGui::InvisibleButton("##tile", tileSz);
			const bool hovered = ImGui::IsItemHovered();
			const bool clicked = ImGui::IsItemClicked();
			const bool dbl = hovered
				&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			const ImVec2 bmin = ImGui::GetItemRectMin();
			const ImVec2 bmax = ImGui::GetItemRectMax();
			// hover/selection chrome under the icon art.
			if (sel) {
				dl->AddRectFilled(bmin, bmax,
					IM_COL32(70, 130, 190, 60), 6.0f);
				dl->AddRect(bmin, bmax, IM_COL32(120, 200, 255, 220),
					6.0f, 0, 1.5f);
			}
			else if (hovered) {
				dl->AddRectFilled(bmin, bmax,
					IM_COL32(255, 255, 255, 16), 6.0f);
			}
			// icon centered (SVG texture, drawn shapes as fallback).
			const ImVec2 ico(bmin.x + (tileSz.x - iconS) * 0.5f,
				bmin.y + 6.0f);
			const IconTex& icon = m_icons[size_t(e.icon)];
			if (icon.imgui != 0) {
				dl->AddImage(icon.imgui, ico,
					ImVec2(ico.x + iconS, ico.y + iconS));
			}
			else {
				DrawAssetIconFallback(dl, ico, iconS, e.icon);
			}
			// Registered-asset badge (green dot, bottom-right of icon).
			if (!e.dir && guidByAbs.contains(e.abs)) {
				dl->AddCircleFilled(
					ImVec2(ico.x + iconS - 3.0f, ico.y + iconS - 3.0f),
					3.5f, IM_COL32(90, 220, 120, 255), 10);
			}
			// Label: up to two centered lines, clipped with "...".
			{
				ImFont* font = ImGui::GetFont();
				const float fsize = ImGui::GetFontSize();
				const float maxW = (tileSz.x) - 10.0f;
				auto textW = [&](const char* s) {
					return font->CalcTextSizeA(fsize, 10000.0f, 0.0f, s).x;
				};
				std::string l1 = e.name, l2;
				if (textW(l1.c_str()) > maxW) {
					// Longest fitting prefix, preferring a separator.
					std::string growing;
					size_t best = 0;
					for (size_t i = 0; i < e.name.size(); ++i) {
						growing.push_back(e.name[i]);
						if (textW(growing.c_str()) > maxW)
							break;
						best = i + 1;
					}
					size_t cut = best;
					for (size_t i = best; i > 1; --i)
						if (e.name[i - 1] == '_' || e.name[i - 1] == '-'
							|| e.name[i - 1] == ' ' || e.name[i - 1] == '.') {
							cut = i;
							break;
						}
					l1 = e.name.substr(0, cut);
					l2 = e.name.substr(cut);
					if (textW(l2.c_str()) > maxW) {
						while (!l2.empty()
							&& textW((l2 + "...").c_str()) > maxW)
							l2.pop_back();
						l2 += "...";
					}
				}
				const ImU32 tcol =
					ImGui::GetColorU32(ImGuiCol_Text);
				dl->PushClipRect(bmin, bmax, true);
				const float yBase = bmax.y - 2.0f * fsize - 3.0f;
				const float cx = bmin.x + tileSz.x * 0.5f;
				if (l2.empty()) {
					dl->AddText(font, fsize,
						ImVec2(cx - textW(l1.c_str()) * 0.5f,
							yBase + fsize * 0.5f), tcol, l1.c_str());
				}
				else {
					dl->AddText(font, fsize,
						ImVec2(cx - textW(l1.c_str()) * 0.5f, yBase),
						tcol, l1.c_str());
					dl->AddText(font, fsize,
						ImVec2(cx - textW(l2.c_str()) * 0.5f,
							yBase + fsize + 1.0f), tcol, l2.c_str());
				}
				dl->PopClipRect();
			}
			if (clicked)
				m_assetsSel = e.abs;
			if (dbl) {
				m_assetsSel = e.abs;
				if (e.back) {
					// Leave the expanded glTF/tour folder.
					m_assetsExpandGltf.clear();
					m_assetsExpandTour.clear();
					m_assetsSel.clear();
				}
				else if (e.gltf) {
					// A glTF with a manifest: open the FOLDER (the derived
					// assets) — never the scene (the .tour references the
					// .mesh asset; the glTF is the container/source).
					m_assetsExpandGltf = e.abs;
					m_assetsSel.clear();
				}
				else if (e.tour) {
					// A .tour with bake refs: open the FOLDER (its bake
					// assets). The tour itself stays openable INSIDE (the
					// source tile's isOpenable branch below) and via the
					// header Open button.
					m_assetsExpandTour = e.abs;
					m_assetsSel.clear();
				}
				else if (e.dir) {
					m_assetsDir = e.abs;
					m_assetsSel.clear();
				}
				else if (isOpenable(e.abs)) {
					if (!m_cookRunning)
						OpenAnySceneFile(e.abs);
					else
						Log("Assets: a cook is running - wait for it "
							"to finish.");
				}
			}
			// Drag-and-drop sources (mesh -> viewport adds a scene
			// reference; material -> the selection assigns it).
			if (e.dragMesh || e.dragMaterial) {
				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
					const char* payloadType =
						e.dragMesh ? "VT_ASSET_MESH" : "VT_ASSET_MAT";

					ImGui::SetDragDropPayload(
						payloadType,
						e.abs.c_str(),
						e.abs.size() + 1,
						ImGuiCond_Once
					);

					ImGui::TextUnformatted(e.name.c_str());
					ImGui::Separator();

					ImGui::TextDisabled(
						e.dragMesh
						? "Drop on viewport to add scene reference"
						: "Drop on viewport to assign material"
					);

					ImGui::EndDragDropSource();
				}
			}
			if (hovered && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				const auto git = guidByAbs.find(e.abs);
				ImGui::SetTooltip("%s\n%s\n%s",
					e.name.c_str(), e.abs.c_str(),
					(git != guidByAbs.end() && !git->second.empty())
					? git->second.c_str() : "(no asset uuid)");
			}
			// Per-tile menu (right-click): delete through the same confirm
			// modal as the header button (selection is pinned first).
			if (ImGui::BeginPopupContextItem("##assettile_ctx")) {
				if (ImGui::MenuItem("Delete")) {
					m_assetsSel = e.abs;
					tileDeleteRequested = e.abs;
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
			ImGui::PopID();
		};
		for (const Entry& e : dirs) {
			if (col != 0)
				ImGui::SameLine(0.0f, 4.0f);
			tile(e);
			if (++col == cols)
				col = 0;
		}
		for (const Entry& e : files) {
			if (col != 0)
				ImGui::SameLine(0.0f, 4.0f);
			tile(e);
			if (++col == cols)
				col = 0;
		}
		if (dirs.empty() && files.empty())
			ImGui::TextDisabled("(empty folder)");
		ImGui::EndChild(); // ##assetgrid
		if (!tileDeleteRequested.empty())
			RequestAssetDelete(tileDeleteRequested);
		// The delete confirm modal (opened by RequestAssetDelete from the
		// header button or a tile menu).
		DrawAssetDeleteModal();
		ImGui::EndChild(); // ##assets_root (non-scrolling header)
	}

	void EditorApp::RequestAssetDelete(const std::string& absPath) {
		m_assetsDeletePending = AssetDeletePlan{};
		if (m_projectRoot.empty()) {
			Log("Delete: open a project first.");
			return;
		}
		if (absPath.empty())
			return;
		// Refuse the live scene's files (deleting the open scene from under
		// the editor desyncs the tour autosave).
		{
			namespace fs = std::filesystem;
			std::error_code ec;
			const std::string abs =
				fs::absolute(fs::path(absPath), ec)
				.lexically_normal().generic_string();
			for (const std::string& live :
				{ m_gltfPath, m_gltfHostPath }) {
				if (live.empty())
					continue;
				std::error_code lec;
				const std::string norm =
					fs::absolute(fs::path(live), lec)
					.lexically_normal().generic_string();
				if (!lec && abs == norm) {
					Log("Delete: '" + absPath
						+ "' is open — close the scene first.");
					return;
				}
			}
		}
		AssetDeletePlan plan;
		std::string err;
		if (!PlanAssetDelete(absPath, m_projectRoot, plan, err)) {
			Log("Delete: " + err + ".");
			return;
		}
		m_assetsDeletePending = std::move(plan);
		ImGui::OpenPopup("Delete asset?");
	}

	void EditorApp::DrawAssetDeleteModal() {
		if (m_assetsDeletePending.target.empty())
			return;
		if (!ImGui::BeginPopupModal("Delete asset?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize))
			return;
		const AssetDeletePlan& plan = m_assetsDeletePending;
		const std::string rel = ProjectRelativePath(plan.target);
		ImGui::Text("Delete '%s'?",
			(rel.empty() ? plan.target.c_str() : rel.c_str()));
		if (plan.isDir) {
			ImGui::TextDisabled("a folder with %zu entr%s (recursive)",
				plan.entryCount, plan.entryCount == 1 ? "y" : "ies");
		}
		if (!plan.also.empty()) {
			ImGui::TextDisabled("+ %zu more file(s) (sidecar/derived)",
				plan.also.size());
		}
		for (const std::string& note : plan.notes)
			ImGui::TextDisabled("%s", note.c_str());
		ImGui::TextDisabled("The GUID row stays missing so references "
			"report cleanly.");
		ImGui::Separator();
		if (ImGui::Button("Delete", ImVec2(120, 0))) {
			std::string err;
			if (ExecuteAssetDelete(plan, err)) {
				size_t n = plan.also.size() + 1;
				Log("Deleted '" + plan.target + "' (" + std::to_string(n)
					+ " file(s)).");
				// Heal the browser state (the deleted paths are gone).
				namespace fs = std::filesystem;
				auto norm = [](const std::string& p) {
					std::error_code ec;
					return fs::absolute(fs::path(p), ec)
						.lexically_normal().generic_string();
				};
				auto under = [&](const std::string& p) {
					const std::string n = norm(p);
					return n == plan.target
						|| (n.size() > plan.target.size()
							&& n.compare(0, plan.target.size(),
								plan.target) == 0
							&& n[plan.target.size()] == '/');
				};
				if (!m_assetsSel.empty() && under(m_assetsSel)) {
					m_assetsSel.clear();
				}
				if (!m_assetsDir.empty() && under(m_assetsDir)) {
					m_assetsDir.clear(); // healed to content/ next draw
				}
				if (!m_assetsExpandGltf.empty()
					&& under(m_assetsExpandGltf)) {
					m_assetsExpandGltf.clear();
					m_assetsSel.clear();
				}
				m_assetInspectPath.clear();
				m_assetInspectMat.reset();
				m_assetInspectMesh.reset();
				m_assetInspectGiCook.reset();
				m_assetInspectLmCook.reset();
			}
			else {
				Log("Delete failed: " + err);
			}
			m_assetsDeletePending = AssetDeletePlan{};
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			m_assetsDeletePending = AssetDeletePlan{};
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	// Fallback icon art (drawn vector shapes): used when an SVG icon failed
	// to rasterize/register (imgui == 0). Color-tinted per browser kind.
	void EditorApp::DrawAssetIconFallback(ImDrawList* dl, const ImVec2& org,
		float s, IconType k) const {
		const float x = org.x, y = org.y;
		switch (k) {
		case IconType::Folder: {
			const ImU32 body = IM_COL32(233, 178, 64, 255);
			const ImU32 edge = IM_COL32(150, 108, 20, 255);
			dl->AddRectFilled(ImVec2(x + s * .08f, y + s * .22f),
				ImVec2(x + s * .46f, y + s * .36f), body, 2.0f);
			dl->AddRectFilled(ImVec2(x + s * .08f, y + s * .30f),
				ImVec2(x + s * .92f, y + s * .80f), body, 4.0f);
			dl->AddRect(ImVec2(x + s * .08f, y + s * .30f),
				ImVec2(x + s * .92f, y + s * .80f), edge, 4.0f, 0, 1.5f);
			break;
		}
		case IconType::Model: {
			const ImU32 fill = IM_COL32(72, 164, 172, 255);
			const ImU32 edge = IM_COL32(22, 94, 102, 255);
			const ImVec2 hex[6] = {
				{ x + s * .50f, y + s * .14f },{ x + s * .86f, y + s * .34f },
				{ x + s * .86f, y + s * .68f },{ x + s * .50f, y + s * .88f },
				{ x + s * .14f, y + s * .68f },{ x + s * .14f, y + s * .34f } };
			dl->AddConvexPolyFilled(hex, 6, fill);
			dl->AddPolyline(hex, 6, edge, ImDrawFlags_Closed, 1.5f);
			const ImVec2 c{ x + s * .50f, y + s * .51f };
			dl->AddLine(c, hex[0], edge, 1.5f);
			dl->AddLine(c, hex[2], edge, 1.5f);
			dl->AddLine(c, hex[4], edge, 1.5f);
			break;
		}
		case IconType::Scene:
		case IconType::File: {
			const ImU32 fill = IM_COL32(238, 240, 243, 255);
			const ImU32 edge = IM_COL32(120, 128, 138, 255);
			dl->AddRectFilled(ImVec2(x + s * .26f, y + s * .12f),
				ImVec2(x + s * .74f, y + s * .88f), fill, 3.0f);
			dl->AddRect(ImVec2(x + s * .26f, y + s * .12f),
				ImVec2(x + s * .74f, y + s * .88f), edge, 3.0f, 0, 1.5f);
			for (int i = 0; i < 3; ++i)
				dl->AddRectFilled(
					ImVec2(x + s * .34f, y + s * (.30f + i * .14f)),
					ImVec2(x + s * .66f, y + s * (.36f + i * .14f)),
					IM_COL32(150, 156, 164, 255), 1.0f);
			break;
		}
		case IconType::Texture: {
			const ImU32 fill = IM_COL32(70, 96, 150, 255);
			const ImU32 edge = IM_COL32(36, 48, 82, 255);
			dl->AddRectFilled(ImVec2(x + s * .14f, y + s * .20f),
				ImVec2(x + s * .86f, y + s * .82f), fill, 3.0f);
			dl->AddRect(ImVec2(x + s * .14f, y + s * .20f),
				ImVec2(x + s * .86f, y + s * .82f), edge, 3.0f, 0, 1.5f);
			dl->AddCircleFilled(ImVec2(x + s * .32f, y + s * .36f),
				s * .07f, IM_COL32(255, 214, 90, 255), 12);
			dl->AddTriangleFilled(
				ImVec2(x + s * .20f, y + s * .74f),
				ImVec2(x + s * .44f, y + s * .44f),
				ImVec2(x + s * .62f, y + s * .74f),
				IM_COL32(120, 150, 96, 255));
			dl->AddTriangleFilled(
				ImVec2(x + s * .46f, y + s * .74f),
				ImVec2(x + s * .66f, y + s * .50f),
				ImVec2(x + s * .84f, y + s * .74f),
				IM_COL32(96, 128, 84, 255));
			break;
		}
		case IconType::Pak: {
			const ImU32 fill = IM_COL32(150, 108, 74, 255);
			const ImU32 edge = IM_COL32(92, 62, 38, 255);
			dl->AddRectFilled(ImVec2(x + s * .18f, y + s * .26f),
				ImVec2(x + s * .82f, y + s * .84f), fill, 3.0f);
			dl->AddRect(ImVec2(x + s * .18f, y + s * .26f),
				ImVec2(x + s * .82f, y + s * .84f), edge, 3.0f, 0, 1.5f);
			dl->AddLine(ImVec2(x + s * .18f, y + s * .55f),
				ImVec2(x + s * .82f, y + s * .55f), edge, 1.5f);
			dl->AddLine(ImVec2(x + s * .50f, y + s * .26f),
				ImVec2(x + s * .50f, y + s * .84f), edge, 1.5f);
			break;
		}
		default: {
			// The remaining node-type icons (light/camera/env/...):
			// a generic tinted page (they are not browser kinds).
			dl->AddRectFilled(ImVec2(x + s * .28f, y + s * .14f),
				ImVec2(x + s * .72f, y + s * .86f),
				IM_COL32(120, 120, 128, 200), 3.0f);
			break;
		}
		}
	}

} // namespace editor
