#pragma once
#include <iostream>
#include <filesystem>

#include <Engine/core.h>
#include <imgui.h>


// Constants //
static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;
std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
};

struct ShadowMap
{
	smvk::Holder<smvk::ImageHandle> image;
	smvk::Holder<smvk::ImageViewHandle> attachmentView;
	smvk::Holder<smvk::ImageViewHandle> sampledView;
	smvk::Holder<smvk::BindlessImageHandle> bindless;
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
};
static_assert(sizeof(FrameUniforms) % 16 == 0);

// Resources
struct AppResources {
	smvk::Holder<smvk::BufferHandle> frameUniforms;
	smvk::Holder<smvk::BufferHandle> lightsBuffer;
	smvk::Holder<smvk::BufferHandle> materialsBuffer;
	smvk::Holder<smvk::ShaderModuleHandle> vertShader;
	smvk::Holder<smvk::ShaderModuleHandle> fragShader;
	smvk::Holder<smvk::ShaderModuleHandle> shadowVert;
	smvk::ShaderProgram mainProgram;
	smvk::ShaderProgram shadowProgram;
};

struct ViewportResources {
	ShadowMap shadowmaps[8];

	smvk::LogicalImageViewHandle shadowmapLogicalViews[8];
	std::vector<smvk::LogicalImageViewHandle> swapImages;
	smvk::LogicalImageViewHandle depthView;
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

		// if mipped maybe a helper like LegitEngine does is a good idea

		// shadowmap
		auto& shadow = m_viewportResources.shadowmaps[0];

		smvk::Format shadowMapFormat = smvk::Format::D32_Float_S8_UInt;
		smvk::ImageCreateInfo shadowMapCI{};
		shadowMapCI.format = shadowMapFormat;
		shadowMapCI.extent = { 2048u, 2048u, 1u };
		shadowMapCI.mipLevels = 1u;
		shadowMapCI.arrayLayers = 1u;
		shadowMapCI.usage = smvk::ImageUsage::DepthStencilAttachment | smvk::ImageUsage::Sampled;
		shadowMapCI.name = "shadowMap";

		// -------------------------------------------------------------------------
		// Image
		// -------------------------------------------------------------------------

		shadow.image =
			m_context->CreateImage(
				shadowMapCI,
				nullptr
			);


		// -------------------------------------------------------------------------
		// Depth attachment view
		// -------------------------------------------------------------------------

		smvk::ImageViewCreateInfo attachmentViewCI{};

		attachmentViewCI.viewType =
			smvk::ImageViewType::View2D;

		attachmentViewCI.format =
			smvk::Format::Undefined;

		attachmentViewCI.subresourceRange = {
			.aspect =
				smvk::ImageAspect::Depth |
				smvk::ImageAspect::Stencil,

			.baseMipLevel = 0,
			.mipLevelCount = 1,

			.baseArrayLayer = 0,
			.arrayLayerCount = 1
		};

		attachmentViewCI.name =
			"shadow attachment";

		shadow.attachmentView =
			m_context->CreateImageView(
				shadow.image,
				attachmentViewCI
			);


		// -------------------------------------------------------------------------
		// Sampled view
		//
		// Important: depth-only, even though the actual format is D32_S8.
		// -------------------------------------------------------------------------

		smvk::ImageViewCreateInfo sampledViewCI{};

		sampledViewCI.viewType =
			smvk::ImageViewType::View2D;

		sampledViewCI.format =
			smvk::Format::Undefined;

		sampledViewCI.subresourceRange = {
			.aspect =
				smvk::ImageAspect::Depth,

			.baseMipLevel = 0,
			.mipLevelCount = 1,

			.baseArrayLayer = 0,
			.arrayLayerCount = 1
		};

		sampledViewCI.name =
			"shadow sampled";

		shadow.sampledView =
			m_context->CreateImageView(
				shadow.image,
				sampledViewCI
			);


		// -------------------------------------------------------------------------
		// Bindless
		// -------------------------------------------------------------------------

		shadow.bindless =
			m_context->CreateBindlessImage(
				shadow.sampledView,
				smvk::ImageLayout::DepthReadOnlyOptimal
			);


		// -------------------------------------------------------------------------
		// Render graph
		// -------------------------------------------------------------------------

		m_viewportResources.shadowmapLogicalViews[0] =
			m_rendergraph->AddExternalImageView(
				shadow.sampledView,
				shadowMapFormat,
				"shadowMapView1"
			);

		// singla sync req would be better
		// Manual Barrier transition
		// this whole shadowmap thing is so badly done, api surface NEEDS more polish

		//auto han = m_context->m_submitter->acquire();
		//auto cmd = m_context->m_submitter->GetCommandBuffer(han);
	/*	ImageBarrier(
			cmd.cmd,
			smimage->vk_image,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
			{
				.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
				.baseMipLevel = 0,
				.levelCount = VK_REMAINING_MIP_LEVELS,
				.baseArrayLayer = 0,
				.layerCount = VK_REMAINING_ARRAY_LAYERS
			},
			VK_PIPELINE_STAGE_2_NONE,
			VK_ACCESS_2_NONE,
			VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
			VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
		);*/
		//m_context->m_submitter->submit(han);
		//m_context->m_submitter->wait(han);
		//

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

		data.time = 0.0f;
		data.viewportSize = { width_, height_ };
		data.invViewportSize = { 1.0f / width_, 1.0f / height_ };

		// Not in use now
		data.nearPlane = 0.f;
		data.farPlane = 0.f;
		data.exposure = 0.f;

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

		glm::vec3 lightDir = glm::normalize(glm::vec3(0.2f, -1.0f, 0.5f));
		lightDir.b *= sin(time_since_start_s) * 0.4;
		auto cameraView = m_camera.getViewMatrix();
		auto cameraProj = m_camera.getProjMatrix();
		//cameraProj[1][1] *= -1;

		glm::mat4 lightViewOut;
		glm::mat4 lightProjOut;
		constexpr float cameraNear = 0.001f;
		constexpr float cameraFar = 50.0f;

		smvk::Light lights[MAX_LIGHTS]{};
		smvk::Light& light = lights[0];
		light.color = glm::vec4(1.0f, 1.f, 1.0f, 1.0f);
		light.direction[0] = lightDir.x;
		light.direction[1] = lightDir.y;
		light.direction[2] = lightDir.z;
		light.intensity = 1.0f;
		light.shadow_map_index =
			m_context->GetBindlessIndex(
				m_viewportResources.shadowmaps[0].bindless
			);

		smvk::GetDirectionalLightOrthoVP(
			lightDir,
			//cameraView,
			//cameraProj,
			glm::inverse(cameraView),
			glm::inverse(cameraProj),
			cameraNear,
			cameraFar,
			lightViewOut,
			lightProjOut,  // outProj (function will overwrite, but we recompute below)
			1.f,       // zPadding
			true,       // enableTexelSnap
			2048        // shadowMapRes
		);

		lightProjOut[1][1] *= -1;
		auto lightViewProj = lightProjOut * lightViewOut;
		std::memcpy(
			light.lightVP,
			glm::value_ptr(lightViewProj),
			sizeof(light.lightVP)
		);

		constexpr size_t LIGHT_FRAME_STRIDE = sizeof(smvk::Light) * MAX_LIGHTS;
		size_t frameOffset = currentFrame * LIGHT_FRAME_STRIDE;

		m_context->UploadDataToBuffer(
			m_resources.lightsBuffer,
			lights,
			sizeof(lights),
			smvk::PipelineStageBit_Vertex,
			smvk::AccessFlags::ShaderRead,
			0,
			//currentFrame * sizeof(lights)
			frameOffset
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

		//
		m_context->UpdateDescriptors();
		UpdateFrameUniforms(fb.frameIndex);
		UpdateLights(fb.frameIndex);
		UpdateMaterials(fb.frameIndex);
		//

		std::vector<smvk::LogicalImageViewHandle> colors;
		colors.push_back(m_viewportResources.swapImages[fb.imageIndex]);

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

		struct alignas(16) PushConstants {
			float model[16];
			uint64_t frameUniforms;
			uint64_t lightsBuffer;
			uint64_t materialsBuffer;
			uint32_t materialId;
			uint32_t comparisonSamplerId;
		} pc{
			.frameUniforms = frameUniformsAddress,
			.lightsBuffer = lightsBufferAddress,
			.materialsBuffer = materialsAddress,
			.materialId = 0, // defaultMaterial
			.comparisonSamplerId = 1
		};

		auto& shadowPass = m_rendergraph->AddPass("Shadow")
			.SetOffset(0, 0)
			.SetExtent({ 2048, 2048 })
			.SetDepth(
				m_viewportResources.shadowmapLogicalViews[0],
				smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store,
				smvk::ClearDepthStencil(1.0f, 0)
			)
			.SetRecordingFunc([this, pc](smvk::CommandBuffer& cmd)
		{

			cmd.BindRenderPipeline(
				m_resources.shadowProgram,
				smvk::VertexFormat::StaticVertexFormat,
				smvk::DepthSettings{
					.depthFunc = smvk::CompareOp::Less,
					.writeEnable = true,
					.depthTestEnable = true,
					.depthBiasEnable = true,
					.depthBiasConstantFactor = 0.f,
					.depthBiasClamp = 0.0f,
					.depthBiasSlopeFactor = 3.f
				}

			);

			// PC were hardcoded in pipelineCreation to ALL 128 wide
			cmd.PushConstants(
				0,
				sizeof(PushConstants),
				&pc,
				smvk::ShaderStage::All 
			);

			// Render meshes with cull mode BACK (render front faces from light's perspective)
			// and depth bias to reduce shadow acne
			m_world->forEachNode(
				smvk::IteratorFlagBit_StaticVertexFmt
				| smvk::IteratorFlagBit_CastShadow
				, [&](NodeId node)
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
				smvk::StaticMesh* meshDesc = m_world->GetMesh(meshId);

				PushConstants objectPC{};
				std::copy_n(glm::value_ptr(model), 16, objectPC.model);

				cmd.PushConstants(
					offsetof(PushConstants, model),
					sizeof(objectPC.model),
					objectPC.model,
					//smvk::ShaderStage::Vertex
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
		});

		auto& mainPass = m_rendergraph->AddPass()
			.SetOffset(0, 0)
			.SetInputImages(
				{
					m_viewportResources.shadowmapLogicalViews[0]
					// we sample trough the bidnless list but we declare intention here for sync.
					// may be bad
				}
			)
			.SetExtent(
				{
					fb.swapchain->GetExtent().width,
					fb.swapchain->GetExtent().height
				}
			)
			.SetColor(
				colors, // this is swapImage directly!
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