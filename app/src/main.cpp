#pragma once
#define GLM_FORCE_RADIANS

# define ENGINE_NAME "VulkanToy"

#include <cassert>

// ---- SETUP -----

#include <Engine/Vulkan/vulkan_setup.h>

#define VMA_IMPLEMENTATION

#define VMA_VULKAN_VERSION 1004000 // Vulkan 1.4
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

// ------------------------------------

#include <Engine/Vulkan/vulkan_helpers.h>
//#include <Engine/Memory/pool.h>

#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>
#include <Engine/Renderer/primitives.h>
#include <Engine/Utils/gltf.h>
#include <Engine/Shared/common.h>

#include <Engine/Memory/context.h>

// Moved readFile here. readFile rnow is just used to read shader files
#include <Engine/fileIO.h>

Scene default_scene = Scene();

static constexpr uint32_t WIDTH = 1280;
static constexpr uint32_t HEIGHT = 720;

static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

static constexpr float near_clip = 0.01f;
static constexpr float far_clip = 100.0f;

// Yes, I'm hardcapping here for development.
static constexpr uint32_t MAX_INSTANCES = 1000;
static constexpr uint32_t MAX_MESHES = 1000;
static constexpr uint32_t MAX_TEXTURES = 4096;
static constexpr uint32_t MAX_MATERIAL_INSTANCES = 1000;
static constexpr uint32_t MAX_MATERIALS = 1000;


struct alignas(16) IndexSlot {
	uint32_t idx;      // put the 16-bit value here (zero-extend)
	uint32_t pad[3];   // 16 bytes total per element
};
static_assert(sizeof(IndexSlot) % 16 == 0);


struct PushConstants {
	glm::mat4 view;
	glm::mat4 proj;
	int material_index;
	uint32_t     _pad0[3];
};

// Initialized with crude defaults;
struct RasterSettings {
	uint32_t depthClampEnable = VK_FALSE;
	uint32_t rasterizerDiscardEnable = VK_FALSE;
	uint32_t polygonMode = VK_POLYGON_MODE_FILL;
	uint32_t lineWidth = 1.0f;
	uint32_t cullMode = VK_CULL_MODE_BACK_BIT;
	uint32_t frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	uint32_t depthBiasEnable = VK_FALSE;
};


//CPU Side struct
struct Material {

	VkShaderModule vert;
	VkShaderModule frag;
	RasterSettings raster_settings;
	uint32_t diffuse;

} default_material;


// Gpu Side
struct MaterialValues {
	glm::vec4 ambient = glm::vec4(0.0f);
	glm::vec4 diffuse = glm::vec4(0.0f);
	uint32_t texAmbient = 0;
	uint32_t texDiffuse = 0;
	uint32_t texAlpha = 0;
	uint32_t padding[1];
};

static_assert(sizeof(MaterialValues) % 16 == 0);



class Pool {

public:
	std::vector<Mesh> meshes_ = { Quad, Triangle };
	std::vector<MaterialDefinition> materials_{};
	std::vector<VkDescriptorSet> mat_descs_{};


	Pool() {
		meshes_.reserve(MAX_MESHES);
		materials_.reserve(MAX_MATERIALS);
		mat_descs_.reserve(MAX_MATERIALS);
	}

};

class VulkanApp {

public:

	int width_ = WIDTH;
	int height_ = HEIGHT;

	bool run();

private:

	Pool test_pool_;

	Scene* scene_ = &default_scene;

	InstanceData instance_data_[MAX_INSTANCES];
	uint32_t instance_count_ = 10;

	VmaAllocator app_allocator_;

	VkBuffer static_vertex_buffer_;
	VmaAllocation static_vertex_allocation_;

	VkBuffer static_index_buffer_;
	VmaAllocation static_index_allocation_;

	// I doubt this is really needed.
	VkFence f_level_ready_;

	// _---------------

	VkQueue graphicsQueue_;
	VkQueue computeQueue_;
	VkQueue presentQueue_;
	VkQueue transferQueue_;

#pragma region ===[ App / Core State ]===

	float delta_time_;

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

#pragma region ===[ Texture Resources ]===

	// Lets do arrays of MAX_TEXTURES.
	//VkImage* textureImages;
	//VkImageView* textureImageViews;
	//VkSampler* textureSamplers;

#pragma endregion

#pragma region ===[ Descriptors ]===

	VkDescriptorPool descriptorPool;
	VkDescriptorSet frameDescriptorSets[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

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

#pragma region ===[ Uniforms / Descriptor Sets ]===

	UniformBufferAllocation uniforms_[MAX_FRAMES_IN_FLIGHT];
	UniformBufferAllocation instanceDataUniforms_[MAX_FRAMES_IN_FLIGHT];
	UniformBufferAllocation instanceIndexUniforms_[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

#pragma region ===[ Pipeline / Render Resources ]===

	// These are not single objects! Should be made pools. 
	VkDescriptorSetLayout descriptorSetLayout;
	VkPipelineLayout pipelineLayout;
	VkPipeline graphicsPipeline;
	// *

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
	glm::vec3 last_camera_pos;
	bool rightMousePressed = false;
	bool firstMouse;
	float lastX, lastY;
	int currentKey = -1; // Keeps track of the currently pressed key (W, A, S, D)
#pragma endregion

#pragma region ===[ Flags / Resize Events ]===
	bool framebufferResized = false;
#pragma endregion

	void initVulkanApp();

	void UpdateDeltaTime() {

		// Its a bad idea to have this across the fucntion border
		static auto startTime = std::chrono::high_resolution_clock::now();
		auto currentTime = std::chrono::high_resolution_clock::now();
		delta_time_ = std::chrono::duration<float, std::chrono::seconds::period>(
			currentTime - startTime)
			.count();
	}

	void UpdateUniformBuffer(uint32_t currentImage) {
	}

	enum class BufferRole { UBO, SSBO };

	void UploadDataToBuffer(
		UniformBufferAllocation& ubo,
		void* data,
		size_t dataSize,
		VkCommandBuffer& cmdBuf,
		BufferRole role,
		VkPipelineStageFlags dstStages // e.g. VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
	) {
		const VkAccessFlags dstAccess =
			(role == BufferRole::UBO) ? VK_ACCESS_UNIFORM_READ_BIT
			: VK_ACCESS_SHADER_READ_BIT;

		VkResult result;
		if (ubo.is_mapapble()) {
			result = vmaCopyMemoryToAllocation(app_allocator_, data, ubo.alloc, 0, dataSize);
			assert(result == VK_SUCCESS);

			VkBufferMemoryBarrier b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
			b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
			b.dstAccessMask = dstAccess;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.buffer = ubo.buffer;
			b.offset = 0;
			b.size = VK_WHOLE_SIZE;

			vkCmdPipelineBarrier(cmdBuf,
				VK_PIPELINE_STAGE_HOST_BIT,
				dstStages, 0, 0, nullptr, 1, &b, 0, nullptr);
		}
		else {
			VkBufferMemoryBarrier b2{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
			b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b2.dstAccessMask = dstAccess;
			b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b2.buffer = ubo.buffer;
			b2.offset = 0;
			b2.size = VK_WHOLE_SIZE;

			vkCmdPipelineBarrier(cmdBuf,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				dstStages,
				0, 0, nullptr, 1, &b2, 0, nullptr);
		}
	}

	void UpdateInstanceBuffer(uint32_t currentImage) {

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
		for (const glm::mat4 tr : scene_->localTransform) {
			std::memcpy(
				data[i].transform,
				glm::value_ptr(tr),
				sizeof(float) * 16);
			i++;
		}

		std::array<uint16_t, MAX_INSTANCES> indexData = {};
		for (uint16_t index = 0; index < MAX_INSTANCES; index++) {
			indexData[index] = index;
		}

		std::vector<IndexSlot> padded(MAX_INSTANCES);
		for (size_t i = 0; i < MAX_INSTANCES; ++i) {
			padded[i].idx = static_cast<uint32_t>(indexData[i]); // zero-extend
		}

		VkResult result;

		// This is bad, will stall. Frame update should use same command buffer for all update operations.
		// FrameGraph! FrameGraph!
		VkCommandBuffer cmdBuf = beginSingleTimeCommands();

		// Right now the mapping is redundadnt 

		UploadDataToBuffer(instanceIndexUniforms_[currentImage], padded.data(),
			sizeof(IndexSlot) * MAX_INSTANCES, cmdBuf,
			BufferRole::UBO, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);

		UploadDataToBuffer(instanceDataUniforms_[currentImage], data,
			sizeof(InstanceData) * MAX_INSTANCES, cmdBuf,
			BufferRole::SSBO, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);

		endSingleTimeCommands(cmdBuf);
		delete[] data;
	}

	void LoadScene();
	void UploadScene();

#pragma region ===[ Input ]===

	static void mouseCallback(GLFWwindow* window, double xpos, double ypos) {
		auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
		if (app) app->handleMouseMovement(xpos, ypos);
	}

	static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
		auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
		if (app) app->handleKey(key, scancode, action, mods);
	}

	static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
		auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
		if (app) app->handleMouseButton(button, action, mods);
	}

	static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset) {
		auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
		if (app) app->handleMouseScroll(xOffset, yOffset);
	}

	void handleMouseScroll(double xOffset, double yOffset) {
		const float scrollSpeed = 1.0f;
		glm::vec3 forward = cameraPositioner.getForward();
		cameraPositioner.setPosition(
			cameraPositioner.getPosition() + static_cast<float>(yOffset) * scrollSpeed * forward
		);
	}

	void handleMouseMovement(double xpos, double ypos) {
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

	void handleMouseButton(int button, int action, int mods) {
		if (button == GLFW_MOUSE_BUTTON_RIGHT) {
			rightMousePressed = (action == GLFW_PRESS);
		}
	}

	void handleKey(int key, int scancode, int action, int mods) {
		bool isPressed = action != GLFW_RELEASE;

		switch (key) {
		case GLFW_KEY_R:
		case GLFW_KEY_F:
			cameraPositioner.reset();
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

	static void framebufferResizeCallback(GLFWwindow* window, int width,
		int height) {
		auto app =
			reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
		app->framebufferResized = true;
	}

#pragma endregion

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

			// Synthetic movement
			std::cout << camera_.getPosition() << std::endl;


			for (size_t i = 0; i < scene_->localTransform.size(); ++i) {
				glm::mat4& local = scene_->localTransform[i];
				float phase = std::sin(i + delta_time_ * 0.35f);  // staggered phases
				local[3][0] = static_cast<float>(i) * 1.4f + phase;
				markAsChanged(*scene_, i);
			}
			recalculateGlobalTransforms(*scene_);

			drawFrame();

		}

		vkDeviceWaitIdle(device);
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
		camera_.setPerspective(45.0f, newAspect, near_clip, far_clip);
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

	void createGraphicsPipeline(
		VkShaderModule vert,
		VkShaderModule frag,
		VkDescriptorSetLayout* descriptorSetLayout,
		VkPipelineLayout* pipelineLayout
	) {


		VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
		vertShaderStageInfo.sType =
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
		vertShaderStageInfo.module = vert;
		vertShaderStageInfo.pName = "main";

		VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
		fragShaderStageInfo.sType =
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		fragShaderStageInfo.module = frag;
		fragShaderStageInfo.pName = "main";

		VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo,
														  fragShaderStageInfo };

		VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
		vertexInputInfo.sType =
			VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		auto bindingDescription = Vertex::getBindingDescription();
		auto attributeDescriptions = Vertex::getAttributeDescriptions();

		vertexInputInfo.vertexBindingDescriptionCount = 1;
		vertexInputInfo.vertexAttributeDescriptionCount =
			static_cast<uint32_t>(attributeDescriptions.size());
		vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
		vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType =
			VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		inputAssembly.primitiveRestartEnable = VK_FALSE;

		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo rasterizer{};
		rasterizer.sType =
			VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.depthClampEnable = VK_FALSE;
		rasterizer.rasterizerDiscardEnable = VK_FALSE;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.lineWidth = 1.0f;
		rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
		rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterizer.depthBiasEnable = VK_FALSE;

		VkPipelineMultisampleStateCreateInfo multisampling{};
		multisampling.sType =
			VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampling.sampleShadingEnable = VK_FALSE;
		multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineDepthStencilStateCreateInfo depthStencil{};
		depthStencil.sType =
			VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = VK_TRUE;
		depthStencil.depthWriteEnable = VK_TRUE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
		depthStencil.depthBoundsTestEnable = VK_FALSE;
		depthStencil.stencilTestEnable = VK_FALSE;

		VkPipelineColorBlendAttachmentState colorBlendAttachment{};
		colorBlendAttachment.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachment.blendEnable = VK_FALSE;

		VkPipelineColorBlendStateCreateInfo colorBlending{};
		colorBlending.sType =
			VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlending.logicOpEnable = VK_FALSE;
		colorBlending.logicOp = VK_LOGIC_OP_COPY;
		colorBlending.attachmentCount = 1;
		colorBlending.pAttachments = &colorBlendAttachment;
		colorBlending.blendConstants[0] = 0.0f;
		colorBlending.blendConstants[1] = 0.0f;
		colorBlending.blendConstants[2] = 0.0f;
		colorBlending.blendConstants[3] = 0.0f;

		std::vector<VkDynamicState> dynamicStates = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR
		};

		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount =
			static_cast<uint32_t>(dynamicStates.size());
		dynamicState.pDynamicStates = dynamicStates.data();

		// One push constant range, covers all stages
		VkPushConstantRange pcRange{};
		pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		pcRange.offset = 0;
		pcRange.size = sizeof(PushConstants);

		VkPipelineLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = descriptorSetLayout;
		layoutInfo.pushConstantRangeCount = 1;
		layoutInfo.pPushConstantRanges = &pcRange;

		//VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		//pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		//pipelineLayoutInfo.setLayoutCount = 1;
		//pipelineLayoutInfo.pSetLayouts = descriptorSetLayout;

		//pipelineLayoutInfo.pushConstantRangeCount = 2;
		//pipelineLayoutInfo.pPushConstantRanges = pcRanges;

		if (vkCreatePipelineLayout(device, &layoutInfo, nullptr,
			pipelineLayout) != VK_SUCCESS) {
			throw std::runtime_error("failed to create pipeline layout!");
		}

		auto depth_format = findDepthFormat(physicalDevice);

		// Provide information for dynamic rendering
		VkPipelineRenderingCreateInfo pipeline_create{
			VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
		pipeline_create.pNext = VK_NULL_HANDLE;
		pipeline_create.colorAttachmentCount = 1;
		pipeline_create.pColorAttachmentFormats = &swapChainImageFormat;
		pipeline_create.depthAttachmentFormat = depth_format;
		pipeline_create.stencilAttachmentFormat = depth_format;


		VkGraphicsPipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = shaderStages;
		pipelineInfo.pVertexInputState = &vertexInputInfo;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.pDepthStencilState = &depthStencil;
		pipelineInfo.layout = *pipelineLayout;
		pipelineInfo.pNext = &pipeline_create;


		if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
			nullptr, &graphicsPipeline) != VK_SUCCESS) {
			throw std::runtime_error("failed to create graphics pipeline!");
		}

	}

	void createDepthResources() {
		VkFormat depthFormat = findDepthFormat(physicalDevice);

		main_d_alloc = createImage(
			swapChainExtent.width,
			swapChainExtent.height,
			depthFormat,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			main_depthImage
		);

		createImageView(main_depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, main_depthImageView);
	}

	// --------------------------

	void createTextureImage(std::string texture_path, VkImage* texture_image) {
		int texWidth, texHeight, texChannels;

		stbi_uc* pixels = stbi_load(texture_path.c_str(), &texWidth, &texHeight,
			&texChannels, STBI_rgb_alpha);

		VkDeviceSize imageSize = texWidth * texHeight * 4;

		if (!pixels) {
			throw std::runtime_error("failed to load texture image!");
		}

		VkBuffer stagingBuffer;
		VkDeviceMemory stagingBufferMemory;
		createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer, stagingBufferMemory);

		void* data;
		vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, &data);
		memcpy(data, pixels, static_cast<size_t>(imageSize));
		vkUnmapMemory(device, stagingBufferMemory);

		stbi_image_free(pixels);

		createImage(
			texWidth,
			texHeight,
			VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			*texture_image
		);

		transitionImageLayout(
			*texture_image,
			VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		copyBufferToImage(stagingBuffer,
			*texture_image,
			static_cast<uint32_t>(texWidth),
			static_cast<uint32_t>(texHeight)
		);

		transitionImageLayout(
			*texture_image,
			VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);

		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingBufferMemory, nullptr);

	}

	void createTextureImageView(const VkImage* textureImage, VkImageView* textureImageView) {
		createImageView(
			*textureImage,
			VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_ASPECT_COLOR_BIT,
			*textureImageView
		);
	}

	void createTextureSampler(VkSampler* textureSampler) {
		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;

		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);

		samplerInfo.anisotropyEnable = VK_TRUE;
		samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;

		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.mipLodBias = 0.0f;
		samplerInfo.mipLodBias = 0.0f;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 0.0f;

		if (vkCreateSampler(device, &samplerInfo, nullptr, textureSampler) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create texture sampler!");
		}

	}

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

	VmaAllocation createImage(
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
		//imgCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		//imgCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		imgCreateInfo.usage = usage;
		imgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		VmaAllocationCreateInfo allocCreateInfo = {};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		allocCreateInfo.priority = 1.0f;

		VmaAllocation alloc;
		vmaCreateImage(app_allocator_, &imgCreateInfo, &allocCreateInfo, &image, &alloc, nullptr);
		return alloc;
	}

	void transitionImageLayout(VkImage image, VkFormat format,
		VkImageLayout oldLayout, VkImageLayout newLayout) {
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

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

		endSingleTimeCommands(commandBuffer);
	}

	void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
		uint32_t height) {
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

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

		endSingleTimeCommands(commandBuffer);
	}

	// --------------------------

	void createUniformBuffer(
		VkBufferCreateInfo buffer_create_info,
		VmaAllocation& alloc,
		VkBuffer& buffer,
		VmaAllocationInfo& allocInfo,
		VkMemoryPropertyFlags& memory_flags
	) {

		//VkBufferCreateInfo bufCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		//bufCreateInfo.size = size;
		//bufCreateInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		// Not sure if i can extract this from the loop
		VmaAllocationCreateInfo allocCreateInfo = {};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
			VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
			VMA_ALLOCATION_CREATE_MAPPED_BIT;

		VkResult result = vmaCreateBuffer(
			app_allocator_,
			&buffer_create_info,
			&allocCreateInfo,
			&buffer,
			&alloc,
			&allocInfo
		);

		vmaGetAllocationMemoryProperties(app_allocator_, alloc, &memory_flags);

	}

	void createInstanceDataBuffers() {

		//VkDeviceSize bufferSize = sizeof(InstanceData) * MAX_INSTANCES;

		VkBufferCreateInfo instIndexBufInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		instIndexBufInfo.size = sizeof(IndexSlot) * MAX_INSTANCES;
		instIndexBufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;


		VkBufferCreateInfo instDataBufInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		instDataBufInfo.size = sizeof(InstanceData) * MAX_INSTANCES;
		instDataBufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

			createUniformBuffer(
				instIndexBufInfo,
				instanceIndexUniforms_[i].alloc,
				instanceIndexUniforms_[i].buffer,
				instanceIndexUniforms_[i].allocInfo,
				instanceIndexUniforms_[i].memory_flags
			);

		}


		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

			createUniformBuffer(
				instDataBufInfo,
				instanceDataUniforms_[i].alloc,
				instanceDataUniforms_[i].buffer,
				instanceDataUniforms_[i].allocInfo,
				instanceDataUniforms_[i].memory_flags
			);

			// I dont like how I cant leave UniformBufferAllocation in a valid state easily if I dont fill every proper field. Need a better Solution

		}
	}

	void createUniformBuffers() {

		VkBufferCreateInfo bufCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bufCreateInfo.size = sizeof(UniformBufferObject) * MAX_INSTANCES;
		bufCreateInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;


		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

			uniforms_[i].size = sizeof(UniformBufferObject) * MAX_INSTANCES;
			createUniformBuffer(
				bufCreateInfo,
				uniforms_[i].alloc,
				uniforms_[i].buffer,
				uniforms_[i].allocInfo,
				uniforms_[i].memory_flags
			);

			// I dont like how I cant leave UniformBufferAllocation in a valid state easily if I dont fill every proper field. Need a better Solution

		}
	}

	void createDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout* descriptorSetLayout) {


		VkDescriptorSetLayoutBinding instanceIndexBinding{};
		instanceIndexBinding.binding = 0;
		instanceIndexBinding.descriptorCount = 1;
		instanceIndexBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		instanceIndexBinding.pImmutableSamplers = nullptr;
		instanceIndexBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

		VkDescriptorSetLayoutBinding instanceDataBinding{};
		instanceDataBinding.binding = 1;
		instanceDataBinding.descriptorCount = 1;
		instanceDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		instanceDataBinding.pImmutableSamplers = nullptr;
		instanceDataBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

		VkDescriptorSetLayoutBinding samplerLayoutBinding{};
		samplerLayoutBinding.binding = 2;
		samplerLayoutBinding.descriptorCount = MAX_MATERIALS;
		samplerLayoutBinding.descriptorType =
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		samplerLayoutBinding.pImmutableSamplers = nullptr;
		samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		std::array<VkDescriptorSetLayoutBinding, 3> bindings = {
			instanceIndexBinding,
			instanceDataBinding,
			samplerLayoutBinding
		};

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
		layoutInfo.pBindings = bindings.data();

		if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr,
			descriptorSetLayout) != VK_SUCCESS) {
			throw std::runtime_error("failed to create descriptor set layout!");
		}
	}

	void createDescriptorPool() {

		// This is just how many dsc of each type are allowed in the pool. Has no bearing with layouts
		// except for simple compat.
		std::array<VkDescriptorPoolSize, 3> poolSizes = { {
				// We should need these one anymore once we move InstanceData to ssbo. Maybe instance data is promoted to ubo
				// in cases where it can work. Also we are adding an indirect mapping for instanceData with a table.
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,        100 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,        100 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
		} };

		// --------

		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		poolInfo.pPoolSizes = poolSizes.data();


		if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create descriptor pool!");
		}
	}

	// We should have separate sets
	//Set Index	Concern	Frequency of Update	Example Resources
	//	0	Frame - level / Pass - level	Once per frame	Camera UBO(view / proj), lights UBO, environment maps
	//	1	Object - level / Draw - call - level	Once per object	Model matrix UBO(or dynamic UBO), per - object parameters
	//	2	Material - level	Rarely(per material)	Material UBO(e.g.roughness / metallic factors), textures, samplers
	//	3 + Less common / Optional	As needed	Shadow maps, decals, particle data, etc.

	void createDescriptorSets() {

		std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT,
			descriptorSetLayout);

		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = descriptorPool;
		allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
		allocInfo.pSetLayouts = layouts.data();

		// --

		VkResult result = vkAllocateDescriptorSets(device, &allocInfo, frameDescriptorSets);

		if (result == VK_ERROR_OUT_OF_POOL_MEMORY) {
			// Rotate the descriptor pool. try again
			// Manage live dspools. defrag
		}

		if (result != VK_SUCCESS) {
			throw std::runtime_error("failed to allocate descriptor sets!");
		}

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

			VkDescriptorBufferInfo instanceIndexBufferInfo{
				.buffer = instanceIndexUniforms_[i].buffer,
				.offset = 0,
				.range = sizeof(uint16_t) * MAX_INSTANCES,
				//.range = VK_WHOLE_SIZE
			};
			VkDescriptorBufferInfo instanceBufferInfo{
				.buffer = instanceDataUniforms_[i].buffer,
				.offset = 0,

				.range = sizeof(InstanceData) * MAX_INSTANCES,
			};

			std::array<VkWriteDescriptorSet, 2> descriptorWrites{};

			descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			descriptorWrites[0].dstSet = frameDescriptorSets[i];
			descriptorWrites[0].dstBinding = 0;
			descriptorWrites[0].dstArrayElement = 0;
			descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			descriptorWrites[0].descriptorCount = 1;
			descriptorWrites[0].pBufferInfo = &instanceIndexBufferInfo;

			descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			descriptorWrites[1].dstSet = frameDescriptorSets[i];
			descriptorWrites[1].dstBinding = 1;
			descriptorWrites[1].dstArrayElement = 0;
			descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			descriptorWrites[1].descriptorCount = 1;
			descriptorWrites[1].pBufferInfo = &instanceBufferInfo;

			vkUpdateDescriptorSets(device,
				static_cast<uint32_t>(descriptorWrites.size()),
				descriptorWrites.data(), 0, nullptr);
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

	VkCommandBuffer beginSingleTimeCommands() {
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = framesCommandPool[currentFrame];
		allocInfo.commandBufferCount = 1;

		VkCommandBuffer commandBuffer;
		vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		vkBeginCommandBuffer(commandBuffer, &beginInfo);

		return commandBuffer;
	}

	void endSingleTimeCommands(VkCommandBuffer commandBuffer) {
		vkEndCommandBuffer(commandBuffer);

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;

		vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
		vkQueueWaitIdle(graphicsQueue_);

		vkFreeCommandBuffers(device, framesCommandPool[currentFrame], 1, &commandBuffer);
	}

	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

		VkBufferCopy copyRegion{};
		copyRegion.size = size;
		vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

		endSingleTimeCommands(commandBuffer);
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

	void recordDraw(VkCommandBuffer commandBuffer, uint32_t imageIndex) {


		// ---- Update descriptor set once with all materials ----
		std::vector<VkDescriptorImageInfo> infos;
		infos.reserve(test_pool_.materials_.size());

		for (auto& mat : test_pool_.materials_) {
			VkDescriptorImageInfo info{};
			info.sampler = mat.sampler;
			info.imageView = mat.imageView;
			info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			infos.push_back(info);
		}

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = frameDescriptorSets[currentFrame];
		write.dstBinding = 2; // binding for materials[]
		write.dstArrayElement = 0;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.descriptorCount = static_cast<uint32_t>(infos.size());
		write.pImageInfo = infos.data();

		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
			throw std::runtime_error("failed to begin recording command buffer!");
		}

		VkRenderingAttachmentInfoKHR depthAttachment{};
		depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
		depthAttachment.imageView = main_depthImageView;
		depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.clearValue.depthStencil = { 1.0f, 0 };

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

		vkCmdBeginRendering(commandBuffer, &renderingInfo);
		vkCmdBindPipeline(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			graphicsPipeline
		);



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
			VK_INDEX_TYPE_UINT32);

		vkCmdBindDescriptorSets(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipelineLayout,
			0,
			1,
			&frameDescriptorSets[currentFrame],
			0,
			nullptr);

		std::vector<VkDescriptorImageInfo> imageInfos;
		imageInfos.reserve(test_pool_.materials_.size());


		PushConstants pc = {  };
		pc.view = camera_.getViewMatrix();
		pc.proj = camera_.getProjMatrix();
		pc.proj[1][1] *= -1;

		// Well need ned pc range for mat_values.
		// Mat values point to texture slots. contant data.
		// Vert/Frag programs need to be binded before this
		// I would like to encode in mat constructs also raster settings.
		//MaterialValues mat_values_;

		for (int i = 0; i < scene_->hierarchy.size(); i++) {


			auto mesh_idx = scene_->meshForNode[i];
			auto mesh_ = test_pool_.meshes_[mesh_idx];
			pc.material_index = scene_->materialForNode[i];

			vkCmdPushConstants(
				commandBuffers[currentFrame],
				pipelineLayout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0,
				sizeof(pc),
				&pc
			);

			vkCmdDrawIndexed(
				commandBuffer,
				static_cast<uint32_t>(mesh_.indices.size()), //indexCount
				1, //instanceCount
				mesh_.first_index, //firstIndex
				mesh_.vert_offset, //vertexOffset
				i //firstInstance. Change this so shader acces keeps 0index but we can manipulate view from command side
			);
		}

		vkCmdEndRendering(commandBuffer);

		// Not neeeded with dynamic renderinng because attachment info is simplified from renderpass system, simplifying attachment layouts away.
		//transitionImageLayout(
		//	swapChainImages[currentFrame], VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
		//);

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

	void drawFrame() {
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
		UpdateUniformBuffer(currentFrame);
		UpdateInstanceBuffer(currentFrame);


		vkResetFences(device, 1, &inFlightFences[currentFrame]);
		vkResetCommandPool(device, framesCommandPool[currentFrame], 0);

		//vkResetCommandBuffer(commandBuffers[currentFrame],
		//	/*VkCommandBufferResetFlagBits*/ 0);

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
};

void VulkanApp::LoadScene() {
	// First node.

	//rootNode
	addNode(*scene_, -1, 0);
	addNode(*scene_, 0, 1);
	addNode(*scene_, 0, 1);

	//std::cout << std::filesystem::current_path() << std::endl;

	Mesh mesh_;
	auto a_ = GltfMesh("models/avocado_gltf/Avocado.gltf");
	mesh_.indices = a_.indices;
	mesh_.vertices = a_.vertices;

	Mesh mesh_2;
	auto b_ = GltfMesh("models/BoomBox/BoomBox.gltf");
	mesh_2.indices = b_.indices;
	mesh_2.vertices = b_.vertices;

	test_pool_.meshes_.push_back(mesh_);
	test_pool_.meshes_.push_back(mesh_2);

	auto m_m = scene_->localTransform[0];

	scene_->meshForNode[0] = 1;
	scene_->materialForNode[0] = 0;
	//scene_->localTransform[0] = glm::translate(m_m, glm::vec3(5, 0, 0));

	scene_->meshForNode[1] = 3;
	scene_->materialForNode[1] = 1;
	
	scene_->meshForNode[2] = 2;
	scene_->materialForNode[2] = 2;

	scene_->localTransform[2][0][0] = 100;
	scene_->localTransform[2][1][1] = 100;
	scene_->localTransform[2][2][2] = 100;

	scene_->localTransform[1][0][0] = 100;
	scene_->localTransform[1][1][1] = 100;
	scene_->localTransform[1][2][2] = 100;

	auto vertShaderCode = readFile("shaders/example.vert.spv");
	auto fragShaderCode = readFile("shaders/example.frag.spv");

	VkShaderModule vertShaderModule = createShaderModule(device, vertShaderCode);
	VkShaderModule fragShaderModule = createShaderModule(device, fragShaderCode);

	test_pool_.materials_.push_back(MaterialDefinition{});
	test_pool_.materials_.push_back(MaterialDefinition{});
	test_pool_.materials_.push_back(MaterialDefinition{});

	test_pool_.materials_[0].frag = fragShaderModule;
	test_pool_.materials_[0].vert = vertShaderModule;

	test_pool_.materials_[1].frag = fragShaderModule;
	test_pool_.materials_[1].vert = fragShaderModule;

	test_pool_.materials_[2].frag = vertShaderModule;
	test_pool_.materials_[2].vert = vertShaderModule;


	createDescriptorSetLayout(
		device,
		&descriptorSetLayout
	);

	createGraphicsPipeline(
		vertShaderModule,
		fragShaderModule,
		&descriptorSetLayout,
		&pipelineLayout
	);

	createTextureImage(
		"textures/viking_room.png",
		&test_pool_.materials_[0].image
	);

	createTextureImage(
		"textures/texture.jpg",
		&test_pool_.materials_[1].image
	);
	createTextureImage(
		"models/avocado_gltf/Avocado_baseColor.png",
		&test_pool_.materials_[2].image
	);


	createTextureImageView(
		&test_pool_.materials_[0].image,
		&test_pool_.materials_[0].imageView
	);


	createTextureImageView(
		&test_pool_.materials_[1].image,
		&test_pool_.materials_[1].imageView
	);

	createTextureImageView(
		&test_pool_.materials_[2].image,
		&test_pool_.materials_[2].imageView
	);


	createTextureSampler(
		&test_pool_.materials_[0].sampler
	);

	createTextureSampler(
		&test_pool_.materials_[1].sampler
	);

	createTextureSampler(
		&test_pool_.materials_[2].sampler
	);


}

void VulkanApp::UploadScene() {


	uint32_t static_vertex_buffer_size = 0;
	uint32_t static_index_buffer_size = 0;

	for (int node = 0; node < scene_->hierarchy.size(); node++) {

		static_vertex_buffer_size += sizeof(Vertex) * test_pool_.meshes_[node].vertices.size();
		static_vertex_buffer_size += sizeof(uint32_t) * test_pool_.meshes_[node].indices.size();

	}

	// Compute totals and write per-mesh offsets (skip slot 0 = empty)
	VkDeviceSize totalVertexBytes = 0;
	VkDeviceSize totalIndexBytes = 0;

	uint32_t runningVertexOffset = 0; // in vertices
	uint32_t runningFirstIndex = 0; // in indices

	for (int i = 0; i < test_pool_.meshes_.size(); ++i) {
		auto& m = test_pool_.meshes_[i];

		m.vert_offset = runningVertexOffset;         // elements, not bytes
		m.first_index = runningFirstIndex;           // elements, not bytes

		totalVertexBytes += sizeof(Vertex) * m.vertices.size();
		totalIndexBytes += sizeof(uint32_t) * m.indices.size();

		runningVertexOffset += static_cast<uint32_t>(m.vertices.size());
		runningFirstIndex += static_cast<uint32_t>(m.indices.size());
	}

	// Create GPU static buffers (DST)
	{
		assert(totalVertexBytes > 0 && "Vertex buffer size was 0");
		VkBufferCreateInfo vbInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = totalVertexBytes,
			.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		};
		VmaAllocationCreateInfo vbAlloc{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
		vmaCreateBuffer(app_allocator_, &vbInfo, &vbAlloc,
			&static_vertex_buffer_, &static_vertex_allocation_, nullptr);
	}
	{
		assert(totalIndexBytes > 0 && "Index buffer size was 0");
		VkBufferCreateInfo ibInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = totalIndexBytes,
			.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		};
		VmaAllocationCreateInfo ibAlloc{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
		vmaCreateBuffer(app_allocator_, &ibInfo, &ibAlloc,
			&static_index_buffer_, &static_index_allocation_, nullptr);
	}

	// Staging + upload: VERTICES
	{
		VkBuffer stagingBuf; VmaAllocation stagingAlloc;
		VkBufferCreateInfo sInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = totalVertexBytes,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		};
		VmaAllocationCreateInfo sAlloc{};
		sAlloc.usage = VMA_MEMORY_USAGE_AUTO;
		sAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

		vmaCreateBuffer(app_allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr);

		VkDeviceSize curr = 0; // byte offset
		for (int i = 0; i < test_pool_.meshes_.size(); ++i) {
			const auto& m = test_pool_.meshes_[i];
			const VkDeviceSize bytes = sizeof(Vertex) * m.vertices.size();
			if (bytes == 0) continue;

			vmaCopyMemoryToAllocation(app_allocator_, m.vertices.data(), stagingAlloc, curr, bytes);
			curr += bytes;
		}

		copyBuffer(stagingBuf, static_vertex_buffer_, totalVertexBytes);
		vmaDestroyBuffer(app_allocator_, stagingBuf, stagingAlloc);
	}

	// Staging + upload: INDICES
	{
		VkBuffer stagingBuf; VmaAllocation stagingAlloc;
		VkBufferCreateInfo sInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = totalIndexBytes,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		};
		VmaAllocationCreateInfo sAlloc{};
		sAlloc.usage = VMA_MEMORY_USAGE_AUTO;
		sAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

		vmaCreateBuffer(app_allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr);

		VkDeviceSize curr = 0; // byte offset
		for (int i = 0; i < test_pool_.meshes_.size(); ++i) {
			const auto& m = test_pool_.meshes_[i];
			const VkDeviceSize bytes = sizeof(uint32_t) * m.indices.size();
			if (bytes == 0) continue;

			// Indices remain local (0-based). We won't rewrite them here.
			vmaCopyMemoryToAllocation(app_allocator_, m.indices.data(), stagingAlloc, curr, bytes);
			curr += bytes;
		}

		copyBuffer(stagingBuf, static_index_buffer_, totalIndexBytes);
		vmaDestroyBuffer(app_allocator_, stagingBuf, stagingAlloc);
	}

}


bool VulkanApp::run() {

	camera_.setPerspective(45.0f, float(width_) / float(height_), near_clip, far_clip);

	// ---------------

	if (volkInitialize() != VK_SUCCESS) {
		std::cerr << "Failed to initialize Volk\n";
		glfwTerminate();
		return false;
	};

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
	//glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	glfwSetWindowTitle(window, ENGINE_NAME);
	glfwSetKeyCallback(window, VulkanApp::keyCallback);
	glfwSetCursorPosCallback(window, VulkanApp::mouseCallback);
	glfwSetMouseButtonCallback(window, VulkanApp::mouseButtonCallback);
	glfwSetScrollCallback(
		window, VulkanApp::scrollCallback);  // Register scroll callback

	glfwSetWindowUserPointer(window, this);
	glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

	initVulkanApp();

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

	LoadScene();
	UploadScene();

	// FIXED SIZES
	createUniformBuffers();
	createInstanceDataBuffers();

	createCommandBuffers();
	createSyncObjects();
	createDepthResources();

	// MATERIAL
	// -------------------------------------------------------------------------------------

	// this doenst use a shader cache
	createDescriptorPool(); // Pool has the shape of the material!. Also 
	createDescriptorSets();

	// -------------------------------------------------------------------------------------


	mainLoop();

	//cleanup();
	vmaDestroyAllocator(app_allocator_);

	return true;

};


void VulkanApp::initVulkanApp() {

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

		VkPhysicalDeviceVulkan13Features vulkan13Features{};
		vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		vulkan13Features.dynamicRendering = VK_TRUE;

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


	createSwapChain();
	renderFinishedSemaphores = new VkSemaphore[swapchainImageCount];
	createSwapchainImageViews();

}

int main(int argc, const char* argv[]) {
	if (argc > 1) {
		std::cout << argv[0];
	}

	VulkanApp app;
	try {
		app.run();
	}
	catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
