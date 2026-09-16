#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <Engine/core.h>
#include <Engine/Asset/pak.h>
#include <Engine/Rendergraph/bloom.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <Engine/Rendergraph/environment.h>
#include <Engine/Rendergraph/evsm.h>
#include <Engine/Rendergraph/exposure.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/Rendergraph/spotshadow.h>
#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/msaa.h>
#include <Engine/Rendergraph/motionblur.h>
#include <Engine/Rendergraph/motionvector.h>
#include <Engine/Rendergraph/ssao.h>
#include <Engine/Rendergraph/tonemap.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/ui.h>
#include <Engine/Renderer/cookies.h>
#include <App/renderer.h>
#include <App/forwardplus_renderer.h>
#include "lut_io.h"
#include <App/frame_context.h>
#include <App/push_constants.h>
using app::PushConstants;
#include <Engine/Vfs/vfs.h>
#include <Engine/Utils/texture_loader.h>
#include <App/scene_renderer.h>
#include <Engine/Profile/profile.h>
#include <imgui.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


// Constants (clips/FOV live on SceneRenderer as members; far clip is
// per-scene, see SceneDesc::farClip). Scene selection (gltf/pak paths) is
// also per-scene; see SceneRegistry below.

// Reads a content-relative VFS path ("shaders/...", "models/...") through
// the app overlay (loose content/ dir + mounted paks). Throws on miss.
// Bench phase accumulators (RenderFrame sub-timings, --bench only).
static double s_benchUpdates = 0.0, s_benchPasses = 0.0, s_benchReadbacks = 0.0,
	s_benchUi = 0.0, s_benchExecute = 0.0, s_benchUpdA = 0.0, s_benchUpdB = 0.0,
	s_benchStream = 0.0, s_benchFp = 0.0, s_benchFpGen = 0.0, s_benchFpUp = 0.0,
	s_benchFpDs = 0.0, s_benchPreMain = 0.0, s_benchPostMain = 0.0,
	s_benchPreA = 0.0, s_benchPreB = 0.0, s_benchPreC = 0.0;

static std::vector<char> readContentFile(smvk::OverlayVfs& vfs, const std::string& vfsPath) {
	std::vector<uint8_t> bytes = vfs.ReadBinary(vfsPath);
	return std::vector<char>(bytes.begin(), bytes.end());
}

// -----------------------------------------------------------------------------
// Directional cascade shadow-map config (resources + math live in
// Engine/Rendergraph/cascadedshadowmap.h)
// -----------------------------------------------------------------------------
static constexpr uint32_t NUM_CASCADES = 4;
static constexpr uint32_t CASCADE_RES = 2048; // per cascade tile (2048 -> 4096 atlas for 4 cascades)
static constexpr float CASCADE_LAMBDA = 0.8f; // 0 = linear, 1 = pure logarithmic (1.0 starves the far cascade)
static constexpr float CASCADE_Z_PADDING = 10.0f;
static constexpr smvk::Format SHADOW_FORMAT = smvk::Format::D32_Float; // no stencil on the directional atlas

// -----------------------------------------------------------------------------
// Bloom config (resources + passes live in Engine/Rendergraph/bloom.h,
// mip images via MipBuilder in Engine/Rendergraph/mipchain.h)
// -----------------------------------------------------------------------------
static constexpr uint32_t BLOOM_MIP_COUNT = 5;
static constexpr uint32_t BLOOM_BASE_DIVISOR = 2; // base = hdr / 2 (half-res)
static constexpr float BLOOM_THRESHOLD = 1000.0f; // pre-exposure HDR; sun-lit diffuse spans ~0-1600
static constexpr float BLOOM_KNEE = 250.0f;       // keeps soft ratio knee/threshold = 0.25
static constexpr float BLOOM_INTENSITY = 0.6f;
static constexpr float BLOOM_UPSAMPLE_RADIUS = 1.0f;

static smvk::BloomDesc MakeBloomDesc() {
	smvk::BloomDesc d{};
	d.mipCount = BLOOM_MIP_COUNT;
	d.format = smvk::Format::RGBA16_Float;
	d.baseDivisor = BLOOM_BASE_DIVISOR;
	d.threshold = BLOOM_THRESHOLD;
	d.knee = BLOOM_KNEE;
	d.intensity = BLOOM_INTENSITY;
	d.upsampleRadius = BLOOM_UPSAMPLE_RADIUS;
	d.name = "bloom";
	return d;
}

// -----------------------------------------------------------------------------
// Tonemap config (program + pass live in Engine/Rendergraph/tonemap.h)
// -----------------------------------------------------------------------------
static constexpr float TONEMAP_EXPOSURE = 0.001f; // sun 5000 lux * 0.001 â‰ˆ HDR 5 pre-ACES
static constexpr uint32_t TONEMAP_MODE = 1; // 0=Reinhard, 1=ACES, 2=Uncharted2

static smvk::TonemapDesc MakeTonemapDesc() {
	smvk::TonemapDesc d{};
	d.exposure = TONEMAP_EXPOSURE;
	d.tonemapMode = TONEMAP_MODE;
	d.name = "Tonemap";
	return d;
}

// -----------------------------------------------------------------------------
// Auto-exposure config (histogram meter + CPU eye adaptation, see
// Engine/Rendergraph/exposure.h). The HDR buffer is pre-exposure, so the
// metered percentile luminance is exposure-independent; the smoothed
// exposure converges near TONEMAP_EXPOSURE for the default sunlit view.
// -----------------------------------------------------------------------------
static smvk::ExposureDesc MakeExposureDesc() {
	smvk::ExposureDesc d{};
	d.bins = smvk::kExposureBinCount;
	d.minLogLum = -10.0f; // ~0.001 pre-exposure (deep shadow)
	d.maxLogLum = 12.0f;  // ~4096 pre-exposure (sun disc / fireflies)
	d.sampleStep = 4;     // every 4th pixel per axis (~6% of texels)
	d.keyValue = 0.18f;   // middle-grey target
	d.percentile = 0.6f;  // robust to sky/sun outliers
	d.name = "exposure";
	return d;
}

// -----------------------------------------------------------------------------
// Motion blur config (velocity gather in HDR, see
// Engine/Rendergraph/motionblur.h). Runs after Main, before Bloom/Tonemap;
// bloom + tonemap sample the blurred HDR when the pass runs.
// -----------------------------------------------------------------------------
static constexpr uint32_t MOTIONBLUR_SAMPLES = 12;
static constexpr float MOTIONBLUR_MAX_RADIUS_PX = 24.0f;
static constexpr float MOTIONBLUR_VELOCITY_SCALE = 1.0f;

static smvk::MotionBlurDesc MakeMotionBlurDesc() {
	smvk::MotionBlurDesc d{};
	d.format = smvk::Format::RGBA16_Float; // must match the HDR target
	d.sampleCount = MOTIONBLUR_SAMPLES;
	d.maxRadiusPx = MOTIONBLUR_MAX_RADIUS_PX;
	d.velocityScale = MOTIONBLUR_VELOCITY_SCALE;
	d.name = "motionBlur";
	return d;
}

// -----------------------------------------------------------------------------
// Depth-of-field config (HDR post-process, see Engine/Rendergraph/dof.h).
// Runs after motion blur, before exposure/bloom/tonemap; downstream passes
// sample the DOF output when the pass runs, the sharp HDR otherwise.
// -----------------------------------------------------------------------------
static constexpr float DOF_FOCUS_DISTANCE = 8.0f; // in-focus plane (m)
static constexpr float DOF_FOCUS_RANGE = 2.0f; // sharp half-depth (m)
static constexpr float DOF_MAX_RADIUS_PX = 12.0f; // low-pass clamp (px)
static constexpr float DOF_BOKEH_MAX_RADIUS_PX = 24.0f; // HQ bokeh clamp (px)
static constexpr float DOF_HIGHLIGHT_BOOST = 1.5f;
static constexpr float DOF_HIGHLIGHT_THRESHOLD = 1.0f; // pre-exposure luminance knee

static smvk::DofDesc MakeDofDesc() {
	smvk::DofDesc d{};
	d.format = smvk::Format::RGBA16_Float; // must match the HDR target
	d.cocFormat = smvk::Format::R8_UNorm;
	d.hqBokeh = false;
	d.focusDistance = DOF_FOCUS_DISTANCE;
	d.focusRange = DOF_FOCUS_RANGE;
	d.maxRadiusPx = DOF_MAX_RADIUS_PX;
	d.bokehMaxRadiusPx = DOF_BOKEH_MAX_RADIUS_PX;
	d.highlightBoost = DOF_HIGHLIGHT_BOOST;
	d.highlightThreshold = DOF_HIGHLIGHT_THRESHOLD;
	d.name = "dof";
	return d;
}

// -----------------------------------------------------------------------------
// Screen-space reflections config (HDR composite, see
// Engine/Rendergraph/ssr.h). Runs after DOF, before exposure/bloom/tonemap
// so reflections meter/bloom/tonemap with the scene. Bistro shopfront glass
// is the calibration case (default Bistro street view at grazing angles).
// -----------------------------------------------------------------------------
static constexpr uint32_t SSR_STEPS = 32;
static constexpr uint32_t SSR_REFINE_STEPS = 4;
static constexpr float SSR_MAX_DISTANCE = 50.0f;
static constexpr float SSR_THICKNESS = 0.3f;
static constexpr float SSR_INTENSITY = 1.0f;
static constexpr float SSR_F0 = 0.04f; // dielectric glass
static constexpr float SSR_BLUR_SHARPNESS = 10.0f; // bilateral depth edge weight

static smvk::SsrDesc MakeSsrDesc() {
	smvk::SsrDesc d{};
	d.format = smvk::Format::RGBA16_Float; // must match the HDR target
	d.steps = SSR_STEPS;
	d.refineSteps = SSR_REFINE_STEPS;
	d.maxDistance = SSR_MAX_DISTANCE;
	d.thickness = SSR_THICKNESS;
	d.intensity = SSR_INTENSITY;
	d.f0 = SSR_F0;
	d.blurSharpness = SSR_BLUR_SHARPNESS;
	d.name = "ssr";
	return d;
}

// -----------------------------------------------------------------------------
// Raymarched volumetric fog config (GI-probe lit, see
// Engine/Rendergraph/volumetric.h). Runs after SSR, before
// exposure/bloom/tonemap so fog meters/blooms/tonemaps with the scene.
// Medium default (16 steps, full-res); quality scales with steps.
// -----------------------------------------------------------------------------
static smvk::VolumetricDesc MakeVolumetricDesc() {
	smvk::VolumetricDesc d{};
	d.format = smvk::Format::RGBA16_Float; // must match the HDR target
	smvk::ApplyVolumetricQuality(d, smvk::VolumetricQuality::Medium);
	d.maxDistance = 80.0f;
	d.density = 0.015f;
	d.intensity = 1.0f;
	d.anisotropy = 0.4f;
	d.sunScale = 1.0f;
	d.giScale = 1.0f;
	d.useGi = 1;
	d.useSun = 1;
	d.jitterStrength = 1.0f;
	d.heightFalloff = 0.0f;
	d.heightBase = 0.0f;
	d.name = "volumetric";
	return d;
}

// -----------------------------------------------------------------------------
// SSAO config (half-res depth-only AO, see Engine/Rendergraph/ssao.h).
// Runs before Main; composites in the opaque ambient (default_lit.frag.hlsl
// upsamples the half-res AO with full-res depth authority, see ao.hlsli).
// -----------------------------------------------------------------------------
static constexpr float SSAO_RADIUS = 0.5f; // view-space sample radius (m)
static constexpr float SSAO_BIAS = 0.025f; // self-occlusion bias
static constexpr float SSAO_INTENSITY = 1.0f; // ambient composite strength
static constexpr float SSAO_MAX_DISTANCE = 25.0f; // fade-out distance (m)
static constexpr uint32_t SSAO_SAMPLES = 16; // hemisphere taps 1..32
static constexpr float SSAO_BLUR_SHARPNESS = 8.0f; // depth edge weight

static smvk::SsaoDesc MakeSsaoDesc() {
	smvk::SsaoDesc d{};
	d.format = smvk::Format::R8_UNorm;
	d.radius = SSAO_RADIUS;
	d.bias = SSAO_BIAS;
	d.intensity = SSAO_INTENSITY;
	d.maxDistance = SSAO_MAX_DISTANCE;
	d.sampleCount = SSAO_SAMPLES;
	d.blurSharpness = SSAO_BLUR_SHARPNESS;
	d.name = "ssao";
	d.rawName = "ssaoRaw";
	d.blurName = "ssaoBlur";
	return d;
}

// -----------------------------------------------------------------------------
// GTAO config (half-res horizon-search AO, see Engine/Rendergraph/gtao.h).
// Same consumer path as SSAO (blur + depth-guided compose + opaque ambient),
// only the raw pass differs. Shares radius/bias/intensity/maxDistance/blur
// with SSAO; slices/steps/thickness are GTAO-specific.
// -----------------------------------------------------------------------------
static constexpr uint32_t GTAO_SLICES = 3; // slice directions 1..8
static constexpr uint32_t GTAO_STEPS = 4; // taps per slice side 1..8
static constexpr float GTAO_THICKNESS = 1.0f; // thin-occluder falloff (m)

static smvk::GtaoDesc MakeGtaoDesc() {
	smvk::GtaoDesc d{};
	d.format = smvk::Format::R8_UNorm;
	d.radius = SSAO_RADIUS;
	d.bias = SSAO_BIAS;
	d.thickness = GTAO_THICKNESS;
	d.intensity = SSAO_INTENSITY;
	d.maxDistance = SSAO_MAX_DISTANCE;
	d.sliceCount = GTAO_SLICES;
	d.stepsPerSlice = GTAO_STEPS;
	d.blurSharpness = SSAO_BLUR_SHARPNESS;
	d.name = "gtao";
	d.rawName = "gtaoRaw";
	d.blurName = "gtaoBlur";
	d.composeName = "gtaoComposed";
	return d;
}

// -----------------------------------------------------------------------------
// Resolution config.
//
// Internal resolution (render scale): the scene (HDR/MSAA/bloom/Forward+
// depth/prepass/main) renders at swapExtent * renderScale; the tonemapper
// already samples the HDR target by UV, so it upscales/downscales to the
// swapchain for free. > 1 = supersampling, < 1 = GPU savings.
//
// Separated UI pass (ui scale): ImGui renders into its own offscreen target
// at swapExtent * uiScale (cleared transparent), and UiComposite alpha-blends
// it over the tonemapped swapchain. io.DisplayFramebufferScale maps ImGui's
// screen-space draw data onto the UI target, so UI layout stays constant
// while its resolution is configurable.
//
// UI DPI scale: ImGui font raster density + style layout scaling
// (see ImGuiOverlay::ApplyDpiScale).
// -----------------------------------------------------------------------------
static constexpr float RENDER_SCALE_MIN = 0.25f;
static constexpr float RENDER_SCALE_MAX = 2.0f;
static constexpr float UI_SCALE_MIN = 0.5f;
static constexpr float UI_SCALE_MAX = 2.0f;
static constexpr float UI_DPI_MIN = 0.5f;
static constexpr float UI_DPI_MAX = 3.0f;
static constexpr float UI_FONT_BASE_SIZE = 16.0f;
static const char* UI_FONT_PATH = "content/fonts/Roboto-Medium.ttf";

static smvk::Extent2D ScaledExtent(smvk::Extent2D base, float scale) {
	scale = std::clamp(scale, 0.1f, 4.0f);
	return {
		std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(base.width) * scale))),
		std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(base.height) * scale))),
	};
}

// Frustum guard band (px, internal resolution) for the main camera views.
// The SMAA T2x projection jitter swings the frustum by +/-0.25 px every
// frame; exact plane tests would drop/re-add boundary clusters on
// alternating frames (visible popping). A 1 px conservative dilation keeps
// the boundary band on both phases - it can only reduce culling, never
// punch holes. Shadow views stay exact (their projections never jitter).
static constexpr float kMainViewCullGuardPx = 1.0f;

static smvk::CascadedShadowMapDesc MakeShadowDesc() {
	smvk::CascadedShadowMapDesc d{};
	d.cascadeCount = NUM_CASCADES;
	d.cascadeResolution = CASCADE_RES;
	d.format = SHADOW_FORMAT;
	d.lambda = CASCADE_LAMBDA;
	d.zPadding = CASCADE_Z_PADDING;
	d.name = "shadowMapAtlas";
	return d;
}

// -----------------------------------------------------------------------------
// Forward+ tiled config (resources + passes live in
// Engine/Rendergraph/forwardplus.h)
// -----------------------------------------------------------------------------
static constexpr uint32_t FP_TILE_SIZE = 16;
static constexpr uint32_t FP_MAX_LIGHTS = 1024;
// Hard per-tile list cap. Overflow lights pop (accepted): the shading pass
// walks only the first FP_MAX_PER_TILE list entries per tile. Spots are
// written to the light buffer before the point grid so the few expensive
// cone lights never lose their slots to the crowd.
static constexpr uint32_t FP_MAX_PER_TILE = 8;
// Decals share the tiling (parallel per-tile lists, see ForwardPlusDecal):
// global decal cap (ring stride) + per-tile list cap. Must stay in sync
// with the shader defines MAX_FP_DECALS / MAX_FP_DECALS_PER_TILE.
static constexpr uint32_t FP_MAX_DECALS = 256;
static constexpr uint32_t FP_MAX_DECALS_PER_TILE = 16;

static smvk::ForwardPlusDesc MakeForwardPlusDesc() {
	smvk::ForwardPlusDesc d{};
	d.tileSize = FP_TILE_SIZE;
	d.maxLights = FP_MAX_LIGHTS;
	d.maxLightsPerTile = FP_MAX_PER_TILE;
	d.maxDecals = FP_MAX_DECALS;
	d.maxDecalsPerTile = FP_MAX_DECALS_PER_TILE;
	d.name = "forwardplus";
	return d;
}

// -----------------------------------------------------------------------------
// Spot-shadow atlas config (tile descriptors live in spotshadow.h).
// The atlas is fixed at kMaxSpotShadows slots; the per-frame selection picks
// at most m_fpDemo.spotShadowMax casters (upper limit + distance/screenspace
// heuristic, see SelectSpotShadowCasters).
// -----------------------------------------------------------------------------
static constexpr uint32_t SPOT_SHADOW_RES = 1024;
static constexpr float SPOT_SHADOW_NEAR = 0.5f;
static constexpr float SPOT_SHADOW_BIAS = 0.0005f; // NDC base for slope-scaled ODB (Sec. 4.2)

static smvk::SpotShadowMapDesc MakeSpotShadowDesc() {
	smvk::SpotShadowMapDesc d{};
	d.slotCount = smvk::kMaxSpotShadows;
	d.resolution = SPOT_SHADOW_RES;
	d.format = smvk::Format::D32_Float;
	d.nearPlane = SPOT_SHADOW_NEAR;
	d.bias = SPOT_SHADOW_BIAS;
	d.name = "spotShadowAtlas";
	return d;
}

// -----------------------------------------------------------------------------
// EVSM moment atlases (RG32F 2xMSAA + async separable blur, 1-frame lag).
// Tile counts/resolutions mirror the PCF atlases so the same lightVP math
// (BuildDirectionalLight / BuildSpotTile) drives both representations.
// -----------------------------------------------------------------------------
static smvk::EvsmDesc MakeEvsmDirDesc(smvk::EvsmPipelineMode pipeline) {
	smvk::EvsmDesc d{};
	d.tileResolution = CASCADE_RES;
	d.tileCount = NUM_CASCADES;
	d.momentsFormat = smvk::Format::RG32_Float;
	d.depthFormat = smvk::Format::D32_Float;
	d.msaaSamples = 2;
	d.exponent = smvk::kEvsmExponent;
	d.blurRadius = smvk::kEvsmBlurRadius;
	d.pipeline = pipeline;
	d.name = "evsmDirAtlas";
	return d;
}

static smvk::EvsmDesc MakeEvsmSpotDesc(smvk::EvsmPipelineMode pipeline) {
	smvk::EvsmDesc d{};
	d.tileResolution = SPOT_SHADOW_RES;
	d.tileCount = smvk::kMaxSpotShadows;
	d.momentsFormat = smvk::Format::RG32_Float;
	d.depthFormat = smvk::Format::D32_Float;
	d.msaaSamples = 2;
	d.exponent = smvk::kEvsmExponent;
	d.blurRadius = smvk::kEvsmBlurRadius;
	d.pipeline = pipeline;
	d.name = "evsmSpotAtlas";
	return d;
}

// Scene samples (one row per sample; the framegraph is shared, so adding a
// sample never copies pass code). Cook commands:
//   Sponza: PakCooker --content-dir F:/content
//             --in-vfs models/Sponza/sponza.gltf
//             --out-pak F:/content/paks/sponza.pak --meshlets on
//   Bistro: py tools/cook_bistro.py --niagara-dir F:/REPOS/niagara_bistro
//             (stages models/Bistro + cooks content/paks/bistro.pak with
//             --meshlets on --lods on; transitions use each LOD's cooked
//             simplifier error projected to pixels, see LodMeshForNode).
//   Scanlands: py tools/cook_scanlands.py [--glb F:/content/models/scanlands.glb]
//             (extracts the embedded GLB to models/Scanlands + cooks
//             content/paks/scanlands.pak with --meshlets on --lods on;
//             no GI bake â€” sky-IBL ambient fallback, see LoadGiVolume).
namespace app {

const SceneDesc& SceneRegistry::Sponza() {
	static const SceneDesc sponza{
		/*id=*/"sponza",
		/*label=*/"Sponza",
		/*gltfPath=*/"content/models/Sponza/sponza.gltf",
		/*pakPath=*/"content/paks/sponza.pak",
		// Default view rotated 90 degrees (yaw +90 from the old +Z look):
		// looks down the long +X corridor arm so the decal check splats
		// (SpawnDecalPresets) are in frame at spawn.
		/*camPos=*/glm::vec3(0.0f, 1.0f, 0.0f),
		/*camTarget=*/glm::vec3(1.0f, 1.0f, 0.0f),
		/*farClip=*/50.0f,
	};
	return sponza;
}

const SceneDesc& SceneRegistry::Bistro() {
	static const SceneDesc bistro{
		/*id=*/"bistro",
		/*label=*/"Bistro",
		/*gltfPath=*/"content/models/Bistro/bistro.gltf",
		/*pakPath=*/"content/paks/bistro.pak",
		// Canon test view (user-specified): mid-street, looking down the
		// road toward the far corner; yaw -80.4 / pitch -4.0 from pos
		// (27.61, 2.26, -10.19), fwd (-0.986, 0.012, -0.167).
		/*camPos=*/glm::vec3(27.61f, 2.26f, -10.19f),
		/*camTarget=*/glm::vec3(27.61f, 2.26f, -10.19f)
			+ glm::vec3(-0.986f, 0.012f, -0.167f),
		/*farClip=*/200.0f,
		/*coordFix=*/0,
	};
	return bistro;
}

const SceneDesc& SceneRegistry::Scanlands() {
	// Scanlands GLB (staged + cooked by tools/cook_scanlands.py: embedded
	// GLB images extracted to textures/*.ktx2, pak at content/paks with
	// meshlets + LODs). No GI bake by design â€” the app falls back to the
	// analytic sky IBL when content/gi/scanlands.sggi is absent.
	// World layout (engine space, coordFix 0 flips glTF x/z): a ~18 km
	// procedural scatter (186k tree instances of glTF mesh 0, planted
	// from the origin outward in node order; base heights vary by tens
	// of meters over meters) with a hero village (boats, city,
	// buildings) at engine â‰ˆ (-520, 105, 150). Engine limits (kMaxDraws
	// 4096/view, MAX_MATERIALS 4096) saturate on this scene and the
	// filter pushes in node order, so the default view overlooks the
	// sparser ground east of the origin (fly with WASD; the village
	// lies ~700 m southwest). The far clip covers village-scale views.
	static const SceneDesc scanlands{
		/*id=*/"scanlands",
		/*label=*/"Scanlands",
		/*gltfPath=*/"content/models/Scanlands/scanlands.gltf",
		/*pakPath=*/"content/paks/scanlands.pak",
		/*camPos=*/glm::vec3(50.0f, 48.0f, -50.0f),
		/*camTarget=*/glm::vec3(-20.0f, 15.0f, 20.0f),
		/*farClip=*/600.0f,
		/*coordFix=*/0,
	};
	return scanlands;
}

const SceneDesc& SceneRegistry::BistroNiagara() {
	// Known-good Niagara bistro.gltf (staged + root-shim neutralized by
	// tools/stage_niagara_gltf.py). A/B against "bistro" (Lumberyard v5.2
	// via Blender): if this one renders clean through the same direct
	// import path, the bug is in the v5.2 conversion; if it explodes too,
	// the bug is in the engine import/draw path. No pak is cooked for it,
	// so the loader always takes the direct glTF branch. Textures are
	// DDS/PNG upstream and intentionally unresolved (geometry check only).
	static const SceneDesc bistroNiagara{
		/*id=*/"bistro-niagara",
		/*label=*/"Bistro (Niagara glTF)",
		/*gltfPath=*/"content/models/BistroNiagara/bistro.gltf",
		/*pakPath=*/"content/paks/bistro-niagara.pak",
		/*camPos=*/glm::vec3(7.0f, 4.0f, 9.0f),
		/*camTarget=*/glm::vec3(0.0f, 2.0f, 0.0f),
		/*farClip=*/200.0f,
		/*coordFix=*/0,
	};
	return bistroNiagara;
}

const SceneDesc& SceneRegistry::LayersCube() {
	// Procedural layered-material test: a subdivided cube with smooth
	// vertex-color layer weights (R/G along X, B/A along Y) blending four
	// Sponza-sourced PBR materials. No glTF/pak involved (builtin tag).
	static const SceneDesc layersCube{
		/*id=*/"layers-cube",
		/*label=*/"Layers Cube (vertex paint)",
		/*gltfPath=*/"",
		/*pakPath=*/"",
		/*camPos=*/glm::vec3(3.4f, 2.6f, 3.4f),
		/*camTarget=*/glm::vec3(0.0f, 0.1f, 0.0f),
		/*farClip=*/50.0f,
		/*coordFix=*/1,
		/*preTransform=*/glm::mat4(1.0f),
		/*builtin=*/"layers-cube",
	};
	return layersCube;
}
} // namespace app (scene registry methods; class methods open their own blocks below)

namespace app {

 	std::vector<SceneDesc> SceneRegistry::All() {
 	return { Sponza(), Bistro(), BistroNiagara(), Scanlands(), LayersCube() };
}

std::optional<SceneDesc> SceneRegistry::Find(const std::string& id) {
	for (const SceneDesc& s : All()) {
		if (id == s.id)
			return s;
	}
return std::nullopt;
	}
} // namespace app

namespace app {

void SceneRenderer::Configure(SceneDesc scene, ScenePassConfig passes) {
	m_scene = scene;
	m_passes = passes;
	// Debug: force the AA mode for headless A/B benches (the UI combo
	// keeps working after this; values: off | msaa2x | smaa1x | smaat2x).
	if (const char* aaEnv = std::getenv("VULKANTOY_AA")) {
		if (!_stricmp(aaEnv, "off"))
			m_aaMode = AaMode::Off;
		else if (!_stricmp(aaEnv, "msaa2x"))
			m_aaMode = AaMode::Msaa2x;
		else if (!_stricmp(aaEnv, "smaa1x"))
			m_aaMode = AaMode::Smaa1x;
		else if (!_stricmp(aaEnv, "smaat2x"))
			m_aaMode = AaMode::SmaaT2x;
		else
			std::cerr << "[SceneRenderer] bad VULKANTOY_AA (want "
				"off|msaa2x|smaa1x|smaat2x): " << aaEnv << "\n";
	}
	// Opt-in A2C for masked cutout in MSAA mode (quality trade-off, see
	// m_msaaAlphaToCoverage).
	if (const char* a2cEnv = std::getenv("VULKANTOY_A2C")) {
		m_msaaAlphaToCoverage = std::atoi(a2cEnv) != 0;
	}
	// Point the FPS camera at the scene's default viewpoint (Camera keeps a
	// stable pointer to m_cameraPositioner, so lookAt is assignment-safe).
	m_cameraPositioner.lookAt(scene.camPos, scene.camTarget,
		glm::vec3(0.0f, 1.0f, 0.0f));
	// Debug: rotate the default view around world up (degrees) ï¿½ A/B runs
	// without mouse input (e.g. sponza spawns facing away from the demo
	// light grid; VULKANTOY_CAM_YAW=90 turns to face it).
	glm::vec3 dir = glm::normalize(scene.camTarget - scene.camPos);
	bool dirOverridden = false;
	if (const char* fwdEnv = std::getenv("VULKANTOY_CAM_FWD")) {
		// Absolute forward override ("x,y,z"): wins over the yaw env so a
		// fully specified diagnostic camera reproduces exactly.
		glm::vec3 fwd(0.0f);
		if (std::sscanf(fwdEnv, "%f,%f,%f", &fwd.x, &fwd.y, &fwd.z) == 3
			&& glm::length(fwd) > 1e-4f) {
			dir = glm::normalize(fwd);
			dirOverridden = true;
		} else {
			std::cerr << "[SceneRenderer] bad VULKANTOY_CAM_FWD (want x,y,z): "
				<< fwdEnv << "\n";
		}
	}
	if (!dirOverridden) {
		if (const char* yawEnv = std::getenv("VULKANTOY_CAM_YAW")) {
			const float yaw = std::strtof(yawEnv, nullptr);
			const float rad = glm::radians(yaw);
			dir = glm::vec3(
				dir.x * std::cos(rad) + dir.z * std::sin(rad), dir.y,
				-dir.x * std::sin(rad) + dir.z * std::cos(rad));
		}
	}
	// Debug: override the spawn position ("x,y,z"); composes with the
	// yaw env (the view keeps looking along the yaw-rotated default
	// direction) or with the absolute VULKANTOY_CAM_FWD.
	if (const char* posEnv = std::getenv("VULKANTOY_CAM_POS")) {
		glm::vec3 pos(scene.camPos);
		if (std::sscanf(posEnv, "%f,%f,%f", &pos.x, &pos.y, &pos.z) == 3) {
			m_cameraPositioner.lookAt(pos, pos + dir, glm::vec3(0.0f, 1.0f, 0.0f));
			return;
		}
		std::cerr << "[SceneRenderer] bad VULKANTOY_CAM_POS (want x,y,z): "
			<< posEnv << "\n";
	}
	m_cameraPositioner.lookAt(scene.camPos, scene.camPos + dir,
		glm::vec3(0.0f, 1.0f, 0.0f));
}

// FrameUniforms / AppResources / ViewportResources live in
// App/scene_renderer.h (needed by the class declaration); method
// definitions follow the (removed) in-class block below.

} // namespace app (scene registry + helpers above; methods open their own block below)


// Class declaration lives in App/scene_renderer.h; out-of-line method
// definitions follow.
namespace app {
using Super = smvk::AppImpl;

namespace {

// Layer sources for the layers-cube test: four visually distinct Sponza
// baseColor KTX2s with spread PBR factors (the metallic layer 3 exercises
// the specular blend path).
struct LayerSource {
	const char* ktx = nullptr;
	float roughness = 1.0f;
	float metallic = 0.0f;
};
constexpr LayerSource kCubeLayers[4] = {
	{ "content/models/sponza_out/5792855332885324923.ktx2", 0.9f, 0.0f },
	{ "content/models/sponza_out/7441062115984513793.ktx2", 0.6f, 0.0f },
	{ "content/models/sponza_out/11490520546946913238.ktx2", 0.4f, 0.2f },
	{ "content/models/sponza_out/15295713303328085182.ktx2", 0.3f, 0.6f },
};

// Subdivided cube (2x2x2, centered) with smooth vertex-color layer weights:
// R = 1-u, G = u along X; B = 1-v, A = v along Y. Corners isolate single
// layers, edges blend pairs, faces blend all four â€” the full blend space
// on one mesh. Per-face grids (split vertices) keep normals crisp.
smvk::StaticMeshUpload BuildLayerCubeUpload(int segments) {
	smvk::StaticMeshUpload data;
	data.aabbMin = glm::vec3(-1.0f);
	data.aabbMax = glm::vec3(1.0f);
	struct Face {
		glm::vec3 n, u, v, origin;
	};
	// (outward N; u = screen-right, v = screen-up seen from outside;
	// N == u x v; origin = bottom-left corner.)
	const Face faces[6] = {
		{ { 1, 0, 0 }, { 0, 0,-1 }, { 0, 1, 0 }, { 1,-1, 1 } }, // +X
		{ {-1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, {-1,-1,-1 } }, // -X
		{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0,-1 }, {-1, 1, 1 } }, // +Y
		{ { 0,-1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, {-1,-1,-1 } }, // -Y
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, {-1,-1, 1 } }, // +Z
		{ { 0, 0,-1 }, {-1, 0, 0 }, { 0, 1, 0 }, { 1,-1,-1 } }, // -Z
	};
	const int n = std::max(1, segments);
	for (const Face& f : faces) {
		const uint32_t base = static_cast<uint32_t>(data.vertices.size());
		for (int j = 0; j <= n; ++j) {
			for (int i = 0; i <= n; ++i) {
				const float a = static_cast<float>(i) / static_cast<float>(n);
				const float b = static_cast<float>(j) / static_cast<float>(n);
				smvk::StaticVertex v{};
				v.pos = f.origin + f.u * (2.0f * a) + f.v * (2.0f * b);
				const float u = v.pos.x * 0.5f + 0.5f;
				const float w = v.pos.y * 0.5f + 0.5f;
				v.color = glm::vec4(1.0f - u, u, 1.0f - w, w);
				v.texCoord = glm::vec2(a, b);
				v.normal = f.n;
				v.tangent = glm::vec4(f.u, 1.0f);
				data.vertices.push_back(v);
			}
		}
		for (int j = 0; j < n; ++j) {
			for (int i = 0; i < n; ++i) {
				const uint32_t a = base + (uint32_t)j * (n + 1) + i;
				const uint32_t b = a + 1;
				const uint32_t c = a + (n + 1) + 1;
				const uint32_t d = a + (n + 1);
				data.indices.push_back(a);
				data.indices.push_back(b);
				data.indices.push_back(c);
				data.indices.push_back(a);
				data.indices.push_back(c);
				data.indices.push_back(d);
			}
		}
	}
	return data;
}

// UV sphere (lat-long) centered on the origin. Triangles follow the same
// (a,b,c),(a,c,d) pattern as the cube above with b one step in +theta
// (longitude) and d one step in +phi (pole to pole): dTheta x dPhi points
// outward, so the winding matches the engine's CCW front faces.
smvk::StaticMeshUpload BuildChromeBallUpload(float radius, int slices, int stacks) {
	smvk::StaticMeshUpload data;
	data.aabbMin = glm::vec3(-radius);
	data.aabbMax = glm::vec3(radius);
	const float pi = glm::pi<float>();
	const int nSlices = std::max(8, slices);
	const int nStacks = std::max(4, stacks);
	for (int j = 0; j <= nStacks; ++j) {
		const float phi = pi * static_cast<float>(j) / static_cast<float>(nStacks);
		const float sinPhi = std::sin(phi);
		const float cosPhi = std::cos(phi);
		for (int i = 0; i <= nSlices; ++i) {
			const float theta = 2.0f * pi * static_cast<float>(i) / static_cast<float>(nSlices);
			const float sinT = std::sin(theta);
			const float cosT = std::cos(theta);
			const glm::vec3 n(sinPhi * cosT, cosPhi, sinPhi * sinT);
			smvk::StaticVertex v{};
			v.pos = n * radius;
			v.color = glm::vec4(1.0f);
			v.texCoord = glm::vec2(
				static_cast<float>(i) / static_cast<float>(nSlices),
				static_cast<float>(j) / static_cast<float>(nStacks));
			v.normal = n;
			glm::vec3 t(-sinT, 0.0f, cosT);
			if (glm::dot(t, t) < 1e-12f)
				t = glm::vec3(1.0f, 0.0f, 0.0f); // poles: theta tangent degenerates
			v.tangent = glm::vec4(glm::normalize(t), 1.0f);
			data.vertices.push_back(v);
		}
	}
	const uint32_t row = static_cast<uint32_t>(nSlices) + 1u;
	for (int j = 0; j < nStacks; ++j) {
		for (int i = 0; i < nSlices; ++i) {
			const uint32_t a = static_cast<uint32_t>(j) * row + static_cast<uint32_t>(i);
			const uint32_t b = a + 1u;
			const uint32_t c = a + row + 1u;
			const uint32_t d = a + row;
			data.indices.push_back(a);
			data.indices.push_back(b);
			data.indices.push_back(c);
			data.indices.push_back(a);
			data.indices.push_back(c);
			data.indices.push_back(d);
		}
	}
	return data;
}

} // namespace

bool SceneRenderer::BuildLayersCubeScene(smvk::Cluster& cluster) {
	// Textures (full-resident; a 4-texture test doesn't need streaming).
	smvk::TextureHandle layerTex[4]{};
	for (int i = 0; i < 4; ++i) {
		smvk::Holder<smvk::TextureHandle> holder =
			smvk::LoadKtxTextureFile(*m_context, kCubeLayers[i].ktx, {});
		if (holder.empty()) {
			std::cerr << "[LayersCube] texture load failed: "
				<< kCubeLayers[i].ktx << std::endl;
			return false;
		}
		layerTex[i] = holder;
		cluster.textures.create(std::move(holder));
	}
	// Materials (layer 0 = base; layers 1..3 referenced by pool index).
	smvk::ClusterMaterialHandle layerHandle[4]{};
	// Flat-factor mode (VULKANTOY_LAYERS_FLAT=1): saturated albedo factors
	// instead of textures, for validating the blend weights by eye
	// (corners isolate single layers, edges blend pairs).
	const bool flatValidate = std::getenv("VULKANTOY_LAYERS_FLAT") != nullptr;
	const glm::vec4 flatColors[4] = {
		glm::vec4(1.0f, 0.15f, 0.15f, 1.0f),
		glm::vec4(0.15f, 1.0f, 0.15f, 1.0f),
		glm::vec4(0.15f, 0.15f, 1.0f, 1.0f),
		glm::vec4(1.0f, 1.0f, 0.15f, 1.0f),
	};
	for (int i = 0; i < 4; ++i) {
		smvk::MaterialCreateInfo ci{};
		ci.baseColorTexture = flatValidate ? smvk::TextureHandle{} : layerTex[i];
		ci.diffuse = flatValidate ? smvk::TextureHandle{} : layerTex[i];
		ci.baseColorFactor = flatValidate ? flatColors[i] : glm::vec4(1.0f);
		ci.metallicFactor = kCubeLayers[i].metallic;
		ci.roughnessFactor = kCubeLayers[i].roughness;
		ci.emissiveFactor = glm::vec3(0.0f);
		ci.alphaMode = smvk::AlphaMode::Opaque;
		ci.shadingModel = smvk::ShadingModel::DefaultLit;
		layerHandle[i] = cluster.CreateMaterial(ci);
		if (layerHandle[i].empty()) {
			std::cerr << "[LayersCube] material create failed for layer "
				<< i << std::endl;
			return false;
		}
	}
	// Wire the base material's layer table (cluster-local ids; the World
	// remap turns them into world material ids at ResolveAndFixup).
	if (smvk::Material* base = cluster.materials.get(layerHandle[0])) {
		base->values.layerMaterials = glm::ivec4(
			static_cast<int32_t>(layerHandle[1].index()),
			static_cast<int32_t>(layerHandle[2].index()),
			static_cast<int32_t>(layerHandle[3].index()),
			4);
	}
	else {
		return false;
	}
	// Mesh + nodes.
	smvk::StaticMeshUpload upload = BuildLayerCubeUpload(8);
	smvk::ClusterStaticMeshHandle meshHandle = cluster.UploadStaticMesh(upload);
	if (meshHandle.empty()) {
		std::cerr << "[LayersCube] mesh upload failed\n";
		return false;
	}
	const int root = addNode(cluster.scene, -1, 0);
	cluster.scene.localTransform[root] = glm::mat4(1.0f);
	cluster.scene.globalTransform[root] = glm::mat4(1.0f);
	const int cubeNode = addNode(cluster.scene, root, 1);
	cluster.scene.localTransform[cubeNode] = glm::mat4(1.0f);
	cluster.scene.globalTransform[cubeNode] = glm::mat4(1.0f);
	cluster.scene.meshForNode[cubeNode] = meshHandle.index();
	cluster.scene.materialForNode[cubeNode] = layerHandle[0].index();
	markAsChanged(cluster.scene, cubeNode);
	std::cerr << "[LayersCube] cube built: "
		<< upload.vertices.size() << " verts, "
		<< upload.indices.size() / 3 << " tris, 4 layered materials\n";
	return true;
}

bool SceneRenderer::Initialize() {
		if (!Super::Initialize())
			return false;

		// DLSS capability is fixed per run (adapter + driver + Streamline
		// runtime): the device exists now, so ask once. Failure leaves the
		// mode combo cosmetic â€” the status readout explains why.
		m_dlssSupported = smvk::DlssFeatureSupported();

		// Env values arrive with shell quoting artifacts (notably a
		// trailing space from `set VAR=x && ...` in cmd, which broke
		// string-compared toggles while atoi-based ones survived). Trim
		// ASCII whitespace so "0 " behaves like "0".
		auto trimEnv = [](const char* s) -> std::string_view {
			std::string_view v{ s ? s : "" };
			while (!v.empty() && (v.front() == ' ' || v.front() == '\t'
				|| v.front() == '\r' || v.front() == '\n'))
				v.remove_prefix(1);
			while (!v.empty() && (v.back() == ' ' || v.back() == '\t'
				|| v.back() == '\r' || v.back() == '\n'))
				v.remove_suffix(1);
			return v;
		};

		// Headless toggles for A/B runs without UI interaction.
		// Kill the occluder-depth path (VULKANTOY_HIZ=0).
		if (const char* hizEnv = std::getenv("VULKANTOY_HIZ")) {
			if (std::atoi(hizEnv) == 0)
				m_hizOccluderDepth = false;
		}
		// Headless LOD kill (VULKANTOY_LOD=0 forces LOD0).
		if (const char* lodEnv = std::getenv("VULKANTOY_LOD")) {
			if (std::atoi(lodEnv) == 0)
				m_lodEnabled = false;
		}
		// Headless shader-LOD kill (VULKANTOY_SHADERLOD=0 keeps the
		// farthest level full-lit) for A/B shading-cost runs.
		if (const char* lodShEnv = std::getenv("VULKANTOY_SHADERLOD")) {
			if (std::atoi(lodShEnv) == 0)
				m_lodShaderEnabled = false;
		}
		// Headless wireframe opt-in (VULKANTOY_WIREFRAME=1) for capture/A-B
		// runs without UI interaction; the P toggle stays live after.
		if (const char* wfEnv = std::getenv("VULKANTOY_WIREFRAME")) {
			if (std::atoi(wfEnv) == 1)
				m_wireframe = true;
		}
		// Headless debug-view select (lit|unlit|meshlets|cascades|lods|vertexcolors).
		if (const char* dvEnv = std::getenv("VULKANTOY_DEBUGVIEW")) {
			const std::string_view dv = trimEnv(dvEnv);
			if (dv == "lit") m_debugView = DebugView::Lit;
			else if (dv == "unlit") m_debugView = DebugView::Unlit;
			else if (dv == "meshlets") m_debugView = DebugView::Meshlets;
			else if (dv == "cascades") m_debugView = DebugView::Cascades;
			else if (dv == "lods") m_debugView = DebugView::Lods;
			else if (dv == "vertexcolors" || dv == "vcolors") m_debugView = DebugView::VertexColors;
			else if (dv == "uv0" || dv == "uv1_base") m_debugView = DebugView::Uv0;
			else if (dv == "uv1" || dv == "uv2" || dv == "uv1_lightmap") m_debugView = DebugView::Uv1;
			else std::cerr << "[SceneRenderer] unknown VULKANTOY_DEBUGVIEW value: " << dv << "\n";
		}
		// AA mode override for A/B runs without UI interaction
		// (off | msaa | smaa1x | t2x). The ImGui combo stays live after.
		if (const char* aaEnv = std::getenv("VULKANTOY_AA")) {
			const std::string_view aa = trimEnv(aaEnv);
			if (aa == "off") m_aaMode = AaMode::Off;
			else if (aa == "msaa") m_aaMode = AaMode::Msaa2x;
			else if (aa == "smaa1x") m_aaMode = AaMode::Smaa1x;
			else if (aa == "t2x") m_aaMode = AaMode::SmaaT2x;
			else std::cerr << "[SceneRenderer] unknown VULKANTOY_AA value: " << aa << "\n";
		}

		// DLSS quality-mode override for A/B runs (off | ultraperf | perf |
		// balanced | quality | ultraquality | dlaa). DLSS takes over the
		// internal resolution + upscaling and bypasses the AA modes.
		if (const char* dlssEnv = std::getenv("VULKANTOY_DLSS")) {
			const std::string_view dlss = trimEnv(dlssEnv);
			if (dlss == "off") m_dlssMode = smvk::DlssMode::Off;
			else if (dlss == "ultraperf" || dlss == "ultraperformance")
				m_dlssMode = smvk::DlssMode::UltraPerformance;
			else if (dlss == "perf" || dlss == "performance")
				m_dlssMode = smvk::DlssMode::MaxPerformance;
			else if (dlss == "balanced") m_dlssMode = smvk::DlssMode::Balanced;
			else if (dlss == "quality") m_dlssMode = smvk::DlssMode::MaxQuality;
			else if (dlss == "ultraquality") m_dlssMode = smvk::DlssMode::UltraQuality;
			else if (dlss == "dlaa") m_dlssMode = smvk::DlssMode::DLAA;
			else std::cerr << "[SceneRenderer] unknown VULKANTOY_DLSS value: " << dlss << "\n";
		}

		// Internal render-resolution override for headless A/B runs (the
		// ImGui "Resolution" slider stays live after; 1.0 = swapchain 1:1).
		if (const char* rsEnv = std::getenv("VULKANTOY_RENDER_SCALE")) {
			const float rs = std::strtof(rsEnv, nullptr);
			if (rs >= 0.25f && rs <= 2.0f)
				m_renderScale = rs;
			else
				std::cerr << "[SceneRenderer] VULKANTOY_RENDER_SCALE out of range "
					<< "(0.25..2.0): " << rsEnv << "\n";
		}

		// Transparency technique override for A/B runs without UI
		// interaction (dither | blend). Default is dithered; the ImGui
		// combo stays live after.
		if (const char* trEnv = std::getenv("VULKANTOY_TRANSPARENCY")) {
			const std::string_view tr = trimEnv(trEnv);
			if (tr == "dither" || tr == "dithered") m_transparentMode = smvk::TransparentMode::Dithered;
			else if (tr == "blend" || tr == "blended") m_transparentMode = smvk::TransparentMode::Blended;
			else std::cerr << "[SceneRenderer] unknown VULKANTOY_TRANSPARENCY value: " << tr << "\n";
		}

		// Deferred shadows override for A/B runs without UI interaction
		// (1 | 0). Runtime H toggle + the ImGui checkbox stay live after.
		if (const char* dsEnv = std::getenv("VULKANTOY_DS")) {
			const std::string_view ds = trimEnv(dsEnv);
			if (ds == "1" || ds == "on") m_dsEnabled = true;
			else if (ds == "0" || ds == "off") m_dsEnabled = false;
			else std::cerr << "[SceneRenderer] unknown VULKANTOY_DS value: " << ds << "\n";
		}
		// One-shot deferred-shadow buffer dump after frame <n> Execute
		// (stalls like the HZB dump; see DumpDeferredShadows). Frame 0 is
		// warmup (seed/targets), so steady-state frames are the useful ones.
		if (const char* dumpEnv = std::getenv("VULKANTOY_DS_DUMP")) {
			const int dumpFrame = std::atoi(dumpEnv);
			if (dumpFrame >= 0)
				m_dumpAtFrame = dumpFrame;
		}
#if defined(DS_DEBUG_VIZ)
		// Deferred-shadow visualizer mode for headless runs (0..7, see
		// DsDebugViz): pairs with VULKANTOY_CAPTURE to bisect the pipeline
		// without UI interaction. Debug builds only (release compiles the
		// visualizers out; the env var reads as off there).
		if (const char* vizEnv = std::getenv("VULKANTOY_DS_VIZ")) {
			const int viz = std::atoi(vizEnv);
			if (viz >= 0 && viz <= static_cast<int>(DsDebugViz::LightVis))
				m_dsDebugMode = static_cast<DsDebugViz>(viz);
			else
				std::cerr << "[SceneRenderer] unknown VULKANTOY_DS_VIZ value: "
					<< viz << " (0..7)\n";
		}
#endif
		// Screenshot capture at a fixed frame number (no F7 needed for
		// headless A/B runs; same PNG export path as the UI capture).
		if (const char* capEnv = std::getenv("VULKANTOY_CAPTURE")) {
			m_captureAtFrame = std::atoi(capEnv);
		}
		// Freeze the demo light animation (bob/orbit/flicker) so A/B
		// screenshot comparisons isolate the rendering change instead of
		// the wall-clock phase of the animated lights.
		if (const char* statEnv = std::getenv("VULKANTOY_LIGHTS_STATIC")) {
			if (std::atoi(statEnv) != 0) {
				m_fpDemo.animate = false;
				m_fpDemo.lightFunctionsEnabled = false;
			}
		}
		// Mute the forward+ demo lights entirely (1 | 0): removes their
		// direct light + tile passes from A/B runs (pure sun + GI frame;
		// runtime F toggle + the ImGui checkbox stay live after).
		if (const char* lightsEnv = std::getenv("VULKANTOY_LIGHTS")) {
			const std::string_view lights = trimEnv(lightsEnv);
			if (lights == "1" || lights == "on") m_fpDemo.enabled = true;
			else if (lights == "0" || lights == "off") m_fpDemo.enabled = false;
			else std::cerr << "[SceneRenderer] unknown VULKANTOY_LIGHTS value: " << lights << "\n";
		}
		// Forward+ decals (tiled projected splats) toggle (1 | 0):
		// 0 also skips the Sponza preset splats at load; J + the ImGui
		// checkbox stay live after.
		if (const char* decEnv = std::getenv("VULKANTOY_DECALS")) {
			const std::string_view dec = trimEnv(decEnv);
			if (dec == "1" || dec == "on") m_decalDemo.enabled = true;
			else if (dec == "0" || dec == "off") m_decalDemo.enabled = false;
			else std::cerr << "[SceneRenderer] unknown VULKANTOY_DECALS value: " << dec << "\n";
		}
		// Headless crosshair splat at a fixed frame (see m_decalAutoSplatAtFrame):
		// fires the J path once so capture runs verify the pick BVH.
		if (const char* autoEnv = std::getenv("VULKANTOY_DECAL_AUTOSPLAT")) {
			m_decalAutoSplatAtFrame = std::atoi(autoEnv);
		}
		// Deterministic A/B helpers: manual exposure + IBL off remove the
		// two global feedback paths (eye adaptation, sky IBL ambient) so
		// pixel diffs isolate the per-light shading change.
		if (const char* expEnv = std::getenv("VULKANTOY_AUTO_EXPOSURE")) {
			if (std::atoi(expEnv) == 0)
				m_autoExposureEnabled = false;
			else
				m_autoExposureEnabled = true;
		}
		// EVSM toggle for A/B (default ON; VULKANTOY_EVSM=0: PCF depth
		// atlases, no moment/blur passes, single-submit frames (no async
		// split); =1: force EVSM on).
		if (const char* evsmEnv = std::getenv("VULKANTOY_EVSM")) {
			if (std::atoi(evsmEnv) == 0)
				m_evsmEnabled = false;
			else
				m_evsmEnabled = true;
		}
		// EVSM softness override for A/B captures (VULKANTOY_EVSM_SOFT=x).
		if (const char* softEnv = std::getenv("VULKANTOY_EVSM_SOFT")) {
			m_evsmSoftness = std::max(0.0f, (float)std::atof(softEnv));
		}
		// Opt-in EVSM atlas dump (VULKANTOY_EVSM_DUMP=<frame>): see header.
		if (const char* dumpEnv = std::getenv("VULKANTOY_EVSM_DUMP")) {
			m_evsmDumpAtFrame = std::atoi(dumpEnv);
		}

		if (const char* iblEnv = std::getenv("VULKANTOY_IBL")) {
			// Mute the analytic sky ambient (any ambient mode -> off;
			// All carries sky + baked, so muting kills all ambient).
			if (std::atoi(iblEnv) == 0 && m_ambientMode != AmbientMode::Off)
				m_ambientMode = AmbientMode::Off;
		}
		// Ambient / GI source override for headless runs (same states the
		// N key cycles; the runtime cycle stays live after):
		//   VULKANTOY_AMBIENT=all|lightmaps|probes|skyibl|off
		if (const char* amb = std::getenv("VULKANTOY_AMBIENT")) {
			const std::string_view a = trimEnv(amb);
			if (a == "all") m_ambientMode = AmbientMode::All;
			else if (a == "lightmaps" || a == "lm") m_ambientMode = AmbientMode::Lightmaps;
			else if (a == "probes") m_ambientMode = AmbientMode::Probes;
			else if (a == "skyibl" || a == "ibl") m_ambientMode = AmbientMode::SkyIbl;
			else if (a == "off" || a == "none") m_ambientMode = AmbientMode::Off;
			else std::cerr << "[SceneRenderer] unknown VULKANTOY_AMBIENT value: "
				<< a << " (all|lightmaps|probes|skyibl|off)\n";
			std::cerr << "[SceneRenderer] ambient source: "
				<< AmbientModeName(m_ambientMode) << "\n";
		}
		// Legacy GI override (pre-ambient-modes runs): 1/on -> All,
		// 0/off -> out of the baked modes into SkyIbl.
		if (const char* giEnv = std::getenv("VULKANTOY_GI")) {
			const std::string_view gi = trimEnv(giEnv);
			if (gi == "1" || gi == "on") m_ambientMode = AmbientMode::All;
			else if (gi == "0" || gi == "off") {
				if (m_ambientMode == AmbientMode::All
					|| m_ambientMode == AmbientMode::Probes)
					m_ambientMode = AmbientMode::SkyIbl;
			}
			else std::cerr << "[SceneRenderer] unknown VULKANTOY_GI value: " << gi << "\n";
		}
		// GI debug view for headless runs (same modes the I key cycles;
		// F8 is the legacy alias): 0 off, 1 baked irradiance,
		// 2 source palette.
		if (const char* giDbgEnv = std::getenv("VULKANTOY_GI_DEBUG")) {
			const int mode = std::atoi(giDbgEnv);
			if (mode >= 0 && mode <= 2)
				m_giDebug = mode;
			else
				std::cerr << "[SceneRenderer] VULKANTOY_GI_DEBUG wants 0..2\n";
		}
		// Probe-point overlay for headless runs (same as the panel
		// checkbox; draws the probe lattice colored by octree level).
		if (const char* giPtsEnv = std::getenv("VULKANTOY_GI_POINTS")) {
			if (std::atoi(giPtsEnv) != 0)
				m_giShowProbes = true;
		}
		// Occlusion cull for the probe overlay (same as the panel
		// checkbox; hides probes behind scene geometry via the CPU pick
		// BVH so captures show only probes that can affect the view).
		if (const char* giOccEnv = std::getenv("VULKANTOY_GI_OCCLUDE")) {
			if (std::atoi(giOccEnv) != 0)
				m_giProbeOcclusionCull = true;
		}

		// Tear-free VSync without the strict-FIFO 30fps lock (see PresentMode);
		// the overlay combo can change it later (applies via Recreate in DrawFrame).
		m_swapchain->SetPresentMode(m_presentMode);
		{
			const auto vkExt = m_swapchain->GetExtent();
			UpdateCameraAspect(ScaledExtent(
				smvk::Extent2D{ vkExt.width, vkExt.height }, m_renderScale));
		}
		m_resources.frameUniforms = m_context->CreateBuffer(
			sizeof(FrameUniforms) * m_swapchain->framesInFlight(),
			smvk::BufferType_Uniform,
			"frame uniforms"
		);
		m_resources.lightsBuffer = m_context->CreateBuffer(
			sizeof(smvk::Light) * MAX_LIGHTS * m_swapchain->framesInFlight(),
			smvk::BufferType_Storage,
			"lights buffer"
		);
		m_resources.materialsBuffer = m_context->CreateBuffer(
			sizeof(smvk::MaterialValues) * MAX_MATERIALS * m_swapchain->framesInFlight(),
			smvk::BufferType_Storage,
			"materials buffer"
		);
		m_resources.directionalCascadeBuffer = m_context->CreateBuffer(
			sizeof(smvk::GpuDirectionalLightDescription) * m_swapchain->framesInFlight(),
			smvk::BufferType_Storage,
			"directional cascade buffer"
		);
		m_resources.fpLightsBuffer = m_context->CreateBuffer(
			sizeof(smvk::ForwardPlusLight) * FP_MAX_LIGHTS * m_swapchain->framesInFlight(),
			smvk::BufferType_Storage,
			"forwardplus lights buffer"
		);
		m_resources.fpLightsLogical = m_rendergraph->AddExternalBuffer(
			m_resources.fpLightsBuffer, "forwardplus lights");
		// Decal ring (one ForwardPlusDecal[FP_MAX_DECALS] slice per flight
		// frame; rebuilt from DecalDemo splats every frame, see UpdateDecals).
		m_resources.decalsBuffer = m_context->CreateBuffer(
			sizeof(smvk::ForwardPlusDecal) * FP_MAX_DECALS * m_swapchain->framesInFlight(),
			smvk::BufferType_Storage,
			"forwardplus decals buffer"
		);
		m_resources.decalsLogical = m_rendergraph->AddExternalBuffer(
			m_resources.decalsBuffer, "forwardplus decals");
		m_resources.spotShadowBuffer = m_context->CreateBuffer(
			sizeof(smvk::GpuCascadeDescription) * smvk::kMaxSpotShadows * m_swapchain->framesInFlight(),
			smvk::BufferType_Storage,
			"spot shadow tiles buffer"
		);
		m_resources.spotShadowLogical = m_rendergraph->AddExternalBuffer(
			m_resources.spotShadowBuffer, "spot shadow tiles");
		// RenderView slab: per-frame virtual cameras (main + shadow views).
		if (!m_renderViews.Ensure(*m_context, m_swapchain->framesInFlight())) {
			std::cerr << "[SceneRenderer] Failed to create render views buffer\n";
			return false;
		}
		// Procedural blood-splat textures for the decal system (bindless;
		// failure is non-fatal â€” DecalsActive() stays false and the
		// decal legs simply never run).
		if (!InitDecalTextures()) {
			std::cerr << "[SceneRenderer] decal textures unavailable - decals off\n";
		}

		// VFS overlay: loose content/ for iteration, cooked paks mounted
		// underneath (loose files win on collisions). Shader reads below
		// go through the same overlay so packaged builds just work.
		// Engine-owned baked resources (light cookies, staged next to the
		// app by its CMake step) sit at the lowest priority so app
		// content/paks always win; CookieLibrary reads its .ktx2 files
		// through this overlay.
		smvk::OverlayVfs contentVfs;
		contentVfs.Mount(std::make_shared<smvk::LooseBackend>("content"), 0, "content");
		contentVfs.Mount(
			std::make_shared<smvk::LooseBackend>("engine_resources"), 100, "engine_resources");

		// Main ShaderProgram (scene-owned; shadow/bloom/tonemap programs live
		// in their engine builders with defaults + Ensure).
		auto vertCode = readContentFile(contentVfs, "shaders/default.vert.hlsl.spv");
		auto fragCode = readContentFile(contentVfs, "shaders/default_lit.frag.hlsl.spv");
		m_resources.vertShader = m_context->CreateShaderModule(vertCode, "vert");
		m_resources.fragShader = m_context->CreateShaderModule(fragCode, "fragLit");

		auto vert = smvk::Shader{
			.module = m_resources.vertShader,
			.entryPoint = "mainVS",
			.stage = smvk::ShaderStage::Vertex
		};
		auto frag = smvk::Shader{
			.module = m_resources.fragShader,
			.entryPoint = "mainPS",
			.stage = smvk::ShaderStage::Fragment
		};
	m_resources.mainProgram = {
		.shaders = {vert, frag},
		.shaderCount = 2
	};

	// A2C lit twin (default_lit_a2c): masked cutout via coverage instead
	// of discard for 2x MSAA Main. Non-fatal: missing module keeps every
	// material on the discard variant.
	try {
		auto a2cCode = readContentFile(contentVfs, "shaders/default_lit_a2c.frag.hlsl.spv");
		m_resources.fragA2CShader = m_context->CreateShaderModule(a2cCode, "fragLitA2C");
	}
	catch (...) {
		std::cerr << "[SceneRenderer] default_lit_a2c unavailable - masked MSAA keeps the discard test\n";
	}
	if (!m_resources.fragA2CShader.empty()) {
		m_resources.mainProgramA2C = {
			.shaders = {
				smvk::Shader{
					.module = m_resources.vertShader,
					.entryPoint = "mainVS",
					.stage = smvk::ShaderStage::Vertex
				},
				smvk::Shader{
					.module = m_resources.fragA2CShader,
					.entryPoint = "mainPS",
					.stage = smvk::ShaderStage::Fragment
				}
			},
			.shaderCount = 2
		};
	}

		// Debug-view fragments (F2 unlit, F4 cascade coverage, F6 vertex
		// colors): same VS as main. Failure is non-fatal (program stays at
		// shaderCount 0 and the view falls back to Lit at record time).
		try {
			auto unlitCode = readContentFile(contentVfs, "shaders/default_unlit.frag.hlsl.spv");
			m_resources.fragUnlitShader = m_context->CreateShaderModule(unlitCode, "fragUnlit");
		}
		catch (...) {
			std::cerr << "[SceneRenderer] default_unlit unavailable - F2 falls back to Lit\n";
		}
		try {
			auto cascadeCode = readContentFile(contentVfs, "shaders/default_cascade_debug.frag.hlsl.spv");
			m_resources.fragCascadeShader = m_context->CreateShaderModule(cascadeCode, "fragCascade");
		}
		catch (...) {
			std::cerr << "[SceneRenderer] default_cascade_debug unavailable - F4 falls back to Lit\n";
		}
		try {
			auto vcolorCode = readContentFile(contentVfs, "shaders/default_vertexcolor_debug.frag.hlsl.spv");
			m_resources.fragVertexColorShader = m_context->CreateShaderModule(vcolorCode, "fragVertexColor");
		}
		catch (...) {
			std::cerr << "[SceneRenderer] default_vertexcolor_debug unavailable - F6 falls back to Lit\n";
		}
		if (!m_resources.fragUnlitShader.empty()) {
			m_resources.unlitProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragUnlitShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}
		if (!m_resources.fragCascadeShader.empty()) {
			m_resources.cascadeProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragCascadeShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}
		if (!m_resources.fragVertexColorShader.empty()) {
			m_resources.vertexColorProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragVertexColorShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}
		try {
			auto uv0Code = readContentFile(contentVfs, "shaders/default_uv0_debug.frag.hlsl.spv");
			m_resources.fragUv0DebugShader = m_context->CreateShaderModule(uv0Code, "fragUv0Debug");
		}
		catch (...) {
			std::cerr << "[SceneRenderer] default_uv0_debug unavailable - F9 falls back to Lit\n";
		}
		try {
			auto uv1Code = readContentFile(contentVfs, "shaders/default_uv1_debug.frag.hlsl.spv");
			m_resources.fragUv1DebugShader = m_context->CreateShaderModule(uv1Code, "fragUv1Debug");
		}
		catch (...) {
			std::cerr << "[SceneRenderer] default_uv1_debug unavailable - F10 falls back to Lit\n";
		}
		if (!m_resources.fragUv0DebugShader.empty()) {
			m_resources.uv0DebugProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragUv0DebugShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}
		if (!m_resources.fragUv1DebugShader.empty()) {
			m_resources.uv1DebugProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragUv1DebugShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}

		// Shader-LOD program (farthest level, see World::LodShaderAt):
		// vertex-lit VS + pass-through FS. Failure is non-fatal â€” the
		// subsystem falls back to full-lit everywhere (ShaderLodActive).
		try {
			auto vlVertCode = readContentFile(contentVfs, "shaders/default_vertexlit.vert.hlsl.spv");
			auto vlFragCode = readContentFile(contentVfs, "shaders/default_vertexlit.frag.hlsl.spv");
			m_resources.vertexLitVertShader = m_context->CreateShaderModule(vlVertCode, "vertLitLod");
			m_resources.vertexLitFragShader = m_context->CreateShaderModule(vlFragCode, "fragLitLod");
		}
		catch (...) {
			std::cerr << "[SceneRenderer] default_vertexlit unavailable - shader LODs stay full-lit\n";
		}
		if (!m_resources.vertexLitVertShader.empty()
			&& !m_resources.vertexLitFragShader.empty()) {
			m_resources.vertexLitProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertexLitVertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.vertexLitFragShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}

		// Wireframe (P) flat-line twins: default_wire.frag pairs with the
		// same VSs as the shaded legs (a FS consuming only SV_Position links
		// with any of them). Failure is non-fatal â€” wireframe keeps the
		// shaded FS (raster override only).
		try {
			auto wireCode = readContentFile(contentVfs, "shaders/default_wire.frag.hlsl.spv");
			m_resources.fragWireShader = m_context->CreateShaderModule(wireCode, "fragWire");
		}
		catch (...) {
			std::cerr << "[SceneRenderer] default_wire unavailable - wireframe keeps the shaded FS\n";
		}
		if (!m_resources.fragWireShader.empty()) {
			m_resources.mainWireProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragWireShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}
		if (!m_resources.fragWireShader.empty()
			&& !m_resources.vertexLitVertShader.empty()) {
			m_resources.vertexLitWireProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertexLitVertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragWireShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}

	// Masked shadow-depth program (leaf-shaped shadows for cutout).
	auto shadowVertCode = readContentFile(contentVfs, "shaders/shadow.vert.hlsl.spv");
	auto shadowFragCode = readContentFile(contentVfs, "shaders/shadow.frag.hlsl.spv");
	m_resources.shadowMaskedVertShader = m_context->CreateShaderModule(shadowVertCode, "shadowMaskedVert");
	m_resources.shadowMaskedFragShader = m_context->CreateShaderModule(shadowFragCode, "shadowMaskedFrag");
		m_resources.shadowMaskedProgram = {
			.shaders = {
				smvk::Shader{
					.module = m_resources.shadowMaskedVertShader,
					.entryPoint = "mainVS",
					.stage = smvk::ShaderStage::Vertex
				},
				smvk::Shader{
					.module = m_resources.shadowMaskedFragShader,
					.entryPoint = "mainPS",
					.stage = smvk::ShaderStage::Fragment
				}
			},
			.shaderCount = 2
		};
	// Reversed-Z conservative twin (shadow_rev.frag): the prepass / seed /
	// remainder masked phases run clear-0 GreaterEqual, so the FS must
	// promise "never nearer" (SV_DepthLessEqual), not the atlas's
	// "never farther". Failure is non-fatal: the prepass falls back to the
	// normal-Z module (conservative depth is a perf hint, not a
	// correctness requirement â€” the passthrough write satisfies both
	// bounds).
	try {
		auto shadowRevCode = readContentFile(contentVfs, "shaders/shadow_rev.frag.hlsl.spv");
		m_resources.shadowMaskedFragRevShader = m_context->CreateShaderModule(shadowRevCode, "shadowMaskedFragRev");
	}
	catch (...) {
		std::cerr << "[SceneRenderer] shadow_rev unavailable - prepass masked keeps the normal-Z FS\n";
	}

		// Masked prepass program: shadow's inputs (pos/color/uv) are a
		// subset of default.vert's outputs, so it links directly. Prefers
		// the reversed-Z conservative FS when loaded.
		m_resources.prepassMaskedProgram = {
			.shaders = {
				smvk::Shader{
					.module = m_resources.vertShader,
					.entryPoint = "mainVS",
					.stage = smvk::ShaderStage::Vertex
				},
				smvk::Shader{
					.module = !m_resources.shadowMaskedFragRevShader.empty()
						? m_resources.shadowMaskedFragRevShader
						: m_resources.shadowMaskedFragShader,
					.entryPoint = "mainPS",
					.stage = smvk::ShaderStage::Fragment
				}
			},
			.shaderCount = 2
		};

		smvk::AllocateStaticVertexFmt(*m_context);

		// Texture streaming: tail-resident glTF uploads with a CPU
		// screen-coverage demand pass (see UpdateTextureStreaming). The
		// streamer views cluster-owned KTX payloads, so clusters must
		// outlive it (they do: world owns both). Conservative defaults:
		// <=1K textures load full, 2K/4K load capped and upgrade on
		// demand within a few frames.
		// Kill switch for A/B (VULKANTOY_STREAMING=0): full-resident
		// legacy loads, no streamer, no feedback.
		if (const char* streamEnv = std::getenv("VULKANTOY_STREAMING")) {
			if (std::atoi(streamEnv) == 0)
				m_textureStreamingEnabled = false;
		}
		if (m_textureStreamingEnabled && !m_textureStreamer) {
			smvk::TextureStreamerConfig streamCfg;
			streamCfg.budgetBytes = 256ull * 1024 * 1024;
			streamCfg.minResidentMips = 2;
			streamCfg.initialResidentDimCap = 1024;
			streamCfg.maxUpgradesPerFrame = 4;
			streamCfg.maxDowngradesPerFrame = 8;
			streamCfg.evictCooldownFrames = 60;
			streamCfg.mipBias = 0.0f;
			m_textureStreamer =
				std::make_unique<smvk::TextureStreamer>(*m_context, streamCfg);
		}
		smvk::TextureStreamer* sceneStreamer =
			(m_textureStreamingEnabled && m_textureStreamer) ? m_textureStreamer.get() : nullptr;
		// GPU feedback consumer for the streamer above (null together with
		// it; the RHI owns the device ring buffers either way so the
		// binding is always valid for the shaders).
		if (sceneStreamer && !m_textureFeedback) {
			m_textureFeedback =
				std::make_unique<smvk::TextureFeedback>(*m_context);
			m_textureFeedback->SetSlotCount(MAX_TEXTURES);
		}

		// Scene import: prefer the cooked pak (fast path, meshopt'd meshes,
		// embedded textures), fall back to glTF via the VFS when no pak is
		// present (dev iteration). Either way the Cluster is registered
		// with the World and resolved into the runtime scene graph.
		// The backend is opened/mounted before the world is mutated so a
		// mount failure leaves the world untouched and the glTF fallback
		// loads the scene exactly once.
		// Scene paths come from the configured SceneDesc (see SceneRegistry):
		// adding a sample never copies this block.
		const std::string sceneGltf = m_scene.gltfPath ? m_scene.gltfPath : "";
		const std::string scenePak = m_scene.pakPath ? m_scene.pakPath : "";
		// Unconditional phase log: Release is otherwise silent, so an init
		// throw (e.g. bad_alloc) would name no phase. Keep these cerr lines.
		std::cerr << "[Scene] loading '" << m_scene.id << "' (pak: " << scenePak << ")"
			<< (m_forceGltf ? " FORCED GLTF" : "") << std::endl;
		bool loadedFromPak = false;
		// Procedural builtin scenes bypass pak/glTF entirely.
		const bool isBuiltinLayersCube = m_scene.builtin
			&& std::string(m_scene.builtin) == "layers-cube";
		if (isBuiltinLayersCube) {
			auto& cluster = m_world->LoadCluster(*m_context);
			if (!BuildLayersCubeScene(cluster)) {
				std::cerr << "[SceneRenderer] layers-cube build failed\n";
				return false;
			}
			loadedFromPak = true; // skip the glTF fallback below
		}
		if (!m_forceGltf)
		{
			std::ifstream pakFile(scenePak, std::ios::binary | std::ios::ate);
			if (pakFile) {
				const auto pakEnd = pakFile.tellg();
				if (pakEnd != std::ifstream::pos_type(-1)) {
					pakFile.seekg(0);
					auto pakImage = std::make_shared<std::vector<uint8_t>>(
						static_cast<size_t>(pakEnd));
					// Texture/material/mesh uploads below are synchronous, so
					// the pak image only needs to outlive this block; the
					// PakBackend mount serves VFS texture reads during init
					// (below loose content/ so dev overrides win).
					if (pakFile.read(reinterpret_cast<char*>(pakImage->data()),
							static_cast<std::streamsize>(pakImage->size()))) {
						std::string pakErr;
						auto reader = smvk::PakReader::Open(
							std::span<const uint8_t>(*pakImage), pakErr);
						if (!reader) {
							std::cerr << "[SceneRenderer] Pak parse failed (" << pakErr
								<< "), falling back to glTF" << std::endl;
						}
						else if (auto pakBackend = smvk::PakBackend::Open(
								pakImage, scenePak, pakErr)) {
							try {
								std::cerr << "[Scene] pak parsed, loading cluster ("
									<< reader->EntryCount() << " entries)..." << std::endl;
								m_world->LoadClusterFromPak(*m_context, *reader);
								// Serve pak-embedded textures through the VFS
								// (below loose content/ so dev overrides win).
								contentVfs.Mount(pakBackend, 10, scenePak);
							loadedFromPak = true;
							// NOTE: world mesh/material slots only exist
							// after ResolveAndFixup_clusters below, so no
							// counts are printed here (they would read 0).
							std::cerr << "[Scene] pak cluster loaded"
								<< std::endl;
							}
							catch (const std::exception& e) {
								std::cerr << "[SceneRenderer] Pak load failed, falling back "
									<< "to glTF: " << e.what() << std::endl;
								// Drain staged uploads while their destination
								// buffers are still alive, then reset the world
								// so the glTF fallback starts clean, and wait
								// for deferred destroys to complete. Without
								// this, queued copies reference destroyed
								// buffers and the fallback imports on top of
								// unreclaimed GPU memory (GPU fault).
								m_context->FlushUploads();
								WaitForDeviceIdle();
								m_world = std::make_unique<smvk::World>();
								m_context->ProcessDeferredTasks();
								WaitForDeviceIdle();
								// Release the (possibly huge) pak image before
								// the fallback import to keep peak memory down.
								pakImage->clear();
								pakImage->shrink_to_fit();
							}
						}
						else {
							std::cerr << "[SceneRenderer] PakBackend mount failed: "
								<< pakErr << std::endl;
						}
					}
				}
			}
		}
		if (!loadedFromPak) {
			tinygltf::Model model;
			if (!LoadModelFromAsciiFile(model, sceneGltf.c_str())) {
				return false;
			}

			auto& cluster = m_world->LoadCluster(*m_context);
			GltfToCluster(*m_context, model, sceneGltf, cluster, smvk::MeshOptConfig{}, smvk::MeshletConfig{}, m_scene.coordFix, m_scene.preTransform, sceneStreamer);
		}

		// Chrome probe ball (sponza only): procedural UV sphere, 0.5 m in
		// diameter, at the atrium center resting on the floor (y=0). White
		// metal (metallic 1, roughness 0; the lit shader clamps roughness
		// to 0.04 so there are no GGX singularities). Injected into the
		// loaded cluster before Resolve so it joins culling, shadows, AO
		// and reflections like any imported node. Independent root (own
		// identity-parent transform), so scene root transforms can't move
		// it and the static-freeze loop below simply leaves it dynamic.
		if (std::string_view(m_scene.id) == "sponza" && !m_world->clusters.empty()) {
			smvk::Cluster& sponzaCluster = *m_world->clusters.back();
			const float ballRadius = 0.25f;
			const glm::vec3 ballCenter(0.0f, ballRadius, 0.0f);
			smvk::StaticMeshUpload ballUpload = BuildChromeBallUpload(ballRadius, 48, 32);
			smvk::ClusterStaticMeshHandle ballMesh = sponzaCluster.UploadStaticMesh(ballUpload);
			smvk::MaterialCreateInfo chromeCi{};
			chromeCi.baseColorFactor = glm::vec4(1.0f);
			chromeCi.metallicFactor = 1.0f;
			chromeCi.roughnessFactor = 0.0f;
			chromeCi.emissiveFactor = glm::vec3(0.0f);
			chromeCi.alphaMode = smvk::AlphaMode::Opaque;
			chromeCi.shadingModel = smvk::ShadingModel::DefaultLit;
			smvk::ClusterMaterialHandle chromeMat = sponzaCluster.CreateMaterial(chromeCi);
			if (ballMesh.empty() || chromeMat.empty()) {
				std::cerr << "[Scene] chrome ball inject failed (mesh/material)\n";
			}
			else {
				const int ballNode = addNode(sponzaCluster.scene, -1, 0);
				sponzaCluster.scene.localTransform[ballNode] =
					glm::translate(glm::mat4(1.0f), ballCenter);
				sponzaCluster.scene.globalTransform[ballNode] =
					glm::translate(glm::mat4(1.0f), ballCenter);
				sponzaCluster.scene.meshForNode[ballNode] = ballMesh.index();
				sponzaCluster.scene.materialForNode[ballNode] = chromeMat.index();
				setNodeName(sponzaCluster.scene, ballNode, "chrome_ball");
				markAsChanged(sponzaCluster.scene, ballNode);
				std::cerr << "[Scene] chrome ball: r=" << ballRadius
					<< "m at (" << ballCenter.x << "," << ballCenter.y << "," << ballCenter.z << "), "
					<< ballUpload.vertices.size() << " verts, "
					<< ballUpload.indices.size() / 3 << " tris\n";
			}
		}

		// Freeze everything static (no recook needed): the exporters stamp
		// no STATIC_ marks, so cooked paks carry staticRoots=0 and the
		// World's static BVH would stay empty. These scenes never animate,
		// so mark each cluster root static here; Resolve carries the spec
		// into the world scene and RebuildStaticBvh picks it up. Future
		// dynamic nodes must ClearSubtreeStatic() before Resolve to opt out.
		for (auto& clusterPtr : m_world->clusters) {
			if (clusterPtr && !clusterPtr->scene.hierarchy.empty())
				MarkSubtreeStatic(clusterPtr->scene, 0);
		}

		m_world->ResolveAndFixup_clusters();
		std::cerr << "[Scene] world resolved: nodes="
			<< m_world->scene.hierarchy.size() << " meshes="
			<< m_world->meshLocations.size() << " materials="
			<< m_world->materialLocations.size() << std::endl;

		// Fit the procedural Forward+ light grid to the loaded scene's
		// world bounds so the crowd fills Bistro/Sponza instead of
		// clustering around the origin.
		{
			const ::Aabb bounds = m_world->ComputeSceneBounds();
			if (bounds.mx.x >= bounds.mn.x && bounds.mx.y >= bounds.mn.y && bounds.mx.z >= bounds.mn.z) {
				m_fpDemo.FitToBounds(bounds.mn, bounds.mx);
				std::cerr << "[Scene] bounds min=(" << bounds.mn.x << "," << bounds.mn.y << "," << bounds.mn.z
					<< ") max=(" << bounds.mx.x << "," << bounds.mx.y << "," << bounds.mx.z << ")\n";
			}
		}

		// Hero decal splats for the check scene (floor pools + a wall
		// smear in the default camera view; see SpawnDecalPresets).
		SpawnDecalPresets();

		// GeometryFX-style per-view filter (clustered): register static
		// meshes from cooked meshlets (pak path) or retained uploads
		// (procedural path), then bake the cluster-ordered index heap.
		// Non-fatal: unregistered meshes keep rendering via direct draws.
		// The pass config gates the whole filter (sample-level); the member
		// toggle stays as the runtime A/B switch.
		if (m_passes.gfxFilter && m_useGfxFilter) {
			m_gfxRenderer.ConfigureFrag(m_resources.fragShader);
			// Main-view seed masked phase is reversed-Z: prefer the
			// conservative shadow_rev module (falls back to the normal-Z
			// shadow.frag module â€” perf hint, not correctness).
			m_gfxRenderer.ConfigureSeedFrag(!m_resources.shadowMaskedFragRevShader.empty()
				? m_resources.shadowMaskedFragRevShader
				: m_resources.shadowMaskedFragShader);
			// Shadow-atlas seed masked phase is normal-Z (clear 1.0,
			// LessEqual): the plain shadow.frag module.
			m_gfxRenderer.ConfigureShadowSeedFrag(m_resources.shadowMaskedFragShader);
			// A2C filtered program for Masked ranges in 2x MSAA Main.
			if (!m_resources.fragA2CShader.empty())
				m_gfxRenderer.ConfigureA2CFrag(m_resources.fragA2CShader);
			// Wireframe twin of the filtered program (flat lines in P).
			if (!m_resources.fragWireShader.empty())
				m_gfxRenderer.ConfigureWireFrag(m_resources.fragWireShader);
			if (!m_gfxRenderer.Ensure(*m_context, *m_rendergraph)) {
				std::cerr << "[SceneRenderer] GfxFilter unavailable - direct draws\n";
				m_useGfxFilter = false;
			}
		else {
			m_gfxRenderer.RegisterWorldMeshes(*m_world);
			m_gfxRenderer.UploadStaticData(*m_context, *m_rendergraph);
			std::cerr << "[Scene] gfx clusters="
				<< smvk::gfx::ClusterCount(m_gfxRenderer.State()) << std::endl;
			if (smvk::gfx::ClusterCount(m_gfxRenderer.State()) == 0) {
				std::cerr << "[SceneRenderer] GfxFilter: no clusters - direct draws\n";
				m_useGfxFilter = false;
			}
		}
		}
		// Forward+ frame renderer (owns sizing config; references the
		// viewport builder for storage so main-pass sampling is unchanged).
		m_fpRenderer.Configure(&m_viewportResources.forwardPlus,
			MakeForwardPlusDesc(), m_resources.prepassMaskedProgram);

		// Light cookies: baked .ktx2 files from the engine resources mount
		// (overridable by app content / paks through the overlay; falls
		// back to procedural generation when the files are missing).
		m_cookies.Ensure(*m_context, contentVfs);
		// SG-GI baked probe volume (tools/gi_baker output): replaces the
		// analytic sky IBL ambient with path-traced probes when present.
		// Missing file is the normal state (no bake yet) - non-fatal.
		LoadGiVolume(contentVfs);
		// SG 2D lightmaps (cooked with PakCooker --lightmap): uploads the
		// atlas + chunk descriptors only (UVs/chunk ids live in the pak).
		// Missing file is the normal state (no cook yet) - non-fatal.
		LoadGiLightmaps(contentVfs);
		// SG regular 3D probe volumes (DYNAMIC objects): artist OBB
		// grids from tools/gi_baker --regular-out. Missing manifest is
		// the normal state (no bake yet) - non-fatal.
		LoadGiRegularVolumes(contentVfs);
		// Single-source isolation modes degrade when their source did not
		// load (All/SkyIbl need nothing): keeps them from being silently
		// empty — the UI/env selector keeps working after.
		DegradeAmbientForMissingSources();
		// SMAA precomputed LUTs (official area/search tables, embedded in
		// the engine - see Engine/Rendergraph/smaa.h). Uploaded once here so
		// the staged uploads join the cookie flush below.
		if (!m_viewportResources.smaa.EnsureLuts(*m_context))
			std::cerr << "[SceneRenderer] SMAA LUT upload failed - SMAA unavailable\n";
		m_context->FlushUploads();
		// Artistic LUTs: seed the neutral strip + list editable strips.
		ScanLutFiles();
		ExportNeutralLut();


		// FRAME RESOURCES
		for (int i = 0; i < m_swapchain->GetImageCount(); i++) {
			auto logicalSwapchainImage = m_rendergraph->AddExternalImageView(
				m_swapchain->GetImageViewHandle(i),
				m_swapchain->GetSurfaceFormat(),
				"swap"
			);
			m_viewportResources.swapImages.push_back(logicalSwapchainImage);
		}
		// Scene depth is the app-owned DepthTarget at the internal render
		// resolution (see RenderFrame); the swapchain-owned depth is unused.

		// Directional cascade atlas + default shadow program (builder owns Ensure).
		if (!m_viewportResources.directionalShadows.Ensure(*m_context, *m_rendergraph, MakeShadowDesc())) {
			std::cerr << "[SceneRenderer] Failed to create cascaded shadow atlas\n";
			return false;
		}
		// Spot-shadow atlas (fixed kMaxSpotShadows slots; selection caps usage).
		// Non-fatal: without it all spots simply run unshadowed.
		if (!m_viewportResources.spotShadows.Ensure(*m_context, *m_rendergraph, MakeSpotShadowDesc())) {
			std::cerr << "[SceneRenderer] Spot shadow atlas unavailable â€“ spots run unshadowed\n";
		}
		// EVSM moment atlases (RG32F 2xMSAA + async blur). Non-fatal: when
		// the device lacks RG32F render/blur support (or shaders fail),
		// EvsmActive() stays false and every consumer keeps the PCF path.
		if (!m_context->SupportsEvsm()) {
			std::cerr << "[SceneRenderer] EVSM unsupported (no RG32F 2xMSAA moments) â€“ PCF shadows\n";
		}
		if (!m_viewportResources.evsmDir.Ensure(*m_context, *m_rendergraph, MakeEvsmDirDesc(m_evsmPipeline))) {
			std::cerr << "[SceneRenderer] EVSM directional atlas unavailable â€“ PCF fallback\n";
		}
		if (!m_viewportResources.evsmSpot.Ensure(*m_context, *m_rendergraph, MakeEvsmSpotDesc(m_evsmPipeline))) {
			std::cerr << "[SceneRenderer] EVSM spot atlas unavailable â€“ PCF fallback\n";
		}
		if (EvsmActive()) {
			std::cerr << "[EVSM] moment atlases ready: RG32F 2xMSAA + async blur ("
				<< (m_evsmPipeline == smvk::EvsmPipelineMode::SameFrame ? "SameFrame" : "OneFrameLag")
				<< (m_context->HasDedicatedComputeQueue() ? ", true-async queue" : ", emulated (no dedicated queue)")
				<< ")\n";
		} else if (m_evsmEnabled) {
			std::cerr << "[EVSM] inactive (unsupported or atlas failure) â€“ PCF shadows\n";
		}
		if (!m_viewportResources.tonemap.Ensure(*m_context, MakeTonemapDesc())) {
			std::cerr << "[SceneRenderer] Failed to create tonemap program\n";
			return false;
		}
		// Histogram meter for auto-exposure (extent-independent buffer +
		// compute program). Non-fatal: without it the frame falls back to
		// manual exposure.
		if (!m_viewportResources.exposure.Ensure(*m_context, *m_rendergraph, MakeExposureDesc())) {
			std::cerr << "[SceneRenderer] Exposure meter unavailable â€“ manual exposure only\n";
		}
		// Skydome program (fullscreen-triangle sky.vert/sky.frag). Failure is
		// non-fatal: the Main pass simply skips the sky draw and keeps the
		// clear color.
		if (!m_skydome.Ensure(*m_context)) {
			std::cerr << "[SceneRenderer] Skydome program unavailable â€“ running without sky\n";
		}

		{
			auto vkExt = m_swapchain->GetExtent();
			const smvk::Extent2D swapExtent{ vkExt.width, vkExt.height };
			const smvk::Extent2D renderExtent = ScaledExtent(swapExtent, m_renderScale);
			const smvk::Extent2D uiExtent = ScaledExtent(swapExtent, m_uiScale);
			smvk::DepthDesc depthDesc{};
			depthDesc.format = m_swapchain->GetDepthFormat();
			if (!m_viewportResources.renderDepth.Ensure(*m_context, *m_rendergraph, renderExtent, depthDesc)) {
				std::cerr << "[SceneRenderer] Failed to create render depth target\n";
				return false;
			}
			// Option A occluder depth (HZB source): same extent/format as
			// the full depth, own image so the two prepasses don't alias.
			smvk::DepthDesc hizDepthDesc{};
			hizDepthDesc.format = m_swapchain->GetDepthFormat();
			hizDepthDesc.name = "hizDepth";
			hizDepthDesc.viewName = "hizDepthView";
			hizDepthDesc.logicalName = "hizDepthLogical";
			if (!m_viewportResources.hizDepth.Ensure(*m_context, *m_rendergraph, renderExtent, hizDepthDesc)) {
				std::cerr << "[SceneRenderer] Failed to create HZB occluder depth target\n";
				return false;
			}
			if (!m_viewportResources.forwardPlus.Ensure(*m_context, *m_rendergraph,
				renderExtent, MakeForwardPlusDesc(), m_viewportResources.renderDepth.view)) {
				std::cerr << "[SceneRenderer] Failed to create Forward+ tile buffers\n";
				return false;
			}
			if (!m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, renderExtent)) {
				std::cerr << "[SceneRenderer] Failed to create HDR target\n";
				return false;
			}
			{
				smvk::MsaaDesc msaaDesc{};
				msaaDesc.depthFormat = m_swapchain->GetDepthFormat();
				if (!m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, renderExtent, msaaDesc)) {
					std::cerr << "[SceneRenderer] MSAA target unavailable â€“ falling back to 1x\n";
				}
			}
		if (!m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc())) {
				std::cerr << "[SceneRenderer] Bloom chain unavailable (HDR "
				<< renderExtent.width << "x" << renderExtent.height << ")\n";
		}
		{
			smvk::DofDesc dofDesc = MakeDofDesc();
			dofDesc.hqBokeh = m_dofHqBokeh;
			if (!m_viewportResources.dof.Ensure(*m_context, *m_rendergraph, renderExtent, dofDesc)) {
				std::cerr << "[SceneRenderer] DOF targets unavailable (HDR "
					<< renderExtent.width << "x" << renderExtent.height << ")\n";
			}
		}
		if (!m_viewportResources.ssr.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsrDesc())) {
			std::cerr << "[SceneRenderer] SSR target unavailable (HDR "
				<< renderExtent.width << "x" << renderExtent.height << ")\n";
		}
		if (!m_viewportResources.volumetric.Ensure(*m_context, *m_rendergraph, renderExtent, MakeVolumetricDesc())) {
			std::cerr << "[SceneRenderer] Volumetric target unavailable (HDR "
				<< renderExtent.width << "x" << renderExtent.height << ")\n";
		}
		smvk::UiDesc uiDesc{};
		uiDesc.format = LdrUnormSibling();
		if (!m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, uiDesc)) {
			std::cerr << "[SceneRenderer] UI target unavailable â€“ UI disabled\n";
		}
			if (!m_viewportResources.uiComposite.Ensure(*m_context)) {
				std::cerr << "[SceneRenderer] UI composite program unavailable\n";
			}
		}

		// ImGui overlay â€“ init after swapchain/rendergraph are ready.
		// Uses dynamic rendering; no external RenderPass needed.
		m_imgui = std::make_unique<smvk::ImGuiOverlay>();
		smvk::ImGuiOverlayDesc imguiDesc{};
		imguiDesc.fontPath = UI_FONT_PATH;
		imguiDesc.fontBaseSize = UI_FONT_BASE_SIZE;
		imguiDesc.colorFormat = LdrUnormSibling(); // must match the UI target above
		if (!m_imgui->Init(*m_context, *m_swapchain, window, imguiDesc)) {
			std::cerr << "[SceneRenderer] Failed to init ImGuiOverlay â€“ continuing without UI\n";
			m_imgui.reset();
		} else {
			m_uiDpiScale = m_imgui->AppliedDpiScale(); // auto = monitor content scale
			SetUIMode(false); // start in FPS (DISABLED), I toggles to UI (NORMAL)
			// Feed every SDL event to the ImGui SDL3 backend (App::PollEvents).
			imguiEventHook_ = &smvk::ImGuiOverlay::ProcessEvent;
		}

		return true;
	};

void SceneRenderer::Shutdown() {
		// Ensure GPU idle before destroying ImGui Vulkan resources/Holders.
		WaitForDeviceIdle();
		// GeometryFX filter state: frees rendergraph logicals AND the
		// static-index-monoblock virtual regions (cluster + filtered ring).
		// Must run while the rendergraph and the static heap are alive;
		// otherwise the regions leak and VMA asserts at heap teardown.
		m_gfxRenderer.Destroy(*m_rendergraph);
		m_viewportResources.forwardPlus.Destroy(*m_rendergraph);
		m_viewportResources.exposure.Destroy(*m_rendergraph);
		m_viewportResources.exposure.DestroyShaders();
		m_viewportResources.bloom.Destroy(*m_rendergraph);
		m_viewportResources.spotShadows.Destroy(*m_rendergraph);
		m_viewportResources.directionalShadows.Destroy(*m_rendergraph);
		m_viewportResources.evsmDir.Destroy(*m_rendergraph);
		m_viewportResources.evsmSpot.Destroy(*m_rendergraph);
		m_viewportResources.msaa.Destroy(*m_rendergraph);
		m_viewportResources.smaa.Destroy(*m_rendergraph);
	m_viewportResources.hdr.Destroy(*m_rendergraph);
	m_viewportResources.motionBlur.Destroy(*m_rendergraph);
	m_viewportResources.dof.Destroy(*m_rendergraph);
	m_viewportResources.ssr.Destroy(*m_rendergraph);
	m_viewportResources.volumetric.Destroy(*m_rendergraph);
		m_viewportResources.motionVectors.Destroy(*m_rendergraph);
		m_viewportResources.renderDepth.Destroy(*m_rendergraph);
		m_viewportResources.hizDepth.Destroy(*m_rendergraph);
		m_viewportResources.ui.Destroy(*m_rendergraph);
		m_viewportResources.uiComposite.DestroyShaders();
		if (!m_captureLogical.empty()) {
			m_rendergraph->FreeImageView(m_captureLogical);
			m_captureLogical = {};
		}
		m_captureView = {};
		m_captureImage = {};
		m_lutTexture = {};
		m_lutBindless = UINT32_MAX;
		if (m_imgui) {
			m_imgui->Shutdown();
			m_imgui.reset();
		}
		Super::Shutdown();
	}

	// ---------------------------------------------------------------------------
	// SG-GI baked probe volume (tools/gi_baker output, see smvk::gi).
	// Loads content/gi/<scene>.sggi through the app VFS, packs the GPU
	// payload and uploads it once; per-frame state is only the BDAs + the
	// user toggles (FrameUniforms.gi*). A missing file is the normal
	// "not baked yet" state - the renderer keeps the analytic sky IBL.
	// ---------------------------------------------------------------------------
	void SceneRenderer::LoadGiVolume(smvk::OverlayVfs& vfs) {
		m_giLoaded = false;
		m_giVolume = smvk::gi::SgGiVolume{};
		m_giProbesBuffer = {};
		m_giGridBuffer = {};
		m_giCellsBuffer = {};
		m_giIndicesBuffer = {};
		m_giProbesAddress = m_giGridAddress = m_giCellsAddress = m_giIndicesAddress = 0;

		const std::string vfsPath = std::string("gi/") + m_scene.id + ".sggi";
		// GI-less scenes (e.g. scanlands): the flag gates keep the ambient
		// honest (probes simply never enable); DegradeAmbientForMissing
		// Sources() after the loaders fixes single-source modes. This
		// lambda only logs.
		auto fallbackSky = [&] {
			std::cerr << "[GI] ambient keeps its mode; per-source flags gate\n";
		};
		std::vector<uint8_t> bytes;
		try {
			bytes = vfs.ReadBinary(vfsPath);
		} catch (...) {
			std::cerr << "[GI] no baked volume '" << vfsPath
				<< "' - sky IBL ambient (bake with tools/gi_baker/GiBaker)\n";
			fallbackSky();
			return;
		}
		std::string err;
		if (!m_giVolume.ReadFromMemory(bytes, err)) {
			std::cerr << "[GI] volume rejected: " << err << "\n";
			fallbackSky();
			return;
		}

		std::vector<smvk::gi::SgProbeGpu> probeData;
		smvk::gi::SgGiGridGpu gridData{};
		std::vector<uint32_t> cellData;
		std::vector<uint32_t> indexData;
		m_giVolume.PackGpu(probeData, gridData, cellData, indexData);
		if (probeData.empty()) {
			std::cerr << "[GI] volume has no probes - ignored\n";
			fallbackSky();
			return;
		}

		constexpr smvk::PipelineStage kGiStages =
			smvk::PipelineStage(smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment);
		m_giProbesBuffer = m_context->CreateBuffer(
			probeData.size() * sizeof(smvk::gi::SgProbeGpu),
			smvk::BufferType_Storage, "sggi probes");
		m_giGridBuffer = m_context->CreateBuffer(
			sizeof(smvk::gi::SgGiGridGpu), smvk::BufferType_Storage, "sggi grid");
		m_giCellsBuffer = m_context->CreateBuffer(
			std::max<size_t>(cellData.size(), 1) * sizeof(uint32_t),
			smvk::BufferType_Storage, "sggi cells");
		m_giIndicesBuffer = m_context->CreateBuffer(
			std::max<size_t>(indexData.size(), 1) * sizeof(uint32_t),
			smvk::BufferType_Storage, "sggi indices");

		m_context->UploadDataToBuffer(m_giProbesBuffer, probeData.data(),
			probeData.size() * sizeof(smvk::gi::SgProbeGpu), kGiStages,
			smvk::AccessFlags::ShaderRead);
		m_context->UploadDataToBuffer(m_giGridBuffer, &gridData,
			sizeof(gridData), kGiStages, smvk::AccessFlags::ShaderRead);
		if (!cellData.empty())
			m_context->UploadDataToBuffer(m_giCellsBuffer, cellData.data(),
				cellData.size() * sizeof(uint32_t), kGiStages,
				smvk::AccessFlags::ShaderRead);
		if (!indexData.empty())
			m_context->UploadDataToBuffer(m_giIndicesBuffer, indexData.data(),
				indexData.size() * sizeof(uint32_t), kGiStages,
				smvk::AccessFlags::ShaderRead);

		m_giProbesAddress = m_context->GetBufferDeviceAddress(m_giProbesBuffer);
		m_giGridAddress = m_context->GetBufferDeviceAddress(m_giGridBuffer);
		m_giCellsAddress = m_context->GetBufferDeviceAddress(m_giCellsBuffer);
		m_giIndicesAddress = m_context->GetBufferDeviceAddress(m_giIndicesBuffer);
		m_giLoaded = true;

		// CPU-side sanity: the volume must cover the scene interior.
		if (const char* giDbg = std::getenv("VULKANTOY_GI_DEBUG")) {
			(void)giDbg;
			// World mesh AABB from the RESOLVED runtime scene vs the volume
			// bounds: any transform/axis mismatch between the bake source
			// and the app world shows up here as diverging bounds.
			glm::vec3 worldMin(1e30f), worldMax(-1e30f);
			for (const auto& [node, meshIdx] : m_world->scene.meshForNode) {
				const smvk::StaticMesh* mesh = m_world->GetMesh(meshIdx);
				if (!mesh)
					continue;
				const glm::mat4& g = m_world->scene.globalTransform[node];
				const glm::vec3 mn = glm::vec3(g * glm::vec4(mesh->geometry.aabbMin, 1.0f));
				const glm::vec3 mx = glm::vec3(g * glm::vec4(mesh->geometry.aabbMax, 1.0f));
				worldMin = glm::min(worldMin, mn);
				worldMax = glm::max(worldMax, mx);
			}
			std::cerr << "[GI] dbg: volume bounds (" << m_giVolume.boundsMin.x
				<< "," << m_giVolume.boundsMin.y << "," << m_giVolume.boundsMin.z
				<< ")..(" << m_giVolume.boundsMax.x << "," << m_giVolume.boundsMax.y
				<< "," << m_giVolume.boundsMax.z << ")\n";
			std::cerr << "[GI] dbg: world   bounds (" << worldMin.x << "," << worldMin.y
				<< "," << worldMin.z << ")..(" << worldMax.x << "," << worldMax.y
				<< "," << worldMax.z << ")\n";
			const glm::vec3 center = (m_giVolume.boundsMin + m_giVolume.boundsMax) * 0.5f;
			glm::vec3 amps[smvk::gi::kSgLobeCount];
			const bool covered = m_giVolume.Sample(center, amps);
			const bool camCovered = m_giVolume.Sample(
				m_cameraPositioner.cameraPosition_, amps);
			std::cerr << "[GI] dbg: volume-center covered=" << (covered ? 1 : 0)
				<< " amp0=(" << amps[0].r << "," << amps[0].g << "," << amps[0].b << ")"
				<< "  camera covered=" << (camCovered ? 1 : 0) << "\n";
			for (uint32_t lvl = 0; lvl < m_giVolume.levelCount; ++lvl) {
				std::cerr << "[GI] dbg: level " << lvl
					<< " cellSize=" << m_giVolume.cellSize[lvl];
				if (lvl < m_giVolume.levelStats.size())
					std::cerr << " probes=" << m_giVolume.levelStats[lvl].probeCount
						<< " cells=" << m_giVolume.levelStats[lvl].cellCount;
				std::cerr << "\n";
			}
		}

		std::cerr << "[GI] loaded '" << vfsPath << "': probes="
			<< m_giVolume.probes.size() << " levels=" << m_giVolume.levelCount
			<< " gpu=" << (m_giVolume.GpuBytes() / 1024) << " KiB\n";
	}

	// ---------------------------------------------------------------------------
	// SG 2D lightmaps (STATIC geometry; cooked with PakCooker --lightmap).
	// Loads content/gi/<scene>.sglm and uploads ONLY the 9 RGBA16F atlas
	// layers. The atlas-space UVs + per-mesh lobe counts are COOKED into
	// the pak (texCoord2 / StaticMesh::lmChunk riding the draw ring) —
	// no chunk buffer exists at runtime. Missing file = "not baked yet"
	// - non-fatal.
	// ---------------------------------------------------------------------------
	struct RegObbGpu {
		float center[4];
		float axisX[4];
		float axisY[4];
		float axisZ[4];
		float halfExtents[4];
		uint32_t dims[4];
	};
	static_assert(sizeof(RegObbGpu) == 96);

	void SceneRenderer::LoadGiLightmaps(smvk::OverlayVfs& vfs) {
		// Cook-owned lightmaps: the pak carries the per-vertex atlas-space
		// UVs (texCoord2) + each mesh's lobe count (riding the draw ring);
		// this loader only uploads the atlas (9 RGBA16F layers). NO
		// geometry work, NO buffers beyond the textures happen here.
		m_lmLoaded = false;
		m_lmAtlas = smvk::gi::SgLightmapAtlas{};
		for (auto& layer : m_lmLayers)
			layer = {};
		for (uint32_t& slot : m_lmLayerSlots)
			slot = UINT32_MAX;

		const std::string vfsPath = std::string("gi/") + m_scene.id + ".sglm";
		std::vector<uint8_t> bytes;
		try {
			bytes = vfs.ReadBinary(vfsPath);
		} catch (...) {
			std::cerr << "[LM] no lightmap '" << vfsPath
				<< "' (cook with PakCooker --lightmap on)\n";
			return;
		}
		std::string err;
		if (!m_lmAtlas.ReadFromMemory(bytes, err)) {
			std::cerr << "[LM] atlas rejected: " << err << "\n";
			return;
		}
		if (m_lmAtlas.chunks.empty()) {
			std::cerr << "[LM] atlas has no chunks - ignored\n";
			return;
		}
		if (m_lmAtlas.width == 0 || m_lmAtlas.height == 0
			|| m_lmAtlas.width > 8192 || m_lmAtlas.height > 8192) {
			std::cerr << "[LM] atlas dims rejected: " << m_lmAtlas.width
				<< "x" << m_lmAtlas.height << "\n";
			return;
		}

		// ---- Atlas layers (9 RGBA16F bindless textures) ----
		std::vector<uint16_t> layers[smvk::gi::kLightmapLayerCount];
		m_lmAtlas.PackGpuLayers(layers);
		for (uint32_t t = 0; t < smvk::gi::kLightmapLayerCount; ++t) {
			smvk::ImageCreateInfo ci{};
			ci.extent = { m_lmAtlas.width, m_lmAtlas.height, 1 };
			ci.format = smvk::Format::RGBA16_Float;
			ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
			ci.mipLevels = 1;
			ci.arrayLayers = 1;
			ci.name = "sglm atlas layer";
			auto tex = m_context->CreateTexture(ci, layers[t].data());
			if (tex.empty()) {
				std::cerr << "[LM] atlas layer " << t << " creation failed\n";
				for (uint32_t& slot : m_lmLayerSlots)
					slot = UINT32_MAX;
				return;
			}
			m_lmLayers[t] = std::move(tex);
			m_lmLayerSlots[t] = m_context->GetTextureBindlessIndex(m_lmLayers[t]);
		}
		m_lmLoaded = true;
		std::cerr << "[LM] loaded '" << vfsPath << "': chunks="
			<< m_lmAtlas.chunks.size() << " atlas=" << m_lmAtlas.width << "x"
			<< m_lmAtlas.height
			<< " gpu=" << (m_lmAtlas.GpuBytes() / 1024) << " KiB\n";
	}
	// ---------------------------------------------------------------------------
	// SG regular 3D probe volumes (DYNAMIC objects; tools/gi_baker
	// --regular-out). Loads content/gi/<scene>.regmanifest: one .sgrv vfs
	// path per line, up to SG_REG_MAX_VOLUMES (8). Each volume uploads 7
	// RGBA16F 3D textures + one OBB descriptor; the shader hardware-
	// trilinearly filters between neighboring probes. Missing manifest =
	// "not baked yet" - non-fatal.
	// ---------------------------------------------------------------------------
	void SceneRenderer::LoadGiRegularVolumes(smvk::OverlayVfs& vfs) {
		m_regLoaded = false;
		m_regEnabled = false;
		m_regVolumes.clear();
		m_regObbsBuffer = {};
		m_regObbsAddress = 0;

		const std::string manifestPath = std::string("gi/") + m_scene.id + ".regmanifest";
		std::vector<uint8_t> manifestBytes;
		try {
			manifestBytes = vfs.ReadBinary(manifestPath);
		} catch (...) {
			std::cerr << "[REG] no volume manifest '" << manifestPath
				<< "' (bake: GiBaker --regular-out)\n";
			return;
		}
		// Line-separated .sgrv vfs paths.
		std::vector<std::string> lines;
		{
			std::string cur;
			for (char c : manifestBytes) {
				if (c == '\n' || c == '\r') {
					if (!cur.empty()) {
						lines.push_back(cur);
						cur.clear();
					}
				} else {
					cur.push_back(c);
				}
			}
			if (!cur.empty())
				lines.push_back(cur);
		}
		constexpr uint32_t kMaxVolumes = 8; // SG_REG_MAX_VOLUMES
		for (const std::string& line : lines) {
			if (m_regVolumes.size() >= kMaxVolumes) {
				std::cerr << "[REG] manifest lists more than " << kMaxVolumes
					<< " volumes - extra entries ignored\n";
				break;
			}
			std::vector<uint8_t> volBytes;
			try {
				volBytes = vfs.ReadBinary(line);
			} catch (...) {
				std::cerr << "[REG] missing volume '" << line << "' - skipped\n";
				continue;
			}
			RegVolume rv{};
			std::string verr;
			if (!rv.volume.ReadFromMemory(volBytes, verr)) {
				std::cerr << "[REG] volume rejected: " << verr << "\n";
				continue;
			}
			// 7 RGBA16F 3D textures (bindless; volumeTextures mirror).
			std::vector<uint16_t> layers[7];
			rv.volume.PackGpuLayers(layers);
			bool ok = true;
			for (uint32_t t = 0; t < 7; ++t) {
				smvk::ImageCreateInfo ci{};
				ci.type = smvk::ImageType::Image3D;
				ci.extent = { rv.volume.obb.dims[0], rv.volume.obb.dims[1],
					rv.volume.obb.dims[2] };
				ci.format = smvk::Format::RGBA16_Float;
				ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
				ci.mipLevels = 1;
				ci.arrayLayers = 1;
				ci.name = "sgrv layer";
				auto tex = m_context->CreateTexture(ci, layers[t].data());
				if (tex.empty()) {
					std::cerr << "[REG] layer " << t << " creation failed\n";
					ok = false;
					break;
				}
				rv.layers[t] = std::move(tex);
				rv.layerSlots[t] = m_context->GetTextureBindlessIndex(rv.layers[t]);
			}
			if (!ok)
				continue;
			// Coefficients are now resident on the GPU; drop the CPU copy.
			rv.volume.coeffs.clear();
			rv.volume.coeffs.shrink_to_fit();
			m_regVolumes.push_back(std::move(rv));
		}
		if (m_regVolumes.empty()) {
			std::cerr << "[REG] no volumes loaded\n";
			return;
		}

		// OBB descriptor buffer (float4-padded layout, see RegObbGpu).
		std::vector<RegObbGpu> obbs(m_regVolumes.size());
		for (size_t v = 0; v < m_regVolumes.size(); ++v) {
			const smvk::gi::SgRegularObb& o = m_regVolumes[v].volume.obb;
			RegObbGpu& g = obbs[v];
			g.center[0] = o.center.x; g.center[1] = o.center.y; g.center[2] = o.center.z;
			g.axisX[0] = o.axisX.x; g.axisX[1] = o.axisX.y; g.axisX[2] = o.axisX.z;
			g.axisY[0] = o.axisY.x; g.axisY[1] = o.axisY.y; g.axisY[2] = o.axisY.z;
			g.axisZ[0] = o.axisZ.x; g.axisZ[1] = o.axisZ.y; g.axisZ[2] = o.axisZ.z;
			g.halfExtents[0] = o.halfExtents.x;
			g.halfExtents[1] = o.halfExtents.y;
			g.halfExtents[2] = o.halfExtents.z;
			g.dims[0] = o.dims[0]; g.dims[1] = o.dims[1]; g.dims[2] = o.dims[2];
		}
		constexpr smvk::PipelineStage kRegStages =
			smvk::PipelineStage(smvk::PipelineStageBit_Fragment);
		m_regObbsBuffer = m_context->CreateBuffer(
			obbs.size() * sizeof(RegObbGpu), smvk::BufferType_Storage, "sgrv obbs");
		m_context->UploadDataToBuffer(m_regObbsBuffer, obbs.data(),
			obbs.size() * sizeof(RegObbGpu), kRegStages,
			smvk::AccessFlags::ShaderRead);
		m_regObbsAddress = m_context->GetBufferDeviceAddress(m_regObbsBuffer);

		m_regLoaded = true;
		m_regEnabled = true; // on by default when baked; UI can toggle
		std::cerr << "[REG] loaded " << m_regVolumes.size()
			<< " volume(s) from '" << manifestPath << "'\n";
		for (const auto& rv : m_regVolumes) {
			const auto& o = rv.volume.obb;
			std::cerr << "  dims=" << o.dims[0] << "x" << o.dims[1] << "x" << o.dims[2]
				<< " center=(" << o.center.x << "," << o.center.y << "," << o.center.z << ")"
				<< " half=(" << o.halfExtents.x << "," << o.halfExtents.y
				<< "," << o.halfExtents.z << ")\n";
		}
	}

	void SceneRenderer::DegradeAmbientForMissingSources() {
		// Single-source isolation modes whose source did not load fall
		// back to another baked source (or the sky IBL) instead of being
		// silently empty. All/SkyIbl/Off never need this.
		if (m_ambientMode == AmbientMode::Lightmaps && !m_lmLoaded) {
			m_ambientMode = (m_giLoaded || m_regLoaded)
				? AmbientMode::Probes : AmbientMode::SkyIbl;
			std::cerr << "[Scene] no lightmaps - ambient degraded to "
				<< AmbientModeName(m_ambientMode) << "\n";
		} else if (m_ambientMode == AmbientMode::Probes
			&& !m_giLoaded && !m_regLoaded) {
			m_ambientMode = m_lmLoaded
				? AmbientMode::Lightmaps : AmbientMode::SkyIbl;
			std::cerr << "[Scene] no probes - ambient degraded to "
				<< AmbientModeName(m_ambientMode) << "\n";
		}
	}

	FrameUniforms SceneRenderer::UpdateFrameUniforms(uint32_t currentFrame, smvk::Extent2D renderExtent, smvk::Extent2D swapExtent) {

		const int width_ = renderExtent.width;
		const int height_ = renderExtent.height;

		auto view = m_camera.getViewMatrix();
		auto proj = m_camera.getProjMatrix();
		proj[1][1] *= -1; // viewport fix ditecrly in proj

		// Unjittered snapshot for 2D overlays (ImGui light markers/labels):
		// the UI layer is neither jittered nor temporally resolved, so it
		// must project with the stable matrix (see m_unjitteredViewProj).
		m_unjitteredViewProj = proj * view;
		// Unjittered clip transform for the DLSS constants (SL matrices
		// must not contain the jitter â€” it travels as jitterOffset).
		m_unjitteredProj = proj;

		// SMAA T2x sub-pixel jitter: alternate the sample position between
		// the two diagonal quarter-pixel offsets (official T2x pattern,
		// paired with the subsampleIndices table in smaa.h). Applied to proj
		// so viewProj / invProj / prevViewProj all stay consistent, and
		// quantized to swap-extent (chain/output resolution) texels: the
		// two phases are combined by the resolve, which runs at swapExtent,
		// so the frame-to-frame swing must be exactly 0.5 chain px at any
		// renderScale for the area-table hint to match. In render pixels
		// that is 0.25*renderScale (<=0.5px over the 0.25-2.0 scale range,
		// inside the 1px culling guard); reprojection stays exact regardless
		// because the motion vectors carry the true offset. NDC is y-down
		// after the Y-flip above, so both axes share one sign (the official
		// table assumes bottom-to-top y; the physical corner pair is
		// unchanged).
		m_currJitterUV = glm::vec2(0.0f);
		if (SmaaT2xActive()) {
			// Phase is the global record toggle (snapshot in
			// m_smaaFrameParams), never per-image - see SmaaT2xState.
			const float s = (m_smaaFrameParams.parity == 0) ? 0.25f : -0.25f;
			const float jx = s * 2.0f / float(swapExtent.width);
			const float jy = s * 2.0f / float(swapExtent.height);
			// Clip-space translation x' = x + jx * w (glm column-major
			// [col][row]: add jx * w-row to the x-row, jy to the y-row).
			for (int c = 0; c < 4; ++c) {
				proj[c][0] += jx * proj[c][3];
				proj[c][1] += jy * proj[c][3];
			}
			// UV shift of the sample position = NDC offset / 2.
			m_currJitterUV = glm::vec2(jx, jy) * 0.5f;
		} else if (m_dlssFrameActive) {
			// DLSS Halton jitter (guide 3.7): offsets are quantized to
			// RENDER-resolution pixels (unlike the T2x chain-pixel jitter
			// above) and cycle through m_dlssPhaseCount phases. Same
			// clip-space translation; NDC is y-down after the Y-flip so
			// both axes share the jitter's sign. prevViewProj (below)
			// keeps the previous jittered matrix, so the motion vectors
			// include the jitter (motionVectorsJittered = true).
			const float jx = m_dlssJitter.x * 2.0f / float(renderExtent.width);
			const float jy = m_dlssJitter.y * 2.0f / float(renderExtent.height);
			for (int c = 0; c < 4; ++c) {
				proj[c][0] += jx * proj[c][3];
				proj[c][1] += jy * proj[c][3];
			}
			m_currJitterUV = glm::vec2(jx, jy) * 0.5f;
		}
		// Jitter's contribution to this frame's motion vectors: the pass
		// diffs against the previous record's JITTERED viewProj, so the
		// vectors = trueMotion + (prevJitter - currJitter). Warming up
		// (prev == curr) yields zero, matching the pass' zero-motion
		// fallback. Non-temporal consumers subtract this (motion blur,
		// the MV debug view, SMAA's attenuation magnitude).
		m_jitterDeltaUV = (m_prevViewProjValid ? m_prevJitterUV : m_currJitterUV)
			- m_currJitterUV;

		auto viewProj = proj * view;
		FrameUniforms data{};
		data.cameraPos = m_cameraPositioner.cameraPosition_;

		data.view = view;		data.proj = proj;
		data.viewProj = viewProj;

		data.invView = glm::inverse(view);
		data.invProj = glm::inverse(proj);
		data.invViewProj = glm::inverse(viewProj);

		// Motion vector pass: previous frame's viewProj. First frame falls
		// back to the current one (zero motion instead of garbage).
		data.prevViewProj = m_prevViewProjValid ? m_prevViewProj : viewProj;

		data.time = time_since_start_s;
		data.viewportSize = { width_, height_ };
		data.invViewportSize = { 1.0f / width_, 1.0f / height_ };

		data.nearPlane = default_near_clip;
		data.farPlane = m_scene.farClip;
		// Linear exposure multiplier for the tonemapper (ACES).
		// Sun is 5000 lux (see UpdateLights); 0.001 puts a sun-facing
		// mid-gray (albedo 0.18, NÂ·L=1) at HDR â‰ˆ 0.29 pre-ACES â†’ no clipping.
		// NOTE: FrameUniforms.exposure is currently unread by shaders; the
		// tonemap push constant is the live knob. Kept in sync with the
		// effective exposure (auto-adapted or manual) for future readers.
		data.exposure = FrameExposure();
		// AO consumption (lighting-pass ambient; see ao.hlsli +
		// default_lit.frag.hlsl): the SsaoCompose/GtaoCompose pass already
		// upscaled the AO to full res with depth authority, so the fragment
		// only .Loads it. Mirrors the AO pass gate below so the shader flag
		// and the emitted passes always agree.
		const bool useGtao = m_aoMode == AoMode::Gtao;
		const bool aoWanted = m_passes.ssao && m_aoMode != AoMode::None
			&& m_ssaoIntensity > 0.0f;
		const bool ssaoReady = m_viewportResources.ssao.Valid();
		const bool gtaoReady = m_viewportResources.gtao.Valid();
		const bool aoOn = aoWanted && (useGtao ? gtaoReady : ssaoReady);
		data.aoTextureIndex = aoOn
			? (useGtao ? m_viewportResources.gtao.OutputBindless()
			           : m_viewportResources.ssao.OutputBindless())
			: UINT32_MAX;
		data.aoIntensity = aoOn ? m_ssaoIntensity : 0.0f;
		// Configurable skydome IBL + fog (single source for sky/sun/fog).
		data.env = m_env.ToFrameData();
		// Ambient sources (N cycles All -> Lightmaps -> Probes -> SkyIbl
		// -> Off): per-source isolation — the sky IBL flag enables the
		// analytic sky as the ambient FALLBACK (pixels no baked source
		// covers; the shader composes it after the source branches, so
		// covered pixels take the baked field alone). Direct light /
		// sky dome / shadows are unaffected.
		data.env.fogEndAndFlags.w = IblWanted() ? 1.0f : 0.0f;

		// Deferred shadows: consumer-side BDAs + the frame's on/off flag.
		// DsActive() is the single gate shared with the phase-1 upload and
		// the phase 2-4 dispatch below, so the flag can never advertise
		// lists that were not (re)built this frame.
		if (DsActive()) {
			const smvk::DeferredShadows& ds = m_viewportResources.deferredShadows;
			data.dsTileState = ds.TileStateAddress(*m_context, currentFrame);
			data.dsLightList = ds.LightListAddress(*m_context, currentFrame);
			data.dsVisibility = ds.VisibilityAddress(*m_context, currentFrame);
			data.dsTileCountX = ds.tileCountX;
			data.dsEnabled = 1u;
#if defined(DS_DEBUG_VIZ)
			// Visualizer inputs (paper 5.3/5.4): the phase-1/phase-2
			// TileGroup masks for the light-scoped modes + the panel's
			// mode/light/overlay selection. Gated off with the pipeline
			// so the shader never sees a mode without fresh buffers.
			data.dsTileMasks = ds.TileMasksAddress(*m_context, currentFrame);
			data.dsTileGroupMasks = ds.TileGroupMasksAddress(*m_context, currentFrame);
			data.dsDebugMode = static_cast<uint32_t>(m_dsDebugMode);
			data.dsDebugLight = static_cast<uint32_t>(
				std::clamp(m_dsDebugLight, 0, std::max(0, (int)m_dsLightCount - 1)));
			data.dsDebugFlags = m_dsDebugOverlay ? 1u : 0u;
			data.dsDebugCountCeil = static_cast<uint32_t>(
				std::clamp(m_dsDebugCountCeil, 1, (int)smvk::kDsMaxLightsPerTile));
#endif
		} else {
			data.dsTileState = 0;
			data.dsLightList = 0;
			data.dsVisibility = 0;
			data.dsTileCountX = 0;
			data.dsEnabled = 0u;
#if defined(DS_DEBUG_VIZ)
			data.dsTileMasks = 0;
			data.dsTileGroupMasks = 0;
			data.dsDebugMode = 0;
			data.dsDebugLight = 0;
			data.dsDebugFlags = 0;
			data.dsDebugCountCeil = 32;
#endif
		}

		// GI debug view + shared intensity ride EVERY frame regardless of
		// which source is active: the lit shader's F8/I debug block colors
		// lightmap / grid / probe pixels alike (mode 1 flat irradiance,
		// 2 source palette), so the views must survive source isolation
		// (e.g. Lightmaps mode with no probe volume loaded).
		data.giIntensity = m_giIntensity;
		data.giDebug = uint32_t(std::clamp(m_giDebug, 0, 2)) & 0xffu;
		// SG-GI probe volume (static BDAs + the per-mode flag). GiActive()
		// is the single gate so the shader flag and the buffers can never
		// disagree.
		if (GiActive()) {
			data.giProbes = m_giProbesAddress;
			data.giGrid = m_giGridAddress;
			data.giCells = m_giCellsAddress;
			data.giIndices = m_giIndicesAddress;
			data.giEnabled = 1u;
		} else {
			data.giProbes = 0;
			data.giGrid = 0;
			data.giCells = 0;
			data.giIndices = 0;
			data.giEnabled = 0u;
		}
		// SG 2D lightmaps (static draws; the atlas-space uv + per-draw lobe
		// count ride the pak's vertex data / draw ring). Static/dynamic
		// separation: lm/reg ride the same giIntensity scale as the probe
		// volume so the N ambient-mode UI stays meaningful.
		if (LmActive()) {
			for (uint32_t t = 0; t < 9; ++t)
				data.lmLayers[t] = m_lmLayerSlots[t];
			data.lmEnabled = 1u;
		} else {
			for (uint32_t t = 0; t < 9; ++t)
				data.lmLayers[t] = UINT32_MAX;
			data.lmEnabled = 0u;
		}
		data.lmPad_[0] = 0;
		data.lmPad_[1] = 0;
		// SG regular 3D probe volumes (dynamic draws inside artist OBBs).
		// RegActive() gates the upload (loaded + UI toggle); the ambient
		// mode additionally gates the enable flag (All/Probes only).
		if (RegActive()) {
			data.regObbs = m_regObbsAddress;
			const uint32_t n = std::min<uint32_t>(
				uint32_t(m_regVolumes.size()), 8u);
			for (uint32_t v = 0; v < n; ++v)
				for (uint32_t t = 0; t < 7; ++t)
					data.regLayers[v * 7u + t] = m_regVolumes[v].layerSlots[t];
			data.regCount = n;
			data.regEnabled = RegWanted() ? 1u : 0u;
		} else {
			data.regObbs = 0;
			for (uint32_t& slot : data.regLayers)
				slot = UINT32_MAX;
			data.regCount = 0u;
			data.regEnabled = 0u;
		}
		data.evsmMode = EvsmActive() ? 1u : 0u;

		// ---- Forward+ decals (tiled projected splats) ----
		// Contract with the cull + shading shaders (see forwardplus.cs.hlsl
		// + default_lit_body.hlsli): decalCount > 0 only when the tile-cull
		// dispatch runs this frame (DecalsActive gates both), so the decal
		// tile lists are always fresh; the tile-list BDAs are filled
		// whenever the builder is valid regardless of the toggle â€” the
		// cull stores per-tile 0 counts when decals are off.
		if (m_viewportResources.forwardPlus.Valid()) {
			data.decalTileCounts =
				m_viewportResources.forwardPlus.DecalTileCountsAddress(*m_context);
			data.decalTileIndices =
				m_viewportResources.forwardPlus.DecalTileIndicesAddress(*m_context);
			data.decalMaxPerTile =
				m_viewportResources.forwardPlus.desc.maxDecalsPerTile;
		}
		if (DecalsActive() && m_decalFrameCount > 0
			&& !m_resources.decalsBuffer.empty()) {
			constexpr size_t DECAL_FRAME_STRIDE =
				sizeof(smvk::ForwardPlusDecal) * FP_MAX_DECALS;
			data.decals = m_context->GetBufferDeviceAddress(m_resources.decalsBuffer)
				+ currentFrame * DECAL_FRAME_STRIDE;
			data.decalCount = std::min(m_decalFrameCount, FP_MAX_DECALS);
		} else {
			data.decals = 0;
			data.decalCount = 0;
		}

		// Destination scope covers every consumer: BDA reads in vertex,
		// fragment (lighting/materials/env) and compute (Forward+ cull).
		static constexpr smvk::PipelineStage kFrameUploadStages =
			smvk::PipelineStage(smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute);
		m_context->UploadDataToBuffer(
			m_resources.frameUniforms,
			&data,
			sizeof(FrameUniforms)/* * 1*/,
			kFrameUploadStages,
			smvk::AccessFlags::ShaderRead,
			0,
			currentFrame * sizeof(FrameUniforms)
		);
		return data;

	}

	smvk::GpuDirectionalLightDescription SceneRenderer::UpdateLights(uint32_t currentFrame)
	{
		// Directional sun driven by the environment (single source of truth
		// for the sky sun disc, IBL sun tint, and shadow-casting light).
		// lightDir points FROM the sun toward the scene (i.e. -toSun).
		glm::vec3 toSun = m_env.SunDirectionToSun();
		glm::vec3 lightDir = -toSun;
		auto cameraView = m_camera.getViewMatrix();
		auto cameraProj = m_camera.getProjMatrix();
		// Keep Y-flip consistent with main view (UpdateFrameUniforms does proj[1][1] *= -1 for Vulkan).
		// Cascade frustum extraction must use the same Y-flipped proj, otherwise shadow tiles are Y-mirrored.
		cameraProj[1][1] *= -1.0f;

		smvk::Light lights[MAX_LIGHTS]{};
		smvk::Light& light = lights[0];
		light.color = glm::vec4(m_env.sunColor, 1.0f);
		light.direction[0] = lightDir.x;
		light.direction[1] = lightDir.y;
		light.direction[2] = lightDir.z;
		light.intensity = m_env.sunIntensity;
		// EVSM: the index addresses the blurred moments (sampled with a
		// 1-frame lag); FrameUniforms.evsmMode tells shaders which
		// representation the index refers to. Otherwise the depth atlas.
		if (EvsmActive() && m_passes.directionalShadows)
			light.shadow_map_index =
				m_viewportResources.evsmDir.BlurredBindlessIndex(*m_context, currentFrame);
		else
			light.shadow_map_index =
				m_context->GetBindlessIndex(
					m_viewportResources.directionalShadows.bindless
				);

		// ---- Cascade builder (engine) ----
		auto invView = glm::inverse(cameraView);
		auto invProj = glm::inverse(cameraProj);
				auto splits = smvk::ComputeCascadeSplits(default_near_clip, m_scene.farClip, CASCADE_LAMBDA, NUM_CASCADES);

		smvk::GpuDirectionalLightDescription dir =
			m_viewportResources.directionalShadows.BuildDirectionalLight(
				lightDir,
				light.color,
				light.intensity,
				light.shadow_map_index,
				invView,
				invProj,
				splits,
				CASCADE_Z_PADDING,
				// Soft directional shadows: the environment's sun disc
				// angular radius becomes the physical penumbra angle.
				std::tan(glm::radians(m_env.sunDiscSizeDeg)),
				m_evsmSoftness);
		// Keep lights[0].lightVP in sync for legacy single-cascade path / debug
		std::memcpy(light.lightVP, dir.lightVP, sizeof(light.lightVP));

		constexpr size_t LIGHT_FRAME_STRIDE = sizeof(smvk::Light) * MAX_LIGHTS;
		size_t frameOffset = currentFrame * LIGHT_FRAME_STRIDE;
		m_context->UploadDataToBuffer(
			m_resources.lightsBuffer,
			lights,
			sizeof(lights),
			smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute,
			smvk::AccessFlags::ShaderRead,
			0,
			frameOffset
		);
		constexpr size_t DIR_STRIDE = sizeof(smvk::GpuDirectionalLightDescription);
		size_t dirOffset = currentFrame * DIR_STRIDE;
		// The sampling descriptors must match the atlas CONTENT being read.
		// SameFrame: lighting reads this frame's blurred moments (baked with
		// this frame's fit) -> current dir is correct. OneFrameLag: lighting
		// reads the PREVIOUS flight slot's blurred moments (BlurredBindlessIndex
		// above trails one slot), baked with LAST frame's fit â€” uploading the
		// current fit makes the receiver projection disagree with the stale
		// map content by one frame of camera motion, and the ESM ratio
		// e^{c*(d_map - d_recv)} turns that into acne on every surface.
		// The moment-pass RenderViews always use the CURRENT fit
		// (AllocateDirectionalLightViews(dirLight)) regardless of mode.
		const bool laggedEvsm = EvsmActive()
			&& m_evsmPipeline == smvk::EvsmPipelineMode::OneFrameLag;
		smvk::GpuDirectionalLightDescription dirSample = dir;
		if (laggedEvsm && m_dirSamplePrevValid)
			dirSample = m_dirSamplePrev; // sample with the fit that BAKED the stale atlas
		m_dirSamplePrev = dir; // trail one frame for the next lagged frame
		m_dirSamplePrevValid = true;
		m_context->UploadDataToBuffer(
			m_resources.directionalCascadeBuffer,
			&dirSample,
			sizeof(dirSample),
			smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute,
			smvk::AccessFlags::ShaderRead,
			0,
			dirOffset
		);
		return dir;
	}

	void SceneRenderer::UpdateMaterials(uint32_t currentFrame) {
		// Preserve world material slots: shaders index by world materialId,
		// so a missing slot must stay a hole (default values), not shift
		// every later material down by one.
		const uint32_t slotCount = (uint32_t)m_world->materialLocations.size();
		if (slotCount > MAX_MATERIALS) {
			std::cerr << "[SceneRenderer] Material slots (" << slotCount
				<< ") exceed MAX_MATERIALS (" << MAX_MATERIALS
				<< ") - clamping upload, trailing materials will use defaults\n";
		}
		const uint32_t uploadCount = std::min(slotCount, MAX_MATERIALS);
		std::vector<smvk::MaterialValues> mats(uploadCount);
		for (uint32_t i = 0; i < uploadCount; ++i) {
			if (auto* mat = m_world->GetMaterial(i))
				mats[i] = mat->values;
		}

		constexpr size_t MATERIAL_FRAME_STRIDE =
			sizeof(smvk::MaterialValues) * MAX_MATERIALS;

		size_t frameOffset =
			currentFrame * MATERIAL_FRAME_STRIDE;

		if (!mats.empty()) {
			m_context->UploadDataToBuffer(
				m_resources.materialsBuffer,
				mats.data(),
				sizeof(smvk::MaterialValues) * mats.size(),
				smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute,
				smvk::AccessFlags::ShaderRead,
				0,
				frameOffset
			);
		}
	}

	// Texture-streaming tick (see Engine/Renderer/texture_streamer.h).
	// Demand: every 4th frame, closest camera distance per bindless texture
	// slot over all renderable instances (instance origin as the distance
	// proxy; approxWorldSize absorbs the error). Budget: every frame, paced
	// retargets; re-uploads join the next FlushUploads below. streamFrame
	// is the monotonic m_frameCounter, never the swapchain slot.
	void SceneRenderer::UpdateTextureStreaming(const FrameUniforms& frameData, smvk::Extent2D renderExtent) {
		(void)frameData;
		if (!m_textureStreamingEnabled || !m_textureStreamer)
			return;
		const uint32_t streamFrame = static_cast<uint32_t>(m_frameCounter);
		if ((m_frameCounter & 3u) == 0u) {
			std::unordered_map<uint32_t, float> minDist;
			minDist.reserve(256);
			const glm::vec3 eye = m_cameraPositioner.cameraPosition_;
			auto note = [&](int slot, float dist) {
				if (slot < 0)
					return;
				const uint32_t s = static_cast<uint32_t>(slot);
				auto it = minDist.find(s);
				if (it == minDist.end() || dist < it->second)
					minDist[s] = dist;
			};
			m_world->forEachRenderItem(
				smvk::IteratorFlagBit_Renderable,
				[&](const smvk::World::RenderItem& item) {
					if (item.material == smvk::World::kInvalidIndex)
						return;
					const smvk::Material* mat = m_world->GetMaterial(item.material);
					if (!mat)
						return;
					const glm::mat4& model = m_world->scene.globalTransform[item.node];
					const float dist = glm::distance(eye, glm::vec3(model[3]));
					note(mat->values.textures.x, dist);
					note(mat->values.textures.y, dist);
					note(mat->values.textures.z, dist);
					note(mat->values.textures.w, dist);
					note(mat->values.textures2.x, dist);
				});
			const float fovY = glm::radians(kCameraFovYDegrees);
			m_textureStreamer->RequestMipsByDistance(minDist,
				static_cast<float>(renderExtent.height), fovY, streamFrame);
		}
		m_textureStreamer->Update(streamFrame);
		const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
		if (stats.upgradesLastUpdate > 0 || stats.downgradesLastUpdate > 0)
			m_context->FlushUploads();
	}

	// Histogram readback switch. ON: the engine download now uses a
	// persistent grow-on-demand staging buffer (no per-call staging
	// create/deferred destroy) and ProcessDeferrredTasks runs ready tasks
	// outside its mutex, so the old re-entrancy hazard (destroy ->
	// DeferDestroy -> AddDeferredTask re-locking the drain's non-recursive
	// m_deferredMutex) is gone.
	static constexpr bool kHistogramReadbackEnabled = true;

	// Pre-record eye adaptation (see exposure.h): downloads the 256-byte
	// histogram for frameIndex BEFORE this frame zeroes it. The download
	// must precede both the zero-fill and the main-submit of this frame:
	// UpdateAutoExposure used to run after Execute (but Execute only
	// records into fb.commandBuffer â€” EndAndPresent submits it later), so
	// the download's copy was submitted BEFORE this frame's histogram
	// compute and could only ever observe the just-zeroed buffer (flat
	// ImGui plot; a stale nonzero flash only when transfer/GPU timing
	// slipped). Reading the current flight slice after Acquire (which
	// retires this slot) yields the completed 2-frames-ago accumulation
	// with no extra stall. Refreshes the debug copy, and damps
	// m_exposureAdapted toward the percentile target. Holds the current
	// exposure when the meter is missing or the histogram is empty.
	void SceneRenderer::UpdateAutoExposure(uint32_t frameIndex) {
		if (!kHistogramReadbackEnabled)
			return; // readback disabled (see above): hold the seed/manual exposure
		if (!ExposureMeteringActive())
			return;
		auto& meter = m_viewportResources.exposure;
		if (!meter.Valid())
			return;
		// Current flight slice, read BEFORE this frame's zero-fill below.
		// Acquire already retired this slot, so its bins are the completed
		// accumulation from two frames ago ï¿½ no cross-flight torn reads and
		// no wait on the still-in-flight previous frame.
		if (frameIndex >= ::MAX_FRAMES_IN_FLIGHT)
			return;
		m_exposureFrame = frameIndex;
		std::array<uint32_t, smvk::kExposureBinCount> bins{};
		if (!m_context->DownloadDataFromBuffer(meter.HistogramBuffer(frameIndex),
				bins.data(), sizeof(uint32_t) * bins.size()))
			return;
		AdaptExposureFromBins(bins);
	}

	// Metering gates shared by the histogram download and the batched
	// feedback pairing (debug views hold the last scene exposure; see the
	// note on UpdateAutoExposure).
	bool SceneRenderer::ExposureMeteringActive() const {
		if (!m_autoExposureEnabled || !m_passes.exposure)
			return false;
		// Debug views fill HDR with exposure-compensated flat colors (see
		// the dbgGain pre-scale in Main and the /exposure in the unlit +
		// cascade + vertex-color shaders): metering them would drive the
		// to minimum and feed back into ever larger gains. Hold the last
		// scene exposure while any debug view overwrites HDR (the held
		// value also keeps the debug gain itself stable). The DS
		// visualizers (debug builds, overlay or flat) tint/replace the
		// lit HDR the same way, so any active mode freezes adaptation at
		// the pre-switch value too; turning the view/mode off resumes
		// adapting from the held exposure.
		if (m_debugView != DebugView::Lit)
			return false;
#if defined(DS_DEBUG_VIZ)
		if (m_dsDebugMode != DsDebugViz::Off)
			return false;
#endif
		return true;
	}

	// Bins -> percentile target + damped adapt (tail of UpdateAutoExposure,
	// shared by the single and batched download paths).
	void SceneRenderer::AdaptExposureFromBins(
		const std::array<uint32_t, smvk::kExposureBinCount>& bins) {
		auto& meter = m_viewportResources.exposure;
		uint32_t total = 0;
		for (size_t i = 0; i < bins.size(); ++i) {
			meter.lastBins[i] = bins[i];
			total += bins[i];
			m_exposureBinsUi[i] = static_cast<float>(bins[i]);
		}
		meter.lastTotal = total;
		m_exposureSamples = total;
		{
			static uint64_t s_expDlTick = 0, s_expZeroTick = 0;
			const uint64_t tick = s_expDlTick++;
			if (tick < 5 || tick % 600 == 0
				|| (total == 0 && s_expZeroTick++ < 30)) {
				std::fprintf(stderr,
					"[exposure] download tick=%llu slice=%u total=%u target=%.6f adapted=%.6f\n",
					(unsigned long long)tick, m_exposureFrame, total,
					m_exposureTarget, m_exposureAdapted);
			}
		}
		if (total == 0)
			return;
		if (const float lum = meter.PercentileLuminance(); lum > 0.0f) {
			float target = meter.desc.keyValue * exp2f(m_exposureEvBias) / lum;
			target = std::clamp(target, m_exposureMin, m_exposureMax);
			m_exposureTarget = target;
		}
		m_exposureAdapted = meter.Adapt(m_exposureAdapted, m_exposureEvBias,
			m_exposureSpeedToDark, m_exposureSpeedToBright,
			m_exposureMin, m_exposureMax, delta_time_s);
	}

	// Async GPU->CPU readback tick (exposure histogram + texture feedback
	// + deferred-shadow counters). SUBMIT + POLL on a timeline semaphore â€”
	// the render thread NEVER waits: the old sync downloads locked for
	// the whole in-flight graphics backlog (Optick showed it as a
	// multi-ms CPU block; the DS counter readback had the same spike
	// every 8th frame before it moved here). Every 4th frame a fresh
	// copy of the retired slice is submitted with a ticket; each frame
	// the previous ticket is POLLED non-blocking and, once the GPU
	// signals it, exposure adapts + feedback forwards + the DS overlay
	// stats refresh from the staging memcpy. One ticket in flight: a
	// tick while the previous is pending skips its submit (results land
	// 1-2 frames later; adaptation is time-damped and streaming is laggy
	// anyway). The feedback clear still runs AFTER the copy is queued
	// (download-before-clear, same order as the exposure zero-fill).
	void SceneRenderer::UpdateReadbacks(uint32_t frameSlot, uint32_t streamFrame) {
		const bool feedbackActive = m_textureStreamingEnabled
			&& m_textureStreamer && m_textureFeedback
			&& m_textureFeedback->GetSlotCount() > 0;
		auto& meter = m_viewportResources.exposure;
		const bool meterValid = meter.Valid()
			&& frameSlot < static_cast<uint32_t>(::MAX_FRAMES_IN_FLIGHT);
	// Layout helper: the SAME span shape must accompany the submit and
	// its poll (destinations are stable members; handles/sizes are fixed).
	std::array<smvk::BufferDownload, 3> dls{};
	const auto buildPendingDownloads = [&]() -> std::span<const smvk::BufferDownload> {
		uint32_t n = 0;
		if (m_pendingReadbackHasExposure) {
			dls[n] = { meter.HistogramBuffer(m_pendingReadbackSlot),
				m_pendingExposureBins.data(),
				m_pendingExposureBins.size() * sizeof(uint32_t), 0 };
			++n;
		}
		if (m_pendingReadbackHasFeedback) {
			dls[n] = { m_context->GetTextureFeedbackBuffer(m_pendingReadbackSlot),
				m_pendingFeedback.data(),
				m_pendingFeedback.size() * sizeof(uint32_t), 0 };
			++n;
		}
		if (m_pendingReadbackHasDs) {
			dls[n] = { m_viewportResources.deferredShadows.CounterBuffer(
					m_pendingReadbackSlot),
				m_pendingDsCounters.data(), m_pendingDsCounters.size()
					* sizeof(uint32_t), 0 };
			++n;
		}
		return { dls.data(), n };
	};

	// 1) Poll the in-flight download (non-blocking timeline check).
	if (m_asyncReadbackTicket != 0) {
		if (m_context->PollAsyncDownloadDataFromBuffers(
				m_asyncReadbackTicket, buildPendingDownloads())) {
			m_asyncReadbackTicket = 0;
			if (m_pendingReadbackHasExposure && ExposureMeteringActive()) {
				m_exposureFrame = m_pendingReadbackSlot;
				AdaptExposureFromBins(m_pendingExposureBins);
			}
			if (m_pendingReadbackHasFeedback && feedbackActive) {
				const uint32_t reported = m_textureFeedback->ForwardDownloaded(
					*m_textureStreamer, m_pendingFeedback.data(),
					m_pendingFeedback.size(), streamFrame);
				if ((m_frameCounter & 63u) == 2u) {
					std::fprintf(stderr,
						"[streamfb] tick=%llu slice=%u reported=%u (async)\n",
						(unsigned long long)m_frameCounter,
						m_pendingReadbackSlot, reported);
				}
			}
			// Deferred-shadow counters (retired slice, ~a few frames old):
			// overlay slots + throttled telemetry. Saturation of the
			// flexible allocations means tiles/lights degrade (clamped
			// stores, shadows read as fully lit).
			if (m_pendingReadbackHasDs) {
				m_dsStats.listSlots = m_pendingDsCounters[0];
				m_dsStats.visSlots = m_pendingDsCounters[1];
				m_dsStats.clsAllLit = m_pendingDsCounters[2];
				m_dsStats.clsAllShadow = m_pendingDsCounters[3];
				m_dsStats.clsPenumbra = m_pendingDsCounters[4];
				m_dsStats.sumEarly = m_pendingDsCounters[5];
				m_dsStats.sumFinal = m_pendingDsCounters[6];
				m_dsStats.dbgSpotCone = m_pendingDsCounters[7];
				m_dsStats.dbgSpotClip = m_pendingDsCounters[8];
				static uint64_t s_dsTick = 0;
				const uint64_t tick = s_dsTick++;
				if (tick < 5 || tick % 600 == 0) {
					std::fprintf(stderr,
						"[ds] list=%u/%u vis=%u/%u early=%u final=%u lit=%u shadow=%u penumbra=%u spotCone=%u spotClip=%u lights=%u\n",
						m_dsStats.listSlots, smvk::kDsMaxListUInts,
						m_dsStats.visSlots, smvk::kDsMaxVisSlots,
						m_dsStats.sumEarly, m_dsStats.sumFinal,
						m_dsStats.clsAllLit, m_dsStats.clsAllShadow, m_dsStats.clsPenumbra,
						m_dsStats.dbgSpotCone, m_dsStats.dbgSpotClip,
						m_dsReadbackLightCount);
				}
			}
		}
	}

	// 2) Submit this tick's download of the retired slice: no wait, the
	// ticket is polled on later frames. Skipped while a ticket is
	// pending (single slot) or on submit failure (retry next tick).
	const bool tick = (m_frameCounter & 3u) == 2u;
	if (tick && m_asyncReadbackTicket == 0) {
		m_pendingReadbackSlot = frameSlot;
		m_pendingReadbackHasExposure = kHistogramReadbackEnabled && meterValid;
		m_pendingReadbackHasFeedback = feedbackActive;
		// DS counters (40 B) ride the same batch (no extra submit): the
		// slice's zero-fill was only STAGED in UpdateForwardPlusLights
		// (ring flush happens at EndAndPresent), so the transfer-queue
		// FIFO keeps this copy ahead of it and it reads the retired
		// accumulation like the exposure histogram does.
		m_pendingReadbackHasDs = DsActive();
		if (m_pendingReadbackHasFeedback)
			m_pendingFeedback.assign(m_textureFeedback->GetSlotCount(),
				smvk::TextureFeedback::kNoReport);
		auto dlsSpan = buildPendingDownloads();
		if (!dlsSpan.empty())
			m_asyncReadbackTicket =
				m_context->SubmitAsyncDownloadDataFromBuffers(dlsSpan);
	}

		if (feedbackActive)
			m_textureFeedback->ClearForFrame(frameSlot);
	}

	// ---------------------------------------------------------------------
	// Procedural Forward+ demo lights.
	//
	// The demo light set is pure data (no scene nodes, no emissive ball
	// meshes): UpdateForwardPlusLights() computes the point grid (with the
	// bob animation) and the orbiting spots on the CPU and uploads the
	// Forward+ light buffer directly. The ImGui overlay draws the lights
	// as colored points + indices (see the UI block in RenderFrame).
	// ---------------------------------------------------------------------
	// Builds the procedural Forward+ demo light set into the per-frame upload
	// buffer. Returns the number of lights written (clamped to FP_MAX_LIGHTS).
	// Spots are written FIRST: per-tile lists keep only the first
	// FP_MAX_PER_TILE passing lights, so the few (expensive) cone lights must
	// not compete with the point crowd for those slots. A CPU copy is kept in
	// m_debugLights for the ImGui light-point overlay.
	// Spot-shadow selection runs here: spots carrying the cast-shadow request
	// flag are ranked by the screenspace/distance heuristic and the top-N
	// (upper limit m_fpDemo.spotShadowMax) get atlas tiles; their shadowTile /
	// shadowMapIndex are patched before upload. Results land in
	// m_spotShadowCount / m_spotShadowMapIndex for the frame's passes.
	uint32_t SceneRenderer::UpdateForwardPlusLights(uint32_t currentFrame, const FrameUniforms& frame) {
		using fp_clk = std::chrono::steady_clock;
		const bool fpBench = frameBudget_ > 0 && benchWarmup_ >= 10;
		const auto fpT0 = fp_clk::now();
		std::vector<smvk::ForwardPlusLight> lights;
		lights.reserve(FP_MAX_LIGHTS);
		m_debugLights.clear();
		if (m_fpDemo.enabled) {
			const int gx = std::max(1, m_fpDemo.gridX);
			const int gy = std::max(1, m_fpDemo.gridY);
			const int gz = std::max(1, m_fpDemo.gridZ);
			const uint32_t gridCount = (uint32_t)gx * (uint32_t)gy * (uint32_t)gz;
			const uint32_t spotReserve = (uint32_t)std::max(0, m_fpDemo.spotCount);
			const uint32_t pointCount = (FP_MAX_LIGHTS > spotReserve)
				? std::min(gridCount, FP_MAX_LIGHTS - spotReserve)
				: 0;

		// Orbiting spots to exercise the cone path (written first â€”
		// see the comment above). Centered on the fitted scene bounds
		// (or the origin in manual mode) so they orbit the visible scene.
		const glm::vec3 fitCenter = m_fpDemo.fitValid
			? (m_fpDemo.fitMin + m_fpDemo.fitMax) * 0.5f
			: glm::vec3(0.0f);
		const glm::vec3 fitExtent = m_fpDemo.fitValid
			? (m_fpDemo.fitMax - m_fpDemo.fitMin)
			: glm::vec3(12.0f, 7.0f, 8.0f);
		const bool useFit = m_fpDemo.enabled && m_fpDemo.fitToSceneBounds && m_fpDemo.fitValid;
		const glm::vec3 orbitC = useFit ? fitCenter : glm::vec3(0.0f);
		const float orbitRX = useFit ? std::max(2.0f, fitExtent.x * 0.25f) : 6.0f;
		const float orbitRZ = useFit ? std::max(2.0f, fitExtent.z * 0.25f) : 4.0f;
		const float orbitY = useFit
			? fitCenter.y + std::max(1.0f, fitExtent.y * 0.35f)
			: 7.0f;
		for (int s = 0; s < m_fpDemo.spotCount && lights.size() < FP_MAX_LIGHTS; ++s) {
			smvk::ForwardPlusLight l{};
			float a = time_since_start_s * 0.4f * m_fpDemo.speed + s * 6.2831853f / std::max(1, m_fpDemo.spotCount);
			l.position = { orbitC.x + std::cos(a) * orbitRX, orbitY, orbitC.z + std::sin(a) * orbitRZ };
				l.direction = glm::normalize((orbitC - glm::vec3(l.position)) * 0.35f + glm::vec3(0, -1, 0));
				l.range = m_fpDemo.spotRange;
				l.color = glm::vec4(1.0f, 0.9f, 0.75f, 1.0f);
				l.intensity = m_fpDemo.spotIntensity;
				l.innerAngle = glm::radians(12.0f);
				l.outerAngle = glm::radians(28.0f);
				l.type = smvk::kForwardPlusSpot;
				// Controllable per-light flag: demo opts all spots in; the
				// frame selection below decides which actually get tiles.
				if (m_fpDemo.spotShadowsEnabled)
					l.flags |= smvk::kForwardPlusFlag_CastShadow;
				l.shadowTile = -1;
				l.shadowMapIndex = -1;
				l.shadowBias = SPOT_SHADOW_BIAS;
				// Light cookie: gobo projected through the cone (spot s gets
				// pattern s % kCookiePatternCount). -1 leaves the light clean.
				if (m_fpDemo.cookiesEnabled)
					l.cookieIndex = m_cookies.Bindless(s % smvk::kCookiePatternCount);
				// Light function: procedural intensity curve on the spot.
				if (m_fpDemo.lightFunctionsEnabled) {
					l.functionType = m_fpDemo.functionType;
					l.functionSpeed = m_fpDemo.functionSpeed;
					l.functionScale = m_fpDemo.functionScale;
				}
				lights.push_back(l);
				m_debugLights.push_back({ glm::vec3(l.position), glm::vec3(l.color),
					(int)smvk::kForwardPlusSpot });
			}

		// Point grid (positions were scene-node ball transforms before;
		// same placement/bob math, now pure CPU arithmetic). Fit mode
		// centers the grid on the scene bounds with spacing derived as
		// extent / (count - 1); manual mode uses spacingX/Y/Z + baseY.
		float effSX = m_fpDemo.spacingX, effSY = m_fpDemo.spacingY, effSZ = m_fpDemo.spacingZ;
		float yBase = m_fpDemo.baseY;
		if (useFit) {
			effSX = (gx > 1) ? fitExtent.x / float(gx - 1) : 0.0f;
			effSY = (gy > 1) ? fitExtent.y / float(gy - 1) : 0.0f;
			effSZ = (gz > 1) ? fitExtent.z / float(gz - 1) : 0.0f;
			yBase = m_fpDemo.fitMin.y;
		}
		const float ox = (useFit ? fitCenter.x : 0.0f) - (gx - 1) * 0.5f * effSX;
		const float oz = (useFit ? fitCenter.z : 0.0f) - (gz - 1) * 0.5f * effSZ;
			for (uint32_t i = 0; i < pointCount && lights.size() < FP_MAX_LIGHTS; ++i) {
				const uint32_t ix = i % (uint32_t)gx;
				const uint32_t iy = (i / (uint32_t)gx) % (uint32_t)gy;
				const uint32_t iz = i / ((uint32_t)gx * (uint32_t)gy);
				const float phase = float(ix * 7 + iy * 13 + iz * 29) * 0.35f;
				const float bob = m_fpDemo.animate
					? std::sin(time_since_start_s * m_fpDemo.speed + phase) * 0.5f
					: 0.0f;
			const glm::vec3 pos(
				ox + ix * effSX,
				yBase + iy * effSY + bob,
				oz + iz * effSZ
			);
				smvk::ForwardPlusLight l{};
				l.position = pos;
				l.range = m_fpDemo.range;
				// Warm/cool checkerboard so tiling artifacts read clearly.
				const bool warm = ((ix + iy + iz) & 1) == 0;
				l.color = warm
					? glm::vec4(1.0f, 0.62f, 0.32f, 1.0f)
					: glm::vec4(0.35f, 0.65f, 1.0f, 1.0f);
				l.intensity = m_fpDemo.intensity;
				l.type = smvk::kForwardPlusPoint;
				// Points never cast shadows (no shadow tile), carry no gobo
				// and no light function: sentinels, not 0 (see common.hlsli).
				l.shadowTile = -1;
				l.shadowMapIndex = -1;
				l.cookieIndex = -1;
				l.functionType = smvk::kForwardPlusFunction_None;
				lights.push_back(l);
				m_debugLights.push_back({ pos, glm::vec3(l.color),
					(int)smvk::kForwardPlusPoint });
			}
		}

		// ---- Spot-shadow caster selection (upper limit + heuristic) ----
		m_spotShadowCount = 0;
		m_spotShadowMapIndex = -1;
		m_spotShadowTiles.clear();
		if (m_passes.spotShadows && m_fpDemo.spotShadowsEnabled && m_viewportResources.spotShadows.Valid() && !lights.empty()) {
			const uint32_t wantMax = (uint32_t)std::clamp(m_fpDemo.spotShadowMax, 0, (int)smvk::kMaxSpotShadows);
			const uint32_t budget = std::min(wantMax, m_viewportResources.spotShadows.slotCount);
			if (budget > 0) {
				const float proj11 = std::abs(frame.proj[1][1]);
				auto selected = smvk::SelectSpotShadowCasters(
					lights, frame.viewProj, frame.cameraPos,
					m_fpDemo.spotShadowMaxDistance, budget, proj11);
				const int32_t atlasIdx = m_viewportResources.spotShadows.BindlessIndex(*m_context);
				// EVSM: lights address the blurred moments (1-frame lag);
				// tile descriptors are layout-identical (same 8x1024 grid).
				// NOTE: the tile lightVP below is built from the CURRENT
				// light state while the blurred spot atlas trails one slot
				// under OneFrameLag â€” fine for static spots (the demo
				// lights), but ANIMATED spots would need prev-frame tile
				// descriptors like m_dirSamplePrev does for the sun.
				// SameFrame (default) has no lag by construction.
				const int32_t evsmIdx = (EvsmActive())
					? m_viewportResources.evsmSpot.BlurredBindlessIndex(*m_context, currentFrame)
					: -1;
				const int32_t sampleIdx = (evsmIdx >= 0) ? evsmIdx : atlasIdx;
				if (atlasIdx >= 0 && !selected.empty()) {
					std::vector<smvk::GpuCascadeDescription> tiles(selected.size());
					for (size_t k = 0; k < selected.size(); ++k) {
						smvk::ForwardPlusLight& sl = lights[selected[k].lightIndex];
					tiles[k] = m_viewportResources.spotShadows.BuildSpotTile(
						glm::vec3(sl.position[0], sl.position[1], sl.position[2]),
						glm::normalize(glm::vec3(sl.direction[0], sl.direction[1], sl.direction[2])),
						sl.outerAngle, sl.range, atlasIdx, (uint32_t)k,
						SPOT_SHADOW_NEAR, SPOT_SHADOW_BIAS,
						// Soft spot shadows: emitting-sphere diameter +
						// artistic softness (tile pads, see evsm.hlsli).
						m_evsmSpotLightSize, m_evsmSoftness);
						sl.shadowTile = (int32_t)k;
						sl.shadowMapIndex = sampleIdx;
						sl.shadowBias = SPOT_SHADOW_BIAS;
					}
					constexpr size_t SPOT_FRAME_STRIDE =
						sizeof(smvk::GpuCascadeDescription) * smvk::kMaxSpotShadows;
					m_context->UploadDataToBuffer(
						m_resources.spotShadowBuffer,
						tiles.data(),
						sizeof(smvk::GpuCascadeDescription) * tiles.size(),
						smvk::PipelineStage(smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment),
						smvk::AccessFlags::ShaderRead,
						0,
						currentFrame * SPOT_FRAME_STRIDE
					);
					m_spotShadowCount = (uint32_t)selected.size();
					m_spotShadowMapIndex = sampleIdx;
					// Keep the tiles for the per-frame spot RenderViews.
					m_spotShadowTiles = std::move(tiles);
				}
			}
		}

	// ---- Deferred shadows, phase 1 (CPU Prepare, paper 2.1) ----
	// Sort by closest distance to the near plane BEFORE the upload: the
	// GPU light buffer and every zBin index refer to this order. Runs
	// after the spot-shadow selection so patched shadowTile fields travel
	// with their lights. (When the pipeline is off, keep the spots-first
	// order the capped forward+ list relies on.)
	if (DsActive() && !lights.empty()) {
		smvk::DsSortLightsByNearDistance(lights, frame.view);
#if defined(DS_DEBUG_VIZ)
		// Sorted-buffer snapshot for the visualizer UI: the light count
		// (slider clamp) + the selected light's post-sort identity for
		// the ImGui marker. Indices in dsDebugLight refer to THIS order.
		m_dsLightCount = (uint32_t)lights.size();
		const uint32_t li = std::min((uint32_t)std::max(0, m_dsDebugLight),
			m_dsLightCount - 1);
		const smvk::ForwardPlusLight& L = lights[li];
		m_dsDebugLightPos = glm::vec3(L.position);
		m_dsDebugLightColor = glm::vec3(L.color);
		m_dsDebugLightType = (int)L.type;
		m_dsDebugLightRange = L.range;
		m_dsDebugLightValid = true;
#endif
	}
#if defined(DS_DEBUG_VIZ)
	else {
		m_dsLightCount = 0;
		m_dsDebugLightValid = false;
	}
#endif

	constexpr size_t FP_FRAME_STRIDE = sizeof(smvk::ForwardPlusLight) * FP_MAX_LIGHTS;
	const auto fpT1 = fp_clk::now();
	if (!lights.empty()) {
		m_context->UploadDataToBuffer(
			m_resources.fpLightsBuffer,
			lights.data(),
			sizeof(smvk::ForwardPlusLight) * lights.size(),
			smvk::PipelineStage(smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute),
			smvk::AccessFlags::ShaderRead,
			0,
			currentFrame * FP_FRAME_STRIDE
		);
	}

	// zBins + Tile Light Masks + counter reset (same flight slice the
	// phases below use). Uploads run even with zero lights so the buffers
	// are always freshly zeroed â€” the phases then emit empty lists and
	// the consumer iterates nothing (no stale-data path exists).
	const auto fpT2 = fp_clk::now();
	if (DsActive()) {
		smvk::DeferredShadows& ds = m_viewportResources.deferredShadows;
		// Stats readback of the RETIRED slice moved to UpdateReadbacks
		// (async ticket, exposure pattern): the old synchronous
		// DownloadDataFromBuffer here flushed uploads + waited on the
		// graphics queue every 8th frame, blocking the render thread
		// for the in-flight backlog (multi-ms spikes). The light count
		// is captured here for the throttled [ds] telemetry line.
		m_dsReadbackLightCount = (uint32_t)lights.size();
		// Zero this frame's flexible-allocation counters (transfer write;
		// the phase-2 pass hint promotes them to Compute R/W). Issued
		// BEFORE UpdateReadbacks submits its ticket copy, and the ring
		// only flushes at EndAndPresent, so the transfer-queue FIFO keeps
		// the copy ahead of this zero (download-before-zero, same
		// ordering the exposure histogram relies on).
		static constexpr uint32_t kZeroCounters[smvk::kDsCounterCount] = {};
		m_context->UploadDataToBuffer(ds.CounterBuffer(currentFrame),
			kZeroCounters, sizeof(kZeroCounters),
			smvk::PipelineStageBit_Transfer, smvk::AccessFlags::TransferWrite,
			0, 0);
		// zBins (min/max + ranged min/max per depth bin, sorted indices).
		smvk::DsBuildZBins(lights, frame.view, frame.nearPlane, frame.farPlane,
			m_dsZBinStaging);
		m_context->UploadDataToBuffer(ds.ZBinsBuffer(currentFrame),
			m_dsZBinStaging.data(),
			std::min(m_dsZBinStaging.size() * sizeof(uint32_t), smvk::kDsZBinBytes),
			smvk::PipelineStageBit_Compute, smvk::AccessFlags::ShaderRead,
			0, 0);
		// Tile Light Masks (64x64px TileGroup granularity, figure 4). The
		// builder derives the same group grid from the same extent, so the
		// staging layout matches the GPU buffers exactly.
		uint32_t maskGX = 0, maskGY = 0;
		smvk::DsBuildTileMasks(lights, frame.view, frame.proj,
			(uint32_t)frame.viewportSize.x, (uint32_t)frame.viewportSize.y,
			m_dsMaskStaging, maskGX, maskGY);
		assert(maskGX == ds.groupCountX && maskGY == ds.groupCountY);
		m_context->UploadDataToBuffer(ds.TileMasksBuffer(currentFrame),
			m_dsMaskStaging.data(),
			std::min(m_dsMaskStaging.size() * sizeof(uint32_t), smvk::kDsTileMaskBytes),
			smvk::PipelineStageBit_Compute, smvk::AccessFlags::ShaderRead,
			0, 0);
	}
	if (fpBench) {
		using msd = std::chrono::duration<double, std::milli>;
		const auto fpT3 = fp_clk::now();
		s_benchFpGen += msd(fpT1 - fpT0).count();
		s_benchFpUp += msd(fpT2 - fpT1).count();
		s_benchFpDs += msd(fpT3 - fpT2).count();
	}
	return (uint32_t)lights.size();
	}

	// ---------------------------------------------------------------------
	// Forward+ decals (tiled projected splats, see smvk::ForwardPlusDecal +
	// forwardplus.cs.hlsl / default_lit_body.hlsli). CPU side keeps cheap
	// placement descriptors (DecalDemo::Splat); the GPU list is re-derived
	// every frame so the demo sliders retouch every live decal.
	// ---------------------------------------------------------------------

	uint32_t SceneRenderer::UpdateDecals(uint32_t currentFrame) {
		m_decalFrameCount = 0;
		if (!DecalsActive())
			return 0;

		std::vector<smvk::ForwardPlusDecal> gpu;
		gpu.reserve(std::min<size_t>(m_decalDemo.splats.size(), FP_MAX_DECALS));
		const int32_t flags = smvk::kDecalFlag_Albedo | smvk::kDecalFlag_Normal
			| smvk::kDecalFlag_Material;
		for (const DecalDemo::Splat& s : m_decalDemo.splats) {
			if (gpu.size() >= FP_MAX_DECALS)
				break;
			smvk::gi::Pcg32 yawRng(s.seed ? s.seed : 1u);
			const float yaw = float(yawRng.Next()) * (6.2831853f / 4294967296.0f);
			gpu.push_back(smvk::MakeDecal(s.center, s.axisZ, s.upHint, s.half,
				(int32_t)m_decalAlbedoBindless, (int32_t)m_decalNormalBindless,
				flags,
				glm::clamp(m_decalDemo.opacity * s.alphaScale, 0.0f, 1.0f),
				glm::vec3(1.0f), 0.0f, glm::vec3(0.0f),
				m_decalDemo.normalStrength, m_decalDemo.roughnessTarget,
				m_decalDemo.metalTarget, m_decalDemo.depthFadeStart, yaw));
		}

		if (!gpu.empty()) {
			constexpr size_t DECAL_FRAME_STRIDE =
				sizeof(smvk::ForwardPlusDecal) * FP_MAX_DECALS;
			// Cull reads the decal buffer in compute; Main samples it in
			// fragment (same upload pattern as the fpLights ring).
			m_context->UploadDataToBuffer(
				m_resources.decalsBuffer,
				gpu.data(),
				sizeof(smvk::ForwardPlusDecal) * gpu.size(),
				smvk::PipelineStage(smvk::PipelineStageBit_Compute | smvk::PipelineStageBit_Fragment),
				smvk::AccessFlags::ShaderRead,
				0,
				currentFrame * DECAL_FRAME_STRIDE
			);
		}
		m_decalFrameCount = (uint32_t)gpu.size();
		return m_decalFrameCount;
	}

	bool SceneRenderer::EnsurePickScene() {
		// Re-import the scene glTF as a GPU-free cook-mode cluster with
		// retained CPU uploads â€” the same importer call the GiBaker uses â€”
		// then extract + BVH the triangle soup. (The runtime pak cluster
		// has no retained CPU geometry, and the pick wants the exact
		// loaded transform: same coordFix + preTransform as the scene
		// load. Sponza costs a one-time import + SAH build of a few
		// seconds; the console notes the hitch.)
		if (m_decalDemo.pickBvhState == 0) {
			const char* gltfPathC = m_scene.gltfPath ? m_scene.gltfPath : "";
			const std::string vfsPath = (gltfPathC && std::strncmp(gltfPathC, "content/", 8) == 0)
				? std::string(gltfPathC + 8) : std::string(gltfPathC ? gltfPathC : "");
			if (vfsPath.empty()) {
				std::cerr << "[Pick] no scene glTF - pick BVH off\n";
				m_decalDemo.pickBvhState = -1;
			} else {
				std::cerr << "[Pick] building CPU pick BVH (one-time glTF re-import)...\n";
				const auto t0 = std::chrono::steady_clock::now();
				try {
					smvk::OverlayVfs vfs;
					vfs.Mount(std::make_shared<smvk::LooseBackend>("content"), 0, "content");
					auto cluster = std::make_unique<smvk::Cluster>(nullptr);
					cluster->SetCookMode(true);
					GltfToClusterFromVfs(nullptr, vfs, vfsPath, *cluster,
						smvk::MeshOptConfig{}, smvk::MeshletConfig{},
						m_scene.coordFix, m_scene.preTransform);
					auto scene = std::make_unique<smvk::gi::GiTraceScene>();
					smvk::gi::GiSceneBuildOptions opts{};
					std::string err;
					smvk::gi::BuildTraceSceneFromCluster(*cluster, opts, *scene, err);
					if (!err.empty())
						std::cerr << "[Pick] pick scene: " << err << "\n";
					scene->Finalize();
					const bool ready = err.empty() && !scene->Bvh().Empty();
					m_decalDemo.pickBvhState = ready ? 1 : -1;
					if (ready) {
						const auto t1 = std::chrono::steady_clock::now();
						const double ms =
							std::chrono::duration<double, std::milli>(t1 - t0).count();
						std::cerr << "[Pick] pick BVH ready: "
							<< scene->Bvh().TriangleCount() << " tris ("
							<< (int)ms << " ms)\n";
					} else {
						std::cerr << "[Pick] pick BVH unavailable for this scene\n";
					}
					m_decalPickScene = std::move(scene);
				} catch (const std::exception& e) {
					std::cerr << "[Pick] pick BVH import failed: " << e.what() << "\n";
					m_decalDemo.pickBvhState = -1;
				}
			}
		}
		return m_decalDemo.pickBvhState > 0 && m_decalPickScene
			&& !m_decalPickScene->Bvh().Empty();
	}

	bool SceneRenderer::GiProbeVisibleFromCamera(uint32_t probeIdx, const glm::vec3& camPos) {
		// View-coherent cache: camera still => free; camera moved =>
		// invalidate (recomputed lazily per drawn probe below).
		const glm::mat4 view = m_camera.getViewMatrix();
		if (m_giProbeCullCache.size() != m_giVolume.probes.size()
			|| std::memcmp(&view, &m_giProbeCullView, sizeof(view)) != 0) {
			m_giProbeCullView = view;
			m_giProbeCullCache.assign(m_giVolume.probes.size(), 2);
		}
		char& slot = m_giProbeCullCache[probeIdx];
		if (slot != 2)
			return slot != 0;
		bool visible = true; // fail-open: no pick geometry => draw unculled
		if (EnsurePickScene()) {
			const glm::vec3 p = m_giVolume.probes[probeIdx].position;
			const glm::vec3 d = p - camPos;
			const float dist = glm::length(d);
			if (dist > 1e-4f) {
				smvk::gi::TriangleBvh::Hit hit;
				// Occluded when geometry blocks the ray well short of the
				// probe (5 cm bias: probes float >= 3 cm off surfaces).
				visible = !m_decalPickScene->Bvh().Raycast(
					camPos, d / dist, dist - 0.05f, hit);
			}
		}
		slot = visible ? 1 : 0;
		return visible;
	}

	void SceneRenderer::SpawnDecalAtCrosshair() {
		if (!DecalsActive()) {
			std::cerr << "[Decals] disabled (UI checkbox or VULKANTOY_DECALS=1)\n";
			return;
		}

		// Camera ray: crosshair = view center; the view matrix maps world
		// -> view (looking down -z), so the world-space forward is the
		// negated third column of the inverse view.
		const glm::mat4 invView = glm::inverse(m_camera.getViewMatrix());
		const glm::vec3 eye = m_cameraPositioner.cameraPosition_;
		const glm::vec3 fwd = -glm::normalize(glm::vec3(invView[2]));

		// Lazy pick BVH (first J): shared builder below (one-time glTF
		// re-import hitch, noted on console).
		if (!EnsurePickScene()) {
			std::cerr << "[Decals] no pick geometry - crosshair spawn off\n";
			return;
		}

		smvk::gi::TriangleBvh::Hit hit;
		constexpr float kDecalPickRange = 500.0f;
		if (!m_decalPickScene->Bvh().Raycast(eye, fwd, kDecalPickRange, hit)) {
			std::cerr << "[Decals] crosshair miss (aim at a surface)\n";
			return;
		}
		glm::vec3 n = m_decalPickScene->Bvh().TriangleNormal(hit.tri);
		if (glm::length(n) < 1e-5f) {
			std::cerr << "[Decals] degenerate hit triangle\n";
			return;
		}
		n = glm::normalize(n);
		if (glm::dot(n, fwd) > 0.0f)
			n = -n; // two-sided BVH: face the incoming ray
		const glm::vec3 hitPos = eye + fwd * hit.t;

		// Texture V hint: camera up projected into the surface plane (so
		// wall smears read upright); camera right as the near-floor
		// fallback (world up degenerates against a floor's normal).
		glm::vec3 camUp = glm::normalize(glm::vec3(invView[1]));
		glm::vec3 camRight = glm::normalize(glm::vec3(invView[0]));
		glm::vec3 up = camUp - n * glm::dot(camUp, n);
		if (glm::length(up) < 0.1f)
			up = camRight - n * glm::dot(camRight, n);

		smvk::gi::Pcg32 rng(++m_decalSpawnSeed * 0x9E3779B97F4A7C15ull);
		const float t = rng.Float();
		const float half = m_decalDemo.sizeMin
			+ (m_decalDemo.sizeMax - m_decalDemo.sizeMin) * t;

		DecalDemo::Splat s{};
		s.center = hitPos + n * 0.30f;   // hover the projector off the wall
		s.axisZ = -n;                    // project INTO the surface
		s.upHint = up;
		s.half = glm::vec3(half, half, 0.85f); // depth reach straddles the wall
		s.seed = m_decalSpawnSeed++;
		s.alphaScale = 0.75f + 0.25f * rng.Float();
		if (m_decalDemo.splats.size() >= FP_MAX_DECALS) {
			m_decalDemo.splats.erase(m_decalDemo.splats.begin());
			m_decalDemo.dropped++;
		}
		m_decalDemo.splats.push_back(s);
	}

	void SceneRenderer::SpawnDecalPresets() {
		if (std::string_view(m_scene.id) != "sponza")
			return;
		if (!m_decalDemo.enabled || m_decalAlbedoBindless == UINT32_MAX)
			return;
		if (!m_decalDemo.splats.empty())
			return;

		const auto addSplat = [this](glm::vec3 center, glm::vec3 axisZ,
			glm::vec3 upHint, glm::vec3 half, float alphaScale, uint32_t seed) {
			DecalDemo::Splat s{};
			s.center = center;
			s.axisZ = axisZ;
			s.upHint = upHint;
			s.half = half;
			s.alphaScale = alphaScale;
			s.seed = seed;
			if (m_decalDemo.splats.size() >= FP_MAX_DECALS)
				m_decalDemo.splats.erase(m_decalDemo.splats.begin());
			m_decalDemo.splats.push_back(s);
		};
		// Floor pools down the +X corridor arm (default camera looks +X
		// from (0,1,0) after the 90-degree rotation â€” see SceneRegistry::
		// Sponza; horizon-level view, so the pools sit in the lower band
		// of the frame). Floor plane is y = 0; the box hovers 0.5 m above
		// it, half depth 0.75 m -> |local.z| ~0.67 at the floor, inside
		// the depth fade, and the shallower box limits catches on
		// mid-air clutter (banners, lamp chains) inside the volume.
		addSplat(glm::vec3(4.3f, 0.5f, -0.2f), glm::vec3(0, -1, 0),
			glm::vec3(0, 1, 0), glm::vec3(0.6f, 0.6f, 0.75f), 1.0f, 11u);
		addSplat(glm::vec3(3.5f, 0.5f, -1.35f), glm::vec3(0, -1, 0),
			glm::vec3(0, 1, 0), glm::vec3(0.42f, 0.42f, 0.75f), 0.85f, 23u);
		addSplat(glm::vec3(5.4f, 0.5f, 1.30f), glm::vec3(0, -1, 0),
			glm::vec3(0, 1, 0), glm::vec3(0.55f, 0.55f, 0.75f), 0.90f, 37u);
		// Wall smear on the corridor's far end wall (screen-center at
		// horizon level: the same surface the crosshair spawner hits â€”
		// deep box so the end-arch band lands inside it; the depth fade
		// keeps any double projection on deeper returns faint).
		addSplat(glm::vec3(10.9f, 1.6f, 0.2f), glm::vec3(1, 0, 0),
			glm::vec3(0, 1, 0), glm::vec3(1.0f, 1.0f, 1.6f), 1.0f, 53u);
		// Side-wall spray on the +z arcade (screen-right): the box reaches
		// the arcade/wall band; curved + occluded geometry means this one
		// reads as a fainter secondary spray.
		addSplat(glm::vec3(7.6f, 1.7f, 4.95f), glm::vec3(0, 0, 1),
			glm::vec3(0, 1, 0), glm::vec3(0.8f, 0.8f, 0.85f), 0.95f, 71u);
		std::cerr << "[Decals] sponza preset splats: "
			<< m_decalDemo.splats.size() << "\n";
	}

	bool SceneRenderer::InitDecalTextures() {
		if (m_decalAlbedoBindless != UINT32_MAX && m_decalNormalBindless != UINT32_MAX)
			return true; // idempotent (recreate path)
		if (!m_context)
			return false;

		// Procedural blood splat: metaball blob field + radial droplet
		// comet tails -> coverage alpha; color darkens with thickness
		// (pooled core) and the normal map derives from the blurred field
		// (coagulated surface bump). Deterministic Pcg32 seed so the
		// check scene renders identically every run.
		constexpr int S = 512;
		smvk::gi::Pcg32 rng(0xB100D5EEDull);
		struct Blob { float x, y, r, amp; };
		std::vector<Blob> blobs;
		const int blobCount = 9;
		blobs.reserve(blobCount + 28);
		for (int i = 0; i < blobCount; ++i) {
			Blob b{};
			b.x = (rng.Float() * 2.0f - 1.0f) * 0.16f;
			b.y = (rng.Float() * 2.0f - 1.0f) * 0.16f;
			b.r = 0.10f + rng.Float() * 0.13f;
			b.amp = 0.7f + rng.Float() * 0.6f;
			blobs.push_back(b);
		}
		// Droplets: heads at a radial distance, tails stretched outward.
		for (int i = 0; i < 26; ++i) {
			const float ang = rng.Float() * 6.2831853f;
			const float dist = 0.22f + rng.Float() * 0.55f;
			const float rad = 0.012f + rng.Float() * 0.035f;
			const float ca = std::cos(ang), sa = std::sin(ang);
			// Head + tail pair along the radial direction (comet).
			for (int k = 0; k < 2; ++k) {
				Blob b{};
				const float d = dist - k * rad * 1.6f;
				b.x = ca * d;
				b.y = sa * d;
				b.r = rad * (k == 0 ? 1.0f : 0.62f);
				b.amp = k == 0 ? 0.85f : 0.55f;
				blobs.push_back(b);
			}
		}

		std::vector<float> field(size_t(S) * S, 0.0f);
		for (int y = 0; y < S; ++y) {
			for (int x = 0; x < S; ++x) {
				const float u = (float(x) / float(S - 1)) * 2.0f - 1.0f;
				const float v = (float(y) / float(S - 1)) * 2.0f - 1.0f;
				float f = 0.0f;
				for (const Blob& b : blobs) {
					const float dx = u - b.x, dy = v - b.y;
					const float d2 = dx * dx + dy * dy;
					f += b.amp * std::exp(-d2 / (2.0f * b.r * b.r));
				}
				field[size_t(y) * S + x] = f;
			}
		}

		const auto srgb8 = [](float c) {
			c = std::min(std::max(c, 0.0f), 1.0f);
			const float lo = c * 12.92f;
			const float hi = 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
			const float val = c <= 0.0031308f ? lo : hi;
			return static_cast<uint8_t>(std::lround(val * 255.0f));
		};
		const auto smooth01 = [](float e0, float e1, float x) {
			const float t = std::min(std::max((x - e0) / (e1 - e0), 0.0f), 1.0f);
			return t * t * (3.0f - 2.0f * t);
		};

		std::vector<uint8_t> albedo(size_t(S) * S * 4);
		std::vector<uint8_t> normal(size_t(S) * S * 4);
		for (int y = 0; y < S; ++y) {
			for (int x = 0; x < S; ++x) {
				const size_t i = size_t(y) * S + x;
				const float f = field[i];
				const float alpha = smooth01(0.5f, 0.62f, f);
				// Linear blood color: darker where thick (pooled core),
				// brighter at the thin rim. The low roughness target
				// (DecalDemo) supplies the wet specular read.
				const float thin = 1.0f - alpha;
				glm::vec3 c(
					0.055f + 0.105f * thin,
					0.0025f + 0.0055f * thin,
					0.002f + 0.003f * thin);
				uint8_t* o = &albedo[i * 4];
				o[0] = srgb8(c.r);
				o[1] = srgb8(c.g);
				o[2] = srgb8(c.b);
				o[3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));

				// Normal from the field height (Sobel, 2-texel reach so
				// the bump survives 0.5-scale splats). Edge-clamped.
				const auto hAt = [&](int xx, int yy) {
					return field[size_t(std::clamp(yy, 0, S - 1)) * S
						+ std::clamp(xx, 0, S - 1)];
				};
				const float bump = 0.55f;
				float nx = (hAt(x - 2, y) - hAt(x + 2, y)) * bump;
				float ny = (hAt(x, y - 2) - hAt(x, y + 2)) * bump;
				const float len = std::sqrt(nx * nx + ny * ny + 1.0f);
				nx /= len;
				ny /= len;
				const float nz = 1.0f / len;
				uint8_t* nOut = &normal[i * 4];
				nOut[0] = static_cast<uint8_t>(std::lround((nx * 0.5f + 0.5f) * 255.0f));
				nOut[1] = static_cast<uint8_t>(std::lround((ny * 0.5f + 0.5f) * 255.0f));
				nOut[2] = static_cast<uint8_t>(std::lround((nz * 0.5f + 0.5f) * 255.0f));
				nOut[3] = 255;
			}
		}

		smvk::ImageCreateInfo ci{};
		ci.extent = { (uint32_t)S, (uint32_t)S, 1 };
		ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;

		ci.format = smvk::Format::RGBA8_SRGB; // sampling lands in linear
		ci.name = "decalBloodAlbedo";
		auto albedoTex = m_context->CreateTexture(ci, albedo.data());
		ci.format = smvk::Format::RGBA8_UNorm; // normals stay raw
		ci.name = "decalBloodNormal";
		auto normalTex = m_context->CreateTexture(ci, normal.data());
		if (albedoTex.empty() || normalTex.empty()) {
			std::cerr << "[Decals] splat texture creation failed\n";
			return false;
		}
		m_decalBloodAlbedo = std::move(albedoTex);
		m_decalBloodNormal = std::move(normalTex);
		m_decalAlbedoBindless = m_context->GetTextureBindlessIndex(m_decalBloodAlbedo);
		m_decalNormalBindless = m_context->GetTextureBindlessIndex(m_decalBloodNormal);
		m_context->FlushUploads();
		std::cerr << "[Decals] blood splat textures ready (albedo bindless "
			<< m_decalAlbedoBindless << ", normal bindless "
			<< m_decalNormalBindless << ")\n";
		return m_decalAlbedoBindless != UINT32_MAX
			&& m_decalNormalBindless != UINT32_MAX;
	}

	// ---------------------------------------------------------------------
	// Deferred shadows, phases 2-4 (GPU). Appended right where the legacy
	// forward+ tile cull would run â€” i.e. over the complete depth buffer
	// (seed + prepass + remainder). Each phase is its own compute pass so
	// the rendergraph emits the RAW/WAW buffer barriers between them.
	// ---------------------------------------------------------------------
	void SceneRenderer::AppendDeferredShadowPhases(uint32_t frameIndex,
		uint64_t frameUniformsAddress, uint64_t fpLightsAddress,
		uint64_t spotShadowBufferAddress, uint32_t fpLightCount,
		smvk::LogicalImageViewHandle depthView,
		smvk::LogicalBufferHandle fpLightsLogical)
	{
		smvk::DeferredShadows& ds = m_viewportResources.deferredShadows;
		if (!ds.Valid() || !ds.ShadersValid())
			return;

		SMVK_PROFILE_CATEGORY("DeferredShadows", Optick::Category::Rendering);
		SMVK_PROFILE_TAG("dsTileCountX", ds.tileCountX);
		SMVK_PROFILE_TAG("dsLightCount", fpLightCount);

		smvk::DsPhasePush push{};
		push.frameUniforms = frameUniformsAddress;
		push.lights = fpLightsAddress;
		push.zbins = ds.ZBinsAddress(*m_context, frameIndex);
		push.tileMasks = ds.TileMasksAddress(*m_context, frameIndex);
		push.tileGroupMasks = ds.TileGroupMasksAddress(*m_context, frameIndex);
		push.tileState = ds.TileStateAddress(*m_context, frameIndex);
		push.lightList = ds.LightListAddress(*m_context, frameIndex);
		push.visibility = ds.VisibilityAddress(*m_context, frameIndex);
		push.counters = ds.CountersAddress(*m_context, frameIndex);
		push.spotShadowBuffer = spotShadowBufferAddress;
		push.spotShadowMapIndex =
			m_spotShadowMapIndex >= 0 ? (uint32_t)m_spotShadowMapIndex : UINT32_MAX;
		push.spotShadowCount = m_spotShadowCount;
		push.lightCount = std::min(fpLightCount, FP_MAX_LIGHTS);
		push.tileCountX = ds.tileCountX;
		push.tileCountY = ds.tileCountY;
		push.groupCountX = ds.groupCountX;
		push.groupCountY = ds.groupCountY;
		push.depthTextureIndex = m_viewportResources.forwardPlus.depthBindlessIndex;
		// PCSS (paper 2.4): spot visibility uses Percentage Closer Soft
		// Shadows; the near plane inverts NDC->eye depth and the light
		// size sets the blocker search + penumbra widths.
		push.spotShadowNear = SPOT_SHADOW_NEAR;
		push.pcssLightSize = 0.35f; // demo spots: ~35cm emitting sphere

		// Phase 2 â€” Find Lights Approximate (one TileGroup per work unit).
		{
			auto& pass = ds.AddPhase2Pass(*m_rendergraph, depthView,
				fpLightsLogical, frameIndex);
			auto program = ds.phasePrograms[0];
			const uint32_t gx = ds.groupCountX;
			const uint32_t gy = ds.groupCountY;
			pass.SetRecordingFunc([program, push, gx, gy](smvk::CommandBuffer& cmd) mutable {
				cmd.BindComputePipeline(program);
				cmd.PushConstants(0, sizeof(push), &push, smvk::ShaderStage::All);
				cmd.Dispatch(gx, gy, 1);
			});
		}
		// Phase 3 â€” Find Lights Accurate (one Shadow Tile per work unit).
		{
			auto& pass = ds.AddPhase3Pass(*m_rendergraph, depthView,
				fpLightsLogical, frameIndex);
			auto program = ds.phasePrograms[1];
			const uint32_t tx = ds.tileCountX;
			const uint32_t ty = ds.tileCountY;
			pass.SetRecordingFunc([program, push, tx, ty](smvk::CommandBuffer& cmd) mutable {
				cmd.BindComputePipeline(program);
				cmd.PushConstants(0, sizeof(push), &push, smvk::ShaderStage::All);
				cmd.Dispatch(tx, ty, 1);
			});
		}
		// Phase 4 â€” Generate Deferred Shadows (samples the spot atlas).
		{
			auto& pass = ds.AddPhase4Pass(*m_rendergraph, depthView,
				fpLightsLogical, m_resources.spotShadowLogical,
				m_spotShadowCount > 0 ? m_viewportResources.spotShadows.logicalView
					: smvk::LogicalImageViewHandle{},
				frameIndex);
			// EVSM SameFrame: phase 4 samples the blurred moments via
			// bindless (not the PCF view hinted above) â€” the hint is the
			// V-blur -> Phase4 edge for the async wait. Lag mode reads the
			// other slot (no same-frame edge).
			if (EvsmActive()) {
				smvk::ImageSyncRequest evsmRead{};
				evsmRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
				evsmRead.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
				evsmRead.layout = smvk::ImageLayout::ShaderReadOnlyOptimal;
				pass.AddImageHint(
					m_viewportResources.evsmSpot.blurredLogical[
						m_viewportResources.evsmSpot.SampleSlot(frameIndex)],
					evsmRead);
			}
			auto program = ds.phasePrograms[2];
			const uint32_t tx = ds.tileCountX;
			const uint32_t ty = ds.tileCountY;
			pass.SetRecordingFunc([program, push, tx, ty](smvk::CommandBuffer& cmd) mutable {
				cmd.BindComputePipeline(program);
				cmd.PushConstants(0, sizeof(push), &push, smvk::ShaderStage::All);
				cmd.Dispatch(tx, ty, 1);
			});
		}
	}

	// ---------------------------------------------------------------------
	// Deferred shadows debug dump (VULKANTOY_DS_DUMP=1; post-Execute one
	// shot, stalls). Prints per-phase results so headless runs can bisect
	// the pipeline: phase 2's TileGroup masks (groups with lights, set
	// bits, group-mask consistency) and phase 3/4's per-tile state (tiles
	// with lights, entry totals, slot classification, center-tile list
	// entries decoded as light index + visibility slot / fully-lit flag).
	// ---------------------------------------------------------------------
	void SceneRenderer::DumpDeferredShadows(uint32_t frameIndex) {
		const smvk::DeferredShadows& ds = m_viewportResources.deferredShadows;
		if (!ds.Valid())
			return;
		const uint32_t tiles = ds.tileCountX * ds.tileCountY;
		const uint32_t groups = ds.groupCountX * ds.groupCountY;
		constexpr uint32_t kEntries = 1 + smvk::kDsMaxLightGroups;

		std::fprintf(stderr,
			"[ds-dump] frame=%u spotTiles=%u mapIdx=%d (retired stats: list=%u vis=%u lit=%u shadow=%u penumbra=%u early=%u final=%u)\n",
			frameIndex, m_spotShadowCount, m_spotShadowMapIndex,
			m_dsStats.listSlots, m_dsStats.visSlots, m_dsStats.clsAllLit,
			m_dsStats.clsAllShadow, m_dsStats.clsPenumbra,
			m_dsStats.sumEarly, m_dsStats.sumFinal);

		// ---- Phase 1 output (CPU Tile Light Masks, phase 2's input) ----
		{
			std::vector<uint32_t> masks(size_t(groups) * kEntries, 0);
			if (m_context->DownloadDataFromBuffer(ds.TileMasksBuffer(frameIndex),
				masks.data(), masks.size() * sizeof(uint32_t))) {
				uint32_t withLights = 0, spotGroups = 0;
				uint64_t totalBits = 0;
				for (uint32_t g = 0; g < groups; ++g) {
					const size_t base = size_t(g) * kEntries;
					uint32_t bits = 0;
					for (uint32_t k = 0; k < smvk::kDsMaxLightGroups; ++k)
						bits += std::popcount(masks[base + 1 + k]);
					if (bits > 0)
						++withLights;
					totalBits += bits;
					// After the near-distance sort the long-range spots sit
					// at sorted indices 0..3 (sort key 0); their presence in
					// the CPU mask isolates phase-1 from the GPU phases.
					if ((masks[base + 1] & 0xFu) != 0)
						++spotGroups;
				}
				std::fprintf(stderr,
					"[ds-dump] phase1 masks: groups=%u withLights=%u bits=%llu spotBits(0..3)=%u\n",
					groups, withLights, (unsigned long long)totalBits, spotGroups);
			}
		}

		// ---- Phase 2 output: TileGroup light masks ----
		{
			std::vector<uint32_t> masks(size_t(groups) * kEntries, 0);
			if (m_context->DownloadDataFromBuffer(ds.TileGroupMasksBuffer(frameIndex),
				masks.data(), masks.size() * sizeof(uint32_t))) {
				uint32_t groupsWithLights = 0;
				uint32_t inconsistent = 0;
				uint32_t spotGroups[4] = {};
				uint64_t totalBits = 0;
				for (uint32_t g = 0; g < groups; ++g) {
					const size_t base = size_t(g) * kEntries;
					uint32_t bits = 0;
					uint32_t recomputed = 0;
					for (uint32_t k = 0; k < smvk::kDsMaxLightGroups; ++k) {
						bits += std::popcount(masks[base + 1 + k]);
						if (masks[base + 1 + k] != 0)
							recomputed |= 1u << k;
					}
					if (bits > 0)
						++groupsWithLights;
					totalBits += bits;
					if (recomputed != masks[base])
						++inconsistent;
					// Sorted indices 0..3 = the spots (see phase-1 note).
					for (uint32_t s = 0; s < 4; ++s)
						if ((masks[base + 1] & (1u << s)) != 0)
							++spotGroups[s];
				}
				std::fprintf(stderr,
					"[ds-dump] phase2 masks: groups=%u withLights=%u bits=%llu inconsistent=%u spotGroups=[%u %u %u %u]\n",
					groups, groupsWithLights,
					(unsigned long long)totalBits, inconsistent,
					spotGroups[0], spotGroups[1], spotGroups[2], spotGroups[3]);
			}
		}

		// ---- Phase 3/4 output: tile state + sampled final entries ----
		{
			std::vector<uint32_t> state(size_t(tiles) * 4, 0);
			if (!m_context->DownloadDataFromBuffer(ds.TileStateBuffer(frameIndex),
				state.data(), state.size() * sizeof(uint32_t))) {
				std::fprintf(stderr, "[ds-dump] tileState download FAILED\n");
				return;
			}
			uint32_t tilesWithLights = 0;
			uint64_t totalEntries = 0;
#if defined(DS_DEBUG_VIZ)
			// Debug builds only: phase 2/3 store the approx/early telemetry
			// under DS_DEBUG_VIZ; release tiles keep zeros here.
			uint64_t totalApprox = 0;
			uint64_t totalEarly = 0;
#endif
			uint32_t maxCount = 0;
			for (uint32_t t = 0; t < tiles; ++t) {
				const uint32_t c = state[t * 4 + 1];
				if (c > (uint32_t)smvk::kDsMaxLightsPerTile) {
					std::fprintf(stderr, "[ds-dump] tile %u: count %u > cap!\n", t, c);
					continue;
				}
				if (c > 0)
					++tilesWithLights;
				totalEntries += c;
#if defined(DS_DEBUG_VIZ)
				totalApprox += state[t * 4 + 2];
				totalEarly += state[t * 4 + 3];
#endif
				maxCount = std::max(maxCount, c);
			}
#if defined(DS_DEBUG_VIZ)
			std::fprintf(stderr,
				"[ds-dump] tiles: %ux%u withLights=%u entries=%llu maxPerTile=%u (approx=%llu early=%llu)\n",
				ds.tileCountX, ds.tileCountY, tilesWithLights,
				(unsigned long long)totalEntries, maxCount,
				(unsigned long long)totalApprox, (unsigned long long)totalEarly);
#else
			std::fprintf(stderr,
				"[ds-dump] tiles: %ux%u withLights=%u entries=%llu maxPerTile=%u\n",
				ds.tileCountX, ds.tileCountY, tilesWithLights,
				(unsigned long long)totalEntries, maxCount);
#endif

			// Sample final entries around screen center.
			std::vector<uint32_t> list;
			const uint32_t samples[3][2] = {
				{ ds.tileCountX / 2, ds.tileCountY / 2 },
				{ ds.tileCountX / 4, ds.tileCountY / 4 },
				{ (ds.tileCountX * 3) / 4, (ds.tileCountY * 3) / 4 },
			};
			for (int s = 0; s < 3; ++s) {
				const uint32_t tx = std::min(samples[s][0], ds.tileCountX - 1);
				const uint32_t ty = std::min(samples[s][1], ds.tileCountY - 1);
			const uint32_t t = ty * ds.tileCountX + tx;
			const uint32_t slot = state[t * 4];
			const uint32_t count = state[t * 4 + 1];
				if (count == 0) {
					std::fprintf(stderr, "[ds-dump] tile(%u,%u): empty\n", tx, ty);
					continue;
				}
				if (slot >= smvk::kDsMaxListUInts) {
					std::fprintf(stderr, "[ds-dump] tile(%u,%u): BAD slot %u\n",
						tx, ty, slot);
					continue;
				}
				const uint32_t n = std::min<uint32_t>(count, 8);
				list.resize(n);
				if (!m_context->DownloadDataFromBuffer(ds.LightListBuffer(frameIndex),
					list.data(), size_t(n) * sizeof(uint32_t),
					size_t(slot) * sizeof(uint32_t))) {
					std::fprintf(stderr, "[ds-dump] tile(%u,%u): list download FAILED\n",
						tx, ty);
					continue;
				}
				std::fprintf(stderr, "[ds-dump] tile(%u,%u): slot=%u count=%u first%u=[",
					tx, ty, slot, count, n);
				for (uint32_t i = 0; i < n; ++i) {
					const uint32_t li = list[i] & 0x3FFu;
					const uint32_t slotInfo = list[i] >> 10;
					const char* kind = (slotInfo == smvk::kDsSlotAllLit) ? "lit"
						: (slotInfo < smvk::kDsMaxVisSlots ? "slot" : "BAD");
					std::fprintf(stderr, "%s %u(%s)%s", i ? " " : "", li, kind,
						li >= 1024u ? "!" : "");
				}
				std::fprintf(stderr, "]\n");
			}
		}
	}

	// Error-driven LOD pick for one instance. Distance comes from the node's
	// world translation to the render camera; the world-space simplifier
	// error of each cooked sibling projects to pixels via
	// LodProjectedPixelError() and the coarsest in-budget LOD wins.
	// Decisions are cached per node in the World and refreshed on a
	// staggered-halves cadence (half the nodes per frame; see World::
	// LodIndexForNode) â€” this is the "make LOD update halfrate" hook.
	uint32_t SceneRenderer::LodIndexForNode(NodeId node, uint32_t worldMesh) const {
		if (!m_lodEnabled || !m_world)
			return 0;
		if (m_world->LodChainLength(worldMesh) <= 1)
			return 0;
		return m_world->LodIndexForNode(node, worldMesh, m_lodFrameInputs);
	}

	smvk::StaticMesh* SceneRenderer::LodMeshForNode(NodeId node, uint32_t worldMesh,
		uint32_t* outLod) {
		if (outLod)
			*outLod = 0;
		smvk::StaticMesh* base = m_world->GetMesh(worldMesh);
		if (!base || !m_lodEnabled)
			return base;
		if (m_world->LodChainLength(worldMesh) <= 1)
			return base;
		const uint32_t lod = m_world->LodIndexForNode(node, worldMesh, m_lodFrameInputs);
		if (outLod)
			*outLod = lod;
		++m_lodDraws;
		if (lod > 0) {
			++m_lodSwitched;
			if (smvk::StaticMesh* m = m_world->GetLodMesh(worldMesh, lod))
				return m;
		}
		return base;
	}

	// Shader-LOD decision for an instance's selected level (see World::
	// LodShaderAt): Lit unless the subsystem is on, the level index hits a
	// cooked chain and the level is the farthest one. Shares the halfrate
	// LOD cache with every geometry consumer, so shading and geometry
	// always resolve off the SAME level within a frame.
	smvk::LodShader SceneRenderer::LodShaderForNode(NodeId node,
		uint32_t worldMesh) const {
		if (!ShaderLodActive() || !m_world)
			return smvk::LodShader::Lit;
		const uint32_t lod = LodIndexForNode(node, worldMesh);
		return m_world->LodShaderAt(worldMesh, lod);
	}

	void SceneRenderer::BuildShadowDrawCandidates() {
		SMVK_PROFILE_RENDER("ShadowDrawCandidates");
		m_shadowCandidates.clear();
		if (!m_world)
			return;
		// One pass over the caster set on the render thread: LOD decisions
		// mutate the World halfrate cache (not thread-safe), and every
		// shadow view consumes the same per-node LOD anyway. Workers get a
		// read-only candidate list; the per-view cull happens later in
		// gfx::BuildShadowList (per cluster, against each view's planes).
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
			| smvk::IteratorFlagBit_Opaque
			, [&](const smvk::World::RenderItem& item) {
				if (item.material == smvk::World::kInvalidIndex)
					return;
				const uint32_t lodIdx = LodIndexForNode(item.node, item.mesh);
				const uint32_t gfxMesh = m_gfxRenderer.GfxMeshForLod(item.mesh, lodIdx);
				if (gfxMesh == UINT32_MAX)
					return; // unfilterable: the pass direct fallback draws it
				smvk::gfx::DrawRequest d{};
				d.mesh = gfxMesh;
				d.material = item.material;
				d.model = m_world->scene.globalTransform[item.node];
				// Draw-set bucketing mirrors FilterMainView: MASK materials
				// draw through the alpha-tested phase, double-sided ones
				// keep both facings (cone culling is off for shadow views
				// anyway, the flag is carried for parity/debugging).
				if (const smvk::Material* mat = m_world->GetMaterial(item.material)) {
					if (mat->isDoubleSided())
						d.flags |= smvk::gfx::DrawFlagBit_TwoSided;
					if (mat->shadingModel == smvk::ShadingModel::Masked)
						d.drawSet = smvk::gfx::DrawSetBit_Masked;
				}
				m_shadowCandidates.push_back(d);
			});
		SMVK_PROFILE_TAG("candidates", m_shadowCandidates.size());
	}

	void SceneRenderer::RecordShadowCastersDirect(smvk::CommandBuffer& cmd,
		smvk::ShaderProgram& opaqueProgram, const PushConstants& pc,
		uint32_t casterViewIndex, bool skipFiltered) {
		auto drawCaster = [&](const smvk::World::RenderItem& item, bool maskedPhase) {
			if (item.material == smvk::World::kInvalidIndex)
				return;
			// Indirect path: registered casters are covered by the per-view
			// command list â€” only unfilterable meshes rasterize here.
			if (skipFiltered
				&& m_gfxRenderer.GfxMeshForLod(item.mesh,
					LodIndexForNode(item.node, item.mesh)) != UINT32_MAX)
				return;
			// Masked (cutout) flag replaces the per-node material fetch the
			// phase split used to pay.
			if (((item.flags & smvk::IteratorFlagBit_Masked) != 0) != maskedPhase)
				return;
			auto& model = m_world->scene.globalTransform[item.node];
			smvk::StaticMesh* meshDesc = LodMeshForNode(item.node, item.mesh);
			if (!meshDesc) return;
			// RenderView plane cull: skip casters outside this view's frustum.
			if (casterViewIndex != smvk::RenderViewSet::kInvalidIndex) {
				glm::vec3 sphereCenter;
				float sphereRadius = 0.0f;
				smvk::MeshBoundsToWorld(
					model, meshDesc->geometry.aabbMin, meshDesc->geometry.aabbMax,
					sphereCenter, sphereRadius);
				if (!smvk::RenderViewIntersectsSphere(
					m_renderViews.Get(casterViewIndex), sphereCenter, sphereRadius))
					return;
			}
			PushConstants objectPC{};
			std::copy_n(glm::value_ptr(model), 16, objectPC.model);
			if (maskedPhase) {
				objectPC.materialId = item.material;
				cmd.PushConstants(offsetof(PushConstants, materialId),
					sizeof(objectPC.materialId), &objectPC.materialId,
					smvk::ShaderStage::All);
			}
			cmd.PushConstants(offsetof(PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
			for (auto& draw_set : meshDesc->geometry.surfaces) {
				cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
			};
		};
		// Phase 1: solid casters, depth-only program.
		cmd.BindRenderPipeline(
			opaqueProgram,
			smvk::VertexFormat::StaticVertexFormat,
			smvk::DepthSettings{
				.depthFunc = smvk::CompareOp::LessEqual,
				.writeEnable = true,
				.depthTestEnable = true,
				.depthBiasEnable = false,
				.depthBiasConstantFactor = 0.0f,
				.depthBiasClamp = 0.0f,
				.depthBiasSlopeFactor = 0.0f
			},
			smvk::StencilSettings{},
			smvk::BlendSettings{},
			smvk::MSAASettings{},
			smvk::RasterSettings{
				.cullMode = smvk::CullMode::Back
			}
		);
		cmd.PushConstants(0, sizeof(PushConstants), &pc, smvk::ShaderStage::All);
		// Opaque only: BLEND transparent must not cast (full-quad depth).
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
			| smvk::IteratorFlagBit_Opaque
			, [&](const smvk::World::RenderItem& item) { drawCaster(item, false); });
		// Phase 2: MASK cutout casters, alpha-tested program so foliage
		// casts leaf-shaped shadows instead of full quads. Cull off: masked
		// foliage is double-sided in the main view.
		cmd.BindRenderPipeline(
			m_resources.shadowMaskedProgram,
			smvk::VertexFormat::StaticVertexFormat,
			smvk::DepthSettings{
				.depthFunc = smvk::CompareOp::LessEqual,
				.writeEnable = true,
				.depthTestEnable = true,
				.depthBiasEnable = false,
				.depthBiasConstantFactor = 0.0f,
				.depthBiasClamp = 0.0f,
				.depthBiasSlopeFactor = 0.0f
			},
			smvk::StencilSettings{},
			smvk::BlendSettings{},
			smvk::MSAASettings{},
			smvk::RasterSettings{
				.cullMode = smvk::CullMode::None
			}
		);
		cmd.PushConstants(0, sizeof(PushConstants), &pc, smvk::ShaderStage::All);
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
			| smvk::IteratorFlagBit_Opaque
			, [&](const smvk::World::RenderItem& item) { drawCaster(item, true); });
	}

	void SceneRenderer::RecordEvsmMomentsDirect(smvk::CommandBuffer& cmd,
		smvk::ShaderProgram& momentsProgram, const PushConstants& pc,
		uint32_t casterViewIndex, bool skipFiltered,
		smvk::ShaderProgram* opaqueProgram) {
		// EVSM direct path: same caster walk as RecordShadowCastersDirect.
		// Phases split by cost â€” phase 1 (opaque) binds the CHEAP moments
		// program when provided (no material read / texture fetch /
		// feedback atomic; per-fragment work is two exp() + the RG32F
		// store, ~the PCF depth-only cost), phase 2 (MASK cutout) binds
		// the full alpha-tested program. Binding the full FS for opaque
		// measured ~30x the PCF pass (base-color sample + TS_ReportUsage
		// atomic at 2xMSAA rates over a 2048^2 tile).
		auto drawCaster = [&](const smvk::World::RenderItem& item, bool maskedPhase) {
			if (item.material == smvk::World::kInvalidIndex)
				return;
			if (skipFiltered
				&& m_gfxRenderer.GfxMeshForLod(item.mesh,
					LodIndexForNode(item.node, item.mesh)) != UINT32_MAX)
				return;
			if (((item.flags & smvk::IteratorFlagBit_Masked) != 0) != maskedPhase)
				return;
			auto& model = m_world->scene.globalTransform[item.node];
			smvk::StaticMesh* meshDesc = LodMeshForNode(item.node, item.mesh);
			if (!meshDesc) return;
			if (casterViewIndex != smvk::RenderViewSet::kInvalidIndex) {
				glm::vec3 sphereCenter;
				float sphereRadius = 0.0f;
				smvk::MeshBoundsToWorld(
					model, meshDesc->geometry.aabbMin, meshDesc->geometry.aabbMax,
					sphereCenter, sphereRadius);
				if (!smvk::RenderViewIntersectsSphere(
					m_renderViews.Get(casterViewIndex), sphereCenter, sphereRadius))
					return;
			}
			PushConstants objectPC{};
			std::copy_n(glm::value_ptr(model), 16, objectPC.model);
			if (maskedPhase) {
				objectPC.materialId = item.material;
				cmd.PushConstants(offsetof(PushConstants, materialId),
					sizeof(objectPC.materialId), &objectPC.materialId,
					smvk::ShaderStage::All);
			}
			cmd.PushConstants(offsetof(PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
			for (auto& draw_set : meshDesc->geometry.surfaces) {
				cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
			};
		};
		const smvk::MSAASettings msaa2x = smvk::MSAASettings::Count2();
		// Phase 1: solid casters (cull back) â€” cheap opaque program.
		cmd.BindRenderPipeline(
			opaqueProgram ? *opaqueProgram : momentsProgram,
			smvk::VertexFormat::StaticVertexFormat,
			smvk::DepthSettings{
				.depthFunc = smvk::CompareOp::LessEqual,
				.writeEnable = true,
				.depthTestEnable = true,
				.depthBiasEnable = false,
				.depthBiasConstantFactor = 0.0f,
				.depthBiasClamp = 0.0f,
				.depthBiasSlopeFactor = 0.0f
			},
			smvk::StencilSettings{},
			smvk::BlendSettings{},
			msaa2x,
			smvk::RasterSettings{
				.cullMode = smvk::CullMode::Back
			}
		);
		cmd.PushConstants(0, sizeof(PushConstants), &pc, smvk::ShaderStage::All);
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
			| smvk::IteratorFlagBit_Opaque
			, [&](const smvk::World::RenderItem& item) { drawCaster(item, false); });
		// Phase 2: MASK cutout casters (cull none, full alpha-tested
		// moments program â€” the FS discards under the cutoff like
		// shadow.frag so foliage casts leaf-shaped moments).
		cmd.BindRenderPipeline(
			momentsProgram,
			smvk::VertexFormat::StaticVertexFormat,
			smvk::DepthSettings{
				.depthFunc = smvk::CompareOp::LessEqual,
				.writeEnable = true,
				.depthTestEnable = true,
				.depthBiasEnable = false,
				.depthBiasConstantFactor = 0.0f,
				.depthBiasClamp = 0.0f,
				.depthBiasSlopeFactor = 0.0f
			},
			smvk::StencilSettings{},
			smvk::BlendSettings{},
			msaa2x,
			smvk::RasterSettings{
				.cullMode = smvk::CullMode::None
			}
		);
		cmd.PushConstants(0, sizeof(PushConstants), &pc, smvk::ShaderStage::All);
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
			| smvk::IteratorFlagBit_Opaque
			, [&](const smvk::World::RenderItem& item) { drawCaster(item, true); });
	}

	void SceneRenderer::ResetLodStats() {
		m_lodDraws = 0;
		m_lodSwitched = 0;
		m_lodShaderDraws = 0;
		m_lodChainMeshes = 0;
		m_lodChainLods = 0;
		if (m_world) {
			for (uint32_t i = 0; i < (uint32_t)m_world->meshLocations.size(); ++i) {
				const uint32_t n = m_world->LodChainLength(i);
				if (n > 1) {
					++m_lodChainMeshes;
					m_lodChainLods += n - 1;
				}
			}
		}
	}

	void SceneRenderer::RefreshGpuMemoryStats(bool force) {
		if (!m_context)
			return;
		const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		if (!force && (now - m_gpuMemLastQuerySec) < 1.0)
			return;
		m_gpuMemLastQuerySec = now;
		const auto t0 = std::chrono::steady_clock::now();
		m_gpuMemHeaps = m_context->GetGpuMemoryInfo();
		const auto t1 = std::chrono::steady_clock::now();
		m_gpuMemQueryUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
	}

	void SceneRenderer::LogGpuMemoryToConsole() {
		RefreshGpuMemoryStats(true);
		if (m_gpuMemHeaps.empty()) {
			std::fprintf(stderr, "[GPU memory] no heap data (headless?)\n");
			fflush(stderr);
			return;
		}
		uint64_t totalUsed = 0, totalBudget = 0;
		for (const auto& h : m_gpuMemHeaps) {
			totalUsed += h.usageBytes;
			totalBudget += h.budgetBytes;
		}
		// Coalesce into ONE stderr write: each fprintf(stderr) flushes
		// separately on Windows (console lock + flush per call), so N
		// calls per snapshot is N times the syscall cost.
		char buf[1024];
		int n = snprintf(buf, sizeof(buf), "[GPU memory] total %.1f / %.1f MiB (%.1f%%)",
			(double)totalUsed / (1024.0 * 1024.0),
			(double)totalBudget / (1024.0 * 1024.0),
			totalBudget > 0 ? 100.0 * (double)totalUsed / (double)totalBudget : 0.0);
		for (size_t i = 0; i < m_gpuMemHeaps.size() && n > 0 && (size_t)n < sizeof(buf); ++i) {
			const auto& h = m_gpuMemHeaps[i];
			n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s heap%zu(%s) %.1f/%.1f MiB %.1f%% (heap %.0f MiB)",
				i ? " |" : ":", i, h.deviceLocal ? "VRAM" : "host",
				(double)h.usageBytes / (1024.0 * 1024.0),
				(double)h.budgetBytes / (1024.0 * 1024.0),
				h.budgetBytes > 0 ? 100.0 * (double)h.usageBytes / (double)h.budgetBytes : 0.0,
				(double)h.heapSizeBytes / (1024.0 * 1024.0));
		}
		if ((size_t)n < sizeof(buf) - 2) {
			buf[n++] = '\n';
			buf[n] = '\0';
		}
		std::fputs(buf, stderr);
		fflush(stderr);
		m_gpuMemLastLogSec = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void SceneRenderer::RebuildSwapViews() {
		for (auto h : m_viewportResources.swapImages) {
			m_rendergraph->FreeImageView(h);
		}
		m_viewportResources.swapImages.clear();

		const uint32_t count = m_swapchain->GetImageCount();
		m_viewportResources.swapImages.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			m_viewportResources.swapImages.push_back(
				m_rendergraph->AddExternalImageView(
					m_swapchain->GetImageViewHandle(i),
					m_swapchain->GetSurfaceFormat(),
					"swap"
				)
			);
		}
		auto vkExt = m_swapchain->GetExtent();
		const smvk::Extent2D swapExtent{ vkExt.width, vkExt.height };
		const smvk::Extent2D renderExtent = ScaledExtent(swapExtent, m_renderScale);
		const smvk::Extent2D uiExtent = ScaledExtent(swapExtent, m_uiScale);
		smvk::DepthDesc rebuildDepthDesc{};
		rebuildDepthDesc.format = m_swapchain->GetDepthFormat();
		m_viewportResources.renderDepth.Ensure(*m_context, *m_rendergraph, renderExtent, rebuildDepthDesc);
		smvk::DepthDesc rebuildHizDepthDesc{};
		rebuildHizDepthDesc.format = m_swapchain->GetDepthFormat();
		rebuildHizDepthDesc.name = "hizDepth";
		rebuildHizDepthDesc.viewName = "hizDepthView";
		rebuildHizDepthDesc.logicalName = "hizDepthLogical";
		m_viewportResources.hizDepth.Ensure(*m_context, *m_rendergraph, renderExtent, rebuildHizDepthDesc);
		m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, renderExtent);
		m_viewportResources.motionVectors.Ensure(*m_context, *m_rendergraph, renderExtent);
		m_viewportResources.motionBlur.Ensure(*m_context, *m_rendergraph, renderExtent, MakeMotionBlurDesc());
		m_viewportResources.ssao.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsaoDesc());
		m_viewportResources.gtao.Ensure(*m_context, *m_rendergraph, renderExtent, MakeGtaoDesc());
		smvk::MsaaDesc rebuildMsaaDesc{};
		rebuildMsaaDesc.depthFormat = m_swapchain->GetDepthFormat();
		m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, renderExtent, rebuildMsaaDesc);
		m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc());
		{
			smvk::DofDesc rebuildDofDesc = MakeDofDesc();
			rebuildDofDesc.hqBokeh = m_dofHqBokeh;
			m_viewportResources.dof.Ensure(*m_context, *m_rendergraph, renderExtent, rebuildDofDesc);
		}
		m_viewportResources.ssr.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsrDesc());
		{
			smvk::VolumetricDesc rebuildVolDesc = MakeVolumetricDesc();
			rebuildVolDesc.steps = (uint32_t)std::clamp(m_volumetricSteps, 4, 128);
			m_viewportResources.volumetric.Ensure(*m_context, *m_rendergraph, renderExtent, rebuildVolDesc);
		}
		smvk::UiDesc rebuildUiDesc{};
		rebuildUiDesc.format = LdrUnormSibling();
		m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, rebuildUiDesc);
		smvk::SmaaDesc rebuildSmaaDesc{};
		rebuildSmaaDesc.ldrFormat = LdrUnormSibling();
		m_viewportResources.smaa.Ensure(*m_context, *m_rendergraph, swapExtent, rebuildSmaaDesc);
		// DLSS output follows the swap extent; the recreated target holds
		// garbage until the next evaluate fills it (tonemap reads it only
		// on frames where the DLSS pass ran). The extent change arms a
		// history reset and the options-drift check re-applies
		// slDLSSSetOptions (the plugin then recreates its NGX feature).
		m_viewportResources.dlss.Ensure(*m_context, *m_rendergraph, swapExtent);
		m_dlssReset = true;
		// Recreated history targets hold garbage - the resolve must not
		// blend against them until the T2x chain refills the ping-pong.
		smvk::SmaaInvalidateHistory(m_smaaT2x);
		UpdateCameraAspect(renderExtent);
	}

	// ---------------------------------------------------------------------
	// Artistic LUT grading (TPP-style 32^3 strip, see tonemap.h). The strip
	// is 1024x32 sRGB-encoded PNG (32 tiles along x, blue = tile, red =
	// column, green = row); Photoshop edits the bytes directly.
	// ---------------------------------------------------------------------
	void SceneRenderer::ScanLutFiles() {
		m_lutFiles.clear();
		std::error_code ec;
		std::filesystem::create_directories("content/luts", ec);
		for (const auto& entry : std::filesystem::directory_iterator("content/luts", ec)) {
			if (!entry.is_regular_file())
				continue;
			if (_stricmp(entry.path().extension().string().c_str(), ".png") != 0)
				continue;
			m_lutFiles.push_back(entry.path().filename().string());
		}
		std::sort(m_lutFiles.begin(), m_lutFiles.end());
	}

	bool SceneRenderer::LoadLut(const std::string& file) {
		std::vector<uint8_t> rgba;
		int w = 0, h = 0;
		const std::string path = "content/luts/" + file;
		if (!LutLoadPng(path.c_str(), rgba, w, h)) {
			std::cerr << "[SceneRenderer] LUT decode failed: " << path << "\n";
			return false;
		}
		if (w != 1024 || h != 32) {
			std::cerr << "[SceneRenderer] LUT " << file << " is " << w << "x" << h
				<< ", expected 1024x32 (32^3 strip)\n";
			return false;
		}

		smvk::ImageCreateInfo ci{};
		ci.extent = { 1024, 32, 1 };
		ci.format = smvk::Format::RGBA8_SRGB; // sRGB view: sampling lands in linear
		ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "colorLut";
		auto tex = m_context->CreateTexture(ci, rgba.data());
		if (tex.empty())
			return false;
		m_lutTexture = std::move(tex); // old texture retires via Holder
		m_lutBindless = m_context->GetTextureBindlessIndex(m_lutTexture);
		m_context->FlushUploads();
		m_lutFile = file;
		std::cout << "[SceneRenderer] LUT loaded: " << file
			<< " (bindless " << m_lutBindless << ")\n";
		return true;
	}

	void SceneRenderer::ExportNeutralLut() {
		// Identity strip, sRGB-encoded bytes (matches common.hlsli SrgbEncode):
		// pixel(i,j,t) = srgbEncode(i/31, j/31, t/31). Editing these bytes in
		// Photoshop and loading the result back gives the grading.
		constexpr int W = 1024, H = 32;
		std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
		const auto srgb8 = [](float c) {
			c = std::min(std::max(c, 0.0f), 1.0f);
			const float lo = c * 12.92f;
			const float hi = 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
			const float v = c <= 0.0031308f ? lo : hi;
			return static_cast<uint8_t>(std::lround(v * 255.0f));
		};
		for (int t = 0; t < 32; ++t) {
			for (int j = 0; j < 32; ++j) {
				for (int i = 0; i < 32; ++i) {
					uint8_t* o = &px[(static_cast<size_t>(j) * W
						+ static_cast<size_t>(t) * 32 + i) * 4];
					o[0] = srgb8(i / 31.0f);
					o[1] = srgb8(j / 31.0f);
					o[2] = srgb8(t / 31.0f);
					o[3] = 255;
				}
			}
		}
		std::error_code ec;
		std::filesystem::create_directories("exports", ec);
		if (LutWritePng("exports/lut_neutral.png", W, H, px.data()))
			std::cout << "[SceneRenderer] neutral LUT strip exported: exports/lut_neutral.png\n";
		else
			std::cerr << "[SceneRenderer] neutral LUT export failed (exports/lut_neutral.png)\n";
		// Seed the editable copy into content/luts once; never overwrite (the
		// user may have graded it already - F7 always refreshes exports/).
		if (!std::filesystem::exists("content/luts/neutral.png", ec))
			LutWritePng("content/luts/neutral.png", W, H, px.data());
	}

	void SceneRenderer::EnsureCaptureTarget(smvk::Extent2D extent) {
		if (!m_captureImage.empty() &&
			m_captureExtent.width == extent.width && m_captureExtent.height == extent.height)
			return;
		if (!m_captureLogical.empty()) {
			m_rendergraph->FreeImageView(m_captureLogical);
			m_captureLogical = {};
		}
		m_captureView = {};
		m_captureImage = {};
		smvk::ImageCreateInfo ci{};
		ci.extent = { extent.width, extent.height, 1 };
		ci.format = smvk::Format::RGBA8_SRGB; // same store path as the swapchain
		ci.usage = smvk::ImageUsage::ColorAttachment | smvk::ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "capture";
		m_captureImage = m_context->CreateImage(ci, nullptr);
		if (m_captureImage.empty())
			return;
		smvk::ImageViewCreateInfo vci{};
		vci.viewType = smvk::ImageViewType::View2D;
		vci.format = smvk::Format::Undefined; // inherit
		vci.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = "captureView";
		m_captureView = m_context->CreateImageView(m_captureImage, vci);
		if (m_captureView.empty()) {
			m_captureImage = {};
			return;
		}
		m_captureLogical = m_rendergraph->AddExternalImageView(
			m_captureView, smvk::Format::RGBA8_SRGB, "captureLogical");
		m_captureExtent = extent;
	}

	void SceneRenderer::ExportScreenshot(smvk::Extent2D extent) {
		if (m_captureImage.empty()) {
			std::cerr << "[SceneRenderer] capture target unavailable - screenshot skipped\n";
			return;
		}
		std::vector<uint8_t> px(static_cast<size_t>(extent.width) * extent.height * 4);
		// The capture pass leaves the image in COLOR_ATTACHMENT_OPTIMAL (the
		// rendergraph does not write sync states back, so the layout is
		// declared explicitly - Undefined would discard the contents).
		if (!m_context->DownloadDataFromImage(m_captureImage,
				smvk::ImageLayout::ColorAttachmentOptimal, px.data(), px.size())) {
			std::cerr << "[SceneRenderer] screenshot readback failed\n";
			return;
		}
		const std::time_t t = std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now());
		std::tm tmv{};
		localtime_s(&tmv, &t);
		char stamp[32];
		std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
		std::error_code ec;
		std::filesystem::create_directories("exports", ec);
		const std::string path = "exports/screenshot_" + std::string(stamp) + ".png";
		if (LutWritePng(path.c_str(), extent.width, extent.height, px.data()))
			std::cout << "[SceneRenderer] screenshot exported: " << path << "\n";
		else
			std::cerr << "[SceneRenderer] screenshot write failed (" << path << ")\n";
	}

	void SceneRenderer::DrawFrame() {
		// Frame-arena scratch (URI probing, normalized rewrites) is
		// dead at frame end by definition â€” recycle the blocks.
		smvk::ResetFrameArena();
		// Presentation-mode change from the overlay combo: recreate with
		// the new mode (same path as a resize).
		if (m_presentMode != m_swapchain->GetPresentMode()) {
			m_swapchain->SetPresentMode(m_presentMode);
			m_swapchain->Recreate();
			RebuildSwapViews();
			m_swapchain->ClearFramebufferResized();
		}
		if (m_swapchain->FramebufferResized()) {
			m_swapchain->Recreate();
			RebuildSwapViews();
			m_swapchain->ClearFramebufferResized();
		}

		const auto frameStart = std::chrono::steady_clock::now();
		auto fb = m_swapchain->AcquireFramebuffer();
		// Busy-only CPU clock: starts after acquire returns, so the
		// in-flight fence + vkAcquireNextImageKHR wait (idle, vsync-paced
		// under FIFO) is excluded from CPU and reported as wait instead.
		const auto busyStart = std::chrono::steady_clock::now();
		UpdateDeltaTime();
		RenderFrame(fb);
		m_swapchain->EndAndPresentFramebuffer(fb);
		// CPU/GPU time split: CPU = busy time on this thread (record +
		// submit); wait = acquire idle + vkQueuePresentKHR throttle
		// (vsync-paced under FIFO); GPU = timestamp query around the
		// command buffer (retired frame, lags ~1 frame).
		const auto frameEnd = std::chrono::steady_clock::now();
		const double presentWaitMs = m_swapchain->GetPresentWaitMs();
		double waitMs = std::chrono::duration<double, std::milli>(
			busyStart - frameStart).count() + presentWaitMs;
		double cpuMs = std::chrono::duration<double, std::milli>(
			frameEnd - busyStart).count() - presentWaitMs;
		if (cpuMs < 0.0)
			cpuMs = 0.0;
		constexpr double kEmaAlpha = 0.08;
		m_cpuMsSmooth += (cpuMs - m_cpuMsSmooth) * kEmaAlpha;
		m_waitMsSmooth += (waitMs - m_waitMsSmooth) * kEmaAlpha;
		m_gpuTimingValid = m_swapchain->HasGpuTiming();
		if (m_gpuTimingValid)
			m_gpuMsSmooth += (m_swapchain->GetGpuFrameMs() - m_gpuMsSmooth) * kEmaAlpha;
		// Benchmark split report (once, alongside App's [perf] avg line;
		// benchFrames_ counts up in MainLoop right after this DrawFrame).
		if (frameBudget_ > 0 && benchFrames_ + 1 >= frameBudget_) {
			std::printf("[perf] cpu %.3f ms wait %.3f ms gpu %s present %s\n", m_cpuMsSmooth, m_waitMsSmooth,
				m_gpuTimingValid ? (std::to_string(m_gpuMsSmooth) + " ms").c_str() : "(warming up)",
				smvk::ToString(m_swapchain->GetActivePresentMode()));
			const double n = (double)(benchFrames_ + 1); // this DrawFrame already accumulated
			std::printf("[perf] phases updates %.3f ms (u/l/m %.3f + stream %.3f + fp %.3f [gen %.3f up %.3f ds %.3f]) passes %.3f ms (preA %.3f preB %.3f preC %.3f postMain %.3f) readbacks %.3f ms ui %.3f ms execute %.3f ms\n",
				s_benchUpdates / n, s_benchUpdA / n, s_benchStream / n, s_benchFp / n,
				s_benchFpGen / n, s_benchFpUp / n, s_benchFpDs / n, s_benchPasses / n,
				s_benchPreA / n, s_benchPreB / n, s_benchPreC / n,
				s_benchPostMain / n, s_benchReadbacks / n,
				s_benchUi / n, s_benchExecute / n);
			std::fflush(stdout);
		}
		// Periodic console snapshot of GPU memory (opt-in, every ~10 s).
		// Same numbers as the ImGui bar; "log to console" button forces
		// one immediately regardless of this toggle.
		if (m_gpuMemConsoleAutoLog && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - m_gpuMemLastLogSec >= 10.0)
			LogGpuMemoryToConsole();
		if (m_captureReadbackPending) {
			// The frame's submit (RenderFrame records, EndAndPresent
			// submits) wrote the capture target; it is now safe to stall
			// for the readback (F7-pressed only).
			m_captureReadbackPending = false;
			WaitForDeviceIdle();
			ExportScreenshot(m_captureExtent);
		}
	}

void SceneRenderer::RenderFrame(smvk::Framebuffer& fb) {
	SMVK_PROFILE_FUNCTION();
	SMVK_PROFILE_TAG("frameIndex", fb.frameIndex);

	m_frameCounter += 1;
	m_rendergraph->NewFrame();
	const smvk::Extent2D swapExtent{ fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height };
	// DLSS owns the internal resolution when requested and runnable: the
	// driver-recommended optimal render size replaces the renderScale
	// slider (the slider stays authoritative on the fallback path).
	const bool dlssUsable = DlssRequested() && smvk::DlssAvailable() && m_dlssSupported
		&& m_passes.motionVectors;
	smvk::Extent2D renderExtent = ScaledExtent(swapExtent, m_renderScale);
	if (dlssUsable) {
		smvk::DlssOptimalSettings dlssOpt{};
		if (smvk::DlssQueryOptimalSettings(m_dlssMode,
				swapExtent.width, swapExtent.height, dlssOpt)) {
			renderExtent = { dlssOpt.renderWidth, dlssOpt.renderHeight };
		} else {
			// Query failed (offline NGX etc.): conventional per-mode ratios
			// (Performance 50% / Balanced 58% / Quality 67% / UltraPerf 33%
			// / UltraQuality 78% / DLAA 100%) keep the feature testable.
			float dlssScale = 1.0f;
			switch (m_dlssMode) {
			case smvk::DlssMode::MaxPerformance: dlssScale = 0.5f; break;
			case smvk::DlssMode::Balanced: dlssScale = 0.578f; break;
			case smvk::DlssMode::MaxQuality: dlssScale = 0.667f; break;
			case smvk::DlssMode::UltraPerformance: dlssScale = 0.333f; break;
			case smvk::DlssMode::UltraQuality: dlssScale = 0.778f; break;
			default: break; // DLAA 1:1
			}
			renderExtent = ScaledExtent(swapExtent, dlssScale);
		}
	}
	// Render-extent changes are worth a line: they resize every render
	// target (DLSS mode/quality changes, renderScale edits, resizes).
	if (renderExtent.width != m_dlssLastRenderExtent.width
		|| renderExtent.height != m_dlssLastRenderExtent.height) {
		std::fprintf(stderr, "[DLSS] render extent %ux%u -> swap %ux%u%s\n",
			renderExtent.width, renderExtent.height,
			swapExtent.width, swapExtent.height,
			dlssUsable ? " (DLSS optimal)" : "");
	}
	const smvk::Extent2D uiExtent = ScaledExtent(swapExtent, m_uiScale);
	UpdateCameraAspect(renderExtent);
	{
		SMVK_PROFILE_SCOPE("SceneRenderer_ResourceEnsure");
		smvk::DepthDesc frameDepthDesc{};
		frameDepthDesc.format = m_swapchain->GetDepthFormat();
		m_viewportResources.renderDepth.Ensure(*m_context, *m_rendergraph, renderExtent, frameDepthDesc);
		smvk::DepthDesc frameHizDepthDesc{};
		frameHizDepthDesc.format = m_swapchain->GetDepthFormat();
		frameHizDepthDesc.name = "hizDepth";
		frameHizDepthDesc.viewName = "hizDepthView";
		frameHizDepthDesc.logicalName = "hizDepthLogical";
		m_viewportResources.hizDepth.Ensure(*m_context, *m_rendergraph, renderExtent, frameHizDepthDesc);
			// Tile counts must be fresh for consumers below; the renderer
			// re-checks sizing at pass-build time (resize-safe).
			m_fpRenderer.EnsureSized(*m_context, *m_rendergraph,
				renderExtent, m_viewportResources.renderDepth.view);
			// Deferred shadows: fixed-size buffers, so Ensure only
			// refreshes the tile/group grids on extent drift.
			m_viewportResources.deferredShadows.Ensure(*m_context,
				*m_rendergraph, renderExtent);
			m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, renderExtent);
			m_viewportResources.motionVectors.Ensure(*m_context, *m_rendergraph, renderExtent);
		m_viewportResources.motionBlur.Ensure(*m_context, *m_rendergraph, renderExtent, MakeMotionBlurDesc());
		m_viewportResources.ssao.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsaoDesc());
		m_viewportResources.gtao.Ensure(*m_context, *m_rendergraph, renderExtent, MakeGtaoDesc());
		smvk::MsaaDesc frameMsaaDesc{};
		frameMsaaDesc.depthFormat = m_swapchain->GetDepthFormat();
		m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, renderExtent, frameMsaaDesc);
		m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc());
		{
			smvk::DofDesc frameDofDesc = MakeDofDesc();
			frameDofDesc.hqBokeh = m_dofHqBokeh;
			m_viewportResources.dof.Ensure(*m_context, *m_rendergraph, renderExtent, frameDofDesc);
		}
		m_viewportResources.ssr.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsrDesc());
		{
			smvk::VolumetricDesc rebuildVolDesc = MakeVolumetricDesc();
			rebuildVolDesc.steps = (uint32_t)std::clamp(m_volumetricSteps, 4, 128);
			m_viewportResources.volumetric.Ensure(*m_context, *m_rendergraph, renderExtent, rebuildVolDesc);
		}
		smvk::UiDesc frameUiDesc{};
		frameUiDesc.format = LdrUnormSibling();
		m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, frameUiDesc);
		m_viewportResources.uiComposite.Ensure(*m_context);
		smvk::SmaaDesc frameSmaaDesc{};
		frameSmaaDesc.ldrFormat = LdrUnormSibling();
		// Silent target recreation (extent/format drift outside
		// RebuildSwapViews) loses history content: invalidate first so the
		// next T2x record warms up instead of blending against garbage.
		// RebuildSwapViews invalidates unconditionally; this guard covers
		// any other drift path (including first creation).
		if (!m_viewportResources.smaa.TargetsValid()
			|| m_viewportResources.smaa.extent.width != swapExtent.width
			|| m_viewportResources.smaa.extent.height != swapExtent.height
			|| m_viewportResources.smaa.ldr.format != frameSmaaDesc.ldrFormat)
			smvk::SmaaInvalidateHistory(m_smaaT2x);
		m_viewportResources.smaa.Ensure(*m_context, *m_rendergraph, swapExtent, frameSmaaDesc);
		// AA mode combo (Off / MSAA 2x / SMAA 1x / SMAA T2x): MSAA runs
		// only in its own mode (SMAA after an MSAA resolve wastes the
		// resolve; device support is still gated at the Main pass).
		// DLSS replaces all AA when active.
		m_viewportResources.msaaEnabled = (m_aaMode == AaMode::Msaa2x) && !dlssUsable;
			m_viewportResources.spotShadows.Ensure(*m_context, *m_rendergraph, MakeSpotShadowDesc());
			// EVSM atlases follow the pipeline-mode member so flipping
			// m_evsmPipeline (OneFrameLag <-> SameFrame) takes effect on
			// the next frame without a restart (Ensure is a no-op in
			// steady state; shaders persist across the flip).
			m_viewportResources.evsmDir.Ensure(*m_context, *m_rendergraph, MakeEvsmDirDesc(m_evsmPipeline));
			m_viewportResources.evsmSpot.Ensure(*m_context, *m_rendergraph, MakeEvsmSpotDesc(m_evsmPipeline));
			// Soft shadows: the softness knob drives the PREFILTER width â€”
			// the dominant visible softness (stride-dilated Gaussian; the
			// per-receiver solar/light-size disk in evsm.hlsli adds the
			// variable-penumbra gradient on top). blurRadius is not part
			// of the Ensure recreate key, so mutating it per frame is free.
			{
				const uint32_t blurR = static_cast<uint32_t>(
					std::clamp(2 + int(m_evsmSoftness * 2.0f), 2, 8));
				m_viewportResources.evsmDir.blurRadius = blurR;
				m_viewportResources.evsmSpot.blurRadius = blurR;
			}
			m_viewportResources.exposure.Ensure(*m_context, *m_rendergraph, MakeExposureDesc());
			// DLSS upscaler targets (HDR output at swap extent + 1x1
			// exposure scalar): swap extent, NOT renderExtent.
			m_viewportResources.dlss.Ensure(*m_context, *m_rendergraph, swapExtent);
		}
		// DLSS record snapshot (after Ensure so Valid() matches the
		// pass-emission gate below): jitter phase + offset in render
		// pixels, consumed by UpdateFrameUniforms + the DLSS pass.
		m_dlssFrameActive = DlssActive();
		m_dlssPhaseCount = m_dlssFrameActive
			? smvk::DlssPhaseCount(renderExtent.width, renderExtent.height,
				swapExtent.width, swapExtent.height)
			: 8u;
		m_dlssJitter = m_dlssFrameActive
			? smvk::DlssComputeJitter(m_dlssPhase, m_dlssPhaseCount)
			: smvk::DlssJitter{};
		// slDLSSSetOptions drift tracking: (re)apply when the mode or the
		// output size changed, release plugin resources when DLSS stops
		// running (mode=eOff alone would keep the VRAM allocated).
		if (m_dlssFrameActive) {
			if (!m_dlssOptionsApplied || m_dlssOptionsMode != m_dlssMode
				|| m_dlssOptionsW != swapExtent.width || m_dlssOptionsH != swapExtent.height) {
				if (smvk::DlssApplyOptions(m_dlssMode, swapExtent.width, swapExtent.height)) {
					m_dlssOptionsApplied = true;
					m_dlssOptionsMode = m_dlssMode;
					m_dlssOptionsW = swapExtent.width;
					m_dlssOptionsH = swapExtent.height;
				} else {
					// Options failed this record: skip the pass so tonemap
					// keeps the (bilinear) renderScale path this frame.
					m_dlssFrameActive = false;
				}
			}
		} else if (m_dlssOptionsApplied) {
			// Toggle-off: prior frames' slEvaluateFeature dispatches may
			// still be in flight (2 frames in flight). Freeing NGX state
			// underneath them loses the device (VK_CHECK abort). Stall
			// once here â€” toggle-off is rare â€” so slFreeResources runs
			// GPU-idle. Preset changes never free, hence never crashed.
			WaitForDeviceIdle();
			smvk::DlssDeactivate(m_dlssOptionsW, m_dlssOptionsH);
			m_dlssOptionsApplied = false;
			m_dlssOptionsW = 0;
			m_dlssOptionsH = 0;
			m_dlssPhase = 0;
			m_dlssReset = true; // next activation warms up
		}
		// Exposure applied this frame (debug views: fixed reference â€” see
		// FrameExposure; 1-frame adaptation lag in the Lit view).
		const float frameExposure = FrameExposure();
		// Exposure-aware bloom: the threshold/knee are authored for the
		// reference (manual) exposure; scale them so bloom strength stays
		// constant as auto-exposure shifts. Applied after Ensure (which
		// resets desc to MakeBloomDesc) and before the bloom passes below.
		if (m_viewportResources.bloom.Valid() && TONEMAP_EXPOSURE > 0.0f) {
			const float bloomScale = frameExposure / TONEMAP_EXPOSURE;
			m_viewportResources.bloom.desc.threshold = BLOOM_THRESHOLD * bloomScale;
			m_viewportResources.bloom.desc.knee = BLOOM_KNEE * bloomScale;
		}
		m_context->UpdateDescriptors();
		// Texture-streaming feedback slice for this record. Unconditional:
		// materials always declare the binding, and update-after-bind keeps
		// the rewrite visible to this frame's commands. Without a streamer
		// the writes are simply never consumed.
		m_context->BindTextureFeedbackSlice(fb.frameIndex);
		// SMAA T2x global toggle (see SmaaT2xState in smaa.h): one phase
		// for the recorded-frame sequence, advanced once per T2x record.
		// Any AA-mode change invalidates the history (a T2x -> 1x/Off ->
		// T2x round-trip would otherwise reuse ancient history).
		if (m_aaMode != m_smaaLastMode) {
			smvk::SmaaInvalidateHistory(m_smaaT2x);
			m_smaaLastMode = m_aaMode;
		}
		// Global T2x snapshot for this record: the jitter below, the
		// history slot + subsample indices of the SMAA chain and the overlay
		// all read this one snapshot so they always agree. Evaluated after
		// Ensure so SmaaActive() matches the chain-emission gate below (the
		// first record after (re)creation degrades to a warmup record).
		m_smaaFrameParams = smvk::SmaaBeginFrame(m_smaaT2x, SmaaT2xActive());
		m_smaaFrameRanT2x = false; // set where the T2x chain is emitted
		// Bench phase timers (active only with --bench past warmup).
		using bench_clk = std::chrono::steady_clock;
		const bool benchPhase = frameBudget_ > 0 && benchWarmup_ >= 10;
		const auto rfT0 = bench_clk::now();
		bench_clk::time_point rfT2, rfT3; // assigned in the exposure block below
		bench_clk::time_point rfPA, rfPB; // preMain split: fw+/filter | cull/late
		// Decal ring upload BEFORE UpdateFrameUniforms: it reads the live
		// count (m_decalFrameCount) into FrameUniforms.
		(void)UpdateDecals(fb.frameIndex);
		const FrameUniforms frameData = UpdateFrameUniforms(fb.frameIndex, renderExtent, swapExtent);
		const smvk::GpuDirectionalLightDescription dirLight = UpdateLights(fb.frameIndex);
		UpdateMaterials(fb.frameIndex);
		const auto rfT0b = bench_clk::now();
		UpdateTextureStreaming(frameData, renderExtent);
		const auto rfT0c = bench_clk::now();
		const uint32_t fpLightCount = UpdateForwardPlusLights(fb.frameIndex, frameData);
		const auto rfT1 = bench_clk::now();

		// ---------------------------------------------------------------------
		// RenderView allocation (see smvk::RenderViewSet):
		//   main camera            -> 1 view,
		//   directional sun        -> 1 view per shadow cascade (4),
		//   shadowed spot lights   -> 1 view per selected atlas tile.
		// Indices are stable for the frame; the GPU slab is indexed through
		// PushConstants::renderViews + the per-pass renderViewIndex.
		// ---------------------------------------------------------------------
		m_renderViews.NewFrame();
		const uint32_t mainRenderView = m_renderViews.AllocateMainView(
			frameData.viewProj,
			{ static_cast<float>(renderExtent.width), static_cast<float>(renderExtent.height) },
			kMainViewCullGuardPx);
		const uint32_t cascadeViewBase = m_renderViews.AllocateDirectionalLightViews(
			dirLight,
			{ static_cast<float>(CASCADE_RES), static_cast<float>(CASCADE_RES) });
		uint32_t spotViewBase = smvk::RenderViewSet::kInvalidIndex;
		for (const smvk::GpuCascadeDescription& tile : m_spotShadowTiles) {
			const uint32_t view = m_renderViews.AllocateSpotShadowView(
				tile, { static_cast<float>(SPOT_SHADOW_RES), static_cast<float>(SPOT_SHADOW_RES) });
			if (spotViewBase == smvk::RenderViewSet::kInvalidIndex)
				spotViewBase = view;
		}
		m_renderViews.Upload(*m_context, fb.frameIndex);
		const uint64_t renderViewsAddress =
			m_renderViews.DeviceAddress(*m_context, fb.frameIndex);

		// Shared frame context for IFrameRenderers (filled incrementally;
		// pc-dependent fields arrive once pc is built below).
		AppFrameContext fc{};
		fc.ctx = m_context.get();
		fc.rg = m_rendergraph.get();
		fc.world = m_world.get();
		fc.views = &m_renderViews;
		fc.frameIndex = fb.frameIndex;
		fc.mainViewIndex = mainRenderView;
		fc.eye = m_cameraPositioner.cameraPosition_;
		fc.extent = renderExtent;
		// Error-driven LOD frame inputs (shared with every consumer: the
		// prepass, main pass and the gfx filter all read the same struct;
		// the World's halfrate parity cache keys on frameId).
		ResetLodStats();
		m_lodFrameId = m_lodFrameId + 1; // monotonic (fb.frameIndex wraps)
		m_lodViewportHeight = static_cast<float>(renderExtent.height);
		m_lodFrameInputs.frameId = m_lodFrameId;
		m_lodFrameInputs.eye = m_cameraPositioner.cameraPosition_;
		m_lodFrameInputs.viewportHeight = m_lodViewportHeight;
		m_lodFrameInputs.fovY = glm::radians(kCameraFovYDegrees);
		m_lodFrameInputs.pixelBudget = m_lodPixelBudget;
		if (m_world && m_lodBudgetApplied != m_lodPixelBudget) {
			m_world->InvalidateLodCache(); // budget tweak: refresh all nodes
			m_lodBudgetApplied = m_lodPixelBudget;
		}
		fc.lodEnabled = m_lodEnabled;
		fc.lodInputs = m_lodFrameInputs;
		// Shader LODs ride the same frame inputs; the gfx filter excludes
		// vertex-lit instances so the Main direct leg shades them (see
		// FilterMainView â€” both sides must observe the same decision).
		fc.lodShaderEnabled = ShaderLodActive();
		fc.ditheredTransparency =
			(m_transparentMode == smvk::TransparentMode::Dithered);

		// Per-view triangle filter (main camera): appended below, after
		// fc.pc is built (the temporal seed pass captures frame addresses)
		// but before the Forward+ prepass (seed writes depth first).
		// Shadow/depth passes stay on direct draws; motion draws static
		// filtered geometry via the HZB-culled indirect list (moved nodes
		// fall back to direct draws with correct prevModel).
		uint32_t gfxSlot = smvk::RenderViewSet::kInvalidIndex;
		//
		std::vector<smvk::LogicalImageViewHandle> colors;
		colors.push_back(m_viewportResources.swapImages[fb.imageIndex]);
		// HDR target for scene â€“ intermediate before tonemap
		std::vector<smvk::LogicalImageViewHandle> hdrColors;
		hdrColors.push_back(m_viewportResources.hdr.logicalView);

		uint64_t frameUniformsAddress =
			m_context->GetBufferDeviceAddress(m_resources.frameUniforms)
			+ fb.frameIndex * sizeof(FrameUniforms);

		constexpr size_t LIGHT_FRAME_STRIDE =
			sizeof(smvk::Light) * MAX_LIGHTS;

		uint64_t lightsBufferAddress =
			m_context->GetBufferDeviceAddress(m_resources.lightsBuffer)
			+ fb.frameIndex * LIGHT_FRAME_STRIDE;

		constexpr size_t MATERIAL_FRAME_STRIDE =
			sizeof(smvk::MaterialValues) * MAX_MATERIALS;

		uint64_t materialsAddress =
			m_context->GetBufferDeviceAddress(m_resources.materialsBuffer)
			+ fb.frameIndex * MATERIAL_FRAME_STRIDE;

		constexpr size_t DIR_FRAME_STRIDE =
			sizeof(smvk::GpuDirectionalLightDescription);
		uint64_t cascadeBufferAddress =
			m_context->GetBufferDeviceAddress(m_resources.directionalCascadeBuffer)
			+ fb.frameIndex * DIR_FRAME_STRIDE;

		constexpr size_t FP_LIGHT_FRAME_STRIDE =
			sizeof(smvk::ForwardPlusLight) * FP_MAX_LIGHTS;
		uint64_t fpLightsAddress =
			m_context->GetBufferDeviceAddress(m_resources.fpLightsBuffer)
			+ fb.frameIndex * FP_LIGHT_FRAME_STRIDE;
		constexpr size_t SPOT_FRAME_STRIDE =
			sizeof(smvk::GpuCascadeDescription) * smvk::kMaxSpotShadows;
		uint64_t spotShadowBufferAddress =
			m_context->GetBufferDeviceAddress(m_resources.spotShadowBuffer)
			+ fb.frameIndex * SPOT_FRAME_STRIDE;
		const auto& fp = m_viewportResources.forwardPlus;
		const uint64_t fpTileCountsAddress = fp.TileCountsAddress(*m_context);
		const uint64_t fpTileIndicesAddress = fp.TileIndicesAddress(*m_context);
		const uint32_t fpTileCountX = fp.tileCountX;
		const uint32_t fpTileCountY = fp.tileCountY;

		// Scene push (see App/push_constants.h; must match HLSL common.hlsli).
		PushConstants pc{
			.frameUniforms = frameUniformsAddress,
			.lightsBuffer = lightsBufferAddress,
			.materialsBuffer = materialsAddress,
			.cascadeBuffer = cascadeBufferAddress,
			.materialId = 0, // defaultMaterial
			.comparisonSamplerId = 1,
			.numCascades = NUM_CASCADES,
			.cascadeIndex = 0,
			.fpLights = fpLightsAddress,
			.fpTileCounts = fpTileCountsAddress,
			.fpTileIndices = fpTileIndicesAddress,
			.fpTileCountX = fpTileCountX,
			.fpLightCount = fpLightCount,
			.fpTileSize = FP_TILE_SIZE,
			.fpMaxPerTile = FP_MAX_PER_TILE,
			.spotShadowBuffer = spotShadowBufferAddress,
			.spotShadowMapIndex = m_spotShadowMapIndex >= 0 ? (uint32_t)m_spotShadowMapIndex : UINT32_MAX,
			.spotShadowCount = m_spotShadowCount,
		.spotIndex = UINT32_MAX, // not a spot shadow pass
		// Transparency technique for Blend materials: 0 = dithered
		// screen-door (default, opaque state), 1 = classic alpha blend.
		.pad_pc = (m_transparentMode == smvk::TransparentMode::Blended) ? 1u : 0u,
			.renderViews = renderViewsAddress,
			.mainRenderView = mainRenderView,
			.renderViewIndex = mainRenderView, // scene passes default to the main camera view
};
	// (size checked in App/push_constants.h)

	// ---- Cascaded shadow map passes â€“ one per cascade tile (builder owns offsets/extents/load ops) ----
	// Pass-config gate: no shadow sampling without tiles (the main
	// shader reads cascadeBuffer regardless; tiles just stay cleared).
	//
	// Indirect path (gfx filter on): caster candidates are prebuilt once
	// per frame on the render thread (LOD decisions mutate the World's
	// halfrate cache â€” not thread-safe), each tile's command list is
	// built cooperatively on a worker while the main thread records the
	// rest of the frame, and the tile pass records two indirect phases
	// (opaque multi-draw + per-material masked ranges; see GfxRenderer::
	// RecordShadowList). Unregistered casters keep the direct per-item
	// recording inside the same pass as a fallback.
	const bool spotPassesOn = m_passes.spotShadows && m_spotShadowCount > 0
		&& m_viewportResources.spotShadows.Valid();
	const uint32_t spotSlotCount = spotPassesOn
		? std::min(m_spotShadowCount, m_viewportResources.spotShadows.slotCount)
		: 0;
	// EVSM gates: moment passes replace the depth passes tile-for-tile, and
	// the indirect GPU path stays on the depth-only VS (its FS moments
	// variant is a follow-up), so EVSM forces the direct recording path.
	const bool evsmDirOn = EvsmActive() && m_passes.directionalShadows
		&& m_viewportResources.directionalShadows.Valid();
	const bool evsmSpotOn = EvsmActive() && spotPassesOn;
	const bool shadowIndirect = !evsmDirOn && !evsmSpotOn
		&& (m_passes.directionalShadows || spotSlotCount > 0)
		&& m_passes.gfxFilter && m_useGfxFilter
		&& m_gfxRenderer.EnsureShadowSeeds(*m_context);
	const bool cascadeViewsValid =
		m_passes.directionalShadows
		&& cascadeViewBase != smvk::RenderViewSet::kInvalidIndex;
	const bool spotViewsValid = spotSlotCount > 0
		&& spotViewBase != smvk::RenderViewSet::kInvalidIndex;
	uint32_t cascadeShadowSlot[NUM_CASCADES];
	uint32_t spotShadowSlot[smvk::kMaxSpotShadows];
	std::fill_n(cascadeShadowSlot, NUM_CASCADES, UINT32_MAX);
	std::fill_n(spotShadowSlot, smvk::kMaxSpotShadows, UINT32_MAX);
	if (shadowIndirect && (cascadeViewsValid || spotViewsValid))
	{
		SMVK_PROFILE_CATEGORY("ShadowListDispatch", Optick::Category::Rendering);
		BuildShadowDrawCandidates();
		SMVK_PROFILE_TAG("candidates", m_shadowCandidates.size());
		// Claim ALL shadow view slots before dispatching any build: the
		// slot vector must not grow while workers hold element refs
		// (gfx::ShadowView threading contract).
		m_gfxRenderer.BeginShadowFrame();
		if (cascadeViewsValid)
			for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci)
				cascadeShadowSlot[ci] = m_gfxRenderer.BeginShadowView(
					*m_context, *m_rendergraph,
					m_renderViews.Get(cascadeViewBase + ci),
					smvk::gfx::CullFlagBit_Frustum | smvk::gfx::CullFlagBit_Degenerate,
					CASCADE_RES, CASCADE_RES);
		if (spotViewsValid)
			for (uint32_t si = 0; si < spotSlotCount; ++si)
				spotShadowSlot[si] = m_gfxRenderer.BeginShadowView(
					*m_context, *m_rendergraph,
					m_renderViews.Get(spotViewBase + si),
					smvk::gfx::CullFlagBit_Frustum | smvk::gfx::CullFlagBit_Degenerate,
					SPOT_SHADOW_RES, SPOT_SHADOW_RES);
		// Cooperative per-view list builds: workers overlap the rest of
		// the frame's pass recording; futures join before Execute.
		if (!m_shadowWorkers) {
			unsigned hw = std::thread::hardware_concurrency();
			if (hw <= 1) hw = 2;
			m_shadowWorkers =
				std::make_unique<smvk::ThreadPool>(std::min(hw - 1, 8u));
		}
		auto enqueueBuild = [this](uint32_t slot) {
			m_shadowFutures.push_back(m_shadowWorkers->enqueue([this, slot] {
				SMVK_PROFILE_RENDER("GfxShadowListBuild");
				for (const smvk::gfx::DrawRequest& cand : m_shadowCandidates)
					m_gfxRenderer.PushShadowDraw(slot, cand.mesh, cand.model,
						cand.material, cand.flags, cand.drawSet);
				m_gfxRenderer.BuildShadowList(slot);
			}));
		};
		for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci)
			if (cascadeShadowSlot[ci] != UINT32_MAX)
				enqueueBuild(cascadeShadowSlot[ci]);
		for (uint32_t si = 0; si < spotSlotCount; ++si)
			if (spotShadowSlot[si] != UINT32_MAX)
				enqueueBuild(spotShadowSlot[si]);
	}
	if (m_passes.directionalShadows)
	{
		SMVK_PROFILE_CATEGORY("ShadowCascades", Optick::Category::Rendering);
		SMVK_PROFILE_TAG("cascadeCount", NUM_CASCADES);

		const char* cascadeNames[4] = { "ShadowCascade0", "ShadowCascade1", "ShadowCascade2", "ShadowCascade3" };
		for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci) {
			SMVK_PROFILE_SCOPE_COLOR("ShadowCascade", Optick::Color::OrangeRed);
			SMVK_PROFILE_TAG("cascadeIndex", ci);
			PushConstants cascadePC = pc;
			cascadePC.cascadeIndex = ci;
			// Each cascade renders from its own allocated RenderView.
			const uint32_t casterViewIndex =
				cascadeViewBase != smvk::RenderViewSet::kInvalidIndex
				? cascadeViewBase + ci
				: smvk::RenderViewSet::kInvalidIndex;
			cascadePC.renderViewIndex = casterViewIndex;
			if (evsmDirOn) {
				// EVSM moment pass (RG32F 2xMSAA + resolve, direct path).
				const char* evsmNames[4] = { "EvsmCascade0", "EvsmCascade1", "EvsmCascade2", "EvsmCascade3" };
				auto& momentPass = smvk::EvsmAsyncPrefilter::AddMomentPass(
					*m_rendergraph, m_viewportResources.evsmDir, ci, fb.frameIndex, evsmNames[ci]);
				momentPass.SetRecordingFunc([this, cascadePC, casterViewIndex](smvk::CommandBuffer& cmd)
				{
					RecordEvsmMomentsDirect(cmd,
						m_viewportResources.evsmDir.momentsProgram,
						cascadePC, casterViewIndex, /*skipFiltered=*/false,
						&m_viewportResources.evsmDir.momentsOpaqueProgram);
				});
				(void)momentPass;
				continue;
			}
			auto& shadowPass = m_viewportResources.directionalShadows.AddCascadePass(*m_rendergraph, ci, cascadeNames[ci]);
			if (shadowIndirect && cascadeShadowSlot[ci] != UINT32_MAX) {
				const uint32_t slot = cascadeShadowSlot[ci];
				m_gfxRenderer.AddShadowConsumeHints(shadowPass, slot);
				shadowPass.SetRecordingFunc([this, slot, casterViewIndex, cascadePC, renderViewsAddress](smvk::CommandBuffer& cmd)
				{
					// GPU context + pass scope come from RenderGraph::Execute
					// (pass name "ShadowCascade<ci>" carries the cascade index).
					m_gfxRenderer.RecordShadowList(cmd, *m_context, slot,
						renderViewsAddress, casterViewIndex, cascadePC);
					// Fallback: unregistered casters only (registered ones
					// are covered by the indirect command list).
					RecordShadowCastersDirect(cmd,
						m_viewportResources.directionalShadows.program,
						cascadePC, casterViewIndex, /*skipFiltered=*/true);
				});
			}
			else {
				shadowPass.SetRecordingFunc([this, cascadePC, casterViewIndex](smvk::CommandBuffer& cmd)
				{
					RecordShadowCastersDirect(cmd,
						m_viewportResources.directionalShadows.program,
						cascadePC, casterViewIndex, /*skipFiltered=*/false);
				});
			}
			(void)shadowPass;
		}
	}

		// ---- Spot shadow passes â€“ one per selected caster tile ----
		// Selection (upper limit + screenspace/distance heuristic) ran in
		// UpdateForwardPlusLights; m_spotShadowCount tiles are valid. Each
		// pass renders its tile with the spot perspective VP (RenderView
		// path; the legacy shadow.vert spotIndex path stays for the direct
		// fallback). Opaque + MASK phases mirror the cascade passes. View
		// slots were claimed together with the cascades above so the slot
		// vector never grows while the cooperative builds are in flight.
		if (spotPassesOn) {
			const uint32_t spotCount = spotSlotCount;
			const char* spotNames[8] = { "SpotShadow0", "SpotShadow1", "SpotShadow2", "SpotShadow3",
				"SpotShadow4", "SpotShadow5", "SpotShadow6", "SpotShadow7" };
			static_assert(sizeof(spotNames) / sizeof(spotNames[0]) >= smvk::kMaxSpotShadows);

			SMVK_PROFILE_CATEGORY("SpotShadows", Optick::Category::Rendering);
			SMVK_PROFILE_TAG("spotCount", spotCount);

			for (uint32_t si = 0; si < spotCount; ++si) {
				SMVK_PROFILE_SCOPE_COLOR("SpotShadow", Optick::Color::Orange);
				SMVK_PROFILE_TAG("spotIndex", si);
				PushConstants spotPC = pc;
				spotPC.spotIndex = si;
				// Each spot tile renders from its own allocated RenderView.
				const uint32_t casterViewIndex =
					spotViewBase != smvk::RenderViewSet::kInvalidIndex
					? spotViewBase + si
					: smvk::RenderViewSet::kInvalidIndex;
				spotPC.renderViewIndex = casterViewIndex;
				if (evsmSpotOn) {
					// EVSM moment pass (RG32F 2xMSAA + resolve, direct path).
					auto& momentPass = smvk::EvsmAsyncPrefilter::AddMomentPass(
						*m_rendergraph, m_viewportResources.evsmSpot, si, fb.frameIndex, spotNames[si]);
					momentPass.SetRecordingFunc([this, spotPC, casterViewIndex](smvk::CommandBuffer& cmd)
					{
						RecordEvsmMomentsDirect(cmd,
							m_viewportResources.evsmSpot.momentsProgram,
							spotPC, casterViewIndex, /*skipFiltered=*/false,
							&m_viewportResources.evsmSpot.momentsOpaqueProgram);
					});
					(void)momentPass;
					continue;
				}
				auto& spotPass = m_viewportResources.spotShadows.AddSpotPass(*m_rendergraph, si, spotNames[si]);
				if (shadowIndirect && spotShadowSlot[si] != UINT32_MAX) {
					const uint32_t slot = spotShadowSlot[si];
					m_gfxRenderer.AddShadowConsumeHints(spotPass, slot);
					spotPass.SetRecordingFunc([this, slot, casterViewIndex, spotPC, renderViewsAddress](smvk::CommandBuffer& cmd)
					{
						// GPU context + pass scope come from RenderGraph::Execute
						// (pass name "SpotShadow<si>" carries the spot index).
						m_gfxRenderer.RecordShadowList(cmd, *m_context, slot,
							renderViewsAddress, casterViewIndex, spotPC);
						// Fallback: unregistered casters only (registered ones
						// are covered by the indirect command list).
						RecordShadowCastersDirect(cmd,
							m_viewportResources.spotShadows.program,
							spotPC, casterViewIndex, /*skipFiltered=*/true);
					});
				}
				else {
					spotPass.SetRecordingFunc([this, spotPC, casterViewIndex](smvk::CommandBuffer& cmd)
					{
						RecordShadowCastersDirect(cmd,
							m_viewportResources.spotShadows.program,
							spotPC, casterViewIndex, /*skipFiltered=*/false);
					});
				}
				(void)spotPass;
			}
		}

		// ---- EVSM prefilter (all tiles, after all moment passes) ----
		// Separable H+V blur per tile, AsyncCompute-marked. Recording ALL
		// blurs after ALL moments keeps the async window free of graphics
		// passes, which is what lets RenderGraph::Execute split the frame
		// into G1/C/G2 on the true-async compute queue (an interleaved
		// graphics write inside the window forces the emulated single-
		// submit fallback). SameFrame consumes this frame's blur;
		// OneFrameLag trails by one slot (see EvsmAtlas::SampleSlot).
		if (evsmDirOn) {
			for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci)
				smvk::EvsmAsyncPrefilter::AddTileBlurPasses(
					*m_rendergraph, m_viewportResources.evsmDir, *m_context,
					fb.frameIndex, ci, /*samplerIndex=*/0);
		}
		if (evsmSpotOn) {
			for (uint32_t si = 0; si < spotSlotCount; ++si)
				smvk::EvsmAsyncPrefilter::AddTileBlurPasses(
					*m_rendergraph, m_viewportResources.evsmSpot, *m_context,
					fb.frameIndex, si, /*samplerIndex=*/0);
		}

		// ---- Forward+ passes (depth prepass + tile cull) ----
		// Owned by ForwardPlusRenderer (see App/forwardplus_renderer.h).
		rfPA = bench_clk::now();
		fc.pc = pc;
		fc.depthView = m_viewportResources.renderDepth.logicalView;
		fc.depthViewHandle = m_viewportResources.renderDepth.view;
		// Option A: HZB source handles (empty when the occluder path is
		// off; AppendOccluderPrepass then no-ops and HZB stays on depth).
		fc.hizDepthView = m_hizOccluderDepth
			? m_viewportResources.hizDepth.logicalView
			: smvk::LogicalImageViewHandle{};
		fc.hizDepthViewHandle = m_hizOccluderDepth
			? m_viewportResources.hizDepth.view
			: smvk::ImageViewHandle{};
		fc.hizDepthBindless = m_hizOccluderDepth
			? m_viewportResources.hizDepth.bindlessIndex
			: UINT32_MAX;
		m_fpRenderer.occluderMinPixels = m_hizOccluderMinPixels;
		m_fpRenderer.occluderMinRadius = m_hizOccluderMinRadius;
		fc.fpLights = m_resources.fpLightsLogical;
		fc.fpLightCount = fpLightCount;
		fc.decals = m_resources.decalsLogical;
		fc.decalCount = m_decalFrameCount;

		// ---- GPU-driven filter + temporal seed (before depth prepass) ----
		// Runs after fc.pc/depthView are built (the seed pass captures
		// frame addresses) but before the prepass (seed writes depth first).
		// gfxSlot stays invalid when the filter is off so stale slots from
		// earlier frames can never leak into consume hints or debug views.
		gfxSlot = smvk::RenderViewSet::kInvalidIndex;
		// Debug culling freeze (see members): capture the culling viewpoint
		// once on the rising edge; the render camera keeps moving while
		// batch expansion + the filter CS reuse the frozen (view, eye) and
		// the held HZB below. Seed/remainder stay live (they prime the live
		// depth buffer). Auto-releases on extent drift: a resized HZB would
		// realloc while held and the filter would read an unbuilt pyramid.
		bool cullingFrozen = false;
		uint32_t frozenViewIdx = smvk::RenderViewSet::kInvalidIndex;
		// Pass-config gate for the whole GPU-driven filter leg (seed +
		// HZB + dispatch + remainder). When off, gfxSlot stays invalid and
		// every consumer falls back to direct draws.
		const bool gfxFilterOn = m_passes.gfxFilter && m_useGfxFilter;
		if (gfxFilterOn) {
			if (m_freezeCullingView && !m_frozenViewValid) {
				m_frozenViewProj = frameData.viewProj;
				m_frozenEye = m_cameraPositioner.cameraPosition_;
				m_frozenExtent = renderExtent;
				m_frozenViewValid = true;
			}
			if (!m_freezeCullingView)
				m_frozenViewValid = false;
			cullingFrozen = m_freezeCullingView && m_frozenViewValid
				&& m_frozenExtent.width == renderExtent.width
				&& m_frozenExtent.height == renderExtent.height;
			if (m_freezeCullingView && !cullingFrozen) {
				// Drifted (or capture failed): release rather than cull
				// against a mismatched pyramid.
				m_freezeCullingView = false;
				m_frozenViewValid = false;
			}
			m_gfxRenderer.ClearCullOverride();
			if (cullingFrozen) {
				// CPU-side only: frustum planes + matrix for FilterMainView.
				// Allocated after Upload(), so the GPU slab lacks this slot
				// â€” nothing dereferences it on GPU (filter push carries the
				// matrix; seed/VS stay on the live main view).
				frozenViewIdx = m_renderViews.AllocateCustomView(
					m_frozenViewProj,
					{ (float)renderExtent.width, (float)renderExtent.height });
				if (frozenViewIdx != smvk::RenderViewSet::kInvalidIndex)
					m_gfxRenderer.SetCullOverride(frozenViewIdx, m_frozenEye);
				else
					cullingFrozen = false;
			}
			m_gfxRenderer.AppendPasses(fc);
			gfxSlot = m_gfxRenderer.LastSlot();
		}
		fc.seedDepthPrimed = gfxFilterOn && m_gfxRenderer.SeededThisFrame();
		// Option A split: the occluder prepass fills hizDepth (Clear,
		// independent of the seed) for the HZB build below; the full
		// prepass fills renderDepth (Load when seeded) for Main, SSAO,
		// tile-cull and deferred shadows. Skipped while the culling view
		// is frozen (the held pyramid must not be rebuilt).
		const bool hizOccluderOn = gfxFilterOn && m_hizOccluderDepth
			&& !cullingFrozen && !fc.hizDepthView.empty();
		if (hizOccluderOn)
			m_fpRenderer.AppendOccluderPrepass(fc);
		// Depth prepass (Loads seeded depth when primed, Clears otherwise).
		if (m_passes.forwardPlusPrepass)
			m_fpRenderer.AppendPrepass(fc);

		// HZB build over the occluder depth (Option A) or the full depth
		// (legacy), filter compute dispatch (reads this frame's HZB),
		// then depth pass 2 over newly visible clusters for the full
		// depth. No HZB refresh after the remainder: the pyramid is
		// occluder-only and already complete, so remainder/full-prepass
		// writes must not invalidate it. Tile cull / deferred shadows
		// below still run over the complete full depth.
		// Held while the culling view is frozen (frozen footprints must test
		// against the frozen pyramid, not live depth).
		if (gfxFilterOn) {
			if (!cullingFrozen) {
				if (hizOccluderOn)
					m_gfxRenderer.AppendHizPasses(fc,
						m_viewportResources.hizDepth.logicalView,
						m_viewportResources.hizDepth.bindlessIndex);
				else
					m_gfxRenderer.AppendHizPasses(fc,
						m_viewportResources.renderDepth.logicalView,
						m_viewportResources.forwardPlus.depthBindlessIndex);
			}
			m_gfxRenderer.AppendFilterDispatch(fc);
			if (m_gfxRenderer.RemainderPending()) {
				m_gfxRenderer.AppendRemainderPass(fc);
			}
		}
	// ---- Light culling ----
	// Deferred shadows ON: the lighting pass consumes the phase-4 final
	// light lists instead of the forward+ tile lists, so the classic
	// forward+ tile cull is skipped (dead work) and the 3-phase GPU
	// pipeline runs instead (still over the complete depth below).
	// Deferred shadows OFF: legacy forward+ cull (A/B path).
	// Decals: their tile lists are refreshed by the SAME forward+ cull
	// dispatch, so when live decals exist the dispatch runs even under
	// deferred shadows â€” with lightCount 0 (the DS lists own the lights,
	// the light leg of the cull is dead work; the decal leg still fills
	// the per-tile decal lists, so splats work in BOTH shading paths).
	const bool dsOn = DsActive();
	const bool decalListsNeeded = DecalsActive() && m_decalFrameCount > 0;
	rfPB = bench_clk::now();
	if (m_passes.forwardPlusCull && (!dsOn || decalListsNeeded))
		m_fpRenderer.AppendTileCull(fc, dsOn ? 0u : UINT32_MAX);
	if (dsOn)
		AppendDeferredShadowPhases(fb.frameIndex,
			frameUniformsAddress, fpLightsAddress, spotShadowBufferAddress,
			fpLightCount, fc.depthView, fc.fpLights);

		// Transparent frustum-cull views (snapshots: recording lambdas run
		// at Execute, after this function may have returned for the next
		// frame's NewFrame). Main follows the culling-freeze override so
		// transparent matches the frozen opaque decisions while inspecting;
		// motion + prepass stay live (they prime live depth / live velocity).
		const uint32_t transparentCullIdx =
			(cullingFrozen && frozenViewIdx != smvk::RenderViewSet::kInvalidIndex)
			? frozenViewIdx : mainRenderView;
		const smvk::RenderView transparentCullView = m_renderViews.Get(transparentCullIdx);
		const smvk::RenderView liveMainCullView = m_renderViews.Get(mainRenderView);

		// ---------------------------------------------------------------------
		// Cone-axis debug lines (CPU candidate batches -> apex/tip verts).
		// Built + uploaded here (before Main records); the Main lambda only
		// binds the buffer and draws. No rendergraph hints needed: content
		// is pure CPU (batches/drawConstants/clusters vectors) + the upload
		// barrier, same pattern as the frame-uniform uploads.
		// ---------------------------------------------------------------------
		uint32_t coneLineVerts = 0;
		if (m_debugCones && m_useGfxFilter && gfxSlot != UINT32_MAX &&
			m_gfxRenderer.DebugProgramsValid() &&
			gfxSlot < m_gfxRenderer.State().views.size()) {
			const smvk::gfx::State& st = m_gfxRenderer.State();
			const smvk::gfx::View& v = st.views[gfxSlot];
			std::vector<smvk::DebugLineVertex> lines;
			lines.reserve(v.batches.size() * 2);
			for (const smvk::gfx::Batch& b : v.batches) {
				if (b.draw >= v.drawConstants.size() || b.cluster >= st.clusters.size())
					continue;
				const smvk::gfx::Draw& dc = v.drawConstants[b.draw];
				const smvk::gfx::Cluster& c = st.clusters[b.cluster];
				if (!(c.boundRadius > 0.0f))
					continue;
				const glm::vec3 apex = glm::vec3(dc.model * glm::vec4(c.coneApex, 1.0f));
				glm::vec3 ax = glm::vec3(dc.model * glm::vec4(c.coneAxis, 0.0f));
				const float alen = glm::length(ax);
				if (!(alen > 1e-9f))
					continue;
				ax /= alen;
				const float bs = std::max({
					glm::length(glm::vec3(dc.model[0])),
					glm::length(glm::vec3(dc.model[1])),
					glm::length(glm::vec3(dc.model[2])) });
				if (!(bs > 0.0f))
					continue;
				lines.push_back(smvk::DebugLineVertex{ apex });
				lines.push_back(smvk::DebugLineVertex{ apex + ax * (c.boundRadius * bs) });
			}
			if (!lines.empty()) {
				const size_t bytes = lines.size() * sizeof(smvk::DebugLineVertex);
				if (m_debugLines.empty() || m_debugLinesCap < (uint32_t)lines.size()) {
					m_debugLines = m_context->CreateBuffer(bytes,
						smvk::BufferType_Vertex, "debug cone lines");
					m_debugLinesCap = (uint32_t)lines.size();
				}
				if (!m_debugLines.empty()) {
					m_context->UploadDataToBuffer(m_debugLines, lines.data(), bytes,
						smvk::PipelineStageBit_Vertex,
						smvk::AccessFlags::VertexAttributeRead);
					coneLineVerts = (uint32_t)lines.size();
				}
			}
		}

		// ---------------------------------------------------------------------
		// Motion vectors â€“ per-pixel prevUV - currUV into RG16F (motionvector.h)
		// Re-rasterizes opaque geometry depth-tested READ-ONLY against the 1x
		// prepass depth (reversed-Z GreaterEqual, no writes) â€“ identical in
		// MSAA / non-MSAA modes, Main's transient 2x depth is never touched.
		// MASK cutouts share the program: the FS alpha-tests only when
		// shadingModel == Masked (Cull None phase, like the prepass).
		// ---------------------------------------------------------------------
		if (m_passes.motionVectors && m_viewportResources.motionVectors.Valid()) {
			PushConstants motionPC = pc;
			// Filtered motion path (option b): static filtered geometry
			// draws through the HZB-culled indirect list with the
			// gfxfilteredmotion VS (prevWorld == model). Moved filtered
			// nodes + non-filtered meshes use the direct fallback below.
			const bool motionFilteredActive = m_passes.gfxFilter
				&& m_passes.filteredMotion
				&& m_useGfxFilter
				&& m_useFilteredMotion
				&& gfxSlot != UINT32_MAX
				&& m_gfxRenderer.EnsureFilteredMotion(*m_context,
					m_viewportResources.motionVectors.fragModule);
			// One-shot diagnostic: tells which gate keeps the filtered
			// motion path off (filter toggle, motion toggle, no slot,
			// program/SPV failure, or an empty material-range list).
			{
				static bool motionFilterLogged = false;
				if (!motionFilterLogged) {
					motionFilterLogged = true;
					size_t rangeCount = 0;
					if (gfxSlot != UINT32_MAX
						&& gfxSlot < m_gfxRenderer.State().views.size())
						rangeCount = m_gfxRenderer.MaterialRanges(gfxSlot).size();
#ifndef NDEBUG
					std::fprintf(stderr,
						"[Motion] filtered=%d (filter=%d motion=%d slot=%u prog=%d ranges=%zu)\n",
						motionFilteredActive ? 1 : 0,
						m_useGfxFilter ? 1 : 0, m_useFilteredMotion ? 1 : 0,
						gfxSlot, m_gfxRenderer.FilteredMotionValid() ? 1 : 0,
						rangeCount);
#else
					(void)rangeCount;
#endif
				}
			}
			auto& motionPass = m_viewportResources.motionVectors.AddMotionVectorPass(
				*m_rendergraph,
				m_viewportResources.renderDepth.logicalView,
				m_viewportResources.motionVectors.target.logicalView,
				renderExtent,
				"MotionVectors");
			motionPass.SetRecordingFunc([this, motionPC, motionFilteredActive, gfxSlot, renderViewsAddress, mainRenderView, frameUniformsAddress, liveMainCullView](smvk::CommandBuffer& cmd)
			{
				// GPU context + pass scope come from RenderGraph::Execute.
			auto drawMotionNode = [&](const smvk::World::RenderItem& item, bool maskedPhase, bool ignoreFilter = false) {
				if (item.material == smvk::World::kInvalidIndex)
					return;
				// Static filtered nodes are covered by the HZB-culled
				// indirect draws below: skip them here so they rasterize
				// once. Moved filtered nodes (model != prev) still need
				// the direct path for their correct prevModel. Every
				// registered LOD sibling is in the indirect ranges
				// (FilterMainView pushes the selected LOD), so only
				// unregistered LODs take the direct path.
				// Dithered transparents never enter the filter (opaque-only
				// push), so their leg passes ignoreFilter to always draw.
				if (!ignoreFilter && motionFilteredActive
					&& m_gfxRenderer.GfxMeshForLod(item.mesh,
						LodIndexForNode(item.node, item.mesh)) != UINT32_MAX) {
						const bool prevValid = m_prevGlobalTransforms.size()
							== m_world->scene.globalTransform.size();
						if (prevValid) {
							const glm::mat4& cur = m_world->scene.globalTransform[item.node];
							const glm::mat4& prv = m_prevGlobalTransforms[item.node];
							if (std::memcmp(glm::value_ptr(cur), glm::value_ptr(prv),
									sizeof(cur)) == 0)
								return;
						}
						else {
							return; // no history: prev == curr, indirect covers it
						}
					}
					if (((item.flags & smvk::IteratorFlagBit_Masked) != 0) != maskedPhase)
						return;
					auto& model = m_world->scene.globalTransform[item.node];
					smvk::StaticMesh* meshDesc = LodMeshForNode(item.node, item.mesh);
					if (!meshDesc) return;
					PushConstants objectPC{};
					std::copy_n(glm::value_ptr(model), 16, objectPC.model);
					// Previous frame's transform: zero motion on the first
					// frame or after the node count changed (scene rebuild).
					const bool prevValid =
						m_prevGlobalTransforms.size() == m_world->scene.globalTransform.size();
					const glm::mat4 prevModel =
						prevValid ? m_prevGlobalTransforms[item.node] : model;
					std::copy_n(glm::value_ptr(prevModel), 16, objectPC.prevModel);
					objectPC.materialId = item.material; // FS alpha test
					cmd.PushConstants(offsetof(PushConstants, materialId),
						sizeof(objectPC.materialId), &objectPC.materialId,
						smvk::ShaderStage::All);
					cmd.PushConstants(offsetof(PushConstants, model),
						sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
					cmd.PushConstants(offsetof(PushConstants, prevModel),
						sizeof(objectPC.prevModel), objectPC.prevModel, smvk::ShaderStage::All);
					for (auto& draw_set : meshDesc->geometry.surfaces) {
						cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
					};
				};
				// 1x depth stays read-only in both phases (prepass owns it).
				const smvk::DepthSettings motionDepth{
					.depthFunc = smvk::CompareOp::GreaterEqual,
					.writeEnable = false,
					.depthTestEnable = true,
				};
				// Filtered static path: one HZB-culled multi-draw per
				// material over the compacted index list (model matrices
				// come from the draws buffer via SV_InstanceID; materialId
				// is pushed per range for the FS alpha test). Cull follows
				// the material (double-sided/MASK -> None, else Back).
				if (motionFilteredActive) {
					const auto& motionRanges = m_gfxRenderer.MaterialRanges(gfxSlot);
					for (uint32_t ri = 0; ri < (uint32_t)motionRanges.size(); ++ri) {
						const uint32_t materialId = motionRanges[ri].material;
						smvk::CullMode cull = smvk::CullMode::Back;
						if (smvk::Material* rmat = m_world->GetMaterial(materialId)) {
							if (rmat->isDoubleSided()
								|| rmat->shadingModel == smvk::ShadingModel::Masked)
								cull = smvk::CullMode::None;
						}
						cmd.BindRenderPipeline(
							m_gfxRenderer.FilteredMotionProgram(),
							smvk::VertexFormat::StaticVertexFormat,
							motionDepth,
							smvk::StencilSettings{},
							smvk::BlendSettings::Opaque(),
							smvk::MSAASettings::Count1(),
							smvk::RasterSettings{ .cullMode = cull }
						);
						// Full scene push first (a pipeline must be bound
						// before any push), then the per-range materialId
						// and the filtered VS tail.
						cmd.PushConstants(0, sizeof(PushConstants), &motionPC, smvk::ShaderStage::All);
						cmd.PushConstants(offsetof(PushConstants, materialId),
							sizeof(materialId), &materialId, smvk::ShaderStage::All);
						m_gfxRenderer.PushMotionViewConstants(
							cmd, *m_context, gfxSlot, renderViewsAddress,
							mainRenderView, frameUniformsAddress);
						m_gfxRenderer.DrawRange(cmd, *m_context, gfxSlot, ri);
					}
				}
				// Phase 1: solid geometry, back-culled like the prepass.
				cmd.BindRenderPipeline(
					m_viewportResources.motionVectors.program,
					smvk::VertexFormat::StaticVertexFormat,
					motionDepth,
					smvk::StencilSettings{},
					smvk::BlendSettings::Opaque(),
					smvk::MSAASettings::Count1(),
					smvk::RasterSettings{ .cullMode = smvk::CullMode::Back }
				);
			cmd.PushConstants(0, sizeof(PushConstants), &motionPC, smvk::ShaderStage::All);
			m_world->forEachRenderItem(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_StaticVertexFmt
				, [&](const smvk::World::RenderItem& item) { drawMotionNode(item, false); });
				// Phase 2: MASK cutout, cull off (foliage is double-sided).
				cmd.BindRenderPipeline(
					m_viewportResources.motionVectors.program,
					smvk::VertexFormat::StaticVertexFormat,
					motionDepth,
					smvk::StencilSettings{},
					smvk::BlendSettings::Opaque(),
					smvk::MSAASettings::Count1(),
					smvk::RasterSettings{ .cullMode = smvk::CullMode::None }
				);
		cmd.PushConstants(0, sizeof(PushConstants), &motionPC, smvk::ShaderStage::All);
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_Renderable
			| smvk::IteratorFlagBit_Opaque
			| smvk::IteratorFlagBit_StaticVertexFmt
			, [&](const smvk::World::RenderItem& item) { drawMotionNode(item, true); });
			// Phase 3: dithered transparent (screen-door, opaque state).
			// The FS dithers with the same pattern as the main view.
			// Culling follows the material so motion exists exactly for
			// the pixels the color pass kept. Skipped in blend mode
			// (transparent has no motion vectors there, as before).
			if (m_transparentMode == smvk::TransparentMode::Dithered
				&& !m_hideTransparent) {
				auto drawDitheredMotion = [&](bool doubleSidedPhase) {
					m_world->forEachRenderItem(
						smvk::IteratorFlagBit_Renderable
						| smvk::IteratorFlagBit_Transparent
						| smvk::IteratorFlagBit_StaticVertexFmt
						, [&](const smvk::World::RenderItem& item) {
							if (item.material == smvk::World::kInvalidIndex)
								return;
							if (const smvk::Material* mat = m_world->GetMaterial(item.material)) {
								if (mat->isDoubleSided() != doubleSidedPhase)
									return;
							}
							else if (doubleSidedPhase) {
								return;
							}
							// Frustum cull: transparent never enters the gfx
							// filter, so test the conservative LOD union here
							// (keep when either sibling intersects, matching
							// FilterMainView) to skip off-screen instances.
							{
								const glm::mat4& model = m_world->scene.globalTransform[item.node];
								const uint32_t lodIdx = LodIndexForNode(item.node, item.mesh);
								smvk::StaticMesh* baseDesc = m_world->GetMesh(item.mesh);
								smvk::StaticMesh* selDesc = m_world->GetLodMesh(item.mesh, lodIdx);
								if (baseDesc) {
									glm::vec3 c{ 0.0f }; float r = 0.0f;
									smvk::MeshBoundsToWorld(model, baseDesc->geometry.aabbMin,
										baseDesc->geometry.aabbMax, c, r);
									bool visible = smvk::RenderViewIntersectsSphere(liveMainCullView, c, r);
									if (!visible && selDesc && selDesc != baseDesc) {
										smvk::MeshBoundsToWorld(model, selDesc->geometry.aabbMin,
											selDesc->geometry.aabbMax, c, r);
										visible = smvk::RenderViewIntersectsSphere(liveMainCullView, c, r);
									}
									if (!visible)
										return;
								}
							}
							drawMotionNode(item, false, true);
						});
				};
				cmd.BindRenderPipeline(
					m_viewportResources.motionVectors.program,
					smvk::VertexFormat::StaticVertexFormat,
					motionDepth,
					smvk::StencilSettings{},
					smvk::BlendSettings::Opaque(),
					smvk::MSAASettings::Count1(),
					smvk::RasterSettings{ .cullMode = smvk::CullMode::Back }
				);
				cmd.PushConstants(0, sizeof(PushConstants), &motionPC, smvk::ShaderStage::All);
				drawDitheredMotion(false);
				cmd.BindRenderPipeline(
					m_viewportResources.motionVectors.program,
					smvk::VertexFormat::StaticVertexFormat,
					motionDepth,
					smvk::StencilSettings{},
					smvk::BlendSettings::Opaque(),
					smvk::MSAASettings::Count1(),
					smvk::RasterSettings{ .cullMode = smvk::CullMode::None }
				);
				cmd.PushConstants(0, sizeof(PushConstants), &motionPC, smvk::ShaderStage::All);
				drawDitheredMotion(true);
			}
	});
			// Filtered-path consume hints (same barriers as Main): the
			// filter CS writes the compacted index list + indirect commands
			// + draws ring that the motion indirect draws read.
			if (motionFilteredActive)
				m_gfxRenderer.AddMainConsumeHints(motionPass, gfxSlot);
			(void)motionPass;
		}

	// ---------------------------------------------------------------------
	// Sampler cache: linear clamp shared by SSAO + motion blur + bloom +
	// tonemap blits (AO taps only; depth is always .Load'ed).
	// ---------------------------------------------------------------------
	uint32_t linearSampler = m_context->GetOrCreateSampler(smvk::SamplerCreateInfo{
		.magFilter = smvk::Filter::Linear,
		.minFilter = smvk::Filter::Linear,
		.mipmapMode = smvk::MipmapMode::Linear,
		.addressU = smvk::AddressMode::ClampToEdge,
		.addressV = smvk::AddressMode::ClampToEdge,
		.addressW = smvk::AddressMode::ClampToEdge,
		.maxAnisotropy = 1.0f
	});
	// Nearest/clamp sampler for SMAA: the edge pass reads the color input
	// and the resolve fetches its samples with point filtering (official
	// SMAASamplePoint; everything else in the chain stays linear).
	uint32_t pointSampler = m_context->GetOrCreateSampler(smvk::SamplerCreateInfo{
		.magFilter = smvk::Filter::Nearest,
		.minFilter = smvk::Filter::Nearest,
		.mipmapMode = smvk::MipmapMode::Nearest,
		.addressU = smvk::AddressMode::ClampToEdge,
		.addressV = smvk::AddressMode::ClampToEdge,
		.addressW = smvk::AddressMode::ClampToEdge,
		.maxAnisotropy = 1.0f
	});
	// ---------------------------------------------------------------------
	// AO â€“ half-res depth-only AO (SSAO, see ssao.h; GTAO, see gtao.h).
	// Runs BEFORE Main: every depth-writing pass (Forward+ prepass, gfx
	// remainder, motion vectors) is done, so the 1x prepass depth is final.
	// Depth is opaque-only (transparent excluded by design) so glass reveals
	// the AO behind it instead of occluding like a wall. The raw + blur
	// passes denoise at half res; the compose pass upscales to full res with
	// depth authority (ao.hlsli); Main's opaque ambient only .Loads the
	// composed AO (it must never sample depth â€” Main attaches it).
	// ---------------------------------------------------------------------
	smvk::LogicalImageViewHandle aoView{};
	{
		const bool useGtao = m_aoMode == AoMode::Gtao;
		SMVK_PROFILE_CATEGORY("AO", Optick::Category::Rendering);
		const bool aoWanted = m_passes.ssao && m_aoMode != AoMode::None
			&& m_ssaoIntensity > 0.0f;
		if (aoWanted && !useGtao && m_viewportResources.ssao.Valid()) {
			smvk::SsaoDesc ssaoDesc = MakeSsaoDesc();
			ssaoDesc.radius = m_ssaoRadius;
			ssaoDesc.bias = m_ssaoBias;
			ssaoDesc.intensity = m_ssaoIntensity;
			ssaoDesc.maxDistance = m_ssaoMaxDistance;
			ssaoDesc.sampleCount = (uint32_t)m_ssaoSamples;
			ssaoDesc.blurSharpness = m_ssaoBlurSharpness;
			ssaoDesc.nearPlane = frameData.nearPlane;
			m_viewportResources.ssao.AddSsaoPasses(
				*m_rendergraph,
				m_viewportResources.renderDepth.logicalView,
				m_viewportResources.renderDepth.bindlessIndex,
				frameUniformsAddress,
				linearSampler,
				ssaoDesc,
				"Ssao",
				"SsaoBlur");
			aoView = m_viewportResources.ssao.OutputView();
		} else if (aoWanted && useGtao && m_viewportResources.gtao.Valid()) {
			smvk::GtaoDesc gtaoDesc = MakeGtaoDesc();
			gtaoDesc.radius = m_ssaoRadius;
			gtaoDesc.bias = m_ssaoBias;
			gtaoDesc.thickness = m_gtaoThickness;
			gtaoDesc.intensity = m_ssaoIntensity;
			gtaoDesc.maxDistance = m_ssaoMaxDistance;
			gtaoDesc.sliceCount = (uint32_t)m_gtaoSlices;
			gtaoDesc.stepsPerSlice = (uint32_t)m_gtaoSteps;
			gtaoDesc.blurSharpness = m_ssaoBlurSharpness;
			gtaoDesc.nearPlane = frameData.nearPlane;
			m_viewportResources.gtao.AddGtaoPasses(
				*m_rendergraph,
				m_viewportResources.renderDepth.logicalView,
				m_viewportResources.renderDepth.bindlessIndex,
				frameUniformsAddress,
				linearSampler,
				gtaoDesc,
				"Gtao",
				"GtaoBlur");
			aoView = m_viewportResources.gtao.OutputView();
		}
	}

	const bool useMsaa = m_passes.msaa && m_viewportResources.msaaEnabled &&
		m_context->SupportsMSAA2x() && m_viewportResources.msaa.Valid();
	std::vector<smvk::LogicalImageViewHandle> mainInputs;
	mainInputs.push_back(m_viewportResources.directionalShadows.logicalView);
	if (m_viewportResources.spotShadows.Valid())
		mainInputs.push_back(m_viewportResources.spotShadows.logicalView);
	// Blurred half-res AO is sampled by the opaque ambient (default_lit.frag):
	// SetInputImages emits the read barrier + layout transition.
	if (!aoView.empty())
		mainInputs.push_back(aoView);
	const auto rfTM = bench_clk::now();
	auto& mainPass = m_rendergraph->AddPass("Main")
			.SetOffset(0, 0)
			.SetInputImages(mainInputs)
			.SetExtent(renderExtent);
		if (useMsaa) {
			// 2x MSAA color resolves into the 1x HDR intermediate.
			mainPass.SetColorWithResolve(
				{ m_viewportResources.msaa.colorLogical },
				{ m_viewportResources.hdr.logicalView },
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::DontCare,
				smvk::ClearColor(0, 0, 0, 1)
			)
			// Transient 2x depth: cleared here, written by Main.
			// The 1x prepass depth stays reserved for Forward+ cull.
			.SetDepth(m_viewportResources.msaa.depthLogical,
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::DontCare,
				smvk::ClearDepthStencil(0, 0)
			);
		} else {
			mainPass.SetColor(
				hdrColors, // HDR intermediate â€“ tonemapper will resolve to swap
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0, 0, 0, 1)
			)
			// Depth was filled by the Forward+ prepass: load it, test
			// read-only (GreaterEqual = reversed-Z near-wins).
			.SetDepth(m_viewportResources.renderDepth.logicalView,
				smvk::AttachmentLoadOp::Load,
				smvk::AttachmentStoreOp::DontCare,
				smvk::ClearDepthStencil(0, 0)
			);
		}
		const glm::mat4 frameViewProj = frameData.viewProj; // debug pushes (value)
		mainPass.SetRecordingFunc([this, pc, useMsaa, gfxSlot, renderViewsAddress, mainRenderView, coneLineVerts, frameViewProj, transparentCullView](smvk::CommandBuffer& cmd)
	{
		// 1x: depth was filled by the Forward+ prepass, main is read-only.
		// 2x: transient depth is cleared above, main writes it
		// (GreaterEqual reversed-Z in both cases).
		// Opaque writes depth in MSAA mode (transient target); transparent
		// never writes depth in either mode.
		const smvk::DepthSettings opaqueDepth{
			.depthFunc = smvk::CompareOp::GreaterEqual,
			.writeEnable = useMsaa,
			.depthTestEnable = true,
		};
		const smvk::DepthSettings transparentDepth{
			.depthFunc = smvk::CompareOp::GreaterEqual,
			.writeEnable = false,
			.depthTestEnable = true,
		};
		const smvk::StencilSettings noStencil{};
		const smvk::MSAASettings msaaSettings =
			useMsaa ? smvk::MSAASettings::Count2() : smvk::MSAASettings::Count1();

		// Wireframe (P): force Line polygon mode + no culling on every
		// scene-triangle bind below (filtered ranges, direct draws, flat
		// debug views, transparent leg) so back faces show too. The depth
		// prepass stays Fill, so lines keep hidden-line removal against
		// its depth; skydome (backdrop) and the line overlays stay as-is.
		auto wireframeRaster = [&](smvk::RasterSettings r) {
			if (m_wireframe) {
				r.polygonMode = smvk::PolygonMode::Line;
				r.cullMode = smvk::CullMode::None;
			}
			return r;
		};

		// Wireframe FS swap: line pixels are rare, so running the heavy lit
		// FS (Forward+ loop, GI, texture feedback atomics) on them was the
		// wireframe slowdown. While P is on, the lit/filtered/vertex-lit
		// programs bind their flat-line twin (default_wire.frag + the SAME
		// VS: a FS consuming only SV_Position links with any of them, no
		// VS twins needed). The wire FS reads pc.frameUniforms, which every
		// caller pushes (full push at pass start; filtered ranges push
		// deltas on top). Debug-view FSs (unlit/cascade/vcolor/meshlet
		// colors) have no twin: F2..F6 keep their own shading on the lines,
		// so P keeps stacking. Missing twin (shader failed to load) returns
		// the caller's program â€” the pre-swap shaded wireframe.
		auto wireTwinOf = [&](smvk::ShaderProgram& prog) -> smvk::ShaderProgram& {
			if (!m_wireframe)
				return prog;
			if (&prog == &m_resources.mainProgram
				|| &prog == &m_resources.mainProgramA2C) {
				return m_resources.mainWireProgram.shaderCount == 2
					? m_resources.mainWireProgram : prog;
			}
			if (&prog == &m_gfxRenderer.FilteredProgram()
				|| &prog == &m_gfxRenderer.FilteredProgramA2C()) {
				return m_gfxRenderer.FilteredWireValid()
					? m_gfxRenderer.FilteredWireProgram() : prog;
			}
			if (&prog == &m_resources.vertexLitProgram) {
				return m_resources.vertexLitWireProgram.shaderCount == 2
					? m_resources.vertexLitWireProgram : prog;
			}
			return prog;
		};

		auto drawNode = [&](NodeId node, uint32_t meshId, uint32_t materialId) {
			auto& model = m_world->scene.globalTransform[node];
			smvk::StaticMesh* meshDesc = LodMeshForNode(node, meshId);
			if (!meshDesc)
				return;
			PushConstants pc{};
			pc.materialId = materialId;
			const float* modelData = glm::value_ptr(model);
			std::copy_n(modelData, 16, pc.model);
			cmd.PushConstants(
				offsetof(PushConstants, materialId),
				sizeof(pc.materialId),
				&pc.materialId,
				smvk::ShaderStage::All
			);
			cmd.PushConstants(
				0,
				offsetof(PushConstants, frameUniforms),
				&pc,
				smvk::ShaderStage::All
			);
			for (auto& draw_set : meshDesc->geometry.surfaces) {
				cmd.DrawIndexed(
					draw_set.indexCount,
					1,
					draw_set.firstIndex,
					draw_set.baseVertex
				);
			}
		};

		// A2C fallback for program families without an alpha-to-coverage
		// twin (debug views, vertex-lit LODs, transparents): shaderCount 0
		// keeps them on the caller's classic program.
		smvk::ShaderProgram noA2CProgram{};
		auto bindForMaterial = [&](uint32_t materialId, smvk::ShaderProgram& prog,
				smvk::ShaderProgram& progA2c,
				const smvk::DepthSettings& depth) -> smvk::ShaderProgram& {
			// Per-material raster (doubleSided -> Cull None) + blend
			// (Opaque vs AlphaBlend). Depth writes follow the caller: opaque
			// writes in 2x MSAA mode, transparent never writes.
			smvk::BlendSettings blend = smvk::BlendSettings::Opaque();
			smvk::RasterSettings raster{};
			bool maskedCutout = false;
			if (smvk::Material* mat = m_world->GetMaterial(materialId)) {
				blend = mat->pipeline.blend;
				raster = mat->pipeline.raster;
				maskedCutout =
					mat->shadingModel == smvk::ShadingModel::Masked;
			}
			// Masked cutout in 2x MSAA + A2C opt-in (VULKANTOY_A2C):
			// alpha-to-coverage program (coverage remap in the FS, zero
			// discard) + A2C multisample state â€” foliage silhouettes
			// resolve AA'd. Opt-in because A2C moves the per-sample depth
			// test after the FS: overlapping layers lose early-reject
			// (+6ms bistro foliage bench), so the default stays the
			// demote-discard variant. Skipped while wireframed: the flat
			// line FS writes alpha 1, so A2C coverage would be pointless.
			const bool useA2c =
				!m_wireframe
				&& useMsaa && m_msaaAlphaToCoverage && maskedCutout
				&& progA2c.shaderCount == 2;
			smvk::ShaderProgram& bound = useA2c ? progA2c : prog;
			// wireTwinOf swaps the lit/filtered/vertex-lit FS for the flat
			// line FS while P is on (binds the twin, tracks the original).
			cmd.BindRenderPipeline(
				wireTwinOf(bound),
				smvk::VertexFormat::StaticVertexFormat,
				depth,
				noStencil,
				blend,
				useA2c ? smvk::MSAASettings::Count2A2C() : msaaSettings,
				wireframeRaster(raster)
			);
			return bound;
		};

		// Initial bind BEFORE the first push: PushConstants needs a bound
		// pipeline layout. The opaque loop rebinds per material anyway
		// (boundMaterial starts UINT32_MAX, so the first node always
		// rebinds if it differs).
		// ---- Skydome: fullscreen triangle first (depth Always, no writes) ----
		// Opaque geometry draws over it via GreaterEqual; background pixels
		// keep the sky HDR. Works for both 1x (HDR Load) and 2x MSAA
		// (transient color Clear + resolve) because the sky is part of the
		// same pass that resolves.
		if (m_passes.skydome && m_env.skyEnabled && m_skydome.ShadersValid()) {
			cmd.BindRenderPipeline(
				m_skydome.program,
				smvk::VertexFormat::Empty,
				smvk::DepthSettings::Disabled(),
				noStencil,
				smvk::BlendSettings::Opaque(),
				msaaSettings,
				smvk::RasterSettings{ .cullMode = smvk::CullMode::None }
			);
			cmd.PushConstants(
				0,
				sizeof(PushConstants),
				&pc,
				smvk::ShaderStage::All
			);
			cmd.Draw(3, 1, 0, 0);
		}
		cmd.BindRenderPipeline(
			// Initial bind: the flat-line twin of main while P is on.
			wireTwinOf(m_resources.mainProgram),
			smvk::VertexFormat::StaticVertexFormat,
			opaqueDepth,
			noStencil,
			smvk::BlendSettings::Opaque(),
			msaaSettings,
			wireframeRaster(smvk::RasterSettings{})
		);
		cmd.PushConstants(
			0,
			sizeof(PushConstants),
			&pc,
			smvk::ShaderStage::All
		);

		// ---- Opaque + MASK cutout (alpha-tested in shader) ----
		// Filtered path: one multi-draw per material over the compacted
		// index list (model matrices come from the draws buffer via
		// SV_InstanceID; materialId is pushed per range for the frag).
		// Meshes the filter skipped (no clusters) still draw direct.
		const bool gfxActive = m_useGfxFilter && gfxSlot != UINT32_MAX;
		// Debug views (F1..F6) replace the shaded opaque leg. Meshlet mode
		// draws EVERY registered cluster direct with a stable per-meshlet
		// color, so HZB-culled meshlets stay visible here. Compare with
		// the shaded view to see exactly what the GPU filter dropped.
		// Unlit / cascade / LOD / vertex-color views likewise bypass the
		// filter with direct draws (no filtered-VS variant exists for them).
		// Missing programs fall back to Lit so a bad cook never blanks
		// the frame.
		DebugView effView = m_debugView;
		if ((effView == DebugView::Meshlets || effView == DebugView::Lods)
			&& !m_gfxRenderer.DebugProgramsValid())
			effView = DebugView::Lit;
		if (effView == DebugView::Unlit && m_resources.unlitProgram.shaderCount != 2)
			effView = DebugView::Lit;
		if (effView == DebugView::Cascades && m_resources.cascadeProgram.shaderCount != 2)
			effView = DebugView::Lit;
		if (effView == DebugView::VertexColors && m_resources.vertexColorProgram.shaderCount != 2)
			effView = DebugView::Lit;
		if (effView == DebugView::Uv0 && m_resources.uv0DebugProgram.shaderCount != 2)
			effView = DebugView::Lit;
		if (effView == DebugView::Uv1 && m_resources.uv1DebugProgram.shaderCount != 2)
			effView = DebugView::Lit;
		const bool meshletDbg = (effView == DebugView::Meshlets);
		const bool lodDbg = (effView == DebugView::Lods);
		const bool flatDbg = meshletDbg || lodDbg; // meshDebugProgram path
		const bool unlitDbg = (effView == DebugView::Unlit);
		const bool cascadeDbg = (effView == DebugView::Cascades);
		const bool vertexColorDbg = (effView == DebugView::VertexColors);
		const bool uvDbg = (effView == DebugView::Uv0 || effView == DebugView::Uv1);
		const bool directDbg = unlitDbg || cascadeDbg || vertexColorDbg || uvDbg; // default.vert + debug FS
		const bool bypassFilter = (effView != DebugView::Lit);
		// LOD palette (display colors; pre-scaled by dbgGain like meshlets
		// so they survive the HDR -> tonemap path).
		static const float kLodPalette[8][3] = {
			{ 0.20f, 0.90f, 0.30f }, // LOD0 green
			{ 0.95f, 0.85f, 0.20f }, // LOD1 yellow
			{ 1.00f, 0.55f, 0.15f }, // LOD2 orange
			{ 1.00f, 0.20f, 0.20f }, // LOD3 red
			{ 1.00f, 0.20f, 1.00f }, // LOD4+ magenta
			{ 1.00f, 0.20f, 1.00f },
			{ 1.00f, 0.20f, 1.00f },
			{ 1.00f, 0.20f, 1.00f },
		};
		// Exposure compensation shared by the flat debug views (meshlets +
		// LODs): authored display colors travel through HDR + tonemap
		// (hdr * exposure -> ACES); pre-scale by 1/exposure so the
		// tonemapper reproduces them (ACES still shapes, hues stay
		// distinct). The unlit/cascade shaders do this in-shader.
		// FrameExposure: debug views run at the fixed reference exposure
		// (see scene_renderer.h), so the gain is a constant 1/0.001 and
		// the flat colors are deterministic across runs/scenes.
		const float flatExposure = FrameExposure();
		const float flatGain = (flatExposure > 1e-9f) ? 1.0f / flatExposure : 1.0f;
		if (meshletDbg) {
			cmd.BindRenderPipeline(
				m_gfxRenderer.MeshDebugProgram(),
				smvk::VertexFormat::StaticVertexFormat,
				opaqueDepth,
				noStencil,
				smvk::BlendSettings::Opaque(),
				msaaSettings,
				wireframeRaster(smvk::RasterSettings{ .cullMode = smvk::CullMode::None })
			);
		cmd.BindStaticVertexFmt();
		// No index bind: mesh, cluster-ordered and debug draws all address
		// the static index monoblock absolutely (offset-0 binding above).
		const smvk::gfx::State& dbgState = m_gfxRenderer.State();
		MeshDebugPush dp{};
		std::copy_n(glm::value_ptr(frameViewProj), 16, dp.viewProj);
		// Debug colors are authored as display colors, but they travel
		// through the HDR target + tonemap (hdr * exposure -> ACES). With
		// physical light units the exposure is ~0.001, which crushes every
		// flat color to black. Pre-scale by 1/exposure so hdr * exposure
		// reproduces the authored color (ACES still shapes it, hues stay
		// distinct); guards the divide for pathological exposures.
		const float dbgExposure = FrameExposure();
		const float dbgGain = (dbgExposure > 1e-9f) ? 1.0f / dbgExposure : 1.0f;
			m_world->forEachRenderItem(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_StaticVertexFmt
				,
				[&](const smvk::World::RenderItem& item)
			{
				if (item.material == smvk::World::kInvalidIndex)
					return;
				const uint32_t worldMesh = item.mesh;
				const glm::mat4& model = m_world->scene.globalTransform[item.node];
				std::copy_n(glm::value_ptr(model), 16, dp.model);
				// Debug the SELECTED sibling: every registered LOD has its
				// own gfx mesh (see RegisterWorldMeshes), so the overlay
				// shows what the filter actually culled for this instance.
				const uint32_t gfxMesh = m_gfxRenderer.GfxMeshForLod(worldMesh,
					LodIndexForNode(item.node, worldMesh));
		if (gfxMesh == UINT32_MAX || gfxMesh >= dbgState.meshes.size()) {
			// Not filterable: white direct fallback (nothing vanishes).
			// Mesh-local surfaces address the same index monoblock.
			dp.color[0] = dp.color[1] = dp.color[2] = 0.9f * dbgGain; dp.color[3] = 1.0f;
			cmd.PushConstants(0, sizeof(dp), &dp, smvk::ShaderStage::All);
			if (smvk::StaticMesh* meshDesc = LodMeshForNode(item.node, worldMesh)) {
				for (auto& draw_set : meshDesc->geometry.surfaces) {
					cmd.DrawIndexed(draw_set.indexCount, 1,
						draw_set.firstIndex, draw_set.baseVertex);
				}
			}
			return;
		}
				const smvk::gfx::Mesh& gm = dbgState.meshes[gfxMesh];
				for (uint32_t ci = 0; ci < gm.clusterCount; ++ci) {
					const uint32_t gc = gm.clusterStart + ci;
					if (gc >= dbgState.clusters.size())
						continue;
					const smvk::gfx::Cluster& cl = dbgState.clusters[gc];
					if (cl.triCount == 0)
						continue;
					float rgba[4];
				GfxRenderer::DebugClusterColor(
					gc, cl.coneCutoff, m_debugMeshletMode, rgba);
			dp.color[0] = rgba[0] * dbgGain; dp.color[1] = rgba[1] * dbgGain;
			dp.color[2] = rgba[2] * dbgGain; dp.color[3] = rgba[3];
				cmd.PushConstants(0, sizeof(dp), &dp, smvk::ShaderStage::All);
				// Absolute into the static index monoblock (region base).
				const uint32_t firstIndex =
					smvk::gfx::ClusterIndexBase(dbgState) + cl.indexOffset;
				cmd.DrawIndexed(cl.triCount * 3, 1, firstIndex, 0);
				}
			}
			);
		}
		// LOD debug view (F5): every opaque instance direct with a flat
		// color for its SELECTED LOD sibling (green LOD0 -> red LOD3,
		// magenta beyond). Uses the meshDebugProgram, bypasses the filter.
		if (lodDbg) {
			cmd.BindRenderPipeline(
				m_gfxRenderer.MeshDebugProgram(),
				smvk::VertexFormat::StaticVertexFormat,
				opaqueDepth,
				noStencil,
				smvk::BlendSettings::Opaque(),
				msaaSettings,
				wireframeRaster(smvk::RasterSettings{ .cullMode = smvk::CullMode::None })
			);
			cmd.BindStaticVertexFmt();
			MeshDebugPush lp2{};
			std::copy_n(glm::value_ptr(frameViewProj), 16, lp2.viewProj);
			m_world->forEachRenderItem(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_StaticVertexFmt
				,
				[&](const smvk::World::RenderItem& item)
			{
				if (item.material == smvk::World::kInvalidIndex)
					return;
				const glm::mat4& model = m_world->scene.globalTransform[item.node];
				std::copy_n(glm::value_ptr(model), 16, lp2.model);
				uint32_t lod = 0;
				smvk::StaticMesh* meshDesc = LodMeshForNode(item.node, item.mesh, &lod);
				if (!meshDesc)
					return;
				// Shader-LOD overlay: when the subsystem is on, instances
				// shading vertex-lit (farthest level) report WHITE instead
				// of their LOD palette color, so shading downgrades are
				// visible next to geometry ones.
				static const float kLodVertexLit[3] = { 1.0f, 1.0f, 1.0f };
				const bool vlitLevel = ShaderLodActive()
					&& m_world->LodShaderAt(item.mesh, lod)
						== smvk::LodShader::VertexLit;
				const float* pal = vlitLevel
					? kLodVertexLit : kLodPalette[lod < 8 ? lod : 4];
				lp2.color[0] = pal[0] * flatGain;
				lp2.color[1] = pal[1] * flatGain;
				lp2.color[2] = pal[2] * flatGain;
				lp2.color[3] = 1.0f;
				cmd.PushConstants(0, sizeof(lp2), &lp2, smvk::ShaderStage::All);
				for (auto& draw_set : meshDesc->geometry.surfaces) {
					cmd.DrawIndexed(draw_set.indexCount, 1,
						draw_set.firstIndex, draw_set.baseVertex);
				}
			}
			);
		}
		// Unlit (F2) / cascade (F4) / vertex-color (F6) / UV (F9/F10)
		// views: every opaque instance direct with the debug fragment
		// program. Full push per draw (model + materialId over the
		// captured frame addresses): the flat debug pushes above clobber
		// the shared push range, so delta pushes alone would read garbage
		// frame addresses here.
		if (directDbg) {
			smvk::ShaderProgram& dbgProg =
				unlitDbg ? m_resources.unlitProgram
				: cascadeDbg ? m_resources.cascadeProgram
				: vertexColorDbg ? m_resources.vertexColorProgram
				: effView == DebugView::Uv1 ? m_resources.uv1DebugProgram
				: m_resources.uv0DebugProgram;
			uint32_t boundMaterial = UINT32_MAX;
			m_world->forEachRenderItem(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_StaticVertexFmt
				,
				[&](const smvk::World::RenderItem& item)
			{
				if (item.material == smvk::World::kInvalidIndex)
					return;
				const uint32_t materialId = item.material;
			if (materialId != boundMaterial) {
				bindForMaterial(materialId, dbgProg, noA2CProgram, opaqueDepth);
				boundMaterial = materialId;
			}
				auto& model = m_world->scene.globalTransform[item.node];
				smvk::StaticMesh* meshDesc = LodMeshForNode(item.node, item.mesh);
				if (!meshDesc)
					return;
				PushConstants objPC = pc;
				objPC.materialId = materialId;
				std::copy_n(glm::value_ptr(model), 16, objPC.model);
				cmd.PushConstants(0, sizeof(PushConstants), &objPC, smvk::ShaderStage::All);
				for (auto& draw_set : meshDesc->geometry.surfaces) {
					cmd.DrawIndexed(
						draw_set.indexCount,
						1,
						draw_set.firstIndex,
						draw_set.baseVertex
					);
				}
			}
			);
		}
		if (gfxActive && !bypassFilter) {
			const auto& gfxRanges = m_gfxRenderer.MaterialRanges(gfxSlot);
			for (uint32_t ri = 0; ri < (uint32_t)gfxRanges.size(); ++ri) {
				const uint32_t materialId = gfxRanges[ri].material;
				bindForMaterial(materialId, m_gfxRenderer.FilteredProgram(),
					m_gfxRenderer.FilteredProgramA2C(), opaqueDepth);
				PushConstants matPC{};
				matPC.materialId = materialId;
				cmd.PushConstants(
					offsetof(PushConstants, materialId),
					sizeof(matPC.materialId),
					&matPC.materialId,
					smvk::ShaderStage::All
				);
				m_gfxRenderer.PushViewConstants(
					cmd, *m_context, gfxSlot, renderViewsAddress, mainRenderView);
				m_gfxRenderer.DrawRange(cmd, *m_context, gfxSlot, ri);
			}
		}
		if (!bypassFilter) {
			// UINT32_MAX forces a bind check on the first node; an
			// identical state is deduped by the CommandBuffer's
			// current-pipeline check, so this costs nothing.
			// (Skipped in debug views: those drew everything direct above,
			// filter bypassed so culled data stays visible.)
			// Shader LODs: the vertex-lit program is tracked alongside the
			// material because the same material can shade lit (near level)
			// and vertex-lit (farthest level) in one material-sorted walk.
			const bool shaderLodOn = ShaderLodActive();
			uint32_t boundMaterial = UINT32_MAX;
			smvk::ShaderProgram* boundProgram = nullptr;
			m_world->forEachRenderItem(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				//| smvk::IteratorFlagBit_PassedCullTest
				| smvk::IteratorFlagBit_StaticVertexFmt
				,
				[&](const smvk::World::RenderItem& item)
			{
			if (item.material == smvk::World::kInvalidIndex)
				return;
#if 0 // TMP-DIAG scanlands hunt (disabled): VULKANTOY_SHOWGROUP="n"
			// shows ONLY LOD group n (mirrors the FilterMainView hack).
			// Remove later.
			{
				static int kShow = INT_MAX;
				static bool kInit = false;
				if (!kInit) {
					kInit = true;
					if (const char* r = std::getenv("VULKANTOY_SHOWGROUP")) {
						int n = INT_MAX;
						if (std::sscanf(r, "%d", &n) == 1)
							kShow = n;
					}
				}
				if (kShow != INT_MAX && item.mesh < m_world->meshLodInfos.size()
					&& (int)m_world->meshLodInfos[item.mesh].groupId != kShow)
					return;
			}
#endif
			// Shader LODs: instances whose selected level is the farthest
			// one draw HERE with the vertex-lit program â€” FilterMainView
			// skipped them at registration for the same reason (the
			// filtered VS is lit-only), so this is the exactly-once draw.
			const bool vertexLitLevel = shaderLodOn
				&& LodShaderForNode(item.node, item.mesh) == smvk::LodShader::VertexLit;
			// Filtered instances already drew above via indirect
			// ranges (FilterMainView pushes the selected LOD sibling).
			// Only LODs whose registration failed fall through to the
			// direct LOD draw, so each instance rasterizes once.
			if (gfxActive && !vertexLitLevel && m_gfxRenderer.GfxMeshForLod(item.mesh,
				LodIndexForNode(item.node, item.mesh)) != UINT32_MAX)
				return;
				smvk::ShaderProgram& prog = vertexLitLevel
					? m_resources.vertexLitProgram
					: m_resources.mainProgram;
				smvk::ShaderProgram& progA2c = vertexLitLevel
					? noA2CProgram
					: m_resources.mainProgramA2C;
				const uint32_t materialId = item.material;
				if (materialId != boundMaterial
					|| (&prog != boundProgram && &progA2c != boundProgram)) {
					smvk::ShaderProgram& bound =
						bindForMaterial(materialId, prog, progA2c, opaqueDepth);
					boundMaterial = materialId;
					boundProgram = &bound;
				}
				if (vertexLitLevel)
					++m_lodShaderDraws;
				drawNode(item.node, item.mesh, materialId);
			}
			);
		}

		// ---- Transparent (AlphaMode::Blend): dithered (default) or alpha blend ----
		// Dithered: screen-door discard in shader, opaque pipeline state
		// (blend off, depth test follows the opaque path). Depth holds
		// opaque only (transparent excluded from the prepass by design),
		// so transparents test against it read-only in 1x; in MSAA they
		// write the transient target inline like opaque, so no sorting is
		// needed â€“ order-independent like opaque.
		// Blended: classic back-to-front AlphaBlend, depth test on, no
		// depth writes, sorted by distance-to-camera.
		// m_hideTransparent (debug) skips the whole leg to bisect blending
		// artifacts out of hole reports.
		{
			const bool ditheredTransp =
				(m_transparentMode == smvk::TransparentMode::Dithered);
			struct TransparentDraw { NodeId node; uint32_t meshId; uint32_t materialId; float dist2; };
			std::vector<TransparentDraw> draws;
			const glm::vec3 camPos = m_cameraPositioner.cameraPosition_;
			if (!m_hideTransparent) {
			m_world->forEachRenderItem(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Transparent
				| smvk::IteratorFlagBit_StaticVertexFmt
				,
				[&](const smvk::World::RenderItem& item)
			{
				if (item.material == smvk::World::kInvalidIndex)
					return;
				const glm::mat4& model = m_world->scene.globalTransform[item.node];
				// Frustum cull: transparent never enters the gfx filter, so
				// test the conservative LOD union here (keep when either
				// the base or the selected sibling intersects, matching
				// FilterMainView) instead of drawing off-screen instances.
				{
					const uint32_t lodIdx = LodIndexForNode(item.node, item.mesh);
					smvk::StaticMesh* baseDesc = m_world->GetMesh(item.mesh);
					smvk::StaticMesh* selDesc = m_world->GetLodMesh(item.mesh, lodIdx);
					if (baseDesc) {
						glm::vec3 c{ 0.0f }; float r = 0.0f;
						smvk::MeshBoundsToWorld(model, baseDesc->geometry.aabbMin,
							baseDesc->geometry.aabbMax, c, r);
						bool visible = smvk::RenderViewIntersectsSphere(transparentCullView, c, r);
						if (!visible && selDesc && selDesc != baseDesc) {
							smvk::MeshBoundsToWorld(model, selDesc->geometry.aabbMin,
								selDesc->geometry.aabbMax, c, r);
							visible = smvk::RenderViewIntersectsSphere(transparentCullView, c, r);
						}
						if (!visible)
							return;
					}
				}
				const glm::vec3 center{ model[3][0], model[3][1], model[3][2] };
				const glm::vec3 d = center - camPos;
				draws.push_back(TransparentDraw{
					.node = item.node,
					.meshId = item.mesh,
					.materialId = item.material,
					.dist2 = glm::dot(d, d),
				});
			}
			);
			}
			if (!ditheredTransp) {
				std::sort(draws.begin(), draws.end(),
					[](const TransparentDraw& a, const TransparentDraw& b) { return a.dist2 > b.dist2; });
			}

			// Transparent follows the opaque debug view (Unlit/Cascades shade
			// it with the same debug FS via full pushes; LODs draws it flat
			// in the instance LOD color). Meshlets keeps the legacy look
			// (transparent stays lit).
			smvk::ShaderProgram* transProg = &m_resources.mainProgram;
			if (unlitDbg) transProg = &m_resources.unlitProgram;
			else if (cascadeDbg) transProg = &m_resources.cascadeProgram;
			// Shader LODs apply to transparent too, but only in the Lit view
			// (debug views replace shading wholesale, see above).
			const bool transShaderLodOn = ShaderLodActive()
				&& effView == DebugView::Lit;
			MeshDebugPush transLp{};
			if (lodDbg)
				std::copy_n(glm::value_ptr(frameViewProj), 16, transLp.viewProj);

			uint32_t boundMaterial = UINT32_MAX;
			smvk::ShaderProgram* boundProgram = nullptr;
			for (const auto& d : draws) {
				if (lodDbg) {
					// Flat LOD color (self-contained push; ignores material).
					uint32_t lod = 0;
					smvk::StaticMesh* meshDesc = LodMeshForNode(d.node, d.meshId, &lod);
					if (!meshDesc)
						continue;
					if (d.materialId != boundMaterial) {
						cmd.BindRenderPipeline(
							m_gfxRenderer.MeshDebugProgram(),
							smvk::VertexFormat::StaticVertexFormat,
							ditheredTransp ? opaqueDepth : transparentDepth,
							noStencil,
							smvk::BlendSettings::Opaque(),
							msaaSettings,
							wireframeRaster(smvk::RasterSettings{ .cullMode = smvk::CullMode::None })
						);
						cmd.BindStaticVertexFmt();
						boundMaterial = d.materialId;
					}
					const glm::mat4& model = m_world->scene.globalTransform[d.node];
					std::copy_n(glm::value_ptr(model), 16, transLp.model);
					static const float kLodVertexLitTrans[3] = { 1.0f, 1.0f, 1.0f };
					const bool vlitLevelTrans = ShaderLodActive()
						&& m_world->LodShaderAt(d.meshId, lod)
							== smvk::LodShader::VertexLit;
					const float* pal = vlitLevelTrans
						? kLodVertexLitTrans : kLodPalette[lod < 8 ? lod : 4];
					transLp.color[0] = pal[0] * flatGain;
					transLp.color[1] = pal[1] * flatGain;
					transLp.color[2] = pal[2] * flatGain;
					transLp.color[3] = 1.0f;
					cmd.PushConstants(0, sizeof(transLp), &transLp, smvk::ShaderStage::All);
					for (auto& draw_set : meshDesc->geometry.surfaces) {
						cmd.DrawIndexed(draw_set.indexCount, 1,
							draw_set.firstIndex, draw_set.baseVertex);
					}
					continue;
				}
				// Shader LODs: farthest-level transparents shade vertex-lit
				// with a full push (the per-material delta push below feeds
				// the lit program only). Same coarsest geometry.
				const bool vlitTrans = transShaderLodOn
					&& LodShaderForNode(d.node, d.meshId) == smvk::LodShader::VertexLit;
				if (vlitTrans) {
					if (d.materialId != boundMaterial
						|| boundProgram != &m_resources.vertexLitProgram) {
						if (ditheredTransp) {
							// Opaque state: the FS discard does the work.
							smvk::BlendSettings opaqueBlend = smvk::BlendSettings::Opaque();
							smvk::RasterSettings raster{};
							if (smvk::Material* mat = m_world->GetMaterial(d.materialId)) {
								raster = mat->pipeline.raster;
							}
						cmd.BindRenderPipeline(
							wireTwinOf(m_resources.vertexLitProgram),
							smvk::VertexFormat::StaticVertexFormat,
							opaqueDepth,
							noStencil,
							opaqueBlend,
							msaaSettings,
							wireframeRaster(raster)
						);
					} else {
						bindForMaterial(d.materialId, m_resources.vertexLitProgram,
							noA2CProgram, transparentDepth);
					}
						boundMaterial = d.materialId;
						boundProgram = &m_resources.vertexLitProgram;
					}
					auto& model = m_world->scene.globalTransform[d.node];
					smvk::StaticMesh* meshDesc = LodMeshForNode(d.node, d.meshId);
					if (!meshDesc)
						continue;
					PushConstants objPC = pc;
					objPC.materialId = d.materialId;
					std::copy_n(glm::value_ptr(model), 16, objPC.model);
					cmd.PushConstants(0, sizeof(PushConstants), &objPC, smvk::ShaderStage::All);
					for (auto& draw_set : meshDesc->geometry.surfaces) {
						cmd.DrawIndexed(
							draw_set.indexCount,
							1,
							draw_set.firstIndex,
							draw_set.baseVertex
						);
					}
					++m_lodShaderDraws;
					continue;
				}
				if (d.materialId != boundMaterial || boundProgram != transProg) {
					if (ditheredTransp) {
						// Opaque state: the shader discard did the work.
						// Blend off (bindForMaterial would return AlphaBlend
						// from material.pipeline); depth follows opaque.
						smvk::BlendSettings opaqueBlend = smvk::BlendSettings::Opaque();
						smvk::RasterSettings raster{};
						if (smvk::Material* mat = m_world->GetMaterial(d.materialId)) {
							raster = mat->pipeline.raster;
						}
					cmd.BindRenderPipeline(
						wireTwinOf(*transProg),
						smvk::VertexFormat::StaticVertexFormat,
						opaqueDepth,
						noStencil,
						opaqueBlend,
						msaaSettings,
						wireframeRaster(raster)
					);
					} else {
						bindForMaterial(d.materialId, *transProg, noA2CProgram, transparentDepth);
					}
					boundMaterial = d.materialId;
					boundProgram = transProg;
				}
				if (directDbg) {
					// Full push: the flat debug pushes above clobbered the
					// shared push range, so delta pushes would read garbage
					// frame addresses here.
					auto& model = m_world->scene.globalTransform[d.node];
					smvk::StaticMesh* meshDesc = LodMeshForNode(d.node, d.meshId);
					if (!meshDesc)
						continue;
					PushConstants objPC = pc;
					objPC.materialId = d.materialId;
					std::copy_n(glm::value_ptr(model), 16, objPC.model);
					cmd.PushConstants(0, sizeof(PushConstants), &objPC, smvk::ShaderStage::All);
					for (auto& draw_set : meshDesc->geometry.surfaces) {
						cmd.DrawIndexed(
							draw_set.indexCount,
							1,
							draw_set.firstIndex,
							draw_set.baseVertex
						);
					}
					continue;
				}
				drawNode(d.node, d.meshId, d.materialId);
			}
		}

		// ---- Cone-axis debug overlay (LineList over the CPU batches) ----
		// Shows the normal cone of every candidate (draw x cluster) the
		// filter saw: apex -> apex + axis * worldRadius (yellow). Missing
		// meshlets in the shaded view but present cones here = GPU HZB
		// cull; missing cones = CPU pre-cull (frustum/cone).
		if (m_debugCones && coneLineVerts > 0 && !m_debugLines.empty() &&
			m_gfxRenderer.DebugProgramsValid()) {
			cmd.BindRenderPipeline(
				m_gfxRenderer.LineProgram(),
				smvk::VertexFormat::DebugLine,
				transparentDepth,
				noStencil,
				smvk::BlendSettings::Opaque(),
				msaaSettings,
				smvk::RasterSettings{ .cullMode = smvk::CullMode::None },
				smvk::PrimitiveTopology::LineList
			);
			cmd.BindVertexBuffer(m_debugLines, 0, 0);
			LineDebugPush lp{};
			std::copy_n(glm::value_ptr(frameViewProj), 16, lp.viewProj);
			// Same HDR/exposure path as the meshlet colors above: pre-scale
			// so the tonemap reproduces yellow instead of black.
			const float lineExposure = FrameExposure();
			const float lineGain = (lineExposure > 1e-9f) ? 1.0f / lineExposure : 1.0f;
			lp.color[0] = 1.0f * lineGain; lp.color[1] = 1.0f * lineGain;
			lp.color[2] = 0.0f; lp.color[3] = 1.0f;
			cmd.PushConstants(0, sizeof(lp), &lp, smvk::ShaderStage::All);
			cmd.Draw(coneLineVerts, 1, 0, 0);
		}
	}
	);
	// Filtered-path consume hints: the filter compute writes the
	// compacted index list + indirect commands in the SAME submit Main
	// reads them from (vertex index fetch, indirect fetch, VS BDA reads
	// of the draws ring). Without these the graph emits no compute->
	// vertex barrier and the fetches race the CS â€” geometry appears and
	// vanishes "randomly" whenever workload/timing shifts (MSAA mode,
	// light toggles). Covers the meshlet debug view too (cluster-ordered
	// heap bind).
	if (m_useGfxFilter && gfxSlot != UINT32_MAX)
		m_gfxRenderer.AddMainConsumeHints(mainPass, gfxSlot);
	// Forward+ tile lists are device-written by the cull pass and
	// device-read here: explicit read hints order cull -> main and emit
	// the compute->fragment buffer barriers. With deferred shadows ON the
	// lighting pass instead reads the phase-4 outputs (final light list,
	// visibility buffer, tile state) â€” same fragment-stage read pattern.
	if (dsOn) {
		const smvk::DeferredShadows& ds = m_viewportResources.deferredShadows;
		smvk::DeferredShadows::AddMainReadHints(mainPass,
			ds.TileStateLogical(fb.frameIndex),
			ds.LightListLogical(fb.frameIndex),
			ds.VisibilityLogical(fb.frameIndex));
	} else {
		smvk::ForwardPlus::AddTileReadHints(mainPass,
			m_viewportResources.forwardPlus.tileCountsLogical,
			m_viewportResources.forwardPlus.tileIndicesLogical);
	}
	// Decal tile lists are read by Main in BOTH shading paths (forward+
	// light lists or deferred-shadow lists): the decal leg of the tile
	// cull wrote them this frame (DecalsActive gates the dispatch and
	// the FrameUniforms count together).
	if (decalListsNeeded) {
		smvk::ForwardPlus::AddDecalTileReadHints(mainPass,
			m_viewportResources.forwardPlus.decalTileCountsLogical,
			m_viewportResources.forwardPlus.decalTileIndicesLogical);
		smvk::BufferSyncRequest decalRead{};
		decalRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
		decalRead.barrierMasks.stageMask = smvk::PipelineStageBit_Fragment;
		mainPass.AddBufferHint(m_resources.decalsLogical, decalRead);
	}
		{
			smvk::BufferSyncRequest fpLightRead{};
			fpLightRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
			fpLightRead.barrierMasks.stageMask = smvk::PipelineStageBit_Fragment;
			mainPass.AddBufferHint(m_resources.fpLightsLogical, fpLightRead);
		}
		// Spot-shadow tiles are host-uploaded per frame and sampled in the
		// main fragment shader: hint orders the upload vs. the read.
		if (m_spotShadowCount > 0) {
			smvk::BufferSyncRequest spotRead{};
			spotRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
			spotRead.barrierMasks.stageMask = smvk::PipelineStageBit_Fragment;
			mainPass.AddBufferHint(m_resources.spotShadowLogical, spotRead);
		}
		// EVSM: the main fragment shader samples the blurred moments â€”
		// the hint is the topological edge V-blur -> Main (without it Main
		// could be sorted â€” and executed â€” before this frame's blur, and
		// the async wait would never be inserted). The slot follows the
		// pipeline mode: SameFrame reads what this frame blurs (real
		// edge); OneFrameLag reads the other slot (no same-frame edge,
		// previous frame's compute retired by the slot wait in Execute).
		if (EvsmActive()) {
			smvk::ImageSyncRequest evsmRead{};
			evsmRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
			evsmRead.barrierMasks.stageMask = smvk::PipelineStageBit_Fragment;
			evsmRead.layout = smvk::ImageLayout::ShaderReadOnlyOptimal;
			const uint32_t sampleSlot = m_viewportResources.evsmDir.SampleSlot(fb.frameIndex);
			mainPass.AddImageHint(m_viewportResources.evsmDir.blurredLogical[sampleSlot], evsmRead);
			mainPass.AddImageHint(m_viewportResources.evsmSpot.blurredLogical[sampleSlot], evsmRead);
		}

	// ---------------------------------------------------------------------
	// Motion blur â€“ velocity gather in HDR (see motionblur.h). Runs after
	// Main, before Bloom/Tonemap so both consume the blurred HDR.
	// ---------------------------------------------------------------------
		// Scene HDR for downstream passes: blurred output when the pass ran,
		// sharp HDR otherwise (no copy when disabled).
		smvk::LogicalImageViewHandle sceneHdrView = m_viewportResources.hdr.logicalView;
		uint32_t sceneHdrBindless = m_viewportResources.hdr.bindlessIndex;
		{
			SMVK_PROFILE_CATEGORY("MotionBlur", Optick::Category::Rendering);
			smvk::MotionBlurDesc blurDesc = MakeMotionBlurDesc();
			blurDesc.sampleCount = (uint32_t)m_motionBlurSamples;
			blurDesc.maxRadiusPx = m_motionBlurMaxRadius;
			blurDesc.velocityScale = m_motionBlurStrength;
			const bool blurOn = m_passes.motionBlur && m_motionBlurEnabled
				&& m_viewportResources.motionBlur.Valid()
				&& m_viewportResources.motionVectors.Valid();
			if (blurOn) {
				m_viewportResources.motionBlur.AddMotionBlurPass(
					*m_rendergraph,
					m_viewportResources.hdr.logicalView,
					m_viewportResources.hdr.bindlessIndex,
					m_viewportResources.motionVectors.target.logicalView,
					m_viewportResources.motionVectors.target.bindlessIndex,
					linearSampler,
					renderExtent,
					blurDesc,
					m_jitterDeltaUV.x,
					m_jitterDeltaUV.y,
					"MotionBlur");
				sceneHdrView = m_viewportResources.motionBlur.OutputView();
				sceneHdrBindless = m_viewportResources.motionBlur.OutputBindless();
			}
		}

		// ---------------------------------------------------------------------
		// Depth of field â€“ HDR CoC blur (see dof.h). Runs after Main +
		// motion blur, before exposure/bloom/tonemap so the blurred HDR is
		// what they meter/sample. Low = single-pass 12-tap Poisson gather;
		// HQ = full-res signed-CoC prepass + 48-tap hexagonal bokeh gather
		// with depth-aware foreground weights + highlight boost.
		// ---------------------------------------------------------------------
		bool dofEmitted = false;
		{
			SMVK_PROFILE_CATEGORY("DOF", Optick::Category::Rendering);
			smvk::DofDesc dofDesc = MakeDofDesc();
			dofDesc.hqBokeh = m_dofHqBokeh;
			dofDesc.focusDistance = m_dofFocusDistance;
			dofDesc.focusRange = m_dofFocusRange;
			dofDesc.maxRadiusPx = m_dofMaxRadius;
			dofDesc.bokehMaxRadiusPx = m_dofBokehMaxRadius;
			dofDesc.highlightBoost = m_dofHighlightBoost;
			dofDesc.highlightThreshold = m_dofHighlightThreshold;
			const bool dofOn = m_passes.dof && m_dofEnabled
				&& m_viewportResources.dof.Valid()
				&& !m_viewportResources.renderDepth.logicalView.empty()
				&& m_viewportResources.renderDepth.bindlessIndex != UINT32_MAX;
			if (dofOn) {
				m_viewportResources.dof.AddDofPasses(
					*m_rendergraph,
					sceneHdrView,
					sceneHdrBindless,
					m_viewportResources.renderDepth.logicalView,
					m_viewportResources.renderDepth.bindlessIndex,
					frameUniformsAddress,
					linearSampler,
					renderExtent,
					dofDesc);
				sceneHdrView = m_viewportResources.dof.OutputView();
				sceneHdrBindless = m_viewportResources.dof.OutputBindless();
				dofEmitted = true;
			}
		}

		// ---------------------------------------------------------------------
		// Screen-space reflections – half-res trace + bilateral blur +
		// full-res composite (see ssr.h). Runs after DOF, before
		// exposure/bloom/tonemap so reflections meter/bloom/tonemap with
		// the scene. Normals + Fresnel reconstruct from the 1x prepass
		// depth (no GBuffer); Bistro shopfront glass at street grazing
		// angles is the calibration case.
		// ---------------------------------------------------------------------
		bool ssrEmitted = false;
		{
			SMVK_PROFILE_CATEGORY("SSR", Optick::Category::Rendering);
			smvk::SsrDesc ssrDesc = MakeSsrDesc();
			ssrDesc.steps = (uint32_t)std::clamp(m_ssrSteps, 1, 128);
			ssrDesc.refineSteps = (uint32_t)std::clamp(m_ssrRefineSteps, 0, 8);
			ssrDesc.maxDistance = m_ssrMaxDistance;
			ssrDesc.thickness = m_ssrThickness;
			ssrDesc.intensity = m_ssrIntensity;
			ssrDesc.f0 = m_ssrF0;
			ssrDesc.blurSharpness = m_ssrBlurSharpness;
			const bool ssrOn = m_passes.ssr && m_ssrEnabled
				&& m_viewportResources.ssr.Valid()
				&& !m_viewportResources.renderDepth.logicalView.empty()
				&& m_viewportResources.renderDepth.bindlessIndex != UINT32_MAX
				&& m_ssrIntensity > 0.0f;
			if (ssrOn) {
				m_viewportResources.ssr.AddSsrPasses(
					*m_rendergraph,
					sceneHdrView,
					sceneHdrBindless,
					m_viewportResources.renderDepth.logicalView,
					m_viewportResources.renderDepth.bindlessIndex,
					frameUniformsAddress,
					linearSampler,
					ssrDesc,
					"SSR",
					"SSRBlur",
					"SSRComposite");
				sceneHdrView = m_viewportResources.ssr.OutputView();
				sceneHdrBindless = m_viewportResources.ssr.OutputBindless();
				ssrEmitted = true;
			}
		}

		bool volumetricEmitted = false;
		{
			SMVK_PROFILE_CATEGORY("Volumetric", Optick::Category::Rendering);
			smvk::VolumetricDesc volDesc = MakeVolumetricDesc();
			volDesc.steps = (uint32_t)std::clamp(m_volumetricSteps, 4, 128);
			volDesc.maxDistance = m_volumetricMaxDistance;
			volDesc.density = m_volumetricDensity;
			volDesc.intensity = m_volumetricIntensity;
			volDesc.anisotropy = m_volumetricAnisotropy;
			volDesc.sunScale = m_volumetricSun;
			volDesc.giScale = m_volumetricGi;
			volDesc.useGi = m_volumetricUseGi ? 1u : 0u;
			volDesc.useSun = m_volumetricUseSun ? 1u : 0u;
			volDesc.jitterStrength = m_volumetricJitter;
			volDesc.heightFalloff = m_volumetricHeightFalloff;
			volDesc.heightBase = m_volumetricHeightBase;
			m_viewportResources.volumetric.Ensure(*m_context, *m_rendergraph, renderExtent, volDesc);
			const bool volOn = m_passes.volumetric && m_volumetricEnabled
				&& m_viewportResources.volumetric.Valid()
				&& !m_viewportResources.renderDepth.logicalView.empty()
				&& m_viewportResources.renderDepth.bindlessIndex != UINT32_MAX
				&& m_volumetricDensity > 0.0f && m_volumetricIntensity > 0.0f;
			if (volOn) {
				m_viewportResources.volumetric.AddVolumetricPass(
					*m_rendergraph,
					sceneHdrView,
					sceneHdrBindless,
					m_viewportResources.renderDepth.logicalView,
					m_viewportResources.renderDepth.bindlessIndex,
					frameUniformsAddress,
					linearSampler,
					renderExtent,
					frameData.time,
					volDesc,
					"Volumetric");
				sceneHdrView = m_viewportResources.volumetric.OutputView();
				sceneHdrBindless = m_viewportResources.volumetric.OutputBindless();
				volumetricEmitted = true;
			}
		}

		// ---------------------------------------------------------------------
		// Exposure histogram â€“ log2-luminance meter of the scene HDR
		// (see exposure.h). Runs before Bloom/Tonemap so it meters exactly
		// what they consume. UpdateAutoExposure() downloads + adapts FIRST
		// (this slot retired at Acquire), then the CPU zeroes the buffer
		// (transfer write) and the compute pass accumulates for a future
		// frame's tonemap/bloom.
		// ---------------------------------------------------------------------
		{
			SMVK_PROFILE_CATEGORY("ExposureHistogram", Optick::Category::Rendering);
			auto& meter = m_viewportResources.exposure;
			const bool meterGate = m_passes.exposure && m_autoExposureEnabled &&
				meter.Valid() && meter.ShadersValid() &&
				!sceneHdrView.empty() && sceneHdrBindless != UINT32_MAX &&
				renderExtent.width != 0 && renderExtent.height != 0;
			// One-run diagnostic (throttled): WHY the meter is quiet.
			{
				static uint64_t s_expRecTick = 0;
				const uint64_t tick = s_expRecTick++;
				if (!meterGate && (tick < 5 || tick % 600 == 0)) {
					std::fprintf(stderr,
						"[exposure] record SKIP tick=%llu pass=%d auto=%d valid=%d shaders=%d hdr=%d,%u %ux%u\n",
						(unsigned long long)tick,
						m_passes.exposure ? 1 : 0, m_autoExposureEnabled ? 1 : 0,
						meter.Valid() ? 1 : 0, meter.ShadersValid() ? 1 : 0,
						!sceneHdrView.empty() ? 1 : 0, sceneHdrBindless,
						renderExtent.width, renderExtent.height);
				}
			}
		// Combined GPU->CPU readback tick (histogram + texture-streaming
		// feedback, one shared submit + wait on feedback frames). Runs
		// even when the exposure meter itself is gated off so feedback
		// consume/clear never stalls. Downloads precede the zero-fill
		// below (same queue, FIFO).
		rfT2 = bench_clk::now();
		UpdateReadbacks(fb.frameIndex,
			static_cast<uint32_t>(m_frameCounter));
		rfT3 = bench_clk::now();
			if (meterGate) {
				// Reset the slice's bins (transfer write; the compute hint
				// below transitions the buffer to Compute R/W via barrier).
				// Per-flight slice (see exposure.h): the zero-fill submits
				// immediately at record time, so sharing one buffer across
				// flights would let this frame's zero clobber the previous
				// frame's still-accumulating bins.
				static constexpr uint32_t kZeroBins[smvk::kExposureBinCount] = {};
				m_context->UploadDataToBuffer(meter.HistogramBuffer(fb.frameIndex),
					kZeroBins, sizeof(kZeroBins),
					smvk::PipelineStageBit_Transfer,
					smvk::AccessFlags::TransferWrite, 0, 0);
				smvk::ExposureHistogramPush expPush{};
				expPush.histogramAddress = meter.HistogramAddress(*m_context, fb.frameIndex);
				expPush.hdrTextureIndex = sceneHdrBindless;
				expPush.width = renderExtent.width;
				expPush.height = renderExtent.height;
				expPush.minLogLum = meter.desc.minLogLum;
				expPush.maxLogLum = meter.desc.maxLogLum;
				expPush.binCount = meter.desc.bins;
				expPush.sampleStep = meter.desc.sampleStep;
				const uint32_t dx =
					smvk::ExposureMeter::DispatchXFor(renderExtent, meter.desc.sampleStep);
				const uint32_t dy =
					smvk::ExposureMeter::DispatchYFor(renderExtent, meter.desc.sampleStep);
				if (expPush.histogramAddress != 0 && dx != 0 && dy != 0) {
					{
						static uint64_t s_expGoTick = 0;
						const uint64_t tick = s_expGoTick++;
						if (tick < 5 || tick % 600 == 0) {
							std::fprintf(stderr,
								"[exposure] record GO tick=%llu slice=%u bda=0x%llx dx=%u dy=%u hdrIdx=%u %ux%u\n",
								(unsigned long long)tick, fb.frameIndex,
								(unsigned long long)expPush.histogramAddress,
								dx, dy, expPush.hdrTextureIndex,
								renderExtent.width, renderExtent.height);
						}
					}
					smvk::ShaderProgram histProg = meter.histogramProgram;
					smvk::RenderPass& histPass = meter.AddHistogramPass(
						*m_rendergraph, sceneHdrView, expPush, fb.frameIndex, "ExposureHistogram");
					histPass.SetRecordingFunc(
						[histProg, expPush, dx, dy](smvk::CommandBuffer& cmd) mutable {
							cmd.BindComputePipeline(histProg);
							cmd.PushConstants(0, sizeof(smvk::ExposureHistogramPush),
								&expPush, smvk::ShaderStage::All);
							cmd.Dispatch(dx, dy, 1);
						});
				}
			}
		}

		// ---------------------------------------------------------------------
		// Bloom â€“ HDR downsample / upsample chain (composable, see bloom.h)
		// ---------------------------------------------------------------------
		{
			SMVK_PROFILE_CATEGORY("Bloom", Optick::Category::Rendering);
			if (m_passes.bloom && m_bloomEnabled && m_viewportResources.bloom.Valid()) {
				// Default down/up programs owned by BloomChain (Ensure internalized).
				// Samples the (possibly motion-blurred) scene HDR.
				m_viewportResources.bloom.AddBloomPasses(
					*m_rendergraph,
					sceneHdrView,
					sceneHdrBindless,
					linearSampler);
			}
		}

		// ---------------------------------------------------------------------
		// DLSS â€“ Streamline super resolution (see Engine/Rendergraph/dlss.h).
		// HDR render-res -> HDR swap-res, replacing the tonemapper's
		// bilinear upscale: after this block sceneHdrView/Bindless point at
		// the upscaled output the tonemapper samples at swapExtent.
		// ---------------------------------------------------------------------
		bool dlssEmitted = false;
		if (m_dlssFrameActive) {
			SMVK_PROFILE_CATEGORY("DLSS", Optick::Category::Rendering);
			// History reset: first DLSS record, deactivation gap, mode or
			// extent change, or a broken camera history (same gate the
			// motion-vector pass falls back from).
			const bool dlssReset = m_dlssReset || !m_prevViewProjValid
				|| m_dlssMode != m_dlssLastMode
				|| m_dlssLastRenderExtent.width != renderExtent.width
				|| m_dlssLastRenderExtent.height != renderExtent.height
				|| m_dlssLastSwapExtent.width != swapExtent.width
				|| m_dlssLastSwapExtent.height != swapExtent.height;

			// Color in = the (possibly motion-blurred, DOF'd, SSR'd,
			// volumetric-fogged) HDR
			// the bloom chain just sampled: same target sceneHdrView tracks.
			// Physical handles follow the same chain (Volumetric > SSR > DOF > blur > HDR).
			smvk::ImageHandle dlssColorImage =
				static_cast<smvk::ImageHandle>(m_viewportResources.hdr.image);
			smvk::ImageViewHandle dlssColorView =
				static_cast<smvk::ImageViewHandle>(m_viewportResources.hdr.view);
			if (volumetricEmitted) {
				dlssColorImage = static_cast<smvk::ImageHandle>(m_viewportResources.volumetric.image);
				dlssColorView = static_cast<smvk::ImageViewHandle>(m_viewportResources.volumetric.view);
			} else if (ssrEmitted) {
				dlssColorImage = static_cast<smvk::ImageHandle>(m_viewportResources.ssr.image);
				dlssColorView = static_cast<smvk::ImageViewHandle>(m_viewportResources.ssr.view);
			} else if (dofEmitted) {
				dlssColorImage = static_cast<smvk::ImageHandle>(m_viewportResources.dof.output.image);
				dlssColorView = static_cast<smvk::ImageViewHandle>(m_viewportResources.dof.output.view);
			} else {
				const bool blurOn = m_passes.motionBlur && m_motionBlurEnabled
					&& m_viewportResources.motionBlur.Valid()
					&& m_viewportResources.motionVectors.Valid();
				if (blurOn) {
					dlssColorImage = static_cast<smvk::ImageHandle>(m_viewportResources.motionBlur.image);
					dlssColorView = static_cast<smvk::ImageViewHandle>(m_viewportResources.motionBlur.view);
				}
			}

			smvk::DlssFrameInputs dlssIn{};
			dlssIn.colorInImage = dlssColorImage;
			dlssIn.colorInView = dlssColorView;
			dlssIn.colorInLogical = sceneHdrView;
			dlssIn.colorOutImage = m_viewportResources.dlss.output.image;
			dlssIn.colorOutView = m_viewportResources.dlss.output.view;
			dlssIn.colorOutLogical = m_viewportResources.dlss.output.logicalView;
			dlssIn.depthImage = m_viewportResources.renderDepth.image;
			dlssIn.depthView = m_viewportResources.renderDepth.view;
			dlssIn.depthLogical = m_viewportResources.renderDepth.logicalView;
			dlssIn.mvecImage = m_viewportResources.motionVectors.target.image;
			dlssIn.mvecView = m_viewportResources.motionVectors.target.view;
			dlssIn.mvecLogical = m_viewportResources.motionVectors.target.logicalView;
			dlssIn.exposureImage = m_viewportResources.dlss.exposure.image;
			dlssIn.exposureView = m_viewportResources.dlss.exposure.view;
			dlssIn.exposureLogical = m_viewportResources.dlss.exposure.logicalView;
			dlssIn.renderExtent = renderExtent;
			dlssIn.outputExtent = swapExtent;
			dlssIn.colorFormat = m_viewportResources.hdr.format;
			dlssIn.depthFormat = m_viewportResources.renderDepth.format;
			dlssIn.mvecFormat = m_viewportResources.motionVectors.target.format;
			dlssIn.exposureFormat = m_viewportResources.dlss.exposure.format;

			// Constants: ROW-major, UNJITTERED matrices (SL convention).
			// clipToPrevClip = prevUnjitteredVP * inverse(currUnjitteredVP)
			// maps a current clip point to the previous frame's clip â€”
			// the same camera history the motion-vector pass diffs, minus
			// the per-frame jitter.
			auto toRowMajor = [](const glm::mat4& m, float out[16]) {
				const glm::mat4 t = glm::transpose(m);
				std::memcpy(out, glm::value_ptr(t), sizeof(float) * 16);
			};
			const glm::mat4 currUnjitteredVP = m_unjitteredViewProj;
			const glm::mat4 prevUnjitteredVP =
				m_prevViewProjValid ? m_prevUnjitteredViewProj : currUnjitteredVP;
			const glm::mat4 clipToPrevClip =
				prevUnjitteredVP * glm::inverse(currUnjitteredVP);
			toRowMajor(m_unjitteredProj, dlssIn.cameraViewToClip);
			toRowMajor(glm::inverse(m_unjitteredProj), dlssIn.clipToCameraView);
			toRowMajor(clipToPrevClip, dlssIn.clipToPrevClip);
			toRowMajor(glm::inverse(clipToPrevClip), dlssIn.prevClipToClip);
			dlssIn.jitterOffset[0] = m_dlssJitter.x;
			dlssIn.jitterOffset[1] = m_dlssJitter.y;
			const glm::vec3 camFwd = glm::normalize(m_cameraPositioner.getForward());
			const glm::vec3 camUp = glm::normalize(m_cameraPositioner.up_);
			const glm::vec3 camRight = glm::normalize(glm::cross(camFwd, camUp));
			dlssIn.cameraPos[0] = m_cameraPositioner.cameraPosition_.x;
			dlssIn.cameraPos[1] = m_cameraPositioner.cameraPosition_.y;
			dlssIn.cameraPos[2] = m_cameraPositioner.cameraPosition_.z;
			dlssIn.cameraUp[0] = camUp.x; dlssIn.cameraUp[1] = camUp.y; dlssIn.cameraUp[2] = camUp.z;
			dlssIn.cameraRight[0] = camRight.x; dlssIn.cameraRight[1] = camRight.y; dlssIn.cameraRight[2] = camRight.z;
			dlssIn.cameraFwd[0] = camFwd.x; dlssIn.cameraFwd[1] = camFwd.y; dlssIn.cameraFwd[2] = camFwd.z;
			dlssIn.cameraNear = default_near_clip;
			dlssIn.cameraFar = m_scene.farClip;
			dlssIn.cameraFOV = glm::radians(kCameraFovYDegrees);
			dlssIn.cameraAspectRatio =
				float(renderExtent.width) / float(renderExtent.height);
			dlssIn.reset = dlssReset;

			dlssEmitted = smvk::AddDlssPasses(*m_rendergraph, *m_context,
				m_viewportResources.dlss, dlssIn, frameExposure, fb.frameIndex, "DLSS");
			if (dlssEmitted) {
				sceneHdrView = m_viewportResources.dlss.output.logicalView;
				sceneHdrBindless = m_viewportResources.dlss.output.bindlessIndex;
			}
		}

		// ---------------------------------------------------------------------
		// Tonemapper â€“ HDR (RGBA16F) â†’ LDR swapchain (see tonemap.h).
		// With SMAA active the pass is retargeted onto the offscreen LDR
		// target owned by the SMAA builder (the swapchain cannot be
		// sampled); the SMAA resolve then writes the swapchain.
		// ---------------------------------------------------------------------
		const bool smaaActive = SmaaActive();
		const bool smaaT2x = smaaActive && m_aaMode == AaMode::SmaaT2x;
		const bool swapIsSrgb = SwapchainIsSrgb();
		{
			SMVK_PROFILE_CATEGORY("Tonemap", Optick::Category::Rendering);
			const bool bloomOn = m_passes.bloom && m_bloomEnabled && m_viewportResources.bloom.Valid();
			smvk::LogicalImageViewHandle bloomView{};
			uint32_t bloomBindless = UINT32_MAX;
			float bloomIntensity = 0.0f;
			if (bloomOn) {
				bloomView = m_viewportResources.bloom.MipView(0);
				bloomBindless = m_viewportResources.bloom.MipBindless(0);
				bloomIntensity = m_viewportResources.bloom.desc.intensity;
			}
			smvk::LogicalImageViewHandle motionView{};
			uint32_t motionBindless = UINT32_MAX;
			if (m_passes.motionVectors && m_viewportResources.motionVectors.Valid()) {
				motionView = m_viewportResources.motionVectors.target.logicalView;
				motionBindless = m_viewportResources.motionVectors.target.bindlessIndex;
			}
		std::vector<smvk::LogicalImageViewHandle> tonemapTargets = colors;
		if (smaaActive)
			tonemapTargets = { m_viewportResources.smaa.ldr.logicalView };
		const bool lutOn = !m_lutFile.empty()
			&& m_lutBindless != UINT32_MAX && m_lutIntensity > 0.0f;
		// Screenshot frame: the composite is drawn once more into an sRGB
		// capture target (the tonemap PS writes a single SV_Target, so a
		// multi-attachment pass would leave the capture at its clear value).
		// Scene-only on purpose - the capture is a grading reference.
		bool captureThisFrame = false;
		// Env-requested capture (VULKANTOY_CAPTURE=<frame>): F7-less A/B.
		if (m_captureAtFrame >= 0 && m_frameCounter == (uint64_t)m_captureAtFrame)
			m_captureQueued = true;
		if (m_captureQueued) {
			EnsureCaptureTarget(swapExtent);
			captureThisFrame = !m_captureLogical.empty();
		}
		smvk::TonemapDesc tonemapDesc = m_viewportResources.tonemap.desc;
		tonemapDesc.exposure = frameExposure;
		auto& tonemapPass = m_viewportResources.tonemap.AddTonemapPass(
			*m_rendergraph,
			sceneHdrView,
			sceneHdrBindless,
			bloomView,
			bloomBindless,
			bloomIntensity,
			linearSampler,
			tonemapTargets,
			swapExtent,
			tonemapDesc,
			"Tonemap",
			motionView,
			motionBindless,
			m_showMotionDebug ? 1u : 0u,
			m_motionDebugScale,
			{},
			UINT32_MAX,
			0.0f,
			smaaActive || !swapIsSrgb,
			lutOn ? m_lutBindless : UINT32_MAX,
			m_lutIntensity,
			m_jitterDeltaUV.x,
			m_jitterDeltaUV.y);
		(void)tonemapPass;
		if (captureThisFrame) {
			m_viewportResources.tonemap.AddTonemapPass(
				*m_rendergraph,
				sceneHdrView,
				sceneHdrBindless,
				bloomView,
				bloomBindless,
				bloomIntensity,
				linearSampler,
				{ m_captureLogical },
				swapExtent,
				tonemapDesc,
				"TonemapCapture",
				motionView,
				motionBindless,
				m_showMotionDebug ? 1u : 0u,
				m_motionDebugScale,
				{},
				UINT32_MAX,
				0.0f,
				false,
				lutOn ? m_lutBindless : UINT32_MAX,
				m_lutIntensity,
				m_jitterDeltaUV.x,
				m_jitterDeltaUV.y);
		}
		}

		// ---------------------------------------------------------------------
		// SMAA â€“ LDR post-tonemap antialiasing (see Engine/Rendergraph/smaa.h):
		// luma edges â†’ blend weights â†’ neighborhood blending, plus the T2x
		// temporal resolve over the velocity buffer.
		// ---------------------------------------------------------------------
		if (smaaActive) {
			SMVK_PROFILE_CATEGORY("SMAA", Optick::Category::Rendering);
			m_smaaFrameRanT2x = smaaT2x;
		// Official T2x subsample table (@SUBSAMPLE_INDICES in
		// smaa.hlsli): phase A/B â†’ (1,1,1,0)/(2,2,2,0); SMAA 1x â†’ zeros.
		// Phase/slot/valid come from this record's global snapshot, so
		// the triple always agrees.
			float subsample[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			if (smaaT2x && m_smaaFrameParams.parity == 0) {
				subsample[0] = subsample[1] = subsample[2] = 1.0f;
			} else if (smaaT2x) {
				subsample[0] = subsample[1] = subsample[2] = 2.0f;
			}
			m_viewportResources.smaa.AddSmaaPasses(
				*m_rendergraph,
				m_viewportResources.motionVectors.target.logicalView,
				m_viewportResources.motionVectors.target.bindlessIndex,
				linearSampler,
				pointSampler,
				smaaT2x,
				m_smaaFrameParams.historyValid,
				subsample,
				m_smaaFrameParams.parity,
				colors,
				swapExtent,
				swapIsSrgb,
				m_jitterDeltaUV.x,
				m_jitterDeltaUV.y);
		}

		// ---------------------------------------------------------------------
		// ImGui â€“ build UI (before adding the rendergraph pass)
		// ---------------------------------------------------------------------
		if (m_passes.ui && m_imgui && m_imgui->IsInitialized() && m_viewportResources.ui.Valid()) {
			// Target extent != window size when ui scale != 1: the overlay maps
			// ImGui draw data + font raster density onto the UI target.
			m_imgui->NewFrame(uiExtent);

		// Fixed debug panel: full-height side bar (Right/Left, U toggles side).
		// Position + size enforced every frame (NoMove/NoResize/NoCollapse):
		// never floating, never lost behind the scene. Content is classified
		// into Quality (settings) / Debug (views + overlays) / Stats (readback)
		// tabs; the perf line above the tabs stays always visible.
		{
			const ImVec2 disp = ImGui::GetIO().DisplaySize;
			const float pad = 8.0f;
			const float sideW = 480.0f;
			ImVec2 pos{ 0, 0 }, size{ 0, 0 };
			if (m_uiDock == UiDock::Left) {
				pos = ImVec2(pad, pad);
				size = ImVec2(sideW, disp.y - pad * 2.0f);
			} else {
				pos = ImVec2(disp.x - sideW - pad, pad);
				size = ImVec2(sideW, disp.y - pad * 2.0f);
			}
			ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
			ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		}
		ImGui::Begin("VulkanToy ï¿½ Debug", nullptr,
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
		{
			// Dock selector + cycle button (same action as the U shortcut).
			const char* dockNames[] = { "Right", "Left" };
			int dockIdx = static_cast<int>(m_uiDock);
			if (ImGui::Combo("UI dock", &dockIdx, dockNames, 2))
				SetUiDock(static_cast<UiDock>(dockIdx));
			ImGui::SameLine();
			if (ImGui::Button("Cycle (U)"))
				CycleUiDock();
		}
		ImGui::Text("Frame %.1f FPS (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
		if (m_gpuTimingValid)
			ImGui::Text("CPU %.3f ms (wait %.3f ms)  GPU %.3f ms", m_cpuMsSmooth, m_waitMsSmooth, m_gpuMsSmooth);
		else
			ImGui::Text("CPU %.3f ms (wait %.3f ms)  GPU ... (warming up)", m_cpuMsSmooth, m_waitMsSmooth);
		if (ImGui::BeginTabBar("VulkanToyTabs")) {
			// ---------------------------------------------------------
			// Quality: every setting that changes the image. No readback
			// here besides the control itself (numbers live in Stats).
			// ---------------------------------------------------------
			if (ImGui::BeginTabItem("Quality")) {
				if (ImGui::CollapsingHeader("Frame ï¿½ present + resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
				// VSync / presentation mode. Applies on the next frame via
				// Recreate (see DrawFrame); the active mode may fall back when
				// the driver lacks the requested one. FIFO is strict (missed
				// 16.6ms -> 30fps lock); Mailbox stays tear-free at 60Hz;
				// FIFO relaxed vsyncs on time and never locks to half rate.
				const char* modeNames[] = { "VSync FIFO (strict)", "VSync Mailbox (smooth)",
					"Uncapped Immediate", "VSync FIFO relaxed (no 30fps lock)" };
					int modeIdx = static_cast<int>(m_presentMode);
					if (ImGui::Combo("Present mode", &modeIdx, modeNames, 4))
						m_presentMode = static_cast<smvk::PresentMode>(modeIdx);
					ImGui::SliderFloat("Render scale", &m_renderScale,
						RENDER_SCALE_MIN, RENDER_SCALE_MAX, "%.2fx");
					ImGui::SliderFloat("UI res scale", &m_uiScale,
						UI_SCALE_MIN, UI_SCALE_MAX, "%.2fx");
					ImGui::SliderFloat("UI DPI scale", &m_uiDpiScale,
						UI_DPI_MIN, UI_DPI_MAX, "%.2fx");
					if (ImGui::IsItemEdited() && m_imgui)
						m_imgui->ApplyDpiScale(m_uiDpiScale);
					ImGui::TextDisabled("Render scale resizes HDR/MSAA/bloom/depth + tonemap upscales to swap.\n"
						"UI renders into its own target and composites over the swapchain.");
				}
				if (ImGui::CollapsingHeader("DLSS upscaling (Streamline)", ImGuiTreeNodeFlags_DefaultOpen)) {
					// Conventional order for the combo; indices remap to the
					// sl-aligned DlssMode enum values.
					const char* dlssNames[] = { "Off", "Ultra Performance", "Performance",
						"Balanced", "Quality", "Ultra Quality", "DLAA" };
					const int dlssEnumOf[] = {
						static_cast<int>(smvk::DlssMode::Off),
						static_cast<int>(smvk::DlssMode::UltraPerformance),
						static_cast<int>(smvk::DlssMode::MaxPerformance),
						static_cast<int>(smvk::DlssMode::Balanced),
						static_cast<int>(smvk::DlssMode::MaxQuality),
						static_cast<int>(smvk::DlssMode::UltraQuality),
						static_cast<int>(smvk::DlssMode::DLAA),
					};
					int dlssSel = 0;
					for (int i = 0; i < 7; ++i)
						if (dlssEnumOf[i] == static_cast<int>(m_dlssMode))
							dlssSel = i;
					if (ImGui::Combo("DLSS mode", &dlssSel, dlssNames, 7))
						m_dlssMode = static_cast<smvk::DlssMode>(dlssEnumOf[dlssSel]);
					if (m_dlssFrameActive) {
						ImGui::Text("Active: scene %ux%u -> DLSS -> swap %ux%u",
							renderExtent.width, renderExtent.height,
							swapExtent.width, swapExtent.height);
						ImGui::Text("Halton jitter phase %u / %u", m_dlssPhase, m_dlssPhaseCount);
						ImGui::TextDisabled("DLSS replaces the AA modes and the render-scale slider\n"
							"(HDR upscaled before tonemap; UI composites after).");
					} else if (DlssRequested()) {
						// Why the requested mode is not running.
						if (!smvk::DlssAvailable())
							ImGui::TextDisabled("Streamline runtime (sl.interposer.dll) not loaded.");
						else if (!m_dlssSupported)
							ImGui::TextDisabled("DLSS not supported on this adapter (NVIDIA GPU + up-to-date driver).");
						else if (!m_passes.motionVectors)
							ImGui::TextDisabled("Requires the motion-vector pass (ScenePassConfig).");
						else
							ImGui::TextDisabled("Warming up / options pending this frame.");
					} else {
						ImGui::TextDisabled("Off: render scale slider drives the internal resolution\n"
							"(tonemap upscales bilinearly).");
					}
				}
				if (ImGui::CollapsingHeader("Antialiasing + transparency", ImGuiTreeNodeFlags_DefaultOpen)) {
					// AA mode combo (Off / MSAA 2x / SMAA 1x / SMAA T2x). msaaEnabled
					// is derived from the mode every frame (see the Ensure block), so
					// the combo is the single source of truth. DLSS replaces all
					// AA when active.
					const char* aaNames[] = { "Off", "MSAA 2x", "SMAA 1x", "SMAA T2x" };
					int aaIdx = static_cast<int>(m_aaMode);
					ImGui::BeginDisabled(m_dlssFrameActive);
					if (ImGui::Combo("Antialiasing (Z / 1-4)", &aaIdx, aaNames, 4))
						m_aaMode = static_cast<AaMode>(aaIdx);
					ImGui::EndDisabled();
					if (m_dlssFrameActive)
						ImGui::TextDisabled("AA bypassed while DLSS is active.");
					// Transparency technique for Blend materials (default:
					// dithered screen-door, opaque state; optional blend).
					const char* transpModes[] = { "Dithered (default)", "Alpha blend" };
					int transpIdx = (m_transparentMode == smvk::TransparentMode::Blended) ? 1 : 0;
					if (ImGui::Combo("Transparency (T)", &transpIdx, transpModes, 2))
						m_transparentMode = (transpIdx == 1)
							? smvk::TransparentMode::Blended
							: smvk::TransparentMode::Dithered;
					if (transpIdx == 0)
						ImGui::TextDisabled("Screen-door: depth-writing, order-independent (SMAA T2x integrates it).");
					else
						ImGui::TextDisabled("Classic back-to-front blend: no depth write, sorted.");
				}
				if (ImGui::CollapsingHeader("Exposure ï¿½ histogram eye adaptation", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Auto-exposure (X)", &m_autoExposureEnabled);
					ImGui::SliderFloat("Manual exposure", &m_manualExposure,
						0.00005f, 0.02f, "%.5f", ImGuiSliderFlags_Logarithmic);
					ImGui::SliderFloat("EV bias", &m_exposureEvBias, -4.0f, 4.0f, "%.1f");
					ImGui::SliderFloat("Adapt to dark (/s)", &m_exposureSpeedToDark, 0.1f, 8.0f, "%.2f");
					ImGui::SliderFloat("Adapt to bright (/s)", &m_exposureSpeedToBright, 0.1f, 8.0f, "%.2f");
					ImGui::SliderFloat("Min exposure", &m_exposureMin,
						0.000001f, 0.001f, "%.6f", ImGuiSliderFlags_Logarithmic);
					ImGui::SliderFloat("Max exposure", &m_exposureMax,
						0.005f, 0.1f, "%.4f", ImGuiSliderFlags_Logarithmic);
					if (m_exposureMin > m_exposureMax)
						std::swap(m_exposureMin, m_exposureMax);
				}
				if (ImGui::CollapsingHeader("Environment ï¿½ Skydome IBL + Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Sky dome (K)", &m_env.skyEnabled);
					// Ambient-source selection moved to the GI section (the
					// sky IBL is one AMBIENT source among the isolatable
					// set — N cycles, see AmbientMode).
					ImGui::TextDisabled("Ambient source: %s (N cycles; GI section)",
						AmbientModeName(m_ambientMode));
					ImGui::ColorEdit3("Zenith", &m_env.skyZenithColor.x);
					ImGui::ColorEdit3("Horizon", &m_env.skyHorizonColor.x);
					ImGui::ColorEdit3("Ground", &m_env.groundColor.x);
					ImGui::SliderFloat("Sky intensity (HDR)", &m_env.skyIntensity, 0.0f, 2000.0f);
					ImGui::SliderFloat("Sky exponent", &m_env.skyExponent, 0.25f, 8.0f);
					ImGui::SliderFloat("IBL intensity (HDR)", &m_env.iblIntensity, 0.0f, 200.0f);
					ImGui::Separator();
					ImGui::Text("Sun (drives light + sky disc)");
					ImGui::SliderFloat("Azimuth", &m_env.sunAzimuthDeg, -180.0f, 180.0f);
					ImGui::SliderFloat("Elevation", &m_env.sunElevationDeg, -10.0f, 90.0f);
					ImGui::ColorEdit3("Sun color", &m_env.sunColor.x);
					ImGui::SliderFloat("Sun intensity (lux)", &m_env.sunIntensity, 0.0f, 30000.0f);
					ImGui::Checkbox("Sun disc", &m_env.sunDiscEnabled);
					ImGui::SliderFloat("Disc radius (deg)", &m_env.sunDiscSizeDeg, 0.1f, 10.0f);
					ImGui::Separator();
					ImGui::Text("Fog (HDR pre-tonemap)");
					const char* fogModes[] = { "Off", "Exp2", "Linear" };
					int fogMode = m_env.fogMode;
					if (ImGui::Combo("Mode", &fogMode, fogModes, 3)) m_env.fogMode = fogMode;
					ImGui::ColorEdit3("Fog color", &m_env.fogColor.x);
					ImGui::SliderFloat("Fog intensity (HDR)", &m_env.fogIntensity, 0.0f, 2000.0f);
					ImGui::SliderFloat("Density", &m_env.fogDensity, 0.0f, 0.1f, "%.4f");
					ImGui::SliderFloat("Start (m)", &m_env.fogStart, 0.0f, 100.0f);
					ImGui::SliderFloat("End (m)", &m_env.fogEnd, 1.0f, 200.0f);
					ImGui::SliderFloat("Height (m)", &m_env.fogHeight, -10.0f, 30.0f);
					ImGui::SliderFloat("Height falloff", &m_env.fogHeightFalloff, 0.0f, 2.0f);
					if (!m_skydome.ShadersValid())
						ImGui::TextDisabled("Sky program missing ï¿½ dome skipped");
				}
				if (ImGui::CollapsingHeader("GI ï¿½ ambient source + intensity", ImGuiTreeNodeFlags_DefaultOpen)) {
					// Ambient source (N cycles): All -> Lightmaps -> Probes
					// -> SkyIbl -> Off — single-source isolation for
					// bisection. R / F8 cycle the GI debug views (Debug
					// tab): flat irradiance + source palette.
					{
						static const char* const kModeNames[5] = {
							"All (baked GI; sky fallback)",
							"Lightmaps only",
							"Probes only (grids + volume)",
							"Sky IBL only",
							"Off (no GI at all)",
						};
						int modeIdx = static_cast<int>(m_ambientMode);
						if (ImGui::Combo("Ambient source (N)", &modeIdx, kModeNames, 5))
							m_ambientMode = static_cast<AmbientMode>(std::clamp(modeIdx, 0, 4));
						ImGui::TextDisabled("N cycles: %s\n"
							"sky IBL is the FALLBACK for uncovered pixels (never added on baked)\n"
							"sources loaded: lightmaps %s, probe volume %s, grids %s\n"
							"R / F8: GI debug views (1 flat irradiance, 2 palette)",
							kModeNames[static_cast<int>(m_ambientMode)],
							m_lmLoaded ? "yes" : "no",
							m_giLoaded ? "yes" : "no",
							m_regLoaded ? "yes" : "no");
					}
					ImGui::SliderFloat("GI intensity", &m_giIntensity, 0.0f, 2.0f, "%.2f");
					if (!m_giLoaded && !m_lmLoaded && !m_regLoaded)
						ImGui::TextDisabled("No baked GI: content/gi/%s.* missing",
							m_scene.id ? m_scene.id : "?");
				}
				if (ImGui::CollapsingHeader("Bloom + AO + motion blur", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Bloom (B)", &m_bloomEnabled);
					{
						static const char* const kAoNames[3] =
							{ "No AO", "SSAO", "GTAO" };
						int aoIdx = static_cast<int>(m_aoMode);
						if (ImGui::Combo("AO mode (O)", &aoIdx, kAoNames, 3))
							m_aoMode = static_cast<AoMode>(std::clamp(aoIdx, 0, 2));
						ImGui::TextDisabled("O cycles AO: %s (half-res, depth only)",
							kAoNames[static_cast<int>(m_aoMode)]);
					}
					if (m_aoMode != AoMode::None) {
						ImGui::SliderFloat("AO radius (m)", &m_ssaoRadius, 0.05f, 2.0f);
						ImGui::SliderFloat("AO bias", &m_ssaoBias, 0.0f, 0.1f);
						ImGui::SliderFloat("AO intensity", &m_ssaoIntensity, 0.0f, 2.0f);
						ImGui::SliderFloat("AO max distance (m)", &m_ssaoMaxDistance, 2.0f, 100.0f);
						ImGui::SliderFloat("AO blur sharpness", &m_ssaoBlurSharpness, 0.0f, 32.0f);
						if (m_aoMode == AoMode::Ssao) {
							ImGui::SliderInt("SSAO samples", &m_ssaoSamples, 1, 32);
						} else {
							ImGui::SliderInt("GTAO slices", &m_gtaoSlices, 1, 8);
							ImGui::SliderInt("GTAO steps / slice", &m_gtaoSteps, 1, 8);
							ImGui::SliderFloat("GTAO thickness (m)", &m_gtaoThickness, 0.0f, 5.0f);
						}
					}
					ImGui::Checkbox("Motion blur (M) (HDR velocity gather)", &m_motionBlurEnabled);
					if (m_motionBlurEnabled) {
						ImGui::SliderInt("Blur samples", &m_motionBlurSamples, 1, 32);
						ImGui::SliderFloat("Blur max radius (px)", &m_motionBlurMaxRadius, 0.0f, 64.0f);
						ImGui::SliderFloat("Blur strength", &m_motionBlurStrength, 0.0f, 2.0f);
					}
				}
				if (ImGui::CollapsingHeader("Depth of field + SSR", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Depth of field (HDR CoC blur)", &m_dofEnabled);
					if (m_dofEnabled) {
						ImGui::Checkbox("HQ bokeh (CoC + 48-tap hex gather)", &m_dofHqBokeh);
						ImGui::SliderFloat("Focus distance (m)", &m_dofFocusDistance, 0.5f, 100.0f);
						ImGui::SliderFloat("Focus range (m)", &m_dofFocusRange, 0.0f, 20.0f);
						ImGui::SliderFloat("Low max radius (px)", &m_dofMaxRadius, 0.0f, 64.0f);
						ImGui::SliderFloat("Bokeh max radius (px)", &m_dofBokehMaxRadius, 0.0f, 64.0f);
						ImGui::SliderFloat("Highlight boost", &m_dofHighlightBoost, 1.0f, 4.0f);
						ImGui::SliderFloat("Highlight threshold", &m_dofHighlightThreshold, 0.0f, 8.0f);
						ImGui::TextDisabled(m_dofHqBokeh
							? "HQ: CoC prepass + hex bokeh (foreground-weighted)."
							: "Low: single-pass 12-tap Poisson (bleeds at silhouettes).");
					}
					ImGui::Checkbox("Screen-space reflections (HDR composite)", &m_ssrEnabled);
					if (m_ssrEnabled) {
						ImGui::SliderInt("Ray steps", &m_ssrSteps, 1, 128);
						ImGui::SliderInt("Refine steps", &m_ssrRefineSteps, 0, 8);
						ImGui::SliderFloat("Max distance (m)", &m_ssrMaxDistance, 1.0f, 200.0f);
						ImGui::SliderFloat("Thickness (m)", &m_ssrThickness, 0.01f, 2.0f);
						ImGui::SliderFloat("Intensity", &m_ssrIntensity, 0.0f, 4.0f);
						ImGui::SliderFloat("F0 (glass 0.04)", &m_ssrF0, 0.0f, 1.0f);
						ImGui::SliderFloat("Bilateral sharpness", &m_ssrBlurSharpness, 0.0f, 32.0f);
						ImGui::TextDisabled("Half-res trace + bilateral blur + depth-aware upscale. Bistro shopfront glass at grazing angles is the test case.");
					}
				}
				if (ImGui::CollapsingHeader("Volumetric fog (GI probes)", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Volumetric fog (HDR raymarch)", &m_volumetricEnabled);
					if (m_volumetricEnabled) {
						static const char* const kVolNames[5] =
							{ "Low (8 steps)", "Medium (16 steps)", "High (32 steps)", "Ultra (64 steps)", "Custom" };
						int volQ = static_cast<int>(m_volumetricQuality);
						if (ImGui::Combo("Quality preset", &volQ, kVolNames, 5)) {
							m_volumetricQuality = static_cast<smvk::VolumetricQuality>(std::clamp(volQ, 0, 4));
							if (m_volumetricQuality != smvk::VolumetricQuality::Custom) {
								smvk::VolumetricDesc qd = MakeVolumetricDesc();
								smvk::ApplyVolumetricQuality(qd, m_volumetricQuality);
								m_volumetricSteps = (int)qd.steps;
							}
						}
						if (ImGui::SliderInt("March steps", &m_volumetricSteps, 4, 128))
							m_volumetricQuality = smvk::VolumetricQuality::Custom;
						ImGui::SliderFloat("Max distance (m)", &m_volumetricMaxDistance, 5.0f, 200.0f);
						ImGui::SliderFloat("Density (1/m)", &m_volumetricDensity, 0.0f, 0.1f, "%.4f");
						ImGui::SliderFloat("Scattering intensity", &m_volumetricIntensity, 0.0f, 4.0f);
						ImGui::SliderFloat("Anisotropy g", &m_volumetricAnisotropy, -0.9f, 0.9f);
						ImGui::SliderFloat("Sun strength", &m_volumetricSun, 0.0f, 4.0f);
						ImGui::SliderFloat("GI strength", &m_volumetricGi, 0.0f, 4.0f);
						ImGui::Checkbox("GI probes ambient", &m_volumetricUseGi);
						ImGui::Checkbox("Sun single-scatter", &m_volumetricUseSun);
						ImGui::SliderFloat("Dither (banding guard)", &m_volumetricJitter, 0.0f, 1.0f);
						ImGui::SliderFloat("Height falloff (1/m)", &m_volumetricHeightFalloff, 0.0f, 1.0f, "%.3f");
						if (m_volumetricHeightFalloff > 0.0f)
							ImGui::SliderFloat("Height base (m)", &m_volumetricHeightBase, -10.0f, 30.0f);
						if (!m_giLoaded)
							ImGui::TextDisabled("No probe volume: ambient falls back to sky IBL.");
						ImGui::TextDisabled("Probes carry sky+sun bounce; sun adds HG forward scatter.");
					}
				}
				if (ImGui::CollapsingHeader("Forward+ lights", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Tiled point/spot lights (F)", &m_fpDemo.enabled);
					ImGui::Checkbox("Animate", &m_fpDemo.animate);
					ImGui::SliderFloat("Speed", &m_fpDemo.speed, 0.0f, 3.0f);
					int gx = m_fpDemo.gridX, gy = m_fpDemo.gridY, gz = m_fpDemo.gridZ;
					if (ImGui::SliderInt("Grid X", &gx, 1, 12)) m_fpDemo.gridX = gx;
					if (ImGui::SliderInt("Grid Y", &gy, 1, 8)) m_fpDemo.gridY = gy;
					if (ImGui::SliderInt("Grid Z", &gz, 1, 12)) m_fpDemo.gridZ = gz;
				ImGui::SliderFloat("Spacing X", &m_fpDemo.spacingX, 0.5f, 8.0f);
				ImGui::SliderFloat("Spacing Y", &m_fpDemo.spacingY, 0.5f, 6.0f);
				ImGui::SliderFloat("Spacing Z", &m_fpDemo.spacingZ, 0.5f, 8.0f);
				ImGui::Checkbox("Fit grid to scene bounds", &m_fpDemo.fitToSceneBounds);
				if (m_fpDemo.fitValid) {
					ImGui::TextDisabled("Bounds min (%.1f, %.1f, %.1f) max (%.1f, %.1f, %.1f)",
						m_fpDemo.fitMin.x, m_fpDemo.fitMin.y, m_fpDemo.fitMin.z,
						m_fpDemo.fitMax.x, m_fpDemo.fitMax.y, m_fpDemo.fitMax.z);
				} else {
					ImGui::TextDisabled("Bounds: unavailable (manual placement)");
				}
				if (ImGui::Button("Refit to scene")) {
					const ::Aabb bounds = m_world->ComputeSceneBounds();
					if (bounds.mx.x >= bounds.mn.x && bounds.mx.y >= bounds.mn.y && bounds.mx.z >= bounds.mn.z)
						m_fpDemo.FitToBounds(bounds.mn, bounds.mx);
				}
					ImGui::SliderFloat("Range", &m_fpDemo.range, 1.0f, 30.0f);
					ImGui::SliderFloat("Intensity (cd)", &m_fpDemo.intensity, 0.0f, 2000.0f);
					int spots = m_fpDemo.spotCount;
					if (ImGui::SliderInt("Spots", &spots, 0, 16)) m_fpDemo.spotCount = spots;
					ImGui::SliderFloat("Spot intensity (cd)", &m_fpDemo.spotIntensity, 0.0f, 4000.0f);
				}
				if (ImGui::CollapsingHeader("Spot shadows + cookies", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Spot shadows (per-light flag)", &m_fpDemo.spotShadowsEnabled);
					int spotMax = m_fpDemo.spotShadowMax;
					if (ImGui::SliderInt("Shadow spots max", &spotMax, 0, (int)smvk::kMaxSpotShadows))
						m_fpDemo.spotShadowMax = spotMax;
					ImGui::SliderFloat("Shadow max distance (m)", &m_fpDemo.spotShadowMaxDistance, 5.0f, 120.0f);
					ImGui::Separator();
					ImGui::Text("Light cookies & functions");
					ImGui::Checkbox("Spot cookies (C) (gobo)", &m_fpDemo.cookiesEnabled);
					ImGui::Checkbox("Light functions (spots)", &m_fpDemo.lightFunctionsEnabled);
					const char* functionNames[] = { "None", "Flicker", "Pulse", "Cone bands" };
					int fn = m_fpDemo.functionType;
					if (ImGui::Combo("Function", &fn, functionNames, 4))
						m_fpDemo.functionType = fn;
				ImGui::SliderFloat("Fn speed", &m_fpDemo.functionSpeed, 0.05f, 5.0f);
				ImGui::SliderFloat("Fn depth", &m_fpDemo.functionScale, 0.0f, 1.0f);
			}
				if (ImGui::CollapsingHeader("Decals (splats)", ImGuiTreeNodeFlags_DefaultOpen)) {
					// Forward+ tiled decals: per-tile lists refreshed by the
					// same cull dispatch as the lights; splats re-derive
					// every frame so these sliders retouch live decals.
					ImGui::Checkbox("Enable decals", &m_decalDemo.enabled);
					ImGui::SameLine();
					ImGui::TextDisabled(DecalsActive() ? "(active)" : "(off)");
					ImGui::SliderFloat("Opacity", &m_decalDemo.opacity, 0.0f, 1.0f);
					ImGui::SliderFloat("Normal strength", &m_decalDemo.normalStrength, 0.0f, 2.0f);
					ImGui::SliderFloat("Roughness target", &m_decalDemo.roughnessTarget, 0.04f, 1.0f);
					ImGui::SliderFloat("Depth fade", &m_decalDemo.depthFadeStart, 0.1f, 0.95f);
					ImGui::SliderFloat("Spawn size min", &m_decalDemo.sizeMin, 0.1f, 1.0f);
					ImGui::SliderFloat("Spawn size max", &m_decalDemo.sizeMax, 0.1f, 2.0f);
					ImGui::Text("Splats: %u / %u%s", m_decalFrameCount, FP_MAX_DECALS,
						m_decalDemo.dropped ? "  (oldest dropped)" : "");
					if (ImGui::Button("Splat at crosshair (J)"))
						SpawnDecalAtCrosshair();
					ImGui::SameLine();
					if (ImGui::Button("Clear")) {
						ClearDecals();
					}
					ImGui::TextDisabled("J raycasts the crosshair (pick BVH builds once,"
						" %s)", m_decalDemo.pickBvhState == 1 ? "ready"
						: m_decalDemo.pickBvhState == 0 ? "first press builds it"
						: "unavailable for this scene");
				}
				if (ImGui::CollapsingHeader("EVSM shadow maps", ImGuiTreeNodeFlags_DefaultOpen)) {
					// EVSM (Exponential Variance Shadow Maps): RG32F 2xMSAA
					// moments + async separable blur, 1-frame-lag pipelined.
					// Off = PCF depth atlases. Needs RG32F 2xMSAA support.
				ImGui::Checkbox("EVSM enabled", &m_evsmEnabled);
				ImGui::SameLine();
				ImGui::TextDisabled(EvsmActive() ? "(active)" : "(PCF fallback)");
				int evsmPipe = (m_evsmPipeline == smvk::EvsmPipelineMode::SameFrame) ? 1 : 0;
				if (ImGui::Combo("Pipeline", &evsmPipe, "One-frame lag\0Same-frame\0")) {
					m_evsmPipeline = (evsmPipe == 1)
						? smvk::EvsmPipelineMode::SameFrame
						: smvk::EvsmPipelineMode::OneFrameLag;
				}
				ImGui::SliderFloat("Softness", &m_evsmSoftness, 0.0f, 3.0f);
				ImGui::SliderFloat("Spot light size (m)", &m_evsmSpotLightSize, 0.0f, 2.0f);
				ImGui::TextDisabled("RG32F 2xMSAA moments, 5-tap Gaussian H+V (async);"
					" soft = moment-blocker PCSS (solar angle / light size)");
				}
				if (ImGui::CollapsingHeader("Deferred shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
					// Deferred shadows (Square-Enix-style tiled/zBin pipeline).
					// A/B against the inline forward+ shadow path; the counters
					// are the retired-slice peaks (~2 frames old, see Stats).
					ImGui::Checkbox("Enable (H)", &m_dsEnabled);
					ImGui::SameLine();
					ImGui::TextDisabled(m_passes.deferredShadows ? "" : "(pass disabled)");
				}
				if (ImGui::CollapsingHeader("GfxFilter ï¿½ culling", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Enable filtered main view (G)", &m_useGfxFilter);
					ImGui::Checkbox("Enable filtered motion vectors", &m_useFilteredMotion);
					// Cull-flag toggles write straight into the renderer config
					// (takes effect next frame). FlipWinding default-off is
					// pinned by gfxfilter_tests (det>0 == back, exactly).
					bool b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Degenerate) != 0;
					if (ImGui::Checkbox("Cull degenerate", &b))
						m_gfxRenderer.config.cullFlags = b
						? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Degenerate
						: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Degenerate;
					b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Backface) != 0;
					if (ImGui::Checkbox("Cull backface", &b))
						m_gfxRenderer.config.cullFlags = b
						? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Backface
						: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Backface;
					b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Frustum) != 0;
					if (ImGui::Checkbox("Cull frustum", &b))
						m_gfxRenderer.config.cullFlags = b
						? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Frustum
						: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Frustum;
					b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_SmallPrim) != 0;
					if (ImGui::Checkbox("Cull small prims", &b))
						m_gfxRenderer.config.cullFlags = b
						? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_SmallPrim
						: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_SmallPrim;
					b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_FlipWinding) != 0;
					if (ImGui::Checkbox("Flip winding (debug)", &b))
						m_gfxRenderer.config.cullFlags = b
						? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_FlipWinding
						: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_FlipWinding;
					b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_HzbOcclusion) != 0;
					if (ImGui::Checkbox("HZB occlusion (T4)", &b))
						m_gfxRenderer.config.cullFlags = b
						? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_HzbOcclusion
						: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_HzbOcclusion;
					ImGui::Checkbox("HZB occluder depth (big crudest-LOD)", &m_hizOccluderDepth);
					ImGui::SliderFloat("Occluder min px", &m_hizOccluderMinPixels, 0.0f, 128.0f);
					ImGui::SliderFloat("Occluder min radius m", &m_hizOccluderMinRadius, 0.0f, 5.0f);
					b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_Contribution) != 0;
					if (ImGui::Checkbox("Contribution cull (T4)", &b))
						m_gfxRenderer.config.cullFlags = b
						? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_Contribution
						: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_Contribution;
					{
						float mc = m_gfxRenderer.MinContribution();
						if (ImGui::SliderFloat("Min contribution px^2", &mc, 0.0f, 16.0f))
							m_gfxRenderer.SetMinContribution(mc);
					}
				}
			if (ImGui::CollapsingHeader("LODs ï¿½ error-driven switches", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Checkbox("Enable LODs (L)", &m_lodEnabled);
				ImGui::SliderFloat("Pixel budget (px)", &m_lodPixelBudget, 0.25f, 8.0f, "%.2f");
				ImGui::Checkbox("Vertex-lit farthest LOD (shader LODs)", &m_lodShaderEnabled);
				ImGui::SameLine();
				ImGui::TextDisabled("(?)");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"The farthest LOD level keeps its coarse geometry but shades\n"
						"with the vertex-lit program: lighting is computed per vertex\n"
						"(sun + ambient, no shadows/point lights/specular/textures).\n"
						"Other levels stay full PBR. F5 shows the level in white.");
			}
				if (ImGui::CollapsingHeader("Texture streaming + color LUT", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("texture streaming (applies to next scene load)", &m_textureStreamingEnabled);
					ImGui::Separator();
					ImGui::Text("Color grading (TPP-style LUT)");
					{
						const char* current = m_lutFile.empty() ? "Off" : m_lutFile.c_str();
						if (ImGui::BeginCombo("Color LUT", current)) {
							if (ImGui::Selectable("Off", m_lutFile.empty())) {
								m_lutFile.clear();
								m_lutTexture = {};
								m_lutBindless = UINT32_MAX;
							}
							for (const std::string& f : m_lutFiles) {
								const bool selected = (m_lutFile == f);
								if (ImGui::Selectable(f.c_str(), selected))
									LoadLut(f);
							}
							ImGui::EndCombo();
						}
						if (!m_lutFile.empty())
							ImGui::SliderFloat("LUT intensity", &m_lutIntensity, 0.0f, 1.0f, "%.2f");
						ImGui::SameLine();
						if (ImGui::Button("Rescan"))
							ScanLutFiles();
						ImGui::TextDisabled(
							"F7: export neutral strip + screenshot to exports/.\n"
							"PS: grade the 1024x32 strip, save PNG into content/luts/.");
					}
				}
				ImGui::EndTabItem();
			}
			// ---------------------------------------------------------
			// Debug: views, overlays and one-shot diagnostics. Nothing here
			// changes the frame permanently; toggles are for bisection.
			// ---------------------------------------------------------
			if (ImGui::BeginTabItem("Debug")) {
				if (ImGui::CollapsingHeader("Debug views (bypass GPU cull, F1..F6, F9/F10)", ImGuiTreeNodeFlags_DefaultOpen)) {
					{
						const char* viewNames[] = {
							"F1 Lit", "F2 Unlit", "F3 Meshlets",
							"F4 Cascades", "F5 LODs", "F6 Vertex Colors",
							"F9 UV0 (base)", "F10 UV1 (lightmap)",
						};
						int view = static_cast<int>(m_debugView);
						if (ImGui::Combo("Debug view", &view, viewNames, 8))
							m_debugView = static_cast<DebugView>(view);
					}
					ImGui::TextDisabled("F6 vertex colors: layered materials show normalized weights as R/G/B/Y per layer 0..3;\n"
						"legacy materials show the raw vertex tint.");
					ImGui::TextDisabled("F9/F10 UV checkers: base TEXCOORD0 vs lightmap uv2. Magenta glow = huge uv\n"
						"screen-derivative (lightmap seam corners sweeping across atlas rects).\n"
						"UV1 stripes magenta when uv2 leaves [0,1] (atlas-space bug).");
					ImGui::TextDisabled("GI bisection: N isolates the ambient source (All -> Lightmaps -> Probes ->\n"
						"SkyIbl -> Off); R / F8 cycle the GI debug views (flat irradiance, source palette).\n"
						"Both work in every ambient mode.");
					const char* meshletModes[] = { "Random per meshlet", "Cone-cutoff heat" };
					ImGui::Combo("Meshlet color mode (F3)", &m_debugMeshletMode, meshletModes, 2);
					ImGui::Checkbox("Cone axes (line overlay)", &m_debugCones);
					ImGui::Checkbox("Wireframe (P, Line polygon mode)", &m_wireframe);
					ImGui::Checkbox("Hide transparent (debug, bisect blending)", &m_hideTransparent);
					if (!m_gfxRenderer.DebugProgramsValid())
						ImGui::TextDisabled("Debug programs missing ï¿½ views disabled");
					ImGui::TextDisabled("Colors: every registered cluster direct; culled ones stay visible here.\n"
						"Heat: red = degenerate cone (never culls), green = tight cone.");
					ImGui::TextDisabled("Bisect: disable flags top-down (small/backface/frustum/degen), then bypass.");
				}
				if (ImGui::CollapsingHeader("Culling freeze + dumps", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Freeze culling view (debug)", &m_freezeCullingView);
					ImGui::TextDisabled("Holds frustum/eye/HZB; render camera stays live.\n"
						"Move the camera to inspect what the frozen view culled.");
					if (ImGui::Button("Dump indirect audit (stderr, stalls)"))
						m_dumpIndirectAudit = true;
					ImGui::SameLine();
					if (ImGui::Button("Dump HZB (stderr, stalls)"))
						m_debugDumpHzb = true;
				}
				if (ImGui::CollapsingHeader("Motion + light overlays", ImGuiTreeNodeFlags_DefaultOpen)) {
					if (m_passes.motionVectors && m_viewportResources.motionVectors.Valid()) {
						ImGui::Checkbox("Show motion vectors (V)", &m_showMotionDebug);
						if (m_showMotionDebug) {
							ImGui::SliderFloat("Motion debug scale", &m_motionDebugScale, 1.0f, 128.0f);
							ImGui::TextDisabled("Direction -> hue, magnitude -> brightness");
						}
					} else {
						ImGui::TextDisabled("Motion vectors: target/shaders missing");
					}
					ImGui::Checkbox("Light points (overlay)", &m_fpDemo.showLightPoints);
					ImGui::Checkbox("Light numbers (overlay)", &m_fpDemo.showLightNumbers);
					if (m_fpDemo.showLightPoints || m_fpDemo.showLightNumbers) {
						ImGui::SliderFloat("Overlay point size (px)", &m_fpDemo.lightPointSize, 2.0f, 16.0f);
						ImGui::SliderInt("Overlay max labels", &m_fpDemo.maxLightLabels, 0, 1024);
					}
					ImGui::TextDisabled("Lights draw in the UI pass (points = light color, spots diamond);\n"
						"no depth test: occluded lights stay visible (debug intent)");
					ImGui::Checkbox("Probe points overlay", &m_giShowProbes);
					if (m_giShowProbes) {
						ImGui::SliderFloat("Overlay radius (m)", &m_giProbePointMaxDist,
							2.0f, 120.0f, "%.0f");
						ImGui::TextDisabled("colors: L0 red -> L4 cyan -> L7 pink (coarse->fine)");
						ImGui::Checkbox("Occlusion cull (pick BVH)", &m_giProbeOcclusionCull);
						if (m_giProbeOcclusionCull)
							ImGui::TextDisabled("hides probes behind geometry (one-time import hitch)");
					}
				if (m_giLoaded || m_lmLoaded || m_regLoaded) {
					const char* dbgNames[3] = { "Off", "Baked irradiance",
						"GI source" };
					int dbgIdx = m_giDebug;
					if (ImGui::Combo("GI debug view (R / F8)", &dbgIdx, dbgNames, 3))
						m_giDebug = std::clamp(dbgIdx, 0, 2);
					if (m_giDebug == 1) {
						ImGui::TextDisabled("flat irradiance of the active source "
							"(lightmap/grid/probes); magenta = none\n"
							"works in EVERY ambient mode (N): isolate the source "
							"with N, then read it here");
					} else if (m_giDebug == 2) {
						ImGui::TextDisabled("source palette: red = lightmap, "
							"green = regular grid, blue = probe volume, "
							"magenta = none (static/dynamic separation)");
					}
				}
				}
#if defined(DS_DEBUG_VIZ)
				// ---- Deferred-shadow debug visualizers (paper 5.3/5.4).
				// Debug builds only (DS_DEBUG_VIZ): Y cycles the combo. ----
				if (ImGui::CollapsingHeader("DS visualizers (Y cycles)", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Text("DS visualizers (Y cycles)");
					{
						const char* vizNames[] = {
							"Off",
							"Light count approx (ph2)",
							"Light count accurate (ph3)",
							"Light count final (ph4)",
							"Count diff approx-accurate",
							"Cull step (light)",
							"Light list (light)",
							"Light visibility (light)",
						};
						int viz = static_cast<int>(m_dsDebugMode);
						if (ImGui::Combo("Visualizer", &viz, vizNames, 8))
							m_dsDebugMode = static_cast<DsDebugViz>(viz);
						const bool vizOn = m_dsDebugMode != DsDebugViz::Off;
						const bool lightScoped = vizOn && m_dsDebugMode >= DsDebugViz::CullStep;
						if (vizOn && !DsActive())
							ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
								"Pipeline off: enable deferred shadows (H) first");
						if (vizOn)
							ImGui::Checkbox("Overlay over lit", &m_dsDebugOverlay);
						if (m_dsDebugMode >= DsDebugViz::CountApprox
							&& m_dsDebugMode <= DsDebugViz::CountDiff) {
							ImGui::SliderInt("Heat ceiling (lights)",
								&m_dsDebugCountCeil, 1, (int)smvk::kDsMaxLightsPerTile);
							ImGui::TextDisabled("Heat: 0=black, blue/cyan/green/yellow -> red at ceiling.\n"
								"8px Shadow Tiles, faint grid = 64px TileGroups.");
						}
						if (lightScoped) {
							const int maxLight = std::max(0, (int)m_dsLightCount - 1);
							ImGui::SliderInt("Debug light (sorted idx)", &m_dsDebugLight,
								0, maxLight);
							if (m_dsDebugLightValid) {
								ImGui::Text("Light %d: %s  range %.1f m", m_dsDebugLight,
									m_dsDebugLightType == (int)smvk::kForwardPlusSpot ? "spot" : "point",
									m_dsDebugLightRange);
							}
							if (m_dsDebugMode == DsDebugViz::CullStep)
								ImGui::TextDisabled("Last step passed: RED ph1 mask / ORANGE ph2 zBin-froxel\n"
									"(on lit pixels = ERRONEOUS CULL), BLUE ph4 all-shadow drop,\n"
									"GREEN full pass (brightness = visibility).");
							else if (m_dsDebugMode == DsDebugViz::LightList)
								ImGui::TextDisabled("RED in no list / ORANGE ph1 mask only (erroneous),\n"
									"YELLOW early only (shadow-dropped), GREEN final list\n"
									"(brightness = visibility, dim = sibling-pixel coverage).");
							else
								ImGui::TextDisabled("WHITE lit -> BLACK shadowed (precomputed byte);\n"
									"dark blue = not in the final list.");
						}
					}
				}
#endif
				ImGui::EndTabItem();
			}
			// ---------------------------------------------------------
			// Stats: read-only readback. Retired-slice counters lag ~2
			// frames (same lag as the exposure meter).
			// ---------------------------------------------------------
			if (ImGui::BeginTabItem("Stats")) {
				if (ImGui::CollapsingHeader("Camera + swapchain", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Text("Camera pos: %.2f %.2f %.2f", m_cameraPositioner.cameraPosition_.x, m_cameraPositioner.cameraPosition_.y, m_cameraPositioner.cameraPosition_.z);
					{
						const glm::vec3 euler = m_cameraPositioner.getEulerDegrees(); // x=pitch, y=yaw, z=roll
						const glm::vec3 fwd = m_cameraPositioner.getForward();
						const glm::quat q = m_cameraPositioner.getOrientation();
						ImGui::Text("Camera rot yaw: %.1f pitch: %.1f roll: %.1f (deg)", euler.y, euler.x, euler.z);
						ImGui::Text("Camera fwd: %.3f %.3f %.3f", fwd.x, fwd.y, fwd.z);
						ImGui::TextDisabled("Camera quat: %.3f %.3f %.3f %.3f", q.x, q.y, q.z, q.w);
					}
					// Controller status: name + live stick state, or "none".
					if (m_input.hasGamepad()) {
						ImGui::Text("Gamepad: %s", m_input.gamepadName(0).c_str());
						ImGui::TextDisabled("LX %+.2f LY %+.2f  RX %+.2f RY %+.2f",
							m_input.gamepadAxis(0, SDL_GAMEPAD_AXIS_LEFTX),
							m_input.gamepadAxis(0, SDL_GAMEPAD_AXIS_LEFTY),
							m_input.gamepadAxis(0, SDL_GAMEPAD_AXIS_RIGHTX),
							m_input.gamepadAxis(0, SDL_GAMEPAD_AXIS_RIGHTY));
						ImGui::TextDisabled("Left stick move, right stick look, LT/RT down/up, RB sprint, Start UI");
					} else {
						ImGui::TextDisabled("Gamepad: none connected");
					}
					{
						float dzMove = m_input.stickDeadzone();
						float dzLook = m_input.lookDeadzone();
						float dzTrig = m_input.triggerDeadzone();
						if (ImGui::SliderFloat("Move deadzone", &dzMove, 0.0f, 0.5f))
							m_input.setStickDeadzone(dzMove);
						if (ImGui::SliderFloat("Look deadzone", &dzLook, 0.0f, 0.5f))
							m_input.setLookDeadzone(dzLook);
						if (ImGui::SliderFloat("Trigger deadzone", &dzTrig, 0.0f, 0.5f))
							m_input.setTriggerDeadzone(dzTrig);
					}
					ImGui::Text("Swapchain %ux%u  framesInFlight=%u",
						fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height, fb.swapchain->framesInFlight());
					ImGui::Text("Scene: %ux%u (%.0f%%) -> swap %ux%u",
						renderExtent.width, renderExtent.height, m_renderScale * 100.0f,
						swapExtent.width, swapExtent.height);
					ImGui::Text("UI pass: %ux%u (%.0f%%)",
						uiExtent.width, uiExtent.height, m_uiScale * 100.0f);
					const smvk::PresentMode active = fb.swapchain->GetActivePresentMode();
					if (active != m_presentMode)
						ImGui::TextDisabled("Active: %s (requested unavailable?)", smvk::ToString(active));
					else
						ImGui::TextDisabled("Active: %s", smvk::ToString(active));
				}
				if (ImGui::CollapsingHeader("GPU memory + streaming", ImGuiTreeNodeFlags_DefaultOpen)) {
					// ---- GPU memory (VMA budgets, VK_EXT_memory_budget) ----
					// Throttled to 1 Hz (see RefreshGpuMemoryStats); the bar
					// shows total usage vs budget. Query cost is shown so a
					// slow driver budget query can't hide behind this UI.
					RefreshGpuMemoryStats(false);
					auto fmtMiB = [](uint64_t bytes, char* out, size_t n) {
						const double mib = (double)bytes / (1024.0 * 1024.0);
						if (mib >= 1024.0)
							snprintf(out, n, "%.2f GiB", mib / 1024.0);
						else
							snprintf(out, n, "%.0f MiB", mib);
					};
					if (!m_gpuMemHeaps.empty()) {
						uint64_t totalUsed = 0, totalBudget = 0;
						for (const auto& h : m_gpuMemHeaps) {
							totalUsed += h.usageBytes;
							totalBudget += h.budgetBytes;
						}
						const float frac = totalBudget > 0
							? (float)((double)totalUsed / (double)totalBudget) : 0.0f;
						char usedS[32], budgetS[32];
						fmtMiB(totalUsed, usedS, sizeof(usedS));
						fmtMiB(totalBudget, budgetS, sizeof(budgetS));
						char overlay[64];
						snprintf(overlay, sizeof(overlay), "%s / %s (%.1f%%)",
							usedS, budgetS, frac * 100.0f);
						// Bar color ramps green -> yellow -> red with pressure.
						const ImVec4 barCol = frac < 0.6f
							? ImVec4(0.20f, 0.70f, 0.30f, 1.0f)
							: (frac < 0.85f ? ImVec4(0.85f, 0.75f, 0.20f, 1.0f)
								: ImVec4(0.85f, 0.25f, 0.20f, 1.0f));
						ImGui::Text("GPU memory %s", overlay);
						ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
						ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
						ImGui::PopStyleColor();
						// Per-heap breakdown (collapsed by default: VRAM heap only
						// matters on most discrete GPUs; integrated shows host).
						if (ImGui::SmallButton(m_gpuMemShowAllHeaps ? "heaps: all [-]" : "heaps: details [+]"))
							m_gpuMemShowAllHeaps = !m_gpuMemShowAllHeaps;
						ImGui::SameLine();
						if (ImGui::SmallButton("log to console"))
							LogGpuMemoryToConsole();
						ImGui::SameLine();
						ImGui::Checkbox("auto-log 10s", &m_gpuMemConsoleAutoLog);
						ImGui::TextDisabled("query %.0f us @1Hz%s", m_gpuMemQueryUs,
							m_gpuMemConsoleAutoLog ? "" : " (console auto-log off)");
						const size_t showCount = m_gpuMemShowAllHeaps ? m_gpuMemHeaps.size() : 1;
						for (size_t i = 0; i < m_gpuMemHeaps.size() && i < showCount; ++i) {
							// Default view: the first DEVICE_LOCAL heap (VRAM).
							size_t idx = i;
							if (!m_gpuMemShowAllHeaps) {
								idx = 0;
								for (size_t k = 0; k < m_gpuMemHeaps.size(); ++k) {
									if (m_gpuMemHeaps[k].deviceLocal) { idx = k; break; }
								}
							}
							const auto& h = m_gpuMemHeaps[idx];
							const float hf = h.budgetBytes > 0
								? (float)((double)h.usageBytes / (double)h.budgetBytes) : 0.0f;
							char hu[32], hb[32], hs[32], hov[80];
							fmtMiB(h.usageBytes, hu, sizeof(hu));
							fmtMiB(h.budgetBytes, hb, sizeof(hb));
							fmtMiB(h.heapSizeBytes, hs, sizeof(hs));
							snprintf(hov, sizeof(hov), "heap%zu %s %s/%s %.1f%% (heap %s)",
								idx, h.deviceLocal ? "VRAM" : "host",
								hu, hb, hf * 100.0f, hs);
							ImGui::ProgressBar(hf, ImVec2(-1.0f, 0.0f), hov);
							if (!m_gpuMemShowAllHeaps)
								break;
						}
					} else {
						ImGui::TextDisabled("GPU memory: no heap data");
					}
					// Texture streaming: resident tail vs full-residency cost.
					if (m_textureStreamer) {
						const smvk::TextureStreamingStats tstats = m_textureStreamer->GetStats();
						const double resMiB = (double)tstats.residentBytes / 1048576.0;
						const double fullMiB = (double)tstats.fullBytes / 1048576.0;
						const double savedPct = fullMiB > 0.0
							? 100.0 * (1.0 - resMiB / fullMiB) : 0.0;
						ImGui::Text("Streaming: %u tex (%u full)  %.0f/%.0f MiB resident (%.0f%% saved)  +%u -%u",
							tstats.textureCount, tstats.fullResidentCount,
							resMiB, fullMiB, savedPct,
							tstats.upgradesLastUpdate, tstats.downgradesLastUpdate);
						if (m_textureFeedback) {
							ImGui::Text("GPU feedback: %u slots  %u reported last consume",
								m_textureFeedback->GetSlotCount(),
								m_textureFeedback->GetLastReported());
						}
					}
					else {
						ImGui::TextDisabled("Streaming: off (full-resident)");
					}
				}
				if (ImGui::CollapsingHeader("Shadows ï¿½ CSM + spots + deferred", ImGuiTreeNodeFlags_DefaultOpen)) {
					auto splits = smvk::ComputeCascadeSplits(default_near_clip, m_scene.farClip, CASCADE_LAMBDA, NUM_CASCADES);
					const auto& atlas = m_viewportResources.directionalShadows;
					ImGui::Text("CSM: %u cascades  tile %ux%u  atlas %ux%u",
						NUM_CASCADES, CASCADE_RES, CASCADE_RES,
						atlas.atlasExtent.width, atlas.atlasExtent.height);
					for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
						ImGui::Text(" cascade %u: [%.2f .. %.2f] splitEnd %.2f", i, splits[i], splits[i + 1], splits[i + 1]);
					}
					const glm::vec3 toSunDbg = m_env.SunDirectionToSun();
					ImGui::Text(" sun toSun: %.2f %.2f %.2f  int %.0f lux",
						toSunDbg.x, toSunDbg.y, toSunDbg.z, m_env.sunIntensity);
					ImGui::Text("RenderViews: %u allocated (main=%u, CSM @%u, spots @%u)",
						m_renderViews.Count(), m_renderViews.MainViewIndex(),
						cascadeViewBase, spotViewBase);
					ImGui::Separator();
					ImGui::Text("Spot shadows: %u / %u tiles  atlas %ux%u",
						m_spotShadowCount, smvk::kMaxSpotShadows,
						m_viewportResources.spotShadows.atlasExtent.width,
						m_viewportResources.spotShadows.atlasExtent.height);
					ImGui::TextDisabled("Select: intensity*cone*coverage/dist^2, NDC-box + distance culled");
					if (!m_viewportResources.spotShadows.Valid())
						ImGui::TextDisabled("Spot atlas: MISSING (spots run unshadowed)");
					ImGui::Separator();
					// Deferred shadows (tiled/zBin): counters are retired-slice
					// peaks (~2 frames old).
					const smvk::DeferredShadows& ds = m_viewportResources.deferredShadows;
					if (ds.Valid()) {
						ImGui::Text("Shadow tiles: %ux%u (8px)  TileGroups: %ux%u (64px)",
							ds.tileCountX, ds.tileCountY, ds.groupCountX, ds.groupCountY);
						ImGui::Text("Light list: %u / %u uints (%.1f MB used)",
							m_dsStats.listSlots, smvk::kDsMaxListUInts,
							float(m_dsStats.listSlots) * 4.0f / (1024.0f * 1024.0f));
						ImGui::Text("Visibility: %u / %u slots x 64B (%.1f MB used)",
							m_dsStats.visSlots, smvk::kDsMaxVisSlots,
							float(m_dsStats.visSlots) * 64.0f / (1024.0f * 1024.0f));
						if (m_dsStats.listSlots >= smvk::kDsMaxListUInts)
							ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
								"Light list SATURATED: tiles clamp (lights drop to fully lit)");
						if (m_dsStats.visSlots >= smvk::kDsMaxVisSlots)
							ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
								"Visibility SATURATED: partial shadows degrade to fully lit");
					} else {
						ImGui::TextDisabled("Buffers/shaders missing (extent cap?); pipeline off");
					}
					ImGui::TextDisabled("CPU phase 1: zBins (1024) + 64px tile masks; GPU phases:\n"
						"2 approx froxel cull, 3 per-pixel narrow, 4 PCF visibility\n"
						"(%s path active)", DsActive() ? "deferred" : "inline forward+");
				}
				if (ImGui::CollapsingHeader("Bloom + exposure meter", ImGuiTreeNodeFlags_DefaultOpen)) {
					const auto& bloom = m_viewportResources.bloom;
					if (bloom.Valid() && m_bloomEnabled) {
						ImGui::Text("Bloom: %u mips  base %ux%u  thr %.2f knee %.2f int %.2f",
							bloom.MipCount(),
							bloom.MipExtent(0).width, bloom.MipExtent(0).height,
							bloom.desc.threshold, bloom.desc.knee, bloom.desc.intensity);
						for (uint32_t i = 0; i < bloom.MipCount(); ++i)
							ImGui::Text(" mip %u: %ux%u", i, bloom.MipExtent(i).width, bloom.MipExtent(i).height);
					} else {
						ImGui::TextDisabled("Bloom: OFF");
					}
					// Reports what this frame actually applies (debug views
					// run at the fixed reference â€” see FrameExposure).
					const float effExposure = FrameExposure();
					ImGui::Text("Exposure: %.5f (target %.5f)  metered %u px",
						effExposure, m_exposureTarget, m_exposureSamples);
					float histPeak = 1.0f;
					for (float v : m_exposureBinsUi)
						histPeak = std::max(histPeak, v);
					ImGui::PlotHistogram("##expHist", m_exposureBinsUi,
						(int)smvk::kExposureBinCount, 0, nullptr, 0.0f, histPeak, ImVec2(0, 48));
					if (!m_viewportResources.exposure.Valid())
						ImGui::TextDisabled("Meter missing ï¿½ manual exposure only");
					else
						ImGui::TextDisabled("Bloom threshold scales with exposure (ref %.4f); percentile %.0f%%, key %.2f",
							TONEMAP_EXPOSURE, m_viewportResources.exposure.desc.percentile * 100.0f,
							m_viewportResources.exposure.desc.keyValue);
				}
				if (ImGui::CollapsingHeader("GI volume", ImGuiTreeNodeFlags_DefaultOpen)) {
					if (m_giLoaded) {
						ImGui::Text("Probes: %u  levels: %u  GPU: %zu KiB",
							(unsigned)m_giVolume.probes.size(), m_giVolume.levelCount,
							m_giVolume.GpuBytes() / 1024);
						for (uint32_t lvl = 0; lvl < m_giVolume.levelCount
							&& lvl < m_giVolume.levelStats.size(); ++lvl) {
							const auto& s = m_giVolume.levelStats[lvl];
							ImGui::BulletText("lvl %u: %u probes  %u cells (max %u/cell)",
								lvl, s.probeCount, s.cellCount, s.maxProbesPerCell);
						}
						ImGui::TextDisabled("9 SG lobes @ sharpness 4, 96 B/probe, 6-axis visibility");
					} else {
						ImGui::TextDisabled("No baked volume: content/gi/%s.sggi",
							m_scene.id ? m_scene.id : "?");
					}
				}
				if (ImGui::CollapsingHeader("AA + motion + AO state", ImGuiTreeNodeFlags_DefaultOpen)) {
					if (m_aaMode == AaMode::Msaa2x) {
						if (!m_context->SupportsMSAA2x())
							ImGui::TextDisabled("MSAA 2x unsupported - running 1x");
						else
							ImGui::TextDisabled(m_viewportResources.msaa.Valid()
								? "MSAA 2x -> HDR resolve" : "MSAA target missing - running 1x");
					} else if (m_aaMode == AaMode::Smaa1x || m_aaMode == AaMode::SmaaT2x) {
						if (!m_viewportResources.smaa.Valid())
							ImGui::TextDisabled("SMAA unavailable (shaders/LUTs missing - rebuild?)");
						else if (!(m_passes.motionVectors && m_viewportResources.motionVectors.Valid()))
							ImGui::TextDisabled("SMAA needs the motion-vector pass - disabled");
						else if (m_aaMode == AaMode::SmaaT2x)
							ImGui::TextDisabled("SMAA T2x: quarter-pixel jitter + temporal resolve%s [img=%u par=%u]",
								m_smaaFrameParams.historyValid ? "" : " (history warming up)",
								fb.imageIndex, m_smaaFrameParams.parity);
						else
							ImGui::TextDisabled("SMAA 1x: edges -> weights -> neighborhood blend");
				} else {
					ImGui::TextDisabled("AA: off");
				}
				{
					// DLSS line: availability, active state and the reset
					// history flag (one-record visibility for warmup).
					const char* dlssModeName[] = { "off", "performance", "balanced",
						"quality", "ultra performance", "ultra quality", "DLAA" };
					const int modeIdx = static_cast<int>(m_dlssMode);
					if (m_dlssFrameActive)
						ImGui::TextDisabled("DLSS %s: HDR %ux%u -> %ux%u%s",
							dlssModeName[std::clamp(modeIdx, 0, 6)],
							renderExtent.width, renderExtent.height,
							swapExtent.width, swapExtent.height,
							m_dlssReset ? " (history reset)" : "");
					else if (DlssRequested())
						ImGui::TextDisabled("DLSS %s: INACTIVE (%s%s)",
							dlssModeName[std::clamp(modeIdx, 0, 6)],
							smvk::DlssAvailable() ? "unsupported adapter" : "runtime missing",
							m_passes.motionVectors ? "" : " / no motion vectors");
					else
						ImGui::TextDisabled("DLSS: off (Streamline %s)",
							smvk::DlssAvailable() ? "loaded" : "not loaded");
				}
				if (!m_viewportResources.motionBlur.Valid())
						ImGui::TextDisabled("Motion blur target/shaders missing ï¿½ sharp HDR");
					else
						ImGui::TextDisabled("Motion blur: gather along motion vectors, pre-tonemap HDR");
					const auto& ssao = m_viewportResources.ssao;
					const auto& gtao = m_viewportResources.gtao;
					{
						static const char* const kAoNames[3] =
							{ "No AO", "SSAO", "GTAO" };
						ImGui::TextDisabled("AO mode: %s",
							kAoNames[static_cast<int>(m_aoMode)]);
					}
					if (m_aoMode == AoMode::Ssao) {
						if (!ssao.Valid())
							ImGui::TextDisabled("SSAO target/shaders missing â€” no AO");
						else
							ImGui::Text("SSAO: half %ux%u (full %ux%u)",
								ssao.halfExtent.width, ssao.halfExtent.height,
								ssao.fullExtent.width, ssao.fullExtent.height);
					} else if (m_aoMode == AoMode::Gtao) {
						if (!gtao.Valid())
							ImGui::TextDisabled("GTAO target/shaders missing â€” no AO");
						else
							ImGui::Text("GTAO: half %ux%u (full %ux%u) %ux%u",
								gtao.halfExtent.width, gtao.halfExtent.height,
								gtao.fullExtent.width, gtao.fullExtent.height,
								(uint32_t)std::clamp(m_gtaoSlices, 1, 8),
								(uint32_t)std::clamp(m_gtaoSteps, 1, 8));
					}
					if (!m_viewportResources.dof.Valid())
						ImGui::TextDisabled("DOF targets/shaders missing - sharp HDR");
					else
						ImGui::TextDisabled("DOF: %s focus %.1fm range %.1fm (%s)",
							m_dofEnabled ? "ON" : "off",
							m_dofFocusDistance, m_dofFocusRange,
							m_dofHqBokeh ? "HQ bokeh" : "low");
					if (!m_viewportResources.ssr.Valid())
						ImGui::TextDisabled("SSR target/shaders missing - no reflections");
					else
						ImGui::TextDisabled("SSR: %s %u steps intensity %.2f",
							m_ssrEnabled ? "ON" : "off",
							(uint32_t)std::clamp(m_ssrSteps, 1, 128), m_ssrIntensity);
					if (!m_viewportResources.volumetric.Valid())
						ImGui::TextDisabled("Volumetric target/shaders missing - no fog");
					else
						ImGui::TextDisabled("Volumetric: %s %s %u steps dens %.4f int %.2f",
							m_volumetricEnabled ? "ON" : "off",
							smvk::VolumetricQualityName(m_volumetricQuality),
							(uint32_t)std::clamp(m_volumetricSteps, 4, 128),
							m_volumetricDensity, m_volumetricIntensity);
				}
				if (ImGui::CollapsingHeader("Forward+ tiles + GfxFilter", ImGuiTreeNodeFlags_DefaultOpen)) {
					const auto& fwd = m_viewportResources.forwardPlus;
					ImGui::Text("Forward+: %ux%u tiles (%u)  tile %u  max/tile %u",
						fwd.tileCountX, fwd.tileCountY, fwd.tileCount,
						FP_TILE_SIZE, FP_MAX_PER_TILE);
					const int wantLights = m_fpDemo.enabled
						? (m_fpDemo.gridX * m_fpDemo.gridY * m_fpDemo.gridZ + m_fpDemo.spotCount) : 0;
				ImGui::Text("Lights: %d / %u%s", wantLights, FP_MAX_LIGHTS,
					wantLights > (int)FP_MAX_LIGHTS ? "  (CLAMPED!)" : "");
				ImGui::TextDisabled("Tiles cap at %u lights: overflow pops (spots win slots, written first)",
					FP_MAX_PER_TILE);
				ImGui::Text("Decals: %u / %u (cap %u/tile)%s", m_decalFrameCount,
					FP_MAX_DECALS, FP_MAX_DECALS_PER_TILE,
					DecalsActive() ? "" : "  (off)");
				ImGui::TextDisabled("Decal lists refresh in the same cull dispatch as lights (also under deferred shadows)");
					if (fwd.depthBindlessIndex == UINT32_MAX)
						ImGui::TextDisabled("Depth bindless: MISSING (cull falls back to empty)");
					ImGui::TextDisabled("Cookies: soft disc / blinds / starburst per spot; patterns modulate light color");
					ImGui::Separator();
					const smvk::gfx::Stats gs = m_gfxRenderer.LastStats();
					ImGui::Text("view: %u draws %u cmds %u tris | clusters %u/%u culled",
						gs.draws, gs.commands, gs.trianglesProcessed,
						gs.clustersCulled, gs.clustersProcessed);
					ImGui::Text("materials: %u ranges  batches: %u",
						(unsigned)m_gfxRenderer.LastMaterialRangeCount(),
						(unsigned)m_gfxRenderer.LastBatchCount());
					// HZB occlusion status: the CPU "clusters culled" line above
					// never moves for GPU occlusion kills (those stay batches
					// with indexCount 0), so surface the HZB inputs here. Any
					// "OFF/empty/INVALID" below means the filter's occlusion
					// test is silently disabled ï¿½ use Dump HZB + Dump indirect
					// audit (stderr, Debug tab) to confirm what the GPU saw.
					{
						const bool occOn = (m_gfxRenderer.config.cullFlags
							& smvk::gfx::CullFlagBit_HzbOcclusion) != 0;
						uint32_t hw = 0, hh = 0, hm = 0;
						if (gfxSlot != smvk::RenderViewSet::kInvalidIndex
							&& gfxSlot < m_gfxRenderer.State().temporals.size()) {
							const smvk::gfx::HizResources& hz =
								smvk::gfx::Hiz(m_gfxRenderer.State(), gfxSlot);
							hw = hz.width; hh = hz.height; hm = hz.mipCount;
						}
						const bool depthOk = m_hizOccluderDepth
							? m_viewportResources.hizDepth.bindlessIndex != UINT32_MAX
							: m_viewportResources.forwardPlus.depthBindlessIndex != UINT32_MAX;
						ImGui::Text("hzb: %ux%u/%u mips depthIdx=%s occ=%s cull=%s src=%s",
							hw, hh, hm, depthOk ? "ok" : "INVALID", occOn ? "on" : "OFF",
							cullingFrozen ? "FROZEN" : "live",
							m_hizOccluderDepth ? "occluder" : "full");
						if (!occOn)
							ImGui::TextDisabled("HZB flag OFF: no occlusion culls possible.");
						else if (hm == 0)
							ImGui::TextDisabled("HZB empty: occlusion test disabled (extent 0?).");
						else if (!depthOk)
							ImGui::TextDisabled("Depth bindless INVALID: HZB reads garbage.");
					}
				}
			if (ImGui::CollapsingHeader("LODs", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Text("Cooked: %u meshes / %u siblings | draws: %llu (%llu switched, %llu vertex-lit)",
					m_lodChainMeshes, m_lodChainLods,
					(unsigned long long)m_lodDraws, (unsigned long long)m_lodSwitched,
					(unsigned long long)m_lodShaderDraws);
				ImGui::TextDisabled("Transition = projected simplifier error (world err * H / "
					"(2*tan(fov/2)*dist) <= budget). LOD0 stays in the GPU filter; LOD1+ draws direct.");
				ImGui::TextDisabled("Shader LODs: farthest level shades vertex-lit (F5: white).");
				if (!m_lodEnabled)
					ImGui::TextDisabled("LODs OFF ï¿½ always LOD0.");
				else if (m_lodChainMeshes == 0)
					ImGui::TextDisabled("No cooked LOD chain in this scene ï¿½ cook with --lods on.");
				else if (!ShaderLodActive())
					ImGui::TextDisabled("Shader LODs OFF (toggle or missing program) ï¿½ every level full-lit.");
			}
				ImGui::Separator();
				ImGui::TextDisabled("[I] %s  [ESC] quit", m_uiMode ? "UI (NORMAL) -> press I for FPS" : "FPS (DISABLED) -> press I for UI");
				if (m_input.hasGamepad())
					ImGui::TextDisabled("[Gamepad] %s connected (Start = UI)", m_input.gamepadName(0).c_str());
				else
					ImGui::TextDisabled("[Gamepad] none connected");
				ImGui::TextDisabled("Keys: M blur  O AO (No/SSAO/GTAO)  B bloom  X exposure  V vectors");
				ImGui::TextDisabled("      L LODs  F lights  G filter  C cookies  K sky");
				ImGui::TextDisabled("      J blood splat at crosshair  VULKANTOY_DECALS=0/1");
				ImGui::TextDisabled("      N ambient GI/sky/off  F8 GI irradiance view");
				ImGui::TextDisabled("      F1 lit  F2 unlit  F3 meshlets  F4 cascades  F5 LOD colors  F6 vertex colors");
#if defined(DS_DEBUG_VIZ)
				ImGui::TextDisabled("      Y DS visualizer (%s)",
					m_dsDebugMode == DsDebugViz::Off ? "off" : "on");
#endif
				ImGui::TextDisabled("      T transparency  Z/1-4 AA  U dock (%s)  P wireframe", UiDockName());
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::End();

			// Forward+ light debug overlay: project the CPU light copy
			// (m_debugLights, filled in UpdateForwardPlusLights) through the
			// UNJITTERED viewProj into the ImGui foreground draw list. It
			// rides the UI pass and composites over the tonemapped scene.
			// The jittered frame matrix would buzz every marker +/-0.25 px
			// per frame (the UI layer has no temporal resolve). No depth
			// test: occluded / behind-camera lights stay visible (debug).
			if (m_fpDemo.enabled && (m_fpDemo.showLightPoints || m_fpDemo.showLightNumbers)
				&& !m_debugLights.empty()) {
				ImDrawList* dl = ImGui::GetForegroundDrawList();
				const ImVec2 disp = ImGui::GetIO().DisplaySize;
				const float r = std::max(m_fpDemo.lightPointSize, 1.0f);
				const auto toSrgb8 = [](float c) {
					c = std::min(std::max(c, 0.0f), 1.0f);
					return (int)std::lround(std::pow(c, 1.0f / 2.2f) * 255.0f);
				};
				uint32_t labels = 0;
				const uint32_t maxLabels =
					(uint32_t)std::max(0, m_fpDemo.maxLightLabels);
				for (uint32_t i = 0; i < m_debugLights.size(); ++i) {
					const DebugLight& l = m_debugLights[i];
					const glm::vec4 clip =
						m_unjitteredViewProj * glm::vec4(l.position, 1.0f);
					if (clip.w <= 1e-4f)
						continue; // behind the camera / degenerate
					const glm::vec3 ndc = glm::vec3(clip) / clip.w;
					if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f)
						continue; // off-screen
					// viewProj is post-Y-flip (Vulkan framebuffer y-down), so
					// NDC maps straight onto ImGui's y-down display space.
					const ImVec2 pt(
						(ndc.x * 0.5f + 0.5f) * disp.x,
						(ndc.y * 0.5f + 0.5f) * disp.y);
					const ImU32 col = IM_COL32(toSrgb8(l.color.r), toSrgb8(l.color.g),
						toSrgb8(l.color.b), 220);
					if (l.type == (int)smvk::kForwardPlusSpot) {
						// Spots: diamond marker so cones read differently.
						dl->AddQuadFilled(
							ImVec2(pt.x, pt.y - r), ImVec2(pt.x + r, pt.y),
							ImVec2(pt.x, pt.y + r), ImVec2(pt.x - r, pt.y), col);
					} else {
						dl->AddCircleFilled(pt, r, col);
					}
				if (m_fpDemo.showLightNumbers && labels < maxLabels) {
					char label[16];
					snprintf(label, sizeof(label), "%u", i);
					dl->AddText(ImVec2(pt.x + r + 2.0f + 1.0f, pt.y - r + 1.0f),
						IM_COL32(0, 0, 0, 200), label);
					dl->AddText(ImVec2(pt.x + r + 2.0f, pt.y - r),
						IM_COL32_WHITE, label);
					++labels;
				}
			}
		}

		// SG-GI probe-point overlay (like the forward+ light points):
		// draws the probe lattice colored by octree level through the
		// UNJITTERED viewProj. Distance-filtered + count-capped so huge
		// volumes stay ImGui-cheap. Without occlusion culling there is no
		// depth test: occluded probes stay visible (debug).
		if (m_giLoaded && m_giShowProbes && !m_giVolume.probes.empty()) {
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			const ImVec2 disp = ImGui::GetIO().DisplaySize;
			const glm::vec3 camPos = m_cameraPositioner.cameraPosition_;
			const float maxDist = std::max(m_giProbePointMaxDist, 1.0f);
			const float maxDist2 = maxDist * maxDist;
			// Level colors: coarse red -> fine pink; point size shrinks
			// with level so dense fine probes don't swamp the view.
			static const ImU32 kLevelColors[8] = {
				IM_COL32(255, 64, 64, 210), IM_COL32(255, 160, 32, 210),
				IM_COL32(230, 230, 60, 210), IM_COL32(70, 230, 90, 210),
				IM_COL32(60, 200, 255, 210), IM_COL32(110, 130, 255, 210),
				IM_COL32(200, 100, 255, 210), IM_COL32(255, 110, 200, 210),
			};
			uint32_t drawn = 0;
			uint32_t culled = 0;
			constexpr uint32_t kMaxDrawn = 6000;
			for (uint32_t pi = 0; pi < m_giVolume.probes.size(); ++pi) {
				const smvk::gi::SgProbe& p = m_giVolume.probes[pi];
				if (drawn >= kMaxDrawn)
					break;
				if (!p.valid)
					continue;
				const glm::vec3 d = p.position - camPos;
				if (glm::dot(d, d) > maxDist2)
					continue;
				if (m_giProbeOcclusionCull && !GiProbeVisibleFromCamera(pi, camPos)) {
					++culled;
					continue;
				}
				const glm::vec4 clip = m_unjitteredViewProj * glm::vec4(p.position, 1.0f);
				if (clip.w <= 1e-4f)
					continue;
				const glm::vec3 ndc = glm::vec3(clip) / clip.w;
				if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f)
					continue;
				const ImVec2 pt(
					(ndc.x * 0.5f + 0.5f) * disp.x,
					(ndc.y * 0.5f + 0.5f) * disp.y);
				const uint32_t lvl = std::min(p.level, 7u);
				const float r = std::max(4.0f - 0.7f * float(lvl), 1.5f);
				dl->AddCircleFilled(pt, r, kLevelColors[lvl]);
				++drawn;
			}
			// Legend: level colors + drawn count, pinned top-left.
			float lx = 8.0f;
			const float ly = 8.0f;
			char cap[128];
			if (m_giProbeOcclusionCull)
				snprintf(cap, sizeof(cap), "GI probes: %u/%u drawn (<= %.0fm), %u occluded",
					drawn, (unsigned)m_giVolume.probes.size(), maxDist, culled);
			else
				snprintf(cap, sizeof(cap), "GI probes: %u/%u drawn (<= %.0fm)",
					drawn, (unsigned)m_giVolume.probes.size(), maxDist);
			dl->AddText(ImVec2(lx + 1.0f, ly + 1.0f), IM_COL32(0, 0, 0, 200), cap);
			dl->AddText(ImVec2(lx, ly), IM_COL32_WHITE, cap);
			for (uint32_t lvl = 0; lvl < std::min(m_giVolume.levelCount, 8u); ++lvl) {
				dl->AddCircleFilled(ImVec2(lx + 4.0f, ly + 22.0f), 3.5f, kLevelColors[lvl]);
				char lab[8];
				snprintf(lab, sizeof(lab), "L%u", lvl);
				dl->AddText(ImVec2(lx + 10.0f + 1.0f, ly + 16.0f + 1.0f),
					IM_COL32(0, 0, 0, 200), lab);
				dl->AddText(ImVec2(lx + 10.0f, ly + 16.0f), IM_COL32_WHITE, lab);
				lx += 34.0f;
			}
			// Headless heartbeat: proves the overlay (and the occlusion
			// cull, when on) actually executes; captures exclude UI.
			if ((m_frameCounter % 300) == 0) {
				std::cerr << "[GI] overlay: drawn=" << drawn
					<< " culled=" << culled
					<< " cull=" << (m_giProbeOcclusionCull ? "on" : "off")
					<< " cache=" << m_giProbeCullCache.size() << "\n";
			}
		}

#if defined(DS_DEBUG_VIZ)
		// DS visualizer: selected-light marker. White ring + "DS#<idx>" at
		// the light's captured POST-SORT position (m_debugLights above holds
		// the pre-sort order, which does not match dsDebugLight's index
		// space). Drawn only while a light-scoped mode is active.
		if (m_dsDebugMode >= DsDebugViz::CullStep && m_dsDebugLightValid
			&& DsActive()) {
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			const ImVec2 disp = ImGui::GetIO().DisplaySize;
			const glm::vec4 clip =
				m_unjitteredViewProj * glm::vec4(m_dsDebugLightPos, 1.0f);
			if (clip.w > 1e-4f) {
				const glm::vec3 ndc = glm::vec3(clip) / clip.w;
				if (std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f) {
					const ImVec2 pt(
						(ndc.x * 0.5f + 0.5f) * disp.x,
						(ndc.y * 0.5f + 0.5f) * disp.y);
					dl->AddCircle(pt, std::max(m_fpDemo.lightPointSize, 4.0f) + 5.0f,
						IM_COL32_WHITE, 24, 2.0f);
					char label[32];
					snprintf(label, sizeof(label), "DS#%d", m_dsDebugLight);
					dl->AddText(ImVec2(pt.x + 1.0f, pt.y - 26.0f + 1.0f),
						IM_COL32(0, 0, 0, 200), label);
					dl->AddText(ImVec2(pt.x, pt.y - 26.0f), IM_COL32_WHITE, label);
				}
			}
		}
#endif


			// Separated UI pass: ImGui renders into its own offscreen target
			// (transparent clear, ui scale resolution), then UiComposite
			// alpha-blends it over the tonemapped swapchain at swap extent.
			// Internally AddPass calls ImGui::Render() +
			// ImGui_ImplVulkan_RenderDrawData inside the pass recording.
			m_imgui->AddPass(*m_rendergraph,
				m_viewportResources.ui.logicalView, uiExtent,
				smvk::AttachmentLoadOp::Clear, smvk::ClearColor(0, 0, 0, 0));
			m_viewportResources.uiComposite.AddCompositePass(*m_rendergraph,
				m_viewportResources.ui.logicalView,
				m_viewportResources.ui.bindlessIndex,
				linearSampler,
				colors,
				swapExtent,
				swapIsSrgb,
				"UiComposite");

			// Manual alternative (equivalent, more explicit):
			// ImGui::Render();
			// m_rendergraph->AddPass("ImGuiManual")
			//     .SetOffset(0,0)
			//     .SetExtent(uiExtent)
			//     .SetColor({m_viewportResources.ui.logicalView}, smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store)
			//     .SetRecordingFunc([this](smvk::CommandBuffer& cmd){ m_imgui->Render(cmd); });
		}

		// Cooperative shadow list builds must land before Execute: the
		// cascade/spot pass recording lambdas read the built ranges at
		// record time. The ring uploads then run on the main thread (the
		// engine's buffer sync-state tables are sharded by handle, not by
		// calling thread) and flush with the EndAndPresent upload batch
		// ahead of every consumer pass.
		if (!m_shadowFutures.empty()) {
			SMVK_PROFILE_SCOPE("ShadowListJoin");
			for (auto& fut : m_shadowFutures)
				fut.get();
			m_shadowFutures.clear();
			{
				SMVK_PROFILE_SCOPE("ShadowListUpload");
				for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci)
					if (cascadeShadowSlot[ci] != UINT32_MAX)
						m_gfxRenderer.UploadShadowList(*m_context, fb.frameIndex, cascadeShadowSlot[ci]);
				for (uint32_t si = 0; si < spotSlotCount; ++si)
					if (spotShadowSlot[si] != UINT32_MAX)
						m_gfxRenderer.UploadShadowList(*m_context, fb.frameIndex, spotShadowSlot[si]);
			}
		}
		const auto rfT4 = bench_clk::now();
		m_rendergraph->Execute(fb);
		const auto rfT5 = bench_clk::now();
		if (benchPhase) {
			using msd = std::chrono::duration<double, std::milli>;
			s_benchUpdA += msd(rfT0b - rfT0).count();
			s_benchUpdB += msd(rfT1 - rfT0b).count();
			s_benchStream += msd(rfT0c - rfT0b).count();
			s_benchFp += msd(rfT1 - rfT0c).count();
			s_benchUpdates += msd(rfT1 - rfT0).count();
			s_benchPasses += msd(rfT2 - rfT1).count();
			s_benchPreMain += msd(rfTM - rfT1).count();
			s_benchPostMain += msd(rfT2 - rfTM).count();
			s_benchPreA += msd(rfPA - rfT1).count();
			s_benchPreB += msd(rfPB - rfPA).count();
			s_benchPreC += msd(rfTM - rfPB).count();
			s_benchReadbacks += msd(rfT3 - rfT2).count();
			s_benchUi += msd(rfT4 - rfT3).count();
			s_benchExecute += msd(rfT5 - rfT4).count();
		}
		if (m_captureQueued) {
			// The capture target was written by the frame's commands, but the
			// submit happens in EndAndPresentFramebuffer (after RenderFrame
			// returns) - the readback must wait for it. DrawFrame picks this up.
			m_captureQueued = false;
			m_captureReadbackPending = true;
		}
		m_context->ProcessDeferrredTasks();
		// Eye adaptation already ran at record time (before this frame's
		// zero-fill); nothing to download here â€” the histogram compute was
		// just recorded into fb.commandBuffer and submits in EndAndPresent.

		// One-shot indirect audit (debug): downloads this frame's filter
		// commands + HZB after Execute so the GPU results are final. Stalls
		// (device-idle wait); button-pressed only.
		if (m_dumpIndirectAudit) {
			m_dumpIndirectAudit = false;
			if (m_useGfxFilter && gfxSlot != UINT32_MAX) {
				WaitForDeviceIdle();
				m_gfxRenderer.DumpIndirectAudit(*m_context, gfxSlot);
			}
		}
		if (m_debugDumpHzb) {
			m_debugDumpHzb = false;
			if (m_useGfxFilter && gfxSlot != UINT32_MAX) {
				WaitForDeviceIdle();
				m_gfxRenderer.DumpHzb(*m_context, gfxSlot);
			}
		}
		// Opt-in first-frame HZB dump (debug): stalls the GPU (device-idle
		// wait), so it stays off unless explicitly enabled.
		if (m_dumpFirstFrameHzb && m_useGfxFilter && gfxSlot != UINT32_MAX) {
			m_dumpFirstFrameHzb = false;
			WaitForDeviceIdle();
			m_gfxRenderer.DumpHzb(*m_context, gfxSlot);
		}
		// Opt-in deferred-shadow dump (VULKANTOY_DS_DUMP=<frame>): one
		// shot after Execute with the phase results final. Stalls
		// (device-idle wait), so it stays off unless explicitly enabled.
		if (m_dumpAtFrame >= 0 && m_frameCounter == (uint64_t)m_dumpAtFrame) {
			m_dumpAtFrame = -1;
			if (DsActive()) {
				WaitForDeviceIdle();
				DumpDeferredShadows(fb.frameIndex);
			}
		}
		// Opt-in EVSM atlas dump (VULKANTOY_EVSM_DUMP=<frame>): one-shot
		// after Execute (the blur completed in this frame's submit â€” the
		// dump waits for device idle first).
		if (m_evsmDumpAtFrame >= 0 && m_frameCounter == (uint64_t)m_evsmDumpAtFrame) {
			m_evsmDumpAtFrame = -1;
			if (EvsmActive()) {
				WaitForDeviceIdle();
				EvsmDumpAtlas(fb.frameIndex);
			}
		}

		// Snapshot this frame's camera + node transforms for the next
		// frame's motion vector pass. Must run AFTER Execute: the motion
		// pass recording lambda reads these members during Execute.
		m_prevViewProj = frameData.viewProj;
		m_prevUnjitteredViewProj = m_unjitteredViewProj;
		m_prevViewProjValid = true;
		m_prevJitterUV = m_currJitterUV;
		m_prevGlobalTransforms = m_world->scene.globalTransform;
		// SMAA T2x global toggle: the neighborhood pass wrote
		// history[parity] during Execute (when the T2x chain ran), so the
		// next record blends against this one. A non-T2x record freezes
		// the phase and invalidates (see SmaaEndFrame).
		smvk::SmaaEndFrame(m_smaaT2x, m_smaaFrameRanT2x);
		// DLSS end-of-record step: advance the Halton phase when the pass
		// ran (record cadence like the T2x parity, never per-image), arm
		// the history reset when it didn't (next activation warms up), and
		// remember the extents/mode this record saw so drift resets.
		if (dlssEmitted) {
			m_dlssPhase = (m_dlssPhase + 1) % (m_dlssPhaseCount ? m_dlssPhaseCount : 1);
			m_dlssReset = false;
		} else {
			m_dlssReset = true;
		}
		m_dlssLastMode = m_dlssMode;
		m_dlssLastRenderExtent = renderExtent;
		m_dlssLastSwapExtent = swapExtent;
	}
}

namespace app {

void SceneRenderer::EvsmDumpAtlas(uint32_t frameIndex) {
	// Opt-in debug (VULKANTOY_EVSM_DUMP=<frame>): per-stage stats over the
	// cascade-0 tile of the moments/temp/blurred images â€” R = E[exp(+c d)],
	// G = E[exp(-c d)] â€” plus a log-encoded G-channel PNG per stage in
	// exports/ (dark = far). The PNG edge-band width IS the prefilter
	// width, which is how the softness knob's effect was verified.
	const smvk::EvsmAtlas& atlas = m_viewportResources.evsmDir;
	// (Holder is move-only: reference the originals instead of copies.)
	const smvk::Holder<smvk::ImageHandle>* stagesArr[3] = {
		&atlas.moments[0], &atlas.blurTemp[0], &atlas.blurred[0]
	};
	const char* stageNames[3] = { "moments", "temp", "blurred" };
	const smvk::ImageLayout stageLayouts[3] = {
		smvk::ImageLayout::ColorAttachmentOptimal,
		smvk::ImageLayout::General,
		smvk::ImageLayout::ShaderReadOnlyOptimal,
	};
	const uint32_t w = atlas.atlasExtent.width;
	const uint32_t h = atlas.atlasExtent.height;
	const size_t bytes = size_t(w) * h * 8;
	std::vector<uint8_t> px(bytes);
	const uint32_t tile = std::min(atlas.tileExtent.width, w);
	for (int s = 0; s < 3; ++s) {
		const smvk::Holder<smvk::ImageHandle>& img = *stagesArr[s];
		const char* name = stageNames[s];
		if (img.empty())
			continue;
		if (!m_context->DownloadDataFromImage(img, stageLayouts[s], px.data(), bytes)) {
			std::fprintf(stderr, "[EVSM-dump] %s readback failed\n", name);
			continue;
		}
		// Per-tile stats (all cascade tiles, not just tile 0): mean/max
		// positive moment + implied depth range per tile.
		for (uint32_t t = 0; t < 4; ++t) {
			int32_t ox = 0, oy = 0;
			atlas.TileOrigin(t, ox, oy);
			double sumR = 0; uint64_t n = 0; float maxR = 0;
			for (uint32_t y = 0; y < tile; y += 8) {
				for (uint32_t x = 0; x < tile; x += 8) {
					const size_t i = (size_t(oy + y) * w + (ox + x)) * 2;
					const float r = reinterpret_cast<const float*>(px.data())[i];
					if (!std::isfinite(r))
						continue;
					sumR += r; ++n;
					if (r > maxR) maxR = r;
				}
			}
			std::fprintf(stderr,
				"[EVSM-dump] %s tile%u: R mean=%.3e max=%.3e (d: %.3f..%.3f)\n",
				name, t, n ? sumR / (double)n : 0.0, maxR,
				n ? std::log(sumR / (double)n) / 42.0 : 0.0,
				std::log(std::max(maxR, 1e-6f)) / 42.0);
		}
		const float* f = reinterpret_cast<const float*>(px.data());
		double sumR = 0, sumG = 0, minR = 1e300, maxR = 0, minG = 1e300, maxG = 0;
		double gradR = 0, gradG = 0; // mean |delta| to the right neighbor
		uint64_t n = 0, ng = 0;
		for (uint32_t y = 0; y < tile; y += 4) {
			for (uint32_t x = 0; x < tile; x += 4) {
				const size_t i = (size_t(y) * w + x) * 2;
				const float r = f[i], g = f[i + 1];
				sumR += r; sumG += g;
				minR = std::min(minR, (double)r); maxR = std::max(maxR, (double)r);
				minG = std::min(minG, (double)g); maxG = std::max(maxG, (double)g);
				if (x + 1 < tile) {
					const size_t j = (size_t(y) * w + x + 1) * 2;
					gradR += std::abs((double)f[j] - r);
					gradG += std::abs((double)f[j + 1] - g);
					++ng;
				}
				++n;
			}
		}
		std::fprintf(stderr,
			"[EVSM-dump] %-8s c0: gradR=%.4e gradG=%.4e | R mean=%.3e max=%.3e | G mean=%.3e min=%.3e (blurR=%u)\n",
			name, gradR / (double)ng, gradG / (double)ng,
			sumR / (double)n, maxR, sumG / (double)n, minG, atlas.blurRadius);
		std::fflush(stderr);
		// PNG of the cascade-0 tile (G channel log-encoded: dark = far).
		// Visual edge-band width = the prefilter width â€” definitive.
		{
			std::vector<uint8_t> rgba(size_t(tile) * tile * 4);
			for (uint32_t y = 0; y < tile; ++y) {
				for (uint32_t x = 0; x < tile; ++x) {
					const size_t i = (size_t(y) * w + x) * 2;
					const size_t o = (size_t(y) * tile + x) * 4;
					// G = e^(-42 d): log2 spread over [-42, 0] -> [0,255].
					const double lg = std::max(0.0, -std::log2(std::max((double)f[i + 1], 1e-30)) / 42.0);
					const uint8_t v = (uint8_t)std::min(255.0, lg * 255.0);
					rgba[o] = v; rgba[o + 1] = v; rgba[o + 2] = v; rgba[o + 3] = 255;
				}
			}
			char path[128];
			std::snprintf(path, sizeof(path), "exports/evsm_%s_r%u.png", name,
				(unsigned)atlas.blurRadius);
			if (LutWritePng(path, (int)tile, (int)tile, rgba.data()))
				std::fprintf(stderr, "[EVSM-dump] wrote %s\n", path);
		}
	}
}

void SceneRenderer::UpdateCamera(float dt)
{
	auto& movement = m_cameraPositioner.movement_;

	movement.forward_ = m_input.isKeyHeld(GLFW_KEY_W);
	movement.backward_ = m_input.isKeyHeld(GLFW_KEY_S);
	movement.left_ = m_input.isKeyHeld(GLFW_KEY_A);
	movement.right_ = m_input.isKeyHeld(GLFW_KEY_D);

	// Check the behacviour of this. 
	// is up and down in camera orientation, not world space.
	movement.up_ = m_input.isKeyHeld(GLFW_KEY_E) || m_input.isKeyHeld(GLFW_KEY_SPACE);
	movement.down_ = m_input.isKeyHeld(GLFW_KEY_Q) || m_input.isKeyHeld(GLFW_KEY_LEFT_CONTROL);

	movement.fastSpeed_ =
		m_input.isKeyHeld(GLFW_KEY_LEFT_SHIFT) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_SHIFT);

	// Gamepad camera: left stick moves (camera-relative), right stick
	// looks, triggers rise/sink, right shoulder sprints. Mouse/keyboard
	// above keep working â€” inputs sum. Sticks use radial deadzones with
	// rescaling (see Input deadzone sliders in Stats).
	if (m_input.hasGamepad()) {
		float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
		m_input.gamepadStick(0, true, lx, ly);
		m_input.gamepadStick(0, false, rx, ry);
		movement.stickMove_.x = lx;
		movement.stickMove_.y = -ly;
		movement.stickLook_.x = rx;
		movement.stickLook_.y = -ry;
		movement.vertical_ =
			m_input.gamepadTrigger(0, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) -
			m_input.gamepadTrigger(0, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
		movement.fastSpeed_ = movement.fastSpeed_ ||
			m_input.gamepadButtonHeld(0, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
	} else {
		movement.stickMove_ = glm::vec2(0.0f);
		movement.stickLook_ = glm::vec2(0.0f);
		movement.vertical_ = 0.0f;
	}

	// ---- Mouselook (SDL relative deltas -> virtual cursor) ----
	// SDL relative mode warps the OS cursor every frame, so the absolute
	// x/y positions the old GLFW path consumed are garbage while captured
	// (they ping-pong around the warp point) â€” that is what broke mouse
	// look. xrel/yrel are valid in BOTH captured and normal modes, so
	// fold them into an unbounded virtual NDC cursor (y down â€” same
	// mapping the old getMouseX/getMouseY path produced, so
	// rotationSpeed_ keeps its calibration). The cursor is captured in
	// FPS mode (nothing to click), so look is always active here; UI
	// mode never reaches UpdateCamera.
	{
		const float w = static_cast<float>(std::max(1, m_swapchain->GetWindowWidth()));
		const float h = static_cast<float>(std::max(1, m_swapchain->GetWindowHeight()));
		m_virtualMouse.x += (m_input.getRelativeDeltaX() / w) * 2.0f;
		// Y flip: the old absolute mapping was 1 - (y/h)*2 (screen-up = +NDC),
		// so mouse-down must DECREASE virtual Y for the positioner's
		// pitch = -rotSpeed * deltaY to look down. relY is +down in SDL.
		m_virtualMouse.y -= (m_input.getRelativeDeltaY() / h) * 2.0f;
	}
	const glm::vec2 mousePos = m_virtualMouse;

	if (m_input.isMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT))
	{
		// Zero the pending delta so a click never yanks the camera.
		m_cameraPositioner.resetMousePosition(mousePos);
	}

	m_cameraPositioner.update(
		static_cast<double>(dt),
		mousePos,
		true
	);
}

void SceneRenderer::SetUIMode(bool ui)
{
	m_uiMode = ui;
	// SDL3: relative mode captures the mouse for FPS; normal mode frees it
	// for ImGui interaction (replaces glfwSetInputMode CURSOR_DISABLED/NORMAL).
	SetRelativeMouseMode(!ui);
	if (ui) {
		// Center cursor for UI interaction; let ImGui take over.
		// Don't reset camera mouse pos on mode switch to avoid jump when returning to FPS.
	} else {
		// When returning to FPS, sync positioner to the virtual cursor so
		// the first relative delta doesn't yank the camera.
		m_cameraPositioner.resetMousePosition(m_virtualMouse);
	}
}

} // namespace app

// NOTE: entry point moved to main.cpp (scene selection + Configure).
