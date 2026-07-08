#pragma once
#include <Engine/App/app.h>
#include <Engine/Renderer/resource_manager.h>

std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
};

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


struct MainViewPushConstants {
	glm::mat4 model;
	int material_index;
	uint32_t     _pad0[3];
};
static_assert(sizeof(MainViewPushConstants) % 16 == 0);

struct Level : Scene {

	MeshManager* m_meshManagerPimpl = nullptr;
	BvhTree tree = {};
	BvhTree static_tree = {};
	bool tree_done = false;
	//std::map<uint32_t, MeshHandle> m_meshForNode;

	Level(MeshManager* meshManager) : m_meshManagerPimpl(meshManager) {};
	~Level() {}

	void Upload(
		std::filesystem::path gltf_model_path
	) {
		tinygltf::Model model;
		LoadModelFromAsciiFile(model, gltf_model_path.string().c_str());

		GltfToSceneImporter importer(*m_meshManagerPimpl, *this, model, std::move(gltf_model_path));
		importer.importNodeRecursive(/*rootNodeIdx*/0, /*parentSceneNode=*/-1, /*level=*/0);

		// This is needed or else scene will not carry transforms effectively
		markAsChanged(*this, 0);
		recalculateGlobalTransforms(*this);
	}

};



struct AppResources {
	LogicalBuffer frameUniformsBuffer{};
	LogicalBuffer drawDataBuffer{};
	//ShaderProgram defaultMainView{};
};

class TestApp : public SamToy::VulkanApp {
	using Super = SamToy::VulkanApp;
public:
	~TestApp() {};
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

	bool Initialize() {
		if (!Super::Initialize())
			return false;

		m_meshManagerPimpl = std::make_unique<MeshManager>(MeshManager(*context));
		m_camera.setPerspective(45.0f, float(swapchain->extent_.width) / float(swapchain->extent_.height), default_near_clip, default_far_clip);
		//
		std::filesystem::path p(gltf_meshes_paths[0]);
		//
		m_currentLevel.m_meshManagerPimpl = m_meshManagerPimpl.get();
		m_currentLevel.Upload(p);
		m_currentLevel.static_tree = treeFromScene(m_currentLevel,
			[&](uint32_t index) -> const Mesh& {return this->m_meshManagerPimpl->meshes.objects_[index].obj_; }
		);
		m_currentLevel.tree_done = true;
		// 
		auto rg = rendergraph.get();
		m_resources.frameUniformsBuffer = rg->AddBuffer<FrameUniforms>(1); //frames in flight ring access? how to semantically say that this should be inifr
		m_resources.drawDataBuffer = rg->AddBuffer<DrawSetData>(MAX_DRAWS); // this is storage
		//
		return true;
	};

	void UpdateBindlessTexturesSets() {

		//const uint32_t materialCount = static_cast<uint32_t>(m_meshManagerPimpl.materials.numObjects());

		//std::vector<VkDescriptorImageInfo> imageInfos;
		//std::vector<VkDescriptorImageInfo> samplerInfos;

		//int count = (textures_.numObjects());
		//imageInfos.reserve(count);
		//samplerInfos.reserve(count);
		////textures_.forEach([&](Image obj_)
		//for (auto textureHandle : textures_)
		//{
		//	auto obj_ = context->images_.get(textureHandle);
		//	VkDescriptorImageInfo ii{};
		//	ii.sampler = VK_NULL_HANDLE; // separate samplers -> leave null
		//	ii.imageView = obj_->sampleView;
		//	ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		//	imageInfos.push_back(ii);

		//	// Sampler for the bindless texure approach needs refacotr.
		//	// Doesnt Really make sense.
		//	VkDescriptorImageInfo si{};
		//	si.sampler = obj_->sampler;
		//	si.imageView = VK_NULL_HANDLE;            // not used for SAMPLER
		//	si.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED; // ignored for SAMPLER
		//	samplerInfos.push_back(si);
		//}
		//	);

		//	for (size_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT; frameIndex++) {
		//		//// --- Two writes: images at index 2, samplers at index 3 ---
		//		VkWriteDescriptorSet writes[2]{};

		//		auto setHandle = perFrameData[frameIndex].frameDescriptorSet;
		//		auto set = context->descriptorSets_.get(setHandle);

		//		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		//		writes[0].dstSet = set->set;
		//		writes[0].dstBinding = 2;
		//		writes[0].dstArrayElement = 0;
		//		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		//		writes[0].descriptorCount = imageInfos.size();
		//		writes[0].pImageInfo = imageInfos.data();

		//		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		//		writes[1].dstSet = set->set;
		//		writes[1].dstBinding = 3;
		//		writes[1].dstArrayElement = 0;
		//		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		//		writes[1].descriptorCount = static_cast<uint32_t>(samplerInfos.size());   // or 1 if you use a single shared sampler
		//		writes[1].pImageInfo = samplerInfos.data();

		//		vkUpdateDescriptorSets(core.device_, 2, writes, 0, nullptr);
		//	}

	}


	FrameUniforms UpdateFrameUniforms(uint32_t currentFrame) {

		int width_ = swapchain->GetExtent().width;
		int height_ = swapchain->GetExtent().height;

		auto view = m_camera.getViewMatrix();
		auto proj = m_camera.getProjMatrix();
		proj[1][1] *= -1;

		auto viewProj = proj * view;

		FrameUniforms data{};
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

		// reowrk this
		context->UploadDataToBuffer(
			m_resources.frameUniformsBuffer.resolvedBufferHandle,
			&data,
			sizeof(FrameUniforms)/* * 1*/,
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			0,
			currentFrame * m_resources.frameUniformsBuffer.key.elementSize * m_resources.frameUniformsBuffer.key.elementCount // dynamicOffsetPerFrame
		);
		return data;

	}

	void DrawFrame() {
		// This waits on fence to the swapchainIamge 
		// All control logic around this is in swapchain realyl
		auto fb = AcquireFramebuffer();
		RenderFrame(fb);
		EndAndPresentFramebuffer(fb);
	}


	void RenderFrame(ToyFrameBuffer fb) {

		//ImGuiScopedFrame uiFrame;
		rendergraph->Clear();

		auto color = rendergraph->AddImageView(fb.swapchain.imageViews_[fb.frameIndex]);
		auto depthView = context->images_.get(fb.swapchain.depthImage_)->renderView;
		auto depthViewId = rendergraph->AddImageView(depthView);

		auto vertexBuffers = rendergraph->AddExternalBuffers(m_meshManagerPimpl.GetVertexBuffers());

		std::vector<LogicalImage> colors;
		colors.push_back(color);

		auto mainPass = rendergraph->AddPass(
			{
				//.rg_ = *rendergraph, // dont like how this is done
				//.type = ToyRenderPass::PassType::Rasterize,
			}
			)
			.setOffset({ 0,0 })
			//.setGraphicsPipeline()
			.setExtent(fb.swapchain.extent_)
			.setVertexBuffers(vertexBuffers)
			.setColor(colors, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, { {0.0f, 0.0f, 0.0f, 1.0f} })
			.setDepth(depthViewId, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, { .0f, 0 })
			.setGraphicsPipeline()
			.setRecordingFunc([this, fb](ToyRenderPassContext renderpassctx)
		{

			// This shoudl be done by the rg
			auto dynamic_set = context->descriptorSets_.get(perFrameData[swapchain->currentFrame_].frameDynamicDescriptorSet);
			auto update_after_bind_set = context->descriptorSets_.get(perFrameData[swapchain->currentFrame_].frameDescriptorSet);

			// dont really like this fb passign.
			auto frameData = UpdateFrameUniforms(fb.swapchain.GetCurrentFrameIndex());

			std::vector<int> visible;
			if (m_currentLevel.tree_done) {
				frustumCullBVH(
					m_currentLevel.static_tree,
					0,
					frameData.viewProj,
					m_camera.getPosition(),
					visible
				);
			}
			else {
				assert(false && "tree wasnt done");
			}

			// VISIBLE PIPELINES
			std::unordered_map<VkPipeline, std::vector<int>> pipeline_map;

			for (uint32_t primSlot : visible) {
				if (primSlot >= m_currentLevel.static_tree.primToNode.size()) continue;

				uint32_t node = m_currentLevel.static_tree.primToNode[primSlot];

				auto it = m_currentLevel.materialForNode.find(node);
				if (it == m_currentLevel.materialForNode.end()) continue;

				uint32_t materialIndex = it->second;
				auto matHandle = m_meshManagerPimpl.materials.getHandle(materialIndex);
				auto mat = m_meshManagerPimpl.materials.get(matHandle);
				if (!mat) continue;

				// crude hack
				if (!mat->pipeline) {
					assert(false);
				}

				pipeline_map[mat->pipeline].push_back(primSlot);
			}

			if (pipeline_map.empty()) {
				assert(false);
			}

			uint32_t dynamicOffsets[2] = {
				frameUniformsBinding.dynamicOffsetFrame(swapchain->currentFrame_),
				drawDataBinding.dynamicOffsetFrame(swapchain->currentFrame_),
			};

			//vkCmdBindDescriptorSets(
			//	fb.commandBuffer,
			//	VK_PIPELINE_BIND_POINT_GRAPHICS,
			//	0,
			//	2,
			//	sets,
			//	2,
			//);

			MainViewPushConstants pc{};
			pc.model = glm::mat4(1.0f);

			for (const auto& [han_, visible] : pipeline_map)
			{
				BindPipeline(
					fb.commandBuffer,
					{

					},
					);
				//vkCmdBindPipeline(
				//	fb.commandBuffer,
				//	VK_PIPELINE_BIND_POINT_GRAPHICS,
				//	context->pipelines_.get(han_)->pipeline
				//);

				for (uint32_t visible_prim : visible) {
					uint32_t visible_node = m_currentLevel.static_tree.primToNode[visible_prim];

					auto it = m_currentLevel.meshForNode.find(visible_node);
					if (it == m_currentLevel.meshForNode.end()) continue;

					uint32_t meshIndex = it->second;

					auto mesh_desc = m_meshManagerPimpl.meshes.get(
						m_meshManagerPimpl.meshes.getHandle(meshIndex)
					);

					auto material_instance_index = m_currentLevel.materialForNode[visible_node];
					pc.material_index = material_instance_index;

					auto matHandle = m_meshManagerPimpl.materials.getHandle(material_instance_index);
					auto mat = m_meshManagerPimpl.materials.get(matHandle);

					assert(mat);

					Material::Values pcm = mat->values;

					struct PC_All {
						MainViewPushConstants pc;
						Material::Values mpc;
					};
					PC_All all{ pc, pcm };

					vkCmdPushConstants(
						fb.commandBuffer,
						pipe_lyt.layout,
						VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
						0,
						sizeof(PC_All),
						&all
					);

					for (auto& draw_set : mesh_desc->surfaces) {

						auto& model_ = m_currentLevel.globalTransform[visible_node];
						vkCmdPushConstants(
							fb.commandBuffer,
							pipe_lyt.layout,
							VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
							offsetof(MainViewPushConstants, model),
							sizeof(glm::mat4),
							glm::value_ptr(model_)
						);

						vkCmdDrawIndexed(
							fb.commandBuffer,
							draw_set.index_count,
							1,
							draw_set.first_index,
							draw_set.base_vertex,
							0
						);

					}
				}
			}

			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();

			// UI Here
			ImGuiIO& io = ImGui::GetIO();
			ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
				1000.0f / io.Framerate, io.Framerate);

			ImDrawList* drawList = ImGui::GetForegroundDrawList();
			ImGui::Render();
			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fb.commandBuffer);
			//
		}
			);

		rendergraph->Execute(fb);

	}


private:
	AppResources m_resources;
	Level m_currentLevel;
	std::unique_ptr<MeshManager> m_meshManagerPimpl;
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

	movement.forward_ = input.isKeyHeld(GLFW_KEY_W);
	movement.backward_ = input.isKeyHeld(GLFW_KEY_S);
	movement.left_ = input.isKeyHeld(GLFW_KEY_A);
	movement.right_ = input.isKeyHeld(GLFW_KEY_D);

	// Check the behacviour of this. 
	// is up and down in camera orientation, not world space.
	movement.up_ = input.isKeyHeld(GLFW_KEY_E) || input.isKeyHeld(GLFW_KEY_SPACE);
	movement.down_ = input.isKeyHeld(GLFW_KEY_Q) || input.isKeyHeld(GLFW_KEY_LEFT_CONTROL);

	movement.fastSpeed_ =
		input.isKeyHeld(GLFW_KEY_LEFT_SHIFT) ||
		input.isKeyHeld(GLFW_KEY_RIGHT_SHIFT);

	// NDC, but to frameBuffer dimensions. 
	//glm::vec2 mousePos = glm::vec2(
	//	(static_cast<float>(input.getMouseX()) / static_cast<float>(swapchain->GetExtent().width)) * 2.0f - 1.0f,
	//	1.0f - (static_cast<float>(input.getMouseY()) / static_cast<float>(swapchain->GetExtent().height)) * 2.0f
	//);

	// NDC, to window dimensions. These two may not match, depends on DPI settings. Be careful.
	// Input should be normalized and not be affected by resolution!
	glm::vec2 mousePos = glm::vec2(
		(static_cast<float>(input.getMouseX()) / static_cast<float>(swapchain->windowWidth)) * 2.0f - 1.0f,
		1.0f - (static_cast<float>(input.getMouseY()) / static_cast<float>(swapchain->windowHeight)) * 2.0f
	);

	const bool mousePressed =
		input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_RIGHT);

	if (input.isMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT))
	{
		m_cameraPositioner.resetMousePosition(mousePos);
	}

	m_cameraPositioner.update(
		static_cast<double>(dt),
		mousePos,
		mousePressed
	);

}
