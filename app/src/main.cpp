#pragma once
#include <iostream>
#include <filesystem>
#include <array>
#include <cmath>

#include <Engine/core.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>
#include <imgui.h>


// Constants //
static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;
std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
};

// -----------------------------------------------------------------------------
// Directional cascade shadow-map config (resources + math live in
// Engine/Rendergraph/cascadedshadowmap.h)
// -----------------------------------------------------------------------------
static constexpr uint32_t NUM_CASCADES = 4;
static constexpr uint32_t CASCADE_RES = 2048; // per cascade tile (2048 -> 4096 atlas for 4 cascades, Option B)
static constexpr float CASCADE_LAMBDA = 0.92f; // 0 = linear, 1 = logarithmic
static constexpr float CASCADE_Z_PADDING = 1.0f;
static constexpr smvk::Format SHADOW_FORMAT = smvk::Format::D32_Float; // stencil dropped for directional atlas (Option B)

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
};
static_assert(sizeof(FrameUniforms) % 16 == 0);

// Resources
struct AppResources {
	smvk::Holder<smvk::BufferHandle> frameUniforms;
	smvk::Holder<smvk::BufferHandle> lightsBuffer;
	smvk::Holder<smvk::BufferHandle> materialsBuffer;
	smvk::Holder<smvk::BufferHandle> directionalCascadeBuffer; // GpuDirectionalLightDescription per frame
	smvk::Holder<smvk::ShaderModuleHandle> vertShader;
	smvk::Holder<smvk::ShaderModuleHandle> fragShader;
	smvk::Holder<smvk::ShaderModuleHandle> shadowVert;
	smvk::Holder<smvk::ShaderModuleHandle> tonemapVert;
	smvk::Holder<smvk::ShaderModuleHandle> tonemapFrag;
	smvk::ShaderProgram mainProgram;
	smvk::ShaderProgram shadowProgram;
	smvk::ShaderProgram tonemapProgram;
};

struct HdrResources {
	smvk::Holder<smvk::ImageHandle> image;
	smvk::Holder<smvk::ImageViewHandle> view;
	smvk::Holder<smvk::BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	smvk::LogicalImageViewHandle logicalView;
	smvk::Extent2D extent{0,0};
};

struct ViewportResources {
	smvk::CascadedShadowAtlas directionalShadows; // cascade atlas builder (image + views + bindless + logical)
	std::vector<smvk::LogicalImageViewHandle> swapImages;
	smvk::LogicalImageViewHandle depthView;
	HdrResources hdr;
};


class TestApp : public smvk::AppImpl {
	using Super = smvk::AppImpl;
private:
	void UpdateCamera(float dt);
	void SetUIMode(bool ui);
	void EnsureHdrResources();
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

		// Main ShaderProgram
		auto vertCode = readFile("content/shaders/default.vert.hlsl.spv");
		auto fragCode = readFile("content/shaders/default.frag.hlsl.spv");
		m_resources.vertShader = m_context->CreateShaderModule(vertCode, "vert");
		m_resources.fragShader = m_context->CreateShaderModule(fragCode, "frag");

		auto shadowVertCode = readFile("content/shaders/shadow.vert.hlsl.spv");
		m_resources.shadowVert = m_context->CreateShaderModule(shadowVertCode, "shadowVert");

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

		//
		auto shadowVert = smvk::Shader{
			.module = m_resources.shadowVert,
			.entryPoint = "mainVS",
			.stage = smvk::ShaderStage::Vertex
		};
		m_resources.shadowProgram = {
			.shaders = {shadowVert},
			.shaderCount = 1
		};

		// Tonemapper (fullscreen triangle HDR -> LDR)
		auto tonemapVertCode = readFile("content/shaders/tonemap.vert.hlsl.spv");
		auto tonemapFragCode = readFile("content/shaders/tonemap.frag.hlsl.spv");
		m_resources.tonemapVert = m_context->CreateShaderModule(tonemapVertCode, "tonemapVert");
		m_resources.tonemapFrag = m_context->CreateShaderModule(tonemapFragCode, "tonemapFrag");
		auto tonemapVS = smvk::Shader{ .module = m_resources.tonemapVert, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
		auto tonemapPS = smvk::Shader{ .module = m_resources.tonemapFrag, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
		m_resources.tonemapProgram = { .shaders = { tonemapVS, tonemapPS }, .shaderCount = 2 };

		smvk::AllocateStaticVertexFmt(*m_context); // dont like this

		// Import the glTF scene into a Cluster (owned by the cluster's resource
		// pools), register it with the World, and resolve all imported scenes
		// into the single runtime scene graph.
		tinygltf::Model model;
		if (!LoadModelFromAsciiFile(model, gltf_meshes_paths[0].c_str())) {
			return false;
		}

		auto& cluster = m_world->LoadCluster(*m_context);
		GltfToCluster(*m_context, model, gltf_meshes_paths[0], cluster);
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

		// Directional cascade atlas via engine builder (ports previous inline code).
		smvk::CascadedShadowMapDesc shadowDesc{};
		shadowDesc.cascadeCount = NUM_CASCADES;
		shadowDesc.cascadeResolution = CASCADE_RES;
		shadowDesc.format = SHADOW_FORMAT;
		shadowDesc.lambda = CASCADE_LAMBDA;
		shadowDesc.zPadding = CASCADE_Z_PADDING;
		shadowDesc.name = "shadowMapAtlas";
		if (!m_viewportResources.directionalShadows.Create(*m_context, *m_rendergraph, shadowDesc)) {
			std::cerr << "[TestApp] Failed to create cascaded shadow atlas\n";
			return false;
		}

		EnsureHdrResources();

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
		// Linear exposure multiplier for tonemapper (ACES). 1.0 = no extra exposure;
		// raise for brighter HDR (e.g. sun 50k lux → exposure 0.02 to fit LDR).
		// Kept at 1.0 for now – tonemap exposure push is used instead.
		data.exposure = 1.0f;

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
		// Keep lights[0] hardcoded directional – upgraded to cascades
		glm::vec3 lightDir = glm::normalize(glm::vec3(0.5f, -1.0f, 0.5f));
		//lightDir.b *= sin(time_since_start_s) * 0.4;
		auto cameraView = m_camera.getViewMatrix();
		auto cameraProj = m_camera.getProjMatrix();
		// Keep Y-flip consistent with main view (UpdateFrameUniforms does proj[1][1] *= -1 for Vulkan).
		// Cascade frustum extraction must use the same Y-flipped proj, otherwise shadow tiles are Y-mirrored.
		cameraProj[1][1] *= -1.0f;

		smvk::Light lights[MAX_LIGHTS]{};
		smvk::Light& light = lights[0];
		light.color = glm::vec4(1.0f, 1.f, 1.0f, 1.0f);
		light.direction[0] = lightDir.x;
		light.direction[1] = lightDir.y;
		light.direction[2] = lightDir.z;
		light.intensity = 4.0f;
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
		EnsureHdrResources();
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
		EnsureHdrResources();
		m_context->UpdateDescriptors();
		UpdateFrameUniforms(fb.frameIndex);
		UpdateLights(fb.frameIndex);
		UpdateMaterials(fb.frameIndex);
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
		} pc{
			.frameUniforms = frameUniformsAddress,
			.lightsBuffer = lightsBufferAddress,
			.materialsBuffer = materialsAddress,
			.cascadeBuffer = cascadeBufferAddress,
			.materialId = 0, // defaultMaterial
			.comparisonSamplerId = 1,
			.numCascades = NUM_CASCADES,
			.cascadeIndex = 0
		};
		static_assert(sizeof(PushConstants) <= 128, "PushConstants must fit in 128 bytes");

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
						m_resources.shadowProgram,
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
					m_world->forEachNode(
						smvk::IteratorFlagBit_StaticVertexFmt | smvk::IteratorFlagBit_CastShadow
						, [&](NodeId node)
					{
						auto meshForNode = m_world->scene.meshForNode.find(node);
						if (meshForNode == m_world->scene.meshForNode.end()) return;
						auto materialForNode = m_world->scene.materialForNode.find(node);
						if (materialForNode == m_world->scene.materialForNode.end()) return;
						auto& model = m_world->scene.globalTransform[node];
						uint32_t meshId = meshForNode->second;
						smvk::StaticMesh* meshDesc = m_world->GetMesh(meshId);
						PushConstants objectPC{};
						std::copy_n(glm::value_ptr(model), 16, objectPC.model);
						cmd.PushConstants(offsetof(PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
						for (auto& draw_set : meshDesc->geometry.surfaces) {
							cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
						};
					});
				});
				(void)shadowPass;
			}
		}

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
			)
			.SetColor(
				hdrColors, // HDR intermediate – tonemapper will resolve to swap
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0, 0, 0, 1)
			)
			.SetDepth(m_viewportResources.depthView,
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::DontCare,
				smvk::ClearDepthStencil(0, 0)
			)
			.SetRecordingFunc([this, pc](smvk::CommandBuffer& cmd)
		{

			cmd.BindRenderPipeline(
				m_resources.mainProgram,
				smvk::VertexFormat::StaticVertexFormat,
				smvk::DepthSettings
				{
					.depthFunc = smvk::CompareOp::GreaterEqual
				}
			);

			cmd.PushConstants(
				0,
				sizeof(PushConstants),
				&pc,
				smvk::ShaderStage::All
			);

			m_world->forEachNode(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				//| smvk::IteratorFlagBit_PassedCullTest
				| smvk::IteratorFlagBit_StaticVertexFmt
				,
				[&](NodeId node)
			{
				auto meshForNode = m_world->scene.meshForNode.find(node);
				if (meshForNode == m_world->scene.meshForNode.end()) {
					return;
				}

				auto materialForNode = m_world->scene.materialForNode.find(node);
				if (materialForNode == m_world->scene.materialForNode.end()) {
					return;
				}

				auto& model = m_world->scene.globalTransform[node];

				uint32_t meshId = meshForNode->second;
				uint32_t materialId = materialForNode->second;
				smvk::StaticMesh* meshDesc = m_world->GetMesh(meshId);

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
				};
			}
			);

		}
			);

		// ---------------------------------------------------------------------
		// Tonemapper – HDR (RGBA16F) → LDR swapchain (ACES / Reinhard)
		// ---------------------------------------------------------------------
		{
			// Sampler cache: linear clamp for fullscreen blit, shadow sampler is at 1
			uint32_t linearSampler = m_context->GetOrCreateSampler(smvk::SamplerCreateInfo{
				.magFilter = smvk::Filter::Linear,
				.minFilter = smvk::Filter::Linear,
				.mipmapMode = smvk::MipmapMode::Linear,
				.addressU = smvk::AddressMode::ClampToEdge,
				.addressV = smvk::AddressMode::ClampToEdge,
				.addressW = smvk::AddressMode::ClampToEdge,
				.maxAnisotropy = 1.0f
			});
			struct TonemapPush { uint32_t hdrTextureIndex; uint32_t samplerIndex; float exposure; uint32_t tonemapMode; };
			TonemapPush tp{
				.hdrTextureIndex = m_viewportResources.hdr.bindlessIndex,
				.samplerIndex = linearSampler,
				.exposure = 1.0f, // FrameUniforms.exposure could be used (pow2)
				.tonemapMode = 1  // 0=Reinhard, 1=ACES, 2=Uncharted2
			};
			auto& tonemapPass = m_rendergraph->AddPass("Tonemap")
				.SetOffset(0, 0)
				.SetExtent({ fb.swapchain->GetExtent().width, fb.swapchain->GetExtent().height })
				.SetInputImages({ m_viewportResources.hdr.logicalView })
				.SetColor(colors, smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store, smvk::ClearColor(0,0,0,1))
				.SetRecordingFunc([this, tp](smvk::CommandBuffer& cmd){
					cmd.BindRenderPipeline(
						m_resources.tonemapProgram,
						smvk::VertexFormat::Empty,
						smvk::DepthSettings::Disabled(),
						smvk::StencilSettings::DefaultDisabled(),
						smvk::BlendSettings::Opaque(),
						smvk::MSAASettings::Count1(),
						smvk::RasterSettings{ .cullMode = smvk::CullMode::None }
					);
					cmd.PushConstants(0, sizeof(TonemapPush), &tp, smvk::ShaderStage::All);
					cmd.Draw(3, 1, 0, 0); // fullscreen triangle
				});
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
				ImGui::Text(" lightDir: %.2f %.2f %.2f", 
					glm::normalize(glm::vec3(0.2f, -1.0f, 0.5f)).x,
					glm::normalize(glm::vec3(0.2f, -1.0f, 0.5f)).y,
					glm::normalize(glm::vec3(0.2f, -1.0f, 0.5f)).z);
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
	CameraPositioner_FirstPerson m_cameraPositioner = CameraPositioner_FirstPerson(
		glm::vec3(0.0f, 1.0f, 0.0f), //pos
		glm::vec3(0.0f, 1.0f, 1.0f), //target
		glm::vec3(0.0f, 1.0f, 0.0f) //up
	);
	Camera m_camera = Camera(m_cameraPositioner);
};

void TestApp::EnsureHdrResources() {
	auto vkExt = m_swapchain->GetExtent();
	smvk::Extent2D extent{ vkExt.width, vkExt.height };
	auto& hdr = m_viewportResources.hdr;
	if (!hdr.image.empty() && hdr.extent.width == extent.width && hdr.extent.height == extent.height)
		return;
	if (!hdr.logicalView.empty()) {
		m_rendergraph->FreeImageView(hdr.logicalView);
		hdr.logicalView = {};
	}
	hdr.bindless = {};
	hdr.view = {};
	hdr.image = {};

	smvk::ImageCreateInfo ci{};
	ci.extent = { extent.width, extent.height, 1 };
	ci.format = smvk::Format::RGBA16_Float;
	ci.usage = smvk::ImageUsage::ColorAttachment | smvk::ImageUsage::Sampled;
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.name = "hdr";

	hdr.image = m_context->CreateImage(ci, nullptr);
	smvk::ImageViewCreateInfo vci{};
	vci.viewType = smvk::ImageViewType::View2D;
	vci.format = smvk::Format::Undefined;
	vci.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 };
	vci.name = "hdrView";
	hdr.view = m_context->CreateImageView(hdr.image, vci);
	hdr.bindless = m_context->CreateBindlessImage(hdr.view, smvk::ImageLayout::ShaderReadOnlyOptimal);
	hdr.bindlessIndex = m_context->GetBindlessIndex(hdr.bindless);
	hdr.extent = extent;
	hdr.logicalView = m_rendergraph->AddExternalImageView(hdr.view, smvk::Format::RGBA16_Float, "hdrLogical");
	m_context->GetOrCreateSampler(smvk::SamplerCreateInfo{});
}

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