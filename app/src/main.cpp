#pragma once

#include <cassert>

// ---- SETUP -----

// Volk. Glm
#include <Engine/Vulkan/vulkan_setup.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// This needed to be defined elsewhere so al cop units got em the same, as they affect data structures.
//#define IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
//#define IMGUI_IMPL_VULKAN_USE_VOLK
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include <vk_mem_alloc.h>

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

// We are using ktx for textures. also I dont want tinygltf to load texture data, just scene data.
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_EXTERNAL_IMAGE

// --------------ENGINE--------------------

#include <Engine/Vulkan/vulkan_helpers.h>
#include <Engine/Vulkan/vulkan_funs.h>

#include <Engine/Renderer/constants.h>
#include <Engine/Renderer/context.h>

#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>

// Include glm/else.
#include <Engine/Shared/common.h>

#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>

#include <Engine/Renderer/primitives.h>

#include <Engine/Utils/gltf.h>

// Moved readFile here. readFile rnow is just used to read shader files.
// this should go to Platform layer. This is windows first for the foreseable future
#include <Engine/fileIO.h>

// --------------APP--------------------

#include <App/utils.h>

// ------------------------------------
// If theres only one I probably could go with argv
std::vector<std::string> gltf_meshes_paths = {
	"models/Sponza/sponza.gltf" // Index 0
};

Scene getSceneFromGltf(const GltfModel& gltf_model) {

	Scene scene;

	// adding top parent to apply scale here

	// top parent
	int root = addNode(scene, -1, 0);

	int meshIndex = 0;
	for (auto& mesh_data : gltf_model.mesh_data) {
		int node = addNode(scene, root, 1); // child of root
		scene.meshForNode[node] = meshIndex;

		// TODO: set real material index from mesh_data
		scene.materialForNode[node] = 0;

		meshIndex++;
	}

	scene.localTransform[0] = glm::scale(glm::mat4(1.0f), glm::vec3(0.008f)/*, glm::vec3(0.008f)*/);
	markAsChanged(scene, 0);
	return scene;
}

struct RGB { float r, g, b; };

RGB hsv2rgb(float h, float s, float v)
{
	float c = v * s;
	float x = c * (1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
	float m = v - c;

	float r, g, b;
	if (h < 1.0f / 6.0f) { r = c; g = x; b = 0; }
	else if (h < 2.0f / 6.0f) { r = x; g = c; b = 0; }
	else if (h < 3.0f / 6.0f) { r = 0; g = c; b = x; }
	else if (h < 4.0f / 6.0f) { r = 0; g = x; b = c; }
	else if (h < 5.0f / 6.0f) { r = x; g = 0; b = c; }
	else { r = c; g = 0; b = x; }

	return { r + m, g + m, b + m };
}

void FabricateLightData(Light* data) {

	//Light* data = new Light[MAX_LIGHTS];

	for (int i = 0; i < MAX_LIGHTS; i++) {
		data[i].position[0] = 3.0f * i;
		data[i].position[1] = 1.0f;
		data[i].position[2] = 0.0f;

		data[i].direction[0] = .0f;
		data[i].direction[1] = .0f;
		data[i].direction[2] = .0f;

		float hue = float(i) / float(MAX_LIGHTS);
		RGB rgb = hsv2rgb(hue, 1.0f, 1.0f);

		data[i].color = { rgb.r, rgb.g, rgb.b, 1.0f };
		data[i].intensity = 1.0f;
		data[i].type = LIGHT_TYPE_POINT;
		if (i % 2 == 1) {
			data[i].type = LIGHT_TYPE_SPOT;
			data[i].position[1] = 3.0f;
			data[i].position[0] = 2.0f;
			data[i].direction[3] = 3.0f;
		}
	}

}


class VulkanApp {

public:

	int width_ = WIDTH;
	int height_ = HEIGHT;

	bool Initialize();
	bool Run() {

		bool result = Initialize();
		assert(result && "Failed to initialize");
		Start();
		MainLoop();
		CleanUp();

		return true;

	};
	void Start();


	// Initial ShadowMap implementation.

	//void InitializeShadowMaps() {

	//	for (int i = 0; i < MAX_LIGHTS; i++) {
	//		auto l = lights[i];

	//		if (l.type == LIGHT_TYPE_SPOT) {
	//			if (l.shadow_map_index == -1) {

	//				context->CreateCascadedShadowMapsDepthImage(400, 400, 1);


	//			}
	//		}

	//	}

	//}
	void RenderShadows();
	Light* lights = new Light[MAX_LIGHTS];
	std::unordered_map<uint32_t, ShadowMapHandle> light_to_shadowmap;

private:

	Context* context;
	ImGuiContext* imgui_ctx;

	Scene scene_ = getSceneFromGltf(gltf_meshes_paths[0]);

	DescriptorSetHandle frameDescriptorSets[MAX_FRAMES_IN_FLIGHT];

	// Presnt still on app side. moved the rest to cotnext
	VkQueue presentQueue_;
	//VkQueue graphicsQueue_;
	//VkQueue computeQueue_;
	//VkQueue transferQueue_;

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

	// Main depth image is the one for the main view.
	VkImage main_depthImage;
	VkDeviceMemory main_depthImageMemory;
	VkImageView main_depthImageView;
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

	DeviceBufferHandle uniforms_[MAX_FRAMES_IN_FLIGHT];
	DeviceBufferHandle instanceDataStorages_[MAX_FRAMES_IN_FLIGHT];
	DeviceBufferHandle instanceIndexUniforms_[MAX_FRAMES_IN_FLIGHT];
	DeviceBufferHandle lightDataUniformBuffers[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

#pragma region ===[ Pipeline / Render Resources ]===
	// We separate command pools for each frames becuase its cheaper to reset them this way

	VkCommandPool framesCommandPool[MAX_FRAMES_IN_FLIGHT];
	VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

#pragma region ===[ Synchronization Primitives ]===

	VkSemaphore * renderFinishedSemaphores;
	VkSemaphore imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT];
	VkFence inFlightFences[MAX_FRAMES_IN_FLIGHT];

	uint32_t currentFrame = 0;
#pragma endregion

#pragma region ===[ Input / Camera State ]===
	bool rightMousePressed = false;
	bool firstMouse;
	float lastX, lastY;
	int currentKey = -1; // Keeps track of the currently pressed key (W, A, S, D)
#pragma endregion

#pragma region ===[ Flags / Resize Events ]===
	bool framebufferResized = false;
#pragma endregion


	void InitVulkanApp();
	void InitializeImGui();

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

	void UploadScene();

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



	}

	// Thsese are really helpers. not critical.
	void CreateDefaultMaterialPipeline();
	MaterialInstanceHandle CreateDiffuseMaterial(std::string image_path);


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

		//std::cout << camera_.getPosition() << std::endl;
		////Synthetic movement
		//for (size_t i = 0; i < scene_.localTransform.size(); ++i) {
		//	glm::mat4& local = scene_.localTransform[i];
		//	float phase = std::sin(i + delta_time_ * 0.35f);  // staggered phases
		//	local[3][0] = static_cast<float>(i) * 10.4f + phase;
		//	markAsChanged(scene_, i);
		//}
	}

	void LateUpdate() {

		recalculateGlobalTransforms(scene_);

	}

#pragma endregion

	void recreateSwapChain() {

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
		createSwapChain();
		createSwapchainImageViews();
		CreateDepthResources();

		// ---- update camera projection to match new size ----
		float newAspect = float(width) / float(height);
		camera_.setPerspective(45.0f, newAspect, default_near_clip, default_far_clip);
	}

	void setupDebugMessenger() {
		if (!enableValidationLayers) return;

		VkDebugUtilsMessengerCreateInfoEXT createInfo;
		populateDebugMessengerCreateInfo(createInfo);

		if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr,
			&debugMessenger) != VK_SUCCESS) {
			throw std::runtime_error("failed to set up debug messenger!");
		}
	}

	void createSwapChain() {

		SwapChainSupportDetails swapChainSupport =
			querySwapChainSupport(physicalDevice, surface);
		VkSurfaceFormatKHR surfaceFormat =
			chooseSwapSurfaceFormat(swapChainSupport.formats);

		//VkPresentModeKHR presentMode =
		//	chooseSwapPresentMode(swapChainSupport.presentModes);
		VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

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

	void createSwapchainImageViews() {

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
		context->TransitionImageLayout(main_depthImage, depthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

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
	// --------------------------

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

	void CreateFrameDataBuffers() {

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

			instanceIndexUniforms_[i] = context->createBuffer(
				sizeof(IndexSlot) * MAX_INSTANCES,
				// Just using this ftm:
				VMA_MEMORY_USAGE_AUTO,
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			);
			instanceDataStorages_[i] = context->createBuffer(
				sizeof(InstanceData) * MAX_INSTANCES,
				VMA_MEMORY_USAGE_AUTO,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			);

			// Lights
			lightDataUniformBuffers[i] = context->createBuffer(
				sizeof(Light) * MAX_LIGHTS,
				VMA_MEMORY_USAGE_AUTO,
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			);
		}
	}

	void UpdateInstanceDescriptorSets(uint32_t currentFrame)
	{
		assert(currentFrame < MAX_FRAMES_IN_FLIGHT);

		auto ii_data = context->buffers_.get(instanceIndexUniforms_[currentFrame]);
		auto id_data = context->buffers_.get(instanceDataStorages_[currentFrame]);

		VkDescriptorBufferInfo instanceIndexBufferInfo{
			.buffer = ii_data->buffer,
			.offset = 0,
			.range = sizeof(IndexSlot) * MAX_INSTANCES
		};
		VkDescriptorBufferInfo instanceBufferInfo{
			.buffer = id_data->buffer,
			.offset = 0,
			.range = sizeof(InstanceData) * MAX_INSTANCES
		};

		auto setHandle = frameDescriptorSets[currentFrame];
		auto setObj = context->descriptrorSetPool_.get(setHandle);
		VkDescriptorSet vkSet = setObj->set;

		VkWriteDescriptorSet writes[3]{};

		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = vkSet;
		writes[0].dstBinding = 0; // UBO
		writes[0].dstArrayElement = 0;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[0].descriptorCount = 1;
		writes[0].pBufferInfo = &instanceIndexBufferInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = vkSet;
		writes[1].dstBinding = 1; // SSBO
		writes[1].dstArrayElement = 0;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].descriptorCount = 1;
		writes[1].pBufferInfo = &instanceBufferInfo;

		vkUpdateDescriptorSets(device, 2 /*descitprorWriteCount*/, writes, 0, nullptr);

	}

	void UpdateLightDescriptorSets(uint32_t currentFrame) {

		assert(currentFrame < MAX_FRAMES_IN_FLIGHT);

		auto light_data_bfd = context->buffers_.get(lightDataUniformBuffers[currentFrame]);
		VkDescriptorBufferInfo lightDataBufferRangeInfo{
			.buffer = light_data_bfd->buffer,
			.offset = 0,
			.range = sizeof(Light) * MAX_LIGHTS
		};

		auto setHandle = frameDescriptorSets[currentFrame];
		auto setObj = context->descriptrorSetPool_.get(setHandle);
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

	void CreateFrameCommandBuffers() {

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = framesCommandPool[currentFrame];
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = (uint32_t)MAX_FRAMES_IN_FLIGHT;

		if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to allocate command buffers!");
		}
	}

	// This could be done way more clever. 
	void UpdateBindlessTextures(uint32_t currentFrame) {

		const uint32_t materialCount = static_cast<uint32_t>(context->materialInstancesPool_.numObjects());

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

		auto setHandle = frameDescriptorSets[currentFrame];

		//for (auto setHandle : frameDescriptorSets)
		//{

		auto set = context->descriptrorSetPool_.get(setHandle);

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

		//}

	}

	void RecordCommandsForFrame(VkCommandBuffer commandBuffer, uint32_t imageIndex) {

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
			throw std::runtime_error("failed to begin recording command buffer!");
		}

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
		//depthAttachment.clearValue.depthStencil = { 1.0f, 0 };
		//Lets do reverse z
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

		auto vbh = context->buffers_.get(context->vertex_buffer);
		auto ibh = context->buffers_.get(context->index_buffer);

		VkBuffer vertexBuffers[] = { vbh->buffer };
		VkDeviceSize offsets[] = { 0 };

		vkCmdBindVertexBuffers(
			commandBuffer,
			0,
			1,
			vertexBuffers,
			offsets
		);
		vkCmdBindIndexBuffer(commandBuffer,
			ibh->buffer,
			0,
			VK_INDEX_TYPE_UINT32)
			;


		PushConstants pc{};
		pc.view = camera_.getViewMatrix();
		pc.proj = camera_.getProjMatrix();
		pc.proj[1][1] *= -1;

		for (auto mat_pipe_entry : context->pipelinesPool_.objects_) {

			vkCmdBindPipeline(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				mat_pipe_entry.obj_.pipeline
			);

			auto sets = context->descriptrorSetPool_.get(frameDescriptorSets[currentFrame]);
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

				auto draw_set_h = context->meshes_.getHandle(scene_.meshForNode[i]);
				auto draw_set = context->meshes_.get(draw_set_h);

				// Need to find why this doesn't work.
				//uint32_t material_index = scene_.materialForNode[i];
				//assert(material_index == draw_set->material_index);

				uint32_t material_index = draw_set->material_index;
				pc.material_instance_index = material_index;

				MaterialPushConstants pcm =
					context->materialInstancesPool_.objects_[material_index].obj_.values;

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

				vkCmdDrawIndexed(
					commandBuffer,
					draw_set->index_count, //indexCount
					1, //instanceCount
					draw_set->first_index, //firstIndex, like offset but its just instead on index size times, just index n
					draw_set->vert_offset, //offsetIntoVertexBuffer
					i // instanceId
				);
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


		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
			throw std::runtime_error("failed to record command buffer!");
		}

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

	// UI
	static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
	static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);
	static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
	static void mouseCallback(GLFWwindow* window, double xpos, double ypos);
	static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

	void handleMouseScroll(double xOffset, double yOffset);
	void handleMouseMovement(double xpos, double ypos);
	void handleMouseButton(int button, int action, int mods);
	void handleKey(int key, int scancode, int action, int mods);

	// utils
	//void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
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

	auto im_dp = context->descriptorSetPoolPool_.get(context->imgui_descriptor_pool);

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

void Initialize() {

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
	createSwapChain();
	renderFinishedSemaphores = new VkSemaphore[swapchainImageCount];
	createSwapchainImageViews();

	// Queues.
	QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice, surface);

	VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value(),
	};

	for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
		if (vkCreateCommandPool(device, &poolInfo, nullptr, &framesCommandPool[fn]) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create graphics command pool!");
		}
	}

	CreateFrameCommandBuffers();
	CreateRenderSyncObjects();
	CreateDepthResources();


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

inline void VulkanApp::DrawFrame() {

	vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE,
		UINT64_MAX);

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(
		device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame],
		VK_NULL_HANDLE, &imageIndex);


	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		recreateSwapChain();
		return;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	UpdateDeltaTime();

	// We should check for size and promote or demote from pushconstants -> ubo -> ssbo. Or not.
	// This method goes absolutley independent using iths on commandPool. We could maybe chain this to framesCommandPool[curretFrame]
	// As this is an upload to Gpu maybe transferque is better for this.
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
	vkResetCommandPool(device, framesCommandPool[currentFrame], 0);


#pragma region GPU_UPDATE


	UpdateLightsBuffers(currentFrame);
	UpdateInstanceDescriptorSets(currentFrame);
	UpdateLightDescriptorSets(currentFrame);
	UpdateBindlessTextures(currentFrame);

#pragma endregion

	UiPass();

	RecordCommandsForFrame(commandBuffers[currentFrame], imageIndex);

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
			recreateSwapChain();
		}
		else if (result != VK_SUCCESS) {
			throw std::runtime_error("failed to present swap chain image!");
		}


	}

	// Update current frame
	currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanApp::CreateDefaultMaterialPipeline() {

	auto vertShaderCode = readFile("shaders/example.vert.hlsl.spv");
	auto vertColorFragCode = readFile("shaders/vertex_color.frag.hlsl.spv");
	auto fragShaderCode = readFile("shaders/example.frag.hlsl.spv");

	//VkShaderModule vertShaderModule = createShaderModule(device, vertShaderCode);
	//VkShaderModule fragShaderModule = createShaderModule(device, fragShaderCode);
	//VkShaderModule vertexColorFrag = createShaderModule(device, vertColorFragCode);

	auto vertHandle = context->CreateShaderModule(
		vertShaderCode
	);
	auto fragHandle = context->CreateShaderModule(
		fragShaderCode
	);

	auto def_pipe = context->CreateMaterialPipeline(
		vertHandle,
		fragHandle,
		default_raster_settings,
		findDepthFormat(physicalDevice), // THIS IS REALLY BAD. I WANT FIXED DEPTH FORMAT OR EARLY OUT ON DEVICE CRTEATION. lifted form vulkan tutorial
		swapChainImageFormat
	);

}

MaterialInstanceHandle VulkanApp::CreateDiffuseMaterial(std::string image_path) {

	ktxTexture2* ktxTex = nullptr;
	KTX_error_code res = ktxTexture2_CreateFromNamedFile(
		image_path.c_str(),
		KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, // <<–– correct flags
		&ktxTex
	);

	if (res != KTX_SUCCESS) {
		// do error
		throw std::runtime_error("KTX load failed");
	}

	// if you want mips generated by the ktx library:
	// I just generated them offline ;=)
	//ktxTex->generateMipmaps = KTX_TRUE;
	auto texHandle = context->CreateTexture(*ktxTex);

	// free after upload
	ktxTexture2_Destroy(ktxTex);

	auto material_pipe_0 = context->pipelinesPool_.getHandle(0);
	assert(material_pipe_0.valid());

	auto mat_h = context->CreateMaterialInstance(material_pipe_0);
	context->materialInstancesPool_.get(mat_h)->values.texDiffuse = texHandle.index();
	return mat_h;

}

inline void VulkanApp::UpdateInstanceBuffers(uint32_t currentImage) {

	// Moved this to the HEAP. This will be state of the app instead of being fabricated here anyways.
	InstanceData* data = new InstanceData[MAX_INSTANCES];
	int i = 0;
	for (const glm::mat4 tr : scene_.globalTransform) {
		//for (const glm::mat4 tr : scene_.localTransform) {
		std::memcpy(
			data[i].transform,
			glm::value_ptr(tr),
			sizeof(float) * 16);

		data[i].color = { i * 0.20f,0.3f,-i * 0.7f,1.0f };

		i++;
	}

	std::array<uint16_t, MAX_INSTANCES> indexData = {};
	for (uint16_t index = 0; index < MAX_INSTANCES; index++) {
		indexData[index] = index;
	}

	std::vector<IndexSlot> indexSlotData(MAX_INSTANCES);
	for (size_t i = 0; i < MAX_INSTANCES; ++i) {
		indexSlotData[i].idx = static_cast<uint32_t>(indexData[i]); // zero-extend
	}

	VkResult result{};

	// This is bad, will stall. Frame update should use same command buffer for all update operations.
	// FrameGraph! FrameGraph!
	VkCommandBuffer cmdBuf = beginSingleTimeCommands(device, framesCommandPool[currentImage]);

	static_assert(sizeof(IndexSlot) * MAX_INSTANCES < (1 << 16),
		"Instance buffer too large for 64KB UBO limit");

	context->UploadDataToBuffer(
		instanceIndexUniforms_[currentImage],
		indexSlotData.data(),
		sizeof(IndexSlot) * MAX_INSTANCES,
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		// This could be simplified really to 
		VK_ACCESS_SHADER_READ_BIT
		//VK_ACCESS_UNIFORM_READ_BIT

	);

	context->UploadDataToBuffer(
		instanceDataStorages_[currentImage],
		data,
		sizeof(InstanceData) * MAX_INSTANCES,
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT
	);

	endSingleTimeCommands(device, cmdBuf, context->default_queue, framesCommandPool[currentImage]);
	delete[] data;
}

void VulkanApp::UpdateLightsBuffers(uint32_t currentImage) {

	VkCommandBuffer cmdBuf = beginSingleTimeCommands(device, framesCommandPool[currentImage]);

	static_assert(sizeof(Light) * MAX_LIGHTS < (1 << 16),
		"Light buffer too large for 64KB UBO limit");

	context->UploadDataToBuffer(
		lightDataUniformBuffers[currentImage],
		lights,
		sizeof(Light) * MAX_LIGHTS,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT
		//VK_ACCESS_UNIFORM_READ_BIT 

	);

	endSingleTimeCommands(device, cmdBuf, context->default_queue, framesCommandPool[currentImage]);
	//delete[] lights;

}

void VulkanApp::UploadScene() {

	std::vector<MeshData> data_to_upload;
	std::set<uint32_t> already_uploaded;

	const GltfModel loaded_gltf = GltfModel(gltf_meshes_paths[0]);
	for (auto mesh_data : loaded_gltf.mesh_data) {
		data_to_upload.push_back(std::move(mesh_data));
	}

	std::unordered_map<std::string, int> already_assigned_;
	int j = 0;

	for (const auto& mesh : loaded_gltf.gltfModel.meshes) {
		for (auto& primitive : mesh.primitives) {
			auto path = baseColorPathFor(loaded_gltf.gltfModel, primitive, "./_extracted_textures");
			if (path) {
				if (already_assigned_.contains(*path)) {
					data_to_upload[j].materialIndex = already_assigned_[*path];
				}
				else {
					// Create and use the real handle index
					MaterialInstanceHandle mat_h = CreateDiffuseMaterial("models/sponza/" + *path);

					const uint32_t idx = mat_h.index();  // <-- the pool index you actually use later
					already_assigned_[*path] = idx;
					data_to_upload[j].materialIndex = idx;  // <-- assign for the *first* occurrence
				}


				// Crude patch
				//scene_.materialForNode[j] = data_to_upload[j].materialIndex;

			}
			else {
				std::cout << "Primitive has no baseColor texture (or embedded not extracted).\n";
				// consider setting a default/fallback material index here
			}
			j++;
		}
	}

	auto upload_result = context->BatchUploadMeshData(data_to_upload);


	for (auto ds : upload_result) {
		context->meshes_.create(std::move(ds));
	}

}

void VulkanApp::CleanUp() {

	context->CleanUp();

}

void VulkanApp::Start() {

	FabricateLightData(lights);
	//for (int i = 0; i < MAX_LIGHTS; i++) {
	//	if (i == 0) {
	//		auto sm_h = context->CreateShadowMap();
	//		lights[i].shadow_map_index = sm_h.index();
	//	}
	//}

	CreateDefaultMaterialPipeline();
	// This is part of the framegraph really....
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		frameDescriptorSets[i] = context->createDescriptorSet(
			context->default_descriptorLayout
		);
	}

	// This is pipeline setup always required.
	CreateFrameDataBuffers();
	camera_.setPerspective(45.0f, float(width_) / float(height_), default_near_clip, default_far_clip);

	UploadScene();

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