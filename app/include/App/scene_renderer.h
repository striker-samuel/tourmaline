#pragma once
// SceneRenderer: whole-frame renderer class extracted from the former
// app/main.cpp TestApp. It owns every framegraph resource (pipelines,
// targets, per-frame buffers) plus the scene (world cluster) and appends
// all passes every frame from a configurable pass set, so adding a scene
// sample (Sponza, Bistro, ...) is a SceneDesc row — never a main.cpp copy.
//
// Typical use (see app/src/main.cpp):
//   app::SceneRenderer renderer;
//   renderer.Configure(app::SceneRegistry::Find("bistro").value(),
//                      app::ScenePassConfig::All());
//   renderer.Run(); // smvk::AppImpl main loop

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/Camera/camera.h>
#include <Engine/ImGui/imgui_overlay.h>
#include <Engine/Renderer/cookies.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/Rendergraph/bloom.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <Engine/Rendergraph/deferredshadow.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/environment.h>
#include <Engine/Rendergraph/exposure.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/motionblur.h>
#include <Engine/Rendergraph/motionvector.h>
#include <Engine/Rendergraph/msaa.h>
#include <Engine/Rendergraph/smaa.h>
#include <Engine/Rendergraph/spotshadow.h>
#include <Engine/Rendergraph/ssao.h>
#include <Engine/Rendergraph/tonemap.h>
#include <Engine/Rendergraph/ui.h>

#include <App/forwardplus_renderer.h>
#include <App/frame_context.h>
#include <App/push_constants.h>
#include <App/renderer.h>

namespace app {

// ---------------------------------------------------------------------------
// Per-scene sample descriptor (replaces the former file-static
// gltf_meshes_paths / pak_path globals). A new sample = one row in
// SceneRegistry::All(); the framegraph is shared.
// ---------------------------------------------------------------------------
struct SceneDesc {
	const char* id = "sponza";
	const char* label = "Sponza";
	const char* gltfPath = "content/models/Sponza/sponza.gltf";
	const char* pakPath = "content/paks/sponza.pak";
	glm::vec3 camPos{ 0.0f, 1.0f, 0.0f };
	glm::vec3 camTarget{ 0.0f, 1.0f, 1.0f };
	float farClip = 50.0f;
	// Coordinate system fix for glTF import:
	// 0 = engine fix (180° Y: -X, +Y, -Z), 1 = skip engine fix, 2 = invert Y only,
	// 3 = cancel root -90° X rotation + 100x scale (for Niagara bistro)
	int coordFix = 0;
	// Optional pre-transform applied to scene root after loading (post coordFix)
	glm::mat4 preTransform{ 1.0f };
};

struct SceneRegistry {
	static const SceneDesc& Sponza();
	static const SceneDesc& Bistro();
	static const SceneDesc& BistroNiagara();
	static std::vector<SceneDesc> All();
	// Empty optional when the id is unknown (caller prints usage).
	static std::optional<SceneDesc> Find(const std::string& id);
};

// ---------------------------------------------------------------------------
// Configurable pass set. RenderFrame consults these flags, so scene samples
// (or A/B experiments) toggle passes without touching pass code. Defaults
// are the full pipeline (previous behavior).
// ---------------------------------------------------------------------------
struct ScenePassConfig {
	bool directionalShadows = true; // CSM cascade passes
	bool spotShadows = true;        // spot atlas selection + tile passes
	bool forwardPlusPrepass = true; // depth prepass feeding the tile cull
	bool forwardPlusCull = true;    // tile light cull
	bool gfxFilter = true;          // GPU-driven per-view triangle filter
	bool filteredMotion = true;     // filtered motion-vector path
	bool motionVectors = true;      // motion-vector target pass
	bool motionBlur = true;         // HDR velocity-gather blur
	bool ssao = true;               // half-res depth AO
	bool bloom = true;              // HDR bloom chain
	bool exposure = true;           // histogram meter + CPU eye adaptation
	bool tonemap = true;            // HDR -> swapchain tonemap
	bool ui = true;                 // separated ImGui target + composite
	bool msaa = true;               // 2x MSAA Main (when device supports it)
	bool skydome = true;            // fullscreen-triangle sky in Main
	bool smaa = true;               // SMAA post-process chain (mode via UI AA combo)
	bool deferredShadows = true;    // tiled/zBin deferred-shadow pipeline (H toggles at runtime)

	static ScenePassConfig All() { return ScenePassConfig{}; }
	static ScenePassConfig Minimal() {
		ScenePassConfig c{};
		c.directionalShadows = false;
		c.spotShadows = false;
		c.forwardPlusPrepass = false;
		c.forwardPlusCull = false;
		c.gfxFilter = false;
		c.filteredMotion = false;
		c.motionVectors = false;
		c.motionBlur = false;
		c.ssao = false;
		c.bloom = false;
		c.exposure = false;
		c.ui = false;
		c.msaa = false;
		c.skydome = false;
		c.smaa = false;
		c.deferredShadows = false;
		return c;
	}
};

// ---------------------------------------------------------------------------
// Debug-UI dock position. The panel is pinned (via SetNextWindowPos/Size)
// instead of floating: Right/Left are full-height side bars, the four
// corners are floating overlays. U cycles through them at runtime.
// ---------------------------------------------------------------------------
enum class UiDock : int {
	Right = 0,
	Left = 1,
	TopLeft = 2,
	TopRight = 3,
	BottomLeft = 4,
	BottomRight = 5,
};

// ---------------------------------------------------------------------------
// Antialiasing mode (ImGui combo). MSAA 2x and SMAA are mutually exclusive:
// running SMAA on top of an MSAA-resolved image wastes the resolve without
// adding subpixel coverage (SMAA S2x/4x are separate, unported modes).
// ---------------------------------------------------------------------------
enum class AaMode : int {
	Off = 0,
	Msaa2x = 1,
	Smaa1x = 2,
	SmaaT2x = 3,
};

// ---------------------------------------------------------------------------
// Main-view debug visualization (F1..F5). Lit is the full PBR path; the rest
// replace the opaque leg with a flat debug program so lighting/culling/LOD
// artifacts bisect out in one keypress. Meshlets/Lods reuse the flat
// meshDebugProgram (per-cluster / per-LOD colors); Unlit/Cascades have their
// own fragment shaders pairing with default.vert.hlsl.
// ---------------------------------------------------------------------------
enum class DebugView : int {
	Lit = 0,      // F1: full PBR (default_lit)
	Unlit = 1,    // F2: baseColor + emissive, no lighting (default_unlit)
	Meshlets = 2, // F3: per-cluster colors, bypasses GPU cull (meshdebug)
	Cascades = 3, // F4: shadow cascade coverage (default_cascade_debug)
	Lods = 4,     // F5: LOD index by color (flat, direct draws)
};

#if defined(DS_DEBUG_VIZ)
// ---------------------------------------------------------------------------
// Deferred-shadow debug visualizers (paper sections 5.3/5.4; debug builds
// only — DS_DEBUG_VIZ, see app/CMakeLists.txt + CompileShaders.cmake).
// Rendered in default_lit.frag.hlsl, blended over the lit image or
// replacing it. Numeric values must match the DS_VIZ_* defines in
// engine/shaders/deferredshadow.hlsli.
// ---------------------------------------------------------------------------
enum class DsDebugViz : int {
	Off = 0,            // visualizer off (lit image untouched)
	CountApprox = 1,    // 5.4a: phase-2 approximated lights per Shadow Tile
	CountAccurate = 2,  // 5.4b: phase-3 early-list size per Shadow Tile
	CountFinal = 3,    // phase-4 final-list size per Shadow Tile
	CountDiff = 4,      // 5.4c: approx - accurate (where phase 2 is weakest)
	CullStep = 5,      // 5.3a: last culling step passed by the selected light
	LightList = 6,      // 5.3b: which light list holds the selected light
	LightVis = 7,       // selected light per-pixel visibility byte
};
#endif

// Procedural demo lights (grid of points + a few orbiting spots).
// Point lights are pure GPU lights (no scene nodes / emissive balls);
// the ImGui overlay draws them as colored points + indices.
struct ForwardPlusDemo {
	bool enabled = true;
	// Grid defaults sit at the UI slider maxima (12x8x12 -> 1152 grid
	// lights, clamped to FP_MAX_LIGHTS minus the spot reserve): the dense
	// crowd is the stress case the deferred-shadow pipeline is built for.
	int gridX = 12;
	int gridY = 8;
	int gridZ = 12;
	float spacingX = 3.0f;
	float spacingY = 2.2f;
	float spacingZ = 1.8f;
	float baseY = 1.2f;
	float range = 9.0f;
	float intensity = 600.0f;
	bool animate = true;
	float speed = 0.6f;
	int spotCount = 4;
	float spotRange = 100.0f;
	float spotIntensity = 900.0f;
	// Spot shadows: per-light opt-in flag (demo sets it on all spots) +
	// frame upper limit + screenspace/distance culling. Points never shadow.
	bool spotShadowsEnabled = true;
	int spotShadowMax = 4;               // upper limit, clamped to kMaxSpotShadows
	float spotShadowMaxDistance = 45.0f; // hard cutoff on camera->light distance (m)
	// Light cookies: per-spot gobo projected through the cone; spot s gets
	// pattern s % kCookiePatternCount (soft disc / blinds / starburst).
	bool cookiesEnabled = true;
	// Light functions: procedural per-light intensity curve on the spots
	// (flicker / pulse / cone bands), animated with frame time.
	bool lightFunctionsEnabled = true;
	int functionType = smvk::kForwardPlusFunction_Flicker;
	float functionSpeed = 0.6f;
	float functionScale = 0.65f;
	// Debug overlay: project light positions into the ImGui pass and draw
	// them as colored points with their light-buffer index.
	bool showLightPoints = true;
	bool showLightNumbers = true;
	float lightPointSize = 6.0f; // ImGui circle radius (px)
	int maxLightLabels = 256;    // cap text draws (points are all drawn)
};

struct FrameUniforms
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewProj;

	glm::mat4 invView;
	glm::mat4 invProj;
	glm::mat4 invViewProj;
	glm::mat4 prevViewProj; // last frame's proj*view (post Y-flip) – motion vectors

	glm::vec3 cameraPos;
	float     time;

	glm::vec2 viewportSize;
	glm::vec2 invViewportSize;

	float nearPlane;
	float farPlane;
	float exposure;
	// SSAO consumption (lighting-pass ambient): full-res composed AO
	// (upscaled by the SsaoCompose pass with depth authority, see ao.hlsli).
	// Must match HLSL FrameUniforms in common.hlsli — offsets asserted below.
	uint32_t aoTextureIndex = UINT32_MAX; // full-res composed AO bindless
	float aoIntensity = 0.0f;            // ambient composite strength
	// Explicit pad so env lands at offset 512 (16-byte aligned), matching
	// HLSL FrameUniforms::aoPad. Without this the HLSL compiler packs env
	// at offset 500, which is both a spirv-val Block-layout violation for
	// PhysicalStorageBuffer and a CPU/GPU desync (garbage sun/IBL/fog).
	float aoPad_[3] = {};
	// Configurable skydome IBL + fog (matches HLSL FrameUniforms.env).
	smvk::EnvironmentFrameData env;
	// ---- Deferred shadows (tiled/zBin pipeline, see
	// smvk::DeferredShadows + deferredshadow*.hlsli). Raw BDAs: the HLSL
	// side declares matching vk::BufferPointer fields. dsEnabled flips the
	// default_lit consumer between the deferred lists and the inline
	// forward+ shadow path (A/B). ----
	uint64_t dsTileState = 0;   // 640 per-tile {slot, count, approx, early}
	uint64_t dsLightList = 0;   // 648 final packed entries
	uint64_t dsVisibility = 0;  // 656 64B visibility slots
	uint32_t dsTileCountX = 0;  // 664 8px shadow tiles per row
	uint32_t dsEnabled = 0;     // 668 1 = shade from the deferred lists
#if defined(DS_DEBUG_VIZ)
	// ---- Deferred-shadow debug visualizers (paper 5.3/5.4, debug builds
	// only — CMake defines DS_DEBUG_VIZ for the App target and for DXC in
	// Debug trees; release compiles fields + UI + shader paths out, so
	// the release layout is the 672 B prefix of this one). Tile-state
	// counts + per-light culling/list diagnostics blended over the lit
	// image in default_lit. dsTileMasks/dsTileGroupMasks are the phase-1
	// (CPU) and phase-2 (GPU) TileGroup light masks the light-scoped
	// modes re-query; dsDebug* select mode / light / flags / heat scale. ----
	uint64_t dsTileMasks = 0;      // 672 phase-1 CPU TileGroup masks (viz)
	uint64_t dsTileGroupMasks = 0; // 680 phase-2 GPU TileGroup masks (viz)
	uint32_t dsDebugMode = 0;      // 688 app::DsDebugViz (0 = off)
	uint32_t dsDebugLight = 0;     // 692 selected light (sorted index)
	uint32_t dsDebugFlags = 0;     // 696 bit0 = blend over lit
	uint32_t dsDebugCountCeil = 32; // 700 count-heat ceiling (lights)
#endif
};
static_assert(sizeof(FrameUniforms) % 16 == 0);
// CPU/HLSL ABI: total-size alignment is not enough — each field offset must
// match common.hlsli FrameUniforms (float4-packed, env = 8 x float4). The
// visualizer tail (DS_DEBUG_VIZ, debug trees only) keeps the release
// layout a strict 672 B prefix of the debug one.
#if defined(DS_DEBUG_VIZ)
static_assert(sizeof(FrameUniforms) == 7 * 64 + 16 + 16 + 32 + 128 + 32 + 32);
#else
static_assert(sizeof(FrameUniforms) == 7 * 64 + 16 + 16 + 32 + 128 + 32);
#endif
static_assert(offsetof(FrameUniforms, view) == 0);
static_assert(offsetof(FrameUniforms, proj) == 64);
static_assert(offsetof(FrameUniforms, viewProj) == 128);
static_assert(offsetof(FrameUniforms, invView) == 192);
static_assert(offsetof(FrameUniforms, invProj) == 256);
static_assert(offsetof(FrameUniforms, invViewProj) == 320);
static_assert(offsetof(FrameUniforms, prevViewProj) == 384);
static_assert(offsetof(FrameUniforms, cameraPos) == 448);
static_assert(offsetof(FrameUniforms, time) == 460);
static_assert(offsetof(FrameUniforms, viewportSize) == 464);
static_assert(offsetof(FrameUniforms, invViewportSize) == 472);
static_assert(offsetof(FrameUniforms, nearPlane) == 480);
static_assert(offsetof(FrameUniforms, farPlane) == 484);
static_assert(offsetof(FrameUniforms, exposure) == 488);
static_assert(offsetof(FrameUniforms, aoTextureIndex) == 492);
static_assert(offsetof(FrameUniforms, aoIntensity) == 496);
static_assert(offsetof(FrameUniforms, env) == 512);
static_assert(offsetof(FrameUniforms, dsTileState) == 640);
static_assert(offsetof(FrameUniforms, dsLightList) == 648);
static_assert(offsetof(FrameUniforms, dsVisibility) == 656);
static_assert(offsetof(FrameUniforms, dsTileCountX) == 664);
static_assert(offsetof(FrameUniforms, dsEnabled) == 668);
#if defined(DS_DEBUG_VIZ)
static_assert(offsetof(FrameUniforms, dsTileMasks) == 672);
static_assert(offsetof(FrameUniforms, dsTileGroupMasks) == 680);
static_assert(offsetof(FrameUniforms, dsDebugMode) == 688);
static_assert(offsetof(FrameUniforms, dsDebugLight) == 692);
static_assert(offsetof(FrameUniforms, dsDebugFlags) == 696);
static_assert(offsetof(FrameUniforms, dsDebugCountCeil) == 700);
#endif

// Resources
struct AppResources {
	smvk::Holder<smvk::BufferHandle> frameUniforms;
	smvk::Holder<smvk::BufferHandle> lightsBuffer;
	smvk::Holder<smvk::BufferHandle> materialsBuffer;
	smvk::Holder<smvk::BufferHandle> directionalCascadeBuffer; // GpuDirectionalLightDescription per frame
	smvk::Holder<smvk::BufferHandle> fpLightsBuffer; // ForwardPlusLight * FP_MAX_LIGHTS per frame
	smvk::LogicalBufferHandle fpLightsLogical{}; // rendergraph alias (stable: buffer never resized)
	smvk::Holder<smvk::BufferHandle> spotShadowBuffer; // GpuCascadeDescription * kMaxSpotShadows per frame
	smvk::LogicalBufferHandle spotShadowLogical{}; // rendergraph alias (stable: buffer never resized)
	smvk::Holder<smvk::ShaderModuleHandle> vertShader;
	smvk::Holder<smvk::ShaderModuleHandle> fragShader; // default_lit (F1)
	smvk::ShaderProgram mainProgram;
	// Debug-view fragment programs (F2/F4): same default.vert VS, flat FS.
	// Loaded alongside main; missing modules leave the program at
	// shaderCount 0 and the view falls back to Lit.
	smvk::Holder<smvk::ShaderModuleHandle> fragUnlitShader;   // default_unlit (F2)
	smvk::Holder<smvk::ShaderModuleHandle> fragCascadeShader; // default_cascade_debug (F4)
	smvk::ShaderProgram unlitProgram;
	smvk::ShaderProgram cascadeProgram;
	// Alpha-tested shadow-depth program for MASK cutout casters
	// (shadow.vert mainVS + shadow.frag mainPS). Opaque casters keep
	// the engine builder's depth-only program.
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedVertShader;
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedFragShader;
	smvk::ShaderProgram shadowMaskedProgram;
	// Alpha-tested depth-prepass program for MASK cutout
	// (default.vert mainVS + shadow.frag mainPS): writes cutout-accurate
	// depth so background behind foliage holes survives the main pass.
	// Reuses the main VS and masked-shadow FS modules (no new shaders).
	smvk::ShaderProgram prepassMaskedProgram;
	// NOTE: shadow/bloom/tonemap/forwardplus programs + modules are owned by
	// their engine builders (CascadedShadowAtlas / BloomChain / Tonemapper /
	// ForwardPlus).
};

struct ViewportResources {
	smvk::CascadedShadowAtlas directionalShadows; // atlas + default shadow program + Ensure
	smvk::SpotShadowAtlas spotShadows; // perspective atlas for shadowed spots + Ensure
	smvk::BloomChain bloom; // separated per-mip HDR chain + default down/up programs + Ensure
	smvk::Tonemapper tonemap; // default tonemap program + pass helper
	smvk::ForwardPlus forwardPlus; // tile buffers + cull/depth programs + Ensure
	smvk::DeferredShadows deferredShadows; // fixed-size ring buffers + 3 phase programs
	std::vector<smvk::LogicalImageViewHandle> swapImages;
	smvk::DepthTarget renderDepth; // scene depth at internal resolution + Ensure
	smvk::HdrTarget hdr; // HDR intermediate (image + view + bindless + logical) + Ensure
	smvk::MsaaTarget msaa; // transient 2x color+depth for Main, resolves to hdr
	smvk::MotionVectors motionVectors; // RG16F motion target + program + pass helper
	smvk::MotionBlur motionBlur; // blurred HDR output + velocity-gather program + pass
	smvk::ExposureMeter exposure; // histogram meter + compute program (auto-exposure)
	smvk::Ssao ssao; // half-res raw + blurred AO targets + programs + passes
	smvk::UiTarget ui; // separated UI pass target (ui scale) + Ensure
	smvk::UiComposite uiComposite; // UI -> swapchain alpha composite program + pass
	smvk::Smaa smaa; // LDR target + edges/weights/history + LUTs + programs (see smaa.h)
	bool msaaEnabled = true; // runtime toggle (effective only if device supports)
};

class SceneRenderer : public smvk::AppImpl {
	using Super = smvk::AppImpl;
public:
	void Configure(SceneDesc scene, ScenePassConfig passes);
	const SceneDesc& GetScene() const { return m_scene; }
	ScenePassConfig& Passes() { return m_passes; }
	// Debug: skip the cooked pak and import the scene straight from the
	// SceneDesc glTF path (loose content). A/B against the pak to tell
	// pak/meshlet bugs apart from import/draw-path bugs.
	void SetForceGltf(bool v) { m_forceGltf = v; }

protected:
	void UpdateCamera(float dt);
	void SetUIMode(bool ui);
	// Keeps the projection aspect in sync with the rendered (internal)
	// extent. Must run whenever the swapchain extent or render scale
	// changes (init + resize); otherwise the image stretches and
	// projection-derived data (CSM splits/frusta, selection) uses a stale
	// aspect. Takes the internal extent (not the swapchain): the scene
	// rasterizes at renderExtent, and LOD pixel budgets, the GPU filter
	// dimensions and motion-vector UVs all derive from it, so the aspect
	// must describe the same image (differs from the swapchain ratio by
	// rounding once renderScale != 1).
	// NOTE: Camera::setPerspective forwards to MakeReversedZProjRH, whose
	// fovY is in radians and whose depth maps [near, far] -> [1, 0]
	// (reversed-Z: near maps to depth 1, far to 0, like the old infinite
	// form, whose limit it recovers as far -> infinity).
	void UpdateCameraAspect(smvk::Extent2D extent) {
		if (extent.width == 0 || extent.height == 0)
			return;
		m_camera.setPerspective(
			glm::radians(kCameraFovYDegrees),
			float(extent.width) / float(extent.height),
			default_near_clip, kMainViewFarPlane
		);
	}
	bool m_uiMode = false;
	UiDock m_uiDock = UiDock::Right; // side-pinned panel; U cycles corners/sides
	bool m_uiDockDirty = true; // true = apply dock pos/size once, then let user resize stick
	// Configurable resolution knobs (see "Resolution config" in the .cpp).
	float m_renderScale = 1.0f;
	float m_uiScale = 1.0f;
	float m_uiDpiScale = 1.0f; // synced from ImGuiOverlay at init (auto = monitor DPI)
	// VSync / presentation mode (overlay combo; applied via swapchain Recreate).
	smvk::PresentMode m_presentMode = smvk::PresentMode::Mailbox; // default: no VSync
	// Frame timing: CPU wall time vs GPU execution time (EMA-smoothed ms
	// for the debug overlay). GPU lags 1-2 frames (retired-frame readback).
	double m_cpuMsSmooth = 0.0;
	double m_gpuMsSmooth = 0.0;
	bool m_gpuTimingValid = false;
	// Acquire wait (in-flight fence + vkAcquireNextImageKHR) plus the
	// vkQueuePresentKHR throttle (vsync-paced under FIFO): idle time,
	// reported separately so CPU stays busy-only.
	double m_waitMsSmooth = 0.0;

	void EarlyUpdate() override {
		Super::EarlyUpdate();
	}

	void Update() {
		Super::Update();
		HandleShortcuts();
		if (!m_uiMode) {
			UpdateCamera(delta_time_s);
		}
	}

	// Global feature shortcuts (work in both FPS and UI modes so A/B
	// comparisons don't require opening the panel; see the footer readout
	// in RenderFrame for the full list). WASDQE/Space/Shift/Ctrl are
	// reserved for camera movement and are never used here.
	void HandleShortcuts() {
		if (m_input.isKeyPressed(GLFW_KEY_I)) {
			SetUIMode(!m_uiMode);
		}
		// U cycles the debug-panel dock: Right -> Left -> corners.
		if (m_input.isKeyPressed(GLFW_KEY_U)) {
			CycleUiDock();
		}
		if (m_input.isKeyPressed(GLFW_KEY_M)) {
			m_motionBlurEnabled = !m_motionBlurEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_O)) {
			m_ssaoEnabled = !m_ssaoEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_B)) {
			m_bloomEnabled = !m_bloomEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_X)) {
			m_autoExposureEnabled = !m_autoExposureEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_V)) {
			m_showMotionDebug = !m_showMotionDebug;
		}
		if (m_input.isKeyPressed(GLFW_KEY_L)) {
			m_lodEnabled = !m_lodEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_F)) {
			m_fpDemo.enabled = !m_fpDemo.enabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_H)) {
			m_dsEnabled = !m_dsEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_G)) {
			m_useGfxFilter = !m_useGfxFilter;
		}
		if (m_input.isKeyPressed(GLFW_KEY_C)) {
			m_fpDemo.cookiesEnabled = !m_fpDemo.cookiesEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_K)) {
			m_env.skyEnabled = !m_env.skyEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_T)) {
			m_transparentMode = (m_transparentMode == smvk::TransparentMode::Blended)
				? smvk::TransparentMode::Dithered
				: smvk::TransparentMode::Blended;
		}
		// Z cycles AA: Off -> MSAA 2x -> SMAA 1x -> SMAA T2x. 1..4 jump
		// directly to a mode.
		if (m_input.isKeyPressed(GLFW_KEY_Z)) {
			m_aaMode = static_cast<AaMode>((static_cast<int>(m_aaMode) + 1) % 4);
		}
		if (m_input.isKeyPressed(GLFW_KEY_1)) {
			m_aaMode = AaMode::Off;
		}
		if (m_input.isKeyPressed(GLFW_KEY_2)) {
			m_aaMode = AaMode::Msaa2x;
		}
		if (m_input.isKeyPressed(GLFW_KEY_3)) {
			m_aaMode = AaMode::Smaa1x;
		}
		if (m_input.isKeyPressed(GLFW_KEY_4)) {
			m_aaMode = AaMode::SmaaT2x;
		}
		if (m_input.isKeyPressed(GLFW_KEY_F7)) {
			ExportNeutralLut();
			m_captureQueued = true;
		}
		// Debug views (F1..F5): F1 lit, F2 unlit, F3 meshlets, F4 shadow
		// cascades, F5 LODs by color.
		if (m_input.isKeyPressed(GLFW_KEY_F1)) {
			m_debugView = DebugView::Lit;
		}
		if (m_input.isKeyPressed(GLFW_KEY_F2)) {
			m_debugView = DebugView::Unlit;
		}
		if (m_input.isKeyPressed(GLFW_KEY_F3)) {
			m_debugView = DebugView::Meshlets;
		}
		if (m_input.isKeyPressed(GLFW_KEY_F4)) {
			m_debugView = DebugView::Cascades;
		}
		if (m_input.isKeyPressed(GLFW_KEY_F5)) {
			m_debugView = DebugView::Lods;
		}
#if defined(DS_DEBUG_VIZ)
		// Deferred-shadow visualizer cycle (F6 or Y): Off -> counts -> diff ->
		// light-scoped modes. Needs the pipeline on (H); see the panel's
		// "DS visualizers" block for the mode list. Y is the fallback key:
		// some keyboards/overlays remap or swallow function keys, never
		// letters.
		if (m_input.isKeyPressed(GLFW_KEY_F6) || m_input.isKeyPressed(GLFW_KEY_Y)) {
			m_dsDebugMode = static_cast<DsDebugViz>(
				(static_cast<int>(m_dsDebugMode) + 1)
				% (static_cast<int>(DsDebugViz::LightVis) + 1));
		}
#endif
	}

	void CycleUiDock() {
		m_uiDock = static_cast<UiDock>((static_cast<int>(m_uiDock) + 1) % 6);
		m_uiDockDirty = true;
	}
	void SetUiDock(UiDock dock) {
		if (m_uiDock != dock) {
			m_uiDock = dock;
			m_uiDockDirty = true;
		}
	}
	const char* UiDockName() const {
		switch (m_uiDock) {
		case UiDock::Right: return "Right";
		case UiDock::Left: return "Left";
		case UiDock::TopLeft: return "TopLeft";
		case UiDock::TopRight: return "TopRight";
		case UiDock::BottomLeft: return "BottomLeft";
		case UiDock::BottomRight: return "BottomRight";
		}
		return "Right";
	}

	void LateUpdate() {
		Super::LateUpdate();
	}

	bool Initialize() override;
	void Shutdown() override;

	FrameUniforms UpdateFrameUniforms(uint32_t currentFrame, smvk::Extent2D renderExtent, smvk::Extent2D swapExtent);
	smvk::GpuDirectionalLightDescription UpdateLights(uint32_t currentFrame);
	void UpdateMaterials(uint32_t currentFrame);

	// Procedural Forward+ demo lights (no scene nodes; see UpdateForwardPlusLights).
	uint32_t UpdateForwardPlusLights(uint32_t currentFrame, const FrameUniforms& frame);

	// ---- Deferred shadows (Square-Enix-style tiled/zBin pipeline, see
	// smvk::DeferredShadows) ----
	// Single gate for every consumer (FrameUniforms dsEnabled, phase-1
	// uploads, phases 2-4 dispatch, Main read hints) so they can never
	// disagree and feed the lighting pass stale buffer contents.
	bool DsActive() const {
		return m_passes.deferredShadows && m_dsEnabled
			&& m_viewportResources.deferredShadows.Valid()
			&& m_viewportResources.deferredShadows.ShadersValid();
	}
	// Phases 2-4 compute passes (called after the Forward+ tile cull
	// spot, i.e. over complete depth).
	void AppendDeferredShadowPhases(uint32_t frameIndex,
		uint64_t frameUniformsAddress, uint64_t fpLightsAddress,
		uint64_t spotShadowBufferAddress, uint32_t fpLightCount,
		smvk::LogicalImageViewHandle depthView,
		smvk::LogicalBufferHandle fpLightsLogical);

	void RebuildSwapViews();
	void DrawFrame() override;
	void RenderFrame(smvk::Framebuffer fb);

	// Error-driven LOD selection (see Engine/Asset/mesh_optimizer.h): the
	// scene references LOD0; cooked "/lodN" siblings are picked per instance
	// from their world-space simplifier error projected to pixels at the
	// instance distance. Returns the LOD mesh (LOD0 fallback when disabled
	// or uncooked). `outLod` receives the selected index for stats.
	smvk::StaticMesh* LodMeshForNode(NodeId node, uint32_t worldMesh,
		uint32_t* outLod = nullptr);
	// Cheap LOD index for filter/direct split decisions (cached chains, no
	// mesh fetch). Returns 0 when LODs are disabled or uncooked.
	uint32_t LodIndexForNode(NodeId node, uint32_t worldMesh) const;
	void ResetLodStats();
	// Runtime LOD switch: pixel budget for the projected simplifier error
	// (~1px = conservative, higher = more aggressive). Disabled = always LOD0.
	// Per-node decisions are cached in the World and refreshed on a
	// staggered-halves cadence (half the nodes per frame — see World::
	// LodIndexForNode); m_lodFrameInputs.frameId is the monotonic key.
	bool m_lodEnabled = true;
	smvk::LodFrameInputs m_lodFrameInputs{};
	float m_lodPixelBudget = 1.0f;      // mirrors m_lodFrameInputs.pixelBudget
	float m_lodViewportHeight = 1080.0f; // mirrors m_lodFrameInputs.viewportHeight
	uint64_t m_lodFrameId = 0;         // monotonic per-frame counter
	float m_lodBudgetApplied = -1.0f;  // budget at last InvalidateLodCache
	uint64_t m_lodDraws = 0;     // direct draws this frame
	uint64_t m_lodSwitched = 0;  // ... served by LOD1+
	uint32_t m_lodChainMeshes = 0; // world meshes with a cooked chain (per frame)
	uint32_t m_lodChainLods = 0;   // total cooked siblings (per frame)

	static constexpr float default_near_clip = 0.01f;
	static constexpr float kCameraFovYDegrees = 45.0f;
	// Finite far plane for the MAIN VIEW projection (reversed-Z). Scene
	// metadata (m_scene.farClip) stays per-scene for shadows/LOD; only the
	// projection is finite, so unprojecting far/sky depth yields finite
	// positions and the frustum far plane is functional.
	static constexpr float kMainViewFarPlane = 100000.0f;

	SceneDesc m_scene{};
	ScenePassConfig m_passes{};
	bool m_forceGltf = false;

	std::unique_ptr<smvk::ImGuiOverlay> m_imgui{};
	AppResources m_resources{};
	ViewportResources m_viewportResources{};
	ForwardPlusDemo m_fpDemo{};
	smvk::CookieLibrary m_cookies{}; // baked/engine-resource gobos (see cookies.h)
	uint32_t m_spotShadowCount = 0; // shadowed spots this frame (tiles used)
	int32_t m_spotShadowMapIndex = -1; // bindless spot atlas, -1 = none
	std::vector<smvk::GpuCascadeDescription> m_spotShadowTiles; // spot tiles this frame (feed the spot RenderViews)
	smvk::RenderViewSet m_renderViews{}; // per-frame virtual cameras: main view + shadow cascade/spot views
	// ---- GeometryFX-style per-view filter (clustered, see App/renderer.h) ----
	app::GfxRenderer m_gfxRenderer{};
	bool m_useGfxFilter = true; // false = legacy direct draws (A/B)
	bool m_useFilteredMotion = true; // false = motion stays fully direct
	// ---- Meshlet/cone debug views (see App/renderer.h debug programs) ----
	// m_debugView selects the Main opaque program (F1..F5); anything but Lit
	// bypasses the GPU filter with direct draws so culled data stays visible.
	DebugView m_debugView = DebugView::Lit;
	int m_debugMeshletMode = 0;   // 0 = random per meshlet, 1 = cone heat
	bool m_hideTransparent = false; // debug: skip all transparent draws (bisect blending/overdraw out)
	// Transparency technique for AlphaMode::Blend materials (default:
	// dithered screen-door, opaque state; optional classic alpha blend).
	smvk::TransparentMode m_transparentMode = smvk::TransparentMode::Dithered;
	bool m_debugCones = false;    // cone-axis line overlay (CPU batches)
	smvk::Holder<smvk::BufferHandle> m_debugLines; // pos-only cone line VB
	uint32_t m_debugLinesCap = 0; // verts allocated
	bool m_dumpIndirectAudit = false; // one-shot: dump GPU indirect cmds (stderr)
	bool m_debugDumpHzb = false;    // one-shot: dump HZB buffer contents (stderr)
	bool m_dumpFirstFrameHzb = false;      // opt-in: dump HZB once on the next frame (stalls)
	int m_dumpAtFrame = -1;                // opt-in (VULKANTOY_DS_DUMP=<frame>): one-shot deferred-shadow dump (stalls)
	void DumpDeferredShadows(uint32_t frameIndex); // post-Execute one-shot (stalls)
	// Headless A/B helpers: auto-capture at a fixed frame (env
	// VULKANTOY_CAPTURE=<n>; -1 = off) keyed on the render-frame counter.
	int m_captureAtFrame = -1;
	uint64_t m_frameCounter = 0;
	// ---- Debug culling freeze: hold the filter's (view, eye) + HZB while
	// the render camera moves freely, so culling decisions stay put and
	// their effect (missing survivors, holes) is inspectable. Captured on
	// the rising edge; auto-released on extent drift (a resized HZB would
	// rebuild empty while held -> garbage reads).
	bool m_freezeCullingView = false;
	bool m_frozenViewValid = false;
	glm::mat4 m_frozenViewProj{ 1.0f };
	glm::vec3 m_frozenEye{ 0.0f };
	smvk::Extent2D m_frozenExtent{ 0, 0 };
	app::ForwardPlusRenderer m_fpRenderer{};
	// ---- Deferred shadows (tiled/zBin pipeline; H toggles) ----
	bool m_dsEnabled = true;
#if defined(DS_DEBUG_VIZ)
	// ---- Deferred-shadow debug visualizers (paper 5.3/5.4; debug builds
	// only). Mode/light/overlay ride FrameUniforms (dsDebug*); the
	// light index refers to the SORTED light buffer (phase 1 sorts by
	// near distance AFTER the spot-shadow selection, so it differs from
	// the ImGui overlay's m_debugLights order). ----
	DsDebugViz m_dsDebugMode = DsDebugViz::Off;
	int m_dsDebugLight = 0;             // selected light (sorted index)
	bool m_dsDebugOverlay = true;       // blend over lit (vs flat replace)
	int m_dsDebugCountCeil = 32;         // count-heat ceiling (lights)
	uint32_t m_dsLightCount = 0;        // sorted lights this frame (UI clamp)
	// Selected light captured post-sort for the ImGui marker (sorted order).
	glm::vec3 m_dsDebugLightPos{ 0.0f };
	glm::vec3 m_dsDebugLightColor{ 1.0f };
	int m_dsDebugLightType = 0;         // 0=point, 1=spot
	float m_dsDebugLightRange = 0.0f;
	bool m_dsDebugLightValid = false;
#endif
	// Retired-slice counter readback for the debug overlay (list/vis slot
	// peaks, ~2 frames old — same lag as the exposure meter). Includes
	// the phase 3/4 classification histogram (telemetry).
	struct DsStats {
		uint32_t listSlots = 0;
		uint32_t visSlots = 0;
		uint32_t clsAllLit = 0;
		uint32_t clsAllShadow = 0;
		uint32_t clsPenumbra = 0;
		uint32_t sumEarly = 0;
		uint32_t sumFinal = 0;
		uint32_t dbgSpotCone = 0;
		uint32_t dbgSpotClip = 0;
	};
	DsStats m_dsStats{};
	std::vector<uint32_t> m_dsZBinStaging;  // 4 * kDsZBinCount
	std::vector<uint32_t> m_dsMaskStaging;  // per-TileGroup mask entries
	// ---- Motion vector pass: previous-frame state + debug view ----
	glm::mat4 m_prevViewProj{ 1.0f }; // last frame's viewProj (post Y-flip)
	// This frame's viewProj WITHOUT the SMAA T2x subpixel jitter (post
	// Y-flip). 2D ImGui overlays (light markers/labels) must project with
	// this: the UI layer is not jittered or temporally resolved, so the
	// jittered matrix would make every marker buzz +/-0.25 px per frame.
	glm::mat4 m_unjitteredViewProj{ 1.0f };
	bool m_prevViewProjValid = false; // false until the first frame completes
	std::vector<glm::mat4> m_prevGlobalTransforms; // last frame's node transforms
	bool m_showMotionDebug = false; // tonemap debug view: motion vectors
	float m_motionDebugScale = 16.0f; // debug view velocity amplification
	// ---- Motion blur pass (HDR velocity gather, see motionblur.h) ----
	bool m_motionBlurEnabled = true; // runs after Main, before Bloom/Tonemap (M)
	int m_motionBlurSamples = 12; // gather taps 1..32
	float m_motionBlurMaxRadius = 24.0f; // blur clamp (px)
	float m_motionBlurStrength = 1.0f; // shutter scale
	// ---- Artistic LUT grading (TPP-style 32^3 strip, see tonemap.h) ----
	// content/luts/*.png: 1024x32 sRGB-encoded strips. F7 exports the
	// neutral strip + a screenshot reference into exports/ for Photoshop.
	smvk::Holder<smvk::TextureHandle> m_lutTexture;
	uint32_t m_lutBindless = UINT32_MAX;
	std::string m_lutFile; // "" = off
	float m_lutIntensity = 1.0f;
	std::vector<std::string> m_lutFiles;
	// Screenshot capture: the tonemap pass writes one extra RGBA8_SRGB
	// target on the capture frame; the bytes read back to PNG after Execute.
	smvk::Holder<smvk::ImageHandle> m_captureImage;
	smvk::Holder<smvk::ImageViewHandle> m_captureView;
	smvk::LogicalImageViewHandle m_captureLogical{};
	smvk::Extent2D m_captureExtent{ 0, 0 };
	bool m_captureQueued = false; // emit the capture pass this frame
	bool m_captureReadbackPending = false; // read back after the submit (DrawFrame)

	void ScanLutFiles();
	bool LoadLut(const std::string& file);
	void ExportNeutralLut();
	void EnsureCaptureTarget(smvk::Extent2D extent);
	void ExportScreenshot(smvk::Extent2D extent);
	// ---- SMAA post-process antialiasing (see Engine/Rendergraph/smaa.h) ----
	// AA mode combo drives everything (MSAA 2x only runs in its own mode):
	// Off / Msaa2x / Smaa1x / SmaaT2x. T2x adds a quarter-pixel alternating
	// camera jitter (UpdateFrameUniforms) and the temporal resolve.
	AaMode m_aaMode = AaMode::SmaaT2x;
	// Global T2x toggle (see SmaaT2xState in smaa.h). The phase belongs to
	// the recorded-frame sequence, not the swapchain image: every T2x
	// record writes history[parity] and blends against history[parity^1]
	// (the immediately preceding record, matching the global prevViewProj
	// the motion pass diffs against). Under VSync FIFO this displays
	// strict P0,P1,P0,P1; Mailbox/Immediate drops only skip a phase on
	// screen while each resolve stays a correct consecutive-record blend.
	// Invalidated on rebuild and on any AA-mode change (history targets
	// recreated / stale data from an older T2x run).
	smvk::SmaaT2xState m_smaaT2x;
	// Last AA mode seen by RenderFrame; any change invalidates the history
	// (covers T2x -> 1x/Off -> T2x round-trips reusing ancient data).
	AaMode m_smaaLastMode = AaMode::SmaaT2x;
	// This record's snapshot (filled at the top of RenderFrame so
	// UpdateFrameUniforms, the pass builders and the overlay all agree).
	smvk::SmaaFrameParams m_smaaFrameParams{};
	// True when this record emitted the T2x chain (end-step input).
	bool m_smaaFrameRanT2x = false;

	bool SmaaRequested() const {
		return m_passes.smaa
			&& (m_aaMode == AaMode::Smaa1x || m_aaMode == AaMode::SmaaT2x);
	}
	// Motion vectors are a hard requirement (the neighborhood pass packs
	// velocity into alpha even in 1x mode), and so are the LUTs + programs.
	bool SmaaActive() const {
		return SmaaRequested() && m_viewportResources.smaa.Valid()
			&& m_passes.motionVectors && m_viewportResources.motionVectors.Valid();
	}
	bool SmaaT2xActive() const {
		return SmaaActive() && m_aaMode == AaMode::SmaaT2x;
	}
	// UNORM sibling of the swapchain surface format. Used by the SMAA LDR
	// chain and the UI target: both hold gamma-encoded bytes that must be
	// stored raw (an _SRGB target would double-encode them on store); the
	// channel order matches the swapchain so the decode-on-write (smaa.h /
	// ui_composite) restores the tonemapper's values. Exotic surface
	// formats fall back to BGRA8.
	smvk::Format LdrUnormSibling() const {
		const smvk::Format surface = m_swapchain->GetSurfaceFormat();
		if (surface == smvk::Format::BGRA8_SRGB || surface == smvk::Format::BGRA8_UNorm)
			return smvk::Format::BGRA8_UNorm;
		if (surface == smvk::Format::RGBA8_SRGB || surface == smvk::Format::RGBA8_UNorm)
			return smvk::Format::RGBA8_UNorm;
		return smvk::Format::BGRA8_UNorm;
	}
	// True when the chosen surface format is hardware-sRGB: swap-writing
	// passes then output linear (the store encodes); manual encode/decode
	// is keyed off this for the UNORM fallbacks.
	bool SwapchainIsSrgb() const {
		const smvk::Format surface = m_swapchain->GetSurfaceFormat();
		return surface == smvk::Format::BGRA8_SRGB || surface == smvk::Format::RGBA8_SRGB;
	}
	// ---- Auto-exposure (histogram meter + CPU eye adaptation, see
	// exposure.h). Meters the (possibly motion-blurred) scene HDR before
	// Bloom/Tonemap; the smoothed exposure feeds the tonemap push next
	// frame (1-frame lag) and scales the bloom threshold/knee so bloom
	// strength stays constant as exposure shifts.
	bool m_autoExposureEnabled = true;
	float m_manualExposure = 0.001f; // used when auto is off (== TONEMAP_EXPOSURE)
	float m_exposureAdapted = 0.001f; // smoothed exposure (seeded to manual)
	float m_exposureTarget = 0.001f; // last computed target (debug readout)
	float m_exposureEvBias = 0.0f; // user EV trim (-4..+4)
	float m_exposureSpeedToDark = 1.0f; // adapt rate when exposure rises (/s)
	float m_exposureSpeedToBright = 3.0f; // adapt rate when exposure falls (/s)
	float m_exposureMin = 0.00001f; // absolute clamp (dark interior safe)
	float m_exposureMax = 0.05f; // absolute clamp (night-sky safe)
	// CPU copy of the last histogram for the debug plot (filled by the
	// post-Execute readback; floats for ImGui::PlotHistogram).
	float m_exposureBinsUi[smvk::kExposureBinCount] = {};
	uint32_t m_exposureSamples = 0; // metered pixels last frame
	// Flight slice this frame's histogram lives in (recorded in
	// RenderFrame; the download after Execute reads the same slice).
	uint32_t m_exposureFrame = 0;
	// Effective tonemap exposure: adapted when auto is on, the pass is in
	// the set and the meter is live; manual otherwise.
	float EffectiveExposure() const {
		const bool active = m_autoExposureEnabled && m_passes.exposure &&
			m_viewportResources.exposure.Valid();
		return active ? m_exposureAdapted : m_manualExposure;
	}
	void UpdateAutoExposure(uint32_t frameIndex); // download slice BEFORE zero + Adapt (call at record time)
	// ---- Bloom chain (HDR down/up, see bloom.h) ----
	bool m_bloomEnabled = true; // runs before Tonemap; gated with m_passes.bloom (B)
	// ---- SSAO pass (half-res depth-only AO, see ssao.h) ----
	bool m_ssaoEnabled = true; // runs before Main, composites in opaque ambient
	float m_ssaoRadius = 0.5f; // view-space sample radius (m)
	float m_ssaoBias = 0.025f; // self-occlusion bias
	float m_ssaoIntensity = 1.0f; // ambient composite strength (default_lit.frag)
	float m_ssaoMaxDistance = 25.0f; // fade-out distance (m)
	int m_ssaoSamples = 16; // hemisphere taps 1..32
	float m_ssaoBlurSharpness = 8.0f; // blur depth edge weight
	// ---- Forward+ light debug overlay (drawn in the ImGui UI pass) ----
	// CPU copy of the last light set (positions/colors/types), projected
	// through the frame viewProj and drawn as points + buffer indices.
	struct DebugLight {
		glm::vec3 position{};
		glm::vec3 color{}; // linear RGB
		int type = 0;      // 0=point, 1=spot (kForwardPlusPoint/Spot)
	};
	std::vector<DebugLight> m_debugLights;
	smvk::EnvironmentSettings m_env{};
	smvk::Skydome m_skydome{};
	CameraPositioner_FirstPerson m_cameraPositioner = CameraPositioner_FirstPerson(
		glm::vec3(0.0f, 1.0f, 0.0f), //pos
		glm::vec3(0.0f, 1.0f, 1.0f), //target
		glm::vec3(0.0f, 1.0f, 0.0f) //up
	);
	Camera m_camera = Camera(m_cameraPositioner);
};

} // namespace app
