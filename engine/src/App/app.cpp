#include <Engine/App/app.h>

using namespace smvk; // cleanup later dont just pump using here

VulkanApp::VulkanApp(){}
VulkanApp::~VulkanApp() = default;

void VulkanApp::InitializeImGui() {

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Control	ImGui::SetCurrentContext(imgui_ctx);
	// Clipboard via GLFW (optional but nice)
	io.SetClipboardTextFn = [](void*, const char* text) { glfwSetClipboardString(nullptr, text); };
	io.GetClipboardTextFn = [](void*) { return glfwGetClipboardString(nullptr); };

	//auto dp_h = m_context->CreateDescriptorPool();
	//auto im_dp = m_context->descriptorPools_.get(dp_h);

	VkPipelineRenderingCreateInfo pipeline_create{
		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	pipeline_create.pNext = VK_NULL_HANDLE;
	// We are using the single color buffer out.
	pipeline_create.colorAttachmentCount = 1;
	pipeline_create.pColorAttachmentFormats = &swapchain->surfaceFormat_;
	pipeline_create.depthAttachmentFormat = findDepthFormat(vk_core.physicalDevice_);
	pipeline_create.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

	ImGui_ImplGlfw_InitForVulkan(window, /*install callbacks*/ false);
	SwapChainSupportDetails swapChainSupport = querySwapChainSupport(vk_core.physicalDevice_, surface_);

	ImGui_ImplVulkan_InitInfo info{};
	info.Instance = vk_core.instance_;
	info.ApiVersion = VK_API_VERSION_1_3;
	info.PhysicalDevice = vk_core.physicalDevice_;
	info.Device = vk_core.device_;
	info.Queue = vk_core.graphicsQueue_;
	info.DescriptorPool = *im_dp;
	info.MinImageCount = swapChainSupport.capabilities.minImageCount;
	info.ImageCount = swapchain->GetImageCount();
	info.UseDynamicRendering = true,
		info.PipelineRenderingCreateInfo = pipeline_create;
	ImGui_ImplVulkan_Init(&info  /* oUseDynamicRendering=true */);
	//// Upload font texture once
	//VkCommandBuffer cmd = beginCommands();
	//ImGui_ImplVulkan_CreateFontsTexture(cmd);
	//EndSubmitAndWait(cmd);
	//ImGui_ImplVulkan_DestroyFontUploadObjects();

}

bool VulkanApp::Initialize() {

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

	m_swapchain = std::make_unique<VulkanSwapchain>(
		*m_context,
		window,
		static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
		static_cast<uint32_t>(DESIRED_SWAPCHAIN_IMAGE_COUNT)
	);
	m_rendergraph = std::make_unique<RenderGraph>(*m_context);

	//InitializeImGui();

	// FRAME COMMAND 
	// We do pool per frame, because reset pool is faster than reseting indiviual commandBuffers.
	// Also frame is a good lifetime unit/boundary of course!
	//for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
	//	VkCommandBufferAllocateInfo allocInfo{};
	//	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	//	allocInfo.commandPool = perFrameCommandPools[i];
	//	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	//	allocInfo.commandBufferCount = 1;
	//	VK_CHECK(vkAllocateCommandBuffers(vk_core.device_, &allocInfo, &commandBuffers[i]));
	//}

	return true;

}



bool VulkanApp::Run() {

	bool result = Initialize();
	assert(result && "Failed to initialize");

	MainLoop();
	CleanUp();

	return true;

};

void VulkanApp::CleanUp() {
	//vkDeviceWaitIdle(device_);
	delete(&m_swapchain);
	//m_swapchain->CleanupSwapchain();
	// 
	//for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
	//	// destroying the pool fress the commandBuffers too
	//	vkDestroyCommandPool(device_, perFrameCommandPools[i], nullptr);
	//}
	m_context->CleanUp();

	glfwDestroyWindow(window);
	if (m_surface_ != VK_NULL_HANDLE)
	{
		glfwDestroyWindow(window);
		vkDestroySurfaceKHR(core.instance_, core.surface_, nullptr);
		core.surface_ = VK_NULL_HANDLE;
		glfwTerminate();
		window = nullptr;
	}
}

inline void VulkanApp::UpdateDeltaTime() {

	static auto lastFrameTime = std::chrono::high_resolution_clock::now();
	static auto startTime = std::chrono::high_resolution_clock::now();
	auto currentTime = std::chrono::high_resolution_clock::now();

	time_since_start_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - startTime).count();

	delta_time_s = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - lastFrameTime).count();

	lastFrameTime = currentTime;
}

void VulkanApp::MainLoop() {
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

void VulkanApp::keyCallback(
	GLFWwindow* window,
	int key,
	int scancode,
	int action,
	int mods
) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onKey(key, scancode, action, mods);
	}
}


void VulkanApp::mouseButtonCallback(
	GLFWwindow* window,
	int button,
	int action,
	int mods
) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onMouseButton(button, action, mods);
	}
}


void VulkanApp::cursorPositionCallback(
	GLFWwindow* window,
	double xpos,
	double ypos
) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onCursorMove(xpos, ypos);
	}
}


void VulkanApp::scrollCallback(
	GLFWwindow* window,
	double xoffset,
	double yoffset
) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->m_input.onScroll(xoffset, yoffset);
	}
}

void VulkanApp::framebufferResizeCallback(GLFWwindow* window, int width,
	int height) {
	auto app =
		reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	app->framebufferResized = true;
}

#pragma endregion

smvk::FrameBuffer VulkanApp::AcquireFramebuffer()
{
	vkWaitForFences(
		device_,
		1,
		&m_swapchain->GetInFlightFence(),
		VK_TRUE,
		UINT64_MAX
	);

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(
		core.device_,
		m_swapchain->swapchain_,
		UINT64_MAX,
		swapchain->imageAvailableSemaphores_[swapchain->currentFrame_],
		VK_NULL_HANDLE,
		&imageIndex
	);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		swapchain->Recreate();
		assert(false && "not implemented yet");
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	swapchain->SetCurrentImageIndex(imageIndex);

	vkResetFences(core.device_, 1, &swapchain->inFlightFences_[swapchain->currentFrame_]);
	vkResetCommandPool(core.device_, perFrameCommandPools[swapchain->currentFrame_], 0);

	UpdateDeltaTime();

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	if (vkBeginCommandBuffer(commandBuffers[swapchain->currentFrame_], &beginInfo) != VK_SUCCESS) {
		throw std::runtime_error("failed to begin recording command buffer!");
	}

	VkImageMemoryBarrier barrierToColorAttachment{};
	barrierToColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrierToColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrierToColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrierToColorAttachment.srcAccessMask = 0;
	barrierToColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrierToColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrierToColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrierToColorAttachment.image = swapchain->images_[imageIndex];
	barrierToColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrierToColorAttachment.subresourceRange.baseMipLevel = 0;
	barrierToColorAttachment.subresourceRange.levelCount = 1;
	barrierToColorAttachment.subresourceRange.baseArrayLayer = 0;
	barrierToColorAttachment.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(
		commandBuffers[swapchain->currentFrame_],
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrierToColorAttachment
	);

	FrameBuffer frameBufferContext_{
		.frameIndex = swapchain->currentFrame_,
		.commandBuffer = commandBuffers[swapchain->currentFrame_],
		.swapchain = *swapchain

	};
	return frameBufferContext_;
}

void VulkanApp::EndAndPresentFramebuffer(FrameBuffer& fb)
{

	ImageBarrier(
		fb.commandBuffer,
		m_swapchain->GetCurrentImage(),
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		{ // maybe the api surface should specialize subresoruces to avoid having to think about this
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_NONE,
		VK_ACCESS_2_NONE

		);

	// 
	if (vkEndCommandBuffer(fb.commandBuffer) != VK_SUCCESS) {
		throw std::runtime_error("failed to record command buffer!");
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	VkSemaphore waitSemaphores[] = { m_swapchain->imageAvailableSemaphores_[swapchain->currentFrame_] };
	VkPipelineStageFlags waitStages[] = {
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = waitSemaphores;
	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &fb.commandBuffer;
	VkSemaphore signalSemaphores[] = { swapchain->renderFinishedSemaphores_[swapchain->currentFrame_] };
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	if (
		vkQueueSubmit(
			GetGraphicsQueue(),
			1,
			&submitInfo,
			swapchain->GetInFlightFence()
		) != VK_SUCCESS
		) {
		assert(false && "failed to submit draw");
		throw std::runtime_error("failed to submit draw command buffer!");
	}

	//
	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;
	VkSwapchainKHR swapChains[] = { swapchain->swapchain_ };
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &swapchain->currentImageIndex_;

	VkResult result = vkQueuePresentKHR(GetPresentQueue(), &presentInfo);

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
		framebufferResized) {
		framebufferResized = false;
		swapchain->Recreate();
	}
	else if (result != VK_SUCCESS) {
		throw std::runtime_error("failed to present swap chain image!");
	}

	//m_submitter.waitAll(); // maybe ?
	//deferredTask.doAll()
	swapchain->AdvanceFrame();

}
