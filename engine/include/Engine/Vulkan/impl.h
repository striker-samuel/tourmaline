#pragma once

#include <Engine/Config/config.h>
#include <Engine/smvk.h>

namespace smvk {

	// Just VK_1_3 for the moment, should be more than ok
	// https://asawicki.info/articles/state_of_gpu_hardware_2025.php 
	enum VulkanVersion {
		VK_1_3
		//, VK_1_4
	};

	struct VulkanCore {
		static VulkanCore& Get()
		{
			static VulkanCore instance;
			return instance;
		}

		struct CoreConfiguration {
			VulkanVersion version_ = VK_1_3;
			const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation", };
			const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
			const std::vector<const char*> instanceExtensions = {};
		} config_;

		VkPhysicalDeviceDriverProperties vkPhysicalDeviceDriverProperties_ = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
		nullptr
		};

		VkPhysicalDeviceDepthStencilResolveProperties vkPhysicalDeviceDepthStencilResolveProperties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
			&vkPhysicalDeviceDriverProperties_
		};

		//VkPhysicalDeviceVulkan14Properties vkPhysicalDeviceVulkan14Properties_ = {
		//VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES,
		//&vkPhysicalDeviceDepthStencilResolveProperties_
		//};

		VkPhysicalDeviceVulkan13Properties vkPhysicalDeviceVulkan13Properties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
			&vkPhysicalDeviceDepthStencilResolveProperties_
		};

		VkPhysicalDeviceVulkan12Properties vkPhysicalDeviceVulkan12Properties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
			&vkPhysicalDeviceVulkan13Properties_
		};

		VkPhysicalDeviceVulkan11Properties vkPhysicalDeviceVulkan11Properties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
			&vkPhysicalDeviceVulkan12Properties_
		};

		VkPhysicalDeviceProperties2 vkPhysicalDeviceProperties2_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			&vkPhysicalDeviceVulkan11Properties_,
			{}
		};
		VkPhysicalDeviceProperties2& GetPhysicalDeviceProperties() { return this->vkPhysicalDeviceProperties2_; };

		//VkPhysicalDeviceVulkan14Features vkFeatures14_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
		VkPhysicalDeviceVulkan13Features vkFeatures13_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceVulkan12Features vkFeatures12_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &vkFeatures13_ };
		VkPhysicalDeviceVulkan11Features vkFeatures11_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &vkFeatures12_ };
		VkPhysicalDeviceFeatures2 vkFeatures10_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vkFeatures11_ };

		// dont like this data definition for queue exposure.
		QueueFamilyIndices indices;

	public:

		VulkanCore(CoreConfiguration config = {}) :config_(config) {};
		~VulkanCore();
		VulkanCore(const VulkanCore&) = delete;
		VulkanCore& operator=(const VulkanCore&) = delete;
		VulkanCore(VulkanCore&&) = delete;
		VulkanCore& operator=(VulkanCore&&) = delete;

		int CreateInstance();
		bool Init();

		//VkInstance GetInstance();
		VkDevice GetDevice();
		//VkPhysicalDevice GetPhysicalDevice();
		//VkQueue GetGraphicsQueue();

		void CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler);
		void CreateImageView(ImageViewCreateInfo imageViewCreateInfo, VkImageView& view);

		void CreateGraphicsPipeline(VkGraphicsPipelineCreateInfo createInfo, VkPipeline& pipeline);
		void CreateComputePipeline(VkComputePipelineCreateInfo createInfo, VkPipeline& pipeline);

		void DestroyPipeline(VkPipeline pipeline);
	};

	std::unique_ptr<smvk::VulkanCore> CreateVulkanCoreWithWindow(smvk::VulkanCore::CoreConfiguration config, GLFWwindow* window);
	//std::unique_ptr<smvk::VulkanCore> CreateVulkanCoreHeadless(smvk::VulkanCore::CoreConfiguration config);


	//
	struct CommandSubmitHandle {
		uint32_t index_ = 0;
		uint32_t gen_ = 0; // gen 0 means no slot. alas empty. 
		bool empty() const { return gen_ == 0; };
	};
	struct Submitter {
		struct SemaphoreState {
			VkSemaphore semaphore_ = VK_NULL_HANDLE;
			uint64_t initialValue_ = 0;
			uint64_t goal_ = 1; //defautl to 1
		};
		//
		struct CommandBuffer {
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			SemaphoreState finishedSignalSemaphore{};
			bool isEncoding_ = false;
		};
		struct WrapperSlot {
			CommandBuffer wrapper_{};
			VkCommandBuffer allocatedCmd_ = VK_NULL_HANDLE;
			uint32_t gen_ = 1;
			//uint32_t index = UINT32_MAX;  // load balancing gens sorting and using this
		};
		Submitter(VulkanCore& core, uint32_t queueIndex);
		~Submitter();
		CommandSubmitHandle acquire(uint32_t goal = 1);
		CommandBuffer& GetCommandBuffer(CommandSubmitHandle handle);
		void submit(CommandSubmitHandle handle);
		bool validHandle(CommandSubmitHandle handle) const;
		bool isReady(CommandSubmitHandle handle);
		void wait(CommandSubmitHandle handle, uint64_t timeout = UINT64_MAX);
		void waitAll();
		void purge();
	private:
		void reset(CommandSubmitHandle handle);
		VulkanCore& core_;
		VkCommandPool pool_;
		VkQueue m_queue = VK_NULL_HANDLE;
		static const uint32_t kMax = 12;
		uint32_t availableSlots = kMax;
		WrapperSlot commandBuffers_[kMax];
		CommandSubmitHandle m_lastSubmit{}; // not in use right now. used for creating linkedList for a faux dependency chain

		VkSemaphore m_timelineSemaphore = VK_NULL_HANDLE;
		uint64_t m_timelineValue = 1;
	};
	struct DeferredTask {
		DeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle) : task_(std::move(task)), handle_(handle) {}
		std::packaged_task<void()> task_;
		CommandSubmitHandle handle_;
	};


	struct VulkanResourceManager {

		//static VulkanResourceManager& Get()
		//{
		//	static VulkanResourceManager instance{ VulkanCore::Get() };
		//	// doesnt call initialize, maybe dangerous.
		//	return instance;
		//}

		VulkanResourceManager(VulkanCore& core);
		~VulkanResourceManager();
		VulkanResourceManager(const VulkanResourceManager&) = delete;
		VulkanResourceManager& operator=(const VulkanResourceManager&) = delete;
		VulkanResourceManager(VulkanResourceManager&&) = delete;
		VulkanResourceManager& operator=(VulkanResourceManager&&) = delete;

		int Initialize();
		// 	
		void AddDeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle = CommandSubmitHandle{});
		void ProcessDeferrredTasks();
		//
		BufferHandle CreateStagingBuffer(VkDeviceSize size, const char* debugName = "staging"); // Right now only used for image upload, every other data upload is done by mapping
		Holder<ImageHandle> CreateImage(ImageCreateInfo imageCreateInfo, const void* data = nullptr, const char* debugName = nullptr);
		//Holder<TextureHandle> CreateTexture(ImageCreateInfo imageCreateInfo, const void* data = nullptr, const char* debugName = nullptr); // specialization for textures instead of .isTexture property
		Holder<TextureHandle> CreateTexture(ktxTexture2& texture, const char* debugName = nullptr);
		Holder<BufferHandle> CreateBuffer(BufferCreateInfo bufferCreateInfo, const char* debugName = nullptr);
		Holder<ShaderModuleHandle> CreateShaderModule(std::vector<char> code, const char* debugName = nullptr);
		Holder<DescriptorPoolHandle> CreateDescriptorPool(const char* debugName = nullptr);

		void* MapBuffer(smvk::Handle<smvk::Buffer> handle);
		void UnmapBuffer(smvk::Handle<smvk::Buffer> handle);

		void UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);
		void UploadDataToImage2D(Image img, const void* data, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);

		void UpdateBindings(uint32_t frameIndex = 0);

		// VulkanImpl
		VulkanCore& core_;
		VmaAllocator m_allocator;
		Submitter* m_submitter = nullptr;
		std::vector<DeferredTask> m_deferredTasks;

		//  Binding Model
		VkDescriptorSetLayout defaultSetLayout{};
		VkPipelineLayout defaultPipeLayout{};
		VkDescriptorSet defaultDescriptorSets_[MAX_FRAMES_IN_FLIGHT];

		// Resources
		Pool<Image, Image> images_; // VkSwapchainImages are not represented with Image type and just owned by the swapchain
		Pool<Texture, Image> textures_; // specializaed textures away from images
		Pool<Buffer, Buffer> buffers_;
		Pool<Sampler, VkSampler> samplers_;

		Pool<DescriptorPool, VkDescriptorPool> descriptorPools_; // Maybe we should do away with this as we only manage one pool, and one for Imgui
		Pool<ShaderModule, VkShaderModule> shaderModules_; // this should be removed when moving to vk1.4

		// Static vertex geometry heaps. These are permanent-lifetime
		// resources owned by this manager and torn down in
		// ~VulkanResourceManager (freeing the buffers + VMA virtual blocks
		// before the allocator/pools are destroyed). Population is gated by
		// SMVK_STATIC_VERTEX_HEAPS at the allocate call site; when disabled
		// the handles stay empty and teardown is a no-op.
		BufferHandle m_staticVertexPosBuffer{};
		BufferHandle m_staticVertexShadingBuffer{};
		BufferHandle m_staticIndexBuffer{};
		VmaVirtualBlock m_staticVertexBlock = VK_NULL_HANDLE;
		VmaVirtualBlock m_staticIndexBlock = VK_NULL_HANDLE;

		// DEFAULTS
		//TextureHandle m_nilTexture{};
		VkSampler m_defaultSampler{};
		Holder<DescriptorPoolHandle> defaultDescriptorPool;
		//Holder<DescriptorPoolHandle> imguiDescriptorPool;

		mutable bool needToUpdateDescriptors = true;
		//mutable bool needToUpdateTextures = true; //maybe disambiguation is better here?

	};

	void destroy(VulkanResourceManager* ctx, ImageHandle handle);
	void destroy(VulkanResourceManager* ctx, TextureHandle handle);
	void destroy(VulkanResourceManager* ctx, BufferHandle handle);
	void destroy(VulkanResourceManager* ctx, SamplerHandle handle);

	void destroy(VulkanResourceManager* ctx, DescriptorPoolHandle handle);
	void destroy(VulkanResourceManager* ctx, ShaderModuleHandle handle); // will be removed in 1.4

};


inline void BufferBarrier(
	VkCommandBuffer cmd,
	VkBuffer buffer,
	VkDeviceSize offset,
	VkDeviceSize size,
	VkPipelineStageFlags2 srcStage,
	VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage,
	VkAccessFlags2 dstAccess
);

inline void ImageBarrier(
	VkCommandBuffer cmd,
	VkImage vk_image,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	VkImageSubresourceRange subresourceRange,
	VkPipelineStageFlags2 srcStage,
	VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage,
	VkAccessFlags2 dstAccess,
	uint32_t srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	uint32_t dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED
);


namespace smvk {

	//struct SamplerPool {
	//	struct SamplerPoolCreateInfo {
	//		VkDevice device;
	//	};
	//	SamplerPool(SamplerPoolCreateInfo createInfo);
	//	~SamplerPool();
	//	VkDevice device_;
	//	using SamplerKey = SamplerCreateInfo;
	//	std::map<SamplerKey, VkSampler> m_samplers;
	//	VkSampler GetSampler(SamplerKey key);
	//};

	class VulkanSwapchain;
	struct FrameBuffer {
		uint32_t frameIndex = 0;
		uint32_t imageIndex = 0;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VulkanSwapchain& swapchain;
	};

	class VulkanSwapchain {

	public:

		VulkanSwapchain(
			VulkanResourceManager& context
			, GLFWwindow* window
		);
		VulkanSwapchain(const VulkanSwapchain&) = delete;
		VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;
		VulkanSwapchain(VulkanSwapchain&&) = delete;
		VulkanSwapchain& operator=(VulkanSwapchain&&) = delete;
		~VulkanSwapchain();

		void Create() {
			CreateSwapchain();
			CreateImageViews();
			CreateDepthResources();
			CreateSyncObjects();
		}
		void Recreate();

		VkExtent2D GetExtent() const { return extent_; }
		uint32_t GetImageCount() const { return static_cast<uint32_t>(images_.size()); }
		uint32_t GetMinImageCount() const { return minImageCount_; };
		VkImage GetImage(uint32_t index) const { return images_.at(index); }
		VkImageView GetImageView(uint32_t index) const { return imageViews_.at(index); }
		VkImage GetCurrentImage() const { return images_.at(currentImageIndex_); }
		VkImageView GetCurrentImageView() const { return imageViews_.at(currentImageIndex_); }
		uint32_t GetCurrentImageIndex() const { return currentImageIndex_; }
		uint32_t GetCurrentFrameIndex() const { return currentFrame_; }
		VkSemaphore GetImageAvailableSemaphore() const { return imageAvailableSemaphores_.at(currentFrame_); }
		VkFence GetInFlightFence() const { return inFlightFences_.at(currentFrame_); }
		VkSemaphore GetRenderFinishedSemaphore(uint32_t imageIndex) const { return renderFinishedSemaphores_.at(imageIndex); }
		// 

		void AdvanceFrame() { currentFrame_ = (currentFrame_ + 1) % framesInFlight_; }
		void SetCurrentImageIndex(uint32_t imageIndex) { currentImageIndex_ = imageIndex; }

		//private:
		VulkanResourceManager& ctx_;
		GLFWwindow* window_ = nullptr;
		VkSwapchainKHR vk_swapchain_ = VK_NULL_HANDLE;
		VkFormat vk_surfaceFormat_ = VK_FORMAT_UNDEFINED;
		VkFormat vk_depthFormat_ = VK_FORMAT_UNDEFINED;

#pragma region ===[ Frame Commands List ]===
		VkCommandPool perFrameCommandPools[MAX_FRAMES_IN_FLIGHT]{};
		VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT]{};
#pragma endregion

		bool framebufferResized = false;
		uint32_t minImageCount_;

		Holder<ImageHandle> depthImage_;
		std::vector<VkImage> images_;
		std::vector<VkImageView> imageViews_;
		std::vector<VkSemaphore> renderFinishedSemaphores_;
		std::vector<VkSemaphore> imageAvailableSemaphores_;
		std::vector<VkFence> inFlightFences_;
		std::vector<VkFence> imagesInFlight_;

		VkExtent2D extent_{};
		int windowWidth = 0;
		int windowHeight = 0;
		uint32_t framesInFlight_ = 0;
		uint32_t desiredImageCount_ = DESIRED_SWAPCHAIN_IMAGE_COUNT;
		uint32_t currentImageIndex_ = 0;
		uint32_t currentFrame_ = 0;

		void CreateSwapchain();
		void CreateImageViews();
		void CreateDepthResources();
		void CreateSyncObjects();
		void DestroySyncObjects();
		void CleanupSwapchain();

		FrameBuffer AcquireFramebuffer();
		void EndAndPresentFramebuffer(FrameBuffer& cb);
	};

	struct PipelineBundle {
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
	};

	struct PipelineKey {

		ShaderProgram& program;
		std::vector<VkVertexInputBindingDescription> bindingDescriptions{};
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions{};
		std::vector<VkFormat> colorFormats;
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
		VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
		VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
		VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		bool operator==(const PipelineKey& other) const noexcept
		{
			return program == other.program &&
				colorFormats == other.colorFormats &&
				depthFormat == other.depthFormat &&
				stencilFormat == other.stencilFormat &&
				polygonMode == other.polygonMode &&
				cullMode == other.cullMode &&
				frontFace == other.frontFace;
		}
	};

	struct PipelineKeyHash {
		static void HashCombine(std::size_t& seed, std::size_t value) noexcept
		{
			// Standard-ish hash combine.
			seed ^= value + 0x9e3779b97f4a7c15ull +
				(seed << 6) + (seed >> 2);
		}

		template<typename T>
		static void HashValue(std::size_t& seed, const T& value) noexcept
		{
			HashCombine(seed, std::hash<T>{}(value));
		}

		std::size_t operator()(const PipelineKey& key) const noexcept
		{
			std::size_t seed = 0;

			HashValue(seed, key.program.shaderCount);

			for (size_t i = 0; i < key.program.shaderCount; ++i)
			{
				const Shader& shader = key.program.shaders[i];

				HashValue(seed, shader.vkShaderModule.index());
				HashValue(seed, static_cast<uint32_t>(shader.stage));
				HashValue(seed, std::string_view(shader.entryPoint));
			}
			HashValue(seed, key.colorFormats.size());
			for (VkFormat format : key.colorFormats) {
				HashValue(seed, static_cast<uint32_t>(format));
			}

			HashValue(seed, static_cast<uint32_t>(key.depthFormat));
			HashValue(seed, static_cast<uint32_t>(key.stencilFormat));
			HashValue(seed, static_cast<uint32_t>(key.polygonMode));
			HashValue(seed, static_cast<uint32_t>(key.cullMode));
			HashValue(seed, static_cast<uint32_t>(key.frontFace));

			return seed;
		}
	};

	std::vector<VkVertexInputBindingDescription> getVkVertexInputBindingDescriptions(const VertexFormatDescription& format);
	std::vector<VkVertexInputAttributeDescription> getVkVertexAttributeDescriptions(const VertexFormatDescription& format);

	class PipelineCache {
	public:
		explicit PipelineCache(VulkanResourceManager* ctx)
			: ctx_(ctx)
		{}

		~PipelineCache()
		{
			Clear();
		}

		PipelineBundle GetRenderPipeline(
			ShaderProgram& program,
			VertexFormatDescription vertexFormat,
			const VkFormat* colorFormats,
			uint32_t nColorAttachments,
			VkFormat depthFormat,
			VkFormat stencilFormat,
			VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
			VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT,
			VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE)
		{
			PipelineKey key{
				.program = program,
				.bindingDescriptions = getVkVertexInputBindingDescriptions(vertexFormat),
				.attributeDescriptions = getVkVertexAttributeDescriptions(vertexFormat),
				.colorFormats = std::vector<VkFormat>(
					colorFormats,
					colorFormats + nColorAttachments),
				.depthFormat = depthFormat,
				.stencilFormat = stencilFormat,
				.polygonMode = polygonMode,
				.cullMode = cullMode,
				.frontFace = frontFace,
			};

			if (auto it = pipelines_.find(key); it != pipelines_.end()) {
				return it->second;
			}

			PipelineBundle bundle = BuildGraphicsPipeline(key);

			auto [it, inserted] =
				pipelines_.emplace(std::move(key), bundle);

			return it->second;
		}

		void Clear()
		{
			//vkDeviceWaitIdle(ctx_->core_.GetDevice()); //Dont like this
			for (auto& [key, bundle] : pipelines_) {
				if (bundle.pipeline != VK_NULL_HANDLE) {
					ctx_->core_.DestroyPipeline(bundle.pipeline);
				}
			}
			pipelines_.clear();
		}

	private:
		PipelineBundle BuildGraphicsPipeline(const PipelineKey& key)
		{
			std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
			shaderStages.reserve(key.program.shaderCount);

			for (size_t i = 0; i < key.program.shaderCount; ++i)
			{
				const Shader& shader = key.program.shaders[i];

				auto shaderModule = ctx_->shaderModules_.get(shader.vkShaderModule);

				VkPipelineShaderStageCreateInfo stageInfo{};
				stageInfo.sType =
					VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;

				stageInfo.stage = shader.stage;
				stageInfo.module = *shaderModule;
				stageInfo.pName = shader.entryPoint;

				shaderStages.push_back(stageInfo);
			}

			// ---------------------------------------------------------
			// VERTEX INPUT
			// ---------------------------------------------------------

			auto bindingDescriptions = key.bindingDescriptions;
			auto attributeDescriptions = key.attributeDescriptions;
			VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
			vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescriptions.size());
			vertexInputInfo.pVertexBindingDescriptions = bindingDescriptions.data();
			vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
			vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

			// ---------------------------------------------------------
			// INPUT ASSEMBLY
			// ---------------------------------------------------------

			VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
			inputAssembly.sType =
				VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
			inputAssembly.topology =
				VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			inputAssembly.primitiveRestartEnable =
				VK_FALSE;

			// ---------------------------------------------------------
			// VIEWPORT
			// ---------------------------------------------------------

			VkPipelineViewportStateCreateInfo viewportState{};
			viewportState.sType =
				VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			viewportState.viewportCount = 1;
			viewportState.scissorCount = 1;

			// ---------------------------------------------------------
			// RASTERIZATION
			// ---------------------------------------------------------

			VkPipelineRasterizationStateCreateInfo rasterizationState{
				.sType =
					VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,

				.pNext = nullptr,
				.flags = 0,

				.depthClampEnable = VK_FALSE,
				.rasterizerDiscardEnable = VK_FALSE,

				.polygonMode = key.polygonMode,
				.cullMode = key.cullMode,
				.frontFace = key.frontFace,

				.depthBiasEnable = VK_FALSE,
				.depthBiasConstantFactor = 0.0f,
				.depthBiasClamp = 0.0f,
				.depthBiasSlopeFactor = 0.0f,

				.lineWidth = 1.0f,
			};

			// ---------------------------------------------------------
			// MULTISAMPLING
			// ---------------------------------------------------------

			VkPipelineMultisampleStateCreateInfo multisampling{};
			multisampling.sType =
				VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
			multisampling.sampleShadingEnable = VK_FALSE;
			multisampling.rasterizationSamples =
				VK_SAMPLE_COUNT_1_BIT;

			// ---------------------------------------------------------
			// DEPTH / STENCIL
			// ---------------------------------------------------------

			VkPipelineDepthStencilStateCreateInfo depthStencil{};
			depthStencil.sType =
				VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			depthStencil.depthTestEnable =
				key.depthFormat != VK_FORMAT_UNDEFINED;
			depthStencil.depthWriteEnable =
				key.depthFormat != VK_FORMAT_UNDEFINED;
			depthStencil.depthCompareOp =
				VK_COMPARE_OP_GREATER;
			depthStencil.depthBoundsTestEnable =
				VK_FALSE;
			depthStencil.stencilTestEnable =
				VK_FALSE;

			// ---------------------------------------------------------
			// COLOR BLENDING
			// ---------------------------------------------------------

			VkPipelineColorBlendAttachmentState colorBlendAttachment{};
			colorBlendAttachment.colorWriteMask =
				VK_COLOR_COMPONENT_R_BIT |
				VK_COLOR_COMPONENT_G_BIT |
				VK_COLOR_COMPONENT_B_BIT |
				VK_COLOR_COMPONENT_A_BIT;

			colorBlendAttachment.blendEnable =
				VK_FALSE;

			// One attachment state per color target.
			std::vector<VkPipelineColorBlendAttachmentState>
				colorBlendAttachments(
					key.colorFormats.size(),
					colorBlendAttachment);

			VkPipelineColorBlendStateCreateInfo colorBlending{};
			colorBlending.sType =
				VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

			colorBlending.logicOpEnable = VK_FALSE;
			colorBlending.logicOp = VK_LOGIC_OP_COPY;

			colorBlending.attachmentCount =
				static_cast<uint32_t>(colorBlendAttachments.size());

			colorBlending.pAttachments =
				colorBlendAttachments.data();

			// ---------------------------------------------------------
			// DYNAMIC STATE
			// ---------------------------------------------------------

			constexpr VkDynamicState dynamicStates[] = {
				VK_DYNAMIC_STATE_VIEWPORT,
				VK_DYNAMIC_STATE_SCISSOR,
				//VK_DYNAMIC_STATE_DEPTH_BOUNDS,
				//VK_DYNAMIC_STATE_DEPTH_BIAS
			};

			VkPipelineDynamicStateCreateInfo dynamicState{};
			dynamicState.sType =
				VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;

			dynamicState.dynamicStateCount =
				static_cast<uint32_t>(std::size(dynamicStates));

			dynamicState.pDynamicStates =
				dynamicStates;

			// ---------------------------------------------------------
			// PIPELINE LAYOUT
			// ---------------------------------------------------------

			VkPipelineLayout layout =
				ctx_->defaultPipeLayout;

			// ---------------------------------------------------------
			// DYNAMIC RENDERING
			// ---------------------------------------------------------

			VkPipelineRenderingCreateInfo renderingInfo{};
			renderingInfo.sType =
				VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;

			renderingInfo.colorAttachmentCount =
				static_cast<uint32_t>(key.colorFormats.size());

			renderingInfo.pColorAttachmentFormats =
				key.colorFormats.data();

			renderingInfo.depthAttachmentFormat =
				key.depthFormat;

			renderingInfo.stencilAttachmentFormat =
				key.stencilFormat;

			// ---------------------------------------------------------
			// GRAPHICS PIPELINE
			// ---------------------------------------------------------

			VkGraphicsPipelineCreateInfo pipelineInfo{};
			pipelineInfo.sType =
				VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;

			pipelineInfo.pNext =
				&renderingInfo;

			pipelineInfo.stageCount =
				static_cast<uint32_t>(std::size(shaderStages));

			pipelineInfo.pStages =
				shaderStages.data();

			pipelineInfo.pVertexInputState =
				&vertexInputInfo;

			pipelineInfo.pInputAssemblyState =
				&inputAssembly;

			pipelineInfo.pViewportState =
				&viewportState;

			pipelineInfo.pRasterizationState =
				&rasterizationState;

			pipelineInfo.pMultisampleState =
				&multisampling;

			pipelineInfo.pDepthStencilState =
				&depthStencil;

			pipelineInfo.pColorBlendState =
				&colorBlending;

			pipelineInfo.pDynamicState =
				&dynamicState;

			pipelineInfo.layout =
				layout;

			PipelineBundle bundle{};

			ctx_->core_.CreateGraphicsPipeline(
				pipelineInfo,
				bundle.pipeline);

			bundle.layout =
				layout;

			return bundle;
		}

	private:
		VulkanResourceManager* ctx_ = nullptr;

		std::unordered_map<
			PipelineKey,
			PipelineBundle,
			PipelineKeyHash
		> pipelines_;
	};

} // namespace smvk


