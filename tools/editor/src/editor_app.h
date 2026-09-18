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

#include "editor_commands.h"

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
// Cook settings (mirrors the PakCooker flags the editor drives).
// ---------------------------------------------------------------------------
struct CookSettings {
	std::string outPak = "content/paks/edited.pak";
	bool meshopt = true;
	bool lods = false;
	bool lightmap = false;
	uint32_t lmDensity = 32;
	uint32_t lmLobes = 12;
	uint32_t lmAtlasWidth = 2048;
	uint64_t lmBudget = 4ull * 1024 * 1024;
	uint32_t lmRays = 256;
	float lmExcludeSmall = 0.0f;
	std::string lmExcludeKw; // extra --lm-exclude keywords (comma separated)
	float sunAzimuth = 30.0f;
	float sunElevation = 55.0f;
	float sunIntensity = 10000.0f;
	float skyIntensity = 600.0f;
};

class EditorApp : public smvk::AppImpl {
	using Super = smvk::AppImpl;
public:
	EditorApp() = default;
	~EditorApp() override = default;

	bool OpenScene(const std::string& path); // dispatches .pak vs .gltf
	bool OpenPak(const std::string& pakPath); // accepts "file.pak:N" too
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
	bool LoadGltfIntoWorld(const std::string& path);
	bool LoadPakIntoWorld(const std::string& path,
		uint32_t sceneIndex = 0); // cooked pak, GPU world
	bool TryLoadSiblingSglm(const std::string& pakPath); // gi/<stem>.sglm
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
	// Flat UV draw path: the PRIMARY SELECTION's subtree meshes drawn
	// through the uvflat projection (unwrapped + texture content).
	void DrawFlatUvNodes(smvk::CommandBuffer& cmd, ViewMode mode,
		const app::PushConstants& pc);
	// Flat UV wires (glTF model accessors) as line batches in NDC space
	// (built in RecordGizmoPass when any visible viewport is flat).
	// subtreeRoot: world node whose subtree to wire (UINT32_MAX = the
	// whole scene — the PNG export path).
	void BuildFlatUvWires(std::vector<smvk::DebugLineVertex>& outLines,
		ViewMode mode, uint32_t subtreeRoot = UINT32_MAX) const;
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
		bool bakedOnly = false); // bakedOnly: skip unbaked meshes (F8 view)
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
	std::vector<uint32_t> m_selection;
	bool IsSelected(uint32_t node) const;
	void SetSelection(uint32_t node);        // single
	void AddToSelection(uint32_t node);
	void ClearSelection();
	// Rubber-band state (window pixels, viewport-local rect in m_vpBox*).
	bool m_vpBoxDragging = false;
	bool m_vpBoxAdditive = false;   // Shift = add to current selection
	float m_vpBoxX0 = 0, m_vpBoxY0 = 0, m_vpBoxX1 = 0, m_vpBoxY1 = 0;
	int m_vpBoxVp = 0;              // viewport the box started in
	void FinishBoxSelect();        // apply on release
	void DrawViewportChrome();     // borders + labels + box rect (ImGui)

	// ---- UI (editor_ui.cpp) ----
	void DrawUI(smvk::Extent2D targetExtent);
	void DrawMenuBar();
	void DrawOpenDialog(); // file browser modal (Open glTF)
	void DrawOutliner();
	void DrawOutlinerNode(uint32_t node,
		const std::unordered_set<uint32_t>* expandSet = nullptr);
	void DrawInspector();
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
	// live world. No stack changes.
	void ExecCmd(const EditCmd& c, bool forward);
	// Do: execute + push (truncates redo) + save tour.
	void Do(const EditCmd& c);
	// Push an already-executed edit (gizmo/inspector live drags).
	void PushDone(const EditCmd& c);
	bool Undo();
	bool Redo();
	void ClearHistory();
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
	// Live command helpers (editor_commands.cpp).
	void ReadLocalTRS(uint32_t node, glm::vec3& t, glm::vec3& rDeg,
		glm::vec3& s);
	void WriteLocalTRS(uint32_t node, const glm::vec3& t,
		const glm::vec3& rDeg, const glm::vec3& s);
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
	// Renders the whole scene unwrapped by its atlas uv2 over the baked
	// lightmap content into an offscreen target (atlas resolution), then
	// reads it back and writes a PNG — optional UV wireframe overlay.
	// Mirrors the App screenshot path (Capture + DownloadDataFromImage).
	bool m_lmExportQueued = false;  // button sets; RenderFrame renders + saves
	bool m_lmExportWires = true;    // wireframe overlay toggle
	smvk::Holder<smvk::ImageHandle> m_lmExportImage;
	smvk::Holder<smvk::ImageViewHandle> m_lmExportView;
	smvk::Holder<smvk::ImageHandle> m_lmExportDepth;
	smvk::Holder<smvk::ImageViewHandle> m_lmExportDepthView;
	smvk::LogicalImageViewHandle m_lmExportLogical{};
	smvk::LogicalImageViewHandle m_lmExportDepthLogical{};
	smvk::Extent2D m_lmExportExtent{ 0, 0 };
	smvk::Holder<smvk::BufferHandle> m_lmExportLines;
	uint32_t m_lmExportLinesCap = 0;
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
	bool IsRefPlaceholder(uint32_t node) const;
	void DrawReferenceSection(uint32_t node); // Inspector block
	void DrawAddReferenceDialog();

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

	// ---- source scene ----
	// m_gltfPath is the scene identity (a //flintbase/ scheme path when
	// opened from a product); m_gltfHostPath is the resolved file used
	// for loading and fingerprinting (equal to m_gltfPath for local files).
	std::string m_gltfPath;
	std::string m_gltfHostPath;
	std::string m_pendingGltf; // --gltf/--pak path queued before Initialize
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
	// ---- baked-irradiance GPU upload (9 RGBA16F atlas layers, App parity)
	// Feeds FrameUniforms.lmLayers/lmEnabled for the F8 view; released
	// with the atlas (scene change / manual clear).
	smvk::Holder<smvk::TextureHandle> m_lmLayers[smvk::gi::kLightmapLayerCount];
	uint32_t m_lmLayerSlots[smvk::gi::kLightmapLayerCount] = {};
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
	uint32_t m_revealNode = UINT32_MAX; // outliner reveal request (viewport pick)
	bool m_wireOverlay = true;
	bool m_uiMode = true; // false = fullscreen viewport (camera control)
	bool m_showLightGizmos = true; // viewport markers for light nodes
	bool m_wireframeAll = false; // Z: whole-scene wireframe (wire program)
	int m_debugMeshletMode = 0; // F4 color mode: 0 random, 1 cone heat
	bool m_showOverlay = true;  // legend on the ACTIVE panel (toggle:
	                            // View menu or the panel chip button)
	// ---- panel sizes (drag the splitters to resize; constants above
	// are the defaults) ----
	float m_leftW = kEditorLeftW;
	float m_rightW = kEditorRightW;
	float m_bottomH = kEditorBottomH;
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
	int m_gizmoAxis = -1; // 0/1/2 = X/Y/Z
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
	bool m_showOpenDialog = false;
	char m_openPathBuf[512] = "content/models/Sponza/sponza.gltf";
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
	std::string ResolveVfsRoot();
	bool PakSelected() const; // outliner selection is pak-related
	std::string m_browseDir; // file browser cwd (reset each open)
	std::string m_browseSel; // file browser selection (full path)

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
	bool m_showRefDialog = false;
	char m_refSourceBuf[512] = "";
	char m_refNameBuf[128] = "";
};

} // namespace editor
