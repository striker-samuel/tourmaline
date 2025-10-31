#pragma once
#define VMA_DEBUG_INITIALIZE_ALLOCATIONS 1
#define VMA_DEBUG_DETECT_CORRUPTION 1
#define VMA_ASSERT(expr) assert(expr)
# define ENGINE_NAME "VulkanToy"

#include <cassert>

// ---- SETUP -----

#include <Engine/Vulkan/vulkan_setup.h>

#define VMA_IMPLEMENTATION

#define VMA_VULKAN_VERSION 1003800
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

// ------------------------------------

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

// Moved readFile here. readFile rnow is just used to read shader files.
// this should go to Platform layer. This is windows first for the foreseable future
#include <Engine/fileIO.h>

// ------------------------------------
// If theres only one I probably could go with argv
std::vector<std::string> gltf_meshes_paths = {
	"models/Sponza/sponza.gltf" // Index 0
};

// ------------------------------------
// 
// Crap I will delete later
Scene createTestScene() {

	Scene scene;
	//rootNode
	//addNode(scene, -1, 0);

	// two children
	//addNode(*scene_, 0, 1);
	//addNode(*scene_, 0, 1);

	int counter = 0;
	GltfModel loaded_gltf = GltfModel(gltf_meshes_paths[0]);
	for (auto mesh_data : loaded_gltf.mesh_data) {
		addNode(scene, -1, 0);
		scene.meshForNode[counter] = counter;
		//scene.materialForNode[counter] = mesh_data.materialIndex;
		scene.materialForNode[counter] = 0;
		counter++;
	}

	return scene;
}

class VulkanApp {

public:

	int width_ = WIDTH;
	int height_ = HEIGHT;

	bool Run();

private:

	// This really crude. Just temp
	BufferHandle index_bfh;
	BufferHandle vertex_bfh;
	std::vector<DrawSet> static_draw_sets;

	Context* context;
	DescriptorSetHandle frameDescriptorSets[MAX_FRAMES_IN_FLIGHT];

	Scene scene_ = createTestScene();

	VmaAllocator app_allocator_;

	VkBuffer static_vertex_buffer_;
	VmaAllocation static_vertex_allocation_;

	VkBuffer static_index_buffer_;
	VmaAllocation static_index_allocation_;

	VkQueue graphicsQueue_;
	VkQueue computeQueue_;
	VkQueue presentQueue_;
	VkQueue transferQueue_;

#pragma region ===[ App / Core State ]===

	float delta_time_;

	// This is bad. We coupled View with app...
	// Although i like the single cameraView model for an app. Lets not complicate setup
	// I dont want to stablish layouts for multicamera view. then compose ui on top
	// Memory was already split to Context.
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

	// We need to move this to context;
#pragma region ===[ Uniforms / Descriptor Sets ]==

	// These should be moved to Context
	DeviceBufferDescriptionHandle uniforms_[MAX_FRAMES_IN_FLIGHT];
	DeviceBufferDescriptionHandle instanceDataStorages_[MAX_FRAMES_IN_FLIGHT];
	DeviceBufferDescriptionHandle instanceIndexUniforms_[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

#pragma region ===[ Pipeline / Render Resources ]===
	// We separate command pools for each frames becuase its cheaper to reset them this way

	VkCommandPool framesCommandPool[MAX_FRAMES_IN_FLIGHT];
	VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

	// Ok, yes. lets do sync on app code. No framegraph yet :)
#pragma region ===[ Synchronization Primitives ]===

	VkSemaphore * renderFinishedSemaphores;
	VkSemaphore imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT];
	VkFence inFlightFences[MAX_FRAMES_IN_FLIGHT];

	uint32_t currentFrame = 0;
#pragma endregion

#pragma region ===[ Input / Camera State ]===
	glm::vec3 last_camera_pos;
	bool rightMousePressed = false;
	bool firstMouse;
	float lastX, lastY;
	int currentKey = -1; // Keeps track of the currently pressed key (W, A, S, D)
#pragma endregion

#pragma region ===[ Flags / Resize Events ]===
	bool framebufferResized = false;
#pragma endregion

	MaterialInstanceHandle CreateDiffuseMaterial(std::string image_path);
	void InitVulkanApp();

	void CreateDefaultMaterialPipeline();

	void UploadScene();
	void CreateStaticGeoBuffers();

	std::vector<DrawSet> UploadStaticMeshData(std::vector<MeshData>& mesh_data);

	void UpdateDeltaTime() {

		// Its a bad idea to have this across the fucntion border. Latency sensitive.
		static auto startTime = std::chrono::high_resolution_clock::now();
		auto currentTime = std::chrono::high_resolution_clock::now();
		delta_time_ = std::chrono::duration<float, std::chrono::seconds::period>(
			currentTime - startTime)
			.count();
	}

	void UpdateInstanceBuffer(uint32_t currentImage);

	void mainLoop() {

		auto lastTime = std::chrono::high_resolution_clock::now();

		while (!glfwWindowShouldClose(window)) {
			glfwPollEvents();

			auto now = std::chrono::high_resolution_clock::now();
			float delta = std::chrono::duration<float>(now - lastTime).count();
			lastTime = now;

			double x, y;
			glfwGetCursorPos(window, &x, &y);

			cameraPositioner.update(delta,
				glm::vec2(x / width_, y / -height_),
				rightMousePressed);

			EarlyUpdate();
			Update();

			recalculateGlobalTransforms(scene_);
			drawFrame();

		}

		vkDeviceWaitIdle(device);
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

	void cleanupSwapChain() {

		vkDestroyImageView(device, main_depthImageView, nullptr);
		vmaDestroyImage(app_allocator_, main_depthImage, main_d_alloc);

		for (uint32_t i = 0; i < swapchainImageCount; ++i) {
			vkDestroyImageView(device, swapChainImageViews[i], nullptr);
		}
		delete[] swapChainImageViews;

		delete[] swapChainImages;
		swapChainImageViews = nullptr;

		vkDestroySwapchainKHR(device, swapChain, nullptr);
	}

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
		createDepthResources();

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
		VkPresentModeKHR presentMode =
			chooseSwapPresentMode(swapChainSupport.presentModes);
		VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities, window);

		swapchainImageCount = swapChainSupport.capabilities.minImageCount + 1;
		swapChainImages = new VkImage[swapchainImageCount];
		swapChainImageViews = new VkImageView[swapchainImageCount];

		if (swapChainSupport.capabilities.maxImageCount > 0 &&
			swapchainImageCount > swapChainSupport.capabilities.maxImageCount) {
			swapchainImageCount = swapChainSupport.capabilities.maxImageCount;
		}

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
				swapChainImages[i], swapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT, view_
			);

			swapChainImageViews[i] = view_;
		}

	}

	void createDepthResources() {
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

		createImageView(main_depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, main_depthImageView);

		transitionImageLayout(main_depthImage, depthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	}

	// --------------------------

	TextureHandle createTexture(std::string texture_path, VkFormat format) {
		int texWidth, texHeight, texChannels;

		stbi_uc* pixels = stbi_load(texture_path.c_str(), &texWidth, &texHeight,
			&texChannels, STBI_rgb_alpha);


		if (!pixels) {
			throw std::runtime_error("failed to load texture image!");
		}

		// Lets for texChannels to 4 for now.
		texChannels = 4;
		auto texHandle = context->createTexture(texWidth, texHeight, texChannels, format, VK_IMAGE_ASPECT_COLOR_BIT, pixels);

		return texHandle;

	}

	//void createTextureImageView(
	//	const VkImage* textureImage,
	//	VkImageView* textureImageView, VkFormat format) {
	//	createImageView(
	//		*textureImage,
	//		format,
	//		//VK_FORMAT_R8G8B8A8_SRGB,
	//		VK_IMAGE_ASPECT_COLOR_BIT,
	//		*textureImageView
	//	);
	//}

	void createImageView(
		VkImage image,
		VkFormat format,
		VkImageAspectFlags aspectFlags,
		VkImageView& imageView
	) {
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		// viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.aspectMask = aspectFlags;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;

		if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create texture image view!");
		}

	}
	// Obly used now for depthImage.
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
		vmaCreateImage(app_allocator_, &imgCreateInfo, &allocCreateInfo, &image, &alloc, nullptr);
		//return alloc;
	}


	void transitionImageLayout(
		VkImage image,
		VkFormat format,
		VkImageLayout oldLayout,
		VkImageLayout newLayout
	) {
		VkCommandBuffer commandBuffer = beginSingleTimeCommands(device, context->default_command_pool);

		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

			if (hasStencilComponent(format)) {
				barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			}
		}
		else {
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		}

		VkPipelineStageFlags sourceStage;
		VkPipelineStageFlags destinationStage;

		if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
			newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
			newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
			newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

			sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		}
		else {
			throw std::invalid_argument("unsupported layout transition!");
		}

		vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
			nullptr, 0, nullptr, 1, &barrier);

		endSingleTimeCommands(device, commandBuffer, graphicsQueue_, context->default_command_pool);
	}

	void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
		uint32_t height) {
		VkCommandBuffer commandBuffer = beginSingleTimeCommands(device, context->default_command_pool);

		VkBufferImageCopy region{};
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = { 0, 0, 0 };
		region.imageExtent = { width, height, 1 };

		vkCmdCopyBufferToImage(commandBuffer, buffer, image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		endSingleTimeCommands(device, commandBuffer, graphicsQueue_, context->default_command_pool);
	}

	void createInstanceDataBuffers() {

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
		}
	}

	void UploadDefaultDescriptorSets()  // no params; uses frameDescriptorSets[]
	{
		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {


			auto ii_data = context->buffers_.get(instanceIndexUniforms_[i]);
			auto id_data = context->buffers_.get(instanceDataStorages_[i]);

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

			auto setHandle = frameDescriptorSets[i];
			auto setObj = context->descriptrorSetPool_.get(setHandle);
			VkDescriptorSet vkSet = setObj->set;

			VkWriteDescriptorSet writes[2]{};

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

			vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
		}
	}

	void createBuffer(
		VkDeviceSize size,
		VkBufferUsageFlags usage,
		VkMemoryPropertyFlags properties,
		VkBuffer& buffer,
		VkDeviceMemory& bufferMemory
	) {
		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
			throw std::runtime_error("failed to create buffer!");
		}

		VkMemoryRequirements memRequirements;
		vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

		VkMemoryAllocateInfo allocInfo{};

		// This should be moved to VMA
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memRequirements.size;
		allocInfo.memoryTypeIndex =
			findMemoryType(
				physicalDevice,
				memRequirements.memoryTypeBits,
				properties
			);


		if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to allocate buffer memory!");
		}

		vkBindBufferMemory(device, buffer, bufferMemory, 0);
	}

	void createCommandBuffers() {

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

	//void createCommandBuffers() {
	//	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
	//		VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	//		ai.commandPool = framesCommandPool[i];
	//		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	//		ai.commandBufferCount = 1;
	//		VK_CHECK(vkAllocateCommandBuffers(device, &ai, &commandBuffers[i]));
	//	}
	//}

	void UpdateBindlessTextures() {

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

		for (auto setHandle : frameDescriptorSets)
		{

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

		}

	}

	void recordDraw(VkCommandBuffer commandBuffer, uint32_t imageIndex) {

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

		// Begin dynamic rendering
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

		VkBuffer vertexBuffers[] = { static_vertex_buffer_ };
		VkDeviceSize offsets[] = { 0 };

		vkCmdBindVertexBuffers(
			commandBuffer,
			0,
			1,
			vertexBuffers,
			offsets
		);
		vkCmdBindIndexBuffer(commandBuffer,
			static_index_buffer_,
			0,
			VK_INDEX_TYPE_UINT32)
			;

		// I need to batch per materialpipe
		for (auto mat_pipe_entry : context->materialPipelinePool_.objects_) {

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

			PushConstants pc{};
			pc.view = camera_.getViewMatrix();
			pc.proj = camera_.getProjMatrix();
			pc.proj[1][1] *= -1;

			for (int i = 0; i < scene_.hierarchy.size(); i++) {

				//auto draw_set = static_draw_sets[scene_.meshForNode[i]];
				const auto& draw_set = static_draw_sets[i];

				//auto material_instance_index = scene_.materialForNode[i];
				//if (!context->materialInstancesPool_.getHandle(material_instance_index).valid()) {
				//	material_instance_index = 0;
				//}

				//int material_instance_index = ;
				pc.material_instance_index = draw_set.material_index;

				MaterialPushConstants pcm =
					context->materialInstancesPool_.objects_[draw_set.material_index].obj_.values;

				struct PC_All {
					PushConstants pc;               // 144
					MaterialPushConstants mpc;      // 48
				};
				PC_All all{ pc, pcm };

				vkCmdPushConstants(
					commandBuffer,
					pipe_lyt,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					0,
					sizeof(PC_All), &all);

				vkCmdDrawIndexed(
					commandBuffer,
					draw_set.index_count, //indexCount
					1, //instanceCount
					draw_set.first_index, //firstIndex
					draw_set.vert_offset,
					//0,
					i
				);
			}
		}

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

	void createSyncObjects() {

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
				//vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
				vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
				throw std::runtime_error(
					"failed to create synchronization objects for a frame!");
			}
		}
	}

	// ---------------------------------------------------------------------------

	void drawFrame();

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
	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

	// TEMP
	std::vector<MaterialInstanceHandle> material_instance_handles;
	std::vector<MaterialPipelineHandle> material_pipe_handles;

};

void VulkanApp::CreateDefaultMaterialPipeline() {

	auto vertShaderCode = readFile("shaders/example.vert.hlsl.spv");
	auto vertColorFragCode = readFile("shaders/vertex_color.frag.hlsl.spv");
	auto fragShaderCode = readFile("shaders/example.frag.hlsl.spv");

	//VkShaderModule vertShaderModule = createShaderModule(device, vertShaderCode);
	//VkShaderModule fragShaderModule = createShaderModule(device, fragShaderCode);
	//VkShaderModule vertexColorFrag = createShaderModule(device, vertColorFragCode);

	auto vertHandle = context->createShaderModule(
		vertShaderCode
	);
	auto fragHandle = context->createShaderModule(
		fragShaderCode
	);

	auto def_pipe = context->createMaterialPipeline(
		vertHandle,
		fragHandle,
		default_raster_settings,
		findDepthFormat(physicalDevice), // THIS IS REALLY BAD. I WANT FIXED DEPTH FORMAT OR EARLY OUT ON DEVICE CRTEATION. lifted form vulkan tutorial
		swapChainImageFormat
	);

	material_pipe_handles.push_back(def_pipe);
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
			}
			else {
				std::cout << "Primitive has no baseColor texture (or embedded not extracted).\n";
				// consider setting a default/fallback material index here
			}
			j++;
		}
	}

	static_draw_sets = UploadStaticMeshData(data_to_upload);

}

MaterialInstanceHandle VulkanApp::CreateDiffuseMaterial(std::string image_path) {

	auto image1 = createTexture(
		image_path,
		//"textures/viking_room.png",
		VK_FORMAT_R8G8B8A8_SRGB
	);
	
	auto material_pipe_0 = context->materialPipelinePool_.getHandle(0);
	auto mat_h = context->createMaterialInstance(material_pipe_0);
	context->materialInstancesPool_.get(mat_h)->values.texDiffuse = image1.index();
	return mat_h;

}

void VulkanApp::CreateStaticGeoBuffers() {

	// Lets just default this like so
	uint32_t totalVertexBytes = 512u * 1024u * 1024u;
	uint32_t totalIndexBytes = 512u * 1024u * 1024u;


	auto vbh = context->createBuffer(
		totalVertexBytes,
		VMA_MEMORY_USAGE_GPU_ONLY,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
	);

	auto ibh = context->createBuffer(
		totalIndexBytes,
		VMA_MEMORY_USAGE_GPU_ONLY,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
	);


	static_vertex_buffer_ = context->buffers_.get(vbh)->buffer;
	static_index_buffer_ = context->buffers_.get(ibh)->buffer;

}

std::vector<DrawSet> VulkanApp::UploadStaticMeshData(std::vector<MeshData>& mesh_data) {
	assert(!mesh_data.empty());

	VkDeviceSize needed_vertexBytes_for_staging = 0;
	VkDeviceSize needed_indexBytes_for_staging = 0;

	std::vector<DrawSet> mesh_draw_list(mesh_data.size()); // keep 1:1 with mesh_data

	for (size_t i = 0; i < mesh_data.size(); ++i) {
		needed_vertexBytes_for_staging += sizeof(Vertex) * mesh_data[i].vertices.size();
		needed_indexBytes_for_staging += sizeof(uint32_t) * mesh_data[i].indices.size();
	}

	// VERTICES
	{
		VkBuffer stagingBuf; VmaAllocation stagingAlloc;
		VkBufferCreateInfo sInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = needed_vertexBytes_for_staging,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		};
		VmaAllocationCreateInfo sAlloc{};
		sAlloc.usage = VMA_MEMORY_USAGE_AUTO;
		sAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

		vmaCreateBuffer(app_allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr);

		VkDeviceSize currBytes = 0;
		uint32_t currVerts = 0;
		for (size_t i = 0; i < mesh_data.size(); ++i) {
			const auto& m = mesh_data[i];
			const VkDeviceSize bytes = sizeof(Vertex) * m.vertices.size();

			mesh_draw_list[i].vert_offset = currVerts;           // BYTES
			if (bytes) {
				vmaCopyMemoryToAllocation(app_allocator_, m.vertices.data(), stagingAlloc, currBytes, bytes);
				currBytes += bytes;
				currVerts += static_cast<uint32_t>(m.vertices.size());
			}
		}

		copyBuffer(stagingBuf, static_vertex_buffer_, needed_vertexBytes_for_staging);
		vmaDestroyBuffer(app_allocator_, stagingBuf, stagingAlloc);
	}

	// INDICES
	{
		VkBuffer stagingBuf; VmaAllocation stagingAlloc;
		VkBufferCreateInfo sInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = needed_indexBytes_for_staging,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		};
		VmaAllocationCreateInfo sAlloc{};
		sAlloc.usage = VMA_MEMORY_USAGE_AUTO;
		sAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

		vmaCreateBuffer(app_allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr);

		VkDeviceSize currBytes = 0; // byte offset
		for (size_t i = 0; i < mesh_data.size(); ++i) {
			const auto& m = mesh_data[i];
			const VkDeviceSize bytes = sizeof(uint32_t) * m.indices.size();

			mesh_draw_list[i].index_count = static_cast<uint32_t>(m.indices.size());
			mesh_draw_list[i].first_index = static_cast<uint32_t>(currBytes / sizeof(uint32_t)); // ELEMENTS


			// MAT!
			mesh_draw_list[i].material_index = m.materialIndex;

			if (bytes) {
				vmaCopyMemoryToAllocation(app_allocator_, m.indices.data(), stagingAlloc, currBytes, bytes);
				currBytes += bytes;
			}
		}

		copyBuffer(stagingBuf, static_index_buffer_, needed_indexBytes_for_staging);
		vmaDestroyBuffer(app_allocator_, stagingBuf, stagingAlloc);
	}

	return mesh_draw_list;
}

inline void VulkanApp::UpdateInstanceBuffer(uint32_t currentImage) {

	// THIS MUST REMAIN HERE. SENSITIVE TO LAG OTHERWISE
	static auto startTime = std::chrono::high_resolution_clock::now();
	auto currentTime = std::chrono::high_resolution_clock::now();
	delta_time_ = std::chrono::duration<float, std::chrono::seconds::period>(
		currentTime - startTime)
		.count();

	// Moved this to the HEAP. This will be state of the app instead of being fabricated here anyways.
	InstanceData* data = new InstanceData[MAX_INSTANCES];
	int i = 0;
	//for (const glm::mat4 tr : scene_->globalTransform) {
	for (const glm::mat4 tr : scene_.localTransform) {
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
	VkCommandBuffer cmdBuf = beginSingleTimeCommands(device, context->default_command_pool);

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

	endSingleTimeCommands(device, cmdBuf, graphicsQueue_, context->default_command_pool);
	delete[] data;
}

bool VulkanApp::Run() {

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

	createSwapChain();
	renderFinishedSemaphores = new VkSemaphore[swapchainImageCount];
	createSwapchainImageViews();

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

	// createDescriptorSets. Create menas allocating. no data is present there still
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		frameDescriptorSets[i] = context->createDescriptorSet(
			context->default_descriptorLayout
		);
	}

	camera_.setPerspective(45.0f, float(width_) / float(height_), default_near_clip, default_far_clip);

	createInstanceDataBuffers();
	createCommandBuffers();
	createSyncObjects();
	createDepthResources();

	CreateDefaultMaterialPipeline();
	CreateStaticGeoBuffers();
	UploadScene();
	UpdateBindlessTextures();

	mainLoop();

	//cleanup();
	vmaDestroyAllocator(app_allocator_);

	return true;

};

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

		VkPhysicalDeviceFeatures deviceFeatures{
			.samplerAnisotropy = VK_TRUE
		};
		deviceFeatures.samplerAnisotropy = VK_TRUE;

		VkDeviceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

		createInfo.queueCreateInfoCount =
			static_cast<uint32_t>(queueCreateInfos.size());
		createInfo.pQueueCreateInfos = queueCreateInfos.data();

		createInfo.pEnabledFeatures = &deviceFeatures;

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
		createInfo.pNext = &vulkan13Features;


		if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create logical device!");
		}

		volkLoadDevice(device);

		vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue_);
		vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue_);
	}

	VmaAllocatorCreateInfo allocatorCreateInfo = {};
	allocatorCreateInfo.physicalDevice = physicalDevice;
	allocatorCreateInfo.device = device;
	allocatorCreateInfo.instance = instance;
	allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
	allocatorCreateInfo.flags =
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;

	// This next flag is to get handles. To interporate resources between instances, or even between Vulkan and Dx or things like that.
	// VMA_ALLOCATOR_CREATE_KHR_EXTERNAL_MEMORY_WIN32_BIT

	VmaVulkanFunctions vulkanFunctions;
	VkResult res = vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vulkanFunctions);
	allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;

	res = vmaCreateAllocator(&allocatorCreateInfo, &app_allocator_);
	assert(res == VK_SUCCESS);
	context = new Context(&app_allocator_, &device, &physicalDevice, surface); // remember to delete it on shutdown
}

void VulkanApp::mouseCallback(GLFWwindow* window, double xpos, double ypos) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleMouseMovement(xpos, ypos);
}

void VulkanApp::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleKey(key, scancode, action, mods);
}

inline void VulkanApp::drawFrame() {
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

	// BATCH PER MESH, SHADER, INSTANCE

	// We should check for size and promote or demote from pushconstants -> ubo -> ssbo.
	UpdateDeltaTime();
	UpdateInstanceBuffer(currentFrame);

	vkResetFences(device, 1, &inFlightFences[currentFrame]);
	vkResetCommandPool(device, framesCommandPool[currentFrame], 0);

	//vkResetCommandBuffer(commandBuffers[currentFrame],
	//	/*VkCommandBufferResetFlagBits*/ 0);

	UploadDefaultDescriptorSets();
	UpdateBindlessTextures();

	recordDraw(commandBuffers[currentFrame], imageIndex);

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
			graphicsQueue_,
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

	currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
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
	if (!rightMousePressed) return;

	glm::vec2 mousePos = glm::vec2(
		static_cast<float>(xpos) / width_,
		static_cast<float>(ypos) / -height_
	);
	cameraPositioner.update(delta_time_ / 1000,
		mousePos,
		true
	);
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

void VulkanApp::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	VkCommandBuffer commandBuffer = beginSingleTimeCommands(device, context->default_command_pool);

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	endSingleTimeCommands(device, commandBuffer, graphicsQueue_, context->default_command_pool);
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
