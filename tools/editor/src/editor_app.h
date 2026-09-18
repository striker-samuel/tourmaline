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

#include "editor_commands.h"

namespace editor {

// Viewport camera clip + FOV shared by editor_app.cpp / editor_render.cpp.
inline constexpr float kEditorFovYDeg = 45.0f;
inline constexpr float kEditorNearClip = 0.05f;
inline constexpr float kEditorFarClip = 2000.0f;

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
	}
	return "?";
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
	void UpdateCamera(float dt);
	void UpdateFrameData(uint32_t flight, smvk::Extent2D extent);
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
	smvk::ShaderProgram& MainProgram(); // program for m_viewMode (Lit fallback)
	smvk::ShaderProgram& InsetProgram(); // program for m_secondView (Lit fallback)
	bool EnsureBuffers(uint32_t flightCount);
	bool EnsureDepth(smvk::Extent2D extent);
	bool EnsureShadowAtlas();
	void RecordShadowPass(uint32_t cascadeIndex);
	void RecordMainPass(smvk::LogicalImageViewHandle swapView, smvk::Extent2D extent,
		uint32_t flight, bool selectedWireOverlay);
	void RecordInsetPass(smvk::LogicalImageViewHandle swapView,
		smvk::Extent2D extent, uint32_t flight); // UV side-strip
	void RecordGizmoPass(smvk::LogicalImageViewHandle swapView,
		smvk::Extent2D extent);
	void UpdateInsetFrameData(uint32_t flight, smvk::Extent2D stripExtent);
	// Second-viewport state: active unless off or the main view already
	// shows UVs; strip rect (right side) for the inset pass + label.
	bool SecondViewActive() const;
	static smvk::Extent2D InsetStrip(smvk::Extent2D extent, uint32_t& outX);
	void DrawWorldNodes(smvk::CommandBuffer& cmd, const app::PushConstants& basePc,
		bool maskedPhase, bool selectedOnly, bool shadowPass,
		bool bakedOnly = false); // bakedOnly: skip unbaked meshes (F8 view)
	// True when the world holds anything to draw (guards vertex bindings).
	bool HasDrawItems() const;

	// ---- picking (editor_pick.cpp) ----
	struct PickHit { uint32_t node = UINT32_MAX; float t = 0.0f; };
	std::optional<PickHit> PickNode(float mouseX, float mouseY,
		smvk::Extent2D extent) const;
	glm::vec3 MouseRay(float mouseX, float mouseY, smvk::Extent2D extent,
		glm::vec3& outOrigin) const;

	// ---- gizmo (editor_gizmo.cpp) ----
	bool GizmoHandleInput(smvk::Extent2D extent); // returns true while dragging

	// ---- UI (editor_ui.cpp) ----
	void DrawUI(smvk::Extent2D targetExtent);
	void DrawMenuBar();
	void DrawOpenDialog(); // file browser modal (Open glTF)
	void DrawOutliner();
	void DrawOutlinerNode(uint32_t node,
		const std::unordered_set<uint32_t>* expandSet = nullptr);
	void DrawInspector();
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

	// ---- flintbase (editor_flintbase.cpp) ----
	void DrawFlintbasePanel();   // product browser (toggle: View menu)
	void PublishToFlintbase();   // spawn `python -m flintbase.publish`
	void PollPublishProcess();

	// ---- camera ----
	CameraPositioner_FirstPerson m_camPos{};
	Camera m_camera{ m_camPos };
	float m_camYaw = 0.0f, m_camPitch = 0.0f;
	glm::vec3 m_camTarget{ 0.0f, 1.0f, 0.0f };
	float m_camDist = 6.0f;
	// Maya-style framing: F moves the orbit pivot (m_camTarget) to the
	// selection center and fits the distance; Shift+F fits the whole
	// scene. SelectionBounds unions the selected subtree's mesh AABBs.
	bool SelectionBounds(glm::vec3& outCenter, float& outRadius);
	void FrameSelection();
	void FrameAll();

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
	ViewMode m_viewMode = ViewMode::LitShadows;
	SecondView m_secondView = SecondView::Off;
	GizmoMode m_gizmoMode = GizmoMode::Translate;
	uint32_t m_selected = UINT32_MAX; // world node id
	uint32_t m_revealNode = UINT32_MAX; // outliner reveal request (viewport pick)
	bool m_wireOverlay = true;
	bool m_uiMode = true; // false = fullscreen viewport (camera control)
	bool m_showLightGizmos = true; // viewport markers for light nodes

	// ---- sun/ambient (viewport lighting) ----
	// m_sun* are the live viewport values (sliders). On scene load they
	// are seeded from the cooked directional sun when the scene has one
	// (see RebuildCookedLights); otherwise the defaults below stand in.
	glm::vec3 m_sunColor{ 1.0f, 0.96f, 0.92f };
	float m_sunIntensity = 3.0f;
	float m_sunAzimuth = 30.0f;
	float m_sunElevation = 55.0f;
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
	void RebuildCookedLights(); // + seeds the m_sun* sliders from the sun
	const ::SceneLight* FindNodeLight(uint32_t node,
		glm::vec3* outAimWs = nullptr) const; // + world aim for dir/spot
	glm::vec3 m_skyColor{ 0.35f, 0.5f, 0.7f };
	float m_skyIntensity = 0.6f;
	float m_exposure = 1.0f;

	// ---- shading resources ----
	smvk::Holder<smvk::BufferHandle> m_frameUniforms; // ring: frames * sizeof(Frame)
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
	smvk::Holder<smvk::ShaderModuleHandle> m_lineVsModule;
	smvk::Holder<smvk::ShaderModuleHandle> m_lineFsModule;
	smvk::ShaderProgram m_unlitProgram{};
	smvk::ShaderProgram m_litProgram{};
	smvk::ShaderProgram m_uv0Program{}; // UV0 debug (falls back to Lit)
	smvk::ShaderProgram m_uv1Program{}; // lightmap-UV debug (falls back to Lit)
	smvk::ShaderProgram m_vcolorProgram{}; // vertex-color debug (falls back to Lit)
	smvk::ShaderProgram m_bakedProgram{}; // baked-irradiance debug (falls back to Lit)
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
};

} // namespace editor
