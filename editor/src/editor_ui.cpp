// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"
#include "modal_bindings.h"
#include "editor_attr_ui.h" // the attribute-widget stubs (attr::Bool/...)

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
#include <imgui_impl_vulkan.h> // ImGui_ImplVulkan_AddTexture (TextureThumb)
#include <stb_image.h>         // stbi_load (AssetSidecarThumb; impl in editor_branding)

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
			// LEFT COLUMN, STACKED (the layout rework's final form): the
			// world Outliner is ALWAYS VISIBLE on top (an authoring
			// constant — selection context at every glance), and the
			// tabbed browser panel (Assets / Console / Overrides) fills
			// the column below it. The horizontal splitter between them
			// (DrawPanelSplitters' ##splitOut) adjusts the split; the
			// draw-time clamp keeps both panels usable at any window
			// size. The browser wraps its header actions + tiles to the
			// host width (SameLineIfFits inside); the console + the
			// override rule table scroll in the narrow column — width
			// was the dock-era trade, the column layout takes it back.
			const float colH = vh - menuH - BottomDockH() - kEditorStatusH;
			const float outH = std::clamp(m_outlinerH, 160.0f,
				std::max(colH - 140.0f, 200.0f));
			ImGui::SetNextWindowPos(ImVec2(0, menuH), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(m_outlinerW, outH),
				ImGuiCond_Always);
			if (ImGui::Begin("Outliner", nullptr,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
				ImGuiWindowFlags_NoResize)) {
				DrawOutliner();
			}
			ImGui::End();
			ImGui::SetNextWindowPos(ImVec2(0, menuH + outH), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(m_outlinerW, colH - outH),
				ImGuiCond_Always);
			if (ImGui::Begin("Assets", nullptr,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
				ImGuiWindowFlags_NoResize)) {
				if (ImGui::BeginTabBar("##leftTabs")) {
					if (ImGui::BeginTabItem("Assets")) {
						m_leftTab = 0;
						DrawAssetsPanel();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Console")) {
						m_leftTab = 1;
						DrawConsolePanel();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Overrides")) {
						m_leftTab = 2;
						DrawOverridesPanel();
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}
			}
			ImGui::End();

			ImGui::SetNextWindowPos(
				ImVec2(float(ImGui::GetIO().DisplaySize.x) - m_inspectorW, menuH),
				ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(m_inspectorW, vh - menuH - BottomDockH() - kEditorStatusH),
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
					// (The UV tab is retired: the atlas/lightmap readouts
					// live in the Inspector's material page, the view-mode
					// toggles ride the F5/F8/F9/F10 shortcuts.)
					if (ImGui::BeginTabItem("Material")) {
						m_rightTab = 5;
						DrawMaterialEditor();
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}
			}
			ImGui::End();
			DrawStatusBar();
		} // m_uiMode (panel columns)

		DrawPanelSplitters(); // resize handles between panels and viewport
		DrawInsetTag();
		DrawViewportChrome(); // borders, camera chips, box-select rect
		// THE LIVE VIEW LIVES IN THE VIEW PANEL NOW (the center 4:3
		// stage): the capture + the LIVE badge + the unavailable hint.
		// The bottom dock is RETIRED — nothing hosts it anymore (the
		// Left-column tabs took the authoring panels, the world render
		// took the stage).
		DrawLiveViewStage();
		DrawVerbToast();
		// UV editor window retired (the Live view window replaced the
		// canvas era; solid backgrounds everywhere now — no transparent
		// content regions). See editor_viewports.cpp's DrawUvEditorWindow.
		//DrawUvEditorWindow(); // floating UV editor (View menu; canvas via GPU)
		DrawCookWindow(); // floating cook/bake tools (View menu)
		DrawSelectionRadial(); // Q radial: Mesh/Face/Vertex highlight modes
		DrawReferencesPanel(); // Maya-style reference list (View menu)
		DrawPathsPanel(); // consolidated file/path view (File menu)
		DrawGiVizWindow(); // GI-authoring legend + probe overlay controls
		// (The WYSIWYG live view is no floating window: it is the bottom
		// dock's "Live view" TAB — see the BottomDock block above.)
		if (m_placement.showGraph)
			DrawPlacementGraph();
		// Frame-time histogram (View menu / F12): a SEPARATE bottom strip —
		// full-width horizontal bucket graph pinned to the bottom of the
		// screen, drawn last so it sits on top of the panels/docks. Zero cost
		// while disabled: one flag check.
		if (smvk::profiler::FrameHistogramEnabled())
			smvk::profiler::DrawFrameHistogramUi();
		// THE MODAL CACHE (queried HERE — the imgui frame is ALIVE; the
		// live view's compose pass (recorded AFTER the imgui pass) reads
		// the cache — a direct query there tears down the frame (the boot
		// AV crash). Includes the GENERIC DIALOG (the manual modal isn't
		// a popup — IsPopupOpen can't see it; DialogUp() covers it).
		m_imguiModalOpen = DialogUp()
			|| ImGui::IsPopupOpen("Save tour as")
			|| ImGui::IsPopupOpen("Recover autosave")
			|| ImGui::IsPopupOpen("Create Terrain")
			|| ImGui::IsPopupOpen("Delete asset?")
			|| ImGui::IsPopupOpen("TaskProgress");
	}

	// ---- Maya-like bottom status strip (process progress) --------------
	// Always visible in UI mode: left = what is happening (task stage,
	// sticky result, else Ready), right = determinate progress while a
	// task reports fractions, else a reopen button for backgrounded
	// tasks. Derived per frame from live state — no stored message.
	// ---- THE VERB TOAST (the command feedback over the view panels' rect)
	// The verb's friendly name — minimal (the action's word, not a dialog).
	void EditorApp::ShowVerbToast(const EditCmd& c) {
		const char* v = "?";
		switch (c.verb) {
		case EditVerb::Translate: v = "move"; break;
		case EditVerb::Rotate: v = "rotate"; break;
		case EditVerb::Scale: v = "scale"; break;
		case EditVerb::Rename: v = "rename"; break;
		case EditVerb::Parent: v = "reparent"; break;
		case EditVerb::CreateLight: v = "create light"; break;
		case EditVerb::CreateTerrain: v = "create terrain"; break;
		case EditVerb::Delete: v = "delete"; break;
		case EditVerb::AssignMaterial: v = "assign material"; break;
		case EditVerb::SetMatAttr: v = c.attr.empty() ? "material"
				: c.attr.c_str(); break;
		case EditVerb::SetVisibility: v = "visibility"; break;
		case EditVerb::SetLightAttr: v = c.lightAttr.empty() ? "light attr"
				: c.lightAttr.c_str(); break;
		default: break;
		}
		m_verbToast = v;
		m_verbToastAge = 0.0f;
	}

	void EditorApp::DrawVerbToast() {
		if (m_verbToastAge < 0.0f)
			return;
		// ~1.6s lifetime, the last 0.4s fades out.
		constexpr float kToastLife = 1.6f;
		m_verbToastAge += ImGui::GetIO().DeltaTime;
		if (m_verbToastAge >= kToastLife) {
			m_verbToastAge = -1.0f;
			return;
		}
		const float alpha = m_verbToastAge < kToastLife - 0.4f
			? 1.0f
			: (kToastLife - m_verbToastAge) / 0.4f;
		// The chip: centered over the ACTIVE viewport's rect, near the top
		// (above the content, below the chrome).
		const ViewportRect& vp = m_viewports[m_activeVp].rect;
		ImDrawList* dl = ImGui::GetBackgroundDrawList();
		const ImVec2 ts = ImGui::CalcTextSize(m_verbToast.c_str());
		const ImVec2 c(vp.x + vp.w * 0.5f, vp.y + 26.0f);
		const ImVec2 bmin(c.x - ts.x * 0.5f - 10.0f, c.y - 4.0f);
		const ImVec2 bmax(c.x + ts.x * 0.5f + 10.0f, c.y + ts.y + 4.0f);
		dl->AddRectFilled(bmin, bmax,
			IM_COL32(18, 22, 30, int(200 * alpha)), 5.0f);
		dl->AddRect(bmin, bmax,
			IM_COL32(120, 200, 255, int(120 * alpha)), 5.0f);
		dl->AddText(ImVec2(bmin.x + 10.0f, bmin.y + 3.0f),
			IM_COL32(220, 235, 255, int(235 * alpha)),
			m_verbToast.c_str());
	}

	void EditorApp::DrawStatusBar() {
		const float vw = float(ImGui::GetIO().DisplaySize.x);
		const float vh = float(ImGui::GetIO().DisplaySize.y);
		// The bar's own padding: the theme's 10px WindowPadding plus a
		// 24px strip clipped the text into the window's bottom edge (the
		// "collides with the bottom" report) — a tight (6,3) padding and
		// one line of breathing room (kEditorStatusH 28) keeps the text
		// off the edges.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 3.0f));
		ImGui::SetNextWindowPos(ImVec2(0, vh - kEditorStatusH),
			ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(vw, kEditorStatusH),
			ImGuiCond_Always);
		if (!ImGui::Begin("##statusbar", nullptr,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
				ImGuiWindowFlags_NoScrollbar)) {
			ImGui::End();
			ImGui::PopStyleVar();
			return;
		}
		const bool running = m_cookRunning || m_bakeRunning;
		// THE MODE LEGEND (color-coded, always visible): which editor
		// mode is live + the F-keys that switch — the bottom bar's
		// persistent orientation row (the toolbar's number chips show
		// it too; this is the at-a-glance color code).
		{
			static const char* kModeNames[4] = { "1 LAYOUT", "2 LIGHTING",
				"3 TEST", "4 PROFILE" };
			// The mode color code: layout blue, lighting amber, test
			// green, profile violet (the chips + the legend agree).
			static const ImVec4 kModeColors[4] = {
				ImVec4(0.45f, 0.65f, 1.00f, 1.0f),
				ImVec4(1.00f, 0.75f, 0.30f, 1.0f),
				ImVec4(0.45f, 0.95f, 0.55f, 1.0f),
				ImVec4(0.80f, 0.60f, 1.00f, 1.0f)
			};
			const int mode = std::clamp(int(m_mode), 0, 3);
			ImGui::TextColored(kModeColors[mode], "%s",
				kModeNames[mode]);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Editor mode (keys 1-4 anywhere).\n"
					"Layout: build. Lighting: the look + GI authoring.\n"
					"Test: the run preview. Profile: capture + stats.");
			ImGui::SameLine(0.0f, 12.0f);
		}
		if (running) {
			ImGui::TextUnformatted(m_taskWindow.stage.c_str());
			ImGui::SameLine(vw - 260.0f);
			if (m_taskWindow.progress >= 0.0f) {
				ImGui::ProgressBar(m_taskWindow.progress,
					ImVec2(200.0f, 0.0f), "");
			}
			else {
				ImGui::ProgressBar(-1.0f, ImVec2(200.0f, 0.0f),
					"working...");
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Task")) {
				ReopenTaskWindow();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Bring the task window back.");
		}
		else if (m_taskWindow.done) {
			ImGui::TextColored(m_taskWindow.ok
				? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
				: ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
				"%s", m_taskWindow.result.c_str());
		}
		else {
			ImGui::TextDisabled("Ready");
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

	void EditorApp::DrawMenuBar() {
		if (ImGui::BeginMenu("File")) {
			// Project first (Unity/Unreal style): a project folder owns
			// content/, paks/, gi/ and its own .tourmaline/ cache. Imports
			// land in it and the editor shows them
			// pipeline — no manual staging.
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
			if (ImGui::MenuItem("Export .pak...", "Ctrl+Shift+C", false,
					m_world != nullptr)) {
				ExportPak();
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
				DraftCreateGiVolume();
			if (ImGui::MenuItem("Cubemap capture (pinned)", nullptr, false,
				canCreate))
				DraftCreateCubemap();
			if (ImGui::MenuItem("Camera", nullptr, false, canCreate))
				DraftCreateCamera();
			if (ImGui::MenuItem("Locator", nullptr, false, canCreate)) {
				// A named bare transform (no mesh/light/pool) — a game
				// code anchor: spawn points, trigger zones, patrol
				// markers. Tour: createNode -at transform; the runtime
				// finds it by NAME (findNodeByName).
				DraftCreateLocator();
			}
			if (ImGui::MenuItem("Terrain...", nullptr, false, canCreate)) {
				// Seed the dialog defaults (name, material) once per open.
				std::snprintf(m_terrainNameBuf, sizeof(m_terrainNameBuf), "%s",
					UniqueTerrainName().c_str());
				m_terrainMat = UINT32_MAX;
				m_showTerrainDialog = true;
			}
			if (ImGui::MenuItem("Placement graph", nullptr, false,
					canCreate)) {
				// (The toolbar's old "New graph" button — creation belongs
				// here, the bar keeps the live overlays/actions.)
				m_placement.pushUndo();
				m_placement.addGraph("");
				MarkPlacementDirty();
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
			// stable — hide those instead).
			if (ImGui::MenuItem("Delete selection", "Del", false,
				m_world && (m_selected != UINT32_MAX
					|| !m_selection.empty())))
				DeleteSelection();
			// Ctrl+D twin (modal registry 'Ctrl+D'): fresh Creates seeded
			// from the LIVE state of every selected duplicable node
			// (lights/cameras/cubemaps/GI volumes/transforms under the same
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
			// undoable — same contract as Maya's delete-history.
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
					"channel — the surviving state — so the tour replays "
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
				"F9..F11, P) — same view, same key, live view.");
			// Shading: the GI tri-view (the bake workflow's three looks),
			// ONE block — the old F1/F2 pair ("Unlit"/"Lit") duplicated
			// these (same ViewMode targets; the tri-view adds the ambient
			// split below). Unlit (albedo) = pure albedo; Fully Lit
			// renders the baked GI sources (world field + convex volumes
			// + cubemaps); Realtime Lit = the analytic look (direct sun +
			// sky IBL, no bake). The latter two both ride the Lit frame;
			// the difference is the ambient source (SetAmbientMode All vs
			// SkyIbl, driven every frame with the debug views).
			if (ImGui::MenuItem("Unlit (albedo)", "F2",
					ActiveViewMode() == ViewMode::Unlit))
				ActiveViewMode() = ViewMode::Unlit;
			if (ImGui::MenuItem("Fully Lit (with GI)", "F1",
					ActiveViewMode() == ViewMode::Lit && m_litView == 0)) {
				ActiveViewMode() = ViewMode::Lit;
				m_litView = 0;
			}
			if (ImGui::MenuItem("Realtime Lit", nullptr,
					ActiveViewMode() == ViewMode::Lit && m_litView == 1)) {
				ActiveViewMode() = ViewMode::Lit;
				m_litView = 1;
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
			// UV editor window retired (see the DrawUvEditorWindow note).
			//if (m_showUvEditor && ImGui::MenuItem("Reset UV window (zoom 1)")) {
			//	m_uvWinZoom = 1.0f;
			//	m_uvWinPan = glm::vec2(0.0f);
			//}
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
		if (ImGui::MenuItem("GI volume boxes", nullptr, m_showGiVolumeGizmos))
			m_showGiVolumeGizmos = !m_showGiVolumeGizmos;
		if (ImGui::MenuItem("Cubemap capture markers", nullptr,
			m_showCubemapGizmos))
			m_showCubemapGizmos = !m_showCubemapGizmos;
		// GI VISUALIZATION CLUSTER — ONE entry: the probe-viz window
		// owns every probe/density toggle (overlay on/off, octahedra
		// markers, rings, labels, density previews, sizes, distances).
		// The individual items used to live here too — that was the
		// duplicated-entries state; the window is the single control
		// surface for this cluster now.
		if (ImGui::MenuItem("GI probe viz...", nullptr,
			m_showGiVizWindow))
			m_showGiVizWindow = !m_showGiVizWindow;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Legend + controls: the probe overlay,\n"
				"octahedra markers, rings, labels, density previews,\n"
				"sizes/distances, and the probe counts (in level /\n"
				"in view / per octree level) — LIGHTING only.");
		// (The WYSIWYG live view needs NO toggle anymore: it IS the Lit
		// view mode (F1) — the active viewport in Lit renders through the
		// engine's full pipeline. Every other mode (F2 Unlit, F3..) is
		// the editor's own approximation pass, exactly as before.)
		if (ImGui::MenuItem("Camera frustums", nullptr,
				m_showCameraGizmos))
			m_showCameraGizmos = !m_showCameraGizmos;
		ImGui::Separator();
			{
				// Convex GI volume preview (LIGHTING 'J'): off/wire/
				// solid/probes (the probe mode draws the dense bake
				// lattice — coverage: green = baked nonzero, red =
				// zero/invalid).
				const char* cvNames[4] = { "Off", "Wireframe", "Solid",
					"Probes" };
				if (ImGui::BeginCombo("GI volumes (J)",
					cvNames[m_giVolumePreviewMode])) {
					for (int i = 0; i < 4; ++i)
						if (ImGui::Selectable(cvNames[i], m_giVolumePreviewMode == i))
							m_giVolumePreviewMode = i;
					ImGui::EndCombo();
				}
			}
			if (ImGui::MenuItem("Panels visible", nullptr, m_uiMode))
				m_uiMode = !m_uiMode;
			// UV editor window retired (see the DrawUvEditorWindow note).
			//if (ImGui::MenuItem("UV editor window", nullptr, m_showUvEditor))
			//	m_showUvEditor = !m_showUvEditor;
			if (ImGui::MenuItem("Cook & bake window", nullptr,
					m_showCookWindow))
				m_showCookWindow = !m_showCookWindow;
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
		// (The startup project dialog is RETIRED with the editor/launcher
		// split: EditorLauncher owns project selection; the editor boots
		// straight into the --project pick.)
	}

	// ---- tool topbar --------------------------------------------------------
	// Tab strip of tool types + one row of the active tab's controls. Returns
	// the toolbar's height (0 when the window fails to draw). The toolbar is
	// chrome: NoMove/NoBringToFront, fixed under the menu bar, hidden with
	// the panels (m_uiMode) so fullscreen viewports keep only the menu bar.
	float EditorApp::DrawToolTabs(float top) {
		// THE TOP TOOLBAR (the rework): ONE bar — every tool section
		// (Select / Transform / Modes / Terrain / Placement) is a GROUP of
		// letter buttons separated by verticals, no tabs. The letters are
		// PLACEHOLDER icons (the asset browser's SVG machinery will host
		// the real set later) — and they double as the hotkeys the legend
		// documents: the MAYA-STYLE dim strip under the buttons maps them
		// at a glance. MODES PERSIST: the section states save + restore
		// across sessions ([tools] prefs — sel/gizmo/mode).
		const float btnH = 26.0f;
		const float legendH = ImGui::GetTextLineHeightWithSpacing() + 5.0f;
		const float h = btnH + 10.0f + legendH;
		ImGui::SetNextWindowPos(ImVec2(0.0f, top), ImGuiCond_Always);
		ImGui::SetNextWindowSize(
			ImVec2(float(ImGui::GetIO().DisplaySize.x), h), ImGuiCond_Always);
		if (!ImGui::Begin("##toptoolbar", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav)) {
			ImGui::End();
			return 0.0f;
		}
		// The letter button: square, ACTIVE reads as the theme's selected
		// accent (the TabSelected blues) so the current tool group state
		// is the bar's loudest signal. Tooltip carries the words + the
		// hotkey the letter doubles as.
		auto letterTool = [&](const char* letter, bool active,
				const char* tip) {
			if (active) {
				ImGui::PushStyleColor(ImGuiCol_Button,
					ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
					ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,
					ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			const bool clicked = ImGui::Button(letter, ImVec2(0.0f, btnH));
			if (active)
				ImGui::PopStyleColor(3);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tip);
			return clicked;
		};
		// The vertical between groups (Maya's group dividers). (A dim
		// text pipe: SeparatorEx is not exposed by this ImGui build.)
		auto vsep = [&]() {
			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
		};

		// ---- SELECT: M mesh / F face / V vertex + frame + clear -------
		ImGui::PushID("secSelect");
		{
			static const char* kLetters[3] = { "M", "F", "V" };
			static const char* kTips[3] = {
				"Select MESH (Q: the radial wheel)",
				"Select FACE (Q: the radial wheel)",
				"Select VERTEX (Q: the radial wheel)" };
			for (int i = 0; i < 3; ++i) {
				if (i > 0)
					ImGui::SameLine();
				if (letterTool(kLetters[i], int(m_selMode) == i, kTips[i])
						&& int(m_selMode) != i) {
					m_selMode = SelMode(i);
					ClearSubSelection();
				}
			}
			ImGui::SameLine();
			if (letterTool("Frm", false, "Frame the selection (F)"))
				FrameSelection();
			ImGui::SameLine();
			if (letterTool("Clr", false, "Clear the selection"))
				ClearSelection();
		}
		ImGui::PopID();
		vsep();
		//// ---- TRANSFORM: W move / E rotate / R scale (the hotkeys ARE
		//// the letters — Maya's convention, the legend's anchor row) ------
		//ImGui::PushID("secXform");
		//{
		//	static const char* kLetters[3] = { "W", "E", "R" };
		//	static const char* kTips[3] = { "MOVE (W)",
		//		"ROTATE (E)", "SCALE (R)" };
		//	for (int i = 0; i < 3; ++i) {
		//		if (i > 0)
		//			ImGui::SameLine();
		//		if (letterTool(kLetters[i], int(m_gizmoMode) == i, kTips[i])
		//				&& int(m_gizmoMode) != i)
		//			m_gizmoMode = GizmoMode(i);
		//	}
		//	ImGui::SameLine();
		//	ImGui::TextDisabled("click gizmo axes to drag | sel: %s",
		//		(m_world && m_selected != UINT32_MAX)
		//		? getNodeName(m_world->scene, int(m_selected)).c_str()
		//		: "-");
		//}
		//ImGui::PopID();
		//vsep();
		//// ---- MODES: 1-4 (Layout/Lighting/Test/Profile; the numbers ARE
		//// the anywhere-hotkeys, the bar shows which one is live) ---------
		//ImGui::PushID("secModes");
		//{
		//	static const char* kLetters[4] = { "1", "2", "3", "4" };
		//	static const char* kTips[4] = { "LAYOUT mode (1)",
		//		"LIGHTING mode (2)", "TEST mode (3)", "PROFILE mode (4)" };
		//	for (int i = 0; i < 4; ++i) {
		//		if (i > 0)
		//			ImGui::SameLine();
		//		if (letterTool(kLetters[i], int(m_mode) == i, kTips[i])
		//				&& int(m_mode) != i)
		//			SetMode(EditorMode(i));
		//	}
		//}
		//ImGui::PopID();
		//vsep();
		//// ---- TERRAIN: the paint cluster (the brush controls live while
		//// a terrain group is selected; CREATION lives in the Create menu
		//// — the bar's job is the live tool state, not spawning) ---------
		//ImGui::PushID("secTerrain");
		//{
		//	const bool hasWorld = m_world != nullptr;
		//	const uint32_t group = hasWorld
		//		? TerrainGroupOf(m_selected) : UINT32_MAX;
		//	if (!hasWorld || group == UINT32_MAX)
		//		ImGui::TextDisabled("(select a terrain)");
		//	else {
		//		ImGui::SameLine();
		//		if (letterTool("P", m_paintTerrain,
		//				"Paint the selected terrain group (LMB in a 3D "
		//				"viewport)"))
		//			m_paintTerrain = !m_paintTerrain;
		//		static const char* kOps[4] = { "R", "L", "F", "S" };
		//		static const char* kOpTips[4] = { "RAISE brush", "LOWER brush",
		//			"FLATTEN brush", "SMOOTH brush" };
		//		for (int k = 0; k < 4; ++k) {
		//			ImGui::SameLine();
		//			if (letterTool(kOps[k], m_paintOp == k, kOpTips[k]))
		//				m_paintOp = k;
		//		}
		//		ImGui::SameLine();
		//		ImGui::SetNextItemWidth(90.0f);
		//		ImGui::DragFloat("R##terr", &m_paintRadius, 0.25f, 0.5f,
		//			256.0f, "%.1f m");
		//		if (ImGui::IsItemHovered())
		//			ImGui::SetTooltip("Brush radius (m)");
		//		ImGui::SameLine();
		//		ImGui::SetNextItemWidth(90.0f);
		//		ImGui::DragFloat("S##terr", &m_paintStrength, 0.01f, 0.01f,
		//			64.0f, "%.2f");
		//		if (ImGui::IsItemHovered())
		//			ImGui::SetTooltip("Brush strength");
		//	}
		//}
		//ImGui::PopID();
		//// (The PLACEMENT section is out of the bar: its toggles + refresh
		//// live in the placement graph window's own controls - the bar
		//// carries the authoring tools, not every subsystem's panel state.
		//// Creation was already in the Create menu.)
		//// ---- THE LEGEND (Maya-style): the letter/hotkey map under the
		//// buttons - everything the bar can do, one dim line.
		//ImGui::TextDisabled(
		//	"Q select | W move  E rotate  R scale | F frame | 1-4 modes"
		//	" | M/F/V mesh-face-vertex | P paint  R/L/F/S brush");
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
		// never scene nodes — they don't select, don't cook, and stay out
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
		// never part of it — no children, no drag&drop, no rename/delete.
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
				std::snprintf(label, sizeof(label), "%u: %s%s%s%s%s%s%s%s%s%s%s", n,
					name.empty() ? "(unnamed)" : name.c_str(),
					m_world->scene.meshForNode.contains(n) ? " [mesh]" : "",
					FindNodeLight(n) ? " [light]" : "",
					m_world->scene.giVolumeForNode.contains(n) ? " [giv]" : "",
					m_world->scene.cubemapForNode.contains(n) ? " [cube]" : "",
					m_world->scene.cameraForNode.contains(n) ? " [camera]" : "",
					m_world->scene.exportForNode.contains(n) ? " [export]" : "",
					IsEnvNode(n) ? " [env]" : "",
					(IsLocatorNode(m_world->scene, n) && !IsRefPlaceholder(n))
						? " [locator]" : "",
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
		std::snprintf(label, sizeof(label), "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
			name.empty() ? "(node)" : name.c_str(),
			hasMesh ? " [mesh]" : "", lightTag,
			scene.giVolumeForNode.contains(node) ? " [giv]" : "",
			scene.cubemapForNode.contains(node) ? " [cube]" : "",
			scene.cameraForNode.contains(node) ? " [camera]" : "",
			scene.exportForNode.contains(node) ? " [export]" : "",
			IsNoLightmapNode(scene, int(node)) ? " [noLM]" : "",
			IsLightmapNode(scene, int(node)) ? " [LM]" : "",
			IsDitherCloseupNode(scene, int(node)) ? " [dither]" : "",
			IsStaticSubtreeRoot(scene, int(node)) ? " [static]" : "",
			IsEnvNode(node) ? " [env]" : "",
			(IsLocatorNode(scene, node) && !IsRefPlaceholder(node))
				? " [locator]" : "",
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
			// for re-enabling) but the TREE is not shown — the nodes stay
			// in the scene (stable ids), just out of sight.
			if (m_disabledRefRoots.contains(node)) {
				ImGui::TextDisabled("    (reference disabled — %zu node(s) "
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
		// Breadcrumb: Back to node | the trail of visited materials.
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
		// THE SHARED BODY: the same material surface the Material tab and
		// the node page render (the consolidation — this page used to
		// carry its own old-presentation copy).
		DrawMaterialBody(mat);
	}

	void EditorApp::DrawGiVolumeSection(uint32_t node) {
		// Authored GI volume on this node (editor GI volume node):
		// box shape from the node's TRS (translate = center, scale = full
		// extents), bake params below. Undoable SetGiVolumeAttr edits persisted
		// as setGiVolumeAttr tour verbs so cooks replay them.
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		auto it = scene.giVolumeForNode.find(node);
		if (it == scene.giVolumeForNode.end())
			return;
		::SceneGiVolume& def = it->second;
		ImGui::Separator();
		ImGui::Text("Convex volume: '%s'",
			getNodeName(scene, int(node)).c_str());
		glm::vec3 mn, mx;
		if (GiVolumeBox(node, mn, mx))
			ImGui::TextDisabled("aabb (%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f) m",
				mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
		ImGui::TextDisabled("shape = node translate/scale/rotation (gizmo "
			"or transform; a rotated box — the bake rotates the planes)");
		auto beginGiVolumeDrag = [&](const char* attr) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetGiVolumeAttr ||
				m_pending.cmd.node != node || m_pending.cmd.giVolumeAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetGiVolumeAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.giVolumeAttr = attr;
				ReadNodeGiVolumeAttr(node, attr, m_pending.cmd.beforeVec4,
					m_pending.cmd.beforeNum, m_pending.cmd.beforeText);
			}
		};
		auto endGiVolumeDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.verb == EditVerb::SetGiVolumeAttr &&
				m_pending.cmd.node == node &&
				m_pending.cmd.giVolumeAttr == attr)
				FinishDrag();
		};
		int prio = int(def.priority);
		if (ImGui::DragInt("Priority (higher wins)", &prio, 0.2f, 0, 16)) {
			prio = std::clamp(prio, 0, 255);
			beginGiVolumeDrag("priority");
			WriteNodeGiVolumeAttr(node, "priority", glm::vec4(0), double(prio), {});
			m_pending.cmd.number = double(prio);
		}
		endGiVolumeDrag("priority");
		float spacing = def.spacing;
		if (ImGui::DragFloat("Probe spacing (m)", &spacing, 0.01f, 0.05f,
			4.0f, "%.2f")) {
			beginGiVolumeDrag("spacing");
			WriteNodeGiVolumeAttr(node, "spacing", glm::vec4(0), double(spacing),
				{});
			m_pending.cmd.number = double(spacing);
		}
		endGiVolumeDrag("spacing");
		float inside = def.insideFade;
		if (ImGui::DragFloat("Inside fade (m)", &inside, 0.01f, 0.0f,
			4.0f, "%.2f")) {
			beginGiVolumeDrag("inside");
			WriteNodeGiVolumeAttr(node, "inside", glm::vec4(0), double(inside),
				{});
			m_pending.cmd.number = double(inside);
		}
		endGiVolumeDrag("inside");
		float outside = def.outsideFade;
		if (ImGui::DragFloat("Outside fade (m)", &outside, 0.01f, 0.0f,
			4.0f, "%.2f")) {
			beginGiVolumeDrag("outside");
			WriteNodeGiVolumeAttr(node, "outside", glm::vec4(0), double(outside),
				{});
			m_pending.cmd.number = double(outside);
		}
		endGiVolumeDrag("outside");
		ImGui::TextDisabled("bake: a future gi_baker --tour pass harvests "
			"these (priority/spacing/fades) + the box");
	}

	void EditorApp::DrawExportSettingsSection(uint32_t node) {
		// The scene's exportSettings node: the WorldSettings-style EXPORT
		// CONFIG page (cook knobs). Every edit is an undoable SetExportAttr
		// command persisted as a setExportAttr tour line; Cook.exe/Pak.exe
		// read the replayed scene (CLI flags still win). outPak/contentDir
		// stay session-only in the Cook panel (never scene state).
		if (!m_world || node >= m_world->scene.hierarchy.size())
			return;
		Scene& scene = m_world->scene;
		auto it = scene.exportForNode.find(node);
		if (it == scene.exportForNode.end())
			return;
		ImGui::Separator();
		ImGui::Text("Export settings: '%s'",
			getNodeName(scene, int(node)).c_str());
		ImGui::TextDisabled("cook/export knobs (createNode -at "
			"exportSettings + setExportAttr; CLI flags still win)");

		auto beginDrag = [&](const char* attr) {
			if (m_pending.active
				&& (m_pending.cmd.verb != EditVerb::SetExportAttr
					|| m_pending.cmd.node != node
					|| m_pending.cmd.exportAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetExportAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.exportAttr = attr;
				ReadNodeExportAttr(node, attr, m_pending.cmd.beforeNum,
					m_pending.cmd.beforeFlag, m_pending.cmd.beforeText);
			}
		};
		auto endDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active
				&& m_pending.cmd.verb == EditVerb::SetExportAttr
				&& m_pending.cmd.node == node
				&& m_pending.cmd.exportAttr == attr)
				FinishDrag();
		};
		auto setBool = [&](const char* label, const char* attr, bool v,
			const char* tip) {
			bool val = v;
			if (ImGui::Checkbox(label, &val))
				PushExportAttr(node, attr, 0.0, val, "");
			if (tip && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tip);
		};
		auto dragInt = [&](const char* label, const char* attr, int v,
			float speed, int lo, int hi) {
			int val = v;
			if (ImGui::DragInt(label, &val, speed, lo, hi)) {
				val = std::clamp(val, lo, hi);
				beginDrag(attr);
				WriteNodeExportAttr(node, attr, double(val), false, {});
				m_pending.cmd.number = double(val);
			}
			endDrag(attr);
		};
		auto dragFloat = [&](const char* label, const char* attr, float v,
			float speed, float lo, float hi, const char* fmt) {
			float val = v;
			if (ImGui::DragFloat(label, &val, speed, lo, hi, fmt)) {
				val = std::clamp(val, lo, hi);
				beginDrag(attr);
				WriteNodeExportAttr(node, attr, double(val), false, {});
				m_pending.cmd.number = double(val);
			}
			endDrag(attr);
		};

		const SceneExportSettings& es = it->second;
		setBool("MeshOpt (meshoptimizer)", "meshopt", es.meshopt,
			"Reorder vertices/indices for GPU cache efficiency.");
		setBool("Generate LODs", "lods", es.lods,
			"Build mesh LOD chains via meshoptimizer.");
		if (es.lods) {
			ImGui::Indent();
			char ratios[64];
			std::snprintf(ratios, sizeof(ratios), "%s",
				es.lodRatios.c_str());
			ImGui::Text("LOD ratios (csv):");
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputText("##lodRatios", ratios, sizeof(ratios),
				ImGuiInputTextFlags_EnterReturnsTrue))
				PushExportAttr(node, "lodRatios", 0.0, false, ratios);
			dragFloat("LOD error", "lodError", es.lodError, 0.001f,
				0.0f, 1.0f, "%.3f");
			ImGui::Unindent();
		}
		setBool("Meshlets", "meshlets", es.meshlets,
			"Per-meshlet build over the cooked topology.");
		if (es.meshlets) {
			ImGui::Indent();
			dragInt("Max verts", "meshletMaxVerts",
				int(es.meshletMaxVerts), 1.0f, 3, 255);
			dragInt("Max tris", "meshletMaxTris",
				int(es.meshletMaxTris), 1.0f, 1, 512);
			dragFloat("Cone weight", "meshletConeWeight",
				es.meshletConeWeight, 0.01f, 0.0f, 1.0f, "%.2f");
			ImGui::Unindent();
		}
		setBool("Auto-convert textures (KTX2)", "autoKtx", es.autoKtx,
			"Non-KTX sources transcode in-process at cook.");
		setBool("Skip loose VFS bake files (ignoreVfs)", "ignoreVfs",
			es.ignoreVfs,
			"Embed ONLY what the scene itself carries: no loose\n"
			"gi/<stem>.sggi, no givolumemanifest/.givolume rows, no cubemap\n"
			"manifest/KTX rows at pak assembly (minimal variant pak).");

		setBool("Lightmaps (bake)", "lightmap", es.lightmap,
			"On-demand lightmap bake (Bake GI).");
		if (es.lightmap) {
			ImGui::Indent();
			dragInt("Voxel proxy resolution", "lmProxyRes",
				es.lmProxyRes, 1.0f, 0, 256);
			dragFloat("Meters per texel", "lmMetersPerTexel",
				es.lmMetersPerTexel, 0.01f, 0.01f, 1.0f, "%.3f");
			dragInt("Max SG lobes", "lmLobes", int(es.lmLobes), 1.0f, 5, 12);
			dragInt("Atlas width (px)", "lmAtlasWidth",
				int(es.lmAtlasWidth), 64.0f, 256, 8192);
			dragInt("Rays per texel", "lmRays", int(es.lmRays), 8.0f,
				64, 8192);
			ImGui::Unindent();
		}

		setBool("Cubemap captures", "cubemap", es.cubemap,
			"On-demand preconvolved specular capture set (Update cubemaps).");
		if (es.cubemap) {
			ImGui::Indent();
			dragInt("Size", "cubemapSize", int(es.cubemapSize), 32.0f,
				64, 2048);
			dragInt("Rays per texel", "cubemapRays",
				int(es.cubemapRays), 8.0f, 1, 4096);
			setBool("Octree placement (adaptive)", "cubemapOctree",
				es.cubemapOctree,
				"Sparse adaptive placement; growth stops at the\n"
				"32-probe runtime budget.");
			if (es.cubemapOctree) {
				ImGui::Indent();
				dragFloat("Max cell (m)", "cubemapOctreeMax",
					es.cubemapOctreeMax, 0.25f, 0.5f, 64.0f, "%.1f");
				dragFloat("Min cell (m)", "cubemapOctreeMin",
					es.cubemapOctreeMin, 0.1f, 0.125f,
					std::max(es.cubemapOctreeMax, 0.25f), "%.2f");
				dragInt("Depth cap", "cubemapOctreeDepth",
					int(es.cubemapOctreeDepth), 1.0f, 0, 8);
				dragFloat("Pinned influence (m)", "cubemapExtraRadius",
					es.cubemapExtraRadius, 0.25f, 0.5f, 50.0f, "%.1f");
				ImGui::Unindent();
			}
			ImGui::Unindent();
		}

		ImGui::Separator();
		ImGui::Text("GI bakes (on demand — Bake GI in the Cook panel)");
		setBool("GI volumes (bake authored)", "giVolumes", es.giVolumes,
			"Bake the authored GI volume nodes (gi/<stem>_*.givolume).");
		ImGui::TextDisabled("the world-field octree bake is retired — convex\n"
			"volumes + the sky IBL carry the ambient");
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
		ImGui::TextDisabled("pinned probe — wins weight ties at load");
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
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("UV1 (F10) shows baked texCoord2;\n"
							"F8 shows baked irradiance in place.");
				}
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
			// (The readouts — the live color/intensity, the aim axis, the
			// world position, the cone angles — ride the ATTRIBUTE rows'
			// hover hints now; the info text rows below the attrs are the
			// killed presentation.)
			if ((li->flags & ::SceneLight::kSceneLightFlag_Static) != 0
				&& ImGui::IsItemHovered())
				ImGui::SetTooltip("STATIC: bake-only (excluded from runtime)");
			// ---- editable light properties (point/spot): SetLightAttr,
			// undoable, persisted as setLightAttr tour verbs so cooks replay
			// them. Sliders coalesce one command per drag (mattr pattern);
			// chips commit immediately. Every row rides the attr stubs. ----
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
				{
					float col4[4] = { lcol.x, lcol.y, lcol.z, 1.0f };
					char hint[160];
					std::snprintf(hint, sizeof(hint),
						"Light color (live %.2f %.2f %.2f)", lcol.x, lcol.y, lcol.z);
					if (attr::Color("Color", col4, hint)) {
						beginLightDrag("color");
						WriteNodeLightAttr(node, "color",
							glm::vec4(col4[0], col4[1], col4[2], 1.0f), 0, false, {});
						m_pending.cmd.vec4 =
							glm::vec4(col4[0], col4[1], col4[2], 1.0f);
					}
					endLightDrag("color");
				}
				float lint = li->intensity;
				if (attr::Float("Intensity", &lint, 1.0f, 0.0f, 1e7f, "%.1f",
						isSpot ? "Spot intensity (cd), attenuated by range"
						: "Point intensity (cd), attenuated by range")) {
					beginLightDrag("intensity");
					WriteNodeLightAttr(node, "intensity", glm::vec4(0), lint, false, {});
					m_pending.cmd.number = lint;
				}
				endLightDrag("intensity");
				float lrange = li->range;
				if (attr::Float("Range", &lrange, 0.1f, 0.0f, 500.0f, "%.1f",
						"Attenuation range in m (0 = infinite)")) {
					beginLightDrag("range");
					WriteNodeLightAttr(node, "range", glm::vec4(0), lrange, false, {});
					m_pending.cmd.number = lrange;
				}
				endLightDrag("range");
				if (isSpot) {
					float inner = glm::degrees(li->innerConeAngle);
					if (attr::Slider("Inner cone", &inner, 0.0f, 89.0f, "%.1f",
							"Inner cone angle (deg) — full intensity inside")) {
						beginLightDrag("inner");
						WriteNodeLightAttr(node, "inner", glm::vec4(0), inner, false, {});
						m_pending.cmd.number = inner;
					}
					endLightDrag("inner");
					float outer = glm::degrees(li->outerConeAngle);
					if (attr::Slider("Outer cone", &outer, 0.0f, 89.0f, "%.1f",
							"Outer cone angle (deg) — the falloff edge")) {
						beginLightDrag("outer");
						WriteNodeLightAttr(node, "outer", glm::vec4(0), outer, false, {});
						m_pending.cmd.number = outer;
					}
					endLightDrag("outer");
				}
				bool lstatic = (li->flags & ::SceneLight::kSceneLightFlag_Static) != 0;
				if (attr::Bool("Static", lstatic,
						"STATIC — bake-only (excluded from the runtime "
						"Forward+ set)")) {
					EditCmd c{};
					c.verb = EditVerb::SetLightAttr;
					c.node = node;
					c.lightAttr = "static";
					c.afterFlag = lstatic ? 0 : 1;
					ReadNodeLightAttr(node, "static", c.beforeVec4, c.beforeNum,
						c.beforeFlag, c.beforeText);
					Do(c);
				}
				// Cast-shadow participation: spots get one perspective tile,
				// points get six cube-face tiles - both ride the local-light
				// shadow atlas (spotshadow.h).
				if (isSpot || li->type == ::SceneLight::Type::Point) {
					bool lshadow = (li->flags & ::SceneLight::kSceneLightFlag_CastShadow) != 0;
					if (attr::Bool("Shadows", lshadow,
							isSpot ? "Spot shadow map participation"
								: "Point shadow map participation (six cube-face tiles)")) {
						EditCmd c{};
						c.verb = EditVerb::SetLightAttr;
						c.node = node;
						c.lightAttr = "shadow";
						c.afterFlag = lshadow ? 0 : 1;
						ReadNodeLightAttr(node, "shadow", c.beforeVec4, c.beforeNum,
							c.beforeFlag, c.beforeText);
						Do(c);
					}
				}
				if (isSpot) {
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

		// Authored GI volume on this node (GI volume inspector).
		DrawGiVolumeSection(node);
		// Authored cubemap capture on this node (pinned probe inspector).
		DrawCubemapSection(node);
		// Authored camera on this node (lens inspector).
		DrawCamSection(node);
		// The scene's export config node (cook/export knobs).
		DrawExportSettingsSection(node);

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

		// Lightmap participation: opt-IN (default off) + legacy opt-out.
		// A mesh charts only when opted in here AND not excluded below.
		{
			ImGui::Separator();
			const bool optedIn = IsLightmapNode(scene, int(node));
			bool self = node < scene.lightmap.size() && scene.lightmap[node] != 0;
			if (ImGui::Checkbox("Bake lightmaps (subtree, opt-in)", &self)) {
				EditCmd c{};
				c.verb = EditVerb::SetLightmap;
				c.node = node;
				c.afterFlag = self;
				c.beforeFlag = !self;
				Do(c);
			}
			ImGui::TextDisabled("effective: %s (cook --lightmap charts it)",
				optedIn ? "opted in" : "skipped");
		}
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
			ImGui::TextDisabled("effective: %s (legacy opt-out; opt-in above still required)",
				inherited ? "excluded" : "not excluded");
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
				&m_paintTerrain)) {
				// (The old "surface the Terrain toolbar tab" hint went
				// with the tabs — the paint cluster is ALWAYS in the top
				// toolbar's Terrain section now.)
			}
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
		// definition — the block lives in the scene, the selection below
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
			// 2D material preview + THE SHARED BODY (the consolidation:
			// this block used to carry its own preview + scalar/flags
			// copy — the same surface the Material tab and the focus
			// page render, from ONE function).
			DrawMaterialBody(curMat);
		}
		// NOTE: the active-viewport orbit camera used to live here; it moved
		// to the World panel ("Viewport camera"). Node pages show authored
		// state only — camera nodes get the lens section above instead.
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
					case smvk::GiBinding::Kind::GiVolumeManifest: kindName = "givolumemanifest"; break;
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
	// (path + colorspace + sampler intent — the cooker reads them for KTX
	// conversion); fileNodes win on load.
	void EditorApp::DrawImagesTab() {
		if (ImGui::Button("Add image...")) {
			std::string picked;
			if (NativeOpenFile(picked, true) && !picked.empty())
				AddImageDef(picked);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%zu image(s) — outside the DAG, editor-only",
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
		// (Viewport legend removed — overlay window, menu item and chip
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

	void EditorApp::ExportPak() {
		if (!m_world) {
			Log("Export: no scene open.");
			return;
		}
		if (m_cookRunning || m_bakeRunning) {
			Log("Export: a cook/bake is already running.");
			return;
		}
		// The destination is chosen up front in the native Windows save
		// dialog (suggesting <scene>.pak in content/paks). The bake/export
		// knobs are NOT here: they live on the scene's exportSettings node
		// and the cook replays the tour to read them.
		std::string suggested =
			std::filesystem::path(m_gltfPath).stem().string();
		if (!suggested.empty())
			suggested += ".pak";
		std::string picked;
		if (!NativeSaveFile(picked, suggested) || picked.empty())
			return;
		m_cook.outPak = picked;
		std::snprintf(m_cookOutPakBuf, sizeof(m_cookOutPakBuf), "%s",
			picked.c_str());
		CookToPak();
	}

	void EditorApp::DrawCookWindow() {
		if (!m_showCookWindow)
			return;
		ImGui::SetNextWindowSize(ImVec2(470, 620), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Cook & bake", &m_showCookWindow))
			DrawCookPanel();
		ImGui::End();
	}

	// ---- WYSIWYG Live view — THE LIT VIEW MODE (editor_app.h decl) ------
	// NO special distinction anymore: the WYSIWYG render is simply what
	// the Lit mode (F1, the viewports' DEFAULT) shows. The ACTIVE
	// viewport in Lit renders through the engine's whole pipeline (the
	// world layer's inline target composited over its rect — the same
	// view the game gets); every other mode (F2 Unlit, F3.. the debug
	// views) and the non-active viewports render the editor's own
	// passes exactly as before. The GPU composite pass
	// (RecordLiveViewComposePass, editor_render.cpp) is recorded AFTER
	// the imgui pass and over-draws the captured rect — so nothing
	// ImGui-side can live inside it while the composite runs. This draws
	// only the rect capture (the ACTIVE viewport's rect, inset 3px so
	// the chrome border survives the composite's edge) + the
	// unavailable hint INSIDE the rect (visible only while there is no
	// composite to cover it — no LIVE badge, no toggle: a panel like the
	// others).
	void EditorApp::DrawLiveViewStage() {
		if (!LiveViewActive())
			return;
		const ViewportRect& stage = m_viewports[kLiveViewVp].rect;
		if (stage.w < 8.0f || stage.h < 8.0f)
			return;
		// The capture: THE FULL stage rect — NO INSET. The first cut
		// inset 3px to preserve the chrome border stroke, but the inline
		// target then rendered at (w-6)/(h-6) while the editor preview
		// renders w/h — a wider aspect at the same fovY, slightly more
		// horizontal world: "lit almost matches but is not identical to
		// unlit". The live view's extent and the preview's rect are now
		// THE SAME pixels → the same aspect → the identical camera. (The
		// chrome border's stroke straddles the rect edge; the composite
		// covers its inner half — the outer half still reads.)
		m_wysiwygLiveRectMin = ImVec2(stage.x, stage.y);
		m_wysiwygLiveRectMax =
			ImVec2(stage.x + stage.w, stage.y + stage.h);
		m_wysiwygLiveRectFresh = true;
		// The placeholder hint only reads while the renderer is
		// unavailable (attach failed / no scene): the composite paints
		// over the whole rect once it runs.
		if (!(m_wysiwyg && m_wysiwygValid)) {
			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			dl->AddText(
				ImVec2(stage.x + 14.0f, stage.y + 14.0f),
				IM_COL32(160, 160, 170, 255), "Lit: %s", m_world
				? "renderer unavailable (attach failed - see the console)"
				: "no scene loaded");
		}
	}
	// ---- GI probe viz window (View menu; editor_app.h decl) -----------
	// The GI-authoring legend + control center: every probe-overlay
	// switch in one place, the size/distance knobs, and the counts the
	// user reasons with while placing probes — "how many am I
	// authoring, here or anywhere in the level":
	//   - Cubemap probes: manifest rows in the LEVEL, authored pins,
	//     how many are in view, plus the camera-distance color legend.
	//   - .sggi lattice: total probes, then the per-OCTREE-LEVEL
	//     breakdown (probe count, effective cell size, lookup cells and
	//     max probes per cell — the chunk/cluster density answer), each
	//     row tagged with the level's ramp swatch.
	// Read-live like the overlays (the bake manifest + the .sggi
	// cache are queried per frame — the window can never disagree with
	// what the viewports draw). LIGHTING mode only: the overlays it
	// documents are LIGHTING-gated too.
	void EditorApp::DrawGiVizWindow() {
		if (!m_showGiVizWindow || m_mode != EditorMode::Lighting)
			return;
		ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("GI probe viz", &m_showGiVizWindow))
			return;
		// A 12px color swatch before a text line (the legend rows):
		// drawlist rect + a dummy advance so the swatch rides IN the
		// layout flow instead of fighting SameLine offsets.
		const auto legendSwatch = [](ImU32 col) {
			ImDrawList* wdl = ImGui::GetWindowDrawList();
			const ImVec2 p = ImGui::GetCursorScreenPos();
			wdl->AddRectFilled(p,
				ImVec2(p.x + 12.0f, p.y + 12.0f), col);
			ImGui::Dummy(ImVec2(16.0f, 14.0f));
			ImGui::SameLine();
		};

		// ---- marker switches ----
		// (The gizmo-family toggles — capture markers, light gizmos,
		// GI volume boxes — stay View-menu-only; this window owns the
		// probe/density cluster, no overlap.)
		ImGui::Checkbox("Cubemap probe overlay", &m_showProbeOverlay);
		ImGui::Checkbox("Octahedra markers (all probe sets)",
			&m_showProbeOctahedra);
		ImGui::Checkbox("Influence/fade rings", &m_probeRings);
		ImGui::Checkbox("C<idx>/P<idx> labels", &m_probeLabels);
		ImGui::SliderFloat("Marker size", &m_probeMarkerSize, 1.0f, 12.0f,
			"%.1f px");
		ImGui::SliderFloat("Probe overlay distance",
			&m_probeOverlayMaxDist, 5.0f, 300.0f, "%.0f m");

		// ---- density preview toggles (same flags as the GI panel) ----
		ImGui::Separator();
		ImGui::TextUnformatted("Bake density previews (no baker run)");
		if (ImGui::Checkbox("Cubemap density preview",
				&m_cubemapDensityOn)
			&& m_cubemapDensityOn && m_cubemapDensity.empty())
			RefreshCubemapDensity();

		// ---- legend: cubemap probes ----
		ImGui::Separator();
		ImGui::TextUnformatted("Cubemap probes (bake manifest)");
		std::vector<CubemapProbeRow> rows;
		CubemapProbeRowsFromFile(rows);
		ImGui::Text("In level: %zu    in view: %u (cap 64)",
			rows.size(), m_probeDrawnLast);
		if (m_world) {
			ImGui::Text("Authored pins: %zu node(s)",
				m_world->scene.cubemapForNode.size());
		}
		legendSwatch(IM_COL32(80, 255, 120, 230));
		ImGui::TextUnformatted("camera inside influence (full weight)");
		legendSwatch(IM_COL32(255, 220, 80, 230));
		ImGui::TextUnformatted("camera inside fade ring (blending)");
		legendSwatch(IM_COL32(120, 140, 160, 160));
		ImGui::TextUnformatted("beyond fade (probe idle for this view)");
		legendSwatch(IM_COL32(0, 0, 0, 255));
		ImGui::TextUnformatted(
			"authored pin, no baked probe behind it (black ball)");

		ImGui::End();
	}

	void EditorApp::DrawCookPanel() {
		ImGui::Text("Export the current scene + tour edits to a cooked .pak.");
		ImGui::Separator();

		// Labels ride their OWN lines and fields take the full width —
		// shared label/widget lines collide in narrow windows.
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

		// Bake/export knobs live on the scene's exportSettings NODE now
		// (authored in the Inspector — select it below; persisted as
		// createNode -at exportSettings + setExportAttr tour lines and
		// read by Cook.exe/Pak.exe off the replayed scene).
		ImGui::Separator();
		{
			const uint32_t esNode = FindExportSettingsNode();
			if (esNode != UINT32_MAX) {
				ImGui::Text("Export settings: node '%s' (#%u)",
					getNodeName(m_world->scene, int(esNode)).c_str(),
					esNode);
				if (ImGui::Button("Select ExportSettings node")) {
					SetSelection(esNode);
					m_rightTab = 0;
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Quality/GI knobs (meshopt, LODs,\n"
						"lightmaps, cubemaps, meshlets, ignoreVfs...) are\n"
						"edited on that node's Inspector page.");
			}
			else {
				ImGui::TextDisabled("No exportSettings node in this scene.");
				if (ImGui::Button("Create ExportSettings node"))
					EnsureExportSettingsNode();
			}
		}

		ImGui::Separator();
		ImGui::Text("GI (probes) - baked ON DEMAND, never with the cook");
		ImGui::TextDisabled("%u GI volume node(s) authored in this scene",
			m_world ? uint32_t(m_world->scene.giVolumeForNode.size()) : 0u);
		ImGui::TextDisabled("bake selectors (giVolumes) live on the "
			"ExportSettings node (the world-field selector is retired "
			"with the octree)");
		if (!m_giBakedHash.empty()) {
			const bool fresh = !m_cookDefHash.empty()
				&& m_giBakedHash == m_cookDefHash;
			ImGui::TextDisabled("GI baked (%s)", fresh ? "bakes fresh"
				: "may predate this cook — rebake to refresh");
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
				"Bake files land next to the cook's out pak; the live view\n"
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
		if (ImGui::Button("New .givolume")) {
			CreateGivolumeAsset(m_assetsDir,
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
		ImGui::Separator();
		ImGui::Text("Bake density (predictive overlays, no baker run)");
		if (ImGui::Checkbox("Cubemap density preview",
				&m_cubemapDensityOn)
			&& m_cubemapDensityOn && m_cubemapDensity.empty())
			RefreshCubemapDensity();
		ImGui::SameLine();
		if (ImGui::Button("Refresh prediction")) {
			RefreshCubemapDensity();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Rebuild the BVH + rerun placement from the\n"
				"cook dialog's cubemap octree knobs (8 m grid mirror\n"
				"when the octree is off). Manual like 'Update precook'.");
		ImGui::TextDisabled("octree max %.1f / min %.1f / depth %u%s",
			ExportCfg().cubemapOctreeMax, ExportCfg().cubemapOctreeMin,
			ExportCfg().cubemapOctreeDepth,
			ExportCfg().cubemapOctree ? "" : " (octree OFF: 8 m grid mirror)");
		ImGui::TextDisabled("%s", m_cubemapDensityInfo.empty()
			? "no prediction yet"
			: m_cubemapDensityInfo.c_str());
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
					"capture finishes. Loose bakes override the pak\n"
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
				CookToPak();
			}
			ImGui::SameLine();
			if (ImGui::Button("Update bakes (no pak)")) {
				m_cook.outPak = m_cookOutPakBuf;
				CookToPak(true);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Quick refresh, no pak write: GiBaker\n"
					"re-runs the GI bakes in place, then Cook\n"
					"--quick-bakes re-captures the cubemap set as\n"
					"loose bake files. The last pak stays untouched.");
		}
	}

	// ---- Material editor (right-dock tab) ---------------------------------
	// Texture-centric material authoring: pick a material, assign its PBR
	// texture slots from the cluster texture registry, manage layers, and add
	// textures as FILE PATH refs (KTX2 loads into the pool with the path as
	// the URI — cook re-embeds the stashed bytes). Deeper per-material pages
	// (emissive/alpha/flags/travel) live in the material focus page.
	// Texture thumbnail bindings (the material page's previews; see the
	// header block). The sampler is shared with the cubemap face strip
	// (created there once — the first user creates it).
	ImTextureID EditorApp::TextureThumb(const smvk::TextureHandle& h) {
		if (h.empty() || !m_context || !m_imgui)
			return 0;
		const uint64_t key = uint64_t(h.index())
			| (uint64_t(h.gen()) << 32);
		for (const auto& [k, id] : m_matThumbs)
			if (k == key)
				return id;
		const void* vkView = m_context->GetTextureVkImageView(h);
		if (!vkView)
			return 0;
		if (m_cubeFaceSampler == UINT32_MAX) {
			m_cubeFaceSampler = m_context->GetOrCreateSampler(
				smvk::SamplerCreateInfo{
					.magFilter = smvk::Filter::Linear,
					.minFilter = smvk::Filter::Linear,
					.mipmapMode = smvk::MipmapMode::Nearest,
					.addressU = smvk::AddressMode::ClampToEdge,
					.addressV = smvk::AddressMode::ClampToEdge,
					.addressW = smvk::AddressMode::ClampToEdge,
					.maxAnisotropy = 1.0f });
		}
		const void* vkSampler =
			m_context->GetVkSamplerOpaque(m_cubeFaceSampler);
		if (!vkSampler)
			return 0;
		VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
			VkSampler(const_cast<void*>(vkSampler)),
			VkImageView(const_cast<void*>(vkView)),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		if (set == VK_NULL_HANDLE)
			return 0;
		const ImTextureID id = ImTextureID(reinterpret_cast<ImU64>(set));
		m_matThumbs.emplace_back(key, id);
		return id;
	}

	void EditorApp::DropTextureThumbs() {
		if (m_matThumbs.empty())
			return;
		for (auto& [k, id] : m_matThumbs)
			if (id != 0)
				ImGui_ImplVulkan_RemoveTexture(
					reinterpret_cast<VkDescriptorSet>(ImU64(id)));
		m_matThumbs.clear();
	}

	// The asset browser's SIDECAR thumbnails (the .material ball previews):
	// the <file>.material.png the matball pass writes, loaded as an engine
	// texture + bound for ImGui (the cubeface pattern). Path + mtime keyed —
	// a refreshed sidecar (the Ball preview button re-rendered it) reloads,
	// a deleted sidecar drops the entry and the tile falls back to the
	// generic icon. stb_image (the loader half of the branding pair).
	ImTextureID EditorApp::AssetSidecarThumb(const std::string& pngPath) {
		namespace fs = std::filesystem;
		std::error_code ec;
		if (!m_context || !m_imgui || !fs::is_regular_file(pngPath, ec))
			return 0;
		const auto mt = fs::last_write_time(pngPath, ec);
		if (ec)
			return 0;
		auto it = std::find_if(m_assetThumbs.begin(), m_assetThumbs.end(),
			[&](const AssetThumb& t) { return t.path == pngPath; });
		if (it != m_assetThumbs.end()) {
			if (it->hasMtime && mt == it->mtime && it->imgui != 0)
				return it->imgui; // current
			// Stale (the sidecar re-rendered) or half-initialized: drop,
			// rebuild below.
			if (it->imgui != 0)
				ImGui_ImplVulkan_RemoveTexture(
					reinterpret_cast<VkDescriptorSet>(ImU64(it->imgui)));
			it->tex = {};
			m_assetThumbs.erase(it);
		}
		int w = 0, h = 0, comp = 0;
		stbi_uc* data = stbi_load(pngPath.c_str(), &w, &h, &comp, 4);
		if (!data || w <= 0 || h <= 0) {
			stbi_image_free(data);
			return 0;
		}
		smvk::ImageCreateInfo ci{};
		ci.extent = smvk::Extent3D{ uint32_t(w), uint32_t(h), 1 };
		ci.format = smvk::Format::RGBA8_SRGB; // the matball PNG is display-ready
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "asset sidecar thumb";
		AssetThumb t;
		t.tex = m_context->CreateTexture(ci, data);
		stbi_image_free(data);
		if (t.tex.empty())
			return 0;
		const void* vkView = m_context->GetTextureVkImageView(t.tex);
		if (vkView) {
			if (m_cubeFaceSampler == UINT32_MAX) {
				m_cubeFaceSampler = m_context->GetOrCreateSampler(
					smvk::SamplerCreateInfo{
						.magFilter = smvk::Filter::Linear,
						.minFilter = smvk::Filter::Linear,
						.mipmapMode = smvk::MipmapMode::Nearest,
						.addressU = smvk::AddressMode::ClampToEdge,
						.addressV = smvk::AddressMode::ClampToEdge,
						.addressW = smvk::AddressMode::ClampToEdge,
						.maxAnisotropy = 1.0f });
			}
			const void* vkSampler =
				m_context->GetVkSamplerOpaque(m_cubeFaceSampler);
			if (vkSampler) {
				VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
					VkSampler(const_cast<void*>(vkSampler)),
					VkImageView(const_cast<void*>(vkView)),
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				if (set != VK_NULL_HANDLE)
					t.imgui = ImTextureID(reinterpret_cast<ImU64>(set));
			}
		}
		if (t.imgui == 0)
			return 0;
		t.path = pngPath;
		t.mtime = mt;
		t.hasMtime = true;
		m_assetThumbs.push_back(std::move(t));
		return m_assetThumbs.back().imgui;
	}

	// ---- THE SHARED MATERIAL BODY (one surface, three hosts) ---------
	// Everything a material can author, in ONE function: the texture
	// slots (the project registry picker + thumbnails), the scalar
	// look (base/metal/rough/emissive, the shared-drag command path),
	// the attribute flags (chips) + alpha, the vertex-color layers, the
	// diffuse preview + the ball-preview sidecar button + the layer
	// travel. The hosts own ONLY their headers:
	//   - the Inspector's node block: the assignment combo + travel
	//   - the Inspector's focus page: the breadcrumb + trail
	//   - the right column's Material tab: the picker (auto-follow)
	// (Before the consolidation the three each carried a subset — the
	// tab had slots+layers but no scalars, the node page had scalars+
	// flags but no slots, the focus page had scalars+layers in the old
	// pre-revamp presentation. Now one body, the attr stubs everywhere.)
	void EditorApp::DrawMaterialBody(uint32_t mat) {
		smvk::Material* m = m_world ? m_world->GetMaterial(mat) : nullptr;
		if (!m) {
			ImGui::TextDisabled("Material resolve failed.");
			return;
		}
		smvk::Cluster* cl = m_world->clusters.empty()
			? nullptr : m_world->clusters[0].get();

		// ---- Preview + the ball sidecar --------------------------------
		ImGui::Separator();
		DrawDiffusePreview(mat, m->values.diffuse);
		ImGui::SameLine();
		ImGui::BeginGroup();
		{
			const TexPreview& pv = ResolveDiffusePreview(mat);
			if (pv.ok)
				ImGui::TextWrapped("map: %s", pv.uri.c_str());
			else
				ImGui::TextDisabled("map: %s", pv.status.c_str());
			if (ImGui::IsItemHovered()) {
				const glm::ivec4& t0 = m->values.textures;
				const glm::ivec4& t1 = m->values.textures2;
				ImGui::SetTooltip("slots: base %d | mr %d | normal %d "
					"| occ %d | emis %d\nbase %.2f %.2f %.2f a %.2f | "
					"metal %.2f rough %.2f | emis x %.2f",
					t0.x, t0.y, t0.z, t0.w, t1.x,
					m->values.diffuse.x, m->values.diffuse.y,
					m->values.diffuse.z, m->values.diffuse.w,
					m->values.params.y, m->values.params.z,
					m->values.params2.z);
			}
		}
		ImGui::EndGroup();
		ImGui::SameLine();
		{
			// The BALL PREVIEW sidecar: renders the material on a sphere
			// under the fixed default sky into a 256x256 PNG next to the
			// source .material asset (editor_matball.cpp).
			if (ImGui::SmallButton("Ball preview##body")) {
				const auto& loc = m_world->materialLocations[mat];
				std::string uri = loc.cluster
					? std::string(loc.cluster->GetMaterialUri(loc.handle).Str())
					: "";
				std::string sidecar;
				if (!uri.empty()) {
					namespace fs = std::filesystem;
					fs::path host = fs::path(ResolveVfsRoot()) / uri;
					if (uri.size() > 9
						&& host.extension().generic_string() == ".material")
						sidecar = host.generic_string() + ".png";
				}
				if (sidecar.empty())
					sidecar = "exports/matball_"
						+ std::to_string(mat) + ".png";
				QueueMaterialBallPreview(mat, sidecar);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Renders this material on a sphere "
					"under the default sky; the PNG lands next to the "
					"source .material asset (the browser's tile).");
		}

		// ---- PBR texture slots (the registry picker + thumbnails) -----
		ImGui::Separator();
		ImGui::Text("Texture slots");
		struct SlotDef { const char* label; const char* attr; int* slot; };
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
		struct TexAsset { std::string name; std::string abs; std::string guid; };
		std::vector<TexAsset> texAssets;
		if (m_assetDb.IsOpen()) {
			std::vector<smvk::cache::AssetRecord> rows;
			std::string derr;
			if (m_assetDb.All(rows, derr)) {
				texAssets.reserve(rows.size());
				for (const auto& r : rows) {
					if (r.kind != "texture")
						continue;
					const std::string abs = m_assetDb.ResolvePath(r.path);
					if (abs.empty())
						continue;
					TexAsset t;
					t.name = std::filesystem::path(abs)
						.filename().generic_string();
					t.abs = abs;
					t.guid = r.guid;
					texAssets.push_back(std::move(t));
				}
				std::sort(texAssets.begin(), texAssets.end(),
					[](const TexAsset& a, const TexAsset& b) {
						return a.name < b.name;
					});
			}
		}
		for (const SlotDef& s : slots) {
			ImGui::PushID(s.attr);
			std::string preview = *s.slot >= 0
				? ("tex." + std::to_string(*s.slot)) : std::string("(none)");
			smvk::TextureHandle slotTex{};
			if (*s.slot >= 0)
				for (const auto& [uri, th] : texList)
					if (int(th.index()) == *s.slot) {
						slotTex = th;
						preview = std::filesystem::path(uri.Str())
							.filename().generic_string();
						break;
					}
			if (!slotTex.empty()) {
				const ImTextureID tid = TextureThumb(slotTex);
				if (tid != 0) {
					ImGui::Image(tid, ImVec2(48, 48));
					ImGui::SameLine();
				}
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
				if (texAssets.empty())
					ImGui::TextDisabled("(no texture assets in the project"
						" — import or create them in the asset browser)");
				for (const TexAsset& ta : texAssets) {
					if (ImGui::Selectable(ta.name.c_str(),
							preview == ta.name)) {
						int poolIdx = -1;
						for (const auto& [uri, th] : texList)
							if (uri.Str() == ta.abs) {
								poolIdx = int(th.index());
								break;
							}
						if (poolIdx < 0)
							poolIdx = AddTextureFromFile(ta.abs);
						if (poolIdx < 0)
							continue;
						EditCmd c{};
						c.verb = EditVerb::SetMatAttr;
						c.matWorld = mat;
						c.attr = s.attr;
						c.number = double(poolIdx);
						ReadSharedMatAttr(mat, s.attr, c.beforeVec4,
							c.beforeNum);
						Do(c);
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s\n%s", ta.abs.c_str(),
							ta.guid.c_str());
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
		}

		// ---- The scalar look (shared edit, the drag command path) ----
		ImGui::Separator();
		{
			glm::vec4 bc = m->values.diffuse;
			if (attr::Color("Base color", &bc.x,
					("Shared edit — affects every node using mat."
						+ std::to_string(mat)).c_str())) {
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
				m_pending.cmd.attr == "baseColor")
				FinishDrag();
			float metallic = m->values.params.y;
			if (attr::Slider("Metallic", &metallic, 0.0f, 1.0f, "%.2f",
					"Metallic factor (0 dielectric .. 1 metal)")) {
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
				m_pending.cmd.attr == "metallic")
				FinishDrag();
			float rough = m->values.params.z;
			if (attr::Slider("Roughness", &rough, 0.04f, 1.0f, "%.2f",
					"Perceptual roughness (0.04 mirror .. 1 diffuse)")) {
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
				m_pending.cmd.attr == "roughness")
				FinishDrag();
			// ---- EMISSIVE COLOR + STRENGTH (HDR — the BLOOM link) ----
			// The strength is the HDR radiance multiplier, the SAME
			// scale the bloom threshold thresholds (pre-exposure HDR;
			// the sunlit default thr=400, the panel slider to 4000):
			// emissive * strength must CROSS the threshold for the glow
			// to bloom (the bloom pass prefilters at threshold/knee —
			// the exposure-relative scaling is the renderer's). The OLD
			// combined control rode the strength on ColorEdit4's A
			// slider — capped at 1.0, an emissive could NEVER cross a
			// 400-threshold: "emissive not considered in lit view" —
			// it read as a faint additive tint and never glowed.
			glm::vec4 em(m->values.emissiveFactor.x,
				m->values.emissiveFactor.y, m->values.emissiveFactor.z,
				1.0f);
			if (attr::Color("Emissive color", &em.x,
					"The emissive TINT (RGB); the intensity is the "
					"strength row")) {
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
				// The color edit PRESERVES the current strength: the
				// picker's own A slider (0..1) would clobber an HDR
				// strength back to a faint tint — the strength row owns
				// that field; write the picker's RGB with the live
				// strength as the w.
				WriteSharedMatAttr(mat, "emissive",
					glm::vec4(em.x, em.y, em.z,
						m->values.params2.z), 0.0);
				m_pending.cmd.vec4 = glm::vec4(em.x, em.y, em.z,
					m->values.params2.z);
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "emissive")
				FinishDrag();
			{
				float strength = m->values.params2.z;
				if (attr::Float("Emissive strength", &strength, 5.0f, 0.0f,
						4000.0f, "%.1f",
						"HDR radiance multiplier — same HDR scale as the "
						"bloom threshold: past it, the emissive BLOOMS "
						"(sunlit scenes read ~400; 1.0 is a faint tint)")) {
					if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
						m_pending.cmd.matWorld != mat
						|| m_pending.cmd.attr != "emissiveStrength"))
						FinalizePending();
					if (!m_pending.active) {
						m_pending.active = true;
						m_pending.cmd = EditCmd{};
						m_pending.cmd.verb = EditVerb::SetMatAttr;
						m_pending.cmd.matWorld = mat;
						m_pending.cmd.attr = "emissiveStrength";
						ReadSharedMatAttr(mat, "emissiveStrength",
							m_pending.cmd.beforeVec4,
							m_pending.cmd.beforeNum);
					}
					WriteSharedMatAttr(mat, "emissiveStrength",
						glm::vec4(0), strength);
					m_pending.cmd.number = strength;
				}
				if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active
					&& m_pending.cmd.attr == "emissiveStrength")
					FinishDrag();
			}
		}

		// ---- Attribute flags + alpha ------------------------------------
		ImGui::Separator();
		{
			ImGui::TextDisabled("flags");
			ImGui::SameLine();
			auto flagChip = [&](const char* label, bool on, const char* tip) {
				return attr::Bool(label, on, tip);
			};
			(void)flagChip;
			if (attr::Bool("DS", m->doubleSided,
					"DOUBLE SIDED — both faces shade")) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = mat;
				c.attr = "doubleSided";
				c.number = m->doubleSided ? 0.0 : 1.0;
				ReadSharedMatAttr(mat, "doubleSided",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			ImGui::SameLine();
			if (attr::Bool("Shadow", m->castsShadows,
					"CASTS SHADOWS — off excludes it from the shadow "
					"passes (decal-style)")) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = mat;
				c.attr = "castsShadows";
				c.number = m->castsShadows ? 0.0 : 1.0;
				ReadSharedMatAttr(mat, "castsShadows",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			ImGui::SameLine();
			int alpha = int(m->alphaMode);
			if (attr::Enum("Alpha", &alpha, "Opaque\0Mask (cutout)\0Blend\0",
					"Alpha: the blend/cutout mode (the cut value reads "
					"here once the Mask editor lands)")) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = mat;
				c.attr = "alphaMode";
				c.number = double(alpha);
				ReadSharedMatAttr(mat, "alphaMode",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
		}

		// ---- Layers (vertex-color blending) + the travel ---------------
		ImGui::Separator();
		ImGui::Text("Layers (vertex-color blending)");
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
					c.vec4 = glm::vec4(float(nl.x), float(nl.y),
						float(nl.z), float(nl.w));
					c.number = double(nl.w);
					Do(c);
				}
			}
			int layerCount = lm.w;
			if (attr::Int("Active layers", &layerCount, 0, 4,
					"RGBA vertex weights blend layers 1..3 over this "
					"material (layer 0); count 0 = the legacy plain tint")) {
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
				c.vec4 = glm::vec4(float(nl.x), float(nl.y),
					float(nl.z), float(nl.w));
				c.number = double(nl.w);
				Do(c);
			}
			// Travel: jump to a layer material (the trail keeps the way
			// back; the jump raises the Inspector's focus page).
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
	}

	void EditorApp::DrawMaterialEditor() {
		if (!m_world || m_world->materialLocations.empty()) {
			ImGui::TextDisabled("No materials (open a scene).");
			return;
		}
		// AUTO-FOLLOW THE SELECTION: the tab positions itself on the
		// CURRENT SELECTION's material — picking a node in the viewport
		// (or the outliner) lands its material here (the manual combo
		// still works between selections: the follow only fires when the
		// SELECTION's material CHANGED, not on every frame).
		{
			uint32_t selMat = UINT32_MAX;
			if (m_selected != UINT32_MAX) {
				auto mit = m_world->scene.materialForNode.find(m_selected);
				if (mit != m_world->scene.materialForNode.end())
					selMat = mit->second;
			}
			if (selMat != m_matEditorSelFromSel) {
				m_matEditorSelFromSel = selMat;
				if (selMat != UINT32_MAX
					&& selMat < m_world->materialLocations.size())
					m_matEditorSel = selMat;
			}
		}
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
		// THE SHARED BODY: slots + scalars + flags + layers + previews —
		// the same surface the node page and the focus page render.
		DrawMaterialBody(m_matEditorSel);
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
		const bool wantGiv = !wantTex && !wantMat && !wantMesh && !wantGi && !wantLm && !wantCm
			&& m_assetsSel.size() > 9
			&& m_assetsSel.compare(m_assetsSel.size() - 9, 9, ".givolume") == 0;
		if (!wantTex && !wantMat && !wantMesh && !wantGi && !wantLm && !wantCm && !wantGiv)
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
			m_assetInspectGivolume.reset();
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
			else if (wantGiv) {
				m_assetInspectGivolume.emplace();
				if (!smvk::cook::LoadGivolumeAsset(m_assetsSel,
					*m_assetInspectGivolume, err)) {
					Log("Assets: GI volume inspect failed: " + err);
					m_assetInspectGivolume.reset();
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
			ImGui::TextDisabled("convex volumes: %s   (the world-field "
				"flag is retired with the octree)",
				c.giVolumes ? "on" : "off");
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
		else if (m_assetInspectGivolume) {
			const smvk::cook::GivolumeAsset& c = *m_assetInspectGivolume;
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
				const bool stale = smvk::cook::GivolumeBlobStale(c);
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
		// The input hook (OnInputEvent) collects FileDrop batches;
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
			//// NOTE: .gicook/.lmcook are NOT listed here on purpose — they
			//// are an internal consequence of baking (minted/updated per
			//// scene by RecordBakeAssetsFromCook), never user-created.
			//if (ImGui::MenuItem("Cubemap Asset (.cubemap)")) {
			//	CreateCubemapAsset(m_assetsDir);
			//}
			//if (ImGui::MenuItem("GI Volume Asset (.givolume)")) {
			//	CreateGivolumeAsset(m_assetsDir);
			//}
			//// Texture asset (from a picked source image): the .texture
			//// import-settings record + identity (see CreateTextureAsset).
			//if (ImGui::MenuItem("Texture (from image)...")) {
			//	std::string picked;
			//	if (NativeOpenFile(picked, true) && !picked.empty())
			//		CreateTextureAsset(picked, m_assetsDir);
			//}
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
			// MULTI: the set deletes as one request when >1.
			SameLineIfFits("Delete");
			ImGui::BeginDisabled(m_assetsSel.empty());
			if (ImGui::SmallButton(
					m_assetsSelSet.size() > 1
						? "Delete (N)##sel" : "Delete##sel")) {
				if (m_assetsSelSet.size() > 1)
					RequestAssetDelete(m_assetsSelSet.data(),
						m_assetsSelSet.size());
				else
					RequestAssetDelete(&m_assetsSel, 1);
			}
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
		bool tileDeleteMulti = false; // the multi-set delete request
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
			if (ImGui::MenuItem("New GI Volume Asset (.givolume)"))
				CreateGivolumeAsset(m_assetsDir);
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
		// FRAME-ARENA STRINGS (the App's pattern — Memory/arena.h): the
		// per-frame entry storage rides smvk::FrameArena() (AllocateString
		// copies in, NUL-terminated, zero heap churn; the arena resets at
		// the frame boundary — MainLoop's ResetFrameArena). The views are
		// FRAME-SCOPED: everything that OUTLIVES the frame (the selection,
		// the expand state, the drag payload SINKS) copies out via Str().
		auto& fArena = smvk::FrameArena();
		auto put = [&](const std::string& s) {
			return fArena.AllocateString(smvk::StringViewT(s));
		};
		struct Entry {
			smvk::StringViewT name;
			smvk::StringViewT abs;
			bool dir = false;
			IconType icon{};
			bool gltf = false;  // .gltf with a manifest: presents as folder
			bool tour = false;  // .tour with bake refs: presents as folder
			bool back = false;  // the expanded view's ".." tile
			// Drag-drop payload kinds ("" = not draggable).
			bool dragMesh = false;
			bool dragMaterial = false;
			// >= 0: a VIRTUAL FOLDER tile — the canon collection view of
			// that filter index (see the kVFolders table below). Chrome,
			// not an asset: never selects, never deletes, never drags.
			int vfilter = -1;
		};
		std::vector<Entry> dirs, files;
		// ---- VIRTUAL FOLDERS (the canon collection views) ---------------
		// The header filter combo's views present as FOLDER TILES in the
		// grid (the Unity project-window feel): each enters its collection
		// on double-click; the active view's ".." leaves back to All. The
		// tile ids ride the "vfolder://<n>" scheme — never a real path
		// (plus the per-tile numeric uid below makes every tile id unique
		// regardless — the duplicate-abs conflict fix).
		{
			struct VFolderDef { const char* label; int filter; IconType icon; };
			static const VFolderDef kVFolders[8] = {
				{ "Meshes", 1, IconType::Model },
				{ "Materials", 2, IconType::Texture },
				{ "Animations", 3, IconType::Animation },
				{ "GI bakes", 4, IconType::GI },
				{ "Cubemaps", 5, IconType::Env },
				{ "GI cooks", 6, IconType::GI },
				{ "Lightmap cooks", 7, IconType::GI },
				{ "GI volumes", 8, IconType::GI },
			};
			if (m_assetsFilter != 0) {
				// The active collection view's leave tile (".." -> All).
				// (The expanded glTF/tour views build their own back tiles
				// in their branches below — no second one here.)
				Entry back{};
				back.name = "..";
				back.abs = put("vfolder://up");
				back.dir = true;
				back.back = true;
				back.icon = IconType::Folder;
				dirs.push_back(back);
			}
			else if (m_assetsExpandGltf.empty()
				&& m_assetsExpandTour.empty()) {
				// The root view: the canon folders up front (pinned
				// ahead of the real directories by the stable sort
				// below — the Unity feel: the collection roots are the
				// affordance, the real folders follow).
				for (const VFolderDef& v : kVFolders) {
					Entry t{};
					t.name = put(v.label);
					t.abs = put("vfolder://" + std::to_string(v.filter));
					t.dir = true;
					t.vfilter = v.filter;
					t.icon = v.icon;
					dirs.push_back(t);
				}
			}
		}
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
				t.name = put(e.name.empty() ? e.file : e.name);
				t.abs = put((fs::path(base) / e.file).generic_string());
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
					: (m_assetsFilter == 7) ? "lmcook" : "givolume";
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
					t.name = put(fs::path(abs).filename().generic_string());
					t.abs = put(abs);
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
			back.abs = put(expandGltf);
			back.dir = true; // plain-directory styling
			back.back = true; // double-click = leave the folder
			back.icon = IconType::Folder;
			dirs.push_back(back);
			manifestChildren(expandGltf, -1, files);
			Entry src{};
			src.name = put(fs::path(expandGltf).filename().string());
			src.abs = put(expandGltf);
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
			back.abs = put(expandTour);
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
				t.name = put(fs::path(git->second).filename().generic_string());
				t.abs = put(git->second);
				t.icon = (t.name.size() > 13
					&& t.name.Str().compare(t.name.size() - 13, 13,
						".cubemanifest") == 0)
					? IconType::Env : IconType::GI;
				files.push_back(std::move(t));
			}
			// The source itself: a plain openable .tour tile (double-click
			// opens the scene — see the tile handler's isOpenable branch).
			Entry src{};
			src.name = put(fs::path(expandTour).filename().string());
			src.abs = put(expandTour);
			src.icon = IconTypeFor(src.name.Str(), false);
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
					put(name),
					put(p.generic_string()),
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
			std::string la = a.name.Str(), lb = b.name.Str();
			for (char& c : la)
				c = char(std::tolower((unsigned char)c));
			for (char& c : lb)
				c = char(std::tolower((unsigned char)c));
			return la < lb;
		};
		// The canon VIRTUAL FOLDERS stay PINNED to the front of the dirs
		// row (pre-sort push order preserved: a stable partition — the
		// vfolders first in their authored order, the real directories
		// byName behind them). The Unity feel: the collection roots are
		// the affordance, the real folders follow.
		const auto vfolderFirst = [](const Entry& a, const Entry& b) {
			if ((a.vfilter >= 0) != (b.vfilter >= 0))
				return a.vfilter >= 0;
			if (a.vfilter >= 0 && b.vfilter >= 0)
				return a.vfilter < b.vfilter;
			return false;
		};
		std::stable_sort(dirs.begin(), dirs.end(),
			[&](const Entry& a, const Entry& b) {
				if (vfolderFirst(a, b))
					return true;
				if (vfolderFirst(b, a))
					return false;
				return byName(a, b);
			});
		std::sort(files.begin(), files.end(), byName);

		const float tileW = 96.0f, tileH = 104.0f, iconS = 56.0f;
		int cols = std::max(1,
			int(ImGui::GetContentRegionAvail().x / tileW));
		int col = 0;
		// THE PER-TILE UID (the id-conflict fix): every tile pushes a
		// unique numeric seed before its abs id — a duplicate abs in one
		// view (the same file listed twice through different views, or
		// any future collision) can no longer collide ids (the red debug
		// flash the filters showed).
		int tileSeq = 0;
		auto tile = [&](const Entry& e, int uid) {
			ImGui::PushID(uid);
			ImGui::PushID(e.abs.data());
			ImGui::PushID(e.back ? 1 : 0);
			// The SET highlight (the multi-selection); the primary
			// (m_assetsSel) is redundant inside the set on plain clicks.
			// NAVIGATION CHROME NEVER HIGHLIGHTS: the ".." back tile
			// carries the SAME abs as the expanded view's SOURCE tile
			// (both = the glTF path) — selecting the source lit the
			// ".." too ("the .. up directory gets highlighted"); the
			// VIRTUAL FOLDERS are chrome the same way.
			const bool chrome = e.back || e.vfilter >= 0;
			const bool sel = !chrome
				&& (std::find(m_assetsSelSet.begin(),
					m_assetsSelSet.end(), e.abs.Str())
					!= m_assetsSelSet.end()
					|| m_assetsSel == e.abs.Str());
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
			// THE .material SIDECAR PREVIEW (the matball pass's PNG): the
			// ball-under-default-sky thumbnail replaces the generic icon
			// when the sidecar exists (the Material tab's Ball preview
			// button renders it). Missing/stale = the generic icon (the
			// button regenerates on demand).
			ImTextureID sidecar = 0;
			if (!e.dir && e.name.size() > 9
				&& e.name.Str().compare(e.name.size() - 9, 9, ".material") == 0)
				sidecar = AssetSidecarThumb(e.abs.Str() + ".png");
			if (sidecar != 0) {
				dl->AddImage(sidecar, ico,
					ImVec2(ico.x + iconS, ico.y + iconS));
			}
			else {
				const IconTex& icon = m_icons[size_t(e.icon)];
				if (icon.imgui != 0) {
					dl->AddImage(icon.imgui, ico,
						ImVec2(ico.x + iconS, ico.y + iconS));
				}
				else {
					DrawAssetIconFallback(dl, ico, iconS, e.icon);
				}
			}
			// Registered-asset badge (green dot, bottom-right of icon).
			if (!e.dir && guidByAbs.contains(e.abs.Str())) {
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
				std::string l1 = e.name.Str(), l2;
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
					l1 = e.name.Str().substr(0, cut);
					l2 = e.name.Str().substr(cut);
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
			// ---- SELECTION: single / ctrl-toggle / shift-range ----------
			// The SET is the browser's selection (m_assetsSel stays the
			// PRIMARY for the single-flow consumers: drags, the header
			// buttons). A plain click replaces the set; ctrl toggles the
			// entry; shift selects the anchor..entry RANGE over the
			// display order (dirs then files — the vectors in scope).
			// NAVIGATION TILES NEVER JOIN: the virtual-folder chrome (the
			// ".." back tile, the canon VIRTUAL FOLDERS) is a move, not
			// an asset — chrome entering the set made ranges span it +
			// the delete target the folder's PARENT.
			if (clicked && e.vfilter < 0) {
				const bool ctrl = ImGui::GetIO().KeyCtrl;
				const bool shift = ImGui::GetIO().KeyShift;
				m_assetsSel = e.abs.Str();
				if (e.back) {
					// Navigation-only: the primary follows (the legacy
					// single-selection behavior), the SET never grows.
				}
				else if (ctrl) {
					// Toggle; the anchor moves (the next shift ranges
					// from HERE).
				auto it = std::find(m_assetsSelSet.begin(),
					m_assetsSelSet.end(), e.abs.Str());
				if (it != m_assetsSelSet.end())
						m_assetsSelSet.erase(it);
					else
						m_assetsSelSet.push_back(e.abs.Str());
					m_assetsAnchor = e.abs.Str();
				}
				else if (shift && !m_assetsAnchor.empty()) {
					// The anchor..entry range over the display order.
					const auto span = [&](const Entry& en) {
						return en.abs.Str() == e.abs.Str() || en.abs.Str() == m_assetsAnchor;
					};
					auto collectRange = [&](const std::vector<Entry>& v) {
						auto first = std::find_if(v.begin(), v.end(), span);
						if (first == v.end())
							return;
						auto last = std::find_if(v.rbegin(), v.rend(),
							span).base();
						for (auto it = first; it != last; ++it)
							if (!(it->back || it->vfilter >= 0)
								// chrome never ranges in
								&& std::find(m_assetsSelSet.begin(),
										m_assetsSelSet.end(), it->abs.Str())
									== m_assetsSelSet.end())
								m_assetsSelSet.push_back(it->abs.Str());
					};
					// Clear the non-range picks? Shift-select ADDS the
					// range to the set (the ctrl-picks survive — the
					// standard additive model).
					collectRange(dirs);
					collectRange(files);
				}
				else {
					m_assetsSelSet = { e.abs.Str() };
					m_assetsAnchor = e.abs.Str();
				}
			}
			if (dbl) {
				if (e.vfilter >= 0) {
					// ENTER THE CANON COLLECTION VIEW (the virtual
					// folder): the filter combo and the tile are the same
					// knob.
					m_assetsFilter = e.vfilter;
					m_assetsSel.clear();
					m_assetsSelSet.clear();
					m_assetsAnchor.clear();
				}
				else {
				m_assetsSel = e.abs.Str();
				m_assetsSelSet = { e.abs.Str() };
				m_assetsAnchor = e.abs.Str();
				if (e.back) {
					// Leave the expanded glTF/tour folder OR the canon
					// collection view (".." -> All).
					m_assetsExpandGltf.clear();
					m_assetsExpandTour.clear();
					m_assetsFilter = 0;
					m_assetsSel.clear();
				}
				else if (e.gltf) {
					// A glTF with a manifest: open the FOLDER (the derived
					// assets) — never the scene (the .tour references the
					// .mesh asset; the glTF is the container/source).
					m_assetsExpandGltf = e.abs.Str();
					m_assetsSel.clear();
				}
				else if (e.tour) {
					// A .tour with bake refs: open the FOLDER (its bake
					// assets). The tour itself stays openable INSIDE (the
					// source tile's isOpenable branch below) and via the
					// header Open button.
					m_assetsExpandTour = e.abs.Str();
					m_assetsSel.clear();
				}
				else if (e.dir) {
					m_assetsDir = e.abs.Str();
					m_assetsSel.clear();
				}
				else if (isOpenable(e.abs.Str())) {
					if (!m_cookRunning)
						OpenAnySceneFile(e.abs.Str());
					else
						Log("Assets: a cook is running - wait for it "
							"to finish.");
				}
				}
			}
			// Drag-and-drop sources (mesh -> viewport adds a scene
			// reference; material -> the selection assigns it).
			// (The canon VIRTUAL FOLDERS never drag — chrome.)
			if ((e.dragMesh || e.dragMaterial) && e.vfilter < 0) {
				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
					const char* payloadType =
						e.dragMesh ? "VT_ASSET_MESH" : "VT_ASSET_MAT";

					ImGui::SetDragDropPayload(
						payloadType,
						e.abs.data(),
						e.abs.size() + 1,
						ImGuiCond_Once
					);

					ImGui::TextUnformatted(e.name.data());
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
				if (e.vfilter >= 0) {
					ImGui::SetTooltip("VIRTUAL FOLDER — the canon "
						"collection view: every asset of this kind "
						"across the project's content tree.\n"
						"Double-click to enter; \"..\" leaves.");
				}
				else {
					const auto git = guidByAbs.find(e.abs.Str());
					ImGui::SetTooltip("%s\n%s\n%s",
						e.name.data(), e.abs.data(),
						(git != guidByAbs.end() && !git->second.empty())
						? git->second.c_str() : "(no asset uuid)");
				}
			}
			// Per-tile menu (right-click): delete through the same confirm
			// modal as the header button (selection is pinned first).
			// (The canon VIRTUAL FOLDERS never delete — chrome.)
			if (e.vfilter < 0
				&& ImGui::BeginPopupContextItem("##assettile_ctx")) {
				// MULTI-AWARE: deleting a tile that sits in a multi-
				// selection (>1) requests the WHOLE set (one confirm,
				// every plan executes); a single/unselected tile keeps
				// the single flow.
				const bool inSet = std::find(m_assetsSelSet.begin(),
					m_assetsSelSet.end(), e.abs.Str())
					!= m_assetsSelSet.end();
				if (ImGui::MenuItem(m_assetsSelSet.size() > 1 && inSet
						? "Delete (selection)" : "Delete")) {
					m_assetsSel = e.abs.Str();
					if (inSet && m_assetsSelSet.size() > 1)
						tileDeleteMulti = true;
					else
						tileDeleteRequested = e.abs.Str();
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
		};
		for (const Entry& e : dirs) {
			if (col != 0)
				ImGui::SameLine(0.0f, 4.0f);
			tile(e, tileSeq++);
			if (++col == cols)
				col = 0;
		}
		for (const Entry& e : files) {
			if (col != 0)
				ImGui::SameLine(0.0f, 4.0f);
			tile(e, tileSeq++);
			if (++col == cols)
				col = 0;
		}
		if (dirs.empty() && files.empty())
			ImGui::TextDisabled("(empty folder)");
		ImGui::EndChild(); // ##assetgrid
		// MULTIDELETE on the Delete KEY while the grid holds the focus
		// (the key + the context menu are the two delete gestures).
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)
			&& ImGui::IsKeyPressed(ImGuiKey_Delete)) {
			if (m_assetsSelSet.size() > 1)
				tileDeleteMulti = true;
			else if (!m_assetsSel.empty())
				tileDeleteRequested = m_assetsSel;
		}
		// THE DEFERRED OPEN: the request records the plans + the flag
		// from the tile-menu / key paths (deeper or mid-close scopes);
		// the OPEN itself lives INSIDE DrawAssetDeleteModal, on the line
		// directly above the BeginPopupModal — the same function, the
		// same id stack BY CONSTRUCTION (no intervening statement can
		// ever shift the stack again).
		if (tileDeleteMulti && !m_assetsSelSet.empty())
			RequestAssetDelete(m_assetsSelSet.data(), m_assetsSelSet.size());
		else if (!tileDeleteRequested.empty())
			RequestAssetDelete(&tileDeleteRequested, 1);
		DrawAssetDeleteModal();
		ImGui::EndChild(); // ##assets_root (non-scrolling header)
	}

	// THE delete request — ONE method, targets* + targetCount: the
	// single delete is count 1; the multi passes the selection set. Each
	// target validates (project containment, not the open scene,
	// planable); the plans ride m_assetsDeletePending (a vector; a
	// refused target logs + drops, the rest proceed) + the ONE confirm
	// modal opens.
	void EditorApp::RequestAssetDelete(const std::string* targets,
		size_t targetCount) {
		// TMP-DIAG: the request trace (the delete-flow hunt — which
		// gesture reached here, with what). Remove when stable.
		{
			std::string list;
			for (size_t i = 0; targets && i < targetCount && i < 6; ++i)
				list += (i ? ", '" : "'") + targets[i] + "'";
			if (targetCount > 6)
				list += " ...";
			Log("Delete request: " + std::to_string(targetCount)
				+ " target(s)" + (list.empty() ? "" : ": " + list));
		}
		m_assetsDeletePending.clear();
		if (m_projectRoot.empty()) {
			Log("Delete: open a project first.");
			return;
		}
		if (!targets || targetCount == 0)
			return;
		// The live-scene set (refused targets): deleting the open scene
		// from under the editor desyncs the tour autosave — refuse those
		// per-target, keep the rest.
		auto isLiveScene = [&](const std::string& absPath) {
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
				if (!lec && abs == norm)
					return true;
			}
			return false;
		};
		size_t refused = 0;
		for (size_t i = 0; i < targetCount; ++i) {
			const std::string& absPath = targets[i];
			if (absPath.empty())
				continue;
			if (isLiveScene(absPath)) {
				Log("Delete: '" + absPath + "' is open — close the "
					"scene first.");
				++refused;
				continue;
			}
			AssetDeletePlan plan;
			std::string err;
			if (!PlanAssetDelete(absPath, m_projectRoot, plan, err)) {
				Log("Delete: " + err + ".");
				++refused;
				continue;
			}
			m_assetsDeletePending.push_back(std::move(plan));
		}
		// TMP-DIAG (the outcome half of the request trace): ONE line
		// that tells the story — the refusals print their reasons above,
		// this makes the tally unmissable.
		Log("Delete request outcome: "
			+ std::to_string(m_assetsDeletePending.size()) + " planned, "
			+ std::to_string(refused) + " refused"
			+ (m_assetsDeletePending.empty()
				? " — nothing to confirm (see the refusal reasons above)"
				: " — confirm in the dialog"));
		if (!m_assetsDeletePending.empty()) {
			m_assetsDeletePopupWanted = true;
		}
	}

	// ---- THE GENERIC DIALOG (the ONE modal mechanism — see the header
	// block): a plain centered window + a dim overlay, gated on a bool —
	// no popup stack anywhere in the flow. One dialog at a time.
	void EditorApp::DialogOpen() {
		m_dialogUp = true;
	}
	void EditorApp::DialogClose() {
		m_dialogUp = false;
	}
	bool EditorApp::DialogBegin(const char* title, const ImVec2& size) {
		if (!m_dialogUp)
			return false;
		// The dim behind (a full-viewport transparent window): the modal
		// read + it swallows clicks (the borderless frame's
		// InvisibleButton covers the screen).
		{
			const ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
			ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
			ImGui::SetNextWindowSize(vp, ImGuiCond_Always);
			ImGui::PushStyleColor(ImGuiCol_WindowBg,
				ImVec4(0.0f, 0.0f, 0.0f, 0.45f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::Begin("##genericdialogdim", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoBringToFrontOnFocus);
			ImGui::InvisibleButton("##block", vp);
			ImGui::End();
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
		}
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing,
			ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
		if (!ImGui::Begin(title, nullptr,
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::End();
			return false;
		}
		return true;
	}
	void EditorApp::DialogEnd() {
		ImGui::End();
	}

	void EditorApp::DrawAssetDeleteModal() {
		// Stale dialog without plans: drop it.
		if (m_assetsDeletePending.empty()) {
			if (DialogUp())
				DialogClose();
			return;
		}
		// The request sets the flag one frame (the menu-close frame);
		// this raises the GENERIC DIALOG (the one modal mechanism).
		if (m_assetsDeletePopupWanted) {
			m_assetsDeletePopupWanted = false;
			DialogOpen();
		}
		if (!DialogBegin("Delete asset?", ImVec2(460, 0)))
			return;
		// Count-aware display: ONE plan reads exactly like the old
		// single; N plans list the names (capped, the count carries the
		// rest) with each plan's also/notes folded into its row.
		const size_t count = m_assetsDeletePending.size();
		if (count == 1) {
			const AssetDeletePlan& plan = m_assetsDeletePending[0];
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
		}
		else {
			ImGui::Text("Delete %zu assets?", count);
			size_t shown = 0;
			for (const AssetDeletePlan& plan : m_assetsDeletePending) {
				if (shown++ >= 8) {
					ImGui::TextDisabled("  ... (%zu more)",
						count - 8);
					break;
				}
				const std::string rel = ProjectRelativePath(plan.target);
				ImGui::BulletText("%s%s",
					(rel.empty() ? plan.target.c_str() : rel.c_str()),
					plan.isDir ? " (folder, recursive)" : "");
				if (!plan.also.empty())
					ImGui::TextDisabled("    + %zu sidecar/derived",
						plan.also.size());
			}
		}
		ImGui::TextDisabled("The GUID row stays missing so references "
			"report cleanly.");
		ImGui::Separator();
		if (ImGui::Button("Delete", ImVec2(120, 0))) {
			size_t okCount = 0, fileCount = 0;
			for (const AssetDeletePlan& plan : m_assetsDeletePending) {
				std::string err;
				if (!ExecuteAssetDelete(plan, err)) {
					Log("Delete failed ('" + plan.target + "'): " + err);
					continue;
				}
				++okCount;
				fileCount += plan.also.size() + 1;
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
				if (!m_assetsSel.empty() && under(m_assetsSel))
					m_assetsSel.clear();
				if (!m_assetsDir.empty() && under(m_assetsDir))
					m_assetsDir.clear(); // healed to content/ next draw
				if (!m_assetsExpandGltf.empty()
					&& under(m_assetsExpandGltf)) {
					m_assetsExpandGltf.clear();
					m_assetsSel.clear();
				}
			}
			// The set heals wholesale: drop every entry under any target.
			m_assetsSelSet.erase(std::remove_if(m_assetsSelSet.begin(),
				m_assetsSelSet.end(), [&](const std::string& p) {
					for (const AssetDeletePlan& plan
						: m_assetsDeletePending) {
						if (p == plan.target)
							return true;
					}
					return false;
				}), m_assetsSelSet.end());
			m_assetInspectPath.clear();
			m_assetInspectMat.reset();
			m_assetInspectMesh.reset();
			m_assetInspectGiCook.reset();
			m_assetInspectLmCook.reset();
			if (okCount)
				Log("Deleted " + std::to_string(okCount) + " asset(s) ("
					+ std::to_string(fileCount) + " file(s)).");
		m_assetsDeletePending.clear();
		DialogClose();
		DialogEnd();
		return; // the confirm pass ends here (no Cancel row this frame)
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			m_assetsDeletePending.clear();
			DialogClose();
		}
		DialogEnd();
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
