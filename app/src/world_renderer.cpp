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
#include <stdexcept> // hosted ctor failure (std::runtime_error)
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
#include <Engine/platform.h> // readFile (exe-relative SPV loads)
#include <Engine/Asset/bake_asset.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <gltf_import/gltf.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <Engine/Rendergraph/bloom.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <Engine/Rendergraph/environment.h>
#include <Engine/Rendergraph/evsm.h>
#include <Engine/Rendergraph/exposure.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/Rendergraph/spotshadow.h>
#include <Engine/Rendergraph/stencil_bits.h>
#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/msaa.h>
#include <Engine/Rendergraph/motionblur.h>
#include <Engine/Rendergraph/motionvector.h>
#include <Engine/Rendergraph/ssao.h>
#include <Engine/Rendergraph/sao.h>
#include <Engine/Rendergraph/tonemap.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/ui.h>
#include <Engine/Renderer/cookies.h>
#include <Engine/Renderer/spirv_reflection.h> // PackPushFloat3x4 (prevModel blob)
#include <App/renderer.h>
#include <App/forwardplus_renderer.h>
#include "lut_io.h"
#include <App/frame_context.h>
#include <App/push_constants.h>
using app::PushConstants;
#include <Engine/Vfs/vfs.h>
#include <Engine/Utils/texture_loader.h>
#include <App/world_renderer.h>
#include <Engine/Profile/profile.h>
#include <imgui.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


// Constants (clips/FOV live on WorldRenderer as members; the far clip is
// per-scene — m_sceneFarClip, fitted to the framed world bounds at load).
// The WORLD itself is host-provided (SetWorldSource / borrowed): the
// renderer never loads scene content of its own. PIPELINE SHADERS are not
// content: they load from the exe-relative content/shaders/ via readFile
// (engine-builder parity), never through the content VFS.
// Bench phase accumulators (RenderFrame sub-timings, --bench only).
static double s_benchUpdates = 0.0, s_benchPasses = 0.0, s_benchReadbacks = 0.0,
	s_benchUi = 0.0, s_benchExecute = 0.0, s_benchUpdA = 0.0, s_benchUpdB = 0.0,
	s_benchStream = 0.0, s_benchFp = 0.0, s_benchFpGen = 0.0, s_benchFpUp = 0.0,
	s_benchFpDs = 0.0, s_benchPreMain = 0.0, s_benchPostMain = 0.0,
	s_benchPreA = 0.0, s_benchPreB = 0.0, s_benchPreC = 0.0;

// -----------------------------------------------------------------------------
// Directional cascade shadow-map config (resources + math live in
// Engine/Rendergraph/cascadedshadowmap.h)
// -----------------------------------------------------------------------------
static constexpr uint32_t NUM_CASCADES = 4;
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
static constexpr float TONEMAP_EXPOSURE = 0.001f; // sun 5000 lux * 0.001 ≈ HDR 5 pre-tonemap
static constexpr uint32_t TONEMAP_MODE = 1; // 0=Reinhard, 1=ACES, 2=Uncharted2, 3=GT7

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
	d.tileDilation = true; // CoD:AW tile max + 5x5 dilate prepasses
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
// SAO config (half-res spiral-tap volume obscurance, see Engine/Rendergraph/sao.h).
// Same consumer path as SSAO/GTAO (blur + depth-guided compose + opaque ambient),
// only the raw pass differs. Shares radius/bias/intensity/maxDistance/blur
// with SSAO; sampleCount/spiralTurns are SAO-specific.
// -----------------------------------------------------------------------------
static constexpr uint32_t SAO_SAMPLES = 11; // spiral taps 1..32
static constexpr uint32_t SAO_SPIRAL_TURNS = 7; // extra spiral twist 1..16

static smvk::SaoDesc MakeSaoDesc() {
	smvk::SaoDesc d{};
	d.format = smvk::Format::R8_UNorm;
	d.radius = SSAO_RADIUS;
	d.bias = SSAO_BIAS;
	d.intensity = SSAO_INTENSITY;
	d.maxDistance = SSAO_MAX_DISTANCE;
	d.sampleCount = SAO_SAMPLES;
	d.spiralTurns = SAO_SPIRAL_TURNS;
	d.blurSharpness = SSAO_BLUR_SHARPNESS;
	d.name = "sao";
	d.rawName = "saoRaw";
	d.blurName = "saoBlur";
	d.composeName = "saoComposed";
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

static smvk::CascadedShadowMapDesc MakeShadowDesc(uint32_t resolution, float lambda) {
	smvk::CascadedShadowMapDesc d{};
	d.cascadeCount = NUM_CASCADES;
	d.cascadeResolution = resolution;
	d.format = SHADOW_FORMAT;
	d.lambda = lambda;
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
// Probe entries ride the light buffer tail (see smvk::kForwardPlusProbe),
// so light building caps early: appended probes can never truncate a
// selected shadow spot, and the flight-slice stride still fits.
static constexpr uint32_t FP_PROBE_RESERVE = smvk::kForwardPlusMaxProbes;
static constexpr uint32_t FP_MAX_BUILD_LIGHTS = FP_MAX_LIGHTS - FP_PROBE_RESERVE;
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
// GPU particle config (resources + passes live in
// Engine/Rendergraph/particles.h). Caps must stay in sync with the shader
// defines MAX_PARTICLES / MAX_PARTICLE_EMITTERS and the sim numthreads.
// -----------------------------------------------------------------------------
static constexpr uint32_t PARTICLE_MAX = 65536;
static constexpr uint32_t PARTICLE_MAX_EMITTERS = 16;
static constexpr uint32_t PARTICLE_SIM_THREADS = 64;

static smvk::ParticlesDesc MakeParticlesDesc() {
	smvk::ParticlesDesc d{};
	d.maxParticles = PARTICLE_MAX;
	d.maxEmitters = PARTICLE_MAX_EMITTERS;
	d.simThreads = PARTICLE_SIM_THREADS;
	d.name = "particles";
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
// EVSM moment atlases (RGBA32F 2xMSAA + async separable blur, 1-frame lag).
// Tile counts/resolutions mirror the PCF atlases so the same lightVP math
// (BuildDirectionalLight / BuildSpotTile) drives both representations.
// -----------------------------------------------------------------------------
static smvk::EvsmDesc MakeEvsmDirDesc(smvk::EvsmPipelineMode pipeline, uint32_t tileResolution) {
	smvk::EvsmDesc d{};
	d.tileResolution = tileResolution;
	d.tileCount = NUM_CASCADES;
	d.momentsFormat = smvk::Format::RGBA32_Float;
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
	d.momentsFormat = smvk::Format::RGBA32_Float;
	d.depthFormat = smvk::Format::D32_Float;
	d.msaaSamples = 2;
	d.exponent = smvk::kEvsmExponent;
	d.blurRadius = smvk::kEvsmBlurRadius;
	d.pipeline = pipeline;
	d.name = "evsmSpotAtlas";
	return d;
}

// ---------------------------------------------------------------------------
// Pass-set config + the camera's generic default pose. The world itself
// arrives through the host's loader (SetWorldSource) or borrowed
// (SetHostedBorrowedWorld) � nothing about the scene is known here. The
// load-time framing (Initialize) re-poses the camera on the world bounds
// unless VULKANTOY_CAM_* pins it for benches.
// ---------------------------------------------------------------------------
namespace app {

void WorldRenderer::Configure(ScenePassConfig passes) {
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
			std::cerr << "[WorldRenderer] bad VULKANTOY_AA (want "
				"off|msaa2x|smaa1x|smaat2x): " << aaEnv << "\n";
	}
	// Opt-in A2C for masked cutout in MSAA mode (quality trade-off, see
	// m_msaaAlphaToCoverage).
	if (const char* a2cEnv = std::getenv("VULKANTOY_A2C")) {
		m_msaaAlphaToCoverage = std::atoi(a2cEnv) != 0;
	}
	// Texture filtering default for headless A/B (bilinear | trilinear).
	// The ImGui combo stays live after; applied every RenderFrame.
	if (const char* filtEnv = std::getenv("VULKANTOY_TEXTURE_FILTER")) {
		if (!_stricmp(filtEnv, "bilinear"))
			m_textureFiltering = TextureFiltering::Bilinear;
		else if (!_stricmp(filtEnv, "trilinear"))
			m_textureFiltering = TextureFiltering::Trilinear;
		else
			std::cerr << "[WorldRenderer] bad VULKANTOY_TEXTURE_FILTER (want "
				"bilinear|trilinear): " << filtEnv << "\n";
	}
	// Point the FPS camera at a generic default viewpoint (Camera keeps a
	// stable pointer to m_cameraPositioner, so lookAt is assignment-safe).
	// Initialize re-poses this on the loaded world's bounds (generic
	// framing) unless the env vars below pin it for benches.
	const glm::vec3 camPos(4.0f, 3.0f, 6.0f);
	const glm::vec3 camTarget(0.0f, 1.0f, 0.0f);
	m_cameraPositioner.lookAt(camPos, camTarget, glm::vec3(0.0f, 1.0f, 0.0f));
	// Debug: rotate the default view around world up (degrees) � A/B runs
	// without mouse input.
	glm::vec3 dir = glm::normalize(camTarget - camPos);
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
			std::cerr << "[WorldRenderer] bad VULKANTOY_CAM_FWD (want x,y,z): "
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
		glm::vec3 pos(camPos);
		if (std::sscanf(posEnv, "%f,%f,%f", &pos.x, &pos.y, &pos.z) == 3) {
			m_cameraPositioner.lookAt(pos, pos + dir, glm::vec3(0.0f, 1.0f, 0.0f));
			return;
		}
		std::cerr << "[WorldRenderer] bad VULKANTOY_CAM_POS (want x,y,z): "
			<< posEnv << "\n";
	}
	m_cameraPositioner.lookAt(camPos, camPos + dir, glm::vec3(0.0f, 1.0f, 0.0f));
}
// FrameUniforms / AppResources / ViewportResources live in
// App/world_renderer.h (needed by the class declaration); method
// definitions follow the (removed) in-class block below.

} // namespace app (scene registry + helpers above; methods open their own block below)


// Class declaration lives in App/world_renderer.h; out-of-line method
// definitions follow.
namespace app {
using Super = smvk::AppImpl;

void WorldRenderer::DriveExternalCamera(const glm::vec3& eye,
	const glm::vec3& target, float fovYDeg, float zNear, float zFar,
	float orthoHalfH) {
	// Degenerate-lookAt guard (ResolveLookAtUp — the SINGLE shared
	// resolution, same one the editor's overlay camera uses): a view
	// direction parallel to the up vector (top-down framing) crosses NaNs
	// into the view matrix — resolve the up axis instead of nudging the
	// target (a nudge pans the ortho view AND disagrees with the
	// direct-lookAt consumers, which rotated the editor's wire boxes
	// against the scene in the straight-down top view).
	if (glm::length(target - eye) < 1e-6f)
		return; // eye == target: nothing sensible to look at (keep last)
	m_extCamera = true;
	m_extFovYDeg = fovYDeg;
	m_extOrthoHalfH = std::max(orthoHalfH, 0.0f);
	m_extNear = zNear;
	m_extFar = zFar;
	// The positioner drives the view matrix (+ every viewProj-derived
	// consumer: culling, cascades, motion, markers). The proj constants
	// ride UpdateCameraAspect's external branch (next RenderFrame).
	m_cameraPositioner.lookAt(eye, target,
		ResolveLookAtUp(eye, target));
}

bool WorldRenderer::Initialize() {
		if (!Super::Initialize())
			return false;

		// Static geo heaps FIRST (3 x 512 MB device buffers + VMA virtual
		// blocks): everything downstream (pak world source upload below,
		// shader/pipeline setup) can overlap their use, and the pak loader's
		// heap writes must not wait behind shader loading. Idempotent, so
		// the editor's own early call stays safe.
		smvk::AllocateStaticVertexFmt(*m_context);

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
		// Headless LOD cadence kill (VULKANTOY_LODFULL=0 restores the old
		// halfrate refresh: parity-split, 1-frame-stale decisions).
		if (const char* lodFullEnv = std::getenv("VULKANTOY_LODFULL")) {
			if (std::atoi(lodFullEnv) == 0)
				m_lodFullRate = false;
		}
		// Headless shader-LOD kill (VULKANTOY_SHADERLOD=0 keeps the
		// farthest level full-lit) for A/B shading-cost runs.
		if (const char* lodShEnv = std::getenv("VULKANTOY_SHADERLOD")) {
			if (std::atoi(lodShEnv) == 0)
				m_lodShaderEnabled = false;
		}
		// Debug: kill the Northlight depth split (VULKANTOY_SPLIT=0 falls
		// back to the legacy flow: full prepass before the HZB, remainder
		// pass after the filter). A/B which assembly owns a regression.
		if (const char* splitEnv = std::getenv("VULKANTOY_SPLIT")) {
			if (std::atoi(splitEnv) == 0)
				m_splitDisabled = true;
		}
		// Headless wireframe opt-in (VULKANTOY_WIREFRAME=1) for capture/A-B
		// runs without UI interaction; the P toggle stays live after.
		if (const char* wfEnv = std::getenv("VULKANTOY_WIREFRAME")) {
			if (std::atoi(wfEnv) == 1)
				m_wireframe = true;
		}
		// Headless filter cull-flags override (hex, base-0 auto): A/B the
		// GPU filter stages without UI interaction. Bits mirror
		// smvk::gfx::CullFlagBits (e.g. 0xb = degenerate|backface|frustum,
		// 0x4b adds HZB occlusion, 0x24b the default + MASK exclusion).
		if (const char* cullEnv = std::getenv("VULKANTOY_CULLFLAGS")) {
			const unsigned long long flags =
				std::strtoull(cullEnv, nullptr, 0);
			m_gfxRenderer.config.cullFlags =
				static_cast<smvk::gfx::CullFlags>(flags);
		}
		// Debug: one-shot [GfxAudit] indirect dump at frame N (stderr,
		// stalls; per-batch GPU kill reasons for bisecting the filter).
		if (const char* audEnv = std::getenv("VULKANTOY_AUDIT")) {
			const int auditFrame = std::atoi(audEnv);
			if (auditFrame >= 0)
				m_auditAtFrame = auditFrame;
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
			else if (dv == "pom" || dv == "parallax" || dv == "parallaxocclusion") m_debugView = DebugView::Pom;
		else if (dv == "ssr" || dv == "ssr_contribution") m_debugView = DebugView::SsrContribution;
		else std::cerr << "[WorldRenderer] unknown VULKANTOY_DEBUGVIEW value: " << dv << "\n";
		}
		// AA mode override for A/B runs without UI interaction
		// (off | msaa | smaa1x | t2x). The ImGui combo stays live after.
		if (const char* aaEnv = std::getenv("VULKANTOY_AA")) {
			const std::string_view aa = trimEnv(aaEnv);
			if (aa == "off") m_aaMode = AaMode::Off;
			else if (aa == "msaa") m_aaMode = AaMode::Msaa2x;
			else if (aa == "smaa1x") m_aaMode = AaMode::Smaa1x;
			else if (aa == "t2x") m_aaMode = AaMode::SmaaT2x;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_AA value: " << aa << "\n";
		}
		// Texture filtering override for A/B runs without UI interaction
		// (bilinear | trilinear). The ImGui combo stays live after.
		if (const char* filtEnv = std::getenv("VULKANTOY_TEXTURE_FILTER")) {
			const std::string_view f = trimEnv(filtEnv);
			if (f == "bilinear") m_textureFiltering = TextureFiltering::Bilinear;
			else if (f == "trilinear") m_textureFiltering = TextureFiltering::Trilinear;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_TEXTURE_FILTER value: " << f << "\n";
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
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_DLSS value: " << dlss << "\n";
		}
		// SSR / volumetric fog opt-in toggles for headless A/B + capture
		// runs (both default off; the ImGui checkboxes stay live after).
		if (const char* ssrEnv = std::getenv("VULKANTOY_SSR")) {
			const std::string_view ssr = trimEnv(ssrEnv);
			if (ssr == "1" || ssr == "on") m_ssrEnabled = true;
			else if (ssr == "0" || ssr == "off") m_ssrEnabled = false;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_SSR value: " << ssr << "\n";
		}
		if (const char* volEnv = std::getenv("VULKANTOY_VOLUMETRIC")) {
			const std::string_view vol = trimEnv(volEnv);
			if (vol == "1" || vol == "on") m_volumetricEnabled = true;
			else if (vol == "0" || vol == "off") m_volumetricEnabled = false;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_VOLUMETRIC value: " << vol << "\n";
		}
		// Pane-layer (SSR pane receivers) A/B knob: 0 keeps the pre-layer
		// behavior — no PaneMask pass, trace reflects off the opaque
		// depth only (the seen-through background at pane pixels).
		if (const char* glassEnv = std::getenv("VULKANTOY_SSR_GLASS")) {
			const std::string_view gl = trimEnv(glassEnv);
			if (gl == "0" || gl == "off") m_ssrGlassEnabled = false;
			else if (gl == "1" || gl == "on") m_ssrGlassEnabled = true;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_SSR_GLASS value: " << gl << "\n";
		}

		// Internal render-resolution override for headless A/B runs (the
		// ImGui "Resolution" slider stays live after; 1.0 = swapchain 1:1).
		if (const char* rsEnv = std::getenv("VULKANTOY_RENDER_SCALE")) {
			const float rs = std::strtof(rsEnv, nullptr);
			if (rs >= 0.25f && rs <= 2.0f)
				m_renderScale = rs;
			else
				std::cerr << "[WorldRenderer] VULKANTOY_RENDER_SCALE out of range "
					<< "(0.25..2.0): " << rsEnv << "\n";
		}

		// Transparency technique override for A/B runs without UI
		// interaction (dither | blend). Default is dithered; the ImGui
		// combo stays live after.
		if (const char* trEnv = std::getenv("VULKANTOY_TRANSPARENCY")) {
			const std::string_view tr = trimEnv(trEnv);
			if (tr == "dither" || tr == "dithered") m_transparentMode = smvk::TransparentMode::Dithered;
			else if (tr == "blend" || tr == "blended") m_transparentMode = smvk::TransparentMode::Blended;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_TRANSPARENCY value: " << tr << "\n";
		}

		// Deferred shadows override for A/B runs without UI interaction
		// (1 | 0). Runtime H toggle + the ImGui checkbox stay live after.
		if (const char* dsEnv = std::getenv("VULKANTOY_DS")) {
			const std::string_view ds = trimEnv(dsEnv);
			if (ds == "1" || ds == "on") m_dsEnabled = true;
			else if (ds == "0" || ds == "off") m_dsEnabled = false;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_DS value: " << ds << "\n";
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
				std::cerr << "[WorldRenderer] unknown VULKANTOY_DS_VIZ value: "
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
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_LIGHTS value: " << lights << "\n";
		}
		// Forward+ decals (tiled projected splats) toggle (1 | 0):
		// 0 also skips the Sponza preset splats at load; J + the ImGui
		// checkbox stay live after.
		if (const char* decEnv = std::getenv("VULKANTOY_DECALS")) {
			const std::string_view dec = trimEnv(decEnv);
			if (dec == "1" || dec == "on") m_decalDemo.enabled = true;
			else if (dec == "0" || dec == "off") m_decalDemo.enabled = false;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_DECALS value: " << dec << "\n";
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
		// EVSM opt-in (default OFF; VULKANTOY_EVSM=1 forces on; =0 forces
		// the PCF depth atlases off). Main-view feature — the secondary
		// multiviews keep PCF depth atlases either way.
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
	// Opt-in SSR/volumetric stage dump (VULKANTOY_SSR_DUMP=<frame>): see
	// header — fires at N and N+1 (one-frame-lag discrimination).
	if (const char* ssrDumpEnv = std::getenv("VULKANTOY_SSR_DUMP")) {
		m_ssrDumpAtFrame = std::atoi(ssrDumpEnv);
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
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_AMBIENT value: "
				<< a << " (all|lightmaps|probes|skyibl|off)\n";
			std::cerr << "[WorldRenderer] ambient source: "
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
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_GI value: " << gi << "\n";
		}
		// GI debug view for headless runs (same modes the I key cycles;
		// F8 is the legacy alias): 0 off, 1 baked irradiance,
		// 2 source palette.
		if (const char* giDbgEnv = std::getenv("VULKANTOY_GI_DEBUG")) {
			const int mode = std::atoi(giDbgEnv);
			if (mode >= 0 && mode <= 2)
				m_giDebug = mode;
			else
				std::cerr << "[WorldRenderer] VULKANTOY_GI_DEBUG wants 0..2\n";
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
		// MULTIVIEW slab sizing: the five per-view buffers below get a
		// VIEW-MAJOR stride — (view * framesInFlight + frame) * element —
		// so the main view (view 0) keeps today's layout while each hosted
		// secondary view owns a parallel slice (its own FrameUniforms, sun
		// light + CSM fit, Forward+ light set with its own spot-shadow
		// patch, spot tiles). Materials + decals stay per-frame only
		// (view-independent content).
		const uint32_t viewSlabs = 1u + kMaxSecondaryViews;
		m_resources.frameUniforms = m_context->CreateBuffer(
			sizeof(FrameUniforms) * m_swapchain->framesInFlight() * viewSlabs,
			smvk::BufferType_Uniform,
			"frame uniforms"
		);
		m_resources.frameHot = m_context->CreateBuffer(
			sizeof(FramePSHot) * m_swapchain->framesInFlight() * viewSlabs,
			smvk::BufferType_Uniform,
			"frame hot uniforms"
		);
		m_resources.lightsBuffer = m_context->CreateBuffer(
			sizeof(smvk::Light) * MAX_LIGHTS * m_swapchain->framesInFlight() * viewSlabs,
			smvk::BufferType_Storage,
			"lights buffer"
		);
		m_resources.materialsBuffer = m_context->CreateBuffer(
			sizeof(smvk::MaterialValues) * MAX_MATERIALS * m_swapchain->framesInFlight(),
			smvk::BufferType_Storage,
			"materials buffer"
		);
		m_resources.directionalCascadeBuffer = m_context->CreateBuffer(
			sizeof(smvk::GpuDirectionalLightDescription) * m_swapchain->framesInFlight() * viewSlabs,
			smvk::BufferType_Storage,
			"directional cascade buffer"
		);
		m_resources.fpLightsBuffer = m_context->CreateBuffer(
			sizeof(smvk::ForwardPlusLight) * FP_MAX_LIGHTS * m_swapchain->framesInFlight() * viewSlabs,
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
			sizeof(smvk::GpuCascadeDescription) * smvk::kMaxSpotShadows * m_swapchain->framesInFlight() * viewSlabs,
			smvk::BufferType_Storage,
			"spot shadow tiles buffer"
		);
		m_resources.spotShadowLogical = m_rendergraph->AddExternalBuffer(
			m_resources.spotShadowBuffer, "spot shadow tiles");
		// RenderView slab: per-frame virtual cameras (main + shadow views).
		if (!m_renderViews.Ensure(*m_context, m_swapchain->framesInFlight())) {
			std::cerr << "[WorldRenderer] Failed to create render views buffer\n";
			return false;
		}
		// Procedural blood-splat textures for the decal system (bindless;
		// failure is non-fatal — DecalsActive() stays false and the
		// decal legs simply never run).
		if (!InitDecalTextures()) {
			std::cerr << "[WorldRenderer] decal textures unavailable - decals off\n";
		}
		// GPU particle demo emitters (world-space defaults near the scene
		// origin; the panel below retouches them live).
		SeedParticleEmitters();

		// VFS overlay: loose content/ for iteration, cooked paks mounted
		// underneath (loose files win on collisions). Shader reads below
		// go through the same overlay so packaged builds just work.
		// Engine-owned baked resources (light cookies, staged next to the
		// app by its CMake step) sit at the lowest priority so app
		// content/paks always win; CookieLibrary reads its .ktx2 files
		// through this overlay.
		smvk::OverlayVfs contentVfs;
		MountContentVfs(contentVfs);

		// Main ShaderProgram (scene-owned; shadow/bloom/tonemap programs live
		// in their engine builders with defaults + Ensure).
		auto vertCode = readFile("content/shaders/default.vert.hlsl.spv");
		auto fragCode = readFile("content/shaders/default_lit.frag.hlsl.spv");
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
		auto a2cCode = readFile("content/shaders/default_lit_a2c.frag.hlsl.spv");
		m_resources.fragA2CShader = m_context->CreateShaderModule(a2cCode, "fragLitA2C");
	}
	catch (...) {
		std::cerr << "[WorldRenderer] default_lit_a2c unavailable - masked MSAA keeps the discard test\n";
	}
	// Fixed-lobe lit FS twins (SG permutations for the Main filtered
	// leg): per-count SG evaluation without the runtime basis dispatch.
	// Non-fatal: a missing variant keeps that count on the generic lit.
	for (uint32_t li = 0; li < app::GfxRenderer::LitSgVariantCount(); ++li) {
		try {
			const std::string path = std::string("content/shaders/default_lit.frag.hlsl.sg")
				+ std::to_string(app::GfxRenderer::LitSgVariantLobes(li)) + ".spv";
			auto sgCode = readFile(path);
			m_resources.fragLitSgModules[li] =
				m_context->CreateShaderModule(sgCode, "fragLitSg");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] SG lobe-count lit variant "
				<< li << " unavailable - count falls back to generic lit\n";
		}
	}

	// GI permutation lit FS variants: each permutation compiles a different
	// GI path (none/octree/regular/full). The app selects the matching
	// program at draw time based on loaded GI assets.
	// Non-fatal: missing variants fall back to the base mainProgram.
	for (uint32_t gi = 0; gi < 4; ++gi) {
		try {
			const std::string path = std::string("content/shaders/default_lit.frag.hlsl.gi")
				+ std::to_string(gi) + ".spv";
			auto giCode = readFile(path);
			m_resources.fragLitGiModules[gi] =
				m_context->CreateShaderModule(giCode, "fragLitGi");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] GI permutation lit variant "
				<< gi << " unavailable - falls back to generic lit\n";
		}
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
			auto unlitCode = readFile("content/shaders/default_unlit.frag.hlsl.spv");
			m_resources.fragUnlitShader = m_context->CreateShaderModule(unlitCode, "fragUnlit");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] default_unlit unavailable - F2 falls back to Lit\n";
		}
		try {
			auto cascadeCode = readFile("content/shaders/default_cascade_debug.frag.hlsl.spv");
			m_resources.fragCascadeShader = m_context->CreateShaderModule(cascadeCode, "fragCascade");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] default_cascade_debug unavailable - F4 falls back to Lit\n";
		}
		try {
			auto vcolorCode = readFile("content/shaders/default_vertexcolor_debug.frag.hlsl.spv");
			m_resources.fragVertexColorShader = m_context->CreateShaderModule(vcolorCode, "fragVertexColor");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] default_vertexcolor_debug unavailable - F6 falls back to Lit\n";
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

	// GI permutation lit programs (one per GI variant: NONE/OCTREE/REGULAR/FULL).
	// Created if the corresponding fragment module loaded successfully.
	// The app selects the matching program at draw time based on loaded GI assets.
	for (uint32_t gi = 0; gi < 4; ++gi) {
		if (!m_resources.fragLitGiModules[gi].empty()) {
			m_resources.mainProgramGi[gi] = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragLitGiModules[gi],
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}
	}

	try {
		auto uv0Code = readFile("content/shaders/default_uv0_debug.frag.hlsl.spv");
			m_resources.fragUv0DebugShader = m_context->CreateShaderModule(uv0Code, "fragUv0Debug");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] default_uv0_debug unavailable - F9 falls back to Lit\n";
		}
		try {
			auto uv1Code = readFile("content/shaders/default_uv1_debug.frag.hlsl.spv");
			m_resources.fragUv1DebugShader = m_context->CreateShaderModule(uv1Code, "fragUv1Debug");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] default_uv1_debug unavailable - F10 falls back to Lit\n";
		}
		try {
			auto pomCode = readFile("content/shaders/default_pom.frag.hlsl.spv");
			m_resources.fragPomShader = m_context->CreateShaderModule(pomCode, "fragPom");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] default_pom unavailable - F11 falls back to Lit\n";
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

		// SSR contribution debug view (F12): shows the SSR contribution isolated
		// (SSR layer only, without the base HDR). The SSR thin G-buffer (oct normal +
		// roughness + metal) is written by the Forward+ depth prepass; the trace
		// uses real normals (no wireframe artifact) and continuous roughness.
		// Mirror the UV debug pattern: vert from default.vert, frag from
		// ssr_contribution_debug.frag.hlsl.
		if (!m_resources.fragSsrContributionDebugShader.empty()) {
			m_resources.ssrContributionDebugProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragSsrContributionDebugShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}
		if (!m_resources.fragPomShader.empty()) {
			m_resources.pomProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragPomShader,
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
			auto vlVertCode = readFile("content/shaders/default_vertexlit.vert.hlsl.spv");
			auto vlFragCode = readFile("content/shaders/default_vertexlit.frag.hlsl.spv");
			m_resources.vertexLitVertShader = m_context->CreateShaderModule(vlVertCode, "vertLitLod");
			m_resources.vertexLitFragShader = m_context->CreateShaderModule(vlFragCode, "fragLitLod");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] default_vertexlit unavailable - shader LODs stay full-lit\n";
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
			auto wireCode = readFile("content/shaders/default_wire.frag.hlsl.spv");
			m_resources.fragWireShader = m_context->CreateShaderModule(wireCode, "fragWire");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] default_wire unavailable - wireframe keeps the shaded FS\n";
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

	// Pane-depth writer (SSR pane reflections — the surviving pane leg of
	// the old GlassMask redraw, see the PaneMask pass): same default.vert
	// VS + a FS that outputs the pane fragment's reverse-Z depth — a FS
	// consuming only SV_Position links with it, same trick as the wire
	// twin. Non-fatal: missing module leaves the program empty, the
	// PaneMask pass is skipped, and SSR keeps the opaque-depth receiver.
	try {
		auto paneDepthCode = readFile("content/shaders/panedepth.frag.hlsl.spv");
		m_resources.fragPaneDepthShader = m_context->CreateShaderModule(paneDepthCode, "fragPaneDepth");
	}
	catch (...) {
		std::cerr << "[WorldRenderer] panedepth unavailable - SSR keeps the opaque receiver\n";
	}
	if (!m_resources.fragPaneDepthShader.empty()) {
		m_resources.paneDepthProgram = {
			.shaders = {
				smvk::Shader{
					.module = m_resources.vertShader,
					.entryPoint = "mainVS",
					.stage = smvk::ShaderStage::Vertex
				},
				smvk::Shader{
					.module = m_resources.fragPaneDepthShader,
					.entryPoint = "mainPS",
					.stage = smvk::ShaderStage::Fragment
				}
			},
			.shaderCount = 2
		};
	}

	// Masked shadow-depth program (leaf-shaped shadows for cutout).
	auto shadowVertCode = readFile("content/shaders/shadow.vert.hlsl.spv");
	auto shadowFragCode = readFile("content/shaders/shadow.frag.hlsl.spv");
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
		auto shadowRevCode = readFile("content/shaders/shadow_rev.frag.hlsl.spv");
		m_resources.shadowMaskedFragRevShader = m_context->CreateShaderModule(shadowRevCode, "shadowMaskedFragRev");
	}
	catch (...) {
		std::cerr << "[WorldRenderer] shadow_rev unavailable - prepass masked keeps the normal-Z FS\n";
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

	// SSSR-style thin G-buffer programs (see gbuffer.frag.hlsl): the
	// Forward+ depth prepass's G-buffer writers — default.vert (the
	// already-loaded vertShader; its world normal output at location 3
	// is the G-buffer's normal source) + the trivial solid/cutout FS
	// pair. The prepass binds these instead of the VS-only depthProgram
	// + prepassMaskedProgram when the G-buffer layer is wanted; Main is
	// untouched (no MRT, no stencil classification).
	try {
		auto gbufferCode = readFile("content/shaders/gbuffer.frag.hlsl.spv");
		auto gbufferMaskedCode = readFile("content/shaders/gbuffer_masked.frag.hlsl.spv");
		m_resources.gbufferFragShader =
			m_context->CreateShaderModule(gbufferCode, "gbufferFrag");
		m_resources.gbufferMaskedFragShader =
			m_context->CreateShaderModule(gbufferMaskedCode, "gbufferMaskedFrag");
		if (!m_resources.gbufferFragShader.empty()
			&& !m_resources.gbufferMaskedFragShader.empty()) {
			m_resources.gbufferProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.gbufferFragShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
			m_resources.gbufferMaskedProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.gbufferMaskedFragShader,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}
	}
	catch (...) {
		std::cerr << "[WorldRenderer] gbuffer shaders unavailable - prepass keeps the VS-only legacy\n";
	}

	// SSSR trace Hi-Z build module (hzb_max.cs.hlsl — the MAX-reduce
	// nearest-surface twin of the culling MIN pyramid; see the header
	// comment on m_ssrHzb*). Failure is non-fatal: the trace keeps the
	// plain adaptive-stride march (the HZB skip stays off, BDA 0).
	try {
		auto hzbMaxCode = readFile("content/shaders/hzb_max.cs.hlsl.spv");
		m_ssrHzbModule = m_context->CreateShaderModule(hzbMaxCode, "ssrHzbMaxCS");
		if (!m_ssrHzbModule.empty()) {
			smvk::Shader cs{
				.module = m_ssrHzbModule,
				.entryPoint = "mainCS",
				.stage = smvk::ShaderStage::Compute };
			m_ssrHzbProgram = { .shaders = { cs }, .shaderCount = 1 };
		}
	}
	catch (...) {
		std::cerr << "[WorldRenderer] hzb_max unavailable - SSR trace keeps the linear march\n";
	}

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
				smvk::mem::MakeProcess<smvk::TextureStreamer>(*m_context, streamCfg);
		}
		smvk::TextureStreamer* sceneStreamer =
			(m_textureStreamingEnabled && m_textureStreamer) ? m_textureStreamer.get() : nullptr;

		// WORLD acquisition: the host owns all content loading; this
		// renderer never imports a scene of its own.
		// LIVE VIEW (SetHostedBorrowedWorld): the host (editor) handed over
		// its OWN resolved world — the live engine representation (imports
		// preprocessed, references bound by GUID, edits applied). The whole
		// scene-build + resolve block is the host's job and SKIPPED;
		// the framing/lights/GI/registration steps that follow operate on
		// the borrowed world unchanged. The world stays the HOST's: it
		// mutates it live (edits), the hosted renderer renders it per
		// frame — world swaps ride RebindWorld (in place, no relaunch).
		if (IsHosted() && HostedWorldBorrowed()) {
			std::cerr << "[Scene] live borrowed world: nodes="
				<< m_world->scene.hierarchy.size() << " meshes="
				<< m_world->meshLocations.size() << " materials="
				<< m_world->materialLocations.size()
				<< " (host's engine representation; no import)\n";
		}
		else {
			// WINDOWED RUNS: the host-installed world source owns all
			// content loading (SetWorldSource � the App's main.cpp
			// loads the pak, the editor borrows its live world above).
			// The world arrives RESOLVED, its clusters/resources already
			// uploaded to the context � everything from here on renders
			// by handles; this class never parses a pak or imports a
			// scene.
			if (!m_worldSource) {
				std::cerr << "[WorldRenderer] no world: install a world "
					"source (SetWorldSource) or borrow one "
					"(SetHostedBorrowedWorld)\n";
				return false;
			}
			m_world = m_worldSource(*m_context, contentVfs);
			if (!m_world) {
				std::cerr << "[WorldRenderer] world source failed - "
					"nothing to render (see the errors above)\n";
				return false;
			}
			std::cerr << "[Scene] world resolved: nodes="
				<< m_world->scene.hierarchy.size() << " meshes="
				<< m_world->meshLocations.size() << " materials="
				<< m_world->materialLocations.size() << std::endl;		} // !liveWorld (scene build + resolve)

		// Fit the procedural Forward+ light grid to the loaded scene's
		// world bounds so the crowd fills Bistro/Sponza instead of
		// clustering around the origin.
		{
			const ::Aabb bounds = m_world->ComputeSceneBounds();
			if (bounds.mx.x >= bounds.mn.x && bounds.mx.y >= bounds.mn.y && bounds.mx.z >= bounds.mn.z) {
				m_fpDemo.FitToBounds(bounds.mn, bounds.mx);
				std::cerr << "[Scene] bounds min=(" << bounds.mn.x << "," << bounds.mn.y << "," << bounds.mn.z
					<< ") max=(" << bounds.mx.x << "," << bounds.mx.y << "," << bounds.mx.z << ")\n";
				// Generic framing: no curated viewpoints exist anymore —
				// park the camera radius*2.2 from the bounds center and
				// fit the far clip to the framed view (the 2000 m default
				// stretches the cascade splits over empty space; cascade 0
				// covers ~100 m -> blurry EVSM/PCF at the mesh). Explicit
				// VULKANTOY_CAM_POS/FWD/YAW pins win (bench determinism).
				// YAW included: the Configure-time yaw override is one of
				// the documented pins — without it here a yaw-only bench
				// would silently re-frame onto the generic orbit pose
				// (observed: four yaw A/B runs rendered bit-identical).
				if (!std::getenv("VULKANTOY_CAM_POS") &&
					!std::getenv("VULKANTOY_CAM_FWD") &&
					!std::getenv("VULKANTOY_CAM_YAW")) {
					const glm::vec3 center = (bounds.mn + bounds.mx) * 0.5f;
					const float radius =
						glm::length(bounds.mx - bounds.mn) * 0.5f;
					const float dist = std::max(radius * 2.2f, 1.0f);
					const float yaw = glm::radians(35.0f);
					const float pitch = glm::radians(-18.0f);
					const glm::vec3 eye = center + dist * glm::vec3(
						std::cos(pitch) * std::sin(yaw),
						-std::sin(pitch),
						std::cos(pitch) * std::cos(yaw));
					m_cameraPositioner.lookAt(eye, center,
						glm::vec3(0.0f, 1.0f, 0.0f));
					// Far clip covers the orbit distance plus the scene
					// diameter, clamped sane.
					{
						const float fitted = dist + radius * 2.0f;
						m_sceneFarClip = std::clamp(fitted, 50.0f, 10000.0f);
					}
					std::cerr << "[Scene] camera framed: dist=" << dist
						<< " farClip=" << m_sceneFarClip << "\n";
				}
			}
		}

		// Authored punctual lights (KHR_lights_punctual -> pak): when the
		// scene carries them they replace the procedural demo set (sun
		// drives the environment, points/spots upload as the Forward+
		// set — see BuildCookedLightsFromWorld / UpdateForwardPlusLights).
		BuildCookedLightsFromWorld();

		// Named GI sidecars (pak Gi registry; one per kind for now).
		ResolveGiSidecars();

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
			for (uint32_t li = 0; li < app::GfxRenderer::LitSgVariantCount(); ++li)
				m_gfxRenderer.ConfigureLitFragSg(li, m_resources.fragLitSgModules[li]);
			// Main-view seed masked phase is reversed-Z: prefer the
			// conservative shadow_rev module (falls back to the normal-Z
			// shadow.frag module â€” perf hint, not correctness).
			m_gfxRenderer.ConfigureSeedFrag(!m_resources.shadowMaskedFragRevShader.empty()
				? m_resources.shadowMaskedFragRevShader
				: m_resources.shadowMaskedFragShader);
		// Shadow-atlas seed masked phase is normal-Z (clear 1.0,
		// LessEqual): the plain shadow.frag module.
		m_gfxRenderer.ConfigureShadowSeedFrag(m_resources.shadowMaskedFragShader);
		// Material depth-phase state (cull + alpha-tested) for the range-
		// based depth recorders (seed/remainder/shadow/filtered depth).
		// Resolved lazily at record time so it always sees the live World.
		m_gfxRenderer.ConfigureMaterialDepth(
			[this](uint32_t materialId) -> app::MaterialDepthState {
				if (!m_world)
					return {};
				const smvk::Material* mat = m_world->GetMaterial(materialId);
				if (!mat)
					return {};
				return { mat->pipeline.raster.cullMode,
					mat->shadingModel == smvk::ShadingModel::Masked };
			});
			// A2C filtered program for Masked ranges in 2x MSAA Main.
			if (!m_resources.fragA2CShader.empty())
				m_gfxRenderer.ConfigureA2CFrag(m_resources.fragA2CShader);
			// Wireframe twin of the filtered program (flat lines in P).
			if (!m_resources.fragWireShader.empty())
				m_gfxRenderer.ConfigureWireFrag(m_resources.fragWireShader);
			if (!m_gfxRenderer.Ensure(*m_context, *m_rendergraph)) {
				std::cerr << "[WorldRenderer] GfxFilter unavailable - direct draws\n";
				m_useGfxFilter = false;
			}
		else {
			m_gfxRenderer.RegisterWorldMeshes(*m_world);
			m_gfxRenderer.UploadStaticData(*m_context, *m_rendergraph);
			std::cerr << "[Scene] gfx clusters="
				<< smvk::gfx::ClusterCount(m_gfxRenderer.State()) << std::endl;
			if (smvk::gfx::ClusterCount(m_gfxRenderer.State()) == 0) {
				std::cerr << "[WorldRenderer] GfxFilter: no clusters - direct draws\n";
				m_useGfxFilter = false;
			}
		}
		}
		// Forward+ frame renderer (owns sizing config; references the
		// viewport builder for storage so main-pass sampling is unchanged).
		m_fpRenderer.Configure(&m_viewportResources.forwardPlus,
			MakeForwardPlusDesc(), m_resources.prepassMaskedProgram);
		// SSSR thin G-buffer writers (the prepass's G-buffer FS pair —
		// empty programs keep the legacy VS-only prepass).
		m_fpRenderer.ConfigureGbuffer(m_resources.gbufferProgram,
			m_resources.gbufferMaskedProgram);

		// Light cookies: baked .ktx2 files from the engine resources mount
		// (overridable by app content / paks through the overlay; falls
		// back to procedural generation when the files are missing).
		m_cookies.Ensure(*m_context, contentVfs);
		// SG-GI baked probe volume (tools/gi_baker output): replaces the
		// analytic sky IBL ambient with path-traced probes when present.
		// Missing file is the normal state (no bake yet) - non-fatal.
		LoadGiVolume(contentVfs);
#if 0 // Lightmaps deprecated
		// SG 2D lightmaps (cooked with PakCooker --lightmap): uploads the
		// atlas + chunk descriptors only (UVs/chunk ids live in the pak).
		// Missing file is the normal state (no cook yet) - non-fatal.
		LoadGiLightmaps(contentVfs);
#endif
		// SG regular 3D probe volumes (DYNAMIC objects): artist OBB
		// grids from tools/gi_baker --regular-out. Missing manifest is
		// the normal state (no bake yet) - non-fatal.
		LoadGiRegularVolumes(contentVfs);
		// Convex GI volumes (dense probe grids in authored convex shapes,
		// cooked by cubemap reprojection; GiBaker --cv-spec). Composited
		// over the octree world field by priority at runtime. Missing
		// manifest is the normal state (no bake yet) - non-fatal.
		LoadCvolumes(contentVfs);
		// Reflection cubemaps for specular IBL (cubemap_baker output):
		// replaces the analytic sky specular with captured environment.
		// Missing file is non-fatal (falls back to analytic sky).
		LoadCubemaps(contentVfs);
		// Single-source isolation modes degrade when their source did not
		// load (All/SkyIbl need nothing): keeps them from being silently
		// empty — the UI/env selector keeps working after.
		DegradeAmbientForMissingSources();
		// SMAA precomputed LUTs (official area/search tables, embedded in
		// the engine - see Engine/Rendergraph/smaa.h). Uploaded once here so
		// the staged uploads join the cookie flush below.
		if (!m_viewportResources.smaa.EnsureLuts(*m_context))
			std::cerr << "[WorldRenderer] SMAA LUT upload failed - SMAA unavailable\n";
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
		if (!m_viewportResources.directionalShadows.Ensure(*m_context, *m_rendergraph, MakeShadowDesc(m_cascadeResolution, m_cascadeLambda))) {
			std::cerr << "[WorldRenderer] Failed to create cascaded shadow atlas\n";
			return false;
		}
		// Spot-shadow atlas (fixed kMaxSpotShadows slots; selection caps usage).
		// Non-fatal: without it all spots simply run unshadowed.
		if (!m_viewportResources.spotShadows.Ensure(*m_context, *m_rendergraph, MakeSpotShadowDesc())) {
			std::cerr << "[WorldRenderer] Spot shadow atlas unavailable â€“ spots run unshadowed\n";
		}
	// EVSM moment atlases (RGBA32F 2xMSAA + async blur). Non-fatal: when
	// the device lacks RGBA32F render/blur/filter support (or shaders
	// fail), EvsmActive() stays false and every consumer keeps the PCF path.
	if (!m_context->SupportsEvsm()) {
		std::cerr << "[WorldRenderer] EVSM unsupported (no RGBA32F 2xMSAA moments) – PCF shadows\n";
	}
	if (!m_viewportResources.evsmDir.Ensure(*m_context, *m_rendergraph, MakeEvsmDirDesc(m_evsmPipeline, m_cascadeResolution))) {
		std::cerr << "[WorldRenderer] EVSM directional atlas unavailable – PCF fallback\n";
	}
	if (!m_viewportResources.evsmSpot.Ensure(*m_context, *m_rendergraph, MakeEvsmSpotDesc(m_evsmPipeline))) {
		std::cerr << "[WorldRenderer] EVSM spot atlas unavailable – PCF fallback\n";
	}
	if (EvsmActive()) {
		std::cerr << "[EVSM] moment atlases ready: RGBA32F 2xMSAA + async blur ("
				<< (m_evsmPipeline == smvk::EvsmPipelineMode::SameFrame ? "SameFrame" : "OneFrameLag")
				<< (m_context->HasDedicatedComputeQueue() ? ", true-async queue" : ", emulated (no dedicated queue)")
				<< ")\n";
		} else if (m_evsmEnabled) {
			std::cerr << "[EVSM] inactive (unsupported or atlas failure) â€“ PCF shadows\n";
		}
		if (!m_viewportResources.tonemap.Ensure(*m_context, MakeTonemapDesc())) {
			std::cerr << "[WorldRenderer] Failed to create tonemap program\n";
			return false;
		}
		// Histogram meter for auto-exposure (extent-independent buffer +
		// compute program). Non-fatal: without it the frame falls back to
		// manual exposure.
		if (!m_viewportResources.exposure.Ensure(*m_context, *m_rendergraph, MakeExposureDesc())) {
			std::cerr << "[WorldRenderer] Exposure meter unavailable â€“ manual exposure only\n";
		}
		// Skydome program (fullscreen-triangle sky.vert/sky.frag). Failure is
		// non-fatal: the Main pass simply skips the sky draw and keeps the
		// clear color.
		if (!m_skydome.Ensure(*m_context)) {
			std::cerr << "[WorldRenderer] Skydome program unavailable â€“ running without sky\n";
		}

		{
			auto vkExt = m_swapchain->GetExtent();
			const smvk::Extent2D swapExtent{ vkExt.width, vkExt.height };
			const smvk::Extent2D renderExtent = ScaledExtent(swapExtent, m_renderScale);
			const smvk::Extent2D uiExtent = ScaledExtent(swapExtent, m_uiScale);
			smvk::DepthDesc depthDesc{};
			depthDesc.format = m_swapchain->GetDepthFormat();
			if (!m_viewportResources.renderDepth.Ensure(*m_context, *m_rendergraph, renderExtent, depthDesc)) {
				std::cerr << "[WorldRenderer] Failed to create render depth target\n";
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
				std::cerr << "[WorldRenderer] Failed to create HZB occluder depth target\n";
				return false;
			}
			if (!m_viewportResources.forwardPlus.Ensure(*m_context, *m_rendergraph,
				renderExtent, MakeForwardPlusDesc(), m_viewportResources.renderDepth.view)) {
				std::cerr << "[WorldRenderer] Failed to create Forward+ tile buffers\n";
				return false;
			}
			if (!m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, renderExtent)) {
				std::cerr << "[WorldRenderer] Failed to create HDR target\n";
				return false;
			}
			{
				smvk::MsaaDesc msaaDesc{};
				msaaDesc.depthFormat = m_swapchain->GetDepthFormat();
				if (!m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, renderExtent, msaaDesc)) {
					std::cerr << "[WorldRenderer] MSAA target unavailable â€“ falling back to 1x\n";
				}
			}
		if (!m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc())) {
				std::cerr << "[WorldRenderer] Bloom chain unavailable (HDR "
				<< renderExtent.width << "x" << renderExtent.height << ")\n";
		}
		{
			smvk::DofDesc dofDesc = MakeDofDesc();
			dofDesc.hqBokeh = m_dofHqBokeh;
			if (!m_viewportResources.dof.Ensure(*m_context, *m_rendergraph, renderExtent, dofDesc)) {
				std::cerr << "[WorldRenderer] DOF targets unavailable (HDR "
					<< renderExtent.width << "x" << renderExtent.height << ")\n";
			}
		}
		if (!m_viewportResources.ssr.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsrDesc())) {
			std::cerr << "[WorldRenderer] SSR target unavailable (HDR "
				<< renderExtent.width << "x" << renderExtent.height << ")\n";
		}
		if (!m_viewportResources.volumetric.Ensure(*m_context, *m_rendergraph, renderExtent, MakeVolumetricDesc())) {
			std::cerr << "[WorldRenderer] Volumetric target unavailable (HDR "
				<< renderExtent.width << "x" << renderExtent.height << ")\n";
		}
		smvk::UiDesc uiDesc{};
		uiDesc.format = LdrUnormSibling();
		if (!m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, uiDesc)) {
			std::cerr << "[WorldRenderer] UI target unavailable â€“ UI disabled\n";
		}
			if (!m_viewportResources.uiComposite.Ensure(*m_context)) {
				std::cerr << "[WorldRenderer] UI composite program unavailable\n";
			}
		}

		// ImGui overlay â€“ init after swapchain/rendergraph are ready.
		// Uses dynamic rendering; no external RenderPass needed.
		// HOSTED PREVIEW: never create the overlay — the host (editor)
		// owns the process's single ImGui context; a second context
		// here would corrupt the host's UI.
		if (!IsHosted()) {
		m_imgui = smvk::mem::MakeProcess<smvk::ImGuiOverlay>();
		smvk::ImGuiOverlayDesc imguiDesc{};
		imguiDesc.fontPath = UI_FONT_PATH;
		imguiDesc.fontBaseSize = UI_FONT_BASE_SIZE;
		imguiDesc.colorFormat = LdrUnormSibling(); // must match the UI target above
		if (!m_imgui->Init(*m_context, *m_swapchain, window, imguiDesc)) {
			std::cerr << "[WorldRenderer] Failed to init ImGuiOverlay â€“ continuing without UI\n";
			m_imgui.reset();
		} else {
			m_uiDpiScale = m_imgui->AppliedDpiScale(); // auto = monitor content scale
			SetUIMode(false); // start in FPS (DISABLED), I toggles to UI (NORMAL)
			// Feed every SDL event to the ImGui SDL3 backend (App::PollEvents).
			imguiEventHook_ = &smvk::ImGuiOverlay::ProcessEvent;
		}
		} else {
			std::cerr << "[WorldRenderer] hosted preview: UI overlay "
				"skipped (host owns ImGui)\n";
		}

		return true;
};

void WorldRenderer::Shutdown() {
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
	void WorldRenderer::LoadGiVolume(smvk::OverlayVfs& vfs) {
		m_giLoaded = false;
		m_giVolume = smvk::gi::SgGiVolume{};
		m_giProbesBuffer = {};
		m_giGridBuffer = {};
		m_giCellsBuffer = {};
		m_giIndicesBuffer = {};
		m_giProbesAddress = m_giGridAddress = m_giCellsAddress = m_giIndicesAddress = 0;

		const std::string stem = m_giProbeName.empty()
			? m_sceneId : m_giProbeName;
		const std::string vfsPath = std::string("gi/") + stem + ".sggi";
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
	// Reflection cubemaps for specular IBL (tools/cubemap_baker output).
	// Loads content/textures/cubemaps/<scene>.ktx2 through the app VFS.
	// A missing file is non-fatal (falls back to analytic sky).
	// ---------------------------------------------------------------------------
	void WorldRenderer::LoadCubemaps(smvk::OverlayVfs& vfs) {
		m_cubemaps.entries.clear();
		m_cubemapProbes.clear();

		// Clamp sampler for TextureCube sampling (cube seams must not wrap;
		// linear min/mag + linear mips for the GGX-prefiltered chains).
		// Created once — GetOrCreateSampler dedupes, so this is cheap after
		// the first scene load. The shader reads it via FrameUniforms
		// (cubemapSampler), never samplers[0] (Repeat would bleed seams).
		m_cubemapSampler = m_context->GetOrCreateSampler(smvk::CubemapSampler());

		// Try a pak-registered bake first (the cubemanifest GUID from the
		// pak's Gi registry — stable across pak/scene renames), then the
		// scene stem, then the "sky" fallback.
		std::vector<std::string> cubemapBases;
		if (!m_giCubemapName.empty())
			cubemapBases.push_back(m_giCubemapName);
		cubemapBases.push_back(m_sceneId);
		cubemapBases.push_back("sky");

		bool fileBakeFound = false;
		// Pinned manifest rows ('pin', cook --cubemap-extra-pos) that loaded:
		// they sort before every auto-placed probe (ties keep the first
		// entry, so an artist-placed capture — e.g. at the chrome ball —
		// wins against overlapping coverage probes).
		size_t pinnedLoaded = 0;
		for (const auto& base : cubemapBases) {
			if (base.empty())
				continue;

			// 1. Grid manifest: the cook writes
			//    textures/cubemaps/<base>.cubemanifest when one capture
			//    bakes N > 1 probes (<base>_0.ktx2, ...). Positions ride
			//    the manifest in cook/world meters (trace BVH space =
			//    World::ComputeSceneBounds space — both apply the scene
			//    global transforms).
			if (auto manifestBytes = vfs.TryRead(smvk::CubemapManifestVfsPath(base))) {
				std::string mErr;
				std::vector<smvk::CubemapManifestEntry> rows;
				const std::string_view text(
					reinterpret_cast<const char*>(manifestBytes->data()),
					manifestBytes->size());
				if (!smvk::CubemapManifestParse(text, rows, mErr)) {
					std::cerr << "[Cubemap] manifest rejected '"
						<< smvk::CubemapManifestVfsPath(base) << "': "
						<< mErr << "\n";
					continue;
				}
				// Pinned rows first (stable: file order within groups).
				std::stable_partition(rows.begin(), rows.end(),
					[](const smvk::CubemapManifestEntry& e) { return e.pinned; });
				size_t loaded = 0;
				pinnedLoaded = 0;
				m_cubemapProbes.reserve(rows.size());
				for (const auto& row : rows) {
					// Derive the library name from the KTX basename so the
					// entry key stays human-readable (e.g. "sponza_3").
					std::string entryName = row.vfsPath;
					if (auto slash = entryName.find_last_of('/'); slash != std::string::npos)
						entryName = entryName.substr(slash + 1);
					if (auto dot = entryName.find_last_of('.'); dot != std::string::npos)
						entryName = entryName.substr(0, dot);
					if (!vfs.TryRead(row.vfsPath)) {
						std::cerr << "[Cubemap] manifest row missing: "
							<< row.vfsPath << " - skipped\n";
						continue;
					}
					if (!m_cubemaps.Load(*m_context, vfs, entryName, row.vfsPath)) {
						std::cerr << "[Cubemap] manifest row load failed: "
							<< row.vfsPath << " - skipped\n";
						continue;
					}
					const auto* entry = m_cubemaps.Find(entryName);
					if (!entry || entry->bindless < 0)
						continue;
					if (!entry->baked) {
						// Decode failed and Load fell back to the neon
						// procedural pattern: never place that as a grid
						// probe (recook the row instead).
						std::cerr << "[Cubemap] manifest row not a cubemap: "
							<< row.vfsPath << " - skipped\n";
						m_cubemaps.entries.erase(entryName);
						continue;
					}
					CubemapProbeGpu p{};
					p.posInfluence = glm::vec4(row.position, row.influenceRadius);
					p.fadeCubemap = glm::vec4(row.fadeRadius,
						static_cast<float>(entry->bindless),
						static_cast<float>(entry->mipLevels), 0.0f);
					m_cubemapProbes.push_back(p);
					if (row.pinned)
						++pinnedLoaded;
					++loaded;
				}
				if (loaded == 0) {
					std::cerr << "[Cubemap] manifest '" << base
						<< "' loaded 0 probes - trying next candidate\n";
					m_cubemapProbes.clear();
					pinnedLoaded = 0;
					continue;
				}
				std::cerr << "[Cubemap] loaded manifest '" << base << "': "
					<< loaded << "/" << rows.size() << " probes"
					<< (pinnedLoaded > 0
						? " (" + std::to_string(pinnedLoaded) + " pinned first)"
						: "") << "\n";
				fileBakeFound = true;
				break;
			}

			// 2. Single capture: textures/cubemaps/<base>.ktx2. Probe rows
			//    are built after the loop (global probe at the cached scene
			//    center) so manifest + single paths share one upload.
			if (vfs.TryRead(smvk::CubemapVfsPath(base))) {
				if (m_cubemaps.Load(*m_context, vfs, base)) {
					if (const auto* entry = m_cubemaps.Find(base);
						entry && entry->bindless >= 0) {
						std::cerr << "[Cubemap] loaded '" << base << "' (bindless=" << entry->bindless
							<< ", size=" << entry->size << "x" << entry->size
							<< ", mips=" << entry->mipLevels
							<< (entry->baked ? ", baked" : ", PROCEDURAL fallback") << ")\n";
						fileBakeFound = true;
						break;
					}
				}
			}

			// 3. Legacy grid without a manifest (old paks): <base>_N.ktx2.
			//    Positions are unknown, so only the first capture becomes
			//    the global probe; the rest still load (keeps their
			//    bindless slots valid) but warn to recook for placement.
			bool legacyAny = false;
			for (uint32_t n = 0; n < 32; ++n) {
				const std::string name = base + "_" + std::to_string(n);
				if (!vfs.TryRead(smvk::CubemapVfsPath(name)))
					break;
				if (m_cubemaps.Load(*m_context, vfs, name) && m_cubemaps.Find(name)) {
					legacyAny = true;
				}
			}
			if (legacyAny) {
				std::cerr << "[Cubemap] legacy grid '" << base
					<< "_N' without a manifest: using '" << base
					<< "_0' as the global probe - recook with --cubemap-grid for placement\n";
				fileBakeFound = true;
				break;
			}
		}

		if (!fileBakeFound) {
			std::cerr << "[Cubemap] no baked cubemap for '"
				<< (m_sceneId.empty() ? "?" : m_sceneId)
				<< "' or 'sky' - specular IBL uses the analytic sky"
				<< " (bake with tools/cubemap_baker/CubemapBaker or PakCooker --cubemap on)\n";
			if (!m_context->SupportsEvsm()) {
				std::cerr << "[Cubemap] note: RGBA32F cubemap filtering is untested on this"
					<< " device (EVSM unsupported) - baked cubemaps may sample unfiltered\n";
			}
		}

		// Build the cached probe rows ONCE per scene load (never per frame):
		//  - manifest path already filled m_cubemapProbes above;
		//  - single/legacy path centers one global probe on the scene bounds.
		if (fileBakeFound && m_cubemapProbes.empty() && !m_cubemaps.entries.empty()) {
			const ::Aabb bounds = m_world ? m_world->ComputeSceneBounds()
				: ::Aabb::empty();
			glm::vec3 center{ 0.0f };
			float radius = 10.0f;
			if (bounds.mn.x <= bounds.mx.x && bounds.mn.y <= bounds.mx.y && bounds.mn.z <= bounds.mx.z) {
				center = (bounds.mn + bounds.mx) * 0.5f;
				radius = glm::length(bounds.mx - bounds.mn) * 0.5f;
				radius = std::max(radius, 1.0f);
			}
			// First loaded entry wins (insertion order: base, then base_0...).
			for (const auto& [name, entry] : m_cubemaps.entries) {
				if (entry.bindless < 0)
					continue;
				CubemapProbeGpu p{};
				p.posInfluence = glm::vec4(center, radius * 2.0f);
				p.fadeCubemap = glm::vec4(radius * 0.5f,
					static_cast<float>(entry.bindless),
					static_cast<float>(entry.mipLevels), 0.0f);
				m_cubemapProbes.push_back(p);
				std::cerr << "[Cubemap] global probe '" << name
					<< "' at (" << center.x << "," << center.y << "," << center.z
					<< ") r=" << radius << (entry.baked ? " (baked)" : " (PROCEDURAL test pattern)") << "\n";
				break;
			}
		}

		// Order: pinned rows first (artist-placed captures win weight
		// ties — ties keep the first entry), then widest influence within
		// each block (the lit-shader probe walk breaks at the first
		// full-weight entry, and tile overflow keeps the earliest upload
		// slots). Probe data rides the FP light buffer as type-2 entries,
		// appended post-selection in UpdateForwardPlusLights.
		const size_t pinSplit = std::min(pinnedLoaded, m_cubemapProbes.size());
		std::sort(m_cubemapProbes.begin(), m_cubemapProbes.begin() + pinSplit,
			[](const CubemapProbeGpu& a, const CubemapProbeGpu& b) {
				return a.posInfluence.w > b.posInfluence.w;
			});
		std::sort(m_cubemapProbes.begin() + pinSplit, m_cubemapProbes.end(),
			[](const CubemapProbeGpu& a, const CubemapProbeGpu& b) {
				return a.posInfluence.w > b.posInfluence.w;
			});
	}

	// ---------------------------------------------------------------------------
	// SG 2D lightmaps (STATIC geometry; cooked with PakCooker --lightmap).
	// Loads the registry lightmap (gi/<name>.sglm, else gi/<scene>.sglm)
	// and uploads ONLY the 9 RGBA16F atlas
	// layers. The chart uvs + per-chart atlas transforms + per-mesh lobe
	// counts are COOKED into the pak (texCoord2/chartId +
	// StaticMesh::lmChartXforms, StaticMesh::lmChunk riding the draw
	// ring) — no chunk buffer exists at runtime. Missing file =
	// "not baked yet" - non-fatal.
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

#if 0 // Lightmaps deprecated
	void WorldRenderer::LoadGiLightmaps(smvk::OverlayVfs& vfs) {
		// Cook-owned lightmaps: the pak carries the per-vertex chart uvs
		// + chart ids (texCoord2/chartId), each mesh's chart transforms
		// (scene table, unpacked in the VS) + lobe count (riding the
		// draw ring); this loader only uploads the atlas textures. NO
		// geometry work, NO buffers beyond the textures happen here.
		m_lmLoaded = false;
		m_lmLayerKind = 0;
		m_lmAtlas = smvk::gi::SgLightmapAtlas{};
		for (auto& layer : m_lmLayers)
			layer = {};
		for (uint32_t& slot : m_lmLayerSlots)
			slot = UINT32_MAX;

		const std::string stem = m_giLightmapName.empty()
			? m_sceneId : m_giLightmapName;
		const std::string vfsPath = std::string("gi/") + stem + ".sglm";
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

		// ---- Atlas textures ----
		// Uncompressed (sglm v1/v2): 9 RGBA16F stream layers, kind 0.
		// BC6H (v3): the cooked blocks upload verbatim as one BC6H_UFloat
		// texture per lobe (kind 1; slots past the scene's lobe count
		// stay invalid). Hardware filtering is identical in both.
		const bool bc6h = m_lmAtlas.compression
			!= smvk::gi::SgLightmapCompression::Uncompressed;
		uint32_t lmLayerKind = 0;
		if (bc6h) {
			const size_t blockBytes = size_t(m_lmAtlas.width / 4)
				* (m_lmAtlas.height / 4) * 16;
			for (uint32_t l = 0; l < m_lmAtlas.bc6hLobes; ++l) {
				smvk::ImageCreateInfo ci{};
				ci.extent = { m_lmAtlas.width, m_lmAtlas.height, 1 };
				ci.format = smvk::Format::BC6H_UFloat;
				ci.usage = smvk::ImageUsage::Sampled
					| smvk::ImageUsage::TransferDst;
				ci.mipLevels = 1;
				ci.arrayLayers = 1;
				ci.name = "sglm atlas bc6h lobe";
				auto tex = m_context->CreateTexture(ci,
					m_lmAtlas.bc6hBlocks.data() + size_t(l) * blockBytes);
				if (tex.empty()) {
					std::cerr << "[LM] bc6h lobe texture " << l
						<< " creation failed\n";
					for (uint32_t& slot : m_lmLayerSlots)
						slot = UINT32_MAX;
					return;
				}
				m_lmLayers[l] = std::move(tex);
				m_lmLayerSlots[l] =
					m_context->GetTextureBindlessIndex(m_lmLayers[l]);
			}
			lmLayerKind = 1;
		} else {
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
		}
		m_lmLayerKind = lmLayerKind;
		m_lmLoaded = true;
		std::cerr << "[LM] loaded '" << vfsPath << "': chunks="
			<< m_lmAtlas.chunks.size() << " atlas=" << m_lmAtlas.width << "x"
			<< m_lmAtlas.height
			<< " gpu=" << (m_lmAtlas.GpuBytes() / 1024) << " KiB ("
			<< (bc6h ? "bc6h" : "rgba16f") << ")\n";
	}
#endif
	// ---------------------------------------------------------------------------
	// SG regular 3D probe volumes (DYNAMIC objects; tools/gi_baker
	// --regular-out). Loads content/gi/<scene>.regmanifest: one .sgrv vfs
	// path per line, up to SG_REG_MAX_VOLUMES (8). Each volume uploads 7
	// RGBA16F 3D textures + one OBB descriptor; the shader hardware-
	// trilinearly filters between neighboring probes. Missing manifest =
	// "not baked yet" - non-fatal.
	// ---------------------------------------------------------------------------
	void WorldRenderer::LoadGiRegularVolumes(smvk::OverlayVfs& vfs) {
		m_regLoaded = false;
		m_regEnabled = false;
		m_regVolumes.clear();
		m_regObbsBuffer = {};
		m_regObbsAddress = 0;

		const std::string manifestPath = !m_giRegName.empty()
			? std::string("gi/") + m_giRegName + ".regmanifest"
			: std::string("gi/") + m_sceneId + ".regmanifest";
		std::vector<uint8_t> manifestBytes;
		try {
			manifestBytes = vfs.ReadBinary(manifestPath);
		} catch (...) {
			std::cerr << "[REG] no volume manifest '" << manifestPath
				<< "' (bake: GiBaker --regular-out)\n";
			return;
		}
		// Line-separated .sgrv vfs paths. Bake-asset rows carry the
		// payload guid first ("<guid> <path>  # <note>"); legacy and
		// hand-edited rows are bare paths — SplitManifestRow keeps both.
		std::vector<std::string> lines;
		{
			std::string cur;
			for (char c : manifestBytes) {
				if (c == '\n' || c == '\r') {
					const std::string path = smvk::bake::SplitManifestRow(cur);
					if (!path.empty())
						lines.push_back(path);
					cur.clear();
				} else {
					cur.push_back(c);
				}
			}
			const std::string path = smvk::bake::SplitManifestRow(cur);
			if (!path.empty())
				lines.push_back(path);
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

	// ---------------------------------------------------------------------------
	// Convex GI volumes (Engine/GI/cubemap_volume.h; GiBaker --cv-spec).
	// Loads content/gi/<scene>.cvmanifest: one .cvol vfs path per line, up
	// to SG_CV_MAX_VOLUMES (8). Each volume uploads 7 RGBA16F 3D textures
	// (dense probe grid, hardware trilinear) + one 560 B descriptor with
	// the convex plane set INLINE (per-plane fades) + priority. Volumes are
	// uploaded priority-sorted (LOW first) so the shader's ordered
	// compositing is a plain loop — later volumes override earlier ones
	// (FOX override-volume rule). Missing manifest = "not baked yet" -
	// non-fatal.
	// ---------------------------------------------------------------------------
	struct CvolPlaneGpu {
		float normal_d[4]; // xyz plane normal, w = d (inside n.P + d <= 0)
		float fades[4];    // x insideFade, y outsideFade, zw pad
	};
	struct CvolVolumeGpu {
		float gridMin_spacing[4]; // xyz lattice origin, w spacing
		uint32_t dims_priority[4]; // xyz dims, w priority
		uint32_t planeCount_pad[4]; // x plane count
		CvolPlaneGpu planes[16]; // SG_CV_MAX_PLANES (sg_lightmap_types.hlsli)
	};
	static_assert(sizeof(CvolVolumeGpu) == 560);

	void WorldRenderer::LoadCvolumes(smvk::OverlayVfs& vfs) {
		m_cvolLoaded = false;
		m_cvolEnabled = false;
		m_cvolVolumes.clear();
		m_cvolVolumesBuffer = {};
		m_cvolVolumesAddress = 0;

		const std::string manifestPath = !m_giCvolName.empty()
			? std::string("gi/") + m_giCvolName + ".cvmanifest"
			: std::string("gi/") + m_sceneId + ".cvmanifest";
		std::vector<uint8_t> manifestBytes;
		try {
			manifestBytes = vfs.ReadBinary(manifestPath);
		} catch (...) {
			std::cerr << "[CV] no convex-volume manifest '" << manifestPath
				<< "' (bake: GiBaker --cv-spec <file>)\n";
			return;
		}
		// Line-separated .cvol vfs paths (bake-asset rows carry the
		// payload guid first — see smvk::bake::SplitManifestRow; '#' comments
		// and blanks skipped — GiBaker writes a header comment).
		std::vector<std::string> lines;
		{
			std::string cur;
			for (char c : manifestBytes) {
				if (c == '\n' || c == '\r') {
					const std::string path = smvk::bake::SplitManifestRow(cur);
					if (!path.empty())
						lines.push_back(path);
					cur.clear();
				} else {
					cur.push_back(c);
				}
			}
			const std::string path = smvk::bake::SplitManifestRow(cur);
			if (!path.empty())
				lines.push_back(path);
		}
		constexpr uint32_t kMaxVolumes = 8; // SG_CV_MAX_VOLUMES
		for (const std::string& line : lines) {
			if (m_cvolVolumes.size() >= kMaxVolumes) {
				std::cerr << "[CV] manifest lists more than " << kMaxVolumes
					<< " volumes - extra entries ignored\n";
				break;
			}
			std::vector<uint8_t> volBytes;
			try {
				volBytes = vfs.ReadBinary(line);
			} catch (...) {
				std::cerr << "[CV] missing volume '" << line << "' - skipped\n";
				continue;
			}
			CvolVolume cv{};
			std::string verr;
			if (!cv.volume.ReadFromMemory(volBytes, verr)) {
				std::cerr << "[CV] volume rejected: " << verr << "\n";
				continue;
			}
			// 7 RGBA16F 3D textures (bindless; volumeTextures mirror).
			std::vector<uint16_t> layers[7];
			cv.volume.PackGpuLayers(layers);
			bool ok = true;
			for (uint32_t t = 0; t < 7; ++t) {
				smvk::ImageCreateInfo ci{};
				ci.type = smvk::ImageType::Image3D;
				ci.extent = { cv.volume.dims[0], cv.volume.dims[1],
					cv.volume.dims[2] };
				ci.format = smvk::Format::RGBA16_Float;
				ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
				ci.mipLevels = 1;
				ci.arrayLayers = 1;
				ci.name = "cvol layer";
				auto tex = m_context->CreateTexture(ci, layers[t].data());
				if (tex.empty()) {
					std::cerr << "[CV] layer " << t << " creation failed\n";
					ok = false;
					break;
				}
				cv.layers[t] = std::move(tex);
				cv.layerSlots[t] = m_context->GetTextureBindlessIndex(cv.layers[t]);
			}
			if (!ok)
				continue;
			// Coefficients are now resident on the GPU; drop the CPU copy.
			cv.volume.coeffs.clear();
			cv.volume.coeffs.shrink_to_fit();
			m_cvolVolumes.push_back(std::move(cv));
		}
		if (m_cvolVolumes.empty()) {
			std::cerr << "[CV] no volumes loaded\n";
			return;
		}

		// Priority sort (stable, LOW first): the shader composites in
		// upload order, so later entries win — the FOX override rule.
		std::stable_sort(m_cvolVolumes.begin(), m_cvolVolumes.end(),
			[](const CvolVolume& a, const CvolVolume& b) {
				return a.volume.def.priority < b.volume.def.priority;
			});

		// Descriptor buffer (float4/uint4-padded layout, see CvolVolumeGpu).
		std::vector<CvolVolumeGpu> descs(m_cvolVolumes.size());
		for (size_t v = 0; v < m_cvolVolumes.size(); ++v) {
			const smvk::gi::CvVolume& vol = m_cvolVolumes[v].volume;
			CvolVolumeGpu& g = descs[v];
			g.gridMin_spacing[0] = vol.gridMin.x;
			g.gridMin_spacing[1] = vol.gridMin.y;
			g.gridMin_spacing[2] = vol.gridMin.z;
			g.gridMin_spacing[3] = vol.spacing;
			g.dims_priority[0] = vol.dims[0];
			g.dims_priority[1] = vol.dims[1];
			g.dims_priority[2] = vol.dims[2];
			g.dims_priority[3] = vol.def.priority;
			const uint32_t planeCount = std::min<uint32_t>(
				uint32_t(vol.def.planes.size()), 16u);
			g.planeCount_pad[0] = planeCount;
			for (uint32_t p = 0; p < planeCount; ++p) {
				const smvk::gi::CvPlane& pl = vol.def.planes[p];
				g.planes[p].normal_d[0] = pl.normal.x;
				g.planes[p].normal_d[1] = pl.normal.y;
				g.planes[p].normal_d[2] = pl.normal.z;
				g.planes[p].normal_d[3] = pl.d;
				g.planes[p].fades[0] = pl.insideFade;
				g.planes[p].fades[1] = pl.outsideFade;
				g.planes[p].fades[2] = 0.0f;
				g.planes[p].fades[3] = 0.0f;
			}
		}
		constexpr smvk::PipelineStage kCvStages =
			smvk::PipelineStage(smvk::PipelineStageBit_Fragment);
		m_cvolVolumesBuffer = m_context->CreateBuffer(
			descs.size() * sizeof(CvolVolumeGpu), smvk::BufferType_Storage,
			"cvol volumes");
		m_context->UploadDataToBuffer(m_cvolVolumesBuffer, descs.data(),
			descs.size() * sizeof(CvolVolumeGpu), kCvStages,
			smvk::AccessFlags::ShaderRead);
		m_cvolVolumesAddress = m_context->GetBufferDeviceAddress(m_cvolVolumesBuffer);

		m_cvolLoaded = true;
		m_cvolEnabled = true; // on by default when baked; UI can toggle
		std::cerr << "[CV] loaded " << m_cvolVolumes.size()
			<< " convex volume(s) from '" << manifestPath << "'\n";
		for (const auto& cv : m_cvolVolumes) {
			const auto& d = cv.volume.def;
			std::cerr << "  dims=" << cv.volume.dims[0] << "x" << cv.volume.dims[1]
				<< "x" << cv.volume.dims[2] << " spacing=" << cv.volume.spacing
				<< " planes=" << d.planes.size() << " priority=" << d.priority;
			if (!d.name.empty())
				std::cerr << " name='" << d.name << "'";
			std::cerr << "\n";
		}
	}

void WorldRenderer::DegradeAmbientForMissingSources() {
#if 0 // Lightmaps deprecated
	// Single-source isolation modes whose source did not load fall
	// back to another baked source (or the sky IBL) instead of being
	// silently empty. All/SkyIbl/Off never need this.
	if (m_ambientMode == AmbientMode::Lightmaps && !m_lmLoaded) {
		m_ambientMode = (m_giLoaded || m_regLoaded || m_cvolLoaded)
			? AmbientMode::Probes : AmbientMode::SkyIbl;
		std::cerr << "[Scene] no lightmaps - ambient degraded to "
			<< AmbientModeName(m_ambientMode) << "\n";
	} else if (m_ambientMode == AmbientMode::Probes
		&& !m_giLoaded && !m_regLoaded && !m_cvolLoaded) {
		m_ambientMode = m_lmLoaded
			? AmbientMode::Lightmaps : AmbientMode::SkyIbl;
		std::cerr << "[Scene] no probes - ambient degraded to "
			<< AmbientModeName(m_ambientMode) << "\n";
	}
#endif
}

	FrameUniforms WorldRenderer::UpdateFrameUniforms(uint32_t currentFrame, smvk::Extent2D renderExtent, smvk::Extent2D swapExtent, const smvk::EnvironmentSettings& env) {

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
		data.farPlane = m_sceneFarClip;
		// Linear exposure multiplier for the tonemapper (ACES).
		// Sun is 5000 lux (see UpdateLights); 0.001 puts a sun-facing
		// mid-gray (albedo 0.18, NÂ·L=1) at HDR â‰ˆ 0.29 pre-ACES â†’ no clipping.
		// NOTE: FrameUniforms.exposure is currently unread by shaders; the
		// tonemap push constant is the live knob. Kept in sync with the
		// effective exposure (auto-adapted or manual) for future readers.
		data.exposure = FrameExposure();
		// AO consumption (lighting-pass ambient; see ao.hlsli +
		// default_lit.frag.hlsl): the SsaoCompose/GtaoCompose/SaoCompose pass already
		// upscaled the AO to full res with depth authority, so the fragment
		// only .Loads it. Mirrors the AO pass gate below so the shader flag
		// and the emitted passes always agree.
		const bool aoWanted = m_passes.ssao && m_aoMode != AoMode::None
			&& m_ssaoIntensity > 0.0f;
		const bool ssaoReady = m_viewportResources.ssao.Valid();
		const bool gtaoReady = m_viewportResources.gtao.Valid();
		const bool saoReady = m_viewportResources.sao.Valid();
		const bool aoOn = aoWanted && (m_aoMode == AoMode::Gtao ? gtaoReady
			: m_aoMode == AoMode::Sao ? saoReady : ssaoReady);
		data.aoTextureIndex = aoOn
			? (m_aoMode == AoMode::Gtao ? m_viewportResources.gtao.OutputBindless()
				: m_aoMode == AoMode::Sao ? m_viewportResources.sao.OutputBindless()
			           : m_viewportResources.ssao.OutputBindless())
			: UINT32_MAX;
		data.aoIntensity = aoOn ? m_ssaoIntensity : 0.0f;
		// Configurable skydome IBL + fog (single source for sky/sun/fog,
		// built from the world's scene env by the caller).
		data.env = env.ToFrameData();
		// Ambient sources (N cycles All -> Lightmaps -> Probes -> SkyIbl
		// -> Off): per-source isolation — the sky IBL flag enables the
		// analytic sky as the ambient FALLBACK (pixels no baked source
		// covers; the shader composes it after the source branches, so
		// covered pixels take the baked field alone). Direct light /
		// sky dome / shadows are unaffected.
		data.env.fogEndAndFlags.w = IblWanted() ? 1.0f : 0.0f;

		// Reflection cubemap for specular IBL (global fallback) + knobs.
		// Probe data rides the FP light buffer as type-2 entries (see
		// UpdateForwardPlusLights); this frame only publishes the knobs +
		// the tile-list freshness bit. Disabled UI (or nothing loaded)
		// publishes "none" so shaders take the analytic sky path.
		data.cubemapIndex = UINT32_MAX;
		data.cubemapSampler = m_cubemapSampler;
		data.cubemapIntensity = m_cubemapEnabled ? m_cubemapIntensity : 0.0f;
		data.cubemapDebug = static_cast<uint32_t>(std::clamp(m_cubemapDebug, 0, 2));
		data.cubemapLodBias = m_cubemapLodBias;
		data.probeTileFresh = ProbeTileListsWanted() ? 1u : 0u;
		data.probeEntries = ProbeEntryCount();
		data.probePad_ = 0;
		if (m_cubemapEnabled && !m_cubemaps.entries.empty()) {
			// Global fallback = first loaded entry (matches the probe
			// cache's global-probe choice in LoadCubemaps).
			for (const auto& [name, entry] : m_cubemaps.entries) {
				if (entry.bindless >= 0) {
					data.cubemapIndex = static_cast<uint32_t>(entry.bindless);
					break;
				}
			}
		}

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
#if 0 // Lightmaps deprecated
		// SG 2D lightmaps (static draws; the atlas-space uv + per-draw lobe
		// count ride the pak's vertex data / draw ring). Static/dynamic
		// separation: lm/reg ride the same giIntensity scale as the probe
		// volume so the N ambient-mode UI stays meaningful.
		if (LmActive()) {
			for (uint32_t t = 0; t < 12; ++t)
				data.lmLayers[t] = m_lmLayerSlots[t];
			data.lmLayerKind = m_lmLayerKind;
			data.lmEnabled = 1u;
		} else {
			for (uint32_t t = 0; t < 12; ++t)
				data.lmLayers[t] = UINT32_MAX;
			data.lmLayerKind = 0u;
			data.lmEnabled = 0u;
		}
		data.lmPad_[0] = 0;
		data.lmPad_[1] = 0;
#endif
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
		// Convex GI volumes (probes-only pivot's confined-space path).
		// CvolActive() gates the upload (loaded + UI toggle + probes
		// ambient mode); the volumes ride the same giIntensity scale as
		// the octree. Buffers/slots publish whenever loaded so the
		// shader flag and GPU contents can never disagree.
		if (m_cvolLoaded && m_cvolEnabled) {
			data.cvolVolumes = m_cvolVolumesAddress;
			const uint32_t n = std::min<uint32_t>(
				uint32_t(m_cvolVolumes.size()), 8u);
			for (uint32_t v = 0; v < n; ++v)
				for (uint32_t t = 0; t < 7; ++t)
					data.cvolLayers[v * 7u + t] = m_cvolVolumes[v].layerSlots[t];
			data.cvolCount = n;
			data.cvolEnabled = ProbesWanted() ? 1u : 0u;
		} else {
			data.cvolVolumes = 0;
			for (uint32_t& slot : data.cvolLayers)
				slot = UINT32_MAX;
			data.cvolCount = 0u;
			data.cvolEnabled = 0u;
		}
		data.cvolPad_[0] = 0;
		data.cvolPad_[1] = 0;
		data.cvolPad_[2] = 0;
		data.cvolPad_[3] = 0;
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

		// This view's FramePSHot slab BDA (the FS reaches the hot struct
		// through FrameUniforms — pc.frameHot is gone, see
		// push_constants.h): slab 0, same math as the hot upload below.
		data.frameHot = m_context->GetBufferDeviceAddress(m_resources.frameHot)
			+ currentFrame * sizeof(FramePSHot);

		m_context->UploadDataToBuffer(
			m_resources.frameUniforms,
			&data,
			sizeof(FrameUniforms)/* * 1*/,
			kFrameUploadStages,
			smvk::AccessFlags::ShaderRead,
			0,
			currentFrame * sizeof(FrameUniforms)
		);

		// Upload FramePSHot — tiny hot struct for pixel shader register pressure reduction.
		FramePSHot hot{};
		hot.cameraPos = data.cameraPos;
		hot.time = data.time;
		hot.viewportSize = data.viewportSize;
		hot.invViewportSize = data.invViewportSize;
		hot.nearPlane = data.nearPlane;
		hot.farPlane = data.farPlane;
		hot.exposure = data.exposure;
		hot.aoTextureIndex = data.aoTextureIndex;
		hot.aoIntensity = data.aoIntensity;
		hot.evsmMode = data.evsmMode;
		hot.dsEnabled = data.dsEnabled;
		hot.dsTileCountX = data.dsTileCountX;
		hot.fogEndAndFlags_w = data.env.fogEndAndFlags.w;
		hot.giIntensity = data.giIntensity;
		hot.giDebug = data.giDebug;
		hot.cvolEnabled = data.cvolEnabled;
		hot.pad_[0] = 0;
		hot.pad_[1] = 0;

		m_context->UploadDataToBuffer(
			m_resources.frameHot,
			&hot,
			sizeof(FramePSHot),
			kFrameUploadStages,
			smvk::AccessFlags::ShaderRead,
			0,
			currentFrame * sizeof(FramePSHot)
		);

		return data;

	}

	smvk::GpuDirectionalLightDescription WorldRenderer::UpdateLights(uint32_t currentFrame, const smvk::EnvironmentSettings& env)
	{
		// Directional sun driven by the frame environment (single source
		// of truth for the sky sun disc, IBL sun tint, and
		// shadow-casting light — built from the world's scene env by
		// the caller; the renderer owns no environment of its own).
		// lightDir points FROM the sun toward the scene (i.e. -toSun).
			glm::vec3 toSun = env.SunDirectionToSun();
		glm::vec3 lightDir = -toSun;
		auto cameraView = m_camera.getViewMatrix();
		auto cameraProj = m_camera.getProjMatrix();
		// Keep Y-flip consistent with main view (UpdateFrameUniforms does proj[1][1] *= -1 for Vulkan).
		// Cascade frustum extraction must use the same Y-flipped proj, otherwise shadow tiles are Y-mirrored.
		cameraProj[1][1] *= -1.0f;

		smvk::Light lights[MAX_LIGHTS]{};
		smvk::Light& light = lights[0];
		light.color = glm::vec4(env.sunColor, 1.0f);
		light.direction[0] = lightDir.x;
		light.direction[1] = lightDir.y;
		light.direction[2] = lightDir.z;
		light.intensity = env.sunIntensity;
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
		auto splits = smvk::ComputeCascadeSplits(default_near_clip, CascadeFar(), m_cascadeLambda, NUM_CASCADES);

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
			std::tan(glm::radians(env.sunDiscSizeDeg)),
			m_evsmSoftness);
		// UI bias scales over the engine's texel-scaled per-cascade bias.
		for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
			dir.cascades[i].bias *= m_cascadeDepthBiasScale;
			dir.cascades[i].normalBias *= m_cascadeNormalBiasScale;
		}
		// Mirrors for the shadow UI sections (ranges + applied biases).
		for (uint32_t i = 0; i <= NUM_CASCADES; ++i)
			m_cascadeSplitsDbg[i] = splits[i];
		for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
			m_cascadeBiasDbg[i] = dir.cascades[i].bias;
			m_cascadeNormalBiasDbg[i] = dir.cascades[i].normalBias;
		}
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

	void WorldRenderer::UpdateMaterials(uint32_t currentFrame) {
		// Preserve world material slots: shaders index by world materialId,
		// so a missing slot must stay a hole (default values), not shift
		// every later material down by one.
		const uint32_t slotCount = (uint32_t)m_world->materialLocations.size();
		if (slotCount > MAX_MATERIALS) {
			std::cerr << "[WorldRenderer] Material slots (" << slotCount
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
	void WorldRenderer::UpdateTextureStreaming(const FrameUniforms& frameData, smvk::Extent2D renderExtent) {
		(void)frameData;
		if (!m_textureStreamingEnabled || !m_textureStreamer)
			return;
		const uint32_t streamFrame = static_cast<uint32_t>(m_frameCounter);
		if ((m_frameCounter & 3u) == 0u) {
			SMVK_PROFILE_STREAM("TexStreamingMinDistPass");
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
			SMVK_PROFILE_TAG("slots", minDist.size());
			const float fovY = glm::radians(kCameraFovYDegrees);
			m_textureStreamer->RequestMipsByDistance(minDist,
				static_cast<float>(renderExtent.height), fovY, streamFrame);
		}
		{
			SMVK_PROFILE_STREAM("TexStreamingUpdate");
			m_textureStreamer->Update(streamFrame);
		}
		const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
		if (stats.upgradesLastUpdate > 0 || stats.downgradesLastUpdate > 0) {
			SMVK_PROFILE_STREAM("TexStreamingFlushUploads");
			m_context->FlushUploads();
		}
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
	void WorldRenderer::UpdateAutoExposure(uint32_t frameIndex) {
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
	bool WorldRenderer::ExposureMeteringActive() const {
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

	// Debug-view gate for the scene post-fx emissions (PaneMask + SSR +
	// volumetric): true when a visualization replaces the scene pixels,
	// so effects that composite over them must stay off (see the header
	// comment). Skipping the SSR record also freezes + invalidates the
	// temporal history for free (m_ssrFrameRanTemporal stays false, so
	// SsrTaaEndFrame invalidates — the first lit record after the view
	// warms up instead of blending against pre-view history).
	bool WorldRenderer::DebugVisualizationActive() const {
		// F2..F11 replace the Main opaque/transparent legs with flat
		// programs; the GI debug views (R/F8) and the DS visualizers (Y)
		// early-out of the lit fragment shader the same way.
		if (m_debugView != DebugView::Lit)
			return true;
		if (m_giDebug != 0)
			return true;
#if defined(DS_DEBUG_VIZ)
		if (m_dsDebugMode != DsDebugViz::Off)
			return true;
#endif
		return false;
	}

	// Bins -> percentile target + damped adapt (tail of UpdateAutoExposure,
	// shared by the single and batched download paths).
	void WorldRenderer::AdaptExposureFromBins(
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
	// signals it, exposure adapts + the DS overlay stats refresh from
	// the staging memcpy. One ticket in flight: a tick while the
	// previous is pending skips its submit (results land 1-2 frames
	// later; adaptation is time-damped anyway).
	void WorldRenderer::UpdateReadbacks(uint32_t frameSlot, uint32_t streamFrame) {
		auto& meter = m_viewportResources.exposure;
		const bool meterValid = meter.Valid()
			&& frameSlot < static_cast<uint32_t>(::MAX_FRAMES_IN_FLIGHT);
	// Layout helper: the SAME span shape must accompany the submit and
	// its poll (destinations are stable members; handles/sizes are fixed).
	std::array<smvk::BufferDownload, 2> dls{};
	const auto buildPendingDownloads = [&]() -> std::span<const smvk::BufferDownload> {
		uint32_t n = 0;
		if (m_pendingReadbackHasExposure) {
			dls[n] = { meter.HistogramBuffer(m_pendingReadbackSlot),
				m_pendingExposureBins.data(),
				m_pendingExposureBins.size() * sizeof(uint32_t), 0 };
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
		// DS counters (40 B) ride the same batch (no extra submit): the
		// slice's zero-fill was only STAGED in UpdateForwardPlusLights
		// (ring flush happens at EndAndPresent), so the transfer-queue
		// FIFO keeps this copy ahead of it and it reads the retired
		// accumulation like the exposure histogram does.
		m_pendingReadbackHasDs = DsActive();
		auto dlsSpan = buildPendingDownloads();
		if (!dlsSpan.empty())
			m_asyncReadbackTicket =
				m_context->SubmitAsyncDownloadDataFromBuffers(dlsSpan);
	}
	}

	// ---------------------------------------------------------------------
	// Named GI sidecars (Cluster::giBindings across the loaded clusters).
	// Model supports N bindings; exactly one per kind is honored for now
	// (first wins + warn). Empty outcome = registry-less pak: callers
	// fall back to the scene id. Since the bake-asset migration the
	// names are bake GUIDs (see Engine/Asset/bake_asset.h); legacy paks
	// record human stems — both resolve through the same VFS paths.
	// ---------------------------------------------------------------------
	void WorldRenderer::ResolveGiSidecars() {
		m_giProbeName.clear();
#if 0 // Lightmaps deprecated
		m_giLightmapName.clear();
#endif
		m_giRegName.clear();
		m_giCvolName.clear();
		m_giCubemapName.clear();
		if (!m_world)
			return;
		auto takeFirst = [](std::string& slot, const std::string& name,
				const char* label) {
			if (slot.empty())
				slot = name;
			else if (slot != name)
				std::cerr << "[GI] extra " << label << " binding '"
					<< name << "' ignored (one per kind for now)\n";
		};
		for (const auto& clusterPtr : m_world->clusters) {
			if (!clusterPtr)
				continue;
			for (const smvk::GiBinding& b : clusterPtr->giBindings) {
				if (b.name.empty())
					continue;
				switch (b.kind) {
				case smvk::GiBinding::Kind::Probe:
					takeFirst(m_giProbeName, b.name, "probe");
					break;
#if 0 // Lightmaps deprecated
				case smvk::GiBinding::Kind::Lightmap:
					takeFirst(m_giLightmapName, b.name, "lightmap");
					break;
#endif
				case smvk::GiBinding::Kind::RegManifest:
					takeFirst(m_giRegName, b.name, "regmanifest");
					break;
				case smvk::GiBinding::Kind::CvolManifest:
					takeFirst(m_giCvolName, b.name, "cvolmanifest");
					break;
				case smvk::GiBinding::Kind::Cubemap:
					takeFirst(m_giCubemapName, b.name, "cubemap");
					break;
				}
			}
		}
		if (!m_giProbeName.empty() || !m_giLightmapName.empty()
			|| !m_giRegName.empty() || !m_giCvolName.empty()
			|| !m_giCubemapName.empty())
			std::cerr << "[GI] sidecars from pak registry: probe='"
				<< m_giProbeName << "' lightmap='" << m_giLightmapName
				<< "' reg='" << m_giRegName << "' cvol='" << m_giCvolName
				<< "' cubemap='" << m_giCubemapName << "'\n";
	}

	// ---------------------------------------------------------------------
	// Cooked scene lights (KHR_lights_punctual).
	//
	// Built once per scene load from the resolved world: the definitions
	// come back as Cluster pool assets (Light blobs in the pak, loaded
	// exactly like meshes — m_world->lightLocations resolves the world
	// light index to Cluster + handle) and the node links ride the scene
	// graph, so global transforms apply coordFix + parent chains exactly
	// like meshes. The directional sun seeds the environment
	// (azimuth/elevation/color/intensity, so the sky disc, IBL tint and
	// shadow light stay in sync) and the points/spots become the static
	// Forward+ set. Emission direction is the node's -Z axis (glTF
	// punctual convention).
	// ---------------------------------------------------------------------
	void WorldRenderer::BuildCookedLightsFromWorld() {
		m_cookedLights.clear();
		if (!m_world)
			return;
		const ::Scene& scene = m_world->scene;
		const auto& lightLocs = m_world->lightLocations;
		if (lightLocs.empty() || scene.lightForNode.empty()) {
			std::cerr << "[Scene] no cooked lights (procedural Forward+ active)\n";
			return;
		}
		// Deterministic node order (pak order is already sorted, but the
		// world map is a hash — sort the node ids).
		std::vector<uint32_t> nodes;
		nodes.reserve(scene.lightForNode.size());
		for (const auto& [node, li] : scene.lightForNode)
			nodes.push_back(node);
		std::sort(nodes.begin(), nodes.end());
		std::vector<smvk::ForwardPlusLight> spots, points;
		bool sunTaken = false;
		uint32_t staticBakeOnly = 0;
		// glTF aim axis after the coordFix basis change (cooked content
		// is baked at coordFix 0, which flips Z; a plain -Z read off the
		// conjugated transform aims 180 degrees off — the Bistro sun
		// seeded the env underground).
		const glm::vec3 aimAxis = GltfPunctualAimAxis(0);
		for (uint32_t node : nodes) {
			auto lit = scene.lightForNode.find(node);
			if (lit == scene.lightForNode.end())
				continue;
			if (lit->second >= lightLocs.size() || node >= scene.globalTransform.size())
				continue;
			const auto& loc = lightLocs[lit->second];
			if (!loc.cluster || !loc.handle.valid())
				continue;
			const ::SceneLight* Lp = loc.cluster->lights.get(loc.handle);
			if (!Lp)
				continue;
			const ::SceneLight& L = *Lp;
			const glm::mat4& G = scene.globalTransform[node];
			// Static-flagged punctual lights are bake-only: their direct +
			// bounced term is carried by the lightmap/probe sources for
			// covered pixels (see CollectStaticBakeLights), so rendering
			// them as forward+ lights too would double the direct
			// contribution. Directionals are exempt: the sun stays a
			// runtime CSM light (the bake transports only its bounce).
			if (L.type != ::SceneLight::Type::Directional &&
				(L.flags & ::SceneLight::kSceneLightFlag_Static) != 0) {
				++staticBakeOnly;
				continue;
			}
			if (L.type == ::SceneLight::Type::Directional) {
				if (sunTaken)
					continue; // single-sun renderer: first directional wins
				glm::vec3 emis = glm::vec3(G * glm::vec4(aimAxis, 0.0f));
				if (glm::length(emis) < 1e-6f)
					continue;
				emis = glm::normalize(emis);
				m_cookedLights.sunDirToSun = -emis;
				m_cookedLights.sunColor = L.color;
				m_cookedLights.sunIntensity = L.intensity;
				m_cookedLights.hasSun = true;
				sunTaken = true;
			}
			else if (L.type == ::SceneLight::Type::Point) {
				smvk::ForwardPlusLight pl{};
				pl.color = glm::vec4(L.color, 1.0f);
				pl.position = glm::vec3(G[3]);
				pl.range = L.range; // 0 = infinite (shaders handle <= 0)
				pl.intensity = L.intensity;
				pl.type = (int32_t)smvk::kForwardPlusPoint;
				pl.shadowTile = -1;
				pl.shadowMapIndex = -1;
				pl.cookieIndex = -1;
				pl.functionType = smvk::kForwardPlusFunction_None;
				points.push_back(pl);
			}
		else if (L.type == ::SceneLight::Type::Spot) {
			glm::vec3 emis = glm::vec3(G * glm::vec4(aimAxis, 0.0f));
				if (glm::length(emis) < 1e-6f)
					continue;
				emis = glm::normalize(emis);
				smvk::ForwardPlusLight sl{};
				sl.color = glm::vec4(L.color, 1.0f);
				sl.position = glm::vec3(G[3]);
				sl.direction = emis;
				sl.range = L.range;
				sl.intensity = L.intensity;
				sl.innerAngle = L.innerConeAngle;
				sl.outerAngle = L.outerConeAngle;
				sl.type = (int32_t)smvk::kForwardPlusSpot;
				// Editor-authored shadow request (SceneLight cast-shadow
				// flag; points ignore shadow fields): the frame's spot
				// tile selection honors the request.
				if ((L.flags & ::SceneLight::kSceneLightFlag_CastShadow) != 0)
					sl.flags |= smvk::kForwardPlusFlag_CastShadow;
				sl.shadowTile = -1;
				sl.shadowMapIndex = -1;
				sl.cookieIndex = -1;
				sl.functionType = smvk::kForwardPlusFunction_None;
				spots.push_back(sl);
			}
		}
		// Spots first (per-tile slot priority, same invariant as procedural).
		m_cookedLights.pointSpots.reserve(spots.size() + points.size());
		m_cookedLights.pointSpots.insert(
			m_cookedLights.pointSpots.end(), spots.begin(), spots.end());
		m_cookedLights.pointSpots.insert(
			m_cookedLights.pointSpots.end(), points.begin(), points.end());
		if (m_cookedLights.pointSpots.size() > FP_MAX_LIGHTS) {
			std::cerr << "[Scene] cooked lights clamped: "
				<< m_cookedLights.pointSpots.size() << " -> " << FP_MAX_LIGHTS << "\n";
			m_cookedLights.pointSpots.resize(FP_MAX_LIGHTS);
		}
		// Seed the SCENE's sun from the authored light so the sky disc, IBL
		// tint and shadow-casting light stay in sync (sun fields only, in
		// scene units — like the editor's cooked-sun seed). Runs only when
		// the scene carries no authored look (old content without env
		// blocks); a tour/cooked env always wins. The user can still
		// retouch the look afterwards (this runs once at load/rebind).
		if (m_cookedLights.hasSun && m_world && !m_world->scene.hasEnvironment) {
			const glm::vec3 t = glm::normalize(m_cookedLights.sunDirToSun);
			SceneEnv& sceneEnv = m_world->scene.defaultEnvironment;
			sceneEnv.sunElevation = glm::degrees(std::asin(
				std::clamp(t.y, -1.0f, 1.0f)));
			sceneEnv.sunAzimuth = glm::degrees(std::atan2(t.x, t.z));
			sceneEnv.sunColor = m_cookedLights.sunColor;
			sceneEnv.sunIntensity = glm::clamp(
				m_cookedLights.sunIntensity * 0.001f, 0.0f, 10.0f);
			m_world->scene.environment = sceneEnv;
			m_world->scene.hasEnvironment = true;
			std::cerr << "[Scene] cooked sun: az=" << sceneEnv.sunAzimuth
				<< " el=" << sceneEnv.sunElevation
				<< " I=" << sceneEnv.sunIntensity << "\n";
		}
		std::cerr << "[Scene] cooked lights: spots=" << spots.size()
			<< " points=" << points.size()
			<< (m_cookedLights.hasSun ? " + sun" : " (no sun)");
		if (staticBakeOnly > 0)
			std::cerr << " | static(bake-only)=" << staticBakeOnly;
		std::cerr << "\n";
	}

	// ---------------------------------------------------------------------
	// Procedural Forward+ demo lights.
	//
	// The demo light set is pure data (no scene nodes, no emissive ball
	// meshes): UpdateForwardPlusLights() computes the point grid (with the
	// bob animation) and the orbiting spots on the CPU and uploads the
	// Forward+ light buffer directly. The ImGui overlay draws the lights
	// as colored points + indices (see the UI block in RenderFrame).
	// When the scene carries cooked KHR_lights_punctual lights (and the
	// cooked toggle is on) those are uploaded INSTEAD — static authored
	// placement, no orbit/bob, no cookies/functions.
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
	uint32_t WorldRenderer::UpdateForwardPlusLights(uint32_t currentFrame, const FrameUniforms& frame) {
		using fp_clk = std::chrono::steady_clock;
		const bool fpBench = frameBudget_ > 0 && benchWarmup_ >= 10;
		const auto fpT0 = fp_clk::now();
		std::vector<smvk::ForwardPlusLight> lights;
		lights.reserve(FP_MAX_LIGHTS);
		m_debugLights.clear();
		// Cooked authored lights win over the procedural grid when present.
		if (CookedLightsActive()) {
			for (const auto& cl : m_cookedLights.pointSpots) {
				if (lights.size() >= FP_MAX_BUILD_LIGHTS)
					break;
				lights.push_back(cl);
				m_debugLights.push_back({ glm::vec3(cl.position),
					glm::vec3(cl.color), (int)cl.type });
			}
		}
		else if (m_fpDemo.enabled) {
			const int gx = std::max(1, m_fpDemo.gridX);
			const int gy = std::max(1, m_fpDemo.gridY);
			const int gz = std::max(1, m_fpDemo.gridZ);
			const uint32_t gridCount = (uint32_t)gx * (uint32_t)gy * (uint32_t)gz;
			const uint32_t spotReserve = (uint32_t)std::max(0, m_fpDemo.spotCount);
			const uint32_t pointCount = (FP_MAX_BUILD_LIGHTS > spotReserve)
				? std::min(gridCount, FP_MAX_BUILD_LIGHTS - spotReserve)
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
		for (int s = 0; s < m_fpDemo.spotCount && lights.size() < FP_MAX_BUILD_LIGHTS; ++s) {
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
			for (uint32_t i = 0; i < pointCount && lights.size() < FP_MAX_BUILD_LIGHTS; ++i) {
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
	// MULTIVIEW: capture the final (post-DS-sort, pre-probe-tail) light set
	// for the secondary views — each hosted view copies it, runs its own
	// spot-shadow selection over the copy (patching shadowTile/
	// shadowMapIndex for ITS atlas) and uploads into its view-major slab.
	m_frameLights = lights;
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

	// ---- Reflection probes as FP light entries (type kForwardPlusProbe) ----
	// Appended AFTER every light consumer above (spot selection, DS zBins
	// + masks, telemetry, debug UI): the tile cull sphere-tests the full
	// [0, L+P) range while every light path keeps its lights-only count,
	// so probes can neither steal shadow slots nor leak into DS lists.
	// Upload order is the cache's widest-first order, so tile overflow
	// keeps the widest probes.
	{
		const uint32_t probeEntries = ProbeEntryCount();
		if (probeEntries > 0
			&& lights.size() + probeEntries <= FP_MAX_LIGHTS) {
			std::vector<smvk::ForwardPlusLight> gpu;
			gpu.reserve(probeEntries);
			for (uint32_t i = 0; i < probeEntries; ++i) {
				const CubemapProbeGpu& pr = m_cubemapProbes[i];
				gpu.push_back(smvk::MakeProbeLightEntry(
					glm::vec3(pr.posInfluence.x, pr.posInfluence.y, pr.posInfluence.z),
					pr.posInfluence.w, pr.fadeCubemap.x,
					static_cast<uint32_t>(pr.fadeCubemap.y),
					static_cast<uint32_t>(pr.fadeCubemap.z)));
			}
			m_context->UploadDataToBuffer(
				m_resources.fpLightsBuffer,
				gpu.data(),
				sizeof(smvk::ForwardPlusLight) * gpu.size(),
				smvk::PipelineStage(smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute),
				smvk::AccessFlags::ShaderRead,
				0,
				currentFrame * FP_FRAME_STRIDE
					+ lights.size() * sizeof(smvk::ForwardPlusLight)
			);
		}
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

	uint32_t WorldRenderer::UpdateDecals(uint32_t currentFrame) {
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

	void WorldRenderer::SeedParticleEmitters() {
		if (!m_particleDemo.emitters.empty())
			return;
		m_particleDemo.emitters.reserve(4);

		// Rising embers: additive orange sparks drifting up from a small
		// cone at the origin (negative gravityScale = buoyant).
		{
			smvk::ParticleEmitterGpu embers{};
			embers.position = glm::vec4(0.0f, 0.35f, 0.0f, 0.22f);
			embers.direction = glm::vec4(0.0f, 1.0f, 0.0f, 0.35f);
			embers.color = glm::vec4(1.0f, 0.42f, 0.10f, 6.0f);
			embers.velocity = glm::vec4(0.7f, 1.6f, 0.45f, -0.35f);
			embers.size = glm::vec4(0.012f, 0.045f, 2.0f, 11u);
			embers.life = glm::vec4(1.6f, 3.6f, 90.0f,
				(float)smvk::kParticleShape_Cone);
			embers.fade = glm::vec4(0.05f, 0.40f, 0.30f, 0.0f);
			m_particleDemo.emitters.push_back(embers);
		}
		// Dust motes: slow, faint, wide sphere drift — reads best inside
		// the tiled light crowd (almost pure lit response, tiny emissive).
		{
			smvk::ParticleEmitterGpu motes{};
			motes.position = glm::vec4(0.0f, 2.0f, 0.0f, 3.5f);
			motes.direction = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			motes.color = glm::vec4(0.70f, 0.78f, 1.0f, 0.12f);
			motes.velocity = glm::vec4(0.04f, 0.12f, 0.15f, 0.0f);
			motes.size = glm::vec4(0.008f, 0.022f, 0.6f, 77u);
			motes.life = glm::vec4(4.0f, 9.0f, 45.0f,
				(float)smvk::kParticleShape_Sphere);
			motes.fade = glm::vec4(0.15f, 0.30f, 0.20f, 0.0f);
			m_particleDemo.emitters.push_back(motes);
		}
		m_particleDemo.selected = 0;
	}

	uint32_t WorldRenderer::UpdateParticles(uint32_t currentFrame) {
		smvk::Particles& ps = m_viewportResources.particles;
		if (!ParticlesActive() || m_particleDemo.emitters.empty())
			return 0;
		// Every frame, like the decal ring: sliders retouch live emitters
		// immediately and every flight slice stays fresh. The GPU's own
		// fractional-rate carry lives in the separate emitterState buffer,
		// so this upload can never clobber spawn scheduling.
		return ps.UpdateEmitters(*m_context, currentFrame,
			m_particleDemo.emitters.data(),
			(uint32_t)m_particleDemo.emitters.size());
	}

	bool WorldRenderer::EnsurePickScene() {
		// Pick acceleration needs CPU-side triangle soup. Cooked worlds
		// carry none (pak clusters upload straight to the GPU; the glTF
		// re-import source went away with the scene registry), so the
		// BVH stays off and every consumer fails OPEN: probe culling
		// draws unculled, the crosshair splat logs + no-ops.
		if (m_decalDemo.pickBvhState == 0) {
			std::cerr << "[Pick] no CPU pick source (cooked worlds upload "
				"straight to the GPU) - pick BVH off\n";
			m_decalDemo.pickBvhState = -1;
		}
		return m_decalDemo.pickBvhState > 0 && m_decalPickScene
			&& !m_decalPickScene->Bvh().Empty();
	}

	bool WorldRenderer::GiProbeVisibleFromCamera(uint32_t probeIdx, const glm::vec3& camPos) {
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

	void WorldRenderer::SpawnDecalAtCrosshair() {
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

	void WorldRenderer::SpawnDecalPresets() {
		if (m_sceneId != "sponza")
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
		// from (0,1,0); horizon-level view, so the pools sit in the lower
		// band of the frame). Floor plane is y = 0; the box hovers 0.5 m above
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

	bool WorldRenderer::InitDecalTextures() {
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
	void WorldRenderer::AppendDeferredShadowPhases(uint32_t frameIndex,
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
	void WorldRenderer::DumpDeferredShadows(uint32_t frameIndex) {
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
	// Decisions are cached per node in the World and refreshed EVERY frame
	// by default (halfrate parity opt-out; see World::LodIndexForNode and
	// m_lodFullRate). Selection is hysteretic around the pixel budget
	// (kLodSelectHysteresis): a node keeps its level while the projected
	// error stays inside the budget band, so switch distances don't flip
	// siblings frame to frame while the camera moves.
	uint32_t WorldRenderer::LodIndexForNode(NodeId node, uint32_t worldMesh) const {
		if (!m_lodEnabled || !m_world)
			return 0;
		if (m_world->LodChainLength(worldMesh) <= 1)
			return 0;
		return m_world->LodIndexForNode(node, worldMesh, m_lodFrameInputs);
	}

	smvk::StaticMesh* WorldRenderer::LodMeshForNode(NodeId node, uint32_t worldMesh,
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
	smvk::LodShader WorldRenderer::LodShaderForNode(NodeId node,
		uint32_t worldMesh) const {
		if (!ShaderLodActive() || !m_world)
			return smvk::LodShader::Lit;
		const uint32_t lod = LodIndexForNode(node, worldMesh);
		return m_world->LodShaderAt(worldMesh, lod);
	}

	void WorldRenderer::BuildShadowDrawCandidates() {
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

	void WorldRenderer::RecordShadowCastersDirect(smvk::CommandBuffer& cmd,
		smvk::ShaderProgram& opaqueProgram, const PushConstants& pc,
		uint32_t casterViewIndex, bool skipFiltered) {
		// Phase 2 masked pipeline: cull follows the material raster so
		// the shadow depth contains exactly the faces the main view draws
		// (single-sided cutout has no backface shadow depth). Rebinds
		// only on cull changes (CommandBuffer dedupes identical states).
		smvk::CullMode boundShadowMaskedCull = smvk::CullMode::FrontAndBack;
		auto ensureShadowMaskedPipeline = [&](smvk::CullMode cull) {
			if (cull == boundShadowMaskedCull)
				return;
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
				smvk::RasterSettings{ .cullMode = cull }
			);
			cmd.PushConstants(0, sizeof(PushConstants), &pc, smvk::ShaderStage::All);
			boundShadowMaskedCull = cull;
		};
		auto drawCaster = [&](const smvk::World::RenderItem& item, bool maskedPhase) {
			if (item.material == smvk::World::kInvalidIndex)
				return;
			// Indirect path: registered casters are covered by the per-view
			// command list — only unfilterable meshes rasterize here.
			if (skipFiltered
				&& m_gfxRenderer.GfxMeshForLod(item.mesh,
					LodIndexForNode(item.node, item.mesh)) != UINT32_MAX)
				return;
			// Masked (cutout) flag replaces the per-node material fetch the
			// phase split used to pay.
			if (((item.flags & smvk::IteratorFlagBit_Masked) != 0) != maskedPhase)
				return;
			if (maskedPhase) {
				smvk::CullMode cull = smvk::CullMode::None;
				if (const smvk::Material* mat = m_world->GetMaterial(item.material))
					cull = mat->pipeline.raster.cullMode;
				ensureShadowMaskedPipeline(cull);
			}
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
		// Phase 1: solid casters, depth-only program. FRONT-face culling
		// (render the back faces only): the classic shadow-map anti-acne —
		// the back face of a solid occluder sits behind its lit face by
		// the surface thickness, so receiver self-shadow acne falls off
		// the depth test without slope-scaled bias. Masked (cutout) phase
		// 2 below keeps material-following cull (single-sided foliage
		// has no back face to render).
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
				.cullMode = smvk::CullMode::Front
			}
		);
		cmd.PushConstants(0, sizeof(PushConstants), &pc, smvk::ShaderStage::All);
		// Opaque only: BLEND transparent must not cast (full-quad depth).
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
			| smvk::IteratorFlagBit_Opaque
			, [&](const smvk::World::RenderItem& item) { drawCaster(item, false); });
		// Phase 2: MASK cutout casters, alpha-tested program so foliage
		// casts leaf-shaped shadows instead of full quads. The pipeline
		// binds lazily per material cull (ensureShadowMaskedPipeline).
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
			| smvk::IteratorFlagBit_Opaque
			, [&](const smvk::World::RenderItem& item) { drawCaster(item, true); });
	}

	void WorldRenderer::RecordEvsmMomentsDirect(smvk::CommandBuffer& cmd,
		smvk::ShaderProgram& momentsProgram, const PushConstants& pc,
		uint32_t casterViewIndex, bool skipFiltered,
		smvk::ShaderProgram* opaqueProgram) {
		// EVSM direct path: same caster walk as RecordShadowCastersDirect.
		// Phases split by cost — phase 1 (opaque) binds the CHEAP moments
		// program when provided (no material read / texture fetch;
		// per-fragment work is two exp() + two squares + the
		// RGBA32F store, ~the PCF depth-only cost), phase 2 (MASK cutout)
		// binds the full alpha-tested program. Binding the full FS for
		// opaque measured ~30x the PCF pass (base-color sample
		// at 2xMSAA rates over a 2048^2 tile).
		const smvk::MSAASettings msaa2x = smvk::MSAASettings::Count2();
		// Phase 2 masked pipeline: cull follows the material raster
		// (matching the main view); rebinds only on cull changes.
		smvk::CullMode boundMomentsMaskedCull = smvk::CullMode::FrontAndBack;
		auto ensureMomentsMaskedPipeline = [&](smvk::CullMode cull) {
			if (cull == boundMomentsMaskedCull)
				return;
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
				smvk::RasterSettings{ .cullMode = cull }
			);
			cmd.PushConstants(0, sizeof(PushConstants), &pc, smvk::ShaderStage::All);
			boundMomentsMaskedCull = cull;
		};
		auto drawCaster = [&](const smvk::World::RenderItem& item, bool maskedPhase) {
			if (item.material == smvk::World::kInvalidIndex)
				return;
			if (skipFiltered
				&& m_gfxRenderer.GfxMeshForLod(item.mesh,
					LodIndexForNode(item.node, item.mesh)) != UINT32_MAX)
				return;
			if (((item.flags & smvk::IteratorFlagBit_Masked) != 0) != maskedPhase)
				return;
			if (maskedPhase) {
				smvk::CullMode cull = smvk::CullMode::None;
				if (const smvk::Material* mat = m_world->GetMaterial(item.material))
					cull = mat->pipeline.raster.cullMode;
				ensureMomentsMaskedPipeline(cull);
			}
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
		// Phase 1: solid casters (front-culled like the PCF direct path —
		// back faces only, the anti-acne classic), cheap opaque program.
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
				.cullMode = smvk::CullMode::Front
			}
		);
		cmd.PushConstants(0, sizeof(PushConstants), &pc, smvk::ShaderStage::All);
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
			| smvk::IteratorFlagBit_Opaque
			, [&](const smvk::World::RenderItem& item) { drawCaster(item, false); });
		// Phase 2: MASK cutout casters (full alpha-tested moments program
		// - the FS discards under the cutoff like shadow.frag so foliage
		// casts leaf-shaped moments). The pipeline binds lazily per
		// material cull (ensureMomentsMaskedPipeline above).
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
			| smvk::IteratorFlagBit_Opaque
			, [&](const smvk::World::RenderItem& item) { drawCaster(item, true); });
	}

	void WorldRenderer::ResetLodStats() {
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

	void WorldRenderer::RefreshGpuMemoryStats(bool force) {
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

	void WorldRenderer::LogGpuMemoryToConsole() {
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

	void WorldRenderer::RebuildSwapViews() {
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
		m_viewportResources.sao.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSaoDesc());
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
		// Same for the SSR temporal history (targets recreated above).
		smvk::SsrTaaInvalidateHistory(m_ssrTaa);
		UpdateCameraAspect(renderExtent);
	}

	// ---------------------------------------------------------------------
	// Artistic LUT grading (TPP-style 32^3 strip, see tonemap.h). The strip
	// is 1024x32 sRGB-encoded PNG (32 tiles along x, blue = tile, red =
	// column, green = row); Photoshop edits the bytes directly.
	// ---------------------------------------------------------------------
	void WorldRenderer::ScanLutFiles() {
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

	bool WorldRenderer::LoadLut(const std::string& file) {
		std::vector<uint8_t> rgba;
		int w = 0, h = 0;
		const std::string path = "content/luts/" + file;
		if (!LutLoadPng(path.c_str(), rgba, w, h)) {
			std::cerr << "[WorldRenderer] LUT decode failed: " << path << "\n";
			return false;
		}
		if (w != 1024 || h != 32) {
			std::cerr << "[WorldRenderer] LUT " << file << " is " << w << "x" << h
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
		std::cout << "[WorldRenderer] LUT loaded: " << file
			<< " (bindless " << m_lutBindless << ")\n";
		return true;
	}

	void WorldRenderer::ExportNeutralLut() {
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
			std::cout << "[WorldRenderer] neutral LUT strip exported: exports/lut_neutral.png\n";
		else
			std::cerr << "[WorldRenderer] neutral LUT export failed (exports/lut_neutral.png)\n";
		// Seed the editable copy into content/luts once; never overwrite (the
		// user may have graded it already - F7 always refreshes exports/).
		if (!std::filesystem::exists("content/luts/neutral.png", ec))
			LutWritePng("content/luts/neutral.png", W, H, px.data());
	}

	void WorldRenderer::EnsureCaptureTarget(smvk::Extent2D extent) {
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

	void WorldRenderer::ExportScreenshot(smvk::Extent2D extent) {
		if (m_captureImage.empty()) {
			std::cerr << "[WorldRenderer] capture target unavailable - screenshot skipped\n";
			return;
		}
		std::vector<uint8_t> px(static_cast<size_t>(extent.width) * extent.height * 4);
		// The capture pass leaves the image in COLOR_ATTACHMENT_OPTIMAL (the
		// rendergraph does not write sync states back, so the layout is
		// declared explicitly - Undefined would discard the contents).
		if (!m_context->DownloadDataFromImage(m_captureImage,
				smvk::ImageLayout::ColorAttachmentOptimal, px.data(), px.size())) {
			std::cerr << "[WorldRenderer] screenshot readback failed\n";
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
			std::cout << "[WorldRenderer] screenshot exported: " << path << "\n";
		else
			std::cerr << "[WorldRenderer] screenshot write failed (" << path << ")\n";
	}

	void WorldRenderer::DrawFrame() {
		// Frame-arena scratch (URI probing, normalized rewrites) is
		// dead at frame end by definition — recycle the blocks.
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
		// Frame-time histogram hook (zero cost while disabled — the F12
		// toggle; see profiler::RecordFrameTime). Hosted mode skips: the
		// editor (the host) records its own frame instead.
		if (!IsHosted())
			smvk::profiler::RecordFrameTime(double(delta_time_s) * 1000.0);
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

// Shared transparent draw list (see header). Mirrors Main's transparent
// leg item-for-item: Renderable|Transparent|StaticVertexFmt items,
// frustum-tested against the FROZEN transparent cull view (conservative
// LOD union, matching FilterMainView) so the shaded leg and the SSR
// PaneMask redraw always draw the same geometry. Distance-sorted only
// when the caller blends back-to-front; the pane pass passes false
// (its Max blend is order-independent).
std::vector<WorldRenderer::TransparentDrawItem>
WorldRenderer::CollectTransparentDraws(const smvk::RenderView& cullView,
	bool sortBackToFront, std::optional<glm::vec3> sortCamPos)
{
	std::vector<TransparentDrawItem> draws;
	if (m_hideTransparent)
		return draws;
	// Sort camera: the main positioner by default; multiview secondary
	// views pass their own eye so back-to-front ordering is view-correct.
	const glm::vec3 camPos = sortCamPos
		? *sortCamPos : m_cameraPositioner.cameraPosition_;
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
				bool visible = smvk::RenderViewIntersectsSphere(cullView, c, r);
				if (!visible && selDesc && selDesc != baseDesc) {
					smvk::MeshBoundsToWorld(model, selDesc->geometry.aabbMin,
						selDesc->geometry.aabbMax, c, r);
					visible = smvk::RenderViewIntersectsSphere(cullView, c, r);
				}
				if (!visible)
					return;
			}
		}
		const glm::vec3 center{ model[3][0], model[3][1], model[3][2] };
		const glm::vec3 d = center - camPos;
		draws.push_back(TransparentDrawItem{
			.node = item.node,
			.meshId = item.mesh,
			.materialId = item.material,
			.dist2 = glm::dot(d, d),
		});
	}
	);
	if (sortBackToFront) {
		std::sort(draws.begin(), draws.end(),
			[](const TransparentDrawItem& a, const TransparentDrawItem& b)
			{ return a.dist2 > b.dist2; });
	}
	return draws;
}

// ===========================================================================
// HOSTED one-call setup (see the header): boot off the host's context,
// borrow the world, configure the full framegraph + hosted invariants and
// initialize — the host specifies the world and is done.
// ===========================================================================

WorldRenderer::WorldRenderer(smvk::App& host, smvk::World& world,
	smvk::Extent2D extent, std::string sceneId, std::string projectRoot,
	smvk::Format format)
{
	// Hosted boot BEFORE Initialize (App::Initialize takes the hosted
	// branch: borrowed context + offscreen target, no window/SDL).
	if (!RequestHostedBoot(host, extent, format)) {
		throw std::runtime_error(
			"WorldRenderer: hosted boot request rejected");
	}
	// Borrow the host's world (the renderer never imports anything of
	// its own on hosted runs; edits show up live, per frame).
	SetHostedBorrowedWorld(&world);
	// Scene id + project root precede Initialize (the GI sidecar loads
	// resolve gi/<id>.* through the project-aware VFS there).
	SetSceneId(sceneId);
	SetProjectRoot(projectRoot);
	Configure(ScenePassConfig::All());
	// Hosted invariants: the host owns the input, there is no present
	// chain to upscale, and the live authoring view stays CRISP: AA off
	// (no MSAA resolves / temporal smearing) and motion blur off (an
	// authoring view must show the scene as authored, not smeared by
	// camera velocity). Both are App-run toggles the hosted view never
	// grants (SetDisableControl parks the App's own controls).
	SetDisableControl(true);
	SetDlssMode(smvk::DlssMode::Off);
	SetAaMode(AaMode::Off);
	m_motionBlurEnabled = false;
	if (!HostedInitialize()) {
		throw std::runtime_error(
			"WorldRenderer::Initialize failed (see the errors above)");
	}
}

// ===========================================================================
// MULTIVIEW (hosted): per-view image buffers + tic/toc refresh scheduling.
// See world_renderer.h (ViewRefresh + SecondaryView) for the design notes.
// ===========================================================================

void WorldRenderer::SetViewCount(uint32_t count) {
	// Hosted-only: the windowed run keeps the single main view exactly as
	// today. Called at frame boundaries (the hosted launch contract); the
	// device-idle wait below covers a stray mid-frame call anyway.
	if (!IsHosted())
		return;
	count = std::min(count, kMaxSecondaryViews);
	if (count == (uint32_t)m_views.size())
		return;
	// Per-view GPU teardown (logical handles + Holders) needs an idle
	// device — same contract as the hosted launch/stop.
	WaitForDeviceIdle();
	for (SecondaryView& v : m_views) {
		if (!v.outLogical.empty())
			m_rendergraph->FreeImageView(v.outLogical);
		v.outBindlessIndex = UINT32_MAX;
		v.outBindless = {};
		v.outView = {};
		v.outImage = {};
		v.outLogical = {};
		v.outExtent = { 0, 0 };
		v.outFormat = smvk::Format::Undefined;
		v.outValid = false;
		v.vp.forwardPlus.Destroy(*m_rendergraph);
		v.vp.directionalShadows.Destroy(*m_rendergraph);
		v.vp.spotShadows.Destroy(*m_rendergraph);
		v.vp.hdr.Destroy(*m_rendergraph);
		v.vp.renderDepth.Destroy(*m_rendergraph);
		v.dirLightValid = false;
		v.spotShadowCount = 0;
		v.spotShadowMapIndex = -1;
		v.spotShadowTiles.clear();
		v.lightScratch.clear();
		v.prevViewProjValid = false;
		v.renderedLastFrame = false;
	}
	m_views.clear();
	m_views.resize(count);
	const auto ext = m_swapchain->GetExtent();
	for (uint32_t i = 0; i < count; ++i) {
		SecondaryView& v = m_views[i];
		v.ordinal = i + 1u; // 1-based (view 0 = the main hosted view)
		// Extent defaults to the hosted target extent (per-view override
		// via SetViewExtent; zero clamps at the first render).
		v.desc.extent = smvk::Extent2D{ ext.width, ext.height };
		// Refresh spread: alternate Tic/Toc so a full secondary set splits
		// across the two phases (the host can retune per view any time).
		v.desc.refresh = (i & 1u) ? ViewRefresh::Toc : ViewRefresh::Tic;
	}
}

void WorldRenderer::DriveView(uint32_t view, const glm::vec3& eye,
	const glm::vec3& target, float fovYDeg, float zNear, float zFar,
	float orthoHalfH) {
	if (view >= m_views.size())
		return;
	if (glm::length(target - eye) < 1e-6f)
		return; // eye == target: nothing sensible to look at (keep last)
	SecondaryView& v = m_views[view];
	// Degenerate-lookAt guard (mirrors DriveExternalCamera): the shared
	// ResolveLookAtUp picks the up axis for top-down framing — no target
	// nudge (see DriveExternalCamera for why a nudge is wrong: it pans
	// the ortho view and disagrees with the direct-lookAt consumers).
	v.desc.eye = eye;
	v.desc.target = target;
	v.desc.fovYDeg = fovYDeg;
	v.desc.orthoHalfH = std::max(orthoHalfH, 0.0f);
	v.desc.zNear = std::max(zNear, 0.001f);
	v.desc.zFar = std::max(zFar, v.desc.zNear + 0.1f);
	v.positioner.lookAt(eye, target,
		ResolveLookAtUp(eye, target));
}

void WorldRenderer::DriveGrid(bool show, float cellSizeMeters,
	uint32_t extentCells) {
	// Hosted ground-grid drive (see header): zero GPU work, just latches
	// the settings — RenderFrame rebuilds the lines, every Main draws
	// them depth-tested against its own depth.
	m_gridEnabled = show;
	m_gridCellSize = std::max(cellSizeMeters, 0.01f);
	m_gridExtentCells =
		(uint32_t)std::clamp<uint32_t>(extentCells, 2, 512);
}

// Ground grid (XZ plane at y = 0, editor parity): minor lines every
// cell (multiples of 5 skipped), a stronger major line every 5 cells.
// Two contiguous ranges (minor first) for the two authored colors.
// Vertices are cheap enough to rebuild per frame.
static void BuildGridVerts(std::vector<smvk::DebugLineVertex>& minor,
	std::vector<smvk::DebugLineVertex>& major, float cell, int n) {
	for (int i = -n; i <= n; ++i) {
		const float v = float(i) * cell;
		if (i % 5 == 0)
			continue; // major lines below
		minor.push_back({ glm::vec3(-float(n) * cell, 0.0f, v) });
		minor.push_back({ glm::vec3(float(n) * cell, 0.0f, v) });
		minor.push_back({ glm::vec3(v, 0.0f, -float(n) * cell) });
		minor.push_back({ glm::vec3(v, 0.0f, float(n) * cell) });
	}
	for (int i = -n; i <= n; i += 5) {
		const float v = float(i) * cell;
		major.push_back({ glm::vec3(-float(n) * cell, 0.0f, v) });
		major.push_back({ glm::vec3(float(n) * cell, 0.0f, v) });
		major.push_back({ glm::vec3(v, 0.0f, -float(n) * cell) });
		major.push_back({ glm::vec3(v, 0.0f, float(n) * cell) });
	}
}

void WorldRenderer::BuildGridLines() {
	m_gridMinorVerts = 0;
	m_gridMajorVerts = 0;
	if (!m_gridEnabled)
		return;
	std::vector<smvk::DebugLineVertex> minor, major;
	BuildGridVerts(minor, major, m_gridCellSize, (int)m_gridExtentCells);
	if (minor.empty() && major.empty())
		return;
	std::vector<smvk::DebugLineVertex> lines;
	lines.reserve(minor.size() + major.size());
	lines.insert(lines.end(), minor.begin(), minor.end());
	lines.insert(lines.end(), major.begin(), major.end());
	const size_t bytes = lines.size() * sizeof(smvk::DebugLineVertex);
	if (m_gridLines.empty() || m_gridLinesCap < (uint32_t)lines.size()) {
		m_gridLines = m_context->CreateBuffer(bytes,
			smvk::BufferType_Vertex, "grid lines");
		m_gridLinesCap = (uint32_t)lines.size();
	}
	if (m_gridLines.empty())
		return;
	m_context->UploadDataToBuffer(m_gridLines, lines.data(), bytes,
		smvk::PipelineStageBit_Vertex,
		smvk::AccessFlags::VertexAttributeRead);
	m_gridMinorVerts = (uint32_t)minor.size();
	m_gridMajorVerts = (uint32_t)major.size();
}

// Scene->frame environment mapping (see SceneEnv): the scene authors
// sun 0..10 / sky 0..2 while the pipeline is lux-scale (sun 10000 / sky
// 600 defaults), so both ride kSceneEnvToHostedLux (editor 3.0 ->
// 3000 lux, 0.6 -> 600 — the sun:sky:ambient ratios stay the authored
// 5:1:1 and the auto-exposure normalizes the rest). Everything the scene
// does NOT define (sun disc, sky exponent, fog shape, exposure knobs)
// stays at the engine defaults — the renderer owns no environment state
// of its own; it renders the world's look. unlitKillSun mirrors the
// editor frame's Unlit handling (the Unlit debug view replaces shading
// wholesale anyway; this keeps the lit leftovers — transparents,
// particles — honest).
static smvk::EnvironmentSettings MapSceneEnv(const SceneEnv& w,
	bool unlitKillSun)
{
	static constexpr float kSceneEnvToHostedLux = 1000.0f;
	smvk::EnvironmentSettings env{}; // engine defaults (disc, exponent, fog shape)
	env.skyEnabled = true;
	env.iblEnabled = true;
	env.skyZenithColor = w.skyColor;
	env.skyHorizonColor = w.skyColor;
	env.skyIntensity = w.skyIntensity * kSceneEnvToHostedLux;
	env.groundColor = w.skyColor * 0.5f;
	env.iblIntensity = w.skyIntensity * kSceneEnvToHostedLux;
	env.sunAzimuthDeg = w.sunAzimuth;
	env.sunElevationDeg = w.sunElevation;
	env.sunColor = w.sunColor;
	env.sunIntensity = unlitKillSun ? 0.0f
		: w.sunIntensity * kSceneEnvToHostedLux;
	env.fogColor = w.fogColor;
	env.fogDensity = w.fogDensity;
	env.fogMode = 0; // volumetrics authored, not yet rendered here
	return env;
}

void WorldRenderer::SetViewExtent(uint32_t view, smvk::Extent2D extent) {
	if (view >= m_views.size())
		return;
	if (extent.width == 0 || extent.height == 0)
		return; // 0 = keep
	SecondaryView& v = m_views[view];
	if (v.desc.extent.width == extent.width && v.desc.extent.height == extent.height)
		return;
	v.desc.extent = extent;
	// Out-image drift handled by the next Ensure (recreate key).
}

void WorldRenderer::SetViewRefresh(uint32_t view, ViewRefresh refresh) {
	if (view >= m_views.size())
		return;
	m_views[view].desc.refresh = refresh;
}

ViewRefresh WorldRenderer::GetViewRefresh(uint32_t view) const {
	return view < m_views.size() ? m_views[view].desc.refresh : ViewRefresh::Full;
}

void WorldRenderer::SetViewEnabled(uint32_t view, bool enabled) {
	if (view >= m_views.size())
		return;
	m_views[view].desc.enabled = enabled;
}

bool WorldRenderer::ViewRenderedThisFrame(uint32_t view) const {
	return view < m_views.size() && m_views[view].desc.enabled
		&& ViewRefreshDue(m_views[view].desc);
}

uint32_t WorldRenderer::ViewOutBindless(uint32_t view) const {
	return view < m_views.size() ? m_views[view].outBindlessIndex : UINT32_MAX;
}

smvk::Extent2D WorldRenderer::ViewOutExtent(uint32_t view) const {
	return view < m_views.size() ? m_views[view].outExtent : smvk::Extent2D{ 0, 0 };
}

smvk::ImageHandle WorldRenderer::ViewOutImage(uint32_t view) const {
	// Holder -> handle (implicit conversion; empty before the view's
	// first rendered frame).
	if (view >= m_views.size())
		return smvk::ImageHandle{};
	return m_views[view].outImage;
}

void WorldRenderer::MountContentVfs(smvk::OverlayVfs& vfs) const {
	// The renderer's content overlay (see Initialize): loose content/
	// at the bottom, the project root BELOW it (project files win on
	// collisions), the flintbase cache + engine resources on top.
	// Mounts into the CALLER'S vfs (OverlayVfs is non-copyable).
	vfs.Mount(std::make_shared<smvk::LooseBackend>("content"), 0, "content");
	// Project root (editor WYSIWYG, see SetProjectRoot): the whole
	// project tree mounts BELOW the bundled content/ priority so
	// project files win — <root>/content/* shadows the shipped
	// samples, and <root>/gi/* + <root>/paks/* resolve the sidecars
	// the cooker wrote next to the project paks.
	if (!m_projectRoot.empty())
		vfs.Mount(std::make_shared<smvk::LooseBackend>(m_projectRoot),
			-10, "project");
	vfs.Mount(std::make_shared<smvk::FlintbaseBackend>(), 50, "flintbase");
	vfs.Mount(
		std::make_shared<smvk::LooseBackend>("engine_resources"), 100,
		"engine_resources");
}

void WorldRenderer::RebindWorld(smvk::World* world) {
	// IN-PLACE world rebind (hosted live view): swap the borrowed world
	// and rebuild ONLY the world-derived state — the framegraph (every
	// program, atlas, target, buffer, per-view image) persists untouched.
	// Scene opens / reimports / pak switches / GI-bake sidecar refreshes
	// all ride this path; the hosted renderer is NEVER stopped +
	// relaunched for a world change (the old F12 relaunch rebind was
	// retired — relaunching the whole renderer for a world swap was
	// never actually necessary).
	// Callers: SetSceneId/SetProjectRoot FIRST (the GI sidecar loaders
	// resolve gi/<id>.* through the project-aware VFS), then this.
	if (!IsHosted() || !HostedWorldBorrowed() || !world
		|| world == m_world.get())
		return;
	WaitForDeviceIdle(); // frame-boundary contract (same as the launch)
	RebindBorrowedWorld(world);
	std::cerr << "[Scene] live view rebound in place: nodes="
		<< m_world->scene.hierarchy.size() << " meshes="
		<< m_world->meshLocations.size() << " materials="
		<< m_world->materialLocations.size() << std::endl;
	// ---- world-derived state rebuild (the Initialize tail, world-bound) --
	smvk::OverlayVfs contentVfs;
	MountContentVfs(contentVfs);
	// Procedural light-grid fit to the new bounds + far-clip refit (the
	// clip feeds the CSM splits + LOD distances; the hosted lens itself
	// is the host's — the ext camera drive owns the projection).
	{
		const ::Aabb bounds = m_world->ComputeSceneBounds();
		if (bounds.mx.x >= bounds.mn.x && bounds.mx.y >= bounds.mn.y
			&& bounds.mx.z >= bounds.mn.z) {
			m_fpDemo.FitToBounds(bounds.mn, bounds.mx);
			const float radius = glm::length(bounds.mx - bounds.mn) * 0.5f;
			m_sceneFarClip = std::clamp(radius * 4.2f, 50.0f, 10000.0f);
		}
	}
	// Authored punctual lights + named GI sidecar stems + the sidecar
	// loaders (each self-resets, so re-running over the old state is
	// clean) + the single-source degrade pass.
	BuildCookedLightsFromWorld();
	ResolveGiSidecars();
	LoadGiVolume(contentVfs);
#if 0 // Lightmaps deprecated
	LoadGiLightmaps(contentVfs);
#endif
	LoadGiRegularVolumes(contentVfs);
	LoadCvolumes(contentVfs);
	LoadCubemaps(contentVfs);
	DegradeAmbientForMissingSources();
	// Decal state dies with the old world (pick soup + hero presets).
	m_decalPickScene.reset();
	m_decalDemo.pickBvhState = 0; // rebuild lazily on the next crosshair pick
	ClearDecals();
	SpawnDecalPresets();
	// Gfx filter registration over the new meshes (full rebuild; direct
	// draws stay the fallback when registration comes up empty).
	if (m_passes.gfxFilter && m_useGfxFilter) {
		m_gfxRenderer.RegisterWorldMeshes(*m_world);
		m_gfxRenderer.UploadStaticData(*m_context, *m_rendergraph);
		std::cerr << "[Scene] gfx re-registered: clusters="
			<< smvk::gfx::ClusterCount(m_gfxRenderer.State()) << std::endl;
		if (smvk::gfx::ClusterCount(m_gfxRenderer.State()) == 0) {
			std::cerr << "[WorldRenderer] GfxFilter: no clusters - direct draws\n";
			m_useGfxFilter = false;
		}
	}
	// Temporal histories die with the old world.
	m_prevGlobalTransforms.clear();
	m_prevViewProjValid = false;
	m_dirSamplePrevValid = false;
	// The multiview per-view state is world-independent: targets +
	// refresh schedules persist; the next rendered frame re-fits the
	// per-view lights against the new world.
}

void WorldRenderer::EnsureSecondaryTargets(SecondaryView& v, uint32_t vi) {
	// Extent guard: zero/extreme clamps to something renderable.
	smvk::Extent2D ext = v.desc.extent;
	ext.width = std::max(std::min(ext.width, 16384u), 64u);
	ext.height = std::max(std::min(ext.height, 16384u), 64u);
	v.desc.extent = ext;
	// Scene depth (1x; no MSAA on secondary views).
	smvk::DepthDesc depthDesc{};
	depthDesc.format = m_swapchain->GetDepthFormat();
	depthDesc.name = v.Name("viewDepth", 0);
	depthDesc.viewName = v.Name("viewDepthView", 1);
	depthDesc.logicalName = v.Name("viewDepthLogical", 2);
	v.vp.renderDepth.Ensure(*m_context, *m_rendergraph, ext, depthDesc);
	// HDR intermediate (pre-tonemap).
	smvk::HdrDesc hdrDesc{};
	hdrDesc.name = v.Name("viewHdr", 3);
	hdrDesc.viewName = v.Name("viewHdrView", 4);
	hdrDesc.logicalName = v.Name("viewHdrLogical", 5);
	v.vp.hdr.Ensure(*m_context, *m_rendergraph, ext, hdrDesc);
	// Forward+ (tile buffers; sized to the view extent — Ensure is
	// idempotent, but MIXED per-view extents alternate the recreate key
	// and churn recreations every frame; keep the set uniform-sized, or
	// accept the churn while dragging a split).
	m_fpRenderer.Configure(&v.vp.forwardPlus, MakeForwardPlusDesc(),
		m_resources.prepassMaskedProgram);
	m_fpRenderer.ConfigureGbuffer(m_resources.gbufferProgram,
		m_resources.gbufferMaskedProgram);
	m_fpRenderer.EnsureSized(*m_context, *m_rendergraph, ext, v.vp.renderDepth.view);
	// Directional CSM + spot atlases (own fits per view). EVSM stays a
	// MAIN-view opt-in (default off; the ImGui checkbox): the moment
	// atlases are RGBA32F-2xMSAA-per-tile giants (hundreds of MB each;
	// OOM when replicated per view) and the lean secondary path wants
	// depth shadows anyway — the view's light entry +
	// FrameUniforms.evsmMode=0 keep its PCF sampling consistent.
	smvk::CascadedShadowMapDesc shadowDesc =
		MakeShadowDesc(m_cascadeResolution, m_cascadeLambda);
	shadowDesc.name = v.Name("viewShadowAtlas", 8);
	v.vp.directionalShadows.Ensure(*m_context, *m_rendergraph, shadowDesc);
	smvk::SpotShadowMapDesc spotDesc = MakeSpotShadowDesc();
	spotDesc.name = v.Name("viewSpotAtlas", 9);
	v.vp.spotShadows.Ensure(*m_context, *m_rendergraph, spotDesc);
	v.vp.tonemap.Ensure(*m_context, MakeTonemapDesc());
	// Output image: post-tonemap LDR in the hosted surface format (SRGB
	// where the surface is; the UNORM sibling + tonemap manual-encode
	// otherwise). PERSISTS across off-phase frames (recreate key = extent
	// + format) so the host composites continuously.
	const smvk::Format surface = m_swapchain->GetSurfaceFormat();
	smvk::Format outFmt = surface;
	if (surface != smvk::Format::BGRA8_SRGB && surface != smvk::Format::RGBA8_SRGB
		&& surface != smvk::Format::BGRA8_UNorm && surface != smvk::Format::RGBA8_UNorm)
		outFmt = LdrUnormSibling();
	if (v.outValid && v.outExtent.width == ext.width
		&& v.outExtent.height == ext.height && v.outFormat == outFmt)
		return;
	if (!v.outLogical.empty())
		m_rendergraph->FreeImageView(v.outLogical);
	v.outBindlessIndex = UINT32_MAX;
	v.outBindless = {};
	v.outView = {};
	v.outImage = {};
	v.outValid = false;
	smvk::ImageCreateInfo ci{};
	ci.extent = { ext.width, ext.height, 1 };
	ci.format = outFmt;
	ci.usage = smvk::ImageUsage::ColorAttachment | smvk::ImageUsage::Sampled;
	// TransferSrc rides the debug-dump arm (the editor's hosted-view
	// readbacks need it on the source image, same as HdrTarget).
	if (smvk::DebugImageDumpArmed())
		ci.usage |= smvk::ImageUsage::TransferSrc;
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.name = v.Name("viewOut", 6);
	v.outImage = m_context->CreateImage(ci, nullptr);
	if (v.outImage.empty())
		return;
	smvk::ImageViewCreateInfo vci{};
	vci.viewType = smvk::ImageViewType::View2D;
	vci.format = smvk::Format::Undefined; // inherit
	vci.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 };
	vci.name = v.Name("viewOutView", 7);
	v.outView = m_context->CreateImageView(v.outImage, vci);
	if (v.outView.empty()) {
		v.outImage = {};
		return;
	}
	v.outBindless = m_context->CreateBindlessImage(v.outView,
		smvk::ImageLayout::ShaderReadOnlyOptimal);
	if (v.outBindless.empty()) {
		v.outView = {};
		v.outImage = {};
		return;
	}
	v.outBindlessIndex = m_context->GetBindlessIndex(v.outBindless);
	v.outLogical = m_rendergraph->AddExternalImageView(v.outView, outFmt,
		v.Name("viewOutLogical", 12));
	if (v.outLogical.empty()) {
		v.outBindless = {};
		v.outView = {};
		v.outImage = {};
		v.outBindlessIndex = UINT32_MAX;
		return;
	}
	v.outExtent = ext;
	v.outFormat = outFmt;
	v.outValid = true;
}

FrameUniforms WorldRenderer::BuildSecondaryFrameUniforms(SecondaryView& v,
	uint32_t vi, uint32_t frameIdx, const smvk::EnvironmentSettings& env)
{
	// Lean per-view twin of UpdateFrameUniforms: the view's own camera
	// (positioner + Camera, aspect from desc.extent), no jitter (secondary
	// views run no T2x/DLSS/motion chain), no AO (no per-view AO passes),
	// deferred shadows off (the lit shader's inline forward+ shadow path
	// reads the view's own tile lists). Everything view-independent mirrors
	// the main builder exactly (env/ambient/GI/lightmaps/volumes/decals).
	// Ortho lens (desc.orthoHalfH > 0): TRUE orthographic, matching the
	// editor's Top/Side cameras (parallel lines stay parallel).
	if (v.desc.orthoHalfH > 0.0f)
		v.camera.setOrtho3D(v.desc.orthoHalfH,
			float(v.desc.extent.width) / float(v.desc.extent.height),
			v.desc.zNear, v.desc.zFar);
	else
		v.camera.setPerspective(glm::radians(v.desc.fovYDeg),
			float(v.desc.extent.width) / float(v.desc.extent.height),
			v.desc.zNear, v.desc.zFar);
	const auto view = v.camera.getViewMatrix();
	auto proj = v.camera.getProjMatrix();
	proj[1][1] *= -1; // Vulkan Y-flip (UpdateFrameUniforms parity)
	const auto viewProj = proj * view;
	FrameUniforms data{};
	data.cameraPos = v.positioner.cameraPosition_;
	data.view = view;
	data.proj = proj;
	data.viewProj = viewProj;
	data.invView = glm::inverse(view);
	data.invProj = glm::inverse(proj);
	data.invViewProj = glm::inverse(viewProj);
	// No motion-vector pass per view; keep the per-view history anyway so
	// a future per-view motion chain plugs in without a layout change.
	data.prevViewProj = v.prevViewProjValid ? v.prevViewProj : viewProj;
	v.prevViewProj = viewProj;
	v.prevViewProjValid = true;
	const float width_ = (float)v.desc.extent.width;
	const float height_ = (float)v.desc.extent.height;
	data.time = time_since_start_s;
	data.viewportSize = { width_, height_ };
	data.invViewportSize = { 1.0f / width_, 1.0f / height_ };
	data.nearPlane = v.desc.zNear;
	data.farPlane = v.desc.zFar;
	data.exposure = FrameExposure();
	// No per-view AO passes: ambient composites without the AO layer.
	data.aoTextureIndex = UINT32_MAX;
	data.aoIntensity = 0.0f;
	// Same scene-built look as the main view.
	data.env = env.ToFrameData();
	data.env.fogEndAndFlags.w = IblWanted() ? 1.0f : 0.0f;
	// Reflection cubemap (global fallback) + probe tile freshness for the
	// VIEW's forward+ tile lists (the per-view cull files them).
	data.cubemapIndex = UINT32_MAX;
	data.cubemapSampler = m_cubemapSampler;
	data.cubemapIntensity = m_cubemapEnabled ? m_cubemapIntensity : 0.0f;
	data.cubemapDebug = static_cast<uint32_t>(std::clamp(m_cubemapDebug, 0, 2));
	data.cubemapLodBias = m_cubemapLodBias;
	data.probeTileFresh = ProbeTileListsWanted() ? 1u : 0u;
	data.probeEntries = ProbeEntryCount();
	data.probePad_ = 0;
	if (m_cubemapEnabled && !m_cubemaps.entries.empty()) {
		for (const auto& [name, entry] : m_cubemaps.entries) {
			if (entry.bindless >= 0) {
				data.cubemapIndex = static_cast<uint32_t>(entry.bindless);
				break;
			}
		}
	}
	// Deferred shadows off per view (inline forward+ shadow path; the DS
	// buffers + phase-1 upload are main-view-only).
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
	// Ambient sources (per-source isolation mirrors the main builder).
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
	data.giIntensity = m_giIntensity;
	data.giDebug = uint32_t(std::clamp(m_giDebug, 0, 2)) & 0xffu;
#if 0 // Lightmaps deprecated
	if (LmActive()) {
		for (uint32_t t = 0; t < 12; ++t)
			data.lmLayers[t] = m_lmLayerSlots[t];
		data.lmLayerKind = m_lmLayerKind;
		data.lmEnabled = 1u;
	} else {
		for (uint32_t t = 0; t < 12; ++t)
			data.lmLayers[t] = UINT32_MAX;
		data.lmLayerKind = 0u;
		data.lmEnabled = 0u;
	}
	data.lmPad_[0] = 0;
	data.lmPad_[1] = 0;
#endif
	if (RegActive()) {
		data.regObbs = m_regObbsAddress;
		const uint32_t n = std::min<uint32_t>(uint32_t(m_regVolumes.size()), 8u);
		for (uint32_t rv = 0; rv < n; ++rv)
			for (uint32_t t = 0; t < 7; ++t)
				data.regLayers[rv * 7u + t] = m_regVolumes[rv].layerSlots[t];
		data.regCount = n;
		data.regEnabled = RegWanted() ? 1u : 0u;
	} else {
		data.regObbs = 0;
		for (uint32_t& slot : data.regLayers)
			slot = UINT32_MAX;
		data.regCount = 0u;
		data.regEnabled = 0u;
	}
	if (m_cvolLoaded && m_cvolEnabled) {
		data.cvolVolumes = m_cvolVolumesAddress;
		const uint32_t n = std::min<uint32_t>(uint32_t(m_cvolVolumes.size()), 8u);
		for (uint32_t cv = 0; cv < n; ++cv)
			for (uint32_t t = 0; t < 7; ++t)
				data.cvolLayers[cv * 7u + t] = m_cvolVolumes[cv].layerSlots[t];
		data.cvolCount = n;
		data.cvolEnabled = ProbesWanted() ? 1u : 0u;
	} else {
		data.cvolVolumes = 0;
		for (uint32_t& slot : data.cvolLayers)
			slot = UINT32_MAX;
		data.cvolCount = 0u;
		data.cvolEnabled = 0u;
	}
	data.cvolPad_[0] = 0;
	data.cvolPad_[1] = 0;
	data.cvolPad_[2] = 0;
	data.cvolPad_[3] = 0;
	// EVSM stays a main-view opt-in: the view's light entries address its
	// PCF depth atlases (see UpdateSecondaryLights), so its uniforms
	// publish the PCF interpretation.
	data.evsmMode = 0u;
	// Decals: the global placement ring + the VIEW's forward+ tile-list
	// BDAs (the per-view tile cull refreshes them — contract preserved:
	// decalCount > 0 only when the view's cull dispatch runs).
	if (v.vp.forwardPlus.Valid()) {
		data.decalTileCounts = v.vp.forwardPlus.DecalTileCountsAddress(*m_context);
		data.decalTileIndices = v.vp.forwardPlus.DecalTileIndicesAddress(*m_context);
		data.decalMaxPerTile = v.vp.forwardPlus.desc.maxDecalsPerTile;
	}
	if (DecalsActive() && m_decalFrameCount > 0
		&& !m_resources.decalsBuffer.empty()) {
		constexpr size_t DECAL_FRAME_STRIDE =
			sizeof(smvk::ForwardPlusDecal) * FP_MAX_DECALS;
		data.decals = m_context->GetBufferDeviceAddress(m_resources.decalsBuffer)
			+ frameIdx * DECAL_FRAME_STRIDE;
		data.decalCount = std::min(m_decalFrameCount, FP_MAX_DECALS);
	} else {
		data.decals = 0;
		data.decalCount = 0;
	}
	// This view's FramePSHot slab BDA (the FS reaches the hot struct through
	// FrameUniforms; same slab math as the hot upload below).
	data.frameHot = m_context->GetBufferDeviceAddress(m_resources.frameHot)
		+ ViewSlabOffset(1u + vi, frameIdx, sizeof(FramePSHot));
	// View-major slab upload (view 0 = the main view's slice).
	static constexpr smvk::PipelineStage kFrameUploadStages =
		smvk::PipelineStage(smvk::PipelineStageBit_Vertex
			| smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute);
	m_context->UploadDataToBuffer(
		m_resources.frameUniforms,
		&data,
		sizeof(FrameUniforms),
		kFrameUploadStages,
		smvk::AccessFlags::ShaderRead,
		0,
		ViewSlabOffset(1u + vi, frameIdx, sizeof(FrameUniforms))
	);

	// Upload FramePSHot for secondary view.
	FramePSHot hot{};
	hot.cameraPos = data.cameraPos;
	hot.time = data.time;
	hot.viewportSize = data.viewportSize;
	hot.invViewportSize = data.invViewportSize;
	hot.nearPlane = data.nearPlane;
	hot.farPlane = data.farPlane;
	hot.exposure = data.exposure;
	hot.aoTextureIndex = data.aoTextureIndex;
	hot.aoIntensity = data.aoIntensity;
	hot.evsmMode = data.evsmMode;
	hot.dsEnabled = data.dsEnabled;
	hot.dsTileCountX = data.dsTileCountX;
	hot.fogEndAndFlags_w = data.env.fogEndAndFlags.w;
	hot.giIntensity = data.giIntensity;
	hot.giDebug = data.giDebug;
	hot.cvolEnabled = data.cvolEnabled;
	hot.pad_[0] = 0;
	hot.pad_[1] = 0;

	m_context->UploadDataToBuffer(
		m_resources.frameHot,
		&hot,
		sizeof(FramePSHot),
		kFrameUploadStages,
		smvk::AccessFlags::ShaderRead,
		0,
		ViewSlabOffset(1u + vi, frameIdx, sizeof(FramePSHot))
	);

	return data;
}

void WorldRenderer::UpdateSecondaryLights(SecondaryView& v, uint32_t vi,
	uint32_t frameIdx, const smvk::EnvironmentSettings& env)
{
	// Per-view twin of UpdateLights: CSM fitted to the VIEW camera, the
	// sun's shadow index addressing the VIEW's atlas, uploads into the
	// view-major lights + cascade slabs. Same scene-built sun as the
	// main fit.
	glm::vec3 toSun = env.SunDirectionToSun();
	glm::vec3 lightDir = -toSun;
	auto cameraView = v.camera.getViewMatrix();
	auto cameraProj = v.camera.getProjMatrix();
	// Y-flip parity with the main fit (UpdateLights).
	cameraProj[1][1] *= -1.0f;
	smvk::Light lights[MAX_LIGHTS]{};
	smvk::Light& light = lights[0];
	light.color = glm::vec4(env.sunColor, 1.0f);
	light.direction[0] = lightDir.x;
	light.direction[1] = lightDir.y;
	light.direction[2] = lightDir.z;
	light.intensity = env.sunIntensity;
	// PCF depth atlas only per view (EVSM is a main-view opt-in; see
	// EnsureSecondaryTargets) — FrameUniforms.evsmMode=0 keeps the view's
	// sampling interpretation consistent.
	light.shadow_map_index =
		m_context->GetBindlessIndex(v.vp.directionalShadows.bindless);
	auto invView = glm::inverse(cameraView);
	auto invProj = glm::inverse(cameraProj);
	// Split far plane: the view's far clip, capped like the main fit
	// (CascadeFar: [near + 1, scene far]).
	const float splitFar = std::clamp(v.desc.zFar,
		v.desc.zNear + 1.0f, m_sceneFarClip);
	auto splits = smvk::ComputeCascadeSplits(v.desc.zNear, splitFar,
		m_cascadeLambda, NUM_CASCADES);
	smvk::GpuDirectionalLightDescription dir =
		v.vp.directionalShadows.BuildDirectionalLight(
			lightDir,
			light.color,
			light.intensity,
			light.shadow_map_index,
			invView,
			invProj,
			splits,
			CASCADE_Z_PADDING,
			std::tan(glm::radians(env.sunDiscSizeDeg)),
			m_evsmSoftness);
	for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
		dir.cascades[i].bias *= m_cascadeDepthBiasScale;
		dir.cascades[i].normalBias *= m_cascadeNormalBiasScale;
	}
	std::memcpy(light.lightVP, dir.lightVP, sizeof(light.lightVP));
	constexpr size_t LIGHT_FRAME_STRIDE = sizeof(smvk::Light) * MAX_LIGHTS;
	m_context->UploadDataToBuffer(
		m_resources.lightsBuffer,
		lights,
		sizeof(lights),
		smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment
			| smvk::PipelineStageBit_Compute,
		smvk::AccessFlags::ShaderRead,
		0,
		ViewSlabOffset(1u + vi, frameIdx, LIGHT_FRAME_STRIDE)
	);
	constexpr size_t DIR_STRIDE = sizeof(smvk::GpuDirectionalLightDescription);
	// PCF only per view: the sampling descriptors are always the CURRENT
	// fit (no EVSM OneFrameLag trailing copy per view).
	m_context->UploadDataToBuffer(
		m_resources.directionalCascadeBuffer,
		&dir,
		sizeof(dir),
		smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment
			| smvk::PipelineStageBit_Compute,
		smvk::AccessFlags::ShaderRead,
		0,
		ViewSlabOffset(1u + vi, frameIdx, DIR_STRIDE)
	);
	v.dirLight = dir;
	v.dirLightValid = true;
}

void WorldRenderer::UploadSecondaryForwardPlusLights(SecondaryView& v,
	uint32_t vi, uint32_t frameIdx)
{
	// Per-view twin of UpdateForwardPlusLights' SELECTION + UPLOAD legs
	// (the light SET is shared — m_frameLights, captured by the main view
	// after its DS sort). Each view copies it, runs its own spot-shadow
	// selection against ITS camera (patching shadowTile/shadowMapIndex
	// for ITS atlas) and uploads into its view-major slabs.
	v.spotShadowCount = 0;
	v.spotShadowMapIndex = -1;
	v.spotShadowTiles.clear();
	v.lightScratch = m_frameLights;
	if (m_passes.spotShadows && m_fpDemo.spotShadowsEnabled
		&& v.vp.spotShadows.Valid() && !v.lightScratch.empty()) {
		const uint32_t wantMax = (uint32_t)std::clamp(m_fpDemo.spotShadowMax,
			0, (int)smvk::kMaxSpotShadows);
		const uint32_t budget = std::min(wantMax, v.vp.spotShadows.slotCount);
		if (budget > 0) {
			const float proj11 = std::abs(v.frameData.proj[1][1]);
			auto selected = smvk::SelectSpotShadowCasters(
				v.lightScratch, v.frameData.viewProj, v.frameData.cameraPos,
				m_fpDemo.spotShadowMaxDistance, budget, proj11);
			const int32_t atlasIdx = v.vp.spotShadows.BindlessIndex(*m_context);
			// PCF depth atlas only per view (EVSM is a main-view opt-in).
			const int32_t sampleIdx = atlasIdx;
			if (atlasIdx >= 0 && !selected.empty()) {
				std::vector<smvk::GpuCascadeDescription> tiles(selected.size());
				for (size_t k = 0; k < selected.size(); ++k) {
					smvk::ForwardPlusLight& sl =
						v.lightScratch[selected[k].lightIndex];
					tiles[k] = v.vp.spotShadows.BuildSpotTile(
						glm::vec3(sl.position[0], sl.position[1], sl.position[2]),
						glm::normalize(glm::vec3(sl.direction[0], sl.direction[1],
							sl.direction[2])),
						sl.outerAngle, sl.range, atlasIdx, (uint32_t)k,
						SPOT_SHADOW_NEAR, SPOT_SHADOW_BIAS,
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
					smvk::PipelineStage(smvk::PipelineStageBit_Vertex
						| smvk::PipelineStageBit_Fragment),
					smvk::AccessFlags::ShaderRead,
					0,
					ViewSlabOffset(1u + vi, frameIdx, SPOT_FRAME_STRIDE)
				);
				v.spotShadowCount = (uint32_t)selected.size();
				v.spotShadowMapIndex = sampleIdx;
				v.spotShadowTiles = std::move(tiles);
			}
		}
	}
	constexpr size_t FP_FRAME_STRIDE =
		sizeof(smvk::ForwardPlusLight) * FP_MAX_LIGHTS;
	if (!v.lightScratch.empty()) {
		m_context->UploadDataToBuffer(
			m_resources.fpLightsBuffer,
			v.lightScratch.data(),
			sizeof(smvk::ForwardPlusLight) * v.lightScratch.size(),
			smvk::PipelineStage(smvk::PipelineStageBit_Vertex
				| smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute),
			smvk::AccessFlags::ShaderRead,
			0,
			ViewSlabOffset(1u + vi, frameIdx, FP_FRAME_STRIDE)
		);
	}
	// Reflection probes as FP light entries (type kForwardPlusProbe) — the
	// same widest-first tail, uploaded into the VIEW's slab so the per-view
	// tile cull files them into the view's tile lists.
	{
		const uint32_t probeEntries = ProbeEntryCount();
		if (probeEntries > 0
			&& v.lightScratch.size() + probeEntries <= FP_MAX_LIGHTS) {
			std::vector<smvk::ForwardPlusLight> gpu;
			gpu.reserve(probeEntries);
			for (uint32_t i = 0; i < probeEntries; ++i) {
				const CubemapProbeGpu& pr = m_cubemapProbes[i];
				gpu.push_back(smvk::MakeProbeLightEntry(
					glm::vec3(pr.posInfluence.x, pr.posInfluence.y,
						pr.posInfluence.z),
					pr.posInfluence.w, pr.fadeCubemap.x,
					static_cast<uint32_t>(pr.fadeCubemap.y),
					static_cast<uint32_t>(pr.fadeCubemap.z)));
			}
			m_context->UploadDataToBuffer(
				m_resources.fpLightsBuffer,
				gpu.data(),
				sizeof(smvk::ForwardPlusLight) * gpu.size(),
				smvk::PipelineStage(smvk::PipelineStageBit_Vertex
					| smvk::PipelineStageBit_Fragment
					| smvk::PipelineStageBit_Compute),
				smvk::AccessFlags::ShaderRead,
				0,
				ViewSlabOffset(1u + vi, frameIdx, FP_FRAME_STRIDE)
					+ v.lightScratch.size() * sizeof(smvk::ForwardPlusLight)
			);
		}
	}
}

void WorldRenderer::PrepareSecondaryViews(uint32_t frameIdx, const smvk::EnvironmentSettings& env) {
	// Frame-top per-view prep for every view due THIS frame (phase gate):
	// targets + lean FrameUniforms + per-view CSM fit/spot selection + the
	// view-major slab uploads. Off-phase views are skipped entirely (their
	// images persist); RenderFrame allocates their RenderViews only when
	// dirLightValid (i.e. prepared this frame — the flag is CLEARED on
	// every skip so a stale fit can never ride a later frame's slab).
	if (!IsHosted())
		return;
	static const bool mvTrace = std::getenv("VULKANTOY_MULTIVIEW_TRACE") != nullptr;
	if (mvTrace) {
		std::fprintf(stderr, "[MV] frame=%llu phase=%u views=%zu\n",
			(unsigned long long)m_frameCounter, m_viewPhase, m_views.size());
	}
	for (uint32_t vi = 0; vi < (uint32_t)m_views.size(); ++vi) {
		SecondaryView& v = m_views[vi];
		if (!v.desc.enabled || !ViewRefreshDue(v.desc)) {
			v.renderedLastFrame = false;
			v.dirLightValid = false; // prepared-this-frame flag
			if (mvTrace) {
				std::fprintf(stderr,
					"[MV] view %u skipped (enabled=%d refresh=%u phase=%u)\n",
					vi + 1u, (int)v.desc.enabled,
					(unsigned)v.desc.refresh, m_viewPhase);
			}
			continue;
		}
		EnsureSecondaryTargets(v, vi);
		if (!v.vp.renderDepth.Valid() || !v.vp.hdr.Valid() || !v.outValid) {
			v.renderedLastFrame = false;
			v.dirLightValid = false;
			std::fprintf(stderr,
				"[MV] view %u target creation failed (depth=%d hdr=%d out=%d) "
				"— frame skipped\n",
				vi + 1u, (int)v.vp.renderDepth.Valid(), (int)v.vp.hdr.Valid(),
				(int)v.outValid);
			continue; // target creation failed — skip the frame for this view
		}
		v.frameData = BuildSecondaryFrameUniforms(v, vi, frameIdx, env);
		UpdateSecondaryLights(v, vi, frameIdx, env);
		UploadSecondaryForwardPlusLights(v, vi, frameIdx);
		v.renderedLastFrame = true;
		if (mvTrace) {
			std::fprintf(stderr,
				"[MV] view %u prepared %ux%u eye=(%.1f,%.1f,%.1f) "
				"spots=%u lights=%zu\n",
				vi + 1u, v.desc.extent.width, v.desc.extent.height,
				v.desc.eye.x, v.desc.eye.y, v.desc.eye.z,
				v.spotShadowCount, v.lightScratch.size());
		}
	}
}

void WorldRenderer::RenderSecondaryViews(smvk::Framebuffer& fb) {
	// Per-view pass emission (after the main view's post/UI, before the
	// shadow-futures join): shadows fitted to the view camera, Forward+
	// prepass + tile cull, direct-draw Main, tonemap into the view image.
	// Each pass targets the VIEW's targets; shared state (light set,
	// RenderView slab, decal ring, materials) rides the per-view push
	// constants' view-major addresses.
	if (!IsHosted())
		return;
	const uint32_t activeViews = (uint32_t)m_views.size();
	if (activeViews == 0)
		return;
	constexpr size_t LIGHT_FRAME_STRIDE = sizeof(smvk::Light) * MAX_LIGHTS;
	constexpr size_t DIR_FRAME_STRIDE =
		sizeof(smvk::GpuDirectionalLightDescription);
	constexpr size_t FP_LIGHT_FRAME_STRIDE =
		sizeof(smvk::ForwardPlusLight) * FP_MAX_LIGHTS;
	constexpr size_t SPOT_FRAME_STRIDE =
		sizeof(smvk::GpuCascadeDescription) * smvk::kMaxSpotShadows;
	const uint64_t frameUniformsBase =
		m_context->GetBufferDeviceAddress(m_resources.frameUniforms);
	const uint64_t lightsBase =
		m_context->GetBufferDeviceAddress(m_resources.lightsBuffer);
	const uint64_t cascadeBase =
		m_context->GetBufferDeviceAddress(m_resources.directionalCascadeBuffer);
	const uint64_t fpLightsBase =
		m_context->GetBufferDeviceAddress(m_resources.fpLightsBuffer);
	const uint64_t spotBase =
		m_context->GetBufferDeviceAddress(m_resources.spotShadowBuffer);
	const uint64_t renderViewsAddress =
		m_renderViews.DeviceAddress(*m_context, fb.frameIndex);
	const uint32_t linearSampler = m_context->GetOrCreateSampler(
		smvk::SamplerCreateInfo{});
	const float frameExposure = FrameExposure();
	const char* cascadeNames[4] = { "ViewShadowCascade0", "ViewShadowCascade1",
		"ViewShadowCascade2", "ViewShadowCascade3" };
	const char* spotNames[8] = { "ViewSpotShadow0", "ViewSpotShadow1",
		"ViewSpotShadow2", "ViewSpotShadow3", "ViewSpotShadow4",
		"ViewSpotShadow5", "ViewSpotShadow6", "ViewSpotShadow7" };
	const uint64_t materialsBase =
		m_context->GetBufferDeviceAddress(m_resources.materialsBuffer);
	// One COMPLETE per-view push (view-major slab addresses + the view's
	// tile/spot selection) — the shadow + scene phases derive from it by
	// copy exactly like the main view's cascadePC/spotPC.
	auto makeViewPc = [&](uint32_t vi) {
		const SecondaryView& v = m_views[vi];
		const uint32_t lightCount = (uint32_t)v.lightScratch.size();
		PushConstants viewPc{};
		viewPc.frameUniforms = frameUniformsBase
			+ ViewSlabOffset(1u + vi, fb.frameIndex, sizeof(FrameUniforms));
		viewPc.lightsBuffer = lightsBase
			+ ViewSlabOffset(1u + vi, fb.frameIndex, LIGHT_FRAME_STRIDE);
		viewPc.materialsBuffer = materialsBase
			+ fb.frameIndex * sizeof(smvk::MaterialValues) * MAX_MATERIALS;
		viewPc.cascadeBuffer = cascadeBase
			+ ViewSlabOffset(1u + vi, fb.frameIndex, DIR_FRAME_STRIDE);
		viewPc.materialId = 0;
		viewPc.comparisonSamplerId = 1;
		viewPc.numCascades = NUM_CASCADES;
		viewPc.fpLights = fpLightsBase
			+ ViewSlabOffset(1u + vi, fb.frameIndex, FP_LIGHT_FRAME_STRIDE);
		viewPc.fpTileCounts = v.vp.forwardPlus.TileCountsAddress(*m_context);
		viewPc.fpTileIndices = v.vp.forwardPlus.TileIndicesAddress(*m_context);
		viewPc.fpTileCountX = v.vp.forwardPlus.tileCountX;
		viewPc.fpLightCount = lightCount;
		viewPc.fpTileSize = FP_TILE_SIZE;
		viewPc.fpMaxPerTile = FP_MAX_PER_TILE;
		viewPc.spotShadowBuffer = spotBase
			+ ViewSlabOffset(1u + vi, fb.frameIndex, SPOT_FRAME_STRIDE);
		viewPc.spotShadowMapIndex = v.spotShadowMapIndex >= 0
			? (uint32_t)v.spotShadowMapIndex : UINT32_MAX;
		viewPc.spotShadowCount = v.spotShadowCount;
		viewPc.spotIndex = UINT32_MAX;
		viewPc.pad_pc = (m_transparentMode == smvk::TransparentMode::Blended)
			? 1u : 0u;
		viewPc.renderViews = renderViewsAddress;
		viewPc.mainRenderView = v.mainViewIdx;
		viewPc.renderViewIndex = v.mainViewIdx;
		return viewPc;
	};
	// Phase 1 — per-view shadow passes (PCF depth tiles only; EVSM is a
	// main-view opt-in, so no moment/blur passes exist on this path).
	for (uint32_t vi = 0; vi < activeViews; ++vi) {
		SecondaryView& v = m_views[vi];
		if (!v.desc.enabled || !v.dirLightValid || !v.outValid)
			continue;
		const PushConstants viewPc = makeViewPc(vi);
		if (m_passes.directionalShadows && v.vp.directionalShadows.Valid()) {
			for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci) {
				PushConstants cascadePC = viewPc;
				cascadePC.cascadeIndex = ci;
				const uint32_t casterViewIndex =
					v.cascadeViewBase != smvk::RenderViewSet::kInvalidIndex
					? v.cascadeViewBase + ci
					: smvk::RenderViewSet::kInvalidIndex;
				cascadePC.renderViewIndex = casterViewIndex;
				auto& shadowPass = v.vp.directionalShadows.AddCascadePass(
					*m_rendergraph, ci, cascadeNames[ci]);
				shadowPass.SetRecordingFunc(
					[this, cascadePC, casterViewIndex, vi](smvk::CommandBuffer& cmd) {
					RecordShadowCastersDirect(cmd,
						m_views[vi].vp.directionalShadows.program,
						cascadePC, casterViewIndex, /*skipFiltered=*/false);
				});
			}
		}
		// Spot tiles (per-view selection; direct casters, no filter).
		if (m_passes.spotShadows && v.spotShadowCount > 0
			&& v.vp.spotShadows.Valid()) {
			const uint32_t spotCount = std::min(v.spotShadowCount,
				v.vp.spotShadows.slotCount);
			for (uint32_t si = 0; si < spotCount; ++si) {
				PushConstants spotPC = viewPc;
				spotPC.spotIndex = si;
				const uint32_t casterViewIndex =
					v.spotViewBase != smvk::RenderViewSet::kInvalidIndex
					? v.spotViewBase + si
					: smvk::RenderViewSet::kInvalidIndex;
				spotPC.renderViewIndex = casterViewIndex;
				auto& spotPass = v.vp.spotShadows.AddSpotPass(
					*m_rendergraph, si, spotNames[si]);
				spotPass.SetRecordingFunc(
					[this, spotPC, casterViewIndex, vi](smvk::CommandBuffer& cmd) {
					RecordShadowCastersDirect(cmd,
						m_views[vi].vp.spotShadows.program,
						spotPC, casterViewIndex, /*skipFiltered=*/false);
				});
			}
		}
	}
	// Phase 3 — per-view scene passes: Forward+ prepass + tile cull, Main,
	// tonemap into the view image.
	for (uint32_t vi = 0; vi < activeViews; ++vi) {
		SecondaryView& v = m_views[vi];
		if (!v.desc.enabled || !v.dirLightValid || !v.outValid
			|| v.mainViewIdx == smvk::RenderViewSet::kInvalidIndex)
			continue;
		const uint32_t lightCount = (uint32_t)v.lightScratch.size();
		const PushConstants pc = makeViewPc(vi);
		// Forward+ prepass + tile cull (the renderer is re-configured to
		// the VIEW's builder + depth; EnsureSized is idempotent per view).
		if (m_passes.forwardPlusPrepass || m_passes.forwardPlusCull) {
			AppFrameContext vfc{};
			vfc.ctx = m_context.get();
			vfc.rg = m_rendergraph.get();
			vfc.world = m_world.get();
			vfc.views = &m_renderViews;
			vfc.frameIndex = fb.frameIndex;
			vfc.pc = pc;
			vfc.mainViewIndex = v.mainViewIdx;
			vfc.eye = v.positioner.cameraPosition_;
			vfc.extent = v.desc.extent;
			vfc.depthView = v.vp.renderDepth.logicalView;
			vfc.depthViewHandle = v.vp.renderDepth.view;
			vfc.fpLights = m_resources.fpLightsLogical;
			vfc.fpLightCount = lightCount;
			vfc.decals = m_resources.decalsLogical;
			vfc.decalCount = m_decalFrameCount;
			vfc.lodEnabled = m_lodEnabled;
			vfc.lodInputs = m_lodFrameInputs; // main-view decisions (shared cache)
			vfc.lodShaderEnabled = ShaderLodActive();
			vfc.ditheredTransparency =
				(m_transparentMode == smvk::TransparentMode::Dithered);
			m_fpRenderer.Configure(&v.vp.forwardPlus, MakeForwardPlusDesc(),
				m_resources.prepassMaskedProgram);
			m_fpRenderer.ConfigureGbuffer(m_resources.gbufferProgram,
				m_resources.gbufferMaskedProgram);
			m_fpRenderer.EnsureSized(*m_context, *m_rendergraph,
				v.desc.extent, v.vp.renderDepth.view);
			if (m_passes.forwardPlusPrepass)
				m_fpRenderer.AppendPrepass(vfc);
			if (m_passes.forwardPlusCull)
				m_fpRenderer.AppendTileCull(vfc, lightCount + ProbeEntryCount());
		}
		// Main (lean direct-draw path: sky + opaque + masked + transparent,
		// frustum-culled against the view's RenderView; wireframe honored).
		const smvk::RenderView cullView = m_renderViews.Get(v.mainViewIdx);
		std::vector<smvk::LogicalImageViewHandle> mainInputs;
		mainInputs.push_back(v.vp.directionalShadows.logicalView);
		if (v.vp.spotShadows.Valid())
			mainInputs.push_back(v.vp.spotShadows.logicalView);
		auto& mainPass = m_rendergraph->AddPass("ViewMain")
			.SetOffset(0, 0)
			.SetInputImages(mainInputs)
			.SetExtent(v.desc.extent);
		mainPass.SetColor({ v.vp.hdr.logicalView },
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0, 0, 0, 1))
			// Depth was filled by the view's Forward+ prepass (Clear when
			// the prepass is off — LoadOp::DontCare would leave garbage).
			.SetDepth(v.vp.renderDepth.logicalView,
				m_passes.forwardPlusPrepass ? smvk::AttachmentLoadOp::Load
					: smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store,
				smvk::ClearDepthStencil(0, 0));
		mainPass.SetRecordingFunc(
			[this, pc, cullView, eye = v.frameData.cameraPos](smvk::CommandBuffer& cmd) {
			RecordSecondaryMain(cmd, pc, cullView, eye);
		});
		// Tonemap -> the view's own output image (persists across
		// off-phase frames; the host composites it every frame).
		{
			smvk::TonemapDesc td = v.vp.tonemap.desc;
			td.exposure = frameExposure;
			td.tonemapMode = m_tonemapMode;
			const bool lutOn = !m_lutFile.empty()
				&& m_lutBindless != UINT32_MAX && m_lutIntensity > 0.0f;
			auto& tonemapPass = v.vp.tonemap.AddTonemapPass(
				*m_rendergraph,
				v.vp.hdr.logicalView,
				v.vp.hdr.bindlessIndex,
				smvk::LogicalImageViewHandle{},
				UINT32_MAX,
				0.0f,
				linearSampler,
				{ v.outLogical },
				v.outExtent,
				td,
				"ViewTonemap",
				smvk::LogicalImageViewHandle{},
				UINT32_MAX,
				0u,
				0.0f,
				{},
				UINT32_MAX,
				0.0f,
				!SwapchainIsSrgb(),
				lutOn ? m_lutBindless : UINT32_MAX,
				m_lutIntensity,
				0.0f,
				0.0f);
			(void)tonemapPass;
		}
		// Output export transition: the host samples this image (bindless)
		// AFTER this submit — leave it in SHADER_READ (the graph emits
		// the tonemap write -> sampled-read barrier from the pass above;
		// the next rendered frame's tonemap transitions back). Without
		// this the image sits in COLOR_ATTACHMENT and every host sample
		// trips VUID-vkCmdDraw-None-09600 (layout mismatch). Barrier-only
		// compute pass (no dispatch): stays in the graphics stream so it
		// orders right after the tonemap; the dst stage names the real
		// (fragment) consumer for a strict write->read edge.
		{
			smvk::ImageSyncRequest outRead{};
			outRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
			outRead.barrierMasks.stageMask = smvk::PipelineStageBit_Fragment;
			outRead.layout = smvk::ImageLayout::ShaderReadOnlyOptimal;
			auto& exportPass = m_rendergraph->AddComputePass("ViewOutExport");
			exportPass.AddImageHint(v.outLogical, outRead);
			exportPass.SetRecordingFunc([](smvk::CommandBuffer&) {});
		}
	}
}

void WorldRenderer::RecordSecondaryMain(smvk::CommandBuffer& cmd,
	const PushConstants& pc, const smvk::RenderView& cullView,
	const glm::vec3& viewEye)
{
	// Lean Main twin (direct draws — no GPU filter, no MSAA, no debug-view
	// swaps): skydome, opaque + MASK cutout (frustum-culled against the
	// view), transparent (dithered/blended like the main view). LOD
	// decisions reuse the main view's cache (World halfrate, per frameId).
	const smvk::DepthSettings opaqueDepth{
		.depthFunc = smvk::CompareOp::GreaterEqual,
		.writeEnable = false, // prepass owns depth (read-only Main, 1x)
		.depthTestEnable = true,
	};
	const smvk::DepthSettings transparentDepth{
		.depthFunc = smvk::CompareOp::GreaterEqual,
		.writeEnable = false,
		.depthTestEnable = true,
	};
	const smvk::StencilSettings noStencil{};
	const smvk::MSAASettings msaaSettings = smvk::MSAASettings::Count1();
	auto wireframeRaster = [&](smvk::RasterSettings r) {
		if (m_wireframe) {
			r.polygonMode = smvk::PolygonMode::Line;
			r.cullMode = smvk::CullMode::None;
		}
		return r;
	};
	auto bindForMaterial = [&](uint32_t materialId, smvk::ShaderProgram& prog,
			const smvk::DepthSettings& depth) {
		smvk::BlendSettings blend = smvk::BlendSettings::Opaque();
		smvk::RasterSettings raster{};
		if (smvk::Material* mat = m_world->GetMaterial(materialId)) {
			blend = mat->pipeline.blend;
			raster = mat->pipeline.raster;
		}
		smvk::ShaderProgram& bound =
			(m_wireframe && m_resources.mainWireProgram.shaderCount == 2)
			? m_resources.mainWireProgram : prog;
		cmd.BindRenderPipeline(
			bound,
			smvk::VertexFormat::StaticVertexFormat,
			depth,
			noStencil,
			blend,
			msaaSettings,
			wireframeRaster(raster)
		);
	};
	auto drawNode = [&](NodeId node, uint32_t meshId, uint32_t materialId) {
		auto& model = m_world->scene.globalTransform[node];
		smvk::StaticMesh* meshDesc = LodMeshForNode(node, meshId);
		if (!meshDesc)
			return;
		PushConstants objPC = pc;
		objPC.materialId = materialId;
		std::copy_n(glm::value_ptr(model), 16, objPC.model);
		cmd.PushConstants(0, sizeof(PushConstants), &objPC, smvk::ShaderStage::All);
		for (auto& draw_set : meshDesc->geometry.surfaces) {
			cmd.DrawIndexed(draw_set.indexCount, 1,
				draw_set.firstIndex, draw_set.baseVertex);
		}
	};
	auto itemVisible = [&](const smvk::World::RenderItem& item) {
		const glm::mat4& model = m_world->scene.globalTransform[item.node];
		const uint32_t lodIdx = LodIndexForNode(item.node, item.mesh);
		smvk::StaticMesh* desc = m_world->GetMesh(item.mesh);
		if (!desc)
			return true; // unknown bounds: draw (conservative)
		glm::vec3 c{ 0.0f };
		float r = 0.0f;
		smvk::MeshBoundsToWorld(model, desc->geometry.aabbMin,
			desc->geometry.aabbMax, c, r);
		if (smvk::RenderViewIntersectsSphere(cullView, c, r))
			return true;
		// Conservative LOD union (matches CollectTransparentDraws): keep
		// when the selected sibling pokes into the frustum.
		smvk::StaticMesh* sel = m_world->GetLodMesh(item.mesh, lodIdx);
		if (sel && sel != desc) {
			smvk::MeshBoundsToWorld(model, sel->geometry.aabbMin,
				sel->geometry.aabbMax, c, r);
			return smvk::RenderViewIntersectsSphere(cullView, c, r);
		}
		return false;
	};
	// Skydome first (depth Always, no writes).
	if (m_passes.skydome && m_skyEnabled && m_skydome.ShadersValid()) {
		cmd.BindRenderPipeline(
			m_skydome.program,
			smvk::VertexFormat::Empty,
			smvk::DepthSettings::Disabled(),
			noStencil,
			smvk::BlendSettings::Opaque(),
			msaaSettings,
			smvk::RasterSettings{ .cullMode = smvk::CullMode::None }
		);
		cmd.PushConstants(0, sizeof(PushConstants), &pc, smvk::ShaderStage::All);
		cmd.Draw(3, 1, 0, 0);
	}
	// Opaque + MASK cutout (alpha-tested in shader), per-material binds.
	{
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
			if (!itemVisible(item))
				return;
			const uint32_t materialId = item.material;
			if (materialId != boundMaterial) {
				// Select GI permutation program based on loaded assets
				const app::GIVariant giVar = GetActiveGIVariant();
				smvk::ShaderProgram& prog = m_resources.mainProgramGi[static_cast<uint32_t>(giVar)];
				if (prog.shaderCount == 0) {
					prog = m_resources.mainProgram; // fallback
				}
				bindForMaterial(materialId, prog, opaqueDepth);
				boundMaterial = materialId;
			}
			drawNode(item.node, item.mesh, materialId);
		}
		);
	}
	// Ground grid (editor construction grid, XZ at y = 0): same driven
	// lines as the main view, depth-tested read-only against this view's
	// prepass depth, before transparents so glass blends over them.
	if (m_gridEnabled && (m_gridMinorVerts + m_gridMajorVerts) > 0
		&& !m_gridLines.empty() && m_gfxRenderer.DebugProgramsValid()) {
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
		cmd.BindVertexBuffer(m_gridLines, 0, 0);
		const float gridExposure = FrameExposure();
		const float gridGain = (gridExposure > 1e-9f) ? 1.0f / gridExposure : 1.0f;
		LineDebugPush gp{};
		std::copy_n(glm::value_ptr(cullView.m_viewProjectionMatrix), 16, gp.viewProj);
		gp.color[0] = 0.32f * gridGain; gp.color[1] = 0.34f * gridGain;
		gp.color[2] = 0.38f * gridGain; gp.color[3] = 1.0f;
		cmd.PushConstants(0, sizeof(gp), &gp, smvk::ShaderStage::All);
		cmd.Draw(m_gridMinorVerts, 1, 0, 0);
		gp.color[0] = 0.45f * gridGain; gp.color[1] = 0.47f * gridGain;
		gp.color[2] = 0.52f * gridGain; gp.color[3] = 1.0f;
		cmd.PushConstants(0, sizeof(gp), &gp, smvk::ShaderStage::All);
		cmd.Draw(m_gridMajorVerts, 1, m_gridMinorVerts, 0);
	}
	// Transparent (AlphaMode::Blend): dithered (opaque state, screen-door
	// discard) or classic back-to-front blend — mirrors the main leg
	// (per-view eye for the distance sort).
	{
		const bool ditheredTransp =
			(m_transparentMode == smvk::TransparentMode::Dithered);
		const std::vector<TransparentDrawItem> draws = CollectTransparentDraws(
			cullView, !ditheredTransp, viewEye);
		uint32_t boundMaterial = UINT32_MAX;
		for (const TransparentDrawItem& d : draws) {
			if (d.materialId != boundMaterial) {
				if (ditheredTransp) {
					// Opaque state: the shader discard does the work
					// (blend off; depth follows opaque) — main-leg parity.
					smvk::BlendSettings opaqueBlend = smvk::BlendSettings::Opaque();
					smvk::RasterSettings raster{};
					if (smvk::Material* mat = m_world->GetMaterial(d.materialId))
						raster = mat->pipeline.raster;
					cmd.BindRenderPipeline(
						(m_wireframe && m_resources.mainWireProgram.shaderCount == 2)
							? m_resources.mainWireProgram
							: m_resources.mainProgram,
						smvk::VertexFormat::StaticVertexFormat,
						opaqueDepth,
						noStencil,
						opaqueBlend,
						msaaSettings,
						wireframeRaster(raster)
					);
				} else {
					bindForMaterial(d.materialId,
						GetActiveGIVariant() != GIVariant::None && m_resources.mainProgramGi[static_cast<uint32_t>(GetActiveGIVariant())].shaderCount
						? m_resources.mainProgramGi[static_cast<uint32_t>(GetActiveGIVariant())]
						: m_resources.mainProgram,
						transparentDepth);
				}
				boundMaterial = d.materialId;
			}
			drawNode(d.node, d.meshId, d.materialId);
		}
	}
}

void WorldRenderer::RenderFrame(smvk::Framebuffer& fb) {
	SMVK_PROFILE_FUNCTION();
	SMVK_PROFILE_TAG("frameIndex", fb.frameIndex);

	m_frameCounter += 1;
	// Open the Streamline frame (one SL token per rendered frame, shared
	// with the DLSS evaluate below + the engine's PCL submit/present
	// markers in EndAndPresentFramebuffer). Silent no-op unless the PCL
	// plugin loaded — same monotonic-id convention as dlssFrameId.
	(void)smvk::DlssBeginFrame(static_cast<uint32_t>(m_frameCounter & 0xffffffffu));
	m_rendergraph->NewFrame();
	// Texture filtering (bilinear <-> trilinear) is renderer state: enforce
	// the selected mipmap mode on the bindless sampler slots every frame
	// (no-op when already set; recreates slots 0 + 2 on change).
	ApplyTextureFiltering();
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
		SMVK_PROFILE_SCOPE("WorldRenderer_ResourceEnsure");
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
		m_viewportResources.sao.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSaoDesc());
		smvk::MsaaDesc frameMsaaDesc{};
		frameMsaaDesc.depthFormat = m_swapchain->GetDepthFormat();
		m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, renderExtent, frameMsaaDesc);
		m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc());
		{
			smvk::DofDesc frameDofDesc = MakeDofDesc();
			frameDofDesc.hqBokeh = m_dofHqBokeh;
			m_viewportResources.dof.Ensure(*m_context, *m_rendergraph, renderExtent, frameDofDesc);
		}
		// Silent SSR target recreation (extent/format drift outside
		// RebuildSwapViews) loses the temporal history content: invalidate
		// first so the next temporal record warms up instead of blending
		// against garbage. RebuildSwapViews invalidates unconditionally;
		// this guard covers any other drift path (including first creation).
		if (!m_viewportResources.ssr.TargetsValid()
			|| m_viewportResources.ssr.extent.width != renderExtent.width
			|| m_viewportResources.ssr.extent.height != renderExtent.height)
			smvk::SsrTaaInvalidateHistory(m_ssrTaa);
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
		// Directional cascade atlas follows the UI tile resolution (Ensure
		// is a no-op in steady state; recreates once on slider change).
		m_viewportResources.directionalShadows.Ensure(*m_context, *m_rendergraph,
			MakeShadowDesc(m_cascadeResolution, m_cascadeLambda));
		m_viewportResources.spotShadows.Ensure(*m_context, *m_rendergraph, MakeSpotShadowDesc());
		// EVSM atlases follow the pipeline-mode member so flipping
		// m_evsmPipeline (OneFrameLag <-> SameFrame) takes effect on
		// the next frame without a restart (Ensure is a no-op in
		// steady state; shaders persist across the flip).
		m_viewportResources.evsmDir.Ensure(*m_context, *m_rendergraph, MakeEvsmDirDesc(m_evsmPipeline, m_cascadeResolution));
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
		// SMAA T2x global toggle (see SmaaT2xState in smaa.h): one phase
		// for the recorded-frame sequence, advanced once per T2x record.
		// Any AA-mode change invalidates the history (a T2x -> 1x/Off ->
		// T2x round-trip would otherwise reuse ancient history).
		if (m_aaMode != m_smaaLastMode) {
			smvk::SmaaInvalidateHistory(m_smaaT2x);
			m_smaaLastMode = m_aaMode;
			// The T2x jitter pattern moved with the mode: the SSR trace
			// ran under a different proj jitter, so its history no longer
			// reprojects exactly — warm the temporal chain up again.
			smvk::SsrTaaInvalidateHistory(m_ssrTaa);
		}
		// Global T2x snapshot for this record: the jitter below, the
		// history slot + subsample indices of the SMAA chain and the overlay
		// all read this one snapshot so they always agree. Evaluated after
		// Ensure so SmaaActive() matches the chain-emission gate below (the
		// first record after (re)creation degrades to a warmup record).
		m_smaaFrameParams = smvk::SmaaBeginFrame(m_smaaT2x, SmaaT2xActive());
		m_smaaFrameRanT2x = false; // set where the T2x chain is emitted
		m_ssrFrameRanTemporal = false; // set where the SSR temporal pass is emitted
		// Bench phase timers (active only with --bench past warmup).
		using bench_clk = std::chrono::steady_clock;
		const bool benchPhase = frameBudget_ > 0 && benchWarmup_ >= 10;
		const auto rfT0 = bench_clk::now();
		bench_clk::time_point rfT2, rfT3; // assigned in the exposure block below
		bench_clk::time_point rfPA, rfPB; // preMain split: fw+/filter | cull/late
		// Hitch logger (VULKANTOY_HITCH_LOG=<ms>): one line per frame that
		// exceeds the budget — traversal hitches vs shader-compile warm-up
		// (a hitch that also mints pipelines is compilation; a hitch with a
		// stable pipeline count is genuine traversal/alloc cost). Logged at
		// the frame end below.
		static const double sHitchBudgetMs = [] {
			const char* e = std::getenv("VULKANTOY_HITCH_LOG");
			return e ? std::atof(e) : 0.0;
		}();
		static size_t sHitchLastPipelines = 0;
		// Decal ring upload BEFORE UpdateFrameUniforms: it reads the live
		// count (m_decalFrameCount) into FrameUniforms.
		(void)UpdateDecals(fb.frameIndex);
		// The renderer owns no environment: the frame's look is built
		// from the WORLD's scene env here, once per frame (the main
		// view, the shadow fits and every secondary view take it as a
		// parameter below — never member state). A worldless frame falls
		// back to engine defaults (Initialize refuses those anyway).
		const smvk::EnvironmentSettings frameEnv = m_world
			? MapSceneEnv(m_world->scene.environment,
				m_debugView == DebugView::Unlit)
			: smvk::EnvironmentSettings{};
		const FrameUniforms frameData = UpdateFrameUniforms(fb.frameIndex, renderExtent, swapExtent, frameEnv);
		const smvk::GpuDirectionalLightDescription dirLight = UpdateLights(fb.frameIndex, frameEnv);
		UpdateMaterials(fb.frameIndex);
		const auto rfT0b = bench_clk::now();
		UpdateTextureStreaming(frameData, renderExtent);
		const auto rfT0c = bench_clk::now();
		const uint32_t fpLightCount = UpdateForwardPlusLights(fb.frameIndex, frameData);
		const auto rfT1 = bench_clk::now();

		// ------------------------------------------------------------------
		// MULTIVIEW (hosted): advance the global tic/toc phase + prep every
		// secondary view due THIS frame (targets, lean FrameUniforms,
		// per-view CSM fit + spot selection, view-major slab uploads).
		// Off-phase views are skipped entirely (their images persist).
		// Runs BEFORE the RenderView allocation below so the per-view
		// cameras ride the same slab upload.
		// ------------------------------------------------------------------
		m_viewPhase = static_cast<uint32_t>(m_frameCounter & 1ull);
		PrepareSecondaryViews(fb.frameIndex, frameEnv);

		// Ground grid (editor construction grid): rebuilt + uploaded
		// here (before any pass records); every Main below draws it
		// depth-tested against its own depth. Driven (hosted editor
		// only) — the windowed App never enables it.
		BuildGridLines();

		// ------------------------------------------------------------------
		// GPU particles (see smvk::Particles + particles.h): extent-
		// independent builder Ensure (pool/free-list/emitter ring + the
		// three programs) + the per-frame emitter parameter upload. The
		// GPU owns spawn scheduling (fractional carry) and simulation —
		// the CPU never touches particle state.
		// ------------------------------------------------------------------
		m_viewportResources.particles.Ensure(*m_context, *m_rendergraph,
			m_swapchain->framesInFlight(), MakeParticlesDesc());
		const bool particlesOn = ParticlesActive();
		const uint32_t particleEmitterCount =
			particlesOn ? UpdateParticles(fb.frameIndex) : 0u;

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
			{ static_cast<float>(CascadeTileExtent().width), static_cast<float>(CascadeTileExtent().height) });
		uint32_t spotViewBase = smvk::RenderViewSet::kInvalidIndex;
		for (const smvk::GpuCascadeDescription& tile : m_spotShadowTiles) {
			const uint32_t view = m_renderViews.AllocateSpotShadowView(
				tile, { static_cast<float>(SPOT_SHADOW_RES), static_cast<float>(SPOT_SHADOW_RES) });
			if (spotViewBase == smvk::RenderViewSet::kInvalidIndex)
				spotViewBase = view;
		}
		// MULTIVIEW: the due secondary views' cameras join the same slab
		// (main-flagged views + their cascade/spot fits). Only views
		// prepared this frame (dirLightValid) allocate — off-phase views
		// render nothing and hold no slots (kMaxRenderViews budget: the
		// main view + up to 3 secondary views x (1 main + 4 cascades +
		// up to 8 spots) fits the 64-view slab).
		for (SecondaryView& v : m_views) {
			v.mainViewIdx = smvk::RenderViewSet::kInvalidIndex;
			v.cascadeViewBase = smvk::RenderViewSet::kInvalidIndex;
			v.spotViewBase = smvk::RenderViewSet::kInvalidIndex;
			if (!v.dirLightValid)
				continue; // not due (or prep failed) this frame
			v.mainViewIdx = m_renderViews.AllocateView(
				v.frameData.viewProj,
				{ static_cast<float>(v.desc.extent.width),
					static_cast<float>(v.desc.extent.height) },
				smvk::RenderViewFlagBit_MainView, kMainViewCullGuardPx);
			if (v.mainViewIdx == smvk::RenderViewSet::kInvalidIndex)
				continue; // slab full: skip the view this frame
			v.cascadeViewBase = m_renderViews.AllocateDirectionalLightViews(
				v.dirLight,
				{ static_cast<float>(CascadeTileExtent().width),
					static_cast<float>(CascadeTileExtent().height) });
			for (const smvk::GpuCascadeDescription& tile : v.spotShadowTiles) {
				const uint32_t view = m_renderViews.AllocateSpotShadowView(
					tile, { static_cast<float>(SPOT_SHADOW_RES),
						static_cast<float>(SPOT_SHADOW_RES) });
				if (v.spotViewBase == smvk::RenderViewSet::kInvalidIndex)
					v.spotViewBase = view;
			}
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
		// the World's per-node decision cache keys on frameId).
		ResetLodStats();
		m_lodFrameId = m_lodFrameId + 1; // monotonic (fb.frameIndex wraps)
		m_lodViewportHeight = static_cast<float>(renderExtent.height);
		m_lodFrameInputs.frameId = m_lodFrameId;
		m_lodFrameInputs.eye = m_cameraPositioner.cameraPosition_;
		m_lodFrameInputs.viewportHeight = m_lodViewportHeight;
		m_lodFrameInputs.fovY = glm::radians(kCameraFovYDegrees);
		m_lodFrameInputs.pixelBudget = m_lodPixelBudget;
		if (m_world) {
			m_world->SetLodFullRate(m_lodFullRate);
			if (m_lodBudgetApplied != m_lodPixelBudget) {
				m_world->InvalidateLodCache(); // budget tweak: refresh all nodes
				m_lodBudgetApplied = m_lodPixelBudget;
			}
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
					CascadeTileExtent().width, CascadeTileExtent().height);
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
				smvk::mem::MakeProcess<smvk::ThreadPool>(std::min(hw - 1, 8u));
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
				// EVSM moment pass (RGBA32F 2xMSAA + resolve, direct path).
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
					// EVSM moment pass (RGBA32F 2xMSAA + resolve, direct path).
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

		// ---- GPU-driven filter + temporal seed ----
		// Runs after fc.pc/depthView are built (the seed pass captures
		// frame addresses); the seed writes depth FIRST. When the seed
		// primed depth, the Northlight depth split below builds the HZB
		// over that seed-only occluder depth, culls the rest, and only
		// then completes depth (fallback prepass + filtered ranges).
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
		// 2x MSAA mode (hoisted: the fc fill below + Main + the pane layer
		// all key on it — Main writes the TRANSIENT MSAA depth whose
		// stencil is DontCare-stored, so the 1x stencil aspect goes
		// unwritten there and the layer must fall back).
		const bool useMsaa = m_passes.msaa && m_viewportResources.msaaEnabled
			&& m_context->SupportsMSAA2x() && m_viewportResources.msaa.Valid();
		// SSSR-style thin G-buffer (see gbuffer.frag.hlsl): the Forward+
		// depth prepass writes it (RGBA8 oct normal + roughness + metal —
		// the prepass already rasterizes every opaque surface; Main is
		// untouched, no stencil classification). False under MSAA (the
		// prepass has no stable 1x G-buffer companion) and for SSR-off
		// frames (the only consumer is the SSR trace).
		fc.ssrGbufferWanted = m_passes.ssr && m_ssrEnabled
			&& m_ssrIntensity > 0.0f
			&& !DebugVisualizationActive()
			&& !useMsaa
			&& m_viewportResources.ssr.gbuffer.Valid();
		fc.gbufferView = fc.ssrGbufferWanted
			? m_viewportResources.ssr.gbuffer.logical
			: smvk::LogicalImageViewHandle{};
		// One-shot material census (VULKANTOY_MATERIAL_CENSUS): dumps
		// every material's factors (incl. layers) + the resulting
		// G-buffer roughness/metal to stderr — the per-material ground
		// truth for the G-buffer's continuous values.
		static bool sMaterialCensusDone = false;
		if (!sMaterialCensusDone && m_world) {
			sMaterialCensusDone = true;
			if (std::getenv("VULKANTOY_MATERIAL_CENSUS")) {
				// Layer combine: MIN roughness wins (strongest reflector),
				// metal ORs — the same combine the G-buffer represents.
				auto classify = [&](const smvk::Material* mat) -> const char* {
					if (!mat)
						return "<null>";
					uint32_t ref = smvk::stencil::ReceiverRef(mat->values);
					const int layerMode = mat->values.layerMaterials.w;
					if (layerMode > 1) {
						const int32_t layerIds[3] = {
							mat->values.layerMaterials.x,
							mat->values.layerMaterials.y,
							mat->values.layerMaterials.z,
						};
						for (int i = 0; i < layerMode - 1 && i < 3; ++i) {
							if (layerIds[i] < 0)
								continue;
							const smvk::Material* layer =
								m_world->GetMaterial(static_cast<uint32_t>(layerIds[i]));
							if (!layer)
								continue;
							const uint32_t lref =
								smvk::stencil::ReceiverRef(layer->values);
							// min roughness (most reflective) wins; metal ORs.
							const uint32_t rMin = (std::min)(ref, lref)
								& smvk::stencil::kRoughnessMaxValue;
							ref = rMin | ((ref | lref) & smvk::stencil::kMetalBit);
						}
					}
					// Display names keep the old mirror-class buckets
					// (kCensusMirrorRoughness); the byte itself is continuous.
					const float r = smvk::stencil::RefRoughness(
						static_cast<uint8_t>(ref));
					const bool metal = smvk::stencil::RefMetal(
						static_cast<uint8_t>(ref));
					return r > smvk::stencil::kCensusMirrorRoughness ? "-"
						: metal ? "SSR+METAL" : "SSR";
				};
				uint32_t counts[4] = { 0, 0, 0, 0 };
				std::fprintf(stderr, "[MaterialCensus] id | metal rough shade mrTex | layers | class\n");
				for (uint32_t i = 0; i < m_world->materialLocations.size(); ++i) {
					const smvk::Material* mat = m_world->GetMaterial(i);
					const char* cls = classify(mat);
					if (!mat) {
						std::fprintf(stderr, "[MaterialCensus] %4u | <null> | - | -\n", i);
						continue;
					}
				++counts[cls[0] == '-' ? 0
					: strcmp(cls, "SSR") == 0 ? 1
					: strcmp(cls, "SSR+METAL") == 0 ? 2 : 3];
				std::fprintf(stderr,
					"[MaterialCensus] %4u | %4.2f %4.2f %1d %c | L%d(x%d,y%d,z%d) | %s\n",
					i, mat->values.params.y, mat->values.params.z,
					(int)mat->values.params.w,
					mat->values.textures.y >= 0 ? 'T' : '-',
					mat->values.layerMaterials.w,
					mat->values.layerMaterials.x,
					mat->values.layerMaterials.y,
					mat->values.layerMaterials.z,
					cls);
			}
			std::fprintf(stderr,
				"[MaterialCensus] totals: none=%u SSR=%u SSR+METAL=%u other=%u\n",
				counts[0], counts[1], counts[2], counts[3]);
			}
		}
		// Option A split: the occluder prepass fills hizDepth (Clear,
		// independent of the seed) for the HZB build below; renderDepth
		// (seeded) serves Main, SSAO, tile-cull and deferred shadows.
		// Skipped while the culling view is frozen (the held pyramid
		// must not be rebuilt).
		const bool hizOccluderOn = gfxFilterOn && m_hizOccluderDepth
			&& !cullingFrozen && !fc.hizDepthView.empty();
		if (hizOccluderOn)
			m_fpRenderer.AppendOccluderPrepass(fc);

		// Northlight depth split (filter on + seed primed depth): the HZB
		// is built over the SEED-ONLY depth (the known-visible occluder
		// set rasterized at the current LOD with the real alpha test),
		// so the filter's occlusion test is live for the first time.
		// Without the split (full prepass before the HZB) the occlusion
		// test is a structural no-op: every CPU-visible cluster's own
		// depth is already inside its footprint texels (the MIN-reduce
		// keeps through the cluster's own fragments), so nothing can ever
		// be culled — which is why T4 occlusion looked permanently silent.
		//
		// Depth assembly afterwards (every filterable cluster depthed
		// EXACTLY ONCE, no full prepass walk):
		//   seed      prev∩curr clusters (occluders: opaque VS-only +
		//             masked alpha-tested) — drawn in AppendPasses above.
		//   remainder curr−prev clusters (newly visible: disocclusion,
		//             LOD flips, frustum entries) — the T5 CPU-command
		//             pass after the filter. It ignores the GPU verdicts
		//             (draws all curr−prev even if occluded), but that set
		//             is exactly the disocclusion candidates the seed-only
		//             HZB cannot prove occluded anyway.
		//   fallback  prepass walks ONLY the unfilterable rest: unregistered
		//             meshes/LOD levels, material-less meshes and shader-LOD
		//             vertex-lit instances (fallbackOnly=true below).
		// No indirect alpha-tested draws: drawing the MASKED ranges through
		// the filter's GPU-written commands misrasterizes depth (the
		// opaque-white regression; bisected — any depth write from that
		// combination poisons Main's 1x read-only test), and the seed +
		// remainder pair covers masked depth with the proven CPU-command
		// machinery, one draw per cluster. Tile cull / deferred shadows /
		// AO below run over this complete depth (filter-culled clusters are
		// not rendered anywhere, so their absence from depth is correct).
		// Held while the culling view is frozen (frozen footprints must
		// test against the frozen pyramid, not live depth).
		// The SSSR thin G-BUFFER needs the FULL prepass walk every frame:
		// the seed pass draws the filterable majority depth-only (its VS
		// never writes the G-buffer), so on split frames the slim
		// fallback-only prepass leaves the majority's G-buffer values
		// STALE FOREVER (frozen normals + roughness under a moving camera
		// = smeared reflections; the first split frames load creation
		// garbage that is never overwritten). Disabling the split when
		// the G-buffer is wanted runs the proven full-prepass path: the
		// seed still primes depth (Loads), the full walk early-Z-rejects
		// the occluded against it, and every VISIBLE surface's surviving
		// fragment writes fresh G-buffer values each frame. The
		// seed-writes-the-G-buffer variant (a normal-outputting seed VS +
		// the trivial FS pair on the indirect draws) is the follow-up
		// that re-enables the slim split with SSR on.
		const bool depthSplit = gfxFilterOn && fc.seedDepthPrimed
			&& !m_splitDisabled
			&& !fc.ssrGbufferWanted;
		fc.gfxLodFilterable = depthSplit
			? std::function<bool(NodeId, uint32_t, uint32_t)>(
				[this](NodeId node, uint32_t worldMesh, uint32_t lodIdx) {
					// Shader-LOD vertex-lit levels draw direct in Main —
					// the prepass must keep covering them.
					if (ShaderLodActive()
						&& m_world->LodShaderAt(worldMesh, lodIdx)
							== smvk::LodShader::VertexLit)
						return false;
					(void)node;
					return m_gfxRenderer.GfxMeshForLod(worldMesh, lodIdx)
						!= UINT32_MAX;
				})
			: std::function<bool(NodeId, uint32_t, uint32_t)>{};
		auto appendHizPasses = [&]() {
			if (hizOccluderOn)
				m_gfxRenderer.AppendHizPasses(fc,
					m_viewportResources.hizDepth.logicalView,
					m_viewportResources.hizDepth.bindlessIndex);
			else
				m_gfxRenderer.AppendHizPasses(fc,
					m_viewportResources.renderDepth.logicalView,
					m_viewportResources.forwardPlus.depthBindlessIndex);
		};
		if (depthSplit) {
			// Seed (prev-visible occluders, current LOD, alpha-tested) ->
			// HZB over that depth -> filter culls the rest.
			if (!cullingFrozen)
				appendHizPasses();
			m_gfxRenderer.AppendFilterDispatch(fc);
			// Depth pass 2: slim direct fallback (Loads the seeded depth)
			// + the remainder (newly visible clusters, both buckets).
			if (m_passes.forwardPlusPrepass)
				m_fpRenderer.AppendPrepass(fc, /*fallbackOnly=*/true);
			if (m_gfxRenderer.RemainderPending()) {
				m_gfxRenderer.AppendRemainderPass(fc);
			}
		}
		else {
			// Full prepass before the HZB (Clear when not seeded).
			if (m_passes.forwardPlusPrepass)
				m_fpRenderer.AppendPrepass(fc);
			if (gfxFilterOn) {
				if (!cullingFrozen)
					appendHizPasses();
				m_gfxRenderer.AppendFilterDispatch(fc);
				if (m_gfxRenderer.RemainderPending()) {
					m_gfxRenderer.AppendRemainderPass(fc);
				}
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
	// Reflection probes ride the light buffer tail as type-2 entries: the
	// SAME forward+ cull dispatch buckets them, so when probes are loaded
	// the dispatch runs even under deferred shadows. The cull MUST see the
	// full [0, lights+probes) range — the probe tail lives at [L, L+P) and
	// the cull reads buffer indices from 0, so a lights-only count never
	// files a single probe (every consumer then degrades to the global
	// fallback cubemap). Under DS the light leg re-culls lights as dead
	// work (negligible next to the depth preload); the DS lists own the
	// lights either way.
	const bool probeListsWanted = ProbeTileListsWanted();
	rfPB = bench_clk::now();
	if (m_passes.forwardPlusCull && (!dsOn || decalListsNeeded || probeListsWanted))
		m_fpRenderer.AppendTileCull(fc, fpLightCount + ProbeEntryCount());
	if (dsOn)
		AppendDeferredShadowPhases(fb.frameIndex,
			frameUniformsAddress, fpLightsAddress, spotShadowBufferAddress,
			fpLightCount, fc.depthView, fc.fpLights);

	// ---- GPU particles: emit + simulate (compute). Runs after the light
	// cull spot (independent of it — the draw pass below consumes the tile
	// lists; emit -> sim ordering matters and comes from the pass hints:
	// emit pops free slots the PREVIOUS frame's sim recycled, so a slot
	// can never be handed out twice within a frame).
	const bool particlesRun = particlesOn && particleEmitterCount > 0;
	if (particlesRun) {
		smvk::Particles& ps = m_viewportResources.particles;
		// Clamp dt: a tab-out/hibernate spike would otherwise fling every
		// particle across the scene (spawn quotas too — rate*dt is the
		// quota, so a 5 s dt would try to spawn 5 s worth at once).
		const float particleDt = std::min((float)delta_time_s, 1.0f / 30.0f);

		smvk::ParticlesEmitPush emitPush{};
		emitPush.emitters = ps.EmittersAddress(*m_context, fb.frameIndex);
		emitPush.emitterState = ps.EmitterStateAddress(*m_context);
		emitPush.particles = ps.ParticlesAddress(*m_context);
		emitPush.freeList = ps.FreeListAddress(*m_context);
		emitPush.emitterCount = particleEmitterCount;
		emitPush.maxParticles = PARTICLE_MAX;
		emitPush.dt = particleDt;
		emitPush.frameSeed = 1664525u * (uint32_t)m_frameCounter + 1013904223u;
		auto& emitPass = ps.AddEmitPass(*m_rendergraph);
		emitPass.SetRecordingFunc([this, emitPush](smvk::CommandBuffer& cmd) {
			cmd.BindComputePipeline(m_viewportResources.particles.emitProgram);
			cmd.PushConstants(0, sizeof(emitPush), &emitPush, smvk::ShaderStage::All);
			cmd.Dispatch(PARTICLE_MAX_EMITTERS, 1, 1);
		});

		smvk::ParticlesSimPush simPush{};
		simPush.particles = ps.ParticlesAddress(*m_context);
		simPush.freeList = ps.FreeListAddress(*m_context);
		simPush.emitters = ps.EmittersAddress(*m_context, fb.frameIndex);
		simPush.gravityX = 0.0f;
		simPush.gravityY = m_particleDemo.gravityY;
		simPush.gravityZ = 0.0f;
		simPush.dt = particleDt;
		simPush.maxParticles = PARTICLE_MAX;
		auto& simPass = ps.AddSimPass(*m_rendergraph);
		simPass.SetRecordingFunc([this, simPush](smvk::CommandBuffer& cmd) {
			cmd.BindComputePipeline(m_viewportResources.particles.simProgram);
			cmd.PushConstants(0, sizeof(simPush), &simPush, smvk::ShaderStage::All);
			cmd.Dispatch(
				smvk::Particles::SimGroupsFor(PARTICLE_MAX, PARTICLE_SIM_THREADS),
				1, 1);
		});
	}

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
				// 1x depth stays read-only in both phases (prepass owns it).
			const smvk::DepthSettings motionDepth{
				.depthFunc = smvk::CompareOp::GreaterEqual,
				.writeEnable = false,
				.depthTestEnable = true,
			};
			// Phase 2 masked pipeline: cull follows the material raster so
			// motion coverage matches Main (single-sided cutout culls
			// back; double-sided keeps Cull None). Rebinds only on cull
			// changes (the CommandBuffer dedupes identical states).
			smvk::CullMode boundMotionMaskedCull = smvk::CullMode::FrontAndBack;
			auto ensureMotionMaskedPipeline = [&](smvk::CullMode cull) {
				if (cull == boundMotionMaskedCull)
					return;
				cmd.BindRenderPipeline(
					m_viewportResources.motionVectors.program,
					smvk::VertexFormat::StaticVertexFormat,
					motionDepth,
					smvk::StencilSettings{},
					smvk::BlendSettings::Opaque(),
					smvk::MSAASettings::Count1(),
					smvk::RasterSettings{ .cullMode = cull }
				);
				cmd.PushConstants(0, sizeof(PushConstants), &motionPC, smvk::ShaderStage::All);
				boundMotionMaskedCull = cull;
			};
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
				if (maskedPhase) {
					smvk::CullMode cull = smvk::CullMode::None;
					if (const smvk::Material* mat = m_world->GetMaterial(item.material))
						cull = mat->pipeline.raster.cullMode;
					ensureMotionMaskedPipeline(cull);
				}
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
				// Pack as float3x4 columns (smvk::PackPushFloat3x4): a
				// flat 12-float copy is wrong (drops translation, leaks
				// the projection row — see the helper's comment).
				smvk::PackPushFloat3x4(glm::value_ptr(prevModel), objectPC.prevModel);
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
				// Filtered static path: one HZB-culled multi-draw per
				// material over the compacted index list (model matrices
				// come from the draws buffer via SV_InstanceID; materialId
				// is pushed per range for the FS alpha test). Cull follows
				// the material raster so motion coverage matches Main.
				if (motionFilteredActive) {
					const auto& motionRanges = m_gfxRenderer.MaterialRanges(gfxSlot);
					for (uint32_t ri = 0; ri < (uint32_t)motionRanges.size(); ++ri) {
						const uint32_t materialId = motionRanges[ri].material;
						smvk::CullMode cull = smvk::CullMode::Back;
						if (smvk::Material* rmat = m_world->GetMaterial(materialId))
							cull = rmat->pipeline.raster.cullMode;
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
				// Phase 2: MASK cutout — the pipeline binds lazily per
				// material cull (ensureMotionMaskedPipeline above).
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
	// AO – half-res depth-only AO (SSAO, see ssao.h; GTAO, see gtao.h;
	// SAO, see sao.h).
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
		SMVK_PROFILE_CATEGORY("AO", Optick::Category::Rendering);
		const bool aoWanted = m_passes.ssao && m_aoMode != AoMode::None
			&& m_ssaoIntensity > 0.0f;
		if (aoWanted && m_aoMode == AoMode::Ssao && m_viewportResources.ssao.Valid()) {
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
		} else if (aoWanted && m_aoMode == AoMode::Gtao && m_viewportResources.gtao.Valid()) {
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
		} else if (aoWanted && m_aoMode == AoMode::Sao && m_viewportResources.sao.Valid()) {
			smvk::SaoDesc saoDesc = MakeSaoDesc();
			saoDesc.radius = m_ssaoRadius;
			saoDesc.bias = m_ssaoBias;
			saoDesc.intensity = m_ssaoIntensity;
			saoDesc.maxDistance = m_ssaoMaxDistance;
			saoDesc.sampleCount = (uint32_t)m_saoSamples;
			saoDesc.spiralTurns = (uint32_t)m_saoSpiralTurns;
			saoDesc.blurSharpness = m_ssaoBlurSharpness;
			saoDesc.nearPlane = frameData.nearPlane;
			m_viewportResources.sao.AddSaoPasses(
				*m_rendergraph,
				m_viewportResources.renderDepth.logicalView,
				m_viewportResources.renderDepth.bindlessIndex,
				frameUniformsAddress,
				linearSampler,
				saoDesc,
				"Sao",
				"SaoBlur");
			aoView = m_viewportResources.sao.OutputView();
		}
	}

	// useMsaa is hoisted above the fc fill (the stencil layer gate + the
	// pane layer below + Main's attachments here all key on it).
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
				hdrColors, // HDR intermediate – tonemapper will resolve to swap
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0, 0, 0, 1)
			)
			// Depth was filled by the Forward+ prepass: load it, test
			// read-only (GreaterEqual = reversed-Z near-wins).
			// StoreOp MUST be Store, never DontCare: SSR/volumetric/DS
			// sample this depth AFTER Main, and a DontCare store makes
			// the contents undefined past the pass — the driver may
			// decompress-with-discard any tile it likes, leaving raw-
			// memory garbage (huge/NaN) in ~5% of texels that every
			// depth march then hits. This was the "only works in Nsight"
			// bug: Nsight's frame debugger hooks store ops to PRESERVE
			// attachment contents, hiding it under the profiler.
			.SetDepth(m_viewportResources.renderDepth.logicalView,
				smvk::AttachmentLoadOp::Load,
				smvk::AttachmentStoreOp::Store,
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

		// NOTE: no stencil classification here — the SSSR-style thin
		// G-buffer (receiver normals + roughness + metal) is written by
		// the Forward+ depth PREPASS (see gbuffer.frag.hlsl), so Main
		// binds no stencil write state at all (the old per-material
		// REPLACE classification is gone with it).

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
			// Generic lit, A2C twin and every fixed-lobe twin all shade
			// through the lit FS family -> the same flat twin.
			if (m_gfxRenderer.IsFilteredLitProgram(prog)) {
				return m_gfxRenderer.FilteredWireValid()
					? m_gfxRenderer.FilteredWireProgram() : prog;
			}
			if (&prog == &m_resources.vertexLitProgram) {
				return m_resources.vertexLitWireProgram.shaderCount == 2
					? m_resources.vertexLitWireProgram : prog;
			}
			if (m_gfxRenderer.IsFilteredLitProgram(prog)) {
				return m_gfxRenderer.FilteredWireValid()
					? m_gfxRenderer.FilteredWireProgram() : prog;
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
				const smvk::DepthSettings& depth,
				bool writeStencilClass = true) -> smvk::ShaderProgram& {
			// Per-material raster (doubleSided -> Cull None) + blend
			// (Opaque vs AlphaBlend). Depth writes follow the caller: opaque
			// writes in 2x MSAA mode, transparent never writes.
			// writeStencilClass: kept for call-shape stability — the SSR
			// receiver-material stencil classification is GONE (the thin
			// G-buffer owns receiver normals + roughness + metal now, see
			// gbuffer.frag.hlsl); every bind carries noStencil.
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
			// discard) + A2C multisample state — foliage silhouettes
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
			(void)writeStencilClass;
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
		if (m_passes.skydome && m_skyEnabled && m_skydome.ShadersValid()) {
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
		if (effView == DebugView::Pom && m_resources.pomProgram.shaderCount != 2)
			effView = DebugView::Lit;
		const bool meshletDbg = (effView == DebugView::Meshlets);
		const bool lodDbg = (effView == DebugView::Lods);
		const bool flatDbg = meshletDbg || lodDbg; // meshDebugProgram path
		const bool unlitDbg = (effView == DebugView::Unlit);
		const bool cascadeDbg = (effView == DebugView::Cascades);
		const bool vertexColorDbg = (effView == DebugView::VertexColors);
		const bool uvDbg = (effView == DebugView::Uv0 || effView == DebugView::Uv1);
		const bool pomDbg = (effView == DebugView::Pom);
		const bool directDbg = unlitDbg || cascadeDbg || vertexColorDbg || uvDbg || pomDbg; // default.vert + debug FS
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
		// (see world_renderer.h), so the gain is a constant 1/0.001 and
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
		// Unlit (F2) / cascade (F4) / vertex-color (F6) / UV (F9/F10) /
		// POM (F11) views: every opaque instance direct with the debug
		// fragment program. Full push per draw (model + materialId over the
		// captured frame addresses): the flat debug pushes above clobber
		// the shared push range, so delta pushes alone would read garbage
		// frame addresses here.
		if (directDbg) {
			smvk::ShaderProgram& dbgProg =
				unlitDbg ? m_resources.unlitProgram
				: cascadeDbg ? m_resources.cascadeProgram
				: vertexColorDbg ? m_resources.vertexColorProgram
				: pomDbg ? m_resources.pomProgram
				: effView == DebugView::Uv1 ? m_resources.uv1DebugProgram
				: effView == DebugView::SsrContribution ? m_resources.ssrContributionDebugProgram
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
			// TEMP-DIAG: per-frame census of which lit program each
			// material range binds (generic vs fixed-lobe twin).
			for (uint32_t ri = 0; ri < (uint32_t)gfxRanges.size(); ++ri) {
				const uint32_t materialId = gfxRanges[ri].material;
				// Fixed-lobe lit twin for the range's cooked count: the SG
				// evaluation folds to literals. Falls back to the generic
				// lit program for mixed/unlightmapped ranges (incl. the
				// A2C leg, which intentionally stays generic).
				smvk::ShaderProgram& litProg =
					m_gfxRenderer.FilteredLitProgram(gfxRanges[ri].lobeCount);
				bindForMaterial(materialId, litProg,
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

		// ---- Ground grid (editor construction grid, XZ at y = 0) ----
		// Driven (hosted editor only — the App never enables it). Drawn
		// depth-tested read-only against the prepass depth (same contract
		// as the cone overlay below), before transparents so glass blends
		// over the lines. Two authored colors (minor + major).
		if (m_gridEnabled && (m_gridMinorVerts + m_gridMajorVerts) > 0
			&& !m_gridLines.empty() && m_gfxRenderer.DebugProgramsValid()) {
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
			cmd.BindVertexBuffer(m_gridLines, 0, 0);
			// Display-space authored grays: pre-scale like the flat debug
			// colors above so the tonemap reproduces them.
			const float gridExposure = FrameExposure();
			const float gridGain = (gridExposure > 1e-9f) ? 1.0f / gridExposure : 1.0f;
			LineDebugPush gp{};
			std::copy_n(glm::value_ptr(frameViewProj), 16, gp.viewProj);
			gp.color[0] = 0.32f * gridGain; gp.color[1] = 0.34f * gridGain;
			gp.color[2] = 0.38f * gridGain; gp.color[3] = 1.0f;
			cmd.PushConstants(0, sizeof(gp), &gp, smvk::ShaderStage::All);
			cmd.Draw(m_gridMinorVerts, 1, 0, 0);
			gp.color[0] = 0.45f * gridGain; gp.color[1] = 0.47f * gridGain;
			gp.color[2] = 0.52f * gridGain; gp.color[3] = 1.0f;
			cmd.PushConstants(0, sizeof(gp), &gp, smvk::ShaderStage::All);
			cmd.Draw(m_gridMajorVerts, 1, m_gridMinorVerts, 0);
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
		// artifacts out of hole reports. The list itself is shared with
		// the SSR PaneMask redraw (CollectTransparentDraws).
		{
			const bool ditheredTransp =
				(m_transparentMode == smvk::TransparentMode::Dithered);
			const std::vector<TransparentDrawItem> draws =
				CollectTransparentDraws(transparentCullView, !ditheredTransp);

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
							noA2CProgram, transparentDepth,
							/*writeStencilClass=*/false);
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
						bindForMaterial(d.materialId, *transProg, noA2CProgram,
							transparentDepth, /*writeStencilClass=*/false);
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
	// Probe entries live in the shared light tile lists: under DS the main
	// fragment shader still samples them for specular IBL, so the tile
	// buffers need the cull->Main edge here too (probeListsWanted gates
	// the dispatch and the freshness bit together). The FP path already
	// adds the same edge above.
	if (probeListsWanted && dsOn) {
		smvk::ForwardPlus::AddTileReadHints(mainPass,
			m_viewportResources.forwardPlus.tileCountsLogical,
			m_viewportResources.forwardPlus.tileIndicesLogical);
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
	// GPU particles – additive HDR billboards over the resolved Main color
	// (see particles.h). Runs after Main (HDR + prepass depth complete),
	// before the post chain: motion blur / bloom / exposure all pick the
	// particles up like any other HDR content. Depth-tested read-only
	// against the prepass depth (1x in both MSAA modes: MSAA Main writes
	// the transient target, the prepass wrote renderDepth — same opaque
	// scene, which is exactly what particles must occlude against).
	// ---------------------------------------------------------------------
	if (particlesRun) {
		smvk::Particles& ps = m_viewportResources.particles;
		auto& particlePass = ps.AddDrawPass(*m_rendergraph,
			m_viewportResources.hdr.logicalView,
			m_viewportResources.renderDepth.logicalView,
			renderExtent);
		// Tiled lighting inputs: only when the forward+ cull ran with the
		// real light count this frame. Under deferred shadows the tile
		// lists are stale/empty for lights — the shader then walks a
		// capped brute-force light loop (fpTileCountX 0), and only the
		// light-buffer read hint remains (upload -> read ordering).
		const bool particleTilesUsable = m_passes.forwardPlusCull && !dsOn
			&& m_viewportResources.forwardPlus.Valid();
		if (particleTilesUsable) {
			smvk::Particles::AddDrawLightHints(particlePass,
				m_resources.fpLightsLogical,
				m_viewportResources.forwardPlus.tileCountsLogical,
				m_viewportResources.forwardPlus.tileIndicesLogical);
		} else {
			smvk::BufferSyncRequest fpLightRead{};
			fpLightRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
			fpLightRead.barrierMasks.stageMask = smvk::PipelineStageBit_Fragment;
			particlePass.AddBufferHint(m_resources.fpLightsLogical, fpLightRead);
		}
		smvk::ParticlesDrawPush drawPush{};
		drawPush.frameUniforms = frameUniformsAddress;
		drawPush.particles = ps.ParticlesAddress(*m_context);
		drawPush.emitters = ps.EmittersAddress(*m_context, fb.frameIndex);
		drawPush.fpLights = fpLightsAddress;
		drawPush.fpTileCounts = fpTileCountsAddress;
		drawPush.fpTileIndices = fpTileIndicesAddress;
		drawPush.fpTileCountX = particleTilesUsable ? fpTileCountX : 0u;
		drawPush.fpLightCount = fpLightCount;
		drawPush.fpTileSize = FP_TILE_SIZE;
		drawPush.fpMaxPerTile = FP_MAX_PER_TILE;
		drawPush.maxParticles = PARTICLE_MAX;
		particlePass.SetRecordingFunc([this, drawPush](smvk::CommandBuffer& cmd) {
			cmd.BindRenderPipeline(
				m_viewportResources.particles.drawProgram,
				smvk::VertexFormat::Empty,
				smvk::DepthSettings{
					.depthFunc = smvk::CompareOp::GreaterEqual,
					.writeEnable = false,
					.depthTestEnable = true,
				},
				smvk::StencilSettings{},
				smvk::BlendSettings::Add(),
				smvk::MSAASettings::Count1(),
				smvk::RasterSettings{ .cullMode = smvk::CullMode::None }
			);
			cmd.PushConstants(0, sizeof(drawPush), &drawPush, smvk::ShaderStage::All);
			// Fixed instance count: dead slots collapse to off-screen
			// degenerate quads in the VS (no CPU alive list — the pool is
			// fully GPU-resident; the dead-slot VS cost is the trade).
			cmd.Draw(6, PARTICLE_MAX, 0, 0);
		});
	}

		// ---------------------------------------------------------------------
		// Motion blur – velocity gather in HDR (see motionblur.h). Runs after
		// Main, before Bloom/Tonemap so both consume the blurred HDR. With
		// tile dilation on, two tiny compute prepasses (per-tile max
		// velocity + 5x5 dilate) run ahead of the gather so revealed
		// background streaks with the foreground that vacated it.
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
			blurDesc.tileDilation = m_motionBlurTileDilation;
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
		// (The old PaneMask transparents-redraw layer is GONE: panes read as
		// their seen-through opaque surface — the trace's receiver is the
		// opaque depth + the thin G-buffer's seen-through values. See
		// ssr.h + gbuffer.frag.hlsl.)
		// ---------------------------------------------------------------------

		// ---------------------------------------------------------------------
		// Screen-space reflections – half-res trace + bilateral blur +
		// full-res composite (see ssr.h). Runs after DOF, before
		// exposure/bloom/tonemap so reflections meter/bloom/tonemap with
		// the scene. Normals + Fresnel reconstruct from the 1x prepass
		// depth (no GBuffer) PLUS the pane layer above (pane receivers) and
		// the receiver-material stencil (prepass writes; see
		// Rendergraph/stencil_bits.h);
		// Bistro shopfront glass at street grazing angles is the
		// calibration case. Suppressed while a debug view replaces the
		// scene (DebugVisualizationActive) — reflections would composite
		// additively over the flat readout; the skipped record
		// invalidates the temporal history, so the first lit record
		// after the view warms up instead of blending stale.
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
			&& m_ssrIntensity > 0.0f
			&& !DebugVisualizationActive();
		if (ssrOn) {
			// Receiver-material layer availability: the thin G-buffer
			// (oct normal + roughness + metal) is written by the Forward+
			// depth prepass when fc.ssrGbufferWanted (see
			// gbuffer.frag.hlsl); when absent the trace falls back to the
			// legacy uniform-F0 mirror behavior.
			const bool gbufferLayerOn = fc.ssrGbufferWanted
				&& m_viewportResources.ssr.gbuffer.Valid();
			// ---- SSSR trace Hi-Z (MAX-reduce over the COMPLETE renderDepth
			// — hzb_max.cs.hlsl): built HERE, after every depth pass (the
			// trace runs after Main — the depth is final; the CULLING HZB
			// builds over the seed's partial set / Option A occluders
			// earlier and is MIN-reduced — unusable for marching). One
			// compute pass per mip level (rendergraph barriers between;
			// no intra-pass barrier API); mip 0 copies the depth texture,
			// mips 1+ MAX-reduce. Failure of any leg leaves the linear
			// adaptive march (the trace's HZB skip gates on BDA/mips 0).
			uint64_t ssrHzbAddr = 0;
			uint32_t ssrHzbMips = 0;
			smvk::LogicalBufferHandle ssrHzbLogical{};
			if (m_ssrHzbProgram.shaderCount == 1
				&& EnsureSsrHzb(*m_context, *m_rendergraph,
					m_viewportResources.ssr.extent.width,
					m_viewportResources.ssr.extent.height)) {
				ssrHzbAddr = m_context->GetBufferDeviceAddress(m_ssrHzbBuffer);
				if (ssrHzbAddr != 0)
					ssrHzbMips = m_ssrHzbMipCount;
				if (ssrHzbMips != 0) {
					ssrHzbLogical = m_ssrHzbLogical;
					const uint32_t depthBindless =
						m_viewportResources.renderDepth.bindlessIndex;
					for (uint32_t m = 0; m < m_ssrHzbMipCount; ++m) {
						const bool fromDepth = (m == 0);
						smvk::RenderPass& hzbPass =
							m_rendergraph->AddComputePass("SsrHzb");
						{
							smvk::BufferSyncRequest hzbRW{};
							hzbRW.barrierMasks.accessMask =
								smvk::AccessFlags::ShaderRead | smvk::AccessFlags::ShaderWrite;
							hzbRW.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
							hzbPass.AddBufferHint(m_ssrHzbLogical, hzbRW);
						}
						if (fromDepth) {
							// Depth sampled in compute (DepthStencilReadOnlyOptimal:
							// matches the bindless descriptor layout — see gfx's
							// AddHizPass note).
							smvk::ImageSyncRequest depthReq{};
							depthReq.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
							depthReq.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
							depthReq.layout = smvk::ImageLayout::DepthStencilReadOnlyOptimal;
							hzbPass.AddImageHint(
								m_viewportResources.renderDepth.logicalView, depthReq);
						}
						const uint32_t mip = m;
						hzbPass.SetRecordingFunc(
							[this, mip, depthBindless, fromDepth, ssrHzbAddr]
							(smvk::CommandBuffer& cmd) {
							uint32_t dw = 0, dh = 0, sw = 0, sh = 0;
							smvk::gfx::HzbMipDims(m_ssrHzbWidth, m_ssrHzbHeight,
								mip, dw, dh);
							smvk::gfx::HzbMipDims(m_ssrHzbWidth, m_ssrHzbHeight,
								fromDepth ? 0u : mip - 1u, sw, sh);
							smvk::gfx::HzbPush push{};
							push.hzb = ssrHzbAddr;
							push.dstMip = mip;
							push.dstWidth = dw;
							push.dstHeight = dh;
							push.dstOffset = m_ssrHzbMipOffsets[mip];
							push.depthTextureIndex = depthBindless;
							if (fromDepth) {
								push.srcMip = smvk::gfx::kHizSrcDepth;
								push.srcWidth = m_ssrHzbWidth;
								push.srcHeight = m_ssrHzbHeight;
								push.srcOffset = 0;
							}
							else {
								push.srcMip = mip - 1;
								push.srcWidth = sw;
								push.srcHeight = sh;
								push.srcOffset = m_ssrHzbMipOffsets[mip - 1];
							}
							cmd.BindComputePipeline(m_ssrHzbProgram);
							cmd.PushConstants(0, sizeof(push), &push,
								smvk::ShaderStage::Compute);
							cmd.Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
						});
					}
				}
			}
			// Temporal path needs the motion-vector target (reprojection
			// source); without it the record takes the spatial-only path
			// and the TAA history invalidates at the end of the frame.
			// TEMPORARY: the temporal (TAA) accumulation path is COMMENTED
			// OUT (spatial-only while the smear diagnosis is open): the
			// record always takes AddSsrPasses below. The history state
			// machine (m_ssrTaa) keeps warming up/invalidating through the
			// spatial records, so re-enabling = flip this back to true
			// (the temporal program, history targets and BlendSettings
			// are untouched).
			const bool taaUsable = false; // SsrTaaRequested()
				// && m_passes.motionVectors && m_viewportResources.motionVectors.Valid()
				// && !m_viewportResources.motionVectors.target.logicalView.empty()
				// && m_viewportResources.motionVectors.target.bindlessIndex != UINT32_MAX;
			if (taaUsable) {
				const smvk::SsrTaaFrameParams taaParams =
					smvk::SsrTaaBeginFrame(m_ssrTaa, true);
				// Animated ray-origin dither: the record counter decorrelates
				// consecutive traces so the temporal pass converges speckle
				// to its mean (static dither would just park the pattern).
				const uint32_t ssrDitherEpoch = static_cast<uint32_t>(m_frameCounter & 0xffffffffu);
			m_viewportResources.ssr.AddSsrTemporalPasses(
				*m_rendergraph,
				sceneHdrView,
				sceneHdrBindless,
				m_viewportResources.renderDepth.logicalView,
				m_viewportResources.renderDepth.bindlessIndex,
				gbufferLayerOn ? m_viewportResources.ssr.gbuffer.logical
					: smvk::LogicalImageViewHandle{},
				gbufferLayerOn ? m_viewportResources.ssr.gbuffer.bindlessIndex
					: UINT32_MAX,
				ssrHzbAddr,
				ssrHzbMips,
				ssrHzbLogical,
				m_viewportResources.motionVectors.target.logicalView,
					m_viewportResources.motionVectors.target.bindlessIndex,
					frameUniformsAddress,
					linearSampler,
					ssrDesc,
					m_jitterDeltaUV.x,
					m_jitterDeltaUV.y,
					m_ssrTaaBlend,
					taaParams.historyValid,
					taaParams.parity,
					"SSR",
					"SSRBlur",
					"SSRTemporal",
					"SSRComposite",
					ssrDitherEpoch);
				m_ssrFrameRanTemporal = true;
} else {
			const uint64_t rayListAddr = m_viewportResources.ssr.rayList.buffer.empty() ? 0 :
				m_context->GetBufferDeviceAddress(m_viewportResources.ssr.rayList.buffer);
			const uint64_t rayCounterAddr = m_viewportResources.ssr.rayCounter.buffer.empty() ? 0 :
				m_context->GetBufferDeviceAddress(m_viewportResources.ssr.rayCounter.buffer);
			const uint64_t radianceAddr = m_viewportResources.ssr.radiance.buffer.empty() ? 0 :
				m_context->GetBufferDeviceAddress(m_viewportResources.ssr.radiance.buffer);
			const uint64_t indirectArgsAddr = m_viewportResources.ssr.indirectArgs.buffer.empty() ? 0 :
				m_context->GetBufferDeviceAddress(m_viewportResources.ssr.indirectArgs.buffer);

			m_viewportResources.ssr.AddSsrPasses(
				*m_rendergraph,
				sceneHdrView,
				sceneHdrBindless,
				m_viewportResources.renderDepth.logicalView,
				m_viewportResources.renderDepth.bindlessIndex,
				gbufferLayerOn ? m_viewportResources.ssr.gbuffer.logical
					: smvk::LogicalImageViewHandle{},
				gbufferLayerOn ? m_viewportResources.ssr.gbuffer.bindlessIndex
					: UINT32_MAX,
				rayListAddr,
				rayCounterAddr,
				radianceAddr,
				indirectArgsAddr,
				ssrHzbAddr,
				ssrHzbMips,
				ssrHzbLogical,
				frameUniformsAddress,
				linearSampler,
				ssrDesc,
				"SSR",
				"SSRBlur",
				"SSRComposite");
			m_ssrFrameRanTemporal = false;
			}
			sceneHdrView = m_viewportResources.ssr.OutputView();
			sceneHdrBindless = m_viewportResources.ssr.OutputBindless();
			ssrEmitted = true;

			// Ground-truth depth view (VULKANTOY_SSR_DUMP armed): sample the
			// depth through the trace's exact bindless path and write it to
			// the raw target. Raw is dead after SSRBlur (its only reader),
			// so this pass owns raw's final content for the dump frames —
			// the ssr_raw dump slot then shows DEPTH-AS-SSR-SEES-IT, not
			// the (copy-based, layout-fragile) staging readback.
			if (m_ssrDumpAtFrame >= 0
				&& m_viewportResources.renderDepth.bindlessIndex != UINT32_MAX) {
				if (m_ssrDepthViewProgram.shaderCount == 0) {
					try {
						auto vs = m_context->CreateShaderModule(
							readFile("content/shaders/tonemap.vert.hlsl.spv"),
							"ssrDepthViewVert");
						auto ps = m_context->CreateShaderModule(
							readFile("content/shaders/ssr_depthview.frag.hlsl.spv"),
							"ssrDepthViewFrag");
						if (!vs.empty() && !ps.empty()) {
							smvk::Shader vsS{ .module = vs, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
							smvk::Shader psS{ .module = ps, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
							m_ssrDepthViewProgram = { .shaders = { vsS, psS }, .shaderCount = 2 };
							m_ssrDepthViewVs = std::move(vs);
							m_ssrDepthViewPs = std::move(ps);
						}
					} catch (...) {
					}
				}
				if (m_ssrDepthViewProgram.shaderCount != 0) {
					struct SsrDepthViewPush {
						uint32_t depthTextureIndex;
						uint32_t pad0;
						uint32_t pad1;
						uint32_t pad2;
					};
					static_assert(sizeof(SsrDepthViewPush) == 16);
					SsrDepthViewPush push{};
					push.depthTextureIndex =
						m_viewportResources.renderDepth.bindlessIndex;
					smvk::ShaderProgram progCopy = m_ssrDepthViewProgram;
					smvk::RenderPass& depthViewPass =
						m_rendergraph->AddPass("SsrDepthView");
					depthViewPass.SetOffset(0, 0)
						.SetExtent(m_viewportResources.ssr.halfExtent)
						.SetInputImages({ m_viewportResources.renderDepth.logicalView })
						.SetColor({ m_viewportResources.ssr.raw.logical },
							smvk::AttachmentLoadOp::DontCare,
							smvk::AttachmentStoreOp::Store);
					depthViewPass.SetRecordingFunc(
						[progCopy, push](smvk::CommandBuffer& cmd) mutable {
							cmd.BindRenderPipeline(
								progCopy,
								smvk::VertexFormat::Empty,
								smvk::DepthSettings::Disabled(),
								smvk::StencilSettings::DefaultDisabled(),
								smvk::BlendSettings::Opaque(),
								smvk::MSAASettings::Count1(),
								smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
							cmd.PushConstants(0, sizeof(SsrDepthViewPush), &push,
								smvk::ShaderStage::All);
							cmd.Draw(3, 1, 0, 0);
						});
				std::fprintf(stderr,
					"[SSR-dump] depth-view pass armed: ssr_raw will hold the sampled depth\n");
			}
			// Ground-truth stencil view (VULKANTOY_SSR_DUMP armed +
			// VULKANTOY_SSR_STENCILVIEW): loads the receiver-material
			// stencil through the trace's exact uintTextures[] path and
			// writes it into the (post-composite dead) blurred target —
			// R = the raw byte (0..255), G/B = the decoded STENCIL_SSR /
			// STENCIL_SSR_METAL flags. The ssr_blur dump slot then shows
			// STENCIL-AS-SSR-SEES-IT (no reliance on the layout-fragile
			// raw depth/stencil readback).
			static const bool sSsrStencilView =
				std::getenv("VULKANTOY_SSR_STENCILVIEW") != nullptr;
			static const int sSsrStencilViewSrc =
				std::getenv("VULKANTOY_SSR_STENCILVIEW") != nullptr
				? std::atoi(std::getenv("VULKANTOY_SSR_STENCILVIEW")) : 1;
			if (m_ssrDumpAtFrame >= 0 && sSsrStencilView && gbufferLayerOn) {
				if (m_ssrStencilViewProgram.shaderCount == 0) {
					try {
						auto vs = m_context->CreateShaderModule(
							readFile("content/shaders/tonemap.vert.hlsl.spv"),
							"ssrStencilViewVert");
						auto ps = m_context->CreateShaderModule(
							readFile("content/shaders/ssr_stencilview.frag.hlsl.spv"),
							"ssrStencilViewFrag");
						if (!vs.empty() && !ps.empty()) {
							smvk::Shader vsS{ .module = vs, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
							smvk::Shader psS{ .module = ps, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
							m_ssrStencilViewProgram = { .shaders = { vsS, psS }, .shaderCount = 2 };
							m_ssrStencilViewVs = std::move(vs);
							m_ssrStencilViewPs = std::move(ps);
						}
					} catch (...) {
					}
				}
				if (m_ssrStencilViewProgram.shaderCount != 0) {
					struct SsrStencilViewPush {
						uint32_t srcSelect;     // 1 stencil(uint) 2 depth(uint) 3 stencil(float)
						uint32_t stencilTextureIndex;
						uint32_t depthTextureIndex;
						uint32_t pad0;
					};
					static_assert(sizeof(SsrStencilViewPush) == 16);
					SsrStencilViewPush push{};
					push.srcSelect = sSsrStencilViewSrc >= 1 && sSsrStencilViewSrc <= 3
						? static_cast<uint32_t>(sSsrStencilViewSrc) : 1u;
					push.stencilTextureIndex =
						m_viewportResources.renderDepth.stencilBindlessIndex;
					push.depthTextureIndex =
						m_viewportResources.renderDepth.bindlessIndex;
					smvk::ShaderProgram progCopy = m_ssrStencilViewProgram;
					smvk::RenderPass& stencilViewPass =
						m_rendergraph->AddPass("SsrStencilView");
					stencilViewPass.SetOffset(0, 0)
						.SetExtent(m_viewportResources.ssr.halfExtent)
						.SetInputImages({ m_viewportResources.renderDepth.stencilLogicalView })
						.SetColor({ m_viewportResources.ssr.blurred.logical },
							smvk::AttachmentLoadOp::DontCare,
							smvk::AttachmentStoreOp::Store);
					stencilViewPass.SetRecordingFunc(
						[progCopy, push](smvk::CommandBuffer& cmd) mutable {
							cmd.BindRenderPipeline(
								progCopy,
								smvk::VertexFormat::Empty,
								smvk::DepthSettings::Disabled(),
								smvk::StencilSettings::DefaultDisabled(),
								smvk::BlendSettings::Opaque(),
								smvk::MSAASettings::Count1(),
								smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
							cmd.PushConstants(0, sizeof(SsrStencilViewPush), &push,
								smvk::ShaderStage::All);
							cmd.Draw(3, 1, 0, 0);
						});
					std::fprintf(stderr,
						"[SSR-dump] stencil-view pass armed: ssr_blur will hold the sampled stencil\n");
				}
			}
		}
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
				&& m_volumetricDensity > 0.0f && m_volumetricIntensity > 0.0f
				// Debug views: fog would Beer-Lambert the flat readout
				// and add scatter on top (DebugVisualizationActive).
				&& !DebugVisualizationActive();
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
			dlssIn.cameraFar = m_sceneFarClip;
			dlssIn.cameraFOV = glm::radians(kCameraFovYDegrees);
			dlssIn.cameraAspectRatio =
				float(renderExtent.width) / float(renderExtent.height);
			dlssIn.reset = dlssReset;

			// SL frame token must be unique per rendered frame: use the
			// monotonic render-frame counter, never the wrapping swapchain
			// slot (fb.frameIndex cycles 0..N-1, so reusing it makes SL see
			// different constants under the same token -> result 27
			// eErrorDuplicatedConstants "Setting different 'common'
			// constants multiple times within the same frame").
			const uint32_t dlssFrameId = static_cast<uint32_t>(m_frameCounter & 0xffffffffu);
			dlssEmitted = smvk::AddDlssPasses(*m_rendergraph, *m_context,
				m_viewportResources.dlss, dlssIn, frameExposure, dlssFrameId, "DLSS");
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
		tonemapDesc.tonemapMode = m_tonemapMode;
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
					// Texture filtering (renderer state): bilinear = linear
					// within a mip, nearest between mips; trilinear adds
					// linear between mips (default). Applies to the bindless
					// texture samplers (slots 0 + 2) via in-place recreate.
					{
						const char* filtNames[] = { "Bilinear", "Trilinear" };
						int filtIdx = (m_textureFiltering == TextureFiltering::Bilinear) ? 0 : 1;
						if (ImGui::Combo("Texture filtering", &filtIdx, filtNames, 2))
							SetTextureFiltering(filtIdx == 0
								? TextureFiltering::Bilinear
								: TextureFiltering::Trilinear);
						if (filtIdx == 0)
							ImGui::TextDisabled("Bilinear: sharper mip transitions, slight shimmer at grazing angles.");
						else
							ImGui::TextDisabled("Trilinear: smooth mip blends (default).");
					}
				}
				if (ImGui::CollapsingHeader("Exposure — histogram eye adaptation", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Auto-exposure (X)", &m_autoExposureEnabled);
					{
						static const char* const kTonemapNames[4] =
							{ "Reinhard", "ACES", "Uncharted2", "GT7" };
						int tonemapIdx = std::clamp(static_cast<int>(m_tonemapMode), 0, 3);
						if (ImGui::Combo("Tonemap operator", &tonemapIdx, kTonemapNames, 4)) {
							m_tonemapMode = static_cast<uint32_t>(std::clamp(tonemapIdx, 0, 3));
							m_viewportResources.tonemap.desc.tonemapMode = m_tonemapMode;
						}
						ImGui::TextDisabled("GT7: Polyphony Digital SDR (250-nit paper white, ICtCp chroma preserve).");
					}
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
					ImGui::Checkbox("Sky dome (K)", &m_skyEnabled);
					// Ambient-source selection moved to the GI section (the
					// sky IBL is one AMBIENT source among the isolatable
					// set — N cycles, see AmbientMode).
					ImGui::TextDisabled("Ambient source: %s (N cycles; GI section)",
						AmbientModeName(m_ambientMode));
					// Scene-backed look (same fields + units as the editor
					// LIGHTING panel): sun 0..30, sky 0..2, fog 0..1. The
					// renderer owns no environment — these write the
					// world's scene directly (any drag marks it authored).
					// No home here for the old HDR-unit knobs (sky
					// exponent, IBL/ground intensities, sun disc, fog
					// shape): the look authority is SceneEnv now.
					if (!m_world) {
						ImGui::TextDisabled("No world — nothing to author.");
					}
					else {
						SceneEnv& se = m_world->scene.environment;
						auto authored = [&]() { m_world->scene.hasEnvironment = true; };
						if (ImGui::ColorEdit3("Sky color", &se.skyColor.x)) authored();
						if (ImGui::SliderFloat("Sky intensity", &se.skyIntensity, 0.0f, 2.0f)) authored();
						ImGui::Separator();
						ImGui::Text("Sun (drives light + sky disc)");
						if (ImGui::SliderFloat("Azimuth", &se.sunAzimuth, -180.0f, 180.0f)) authored();
						if (ImGui::SliderFloat("Elevation", &se.sunElevation, -10.0f, 90.0f)) authored();
						if (ImGui::ColorEdit3("Sun color", &se.sunColor.x)) authored();
						if (ImGui::SliderFloat("Sun intensity", &se.sunIntensity, 0.0f, 30.0f)) authored();
						ImGui::Separator();
						ImGui::Text("Fog (authored; analytic fog stays off — see SceneEnv)");
						if (ImGui::ColorEdit3("Fog color", &se.fogColor.x)) authored();
						if (ImGui::SliderFloat("Fog density", &se.fogDensity, 0.0f, 1.0f)) authored();
					}
					if (!m_skydome.ShadersValid())
						ImGui::TextDisabled("Sky program missing ï¿½ dome skipped");
				}
				if (ImGui::CollapsingHeader("Reflection cubemaps (specular IBL)")) {
					ImGui::Checkbox("Cubemap specular IBL", &m_cubemapEnabled);
					ImGui::SliderFloat("Cubemap intensity", &m_cubemapIntensity, 0.0f, 4.0f, "%.2f");
					ImGui::SliderFloat("Cubemap LOD bias", &m_cubemapLodBias, -2.0f, 4.0f, "%.2f");
					ImGui::Checkbox("Probe locations overlay", &m_cubemapShowProbes);
					if (m_cubemapShowProbes)
						ImGui::SliderFloat("Overlay max dist (m)", &m_cubemapOverlayMaxDist, 5.0f, 300.0f, "%.0f");
					{
						static const char* const kCubeDbgNames[3] = {
							"Off", "Cubemap only (no Fresnel)", "Probe weight heat",
						};
						int dbgIdx = std::clamp(m_cubemapDebug, 0, 2);
						if (ImGui::Combo("Cubemap debug", &dbgIdx, kCubeDbgNames, 3))
							m_cubemapDebug = std::clamp(dbgIdx, 0, 2);
					}
					size_t bakedCount = 0, procCount = 0;
					for (const auto& [name, entry] : m_cubemaps.entries) {
						if (entry.bindless >= 0 && !entry.texture.empty())
							(entry.baked ? bakedCount : procCount)++;
					}
					ImGui::Text("Cubemaps: %u (baked %u, procedural %u)  Probes: %u (light tail)  Sampler: %u",
						(unsigned)m_cubemaps.entries.size(),
						(unsigned)bakedCount, (unsigned)procCount,
						(unsigned)ProbeEntryCount(), m_cubemapSampler);
					{
						const bool tiled = ProbeTileListsWanted();
						ImGui::Text("Tiled probes: %s (stride %u shared, tiles %ux%u)",
							tiled ? "fresh" : "stale (global cubemap)",
							FP_MAX_PER_TILE,
							m_viewportResources.forwardPlus.tileCountX,
							m_viewportResources.forwardPlus.tileCountY);
						ImGui::TextDisabled("Probes ride the FP light buffer tail (type 2); pixels"
							" filter them out of the same tile walk the lights use."
							" Stale lists fall back to the global cubemap.");
					}
					for (const auto& [name, entry] : m_cubemaps.entries) {
						ImGui::BulletText("%s: %upx mips=%u bindless=%d %s",
							name.c_str(), entry.size, entry.mipLevels, entry.bindless,
							entry.baked ? "(baked)" : "(PROCEDURAL test pattern)");
					}
					for (size_t i = 0; i < m_cubemapProbes.size() && i < 32; ++i) {
						const auto& p = m_cubemapProbes[i];
						ImGui::BulletText("probe %u: pos=(%.1f,%.1f,%.1f) r=%.1f fade=%.1f cube=%d mips=%.0f",
							(unsigned)i,
							p.posInfluence.x, p.posInfluence.y, p.posInfluence.z,
							p.posInfluence.w, p.fadeCubemap.x,
							(int)p.fadeCubemap.y, (double)p.fadeCubemap.z);
					}
					if (procCount > 0)
						ImGui::TextDisabled("PROCEDURAL entries are the neon test pattern (face-orientation check),"
							" not a bake - expect wrong specular until cooked.");
					if (m_cubemaps.entries.empty())
						ImGui::TextDisabled("No cubemap: specular IBL uses the analytic sky."
							" Bake with PakCooker --cubemap on (grid writes a .cubemanifest).");
					ImGui::TextDisabled("Specular IBL is independent of the ambient-source isolation"
						" (N cycles isolate diffuse only). Baked cubemaps are TRUE radiance scaled"
						" only by this panel's intensity (mirrors match the sun-lit scene); the"
						" Environment IBL intensity scales the analytic-sky fallback only.");
					ImGui::TextDisabled("Probes upload once per scene load (see [Cubemap] log lines);"
						" intensity/debug/LOD apply instantly.");
				}
				if (ImGui::CollapsingHeader("GI ï¿½ ambient source + intensity", ImGuiTreeNodeFlags_DefaultOpen)) {
					// Ambient source (N cycles): All -> Lightmaps -> Probes
					// -> SkyIbl -> Off — single-source isolation for
					// bisection. R / F8 cycle the GI debug views (Debug
					// tab): flat irradiance + source palette.
					{
#if 0 // Lightmaps deprecated
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
#else
						// 5-entry table keyed by the RAW AmbientMode value
						// (All=0..Off=4). A previous cleanup shrank this to
						// 4 entries while still indexing by the raw enum —
						// Off (4) read kModeNames[4] out of bounds and the
						// garbage char* crashed ImGui's %s formatting
						// (ucrtbase AV at a fixed offset; only with the UI
						// pass on, only in Off). Keep the table full-size;
						// the deprecated Lightmaps row stays as a label (the
						// combo skips it below; N still cycles through it
						// and DegradeAmbientForMissingSources bounces it).
						static const char* const kModeNames[5] = {
							"All (baked GI; sky fallback)",
							"Lightmaps only (deprecated)",
							"Probes only (grids + volume)",
							"Sky IBL only",
							"Off (no GI at all)",
						};
						int modeIdx = static_cast<int>(m_ambientMode);
						if (ImGui::Combo("Ambient source (N)", &modeIdx, kModeNames, 5))
							m_ambientMode = static_cast<AmbientMode>(std::clamp(modeIdx, 0, 4));
						ImGui::TextDisabled("N cycles: %s\n"
							"sky IBL is the FALLBACK for uncovered pixels (never added on baked)\n"
							"sources loaded: probe volume %s, grids %s\n"
							"R / F8: GI debug views (1 flat irradiance, 2 palette)",
							kModeNames[static_cast<int>(m_ambientMode)],
							m_giLoaded ? "yes" : "no",
							m_regLoaded ? "yes" : "no");
#endif
					}
					ImGui::SliderFloat("GI intensity", &m_giIntensity, 0.0f, 2.0f, "%.2f");
#if 0 // Lightmaps deprecated
					if (!m_giLoaded && !m_lmLoaded && !m_regLoaded)
#else
					if (!m_giLoaded && !m_regLoaded)
#endif
						ImGui::TextDisabled("No baked GI: content/gi/%s.* missing",
							m_sceneId.empty() ? "?" : m_sceneId.c_str());
				}
				if (ImGui::CollapsingHeader("Bloom + AO + motion blur", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::Checkbox("Bloom (B)", &m_bloomEnabled);
					{
						static const char* const kAoNames[4] =
							{ "No AO", "SSAO", "GTAO", "SAO" };
						int aoIdx = static_cast<int>(m_aoMode);
						if (ImGui::Combo("AO mode (O)", &aoIdx, kAoNames, 4))
							m_aoMode = static_cast<AoMode>(std::clamp(aoIdx, 0, 3));
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
						} else if (m_aoMode == AoMode::Gtao) {
							ImGui::SliderInt("GTAO slices", &m_gtaoSlices, 1, 8);
							ImGui::SliderInt("GTAO steps / slice", &m_gtaoSteps, 1, 8);
							ImGui::SliderFloat("GTAO thickness (m)", &m_gtaoThickness, 0.0f, 5.0f);
						} else {
							ImGui::SliderInt("SAO samples", &m_saoSamples, 1, 32);
							ImGui::SliderInt("SAO spiral turns", &m_saoSpiralTurns, 1, 16);
						}
					}
				ImGui::Checkbox("Motion blur (M) (HDR velocity gather)", &m_motionBlurEnabled);
				if (m_motionBlurEnabled) {
					ImGui::SliderInt("Blur samples", &m_motionBlurSamples, 1, 32);
					ImGui::SliderFloat("Blur max radius (px)", &m_motionBlurMaxRadius, 0.0f, 64.0f);
					ImGui::SliderFloat("Blur strength", &m_motionBlurStrength, 0.0f, 2.0f);
					ImGui::Checkbox("Tile dilation (16px tiles, 5x5)", &m_motionBlurTileDilation);
					ImGui::TextDisabled("IGN-jittered taps (CoD:AW); dilation streaks\n"
						"revealed background with the foreground that vacated it.");
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
						ImGui::SliderFloat("SSR max distance (m)", &m_ssrMaxDistance, 1.0f, 200.0f);
						ImGui::SliderFloat("Thickness (m)", &m_ssrThickness, 0.01f, 2.0f);
						ImGui::SliderFloat("Intensity", &m_ssrIntensity, 0.0f, 4.0f);
						ImGui::SliderFloat("F0 (glass 0.04)", &m_ssrF0, 0.0f, 1.0f);
						ImGui::SliderFloat("Bilateral sharpness", &m_ssrBlurSharpness, 0.0f, 32.0f);
						ImGui::Checkbox("Temporal filter (TAA, reflection layer only)", &m_ssrTaaEnabled);
						if (m_ssrTaaEnabled) {
							ImGui::SliderFloat("TAA history blend", &m_ssrTaaBlend, 0.0f, 0.98f);
							ImGui::TextDisabled("Motion-vector reprojection + color-AABB anti-ghost. Needs motion vectors; falls back to spatial only.");
						}
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
						ImGui::SliderFloat("Fog max distance (m)", &m_volumetricMaxDistance, 5.0f, 200.0f);
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
					if (!m_cookedLights.pointSpots.empty() || m_cookedLights.hasSun) {
						ImGui::Checkbox("Use cooked scene lights", &m_useCookedLights);
						ImGui::TextDisabled("Cooked: %u point/spot%s%s",
							(unsigned)m_cookedLights.pointSpots.size(),
							m_cookedLights.hasSun ? " + sun" : "",
							CookedLightsActive() ? " (active — procedural off)"
								: " (off — procedural fallback)");
					}
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
			if (ImGui::CollapsingHeader("Particles (GPU emitters)", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Checkbox("Enable particles", &m_particlesEnabled);
				ImGui::SameLine();
				ImGui::TextDisabled(ParticlesActive() ? "(active)" : "(off)");
				if (m_passes.deferredShadows)
					ImGui::TextDisabled("(deferred shadows own the lights: "
						"tile lists bypassed, capped light loop)");
				ImGui::SliderFloat("Gravity Y (m/s^2)", &m_particleDemo.gravityY, -20.0f, 5.0f);
				ImGui::Text("Emitters: %u / %u   steady-state ~%.0f particles",
					(unsigned)m_particleDemo.emitters.size(), PARTICLE_MAX_EMITTERS,
					m_particleDemo.SteadyStateCount());
				const int emitterCount = (int)m_particleDemo.emitters.size();
				if (emitterCount > 0) {
					m_particleDemo.selected = std::min(
						std::max(m_particleDemo.selected, 0), emitterCount - 1);
					if (ImGui::BeginCombo("Edit emitter", std::to_string(
							m_particleDemo.selected).c_str())) {
						for (int i = 0; i < emitterCount; ++i) {
							const bool sel = (i == m_particleDemo.selected);
							if (ImGui::Selectable(std::to_string(i).c_str(), sel))
								m_particleDemo.selected = i;
							if (sel)
								ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
					auto& e = m_particleDemo.emitters[m_particleDemo.selected];
					ImGui::SliderFloat3("Position", &e.position.x, -25.0f, 25.0f);
					ImGui::SliderFloat("Spawn radius (m)", &e.position.w, 0.0f, 8.0f);
					ImGui::SliderFloat3("Axis", &e.direction.x, -1.0f, 1.0f);
					ImGui::SliderFloat("Spread (rad)", &e.direction.w, 0.0f, 3.14f);
					ImGui::ColorEdit3("Color", &e.color.x);
					ImGui::SliderFloat("Emissive intensity", &e.color.w, 0.0f, 20.0f);
					ImGui::SliderFloat("Speed min", &e.velocity.x, 0.0f, 10.0f);
					ImGui::SliderFloat("Speed max", &e.velocity.y, 0.0f, 10.0f);
					ImGui::SliderFloat("Drag (1/s)", &e.velocity.z, 0.0f, 4.0f);
					ImGui::SliderFloat("Gravity scale", &e.velocity.w, -2.0f, 2.0f);
					ImGui::SliderFloat("Size start (m)", &e.size.x, 0.001f, 0.3f, "%.3f");
					ImGui::SliderFloat("Size end (m)", &e.size.y, 0.001f, 0.5f, "%.3f");
					ImGui::SliderFloat("Spin (rad/s)", &e.size.z, 0.0f, 8.0f);
					ImGui::SliderFloat("Rate (/s)", &e.life.z, 0.0f, 500.0f);
					ImGui::SliderFloat("Life min (s)", &e.life.x, 0.1f, 10.0f);
					ImGui::SliderFloat("Life max (s)", &e.life.y, 0.1f, 10.0f);
					ImGui::SliderFloat("Fade in", &e.fade.x, 0.0f, 0.5f);
					ImGui::SliderFloat("Fade out", &e.fade.y, 0.0f, 0.9f);
					ImGui::SliderFloat("Color jitter", &e.fade.z, 0.0f, 1.0f);
					const char* shapes[] = { "Cone", "Sphere", "Point" };
					int shape = (int)e.life.w;
					if (ImGui::Combo("Shape", &shape, shapes, 3))
						e.life.w = (float)shape;
					ImGui::TextDisabled("CPU owns only these parameters: spawn carry, "
						"simulation and drawing are all GPU-side.");
				}
				if (ImGui::Button("Add emitter") &&
						m_particleDemo.emitters.size() < PARTICLE_MAX_EMITTERS) {
					m_particleDemo.emitters.push_back(smvk::ParticleEmitterGpu{});
					m_particleDemo.selected = (int)m_particleDemo.emitters.size() - 1;
				}
				ImGui::SameLine();
				if (ImGui::Button("Remove") && !m_particleDemo.emitters.empty()) {
					m_particleDemo.emitters.erase(
						m_particleDemo.emitters.begin() + m_particleDemo.selected);
					m_particleDemo.selected = std::max(0, m_particleDemo.selected - 1);
				}
				ImGui::TextDisabled("Pool %u particles, emit+sim+draw all compute/GPU "
					"resident (dead slots collapse in the VS).",
					(unsigned)PARTICLE_MAX);
			}
				if (ImGui::CollapsingHeader("Directional CSM", ImGuiTreeNodeFlags_DefaultOpen)) {
				// Tile resolution (depth + EVSM moment atlases recreate once
				// on change via the per-frame Ensure; one-time hitch).
				{
					const uint32_t resOptions[] = { 512, 1024, 2048, 4096 };
					const char* resNames[] = { "512", "1024", "2048", "4096" };
					int resIdx = 2;
					for (int i = 0; i < 4; ++i)
						if (resOptions[i] == m_cascadeResolution)
							resIdx = i;
					if (ImGui::Combo("Cascade resolution", &resIdx, resNames, 4))
						m_cascadeResolution = resOptions[std::clamp(resIdx, 0, 3)];
					const auto tile = CascadeTileExtent();
					ImGui::TextDisabled("Live tile %ux%u (atlas %ux%u).",
						tile.width, tile.height,
						m_viewportResources.directionalShadows.atlasExtent.width,
						m_viewportResources.directionalShadows.atlasExtent.height);
				}
				ImGui::SliderFloat("Split lambda", &m_cascadeLambda, 0.0f, 1.0f, "%.2f");
				ImGui::TextDisabled("0 = linear splits, 1 = logarithmic (starves the far cascade).");
				ImGui::SliderFloat("Shadow distance (m, 0 = scene far)", &m_cascadeMaxDistance, 0.0f, 500.0f, "%.0f");
				ImGui::Text("Effective far: %.1f m (scene far %.1f m)",
					(double)CascadeFar(), (double)m_sceneFarClip);
				ImGui::SliderFloat("Depth bias scale", &m_cascadeDepthBiasScale, 0.0f, 4.0f, "%.2f");
				ImGui::SliderFloat("Normal bias scale", &m_cascadeNormalBiasScale, 0.0f, 4.0f, "%.2f");
				ImGui::TextDisabled("Scales the texel-sized per-cascade auto bias "
					"(up = fewer acne, more peter-panning).");
				for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
					ImGui::Text(" cascade %u: [%.2f .. %.2f] depthBias %.5f normalBias %.4f",
						i, (double)m_cascadeSplitsDbg[i], (double)m_cascadeSplitsDbg[i + 1],
						(double)m_cascadeBiasDbg[i], (double)m_cascadeNormalBiasDbg[i]);
				}
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
				// EVSM (Exponential Variance Shadow Maps): RGBA32F 2xMSAA
				// moments (M1/M2 of both exponential warps) + async separable
				// blur, 1-frame-lag pipelined. Off = PCF depth atlases.
				// Needs RGBA32F 2xMSAA + linear-filter support.
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
				ImGui::TextDisabled("RGBA32F 2xMSAA moments (dual Cantelli), 9-tap Gaussian H+V (async);"
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
				b = (m_gfxRenderer.config.cullFlags & smvk::gfx::CullFlagBit_HzbSkipMasked) != 0;
				if (ImGui::Checkbox("Exclude MASK from HZB test", &b))
					m_gfxRenderer.config.cullFlags = b
					? m_gfxRenderer.config.cullFlags | smvk::gfx::CullFlagBit_HzbSkipMasked
					: m_gfxRenderer.config.cullFlags & ~smvk::gfx::CullFlagBit_HzbSkipMasked;
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
			ImGui::Checkbox("Full-rate LOD updates", &m_lodFullRate);
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"On (default): every node re-evaluates its LOD every frame.\n"
					"Off: halfrate refresh - half the nodes per frame, decisions\n"
					"up to 1 frame stale (the old default). Selection is hysteretic\n"
					"around the pixel budget either way, so levels don't flip-flop\n"
					"at switch distances.");
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
			if (ImGui::CollapsingHeader("Debug views (bypass GPU cull, F1..F6, F9..F11)", ImGuiTreeNodeFlags_DefaultOpen)) {
				{
					const char* viewNames[] = {
						"F1 Lit", "F2 Unlit", "F3 Meshlets",
						"F4 Cascades", "F5 LODs", "F6 Vertex Colors",
						"F9 UV0 (base)", "F10 UV1 (lightmap)",
						"F11 POM (parallax occlusion)",
					};
					int view = static_cast<int>(m_debugView);
					if (ImGui::Combo("Debug view", &view, viewNames, 9))
						m_debugView = static_cast<DebugView>(view);
				}
					ImGui::TextDisabled("F6 vertex colors: layered materials show normalized weights as R/G/B/Y per layer 0..3;\n"
						"legacy materials show the raw vertex tint.");
			ImGui::TextDisabled("F9/F10 UV checkers: base TEXCOORD0 vs lightmap atlas uv (chart uv + chart id,\n"
				"unpacked in the VS via the draw's chart offset). Magenta glow = huge uv\n"
				"screen-derivative (a chart corner landing in the wrong atlas rect).\n"
				"UV1 stripes magenta when the atlas uv leaves [0,1] (transform bug).");
				ImGui::TextDisabled("F11 POM: ray-marched parallax uv through the occlusion-map heightfield\n"
					"(AO 1 = surface, 0 = cavity) + sun self-shadowing. Sun/sky shading at the displaced\n"
					"uv; baked GI / decals / forward+ off so the only delta vs F1 is the technique itself.\n"
					"Materials without an occlusion map degenerate to plain sampling.");
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
					ImGui::Checkbox("GI volume bounds (wireframe)", &m_giShowVolumeBounds);
					if (m_giShowVolumeBounds) {
						ImGui::TextDisabled("octree AABB (cyan) + regular OBBs (green) +\n"
							"convex grid AABBs (priority hue); no depth test,\n"
							"drawn over the current view");
					}
			if (m_giLoaded || m_regLoaded || m_cvolLoaded) {
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
					ImGui::TextDisabled("source palette: red = lightmap (deprecated), "
						"green = regular grid (legacy), blue = octree world field, "
						"cyan = convex volume blended, magenta = none");
				}
			if (m_cvolLoaded) {
				ImGui::Checkbox("Convex GI volumes", &m_cvolEnabled);
				ImGui::TextDisabled("stacked probe volumes blend over the "
					"octree by priority (gi/<scene>.cvmanifest)");
			}
#if 0 // Lightmaps deprecated
			// Lightmaps: proxy-baked .sglm atlas content (PakCooker
			// --lightmap: one voxel-remesh proxy bake, UV-transferred
			// to the real meshes). Off until opted in per view.
			if (m_lmLoaded) {
				ImGui::Checkbox("Lightmaps", &m_lmEnabled);
			}
#endif
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
					}
					else {
						ImGui::TextDisabled("Streaming: off (full-resident)");
					}
				}
				if (ImGui::CollapsingHeader("Shadows ï¿½ CSM + spots + deferred", ImGuiTreeNodeFlags_DefaultOpen)) {
					const auto& atlas = m_viewportResources.directionalShadows;
					ImGui::Text("CSM: %u cascades  tile %ux%u  atlas %ux%u  lambda %.2f  far %.1fm",
						NUM_CASCADES, CascadeTileExtent().width, CascadeTileExtent().height,
						atlas.atlasExtent.width, atlas.atlasExtent.height,
						(double)m_cascadeLambda, (double)CascadeFar());
					for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
						ImGui::Text(" cascade %u: [%.2f .. %.2f] depthBias %.5f normalBias %.4f",
							i, (double)m_cascadeSplitsDbg[i], (double)m_cascadeSplitsDbg[i + 1],
							(double)m_cascadeBiasDbg[i], (double)m_cascadeNormalBiasDbg[i]);
					}
					const glm::vec3 toSunDbg = frameEnv.SunDirectionToSun();
					ImGui::Text(" sun toSun: %.2f %.2f %.2f  int %.0f lux",
						toSunDbg.x, toSunDbg.y, toSunDbg.z, frameEnv.sunIntensity);
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
							m_sceneId.empty() ? "?" : m_sceneId.c_str());
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
					const auto& sao = m_viewportResources.sao;
					{
						static const char* const kAoNames[4] =
							{ "No AO", "SSAO", "GTAO", "SAO" };
						ImGui::TextDisabled("AO mode: %s",
							kAoNames[static_cast<int>(m_aoMode)]);
					}
					if (m_aoMode == AoMode::Ssao) {
						if (!ssao.Valid())
							ImGui::TextDisabled("SSAO target/shaders missing — no AO");
						else
							ImGui::Text("SSAO: half %ux%u (full %ux%u)",
								ssao.halfExtent.width, ssao.halfExtent.height,
								ssao.fullExtent.width, ssao.fullExtent.height);
					} else if (m_aoMode == AoMode::Gtao) {
						if (!gtao.Valid())
							ImGui::TextDisabled("GTAO target/shaders missing — no AO");
						else
							ImGui::Text("GTAO: half %ux%u (full %ux%u) %ux%u",
								gtao.halfExtent.width, gtao.halfExtent.height,
								gtao.fullExtent.width, gtao.fullExtent.height,
								(uint32_t)std::clamp(m_gtaoSlices, 1, 8),
								(uint32_t)std::clamp(m_gtaoSteps, 1, 8));
					} else if (m_aoMode == AoMode::Sao) {
						if (!sao.Valid())
							ImGui::TextDisabled("SAO target/shaders missing — no AO");
						else
							ImGui::Text("SAO: half %ux%u (full %ux%u) %u taps %u turns",
								sao.halfExtent.width, sao.halfExtent.height,
								sao.fullExtent.width, sao.fullExtent.height,
								(uint32_t)std::clamp(m_saoSamples, 1, 32),
								(uint32_t)std::clamp(m_saoSpiralTurns, 1, 16));
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
						ImGui::TextDisabled("SSR: %s %u steps intensity %.2f TAA:%s",
							m_ssrEnabled ? "ON" : "off",
							(uint32_t)std::clamp(m_ssrSteps, 1, 128), m_ssrIntensity,
							m_ssrTaaEnabled ? "on" : "off");
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
					const int wantLights = CookedLightsActive()
						? (int)m_cookedLights.pointSpots.size()
						: (m_fpDemo.enabled
							? (m_fpDemo.gridX * m_fpDemo.gridY * m_fpDemo.gridZ + m_fpDemo.spotCount) : 0);
				ImGui::Text("Lights: %d / %u%s%s", wantLights, FP_MAX_LIGHTS,
					wantLights > (int)FP_MAX_LIGHTS ? "  (CLAMPED!)" : "",
					CookedLightsActive() ? "  (cooked)" : "");
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
						m_hizOccluderDepth ? "occluder"
							: (m_gfxRenderer.SeededThisFrame() ? "seed" : "full"));
					if (!occOn)
						ImGui::TextDisabled("HZB flag OFF: no occlusion culls possible.");
					else if (hm == 0)
						ImGui::TextDisabled("HZB empty: occlusion test disabled (extent 0?).");
					else if (!depthOk)
						ImGui::TextDisabled("Depth bindless INVALID: HZB reads garbage.");
					if ((m_gfxRenderer.config.cullFlags
							& smvk::gfx::CullFlagBit_HzbSkipMasked) != 0)
						ImGui::TextDisabled("MASK draws excluded from the HZB test (cutout poison guard).");
					}
				}
			if (ImGui::CollapsingHeader("LODs", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Text("Cooked: %u meshes / %u siblings | draws: %llu (%llu switched, %llu vertex-lit)",
					m_lodChainMeshes, m_lodChainLods,
					(unsigned long long)m_lodDraws, (unsigned long long)m_lodSwitched,
					(unsigned long long)m_lodShaderDraws);
			ImGui::TextDisabled("Transition = projected simplifier error (world err * H / "
				"(2*tan(fov/2)*dist) <= budget), hysteretic around the budget. "
				"Every level registers its own gfx mesh: the selected LOD draws "
				"through the HZB-culled indirect ranges.");
			ImGui::TextDisabled("Refresh: %s (VULKANTOY_LODFULL=0 restores halfrate).",
				m_lodFullRate ? "full-rate, every node per frame"
					: "halfrate parity split");
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
				ImGui::TextDisabled("Keys: M blur  O AO (No/SSAO/GTAO/SAO)  B bloom  X exposure  V vectors");
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

		// Frame-time histogram (F12): SEPARATE UI from the debug panel —
		// a full-width bottom strip (see profiler::DrawFrameHistogramUi).
		// Zero cost while disabled: one flag check.
		if (smvk::profiler::FrameHistogramEnabled())
			smvk::profiler::DrawFrameHistogramUi();

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

		// GI volume bounding-box wireframe (m_giShowVolumeBounds): AABB/OBB
		// edges through the UNJITTERED viewProj into the ImGui foreground
		// list, so they ride on top of the current view with no depth test
		// (debug intent: authoring bounds against walls/roofs). Sources:
		// octree world field (m_giVolume.boundsMin/Max, cyan), regular
		// OBB volumes (green), convex grid AABBs (priority hue, like the
		// editor LIGHTING preview). Edges whose both endpoints project
		// (in front of the camera) draw; partially behind-camera boxes
		// clip per-edge (may drop the crossing segment).
		if (m_giShowVolumeBounds
			&& (m_giLoaded || m_regLoaded || m_cvolLoaded)) {
			ImDrawList* bdl = ImGui::GetForegroundDrawList();
			const ImVec2 bdisp = ImGui::GetIO().DisplaySize;
			const auto projectBox = [&](const glm::vec3& w, ImVec2& out) {
				const glm::vec4 clip =
					m_unjitteredViewProj * glm::vec4(w, 1.0f);
				if (clip.w <= 1e-4f)
					return false; // behind the camera
				const glm::vec3 ndc = glm::vec3(clip) / clip.w;
				if (std::abs(ndc.x) > 1.2f || std::abs(ndc.y) > 1.2f
					|| ndc.z < -1.0f || ndc.z > 1.0f)
					return false; // off-screen / outside depth range
				// viewProj is post-Y-flip (Vulkan y-down): NDC maps
				// straight onto ImGui's y-down display space.
				out.x = (ndc.x * 0.5f + 0.5f) * bdisp.x;
				out.y = (ndc.y * 0.5f + 0.5f) * bdisp.y;
				return true;
			};
			const auto drawBoxEdges = [&](const glm::vec3 q[8], ImU32 col) {
				// q: mn/mx corners (x fast, then y, then z):
				// 0..3 = z-min ring, 4..7 = z-max ring.
				static const int kEdges[12][2] = {
					{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
					{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
					{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
				};
				for (const auto& e : kEdges) {
					ImVec2 a, b;
					if (projectBox(q[e[0]], a) && projectBox(q[e[1]], b))
						bdl->AddLine(a, b, col, 1.5f);
				}
			};
			const auto boxCorners = [](const glm::vec3& mn,
				const glm::vec3& mx, glm::vec3 q[8]) {
				q[0] = { mn.x, mn.y, mn.z }; q[1] = { mx.x, mn.y, mn.z };
				q[2] = { mx.x, mx.y, mn.z }; q[3] = { mn.x, mx.y, mn.z };
				q[4] = { mn.x, mn.y, mx.z }; q[5] = { mx.x, mn.y, mx.z };
				q[6] = { mx.x, mx.y, mx.z }; q[7] = { mn.x, mx.y, mx.z };
			};
			uint32_t boxesDrawn = 0;
			if (m_giLoaded && !m_giVolume.probes.empty()) {
				glm::vec3 q[8];
				boxCorners(m_giVolume.boundsMin, m_giVolume.boundsMax, q);
				drawBoxEdges(q, IM_COL32(60, 200, 255, 220));
				++boxesDrawn;
				ImVec2 c;
				if (projectBox(
					(m_giVolume.boundsMin + m_giVolume.boundsMax) * 0.5f, c))
					bdl->AddText(ImVec2(c.x + 5.0f, c.y - 8.0f),
						IM_COL32(60, 200, 255, 220), "octree");
			}
			if (m_regLoaded) {
				for (size_t vi = 0; vi < m_regVolumes.size(); ++vi) {
					const auto& obb = m_regVolumes[vi].volume.obb;
					const glm::vec3 ex = obb.axisX * obb.halfExtents.x;
					const glm::vec3 ey = obb.axisY * obb.halfExtents.y;
					const glm::vec3 ez = obb.axisZ * obb.halfExtents.z;
					glm::vec3 q[8];
					q[0] = obb.center - ex - ey - ez;
					q[1] = obb.center + ex - ey - ez;
					q[2] = obb.center + ex + ey - ez;
					q[3] = obb.center - ex + ey - ez;
					q[4] = obb.center - ex - ey + ez;
					q[5] = obb.center + ex - ey + ez;
					q[6] = obb.center + ex + ey + ez;
					q[7] = obb.center - ex + ey + ez;
					drawBoxEdges(q, IM_COL32(70, 230, 90, 220));
					++boxesDrawn;
					ImVec2 c;
					if (projectBox(obb.center, c)) {
						char lab[32];
						snprintf(lab, sizeof(lab), "reg%u", (unsigned)vi);
						bdl->AddText(ImVec2(c.x + 5.0f, c.y - 8.0f),
							IM_COL32(70, 230, 90, 220), lab);
					}
				}
			}
			if (m_cvolLoaded) {
				for (size_t vi = 0; vi < m_cvolVolumes.size(); ++vi) {
					const auto& vol = m_cvolVolumes[vi].volume;
					const glm::vec3 mn = vol.gridMin;
					const glm::vec3 mx = vol.gridMin
						+ glm::vec3(vol.dims[0], vol.dims[1], vol.dims[2])
						* vol.spacing;
					glm::vec3 q[8];
					boxCorners(mn, mx, q);
					const float hue =
						std::fmod(float(vol.def.priority) * 0.13f, 1.0f);
					ImColor ic = ImColor::HSV(hue, 0.7f, 1.0f);
					ic.Value.w = 220.0f / 255.0f;
					drawBoxEdges(q, (ImU32)ic);
					++boxesDrawn;
					ImVec2 c;
					if (projectBox((mn + mx) * 0.5f, c)) {
						char lab[64];
						snprintf(lab, sizeof(lab), "cv%u p%u",
							(unsigned)vi, vol.def.priority);
						bdl->AddText(ImVec2(c.x + 5.0f, c.y - 8.0f),
							(ImU32)ic, lab);
					}
				}
			}
			char bcap[96];
			snprintf(bcap, sizeof(bcap), "GI volumes: %u bounds", boxesDrawn);
			bdl->AddText(ImVec2(8.0f + 1.0f, 36.0f + 1.0f),
				IM_COL32(0, 0, 0, 200), bcap);
			bdl->AddText(ImVec2(8.0f, 36.0f), IM_COL32_WHITE, bcap);
		}

		// Reflection-probe location overlay (cubemap panel toggle): one
		// marker per cached probe row — center dot + index, solid ring =
		// full-weight influence, dim ring = fade extent. Bright green =
		// camera inside influence, yellow = in fade, gray = out of range:
		// fly to the chrome ball and the covering probe lights up. Answers
		// "is there a cubemap near X?" at a glance, including embedded
		// probes (markers inside walls) the bake should have culled.
		if (m_cubemapShowProbes && !m_cubemapProbes.empty()) {
			ImDrawList* cdl = ImGui::GetForegroundDrawList();
			const ImVec2 cdisp = ImGui::GetIO().DisplaySize;
			const glm::vec3 ccamPos = m_cameraPositioner.cameraPosition_;
			const float cmaxDist = std::max(m_cubemapOverlayMaxDist, 1.0f);
			const float cmaxDist2 = cmaxDist * cmaxDist;
			uint32_t cdrawn = 0;
			for (uint32_t ci = 0; ci < m_cubemapProbes.size() && cdrawn < 64; ++ci) {
				const CubemapProbeGpu& pr = m_cubemapProbes[ci];
				const glm::vec3 ppos(pr.posInfluence.x, pr.posInfluence.y, pr.posInfluence.z);
				const float influence = pr.posInfluence.w;
				const float fade = pr.fadeCubemap.x;
				const glm::vec3 dc = ppos - ccamPos;
				if (glm::dot(dc, dc) > cmaxDist2)
					continue;
				const glm::vec4 cclip = m_unjitteredViewProj * glm::vec4(ppos, 1.0f);
				if (cclip.w <= 1e-4f)
					continue;
				const glm::vec3 cndc = glm::vec3(cclip) / cclip.w;
				if (std::abs(cndc.x) > 1.0f || std::abs(cndc.y) > 1.0f)
					continue;
				const ImVec2 cpt(
					(cndc.x * 0.5f + 0.5f) * cdisp.x,
					(cndc.y * 0.5f + 0.5f) * cdisp.y);
				const float cdist = std::sqrt(glm::dot(dc, dc));
				ImU32 col;
				if (cdist <= influence)
					col = IM_COL32(80, 255, 120, 230);
				else if (fade > 0.0f && cdist <= influence + fade)
					col = IM_COL32(255, 220, 80, 230);
				else
					col = IM_COL32(120, 140, 160, 160);
				// Influence ring: project a world-X offset of one radius.
				const glm::vec4 eclip = m_unjitteredViewProj
					* glm::vec4(ppos + glm::vec3(std::max(influence, 0.01f), 0.0f, 0.0f), 1.0f);
				if (eclip.w > 1e-4f) {
					const glm::vec3 endc = glm::vec3(eclip) / eclip.w;
					const ImVec2 ept(
						(endc.x * 0.5f + 0.5f) * cdisp.x,
						(endc.y * 0.5f + 0.5f) * cdisp.y);
					const float rPx = std::sqrt((ept.x - cpt.x) * (ept.x - cpt.x)
						+ (ept.y - cpt.y) * (ept.y - cpt.y));
					if (rPx > 2.0f && rPx < 4000.0f) {
						cdl->AddCircle(cpt, rPx, col & IM_COL32(255, 255, 255, 90), 48);
						if (fade > 0.0f) {
							const float fr = rPx * (influence + fade) / std::max(influence, 1e-3f);
							if (fr < 4000.0f)
								cdl->AddCircle(cpt, fr, col & IM_COL32(255, 255, 255, 45), 48);
						}
					}
				}
				cdl->AddCircleFilled(cpt, 3.5f, col);
				char clab[16];
				snprintf(clab, sizeof(clab), "C%u", ci);
				cdl->AddText(ImVec2(cpt.x + 6.0f, cpt.y - 7.0f), col, clab);
				++cdrawn;
			}
			char ccap[160];
			snprintf(ccap, sizeof(ccap), "Cubemap probes: %u drawn (<= %.0fm, %s)",
				cdrawn, cmaxDist,
				ProbeTileListsWanted() ? "light-tail tiling fresh" : "tiling STALE (global cubemap)");
			cdl->AddText(ImVec2(8.0f + 1.0f, 58.0f + 1.0f), IM_COL32(0, 0, 0, 200), ccap);
			cdl->AddText(ImVec2(8.0f, 58.0f), IM_COL32_WHITE, ccap);
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

		// ------------------------------------------------------------------
		// MULTIVIEW (hosted): the due secondary views' passes — per-view
		// shadows (own CSM/spot fits), Forward+ prepass + tile cull, lean
		// direct-draw Main and tonemap into each view's own image. All
		// targets are per-view; shared state rides the per-view push
		// constants. Runs after the main view's UI so the main path's
		// ordering is untouched.
		// ------------------------------------------------------------------
		RenderSecondaryViews(fb);

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
		// Flush bindless descriptor writes HERE — after every
		// GetOrCreateSampler / Ensure / CreateBindlessImage of this frame
		// (the per-frame sampler cache block and post passes sit ABOVE this
		// point), and BEFORE Execute records a single command. The old call
		// site (top of RenderFrame) ran before the sampler block, so a
		// sampler created that frame (e.g. the SMAA point sampler on first
		// use) stayed an unwritten descriptor slot for the whole frame —
		// GPU-AV: "Descriptor index 3 is uninitialized" — with the draw
		// sampling through garbage. Update-after-bind keeps these writes
		// visible to everything recorded below.
		m_context->UpdateDescriptors();
		if (IsHosted() && std::getenv("VULKANTOY_HOSTED_TRACE")) {
			std::fprintf(stderr, "[HOSTED] pre-execute frame=%llu\n",
				(unsigned long long)m_frameCounter);
			std::fflush(stderr);
		}
		m_rendergraph->Execute(fb);
		if (IsHosted() && std::getenv("VULKANTOY_HOSTED_TRACE")) {
			std::fprintf(stderr, "[HOSTED] post-execute\n");
			std::fflush(stderr);
		}
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
		// (device-idle wait); button-pressed or VULKANTOY_AUDIT=<frame>.
		if (m_auditAtFrame >= 0 && m_frameCounter == (uint64_t)m_auditAtFrame) {
			m_auditAtFrame = -1;
			m_dumpIndirectAudit = true;
		}
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
		// Opt-in SSR/volumetric stage dump (VULKANTOY_SSR_DUMP=<frame>):
		// fires at N AND N+1 (the counter is not cleared on the first hit),
		// stalls once per shot via device-idle.
		if (m_ssrDumpAtFrame >= 0
			&& (m_frameCounter == (uint64_t)m_ssrDumpAtFrame
				|| m_frameCounter == (uint64_t)m_ssrDumpAtFrame + 1)) {
			if (m_frameCounter == (uint64_t)m_ssrDumpAtFrame + 1)
				m_ssrDumpAtFrame = -1;
			WaitForDeviceIdle();
			DumpSsrStages(fb.frameIndex);
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
		// SSR temporal toggle: the temporal pass wrote history[parity]
		// during Execute (when the temporal chain ran). A spatial-only
		// record freezes the phase and invalidates (see SsrTaaEndFrame).
		smvk::SsrTaaEndFrame(m_ssrTaa, m_ssrFrameRanTemporal);
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
		// Hitch logger (see the rfT0 site): frames over the
		// VULKANTOY_HITCH_LOG budget, with the pipeline-cache delta —
		// mints>0 = shader-compile warm-up, mints=0 = traversal cost.
		// Hitch frames also dump the per-phase split (the rfT* locals
		// are all assigned by this point on the straight-line path) plus
		// texture-streaming stats and linear-allocator commit state, so
		// a hitch line names its phase without opening the Optick
		// capture.
		if (sHitchBudgetMs > 0.0) {
			const double frameMs = std::chrono::duration<double, std::milli>(
				bench_clk::now() - rfT0).count();
			if (frameMs > sHitchBudgetMs) {
				const size_t pipelines = m_rendergraph->PipelineCacheCount();
				std::fprintf(stderr,
					"[Hitch] f%llu %.1fms pipelines %llu (+%llu, total %llu)\n",
					(unsigned long long)m_frameCounter, frameMs,
					(unsigned long long)pipelines,
					(unsigned long long)(pipelines - sHitchLastPipelines),
					(unsigned long long)pipelines);
				{
					using msd = std::chrono::duration<double, std::milli>;
					const double updMs = msd(rfT1 - rfT0).count();
					const double streamMs = msd(rfT0c - rfT0b).count();
					const double preAMs = msd(rfPA - rfT1).count();
					const double preBMs = msd(rfPB - rfPA).count();
					const double preCMs = msd(rfTM - rfPB).count();
					const double postMs = msd(rfT2 - rfTM).count();
					const double readMs = msd(rfT3 - rfT2).count();
					const double uiMs = msd(rfT4 - rfT3).count();
					const double execMs = msd(rfT5 - rfT4).count();
					const double tailMs = msd(bench_clk::now() - rfT5).count();
					const glm::vec3 cp = m_cameraPositioner.cameraPosition_;
					std::fprintf(stderr,
						"[Hitch]   phases: upd %.1f (stream %.1f) passes preA %.1f preB %.1f preC %.1f post %.1f | read %.1f ui %.1f exec %.1f tail %.1f | cam %.1f %.1f %.1f\n",
						updMs, streamMs, preAMs, preBMs, preCMs, postMs,
						readMs, uiMs, execMs, tailMs, cp.x, cp.y, cp.z);
					if (m_textureStreamer) {
						const smvk::TextureStreamingStats ts = m_textureStreamer->GetStats();
						std::fprintf(stderr,
							"[Hitch]   stream: %u textures, upgrades %u downgrades %u, resident %.1f/%.1f MB\n",
							ts.textureCount, ts.upgradesLastUpdate,
							ts.downgradesLastUpdate,
							ts.residentBytes / (1024.0 * 1024.0),
							ts.fullBytes / (1024.0 * 1024.0));
					}
					std::fprintf(stderr,
						"[Hitch]   linear: frame %zu/%zu KB transient %zu/%zu KB (used/committed)\n",
						smvk::mem::FrameAllocator().Used() / 1024,
						smvk::mem::FrameAllocator().Committed() / 1024,
						smvk::mem::TransientAllocator().Used() / 1024,
						smvk::mem::TransientAllocator().Committed() / 1024);
				}
				std::fflush(stderr);
			}
			sHitchLastPipelines = m_rendergraph->PipelineCacheCount();
		}
	}
}

namespace app {

void WorldRenderer::EvsmDumpAtlas(uint32_t frameIndex) {
	// Opt-in debug (VULKANTOY_EVSM_DUMP=<frame>): per-stage stats over the
	// cascade-0 tile of the moments/temp/blurred images â€” channels are
	// (M1+ = E[exp(+c d)], M2+ = E[exp(+2c d)], M1- = E[exp(-c d)],
	// M2- = E[exp(-2c d)]) â€” plus a log-encoded M1- PNG per stage in
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
	const size_t bytes = size_t(w) * h * 16; // RGBA32F moments
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
					const size_t i = (size_t(oy + y) * w + (ox + x)) * 4;
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
				const size_t i = (size_t(y) * w + x) * 4;
				// R = M1+ (positive first moment), G = M1- (negative first
				// moment: the soft-path blocker channel).
				const float r = f[i], g = f[i + 2];
				sumR += r; sumG += g;
				minR = std::min(minR, (double)r); maxR = std::max(maxR, (double)r);
				minG = std::min(minG, (double)g); maxG = std::max(maxG, (double)g);
				if (x + 1 < tile) {
					const size_t j = (size_t(y) * w + x + 1) * 4;
					gradR += std::abs((double)f[j] - r);
					gradG += std::abs((double)f[j + 2] - g);
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
		// PNG of the cascade-0 tile (M1- log-encoded: dark = far).
		// Visual edge-band width = the prefilter width â€” definitive.
		{
			std::vector<uint8_t> rgba(size_t(tile) * tile * 4);
			for (uint32_t y = 0; y < tile; ++y) {
				for (uint32_t x = 0; x < tile; ++x) {
					const size_t i = (size_t(y) * w + x) * 4;
					const size_t o = (size_t(y) * tile + x) * 4;
					// M1- = e^(-42 d): log2 spread over [-42, 0] -> [0,255].
					const double lg = std::max(0.0, -std::log2(std::max((double)f[i + 2], 1e-30)) / 42.0);
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

// Opt-in SSR/volumetric stage dump (VULKANTOY_SSR_DUMP=<frame>): stats on
// stderr (non-finite count is the garbage detector) + tone-mapped PNGs in
// exports/ — NaN/Inf paint HOT PINK so a broken stage is unmistakable.
// Stage order is the frame's HDR chain: Main's hdr -> ssrRaw (half) ->
// ssrBlur (half) -> ssrHist0/1 (half, temporal ping-pong) ->
// ssrComposite -> volumetric -> depth (grayscale).
// The color stages are RGBA16F (8 B/px, half-floats decoded below); the
// depth stage is D32F (4 B/px). Reading them as raw float32 with a wrong
// stride produces the impossible ~1e14 stats that masked the first run.
namespace {
float SsrDumpHalfToFloat(uint16_t h)
{
	const uint32_t sign = uint32_t(h & 0x8000u) << 16;
	uint32_t exp = (h >> 10) & 0x1fu;
	uint32_t mant = h & 0x3ffu;
	uint32_t bits = 0;
	if (exp == 0) {
		if (mant == 0) {
			bits = sign; // +/-0
		}
		else {
			// Subnormal half: value = mant * 2^-24. Normalize by shifting
			// until the implicit bit lands: exp32 = 113 - d, fraction from
			// the remaining 10 bits.
			uint32_t d = 0, m = mant;
			while (!(m & 0x0400u)) { m <<= 1; ++d; }
			m &= 0x03ffu;
			bits = sign | ((113u - d) << 23) | (m << 13);
		}
	}
	else if (exp == 31) {
		bits = sign | 0x7f800000u | (mant << 13); // inf / nan
	}
	else {
		bits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
	}
	float out;
	std::memcpy(&out, &bits, sizeof(out));
 return out;
 }
}

// SSSR trace Hi-Z sizing (mirrors gfx's EnsureHiz discipline): recreate
// only on extent drift; the zero-fill reads as far/sky at every MAX block
// (an unbuilt block's nearest is behind everything — the trace skip can
// only skip a provably-empty block, never cross unknown content).
bool WorldRenderer::EnsureSsrHzb(smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
	uint32_t w, uint32_t h) {
	if (w == 0 || h == 0)
		return false;
	const uint32_t wantMips = smvk::gfx::HzbMipCount(w, h);
	if (m_ssrHzbWidth == w && m_ssrHzbHeight == h
		&& m_ssrHzbMipCount == wantMips
		&& !m_ssrHzbBuffer.empty() && !m_ssrHzbLogical.empty())
		return true;
	if (!m_ssrHzbLogical.empty())
		rg.FreeBuffer(m_ssrHzbLogical);
	m_ssrHzbLogical = {};
	m_ssrHzbBuffer = {};
	m_ssrHzbWidth = w;
	m_ssrHzbHeight = h;
	m_ssrHzbMipCount = wantMips;
	for (uint32_t m = 0; m < wantMips; ++m)
		m_ssrHzbMipOffsets[m] = smvk::gfx::HzbMipOffsetFloats(w, h, m);
	for (uint32_t m = wantMips; m < smvk::gfx::kHizMaxMips; ++m)
		m_ssrHzbMipOffsets[m] = 0;
	const uint32_t totalFloats = smvk::gfx::HzbTotalFloats(w, h);
	// Shader-side array cap (hzb.hlsli HZB_MAX_FLOATS): 4K-class targets
	// fit; beyond it the shader reads/writes OOB (diagnosed loudly).
	if (totalFloats > 12582912u)
		std::cerr << "[WorldRenderer] ssr hzb " << w << "x" << h
			<< " needs " << totalFloats << " floats (shader cap 12582912): "
			<< "the trace skip stays off for this extent\n";
	m_ssrHzbBuffer = ctx.CreateBuffer(
		(size_t)totalFloats * sizeof(float),
		smvk::BufferType_Storage, "ssr hzb max");
	if (m_ssrHzbBuffer.empty()) {
		m_ssrHzbWidth = m_ssrHzbHeight = m_ssrHzbMipCount = 0;
		return false;
	}
	{
		std::vector<uint8_t> zeros((size_t)totalFloats * sizeof(float), 0);
		if (!zeros.empty()) {
			ctx.UploadDataToBuffer(m_ssrHzbBuffer, zeros.data(), zeros.size(),
				smvk::PipelineStage(smvk::PipelineStageBit_Compute),
				smvk::AccessFlags::ShaderRead | smvk::AccessFlags::ShaderWrite);
		}
	}
	m_ssrHzbLogical = rg.AddExternalBuffer(m_ssrHzbBuffer, "ssr hzb max");
	if (m_ssrHzbLogical.empty()) {
		m_ssrHzbBuffer = {};
		m_ssrHzbWidth = m_ssrHzbHeight = m_ssrHzbMipCount = 0;
		return false;
	}
	return true;
}

void WorldRenderer::DumpSsrStages(uint32_t frameIndex) {
	const auto& ssr = m_viewportResources.ssr;
	const auto& vol = m_viewportResources.volumetric;
	const auto& hdrRes = m_viewportResources.hdr;
	const auto& depthRes = m_viewportResources.renderDepth;
	struct Stage {
		const char* name;
		const smvk::Holder<smvk::ImageHandle>* image;
		uint32_t w, h;
		bool depth;   // D32F grayscale vs RGBA16F tone-map
		bool unorm8;  // RGBA8 thin G-buffer (oct normal + roughness + metal)
	};
	const uint32_t fw = ssr.extent.width ? ssr.extent.width : 1;
	const uint32_t fh = ssr.extent.height ? ssr.extent.height : 1;
	const uint32_t hw = ssr.halfExtent.width ? ssr.halfExtent.width : 1;
	const uint32_t hh = ssr.halfExtent.height ? ssr.halfExtent.height : 1;
	const Stage stages[] = {
		{ "hdr_main",   &hdrRes.image,        fw, fh, false, false },
		{ "ssr_raw",    &ssr.raw.image,       hw, hh, false, false },
		{ "ssr_blur",   &ssr.blurred.image,   hw, hh, false, false },
		{ "ssr_gbuffer", &ssr.gbuffer.image,  fw, fh, false, true },
		{ "ssr_hist0",  &ssr.history[0].image, hw, hh, false, false },
		{ "ssr_hist1",  &ssr.history[1].image, hw, hh, false, false },
		{ "ssr_comp",   &ssr.image,           fw, fh, false, false },
		{ "volumetric", &vol.image,           fw, fh, false, false },
		{ "depth",      &depthRes.image,      fw, fh, true,  false },
	};
	for (const Stage& st : stages) {
		if (st.image->empty()) {
			std::fprintf(stderr, "[SSR-dump] %s: target invalid (skipped)\n", st.name);
			continue;
		}
		const size_t bpp = st.depth
			? (depthRes.format == smvk::Format::D32_Float_S8_UInt ? 8 : 4)
			: (st.unorm8 ? 4 : 8); // D32F+S8 vs RGBA8 vs RGBA16F
		const size_t bytes = size_t(st.w) * st.h * bpp;
		std::vector<uint8_t> px(bytes);
		const smvk::ImageLayout layout = st.depth
			? smvk::ImageLayout::DepthStencilReadOnlyOptimal
			: smvk::ImageLayout::ShaderReadOnlyOptimal;
		if (!m_context->DownloadDataFromImage(*st.image, layout, px.data(), bytes)) {
			std::fprintf(stderr, "[SSR-dump] %s: readback failed\n", st.name);
			continue;
		}
		// Decode into float32 per channel.
		const size_t chans = st.depth ? 1 : 4;
		std::vector<float> f(size_t(st.w) * st.h * chans);
		if (!st.depth && st.unorm8) {
			// Thin G-buffer (RGBA8): R,G = oct normal, B = roughness,
			// A = metal. Decoded raw (4 channels) — the PNG shows the
			// oct normal map + roughness/metal planes directly.
			const uint8_t* u8 = reinterpret_cast<const uint8_t*>(px.data());
			for (size_t i = 0; i < f.size(); ++i)
				f[i] = u8[i] / 255.0f;
		}
		else if (!st.depth) {
			const uint16_t* h16 = reinterpret_cast<const uint16_t*>(px.data());
			for (size_t i = 0; i < f.size(); ++i)
				f[i] = SsrDumpHalfToFloat(h16[i]);
		}
		else if (depthRes.format == smvk::Format::D32_Float_S8_UInt) {
			// 64-bit layout: float32 depth + stencil byte + undefined.
			const uint32_t* u32 = reinterpret_cast<const uint32_t*>(px.data());
			for (size_t i = 0; i < f.size(); ++i)
				std::memcpy(&f[i], &u32[i * 2], sizeof(float));
		}
		else if (depthRes.format == smvk::Format::D24_UNorm_S8_UInt) {
			// 32-bit packed: D24 in [31:8], stencil in [7:0].
			const uint32_t* u32 = reinterpret_cast<const uint32_t*>(px.data());
			for (size_t i = 0; i < f.size(); ++i)
				f[i] = (u32[i] >> 8) / float(0xFFFFFFu);
		}
		else {
			std::memcpy(f.data(), px.data(), bytes);
		}
		uint64_t nonfinite = 0;
		double mn = 1e300, mx = -1e300, sum = 0;
		uint64_t cnt = 0;
		for (float v : f) {
			if (!std::isfinite(v)) { ++nonfinite; continue; }
			mn = std::min(mn, (double)v);
			mx = std::max(mx, (double)v);
			sum += v;
			++cnt;
		}
		std::fprintf(stderr,
			"[SSR-dump] f%u %-10s %ux%u: nonfinite=%llu (%.4f%%) min=%.4f mean=%.4f max=%.4f\n",
			frameIndex, st.name, st.w, st.h,
			(unsigned long long)nonfinite,
			100.0 * nonfinite / (double)f.size(),
			cnt ? mn : 0.0, cnt ? sum / (double)cnt : 0.0,
			cnt ? mx : 0.0);
		// Depth readback diagnostic: raw first-pixel words to pin the
		// D32S8 / D24S8 layout (or expose a broken copy) at a glance, plus
		// the outliers' locations — clustered/strided = readback layout
		// bug, scattered = content.
		if (st.depth) {
			const uint32_t* raw = reinterpret_cast<const uint32_t*>(px.data());
			std::fprintf(stderr, "[SSR-dump]   depth raw[0..7]:");
			for (int i = 0; i < 8; ++i)
				std::fprintf(stderr, " %08x", raw[i]);
			const uint32_t* mid = raw + (size_t(st.h / 2) * st.w) * (bpp / 4);
			std::fprintf(stderr, "  mid[0..3]:");
			for (int i = 0; i < 4; ++i)
				std::fprintf(stderr, " %08x", mid[i]);
			std::fprintf(stderr, "\n");
			uint64_t outliers = 0;
			size_t firstOut = SIZE_MAX;
			for (size_t i = 0; i < f.size(); ++i) {
				const double a = std::abs((double)f[i]);
				if (a > 1e30) {
					++outliers;
					if (firstOut == SIZE_MAX) firstOut = i;
				}
			}
			std::fprintf(stderr,
				"[SSR-dump]   depth outliers(>1e30)=%llu, first at pixel %zu (x=%zu y=%zu), neighborhood:",
				(unsigned long long)outliers,
				firstOut, firstOut % st.w, firstOut / st.w);
			if (firstOut != SIZE_MAX) {
				for (size_t j = firstOut; j < std::min(f.size(), firstOut + 6); ++j)
					std::fprintf(stderr, " [%08x]", raw[j * (bpp / 4)]);
			}
			std::fprintf(stderr, "\n");
		}
		// Depth-view (SsrDepthView pass): the ssr_raw slot holds the
		// SAMPLED depth — anything outside [0,1] there is garbage the SSR
		// trace would march against. Bounding box + edge counts separate
		// a viewport/scissor artifact (banded edges) from scattered
		// memory corruption.
		if (!st.depth && std::strcmp(st.name, "ssr_raw") == 0) {
			uint64_t outCount = 0;
			uint32_t minX = UINT32_MAX, minY = UINT32_MAX, maxX = 0, maxY = 0;
			uint64_t topRows = 0, bottomRows = 0, leftCols = 0, rightCols = 0;
			const uint32_t edge = 4;
			for (uint32_t y = 0; y < st.h; ++y) {
				for (uint32_t x = 0; x < st.w; ++x) {
					const size_t fi = (size_t(y) * st.w + x) * 4;
					const float d = f[fi];
					if (!(d >= 0.0f) || !(d <= 1.0f)) { // NaN-safe range test
						++outCount;
						minX = std::min(minX, x); maxX = std::max(maxX, x);
						minY = std::min(minY, y); maxY = std::max(maxY, y);
						if (y < edge) ++topRows;
						if (y >= st.h - edge) ++bottomRows;
						if (x < edge) ++leftCols;
						if (x >= st.w - edge) ++rightCols;
					}
				}
			}
			if (outCount) {
				std::fprintf(stderr,
					"[SSR-dump]   depth-view outliers=%llu bbox=[%u..%u x %u..%u] edges: top=%llu bottom=%llu left=%llu right=%llu\n",
					(unsigned long long)outCount, minX, maxX, minY, maxY,
					(unsigned long long)topRows, (unsigned long long)bottomRows,
					(unsigned long long)leftCols, (unsigned long long)rightCols);
			}
			else {
				std::fprintf(stderr, "[SSR-dump]   depth-view CLEAN (all in [0,1])\n");
			}
			std::fflush(stderr);
		}
		std::fflush(stderr);
		// PNG: tone-map x/(1+x) for HDR, reverse-Z grayscale for depth,
		// hot pink for non-finite.
		std::vector<uint8_t> rgba(size_t(st.w) * st.h * 4);
		for (uint32_t y = 0; y < st.h; ++y) {
			for (uint32_t x = 0; x < st.w; ++x) {
				const size_t fi = (size_t(y) * st.w + x) * chans;
				const size_t o = (size_t(y) * st.w + x) * 4;
				bool bad = false;
				float c[3] = { 0, 0, 0 };
				if (st.depth) {
					const float d = f[fi];
					if (!std::isfinite(d)) bad = true;
					c[0] = c[1] = c[2] = d;
				}
				else {
					for (int k = 0; k < 3; ++k) {
						c[k] = f[fi + k];
						if (!std::isfinite(c[k])) bad = true;
					}
				}
				if (bad) {
					rgba[o] = 255; rgba[o + 1] = 0; rgba[o + 2] = 255; rgba[o + 3] = 255;
				}
				else {
					for (int k = 0; k < 3; ++k) {
						const double t = std::min(1.0, std::max(0.0, (double)c[k] / (1.0 + std::abs((double)c[k]))));
						rgba[o + k] = (uint8_t)(t * 255.0);
					}
					rgba[o + 3] = 255;
				}
			}
		}
		char path[128];
		std::snprintf(path, sizeof(path), "exports/ssrdump_%s_f%u.png", st.name, frameIndex);
		if (LutWritePng(path, (int)st.w, (int)st.h, rgba.data()))
			std::fprintf(stderr, "[SSR-dump] wrote %s\n", path);
	}
}

void WorldRenderer::UpdateCamera(float dt)
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

void WorldRenderer::SetUIMode(bool ui)
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

void WorldRenderer::SetTextureFiltering(TextureFiltering mode)
{
	if (m_textureFiltering == mode)
		return;
	m_textureFiltering = mode;
	ApplyTextureFiltering();
}

void WorldRenderer::ApplyTextureFiltering()
{
	if (!m_context)
		return;
	// Bilinear = linear within a mip, nearest between mips.
	// Trilinear = linear within + linear between (default).
	const smvk::MipmapMode mipMode = (m_textureFiltering == TextureFiltering::Bilinear)
		? smvk::MipmapMode::Nearest
		: smvk::MipmapMode::Linear;
	// Slot 0: default Repeat sampler (material albedo/normal/metalness/...,
	// shaders hardcode samplers[0]). Slot 2: clamp sampler (lightmap
	// atlases, 3D probe volumes via samplers[2], cubemaps via dedup).
	// Both are recreated in place so the bindless indices stay stable.
	if (m_context->GetSamplerCount() > 0) {
		smvk::SamplerCreateInfo repeatCi{};
		repeatCi.mipmapMode = mipMode;
		m_context->RecreateSampler(0, repeatCi);
	}
	if (m_context->GetSamplerCount() > 2) {
		smvk::SamplerCreateInfo clampCi{};
		clampCi.addressU = smvk::AddressMode::ClampToEdge;
		clampCi.addressV = smvk::AddressMode::ClampToEdge;
		clampCi.addressW = smvk::AddressMode::ClampToEdge;
		clampCi.mipmapMode = mipMode;
		m_context->RecreateSampler(2, clampCi);
	}
}

} // namespace app

// NOTE: entry point moved to main.cpp (scene selection + Configure).
