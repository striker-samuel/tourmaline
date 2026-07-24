#pragma once
#include <Engine/App/app.h>

#include <Engine/Renderer/resource_manager.h>
#include <Engine/Renderer/rendergraph.h>

#include <Engine/Utils/bvh.h>
#include <Engine/Utils/gltf.h> 
#include <Engine/Camera/camera.h> 

struct Texture;
struct Level : Scene {
	MeshManager* m_meshManagerPimpl = nullptr;
	BvhTree tree = {};
	BvhTree static_tree = {};
	bool tree_done = false;
	//std::map<uint32_t, MeshHandle> m_meshForNode;
	//Level(MeshManager* meshManager) : m_meshManagerPimpl(meshManager) {};
	~Level() {}
	smvk::Pool<Texture, smvk::Image> textures_;

	void Upload(
		std::filesystem::path gltf_model_path
	) {
		tinygltf::Model model;
		LoadModelFromAsciiFile(model, gltf_model_path.string().c_str());
		GltfToSceneImporter importer(
			*this,
			*m_meshManagerPimpl,
			model,
			std::move(gltf_model_path
			)
		);
		importer.importNodeRecursive(/*rootNodeIdx*/0, /*parentSceneNode=*/-1, /*level=*/0);
		// This is needed or else scene will not carry transforms effectively
		markAsChanged(*this, 0);
		recalculateGlobalTransforms(*this);
		//m_meshManagerPimpl->ctx_.ProcessDeferrredTasks(); //  TODO. fails
	}
};
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

struct ViewportResources {};

struct AppResources {
	smvk::LogicalBufferHandle frameUniformsBuffer{};
	smvk::LogicalBufferHandle drawDataBuffer{};
};

class TestApp : public smvk::VulkanApp {
	using Super = smvk::VulkanApp;
public:
	TestApp() {};
	~TestApp() {};
private:
	uint32_t kMaxDraws = 1000;

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

		m_meshManagerPimpl = std::make_unique<MeshManager>(*m_context);
		m_camera.setPerspective(45.0f, float(m_swapchain->GetExtent().width) / float(m_swapchain->GetExtent().height),
			default_near_clip, default_far_clip
		);
		//
		std::filesystem::path p(gltf_meshes_paths[0]);
		m_currentLevel.m_meshManagerPimpl = m_meshManagerPimpl.get();
		m_currentLevel.Upload(p);
		m_currentLevel.static_tree = treeFromScene(m_currentLevel,
			[&](uint32_t index) -> const Mesh& {return this->m_meshManagerPimpl->meshes.objects_[index].obj_; }
		);
		m_currentLevel.tree_done = true;
		m_context->ProcessDeferrredTasks();

		return true;
	};

	FrameUniforms UpdateFrameUniforms(uint32_t currentFrame, smvk::BufferHandle handle) {

		int width_ = m_swapchain->GetExtent().width;
		int height_ = m_swapchain->GetExtent().height;

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

		m_context->UploadDataToBuffer(
			handle,
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

	void DrawFrame() {
		// This waits on fence to the swapchainIamge 
		// All control logic around this is in swapchain realyl
		auto fb = m_swapchain->AcquireFramebuffer();
		UpdateDeltaTime();
		RenderFrame(fb);
		m_swapchain->EndAndPresentFramebuffer(fb);
	}


	void RenderFrame(smvk::FrameBuffer fb) {
		m_rendergraph->Clear();

		// Update Buffer Contents.
		m_resources.frameUniformsBuffer = m_rendergraph->AddRingBuffer<FrameUniforms>(1, "frameUniforms");
		m_resources.drawDataBuffer = m_rendergraph->AddRingBuffer<DrawSetData>(kMaxDraws, "drawData");

		// ATTACHMENT SETUP
		auto colorProxy = m_rendergraph->AddImageView(fb.swapchain.imageViews_[fb.frameIndex]);
		auto depthProxy = m_rendergraph->AddImageView(
			m_context->images_.get(fb.swapchain.depthImage_)->renderView //hacky
		);
		std::vector<smvk::LogicalImageViewHandle> colors;
		colors.push_back(colorProxy);

		auto& mainPass = m_rendergraph->AddPass()
			.setOffset({ 0,0 })
			.setExtent(fb.swapchain.extent_)
			.setColor(colors, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, { {0.0f, 0.0f, 0.0f, 1.0f} })
			.setDepth(depthProxy, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, { .0f, 0 })
			.setRecordingFunc([this, fb](smvk::RenderPassContext pass)
		{
			auto frameData = UpdateFrameUniforms(fb.swapchain.currentImageIndex_);
			auto meshMan = m_meshManagerPimpl.get();

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

			for (Hierarchy node : m_currentLevel.hierarchy)
			{
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


					auto mesh_desc = meshMan->meshes.get(
						meshMan->meshes.getHandle(meshIndex)
					);

					auto material_instance_index = m_currentLevel.materialForNode[visible_node];
					//pc.material_index = material_instance_index;

					auto matHandle = m_meshManagerPimpl->materials.getHandle(material_instance_index);
					auto mat = m_meshManagerPimpl->materials.get(matHandle);

					assert(mat);

					//Material::Values pcm = mat->values;

					//struct PC_All {
					//	MainViewPushConstants pc;
					//	Material::Values mpc;
					//};
					//PC_All all{ pc, pcm };

					//vkCmdPushConstants(
					//	fb.commandBuffer,
					//	pipe_lyt.layout,
					//	VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					//	0,
					//	sizeof(PC_All),
					//	&all
					//);

					for (auto& draw_set : mesh_desc->surfaces) {

						//auto& model_ = m_currentLevel.globalTransform[visible_node];
						//vkCmdPushConstants(
						//	fb.commandBuffer,
						//	pipe_lyt.layout,
						//	VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
						//	offsetof(MainViewPushConstants, model),
						//	sizeof(glm::mat4),
						//	glm::value_ptr(model_)
						//);

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

			//ImGui_ImplVulkan_NewFrame();
			//ImGui_ImplGlfw_NewFrame();
			//ImGui::NewFrame();

			//// UI Here
			//ImGuiIO& io = ImGui::GetIO();
			//ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
			//	1000.0f / io.Framerate, io.Framerate);

			//ImDrawList* drawList = ImGui::GetForegroundDrawList();
			//ImGui::Render();
			//ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fb.commandBuffer);
			//
		}
			);

		m_rendergraph->Execute(fb);
		m_context->ProcessDeferrredTasks();

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
