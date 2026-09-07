#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <Engine/core.h>
#include <Engine/Asset/pak.h>
#include <Engine/Rendergraph/bloom.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <Engine/Rendergraph/environment.h>
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
#include <App/frame_context.h>
#include <App/push_constants.h>
using app::PushConstants;
#include <Engine/Vfs/vfs.h>
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
static constexpr float CASCADE_LAMBDA = 0.92f; // 0 = linear, 1 = logarithmic
static constexpr float CASCADE_Z_PADDING = 1.0f;
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
static constexpr float TONEMAP_EXPOSURE = 0.001f; // sun 5000 lux * 0.001 ≈ HDR 5 pre-ACES
static constexpr uint32_t TONEMAP_MODE = 1; // 0=Reinhard, 1=ACES, 2=Uncharted2

static smvk::TonemapDesc MakeTonemapDesc() {
	smvk::TonemapDesc d{};
	d.exposure = TONEMAP_EXPOSURE;
	d.tonemapMode = TONEMAP_MODE;
	d.name = "Tonemap";
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
// SSAO config (half-res depth-only AO, see Engine/Rendergraph/ssao.h).
// Runs before Main; composites in the opaque ambient (default.frag.hlsl
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
static constexpr uint32_t FP_MAX_PER_TILE = 128;

static smvk::ForwardPlusDesc MakeForwardPlusDesc() {
	smvk::ForwardPlusDesc d{};
	d.tileSize = FP_TILE_SIZE;
	d.maxLights = FP_MAX_LIGHTS;
	d.maxLightsPerTile = FP_MAX_PER_TILE;
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
static constexpr float SPOT_SHADOW_BIAS = 0.0f; // 0: oriented depth bias (Sec. 4.2) in shader

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

// Scene samples (one row per sample; the framegraph is shared, so adding a
// sample never copies pass code). Cook commands:
//   Sponza: PakCooker --content-dir app/content
//             --in-vfs models/Sponza/sponza.gltf
//             --out-pak app/content/paks/sponza.pak --meshlets on
//   Bistro: py tools/cook_bistro.py --niagara-dir F:/REPOS/niagara_bistro
//             (stages models/Bistro + cooks content/paks/bistro.pak)
namespace app {

const SceneDesc& SceneRegistry::Sponza() {
	static const SceneDesc sponza{
		/*id=*/"sponza",
		/*label=*/"Sponza",
		/*gltfPath=*/"content/models/Sponza/sponza.gltf",
		/*pakPath=*/"content/paks/sponza.pak",
		/*camPos=*/glm::vec3(0.0f, 1.0f, 0.0f),
		/*camTarget=*/glm::vec3(0.0f, 1.0f, 1.0f),
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
		// Meters-scale exterior (root 100x scale + Level -90 deg X are
		// neutralized at cook time, see tools/cook_bistro.py).
		/*camPos=*/glm::vec3(7.0f, 4.0f, 9.0f),
		/*camTarget=*/glm::vec3(0.0f, 2.0f, 0.0f),
		/*farClip=*/200.0f,
		/*coordFix=*/0,
	};
	return bistro;
}
} // namespace app (scene registry methods; class methods open their own blocks below)

namespace app {

std::vector<SceneDesc> SceneRegistry::All() {
	return { Sponza(), Bistro() };
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
	// Point the FPS camera at the scene's default viewpoint (Camera keeps a
	// stable pointer to m_cameraPositioner, so lookAt is assignment-safe).
	m_cameraPositioner.lookAt(scene.camPos, scene.camTarget,
		glm::vec3(0.0f, 1.0f, 0.0f));
}

// Unit UV sphere (radius 1, rings x segments quads) for the emissive
// light-ball objects. Vertex colors are white (the ball material is unlit
// with a black base color, so only the emissive factor shows). Winding is
// CCW viewed from outside (glTF convention); the ball material is also
// double-sided so rasterization is winding-agnostic.
static smvk::StaticMeshUpload MakeUvSphereUpload(int rings, int segments)
{
	smvk::StaticMeshUpload out{};
	out.vertices.reserve((size_t)(rings + 1) * (segments + 1));
	out.indices.reserve((size_t)rings * segments * 6);

	for (int iy = 0; iy <= rings; ++iy) {
		const float v = float(iy) / float(rings);
		const float theta = v * glm::pi<float>();
		const float ct = std::cos(theta);
		const float st = std::sin(theta);
		for (int ix = 0; ix <= segments; ++ix) {
			const float u = float(ix) / float(segments);
			const float phi = u * 2.0f * glm::pi<float>();
			const float cp = std::cos(phi);
			const float sp = std::sin(phi);
			smvk::StaticVertex vtx{};
			vtx.pos = { st * cp, ct, st * sp };
			vtx.normal = vtx.pos;
			vtx.color = { 1.0f, 1.0f, 1.0f };
			vtx.texCoord = { u, 1.0f - v };
			vtx.tangent = { -sp, 0.0f, cp, 1.0f };
			out.vertices.push_back(vtx);
		}
	}

	const auto idx = [segments](int iy, int ix) {
		return (uint32_t)(iy * (segments + 1) + ix);
	};
	for (int iy = 0; iy < rings; ++iy) {
		for (int ix = 0; ix < segments; ++ix) {
			const uint32_t a = idx(iy, ix);
			const uint32_t b = idx(iy, ix + 1);
			const uint32_t c = idx(iy + 1, ix);
			const uint32_t d = idx(iy + 1, ix + 1);
			out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(c);
			out.indices.push_back(b); out.indices.push_back(d); out.indices.push_back(c);
		}
	}

	smvk::Surface surface{};
	surface.firstIndex = 0;
	surface.indexCount = (uint32_t)out.indices.size();
	surface.baseVertex = 0;
	out.surfaces.push_back(surface);
	out.aabbMin = { -1.0f, -1.0f, -1.0f };
	out.aabbMax = { 1.0f, 1.0f, 1.0f };
	return out;
}

// FrameUniforms / AppResources / ViewportResources live in
// App/scene_renderer.h (needed by the class declaration); method
// definitions follow the (removed) in-class block below.

} // namespace app (scene registry + helpers above; methods open their own block below)


// Class declaration lives in App/scene_renderer.h; out-of-line method
// definitions follow.
namespace app {
using Super = smvk::AppImpl;

bool SceneRenderer::Initialize() {
		if (!Super::Initialize())
			return false;

		UpdateCameraAspect();
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
		auto fragCode = readContentFile(contentVfs, "shaders/default.frag.hlsl.spv");
		m_resources.vertShader = m_context->CreateShaderModule(vertCode, "vert");
		m_resources.fragShader = m_context->CreateShaderModule(fragCode, "frag");

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

		// Masked prepass program: shadow.frag's inputs (pos/color/uv) are a
		// subset of default.vert's outputs, so it links directly.
		m_resources.prepassMaskedProgram = {
			.shaders = {
				smvk::Shader{
					.module = m_resources.vertShader,
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

		smvk::AllocateStaticVertexFmt(*m_context);

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
		bool loadedFromPak = false;
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
								m_world->LoadClusterFromPak(*m_context, *reader);
								// Serve pak-embedded textures through the VFS
								// (below loose content/ so dev overrides win).
								contentVfs.Mount(pakBackend, 10, scenePak);
								loadedFromPak = true;
							}
							catch (const std::exception& e) {
								std::cerr << "[SceneRenderer] Pak load failed, falling back "
									<< "to glTF: " << e.what() << std::endl;
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
			GltfToCluster(*m_context, model, sceneGltf, cluster, smvk::MeshOptConfig{}, smvk::MeshletConfig{}, m_scene.coordFix, m_scene.preTransform);
		}

		// Emissive point-light balls: a procedural cluster with one scene
		// node per demo point light. Must be registered before the world
		// merge so the nodes land in the runtime scene graph.
		CreatePointLightBalls();

		m_world->ResolveAndFixup_clusters();
		// Post-merge: resolve world-level ball mesh/material indices,
		// collect ball nodes, strip their shadow-caster flag.
		FixupPointLightBallNodes();

		// GeometryFX-style per-view filter (clustered): register static
		// meshes from cooked meshlets (pak path) or retained uploads
		// (procedural path), then bake the cluster-ordered index heap.
		// Non-fatal: unregistered meshes keep rendering via direct draws.
		// The pass config gates the whole filter (sample-level); the member
		// toggle stays as the runtime A/B switch.
		if (m_passes.gfxFilter && m_useGfxFilter) {
			m_gfxRenderer.ConfigureFrag(m_resources.fragShader);
			m_gfxRenderer.ConfigureSeedFrag(m_resources.shadowMaskedFragShader);
			if (!m_gfxRenderer.Ensure(*m_context, *m_rendergraph)) {
				std::cerr << "[SceneRenderer] GfxFilter unavailable - direct draws\n";
				m_useGfxFilter = false;
			}
			else {
				m_gfxRenderer.RegisterWorldMeshes(*m_world);
				m_gfxRenderer.UploadStaticData(*m_context, *m_rendergraph);
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
		m_context->FlushUploads();


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
			std::cerr << "[SceneRenderer] Spot shadow atlas unavailable – spots run unshadowed\n";
		}
		if (!m_viewportResources.tonemap.Ensure(*m_context, MakeTonemapDesc())) {
			std::cerr << "[SceneRenderer] Failed to create tonemap program\n";
			return false;
		}
		// Skydome program (fullscreen-triangle sky.vert/sky.frag). Failure is
		// non-fatal: the Main pass simply skips the sky draw and keeps the
		// clear color.
		if (!m_skydome.Ensure(*m_context)) {
			std::cerr << "[SceneRenderer] Skydome program unavailable – running without sky\n";
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
					std::cerr << "[SceneRenderer] MSAA target unavailable – falling back to 1x\n";
				}
			}
			if (!m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc())) {
				std::cerr << "[SceneRenderer] Bloom chain unavailable (HDR "
					<< renderExtent.width << "x" << renderExtent.height << ")\n";
			}
			smvk::UiDesc uiDesc{};
			uiDesc.format = m_swapchain->GetSurfaceFormat();
			if (!m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, uiDesc)) {
				std::cerr << "[SceneRenderer] UI target unavailable – UI disabled\n";
			}
			if (!m_viewportResources.uiComposite.Ensure(*m_context)) {
				std::cerr << "[SceneRenderer] UI composite program unavailable\n";
			}
		}

		// ImGui overlay – init after swapchain/rendergraph are ready.
		// Uses dynamic rendering; no external RenderPass needed.
		m_imgui = std::make_unique<smvk::ImGuiOverlay>();
		smvk::ImGuiOverlayDesc imguiDesc{};
		imguiDesc.fontPath = UI_FONT_PATH;
		imguiDesc.fontBaseSize = UI_FONT_BASE_SIZE;
		if (!m_imgui->Init(*m_context, *m_swapchain, window, imguiDesc)) {
			std::cerr << "[SceneRenderer] Failed to init ImGuiOverlay – continuing without UI\n";
			m_imgui.reset();
		} else {
			m_uiDpiScale = m_imgui->AppliedDpiScale(); // auto = monitor content scale
			SetUIMode(false); // start in FPS (DISABLED), I toggles to UI (NORMAL)
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
		m_viewportResources.bloom.Destroy(*m_rendergraph);
		m_viewportResources.spotShadows.Destroy(*m_rendergraph);
		m_viewportResources.directionalShadows.Destroy(*m_rendergraph);
		m_viewportResources.msaa.Destroy(*m_rendergraph);
		m_viewportResources.hdr.Destroy(*m_rendergraph);
		m_viewportResources.motionBlur.Destroy(*m_rendergraph);
		m_viewportResources.motionVectors.Destroy(*m_rendergraph);
		m_viewportResources.renderDepth.Destroy(*m_rendergraph);
		m_viewportResources.ui.Destroy(*m_rendergraph);
		m_viewportResources.uiComposite.DestroyShaders();
		if (m_imgui) {
			m_imgui->Shutdown();
			m_imgui.reset();
		}
		Super::Shutdown();
	}

	FrameUniforms SceneRenderer::UpdateFrameUniforms(uint32_t currentFrame, smvk::Extent2D renderExtent) {

		const int width_ = renderExtent.width;
		const int height_ = renderExtent.height;

		auto view = m_camera.getViewMatrix();
		auto proj = m_camera.getProjMatrix();
		proj[1][1] *= -1; // viewport fix ditecrly in proj

		auto viewProj = proj * view;
		FrameUniforms data{};
		data.cameraPos = m_cameraPositioner.cameraPosition_;

		data.view = view;
		data.proj = proj;
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
		// mid-gray (albedo 0.18, N·L=1) at HDR ≈ 0.29 pre-ACES → no clipping.
		// NOTE: FrameUniforms.exposure is currently unread by shaders; the
		// tonemap push constant (TONEMAP_EXPOSURE) is the live knob.
		data.exposure = TONEMAP_EXPOSURE;
		// SSAO consumption (lighting-pass ambient; see ao.hlsli +
		// default.frag.hlsl): the SsaoCompose pass already upscaled the AO
		// to full res with depth authority, so the fragment only .Loads it.
		// Mirrors the AddSsaoPasses gate below so the shader flag and the
		// emitted passes always agree.
		const bool ssaoOn = m_passes.ssao && m_ssaoEnabled
			&& m_viewportResources.ssao.Valid()
			&& m_ssaoIntensity > 0.0f;
		data.aoTextureIndex = ssaoOn
			? m_viewportResources.ssao.OutputBindless() : UINT32_MAX;
		data.aoIntensity = ssaoOn ? m_ssaoIntensity : 0.0f;
		// Configurable skydome IBL + fog (single source for sky/sun/fog).
		data.env = m_env.ToFrameData();

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
				CASCADE_Z_PADDING);
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
		m_context->UploadDataToBuffer(
			m_resources.directionalCascadeBuffer,
			&dir,
			sizeof(dir),
			smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment | smvk::PipelineStageBit_Compute,
			smvk::AccessFlags::ShaderRead,
			0,
			dirOffset
		);
		return dir;
	}

	void SceneRenderer::UpdateMaterials(uint32_t currentFrame) {
		// Keep the shared emissive-ball materials in sync with the demo knob
		// (bloom feeds off this; uploaded below with all other materials).
		if (m_lightCluster) {
			if (smvk::Material* mat = m_world->GetMaterial(m_ballMatWarmIdx))
				mat->values.params2.z = m_fpDemo.ballEmissive;
			if (smvk::Material* mat = m_world->GetMaterial(m_ballMatCoolIdx))
				mat->values.params2.z = m_fpDemo.ballEmissive;
		}

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

	// ---------------------------------------------------------------------
	// Emissive point-light ball objects (lights as real scene nodes).
	//
	// CreatePointLightBalls() builds a procedural Cluster (shared UV-sphere
	// mesh + two unlit emissive materials) with one scene node per demo
	// point light; the world merge folds it into the scene graph.
	// FixupPointLightBallNodes() then resolves world-level mesh/material
	// indices, collects the ball nodes and strips their shadow-caster flag
	// (emissive balls never cast shadows). Per frame,
	// UpdatePointLightBalls() drives the nodes: local transforms (bob
	// animation, radius), warm/cool material assignment and iterator flags;
	// the Forward+ light buffer is built from the resolved global
	// transforms in UpdateForwardPlusLights.
	// ---------------------------------------------------------------------
	void SceneRenderer::CreatePointLightBalls() {
		smvk::Cluster& cluster = m_world->LoadCluster(*m_context);
		m_lightCluster = &cluster;
		// Retain the CPU upload so the per-view filter can cook meshlet
		// clusters at register time (see RegisterWorldMeshes).
		cluster.SetRetainUploads(true);

		const smvk::ClusterStaticMeshHandle mesh =
			cluster.UploadStaticMesh(MakeUvSphereUpload(16, 24));
		if (mesh.empty()) {
			std::cerr << "[SceneRenderer] Light-ball mesh upload failed - scene-node lights disabled\n";
			m_lightCluster = nullptr;
			return;
		}

		// Unlit + black base color: the ball shows pure emissive (no PBR
		// lighting, no shadow sampling). Double-sided so winding of the
		// procedural sphere never matters.
		smvk::MaterialCreateInfo warmCi{};
		warmCi.shadingModel = smvk::ShadingModel::Unlit;
		warmCi.alphaMode = smvk::AlphaMode::Opaque;
		warmCi.doubleSided = true;
		warmCi.baseColorFactor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		warmCi.emissiveFactor = { 1.0f, 0.62f, 0.32f };
		warmCi.emissiveStrength = m_fpDemo.ballEmissive;
		const smvk::ClusterMaterialHandle warmMat = cluster.CreateMaterial(warmCi);

		smvk::MaterialCreateInfo coolCi = warmCi;
		coolCi.emissiveFactor = { 0.35f, 0.65f, 1.0f };
		const smvk::ClusterMaterialHandle coolMat = cluster.CreateMaterial(coolCi);

		if (warmMat.empty() || coolMat.empty()) {
			std::cerr << "[SceneRenderer] Light-ball material creation failed - scene-node lights disabled\n";
			m_lightCluster = nullptr;
			return;
		}
		m_ballMesh = mesh;
		m_ballMatWarm = warmMat;
		m_ballMatCool = coolMat;

		// One node per current grid light under a named "PointLights" root.
		// Nodes stay dynamic (no staticRoot mark) so the bob animation and
		// runtime pool growth keep working.
		Scene& cs = cluster.scene;
		const int root = addNode(cs, -1, 0);
		cs.localTransform[root] = glm::mat4(1.0f);
		cs.globalTransform[root] = glm::mat4(1.0f);
		setNodeName(cs, root, "PointLights");

		const int gx = std::max(1, m_fpDemo.gridX);
		const int gy = std::max(1, m_fpDemo.gridY);
		const int gz = std::max(1, m_fpDemo.gridZ);
		const float ox = -(gx - 1) * 0.5f * m_fpDemo.spacingX;
		const float oz = -(gz - 1) * 0.5f * m_fpDemo.spacingZ;
		uint32_t ballIdx = 0;
		for (int iz = 0; iz < gz; ++iz) {
			for (int iy = 0; iy < gy; ++iy) {
				for (int ix = 0; ix < gx; ++ix) {
					const int n = addNode(cs, root, 1);
					const glm::vec3 pos(
						ox + ix * m_fpDemo.spacingX,
						m_fpDemo.baseY + iy * m_fpDemo.spacingY,
						oz + iz * m_fpDemo.spacingZ
					);
					cs.localTransform[n] =
						glm::translate(glm::mat4(1.0f), pos) *
						glm::scale(glm::mat4(1.0f), glm::vec3(m_fpDemo.ballRadius));
					cs.globalTransform[n] = glm::mat4(1.0f); // resolved by the merge
					cs.meshForNode[n] = mesh.index();
					// Warm/cool checkerboard (kept in sync per frame).
					const bool warm = ((ix + iy + iz) & 1) == 0;
					cs.materialForNode[n] = (warm ? warmMat : coolMat).index();
					setNodeName(cs, n, "PLBall_" + std::to_string(ballIdx));
					++ballIdx;
				}
			}
		}
		// No markAsChanged here: ResolveAndFixup_clusters() marks each
		// cluster root and resolves the appended subtree.
	}

	void SceneRenderer::FixupPointLightBallNodes() {
		if (!m_lightCluster)
			return;
		Scene& scene = m_world->scene;

		// Resolve world-level indices for the shared ball mesh + materials
		// (needed to grow the pool at runtime and to assign materials).
		m_ballMeshIdx = UINT32_MAX;
		for (uint32_t i = 0; i < m_world->meshLocations.size(); ++i) {
			const auto& loc = m_world->meshLocations[i];
			if (loc.cluster == m_lightCluster && loc.handle == m_ballMesh) {
				m_ballMeshIdx = i;
				break;
			}
		}
		m_ballMatWarmIdx = UINT32_MAX;
		m_ballMatCoolIdx = UINT32_MAX;
		for (uint32_t i = 0; i < m_world->materialLocations.size(); ++i) {
			const auto& loc = m_world->materialLocations[i];
			if (loc.cluster != m_lightCluster)
				continue;
			if (loc.handle == m_ballMatWarm)
				m_ballMatWarmIdx = i;
			else if (loc.handle == m_ballMatCool)
				m_ballMatCoolIdx = i;
		}
		m_lightRootNode = findNodeByName(scene, "PointLights");

		// Collect ball nodes (hierarchy scan keeps creation/grid order) and
		// stamp them as scene light objects that never cast shadows.
		m_lightBalls.clear();
		for (uint32_t n = 0; n < scene.hierarchy.size(); ++n) {
			if (getNodeName(scene, (int)n).rfind("PLBall_", 0) != 0)
				continue;
			if (n < m_world->flags.size()) {
				m_world->flags[n] &= ~smvk::IteratorFlags(smvk::IteratorFlagBit_CastShadow);
				m_world->flags[n] |= smvk::IteratorFlagBit_LightPoint;
			}
			scene.lightForNode[n] = (uint32_t)m_lightBalls.size();
			m_lightBalls.push_back(PointLightBall{ .node = n, .active = false });
		}

		if (m_lightBalls.empty() || m_ballMeshIdx == UINT32_MAX ||
			m_ballMatWarmIdx == UINT32_MAX || m_ballMatCoolIdx == UINT32_MAX ||
			m_lightRootNode < 0) {
			std::cerr << "[SceneRenderer] Light-ball fixup failed (nodes=" << m_lightBalls.size()
				<< ") - scene-node lights disabled\n";
			m_lightCluster = nullptr;
			m_lightBalls.clear();
		} else {
#ifndef NDEBUG
			std::cout << "[SceneRenderer] Scene-node point lights: " << m_lightBalls.size()
				<< " emissive balls under 'PointLights' (mesh slot " << m_ballMeshIdx
				<< ", warm/cool materials " << m_ballMatWarmIdx << "/"
				<< m_ballMatCoolIdx << ")\n";
#endif
		}
	}

	// Grows the ball node pool on demand (grid sliders). New nodes are
	// appended directly to the resolved world scene (the ball cluster is
	// merged once at startup) and share the cluster's mesh/materials.
	void SceneRenderer::EnsurePointLightBalls(uint32_t count) {
		if (!m_lightCluster || m_ballMeshIdx == UINT32_MAX || m_ballMatWarmIdx == UINT32_MAX)
			return;
		Scene& scene = m_world->scene;
		while (m_lightBalls.size() < count) {
			const int parent = m_lightRootNode >= 0 ? m_lightRootNode : 0;
			const int n = addNode(scene, parent, getNodeLevel(scene, parent) + 1);
			scene.localTransform[n] = glm::mat4(1.0f);
			scene.globalTransform[n] = glm::mat4(1.0f);
			scene.meshForNode[n] = m_ballMeshIdx;
			scene.materialForNode[n] = m_ballMatWarmIdx;
			scene.lightForNode[n] = (uint32_t)m_lightBalls.size();
			setNodeName(scene, n, "PLBall_" + std::to_string(m_lightBalls.size()));
			// Grow world bookkeeping immediately: the per-frame drive below
			// writes flags[ball.node] in the same frame the node is created.
			if (m_world->flags.size() <= (size_t)n)
				m_world->flags.resize((size_t)n + 1, smvk::IteratorFlagBit_None);
			m_world->flags[n] = smvk::IteratorFlags(
				smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_LightPoint);
			m_lightBalls.push_back(PointLightBall{ .node = (NodeId)n, .active = false });
		}
		if (m_world->flags.size() < scene.hierarchy.size())
			m_world->flags.resize(scene.hierarchy.size(), smvk::IteratorFlagBit_None);
	}

	// Per-frame ball node drive: transforms (position + bob + radius),
	// warm/cool material of the visible ball, and iterator flags. Active
	// balls render (Renderable|Opaque) but never enter shadow passes
	// (no CastShadow); inactive pool leftovers stay as inert light nodes.
	void SceneRenderer::UpdatePointLightBalls() {
		m_activePointBalls = 0;
		if (!m_lightCluster)
			return;

		const int gx = std::max(1, m_fpDemo.gridX);
		const int gy = std::max(1, m_fpDemo.gridY);
		const int gz = std::max(1, m_fpDemo.gridZ);
		const uint32_t gridCount = (uint32_t)gx * (uint32_t)gy * (uint32_t)gz;
		const uint32_t spotReserve = (uint32_t)std::max(0, m_fpDemo.spotCount);
		if (m_fpDemo.enabled && FP_MAX_LIGHTS > spotReserve)
			m_activePointBalls = std::min(gridCount, FP_MAX_LIGHTS - spotReserve);
		EnsurePointLightBalls(m_activePointBalls);

		Scene& scene = m_world->scene;
		const float ox = -(gx - 1) * 0.5f * m_fpDemo.spacingX;
		const float oz = -(gz - 1) * 0.5f * m_fpDemo.spacingZ;
		for (uint32_t i = 0; i < m_activePointBalls && i < m_lightBalls.size(); ++i) {
			PointLightBall& ball = m_lightBalls[i];
			const uint32_t ix = i % (uint32_t)gx;
			const uint32_t iy = (i / (uint32_t)gx) % (uint32_t)gy;
			const uint32_t iz = i / ((uint32_t)gx * (uint32_t)gy);
			const bool warm = ((ix + iy + iz) & 1) == 0;
			const float phase = float(ix * 7 + iy * 13 + iz * 29) * 0.35f;
			const float bob = m_fpDemo.animate
				? std::sin(time_since_start_s * m_fpDemo.speed + phase) * 0.5f
				: 0.0f;
			const glm::vec3 pos(
				ox + ix * m_fpDemo.spacingX,
				m_fpDemo.baseY + iy * m_fpDemo.spacingY + bob,
				oz + iz * m_fpDemo.spacingZ
			);
			scene.localTransform[ball.node] =
				glm::translate(glm::mat4(1.0f), pos) *
				glm::scale(glm::mat4(1.0f), glm::vec3(m_fpDemo.ballRadius));
			// Keep the visible ball color in sync with the light checkerboard.
			scene.materialForNode[ball.node] = warm ? m_ballMatWarmIdx : m_ballMatCoolIdx;
			markAsChangedDynamic(scene, (int)ball.node);
			if (ball.node < m_world->flags.size())
				m_world->flags[ball.node] = smvk::IteratorFlags(
					smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_Opaque
					| smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_LightPoint);
			ball.active = true;
		}
		// Pool leftovers (grid shrank / demo disabled): inert light nodes -
		// no render, no shadow contribution, no Forward+ light.
		for (size_t i = m_activePointBalls; i < m_lightBalls.size(); ++i) {
			PointLightBall& ball = m_lightBalls[i];
			ball.active = false;
			if (ball.node < m_world->flags.size() &&
				(m_world->flags[ball.node] & smvk::IteratorFlagBit_Renderable))
				m_world->flags[ball.node] = smvk::IteratorFlagBit_LightPoint;
		}
		if (m_activePointBalls > 0)
			recalculateGlobalTransformsDynamic(scene);
	}

	// Builds the procedural Forward+ demo light set into the per-frame upload
	// buffer. Returns the number of lights written (clamped to FP_MAX_LIGHTS).
	// Point lights come from the emissive ball scene nodes (positions are
	// their resolved global transforms); spots stay procedural orbiters.
	// Spot-shadow selection runs here: spots carrying the cast-shadow request
	// flag are ranked by the screenspace/distance heuristic and the top-N
	// (upper limit m_fpDemo.spotShadowMax) get atlas tiles; their shadowTile /
	// shadowMapIndex are patched before upload. Results land in
	// m_spotShadowCount / m_spotShadowMapIndex for the frame's passes.
	uint32_t SceneRenderer::UpdateForwardPlusLights(uint32_t currentFrame, const FrameUniforms& frame) {
		// Drive the ball scene nodes first (transforms + flags + globals).
		UpdatePointLightBalls();

		std::vector<smvk::ForwardPlusLight> lights;
		lights.reserve(FP_MAX_LIGHTS);
		if (m_fpDemo.enabled) {
			const int gx = std::max(1, m_fpDemo.gridX);
			const int gy = std::max(1, m_fpDemo.gridY);
			const int gz = std::max(1, m_fpDemo.gridZ);
			const uint32_t ballCount =
				std::min(m_activePointBalls, (uint32_t)m_lightBalls.size());
			for (uint32_t i = 0; i < ballCount && lights.size() < FP_MAX_LIGHTS; ++i) {
				const glm::mat4& model = m_world->scene.globalTransform[m_lightBalls[i].node];
				const uint32_t ix = i % (uint32_t)gx;
				const uint32_t iy = (i / (uint32_t)gx) % (uint32_t)gy;
				const uint32_t iz = i / ((uint32_t)gx * (uint32_t)gy);
				smvk::ForwardPlusLight l{};
				l.position = { model[3][0], model[3][1], model[3][2] };
				l.range = m_fpDemo.range;
				// Warm/cool checkerboard so tiling artifacts read clearly
				// (matches the visible ball's emissive material).
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
			}
			// Orbiting spots to exercise the cone path.
			for (int s = 0; s < m_fpDemo.spotCount && lights.size() < FP_MAX_LIGHTS; ++s) {
				smvk::ForwardPlusLight l{};
				float a = time_since_start_s * 0.4f * m_fpDemo.speed + s * 6.2831853f / std::max(1, m_fpDemo.spotCount);
				l.position = { std::cos(a) * 6.0f, 7.0f, std::sin(a) * 4.0f };
				l.direction = glm::normalize(glm::vec3(-l.position.x, -7.0f, -l.position.z) * 0.35f + glm::vec3(0, -1, 0));
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
				if (atlasIdx >= 0 && !selected.empty()) {
					std::vector<smvk::GpuCascadeDescription> tiles(selected.size());
					for (size_t k = 0; k < selected.size(); ++k) {
						smvk::ForwardPlusLight& sl = lights[selected[k].lightIndex];
						tiles[k] = m_viewportResources.spotShadows.BuildSpotTile(
							glm::vec3(sl.position[0], sl.position[1], sl.position[2]),
							glm::normalize(glm::vec3(sl.direction[0], sl.direction[1], sl.direction[2])),
							sl.outerAngle, sl.range, atlasIdx, (uint32_t)k,
							SPOT_SHADOW_NEAR, SPOT_SHADOW_BIAS);
						sl.shadowTile = (int32_t)k;
						sl.shadowMapIndex = atlasIdx;
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
					m_spotShadowMapIndex = atlasIdx;
					// Keep the tiles for the per-frame spot RenderViews.
					m_spotShadowTiles = std::move(tiles);
				}
			}
		}

		constexpr size_t FP_FRAME_STRIDE = sizeof(smvk::ForwardPlusLight) * FP_MAX_LIGHTS;
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
		return (uint32_t)lights.size();
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
		m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, renderExtent);
		m_viewportResources.motionVectors.Ensure(*m_context, *m_rendergraph, renderExtent);
		m_viewportResources.motionBlur.Ensure(*m_context, *m_rendergraph, renderExtent, MakeMotionBlurDesc());
		m_viewportResources.ssao.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsaoDesc());
		smvk::MsaaDesc rebuildMsaaDesc{};
		rebuildMsaaDesc.depthFormat = m_swapchain->GetDepthFormat();
		m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, renderExtent, rebuildMsaaDesc);
		m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc());
		smvk::UiDesc rebuildUiDesc{};
		rebuildUiDesc.format = m_swapchain->GetSurfaceFormat();
		m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, rebuildUiDesc);
		UpdateCameraAspect();
	}

	void SceneRenderer::DrawFrame() {
		if (m_swapchain->FramebufferResized()) {
			m_swapchain->Recreate();
			RebuildSwapViews();
			m_swapchain->ClearFramebufferResized();
		}

		auto fb = m_swapchain->AcquireFramebuffer();
		UpdateDeltaTime();
		RenderFrame(fb);
		m_swapchain->EndAndPresentFramebuffer(fb);
	}

void SceneRenderer::RenderFrame(smvk::Framebuffer fb) {
	SMVK_PROFILE_FUNCTION();
	SMVK_PROFILE_TAG("frameIndex", fb.frameIndex);

	m_rendergraph->NewFrame();
	const smvk::Extent2D swapExtent{ fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height };
	const smvk::Extent2D renderExtent = ScaledExtent(swapExtent, m_renderScale);
	const smvk::Extent2D uiExtent = ScaledExtent(swapExtent, m_uiScale);
	UpdateCameraAspect();
	{
		SMVK_PROFILE_SCOPE("SceneRenderer_ResourceEnsure");
		smvk::DepthDesc frameDepthDesc{};
		frameDepthDesc.format = m_swapchain->GetDepthFormat();
		m_viewportResources.renderDepth.Ensure(*m_context, *m_rendergraph, renderExtent, frameDepthDesc);
			// Tile counts must be fresh for consumers below; the renderer
			// re-checks sizing at pass-build time (resize-safe).
			m_fpRenderer.EnsureSized(*m_context, *m_rendergraph,
				renderExtent, m_viewportResources.renderDepth.view);
			m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, renderExtent);
			m_viewportResources.motionVectors.Ensure(*m_context, *m_rendergraph, renderExtent);
			m_viewportResources.motionBlur.Ensure(*m_context, *m_rendergraph, renderExtent, MakeMotionBlurDesc());
			m_viewportResources.ssao.Ensure(*m_context, *m_rendergraph, renderExtent, MakeSsaoDesc());
			smvk::MsaaDesc frameMsaaDesc{};
			frameMsaaDesc.depthFormat = m_swapchain->GetDepthFormat();
			m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, renderExtent, frameMsaaDesc);
			m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc());
			smvk::UiDesc frameUiDesc{};
			frameUiDesc.format = m_swapchain->GetSurfaceFormat();
			m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, frameUiDesc);
			m_viewportResources.uiComposite.Ensure(*m_context);
			m_viewportResources.spotShadows.Ensure(*m_context, *m_rendergraph, MakeSpotShadowDesc());
		}
		m_context->UpdateDescriptors();
		const FrameUniforms frameData = UpdateFrameUniforms(fb.frameIndex, renderExtent);
		const smvk::GpuDirectionalLightDescription dirLight = UpdateLights(fb.frameIndex);
		UpdateMaterials(fb.frameIndex);
		const uint32_t fpLightCount = UpdateForwardPlusLights(fb.frameIndex, frameData);

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
			{ static_cast<float>(renderExtent.width), static_cast<float>(renderExtent.height) });
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
		// HDR target for scene – intermediate before tonemap
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
			.pad_pc = 0,
			.renderViews = renderViewsAddress,
			.mainRenderView = mainRenderView,
			.renderViewIndex = mainRenderView, // scene passes default to the main camera view
};
	// (size checked in App/push_constants.h)

	// ---- Cascaded shadow map passes – one per cascade tile (builder owns offsets/extents/load ops) ----
	// Pass-config gate: no shadow sampling without tiles (the main
	// shader reads cascadeBuffer regardless; tiles just stay cleared).
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
auto& shadowPass = m_viewportResources.directionalShadows.AddCascadePass(*m_rendergraph, ci, cascadeNames[ci])
				.SetRecordingFunc([this, cascadePC, casterViewIndex](smvk::CommandBuffer& cmd)
			{
				// GPU context + pass scope come from RenderGraph::Execute
				// (pass name "ShadowCascade<ci>" carries the cascade index).
					cmd.BindRenderPipeline(
						m_viewportResources.directionalShadows.program,
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
					cmd.PushConstants(0, sizeof(PushConstants), &cascadePC, smvk::ShaderStage::All);
					// Opaque only: BLEND transparent must not cast (full-quad depth).
					auto drawCaster = [&](NodeId node, bool maskedPhase) {
						auto meshForNode = m_world->scene.meshForNode.find(node);
						if (meshForNode == m_world->scene.meshForNode.end()) return;
						auto materialForNode = m_world->scene.materialForNode.find(node);
						if (materialForNode == m_world->scene.materialForNode.end()) return;
						smvk::Material* mat = m_world->GetMaterial(materialForNode->second);
						const bool masked = mat &&
							mat->shadingModel == smvk::ShadingModel::Masked;
						if (masked != maskedPhase) return;
						auto& model = m_world->scene.globalTransform[node];
						smvk::StaticMesh* meshDesc = m_world->GetMesh(meshForNode->second);
						if (!meshDesc) return;
						// RenderView plane cull: skip casters outside this cascade's frustum.
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
							objectPC.materialId = materialForNode->second;
							cmd.PushConstants(offsetof(PushConstants, materialId),
								sizeof(objectPC.materialId), &objectPC.materialId,
								smvk::ShaderStage::All);
						}
						cmd.PushConstants(offsetof(PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
						for (auto& draw_set : meshDesc->geometry.surfaces) {
							cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
						};
					};
					// Phase 1: solid casters, depth-only program (unchanged).
					m_world->forEachNode(
						smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
						| smvk::IteratorFlagBit_Opaque
						, [&](NodeId node) { drawCaster(node, false); });
					// Phase 2: MASK cutout casters, alpha-tested program so
					// foliage casts leaf-shaped shadows instead of full quads.
					// Cull off: masked foliage is double-sided in the main view.
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
					cmd.PushConstants(0, sizeof(PushConstants), &cascadePC, smvk::ShaderStage::All);
					m_world->forEachNode(
						smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
						| smvk::IteratorFlagBit_Opaque
						, [&](NodeId node) { drawCaster(node, true); });
				});
				(void)shadowPass;
			}
		}

		// ---- Spot shadow passes – one per selected caster tile ----
		// Selection (upper limit + screenspace/distance heuristic) ran in
		// UpdateForwardPlusLights; m_spotShadowCount tiles are valid. Each
		// pass renders its tile with the spot perspective VP (shadow.vert
		// spotIndex path). Opaque + MASK phases mirror the cascade passes.
		if (m_passes.spotShadows && m_spotShadowCount > 0 && m_viewportResources.spotShadows.Valid()) {
			const uint32_t spotCount = std::min(m_spotShadowCount, m_viewportResources.spotShadows.slotCount);
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
				auto& spotPass = m_viewportResources.spotShadows.AddSpotPass(*m_rendergraph, si, spotNames[si])
					.SetRecordingFunc([this, spotPC, casterViewIndex](smvk::CommandBuffer& cmd)
				{
					// GPU context + pass scope come from RenderGraph::Execute
					// (pass name "SpotShadow<si>" carries the spot index).
					cmd.BindRenderPipeline(
						m_viewportResources.spotShadows.program,
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
					cmd.PushConstants(0, sizeof(PushConstants), &spotPC, smvk::ShaderStage::All);
					// Opaque only: BLEND transparent must not cast (full-quad depth).
					auto drawCaster = [&](NodeId node, bool maskedPhase) {
						auto meshForNode = m_world->scene.meshForNode.find(node);
						if (meshForNode == m_world->scene.meshForNode.end()) return;
						auto materialForNode = m_world->scene.materialForNode.find(node);
						if (materialForNode == m_world->scene.materialForNode.end()) return;
						smvk::Material* mat = m_world->GetMaterial(materialForNode->second);
						const bool masked = mat &&
							mat->shadingModel == smvk::ShadingModel::Masked;
						if (masked != maskedPhase) return;
						auto& model = m_world->scene.globalTransform[node];
						smvk::StaticMesh* meshDesc = m_world->GetMesh(meshForNode->second);
						if (!meshDesc) return;
						// RenderView plane cull: skip casters outside the spot cone frustum.
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
							objectPC.materialId = materialForNode->second;
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
					m_world->forEachNode(
						smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
						| smvk::IteratorFlagBit_Opaque
						, [&](NodeId node) { drawCaster(node, false); });
					// Phase 2: MASK cutout casters, alpha-tested program.
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
					cmd.PushConstants(0, sizeof(PushConstants), &spotPC, smvk::ShaderStage::All);
					m_world->forEachNode(
						smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
						| smvk::IteratorFlagBit_Opaque
						, [&](NodeId node) { drawCaster(node, true); });
				});
				(void)spotPass;
			}
		}

		// ---- Forward+ passes (depth prepass + tile cull) ----
		// Owned by ForwardPlusRenderer (see App/forwardplus_renderer.h).
		fc.pc = pc;
		fc.depthView = m_viewportResources.renderDepth.logicalView;
		fc.depthViewHandle = m_viewportResources.renderDepth.view;
		fc.fpLights = m_resources.fpLightsLogical;
		fc.fpLightCount = fpLightCount;

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
				// — nothing dereferences it on GPU (filter push carries the
				// matrix; seed/VS stay on the live main view).
				const uint32_t frozenView = m_renderViews.AllocateCustomView(
					m_frozenViewProj,
					{ (float)renderExtent.width, (float)renderExtent.height });
				if (frozenView != smvk::RenderViewSet::kInvalidIndex)
					m_gfxRenderer.SetCullOverride(frozenView, m_frozenEye);
				else
					cullingFrozen = false;
			}
			m_gfxRenderer.AppendPasses(fc);
			gfxSlot = m_gfxRenderer.LastSlot();
		}
		fc.seedDepthPrimed = gfxFilterOn && m_gfxRenderer.SeededThisFrame();
		// Depth prepass (Loads seeded depth when primed, Clears otherwise).
		if (m_passes.forwardPlusPrepass)
			m_fpRenderer.AppendPrepass(fc);

		// HZB build over depth (seed or full prepass), filter compute
		// dispatch (reads this frame's HZB), depth pass 2 over newly
		// visible clusters + refresh, then tile cull over complete depth.
		// Held while the culling view is frozen (frozen footprints must test
		// against the frozen pyramid, not live depth).
		if (gfxFilterOn) {
			if (!cullingFrozen)
				m_gfxRenderer.AppendHizPasses(fc,
					m_viewportResources.renderDepth.logicalView,
					m_viewportResources.forwardPlus.depthBindlessIndex);
			m_gfxRenderer.AppendFilterDispatch(fc);
			if (m_gfxRenderer.RemainderPending()) {
				m_gfxRenderer.AppendRemainderPass(fc);
				if (!cullingFrozen)
					m_gfxRenderer.AppendHizPasses(fc,
						m_viewportResources.renderDepth.logicalView,
						m_viewportResources.forwardPlus.depthBindlessIndex,
						"GfxHizRefresh");
			}
		}
		if (m_passes.forwardPlusCull)
			m_fpRenderer.AppendTileCull(fc);

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
		// Motion vectors – per-pixel prevUV - currUV into RG16F (motionvector.h)
		// Re-rasterizes opaque geometry depth-tested READ-ONLY against the 1x
		// prepass depth (reversed-Z GreaterEqual, no writes) – identical in
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
			motionPass.SetRecordingFunc([this, motionPC, motionFilteredActive, gfxSlot, renderViewsAddress, mainRenderView, frameUniformsAddress](smvk::CommandBuffer& cmd)
			{
				// GPU context + pass scope come from RenderGraph::Execute.
				auto drawMotionNode = [&](NodeId node, bool maskedPhase) {
					auto meshForNode = m_world->scene.meshForNode.find(node);
					if (meshForNode == m_world->scene.meshForNode.end()) return;
					auto materialForNode = m_world->scene.materialForNode.find(node);
					if (materialForNode == m_world->scene.materialForNode.end()) return;
					// Static filtered nodes are covered by the HZB-culled
					// indirect draws below: skip them here so they rasterize
					// once. Moved filtered nodes (model != prev) still need
					// the direct path for their correct prevModel.
					if (motionFilteredActive
						&& m_gfxRenderer.IsMeshFiltered(meshForNode->second)) {
						const bool prevValid = m_prevGlobalTransforms.size()
							== m_world->scene.globalTransform.size();
						if (prevValid) {
							const glm::mat4& cur = m_world->scene.globalTransform[node];
							const glm::mat4& prv = m_prevGlobalTransforms[node];
							if (std::memcmp(glm::value_ptr(cur), glm::value_ptr(prv),
									sizeof(cur)) == 0)
								return;
						}
						else {
							return; // no history: prev == curr, indirect covers it
						}
					}
					smvk::Material* mat = m_world->GetMaterial(materialForNode->second);
					const bool masked = mat &&
						mat->shadingModel == smvk::ShadingModel::Masked;
					if (masked != maskedPhase) return;
					auto& model = m_world->scene.globalTransform[node];
					smvk::StaticMesh* meshDesc = m_world->GetMesh(meshForNode->second);
					if (!meshDesc) return;
					PushConstants objectPC{};
					std::copy_n(glm::value_ptr(model), 16, objectPC.model);
					// Previous frame's transform: zero motion on the first
					// frame or after the node count changed (scene rebuild).
					const bool prevValid =
						m_prevGlobalTransforms.size() == m_world->scene.globalTransform.size();
					const glm::mat4 prevModel =
						prevValid ? m_prevGlobalTransforms[node] : model;
					std::copy_n(glm::value_ptr(prevModel), 16, objectPC.prevModel);
					objectPC.materialId = materialForNode->second; // FS alpha test
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
				m_world->forEachNode(
					smvk::IteratorFlagBit_Renderable
					| smvk::IteratorFlagBit_Opaque
					| smvk::IteratorFlagBit_StaticVertexFmt
					, [&](NodeId node) { drawMotionNode(node, false); });
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
				m_world->forEachNode(
					smvk::IteratorFlagBit_Renderable
					| smvk::IteratorFlagBit_Opaque
					| smvk::IteratorFlagBit_StaticVertexFmt
					, [&](NodeId node) { drawMotionNode(node, true); });
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
	// ---------------------------------------------------------------------
	// SSAO – half-res depth-only AO (see ssao.h). Runs BEFORE Main: every
	// depth-writing pass (Forward+ prepass, gfx remainder, motion vectors)
	// is done, so the 1x prepass depth is final. The raw + blur passes
	// denoise at half res; the compose pass upscales to full res with
	// depth authority (ao.hlsli); Main's opaque ambient only .Loads the
	// composed AO (it must never sample depth — Main attaches it).
	// ---------------------------------------------------------------------
	smvk::LogicalImageViewHandle aoView{};
	{
		SMVK_PROFILE_CATEGORY("SSAO", Optick::Category::Rendering);
		smvk::SsaoDesc ssaoDesc = MakeSsaoDesc();
		ssaoDesc.radius = m_ssaoRadius;
		ssaoDesc.bias = m_ssaoBias;
		ssaoDesc.intensity = m_ssaoIntensity;
		ssaoDesc.maxDistance = m_ssaoMaxDistance;
		ssaoDesc.sampleCount = (uint32_t)m_ssaoSamples;
		ssaoDesc.blurSharpness = m_ssaoBlurSharpness;
		ssaoDesc.nearPlane = frameData.nearPlane;
		const bool ssaoOn = m_passes.ssao && m_ssaoEnabled
			&& m_viewportResources.ssao.Valid()
			&& m_ssaoIntensity > 0.0f;
		if (ssaoOn) {
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
		}
	}

	const bool useMsaa = m_passes.msaa && m_viewportResources.msaaEnabled &&
		m_context->SupportsMSAA2x() && m_viewportResources.msaa.Valid();
	std::vector<smvk::LogicalImageViewHandle> mainInputs;
	mainInputs.push_back(m_viewportResources.directionalShadows.logicalView);
	if (m_viewportResources.spotShadows.Valid())
		mainInputs.push_back(m_viewportResources.spotShadows.logicalView);
	// Blurred half-res AO is sampled by the opaque ambient (default.frag):
	// SetInputImages emits the read barrier + layout transition.
	if (!aoView.empty())
		mainInputs.push_back(aoView);
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
			.SetDepth(m_viewportResources.renderDepth.logicalView,
				smvk::AttachmentLoadOp::Load,
				smvk::AttachmentStoreOp::DontCare,
				smvk::ClearDepthStencil(0, 0)
			);
		}
		const glm::mat4 frameViewProj = frameData.viewProj; // debug pushes (value)
		mainPass.SetRecordingFunc([this, pc, useMsaa, gfxSlot, renderViewsAddress, mainRenderView, coneLineVerts, frameViewProj](smvk::CommandBuffer& cmd)
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

		auto drawNode = [&](NodeId node, uint32_t meshId, uint32_t materialId) {
			auto& model = m_world->scene.globalTransform[node];
			smvk::StaticMesh* meshDesc = m_world->GetMesh(meshId);
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

		auto bindForMaterial = [&](uint32_t materialId, smvk::ShaderProgram& prog,
				const smvk::DepthSettings& depth) {
			// Per-material raster (doubleSided -> Cull None) + blend
			// (Opaque vs AlphaBlend). Depth writes follow the caller: opaque
			// writes in 2x MSAA mode, transparent never writes.
			smvk::BlendSettings blend = smvk::BlendSettings::Opaque();
			smvk::RasterSettings raster{};
			if (smvk::Material* mat = m_world->GetMaterial(materialId)) {
				blend = mat->pipeline.blend;
				raster = mat->pipeline.raster;
			}
			cmd.BindRenderPipeline(
				prog,
				smvk::VertexFormat::StaticVertexFormat,
				depth,
				noStencil,
				blend,
				msaaSettings,
				raster
			);
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
			m_resources.mainProgram,
			smvk::VertexFormat::StaticVertexFormat,
			opaqueDepth,
			noStencil,
			smvk::BlendSettings::Opaque(),
			msaaSettings,
			smvk::RasterSettings{}
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
		// Meshlet debug view (replaces shaded opaque): draws EVERY
		// registered cluster direct with a stable per-meshlet color, so
		// HZB-culled meshlets stay visible here. Compare with the shaded
		// view to see exactly what the GPU filter dropped.
		const bool meshletDbg = m_debugMeshlets && m_gfxRenderer.DebugProgramsValid();
		if (meshletDbg) {
			cmd.BindRenderPipeline(
				m_gfxRenderer.MeshDebugProgram(),
				smvk::VertexFormat::StaticVertexFormat,
				opaqueDepth,
				noStencil,
				smvk::BlendSettings::Opaque(),
				msaaSettings,
				smvk::RasterSettings{ .cullMode = smvk::CullMode::None }
			);
		cmd.BindStaticVertexFmt();
		// No index bind: mesh, cluster-ordered and debug draws all address
		// the static index monoblock absolutely (offset-0 binding above).
		const smvk::gfx::State& dbgState = m_gfxRenderer.State();
			MeshDebugPush dp{};
			std::copy_n(glm::value_ptr(frameViewProj), 16, dp.viewProj);
			m_world->forEachNode(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_StaticVertexFmt
				,
				[&](NodeId node)
			{
				auto meshForNode = m_world->scene.meshForNode.find(node);
				if (meshForNode == m_world->scene.meshForNode.end())
					return;
				if (m_world->scene.materialForNode.find(node) ==
					m_world->scene.materialForNode.end())
					return;
				const uint32_t worldMesh = meshForNode->second;
				const glm::mat4& model = m_world->scene.globalTransform[node];
				std::copy_n(glm::value_ptr(model), 16, dp.model);
				const uint32_t gfxMesh = m_gfxRenderer.GfxMeshForWorldMesh(worldMesh);
		if (gfxMesh == UINT32_MAX || gfxMesh >= dbgState.meshes.size()) {
			// Not filterable: white direct fallback (nothing vanishes).
			// Mesh-local surfaces address the same index monoblock.
			dp.color[0] = dp.color[1] = dp.color[2] = 0.9f; dp.color[3] = 1.0f;
			cmd.PushConstants(0, sizeof(dp), &dp, smvk::ShaderStage::All);
			if (smvk::StaticMesh* meshDesc = m_world->GetMesh(worldMesh)) {
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
				dp.color[0] = rgba[0]; dp.color[1] = rgba[1];
				dp.color[2] = rgba[2]; dp.color[3] = rgba[3];
				cmd.PushConstants(0, sizeof(dp), &dp, smvk::ShaderStage::All);
				// Absolute into the static index monoblock (region base).
				const uint32_t firstIndex =
					smvk::gfx::ClusterIndexBase(dbgState) + cl.indexOffset;
				cmd.DrawIndexed(cl.triCount * 3, 1, firstIndex, 0);
				}
			}
			);
		}
		if (gfxActive && !meshletDbg) {
			const auto& gfxRanges = m_gfxRenderer.MaterialRanges(gfxSlot);
			for (uint32_t ri = 0; ri < (uint32_t)gfxRanges.size(); ++ri) {
				const uint32_t materialId = gfxRanges[ri].material;
				bindForMaterial(materialId, m_gfxRenderer.FilteredProgram(), opaqueDepth);
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
		if (!meshletDbg) {
			// UINT32_MAX forces a bind check on the first node; an
			// identical state is deduped by the CommandBuffer's
			// current-pipeline check, so this costs nothing.
			// (Skipped in meshlet-debug mode: unfiltered meshes already
			// drew white above.)
			uint32_t boundMaterial = UINT32_MAX;
			m_world->forEachNode(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				//| smvk::IteratorFlagBit_PassedCullTest
				| smvk::IteratorFlagBit_StaticVertexFmt
				,
				[&](NodeId node)
			{
				auto meshForNode = m_world->scene.meshForNode.find(node);
				if (meshForNode == m_world->scene.meshForNode.end())
					return;
				auto materialForNode = m_world->scene.materialForNode.find(node);
				if (materialForNode == m_world->scene.materialForNode.end())
					return;
				// Filtered meshes already drew above via indirect ranges.
				if (gfxActive && m_gfxRenderer.IsMeshFiltered(meshForNode->second))
					return;
				const uint32_t materialId = materialForNode->second;
				if (materialId != boundMaterial) {
					bindForMaterial(materialId, m_resources.mainProgram, opaqueDepth);
					boundMaterial = materialId;
				}
				drawNode(node, meshForNode->second, materialId);
			}
			);
		}

		// ---- BLEND transparent, back-to-front ----
		// Same pass (color already cleared, depth loaded): depth test on,
		// no depth writes, AlphaBlend from material.pipeline. Sorted by
		// distance-to-camera so foliage/glass composites correctly.
		{
			struct TransparentDraw { NodeId node; uint32_t meshId; uint32_t materialId; float dist2; };
			std::vector<TransparentDraw> draws;
			const glm::vec3 camPos = m_cameraPositioner.cameraPosition_;
			m_world->forEachNode(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Transparent
				| smvk::IteratorFlagBit_StaticVertexFmt
				,
				[&](NodeId node)
			{
				auto meshForNode = m_world->scene.meshForNode.find(node);
				if (meshForNode == m_world->scene.meshForNode.end())
					return;
				auto materialForNode = m_world->scene.materialForNode.find(node);
				if (materialForNode == m_world->scene.materialForNode.end())
					return;
				const glm::mat4& model = m_world->scene.globalTransform[node];
				const glm::vec3 center{ model[3][0], model[3][1], model[3][2] };
				const glm::vec3 d = center - camPos;
				draws.push_back(TransparentDraw{
					.node = node,
					.meshId = meshForNode->second,
					.materialId = materialForNode->second,
					.dist2 = glm::dot(d, d),
				});
			}
			);
			std::sort(draws.begin(), draws.end(),
				[](const TransparentDraw& a, const TransparentDraw& b) { return a.dist2 > b.dist2; });

			uint32_t boundMaterial = UINT32_MAX;
			for (const auto& d : draws) {
				if (d.materialId != boundMaterial) {
					bindForMaterial(d.materialId, m_resources.mainProgram, transparentDepth);
					boundMaterial = d.materialId;
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
			lp.color[0] = 1.0f; lp.color[1] = 1.0f;
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
	// vertex barrier and the fetches race the CS — geometry appears and
	// vanishes "randomly" whenever workload/timing shifts (MSAA mode,
	// light toggles). Covers the meshlet debug view too (cluster-ordered
	// heap bind).
	if (m_useGfxFilter && gfxSlot != UINT32_MAX)
		m_gfxRenderer.AddMainConsumeHints(mainPass, gfxSlot);
	// Forward+ tile lists are device-written by the cull pass and
	// device-read here: explicit read hints order cull -> main and emit
	// the compute->fragment buffer barriers.
	smvk::ForwardPlus::AddTileReadHints(mainPass,
			m_viewportResources.forwardPlus.tileCountsLogical,
			m_viewportResources.forwardPlus.tileIndicesLogical);
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

	// ---------------------------------------------------------------------
	// Motion blur – velocity gather in HDR (see motionblur.h). Runs after
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
					"MotionBlur");
				sceneHdrView = m_viewportResources.motionBlur.OutputView();
				sceneHdrBindless = m_viewportResources.motionBlur.OutputBindless();
			}
		}

		// ---------------------------------------------------------------------
		// Bloom – HDR downsample / upsample chain (composable, see bloom.h)
		// ---------------------------------------------------------------------
		{
			SMVK_PROFILE_CATEGORY("Bloom", Optick::Category::Rendering);
			if (m_passes.bloom && m_viewportResources.bloom.Valid()) {
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
		// Tonemapper – HDR (RGBA16F) → LDR swapchain (see tonemap.h)
		// ---------------------------------------------------------------------
		{
			SMVK_PROFILE_CATEGORY("Tonemap", Optick::Category::Rendering);
			const bool bloomOn = m_passes.bloom && m_viewportResources.bloom.Valid();
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
		auto& tonemapPass = m_viewportResources.tonemap.AddTonemapPass(
			*m_rendergraph,
			sceneHdrView,
			sceneHdrBindless,
			bloomView,
			bloomBindless,
			bloomIntensity,
			linearSampler,
			colors,
			swapExtent,
			"Tonemap",
			motionView,
			motionBindless,
			m_showMotionDebug ? 1u : 0u,
			m_motionDebugScale);
		(void)tonemapPass;
		}

		// ---------------------------------------------------------------------
		// ImGui – build UI (before adding the rendergraph pass)
		// ---------------------------------------------------------------------
		if (m_passes.ui && m_imgui && m_imgui->IsInitialized() && m_viewportResources.ui.Valid()) {
			// Target extent != window size when ui scale != 1: the overlay maps
			// ImGui draw data + font raster density onto the UI target.
			m_imgui->NewFrame(uiExtent);

			// Minimal UI – name + camera pos
			ImGui::Begin("VulkanToy — Debug");
			ImGui::Text("Frame %.1f FPS (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
			ImGui::Text("Camera pos: %.2f %.2f %.2f", m_cameraPositioner.cameraPosition_.x, m_cameraPositioner.cameraPosition_.y, m_cameraPositioner.cameraPosition_.z);
			ImGui::Text("Swapchain %ux%u  framesInFlight=%u",
				fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height, fb.swapchain->framesInFlight());
			ImGui::Separator();
			{
		auto splits = smvk::ComputeCascadeSplits(default_near_clip, m_scene.farClip, CASCADE_LAMBDA, NUM_CASCADES);
				const auto& atlas = m_viewportResources.directionalShadows;
				ImGui::Text("CSM: %u cascades  tile %ux%u  atlas %ux%u",
					NUM_CASCADES, CASCADE_RES, CASCADE_RES,
					atlas.atlasExtent.width, atlas.atlasExtent.height);
				for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
					ImGui::Text(" cascade %u: [%.2f .. %.2f] splitEnd %.2f", i, splits[i], splits[i+1], splits[i+1]);
				}
				const glm::vec3 toSunDbg = m_env.SunDirectionToSun();
				ImGui::Text(" sun toSun: %.2f %.2f %.2f  int %.0f lux",
					toSunDbg.x, toSunDbg.y, toSunDbg.z, m_env.sunIntensity);
				ImGui::Text("RenderViews: %u allocated (main=%u, CSM @%u, spots @%u)",
					m_renderViews.Count(), m_renderViews.MainViewIndex(),
					cascadeViewBase, spotViewBase);
			}
			{
				const auto& bloom = m_viewportResources.bloom;
				if (bloom.Valid()) {
					ImGui::Text("Bloom: %u mips  base %ux%u  thr %.2f knee %.2f int %.2f",
						bloom.MipCount(),
						bloom.MipExtent(0).width, bloom.MipExtent(0).height,
						bloom.desc.threshold, bloom.desc.knee, bloom.desc.intensity);
					for (uint32_t i = 0; i < bloom.MipCount(); ++i)
						ImGui::Text(" mip %u: %ux%u", i, bloom.MipExtent(i).width, bloom.MipExtent(i).height);
				} else {
					ImGui::TextDisabled("Bloom: OFF");
				}
			}
		{
			const bool supported = m_context->SupportsMSAA2x();
			if (!supported)
				m_viewportResources.msaaEnabled = false;
			ImGui::Checkbox("MSAA 2x (Main)", &m_viewportResources.msaaEnabled);
			if (!supported)
				ImGui::TextDisabled("MSAA 2x unsupported – running 1x");
			else
				ImGui::TextDisabled(m_viewportResources.msaa.Valid() ? "MSAA 2x -> HDR resolve" : "MSAA target missing – running 1x");
		}
		ImGui::Separator();
		if (ImGui::CollapsingHeader("Resolution — internal res + UI pass + DPI", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Scene: %ux%u (%.0f%%) -> swap %ux%u",
				renderExtent.width, renderExtent.height, m_renderScale * 100.0f,
				swapExtent.width, swapExtent.height);
			ImGui::SliderFloat("Render scale", &m_renderScale,
				RENDER_SCALE_MIN, RENDER_SCALE_MAX, "%.2fx");
			ImGui::Text("UI pass: %ux%u (%.0f%%)",
				uiExtent.width, uiExtent.height, m_uiScale * 100.0f);
			ImGui::SliderFloat("UI res scale", &m_uiScale,
				UI_SCALE_MIN, UI_SCALE_MAX, "%.2fx");
			ImGui::SliderFloat("UI DPI scale", &m_uiDpiScale,
				UI_DPI_MIN, UI_DPI_MAX, "%.2fx");
			if (ImGui::IsItemEdited() && m_imgui)
				m_imgui->ApplyDpiScale(m_uiDpiScale);
			ImGui::TextDisabled("Render scale resizes HDR/MSAA/bloom/depth + tonemap upscales to swap.\n"
				"UI renders into its own target and composites over the swapchain.");
		}
			ImGui::Separator();
			if (ImGui::CollapsingHeader("Environment — Skydome IBL + Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Checkbox("Sky dome", &m_env.skyEnabled);
				ImGui::Checkbox("IBL ambient/spec", &m_env.iblEnabled);
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
					ImGui::TextDisabled("Sky program missing – dome skipped");
			}
			ImGui::Separator();
			{
				// Motion vectors (pass + target built in RenderFrame) +
				// motion blur (HDR velocity gather before bloom/tonemap).
		if (m_passes.motionVectors && m_viewportResources.motionVectors.Valid()) {
					ImGui::Checkbox("Show motion vectors", &m_showMotionDebug);
					if (m_showMotionDebug) {
						ImGui::SliderFloat("Motion debug scale", &m_motionDebugScale, 1.0f, 128.0f);
						ImGui::TextDisabled("Direction -> hue, magnitude -> brightness");
					}
				} else {
					ImGui::TextDisabled("Motion vectors: target/shaders missing");
				}
				ImGui::Checkbox("Motion blur (HDR velocity gather)", &m_motionBlurEnabled);
				if (m_motionBlurEnabled) {
					ImGui::SliderInt("Blur samples", &m_motionBlurSamples, 1, 32);
					ImGui::SliderFloat("Blur max radius (px)", &m_motionBlurMaxRadius, 0.0f, 64.0f);
					ImGui::SliderFloat("Blur strength", &m_motionBlurStrength, 0.0f, 2.0f);
					if (!m_viewportResources.motionBlur.Valid())
						ImGui::TextDisabled("Motion blur target/shaders missing – sharp HDR");
					else
						ImGui::TextDisabled("Gather along motion vectors, pre-tonemap HDR");
				}
				ImGui::Checkbox("SSAO (half-res, depth only)", &m_ssaoEnabled);
				if (m_ssaoEnabled) {
					ImGui::SliderFloat("AO radius (m)", &m_ssaoRadius, 0.05f, 2.0f);
					ImGui::SliderFloat("AO bias", &m_ssaoBias, 0.0f, 0.1f);
					ImGui::SliderFloat("AO intensity", &m_ssaoIntensity, 0.0f, 2.0f);
					ImGui::SliderFloat("AO max distance (m)", &m_ssaoMaxDistance, 2.0f, 100.0f);
					ImGui::SliderInt("AO samples", &m_ssaoSamples, 1, 32);
					ImGui::SliderFloat("AO blur sharpness", &m_ssaoBlurSharpness, 0.0f, 32.0f);
					const auto& ssao = m_viewportResources.ssao;
					if (!ssao.Valid())
						ImGui::TextDisabled("SSAO target/shaders missing – no AO");
					else
						ImGui::Text("SSAO: half %ux%u (full %ux%u)",
							ssao.halfExtent.width, ssao.halfExtent.height,
							ssao.fullExtent.width, ssao.fullExtent.height);
				}
			}
			ImGui::Separator();
			{
				const auto& fwd = m_viewportResources.forwardPlus;
				ImGui::Text("Forward+: %ux%u tiles (%u)  tile %u  max/tile %u",
					fwd.tileCountX, fwd.tileCountY, fwd.tileCount,
					FP_TILE_SIZE, FP_MAX_PER_TILE);
				ImGui::Checkbox("Tiled point/spot lights", &m_fpDemo.enabled);
				ImGui::Checkbox("Animate", &m_fpDemo.animate);
				ImGui::SliderFloat("Speed", &m_fpDemo.speed, 0.0f, 3.0f);
				int gx = m_fpDemo.gridX, gy = m_fpDemo.gridY, gz = m_fpDemo.gridZ;
				if (ImGui::SliderInt("Grid X", &gx, 1, 12)) m_fpDemo.gridX = gx;
				if (ImGui::SliderInt("Grid Y", &gy, 1, 8)) m_fpDemo.gridY = gy;
				if (ImGui::SliderInt("Grid Z", &gz, 1, 12)) m_fpDemo.gridZ = gz;
				ImGui::SliderFloat("Spacing X", &m_fpDemo.spacingX, 0.5f, 8.0f);
				ImGui::SliderFloat("Spacing Y", &m_fpDemo.spacingY, 0.5f, 6.0f);
			ImGui::SliderFloat("Spacing Z", &m_fpDemo.spacingZ, 0.5f, 8.0f);
				ImGui::SliderFloat("Range", &m_fpDemo.range, 1.0f, 30.0f);
				ImGui::SliderFloat("Intensity (cd)", &m_fpDemo.intensity, 0.0f, 2000.0f);
				ImGui::SliderFloat("Ball radius (m)", &m_fpDemo.ballRadius, 0.02f, 0.5f);
				ImGui::SliderFloat("Ball emissive (HDR)", &m_fpDemo.ballEmissive, 0.0f, 5000.0f);
				int spots = m_fpDemo.spotCount;
				if (ImGui::SliderInt("Spots", &spots, 0, 16)) m_fpDemo.spotCount = spots;
				ImGui::SliderFloat("Spot intensity (cd)", &m_fpDemo.spotIntensity, 0.0f, 4000.0f);
				const int wantLights = m_fpDemo.enabled
					? (m_fpDemo.gridX * m_fpDemo.gridY * m_fpDemo.gridZ + m_fpDemo.spotCount) : 0;
				ImGui::Text("Lights: %d / %u%s", wantLights, FP_MAX_LIGHTS,
					wantLights > (int)FP_MAX_LIGHTS ? "  (CLAMPED!)" : "");
				ImGui::Text("Light balls: %u active / %u nodes (scene hierarchy)",
					m_activePointBalls, (uint32_t)m_lightBalls.size());
				ImGui::TextDisabled("Point lights = emissive ball nodes (unlit, never cast shadows)");
				if (fwd.depthBindlessIndex == UINT32_MAX)
					ImGui::TextDisabled("Depth bindless: MISSING (cull falls back to empty)");
				ImGui::Separator();
				ImGui::Text("Spot shadows: %u / %u tiles  atlas %ux%u",
					m_spotShadowCount, smvk::kMaxSpotShadows,
					m_viewportResources.spotShadows.atlasExtent.width,
					m_viewportResources.spotShadows.atlasExtent.height);
				ImGui::Checkbox("Spot shadows (per-light flag)", &m_fpDemo.spotShadowsEnabled);
				int spotMax = m_fpDemo.spotShadowMax;
				if (ImGui::SliderInt("Shadow spots max", &spotMax, 0, (int)smvk::kMaxSpotShadows))
					m_fpDemo.spotShadowMax = spotMax;
				ImGui::SliderFloat("Shadow max distance (m)", &m_fpDemo.spotShadowMaxDistance, 5.0f, 120.0f);
			ImGui::TextDisabled("Select: intensity*cone*coverage/dist^2, NDC-box + distance culled");
			if (!m_viewportResources.spotShadows.Valid())
				ImGui::TextDisabled("Spot atlas: MISSING (spots run unshadowed)");
			ImGui::Separator();
			ImGui::Text("Light cookies & functions");
			ImGui::Checkbox("Spot cookies (gobo)", &m_fpDemo.cookiesEnabled);
			ImGui::Checkbox("Light functions (spots)", &m_fpDemo.lightFunctionsEnabled);
			const char* functionNames[] = { "None", "Flicker", "Pulse", "Cone bands" };
			int fn = m_fpDemo.functionType;
			if (ImGui::Combo("Function", &fn, functionNames, 4))
				m_fpDemo.functionType = fn;
			ImGui::SliderFloat("Fn speed", &m_fpDemo.functionSpeed, 0.05f, 5.0f);
			ImGui::SliderFloat("Fn depth", &m_fpDemo.functionScale, 0.0f, 1.0f);
			ImGui::TextDisabled("Cookies: soft disc / blinds / starburst per spot; patterns modulate light color");
			}
			ImGui::Separator();
			ImGui::Text("GfxFilter (per-view triangle filter)");
			ImGui::Checkbox("Enable filtered main view", &m_useGfxFilter);
			ImGui::Checkbox("Enable filtered motion vectors", &m_useFilteredMotion);
			{
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
				ImGui::Checkbox("Freeze culling view (debug)", &m_freezeCullingView);
				ImGui::TextDisabled("Holds frustum/eye/HZB; render camera stays live.\n"
					"Move the camera to inspect what the frozen view culled.");
				ImGui::Separator();
				ImGui::Text("Meshlet debug views (bypass GPU cull)");
				ImGui::Checkbox("Meshlet colors (replaces opaque)", &m_debugMeshlets);
				const char* meshletModes[] = { "Random per meshlet", "Cone-cutoff heat" };
				ImGui::Combo("Meshlet color mode", &m_debugMeshletMode, meshletModes, 2);
				ImGui::Checkbox("Cone axes (line overlay)", &m_debugCones);
				if (!m_gfxRenderer.DebugProgramsValid())
					ImGui::TextDisabled("Debug programs missing – views disabled");
				if (ImGui::Button("Dump indirect audit (stderr, stalls)"))
					m_dumpIndirectAudit = true;
				ImGui::SameLine();
				if (ImGui::Button("Dump HZB (stderr, stalls)"))
					m_debugDumpHzb = true;
				ImGui::TextDisabled("Colors: every registered cluster direct; culled ones stay visible here.\n"
					"Heat: red = degenerate cone (never culls), green = tight cone.");
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
				// test is silently disabled — use Dump HZB + Dump indirect
				// audit (stderr) to confirm what the GPU saw.
				{
					const bool occOn = (m_gfxRenderer.config.cullFlags
						& smvk::gfx::CullFlagBit_HzbOcclusion) != 0;
					uint32_t hw = 0, hh = 0, hm = 0;
					if (gfxSlot != smvk::RenderViewSet::kInvalidIndex
						&& gfxSlot < m_gfxRenderer.State().temporals.size()) {
						const smvk::gfx::TemporalView& ht =
							m_gfxRenderer.State().temporals[gfxSlot];
						hw = ht.hiz.width; hh = ht.hiz.height; hm = ht.hiz.mipCount;
					}
					const bool depthOk = m_viewportResources.forwardPlus.depthBindlessIndex
						!= UINT32_MAX;
					ImGui::Text("hzb: %ux%u/%u mips depthIdx=%s occ=%s cull=%s",
						hw, hh, hm, depthOk ? "ok" : "INVALID", occOn ? "on" : "OFF",
						cullingFrozen ? "FROZEN" : "live");
					if (!occOn)
						ImGui::TextDisabled("HZB flag OFF: no occlusion culls possible.");
					else if (hm == 0)
						ImGui::TextDisabled("HZB empty: occlusion test disabled (extent 0?).");
					else if (!depthOk)
						ImGui::TextDisabled("Depth bindless INVALID: HZB reads garbage.");
				}
				ImGui::TextDisabled("Bisect: disable flags top-down (small/backface/frustum/degen), then bypass.");
			}
			ImGui::Separator();
			ImGui::TextDisabled("[I] %s  [ESC] quit", m_uiMode ? "UI (NORMAL) -> press I for FPS" : "FPS (DISABLED) -> press I for UI");
			ImGui::End();

			// Alternative: draw directly with custom draw lists etc.
			// ImGui::GetForegroundDrawList()->AddCircle(...);

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
				"UiComposite");

			// Manual alternative (equivalent, more explicit):
			// ImGui::Render();
			// m_rendergraph->AddPass("ImGuiManual")
			//     .SetOffset(0,0)
			//     .SetExtent(uiExtent)
			//     .SetColor({m_viewportResources.ui.logicalView}, smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store)
			//     .SetRecordingFunc([this](smvk::CommandBuffer& cmd){ m_imgui->Render(cmd); });
		}

		m_rendergraph->Execute(fb);
		m_context->ProcessDeferrredTasks();

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

		// Snapshot this frame's camera + node transforms for the next
		// frame's motion vector pass. Must run AFTER Execute: the motion
		// pass recording lambda reads these members during Execute.
		m_prevViewProj = frameData.viewProj;
		m_prevViewProjValid = true;
		m_prevGlobalTransforms = m_world->scene.globalTransform;
	}
}

namespace app {

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

	// If ImGui wants mouse, don't apply FPS mouselook unless right-button held?
	// We keep mouse handling outside the keyboard capture branch so cursor
	// reset still works; the positioner internally ignores updates when not pressed.

	// NDC, but to frameBuffer dimensions. 
	//glm::vec2 mousePos = glm::vec2(
	//	(static_cast<float>(m_input.getMouseX()) / static_cast<float>(swapchain->GetExtent().width)) * 2.0f - 1.0f,
	//	1.0f - (static_cast<float>(m_input.getMouseY()) / static_cast<float>(swapchain->GetExtent().height)) * 2.0f
	//);

	// NDC, to window dimensions. These two may not match, depends on DPI settings. Be careful.
	// m_input should be normalized and not be affected by resolution!
	glm::vec2 mousePos = glm::vec2(
		(static_cast<float>(m_input.getMouseX()) / static_cast<float>(m_swapchain->GetWindowWidth())) * 2.0f - 1.0f,
		1.0f - (static_cast<float>(m_input.getMouseY()) / static_cast<float>(m_swapchain->GetWindowHeight())) * 2.0f
	);

	const bool mousePressed =
		m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_RIGHT);

	if (m_input.isMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT))
	{
		m_cameraPositioner.resetMousePosition(mousePos);
	}

	m_cameraPositioner.update(
		static_cast<double>(dt),
		mousePos,
		mousePressed
	);
}

void SceneRenderer::SetUIMode(bool ui)
{
	m_uiMode = ui;
	glfwSetInputMode(window, GLFW_CURSOR, ui ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
	if (ui) {
		// Center cursor for UI interaction; let ImGui take over.
		// Don't reset camera mouse pos on mode switch to avoid jump when returning to FPS.
	} else {
		// When returning to FPS, sync positioner to current mouse to avoid jump.
		glm::vec2 mousePos = glm::vec2(
			(static_cast<float>(m_input.getMouseX()) / static_cast<float>(m_swapchain->GetWindowWidth())) * 2.0f - 1.0f,
			1.0f - (static_cast<float>(m_input.getMouseY()) / static_cast<float>(m_swapchain->GetWindowHeight())) * 2.0f
		);
		m_cameraPositioner.resetMousePosition(mousePos);
	}
}

} // namespace app

// NOTE: entry point moved to main.cpp (scene selection + Configure).
