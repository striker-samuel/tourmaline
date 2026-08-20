#pragma once
#include <iostream>
#include <filesystem>

#include <Engine/App/app.h>
#include <Engine/App/world.h>

#include <Engine/Camera/camera.h> 
#include <Engine/Utils/gltf.h>
#include <Engine/Renderer/rendergraph.h>
#include <Engine/platform.h>

// Constants //
static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;
std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
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
	smvk::Holder<smvk::ShaderModuleHandle> vertShader;
	smvk::Holder<smvk::ShaderModuleHandle> fragShader;
	smvk::ShaderProgram mainProgram;
};

class TestApp : public smvk::VulkanApp {
	using Super = smvk::VulkanApp;
public:
	//TestApp() {};
	//~TestApp() {};
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

		m_camera.setPerspective(
			45.0f,
			float(m_swapchain->GetExtent().width) / float(m_swapchain->GetExtent().height),
			default_near_clip, default_far_clip
		);
		std::filesystem::path p(gltf_meshes_paths[0]);
		//m_context->ProcessDeferrredTasks();
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
		m_resources.vertShader = m_context->CreateShaderModule(vertCode, "vert");
		m_resources.fragShader = m_context->CreateShaderModule(fragCode, "frag");

		auto vert = smvk::Shader{
			.vkShaderModule = m_resources.vertShader,
			.entryPoint = "mainVS",
			.stage = VK_SHADER_STAGE_VERTEX_BIT
		};
		auto frag = smvk::Shader{
			.vkShaderModule = m_resources.fragShader,
			.entryPoint = "mainPS",
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT
		};
		m_resources.mainProgram = {
			.shaders = {vert, frag},
			.shaderCount = 2
		};

		smvk::AllocateStaticVertexFmt(*m_context);
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

		return true;
	};

	void CleanUp() override {
		Super::CleanUp();
	}

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

		auto& mainPass = m_rendergraph->AddPass()
			.setOffset({ 0,0 })
			.setExtent(fb.swapchain.extent_)
			.setColor(colors, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, { {0.0f, 0.0f, 0.0f, 1.0f} })  // Rendering directly to swapchainImage!
			.setDepth(logicalDepthImageView, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, { .0f, 0 })
			.setRecordingFunc([this, fb](smvk::CommandBuffer& cmd)
		{
			auto frameData = UpdateFrameUniforms(fb.swapchain.currentImageIndex_);
			cmd.BindRenderPipeline(
				m_resources.mainProgram,
				smvk::VertexFormat::StaticVertexFormat
			);

			//Push frame
			struct alignas(16) PushConstants {
				float model[16];
				smvk::MaterialValues mpc{};
				uint64_t frameUniforms;
			};
			uint64_t frameUniforms =
				m_context->buffers_
				.get(m_resources.frameUniforms)
				->deviceAddress;
			cmd.PushConstants(
				offsetof(PushConstants, frameUniforms),
				sizeof(frameUniforms),
				&frameUniforms
			);

			m_world->forEachNode(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_PassedCullTest
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
				auto mat = m_world->GetMaterial(materialId);

				PushConstants pc{};
				pc.mpc = mat->values;
				const float* modelData = glm::value_ptr(model);
				std::copy_n(modelData, 16, pc.model);

				cmd.PushConstants(
					0,
					offsetof(PushConstants, frameUniforms),
					&pc
				);

				for (auto& draw_set : meshDesc->geometry.surfaces) {
					vkCmdDrawIndexed(
						fb.commandBuffer,
						draw_set.indexCount,
						1,
						draw_set.firstIndex,
						draw_set.baseVertex,
						0
					);
				};
			}
			);

		}
			);


		//// UI PASS
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
