// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// vi-style modal input: the ACTIVE mode's keymap interprets keys.
//
// Architecture (see modal_bindings.h — the single registry):
//   - EVERY binding lives in kModalBindings exactly once, with the
//     modes it belongs to (mode rows first, globals last). Dispatch,
//     the viewport legend and the conflict validator all read that
//     list, so a key can never be silently double-assigned.
//   - UpdateModalInput() (called from RenderFrame, before DrawUI) makes
//     one pass over the registry: a row fires on exact key+modifier
//     match when it applies to the active mode. A key fires at most
//     once per frame.
//   - Keys are skipped while ImGui wants the keyboard (typing into a
//     field) or while the Q radial popup is open (it owns 1/2/3).
//   - PROFILE is a stub: the mode + dispatch + OnModeEnter/OnModeExit
//     hooks exist and one placeholder binding proves the path; capture /
//     frame-stats bindings land in kModalBindings later.
//
// Adding a binding: write a tiny handler below, declare it in
// editor_app.h next to the others, append one row to kModalBindings.

#include "editor_app.h"
#include "modal_bindings.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include <Engine/App/input.h>
#include <Engine/Profile/profile.h>

namespace editor {
namespace {

template <size_t N>
constexpr size_t CountOf(const ModalBindingDef (&arr)[N]) { return N; }

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

namespace {
// File-scope view of the central registry (modal_bindings.h): the
// master array itself is a function-local static inside
// UpdateModalInput (so the table sits next to the dispatch that reads
// it); published here once for the legend + validator below.
const ModalBindingDef* g_modalBindings = nullptr;
size_t g_modalBindingCount = 0;
} // namespace

size_t ModalBindingCount() { return g_modalBindingCount; }

const ModalBindingDef& ModalBindingAt(size_t i) {
	return g_modalBindings[i];
}

bool ModalBindingApplies(const ModalBindingDef& b, EditorMode mode) {
	if ((b.scopes & kScopeGlobal) != 0)
		return true;
	switch (mode) {
	case EditorMode::Layout: return (b.scopes & kScopeLayout) != 0;
	case EditorMode::Lighting: return (b.scopes & kScopeLighting) != 0;
	case EditorMode::Test: return (b.scopes & kScopeTest) != 0;
	case EditorMode::Profile: return (b.scopes & kScopeProfile) != 0;
	}
	return false;
}

// Conflict validator: every same-(key,mods) pair that resolves in a
// common mode is reported — a deliberate mode-over-global shadow is a
// WARNING (must stay the exception), anything else is an ERROR.
// Debug builds assert (fail fast in dev); Release only logs.
int EditorApp::ValidateModalBindings() {
	int errors = 0;
	const size_t n = ModalBindingCount();
	for (size_t i = 0; i < n; ++i) {
		const ModalBindingDef& a = ModalBindingAt(i);
		if (!a.run)
			continue;
		for (size_t j = i + 1; j < n; ++j) {
			const ModalBindingDef& b = ModalBindingAt(j);
			if (!b.run || a.key != b.key || a.mods != b.mods)
				continue;
			bool common = false;
			for (int m = 0; m < 4 && !common; ++m) {
				const EditorMode mode = EditorMode(m);
				common = ModalBindingApplies(a, mode)
					&& ModalBindingApplies(b, mode);
			}
			if (!common)
				continue;
			const bool aGlobal = (a.scopes & kScopeGlobal) != 0;
			const bool bGlobal = (b.scopes & kScopeGlobal) != 0;
			char buf[320];
			if (aGlobal != bGlobal) {
				std::snprintf(buf, sizeof(buf),
					"modal keys: '%s' shadows global '%s' (%s overrides)",
					aGlobal ? b.label : a.label,
					aGlobal ? a.label : b.label, b.keycap);
				Log(std::string("WARNING ") + buf);
			} else {
				std::snprintf(buf, sizeof(buf),
					"modal keys: CONFLICT '%s' vs '%s' (key %s)",
					a.label, b.label, a.keycap);
				Log(std::string("ERROR ") + buf);
				++errors;
			}
		}
	}
	if (errors == 0)
		Log("modal keys: validated (" + std::to_string(n) + " bindings).");
	assert(errors == 0 && "modal key conflicts (see log)");
	return errors;
}

void EditorApp::UpdateModalInput() {
	// A blocking task window (cook / bake / publish progress) owns the
	// UI while up: no mode switches, gizmo ops, deletes or undos behind
	// the modal (viewport mouse is already eaten by the modal itself via
	// WantsCaptureMouse; this gate covers the keyboard half).
	if (TaskWindowBlocking())
		return;
	// Keys belong to ImGui while typing into a field, and to the Q
	// radial popup while it is open (its 1/2/3 must not switch modes).
	if (m_uiMode && m_imgui && m_imgui->WantsCaptureKeyboard())
		return;
	if (m_selRadialOpen)
		return;
	const bool ctrl = m_input.isKeyHeld(TOUR_KEY_LEFT_CONTROL) ||
		m_input.isKeyHeld(TOUR_KEY_RIGHT_CONTROL);
	const bool shift = m_input.isKeyHeld(TOUR_KEY_LEFT_SHIFT) ||
		m_input.isKeyHeld(TOUR_KEY_RIGHT_SHIFT);
	const bool alt = m_input.isKeyHeld(TOUR_KEY_LEFT_ALT) ||
		m_input.isKeyHeld(TOUR_KEY_RIGHT_ALT);

	// ---- central key registry (modal_bindings.h: the single place) ----
	// Every binding lives here exactly once, with the modes it belongs
	// to. Mode-specific rows come first, globals last: on a shared key
	// the first matching row wins (so a mode entry shadows a global —
	// the validator flags those, and same-scope duplicates are errors).
	static const ModalBindingDef kModalBindings[] = {
		// LAYOUT: object-level editing (mirrors the old kLayout table).
		{ TOUR_KEY_W, 0, kScopeLayout, &EditorApp::ModalGizmoTranslate,
			"gizmo: translate", "W" },
		{ TOUR_KEY_E, 0, kScopeLayout, &EditorApp::ModalGizmoRotate,
			"gizmo: rotate", "E" },
		{ TOUR_KEY_R, 0, kScopeLayout, &EditorApp::ModalGizmoScale,
			"gizmo: scale", "R" },
		{ TOUR_KEY_G, 0, kScopeLayout, &EditorApp::ModalGizmoCycle,
			"gizmo: cycle", "G" },
		// THE SELECT RADIAL (Q): GLOBAL — every mode selects (the old
	// Layout-only binding left Q dead in Lighting/Test/Profile: "the
	// quick menu doesn't show on key" in any non-Layout mode, which the
	// Sponza GI workflow (LIGHTING) hit by default).
	{ TOUR_KEY_Q, 0, kScopeGlobal, &EditorApp::ModalSelRadial,
			"select: radial (Mesh/Face/Vertex)", "Q" },
		{ TOUR_KEY_Z, 0, kScopeLayout, &EditorApp::ModalWireframeToggle,
			"view: wireframe all", "Z" },
		{ TOUR_KEY_F, 0, kScopeLayout, &EditorApp::ModalFrameSel,
			"frame selection", "F" },
		{ TOUR_KEY_F, kModalShift, kScopeLayout, &EditorApp::ModalFrameAll,
			"frame all", "Shift+F" },
		{ TOUR_KEY_UP, 0, kScopeLayout, &EditorApp::ModalCycleNodePrev,
			"selection: previous node", "Up" },
		{ TOUR_KEY_DOWN, 0, kScopeLayout, &EditorApp::ModalCycleNodeNext,
			"selection: next node", "Down" },
		// LIGHTING: sun/env + lightmap authoring (old kLighting table).
		{ TOUR_KEY_W, 0, kScopeLighting, &EditorApp::ModalGizmoTranslate,
			"gizmo: translate", "W" },
		{ TOUR_KEY_E, 0, kScopeLighting, &EditorApp::ModalGizmoRotate,
			"gizmo: rotate", "E" },
		{ TOUR_KEY_R, 0, kScopeLighting, &EditorApp::ModalGizmoScale,
			"gizmo: scale", "R" },
		{ TOUR_KEY_G, 0, kScopeLighting, &EditorApp::ModalGizmoCycle,
			"gizmo: cycle", "G" },
		{ TOUR_KEY_F, 0, kScopeLighting, &EditorApp::ModalFrameSel,
			"frame selection", "F" },
		{ TOUR_KEY_F, kModalShift, kScopeLighting, &EditorApp::ModalFrameAll,
			"frame all", "Shift+F" },
		{ TOUR_KEY_L, 0, kScopeGlobal, &EditorApp::ModalLightGizmosToggle,
			"light gizmos", "L" },
		{ TOUR_KEY_J, 0, kScopeGlobal, &EditorApp::ModalCvolPreviewCycle,
			"view: GI volumes (off/wire/solid)", "J" },
		{ TOUR_KEY_F8, 0, kScopeGlobal, &EditorApp::ModalViewBaked,
			"view: baked irradiance", "F8" },
		{ TOUR_KEY_F5, kModalShift, kScopeLighting,
			&EditorApp::ModalUvInsetCycle, "UV inset: cycle", "Shift+F5" },
		{ TOUR_KEY_LEFT_BRACKET, 0, kScopeLighting,
			&EditorApp::ModalSunAzimuthNeg, "sun azimuth -5", "[" },
		{ TOUR_KEY_RIGHT_BRACKET, 0, kScopeLighting,
			&EditorApp::ModalSunAzimuthPos, "sun azimuth +5", "]" },
		{ TOUR_KEY_MINUS, 0, kScopeLighting,
			&EditorApp::ModalSunElevationNeg, "sun elevation -2", "-" },
		{ TOUR_KEY_EQUAL, 0, kScopeLighting,
			&EditorApp::ModalSunElevationPos, "sun elevation +2", "=" },
		{ TOUR_KEY_COMMA, 0, kScopeLighting,
			&EditorApp::ModalSunIntensityNeg, "sun intensity /1.25", "," },
		{ TOUR_KEY_PERIOD, 0, kScopeLighting,
			&EditorApp::ModalSunIntensityPos, "sun intensity *1.25", "." },
		// TEST: verify (old kTest table).
		{ TOUR_KEY_C, 0, kScopeTest, &EditorApp::ModalCook,
			"cook to .pak", "C" },
		{ TOUR_KEY_T, 0, kScopeTest, &EditorApp::ModalTexModeCycle,
			"textures: residency mode", "T" },
		// PROFILE: stub (old kProfile table).
		{ TOUR_KEY_O, 0, kScopeProfile, &EditorApp::ModalProfileStub,
			"profile: placeholder", "O" },
		// GLOBAL: mode switches, App-parity debug views (F1/F2/F3/F4/F5/
		// F6/F9/F10/F11 — the same keys the standalone App uses, wired
		// straight through to the editor passes), wireframe (P -
		// App parity; Z stays as the LAYOUT alias), window toggles,
		// document ops — reachable from ANY mode.
		{ TOUR_KEY_1, 0, kScopeGlobal, &EditorApp::ModalModeLayout,
			"mode: LAYOUT", "1" },
		{ TOUR_KEY_2, 0, kScopeGlobal, &EditorApp::ModalModeLighting,
			"mode: LIGHTING", "2" },
		{ TOUR_KEY_3, 0, kScopeGlobal, &EditorApp::ModalModeTest,
			"mode: TEST", "3" },
		{ TOUR_KEY_4, 0, kScopeGlobal, &EditorApp::ModalModeProfile,
			"mode: PROFILE", "4" },
		{ TOUR_KEY_F1, 0, kScopeGlobal, &EditorApp::ModalViewLit,
			"view: lit", "F1" },
		{ TOUR_KEY_F2, 0, kScopeGlobal, &EditorApp::ModalViewUnlit,
			"view: unlit", "F2" },
		{ TOUR_KEY_F3, 0, kScopeGlobal, &EditorApp::ModalViewMeshlets,
			"view: meshlets", "F3" },
		{ TOUR_KEY_F4, 0, kScopeGlobal, &EditorApp::ModalViewCascades,
			"view: shadow cascades", "F4" },
		{ TOUR_KEY_F5, 0, kScopeGlobal, &EditorApp::ModalViewLods,
			"view: LODs", "F5" },
		{ TOUR_KEY_F6, 0, kScopeGlobal, &EditorApp::ModalViewVertexColors,
			"view: vertex colors", "F6" },
		{ TOUR_KEY_F9, 0, kScopeGlobal, &EditorApp::ModalViewUv0,
			"view: UV0", "F9" },
		{ TOUR_KEY_F10, 0, kScopeGlobal, &EditorApp::ModalViewLmUv,
			"view: UV1 (lightmap)", "F10" },
		{ TOUR_KEY_F11, 0, kScopeGlobal, &EditorApp::ModalViewPom,
			"view: parallax (POM)", "F11" },
		// F12 (App parity): the live frame-time histogram toggle — the
		// same state the standalone App's F12 flips, the View menu item
		// calls the same handler (ModalFrameHistogramToggle), and the
		// strip draws at the end of DrawUI (profiler::DrawFrameHistogramUi).
		{ TOUR_KEY_F12, 0, kScopeGlobal, &EditorApp::ModalFrameHistogramToggle,
			"view: frame-time histogram", "F12" },
		{ TOUR_KEY_P, 0, kScopeGlobal,
			&EditorApp::ModalParentSelection,
			"parent: all selected to the last selected (Maya)", "P" },
		{ TOUR_KEY_P, kModalShift, kScopeGlobal,
			&EditorApp::ModalUnparentSelection,
			"unparent: detach the selection to the root", "Shift+P" },
		{ TOUR_KEY_V, 0, kScopeGlobal, &EditorApp::ModalVpLayoutToggle,
			"viewport: single/quad", "V" },
		{ TOUR_KEY_I, 0, kScopeGlobal, &EditorApp::ModalUiToggle,
			"ui: panels on/off", "I" },
		{ TOUR_KEY_DELETE, 0, kScopeGlobal, &EditorApp::ModalDelete,
			"delete: selection", "Del" },
		{ TOUR_KEY_D, kModalCtrl, kScopeGlobal, &EditorApp::ModalDuplicate,
			"duplicate: selection", "Ctrl+D" },
		{ TOUR_KEY_H, kModalCtrl, kScopeGlobal,
			&EditorApp::ModalToggleVisibility,
			"visibility: hide/unhide selection", "Ctrl+H" },
		{ TOUR_KEY_Z, kModalCtrl, kScopeGlobal, &EditorApp::ModalUndo,
			"undo", "Ctrl+Z" },
		{ TOUR_KEY_Z, kModalCtrl | kModalShift, kScopeGlobal,
			&EditorApp::ModalRedo, "redo", "Ctrl+Shift+Z" },
		{ TOUR_KEY_Y, kModalCtrl, kScopeGlobal, &EditorApp::ModalRedo,
			"redo", "Ctrl+Y" },
		{ TOUR_KEY_S, kModalCtrl, kScopeGlobal, &EditorApp::ModalSave,
			"tour: save", "Ctrl+S" },
		{ TOUR_KEY_S, kModalCtrl | kModalAlt, kScopeGlobal,
			&EditorApp::ModalSaveIncremental, "tour: save incremental",
			"Ctrl+Alt+S" },
	};

	// Publish the registry for the file-scope accessors (first frame;
	// idempotent — the legend and validator read through it).
	if (!g_modalBindings) {
		g_modalBindings = kModalBindings;
		g_modalBindingCount = CountOf(kModalBindings);
		static const bool kKeysChecked = (ValidateModalBindings(), true);
		(void)kKeysChecked;
	}
	// (master list above; the per-mode tables are gone — every row
	// lives in kModalBindings exactly once with its scope bits).
	// TEST: verify (cook / run in App / debug views / tex residency).
	// PROFILE: perf capture (STUB). Architecture only for now: the table
	// + dispatch + OnModeEnter hook prove the path; Optick capture /
	// frame-stat bindings land here later.

	// A key fires at most once per frame (even if two rows list it).
	// Single pass over the central registry: a row fires when it
	// belongs to the active mode (or is global) with an exact
	// key+modifier match. File order is priority (mode rows precede
	// globals in kModalBindings).
	bool consumed[Input::KeyCount] = {};
	const size_t bindingCount = ModalBindingCount();
	for (size_t i = 0; i < bindingCount; ++i) {
		const ModalBindingDef& b = ModalBindingAt(i);
		if (!b.run || consumed[b.key] || !m_input.isKeyPressed(b.key))
			continue;
		if (!ModalBindingApplies(b, m_mode))
			continue;
		if (ModsMatch(b.mods, ctrl, shift, alt)) {
			consumed[b.key] = true;
			(this->*b.run)();
		}
	}
}

// ---- mode switches ----

void EditorApp::ModalModeLayout() { SetMode(EditorMode::Layout); }
void EditorApp::ModalModeLighting() { SetMode(EditorMode::Lighting); }
void EditorApp::ModalModeTest() { SetMode(EditorMode::Test); }
void EditorApp::ModalModeProfile() { SetMode(EditorMode::Profile); }

// ---- global handlers ----

void EditorApp::ModalViewLit() { ActiveViewMode() = ViewMode::Lit; }
void EditorApp::ModalViewUnlit() { ActiveViewMode() = ViewMode::Unlit; }

void EditorApp::ModalFrameHistogramToggle() {
	// F12 / View menu — one handler, one binding (see kModalBindings).
	// Toggles the shared profiler state (the standalone App's F12 flips
	// the same flag); the bottom strip draws at the end of DrawUI.
	const bool on = !smvk::profiler::FrameHistogramEnabled();
	smvk::profiler::SetFrameHistogramEnabled(on);
	Log(on ? "Frame-time histogram: on (bottom strip; F12 or View menu)."
		: "Frame-time histogram: off.");
}

void EditorApp::ModalVpLayoutToggle() {
	m_vpLayout = (m_vpLayout == 0) ? 1 : 0;
	// Single layout shows viewport 0 only: make it the current one so
	// the active camera/view-mode/inspector track the visible panel.
	if (m_vpLayout == 0)
		m_activeVp = 0;
}
void EditorApp::ModalUiToggle() { m_uiMode = !m_uiMode; }

void EditorApp::ModalDelete() {
	// The placement graph window owns Del while its hierarchy is
	// focused (the canvas deletes graph nodes/wires there); the modal
	// binding serves the world selection otherwise. The flag is set by
	// DrawPlacementGraph (last frame's focus).
	if (m_imguiFocusInPlacementGraph)
		return;
	DeleteSelection();
}

void EditorApp::ModalDuplicate() {
	// Ctrl+D / Edit > Duplicate — one handler, one binding (same
	// contract as Del above: the placement graph window owns Ctrl+D
	// while its hierarchy is focused, so its own node duplication
	// stays reachable there without a double action).
	if (m_imguiFocusInPlacementGraph)
		return;
	DuplicateSelection();
}

void EditorApp::ModalToggleVisibility() {
	// Ctrl+H: toggle the selection's self-hidden flag. All nodes follow
	// the PRIMARY's inverse (a mixed selection hides when the primary
	// is visible, unhides when it is hidden) — predictable bulk edit.
	if (!m_world || m_worldSelected)
		return;
	std::vector<uint32_t> targets(m_selection);
	if (targets.empty() && m_selected != UINT32_MAX)
		targets.push_back(m_selected);
	if (targets.empty())
		return;
	bool hide = false;
	const uint32_t primary = m_selected != UINT32_MAX ? m_selected
		: targets[0];
	if (primary < m_world->scene.hierarchy.size())
		hide = IsVisibleNode(m_world->scene, int(primary));
	int n = 0;
	int skippedRef = 0;
	for (uint32_t t : targets) {
		if (t == UINT32_MAX || t >= m_world->scene.hierarchy.size()
			|| m_deletedNodes.contains(t))
			continue;
		if (IsRefPlaceholder(t)) {
			// Placeholder visibility is owned by the reference's
			// `enabled` bool (Reference editor checkbox) — never by
			// visibility commands.
			++skippedRef;
			continue;
		}
		EditCmd c{};
		c.verb = EditVerb::SetVisibility;
		c.node = t;
		const bool hidden = IsHiddenNode(m_world->scene, int(t));
		c.afterFlag = hide;
		c.beforeFlag = hidden;
		Do(c);
		++n;
	}
	if (skippedRef > 0)
		Log("Visibility: reference placeholder(s) skipped — use the "
			"reference checkbox (File > Reference editor).");
	if (n > 0)
		Log(std::string("Visibility: ") + (hide ? "hid" : "showed") + " "
			+ std::to_string(n) + " node" + (n > 1 ? "s" : "")
			+ " (Ctrl+H toggles).");
}

void EditorApp::ModalUndo() { Undo(); }
void EditorApp::ModalRedo() { Redo(); }
void EditorApp::ModalSave() {
	if (SaveTour())
		Log("Tour saved: " + TourTarget());
}
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

void EditorApp::ModalParentSelection() {
	// Maya P: parent EVERY selected node under the LAST selected (the
	// newest = the target; it never parents itself). One undoable
	// Parent command per node (the cmd holds one node).
	if (!m_world || m_selection.size() < 2) {
		if (m_selection.size() < 2)
			Log("Parent: select at least two nodes (the last selected "
				"is the parent).");
		return;
	}
	const uint32_t target = m_selected; // newest = last selected
	uint32_t moved = 0;
	for (uint32_t n : m_selection) {
		if (n == target || n >= m_world->scene.hierarchy.size())
			continue;
		if (uint32_t(m_world->scene.hierarchy[n].parent) == target)
			continue; // already under the target
		EditCmd c{};
		c.verb = EditVerb::Parent;
		c.node = n;
		c.node2 = target;
		c.beforeParent =
			uint32_t(m_world->scene.hierarchy[n].parent);
		c.afterText = getNodeName(m_world->scene, int(target));
		Do(c);
		++moved;
	}
	Log("Parent: " + std::to_string(moved) + " node(s) under '"
		+ getNodeName(m_world->scene, int(target)) + "'.");
}

void EditorApp::ModalUnparentSelection() {
	// Maya Shift+P: detach every selected node to the ROOT (the cluster
	// root node 0 — the top of the scene DAG).
	if (!m_world || m_selection.empty())
		return;
	uint32_t moved = 0;
	for (uint32_t n : m_selection) {
		if (n == 0 || n >= m_world->scene.hierarchy.size())
			continue;
		if (m_world->scene.hierarchy[n].parent <= 0)
			continue; // already at the root
		EditCmd c{};
		c.verb = EditVerb::Parent;
		c.node = n;
		c.node2 = 0; // the cluster root
		c.beforeParent =
			uint32_t(m_world->scene.hierarchy[n].parent);
		Do(c);
		++moved;
	}
	Log("Unparent: " + std::to_string(moved)
		+ " node(s) detached to the root.");
}

void EditorApp::ModalFrameSel() {
	// UV editor window retired — F always frames the selection.
	//if (m_showUvEditor && m_uvWinHovered) {
	//	m_uvWinZoom = 1.0f;
	//	m_uvWinPan = glm::vec2(0.0f);
	//	Log("UV window reset (zoom 1, pan 0).");
	//}
	{
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

void EditorApp::ModalCvolPreviewCycle() {
	// Off -> wireframe -> wireframe + translucent fill -> probe grid
	// (the convex GI volumes baked by GiBaker --cv-spec; color-coded by
	// priority; the probe mode draws the dense bake lattice — green
	// dots carry radiance, red are zero/invalid voxels: the coverage
	// answer).
	static const char* kNames[4] = { "off", "wireframe", "solid",
		"probes" };
	m_cvolPreviewMode = (m_cvolPreviewMode + 1) % 4;
	if (m_cvolPreviews.empty() && m_cvolPreviewMode != 0) {
		Log("GI volume preview: no .cvmanifest bake files loaded "
			"(bake GiBaker --cv-spec next to the pak).");
	} else {
		Log("GI volume preview: " + std::string(kNames[m_cvolPreviewMode])
			+ " (" + std::to_string(m_cvolPreviews.size()) + " volume(s))");
	}
}

void EditorApp::ModalViewBaked() {
	if (m_lmGpu) {
		// Honest empty-view warning: an atlas with a scene whose meshes
		// carry no lmChunk stamps (a recook with lightmap off strips
		// them) draws NOTHING — the baked pass skips unchunked meshes
		// by design (they would sample foreign texels). Say so instead
		// of a silently blank viewport.
		if (m_world) {
			bool anyChunk = false;
			m_world->forEachRenderItem(
				smvk::IteratorFlags(smvk::IteratorFlagBit_Renderable
					| smvk::IteratorFlagBit_Transparent
					| smvk::IteratorFlagBit_StaticVertexFmt),
				[&](const smvk::World::RenderItem& item) {
					if (smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh))
						if (mesh->lmChunk != 0xFFFFFFFFu)
							anyChunk = true;
				});
			if (!anyChunk)
				Log("Baked view: the cooked scene carries no lightmap "
					"chunks (recooked with lightmap off?) — unchunked "
					"meshes are skipped, the view will be empty.");
		}
		ActiveViewMode() = ViewMode::Baked;
	}
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
	// Lux-native env (SceneEnv): the nudges walk the lux scale directly.
	e.sunIntensity = glm::clamp(e.sunIntensity / 1.25f, 0.0f, 2e5f);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "Sun intensity: %.0f lux", e.sunIntensity);
	Log(buf);
}

void EditorApp::ModalSunIntensityPos() {
	EditorEnv& e = CurrentEnv();
	e.sunIntensity = glm::clamp(e.sunIntensity * 1.25f, 0.0f, 2e5f);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "Sun intensity: %.0f lux", e.sunIntensity);
	Log(buf);
}

// ---- test handlers ----

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

void EditorApp::ModalViewCascades() {
	ActiveViewMode() = ViewMode::Cascades;
}

void EditorApp::ModalViewLods() {
	ActiveViewMode() = ViewMode::Lods;
}

void EditorApp::ModalViewPom() {
	ActiveViewMode() = ViewMode::Pom;
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
