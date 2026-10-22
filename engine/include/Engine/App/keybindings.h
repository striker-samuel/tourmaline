// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Centralized App keybindings: every global shortcut owns ONE semantic
// name here, mapped to its physical key. Edit keys in this file — never
// scatter raw GLFW_KEY_* reads across shortcut handlers (see
// WorldRenderer::HandleShortcuts, which consumes exactly these names).
//
// Rules:
// - WASDQE/Space/Shift/Ctrl are camera movement (held, in UpdateCamera)
//   and are NEVER bound here.
// - One action per key: adding a binding on an occupied key steals it from
//   the previous owner.
// - Debug-view selectors (F1..F11) and the AA bank (Z, 1..4) are names
//   too, so the panel hints and the handlers cannot drift apart.

#include <Engine/App/input.h>

namespace engine {
namespace keybind {

// ---- Mode / UI ----
constexpr int UI_TOGGLE = GLFW_KEY_I;       // FPS <-> UI panel mode
// The debug PANEL (the right-hand "Tourmaline - Debug" sidebar): TAB pops
// it in/out (the window's close X flips the same flag). The panel starts
// HIDDEN — hosts render a clean scene by default; the stats HUD below is
// the light on-screen readout when wanted. TAB is otherwise unbound
// (UiConfirm is RETURN in the Input action bindings, not TAB).
constexpr int DEBUG_PANEL_TOGGLE = GLFW_KEY_TAB;
// VRAM usage viewer (Ctrl+M, the HUD_TOGGLE chord pattern): a floating
// tool window listing the GPU memory heaps (sizes + VMA budgets/usage).
// The CHORD avoids the bare-M motion-blur toggle; Ctrl alone stays
// camera-descend (the movement reservation is untouched — only the
// chord toggles). Works in FPS and UI modes like the texture viewer.
constexpr int VRAM_VIEWER_TOGGLE = GLFW_KEY_M;

// ---- Feature toggles ----
constexpr int MOTION_BLUR_TOGGLE = GLFW_KEY_M;
constexpr int AO_CYCLE = GLFW_KEY_O;        // None -> Ssao -> Sao
constexpr int BLOOM_TOGGLE = GLFW_KEY_B;
constexpr int AUTO_EXPOSURE_TOGGLE = GLFW_KEY_X;
constexpr int MOTION_DEBUG_TOGGLE = GLFW_KEY_V;
constexpr int LOD_TOGGLE = GLFW_KEY_L;
constexpr int PARTICLES_TOGGLE = GLFW_KEY_F;// GPU particle system on/off
constexpr int DEFERRED_SHADOWS_TOGGLE = GLFW_KEY_H;
// G was free (the old filtered-view A/B toggle died with the legacy
// direct-draws off path); it is now the camera reset (CAMERA_RESET above).
constexpr int SKY_TOGGLE = GLFW_KEY_K;
constexpr int WIREFRAME_TOGGLE = GLFW_KEY_P;
// Camera reset: G snaps the FPS camera back to the scene spawn pose
// (positioner.reset() — the LAST lookAt framing: Configure defaults /
// authored scene camera / generic fit; the glide dies too). G was the
// documented free letter (the old filtered-view toggle died with the
// legacy path).
constexpr int CAMERA_RESET = GLFW_KEY_G;
// Tiled-terrain render on/off (replaces the old Terrain field checkbox).
constexpr int TERRAIN_TOGGLE = GLFW_KEY_T;
// Culling freeze (m_freezeCullingView): Ctrl chord like VRAM_VIEWER_TOGGLE
// below — bare F stays the particle toggle. Ctrl alone stays
// camera-descend; the chord only fires the freeze.
constexpr int CULLING_FREEZE_TOGGLE = GLFW_KEY_F;

// ---- Ambient / GI ----
constexpr int AMBIENT_CYCLE = GLFW_KEY_N;   // All -> Lightmaps -> Probes -> SkyIbl -> Off
constexpr int GI_DEBUG_CYCLE = GLFW_KEY_R;  // baked-GI debug views (alias GI_DEBUG_CYCLE_ALT)
constexpr int GI_DEBUG_CYCLE_ALT = GLFW_KEY_F8;

// ---- Actions (fire once, not toggles) ----
constexpr int DECAL_SPLAT = GLFW_KEY_J;     // blood splat at the crosshair
constexpr int CAPTURE = GLFW_KEY_F7;        // LUT export + screenshot

// ---- Tool windows (pop up on demand, outside the debug panel) ----
constexpr int TEXTURE_VIEWER_TOGGLE = GLFW_KEY_GRAVE_ACCENT; // ` texture inspector window
// World Map (top-down XZ canvas: heightfield + volumes + probes).
constexpr int WORLD_MAP_TOGGLE = GLFW_KEY_C;

// ---- AA bank ----
constexpr int AA_CYCLE = GLFW_KEY_Z;        // Off -> MSAA 2x -> SMAA 1x -> SMAA T2x
constexpr int AA_OFF = GLFW_KEY_1;
constexpr int AA_MSAA2X = GLFW_KEY_2;
constexpr int AA_SMAA1X = GLFW_KEY_3;
constexpr int AA_SMAAT2X = GLFW_KEY_4;

// ---- Debug views (shading swaps; wireframe stacks on top) ----
constexpr int DEBUG_VIEW_LIT = GLFW_KEY_F1;
constexpr int DEBUG_VIEW_UNLIT = GLFW_KEY_F2;
constexpr int DEBUG_VIEW_MESHLETS = GLFW_KEY_F3;
constexpr int DEBUG_VIEW_CASCADES = GLFW_KEY_F4;
constexpr int DEBUG_VIEW_LODS = GLFW_KEY_F5;
constexpr int DEBUG_VIEW_VERTEXCOLORS = GLFW_KEY_F6;
constexpr int DEBUG_VIEW_UV0 = GLFW_KEY_F9;
constexpr int DEBUG_VIEW_CUBEMAP = GLFW_KEY_F10; // reused from the lightmap-UV view
// F11: parallax occlusion mapping. The lightmap atlas UV view (Uv1) has NO
// key now - F10 was reassigned to the cubemap-only view above; Uv1 stays
// selectable via the Debug panel combo + TOURMALINE_DEBUGVIEW=uv1.
constexpr int DEBUG_VIEW_POM = GLFW_KEY_F11;

// ---- Diagnostics ----
constexpr int FRAME_HISTOGRAM_TOGGLE = GLFW_KEY_F12; // live frame-time strip (keep; do not add more F12 uses)
constexpr int DS_VIZ_CYCLE = GLFW_KEY_Y;             // deferred-shadow visualizers (DS_DEBUG_VIZ builds)
// Text-only stats HUD (top-left overlay): Ctrl held + T pressed. T alone
// does nothing (no bare binding); Ctrl alone stays camera-descend (the
// WASDQE/Space/Shift/Ctrl movement reservation is untouched — only the
// chord toggles). No F-key was free (F12 is forbidden), and Ctrl+C would
// collide with ImGui copy while typing.
constexpr int HUD_TOGGLE = GLFW_KEY_T;

// Shared GI-debug cycle length: flat irradiance, source palette,
// isolated level, dominant level, + off. HandleShortcuts and UpdateCamera
// used to disagree here (%3 vs %5) and double-fired R in FPS mode; the
// single CycleGiDebug helper below is the only owner.
constexpr int GI_DEBUG_MODE_COUNT = 5;

inline void CycleGiDebug(int& mode) {
	mode = (mode + 1) % GI_DEBUG_MODE_COUNT;
}

} // namespace keybind
} // namespace engine
