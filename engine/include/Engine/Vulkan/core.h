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
#define ENGINE_NAME "smvk"

#include <ktx.h>
#include <ktxVulkan.h>
//
//
static constexpr uint32_t DEFAULT_WIDTH = 1280;
static constexpr uint32_t DEFAULT_HEIGHT = 720;
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t DESIRED_SWAPCHAIN_IMAGE_COUNT = 2;
static constexpr uint32_t MAX_BUFFERS = 1024;
static constexpr uint32_t MAX_LIGHTS = 4096;
static constexpr uint32_t MAX_TEXTURES = 4096;
static constexpr uint32_t MAX_SAMPLERS = 8;
static constexpr uint32_t MAX_COLOR_ATTACHEMNTS = 4096;


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

	// Predegined buffer usages so we have flags preset.
	// Theres no need for infinite granularity so clearly this will be the best approach when ready
	//enum BufferTypeBits : uint8_t {
	//  BufferUsageBits_Index = 1 << 0,
	//  BufferUsageBits_Vertex = 1 << 1,
	//  BufferUsageBits_Uniform = 1 << 2,
	//  BufferUsageBits_Storage = 1 << 3,
	//  BufferUsageBits_Indirect = 1 << 4,
	//  // ray tracing
	//  BufferUsageBits_ShaderBindingTable = 1 << 5,
	//  BufferUsageBits_AccelStructBuildInputReadOnly = 1 << 6,
	//  BufferUsageBits_AccelStructStorage = 1 << 7
	//};

	struct BufferCreateInfo {
		VkDeviceSize size;
		VmaMemoryUsage vma_usage;
		VkBufferUsageFlags usage_flags;
		const char* name;
		//void* data;
		//uint8_t usage = 0;
	};
	struct ImageCreateInfo {

		VkSampleCountFlags vk_samples = VK_SAMPLE_COUNT_1_BIT;
		VkExtent3D vk_extent;
		VkFormat vk_format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		uint32_t levels = 1;
		uint32_t arrayLayers = 1;
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
		bool operator<(const SamplerCreateInfo& other) const noexcept {
			return std::tie(
				mag, min, mip,
				u, v, w,
				maxLod, maxAniso,
				compareOp, compareEnable
			) < std::tie(
				other.mag, other.min, other.mip,
				other.u, other.v, other.w,
				other.maxLod, other.maxAniso,
				other.compareOp, other.compareEnable
			);
		}
	};
};

namespace smvk {
	struct Image {
		VkImage vk_image = VK_NULL_HANDLE;
		VmaAllocation vma_alloc = VK_NULL_HANDLE;
		//ImageCreateInfo createInfo;
		VkExtent3D vk_extent = { 1,1,1 };
		VkFormat vk_format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		uint32_t levels = 1;
		uint32_t arrayLayers = 1;
		VkSampleCountFlags vk_samples = VK_SAMPLE_COUNT_1_BIT;
		VkImageView sampleView = VK_NULL_HANDLE; // you can sample multiple levels
		VkImageView storageView = VK_NULL_HANDLE; // but can only render to one. Views are created with this baked in. Thats the why to separate them
		VkImageView arrayView[6]{
			VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
			VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE
		}; // for cubemap
		VkSampler vk_sampler = VK_NULL_HANDLE; // Not owning
		bool owns_views = true;
		bool is_swapchain = false;
		ImageCreateInfo getImageCreateInfo() const {
			return {
				.vk_extent = vk_extent,
				.vk_format = vk_format,
				.vk_usage = vk_usage,
				.levels = levels,
				.arrayLayers = arrayLayers
			};
		};
	};

	// Just piggybacking on vma really
	struct Buffer {
		VkBuffer vk_buffer = VK_NULL_HANDLE;
		VmaAllocation vma_alloc{};
		VkBufferUsageFlags usage = 0;
		VkMemoryPropertyFlags memory_flags = 0;
		VkDeviceAddress deviceAddress = 0;
		[[nodiscard]] bool is_mappable() const noexcept {
			return (memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
		}
		//[[nodiscard]] bool is_valid() const noexcept {
		//	return vk_buffer != VK_NULL_HANDLE && vma_alloc != nullptr;
		//}
	};



	using ImageHandle = Handle<struct Image>;
	using TextureHandle = Handle<struct Texture>;
	using SamplerHandle = Handle<struct Sampler>;
	using BufferHandle = Handle<struct Buffer>;
	using ShaderModuleHandle = Handle<struct ShaderModule>;
	using DescriptorPoolHandle = Handle<struct DescriptorPool>;
	using DescriptorSetHandle = Handle<struct DescriptorSet>;

	using RenderPipelineHandle = Handle<struct RenderPipeline>;
};

namespace smvk {

	struct RenderPipelineDesc {

		// This will be hardcoded in build for now.
		// INPUT ASS
		//Topology topology = Topology_Triangle;
		//lvk::VertexInput vertexInput;

		ShaderModuleHandle smVert;
		ShaderModuleHandle smFrag;

		// lifted from lvk but not implemented yey
		//ShaderModuleHandle smTesc;
		//ShaderModuleHandle smTese;
		//ShaderModuleHandle smGeom;
		//ShaderModuleHandle smTask;
		//ShaderModuleHandle smMesh;

		// Same, not supported yet
		//SpecializationConstantDesc specInfo = {};

		const char* entryPointVert = "main";
		const char* entryPointFrag = "main";

		struct ColorAttachemnt {
			VkFormat vkColorFormat = VK_FORMAT_UNDEFINED;
			bool blend = false;
			//BlendState blendState;
		} color[6]; // hardcoded for max # of color attachments. 6 for cubemap

		// TARGET
		//ColorAttachment color[LVK_MAX_COLOR_ATTACHMENTS] = {};
		VkFormat vkDepthFormat = VK_FORMAT_UNDEFINED; // maybe max enum for default better?
		VkFormat vkStencilFormat = VK_FORMAT_UNDEFINED; // maybe max enum for default better?
		//Format depthFormat = Format_Invalid;
		//Format stencilFormat = Format_Invalid;
		VkCullModeFlags vkCullMode = VK_CULL_MODE_BACK_BIT;
		VkFrontFace vkFrontFace = VK_FRONT_FACE_CLOCKWISE;
		VkPolygonMode vkPolygonMode = VK_POLYGON_MODE_FILL;
		// Stencil not yet implemented.
		//StencilState backFaceStencil = {};
		//StencilState frontFaceStencil = {};
		// TES
		//uint32_t patchControlPoints = 0;
		// MSAA
		uint32_t samplesCount = 1u;
		float minSampleShading = 0.0f;
		const char* debugName = "";

		bool operator<(const RenderPipelineDesc& other) const noexcept {
		}
	};

	struct RenderPipelineState {
		RenderPipelineDesc description{};
		VkPipeline vkPipeline = VK_NULL_HANDLE;
		VkPipelineLayout vkLayout = VK_NULL_HANDLE;
		bool operator<(const RenderPipelineState& other) const noexcept {
		    return description < other.description;
		}
	};

	//struct PipelineCreateInfo {
	//	struct BindingDescription {
	//		uint32_t binding = 0; // If we want sparse bindings we need this
	//		VkDescriptorType type{};
	//		uint32_t count = 1;
	//		VkShaderStageFlags stages = 0;
	//		VkDescriptorBindingFlags flags = 0;
	//		std::vector<VkSampler> immutableSamplers;
	//	};
	//	struct DescriptorSetDescription {
	//		VkDescriptorSetLayoutCreateFlags createFlags = 0; // e.g. UPDATE_AFTER_BIND_POOL_BIT
	//		std::vector<BindingDescription> bindings;
	//	};
	//};
};



namespace smvk {

	// Just VK_1_3 for the moment, should be more than ok
	// https://asawicki.info/articles/state_of_gpu_hardware_2025.php 
	enum VulkanVersion {
		VK_1_3
	};

	// This was badly solved.
	enum struct QueuePreference {
		GRAPHICS,
		TRANSFER,
		COMPUTE
	};


	struct VulkanCore {
		struct CoreConfiguration {
			VulkanVersion version_ = VK_1_3;
			const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation", };
			const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
			const std::vector<const char*> instanceExtensions = {};
		} config_;
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
		//VkPhysicalDeviceVulkan14Features vkFeatures14_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
		VkPhysicalDeviceVulkan13Features vkFeatures13_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceVulkan12Features vkFeatures12_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &vkFeatures13_ };
		VkPhysicalDeviceVulkan11Features vkFeatures11_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &vkFeatures12_ };
		VkPhysicalDeviceFeatures2 vkFeatures10_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vkFeatures11_ };
		// 

	public:

		VulkanCore(CoreConfiguration config = {}) :config_(config) {};
		~VulkanCore();
		VulkanCore(const VulkanCore&) = delete;
		VulkanCore& operator=(const VulkanCore&) = delete;
		VulkanCore(VulkanCore&&) = delete;
		VulkanCore& operator=(VulkanCore&&) = delete;

		int CreateInstance();
		bool Init();


		void CreateGraphicsPipeline(VkGraphicsPipelineCreateInfo createInfo, VkPipeline& pipeline);
		void CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler);
		void CreateImageView(ImageViewCreateInfo imageViewCreateInfo, VkImageView& view);
		void CreateShaderModule(const std::vector<char>& code, VkShaderModule& out);
		void CreateTimelineSemaphore(uint64_t initialValue, VkSemaphore& out);
		//void CreateFence(const VkFenceCreateInfo createInfo, VkFence& out);
		//void CreateCommandPool(QueuePreference queue, VkCommandPoolCreateFlags flags, VkCommandPool& out);
	};


	void CreateDefaultDescriptorSetLayout(VkDescriptorSetLayout& dsl);
	void CreateDefaultPipelineLayout(VkPipelineLayout& out);
	std::unique_ptr<smvk::VulkanCore> CreateVulkanCoreWithWindow(smvk::VulkanCore::CoreConfiguration config, GLFWwindow* window);

	struct CommandSubmitHandle {
		uint32_t index_ = 0;
		uint32_t gen_ = 0; // gen 0 means no slot. alas empty. 
		bool empty() const { return gen_ == 0; };
	};

	// This is a copy of the approach of IGL/lightweightVk but I changed fences for timelineSempahores.
	// This change doenst really make sense. TimelineSemaphore support would be more useful to model something like
	// QueueTimeline from nvprocore2. It works so its staying for now but this is DUMB!
	// * The idea was to peg multiple cmd to same timeline semaphore for batched jobs on a single slot.
	// makes no sense in practice. 
	struct Submitter {
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
			VkCommandBuffer allocatedCmd_ = VK_NULL_HANDLE;
			uint32_t gen_ = 1;
			//uint32_t index = UINT32_MAX;  // load balancing gens sorting and using this
		};
		//
		Submitter(VulkanCore& core, QueuePreference queue);
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
		VkQueue m_queue = VK_NULL_HANDLE;
		QueuePreference queue_ = QueuePreference::GRAPHICS;
		static const uint32_t kMax = 12;
		uint32_t availableSlots = kMax;
		WrapperSlot commandBuffers_[kMax];
		CommandSubmitHandle m_lastSubmit{}; // not in use right now. used for creating linkedList for a faux dependency chain
	};
	struct DeferredTask {
		DeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle) : task_(std::move(task)), handle_(handle) {}
		std::packaged_task<void()> task_;
		CommandSubmitHandle handle_;
	};



	// 4.6gb is good amount to reserver for 8gb vram system
	// 2gb for Windows is good practice. And a little headroom for aux apps
	struct Context {
		Context(VulkanCore& core);
		~Context();
		Context(const Context&) = delete;
		Context& operator=(const Context&) = delete;
		Context(Context&&) = delete;
		Context& operator=(Context&&) = delete;

		int Initialize();
		//
		void AddDeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle = CommandSubmitHandle{});
		void ProcessDeferrredTasks();
		//
		BufferHandle CreateStagingBuffer(VkDeviceSize size);
		Holder<ImageHandle> CreateImage(ImageCreateInfo imageCreateInfo, const void* data = nullptr);
		Holder<RenderPipelineHandle> CreateRenderPipeline(smvk::ShaderModuleHandle vert, smvk::ShaderModuleHandle frag);
		// 
		Holder<TextureHandle> CreateTexture(ImageCreateInfo imageCreateInfo, const void* data = nullptr); // specialization for textures instead of .isTexture property
		Holder<TextureHandle> CreateTexture(ktxTexture2& texture);
		Holder<BufferHandle> CreateBuffer(BufferCreateInfo bufferCreateInfo);
		Holder<ShaderModuleHandle> CreateShaderModule(std::vector<char> code);
		Holder<DescriptorPoolHandle> CreateDescriptorPool();
		//Holder<DescriptorSetHandle> CreateDescriptorSet(VkDescriptorSetLayout setLayout);
		//

		void* MapBuffer(smvk::Handle<smvk::Buffer> handle);
		void UnmapBuffer(smvk::Handle<smvk::Buffer> handle);

		void UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);
		void UploadDataToImage2D(Image img, const void* data, size_t dataSize, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);

		void CleanUp();

		//private:
		VulkanCore& core_;
		Submitter* m_submitter = nullptr;
		std::vector<DeferredTask> m_deferredTasks;

		Pool<Image, Image> images_; // VkSwapchainImages are not represented with Image type and just owned by the swapchain
		Pool<Texture, Image> textures_; // specializaed textures away from images
		Pool<Buffer, Buffer> buffers_;
		Pool<ShaderModule, VkShaderModule> shaderModules_;
		Pool<Sampler, VkSampler> samplers_;
		Pool<DescriptorPool, VkDescriptorPool> descriptorPools_;

		Pool<RenderPipeline, RenderPipelineState> graphicPipelines_;

		VmaAllocator m_allocator;
		// no pool in vmaCreateallocation goes to a default pool provided a vma for the memory type used
		// more granularity could provide useful
		VmaPool m_imagesPool;
		VmaPool m_stagingPool;

		TextureHandle m_nilTexture{}; // needed to avoid sparse list in bindless
		VkSampler m_defaultSampler{}; // This really should be a SamplersPool implementation the same way nprocore2 handles this


		void UpdateBindings();
		// Binding model is just hardcoded.
		VkDescriptorSetLayout defaultSetLayout;
		VkPipelineLayout defaultPipeLyt;
		Holder<DescriptorPoolHandle> defaultDescriptorPool;
		VkDescriptorSet defaultDescriptorSets_[MAX_FRAMES_IN_FLIGHT];
	};

	void destroy(Context* ctx, ImageHandle handle);
	void destroy(Context* ctx, TextureHandle handle);
	void destroy(Context* ctx, BufferHandle handle);
	void destroy(Context* ctx, ShaderModuleHandle handle);
	void destroy(Context* ctx, SamplerHandle handle);

	void destroy(Context* ctx, DescriptorPoolHandle handle);
	void destroy(Context* ctx, DescriptorSetHandle handle);
	void destroy(Context* ctx, RenderPipelineHandle handle);


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

	struct SamplerPool {
		struct SamplerPoolCreateInfo {
			VkDevice device;
		};
		SamplerPool(SamplerPoolCreateInfo createInfo);
		~SamplerPool();
		VkDevice device_;
		using SamplerKey = SamplerCreateInfo;
		std::map<SamplerKey, VkSampler> m_samplers;
		VkSampler GetSampler(SamplerKey key);
	};

	class VulkanSwapchain;
	struct FrameBuffer {
		uint32_t frameIndex = 0; // Maybe end os uint32_ to catch for unitialized? this value can only be ip to SWAPCHAIN_IMAGE_COUNT
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VulkanSwapchain& swapchain;
	};

	class VulkanSwapchain {

	public:

		VulkanSwapchain(
			Context& context
			, GLFWwindow* window
			//, uint32_t framesInFlight,
			//, uint32_t desiredImageCount
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
		Context& ctx_;
		GLFWwindow* window_ = nullptr;
		VkSwapchainKHR vk_swapchain_ = VK_NULL_HANDLE;
		VkFormat vk_surfaceFormat_ = VK_FORMAT_UNDEFINED;
		VkFormat vk_depthFormat_ = VK_FORMAT_UNDEFINED;

#pragma region ===[ Frame Commands List ]===
		VkCommandPool perFrameCommandPools[MAX_FRAMES_IN_FLIGHT]{};
		VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT]{};
#pragma endregion

		bool framebufferResized = false;

		Holder<ImageHandle> depthImage_;
		std::vector<VkImage> images_;
		std::vector<VkImageView> imageViews_;
		std::vector<VkSemaphore> renderFinishedSemaphores_;
		std::vector<VkSemaphore> imageAvailableSemaphores_;
		std::vector<VkFence> inFlightFences_;

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
};
