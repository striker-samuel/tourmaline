#pragma once
#include <Engine/App/app.h>

#include <Engine/Renderer/resource_manager.h>
#include <Engine/Renderer/rendergraph.h>

#include <Engine/Utils/bvh.h>
#include <Engine/Utils/gltf.h> 
#include <Engine/Camera/camera.h> 

#include <Engine/platform.h>

// Constants //
static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;
std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
};

// Bring Phyre asset to runtime scene mapping-
//struct AssetReference{};
//struct Cluster{};

//
struct Level : Scene {
	MeshManager* m_meshManagerPimpl = nullptr;
	BvhTree tree = {};
	// 
	BvhTree static_tree = {};
	// We should split static scene objects to another tree, or maybe flag nodes an static and skip bhv reconstruction each frame for them
	bool tree_done = false;
	//std::map<uint32_t, MeshHandle> m_meshForNode;
	//Level(MeshManager* meshManager) : m_meshManagerPimpl(meshManager) {};
	~Level() {}
	smvk::Pool<smvk::Texture, smvk::Image> textures_;

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
		m_meshManagerPimpl->ctx_.ProcessDeferrredTasks();
	}
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
struct ViewportResources {};
struct AppResources {
	smvk::Holder<smvk::BufferHandle> frameUniforms;
	smvk::ShaderProgram mainProgram;
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

		m_resources.frameUniforms = m_context->CreateBuffer(
			{
				.size = sizeof(FrameUniforms) * m_swapchain->framesInFlight_,
				.vma_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
				.usage_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			}
		);

		// Main ShaderProgram
		auto vertCode = readFile("content/shaders/default.vert.hlsl.spv");
		auto fragCode = readFile("content/shaders/default.frag.hlsl.spv");
		auto vert = smvk::Shader{
			.vkShaderModule = m_context->CreateShaderModule(vertCode).release(),
			.entryPoint = "mainVS",
			.stage = VK_SHADER_STAGE_VERTEX_BIT
		};
		auto frag = smvk::Shader{
			.vkShaderModule = m_context->CreateShaderModule(fragCode).release(),
			.entryPoint = "mainPS",
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT
		};
		m_resources.mainProgram = {
			.shaders = {vert, frag},
			.shaderCount = 2
		};

		// dirty textures right now only happen here so this would be more efficient like this...
		// but lets not optimize this now. for proper optimization we need proper app scene load/unload semantics, we actually dont have now
		//m_context->UpdateBindings();

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

	void DrawFrame() {
		auto fb = m_swapchain->AcquireFramebuffer();
		UpdateDeltaTime();
		RenderFrame(fb);
		m_swapchain->EndAndPresentFramebuffer(fb);
	}


	void RenderFrame(smvk::FrameBuffer fb) {

		m_rendergraph->NewFrame();
		m_context->UpdateBindings();

		// ATTACHMENT SETUP
		auto logicalSwapchainImage = m_rendergraph->AddImageView(
			fb.swapchain.imageViews_[fb.imageIndex],
			fb.swapchain.vk_surfaceFormat_
		);
		auto depthImage = m_context->images_.get(fb.swapchain.depthImage_);
		auto logicalDepthImageView = m_rendergraph->AddImageView(
			depthImage->sampleView,
			fb.swapchain.vk_depthFormat_
		);
		std::vector<smvk::LogicalImageViewHandle> colors;
		colors.push_back(logicalSwapchainImage);
		//
		m_meshManagerPimpl->BindIndexVertexBuffers(fb.commandBuffer);

		auto& mainPass = m_rendergraph->AddPass()
			.setOffset({ 0,0 })
			.setExtent(fb.swapchain.extent_)
			.setColor(colors, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, { {0.0f, 0.0f, 0.0f, 1.0f} })  // Rendering directly to swapchainImage!
			.setDepth(logicalDepthImageView, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, { .0f, 0 })
			.setRecordingFunc([this, fb](smvk::RenderPassContext pass)
		{
			auto frameData = UpdateFrameUniforms(fb.swapchain.currentImageIndex_);
			auto mesh_manager = m_meshManagerPimpl.get();

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
			m_rendergraph->BindRenderPipeline(fb.commandBuffer, m_resources.mainProgram);

			//Push frame
			struct alignas(16) PushConstants {
				float model[16];
				Material::Values mpc{};
				uint64_t frameUniforms;
			};
			uint64_t frameUniforms =
				m_context->buffers_
				.get(m_resources.frameUniforms)
				->deviceAddress;

			m_rendergraph->PushConstants(
				fb.commandBuffer,
				offsetof(PushConstants, frameUniforms),
				sizeof(frameUniforms),
				&frameUniforms
			);
			//
			for (const Hierarchy& node : m_currentLevel.hierarchy)
			{

				for (uint32_t visible_prim : visible) {
					uint32_t visible_node = m_currentLevel.static_tree.primToNode[visible_prim];

					auto it = m_currentLevel.meshForNode.find(visible_node);
					if (it == m_currentLevel.meshForNode.end()) continue;

					uint32_t meshIndex = it->second;
					auto mesh_desc = mesh_manager->meshes.get(
						mesh_manager->meshes.getHandle(meshIndex)
					);

					auto material_instance_index = m_currentLevel.materialForNode[visible_node];
					auto matHandle = m_meshManagerPimpl->materials.getHandle(material_instance_index);
					auto mat = m_meshManagerPimpl->materials.get(matHandle);
					assert(mat);

					auto& model = m_currentLevel.globalTransform[visible_node];
					PushConstants pc{};
					pc.mpc = mat->values;
					const float* modelData = glm::value_ptr(model);
					std::copy_n(modelData, 16, pc.model);

					m_rendergraph->PushConstants(
						fb.commandBuffer,
						0,
						offsetof(PushConstants, frameUniforms),
						&pc
					);

					for (auto& draw_set : mesh_desc->surfaces) {
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
	AppResources m_resources{};
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
