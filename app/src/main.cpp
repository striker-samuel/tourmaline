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
#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/msaa.h>
#include <Engine/Rendergraph/tonemap.h>
#include <Engine/Vfs/vfs.h>
#include <imgui.h>


// Constants //
static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;
std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
};
// Cooked pak for the scene above (VFS root is the content dir, so the pak
// holds "models/..." VFS paths). Built with:
//   PakCooker --content-dir content --in-vfs models/Sponza/sponza.gltf
//             --out-pak content/paks/sponza.pak
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

struct FrameUniforms
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewProj;

	glm::mat4 invView;
	glm::mat4 invProj;
	glm::mat4 invViewProj;

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
	smvk::BloomChain bloom; // separated per-mip HDR chain + default down/up programs + Ensure
	smvk::Tonemapper tonemap; // default tonemap program + pass helper
	smvk::ForwardPlus forwardPlus; // tile buffers + cull/depth programs + Ensure
	std::vector<smvk::LogicalImageViewHandle> swapImages;
	smvk::LogicalImageViewHandle depthView;
	smvk::HdrTarget hdr; // HDR intermediate (image + view + bindless + logical) + Ensure
	smvk::MsaaTarget msaa; // transient 2x color+depth for Main, resolves to hdr
	bool msaaEnabled = true; // runtime toggle (effective only if device supports)
};


class TestApp : public smvk::AppImpl {
	using Super = smvk::AppImpl;
private:
	void UpdateCamera(float dt);
	void SetUIMode(bool ui);
	bool m_uiMode = false;
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

		// VFS overlay: loose content/ for iteration, cooked paks mounted
		// underneath (loose files win on collisions). Shader reads below
		// go through the same overlay so packaged builds just work.
		smvk::OverlayVfs contentVfs;
		contentVfs.Mount(std::make_shared<smvk::LooseBackend>("content"), 0, "content");

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
		m_world->ResolveAndFixup_clusters();
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
		m_viewportResources.depthView = m_rendergraph->AddExternalImageView(
			m_swapchain->GetDepthImageViewHandle(),
			m_swapchain->GetDepthFormat(),
			"depth"
		);

		// Directional cascade atlas + default shadow program (builder owns Ensure).
		if (!m_viewportResources.directionalShadows.Ensure(*m_context, *m_rendergraph, MakeShadowDesc())) {
			std::cerr << "[TestApp] Failed to create cascaded shadow atlas\n";
			return false;
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
			const smvk::Extent2D extent{ vkExt.width, vkExt.height };
			if (!m_viewportResources.forwardPlus.Ensure(*m_context, *m_rendergraph,
				extent, MakeForwardPlusDesc(), m_swapchain->GetDepthImageViewHandle())) {
				std::cerr << "[TestApp] Failed to create Forward+ tile buffers\n";
				return false;
			}
			if (!m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, extent)) {
				std::cerr << "[TestApp] Failed to create HDR target\n";
				return false;
			}
			{
				smvk::MsaaDesc msaaDesc{};
				msaaDesc.depthFormat = m_swapchain->GetDepthFormat();
				if (!m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, extent, msaaDesc)) {
					std::cerr << "[TestApp] MSAA target unavailable – falling back to 1x\n";
				}
			}
			if (!m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, extent, MakeBloomDesc())) {
				std::cerr << "[TestApp] Bloom chain unavailable (HDR "
					<< extent.width << "x" << extent.height << ")\n";
			}
		}

		// ImGui overlay – init after swapchain/rendergraph are ready.
		// Uses dynamic rendering; no external RenderPass needed.
		m_imgui = std::make_unique<smvk::ImGuiOverlay>();
		if (!m_imgui->Init(*m_context, *m_swapchain, window)) {
			std::cerr << "[TestApp] Failed to init ImGuiOverlay – continuing without UI\n";
			m_imgui.reset();
		} else {
			SetUIMode(false); // start in FPS (DISABLED), I toggles to UI (NORMAL)
		}

		return true;
	};

	void Shutdown() override {
		// Ensure GPU idle before destroying ImGui Vulkan resources/Holders.
		WaitForDeviceIdle();
		m_viewportResources.forwardPlus.Destroy(*m_rendergraph);
		m_viewportResources.bloom.Destroy(*m_rendergraph);
		m_viewportResources.directionalShadows.Destroy(*m_rendergraph);
		m_viewportResources.msaa.Destroy(*m_rendergraph);
		m_viewportResources.hdr.Destroy(*m_rendergraph);
		if (m_imgui) {
			m_imgui->Shutdown();
			m_imgui.reset();
		}
		Super::Shutdown();
	}

	FrameUniforms UpdateFrameUniforms(uint32_t currentFrame) {

		int width_ = m_swapchain->GetExtent().width;
		int height_ = m_swapchain->GetExtent().height;

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

	void UpdateLights(uint32_t currentFrame)
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
	}

	void UpdateMaterials(uint32_t currentFrame) {
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

	// Builds the procedural Forward+ demo light set into the per-frame upload
	// buffer. Returns the number of lights written (clamped to FP_MAX_LIGHTS).
	uint32_t UpdateForwardPlusLights(uint32_t currentFrame) {
		std::vector<smvk::ForwardPlusLight> lights;
		lights.reserve(FP_MAX_LIGHTS);
		if (m_fpDemo.enabled) {
			const int gx = std::max(1, m_fpDemo.gridX);
			const int gy = std::max(1, m_fpDemo.gridY);
			const int gz = std::max(1, m_fpDemo.gridZ);
			const float ox = -(gx - 1) * 0.5f * m_fpDemo.spacingX;
			const float oz = -(gz - 1) * 0.5f * m_fpDemo.spacingZ;
			for (int iz = 0; iz < gz && lights.size() + (size_t)m_fpDemo.spotCount < FP_MAX_LIGHTS; ++iz) {
				for (int iy = 0; iy < gy && lights.size() + (size_t)m_fpDemo.spotCount < FP_MAX_LIGHTS; ++iy) {
					for (int ix = 0; ix < gx && lights.size() + (size_t)m_fpDemo.spotCount < FP_MAX_LIGHTS; ++ix) {
						smvk::ForwardPlusLight l{};
						float phase = float(ix * 7 + iy * 13 + iz * 29) * 0.35f;
						float bob = m_fpDemo.animate
							? std::sin(time_since_start_s * m_fpDemo.speed + phase) * 0.5f
							: 0.0f;
						l.position = {
							ox + ix * m_fpDemo.spacingX,
							m_fpDemo.baseY + iy * m_fpDemo.spacingY + bob,
							oz + iz * m_fpDemo.spacingZ
						};
						l.range = m_fpDemo.range;
						// Warm/cool checkerboard so tiling artifacts read clearly.
						bool warm = ((ix + iy + iz) & 1) == 0;
						l.color = warm
							? glm::vec4(1.0f, 0.62f, 0.32f, 1.0f)
							: glm::vec4(0.35f, 0.65f, 1.0f, 1.0f);
						l.intensity = m_fpDemo.intensity;
						l.type = smvk::kForwardPlusPoint;
						lights.push_back(l);
					}
				}
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
				lights.push_back(l);
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
		const smvk::Extent2D extent{ vkExt.width, vkExt.height };
		m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, extent);
		smvk::MsaaDesc rebuildMsaaDesc{};
		rebuildMsaaDesc.depthFormat = m_swapchain->GetDepthFormat();
		m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, extent, rebuildMsaaDesc);
		m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, extent, MakeBloomDesc());
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
		{
			auto vkExt = m_swapchain->GetExtent();
			const smvk::Extent2D extent{ vkExt.width, vkExt.height };
			m_viewportResources.forwardPlus.Ensure(*m_context, *m_rendergraph,
				extent, MakeForwardPlusDesc(), m_swapchain->GetDepthImageViewHandle());
			m_viewportResources.hdr.Ensure(*m_context, *m_rendergraph, extent);
			smvk::MsaaDesc frameMsaaDesc{};
			frameMsaaDesc.depthFormat = m_swapchain->GetDepthFormat();
			m_viewportResources.msaa.Ensure(*m_context, *m_rendergraph, extent, frameMsaaDesc);
			m_viewportResources.bloom.Ensure(*m_context, *m_rendergraph, extent, MakeBloomDesc());
		}
		m_context->UpdateDescriptors();
		UpdateFrameUniforms(fb.frameIndex);
		UpdateLights(fb.frameIndex);
		UpdateMaterials(fb.frameIndex);
		const uint32_t fpLightCount = UpdateForwardPlusLights(fb.frameIndex);
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
		const auto& fp = m_viewportResources.forwardPlus;
		const uint64_t fpTileCountsAddress = fp.TileCountsAddress(*m_context);
		const uint64_t fpTileIndicesAddress = fp.TileIndicesAddress(*m_context);
		const uint32_t fpTileCountX = fp.tileCountX;
		const uint32_t fpTileCountY = fp.tileCountY;

		// Must match HLSL PushConstants in common.hlsli (field order + size).
		struct alignas(16) PushConstants {
			float model[16];
			uint64_t frameUniforms;
			uint64_t lightsBuffer;
			uint64_t materialsBuffer;
			uint64_t cascadeBuffer;
			uint32_t materialId;
			uint32_t comparisonSamplerId;
			uint32_t numCascades;
			uint32_t cascadeIndex;
			uint64_t fpLights;
			uint64_t fpTileCounts;
			uint64_t fpTileIndices;
			uint32_t fpTileCountX;
			uint32_t fpLightCount;
			uint32_t fpTileSize;
			uint32_t fpMaxPerTile;
		} pc{
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
		};
		static_assert(sizeof(PushConstants) <= 256, "PushConstants must fit in 256 bytes (engine limit)");

		// ---- Cascaded shadow map passes – one per cascade tile (builder owns offsets/extents/load ops) ----
		{
			const char* cascadeNames[4] = { "ShadowCascade0", "ShadowCascade1", "ShadowCascade2", "ShadowCascade3" };
			for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci) {
				PushConstants cascadePC = pc;
				cascadePC.cascadeIndex = ci;
				auto& shadowPass = m_viewportResources.directionalShadows.AddCascadePass(*m_rendergraph, ci, cascadeNames[ci])
					.SetRecordingFunc([this, cascadePC](smvk::CommandBuffer& cmd)
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

		// ---- Forward+ depth prepass – fills depth for tile min/max cull ----
		// No color. Solid geometry uses the VS-only fast path; MASK cutout
		// uses the alpha-tested program so holes keep background depth.
		// (Full-quad prepass depth would poison the read-only main pass:
		// background behind foliage holes would fail depth and render the
		// HDR clear color = black quad halos. Excluding masked instead is
		// NOT an option – order-independent opaque compositing needs their
		// depth in the buffer.)
		{
			PushConstants depthPC = pc;
			auto& prepass = m_viewportResources.forwardPlus.AddDepthPrepass(
				*m_rendergraph, m_viewportResources.depthView,
				{ fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height },
				"ForwardPlusDepth");
			prepass.SetRecordingFunc([this, depthPC](smvk::CommandBuffer& cmd)
			{
				auto drawPrepassNode = [&](NodeId node, bool maskedPhase) {
					auto meshForNode = m_world->scene.meshForNode.find(node);
					if (meshForNode == m_world->scene.meshForNode.end()) return;
					smvk::Material* mat = nullptr;
					if (auto materialForNode = m_world->scene.materialForNode.find(node);
						materialForNode != m_world->scene.materialForNode.end())
						mat = m_world->GetMaterial(materialForNode->second);
					const bool masked = mat &&
						mat->shadingModel == smvk::ShadingModel::Masked;
					if (masked != maskedPhase) return;
					auto& model = m_world->scene.globalTransform[node];
					smvk::StaticMesh* meshDesc = m_world->GetMesh(meshForNode->second);
					if (!meshDesc) return;
					PushConstants objectPC{};
					std::copy_n(glm::value_ptr(model), 16, objectPC.model);
					if (maskedPhase) {
						auto materialForNode = m_world->scene.materialForNode.find(node);
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
				// Phase 1: solid geometry, VS-only fast path (unchanged).
				cmd.BindRenderPipeline(
					m_viewportResources.forwardPlus.depthProgram,
					smvk::VertexFormat::StaticVertexFormat,
					smvk::DepthSettings{
						.depthFunc = smvk::CompareOp::GreaterEqual,
						.writeEnable = true,
						.depthTestEnable = true,
					},
					smvk::StencilSettings{},
					smvk::BlendSettings{},
					smvk::MSAASettings{},
					smvk::RasterSettings{
						.cullMode = smvk::CullMode::Back
					}
				);
				cmd.PushConstants(0, sizeof(PushConstants), &depthPC, smvk::ShaderStage::All);
				m_world->forEachNode(
					smvk::IteratorFlagBit_Renderable
					| smvk::IteratorFlagBit_Opaque
					| smvk::IteratorFlagBit_StaticVertexFmt
					, [&](NodeId node) { drawPrepassNode(node, false); });
				// Phase 2: MASK cutout, alpha-tested so holes keep
				// background depth. Cull off: foliage is double-sided.
				cmd.BindRenderPipeline(
					m_resources.prepassMaskedProgram,
					smvk::VertexFormat::StaticVertexFormat,
					smvk::DepthSettings{
						.depthFunc = smvk::CompareOp::GreaterEqual,
						.writeEnable = true,
						.depthTestEnable = true,
					},
					smvk::StencilSettings{},
					smvk::BlendSettings{},
					smvk::MSAASettings{},
					smvk::RasterSettings{
						.cullMode = smvk::CullMode::None
					}
				);
				cmd.PushConstants(0, sizeof(PushConstants), &depthPC, smvk::ShaderStage::All);
				m_world->forEachNode(
					smvk::IteratorFlagBit_Renderable
					| smvk::IteratorFlagBit_Opaque
					| smvk::IteratorFlagBit_StaticVertexFmt
					, [&](NodeId node) { drawPrepassNode(node, true); });
			});
		}

		// ---- Forward+ tile cull – depth + light volumes -> per-tile lists ----
		{
			smvk::ForwardPlusCullPush cullPC{};
			cullPC.frameUniforms = frameUniformsAddress;
			cullPC.lights = fpLightsAddress;
			cullPC.tileCounts = fpTileCountsAddress;
			cullPC.tileIndices = fpTileIndicesAddress;
			cullPC.depthTextureIndex = m_viewportResources.forwardPlus.depthBindlessIndex;
			cullPC.tileCountX = fpTileCountX;
			cullPC.tileCountY = fpTileCountY;
			cullPC.lightCount = fpLightCount;
			cullPC.tileSize = FP_TILE_SIZE;
			cullPC.maxLightsPerTile = FP_MAX_PER_TILE;
			auto& cullPass = m_viewportResources.forwardPlus.AddCullPass(
				*m_rendergraph, m_viewportResources.depthView,
				m_resources.fpLightsLogical, "ForwardPlusCull");
			cullPass.SetRecordingFunc([this, cullPC](smvk::CommandBuffer& cmd) mutable
			{
				cmd.BindComputePipeline(
					m_viewportResources.forwardPlus.cullProgram);
				cmd.PushConstants(0, sizeof(smvk::ForwardPlusCullPush), &cullPC,
					smvk::ShaderStage::All);
				cmd.Dispatch(
					m_viewportResources.forwardPlus.tileCountX,
					m_viewportResources.forwardPlus.tileCountY,
					1);
			});
		}

		const bool useMsaa = m_viewportResources.msaaEnabled &&
			m_context->SupportsMSAA2x() && m_viewportResources.msaa.Valid();
		auto& mainPass = m_rendergraph->AddPass("Main")
			.SetOffset(0, 0)
			.SetInputImages(
				{
					m_viewportResources.directionalShadows.logicalView
				}
			)
			.SetExtent(
				{
					fb.swapchain->GetExtent().width,
					fb.swapchain->GetExtent().height
				}
			);
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
			.SetDepth(m_viewportResources.depthView,
				smvk::AttachmentLoadOp::Load,
				smvk::AttachmentStoreOp::DontCare,
				smvk::ClearDepthStencil(0, 0)
			);
		}
		mainPass.SetRecordingFunc([this, pc, useMsaa](smvk::CommandBuffer& cmd)
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

		auto bindForMaterial = [&](uint32_t materialId) {
			// Per-material raster (doubleSided -> Cull None) + blend
			// (Opaque vs AlphaBlend). 1x depth is read-only, 2x writes.
			smvk::BlendSettings blend = smvk::BlendSettings::Opaque();
			smvk::RasterSettings raster{};
			if (smvk::Material* mat = m_world->GetMaterial(materialId)) {
				blend = mat->pipeline.blend;
				raster = mat->pipeline.raster;
			}
			cmd.BindRenderPipeline(
				m_resources.mainProgram,
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
				const uint32_t materialId = materialForNode->second;
				if (materialId != boundMaterial) {
					bindForMaterial(materialId);
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
					bindForMaterial(d.materialId);
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
			auto& tonemapPass = m_viewportResources.tonemap.AddTonemapPass(
				*m_rendergraph,
				m_viewportResources.hdr.logicalView,
				m_viewportResources.hdr.bindlessIndex,
				bloomView,
				bloomBindless,
				bloomIntensity,
				linearSampler,
				colors,
				{ fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height },
				"Tonemap");
			(void)tonemapPass;
		}

		// ---------------------------------------------------------------------
		// ImGui – build UI (before adding the rendergraph pass)
		// ---------------------------------------------------------------------
		if (m_imgui && m_imgui->IsInitialized()) {
			m_imgui->NewFrame();

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
				int spots = m_fpDemo.spotCount;
				if (ImGui::SliderInt("Spots", &spots, 0, 16)) m_fpDemo.spotCount = spots;
				ImGui::SliderFloat("Spot intensity (cd)", &m_fpDemo.spotIntensity, 0.0f, 4000.0f);
				const int wantLights = m_fpDemo.enabled
					? (m_fpDemo.gridX * m_fpDemo.gridY * m_fpDemo.gridZ + m_fpDemo.spotCount) : 0;
				ImGui::Text("Lights: %d / %u%s", wantLights, FP_MAX_LIGHTS,
					wantLights > (int)FP_MAX_LIGHTS ? "  (CLAMPED!)" : "");
				if (fwd.depthBindlessIndex == UINT32_MAX)
					ImGui::TextDisabled("Depth bindless: MISSING (cull falls back to empty)");
			}
			ImGui::Separator();
			ImGui::TextDisabled("[I] %s  [ESC] quit", m_uiMode ? "UI (NORMAL) -> press I for FPS" : "FPS (DISABLED) -> press I for UI");
			ImGui::End();

			// Alternative: draw directly with custom draw lists etc.
			// ImGui::GetForegroundDrawList()->AddCircle(...);

			// Register a rendergraph pass that composites ImGui over the swapchain.
			// It uses LoadOp::Load so the scene color is preserved, and is
			// recorded as the *last* pass so UI is on top.
			// Internally this calls ImGui::Render() + ImGui_ImplVulkan_RenderDrawData.
			m_imgui->AddPass(*m_rendergraph, colors[0],
				{ fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height });

			// Manual alternative (equivalent, more explicit):
			// ImGui::Render();
			// m_rendergraph->AddPass("ImGuiManual")
			//     .SetOffset(0,0)
			//     .SetExtent({fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height})
			//     .SetColor(colors, smvk::AttachmentLoadOp::Load, smvk::AttachmentStoreOp::Store)
			//     .SetRecordingFunc([this](smvk::CommandBuffer& cmd){ m_imgui->Render(cmd); });
		}

		m_rendergraph->Execute(fb);
		m_context->ProcessDeferrredTasks();

	}


private:
	std::unique_ptr<smvk::ImGuiOverlay> m_imgui{};
	AppResources m_resources{};
	ViewportResources m_viewportResources{};
	ForwardPlusDemo m_fpDemo{};
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