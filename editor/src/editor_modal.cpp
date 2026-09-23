// vi-style modal input: the ACTIVE mode's keymap interprets keys.
//
// Architecture:
//   - One GLOBAL table (mode switches 1-4, shading F1-F3, window toggles,
//     undo/redo/save) + one table per mode (LAYOUT / LIGHTING / TEST /
//     PROFILE). Bindings are exact key+modifier matches pointing at tiny
//     EditorApp handlers, so a key can mean different things per mode
//     (like vi's normal/insert) and the tables stay the single source of
//     truth for what a key does.
//   - The tables are function-local statics inside UpdateModalInput (a
//     member function): they address PRIVATE handlers, so they must live
//     in member scope. Built once, read every frame.
//   - UpdateModalInput() (called from RenderFrame, before DrawUI) walks
//     the ACTIVE mode's table first (modes may shadow globals), then the
//     global table. A key fires at most once per frame.
//   - Keys are skipped while ImGui wants the keyboard (typing into a
//     field) or while the Q radial popup is open (it owns 1/2/3).
//   - PROFILE is a stub: the mode + dispatch + OnModeEnter/OnModeExit
//     hooks exist and one placeholder binding proves the path; capture /
//     frame-stats bindings land in kProfile later.
//
// Adding a binding: write a tiny handler below, declare it in
// editor_app.h next to the others, add the row to a table.

#include "editor_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <Engine/App/input.h>

namespace editor {
namespace {

template <size_t N>
constexpr size_t CountOf(const ModalBinding (&arr)[N]) { return N; }

bool ModsMatch(uint8_t mods, bool ctrl, bool shift, bool alt) {
	return ((mods & kModalCtrl) != 0) == ctrl &&
		((mods & kModalShift) != 0) == shift &&
		((mods & kModalAlt) != 0) == alt;
}

} // namespace

void EditorApp::SetMode(EditorMode m) {
	if (m == m_mode)
		return;
	OnModeExit(m_mode);
	m_mode = m;
	Log(std::string("Mode: ") + EditorModeName(m) +
		" (1 LAYOUT / 2 LIGHTING / 3 TEST / 4 PROFILE)");
	OnModeEnter(m);
}

void EditorApp::OnModeEnter(EditorMode m) {
	// PROFILE wiring point: start the Optick capture / show the frame
	// stats overlay from here when that work lands.
	if (m == EditorMode::Profile)
		Log("PROFILE mode is a stub: bindings + capture come later.");
}

void EditorApp::OnModeExit(EditorMode) {
	// PROFILE wiring point: stop the capture when leaving Profile.
}

void EditorApp::UpdateModalInput() {
	// Keys belong to ImGui while typing into a field, and to the Q
	// radial popup while it is open (its 1/2/3 must not switch modes).
	if (m_uiMode && m_imgui && m_imgui->WantsCaptureKeyboard())
		return;
	if (m_selRadialOpen)
		return;
	const bool ctrl = m_input.isKeyHeld(GLFW_KEY_LEFT_CONTROL) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_CONTROL);
	const bool shift = m_input.isKeyHeld(GLFW_KEY_LEFT_SHIFT) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_SHIFT);
	const bool alt = m_input.isKeyHeld(GLFW_KEY_LEFT_ALT) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_ALT);

	// ---- keymap tables (see the file header) --------------------------
	static const ModalBinding kGlobal[] = {
		// Mode switches (1-4): reachable from ANY mode, vi-style. The
		// shading trio + window toggles + document ops stay global so
		// they work like vi's ctrl-keys in every mode.
		{ GLFW_KEY_1, 0, &EditorApp::ModalModeLayout, "mode: LAYOUT" },
		{ GLFW_KEY_2, 0, &EditorApp::ModalModeLighting, "mode: LIGHTING" },
		{ GLFW_KEY_3, 0, &EditorApp::ModalModeTest, "mode: TEST" },
		{ GLFW_KEY_4, 0, &EditorApp::ModalModeProfile, "mode: PROFILE" },
		{ GLFW_KEY_F1, 0, &EditorApp::ModalViewLit, "view: lit" },
		{ GLFW_KEY_F2, 0, &EditorApp::ModalViewUnlit, "view: unlit" },
		{ GLFW_KEY_F3, 0, &EditorApp::ModalViewLitShadows,
			"view: lit+shadows" },
		{ GLFW_KEY_V, 0, &EditorApp::ModalVpLayoutToggle,
			"viewport: single/quad" },
		{ GLFW_KEY_I, 0, &EditorApp::ModalUiToggle, "ui: panels on/off" },
		{ GLFW_KEY_Z, kModalCtrl, &EditorApp::ModalUndo, "undo" },
		{ GLFW_KEY_Z, kModalCtrl | kModalShift, &EditorApp::ModalRedo,
			"redo" },
		{ GLFW_KEY_Y, kModalCtrl, &EditorApp::ModalRedo, "redo" },
		{ GLFW_KEY_S, kModalCtrl | kModalAlt,
			&EditorApp::ModalSaveIncremental, "tour: save incremental" },
	};
	// LAYOUT: the "normal" mode — object-level editing (gizmos,
	// selection, framing, wireframe, UV0 check).
	static const ModalBinding kLayout[] = {
		{ GLFW_KEY_W, 0, &EditorApp::ModalGizmoTranslate,
			"gizmo: translate" },
		{ GLFW_KEY_E, 0, &EditorApp::ModalGizmoRotate, "gizmo: rotate" },
		{ GLFW_KEY_R, 0, &EditorApp::ModalGizmoScale, "gizmo: scale" },
		{ GLFW_KEY_G, 0, &EditorApp::ModalGizmoCycle, "gizmo: cycle" },
		{ GLFW_KEY_Q, 0, &EditorApp::ModalSelRadial,
			"select: radial (Mesh/Face/Vertex)" },
		{ GLFW_KEY_Z, 0, &EditorApp::ModalWireframeToggle,
			"view: wireframe all" },
		{ GLFW_KEY_F, 0, &EditorApp::ModalFrameSel,
			"frame selection (flat pane: reset UV nav)" },
		{ GLFW_KEY_F, kModalShift, &EditorApp::ModalFrameAll,
			"frame all" },
		{ GLFW_KEY_UP, 0, &EditorApp::ModalCycleNodePrev,
			"selection: previous node" },
		{ GLFW_KEY_DOWN, 0, &EditorApp::ModalCycleNodeNext,
			"selection: next node" },
		{ GLFW_KEY_F9, 0, &EditorApp::ModalViewUv0, "view: UV0" },
	};
	// LIGHTING: sun/env + lightmap authoring. Lights are nodes, so the
	// object-manipulation core (gizmo + framing) stays usable here (like
	// vi's hjkl surviving into visual mode); on top of that: lightmap
	// views, light gizmos and sun nudges on CurrentEnv() (editor IBL —
	// viewport-only, never cooked).
	static const ModalBinding kLighting[] = {
		{ GLFW_KEY_W, 0, &EditorApp::ModalGizmoTranslate,
			"gizmo: translate" },
		{ GLFW_KEY_E, 0, &EditorApp::ModalGizmoRotate, "gizmo: rotate" },
		{ GLFW_KEY_R, 0, &EditorApp::ModalGizmoScale, "gizmo: scale" },
		{ GLFW_KEY_G, 0, &EditorApp::ModalGizmoCycle, "gizmo: cycle" },
		{ GLFW_KEY_F, 0, &EditorApp::ModalFrameSel,
			"frame selection (flat pane: reset UV nav)" },
		{ GLFW_KEY_F, kModalShift, &EditorApp::ModalFrameAll,
			"frame all" },
		{ GLFW_KEY_L, 0, &EditorApp::ModalLightGizmosToggle,
			"light gizmos" },
		{ GLFW_KEY_F8, 0, &EditorApp::ModalViewBaked,
			"view: baked irradiance" },
		{ GLFW_KEY_F10, 0, &EditorApp::ModalViewLmUv,
			"view: lightmap UV" },
		{ GLFW_KEY_F5, 0, &EditorApp::ModalUvInsetCycle,
			"UV inset: cycle" },
		{ GLFW_KEY_LEFT_BRACKET, 0, &EditorApp::ModalSunAzimuthNeg,
			"sun azimuth -5" },
		{ GLFW_KEY_RIGHT_BRACKET, 0, &EditorApp::ModalSunAzimuthPos,
			"sun azimuth +5" },
		{ GLFW_KEY_MINUS, 0, &EditorApp::ModalSunElevationNeg,
			"sun elevation -2" },
		{ GLFW_KEY_EQUAL, 0, &EditorApp::ModalSunElevationPos,
			"sun elevation +2" },
		{ GLFW_KEY_COMMA, 0, &EditorApp::ModalSunIntensityNeg,
			"sun intensity /1.25" },
		{ GLFW_KEY_PERIOD, 0, &EditorApp::ModalSunIntensityPos,
			"sun intensity *1.25" },
	};
	// TEST: verify (cook / run in App / debug views / tex residency).
	static const ModalBinding kTest[] = {
		{ GLFW_KEY_P, 0, &EditorApp::ModalRunInApp, "run in App" },
		{ GLFW_KEY_C, 0, &EditorApp::ModalCook, "cook to .pak" },
		{ GLFW_KEY_T, 0, &EditorApp::ModalTexModeCycle,
			"textures: residency mode" },
		{ GLFW_KEY_F4, 0, &EditorApp::ModalViewMeshlets,
			"view: meshlets" },
		{ GLFW_KEY_F6, 0, &EditorApp::ModalViewVertexColors,
			"view: vertex colors" },
		{ GLFW_KEY_F9, 0, &EditorApp::ModalViewUv0, "view: UV0" },
	};
	// PROFILE: perf capture (STUB). Architecture only for now: the table
	// + dispatch + OnModeEnter hook prove the path; Optick capture /
	// frame-stat bindings land here later.
	static const ModalBinding kProfile[] = {
		{ GLFW_KEY_O, 0, &EditorApp::ModalProfileStub,
			"profile: placeholder" },
	};

	const ModalBinding* modeTable = nullptr;
	size_t modeCount = 0;
	switch (m_mode) {
	case EditorMode::Layout:
		modeTable = kLayout;
		modeCount = CountOf(kLayout);
		break;
	case EditorMode::Lighting:
		modeTable = kLighting;
		modeCount = CountOf(kLighting);
		break;
	case EditorMode::Test:
		modeTable = kTest;
		modeCount = CountOf(kTest);
		break;
	case EditorMode::Profile:
		modeTable = kProfile;
		modeCount = CountOf(kProfile);
		break;
	}

	// A key fires at most once per frame (even if two tables list it).
	bool consumed[Input::KeyCount] = {};
	const auto dispatch = [&](const ModalBinding* table, size_t count) {
		for (size_t i = 0; i < count; ++i) {
			const ModalBinding& b = table[i];
			if (!b.run || consumed[b.key] || !m_input.isKeyPressed(b.key))
				continue;
			if (ModsMatch(b.mods, ctrl, shift, alt)) {
				consumed[b.key] = true;
				(this->*b.run)();
			}
		}
	};
	if (modeTable)
		dispatch(modeTable, modeCount); // mode table first (shadows)
	dispatch(kGlobal, CountOf(kGlobal)); // global fallback
}

// ---- mode switches ----

void EditorApp::ModalModeLayout() { SetMode(EditorMode::Layout); }
void EditorApp::ModalModeLighting() { SetMode(EditorMode::Lighting); }
void EditorApp::ModalModeTest() { SetMode(EditorMode::Test); }
void EditorApp::ModalModeProfile() { SetMode(EditorMode::Profile); }

// ---- global handlers ----

void EditorApp::ModalViewLit() { ActiveViewMode() = ViewMode::Lit; }
void EditorApp::ModalViewUnlit() { ActiveViewMode() = ViewMode::Unlit; }
void EditorApp::ModalViewLitShadows() {
	ActiveViewMode() = ViewMode::LitShadows;
}

void EditorApp::ModalVpLayoutToggle() { m_vpLayout = (m_vpLayout == 0) ? 1 : 0; }
void EditorApp::ModalUiToggle() { m_uiMode = !m_uiMode; }

void EditorApp::ModalUndo() { Undo(); }
void EditorApp::ModalRedo() { Redo(); }
void EditorApp::ModalSaveIncremental() { SaveTourIncremental(); }

// ---- layout handlers ----

void EditorApp::ModalGizmoTranslate() { m_gizmoMode = GizmoMode::Translate; }
void EditorApp::ModalGizmoRotate() { m_gizmoMode = GizmoMode::Rotate; }
void EditorApp::ModalGizmoScale() { m_gizmoMode = GizmoMode::Scale; }
void EditorApp::ModalGizmoCycle() {
	m_gizmoMode = GizmoMode((int(m_gizmoMode) + 1) % 3);
}

void EditorApp::ModalSelRadial() { m_showSelRadial = true; }

void EditorApp::ModalWireframeToggle() { m_wireframeAll = !m_wireframeAll; }

void EditorApp::ModalFrameSel() {
	if (IsFlatUvViewMode(ActiveViewMode())) {
		// Flat pane: reset navigation (zoom 1, pan 0).
		m_uvZoom[m_activeVp] = 1.0f;
		m_uvPan[m_activeVp] = glm::vec2(0.0f);
		Log("UV pane reset (zoom 1, pan 0).");
	}
	else {
		FrameSelection();
	}
}

void EditorApp::ModalFrameAll() { FrameAll(); }

void EditorApp::ModalCycleNodePrev() {
	if (m_inspectorCycle)
		SelectPrevNode();
}

void EditorApp::ModalCycleNodeNext() {
	if (m_inspectorCycle)
		SelectNextNode();
}

void EditorApp::ModalViewUv0() { ActiveViewMode() = ViewMode::Uv0; }

// ---- lighting handlers ----

void EditorApp::ModalLightGizmosToggle() {
	m_showLightGizmos = !m_showLightGizmos;
}

void EditorApp::ModalViewBaked() {
	if (m_lmGpu)
		ActiveViewMode() = ViewMode::Baked;
	else
		Log("Baked view needs a lightmap atlas (open a baked pak).");
}

void EditorApp::ModalViewLmUv() { ActiveViewMode() = ViewMode::Uv1; }

void EditorApp::ModalUvInsetCycle() {
	m_secondView = SecondView((int(m_secondView) + 1) % 3);
}

void EditorApp::ModalSunAzimuthNeg() {
	EditorEnv& e = CurrentEnv();
	e.sunAzimuth = std::fmod(e.sunAzimuth - 5.0f + 360.0f, 360.0f);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "Sun azimuth: %.0f deg", e.sunAzimuth);
	Log(buf);
}

void EditorApp::ModalSunAzimuthPos() {
	EditorEnv& e = CurrentEnv();
	e.sunAzimuth = std::fmod(e.sunAzimuth + 5.0f + 360.0f, 360.0f);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "Sun azimuth: %.0f deg", e.sunAzimuth);
	Log(buf);
}

void EditorApp::ModalSunElevationNeg() {
	EditorEnv& e = CurrentEnv();
	e.sunElevation = std::clamp(e.sunElevation - 2.0f, 0.0f, 90.0f);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "Sun elevation: %.0f deg", e.sunElevation);
	Log(buf);
}

void EditorApp::ModalSunElevationPos() {
	EditorEnv& e = CurrentEnv();
	e.sunElevation = std::clamp(e.sunElevation + 2.0f, 0.0f, 90.0f);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "Sun elevation: %.0f deg", e.sunElevation);
	Log(buf);
}

void EditorApp::ModalSunIntensityNeg() {
	EditorEnv& e = CurrentEnv();
	e.sunIntensity = std::clamp(e.sunIntensity / 1.25f, 0.0f, 10.0f);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "Sun intensity: %.2f", e.sunIntensity);
	Log(buf);
}

void EditorApp::ModalSunIntensityPos() {
	EditorEnv& e = CurrentEnv();
	e.sunIntensity = std::clamp(e.sunIntensity * 1.25f, 0.0f, 10.0f);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "Sun intensity: %.2f", e.sunIntensity);
	Log(buf);
}

// ---- test handlers ----

void EditorApp::ModalRunInApp() { RunInApp(); } // guards + logs inside
void EditorApp::ModalCook() { CookToPak(); }    // guards + logs inside

void EditorApp::ModalViewMeshlets() {
	ActiveViewMode() = ViewMode::Meshlets;
	if (!m_meshletDebugHasData && !EnsureMeshletDebug())
		Log("Meshlet view: no cooked meshlets in this scene "
			"(cook with --meshlets or a pak cooked with meshlets); "
			"meshes show as flat fallbacks.");
}

void EditorApp::ModalViewVertexColors() {
	ActiveViewMode() = ViewMode::VertexColors;
}

void EditorApp::ModalTexModeCycle() {
	static const char* kTexModeNames[3] = {
		"streaming (distance demand)", "coarsest mip only", "full (reimport)"
	};
	m_texMode = TexMode((int(m_texMode) + 1) % 3);
	Log(std::string("Texture mode: ") + kTexModeNames[int(m_texMode)]);
	ApplyTexMode(); // reimports glTF scenes; pak guard logs inside
}

// ---- profile handlers (stub) ----

void EditorApp::ModalProfileStub() {
	Log("PROFILE: placeholder binding (architecture stub) — Optick "
		"capture + frame stats land here.");
}

} // namespace editor
