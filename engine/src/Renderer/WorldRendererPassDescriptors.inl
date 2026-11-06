// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Renderer pass descriptor policy and utility constants. Included once by
// WorldRenderer.cpp after Rendergraph/RendererLimits dependencies. Static
// linkage and exact definitions are kept to preserve behavior/ABI.
// This collaborator owns descriptor construction, not GPU resources.

// -----------------------------------------------------------------------------
// Directional cascade shadow-map config (resources + math live in
// Engine/Rendergraph/cascadedshadowmap.h)
// -----------------------------------------------------------------------------
static constexpr uint32_t NUM_CASCADES = 4;
// Light-space depth padding either side of each cascade's RECEIVER slice.
// GetDirectionalLightOrthoVP also backs the light off by the same amount
// toward the sun, so the effective sunward coverage is ~2x this PAST THE
// SLICE's own extent. DEPTH COVERAGE OF TALL/UP-SUN CASTERS is no longer
// this constant's job: the caster-aware fit (ShadowCasterFitBounds, fed
// from the caster-candidate AABB below) extends the backoff + depth range
// to the retained casters' light-axis extent, clamped by
// kDirectionalShadowMaxDistance — the explicit pop bound. Casters beyond
// that past the slice clip out of the map (tall rooftop/trim shadows
// popping as the fit moves with the camera) BY DECISION, not by luck.
// Depth precision is a non-issue: the atlas is D32 and the per-cascade
// depth bias divides by the ACTUAL depthRange, so a wider range stays
// bias-corrected.
static constexpr float CASCADE_Z_PADDING = 25.0f;
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
static constexpr float BLOOM_THRESHOLD = 800.0f;
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
	// Headless SPD override (TOURMALINE_SPD=0 forces the raster 13-tap
	// downsample for A/B captures; =1 forces the single-pass compute path).
	if (const char* spdEnv = std::getenv("TOURMALINE_SPD")) {
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

// HSV -> RGB (h in 0..1 wraps, s/v in 0..1) - the C++ twin of the shader's
// HsvToRgb (tonemap.frag.hlsl). Used by the Forward+ light crowd for its
// hue-ladder colors.
static glm::vec3 HsvToRgbCpp(float h, float s, float v) {
	h = h - std::floor(h);
	const glm::vec3 k(1.0f, 2.0f / 3.0f, 1.0f / 3.0f);
	const glm::vec3 p = glm::abs(glm::fract(glm::vec3(h) + k) * 6.0f - glm::vec3(k.y));
	return v * glm::mix(glm::vec3(k.x), glm::clamp(p - glm::vec3(k.x), 0.0f, 1.0f), s);
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

static smvk::SsrDesc MakeSsrDesc(bool debugContrib) {
	smvk::SsrDesc d{};
	d.format = smvk::Format::RGBA16_Float; // must match the HDR target
	d.steps = SSR_STEPS;
	d.refineSteps = SSR_REFINE_STEPS;
	d.maxDistance = SSR_MAX_DISTANCE;
	d.thickness = SSR_THICKNESS;
	d.intensity = SSR_INTENSITY;
	d.f0 = SSR_F0;
	d.blurSharpness = SSR_BLUR_SHARPNESS;
	d.debugContrib = debugContrib; // m_debugSsr (TOURMALINE_SSR_DEBUG)
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
	d.useGiVolumes = 1;
	d.jitterStrength = 1.0f;
	d.heightFalloff = 0.0f;
	d.heightBase = 0.0f;
	d.name = "volumetric";
	return d;
}

// -----------------------------------------------------------------------------
// Screen-space god rays config (Engine/Rendergraph/godrays.h - the
// Dishonored-style sun occlusion + radial scattering): 32 samples,
// decay 0.97, a broad sun glow radius (the radial gather needs
// illuminated area to work with), SUBTLE intensity (an approximation
// stacked ADDITIVELY over the CSM-shadowed fog - never multiplied by
// it). Runs after the volumetric composite, before the HDR tail.
// -----------------------------------------------------------------------------
static constexpr float GODRAYS_INTENSITY = 0.5f;
static constexpr float GODRAYS_DECAY = 0.97f;
static constexpr float GODRAYS_REACH = 1.0f;
static constexpr float GODRAYS_GLOW_INNER = 0.01f;
static constexpr float GODRAYS_GLOW_OUTER = 0.12f;
static constexpr uint32_t GODRAYS_SAMPLES = 32;

static smvk::GodraysDesc MakeGodraysDesc() {
	smvk::GodraysDesc d{};
	d.format = smvk::Format::RGBA16_Float; // must match the HDR target
	d.intensity = GODRAYS_INTENSITY;
	d.decay = GODRAYS_DECAY;
	d.reach = GODRAYS_REACH;
	d.glowInner = GODRAYS_GLOW_INNER;
	d.glowOuter = GODRAYS_GLOW_OUTER;
	d.sampleCount = GODRAYS_SAMPLES;
	d.name = "godrays";
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
// Diffuse SSGI config (Engine/Rendergraph/ssgi_diffuse.h — one
// cosine-hemisphere bounce at half res + the energy-conserving
// ambient-REPLACEMENT compose). The values are the plan's v1 starting
// points for tuning, not long-standing engine defaults: 1 ray/pixel,
// 5 m bounce, 64 traversal steps, 0.9 temporal weight. Runs AFTER the
// AO compose (its baseline output) and BEFORE SSR (which then traces
// + composites over the GI-adjusted HDR).
// -----------------------------------------------------------------------------
static constexpr float SSGI_DIFFUSE_MAX_DISTANCE = 5.0f; // the LOCAL bounce (m)
static constexpr float SSGI_DIFFUSE_THICKNESS = 0.3f;    // depth acceptance band (m)
static constexpr float SSGI_DIFFUSE_INTENSITY = 1.0f;    // replacement strength
static constexpr float SSGI_DIFFUSE_TEMPORAL_BLEND = 0.9f; // history weight
static constexpr float SSGI_DIFFUSE_BLUR_SHARPNESS = 10.0f; // depth edge weight
static constexpr uint32_t SSGI_DIFFUSE_MAX_INTERSECTIONS = 64; // traversal cap

static smvk::SsgiDiffuseDesc MakeSsgiDiffuseDesc() {
	smvk::SsgiDiffuseDesc d{};
	d.format = smvk::Format::RGBA16_Float; // must match the HDR target
	d.maxDistance = SSGI_DIFFUSE_MAX_DISTANCE;
	d.thickness = SSGI_DIFFUSE_THICKNESS;
	d.intensity = SSGI_DIFFUSE_INTENSITY;
	d.temporalBlend = SSGI_DIFFUSE_TEMPORAL_BLEND;
	d.blurSharpness = SSGI_DIFFUSE_BLUR_SHARPNESS;
	d.maxIntersections = SSGI_DIFFUSE_MAX_INTERSECTIONS;
	d.raysPerPixel = 1; // v1: the fixed one-ray full dispatch
	d.name = "ssgiDiffuse";
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
	// The coarse VOLUMETRIC shadow atlas (the fog god-ray pass): a
	// DEDICATED second D32 image at HALF the cascade resolution -
	// ALWAYS allocated (a live mode flip to Coarse never recreates
	// anything; 4 MB at 2048-class cascades). The main cascade atlas
	// keeps its exact pre-volumetric grid extent (zero layout change
	// for every existing consumer), and the dedicated atlas carries its
	// own bindless index + graph view the fog march consumes.
	d.fogShadowResolution = std::max(256u, resolution / 2);
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
// Runtime simultaneous-light budget — the renderer-owned limit shared by
// BOTH light paths (the Forward+ ring below AND the legacy smvk::Light
// ring / LightBuffer shader bound; see Engine/Renderer/RendererLimits.h).
static constexpr uint32_t FP_MAX_LIGHTS =
	engine::renderer::limits::MaxSimultaneousLights;
// Probe entries ride the light buffer tail (see smvk::kForwardPlusProbe),
// so light building caps early: appended probes can never truncate a
// selected shadow spot, and the flight-slice stride still fits.
static constexpr uint32_t FP_PROBE_RESERVE = smvk::kForwardPlusMaxProbes;
static constexpr uint32_t FP_MAX_BUILD_LIGHTS = FP_MAX_LIGHTS - FP_PROBE_RESERVE;
// Hard per-tile SURFACE list cap. The cull RELEVANCE-SORTS candidates
// (key = light-center distance to the tile depth slab; spots pinned
// ahead of points), so overflow pops the FARTHEST candidates — a
// graceful distance fade, not the first-come hard edge that read as
// tile seams under dense crowds (neighboring tiles kept different
// arbitrary subsets; the seam was loudest where their depth slabs
// disagreed, i.e. at depth discontinuities). 8 first-come -> 16
// sorted: the per-pixel walk doubles at most, and the dropped lights
// are the dimmest by distance. The sort capacity is the shader's
// MAX_FP_TILE_SORT_SLOTS (32) — keep this <= it.
static constexpr uint32_t FP_MAX_PER_TILE = 16;
// Per-tile FOG list cap (the volumetric march's own lists — march-range
// candidacy, no depth-slab cuts; see ForwardPlusCullPush::fogTile*).
// Bounded so the march's per-refresh light walk stays cheap even in
// dense crowds (the refresh cadence walks this list every kVolGiRefresh
// meters per pixel). Same sort; same MAX_FP_TILE_SORT_SLOTS ceiling.
static constexpr uint32_t FP_FOG_PER_TILE = 16;
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
	d.maxFogPerTile = FP_FOG_PER_TILE;
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
// Local-light shadow atlas config (tile descriptors live in spotshadow.h).
// The atlas is fixed at kMaxLocalShadowTiles slots (8 spot tiles + 12
// point cube-face tiles); the per-frame selections pick at most
// m_spotShadowMax spots + m_pointShadowMax points (upper limit +
// distance/screenspace heuristic, see SelectSpotShadowCasters /
// SelectPointShadowCasters). Point faces ride the SAME atlas/descriptor
// buffer/bindless index - only the tile math differs (six 90-degree
// perspective faces per light).
// -----------------------------------------------------------------------------
static constexpr uint32_t SPOT_SHADOW_RES = 1024; // ceiling (the member clamps below)
static constexpr float SPOT_SHADOW_NEAR = 0.5f;
static constexpr float SPOT_SHADOW_BIAS = 0.0005f; // NDC base for slope-scaled ODB (Sec. 4.2)

static smvk::SpotShadowMapDesc MakeSpotShadowDesc(uint32_t resolution) {
	smvk::SpotShadowMapDesc d{};
	d.slotCount = smvk::kMaxLocalShadowTiles;
	d.resolution = resolution;
	d.format = smvk::Format::D32_Float;
	d.nearPlane = SPOT_SHADOW_NEAR;
	d.bias = SPOT_SHADOW_BIAS;
	d.name = "spotShadowAtlas";
	return d;
}

// -----------------------------------------------------------------------------
// EVSM moment atlases (RGBA32F 2xMSAA + async separable blur, 1-frame lag).
// Tile counts and normalized tile layout mirror the PCF atlases so the
// same lightVP math (BuildDirectionalLight / BuildSpotTile) addresses both.
// Tile pixel resolution may be capped by the host to avoid EVSM's large
// multi-flight RGBA32F allocation (the tile UV fractions stay identical).
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

static smvk::EvsmDesc MakeEvsmSpotDesc(smvk::EvsmPipelineMode pipeline,
	uint32_t tileResolution) {
	smvk::EvsmDesc d{};
	d.tileResolution = tileResolution;
	// Full combined grid (spots first, point faces after) so the moment
	// atlas tiles EXACTLY the D32 spot-shadow atlas: both derive cols from
	// ceil(sqrt) over the same count, hence the tile descriptors
	// (lightVP + uvBias/uvScale/uvClamp) are layout-identical and one
	// descriptor table addresses either representation. Sizing to the
	// spots alone (the old kMaxSpotShadows) drifted the grid for tiles
	// >= 4 (ceil(sqrt(8)) = 3 cols vs the D32's 5) AND caps the atlas at
	// spot-only. Memory is the price of the union grid:
	//   RGBA32F moments/scratch/blurred = 6 x (cols*res x rows*res x 16 B)
	//   -> 256-res ~126 MB, 512-res ~505 MB, 1024-res ~2 GB (+ MSAA depth)
	// only the tiles in use take moment/blur passes though - unused
	// regions cost VRAM, not time.
	d.tileCount = smvk::kMaxLocalShadowTiles;
	d.momentsFormat = smvk::Format::RGBA32_Float;
	d.depthFormat = smvk::Format::D32_Float;
	d.msaaSamples = 2;
	d.exponent = smvk::kEvsmExponent;
	d.blurRadius = smvk::kEvsmBlurRadius;
	d.pipeline = pipeline;
	d.name = "evsmSpotAtlas";
	return d;
}

