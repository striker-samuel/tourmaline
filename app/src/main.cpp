#pragma once

#include <cassert>

// ---- SETUP -----

#include <Engine/Vulkan/vulkan_setup.h>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include <vk_mem_alloc.h>
#include <tiny_gltf.h>

// --------------ENGINE--------------------

#include <Engine/Vulkan/vulkan_helpers.h>
#include <Engine/Vulkan/vulkan_funs.h>

#include <Engine/Renderer/constants.h>
#include <Engine/Renderer/context.h>

#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>

#include <Engine/Shared/common.h>
#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>
#include <Engine/Renderer/primitives.h>

#include <Engine/Utils/gltf.h>
#include <Engine/Formats/optimize.h>

#include <Engine/Formats/pak.h>
#include <Engine/platform.h>

// --------------APP--------------------

constexpr int NUM_CASCADES = 3;
constexpr VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // Th

#include <App/TestData/test_data_generators.h>

// ------------------------------------
// If theres only one I probably could go with argv
std::vector<std::string> gltf_meshes_paths = {
	"models/Sponza/sponza.gltf" // Index 0
};

CameraPositioner_FirstPerson cameraPositioner(
	glm::vec3(0.0f, 1.0f, 0.0f), //pos
	glm::vec3(0.0f, 1.0f, 1.0f), //target
	glm::vec3(0.0f, 1.0f, 0.0f) //up
);


struct FrameData{
	DescriptorSetHandle frameDescriptorSet;
	DescriptorSetHandle frameShadowDescriptorSet;

	DeviceBufferHandle frameInstancesDataBuffer;
	DeviceBufferHandle frameLightsDataBuffer;
};


class VulkanApp {

public:

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

	PipelineHandle default_material_pipeline;
	PipelineHandle default_shadow_pipeline;

	std::array<ShadowMapSlot, SHADOWMAP_SLOTS> shadowmaps;

	void IntializeShadowMaps() {
		for (int i = 0; i < SHADOWMAP_SLOTS; i++) {
			for (int j = 0; j < MAX_FRAMES_IN_FLIGHT; j++)
			{
				shadowmaps[i].perFrame[j] = context->CreateShadowMap();
			}
		}
	}

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
	VkDevice device;
#pragma endregion

#pragma region ===[ Uniforms / Descriptor Sets ]==

	FrameData perFrameData[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

#pragma region ===[ Pipeline / Render Resources ]===

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
	void UpdateInstanceBuffers(uint32_t currentImage);
	void UpdateLightsBuffers(uint32_t currentImage);

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

		//int n = 0;
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
		//		std::to_string(n).c_str());
		//	n++;
		//}

	}


#pragma region MainLoop

	void MainLoop() {

		while (!glfwWindowShouldClose(window)) {
			glfwPollEvents();

			CameraUpdate();
			EarlyUpdate();
			Update();
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

	}

	void Update() {

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

		recalculateGlobalTransforms(scene_);

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

		cleanupSwapChain();
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

		if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) !=
			VK_SUCCESS) {
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
	void cleanupSwapChain() {

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

	void CreateFrameDataBuffers() {

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

			perFrameData[i].frameInstancesDataBuffer = context->createBuffer(
				sizeof(InstanceData) * MAX_INSTANCES,
				VMA_MEMORY_USAGE_AUTO,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			);

			perFrameData[i].frameLightsDataBuffer = context->createBuffer(
				sizeof(Light) * MAX_LIGHTS,
				VMA_MEMORY_USAGE_AUTO,
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			);
		}
	}

	void UpdateMainViewFrameData(uint32_t currentFrame)
	{
		assert(currentFrame < MAX_FRAMES_IN_FLIGHT);

		auto instancesDataBufferHandle = context->buffers_.get(perFrameData[currentFrame].frameInstancesDataBuffer);
		VkDescriptorBufferInfo instanceBufferInfo{
			.buffer = instancesDataBufferHandle->buffer,
			.offset = 0,
			.range = sizeof(InstanceData) * MAX_INSTANCES
		};


		auto setHandle = perFrameData[currentFrame].frameDescriptorSet;
		auto setObj = context->descriptorSets_.get(setHandle);

		{
			VkDescriptorSet vkSet = setObj->set;

			VkWriteDescriptorSet writes[1]{};

			writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet = vkSet;
			writes[0].dstBinding = 1; // SSBO
			writes[0].dstArrayElement = 0;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[0].descriptorCount = 1;
			writes[0].pBufferInfo = &instanceBufferInfo;

			vkUpdateDescriptorSets(device, 1 /*descriptorWriteCount*/, writes, 0, nullptr);

		}
	}

	void UpdateMainViewShadowmapFrames(uint32_t currentFrame)
	{
		auto setHandle = perFrameData[currentFrame].frameDescriptorSet;
		auto setObj = context->descriptorSets_.get(setHandle);
		VkDescriptorSet vkSet = setObj->set;

		auto sam = context->samplers["default_shadow"];
		VkSampler shadowSampler = *context->samplers_.get(sam);

		std::array<VkDescriptorImageInfo, SHADOWMAP_SLOTS> imageInfos{};
		std::array<VkDescriptorImageInfo, SHADOWMAP_SLOTS> samplerInfos{};

		for (uint32_t slot = 0; slot < SHADOWMAP_SLOTS; ++slot)
		{
			ShadowMapHandle h = shadowmaps[slot].perFrame[currentFrame];
			ShadowMap* sm = context->shadowmaps_.get(h);
			VkImageView view = *context->imageViews_.get(sm->renderView);

			imageInfos[slot] = VkDescriptorImageInfo{
				.sampler = VK_NULL_HANDLE,
				.imageView = view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			};

			samplerInfos[slot] = VkDescriptorImageInfo{
					.sampler = shadowSampler,
					.imageView = VK_NULL_HANDLE,
					.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED
			};

		}

		VkWriteDescriptorSet writes[2]{};

		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = vkSet;
		writes[0].dstBinding = 5;
		writes[0].dstArrayElement = 0;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].descriptorCount = SHADOWMAP_SLOTS;
		writes[0].pImageInfo = imageInfos.data();


		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = vkSet;
		writes[1].dstBinding = 6; // binding index for shadow map combined sampler
		writes[1].dstArrayElement = 0;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		writes[1].descriptorCount = SHADOWMAP_SLOTS;
		writes[1].pImageInfo = samplerInfos.data();

		vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
	}

	void UpdateLightDescriptorSets(uint32_t currentFrame) {

		assert(currentFrame < MAX_FRAMES_IN_FLIGHT);

		auto light_data_bfd = context->buffers_.get(perFrameData->frameLightsDataBuffer);
		VkDescriptorBufferInfo lightDataBufferRangeInfo{
			.buffer = light_data_bfd->buffer,
			.offset = 0,
			.range = sizeof(Light) * MAX_LIGHTS
		};

		auto setHandle = perFrameData[currentFrame].frameDescriptorSet;
		auto setObj = context->descriptorSets_.get(setHandle);
		VkDescriptorSet vkSet = setObj->set;

		VkWriteDescriptorSet writes[1]{};

		// LIGHTS
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = vkSet;
		writes[0].dstBinding = 4;
		writes[0].dstArrayElement = 0;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[0].descriptorCount = 1;
		writes[0].pBufferInfo = &lightDataBufferRangeInfo;

		vkUpdateDescriptorSets(device, 1 /*descitprorWriteCount*/, writes, 0, nullptr);

	}

	// This could be done way more clever. Only updating changed textures.
	// This may cause problems over the frame boundary if texture list changes...
	void UpdateBindlessTextures(uint32_t currentFrame) {

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

		//// --- Two writes: images at index 2, samplers at index 3 ---
		VkWriteDescriptorSet writes[2]{};

		auto setHandle = perFrameData[currentFrame].frameDescriptorSet;
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
	
	// shadowmaps are per frame
	// This is a helper to get frame.slot shadowmap
	ShadowMap GetShadowMapFromShadowMapIndex(uint32_t shadowmap_index, uint32_t currentFrame) {

		assert(shadowmap_index >= 0);
		assert(shadowmap_index < SHADOWMAP_SLOTS);
		assert(currentFrame < MAX_FRAMES_IN_FLIGHT);

		auto shadowmap = context->shadowmaps_.get(shadowmaps[shadowmap_index].perFrame[currentFrame]);
		return *shadowmap;

	}

	void RecordCascadesForDirectionalLight(
		DirectionalLight& light,
		VkCommandBuffer& commandBuffer,
		ShadowMap& shadowmap,
		VkPipelineLayout& pipe_lyt
	) {


		auto image = context->images_.get(shadowmap.image);

		// ---- cascade split settings ----
		const float cameraNear = 0.1f;   // ideally: camera_.nearPlane()
		const float shadowFarMax = 20.0f;  // your old single-cascade far (tune this)
		const float lambda = 0.5f;   // 0=linear, 1=log (0.5 common)

		// ---- atlas tiling (2x2 for 4, 2x2 for <=4, 4x4 for 16, etc.) ----
		const int gridDim = (int)std::ceil(std::sqrt((float)NUM_CASCADES));
		const uint32_t tileW = image->width / gridDim;
		const uint32_t tileH = image->height / gridDim;

		// Precompute split ends in view-space depth
		float splitEnds[NUM_CASCADES]{};
		for (int c = 0; c < NUM_CASCADES; ++c) {
			const float p = float(c + 1) / float(NUM_CASCADES);

			const float logSplit = cameraNear * std::pow(shadowFarMax / cameraNear, p);
			const float linSplit = cameraNear + (shadowFarMax - cameraNear) * p;

			splitEnds[c] = linSplit + lambda * (logSplit - linSplit);
		}

		// Render each cascade
		float prevSplit = cameraNear;
		for (int c = 0; c < NUM_CASCADES; ++c) {

			const float cascadeNear = prevSplit;
			const float cascadeFar = splitEnds[c];
			prevSplit = cascadeFar;

			// --- atlas tile placement ---
			const int tx = c % gridDim;
			const int ty = c / gridDim;

			VkViewport cvp{};
			cvp.x = float(tx * tileW);
			cvp.y = float(ty * tileH);
			cvp.width = float(tileW);
			cvp.height = float(tileH);
			cvp.minDepth = 0.0f;
			cvp.maxDepth = 1.0f;
			vkCmdSetViewport(commandBuffer, 0, 1, &cvp);

			VkRect2D cscissor{};
			cscissor.offset = { int32_t(tx * tileW), int32_t(ty * tileH) };
			cscissor.extent = { tileW, tileH };
			vkCmdSetScissor(commandBuffer, 0, 1, &cscissor);

			PushConstants pc{};
			// --- compute per-cascade VP ---

			GetDirectionalLightOrthoVP(
				light,
				camera_.getViewMatrix(),
				camera_.getProjMatrix(),
				0.1f, 20.0f,
				pc.view, pc.proj,
				/*zPadding*/ 50.0f,
				/*snap*/ true,
				/*res*/ tileW
			);
			pc.proj[1][1] *= -1.0f;

			vkCmdPushConstants(
				commandBuffer,
				pipe_lyt,
				VK_SHADER_STAGE_VERTEX_BIT,
				0,
				sizeof(PushConstants),
				&pc
			);

			for (int n = 0; n < static_cast<int>(scene_.hierarchy.size()); ++n) {
				auto mesh_desc_handle = context->meshes_.getHandle(scene_.meshForNode[n]);
				auto mesh_desc = context->meshes_.get(mesh_desc_handle);

				for (auto draw_set : mesh_desc->sets) {
					vkCmdDrawIndexed(
						commandBuffer,
						draw_set.index_count,
						1,
						draw_set.first_index,
						draw_set.base_vertex,
						0
					);
				}
			}
		}
	}

	//void RecordCommandsForFrameShadowMaps(VkCommandBuffer commandBuffer)
	//{

	//	for (auto& spot : spotLights) {

	//		int smi = light.shadow_map_index;
	//		if (smi < 0) continue;
	//		if (smi >= SHADOWMAP_SLOTS) continue; // or assert
	//		auto sm = GetShadowMapFromShadowMapIndex(smi, currentFrame);

	//		auto shadowMapImage = context->images_.get(sm.image);
	//		VkImageView shadowMapRenderView = *context->imageViews_.get(sm.renderView);

	//		VkExtent2D shadowExtent = { shadowMapImage->width,shadowMapImage->height };           // e.g. { width, height }

	//		{
	//			VkImageMemoryBarrier barrier{};
	//			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	//			//				barrier.oldLayout = used_image ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	//			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // I do loadOp clear so this can be undefined no problem
	//			barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
	//			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	//			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	//			barrier.image = shadowMapImage->image;
	//			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	//			barrier.subresourceRange.baseMipLevel = 0;
	//			barrier.subresourceRange.levelCount = 1;
	//			barrier.subresourceRange.baseArrayLayer = 0;     // or layer if using array
	//			barrier.subresourceRange.layerCount = 1;

	//			barrier.srcAccessMask = 0;                       // nothing before
	//			barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	//			vkCmdPipelineBarrier(
	//				commandBuffer,
	//				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	//				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
	//				0,
	//				0, nullptr,
	//				0, nullptr,
	//				1, &barrier
	//			);
	//		}

	//		VkRenderingAttachmentInfoKHR depthAttachment{};
	//		depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
	//		depthAttachment.imageView = shadowMapRenderView;
	//		depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
	//		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	//		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // we want to sample this later

	//		VkClearValue clearDepth{};

	//		// Normal Z Depth, unlike primary view.
	//		clearDepth.depthStencil.depth = 1.0f;
	//		depthAttachment.clearValue = clearDepth;

	//		VkRenderingInfoKHR renderingInfo{};
	//		renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
	//		renderingInfo.renderArea.offset = { 0, 0 };
	//		renderingInfo.renderArea.extent = shadowExtent;           // shadow map size
	//		renderingInfo.layerCount = 1;
	//		renderingInfo.colorAttachmentCount = 0;                      // depth-only
	//		renderingInfo.pColorAttachments = nullptr;
	//		renderingInfo.pDepthAttachment = &depthAttachment;
	//		renderingInfo.pStencilAttachment = nullptr;

	//		vkCmdBeginRendering(commandBuffer, &renderingInfo);

	//		VkViewport vp{};
	//		vp.x = 0.0f;
	//		vp.y = 0.0f;
	//		vp.width = static_cast<float>(shadowExtent.width);
	//		vp.height = static_cast<float>(shadowExtent.height);
	//		vp.minDepth = 0.0f;
	//		vp.maxDepth = 1.0f;
	//		vkCmdSetViewport(commandBuffer, 0, 1, &vp);

	//		// We should resize scissor to avoid unnecesary pixelinvokations
	//		VkRect2D scissor{};
	//		scissor.offset = { 0, 0 };
	//		scissor.extent = shadowExtent;
	//		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

	//		BindVertex(commandBuffer);

	//		// Shadow Pipelien depthCompare is LESS_OR_EQUAL
	//		auto s_pipeline = context->pipelines_.get(default_shadow_pipeline);
	//		auto pipe_lyt = s_pipeline->plyt;

	//		vkCmdBindPipeline(
	//			commandBuffer,
	//			VK_PIPELINE_BIND_POINT_GRAPHICS,
	//			s_pipeline->pipeline
	//		);

	//		auto sets = context->descriptorSets_.get(shadowFrameDescriptorSets[currentFrame]);

	//		vkCmdBindDescriptorSets(
	//			commandBuffer,
	//			VK_PIPELINE_BIND_POINT_GRAPHICS,
	//			pipe_lyt,
	//			0,
	//			1,
	//			&sets->set,
	//			0,
	//			nullptr
	//		);

	//		if (light.type == LIGHT_TYPE_SPOT) {

	//			PushConstants pc{};
	//			pc.view = GetSpotLightView(light);
	//			pc.proj = GetPerspectiveProjectionForLight(light, /*aspect*/ 1.0f, /*near*/ 0.1f, /*far*/ 100.0f);
	//			pc.proj[1][1] *= -1;

	//			vkCmdPushConstants(
	//				commandBuffer,
	//				pipe_lyt,
	//				VK_SHADER_STAGE_VERTEX_BIT, // | VK_SHADER_STAGE_FRAGMENT_BIT if you want
	//				0,
	//				sizeof(PushConstants),
	//				&pc
	//			);

	//			for (int i = 0; i < static_cast<int>(scene_.hierarchy.size()); ++i) {
	//				auto mesh_desc_handle = context->meshes_.getHandle(scene_.meshForNode[i]);
	//				auto mesh_desc = context->meshes_.get(mesh_desc_handle);

	//				for (auto draw_set : mesh_desc->sets) {

	//					vkCmdDrawIndexed(
	//						commandBuffer,
	//						draw_set.index_count,
	//						1,
	//						draw_set.first_index,
	//						draw_set.base_vertex,
	//						0
	//					);

	//				}

	//			}



	//		}
	//		else {
	//			assert(false && "Unsuported lightype");
	//		}

	//		vkCmdEndRendering(commandBuffer);

	//		{
	//			VkImageMemoryBarrier barrier{};
	//			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	//			barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
	//			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	//			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	//			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	//			barrier.image = shadowMapImage->image;
	//			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	//			barrier.subresourceRange.baseMipLevel = 0;
	//			barrier.subresourceRange.levelCount = 1;
	//			barrier.subresourceRange.baseArrayLayer = 0;
	//			barrier.subresourceRange.layerCount = 1;

	//			barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	//			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	//			vkCmdPipelineBarrier(
	//				commandBuffer,
	//				VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	//				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	//				0,
	//				0, nullptr,
	//				0, nullptr,
	//				1, &barrier
	//			);

	//		}
	//	}


	//}

	void RecordMainViewCommmandsForFrame(VkCommandBuffer commandBuffer, uint32_t imageIndex) {

		VkImageMemoryBarrier barrierToColorAttachment{};
		barrierToColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrierToColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // or VK_IMAGE_LAYOUT_PRESENT_SRC_KHR if reusing
		barrierToColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrierToColorAttachment.srcAccessMask = 0;  // No access before
		barrierToColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrierToColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrierToColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrierToColorAttachment.image = swapChainImages[imageIndex];
		barrierToColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrierToColorAttachment.subresourceRange.baseMipLevel = 0;
		barrierToColorAttachment.subresourceRange.levelCount = 1;
		barrierToColorAttachment.subresourceRange.baseArrayLayer = 0;
		barrierToColorAttachment.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrierToColorAttachment
		);

		VkRenderingAttachmentInfoKHR depthAttachment{};
		depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
		depthAttachment.imageView = main_depthImageView;
		depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.clearValue.depthStencil = { 0.0f, 0 };

		VkRenderingAttachmentInfoKHR colorAttachment{};
		colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
		colorAttachment.imageView = swapChainImageViews[imageIndex];  // actual view
		colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.clearValue = { {0.0f, 0.0f, 0.0f, 1.0f} };

		VkRenderingInfoKHR renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
		renderingInfo.renderArea.offset = { 0, 0 };
		renderingInfo.renderArea.extent = swapChainExtent;
		renderingInfo.layerCount = 1;
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachments = &colorAttachment;
		renderingInfo.pDepthAttachment = &depthAttachment;

		vkCmdBeginRendering(commandBuffer, &renderingInfo);

		// VIEWPORT
		// Flipped because of this :)
		// https://www.saschawillems.de/blog/2019/03/29/flipping-the-vulkan-viewport/
		VkViewport vp{};
		vp.x = 0.0f;
		vp.y = 0.0f;
		vp.width = (float)swapChainExtent.width;
		vp.height = (float)swapChainExtent.height;
		vp.minDepth = 0.0f;
		vp.maxDepth = 1.0f;
		vkCmdSetViewport(commandBuffer, 0, 1, &vp);

		// SCISSOR.
		VkRect2D scissor{};
		scissor.offset = { 0, 0 };
		scissor.extent = swapChainExtent;
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

		// BIND
		BindVertex(commandBuffer);

		PushConstants pc{};
		pc.view = camera_.getViewMatrix();
		pc.proj = camera_.getProjMatrix();
		pc.proj[1][1] *= -1;

		// I long ago did this to be able to run trough different pipeline setuops like transparency and else. Amay be wrong and dumb
		for (auto mat_pipe_entry : context->pipelines_.objects_) {

			if (mat_pipe_entry.obj_.type != PIPELINE_MATERIAL) {
				continue;
			}

			vkCmdBindPipeline(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				mat_pipe_entry.obj_.pipeline
			);

			auto sets = context->descriptorSets_.get(perFrameData[currentFrame].frameDescriptorSet);
			auto pipe_lyt = mat_pipe_entry.obj_.plyt;

			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				pipe_lyt,
				0,
				1,
				&sets->set,
				0,
				nullptr);

			for (int i = 0; i < scene_.hierarchy.size(); i++) {

				auto mesh_desc = context->meshes_.get(
					context->meshes_.getHandle(
						scene_.meshForNode[i]
					)
				);

				auto material_instance_index = scene_.materialForNode[i];
				pc.material_instance_index = material_instance_index;

				MaterialPushConstants pcm =
					context->materialInstances_.objects_[material_instance_index].obj_.values;

				struct PC_All {
					PushConstants pc;               // 144
					MaterialPushConstants mpc;      // 48
				};
				PC_All all{ pc, pcm };

				vkCmdPushConstants(
					commandBuffer,
					pipe_lyt,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					0, //offset to start of the range to push
					sizeof(PC_All), // size to push
					&all
				);

				for (auto draw_set : mesh_desc->sets) {

					vkCmdDrawIndexed(
						commandBuffer,
						draw_set.index_count,
						1,
						draw_set.first_index,
						draw_set.base_vertex,
						0
					);

				}

			}
		}


		ImGui::Render();

		// Record the ImGui draw data into your Vulkan command buffer.
		// You must already be inside a render pass (or dynamic rendering block)
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);

		vkCmdEndRendering(commandBuffer);

		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = 0;  // Nothing needs to read it before present
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = swapChainImages[imageIndex];
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // after rendering finishes
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          // before presentation
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier
		);

	}

	void CreateRenderSyncObjects() {

		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

		for (uint32_t i = 0; i < swapchainImageCount; i++) {
			if (vkCreateSemaphore(device, &semInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS) {
				throw std::runtime_error("failed to create render-finished semaphore!");
			}
		}

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

			if (
				vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
				vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
				throw std::runtime_error(
					"failed to create synchronization objects for a frame!");
			}
		}
	}

	void DrawFrame();

	// UI EVENTS

	static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
	static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);
	static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
	static void mouseCallback(GLFWwindow* window, double xpos, double ypos);
	static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

	void handleMouseScroll(double xOffset, double yOffset);
	void handleMouseMovement(double xpos, double ypos);
	void handleMouseButton(int button, int action, int mods);
	void handleKey(int key, int scancode, int action, int mods);

};

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

	auto im_dp = context->descriptorSetPools_.get(context->imgui_descriptor_pool);

	SwapChainSupportDetails swapChainSupport =
		querySwapChainSupport(physicalDevice, surface);

	uint32_t desired = DESIRED_SWAPCHAING_IMAGE_COUNT; // triple buffering target
	uint32_t minReq = swapChainSupport.capabilities.minImageCount;
	uint32_t maxReq = swapChainSupport.capabilities.maxImageCount; // 0 means "no maximum"

	swapchainImageCount = std::max(minReq, desired);
	if (maxReq != 0) swapchainImageCount = std::min(swapchainImageCount, maxReq);


	// Provide information for dynamic rendering
	VkPipelineRenderingCreateInfo pipeline_create{
		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	pipeline_create.pNext = VK_NULL_HANDLE;

	// We are using the single color buffer out.
	pipeline_create.colorAttachmentCount = 1;
	pipeline_create.pColorAttachmentFormats = &swapChainImageFormat;

	pipeline_create.depthAttachmentFormat = findDepthFormat(physicalDevice);
	pipeline_create.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
	ImGui_ImplGlfw_InitForVulkan(window, /*install callbacks*/ false);

	ImGui_ImplVulkan_InitInfo info{};
	info.Instance = instance;
	info.ApiVersion = VK_API_VERSION_1_3;
	info.PhysicalDevice = physicalDevice;
	info.Device = device;
	info.Queue = context->default_queue;
	info.DescriptorPool = *im_dp;
	info.MinImageCount = minReq;
	info.ImageCount = swapchainImageCount;
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

void VulkanApp::InitVulkanApp() {

	{
		if (enableValidationLayers && !checkValidationLayerSupport()) {
			throw std::runtime_error(
				"validation layers requested, but not available!");
		}

		VkApplicationInfo appInfo{};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "VulkanToy";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "Sam2025Vulkan_one_three";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_3;

		VkInstanceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &appInfo;

		auto extensions = getRequiredExtensions();
		createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		createInfo.ppEnabledExtensionNames = extensions.data();

		if (enableValidationLayers) {
			createInfo.enabledLayerCount =
				static_cast<uint32_t>(validationLayers.size());
			createInfo.ppEnabledLayerNames = validationLayers.data();

			VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo;
			populateDebugMessengerCreateInfo(debugCreateInfo);
			createInfo.pNext = (void*)&debugCreateInfo;

		}
		else {
			createInfo.enabledLayerCount = 0;
			createInfo.pNext = nullptr;
		}

		if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
			throw std::runtime_error("failed to create instance!");
		}

		volkLoadInstance(instance);

	}

	if (enableValidationLayers) {

		VkDebugUtilsMessengerCreateInfoEXT createInfo;
		populateDebugMessengerCreateInfo(createInfo);

		if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr,
			&debugMessenger) != VK_SUCCESS) {
			throw std::runtime_error("failed to set up debug messenger!");
		}
	}


	// Create Surface
	if (glfwCreateWindowSurface(instance, window, nullptr, &surface) !=
		VK_SUCCESS) {
		throw std::runtime_error("failed to create window surface!");
	}

	// Pick Device
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

	if (deviceCount == 0) {
		throw std::runtime_error("failed to find GPUs with Vulkan support!");
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

	for (const auto& device : devices) {
		if (isDeviceSuitable(device, surface)) {
			physicalDevice = device;
			break;
		}
	}

	if (physicalDevice == VK_NULL_HANDLE) {
		throw std::runtime_error("failed to find a suitable GPU!");
	}

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);

#ifndef DEBUG

	std::cout << "----DEVICE INFO-----" << std::endl;

	std::cout << "	maxUniformBufferRange = "
		<< properties.limits.maxUniformBufferRange << " bytes\n";

	std::cout << "	minUniformBufferOffsetAlignment = "
		<< properties.limits.minUniformBufferOffsetAlignment << " bytes\n";

	std::cout << "	maxStorageBufferRange = "
		<< properties.limits.maxStorageBufferRange << " bytes\n\n";
#endif

	// Create Logical Device
	{
		QueueFamilyIndices indices = findQueueFamilies(physicalDevice, surface);

		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
		std::set<uint32_t> uniqueQueueFamilies = { indices.graphicsFamily.value(),
												  indices.presentFamily.value() };

		float queuePriority = 1.0f;
		for (uint32_t queueFamily : uniqueQueueFamilies) {
			VkDeviceQueueCreateInfo queueCreateInfo{};
			queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueCreateInfo.queueFamilyIndex = queueFamily;
			queueCreateInfo.queueCount = 1;
			queueCreateInfo.pQueuePriorities = &queuePriority;
			queueCreateInfos.push_back(queueCreateInfo);
		}


		// Descriptor indexing (core, not EXT)
		VkPhysicalDeviceDescriptorIndexingFeatures descIdx{};
		descIdx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
		// Enable what you actually need:
		descIdx.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		descIdx.descriptorBindingPartiallyBound = VK_TRUE;
		descIdx.runtimeDescriptorArray = VK_TRUE;
		descIdx.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

		// Maybe I dont want this one
		descIdx.descriptorBindingVariableDescriptorCount = VK_TRUE;

		VkPhysicalDeviceVulkan13Features vulkan13Features{};
		vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		vulkan13Features.dynamicRendering = VK_TRUE;
		vulkan13Features.pNext = &descIdx;

		// Core features (BC + anisotropy) go in Features2.features
		VkPhysicalDeviceFeatures2 feats2{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			.pNext = &vulkan13Features,
			.features = {
				.samplerAnisotropy = VK_TRUE,
				.textureCompressionBC = VK_TRUE, // REQUIRED for BC1..BC7
			},
		};

		VkDeviceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		createInfo.queueCreateInfoCount =
			static_cast<uint32_t>(queueCreateInfos.size());
		createInfo.pQueueCreateInfos = queueCreateInfos.data();

		// Using Features2
		createInfo.pEnabledFeatures = nullptr;
		createInfo.pNext = &feats2;

		createInfo.enabledExtensionCount =
			static_cast<uint32_t>(deviceExtensions.size());
		createInfo.ppEnabledExtensionNames = deviceExtensions.data();

		if (enableValidationLayers) {
			createInfo.enabledLayerCount =
				static_cast<uint32_t>(validationLayers.size());
			createInfo.ppEnabledLayerNames = validationLayers.data();
		}
		else {
			createInfo.enabledLayerCount = 0;
		}

		if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create logical device!");
		}

		volkLoadDevice(device);

		//vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue_);
		vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue_);

		// Assert for BC7
		VkFormatProperties p{};
		vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_BC7_SRGB_BLOCK, &p);
		bool sampleOK = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
		assert(sampleOK);

	}

	context = new Context(instance, &device, &physicalDevice, surface); // remember to delete it on shutdown
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

	window = glfwCreateWindow(width_, height_, "Vulkan", nullptr, nullptr);

	if (!window) {
		std::cerr << "Failed to create GLFW window\n";
		glfwTerminate();
		return false;
	}
	glfwMakeContextCurrent(window);
	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	glfwSetWindowTitle(window, ENGINE_NAME);
	glfwSetKeyCallback(window, VulkanApp::keyCallback);
	glfwSetCursorPosCallback(window, VulkanApp::mouseCallback);
	glfwSetMouseButtonCallback(window, VulkanApp::mouseButtonCallback);
	glfwSetScrollCallback(
		window, VulkanApp::scrollCallback);  // Register scroll callback

	glfwSetWindowUserPointer(window, this);
	glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

	// Pick physical device, create logical device, get queues from physdevice. Create VMA allocator
	InitVulkanApp();

	SwapChainSupportDetails swapChainSupport =
		querySwapChainSupport(physicalDevice, surface);
	VkSurfaceFormatKHR surfaceFormat =
		chooseSwapSurfaceFormat(swapChainSupport.formats);

	swapChainImageFormat = surfaceFormat.format;

	InitializeImGui();

	// SwapChain
	CreateSwapChain();
	renderFinishedSemaphores = new VkSemaphore[swapchainImageCount];
	CreateSwapchainImageViews();

	// Queues.
	QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice, surface);

	VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value(),
	};

	for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
		if (vkCreateCommandPool(device, &poolInfo, nullptr, &perFrameCommandPools[fn]) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create graphics command pool!");
		}
	}

	//CreateFrameCommandBuffers();

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = perFrameCommandPools[currentFrame];
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = (uint32_t)MAX_FRAMES_IN_FLIGHT;

	if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers) !=
		VK_SUCCESS) {
		throw std::runtime_error("failed to allocate command buffers!");
	}


	CreateRenderSyncObjects();
	CreateDepthResources();

	// 
	CreateFrameDataBuffers();

	// All ShadowMap Slots images are created here directly on startup ;)
	IntializeShadowMaps();

	return true;

}

bool VulkanApp::InitializeAppData() {
	//WritePak();
	//RegisterPaks();
	return true;

}

#pragma region ===[ Callbacks ]===

void VulkanApp::mouseCallback(GLFWwindow* window, double xpos, double ypos) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleMouseMovement(xpos, ypos);
}

void VulkanApp::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleKey(key, scancode, action, mods);
}

void VulkanApp::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleMouseButton(button, action, mods);
}

void VulkanApp::scrollCallback(GLFWwindow* window, double xOffset, double yOffset) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleMouseScroll(xOffset, yOffset);
}

void VulkanApp::handleMouseScroll(double xOffset, double yOffset) {
	const float scrollSpeed = 1.0f;
	glm::vec3 forward = cameraPositioner.getForward();
	cameraPositioner.setPosition(
		cameraPositioner.getPosition() + static_cast<float>(yOffset) * scrollSpeed * forward
	);
}

void VulkanApp::handleMouseMovement(double xpos, double ypos) {
	//if (!rightMousePressed) return;
	//glm::vec2 mousePos = glm::vec2(
	//	static_cast<float>(xpos) / width_,
	//	static_cast<float>(ypos) / -height_
	//);
	//cameraPositioner.update(delta_time_s,
	//	mousePos,
	//	true
	//);
}

void VulkanApp::handleMouseButton(int button, int action, int mods) {
	if (button == GLFW_MOUSE_BUTTON_RIGHT) {
		rightMousePressed = (action == GLFW_PRESS);
	}
}

void VulkanApp::handleKey(int key, int scancode, int action, int mods) {
	bool isPressed = action != GLFW_RELEASE;

	switch (key) {
	case GLFW_KEY_R:
	case GLFW_KEY_F:
		cameraPositioner.reset();
		break;
	case GLFW_KEY_W:
	case GLFW_KEY_UP:
		cameraPositioner.movement_.forward_ = isPressed;
		break;
	case GLFW_KEY_S:
	case GLFW_KEY_DOWN:
		cameraPositioner.movement_.backward_ = isPressed;
		break;
	case GLFW_KEY_A:
	case GLFW_KEY_LEFT:
		cameraPositioner.movement_.left_ = isPressed;
		break;
	case GLFW_KEY_D:
	case GLFW_KEY_RIGHT:
		cameraPositioner.movement_.right_ = isPressed;
		break;
	case GLFW_KEY_Q:
	case GLFW_KEY_SPACE:
		cameraPositioner.movement_.up_ = isPressed;
		break;
	case GLFW_KEY_E:
		cameraPositioner.movement_.down_ = isPressed;
		break;
	case GLFW_KEY_LEFT_SHIFT:
		cameraPositioner.movement_.fastSpeed_ = isPressed;
		break;
	default:
		break;
	}
}

void VulkanApp::framebufferResizeCallback(GLFWwindow* window, int width,
	int height) {
	auto app =
		reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	app->framebufferResized = true;
}

#pragma endregion

void VulkanApp::DrawFrame() {

	vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE,
		UINT64_MAX);

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(
		device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame],
		VK_NULL_HANDLE, &imageIndex);


	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		RecreateSwapChain();
		return;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	// This is here to be as close as posible.
	UpdateDeltaTime();

#pragma region GPU_UPDATE

	// Update Device state
	UpdateInstanceBuffers(currentFrame);

	// Moving this after the reset prompts this validation error. Why?
	//---------------- Vulkan Validation ----------------
	//Message: Validation Error: [ VUID-vkResetCommandPool-commandPool-00040 ] Object 0: handle = 0x1a3033b7f00,
	// type = VK_OBJECT_TYPE_COMMAND_BUFFER; Object 1: handle = 0x95a125000000001a, type = VK_OBJECT_TYPE_COMMAND_POOL;
	// | MessageID = 0xb53e2331 | vkResetCommandPool():  (VkCommandBuffer 0x1a3033b7f00[]) is in use.
	// The Vulkan spec states: All VkCommandBuffer objects allocated from commandPool must not be in the pending state (https://vulkan.lunarg.com/doc/view/1.3.283.0/windows/1.3-extensions/vkspec.html#VUID-vkResetCommandPool-commandPool-00040)

	vkResetFences(device, 1, &inFlightFences[currentFrame]);

	// Resetting entire pool is faster and better. I have split pools per frame so no biggie.
	//vkResetCommandBuffer(commandBuffers[currentFrame],
	//	/*VkCommandBufferResetFlagBits*/ 0);
	vkResetCommandPool(device, perFrameCommandPools[currentFrame], 0);



	// These are bandwitth heavy because im not tracking what needs updating in any way...
	UpdateLightsBuffers(currentFrame);
	//UpdateShadowMapDescriptorSets(currentFrame);
	UpdateMainViewFrameData(currentFrame);
	UpdateLightDescriptorSets(currentFrame);
	UpdateBindlessTextures(currentFrame);

#pragma endregion

	UiPass();

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	if (vkBeginCommandBuffer(commandBuffers[currentFrame], &beginInfo) != VK_SUCCESS) {
		throw std::runtime_error("failed to begin recording command buffer!");
	}

	//RecordCommandsForFrameShadowMaps(commandBuffers[currentFrame]);
	RecordMainViewCommmandsForFrame(commandBuffers[currentFrame], imageIndex);

	if (vkEndCommandBuffer(commandBuffers[currentFrame]) != VK_SUCCESS) {
		throw std::runtime_error("failed to record command buffer!");
	}


	// Submit Frame 
	{

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		VkSemaphore waitSemaphores[] = { imageAvailableSemaphores[currentFrame] };
		VkPipelineStageFlags waitStages[] = {
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;

		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

		VkSemaphore signalSemaphores[] = { renderFinishedSemaphores[currentFrame] };
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		if (
			vkQueueSubmit(
				context->default_queue,
				1,
				&submitInfo,
				inFlightFences[currentFrame]) != VK_SUCCESS
			) {
			throw std::runtime_error("failed to submit draw command buffer!");
		}


		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = { swapChain };
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &imageIndex;

		result = vkQueuePresentKHR(presentQueue_, &presentInfo);

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
			framebufferResized) {
			framebufferResized = false;
			RecreateSwapChain();
		}
		else if (result != VK_SUCCESS) {
			throw std::runtime_error("failed to present swap chain image!");
		}


	}

	// Update current frame
	currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanApp::UpdateInstanceBuffers(uint32_t currentImage) {

	InstanceData* data = new InstanceData[MAX_INSTANCES];
	int i = 0;

	const auto& src = scene_.globalTransform;
	for (size_t i = 0, n = src.size(); i < n; ++i) {
		std::memcpy(data[i].transform, glm::value_ptr(src[i]), sizeof(float) * 16);
		data[i].color = { float(i) * 0.20f, 0.3f, -float(i) * 0.7f, 1.0f };

	}

	VkResult result{};
	VkCommandBuffer cmdBuf = beginSingleTimeCommands(device, perFrameCommandPools[currentImage]);

	auto instance_data_buffer_handle = perFrameData[currentImage].frameInstancesDataBuffer;

	context->UploadDataToBuffer(
		instance_data_buffer_handle,
		data,
		sizeof(InstanceData) * MAX_INSTANCES,
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT
	);

	endSingleTimeCommands(device, cmdBuf, context->default_queue, perFrameCommandPools[currentImage]);
	delete[] data;
}

void VulkanApp::UpdateLightsBuffers(uint32_t currentImage) {

	VkCommandBuffer cmdBuf = beginSingleTimeCommands(device, perFrameCommandPools[currentImage]);

	//for (auto spot : spot_lights) {

	//	auto proj = GetPerspectiveProjectionForLight(lights[i]);
	//	proj[1][1] *= -1;
	//	lvp = proj * GetLightView(lights[i]);

	//	std::memcpy(
	//		lights[i].lightVP,
	//		glm::value_ptr(lvp),
	//		sizeof(float) * 16);
	//}

	//for (auto dirl : directional_lights)
	//{

	//	glm::mat4 lvp;

	//	if (lights[i].type == LIGHT_TYPE_SPOT) {
	//	}
	//	else if (lights[i].type == LIGHT_TYPE_DIRECTIONAL) {
	//		glm::mat4 proj, view;

	//		auto sm = GetShadowMapFromShadowMapIndex(lights[i].shadow_map_index, currentFrame);
	//		auto shadowMapImage = context->images_.get(sm.image);

	//		GetDirectionalLightOrthoVP(
	//			lights[i],
	//			camera_.getViewMatrix(),
	//			camera_.getProjMatrix(),
	//			0.1f, 20.0f,
	//			view, proj,
	//			/*zPadding*/ 50.0f,
	//			/*snap*/ true,
	//			/*res*/ shadowMapImage->width
	//		);

	//		proj[1][1] *= -1;
	//		lvp = proj * view;


	//context->UploadDataToBuffer(
	//	lightDataUniformBuffers[currentImage],
	//	lights.data(),
	//	sizeof(Light) * lights.size(),
	//	VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	//	VK_ACCESS_SHADER_READ_BIT
	//	//VK_ACCESS_UNIFORM_READ_BIT 

	//);

	endSingleTimeCommands(device, cmdBuf, context->default_queue, perFrameCommandPools[currentImage]);

}

void VulkanApp::CleanUp() {

	context->CleanUp();

}

void VulkanApp::Start() {

	auto vertShaderCode = readFile("shaders/default.vert.hlsl.spv");
	auto fragShaderCode = readFile("shaders/default.frag.hlsl.spv");

	auto vert_shader_h = context->CreateShaderModule(
		vertShaderCode
	);
	auto fragHandle = context->CreateShaderModule(
		fragShaderCode
	);

	// DEFAULT 

	default_material_pipeline = context->CreateMaterialPipeline(
		vert_shader_h,
		fragHandle,
		default_raster_settings,
		findDepthFormat(physicalDevice), // THIS IS REALLY BAD. I WANT FIXED DEPTH FORMAT OR EARLY OUT ON DEVICE CRTEATION. lifted form vulkan tutorial
		swapChainImageFormat
	);

	default_shadow_pipeline = context->CreateShadowPipeline(vert_shader_h);

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

		perFrameData[i].frameDescriptorSet = context->createDescriptorSet(
			context->default_descriptorLayout
		);

		//shadowFrameDescriptorSets[i] = context->createDescriptorSet(
		//	context->default_shadow_descriptorLayout
		//);
	}

	LoadScene();

}

void VulkanApp::LoadScene() {

	camera_.setPerspective(45.0f, float(width_) / float(height_), default_near_clip, default_far_clip);

	// This will upload 
	Scene imported_scene;
	std::filesystem::path p(gltf_meshes_paths[0]);
	UploadGltfToContext(context, p, imported_scene);

	scene_ = imported_scene;

}

int main(int argc, const char* argv[]) {
	if (argc > 1) {
		std::cout << argv[0];
	}

	VulkanApp app = VulkanApp();
	try {
		app.Run();
	}
	catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}


