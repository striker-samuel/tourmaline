#pragma once

#include <cassert>
#include <array>
#include <tuple>
#include <map>
#include <future>
#include <algorithm>
#include <optional>
#include <vector>
#include <set>
//
#include "volk.h"
#include "vk_mem_alloc.h"
//
#include "Engine/Shared/common.h"
#include <Engine/Memory/pool.h>
//
//#define USE_KTX // lets code with this on by default
#ifdef USE_KTX
#include <ktx.h>
#include <ktxvulkan.h>
#endif
// 
#define ENGINE_NAME "smvk"
//
static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;
//
static constexpr uint32_t DEFAULT_WIDTH = 1280;
static constexpr uint32_t DEFAULT_HEIGHT = 720;
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t DESIRED_SWAPCHAIN_IMAGE_COUNT = 2;
static constexpr uint32_t MAX_BUFFERS = 1024;
static constexpr uint32_t MAX_LIGHTS = 4096;
static constexpr uint32_t MAX_TEXTURES = 4096;


inline const char* vkResultToString(VkResult result);

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
	struct QueueFamilyIndices {
		std::optional<uint32_t> graphicsFamily;
		std::optional<uint32_t> presentFamily;
		std::optional<uint32_t> transferFamily;

		bool isComplete() {
			return graphicsFamily.has_value() && presentFamily.has_value();
		}
	};

	struct SwapChainSupportDetails {
		VkSurfaceCapabilitiesKHR capabilities{};
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> presentModes;
	};
};
// CreatePayloads//Keys for logicalReources
namespace smvk {
	struct BufferCreateInfo {
		VkDeviceSize size;
		VmaMemoryUsage vma_usage;
		VkBufferUsageFlags usage_flags;
	};
	struct ImageCreateInfo {
		int texWidth;
		int texHeight;
		VkFormat vk_format;
		VkImageUsageFlags usage;
		uint32_t levels;
		uint32_t arrayLayers;
		bool operator<(const ImageCreateInfo& other) const {
			return std::tie(texWidth, texHeight, vk_format, usage, levels, arrayLayers) <
				std::tie(other.texWidth, other.texHeight, other.vk_format,
					other.usage, other.levels, other.arrayLayers);
		}
	};
	struct ImageViewCreateInfo {
		VkImageViewCreateFlags     flags{};
		VkImage                    vk_image{};
		VkImageViewType            viewType{};
		VkFormat                   vk_format{};
		VkImageSubresourceRange    subResourceRange{};
	};
	struct SamplerCreateInfo {
		VkFilter mag = VK_FILTER_LINEAR;
		VkFilter min = VK_FILTER_LINEAR;
		VkSamplerMipmapMode mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		VkSamplerAddressMode u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		VkSamplerAddressMode v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		VkSamplerAddressMode w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		float maxLod = 1000.0f;
		float maxAniso = 1.0f;
		VkCompareOp compareOp = VK_COMPARE_OP_ALWAYS;
		bool compareEnable = false;
	};
	void CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler);
	void CreateImageView(ImageViewCreateInfo imageViewCreateInfo, VkImageView& view);
};

// maybe this should be impl.cpp exclusive
namespace smvk {

	struct Pipeline {
		VkPipeline pipeline;
		VkPipelineLayout layout;
	};

	struct Image {
		VkImage vk_image = VK_NULL_HANDLE;
		VmaAllocation vma_alloc = VK_NULL_HANDLE;
		VkFormat vk_format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags vk_usageFlags = 0;
		VkExtent3D extent{};
		uint32_t arrayLayers = 0;
		uint32_t levels = 0;
		VkImageView sampleView = VK_NULL_HANDLE; // you can sample multiple levels
		VkImageView renderView = VK_NULL_HANDLE; // but can only render to one. Views are created with this baked in. Thats the why to separate them
		VkImageView arrayView[6]{
			VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
			VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE
		};
		// not fully implemented yet.
		VkSampler sampler_ = VK_NULL_HANDLE;
		bool owns_sampler = false;
		bool owns_views = true;
		bool is_swapchain = false;
	};

	// Just piggybacking on vma really
	struct Buffer {
		VkBuffer vk_buffer = VK_NULL_HANDLE;
		VmaAllocation vma_alloc{};
		// VmaAllocationInfo allocInfo{};
		// this is just an info return. the info contained maybe altered during refrag so makes no sense storing it
		VkBufferUsageFlags usage;
		VkMemoryPropertyFlags memory_flags;
		VkDeviceAddress deviceAdress = 0;
		uint64_t deviceAdress;
		bool is_mapable() const {
			return this->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		}
		//bool operator()(const Buffer& a, const Buffer& b) const {
		//	return std::tie(a.vk_buffer, a.vma_alloc, a.usage, a.memory_flags)
		//		< std::tie(b.vk_buffer, b.vma_alloc, b.usage, b.memory_flags);
		//}
	};
};



namespace smvk {

	// TODO:: Rework this
	VkQueue GetGraphicsQueue(); // really hacky Only needed becuse app.cpp has presetn code and needs it
	VkQueue GetPresentQueue(); // really hacky Only needed becuse app.cpp has presetn code and needs it


	// Just VK_1_3 for the moment, should be more than ok
	// https://asawicki.info/articles/state_of_gpu_hardware_2025.php 
	enum VulkanVersion {
		VK_1_3
	};
	enum struct QueuePreference {
		GRAPHICS,
		TRANSFER,
		COMPUTE
	};

	struct DeferredTask {
		DeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle) : task_(std::move(task)), handle_(handle) {}
		std::packaged_task<void()> task_;
		CommandSubmitHandle handle_;
	};
	//
	using ImageHandle = Handle<struct Image>; // Attachments
	using TextureHandle = Handle<struct Image>;
	using SamplerHanlde = Handle<struct Sampler>;
	using BufferHandle = Handle<struct Buffer>;
	using ShaderModuleHandle = Handle<struct ShaderModule>;
	// Maybe specializing it here its a good idea
	using GraphicsPipelineHandle = Handle<struct Pipeline>;

	//using DescriptorPoolHandle = Handle<struct DescriptorPool>;
	//using DescriptorSetHandle = Handle<struct DescriptorSet>;

	struct VulkanCore {
		struct CoreConfiguration {
			VulkanVersion version_ = VK_1_3;
			const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation", };
			const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
			const std::vector<const char*> instanceExtensions = {};
		} config_;

		//VkSurfaceKHR surface_;

		// Enviroment Descriptions
		VkPhysicalDeviceDriverProperties vkPhysicalDeviceDriverProperties_ = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, nullptr };
		VkPhysicalDeviceDepthStencilResolveProperties vkPhysicalDeviceDepthStencilResolveProperties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
			&vkPhysicalDeviceVulkan12Properties_,
		};
		VkPhysicalDeviceVulkan13Properties vkPhysicalDeviceVulkan13Properties_ = {
			 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
			 &vkPhysicalDeviceDriverProperties_,
		};
		// provided by Vulkan 1.2
		VkPhysicalDeviceVulkan12Properties vkPhysicalDeviceVulkan12Properties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
			&vkPhysicalDeviceVulkan13Properties_,
		};
		// provided by Vulkan 1.1
		VkPhysicalDeviceVulkan11Properties vkPhysicalDeviceVulkan11Properties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
			&vkPhysicalDeviceDepthStencilResolveProperties_,
		};
		VkPhysicalDeviceProperties2 vkPhysicalDeviceProperties2_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			&vkPhysicalDeviceVulkan11Properties_,
			VkPhysicalDeviceProperties{},
		};
		VkPhysicalDeviceProperties2& GetPhysicalDeviceProperties() { return this->vkPhysicalDeviceProperties2_; };

		// missing 1.4 .. dont care yet
		VkPhysicalDeviceVulkan13Features vkFeatures13_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceVulkan12Features vkFeatures12_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &vkFeatures13_ };
		VkPhysicalDeviceVulkan11Features vkFeatures11_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &vkFeatures12_ };
		VkPhysicalDeviceFeatures2 vkFeatures10_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vkFeatures11_ };
		// 

	public:

		VulkanCore(CoreConfiguration config = {}) :config_(config) {};
		VulkanCore(const VulkanCore&) = delete;
		VulkanCore& operator=(const VulkanCore&) = delete;
		VulkanCore(VulkanCore&&) = delete;
		VulkanCore& operator=(VulkanCore&&) = delete;

		int CreateInstance();
		bool Init();
		//
		void CreateFence(const VkFenceCreateInfo createInfo, VkFence& out);
		void CreateSemaphore(VkSemaphoreCreateInfo createInfo, VkSemaphore& out);
		void CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler);
		void CreateImageView(ImageViewCreateInfo imageViewCreateInfo, VkImageView& view);
		void CreateShaderModule(const std::vector<char>& code, VkShaderModule& out);
		void CreateImageView(VkImage vk_image, VkFormat vk_format, VkImageAspectFlags aspectFlags, int levelCount, int baseArrayLayer, int layerCount, VkImageViewType viewType, VkImageView& view);
		void CreateCommandPool(QueuePreference queue, VkCommandPoolCreateFlags flags, VkCommandPool& out);
		//void CreateDescriptorSet();
		//void CreateDescriptorPool();
		//
		//VkInstance& GetInstance();
		VkCommandPool GetCommandPool(QueuePreference preferredQueue);
	};

	std::unique_ptr<smvk::VulkanCore> CreateVulkanCoreWithWindow(smvk::VulkanCore::CoreConfiguration config, GLFWwindow* window);

	struct CommandBufferManager {
		struct TimelineSemaphore {
			VkSemaphore semaphore_ = VK_NULL_HANDLE;
			uint64_t initialValue_ = 0;
			uint64_t goal_ = 1; //defautl to 1
		};
		//
		struct CommandBuffer {
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			TimelineSemaphore finishedSignalSemaphore{};
			bool isEncoding_ = false;
			//QueuePreference queue_ = QueuePreference::GRAPHICS; // Dont know yet how to properly resolve this. will come with real use
		};
		struct WrapperSlot {
			CommandBuffer wrapper_{};
			VkCommandBuffer allocatedCmd_;
			uint32_t gen_ = 1;
		};
		//
		CommandBufferManager(QueuePreference queue);;
		~CommandBufferManager();
		[[nodiscard]] CommandSubmitHandle acquire(uint32_t goal = 1);;
		[[nodiscard]] CommandBuffer& GetCommandBuffer(CommandSubmitHandle handle);;
		void submit(CommandSubmitHandle handle);;
		bool validHandle(CommandSubmitHandle handle) const;
		bool isReady(CommandSubmitHandle handle);
		void wait(CommandSubmitHandle handle, uint64_t timeout = UINT64_MAX);
		void waitAll();
		void purge();
	private:
		void reset(CommandSubmitHandle handle);;
		VulkanCore& core_;
		static const uint32_t kMax = 64;
		uint32_t availableSlots = kMax;
		WrapperSlot commandBuffers_[kMax];
		CommandSubmitHandle m_lastSubmit{}; // not in use right now. used for creating linkedList for a faux dependency chain
		//QueuePreference queue_;
	};
	struct CommandSubmitHandle {
		uint32_t index_ = 0;
		uint32_t gen_ = 0; // gen 0 means no slot. alas empty. 
		bool empty() { return gen_ == 0; };
	};

	struct PipelineCreateInfo {
		struct RasterSettings {
			uint32_t depthClampEnable = VK_FALSE;
			uint32_t rasterizerDiscardEnable = VK_FALSE;
			VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
			float lineWidth = 1.0f;
			uint32_t cullMode = VK_CULL_MODE_BACK_BIT;
			VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			uint32_t depthBiasEnable = VK_FALSE;
			float depthBiasConstant = 0.0f;
			float depthBiasSlope = 1.0f; // This should be PCF radius https://mynameismjp.wordpress.com/2013/09/10/shadow-maps/
		};

		//Handle vert_;
		//Handle frag_;

		//struct BindingDescription {
		//	uint32_t binding = 0; // If we want sparse bindings we need this
		//	VkDescriptorType type{};
		//	uint32_t count = 1;
		//	VkShaderStageFlags stages = 0;
		//	VkDescriptorBindingFlags flags = 0;
		//	std::vector<VkSampler> immutableSamplers;
		//};
		//struct DescriptorSetDescription {
		//	VkDescriptorSetLayoutCreateFlags createFlags = 0; // e.g. UPDATE_AFTER_BIND_POOL_BIT
		//	std::vector<BindingDescription> bindings;
		//};
		//struct PushConstantDescription {
		//	VkShaderStageFlags stages = 0;
		//	uint32_t offset = 0;
		//	uint32_t size = 0;
		//};
		//struct PipelineLayoutDesc {
		//	bool valid = 0;
		//	std::vector<DescriptorSetDescription> setDescriptions; // each has set index
		//	std::vector<PushConstantDescription> pushConstants;
		//};
		//PipelineLayoutDesc layoutDescription{};
		RasterSettings rasterSettings{};
	};

	// 4.6gb is good amount to reserver for 8gb vram system
	// 2gb for Windows is good practice. And a little headroom for aux apps
	struct Context {
#ifdef USE_KTX
		ktxVulkanDeviceInfo ktxInfo_;
		const ktxVulkanDeviceInfo GetKtxInfo() { return ktxInfo_; };
		Holder<TextureHandle> CreateTexture(
			ktxTexture2& ktxTexture
		);;
#endif
		Context(VulkanCore& core);
		~Context();
		Context(const Context&) = delete;
		Context& operator=(const Context&) = delete;
		Context(Context&&) = delete;
		Context& operator=(Context&&) = delete;

		int Initialize();
		void AddDeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle = CommandSubmitHandle{});
		void ProcessDeferrredTasks(); //void Flush();
		//
		BufferHandle CreateStagingBuffer(VkDeviceSize size);
		//
		Holder<ImageHandle> CreateImage(ImageCreateInfo imageCreateInfo, const void* data = nullptr);
		Holder<TextureHandle> CreateTexture(ImageCreateInfo imageCreateInfo, const void* data = nullptr); // specialization for textures instead of .isTexture property
		Holder<BufferHandle> CreateBuffer(BufferCreateInfo bufferCreateInfo);
		Holder<ShaderModuleHandle> CreateShaderModule(std::vector<char> code);
		//

		//void UploadDataToImage(ImageHandle image, const void* data) {};
		void* MapBuffer(smvk::Handle<smvk::Buffer> handle);
		void UnmapBuffer(smvk::Handle<smvk::Buffer> handle);
		void UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags dstStages, VkAccessFlags dstAccess, size_t src_offset = 0, size_t dest_offset = 0);
		void CleanUp();
#ifdef USE_KTX
		void GetKtxDeviceInfo();
#endif
		//private:
		Pool<Image, Image> images_;
		Pool<Buffer, Buffer> buffers_;
		Pool<ShaderModule, VkShaderModule> shaderModules_;
		//
		VkDescriptorPool descriptorPool_;
		VkDescriptorSet descriptorSets_[MAX_FRAMES_IN_FLIGHT];
		//
		VulkanCore& core_;
		CommandBufferManager m_submitter;
		VmaAllocator m_allocator;
		VmaPool m_imagesPool;
		VmaPool m_stagingPool;
		std::vector<DeferredTask> m_deferredTasks;
		//
		TextureHandle m_nilTexture{};
		SamplerHandle m_defaultSampler{}; // maybe shoudl be handle
	};

	void destroy(Context* ctx, ImageHandle handle);
	void destroy(Context* ctx, BufferHandle handle);
	void destroy(Context* ctx, ShaderModuleHandle handle);
	//void destroy(Context* ctx, DescriptorPolHandle handle);

	//
	struct ShaderProgram {
		Context& ctx_;
		ShaderProgram(Context& ctx) : ctx_(ctx) {};
		~ShaderProgram() {};
		//
		ShaderModuleHandle vert_{};
		ShaderModuleHandle frag_{};
		// Pass will give us this
		PipelineCreateInfo::RasterSettings rasterSettings_{};
		//PipelineCreateInfo::PipelineLayoutDesc pipelineLayoutDescription_{};
		// Graph will give us this
		VkPipelineRenderingCreateInfo dynamicRenderingInfo_;
		//std::map<PipelineCreateInfo, Holder<PipelineHandle>> pipelineCache_;
	public:
		ShaderProgram& setDynamicRenderingInfo(VkPipelineRenderingCreateInfo dynamicRenderingInfo) { this->dynamicRenderingInfo_ = dynamicRenderingInfo; };
		ShaderProgram& setLayoutDescription();
		ShaderProgram& setVert(ShaderModuleHandle vert) { this->vert_ = Holder<ShaderModuleHandle>(&ctx_, vert); return *this; };
		ShaderProgram& setFrag(ShaderModuleHandle frag) { this->frag_ = Holder<ShaderModuleHandle>(&ctx_, frag); return *this; };
		VkPipeline Build() {
			PipelineCreateInfo info;
			info.rasterSettings = rasterSettings_;
			//info.vert = vert_;
			//info.frag = frag_;
			//info.layoutDescription;
		};
	};
	//VkCommandPool GetCommandPool(QueuePreference preferredQueue);
};


// In reality these should be added automatically by the rg
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

// this covers image transitions as well sycn2 mandted.
inline void ImageBarrier(
	VkCommandBuffer cmd,
	VkImage vk_image,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	VkImageSubresourceRange subresourceRange,
	VkPipelineStageFlags2 srcStage,
	VkAccessFlags2 srcAccess,
	VkPipelineStageFlags2 dstStage,
	VkAccessFlags2 dstAccess
);


namespace smvk {

	class VulkanSwapchain {

	public:

		VulkanSwapchain(
			Context& context,
			GLFWwindow* window,
			uint32_t framesInFlight,
			uint32_t desiredImageCount
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

		// 
		VkFormat GetImageFormat() const { return surfaceFormat_; }
		VkFormat GetDepthImageFormat() const { return depthFormat_; }
		VkExtent2D GetExtent() const { return extent_; }
		uint32_t GetImageCount() const { return static_cast<uint32_t>(images_.size()); }
		VkImage GetImage(uint32_t index) const { return images_.at(index); }
		VkImageView GetImageView(uint32_t index) const { return imageViews_.at(index); }
		VkImage GetCurrentImage() const { return images_.at(currentImageIndex_); }
		VkImageView GetCurrentImageView() const { return imageViews_.at(currentImageIndex_); }
		uint32_t GetCurrentImageIndex() const { return currentImageIndex_; }
		uint32_t GetCurrentFrameIndex() const { return currentFrame_; }
		VkSemaphore GetImageAvailableSemaphore() const { return imageAvailableSemaphores_.at(currentFrame_); }
		VkFence GetInFlightFence() const { return inFlightFences_.at(currentFrame_); }
		VkSemaphore GetRenderFinishedSemaphore(uint32_t imageIndex) const { return renderFinishedSemaphores_.at(imageIndex); }
		ImageHandle GetDepthImage() const { return depthImage_; }
		// 

		void AdvanceFrame() { currentFrame_ = (currentFrame_ + 1) % framesInFlight_; }
		void SetCurrentImageIndex(uint32_t imageIndex) { currentImageIndex_ = imageIndex; }

	private:
		Context& ctx_;
		GLFWwindow* window_ = nullptr;
		VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;

		Holder<ImageHandle> depthImage_{};
		std::vector<VkImage> images_;
		// these are external images that cannnot be represented by our Image obj
		std::vector<VkImageView> imageViews_;
		std::vector<VkSemaphore> renderFinishedSemaphores_;
		std::vector<VkSemaphore> imageAvailableSemaphores_;
		std::vector<VkFence> inFlightFences_;

		VkFormat surfaceFormat_ = VK_FORMAT_UNDEFINED;
		VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;

		VkExtent2D extent_{};
		int windowWidth = 0;
		int windowHeight = 0;
		uint32_t framesInFlight_ = 0;
		uint32_t desiredImageCount_ = 0;
		uint32_t currentImageIndex_ = 0;
		uint32_t currentFrame_ = 0;

		void CreateSwapchain();
		void CreateImageViews();
		void CreateDepthResources();
		void CreateSyncObjects();
		void DestroySyncObjects();
		void CleanupSwapchain();
	};

};
