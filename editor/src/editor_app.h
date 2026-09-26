#pragma once
// GltfEditor: inspect + cook glTF scenes with a .tour overlay.
//
// Loads a glTF straight into a GPU World (same importer as App/PakCooker),
// shows it in a minimal forward viewport with three shading modes
// (Unlit / Lit / Lit+Shadows), and lets the user:
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
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/tour.h>
#include <Engine/Camera/camera.h>
#include <Engine/Flintbase/flintbase_client.h>
#include <Engine/GI/lightmap.h>
#include <Engine/ImGui/imgui_overlay.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Renderer/light.h>
#include <Engine/Renderer/texture_streamer.h>

// ABI-checked shared structs (offsets asserted against common.hlsli).
#include <App/scene_renderer.h>

#include <tiny_gltf.h>

#include <imgui.h> // menu-bar gem mark + inspector pages

#include <Engine/Placement/placement_terrain.h>
#include "editor_commands.h"

#include "editor_placement.h"

namespace editor {

// Menu-bar gem mark (editor_branding.cpp): vector twin of
// tourmaline_logo.png, drawn straight into the given draw list.
void DrawTourmalineGem(ImDrawList* dl, const ImVec2& p, float h);

// Viewport camera clip + FOV shared by editor_app.cpp / editor_render.cpp.
inline constexpr float kEditorFovYDeg = 45.0f;
inline constexpr float kEditorNearClip = 0.05f;
inline constexpr float kEditorFarClip = 2000.0f;

// Panel layout constants shared by editor_ui.cpp / editor_render.cpp /
// editor_gizmo.cpp: the 3D viewport is the CENTRAL rect between the
// panels (panels are siblings in one layout — they never overlap the
// rendered view). kEditorBottomH is the Overrides/Console dock height.
inline constexpr float kEditorLeftW = 330.0f;
inline constexpr float kEditorRightW = 370.0f;
inline constexpr float kEditorBottomH = 230.0f;

// Central viewport rect in window pixels (x/y = top-left offset).
struct ViewportRect {
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// ---------------------------------------------------------------------------
// Editor cameras: orbit-frame cameras owned by the EDITOR (never cooked,
// never part of the scene). Perspective (finite reversed-Z) or orthographic
// (Top/Side); per-camera FOV + near/far + ortho half-height.
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
// Viewport shading mode: default diffuse unlit, default lit, and lit with
// simple (single-cascade PCF) shadows.
// ---------------------------------------------------------------------------
enum class ViewMode : int {
	Unlit = 0,      // default_unlit: baseColor + emissive, no lighting
	Lit = 1,        // default_lit: sun + sky ambient, no shadow maps
	LitShadows = 2, // default_lit + 1-cascade directional shadow atlas
	Uv0 = 3,        // default_uv0_debug: base UVs
	Uv1 = 4,        // default_uv1_debug: lightmap UVs
	VertexColors = 5, // default_vertexcolor_debug: vertex colors
	Baked = 6,      // default_baked_debug: baked atlas irradiance (F8)
	Meshlets = 7,   // meshdebug: flat per-meshlet colors (F4)
	FlatUv0 = 8,    // uvflat0: unwrapped TEXCOORD0 + diffuse content
	FlatUv1 = 9,    // uvflat1: unwrapped atlas uv2 + lightmap content
};
inline const char* ViewModeName(ViewMode m) {
	switch (m) {
	case ViewMode::Unlit: return "Unlit (diffuse)";
	case ViewMode::Lit: return "Lit (no shadows)";
	case ViewMode::LitShadows: return "Lit + shadows";
	case ViewMode::Uv0: return "UV0";
	case ViewMode::Uv1: return "UV1 (lightmap)";
	case ViewMode::VertexColors: return "Vertex colors";
	case ViewMode::Baked: return "Baked irradiance";
	case ViewMode::Meshlets: return "Meshlets";
	case ViewMode::FlatUv0: return "Flat UV (diffuse)";
	case ViewMode::FlatUv1: return "Flat UV (lightmap)";
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
// Cook settings (mirrors the PakCooker flags the editor drives).
// ---------------------------------------------------------------------------
struct CookSettings {
	std::string outPak = "content/paks/edited.pak";
	std::string contentDir = "content";
	std::string inVfs; // VFS path for --in-vfs when no tour
	bool meshopt = true;
	bool lods = false;
	std::string lodRatios = "0.6,0.3,0.12"; // LOD chain ratios (csv)
	float lodError = 0.02f;                  // simplification target error
	bool lightmap = false;
	float lmMetersPerTexel = 0.25f; // chart density (m/texel)
	uint32_t lmLobes = 12;
	uint32_t lmAtlasWidth = 1024;   // FIXED atlas width; height grows
	uint32_t lmRays = 256;
	int lmProxyRes = 256; // voxel-remesh scene proxy (0 = legacy per-mesh)
	float lmExcludeSmall = 0.0f;
	std::string lmExcludeKw; // extra --lm-exclude keywords (comma separated)
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
	// Cubemap captures
	bool cubemap = false;
	std::string cubemapName;
	uint32_t cubemapSize = 128;
	uint32_t cubemapRays = 128;
	uint32_t cubemapBounces = 3;
	// GI bake stage: authored convex volumes (cvol nodes) bake with
	// the cook via GiBaker — sidecars gi/<pak-stem>_* keyed to the
	// pak stem. No-op when the definition carries no cvolumes.
	bool giCvol = true;
	// World-field bake: the adaptive probe octree (.sggi) bakes with
	// the cook too (GiBaker --out gi/<pak-stem>.sggi). This is the
	// global fallback the runtime samples outside any convex volume;
	// it is the slow stage (minutes on real scenes).
	bool giWorld = true;
	// Meshlet cook: per-meshlet build over the cooked topology.
	bool meshlets = false;
	uint32_t meshletMaxVerts = 64;
	uint32_t meshletMaxTris = 124;
	float meshletConeWeight = 0.0f;
};

class EditorApp : public smvk::AppImpl {
	using Super = smvk::AppImpl;
public:
	EditorApp() = default;
	~EditorApp() override = default;

	bool OpenScene(const std::string& path); // dispatches .pak vs .gltf
	bool OpenPak(const std::string& pakPath); // accepts "file.pak:N" too
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
	bool LoadPakIntoWorld(const std::string& path,
		uint32_t sceneIndex = 0); // cooked pak, GPU world
	bool TryLoadSiblingSglm(const std::string& pakPath); // gi/<stem>.sglm
	// Convex GI volume sidecars (probes-only pivot): gi/<stem>.cvmanifest
	// + .cvol next to the opened pak. Builds the preview hulls (AABB
	// clipped by the volume's planes) for the LIGHTING-mode view option.
	bool TryLoadCvolumes(const std::string& pakPath);
	static bool HasPakExtension(const std::string& path);
	void UpdateTextureStreaming(smvk::Extent2D renderExtent);
	void RebuildNodeIndex(); // world-node -> cluster/prim mapping for picking
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
		const app::PushConstants& pc);
	// Flat UV wires (glTF model accessors) as line batches in NDC space
	// (built in RecordGizmoPass when any visible viewport is flat).
	// subtreeRoot: world node whose subtree to wire (UINT32_MAX = the
	// whole scene — the PNG export path).
	void BuildFlatUvWires(std::vector<smvk::DebugLineVertex>& outLines,
		ViewMode mode, uint32_t subtreeRoot = UINT32_MAX,
		int vpIdx = 0) const;
	smvk::ShaderProgram& InsetProgram(); // program for m_secondView (Lit fallback)
	bool EnsureBuffers(uint32_t flightCount);
	bool EnsureDepth(smvk::Extent2D extent);
	bool EnsureShadowAtlas();
	void RecordShadowPass(uint32_t cascadeIndex); // active viewport's frame
	// Per-viewport scene pass: offset/extent from m_viewports[vpIdx].rect,
	// frame slice + push constants from that viewport's camera.
	void RecordMainPass(smvk::LogicalImageViewHandle swapView, int vpIdx,
		uint32_t flight, bool selectedWireOverlay);
	void RecordInsetPass(smvk::LogicalImageViewHandle swapView,
		uint32_t flight); // UV side-strip (single layout, viewport 0)
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
	void DrawWorldNodes(smvk::CommandBuffer& cmd, const app::PushConstants& basePc,
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
		ViewMode viewMode = ViewMode::LitShadows; // this panel's view
		ViewportRect rect;     // computed every frame
		bool hovered = false;  // mouse inside this viewport
		app::FrameUniforms frame{}; // CPU copy (picking/gizmo math)
		glm::mat4 viewProj{ 1.0f };
		uint64_t frameAddr = 0; // this flight's frame-uniform slice
	};
	std::vector<EditorCam> m_editorCams;   // Persp + Top + Side defaults
	EditorViewport m_viewports[kMaxViewports];
	int m_vpLayout = 0;  // 0 = single, 1 = quad (four panels)
	int m_activeVp = 0;  // hovered/last-active viewport index
	int ViewportCount() const { return m_vpLayout == 1 ? kMaxViewports : 1; }
	EditorCam& ActiveCam() { return m_editorCams[m_viewports[m_activeVp].camIdx]; }
	ViewMode& ActiveViewMode() { return m_viewports[m_activeVp].viewMode; }
	ViewMode ActiveViewMode() const { return m_viewports[m_activeVp].viewMode; }
	void SetupDefaultCameras();
	bool GridVisible() const { return m_showGrid; }

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
	// Per-viewport flat-UV navigation (FlatUv0/FlatUv1 panels only):
	// centered zoom + pan in UV units (uvT = (uv-0.5)*zoom + 0.5 + pan).
	float m_uvZoom[kMaxViewports] = { 1.0f, 1.0f, 1.0f, 1.0f };
	glm::vec2 m_uvPan[kMaxViewports] = {};
	bool IsSelected(uint32_t node) const;
	void SetSelection(uint32_t node);        // single
	void AddToSelection(uint32_t node);
	void ClearSelection();
	void ClearSubSelection(); // clears face/vertex highlight only
	// World pseudo-selection: the Outliner's "World" entry (global render
	// settings) is NOT a hierarchy node — it never enters m_selection /
	// m_selected (which stay UINT32_MAX), so gizmos, picking, drags and
	// commands keep working unchanged. Mutually exclusive with node
	// selection: selecting either clears the other.
	bool m_worldSelected = false;
	void SelectWorldNode();
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
	// Flat-UV pane pick: mouse (viewport-local) -> UV point (inverse zoom/
	// pan) -> brute-force point-in-triangle over the selection subtree
	// (or whole scene when nothing selected). No accel structures.
	std::optional<SubHit> PickFlatUv(int vpIdx, float mouseX,
		float mouseY) const;
	void ApplySubSelection(const SubHit& h); // node + face/vert per m_selMode
	// Flat-UV NDC helpers (must match uvflat.vert.hlsli transform).
	glm::vec2 UvToFlatNdc(glm::vec2 uv, int vpIdx) const;
	glm::vec2 FlatNdcToUv(glm::vec2 ndc, int vpIdx) const;
	// Resolve the selected face/vertex UVs for highlight batches.
	bool SelectedFaceUvs(glm::vec2 outUv[3], ViewMode mode) const;
	bool SelectedVertUv(glm::vec2& outUv, ViewMode mode) const;
	// World-space positions for the 3D face/vertex highlight.
	bool SelectedFaceWorld(glm::vec3 outW[3]) const;
	bool SelectedVertWorld(glm::vec3& outW) const;
	void UpdateUvPaneInput(); // MMB pan + wheel zoom on flat viewports
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
	void ModalViewLitShadows();
	void ModalVpLayoutToggle();
	void ModalUiToggle();
	void ModalUndo();
	void ModalRedo();
	void ModalSave(); // Ctrl+S: SaveTour (explicit save -> project file)
	void ModalSaveIncremental();
	void ModalDelete(); // Del: DeleteSelection() (skipped while the
	                     // placement graph window owns the keyboard —
	                     // its canvas Del removes graph nodes instead)
	void ModalToggleVisibility(); // Ctrl+H: hide/unhide the selection
	                               // (all nodes follow the primary's inverse)
	void ModalGizmoTranslate();
	void ModalGizmoRotate();
	void ModalGizmoScale();
	void ModalGizmoCycle();
	void ModalSelRadial();
	void ModalWireframeToggle();
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
	void ModalRunInApp();
	void ModalCook();
	void ModalViewMeshlets();
	void ModalViewVertexColors();
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
	void DrawPlayBar(); // Unity-style Run-in-App bar (top center)
	void DrawCookPanel();
	void DrawLightmapPanel(); // pak manifest + .sglm atlas inspection
	void DumpPakManifest(); // pak-sourced counts + lmChunk histogram to console
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
	void DrawViewportOverlay(smvk::Extent2D extent);
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
	// Tour-stable default parent for new lights: the primary cluster's
	// root (WorldToCluster maps it to cluster id 0), else world root.
	uint32_t DefaultCreateParent() const;
	// Unique "Point_N"/"Spot_N" name over the live world lights.
	std::string UniqueLightName(bool spot) const;
	// Unique "CVol_N" name over the live world cvolume nodes.
	std::string UniqueCvolName() const;
	// Unique "Cube_N" name over the live world cubemap captures.
	std::string UniqueCubemapName() const;
	// Unique "Terrain_N" name over the live world terrains.
	std::string UniqueTerrainName() const;
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
	bool SaveTourTo(const std::string& path); // becomes the target
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
	// Cook dialog (File > Cook...)
	bool m_showCookDialog = false;
	char m_cookOutPakBuf[512] = "";
	char m_cookContentDirBuf[512] = "content";
	char m_cookCubemapNameBuf[128] = "";
	// Set by PollCookProcess when the async cook completes; the cook
	// dialog consumes it inside its begin block and closes itself.
	bool m_cookJustFinished = false;
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
	void ShowCookDialog();
	void DrawCookDialog();
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
	// World-space authoring box of a cvolume node (center = global
	// translate, full extents = global scale, axis-aligned — rotation
	// ignored by design). False when the node has no cvol.
	bool CvolBox(uint32_t node, glm::vec3& mn, glm::vec3& mx) const;
	// Inspector "Convex volume" section for cvolume nodes (priority /
	// spacing / fades via undoable SetCvolAttr; shape via the transform).
	void DrawCvolSection(uint32_t node);
	// Inspector "Cubemap capture" section for capture nodes (radius
	// via undoable SetCubemapAttr; position via the transform).
	void DrawCubemapSection(uint32_t node);
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
	void CookToPak(); // save tour + PakCooker --tour on a worker
	void PollCookProcess();
	// ---- run in App (editor_cook.cpp): Unity-style Play launches the
	// current scene in the runtime App (--pak for pak scenes, --gltf for
	// sources). Empty when nothing runnable is loaded.
	std::string RunArgs() const;
	void RunInApp();
	void PollRunProcess();
	bool m_runRunning = false;

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

	// ---- flintbase (editor_flintbase.cpp) ----
	void DrawFlintbasePanel();   // product browser (toggle: View menu)
	void PublishToFlintbase();   // spawn `python -m flintbase.publish`
	void PollPublishProcess();

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
	bool AddSceneReference(const std::string& source, const std::string& name);
	bool RemoveSceneReference(uint32_t refWorldNode);
	bool RemoveSceneReferenceAt(size_t idx); // panel path (index-exact)
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
	void FrameSelection(); // frames the ACTIVE viewport's camera
	void FrameAll();

	// ---- environment (editor IBL, NEVER cooked) ----
	// Editor viewport lighting lives in EditorEnv blocks. The default
	// block is the editor fallback; any scene node can be flagged as an
	// ENVIRONMENT NODE (skybox + EV/volumetrics authoring — position =
	// node translate, size = node scale; convex-volume support later)
	// and picked as the CURRENT source. Everything here is editor-only:
	// saved as editorSet lines in the .tour, skipped by ApplyTourToCluster.
	struct EditorEnv {
		glm::vec3 skyColor{ 0.35f, 0.5f, 0.7f };
		float skyIntensity = 0.6f;
		glm::vec3 sunColor{ 1.0f, 0.96f, 0.92f };
		float sunAzimuth = 30.0f;
		float sunElevation = 55.0f;
		float sunIntensity = 3.0f;
		float exposure = 1.0f;
		// Authored but not yet rendered by the editor viewport:
		float fogDensity = 0.0f;
		glm::vec3 fogColor{ 0.6f, 0.7f, 0.8f };
		float volumetrics = 0.0f; // 0 = off (future: editor volume pass)
		std::string skyboxUri;     // stored (skybox sampling is TODO)
	};
	EditorEnv m_envDefault;
	std::unordered_map<uint32_t, EditorEnv> m_envByNode; // per env node
	uint32_t m_envCurrent = UINT32_MAX; // UINT32_MAX = editor default
	EditorEnv& CurrentEnv();   // the source the viewport renders with
	const EditorEnv& CurrentEnv() const;
	bool IsEnvNode(uint32_t node) const { return m_envByNode.contains(node); }
	void MarkEnvNode(uint32_t node);     // flag node + copy current settings
	void UnmarkEnvNode(uint32_t node);
	void ApplyEnvToFrame(app::FrameUniforms& frame,
		smvk::EnvironmentSettings& env) const;
	void DrawEnvPanel();       // Inspector "Environment (IBL)" section
	// editorSet persistence (tour editorSettings; cluster-space node ids).
	void SaveEnvToDoc(smvk::tour::Doc& doc) const;
	void LoadEnvFromDoc(const smvk::tour::Doc& doc);

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
	// m_gltfPath is the scene identity (a //flintbase/ scheme path when
	// opened from a product); m_gltfHostPath is the resolved file used
	// for loading and fingerprinting (equal to m_gltfPath for local files).
	std::string m_gltfPath;
	std::string m_gltfHostPath;
	std::string m_pendingGltf; // --gltf/--pak path queued before Initialize
	std::string m_prefsPath;   // Documents/tourmalineEditor/editor.prefs
	// ---- cooked pak state (kept alive: PakReader borrows these bytes) ----
	std::string m_pakPath; // empty when the scene came from glTF
	std::vector<uint8_t> m_pakImage;
	// Parsed once per pak load (never per frame: Open decompresses).
	std::optional<smvk::PakReader> m_pakReader;
	std::vector<smvk::PakSceneInfo> m_pakScenes;
	uint32_t m_pakSceneIndex = 0;
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
	tinygltf::Model m_gltfModel; // kept for picking
	bool m_hasGltfModel = false;
	// world prim-node -> (gltf mesh, primitive) for exact ray picking
	struct PrimRef { int mesh = -1; int prim = -1; };
	std::unordered_map<uint32_t, PrimRef> m_primRef; // node -> ref
	uint32_t m_clusterRootWorld = UINT32_MAX; // world id of the cluster root

	// ---- editor state ----
	// View modes are PER VIEWPORT (m_viewports[].viewMode; the F-keys
	// and View menu act on the ACTIVE one — UV views are panel types).
	SecondView m_secondView = SecondView::Off;
	GizmoMode m_gizmoMode = GizmoMode::Translate;
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
	bool m_wireframeAll = false; // Z: whole-scene wireframe (wire program)
	int m_debugMeshletMode = 0; // F4 color mode: 0 random, 1 cone heat
	bool m_showOverlay = true;  // legend on the ACTIVE panel (toggle:
	                            // View menu or the panel chip button)
	// ---- panel sizes (drag the splitters to resize; constants above
	// are the defaults) ----
	float m_outlinerW = kEditorLeftW;
	float m_inspectorW = kEditorRightW;
	float m_bottomDockH = kEditorBottomH;
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
	// ---- viewport lighting (see EditorEnv above: the live values live in
	// CurrentEnv(); RebuildCookedLights seeds m_envDefault from the cooked
	// directional sun when the scene has one, else the defaults stand in).
	// ---- cooked scene lights (KHR punctual, glTF import or pak) ----
	// First directional wins as the viewport sun (App parity). Aim is the
	// node's G*(0,0,1): editor content is always coordFix-0 baked
	// (PakCooker cooks at 0, the editor imports at 0), so the engine-local
	// aim is +Z (see GltfPunctualAimAxis). Points/spots are counted and
	// logged; the editor has no forward+ path, so only the sun renders.
	bool m_hasCookedSun = false;
	std::string m_cookedSunName;
	uint32_t m_cookedDirs = 0, m_cookedPoints = 0, m_cookedSpots = 0;
	uint32_t m_cookedStaticSkipped = 0;
	void RebuildCookedLights(); // + seeds m_envDefault's sun from the sun
	const ::SceneLight* FindNodeLight(uint32_t node,
		glm::vec3* outAimWs = nullptr) const; // + world aim for dir/spot

	// ---- shading resources ----
	// Frame ring holds one slice PER (flight, viewport): frameUniforms is
	// sized frames * kMaxViewports * sizeof(Frame); each viewport's addr
	// is stored in m_viewports[i].frameAddr.
	smvk::Holder<smvk::BufferHandle> m_frameUniforms; // ring: frames * kMaxViewports * sizeof(Frame)
	smvk::Holder<smvk::BufferHandle> m_insetFrameUniforms; // second viewport (strip aspect)
	uint64_t m_lastInsetFrameAddr = 0; // current flight's inset slice
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
	smvk::ShaderProgram m_shadowProgram{}; // depth-only (atlas-owned default)
	smvk::ShaderProgram m_lineProgram{}; // gizmo/AABB lines
	smvk::CascadedShadowAtlas m_shadowAtlas{};
	smvk::DepthTarget m_depth{};
	std::vector<smvk::LogicalImageViewHandle> m_swapViews;
	uint32_t m_flightCount = 0;
	app::FrameUniforms m_lastFrame{}; // CPU copy for gizmo/pick math
	glm::mat4 m_lastViewProj{ 1.0f };
	// GPU addresses of the current flight's uniform slices (set by
	// UpdateFrameData, consumed by the shadow + main record funcs).
	struct FrameAddrs { uint64_t frame = 0, lights = 0, materials = 0, cascades = 0; };
	FrameAddrs m_lastFrameAddrs{};

	// per-frame upload caches
	uint64_t m_frameCounter = 0;

	// ---- gizmo drag state ----
	bool m_gizmoDragging = false;
	int m_gizmoAxis = -1; // 0/1/2 = X/Y/Z axes; 3/4/5 = YZ/XZ/XY planes
	                      // (translate/scale two-axis handles); 6 =
	                      // rotate arcball (free rotation inside the sphere)
	glm::vec3 m_dragGrabPoint{ 0.0f };
	glm::vec3 m_dragNodeOrigin{ 0.0f };
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

	// ---- imgui ----
	std::unique_ptr<smvk::ImGuiOverlay> m_imgui{};

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
	std::unique_ptr<smvk::TextureStreamer> m_textureStreamer;
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
	struct CookResult { bool ok = false; std::string log; };
	std::future<CookResult> m_cookFuture{};
	std::future<CookResult> m_runFuture{}; // Run-in-App worker (see above)
	// ---- ui state ----
	char m_filterBuf[128] = "";
	// Outliner tabs (Scene graph vs VFS browser) + VFS selection.
	// m_vfsSel: "" none; "disk:<full>" disk entry; "pak:" loaded-pak root;
	// "pak:<tocPath>" entry inside the loaded pak.
	int m_outlinerTab = 0;
	std::string m_vfsSel;
	std::string m_vfsRoot; // content dir, resolved on scene load
	int m_rightTab = 0; // Inspector | UV | Cook | Bake | Textures
	int m_bottomTab = 0; // bottom dock: Overrides | Console
	void DrawSceneOutliner();
	void DrawVfsOutliner();
	void DrawVfsDir(const std::string& dir, int depth);
	void DrawUvPanel();
	void DrawPakBox(); // contextual pak actions (VFS selection only)
	std::string ResolveVfsRoot() const;
	bool PakSelected() const; // outliner selection is pak-related

	// ---- flintbase panel state ----
	bool m_showFlintbasePanel = false;
	char m_fbProject[64] = "";
	char m_fbItem[128] = "";
	char m_fbTarget[512] = "";
	char m_fbPublishSource[512] = "";
	struct FbItemRow { std::string name; std::string type; };
	std::vector<FbItemRow> m_fbItems;
	int m_fbItemSel = -1;
	std::vector<smvk::FlintbaseProductInfo> m_fbProducts;
	int m_fbProductSel = -1;
	bool m_publishRunning = false;
	struct PublishResult { bool ok = false; std::string log; };
	std::future<PublishResult> m_publishFuture{};

	// ---- add-reference dialog state ----
};

// Native Windows open dialog (editor_native_dialog.cpp): single entry
// point for .gltf/.pak/.tour, starts in C:/. False on cancel/failure.
	bool NativeOpenFile(std::string& outPath, bool images = false);
	bool NativeSaveFile(std::string& outPath);

} // namespace editor
