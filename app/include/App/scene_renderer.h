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
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/Camera/camera.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/lightmap.h>
#include <Engine/GI/probe_volume.h>
#include <Engine/GI/regular_volume.h>
#include <Engine/ImGui/imgui_overlay.h>
#include <Engine/Renderer/cookies.h>
#include <Engine/Renderer/cubemap.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/Renderer/texture_streamer.h>
#include <Engine/Renderer/texture_feedback.h>
#include <Engine/Rendergraph/bloom.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <Engine/Rendergraph/deferredshadow.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/dlss.h>
#include <Engine/Rendergraph/dof.h>
#include <Engine/Rendergraph/environment.h>
#include <Engine/Rendergraph/evsm.h>
#include <Engine/Rendergraph/exposure.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/motionblur.h>
#include <Engine/Rendergraph/motionvector.h>
#include <Engine/Rendergraph/msaa.h>
#include <Engine/Rendergraph/particles.h>
#include <Engine/Rendergraph/smaa.h>
#include <Engine/Rendergraph/spotshadow.h>
#include <Engine/Rendergraph/ssao.h>
#include <Engine/Rendergraph/gtao.h>
#include <Engine/Rendergraph/ssr.h>
#include <Engine/Rendergraph/volumetric.h>
#include <Engine/Rendergraph/tonemap.h>
#include <Engine/Rendergraph/ui.h>
#include <Engine/Utils/thread_pool.h>

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
	// Procedural builtin scene tag (nullptr = load gltf/pak). Currently
	// supported: "layers-cube" (vertex-painted layered-material test cube).
	const char* builtin = nullptr;
	// True when camPos/camTarget/farClip are a curated viewpoint for this
	// scene (all SceneRegistry rows). The external-override entry point
	// (app/src/main.cpp, always SetSceneOverride(true)) adopts the
	// registry row matching the pak/gltf stem so known scenes keep their
	// close-up view + tight far clip (cascade resolution, LOD distances);
	// only truly unknown scenes fall back to whole-bounds orbit framing
	// (which parks the camera radius*2.2 away with a generic far clip —
	// the "spawns far / lowest LOD / blurry EVSM" combo).
	bool authoredView = false;
};

struct SceneRegistry {
	static const SceneDesc& Sponza();
	static const SceneDesc& Bistro();
	static const SceneDesc& BistroNiagara();
	static const SceneDesc& Scanlands();
	static const SceneDesc& LayersCube();
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
	bool dof = true;                // depth-of-field (low single-pass / HQ bokeh)
	bool ssr = true;                // screen-space reflections (depth-reconstructed)
	bool volumetric = true;         // raymarched GI-probe volumetric fog (HDR composite)
	bool ssao = true;               // AO master switch (SSAO or GTAO per AoMode)
	bool bloom = true;              // HDR bloom chain
	bool exposure = true;           // histogram meter + CPU eye adaptation
	bool tonemap = true;            // HDR -> swapchain tonemap
	bool ui = true;                 // separated ImGui target + composite
	bool msaa = true;               // 2x MSAA Main (when device supports it)
	bool skydome = true;            // fullscreen-triangle sky in Main
	bool smaa = true;               // SMAA post-process chain (mode via UI AA combo)
	bool deferredShadows = true;    // tiled/zBin deferred-shadow pipeline (H toggles at runtime)
	bool particles = true;          // GPU particle system (compute emitters + tiled-lit billboards)

	static ScenePassConfig All() { return ScenePassConfig{}; }
	static ScenePassConfig Minimal() {		ScenePassConfig c{};
		c.directionalShadows = false;
		c.spotShadows = false;
		c.forwardPlusPrepass = false;
		c.forwardPlusCull = false;
		c.gfxFilter = false;
		c.filteredMotion = false;
		c.motionVectors = false;
		c.motionBlur = false;
		c.dof = false;
		c.ssr = false;
		c.volumetric = false;
		c.ssao = false;
		c.bloom = false;
		c.exposure = false;
		c.ui = false;
		c.msaa = false;
		c.skydome = false;
		c.smaa = false;
		c.deferredShadows = false;
		c.particles = false;
		return c;
	}
	// Bisection config: filtered Main + HZB + tonemap only (depth prepass
	// feeds the occluder/HZB chain, the GPU filter picks draws, Main shades
	// survivors). Everything else off.
	static ScenePassConfig FilterOnly() {
		ScenePassConfig c = Minimal();
		c.forwardPlusPrepass = true;
		c.gfxFilter = true;
		return c;
	}
};

// ---------------------------------------------------------------------------
// Debug-UI dock position. Fixed full-height side bar (no floating): Right
// or Left. U toggles the side. The panel position/size is enforced every
// frame (ImGuiWindowFlags_NoMove | NoResize) with a TabBar inside
// (Quality / Debug / Stats).
// ---------------------------------------------------------------------------
enum class UiDock : int {
	Right = 0,
	Left = 1,
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
// Ambient / GI source selection (N cycles): exactly one active source set
// for clean isolation — All is the production state, the rest isolate a
// single GI contribution for bisection. The ambient is the only thing
// this drives; the visible sky dome (K) and the direct sun / cascaded
// shadows are unaffected (Off = direct light only).
//   All:        every baked source (per-pixel priority: lightmap >
//               regular grid > probe volume); the sky IBL is the
//               FALLBACK for uncovered pixels only, never additive
//   Lightmaps:  baked 2D SG lightmaps ONLY (static draws)
//   Probes:     baked 3D probes ONLY (regular grids + octree volume —
//               the palette view splits grid vs volume per pixel)
//   SkyIbl:     analytic sky IBL only (all baked sources disabled)
//   Off:        no GI/ambient contribution at all (pure direct sun frame)
// ---------------------------------------------------------------------------
enum class AmbientMode : int {
	All = 0,        // baked GI; sky IBL fallback for uncovered pixels
	Lightmaps = 1,  // baked lightmaps only
	Probes = 2,     // baked probe systems only (grids + volume)
	SkyIbl = 3,     // analytic sky IBL only
	Off = 4,        // direct light only
};
inline const char* AmbientModeName(AmbientMode m) {
	switch (m) {
	case AmbientMode::All: return "All (baked GI; sky fallback)";
	case AmbientMode::Lightmaps: return "Lightmaps only";
	case AmbientMode::Probes: return "Probes only (grids + volume)";
	case AmbientMode::SkyIbl: return "Sky IBL only";
	case AmbientMode::Off: return "Off (direct only)";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// Main-view debug visualization (F1..F6, F9/F10). Lit is the full PBR path;
// the rest replace the opaque leg with a flat debug program so
// lighting/culling/LOD artifacts bisect out in one keypress. Meshlets/Lods
// reuse the flat meshDebugProgram (per-cluster / per-LOD colors);
// Unlit/Cascades/VertexColors/UV have their own fragment shaders pairing
// with default.vert.hlsl.
// ---------------------------------------------------------------------------
enum class DebugView : int {
	Lit = 0,      // F1: full PBR (default_lit)
	Unlit = 1,    // F2: baseColor + emissive, no lighting (default_unlit)
	Meshlets = 2, // F3: per-cluster colors, bypasses GPU cull (meshdebug)
	Cascades = 3, // F4: shadow cascade coverage (default_cascade_debug)
	Lods = 4,     // F5: LOD index by color (flat, direct draws)
	VertexColors = 5, // F6: vertex-color layer weights as false color (default_vertexcolor_debug)
	Uv0 = 6,      // F9: base UV (TEXCOORD0) checker + gradient (default_uv0_debug)
	Uv1 = 7,      // F10: lightmap atlas UV (uv2) checker + seam glow (default_uv1_debug)
	Pom = 8,      // F11: parallax occlusion mapping (default_pom)
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

// Cooked scene lights (KHR_lights_punctual via the pak/glTF import).
// When non-empty and enabled, these REPLACE the procedural demo grid:
// the sun drives the environment/directional light and the points/spots
// upload directly as the Forward+ set (static, no orbit/bob). Empty on
// scenes without authored lights (e.g. Sponza) — the procedural path is
// then the fallback.
struct CookedSceneLights {
	bool hasSun = false;
	glm::vec3 sunDirToSun{ 0.0f, 1.0f, 0.0f };
	glm::vec3 sunColor{ 1.0f, 0.95f, 0.90f };
	float sunIntensity = 10000.0f;
	// World-space static lights, spots FIRST (per-tile lists keep only
	// the first FP_MAX_PER_TILE passing lights — cone lights must not
	// compete with the point crowd for those slots).
	std::vector<smvk::ForwardPlusLight> pointSpots;
	void clear() { hasSun = false; pointSpots.clear(); }
	bool hasLights() const { return !pointSpots.empty(); }
};

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
	// Fit-to-scene: when true the point grid is centered on the loaded
	// scene's world bounds and the per-axis spacing is derived as
	// extent / (count - 1), so the crowd fills the scene (e.g. Bistro)
	// instead of clustering around the origin. Spacing/baseY above are
	// the manual fallback (fit off). Refit at load; the UI can refit.
	bool fitToSceneBounds = true;
	glm::vec3 fitMin{ 0.0f }; // scene bounds at last fit (read-only in UI)
	glm::vec3 fitMax{ 0.0f };
	bool fitValid = false;    // false = bounds empty, manual placement used
	void FitToBounds(const glm::vec3& mn, const glm::vec3& mx) {
		fitMin = mn;
		fitMax = mx;
		fitValid = mx.x >= mn.x && mx.y >= mn.y && mx.z >= mn.z;
	}
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
	bool showLightPoints = false;
	bool showLightNumbers = false;
	float lightPointSize = 6.0f; // ImGui circle radius (px)
	int maxLightLabels = 256;    // cap text draws (points are all drawn)
};

// ---------------------------------------------------------------------------
// Forward+ decals demo (tiled projected splats, see smvk::ForwardPlusDecal
// + forwardplus.cs.hlsl). Splats are kept as PLACEMENT descriptors and
// re-derived into GPU decals every frame, so the sliders below retouch
// every live decal immediately. Blood look: procedural splat albedo
// (coverage in alpha) + derived normal map, roughness lerped to
// roughnessTarget for the wet-glossy read, metallic 0.
// ---------------------------------------------------------------------------
struct DecalDemo {
	bool enabled = false;
	float opacity = 1.0f;          // global splat opacity
	float normalStrength = 0.85f;  // decal normal-map strength
	float roughnessTarget = 0.14f; // wet-blood roughness (metal stays 0)
	float metalTarget = 0.0f;
	float depthFadeStart = 0.6f;   // |local.z| fade start (box-face pop guard)
	// Splat size range for spawned decals (half extents, meters).
	float sizeMin = 0.28f;
	float sizeMax = 0.55f;
	// Live placement list (re-derived per frame; capped at FP_MAX_DECALS).
	struct Splat {
		glm::vec3 center;    // box center (world)
		glm::vec3 axisZ;     // unit projection axis (decal -> surface)
		glm::vec3 upHint;    // texture V direction hint
		glm::vec3 half;      // half extents (x, y, depth)
		uint32_t seed = 0;   // in-plane yaw RNG
		float alphaScale = 1.0f; // per-splat opacity trim (smaller = fainter)
	};
	std::vector<Splat> splats;
	uint32_t dropped = 0; // spawns that fell out of the cap (UI note)
	// Crosshair-picking acceleration (smvk::gi::GiTraceScene BVH, built
	// lazily on the first crosshair spawn — Sponza builds in a few
	// seconds; see SceneRenderer::EnsureDecalPickBvh).
	int pickBvhState = 0; // 0 = not built, 1 = ready, -1 = unavailable
};

// ---------------------------------------------------------------------------
// GPU particle demo (see smvk::Particles + particles_emit/sim/draw shaders).
// The CPU only owns the emitter PARAMETER rows below: spawn scheduling
// (rate carry), simulation and drawing all run on the GPU. Rows re-upload
// every frame (ring slice like the decal splats), so these sliders retouch
// live emitters immediately; the GPU keeps its own fractional-rate carry
// in a separate buffer the uploads never touch.
// ---------------------------------------------------------------------------
struct ParticleDemo {
	bool enabled = true;
	// Global sim gravity (m/s^2; per-emitter gravityScale multiplies it).
	float gravityY = -9.81f;
	// Live emitter parameters (capped at PARTICLE_MAX_EMITTERS on upload).
	std::vector<smvk::ParticleEmitterGpu> emitters;
	int selected = 0; // UI combo index into `emitters`
	// Steady-state alive estimate for the UI readout: sum(rate * avg life).
	float SteadyStateCount() const {
		double n = 0.0;
		for (const auto& e : emitters)
			n += (double)e.life.z * (double)(e.life.x + e.life.y) * 0.5;
		return (float)n;
	}
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
	// Reflection cubemap for specular IBL (bindless TextureCube index).
	// NOTE: this 16-byte block must stay exactly 16 bytes (offsets
	// asserted below): index + sampler + intensity + debug. Growing it
	// shifts probes/DS/GI/evsm and corrupts shadow reads on the GPU.
	uint32_t cubemapIndex = UINT32_MAX; // 640 global fallback cubemap
	uint32_t cubemapSampler = 0;        // 644 clamp sampler for TextureCube
	float cubemapIntensity = 1.0f;      // 648 specular IBL scale (UI)
	uint32_t cubemapDebug = 0;          // 652 0 off, 1 cubemap-only, 2 probe weight
	// Reflection probes for local specular IBL ride the Forward+ light
	// buffer as type-2 entries (see smvk::kForwardPlusProbe) — no probe
	// storage buffer, no parallel tile lists. Specular knobs only here
	// (16 bytes, dsTileState stays at 672).
	float cubemapLodBias = 0.0f;       // 656 added to roughness LOD (UI)
	uint32_t probeTileFresh = 0;       // 660 1 = tile cull filed probe entries
	uint32_t probePad2_[2] = {};       // 664 16-align tail
	// ---- Deferred shadows (tiled/zBin pipeline, see
	// smvk::DeferredShadows + deferredshadow*.hlsli). Raw BDAs: the HLSL
	// side declares matching vk::BufferPointer fields. dsEnabled flips the
	// default_lit consumer between the deferred lists and the inline
	// forward+ shadow path (A/B). ----
	uint64_t dsTileState = 0;   // 672 per-tile {slot, count, approx, early}
	uint64_t dsLightList = 0;   // 680 final packed entries
	uint64_t dsVisibility = 0;  // 688 64B visibility slots
	uint32_t dsTileCountX = 0;  // 696 8px shadow tiles per row
	uint32_t dsEnabled = 0;     // 700 1 = shade from the deferred lists
	// ---- SG-GI baked probe volume (see smvk::gi + sggi.hlsli). Static
	// buffers created + uploaded once at scene load; giEnabled flips the
	// default_lit ambient between the probe field and the analytic sky
	// IBL (A/B). giDebug = F8 flat irradiance view. ----
	uint64_t giProbes = 0;      // 672 SgProbeGpu array (96 B each)
	uint64_t giGrid = 0;        // 680 SgGiGrid level descriptors
	uint64_t giCells = 0;       // 688 uint2 (start, count) per cell
	uint64_t giIndices = 0;     // 696 u32 probe index list
	uint32_t giEnabled = 0;     // 704 1 = probes replace sky ambient
	float giIntensity = 1.0f;   // 708 ambient scale
	uint32_t giDebug = 0;       // 712 F8 irradiance debug view
	// EVSM shadow mode (offset 716, was padding): 0 = PCF depth atlases,
	// 1 = prefiltered EVSM moments (blurred RG32F, 1-frame lag). The
	// shadow_map_index / spotShadowMapIndex fields then address the
	// blurred EVSM atlas instead of the depth atlas. Same offset/size as
	// HLSL FrameUniforms::evsmMode — see the static_asserts below.
	uint32_t evsmMode = 0;      // 716
	// ---- Forward+ decals (tiled projected splats, see
	// smvk::ForwardPlusDecal + forwardplus.cs.hlsl). decals points at
	// this frame's flight slice of the decal ring; the tile lists are the
	// ForwardPlus builder's decal buffers (same tiling as the light
	// lists; the tile-cull dispatch refreshes them every frame it runs,
	// storing 0 with no decals). decalCount > 0 only when the cull
	// dispatch runs this frame, so the shader never reads stale lists —
	// and the tile pointers stay valid whenever the cull can run. ----
	uint64_t decals = 0;            // 720 per-frame decal ring slice
	uint64_t decalTileCounts = 0;   // 728 builder decal list counts
	uint64_t decalTileIndices = 0;  // 736 builder decal list indices
	uint32_t decalCount = 0;        // 744 live decals this frame
	uint32_t decalMaxPerTile = 0;   // 748 list stride (matches allocation)
	// ---- SG 2D lightmaps (STATIC geometry, Engine/GI/lightmap.h): the
	// atlas layers ride the bindless texture slots. lmLayerKind selects
	// the layout: 0 = the classic 9 RGBA16F stream layers (half h =
	// lobe*3 + channel -> layer h/4), 1 = BC6H one-texture-per-lobe
	// (12 slots, texture l = lobe l, .rgb = the amplitude; slots past
	// the scene's lobe count stay UINT32_MAX). The chart uvs + chart ids
	// are COOKED into the pak's vertex payloads (texCoord2/chartId),
	// the per-chart atlas transforms into the scene table (unpacked in
	// the VS via the draw's chart offset), and the lobe count rides the
	// draw ring — NO chunk buffer exists at runtime. Static/dynamic
	// separation: draws carrying a lobe count sample the atlas; dynamic
	// objects sample the regular grids below. ----
	uint32_t lmLayers[12] = {};      // 752 atlas bindless slots
	uint32_t lmEnabled = 0;         // 800 1 = lightmap branch active
	uint32_t lmLayerKind = 0;        // 804 0 = RGBA16F stream, 1 = BC6H
	uint32_t lmPad_[2] = {};         // 808 8-align pad (mirrors HLSL lmPad)
	// ---- SG regular 3D probe volumes (DYNAMIC objects, Engine/GI/
	// regular_volume.h): artist OBBs + density, baked per volume, 7
	// RGBA16F 3D textures each, hardware trilinear between probes. ----
	uint64_t regObbs = 0;           // 816 SgRegularObbGpu array
	uint32_t regLayers[56] = {};    // 824 8 volumes x 7 layer slots
	uint32_t regCount = 0;          // 1048 active volumes
	uint32_t regEnabled = 0;        // 1052 1 = grid branch active
	uint32_t regPad_[4] = {};       // 1056 16-align tail (mirrors HLSL regPad)
#if defined(DS_DEBUG_VIZ)
	// ---- Deferred-shadow debug visualizers (paper 5.3/5.4, debug builds
	// only — CMake defines DS_DEBUG_VIZ for the App target and for DXC in
	// Debug trees; release compiles fields + UI + shader paths out, so
	// the release layout is the strict prefix of this one — the core +
	// SG-GI + decal fields are identical, the visualizer tail follows).
	// Tile-state counts + per-light culling/list diagnostics blended over
	// the lit image in default_lit. dsTileMasks/dsTileGroupMasks are the
	// phase-1 (CPU) and phase-2 (GPU) TileGroup light masks the
	// light-scoped modes re-query; dsDebug* select mode / light / flags /
	// heat scale. ----
	uint64_t dsTileMasks = 0;      // 1072 phase-1 CPU TileGroup masks (viz)
	uint64_t dsTileGroupMasks = 0; // 1080 phase-2 GPU TileGroup masks (viz)
	uint32_t dsDebugMode = 0;      // 1088 app::DsDebugViz (0 = off)
	uint32_t dsDebugLight = 0;     // 1092 selected light (sorted index)
	uint32_t dsDebugFlags = 0;     // 1096 bit0 = blend over lit
	uint32_t dsDebugCountCeil = 32; // 1100 count-heat ceiling (lights)
	uint32_t dsDebugPad_[4] = {};  // 1104 16-align tail (mirrors HLSL)
#endif
};
static_assert(sizeof(FrameUniforms) % 16 == 0);
// CPU/HLSL ABI: total-size alignment is not enough — each field offset must
// match common.hlsli FrameUniforms (float4-packed, env = 8 x float4). The
// SG-GI block sits between the DS core and the decal block; the lightmap +
// regular-volume block follows the decals; the DS visualizer tail is last
// so the release layout stays a strict prefix of the debug one.
// Release prefix: 2120 = 7*64 (matrices) + 16 (camPos/time) + 16 (viewport)
//   + 32 (near/far/exposure/ao) + 128 (env) + 16 (cubemap) + 1028 (reflection probes)
//   + 32 (DS) + 48 (GI) + 32 (decals) + 56 (lmLayers/lmEnabled/lmLayerKind/pads)
//   + 256 (regLayers) + 24 (regObbs/regCount/regEnabled/regPad). Debug tail: +48.
#if defined(DS_DEBUG_VIZ)
static_assert(sizeof(FrameUniforms) % 16 == 0);
#else
static_assert(sizeof(FrameUniforms) % 16 == 0);
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
static_assert(offsetof(FrameUniforms, cubemapIndex) == 640);
static_assert(offsetof(FrameUniforms, cubemapSampler) == 644);
static_assert(offsetof(FrameUniforms, cubemapIntensity) == 648);
static_assert(offsetof(FrameUniforms, cubemapDebug) == 652);
// NOTE: reflectionProbes (BDA) + reflectionProbeCount are gone: probe data
// rides ForwardPlusLight type-2 entries (see smvk::kForwardPlusProbe).
static_assert(offsetof(FrameUniforms, cubemapLodBias) == 656);        // 656 LOD bias
static_assert(offsetof(FrameUniforms, probeTileFresh) == 660);        // 660 fresh bit
static_assert(offsetof(FrameUniforms, probePad2_) == 664);            // 664 pad
static_assert(offsetof(FrameUniforms, dsTileState) == 672);           // 672
static_assert(offsetof(FrameUniforms, dsLightList) == 680);           // 680
static_assert(offsetof(FrameUniforms, dsVisibility) == 688);          // 688
static_assert(offsetof(FrameUniforms, dsTileCountX) == 696);          // 696
static_assert(offsetof(FrameUniforms, dsEnabled) == 700);             // 700
static_assert(offsetof(FrameUniforms, giProbes) == 704);              // 704
static_assert(offsetof(FrameUniforms, giGrid) == 712);                // 712
static_assert(offsetof(FrameUniforms, giCells) == 720);               // 720
static_assert(offsetof(FrameUniforms, giIndices) == 728);             // 728
static_assert(offsetof(FrameUniforms, giEnabled) == 736);             // 736 (aligned)
static_assert(offsetof(FrameUniforms, giIntensity) == 740);           // 740
static_assert(offsetof(FrameUniforms, giDebug) == 744);               // 744
static_assert(offsetof(FrameUniforms, evsmMode) == 748);              // 748
static_assert(offsetof(FrameUniforms, decals) == 752);                // 752
static_assert(offsetof(FrameUniforms, decalTileCounts) == 760);       // 760
static_assert(offsetof(FrameUniforms, decalTileIndices) == 768);      // 768
static_assert(offsetof(FrameUniforms, decalCount) == 776);            // 776
static_assert(offsetof(FrameUniforms, decalMaxPerTile) == 780);       // 780
static_assert(offsetof(FrameUniforms, lmLayers) == 784);              // 784
static_assert(offsetof(FrameUniforms, lmEnabled) == 832);             // 832
static_assert(offsetof(FrameUniforms, lmLayerKind) == 836);           // 836
static_assert(offsetof(FrameUniforms, lmPad_) == 840);                // 840
static_assert(offsetof(FrameUniforms, regObbs) == 848);               // 848
static_assert(offsetof(FrameUniforms, regLayers) == 856);             // 856
static_assert(offsetof(FrameUniforms, regCount) == 1080);             // 1080
static_assert(offsetof(FrameUniforms, regEnabled) == 1084);           // 1084
static_assert(offsetof(FrameUniforms, regPad_) == 1088);              // 1088
#if defined(DS_DEBUG_VIZ)
static_assert(offsetof(FrameUniforms, dsTileMasks) == 1104);          // 1104
static_assert(offsetof(FrameUniforms, dsTileGroupMasks) == 1112);     // 1112
static_assert(offsetof(FrameUniforms, dsDebugMode) == 1120);          // 1120
static_assert(offsetof(FrameUniforms, dsDebugLight) == 1124);         // 1124
static_assert(offsetof(FrameUniforms, dsDebugFlags) == 1128);         // 1128
static_assert(offsetof(FrameUniforms, dsDebugCountCeil) == 1132);     // 1132
#endif

// Resources
struct AppResources {
	smvk::Holder<smvk::BufferHandle> frameUniforms;
	smvk::Holder<smvk::BufferHandle> lightsBuffer;
	smvk::Holder<smvk::BufferHandle> materialsBuffer;
	smvk::Holder<smvk::BufferHandle> directionalCascadeBuffer; // GpuDirectionalLightDescription per frame
	smvk::Holder<smvk::BufferHandle> fpLightsBuffer; // ForwardPlusLight * FP_MAX_LIGHTS per frame
	smvk::LogicalBufferHandle fpLightsLogical{}; // rendergraph alias (stable: buffer never resized)
	// Decal ring (ForwardPlusDecal * FP_MAX_DECALS per flight frame):
	// rebuilt + uploaded every frame from DecalDemo splats (live sliders).
	smvk::Holder<smvk::BufferHandle> decalsBuffer;
	smvk::LogicalBufferHandle decalsLogical{}; // rendergraph alias (stable)
	smvk::Holder<smvk::BufferHandle> spotShadowBuffer; // GpuCascadeDescription * kMaxSpotShadows per frame
	smvk::LogicalBufferHandle spotShadowLogical{}; // rendergraph alias (stable: buffer never resized)
	smvk::Holder<smvk::ShaderModuleHandle> vertShader;
	smvk::Holder<smvk::ShaderModuleHandle> fragShader; // default_lit (F1)
	smvk::ShaderProgram mainProgram;
	// Alpha-to-coverage lit FS (default_lit_a2c): same VS, coverage-from-
	// alpha cutout without discard — bound for Masked materials in 2x MSAA
	// Main. Missing module leaves shaderCount 0 and Main keeps the
	// discard variant everywhere.
	smvk::Holder<smvk::ShaderModuleHandle> fragA2CShader;
	smvk::ShaderProgram mainProgramA2C;
	// Debug-view fragment programs (F2/F4/F6): same default.vert VS, flat FS.
	// Loaded alongside main; missing modules leave the program at
	// shaderCount 0 and the view falls back to Lit.
	smvk::Holder<smvk::ShaderModuleHandle> fragUnlitShader;   // default_unlit (F2)
	smvk::Holder<smvk::ShaderModuleHandle> fragCascadeShader; // default_cascade_debug (F4)
	smvk::ShaderProgram unlitProgram;
	smvk::ShaderProgram cascadeProgram;
	smvk::Holder<smvk::ShaderModuleHandle> fragVertexColorShader; // default_vertexcolor_debug (F6)
	smvk::ShaderProgram vertexColorProgram;
	// UV debug views (F9 base UV / F10 lightmap uv2): checker + gradient
	// FS pairs, channel selected per compiled module.
	smvk::Holder<smvk::ShaderModuleHandle> fragUv0DebugShader; // default_uv0_debug (F9)
	smvk::Holder<smvk::ShaderModuleHandle> fragUv1DebugShader; // default_uv1_debug (F10)
	smvk::ShaderProgram uv0DebugProgram;
	smvk::ShaderProgram uv1DebugProgram;
	// Parallax occlusion mapping view (F11): occlusion-map heightfield
	// march + sun self-shadow, sun/sky shading sampled at the displaced
	// uv (see default_pom.frag.hlsl). Missing module leaves shaderCount 0
	// and the view falls back to Lit.
	smvk::Holder<smvk::ShaderModuleHandle> fragPomShader; // default_pom (F11)
	smvk::ShaderProgram pomProgram;
	// Shader-LOD farthest-level program (World::LodShader::VertexLit):
	// its own VS computes the lighting per vertex (see
	// default_vertexlit.vert.hlsl) and the FS is a fog pass-through.
	// Missing modules leave shaderCount 0 and every level stays lit.
	smvk::Holder<smvk::ShaderModuleHandle> vertexLitVertShader;
	smvk::Holder<smvk::ShaderModuleHandle> vertexLitFragShader;
	smvk::ShaderProgram vertexLitProgram;
	// Wireframe (P) flat-line twins: default_wire.frag pairs with the same
	// VSs as the shaded legs, so wireframe line pixels skip the heavy lit
	// FS (Forward+ loop, GI, texture feedback). Missing module leaves
	// shaderCount 0 and wireframe keeps the shaded FS (raster override
	// only, the pre-swap behavior).
	smvk::Holder<smvk::ShaderModuleHandle> fragWireShader;
	smvk::ShaderProgram mainWireProgram;       // default.vert + wire FS
	smvk::ShaderProgram vertexLitWireProgram;  // default_vertexlit.vert + wire FS
	// SSR receiver-material mask writers (see ssr.h): default.vert +
	// glassmask.frag redraw of the transparent list into ssr.glass
	// (R = pane presence, G = pane reverse-Z depth) and default.vert +
	// ssrmat.frag redraw of the opaque list (B = effective metallic,
	// A = gloss). Missing modules leave shaderCount 0 and the GlassMask
	// pass skips that leg; when both are missing the pass is not emitted
	// and SSR keeps the opaque-depth receiver with the uniform-F0
	// legacy behavior (pre-mask look: reflections fire on every
	// surface, rough stone included).
	smvk::Holder<smvk::ShaderModuleHandle> fragGlassMaskShader;
	smvk::ShaderProgram glassMaskProgram;
	smvk::Holder<smvk::ShaderModuleHandle> fragSsrMatShader;
	smvk::ShaderProgram ssrMatProgram;
	// Alpha-tested shadow-depth program for MASK cutout casters
	// (shadow.vert mainVS + shadow.frag mainPS). Opaque casters keep
	// the engine builder's depth-only program. shadow.frag declares
	// conservative depth for the atlas's normal-Z convention.
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedVertShader;
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedFragShader;
	smvk::ShaderProgram shadowMaskedProgram;
	// Reversed-Z conservative twin of shadow.frag (shadow_rev.frag):
	// the Forward+ prepass / temporal seed / remainder masked phases run
	// clear-0 GreaterEqual (reversed-Z), so their bound must promise
	// "never nearer" instead of the atlas's "never farther".
	smvk::Holder<smvk::ShaderModuleHandle> shadowMaskedFragRevShader;
	// Alpha-tested depth-prepass program for MASK cutout
	// (default.vert mainVS + shadow_rev.frag mainPS): writes cutout-accurate
	// depth so background behind foliage holes survives the main pass.
	// Reuses the main VS and the reversed-Z masked-shadow FS module.
	smvk::ShaderProgram prepassMaskedProgram;
	// NOTE: shadow/bloom/tonemap/forwardplus programs + modules are owned by
	// their engine builders (CascadedShadowAtlas / BloomChain / Tonemapper /
	// ForwardPlus).
};

struct ViewportResources {
	smvk::CascadedShadowAtlas directionalShadows; // atlas + default shadow program + Ensure
	smvk::SpotShadowAtlas spotShadows; // perspective atlas for shadowed spots + Ensure
	smvk::EvsmAtlas evsmDir;  // EVSM moments + blurred output (directional CSM tiles)
	smvk::EvsmAtlas evsmSpot; // EVSM moments + blurred output (spot tiles)
	smvk::BloomChain bloom; // separated per-mip HDR chain + default down/up programs + Ensure
	smvk::Tonemapper tonemap; // default tonemap program + pass helper
	smvk::ForwardPlus forwardPlus; // tile buffers + cull/depth programs + Ensure
	smvk::DeferredShadows deferredShadows; // fixed-size ring buffers + 3 phase programs
	smvk::Particles particles; // GPU pool/free-list/emitter-ring buffers + emit/sim/draw programs
	std::vector<smvk::LogicalImageViewHandle> swapImages;
	smvk::DepthTarget renderDepth; // scene depth at internal resolution + Ensure
	// Occluder-only depth for the HZB pyramid (Option A split): filled by
	// ForwardPlusRenderer::AppendOccluderPrepass with big solid occluders
	// at their crudest LOD. HZB + the GPU filter read this; Main, SSAO,
	// tile-cull and deferred shadows keep reading the full renderDepth.
	smvk::DepthTarget hizDepth;
	smvk::HdrTarget hdr; // HDR intermediate (image + view + bindless + logical) + Ensure
	smvk::MsaaTarget msaa; // transient 2x color+depth for Main, resolves to hdr
	smvk::MotionVectors motionVectors; // RG16F motion target + program + pass helper
	smvk::MotionBlur motionBlur; // blurred HDR output + velocity-gather program + pass
	smvk::DepthOfField dof; // DOF HDR output + CoC target + low/bokeh programs + passes
	smvk::ScreenSpaceReflections ssr; // SSR HDR composite output + program + pass
	smvk::VolumetricFog volumetric; // GI-probe raymarched fog HDR output + program + pass
	smvk::ExposureMeter exposure; // histogram meter + compute program (auto-exposure)
	smvk::Ssao ssao; // half-res raw + blurred AO targets + programs + passes
	smvk::Gtao gtao; // half-res GTAO horizon-search targets + programs + passes
	smvk::UiTarget ui; // separated UI pass target (ui scale) + Ensure
	smvk::UiComposite uiComposite; // UI -> swapchain alpha composite program + pass
	smvk::Smaa smaa; // LDR target + edges/weights/history + LUTs + programs (see smaa.h)
	smvk::Dlss dlss; // HDR output (swap extent) + 1x1 exposure scalar (see dlss.h)
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
	// CLI override (main.cpp --pak / --gltf): external scene instead of
	// a registry sample. Registry camera pose is meaningless for it, so
	// the camera frames the loaded bounds after resolve (unless
	// VULKANTOY_CAM_POS/FWD pin it for benches).
	void SetSceneOverride(bool v) { m_sceneOverride = v; }
	// Which Scene blob of the pak to load (see ListPakScenes; 0 = default).
	void SetSceneIndex(int v) { m_sceneIndex = v < 0 ? 0 : v; }
	// CLI overrides (main.cpp): fixed present mode + benchmark frame budget.
	void SetPresentMode(smvk::PresentMode m) { m_presentMode = m; }
	// Bench freeze: skip every camera-control update (keyboard/gamepad/
	// mouselook) so the camera stays at the scene's configured pose for
	// the whole run and A/B comparisons are deterministic. The I-key UI
	// mode and the feature shortcuts (L/G/H/...) keep working.
	void SetDisableControl(bool v) { m_disableControl = v; }
	// Bench determinism: fixed manual exposure (no eye adaptation), so
	// captures from different runs are directly comparable. main.cpp
	// forces this off for --bench; X toggles it back live.
	void SetAutoExposureEnabled(bool v) { m_autoExposureEnabled = v; }
	// CLI override (main.cpp --no-dlss): force the DLSS mode (Off skips
	// the feature passes entirely). Pairs with
	// smvk::DlssSetStartupEnabled(false), which keeps the Streamline
	// interposer out of the Vulkan call chain for RenderDoc captures.
	void SetDlssMode(smvk::DlssMode mode) { m_dlssMode = mode; }

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
	UiDock m_uiDock = UiDock::Right; // fixed side bar; U toggles Right <-> Left
	// Configurable resolution knobs (see "Resolution config" in the .cpp).
	float m_renderScale = 1.0f;
	float m_uiScale = 1.0f;
	float m_uiDpiScale = 1.0f; // synced from ImGuiOverlay at init (auto = monitor DPI)
	// VSync / presentation mode (overlay combo; applied via swapchain Recreate).
	// Immediate default: uncapped, lowest latency (the overlay combo can pick
	// the vsync modes; FIFO is strict — a missed 16.6ms locks to 30fps).
	smvk::PresentMode m_presentMode = smvk::PresentMode::Immediate;
	// Frame timing: CPU wall time vs GPU execution time (EMA-smoothed ms
	// for the debug overlay). GPU lags 1-2 frames (retired-frame readback).
	double m_cpuMsSmooth = 0.0;
	double m_gpuMsSmooth = 0.0;
	bool m_gpuTimingValid = false;
	// Acquire wait (in-flight fence + vkAcquireNextImageKHR) plus the
	// vkQueuePresentKHR throttle (vsync-paced under FIFO): idle time,
	// reported separately so CPU stays busy-only.
	double m_waitMsSmooth = 0.0;
	// ---- GPU memory stats (VMA budgets, refreshed at most 1 Hz, UI only) ----
	std::vector<smvk::GpuMemoryHeap> m_gpuMemHeaps{};
	double m_gpuMemLastQuerySec = -1e9; // throttle: max 1 Hz
	double m_gpuMemLastLogSec = -1e9; // console: max ~1 per 10 s (when enabled)
	double m_gpuMemQueryUs = 0.0; // last query wall time (µs, shown in UI)
	bool m_gpuMemShowAllHeaps = false; // collapsed by default: VRAM heap only
	bool m_gpuMemConsoleAutoLog = false; // periodic stderr snapshot; button always works
	void RefreshGpuMemoryStats(bool force = false); // throttled query
	void LogGpuMemoryToConsole(); // fprintf(stderr) one snapshot

	void EarlyUpdate() override {
		Super::EarlyUpdate();
	}

	void Update() {
		Super::Update();
		HandleShortcuts();
		// Headless crosshair splat (VULKANTOY_DECAL_AUTOSPLAT=<frame>):
		// same path as the J key, fired once past the frame index so
		// capture/A-B runs exercise the pick-BVH + raycast glue.
		if (m_decalAutoSplatAtFrame >= 0 && (int64_t)m_frameCounter >= m_decalAutoSplatAtFrame) {
			m_decalAutoSplatAtFrame = -1;
			SpawnDecalAtCrosshair();
		}
		if (!m_uiMode && !m_disableControl) {
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
		// Gamepad Start mirrors I (UI <-> FPS) so controller-only users
		// can reach the debug panel.
		if (m_input.hasGamepad() &&
			m_input.gamepadButtonPressed(0, SDL_GAMEPAD_BUTTON_START)) {
			SetUIMode(!m_uiMode);
		}
		// U toggles the fixed debug-panel side: Right <-> Left.
		if (m_input.isKeyPressed(GLFW_KEY_U)) {
			CycleUiDock();
		}
		if (m_input.isKeyPressed(GLFW_KEY_M)) {
			m_motionBlurEnabled = !m_motionBlurEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_O)) {
			m_aoMode = static_cast<AoMode>(
				(static_cast<int>(m_aoMode) + 1) % 3); // None -> Ssao -> Gtao
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
		// J spawns a blood splat on whatever the camera crosshair hits
		// (CPU raycast over the scene triangle soup; the pick BVH builds
		// lazily on first use — see SpawnDecalAtCrosshair).
		if (m_input.isKeyPressed(GLFW_KEY_J)) {
			SpawnDecalAtCrosshair();
		}
		if (m_input.isKeyPressed(GLFW_KEY_H)) {
			m_dsEnabled = !m_dsEnabled;
		}
		if (m_input.isKeyPressed(GLFW_KEY_G)) {
			m_useGfxFilter = !m_useGfxFilter;
		}
		// Ambient / GI source: N cycles All -> Lightmaps -> Probes ->
		// SkyIbl -> Off (see AmbientMode — single-source isolation for
		// bisection, All is the production state).
		// R cycles the baked-GI debug views: 0 off, 1 flat irradiance of
		// the active source, 2 source palette (red lm, green grid, blue
		// probes; magenta = none). F8 is an alias for R. Both work in
		// every ambient mode. (R because I is taken — I toggles the
		// UI/FPS mode.)
		if (m_input.isKeyPressed(GLFW_KEY_N)) {
			CycleAmbientMode();
		}
		if (m_input.isKeyPressed(GLFW_KEY_R)
			|| m_input.isKeyPressed(GLFW_KEY_F8)) {
			m_giDebug = (m_giDebug + 1) % 3;
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
		// Debug views (F1..F6, F9..F11): F1 lit, F2 unlit, F3 meshlets, F4
		// shadow cascades, F5 LODs by color, F6 vertex-color layer
		// weights, F9 base-UV checker, F10 lightmap-uv2 checker,
		// F11 parallax occlusion mapping.
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
		if (m_input.isKeyPressed(GLFW_KEY_F6)) {
			m_debugView = DebugView::VertexColors;
		}
		// UV debug views: F9 = base UV (TEXCOORD0), F10 = lightmap
		// atlas UV (uv2). Checker + gradient + seam glow (see
		// default_uv_debug.hlsli) — lightmap chart layout and
		// cross-island streaks read at a glance.
		if (m_input.isKeyPressed(GLFW_KEY_F9)) {
			m_debugView = DebugView::Uv0;
		}
		if (m_input.isKeyPressed(GLFW_KEY_F10)) {
			m_debugView = DebugView::Uv1;
		}
		// Parallax occlusion mapping (F11): every material texture
		// sampled at a ray-marched parallax uv through the occlusion-
		// map heightfield, with sun self-shadowing (default_pom).
		if (m_input.isKeyPressed(GLFW_KEY_F11)) {
			m_debugView = DebugView::Pom;
		}
		// P toggles wireframe (PolygonMode::Line, Cull None) over the
		// active debug view — a raster override, not a shading swap, so
		// it stacks with F1..F6.
		if (m_input.isKeyPressed(GLFW_KEY_P)) {
			m_wireframe = !m_wireframe;
		}
#if defined(DS_DEBUG_VIZ)
		// Deferred-shadow visualizer cycle (Y): Off -> counts -> diff ->
		// light-scoped modes. Needs the pipeline on (H); see the panel's
		// "DS visualizers" block for the mode list. Y is the key (not F6):
		// F6 selects the vertex-color debug view, and some
		// keyboards/overlays remap or swallow function keys, never
		// letters.
		if (m_input.isKeyPressed(GLFW_KEY_Y)) {
			m_dsDebugMode = static_cast<DsDebugViz>(
				(static_cast<int>(m_dsDebugMode) + 1)
				% (static_cast<int>(DsDebugViz::LightVis) + 1));
		}
#endif
	}

	void CycleUiDock() {
		m_uiDock = (m_uiDock == UiDock::Right) ? UiDock::Left : UiDock::Right;
	}
	void SetUiDock(UiDock dock) {
		m_uiDock = (dock == UiDock::Left) ? UiDock::Left : UiDock::Right;
	}
	const char* UiDockName() const {
		return (m_uiDock == UiDock::Left) ? "Left" : "Right";
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

	// ---- Forward+ decals (tiled projected splats; see DecalDemo +
	// smvk::ForwardPlusDecal) ----
	// Single gate for every consumer (FrameUniforms decalCount/BDAs, the
	// decal leg of the forward+ tile cull, Main read hints) so the shader
	// flag and the GPU lists can never disagree. Requires the forward+
	// tile buffers: the cull dispatch refreshes the decal lists (storing
	// 0 per tile with no decals), so decals ride the same pass as lights —
	// including when the deferred-shadow pipeline owns the LIGHT lists
	// (lightCount=0 cull; decals keep working).
	bool DecalsActive() const {
		return m_decalDemo.enabled && m_passes.forwardPlusCull
			&& m_viewportResources.forwardPlus.Valid()
			&& !m_resources.decalsBuffer.empty()
			&& m_decalAlbedoBindless != UINT32_MAX;
	}
	// Re-derives the GPU decal list from DecalDemo splats and uploads the
	// flight-sliced ring; returns the live count. Runs before
	// UpdateFrameUniforms (which reads the count via m_decalFrameCount).
	uint32_t UpdateDecals(uint32_t currentFrame);
	// J: lazily builds the CPU pick BVH (scene triangle soup, see
	// smvk::gi::BuildTraceSceneFromCluster) and raycasts the camera
	// center ray; on hit spawns a blood splat oriented to the surface.
	void SpawnDecalAtCrosshair();
	// Lazy CPU pick scene (same triangle soup the GI baker sees): shared
	// by crosshair picking and the probe-overlay occlusion cull. Builds
	// once (one-time glTF re-import hitch, noted on console); true when
	// the BVH is ready for raycasts.
	bool EnsurePickScene();
	// Headless J (VULKANTOY_DECAL_AUTOSPLAT=<frame>): auto-runs the
	// crosshair spawn at the given frame so capture/A-B runs verify the
	// pick path without window focus. -1 = off.
	int m_decalAutoSplatAtFrame = -1;
	// Hero splats for the loaded scene (Sponza: floor pools + a wall smear
	// visible from the default camera; no-op elsewhere — J covers the
	// rest).
	void SpawnDecalPresets();
	// Procedural blood-splat textures: RGBA albedo (coverage in alpha,
	// sRGB) + a tangent-space normal map derived from the splat height
	// field. Bound bindless for the decal material slots.
	bool InitDecalTextures();
	void ClearDecals() {
		m_decalDemo.splats.clear();
		m_decalDemo.dropped = 0;
	}

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
	// By reference: RenderGraph::Execute publishes the async-split second
	// graphics part (G2 CB + compute wait) back through fb for
	// EndAndPresent. By value would drop it and G1's CB would be ended +
	// submitted twice (GPU hang on lights-off, when the split engages).
	void RenderFrame(smvk::Framebuffer& fb);

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

	// ---- Transparent list (AlphaMode::Blend + dithered) ---------------
	// Shared by Main's transparent leg and the SSR GlassMask redraw so
	// both draw the same geometry against the same cull view: items are
	// Renderable|Transparent|StaticVertexFmt, frustum-tested against the
	// frozen transparent cull view (conservative LOD union, matching
	// FilterMainView). sortBackToFront orders by distance-to-camera for
	// the classic AlphaBlend leg; the mask pass passes false (its Max
	// blend is order-independent).
	struct TransparentDrawItem {
		NodeId node;
		uint32_t meshId;
		uint32_t materialId;
		float dist2;
	};
	std::vector<TransparentDrawItem> CollectTransparentDraws(
		const smvk::RenderView& cullView, bool sortBackToFront);
	void ResetLodStats();
	// ---- Shader LODs (per-level shading programs) -------------------
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
	// The farthest level of a cooked chain shades with the vertex-lit
	// program instead of full PBR (same coarsest geometry — see World::
	// LodShaderAt). Toggle = pure fallback: off/A/B (UI checkbox or
	// VULKANTOY_SHADERLOD=0) keeps every level full-lit.
	bool m_lodShaderEnabled = false;
	uint64_t m_lodShaderDraws = 0; // Main draws shaded vertex-lit this frame
	// Effective decision for an instance's selected level: full-lit when
	// LODs/shader-LODs are disabled, uncooked, or the program is missing.
	smvk::LodShader LodShaderForNode(NodeId node, uint32_t worldMesh) const;
	bool ShaderLodActive() const {
		return m_lodShaderEnabled && m_resources.vertexLitProgram.shaderCount == 2;
	}

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
	bool m_sceneOverride = false;
	int m_sceneIndex = 0;
	// Camera control frozen (see SetDisableControl; --disable_control).
	bool m_disableControl = false;

	std::unique_ptr<smvk::ImGuiOverlay> m_imgui{};
	AppResources m_resources{};
	ViewportResources m_viewportResources{};
	ForwardPlusDemo m_fpDemo{};
	// Cooked KHR_lights_punctual set for the loaded scene (built once in
	// BuildCookedLightsFromWorld after Resolve; see scene_renderer.cpp).
	CookedSceneLights m_cookedLights{};
	bool m_useCookedLights = true;
	void BuildCookedLightsFromWorld();
	bool CookedLightsActive() const {
		return m_useCookedLights && m_cookedLights.hasLights();
	}
	// ---- Forward+ decals (tiled projected splats; see DecalDemo) ----
	DecalDemo m_decalDemo{};
	uint32_t m_decalFrameCount = 0; // live decal count (set by UpdateDecals,
	                                // read by UpdateFrameUniforms)
	uint32_t m_decalSpawnSeed = 1;  // splat yaw RNG stream (Pcg32)
	smvk::Holder<smvk::TextureHandle> m_decalBloodAlbedo;  // sRGB rgba splat
	smvk::Holder<smvk::TextureHandle> m_decalBloodNormal;  // tangent-space map
	uint32_t m_decalAlbedoBindless = UINT32_MAX;
	uint32_t m_decalNormalBindless = UINT32_MAX;
	// Lazy crosshair-pick acceleration (J): full scene triangle soup +
	// CPU BVH, built on first use from the resolved world clusters.
	std::unique_ptr<smvk::gi::GiTraceScene> m_decalPickScene;
	// ---- GPU particles (compute emitters, see ParticleDemo + smvk::Particles) ----
	// Single gate for every consumer (Ensure, emitter ring upload, emit/sim
	// dispatches, draw pass) so the flag and the GPU state can never
	// disagree. Requires the Forward+ light pipeline only for the TILED
	// lighting path — the draw shader falls back to a brute-force light
	// loop when the tile lists are not fresh (deferred shadows own them).
	bool m_particlesEnabled = true;
	ParticleDemo m_particleDemo{};
	bool ParticlesActive() const {
		return m_passes.particles && m_particlesEnabled
			&& m_viewportResources.particles.Valid()
			&& m_viewportResources.particles.ShadersValid();
	}
	// Re-uploads the emitter parameter ring slice (every frame; carry lives
	// GPU-side) and returns the live emitter count.
	uint32_t UpdateParticles(uint32_t currentFrame);
	// Demo default emitters (embers + drifting motes at the scene origin).
	void SeedParticleEmitters();
	smvk::CookieLibrary m_cookies{}; // baked/engine-resource gobos (see cookies.h)
	smvk::CubemapLibrary m_cubemaps{}; // reflection cubemaps for IBL
	// ---- Reflection cubemap UI + cached probe rows ----
	// Probe data rides the Forward+ light buffer as type-2 entries
	// (smvk::kForwardPlusProbe, appended post-selection in
	// UpdateForwardPlusLights): no probe storage buffer, no parallel tile
	// lists. m_cubemapProbes is the upload source (widest-first order, so
	// tile overflow keeps the widest entries); the overlay reads it too.
	bool m_cubemapEnabled = true;   // specular IBL on/off (UI)
	float m_cubemapIntensity = 1.0f;// specular IBL scale (UI, FrameUniforms)
	float m_cubemapLodBias = 0.0f;  // added to roughness LOD (UI, FrameUniforms)
	int m_cubemapDebug = 0;         // 0 off, 1 cubemap-only, 2 probe weight (UI)
	bool m_cubemapShowProbes = false; // overlay: probe positions + influence rings (UI)
	float m_cubemapOverlayMaxDist = 80.0f; // overlay distance cutoff, meters (UI)
	uint32_t m_cubemapSampler = 0;  // clamp sampler for TextureCube (created once)
	struct CubemapProbeGpu {
		glm::vec4 posInfluence{ 0.0f }; // xyz = center (world), w = influence radius
		glm::vec4 fadeCubemap{ 0.0f };  // x = fade, y = cubemap idx bits, z = mipLevels, w = pad
	};
	static_assert(sizeof(CubemapProbeGpu) == 32);
	std::vector<CubemapProbeGpu> m_cubemapProbes; // cached probe rows (<= 32 appended)
	void LoadCubemaps(smvk::OverlayVfs& vfs);
	// Probe entries appended to the FP light upload this frame (0 when
	// disabled or nothing loaded). The dispatch gate + the freshness bit
	// share this so shader and GPU lists can never disagree.
	uint32_t ProbeEntryCount() const {
		if (!m_cubemapEnabled || m_cubemapProbes.empty())
			return 0;
		return static_cast<uint32_t>(
			std::min<size_t>(m_cubemapProbes.size(), smvk::kForwardPlusMaxProbes));
	}
	// Single gate for the tile-bucketed probe path: the forward+ tile cull
	// files probe entries into the shared light tile lists, and
	// FrameUniforms.probeTileFresh publishes 1/0 so the lit shader and the
	// read hints can never disagree with the dispatch below.
	bool ProbeTileListsWanted() const {
		return ProbeEntryCount() > 0
			&& m_passes.forwardPlusCull
			&& m_viewportResources.forwardPlus.Valid();
	}
	uint32_t m_spotShadowCount = 0; // shadowed spots this frame (tiles used)
	int32_t m_spotShadowMapIndex = -1; // bindless spot atlas, -1 = none
	std::vector<smvk::GpuCascadeDescription> m_spotShadowTiles; // spot tiles this frame (feed the spot RenderViews)
	// ---- EVSM (Exponential Variance Shadow Maps, prefiltered async) ----
	// When enabled (and the device supports RG32F 2xMSAA moments), the
	// cascade/spot depth passes are replaced by EVSM moment passes
	// (RG32F 2xMSAA + resolve) followed by separable Gaussian blur passes
	// marked AsyncCompute. The blur runs on the dedicated compute queue
	// (G1/C/G2 split submit, SameFrame default — no shadow lag); the
	// OneFrameLag pipeline stays available in the UI. shadow_map_index
	// fields then address the blurred atlas; FrameUniforms.evsmMode tells
	// shaders. Falls back to PCF automatically when unsupported/invalid.
	// NOTE: default ON — EVSM is the shadow path (dual-ESM visibility with
	// symmetric warp-space bias compensation, dense prefilter, 9-tap soft
	// penumbra). Falls back to PCF automatically when unsupported/invalid.
	// Opt out with VULKANTOY_EVSM=0 or the ImGui checkbox.
	bool m_evsmEnabled = true; // user toggle (ImGui); effective only if supported
	// SameFrame default: moments are blurred on the true-async compute
	// queue and consumed by this frame's lighting (no shadow lag/wobble).
	// OneFrameLag remains in the ImGui combo for overlap comparison.
	smvk::EvsmPipelineMode m_evsmPipeline = smvk::EvsmPipelineMode::SameFrame;
	// Directional sampling descriptors for OneFrameLag: the blurred atlas
	// read by lighting trails one flight slot, so it was baked with LAST
	// frame's cascade fit — the sampling copy of
	// GpuDirectionalLightDescription must trail one frame too or the
	// receiver projection disagrees with the stale map content (EVSM-only
	// acne under camera motion). SameFrame uploads the current fit.
	smvk::GpuDirectionalLightDescription m_dirSamplePrev{};
	bool m_dirSamplePrevValid = false;
	// Soft shadows: artistic scalar over the physical penumbrae —
	// directional uses the sun disc angular radius (environment
	// sunDiscSizeDeg) projected per cascade; spots use the emitting-sphere
	// diameter (m_evsmSpotLightSize) with PCSS similar-triangles. 0 = hard.
	float m_evsmSoftness = 1.0f;
	float m_evsmSpotLightSize = 0.35f; // m, matches the demo PCSS light size
	bool EvsmActive() const {
		return m_evsmEnabled
			&& m_viewportResources.evsmDir.Valid()
			&& m_viewportResources.evsmDir.ShadersValid()
			&& m_viewportResources.evsmSpot.Valid()
			&& m_viewportResources.evsmSpot.ShadersValid();
	}
	// Direct-path EVSM moment recording. Phases split like PCF: opaque
	// casters bind the cheap no-material moments program (opaqueProgram,
	// null = fall back to the full one), MASK cutout bind the full
	// alpha-tested moments program.
	void RecordEvsmMomentsDirect(smvk::CommandBuffer& cmd,
		smvk::ShaderProgram& momentsProgram, const PushConstants& pc,
		uint32_t casterViewIndex, bool skipFiltered,
		smvk::ShaderProgram* opaqueProgram = nullptr);
	smvk::RenderViewSet m_renderViews{}; // per-frame virtual cameras: main view + shadow cascade/spot views
	// ---- GeometryFX-style per-view filter (clustered, see App/renderer.h) ----
	app::GfxRenderer m_gfxRenderer{};
	bool m_useGfxFilter = true; // false = legacy direct draws (A/B)
	// ---- Cooperative shadow draw lists (cascade + spot tiles) ----
	// Candidates are prebuilt once per frame on the render thread (LOD
	// decisions mutate the World halfrate cache — not thread-safe), then
	// each shadow view's command list is built on a worker while the main
	// thread records the rest of the frame. Futures join before Execute
	// (the tile-pass lambdas read the built ranges) and the ring uploads
	// run on the main thread right after (see UploadShadowList).
	void BuildShadowDrawCandidates();
	// Direct caster recording, both phases (opaque VS-only + masked
	// alpha-test): the full legacy loop when the filter is off, and the
	// unfiltered-caster fallback inside the indirect tile passes
	// (skipFiltered=true skips registered meshes — they are covered by
	// the per-view command list).
	void RecordShadowCastersDirect(smvk::CommandBuffer& cmd,
		smvk::ShaderProgram& opaqueProgram, const PushConstants& pc,
		uint32_t casterViewIndex, bool skipFiltered);
	std::unique_ptr<smvk::ThreadPool> m_shadowWorkers;
	std::vector<std::future<void>> m_shadowFutures;
	std::vector<smvk::gfx::DrawRequest> m_shadowCandidates;
	// ---- Option A: occluder-only HZB depth ----
	// True = the HZB pyramid builds from hizDepth (big solid occluders at
	// crudest LOD) while Main/SSAO/tile-cull stay on the full renderDepth.
	// False = legacy single-depth path (HZB over the full prepass depth).
	bool m_hizOccluderDepth = false;
	float m_hizOccluderMinPixels = 24.0f; // projected bound-diameter threshold (px)
	float m_hizOccluderMinRadius = 0.5f;  // world-space bound-radius threshold (m)
	bool m_useFilteredMotion = true; // false = motion stays fully direct
	// ---- Meshlet/cone debug views (see App/renderer.h debug programs) ----
	// m_debugView selects the Main opaque program (F1..F6); anything but Lit
	// bypasses the GPU filter with direct draws so culled data stays visible.
	DebugView m_debugView = DebugView::Lit;
	int m_debugMeshletMode = 0;   // 0 = random per meshlet, 1 = cone heat
	bool m_hideTransparent = false; // debug: skip all transparent draws (bisect blending/overdraw out)
	// Wireframe (P): raster override on every scene-triangle bind in the
	// Main pass — PolygonMode::Line + Cull None (see backfaces). Composes
	// with any debug view (lit/unlit/meshlets/LODs) and the transparent
	// leg; skydome, cone-line overlays and the depth prepass stay Fill so
	// lines keep hidden-line removal against the prepass depth.
	bool m_wireframe = false;
	// Transparency technique for AlphaMode::Blend materials (default:
	// dithered screen-door, opaque state; optional classic alpha blend).
	smvk::TransparentMode m_transparentMode = smvk::TransparentMode::Blended;
	bool m_debugCones = false;    // cone-axis line overlay (CPU batches)
	smvk::Holder<smvk::BufferHandle> m_debugLines; // pos-only cone line VB
	uint32_t m_debugLinesCap = 0; // verts allocated
	bool m_dumpIndirectAudit = false; // one-shot: dump GPU indirect cmds (stderr)
	// Opt-in EVSM atlas debug dump (VULKANTOY_EVSM_DUMP=<frame>): one-shot
	// after Execute — per-stage stats (moments/temp/blurred) + a log-encoded
	// PNG of the cascade-0 tile in exports/ (edge-band width = prefilter
	// width). Same pattern as VULKANTOY_DS_DUMP (stalls; debug only).
	int m_evsmDumpAtFrame = -1;
	void EvsmDumpAtlas(uint32_t frameIndex);
	// Opt-in SSR/volumetric stage dump (VULKANTOY_SSR_DUMP=<frame>): the
	// HDR chain inputs + every SSR/volumetric intermediate at frame N and
	// N+1, with NaN/Inf/garbage stats on stderr and tone-mapped PNGs
	// (NaN/Inf hot-pink) in exports/. Pinpoints WHICH stage turns wrong.
	int m_ssrDumpAtFrame = -1;
	void DumpSsrStages(uint32_t frameIndex);
	// Ground-truth depth-view pass (runs when VULKANTOY_SSR_DUMP is armed):
	// samples the depth through the SSR trace's exact bindless path and
	// writes it to the (post-blur dead) raw target for the dump.
	smvk::Holder<smvk::ShaderModuleHandle> m_ssrDepthViewVs;
	smvk::Holder<smvk::ShaderModuleHandle> m_ssrDepthViewPs;
	smvk::ShaderProgram m_ssrDepthViewProgram{};
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
	// ---- SG-GI baked probe volume (tools/gi_baker output, see
	// smvk::gi + Engine/GI). Loaded once at scene init from
	// content/gi/<scene>.sggi (loose content or a pak). N cycles the
	// ambient source (All -> Lightmaps -> Probes -> SkyIbl -> Off — see
	// AmbientMode); R / F8 cycle the baked-GI debug views. ----
	AmbientMode m_ambientMode = AmbientMode::All; // N cycles (production default)
	int m_giDebug = 0;             // R / F8 cycle: 0 off, 1 baked irradiance,
	                                // 2 GI source palette
	float m_giIntensity = 1.0f;   // ambient scale (FrameUniforms.giIntensity)
	bool m_giShowProbes = false;  // probe-point overlay (ImGui foreground,
	                              // like the forward+ light points)
	float m_giProbePointMaxDist = 40.0f; // overlay: draw probes within N m
	bool m_giProbeOcclusionCull = false; // overlay: hide probes occluded
	                              // from the camera (CPU pick-BVH raycast;
	                              // off = see through walls, debug intent)
	glm::mat4 m_giProbeCullView = glm::mat4(0.0f); // view matrix the cache was built for
	std::vector<char> m_giProbeCullCache; // per-probe: 0 occluded, 1 visible, 2 unknown
	bool GiProbeVisibleFromCamera(uint32_t probeIdx, const glm::vec3& camPos);
	bool m_giLoaded = false;      // volume + GPU buffers live
	smvk::gi::SgGiVolume m_giVolume; // CPU mirror (stats; grids stay built)
	smvk::Holder<smvk::BufferHandle> m_giProbesBuffer;
	smvk::Holder<smvk::BufferHandle> m_giGridBuffer;
	smvk::Holder<smvk::BufferHandle> m_giCellsBuffer;
	smvk::Holder<smvk::BufferHandle> m_giIndicesBuffer;
	uint64_t m_giProbesAddress = 0;
	uint64_t m_giGridAddress = 0;
	uint64_t m_giCellsAddress = 0;
	uint64_t m_giIndicesAddress = 0;
	// Ambient-source gating (see AmbientMode): the per-source "wanted"
	// helpers drive the FrameUniforms enable flags; the *Active() gates
	// additionally require the content to be loaded. Sky IBL rides the
	// env flag (All + SkyIbl).
	bool IblWanted() const {
		return m_ambientMode == AmbientMode::All
			|| m_ambientMode == AmbientMode::SkyIbl;
	}
	bool ProbesWanted() const {
		return m_ambientMode == AmbientMode::All
			|| m_ambientMode == AmbientMode::Probes;
	}
	bool LmWanted() const {
		return m_ambientMode == AmbientMode::All
			|| m_ambientMode == AmbientMode::Lightmaps;
	}
	bool RegWanted() const {
		return m_ambientMode == AmbientMode::All
			|| m_ambientMode == AmbientMode::Probes;
	}
	bool GiActive() const { return m_giLoaded && ProbesWanted(); }
	// GI sidecar stems resolved from the loaded clusters' registry
	// (first binding per kind wins + warn; empty = fall back to the
	// scene id, which is what registry-less paks use). Resolved once
	// per scene load, before the GI loaders run.
	std::string m_giProbeName, m_giLightmapName;
	void ResolveGiSidecars();
	void CycleAmbientMode() {
		m_ambientMode = static_cast<AmbientMode>(
			(static_cast<int>(m_ambientMode) + 1) % 5);
	}
	void LoadGiVolume(smvk::OverlayVfs& vfs);
	// (LoadCubemaps declared with the cubemap members above.)
	// ---- SG 2D lightmaps (STATIC geometry, tools/gi_baker --lightmap-out
	// output). Loaded once at scene init from the registry lightmap
	// (gi/<name>.sglm, else content/gi/<scene>.sglm):
	// uploads ONLY the 9 RGBA16F atlas textures (bindless). The chart
	// uvs + chart ids + per-chart atlas transforms + per-mesh lobe counts
	// are COOKED into the pak (texCoord2/chartId, lmChartXforms,
	// StaticMesh::lmChunk riding the draw ring) — no chunk buffer exists
	// at runtime. Static/dynamic separation: lobe-flagged draws sample
	// the atlas; dynamic objects sample the regular 3D probe grids
	// (below) or the fallback ambient. Missing file = not baked yet
	// (non-fatal).
	bool m_lmLoaded = false;
	smvk::gi::SgLightmapAtlas m_lmAtlas; // CPU mirror (chunk table + debug views)
	// Atlas texture mirrors: 12 slots — the uncompressed path fills 9
	// (kLightmapLayerCount RGBA16F stream layers, kind 0), the BC6H
	// path fills scene-lobe-count one-per-lobe textures (kind 1).
	smvk::Holder<smvk::TextureHandle> m_lmLayers[12];
	uint32_t m_lmLayerSlots[12] = {}; // bindless slots (FrameUniforms.lmLayers)
	uint32_t m_lmLayerKind = 0;       // FrameUniforms.lmLayerKind mirror
	bool LmActive() const { return m_lmLoaded && LmWanted(); }
	void LoadGiLightmaps(smvk::OverlayVfs& vfs);
	// ---- SG regular 3D probe volumes (DYNAMIC objects, tools/gi_baker
	// --regular-out output; artist OBBs + density, hardware trilinear).
	// Loaded from content/gi/<scene>.regmanifest: up to 8 volumes, each 7
	// RGBA16F 3D textures + one OBB descriptor. Inside a volume's OBB the
	// grid REPLACES the sky-IBL ambient for unflagged (dynamic) draws. ----
	bool m_regLoaded = false;
	bool m_regEnabled = false;         // UI toggle (dynamic-object GI)
	struct RegVolume {
		smvk::gi::SgRegularVolume volume; // CPU mirror (OBB; coeffs freed)
		smvk::Holder<smvk::TextureHandle> layers[7]; // RGBA16F 3D
		uint32_t layerSlots[7] = {};    // bindless slots
	};
	std::vector<RegVolume> m_regVolumes;
	smvk::Holder<smvk::BufferHandle> m_regObbsBuffer;
	uint64_t m_regObbsAddress = 0;
	bool RegActive() const { return m_regLoaded && m_regEnabled; }
	void LoadGiRegularVolumes(smvk::OverlayVfs& vfs);
	// Degrades single-source ambient modes whose source did not load
	// (called once after all three GI loaders at scene init): keeps the
	// isolation states meaningful instead of silently empty.
	void DegradeAmbientForMissingSources();
	// Procedural layered-material test scene ("layers-cube" builtin):
	// subdivided cube with vertex-painted layer weights + four Sponza
	// PBR materials. Builds directly into the cluster (no glTF/pak).
	bool BuildLayersCubeScene(smvk::Cluster& cluster);
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
	// Light count captured at phase-1 upload time (telemetry reference
	// for the async counter readback; see UpdateReadbacks).
	uint32_t m_dsReadbackLightCount = 0;
	// ---- Motion vector pass: previous-frame state + debug view ----
	glm::mat4 m_prevViewProj{ 1.0f }; // last frame's viewProj (post Y-flip)
	// This frame's viewProj WITHOUT the SMAA T2x subpixel jitter (post
	// Y-flip). 2D ImGui overlays (light markers/labels) must project with
	// this: the UI layer is not jittered or temporally resolved, so the
	// jittered matrix would make every marker buzz +/-0.25 px per frame.
	glm::mat4 m_unjitteredViewProj{ 1.0f };
	// This frame's projection WITHOUT jitter (post Y-flip): DLSS wants the
	// clip transform unjittered (cameraViewToClip) — jitter travels as the
	// separate pixel-space offset, never inside the matrices.
	glm::mat4 m_unjitteredProj{ 1.0f };
	// Last frame's m_unjitteredViewProj (same validity as m_prevViewProj):
	// DLSS clipToPrevClip must reproduce the motion-vector pass' camera
	// history without the per-frame jitter.
	glm::mat4 m_prevUnjitteredViewProj{ 1.0f };
	bool m_prevViewProjValid = false; // false until the first frame completes
	// ---- Temporal jitter bookkeeping (SMAA T2x / DLSS) ----
	// The motion vectors MUST carry the jitter (prevViewProj is jittered)
	// so SMAA T2x / DLSS reproject exactly. Non-temporal consumers (motion
	// blur, the MV debug view, and SMAA's history-attenuation metric) get
	// the known jitter component of the vectors to subtract:
	//   m_currJitterUV  - this record's sample-position offset (UV)
	//   m_prevJitterUV  - the previous record's (snapshotted with prevViewProj)
	//   m_jitterDeltaUV - (prev - curr): the jitter's contribution to the
	//                     MV texture (prevUV - currUV); zero when still
	//                     warming up (prevViewProj == viewProj) or no jitter.
	glm::vec2 m_currJitterUV{ 0.0f };
	glm::vec2 m_prevJitterUV{ 0.0f };
	glm::vec2 m_jitterDeltaUV{ 0.0f };
	std::vector<glm::mat4> m_prevGlobalTransforms; // last frame's node transforms
	bool m_showMotionDebug = false; // tonemap debug view: motion vectors
	float m_motionDebugScale = 16.0f; // debug view velocity amplification
	// ---- Motion blur pass (HDR velocity gather, see motionblur.h) ----
	bool m_motionBlurEnabled = true; // runs after Main, before Bloom/Tonemap (M)
	int m_motionBlurSamples = 12; // gather taps 1..32
	float m_motionBlurMaxRadius = 24.0f; // blur clamp (px)
	float m_motionBlurStrength = 1.0f; // shutter scale
	// ---- Depth-of-field pass (HDR, post-motion-blur, pre-bloom) ----
	bool m_dofEnabled = false; // opt-in: off keeps the reference sharp look
	bool m_dofHqBokeh = false; // false = low single-pass Poisson, true = CoC + 48-tap hex bokeh
	float m_dofFocusDistance = 8.0f; // in-focus plane (m, view distance)
	float m_dofFocusRange = 2.0f; // sharp half-depth around focus (m)
	float m_dofMaxRadius = 12.0f; // low-pass clamp (px)
	float m_dofBokehMaxRadius = 24.0f; // HQ bokeh clamp (px)
	float m_dofHighlightBoost = 1.5f; // HQ bright-tap weight multiplier
	float m_dofHighlightThreshold = 1.0f; // HQ pre-exposure luminance knee
	// ---- Screen-space reflections (HDR composite, post-DOF, pre-bloom) ----
	// Opt-in plus pass: no GBuffer, so normals/Fresnel reconstruct from the
	// 1x prepass depth PLUS the glass layer (GlassMask redraw of the
	// transparent list: pane presence + pane depth — see ssr.glass).
	// Bistro shopfront glass at street grazing angles is the calibration
	// case (default Bistro view looks down the road at it).
	bool m_ssrEnabled = false;
	// Glass layer (pane receivers) on/off — VULKANTOY_SSR_GLASS A/B knob.
	// Off = the GlassMask pass skips its pane leg, SSR reflects off the
	// opaque depth only (the pre-layer pane behavior); the opaque
	// material leg still runs, so the per-receiver metallic/gloss gate
	// stays active.
	bool m_ssrGlassEnabled = true;
	int m_ssrSteps = 32; // raymarch steps 1..128 (half-res trace)
	int m_ssrRefineSteps = 4; // binary refine iterations 0..8
	float m_ssrMaxDistance = 50.0f; // ray length (m)
	float m_ssrThickness = 0.3f; // depth acceptance band (m)
	float m_ssrIntensity = 1.0f; // reflection strength
	float m_ssrF0 = 0.04f; // dielectric base reflectance (glass)
	float m_ssrBlurSharpness = 10.0f; // bilateral depth edge weight (higher = tighter)
	// ---- SSR temporal filter (TAA of the reflection layer only) ----
	// Motion-vector-reprojected accumulation of the half-res reflection
	// layer (see SsrTaaState in ssr.h). The base HDR is never filtered.
	// Needs the motion-vector target; without it (or on invalid history)
	// the record falls back to the spatial-only path.
	bool m_ssrTaaEnabled = true; // temporal accumulation of the SSR layer
	float m_ssrTaaBlend = 0.9f; // history weight 0..0.98 (attenuated by motion)
	// Record-sequence state (same discipline as m_smaaT2x): parity picks
	// the history slot the next temporal record writes; invalidated on
	// target rebuild, SSR/motion loss and AA-mode changes (jitter shift).
	smvk::SsrTaaState m_ssrTaa;
	// True when this record emitted the temporal chain (end-step input).
	bool m_ssrFrameRanTemporal = false;
	// Requested = UI toggle only; the emission gate additionally needs
	// valid motion vectors (checked at the SSR site, like SmaaActive).
	bool SsrTaaRequested() const { return m_ssrTaaEnabled; }
	// ---- Raymarched volumetric fog lit by the SG-GI probes (see
	// volumetric.h). Single-scatter HDR composite after SSR: extinction +
	// probe-ambient in-scatter + HG sun scatter. Off by default (opt-in;
	// zero density/intensity is a copy-through). Quality preset drives
	// march steps (full-res composite; hand edits flip to Custom). ----
	bool m_volumetricEnabled = false;
	smvk::VolumetricQuality m_volumetricQuality = smvk::VolumetricQuality::Medium;
	int m_volumetricSteps = 16; // march steps 4..128
	float m_volumetricMaxDistance = 80.0f; // ray clamp (m)
	float m_volumetricDensity = 0.015f; // extinction (1/m)
	float m_volumetricIntensity = 1.0f; // scattering multiplier
	float m_volumetricAnisotropy = 0.4f; // HG g -0.9..0.9
	float m_volumetricSun = 1.0f; // sun scatter strength
	float m_volumetricGi = 1.0f; // probe ambient strength
	bool m_volumetricUseGi = true; // sample SG-GI probes (else sky-IBL fallback)
	bool m_volumetricUseSun = true; // analytic sun single-scatter
	float m_volumetricJitter = 1.0f; // IGN dither 0..1 (banding guard)
	float m_volumetricHeightFalloff = 0.0f; // extra height falloff (1/m, 0=off)
	float m_volumetricHeightBase = 0.0f; // reference height (m)
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
	// Opt-in alpha-to-coverage for Masked cutout in MSAA 2x Main
	// (VULKANTOY_A2C=1, default off). Bench data (bistro foliage view,
	// RTX 5070 Ti): A2C moves the masked coverage/depth test after the FS
	// (coverage needs the output alpha), so overlapping foliage layers
	// can no longer early-reject — +6ms vs the demote-discard path. Keep
	// it as a quality mode: foliage silhouettes resolve AA'd in MSAA.
	bool m_msaaAlphaToCoverage = false;
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

	// ---- DLSS Super Resolution (Streamline, see Engine/Rendergraph/dlss.h) ----
	// Off by default; when active it OWNS the internal resolution (DLSS
	// optimal render size) and the upscale (HDR render-res -> HDR swap-res
	// before tonemap), and it bypasses the AA modes (its own AA replaces
	// them; MSAA would fight the reduced-resolution input).
	smvk::DlssMode m_dlssMode = smvk::DlssMode::Off;
	// One-time capability check (slIsFeatureSupported on this adapter).
	bool m_dlssSupported = false;
	// This record's snapshot: filled after the target Ensure block so the
	// jitter (UpdateFrameUniforms), the MSAA gate and the tonemap retarget
	// all agree with the pass-emission gate below.
	bool m_dlssFrameActive = false;
	// History reset request: set on deactivation/mode/extent change, held
	// until the next DLSS record consumes it (Constants::reset).
	bool m_dlssReset = true;
	// Halton jitter phase (record cadence like the T2x parity, not
	// per-image) + this record's snapshot of the jitter in render pixels.
	uint32_t m_dlssPhase = 0;
	uint32_t m_dlssPhaseCount = 8;
	smvk::DlssJitter m_dlssJitter{};
	// slDLSSSetOptions drift tracking (re-applied on mode/output change;
	// slFreeResources on deactivate — see RenderFrame's DLSS block).
	bool m_dlssOptionsApplied = false;
	smvk::DlssMode m_dlssOptionsMode = smvk::DlssMode::Off;
	uint32_t m_dlssOptionsW = 0;
	uint32_t m_dlssOptionsH = 0;
	// Reset bookkeeping: extents/mode the last DLSS record saw.
	smvk::DlssMode m_dlssLastMode = smvk::DlssMode::Off;
	smvk::Extent2D m_dlssLastRenderExtent{ 0, 0 };
	smvk::Extent2D m_dlssLastSwapExtent{ 0, 0 };

	bool DlssRequested() const { return m_dlssMode != smvk::DlssMode::Off; }
	// Hard requirements: Streamline alive + adapter support + motion
	// vectors (camera-motion input) + valid upscaler targets.
	bool DlssActive() const {
		return DlssRequested() && smvk::DlssAvailable() && m_dlssSupported
			&& m_passes.motionVectors
			&& m_viewportResources.motionVectors.Valid()
			&& m_viewportResources.dlss.Valid();
	}

	bool SmaaRequested() const {
		return m_passes.smaa && !m_dlssFrameActive
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
	// Fixed reference exposure for the debug views (F2..F6). Debug colors
	// are authored in display space (the flat palette pre-scales below and
	// the unlit/cascade/vertex-color FS divide by the same value), and
	// metering is frozen for them anyway (ExposureMeteringActive holds
	// the last scene exposure). Rendering at a FIXED exposure instead of
	// the held one keeps debug frames deterministic and the authored
	// colors independent of the scene's adaptation state. Mirrors
	// TONEMAP_EXPOSURE (scene_renderer.cpp) — keep in sync.
	static constexpr float kDebugViewExposure = 0.001f;
	// Exposure actually applied this frame: the adapted/manual value in
	// the Lit view, the fixed debug reference otherwise. Shading
	// (FrameUniforms.exposure), the flat debug gains AND the tonemap pass
	// all read this, so the 1/exposure pre-scale cancels end-to-end.
	float FrameExposure() const {
		return (m_debugView == DebugView::Lit)
			? EffectiveExposure() : kDebugViewExposure;
	}
	// True when a debug visualization REPLACES the scene pixels this
	// record: any non-Lit DebugView (F2..F11 flat programs), the GI
	// baked debug views (R/F8) or a DS visualizer (Y, DS_DEBUG_VIZ).
	// Scene post-fx that composites ON TOP of those pixels — SSR
	// reflections (additive), volumetric fog (Beer-Lambert throughput +
	// scatter) and the SSR-only GlassMask layer — stays off while true,
	// same discipline as the metering freeze in ExposureMeteringActive:
	// compositing over a flat readout pollutes it (reflections of the
	// palette itself, fog-faded heat colors) the way metering it would
	// poison the exposure.
	bool DebugVisualizationActive() const;
	void UpdateAutoExposure(uint32_t frameIndex); // download slice BEFORE zero + Adapt (call at record time)
	// ---- Bloom chain (HDR down/up, see bloom.h) ----
	bool m_bloomEnabled = true; // runs before Tonemap; gated with m_passes.bloom (B)
	// ---- AO (half-res depth-only, runs before Main, composites in opaque
	// ambient; see ssao.h / gtao.h). O cycles No AO -> SSAO -> GTAO. ----
	enum class AoMode : int { None = 0, Ssao = 1, Gtao = 2 };
	AoMode m_aoMode = AoMode::Ssao;
	float m_ssaoRadius = 0.5f; // view-space sample radius (m, shared SSAO/GTAO)
	float m_ssaoBias = 0.025f; // self-occlusion / horizon bias
	float m_ssaoIntensity = 1.0f; // ambient composite strength (default_lit.frag)
	float m_ssaoMaxDistance = 25.0f; // fade-out distance (m)
	int m_ssaoSamples = 16; // SSAO hemisphere taps 1..32
	float m_ssaoBlurSharpness = 8.0f; // blur depth edge weight
	int m_gtaoSlices = 3; // GTAO slice directions 1..8
	int m_gtaoSteps = 4; // GTAO taps per slice side 1..8
	float m_gtaoThickness = 1.0f; // GTAO thin-occluder falloff (m); 0 = off
	// Compat: SSAO on/off maps onto AoMode (None vs Ssao); GTAO counts as on.
	[[nodiscard]] bool SsaoEnabled() const { return m_aoMode != AoMode::None; }
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
	// Virtual cursor for FPS mouselook: SDL relative mode warps the real
	// cursor, so absolute positions are garbage while captured. The look
	// deltas (xrel/yrel, valid in BOTH relative and normal mode) fold
	// into this unbounded NDC-space position that feeds the positioner.
	glm::vec2 m_virtualMouse{ 0.0f };
	// ---- Texture streaming (mip-tail residency, see
	// Engine/Renderer/texture_streamer.h). Owns residency for every
	// glTF-loaded texture; pak-loaded textures stay full-resident until
	// the pak backend learns to hand over payloads (follow-up).
	bool m_textureStreamingEnabled = true;
	std::unique_ptr<smvk::TextureStreamer> m_textureStreamer;
	// GPU feedback consumer (Skull & Bones style readback, see
	// Engine/Renderer/texture_feedback.h). Lives alongside the streamer;
	// null together with it when streaming is disabled.
	std::unique_ptr<smvk::TextureFeedback> m_textureFeedback;
	// ---- Async readback ticket (exposure histogram + texture feedback +
	// deferred-shadow counters) ----
	// Submit + non-blocking poll each tick: the render thread NEVER waits
	// on the graphics-queue backlog (the old sync download locked for the
	// whole in-flight frame). One ticket in flight; while pending, new
	// ticks skip their submit (results land 1-2 frames later instead of
	// the old 2-3 — adaptation is time-damped, streaming is laggy anyway).
	uint64_t m_asyncReadbackTicket = 0;
	uint32_t m_pendingReadbackSlot = 0;
	bool m_pendingReadbackHasExposure = false;
	bool m_pendingReadbackHasFeedback = false;
	bool m_pendingReadbackHasDs = false;
	std::array<uint32_t, smvk::kExposureBinCount> m_pendingExposureBins{};
	std::vector<uint32_t> m_pendingFeedback;
	std::array<uint32_t, smvk::kDsCounterCount> m_pendingDsCounters{};
	// CPU demand + budget tick. streamFrame must be monotonic (NOT the
	// swapchain slot); call once per RenderFrame after UpdateMaterials.
	void UpdateTextureStreaming(const FrameUniforms& frameData, smvk::Extent2D renderExtent);
	// True when this frame may meter exposure (auto + pass + lit view).
	// Shared by the histogram download and the batched feedback pairing.
	bool ExposureMeteringActive() const;
	// Bins -> percentile target + damped adapt (tail of UpdateAutoExposure).
	void AdaptExposureFromBins(const std::array<uint32_t, smvk::kExposureBinCount>& bins);
	// Combined GPU->CPU readback tick (replaces the lone UpdateAutoExposure
	// call): on GPU-feedback frames the histogram + feedback + DS-counter
	// downloads share ONE submit + wait; otherwise the existing single
	// download runs. Always clears the feedback slice afterwards
	// (download-before-clear).
	void UpdateReadbacks(uint32_t frameSlot, uint32_t streamFrame);
};

} // namespace app
