
#pragma once

#include <cassert>
#include <chrono>
#include <filesystem>

#include "volk.h"
#include "vk_mem_alloc.h"
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include <Engine/Renderer/context.h>
#include <Engine/Renderer/ringBinding.h>

#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>
#include <Engine/platform.h>

constexpr int NUM_CASCADES = 3;
constexpr VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

// ------------------------------------
// If theres only one I probably could go with argv
std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf" // Index 0
};

CameraPositioner_FirstPerson cameraPositioner(
	glm::vec3(0.0f, 1.0f, 0.0f), //pos
	glm::vec3(0.0f, 1.0f, 1.0f), //target
	glm::vec3(0.0f, 1.0f, 0.0f) //up
);

struct FrameResources {
	DescriptorSetHandle frameDescriptorSet;
	DescriptorSetHandle frameDynamicDescriptorSet;
};

class VulkanApp {

public:

	RingBinding<FrameUniforms, 1> frameUniformsBinding;
	RingBinding<DrawSetData, MAX_DRAWS> drawDataBinding;
	RingBinding<Light, MAX_LIGHTS> frameLigthsBinding;
	//PipelineHandle default_material_pipeline;
	//PipelineHandle default_shadow_pipeline;
	//std::array<ShadowMapSlot, SHADOWMAP_SLOTS> shadowmaps;



	// These match swapchainImageDimensions.
	int width_ = WIDTH;
	int height_ = HEIGHT;

	bool Initialize();
	bool InitializeAppData();
	bool Run() {


		bool result = Initialize();
		assert(result && "Failed to initialize");

		Start();
		MainLoop();
		CleanUp();

		return true;

	};
	void Start();
	void LoadScene();


private:



	Context* context;
	ImGuiContext* imgui_ctx;
	Scene scene_ = {}; // empty scene initialization

	// App still has ownership over the present queue for the current moment.
	VkQueue presentQueue_;

#pragma region ===[ App / Core State ]===

	float delta_time_s;
	float time_since_start_s;

	// This is bad. We coupled Camera/View with app... So we cant do multipanel/multiview.
	Camera camera_ = Camera(cameraPositioner);

#pragma endregion

#pragma region ===[ Swapchain ]===

	VkSwapchainKHR swapChain;
	uint32_t swapchainImageCount;
	VkImage* swapChainImages;
	VkImageView* swapChainImageViews;

	VkFormat swapChainImageFormat;
	VkExtent2D swapChainExtent;

#pragma endregion

	VkImage main_depthImage;
	VkImageView main_depthImageView;

	ImageHandle mainViewDepthImage;
	ImageViewHandle mainViewDepthImageView;
	VmaAllocation main_d_alloc;

#pragma region ===[ GLFW / Window State ]===
	GLFWwindow * window;
#pragma endregion

#pragma region ===[ Vulkan Instance / Device / Queues ]===
	VkInstance instance;
	VkDebugUtilsMessengerEXT debugMessenger;
	VkSurfaceKHR surface;

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
#pragma endregion

#pragma region ===[ Uniforms / Descriptor Sets ]==

	FrameResources perFrameData[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

#pragma region ===[ Pipeline / Render Resources ]===

	PipelineHandle main_view_pipeline_h;
	VkCommandPool perFrameCommandPools[MAX_FRAMES_IN_FLIGHT];
	VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

#pragma region ===[ Synchronization Primitives ]===

	VkSemaphore * renderFinishedSemaphores; // this is swapchainImageCountSize. that count cant be known without a query thats why here lies a pointer.
	VkSemaphore imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT];
	VkFence inFlightFences[MAX_FRAMES_IN_FLIGHT];

	uint32_t currentFrame = 0;
#pragma endregion

#pragma region ===[ Input ]===
	bool rightMousePressed = false;
	bool firstMouse;
	float lastX, lastY;
	int currentKey = -1; // Keeps track of the currently pressed key (W, A, S, D)
#pragma endregion

#pragma region ===[ Flags / Events ]===
	bool framebufferResized = false;
#pragma endregion

#pragma region Initialization

	void InitVulkanApp();
	void InitializeImGui();
	void CreateRenderSyncObjects();

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
	void UpdateFrameUniforms(uint32_t currentImage);
	void UpdateDrawDataBuffers(uint32_t currentImage);
	//void UpdateLightsBuffers(uint32_t currentImage);

	void UiPass() {

		// These all 3 are needed for initialization.
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();


		// UI Here
		ImGuiIO& io = ImGui::GetIO();
		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
			1000.0f / io.Framerate, io.Framerate);

		ImDrawList* drawList = ImGui::GetForegroundDrawList();


		// DEBUG DRAW POINTS OVER LIGHTS:
		//glm::mat4 view = camera_.getViewMatrix();
		//glm::mat4 proj = camera_.getProjMatrix();
		//proj[1][1] *= -1.0f;  // match your main pass

		//int node = 0;
		//for (auto light : lights) {

		//	if (light.intensity <= 0.0f)
		//		continue;

		//	glm::vec3 worldPos(
		//		light.position[0],
		//		light.position[1],
		//		light.position[2]
		//	);

		//	glm::vec4 clip = proj * view * glm::vec4(worldPos, 1.0f);
		//	if (clip.w <= 0.0f)
		//		continue;

		//	glm::vec3 ndc = glm::vec3(clip) / clip.w; // [-1,1]

		//	if (ndc.x < -1.0f || ndc.x > 1.0f ||
		//		ndc.y < -1.0f || ndc.y > 1.0f ||
		//		ndc.z < 0.0f || ndc.z > 1.0f)
		//		continue;

		//	// Map NDC -> ImGui screen space (origin top-left)
		//	const float sx = (ndc.x * 0.5f + 0.5f) * io.DisplaySize.x;
		//	const float sy = (ndc.y * 0.5f + 0.5f) * io.DisplaySize.y;  // <- no extra flip

		//	float r = light.color[0];
		//	float g = light.color[1];
		//	float b = light.color[2];
		//	if (r == 0.0f && g == 0.0f && b == 0.0f) {
		//		r = 1.0f; g = 1.0f; b = 0.0f;
		//	}

		//	ImU32 col = IM_COL32(
		//		(int)(glm::clamp(r, 0.0f, 1.0f) * 255.0f),
		//		(int)(glm::clamp(g, 0.0f, 1.0f) * 255.0f),
		//		(int)(glm::clamp(b, 0.0f, 1.0f) * 255.0f),
		//		255
		//	);

		//	const float radius = 6.0f;
		//	drawList->AddCircleFilled(ImVec2(sx, sy), radius, col, 12);

		//	// Optional label
		//	drawList->AddText(ImVec2(sx + radius + 2.0f, sy - radius),
		//		IM_COL32(255, 255, 255, 255),
		//		std::to_string(node).c_str());
		//	node++;
		//}

	}

#pragma region MainLoop

	void MainLoop() {

		while (!glfwWindowShouldClose(window)) {
			glfwPollEvents();

			// Event loop
			CameraUpdate();
			EarlyUpdate();
			Update(); // Main
			LateUpdate();
			DrawFrame();

		}

		// This holds. cpu. until queue completion. presentations is done by presetnQueue
		vkDeviceWaitIdle(device);
	}

#pragma endregion

#pragma region Events

	void CameraUpdate() {
		double x, y;
		glfwGetCursorPos(window, &x, &y);

		cameraPositioner.update(delta_time_s,
			glm::vec2(x / width_, y / -height_),
			rightMousePressed);

	}

	void EarlyUpdate() {
		recalculateGlobalTransforms(scene_);
	}

	void Update() {

		int i = 3;
		//float phase = std::sin(i + time_since_start_s * 0.35f);
		//scene_.localTransform[1][3][1] = phase *10;
		//markAsChanged(scene_, 1);

		//// fake animation
		//int i = 0;
		//for (auto light : lights) {

		//	if (light.type != LIGHT_TYPE_DIRECTIONAL) {
		//		float phase = std::sin(i + time_since_start_s * 0.35f);  // staggered phases
		//		light.position[2] = static_cast<float>(i) * 1 + phase;
		//		i++;
		//	}

		//}

	}

	void LateUpdate() {
	}

#pragma endregion

	void SetupDebugMessenger() {
		if (!enableValidationLayers) return;

		VkDebugUtilsMessengerCreateInfoEXT createInfo;
		populateDebugMessengerCreateInfo(createInfo);

		if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr,
			&debugMessenger) != VK_SUCCESS) {
			throw std::runtime_error("failed to set up debug messenger!");
		}
	}

	void RecreateSwapChain() {

		int width = 0, height = 0;
		glfwGetFramebufferSize(window, &width, &height);
		while (width == 0 || height == 0) {
			glfwGetFramebufferSize(window, &width, &height);
			glfwWaitEvents();
		}

		width_ = width;
		height_ = height;

		vkDeviceWaitIdle(device);

		CleanUpSwapChain();
		CreateSwapChain();
		CreateSwapchainImageViews();

		CreateDepthResources();

		// ---- update camera projection to match new size ----
		float newAspect = float(width) / float(height);
		camera_.setPerspective(45.0f, newAspect, default_near_clip, default_far_clip);
	}

	void CreateSwapChain() {

		SwapChainSupportDetails swapChainSupport =
			querySwapChainSupport(physicalDevice, surface);
		VkSurfaceFormatKHR surfaceFormat =
			chooseSwapSurfaceFormat(swapChainSupport.formats);

		//VkPresentModeKHR presentMode =
		//	chooseSwapPresentMode(swapChainSupport.presentModes);

		VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities, window);

		if (swapChainSupport.capabilities.maxImageCount > 0 &&
			swapchainImageCount > swapChainSupport.capabilities.maxImageCount) {
			swapchainImageCount = swapChainSupport.capabilities.maxImageCount;
		}
		swapChainImages = new VkImage[swapchainImageCount];
		swapChainImageViews = new VkImageView[swapchainImageCount];

		VkSwapchainCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = surface;

		createInfo.minImageCount = swapchainImageCount;
		createInfo.imageFormat = surfaceFormat.format;
		createInfo.imageColorSpace = surfaceFormat.colorSpace;
		createInfo.imageExtent = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

		QueueFamilyIndices indices = findQueueFamilies(physicalDevice, surface);
		uint32_t queueFamilyIndices[] = { indices.graphicsFamily.value(),
										 indices.presentFamily.value() };

		if (indices.graphicsFamily != indices.presentFamily) {
			createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			createInfo.queueFamilyIndexCount = 2;
			createInfo.pQueueFamilyIndices = queueFamilyIndices;
		}
		else {
			createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}

		createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.presentMode = presentMode;
		createInfo.clipped = VK_TRUE;

		VkResult r = vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain);
		if (r != VK_SUCCESS) {
			std::cerr << "vkCreateSwapchainKHR failed: " << r << "\n";
			throw std::runtime_error("failed to create swap chain!");
		}

		vkGetSwapchainImagesKHR(device, swapChain, &swapchainImageCount, nullptr);
		vkGetSwapchainImagesKHR(device, swapChain, &swapchainImageCount, swapChainImages);

		swapChainImageFormat = surfaceFormat.format;
		swapChainExtent = extent;
	}

	void CreateSwapchainImageViews() {

		for (uint32_t i = 0; i < swapchainImageCount; ++i) {

			VkImageView view_;

			createImageView(
				swapChainImages[i], swapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT, view_, 1
			);

			swapChainImageViews[i] = view_;
		}

	}

	void CreateDepthResources() {
		VkFormat depthFormat = findDepthFormat(physicalDevice);

		//mainViewDepthImage = context->CreateImage(
		//	swapChainExtent.width,
		//	swapChainExtent.height,
		//	depthFormat,
		//	VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		//	0, //levels == mips. mainView depth should be 0 levels. 
		//	1
		//); // TILING IS ASSUMED OPTIMAL

		//mainViewDepthImageView = context->CreateImageView(
		//	mainViewDepthImage,
		//	VK_IMAGE_ASPECT_DEPTH_BIT,
		//	0,
		//	0,
		//	1, 
		//	VK_IMAGE_VIEW_TYPE_2D
		//);

		createImage(
			swapChainExtent.width,
			swapChainExtent.height,
			depthFormat,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			main_depthImage
		);

		createImageView(main_depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, main_depthImageView, 1);

		context->TransitionImageLayout(main_depthImage, depthFormat,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	}

	// Cleanup
	void CleanUpSwapChain() {

		vkDestroyImageView(device, main_depthImageView, nullptr);
		vmaDestroyImage(*context->allocator_, main_depthImage, main_d_alloc);

		for (uint32_t i = 0; i < swapchainImageCount; ++i) {
			vkDestroyImageView(device, swapChainImageViews[i], nullptr);
		}
		delete[] swapChainImageViews;

		delete[] swapChainImages;
		swapChainImageViews = nullptr;

		vkDestroySwapchainKHR(device, swapChain, nullptr);
	}

	void CleanUp();

	void createImageView(
		VkImage image,
		VkFormat format,
		VkImageAspectFlags aspectFlags,
		VkImageView& imageView,
		int levelCount
	) {
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		// viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.aspectMask = aspectFlags;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = levelCount;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;

		if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create texture image view!");
		}

	}
	// Obly used now for depthImage. Im gonnad need to add supprot for this on context.... for shadowmpas.
	void createImage(
		uint32_t width,
		uint32_t height,
		VkFormat format,
		VkImageTiling tiling,
		VkImageUsageFlags usage,
		VkMemoryPropertyFlags properties,
		VkImage& image
	) {

		VkImageCreateInfo imgCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imgCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imgCreateInfo.extent.width = width;
		imgCreateInfo.extent.height = height;
		imgCreateInfo.extent.depth = 1;
		imgCreateInfo.mipLevels = 1;
		imgCreateInfo.arrayLayers = 1;
		imgCreateInfo.format = format;
		imgCreateInfo.tiling = tiling;
		imgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imgCreateInfo.usage = usage;
		imgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		VmaAllocationCreateInfo allocCreateInfo = {};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		allocCreateInfo.priority = 1.0f;

		VmaAllocation alloc;
		vmaCreateImage(*context->allocator_, &imgCreateInfo, &allocCreateInfo, &image, &alloc, nullptr);
		//return alloc;
	}

	// --------------------------

	// These descriptors sets can be set and done.
	// Bindless textures image and samplers set cannot be
	void SetDescriptorSets()
	{
		// Dynamic
		for (size_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT; frameIndex++) {

			auto setHandle = perFrameData[frameIndex].frameDynamicDescriptorSet;
			auto setObj = context->descriptorSets_.get(setHandle);
			VkDescriptorSet vkSet = setObj->set;

			// FrameUniforms 
			auto frameUniformsBufferHandle = context->buffers_.get(frameUniformsBinding.buffer_h);
			VkDescriptorBufferInfo frameUniformsInfo{
				.buffer = frameUniformsBufferHandle->buffer,
				.offset = 0,
				.range = sizeof(FrameUniforms) // this is the size of the window accesed per bind
			};

			// Draw data
			auto drawsDataBufferHandle = context->buffers_.get(drawDataBinding.buffer_h);
			VkDescriptorBufferInfo drawBufferInfo{
				.buffer = drawsDataBufferHandle->buffer,
				.offset = 0,
				.range = sizeof(DrawSetData) * MAX_DRAWS
			};
			VkWriteDescriptorSet writes[2]{};

			writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet = vkSet;
			writes[0].dstBinding = 0; // UBO
			writes[0].dstArrayElement = 0;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			writes[0].descriptorCount = 1;
			writes[0].pBufferInfo = &frameUniformsInfo;

			writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[1].dstSet = vkSet;
			writes[1].dstBinding = 1; // SSBO
			writes[1].dstArrayElement = 0;
			writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
			writes[1].descriptorCount = 1;
			writes[1].pBufferInfo = &drawBufferInfo;

			vkUpdateDescriptorSets(device, 2 /*descriptorWriteCount*/, writes, 0, nullptr);

		}
	}
	// This could be done way more clever. Only updating changed textures.
	// This may cause problems over the frame boundary if texture list changes...
	void UpdateBindlessTexturesSets() {

		const uint32_t materialCount = static_cast<uint32_t>(context->materialInstances_.numObjects());

		// --- Build image infos (sampled images) ---
		std::vector<VkDescriptorImageInfo> imageInfos;
		std::vector<VkDescriptorImageInfo> samplerInfos;

		int count = (context->textures_.numObjects());
		imageInfos.reserve(count);
		samplerInfos.reserve(count);

		for (int i = 0; i < count; i++)
		{
			auto tex = context->textures_.objects_[i].obj_;

			VkDescriptorImageInfo ii{};
			ii.sampler = VK_NULL_HANDLE; // separate samplers -> leave null
			ii.imageView = tex.imageView;
			ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfos.push_back(ii);

			VkDescriptorImageInfo si{};
			si.sampler = tex.sampler;
			si.imageView = VK_NULL_HANDLE;            // not used for SAMPLER
			si.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED; // ignored for SAMPLER
			samplerInfos.push_back(si);
		};



		for (size_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT; frameIndex++) {
			//// --- Two writes: images at index 2, samplers at index 3 ---
			VkWriteDescriptorSet writes[2]{};

			auto setHandle = perFrameData[frameIndex].frameDescriptorSet;
			auto set = context->descriptorSets_.get(setHandle);

			writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet = set->set;
			writes[0].dstBinding = 2;
			writes[0].dstArrayElement = 0;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			writes[0].descriptorCount = imageInfos.size();
			writes[0].pImageInfo = imageInfos.data();

			writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[1].dstSet = set->set;
			writes[1].dstBinding = 3;
			writes[1].dstArrayElement = 0;
			writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			writes[1].descriptorCount = samplerInfos.size();   // or 1 if you use a single shared sampler
			writes[1].pImageInfo = samplerInfos.data();

			vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
		}

	}

	void BindVertex(VkCommandBuffer commandBuffer) {

		auto page = context->geometry_pages.get(context->current_geometry_page);

		VkBuffer bufs[] = {
			context->buffers_.get(page->vertexPosBuffer)->buffer,
			context->buffers_.get(page->vertexShadingBuffer)->buffer
		};
		VkDeviceSize offs[] = { 0, 0 };
		vkCmdBindVertexBuffers(commandBuffer, 0, 2, bufs, offs);

		vkCmdBindIndexBuffer(commandBuffer, context->buffers_.get(page->indexBuffer)->buffer, 0, VK_INDEX_TYPE_UINT32);
	}



#pragma region ===[ CommandRecording ]===

	void DrawFrame();
	void RecordMainViewCommmandsForFrame(
		VkCommandBuffer commandBuffer,
		uint32_t imageIndex
	);
	void RecordCascadesForDirectionalLight(
		DirectionalLight& light,
		VkCommandBuffer& commandBuffer,
		ShadowMap& shadowmap, 
		VkPipelineLayout& pipe_lyt
	);

	void RecordCommandsForFrameShadowMaps(VkCommandBuffer commandBuffer);

#pragma endregion

#pragma region ===[ InputHooks ]===

	static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
	static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);
	static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
	static void mouseCallback(GLFWwindow* window, double xpos, double ypos);
	static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

	void handleMouseScroll(double xOffset, double yOffset);
	void handleMouseMovement(double xpos, double ypos);
	void handleMouseButton(int button, int action, int mods);
	void handleKey(int key, int scancode, int action, int mods);

#pragma endregion


};
