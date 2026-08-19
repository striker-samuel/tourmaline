#include <iostream>
#include <chrono>

#include <Engine/App/app.h>

#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>
#include <Engine/Renderer/rendergraph.h>
#include <Engine/platform.h>


smvk::VulkanApp::VulkanApp() {}
smvk::VulkanApp::~VulkanApp() = default;
//void smvk::VulkanApp::InitializeImGui() {
//
//	// Setup Dear ImGui context
//	IMGUI_CHECKVERSION();
//	ImGui::CreateContext();
//	ImGuiIO& io = ImGui::GetIO(); (void)io;
//	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
//	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Control	ImGui::SetCurrentContext(imgui_ctx);
//	//io.SetClipboardTextFn = [](void*, const char* text) { glfwSetClipboardString(nullptr, text); };
//	//io.GetClipboardTextFn = [](void*) { return glfwGetClipboardString(nullptr); };
//
//	VkPipelineRenderingCreateInfo pipeline_create{
//		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
//	pipeline_create.pNext = VK_NULL_HANDLE;
//	// We are using the single color buffer out.
//	pipeline_create.colorAttachmentCount = 1;
//	pipeline_create.pColorAttachmentFormats = &m_swapchain->vk_surfaceFormat_;
//	pipeline_create.depthAttachmentFormat = m_swapchain->vk_depthFormat_;
//	pipeline_create.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
//
//	ImGui_ImplGlfw_InitForVulkan(window, /*install callbacks*/ false);
//
//	imgui_descriptorPool = m_context->CreateDescriptorPool();
//	auto dp = m_context->descriptorPools_.get(imgui_descriptorPool);
//
//	ImGui_ImplVulkan_InitInfo info{};
//	info.Instance = core_->GetInstance();
//	info.ApiVersion = m_context->core_.config_.version_;
//	info.PhysicalDevice = core_->GetPhysicalDevice();
//	info.Device = core_->GetDevice();
//	info.Queue = core_->GetGraphicsQueue();
//	info.DescriptorPool = *dp;
//	info.MinImageCount = m_swapchain->GetMinImageCount();
//	info.ImageCount = m_swapchain->GetImageCount();
//	info.UseDynamicRendering = true,
//	info.PipelineRenderingCreateInfo = pipeline_create;
//	ImGui_ImplVulkan_Init(&info  /* oUseDynamicRendering=true */);
//
//	// I think this is no longer needed
//	//auto han = m_context->m_submitter->acquire();
//	//auto cmd = m_context->m_submitter->GetCommandBuffer(han);
//	//ImGui_ImplVulkan_CreateFontsTexture(cmd.cmd);
//	//ImGui_ImplVulkan_DestroyFontUploadObjects(cmd.cmd);
//	//m_context->m_submitter->submit(han);
//
//}

bool smvk::VulkanApp::Initialize() {

	if (volkInitialize() != VK_SUCCESS) {
		std::cerr << "Failed to initialize Volk\n";
		glfwTerminate();
		return false;
	};

	// GLFW;

	if (!glfwInit()) {
		std::cerr << "Failed to initialize GLFW\n";
		return false;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	int width_ = DEFAULT_WIDTH;
	int height_ = DEFAULT_HEIGHT;
	window = glfwCreateWindow(width_, height_, "Vulkan", nullptr, nullptr);

	if (!window) {
		std::cerr << "Failed to create GLFW window\n";
		glfwTerminate();
		return false;
	}
	glfwMakeContextCurrent(window);
	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	glfwSetWindowTitle(window, /*ENGINE_NAME*/ "samuelmartin.1999@outlok.com");

	glfwSetKeyCallback(window, keyCallback);
	glfwSetMouseButtonCallback(window, mouseButtonCallback);
	glfwSetCursorPosCallback(window, cursorPositionCallback);
	glfwSetScrollCallback(window, scrollCallback);

	glfwSetWindowUserPointer(window, this);
	glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

	core_ = CreateVulkanCoreWithWindow(VulkanCore::CoreConfiguration{}, window);
	m_context = std::make_unique<VulkanResourceManager>(*core_.get());
	m_swapchain = std::make_unique<VulkanSwapchain>(
		*m_context,
		window
	);

	m_rendergraph = std::make_unique<RenderGraph>(*m_context);
	//InitializeImGui();


	return true;

}


bool smvk::VulkanApp::Run() {
	bool result = Initialize();
	assert(result && "Failed to initialize");
	MainLoop();
	CleanUp();
	return true;

};

void smvk::VulkanApp::CleanUp()
{
	vkDeviceWaitIdle(core_->GetDevice());

	//
	//smvk::DeallocateStaticVertexFmt(*m_context.get());

	if (window != nullptr) {
		glfwDestroyWindow(window);
		window = nullptr;
	}
	glfwTerminate();
}


void smvk::VulkanApp::UpdateDeltaTime() {

	static auto lastFrameTime = std::chrono::high_resolution_clock::now();
	static auto startTime = std::chrono::high_resolution_clock::now();
	auto currentTime = std::chrono::high_resolution_clock::now();

	time_since_start_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - startTime).count();

	delta_time_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - lastFrameTime).count();

	lastFrameTime = currentTime;
}

void smvk::VulkanApp::MainLoop() {
	while (!glfwWindowShouldClose(window)) {
		m_input.beginFrame();
		if (m_input.isKeyHeld(GLFW_KEY_ESCAPE)) {
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}
		glfwPollEvents();
		EarlyUpdate();
		Update();
		LateUpdate();

		DrawFrame();
	}
}

#pragma region ===[ Callbacks ]===

void smvk::VulkanApp::keyCallback(
	GLFWwindow* window,
	int key,
	int scancode,
	int action,
	int mods
) {
	auto* app = static_cast<smvk::VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onKey(key, scancode, action, mods);
	}
}


void smvk::VulkanApp::mouseButtonCallback(
	GLFWwindow* window,
	int button,
	int action,
	int mods
) {
	auto* app = static_cast<smvk::VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onMouseButton(button, action, mods);
	}
}


void smvk::VulkanApp::cursorPositionCallback(
	GLFWwindow* window,
	double xpos,
	double ypos
) {
	auto* app = static_cast<smvk::VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onCursorMove(xpos, ypos);
	}
}


void smvk::VulkanApp::scrollCallback(
	GLFWwindow* window,
	double xoffset,
	double yoffset
) {
	auto* app = static_cast<smvk::VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onScroll(xoffset, yoffset);
	}
}

void smvk::VulkanApp::framebufferResizeCallback(GLFWwindow* window, int width,
	int height) {
	auto app =
		reinterpret_cast<smvk::VulkanApp*>(glfwGetWindowUserPointer(window));
	app->m_swapchain.get()->framebufferResized = true;
}

#pragma endregion


