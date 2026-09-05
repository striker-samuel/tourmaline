#pragma once

#include <GLFW/glfw3.h>
#include <Engine/smvk.h>
#include <Engine/Memory/pool.h>
#include <Engine/Renderer/spirv_reflection.h>

#include "volk.h"
#include "vk_mem_alloc.h"
#include <source_location>
#include <ktx.h>
#include <ktxVulkan.h>

#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>

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

	class VulkanSamplerCache;
	class App;
	class VulkanSwapchain;
	using VulkanApp = App;

	struct VulkanFrameBuffer {
		uint32_t frameIndex = 0;
		uint32_t imageIndex = 0;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VulkanSwapchain& swapchain;
	};

	struct VulkanPipeline {
		VkPipeline pipeline{};
		VkPipelineLayout layout{};
	};

	struct PipelineBundle {
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
	};

	struct StaticMeshAllocation {
	//	std::vector<VmaVirtualAllocation> allocs;
		VmaVirtualAllocation vertexAllocation = VK_NULL_HANDLE;
		VmaVirtualAllocation indexAllocation = VK_NULL_HANDLE;
	};

	inline struct StaticVertexFmtHeap {
		BufferHandle m_staticVertexPosBuffer{};
		BufferHandle m_staticVertexShadingBuffer{};
		BufferHandle m_staticIndexBuffer{};
		VmaVirtualBlock m_staticVertexBlock = VK_NULL_HANDLE;
		VmaVirtualBlock m_staticIndexBlock = VK_NULL_HANDLE;
	} staticVertexFmtHeap;

	struct Image
	{
		VkImage vkImage = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;

		ImageType type = ImageType::Image2D;
		Extent3D extent{};
		Format format = Format::Undefined;
		ImageUsage usage = ImageUsage::None;
		SampleCount samples = SampleCount::Count1;
		uint32_t mipLevels = 1;
		uint32_t arrayLayers = 1;
		uint32_t liveViewCount = 0;
		ResourceOwnership ownership = ResourceOwnership::Owned;
		bool isConcurrent = false;
		const char* name;
	};

	struct ImageView
	{
		VkImageView vkView = VK_NULL_HANDLE;

		ImageHandle image{};
		ImageViewType viewType = ImageViewType::View2D;
		Format format = Format::Undefined;
		ImageSubresourceRange subresourceRange{};

		ResourceOwnership ownership = ResourceOwnership::Owned;
	};

	struct Buffer
	{
		VkBuffer vkBuffer = VK_NULL_HANDLE;
		VmaAllocation vma_alloc = VK_NULL_HANDLE;
		VkBufferUsageFlags usage = 0;
		VkMemoryPropertyFlags memoryFlags = 0;
		VkDeviceAddress deviceAddress = 0;
		bool isConcurrent = false;

		bool isMappable() const noexcept
		{
			return
				(memoryFlags &
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
		}
	};

	struct Texture
	{
		ImageHandle image{};
		ImageViewHandle view{};
		BindlessImageHandle bindless{};
	};

	struct ShaderModule
	{
		VkShaderModule module = VK_NULL_HANDLE;
		// Retained SPIR-V bytes for reflection readback (push ranges,
		// specialization constants). Populated at CreateShaderModule time.
		std::vector<char> code;
	};

	struct BindlessImage
	{
		ImageViewHandle view{};
		uint32_t descriptorIndex = UINT32_MAX;
		VkImageLayout imageLayout =
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	};

	///*constexpr*/ bool FormatHasStencil(VkFormat format);
	///*constexpr*/ bool FormatHasDepth(VkFormat format);

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
	};

	std::unique_ptr<smvk::VulkanCore> CreateVulkanCoreWithWindow(smvk::VulkanCore::CoreConfiguration config, ::GLFWwindow* window);
	//std::unique_ptr<smvk::VulkanCore> CreateVulkanCoreHeadless(smvk::VulkanCore::CoreConfiguration config);

	struct CommandBufferPool;
	//
	struct CommandSubmitHandle {
		CommandBufferPool* pool = nullptr;
		uint32_t index_ = UINT32_MAX;
		uint32_t gen_ = 0; // gen 0 means no slot. alas empty. 
		bool empty() const { return gen_ == 0 || pool == nullptr; };
	};

	struct TimelinePoint
	{
		VkSemaphore semaphore = VK_NULL_HANDLE;
		uint64_t value = 0;

		explicit operator bool() const
		{
			return semaphore != VK_NULL_HANDLE && value != 0;
		}

		[[nodiscard]]
		bool valid() const noexcept
		{
			return semaphore != VK_NULL_HANDLE && value != 0;
		}
	};

	struct TimelineWait
	{
		TimelinePoint point{};
		VkPipelineStageFlags2 stageMask =
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

	};

	struct Submitter;
	struct WrapperSlot {
		VkCommandBuffer cmd_ = VK_NULL_HANDLE;
		VkCommandBuffer allocatedCmd_ = VK_NULL_HANDLE;
		uint32_t gen_ = 1;
		enum class CommandState : uint8_t { Free, Recording, Submitted } state_ = CommandState::Free;
		VkSemaphore retireSemaphore_ = VK_NULL_HANDLE;
		uint64_t retireValue_ = 0;
	};
	struct CommandBufferPool {
		VkCommandPool pool = VK_NULL_HANDLE;
		static constexpr uint32_t kMax = 16;
		WrapperSlot slots[kMax];
		uint32_t availableSlots = kMax;
		uint32_t queueFamilyIndex = UINT32_MAX;
		VkDevice device = VK_NULL_HANDLE;
		void init(VkDevice dev, uint32_t qfi);
		void destroy();
		CommandSubmitHandle acquire(VkDevice dev, uint32_t qfi);
		VkCommandBuffer getCommandBuffer(CommandSubmitHandle h);
		void recycleSlot(uint32_t idx);
		bool isReady(CommandSubmitHandle h, VkSemaphore timeline, uint64_t& completed) const;
	};
	struct PoolKey {
		VkDevice device = VK_NULL_HANDLE;
		uint32_t qfi = 0;
		const Submitter* owner = nullptr;
		bool operator==(const PoolKey& o) const noexcept { return device==o.device && qfi==o.qfi && owner==o.owner; }
	};
	struct PoolKeyHash {
		size_t operator()(const PoolKey& k) const noexcept {
			size_t a = std::hash<void*>{}(reinterpret_cast<void*>(k.device));
			size_t b = std::hash<uint32_t>{}(k.qfi);
			size_t c = std::hash<const void*>{}(k.owner);
			size_t h = a ^ (b + 0x9e3779b9 + (a<<6) + (a>>2));
			h ^= c + 0x9e3779b9 + (h<<6) + (h>>2);
			return h;
		}
	};
	extern thread_local std::unordered_map<PoolKey, CommandBufferPool*, PoolKeyHash> t_commandPools;
	extern std::mutex g_allPoolsMutex;
	extern std::unordered_map<PoolKey, std::vector<CommandBufferPool*>, PoolKeyHash> g_allPools;
	CommandBufferPool* GetThreadLocalPool(uint32_t queueFamilyIndex, VkDevice device, const Submitter* owner = nullptr);


	struct VulkanResourceManager;
	struct Submitter
	{
		using CommandState = WrapperSlot::CommandState;

		Submitter(VulkanCore& core, uint32_t queueFamilyIndex);
		~Submitter();

		CommandSubmitHandle acquire();
		VkCommandBuffer GetCommandBuffer(CommandSubmitHandle handle);
		TimelinePoint submit(
			CommandSubmitHandle handle,
			std::span<const TimelineWait> waits = {},
			std::source_location loc =
			std::source_location::current());
		bool validHandle(CommandSubmitHandle handle) const;
		bool isReady(CommandSubmitHandle handle);
		void wait(CommandSubmitHandle handle, uint64_t timeout = UINT64_MAX);
		void waitAll();
		void purge();
		TimelinePoint nextSignalPoint() const;
		uint32_t queueFamilyIndex() const noexcept { return m_queueFamilyIndex; }
		VkQueue queue() const noexcept { return m_queue; }

	private:
		void recycleSlot(uint32_t index);
		void recycleSlot(CommandBufferPool* pool, uint32_t index);
		VkResult waitTimeline(
			uint64_t value,
			uint64_t timeout = UINT64_MAX) const;

	private:
		VulkanCore& core_;
		uint32_t m_queueFamilyIndex = 0;

		mutable std::recursive_mutex m_mutex;
		VkQueue m_queue = VK_NULL_HANDLE;
		CommandSubmitHandle m_lastSubmit{};
		VkSemaphore m_timelineSemaphore = VK_NULL_HANDLE;
		uint64_t m_timelineValue = 1;
		uint64_t m_lastTimelineValue = 0;

		CommandBufferPool* getPool() const;
	};

	// ---------------------------------------------------------------------
	// StagingRing – persistent host-visible buffer for batched uploads
	// ---------------------------------------------------------------------
	
	
	struct VulkanResourceManager;
	// Global sync states – for UploadStreamer visibility
	struct ImageGlobalSyncState {
		SyncBarrierMasks masks{ AccessFlags::None, PipelineStageBit_None };
		ImageLayout layout = ImageLayout::Undefined;
	};
	struct BufferGlobalSyncState {
		SyncBarrierMasks masks{ AccessFlags::None, PipelineStageBit_None };
	};

	// ---------------------------------------------------------------------
	// StagingRing – persistent host-visible buffer for batched uploads
	// ---------------------------------------------------------------------
	struct StagingRing {
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		void* mapped = nullptr;
		VkDeviceSize capacity = 0;
		VkDeviceSize offset = 0;
		VkDeviceSize alignedOffset(VkDeviceSize alignment) const noexcept {
			return (offset + alignment - 1) & ~(alignment - 1);
		}
	};

	// ---------------------------------------------------------------------
	// UploadStreamer – batched transfer hot-path (per-thread sharded).
	//
	// Multithread model: one local upload context per thread. Each shard
	// owns its StagingRing, current command buffer and pending barrier
	// lists, so record-path uploads never touch a shared mutex. Submits
	// still go through the shared Submitter (brief lock, once per flush –
	// negligible vs per-object pool locks). flush()/waitAll() drain ALL
	// shards and must run after workers joined (or on the render thread).
	// ---------------------------------------------------------------------
	struct UploadStreamer {
		UploadStreamer() = default;
		UploadStreamer(VulkanResourceManager* rm, Submitter* transfer, Submitter* graphics);
		~UploadStreamer();

		UploadStreamer(const UploadStreamer&) = delete;
		UploadStreamer& operator=(const UploadStreamer&) = delete;

		bool init(VkDeviceSize ringCapacity = 256ull * 1024 * 1024);
		void destroy();
		void uploadBuffer(BufferHandle dst, const void* data, size_t size, size_t dstOffset,
			VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess);
		void uploadImage(ImageHandle imageHandle, const void* data, size_t dataSize,
			VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess);
		void uploadImageKtx(ImageHandle imageHandle, const void* data, size_t dataSize, const std::vector<VkBufferImageCopy>& regions,
			VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess);
		bool tryHostImageCopy(VkImage image, VkFormat format, VkExtent3D extent, uint32_t mipLevels,
			uint32_t arrayLayers, const void* data, size_t dataSize,
			const std::vector<VkBufferImageCopy>& regions, VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		void flush();
		// Flush only the calling thread's shard (workers call this before
		// returning so their submits overlap with other workers' recording).
		void flushLocal();
		TimelinePoint flushAndGetTimeline();
		void waitAll();
		bool hasPending() const { return states_[CurrentPoolShard()].hasPending; }
		bool hasAnyPending() const noexcept;
	private:
		struct ThreadUploadState {
			StagingRing ring{};
			CommandSubmitHandle curHandle{};
			VkCommandBuffer curCmd = VK_NULL_HANDLE;
			bool hasPending = false;
			std::vector<VkBufferMemoryBarrier2> pendingBufferAcquires;
			std::vector<VkImageMemoryBarrier2> pendingImageAcquires;
			std::vector<std::packaged_task<void()>> pendingCleanups;
			VkDeviceSize stagedBytesSinceFlush = 0;
		};
		ThreadUploadState states_[kNumPoolShards];
		VkDeviceSize ringCapacityPerThread_ = 32ull * 1024 * 1024;
		bool ensureShardRing(ThreadUploadState& st);
		VkDeviceSize allocFromRing(ThreadUploadState& st, VkDeviceSize size, VkDeviceSize alignment);
		void ensureCmd(ThreadUploadState& st);
		void submitState(ThreadUploadState& st, bool waitOldestIfFull = true);
		VulkanResourceManager* m_rm = nullptr;
		Submitter* m_transfer = nullptr;
		Submitter* m_graphics = nullptr;
		bool m_hostCopySupported = false;

		static constexpr VkDeviceSize kFlushThreshold = 256ull * 1024 * 1024;
	};

	struct DeferredTask {
		DeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle) : task_(std::move(task)), handle_(handle), retirePoint_{}, submitter_(nullptr) {}
		DeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle, Submitter* submitter) : task_(std::move(task)), handle_(handle), retirePoint_{}, submitter_(submitter) {}
		DeferredTask(std::packaged_task<void()>&& task, TimelinePoint point) : task_(std::move(task)), handle_{}, retirePoint_(point), submitter_(nullptr) {}
		std::packaged_task<void()> task_;
		CommandSubmitHandle handle_;
		TimelinePoint retirePoint_;
		Submitter* submitter_ = nullptr;
		bool isReady() const;
	};

	//struct IResourceManager {};
	struct VulkanResourceManager final /*: public IResourceManager*/ {

		VulkanResourceManager(VulkanCore::CoreConfiguration configuration = VulkanCore::CoreConfiguration(), ::GLFWwindow* window=nullptr);
		~VulkanResourceManager();
		VulkanResourceManager(const VulkanResourceManager&) = delete;
		VulkanResourceManager& operator=(const VulkanResourceManager&) = delete;
		VulkanResourceManager(VulkanResourceManager&&) = delete;
		VulkanResourceManager& operator=(VulkanResourceManager&&) = delete;

		//
		void AddDeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle = CommandSubmitHandle{});
		void AddDeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle, Submitter* submitter);
		void AddDeferredTask(std::packaged_task<void()>&& task, TimelinePoint point);
		void ProcessDeferrredTasks();
		// Runs every queued deferred task unconditionally (no readiness check).
		// Only safe when the device is idle (or tearing down); used so teardown
		// can never leak resources whose retire point would never be reached.
		void FlushDeferredTasks();
		// Records the latest graphics-queue (swapchain) submit. destroy() keys
		// resource retirement on this point so vkDestroy* never runs while an
		// in-flight command buffer still references the object. Must be called
		// after every successful graphics submit (see EndAndPresentFramebuffer).
		void NotifyGraphicsSubmit(VkSemaphore timelineSemaphore, uint64_t timelineValue);
		// Queues task until all graphics work submitted so far has retired.
		// Runs it immediately if no graphics submit has ever been recorded
		// (GPU is idle by definition). Render thread only, like AddDeferredTask.
		void DeferDestroy(std::packaged_task<void()>&& task);
		//
		Holder<ImageHandle>          CreateImage(ImageCreateInfo imageCreateInfo, const void* data = nullptr);
		Holder<TextureHandle>        CreateTexture(ImageCreateInfo imageCreateInfo, const void* data = nullptr);
		Holder<TextureHandle>        CreateTexture(ktxTexture2& texture, const char* debugName = "Texture", ImageCreateInfo imageCreateInfo = {});
		Holder<BindlessImageHandle>  CreateBindlessImage(ImageViewHandle viewHandle, VkImageLayout readLayout);
		Holder<ImageViewHandle>      CreateImageView(ImageHandle image, const ImageViewCreateInfo& createInfo);
		ImageViewHandle               RegisterImageView(VkImageView vkView, Format format, const char* name = nullptr);

		// In-place recreation: keeps the same handle alive while swapping the
		// underlying Vulkan object. Used so external owners (e.g. the swapchain)
		// survive Recreate() without invalidating handles held by the rendergraph.
		void                          SetImageViewVkView(ImageViewHandle handle, VkImageView vkView);
		ImageViewHandle               RecreateImageView(ImageViewHandle handle, ImageHandle image, const ImageViewCreateInfo& createInfo);
		ImageHandle                   RecreateImage(ImageHandle handle, const ImageCreateInfo& createInfo, const void* data = nullptr);
		Holder<BufferHandle>         CreateBuffer(VkDeviceSize size, BufferType bufferType, const char* debugName = "Buffer");
		Holder<DescriptorPoolHandle> CreateDescriptorPool(std::vector<VkDescriptorPoolSize> poolSizes, const char* debugName = "DescriptorPool");

		Holder<ShaderModuleHandle>   CreateShaderModule(std::vector<char> code, const char* debugName = "ShaderModule");

		// SPIR-V readback: retained bytes + reflection (push ranges, spec consts).
		// CPU-only, safe to call any time after CreateShaderModule.
		std::span<const char> GetShaderModuleCode(ShaderModuleHandle handle) const;
		SpirvReflection ReflectShaderModule(ShaderModuleHandle handle) const;

		void* MapBuffer(BufferHandle handle);
		void UnmapBuffer(BufferHandle handle);

		void UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);
		void UploadDataToImage2D(const Image& image, const void* data, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset);

		void UpdateDescriptors(uint32_t frameIndex = 0);

		// Engine globals (set 0 / kBinding_EngineGlobals): time / deltaTime /
		// frameIndex / fps, computed engine-side from the acquire rhythm and
		// uploaded once per frame. Called by the swapchain right after the
		// in-flight slot retires (AcquireFramebuffer). No app plumbing.
		void FlushEngineGlobals(uint32_t frameIndex);

		uint32_t GetBindlessIndex(BindlessImageHandle handle) const;
		uint32_t GetTextureBindlessIndex(TextureHandle handle) const;

		std::unique_ptr<VulkanCore> core_;
		VmaAllocator m_allocator;
		Submitter* m_submitter = nullptr;
		Submitter* m_transferSubmitter = nullptr;
		std::unique_ptr<UploadStreamer> m_streamer;
		bool m_hostImageCopySupported = false;
		VkPhysicalDeviceHostImageCopyPropertiesEXT m_hostCopyProps{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES_EXT };
		std::vector<VkImageLayout> m_hostCopySrcLayouts;
		std::vector<VkImageLayout> m_hostCopyDstLayouts;
		std::vector<DeferredTask> m_deferredTasks;
		// Guards m_deferredTasks pushes from upload worker threads (rare:
		// only temp-staging cleanups on ring overflow). Render-thread
		// drain (Process/Flush) must not run concurrently with worker
		// submits – in practice FlushUploads joins workers first.
		mutable std::mutex m_deferredMutex;
		// Latest graphics submit retirement point (swapchain timeline).
		// Monotonic while the swapchain lives; reset only with the semaphore.
		VkSemaphore m_lastGraphicsSemaphore = VK_NULL_HANDLE;
		uint64_t m_lastGraphicsValue = 0;

		VkDescriptorSetLayout defaultSetLayout{};
		VkPipelineLayout defaultPipeLayout{};
		VkDescriptorSet defaultDescriptorSets_[MAX_FRAMES_IN_FLIGHT]{};

		// Resources – one local shard per thread, no pool locks (see pool.h).
		// create() allocates in the calling thread's shard; get()/destroy()
		// decode the shard from the handle. Cross-thread reads after join
		// are safe; concurrent mutation of the same entry is UB.
		ShardedPool<ImageTag, Image> images_;
		ShardedPool<TextureTag, Texture> textures_;
		ShardedPool<BufferTag, Buffer> buffers_;
		ShardedPool<ImageViewTag, ImageView> imageViews_;

		ShardedPool<SamplerTag, VkSampler> samplers_;
		ShardedPool<BindlessImageTag, BindlessImage> bindlessImages_;
		ShardedPool<DescriptorPoolTag, VkDescriptorPool> descriptorPools_;
		ShardedPool<ShaderModuleTag, ShaderModule> shaderModules_;

		// DEFAULTS (legacy handles – kept for ABI; new code should use samplerCache)
		VkSampler m_defaultSampler{};
		VkSampler m_defaultShadowSampler{};
		std::unique_ptr<VulkanSamplerCache> samplerCache_;

		mutable std::atomic<bool> m_needToUpdateTextures{ true };
		mutable std::atomic<bool> m_needToUpdateSamplers{ true };

		// SamplerCache API – stable index into sampler descriptor array [0, MAX_SAMPLERS)
		uint32_t GetOrCreateSampler(const SamplerCreateInfo& ci);
		VkSampler GetVkSampler(uint32_t index) const;
		uint32_t GetSamplerCount() const;
		Submitter* GetTransferSubmitter() const noexcept { return m_transferSubmitter; }
		Submitter* GetGraphicsSubmitter() const noexcept { return m_submitter; }
		UploadStreamer* GetStreamer() const noexcept { return m_streamer.get(); }
		bool IsHostImageCopySupported() const noexcept { return m_hostImageCopySupported; }
		bool IsHostCopySrcLayoutSupported(VkImageLayout layout) const noexcept {
			for (auto l : m_hostCopySrcLayouts) if (l == layout) return true;
			return false;
		}
		bool IsHostCopyDstLayoutSupported(VkImageLayout layout) const noexcept {
			for (auto l : m_hostCopyDstLayouts) if (l == layout) return true;
			return false;
		}
		const std::vector<VkImageLayout>& GetHostCopySrcLayouts() const noexcept { return m_hostCopySrcLayouts; }
		const std::vector<VkImageLayout>& GetHostCopyDstLayouts() const noexcept { return m_hostCopyDstLayouts; }
		const VkPhysicalDeviceHostImageCopyPropertiesEXT& GetHostCopyProps() const noexcept { return m_hostCopyProps; }
		void FlushUploads();
		void WaitForUploads();

		Holder<DescriptorPoolHandle> defaultDescriptorPool{};
		TextureHandle m_nilTexture{};

		// Engine globals: storage buffer (one slice per frame in flight) +
		// engine-side timing state. time/deltaTime derive from the acquire
		// rhythm (steady clock), frameIndex counts acquires, fps is an EMA.
		// Flushed every frame into kBinding_EngineGlobals.
		BufferHandle m_engineGlobalsBuffer{};
		std::chrono::steady_clock::time_point m_globalsStartTime{};
		std::chrono::steady_clock::time_point m_globalsLastAcquireTime{};
		uint32_t m_globalsFrameCounter = 0;
		float m_globalsSmoothedFps = 0.0f;

		// Global sync bookkeeping – use global types for UploadStreamer visibility
		using ImageGlobalSyncState = ::smvk::ImageGlobalSyncState;
		using BufferGlobalSyncState = ::smvk::BufferGlobalSyncState;
		static uint64_t SyncKey(uint32_t idx, uint32_t gen) noexcept {
			return (static_cast<uint64_t>(gen) << 32) | idx;
		}
		template<typename H> static uint64_t SyncKey(H h) noexcept { return SyncKey(h.index(), h.gen()); }

		// Per-thread sync shards – hot path is lock-free. Shard is decoded
		// from the handle index (index / kShardCapacity), matching the pool
		// shard that owns the object. Writes only touch the owning thread's
		// shard; Snapshot/Commit iterate all shards when quiescent.
		std::unordered_map<uint64_t, ImageGlobalSyncState> imageSyncShards_[kNumPoolShards];
		std::unordered_map<uint64_t, BufferGlobalSyncState> bufferSyncShards_[kNumPoolShards];
		static uint32_t SyncShard(uint32_t globalIndex) noexcept { return globalIndex / kShardCapacity; }

		// Global sync accessors – thread-safe.
		ImageGlobalSyncState GetImageSyncState(ImageHandle h) const;
		BufferGlobalSyncState GetBufferSyncState(BufferHandle h) const;
		void SetImageSyncState(ImageHandle h, ImageGlobalSyncState state);
		void SetBufferSyncState(BufferHandle h, BufferGlobalSyncState state);
		void RemoveImageSyncState(ImageHandle h);
		void RemoveBufferSyncState(BufferHandle h);
		std::unordered_map<uint64_t, ImageGlobalSyncState> SnapshotImageSyncStates() const;
		std::unordered_map<uint64_t, BufferGlobalSyncState> SnapshotBufferSyncStates() const;
		void CommitImageSyncStates(const std::unordered_map<uint64_t, ImageGlobalSyncState>& states);
		void CommitBufferSyncStates(const std::unordered_map<uint64_t, BufferGlobalSyncState>& states);


		friend struct UploadStreamer;
	private:
		ImageSubresourceRange ResolveImageSubresourceRange(const Image& image, ImageSubresourceRange range) const;
		Image CreateImageImpl(const ImageCreateInfo& ci);
	};

	class VulkanSwapchain final /*: public ISwapchain*/ {

	public:

		VulkanSwapchain(
			VulkanResourceManager& context
			, ::GLFWwindow* window
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
		ImageViewHandle GetImageViewHandle(uint32_t index) const { return imageViewHandles_.at(index); }
		VkImage GetCurrentImage() const { return images_.at(currentImageIndex_); }
		VkImageView GetCurrentImageView() const { return imageViews_.at(currentImageIndex_); }

		bool FramebufferResized() const { return framebufferResized; }
		void ClearFramebufferResized() { framebufferResized = false; }
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
		::GLFWwindow* window_ = nullptr;
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
		std::vector<ImageViewHandle> imageViewHandles_;
		std::vector<VkSemaphore> renderFinishedSemaphores_;
		std::vector<VkSemaphore> imageAvailableSemaphores_;
		std::vector<VkFence> inFlightFences_;
		std::vector<VkFence> imagesInFlight_;
		VkSemaphore timelineSemaphore_ = VK_NULL_HANDLE;
		uint64_t timelineValue_ = 0;
		std::vector<uint64_t> frameTimelineValues_;
		std::vector<uint64_t> imageTimelineValues_;

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
		void CleanupSwapchain(bool fullDestroy = false);

		VulkanFrameBuffer AcquireFramebuffer();
		void EndAndPresentFramebuffer(VulkanFrameBuffer& cb);
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
				HashValue(seed, shader.module.gen());
				HashValue(seed, static_cast<uint32_t>(shader.stage));
				HashValue(seed, std::string_view(shader.entryPoint ? shader.entryPoint : ""));
				// Specialization constants participate in the pipeline state:
				// same module + different spec values must miss the cache.
				const bool hasSpec = shader.specialization && !shader.specialization->entries.empty();
				HashValue(seed, hasSpec);
				if (hasSpec) {
					for (const auto& e : shader.specialization->entries) {
						HashValue(seed, e.constantID);
						HashValue(seed, e.offset);
						HashValue(seed, e.size);
					}
					for (uint8_t b : shader.specialization->data)
						HashValue(seed, b);
				}
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

	// ------------------------------------------------------------------
	// VulkanSamplerCache – deduplicates VkSampler objects.
	// Like VulkanPipelineCache, a description key (SamplerCreateInfo) maps
	// to a stable descriptor-array index (0 .. MAX_SAMPLERS-1). The cache
	// owns the VkSampler handles and drives descriptor updates.
	// ------------------------------------------------------------------
	class VulkanSamplerCache
	{
	public:
		explicit VulkanSamplerCache(VulkanResourceManager* rm);
		~VulkanSamplerCache();
		VulkanSamplerCache(const VulkanSamplerCache&) = delete;
		VulkanSamplerCache& operator=(const VulkanSamplerCache&) = delete;

		// Returns stable index into the global sampler descriptor array.
		// Creates the VkSampler on first use; subsequent calls with the
		// same SamplerCreateInfo return the same index without allocation.
		// Asserts if MAX_SAMPLERS would be exceeded (sampler count is a
		// physical limit – caller must stay within budget).
		uint32_t GetOrCreate(const SamplerCreateInfo& ci);

		// Read-only access for descriptor uploads.
		uint32_t size() const noexcept { std::lock_guard<std::mutex> lock(mutex_); return static_cast<uint32_t>(samplers_.size()); }
		VkSampler at(uint32_t index) const;
		void Clear();

	private:
		VulkanResourceManager* rm_ = nullptr;
		// Dense array – index == descriptor array slot.
		mutable std::mutex mutex_;
		std::vector<VkSampler> samplers_;
		std::vector<SamplerCreateInfo> infos_;
		std::unordered_map<SamplerCreateInfo, uint32_t, SamplerCreateInfoHash> map_;
	};

	class VulkanPipelineCache
	{
	public:
		explicit VulkanPipelineCache(VulkanResourceManager* ctx)
			: ctx_(ctx)
		{}

		~VulkanPipelineCache()
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
			const MSAASettings& msaa = MSAASettings::Count1());
		PipelineBundle GetComputePipeline(
			ShaderProgram& program);
		void Clear();


	private:
		PipelineBundle BuildGraphicsPipeline(
			const PipelineKey& key);

		PipelineBundle BuildComputePipeline(
			ShaderProgram& program);

	private:
		VulkanResourceManager* ctx_ = nullptr;

		std::unordered_map<
			PipelineKey,
			PipelineBundle,
			PipelineKeyHash
		> pipelines_;

		struct ComputeKey {
			ShaderModuleHandle module{};
			std::string entry;
			std::vector<SpecializationEntry> specEntries;
			std::vector<uint8_t> specData;
			bool operator==(const ComputeKey& o) const noexcept {
				return module == o.module && entry == o.entry &&
					specEntries == o.specEntries && specData == o.specData;
			}
		};
		struct ComputeKeyHash {
			size_t operator()(const ComputeKey& k) const noexcept {
				size_t h = std::hash<uint32_t>{}(k.module.index());
				h ^= std::hash<uint32_t>{}(k.module.gen()) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<std::string>{}(k.entry) + 0x9e3779b9 + (h << 6) + (h >> 2);
				for (const auto& e : k.specEntries) {
					h ^= std::hash<uint32_t>{}(e.constantID) + 0x9e3779b9 + (h << 6) + (h >> 2);
					h ^= std::hash<uint32_t>{}(e.offset) + 0x9e3779b9 + (h << 6) + (h >> 2);
					h ^= std::hash<uint32_t>{}(e.size) + 0x9e3779b9 + (h << 6) + (h >> 2);
				}
				for (uint8_t b : k.specData)
					h ^= std::hash<uint8_t>{}(b) + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};
		std::unordered_map<ComputeKey, PipelineBundle, ComputeKeyHash> computePipelines_;
	};

	std::vector<VkVertexInputBindingDescription> getVkVertexInputBindingDescriptions(const VertexFormatDescription& format);
	std::vector<VkVertexInputAttributeDescription> getVkVertexAttributeDescriptions(const VertexFormatDescription& format);

	// destroy for internal VulkanResourceManager – for Holder type-erased deleter
	void destroy(VulkanResourceManager* ctx, ImageHandle handle);
	void destroy(VulkanResourceManager* ctx, TextureHandle handle);
	void destroy(VulkanResourceManager* ctx, BufferHandle handle);
	void destroy(VulkanResourceManager* ctx, SamplerHandle handle);
	void destroy(VulkanResourceManager* ctx, BindlessImageHandle handle);
	void destroy(VulkanResourceManager* ctx, ImageViewHandle handle);
	void destroy(VulkanResourceManager* ctx, DescriptorPoolHandle handle);
	void destroy(VulkanResourceManager* ctx, ShaderModuleHandle handle);

	// Vulkan globals accessors for ImGui / external integrations (implemented in impl.cpp).
	VkInstance       GetVkInstance();
	VkDevice         GetVkDevice();
	VkPhysicalDevice GetVkPhysicalDevice();
	VkQueue          GetVkGraphicsQueue();
	VkQueue          GetVkPresentQueue();
	VkQueue          GetVkTransferQueue();
	VkSurfaceKHR     GetVkSurface();
	uint32_t         GetVkGraphicsQueueFamily();
	// True when the device was created with multiDrawIndirect enabled.
	// CommandBuffer::DrawIndexedIndirect uses the single-call fast path
	// then, and falls back to one-by-one emulation otherwise.
	bool             SupportsMultiDrawIndirect();
	uint32_t         GetVkTransferQueueFamily();

}; // namespace smvk

