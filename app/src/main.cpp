#pragma once
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <array>
#include <cmath>
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
#include <Engine/Rendergraph/motionvector.h>
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
#include <imgui.h>
#include <glm/gtc/constants.hpp>


// Constants //
static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;
std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
};
// Cooked pak for the scene above (VFS root is the content dir, so the pak
// holds "models/..." VFS paths). Built with:
//   PakCooker --content-dir app/content --in-vfs models/Sponza/sponza.gltf
//             --out-pak app/content/paks/sponza.pak --meshlets on
// (--meshlets on persists per-mesh meshlet/cone data, which the
// GPU-driven filter consumes via RegisterMeshFromCooked.)
static const char* pak_path = "content/paks/sponza.pak";

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
static constexpr uint32_t CASCADE_RES = 2048; // per cascade tile (2048 -> 4096 atlas for 4 cascades, Option B)
static constexpr float CASCADE_LAMBDA = 0.92f; // 0 = linear, 1 = logarithmic
static constexpr float CASCADE_Z_PADDING = 1.0f;
static constexpr smvk::Format SHADOW_FORMAT = smvk::Format::D32_Float; // stencil dropped for directional atlas (Option B)

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
static constexpr float SPOT_SHADOW_BIAS = 0.002f;

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

// Procedural demo lights (grid of points + a few orbiting spots).
struct ForwardPlusDemo {
	bool enabled = true;
	int gridX = 8;
	int gridY = 3;
	int gridZ = 8;
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
	// Emissive ball objects for the point lights (see PointLightBall): the
	// point lights are real nodes in the scene graph, each carrying a small
	// unlit emissive sphere. Balls never cast shadows (flag cleared).
	float ballRadius = 0.12f;     // world-space radius of each ball (m)
	float ballEmissive = 1200.0f; // HDR emissive strength (bloom threshold ~1000)
};

// One scene-graph node per demo point light: an emissive ball mesh with an
// unlit material. The node's global transform is the light position (the
// Forward+ light buffer is built from it, see UpdateForwardPlusLights);
// the ball itself renders as pure emissive (no lighting, no shadows).
struct PointLightBall {
	NodeId node = UINT32_MAX;
	bool active = false;
};

// Data definitions //
struct DrawSetData {
	float transform[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	glm::vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
};
static_assert(sizeof(DrawSetData) % 16 == 0);

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
	float padding; // keep 16-byte alignment

	// Configurable skydome IBL + fog (matches HLSL FrameUniforms.env).
	smvk::EnvironmentFrameData env;
};
static_assert(sizeof(FrameUniforms) % 16 == 0);

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
	smvk::Holder<smvk::ShaderModuleHandle> fragShader;
	smvk::ShaderProgram mainProgram;
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
	std::vector<smvk::LogicalImageViewHandle> swapImages;
	smvk::DepthTarget renderDepth; // scene depth at internal resolution + Ensure
	smvk::HdrTarget hdr; // HDR intermediate (image + view + bindless + logical) + Ensure
	smvk::MsaaTarget msaa; // transient 2x color+depth for Main, resolves to hdr
	smvk::MotionVectors motionVectors; // RG16F motion target + program + pass helper
	smvk::UiTarget ui; // separated UI pass target (ui scale) + Ensure
	smvk::UiComposite uiComposite; // UI -> swapchain alpha composite program + pass
	bool msaaEnabled = true; // runtime toggle (effective only if device supports)
};


class TestApp : public smvk::AppImpl {
	using Super = smvk::AppImpl;
private:
	void UpdateCamera(float dt);
	void SetUIMode(bool ui);
	bool m_uiMode = false;
	// Configurable resolution knobs (see "Resolution config" above).
	float m_renderScale = 1.0f;
	float m_uiScale = 1.0f;
	float m_uiDpiScale = 1.0f; // synced from ImGuiOverlay at init (auto = monitor DPI)
	//

	void EarlyUpdate() override {
		Super::EarlyUpdate();
	}

	void Update() {
		Super::Update();
		if (m_input.isKeyPressed(GLFW_KEY_I)) {
			SetUIMode(!m_uiMode);
		}
		if (!m_uiMode) {
			UpdateCamera(delta_time_s);
		}
	}

	void LateUpdate() {
		Super::LateUpdate();
	}

	bool Initialize() override {
		if (!Super::Initialize())
			return false;

		m_camera.setPerspective(
			45.0f,
			float(m_swapchain->GetExtent().width) / float(m_swapchain->GetExtent().height),
			default_near_clip, default_far_clip
		);
		std::filesystem::path p(gltf_meshes_paths[0]);
		//m_context->ProcessDeferrredTasks();
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
			std::cerr << "[TestApp] Failed to create render views buffer\n";
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

		smvk::AllocateStaticVertexFmt(*m_context); // dont like this

		// Scene import: prefer the cooked pak (fast path, meshopt'd meshes,
		// embedded textures), fall back to glTF via the VFS when no pak is
		// present (dev iteration). Either way the Cluster is registered
		// with the World and resolved into the runtime scene graph.
		bool loadedFromPak = false;
		{
			std::ifstream pakFile(pak_path, std::ios::binary | std::ios::ate);
			if (pakFile) {
				const auto pakSize = pakFile.tellg();
				pakFile.seekg(0);
				auto pakImage = std::make_shared<const std::vector<uint8_t>>(
					static_cast<size_t>(pakSize));
				// Texture/material/mesh uploads below are synchronous, so
				// the pak image only needs to outlive this block; the
				// PakBackend mount serves VFS texture reads during init
				// (below loose content/ so dev overrides win).
				if (pakFile.read(reinterpret_cast<char*>(
						const_cast<uint8_t*>(pakImage->data())),
						pakSize)) {
					std::string pakErr;
					auto reader = smvk::PakReader::Open(
						std::span<const uint8_t>(*pakImage), pakErr);
					if (reader) {
						try {
							m_world->LoadClusterFromPak(*m_context, *reader);
							// Serve pak-embedded textures through the VFS
							// (below loose content/ so dev overrides win).
							std::string mountErr;
							if (auto pakBackend = smvk::PakBackend::Open(
									pakImage, pak_path, mountErr)) {
								contentVfs.Mount(pakBackend, 10, pak_path);
								loadedFromPak = true;
							}
							else {
								std::cerr << "[TestApp] PakBackend mount failed: "
									<< mountErr << std::endl;
							}
						}
						catch (const std::exception& e) {
							std::cerr << "[TestApp] Pak load failed, falling back "
								<< "to glTF: " << e.what() << std::endl;
						}
					}
					else {
						std::cerr << "[TestApp] Pak parse failed (" << pakErr
							<< "), falling back to glTF" << std::endl;
					}
				}
			}
		}
		if (!loadedFromPak) {
			tinygltf::Model model;
			if (!LoadModelFromAsciiFile(model, gltf_meshes_paths[0].c_str())) {
				return false;
			}

			auto& cluster = m_world->LoadCluster(*m_context);
			GltfToCluster(*m_context, model, gltf_meshes_paths[0], cluster);
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
		if (m_useGfxFilter) {
			m_gfxRenderer.ConfigureFrag(m_resources.fragShader);
			m_gfxRenderer.ConfigureSeedFrag(m_resources.shadowMaskedFragShader);
			if (!m_gfxRenderer.Ensure(*m_context, *m_rendergraph)) {
				std::cerr << "[TestApp] GfxFilter unavailable - direct draws\n";
				m_useGfxFilter = false;
			}
			else {
				m_gfxRenderer.RegisterWorldMeshes(*m_world);
				m_gfxRenderer.UploadStaticData(*m_context, *m_rendergraph);
				if (smvk::gfx::ClusterCount(m_gfxRenderer.State()) == 0) {
					std::cerr << "[TestApp] GfxFilter: no clusters - direct draws\n";
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
			std::cerr << "[TestApp] Failed to create cascaded shadow atlas\n";
			return false;
		}
		// Spot-shadow atlas (fixed kMaxSpotShadows slots; selection caps usage).
		// Non-fatal: without it all spots simply run unshadowed.
		if (!m_viewportResources.spotShadows.Ensure(*m_context, *m_rendergraph, MakeSpotShadowDesc())) {
			std::cerr << "[TestApp] Spot shadow atlas unavailable – spots run unshadowed\n";
		}
		if (!m_viewportResources.tonemap.Ensure(*m_context, MakeTonemapDesc())) {
			std::cerr << "[TestApp] Failed to create tonemap program\n";
			return false;
		}
		// Skydome program (fullscreen-triangle sky.vert/sky.frag). Failure is
		// non-fatal: the Main pass simply skips the sky draw and keeps the
		// clear color.
		if (!m_skydome.Ensure(*m_context)) {
			std::cerr << "[TestApp] Skydome program unavailable – running without sky\n";
		}

		{
			auto vkExt = m_swapchain->GetExtent();
			const smvk::Extent2D swapExtent{ vkExt.width, vkExt.height };
			const smvk::Extent2D renderExtent = ScaledExtent(swapExtent, m_renderScale);
			const smvk::Extent2D uiExtent = ScaledExtent(swapExtent, m_uiScale);
			smvk::DepthDesc depthDesc{};
			depthDesc.format = m_swapchain->GetDepthFormat();
			if (!m_viewportResources.renderDepth.Ensure(*m_context, *m_rendergraph, renderExtent, depthDesc)) {
				std::cerr << "[TestApp] Failed to create render depth target\n";
				return false;
			}
			if (!m_viewportResources.forwardPlus.Ensure(*m_context, *m_rendergraph,
				renderExtent, MakeForwardPlusDesc(), m_viewportResources.renderDepth.view)) {
				std::cerr << "[TestApp] Failed to create Forward+ tile buffers\n";
				return false;
			}
			if (!m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, renderExtent)) {
				std::cerr << "[TestApp] Failed to create HDR target\n";
				return false;
			}
			{
				smvk::MsaaDesc msaaDesc{};
				msaaDesc.depthFormat = m_swapchain->GetDepthFormat();
				if (!m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, renderExtent, msaaDesc)) {
					std::cerr << "[TestApp] MSAA target unavailable – falling back to 1x\n";
				}
			}
			if (!m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc())) {
				std::cerr << "[TestApp] Bloom chain unavailable (HDR "
					<< renderExtent.width << "x" << renderExtent.height << ")\n";
			}
			smvk::UiDesc uiDesc{};
			uiDesc.format = m_swapchain->GetSurfaceFormat();
			if (!m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, uiDesc)) {
				std::cerr << "[TestApp] UI target unavailable – UI disabled\n";
			}
			if (!m_viewportResources.uiComposite.Ensure(*m_context)) {
				std::cerr << "[TestApp] UI composite program unavailable\n";
			}
		}

		// ImGui overlay – init after swapchain/rendergraph are ready.
		// Uses dynamic rendering; no external RenderPass needed.
		m_imgui = std::make_unique<smvk::ImGuiOverlay>();
		smvk::ImGuiOverlayDesc imguiDesc{};
		imguiDesc.fontPath = UI_FONT_PATH;
		imguiDesc.fontBaseSize = UI_FONT_BASE_SIZE;
		if (!m_imgui->Init(*m_context, *m_swapchain, window, imguiDesc)) {
			std::cerr << "[TestApp] Failed to init ImGuiOverlay – continuing without UI\n";
			m_imgui.reset();
		} else {
			m_uiDpiScale = m_imgui->AppliedDpiScale(); // auto = monitor content scale
			SetUIMode(false); // start in FPS (DISABLED), I toggles to UI (NORMAL)
		}

		return true;
	};

	void Shutdown() override {
		// Ensure GPU idle before destroying ImGui Vulkan resources/Holders.
		WaitForDeviceIdle();
		m_viewportResources.forwardPlus.Destroy(*m_rendergraph);
		m_viewportResources.bloom.Destroy(*m_rendergraph);
		m_viewportResources.spotShadows.Destroy(*m_rendergraph);
		m_viewportResources.directionalShadows.Destroy(*m_rendergraph);
		m_viewportResources.msaa.Destroy(*m_rendergraph);
		m_viewportResources.hdr.Destroy(*m_rendergraph);
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

	FrameUniforms UpdateFrameUniforms(uint32_t currentFrame, smvk::Extent2D renderExtent) {

		const int width_ = renderExtent.width;
		const int height_ = renderExtent.height;

		auto view = m_camera.getViewMatrix();
		auto proj = m_camera.getProjMatrix();
		proj[1][1] *= -1; // viewport fix ditecrly in proj

		auto viewProj = proj * view;
		FrameUniforms data;
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
		data.farPlane = default_far_clip;
		// Linear exposure multiplier for the tonemapper (ACES).
		// Sun is 5000 lux (see UpdateLights); 0.001 puts a sun-facing
		// mid-gray (albedo 0.18, N·L=1) at HDR ≈ 0.29 pre-ACES → no clipping.
		// NOTE: FrameUniforms.exposure is currently unread by shaders; the
		// tonemap push constant (TONEMAP_EXPOSURE) is the live knob. Keep in sync.
		data.exposure = 0.001f;
		// Configurable skydome IBL + fog (single source for sky/sun/fog).
		data.env = m_env.ToFrameData();

		m_context->UploadDataToBuffer(
			m_resources.frameUniforms,
			&data,
			sizeof(FrameUniforms)/* * 1*/,
			smvk::PipelineStageBit_Vertex,
			smvk::AccessFlags::ShaderRead,
			0,
			currentFrame * sizeof(FrameUniforms)
			//currentFrame * m_resources.frameUniformsBuffer.key.elementSize * m_resources.frameUniformsBuffer.key.elementCount // dynamicOffsetPerFrame
		);
		return data;

	}

	smvk::GpuDirectionalLightDescription UpdateLights(uint32_t currentFrame)
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
		auto splits = smvk::ComputeCascadeSplits(default_near_clip, default_far_clip, CASCADE_LAMBDA, NUM_CASCADES);

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
			smvk::PipelineStageBit_Vertex,
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
			smvk::PipelineStageBit_Vertex,
			smvk::AccessFlags::ShaderRead,
			0,
			dirOffset
		);
		return dir;
	}

	void UpdateMaterials(uint32_t currentFrame) {
		// Keep the shared emissive-ball materials in sync with the demo knob
		// (bloom feeds off this; uploaded below with all other materials).
		if (m_lightCluster) {
			if (smvk::Material* mat = m_world->GetMaterial(m_ballMatWarmIdx))
				mat->values.params2.z = m_fpDemo.ballEmissive;
			if (smvk::Material* mat = m_world->GetMaterial(m_ballMatCoolIdx))
				mat->values.params2.z = m_fpDemo.ballEmissive;
		}

		std::vector<smvk::MaterialValues> mats;
		mats.reserve(m_world->materialLocations.size());

		for (uint32_t i = 0; i < m_world->materialLocations.size(); i++) {
			auto* mat = m_world->GetMaterial(i);
			if (mat) {
				mats.push_back(mat->values);
			}
		}

		//constexpr size_t MAX_MATERIALS = 1024;
		constexpr size_t MATERIAL_FRAME_STRIDE =
			sizeof(smvk::MaterialValues) * MAX_MATERIALS;

		size_t frameOffset =
			currentFrame * MATERIAL_FRAME_STRIDE;

		m_context->UploadDataToBuffer(
			m_resources.materialsBuffer,
			mats.data(),
			sizeof(smvk::MaterialValues) * mats.size(),
			smvk::PipelineStageBit_Vertex,
			smvk::AccessFlags::ShaderRead,
			0,
			frameOffset
		);
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
	void CreatePointLightBalls() {
		smvk::Cluster& cluster = m_world->LoadCluster(*m_context);
		m_lightCluster = &cluster;
		// Retain the CPU upload so the per-view filter can cook meshlet
		// clusters at register time (see RegisterWorldMeshes).
		cluster.SetRetainUploads(true);

		const smvk::ClusterStaticMeshHandle mesh =
			cluster.UploadStaticMesh(MakeUvSphereUpload(16, 24));
		if (mesh.empty()) {
			std::cerr << "[TestApp] Light-ball mesh upload failed - scene-node lights disabled\n";
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
			std::cerr << "[TestApp] Light-ball material creation failed - scene-node lights disabled\n";
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

	void FixupPointLightBallNodes() {
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
			std::cerr << "[TestApp] Light-ball fixup failed (nodes=" << m_lightBalls.size()
				<< ") - scene-node lights disabled\n";
			m_lightCluster = nullptr;
			m_lightBalls.clear();
		} else {
			std::cout << "[TestApp] Scene-node point lights: " << m_lightBalls.size()
				<< " emissive balls under 'PointLights' (mesh slot " << m_ballMeshIdx
				<< ", warm/cool materials " << m_ballMatWarmIdx << "/"
				<< m_ballMatCoolIdx << ")\n";
		}
	}

	// Grows the ball node pool on demand (grid sliders). New nodes are
	// appended directly to the resolved world scene (the ball cluster is
	// merged once at startup) and share the cluster's mesh/materials.
	void EnsurePointLightBalls(uint32_t count) {
		if (!m_lightCluster || m_ballMeshIdx == UINT32_MAX || m_ballMatWarmIdx == UINT32_MAX)
			return;
		Scene& scene = m_world->scene;
		if (m_world->flags.size() < scene.hierarchy.size())
			m_world->flags.resize(scene.hierarchy.size(), smvk::IteratorFlagBit_None);
		while (m_lightBalls.size() < count) {
			const int parent = m_lightRootNode >= 0 ? m_lightRootNode : 0;
			const int n = addNode(scene, parent, getNodeLevel(scene, parent) + 1);
			scene.localTransform[n] = glm::mat4(1.0f);
			scene.globalTransform[n] = glm::mat4(1.0f);
			scene.meshForNode[n] = m_ballMeshIdx;
			scene.materialForNode[n] = m_ballMatWarmIdx;
			scene.lightForNode[n] = (uint32_t)m_lightBalls.size();
			setNodeName(scene, n, "PLBall_" + std::to_string(m_lightBalls.size()));
			m_lightBalls.push_back(PointLightBall{ .node = (NodeId)n, .active = false });
		}
	}

	// Per-frame ball node drive: transforms (position + bob + radius),
	// warm/cool material of the visible ball, and iterator flags. Active
	// balls render (Renderable|Opaque) but never enter shadow passes
	// (no CastShadow); inactive pool leftovers stay as inert light nodes.
	void UpdatePointLightBalls() {
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
	uint32_t UpdateForwardPlusLights(uint32_t currentFrame, const FrameUniforms& frame) {
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
				// Points never cast shadows (no shadow tile).
				l.shadowTile = -1;
				l.shadowMapIndex = -1;
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
		if (m_fpDemo.spotShadowsEnabled && m_viewportResources.spotShadows.Valid() && !lights.empty()) {
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

	void RebuildSwapViews() {
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
		smvk::MsaaDesc rebuildMsaaDesc{};
		rebuildMsaaDesc.depthFormat = m_swapchain->GetDepthFormat();
		m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, renderExtent, rebuildMsaaDesc);
		m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, renderExtent, MakeBloomDesc());
		smvk::UiDesc rebuildUiDesc{};
		rebuildUiDesc.format = m_swapchain->GetSurfaceFormat();
		m_viewportResources.ui.Ensure(*m_context, *m_rendergraph, uiExtent, rebuildUiDesc);
	}

	void DrawFrame() {
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

	void RenderFrame(smvk::Framebuffer fb) {

		m_rendergraph->NewFrame();
		const smvk::Extent2D swapExtent{ fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height };
		const smvk::Extent2D renderExtent = ScaledExtent(swapExtent, m_renderScale);
		const smvk::Extent2D uiExtent = ScaledExtent(swapExtent, m_uiScale);
		{
			smvk::DepthDesc frameDepthDesc{};
			frameDepthDesc.format = m_swapchain->GetDepthFormat();
			m_viewportResources.renderDepth.Ensure(*m_context, *m_rendergraph, renderExtent, frameDepthDesc);
			// Tile counts must be fresh for consumers below; the renderer
			// re-checks sizing at pass-build time (resize-safe).
			m_fpRenderer.EnsureSized(*m_context, *m_rendergraph,
				renderExtent, m_viewportResources.renderDepth.view);
			m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, renderExtent);
			m_viewportResources.motionVectors.Ensure(*m_context, *m_rendergraph, renderExtent);
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
		app::AppFrameContext fc{};
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
		// Shadow/depth/motion passes stay on direct draws.
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
		{
			const char* cascadeNames[4] = { "ShadowCascade0", "ShadowCascade1", "ShadowCascade2", "ShadowCascade3" };
			for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci) {
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
					cmd.BindRenderPipeline(
						m_viewportResources.directionalShadows.program,
						smvk::VertexFormat::StaticVertexFormat,
						smvk::DepthSettings{
							.depthFunc = smvk::CompareOp::LessEqual,
							.writeEnable = true,
							.depthTestEnable = true,
							.depthBiasEnable = true,
							.depthBiasConstantFactor = 0.1f,
							.depthBiasClamp = 0.0f,
							.depthBiasSlopeFactor = 1.f
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
							.depthBiasEnable = true,
							.depthBiasConstantFactor = 0.1f,
							.depthBiasClamp = 0.0f,
							.depthBiasSlopeFactor = 1.f
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
		if (m_spotShadowCount > 0 && m_viewportResources.spotShadows.Valid()) {
			const uint32_t spotCount = std::min(m_spotShadowCount, m_viewportResources.spotShadows.slotCount);
			const char* spotNames[8] = { "SpotShadow0", "SpotShadow1", "SpotShadow2", "SpotShadow3",
				"SpotShadow4", "SpotShadow5", "SpotShadow6", "SpotShadow7" };
			static_assert(sizeof(spotNames) / sizeof(spotNames[0]) >= smvk::kMaxSpotShadows);
			for (uint32_t si = 0; si < spotCount; ++si) {
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
					cmd.BindRenderPipeline(
						m_viewportResources.spotShadows.program,
						smvk::VertexFormat::StaticVertexFormat,
						smvk::DepthSettings{
							.depthFunc = smvk::CompareOp::LessEqual,
							.writeEnable = true,
							.depthTestEnable = true,
							.depthBiasEnable = true,
							.depthBiasConstantFactor = 0.1f,
							.depthBiasClamp = 0.0f,
							.depthBiasSlopeFactor = 1.f
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
							.depthBiasEnable = true,
							.depthBiasConstantFactor = 0.1f,
							.depthBiasClamp = 0.0f,
							.depthBiasSlopeFactor = 1.f
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
		if (m_useGfxFilter)
			m_gfxRenderer.AppendPasses(fc);
		gfxSlot = m_gfxRenderer.LastSlot();
		fc.seedDepthPrimed = m_useGfxFilter && m_gfxRenderer.SeededThisFrame();
		// Depth prepass (Loads seeded depth when primed, Clears otherwise).
		m_fpRenderer.AppendPrepass(fc);

		// ---- HZB build over depth (seed or full prepass; T2 module) ----
		// ---- Filter compute dispatch (reads this frame's HZB; T4) ----
		// ---- Depth pass 2 over newly visible clusters (T5) + refresh ----
		// ---- Tile cull over complete depth (moved after pass 2; T5) ----
		if (m_useGfxFilter) {
			m_gfxRenderer.AppendHizPasses(fc,
				m_viewportResources.renderDepth.logicalView,
				m_viewportResources.forwardPlus.depthBindlessIndex);
			m_gfxRenderer.AppendFilterDispatch(fc);
			if (m_gfxRenderer.RemainderPending()) {
				m_gfxRenderer.AppendRemainderPass(fc);
				m_gfxRenderer.AppendHizPasses(fc,
					m_viewportResources.renderDepth.logicalView,
					m_viewportResources.forwardPlus.depthBindlessIndex);
			}
		}
		m_fpRenderer.AppendTileCull(fc);

		// ---------------------------------------------------------------------
		// Motion vectors – per-pixel prevUV - currUV into RG16F (motionvector.h)
		// Re-rasterizes opaque geometry depth-tested READ-ONLY against the 1x
		// prepass depth (reversed-Z GreaterEqual, no writes) – identical in
		// MSAA / non-MSAA modes, Main's transient 2x depth is never touched.
		// MASK cutouts share the program: the FS alpha-tests only when
		// shadingModel == Masked (Cull None phase, like the prepass).
		// ---------------------------------------------------------------------
		if (m_viewportResources.motionVectors.Valid()) {
			PushConstants motionPC = pc;
			auto& motionPass = m_viewportResources.motionVectors.AddMotionVectorPass(
				*m_rendergraph,
				m_viewportResources.renderDepth.logicalView,
				m_viewportResources.motionVectors.target.logicalView,
				renderExtent,
				"MotionVectors");
			motionPass.SetRecordingFunc([this, motionPC](smvk::CommandBuffer& cmd)
			{
				auto drawMotionNode = [&](NodeId node, bool maskedPhase) {
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
			(void)motionPass;
		}

		const bool useMsaa = m_viewportResources.msaaEnabled &&
			m_context->SupportsMSAA2x() && m_viewportResources.msaa.Valid();
		std::vector<smvk::LogicalImageViewHandle> mainInputs;
		mainInputs.push_back(m_viewportResources.directionalShadows.logicalView);
		if (m_viewportResources.spotShadows.Valid())
			mainInputs.push_back(m_viewportResources.spotShadows.logicalView);
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
		mainPass.SetRecordingFunc([this, pc, useMsaa, gfxSlot, renderViewsAddress, mainRenderView](smvk::CommandBuffer& cmd)
	{
		// 1x: depth was filled by the Forward+ prepass, main is read-only.
		// 2x: transient depth is cleared above, main writes it
		// (GreaterEqual reversed-Z in both cases).
		const smvk::DepthSettings mainDepth{
			.depthFunc = smvk::CompareOp::GreaterEqual,
			.writeEnable = useMsaa,
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

		auto bindForMaterial = [&](uint32_t materialId, smvk::ShaderProgram& prog) {
			// Per-material raster (doubleSided -> Cull None) + blend
			// (Opaque vs AlphaBlend). 1x depth is read-only, 2x writes.
			smvk::BlendSettings blend = smvk::BlendSettings::Opaque();
			smvk::RasterSettings raster{};
			if (smvk::Material* mat = m_world->GetMaterial(materialId)) {
				blend = mat->pipeline.blend;
				raster = mat->pipeline.raster;
			}
			cmd.BindRenderPipeline(
				prog,
				smvk::VertexFormat::StaticVertexFormat,
				mainDepth,
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
		if (m_env.skyEnabled && m_skydome.ShadersValid()) {
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
			mainDepth,
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
		if (gfxActive) {
			const auto& gfxRanges = m_gfxRenderer.MaterialRanges(gfxSlot);
			for (uint32_t ri = 0; ri < (uint32_t)gfxRanges.size(); ++ri) {
				const uint32_t materialId = gfxRanges[ri].material;
				bindForMaterial(materialId, m_gfxRenderer.FilteredProgram());
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
		{
			// UINT32_MAX forces a bind check on the first node; an
			// identical state is deduped by the CommandBuffer's
			// current-pipeline check, so this costs nothing.
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
					bindForMaterial(materialId, m_resources.mainProgram);
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
					bindForMaterial(d.materialId, m_resources.mainProgram);
					boundMaterial = d.materialId;
				}
				drawNode(d.node, d.meshId, d.materialId);
			}
		}
	}
		);
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
		// Bloom – HDR downsample / upsample chain (composable, see bloom.h)
		// ---------------------------------------------------------------------
		// Sampler cache: linear clamp shared by bloom + tonemap blits
		uint32_t linearSampler = m_context->GetOrCreateSampler(smvk::SamplerCreateInfo{
			.magFilter = smvk::Filter::Linear,
			.minFilter = smvk::Filter::Linear,
			.mipmapMode = smvk::MipmapMode::Linear,
			.addressU = smvk::AddressMode::ClampToEdge,
			.addressV = smvk::AddressMode::ClampToEdge,
			.addressW = smvk::AddressMode::ClampToEdge,
			.maxAnisotropy = 1.0f
		});
		if (m_viewportResources.bloom.Valid()) {
			// Default down/up programs owned by BloomChain (Ensure internalized).
			m_viewportResources.bloom.AddBloomPasses(
				*m_rendergraph,
				m_viewportResources.hdr.logicalView,
				m_viewportResources.hdr.bindlessIndex,
				linearSampler);
		}

		// ---------------------------------------------------------------------
		// Tonemapper – HDR (RGBA16F) → LDR swapchain (see tonemap.h)
		// ---------------------------------------------------------------------
		{
			const bool bloomOn = m_viewportResources.bloom.Valid();
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
			if (m_viewportResources.motionVectors.Valid()) {
				motionView = m_viewportResources.motionVectors.target.logicalView;
				motionBindless = m_viewportResources.motionVectors.target.bindlessIndex;
			}
			auto& tonemapPass = m_viewportResources.tonemap.AddTonemapPass(
				*m_rendergraph,
				m_viewportResources.hdr.logicalView,
				m_viewportResources.hdr.bindlessIndex,
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
		if (m_imgui && m_imgui->IsInitialized() && m_viewportResources.ui.Valid()) {
			// Target extent != window size when ui scale != 1: the overlay maps
			// ImGui draw data + font raster density onto the UI target.
			m_imgui->NewFrame(uiExtent);

			// Minimal UI – name + camera pos (as requested)
			ImGui::Begin("VulkanToy — Debug");
			ImGui::Text("Frame %.1f FPS (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
			ImGui::Text("Camera pos: %.2f %.2f %.2f", m_cameraPositioner.cameraPosition_.x, m_cameraPositioner.cameraPosition_.y, m_cameraPositioner.cameraPosition_.z);
			ImGui::Text("Swapchain %ux%u  framesInFlight=%u",
				fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height, fb.swapchain->framesInFlight());
			ImGui::Separator();
			{
				auto splits = smvk::ComputeCascadeSplits(default_near_clip, default_far_clip, CASCADE_LAMBDA, NUM_CASCADES);
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
				// Motion vector debug view (pass + target built in RenderFrame).
				if (m_viewportResources.motionVectors.Valid()) {
					ImGui::Checkbox("Show motion vectors", &m_showMotionDebug);
					if (m_showMotionDebug) {
						ImGui::SliderFloat("Motion debug scale", &m_motionDebugScale, 1.0f, 128.0f);
						ImGui::TextDisabled("Direction -> hue, magnitude -> brightness");
					}
				} else {
					ImGui::TextDisabled("Motion vectors: target/shaders missing");
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
				ImGui::SliderFloat("Intensity (cd)", &m_fpDemo.intensity, 0.0f, 500.0f);
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
				const smvk::gfx::Stats gs = m_gfxRenderer.LastStats();
				ImGui::Text("view: %u draws %u cmds %u tris | clusters %u/%u culled",
					gs.draws, gs.commands, gs.trianglesProcessed,
					gs.clustersCulled, gs.clustersProcessed);
				ImGui::Text("materials: %u ranges  batches: %u",
					(unsigned)m_gfxRenderer.LastMaterialRangeCount(),
					m_gfxRenderer.LastBatchCount());
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

		// Snapshot this frame's camera + node transforms for the next
		// frame's motion vector pass. Must run AFTER Execute: the motion
		// pass recording lambda reads these members during Execute.
		m_prevViewProj = frameData.viewProj;
		m_prevViewProjValid = true;
		m_prevGlobalTransforms = m_world->scene.globalTransform;
	}


private:
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
	app::ForwardPlusRenderer m_fpRenderer{};
	// ---- Motion vector pass: previous-frame state + debug view ----
	glm::mat4 m_prevViewProj{ 1.0f }; // last frame's viewProj (post Y-flip)
	bool m_prevViewProjValid = false; // false until the first frame completes
	std::vector<glm::mat4> m_prevGlobalTransforms; // last frame's node transforms
	bool m_showMotionDebug = false; // tonemap debug view: motion vectors
	float m_motionDebugScale = 16.0f; // debug view velocity amplification
	// ---- Emissive point-light ball objects (scene-hierarchy lights) ----
	smvk::Cluster* m_lightCluster = nullptr; // owns the shared ball resources
	smvk::ClusterStaticMeshHandle m_ballMesh{};
	smvk::ClusterMaterialHandle m_ballMatWarm{};
	smvk::ClusterMaterialHandle m_ballMatCool{};
	uint32_t m_ballMeshIdx = UINT32_MAX;      // world-level slot (post-merge)
	uint32_t m_ballMatWarmIdx = UINT32_MAX;   // world-level slot (post-merge)
	uint32_t m_ballMatCoolIdx = UINT32_MAX;   // world-level slot (post-merge)
	int m_lightRootNode = -1;                 // "PointLights" hierarchy root
	std::vector<PointLightBall> m_lightBalls; // node pool (grows on demand)
	uint32_t m_activePointBalls = 0;          // active pool prefix this frame
	smvk::EnvironmentSettings m_env{};
	smvk::Skydome m_skydome{};
	CameraPositioner_FirstPerson m_cameraPositioner = CameraPositioner_FirstPerson(
		glm::vec3(0.0f, 1.0f, 0.0f), //pos
		glm::vec3(0.0f, 1.0f, 1.0f), //target
		glm::vec3(0.0f, 1.0f, 0.0f) //up
	);
	Camera m_camera = Camera(m_cameraPositioner);
};

void TestApp::UpdateCamera(float dt)
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

void TestApp::SetUIMode(bool ui)
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

int main(int argc, const char* argv[]) {
	if (argc > 1) {
		std::cout << argv[0];
	}

	TestApp app = TestApp();
	try {
		app.Run();
	}
	catch (const std::exception& e) {
		std::cerr << "\nERROR: " << e.what() << std::endl;
		std::cerr << "\nPress Enter to exit...";
		std::cin.get();
		return EXIT_FAILURE;
	}
	//std::cerr << "\nPress Enter to exit...";
	//std::cin.get();
	return EXIT_SUCCESS;
}
