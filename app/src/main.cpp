#pragma once

#include <pool.h>
#include <Engine/Vulkan/vulkan_setup.h>
#include <Engine/Vulkan/vulkan_helpers.h>
#include <Engine/Vulkan/vulkan_data_structures.h>

#include <cassert>

#define TINYOBJLOADER_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>
#include <Engine/Renderer/primitives.h>
#include <Engine/common.h>

// -------------------------------------



static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t WIDTH = 1280;
static constexpr uint32_t HEIGHT = 720;




int vertex_shader_count = 10;
int frag_shader_count = 10;
VkShaderModule vertex_shaders[10];
VkShaderModule frag_shaders[10];


struct StandardBRDFMatAttachment {

	//texture handles to imageview

};


struct GPUMaterial {

	int vert_shader_id;
	int frag_shader_id;
	//attachments?
	// or maybe descriptor format abstraction. and flags. like opaque transparent/else
};


// Ill be supporting just VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST.
struct GPUMesh {

	int buffer_id;
	int gpu_material_id;
	int vertex_start;
	int index_start;
	int vertex_ammount;
	int index_ammount;

};




CameraPositioner_FirstPerson cameraPositioner(
	glm::vec3(0.0f, 0.0f, -2.0f),     // position
	glm::vec3(0.0f, 0.0f, 1.0f),     // target (looking in +Z direction)
	glm::vec3(0.0f, 1.0f, 0.0f)      // up vector
);

std::vector<Vertex> quad_vertices = {
	{{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
	{{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
	{{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
	{{-0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
};

std::vector<uint32_t> quad_indices = {
	0, 1, 2, 2, 3, 0
};

// ???
struct GPULight {
	// Maybe we dont need scale and can compress there. Myabe not worth it cuz breaks homogeneus coords
	glm::mat4 transform;
	//type
	//cone
	//color
	//intensity
} DefaultLight;



Mesh Quad = Mesh(quad_vertices, quad_indices);




// Statements 
// Forward+ . Maybe tiled deferred shadows in future
// Just static geo. Dont focus on streaming yet
// Dynamic Rendering
// Under 10 Materials Definitions. MAterial is stages, pipeline blend, depth conf, vert and shader. attachment slots desc. No more
// Mateials. Simple standwardSurface (Default), Unlit, Anisotropic, SubSurface, Translucent, Hair, Cloth

// GPU Vulkan state, resources / etc
class VulkanContext {


	VkDevice device;


	// Main depth image is the one for the main view.
	VkImage main_depthImage;
	VkDeviceMemory main_depthImageMemory;
	VkImageView main_depthImageView;

#pragma region ===[ Pipeline / Render Resources ]===

	// These are not single objects! Should be made pools. 
	VkDescriptorSetLayout descriptorSetLayout;
	VkPipelineLayout pipelineLayout;
	VkPipeline graphicsPipeline;
	// *

	VkCommandPool graphicsCommandPool;
	VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

#pragma region ===[ Graphs ]===

	VkDescriptorPool descriptorPool;
	VkDescriptorSet descriptorSets[MAX_FRAMES_IN_FLIGHT];

#pragma endregion


#pragma region ===[ Texture Resources ]===

	VkImage * textureImages;
	VkImageView* textureImageViews;
	VkSampler* textureSamplers;

#pragma endregion

#pragma region ===[ Geometry Buffers ]===

	VkBuffer staticVertexBuffer;
	VkBuffer statcIndexBuffer;

#pragma endregion

#pragma region ===[ Uniforms / Descriptor Sets ]===

	VkBuffer uniformBuffers[MAX_FRAMES_IN_FLIGHT];
	VkDeviceMemory uniformBuffersMemory[MAX_FRAMES_IN_FLIGHT];
	void* uniformBuffersMapped[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

	VkQueue graphicsQueue;
	//VkQueue computeQueue;
	VkQueue presentQueue;


	VkDeviceMemory app_lifetime_memory_images;
	VkDeviceMemory scene_lifetime_memory_images;
	VkDeviceMemory scene_lifetime_memory_tris;
	VkDeviceMemory scene_lifetime_memorty_indexes;
	VkDeviceMemory staging_memory;
	// We may diasmibguate later
	VkDeviceMemory frame_lifetime;


	// Holds both vert and tri index
	VkBuffer static_geo_buffer;
	// Wont use now. Lets do just static ar first
	//VkBuffer dynamic_geo_buffer;


public:

	// Allocate memory buffers
	void initialize();

	int loadScene() {


		//for obj in scene;

		mesh_id = createGPUMesh(Quad);
		isntnace_id = createInstance(mesh_id, )
			createGPULight();


	};

	int createGPUInstance(uint32_t mesh_id);
	int createGPULight();
	int createGPUTexture();
	int createGPUMesh(Mesh mesh);
	int createGPUMaterial();
	int createGPUMeshInstance();


	//frame update. cam ubos maybe frame culling
	void update();

	//build graphs. record commands
	void draw();
	// reset frame-lifetime resources.
	void finish_draw();



	VkShaderModule createShaderModule(const std::vector<char>& code) {
		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = code.size();

		// We feed vector char bin data. SPIRV bin data
		createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

		VkShaderModule shaderModule;
		if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create shader module!");
		}

		return shaderModule;
	}

};





VulkanContext initialize() {

	//Memory Reservations


}




//VulkanContext Upload(char* data, BufferLoc destination) {
//
//}


VulkanContext CreateMesh(Mesh mesh) {

	//Upload vetex data
	mesh.vertices;





	return i;



}




// -------------------------------------

class VulkanApp {


public:


	bool run();


private:

#pragma region ===[ App / Core State ]===
	Camera camera_ = Camera(cameraPositioner);
	Scene* scene;
	float delta_time;
#pragma endregion

#pragma region ===[ Swapchain ]===
	VkSwapchainKHR swapChain;

	uint32_t swapchainImageCount;
	VkImage* swapChainImages;
	VkImageView* swapChainImageViews;
	VkFramebuffer* swapChainFrameBuffers;

	VkFormat swapChainImageFormat;
	VkExtent2D swapChainExtent;
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

	void updateUniformBuffer(uint32_t currentImage) {


		// THIS MUST REMAIN HERE. SENSITIVE TO LAG OTHERWISE
		static auto startTime = std::chrono::high_resolution_clock::now();
		auto currentTime = std::chrono::high_resolution_clock::now();
		delta_time = std::chrono::duration<float, std::chrono::seconds::period>(
			currentTime - startTime)
			.count();

		UniformBufferObject ubo{};
		Transform tesssst = Transform{};

		tesssst.scale = { 10, 10, -10 };
		tesssst.translation = { 0, -1.f, 0.f };

		ubo.model = getWorldTransform(tesssst);
		ubo.proj = camera_.getProjMatrix();
		ubo.view = camera_.getViewMatrix();


		//ctx. get copy location
		//memcpy(uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));

		assert(false);

	}

#pragma region ===[ Events ]===

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

		glm::vec2 mousePos = glm::vec2(static_cast<float>(xpos), static_cast<float>(ypos));
		cameraPositioner.update(0.0, mousePos, true); // We'll pass real deltaSeconds in mainLoop
	}


	void handleMouseButton(int button, int action, int mods) {
		if (button == GLFW_MOUSE_BUTTON_RIGHT) {
			//rightMousePressed = (action == GLFW_PRESS);
			//if (rightMousePressed) {
			//	double x, y;
			//	glfwGetCursorPos(window, &x, &y);
			//	cameraPositioner.resetMousePosition(glm::vec2(x, y));
			//}
		}
	}


	void handleKey(int key, int scancode, int action, int mods) {
		bool isPressed = action != GLFW_RELEASE;

		switch (key) {
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
			cameraPositioner.movement_.down_ = isPressed;
			break;
		case GLFW_KEY_E:
			cameraPositioner.movement_.up_ = isPressed;
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
			cameraPositioner.update(delta, glm::vec2(x, y), rightMousePressed);


			drawFrame();
		}

		vkDeviceWaitIdle(device);
	}

	void cleanupSwapChain() {

		vkDestroyImageView(device, depthImageView, nullptr);
		vkDestroyImage(device, depthImage, nullptr);
		vkFreeMemory(device, depthImageMemory, nullptr);

		// Destroy framebuffers
		for (uint32_t i = 0; i < swapchainImageCount; ++i) {
			vkDestroyFramebuffer(device, swapChainFrameBuffers[i], nullptr);
		}
		delete[] swapChainFrameBuffers;
		swapChainFrameBuffers = nullptr;

		for (uint32_t i = 0; i < swapchainImageCount; ++i) {
			vkDestroyImageView(device, swapChainImageViews[i], nullptr);
		}
		delete[] swapChainImageViews;
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

		vkDeviceWaitIdle(device);

		cleanupSwapChain();

		createSwapChain();
		createSwapchainImageViews();
		createDepthResources();
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
			querySwapChainSupport(physicalDevice);

		VkSurfaceFormatKHR surfaceFormat =
			chooseSwapSurfaceFormat(swapChainSupport.formats);
		VkPresentModeKHR presentMode =
			chooseSwapPresentMode(swapChainSupport.presentModes);
		VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);

		swapchainImageCount = swapChainSupport.capabilities.minImageCount + 1;
		swapChainImages = new VkImage[swapchainImageCount];
		swapChainFrameBuffers = new VkFramebuffer[swapchainImageCount];
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

		QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
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
		renderFinishedSemaphores = new VkSemaphore[swapchainImageCount];

		swapChainImageFormat = surfaceFormat.format;
		swapChainExtent = extent;
	}

	void createSwapchainImageViews() {

		for (uint32_t i = 0; i < swapchainImageCount; ++i) {
			swapChainImageViews[i] = createImageView(
				swapChainImages[i], swapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT
			);
		}

	}

	void createGraphicsPipeline(
		VkShaderModule vert,
		VkShaderModule frag,
		VkDescriptorSetLayout descriptorSetLayout,
		VkPipelineLayout pipelineLayout
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
		//rasterizer.lineWidth = 1.0f;
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

		std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT,
													 VK_DYNAMIC_STATE_SCISSOR };

		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount =
			static_cast<uint32_t>(dynamicStates.size());
		dynamicState.pDynamicStates = dynamicStates.data();

		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;

		if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr,
			&pipelineLayout) != VK_SUCCESS) {
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
		pipelineInfo.layout = pipelineLayout;
		pipelineInfo.pNext = &pipeline_create;


		if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
			nullptr, &graphicsPipeline) != VK_SUCCESS) {
			throw std::runtime_error("failed to create graphics pipeline!");
		}


		for (int i = 0; i < vertex_shader_count; i++) {
			vkDestroyShaderModule(device, vertex_shaders[i], nullptr);
		}

		for (int i = 0; i < frag_shader_count; i++) {
			vkDestroyShaderModule(device, frag_shaders[i], nullptr);
		}

	}

	void createDepthResources() {
		VkFormat depthFormat = findDepthFormat(physicalDevice);

		createImage(
			swapChainExtent.width, swapChainExtent.height, depthFormat,
			VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage, depthImageMemory);

		depthImageView =
			createImageView(depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
	}

	void createTextureImage(std::string texture_path) {
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
			texWidth, texHeight, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, textureImage, textureImageMemory);

		// WE TRANSITION THE IMAGE LAYOUT TO OPTIMIZE COPY!

		transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		copyBufferToImage(stagingBuffer, textureImage,
			static_cast<uint32_t>(texWidth),
			static_cast<uint32_t>(texHeight));
		transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingBufferMemory, nullptr);
	}

	void createTextureImageView() {
		textureImageView = createImageView(textureImage, VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_ASPECT_COLOR_BIT);
	}

	VkSampler createTextureSampler() {
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

		if (vkCreateSampler(device, &samplerInfo, nullptr, &textureSampler) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create texture sampler!");
		}
	}

	VkImageView createImageView(VkImage image, VkFormat format,
		VkImageAspectFlags aspectFlags) {
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

		VkImageView imageView;

		if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create texture image view!");
		}

		return imageView;
	}

	void createImage(uint32_t width, uint32_t height, VkFormat format,
		VkImageTiling tiling, VkImageUsageFlags usage,
		VkMemoryPropertyFlags properties, VkImage& image,
		VkDeviceMemory& imageMemory) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = width;
		imageInfo.extent.height = height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = format;
		imageInfo.tiling = tiling;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = usage;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
			throw std::runtime_error("failed to create image!");
		}

		VkMemoryRequirements memRequirements;
		vkGetImageMemoryRequirements(device, image, &memRequirements);

		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memRequirements.size;
		allocInfo.memoryTypeIndex =
			findMemoryType(memRequirements.memoryTypeBits, properties);

		if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to allocate image memory!");
		}

		vkBindImageMemory(device, image, imageMemory, 0);
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

	void createVertexBuffer() {
		VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

		assert((bufferSize > 0, "Vertex buffer size was 0"));

		VkBuffer stagingBuffer;
		VkDeviceMemory stagingBufferMemory;
		createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer, stagingBufferMemory);

		void* data;
		vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
		memcpy(data, vertices.data(), (size_t)bufferSize);
		vkUnmapMemory(device, stagingBufferMemory);

		createBuffer(
			bufferSize,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			vertexBuffer,
			vertexBufferMemory
		);

		copyBuffer(stagingBuffer, vertexBuffer, bufferSize);

		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingBufferMemory, nullptr);
	}

	void createIndexBuffer() {
		VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();

		VkBuffer stagingBuffer;
		VkDeviceMemory stagingBufferMemory;
		createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer, stagingBufferMemory);

		void* data;
		vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
		memcpy(data, indices.data(), (size_t)bufferSize);
		vkUnmapMemory(device, stagingBufferMemory);

		createBuffer(
			bufferSize,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, indexBuffer, indexBufferMemory);

		copyBuffer(stagingBuffer, indexBuffer, bufferSize);

		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingBufferMemory, nullptr);
	}

	void createUniformBuffers() {
		VkDeviceSize bufferSize = sizeof(UniformBufferObject);


		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
			createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				uniformBuffers[i], uniformBuffersMemory[i]);

			vkMapMemory(device, uniformBuffersMemory[i], 0, bufferSize, 0,
				&uniformBuffersMapped[i]);
		}
	}

	void createDescriptorPool() {
		std::array<VkDescriptorPoolSize, 2> poolSizes{};
		poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		poolSizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
		poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSizes[1].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		;
		poolInfo.pPoolSizes = poolSizes.data();
		poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

		if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create descriptor pool!");
		}
	}

	void createDescriptorSets() {
		std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT,
			descriptorSetLayout);
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = descriptorPool;
		allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
		allocInfo.pSetLayouts = layouts.data();

		if (vkAllocateDescriptorSets(device, &allocInfo, descriptorSets) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to allocate descriptor sets!");
		}

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
			VkDescriptorBufferInfo bufferInfo{};
			bufferInfo.buffer = uniformBuffers[i];
			bufferInfo.offset = 0;
			bufferInfo.range = sizeof(UniformBufferObject);

			VkDescriptorImageInfo imageInfo{};
			imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo.imageView = textureImageView;
			imageInfo.sampler = textureSampler;

			std::array<VkWriteDescriptorSet, 2> descriptorWrites{};

			descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			descriptorWrites[0].dstSet = descriptorSets[i];
			descriptorWrites[0].dstBinding = 0;
			descriptorWrites[0].dstArrayElement = 0;
			descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			descriptorWrites[0].descriptorCount = 1;
			descriptorWrites[0].pBufferInfo = &bufferInfo;

			descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			descriptorWrites[1].dstSet = descriptorSets[i];
			descriptorWrites[1].dstBinding = 1;
			descriptorWrites[1].dstArrayElement = 0;
			descriptorWrites[1].descriptorType =
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			descriptorWrites[1].descriptorCount = 1;
			descriptorWrites[1].pImageInfo = &imageInfo;

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
		allocInfo.commandPool = graphicsCommandPool;
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

		vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
		vkQueueWaitIdle(graphicsQueue);

		vkFreeCommandBuffers(device, graphicsCommandPool, 1, &commandBuffer);
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
		allocInfo.commandPool = graphicsCommandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = (uint32_t)MAX_FRAMES_IN_FLIGHT;

		if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to allocate command buffers!");
		}
	}

	void recordDraw(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
			throw std::runtime_error("failed to begin recording command buffer!");
		}

		VkRenderingAttachmentInfoKHR depthAttachment{};
		depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
		depthAttachment.imageView = depthImageView;
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
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

		VkViewport viewport{};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = (float)swapChainExtent.width;
		viewport.height = (float)swapChainExtent.height;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

		VkRect2D scissor{};
		scissor.offset = { 0, 0 };
		scissor.extent = swapChainExtent;
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

		VkBuffer vertexBuffers[] = { vertexBuffer };
		VkDeviceSize offsets[] = { 0 };

		vkCmdBindVertexBuffers(
			commandBuffer, 0, 1, vertexBuffers, offsets
		);
		vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipelineLayout, 0, 1, &descriptorSets[currentFrame],
			0, nullptr);

		vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indices.size()), 1, 0,
			0, 0);

		// vkCmdEndRenderPass(commandBuffer);
		vkCmdEndRendering(commandBuffer);


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
				//vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
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

		updateUniformBuffer(currentFrame);

		vkResetFences(device, 1, &inFlightFences[currentFrame]);

		vkResetCommandBuffer(commandBuffers[currentFrame],
			/*VkCommandBufferResetFlagBits*/ 0);
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

		if (vkQueueSubmit(graphicsQueue, 1, &submitInfo,
			inFlightFences[currentFrame]) != VK_SUCCESS) {
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

		result = vkQueuePresentKHR(presentQueue, &presentInfo);

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




bool VulkanApp::run() {

	main_camera.setPerspective(45.0f, float(WIDTH) / float(HEIGHT), 0.1f, 100.0f);

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

	window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);

	if (!window) {
		std::cerr << "Failed to create GLFW window\n";
		glfwTerminate();
		return false;
	}
	glfwMakeContextCurrent(window);
	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	glfwSetKeyCallback(window, VulkanApp::keyCallback);
	glfwSetCursorPosCallback(window, VulkanApp::mouseCallback);
	glfwSetMouseButtonCallback(window, VulkanApp::mouseButtonCallback);
	glfwSetScrollCallback(
		window, VulkanApp::scrollCallback);  // Register scroll callback

	glfwSetWindowUserPointer(window, this);
	glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

	initVulkanApp();


	// THIS SHOULD BE MOVE TO VK CONTEXT?

	QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);

	VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value(),
	};

	if (vkCreateCommandPool(device, &poolInfo, nullptr, &graphicsCommandPool) !=
		VK_SUCCESS) {
		throw std::runtime_error("failed to create graphics command pool!");
	}

	ctx uploadMesh(Quad);
	ctx createTexture();

	createVertexBuffer();
	createIndexBuffer();
	createUniformBuffers();

	// this doenst use a shader cache pretty rough!

	auto vertShaderCode = readFile("shaders/example.vert.spv");
	auto fragShaderCode = readFile("shaders/example.frag.spv");

	VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
	VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

	vertex_shaders[0] = vertShaderModule;
	frag_shaders[0] = fragShaderModule;


	createDescriptorSetLayout();
	createGraphicsPipeline();

	createDepthResources();
	createTextureImage("textures/viking_room.png");
	createTextureImageView();
	createTextureSampler();

	createDescriptorPool();
	createDescriptorSets();
	createCommandBuffers();

	createSyncObjects();

	// -------------------------------

	mainLoop();

	//cleanup();

	return true;

};


void VulkanApp::initVulkanApp() {

	//camera_ = new Camera(cameraPositioner);


	{
		if (enableValidationLayers && !checkValidationLayerSupport()) {
			throw std::runtime_error(
				"validation layers requested, but not available!");
		}

		VkApplicationInfo appInfo{};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "Hello Triangle";
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
		if (isDeviceSuitable(device)) {
			physicalDevice = device;
			break;
		}
	}

	if (physicalDevice == VK_NULL_HANDLE) {
		throw std::runtime_error("failed to find a suitable GPU!");
	}

	// Create Logical Device
	{
		QueueFamilyIndices indices = findQueueFamilies(physicalDevice);

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

		vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
		vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
	}

	createSwapChain();
	createSwapchainImageViews();
}

int main(int argc, const char* argv[]) {
	if (argc > 1) {
		std::cout << argv[0];
	}

	constexpr size_t poolCapacity = 1024 * 1024;
	constexpr size_t blockSize = 64;

	VirtualPool<Enemy, poolCapacity, blockSize> enemyPool;

	Handle<Enemy> h1 = enemyPool.create(Enemy(100));
	Handle<Enemy> h2 = enemyPool.create(Enemy(200));

	if (Enemy* e = enemyPool.get(h1)) {
		std::cout << "h1 health: " << e->health << "\n";
	}

	enemyPool.destroy(h1);

	if (!enemyPool.get(h1)) {
		std::cout << "h1 handle is now invalid.\n";
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
