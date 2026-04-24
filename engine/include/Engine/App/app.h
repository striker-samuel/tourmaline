#pragma once

#include <algorithm>
#include <cassert>

//
#include "volk.h"
#include "vk_mem_alloc.h"
#include <GLFW/glfw3.h>

// Imgui
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

//
#include <Engine/Renderer/context.h>
#include <Engine/Vulkan/VulkanSwapchain.h>

#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>

#include <Engine/Utils/gltf.h>
#include <Engine/platform.h>
#include <Engine/Utils/bvh.h>
//

// --Defaults--
constexpr VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;


// 
struct ToyCommandBuffer final {
	// Maybe this shoudl be wrapped
	VkCommandBuffer commandBuffer;

	PipelineHandle pipeline = {};
	VkRenderingInfo renderingInfo;
	//ToyFrameBuffer fb;
	//ToyRenderPass rp;

	//VkPipeline lastBoundPipeline_ = VK_NULL_HANDLE;
	//bool rendering_ = false;

};

struct ToyRenderPass final {
	// This will be needed later 
	//struct AttachmentPass {
	//};

	//AttachmentPass color[1];
	//AttachmentPass depth;
	//AttachmentPass stencil;

	VkRenderingAttachmentInfo color[1] = {};

	VkRenderingAttachmentInfo depth = { .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE };
	VkRenderingAttachmentInfo stencil = { .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE };
};

struct ToyFrameBuffer final {
	struct Attachment {
		VkImageView render;
		//ImageHandle render;
		//ImageHandle resolve_texture;
	};
	Attachment color;
	Attachment depth;
};



struct Level {
	Scene scene = {};
	BvhTree tree = {};
	Context* ctx = nullptr;
	bool tree_done = false;
};


// Should be renamned to somnehting like VulkanAppBase or somethign like that.
class VulkanApp {

public:

	//VulkanApp(){} // this is just defualt really
	virtual ~VulkanApp() = default;

	virtual void DrawFrame() = 0;

	bool Initialize();
	bool Run() {

		bool result = Initialize();
		assert(result && "Failed to initialize");

		MainLoop();
		CleanUp();

		return true;

	};

protected:

	Context* context = nullptr; //resources
	ImGuiContext* imgui_ctx = nullptr;
	VulkanSwapchain* swapchain = nullptr;

	VkQueue presentQueue = VK_NULL_HANDLE;
	VkQueue graphicsQueue = VK_NULL_HANDLE;


#pragma region ===[ App / Core State ]===

	float delta_time_s = .0f;
	float time_since_start_s = .0f;

	glm::mat4 frozenMainView = {};

#pragma endregion

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

#pragma region ===[ Frame Commands List ]===

	VkCommandPool perFrameCommandPools[MAX_FRAMES_IN_FLIGHT];
	VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT];

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

			//OPTICK_FRAME("MainThread");

			glfwPollEvents();

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


	void EarlyUpdate() {
		//level.scene ? recalculateGlobalTransforms(level.scene) : void();
		//recalculateGlobalTransforms(level.scene);

	}
	void Update() {
		//OPTICK_EVENT();
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

	void LateUpdate() {}

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
	void CleanUp();

	//
	//// --------------------------


	// these all should be part of the command list recording cotnext/abstraction

	//// These descriptors sets can be set and done.
	//// Bindless textures image and samplers set cannot be
	//void SetDescriptorSets()
	//{
	//	// Dynamic
	//	for (size_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT; frameIndex++) {

	//		auto setHandle = perFrameData[frameIndex].frameDynamicDescriptorSet;
	//		auto setObj = context->descriptorSets_.get(setHandle);
	//		VkDescriptorSet vkSet = setObj->set;

	//		// FrameUniforms 
	//		auto frameUniformsBufferHandle = context->buffers_.get(frameUniformsBinding.buffer_h);
	//		VkDescriptorBufferInfo frameUniformsInfo{
	//			.buffer = frameUniformsBufferHandle->buffer,
	//			.offset = 0,
	//			.range = sizeof(FrameUniforms) // this is the size of the window accesed per bind
	//		};

	//		// Draw data
	//		auto drawsDataBufferHandle = context->buffers_.get(drawDataBinding.buffer_h);
	//		VkDescriptorBufferInfo drawBufferInfo{
	//			.buffer = drawsDataBufferHandle->buffer,
	//			.offset = 0,
	//			.range = sizeof(DrawSetData) * MAX_DRAWS
	//		};
	//		VkWriteDescriptorSet writes[2]{};

	//		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	//		writes[0].dstSet = vkSet;
	//		writes[0].dstBinding = 0; // UBO
	//		writes[0].dstArrayElement = 0;
	//		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	//		writes[0].descriptorCount = 1;
	//		writes[0].pBufferInfo = &frameUniformsInfo;

	//		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	//		writes[1].dstSet = vkSet;
	//		writes[1].dstBinding = 1; // SSBO
	//		writes[1].dstArrayElement = 0;
	//		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
	//		writes[1].descriptorCount = 1;
	//		writes[1].pBufferInfo = &drawBufferInfo;

	//		vkUpdateDescriptorSets(device, 2 /*descriptorWriteCount*/, writes, 0, nullptr);

	//	}
	//}

	//// This could be done way more clever. Only updating changed textures.
	//// This may cause problems over the frame boundary if texture list changes...
	//void UpdateBindlessTexturesSets() {

	//	const uint32_t materialCount = static_cast<uint32_t>(context->materialInstances_.numObjects());

	//	std::vector<VkDescriptorImageInfo> imageInfos;
	//	std::vector<VkDescriptorImageInfo> samplerInfos;

	//	int count = (context->images_.numObjects());
	//	imageInfos.reserve(count);
	//	samplerInfos.reserve(count);


	//	context->images_.forEach([&](Image obj_) {

	//		VkDescriptorImageInfo ii{};
	//		ii.sampler = VK_NULL_HANDLE; // separate samplers -> leave null
	//		ii.imageView = obj_.sampleView;
	//		ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	//		imageInfos.push_back(ii);


	//		// Sampler for the bindless texure approach needs refacotr.
	//		// Doesnt Really make sense.
	//		VkDescriptorImageInfo si{};
	//		si.sampler = obj_.sampler;
	//		si.imageView = VK_NULL_HANDLE;            // not used for SAMPLER
	//		si.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED; // ignored for SAMPLER
	//		samplerInfos.push_back(si);

	//		}
	//	);

	//	for (size_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT; frameIndex++) {
	//		//// --- Two writes: images at index 2, samplers at index 3 ---
	//		VkWriteDescriptorSet writes[2]{};

	//		auto setHandle = perFrameData[frameIndex].frameDescriptorSet;
	//		auto set = context->descriptorSets_.get(setHandle);

	//		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	//		writes[0].dstSet = set->set;
	//		writes[0].dstBinding = 2;
	//		writes[0].dstArrayElement = 0;
	//		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	//		writes[0].descriptorCount = imageInfos.size();
	//		writes[0].pImageInfo = imageInfos.data();

	//		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	//		writes[1].dstSet = set->set;
	//		writes[1].dstBinding = 3;
	//		writes[1].dstArrayElement = 0;
	//		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	//		writes[1].descriptorCount = static_cast<uint32_t>(samplerInfos.size());   // or 1 if you use a single shared sampler
	//		writes[1].pImageInfo = samplerInfos.data();

	//		vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
	//	}

	//}

	void BindVertex(VkCommandBuffer commandBuffer) {

		// Using pages but here we ignore it...
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

	//This approach is wrong, forces manual discipline, to not interleave acquire and submit of different toyCommandBuffer objs.
	   //Could be better done with the lightweightVk approach
	ToyCommandBuffer AcquireMain(
		//ToyRenderPass rp,
		//ToyFrameBuffer fb
	);
	void Submit(ToyCommandBuffer& cmds);

	// This was the main event entry poitn for frameDraw. No longer done this way
	//void DrawFrame();

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

	auto im_dp = context->descriptorSetPools_.get(context->imgui_descriptor_pool);

	// Provide information for dynamic rendering
	VkPipelineRenderingCreateInfo pipeline_create{
		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	pipeline_create.pNext = VK_NULL_HANDLE;

	// We are using the single color buffer out.
	pipeline_create.colorAttachmentCount = 1;
	pipeline_create.pColorAttachmentFormats = &swapchain->surfaceFormat_;

	pipeline_create.depthAttachmentFormat = findDepthFormat(physicalDevice);
	pipeline_create.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
	ImGui_ImplGlfw_InitForVulkan(window, /*install callbacks*/ false);

	SwapChainSupportDetails swapChainSupport =
		querySwapChainSupport(physicalDevice, surface);

	ImGui_ImplVulkan_InitInfo info{};
	info.Instance = instance;
	info.ApiVersion = VK_API_VERSION_1_3;
	info.PhysicalDevice = physicalDevice;
	info.Device = device;
	info.Queue = context->default_queue;
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

		vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
		vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);

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
	glfwSetWindowTitle(window, /*ENGINE_NAME*/ "VulkanBackend");
	glfwSetKeyCallback(window, VulkanApp::keyCallback);
	glfwSetCursorPosCallback(window, VulkanApp::mouseCallback);
	glfwSetMouseButtonCallback(window, VulkanApp::mouseButtonCallback);
	glfwSetScrollCallback(
		window, VulkanApp::scrollCallback);  // Register scroll callback

	glfwSetWindowUserPointer(window, this);
	glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

	// Pick physical device, create logical device, get queues from physdevice. Create VMA allocator
	InitVulkanApp();

	//

	SwapChainSupportDetails swapChainSupport =
		querySwapChainSupport(physicalDevice, surface);
	VkSurfaceFormatKHR surfaceFormat =
		chooseSwapSurfaceFormat(swapChainSupport.formats);

	// SWAPCHAIN

	auto swap = new VulkanSwapchain(
		*context,
		physicalDevice, // This may be redundant, context already knows of device, and physDevice.
		device, //
		surface,
		presentQueue, // Queue acquisition is currently done on InitVulkanApp()
		window,
		static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT),
		static_cast<uint32_t>(DESIRED_SWAPCHAIN_IMAGE_COUNT)
	);
	// 

	InitializeImGui();

	// FRAME COMMAND 
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

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = perFrameCommandPools[0]; // cant this just be 0 here?
	//allocInfo.commandPool = perFrameCommandPools[currentFrame];
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = (uint32_t)MAX_FRAMES_IN_FLIGHT;

	if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers) !=
		VK_SUCCESS) {
		throw std::runtime_error("failed to allocate command buffers!");
	}

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
	//const float scrollSpeed = 1.0f;
	//glm::vec3 forward = cameraPositioner.getForward();
	//cameraPositioner.setPosition(
	//	cameraPositioner.getPosition() + static_cast<float>(yOffset) * scrollSpeed * forward
	//);
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
	//bool isPressed = action != GLFW_RELEASE;

	//switch (key) {
	//case GLFW_KEY_R:
	//case GLFW_KEY_F:
	//	cameraPositioner.reset();
	//	break;
	//case GLFW_KEY_W:
	//case GLFW_KEY_UP:
	//	cameraPositioner.movement_.forward_ = isPressed;
	//	break;
	//case GLFW_KEY_S:
	//case GLFW_KEY_DOWN:
	//	cameraPositioner.movement_.backward_ = isPressed;
	//	break;
	//case GLFW_KEY_A:
	//case GLFW_KEY_LEFT:
	//	cameraPositioner.movement_.left_ = isPressed;
	//	break;
	//case GLFW_KEY_D:
	//case GLFW_KEY_RIGHT:
	//	cameraPositioner.movement_.right_ = isPressed;
	//	break;
	//case GLFW_KEY_Q:
	//case GLFW_KEY_SPACE:
	//	cameraPositioner.movement_.up_ = isPressed;
	//	break;
	//case GLFW_KEY_E:
	//	cameraPositioner.movement_.down_ = isPressed;
	//	break;
	//case GLFW_KEY_LEFT_SHIFT:
	//	cameraPositioner.movement_.fastSpeed_ = isPressed;
	//	break;
	//default:
	//	break;
	//}
}

void VulkanApp::framebufferResizeCallback(GLFWwindow* window, int width,
	int height) {
	auto app =
		reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	app->framebufferResized = true;
}

#pragma endregion

ToyCommandBuffer VulkanApp::AcquireMain(
	//ToyFrameBuffer fb,
	//ToyRenderPass rp
)
{

	vkWaitForFences(
		device,
		1,
		&swapchain->GetInFlightFence(),
		VK_TRUE,
		UINT64_MAX
	);

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(
		device,
		swapchain->swapchain_,
		UINT64_MAX,
		swapchain->imageAvailableSemaphores_[swapchain->currentFrame_],
		VK_NULL_HANDLE,
		&imageIndex
	);


	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		swapchain->RecreateSwapChain();

		// AcquireMain needs to be reworked so empry return is legal, and tries to acquire again
		assert(false && "not implemented yet");
		//return;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	//
	swapchain->SetCurrentImageIndex(imageIndex);

	// This needs to be here for correctness. Acquire image can hold.
	UpdateDeltaTime();

	// Now we reset the command obj we will be recording to.
	// Resetting pool so its cheaper.
	// We also reset the flag. Flags are raised and done. reset is manual.

	// We reset fence to off
	vkResetFences(device, 1, &swapchain->inFlightFences_[swapchain->currentFrame_]);

	// We also reset the command list to start recording a new
	// Resetting entire pool is faster and better. I have split pools per frame so no biggie.
	//vkResetCommandBuffer(commandBuffers[currentFrame],
	//	/*VkCommandBufferResetFlagBits*/ 0);
	vkResetCommandPool(device, perFrameCommandPools[swapchain->currentFrame_], 0);


	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	if (vkBeginCommandBuffer(commandBuffers[swapchain->currentFrame_], &beginInfo) != VK_SUCCESS) {
		throw std::runtime_error("failed to begin recording command buffer!");
	}

	// Acquire right now returns a commandBuffer to record to frame correspoding swapchain->color Image
	VkImageMemoryBarrier barrierToColorAttachment{};
	barrierToColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrierToColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // or VK_IMAGE_LAYOUT_PRESENT_SRC_KHR if reusing
	barrierToColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrierToColorAttachment.srcAccessMask = 0;  // No access before
	barrierToColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	barrierToColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; // Wew manage just one queue so fuck it
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

	// ImgGui here adds ui commadns.
	//UiPass();

	auto color = swapchain->GetCurrentImageView();
	auto depth = swapchain->GetDepthImage();

	auto depthView = context->images_.get(depth)->renderView;

	VkRenderingAttachmentInfo depthAttachment{};
	depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
	depthAttachment.imageView = depthView;
	depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.clearValue.depthStencil = { 0.0f, 0 };

	VkRenderingAttachmentInfo colorAttachment{};
	colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
	colorAttachment.imageView = color;
	colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.clearValue = { {0.0f, 0.0f, 0.0f, 1.0f} };

	VkRenderingInfoKHR renderingInfo;
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
	renderingInfo.renderArea.offset = { 0, 0 };
	renderingInfo.renderArea.extent = swapchain->GetExtent();
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;

	renderingInfo.pColorAttachments = &colorAttachment;
	renderingInfo.pDepthAttachment = &depthAttachment;

	return ToyCommandBuffer{
		.commandBuffer = commandBuffers[swapchain->currentFrame_],
		.renderingInfo = renderingInfo
	};
}

void VulkanApp::Submit(ToyCommandBuffer& cmds) {

	//cmds.rendering_ = true;

	VkResult result;

	if (vkEndCommandBuffer(commandBuffers[swapchain->currentFrame_]) != VK_SUCCESS) {
		throw std::runtime_error("failed to record command buffer!");
	}
	// Submit Frame 
	{

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		VkSemaphore waitSemaphores[] = { swapchain->imageAvailableSemaphores_[swapchain->currentFrame_] };
		VkPipelineStageFlags waitStages[] = {
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;

		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffers[swapchain->currentFrame_];

		VkSemaphore signalSemaphores[] = { swapchain->renderFinishedSemaphores_[swapchain->currentFrame_] };
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		if (
			vkQueueSubmit(
				context->default_queue,
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

		result = vkQueuePresentKHR(presentQueue, &presentInfo);

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
			framebufferResized) {
			framebufferResized = false;
			swapchain->RecreateSwapChain();
		}
		else if (result != VK_SUCCESS) {
			throw std::runtime_error("failed to present swap chain image!");
		}


	}

	swapchain->AdvanceFrame();
	//currentFrame = (currentFrame + 1) % framesInFlight;
}


void VulkanApp::CleanUp() {

	context->CleanUp();
}

