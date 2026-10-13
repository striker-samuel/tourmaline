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
#include <stdexcept> // std::runtime_error throws (sidecar loads)
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
#include <Engine/debug_log.h> // SMVK_TRACE_LOG ([exposure] dashboards)
#include <Engine/Asset/bake_asset.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <gltf_import/gltf.h>
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
#include <Engine/Rendergraph/ssgi.h>
#include <Engine/Rendergraph/tonemap.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/ui.h>
#include <Engine/Renderer/spirv_reflection.h> // PackPushFloat3x4 (prevModel blob)
#include <Engine/Renderer/GfxRenderer.h>
#include <Engine/Rendergraph/ForwardPlusRenderer.h>
#include <Engine/Utils/LutIo.h>
#include <Engine/Rendergraph/FrameContext.h>
#include <Engine/Renderer/Renderer.h>

#include <Engine/Vfs/vfs.h>
#include <Engine/Utils/texture_loader.h>
#include <Engine/Renderer/WorldRenderer.h>
#include <Engine/Profile/profile.h>
#include <imgui.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


// Constants (clips/FOV live on WorldRenderer as members; the far clip is
// per-scene - m_sceneFarClip, fitted to the framed world bounds at load).
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
// Pre-exposure HDR reference (at TONEMAP_EXPOSURE = 0.001): 400 -> the
// prefilter passes content above ~0.4 POST-exposure - the bright-but-
// not-clipped zone of the GT7 curve (sky/windows/sun-lit walls, with
// the knee rolling in from ~0.15). The old 1000 (1.0 post) sat above
// GT7's shoulder start, so ONLY sun/sky ever bloomed.
static constexpr float BLOOM_THRESHOLD = 400.0f;
static constexpr float BLOOM_KNEE = 100.0f;        // keeps soft ratio knee/threshold = 0.25
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
	// Headless SPD override (VULKANTOY_SPD=0 forces the raster 13-tap
	// downsample for A/B captures; =1 forces the single-pass compute path).
	if (const char* spdEnv = std::getenv("VULKANTOY_SPD")) {
		d.useSpd = std::atoi(spdEnv) != 0;
	}
	return d;
}

// -----------------------------------------------------------------------------
// Tonemap config (program + pass live in Engine/Rendergraph/tonemap.h)
// -----------------------------------------------------------------------------
static constexpr float TONEMAP_EXPOSURE = 0.001f; // sun 5000 lux * 0.001 ~ HDR 5 pre-tonemap
static constexpr uint32_t TONEMAP_MODE = 3; // 0=Reinhard, 1=ACES, 2=Uncharted2, 3=GT7 (== m_tonemapMode default)

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
	d.useCvolGi = 1;
	d.jitterStrength = 1.0f;
	d.heightFalloff = 0.0f;
	d.heightBase = 0.0f;
	d.name = "volumetric";
	return d;
}

// -----------------------------------------------------------------------------
// SSGI AO config (the post-Main AO chain, see Engine/Rendergraph/ssgi.h).
// Runs AFTER Main: Main exports the AO-sensitive lighting through an
// aoTerm MRT target, the compose subtracts aoTerm*(1-aoEff) from the HDR
// (joint-bilateral upsample over the self-carried (ao, repDist, repNormal)
// triple - see ao.hlsli). GTAO is deprecated and removed; SSAO/SAO share
// the chain, only the raw pass differs.
// -----------------------------------------------------------------------------
static constexpr float SSAO_RADIUS = 0.5f; // view-space sample radius (m)
static constexpr float SSAO_BIAS = 0.025f; // self-occlusion bias
static constexpr float SSAO_INTENSITY = 1.0f; // compose-side aoEff lerp
static constexpr float SSAO_MAX_DISTANCE = 25.0f; // fade-out distance (m) + the .g encode range
static constexpr uint32_t SSAO_SAMPLES = 16; // SSAO hemisphere taps 1..32
static constexpr float SSAO_BLUR_SHARPNESS = 8.0f; // depth edge weight
static constexpr uint32_t SAO_SAMPLES = 11; // SAO spiral taps 1..32
static constexpr uint32_t SAO_SPIRAL_TURNS = 7; // extra spiral twist 1..16

static smvk::SsgiDesc MakeSsgiDesc() {
	smvk::SsgiDesc d{};
	d.aoFormat = smvk::Format::RGBA16_UNorm; // (ao, encRepDist, octN)
	d.hdrFormat = smvk::Format::RGBA16_Float; // matches the scene HDR
	d.radius = SSAO_RADIUS;
	d.bias = SSAO_BIAS;
	d.intensity = SSAO_INTENSITY;
	d.maxDistance = SSAO_MAX_DISTANCE;
	d.sampleCount = SSAO_SAMPLES;
	d.spiralTurns = SAO_SPIRAL_TURNS;
	d.blurSharpness = SSAO_BLUR_SHARPNESS;
	d.name = "ssgi";
	d.aoRawName = "ssgiAoRaw";
	d.aoBlurName = "ssgiAoBlur";
	d.aoTermName = "ssgiAoTerm";
	d.aoTermMsaaName = "ssgiAoTermMsaa";
	d.composedName = "ssgiAoHdr";
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
// at most m_spotShadowMax casters (upper limit + distance/screenspace
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
// arrives through the host's loader (SetWorldSource) - nothing about the
// scene is known here. The load-time framing (onAttach) re-poses the
// camera on the world bounds unless VULKANTOY_CAM_* pins it for benches.
// ---------------------------------------------------------------------------
namespace engine {

	smvk::ShaderProgram& WorldRenderer::MainLitProgramFor(uint32_t materialId) {
		const GIVariant giVar = GetActiveGIVariant();
		const uint32_t gv = static_cast<uint32_t>(giVar);
		// Layered-ness for the std-twin pick (mirrors the GfxRenderer
		// resolver: unknown material = layered = monolith, fail-safe).
		const bool layered = [&]() {
			if (!m_world)
				return true;
			const smvk::Material* mat = m_world->GetMaterial(materialId);
			return mat ? (mat->values.layerMaterials.w > 1) : true;
		}();
		const bool stdReady = !layered
			&& m_resources.mainProgramStd.shaderCount == 2;
		if (giVar != GIVariant::None) {
			smvk::ShaderProgram& gi = m_resources.mainProgramGi[gv];
			if (gi.shaderCount == 2) {
				if (!layered && m_resources.mainProgramGiStd[gv].shaderCount == 2)
					return m_resources.mainProgramGiStd[gv];
				return gi;
			}
			return stdReady ? m_resources.mainProgramStd : m_resources.mainProgram;
		}
		return stdReady ? m_resources.mainProgramStd : m_resources.mainProgram;
	}

	void WorldRenderer::Configure(WorldRendererConfig passes) {
		// Compat path for hosts that have not migrated to parse-once yet
		// (editor): parse locally. Prefer Configure(passes, run).
		Configure(passes, RunConfig::Parse());
	}
	void WorldRenderer::Configure(WorldRendererConfig passes, const RunConfig& run) {
		m_passes = passes;
		// Snapshot the headless pins: ApplyHeadless*Overrides re-apply
		// this snapshot after every authored scene seed (Initialize +
		// every load/rebind). The snapshot never re-reads the environment,
		// so pins are stable for the whole run.
		m_headlessPins = run;
		ApplyRunConfig(run);
	}
	void WorldRenderer::ApplyRunConfig(const RunConfig& run) {
		// Debug: force the AA mode for headless A/B benches (the UI combo
		// keeps working after this). The pin is validated once in
		// RunConfig::Parse; ApplyHeadlessPostfxOverrides re-applies the
		// same snapshot after every scene seed.
		if (run.aa.has_value()) {
			switch (*run.aa) {
			case AaPin::Off: m_aaMode = AaMode::Off; break;
			case AaPin::Msaa2x: m_aaMode = AaMode::Msaa2x; break;
			case AaPin::Smaa1x: m_aaMode = AaMode::Smaa1x; break;
			case AaPin::SmaaT2x: m_aaMode = AaMode::SmaaT2x; break;
			}
		}
		// Opt-in A2C for masked cutout in MSAA mode (quality trade-off, see
		// m_msaaAlphaToCoverage).
		m_msaaAlphaToCoverage = run.a2c;
		// Texture filtering default for headless A/B
		// (trilinear | 2x | 4x | 8x | 16x). The ImGui combo stays live after;
		// applied every RenderFrame.
		if (run.textureFilter.has_value()) {
			TextureFiltering parsed{};
			if (TextureFilteringFromString(run.textureFilter->c_str(), parsed))
				m_textureFiltering = parsed;
			else
				std::cerr << "[WorldRenderer] bad VULKANTOY_TEXTURE_FILTER (want "
				"trilinear|2x|4x|8x|16x): " << *run.textureFilter << "\n";
		}
		// Headless terrain override (same discipline as VULKANTOY_VOLUMETRIC
		// / VULKANTOY_SSR): VULKANTOY_TERRAIN=0/1 pins the terrain pass
		// for headless A/B runs (the UI checkbox / combo stays live).
		if (run.terrainPinned) {
			m_passes.terrain = run.terrain;
		}
		// Point the FPS camera at a generic default viewpoint (Camera keeps a
		// stable pointer to m_cameraPositioner, so lookAt is assignment-safe).
		// Initialize re-poses this on the loaded world's bounds (generic
		// framing) unless the env vars below pin it for benches.
		const glm::vec3 camPos(4.0f, 3.0f, 6.0f);
		const glm::vec3 camTarget(0.0f, 1.0f, 0.0f);
		m_cameraPositioner.lookAt(camPos, camTarget, glm::vec3(0.0f, 1.0f, 0.0f));
		// Debug: rotate the default view around world up (degrees) - A/B runs
		// without mouse input.
		glm::vec3 dir = glm::normalize(camTarget - camPos);
		bool dirOverridden = false;
		if (run.camFwd.has_value()) {
			// Absolute forward override ("x,y,z"): wins over the yaw env so a
			// fully specified diagnostic camera reproduces exactly.
			glm::vec3 fwd(0.0f);
			const std::string& fwdEnv = *run.camFwd;
			if (std::sscanf(fwdEnv.c_str(), "%f,%f,%f", &fwd.x, &fwd.y, &fwd.z) == 3
				&& glm::length(fwd) > 1e-4f) {
				dir = glm::normalize(fwd);
				dirOverridden = true;
			}
			else {
				std::cerr << "[WorldRenderer] bad VULKANTOY_CAM_FWD (want x,y,z): "
					<< fwdEnv << "\n";
			}
		}
		if (!dirOverridden) {
			if (run.camYawDeg.has_value()) {
				const float rad = glm::radians(*run.camYawDeg);
				dir = glm::vec3(
					dir.x * std::cos(rad) + dir.z * std::sin(rad), dir.y,
					-dir.x * std::sin(rad) + dir.z * std::cos(rad));
			}
		}
		// Debug: override the spawn position ("x,y,z"); composes with the
		// yaw env (the view keeps looking along the yaw-rotated default
		// direction) or with the absolute VULKANTOY_CAM_FWD.
		if (run.camPos.has_value()) {
			glm::vec3 pos(camPos);
			const std::string& posEnv = *run.camPos;
			if (std::sscanf(posEnv.c_str(), "%f,%f,%f", &pos.x, &pos.y, &pos.z) == 3) {
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

} // namespace engine (scene registry + helpers above; methods open their own block below)


// Class declaration lives in App/world_renderer.h; out-of-line method
// definitions follow.
namespace engine {

	// HOST (the App this element rides on - see the header): cheap
	// construction, everything resolves at onAttach (Run time, after the
	// host's Initialize). The dtor only releases what onDetach did not
	// (onDetach runs at host Shutdown while every member is alive).
	WorldRenderer::WorldRenderer(smvk::App& host)
		: m_host(host) {
	}
	WorldRenderer::~WorldRenderer() = default;

	// Headless volumetric pins (see the header): the Configure snapshot wins
	// over the authored scene seed (the AGENTS.md "scene fog settings lose"
	// contract). Called at startup Initialize AND after the per-scene
	// PushVolumetricEnv seed in BuildCookedLightsFromWorld (scene
	// load/rebind would otherwise stomp a headless force-on).
	void WorldRenderer::ApplyHeadlessVolumetricOverrides() {
		// Validated snapshot (see RunConfig + m_headlessPins): invalid
		// values warned once at startup, authored scene wins those.
		const RunConfig& pins = m_headlessPins;
		if (pins.volumetric.has_value())
			m_volumetricEnabled = (*pins.volumetric == OnOffPin::On);
		// March-step override for headless A/B runs (4..128; heavy fog
		// configs without UI interaction - the quality preset combos
		// stay live after).
		// March-step pin (heavy fog configs without UI interaction - the
		// quality preset combos stay live after).
		if (pins.volumetricSteps.has_value())
			m_volumetricSteps = *pins.volumetricSteps;
		// Extinction-density override for headless A/B runs (0..2): heavy
		// densities make the march output PURE in-scatter (throughput
		// decays to 0, the composited base vanishes), which isolates the
		// scatter sources (sun / GI ambient / forward lights) from the
		// lit-surface base for pixel-diff verification.
		// Extinction-density pin (heavy densities isolate the scatter
		// sources from the lit-surface base for pixel-diff verification).
		if (pins.volumetricDensity.has_value())
			m_volumetricDensity = *pins.volumetricDensity;
		// Fog-GI quality bypass pin (pairs with the volumetric pin for
		// pure-octree fog cost measurements).
		if (pins.volumetricCvol.has_value())
			m_volumetricUseCvolGi = (*pins.volumetricCvol == OnOffPin::On);
	}

	// Headless postfx pins (see the header): the Configure snapshot wins over
	// the authored scene seed (the AGENTS.md "scene settings lose" contract).
	// Called at startup Initialize AND after the per-scene PushPostfxEnv seed
	// in BuildCookedLightsFromWorld (scene load/rebind would otherwise stomp
	// a headless pin). Invalid values warned once at startup (RunConfig::Parse);
	// the UI panel/keys stay live afterwards.
	void WorldRenderer::ApplyHeadlessPostfxOverrides() {
		const RunConfig& pins = m_headlessPins;
		// AA mode pin for A/B runs without UI interaction. The ImGui combo
		// stays live after.
		if (pins.aa.has_value()) {
			switch (*pins.aa) {
			case AaPin::Off: m_aaMode = AaMode::Off; break;
			case AaPin::Msaa2x: m_aaMode = AaMode::Msaa2x; break;
			case AaPin::Smaa1x: m_aaMode = AaMode::Smaa1x; break;
			case AaPin::SmaaT2x: m_aaMode = AaMode::SmaaT2x; break;
			}
		}
		// SSR pin for headless A/B + capture runs (the ImGui checkbox
		// stays live after). SSR (SSGI reflections) defaults ON.
		if (pins.ssr.has_value())
			m_ssrEnabled = (*pins.ssr == OnOffPin::On);
		// Bloom tuning pins (threshold is pre-exposure HDR, same units as
		// the ImGui sliders; intensity is the tonemap add scale). The
		// ImGui sliders stay live after.
		if (pins.bloomThreshold.has_value())
			m_bloomThreshold = *pins.bloomThreshold;
		if (pins.bloomIntensity.has_value())
			m_bloomIntensity = *pins.bloomIntensity;
		// AO mode override for headless A/B runs (off | ssao | sao) - the
		// O key is interactive-only, so captures/benches pin the mode
		// through here. The ImGui combo + O key stay live after.
		// AO mode pin (the O key is interactive-only, so captures/benches
		// pin the mode through here). The ImGui combo + O key stay live.
		if (pins.ao.has_value()) {
			switch (*pins.ao) {
			case AoPin::None: m_aoMode = AoMode::None; break;
			case AoPin::Ssao: m_aoMode = AoMode::Ssao; break;
			case AoPin::Sao: m_aoMode = AoMode::Sao; break;
			}
		}
		// Deterministic A/B helper: manual exposure removes the eye
		// adaptation feedback path so pixel diffs isolate the change.
		if (pins.autoExposure.has_value())
			m_autoExposureEnabled = *pins.autoExposure;
		// Baked-cubemap kill pin (off falls back to the analytic sky,
		// never to black).
		if (pins.cubemap.has_value() && !*pins.cubemap)
			m_cubemapEnabled = false;
	}

	bool WorldRenderer::onAttach() {
		// The host booted before attaching (App::Run: Initialize then
		// onAttach): resolve the borrowed graphics members off it. The
		// rendergraph + world below are the ELEMENT's own.
		m_context = m_host.GetResourceManager();
		m_swapchain = m_host.GetSwapchain();
		if (!m_context || !m_swapchain)
			return false;
		m_rendergraph = smvk::mem::MakeProcess<smvk::RenderGraph>(*m_context);

		// Static geo heaps FIRST (3 x 512 MB device buffers + VMA virtual
		// blocks): everything downstream (pak world source upload below,
		// shader/pipeline setup) can overlap their use, and the pak loader's
		// heap writes must not wait behind shader loading. Idempotent, so
		// a second caller stays safe.
		smvk::AllocateStaticVertexFmt(*m_context);

		// DLSS capability is fixed per run (adapter + driver + Streamline
		// runtime): the device exists now, so ask once. Failure leaves the
		// mode combo cosmetic - the status readout explains why.
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
		// Kill the occluder fill + HZB cull (VULKANTOY_HIZ=0; the hizDepth
		// IMAGE + its claim stay a fixed cost - see the fc.hizDepthView note).
		if (const char* hizEnv = std::getenv("VULKANTOY_HIZ")) {
			if (std::atoi(hizEnv) == 0)
				m_hizOccluderDepth = false;
		}
		// Headless LOD kill (VULKANTOY_LOD=0 forces LOD0).
		if (const char* lodEnv = std::getenv("VULKANTOY_LOD")) {
			if (std::atoi(lodEnv) == 0)
				m_lodEnabled = false;
		}
		// Headless pixel-budget pin (VULKANTOY_LODBUDGET=<px>): forces
		// far-level selection for geometry/shader-LOD A/B runs without
		// the UI slider (a huge budget pushes everything to its coarsest
		// sibling). Clamped to a sane range; the UI slider stays live.
		if (const char* lodBudgetEnv = std::getenv("VULKANTOY_LODBUDGET")) {
			const float b = float(std::atof(trimEnv(lodBudgetEnv).data()));
			if (b >= 0.05f && b <= 256.0f)
				m_lodPixelBudget = b;
		}
		// Headless LOD cadence kill (VULKANTOY_LODFULL=0 restores the old
		// halfrate refresh: parity-split, 1-frame-stale decisions).
		if (const char* lodFullEnv = std::getenv("VULKANTOY_LODFULL")) {
			if (std::atoi(lodFullEnv) == 0)
				m_lodFullRate = false;
		}
		// Headless shader-LOD pin (VULKANTOY_SHADERLOD=0 keeps the
		// farthest level full-lit, =1 forces the subsystem on) for A/B
		// shading-cost runs without the UI checkbox.
		if (const char* lodShEnv = std::getenv("VULKANTOY_SHADERLOD")) {
			m_lodShaderEnabled = std::atoi(lodShEnv) != 0;
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
		// Headless culling-freeze override (same checkbox semantics: batch
		// expansion + the filter CS hold the frozen (view, eye) while the
		// camera keeps moving - pairs with --bench_move to reproduce/verify
		// frozen-cull rendering headlessly).
		if (const char* fzEnv = std::getenv("VULKANTOY_FREEZE_CULL")) {
			m_freezeCullingView = std::atoi(fzEnv) != 0;
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
			if (dv == "lit") m_debugView = ViewMode::Lit;
			else if (dv == "unlit") m_debugView = ViewMode::Unlit;
			else if (dv == "meshlets") m_debugView = ViewMode::Meshlets;
			else if (dv == "cascades") m_debugView = ViewMode::Cascades;
			else if (dv == "lods") m_debugView = ViewMode::Lods;
			else if (dv == "vertexcolors" || dv == "vcolors") m_debugView = ViewMode::VertexColors;
			else if (dv == "uv0" || dv == "uv1_base") m_debugView = ViewMode::Uv0;
			else if (dv == "uv1" || dv == "uv2" || dv == "uv1_lightmap") m_debugView = ViewMode::Uv1;
			else if (dv == "pom" || dv == "parallax" || dv == "parallaxocclusion") m_debugView = ViewMode::Pom;
			else if (dv == "ssr" || dv == "ssr_contribution") m_debugView = ViewMode::SsrContribution;
			else if (dv == "ssrmask" || dv == "ssr_mask" || dv == "ssr-mask") m_debugView = ViewMode::SsrMask;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_DEBUGVIEW value: " << dv << "\n";
		}
		// Postfx headless pins (AA/SSR/bloom/AO/exposure/cubemap) live in
		// ApplyHeadlessPostfxOverrides (re-applied after the scene seed).
		// Texture filtering override for A/B runs without UI interaction
		// (trilinear | 2x | 4x | 8x | 16x). The ImGui combo stays live after.
		if (const char* filtEnv = std::getenv("VULKANTOY_TEXTURE_FILTER")) {
			const std::string_view f = trimEnv(filtEnv);
			TextureFiltering parsed{};
			if (TextureFilteringFromString(f, parsed))
				m_textureFiltering = parsed;
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
		// Volumetric fog toggle for headless A/B + capture runs (the
		// ImGui checkbox stays live after); volumetric defaults off.
		// (SSR/bloom/AO/exposure/cubemap pins: ApplyHeadlessPostfxOverrides.)
		ApplyHeadlessVolumetricOverrides();
		ApplyHeadlessPostfxOverrides();
		// Mesh-shader main leg (VULKANTOY_MESH=0 forces the indexed
		// multi-draws for A/B captures; =1 forces the mesh dispatch when
		// the device supports it). The UI checkbox stays live.
		if (const char* meshEnv = std::getenv("VULKANTOY_MESH")) {
			m_useGfxMesh = std::atoi(meshEnv) != 0;
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
		// Forward+ decals (tiled projected splats) VISIBILITY toggle
		// (1 | 0): the world carries the entities (app presets, J
		// spawns); this only hides/shows them. The app host additionally
		// skips creating its preset entities when 0.
		if (const char* decEnv = std::getenv("VULKANTOY_DECALS")) {
			const std::string_view dec = trimEnv(decEnv);
			if (dec == "1" || dec == "on") m_decalsEnabled = true;
			else if (dec == "0" || dec == "off") m_decalsEnabled = false;
			else std::cerr << "[WorldRenderer] unknown VULKANTOY_DECALS value: " << dec << "\n";
		}
		// Headless crosshair splat at a fixed frame (see m_decalAutoSplatAtFrame):
		// fires the J path once so capture runs verify the pick BVH.
		if (const char* autoEnv = std::getenv("VULKANTOY_DECAL_AUTOSPLAT")) {
			m_decalAutoSplatAtFrame = std::atoi(autoEnv);
		}
		// EVSM opt-in (default OFF; VULKANTOY_EVSM=1 forces on; =0 forces
		// the PCF depth atlases off). Main-view feature - the secondary
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
		// SDSM (sample-distribution cascade fitting) override for A/B
		// captures (VULKANTOY_SDSM=0 forces the fixed split range).
		if (const char* sdsmEnv = std::getenv("VULKANTOY_SDSM")) {
			if (std::atoi(sdsmEnv) == 0)
				m_sdsmEnabled = false;
			else
				m_sdsmEnabled = true;
		}
		// Opt-in EVSM atlas dump (VULKANTOY_EVSM_DUMP=<frame>): see header.
		if (const char* dumpEnv = std::getenv("VULKANTOY_EVSM_DUMP")) {
			m_evsmDumpAtFrame = std::atoi(dumpEnv);
		}
		// Opt-in SSR/volumetric stage dump (VULKANTOY_SSR_DUMP=<frame>): see
		// header - fires at N and N+1 (one-frame-lag discrimination).
		if (const char* ssrDumpEnv = std::getenv("VULKANTOY_SSR_DUMP")) {
			m_ssrDumpAtFrame = std::atoi(ssrDumpEnv);
		}

		if (const char* iblEnv = std::getenv("VULKANTOY_IBL")) {
			// Mute the analytic sky ambient (any ambient mode -> off;
			// All carries sky + baked, so muting kills all ambient).
			if (std::atoi(iblEnv) == 0 && m_ambientMode != AmbientMode::Off)
				m_ambientMode = AmbientMode::Off;
		}
		// Baked-cubemap kill switch for headless runs (same state as the
		// panel's "Cubemap specular IBL" checkbox; off falls back to the
		// analytic sky, never to black): VULKANTOY_CUBEMAP=0 disables,
		// 1 (or unset) keeps the baked specular path. (Lives in
		// ApplyHeadlessPostfxOverrides - the scene postfx seed must not
		// stomp the pin.)
		// Cubemap debug view for headless runs (same modes as the panel
		// combo; see the cubemapDebug field): 0 off, 1 raw cubemap
		// (no Fresnel/roughness - shows which probe serves each pixel),
		// 2 probe blend weight as heat.
		if (const char* cubeDbgEnv = std::getenv("VULKANTOY_CUBEMAP_DEBUG")) {
			const int mode = std::atoi(cubeDbgEnv);
			if (mode >= 0 && mode <= 2)
				m_cubemapDebug = mode;
			else
				std::cerr << "[WorldRenderer] VULKANTOY_CUBEMAP_DEBUG wants 0..2\n";
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
		// GI debug view for headless runs (same modes the R key cycles;
		// F8 is the legacy alias): 0 off, 1 baked irradiance,
		// 2 source palette, 3 isolated level (the level byte selects
		// it - VULKANTOY_GI_DEBUG_LEVEL), 4 dominant level.
		if (const char* giDbgEnv = std::getenv("VULKANTOY_GI_DEBUG")) {
			const int mode = std::atoi(giDbgEnv);
			if (mode >= 0 && mode <= 4)
				m_giDebug = mode;
			else
				std::cerr << "[WorldRenderer] VULKANTOY_GI_DEBUG wants 0..4\n";
		}
		// Isolated-level selector for GI debug mode 3 (headless twin of
		// the panel slider).
		if (const char* giLvlEnv = std::getenv("VULKANTOY_GI_DEBUG_LEVEL")) {
			const int lvl = std::atoi(giLvlEnv);
			if (lvl >= 0 && lvl <= 7)
				m_giDebugLevel = lvl;
			else
				std::cerr << "[WorldRenderer] VULKANTOY_GI_DEBUG_LEVEL wants 0..7\n";
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
		// the overlay combo can change it later (the HOST applies it at the
		// next frame's rebuild check - App::SetPresentMode / DrawFrame pump).
		m_swapchain->SetPresentMode(m_host.GetPresentMode());
		{
			const auto vkExt = m_swapchain->GetExtent();
			UpdateCameraAspect(ScaledExtent(
				smvk::Extent2D{ vkExt.width, vkExt.height }, m_renderScale));
		}
		// MULTIVIEW slab sizing: the five per-view buffers below get a
		// VIEW-MAJOR stride - (view * framesInFlight + frame) * element -
		// Per-frame slices (the main view only): every per-frame ring
		// below keys on (frame, view 0).
		const uint32_t viewSlabs = 1u;
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
		// frame; re-derived from the world's decal entities every frame,
		// see UpdateDecals).
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
		// DS visualizer params ring (one DsVizParams per flight + one zero
		// slice shared by secondary views, which never viz). Sized by the
		// compile-time max (never stale across swapchain recreates, same as
		// the deferredshadow.h rings). Zero-filled once here; per-frame
		// uploads only ever touch slice[flight], never the zero slice.
		m_resources.dsVizParamsBuffer = m_context->CreateBuffer(
			sizeof(DsVizParams) * (size_t(::MAX_FRAMES_IN_FLIGHT) + 1u),
			smvk::BufferType_Storage,
			"ds viz params"
		);
		if (!m_resources.dsVizParamsBuffer.empty()) {
			const std::vector<uint8_t> vizZeros(
				sizeof(DsVizParams) * (size_t(::MAX_FRAMES_IN_FLIGHT) + 1u), 0);
			m_context->UploadDataToBuffer(m_resources.dsVizParamsBuffer,
				vizZeros.data(), vizZeros.size(),
				smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
				smvk::AccessFlags::ShaderRead, 0, 0);
		}
		// RenderView slab: per-frame virtual cameras (main + shadow views).
		if (!m_renderViews.Ensure(*m_context, m_swapchain->framesInFlight())) {
			std::cerr << "[WorldRenderer] Failed to create render views buffer\n";
			return false;
		}
		// GPU particle demo emitters (world-space defaults near the scene
		// origin; the panel below retouches them live). HOST OPT-IN: the
		// APP host seeds its demo crowd (opt-in); default
		// clean (SetDemoContentEnabled).
		if (m_demoContent)
			SeedParticleEmitters();

		// VFS overlay: loose content/ for iteration, cooked paks mounted
		// underneath (loose files win on collisions). Shader reads below
		// go through the same overlay so packaged builds just work.
		// Engine-owned baked resources staged next to the app by its
		// CMake step sit at the lowest priority so app content/paks
		// always win.
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
			.shaders = { vert, frag },
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
		for (uint32_t li = 0; li < engine::GfxRenderer::LitSgVariantCount(); ++li) {
			try {
				const std::string path = std::string("content/shaders/default_lit.frag.hlsl.sg")
					+ std::to_string(engine::GfxRenderer::LitSgVariantLobes(li)) + ".spv";
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
		// Standard-material lit twins (default_lit .std/.giNstd - MAT_STANDARD:
		// the layered machinery compiled out; bound for layerMaterials.w <= 1).
		// Non-fatal: missing modules fall back to the monolith picks.
		try {
			m_resources.fragStdModule = m_context->CreateShaderModule(
				readFile("content/shaders/default_lit.frag.hlsl.std.spv"), "fragLitStd");
			m_resources.mainProgramStd = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragStdModule,
						.entryPoint = "mainPS",
						.stage = smvk::ShaderStage::Fragment
					}
				},
				.shaderCount = 2
			};
		}
		catch (...) {
			std::cerr << "[WorldRenderer] std-material lit variant unavailable"
				" - non-layered materials keep the monolith\n";
		}
		for (uint32_t gi = 0; gi < 4; ++gi) {
			try {
				const std::string path = std::string("content/shaders/default_lit.frag.hlsl.gi")
					+ std::to_string(gi) + "std.spv";
				m_resources.fragLitGiStdModules[gi] =
					m_context->CreateShaderModule(readFile(path), "fragLitGiStd");
				m_resources.mainProgramGiStd[gi] = {
					.shaders = {
						smvk::Shader{
							.module = m_resources.vertShader,
							.entryPoint = "mainVS",
							.stage = smvk::ShaderStage::Vertex
						},
						smvk::Shader{
							.module = m_resources.fragLitGiStdModules[gi],
							.entryPoint = "mainPS",
							.stage = smvk::ShaderStage::Fragment
						}
					},
					.shaderCount = 2
				};
			}
			catch (...) {
				std::cerr << "[WorldRenderer] std-material GI variant " << gi
					<< " unavailable - non-layered materials keep the monolith\n";
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
		try {
			auto ssrmaskCode = readFile("content/shaders/default_ssrmask_debug.frag.hlsl.spv");
			m_resources.fragSsrMaskDebugShader = m_context->CreateShaderModule(ssrmaskCode, "fragSsrMaskDebug");
		}
		catch (...) {
			std::cerr << "[WorldRenderer] default_ssrmask_debug unavailable - SSR mask view falls back to Lit\n";
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
		// SSR receiver-mask debug view (env/UI only): per-material
		// classification mirroring the thin G-buffer writer + the trace
		// matte gate (default_ssrmask_debug.frag.hlsl). Same VS as main.
		// Failure is non-fatal (program stays at shaderCount 0 and the
		// view falls back to Lit at record time, like unlit/etc.).
		if (!m_resources.fragSsrMaskDebugShader.empty()) {
			m_resources.ssrMaskDebugProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.vertShader,
						.entryPoint = "mainVS",
						.stage = smvk::ShaderStage::Vertex
					},
					smvk::Shader{
						.module = m_resources.fragSsrMaskDebugShader,
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

		// Shader-LOD program (farthest level, see World::
		// UseVertexLitShaderAt): vertex-lit VS + pass-through FS. Failure
		// is non-fatal - the subsystem falls back to full-lit everywhere
		// (ShaderLodActive).
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
		// with any of them). Failure is non-fatal - wireframe keeps the
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

		// Masked shadow-depth program (leaf-shaped shadows for cutout).
		auto shadowVertCode = readFile("content/shaders/shadow.vert.hlsl.spv");
		auto shadowFragCode = readFile("content/shaders/shadow.frag.hlsl.spv");
		m_resources.shadowMaskedVertShader = m_context->CreateShaderModule(shadowVertCode, "shadowMaskedVert");
		m_resources.shadowMaskedFragShader = m_context->CreateShaderModule(shadowFragCode, "shadowMaskedFrag");
		// SDSM stats (sdsm_stats.cs.hlsl): decimated depth min/max per frame;
	// per-flight ping-pong buffers so the async readback copy of one
	// flight's stats never races the other's rewrite (the exposure
	// histogram's ring guarantee - see UpdateReadbacks).
		try {
			auto sdsmStatsCode = readFile("content/shaders/sdsm_stats.cs.hlsl.spv");
			m_resources.sdsmStatsModule = m_context->CreateShaderModule(sdsmStatsCode, "sdsmStatsCS");
			m_resources.sdsmStatsProgram = {
				.shaders = {
					smvk::Shader{
						.module = m_resources.sdsmStatsModule,
						.entryPoint = "mainCS",
						.stage = smvk::ShaderStage::Compute
					}
				},
				.shaderCount = 1
			};
			for (uint32_t f = 0; f < 2; ++f)
				m_sdsmStatsBuffers[f] = m_context->CreateBuffer(
					kSdsmStatsBytes, smvk::BufferType_Storage, "sdsm stats");
			m_pendingSdsmStats.assign(kSdsmStatsBytes, 0);
		}
		catch (...) {
			std::cerr << "[WorldRenderer] sdsm_stats unavailable - SDSM falls back to the fixed split range\n";
			m_resources.sdsmStatsProgram = smvk::ShaderProgram{};
			m_resources.sdsmStatsModule = {};
		}
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
		// correctness requirement - the passthrough write satisfies both
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
		// Forward+ depth prepass's G-buffer writers - default.vert (the
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

		// SSSR trace Hi-Z build module (hzb_max.cs.hlsl - the MAX-reduce
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
		// renderer never imports a scene of its own. The host-installed
		// world source (SetWorldSource - the App's main.cpp loads the
		// pak) runs here, after the context exists. The world arrives
		// RESOLVED, its clusters/resources already uploaded to the
		// context - everything from here on renders by handles; this
		// class never parses a pak or imports a scene.
		if (!m_worldSource) {
			std::cerr << "[WorldRenderer] no world: install a world "
				"source (SetWorldSource)\n";
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
			<< m_world->materialLocations.size() << std::endl;

// Scene bounds log + default-camera framing (see below): the light set
// is the cooked authored set - no procedural grid to fit.
		{
			const ::Aabb bounds = m_world->ComputeSceneBounds();
			if (bounds.mx.x >= bounds.mn.x && bounds.mx.y >= bounds.mn.y && bounds.mx.z >= bounds.mn.z) {
				std::cerr << "[Scene] bounds min=(" << bounds.mn.x << "," << bounds.mn.y << "," << bounds.mn.z
					<< ") max=(" << bounds.mx.x << "," << bounds.mx.y << "," << bounds.mx.z << ")\n";
				// Framing: the authored DEFAULT CAMERA (world settings'
				// typed camera.default field, cooked into the pak) wins
				// over generic framing - the author's pose + lens is the
				// shot. Otherwise generic framing: no curated viewpoints
				// exist anymore - park the camera radius*2.2 from the
				// bounds center and fit the far clip to the framed view
				// (the 2000 m default stretches the cascade splits over
				// empty space; cascade 0 covers ~100 m -> blurry EVSM/PCF
				// at the mesh). Explicit VULKANTOY_CAM_POS/FWD/YAW pins
				// win over BOTH (bench determinism). YAW included: the
				// Configure-time yaw override is one of the documented
				// pins - without it here a yaw-only bench would silently
				// re-frame onto the generic orbit pose (observed: four
				// yaw A/B runs rendered bit-identical).
				if (!std::getenv("VULKANTOY_CAM_POS") &&
					!std::getenv("VULKANTOY_CAM_FWD") &&
					!std::getenv("VULKANTOY_CAM_YAW")) {
					const glm::vec3 center = (bounds.mn + bounds.mx) * 0.5f;
					const float radius =
						glm::length(bounds.mx - bounds.mn) * 0.5f;
					// Default camera: pose from the node's global
					// transform (forward = node -Z, engine space - the
					// same convention as the editor's locked
					// look-through), lens from the cameras-pool def.
					// Perspective lens swaps the App defaults (fov/near/
					// far; far fitted to cover the authored view). An
					// ORTHO lens keeps the App's perspective defaults
					// (the FPS camera has no ortho path - the pose still
					// applies).
					const ::Scene* fs = &m_world->scene;
					auto camIt = fs->cameraForNode.find(
						fs->defaultCameraNode);
					const ::SceneCamera* defCam = nullptr;
					if (fs->defaultCameraNode < fs->hierarchy.size()
						&& camIt != fs->cameraForNode.end()
						&& camIt->second
						< m_world->cameraLocations.size()) {
						const auto& loc =
							m_world->cameraLocations[camIt->second];
						if (loc.cluster && loc.handle.valid())
							defCam = loc.cluster->cameras.get(loc.handle);
					}
					bool framed = false;
					if (defCam) {
						const glm::mat4& G = fs->globalTransform[
							fs->defaultCameraNode];
						const glm::vec3 eye(G[3]);
						glm::vec3 fwd = glm::vec3(
							G * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
						if (glm::length(fwd) > 1e-6f) {
							fwd = glm::normalize(fwd);
							m_cameraPositioner.lookAt(eye, eye + fwd,
								ResolveLookAtUp(eye, eye + fwd));
							m_sceneNearClip = std::max(
								defCam->nearClip, 1e-4f);
							m_sceneFovYDeg = (defCam->projection
								== ::SceneCamera::Projection::Perspective)
								? std::clamp(defCam->fovYDeg, 1.0f, 179.0f)
								: kCameraFovYDegrees;
							// Far: the authored clip, but never shorter
							// than the generic fit would give (the
							// authored pose may look ACROSS the scene).
							const float dist = std::max(radius * 2.2f, 1.0f);
							m_sceneFarClip = std::clamp(
								std::max(defCam->farClip, dist + radius * 2.0f),
								50.0f, 10000.0f);
							framed = true;
							std::cerr << "[Scene] default camera '"
								<< (defCam->name.empty()
									? "<unnamed>" : defCam->name)
								<< "': eye=(" << eye.x << "," << eye.y
								<< "," << eye.z << ") fovY="
								<< m_sceneFovYDeg << " near="
								<< m_sceneNearClip << " far="
								<< m_sceneFarClip << "\n";
						}
					}
					if (!framed) {
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
		}

		// Authored punctual lights (KHR_lights_punctual -> pak): when the
		// scene carries them they replace the procedural demo set (sun
		// drives the environment, points/spots upload as the Forward+
		// set - see BuildCookedLightsFromWorld / UpdateForwardPlusLights).
		BuildCookedLightsFromWorld();

		// Named GI sidecars (pak Gi registry; one per kind for now).
		ResolveGiSidecars();

		// GeometryFX-style per-view filter (clustered): register static
		// meshes from cooked meshlets (pak path) or retained uploads
		// (procedural path), then bake the cluster-ordered index heap.
		// Non-fatal: unregistered meshes keep rendering via direct draws.
		// The pass config gates the whole filter (sample-level); the member
		// toggle stays as the runtime A/B switch.
		if (m_passes.gfxFilter && m_useGfxFilter) {
			m_gfxRenderer.ConfigureFrag(m_resources.fragShader);
			for (uint32_t li = 0; li < engine::GfxRenderer::LitSgVariantCount(); ++li)
				m_gfxRenderer.ConfigureLitFragSg(li, m_resources.fragLitSgModules[li]);
			// The GI permutation twins: without these every filtered
			// batch binds a base FS with the GI branches compiled out -
			// no GI ambient, no F8 debug views, on the whole clustered
			// scene (the direct draws select mainProgramGi separately).
			for (uint32_t gi = 0; gi < engine::GfxRenderer::LitGiVariantCount(); ++gi)
				m_gfxRenderer.ConfigureLitFragGi(gi, m_resources.fragLitGiModules[gi]);
			// Standard-material twins (MAT_STANDARD, layered machinery out):
			// the filtered/mesh picks use them for layerMaterials.w <= 1.
			m_gfxRenderer.ConfigureFragStd(m_resources.fragStdModule);
			for (uint32_t gi = 0; gi < engine::GfxRenderer::LitGiVariantCount(); ++gi)
				m_gfxRenderer.ConfigureLitFragGiStd(gi, m_resources.fragLitGiStdModules[gi]);
			// Material layered-ness for the std-twin pick (same lazy-resolve
			// pattern as ConfigureMaterialDepth - the world's material table
			// is authoritative; unknown = layered = monolith, fail-safe).
			m_gfxRenderer.ConfigureMaterialLayered(
				[this](uint32_t materialId) -> bool {
				if (!m_world)
					return true;
				const smvk::Material* mat = m_world->GetMaterial(materialId);
				return mat ? (mat->values.layerMaterials.w > 1) : true;
			});
			// Main-view seed masked phase is reversed-Z: prefer the
			// conservative shadow_rev module (falls back to the normal-Z
			// shadow.frag module - perf hint, not correctness).
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
				[this](uint32_t materialId) -> engine::MaterialDepthState {
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
		// SSSR thin G-buffer writers (the prepass's G-buffer FS pair -
		// empty programs keep the legacy VS-only prepass).
		m_fpRenderer.ConfigureGbuffer(m_resources.gbufferProgram,
			m_resources.gbufferMaskedProgram);

		// SG-GI baked probe volume (tools/gi_baker output): replaces the
		// analytic sky IBL ambient with path-traced probes when present.
		// Missing file is the normal state (no bake yet) - non-fatal.
		LoadGiVolume();
		// SG regular 3D probe volumes (DYNAMIC objects): artist OBB
		// grids from tools/gi_baker --regular-out. Missing manifest is
		// the normal state (no bake yet) - non-fatal.
		LoadGiRegularVolumes();
		// Convex GI volumes (dense probe grids in authored convex shapes,
		// cooked by cubemap reprojection; GiBaker --cv-spec). Composited
		// over the octree world field by priority at runtime. Missing
		// manifest is the normal state (no bake yet) - non-fatal.
		LoadCvolumes();
		// Reflection cubemaps for specular IBL (cubemap_baker output):
		// replaces the analytic sky specular with captured environment.
		// Missing file is non-fatal (falls back to analytic sky).
		LoadCubemaps();
		// Single-source isolation modes degrade when their source did not
		// load (All/SkyIbl need nothing): keeps them from being silently
		// empty - the UI/env selector keeps working after.
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
			std::cerr << "[WorldRenderer] Spot shadow atlas unavailable - spots run unshadowed\n";
		}
		// EVSM moment atlases (RGBA32F 2xMSAA + async blur). Non-fatal: when
		// the device lacks RGBA32F render/blur/filter support (or shaders
		// fail), EvsmActive() stays false and every consumer keeps the PCF path.
		if (!m_context->SupportsEvsm()) {
			std::cerr << "[WorldRenderer] EVSM unsupported (no RGBA32F 2xMSAA moments) - PCF shadows\n";
		}
		if (!m_viewportResources.evsmDir.Ensure(*m_context, *m_rendergraph, MakeEvsmDirDesc(m_evsmPipeline, m_cascadeResolution))) {
			std::cerr << "[WorldRenderer] EVSM directional atlas unavailable - PCF fallback\n";
		}
		if (!m_viewportResources.evsmSpot.Ensure(*m_context, *m_rendergraph, MakeEvsmSpotDesc(m_evsmPipeline))) {
			std::cerr << "[WorldRenderer] EVSM spot atlas unavailable - PCF fallback\n";
		}
		if (EvsmActive()) {
			std::cerr << "[EVSM] moment atlases ready: depth-only 2xMSAA D32 casters + per-sample"
				<< " resolve into single-sample RGBA32F + async blur ("
				<< (m_evsmPipeline == smvk::EvsmPipelineMode::SameFrame ? "SameFrame" : "OneFrameLag")
				<< (m_context->HasDedicatedComputeQueue() ? ", true-async queue" : ", emulated (no dedicated queue)")
				<< ")\n";
		}
		else if (m_evsmEnabled) {
			std::cerr << "[EVSM] inactive (unsupported or atlas failure) - PCF shadows\n";
		}
		if (!m_viewportResources.tonemap.Ensure(*m_context, MakeTonemapDesc())) {
			std::cerr << "[WorldRenderer] Failed to create tonemap program\n";
			return false;
		}
		// Histogram meter for auto-exposure (extent-independent buffer +
		// compute program). Non-fatal: without it the frame falls back to
		// manual exposure.
		if (!m_viewportResources.exposure.Ensure(*m_context, *m_rendergraph, MakeExposureDesc())) {
			std::cerr << "[WorldRenderer] Exposure meter unavailable - manual exposure only\n";
		}
		// Skydome program (fullscreen-triangle sky.vert/sky.frag). Failure is
		// non-fatal: the Main pass simply skips the sky draw and keeps the
		// clear color.
		if (!m_skydome.Ensure(*m_context)) {
			std::cerr << "[WorldRenderer] Skydome program unavailable - running without sky\n";
		}
		// Tiled terrain heightfield (visual-only dressing; Rendergraph/
		// terrain.h): a 4 km x 4 km field of 100 m tiles. Build is gated by
		// m_passes.terrain (off by default); the field is only built when the
		// pass is enabled. Failure is non-fatal (Main skips the field).
		if (m_passes.terrain) {
			smvk::terrain::TerrainDesc td{};
			if (!m_terrain.Build(*m_context, td))
				std::cerr << "[WorldRenderer] terrain build failed - running "
				"without the default field\n";
			// The vertex/index uploads must land before the first frame
			// records the terrain draws (staging flush; the surrounding
			// init flushed earlier uploads before this block).
			m_context->FlushUploads();
		}
		// Environment zone map (Rendergraph/envzones.h): DERIVED from the
		// world's zone entities (scene.envForNode + node transforms - see
		// RebuildEnvZoneMap). Runs after the world source (the zones ride
		// the loaded scene). Failure is non-fatal: every consumer takes
		// the plain env path (UINT32_MAX index).
		RebuildEnvZoneMap();
		// Chrome-ball probe visual (Rendergraph/probeball.h): the shared
		// sphere + programs; draws gate on ShadersValid (a missing .spv
		// only skips the balls).
		m_probeBalls.Ensure(*m_context);

		{
			auto vkExt = m_swapchain->GetExtent();
			const smvk::Extent2D swapExtent{ vkExt.width, vkExt.height };
			const smvk::Extent2D renderExtent = ScaledExtent(swapExtent, m_renderScale);
			const smvk::Extent2D uiExtent = ScaledExtent(swapExtent, m_uiScale);
		smvk::DepthDesc depthDesc{};
		depthDesc.format = m_swapchain->GetDepthFormat();
		// Target-heap window (design A - see DepthDesc): the temporal
		// seed writes this at TargetPhase::Seed, the post chain reads it
		// through tonemap. Disjoint from hizDepth's window below - the
		// two full-screen depths share one packed range.
		// A/B verified pixel-clean (0.005% temporal-noise-floor diff vs the
		// full-frame-default control; the two full-screen depths share one
		// 10.5 MiB range).
		depthDesc.firstPhase = smvk::TargetPhase::Seed;
		depthDesc.lastPhase = smvk::TargetPhase::Tonemap;
		if (!m_viewportResources.renderDepth.Ensure(*m_context, *m_rendergraph, renderExtent, depthDesc)) {
			std::cerr << "[WorldRenderer] Failed to create render depth target\n";
			return false;
		}
		// Option A occluder depth (HZB source): same extent/format as
		// the full depth - its OWN IMAGE (the two prepasses never write
		// one image) at a SHARED packed range: the use windows are
		// disjoint (this one's last read is the HZB pyramid build,
		// renderDepth's first write is the temporal seed).
		smvk::DepthDesc hizDepthDesc{};
		hizDepthDesc.format = m_swapchain->GetDepthFormat();
		hizDepthDesc.name = "hizDepth";
		hizDepthDesc.viewName = "hizDepthView";
		hizDepthDesc.logicalName = "hizDepthLogical";
		hizDepthDesc.firstPhase = smvk::TargetPhase::OccluderPrepass;
		hizDepthDesc.lastPhase = smvk::TargetPhase::Hzb;
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
					std::cerr << "[WorldRenderer] MSAA target unavailable - falling back to 1x\n";
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
				std::cerr << "[WorldRenderer] UI target unavailable - UI disabled\n";
			}
			if (!m_viewportResources.uiComposite.Ensure(*m_context)) {
				std::cerr << "[WorldRenderer] UI composite program unavailable\n";
			}
		}

		// ImGui overlay - init after swapchain/rendergraph are ready.
		// Uses dynamic rendering; no external RenderPass needed.
		m_imgui = smvk::mem::MakeProcess<smvk::ImGuiOverlay>();
		smvk::ImGuiOverlayDesc imguiDesc{};
		imguiDesc.fontPath = UI_FONT_PATH;
		imguiDesc.fontBaseSize = UI_FONT_BASE_SIZE;
		imguiDesc.colorFormat = LdrUnormSibling(); // must match the UI target above
		if (!m_imgui->Init(*m_context, *m_swapchain, m_host.GetWindow(), imguiDesc)) {
			std::cerr << "[WorldRenderer] Failed to init ImGuiOverlay - continuing without UI\n";
			m_imgui.reset();
		}
		else {
			m_uiDpiScale = m_imgui->AppliedDpiScale(); // auto = monitor content scale
			// (Start-in-FPS + the ImGui event hook moved to the ELEMENT's
			// onAttach — the mode switch is control; see
			// WorldRendererElement::onAttach.)
			// Feed every SDL event to the ImGui SDL3 backend (App::PollEvents).
			m_host.SetImGuiEventHook(&smvk::ImGuiOverlay::ProcessEvent);
		}

		return true;
	};

	void WorldRenderer::onDetach() {
		// Ensure GPU idle before destroying ImGui Vulkan resources/Holders.
		m_host.WaitForDeviceIdle();
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
	}

	// ---------------------------------------------------------------------------
	// SG-GI baked probe volume (tools/gi_baker output, see smvk::gi).
	// Reads the CLUSTER-OWNED bake payload (World::ReadBakeBytes - pak-
	// embedded or editor-injected), packs the GPU payload and uploads it
	// once; per-frame state is only the BDAs + the user toggles
	// (FrameUniforms.gi*). A missing payload is the normal "not baked
	// yet" state - the renderer keeps the analytic sky IBL.
	// ---------------------------------------------------------------------------
	void WorldRenderer::LoadGiVolume() {
		m_giLoaded = false;
		m_giVolume = smvk::gi::SgGiVolume{};
		m_giGridBuffer = {};
		m_giGridAddress = 0;
		m_giLevelTextures.clear();
		std::fill(std::begin(m_giLevelLayers), std::end(m_giLevelLayers),
			UINT32_MAX);

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
		const std::span<const uint8_t> bytes = ReadBakeBytes(vfsPath);
		if (bytes.empty()) {
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

		// v2 requirement: the dense runtime layers - the shader
		// trilinear-filters them (the probe-list walk is gone), so a file
		// without them has nothing to serve (v1 files die on the version
		// gate above; a bake that skipped BakeDenseLayers lands here).
		bool anyDense = false;
		for (const auto& dl : m_giVolume.denseLevels)
			anyDense = anyDense || dl.present;
		if (!anyDense) {
			std::cerr << "[GI] volume has no dense layers - re-bake with"
				" GiBaker (the runtime trilinear path needs them)\n";
			fallbackSky();
			return;
		}
		// Device gate (the SupportsEvsm lesson: capability, not
		// convention - BC6H 2D support proves nothing about sampled+linear
		// BC6H 3D): degrade to sky IBL instead of guessing.
		if (!m_context->SupportsBc6h3DSampling()) {
			std::cerr << "[GI] device lacks sampled+linear BC6H 3D -"
				" dense layers unusable (sky IBL ambient)\n";
			fallbackSky();
			return;
		}

		// Grid header only: the shader reads each level's volumeMin/
		// cellSize/dims through frame.giGrid. PackGpu also packs the
		// probe/cell/index payloads - transient CPU vectors, discarded
		// right here (the GPU probe arrays are gone with the walk).
		smvk::gi::SgGiGridGpu gridData{};
		{
			std::vector<smvk::gi::SgProbeGpu> probeData;
			std::vector<uint32_t> cellData;
			std::vector<uint32_t> indexData;
			m_giVolume.PackGpu(probeData, gridData, cellData, indexData);
			if (probeData.empty()) {
				std::cerr << "[GI] volume has no probes - ignored\n";
				fallbackSky();
				return;
			}
		}
		constexpr smvk::PipelineStage kGiStages =
			smvk::PipelineStage(smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment);
		m_giGridBuffer = m_context->CreateBuffer(
			sizeof(smvk::gi::SgGiGridGpu), smvk::BufferType_Storage, "sggi grid");
		m_context->UploadDataToBuffer(m_giGridBuffer, &gridData,
			sizeof(gridData), kGiStages, smvk::AccessFlags::ShaderRead);
		m_giGridAddress = m_context->GetBufferDeviceAddress(m_giGridBuffer);

		// Dense layers: cooked payloads straight to images - no resample,
		// no BC6H encode at load (GiBaker baked the visibility-weighted
		// blend and compressed it; the runtime only creates images). Per
		// level: 9 SG-lobe BC6H payloads + 1 R16F weight volume, all at
		// the REAL dims (no padding on ANY axis - BC6H blocks are 4x4x1
		// and partial edge blocks are legal, so the VkImage is exactly
		// dims; the natural uvw transform follows in the sampler).
		// Slots ride frame.giLevelLayers (level * 10 + layer;
		// UINT32_MAX = absent level, skipped by the sampler).
		{
			const auto& levels = m_giVolume.denseLevels;
			const uint32_t levelCount = uint32_t(std::min<size_t>(
				levels.size(), kGiDenseMaxLevels));
			uint32_t denseLevels = 0;
			size_t denseBytes = 0;
			for (uint32_t lvl = 0; lvl < levelCount; ++lvl) {
				const smvk::gi::SgGiVolume::DenseLevel& dl = levels[lvl];
				if (!dl.present)
					continue;
				// Rollback point: a failed level's already-created
				// textures are exactly the vector tail - erasing frees
				// them (Holder is RAII), so a half-created level leaks
				// nothing for the GI lifetime.
				const size_t texStart = m_giLevelTextures.size();
				bool levelOk = true;
				for (uint32_t lobe = 0;
					lobe < smvk::gi::kSgLobeCount && levelOk; ++lobe) {
					smvk::ImageCreateInfo ci{};
					ci.type = smvk::ImageType::Image3D;
					ci.extent = { dl.dims[0], dl.dims[1], dl.dims[2] };
					ci.format = smvk::Format::BC6H_UFloat;
					ci.usage = smvk::ImageUsage::Sampled
						| smvk::ImageUsage::TransferDst;
					ci.mipLevels = 1;
					ci.arrayLayers = 1;
					ci.name = "gi dense lobe";
					auto tex = m_context->CreateTexture(ci,
						dl.lobeBlocks[lobe].data());
					if (tex.empty()) {
						std::cerr << "[GI] dense level " << lvl
							<< " lobe " << lobe << " texture failed\n";
						levelOk = false;
						break;
					}
					m_giLevelLayers[lvl * kGiDenseLayersPerLevel + lobe] =
						m_context->GetTextureBindlessIndex(tex);
					m_giLevelTextures.push_back(std::move(tex));
				}
				if (levelOk) {
					smvk::ImageCreateInfo ci{};
					ci.type = smvk::ImageType::Image3D;
					ci.extent = { dl.dims[0], dl.dims[1], dl.dims[2] };
					ci.format = smvk::Format::R16_Float;
					ci.usage = smvk::ImageUsage::Sampled
						| smvk::ImageUsage::TransferDst;
					ci.mipLevels = 1;
					ci.arrayLayers = 1;
					ci.name = "gi dense weight";
					auto tex = m_context->CreateTexture(ci, dl.weight.data());
					if (tex.empty()) {
						std::cerr << "[GI] dense level " << lvl
							<< " weight texture failed\n";
						levelOk = false;
					}
					else {
						m_giLevelLayers[lvl * kGiDenseLayersPerLevel
							+ smvk::gi::kSgLobeCount] =
							m_context->GetTextureBindlessIndex(tex);
						m_giLevelTextures.push_back(std::move(tex));
					}
				}
				if (!levelOk) {
					// Free the level's partial set + revert its slots
					// (UINT32_MAX = absent; the sampler skips it).
					m_giLevelTextures.resize(texStart);
					for (uint32_t t = 0; t < kGiDenseLayersPerLevel; ++t)
						m_giLevelLayers[lvl * kGiDenseLayersPerLevel + t] =
						UINT32_MAX;
					continue;
				}
				// Bytes counted only for fully-created levels (failed
				// levels own nothing after the rollback above).
				++denseLevels;
				denseBytes += dl.Bytes();
			}
			std::cerr << "[GI] dense layers: " << denseLevels << "/"
				<< levelCount << " levels ("
				<< (denseBytes / 1024) << " KiB BC6H+R16F)\n";
			if (denseLevels == 0) {
				std::cerr << "[GI] no usable dense levels - sky IBL ambient\n";
				fallbackSky();
				return;
			}
		}
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
	// Reads textures/cubemaps/<scene>.{cubemanifest,ktx2} through the CLUSTER
	// bake-payload channel (pak-embedded or editor-injected). A missing
	// payload is non-fatal (falls back to analytic sky).
	// ---------------------------------------------------------------------------
	void WorldRenderer::LoadCubemaps() {
		m_cubemaps.entries.clear();
		m_cubemapProbes.clear();
		m_cubemapRows.clear();
		m_cubemapStreamPrimed = false;

		// Clamp sampler for TextureCube sampling (cube seams must not wrap;
		// linear min/mag + linear mips for the GGX-prefiltered chains).
		// Created once - GetOrCreateSampler dedupes, so this is cheap after
		// the first scene load. The shader reads it via FrameUniforms
		// (cubemapSampler), never samplers[0] (Repeat would bleed seams).
		m_cubemapSampler = m_context->GetOrCreateSampler(smvk::CubemapSampler());

		// Try a pak-registered bake first (the cubemanifest GUID from the
		// pak's Gi registry - stable across pak/scene renames), then the
		// scene stem, then the "sky" fallback.
		std::vector<std::string> cubemapBases;
		if (!m_giCubemapName.empty())
			cubemapBases.push_back(m_giCubemapName);
		cubemapBases.push_back(m_sceneId);
		cubemapBases.push_back("sky");

		bool fileBakeFound = false;
		// Pinned manifest rows ('pin', cook --cubemap-extra-pos) sort
		// before every auto-placed probe (ties keep the first entry, so
		// an artist-placed capture - e.g. at the chrome ball - wins
		// against overlapping coverage probes). They stay resident always
		// (see m_cubemapRows); other rows stream by camera distance.
		for (const auto& base : cubemapBases) {
			if (base.empty())
				continue;

			// 1. Grid manifest: the cook writes
			//    textures/cubemaps/<base>.cubemanifest when one capture
			//    bakes N > 1 probes (<base>_0.ktx2, ...). Positions ride
			//    the manifest in cook/world meters (trace BVH space =
			//    World::ComputeSceneBounds space - both apply the scene
			//    global transforms).
			const std::span<const uint8_t> manifestBytes =
				ReadBakeBytes(smvk::CubemapManifestVfsPath(base));
			if (!manifestBytes.empty()) {
				std::string mErr;
				std::vector<smvk::CubemapManifestEntry> rows;
				const std::string_view text(
					reinterpret_cast<const char*>(manifestBytes.data()),
					manifestBytes.size());
				if (!smvk::CubemapManifestParse(text, rows, mErr)) {
					std::cerr << "[Cubemap] manifest rejected '"
						<< smvk::CubemapManifestVfsPath(base) << "': "
						<< mErr << "\n";
					continue;
				}
				// Pinned rows first (stable: file order within groups).
				std::stable_partition(rows.begin(), rows.end(),
					[](const smvk::CubemapManifestEntry& e) { return e.pinned; });
				// Streaming setup (see m_cubemapRows): rows are metadata
				// until needed - only pinned rows (artist captures, always
				// resident) upload now; the rest stream by camera distance
				// in UpdateCubemapStreaming. Row order stays pinned-first
				// so the probe-row rebuild keeps the pinned-wins-ties
				// contract.
				m_cubemapRows.clear();
				m_cubemapRows.reserve(rows.size());
				size_t pinnedResident = 0;
				size_t usableRows = 0;
				for (const auto& row : rows) {
					// Derive the library name from the KTX basename so the
					// entry key stays human-readable (e.g. "sponza_3").
					std::string entryName = row.vfsPath;
					if (auto slash = entryName.find_last_of('/'); slash != std::string::npos)
						entryName = entryName.substr(slash + 1);
					if (auto dot = entryName.find_last_of('.'); dot != std::string::npos)
						entryName = entryName.substr(0, dot);
					CubemapRow crow{};
					crow.entryName = entryName;
					crow.vfsPath = row.vfsPath;
					crow.position = row.position;
					crow.influenceRadius = row.influenceRadius;
					crow.fadeRadius = row.fadeRadius;
					crow.pinned = row.pinned;
					if (!row.pinned) {
						m_cubemapRows.push_back(std::move(crow));
						++usableRows;
						continue;
					}
					const std::span<const uint8_t> rowBytes =
						ReadBakeBytes(row.vfsPath);
					if (rowBytes.empty()) {
						std::cerr << "[Cubemap] manifest row missing: "
							<< row.vfsPath << " - skipped\n";
						crow.failed = true;
						m_cubemapRows.push_back(std::move(crow));
						continue;
					}
					if (!m_cubemaps.Load(*m_context, entryName, rowBytes)) {
						std::cerr << "[Cubemap] manifest row load failed: "
							<< row.vfsPath << " - skipped\n";
						crow.failed = true;
						m_cubemapRows.push_back(std::move(crow));
						continue;
					}
					const auto* entry = m_cubemaps.Find(entryName);
					if (!entry || entry->bindless < 0) {
						crow.failed = true;
						m_cubemapRows.push_back(std::move(crow));
						continue;
					}
					if (!entry->baked) {
						// Decode failed and Load fell back to the neon
						// procedural pattern: never place that as a grid
						// probe (recook the row instead).
						std::cerr << "[Cubemap] manifest row not a cubemap: "
							<< row.vfsPath << " - skipped\n";
						m_cubemaps.entries.erase(entryName);
						crow.failed = true;
						m_cubemapRows.push_back(std::move(crow));
						continue;
					}
					m_cubemapRows.push_back(std::move(crow));
					++pinnedResident;
					++usableRows;
				}
				if (usableRows == 0) {
					std::cerr << "[Cubemap] manifest '" << base
						<< "' has 0 usable rows - trying next candidate\n";
					m_cubemapRows.clear();
					m_cubemapProbes.clear();
					continue;
				}
				RebuildCubemapProbeRows();
				std::cerr << "[Cubemap] cached manifest '" << base << "': "
					<< usableRows << "/" << rows.size() << " rows"
					<< (pinnedResident > 0
						? " (" + std::to_string(pinnedResident) + " pinned resident)"
						: "")
					<< " - the rest stream by distance\n";
				fileBakeFound = true;
				break;
			}

			// 2. Single capture: textures/cubemaps/<base>.ktx2. Probe rows
			//    are built after the loop (global probe at the cached scene
			//    center) so manifest + single paths share one upload.
			if (const std::span<const uint8_t> single =
				ReadBakeBytes(smvk::CubemapVfsPath(base));
				!single.empty()) {
				if (m_cubemaps.Load(*m_context, base, single)) {
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
				const std::span<const uint8_t> legacyBytes =
					ReadBakeBytes(smvk::CubemapVfsPath(name));
				if (legacyBytes.empty())
					break;
				if (m_cubemaps.Load(*m_context, name, legacyBytes)
					&& m_cubemaps.Find(name)) {
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

	}

	// Rebuilds the cached probe rows from resident entries (see
	// m_cubemapRows): bindless indices are baked into the rows at upload,
	// so every residency change (stream load/evict) rebuilds. Order:
	// pinned rows first (artist-placed captures win weight ties - ties
	// keep the first entry), then widest influence within each block
	// (the lit-shader probe walk breaks at the first full-weight entry,
	// and tile overflow keeps the earliest upload slots). m_cubemapRows
	// stays pinned-first (stable_partition at parse), so resident pinned
	// rows are already front - the sorts below only order within the
	// blocks. Probe data rides the FP light buffer as type-2 entries,
	// appended post-selection in UpdateForwardPlusLights.
	void WorldRenderer::RebuildCubemapProbeRows() {
		m_cubemapProbes.clear();
		if (m_cubemapRows.empty())
			return;
		m_cubemapProbes.reserve(m_cubemapRows.size());
		size_t pinnedResident = 0;
		for (const CubemapRow& row : m_cubemapRows) {
			if (row.failed)
				continue;
			const auto* entry = m_cubemaps.Find(row.entryName);
			if (!entry || entry->bindless < 0 || !entry->baked)
				continue; // not resident (streamed out or never loaded)
			CubemapProbeGpu p{};
			p.posInfluence = glm::vec4(row.position, row.influenceRadius);
			p.fadeCubemap = glm::vec4(row.fadeRadius,
				static_cast<float>(entry->bindless),
				static_cast<float>(entry->mipLevels), 0.0f);
			m_cubemapProbes.push_back(p);
			if (row.pinned)
				++pinnedResident;
		}
		const size_t pinSplit = std::min(pinnedResident, m_cubemapProbes.size());
		std::sort(m_cubemapProbes.begin(), m_cubemapProbes.begin() + pinSplit,
			[](const CubemapProbeGpu& a, const CubemapProbeGpu& b) {
				return a.posInfluence.w > b.posInfluence.w;
			});
		std::sort(m_cubemapProbes.begin() + pinSplit, m_cubemapProbes.end(),
			[](const CubemapProbeGpu& a, const CubemapProbeGpu& b) {
				return a.posInfluence.w > b.posInfluence.w;
			});
	}

	// Per-frame probe residency (see m_cubemapRows): pinned rows are
	// always resident (loaded at parse); other rows stream by camera
	// distance - the nearest-K plus any probe whose influence+fade
	// covers the eye (mandatory: the shader breaks at the first
	// full-weight entry, so a covering probe MUST be resident or nearby
	// lighting pops to the global fallback). Loads throttle (KTX decode
	// is synchronous); evictions carry hysteresis (rank slack + range
	// margin) so boundary orbits don't thrash; any residency change
	// rebuilds the probe rows (fresh bindless indices). The first update
	// after a load fills synchronously (no throttle), so the opening
	// frame already shades with its neighborhood.
	void WorldRenderer::UpdateCubemapStreaming(const glm::vec3& eye) {
		if (m_cubemapRows.empty() || !m_world || !m_cubemapEnabled)
			return;
		// Headless budget pin (VULKANTOY_CUBEMAP_BUDGET=N): bench A/B over
		// the resident set + eviction verification without the UI slider.
		// The UI slider stays live afterwards (this wins while set).
		uint32_t budget = m_cubemapStreamBudget;
		if (const char* e = std::getenv("VULKANTOY_CUBEMAP_BUDGET")) {
			const long v = std::atol(e);
			if (v >= 0 && v <= 64)
				budget = uint32_t(v);
		}
		const bool priming = !m_cubemapStreamPrimed;
		if (priming) {
			m_cubemapStreamPrimed = true;
		}
		else if ((static_cast<uint32_t>(m_frameCounter) & 3u) != 0u) {
			return; // throttle like texture streaming (loads hitch)
		}
		// Rank non-pinned, non-failed rows by camera distance.
		struct Rank { size_t row; float dist; };
		std::vector<Rank> ranks;
		ranks.reserve(m_cubemapRows.size());
		std::vector<size_t> rankOf(m_cubemapRows.size(), SIZE_MAX);
		for (size_t i = 0; i < m_cubemapRows.size(); ++i) {
			const CubemapRow& row = m_cubemapRows[i];
			if (row.pinned || row.failed)
				continue;
			rankOf[i] = ranks.size();
			ranks.push_back(Rank{ i, glm::distance(eye, row.position) });
		}
		std::sort(ranks.begin(), ranks.end(),
			[](const Rank& a, const Rank& b) { return a.dist < b.dist; });
		for (size_t r = 0; r < ranks.size(); ++r)
			rankOf[ranks[r].row] = r;
		auto isResident = [&](const CubemapRow& row) {
			const auto* entry = m_cubemaps.Find(row.entryName);
			return entry && entry->bindless >= 0 && entry->baked;
		};
		bool changed = false;
		uint32_t loadsDone = 0, evictsDone = 0;
		// Loads: nearest-K + covering (throttled past the priming fill).
		uint32_t loadsLeft = priming ? UINT32_MAX : m_cubemapStreamLoadsPerUpdate;
		for (const Rank& r : ranks) {
			CubemapRow& row = m_cubemapRows[r.row];
			if (isResident(row))
				continue;
			const bool inBudget =
				rankOf[r.row] < size_t(budget);
			const bool covering =
				r.dist < row.influenceRadius + row.fadeRadius;
			if (!inBudget && !covering)
				continue;
			if (loadsLeft == 0)
				continue;
			--loadsLeft;
			const std::span<const uint8_t> bytes = ReadBakeBytes(row.vfsPath);
			if (bytes.empty()) {
				std::cerr << "[Cubemap] stream row missing: "
					<< row.vfsPath << " - skipped\n";
				row.failed = true;
				continue;
			}
			if (!m_cubemaps.Load(*m_context, row.entryName, bytes)) {
				std::cerr << "[Cubemap] stream row load failed: "
					<< row.vfsPath << " - skipped\n";
				row.failed = true;
				continue;
			}
			const auto* entry = m_cubemaps.Find(row.entryName);
			if (!entry || entry->bindless < 0 || !entry->baked) {
				if (entry && !entry->baked)
					m_cubemaps.entries.erase(row.entryName);
				row.failed = true;
				continue;
			}
			++loadsDone;
			changed = true;
		}
		// Evictions (hysteresis: rank slack + range margin past fade).
		if (!priming) {
			for (const Rank& r : ranks) {
				CubemapRow& row = m_cubemapRows[r.row];
				if (!isResident(row))
					continue;
				const bool overBudget = rankOf[r.row]
					>= size_t(budget)
						+ size_t(m_cubemapStreamEvictRankSlack);
				const bool outOfRange = r.dist > row.influenceRadius
					+ row.fadeRadius + m_cubemapStreamEvictRangeMargin;
				if (overBudget && outOfRange) {
					m_cubemaps.entries.erase(row.entryName);
					++evictsDone;
					changed = true;
				}
			}
		}
		if (changed) {
			RebuildCubemapProbeRows();
			std::cerr << "[Cubemap] streaming: +" << loadsDone << "/-"
				<< evictsDone << " -> " << m_cubemapProbes.size()
				<< " resident probe rows\n";
		}
	}

	// ---------------------------------------------------------------------------
	// SG 2D lightmaps (STATIC geometry; cooked with PakCooker --lightmap).
	// Loads the registry lightmap (gi/<name>.sglm, else gi/<scene>.sglm)
	// and uploads ONLY the 9 RGBA16F atlas
	// layers. The chart uvs + per-chart atlas transforms + per-mesh lobe
	// counts are COOKED into the pak (texCoord2/chartId +
	// StaticMesh::lmChartXforms, StaticMesh::lmChunk riding the draw
	// ring) - no chunk buffer exists at runtime. Missing file =
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

	// ---------------------------------------------------------------------------
	// SG regular 3D probe volumes (DYNAMIC objects; tools/gi_baker
	// --regular-out). Reads the cluster bake channel at gi/<scene>.regmanifest: one .sgrv
	// path per line, up to SG_REG_MAX_VOLUMES (8). Each volume uploads 7
	// RGBA16F 3D textures + one OBB descriptor; the shader hardware-
	// trilinearly filters between neighboring probes. Missing manifest =
	// "not baked yet" - non-fatal.
	// ---------------------------------------------------------------------------
	void WorldRenderer::LoadGiRegularVolumes() {
		m_regLoaded = false;
		m_regEnabled = false;
		m_regVolumes.clear();
		m_regObbsBuffer = {};
		m_regObbsAddress = 0;

		const std::string manifestPath = !m_giRegName.empty()
			? std::string("gi/") + m_giRegName + ".regmanifest"
			: std::string("gi/") + m_sceneId + ".regmanifest";
		const std::span<const uint8_t> manifestBytes =
			ReadBakeBytes(manifestPath);
		if (manifestBytes.empty()) {
			std::cerr << "[REG] no volume manifest '" << manifestPath
				<< "' (bake: GiBaker --regular-out)\n";
			return;
		}
		// Line-separated .sgrv vfs paths. Bake-asset rows carry the
		// payload guid first ("<guid> <path>  # <note>"); legacy and
		// hand-edited rows are bare paths - SplitManifestRow keeps both.
		std::vector<std::string> lines;
		{
			std::string cur;
			for (char c : manifestBytes) {
				if (c == '\n' || c == '\r') {
					const std::string path = smvk::bake::SplitManifestRow(cur);
					if (!path.empty())
						lines.push_back(path);
					cur.clear();
				}
				else {
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
			const std::span<const uint8_t> volBytes = ReadBakeBytes(line);
			if (volBytes.empty()) {
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
	// Reads the cluster bake channel at gi/<scene>.cvmanifest: one .cvol path per line, up
	// to SG_CV_MAX_VOLUMES (8). Each volume uploads 7 RGBA16F 3D textures
	// (dense probe grid, hardware trilinear) + one 80 B descriptor with
	// the fade-expanded hull AABB (broad-phase reject), the CPU-packed
	// world->uvw lattice transform and the plane span into a shared plane
	// pool (per-plane fade scale/bias precomputed - no per-pixel division).
	// Volumes are uploaded priority-sorted (LOW first) so the shader's
	// compositing walk (HIGH first, transmittance form) is a plain loop -
	// higher priority overrides lower (FOX override-volume rule).
	// Missing manifest = "not baked yet" - non-fatal.
	// ---------------------------------------------------------------------------
	// GPU pack (must mirror CvVolumeGpu/CvPlaneGpu in
	// sg_lightmap_types.hlsli - all float4/uint4 rows, std430-clean).
	struct CvolPlaneGpu {
		float weightEquation[4]; // xyz -normal*fadeScale, w fadeBias - d*fadeScale
	};
	struct CvolVolumeGpu {
		float aabbMin[4];       // xyz fade-expanded hull AABB min, w pad
		float aabbMax[4];       // xyz fade-expanded max, w pad
		float uvwScale[4];      // xyz 1/(spacing*max(dims,1)), w pad
		float uvwBias[4];       // xyz -gridMin*uvwScale, w pad
		uint32_t planeCountBase[4]; // x plane count, y first pool plane, zw pad
	};
	struct CvolVolumesGpu {
		CvolVolumeGpu volumes[8];          // SG_CV_MAX_VOLUMES
		CvolPlaneGpu planes[8 * 16];      // SG_CV_MAX_VOLUMES * SG_CV_MAX_PLANES
	};
	static_assert(sizeof(CvolPlaneGpu) == 16);
	static_assert(sizeof(CvolVolumeGpu) == 80);
	static_assert(sizeof(CvolVolumesGpu) == 8 * 80 + 128 * 16);

	void WorldRenderer::LoadCvolumes() {
		m_cvolLoaded = false;
		m_cvolEnabled = false;
		m_cvolVolumes.clear();
		m_cvolVolumesBuffer = {};
		m_cvolVolumesAddress = 0;

		const std::string manifestPath = !m_giCvolName.empty()
			? std::string("gi/") + m_giCvolName + ".cvmanifest"
			: std::string("gi/") + m_sceneId + ".cvmanifest";
		const std::span<const uint8_t> manifestBytes =
			ReadBakeBytes(manifestPath);
		if (manifestBytes.empty()) {
			std::cerr << "[CV] no convex-volume manifest '" << manifestPath
				<< "' (bake: GiBaker --cv-spec <file>)\n";
			return;
		}
		// Line-separated .cvol vfs paths (bake-asset rows carry the
		// payload guid first - see smvk::bake::SplitManifestRow; '#' comments
		// and blanks skipped - GiBaker writes a header comment).
		std::vector<std::string> lines;
		{
			std::string cur;
			for (char c : manifestBytes) {
				if (c == '\n' || c == '\r') {
					const std::string path = smvk::bake::SplitManifestRow(cur);
					if (!path.empty())
						lines.push_back(path);
					cur.clear();
				}
				else {
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
			const std::span<const uint8_t> volBytes = ReadBakeBytes(line);
			if (volBytes.empty()) {
				std::cerr << "[CV] missing volume '" << line << "' - skipped\n";
				continue;
			}
			CvolVolume cv{};
			std::string verr;
			if (!cv.volume.ReadFromMemory(volBytes, verr)) {
				std::cerr << "[CV] volume rejected: " << verr << "\n";
				continue;
			}
			// Bake-content audit (distinguishes an empty bake - all-zero
			// coeffs claim full weight and black the field - from a
			// sampling/align bug, which shows nonzero content here).
			{
				double sum = 0.0, mx = 0.0;
				size_t nz = 0, total = 0;
				for (const glm::vec3& a : cv.volume.coeffs) {
					const double l = std::abs(a.x) + std::abs(a.y) + std::abs(a.z);
					sum += l;
					mx = std::max(mx, l);
					nz += (l > 1e-6) ? 1 : 0;
					++total;
				}
				std::cerr << "[CV] coeffs '" << line << "': voxels="
					<< (total / 9) << " nonzero=" << nz << "/" << total
					<< " mean=" << (total ? sum / total : 0.0)
					<< " max=" << mx << "\n";
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
			// Isotropic-mean texture (the volumetric fog's one-fetch path,
			// PackGpuMeanLayer): same failure discipline as the 7 layers -
			// a volume publishes only with all 8 textures up.
			{
				std::vector<uint16_t> meanLayer;
				cv.volume.PackGpuMeanLayer(meanLayer);
				smvk::ImageCreateInfo mci{};
				mci.type = smvk::ImageType::Image3D;
				mci.extent = { cv.volume.dims[0], cv.volume.dims[1],
					cv.volume.dims[2] };
				mci.format = smvk::Format::RGBA16_Float;
				mci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
				mci.mipLevels = 1;
				mci.arrayLayers = 1;
				mci.name = "cvol mean";
				auto meanTex = m_context->CreateTexture(mci, meanLayer.data());
				if (meanTex.empty()) {
					std::cerr << "[CV] mean texture creation failed\n";
					ok = false;
				}
				else {
					cv.meanTex = std::move(meanTex);
					cv.meanSlot = m_context->GetTextureBindlessIndex(cv.meanTex);
				}
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
		// upload order, so later entries win - the FOX override rule.
		std::stable_sort(m_cvolVolumes.begin(), m_cvolVolumes.end(),
			[](const CvolVolume& a, const CvolVolume& b) {
			return a.volume.def.priority < b.volume.def.priority;
		});

		// Descriptor pack (headers + shared plane pool, see CvolVolumesGpu).
		// Priority ride: upload order == compositing priority (LOW first,
		// the shader walks HIGH first) - the old explicit priority field
		// was dead weight, the order carries it.
		CvolVolumesGpu descs{};
		uint32_t planeBase = 0;
		for (size_t v = 0; v < m_cvolVolumes.size(); ++v) {
			const smvk::gi::CvVolume& vol = m_cvolVolumes[v].volume;
			CvolVolumeGpu& g = descs.volumes[v];
			// Fade-expanded hull AABB (broad-phase reject): Bounds() is
			// already a generous superset of the convex hull, grown by the
			// widest outsideFade per axis - a superset of the weight>0
			// region, so an AABB reject is always exact. An unbounded
			// plane set (bad authoring) keeps the volume loadable like
			// before: a huge box means the per-pixel plane tests decide.
			glm::vec3 mn = glm::vec3(-1e30f), mx = glm::vec3(1e30f);
			float fadeOut = 0.0f;
			for (const smvk::gi::CvPlane& pl : vol.def.planes)
				fadeOut = std::max(fadeOut, pl.outsideFade);
			{
				glm::vec3 bmn, bmx;
				if (vol.def.Bounds(bmn, bmx)) {
					mn = bmn - glm::vec3(fadeOut);
					mx = bmx + glm::vec3(fadeOut);
				}
			}
			g.aabbMin[0] = mn.x; g.aabbMin[1] = mn.y; g.aabbMin[2] = mn.z;
			g.aabbMax[0] = mx.x; g.aabbMax[1] = mx.y; g.aabbMax[2] = mx.z;
			// CPU-packed world->uvw lattice transform (the old shader
			// rebuilt this from gridMin/spacing/dims per tap):
			// uvw = saturate(pos * uvwScale + uvwBias), with the degenerate
			// guards (dims >= 1, spacing*dims >= 1e-6) baked in.
			const glm::vec3 d = glm::max(
				glm::vec3(vol.dims[0], vol.dims[1], vol.dims[2]), glm::vec3(1.0f));
			const glm::vec3 s = glm::max(
				glm::vec3(vol.spacing) * d, glm::vec3(1e-6f));
			const glm::vec3 scale = 1.0f / s;
			g.uvwScale[0] = scale.x; g.uvwScale[1] = scale.y; g.uvwScale[2] = scale.z;
			g.uvwBias[0] = -vol.gridMin.x * scale.x;
			g.uvwBias[1] = -vol.gridMin.y * scale.y;
			g.uvwBias[2] = -vol.gridMin.z * scale.z;
			// Plane span into the shared pool, FOLDED into one ramp:
			// t = saturate(dot(wEq.xyz, pos) + wEq.w) with
			// wEq = (-n * fadeScale, fadeBias - d * fadeScale) - the
			// packed form of the old smoothstep(-outsideFade,
			// insideFade, inside) with the division hoisted here.
			// Degenerate spans (< 1e-5 m, authored-CPU step branch)
			// pack as a 1e-5 m ramp - identical below any
			// content-relevant epsilon.
			const uint32_t planeCount = std::min<uint32_t>(
				uint32_t(vol.def.planes.size()), 16u);
			g.planeCountBase[0] = planeCount;
			g.planeCountBase[1] = planeBase;
			for (uint32_t p = 0; p < planeCount; ++p) {
				const smvk::gi::CvPlane& pl = vol.def.planes[p];
				CvolPlaneGpu& pg = descs.planes[planeBase + p];
				// Folded ramp: t = saturate(dot(wEq.xyz, pos) + wEq.w)
				// with wEq = (-n * fadeScale, fadeBias - d * fadeScale)
				// (fadeScale = 1/max(in+out fade), fadeBias =
				// out*fadeScale) - one 16 B slot, dot3 + FMA + saturate.
				const float span = std::max(pl.insideFade + pl.outsideFade, 1e-5f);
				const float fadeScale = 1.0f / span;
				pg.weightEquation[0] = -pl.normal.x * fadeScale;
				pg.weightEquation[1] = -pl.normal.y * fadeScale;
				pg.weightEquation[2] = -pl.normal.z * fadeScale;
				pg.weightEquation[3] = pl.outsideFade * fadeScale
					- pl.d * fadeScale;
			}
			planeBase += planeCount;
		}
		constexpr smvk::PipelineStage kCvStages =
			smvk::PipelineStage(smvk::PipelineStageBit_Fragment);
		m_cvolVolumesBuffer = m_context->CreateBuffer(
			sizeof(CvolVolumesGpu), smvk::BufferType_Storage, "cvol volumes");
		m_context->UploadDataToBuffer(m_cvolVolumesBuffer, &descs,
			sizeof(CvolVolumesGpu), kCvStages, smvk::AccessFlags::ShaderRead);
		m_cvolVolumesAddress = m_context->GetBufferDeviceAddress(m_cvolVolumesBuffer);

		m_cvolLoaded = true;
		m_cvolEnabled = true; // on by default when baked; UI can toggle
		// Headless A/B (same state as the panel checkbox): VULKANTOY_CVOL=0
		// drops the FULL variant to Octree for the run (isolates whether a
		// dark interior comes from the convex volumes or the octree field).
		if (const char* cvolEnv = std::getenv("VULKANTOY_CVOL")) {
			if (std::atoi(cvolEnv) == 0)
				m_cvolEnabled = false;
		}
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

	void WorldRenderer::DegradeAmbientForMissingSources() {}

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
		// must not contain the jitter - it travels as jitterOffset).
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
		}
		else if (m_dlssFrameActive) {
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

		data.time = m_timeSinceStart;
		data.viewportSize = { width_, height_ };
		data.invViewportSize = { 1.0f / width_, 1.0f / height_ };

		data.nearPlane = default_near_clip;
		data.farPlane = m_sceneFarClip;
		// Linear exposure multiplier for the tonemapper (ACES).
		// Sun is 5000 lux (see UpdateLights); 0.001 puts a sun-facing
		// mid-gray (albedo 0.18, N.L=1) at HDR ~ 0.29 pre-ACES -> no clipping.
		// NOTE: the LIVE tonemap knob is the tonemap push constant, not this
		// field - but FrameUniforms.exposure IS read by the shaders: the
		// debug-view FSes pre-scale their authored display-space palette by
		// 1/max(frame.exposure,1e-6) so flat readouts survive any exposure
		// (default_unlit.frag.hlsl:163, default_wire.frag.hlsl:30,
		// default_cascade_debug.frag.hlsl:95, default_vertexcolor_debug.
		// frag.hlsl:130, default_uv_debug.hlsli:112, default_lit_body.
		// hlsli:1221/1233/1241/1290). Keep it in sync with the effective
		// exposure (auto-adapted or manual) - dropping it would break the
		// debug-view gain.
		data.exposure = FrameExposure();
		// AO (DORMANT since the SSGI rework): the lighting pass no longer
		// samples AO - Main exports the AO-sensitive lighting through the
		// aoTerm MRT target and the SSGI compose pass applies it (see
		// ssgi.h + ssao_compose.frag.hlsl). These FrameUniforms fields
		// stay in the layout (offsets asserted vs the C++ mirror; removing
		// them would shift every later field) and stay 0/off so any future
		// reader reads "no AO here".
		data.aoTextureIndex = UINT32_MAX;
		data.aoIntensity = 0.0f;
		// Configurable skydome IBL + fog (single source for sky/sun/fog,
		// built from the world's scene env by the caller).
		data.env = env.ToFrameData();
		// Ambient sources (N cycles All -> Lightmaps -> Probes -> SkyIbl
		// -> Off): per-source isolation - the sky IBL flag enables the
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
			// so the shader never sees a mode without fresh buffers. These
			// ride the standalone DsVizParams ring (NOT FrameUniforms).
			{
				DsVizParams viz{};
				viz.tileMasks = ds.TileMasksAddress(*m_context, currentFrame);
				viz.tileGroupMasks = ds.TileGroupMasksAddress(*m_context, currentFrame);
				viz.mode = static_cast<uint32_t>(m_dsDebugMode);
				viz.light = static_cast<uint32_t>(
					std::clamp(m_dsDebugLight, 0, std::max(0, (int)m_dsLightCount - 1)));
				viz.flags = m_dsDebugOverlay ? 1u : 0u;
				viz.countCeil = static_cast<uint32_t>(
					std::clamp(m_dsDebugCountCeil, 1, (int)smvk::kDsMaxLightsPerTile));
				m_context->UploadDataToBuffer(
					m_resources.dsVizParamsBuffer,
					&viz,
					sizeof(DsVizParams),
					smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
					smvk::AccessFlags::ShaderRead,
					0,
					size_t(currentFrame) * sizeof(DsVizParams)
				);
				// Fast-path bit for the viz overlay (lets the lit FS skip the
				// viz BDA chase when the mode is off). Set from the same
				// condition the shader would otherwise derive after loading.
				if (m_dsDebugMode != DsDebugViz::Off)
					data.flags |= kFrameFlagDsViz;
			}
#endif
		}
		else {
			data.dsTileState = 0;
			data.dsLightList = 0;
			data.dsVisibility = 0;
			data.dsTileCountX = 0;
			data.dsEnabled = 0u;
#if defined(DS_DEBUG_VIZ)
			// Viz payload zeroed (mode 0 = off) so a stale slice can
			// never enable the overlay without fresh buffers.
			{
				const DsVizParams viz{};
				m_context->UploadDataToBuffer(
					m_resources.dsVizParamsBuffer,
					&viz,
					sizeof(DsVizParams),
					smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
					smvk::AccessFlags::ShaderRead,
					0,
					size_t(currentFrame) * sizeof(DsVizParams)
				);
			}
#endif
		}

		// GI debug view + shared intensity ride EVERY frame regardless of
		// which source is active: the lit shader's F8/I debug block colors
		// lightmap / grid / probe pixels alike (mode 1 flat irradiance,
		// 2 source palette, 3 isolated level, 4 dominant level - the
		// sggi walk's level-isolation + argmax debugs), so the views must
		// survive source isolation (e.g. Lightmaps mode with no probe
		// volume loaded). The level byte carries the mode-3 isolated
		// level (see SgGiSample's dbgLvl).
		data.giIntensity = m_giIntensity;
		data.giDebug = (uint32_t(std::clamp(m_giDebug, 0, 4)) & 0xffu)
			| (uint32_t(std::clamp(m_giDebugLevel, 0, 7) & 0xff) << 8);
		// SG-GI probe volume (grid BDA + the per-mode flag). GiActive() is
		// the single gate so the shader flag and the buffers can never
		// disagree. The giProbes/giCells/giIndices slots are legacy-zero
		// (the dense BC6H layers replaced the probe-list walk; the fields
		// stay for the FrameUniforms mirror layout).
		if (GiActive()) {
			data.giProbes = 0;
			data.giGrid = m_giGridAddress;
			data.giCells = 0;
			data.giIndices = 0;
			data.giEnabled = 1u;
			// Dense layers (the BC6H Texture3D runtime path): the sampler
			// trilinear-filters these (cooked payloads, baked by GiBaker).
			for (uint32_t s = 0;
				s < kGiDenseMaxLevels * kGiDenseLayersPerLevel; ++s)
				data.giLevelLayers[s] = m_giLevelLayers[s];
		}
		else {
			data.giProbes = 0;
			data.giGrid = 0;
			data.giCells = 0;
			data.giIndices = 0;
			data.giEnabled = 0u;
			for (uint32_t& slot : data.giLevelLayers)
				slot = UINT32_MAX;
		}
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
		}
		else {
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
			for (uint32_t v = 0; v < n; ++v) {
				for (uint32_t t = 0; t < 7; ++t)
					data.cvolLayers[v * 7u + t] = m_cvolVolumes[v].layerSlots[t];
				// Volumetric one-fetch mean slots (fog-only payload).
				data.cvolMeanLayers[v] = m_cvolVolumes[v].meanSlot;
			}
			data.cvolCount = n;
			data.cvolEnabled = ProbesWanted() ? 1u : 0u;
		}
		else {
			data.cvolVolumes = 0;
			for (uint32_t& slot : data.cvolLayers)
				slot = UINT32_MAX;
			for (uint32_t& slot : data.cvolMeanLayers)
				slot = UINT32_MAX;
			data.cvolCount = 0u;
			data.cvolEnabled = 0u;
		}
		// Convex-volume tile masks: published under the decal tile-list
		// freshness contract above, with the DsActive() term the dispatch
		// gate carries (TileCullWillRun) - the address only goes out when
		// THIS frame's dispatch refreshes the masks. Zeros (= no publish)
		// make the shader walk every volume instead; the per-volume AABB
		// reject in SgCvWeight keeps that path correct.
		if (m_viewportResources.forwardPlus.Valid() && TileCullWillRun()) {
			data.cvolTileMasks =
				m_viewportResources.forwardPlus.VolumeTileMasksAddress(*m_context);
			data.cvolTileCountX = m_viewportResources.forwardPlus.tileCountX;
			data.cvolTileSize = m_viewportResources.forwardPlus.desc.tileSize;
		}
		data.evsmMode = EvsmActive() ? 1u : 0u;

		// ---- Environment zone map (see Rendergraph/envzones.h): publish
		// whenever built (the shader flag and GPU contents can never
		// disagree); m_envZonesEnabled only gates the index (off = the
		// UINT32_MAX sentinel -> every consumer takes the plain env path).
		// The zone definitions ride the FrameUniforms tail every frame (the
		// panel's sliders mutate the desc live; the map's WEIGHTS are the
		// painted state - re-uploaded on paint cadence via UpdateTexture).
		{
			for (uint32_t z = 0; z < smvk::envzones::kEnvZoneMax; ++z) {
				const smvk::envzones::EnvZone& zd =
					m_envZones.desc.zones[z];
				data.envZoneSky[z] = glm::vec4(zd.skyTint, zd.ambientI);
				data.envZoneFog[z] = glm::vec4(zd.fogColor, zd.fogDensity);
				data.envZoneVol[z] = glm::vec4(zd.volumetrics,
					zd.volumetricDensity, 0.0f, 0.0f);
			}
			data.envZoneWorldSize = m_envZones.desc.worldSize;
			data.envZoneMapIndex = (m_envZones.Valid() && m_envZonesEnabled)
				? m_envZones.bindlessIndex : UINT32_MAX;
			data.envZoneSampler = 0; // bindless slot 0: linear
		}

		// ---- Forward+ decals (tiled projected splats) ----
		// Contract with the cull + shading shaders (see forwardplus.cs.hlsl
		// + default_lit_body.hlsli): decalCount > 0 only when the tile-cull
		// dispatch runs this frame (DecalsActive gates both), so the decal
		// tile lists are always fresh; the tile-list BDAs are filled
		// whenever the builder is valid regardless of the toggle - the
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
		}
		else {
			data.decals = 0;
			data.decalCount = 0;
		}

		// Destination scope covers every consumer: BDA reads in vertex,
		// fragment (lighting/materials/env) and compute (Forward+ cull).
		static constexpr smvk::PipelineStage kFrameUploadStages =
			smvk::PipelineStage(smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute);

		// This view's FramePSHot slab BDA (the FS reaches the hot struct
		// through FrameUniforms - pc.frameHot is gone, see
		// push_constants.h): slab 0, same math as the hot upload below.
		data.frameHot = m_context->GetBufferDeviceAddress(m_resources.frameHot)
			+ currentFrame * sizeof(FramePSHot);
		// This view's DsVizParams slice (standalone viz payload ring; the
		// fill/upload below targets the same slice).
		data.dsVizParams = m_context->GetBufferDeviceAddress(m_resources.dsVizParamsBuffer)
			+ size_t(currentFrame) * sizeof(DsVizParams);

		m_context->UploadDataToBuffer(
			m_resources.frameUniforms,
			&data,
			sizeof(FrameUniforms)/* * 1*/,
			kFrameUploadStages,
			smvk::AccessFlags::ShaderRead,
			0,
			currentFrame * sizeof(FrameUniforms)
		);

		// Upload FramePSHot - tiny hot struct for pixel shader register pressure reduction.
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
		// shadow-casting light - built from the world's scene env by
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
		// ---- Split range: SDSM + temporal stabilization ----
		// GPU-driven, distribution-based fit (see the SDSM block in the header
		// for the full design): sdsm_stats.cs.hlsl measures the CURRENT frame's
		// depth (min/max + a 64-bin log histogram), the own async ticket lands
		// it ONE frame later, and this fit consumes the 1%/99% percentile range
		// (one tiny distant object cannot dictate the whole range). The APPLIED
		// range then follows the measured range with asymmetric hysteresis -
		// instant expand (never clip a receiver), slow contract (~2%/frame -
		// the projections don't swim) - quantized to 0.25 m steps (tiny
		// distribution changes don't move the light projections at all).
		// CUTS discard the history: a camera teleport (per-frame position jump)
		// also COOLS DOWN on the fixed heuristic range for 2 frames - the
		// 1-frame-old stats describe the OLD view, so they are wrong for the
		// new one; the fresh post-cut stats snap in after the cooldown. Stale
		// stats (disarmed/failed readback > 3 frames) and a measured range
		// escaping the applied one by > 30% snap immediately.
		float splitNear = default_near_clip;
		float splitFar = CascadeFar();
		const bool sdsmUsable = m_sdsmEnabled && m_sdsmStatsValid
			&& m_sdsmPctMax > m_sdsmPctMin;
		if (sdsmUsable) {
			// Camera teleport cut: a large single-frame position jump. A fast
			// flythrough is ~0.2 m/frame; a cut/teleport is meters instantly.
			const glm::vec3 camPos = m_cameraPositioner.cameraPosition_;
			const bool camTeleport = m_sdsmLastCamPosValid
				&& glm::length(camPos - m_sdsmLastCamPos) > 1.0f;
			m_sdsmLastCamPos = camPos;
			m_sdsmLastCamPosValid = true;
			// Stats staleness: the armed pass + own ticket keep the measure
			// age at ~1 frame; older means the readback starved (or the stats
			// path was off) - the history is stale, not the scene changing.
			const uint64_t statsAge = m_frameCounter - m_sdsmMeasureFrame;
			if (camTeleport) {
				m_sdsmCutCooldown = 2; // heuristic fallback frames (old-view stats are wrong)
				m_sdsmAppliedValid = false;
			}
			else if (statsAge > 3) {
				m_sdsmAppliedValid = false; // snap to the fresh measurement below
			}
			if (m_sdsmCutCooldown > 0) {
				--m_sdsmCutCooldown;
				// Heuristic fallback while the old-view stats are stale: the
				// wide range is always safe (never clips), just coarser.
			}
			else {
				// The target range: percentile bounds + 2%/5% padding against
				// the 1-frame fit racing a moving camera.
				const float wantNear = std::clamp(m_sdsmPctMin * 0.98f,
					default_near_clip, CascadeFar() * 0.5f);
				const float wantFar = std::clamp(m_sdsmPctMax * 1.05f,
					wantNear * 2.0f, CascadeFar());
				if (!m_sdsmAppliedValid) {
					// Snap (first fit after a cut/staleness - quantized below).
					m_sdsmAppliedMin = wantNear;
					m_sdsmAppliedMax = wantFar;
					m_sdsmAppliedValid = true;
				}
				else {
					const float span = std::max(m_sdsmAppliedMax - m_sdsmAppliedMin, 0.25f);
					// Heavy escape: the measured range outran the applied one by
					// > 30% of the span (a look-around found a much deeper view,
					// the hysteresis is too slow) - snap both ends.
					const bool heavyEscape =
						m_sdsmPctMin < m_sdsmAppliedMin - span * 0.3f
						|| m_sdsmPctMax > m_sdsmAppliedMax + span * 0.3f;
					if (heavyEscape) {
						m_sdsmAppliedMin = wantNear;
						m_sdsmAppliedMax = wantFar;
					}
					else {
						// Asymmetric hysteresis: expand INSTANTLY toward the
						// measured (padded) bounds (never clip a receiver),
						// contract slowly (~2% of the span per frame - the
						// projections don't swim with fit noise).
						if (wantNear < m_sdsmAppliedMin)
							m_sdsmAppliedMin = wantNear; // instant expand (nearer)
						else
							m_sdsmAppliedMin = std::min(wantNear,
								m_sdsmAppliedMin + span * 0.02f); // slow contract
						if (wantFar > m_sdsmAppliedMax)
							m_sdsmAppliedMax = wantFar; // instant expand (farther)
						else
							m_sdsmAppliedMax = std::max(wantFar,
								m_sdsmAppliedMax - span * 0.02f); // slow contract
					}
				}
				// Quantize the applied range to 0.25 m steps (floor the near /
				// ceil the far - quantization only ever widens): the fit noise
				// below half a step never moves the projections.
				constexpr float kSdsmQuantum = 0.25f;
				m_sdsmAppliedMin = std::floor(m_sdsmAppliedMin / kSdsmQuantum) * kSdsmQuantum;
				m_sdsmAppliedMax = std::ceil(m_sdsmAppliedMax / kSdsmQuantum) * kSdsmQuantum;
				splitNear = std::clamp(m_sdsmAppliedMin, default_near_clip, CascadeFar() * 0.5f);
				splitFar = std::clamp(m_sdsmAppliedMax, splitNear * 2.0f, CascadeFar());
				if (!m_sdsmLogged) {
					m_sdsmLogged = true;
					std::fprintf(stderr,
						"[SDSM] fitted split range %.3f..%.3f m (measured %.3f..%.3f, pct %.3f..%.3f, heuristic %.3f..%.3f)\n",
						splitNear, splitFar, m_sdsmMin, m_sdsmMax,
						m_sdsmPctMin, m_sdsmPctMax,
						(double)default_near_clip, (double)CascadeFar());
					fflush(stderr);
				}
			}
		}
		else {
			m_sdsmAppliedValid = false; // stats cold / SDSM off: fixed heuristic
		}
		auto splits = smvk::ComputeCascadeSplits(splitNear, splitFar, m_cascadeLambda, NUM_CASCADES);

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
			m_cascadeSplitsDbg[i] = splits[i];		for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
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
		// above trails one slot), baked with LAST frame's fit - uploading the
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

	// Pool index -> bindless slot for the world's cluster textures (see
	// the header). The pool==bindless identity is CREATION-ORDER LUCK that
	// the windowed App gets by loading its world first into a fresh context
	// - long-lived editing sessions do not: bindless entries churn across\r
	// + scene re-imports (envzone map, LUT,
	// GI dense layers) and a re-import allocates cluster textures BEHIND
	// still-live entries, so pool indices stop matching slots and the raw
	// upload below made the shaders sample WRONG (or freed, dangling-
	// descriptor) slots - the same device-death class the editor's own
	// UploadMaterials fixed with m_bindlessBySlot (see editor_render.cpp
	// "[MATSLOT] remap MISS" note: "the observed device-death"). Rebuilt
	// per call so live editor texture edits apply without a rebind signal.
	std::unordered_map<uint32_t, uint32_t> WorldRenderer::BuildPoolToBindlessMap() const {
		std::unordered_map<uint32_t, uint32_t> byPool;
		for (const auto& clusterPtr : m_world->clusters) {
			if (!clusterPtr)
				continue;
			for (const auto& [uri, h] : clusterPtr->ListTextureUris()) {
				(void)uri; // enumeration only: pool idx -> RHI handle
				if (h.empty())
					continue;
				// RHI texture pool indices are GLOBAL (one pool per
				// context), so cluster ordering cannot collide keys.
				byPool[h.index()] = m_context->GetTextureBindlessIndex(h);
			}
		}
		return byPool;
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
		// POOL -> BINDLESS remap (the engine twin of the editor's own
		// material upload fix): MaterialValues.textures store RHI texture
		// POOL indices, but the shaders index the bindless descriptor array.
		// The identity only holds while nothing else churns the bindless
		// pool (see BuildPoolToBindlessMap). A MISS means the pool index
		// points at no live texture (stale material reference into a
		// destroyed texture): upload it raw, matching the editor's
		// diagnostic behavior - the freed-slot null-write in impl.cpp's
		// destroy(BindlessImageHandle) makes that a safe black sample
		// instead of the old dangling-descriptor device death.
		const std::unordered_map<uint32_t, uint32_t> bindlessByPool =
			BuildPoolToBindlessMap();
		auto remapSlot = [&](int poolIdx) -> int {
			if (poolIdx < 0)
				return -1;
			const auto hit = bindlessByPool.find(static_cast<uint32_t>(poolIdx));
			if (hit == bindlessByPool.end()) {
				static uint64_t s_matSlotMissTick = 0;
				if (s_matSlotMissTick++ < 16)
					std::fprintf(stderr,
						"[MATSLOT] engine remap MISS: poolIdx=%d not live (map=%zu) - raw upload (null-sample)\n",
						poolIdx, bindlessByPool.size());
				return poolIdx;
			}
			return static_cast<int>(hit->second);
		};
		std::vector<smvk::MaterialValues> mats(uploadCount);
		for (uint32_t i = 0; i < uploadCount; ++i) {
			if (auto* mat = m_world->GetMaterial(i)) {
				mats[i] = mat->values;
				mats[i].textures.x = remapSlot(mat->values.textures.x);
				mats[i].textures.y = remapSlot(mat->values.textures.y);
				mats[i].textures.z = remapSlot(mat->values.textures.z);
				mats[i].textures.w = remapSlot(mat->values.textures.w);
				mats[i].textures2.x = remapSlot(mat->values.textures2.x);
			}
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
			// The streamer keys demand by BINDLESS slot (RequestMip/
			// RequestMipsByDistance look entries up through
			// GetTextureBindlessIndex), but MaterialValues.textures store
			// POOL indices - remap through the same pool->bindless map as
			// UpdateMaterials, or the CPU demand leg misses every entry
			// once pool!=bindless (re-imports) and streamed
			// textures never upgrade on distance.
			const std::unordered_map<uint32_t, uint32_t> bindlessByPool =
				BuildPoolToBindlessMap();
			auto note = [&](int poolIdx, float dist) {
				if (poolIdx < 0)
					return;
				const auto hit = bindlessByPool.find(
					static_cast<uint32_t>(poolIdx));
				if (hit == bindlessByPool.end())
					return; // no live texture: nothing to demand
				auto it = minDist.find(hit->second);
				if (it == minDist.end() || dist < it->second)
					minDist[hit->second] = dist;
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

	// Gfx-filter demand wiring (draw-level designated-publisher source; the
	// replacement for the retired per-fragment feedback): the filter's
	// PublishDrawDemand resolves each in-frustum draw's material through
	// the materials buffer, so THIS frame's BDA slice + the streamer's mip
	// bias go in once per frame, before AppendPasses records the filter.
	// The per-slot meta table re-uploads only when the streamer's epoch
	// moves (register/unregister - slots are stable across retargets).
	void WorldRenderer::UpdateGfxTextureDemand(uint32_t frameIndex) {
		if (!m_textureStreamingEnabled || !m_textureStreamer)
			return;
		smvk::gfx::State& gfx = m_gfxRenderer.State();
		if (!smvk::gfx::Valid(gfx))
			return;
		// THIS frame's MaterialValues slice (same resolution the main view
		// uses for pc.materialsBuffer: base + frameIndex * slab).
		const uint64_t materialsBda =
			m_context->GetBufferDeviceAddress(m_resources.materialsBuffer)
			+ frameIndex * sizeof(smvk::MaterialValues) * MAX_MATERIALS;
		smvk::gfx::SetDemandInputs(gfx, *m_context, materialsBda,
			m_textureStreamer->GetConfig().mipBias);
		const uint64_t epoch = m_textureStreamer->SlotMetaEpoch();
		if (epoch == m_gfxMetaEpochUploaded)
			return;
		// Typed copy (streamer -> gfx layouts are deliberately decoupled;
		// the loop is the trust boundary).
		const std::vector<smvk::TextureStreamer::TextureSlotMeta> slots =
			m_textureStreamer->BuildSlotMeta();
		std::vector<smvk::gfx::TextureMeta> meta(slots.size());
		for (size_t i = 0; i < slots.size(); ++i) {
			meta[i].longSide = slots[i].longSide;
			meta[i].fullMips = slots[i].fullMips;
		}
		if (smvk::gfx::UploadTextureMeta(gfx, *m_context, epoch,
			meta.data(), meta.size()))
			m_gfxMetaEpochUploaded = epoch;
		// Upload failure: the gfx-cached epoch stays stale, so this
		// re-attempts next frame.
	}

	// Histogram readback switch. ON: the engine download now uses a
	// persistent grow-on-demand staging buffer (no per-call staging
	// create/deferred destroy) and ProcessDeferrredTasks runs ready tasks
	// outside its mutex, so the old re-entrancy hazard (destroy ->
	// DeferDestroy -> AddDeferredTask re-locking the drain's non-recursive
	// m_deferredMutex) is gone.
	static constexpr bool kHistogramReadbackEnabled = true;

	// Metering gates shared by the histogram download and the batched
	// feedback pairing (debug views hold the last scene exposure).
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
		if (m_debugView != ViewMode::Lit)
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
	// SsrTaaEndFrame invalidates - the first lit record after the view
	// warms up instead of blending against pre-view history).
	bool WorldRenderer::DebugVisualizationActive() const {
		// F2..F11 replace the Main opaque/transparent legs with flat
		// programs; the GI debug views (R/F8) and the DS visualizers (Y)
		// early-out of the lit fragment shader the same way.
		if (m_debugView != ViewMode::Lit)
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
				SMVK_TRACE_LOG(
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
		// Adapt on the ELAPSED time since the last completed adapt, not one
		// frame's delta: polls complete every ~4-8 frames (every-4th submit +
		// 1-2 frame poll latency), so a single-frame dt made the effective
		// convergence ~4-8x slower than the "/s" the UI labels promise. The
		// clamp keeps a stall (load hitch, debug-view freeze) from slamming
		// the exposure to its target in one step - the classic post-hitch
		// brightness pop.
		const float adaptDt = std::min(m_exposureAdaptDt, 0.25f);
		m_exposureAdaptDt = 0.0f;
		m_exposureAdapted = meter.Adapt(m_exposureAdapted, m_exposureEvBias,
			m_exposureSpeedToDark, m_exposureSpeedToBright,
			m_exposureMin, m_exposureMax, adaptDt);
	}

	// Async GPU->CPU readback tick (exposure histogram + texture feedback
	// + deferred-shadow counters). SUBMIT + POLL on a timeline semaphore -
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
		// Adaptation clock: accumulate the elapsed time BETWEEN adapt steps
		// (the steps only run when a poll completes, every ~4-8 frames) so
		// AdaptExposureFromBins can damp with the true elapsed seconds -
		// see the note on m_exposureAdaptDt.
		m_exposureAdaptDt += m_deltaTime;
		// Gfx draw-level demand source (designated publisher): active once
		// the filter runs AND the meta table is live (streaming enabled +
		// at least one streamed slot). Gated off => no download leg, no
		// per-frame clear (the shader's publisher is gated off too).
		smvk::gfx::State& gfx = m_gfxRenderer.State();
		const bool gfxDemandActive = m_textureStreamingEnabled
			&& m_textureStreamer
			&& smvk::gfx::Valid(gfx)
			&& smvk::gfx::TextureMetaCount(gfx) > 0;
		// GPU-cull kept-counter leg: active whenever the GPU cull chain ran
		// (the word rides the cullState slice the dispatches wrote).
		const bool gfxKeptActive = smvk::gfx::Valid(gfx) && gfx.gpuCull
			&& !smvk::gfx::CullStateBuffer(gfx, frameSlot).empty();
		// Layout helper: the SAME span shape must accompany the submit and
		// its poll (destinations are stable members; handles/sizes are fixed).
		std::array<smvk::BufferDownload, 4> dls{};
		const auto buildPendingDownloads = [&]() -> std::span<const smvk::BufferDownload> {
			uint32_t n = 0;
			if (m_pendingReadbackHasExposure) {
				dls[n] = { meter.HistogramBuffer(m_pendingReadbackSlot),
					m_pendingExposureBins.data(),
					m_pendingExposureBins.size() * sizeof(uint32_t), 0 };
				++n;
			}
			if (m_pendingReadbackHasGfxDemand) {
				dls[n] = { smvk::gfx::DemandBuffer(gfx, m_pendingReadbackSlot),
					m_pendingGfxDemand.data(),
					m_pendingGfxDemand.size() * sizeof(uint32_t), 0 };
				++n;
			}
			if (m_pendingReadbackHasDs) {
				dls[n] = { m_viewportResources.deferredShadows.CounterBuffer(
						m_pendingReadbackSlot),
					m_pendingDsCounters.data(), m_pendingDsCounters.size()
					* sizeof(uint32_t), 0 };
				++n;
			}
			if (m_pendingReadbackHasGfxKept) {
				// 4 bytes at the counter word (see the member block): the
				// final kept-entry count of that retired dispatch chain.
				dls[n] = { smvk::gfx::CullStateBuffer(gfx, m_pendingReadbackSlot),
					&m_pendingGfxKept, sizeof(uint32_t),
					smvk::gfx::kCullStateCounterWord * sizeof(uint32_t) };
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
				// Gfx draw-level texture demand (retired slice, ~a few frames
				// old): full-chain mips, min-wins inside RequestMip - the CPU
				// distance source merges the same way. kDemandNoReport slots
				// (off-target / occluded / unstreamed) are skipped; absent
				// requests let the streamer decay them toward the floor.
				if (m_pendingReadbackHasGfxDemand && gfxDemandActive) {
					const auto slotMap = m_textureStreamer->BuildBindlessSlotMap();
					const size_t n = std::min(slotMap.size(), m_pendingGfxDemand.size());
					uint32_t reported = 0;
					for (size_t slot = 0; slot < n; ++slot) {
						const uint32_t want = m_pendingGfxDemand[slot];
						if (want == smvk::gfx::kDemandNoReport)
							continue;
						m_textureStreamer->RequestMip(slotMap[slot], want, streamFrame);
						++reported;
					}
					m_gfxDemandReported = reported;
					// Tick-counter throttle (not the frame-parity gates: polls
					// land 1-2 frames after the submit tick, so a frame-parity
					// gate on the poll frame misaligns with every submit
					// cadence - the first 5 + every 64th always land in a
					// 74-frame bench).
					static uint64_t s_gfxDemandTick = 0;
					const uint64_t dTick = s_gfxDemandTick++;
					if (dTick < 5 || dTick % 64 == 0) {
						std::fprintf(stderr,
							"[gfxDemand] tick=%llu slice=%u reported=%u (async)\n",
							(unsigned long long)m_frameCounter,
							m_pendingReadbackSlot, reported);
					}
				}
				// Deferred-shadow counters (retired slice, ~a few frames old):
				// overlay slots + throttled telemetry. Saturation of the
				// flexible allocations means tiles/lights degrade (clamped
				// stores, shadows read as fully lit).
			// GPU-cull kept count (retired slice, ~a few frames old): the
			// final kept-entry count of that dispatch chain - the number
			// the CPU stats cannot see in GPU mode (the cull runs in
			// compute; the CPU stat only counts the CPU-mode path).
			if (m_pendingReadbackHasGfxKept) {
				m_gfxKeptBatches = m_pendingGfxKept;
				m_gfxKeptValid = true;
			}
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
			// ExposureMeteringActive gates the SUBMIT too, not just the
			// adapt: with auto off from boot the histogram zero-fill +
			// compute never ran, so the retired slices are uninitialized
			// device memory - submitting their downloads anyway (the old
			// gate) meant the first 1-2 polls after pressing X adapted
			// once from garbage bins. Same while any debug view holds the
			// meter frozen (stale pre-view bins). No submit, no stale read.
			m_pendingReadbackHasExposure = kHistogramReadbackEnabled
				&& meterValid && ExposureMeteringActive();
			// Gfx demand slice rides the same batch (no extra submit): sized
			// to the LIVE slot count at submit time (the retired slice holds
			// at least that many reporting draws' publications; the poll's
			// span shape uses this same member sizing).
			m_pendingReadbackHasGfxDemand = gfxDemandActive;
			if (m_pendingReadbackHasGfxDemand)
				m_pendingGfxDemand.assign(
					smvk::gfx::TextureMetaCount(gfx), smvk::gfx::kDemandNoReport);
			// GPU-cull kept counter (4 B) rides the same batch (no extra
			// submit): see the member block - the CPU stats cannot see the
			// GPU-mode cull, so the counter word is the kept number.
			m_pendingReadbackHasGfxKept = gfxKeptActive;
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

		// TMP-DIAG (VULKANTOY_FILTERAUDIT=<frame>): one-shot async download
		// of the filter chain's per-frame state (draw verdicts + kept bits +
		// indirect commands + batch headers of the retired flight slice —
		// same retirement rule as the demand ring above), printed at poll
		// time. The distant-LOD-sibling draw-drop hunt: the data side
		// (meshlet ids/bounds/cones, the cluster index ranges, the mesh
		// registrations) audits clean and every flag-gated cull stage can be
		// disabled without restoring the draws, so the kill must show up in
		// these words: a 0 verdict (the draw-cull frustum on the union
		// bound), 0 kept (the batch-cull sphere/cone) or indexCount==0
		// commands (the filter's compaction/ring cap). Remove later.
		{
			static const int filterAuditFrame = [] {
				const char* e = std::getenv("VULKANTOY_FILTERAUDIT");
				return e ? std::atoi(e) : -1;
			}();
			if (filterAuditFrame >= 0 && smvk::gfx::Valid(gfx)
				&& !gfx.views.empty()) {
				smvk::gfx::View& v0 = gfx.views[0];
				const auto buildAuditSpan = [&]() -> std::span<const smvk::BufferDownload> {
					static std::array<smvk::BufferDownload, 5> s_auditDls{};
					uint32_t an = 0;
					s_auditDls[an++] = { v0.cullStateBuffers[m_filterAuditSlot],
						m_filterAuditVerdicts.data(),
						m_filterAuditVerdicts.size() * sizeof(uint32_t), 0 };
					for (uint32_t reg = 0; reg < 2; ++reg)
						s_auditDls[an++] = { v0.keptBuffers[m_filterAuditSlot],
							m_filterAuditKept[reg].data(),
							m_filterAuditKept[reg].size() * sizeof(uint32_t),
							reg * smvk::gfx::kMaxBatchSlots * sizeof(uint32_t) };
					s_auditDls[an++] = { v0.indirectBuffers[m_filterAuditSlot],
						m_filterAuditIndirect.data(),
						m_filterAuditIndirect.size() * sizeof(uint32_t), 0 };
					s_auditDls[an++] = { v0.batchBuffers[m_filterAuditSlot],
						m_filterAuditBatches.data(),
						m_filterAuditBatches.size() * sizeof(uint32_t), 0 };
					return { s_auditDls.data(), an };
				};
				if (m_filterAuditTicket != 0) {
					if (m_context->PollAsyncDownloadDataFromBuffers(
							m_filterAuditTicket, buildAuditSpan())) {
						m_filterAuditTicket = 0;
						std::fprintf(stderr,
							"[FILTERAUDIT] slot=%u draws=%u batches=%u "
							"filteredCounter=%u\n",
							m_filterAuditSlot, m_filterAuditDrawCount,
							m_filterAuditBatchCount,
							m_filterAuditVerdicts[
								smvk::gfx::kCullStateCounterWord]);
						for (uint32_t d = 0; d < m_filterAuditDrawCount; ++d) {
							uint32_t keptCount[2] = { 0, 0 };
							uint32_t clusterCount = 0;
							uint32_t cmdCount = 0;
							uint64_t sumIndex = 0;
							for (uint32_t s = 0; s < m_filterAuditBatchCount; ++s) {
								if (m_filterAuditBatches[s * 4 + 1] != d)
									continue;
								++clusterCount;
								for (uint32_t reg = 0; reg < 2; ++reg)
									if (m_filterAuditKept[reg][s] != 0)
										++keptCount[reg];
								const uint32_t indexCount =
									m_filterAuditIndirect[s * 5 + 0];
								if (indexCount > 0) {
									++cmdCount;
									sumIndex += indexCount;
								}
							}
							const uint32_t mesh = d < m_filterAuditMeshes.size()
								? m_filterAuditMeshes[d] : 0xFFFFFFFFu;
							std::fprintf(stderr,
								"[FILTERAUDIT] draw=%u mesh=%u verdict=%u "
								"kept0=%u/%u kept1=%u/%u cmds=%u sumIdx=%llu\n",
								d, mesh, m_filterAuditVerdicts[d],
								keptCount[0], clusterCount,
								keptCount[1], clusterCount,
								cmdCount, (unsigned long long)sumIndex);
						}
						// Per-slot screen footprints (the same projection the
						// filter's own stages use): answers WHICH clusters
						// cover a screen band — kept or killed. In particular
						// whether the black-band draws' killed clusters land
						// ON the band pixels (cone kill with misplaced/wrong
						// cones: the bound is on-screen but the cone rejects
						// it) or OFF it (sphere kill: the bound itself is
						// misplaced — the triangles project elsewhere than
						// their bounds claim).
						{
							const smvk::gfx::View& vv = gfx.views[0];
							for (uint32_t s = 0; s < m_filterAuditBatchCount; ++s) {
								const uint32_t bd = m_filterAuditBatches[s * 4 + 1];
								const uint32_t bcl = m_filterAuditBatches[s * 4 + 0];
								const bool keptLive = m_filterAuditKept[0][s] != 0
									|| m_filterAuditKept[1][s] != 0;
								// Only the partially-killed draws (the black
								// band): full-kept draws drown the log.
								bool partial = false;
								{
									uint32_t kc = 0, tc = 0;
									for (uint32_t s2 = 0; s2 < m_filterAuditBatchCount; ++s2) {
										if (m_filterAuditBatches[s2 * 4 + 1] != bd)
											continue;
										++tc;
										if (m_filterAuditKept[0][s2] != 0
											|| m_filterAuditKept[1][s2] != 0)
											++kc;
									}
									partial = (kc < tc);
								}
								if (!partial)
									continue;
								if (bd >= vv.drawConstants.size())
									continue;
								const smvk::gfx::Draw& ddc = vv.drawConstants[bd];
								size_t ci = ddc.mesh;
								// NOTE: drawConstants[].mesh is the gfx mesh
								// index; the cluster table below is indexed by
								// the batch's absolute cluster id.
								if (bcl >= gfx.clusters.size())
									continue;
								const smvk::gfx::Cluster& ccl = gfx.clusters[bcl];
								const glm::vec3 wc = glm::vec3(
									ddc.model * glm::vec4(ccl.boundCenter, 1.0f));
								const float bs = std::max({ glm::length(glm::vec3(
									ddc.model[0])), glm::length(glm::vec3(
									ddc.model[1])), glm::length(glm::vec3(
									ddc.model[2])) });
								const smvk::gfx::ClusterFootprint f =
									smvk::gfx::ProjectClusterFootprint(wc,
										ccl.boundRadius * bs, vv.eye, vv.viewProj,
										vv.width, vv.height);
								const uint32_t idxCnt =
									m_filterAuditIndirect[s * 5 + 0];
								std::fprintf(stderr,
									"[FILTERAUDIT] slot draw=%u slot=%u cl=%u "
									"kept=%u idx=%u c=(%.0f,%.0f) r=(%.0f,%.0f)\n",
									bd, s, bcl, keptLive ? 1u : 0u, idxCnt,
									f.centerX, f.centerY, f.radiusX, f.radiusY);
							}
						}
					}
				}
				else if ((int)m_frameCounter >= filterAuditFrame
					&& v0.batchCount > 0) {
					// Submit at the pinned frame (the retired slice the
					// demand ring reads: the previous same-flight user's
					// state — the static bench camera keeps it
					// representative).
					m_filterAuditSlot = frameSlot;
					m_filterAuditBatchCount = v0.batchCount;
					m_filterAuditDrawCount = v0.stats.draws;
					m_filterAuditVerdicts.assign(
						smvk::gfx::kCullStateWords, 0u);
					for (uint32_t reg = 0; reg < 2; ++reg)
						m_filterAuditKept[reg].assign(v0.batchCount, 0u);
					m_filterAuditIndirect.assign(
						size_t(v0.batchCount) * 5, 0u);
					m_filterAuditBatches.assign(
						size_t(v0.batchCount) * 4, 0u);
					m_filterAuditMeshes.clear();
					for (const smvk::gfx::Draw& dc : v0.drawConstants)
						m_filterAuditMeshes.push_back(dc.mesh);
					m_filterAuditTicket = m_context
						->SubmitAsyncDownloadDataFromBuffers(buildAuditSpan());
					std::fprintf(stderr,
						"[FILTERAUDIT] submit frame=%llu slot=%u ticket=%llu "
						"batches=%u draws=%u kept=%u cullState=%u indirect=%u batch=%u\n",
						(unsigned long long)m_frameCounter, m_filterAuditSlot,
						(unsigned long long)m_filterAuditTicket,
						m_filterAuditBatchCount, m_filterAuditDrawCount,
						(uint32_t)v0.keptBuffers[m_filterAuditSlot].valid(),
						(uint32_t)v0.cullStateBuffers[m_filterAuditSlot].valid(),
						(uint32_t)v0.indirectBuffers[m_filterAuditSlot].valid(),
						(uint32_t)v0.batchBuffers[m_filterAuditSlot].valid());
				}
			}
		}

		// Demand clear (download-before-clear: the transfer chain orders this
		// fill after the in-flight readback copy above, and this frame's filter
		// writes wait the chain - the cadence the retired fragment-feedback
		// ring proved; a stale min would pin textures sharp forever).
		if (gfxDemandActive)
			smvk::gfx::ClearDemand(gfx, *m_context, frameSlot);

		// 3) SDSM stats: OWN async ticket, submitted EVERY frame - not the
		// shared every-4th batch above: the split fit must track the camera
		// with ONE frame of latency (current-frame measurement, next-frame
		// fit). Poll first (non-blocking), then submit this frame's armed
		// stats. 1-in-flight + the 2-slot per-flight ring keep the pending
		// copy of slot N's buffer ahead of slot N+2's rewrite (the exposure
		// ring's guarantee).
		if (m_sdsmReadbackTicket != 0) {
			const smvk::BufferDownload sdsmDl{
				m_sdsmStatsBuffers[m_sdsmReadbackSlot % 2],
				m_pendingSdsmStats.data(), kSdsmStatsBytes, 0 };
			if (m_context->PollAsyncDownloadDataFromBuffers(
				m_sdsmReadbackTicket,
				std::span<const smvk::BufferDownload>(&sdsmDl, 1))) {
				m_sdsmReadbackTicket = 0;
				// Merge the 256 partial histograms + the true min/max, then
				// the 1%/99% percentile range (the histogram point: one tiny
				// distant object must not dictate the whole split range).
				float mn = 1e30f, mx = 0.0f;
				uint32_t bins[kSdsmBins] = {};
				uint64_t total = 0;
				const auto* groups = reinterpret_cast<const SdsmGroupStats*>(
					m_pendingSdsmStats.data());
				for (uint32_t g = 0; g < kSdsmGroups; ++g) {
					mn = std::min(mn, groups[g].minV);
					mx = std::max(mx, groups[g].maxV);
					for (uint32_t b = 0; b < kSdsmBins; ++b) {
						bins[b] += groups[g].bins[b];
						total += groups[g].bins[b];
					}
				}
				if (total > 0 && mx > mn) {
					// Percentile walk from both ends over the merged histogram.
					// Bin edges mirror the shader: log2(viewZ) over
					// [log2(zNear), log2(zFar)] -> 64 bins; the depth at the
					// bin that crosses the tail threshold is the bound.
					const float zNear = default_near_clip;
					const float zFar = std::max(CascadeFar(), zNear * 2.0f);
					const float logLo = std::log2(zNear);
					const float logHi = std::log2(zFar);
					const uint64_t tail = std::max<uint64_t>(1, total / 100);
					auto depthAtBin = [&](uint32_t b) {
						return std::pow(2.0f,
							logLo + (float(b) + 0.5f) / float(kSdsmBins) * (logHi - logLo));
					};
					uint64_t acc = 0;
					float pctMin = mn;
					for (uint32_t b = 0; b < kSdsmBins; ++b) {
						acc += bins[b];
						if (acc >= tail) { pctMin = depthAtBin(b); break; }
					}
					acc = 0;
					float pctMax = mx;
					for (uint32_t b = kSdsmBins; b-- > 0;) {
						acc += bins[b];
						if (acc >= tail) { pctMax = depthAtBin(b); break; }
					}
					if (pctMax > pctMin) {
						m_sdsmMin = mn;
						m_sdsmMax = mx;
						m_sdsmPctMin = pctMin;
						m_sdsmPctMax = pctMax;
						m_sdsmStatsValid = true;
						// The stats measured the frame that ran the stats
						// pass - one frame back at this cadence.
						m_sdsmMeasureFrame = m_frameCounter - 1;
					}
				}
			}
		}
		if (m_sdsmStatsArmed && m_sdsmReadbackTicket == 0
			&& !m_sdsmStatsBuffers[frameSlot % 2].empty()) {
			const smvk::BufferDownload sdsmDl{
				m_sdsmStatsBuffers[frameSlot % 2],
				m_pendingSdsmStats.data(), kSdsmStatsBytes, 0 };
			m_sdsmReadbackSlot = frameSlot;
			m_sdsmReadbackTicket =
				m_context->SubmitAsyncDownloadDataFromBuffers(
					std::span<const smvk::BufferDownload>(&sdsmDl, 1));
		}
	}

	// ---------------------------------------------------------------------
	// Named GI sidecars (Cluster::giBindings across the loaded clusters).
	// Model supports N bindings; exactly one per kind is honored for now
	// (first wins + warn). Empty outcome = registry-less pak: callers
	// fall back to the scene id. The names are STABLE bake stems (the
	// gi/<stem>.<ext> files GiBaker writes - like mesh blob paths); both
	// resolve through the same VFS overlay (pak or loose).
	// ---------------------------------------------------------------------
	void WorldRenderer::ResolveGiSidecars() {
		m_giProbeName.clear();
		m_giLightmapName.clear();
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
		// DYNAMIC cluster FIRST (runtime quick-bake bindings override the
		// cooked pak registry - same precedence as World::ReadBakeBytes),
		// then pak clusters in registration order.
		auto walkCluster = [&](const smvk::Cluster& cluster) {
			for (const smvk::GiBinding& b : cluster.giBindings) {
				if (b.name.empty())
					continue;
				switch (b.kind) {
				case smvk::GiBinding::Kind::Probe:
					takeFirst(m_giProbeName, b.name, "probe");
					break;
				case smvk::GiBinding::Kind::Lightmap:
					takeFirst(m_giLightmapName, b.name, "lightmap");
					break;
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
		};
		for (auto it = m_world->clusters.rbegin();
			it != m_world->clusters.rend(); ++it) {
			if (*it && (*it)->isDynamic)
				walkCluster(**it);
		}
		for (const auto& clusterPtr : m_world->clusters) {
			if (clusterPtr && !clusterPtr->isDynamic)
				walkCluster(*clusterPtr);
		}
		if (!m_giProbeName.empty() || !m_giLightmapName.empty()
			|| !m_giRegName.empty() || !m_giCvolName.empty()
			|| !m_giCubemapName.empty())
			std::cerr << "[GI] bake bindings: probe='"
			<< m_giProbeName << "' lightmap='" << m_giLightmapName
			<< "' reg='" << m_giRegName << "' cvol='" << m_giCvolName
			<< "' cubemap='" << m_giCubemapName << "'\n";
	}

	// ---------------------------------------------------------------------
	// Cooked scene lights (KHR_lights_punctual).
	//
	// Built once per scene load from the resolved world: the definitions
	// come back as Cluster pool assets (Light blobs in the pak, loaded
	// exactly like meshes - m_world->lightLocations resolves the world
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
		// Volumetric fog state (the App panel's settings) seeds from the
		// authored env ONCE per scene load/rebind (see SceneEnv - the
		// fog-quality volumetric tier; the panel stays live afterwards).
		// volumetrics is the master: 0 = off (the analytic tier or no
		// fog), else the scattering intensity; the vol* knobs load with
		// it. The runtime degrade (volumetric -> analytic when the pass
		// cannot run) lives at the frame-env build.
		if (m_world->scene.hasEnvironment) {
			const SceneEnv& se = m_world->scene.environment;
			PushVolumetricEnv(se);
			// Postfx look seed (bloom/tonemap/exposure/AO/SSR/motion
			// blur/DOF/AA/cubemap - the App panels' settings, authored in
			// the env): once per scene load/rebind, panels/keys live on.
			PushPostfxEnv(se);
			// Headless overrides win over the scene seed (AGENTS.md:
			// "scene settings lose") - without this a headless pin dies
			// here on any scene whose env disagrees (the seed writes the
			// members unconditionally).
			ApplyHeadlessPostfxOverrides();
			// --bench determinism pin (see m_autoExposurePin): the seed
			// above re-seeds m_autoExposureEnabled from the authored env
			// (default ON), silently re-enabling live eye adaptation
			// mid-bench; the pin re-applies here, after the seed AND the
			// headless overrides (a VULKANTOY_AUTO_EXPOSURE snapshot pin
			// stays the more deliberate headless choice and wins). Lives
			// here - not in ApplyHeadlessPostfxOverrides - so the bench
			// contract doesn't entangle with the headless-pin migration
			// happening in that function. X/UI stay live afterwards (this
			// runs only at load/rebind; a bench never rebinds).
			if (m_autoExposurePin >= 0)
				m_autoExposureEnabled = (m_autoExposurePin != 0);
			// Re-seed the eye adaptation from the FINAL exposure state
			// (seed + overrides above): without this a new scene rendered
			// its first frames at the PREVIOUS scene's adapted exposure
			// and converged from there - a world swap from bright to dark
			// (or vice versa) flashed the wrong look for a good second.
			// The authored manual exposure is the scene's own "correct"
			// starting point; adaptation takes over from the next poll.
			m_exposureAdapted = std::clamp(m_manualExposure,
				m_exposureMin, m_exposureMax);
			m_exposureTarget = m_exposureAdapted;
			m_exposureAdaptDt = 0.0f;
			std::cerr << "[Scene] postfx: bloom="
				<< (m_bloomEnabled ? "on" : "off")
				<< " int=" << m_bloomIntensity
				<< " thr=" << m_bloomThreshold
				<< " tonemap=" << m_tonemapMode
				<< " exposure=" << (m_autoExposureEnabled ? "auto"
					: "manual")
				<< " ao=" << int(m_aoMode)
				<< " ssr=" << (m_ssrEnabled ? "on" : "off")
				<< " aa=" << int(m_aaMode) << "\n";
			// Headless overrides win over the scene seed (AGENTS.md:
			// "scene fog settings lose") - without this a headless
			// VULKANTOY_VOLUMETRIC=1 force-on dies here on any
			// non-volumetric scene (the seed writes enabled=false).
			ApplyHeadlessVolumetricOverrides();
			const bool volOn = se.fogQuality == 2 && se.volumetrics > 0.0f;
			std::cerr << "[Scene] fog: quality="
				<< (se.fogQuality == 0 ? "off"
					: se.fogQuality == 1 ? "analytic (exp2)"
					: "volumetric (raymarched)")
				<< (volOn ? " on" : (se.fogQuality == 2
					? " (volumetrics=0: analytic fallback)" : ""))
				<< "\n";
		}
		PackCookedLightsFromWorld(true);
		// Seed the SCENE's sun from the authored light so the sky disc, IBL
		// tint and shadow-casting light stay in sync (sun fields only - the
		// env block is lux-native, so the light's lux copy straight in,
		// like the editor's cooked-sun seed). Runs only when the scene
		// carries no authored look (old content without env blocks); a
		// tour/cooked env always wins. The user can still retouch the look
		// afterwards (this runs once at load/rebind).
		if (m_cookedLights.hasSun && m_world && !m_world->scene.hasEnvironment) {
			const glm::vec3 t = glm::normalize(m_cookedLights.sunDirToSun);
			SceneEnv& sceneEnv = m_world->scene.defaultEnvironment;
			sceneEnv.sunElevation = glm::degrees(std::asin(
				std::clamp(t.y, -1.0f, 1.0f)));
			sceneEnv.sunAzimuth = glm::degrees(std::atan2(t.x, t.z));
			sceneEnv.sunColor = m_cookedLights.sunColor;
			sceneEnv.sunIntensity = m_cookedLights.sunIntensity;
			m_world->scene.environment = sceneEnv;
			m_world->scene.hasEnvironment = true;
			std::cerr << "[Scene] cooked sun: az=" << sceneEnv.sunAzimuth
				<< " el=" << sceneEnv.sunElevation
				<< " I=" << sceneEnv.sunIntensity << "\n";
		}
	}

	// Light pack shared by the bind-time build (log there) and the
	// per-edit refresh (pack only - no env writes, no world mutation,
	// no log). Reads the LIVE world, so edits apply.
	void WorldRenderer::PackCookedLightsFromWorld(bool log) {
		m_cookedLights.clear();
		const ::Scene& scene = m_world->scene;
		const auto& lightLocs = m_world->lightLocations;
		if (lightLocs.empty() || scene.lightForNode.empty()) {
			if (log)
				std::cerr << "[Scene] no cooked lights (Forward+ set empty)\n";
			return;
		}
		// Deterministic node order (pak order is already sorted, but the
		// world map is a hash - sort the node ids).
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
		// conjugated transform aims 180 degrees off - the Bistro sun
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
				pl.cookieIndex = -1; // points never project a gobo
				// Authored light function (flicker/pulse; the shader's bands
				// form degrades to pulse for points - see EvaluateLightFunction).
				pl.functionType = int32_t(L.functionType);
				pl.functionSpeed = L.functionSpeed;
				pl.functionScale = L.functionScale;
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
				// Authored spot COOKIE (gobo): the .texture asset path (or
				// image URI) resolves through the world's cluster texture
				// pool to the bindless gobo index the shader projects
				// through the cone (see default_lit_body's cookie eval).
				// Unresolved refs warn + drop (never fatal) - the App's
				// pak must carry the texture (the tour's textureAsset
				// records import it).
				sl.cookieIndex = -1;
				if (!L.cookieUri.empty() && m_context) {
					for (const auto& cp : m_world->clusters) {
						if (!cp)
							continue;
						if (auto hit =
							cp->FindTextureByUri(L.cookieUri)) {
							sl.cookieIndex = int32_t(
								m_context->GetTextureBindlessIndex(*hit));
							break;
						}
					}
					if (sl.cookieIndex >= 0)
						m_cookieWarnedUris.erase(L.cookieUri);
					else if (m_cookieWarnedUris.insert(
						L.cookieUri).second)
						std::cerr << "[Scene] spot '" << L.name
						<< "': cookie '" << L.cookieUri
						<< "' unresolved (no cluster texture) - "
						"unshadowed gobo dropped\n";
				}
				// Authored light function (flicker/pulse/bands).
				sl.functionType = int32_t(L.functionType);
				sl.functionSpeed = L.functionSpeed;
				sl.functionScale = L.functionScale;
				spots.push_back(sl);
			}
		}
		// Spots first (per-tile slot priority): the tile lists keep only the
		// first FP_MAX_PER_TILE passing lights, so cone lights must not
		// compete with the point crowd for those slots.
		m_cookedLights.pointSpots.reserve(spots.size() + points.size());
		m_cookedLights.pointSpots.insert(
			m_cookedLights.pointSpots.end(), spots.begin(), spots.end());
		m_cookedLights.pointSpots.insert(
			m_cookedLights.pointSpots.end(), points.begin(), points.end());
		if (m_cookedLights.pointSpots.size() > FP_MAX_LIGHTS) {
			if (log)
				std::cerr << "[Scene] cooked lights clamped: "
				<< m_cookedLights.pointSpots.size() << " -> " << FP_MAX_LIGHTS << "\n";
			m_cookedLights.pointSpots.resize(FP_MAX_LIGHTS);
		}
		if (log)
			std::cerr << "[Scene] cooked lights: spots=" << spots.size()
			<< " points=" << points.size()
			<< (m_cookedLights.hasSun ? " + sun" : " (no sun)")
			<< (staticBakeOnly > 0
				? " | static(bake-only)=" + std::to_string(staticBakeOnly)
				: "")
			<< "\n";
	}

	// ---------------------------------------------------------------------
	// Forward+ light upload.
	//
	// The light set is the scene's cooked authored set (BuildCookedLights
	// FromWorld): static placement, no orbit/bob - cookies + light
	// functions ride the authored fields. A CPU copy is kept in
	// m_debugLights for the ImGui light-point overlay. Spot-shadow
	// selection runs here: spots carrying the cast-shadow request flag
	// are ranked by the screenspace/distance heuristic and the top-N
	// (upper limit m_spotShadowMax) get atlas tiles; their shadowTile /
	// shadowMapIndex are patched before upload. Results land in
	// m_spotShadowCount / m_spotShadowMapIndex for the frame's passes.
	// ---------------------------------------------------------------------
	uint32_t WorldRenderer::UpdateForwardPlusLights(uint32_t currentFrame, const FrameUniforms& frame) {
		using fp_clk = std::chrono::steady_clock;
		const bool fpBench = m_host.BenchMeasuring();
		const auto fpT0 = fp_clk::now();
		std::vector<smvk::ForwardPlusLight> lights;
		lights.reserve(FP_MAX_LIGHTS);
		m_debugLights.clear();
		for (const auto& cl : m_cookedLights.pointSpots) {
			if (lights.size() >= FP_MAX_BUILD_LIGHTS)
				break;
			lights.push_back(cl);
			m_debugLights.push_back({ glm::vec3(cl.position),
				glm::vec3(cl.color), (int)cl.type });
		}

		// ---- Spot-shadow caster selection (upper limit + heuristic) ----
		m_spotShadowCount = 0;
		m_spotShadowMapIndex = -1;
		m_spotShadowTiles.clear();
		if (m_passes.spotShadows && m_spotShadowsEnabled && m_viewportResources.spotShadows.Valid() && !lights.empty()) {
			const uint32_t wantMax = (uint32_t)std::clamp(m_spotShadowMax, 0, (int)smvk::kMaxSpotShadows);
			const uint32_t budget = std::min(wantMax, m_viewportResources.spotShadows.slotCount);
			if (budget > 0) {
				const float proj11 = std::abs(frame.proj[1][1]);
				auto selected = smvk::SelectSpotShadowCasters(
					lights, frame.viewProj, frame.cameraPos,
					m_spotShadowMaxDistance, budget, proj11);
				const int32_t atlasIdx = m_viewportResources.spotShadows.BindlessIndex(*m_context);
				// EVSM: lights address the blurred moments (1-frame lag);
				// tile descriptors are layout-identical (same 8x1024 grid).
				// NOTE: the tile lightVP below is built from the CURRENT
				// light state while the blurred spot atlas trails one slot
				// under OneFrameLag - fine for static spots, but ANIMATED
				// spots would need prev-frame tile descriptors like
				// m_dirSamplePrev does for the sun. SameFrame (default)
				// has no lag by construction.
				const int32_t evsmIdx = (EvsmActive())
					? m_viewportResources.evsmSpot.BlurredBindlessIndex(*m_context, currentFrame)
					: -1;
				const int32_t sampleIdx = (evsmIdx >= 0) ? evsmIdx : atlasIdx;
				if (atlasIdx >= 0 && !selected.empty()) {
					std::vector<smvk::GpuCascadeDescription> tiles(selected.size());
					for (size_t k = 0; k < selected.size(); ++k) {
						smvk::ForwardPlusLight& sl = lights[selected[k].lightIndex];
						if (m_spotLightLiSPSM) {
							tiles[k] = m_viewportResources.spotShadows.BuildSpotTileLiSPSM(
								glm::vec3(sl.position[0], sl.position[1], sl.position[2]),
								glm::normalize(glm::vec3(sl.direction[0], sl.direction[1], sl.direction[2])),
								sl.outerAngle, sl.range, atlasIdx, (uint32_t)k,
								frame.view, frame.proj,
								frame.nearPlane, frame.farPlane,
								SPOT_SHADOW_NEAR, SPOT_SHADOW_BIAS,
								// Soft spot shadows: emitting-sphere diameter +
								// artistic softness (tile pads, see evsm.hlsli).
								m_evsmSpotLightSize, m_evsmSoftness);
						}
						else {
							tiles[k] = m_viewportResources.spotShadows.BuildSpotTile(
								glm::vec3(sl.position[0], sl.position[1], sl.position[2]),
								glm::normalize(glm::vec3(sl.direction[0], sl.direction[1], sl.direction[2])),
								sl.outerAngle, sl.range, atlasIdx, (uint32_t)k,
								SPOT_SHADOW_NEAR, SPOT_SHADOW_BIAS,
								// Soft spot shadows: emitting-sphere diameter +
								// artistic softness (tile pads, see evsm.hlsli).
								m_evsmSpotLightSize, m_evsmSoftness);
						}
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
		// are always freshly zeroed - the phases then emit empty lists and
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
	// forwardplus.cs.hlsl / default_lit_body.hlsli). The WORLD carries the
	// content (SceneDecal pool entities; the shape rides the node
	// transform); this re-derives the GPU list every frame, so
	// dynamically-created decals appear the same frame.
	// ---------------------------------------------------------------------

	uint32_t WorldRenderer::UpdateDecals(uint32_t currentFrame) {
		m_decalFrameCount = 0;
		if (!DecalsActive())
			return 0;

		// Texture URI -> bindless cache for this frame (decal counts are
		// small; the cache avoids re-walking clusters per decal when they
		// share looks). Resolution mirrors the light-cookie path: cluster
		// pool first match wins, unresolved refs warn once + skip.
		std::unordered_map<std::string, int32_t> bindlessByUri;
		auto resolveTex = [&](const std::string& uri) -> int32_t {
			if (uri.empty())
				return -1;
			auto hit = bindlessByUri.find(uri);
			if (hit != bindlessByUri.end())
				return hit->second;
			int32_t bindless = -1;
			for (const auto& cp : m_world->clusters) {
				if (!cp)
					continue;
				if (auto th = cp->FindTextureByUri(uri)) {
					bindless = int32_t(m_context->GetTextureBindlessIndex(*th));
					break;
				}
			}
			if (bindless < 0 && m_decalTexWarnedUris.insert(uri).second)
				std::cerr << "[Decals] texture '" << uri
				<< "' unresolved (no cluster texture) - decal skipped\n";
			bindlessByUri[uri] = bindless;
			return bindless;
		};

		// Node-ordered walk (deterministic frame to frame; unordered_map
		// iteration is not) capped at FP_MAX_DECALS.
		std::vector<std::pair<NodeId, uint32_t>> decalPairs(
			m_world->scene.decalForNode.begin(),
			m_world->scene.decalForNode.end());
		std::sort(decalPairs.begin(), decalPairs.end());

		std::vector<smvk::ForwardPlusDecal> gpu;
		gpu.reserve(std::min<size_t>(decalPairs.size(), FP_MAX_DECALS));
		for (const auto& [node, worldIdx] : decalPairs) {
			if (gpu.size() >= FP_MAX_DECALS)
				break;
			if (node >= m_world->scene.globalTransform.size()
				|| worldIdx >= m_world->decalLocations.size())
				continue;
			const auto& loc = m_world->decalLocations[worldIdx];
			if (!loc.cluster)
				continue;
			const ::SceneDecal* d = loc.cluster->decals.get(loc.handle);
			if (!d)
				continue;

			// Shape from the node transform: translation = projector
			// center; rotation columns = the projection frame (local -Z
			// INTO the surface, +Y texture V up); scale = half extents
			// (x, y in-plane, z depth reach) - see SceneDecal.
			const glm::mat4& g = m_world->scene.globalTransform[node];
			const glm::vec3 center = glm::vec3(g[3]);
			const glm::vec3 colX(g[0]), colY(g[1]), colZ(g[2]);
			const float halfX = glm::length(colX);
			const float halfY = glm::length(colY);
			const float halfZ = glm::length(colZ);
			if (halfX <= 1e-5f || halfY <= 1e-5f || halfZ <= 1e-5f)
				continue; // degenerate frame (zero-scaled axis)
			const glm::vec3 axisZ = -colZ / halfZ; // local -Z, into the surface
			const glm::vec3 upHint = colY / halfY;

			// Channels: only the flags the textures resolved for.
			const int32_t albedoTex = resolveTex(d->albedoUri);
			const int32_t normalTex = resolveTex(d->normalUri);
			if (albedoTex < 0 && normalTex < 0)
				continue; // no look at all - the warn-once above covered it
			int32_t flags = 0;
			if (albedoTex >= 0)
				flags |= smvk::kDecalFlag_Albedo;
			if (normalTex >= 0)
				flags |= smvk::kDecalFlag_Normal;
			flags |= smvk::kDecalFlag_Material;

			smvk::gi::Pcg32 yawRng(d->seed ? d->seed : 1u);
			const float yaw = float(yawRng.Next()) * (6.2831853f / 4294967296.0f);
			gpu.push_back(smvk::MakeDecal(center, axisZ, upHint,
				glm::vec3(halfX, halfY, halfZ), albedoTex, normalTex, flags,
				glm::clamp(d->opacity, 0.0f, 1.0f), d->tint, 0.0f,
				glm::vec3(0.0f), d->normalStrength, d->roughnessTarget,
				d->metalTarget, d->depthFadeStart, yaw));
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
		// Dust motes: slow, faint, wide sphere drift - reads best inside
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
		if (m_decalPickState == 0) {
			std::cerr << "[Pick] no CPU pick source (cooked worlds upload "
				"straight to the GPU) - pick BVH off\n";
			m_decalPickState = -1;
		}
		return m_decalPickState > 0 && m_decalPickScene
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
		// Splat size range for spawned decals (half extents, meters).
		constexpr float kSpawnSizeMin = 0.28f;
		constexpr float kSpawnSizeMax = 0.55f;
		const float half = kSpawnSizeMin
			+ (kSpawnSizeMax - kSpawnSizeMin) * t;

		// Entity frame (see SceneDecal + DecalNodeFrame): local -Z is
		// the projection axis INTO the surface, +Y the texture V hint,
		// scale the half extents; the box hovers 0.3 m off the wall so
		// the depth reach straddles it.
		const glm::vec3 center = hitPos + n * 0.30f;
		const glm::mat4 frame = ::DecalNodeFrame(center, -n, up,
			glm::vec3(half, half, 0.85f));

		::SceneDecal def{};
		def.albedoUri = std::string(::kDecalBloodAlbedoUri);
		def.normalUri = std::string(::kDecalBloodNormalUri);
		def.opacity = 0.75f + 0.25f * rng.Float();
		def.seed = m_decalSpawnSeed++;
		def.name = "blood_splat_" + std::to_string(def.seed);
		if (m_world->CreateDecalAt(frame, std::move(def)) == UINT32_MAX)
			std::cerr << "[Decals] crosshair spawn failed (world refused)\n";
	}

	// ---------------------------------------------------------------------
	// Deferred shadows, phases 2-4 (GPU). Appended right where the legacy
	// forward+ tile cull would run - i.e. over the complete depth buffer
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

		// Phase 2 - Find Lights Approximate (one TileGroup per work unit).
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
		// Phase 3 - Find Lights Accurate (one Shadow Tile per work unit).
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
		// Phase 4 - Generate Deferred Shadows (samples the spot atlas).
		{
			auto& pass = ds.AddPhase4Pass(*m_rendergraph, depthView,
				fpLightsLogical, m_resources.spotShadowLogical,
				m_spotShadowCount > 0 ? m_viewportResources.spotShadows.logicalView
				: smvk::LogicalImageViewHandle{},
				frameIndex);
			// EVSM SameFrame: phase 4 samples the blurred moments via
			// bindless (not the PCF view hinted above) - the hint is the
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
		// (No local counters: the World's per-frame decision stats count
		// every leg's query once - see World::LodIndexForNode.)
		if (lod > 0) {
			if (smvk::StaticMesh* m = m_world->GetLodMesh(worldMesh, lod))
				return m;
		}
		return base;
	}

	// Shader-LOD decision for an instance's selected level + material
	// (see World::FarShadingAt): vertex-lit substitution, specialized
	// lit, or default full-lit. Shares the halfrate LOD cache with every
	// geometry consumer, so shading and geometry always resolve off the
	// SAME level within a frame.
	smvk::World::FarShading WorldRenderer::FarShadingForNode(NodeId node,
		uint32_t worldMesh, uint32_t material) const {
		if (!ShaderLodActive() || !m_world)
			return smvk::World::FarShading{};
		const uint32_t lod = LodIndexForNode(node, worldMesh);
		return m_world->FarShadingAt(worldMesh, lod, material);
	}

	// Specialized lit twins for a far-level quality (see the m_farSpec-
	// Programs comment in WorldRenderer.h): the base twin's modules with
	// the quality spec payload attached to the fragment stage. Only
	// non-default quality values get entries (the shader folds the
	// defaults, so a default entry would duplicate the base program).
	WorldRenderer::FarSpecPrograms* WorldRenderer::FarSpecProgramsFor(
		const smvk::ShaderLodQuality& quality) {
		const uint32_t key = (quality.specular ? 1u : 0u)
			| (quality.normalMap ? 2u : 0u);
		auto hit = m_farSpecPrograms.find(key);
		if (hit != m_farSpecPrograms.end())
			return &hit->second;
		FarSpecPrograms out{};
		auto spec = std::make_shared<smvk::ShaderSpecialization>();
		if (!quality.specular)
			spec->SetBool(smvk::kSpecLitSpecular, false);
		if (!quality.normalMap)
			spec->SetBool(smvk::kSpecLitNormalMap, false);
		auto specialize = [&](const smvk::ShaderProgram& base) {
			smvk::ShaderProgram p = base;
			for (size_t i = 0; i < p.shaderCount; ++i)
				if (p.shaders[i].stage == smvk::ShaderStage::Fragment)
					p.shaders[i].specialization = spec;
			return p;
		};
		if (m_resources.mainProgram.shaderCount == 2)
			out.main = specialize(m_resources.mainProgram);
		if (m_resources.mainProgramA2C.shaderCount == 2)
			out.a2c = specialize(m_resources.mainProgramA2C);
		std::cerr << "[ShaderLOD] built specialized far program (specular="
			<< (quality.specular ? 1 : 0) << ", normalMap="
			<< (quality.normalMap ? 1 : 0) << ")\n";
		m_farSpecPrograms[key] = out;
		return &m_farSpecPrograms[key];
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
		smvk::ShaderProgram& opaqueProgram, const IRenderer::PushConstants& pc,
		uint32_t casterViewIndex, bool skipFiltered,
		const smvk::MSAASettings& msaa) {
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
				msaa,
				smvk::RasterSettings{ .cullMode = cull }
			);
			cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &pc, smvk::ShaderStage::All);
			boundShadowMaskedCull = cull;
		};		auto drawCaster = [&](const smvk::World::RenderItem& item, bool maskedPhase) {
			if (item.material == smvk::World::kInvalidIndex)
				return;
			// Indirect path: registered casters are covered by the per-view
			// command list - only unfilterable meshes rasterize here.
			if (skipFiltered
				&& m_gfxRenderer.GfxMeshForLod(item.mesh,
					LodIndexForNode(item.node, item.mesh)) != UINT32_MAX)
				return;
			// Dither-on-closeup (Scene::ditherCloseup; per frame, per draw -
			// the MAIN camera is the anchor eye even in light-view passes,
			// the fade follows what the camera sees): Hidden casters drop
			// out (a fully faded subtree casts nothing); Fade casters join
			// the MASKED phase so the alpha-tested shadow FS screen-doors
			// the shadow with the lit view's fade (the per-draw flag rides
			// PushConstants::padEnd_[0] -> shadow.vert forwards it flat).
			const DitherCloseupState ditherState =
				DitherCloseupStateFor(m_world->scene, int(item.node),
					m_cameraPositioner.cameraPosition_);
			if (ditherState == DitherCloseupState::Hidden)
				return;
			const bool ditherFade =
				ditherState == DitherCloseupState::Fade;
			// Masked (cutout) flag replaces the per-node material fetch the
			// phase split used to pay.
			if (((item.flags & smvk::IteratorFlagBit_Masked) != 0
				|| ditherFade) != maskedPhase)
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
			// Shadow contribution cull: skip casters outside the MAIN camera's
			// view frustum. Only geometry that could contribute to the final
			// image (i.e., is visible from the camera) is rendered into the
			// shadow map. This is enabled by m_shadowContributionCulling and
			// only applied to shadow views (casterViewIndex != kInvalidIndex).
			if (m_shadowContributionCulling && casterViewIndex != smvk::RenderViewSet::kInvalidIndex) {
				glm::vec3 sphereCenter;
				float sphereRadius = 0.0f;
				smvk::MeshBoundsToWorld(
					model, meshDesc->geometry.aabbMin, meshDesc->geometry.aabbMax,
					sphereCenter, sphereRadius);
				if (!smvk::RenderViewIntersectsSphere(
					m_renderViews.Get(0), sphereCenter, sphereRadius))
					return;
			}
			IRenderer::PushConstants objectPC{};
			std::copy_n(glm::value_ptr(model), 16, objectPC.model);
			// Per-draw dither-on-closeup flag (padEnd_[0]; always pushed - the
			// partial push range persists across draws, so a stale Fade flag
			// must never leak into the next caster's shadow depth).
			objectPC.padEnd_[0] = ditherFade ? 1u : 0u;
			cmd.PushConstants(offsetof(IRenderer::PushConstants, padEnd_),
				sizeof(objectPC.padEnd_[0]), &objectPC.padEnd_[0],
				smvk::ShaderStage::All);
			if (maskedPhase) {
				objectPC.materialId = item.material;
				cmd.PushConstants(offsetof(IRenderer::PushConstants, materialId),
					sizeof(objectPC.materialId), &objectPC.materialId,
					smvk::ShaderStage::All);
			}
			cmd.PushConstants(offsetof(IRenderer::PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
			for (auto& draw_set : meshDesc->geometry.surfaces) {
				cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
			};
		};
		// Phase 1: solid casters, depth-only program. FRONT-face culling
		// (render the back faces only): the classic shadow-map anti-acne -
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
			msaa,
			smvk::RasterSettings{
				.cullMode = smvk::CullMode::Front
			}
		);
		cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &pc, smvk::ShaderStage::All);
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

	void WorldRenderer::ResetLodStats() {
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

	// Text-only stats HUD (top-left overlay, Ctrl+T): framerate, GPU,
	// camera, filter culls, LODs, exposure, streaming. Reads the same
	// members as the panel header + Stats tab rows it replaces (which
	// keep controls, explanations and detail breakdowns). Text only: no
	// bars, plots or widgets (ImGui::Text + one window chrome bit).
	// The window takes no input (clicks pass through to the scene) and
	// never saves settings.
	// Texture-viewer selection body (the shared click / arrow-nav path -
	// see the viewer draw site). A plain 2D gets ONE user-texture binding;
	// a cubemap gets SIX per-face View2D bindings (+X -X +Y -Y +Z -Z - the
	// ImGui backend samples Texture2D only, so the cube's own default
	// ViewCube cannot serve as the preview; each face = one array layer
	// of the cube image). Everything the previous selection owned drops
	// first (one descriptor per Add, always paired; face-view Holders
	// reset -> their VkImageViews retire through the RMI destroy).
	void WorldRenderer::SelectTexDebugTexture(const smvk::TextureInfo& t) {
		DropTexDebugSelection();
		m_texDebugSelected = t.handle;
		const bool plain2d = t.type == smvk::ImageType::Image2D
			&& !t.isCubemap && t.arrayLayers == 1;
		// Cubemap (or 6*N cube array) preview: per-face 2D views.
		// Cube arrays preview cube 0 (faces 0..5).
		const bool cube = t.isCubemap
			&& smvk::IsCubemapLayerCount(t.arrayLayers);
		if (plain2d && t.bindless >= 0) {
			// The overlay's user-texture binding: the backend's descriptor
			// registration wrapped behind RHI-opaque types (a dead handle
			// returns 0 there - the preview simply doesn't appear).
			m_texDebugPreview = m_imgui->AddUserTexture(
				t.handle, m_texDebugSampler);
		}
		else if (cube) {
			// A dead view (destroyed, generation bumped) returns 0 - that
			// face's preview simply doesn't appear.
			static const char* const kFaceNames[6] = {
				"+X", "-X", "+Y", "-Y", "+Z", "-Z"
			};
			const smvk::ImageHandle img =
				m_context->GetTextureImageHandle(t.handle);
			for (uint32_t face = 0; face < 6 && !img.empty(); ++face) {
				smvk::ImageViewCreateInfo vci{};
				vci.viewType = smvk::ImageViewType::View2D;
				vci.format = smvk::Format::Undefined; // inherit
				vci.subresourceRange = {
					smvk::ImageAspect::Color, 0,
					smvk::RemainingSubresources,
					face, 1
				};
				char name[48];
				std::snprintf(name, sizeof(name),
					"texviewer face %s", kFaceNames[face]);
				vci.name = name; // consumed at creation (debug names only)
				m_texDebugCubeViews[face] =
					m_context->CreateImageView(img, vci);
				if (!m_texDebugCubeViews[face].empty())
					m_texDebugCubePreview[face] =
						m_imgui->AddUserTextureView(
							m_texDebugCubeViews[face], m_texDebugSampler);
			}
		}
	}

	void WorldRenderer::DrawHudOverlay() {
		if (!m_showHud)
			return;
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.55f);
		if (!ImGui::Begin("##vulkantoy_hud", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
				| ImGuiWindowFlags_NoSavedSettings
				| ImGuiWindowFlags_NoFocusOnAppearing
				| ImGuiWindowFlags_NoInputs)) {
			ImGui::End();
			return;
		}
		// Frame rate + CPU/GPU times (same sources as the old panel header).
		const float fps = ImGui::GetIO().Framerate;
		if (m_gpuTimingValid)
			ImGui::Text("%.0f FPS (%.1f ms) CPU %.2f GPU %.2f",
				(double)fps, fps > 1e-6f ? 1000.0f / fps : 0.0,
				m_cpuMsSmooth, m_gpuMsSmooth);
		else
			ImGui::Text("%.0f FPS (%.1f ms) CPU %.2f GPU ... (warming up)",
				(double)fps, fps > 1e-6f ? 1000.0f / fps : 0.0,
				m_cpuMsSmooth);
		// GPU memory (VMA budgets; throttled to 1 Hz inside). DEVICE_LOCAL
		// (VRAM) heaps ONLY: summing every heap double-counts host budgets -
		// a 16 GiB card would read ~30 GiB "total". Fallback: if no heap
		// reports deviceLocal (integrated/uniform-memory GPUs), sum all.
		RefreshGpuMemoryStats(false);
		if (!m_gpuMemHeaps.empty()) {
			uint64_t totalUsed = 0, totalBudget = 0;
			bool anyDeviceLocal = false;
			for (const auto& h : m_gpuMemHeaps) {
				if (!h.deviceLocal)
					continue;
				anyDeviceLocal = true;
				totalUsed += h.usageBytes;
				totalBudget += h.budgetBytes;
			}
			if (!anyDeviceLocal) {
				for (const auto& h : m_gpuMemHeaps) {
					totalUsed += h.usageBytes;
					totalBudget += h.budgetBytes;
				}
			}
			const double usedMiB = (double)totalUsed / 1048576.0;
			const double budgetMiB = (double)totalBudget / 1048576.0;
			ImGui::Text("%s %.0f/%.0f MiB (%.0f%%)", anyDeviceLocal ? "VRAM" : "GPU mem",
				usedMiB, budgetMiB,
				totalBudget > 0 ? 100.0 * usedMiB / budgetMiB : 0.0);
		}
		else {
			ImGui::TextDisabled("GPU mem n/a");
		}
		// Camera pose (same sources as the Stats tab camera rows).
		{
			const glm::vec3& p = m_cameraPositioner.cameraPosition_;
			const glm::vec3 fwd = m_cameraPositioner.getForward();
			ImGui::Text("cam %.2f %.2f %.2f", p.x, p.y, p.z);
			ImGui::Text("fwd %.3f %.3f %.3f", fwd.x, fwd.y, fwd.z);
		}
		// Filter culls (last filtered view; zeros when inactive). GPU mode
		// (the default with VK_EXT_mesh_shader): the cull runs in COMPUTE
		// (gfxbatchcull) - the CPU stat only counts candidates, so the kept
		// number lands via the async counter-word readback (~a few frames
		// stale) and culled = candidates - kept. CPU mode reads the CPU
		// cull counters directly.
		{
			const smvk::gfx::Stats fs = m_gfxRenderer.LastStats();
			const bool gpuKept = m_gfxKeptValid
				&& m_gfxRenderer.State().gpuCull;
			uint32_t kept = fs.clustersProcessed - fs.clustersCulled;
			if (gpuKept)
				kept = std::min(m_gfxKeptBatches, fs.clustersProcessed);
			// 6 numeric + 1 string specs = 7 args EXACTLY (a missing arg
			// reads stack garbage as the %s pointer - an AV in the CRT's
			// strlen; this exact bug crashed every frame-0 run).
			ImGui::Text("filter draws %u cmd %u tris %u | cl %u kept %u culled %u%s",
				fs.draws, fs.commands, fs.trianglesProcessed,
				fs.clustersProcessed, kept, fs.clustersProcessed - kept,
				gpuKept ? " (GPU)" : "");
		}
		// LOD decisions this frame (World-level stats: every leg - filter,
		// prepass, shadow, direct, motion - counts once at its first query;
		// far = far-shading draws counted at the Main direct leg).
		ImGui::Text("LOD %u draws (%u sw, %llu far)",
			m_world ? m_world->LodStatDraws() : 0u,
			m_world ? m_world->LodStatSwitched() : 0u,
			(unsigned long long)m_lodShaderDraws);
		// Exposure (applied + target).
		ImGui::Text("exp %.5f (tgt %.5f)",
			(double)FrameExposure(), (double)m_exposureTarget);
		// Texture streaming residency.
		if (m_textureStreamer) {
			const smvk::TextureStreamingStats tstats = m_textureStreamer->GetStats();
			const double resMiB = (double)tstats.residentBytes / 1048576.0;
			const double fullMiB = (double)tstats.fullBytes / 1048576.0;
			ImGui::Text("stream %u tex %.0f/%.0f MiB (%.0f%%) +%u -%u",
				tstats.textureCount, resMiB, fullMiB,
				fullMiB > 0.0 ? 100.0 * (1.0 - resMiB / fullMiB) : 0.0,
				tstats.upgradesLastUpdate, tstats.downgradesLastUpdate);
		}
		else {
			ImGui::TextDisabled("stream off (full-resident)");
		}
		ImGui::End();
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
		m_viewportResources.ssgi.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsgiDesc());
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

	// ------------------------------------------------------------------
	// LAYER / ELEMENT driver (nvpro-style, see Engine/App/Element.h). Sync
	// stays outside: the host acquires fb, drives the stack, presents.
	// ------------------------------------------------------------------
	void WorldRenderer::OnSwapchainRecreated() {
		// The host recreated ITS swapchain (the only sync-legal place to
		// do so — the ELEMENT forwards its onResize here): rebuild our
		// swap views + invalidate temporal histories. The frame path
		// never recreates.
		if (!m_swapchain)
			return;
		RebuildSwapViews();
		m_swapchain->ClearFramebufferResized();
	}
	void WorldRenderer::Tick(float dt) {
		// Element clock latch (the element's onUpdate calls this BEFORE
		// its control pass): camera integration + exposure adaptation key
		// on m_deltaTime; the shader time rides m_timeSinceStart.
		m_deltaTime = dt;
		m_timeSinceStart += dt;
	}
	void WorldRenderer::RenderLayerInto(const ElementFrame& frame) {
		if (!frame.fb)
			return;
		// The host owns the rect per frame (Full by default); adopt it so
		// SetLayerRect (non-stack hosts) and stack hosts share one path.
		m_layerRect = ClampLayerRect(frame.layer, FrameSwapExtent(*frame.fb));
		// Restore the Full default when the frame didn't carry a sub-rect so
		// a stale rect can't pin later full-swap frames.
		if (frame.layer.full)
			m_layerRect = LayerRect::Full();

		// Frame-time histogram hook
		smvk::profiler::RecordFrameTime(double(m_deltaTime) * 1000.0);
		// Busy-only CPU clock: the record span below. The host's acquire
		// idle and the present throttle are reported as wait instead (the
		// host publishes its acquire wait; the present-wait query returns
		// the last RETIRED frame's value here - the EMA absorbs the
		// one-frame lag).
		const auto busyStart = std::chrono::steady_clock::now();
		RenderFrame(*frame.fb);
		const auto frameEnd = std::chrono::steady_clock::now();
		const double presentWaitMs =
			m_swapchain ? m_swapchain->GetPresentWaitMs() : 0.0;
		const double waitMs = m_host.GetLastAcquireWaitMs() + presentWaitMs;
		double cpuMs = std::chrono::duration<double, std::milli>(
			frameEnd - busyStart).count() - presentWaitMs;
		if (cpuMs < 0.0)
			cpuMs = 0.0;
		constexpr double kEmaAlpha = 0.08;
		m_cpuMsSmooth += (cpuMs - m_cpuMsSmooth) * kEmaAlpha;
		m_waitMsSmooth += (waitMs - m_waitMsSmooth) * kEmaAlpha;
		m_gpuTimingValid = m_swapchain && m_swapchain->HasGpuTiming();
		if (m_gpuTimingValid)
			m_gpuMsSmooth += (m_swapchain->GetGpuFrameMs() - m_gpuMsSmooth) * kEmaAlpha;
		// GPU stage timings (shadow-path instrumentation, both PCF + EVSM -
		// see GpuStageTimer): last retired frame's per-stage ms (1-frame
		// lag), EMA copies for the UI, and --bench accumulation for the
		// [perf] split (a stage that never ran stays invalid, not zero).
		m_stageGpuTimingsSupported = m_swapchain
			&& m_swapchain->GetGpuStageTimings(
				m_stageGpuMs, m_stageGpuValid, smvk::kGpuStageCount);
		if (m_stageGpuTimingsSupported) {
			for (uint32_t s = 0; s < smvk::kGpuStageCount; ++s) {
				if (m_stageGpuValid[s])
					m_stageGpuMsSmooth[s] += (m_stageGpuMs[s] - m_stageGpuMsSmooth[s]) * kEmaAlpha;
			}
		}
		if (m_host.BenchMeasuring() && m_stageGpuTimingsSupported) {
			for (uint32_t s = 0; s < smvk::kGpuStageCount; ++s) {
				if (m_stageGpuValid[s]) {
					s_benchStageMs[s] += m_stageGpuMs[s];
				}
			}
			++s_benchStageFrames;
		}
		// Benchmark split report (once, alongside the host's [perf] avg
		// line; the host counts the bench frames after this render).
		if (m_host.BenchFinalFrame()) {
			std::printf("[perf] cpu %.3f ms wait %.3f ms gpu %s present %s\n", m_cpuMsSmooth, m_waitMsSmooth,
				m_gpuTimingValid ? (std::to_string(m_gpuMsSmooth) + " ms").c_str() : "(warming up)",
				m_swapchain ? smvk::ToString(m_swapchain->GetActivePresentMode()) : "?");
			const double n = (double)m_host.GetFrameBudget(); // this render already accumulated
			std::printf("[perf] phases updates %.3f ms (u/l/m %.3f + stream %.3f + fp %.3f [gen %.3f up %.3f ds %.3f]) passes %.3f ms (preA %.3f preB %.3f preC %.3f postMain %.3f) readbacks %.3f ms ui %.3f ms execute %.3f ms\n",
				s_benchUpdates / n, s_benchUpdA / n, s_benchStream / n, s_benchFp / n,
				s_benchFpGen / n, s_benchFpUp / n, s_benchFpDs / n, s_benchPasses / n,
				s_benchPreA / n, s_benchPreB / n, s_benchPreC / n,
				s_benchPostMain / n, s_benchReadbacks / n,
				s_benchUi / n, s_benchExecute / n);
			// GPU shadow-stage split (timestamp queries of the retired frames;
			// avg over the frames each stage ran - the two representations are
			// mutually exclusive per run, so exactly one side reports):
			//   pcf: cascade+spot raster | evsm: casters + resolve + blur stream
			if (m_stageGpuTimingsSupported && s_benchStageFrames > 0) {
				const double sn = (double)s_benchStageFrames;
				std::printf("[perf] shadow GPU pcf raster %s | evsm casters %s resolve %s blur %s (avg ms over %llu frames)\n",
					m_stageGpuValid[smvk::kGpuStagePcfRaster]
					? (std::to_string(s_benchStageMs[smvk::kGpuStagePcfRaster] / sn) + " ms").c_str() : "(off)",
					m_stageGpuValid[smvk::kGpuStageEvsmCasters]
					? (std::to_string(s_benchStageMs[smvk::kGpuStageEvsmCasters] / sn) + " ms").c_str() : "(off)",
					m_stageGpuValid[smvk::kGpuStageEvsmResolve]
					? (std::to_string(s_benchStageMs[smvk::kGpuStageEvsmResolve] / sn) + " ms").c_str() : "(off)",
					m_stageGpuValid[smvk::kGpuStageEvsmBlur]
					? (std::to_string(s_benchStageMs[smvk::kGpuStageEvsmBlur] / sn) + " ms").c_str() : "(off)",
					(unsigned long long)s_benchStageFrames);
			}
			else {
				std::printf("[perf] shadow GPU stage timings unavailable (no cross-queue timestamps)\n");
			}
			std::fflush(stdout);
		}
		// Periodic console snapshot of GPU memory (opt-in, every ~10 s).
		// Same numbers as the ImGui bar; "log to console" button forces
		// one immediately regardless of this toggle.
		if (m_gpuMemConsoleAutoLog && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - m_gpuMemLastLogSec >= 10.0)
			LogGpuMemoryToConsole();
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

	void WorldRenderer::PollCaptureReadback() {
		if (!m_captureReadbackPending)
			return;
		// The previous frame's submit (its TonemapCapture included) has
		// landed - safe to stall for the readback.
		m_captureReadbackPending = false;
		m_host.WaitForDeviceIdle();
		ExportScreenshot(m_captureExtent);
	}
	
	// Scene->frame environment mapping (see SceneEnv): the scene authors
	// LUX-NATIVE intensities (sun 10000 / sky 600 defaults - the same units
	// this EnvironmentSettings and every bake use), so both ride through
	// UNCONVERTED - the env block is the single source of truth for the
	// sun/sky the editor authored, cooked and baked with. Everything the
	// scene does NOT define (sun disc, sky exponent, fog shape, exposure
	// knobs) stays at the engine defaults - the renderer owns no
	// environment state of its own; it renders the world's look.
	// unlitKillSun mirrors the editor frame's Unlit handling (the Unlit
	// debug view replaces shading wholesale anyway; this keeps the lit
	// leftovers - transparents, particles - honest).
	static smvk::EnvironmentSettings MapSceneEnv(const SceneEnv& w,
		bool unlitKillSun)
	{
		smvk::EnvironmentSettings env{}; // engine defaults (disc, exponent, fog shape)
		env.skyEnabled = true;
		env.iblEnabled = true;
		env.skyZenithColor = w.skyColor;
		env.skyHorizonColor = w.skyColor;
		env.skyIntensity = w.skyIntensity;
		env.groundColor = w.skyColor * 0.5f;
		env.iblIntensity = w.skyIntensity;
		env.sunAzimuthDeg = w.sunAzimuth;
		env.sunElevationDeg = w.sunElevation;
		env.sunColor = w.sunColor;
		env.sunIntensity = unlitKillSun ? 0.0f : w.sunIntensity;
		env.fogColor = w.fogColor;
		env.fogDensity = w.fogDensity;
		// Fog quality tiers (see SceneEnv): the volumetric tier's fog renders
		// in the RAYMARCHED pass (the analytic fog stays off - both would
		// read twice); the analytic tier uses the exp2 distance fog (the
		// cheap fallback); off renders nothing. The runtime degrade (the
		// volumetric tier falling back to analytic when the pass cannot run)
		// lives at the frame-env build, where the pass state is known.
		env.fogMode = (w.fogQuality == 1) ? 1 : 0; // 1 = exp2 (kFogExp2)
		return env;
	}

	// Env-zone map rebuild (Initialize):
	// the map DERIVES from the world's zone entities - scene.envForNode
	// nodes whose transforms define coverage (translation/rotation =
	// placement, scale = half extents per local axis, an INFINITE
	// component = an unbounded slab). Channel 0 = the resolved
	// environment (the default look, identity tint + the neutral
	// ambient); channels 1..3 = the top-3 zones (priority descending,
	// then innermost - the same order ZoneAtPosition resolves by, capped
	// by the map's 4 weight channels). Coverage rasterizes into the XZ
	// weight grid as the OBB's XZ projection (the map is 2D; the exact
	// 3D test lives in ZoneAtPosition); bilinear map sampling softens
	// the texel borders. Failure is non-fatal (the plain env path).
	void WorldRenderer::RebuildEnvZoneMap() {
		smvk::envzones::EnvZoneDesc zd{};
		// Channel 0: the resolved look (identity tint, the neutral
		// ambient) - weight 1 outside every zone renders the authored
		// environment exactly.
		zd.zones[0] = smvk::envzones::EnvZone{};

		struct Cand {
			NodeId node;
			float priority;
			double volume;
		};
		std::vector<Cand> cands;
		const SceneEnv* resolved = nullptr;
		uint64_t coverageHash = 1469598103934665603ull;
		if (m_world) {
			const ::Scene& scene = m_world->scene;
			resolved = scene.hasEnvironment
				? &scene.environment : nullptr;
			for (const auto& [node, env] : scene.envForNode) {
				if (node >= scene.globalTransform.size())
					continue;
				const glm::mat4& g = scene.globalTransform[node];
				// Bounded volume = the product of the finite half
				// extents; an INFINITE axis = unbounded (loses ties -
				// "innermost" wins among equal priorities).
				double volume = 1.0;
				bool unbounded = false;
				for (int a = 0; a < 3; ++a) {
					const float h = glm::length(glm::vec3(g[a]));
					if (!std::isfinite(h)) {
						unbounded = true;
						break;
					}
					volume *= double(h);
				}
				if (unbounded)
					volume = std::numeric_limits<double>::infinity();
				cands.push_back({ node, env.zonePriority, volume });
				// Coverage identity: the zone set + each zone's placement
				// (translation + extents) + priority (the top-3 channel
				// selection). Look-only edits keep the hash -> the
				// rasterize/upload tier skips (the defs recompute below).
				auto mix = [&](float v) {
					uint32_t bits;
					std::memcpy(&bits, &v, 4);
					coverageHash = (coverageHash ^ bits)
						* 1099511628211ull;
				};
				coverageHash = (coverageHash ^ node)
					* 1099511628211ull;
				mix(glm::vec3(g[3]).x); mix(glm::vec3(g[3]).y);
				mix(glm::vec3(g[3]).z);
				for (int a = 0; a < 3; ++a)
					mix(glm::length(glm::vec3(g[a])));
				mix(env.zonePriority);
			}
			std::sort(cands.begin(), cands.end(),
				[](const Cand& a, const Cand& b) {
				if (a.priority != b.priority)
					return a.priority > b.priority;
				if (a.volume != b.volume)
					return a.volume < b.volume;
				return a.node < b.node;
			});
			if (cands.size() > 3)
				cands.resize(3); // the map's weight channels cap the set
		}

		// Zone definitions from the authored blocks. The GPU zone look is
		// RELATIVE for the sky (the tint multiplies the env colors built
		// from the resolved block, the ambient scales them): the zone's
		// authored skyColor/skyIntensity map to multipliers of the
		// resolved look (guarded division; the neutral ambient mirrors
		// the identity zone's ambientI = 20). Fog + volumetrics are
		// absolute HDR values.
		auto zoneFromEnv = [&](const SceneEnv& e) {
			smvk::envzones::EnvZone z{};
			const SceneEnv neutral{};
			const SceneEnv& base = resolved ? *resolved : neutral;
			for (int c = 0; c < 3; ++c) {
				const float d = base.skyColor[c];
				z.skyTint[c] = d > 1e-5f ? e.skyColor[c] / d : 1.0f;
			}
			z.ambientI = base.skyIntensity > 1e-5f
				? 20.0f * (e.skyIntensity / base.skyIntensity)
				: 20.0f;
			z.fogColor = e.fogColor;
			z.fogDensity = e.fogDensity;
			z.volumetrics = e.volumetrics;
			z.volumetricDensity = e.volumetricDensity;
			return z;
		};
		for (size_t i = 0; i < cands.size(); ++i) {
			const auto it = m_world->scene.envForNode.find(cands[i].node);
			if (it == m_world->scene.envForNode.end())
				continue;
			zd.zones[1 + i] = zoneFromEnv(it->second);
		}

		// DEFS tier: always. The FrameUniforms fill reads desc.zones per
		// frame, so look-only edits land with no map work at all.
		if (m_envZones.Valid()) {
			std::copy(std::begin(zd.zones), std::end(zd.zones),
				std::begin(m_envZones.desc.zones));
			// COVERAGE tier: only when the zone set/placements/priorities
			// changed (or the map went invalid below).
			if (coverageHash == m_envZoneCoverageHash)
				return;
		}
		m_envZoneCoverageHash = coverageHash;

		if (!m_envZones.Valid() || m_envZones.desc.worldSize != zd.worldSize
			|| m_envZones.desc.texelSize != zd.texelSize) {
			if (!m_envZones.Build(*m_context, zd)) {
				std::cerr << "[WorldRenderer] env zone map build failed - "
					"consumers take the plain env path\n";
				return;
			}
		}
		else {
			std::copy(std::begin(zd.zones), std::end(zd.zones),
				std::begin(m_envZones.desc.zones));
			// In-place reset to the zone-0 fill (wipes the previous
			// raster; the map texture + bindless slot persist).
			std::fill(m_envZones.weights.begin(),
				m_envZones.weights.end(), 0xFFFFFFFFu);
		}

		// Rasterize coverage into the CPU weight grid (hard inside/outside
		// per texel; the map's bilinear sampling blends the borders).
		// XZ projection: local y is ignored (the 2D map; the exact 3D test
		// lives in ZoneAtPosition). INFINITE extents cover the whole map
		// extent by construction (the loop is bounded).
		if (m_world && !cands.empty() && m_envZones.dim > 0) {
			const ::Scene& scene = m_world->scene;
			const uint32_t dim = m_envZones.dim;
			const float ws = m_envZones.desc.worldSize;
			for (size_t i = 0; i < cands.size(); ++i) {
				const Cand& c = cands[i];
				if (c.node >= scene.globalTransform.size())
					continue;
				const glm::mat4& g = scene.globalTransform[c.node];
				const float hx = glm::length(glm::vec3(g[0]));
				const float hz = glm::length(glm::vec3(g[2]));
				const bool infX = !std::isfinite(hx);
				const bool infZ = !std::isfinite(hz);
				// Degenerate finite axis: an empty zone - skip it whole.
				if ((std::isfinite(hx) && !(hx > 0.0f))
					|| (std::isfinite(hz) && !(hz > 0.0f)))
					continue;
				// INF-safe frame (see ZoneCovers): unit columns for the
				// unbounded axes before inverting.
				glm::mat4 frame = g;
				if (infX)
					frame[0] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
				if (infZ)
					frame[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
				const glm::mat4 inv = glm::inverse(frame);
				const uint32_t channel = uint32_t(1 + i);
				for (uint32_t tz = 0; tz < dim; ++tz) {
					const float wz = (float(tz) + 0.5f) / float(dim)
						* ws - ws * 0.5f;
					for (uint32_t tx = 0; tx < dim; ++tx) {
						const float wx = (float(tx) + 0.5f) / float(dim)
							* ws - ws * 0.5f;
						const glm::vec3 local = glm::vec3(
							inv * glm::vec4(wx, 0.0f, wz, 1.0f));
						// UNIT-cube test (the transform carries the
						// extents - see ZoneCovers); XZ projection of the
						// OBB (the map is 2D, local y ignored).
						const bool coverX = infX
							|| std::abs(local.x) <= 1.0f;
						const bool coverZ = infZ
							|| std::abs(local.z) <= 1.0f;
						if (coverX && coverZ)
							m_envZones.weights[tz * dim + tx]
							|= 0xFFu << (8 * channel);
					}
				}
			}
			m_envZones.Upload(*m_context);
		}
		std::cerr << "[Zones] map rebuilt: " << cands.size()
			<< " zone entities (channels 1.." << cands.size()
			<< "), channel 0 = resolved env"
			<< (cands.empty() ? " (identity - no zones authored)" : "")
			<< "\n";
	}
	void WorldRenderer::MountContentVfs(smvk::OverlayVfs& vfs) const {
		// The shipped layout: the project root (when the scene lives in
		// one - project files win on collisions) over loose content/ at
		// the bottom, so project-side cook/bake sidecars resolve exactly
		// where the cooker wrote them.
		if (!m_projectRoot.empty())
			vfs.Mount(std::make_shared<smvk::LooseBackend>(m_projectRoot),
				-10, "project");
		vfs.Mount(std::make_shared<smvk::LooseBackend>("content"), 0, "content");
	}
	void WorldRenderer::RenderFrame(smvk::Framebuffer& fb) {
		SMVK_PROFILE_FUNCTION();
		SMVK_PROFILE_TAG("frameIndex", fb.frameIndex);

		m_frameCounter += 1;
		// Open the Streamline frame (one SL token per rendered frame, shared
		// with the DLSS evaluate below + the engine's PCL submit/present
		// markers in EndAndPresentFramebuffer). Silent no-op unless the PCL
		// plugin loaded - same monotonic-id convention as dlssFrameId.
		(void)smvk::DlssBeginFrame(static_cast<uint32_t>(m_frameCounter & 0xffffffffu));
		m_rendergraph->NewFrame();
		// Texture filtering (trilinear <-> aniso 16x) is renderer state: enforce
		// the selected sampler state on the bindless sampler slots every frame
		// (no-op when already set; recreates slots 0 + 2 on change).
		ApplyTextureFiltering();
		// Sync-oblivious frame: every sync property flows from fb (the
		// host-acquired frame via RenderLayer/onRender), never from the
		// borrowed member. Cross-graph fbs are rejected in RenderLayer
		// above.
		const smvk::Extent2D swapExtent{ FrameSwapExtent(fb).width, FrameSwapExtent(fb).height };
		// Layer rect (element system, see SetLayerRect / onRender): the
		// sub-rect of the swap image this frame draws into. Internal targets
		// size off it, the lens aspects to it, and the swap-writing passes
		// (tonemap / SMAA resolve / UI composite) confine to it with Load
		// semantics so sibling layers survive. Default = full swap.
		const LayerRect layer = ClampedLayer(fb);
		if (!layer.full && (layer.width == 0 || layer.height == 0))
			return; // empty intersection: nothing to draw (counters already bumped above)
		const bool subRect = !layer.full;
		const uint32_t outX = subRect ? layer.x : 0u;
		const uint32_t outY = subRect ? layer.y : 0u;
		smvk::Extent2D outExtent{ subRect ? layer.width : swapExtent.width,
			subRect ? layer.height : swapExtent.height };
		const smvk::AttachmentLoadOp outLoad = subRect
			? smvk::AttachmentLoadOp::Load : smvk::AttachmentLoadOp::Clear;
		// DLSS owns the internal resolution when requested and runnable: the
		// driver-recommended optimal render size replaces the renderScale
		// slider (the slider stays authoritative on the fallback path).
		const bool dlssUsable = DlssRequested() && smvk::DlssAvailable() && m_dlssSupported
			&& m_passes.motionVectors;
		smvk::Extent2D renderExtent = ScaledExtent(outExtent, m_renderScale);
		if (dlssUsable) {
			smvk::DlssOptimalSettings dlssOpt{};
			if (smvk::DlssQueryOptimalSettings(m_dlssMode,
				outExtent.width, outExtent.height, dlssOpt)) {
				renderExtent = { dlssOpt.renderWidth, dlssOpt.renderHeight };
			}
			else {
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
				renderExtent = ScaledExtent(outExtent, dlssScale);
			}
		}
		// Render-extent changes are worth a line: they resize every render
		// target (DLSS mode/quality changes, renderScale edits, resizes).
		// Tag is [EXTENT], NOT [DLSS]: the line fires for ANY extent change
		// (viewport drift, resizes) - DLSS is only one cause, and the old
		// tag had users reading it as "DLSS active in the editor?" (it
		// never is: main.cpp disables the Streamline startup).
		if (renderExtent.width != m_dlssLastRenderExtent.width
			|| renderExtent.height != m_dlssLastRenderExtent.height) {
			std::fprintf(stderr, "[EXTENT] render extent %ux%u -> swap %ux%u%s\n",
				renderExtent.width, renderExtent.height,
				swapExtent.width, swapExtent.height,
				dlssUsable ? " (DLSS optimal)" : "");
		}
		const smvk::Extent2D uiExtent = ScaledExtent(outExtent, m_uiScale);
		UpdateCameraAspect(renderExtent);
		{
			SMVK_PROFILE_SCOPE("WorldRenderer_ResourceEnsure");
			smvk::DepthDesc frameDepthDesc{};
			frameDepthDesc.format = FrameDepthFormat(fb);
			m_viewportResources.renderDepth.Ensure(*m_context, *m_rendergraph, renderExtent, frameDepthDesc);
			smvk::DepthDesc frameHizDepthDesc{};
			frameHizDepthDesc.format = FrameDepthFormat(fb);
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
			m_viewportResources.ssgi.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsgiDesc());
			smvk::MsaaDesc frameMsaaDesc{};
			frameMsaaDesc.depthFormat = FrameDepthFormat(fb);
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
			frameUiDesc.format = LdrUnormSibling(fb);
			m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, frameUiDesc);
			m_viewportResources.uiComposite.Ensure(*m_context);
			smvk::SmaaDesc frameSmaaDesc{};
			frameSmaaDesc.ldrFormat = LdrUnormSibling(fb);
			// Silent target recreation (extent/format drift outside
			// RebuildSwapViews) loses history content: invalidate first so the
			// next T2x record warms up instead of blending against garbage.
			// RebuildSwapViews invalidates unconditionally; this guard covers
			// any other drift path (including first creation).
			if (!m_viewportResources.smaa.TargetsValid()
				|| m_viewportResources.smaa.extent.width != outExtent.width
				|| m_viewportResources.smaa.extent.height != outExtent.height
				|| m_viewportResources.smaa.ldr.format != frameSmaaDesc.ldrFormat)
				smvk::SmaaInvalidateHistory(m_smaaT2x);
			m_viewportResources.smaa.Ensure(*m_context, *m_rendergraph, outExtent, frameSmaaDesc);
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
			// Soft shadows: the softness knob drives the PREFILTER width -
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
			m_viewportResources.dlss.Ensure(*m_context, *m_rendergraph, outExtent);
		}
		// DLSS record snapshot (after Ensure so Valid() matches the
		// pass-emission gate below): jitter phase + offset in render
		// pixels, consumed by UpdateFrameUniforms + the DLSS pass.
		m_dlssFrameActive = DlssActive();
		m_dlssPhaseCount = m_dlssFrameActive
			? smvk::DlssPhaseCount(renderExtent.width, renderExtent.height,
				outExtent.width, outExtent.height)
			: 8u;
		m_dlssJitter = m_dlssFrameActive
			? smvk::DlssComputeJitter(m_dlssPhase, m_dlssPhaseCount)
			: smvk::DlssJitter{};
		// slDLSSSetOptions drift tracking: (re)apply when the mode or the
		// output size changed, release plugin resources when DLSS stops
		// running (mode=eOff alone would keep the VRAM allocated).
		if (m_dlssFrameActive) {
			if (!m_dlssOptionsApplied || m_dlssOptionsMode != m_dlssMode
				|| m_dlssOptionsW != outExtent.width || m_dlssOptionsH != outExtent.height) {
				if (smvk::DlssApplyOptions(m_dlssMode, outExtent.width, outExtent.height)) {
					m_dlssOptionsApplied = true;
					m_dlssOptionsMode = m_dlssMode;
					m_dlssOptionsW = outExtent.width;
					m_dlssOptionsH = outExtent.height;
				}
				else {
					// Options failed this record: skip the pass so tonemap
					// keeps the (bilinear) renderScale path this frame.
					m_dlssFrameActive = false;
				}
			}
		}
		else if (m_dlssOptionsApplied) {
			// Toggle-off: prior frames' slEvaluateFeature dispatches may
			// still be in flight (2 frames in flight). Freeing NGX state
			// underneath them loses the device (VK_CHECK abort). Stall
			// once here - toggle-off is rare - so slFreeResources runs
			// GPU-idle. Preset changes never free, hence never crashed.
			m_host.WaitForDeviceIdle();
			smvk::DlssDeactivate(m_dlssOptionsW, m_dlssOptionsH);
			m_dlssOptionsApplied = false;
			m_dlssOptionsW = 0;
			m_dlssOptionsH = 0;
			m_dlssPhase = 0;
			m_dlssReset = true; // next activation warms up
		}
		// Exposure applied this frame (debug views: fixed reference - see
		// FrameExposure; 1-frame adaptation lag in the Lit view).
		const float frameExposure = FrameExposure();
		// Exposure-aware bloom: the threshold/knee are authored for the
		// reference (manual) exposure; scale them so the bloom trigger
		// stays on the same VISUAL content as auto-exposure shifts. A
		// pixel's tonemapped output depends only on L*exposure, so
		// "blooms iff L*exposure > K" means the pre-exposure threshold
		// scales with 1/exposure - the OLD direct scaling (threshold *
		// exposure) inverted this: dark-adapting scenes (exposure up)
		// pushed the effective threshold to 2-3x the slider value, and
		// bloom silently died until the threshold was slid way down.
		// Applied after Ensure (which resets desc to MakeBloomDesc) and
		// before the bloom passes below. Intensity rides along so the
		// tonemap add matches the sliders.
		if (m_viewportResources.bloom.Valid() && TONEMAP_EXPOSURE > 0.0f) {
			const float bloomScale = TONEMAP_EXPOSURE
				/ std::max(frameExposure, 1e-9f);
			m_viewportResources.bloom.desc.threshold = m_bloomThreshold * bloomScale;
			m_viewportResources.bloom.desc.knee = m_bloomThreshold * 0.25f * bloomScale;
			m_viewportResources.bloom.desc.intensity = m_bloomIntensity;
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
			// reprojects exactly - warm the temporal chain up again.
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
		const bool benchPhase = m_host.BenchMeasuring();
		const auto rfT0 = bench_clk::now();
		bench_clk::time_point rfT2, rfT3; // assigned in the exposure block below
		bench_clk::time_point rfPA, rfPB; // preMain split: fw+/filter | cull/late
		// Hitch logger (VULKANTOY_HITCH_LOG=<ms>): one line per frame that
		// exceeds the budget - traversal hitches vs shader-compile warm-up
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
		// parameter below - never member state). A worldless frame falls
		// back to engine defaults (Initialize refuses those anyway).
		smvk::EnvironmentSettings frameEnv = m_world
			? MapSceneEnv(m_world->scene.environment,
				m_debugView == ViewMode::Unlit)
			: smvk::EnvironmentSettings{};
		// Fog degrade (see SceneEnv::fogQuality): the VOLUMETRIC tier
		// falls back to the ANALYTIC exp2 fog when the raymarched pass
		// cannot run this frame (pass off / disabled / target missing /
		// zero density or intensity / a debug visualization would read
		// twice) - fog never silently disappears. Mirrors the volOn gate
		// at the volumetric pass build below.
		if (m_world && m_world->scene.environment.fogQuality == 2) {
			const bool volRuns = m_passes.volumetric && m_volumetricEnabled
				&& m_viewportResources.volumetric.Valid()
				&& m_volumetricDensity > 0.0f
				&& m_volumetricIntensity > 0.0f
				&& !DebugVisualizationActive();
			if (!volRuns)
				frameEnv.fogMode = 1; // exp2 analytic fallback
		}
		const FrameUniforms frameData = UpdateFrameUniforms(fb.frameIndex, renderExtent, outExtent, frameEnv);
		const smvk::GpuDirectionalLightDescription dirLight = UpdateLights(fb.frameIndex, frameEnv);
		UpdateMaterials(fb.frameIndex);
		const auto rfT0b = bench_clk::now();
		UpdateTextureStreaming(frameData, renderExtent);
		// Probe residency for this frame's eye (throttled inside; the
		// FP upload below reads the rebuilt rows - see m_cubemapRows).
		UpdateCubemapStreaming(m_cameraPositioner.cameraPosition_);
		UpdateGfxTextureDemand(fb.frameIndex);
		const auto rfT0c = bench_clk::now();
		const uint32_t fpLightCount = UpdateForwardPlusLights(fb.frameIndex, frameData);
		const auto rfT1 = bench_clk::now();

		// ------------------------------------------------------------------
		// GPU particles (see smvk::Particles + particles.h): extent-
		// independent builder Ensure (pool/free-list/emitter ring + the
		// three programs) + the per-frame emitter parameter upload. The
		// GPU owns spawn scheduling (fractional carry) and simulation -
		// the CPU never touches particle state.
		// ------------------------------------------------------------------
		m_viewportResources.particles.Ensure(*m_context, *m_rendergraph,
			FrameFlights(fb), MakeParticlesDesc());
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
		m_renderViews.Upload(*m_context, fb.frameIndex);
		const uint64_t renderViewsAddress =
			m_renderViews.DeviceAddress(*m_context, fb.frameIndex);

		// Shared frame context for IFrameRenderers (filled incrementally;
		// pc-dependent fields arrive once pc is built below).
		AppFrameContext fc{};
		fc.ctx = m_context;
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
		// LOD in VISIBLE pixels: the budget keys off the render height.
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
		// FilterMainView - both sides must observe the same decision).
		fc.lodShaderEnabled = ShaderLodActive();

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
		// HDR target for scene - intermediate before tonemap. The aoTerm
		// MRT twin (target1, the SSGI AO export) rides along whenever the
		// SSGI targets exist; Main's lit FS family writes both, the debug
		// views' flat FSs leave target1 at its black clear (the SSGI chain
		// is gated off under debug viz, so the unwritten export is a no-op).
		std::vector<smvk::LogicalImageViewHandle> hdrColors;
		hdrColors.push_back(m_viewportResources.hdr.logicalView);
		if (m_viewportResources.ssgi.aoTerm.Valid())
			hdrColors.push_back(m_viewportResources.ssgi.aoTerm.logical);

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

		// Scene push (see Engine/Renderer/Renderer.h; must match HLSL common.hlsli).
		IRenderer::PushConstants pc{
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
		// (size checked by static_assert in Engine/Renderer/Renderer.h)

		// ---- Cascaded shadow map passes - one per cascade tile (builder owns offsets/extents/load ops) ----
		// Pass-config gate: no shadow sampling without tiles (the main
		// shader reads cascadeBuffer regardless; tiles just stay cleared).
		//
		// Indirect path (gfx filter on): caster candidates are prebuilt once
		// per frame on the render thread (LOD decisions mutate the World's
		// halfrate cache - not thread-safe), each tile's command list is
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
			//
			// Contribution view (mesh-bound fast discard in BuildShadowList):
			// the main camera's view - a caster that cannot touch it
			// contributes nothing to it (the indirect-path twin of the
			// direct fallback's item-level m_shadowContributionCulling).
			// The main view was allocated above (indices stable for the
			// frame); RenderViewSet outlives the worker builds.
			const smvk::RenderView* shadowContributionView = nullptr;
			if (m_shadowContributionCulling
				&& mainRenderView != smvk::RenderViewSet::kInvalidIndex)
				shadowContributionView = &m_renderViews.Get(mainRenderView);
			m_gfxRenderer.BeginShadowFrame();
			if (cascadeViewsValid)
				for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci)
					cascadeShadowSlot[ci] = m_gfxRenderer.BeginShadowView(
						*m_context, *m_rendergraph,
						m_renderViews.Get(cascadeViewBase + ci),
						smvk::gfx::CullFlagBit_Frustum | smvk::gfx::CullFlagBit_Degenerate,
						CascadeTileExtent().width, CascadeTileExtent().height,
						shadowContributionView);
			if (spotViewsValid)
				for (uint32_t si = 0; si < spotSlotCount; ++si)
					spotShadowSlot[si] = m_gfxRenderer.BeginShadowView(
						*m_context, *m_rendergraph,
						m_renderViews.Get(spotViewBase + si),
						smvk::gfx::CullFlagBit_Frustum | smvk::gfx::CullFlagBit_Degenerate,
						SPOT_SHADOW_RES, SPOT_SHADOW_RES,
						shadowContributionView);
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
				IRenderer::PushConstants cascadePC = pc;
				cascadePC.cascadeIndex = ci;
				// Each cascade renders from its own allocated RenderView.
				const uint32_t casterViewIndex =
					cascadeViewBase != smvk::RenderViewSet::kInvalidIndex
					? cascadeViewBase + ci
					: smvk::RenderViewSet::kInvalidIndex;
				cascadePC.renderViewIndex = casterViewIndex;
				if (evsmDirOn) {
					// EVSM moment pass: DEPTH-ONLY 2xMSAA D32 (the PCF shadow
					// programs; the per-sample resolve reconstructs moments
					// right after all moment passes - see the resolve block
					// before the blur block).
					const char* evsmNames[4] = { "EvsmCascade0", "EvsmCascade1", "EvsmCascade2", "EvsmCascade3" };
					auto& momentPass = smvk::EvsmAsyncPrefilter::AddMomentPass(
						*m_rendergraph, m_viewportResources.evsmDir, ci, fb.frameIndex, evsmNames[ci]);
					// GPU stage timer (kGpuStageEvsmCasters): begin at the
					// first moment pass, end at the last (the last spot moment
					// below when spots run, else this cascade).
					const bool evsmTsBegin = (ci == 0);
					const bool evsmTsEnd = (ci == NUM_CASCADES - 1) && !evsmSpotOn;
					momentPass.SetRecordingFunc([this, cascadePC, casterViewIndex, evsmTsBegin, evsmTsEnd](smvk::CommandBuffer& cmd)
					{
						if (evsmTsBegin)
							cmd.WriteStageTimestamp(smvk::kGpuStageEvsmCasters, /*begin=*/true);
						RecordShadowCastersDirect(cmd,
							m_viewportResources.directionalShadows.program,
							cascadePC, casterViewIndex, /*skipFiltered=*/false,
							smvk::MSAASettings::Count2());
						if (evsmTsEnd)
							cmd.WriteStageTimestamp(smvk::kGpuStageEvsmCasters, /*begin=*/false);
					});
					(void)momentPass;
					continue;
				}
				auto& shadowPass = m_viewportResources.directionalShadows.AddCascadePass(*m_rendergraph, ci, cascadeNames[ci]);
				// GPU stage timer (kGpuStagePcfRaster): begin at the first
				// cascade, end at the LAST raster shadow pass (the last spot
				// tile below when spots run, else this cascade).
				const bool pcfTsBegin = (ci == 0);
				const bool pcfTsEnd = (ci == NUM_CASCADES - 1) && !spotPassesOn;
				if (shadowIndirect && cascadeShadowSlot[ci] != UINT32_MAX) {
					const uint32_t slot = cascadeShadowSlot[ci];
					m_gfxRenderer.AddShadowConsumeHints(shadowPass, slot);
					shadowPass.SetRecordingFunc([this, slot, casterViewIndex, cascadePC, renderViewsAddress, pcfTsBegin, pcfTsEnd](smvk::CommandBuffer& cmd)
					{
						if (pcfTsBegin)
							cmd.WriteStageTimestamp(smvk::kGpuStagePcfRaster, /*begin=*/true);
						// GPU context + pass scope come from RenderGraph::Execute
						// (pass name "ShadowCascade<ci>" carries the cascade index).
						m_gfxRenderer.RecordShadowList(cmd, *m_context, slot,
							renderViewsAddress, casterViewIndex, cascadePC);
						// Fallback: unregistered casters only (registered ones
						// are covered by the indirect command list).
						RecordShadowCastersDirect(cmd,
							m_viewportResources.directionalShadows.program,
							cascadePC, casterViewIndex, /*skipFiltered=*/true);
						if (pcfTsEnd)
							cmd.WriteStageTimestamp(smvk::kGpuStagePcfRaster, /*begin=*/false);
					});
				}
				else {
					shadowPass.SetRecordingFunc([this, cascadePC, casterViewIndex, pcfTsBegin, pcfTsEnd](smvk::CommandBuffer& cmd)
					{
						if (pcfTsBegin)
							cmd.WriteStageTimestamp(smvk::kGpuStagePcfRaster, /*begin=*/true);
						RecordShadowCastersDirect(cmd,
							m_viewportResources.directionalShadows.program,
							cascadePC, casterViewIndex, /*skipFiltered=*/false);
						if (pcfTsEnd)
							cmd.WriteStageTimestamp(smvk::kGpuStagePcfRaster, /*begin=*/false);
					});
				}
				(void)shadowPass;
			}
		}

		// ---- Spot shadow passes - one per selected caster tile ----
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
				IRenderer::PushConstants spotPC = pc;
				spotPC.spotIndex = si;
				// Each spot tile renders from its own allocated RenderView.
				const uint32_t casterViewIndex =
					spotViewBase != smvk::RenderViewSet::kInvalidIndex
					? spotViewBase + si
					: smvk::RenderViewSet::kInvalidIndex;
				spotPC.renderViewIndex = casterViewIndex;
				if (evsmSpotOn) {
					// EVSM moment pass: DEPTH-ONLY 2xMSAA D32 (PCF shadow
					// programs; the per-sample resolve follows below). GPU
					// stage timer: the LAST moment pass (the spot tiles
					// follow the cascades) closes kGpuStageEvsmCasters; the
					// FIRST opens it when the directional block didn't
					// (spot-only frames).
					const bool evsmTsBegin = (si == 0) && !evsmDirOn;
					const bool evsmTsEnd = (si == spotCount - 1);
					auto& momentPass = smvk::EvsmAsyncPrefilter::AddMomentPass(
						*m_rendergraph, m_viewportResources.evsmSpot, si, fb.frameIndex, spotNames[si]);
					momentPass.SetRecordingFunc([this, spotPC, casterViewIndex, evsmTsBegin, evsmTsEnd](smvk::CommandBuffer& cmd)
					{
						if (evsmTsBegin)
							cmd.WriteStageTimestamp(smvk::kGpuStageEvsmCasters, /*begin=*/true);
						RecordShadowCastersDirect(cmd,
							m_viewportResources.spotShadows.program,
							spotPC, casterViewIndex, /*skipFiltered=*/false,
							smvk::MSAASettings::Count2());
						if (evsmTsEnd)
							cmd.WriteStageTimestamp(smvk::kGpuStageEvsmCasters, /*begin=*/false);
					});
					(void)momentPass;
					continue;
				}
				auto& spotPass = m_viewportResources.spotShadows.AddSpotPass(*m_rendergraph, si, spotNames[si]);
				// GPU stage timer: the FIRST spot opens kGpuStagePcfRaster
				// when the directional block didn't (spot-only frames); the
				// LAST spot tile closes it (the spot tiles follow the
				// cascades in the raster stage).
				const bool pcfTsBegin = (si == 0) && !m_passes.directionalShadows;
				const bool pcfTsEnd = (si == spotCount - 1);
				if (shadowIndirect && spotShadowSlot[si] != UINT32_MAX) {
					const uint32_t slot = spotShadowSlot[si];
					m_gfxRenderer.AddShadowConsumeHints(spotPass, slot);
					spotPass.SetRecordingFunc([this, slot, casterViewIndex, spotPC, renderViewsAddress, pcfTsBegin, pcfTsEnd](smvk::CommandBuffer& cmd)
					{
						if (pcfTsBegin)
							cmd.WriteStageTimestamp(smvk::kGpuStagePcfRaster, /*begin=*/true);
						// GPU context + pass scope come from RenderGraph::Execute
						// (pass name "SpotShadow<si>" carries the spot index).
						m_gfxRenderer.RecordShadowList(cmd, *m_context, slot,
							renderViewsAddress,
							casterViewIndex,
							spotPC
						);
						// Fallback: unregistered casters only (registered ones
						// are covered by the indirect command list).
						RecordShadowCastersDirect(cmd,
							m_viewportResources.spotShadows.program,
							spotPC, casterViewIndex, /*skipFiltered=*/true);
						if (pcfTsEnd)
							cmd.WriteStageTimestamp(smvk::kGpuStagePcfRaster, /*begin=*/false);
					});
				}
				else {
					spotPass.SetRecordingFunc([this, spotPC, casterViewIndex, pcfTsBegin, pcfTsEnd](smvk::CommandBuffer& cmd)
					{
						if (pcfTsBegin)
							cmd.WriteStageTimestamp(smvk::kGpuStagePcfRaster, /*begin=*/true);
						RecordShadowCastersDirect(cmd,
							m_viewportResources.spotShadows.program,
							spotPC, casterViewIndex, /*skipFiltered=*/false);
						if (pcfTsEnd)
							cmd.WriteStageTimestamp(smvk::kGpuStagePcfRaster, /*begin=*/false);
					});
				}
				(void)spotPass;
			}
		}

		// ---- SDSM depth statistics (after all depth writers) ----
		// sdsm_stats.cs.hlsl: decimated min/max view depth + a 64-bin
		// log-depth histogram over the render depth into the per-flight
		// stats buffer (BDA). UpdateReadbacks submits the buffer on an OWN
		// every-frame async ticket (one-frame latency); UpdateLights fits
		// the cascade split range from the latest completed stats. The
		// rendergraph sorts it after every depth writer via the read hint,
		// wherever Main / the Forward+ prepass wrote depth this frame.
		m_sdsmStatsArmed = false; // set true below iff the pass ran this frame
		if (m_sdsmEnabled && m_passes.directionalShadows
			&& m_resources.sdsmStatsProgram.shaderCount == 1
			&& m_viewportResources.renderDepth.bindlessIndex != UINT32_MAX
			&& !m_sdsmStatsBuffers[fb.frameIndex % 2].empty()) {
			smvk::ImageSyncRequest sdsmDepthRead{};
			sdsmDepthRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
			sdsmDepthRead.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
			sdsmDepthRead.layout = smvk::ImageLayout::DepthStencilReadOnlyOptimal;
			smvk::RenderPass& sdsmPass = m_rendergraph->AddComputePass("SdsmStats");
			sdsmPass.AddImageHint(m_viewportResources.renderDepth.logicalView, sdsmDepthRead);
			SdsmStatsPush sdsmPush{};
			const uint32_t sdsmSlot = fb.frameIndex % 2;
			sdsmPush.stats = m_context->GetBufferDeviceAddress(m_sdsmStatsBuffers[sdsmSlot]);
			sdsmPush.depthIndex = m_viewportResources.renderDepth.bindlessIndex;
			// Main-view near clip (reversed-Z inversion denominator) - same
			// source as UpdateCameraAspect.
			sdsmPush.zNear = default_near_clip;
			// Histogram ceiling = the cascade far cap (the CPU clamps the fit
			// to the same ceiling).
			sdsmPush.zFar = std::max(CascadeFar(), sdsmPush.zNear * 2.0f);
			sdsmPush.width = renderExtent.width;
			sdsmPush.height = renderExtent.height;
			// ceil(totalPixels / 65536 global lanes): ~1 sample per lane =
			// ~65k decimated samples (~a 256^2 mip) - see the shader's lane
			// addressing (gid joins the walk; groups PARTITION the samples).
			sdsmPush.stride = (renderExtent.width * renderExtent.height + 65535u) / 65536u;
			sdsmPush.pad0 = 0;
			smvk::ShaderProgram* sdsmProg = &m_resources.sdsmStatsProgram;
			sdsmPass.SetRecordingFunc([sdsmProg, sdsmPush](smvk::CommandBuffer& cmd) {
				cmd.BindComputePipeline(*sdsmProg);
				cmd.PushConstants(0, sizeof(sdsmPush), &sdsmPush, smvk::ShaderStage::Compute);
				cmd.Dispatch(kSdsmGroups, 1, 1);
			});
			m_sdsmStatsArmed = true;
		}

		// ---- EVSM per-sample resolve (after ALL moment passes) ----
		// ONE dispatch per atlas: evsm_resolve.cs.hlsl loads both depth
		// samples of every texel, warps each, averages into the
		// single-sample moments[flight]. A graphics-queue compute pass
		// (sorted after every moment pass by the msaaDepth RAW hints, before
		// every blur by the moments RAW hints) - the async blur window below
		// stays graphics-free exactly as before.
		if (evsmDirOn)
			smvk::EvsmAsyncPrefilter::AddResolvePass(
				*m_rendergraph, m_viewportResources.evsmDir, *m_context, fb.frameIndex);
		if (evsmSpotOn)
			smvk::EvsmAsyncPrefilter::AddResolvePass(
				*m_rendergraph, m_viewportResources.evsmSpot, *m_context, fb.frameIndex);

		// ---- EVSM prefilter (all tiles, after the resolve) ----
		// Separable H+V blur per tile, AsyncCompute-marked. Recording ALL
		// blurs after ALL moments + the resolve keeps the async window free
		// of graphics passes, which is what lets RenderGraph::Execute split
		// the frame into G1/C/G2 on the true-async compute queue (an
		// interleaved graphics write inside the window forces the emulated
		// single-submit fallback). SameFrame consumes this frame's blur;
		// OneFrameLag trails by one slot (see EvsmAtlas::SampleSlot).
		if (evsmDirOn) {
			for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci)
				smvk::EvsmAsyncPrefilter::AddTileBlurPasses(
					*m_rendergraph, m_viewportResources.evsmDir, *m_context,
					fb.frameIndex, ci);
		}
		if (evsmSpotOn) {
			for (uint32_t si = 0; si < spotSlotCount; ++si)
				smvk::EvsmAsyncPrefilter::AddTileBlurPasses(
					*m_rendergraph, m_viewportResources.evsmSpot, *m_context,
					fb.frameIndex, si);
		}

		// ---- Forward+ passes (depth prepass + tile cull) ----
		// Owned by ForwardPlusRenderer (see App/forwardplus_renderer.h).
		rfPA = bench_clk::now();
		fc.pc = pc;
		fc.depthView = m_viewportResources.renderDepth.logicalView;
		fc.depthViewHandle = m_viewportResources.renderDepth.view;
		// Option A: HZB source handles - UNCONDITIONAL (the hizDepth image
		// is a fixed cost of the renderer: the HZB pyramid ALWAYS builds
		// over it; the occluder toggle below gates only the big-crudest-LOD
		// occluder FILL and the filter's HZB cull usage, never the source).
		// This also keeps the depth targets' use windows unambiguous for
		// the target-heap aliasing (hizDepth [OccluderPrepass..Hzb] vs
		// renderDepth [Seed..] - see TargetPhase).
		fc.hizDepthView = m_viewportResources.hizDepth.logicalView;
		fc.hizDepthViewHandle = m_viewportResources.hizDepth.view;
		fc.hizDepthBindless = m_viewportResources.hizDepth.bindlessIndex;
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
				// - nothing dereferences it on GPU (filter push carries the
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
		// all key on it - Main writes the TRANSIENT MSAA depth whose
		// stencil is DontCare-stored, so the 1x stencil aspect goes
		// unwritten there and the layer must fall back).
		const bool useMsaa = m_passes.msaa && m_viewportResources.msaaEnabled
			&& m_context->SupportsMSAA2x() && m_viewportResources.msaa.Valid();
		// SSSR-style thin G-buffer (see gbuffer.frag.hlsl): the Forward+
		// depth prepass writes it (RGBA8 oct normal + roughness + metal -
		// the prepass already rasterizes every opaque surface; Main is
		// untouched, no stencil classification). False under MSAA (the
		// prepass has no stable 1x G-buffer companion). Consumers: the SSR
		// trace (receiver normals) AND the SSGI AO compose's joint
		// bilateral (center normal) - wanted when EITHER is on (the AO
		// chain degrades to depth-only edge weights without it).
		const bool ssrWantsGbuffer = m_passes.ssr && m_ssrEnabled
			&& m_ssrIntensity > 0.0f;
		const bool aoWantsNormal = m_passes.ssao && m_aoMode != AoMode::None
			&& m_ssaoIntensity > 0.0f;
		fc.ssrGbufferWanted = (ssrWantsGbuffer || aoWantsNormal)
			&& !DebugVisualizationActive()
			&& !useMsaa
			&& m_viewportResources.ssr.gbuffer.Valid();
		fc.gbufferView = fc.ssrGbufferWanted
			? m_viewportResources.ssr.gbuffer.logical
			: smvk::LogicalImageViewHandle{};
		// One-shot material census (VULKANTOY_MATERIAL_CENSUS): dumps
		// every material's factors (incl. layers) + the resulting
		// G-buffer roughness/metal to stderr - the per-material ground
		// truth for the G-buffer's continuous values.
		static bool sMaterialCensusDone = false;
		if (!sMaterialCensusDone && m_world) {
			sMaterialCensusDone = true;
			if (std::getenv("VULKANTOY_MATERIAL_CENSUS")) {
				// Layer combine: MIN roughness wins (strongest reflector),
				// metal ORs - the same combine the G-buffer represents.
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
		// be culled - which is why T4 occlusion looked permanently silent.
		//
		// Depth assembly afterwards (every filterable cluster depthed
		// EXACTLY ONCE, no full prepass walk):
		//   seed      prev intersect curr clusters (occluders: opaque VS-only +
		//             masked alpha-tested) - drawn in AppendPasses above.
		//   remainder curr-prev clusters (newly visible: disocclusion,
		//             LOD flips, frustum entries) - the T5 CPU-command
		//             pass after the filter. It ignores the GPU verdicts
		//             (draws all curr-prev even if occluded), but that set
		//             is exactly the disocclusion candidates the seed-only
		//             HZB cannot prove occluded anyway.
		//   fallback  prepass walks ONLY the unfilterable rest: unregistered
		//             meshes/LOD levels, material-less meshes and shader-LOD
		//             vertex-lit instances (fallbackOnly=true below).
		// No indirect alpha-tested draws: drawing the MASKED ranges through
		// the filter's GPU-written commands misrasterizes depth (the
		// opaque-white regression; bisected - any depth write from that
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
			? std::function<bool(NodeId, uint32_t, uint32_t, uint32_t)>(
				[this](NodeId node, uint32_t worldMesh, uint32_t lodIdx,
					uint32_t material) {
			// Shader-LOD far levels with non-default shading (vertex-lit
			// substitution or quality specialization) draw direct in Main
			// - the prepass must keep covering them.
			if (ShaderLodActive() && m_world
				&& !m_world->FarShadingAt(worldMesh, lodIdx,
					material).isDefault())
				return false;
			(void)node;
			return m_gfxRenderer.GfxMeshForLod(worldMesh, lodIdx)
				!= UINT32_MAX;
		})
			: std::function<bool(NodeId, uint32_t, uint32_t, uint32_t)>{};
		// HZB source: ALWAYS hizDepth (the fixed-cost occluder depth - see
		// the fc.hizDepthView note above; the old renderDepth fallback path
		// is gone). The PYRAMID + the filter's HZB cull toggle WITH the
		// occluder fill (hizOccluderOn): culling over an unfilled depth
		// would cull visible clusters; the hizDepth IMAGE itself stays a
		// fixed cost either way (the target-heap aliasing window declares
		// it [OccluderPrepass..Hzb] regardless).
		auto appendHizPasses = [&]() {
			m_gfxRenderer.AppendHizPasses(fc,
				m_viewportResources.hizDepth.logicalView,
				m_viewportResources.hizDepth.bindlessIndex);
		};
		if (depthSplit) {
			// Seed (prev-visible occluders, current LOD, alpha-tested) ->
			// HZB over that depth -> filter culls the rest.
			if (!cullingFrozen && hizOccluderOn)
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
				if (!cullingFrozen && hizOccluderOn)
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
		// deferred shadows - with lightCount 0 (the DS lists own the lights,
		// the light leg of the cull is dead work; the decal leg still fills
		// the per-tile decal lists, so splats work in BOTH shading paths).
		// Convex GI volumes ride the same rule through their per-tile
		// bitmask leg (TileCullWillRun is the single gate for the dispatch
		// AND the FrameUniforms mask publish - see UpdateFrameUniforms).
		const bool dsOn = DsActive();
		const bool decalListsNeeded = DecalsActive() && m_decalFrameCount > 0;
		// Reflection probes ride the light buffer tail as type-2 entries: the
		// SAME forward+ cull dispatch buckets them, so when probes are loaded
		// the dispatch runs even under deferred shadows. The cull MUST see the
		// full [0, lights+probes) range - the probe tail lives at [L, L+P) and
		// the cull reads buffer indices from 0, so a lights-only count never
		// files a single probe (every consumer then degrades to the global
		// fallback cubemap). Under DS the light leg re-culls lights as dead
		// work (negligible next to the depth preload); the DS lists own the
		// lights either way.
		const bool probeListsWanted = ProbeTileListsWanted();
		rfPB = bench_clk::now();
		if (TileCullWillRun())
			m_fpRenderer.AppendTileCull(fc, fpLightCount + ProbeEntryCount());
		if (dsOn)
			AppendDeferredShadowPhases(fb.frameIndex,
				frameUniformsAddress, fpLightsAddress, spotShadowBufferAddress,
				fpLightCount, fc.depthView, fc.fpLights);

		// ---- GPU particles: emit + simulate (compute). Runs after the light
		// cull spot (independent of it - the draw pass below consumes the tile
		// lists; emit -> sim ordering matters and comes from the pass hints:
		// emit pops free slots the PREVIOUS frame's sim recycled, so a slot
		// can never be handed out twice within a frame).
		const bool particlesRun = particlesOn && particleEmitterCount > 0;
		if (particlesRun) {
			smvk::Particles& ps = m_viewportResources.particles;
			// Clamp dt: a tab-out/hibernate spike would otherwise fling every
			// particle across the scene (spawn quotas too - rate*dt is the
			// quota, so a 5 s dt would try to spawn 5 s worth at once).
			const float particleDt = std::min(m_deltaTime, 1.0f / 30.0f);

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
		// Motion vectors - per-pixel prevUV - currUV into RG16F (motionvector.h)
		// Re-rasterizes opaque geometry depth-tested READ-ONLY against the 1x
		// prepass depth (reversed-Z GreaterEqual, no writes) - identical in
		// MSAA / non-MSAA modes, Main's transient 2x depth is never touched.
		// MASK cutouts share the program: the FS alpha-tests only when
		// shadingModel == Masked (Cull None phase, like the prepass).
		// ---------------------------------------------------------------------
		if (m_passes.motionVectors && m_viewportResources.motionVectors.Valid()) {
			IRenderer::PushConstants motionPC = pc;
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
					cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &motionPC, smvk::ShaderStage::All);
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
					IRenderer::PushConstants objectPC{};
					std::copy_n(glm::value_ptr(model), 16, objectPC.model);
					// Previous frame's transform: zero motion on the first
					// frame or after the node count changed (scene rebuild).
					const bool prevValid =
						m_prevGlobalTransforms.size() == m_world->scene.globalTransform.size();
					const glm::mat4 prevModel =
						prevValid ? m_prevGlobalTransforms[item.node] : model;
					// Pack as float3x4 columns (smvk::PackPushFloat3x4): a
					// flat 12-float copy is wrong (drops translation, leaks
					// the projection row - see the helper's comment).
					smvk::PackPushFloat3x4(glm::value_ptr(prevModel), objectPC.prevModel);
					objectPC.materialId = item.material; // FS alpha test
					cmd.PushConstants(offsetof(IRenderer::PushConstants, materialId),
						sizeof(objectPC.materialId), &objectPC.materialId,
						smvk::ShaderStage::All);
					cmd.PushConstants(offsetof(IRenderer::PushConstants, model),
						sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
					cmd.PushConstants(offsetof(IRenderer::PushConstants, prevModel),
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
						cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &motionPC, smvk::ShaderStage::All);
						cmd.PushConstants(offsetof(IRenderer::PushConstants, materialId),
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
				cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &motionPC, smvk::ShaderStage::All);
				m_world->forEachRenderItem(
					smvk::IteratorFlagBit_Renderable
					| smvk::IteratorFlagBit_Opaque
					| smvk::IteratorFlagBit_StaticVertexFmt
					, [&](const smvk::World::RenderItem& item) { drawMotionNode(item, false); });
				// Phase 2: MASK cutout - the pipeline binds lazily per
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
					cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &motionPC, smvk::ShaderStage::All);
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
					cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &motionPC, smvk::ShaderStage::All);
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
		// AO moved to the SSGI post chain (below, after particles): Main now
		// EXPORTS the AO-sensitive lighting through the aoTerm MRT target and
		// the SSGI compose applies it - the pre-Main scheduling constraint
		// (depth must be final + sampleable without attachment clashes) died
		// with it. See Engine/Rendergraph/ssgi.h + the SSGI block after the
		// particles pass.
		// ---------------------------------------------------------------------

		// useMsaa is hoisted above the fc fill (the stencil layer gate + the
		// pane layer below + Main's attachments here all key on it).
		std::vector<smvk::LogicalImageViewHandle> mainInputs;
		mainInputs.push_back(m_viewportResources.directionalShadows.logicalView);
		if (m_viewportResources.spotShadows.Valid())
			mainInputs.push_back(m_viewportResources.spotShadows.logicalView);
		const auto rfTM = bench_clk::now();
		auto& mainPass = m_rendergraph->AddPass("Main")
			.SetOffset(0, 0)
			.SetInputImages(mainInputs)
			.SetExtent(renderExtent);
		if (useMsaa) {
			// 2x MSAA color resolves into the 1x HDR intermediate; the
			// aoTerm MRT twin rides the SAME resolve (AVERAGE) so the
			// SSGI subtraction stays exact through it (averaging commutes
			// with the linear hdr - term*(1-ao) reconstruction).
			mainPass.SetColorWithResolve(
				{ m_viewportResources.msaa.colorLogical,
				  m_viewportResources.ssgi.aoTermMsaa.logical },
				{ m_viewportResources.hdr.logicalView,
				  m_viewportResources.ssgi.aoTerm.logical },
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
		}
		else {
			mainPass.SetColor(
				hdrColors, // HDR intermediate - tonemapper will resolve to swap
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0, 0, 0, 1)
			)
				// Depth was filled by the Forward+ prepass: load it, test
				// read-only (GreaterEqual = reversed-Z near-wins).
				// StoreOp MUST be Store, never DontCare: SSR/volumetric/DS
				// sample this depth AFTER Main, and a DontCare store makes
				// the contents undefined past the pass - the driver may
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
		// Task-less mesh leg (build-time gate): device support + enabled +
		// programs + opaque state (no wireframe). Execute-time eligibility
		// is per range (RangeMeshEligible: opaque, single-sided, variant
		// twin exists); the indexed loop skips exactly those ranges.
		const bool meshActiveForFrame = m_passes.gfxMesh && m_useGfxMesh
			&& smvk::SupportsMeshShaders()
			&& m_gfxRenderer.MeshProgramsValid()
			&& !m_wireframe
			&& m_useGfxFilter && gfxSlot != UINT32_MAX;
		mainPass.SetRecordingFunc([this, pc, useMsaa, gfxSlot, renderViewsAddress, mainRenderView, coneLineVerts, frameViewProj, transparentCullView, meshActiveForFrame](smvk::CommandBuffer& cmd)
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

			// NOTE: no stencil classification here - the SSSR-style thin
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
			// the caller's program - the pre-swap shaded wireframe.
			auto wireTwinOf = [&](smvk::ShaderProgram& prog) -> smvk::ShaderProgram& {
				if (!m_wireframe)
					return prog;
				if (&prog == &m_resources.mainProgram
					|| &prog == &m_resources.mainProgramA2C
					|| IsFarSpecProgram(prog)) {
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
				// Dither-on-closeup (Scene::ditherCloseup; per frame, per
				// draw): Hidden draws never reach here (the leg loops skip
				// them below); Fade draws carry the per-draw flag
				// (PushConstants::padEnd_[0] - default.vert forwards it flat
				// to the FS family) so the lit leg screen-doors in step with
				// the depth/shadow legs. Always pushed (0 or 1): the partial
				// push range persists across draws, so a stale Fade flag must
				// never leak into the next draw.
				IRenderer::PushConstants pc{};
				pc.materialId = materialId;
				pc.padEnd_[0] = (DitherCloseupStateFor(m_world->scene,
					int(node), m_cameraPositioner.cameraPosition_)
					== DitherCloseupState::Fade) ? 1u : 0u;
				cmd.PushConstants(
					offsetof(IRenderer::PushConstants, padEnd_),
					sizeof(pc.padEnd_[0]),
					&pc.padEnd_[0],
					smvk::ShaderStage::All
				);
				const float* modelData = glm::value_ptr(model);
				std::copy_n(modelData, 16, pc.model);
				cmd.PushConstants(
					offsetof(IRenderer::PushConstants, materialId),
					sizeof(pc.materialId),
					&pc.materialId,
					smvk::ShaderStage::All
				);
				cmd.PushConstants(
					0,
					offsetof(IRenderer::PushConstants, frameUniforms),
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
				// writeStencilClass: kept for call-shape stability - the SSR
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
			// TMP-DIAG (VULKANTOY_NOCULL=1): force CullMode::None on every
			// bindForMaterial bind — the facing proof for the distant-
			// sibling draw-drop (the commands are written with real counts
			// at CULLFLAGS=0x0 yet the pixels stay black; if the band
			// appears with culling off, the filtered leg inverts facing
			// vs the direct legs). Remove later.
			{
				static const bool noCull = [] {
					const char* e = std::getenv("VULKANTOY_NOCULL");
					return e && std::strcmp(e, "1") == 0;
				}();
				if (noCull)
					raster.cullMode = smvk::CullMode::None;
			}
				// Masked cutout in 2x MSAA + A2C opt-in (VULKANTOY_A2C):
				// alpha-to-coverage program (coverage remap in the FS, zero
				// discard) + A2C multisample state - foliage silhouettes
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
					sizeof(IRenderer::PushConstants),
					&pc,
					smvk::ShaderStage::All
				);
				cmd.Draw(3, 1, 0, 0);
			}
			// ---- Terrain field (visual-only dressing): right after the sky,
			// before the scene opaques - depth GreaterEqual against the
			// prepass depth (1x) / the transient cleared depth (2x MSAA), so
			// scene geometry occludes the field and the field composites over
			// the sky background. NOT in the depth prepass / shadow passes by
			// design: in 1x the Main depth is read-only, so post-Main depth
			// consumers (SSR/volumetric marching) see the prepass depth over
			// terrain pixels - accepted for the visual-only first cut.
			if (m_passes.terrain && m_terrainEnabled && m_terrain.valid) {
				m_terrain.RecordDraws(
					cmd,
					frameViewProj,
					m_cameraPositioner.cameraPosition_,
					pc,
					opaqueDepth,
					noStencil,
					smvk::BlendSettings::Opaque(),
					msaaSettings
				);
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
				sizeof(IRenderer::PushConstants),
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
			ViewMode effView = m_debugView;
			if ((effView == ViewMode::Meshlets || effView == ViewMode::Lods)
				&& !m_gfxRenderer.DebugProgramsValid())
				effView = ViewMode::Lit;
			if (effView == ViewMode::Unlit && m_resources.unlitProgram.shaderCount != 2)
				effView = ViewMode::Lit;
			if (effView == ViewMode::Cascades && m_resources.cascadeProgram.shaderCount != 2)
				effView = ViewMode::Lit;
			if (effView == ViewMode::VertexColors && m_resources.vertexColorProgram.shaderCount != 2)
				effView = ViewMode::Lit;
			if (effView == ViewMode::Uv0 && m_resources.uv0DebugProgram.shaderCount != 2)
				effView = ViewMode::Lit;
			if (effView == ViewMode::Uv1 && m_resources.uv1DebugProgram.shaderCount != 2)
				effView = ViewMode::Lit;
			if (effView == ViewMode::Pom && m_resources.pomProgram.shaderCount != 2)
				effView = ViewMode::Lit;
			if (effView == ViewMode::SsrContribution && m_resources.ssrContributionDebugProgram.shaderCount != 2)
				effView = ViewMode::Lit;
			if (effView == ViewMode::SsrMask && m_resources.ssrMaskDebugProgram.shaderCount != 2)
				effView = ViewMode::Lit;
			const bool meshletDbg = (effView == ViewMode::Meshlets);
			const bool lodDbg = (effView == ViewMode::Lods);
			const bool flatDbg = meshletDbg || lodDbg; // meshDebugProgram path
			const bool unlitDbg = (effView == ViewMode::Unlit);
			const bool cascadeDbg = (effView == ViewMode::Cascades);
			const bool vertexColorDbg = (effView == ViewMode::VertexColors);
			const bool uvDbg = (effView == ViewMode::Uv0 || effView == ViewMode::Uv1);
			const bool pomDbg = (effView == ViewMode::Pom);
			const bool ssrMaskDbg = (effView == ViewMode::SsrMask);
			const bool directDbg = unlitDbg || cascadeDbg || vertexColorDbg || uvDbg || pomDbg || ssrMaskDbg; // default.vert + debug FS
			const bool bypassFilter = (effView != ViewMode::Lit);
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
					// with non-default far shading (vertex-lit substitution
					// or quality specialization) report WHITE instead of
					// their LOD palette color, so shading downgrades are
					// visible next to geometry ones.
					static const float kLodVertexLit[3] = { 1.0f, 1.0f, 1.0f };
					const bool vlitLevel = ShaderLodActive() && m_world
						&& !m_world->FarShadingAt(item.mesh, lod,
							item.material).isDefault();
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
					: effView == ViewMode::Uv1 ? m_resources.uv1DebugProgram
					: effView == ViewMode::SsrContribution ? m_resources.ssrContributionDebugProgram
					: ssrMaskDbg ? m_resources.ssrMaskDebugProgram
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
					IRenderer::PushConstants objPC = pc;
					objPC.materialId = materialId;
					std::copy_n(glm::value_ptr(model), 16, objPC.model);
					cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &objPC, smvk::ShaderStage::All);
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
				// Mesh leg first (one DrawMeshTasks per eligible range); the
				// indexed loop below skips exactly those ranges
				// (RangeMeshEligible), so nothing draws twice and
				// nothing drops. Debug views bypass (direct draws above).
				const bool meshActive = meshActiveForFrame && !bypassFilter;
				if (meshActive) {
					const engine::GIVariant meshGiVar = GetActiveGIVariant();
					const bool giActive = meshGiVar != engine::GIVariant::None;
					// frameViewProj = the LIVE main-view matrix (the output
					// projection): under the debug culling freeze the view
					// record's matrix is the frozen CULL one, and the mesh
					// output must still project through the live view (the
					// indexed VS does the same through the render-view slab).
					m_gfxRenderer.RecordMeshMain(cmd, *m_context, gfxSlot,
						renderViewsAddress, mainRenderView,
						giActive, uint32_t(meshGiVar), frameViewProj,
						opaqueDepth, msaaSettings);
				}
				const auto& gfxRanges = m_gfxRenderer.MaterialRanges(gfxSlot);
				// The GI permutation is a per-frame uniform decision (the
				// loaded GI state - one FS for the whole frame): the .giN
				// twin carries the GI ambient + the F8 debug views. It takes
				// priority over the lobe-count refinement - the sgN twins
				// predate the GI permutations (no -DGI_* in their compiles)
				// and would compile the GI branches out of the frame. The
				// (deprecated lightmap) lobe-count twin only refines ranges
				// when NO GI variant is active.
				const engine::GIVariant giVar = GetActiveGIVariant();
				for (uint32_t ri = 0; ri < (uint32_t)gfxRanges.size(); ++ri) {
					// Mesh-drawn ranges skip the indexed multi-draw (the
					// mesh dispatches above covered them; the contract is
					// exact - see RecordMeshMain/RangeMeshEligible).
					if (meshActive && m_gfxRenderer.RangeMeshEligible(gfxSlot, ri,
						giVar != engine::GIVariant::None, uint32_t(giVar)))
						continue;
					const uint32_t materialId = gfxRanges[ri].material;
					// Std twin pick per range (non-layered materials bind the
					// MAT_STANDARD FS - the layered machinery compiled out;
					// layered keep the monolith).
					const bool rangeLayered = m_gfxRenderer.MaterialLayered(materialId);
					smvk::ShaderProgram& litProg = giVar != engine::GIVariant::None
						? m_gfxRenderer.FilteredProgramGi(uint32_t(giVar), rangeLayered)
						: m_gfxRenderer.FilteredLitProgram(gfxRanges[ri].lobeCount, rangeLayered);
					bindForMaterial(materialId, litProg,
						m_gfxRenderer.FilteredProgramA2C(), opaqueDepth);
					IRenderer::PushConstants matPC{};
					matPC.materialId = materialId;
					cmd.PushConstants(
						offsetof(IRenderer::PushConstants, materialId),
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
					// Dither-on-closeup (Scene::ditherCloseup; per frame, per
					// draw): Hidden draws drop out of the direct leg entirely
					// (the CPU cull - no leg sees them this frame; the filter
					// makes the same call at registration, so both paths
					// agree). Fade draws pass through drawNode, which pushes
					// the per-draw flag (padEnd_[0]) for the lit FS family.
					if (DitherCloseupStateFor(m_world->scene, int(item.node),
						m_cameraPositioner.cameraPosition_)
						== DitherCloseupState::Hidden)
						return;
					// Shader LODs: instances whose selected level is the farthest
					// one draw HERE - vertex-lit substitution, or the lit
					// program with the material's quality specialization -
					// FilterMainView skipped them at registration for the
					// same reason (the filtered VS is lit-only), so this is
					// the exactly-once draw.
					const smvk::World::FarShading farShading = shaderLodOn
						? FarShadingForNode(item.node, item.mesh,
							item.material)
						: smvk::World::FarShading{};
					const bool vertexLitLevel = farShading.vertexLit;
					// Specialized far shading: the cached twin pair for the
					// material's quality (falls back to the plain twins when
					// the base programs are missing - never binds empty).
					FarSpecPrograms* farSpec =
						(!vertexLitLevel && !farShading.isDefault())
						? FarSpecProgramsFor(farShading.quality) : nullptr;
					// Filtered instances already drew above via indirect
					// ranges (FilterMainView pushes the selected LOD sibling).
					// Only LODs whose registration failed fall through to the
					// direct LOD draw, so each instance rasterizes once.
					if (gfxActive && !vertexLitLevel && m_gfxRenderer.GfxMeshForLod(item.mesh,
						LodIndexForNode(item.node, item.mesh)) != UINT32_MAX)
						return;
					smvk::ShaderProgram& prog = vertexLitLevel
						? m_resources.vertexLitProgram
						: (farSpec && farSpec->main.shaderCount == 2)
						? farSpec->main : m_resources.mainProgram;
					smvk::ShaderProgram& progA2c = vertexLitLevel
						? noA2CProgram
						: (farSpec && farSpec->a2c.shaderCount == 2)
						? farSpec->a2c : m_resources.mainProgramA2C;
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
			// needed - order-independent like opaque.
			// Blended: classic back-to-front AlphaBlend, depth test on, no
			// depth writes, sorted by distance-to-camera.
			// m_hideTransparent (debug) skips the whole leg to bisect blending
			// artifacts out of hole reports. The list itself is shared with
			// the SSR PaneMask redraw (CollectTransparentDraws).
			{
				const bool ditheredTransp =
					(m_transparentMode == smvk::TransparentMode::Dithered);
				std::vector<TransparentDrawItem> draws =
					CollectTransparentDraws(transparentCullView, !ditheredTransp);
				const bool lodDbg = (effView == ViewMode::Lods);

				// Transparent follows the opaque debug view (Unlit/Cascades shade
				// it with the same debug FS via full pushes; LODs draws it flat
				// in the instance LOD color). Meshlets keeps the legacy look
				// (transparent stays lit).
				smvk::ShaderProgram* transProg = &m_resources.mainProgram;
				if (unlitDbg) transProg = &m_resources.unlitProgram;
				else if (cascadeDbg) transProg = &m_resources.cascadeProgram;
				// Lit-view program: the SAME std/gi twin selection as every
				// other lit leg (MainLitProgramFor) - this leg used to hardcode
				// the monolith mainProgram, making it the ONLY lit leg still
				// binding the full layered-machinery FS for non-layered
				// materials (parity gap; the App path picked the std twin).
				auto litTransProg = [&](uint32_t materialId) -> smvk::ShaderProgram& {
					return (unlitDbg || cascadeDbg)
						? *transProg
						: MainLitProgramFor(materialId);
				};
				// Shader LODs apply to transparent too, but only in the Lit view
				// (debug views replace shading wholesale, see above).
				const bool transShaderLodOn = ShaderLodActive()
					&& effView == ViewMode::Lit;
				MeshDebugPush transLp{};
				if (lodDbg)
					std::copy_n(glm::value_ptr(frameViewProj), 16, transLp.viewProj);

				// Meshlets (F3) frame-push restore: the opaque meshlet pass above
				// full-pushed MeshDebugPush (model/viewProj/color) over the shared
				// [0,256) push range, clobbering every PushConstants frame field -
				// bytes 64..135 (frameUniforms/lightsBuffer/materialsBuffer/
				// cascadeBuffer/fpLights/fpTileCounts/fpTileIndices) now hold
				// viewProj/color float bits, while drawNode below re-pushes only
				// model (bytes 0..63) + materialId. Meshlets is the ONLY debug
				// view whose transparent leg still delta-pushes a shaded program
				// over that garbage (directDbg views full-push per draw below;
				// LODs pushes its flat MeshDebugPush per draw), so the FS
				// dereferences garbage BDAs - pc.bLights.Get().lights[0] is the
				// lit FS's first read - and the device dies on the first
				// transparent draw (headless repro: VK_ERROR_DEVICE_LOST +
				// [DeviceFault] on frame 0; the wild BDA read kills the context
				// instantly, no TDR). The Meshlets debug view hits the same\r
				// leg (its flat MeshDebugPush per draw is self-contained).
				// the same clobbered BDAs). Restore the frame push once, before
				// the loop; the Lit view needs nothing (the filtered/direct
				// legs left a valid range in it).
				if (effView == ViewMode::Meshlets)
					cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &pc,
						smvk::ShaderStage::All);

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
					opaqueDepth,
					noStencil,
					smvk::BlendSettings::Opaque(),
					msaaSettings,
					// TMP-DIAG (VULKANTOY_LODVIEW_CULLBACK=1): bind Back instead
					// of None in the LODs flat view — the winding-flip proof
					// for the distant-sibling draw-drop (every winding-
					// sensitive stage kills them; the winding-agnostic views
					// draw them). With Back bound, LOD3 instances whose
					// decimated triangles are inverted vanish here too while
					// LOD0 stays. Remove later.
					wireframeRaster(smvk::RasterSettings{
						.cullMode = (std::getenv("VULKANTOY_LODVIEW_CULLBACK")
							&& std::strcmp(
								std::getenv("VULKANTOY_LODVIEW_CULLBACK"), "1") == 0)
							? smvk::CullMode::Back : smvk::CullMode::None })
				);
				cmd.BindStaticVertexFmt();
							boundMaterial = d.materialId;
						}
						const glm::mat4& model = m_world->scene.globalTransform[d.node];
						std::copy_n(glm::value_ptr(model), 16, transLp.model);
						static const float kLodVertexLitTrans[3] = { 1.0f, 1.0f, 1.0f };
						const bool vlitLevelTrans = ShaderLodActive() && m_world
							&& !m_world->FarShadingAt(d.meshId, lod,
								d.materialId).isDefault();
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
					// (or the quality-specialized lit program when the
					// material opts out) with a full push (the per-material
					// delta push below feeds the lit program only). Same
					// coarsest geometry.
					const smvk::World::FarShading farShadingTrans = transShaderLodOn
						? FarShadingForNode(d.node, d.meshId, d.materialId)
						: smvk::World::FarShading{};
					const bool vlitTrans = farShadingTrans.vertexLit;
					// Specialized far shading (see the opaque leg): the
					// cached twin pair for the material's quality. Plain
					// mainProgram-based (like the opaque direct leg), not
					// the std/gi twin selection - correct everywhere, and
					// far + transparent + specialized is rare enough that
					// the twin-selection parity gap doesn't matter.
					FarSpecPrograms* farSpecTrans =
						(!vlitTrans && !farShadingTrans.isDefault())
						? FarSpecProgramsFor(farShadingTrans.quality)
						: nullptr;
					const bool farSpecTransValid = farSpecTrans
						&& farSpecTrans->main.shaderCount == 2;
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
							}
							else {
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
						IRenderer::PushConstants objPC = pc;
						objPC.materialId = d.materialId;
						std::copy_n(glm::value_ptr(model), 16, objPC.model);
						cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &objPC, smvk::ShaderStage::All);
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
					// Specialized transparent far leg (mirrors the vertex-lit
					// leg's full-push shape above, but binds the
					// quality-specialized twin): same coarsest geometry, same
					// dithered/blended handling, cheaper shading.
					if (farSpecTransValid) {
						if (d.materialId != boundMaterial
							|| boundProgram != &farSpecTrans->main) {
							if (ditheredTransp) {
								smvk::BlendSettings opaqueBlend = smvk::BlendSettings::Opaque();
								smvk::RasterSettings raster{};
								if (smvk::Material* mat = m_world->GetMaterial(d.materialId)) {
									raster = mat->pipeline.raster;
								}
								cmd.BindRenderPipeline(
									wireTwinOf(farSpecTrans->main),
									smvk::VertexFormat::StaticVertexFormat,
									opaqueDepth,
									noStencil,
									opaqueBlend,
									msaaSettings,
									wireframeRaster(raster)
								);
							}
							else {
								bindForMaterial(d.materialId, farSpecTrans->main,
									farSpecTrans->a2c.shaderCount == 2
									? farSpecTrans->a2c : noA2CProgram,
									transparentDepth,
									/*writeStencilClass=*/false);
							}
							boundMaterial = d.materialId;
							boundProgram = &farSpecTrans->main;
						}
						auto& model = m_world->scene.globalTransform[d.node];
						smvk::StaticMesh* meshDesc = LodMeshForNode(d.node, d.meshId);
						if (!meshDesc)
							continue;
						IRenderer::PushConstants objPC = pc;
						objPC.materialId = d.materialId;
						std::copy_n(glm::value_ptr(model), 16, objPC.model);
						cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &objPC, smvk::ShaderStage::All);
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
					smvk::ShaderProgram* const prog =
						&litTransProg(d.materialId);
					if (d.materialId != boundMaterial || boundProgram != prog) {
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
								wireTwinOf(*prog),
								smvk::VertexFormat::StaticVertexFormat,
								opaqueDepth,
								noStencil,
								opaqueBlend,
								msaaSettings,
								wireframeRaster(raster)
							);
						}
						else {
							bindForMaterial(d.materialId, *prog, noA2CProgram,
								transparentDepth, /*writeStencilClass=*/false);
						}
						boundMaterial = d.materialId;
						boundProgram = prog;
					}
					if (directDbg) {
						// Full push: the flat debug pushes above clobbered the
						// shared push range, so delta pushes would read garbage
						// frame addresses here.
						auto& model = m_world->scene.globalTransform[d.node];
						smvk::StaticMesh* meshDesc = LodMeshForNode(d.node, d.meshId);
						if (!meshDesc)
							continue;
						IRenderer::PushConstants objPC = pc;
						objPC.materialId = d.materialId;
						std::copy_n(glm::value_ptr(model), 16, objPC.model);
						cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &objPC, smvk::ShaderStage::All);
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
		// vertex barrier and the fetches race the CS - geometry appears and
		// vanishes "randomly" whenever workload/timing shifts (MSAA mode,
		// light toggles). Covers the meshlet debug view too (cluster-ordered
		// heap bind).
		if (m_useGfxFilter && gfxSlot != UINT32_MAX)
			m_gfxRenderer.AddMainConsumeHints(mainPass, gfxSlot);
		// Mesh-shader consume barriers (batches/clusters/params/draws for
		// the DrawMeshTasks in the record above).
		if (meshActiveForFrame)
			m_gfxRenderer.AddMeshConsumeHints(mainPass, gfxSlot);
		// Forward+ tile lists are device-written by the cull pass and
		// device-read here: explicit read hints order cull -> main and emit
		// the compute->fragment buffer barriers. With deferred shadows ON the
		// lighting pass instead reads the phase-4 outputs (final light list,
		// visibility buffer, tile state) - same fragment-stage read pattern.
		if (dsOn) {
			const smvk::DeferredShadows& ds = m_viewportResources.deferredShadows;
			smvk::DeferredShadows::AddMainReadHints(mainPass,
				ds.TileStateLogical(fb.frameIndex),
				ds.LightListLogical(fb.frameIndex),
				ds.VisibilityLogical(fb.frameIndex));
		}
		else {
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
		// Convex-volume tile masks are read by Main in BOTH shading paths
		// (cvol compositing is independent of the light-list source); the
		// volume leg of the tile cull wrote them this frame
		// (frameData.cvolTileMasks publishes only when the dispatch ran -
		// TileCullWillRun gates them together).
		if (frameData.cvolTileMasks != 0) {
			smvk::ForwardPlus::AddVolumeMaskReadHints(mainPass,
				m_viewportResources.forwardPlus.volumeTileMasksLogical);
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
		// EVSM: the main fragment shader samples the blurred moments -
		// the hint is the topological edge V-blur -> Main (without it Main
		// could be sorted - and executed - before this frame's blur, and
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
		// GPU particles - additive HDR billboards over the resolved Main color
		// (see particles.h). Runs after Main (HDR + prepass depth complete),
		// before the post chain: motion blur / bloom / exposure all pick the
		// particles up like any other HDR content. Depth-tested read-only
		// against the prepass depth (1x in both MSAA modes: MSAA Main writes
		// the transient target, the prepass wrote renderDepth - same opaque
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
			// lists are stale/empty for lights - the shader then walks a
			// capped brute-force light loop (fpTileCountX 0), and only the
			// light-buffer read hint remains (upload -> read ordering).
			const bool particleTilesUsable = m_passes.forwardPlusCull && !dsOn
				&& m_viewportResources.forwardPlus.Valid();
			if (particleTilesUsable) {
				smvk::Particles::AddDrawLightHints(particlePass,
					m_resources.fpLightsLogical,
					m_viewportResources.forwardPlus.tileCountsLogical,
					m_viewportResources.forwardPlus.tileIndicesLogical);
			}
			else {
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
				// degenerate quads in the VS (no CPU alive list - the pool is
				// fully GPU-resident; the dead-slot VS cost is the trade).
				cmd.Draw(6, PARTICLE_MAX, 0, 0);
			});
		}

		// ---------------------------------------------------------------------
		// Chrome-ball cubemap probe visual (Rendergraph/probeball.h): a
		// DEDICATED pass after Main + particles - one sphere per loaded baked
		// capture, the FS paints the probe's OWN baked cubemap onto the
		// surface (reflect(-V,N) -> TextureCube LOD 0). HDR load/store (the
		// balls composite over the scene; bloom/tonemap see them) + the
		// prepass depth TEST-ONLY (the particles' rationale: 1x in both MSAA
		// modes, occluded balls hide behind the opaque scene, the debug visual
		// never writes depth). Zero scene footprint (the editor's old chrome
		// balls were scene nodes - outliner leaks + the sky-drift bug); the
		// rows come straight from the loaded probe table.
		// Gate: the cubemap path (NOT the terrain dressing pass - the old
		// m_passes.terrain gate kept baked probes invisible whenever the
		// heightfield was off, which is the default).
		if (m_cubemapEnabled && m_cubemapShowBalls
			&& !m_cubemapProbes.empty()) {
			std::vector<smvk::probeball::ProbeBallRow> ballRows;
			ballRows.reserve(m_cubemapProbes.size());
			for (const CubemapProbeGpu& p : m_cubemapProbes) {
				smvk::probeball::ProbeBallRow row{};
				row.center = glm::vec3(p.posInfluence);
				row.influenceRadius = p.posInfluence.w;
				row.fadeRadius = p.fadeCubemap.x;
				row.cubemapIndex = int32_t(p.fadeCubemap.y);
				row.mipLevels = p.fadeCubemap.z;
				ballRows.push_back(row);
			}
			(void)m_probeBalls.AppendPass(*m_rendergraph,
				m_viewportResources.hdr.logicalView,
				m_viewportResources.renderDepth.logicalView,
				renderExtent,
				frameData.viewProj,
				m_cameraPositioner.cameraPosition_,
				ballRows.data(), uint32_t(ballRows.size()),
				m_cubemapIntensity,
				m_cubemapSampler);
		}

		// ---------------------------------------------------------------------
		// SSGI
		// Engine/Rendergraph/ssgi.h
		//
		// Runs AFTER Main + particles and BEFORE MotionBlur/DOF/SSR.
		//
		// Main exports AO-sensitive lighting through the aoTerm MRT target.
		// Particles blend additively into HDR and are not AO-attenuable.
		// Therefore, HDR is final at this point, and all later consumers
		// (MotionBlur, DOF, SSR trace radiance, volumetrics, exposure, bloom,
		// and tonemap) see the AO-applied HDR.
		//
		// This preserves the ordering of the old pre-Main chain, where Main
		// applied AO inside its ambient lighting, but now with zero temporal
		// lag and zero AO reads in the lighting pass.
		//
		// HZB:
		// Built whenever SSR or AO requires it. The SSR trace mip walk,
		// SSR blur/compose block representatives (mip1 = nearest of 2x2),
		// and AO compose center depth and tap representatives all share
		// the same depth pyramid (mip0 = exact 1:1 depth copy).
		//
		// HZB is mandatory for both chains because their shaders access it
		// unconditionally. If HZB construction fails (missing module or
		// extent exceeding the shader limit), both chains skip the frame.
		// The old SSR linear-march fallback was removed along with the
		// depth-load blur.
		//
		// AO pipeline:
		// 1. Raw: SSAO/SAO at half resolution using depth-only inputs.
		// 2. Blur: No depth loads. The (ao, repDist, octN) triple contains
		//    all required information.
		// 3. Compose: Joint bilateral filtering using depth and normals.
		//    HDR composition: hdr = hdr - aoTerm * (1 - aoEff).
		//
		// AO is disabled during debug visualization because the debug
		// fragment shaders do not write aoTerm. Target1 remains at its
		// black clear value, making the AO chain a no-op. Skipping the
		// chain avoids unnecessary processing cost.
		// ---------------------------------------------------------------------
		bool ssgiAoEmitted = false;
		uint64_t ssgiHzbAddr = 0;
		uint32_t ssgiHzbMips = 0;
		smvk::LogicalBufferHandle ssgiHzbLogical{};
		{
			SMVK_PROFILE_CATEGORY("SSGI", Optick::Category::Rendering);
			smvk::Ssgi& ssgi = m_viewportResources.ssgi;
			const bool ssrWantsHzb = m_passes.ssr && m_ssrEnabled
				&& m_viewportResources.ssr.Valid()
				&& m_ssrIntensity > 0.0f
				&& !DebugVisualizationActive()
				&& !m_viewportResources.renderDepth.logicalView.empty()
				&& m_viewportResources.renderDepth.bindlessIndex != UINT32_MAX;
			const bool aoWanted = m_passes.ssao && m_aoMode != AoMode::None
				&& m_ssaoIntensity > 0.0f
				&& !DebugVisualizationActive()
				&& ssgi.Valid();
			const bool ssgiHzbBuilt = (ssrWantsHzb || aoWanted)
				&& m_ssrHzbProgram.shaderCount == 1
				&& ssgi.hzb.Ensure(*m_context, *m_rendergraph,
					renderExtent.width, renderExtent.height)
				&& ssgi.hzb.Valid();
			if (ssgiHzbBuilt) {
				ssgiHzbAddr = ssgi.hzb.Address(*m_context);
				if (ssgiHzbAddr != 0) {
					ssgiHzbMips = ssgi.hzb.mipCount;
					ssgiHzbLogical = ssgi.hzb.logical;
					ssgi.AddHzbMipPasses(*m_rendergraph, m_ssrHzbProgram,
						ssgiHzbAddr,
						m_viewportResources.renderDepth.logicalView,
						m_viewportResources.renderDepth.bindlessIndex);
				}
			}
			if (aoWanted && ssgiHzbBuilt && ssgiHzbMips != 0) {
				smvk::SsgiDesc ssgiDesc = MakeSsgiDesc();
				ssgiDesc.radius = m_ssaoRadius;
				ssgiDesc.bias = m_ssaoBias;
				ssgiDesc.intensity = m_ssaoIntensity;
				ssgiDesc.maxDistance = m_ssaoMaxDistance;
				ssgiDesc.sampleCount = (uint32_t)(m_aoMode == AoMode::Sao
					? m_saoSamples : m_ssaoSamples);
				ssgiDesc.spiralTurns = (uint32_t)m_saoSpiralTurns;
				ssgiDesc.blurSharpness = m_ssaoBlurSharpness;
				ssgiDesc.nearPlane = frameData.nearPlane;
				// Shared G-buffer (joint-bilateral center normal): the
				// prepass wrote it when SSR OR AO wanted it (fc fill
				// above); absent under MSAA - the compose falls back to
				// depth-only edge weights.
				const bool ssgiNormalOn = fc.ssrGbufferWanted
					&& m_viewportResources.ssr.gbuffer.Valid();
				ssgi.AddAoPasses(
					*m_rendergraph,
					*m_context,
					m_aoMode == AoMode::Sao
					? smvk::SsgiAoMode::Sao : smvk::SsgiAoMode::Ssao,
					m_viewportResources.renderDepth.logicalView,
					m_viewportResources.renderDepth.bindlessIndex,
					frameUniformsAddress,
					linearSampler,
					m_viewportResources.hdr.logicalView,
					m_viewportResources.hdr.bindlessIndex,
					ssgiNormalOn ? m_viewportResources.ssr.gbuffer.logical
					: smvk::LogicalImageViewHandle{},
					ssgiNormalOn ? m_viewportResources.ssr.gbuffer.bindlessIndex
					: UINT32_MAX,
					ssgiDesc,
					"SsgiAo",
					"SsgiAoBlur",
					"SsgiAoCompose");
				ssgiAoEmitted = true;
			}
		}

		// ---------------------------------------------------------------------
		// Motion blur - velocity gather in HDR (see motionblur.h).
		//
		// Runs after Main + SSGI and before Bloom/Tonemap, ensuring both
		// downstream passes consume the blurred HDR.
		//
		// When tile dilation is enabled, two small compute prepasses run
		// before the gather:
		// 1. Per-tile maximum velocity calculation.
		// 2. 5x5 velocity dilation.
		//
		// This allows revealed background pixels to streak with the
		// foreground objects that previously covered them.
		// ---------------------------------------------------------------------

		// Scene HDR for downstream passes:
		//
		// Uses the AO-applied HDR target when SSGI compose has run, since
		// the compose pass writes HDR into its own ping-pong target.
		//
		// Otherwise, the original sharp HDR target is used.
		//
		// MotionBlur and DOF may further update this target.
		// No additional copy is performed when these effects are disabled.
		smvk::LogicalImageViewHandle sceneHdrView = ssgiAoEmitted
			? m_viewportResources.ssgi.OutputView()
			: m_viewportResources.hdr.logicalView;
		uint32_t sceneHdrBindless = ssgiAoEmitted
			? m_viewportResources.ssgi.OutputBindless()
			: m_viewportResources.hdr.bindlessIndex;
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
					sceneHdrView,
					sceneHdrBindless,
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
		// Depth of field - HDR CoC blur (see dof.h). Runs after Main +
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
		// Transparency handling:
		// Transparency now use the opaque surface visible through them.
		// The SSR trace receives opaque depth and the thin G-buffer's
		// seen-through surface values.
		//
		// See ssr.h and gbuffer.frag.hlsl.
		// ---------------------------------------------------------------------

		// ---------------------------------------------------------------------
		// Screen-space reflections - SSR (see ssr.h).
		//
		// Runs after DOF and before Exposure/Bloom/Tonemap, ensuring
		// reflections contribute to scene exposure, bloom, and tonemapping.
		//
		// SSR pipeline:
		// 1. Half-resolution reflection trace.
		// 2. Bilateral blur.
		// 3. Full-resolution reflection composite.
		//
		// Surface normals and Fresnel are reconstructed using:
		// 1. Full-resolution prepass depth (no full G-buffer required).
		// 2. Thin G-buffer pane layer for transparent pane receivers.
		// 3. Receiver-material stencil written during the prepass.
		//
		// See Rendergraph/stencil_bits.h for stencil definitions.
		//
		// Debug visualization:
		//
		// SSR is disabled when DebugVisualizationActive is enabled,
		// since reflections would otherwise composite additively over
		// the debug visualization output.
		//
		// Skipping SSR also invalidates its temporal history.
		//
		// When normal rendering resumes, the first lit frame initializes
		// a fresh temporal history instead of blending stale reflection
		// data from before the debug view.
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
				// SSSR trace Hi-Z: Uses the shared SSGI pyramid, built above in the
				// SSGI block (after particles) whenever SSR or AO needs it.
				// See smvk::Ssgi::hzb (Engine/Rendergraph/ssgi.h).
				//
				// SSR blur and composite now dereference the pyramid unconditionally.
				// Their depth reads all use mip0/mip1 BDA scalars, so the SSR chain
				// skips the frame if the HZB build fails (module missing or extent
				// exceeds the shader limit).
				//
				// The old linear-march fallback without HZB was removed when
				// depth-load blur and composite were introduced.
				const uint64_t ssrHzbAddr = ssgiHzbAddr;
				const uint32_t ssrHzbMips = ssgiHzbMips;
				const smvk::LogicalBufferHandle ssrHzbLogical = ssgiHzbLogical;
				const uint32_t ssrHzbMip1Offset =
					m_viewportResources.ssgi.hzb.Mip1Offset();
				const uint32_t ssrHzbMip1Stride =
					m_viewportResources.ssgi.hzb.Mip1Stride();
				if (ssrHzbAddr == 0 || ssrHzbMips == 0 || ssrHzbLogical.empty()
					|| ssrHzbMip1Stride == 0) {
					ssrEmitted = false;
				}
				else {
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
							ssrHzbMip1Offset,
							ssrHzbMip1Stride,
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
					}
					else {
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
							ssrHzbMip1Offset,
							ssrHzbMip1Stride,
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
					// so this pass owns raw's final content for the dump frames -
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
							}
							catch (...) {
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
						// VULKANTOY_SSR_STENCILVIEW): Loads the receiver-material stencil
						// through the exact uintTextures[] path used by the trace shader
						// and writes it into the blurred target (no longer used after
						// composite).
						//
						// R = raw stencil byte (0..255).
						// G = decoded STENCIL_SSR flag.
						// B = decoded STENCIL_SSR_METAL flag.
						//
						// The ssr_blur dump slot then shows the stencil exactly as SSR
						// sees it, without relying on the layout-fragile raw depth/stencil
						// readback path.
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
								}
								catch (...) {
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
				} // (no-HZB skip: ssrOn but the shared pyramid failed - see above)
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
			volDesc.useCvolGi = m_volumetricUseCvolGi ? 1u : 0u;
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
				// Forward+ tile lists for the fog's in-scatter leg: published
				// under the cvol-mask freshness contract (TileCullWillRun -
				// same dispatch writes both, so frameData.cvolTileCountX != 0
				// in the shader implies these are fresh too). Zeros = the
				// shader walks [0, fpLightCount) directly instead.
				uint64_t volFpTileCounts = 0, volFpTileIndices = 0;
				if (m_viewportResources.forwardPlus.Valid() && TileCullWillRun()) {
					volFpTileCounts =
						m_viewportResources.forwardPlus.TileCountsAddress(*m_context);
					volFpTileIndices =
						m_viewportResources.forwardPlus.TileIndicesAddress(*m_context);
				}
				auto& volPass = m_viewportResources.volumetric.AddVolumetricPass(
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
					"Volumetric",
					volFpTileCounts,
					volFpTileIndices,
					fpLightsAddress,
					fpLightCount,
					FP_MAX_PER_TILE);
				// Convex-volume tile masks: the fog march reads the per-tile
				// volume bitmask (fragment) after the tile cull wrote it
				// (compute) - order the edge (same freshness contract as the
				// Main hint: the publish gate and the dispatch gate agree).
				if (frameData.cvolTileMasks != 0) {
					smvk::ForwardPlus::AddVolumeMaskReadHints(volPass,
						m_viewportResources.forwardPlus.volumeTileMasksLogical);
				}
				// Forward+ tile lists: same cull->fog edge for the in-scatter
				// leg (published iff the cull ran - see above).
				if (volFpTileCounts != 0) {
					smvk::ForwardPlus::AddTileReadHints(volPass,
						m_viewportResources.forwardPlus.tileCountsLogical,
						m_viewportResources.forwardPlus.tileIndicesLogical);
				}
				sceneHdrView = m_viewportResources.volumetric.OutputView();
				sceneHdrBindless = m_viewportResources.volumetric.OutputBindless();
				volumetricEmitted = true;
			}
		}

		// ---------------------------------------------------------------------
		// Exposure histogram
		//
		// Log2-luminance meter for the scene HDR (see exposure.h).
		// Runs before Bloom/Tonemap so it measures exactly what they consume.
		//
		// UpdateAutoExposure() downloads and adapts first (this slot was
		// retired at Acquire). The CPU then zeroes the buffer (transfer write),
		// and the compute pass accumulates histogram data for a future
		// frame's tonemap and bloom.
		// ---------------------------------------------------------------------
		{
			SMVK_PROFILE_CATEGORY("ExposureHistogram", Optick::Category::Rendering);
			auto& meter = m_viewportResources.exposure;
			// ExposureMeteringActive() also handles the debug-view / DS-viz
		// exposure freeze. Without it, the histogram compute and zero-fill
		// continued running under debug views, accumulating the debug
		// palette's exposure-compensated colors (pre-scaled by ~1/0.001,
		// making them far brighter than the actual scene).
		//
		// As a result, the first histogram poll after leaving the debug
		// view adapted against those bins, causing a visible dip toward
		// minimum exposure.
		//
		// Freezing the entire metering pass instead preserves the last
		// real scene accumulation in the histogram ring.
			const bool meterGate = m_passes.exposure && m_autoExposureEnabled &&
				meter.Valid() && meter.ShadersValid() &&
				!sceneHdrView.empty() && sceneHdrBindless != UINT32_MAX &&
				renderExtent.width != 0 && renderExtent.height != 0 &&
				ExposureMeteringActive();
			// One-run diagnostic (throttled): WHY the meter is quiet.
			{
				static uint64_t s_expRecTick = 0;
				const uint64_t tick = s_expRecTick++;
				if (!meterGate && (tick < 5 || tick % 600 == 0)) {
					SMVK_TRACE_LOG(
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
							SMVK_TRACE_LOG(
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
		// Bloom - HDR downsample / upsample chain (composable, see bloom.h)
		// ---------------------------------------------------------------------
		{
			SMVK_PROFILE_CATEGORY("Bloom", Optick::Category::Rendering);
			// Disabled during any debug visualization (DebugVisualizationActive).
			// Bloom composites additively over the flat debug readout,
			// contaminating it just like SSR or fog would.
			//
			// Disabling bloom during debug visualization keeps the output clean
			// and consistent with the other rendering effects.
			if (m_passes.bloom && m_bloomEnabled && m_viewportResources.bloom.Valid() &&
				!DebugVisualizationActive()) {
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
		// DLSS - Streamline super resolution (see Engine/Rendergraph/dlss.h).
		//
		// Upscales HDR from render resolution to swapchain resolution,
		// replacing the tonemapper's bilinear upscale.
		//
		// After this block, sceneHdrView and sceneHdrBindless reference
		// the upscaled HDR output, which the tonemapper samples at swapExtent.
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
				|| m_dlssLastSwapExtent.width != outExtent.width
				|| m_dlssLastSwapExtent.height != outExtent.height;

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
			}
			else if (ssrEmitted) {
				dlssColorImage = static_cast<smvk::ImageHandle>(m_viewportResources.ssr.image);
				dlssColorView = static_cast<smvk::ImageViewHandle>(m_viewportResources.ssr.view);
			}
			else if (dofEmitted) {
				dlssColorImage = static_cast<smvk::ImageHandle>(m_viewportResources.dof.output.image);
				dlssColorView = static_cast<smvk::ImageViewHandle>(m_viewportResources.dof.output.view);
			}
			else {
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
			dlssIn.outputExtent = outExtent;
			dlssIn.colorFormat = m_viewportResources.hdr.format;
			dlssIn.depthFormat = m_viewportResources.renderDepth.format;
			dlssIn.mvecFormat = m_viewportResources.motionVectors.target.format;
			dlssIn.exposureFormat = m_viewportResources.dlss.exposure.format;

			// Constants: ROW-major, UNJITTERED matrices (SL convention).
			//
			// clipToPrevClip = prevUnjitteredVP * inverse(currUnjitteredVP)
			//
			// Maps a current clip-space point to the previous frame's clip space.
			// Uses the same camera history as the motion-vector pass, but without
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
		// Tonemapper - HDR (RGBA16F) to LDR swapchain (see tonemap.h).
		//
		// With SMAA active, the pass renders to the offscreen LDR target
		// owned by the SMAA builder, since the swapchain cannot be sampled.
		//
		// The SMAA resolve pass then writes the final result to the swapchain.
		// ---------------------------------------------------------------------
		const bool smaaActive = SmaaActive();
		const bool smaaT2x = smaaActive && m_aaMode == AaMode::SmaaT2x;
		const bool swapIsSrgb = FrameSwapIsSrgb(fb);
		{
			SMVK_PROFILE_CATEGORY("Tonemap", Optick::Category::Rendering);
			const bool bloomOn = m_passes.bloom && m_bloomEnabled && m_viewportResources.bloom.Valid() &&
				!DebugVisualizationActive();
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
				EnsureCaptureTarget(outExtent);
				captureThisFrame = !m_captureLogical.empty();
			}
			smvk::TonemapDesc tonemapDesc = m_viewportResources.tonemap.desc;
			tonemapDesc.exposure = frameExposure;
			tonemapDesc.tonemapMode = m_tonemapMode;
			// Layer write: confined to the element's rect (Load preserves
			// sibling layers for sub-rects; full-swap keeps Clear).
			auto& tonemapPass = m_viewportResources.tonemap.AddTonemapPass(
				*m_rendergraph,
				sceneHdrView,
				sceneHdrBindless,
				bloomView,
				bloomBindless,
				bloomIntensity,
				linearSampler,
				tonemapTargets,
				outExtent,
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
				m_jitterDeltaUV.y,
				outX,
				outY,
				outLoad);
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
					outExtent,
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
		// SMAA - LDR post-tonemap antialiasing (see Engine/Rendergraph/smaa.h).
		//
		// Performs luma edge detection, calculates blend weights, and applies
		// neighborhood blending, followed by the T2x temporal resolve using
		// the velocity buffer.
		// ---------------------------------------------------------------------
		if (smaaActive) {
			SMVK_PROFILE_CATEGORY("SMAA", Optick::Category::Rendering);
			m_smaaFrameRanT2x = smaaT2x;
			// Official T2x subsample table (@SUBSAMPLE_INDICES in smaa.hlsli).
			//
			// Phase A/B: (1,1,1,0) / (2,2,2,0).
			// SMAA 1x: (0,0,0,0).
			//
			// Phase, slot, and valid are taken from this record's global snapshot,
			// ensuring all three values remain consistent.
			float subsample[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			if (smaaT2x && m_smaaFrameParams.parity == 0) {
				subsample[0] = subsample[1] = subsample[2] = 1.0f;
			}
			else if (smaaT2x) {
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
				outExtent,
				swapIsSrgb,
				m_jitterDeltaUV.x,
				m_jitterDeltaUV.y,
				nullptr,
				outX,
				outY);
		}

		// ---------------------------------------------------------------------
		// ImGui - Build the UI before adding the rendergraph pass.
		// ---------------------------------------------------------------------
		if (m_passes.ui && m_imgui && m_imgui->IsInitialized() && m_viewportResources.ui.Valid()) {
			// Target extent != window size when ui scale != 1: the overlay maps
			// ImGui draw data + font raster density onto the UI target.
			m_imgui->NewFrame(uiExtent);

			// Fixed debug panel: full-height side bar on the Right (fixed
			// side - dock cycling removed; the top-left corner stays free
			// for the text stats HUD).
			// Position + size enforced every frame (NoMove/NoResize/NoCollapse):
			// never floating, never lost behind the scene. Content is classified
			// into Quality (settings) / Debug (views + overlays) / Stats (readback)
			// tabs; live numbers live in the text HUD (Ctrl+T) — the panel
			// keeps controls, explanations and detail breakdowns.
			{
				const ImVec2 disp = ImGui::GetIO().DisplaySize;
				const float pad = 8.0f;
				const float sideW = 480.0f;
				ImGui::SetNextWindowPos(ImVec2(disp.x - sideW - pad, pad), ImGuiCond_Always);
				ImGui::SetNextWindowSize(ImVec2(sideW, disp.y - pad * 2.0f), ImGuiCond_Always);
			}
			ImGui::Begin("VulkanToy - Debug", nullptr,
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
			{
				// Live FPS/CPU/GPU readouts live in the text HUD (Ctrl+T)
				// now (DrawHudOverlay); the panel keeps controls only.
				if (ImGui::Button(m_showHud ? "Hide stats HUD (Ctrl+T)" : "Show stats HUD (Ctrl+T)"))
					m_showHud = !m_showHud;
			}
			if (ImGui::BeginTabBar("VulkanToyTabs")) {
				// ---------------------------------------------------------
				// Quality: every setting that changes the image. No readback
				// here besides the control itself (numbers live in Stats).
				// ---------------------------------------------------------
				if (ImGui::BeginTabItem("Quality")) {
					// Group by rendering concern instead of implementation/pass order.
					// Environment owns world lighting/transport: sky, GI/SSGI, reflections and fog.
					if (ImGui::CollapsingHeader("Display & Image Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
						if (ImGui::CollapsingHeader("Presentation & Resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
							// VSync / presentation mode. Applies on the next frame
							// via the HOST's rebuild check (App::SetPresentMode ->
							// the DrawFrame pump recreates); the active mode may
							// fall back when the driver lacks the requested one.
							// FIFO is strict (missed 16.6ms -> 30fps lock); Mailbox
							// stays tear-free at 60Hz; FIFO relaxed vsyncs on time
							// and never locks to half rate.
							const char* modeNames[] = { "VSync FIFO (strict)", "VSync Mailbox (smooth)",
								"Uncapped Immediate", "VSync FIFO relaxed (no 30fps lock)" };
							int modeIdx = static_cast<int>(m_host.GetPresentMode());
							if (ImGui::Combo("Present mode", &modeIdx, modeNames, 4))
								SetPresentMode(static_cast<smvk::PresentMode>(modeIdx));
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
						if (ImGui::CollapsingHeader("Upscaling - DLSS (Streamline)")) {
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
							}
							else if (DlssRequested()) {
								// Why the requested mode is not running.
								if (!smvk::DlssAvailable())
									ImGui::TextDisabled("Streamline runtime (sl.interposer.dll) not loaded.");
								else if (!m_dlssSupported)
									ImGui::TextDisabled("DLSS not supported on this adapter (NVIDIA GPU + up-to-date driver).");
								else if (!m_passes.motionVectors)
									ImGui::TextDisabled("Requires the motion-vector pass (ScenePassConfig).");
								else
									ImGui::TextDisabled("Warming up / options pending this frame.");
							}
							else {
								ImGui::TextDisabled("Off: render scale slider drives the internal resolution\n"
									"(tonemap upscales bilinearly).");
							}
						}
						if (ImGui::CollapsingHeader("Antialiasing, Transparency & Filtering")) {
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
							if (ImGui::Combo("Transparency", &transpIdx, transpModes, 2))
								m_transparentMode = (transpIdx == 1)
								? smvk::TransparentMode::Blended
								: smvk::TransparentMode::Dithered;
							if (transpIdx == 0)
								ImGui::TextDisabled("Screen-door: depth-writing, order-independent (SMAA T2x integrates it).");
							else
								ImGui::TextDisabled("Classic back-to-front blend: no depth write, sorted.");
							// Texture filtering (renderer state): trilinear is the
							// lowest option; aniso Nx adds anisotropic filtering
							// (maxAnisotropy N, clamped to the device limit).
							// Applies to the bindless texture samplers (slots 0 + 2)
							// via in-place recreate.
							{
								const char* filtNames[] = { "Trilinear", "Anisotropic 2x",
									"Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x" };
								int filtIdx = static_cast<int>(m_textureFiltering);
								if (ImGui::Combo("Texture filtering", &filtIdx, filtNames, 5))
									SetTextureFiltering(static_cast<TextureFiltering>(
										std::clamp(filtIdx, 0, 4)));
								if (filtIdx == 0)
									ImGui::TextDisabled("Trilinear: smooth mip blends (lowest option).");
								else
									ImGui::TextDisabled("Anisotropic: sharper textures at grazing angles.");
							}
						}
					}
					if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
						if (ImGui::CollapsingHeader("Sky & Sun", ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::Checkbox("Sky dome (K)", &m_skyEnabled);
							ImGui::Checkbox("Terrain field", &m_terrainEnabled);
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip(
									"The default 4 km x 4 km tiled heightfield (100 m tiles,\n"
									"visual-only dressing rendered after the sky). Near tiles\n"
									"draw with the full terrain format (pos/normal/tangent/color),\n"
									"far tiles with the low format (pos/color off the same buffer).");
							ImGui::TextDisabled("Ambient source: %s (N cycles; GI section)",
								AmbientModeName(m_ambientMode));
							// ---- Environment zones (world/env separation; see
							// Rendergraph/envzones.h): the RGBA8 weight map (10 m per
							// texel over the 4 km world, R/G/B/A = zones 0..3) blends
							// each zone's sky tint/ambient, fog and volumetrics across
							// the terrain + the volumetric fog. ZONES ARE ENTITIES
							// (scene.envForNode nodes): coverage from the node
							// transform (position/rotation placement, scale = half
							// extents, an INFINITE component = an unbounded slab),
							// the look from the env block; the map DERIVES from them
							// (RebuildEnvZoneMap - load, rebind, editor refresh).
							// No brush: zone shapes are authored, never painted.
							if (ImGui::CollapsingHeader("Zones (entity world/env)")) {
								ImGui::Checkbox("Zones enabled", &m_envZonesEnabled);
								if (!m_envZones.Valid()) {
									ImGui::TextDisabled("Zone map not built - the plain env path renders.");
								}
								else {
									size_t zoneCount = 0;
									if (m_world) {
										for (const auto& [node, env]
											: m_world->scene.envForNode) {
											++zoneCount;
											const glm::mat4* g =
												node < m_world->scene
												.globalTransform.size()
												? &m_world->scene
												.globalTransform[node]
												: nullptr;
											ImGui::BulletText("zone %u: '%s' prio=%.0f"
												" half=(%s, %s, %s)",
												node,
												getNodeName(m_world->scene,
													int(node)).c_str(),
												env.zonePriority,
												g && std::isfinite(glm::length(
													glm::vec3((*g)[0])))
												? std::to_string(glm::length(
													glm::vec3((*g)[0])))
												.c_str() : "INF",
												g && std::isfinite(glm::length(
													glm::vec3((*g)[1])))
												? std::to_string(glm::length(
													glm::vec3((*g)[1])))
												.c_str() : "INF",
												g && std::isfinite(glm::length(
													glm::vec3((*g)[2])))
												? std::to_string(glm::length(
													glm::vec3((*g)[2])))
												.c_str() : "INF");
										}
									}
									ImGui::TextDisabled("Zone entities: %u (top 3 by priority,"
										" then innermost, feed the map's channels 1..3;"
										" channel 0 = the resolved env). Scale = half"
										" extents; an INFINITE axis = an unbounded slab.",
										(unsigned)zoneCount);
								}
							}
							if (!m_world) {
								ImGui::TextDisabled("No world - nothing to author.");
							}
							else {
								SceneEnv& se = m_world->scene.environment;
								auto authored = [&]() { m_world->scene.hasEnvironment = true; };
								ImGui::ColorEdit3("Sky color", &se.skyColor.x);
								ImGui::DragFloat("Sky intensity (lux)", &se.skyIntensity,
									5.0f, 0.0f, 10000.0f, "%.0f");
								if (ImGui::IsItemHovered())
									ImGui::SetTooltip("Lux-native scene environment (no conversions).");
								ImGui::Separator();
								ImGui::Text("Sun (drives direct light + sky disc)");
								if (ImGui::SliderFloat("Azimuth", &se.sunAzimuth, 0.0f, 360.0f)) authored();
								if (ImGui::SliderFloat("Elevation", &se.sunElevation, 0.0f, 90.0f)) authored();
								if (ImGui::ColorEdit3("Sun color", &se.sunColor.x)) authored();
								ImGui::DragFloat("Sun intensity (lux)", &se.sunIntensity,
									10.0f, 0.0f, 2e5f, "%.0f");
							}
							if (!m_skydome.ShadersValid())
								ImGui::TextDisabled("Sky program missing - dome skipped");
						}
						if (ImGui::CollapsingHeader("Global Illumination + SSGI / AO", ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::TextDisabled("Ambient source: %s (N cycles)", AmbientModeName(m_ambientMode));
							ImGui::SliderFloat("GI intensity", &m_giIntensity, 0.0f, 2.0f, "%.2f");
							if (!m_giLoaded && !m_regLoaded)
								ImGui::TextDisabled("No baked GI: content/gi/%s.* missing",
									m_sceneId.empty() ? "?" : m_sceneId.c_str());

							ImGui::Separator();
							ImGui::Text("Screen-space ambient occlusion / SSGI chain");
							{
								static const char* const kAoNames[3] = { "No AO", "SSAO", "SAO" };
								int aoIdx = static_cast<int>(m_aoMode);
								if (ImGui::Combo("AO mode (O)", &aoIdx, kAoNames, 3))
									m_aoMode = static_cast<AoMode>(std::clamp(aoIdx, 0, 2));
								ImGui::TextDisabled("O cycles AO: %s (half-res; post-Main SSGI chain)",
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
								}
								else {
									ImGui::SliderInt("SAO samples", &m_saoSamples, 1, 32);
									ImGui::SliderInt("SAO spiral turns", &m_saoSpiralTurns, 1, 16);
								}
							}
						}
						if (ImGui::CollapsingHeader("Reflection Probes / Cubemaps")) {
							ImGui::Checkbox("Cubemap specular IBL", &m_cubemapEnabled);
							ImGui::SliderFloat("Cubemap intensity", &m_cubemapIntensity, 0.0f, 4.0f, "%.2f");
							ImGui::SliderFloat("Cubemap LOD bias", &m_cubemapLodBias, -2.0f, 4.0f, "%.2f");
							ImGui::Checkbox("Probe locations overlay", &m_cubemapShowProbes);
							if (m_cubemapShowProbes)
								ImGui::SliderFloat("Overlay max dist (m)", &m_cubemapOverlayMaxDist, 5.0f, 300.0f, "%.0f");
							ImGui::Checkbox("Chrome balls (lit probe visual)", &m_cubemapShowBalls);
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip(
									"One lit chrome sphere per loaded baked capture,\n"
									"sampling ITS OWN cubemap in the reflection direction\n"
									"(radius reads the influence at a glance). Render-pass\n"
									"visual - no scene nodes, never picked, never cooked.");
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
							size_t streamResident = 0, streamOut = 0, streamFailed = 0;
							for (const CubemapRow& row : m_cubemapRows) {
								if (row.failed)
									++streamFailed;
								else if (m_cubemaps.Find(row.entryName))
									++streamResident;
								else
									++streamOut;
							}
							ImGui::Text("Cubemaps: %u resident / %u rows (baked %u, procedural %u)  Probes: %u (light tail)  Sampler: %u",
								(unsigned)m_cubemaps.entries.size(),
								(unsigned)m_cubemapRows.size(),
								(unsigned)bakedCount, (unsigned)procCount,
								(unsigned)ProbeEntryCount(), m_cubemapSampler);
							if (!m_cubemapRows.empty()) {
								int streamBudget = int(m_cubemapStreamBudget);
								if (ImGui::SliderInt("Stream budget (nearest rows)",
									&streamBudget, 0, 64))
									m_cubemapStreamBudget =
									uint32_t(std::clamp(streamBudget, 0, 64));
								ImGui::TextDisabled("Probe textures stream by camera distance"
									" (nearest-N + covering + pinned stay resident);"
									" failed rows never retry.");
								if (streamFailed > 0)
									ImGui::TextDisabled("stream failed rows: %u (see [Cubemap] log)",
										(unsigned)streamFailed);
								size_t listedOut = 0;
								for (const CubemapRow& row : m_cubemapRows) {
									if (listedOut >= 8)
										break;
									if (row.failed || m_cubemaps.Find(row.entryName))
										continue;
									ImGui::BulletText("streamed out: %s", row.entryName.c_str());
									++listedOut;
								}
								if (streamOut > listedOut)
									ImGui::TextDisabled("... +%u more streamed out",
										(unsigned)(streamOut - listedOut));
							}
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
							ImGui::TextDisabled("Probe textures stream by distance (see [Cubemap] log lines);"
								" rows/intensity/debug/LOD apply instantly.");
						}
						if (ImGui::CollapsingHeader("Screen-Space Reflections (SSR)")) {
							ImGui::Checkbox("Reflections (SSGI composite)", &m_ssrEnabled);
							if (m_ssrEnabled) {
								ImGui::SliderInt("Ray steps", &m_ssrSteps, 1, 128);
								ImGui::SliderInt("Refine steps", &m_ssrRefineSteps, 0, 8);
								ImGui::SliderFloat("Max distance (m)", &m_ssrMaxDistance, 1.0f, 200.0f);
								ImGui::SliderFloat("Thickness (m)", &m_ssrThickness, 0.01f, 2.0f);
								ImGui::SliderFloat("Intensity", &m_ssrIntensity, 0.0f, 4.0f);
								ImGui::SliderFloat("F0 (glass 0.04)", &m_ssrF0, 0.0f, 1.0f);
								ImGui::SliderFloat("Bilateral sharpness", &m_ssrBlurSharpness, 0.0f, 32.0f);
							}
							// Temporal (TAA) accumulation is parked: taaUsable is
							// hard-false in the record while the smear diagnosis is
							// open, so no widgets - the state machine + programs stay
							// for the re-enable (flip taaUsable back).
							ImGui::TextDisabled("Half-res trace + bilateral blur + full-res composite. All depth\n"
								"reads (trace skip, blur/composite block reps) go through the shared\n"
								"final-depth MAX-HZB the SSGI block builds; the pyramid is required -\n"
								"frames without it skip reflections. Bistro shopfront glass at grazing\n"
								"angles is the test case.");
						}
						if (ImGui::CollapsingHeader("Fog & Volumetrics", ImGuiTreeNodeFlags_DefaultOpen)) {
							bool volumetricSelected = false;
							if (!m_world) {
								ImGui::TextDisabled("No world - fog mode cannot be authored.");
								m_volumetricEnabled = false;
							}
							else {
								SceneEnv& se = m_world->scene.environment;
								auto authored = [&]() { m_world->scene.hasEnvironment = true; };
								const char* tiers[3] = {
									"Off", "Analytic (exp2, fallback)", "Volumetric (raymarched GI-lit)"
								};
								if (ImGui::Combo("Fog mode", &se.fogQuality, tiers, 3))
									authored();

								// Fog mode is the single source of truth for the active path.
								volumetricSelected = se.fogQuality == 2;
								// Headless override wins (VULKANTOY_VOLUMETRIC): the
								// AGENTS.md contract is "scene fog settings lose" -
								// without this the env force-on/off dies here on
								// every frame (tier mismatch with the override).
								// Cached: the env cannot change mid-process.
								static const bool sVolumetricEnvPinned =
									std::getenv("VULKANTOY_VOLUMETRIC") != nullptr;
								if (!sVolumetricEnvPinned)
									m_volumetricEnabled = volumetricSelected;

								if (se.fogQuality == 1) {
									if (ImGui::ColorEdit3("Fog color", &se.fogColor.x)) authored();
									if (ImGui::SliderFloat("Fog density", &se.fogDensity, 0.0f, 1.0f)) authored();
								}
							}

							if (volumetricSelected) {
								static const char* const kVolNames[5] = {
									"Low (8 steps)", "Medium (16 steps)", "High (32 steps)",
									"Ultra (64 steps)", "Custom"
								};
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
								ImGui::Checkbox("Convex volumes in fog GI", &m_volumetricUseCvolGi);
								ImGui::SliderFloat("Dither (banding guard)", &m_volumetricJitter, 0.0f, 1.0f);
								ImGui::SliderFloat("Height falloff (1/m)", &m_volumetricHeightFalloff, 0.0f, 1.0f, "%.3f");
								if (m_volumetricHeightFalloff > 0.0f)
									ImGui::SliderFloat("Height base (m)", &m_volumetricHeightBase, -10.0f, 30.0f);
								if (!m_giLoaded)
									ImGui::TextDisabled("No probe volume: ambient falls back to sky IBL.");
								ImGui::TextDisabled("Probes carry sky+sun bounce; sun adds HG forward scatter.");
							}
						}
					}
					if (ImGui::CollapsingHeader("Lighting & Shadows")) {
						if (ImGui::CollapsingHeader("Forward+ Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
							// The scene's authored lights always render when
							// present (no enable toggle): the Forward+ set is
							// whatever the cooked KHR_lights_punctual pack
							// carries, static placement.
							ImGui::TextDisabled("Cooked: %u point/spot%s",
								(unsigned)m_cookedLights.pointSpots.size(),
								m_cookedLights.hasSun ? " + sun" : "");
						}
						if (ImGui::CollapsingHeader("Directional Shadows (CSM)")) {
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
							ImGui::Checkbox("SDSM (fit splits to measured depth)", &m_sdsmEnabled);
							ImGui::TextDisabled("GPU depth stats (min/max + 64-bin log histogram, decimated)\n"
								"every frame; own async readback -> 1-frame latency; 1%%/99%% percentile\n"
								"range + hysteresis (instant expand, slow contract) + 0.25 m quantization.\n"
								"Camera teleports discard history (heuristic cooldown 2 frames).");
							if (m_sdsmEnabled && m_sdsmStatsValid) {
								ImGui::TextDisabled("measured %.2f..%.2f m (pct %.2f..%.2f) -> applied %.2f..%.2f m%s",
									m_sdsmMin, m_sdsmMax, m_sdsmPctMin, m_sdsmPctMax,
									m_sdsmAppliedValid ? m_sdsmAppliedMin : 0.0f,
									m_sdsmAppliedValid ? m_sdsmAppliedMax : 0.0f,
									m_sdsmCutCooldown > 0 ? " (cut cooldown: heuristic)" : "");
							}
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
						if (ImGui::CollapsingHeader("Spot Shadows")) {
							ImGui::Checkbox("Spot shadows (per-light flag)", &m_spotShadowsEnabled);
							int spotMax = m_spotShadowMax;
							if (ImGui::SliderInt("Shadow spots max", &spotMax, 0, (int)smvk::kMaxSpotShadows))
								m_spotShadowMax = spotMax;
							ImGui::SliderFloat("Shadow max distance (m)", &m_spotShadowMaxDistance, 5.0f, 120.0f);
						}
						if (ImGui::CollapsingHeader("Shadow Filtering (EVSM)")) {
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
					}
					if (ImGui::CollapsingHeader("Post Processing")) {
						if (ImGui::CollapsingHeader("Exposure & Tonemapping", ImGuiTreeNodeFlags_DefaultOpen)) {
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
						if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::Checkbox("Bloom (B)", &m_bloomEnabled);
							if (m_bloomEnabled) {
								ImGui::SliderFloat("Bloom intensity", &m_bloomIntensity, 0.0f, 2.0f, "%.2f");
								ImGui::SliderFloat("Bloom threshold (HDR)", &m_bloomThreshold, 0.0f, 4000.0f, "%.0f");
								ImGui::TextDisabled("Threshold is pre-exposure HDR (sun-lit diffuse spans\n"
									"~0-1600); knee tracks at 0.25x. Lower it until highlights glow.");
							}
						}
						if (ImGui::CollapsingHeader("Motion Blur")) {
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
						if (ImGui::CollapsingHeader("Depth of Field")) {
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
						}
						if (ImGui::CollapsingHeader("Color Grading / LUT")) {
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
							ImGui::TextDisabled("F7: export neutral strip + screenshot to exports/.\n"
								"PS: grade the 1024x32 strip, save PNG into content/luts/.");
						}
					}
					if (ImGui::CollapsingHeader("Scene Effects")) {
						if (ImGui::CollapsingHeader("Particles (GPU Emitters)", ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::Checkbox("Enable particles (F)", &m_particlesEnabled);
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
						if (ImGui::CollapsingHeader("Decals (Splats)", ImGuiTreeNodeFlags_DefaultOpen)) {
							// Forward+ tiled decals: per-tile lists refreshed by the
							// same cull dispatch as the lights; splats re-derive
							// every frame so these sliders retouch live decals.
							ImGui::Checkbox("Show decals", &m_decalsEnabled);
							ImGui::SameLine();
							ImGui::TextDisabled(DecalsActive() ? "(active)" : "(off)");
							ImGui::Text("Entities: %u (world) / %u uploaded",
								m_world ? (unsigned)m_world->decalLocations.size() : 0u,
								m_decalFrameCount);
							if (ImGui::Button("Splat at crosshair (J)"))
								SpawnDecalAtCrosshair();
							ImGui::TextDisabled("J raycasts the crosshair (pick BVH builds once,"
								" %s)", m_decalPickState == 1 ? "ready"
								: m_decalPickState == 0 ? "first press builds it"
								: "unavailable for this scene");
							ImGui::TextDisabled("Decals are WORLD entities (cluster pools); looks"
								" are per-decal, authored with the entity.");
						}
					}
					if (ImGui::CollapsingHeader("Renderer & Streaming")) {
						if (ImGui::CollapsingHeader("Direct Lighting Path")) {
							// Deferred direct lighting (Square-Enix-style tiled/zBin pipeline):
							// this switches the WHOLE direct-light path, not just shadow
							// filtering - on = shade from the deferred per-tile light lists,
							// off = legacy inline forward+ path (tile cull + per-pixel loop).
							// A/B the two against each other; the counters below are the
							// retired-slice peaks (~2 frames old, see Stats).
							ImGui::Checkbox("Enable (H)", &m_dsEnabled);
							ImGui::SameLine();
							ImGui::TextDisabled(m_passes.deferredShadows ? "" : "(pass disabled)");
							ImGui::TextDisabled(DsActive()
								? "shading from the deferred lists (forward tile light leg idle)"
								: "shading inline forward+ (deferred phases skipped)");
						}
						if (ImGui::CollapsingHeader("GPU Culling / GfxFilter")) {
							ImGui::Checkbox("Enable filtered main view (G)", &m_useGfxFilter);
							ImGui::Checkbox("Enable filtered motion vectors", &m_useFilteredMotion);
							// Task-less mesh-shader main leg (opaque single-sided,
							// base FS): needs device mesh shaders, else the
							// indexed multi-draws stay (CPU fallback).
							ImGui::Checkbox("Enable mesh-shader main leg", &m_useGfxMesh);
							ImGui::SameLine();
							ImGui::TextDisabled(smvk::SupportsMeshShaders() ? "" : "(no device mesh shaders)");
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
							ImGui::Checkbox("HZB occluder fill + cull (big crudest-LOD; the hizDepth image is a fixed cost)", &m_hizOccluderDepth);
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
						if (ImGui::CollapsingHeader("Geometry LOD")) {
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
						if (ImGui::CollapsingHeader("Texture Streaming")) {
							ImGui::Checkbox("Texture streaming (applies to next scene load)", &m_textureStreamingEnabled);
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
								"SSR mask (receivers)",
							};
							// Combo index <-> ViewMode mapping (SsrContribution has
							// no combo entry: it stays env-only; the combo
							// jumps over it so indices never alias views).
							static const ViewMode kComboViews[] = {
								ViewMode::Lit, ViewMode::Unlit,
								ViewMode::Meshlets, ViewMode::Cascades,
								ViewMode::Lods, ViewMode::VertexColors,
								ViewMode::Uv0, ViewMode::Uv1,
								ViewMode::Pom, ViewMode::SsrMask,
							};
							int view = 0;
							for (int i = 0; i < 10; ++i)
								if (kComboViews[i] == m_debugView)
									view = i;
							if (ImGui::Combo("Debug view", &view, viewNames, 10))
								m_debugView = kComboViews[view];
						}
						ImGui::TextDisabled("SSR mask: per-material receiver class mirroring the thin G-buffer writer +\n"
							"the trace matte gate (roughness < 0.94 marches): green smooth dielectric, blue smooth\n"
							"metal, black matte (skipped), dimmed Blend (never writes the G-buffer the trace reads).");
						ImGui::TextDisabled("F6 vertex colors: layered materials show normalized weights as R/G/B/Y per layer 0..3;\n"
							"legacy materials show the raw vertex tint.");
						ImGui::TextDisabled("F9/F10 UV checkers: base TEXCOORD0 vs lightmap atlas uv (chart uv + chart id,\n"
							"unpacked in the VS via the draw's chart offset). Magenta glow = huge uv\n"
							"screen-derivative (a chart corner landing in the wrong atlas rect).\n"
							"UV1 stripes magenta when the atlas uv leaves [0,1] (transform bug).");
						ImGui::TextDisabled("F11 POM: ray-marched parallax uv through the occlusion-map heightfield\n"
							"(AO 1 = surface, 0 = cavity) + sun self-shadowing. Sun/sky shading at the displaced\n"
							"uv; baked GI / decals / forward+ off so the only delta vs F1 is the technique itself.\n"
							"AO-neutral (the SSGI chain owns AO application; debug views export no aoTerm).\n"
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
							ImGui::TextDisabled("Debug programs missing - views disabled");
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
						}
						else {
							ImGui::TextDisabled("Motion vectors: target/shaders missing");
						}
						ImGui::Checkbox("Light points (overlay)", &m_showLightPoints);
						ImGui::Checkbox("Light numbers (overlay)", &m_showLightNumbers);
						if (m_showLightPoints || m_showLightNumbers) {
							ImGui::SliderFloat("Overlay point size (px)", &m_lightPointSize, 2.0f, 16.0f);
							ImGui::SliderInt("Overlay max labels", &m_maxLightLabels, 0, 1024);
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
							// GI debug views (R / F8 cycle): 0 off, 1 baked
							// irradiance, 2 source palette, 3 isolated octree level
							// (the level slider picks it), 4 dominant octree level
							// (which octave carries the pixel). The sggi walk already
							// implemented modes 3/4 - this combo just reaches them.
							const char* dbgNames[5] = { "Off", "Baked irradiance",
								"GI source", "Isolated level", "Dominant level" };
							int dbgIdx = m_giDebug;
							if (ImGui::Combo("GI debug view (R / F8)", &dbgIdx, dbgNames, 5))
								m_giDebug = std::clamp(dbgIdx, 0, 4);
							if (m_giDebug == 1) {
								ImGui::TextDisabled("flat irradiance of the active source "
									"(lightmap/grid/probes); magenta = none\n"
									"works in EVERY ambient mode (N): isolate the source "
									"with N, then read it here");
							}
							else if (m_giDebug == 2) {
								ImGui::TextDisabled("source palette: red = lightmap (deprecated), "
									"green = regular grid (legacy), blue = octree world field, "
									"cyan = convex volume blended, magenta = none");
							}
							else if (m_giDebug == 3) {
								int lvl = m_giDebugLevel;
								if (ImGui::SliderInt("Isolated level", &lvl, 0, 7))
									m_giDebugLevel = std::clamp(lvl, 0, 7);
								ImGui::TextDisabled("the octree walk gathers ONLY this "
									"level - per-octave contribution in isolation");
							}
							else if (m_giDebug == 4) {
								ImGui::TextDisabled("the octree level carrying most of "
									"this pixel's weight (its argmax over per-level "
									"weight sums)");
							}
							if (m_cvolLoaded) {
								ImGui::Checkbox("Convex GI volumes", &m_cvolEnabled);
								ImGui::TextDisabled("stacked probe volumes blend over the "
									"octree by priority (gi/<scene>.cvmanifest)");
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
					if (ImGui::CollapsingHeader("Shadow GPU stages (timestamp queries, ~1 frame lag)", ImGuiTreeNodeFlags_DefaultOpen)) {
						// Both shadow representations instrumented (see
						// GpuStageTimer): exactly one side is live per run
						// (PCF vs EVSM are mutually exclusive); "(off)" marks
						// the inactive representation + stages that never ran.
						if (m_stageGpuTimingsSupported) {
							ImGui::Text("PCF raster:    %s", m_stageGpuValid[smvk::kGpuStagePcfRaster]
								? (std::to_string(m_stageGpuMsSmooth[smvk::kGpuStagePcfRaster]) + " ms (smooth)").c_str() : "(off)");
							ImGui::Text("EVSM casters:  %s", m_stageGpuValid[smvk::kGpuStageEvsmCasters]
								? (std::to_string(m_stageGpuMsSmooth[smvk::kGpuStageEvsmCasters]) + " ms (smooth)").c_str() : "(off)");
							ImGui::Text("EVSM resolve:  %s", m_stageGpuValid[smvk::kGpuStageEvsmResolve]
								? (std::to_string(m_stageGpuMsSmooth[smvk::kGpuStageEvsmResolve]) + " ms (smooth)").c_str() : "(off)");
							ImGui::Text("EVSM blur:     %s", m_stageGpuValid[smvk::kGpuStageEvsmBlur]
								? (std::to_string(m_stageGpuMsSmooth[smvk::kGpuStageEvsmBlur]) + " ms (smooth)").c_str() : "(off)");
						}
						else {
							ImGui::TextDisabled("(unavailable: device lacks cross-queue timestamps)");
						}
					}
					if (ImGui::CollapsingHeader("Camera + swapchain", ImGuiTreeNodeFlags_DefaultOpen)) {
						// Live pos/fwd readouts live in the text HUD (Ctrl+T).
						{
							const glm::vec3 euler = m_cameraPositioner.getEulerDegrees(); // x=pitch, y=yaw, z=roll
							const glm::quat q = m_cameraPositioner.getOrientation();
							ImGui::Text("Camera rot yaw: %.1f pitch: %.1f roll: %.1f (deg)", euler.y, euler.x, euler.z);
							ImGui::TextDisabled("Camera quat: %.3f %.3f %.3f %.3f", q.x, q.y, q.z, q.w);
						}
						// Controller status: name + live stick state, or "none".
						// (m_inputUi: the ELEMENT's input back-pointer — pure
						// status/config widgets; this class never polls input
						// for control.)
						if (m_inputUi && m_inputUi->hasGamepad()) {
							ImGui::Text("Gamepad: %s", m_inputUi->gamepadName(0).c_str());
							ImGui::TextDisabled("LX %+.2f LY %+.2f  RX %+.2f RY %+.2f",
								m_inputUi->gamepadAxis(0, SDL_GAMEPAD_AXIS_LEFTX),
								m_inputUi->gamepadAxis(0, SDL_GAMEPAD_AXIS_LEFTY),
								m_inputUi->gamepadAxis(0, SDL_GAMEPAD_AXIS_RIGHTX),
								m_inputUi->gamepadAxis(0, SDL_GAMEPAD_AXIS_RIGHTY));
							ImGui::TextDisabled("Left stick move, right stick look, LT/RT down/up, RB sprint, Start UI");
						}
						else {
							ImGui::TextDisabled("Gamepad: none connected");
						}
						if (m_inputUi) {
							float dzMove = m_inputUi->stickDeadzone();
							float dzLook = m_inputUi->lookDeadzone();
							float dzTrig = m_inputUi->triggerDeadzone();
							if (ImGui::SliderFloat("Move deadzone", &dzMove, 0.0f, 0.5f))
								m_inputUi->setStickDeadzone(dzMove);
							if (ImGui::SliderFloat("Look deadzone", &dzLook, 0.0f, 0.5f))
								m_inputUi->setLookDeadzone(dzLook);
							if (ImGui::SliderFloat("Trigger deadzone", &dzTrig, 0.0f, 0.5f))
								m_inputUi->setTriggerDeadzone(dzTrig);
						}
						ImGui::Text("Swapchain %ux%u  framesInFlight=%u",
							fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height, fb.swapchain->framesInFlight());
						ImGui::Text("Scene: %ux%u (%.0f%%) -> swap %ux%u",
							renderExtent.width, renderExtent.height, m_renderScale * 100.0f,
							swapExtent.width, swapExtent.height);
						ImGui::Text("UI pass: %ux%u (%.0f%%)",
							uiExtent.width, uiExtent.height, m_uiScale * 100.0f);
						const smvk::PresentMode active = fb.swapchain->GetActivePresentMode();
						if (active != m_host.GetPresentMode())
							ImGui::TextDisabled("Active: %s (requested unavailable?)", smvk::ToString(active));
						else
							ImGui::TextDisabled("Active: %s", smvk::ToString(active));
					}
					if (ImGui::CollapsingHeader("GPU memory + streaming", ImGuiTreeNodeFlags_DefaultOpen)) {
						// ---- GPU memory (VMA budgets, VK_EXT_memory_budget) ----
						// Throttled to 1 Hz (see RefreshGpuMemoryStats).
						// Totals live in the text HUD (Ctrl+T); this section
						// keeps the controls + the per-heap breakdown below.
						// Query cost is shown so a slow driver budget query
						// can't hide behind this UI.
						RefreshGpuMemoryStats(false);
						auto fmtMiB = [](uint64_t bytes, char* out, size_t n) {
							const double mib = (double)bytes / (1024.0 * 1024.0);
							if (mib >= 1024.0)
								snprintf(out, n, "%.2f GiB", mib / 1024.0);
							else
								snprintf(out, n, "%.0f MiB", mib);
						};
						if (!m_gpuMemHeaps.empty()) {
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
						}
						else {
							ImGui::TextDisabled("GPU memory: no heap data");
						}
						// Texture streaming residency lives in the text HUD
						// (Ctrl+T); nothing else to show here.
						if (!m_textureStreamer) {
							ImGui::TextDisabled("Streaming: off (full-resident)");
						}
					}
					if (ImGui::CollapsingHeader("Shadows - CSM + spots + deferred", ImGuiTreeNodeFlags_DefaultOpen)) {
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
						}
						else {
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
						}
						else {
							ImGui::TextDisabled("Bloom: OFF");
						}
						// Live exposure numbers live in the text HUD (Ctrl+T).
						float histPeak = 1.0f;
						for (float v : m_exposureBinsUi)
							histPeak = std::max(histPeak, v);
						ImGui::PlotHistogram("##expHist", m_exposureBinsUi,
							(int)smvk::kExposureBinCount, 0, nullptr, 0.0f, histPeak, ImVec2(0, 48));
						if (!m_viewportResources.exposure.Valid())
							ImGui::TextDisabled("Meter missing manual exposure only");
						else
							ImGui::TextDisabled("Bloom threshold scales with 1/exposure (ref %.4f); percentile %.0f%%, key %.2f",
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
						}
						else {
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
						}
						else if (m_aaMode == AaMode::Smaa1x || m_aaMode == AaMode::SmaaT2x) {
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
						}
						else {
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
							ImGui::TextDisabled("Motion blur target/shaders missing - sharp HDR");
						else
							ImGui::TextDisabled("Motion blur: gather along motion vectors, pre-tonemap HDR");
						const auto& ssgi = m_viewportResources.ssgi;
						{
							static const char* const kAoNames[3] =
							{ "No AO", "SSAO", "SAO" };
							ImGui::TextDisabled("AO mode: %s (post-Main SSGI chain)",
								kAoNames[static_cast<int>(m_aoMode)]);
						}
						if (m_aoMode != AoMode::None) {
							if (!ssgi.Valid())
								ImGui::TextDisabled("SSGI targets/shaders missing - no AO");
							else if (!ssgi.hzb.Valid())
								ImGui::TextDisabled("SSGI AO: targets up but the shared HZB is missing - chain skips");
							else {
								ImGui::Text("SSGI AO: half %ux%u (full %ux%u) hzb %ux%u (%u mips)",
									ssgi.halfExtent.width, ssgi.halfExtent.height,
									ssgi.fullExtent.width, ssgi.fullExtent.height,
									ssgi.hzb.width, ssgi.hzb.height, ssgi.hzb.mipCount);
								ImGui::TextDisabled("  compose: joint bilateral depth+normal (repDist in the AO triple);\n"
									"  normal source: %s",
									m_viewportResources.msaaEnabled
									? "none (MSAA frame - the prepass writes no G-buffer; depth-only weights)"
									: (m_viewportResources.ssr.gbuffer.Valid()
										? "shared thin G-buffer (oct normal)"
										: "none (G-buffer target missing - depth-only weights)"));
							}
						}
						if (!m_viewportResources.dof.Valid())
							ImGui::TextDisabled("DOF targets/shaders missing - sharp HDR");
						else
							ImGui::TextDisabled("DOF: %s focus %.1fm range %.1fm (%s)",
								m_dofEnabled ? "ON" : "off",
								m_dofFocusDistance, m_dofFocusRange,
								m_dofHqBokeh ? "HQ bokeh" : "low");
						if (!m_viewportResources.ssr.Valid())
							ImGui::TextDisabled("Reflection target/shaders missing - no reflections");
						else if (!m_viewportResources.ssgi.hzb.Valid())
							ImGui::TextDisabled("SSGI reflections: %s %u steps intensity %.2f hzb:MISSING (chain skips)",
								m_ssrEnabled ? "ON" : "off",
								(uint32_t)std::clamp(m_ssrSteps, 1, 128), m_ssrIntensity);
						else
							ImGui::TextDisabled("SSGI reflections: %s %u steps intensity %.2f hzb:shared %ux%u %u mips",
								m_ssrEnabled ? "ON" : "off",
								(uint32_t)std::clamp(m_ssrSteps, 1, 128), m_ssrIntensity,
								m_viewportResources.ssgi.hzb.width,
								m_viewportResources.ssgi.hzb.height,
								m_viewportResources.ssgi.hzb.mipCount);
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
						const int wantLights = (int)m_cookedLights.pointSpots.size();
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
						ImGui::Separator();
						const smvk::gfx::Stats gs = m_gfxRenderer.LastStats();
						// GPU mode: the cull runs in compute (the CPU stat
						// only counts CPU-mode culls - always 0/797 in GPU
						// mode); the kept count lands via the async counter-
						// word readback (see UpdateReadbacks), culled =
						// candidates - kept. See the HUD filter line too.
						const bool gpuKept = m_gfxKeptValid
							&& m_gfxRenderer.State().gpuCull;
						uint32_t kept = gs.clustersProcessed - gs.clustersCulled;
						if (gpuKept)
							kept = std::min(m_gfxKeptBatches, gs.clustersProcessed);
						ImGui::Text("view: %u draws %u cmds %u tris | clusters %u kept %u culled %u%s",
							gs.draws, gs.commands, gs.trianglesProcessed,
							gs.clustersProcessed, kept, gs.clustersProcessed - kept,
							gpuKept ? " (GPU cull)" : "");
						ImGui::Text("materials: %u ranges  batches: %u",
							(unsigned)m_gfxRenderer.LastMaterialRangeCount(),
							(unsigned)m_gfxRenderer.LastBatchCount());
						// HZB occlusion status: the CPU "clusters culled" line above
						// never moves for GPU occlusion kills (those stay batches
						// with indexCount 0), so surface the HZB inputs here. Any
						// "OFF/empty/INVALID" below means the filter's occlusion
						// test is silently disabled - use Dump HZB + Dump indirect
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
						// HZB source is FIXED (hizDepth - see the fc.hizDepthView
						// note); the toggle gates the occluder fill + cull.
						const bool depthOk =
							m_viewportResources.hizDepth.bindlessIndex != UINT32_MAX;
						ImGui::Text("hzb: %ux%u/%u mips depthIdx=%s occ=%s cull=%s fill=%s",
							hw, hh, hm, depthOk ? "ok" : "INVALID", occOn ? "on" : "OFF",
							cullingFrozen ? "FROZEN" : "live",
							m_hizOccluderDepth ? "occluder" : "OFF");
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
						// Per-frame draws/switched/far-shaded live in the text
						// HUD (Ctrl+T); the cooked counts stay here.
						ImGui::Text("Cooked: %u meshes / %u siblings",
							m_lodChainMeshes, m_lodChainLods);
						ImGui::TextDisabled("Transition = projected simplifier error (world err * H / "
							"(2*tan(fov/2)*dist) <= budget), hysteretic around the budget. "
							"Every level registers its own gfx mesh: the selected LOD draws "
							"through the HZB-culled indirect ranges.");
						ImGui::TextDisabled("Refresh: %s (VULKANTOY_LODFULL=0 restores halfrate).",
							m_lodFullRate ? "full-rate, every node per frame"
							: "halfrate parity split");
						ImGui::TextDisabled("Shader LODs: farthest level shades vertex-lit (F5: white).");
						if (!m_lodEnabled)
							ImGui::TextDisabled("LODs OFF - always LOD0.");
						else if (m_lodChainMeshes == 0)
							ImGui::TextDisabled("No cooked LOD chain in this scene - cook with --lods on.");
						else if (!ShaderLodActive())
							ImGui::TextDisabled("Shader LODs OFF (toggle or missing program) - every level full-lit.");
					}
					ImGui::Separator();
						ImGui::TextDisabled("[I] %s  [ESC] quit", m_uiModeHint ? "UI (NORMAL) -> press I for FPS" : "FPS (DISABLED) -> press I for UI");
					if (m_inputUi && m_inputUi->hasGamepad())
						ImGui::TextDisabled("[Gamepad] %s connected (Start = UI)", m_inputUi->gamepadName(0).c_str());
					else
						ImGui::TextDisabled("[Gamepad] none connected");
					ImGui::TextDisabled("Keys: M blur  O AO (No/SSAO/GTAO/SAO)  B bloom  X exposure  V vectors");
					ImGui::TextDisabled("      L LODs  F particles  G filter  K sky");
					ImGui::TextDisabled("      J blood splat at crosshair  VULKANTOY_DECALS=0/1");
					ImGui::TextDisabled("      N ambient GI/sky/off  F8 GI irradiance view");
					ImGui::TextDisabled("      F1 lit  F2 unlit  F3 meshlets  F4 cascades  F5 LOD colors  F6 vertex colors");
#if defined(DS_DEBUG_VIZ)
					ImGui::TextDisabled("      Y DS visualizer (%s)",
						m_dsDebugMode == DsDebugViz::Off ? "off" : "on");
#endif
					ImGui::TextDisabled("      Z/1-4 AA  P wireframe  ` texture viewer  Ctrl+T stats HUD");
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			ImGui::End();

			// Text-only stats HUD (top-left overlay, Ctrl+T): drawn after
			// the panel; the panel is a fixed Right side bar, so the two
			// never overlap.
			DrawHudOverlay();

			// Frame-time histogram (F12): SEPARATE UI from the debug panel -
			// a full-width bottom strip (see profiler::DrawFrameHistogramUi).
			// Zero cost while disabled: one flag check.
			if (smvk::profiler::FrameHistogramEnabled())
				smvk::profiler::DrawFrameHistogramUi();
			// Texture inspector (aux-texture debug view): SEPARATE floating
			// window, never part of the debug panel - it only opens on the
			// backtick shortcut (keybind::TEXTURE_VIEWER_TOGGLE). Closing (X
			// or backtick) drops the preview binding + selection.
			// ---- Texture inspector (aux-texture debug view) --------
			// Lists the LIVE textures (one row per pool texture; the
			// metadata: debug name, extent, format, mips/layers, the
			// bindless slot - NO content filter: every texture is one
			// click away) and previews a clicked one through the
			// ImGui Vulkan backend's user-texture bindings. Plain 2D
			// single-layer textures preview directly; CUBEMAPS preview
			// through six per-face View2D bindings (+X -X +Y -Y +Z -Z,
			// 3x2 grid - the ImGui backend samples Texture2D only, so
			// the cube's own default view cannot serve as the preview);
			// cube arrays preview cube 0. Other arrays/3D list only.
			// Staleness: a selected handle dropping out of the listing
			// (destroyed, generation bumped - UpdateTexture's recreate
			// path) drops the preview + face views.
			// Selection is shared by CLICK and ARROW-KEY NAV: ImGui moves
			// the keyboard-nav cursor onto rows with the arrow keys and
			// the focused row previews IMMEDIATELY (no Enter - the nav
			// branch below treats focus as selection). Click still works;
			// Enter still activates (both funnel into the same select).
			if (m_texDebugWindow) {
				ImGui::SetNextWindowSize(ImVec2(560.0f, 760.0f), ImGuiCond_Appearing);
				ImGui::Begin("Texture Viewer", &m_texDebugWindow);
				// Preview sampler: clamp + linear, created once.
				if (m_texDebugSampler == UINT32_MAX) {
					smvk::SamplerCreateInfo sci{};
					sci.magFilter = smvk::Filter::Linear;
					sci.minFilter = smvk::Filter::Linear;
					sci.mipmapMode = smvk::MipmapMode::Nearest;
					sci.addressU = smvk::AddressMode::ClampToEdge;
					sci.addressV = smvk::AddressMode::ClampToEdge;
					sci.addressW = smvk::AddressMode::ClampToEdge;
					m_texDebugSampler = m_context->GetOrCreateSampler(sci);
				}
				std::vector<smvk::TextureInfo> texs =
					m_context->ListTextureInfos();
				// Common-format label (debug shorthand; see the format
				// legend at the bottom of this window). BC compressed
				// formats included: pak CONTENT textures are BC (the
				// cook transcodes), so the content rows would otherwise
				// all read "(other)".
				auto fmtName = [](smvk::Format f) -> const char* {
					switch (f) {
					case smvk::Format::BC1_RGB_UNorm: return "BC1_RGB";
					case smvk::Format::BC1_RGB_SRGB: return "BC1_RGB_sRGB";
					case smvk::Format::BC1_RGBA_UNorm: return "BC1_RGBA";
					case smvk::Format::BC1_RGBA_SRGB: return "BC1_RGBA_sRGB";
					case smvk::Format::BC2_UNorm: return "BC2";
					case smvk::Format::BC2_SRGB: return "BC2_sRGB";
					case smvk::Format::BC3_UNorm: return "BC3";
					case smvk::Format::BC3_SRGB: return "BC3_sRGB";
					case smvk::Format::BC4_UNorm: return "BC4_UN";
					case smvk::Format::BC4_SNorm: return "BC4_SN";
					case smvk::Format::BC5_UNorm: return "BC5_UN";
					case smvk::Format::BC5_SNorm: return "BC5_SN";
					case smvk::Format::BC6H_UFloat: return "BC6H_UF";
					case smvk::Format::BC6H_SFloat: return "BC6H_SF";
					case smvk::Format::BC7_UNorm: return "BC7";
					case smvk::Format::BC7_SRGB: return "BC7_sRGB";
					case smvk::Format::RGBA8_UNorm: return "RGBA8_UN";
					case smvk::Format::RGBA8_SRGB: return "RGBA8_SRGB";
					case smvk::Format::RGBA16_Float: return "RGBA16F";
					case smvk::Format::RGBA32_Float: return "RGBA32F";
					case smvk::Format::R16_Float: return "R16F";
					case smvk::Format::R32_Float: return "R32F";
					case smvk::Format::RG16_Float: return "RG16F";
					case smvk::Format::D32_Float: return "D32F";
					default: return "(other)";
					}
				};
				// Selection staleness: the handle must still be live.
				bool selAlive = m_texDebugSelected.empty();
				ImGui::BeginChild("##texlist", ImVec2(0.0f, 220.0f), true);
				for (const smvk::TextureInfo& t : texs) {
					if (!m_texDebugSelected.empty()
						&& m_texDebugSelected == t.handle)
						selAlive = true;
					const bool plain2d =
						t.type == smvk::ImageType::Image2D
						&& !t.isCubemap && t.arrayLayers == 1;
					// Cubemap (or 6*N cube array) preview: per-face 2D
					// views. Cube arrays preview cube 0 (faces 0..5).
					const bool cube = t.isCubemap
						&& smvk::IsCubemapLayerCount(t.arrayLayers);
					const bool previewable = plain2d || cube;
					char label[320];
					std::snprintf(label, sizeof(label),
						"%s%s %ux%u %s mips=%u layers=%u bindless=%d",
						t.name.empty() ? "(unnamed)" : t.name.data(),
						cube ? (t.arrayLayers > 6 ? " [cube array: cube 0 preview]"
							: " [cube 6-face preview]")
						: (previewable ? "" : " [no 2D preview]"),
						t.extent.width, t.extent.height, fmtName(t.format),
						t.mipLevels, t.arrayLayers, t.bindless);
					if (ImGui::Selectable(label,
						m_texDebugSelected == t.handle)) {
						// Click (or Enter/gamepad activate): preview
						// immediately. See SelectTexDebugTexture for the
						// shared selection body.
						SelectTexDebugTexture(t);
					}
					else if (ImGui::IsItemFocused()
						&& m_texDebugSelected != t.handle
						&& previewable) {
						// ARROW-KEY NAV: ImGui moved the keyboard-nav
						// cursor onto this row - treat focus as
						// selection so the preview displays immediately
						// (no Enter). The != guard makes this a no-op on
						// the already-selected row (focus persists on it
						// every frame); previewable keeps 3D/arrays
						// non-selectable by nav.
						SelectTexDebugTexture(t);
					}
				}
				ImGui::EndChild();
				if (!m_texDebugSelected.empty() && !selAlive) {
					// Died under the selection (generation bumped):
					// drop the preview + face views, never sample a
					// dead view.
					DropTexDebugSelection();
					ImGui::TextDisabled("selected texture was destroyed "
						"(generation bumped) - selection cleared");
				}
				if (m_texDebugSelected.empty()) {
					ImGui::TextDisabled("%zu live textures - click one to inspect.",
						texs.size());
				}
				else {
					// Preview: fit the window width, keep the aspect.
					// Raw sample - HDR formats read blown/crushed
					// (inspect, not grade).
					const smvk::TextureInfo* sel = nullptr;
					for (const smvk::TextureInfo& t : texs)
						if (t.handle == m_texDebugSelected) {
							sel = &t;
							break;
						}
				if (sel) {
					// The texture NAME, prominent (the list rows
					// truncate long load paths; the preview states
					// what is actually on screen). Arena-owned copy
					// (NUL-terminated), so the (begin, end) form needs
					// no NUL reliance at all.
					ImGui::TextUnformatted(
						sel->name.empty() ? "(unnamed)"
						: sel->name.data(),
						sel->name.empty() ? "(unnamed)"
						: sel->name.data() + sel->name.size());
						if (sel->isCubemap) {
							// Cubemap: the 3x2 per-face grid (+X -X +Y -Y
							// +Z -Z, row-major). One image per face - a
							// face whose view died (its texture streamed
							// a recreation) shows a blank cell (binding 0
							// renders the backend's null texture).
							static const char* const kFaceNames[6] = {
								"+X", "-X", "+Y", "-Y", "+Z", "-Z"
							};
							const float gap = 6.0f;
							const float cell =
								(ImGui::GetContentRegionAvail().x - gap * 2.0f)
								/ 3.0f;
							for (uint32_t face = 0; face < 6; ++face) {
								if (face > 0 && (face % 3) != 0)
									ImGui::SameLine(0.0f, gap);
								ImGui::BeginGroup();
								if (m_texDebugCubePreview[face] != 0) {
									ImGui::Image(m_texDebugCubePreview[face],
										ImVec2(cell, cell));
								}
								else {
									// Dead binding (the view died after the
									// Add - never hand a null descriptor to
									// the backend): placeholder keeps the grid
									// aligned.
									ImGui::Dummy(ImVec2(cell, cell));
									ImGui::TextDisabled("(view died)");
								}
								ImGui::TextDisabled("%s", kFaceNames[face]);
								ImGui::EndGroup();
							}
							ImGui::TextDisabled("%ux%u cube (raw sample - HDR "
								"formats read unexposed; cube arrays show "
								"cube 0)",
								sel->extent.width, sel->extent.height);
							// Probe row: world position + partition info.
							// Matched by the library entry's TEXTURE HANDLE
							// (exact - the bindless slot could alias after
							// churn); entryName is the library key, so at
							// most one row matches. (The Holder converts
							// to Handle - extract BEFORE comparing: the
							// implicit conversion on the left side of the
							// member operator trips MSVC's rewritten-
							// candidate rules.)
							for (const CubemapRow& row : m_cubemapRows) {
								const auto* entry =
									m_cubemaps.Find(row.entryName);
								if (!entry || entry->texture.empty())
									continue;
								const smvk::TextureHandle entryTex =
									entry->texture;
								if (entryTex != sel->handle)
									continue;
								ImGui::Text("probe %s @ (%.1f, %.1f, %.1f)"
									"  influence %.1fm  fade %.1fm  %s",
									row.entryName.c_str(),
									row.position.x, row.position.y,
									row.position.z,
									row.influenceRadius, row.fadeRadius,
									row.pinned
										? "pinned (always resident)"
										: "streamed (distance residency)");
								break;
							}
							// No row matched: the global/skybox capture
							// (no manifest row) - nothing to place.
						}
						else if (m_texDebugPreview != 0) {
							const float w = ImGui::GetContentRegionAvail().x;
							const float aspect =
								float(std::max(sel->extent.height, 1u))
								/ float(std::max(sel->extent.width, 1u));
							ImGui::Image(m_texDebugPreview,
								ImVec2(w, w * aspect));
							ImGui::TextDisabled("%ux%u (raw sample - HDR "
								"formats read unexposed)",
								sel->extent.width, sel->extent.height);
						}
						// Material use (2D only - cubemaps serve IBL, not
						// material slots). Materials store the texture POOL
						// index (TextureHandle::index() - NOT the bindless
						// slot; the two diverge once textures churn, see
						// GetTextureBindlessIndex), so match on that.
						if (!sel->isCubemap && m_world) {
							const int poolIdx =
								static_cast<int>(sel->handle.index());
							uint32_t useBase = 0, useMetRough = 0,
								useNormal = 0, useOcc = 0, useEmis = 0,
								useMats = 0;
							const size_t slotCount =
								m_world->materialLocations.size();
							for (size_t i = 0; i < slotCount; ++i) {
								const smvk::Material* mat =
									m_world->GetMaterial((uint32_t)i);
								if (!mat)
									continue;
								bool uses = false;
								if (mat->values.textures.x == poolIdx)
									{ ++useBase; uses = true; }
								if (mat->values.textures.y == poolIdx)
									{ ++useMetRough; uses = true; }
								if (mat->values.textures.z == poolIdx)
									{ ++useNormal; uses = true; }
								if (mat->values.textures.w == poolIdx)
									{ ++useOcc; uses = true; }
								if (mat->values.textures2.x == poolIdx)
									{ ++useEmis; uses = true; }
								if (uses)
									++useMats;
							}
							if (useMats == 0) {
								ImGui::TextDisabled(
									"no material uses this texture (aux)");
							}
							else {
								// Channel names as users author them:
								// diffuse (baseColor), metallicRoughness
								// (G=roughness B=metallic), normal,
								// occlusion, emissive.
								char usage[256];
								int off = 0;
								const auto append =
									[&](const char* chan, uint32_t n) {
										if (!n || off >= (int)sizeof(usage) - 1)
											return;
										off += std::snprintf(usage + off,
											sizeof(usage) - off, "%s%s x%u",
											off ? ", " : "", chan, n);
									};
								append("diffuse", useBase);
								append("metallicRoughness", useMetRough);
								append("normal", useNormal);
								append("occlusion", useOcc);
								append("emissive", useEmis);
								ImGui::Text("material use (%u material%s): %s",
									useMats, useMats == 1 ? "" : "s", usage);
							}
						}
					}
				}
				// Format legend: the row labels above are debug shorthand.
				if (ImGui::CollapsingHeader("Format legend")) {
					ImGui::TextDisabled(
						"RGBA8_UN    8-bit/channel linear LDR - vertex-color\n"
						"            charts, engine scratch\n"
						"RGBA8_SRGB  8-bit sRGB-encoded color - imported\n"
						"            albedo/diffuse, decal tints\n"
						"RGBA16F     16-bit float HDR - GI volumes, cubemaps,\n"
						"            bloom, SSR targets\n"
						"RGBA32F     32-bit float HDR - light/debug buffers\n"
						"R16F/RG16F/R32F  1-2 channel float - depth meters,\n"
						"            velocity, SH taps, exposure\n"
						"D32F        depth attachment (32-bit float)\n"
						"BC1/BC3/BC5/BC7  block-compressed pak CONTENT\n"
						"            (cook transcodes): albedo / alpha-smooth\n"
						"            / normal / roughness-metallic packed\n"
						"BC4/BC6H    1-channel UN/SN + HDR float blocks\n"
						"            (rare in this content)\n"
						"(other)     everything else (numeric formats,\n"
						"            packed 101010/111110, ...)\n"
						"_sRGB suffix = sRGB variant of the same layout");
				}
				ImGui::End();
			}
			// Closed via the title-bar X: the flag flipped above, drop the
			// binding + selection so nothing samples a dead view.
			if (!m_texDebugWindow
				&& (m_texDebugPreview != 0 || !m_texDebugSelected.empty()))
				DropTexDebugSelection();

			// Forward+ light debug overlay: project the CPU light copy
			// (m_debugLights, filled in UpdateForwardPlusLights) through the
			// UNJITTERED viewProj into the ImGui foreground draw list. It
			// rides the UI pass and composites over the tonemapped scene.
			// The jittered frame matrix would buzz every marker +/-0.25 px
			// per frame (the UI layer has no temporal resolve). No depth
			// test: occluded / behind-camera lights stay visible (debug).
			if ((m_showLightPoints || m_showLightNumbers)
				&& !m_debugLights.empty()) {
				ImDrawList* dl = ImGui::GetForegroundDrawList();
				const ImVec2 disp = ImGui::GetIO().DisplaySize;
				const float r = std::max(m_lightPointSize, 1.0f);
				const auto toSrgb8 = [](float c) {
					c = std::min(std::max(c, 0.0f), 1.0f);
					return (int)std::lround(std::pow(c, 1.0f / 2.2f) * 255.0f);
				};
				uint32_t labels = 0;
				const uint32_t maxLabels =
					(uint32_t)std::max(0, m_maxLightLabels);
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
					}
					else {
						dl->AddCircleFilled(pt, r, col);
					}
					if (m_showLightNumbers && labels < maxLabels) {
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
						{ 0, 1 },{ 1, 2 },{ 2, 3 },{ 3, 0 },
						{ 4, 5 },{ 5, 6 },{ 6, 7 },{ 7, 4 },
						{ 0, 4 },{ 1, 5 },{ 2, 6 },{ 3, 7 },
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
			// marker per cached probe row - center dot + index, solid ring =
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
						dl->AddCircle(pt, std::max(m_lightPointSize, 4.0f) + 5.0f,
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
				outExtent,
				swapIsSrgb,
				"UiComposite",
				outX,
				outY);

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
		// Flush bindless descriptor writes HERE - after every
		// GetOrCreateSampler / Ensure / CreateBindlessImage of this frame
		// (the per-frame sampler cache block and post passes sit ABOVE this
		// point), and BEFORE Execute records a single command. The old call
		// site (top of RenderFrame) ran before the sampler block, so a
		// sampler created that frame (e.g. the SMAA point sampler on first
		// use) stayed an unwritten descriptor slot for the whole frame -
		// GPU-AV: "Descriptor index 3 is uninitialized" - with the draw
		// sampling through garbage. Update-after-bind keeps these writes
		// visible to everything recorded below.
		m_context->UpdateDescriptors();
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
		// zero-fill); nothing to download here - the histogram compute was
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
				m_host.WaitForDeviceIdle();
				m_gfxRenderer.DumpIndirectAudit(*m_context, gfxSlot);
			}
		}
		if (m_debugDumpHzb) {
			m_debugDumpHzb = false;
			if (m_useGfxFilter && gfxSlot != UINT32_MAX) {
				m_host.WaitForDeviceIdle();
				m_gfxRenderer.DumpHzb(*m_context, gfxSlot);
			}
		}
		// Opt-in first-frame HZB dump (debug): stalls the GPU (device-idle
		// wait), so it stays off unless explicitly enabled.
		if (m_dumpFirstFrameHzb && m_useGfxFilter && gfxSlot != UINT32_MAX) {
			m_dumpFirstFrameHzb = false;
			m_host.WaitForDeviceIdle();
			m_gfxRenderer.DumpHzb(*m_context, gfxSlot);
		}
		// Opt-in deferred-shadow dump (VULKANTOY_DS_DUMP=<frame>): one
		// shot after Execute with the phase results final. Stalls
		// (device-idle wait), so it stays off unless explicitly enabled.
		if (m_dumpAtFrame >= 0 && m_frameCounter == (uint64_t)m_dumpAtFrame) {
			m_dumpAtFrame = -1;
			if (DsActive()) {
				m_host.WaitForDeviceIdle();
				DumpDeferredShadows(fb.frameIndex);
			}
		}
		// Opt-in EVSM atlas dump (VULKANTOY_EVSM_DUMP=<frame>): one-shot
		// after Execute (the blur completed in this frame's submit - the
		// dump waits for device idle first).
		if (m_evsmDumpAtFrame >= 0 && m_frameCounter == (uint64_t)m_evsmDumpAtFrame) {
			m_evsmDumpAtFrame = -1;
			if (EvsmActive()) {
				m_host.WaitForDeviceIdle();
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
			m_host.WaitForDeviceIdle();
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
		}
		else {
			m_dlssReset = true;
		}
		m_dlssLastMode = m_dlssMode;
		m_dlssLastRenderExtent = renderExtent;
		m_dlssLastSwapExtent = outExtent;
		// Hitch logger (see the rfT0 site): frames over the
		// VULKANTOY_HITCH_LOG budget, with the pipeline-cache delta -
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

namespace engine {

	void WorldRenderer::EvsmDumpAtlas(uint32_t frameIndex) {
		// Opt-in debug (VULKANTOY_EVSM_DUMP=<frame>): per-stage stats over the
		// cascade-0 tile of the moments/temp/blurred images - channels are
		// (M1+ = E[exp(+c d)], M2+ = E[exp(+2c d)], M1- = E[exp(-c d)],
		// M2- = E[exp(-2c d)]) - plus a log-encoded M1- PNG per stage in
		// exports/ (dark = far). The PNG edge-band width IS the prefilter
		// width, which is how the softness knob's effect was verified.
		const smvk::EvsmAtlas& atlas = m_viewportResources.evsmDir;
		// (Holder is move-only: reference the originals instead of copies.)
		const smvk::Holder<smvk::ImageHandle>* stagesArr[3] = {
			&atlas.moments[0], &atlas.blurTemp[0], &atlas.blurred[0]
		};
		const char* stageNames[3] = { "moments", "temp", "blurred" };
		// moments: written by the resolve (General) then read by the blur H
		// (ShaderReadOnly) - its post-frame layout; temp: blur H write (General);
		// blurred: blur V write (General) -> light sampling (ShaderReadOnly).
		const smvk::ImageLayout stageLayouts[3] = {
			smvk::ImageLayout::ShaderReadOnlyOptimal,
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
			// Visual edge-band width = the prefilter width - definitive.
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
	// exports/ - NaN/Inf paint HOT PINK so a broken stage is unmistakable.
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
				// A = metal. Decoded raw (4 channels) - the PNG shows the
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
			// the outliers' locations - clustered/strided = readback layout
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
			// SAMPLED depth - anything outside [0,1] there is garbage the SSR
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

	// (UpdateCamera + SetUIMode moved to WorldRendererElement with the
	// input polling and the virtual cursor â€” see its UpdateCamera/
	// SetUIMode. The frame build reads the driven camera off Camera().)

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
		// Trilinear baseline (linear within + linear between mips); aniso Nx
		// adds maxAnisotropy N (CreateSampler clamps to the device limit, and
		// enables VK anisotropy only when > 1).
		const float maxAniso = TextureFilteringAnisotropy(m_textureFiltering);
		// Slot 0: default Repeat sampler (material albedo/normal/metalness/...,
		// shaders hardcode samplers[0]). Slot 2: clamp sampler (lightmap
		// atlases, 3D probe volumes via samplers[2], cubemaps via dedup).
		// Both are recreated in place so the bindless indices stay stable.
		if (m_context->GetSamplerCount() > 0) {
			smvk::SamplerCreateInfo repeatCi{};
			repeatCi.mipmapMode = smvk::MipmapMode::Linear;
			repeatCi.maxAnisotropy = maxAniso;
			m_context->RecreateSampler(0, repeatCi);
		}
		if (m_context->GetSamplerCount() > 2) {
			smvk::SamplerCreateInfo clampCi{};
			clampCi.addressU = smvk::AddressMode::ClampToEdge;
			clampCi.addressV = smvk::AddressMode::ClampToEdge;
			clampCi.addressW = smvk::AddressMode::ClampToEdge;
			clampCi.mipmapMode = smvk::MipmapMode::Linear;
			clampCi.maxAnisotropy = maxAniso;
			m_context->RecreateSampler(2, clampCi);
		}
	}

} // namespace engine

// NOTE: entry point moved to main.cpp (scene selection + Configure).
