#include <iostream>
#include <chrono>

#include <Engine/App/app.h>

#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>
#include <Engine/Renderer/rendergraph.h>
#include <Engine/platform.h>


smvk::VulkanApp::VulkanApp() {}
smvk::VulkanApp::~VulkanApp() = default;

void smvk::VulkanApp::InitializeImGui() {}
//
//void VulkanApp::InitializeImGui() {
//
//	// Setup Dear ImGui context
//	IMGUI_CHECKVERSION();
//	ImGui::CreateContext();
//	ImGuiIO& io = ImGui::GetIO(); (void)io;
//	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
//	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Control	ImGui::SetCurrentContext(imgui_ctx);
//	// Clipboard via GLFW (optional but nice)
//	io.SetClipboardTextFn = [](void*, const char* text) { glfwSetClipboardString(nullptr, text); };
//	io.GetClipboardTextFn = [](void*) { return glfwGetClipboardString(nullptr); };
//
//	//auto dp_h = m_context->CreateDescriptorPool();
//	//auto im_dp = m_context->descriptorPools_.get(dp_h);
//
//	VkPipelineRenderingCreateInfo pipeline_create{
//		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
//	pipeline_create.pNext = VK_NULL_HANDLE;
//	// We are using the single color buffer out.
//	pipeline_create.colorAttachmentCount = 1;
//	pipeline_create.pColorAttachmentFormats = &swapchain->vk_surfaceFormat;
//	pipeline_create.depthAttachmentFormat = findDepthFormat(vk_core.physicalDevice_);
//	pipeline_create.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
//
//	ImGui_ImplGlfw_InitForVulkan(window, /*install callbacks*/ false);
//	SwapChainSupportDetails swapChainSupport = querySwapChainSupport(vk_core.physicalDevice_, surface_);
//
//	ImGui_ImplVulkan_InitInfo info{};
//	info.Instance = vk_core.instance_;
//	info.ApiVersion = VK_API_VERSION_1_3;
//	info.PhysicalDevice = vk_core.physicalDevice_;
//	info.Device = vk_core.device_;
//	info.Queue = vk_core.graphicsQueue_;
//	info.DescriptorPool = *im_dp;
//	info.MinImageCount = swapChainSupport.capabilities.minImageCount;
//	info.ImageCount = swapchain->GetImageCount();
//	info.UseDynamicRendering = true,
//		info.PipelineRenderingCreateInfo = pipeline_create;
//	ImGui_ImplVulkan_Init(&info  /* oUseDynamicRendering=true */);
//	//// Upload font texture once
//	//VkCommandBuffer cmd = beginCommands();
//	//ImGui_ImplVulkan_CreateFontsTexture(cmd);
//	//EndSubmitAndWait(cmd);
//	//ImGui_ImplVulkan_DestroyFontUploadObjects();
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
	m_context = std::make_unique<Context>(*core_.get());

	m_swapchain = std::make_unique<VulkanSwapchain>(
		*m_context,
		window
		//,
		//static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
		//static_cast<uint32_t>(DESIRED_SWAPCHAIN_IMAGE_COUNT)
	);

	// Maybe not ok to do this here.
	//for (int i = 0; i < m_swapchain->framesInFlight_; i++)
	//{
	//	if (m_dpools[i].empty()) {
	//		m_dpools[i] = m_context->CreateDescriptorPool();
	//	}
	//}
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

void smvk::VulkanApp::CleanUp() {
	//vkDeviceWaitIdle(device_);
	//for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
	//	// destroying the pool fress the commandBuffers too
	//	vkDestroyCommandPool(device_, perFrameCommandPools[i], nullptr);
	//}
	m_context.release(); // destroy resources and free them
	if (m_swapchain) // if swap
	{
		m_swapchain.reset();
		core_.reset();
		glfwDestroyWindow(window);
		glfwTerminate();
		window = nullptr;
	}
	else
	{
		core_.reset();
	}
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


