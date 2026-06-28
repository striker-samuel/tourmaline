#pragma once

#include <algorithm>
#include <cassert>
//
#include "volk.h"
#include "vk_mem_alloc.h"
//#include <GLFW/glfw3.h>
#include "input.h"

#include <future>

// Imgui
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
//

#include <Engine/Vulkan/core.h>
#include <Engine/Renderer/context.h>
#include <Engine/Vulkan/VulkanSwapchain.h>
#include <Engine/Renderer/rendergraph.h>

#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>

#include <Engine/Utils/gltf.h>
#include <Engine/platform.h>
#include <Engine/Utils/bvh.h>

class VulkanApp {
public:
	VulkanCore core;
	Input input;

	// TODO : check constructo
	VulkanApp() {};
	virtual ~VulkanApp() = default;

	virtual void DrawFrame() = 0;

	virtual void CleanUp();
	virtual bool Initialize();

	bool Run() {

		bool result = Initialize();
		assert(result && "Failed to initialize");

		MainLoop();
		CleanUp();

		return true;

	};

protected:

	Context* context = nullptr;
	std::unique_ptr<RenderGraph> rendergraph;
	ImGuiContext* imgui_ctx = nullptr;
	VulkanSwapchain* swapchain = nullptr;

#pragma region ===[ App / Core State ]===

	float delta_time_s = .0f;
	// This needs rework. naive float for elpased time leads to problems in the long run!
	float time_since_start_s = .0f;

	// the purporse of this is manly culling debug
	glm::mat4 frozenMainView = {};

#pragma endregion

#pragma region ===[ GLFW / Window State ]===
	GLFWwindow * window = nullptr;
#pragma endregion

#pragma region ===[ Frame Commands List ]===

	VkCommandPool perFrameCommandPools[MAX_FRAMES_IN_FLIGHT]{};
	VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT]{};

#pragma endregion

#pragma region ===[ Flags / Events ]===
	bool framebufferResized = false;
#pragma endregion

#pragma region Initialization

	void InitializeImGui();

#pragma endregion

	void UpdateDeltaTime() {

		static auto lastFrameTime = std::chrono::high_resolution_clock::now();
		static auto startTime = std::chrono::high_resolution_clock::now();
		auto currentTime = std::chrono::high_resolution_clock::now();

		time_since_start_s = std::chrono::duration<float, std::chrono::seconds::period>(
			currentTime - startTime).count();

		delta_time_s = std::chrono::duration<float, std::chrono::seconds::period>(
			currentTime - lastFrameTime).count();

		lastFrameTime = currentTime;
	}

#pragma region MainLoop

	void MainLoop() {

		while (!glfwWindowShouldClose(window)) {

			//OPTICK_FRAME("MainThread");

			input.beginFrame();
			glfwPollEvents();

			EarlyUpdate();
			Update();
			LateUpdate();

			DrawFrame();
			// 

		}
	}

#pragma endregion

#pragma region Events

	virtual void EarlyUpdate() {};
	virtual void Update() {}
	virtual void LateUpdate() {}

#pragma endregion


#pragma region ===[ GlfwCallbacks ]===

	static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

	static void keyCallback(
		GLFWwindow* window,
		int key,
		int scancode,
		int action,
		int mods
	);

	static void mouseButtonCallback(
		GLFWwindow* window,
		int button,
		int action,
		int mods
	);

	static void cursorPositionCallback(
		GLFWwindow* window,
		double xpos,
		double ypos
	);

	static void scrollCallback(
		GLFWwindow* window,
		double xoffset,
		double yoffset
	);

#pragma endregion

#pragma region ===[ Frame Lifecycle ]===

	ToyFrameBuffer AcquireFramebuffer();
	void EndAndPresentFramebuffer(ToyFrameBuffer& cb);

#pragma endregion

};


// IMP
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

	//auto im_dp = context->descriptorSetPools_.get(context->imgui_descriptor_pool);
	auto dp_h = context->createDescriptorPool();
	//auto imgui_dp = context->de

	// Provide information for dynamic rendering
	VkPipelineRenderingCreateInfo pipeline_create{
		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	pipeline_create.pNext = VK_NULL_HANDLE;

	// We are using the single color buffer out.
	pipeline_create.colorAttachmentCount = 1;
	pipeline_create.pColorAttachmentFormats = &swapchain->surfaceFormat_;

	pipeline_create.depthAttachmentFormat = findDepthFormat(core.physicalDevice_);
	pipeline_create.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
	ImGui_ImplGlfw_InitForVulkan(window, /*install callbacks*/ false);

	SwapChainSupportDetails swapChainSupport =
		querySwapChainSupport(core.physicalDevice_, core.surface_);

	ImGui_ImplVulkan_InitInfo info{};
	info.Instance = core.instance_;
	info.ApiVersion = VK_API_VERSION_1_3;
	info.PhysicalDevice = core.physicalDevice_;
	info.Device = core.device_;
	info.Queue = core.graphicsQueue_;
	info.DescriptorPool = *im_dp;
	info.MinImageCount = swapChainSupport.capabilities.minImageCount;
	info.ImageCount = swapchain->GetImageCount();
	info.UseDynamicRendering = true,
		info.PipelineRenderingCreateInfo = pipeline_create;

	//PipelineInfoMain.PipelineRenderingCreateInfo structure.
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

	core.Init();
	if (glfwCreateWindowSurface(core.instance_, window, nullptr, &core.surface_) !=
		VK_SUCCESS) {
		throw std::runtime_error("failed to create window surface!");
	}
	core.Init2();
	context = new Context(core);

	SwapChainSupportDetails swapChainSupport =
		querySwapChainSupport(core.physicalDevice_, core.surface_);
	VkSurfaceFormatKHR surfaceFormat =
		chooseSwapSurfaceFormat(swapChainSupport.formats);

	// SWAPCHAIN
	swapchain = new VulkanSwapchain(
		*context,
		window,
		static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
		static_cast<uint32_t>(DESIRED_SWAPCHAIN_IMAGE_COUNT)
	);
	InitializeImGui();

	// FRAME COMMAND 
	QueueFamilyIndices queueFamilyIndices = findQueueFamilies(core.physicalDevice_, core.surface_);
	VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value(),
	};

	for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
		if (vkCreateCommandPool(core.device_, &poolInfo, nullptr, &perFrameCommandPools[fn]) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create graphics command pool!");
		}
	}

	// We do pool per frame, because reset pool is faster than reseting indiviual commandBuffers.
	// Also frame is a good lifetime unit/boundary of course!
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = perFrameCommandPools[i];
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;

		VK_CHECK(vkAllocateCommandBuffers(core.device_, &allocInfo, &commandBuffers[i]));
	}

	rendergraph = std::make_unique<RenderGraph>(*context);
	return true;

}

#pragma region ===[ Callbacks ]===

void VulkanApp::framebufferResizeCallback(GLFWwindow* window, int width,
	int height) {
	auto app =
		reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	app->framebufferResized = true;
}

#pragma endregion



void VulkanApp::CleanUp() {

	vkDeviceWaitIdle(core.device_);
	swapchain->CleanupSwapchain();

	// 
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		// destroying the pool fress the commandBuffers too
		vkDestroyCommandPool(core.device_, perFrameCommandPools[i], nullptr);
	}
	context->CleanUp();

	vkDestroyDevice(core.device_, nullptr);

	if (core.surface_ != VK_NULL_HANDLE)
	{
		glfwDestroyWindow(window);
		vkDestroySurfaceKHR(core.instance_, core.surface_, nullptr);
		core.surface_ = VK_NULL_HANDLE;
		glfwTerminate();
		window = nullptr;
	}
	// 
	vkDestroyDebugUtilsMessengerEXT(
		core.instance_,
		core.debugMessenger_,
		nullptr
	);
	vkDestroyInstance(core.instance_, nullptr);

	// then destroy the device; this releases VkQueue handles too
	core.presentQueue_ = VK_NULL_HANDLE;
	core.graphicsQueue_ = VK_NULL_HANDLE;
	core.instance_ = VK_NULL_HANDLE;

}

void VulkanApp::keyCallback(
	GLFWwindow* window,
	int key,
	int scancode,
	int action,
	int mods
) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->input.onKey(key, scancode, action, mods);
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
		app->input.onMouseButton(button, action, mods);
	}
}


void VulkanApp::cursorPositionCallback(
	GLFWwindow* window,
	double xpos,
	double ypos
) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->input.onCursorMove(xpos, ypos);
	}
}


void VulkanApp::scrollCallback(
	GLFWwindow* window,
	double xoffset,
	double yoffset
) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));

	if (app) {
		app->input.onScroll(xoffset, yoffset);
	}
}

ToyFrameBuffer VulkanApp::AcquireFramebuffer()
{
	vkWaitForFences(
		core.device_,
		1,
		&swapchain->GetInFlightFence(),
		VK_TRUE,
		UINT64_MAX
	);

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(
		core.device_,
		swapchain->swapchain_,
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

	ToyFrameBuffer frameBufferContext_{
		.frameIndex = swapchain->currentFrame_,
		.commandBuffer = commandBuffers[swapchain->currentFrame_],
		.swapchain = *swapchain

	};
	return frameBufferContext_;
}

void VulkanApp::EndAndPresentFramebuffer(ToyFrameBuffer& fb)
{
	VkImageMemoryBarrier barrierToPresent{};
	barrierToPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrierToPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrierToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrierToPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrierToPresent.dstAccessMask = 0;
	barrierToPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrierToPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrierToPresent.image = swapchain->GetCurrentImage();
	barrierToPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrierToPresent.subresourceRange.baseMipLevel = 0;
	barrierToPresent.subresourceRange.levelCount = 1;
	barrierToPresent.subresourceRange.baseArrayLayer = 0;
	barrierToPresent.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(
		fb.commandBuffer,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrierToPresent
	);

	if (vkEndCommandBuffer(fb.commandBuffer) != VK_SUCCESS) {
		throw std::runtime_error("failed to record command buffer!");
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore waitSemaphores[] = { swapchain->imageAvailableSemaphores_[swapchain->currentFrame_] };
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
			core.graphicsQueue_,
			1,
			&submitInfo,
			swapchain->GetInFlightFence()
		) != VK_SUCCESS
		) {
		assert(false && "failed to submit draw");
		throw std::runtime_error("failed to submit draw command buffer!");
	}

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;

	VkSwapchainKHR swapChains[] = { swapchain->swapchain_ };
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &swapchain->currentImageIndex_;

	VkResult result = vkQueuePresentKHR(core.presentQueue_, &presentInfo);

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
		framebufferResized) {
		framebufferResized = false;
		swapchain->Recreate();
	}
	else if (result != VK_SUCCESS) {
		throw std::runtime_error("failed to present swap chain image!");
	}

	swapchain->AdvanceFrame();
}
