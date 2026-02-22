#include <Engine/App/app.h>

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

	//IntializeShadowMaps();

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

	std::cout << "draw" << std::endl;

	// This is here to be as close as posible.
	UpdateDeltaTime();

#pragma region GPU_UPDATE

	// this is per frame scene state
	UpdateFrameUniforms(currentFrame);

	// Not reallly used right now is it
	//UpdateDrawDataBuffers(currentFrame);


	vkResetFences(device, 1, &inFlightFences[currentFrame]);

	// Resetting entire pool is faster and better. I have split pools per frame so no biggie.
	//vkResetCommandBuffer(commandBuffers[currentFrame],
	//	/*VkCommandBufferResetFlagBits*/ 0);
	vkResetCommandPool(device, perFrameCommandPools[currentFrame], 0);

	// These are bandwitth heavy because im not tracking what needs updating in any way...
	//UpdateLightsBuffers(currentFrame);
	//UpdateShadowMapDescriptorSets(currentFrame);
	//UpdateMainViewFrameData(currentFrame);
	//UpdateLightDescriptorSets(currentFrame);
	//UpdateBindlessTextures(currentFrame);

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

			assert(false && "failed to submit draw");
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

// Updates perframe camera view settings. view, proj, invView... etc.
void VulkanApp::UpdateFrameUniforms(uint32_t currentImage) {

	auto view = camera_.getViewMatrix();
	auto proj = camera_.getProjMatrix();
	proj[1][1] *= -1;

	auto viewProj = view * proj;

	FrameUniforms data{};
	data.cameraPos = cameraPositioner.cameraPosition_;

	data.view = view;
	data.proj = proj;
	data.viewProj = viewProj;

	data.invView = glm::inverse(view);
	data.invProj = glm::inverse(proj);
	data.invViewProj = glm::inverse(viewProj);

	data.time = 0.0f;
	data.viewportSize = { width_, height_ };
	data.invViewportSize = { 1 / width_, 1 / height_ };

	// Not implemented for now
	data.nearPlane = 0.f;
	data.farPlane = 0.f;
	data.exposure = 0.f;

	VkResult result{};
	VkCommandBuffer cmdBuf = beginSingleTimeCommands(device, perFrameCommandPools[currentImage]);

	context->UploadDataToBuffer(
		frameUniformsBinding.buffer_h,
		&data,
		sizeof(FrameUniforms)/* * 1*/,
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		0,
		frameUniformsBinding.dynamicOffsetFrame(currentImage)
	);

	endSingleTimeCommands(device, cmdBuf, context->default_queue, perFrameCommandPools[currentImage]);


}

// This is needed for instanced draws.
void VulkanApp::UpdateDrawDataBuffers(uint32_t currentImage) {

	std::array<DrawSetData, MAX_DRAWS> data;
	int i = 0;

	const auto& src = scene_.globalTransform;
	for (size_t i = 0, n = src.size(); i < n; ++i) {
		std::memcpy(data[i].transform, glm::value_ptr(src[i]), sizeof(float) * 16);
		data[i].color = {};
		//data[i].color = { float(i) * 0.20f, 0.3f, -float(i) * 0.7f, 1.0f };
	}

	drawDataBinding.uploadData(
		data,
		currentImage,
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT
	);
}

//void VulkanApp::UpdateLightsBuffers(uint32_t currentImage) {
//
//	VkCommandBuffer cmdBuf = beginSingleTimeCommands(device, perFrameCommandPools[currentImage]);
//
//	//for (auto spot : spot_lights) {
//
//	//	auto proj = GetPerspectiveProjectionForLight(lights[i]);
//	//	proj[1][1] *= -1;
//	//	lvp = proj * GetLightView(lights[i]);
//
//	//	std::memcpy(
//	//		lights[i].lightVP,
//	//		glm::value_ptr(lvp),
//	//		sizeof(float) * 16);
//	//}
//
//	//for (auto dirl : directional_lights)
//	//{
//
//	//	glm::mat4 lvp;
//
//	//	if (lights[i].type == LIGHT_TYPE_SPOT) {
//	//	}
//	//	else if (lights[i].type == LIGHT_TYPE_DIRECTIONAL) {
//	//		glm::mat4 proj, view;
//
//	//		auto sm = GetShadowMapFromShadowMapIndex(lights[i].shadow_map_index, currentFrame);
//	//		auto shadowMapImage = context->images_.get(sm.image);
//
//	//		GetDirectionalLightOrthoVP(
//	//			lights[i],
//	//			camera_.getViewMatrix(),
//	//			camera_.getProjMatrix(),
//	//			0.1f, 20.0f,
//	//			view, proj,
//	//			/*zPadding*/ 50.0f,
//	//			/*snap*/ true,
//	//			/*res*/ shadowMapImage->width
//	//		);
//
//	//		proj[1][1] *= -1;
//	//		lvp = proj * view;
//
//
//	//context->UploadDataToBuffer(
//	//	lightDataUniformBuffers[currentImage],
//	//	lights.data(),
//	//	sizeof(Light) * lights.size(),
//	//	VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
//	//	VK_ACCESS_SHADER_READ_BIT
//	//	//VK_ACCESS_UNIFORM_READ_BIT 
//
//	//);
//
//	endSingleTimeCommands(device, cmdBuf, context->default_queue, perFrameCommandPools[currentImage]);
//
//}

void VulkanApp::CleanUp() {

	context->CleanUp();

}

void VulkanApp::LoadScene() {

	camera_.setPerspective(45.0f, float(width_) / float(height_), default_near_clip, default_far_clip);

	// This will upload 
	Scene imported_scene;
	std::filesystem::path p(gltf_meshes_paths[0]);
	UploadGltfToContext(context, p, imported_scene);

	scene_ = imported_scene;

}

void VulkanApp::RecordMainViewCommmandsForFrame(VkCommandBuffer commandBuffer, uint32_t imageIndex) {

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

	MainViewPushConstants pc{};
	//pc.view = camera_.getViewMatrix(); 
	//pc.proj = camera_.getProjMatrix();
	//pc.proj[1][1] *= -1;

	pc.model = glm::mat4(1.0f);

	// I long ago did this to be able to run trough different pipeline setuops like transparency and else. Amay be wrong and dumb
	for (auto mat_pipe_entry : context->pipelines_.objects_) {

		//if (mat_pipe_entry.obj_.type != PIPELINE_MATERIAL) {
		//	continue;
		//}

		vkCmdBindPipeline(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			mat_pipe_entry.obj_.pipeline
		);

		auto dynamic_set = context->descriptorSets_.get(perFrameData[currentFrame].frameDynamicDescriptorSet);
		auto update_after_bind_set = context->descriptorSets_.get(perFrameData[currentFrame].frameDescriptorSet);

		auto pipe_lyt = mat_pipe_entry.obj_.plyt;
		//auto drawStride = sizeof(DrawSetData) * MAX_DRAWS;

		uint32_t dynamicOffsets[2] = {
			frameUniformsBinding.dynamicOffsetFrame(currentFrame),
			drawDataBinding.dynamicOffsetFrame(currentFrame),
		};

		VkDescriptorSet sets[2] = {
			dynamic_set->set,
			update_after_bind_set->set,
		};

		auto pipeline_layout = context->pipelineLayouts_.get(pipe_lyt);

		vkCmdBindDescriptorSets(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout->layout,
			0,
			2,
			sets,
			2,
			dynamicOffsets
		);

		for (int i = 0; i < scene_.hierarchy.size(); i++) {

			auto mesh_desc = context->meshes_.get(
				context->meshes_.getHandle(
					scene_.meshForNode[i]
				)
			);

			auto material_instance_index = scene_.materialForNode[i];
			pc.material_index = material_instance_index;

			MaterialPushConstants pcm =
				context->materialInstances_.objects_[material_instance_index].obj_.values;

			struct PC_All {
				MainViewPushConstants pc;               // 144
				MaterialPushConstants mpc;      // 48
			};
			PC_All all{ pc, pcm };

			vkCmdPushConstants(
				commandBuffer,
				pipeline_layout->layout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0, //offset to start of the range to push
				sizeof(PC_All), // size to push
				&all
			);

			for (auto& draw_set : mesh_desc->sets) {

				auto& model_ = scene_.globalTransform[i];
				vkCmdPushConstants(
					commandBuffer,
					pipeline_layout->layout,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					offsetof(MainViewPushConstants, model),
					sizeof(glm::mat4),
					glm::value_ptr(model_)
				);

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

void VulkanApp::CreateRenderSyncObjects() {

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

void VulkanApp::RecordCascadesForDirectionalLight(
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

		glm::mat4 view, proj;
		GetDirectionalLightOrthoVP(
			light,
			camera_.getViewMatrix(),
			camera_.getProjMatrix(),
			0.1f, 20.0f,
			view,
			proj,
			/*zPadding*/ 50.0f,
			/*snap*/ true,
			/*res*/ tileW
		);

		proj[1][1] *= -1;
		//vkCmdPushConstants(
		//	commandBuffer,
		//	pipe_lyt,
		//	VK_SHADER_STAGE_VERTEX_BIT,
		//	0,
		//	sizeof(MainViewPushConstants),
		//	&pc
		//);

		// PushDescriptorSet?


		//

		for (int node = 0; node < static_cast<int>(scene_.hierarchy.size()); ++node) {
			auto mesh_desc_handle = context->meshes_.getHandle(scene_.meshForNode[node]);
			auto mesh_desc = context->meshes_.get(mesh_desc_handle);
			for (auto& draw_set : mesh_desc->sets) {

				auto& model_ = scene_.globalTransform[node];
				vkCmdPushConstants(
					commandBuffer,
					pipe_lyt,
					VK_SHADER_STAGE_VERTEX_BIT,
					offsetof(MainViewPushConstants, model),
					sizeof(glm::mat4),
					glm::value_ptr(model_)
				);

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

void VulkanApp::RecordCommandsForFrameShadowMaps(VkCommandBuffer commandBuffer)
{
	//std::vector<SpotLight> spotLights;

	//for (auto& spot : spotLights) {

	//	int smi = light.shadow_map_index;
	//	if (smi < 0) continue;
	//	if (smi >= SHADOWMAP_SLOTS) continue; // or assert
	//	auto sm = GetShadowMapFromShadowMapIndex(smi, currentFrame);

	//	auto shadowMapImage = context->images_.get(sm.image);
	//	VkImageView shadowMapRenderView = *context->imageViews_.get(sm.renderView);

	//	VkExtent2D shadowExtent = { shadowMapImage->width,shadowMapImage->height };           // e.g. { width, height }

	//	{
	//		VkImageMemoryBarrier barrier{};
	//		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	//		//				barrier.oldLayout = used_image ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	//		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // I do loadOp clear so this can be undefined no problem
	//		barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
	//		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	//		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	//		barrier.image = shadowMapImage->image;
	//		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	//		barrier.subresourceRange.baseMipLevel = 0;
	//		barrier.subresourceRange.levelCount = 1;
	//		barrier.subresourceRange.baseArrayLayer = 0;     // or layer if using array
	//		barrier.subresourceRange.layerCount = 1;

	//		barrier.srcAccessMask = 0;                       // nothing before
	//		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	//		vkCmdPipelineBarrier(
	//			commandBuffer,
	//			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	//			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
	//			0,
	//			0, nullptr,
	//			0, nullptr,
	//			1, &barrier
	//		);
	//	}

	//	VkRenderingAttachmentInfoKHR depthAttachment{};
	//	depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
	//	depthAttachment.imageView = shadowMapRenderView;
	//	depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
	//	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	//	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // we want to sample this later

	//	VkClearValue clearDepth{};

	//	// Normal Z Depth, unlike primary view.
	//	clearDepth.depthStencil.depth = 1.0f;
	//	depthAttachment.clearValue = clearDepth;

	//	VkRenderingInfoKHR renderingInfo{};
	//	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
	//	renderingInfo.renderArea.offset = { 0, 0 };
	//	renderingInfo.renderArea.extent = shadowExtent;           // shadow map size
	//	renderingInfo.layerCount = 1;
	//	renderingInfo.colorAttachmentCount = 0;                      // depth-only
	//	renderingInfo.pColorAttachments = nullptr;
	//	renderingInfo.pDepthAttachment = &depthAttachment;
	//	renderingInfo.pStencilAttachment = nullptr;

	//	vkCmdBeginRendering(commandBuffer, &renderingInfo);

	//	VkViewport vp{};
	//	vp.x = 0.0f;
	//	vp.y = 0.0f;
	//	vp.width = static_cast<float>(shadowExtent.width);
	//	vp.height = static_cast<float>(shadowExtent.height);
	//	vp.minDepth = 0.0f;
	//	vp.maxDepth = 1.0f;
	//	vkCmdSetViewport(commandBuffer, 0, 1, &vp);

	//	// We should resize scissor to avoid unnecesary pixelinvokations
	//	VkRect2D scissor{};
	//	scissor.offset = { 0, 0 };
	//	scissor.extent = shadowExtent;
	//	vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

	//	BindVertex(commandBuffer);

	//	// Shadow Pipelien depthCompare is LESS_OR_EQUAL
	//	auto s_pipeline = context->pipelines_.get(default_shadow_pipeline);
	//	auto pipe_lyt = s_pipeline->plyt;

	//	vkCmdBindPipeline(
	//		commandBuffer,
	//		VK_PIPELINE_BIND_POINT_GRAPHICS,
	//		s_pipeline->pipeline
	//	);

	//	auto sets = context->descriptorSets_.get(shadowFrameDescriptorSets[currentFrame]);

	//	vkCmdBindDescriptorSets(
	//		commandBuffer,
	//		VK_PIPELINE_BIND_POINT_GRAPHICS,
	//		pipe_lyt,
	//		0,
	//		1,
	//		&sets->set,
	//		0,
	//		nullptr
	//	);

	//	if (light.type == LIGHT_TYPE_SPOT) {

	//		MainViewPushConstants pc{};
	//		pc.view = GetSpotLightView(light);
	//		pc.proj = GetPerspectiveProjectionForLight(light, /*aspect*/ 1.0f, /*near*/ 0.1f, /*far*/ 100.0f);
	//		pc.proj[1][1] *= -1;

	//		vkCmdPushConstants(
	//			commandBuffer,
	//			pipe_lyt,
	//			VK_SHADER_STAGE_VERTEX_BIT, // | VK_SHADER_STAGE_FRAGMENT_BIT if you want
	//			0,
	//			sizeof(MainViewPushConstants),
	//			&pc
	//		);

	//		for (int i = 0; i < static_cast<int>(scene_.hierarchy.size()); ++i) {
	//			auto mesh_desc_handle = context->meshes_.getHandle(scene_.meshForNode[i]);
	//			auto mesh_desc = context->meshes_.get(mesh_desc_handle);

	//			for (auto draw_set : mesh_desc->sets) {

	//				vkCmdDrawIndexed(
	//					commandBuffer,
	//					draw_set.index_count,
	//					1,
	//					draw_set.first_index,
	//					draw_set.base_vertex,
	//					0
	//				);

	//			}

	//		}




	//	else {
	//		assert(false && "Unsuported lightype");
	//	}

	//	vkCmdEndRendering(commandBuffer);

	//	{
	//		VkImageMemoryBarrier barrier{};
	//		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	//		barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
	//		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	//		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	//		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	//		barrier.image = shadowMapImage->image;
	//		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	//		barrier.subresourceRange.baseMipLevel = 0;
	//		barrier.subresourceRange.levelCount = 1;
	//		barrier.subresourceRange.baseArrayLayer = 0;
	//		barrier.subresourceRange.layerCount = 1;

	//		barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	//		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	//		vkCmdPipelineBarrier(
	//			commandBuffer,
	//			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	//			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	//			0,
	//			0, nullptr,
	//			0, nullptr,
	//			1, &barrier
	//		);

	//	}
	//	}


	//}
}

void VulkanApp::Start() {


	auto vertShaderCode = readFile("content/shaders/default.vert.hlsl.spv");
	auto fragShaderCode = readFile("content/shaders/default.frag.hlsl.spv");

	auto vert_shader_h = context->CreateShaderModule(
		vertShaderCode
	);
	auto fragHandle = context->CreateShaderModule(
		fragShaderCode
	);

	frameUniformsBinding.init(
		context,
		0, //bindding #
		MAX_FRAMES_IN_FLIGHT,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
	);

	drawDataBinding.init(
		context,
		1, //bindding #
		MAX_FRAMES_IN_FLIGHT,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, //bufferUsageFlags
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC //descriptorType
	);

	PipelineLayoutDesc desc;
	desc.sets = {
		DescriptorSetLayoutDesc{
			0, // set
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, // createFlags
			{
frameUniformsBinding.bindingDesc(),
drawDataBinding.bindingDesc(),

			}
		},
		DescriptorSetLayoutDesc{
			.set = 1,
			.createFlags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
			.bindings = {
			BindingDesc{
				.binding = 2,
				.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				.count = MAX_TEXTURES,
				.stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT,
				.flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
						  VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
				.immutableSamplers = {}
			},
		// Example bindless samplers
		BindingDesc{
			.binding = 3,
			.type = VK_DESCRIPTOR_TYPE_SAMPLER,
			.count = MAX_TEXTURES,
			.stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT,
			.flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
					  VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
			.immutableSamplers = {}
		}
		}
	}
	};
	desc.pushConstants.push_back({
		.stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = uint32_t(sizeof(MainViewPushConstants) +
						   sizeof(MaterialPushConstants)),
		});

	main_view_pipeline_h = context->CreatePipeline(
		// Raster settings
		{

			.depthClampEnable = VK_FALSE, // needs to be enabled
			.rasterizerDiscardEnable = VK_FALSE,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_BACK_BIT,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE
		},
		desc,
		vert_shader_h,
		fragHandle,
		findDepthFormat(physicalDevice), // THIS IS REALLY BAD. I WANT FIXED DEPTH FORMAT OR EARLY OUT ON DEVICE CRTEATION. lifted form vulkan tutorial
		swapChainImageFormat
	);
	auto pipe_obj = context->pipelines_.get(main_view_pipeline_h);
	auto aa = context->pipelineLayouts_.get(pipe_obj->plyt);
	auto bb = aa->setLayouts;

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

		perFrameData[i].frameDynamicDescriptorSet = context->createDescriptorSet(
			bb[0]
		);

		perFrameData[i].frameDescriptorSet = context->createDescriptorSet(
			bb[1]
		);

		//shadowFrameDescriptorSets[i] = context->createDescriptorSet(
		//	context->default_shadow_descriptorLayout
		//);
	}

	SetDescriptorSets();

	// APP
	LoadScene();
	UpdateBindlessTexturesSets();

}
