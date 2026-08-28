#pragma once
#include <iostream>
#include <filesystem>

#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/Renderer/rendergraph.h>

#include <Engine/Utils/gltf.h> // for gltf read into smvk::Cluster
#include <Engine/platform.h> // for readFile

#include <Engine/Camera/camera.h> 
#include <Engine/Renderer/light.h>

// Constants //
static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;
std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
};

//struct ShadowmapCascadeAtlas {};
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

class TestApp : public smvk::VulkanApp {
	using Super = smvk::VulkanApp;
public:
	//TestApp() {};
	//~TestApp() {};
private:
	void UpdateCamera(float dt);
	//

	void EarlyUpdate() override {
		Super::EarlyUpdate();
	}

	void Update() {
		Super::Update();
		UpdateCamera(delta_time_s);
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
			sizeof(FrameUniforms) * m_swapchain->framesInFlight_,
			smvk::BufferType_Uniform,
			"frame uniforms"
		);
		m_resources.lightsBuffer = m_context->CreateBuffer(
			sizeof(smvk::Light) * MAX_LIGHTS * m_swapchain->framesInFlight_,
			smvk::BufferType_Storage,
			"lights buffer"
		);
		m_resources.materialsBuffer = m_context->CreateBuffer(
			sizeof(smvk::MaterialValues) * MAX_MATERIALS * m_swapchain->framesInFlight_,
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
				m_swapchain->imageViews_[i],
				m_swapchain->surfaceFormat,
				"swap"
			);
			m_viewportResources.swapImages.push_back(logicalSwapchainImage);
		}
		auto depthImage = m_context->images_.get(m_swapchain->depthImage_);
		auto depthImageView = m_context->imageViews_.get(m_swapchain->depthImageView_);
		m_viewportResources.depthView = m_rendergraph->AddExternalImageView(
			depthImageView->vkView, // vk leaking here
			m_swapchain->depthFormat,
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
				VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
			);


		// -------------------------------------------------------------------------
		// Render graph
		// -------------------------------------------------------------------------

		auto shadowImage =
			m_rendergraph->AddExternalImage(
				shadow.image,
				"shadowMap1"
			);;

		m_viewportResources.shadowmapLogicalViews[0] =
			m_rendergraph->AddImageView(
				shadowImage,
				smvk::ImageViewType::View2D,
				smvk::Format::Undefined,
				{
					.aspect = smvk::ImageAspect::Depth,
					.baseMipLevel = 0,
					.mipLevelCount = 1,
					.baseArrayLayer = 0,
					.arrayLayerCount = 1,
				},
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

		return true;
	};

	FrameUniforms UpdateFrameUniforms(uint32_t currentFrame) {

		int width_ = m_swapchain->GetExtent().width;
		int height_ = m_swapchain->GetExtent().height;

		auto view = m_camera.getViewMatrix();
		auto proj = m_camera.getProjMatrix();
		proj[1][1] *= -1; // viewport fix ditecrly in proj

		auto viewProj = proj * view;

		// Host mapped version. CreateBuffer doesnt support properFlags
		//void* mapped = m_context->MapBuffer(m_resources.frameUniforms);
		//////this is wrong should check properly
		////auto limits = core_->GetPhysicalDeviceProperties().properties.limits;
		////if (!limits.minStorageBufferOffsetAlignment == 16) {assert(false) ;}
		////if (!limits.minUniformBufferOffsetAlignment == 16) {assert(false) ;}
		//auto* curr = reinterpret_cast<FrameUniforms*>(
		//	static_cast<std::byte*>(mapped) +
		//	currentFrame * sizeof(FrameUniforms)
		//	);
		//curr->cameraPos = m_cameraPositioner.cameraPosition_;

		//curr->view = view;
		//curr->proj = proj;
		//curr->viewProj = viewProj;

		//curr->invView = glm::inverse(view);
		//curr->invProj = glm::inverse(proj);
		//curr->invViewProj = glm::inverse(viewProj);

		//curr->time = 0.0f;
		//curr->viewportSize = { width_, height_ };
		//curr->invViewportSize = { 1.0f / width_, 1.0f / height_ };

		//// Not in use now
		//curr->nearPlane = 0.f;
		//curr->farPlane = 0.f;
		//curr->exposure = 0.f;

		//FrameUniforms copy = *curr;
		//m_context->UnmapBuffer(m_resources.frameUniforms);
		//return copy;

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
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			0,
			currentFrame * sizeof(FrameUniforms)
			//currentFrame * m_resources.frameUniformsBuffer.key.elementSize * m_resources.frameUniformsBuffer.key.elementCount // dynamicOffsetPerFrame
		);
		return data;

	}

	void UpdateLights(uint32_t currentFrame)
	{

		glm::vec3 lightDir = glm::normalize(glm::vec3(0.5f, -1.0f, 0.5f));
		auto cameraView = m_camera.getViewMatrix();
		auto cameraProj = m_camera.getProjMatrix();

		glm::mat4 lightViewOut;
		glm::mat4 lightProjOut;
		constexpr float lightNear = 0.1f;
		constexpr float lightFar = 10.0f;

		smvk::Light lights[MAX_LIGHTS]{};
		smvk::Light& light = lights[0];
		light.color = glm::vec4(1.0f, 0.8f, 0.8f, 1.0f);
		light.direction[0] = lightDir.x;
		light.direction[1] = lightDir.y;
		light.direction[2] = lightDir.z;
		light.intensity = 1.0f;
		light.shadow_map_index =
			m_context->GetBindlessIndex(
				m_viewportResources.shadowmaps[0].bindless
			);
		// Replace with your actual flag enum.
		//light.flags = smvk::LightFlagBit_Directional;

		smvk::GetDirectionalLightOrthoVP(
			lightDir,
			cameraView,
			cameraProj,
			//glm::inverse(cameraView),
			//glm::inverse(cameraProj),
			lightNear,
			lightFar,
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
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT,
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
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			0,
			frameOffset
		);
	}

	void DrawFrame() {
		auto fb = m_swapchain->AcquireFramebuffer();
		UpdateDeltaTime();
		RenderFrame(fb);
		m_swapchain->EndAndPresentFramebuffer(fb);
	}

	void RenderFrame(smvk::FrameBuffer fb) {

		m_rendergraph->NewFrame();

		//
		m_context->UpdateBindings();
		auto frameData =
			UpdateFrameUniforms(fb.frameIndex);
		UpdateLights(fb.frameIndex);
		UpdateMaterials(fb.frameIndex);
		//

		std::vector<smvk::LogicalImageViewHandle> colors;
		colors.push_back(m_viewportResources.swapImages[fb.imageIndex]);

		uint64_t frameUniformsAddress =
			m_context->buffers_
			.get(m_resources.frameUniforms)
			->deviceAddress;
		uint64_t lightsBufferAddress =
			m_context->buffers_
			.get(m_resources.lightsBuffer)
			->deviceAddress;
		uint64_t materialsAddress =
			m_context->buffers_
			.get(m_resources.materialsBuffer)
			->deviceAddress;

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
					.depthFunc = smvk::CompareOp::LessEqual,
					.writeEnable = true,
					.depthTestEnable = true,
					.depthBiasEnable = true,
					.depthBiasConstantFactor = 1.f,
					.depthBiasClamp = 0.0f,
					.depthBiasSlopeFactor = 1.f
				}

			);
			cmd.PushConstants(
				0,
				sizeof(PushConstants),
				&pc,
				smvk::ShaderStage::AllGraphics
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

				PushConstants pc{};
				const float* modelData = glm::value_ptr(model);
				std::copy_n(modelData, 16, pc.model);

				cmd.PushConstants(
					0,
					offsetof(PushConstants, frameUniforms),
					&pc,
					smvk::ShaderStage::AllGraphics
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
					// we sample trough the bidnless list but we decalre intention here for sync.
					// may be bad
				}
			)
			.SetExtent(
				{
					fb.swapchain.extent_.width,
					fb.swapchain.extent_.height
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
				offsetof(PushConstants, frameUniforms),
				sizeof(uint64_t),
				&pc.frameUniforms
			);
			cmd.PushConstants(
				offsetof(PushConstants, materialsBuffer),
				sizeof(uint64_t),
				&pc.materialsBuffer
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
					smvk::ShaderStage::AllGraphics
				);
				cmd.PushConstants(
					0,
					offsetof(PushConstants, frameUniforms),
					&pc,
					smvk::ShaderStage::AllGraphics
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

		// UI PASS
		//m_rendergraph->AddPass()
		//	.setOffset({ 0,0 })
		//	.setExtent(fb.swapchain.extent_)
		//	.setColor(colors, VK_ATTACHMENT_LOAD_OP_NONE, VK_ATTACHMENT_STORE_OP_STORE, { {0.0f, 0.0f, 0.0f, 1.0f} })  // Rendering directly to swapchainImage!
		//	//.setDepth(logicalDepthImageView, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, { .0f, 0 })
		//	.setRecordingFunc([this, fb](smvk::CommandBuffer& cmd) {

		//	//ImGui_ImplVulkan_NewFrame();
		//	//ImGui_ImplGlfw_NewFrame();
		//	//ImGui::NewFrame();

		//	//// UI Here
		//	//ImGuiIO& io = ImGui::GetIO();
		//	//ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
		//	//	1000.0f / io.Framerate, io.Framerate);

		//	//ImDrawList* drawList = ImGui::GetForegroundDrawList();
		//	//ImGui::Render();
		//	//ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
		//	//
		//});

		m_rendergraph->Execute(fb);
		m_context->ProcessDeferrredTasks();

	}


private:
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

	// NDC, but to frameBuffer dimensions. 
	//glm::vec2 mousePos = glm::vec2(
	//	(static_cast<float>(m_input.getMouseX()) / static_cast<float>(swapchain->GetExtent().width)) * 2.0f - 1.0f,
	//	1.0f - (static_cast<float>(m_input.getMouseY()) / static_cast<float>(swapchain->GetExtent().height)) * 2.0f
	//);

	// NDC, to window dimensions. These two may not match, depends on DPI settings. Be careful.
	// m_input should be normalized and not be affected by resolution!
	glm::vec2 mousePos = glm::vec2(
		(static_cast<float>(m_input.getMouseX()) / static_cast<float>(m_swapchain->windowWidth)) * 2.0f - 1.0f,
		1.0f - (static_cast<float>(m_input.getMouseY()) / static_cast<float>(m_swapchain->windowHeight)) * 2.0f
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
	std::cerr << "\nPress Enter to exit...";
	std::cin.get();
	return EXIT_SUCCESS;
}