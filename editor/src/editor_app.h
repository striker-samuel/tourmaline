// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GltfEditor: inspect + cook glTF scenes with a .tour overlay.
//
// Loads a glTF straight into a GPU World (same importer as App/PakCooker),
// shows it in a minimal forward viewport with two shading modes
// (Unlit / Lit - lighting varies by view mode), and lets the user:
//   - pick nodes in the 3D view (CPU raycast) or the outliner,
//   - move/rotate/scale them with a selection gizmo (highlight drawn as a
//     wireframe overlay + AABB lines),
//   - reassign materials per node (combo over all imported materials),
//   - flag subtrees to skip lightmap baking (Scene::noLightmap),
//   - reparent nodes (drag&drop / detach),
//   - undo/redo everything (CRUD command stack),
//   - save it all to an ASCII .tour file (Maya-style, no blobs) and cook
//     the edited scene to a .pak via PakCooker --tour (deterministic
//     replay bake, full lightmap support).
//
// Operation is MODAL (vi-style): one ACTIVE mode (LAYOUT / LIGHTING /
// TEST / PROFILE) interprets the keys; 1-4 switch modes anywhere (see
// editor_modal.cpp for the keymaps + dispatch).
//
// VIP: the editor never reimplements cooking or baking — the .tour command
// log is the interchange, PakCooker replays it onto a fresh import.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/app.h>
#include <Engine/App/Element.h> // the synchronized-tick element contract (WorldLayer)
#include <Engine/App/world.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/tour.h>
#include <Engine/Asset/cook_asset.h> // GI/lightmap cook assets (browser creation)
#include "editor_asset_ops.h" // browser delete planning (pending plan member)
#include "editor_fibers.h" // EDITOR-ONLY job pool (see editor_fibers.cpp)
#include <Engine/Vfs/vfs.h>
#include <artifact_cache/artifact_cache.h>
#include <artifact_cache/assetdb.h>
#include <gltf_import/gltf.h> // GltfGuidResolver (the hook below)
#include <gltf_import/asset_export.h> // the preprocessed asset derivation
#include <artifact_cache/store_adapter.h>
#include <Engine/Camera/camera.h>
#include <Engine/GI/lightmap.h>
#include <Engine/GI/probe_volume.h>
#include <Engine/GI/triangle_bvh.h>
#include <Engine/Renderer/cubemap_placement.h>
#include <Engine/ImGui/imgui_overlay.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/tonemap.h>
#include <Engine/Renderer/light.h>
#include <Engine/Renderer/texture_streamer.h>

// ABI-checked shared structs (offsets asserted against common.hlsli).
#include <Engine/Renderer/Renderer.h>
#include <Engine/Renderer/WorldRenderer.h>

#include <tiny_gltf.h>

#include <imgui.h> // menu-bar gem mark + inspector pages

#include <Engine/Placement/placement_terrain.h>
#include "editor_commands.h"

#include "editor_placement.h"

namespace editor {

// ---------------------------------------------------------------------------
// The WYSIWYG world LAYER (the synchronized-tick element — the pump's
// phases replace the old mid-frame bolt-in): the world renderer rides
// the element contract so the editor frame and the world frame stay in
// ONE rhythm —
//   onUpdate (the boundary phase, BEFORE the host's acquire): the lazy
//     launch/stop/relaunch lifecycle (the heavy attach/detach destroys
//     land with the previous frame fully presented), Tick + the camera
//     drive + the extent settle (EnsureInlineExtent) + the readback
//     housekeeping — ALL state work, nothing recording.
//   onRender (the layer phase, between acquire and present in stack
//     order): RenderLayerInto — the record only, at the between-rounds
//     position the drive always had.
//   onResize (the pump's recreate boundary, BEFORE the acquire): the
//     world's OnSwapchainRecreated — the inline-target invalidation
//     happens with no work in flight, the re-ensure lands in the next
//     settle, at the boundary.
// The old DriveWysiwygLive ran all of this MID-FRAME (between the
// editor's recording rounds, the frame command buffer open) — the
// frame-blind placement the gesture-crash hunt pinned.
// ---------------------------------------------------------------------------
class WorldLayer final : public engine::IAppElement {
public:
	explicit WorldLayer(class EditorApp* app) : m_app(app) {}
	const char* ElementName() const override { return "World"; }
	bool onAttach() override { return true; } // the world lifecycle is lazy (onUpdate)
	void onDetach() override {}
	void onUpdate(float dt) override;
	void onResize(smvk::Extent2D swapExtent) override;
	void onRender(engine::ElementFrame& frame) override;
private:
	class EditorApp* m_app = nullptr;
};

// Menu-bar gem mark (editor_branding.cpp): vector twin of
// tourmaline_logo.png, drawn straight into the given draw list.
void DrawTourmalineGem(ImDrawList* dl, const ImVec2& p, float h);

// The shared GI probe marker (editor_viewports.cpp): a regular octahedron
// ortho-projected from a fixed 3/4 view, painter-sorted + Lambert-shaded —
// the "gem" read that keeps individual probes distinct where same-color
// dot clusters smear. Used by BOTH probe overlays (the cubemap manifest
// rows in DrawCaptureBallOverlay and the .sggi lattice in
// DrawBakeDensityOverlay); the color table stays the caller's.
void DrawProbeOctahedron(ImDrawList* dl, const ImVec2& center, float radius,
	ImU32 col);

// Octree-LEVEL ramp, byte-identical to the App's SG-GI probe overlay
// (WorldRenderer: coarse red -> fine pink; shared density language).
// Defined in editor_density.cpp; shared by the sggi density overlay and
// the GI-viz window's legend swatches.
extern const ImU32 kDensityLevelColors[8];

// Viewport camera clip + FOV shared by editor_app.cpp / editor_render.cpp.
inline constexpr float kEditorFovYDeg = 45.0f;
inline constexpr float kEditorNearClip = 0.05f;
inline constexpr float kEditorFarClip = 2000.0f;

// Panel layout constants shared by editor_ui.cpp / editor_render.cpp /
// editor_gizmo.cpp: the 3D viewport is the CENTRAL rect between the
// panels (panels are siblings in one layout — they never overlap the
// rendered view). kEditorBottomH is the Overrides/Console dock height;
// kEditorStatusH is the Maya-like bottom status strip (process progress).
inline constexpr float kEditorLeftW = 330.0f;
inline constexpr float kEditorRightW = 370.0f;
inline constexpr float kEditorBottomH = 230.0f;
inline constexpr float kEditorStatusH = 24.0f;

// Central viewport rect in window pixels (x/y = top-left offset).
struct ViewportRect {
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// ---------------------------------------------------------------------------
// Editor cameras: orbit-frame cameras owned by the EDITOR (never cooked,
// never part of the scene). Perspective (finite reversed-Z) or orthographic
// (Top/Side/Front); per-camera FOV + near/far + ortho half-height.
// ---------------------------------------------------------------------------
struct EditorCam {
	enum class Type : int { Perspective = 0, Orthographic = 1 };
	Type type = Type::Perspective;
	std::string name = "Perspective";
	// Maya-style orbit frame (target + yaw/pitch/distance, Y up).
	glm::vec3 target{ 0.0f, 1.0f, 0.0f };
	float yaw = 0.6109f;    // ~35 deg
	float pitch = -0.3142f; // ~-18 deg
	float dist = 6.0f;
	// Perspective clip + FOV.
	float fovYDeg = kEditorFovYDeg;
	float nearClip = kEditorNearClip;
	float farClip = kEditorFarClip;
	// Orthographic half-height (meters); zoom maps the wheel onto this.
	float orthoHalfH = 10.0f;
};

// Eye position (orbit). For ortho the distance only positions the eye;
// depth is translation-invariant along the view axis.
glm::vec3 EditorCamEye(const EditorCam& c);
glm::mat4 EditorCamView(const EditorCam& c);
// Vulkan reversed-Z projection for the given aspect ([1][1] Y-flipped).
glm::mat4 EditorCamProj(const EditorCam& c, float aspect);

inline constexpr int kMaxViewports = 4; // single layout uses [0]; quad uses all

// ---------------------------------------------------------------------------
// Viewport shading mode. The F-keys match the APP'S DebugView shortcuts
// 1:1 (F1 lit, F2 unlit, F3 meshlets, F4 cascades, F5 lods, F6 vertex
// colors, F9 UV0, F10 UV1/lightmap, F11 POM) + P wireframe — the live
// viewport view modes are driven by exactly these, so the
// editor and a standalone App render the same view on the same key.
// ---------------------------------------------------------------------------
enum class ViewMode : int {
	Unlit = 0,       // default_unlit: baseColor + emissive, no lighting (F2)
	Lit = 1,         // default_lit: full PBR pipeline (F1)
	Uv0 = 2,         // default_uv0_debug: base UVs (F9)
	Uv1 = 3,         // default_uv1_debug: lightmap UVs (F10)
	VertexColors = 4, // default_vertexcolor_debug: vertex colors (F6)
	Baked = 5,       // baked atlas irradiance (F8 — App's GI debug view)
	Meshlets = 6,    // meshdebug: flat per-meshlet colors (F3)
	FlatUv0 = 7,     // UV EDITOR WINDOW channel: uvflat0, TEXCOORD0 + diffuse
	FlatUv1 = 8,     // UV EDITOR WINDOW channel: uvflat1, atlas uv2 + lightmap
	Cascades = 9,    // shadow cascade coverage (F4)
	Lods = 10,       // LOD index by color (F5)
	Pom = 11,        // parallax occlusion mapping (F11)
};
inline const char* ViewModeName(ViewMode m) {
	switch (m) {
	case ViewMode::Unlit: return "Unlit";
	case ViewMode::Lit: return "Lit";
	case ViewMode::Uv0: return "UV0";
	case ViewMode::Uv1: return "UV1 (lightmap)";
	case ViewMode::VertexColors: return "Vertex colors";
	case ViewMode::Baked: return "Baked irradiance";
	case ViewMode::Meshlets: return "Meshlets";
	case ViewMode::FlatUv0: return "Flat UV (diffuse)";
	case ViewMode::FlatUv1: return "Flat UV (lightmap)";
	case ViewMode::Cascades: return "Cascade coverage";
	case ViewMode::Lods: return "LODs";
	case ViewMode::Pom: return "POM";
	}
	return "?";
}
// Modes that need the lightmap atlas GPU layers (Baked F8, FlatUv1).
inline bool ViewModeNeedsAtlas(ViewMode m) {
	return m == ViewMode::Baked || m == ViewMode::FlatUv1;
}

// Second viewport (side-strip inset showing a UV debug view next to the
// main view; F5 cycles). Skipped while the main view is itself a UV view.
enum class SecondView : int {
	Off = 0,
	Uv0 = 1, // base UVs
	Uv1 = 2, // lightmap UVs
};
inline const char* SecondViewName(SecondView m) {
	switch (m) {
	case SecondView::Off: return "Off";
	case SecondView::Uv0: return "UV0";
	case SecondView::Uv1: return "UV1 (lightmap)";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// Gizmo interaction mode.
// ---------------------------------------------------------------------------
enum class GizmoMode : int {
	Translate = 0,
	Rotate = 1,
	Scale = 2,
};

// Transform gizmo ORIENTATION (Q quick menu): which axes the handles
// align to. World (DEFAULT) = the scene axes; Local = the selected
// node's world-rotated local axes. Scale is ALWAYS local (per-axis
// world scaling of a rotated node has no TRS form).
enum class GizmoSpace : int { World = 0, Local = 1 };
inline const char* GizmoSpaceName(GizmoSpace s) {
	switch (s) {
	case GizmoSpace::World: return "World";
	case GizmoSpace::Local: return "Local";
	}
	return "?";
}

// Transform gizmo PIVOT (Q quick menu): where the gizmo sits + the
// rotate/scale math origin. Node (DEFAULT, as before) = the node
// origin; BoundsCenter = the selection's subtree AABB center (rotate/
// scale compensate the translate so they act ABOUT the pivot; exact
// for uniform scale + any rotate, per-axis scale exact for unrotated
// nodes). Translate is pivot-independent (delta math).
enum class GizmoPivot : int { Node = 0, BoundsCenter = 1 };
inline const char* GizmoPivotName(GizmoPivot p) {
	switch (p) {
	case GizmoPivot::Node: return "Node origin";
	case GizmoPivot::BoundsCenter: return "Bounds center";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// Viewport selection mode: Mesh (object/node, legacy), Face (single
// triangle highlight), Vertex (single vertex highlight). Highlight-only:
// the gizmo/inspector keep acting on the node; face/vertex never edits
// geometry (no acceleration structures, brute-force CPU picking only).
// ---------------------------------------------------------------------------
enum class SelMode : int {
	Mesh = 0,
	Face = 1,
	Vertex = 2,
};
inline const char* SelModeName(SelMode m) {
	switch (m) {
	case SelMode::Mesh: return "Mesh";
	case SelMode::Face: return "Face";
	case SelMode::Vertex: return "Vertex";
	}
	return "?";
}
// UV editor window channel test (FlatUv0/1 are NOT viewport modes — the
// separate UV editor window owns both; see m_uvWinMode).
inline bool IsFlatUvViewMode(ViewMode m) {
	return m == ViewMode::FlatUv0 || m == ViewMode::FlatUv1;
}

// ---------------------------------------------------------------------------
// vi-style MODAL operation. One ACTIVE mode interprets every key via its
// own keymap (per-mode table first, global fallback; see editor_modal.cpp).
// Keys 1-4 switch modes from ANY mode; the viewport legend shows the mode
// line (like vi's "-- INSERT --"). PROFILE is a stub: the mode, dispatch
// and enter/exit hooks exist so the capture/stats wiring can land later
// without touching the keymap plumbing again.
// ---------------------------------------------------------------------------
enum class EditorMode : int {
	Layout = 0,   // object-level editing: gizmos, selection, framing
	Lighting = 1, // sun/env + lightmap authoring: lm views, sun nudges
	Test = 2,     // verify: cook, run in App, debug views, tex residency
	Profile = 3,  // perf capture (stub — architecture only for now)
};
inline const char* EditorModeName(EditorMode m) {
	switch (m) {
	case EditorMode::Layout: return "LAYOUT";
	case EditorMode::Lighting: return "LIGHTING";
	case EditorMode::Test: return "TEST";
	case EditorMode::Profile: return "PROFILE";
	}
	return "?";
}
// Exact-match modifier bits for ModalBindingDef::mods (all listed
// modifiers must be held, all unlisted ones released; see
// modal_bindings.h).
inline constexpr uint8_t kModalCtrl = 1;
inline constexpr uint8_t kModalShift = 2;
inline constexpr uint8_t kModalAlt = 4;

// ---------------------------------------------------------------------------
// Cook settings — SESSION-ONLY (machine-local paths; the bake knobs live
// on the scene's exportSettings NODE now: SceneExportSettings, authored
// in the inspector, persisted as createNode -at exportSettings +
// setExportAttr tour verbs, consumed by Cook.exe off the replayed scene
// and by the editor bakes through ExportCfg()). outPak/contentDir never
// persist (the old tour cook block's "destination NOT persisted" rule).
// ---------------------------------------------------------------------------
struct CookSettings {
	std::string outPak = "content/paks/edited.pak";
	std::string contentDir = "content";
	std::string inVfs; // VFS path for --in-vfs when no tour
};

class EditorApp : public smvk::App {
	using Super = smvk::App;
	// The synchronized-tick world layer (see WorldLayer above): the
	// element's phases drive the WYSIWYG halves — onUpdate (the
	// boundary, before the host's acquire), onResize (the pump's
	// recreate boundary), onRender (the between-rounds record position).
	friend class WorldLayer;
public:
	EditorApp();
	~EditorApp() override = default;

	// Source-scene open (paks are refused — they run in the App; the
	// editor is the WYSIWYG authoring tool over sources/assets).
	bool OpenScene(const std::string& path);
	// Open-dialog dispatch: .tour replays (OpenTourFile), else OpenScene.
	bool OpenAnySceneFile(const std::string& path);
	bool LoadSglm(const std::string& sglmPath); // manual lightmap atlas inspect
	void SetOutPak(const std::string& p) { m_cook.outPak = p; }

protected:
	bool Initialize() override;
	void Shutdown() override;
	void DrawFrame() override;
	void EarlyUpdate() override { Super::EarlyUpdate(); }
	void LateUpdate() override { Super::LateUpdate(); }
	void OnFramebufferResized(int w, int h) override {
		Super::OnFramebufferResized(w, h);
		m_swapViews.clear(); // re-register against the new swapchain
	}

private:
	// ---- frame ----
	void RenderFrame(smvk::Framebuffer& fb);
	void UploadMaterials(uint32_t flight);

	// ---- scene ----
	bool LoadGltfIntoWorld(const std::string& path,
		const std::string& tourOverride = {});
	// Baked-atlas probe for the open SOURCE scene (SceneIdForHost stem,
	// project content first / bundled content second): the F8 baked view
	// on a definition with a lightmap bake.
	bool TryLoadSceneSglm();
	// Convex GI volume sidecars (probes-only pivot): the SCENE's baked
	// gi/<stem>.cvmanifest + .cvol. Builds the preview hulls (AABB
	// clipped by the volume's planes) for the LIGHTING-mode view option.
	bool TryLoadCvolumes();
	static bool HasPakExtension(const std::string& path);
	// "A scene worth acting on" (replaces the old m_hasGltfModel gates —
	// a .tour definition just references ASSET UUIDS, so "has a source
	// glTF" was the wrong question): true when no world is loaded or the
	// merged scene carries no content (no meshes/lights/cameras). With
	// pak opens gone (paks run in the App), every non-empty editor scene
	// is definition-backed by construction.
	bool SceneEmpty() const;
	void UpdateTextureStreaming(smvk::Extent2D renderExtent);
	// ---- CPU geometry access (uniform, registry-backed) ----------------
	// The editor retains uploads on its world cluster (source imports),
	// so the world's asset registry resolves mesh/node -> CPU geometry
	// with no importer state kept alive. Null when the mesh/node is
	// missing or its cluster did not retain.
	const smvk::StaticMeshUpload* MeshUploadOf(uint32_t worldMeshIdx) const;
	const smvk::StaticMeshUpload* NodeMeshUpload(uint32_t node) const;
	static glm::mat4 LocalMatrixFromTRS(const glm::vec3& t,
		const glm::vec3& eulerDeg, const glm::vec3& s);
	// Structural world-scene edits (index-preserving; no Resolve needed).
	void ReparentNode(uint32_t node, uint32_t newParent);
	void RefreshNodeDrawState(uint32_t node); // material-driven flags/drawItems
	void RefreshMaterialNodes(uint32_t mat); // after shared material edits
	void RenameNode(uint32_t node, const std::string& name);

	// ---- render passes (editor_render.cpp) ----
	bool EnsureShaders();
	smvk::ShaderProgram& MainProgram(ViewMode m); // per-viewport (Lit fallback)
	smvk::ShaderProgram& FlatUvProgram(ViewMode m); // uvflat0/uvflat1 (null-ish on miss)
	// UV-coordinate export program: same flat VS as uvflat1, fragment
	// outputs the atlas uv itself (uvflat1uv.frag — the QA view of the
	// parameterization, no atlas content read).
	smvk::ShaderProgram& FlatUvCoordProgram();
	// Flat UV draw path: the PRIMARY SELECTION's subtree meshes drawn
	// through the uvflat projection (unwrapped + texture content).
	void DrawFlatUvNodes(smvk::CommandBuffer& cmd, ViewMode mode,
		const engine::IRenderer::PushConstants& pc);
	// Flat UV wires (glTF model accessors) as line batches in NDC space.
	// Zoom/pan version (the UV window owns its nav); whole-scene exports
	// pass identity nav (1.0/zero, ignored — applyNav is false there).
	void BuildFlatUvWires(std::vector<smvk::DebugLineVertex>& outLines,
		ViewMode mode, uint32_t subtreeRoot = UINT32_MAX,
		float zoom = 1.0f, glm::vec2 pan = glm::vec2(0.0f)) const;
	smvk::ShaderProgram& InsetProgram(); // program for m_secondView (Lit fallback)
	bool EnsureBuffers(uint32_t flightCount);
	bool EnsureDepth(smvk::Extent2D extent);
	// Per-viewport scene pass: offset/extent from m_viewports[vpIdx].rect,
	// frame slice + push constants from that viewport's camera.
	void RecordMainPass(smvk::LogicalImageViewHandle swapView, int vpIdx,
		uint32_t flight, bool selectedWireOverlay);
	void RecordInsetPass(smvk::LogicalImageViewHandle swapView,
		uint32_t flight); // UV side-strip (single layout, viewport 0)
	// Preview HDR -> swap tonemap layer for one rect (see m_sceneHdr): the
	// layer's rect on the swap (Load keeps the chrome + sibling layers),
	// the matching sub-rect (texel centers) of the shared full-window HDR
	// sampled through the tonemapper's srcUv window.
	void RecordViewportTonemap(smvk::LogicalImageViewHandle swapView,
		const ViewportRect& rect, const char* name);
	// The preview's standing exposure: the authored MANUAL value (the
	// App's adaptation seed - auto metering is App-only, the editor
	// preview has no histogram pass).
	float PreviewExposure() const;
	// Line overlay (grid + gizmos + light markers): ONE upload, ONE pass
	// PER VISIBLE VIEWPORT (that viewport's viewProj + rect).
	void RecordGizmoPass(smvk::LogicalImageViewHandle swapView);
	void UpdateInsetFrameData(uint32_t flight, smvk::Extent2D stripExtent);
	// Viewport rects for the current frame (single: one central rect;
	// quad: 2x2 grid inside the central area) + hovered/active tracking.
	void ComputeViewportRects(smvk::Extent2D extent);
	// Per-viewport frame uniform build + upload (all visible viewports,
	// one slice per (flight, viewport); the ACTIVE viewport drives the
	// shadow cascade + light buffer).
	void UpdateFrameData(uint32_t flight);
	void UpdateCamera(float dt); // orbit/pan/zoom on the ACTIVE camera
	// Second-viewport state: active unless off or the main view already
	// shows UVs; strip rect (right side of the CENTRAL viewport) for the
	// inset pass + label. outX is window-absolute.
	bool SecondViewActive() const;
	static smvk::Extent2D InsetStrip(const ViewportRect& vp, uint32_t& outX);
	void DrawWorldNodes(smvk::CommandBuffer& cmd, const engine::IRenderer::PushConstants& basePc,
		bool maskedPhase, bool selectedOnly, bool shadowPass,
		bool bakedOnly = false, // bakedOnly: skip unbaked meshes (F8 view)
		bool chartTail = false); // chartTail: push the per-mesh chart tail
		// (direct-path atlas unpack for views that sample lightmaps: F8, F10)
	// True when the world holds anything to draw (guards vertex bindings).
	bool HasDrawItems() const;

	// ---- meshlet debug view (F4, editor_render.cpp) ----
	// Expansion of every cooked meshlet into global-vertex-id triangles
	// in ONE shared index buffer (per-meshlet draw ranges, flat colors
	// via the meshdebug program). Meshes without cooked meshlets fall
	// back to their normal surfaces in a dim color (nothing vanishes).
	struct MeshletRange {
		uint32_t firstIndex = 0; // absolute into m_meshletIndices
		uint32_t indexCount = 0;
		uint32_t meshlet = 0;    // meshlet index (stable color hash)
		float coneCutoff = 1.0f; // heat-mode color
	};
	std::vector<MeshletRange> m_meshletRanges;
	std::unordered_map<uint32_t, std::vector<uint32_t>> m_meshletRangesByMesh;
	smvk::Holder<smvk::BufferHandle> m_meshletIndices;
	uint32_t m_meshletIndexCap = 0;
	bool m_meshletDebugBuilt = false;
	bool m_meshletDebugHasData = false; // any mesh actually carries meshlets
	void ResetMeshletDebug();    // scene change / shutdown
	bool EnsureMeshletDebug();   // lazy build (first F4 frame)
	void DrawMeshletNodes(smvk::CommandBuffer& cmd);

	// ---- picking (editor_pick.cpp) ----
	// mouseX/mouseY are VIEWPORT-LOCAL pixels of the given viewport;
	// viewProj is that viewport's frame (per-viewport cameras).
	struct PickHit { uint32_t node = UINT32_MAX; float t = 0.0f; };
	std::optional<PickHit> PickNode(int vpIdx, float mouseX, float mouseY,
		smvk::Extent2D extent) const;
	// Screen-space gizmo-marker pick (lights, cvolume centers, cubemap
	// pins — nodes with no mesh to raycast): nearest marker within the
	// pixel threshold, X-ray semantics (wins over mesh hits). Honors the
	// same View-menu visibility toggles the markers draw with.
	std::optional<PickHit> PickGizmoMarker(int vpIdx, float mouseX,
		float mouseY, smvk::Extent2D extent) const;
	glm::vec3 MouseRay(float mouseX, float mouseY, smvk::Extent2D extent,
		const glm::mat4& viewProj, glm::vec3& outOrigin) const;

	// ---- gizmo (editor_gizmo.cpp) ----
	// Uses the ACTIVE viewport: mouse converted to viewport-local, hit
	// tests in that viewport's dims/viewProj. Shift = snapping (translate
	// snaps to the grid cell, rotate to m_snapRotateDeg, scale to
	// m_snapScaleStep).
	bool GizmoHandleInput(); // returns true while dragging

	// ---- viewports + editor cameras (editor_render.cpp) ----
	// Layout: Single (viewport 0) or Quad (2x2, four cameras). Each
	// viewport binds one editor camera (combo in the viewport label)
	// AND its own VIEW MODE (Lit/Unlit/UV0/UV1/... — UV views are panel
	// types, not a global mode); the hovered viewport is ACTIVE (input,
	// gizmo, shadows, framing, F-key view switches).
	struct EditorViewport {
		int camIdx = 0;        // index into m_editorCams
		ViewMode viewMode = ViewMode::Lit; // this panel's view
		ViewportRect rect;     // computed every frame
		bool hovered = false;  // mouse inside this viewport
		engine::FrameUniforms frame{}; // CPU copy (picking/gizmo math)
		glm::mat4 viewProj{ 1.0f };
		uint64_t frameAddr = 0; // this flight's frame-uniform slice
		// Locked look-through: scene camera node id this panel follows
		// (-1 = free orbit via camIdx). Persists across undo (a deleted
		// node falls back to orbit until it comes back).
		int boundNode = -1;
	};
	std::vector<EditorCam> m_editorCams;   // Persp + Top + Side + Front defaults
	EditorViewport m_viewports[kMaxViewports];
	int m_vpLayout = 0;  // 0 = single, 1 = quad (four panels)
	int m_activeVp = 0;  // hovered/last-active viewport index
	int ViewportCount() const { return m_vpLayout == 1 ? kMaxViewports : 1; }
	EditorCam& ActiveCam() { return m_editorCams[m_viewports[m_activeVp].camIdx]; }
	// Locked look-through resolution: when viewport vp is bound to a
	// live scene camera node, fills view/proj/eye/clips from the node's
	// global pose + lens and returns true (callers fall back to the
	// orbit camera otherwise).
	bool BoundCameraView(int vp, glm::mat4& view, glm::mat4& proj,
		glm::vec3& eye, float& nearClip, float& farClip) const;
	// The SAME bound-camera framing as the overlay matrices build:
	// pose (eye/forward/up — up already carries the shared degenerate
	// fallback) + lens (fov / TRUE ortho half-height / clips). Single
	// source of truth with BoundCameraView.
	struct BoundCameraPose {
		glm::vec3 eye, fwd, up;
		float fovYDeg = 60.0f;
		float orthoHalfH = 0.0f; // > 0 = orthographic lens
		float nearClip = 0.1f, farClip = 100.0f;
	};
	bool BoundCameraPoseFor(int vp, BoundCameraPose& out) const;
	ViewMode& ActiveViewMode() { return m_viewports[m_activeVp].viewMode; }
	ViewMode ActiveViewMode() const { return m_viewports[m_activeVp].viewMode; }
	void SetupDefaultCameras();
	bool GridVisible() const { return m_showGrid; }
	// ---- lighting view (the GI tri-view: View menu radios) ----
	// The bake workflow's three looks: Unlit (albedo), Fully Lit (With
	// GI) — the baked GI sources (world field + convex volumes +
	// cubemaps) — and Realtime Lit — the analytic look without the bake
	// (direct sun + sky IBL). 0 = Fully, 1 = Realtime; Unlit is the F2
	// view mode (the unlit FS replaces shading wholesale).
	int m_litView = 0;

	// ---- selection (multi) ----
	// m_selection holds every selected node (m_selected = primary: the
	// gizmo/inspector act on it). Box-drag in the viewport adds/replaces.
	// m_selMode switches Mesh/Face/Vertex HIGHLIGHT (selection only, never
	// edits): Face stores (node, triangle), Vertex stores (node, vert).
	std::vector<uint32_t> m_selection;
	SelMode m_selMode = SelMode::Mesh;
	uint32_t m_selFaceNode = UINT32_MAX;
	uint32_t m_selFaceTri = UINT32_MAX; // triangle index within the primitive
	uint32_t m_selVertNode = UINT32_MAX;
	uint32_t m_selVertIdx = UINT32_MAX; // vertex index within the primitive
	glm::vec3 m_selFacePos{ 0.0f }; // world hit point (face mode)
	glm::vec3 m_selVertPos{ 0.0f }; // world position (vertex mode)
	bool m_showSelRadial = false; // Q radial popup request (mouse pos cached)
	ImVec2 m_selRadialPos{ 0.0f, 0.0f };
	bool m_inspectorCycle = true; // Up/Down arrows cycle the node selection
	// UV editor window RETIRED (the Live view window replaced the canvas
	// era; solid backgrounds everywhere — no transparent content
	// regions). The members + plumbing below stay commented for a
	// possible future re-enable (the flat-UV SHADERS were already
	// commented out at their load site when this was retired).
	//bool m_showUvEditor = false; // View menu toggle
	//ViewMode m_uvWinMode = ViewMode::FlatUv0; // FlatUv0 = TEXCOORD0, FlatUv1 = uv2
	//float m_uvWinZoom = 1.0f;
	//glm::vec2 m_uvWinPan{ 0.0f, 0.0f };
	//ImVec2 m_uvWinRectMin{ 0.0f, 0.0f }; // canvas rect (screen px, last DrawUI)
	//ImVec2 m_uvWinRectMax{ 0.0f, 0.0f };
	//bool m_uvWinHovered = false; // canvas hover (last DrawUI; F-reset gate)
	bool IsSelected(uint32_t node) const;
	void SetSelection(uint32_t node);        // single
	void AddToSelection(uint32_t node);
	void ToggleSelection(uint32_t node);     // ctrl-click add/remove
	void SelectRange(uint32_t node);         // shift-click visible range
	void ClearSelection();
	// Visible outliner row order this frame (DAG tree + filter rows;
	// non-DAG rows excluded): shift-range addressing. Rebuilt every
	// DrawSceneOutliner.
	std::vector<uint32_t> m_outlinerOrder;
	void ClearSubSelection(); // clears face/vertex highlight only
	// World pseudo-selection: the Outliner's "World" entry (global render
	// settings) is NOT a hierarchy node — it never enters m_selection /
	// m_selected (which stay UINT32_MAX), so gizmos, picking, drags and
	// commands keep working unchanged. Mutually exclusive with node
	// selection: selecting either clears the other.
	bool m_worldSelected = false;
	void SelectWorldNode();
	// Non-DAG selection (outliner "Non-DAG" section): images (file nodes)
	// and materials live outside the hierarchy, so they never enter
	// m_selection/m_selected either. Mutually exclusive with node and
	// World selection: selecting any one clears the others. Extend
	// NonDagKind + CollectNonDagEntries for future node sets — the
	// section rendering and picking below stay kind-driven.
	enum class NonDagKind : uint8_t { Image = 0, Material = 1, Terrain = 2 };
	struct NonDagSel {
		bool valid = false;
		NonDagKind kind = NonDagKind::Image;
		uint32_t index = UINT32_MAX; // image (m_images) or material (world) idx
	};
	struct NonDagEntry {
		NonDagKind kind = NonDagKind::Image;
		uint32_t index = UINT32_MAX;
		std::string label;
	};
	NonDagSel m_nonDagSel;
	uint32_t m_imageFocus = UINT32_MAX; // Images-tab highlight row
	bool m_showNonDag = false;          // outliner non-DAG section toggle
	void CollectNonDagEntries(std::vector<NonDagEntry>& out) const;
	void SelectNonDag(NonDagKind kind, uint32_t index); // single
	void ClearNonDagSelection();
	void DrawNonDagSection(); // outliner section (tree + filter rows)
	// First directional (sun) light node, or UINT32_MAX (World panel).
	uint32_t FindSunNode() const;
	void SelectPrevNode(); // inspector Up/Prev (hierarchy order)
	void SelectNextNode(); // inspector Down/Next
	// Sub-object pick (brute force, no accel structures): closest triangle
	// under the cursor + nearest of its 3 verts + world hit position.
	struct SubHit {
		uint32_t node = UINT32_MAX;
		uint32_t tri = UINT32_MAX;
		uint32_t vert = UINT32_MAX;
		glm::vec3 pos{ 0.0f };
	};
	std::optional<SubHit> PickSubObject(int vpIdx, float mouseX,
		float mouseY, smvk::Extent2D extent) const;
	// Flat-UV window pick RETIRED with the UV editor canvas.
	//std::optional<SubHit> PickFlatUvWindow(float mouseX, float mouseY) const;
	void ApplySubSelection(const SubHit& h); // node + face/vert per m_selMode
	// Flat-UV NDC helpers (must match uvflat.vert.hlsli transform).
	// Zoom/pan form (the UV window owns its nav state); the indexed
	// shorthands below read the UV window state (single pane).
	glm::vec2 UvToFlatNdc(glm::vec2 uv, float zoom, glm::vec2 pan) const;
	glm::vec2 FlatNdcToUv(glm::vec2 ndc, float zoom, glm::vec2 pan) const;
	//glm::vec2 UvToFlatNdc(glm::vec2 uv) const; // UV window nav (retired)
	//glm::vec2 FlatNdcToUv(glm::vec2 ndc) const; // UV window nav (retired)
	// Resolve the selected face/vertex UVs for highlight batches.
	bool SelectedFaceUvs(glm::vec2 outUv[3], ViewMode mode) const;
	bool SelectedVertUv(glm::vec2& outUv, ViewMode mode) const;
	// World-space positions for the 3D face/vertex highlight.
	bool SelectedFaceWorld(glm::vec3 outW[3]) const;
	bool SelectedVertWorld(glm::vec3& outW) const;
	// UV editor window RETIRED (see the members' note above).
	//void DrawUvEditorWindow();
	//void RecordUvWindowPass(smvk::LogicalImageViewHandle swapView,
	//	uint32_t flight);
	void DrawSelectionRadial(); // Q radial: Mesh/Face/Vertex popup
	// Rubber-band state (window pixels, viewport-local rect in m_vpBox*).
	bool m_vpBoxDragging = false;
	bool m_vpBoxAdditive = false;   // Shift = add to current selection
	float m_vpBoxX0 = 0, m_vpBoxY0 = 0, m_vpBoxX1 = 0, m_vpBoxY1 = 0;
	int m_vpBoxVp = 0;              // viewport the box started in
	void FinishBoxSelect();        // apply on release
	void DrawViewportChrome();     // borders + labels + box rect (ImGui)

	// ---- modal operation (editor_modal.cpp + modal_bindings.h) ----
	// One central registry (kModalBindings): every binding lives there
	// exactly once with its scope bits. Dispatch (per frame, from
	// RenderFrame): single pass, exact key+modifier match, file order
	// is priority. Unmapped keys are ignored. Rows are ModalBindingDef
	// (see modal_bindings.h).
	void SetMode(EditorMode m);  // no-op when unchanged; hooks + log
	void UpdateModalInput();     // per-frame key dispatch (RenderFrame)
	// Central-registry validator (editor_modal.cpp): reports same-key
	// overlaps; Debug asserts. Runs once (first UpdateModalInput).
	int ValidateModalBindings();
	void OnModeEnter(EditorMode m); // hook: PROFILE wiring lands here
	void OnModeExit(EditorMode m);  // hook (empty for now)
	// Binding handlers (one per table entry; see editor_modal.cpp).
	void ModalModeLayout();
	void ModalModeLighting();
	void ModalModeTest();
	void ModalModeProfile();
	void ModalViewLit();
	void ModalViewUnlit();
	// F12 (App parity): toggle the live frame-time histogram — the
	// shared profiler state + bottom strip (the View menu item calls the
	// same handler, so key + menu stay one binding; see
	// profiler::SetFrameHistogramEnabled).
	void ModalFrameHistogramToggle();
	void ModalVpLayoutToggle();
	void ModalUiToggle();
	void ModalUndo();
	void ModalRedo();
	void ModalSave(); // Ctrl+S: SaveTour (explicit save -> project file)
	void ModalSaveIncremental();
	void ModalDelete(); // Del: DeleteSelection() (skipped while the
	                     // placement graph window owns the keyboard —
	                     // its canvas Del removes graph nodes instead)
	void ModalDuplicate(); // Ctrl+D: DuplicateSelection() (same
	                       // placement-graph keyboard guard as Del)
	void ModalToggleVisibility(); // Ctrl+H: hide/unhide the selection
	                               // (all nodes follow the primary's inverse)
	void ModalGizmoTranslate();
	void ModalGizmoRotate();
	void ModalGizmoScale();
	void ModalGizmoCycle();
	void ModalSelRadial();
	void ModalWireframeToggle();
	// Maya P/Shift+P: parent every selected node under the LAST
	// selected (the newest); Shift+P detaches the selection to the
	// cluster root.
	void ModalParentSelection();
	void ModalUnparentSelection();
	void ModalFrameSel();
	void ModalFrameAll();
	void ModalCycleNodePrev();
	void ModalCycleNodeNext();
	void ModalViewUv0();
	void ModalLightGizmosToggle();
	void ModalCvolPreviewCycle(); // LIGHTING 'V': off -> wireframe -> solid
	void ModalViewBaked();
	void ModalViewLmUv();
	void ModalUvInsetCycle();
	void ModalSunAzimuthNeg();
	void ModalSunAzimuthPos();
	void ModalSunElevationNeg();
	void ModalSunElevationPos();
	void ModalSunIntensityNeg();
	void ModalSunIntensityPos();
	void ModalCook();
	void ModalViewMeshlets();
	void ModalViewCascades();
	void ModalViewLods();
	void ModalViewVertexColors();
	void ModalViewPom();
	void ModalTexModeCycle();
	void ModalProfileStub();
	EditorMode m_mode = EditorMode::Layout;
	bool m_selRadialOpen = false; // Q radial owns 1/2/3 while open

	// ---- UI (editor_ui.cpp) ----
	void DrawUI(smvk::Extent2D targetExtent);
	void DrawMenuBar();
	// ---- tool topbar (tabbed toolbar below the menu bar) --------------
	// Ribbon-lite: a tab strip of TOOL TYPES (Select / Transform /
	// Terrain / Placement / Modes) with the active tab's controls in a
	// second row. Hides with the panels (m_uiMode); the combined
	// menu+toolbar offset feeds m_menuH so panels, splitters and the
	// viewport rects shift together.
	int m_toolTab = 0; // 0 Select, 1 Transform, 2 Terrain, 3 Placement, 4 Modes
	float DrawToolTabs(float top); // renders the toolbar; returns height
	void DrawOutliner();
	void DrawOutlinerNode(uint32_t node,
		const std::unordered_set<uint32_t>* expandSet = nullptr);
	void DrawInspector();
	void DrawWorldPanel(); // Inspector page for the World pseudo-node
	// Material editor page (m_materialFocus travel) + camera section.
	void DrawMaterialFocusPage();
	void DrawCameraSection(); // active viewport's editor camera settings
	void DrawLightmapPanel(); // .sglm atlas inspection (bake output)
	// Diffuse (baseColor) texture thumbnail: CPU KTX2 decode of the
	// material's textures.x slot, drawn as an ImDrawList mosaic over a
	// checkerboard (no Vulkan/ImGui-texture interop needed). Tinted by
	// the baseColor factor like the lit shader multiplies them.
	struct TexPreview {
		smvk::Cluster* cluster = nullptr;
		int slot = -1;
		size_t srcBytes = 0;
		bool ok = false;
		std::string uri;
		std::string status; // dims line or the reason there is no preview
		uint32_t imgW = 0, imgH = 0; // decoded mip dims
		uint32_t gridW = 0, gridH = 0;
		std::vector<uint32_t> rgba; // gridW*gridH packed RGBA8 (sRGB bytes)
	};
	const TexPreview& ResolveDiffusePreview(uint32_t matWorld);
	void DrawDiffusePreview(uint32_t matWorld, const glm::vec4& tint);
	void ClearTexPreview() { m_texPreview = TexPreview{}; }
	void DrawConsolePanel();
	void DrawInsetTag(); // UV inset label (the legend window is gone)
	void Log(const std::string& s);

	// ---- commands + tour (editor_commands.cpp) ----
	// Execute a command forward (do/redo) or backward (undo) against the
	// live world. No stack changes. By value: CreateLight records its
	// assigned world id back into the command (redo stability).
	void ExecCmd(EditCmd& c, bool forward);
	// Do: execute + push (truncates redo) + save tour.
	void Do(EditCmd c);
	// Push an already-executed edit (gizmo/inspector live drags).
	void PushDone(EditCmd c);
	bool Undo();
	bool Redo();
	// Collapse the history to the current state: for every collapsible
	// command class (per-node transform/rename/flag/material edits,
	// per-material attr edits, per-light attr edits) keeps ONLY the last
	// occurrence — the state that survives — and drops the superseded
	// earlier duplicates plus the redo tail. Creates and terrain paint
	// stamps are structure/ordered-bake and always kept. The result
	// replays to the identical scene state with a lighter .tour. NOT
	// undoable (structural, like ClearHistory). Returns commands dropped.
	size_t CoalesceHistory();
	void ClearHistory();
	// Save-time coalesced command indices (see SaveTourTo): the saved
	// file keeps only the surviving state per class; the live stack,
	// undo and the autosave are untouched.
	std::vector<size_t> CoalescedSaveIndices() const;
	size_t UndoDepth() const;
	size_t RedoDepth() const;
	std::string UndoLabel() const;
	std::string RedoLabel() const;
	// World<->cluster id translation (single-cluster scenes).
	uint32_t m_clusterBase = UINT32_MAX; // world id of cluster node 0
	int WorldToCluster(uint32_t worldNode) const; // -1 when unmapped
	uint32_t ClusterToWorld(uint32_t clusterNode) const;
	int WorldMatToCluster(uint32_t worldMat) const; // pool idx, -1 none
	int ClusterMatToWorld(uint32_t poolIdx) const; // first world slot, -1 none
	// Undo stack (the document; saved to .tour).
	std::vector<EditCmd> m_history;
	size_t m_historyCursor = 0; // next redo index (commands[cursor..] = redo)
	static constexpr size_t kHistoryCap = 300;
	// True while a tour doc is being replayed (LoadTour/OpenTourFile):
	// ExecCreateLight records live ids into m_tourCreatedNodes then.
	bool m_replayingTour = false;
	// Live ids of history-created lights, in tour CreateLight file order:
	// backing for setLightAttr -ref addressing (TourToCmd) and the
	// save-time creation index (CmdToTour). Rebuilt on every replay.
	std::vector<uint32_t> m_tourCreatedNodes;
	// Same scheme for history-created convex volumes (tour CreateCVolume
	// file order): setCvolAttr -ref addressing. Rebuilt on every replay
	// alongside m_tourCreatedNodes.
	std::vector<uint32_t> m_tourCreatedCvolNodes;
	// Same scheme for history-created cubemap captures (tour
	// CreateCubemap file order): setCubemapAttr -ref addressing.
	// Rebuilt on every replay alongside m_tourCreatedNodes.
	std::vector<uint32_t> m_tourCreatedCubemapNodes;
	// Same scheme for history-created terrain (tour CreateTerrain file
	// order): generic verbs address them via kCreateTerrainRefBase.
	// Rebuilt on every replay alongside m_tourCreatedNodes.
	std::vector<uint32_t> m_tourCreatedTerrainNodes;
	// Same scheme for history-created cameras (tour CreateCamera file
	// order): setCamAttr -ref addressing. Rebuilt on every replay
	// alongside m_tourCreatedNodes.
	std::vector<uint32_t> m_tourCreatedCameraNodes;
	// Same scheme for history-created exportSettings nodes (tour
	// createNode -at exportSettings file order): setExportAttr -ref
	// addressing. Rebuilt on every replay alongside m_tourCreatedNodes
	// (the singleton semantic keeps it at 0 or 1 entries).
	std::vector<uint32_t> m_tourCreatedExportNodes;
	// Reference binding issues from the last scene load
	// (ApplySceneReferences collects them: unresolved sources, tour
	// parse failures, skipped commands inside a referenced .tour,
	// cycles). The Reference editor lists them; a partially-broken
	// reference never aborts the load.
	std::vector<std::string> m_refIssues;
	// Tour commands whose targets did not resolve at the last replay
	// (e.g. edits on a reference whose source changed/failed): they
	// stay OUT of the live history (nothing to execute against), but
	// SaveTour re-emits them VERBATIM — a reference round-trip must
	// never erase edits; they come alive when their nodes resolve
	// again. Rebuilt by every ReplayTourDoc.
	std::vector<smvk::tour::Command> m_orphanTourCmds;
	// World ids of DISABLED reference placeholders (subtree hidden from
	// the outliner; the nodes stay in the scene — stable ids). Rebuilt
	// by RebuildDisabledRefHides on every load and reference toggle.
	std::unordered_set<uint32_t> m_disabledRefRoots;
	void RebuildDisabledRefHides();
	// Canonical RESOLVED source per top-level reference (bind-time,
	// parallel to m_references): the model path the tour's raw source
	// string actually bound to (custom solvers/symlinks/decisions
	// visible). Empty = the source did not resolve. Rebuilt on every
	// load alongside the placeholders.
	std::vector<std::string> m_refResolved;
	// Outliner helpers: disabled reference trees are not shown/read —
	// the placeholder row stays as the re-enable anchor.
	bool InDisabledRefSubtree(uint32_t node) const;
	size_t RefSubtreeSizeForOutliner(uint32_t root) const;
	// ---- created terrain groups (live editor state) ----
	// One entry per created terrain group, keyed by the GROUP world id.
	// The global grid is authoritative: painting edits it, tile meshes
	// rebuild from it, and the placement Terrain domain samples it (the
	// preview scatters over the PAINTED surface, not the analytic one).
	struct TerrainGroup {
		smvk::placement::TerrainGroupData data;
		std::vector<uint32_t> tileNodes; // world ids, [tz * tilesX + tx]
		// True while tombstoned by a Delete command: the tour's
		// live-state CreateTerrain emission skips the group (deleted
		// content drops out of the file, like detached created lights).
		bool deleted = false;
	};
	std::unordered_map<uint32_t, TerrainGroup> m_terrainGroups;
	// GROUP world id of a group-or-tile node (UINT32_MAX when none).
	uint32_t TerrainGroupOf(uint32_t node) const;
	// Rebuilds the touched tiles' meshes from the group grid (destroy old
	// handle + upload new; world mesh indices stay stable so drawItems
	// need no churn). Returns false when a tile upload fails.
	bool RebuildTerrainTiles(uint32_t groupNode, uint32_t x0, uint32_t x1,
		uint32_t z0, uint32_t z1);
	// TerrainPaint execution (ExecCmd cases): stamp + tile rebuild.
	// Forward captures the before-patch on first execution (redos reuse).
	void ExecTerrainPaint(EditCmd& c);
	void ExecUnterrainPaint(EditCmd& c);
	// Creation-order index of a live created light (-1 when the node is
	// not a history create): the -ref value CmdToTour emits.
	int CreatedLightIndex(uint32_t worldNode) const;
	// Creation-order index of a live created convex volume, same contract.
	int CreatedCvolIndex(uint32_t worldNode) const;
	// Creation-order index of a live created cubemap capture, same
	// contract.
	int CreatedCubemapIndex(uint32_t worldNode) const;
	// Creation-order index of a live created terrain, same contract.
	int CreatedTerrainIndex(uint32_t worldNode) const;
	// Creation-order index of a live created camera, same contract.
	int CreatedCameraIndex(uint32_t worldNode) const;
	// Creation-order index of a live created exportSettings node, same
	// contract (the singleton semantic keeps it at 0 or -1).
	int CreatedExportIndex(uint32_t worldNode) const;
	// True when this history create's CmdToTour emission survives (it
	// lands in the FILE's create set): alive per family AND its recorded
	// parent slot resolves in the file (see ParentResolvesInTour). The
	// family -ref index helpers above skip non-emitting creates so the
	// save-time -ref spaces stay identical to the file's create sets —
	// the mismatch emitted dangling "setCubemapAttr -ref k" lines that
	// failed cooks ("tour: setCubemapAttr ref out of range: 0": the
	// create was dropped for an unmappable parent while its attrs and
	// renames still emitted against the missing create).
	static bool CreateCmdEmitsInTour(const EditorApp& app, const EditCmd& h);
	// A create's parent slot resolves IN THE FILE when it addresses a
	// cluster-backed node (numeric -p; the bind builds it), the world
	// root (-p 0), or an editor-created node whose OWN create emits
	// (symbolic -p "name/path"/uuid, resolvable at replay because the
	// parent's create lands earlier in file order) — recursively: a
	// deleted/tombstoned ancestor drops its whole subtree of creates.
	// Terrain tiles emit with their group's createTerrain. Recursion
	// terminates: each step walks to a strictly earlier create.
	static bool ParentResolvesInTour(const EditorApp& app, uint32_t parentSlot);
	// The parent slot a create EMITS (the LIVE parent — reparents ride
	// the create line itself, so the tour always carries the current
	// parenting and Parent verbs on created children are redundant).
	// -1 (detached) reads as the world root (0).
	static uint32_t CreateEmitParent(const EditorApp& app, const EditCmd& h);
	// True when this world node is the child of a history create (any of
	// the six create verbs): its Parent verbs never persist as Parent
	// lines (the child owns no cluster id to address one by) — when the
	// create emits, the create line carries the live parent (see
	// CreateEmitParent); when it doesn't, the child isn't in the file
	// at all. Save skips such Parent lines silently either way.
	static bool IsCreatedChild(const EditorApp& app,
		uint32_t childNode);
	// Tour-stable default parent for new lights: the primary cluster's
	// root (WorldToCluster maps it to cluster id 0), else world root.
	uint32_t DefaultCreateParent() const;
	// Unique "Point_N"/"Spot_N" name over the live world lights.
	std::string UniqueLightName(bool spot) const;
	// Unique "CVol_N" name over the live world cvolume nodes.
	std::string UniqueCvolName() const;
	// Unique "Cube_N" name over the live world cubemap captures.
	std::string UniqueCubemapName() const;
	// Unique "Camera_N" name over the live world cameras.
	std::string UniqueCameraName() const;
	// Unique "Terrain_N" name over the live world terrains.
	std::string UniqueTerrainName() const;
	// Unique copy name for duplicates (Edit > Duplicate / Ctrl+D):
	// keeps the original's prefix ("HeroCam_1" -> "HeroCam_2"; a bare
	// name gains the "_1" suffix) and finds the first free index —
	// Maya-style, so duplicates stay visually grouped in the outliner.
	std::string UniqueCopyName(const std::string& base) const;
	// Live command helpers (editor_commands.cpp).
	void ReadLocalTRS(uint32_t node, glm::vec3& t, glm::vec3& rDeg,
		glm::vec3& s) const;
	void WriteLocalTRS(uint32_t node, const glm::vec3& t,
		const glm::vec3& rDeg, const glm::vec3& s);
	// ---- scene image definitions (Maya file nodes, editor-only) ----
	// Images live OUTSIDE the DAG, like materials: pure editor state
	// (a path + sampling settings), never hierarchy nodes. A material
	// that uses one mints the cluster texture resource (with its pool
	// handle) at use time. Persistence rides the tour's editorSettings
	// (never cooked) like the placement graphs; the Images inspector
	// tab authors them (Create > Image... browses + adds).
	struct ImageDef {
		uint64_t id = 0;
		std::string name;   // display name (defaults to the path stem)
		std::string path;   // source image (filesystem path)
		int wrap = 0;       // tiling: 0 repeat, 1 clamp-edge, 2 mirrored
		int colorspace = 0; // 0 sRGB, 1 linear
		int filter = 0;     // 0 linear, 1 nearest
	};
	std::vector<ImageDef> m_images;
	uint64_t m_nextImageId = 1;
	void AddImageDef(const std::string& path); // Create > Image... (browse)
	void RemoveImageDefAt(size_t idx);         // Images tab rows
	void DrawImagesTab();                     // Inspector "Images" tab
	void SaveImagesToDoc(smvk::tour::Doc& doc) const;
	void LoadImagesFromDoc(const smvk::tour::Doc& doc);
	// Created-light execution (ExecCmd cases): append/remove the world
	// node + pool definition. ExecCreateLight records the assigned id
	// back into the command (c.node).
	void ExecCreateLight(EditCmd& c);
	void ExecUncreateLight(EditCmd& c);
	// CreateTransform execution (ExecCmd cases): append/remove a plain
	// transform node. ExecCreateTransform records the assigned id back
	// into the command (c.node).
	void ExecCreateTransform(EditCmd& c);
	void ExecUncreateTransform(EditCmd& c);
	// Delete execution (ExecCmd cases): remove/restore a node from the
	// live scene. Links (meshForNode/materialForNode/lightForNode) and
	// draw entries drop; the node id joins/leaves m_deletedNodes; ids
	// stay stable and pool resources are never reclaimed. Captured
	// state (light index, map links, draw entries at their positions)
	// rides c.number / c.delNodeMaps / c.delDrawItems for the exact
	// undo. Terrain groups cascade over their tiles.
	void ExecDelete(EditCmd& c);
	void ExecUndelete(EditCmd& c);
	// Delete key / Edit menu: delete every selected deletable node
	// (any scene node except the root, reference placeholders and the
	// World pseudo-node; terrain tiles resolve to their group), logging
	// refusals.
	void DeleteSelection();
	// Edit > Duplicate / Ctrl+D: re-emit a Create for every selected
	// duplicable node from its LIVE state (lens/params/transform), under
	// the same parent, with a unique copy name. Cluster-backed mesh
	// nodes, terrain groups/tiles and reference placeholders own no
	// create verb — skipped with a log line. The new nodes become the
	// selection (multi-duplicates select the batch).
	void DuplicateSelection();
	void ReadSharedMatAttr(uint32_t matWorld, const std::string& attr,
		glm::vec4& v4, double& num);
	void WriteSharedMatAttr(uint32_t matWorld, const std::string& attr,
		const glm::vec4& v4, double num);
	// Inspector drag coalescing (one command per drag, not per tick).
	struct PendingCmd {
		bool active = false;
		EditCmd cmd{};
	};
	PendingCmd m_pending;
	void FinalizePending(); // complete m_pending from live state + push
	bool TrackDrag(EditVerb verb, uint32_t node, const glm::mat4& beforeMat);
	void FinishDrag(); // PushDone(m_pending.cmd) + clear
	// Tour persistence (the stack IS the document).
	std::string TourPath() const; // default <stem>.tour next to the source
	std::string TourTarget() const; // live save target (save-as aware)
	bool SaveTour(); // -> current target
	bool SaveTourTo(const std::string& path, bool becomeTarget = true,
		bool allowExternal = false); // allowExternal: internal temp
		// snapshots (cook/reload) may land outside the project; user
		// saves into a foreign dir are refused (scenes live in-project)
	// becomeTarget = false: snapshot write only (cook temp tour) — the
	// live target, dirty star and autosave slot stay untouched, so the
	// editor keeps pointing at the real scene after the cook.
	bool SaveTourIncremental(); // Maya-style <stem>.NNNN.tour, becomes target
	bool LoadTour(); // replay file into the stack (undoable)
	// Per-edit autosave: the working state lands in
	// %APPDATA%/tourmalineEditor/autosave/<stem>.tour — the project's
	// .tour is only written by explicit saves (save/save-as/incremental)
	// and scene close. Recovery from an autosave is NEVER silent: a
	// newer (or orphan) autosave arms the recovery dialog instead
	// (CheckAutosaveOffer, drawn by DrawRecoverDialog).
	std::string AutosavePath() const;
	bool AutosaveTour();
	void ClearAutosave(); // drop the autosave (after an explicit save)
	std::string LatestTourPath(const std::string& tourPath) const;
	void CheckAutosaveOffer();
	void DrawRecoverDialog(); // "recover the autosave?" modal
	bool m_showRecoverDialog = false;
	bool m_recoverArmed = false; // modal open (armed by the offer check)
	std::string m_recoverAutosave; // autosave offered for recovery
	std::string m_recoverTour;     // tour file it would replace
	std::string m_recoverStamp;    // autosave timestamp (display)
	// Open an arbitrary .tour file (File menu): loads its source scene
	// when it differs, replays the commands into a fresh stack, adopts
	// the cook block. The opened file becomes the save target.
	bool OpenTourFile(const std::string& path);
	// Close the current scene (reset to empty state), saving prefs.
	void CloseScene();
	// Editor preferences: Documents/tourmalineEditor/editor.prefs
	// (resolved in Initialize). Auto-saved on Shutdown and CloseScene.
	void LoadPrefs();
	void SavePrefs();
	// Resolve a tour's source path: as-is, relative to the tour file's
	// dir, else relative to the content root. Empty when unresolvable.
	std::string ResolveTourSource(const std::string& tourPath,
		const std::string& source) const;
	// Shared replay tail (LoadTour + OpenTourFile): fingerprint check vs
	// sourcePath, cook-block adopt, history rebuild, overrides/env/refs.
	bool ReplayTourDoc(smvk::tour::Doc& doc, const std::string& sourcePath);
	// Create menu: point/spot light at the active camera target.
	void DraftCreateLight(bool spot);
	// Create menu: procedural terrain mesh. Draft params come from the
	// Create-terrain dialog (origin/size/res/height/freq/seed/tiles +
	// material); the mesh is fully procedural, so creation is exactly
	// replayable.
	struct TerrainDraft {
		std::string name = "Terrain";
		glm::vec3 origin{ 0.0f }; // node translation (= group corner)
		float size = 64.0f;
		uint32_t res = 64;        // GLOBAL grid samples per side
		float height = 8.0f;
		float freq = 0.02f;
		uint64_t seed = 1;
		uint32_t tilesX = 1;
		uint32_t tilesZ = 1;
		uint32_t material = UINT32_MAX; // world mat idx (resolved at draft)
		uint32_t parent = UINT32_MAX;   // world node (resolved at draft)
	};
	void DraftCreateTerrain(const TerrainDraft& d);
	// Created-terrain execution (ExecCmd cases): append/remove the group
	// node + tile mesh nodes + draw state. ExecCreateTerrain records the
	// assigned group id back into the command (c.node).
	void ExecCreateTerrain(EditCmd& c);
	void ExecUncreateTerrain(EditCmd& c);
	// Editor fallback material ("editor://default"): created on scene
	// load when missing, always in the pool so Create Terrain and the
	// material editor have something valid to bind. World material index.
	uint32_t EnsureDefaultMaterial();
	uint32_t m_defaultMatWorld = UINT32_MAX;
	// Create-terrain dialog state (File > Create > Terrain).
	bool m_showTerrainDialog = false;
	char m_terrainNameBuf[128] = "";
	// Export/cook tools (File > Export .pak... picks the destination in
	// the native save dialog; View > Cook & bake window hosts the tools).
	char m_cookOutPakBuf[512] = "";
	// Set by PollCookProcess when the async cook completes; the cook
	// panel shows the result line until the next cook starts.
	bool m_cookJustFinished = false;
	bool m_cookLastOk = false;
	float m_terrainOrigin[3] = { 0.0f, 0.0f, 0.0f };
	float m_terrainSize = 64.0f;
	int m_terrainRes = 64;
	float m_terrainHeight = 8.0f;
	float m_terrainFreq = 0.02f;
	int m_terrainSeed = 1;
	int m_terrainTilesX = 1;
	int m_terrainTilesZ = 1;
	uint32_t m_terrainMat = UINT32_MAX; // world material index
	void DrawCreateTerrainDialog();
	// File > Export .pak: the destination comes from the native Windows
	// save dialog, then the cook runs with that path.
	void ExportPak();
	// Floating cook/bake tools window (View menu). NOT in the bottom
	// dock: the export knobs live on the scene's exportSettings node,
	// this window only hosts the tools + progress.
	void DrawCookWindow();
	void DrawCookPanel();
	void DrawStatusBar(); // Maya-like bottom strip (task progress)
	// ---- heightfield painting ----
	// When on, LMB in a 3D viewport paints the SELECTED terrain group
	// (group or any tile selects it) instead of picking. One stamp per
	// click; drag stamps at radius-spaced intervals.
	bool m_paintTerrain = false;
	int m_paintOp = 0;            // 0 raise, 1 lower, 2 flatten, 3 smooth
	float m_paintRadius = 8.0f;
	float m_paintStrength = 0.5f;
	glm::vec2 m_paintLast{ -1e9f, -1e9f }; // world XZ of last drag stamp
	bool PaintTerrainAt(const glm::vec3& origin, const glm::vec3& dir);
	// ---- material editor (right-dock tab) ----
	// Lists materials + their textures; textures can be added as file
	// path refs (KTX2 loaded into the cluster texture pool, path kept
	// as the URI) and assigned to material slots. Layer slots ride the
	// existing "layers" shared attr.
	uint32_t m_matEditorSel = UINT32_MAX; // world material index
	char m_matTexPathBuf[512] = "";
	void DrawMaterialEditor();
	// Loads a KTX2 file into the primary cluster's texture pool with
	// the file path as its URI (+ stashes source bytes for cook).
	// Returns the RHI pool index, or -1 on failure (reason logged).
	int AddTextureFromFile(const std::string& path);
	// Light property channels ("color","intensity","range","inner",
	// "outer","position","rotation","static","shadow","name") — the
	// inspector's before-state + live-write path for SetLightAttr
	// (degrees for cones/rotation). False when the node has no light.
	bool ReadNodeLightAttr(uint32_t node, const std::string& attr,
		glm::vec4& v4, double& num, bool& flag, std::string& text) const;
	bool WriteNodeLightAttr(uint32_t node, const std::string& attr,
		const glm::vec4& v4, double num, bool flag, const std::string& text);
	// Convex-volume property channels ("priority","spacing","inside",
	// "outside","position","rotation","size","name") — the inspector's
	// before-state + live-write path for SetCvolAttr (degrees for
	// rotation, meters for triples). False when the node has no cvol.
	bool ReadNodeCvolAttr(uint32_t node, const std::string& attr,
		glm::vec4& v4, double& num, std::string& text) const;
	bool WriteNodeCvolAttr(uint32_t node, const std::string& attr,
		const glm::vec4& v4, double num, const std::string& text);
	// Cubemap-capture property channels ("position","radius","name") —
	// the inspector's before-state + live-write path for
	// SetCubemapAttr. False when the node has no capture def.
	bool ReadNodeCubemapAttr(uint32_t node, const std::string& attr,
		glm::vec4& v4, double& num, std::string& text) const;
	bool WriteNodeCubemapAttr(uint32_t node, const std::string& attr,
		const glm::vec4& v4, double num, const std::string& text);
	// World-space authoring box of a cvolume node: center = global
	// translate, half extents = 0.5 * global basis lengths, axes = the
	// NORMALIZED basis (the node's world rotation — OBB; the bake
	// rotates the six box planes with it). False when the node has
	// no cvol. CvolBox (mn/mx) = the OBB's AABB.
	bool CvolOBB(uint32_t node, glm::vec3& center, glm::mat3& axes,
		glm::vec3& half) const;
	bool CvolBox(uint32_t node, glm::vec3& mn, glm::vec3& mx) const;
	// The OBB's 8 world-space corners (drawing: the gizmo wires show
	// the rotation). False when the node has no cvol.
	bool CvolBoxCorners(uint32_t node, glm::vec3 outCorners[8]) const;
	// Inspector "Convex volume" section for cvolume nodes (priority /
	// spacing / fades via undoable SetCvolAttr; shape via the transform).
	void DrawCvolSection(uint32_t node);
	// Inspector "Cubemap capture" section for capture nodes (radius
	// via undoable SetCubemapAttr; position via the transform).
	void DrawCubemapSection(uint32_t node);
	// Camera property channels ("projection","fovY","orthoHalfH",
	// "near","far","name","position","rotation") — the inspector's
	// before-state + live-write path for SetCamAttr (degrees for
	// rotation). False when the node has no camera.
	bool ReadNodeCameraAttr(uint32_t node, const std::string& attr,
		glm::vec4& v4, double& num, std::string& text) const;
	bool WriteNodeCameraAttr(uint32_t node, const std::string& attr,
		const glm::vec4& v4, double num, const std::string& text);
	// Export-settings property channels — the SceneExportSettings
	// field names (see EditCmd::exportAttr) — the inspector's
	// before-state + live-write path for SetExportAttr. False when the
	// node is not the exportSettings node.
	bool ReadNodeExportAttr(uint32_t node, const std::string& attr,
		double& num, bool& flag, std::string& text) const;
	bool WriteNodeExportAttr(uint32_t node, const std::string& attr,
		double num, bool flag, const std::string& text);
	// Undoable one-field exportSettings edit (reads the before-state,
	// then Do()s a SetExportAttr): the inspector + programmatic toggles
	// (CookCubemapsNow) go through here.
	void PushExportAttr(uint32_t node, const std::string& attr,
		double num, bool flag, const std::string& text);
	// The scene's exportSettings node (the FIRST in node id order —
	// the singleton rule), or UINT32_MAX when the scene carries none.
	uint32_t FindExportSettingsNode() const;
	// The live export settings (the node's payload; the DEFAULTS when
	// the scene carries none — transient: EnsureExportSettingsNode
	// auto-seeds on every scene open). Every bake knob consumer reads
	// this, never session state.
	const SceneExportSettings& ExportCfg() const;
	// The AUTO-SEED (WorldSettings scaffold): every open scene carries
	// exactly one exportSettings node. Plain Do() (joins history, so
	// the save emits it like any create; undo removes it — the next
	// scene open re-seeds). No-op when one exists.
	void EnsureExportSettingsNode();
	// Inspector "Export settings" section for the exportSettings node
	// (every field via undoable SetExportAttr).
	void DrawExportSettingsSection(uint32_t node);
	// Created-exportSettings execution (ExecCmd cases): append/remove
	// the world node + the node-local SceneExportSettings payload.
	// ExecCreateExportSettings records the assigned id back into the
	// command (c.node).
	void DraftCreateExportSettings();
	void ExecCreateExportSettings(EditCmd& c);
	void ExecUncreateExportSettings(EditCmd& c);
	// Inspector "Camera" section for camera nodes (lens via undoable
	// SetCamAttr; pose via the transform + look-through).
	void DrawCamSection(uint32_t node);
	// Created-camera execution (ExecCmd cases): append/remove the world
	// node + cameras-pool definition. ExecCreateCamera records the
	// assigned id back into the command (c.node).
	void ExecCreateCamera(EditCmd& c);
	void ExecUncreateCamera(EditCmd& c);
	// Create menu: camera at the active camera target (perspective,
	// 50 deg, 0.1/1000 m).
	void DraftCreateCamera();
	// First camera node in the live world, or UINT32_MAX (World panel,
	// viewport camera bindings).
	uint32_t FindCameraNode() const;
	// Create menu: convex GI volume box at the active camera target
	// (8 m box, priority 0, 0.5 m spacing, 0.1 m fades).
	void DraftCreateCVolume();
	// Create menu: pinned cubemap capture at the active camera target
	// (influence radius 1.5 m; baked as a 'pin' row by the cook).
	void DraftCreateCubemap();
	// Created-cvolume execution (ExecCmd cases): append/remove the world
	// node + node-local SceneCvol definition. ExecCreateCVolume records
	// the assigned id back into the command (c.node).
	void ExecCreateCVolume(EditCmd& c);
	void ExecUncreateCvolume(EditCmd& c);
	// Created-cubemap execution (ExecCmd cases): append/remove the world
	// node + node-local SceneCubemap definition. ExecCreateCubemap
	// records the assigned id back into the command (c.node).
	void ExecCreateCubemap(EditCmd& c);
	void ExecUncreateCubemap(EditCmd& c);
	// Viewport boxes for cvolume nodes (editor_gizmo.cpp batches).
	bool m_showCvolGizmos = true;
	// Viewport markers for cubemap capture nodes (diamond + radius).
	bool m_showCubemapGizmos = true;
	// Viewport frustums for camera nodes (eye diamond + near/far box).
	bool m_showCameraGizmos = true;
	// App-parity probe overlay (View menu): the LOADED
	// probe rows (the manifest sidecar - see CubemapProbeRowsFromFile)
	// drawn per viewport exactly like the App's probe-location
	// overlay (dot + C<idx> + influence/fade rings, camera-distance
	// colored). LIGHTING mode only, like the rest of the light set.
	bool m_showProbeOverlay = true;
	// Octahedra marker variant for the probe overlays: the same loaded
	// rows and camera-distance color table, but each probe draws as a
	// shaded 3D octahedron instead of the flat dot (the shared
	// DrawProbeOctahedron). Applies to BOTH probe marker sets — the
	// cubemap manifest rows AND the .sggi lattice dots.
	bool m_showProbeOctahedra = false;
	// Probe-overlay visualization controls (GI probe viz window, View
	// menu): rings/labels declutter switches, marker radius in px, and
	// the distance cutoff the overlay culls at.
	bool m_probeRings = true;   // influence/fade circles per probe
	bool m_probeLabels = true;  // C<idx> text per probe
	float m_probeMarkerSize = 3.5f; // marker radius (px)
	float m_probeOverlayMaxDist = 80.0f; // overlay distance cutoff (m)
	// Drawn-count witnesses for the viz window's "in view" legend lines:
	// the last frame's marker counts (probe rows / sggi lattice), kept
	// fresh by the two overlay draw passes.
	uint32_t m_probeDrawnLast = 0;
	uint32_t m_sggiDrawnLast = 0;
	// ---- WYSIWYG LIVE VIEW (the full-graph renderer, View menu) ------
	// The App's whole frame pipeline (CSM/EVSM shadows, AO, SSR,
	// volumetrics, GI, bloom + the authored postfx grade) rendering in
	// the BOTTOM DOCK's "Live view" TAB — the honest "what the cooked
	// game will look like" reference, camera-following the ACTIVE
	// viewport, docked in the panel layout (no floating window).
	//
	// Architecture (the offscreen compositor): a real
	// engine::WorldRenderer driven through its INLINE OUTPUT seam — it
	// renders into its PRIVATE LDR target (never the editor's swap) and
	// records into the editor's command buffer between the editor's
	// two graph rounds. The tab's content rect is painted by a GPU
	// COMPOSITE PASS (the engine's UiComposite, alpha=1 = a pure copy)
	// recorded AFTER the ImGui pass — it over-draws the tab's content
	// region, so the tab content itself stays EMPTY (no
	// transparent-content-region tricks, solid chrome everywhere).
	// The inline tail releases the target to SHADER_READ in the world's
	// own graph, and the editor imports the target as an external view
	// for the composite's SetInputImages (cross-graph ordering rides
	// the context-global sync tables). The world is the EDITOR'S OWN
	// World object — edits show live, per frame; the renderer relaunches
	// on scene swap and bake completion (its GI/cubemap loaders run at
	// attach).
	smvk::mem::LinearPtr<engine::WorldRenderer> m_wysiwyg;
	bool m_showWysiwyg = false;
	bool m_wysiwygValid = false;     // onAttach succeeded (else the tab shows a hint)
	smvk::World* m_wysiwygWorld = nullptr;   // relaunch witness (the world object swapped)
	std::string m_wysiwygSceneId;    // relaunch witness (the scene stem swapped)
	bool m_wysiwygBakeWasRunning = false; // bake-completion edge (GI landed post-attach)
	// The stage rect (screen px, captured same-frame by
	// DrawLiveViewStage in DrawUI — the CENTER 4:3 panel the live view
	// became; the compose pass writes exactly it and the drive renders
	// at its size). m_wysiwygLiveRectFresh: the stage capture ran
	// this/last frame — the compose checks the current frame's flag
	// (after DrawUI), the drive consumes the previous frame's and parks
	// the world render while the view is off.
	ImVec2 m_wysiwygLiveRectMin{ 0.0f, 0.0f };
	ImVec2 m_wysiwygLiveRectMax{ 0.0f, 0.0f };
	bool m_wysiwygLiveRectFresh = false;
	ImVec2 m_wysiwygCanvasSize{ 0.0f, 0.0f }; // the ACTIVE render extent (deadband/hold-filtered, see the drive)
	// The frame-boundary quiesce flag (the user's fix — the old
	// mid-frame wait was frame-blind): set by DriveWysiwygLive on any
	// frame that recreates (or queues a recreate of) the inline target
	// — the extent drift or the swap-recreate format mirror — and
	// consumed AFTER the present in the frame pump (editor_app.cpp):
	// the frame fully submitted, nothing recording, so the wait
	// drains both timelines (the swapchain's AND the graphics
	// Submitter's that the world renderer's readback submits ride)
	// before the next frame's world chain re-ensure destroys anything.
	bool m_wysiwygBoundaryQuiesce = false;
	// The synchronized-tick park state (UpdateWysiwygBoundary ->
	// RenderWysiwygLayer): the boundary half's gate decision — the
	// layer renders only when the boundary half settled everything
	// this frame (world valid + camera perspective + the inline extent
	// settled).
	bool m_wysiwygLayerRenderable = false;
	// The world layer element instance (AddElement'd at Initialize;
	// the base Run attaches it, the base MainLoop drives its phases).
	std::shared_ptr<WorldLayer> m_worldLayer;
	// The editor-graph compose plumbing: the UiComposite builder + the
	// inline target's logical view imported into the EDITOR's graph
	// (re-imported when the target recreates; the bindless index is
	// context-global, read fresh at record time).
	smvk::UiComposite m_wysiwygComposite;
	smvk::LogicalImageViewHandle m_wysiwygComposeLogical{};
	smvk::ImageViewHandle m_wysiwygComposePhysical{}; // the view the logical import was made from (re-import on recreate)
	void LaunchWysiwyg();
	void StopWysiwyg();
	// The synchronized-tick halves (WorldLayer's phases drive these —
	// the element contract replaced the mid-frame monolithic drive):
	// the BOUNDARY half (onUpdate: BEFORE the host's acquire — all the
	// state work: lifecycle, Tick, camera, the extent settle + the
	// extent-change flag) and the LAYER half (onRender: between acquire
	// and present at the between-rounds position — the record only).
	void UpdateWysiwygBoundary(float dt);   // WorldLayer::onUpdate
	void RenderWysiwygLayer(smvk::Framebuffer& fb, smvk::Extent2D extent); // WorldLayer::onRender
	void RecordLiveViewComposePass(smvk::LogicalImageViewHandle swapView); // editor_render.cpp, after the imgui pass
	void DrawLiveViewStage(); // editor_ui.cpp (the center view panel; captures the stage rect + badge/hint)
	// TOURMALINE_LIVE_TRIANGLE=1 (the embed bisect): instead of the world
	// renderer, a MINIMAL second rendergraph — the 01_triangle sample's
	// trivial pass (a fullscreen colored triangle via the inline-HLSL
	// compiler) writing ONE persistent editor-owned image, Executed
	// between the editor's rounds exactly like the Live view drive. Zero
	// claims churn, zero world chain — isolates "a second graph's Execute
	// + mid-frame submit between the rounds" from "the heavy world
	// chain's per-extent target recreation". If the churn death
	// reproduces here too, the embed seam itself is the trigger; if not,
	// the world chain's churn is.
	smvk::mem::LinearPtr<smvk::RenderGraph> m_triangleGraph{};
	smvk::Holder<smvk::ImageHandle> m_triangleImage{};
	smvk::Holder<smvk::ImageViewHandle> m_triangleView{};
	smvk::LogicalImageViewHandle m_triangleLogical{};
	smvk::ShaderProgram m_triangleProgram{};
	smvk::Holder<smvk::ShaderModuleHandle> m_triangleVsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_triangleFsModule;
	bool m_triangleReady = false;
	bool EnsureTriangleEmbed(); // editor_render.cpp (idempotent)
	// ---- Clarisse-style overrides (editor_overrides.cpp) ----
	// Wide regex rules layered over the source asset. Precedence:
	//   source < wide rules (in order, later wins) < per-node local edits.
	// Rules skip nodes that carry a local override for the same attr, so
	// locals always win in the editor and at cook replay. Rule writes are
	// NOT history entries; deleting/disabling a rule restores captured
	// before-values only where the scene still holds the rule's value
	// (later locals are never clobbered).
	struct OverrideApplied {
		uint32_t node = UINT32_MAX;
		bool beforeFlag = false;
		uint32_t beforeMaterial = UINT32_MAX;
	};
	struct OverrideRule {
		uint32_t id = 0;
		bool enabled = true;
		smvk::tour::OverrideAttr attr = smvk::tour::OverrideAttr::Visibility;
		std::string pattern;
		bool flag = true; // Visibility/NoLightmap/Dither value
		uint32_t materialWorld = UINT32_MAX; // Material: world mat idx (UI)
		std::vector<OverrideApplied> applied; // nodes written by this rule
		std::string lastError; // regex compile error, else empty
		int lastMatches = 0; // nodes matched on last apply (pre local-skip)
	};
	bool HasLocalOverride(uint32_t node,
		smvk::tour::OverrideAttr attr) const; // history scan (cursor range)
	// Regex over node names (ECMAScript, search). Returns matched world
	// nodes; errOut set when the pattern fails to compile.
	std::vector<uint32_t> MatchOverrideNodes(const std::string& pattern,
		std::string& errOut) const;
	int CountOverrideMatches(const std::string& pattern) const; // -1 = bad regex
	bool AddOverrideRule(smvk::tour::OverrideAttr attr,
		const std::string& pattern, bool flag, uint32_t materialWorld);
	bool DeleteOverrideRule(uint32_t id);
	bool SetOverrideEnabled(uint32_t id, bool enabled);
	// Flip a bool rule's value (visibility/noLM/dither) in place.
	bool SetOverrideFlag(uint32_t id, bool flag);
	bool MoveOverrideRule(uint32_t id, int dir); // -1 up, +1 down
	bool RetargetOverrideRule(uint32_t id, const std::string& pattern);
	void ClearOverrideRules(); // unapply all, drop definitions
	void ApplyOverrideRule(OverrideRule& r); // expand + write + record
	void UnapplyOverrideRule(OverrideRule& r); // guarded restore
	void ReapplyAllOverrides(); // unapply (reverse) + apply (order)
	// Local-write bookkeeping (precedence: locals win, rules resume when
	// a local is undone). Called from Do/Undo/Redo/PushDone.
	void NoteLocalOverrideWritten(uint32_t node,
		smvk::tour::OverrideAttr attr); // rule drops ownership of node
	void ReassertWideOverrides(uint32_t node); // reapply matching rules
	// True revert: undoes + erases every local entry for node+attr, then
	// lets matching wide rules resume. The badge flips back to inherited
	// (or wide) because HasLocalOverride goes false.
	bool ClearLocalOverrides(uint32_t node, smvk::tour::OverrideAttr attr);
	// Last enabled rule of an attr matching the node name (later wins);
	// null when nothing wide applies (inspector evaluated-state badges).
	const OverrideRule* LastWideRuleFor(smvk::tour::OverrideAttr attr,
		const std::string& nodeName) const;
	void SaveOverridesToDoc(smvk::tour::Doc& doc) const;
	void LoadOverridesFromDoc(const smvk::tour::Doc& doc);
	void DrawOverridesPanel(); // wide-rule editor tab
	void DrawInspectorOverrides(uint32_t node); // per-node override section
	// New-rule form state (Overrides tab).
	char m_overridePatternBuf[256] = "";
	int m_overrideAttrSel = 0; // 0 vis, 1 material, 2 noLM, 3 dither
	bool m_overrideFlagVal = true;
	int m_overrideMatSel = 0;
	std::vector<OverrideRule> m_overrideRules;
	uint32_t m_nextOverrideId = 1;
	// Save-as dialog state.
	bool m_showSaveDialog = false;
	char m_savePathBuf[512] = "";
	std::string m_saveBrowseDir;
	std::string m_saveBrowseSel;
	std::string m_tourPath; // live target, reset to TourPath() on scene load
	void DrawSaveDialog();
	static bool CmdToTour(const EditorApp& app, const EditCmd& c,
		smvk::tour::Command& out);
	static bool TourToCmd(EditorApp& app, const smvk::tour::Command& tc,
		EditCmd& out);

	// ---- cook (editor_cook.cpp) ----
	// Full cook (quickBakes = false): stage 1 re-runs the scene's GI bake
	// IN PLACE (GiBaker — the world field + convex volumes), stage 2 runs
	// PakCooker (meshopt/LODs/lightmaps/cubemap captures + the pak write).
	// Quick bakes (quickBakes = true, the env panel's "Update bakes now"):
	// stage 1 unchanged, stage 2 runs PakCooker --quick-bakes on — the
	// cubemap capture stage ONLY, writing the loose
	// textures/cubemaps/<base>[_N].ktx2 + <base>.cubemanifest sidecars
	// (no pak write, no lightmap/LOD/collapse stages). One click refreshes
	// the GI sidecars AND the capture set; the last pak stays untouched
	// and the fresh loose sidecars override its blobs through the VFS
	// overlay.
	void CookToPak(bool quickBakes = false, bool cubemapsOnly = false); // save tour + cooker on a worker (cubemapsOnly skips GiBaker)
	void PollCookProcess();
	// TEMP tour snapshot shared by the cooker/baker subprocesses (see
	// editor_cook.cpp): outTempTour + definition fingerprint.
	bool SnapshotTourForProcess(const char* tag,
		std::string& outTempTour, std::string& outDefHash);
	// ---- bake GI on demand (editor_cook.cpp): GiBaker over the SAME
	// definition tour the cook snapshots (never part of the cook —
	// cooks stay fast and GI-free; unbaked scenes render sky IBL).
	// Sidecars key to the cook's out-pak stem; completion refreshes
	// the next open so the new bake shows immediately.
	void BakeGI();
	void PollBakeProcess();

	// ---- lightmap flat-view PNG export (Bake panel) ----
	// Renders the whole scene unwrapped by its atlas uv2 into an
	// offscreen target (atlas resolution), then reads it back and writes
	// a PNG — either the baked lightmap content or the UV coordinates
	// themselves (QA view of the parameterization), with an optional UV
	// wireframe overlay. Mirrors the App screenshot path (Capture +
	// DownloadDataFromImage).
	bool m_lmExportQueued = false;  // button sets; RenderFrame renders + saves
	bool m_lmExportWires = true;    // wireframe overlay toggle
	bool m_lmExportUv = false;      // UV coords instead of baked content
	smvk::Holder<smvk::ImageHandle> m_lmExportImage;
	smvk::Holder<smvk::ImageViewHandle> m_lmExportView;
	smvk::Holder<smvk::ImageHandle> m_lmExportDepth;
	smvk::Holder<smvk::ImageViewHandle> m_lmExportDepthView;
	smvk::LogicalImageViewHandle m_lmExportLogical{};
	smvk::LogicalImageViewHandle m_lmExportDepthLogical{};
	smvk::Extent2D m_lmExportExtent{ 0, 0 };
	smvk::Holder<smvk::BufferHandle> m_lmExportLines;
	uint32_t m_lmExportLinesCap = 0;
	uint32_t m_lmExportLinesCount = 0; // vertices drawn this export (< cap)
	void EnsureLightmapExportTarget(smvk::Extent2D extent);
	void RecordLightmapExportPass(uint32_t flight);
	void ExportLightmapPng(); // readback + stb write (after Execute)

	// ---- Clarisse-style nested scene references -------------------------
	// References are scene DEFINITION (bound at import like the source
	// itself — not undoable commands): each is a placeholder group node
	// in the owning cluster whose subtree is another scene, inlined by
	// gltf_import's ApplySceneReferences (recursing through each
	// source's own .tour, cycle-guarded). Referenced nodes are ordinary
	// nodes once inlined, so overrides (local + wide regex) apply per
	// occurrence without touching the referenced asset.
	std::vector<smvk::tour::ReferenceCmd> m_references; // master tour list
	std::vector<uint32_t> m_refPlaceholders; // world ids, parallel
	std::vector<int> m_refPhCluster; // cluster ids until the base maps
	// Material-asset POOL imports in creation order (see
	// smvk::tour::MaterialAssetCmd): asset-browser materials + the
	// editor-default seed, each minted into cluster 0's pool beyond the
	// reference-bound materials. The record order IS the pool order, so
	// the tour's assignMaterial/-m indices recorded later resolve on
	// reload/cook exactly like the authoring session. Reset with the
	// session (a fresh world bind rebuilds from the tour's records).
	std::vector<smvk::tour::MaterialAssetCmd> m_materialAssets;
	// Texture-asset POOL imports in import order (see
	// smvk::tour::TextureAssetCmd): cookie .texture files imported into
	// cluster 0's pool beyond the reference-bound textures. Replayed
	// BEFORE the material records on reload/cook, so material tex refs
	// and light cookies resolve like the authoring session. Reset with
	// the session.
	std::vector<smvk::tour::TextureAssetCmd> m_textureAssets;
	// AddSceneReference: the References PANEL path takes .tour SCENES
	// only (references are scene documents — content enters scenes as
	// ASSETS). allowGeometryAsset = the viewport DROP path (dragging a
	// derived .geometry blob onto a viewport places its content through
	// the asset-bind mechanism — the transition path until the .mesh
	// higher-asset instances land).
	bool AddSceneReference(const std::string& source, const std::string& name,
		bool allowGeometryAsset = false);
	// Reference input (asset UUID or file path) -> in-project asset:
	// absolute path + project-relative -source + asset -guid. GUIDs
	// resolve through the asset directory; out-of-project sources are
	// refused (bring the scene/derive the asset in-project first).
	// False + log on any failure. References never record absolute
	// paths — tours stay project-portable. ResolveReferenceAsset = the
	// PANEL gate (.tour only); the Ex variant exposes the drop path's
	// geometry-asset bind.
	bool ResolveReferenceAsset(const std::string& input,
		std::string& outAbs, std::string& outRel, std::string& outGuid);
	bool ResolveReferenceAssetEx(const std::string& input,
		bool allowGeometryAsset,
		std::string& outAbs, std::string& outRel, std::string& outGuid);
	// Bind-time existence probe for the panel (GUID-first, recorded
	// path second — mirrors ApplySceneReferences).
	bool RefSourceResolves(const std::string& source,
		const std::string& guid);
	bool RemoveSceneReference(uint32_t refWorldNode);
	bool RemoveSceneReferenceAt(size_t idx); // panel path (index-exact)
	// Bulk path (Delete key on several placeholders): erases all
	// indices, then persists + rebinds ONCE. Returns false when
	// nothing was removed.
	bool RemoveSceneReferencesAt(std::vector<size_t> idxs);
	bool SetSceneReferenceEnabled(size_t idx, bool enabled);
	// Reference reload WITHOUT touching the real tour file: snapshots
	// the working doc to a temp tour, rebinds through it (fresh import
	// + replay), then restores cameras/selection/save-target. A plain
	// OpenScene would replay the stale on-disk file and lose unsaved
	// edits — the temp snapshot is the reload's document instead.
	bool ReloadReferencesViaTemp();
	// Reference displacement placeholders (Maya unload stand-ins):
	// created content under a disabled reference moves to a root
	// placeholder transform (original parent's local values); re-enable
	// restores the parenting and deletes the placeholder. Placeholder
	// names ("REFPH_<ref>_<parentId>") carry the restore mapping, so
	// the dance survives save/load through plain tour commands.
	void DisplaceCreatedUnderRef(size_t refIdx);
	void RestoreRefPlaceholders(size_t refIdx); // + rebind
	bool IsRefPlaceholder(uint32_t node) const;
	// Placeholder world node for list entry idx (name match, index
	// fallback; UINT32_MAX when unbound). The index-parallel assumption
	// breaks when a bind fails, so the panel never relies on it alone.
	uint32_t RefPlaceholderNode(size_t idx) const;
	void DrawReferenceSection(uint32_t node); // Inspector block
	// Maya-style Reference Editor (editor_references.cpp): list/status
	// + add/remove/replace/reload/enable per reference.
	bool m_showReferencesPanel = false;
	void DrawReferencesPanel();

	// ---- Path editor (editor_paths.cpp) --------------------------------
	// Consolidated view over EVERY file/path the scene depends on: the
	// scene source + tour, scene references, imported image URIs (the
	// importer's data records — never a kept parsed glTF), editor image
	// definitions, cook outputs and the lightmap atlas. Missing files are
	// highlighted; textures relink LIVE (undoable material-slot
	// reassignment off the import records) AND persistently — the relink
	// is an INPUT PLUG on the source asset (plug.texture.* rows in its
	// .meta sidecar; the importer applies it at every import, so the
	// relink is viewport-, cook- and bake-safe by construction).
	bool m_showPathsPanel = false;
	void DrawPathsPanel();

	// ---- GI probe viz window (editor_ui.cpp, View menu) ----------------
	// The GI-authoring legend + control center: every probe-overlay
	// switch (cubemap rows, octahedra markers, rings, labels), the
	// size/distance knobs, and the counts the user reasons with while
	// authoring — cubemap manifest totals (level / pins / in view), the
	// .sggi lattice per-LEVEL breakdown (probe count, effective cell
	// size, lookup cells, max probes per cell = the per-cluster density
	// answer), each with its ramp swatch. LIGHTING mode only (the
	// overlays it documents are LIGHTING-gated too).
	bool m_showGiVizWindow = false;
	void DrawGiVizWindow();
	// Texture plugs of one source asset (its .meta rows, cached while the
	// panel drives them; re-read after writes).
	std::vector<smvk::cache::AssetPlug> TexturePlugsOf(
		const std::string& source);
	// Upsert one texture-relink plug (oldUri -> newPath) on the source
	// asset's .meta (adopts the asset first when it carries no identity).
	void WriteTexturePlug(const std::string& source,
		const std::string& oldUri, const std::string& newPath);
	// Drop one texture plug (persistent "forget").
	void ForgetTexturePlug(const std::string& source,
		const std::string& oldUri);
	// Interactive relink of one imported image record: loads newPath,
	// reassigns every recorded (material, slot) user via undoable
	// commands, and writes the input plug (persistent). Pool or -1.
	int RelinkTextureLive(const GltfImportImage& rec,
		const std::string& newPath);
	// Material by its registered cluster uri ("<source>#mat.N"; exact
	// match — import records carry full uris). -1 when unmapped.
	int MatWorldForUri(const std::string& uri) const;
	char m_pathSearchDir[512] = "";
	char m_pathPrefixFrom[256] = "";
	char m_pathPrefixTo[256] = "";
	int m_pathFilter = 0; // 0 all, 1 missing only

	// ---- Tourmaline branding (editor_branding.cpp) ----
	// Window title + SDL window icon (tourmaline_logo.png, generated on
	// first run if absent) + the menu-bar gem mark. The logo lives next
	// to the exe / in content and is editor chrome only.
	void ApplyTourmalineBranding();

	// ---- camera ----
	// Editor cameras live in m_editorCams (EditorCam, see above); the
	// orbit state (target/yaw/pitch/dist) is per-camera, so F framing
	// and Alt-navigation act on the ACTIVE viewport's camera.
	// Maya-style framing: F moves the orbit pivot to the selection
	// center and fits the distance; Shift+F fits the whole scene.
	bool SelectionBounds(glm::vec3& outCenter, float& outRadius);
	// MULTI-SELECT pivot: the combined world AABB center over every
	// selected node's subtree meshes (the local-pivot feature: gizmo
	// transforms act about it). Single selection = the primary's
	// subtree bounds. False = nothing to pivot on.
	bool SelectionPivotCenter(glm::vec3& outCenter);
	void FrameSelection(); // frames the ACTIVE viewport's camera
	void FrameAll();

	// ---- environment (scene definition, NEVER a render setting) ----
	// The authored look lives in the SCENE (smvk::SceneEnv, see
	// Engine/Scene/scene.h): the resolved ACTIVE block in
	// scene.environment, the default block in scene.defaultEnvironment
	// and the per-node alternatives in scene.envForNode — saved to the
	// tour with the same env.* keys as before, cooked into paks through
	// the cluster scenes, and consumed by every renderer straight from
	// the scene. The editor keeps ONLY the selection (which block
	// renders, like the active camera) and resolves it into
	// scene.environment on every change; the renderers never receive
	// the environment through any other channel.
	using EditorEnv = SceneEnv;
	uint32_t m_envCurrent = UINT32_MAX; // UINT32_MAX = default block renders

	EditorEnv& CurrentEnv();   // the source that renders (scene-backed)
	const EditorEnv& CurrentEnv() const;
	// The scene's render look (the split postfx settings): the resolved
	// block when a scene stands, defaults otherwise (sceneless-safe).
	const SceneRenderSettings& RenderLook() const;
	bool IsEnvNode(uint32_t node) const { return m_world && m_world->scene.envForNode.contains(node); }
	void MarkEnvNode(uint32_t node);     // flag node + copy current settings
	void UnmarkEnvNode(uint32_t node);
	// Resolve the current selection (default block or selected node
	// block) into scene.environment (the rendered look). Called after
	// every mutation (UI edits, mark/unmark, selection switches, tour
	// load, cooked-light seeding) — scene-direct writes land here.
	void ResolveSceneEnv();
	void ApplyEnvToFrame(engine::FrameUniforms& frame,
		smvk::EnvironmentSettings& env) const;
	// Inspector "Environment" section (World page): ONE sun — the env
	// block's (lux-native) sun rows + the scene directional light node
	// editor when one exists — plus sky/fog/exposure and the Cook now
	// button (the full cook flow from the section that owns its inputs).
	void DrawEnvPanel();
	// RENDER SETTINGS (the split postfx look — SceneRenderSettings):
	// its own section on the World page, editing the scene-global look
	// (no per-node blocks by design: a grade is never zoned).
	void DrawRenderSettingsPanel();
	// editorSet persistence (tour editorSettings; cluster-space node ids).
	void SaveEnvToDoc(smvk::tour::Doc& doc) const;
	void LoadEnvFromDoc(const smvk::tour::Doc& doc);

	// ---- default camera (world settings' TYPED field — tour
	// editorSettings camera.default, cooked into paks for the App) ----
	// The authored camera node the runtime App spawns at (replaces the
	// generic bounds framing). World node id (UINT32_MAX = none); a
	// node that loses its camera link reads as none in the combo and
	// saves as absent. Editor-created cameras address via "ref k"
	// (CreatedCameraIndex — the setCamAttr -ref family space), imported
	// glTF cameras via "node <clusterId>"; both resolve at cook
	// (ApplyTourToCluster) and at editor replay. Not undoable (world
	// settings, like the env blocks).
	uint32_t m_defaultCameraNode = UINT32_MAX;
	void SaveDefaultCameraToDoc(smvk::tour::Doc& doc) const;
	void LoadDefaultCameraFromDoc(const smvk::tour::Doc& doc);

	// ---- bake references (the .tour carries its bake) ----
	// GUIDs of the bake assets a cook minted/updated for this scene
	// (RecordBakeAssetsFromCook): the GiCook manifest asset + the cubemap
	// capture-set manifest. Saved as bake.gicook.guid / bake.cubemap.guid
	// editorSet keys (SaveEnvToDoc/LoadEnvFromDoc) so every incremental
	// tour save carries them; the assetdb + the .tour always agree on
	// which bake belongs to which definition.
	std::string m_bakeGiCookGuid;
	std::string m_bakeCubemapGuid;

	// ---- procedural placement (PlacementGraph authoring, NEVER cooked) --
	// Clarisse-style scatter graphs: domain -> fields -> accept ->
	// prototype/transform/spacing -> Layer. Compiles to the engine's
	// PlacementProgram (CPU oracle + GPU runtime); the viewport shows a
	// live CPU point preview with per-StableID trace. Persisted as
	// place.* editorSettings in the .tour, skipped by ApplyTourToCluster
	// like every other editorSet line.
	void DrawPlacementGraph();   // canvas window (View menu toggle)
	void DrawPlacementParams(placement::GraphDoc& doc, placement::PNode& n);
	void DrawPlacementOverlay(); // viewport point preview
	void SavePlacementToDoc(smvk::tour::Doc& doc) const;
	void LoadPlacementFromDoc(const smvk::tour::Doc& doc);
	void MarkPlacementDirty();
	void RefreshPlacementPreview();
	void PushPlacementUndo();      // snapshot placement doc (undo)
	void DirtyPlacementOnEdit();   // MarkPlacementDirty when active item edited
	// Extracts a world-space mesh (kept glTF primitive) for Mesh-domain
	// nodes. False for pak scenes (no CPU geometry kept) and oversized
	// meshes (>200k tris).
	bool ExtractPlacementMesh(uint32_t worldNode,
		smvk::placement::PlacementMeshData& out) const;
	placement::EditorDoc m_placement;

	// ---- source scene ----
	// m_gltfPath is the scene identity (a local file path);
	// m_gltfHostPath is the resolved file used for loading and
	// fingerprinting (equal to m_gltfPath for local files).
	std::string m_gltfPath;
	std::string m_gltfHostPath;
	std::string m_pendingGltf; // --gltf path queued before Initialize
	std::string m_prefsPath;   // Documents/tourmalineEditor/editor.prefs
	// Scene id (the open scene's stem): names the bake-asset sidecars
	// the cooks/bakes write and the bake-asset flows read back
	// (gi/<stem>.*, textures/cubemaps/<stem>.cubemanifest - see
	// SceneIdForHost and the cook/bake flows in editor_cook*).
	std::string SceneIdForHost() const;
	// ---- project (Unity/Unreal style; see editor_project.cpp) ----
	// A project folder owns the whole body of work: content/ sources,
	// paks/ cook outputs, gi/ bake sidecars, and its OWN .tourmaline/
	// cache (the Unity Library/ equivalent: cache.db + assets.db GUID
	// directory + pak/ per-class paks — no global disk, without a project
	// the cache is off). Opening a project repoints the artifact
	// cache and the cook defaults at it;
	// importing copies an asset in with a UUID (.meta sidecar). Scenes and
	// references live project-bound: references record the asset
	// UUID + project-relative path, saves stay inside the project.
	std::string m_projectRoot; // normalized, has project.tourproj
	std::string m_projectName; // display name (marker or folder name)
	// Opens (or validates) <dir> as the project: closes the live
	// scene, repoints the artifact cache at <dir>/artifacts, persists.
	// False + log when <dir> has no project.tourproj marker.
	bool OpenProject(const std::string& dir);
	// Recent projects (prefs [projects] recentN, most recent first,
	// capped): the startup project dialog lists them.
	std::vector<std::string> m_recentProjects;
	void PushRecentProject(const std::string& dir); // dedupe + cap + save
	// FORCED startup project selection (interactive runs only — the
	// smoke flows drive their own projects): a blocking modal with
	// the recents + New/Browse. The editor is project-bound; no
	// project, no work.
	bool m_projectDialogPending = false;
	void DrawProjectDialog();
	// Creates the project tree when the marker is missing, then opens
	// it (an existing project dir just opens).
	bool NewProjectOrOpen(const std::string& dir);
	// Copies a .gltf/.glb into the project's content/models/, opens
	// it (ENGINE representation ready: GPU world, streamed textures,
	// transcode cache) and targets the cook at the project's paks/.
	// Cooking the product .pak is a separate explicit step
	// (Ctrl+Shift+C) — as is Bake GI (on demand). False + log without
	// an open project, while cooking, or for foreign extensions.
	bool ImportAsset(const std::string& srcPath);
	// THE MESH IMPORTER drive (editor side; see ImportAsset +
	// editor_project.cpp): glTF is IMPORT-ONLY — derive the .geometry
	// artifact family beside the source when stale (MeshArtifactStale),
	// return the .geometry path the session references. "" = failure
	// (logged; the caller must NOT open the glTF). Every .gltf session
	// entry (opens, tour-recorded sources, drops) lands here.
	std::string ImportMeshArtifactFor(const std::string& gltfPath);
	// Asset browser (the LEFT column's "Assets" tab — moved from the
	// bottom dock in the layout rework): a GRID directory
	// explorer of the project folder (Unity Project window style):
	// breadcrumb + Up navigation, folders first, SVG icons, registered
	// asset badges (green dot = assets.db GUID), double-click opens
	// scenes / enters folders, Import + Rescan in the header. No-op
	// without a project.
	void DrawAssetsPanel();
	// Browser navigation state: the browsed directory (absolute;
	// empty/invalid/out-of-project -> <root>/content at the next draw)
	// and the selected entry (absolute, "" = none).
	std::string m_assetsDir;
	std::string m_assetsSel;
	// Collection filter (0 = all/files, 1 = meshes, 2 = materials,
	// 3 = animations, 4 = GI bakes, 5 = cubemaps, 6 = GI cooks,
	// 7 = lightmap cooks): non-zero lists every asset of that kind
	// across the project's content tree (the glTF manifests for
	// meshes/materials/animations; the assetdb bake/cook sidecar rows
	// for the rest), not the directory listing.
	int m_assetsFilter = 0;
	// Expanded glTF (absolute path): the browser shows its derived
	// assets (the manifest children) instead of the directory listing.
	// Empty = normal directory view. Unity-style: the glTF presents as
	// a FOLDER of its preprocessed assets.
	std::string m_assetsExpandGltf;
	// Expanded .tour (absolute path): the SAME folder presentation for
	// tours — the browser shows the tour's BAKE ASSETS (the cook's
	// RecordBakeAssetsFromCook refs: the GiCook manifest + the cubemap
	// capture set, resolved through the assetdb) + a back tile + the
	// tour itself as the source tile (double-click OPENS the scene —
	// the tour stays openable from the browser, nested inside its own
	// folder view). Empty = normal directory view.
	std::string m_assetsExpandTour;
	// ---- SVG icon set (editor/icons/*.svg, rasterized once) ----
	// nanosvg -> premultiplied RGBA8 textures, each registered as an
	// ImGui user texture (ImGui_ImplVulkan_AddTexture) so the asset
	// browser grid can ImDrawList::AddImage them. A failed/missing
	// icon falls back to the drawn vector shapes (DrawAssetIcon).
	enum class IconType : int {
		Folder, Pak, Model, Scene, Texture, File,
		Light, Camera, Env, GI, Character, Skeleton,
		Animation, Timeline, Settings, Count
	};
	struct IconTex {
		smvk::Holder<smvk::TextureHandle> tex; // keeps the image alive
		ImTextureID imgui = 0; // user texture id; 0 = shape fallback
	};
	IconTex m_icons[size_t(IconType::Count)];
	bool m_iconsReady = false;
	bool EnsureSvgIcons(); // rasterize + register (once; ImGui must be up)
	IconType IconTypeFor(const std::string& name, bool dir) const;
	// Fallback icon art (drawn shapes) when an SVG icon is unavailable.
	void DrawAssetIconFallback(ImDrawList* dl, const ImVec2& org,
		float size, IconType k) const;
	// Windows explorer /select reveal (no-op + log on failure).
	void RevealInExplorer(const std::string& absPath);
	// MATERIAL ASSET drop (browser -> viewport): loads the .material
	// file, creates a cluster material (values + textures from its
	// refs, loaded from the files beside it) and assigns it to the
	// SELECTED subtree (undoable AssignMaterial). Nothing selected =
	// log + no-op.
	void AssignMaterialAsset(const std::string& assetPath);
	// COOKIE .texture import (spot-light inspector): the cookie ref's
	// .texture lands in cluster 0's pool NOW (the live view's light
	// build resolves it) + a textureAsset record rides the tour (the
	// cook replays it before the command list). Empty = nothing to
	// import; non-.texture paths are refused with a log (raw images are
	// not cluster-registered — import them as .texture first). No-op
	// during tour replay (the tour's records replay there).
	void ImportCookieTexture(const std::string& cookieRef);
	// GUID of a derived asset file (its glTF manifest entry; "" when
	// unknown). Scans the sibling .gltf.meta manifests.
	std::string DerivedAssetGuid(const std::string& assetPath);
	// Read-only inspector page for a browser-selected material/mesh
	// ASSET (view parameters only; edits happen on scenes). Draws into
	// the Inspector when m_assetsSel is a .material/.geometry file; empty
	// selection draws nothing.
	void DrawAssetReadonlyPage();
	// IMPORTABLE-SOURCE page (a raw image selected in the browser, not
	// yet imported): probe + the "Import as .texture" button (the wrap
	// carries the import settings from here on).
	void DrawImportableTexturePage();
	// Asset inspection cache (loaded on selection change, never per
	// frame): the inspected file + its loaded content.
	std::string m_assetInspectPath;
	std::optional<smvk::assets::MaterialAsset> m_assetInspectMat;
	std::optional<smvk::assets::MeshAsset> m_assetInspectMesh;
	std::optional<smvk::cook::GiCookAsset> m_assetInspectGiCook;
	std::optional<smvk::cook::LightmapCookAsset> m_assetInspectLmCook;
	std::optional<smvk::cook::CubemapAsset> m_assetInspectCubemap;
	std::optional<smvk::cook::GicvolumeAsset> m_assetInspectGicvolume;
	// .texture asset page: the parsed asset + the probed SOURCE header
	// (read-only facts for KTX2 sources; LDR sources edit the settings).
	std::optional<smvk::assets::TextureAsset> m_assetInspectTex;
	TextureSourceInfo m_assetInspectTexInfo{};
	bool m_assetInspectTexInfoOk = false;
	// The copy+adopt half of ImportAsset (no scene switch, no cook
	// priming): copies a .gltf/.glb + companions into the project's
	// content/models/<stem>/, adopts its GUID (.meta sidecar + asset
	// db) and returns the in-project asset path ({} + log on failure).
	// The add-reference flow uses it to auto-import out-of-project
	// sources — scenes only ever reference in-project assets.
	std::string ImportAssetFile(const std::string& srcPath);
	// Cook-asset creation (INTERNAL: not in any menu — .gicook/.lmcook
	// are a consequence of baking, minted/updated per scene by
	// RecordBakeAssetsFromCook; this helper stays for programmatic use).
	// Writes a fresh UNBAKED manifest (<dir>/NewGiCook<N>.gicook
	// or NewLightmapCook<N>.lmcook — settings from the cook defaults,
	// scene seeded from the open scene when one exists, empty blob),
	// adopts its GUID (.meta sidecar + assetdb row kind gicook/lmcook),
	// selects it in the browser. Needs a project only (no open scene) —
	// like ImportAsset, unlike the scene-node Create entries. Returns
	// the new path ({} + log on failure).
	std::string CreateCookAsset(bool gi, const std::string& dir);
	// Typed GI product assets (content/gi/ folder): fresh UNBAKED
	// .cubemap / .gicvolume manifests, adopted into the registry
	// (assetdb kinds cubemap/gicvolume). No loose sidecars — the bake
	// payload path rides blobPath inside the manifest.
	std::string CreateCubemapAsset(const std::string& dir);
	std::string CreateGicvolumeAsset(const std::string& dir,
		uint32_t node = UINT32_MAX);
	// TEXTURE-ASSET creation (the Asset browser's New button + context
	// menu): copies the picked source image (png/jpg/jpeg/tga/bmp/ktx2)
	// into the browsed dir, writes <stem>.texture (import settings — see
	// smvk::assets::TextureAsset: colorSpace, format, mipLevels, filter,
	// wrap) + its .meta identity sidecar, adopts the GUID into the
	// assetdb (kind texture), selects it in the browser. Default
	// settings: sRGB, rgba8, full chain, linear/repeat — edit them in
	// the Inspector (KTX2 sources show colorSpace/format/mipLevels
	// READ-ONLY: the .ktx2 header pre-decided them; a jpg/png source
	// converts with them at the next import/cook). Returns the new path
	// ({} + log on failure).
	std::string CreateTextureAsset(const std::string& srcPath,
		const std::string& dir);
	// OS DRAG-AND-DROP (Windows Explorer -> the asset browser): the
	// backend-neutral hook (App::OnInputEvent override, below) collects
	// input::EventType::FileDrop batches + the drop point (window coords);
	// DrawAssetsPanel routes them against its rect (the only drop target)
	// into HandleDroppedFiles — images become .texture assets in the
	// browsed dir, .gltf/.glb run the full import.
	void HandleDroppedFiles(const std::vector<std::string>& files);
	void OnInputEvent(const input::Event& e) override;
	std::vector<std::string> m_pendingDropFiles;
	float m_pendingDropX = -1.0f; // < 0 = no pending drop
	float m_pendingDropY = -1.0f;
	// Cook-completion bookkeeping (PollCookProcess on a successful cook):
	// the cook's bake outputs become/refresh BAKE ASSETS tied to this
	// scene — the GiCook manifest (settings + the fresh .sggi result
	// pointer) adopted into the project assetdb, the cubemap capture-set
	// manifest adopted when cubemaps baked — and the bake GUID references
	// land in the tour (m_bakeGiCookGuid / m_bakeCubemapGuid, editorSet
	// keys — every incremental tour save carries them). No-op without a
	// scene; logs what it recorded.
	void RecordBakeAssetsFromCook();
	// Artifact cache root (empty when no project is open). The project
	// owns its artifact cache: cache.db (processed-import registry)
	// + assets.db (GUID directory) + pak/ (per-class paks).
	std::string ProjectArtifactDir() const;
	// True when the absolute path lives inside the open project (false
	// without a project). References and scene saves require it.
	bool InProjectDir(const std::string& absPath) const;
	// Project-relative posix path ("content/models/...") for an
	// absolute path ({} when outside the project). Tour reference
	// -source slots carry this form — never absolute paths.
	std::string ProjectRelativePath(const std::string& absPath) const;
	// Records a project source file in the asset directory (reads the
	// sibling .meta, issuing + writing one when missing/invalid).
	// Silent for known assets; logs issuance. No-op without a project.
	void AdoptProjectAsset(const std::string& assetPath,
		const std::string& importer = "model");
	// Asset GUID for a source file (adopts first, so this issues the
	// sidecar on demand). "" without a project or for unknown files —
	// callers store it alongside the path (tour reference -guid) and
	// keep working path-only when empty.
	std::string ProjectAssetGuid(const std::string& assetPath);
	// Asset-browser delete: validates the selection (project
	// containment, not the open scene, planable) and opens the confirm
	// modal (the plan rides m_assetsDeletePending). The modal itself
	// draws + executes in DrawAssetDeleteModal (end of DrawAssetsPanel).
	void RequestAssetDelete(const std::string& absPath);
	void DrawAssetDeleteModal();
	// Pending confirmed-delete plan ("" target = no modal).
	AssetDeletePlan m_assetsDeletePending;
	// <root>/paks/<stem>.pak (empty without a project/stem).
	std::string ProjectCookOutPak(const std::string& sceneStem) const;
	// Points the per-scene cook outputs (direct + dialog) at the
	// project when the user hasn't customized them.
	void PrimeProjectCookDefaults();
	static bool IsProjectDir(const std::string& dir);
	static std::string ReadProjectName(const std::string& dir);
	// ---- project artifact cache (no global disk) ----
	// The project OWNS its artifact cache (<root>/.tourmaline/: cache.db
	// SQLite registry + pak/ per-class paks + assets.db GUID
	// directory). There is no global/user registry: without a project
	// the cache is off (cooks/imports still work — the cache only
	// accelerates processing, never gates it).
	smvk::cache::CacheDb m_cacheDb;
	std::unique_ptr<smvk::cache::ArtifactStore> m_store; // null = cache off
	// Source-asset directory (GUID<->path over the project, Unity
	// SourceAssetDB analog). Open while a project is open with its
	// cache online; closed otherwise (global runs stay path-based).
	smvk::cache::AssetDatabase m_assetDb;
	// GltfGuidResolver over m_assetDb (gltf_import hook): tour
	// references carrying -guid resolve identity-first — moves/
	// renames heal through rescan + the .meta sidecars. Closed db
	// answers "" (path-only fallback, exactly the pre-GUID behavior).
	struct GuidResolverHook final : public GltfGuidResolver {
		EditorApp& app;
		explicit GuidResolverHook(EditorApp& a) : app(a) {}
		std::string PathForGuid(const std::string& guid) override;
	};
	GuidResolverHook m_guidResolver; // init: EditorApp ctor (holds *this)
	// gltf_import hook over the store (texture-transcode
	// acceleration; injected into every import path).
	std::unique_ptr<StoreArtifactCacheAdapter> m_texCacheAdapter;
	// Opens (creating when missing) the PROJECT's artifact dir +
	// cache.db. False when no project is open (cache stays off); the
	// editor runs fine without a db.
	bool OpenCacheDb();
	// Editor VFS: the overlay the registry class paks mount into.
	// ONLY registry ever mounts here — NEVER the release content tree:
	// editor -> release is one-way (forwards). The release pak
	// (content/paks) is a pure output; reading it back would feed
	// stale cooked content into authoring. The artifact store's
	// per-class paks mount as "registry-<class>" @20.
	smvk::OverlayVfs m_editorVfs;
	// Cook definition fingerprint (the scene artifact's cache "source"
	// key): the temp tour bytes with the volatile "//Last modified:"
	// header line stripped — deterministic per definition content.
	std::string m_cookDefHash;
	// ---- lightmap atlas inspection (.sglm, cook output) ----
	smvk::gi::SgLightmapAtlas m_sglm;
	std::string m_sglmPath;
	bool m_hasSglm = false;
	std::string m_sglmStatus = "no atlas loaded";
	char m_sglmBuf[512] = "";
	// ---- baked-irradiance GPU upload (App parity: 9 RGBA16F stream
	// layers, or the sglm v3 BC6H one-per-lobe textures)
	// Feeds FrameUniforms.lmLayers/lmLayerKind/lmEnabled for the F8 view;
	// released with the atlas (scene change / manual clear).
	smvk::Holder<smvk::TextureHandle> m_lmLayers[12];
	uint32_t m_lmLayerSlots[12] = {};
	uint32_t m_lmLayerKind = 0;
	bool m_lmGpu = false;
	void UploadSglmLayers();
	void ClearSglmLayers();
	// ---- diffuse preview cache (single entry; cleared on scene load) ----
	TexPreview m_texPreview;
	// ---- importer data (see gltf_import's GltfImportImage): one row per
	// referenced image across the definition's imports (loaded AND failed),
	// collected by ApplySceneReferences. THE replacement for the old kept
	// tinygltf::Model: the path editor's texture rows + the live relink
	// read this, never a parsed glTF. Cleared with the world.
	std::vector<GltfImportImage> m_importImages;

	// ---- editor state ----
	// View modes are PER VIEWPORT (m_viewports[].viewMode; the F-keys
	// and View menu act on the ACTIVE one — UV views are panel types).
	SecondView m_secondView = SecondView::Off;
	GizmoMode m_gizmoMode = GizmoMode::Translate;
	// Q quick menu options: handle orientation (World default) +
	// pivot (Node origin default; BoundsCenter = the selection's
	// subtree AABB center).
	GizmoSpace m_gizmoSpace = GizmoSpace::World;
	GizmoPivot m_gizmoPivot = GizmoPivot::Node;
	// The gizmo's world pivot point per m_gizmoPivot (BoundsCenter
	// falls back to the node origin when the selection has no mesh
	// bounds). Valid while a selection exists.
	glm::vec3 GizmoPivotPoint();
	uint32_t m_selected = UINT32_MAX; // world node id (primary selection)
	// Deleted nodes (Maya delete, EditVerb::Delete): links dropped from
	// the live scene, ids kept (no renumbering). Drives the outliner
	// filter; undo/redo and tour replay add/remove entries in lockstep
	// with ExecDelete/ExecUndelete. Cleared with the world (import /
	// CloseScene reset it) and rebuilt by Delete commands at replay.
	std::unordered_set<uint32_t> m_deletedNodes;
	uint32_t m_revealNode = UINT32_MAX; // outliner reveal request (viewport pick)
	bool m_wireOverlay = true;
	bool m_uiMode = true; // false = fullscreen viewport (camera control)
	// Tour dirty flag (window-title "*" + save prompts): set whenever
	// the working tour doc moves ahead of the last explicit save
	// (AutosaveTour fires exactly on those paths), cleared by explicit
	// saves, clean loads and scene close.
	bool m_tourDirty = false;
	std::string m_lastWindowTitle; // change-detected SDL title cache
	void UpdateWindowTitle(); // "Tourmaline Editor — <scene> [*]"
	// True while the focused ImGui window sits inside the Placement Graph
	// window hierarchy (drawn last frame): ModalDelete defers to the
	// graph's own Del handler (node/wire removal) instead of the world
	// selection. Updated by DrawPlacementGraph, reset in DrawUI.
	bool m_imguiFocusInPlacementGraph = false;
	bool m_showLightGizmos = true; // viewport markers for light nodes
	// ---- convex GI volume preview (LIGHTING mode, 'V' cycles; loaded
	// from the .cvmanifest next to the opened pak) ----
	// 0 = off, 1 = wireframe, 2 = wireframe + translucent fill.
	// Color-coded by PRIORITY (hue = priority * 0.13 mod 1) so stacked
	// override volumes read at a glance.
	int m_cvolPreviewMode = 0;
	struct CvolPreview {
		std::string name;    // sidecar file stem
		uint32_t priority = 0;
		glm::vec3 gridMin{ 0.0f }; // bake AABB (gridMin..gridMax)
		glm::vec3 gridMax{ 0.0f };
		glm::vec3 center{ 0.0f }; // hull centroid (label anchor)
		std::vector<std::vector<glm::vec3>> polys; // convex hull polygons
	};
	std::vector<CvolPreview> m_cvolPreviews;
	void DrawCvolPreviewOverlay(); // editor_viewports.cpp (ImGui foreground)
	// Capture balls + App-parity probe overlay (editor_viewports.cpp,
	// ImGui foreground, LIGHTING mode only): the scene's LOADED probe
	// rows (the cubemap manifest sidecar - see CubemapProbeRowsFromFile)
	// drawn exactly like the App's probe-location overlay (dot + C<idx>
	// + influence/fade rings, camera-distance colored), plus black balls
	// for authored pins with no loaded probe behind them ("nothing
	// rendered here yet"). Honors m_showProbeOverlay (loaded rows) and
	// m_showCubemapGizmos (pins); m_showProbeOctahedra swaps the dot for
	// a shaded octahedron marker (same rows, same colors).
	void DrawCaptureBallOverlay();
	// One loaded probe row (the manifest sidecar's placement half).
	struct CubemapProbeRow {
		glm::vec3 center{ 0.0f };
		float influence = 0.0f; // full-weight radius (m)
		float fade = 0.0f;      // fade extent past influence (m)
	};
	// Loaded probe rows parsed straight from the scene's cubemap manifest
	// sidecar (gi/cubemaps/<stem>.cubemanifest - the same file the
	// renderer loads at runtime), read under the project root first, then
	// the bundled content tree. Empty (no manifest / no scene) = no
	// loaded probes: the census reads all pins unbaked and the overlay
	// draws the authored pins only.
	bool CubemapProbeRowsFromFile(std::vector<CubemapProbeRow>& out) const;
	// Baked/unbaked census for the authored pins (overlay + cook dialog
	// share it): pins within kCaptureBakeMatchM of a loaded probe
	// count as baked. No probes = 0 baked.
	static constexpr float kCaptureBakeMatchM = 2.0f;
	void CaptureBakeState(size_t& bakedOut, size_t& totalOut);
	// ---- octree PRECOOK visualization (GI authoring overlay) ----
	// Fast authored overlay (no baker run): the world-field bounds tile
	// grid + authored cvol boxes (capture balls ride the dedicated
	// DrawCaptureBallOverlay above so the two never double up).
	// Tiles split the scene AABB at the root-brick size; every tile
	// starts dirty, a bake cleans them, any scene edit dirties them
	// again (partial-update model: only dirty tiles need rebaking).
	// "Update" recomputes the grid; "auto" recomputes on scene change
	// and capture clicks select + refresh when auto is on.
	bool m_octreePreviewOn = false;
	bool m_octreeAutoUpdate = false;
	float m_octreeTileSize = 8.0f; // root-brick size (m)
	struct OctreeTile {
		glm::vec3 mn{ 0.0f };
		glm::vec3 mx{ 0.0f };
		bool dirty = true;
	};
	std::vector<OctreeTile> m_octreeTiles;
	glm::vec3 m_octreeMn{ 0.0f };
	glm::vec3 m_octreeMx{ 0.0f };
	void UpdateOctreePreview(); // recompute tiles from the live scene
	void DrawOctreePreviewOverlay(); // wire tiles + world bounds
	void MarkOctreeDirty(); // scene edit -> all tiles dirty
	// ---- bake-DENSITY visualization (LIGHTING GI authoring overlays) ----
	// Two density views answering "do the bake density settings fit the
	// scene" WITHOUT baking: the cubemap probe set the current cook
	// settings would place (octree placement, or the 8 m grid mirror
	// when the octree is off — same code + same BVH occlusion as the
	// cooker), and the baked .sggi world-field probe lattice (per-level
	// density + effective cell sizes). Both draw in the ImGui foreground
	// per viewport (no depth test, like the App's probe overlays).
	// Refresh is MANUAL (GI panel buttons, same contract as "Update
	// precook") plus auto-refresh when a mode is toggled on with an
	// empty cache.
	bool m_cubemapDensityOn = false;
	struct CubemapDensityProbe {
		glm::vec3 center{ 0.0f };
		float influence = 10.0f; // full-weight radius (m)
		float fade = 0.0f;       // fade extent past influence (m)
		uint32_t depth = 0;      // octree emitting depth (grid = 0)
	};
	std::vector<CubemapDensityProbe> m_cubemapDensity; // prediction cache
	smvk::gi::TriangleBvh m_cubemapDensityBvh; // occlusion source (empty = no cull)
	std::string m_cubemapDensityInfo;  // one-line status for the GI panel
	std::string m_cubemapDensityScene; // scene stem the cache was built for
	void RefreshCubemapDensity(); // rebuild BVH + placement from m_cook
	bool m_sggiDensityOn = false;
	smvk::gi::SgGiVolume m_sggiDensity; // last loaded .sggi (CPU only)
	std::string m_sggiDensityInfo;      // probes/levels/cells readout
	std::string m_sggiDensityPath;      // loaded file (staleness witness)
	float m_sggiDensityMaxDist = 80.0f; // overlay distance cutoff (m)
	void RefreshSggiDensity(); // (re)load gi/<stem>.sggi by binding/stem
	void DrawBakeDensityOverlay(); // ImGui foreground markers + captions
	bool m_wireframeAll = false; // Z: whole-scene wireframe (wire program)
	int m_debugMeshletMode = 0; // F4 color mode: 0 random, 1 cone heat
	// ---- panel sizes (drag the splitters to resize; constants above
	// are the defaults) ----
	float m_outlinerW = kEditorLeftW;
	// The stacked left column's SPLIT: the Outliner (always visible, the
	// upper window) owns this much height; the tabbed browser (Assets /
	// Console / Overrides) fills the rest below. Absolute px like the
	// dock-era height (clamped at draw time against the column height —
	// the window can resize under the saved value).
	float m_outlinerH = 380.0f;
	float m_inspectorW = kEditorRightW;
	float m_bottomDockH = kEditorBottomH;
	// The bottom dock's EFFECTIVE height: ZERO since the dock was
	// RETIRED (the layout rework's final form — the Live view moved
	// into the CENTER view panel, the authoring tabs into the left
	// column, so nothing hosts the strip anymore). The helper + the
	// m_bottomDockH member + the [bottomDock] prefs keys stay wired so
	// a future output strip can re-host the layout slot without
	// touching every consumer again; until then every layout reader
	// (panel heights, viewport rects, splitters) goes through this and
	// gets 0.
	float BottomDockH() const { return 0.0f; }
	void DrawPanelSplitters(); // left/right/bottom resize handles
	// ---- ground grid + snapping (gizmo) ----
	bool m_showGrid = true;
	float m_gridSize = 1.0f;   // cell size in meters (snap step too)
	uint32_t m_gridExtent = 40; // half-extent in cells (both axes)
	// Gizmo screen-size factor (fraction of the camera distance).
	float m_gizmoScale = 0.07f; // default: half the original 0.14
	// Shift-drag snapping: translate steps to m_gridSize, rotate to
	// m_snapRotateDeg, scale to m_snapScaleStep.
	float m_snapRotateDeg = 15.0f;
	float m_snapScaleStep = 0.1f;
	// ---- material focus (Inspector navigation) ----
	// When set, the Inspector shows a MATERIAL EDITOR page for this
	// world material (layers included) instead of the node page;
	// m_materialTrail is the back-stack ("travel between inspectors").
	uint32_t m_materialFocus = UINT32_MAX;
	std::vector<uint32_t> m_materialTrail;
	// Central viewport rect this frame (single layout; the quad layout
	// fills m_viewports[].rect — m_vp stays == m_viewports[active].rect).
	ViewportRect m_vp{};
	// Menu-bar height, cached from the last ImGui frame (ComputeViewportRect
	// runs BEFORE ImGui::NewFrame, where querying ImGui is illegal).
	float m_menuH = 19.0f;

	// ---- sun/ambient (viewport lighting) ----
	// ---- viewport lighting (see SceneEnv: the live values live in the
	// scene — CurrentEnv() reads the default block or the selected node
	// block; RebuildCookedLights seeds the scene default from the cooked
	// directional sun when the scene has one, else the defaults stand in).
	// ---- cooked scene lights (KHR punctual, glTF import or pak) ----
	// First directional wins as the viewport sun (App parity). Aim is the
	// node's G*(0,0,-1): the glTF -Z convention — raw locals (the
	// coordFix basis rides the imported chain's top node; see
	// GltfPunctualAimAxis). Points/spots are counted and
	// logged; the editor has no forward+ path, so only the sun renders.
	bool m_hasCookedSun = false;
	std::string m_cookedSunName;
	uint32_t m_cookedDirs = 0, m_cookedPoints = 0, m_cookedSpots = 0;
	uint32_t m_cookedStaticSkipped = 0;
	void RebuildCookedLights(); // + seeds the scene default's sun from the sun
	const ::SceneLight* FindNodeLight(uint32_t node,
		glm::vec3* outAimWs = nullptr) const; // + world aim for dir/spot
	// Definition for a camera node (null when the node has none).
	// Resolves through world cameraLocations -> cluster cameras pool.
	const ::SceneCamera* FindNodeCamera(uint32_t node) const;

	// ---- shading resources ----
	// Frame ring holds one slice PER (flight, viewport): frameUniforms is
	// sized frames * kMaxViewports * sizeof(Frame); each viewport's addr
	// is stored in m_viewports[i].frameAddr.
	smvk::Holder<smvk::BufferHandle> m_frameUniforms; // ring: frames * kMaxViewports * sizeof(Frame)
	smvk::Holder<smvk::BufferHandle> m_insetFrameUniforms; // second viewport (strip aspect)
	uint64_t m_lastInsetFrameAddr = 0; // current flight's inset slice
	// FramePSHot hot slab (the FS reads it UN-GATED at the top of mainPS via
	// FrameUniforms::frameHot — default_lit_body.hlsli:354): the lit FS
	// dereferences the BDA before anything else, so every frame uniform the
	// editor uploads MUST carry a valid slab address — a zero-init frame
	// left it null and the first world-drawing frame took the device down
	// with an MMU read at 0x0 (Aftermath-confirmed). Mirrors the App
	// renderer's m_resources.frameHot (see UpdateFrameUniforms).
	smvk::Holder<smvk::BufferHandle> m_frameHotBuf; // ring: frames * kMaxViewports * sizeof(FramePSHot)
	smvk::Holder<smvk::BufferHandle> m_materialsBuf;  // ring: frames * MAX_MATERIALS
	smvk::Holder<smvk::BufferHandle> m_lightsBuf;     // ring: frames * MAX_LIGHTS
	smvk::Holder<smvk::BufferHandle> m_cascadeBuf;    // ring: frames * GpuDirDesc
	// Scene-wide lightmap chart transforms (see World::lmChartTable):
	// uploaded on scene version change; the flat lightmap view + PNG
	// export index it per mesh via the uvflat VS tail push.
	smvk::Holder<smvk::BufferHandle> m_lmXformsBuf;
	uint64_t m_lmXformsAddr = 0;
	uint64_t m_lmXformsVersion = ~uint64_t(0);
	size_t m_lmXformsCap = 0;
	uint64_t EnsureChartXforms();
	smvk::Holder<smvk::ShaderModuleHandle> m_vsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_unlitFsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_litFsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_wireFsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_uv0FsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_uv1FsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_vcolorFsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_bakedFsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_meshDebugVsModule; // F4 view
	smvk::Holder<smvk::ShaderModuleHandle> m_meshDebugFsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_flatUv0VsModule; // Flat UV views
	smvk::Holder<smvk::ShaderModuleHandle> m_flatUv0FsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_flatUv1VsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_flatUv1FsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_flatUvCoordFsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_lineVsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_lineFsModule;
	smvk::ShaderProgram m_unlitProgram{};
	smvk::ShaderProgram m_litProgram{};
	smvk::ShaderProgram m_uv0Program{}; // UV0 debug (falls back to Lit)
	smvk::ShaderProgram m_uv1Program{}; // lightmap-UV debug (falls back to Lit)
	smvk::ShaderProgram m_vcolorProgram{}; // vertex-color debug (falls back to Lit)
	smvk::ShaderProgram m_bakedProgram{}; // baked-irradiance debug (falls back to Lit)
	smvk::ShaderProgram m_meshDebugProgram{}; // F4 meshlet view (meshdebug vert+frag)
	smvk::ShaderProgram m_flatUv0Program{}; // Flat UV: uvflat0.vert + uvflat0.frag
	smvk::ShaderProgram m_flatUv1Program{}; // Flat UV: uvflat1.vert + uvflat1.frag
	smvk::ShaderProgram m_flatUvCoordProgram{}; // export only: uvflat1.vert + uvflat1uv.frag
	smvk::ShaderProgram m_wireProgram{}; // selected highlight overlay
	smvk::ShaderProgram m_lineProgram{}; // gizmo/AABB lines
	smvk::DepthTarget m_depth{};
	// Preview HDR + tonemapper (the "washed out colors" fix): the editor
	// used to write the tonemap-chain shaders' RAW output straight to the
	// LDR swapchain - the lit body emits raw-lux HDR radiance (sun 10000
	// lux -> albedo*E/pi is ~3x albedo), so everything above 1.0 crushed
	// to flat white; and the 1/exposure-pre-scaled debug views (unlit/uv/
	// vertexcolor/wire, see their shader headers) would crush to black
	// the moment frame.exposure left 1.0. The preview now runs the App's
	// display chain: the scene passes write a shared FULL-WINDOW HDR at
	// the SAME offset/extent/depth rect discipline as the direct path
	// (the gizmo grid keeps its depth-tested occlusion against the scene
	// depth), a full-window clear pass blacks it (UV-shell gaps never
	// show a shrunken viewport's stale rect), then ONE TONEMAP LAYER PER
	// VIEWPORT RECT resolves it to the swap (the nvapp element-layer
	// pattern: AddTonemapPass offset/extent + Load + the srcUv sub-rect
	// window). Exposure: the authored MANUAL value (the App meters +
	// adapts at runtime - it seeds its adaptation from the same number -
	// the preview can't meter); operator: the authored tonemapMode.
	// Baked + the UV window canvas keep their own Reinhard + gamma
	// mapping and stay direct. Creation failure (HDR or tonemap SPVs)
	// falls back to the old direct write (washed out, never dead).
	smvk::HdrTarget m_sceneHdr{};
	smvk::Tonemapper m_previewTonemap{};
	std::vector<smvk::LogicalImageViewHandle> m_swapViews;
	uint32_t m_flightCount = 0;
	engine::FrameUniforms m_lastFrame{}; // CPU copy for gizmo/pick math
	glm::mat4 m_lastViewProj{ 1.0f };

	// per-frame upload caches
	uint64_t m_frameCounter = 0;

	// ---- gizmo drag state ----
	bool m_gizmoDragging = false;
	int m_gizmoAxis = -1; // 0/1/2 = X/Y/Z axes; 3/4/5 = YZ/XZ/XY planes
	                      // (translate/scale two-axis handles); 6 =
	                      // rotate arcball (free rotation inside the sphere)
	glm::vec3 m_dragGrabPoint{ 0.0f };
	glm::vec3 m_dragNodeOrigin{ 0.0f };
	// Pivot (Q quick menu, GizmoPivot::BoundsCenter): the drag-start
	// pivot in the NODE's local frame + whether it differs from the node
	// origin (rotate/scale compensate the translate about it; zero +
	// false = no compensation, the node-origin behavior).
	glm::vec3 m_dragPivotL{ 0.0f };
	bool m_dragHasPivot = false;
	// MULTI-SELECT drag snapshots (every selected node except the
	// primary — Maya-style multi-transform): the same WORLD transform
	// applies to each (translate delta, rotation delta or uniform
	// scale ratio), converted per node into its parent frame with its
	// own pivot compensation. Per-axis scale stays single-select.
	struct GizmoMultiSnap {
		uint32_t node = 0;
		glm::mat4 baseMat{ 1.0f };
		glm::vec3 t{ 0.0f };
		glm::vec3 r{ 0.0f };
		glm::vec3 s{ 1.0f };
		glm::mat4 parentWorld{ 1.0f }; // the node's parent world matrix
		glm::vec3 pivotP{ 0.0f };      // the drag pivot in the parent frame
	};
	std::vector<GizmoMultiSnap> m_dragMulti;
	bool m_dragMultiLogged = false; // the per-axis unsupported note fires once
	void ApplyMultiDragTranslate();
	void ApplyMultiDragRotate();
	void ApplyMultiDragScaleUniform(float ratio);
	glm::mat4 m_dragBaseMat{ 1.0f }; // baseline local matrix (undo)
	glm::vec3 m_dragBaseT{ 0.0f };
	glm::vec3 m_dragBaseR{ 0.0f };
	glm::vec3 m_dragBaseS{ 1.0f };
	glm::vec3 m_dragCurT{ 0.0f }; // live triple during the drag
	glm::vec3 m_dragCurR{ 0.0f };
	glm::vec3 m_dragCurS{ 1.0f };
	glm::vec3 m_dragPlaneN{ 0.0f, 1.0f, 0.0f };
	float m_dragLastX = 0.0f;
	float m_dragLastY = 0.0f; // arcball previous mouse (viewport-local)
	glm::quat m_dragArcQ{ 1.0f, 0.0f, 0.0f, 0.0f }; // arcball accumulator
	float m_dragBaseAngle = 0.0f; // accumulated rotate radians
	float m_dragStartDist = 1.0f;
	// Gizmo/AABB line upload buffer.
	smvk::Holder<smvk::BufferHandle> m_gizmoLines{};
	uint32_t m_gizmoLinesCap = 0;
	// UV editor window line upload RETIRED with the canvas.
	//smvk::Holder<smvk::BufferHandle> m_uvWinLines{};
	//uint32_t m_uvWinLinesCap = 0;

	// ---- imgui ----
	smvk::mem::LinearPtr<smvk::ImGuiOverlay> m_imgui{};

	// ---- texture streaming (cheap initial mips, distance demand upgrades) ----
	// Streaming: tail-resident uploads upgraded by camera-distance demand
	// (handle-based requests: material slots are RHI pool indices, which
	// only coincide with bindless slots by creation order, so the editor
	// never keys demand by slot). Coarse: same streamer, no demand, the
	// floor decays to the single coarsest mip (flat average-color look,
	// useful for perf/bake-geometry checks). Full: no streamer at all,
	// full-resident uploads (reimport on switch).
	enum class TexMode : int {
		Streaming = 0,
		Coarse = 1,
		Full = 2,
	};
	TexMode m_texMode = TexMode::Streaming;
	smvk::mem::LinearPtr<smvk::TextureStreamer> m_textureStreamer;
	bool m_textureStreamingEnabled = true;
	// RHI pool index (what MaterialValues::textures stores) -> live RHI
	// handle for demand requests. Rebuilt on every scene load; empty for
	// pak scenes (full-resident, no streamer).
	std::unordered_map<uint32_t, smvk::TextureHandle> m_texHandleBySlot;
	// RHI pool index -> bindless descriptor slot for the GPU material
	// upload. The shader indexes textures[] by bindless slot, but
	// materials store pool indices (equal only by creation-order luck),
	// so the editor remaps at upload and logs any divergence.
	std::unordered_map<uint32_t, uint32_t> m_bindlessBySlot;
	void RebuildTexHandleMap();
	bool ApplyTexMode(); // switch mode (Full reimports the scene)
	void DrawTexturePanel(); // mode switch + residency stats + dump
	void DumpStreamerStats(); // entry table to the console

	// ---- cook ----
	CookSettings m_cook{};
	std::vector<std::string> m_consoleLines;
	bool m_cookRunning = false;
	bool m_bakeRunning = false; // on-demand GiBaker worker (see above)
	struct CookResult { bool ok = false; std::string log; };
	// EDITOR-ONLY job backend (see editor_fibers.h; the class keeps its
	// historical FiberBackend name): the single lane for all off-main-
	// thread sync work (the cook/bake workers below; started in
	// Initialize, pumped per frame, stopped in Shutdown BEFORE any state
	// a completion callback would touch). Plain std::thread workers —
	// nothing links Boost anymore.
	editor::FiberBackend m_fibers{};
	// Definition fingerprint the last Bake GI ran for (empty = never
	// baked this session): a cook that moves past it leaves sidecars
	// stale — the cook dialog's freshness readout says so (rebake to
	// refresh the live view's GI).
	std::string m_giBakedHash;
	// Fingerprint of the definition being baked (adopted as
	// m_giBakedHash at BakeGI completion).
	std::string m_giBakePendingHash;
	// Live cook pipe: the async worker pushes one entry per output line
	// (mutex-guarded); PollCookProcess drains it on the main thread so
	// the Console shows progress while the cook runs instead of one
	// dump at completion. Shared-ownership: the worker captures the
	// feed by value so it never dangles if the EditorApp dies mid-cook.
	struct CookFeed {
		std::mutex mutex;
		std::vector<std::string> lines;
	};
	std::shared_ptr<CookFeed> m_cookFeed;
	std::shared_ptr<CookFeed> m_bakeFeed; // Bake-GI live feed (see above)
	// ---- blocking modal task window (cook / bake / publish) ----
	// Long subprocesses run on worker threads with a mutex-guarded line
	// feed (CookFeed); the completion lands through the Poll* pumps on
	// the main thread. This window is the SINGLE progress surface for
	// all of them: a blocking modal popup (viewport mouse is eaten by
	// the modal itself, keyboard shortcuts by the TaskWindowBlocking
	// gate in UpdateModalInput) with the live stage, a progress bar and
	// a scrolling detail log. "Run in background" drops the popup while
	// the worker continues; the top-bar button reopens it on demand.
	struct ModalTaskWindow {
		bool wantOpen = false;   // a task owns the popup (armed/running/done)
		bool inBackground = false; // user sent it behind; worker continues
		bool opened = false;     // OpenPopup consumed (re-armed on reopen)
		std::string popupId = "TaskProgress";
		std::string title;       // header, e.g. "Bake GI"
		std::string stage;       // current stage line
		float progress = -1.0f;  // <0 indeterminate, else 0..1 fraction
		std::vector<std::string> lines; // detail tail (capped below)
		bool done = false;       // process finished (success or failure)
		bool ok = false;         // result flag
		std::string result;      // one-line result for the footer
	};
	ModalTaskWindow m_taskWindow;
	// True while the task popup blocks the UI (armed or up, not
	// backgrounded, result not yet acknowledged): keyboard dispatch and
	// top-bar actions consult this.
	bool TaskWindowBlocking() const {
		return m_taskWindow.wantOpen && !m_taskWindow.inBackground;
	}
	void TaskWindowBegin(const std::string& title,
		const std::string& stage);
	void TaskWindowPush(const std::string& line); // detail log (capped)
	void TaskWindowStage(const std::string& stage, float progress = -1.0f);
	void TaskWindowFinish(bool ok, const std::string& result);
	void ReopenTaskWindow(); // background -> foreground (top-bar button)
	void DrawTaskWindow();   // blocking modal popup (DrawMenuBar tail)
	// ---- cubemap capture face strip (cook dialog) ----
	// Live KTX2 face previews: while a cubemap cook runs, finished
	// captures appear here as tonemapped face strips (parsed from the
	// loose sidecars, never PNG); with no cook running the strip shows
	// the last-baked set. Manifest-independent (single captures show).
	struct CubeFaceView {
		std::string name;    // capture name (<base>[_N])
		std::string ktxPath; // host path of the loose sidecar
		std::filesystem::file_time_type mtime{};
		bool hasMtime = false;
		smvk::Holder<smvk::TextureHandle> tex; // strip texture (6 faces)
		ImTextureID imgui = 0; // backend binding; 0 = none
		int faceSize = 0;    // face width px (strip is 6x wide)
	};
	std::vector<CubeFaceView> m_cubeFaces{};
	uint32_t m_cubeFaceSampler = UINT32_MAX; // preview sampler (created once)
	uint64_t m_cubeFacePollFrame = 0; // throttle: scan every N frames
	// Cubemap progress (parsed from [CUBE] lines while a cubemap cook
	// runs; -1 = unknown total). Drives TaskWindowStage fractions.
	int m_cubeTotal = -1;
	int m_cubeDone = 0;
	bool m_cubeRunActive = false;
	void PollCubeFaces(); // scan sidecars, load new/changed via libktx
	void DropCubeFaces(); // release textures + bindings (new cook, shutdown)
	void DrawCubeFaceStrip(); // cook dialog section (see editor_ui.cpp)
	void CookCubemapsNow(); // cubemap-only quick bake (skips GiBaker)
	// Sidecar roots + capture base for the strip scan (mirrors the
	// manifest reader roots and the completion adopter base).
	bool CubeWatchSpec(std::string& dirOut, std::string& baseOut) const;
	// ---- ui state ----
	char m_filterBuf[128] = "";
	int m_rightTab = 0; // Inspector | UV | Render | Material
	int m_leftTab = 0;  // Assets | Console | Overrides (the left column's LOWER tabbed panel — the Outliner above it is always visible, not a tab)
	// Floating cook/bake tools (View menu). Export itself opens the
	// native save dialog and starts the cook immediately.
	bool m_showCookWindow = false;
	void DrawSceneOutliner();
	void DrawUvPanel();
	// Content root for definition-source resolution ("models/..." style
	// references): the current scene's content dir, else ./content.
	std::string ResolveVfsRoot() const;

	// ---- add-reference dialog state ----
};

// Native Windows dialogs (editor_native_dialog.cpp): single entry
// points for scenes (.gltf/.glb/.tour), outputs (.pak) and
// project folders. False on cancel/failure.
	bool NativeOpenFile(std::string& outPath, bool images = false);
	bool NativeSaveFile(std::string& outPath,
		const std::string& suggestedName = {});
	bool NativePickFolder(std::string& outPath);

} // namespace editor
