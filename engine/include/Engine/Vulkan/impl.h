#pragma once

#include <Engine/smvk.h>

#include "volk.h"
#include "vk_mem_alloc.h"

#include <ktx.h>
#include <ktxVulkan.h>

#include <string_view>
#include <unordered_map>


#define VK_ASSERT_RETURN(func)                                     \
  {                                                                \
    const VkResult vk_assert_result = func;                        \
    if (vk_assert_result != VK_SUCCESS) {                          \
      LLOGW("Vulkan API call failed: %s:%i\n  %s\n  %s\n", \
                    __FILE__,                                      \
                    __LINE__,                                      \
                    #func,                                         \
                    lvk::getVulkanResultString(vk_assert_result)); \
      assert(false);                                               \
      return getResultFromVkResult(vk_assert_result);              \
    }                                                              \
  }

#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = (x);                                             \
        if (err != VK_SUCCESS) {                                        \
            fprintf(stderr, "[Vulkan Error] %s:%d: %s\n",               \
                    __FILE__, __LINE__, vkResultToString(err));         \
            abort();                                                    \
        }                                                               \
    } while (0)


namespace smvk {

	/*constexpr*/ bool FormatHasStencil(VkFormat format);
	/*constexpr*/ bool FormatHasDepth(VkFormat format);

	// ============================================================================
	// Type translators: convert RHI types from smvk.h to Vk types for pipeline state
	// ============================================================================

	constexpr VkImageUsageFlags toVkImageUsageFlags(ImageUsage usage);
	constexpr VkImageType toVkImageType(ImageType type);
	constexpr VkImageViewType toVkImageViewType(ImageViewType viewType);
	constexpr VkClearValue toVkClearValue(ClearColor color);
	constexpr VkClearValue toVkClearValue(ClearDepthStencil clearDepth);
	constexpr VkAttachmentLoadOp  toVkAttachmentLoadOp(AttachmentLoadOp op);
	constexpr VkAttachmentStoreOp toVkAttachmentStoreOp(AttachmentStoreOp op);
	constexpr VkStencilOp toVkStencilOp(uint8_t op);
	constexpr VkShaderStageFlagBits toVkShaderStageBit(ShaderStage stage);
	constexpr VkShaderStageFlags toVkShaderStageFlags(ShaderStage stages);
	constexpr VkBlendFactor toVkSrcBlendFactor(BlendAttachmentState::SrcColorBlendFactor factor);
	constexpr VkBlendFactor toVkDstBlendFactor(BlendAttachmentState::DstColorBlendFactor factor);
	constexpr VkBlendOp toVkBlendOp(BlendAttachmentState::ColorBlendOp op);
	constexpr VkBlendOp toVkBlendOp(BlendAttachmentState::AlphaBlendOp op);
	constexpr VkColorComponentFlags toVkColorWriteMask(uint8_t mask);
	constexpr VkPrimitiveTopology toVkPrimitiveTopology(PrimitiveTopology topology);
	constexpr VkPolygonMode toVkPolygonMode(PolygonMode mode);
	constexpr VkCullModeFlags toVkCullMode(CullMode mode);
	constexpr VkFrontFace toVkFrontFace(FrontFace face);
	constexpr VkSampleCountFlagBits toVkSampleCount(SampleCount samples);
	constexpr VkCompareOp toVkCompareOp(CompareOp op);
	constexpr VkFormat toVkFormat(Format format);
	constexpr VkBlendFactor toVkBlendFactor(BlendAttachmentState::SrcColorBlendFactor factor);
	constexpr VkBlendFactor toVkBlendFactor(BlendAttachmentState::DstColorBlendFactor factor);
	constexpr uint8_t toRhiColorWriteMask(VkColorComponentFlags flags);
	constexpr VkColorSpaceKHR ToVkColorSpace(ColorSpace colorSpace);
	constexpr VkImageAspectFlags toVkImageAspectFlags(ImageAspect aspect);
	// ============================================================================
	// 	Core
	// ============================================================================

	// Just VK_1_3 for the moment, should be more than ok
	// https://asawicki.info/articles/state_of_gpu_hardware_2025.php 
	enum VulkanVersion {
		VK_1_3
		//, VK_1_4
	};

	struct VulkanCore {

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
		void CreateGraphicsPipeline(VkGraphicsPipelineCreateInfo createInfo, VkPipeline& pipeline);
		void CreateComputePipeline(VkComputePipelineCreateInfo createInfo, VkPipeline& pipeline);

		void DestroyGraphicsPipeline(VkPipeline pipeline);
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
		struct CommandBuffer { // duplicated from rendergraph. commandBuffer
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
		Holder<ImageHandle> CreateImage(ImageCreateInfo imageCreateInfo, const void* data = nullptr);
		Holder<TextureHandle> CreateTexture(ImageCreateInfo imageCreateInfo, const void* data = nullptr); // specialization for textures instead of .isTexture property
		Holder<TextureHandle> CreateTexture(ktxTexture2& texture, const char* debugName = nullptr);
		Holder<BindlessImageHandle> CreateBindlessImage(ImageViewHandle viewHandle, VkImageLayout readLayout);
		Holder<ImageViewHandle> CreateImageView(ImageHandle image, const ImageViewCreateInfo& createInfo);
		Holder<BufferHandle> CreateBuffer(VkDeviceSize size, BufferType bufferType, const char* debugName = nullptr);
		Holder<DescriptorPoolHandle> CreateDescriptorPool(std::vector<VkDescriptorPoolSize> poolSizes, const char* debugName = nullptr);

		Holder<ShaderModuleHandle> CreateShaderModule(std::vector<char> code, const char* debugName = nullptr);

		void* MapBuffer(smvk::Handle<smvk::Buffer> handle);
		void UnmapBuffer(smvk::Handle<smvk::Buffer> handle);

		void UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);
		void UploadDataToImage2D(const Image& image, const void* data, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset);
		//void UploadDataToImage2D(Image img, const void* data, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);


		void UpdateBindings(uint32_t frameIndex = 0);
		uint32_t GetBindlessIndex(BindlessImageHandle handle) const;

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
		Pool<Image, Image> images_;
		Pool<Texture, Texture> textures_;
		Pool<Buffer, Buffer> buffers_;
		Pool<ImageView, ImageView> imageViews_;

		// Descriptors
		Pool<Sampler, VkSampler> samplers_;
		Pool<BindlessImage, BindlessImage> bindlessImages_;
		Pool<DescriptorPool, VkDescriptorPool> descriptorPools_;
		Pool<ShaderModule, VkShaderModule> shaderModules_; // this should be removed when moving to vk1.4

		//StaticVertexFmt
		BufferHandle m_staticVertexPosBuffer{};
		BufferHandle m_staticVertexShadingBuffer{};
		BufferHandle m_staticIndexBuffer{};
		VmaVirtualBlock m_staticVertexBlock = VK_NULL_HANDLE;
		VmaVirtualBlock m_staticIndexBlock = VK_NULL_HANDLE;

		// DEFAULTS
		VkSampler m_defaultSampler{};
		VkSampler m_defaultShadowSampler{};

		Holder<DescriptorPoolHandle> defaultDescriptorPool{};
		Holder<TextureHandle> m_nilTexture{};
		//Holder<DescriptorPoolHandle> imguiDescriptorPool;

		mutable bool needToUpdateDescriptors = true;
		//mutable bool needToUpdateTextures = true; //maybe disambiguation is better here?


	private:
		ImageSubresourceRange ResolveImageSubresourceRange(const Image& image, ImageSubresourceRange range) const;
		Image CreateImageImpl(const ImageCreateInfo& ci);
	};

	void destroy(VulkanResourceManager* ctx, ImageHandle handle);
	void destroy(VulkanResourceManager* ctx, TextureHandle handle);
	void destroy(VulkanResourceManager* ctx, BufferHandle handle);
	void destroy(VulkanResourceManager* ctx, SamplerHandle handle);
	void destroy(VulkanResourceManager* ctx, BindlessImageHandle handle);
	void destroy(VulkanResourceManager* ctx, ImageViewHandle handle);
	void destroy(VulkanResourceManager* ctx, DescriptorPoolHandle handle);

	void destroy(VulkanResourceManager* ctx, ShaderModuleHandle handle); // will be removed in 1.4. transition

};


namespace smvk {
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

		Format surfaceFormat{};
		Format depthFormat{};

#pragma region ===[ Frame Commands List ]===
		VkCommandPool perFrameCommandPools[MAX_FRAMES_IN_FLIGHT]{};
		VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT]{};
#pragma endregion

		bool framebufferResized = false;
		uint32_t minImageCount_;

		Holder<ImageHandle> depthImage_;
		Holder<ImageViewHandle> depthImageView_;
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

	struct PipelineKeyHash {
		static void HashCombine(std::size_t& seed, std::size_t value) noexcept
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
		}

		template<typename T>
		static void HashValue(std::size_t& seed, const T& value) noexcept
		{
			HashCombine(seed, std::hash<T>{}(value));
		}

		template<typename E>
		static void HashEnum(std::size_t& seed, E value) noexcept
		{
			HashValue(seed, static_cast<std::underlying_type_t<E>>(value));
		}

		std::size_t operator()(const PipelineKey& key) const noexcept
		{
			std::size_t seed = 0;

			HashValue(seed, key.program.shaderCount);
			for (size_t i = 0; i < key.program.shaderCount; ++i) {
				const Shader& shader = key.program.shaders[i];
				HashValue(seed, shader.module.index());
				HashValue(seed, static_cast<uint32_t>(shader.stage));
				HashValue(seed, std::string_view(shader.entryPoint));
			}

			HashValue(seed, key.vertexFormat.bindings.size());
			for (const VertexBinding& binding : key.vertexFormat.bindings) {
				HashValue(seed, binding.binding);
				HashValue(seed, binding.stride);
				HashEnum(seed, binding.inputRate);
				HashValue(seed, binding.attributes.size());
				for (const VertexAttribute& attribute : binding.attributes) {
					HashEnum(seed, attribute.semantic);
					HashEnum(seed, attribute.format);
					HashValue(seed, attribute.offset);
				}
			}

			HashValue(seed, key.colorFormats.size());
			for (Format format : key.colorFormats) HashEnum(seed, format);
			HashEnum(seed, key.depthFormat);
			HashEnum(seed, key.stencilFormat);
			HashEnum(seed, key.topology);

			HashEnum(seed, key.raster.polygonMode);
			HashEnum(seed, key.raster.cullMode);
			HashEnum(seed, key.raster.frontFace);
			HashValue(seed, key.raster.depthClampEnable);
			HashValue(seed, key.raster.rasterizerDiscardEnable);
			HashValue(seed, key.raster.lineWidth);

			HashEnum(seed, key.depth.depthFunc);
			HashValue(seed, key.depth.writeEnable);
			HashValue(seed, key.depth.depthTestEnable);
			HashValue(seed, key.depth.depthBiasEnable);
			HashValue(seed, key.depth.depthBiasConstantFactor);
			HashValue(seed, key.depth.depthBiasClamp);
			HashValue(seed, key.depth.depthBiasSlopeFactor);

			HashEnum(seed, key.stencil.frontCompareOp);
			HashEnum(seed, key.stencil.backCompareOp);
			HashValue(seed, key.stencil.frontFailOp);
			HashValue(seed, key.stencil.backFailOp);
			HashValue(seed, key.stencil.frontPassOp);
			HashValue(seed, key.stencil.backPassOp);
			HashValue(seed, key.stencil.frontFace);
			HashValue(seed, key.stencil.backFace);
			HashValue(seed, key.stencil.frontTestEnable);
			HashValue(seed, key.stencil.backTestEnable);
			HashValue(seed, key.stencil.frontWriteEnable);
			HashValue(seed, key.stencil.backWriteEnable);

			HashValue(seed, key.blend.blendState.blendEnable);
			for (const BlendAttachmentState& a : key.blend.blendState.attachment) {
				HashEnum(seed, a.srcColorBlendFactor);
				HashEnum(seed, a.dstColorBlendFactor);
				HashEnum(seed, a.colorBlendOp);
				HashEnum(seed, a.alphaBlendOp);
				HashValue(seed, a.colorWriteMask);
			}

			HashEnum(seed, key.msaa.samples);
			return seed;
		}
	};

	class PipelineCache
	{
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
			const VertexFormatDescription& vertexFormat,
			const Format* colorFormats,
			uint32_t colorAttachmentCount,
			Format depthFormat = Format::Undefined,
			Format stencilFormat = Format::Undefined,
			PrimitiveTopology topology = PrimitiveTopology::TriangleList,
			const RasterSettings& raster = {},
			const DepthSettings& depth = DepthSettings::DepthTest(),
			const StencilSettings& stencil = StencilSettings::DefaultDisabled(),
			const BlendSettings& blend = BlendSettings::Opaque(),
			const MSAASettings& msaa = MSAASettings::Count1())
		{
			PipelineKey key{
				.program = program,
				.vertexFormat = vertexFormat,
				.colorFormats = std::vector<Format>(
					colorFormats,
					colorFormats + colorAttachmentCount),
				.depthFormat = depthFormat,
				.stencilFormat = stencilFormat,
				.topology = topology,
				.raster = raster,
				.depth = depth,
				.stencil = stencil,
				.blend = blend,
				.msaa = msaa,
			};

			if (auto it = pipelines_.find(key);
				it != pipelines_.end())
			{
				return it->second;
			}

			PipelineBundle bundle =
				BuildGraphicsPipeline(key);

			auto [it, inserted] =
				pipelines_.emplace(
					std::move(key),
					bundle);

			return it->second;
		}

		void Clear()
		{
			for (auto& [key, bundle] : pipelines_)
			{
				if (bundle.pipeline != VK_NULL_HANDLE)
				{
					ctx_->core_.DestroyGraphicsPipeline(
						bundle.pipeline);
				}
			}

			pipelines_.clear();
		}

	private:
		PipelineBundle BuildGraphicsPipeline(
			const PipelineKey& key);

	private:
		VulkanResourceManager* ctx_ = nullptr;

		std::unordered_map<
			PipelineKey,
			PipelineBundle,
			PipelineKeyHash
		> pipelines_;
	};


	std::vector<VkVertexInputBindingDescription> getVkVertexInputBindingDescriptions(const VertexFormatDescription& format);
	std::vector<VkVertexInputAttributeDescription> getVkVertexAttributeDescriptions(const VertexFormatDescription& format);



} // namespace smvk

