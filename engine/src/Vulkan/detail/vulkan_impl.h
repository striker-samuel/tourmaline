#pragma once

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


struct SDL_Window;

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
		// Async-split second graphics part (G2): set by RenderGraph::Execute
		// when the frame splits G1/C/G2 (null = single submit). EndAndPresent
		// ends + submits it with the compute-done wait below, then presents.
		VkCommandBuffer splitCommandBuffer = VK_NULL_HANDLE;
		VkSemaphore splitWaitSemaphore = VK_NULL_HANDLE; // timeline: C -> G2
		uint64_t splitWaitValue = 0;
		// G1 -> G2 ordering (async split): G2 additionally waits G1's own
		// timeline signal. Per spec, submissions to one queue only respect
		// submission (begin) order and may overlap — G2 must not be able to
		// start (and write the swapchain image in Tonemap) while G1 is still
		// blocked on the acquire semaphore, so the acquire wait G1 performs
		// is inherited explicitly, not "transitively through same-queue
		// submission order". Timeline waits don't consume, so waiting the
		// same G2C semaphore from both C and G2 is legal.
		VkSemaphore splitWaitSemaphore2 = VK_NULL_HANDLE; // timeline: G1 -> G2
		uint64_t splitWaitValue2 = 0;
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
		// Mirrors ImageCreateInfo::isCubemap (VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT).
		// KTX cubemap uploads set it from texture.isCubemap.
		bool isCubemap = false;
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

	std::unique_ptr<smvk::VulkanCore> CreateVulkanCoreWithWindow(smvk::VulkanCore::CoreConfiguration config, SDL_Window* window);
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
		// Queue family that performed the last recorded access
		// (graphics or dedicated compute). Drives the async-compute split
		// in RenderGraph::Execute: a pass on the other queue bridges with
		// an event + layout barrier instead of a same-queue barrier.
		// UINT32_MAX = unknown (treat as graphics; pre-async default).
		uint32_t queueFamily = UINT32_MAX;
	};
	struct BufferGlobalSyncState {
		SyncBarrierMasks masks{ AccessFlags::None, PipelineStageBit_None };
		// Queue family that performed the last recorded access, mirroring
		// ImageGlobalSyncState. Drives the async-compute split: a barrier
		// handed off between queues (compute-written buffer read by a
		// graphics pass or vice versa) cannot carry the writer's stage in
		// the reader's command buffer — the cross-queue semaphore chain
		// provides availability, so the barrier degrades to a pure
		// acquire (srcStage=ALL_COMMANDS, srcAccess=NONE).
		// UINT32_MAX = unknown (treat as graphics; pre-async default).
		uint32_t queueFamily = UINT32_MAX;
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
		// Table-driven bulk staged copy (the load-time counterpart of
		// uploadBuffer): `content` is one packed blob, `copies` indexes into
		// it. Drives the dedicated bulk_ context (512 MB ring, render-thread
		// only — load-time bulk uploads are single-threaded by construction)
		// through the same allocFromRing/ensureCmd/submitState machinery:
		// one memcpy pass, one command buffer, one vmaFlush, ONE barrier set
		// and one submit per 512 MB blob. Entries larger than the ring fall
		// back to the per-entry temp staging (mirrors uploadBuffer).
		void uploadBufferBulk(BufferHandle dst, const void* content,
			std::span<const BufferBulkCopy> copies,
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
		// Newest transfer-batch timeline point across all shards. The
		// swapchain waits it before submitting G1/G2/main command buffers:
		// per spec, same-queue submissions only respect submission (begin)
		// order and may overlap, so uploads -> frame commands must be a
		// semaphore-ordered memory dependency, not queue FIFO. Values on one
		// Submitter timeline are monotonic, so waiting the newest covers
		// every earlier batch on that timeline.
		TimelinePoint lastTransferPoint() const noexcept {
			return TimelinePoint{ m_lastTransferSemaphore,
				m_lastTransferValue.load(std::memory_order_acquire) };
		}
		// Merge an externally-submitted point on the SAME timeline (the
		// transfer Submitter's) into the chain — e.g. the async readback
		// ticket copy. The next submitTransfer waits it, so every
		// transfer-queue submission (upload batches AND downloads) is
		// strictly ordered: per spec, same-queue submissions only respect
		// submission (begin) order and may overlap, which is exactly the
		// WAR/WAW hazard sync validation reports between the per-frame
		// zero-fills (histogram / ds counters / feedback) and the readback
		// copies reading the retired slices.
		void chainExternalPoint(const TimelinePoint& tp) noexcept {
			if (!tp.valid())
				return;
			m_lastTransferSemaphore = tp.semaphore;
			uint64_t observed = m_lastTransferValue.load(std::memory_order_relaxed);
			while (tp.value > observed
				&& !m_lastTransferValue.compare_exchange_weak(
					observed, tp.value, std::memory_order_release)) {}
		}
	private:
		// Ring bytes [start, end) read by one submitted transfer batch.
		// handle waits exactly that batch's retirement (precise wrap sync:
		// an alloc only waits batches overlapping the bytes it overwrites).
		struct InFlightRingRange {
			VkDeviceSize start;
			VkDeviceSize end;
			CommandSubmitHandle handle;
		};
		struct ThreadUploadState {
			StagingRing ring{};
			CommandSubmitHandle curHandle{};
			VkCommandBuffer curCmd = VK_NULL_HANDLE;
			bool hasPending = false;
			std::vector<VkBufferMemoryBarrier2> pendingBufferAcquires;
			std::vector<VkImageMemoryBarrier2> pendingImageAcquires;
			std::vector<std::packaged_task<void()>> pendingCleanups;
			VkDeviceSize stagedBytesSinceFlush = 0;
			// Ring range covered by the batch currently recording into
			// curCmd (VK_WHOLE_SIZE = none yet). Recorded into
			// pendingRanges at submit; the precise wrap/alloc sync in
			// allocFromRing consumes it.
			VkDeviceSize batchRingStart = VK_WHOLE_SIZE;
			std::vector<InFlightRingRange> pendingRanges;
		};
		ThreadUploadState states_[kNumPoolShards];
		VkDeviceSize ringCapacityPerThread_ = 32ull * 1024 * 1024;
		// Dedicated bulk context (512 MB ring, lazily created on the first
		// uploadBufferBulk call so non-bulk users never pay the host
		// memory): the shard rings stay 32 MB for steady-state frame
		// uploads. Render-thread only — bulk uploads are load-time and
		// single-threaded by construction (pak cluster deserialize).
		ThreadUploadState bulk_;
		static constexpr VkDeviceSize kBulkRingCapacity = 512ull * 1024 * 1024;
		bool ensureRing(ThreadUploadState& st, VkDeviceSize capacity, const char* debugName);
		bool ensureBulkRing() {
			return ensureRing(bulk_, kBulkRingCapacity, "UploadStreamer Ring (bulk)");
		}
		bool ensureShardRing(ThreadUploadState& st);
		VkDeviceSize allocFromRing(ThreadUploadState& st, VkDeviceSize size, VkDeviceSize alignment);
		void ensureCmd(ThreadUploadState& st);
		void recordSubmittedRange(ThreadUploadState& st);
		void submitState(ThreadUploadState& st, bool waitOldestIfFull = true);
		// Submit a transfer-queue batch. When the upload queue is distinct
		// from graphics, the submit waits on the latest swapchain
		// retirement point so batches never overwrite data that in-flight
		// frames still read (cross-queue equivalent of the old same-queue
		// FIFO). GPU-side wait only; the CPU never blocks.
		TimelinePoint submitTransfer(CommandSubmitHandle h);
		VulkanResourceManager* m_rm = nullptr;
		Submitter* m_transfer = nullptr;
		Submitter* m_graphics = nullptr;
		bool m_hostCopySupported = false;
		// Newest transfer-batch timeline point (see lastTransferPoint).
		// The semaphore is set once (first submit) and is then immutable;
		// only the value races between shards, and losing a concurrent
		// SMALLER max is harmless (frame consumers only depend on batches
		// the render thread itself flushed — those are serialized).
		VkSemaphore m_lastTransferSemaphore = VK_NULL_HANDLE;
		std::atomic<uint64_t> m_lastTransferValue{ 0 };

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

		VulkanResourceManager(VulkanCore::CoreConfiguration configuration = VulkanCore::CoreConfiguration(), SDL_Window* window=nullptr);
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
		// Streaming subrange variant + in-place retarget (see RHI/resource_manager.h).
		Holder<TextureHandle>        CreateStreamingTexture(ktxTexture2& texture, uint32_t firstMip, uint32_t mipCount, const char* debugName = "Texture", ImageCreateInfo imageCreateInfo = {});
		void                          RetargetStreamingTexture(TextureHandle handle, ktxTexture2& texture, uint32_t firstMip, uint32_t mipCount, const char* debugName = "Texture");
		Holder<BindlessImageHandle>  CreateBindlessImage(ImageViewHandle viewHandle, VkImageLayout readLayout);
		Holder<ImageViewHandle>      CreateImageView(ImageHandle image, const ImageViewCreateInfo& createInfo);
		ImageViewHandle               RegisterImageView(VkImageView vkView, Format format, const char* name = nullptr);

		// In-place recreation: keeps the same handle alive while swapping the
		// underlying Vulkan object. Used so external owners (e.g. the swapchain)
		// survive Recreate() without invalidating handles held by the rendergraph.
		void                          SetImageViewVkView(ImageViewHandle handle, VkImageView vkView);
		ImageViewHandle               RecreateImageView(ImageViewHandle handle, ImageHandle image, const ImageViewCreateInfo& createInfo);
		ImageHandle                   RecreateImage(ImageHandle handle, const ImageCreateInfo& createInfo, const void* data = nullptr);
		Holder<BufferHandle>         CreateBuffer(VkDeviceSize size, BufferType bufferType, const char* debugName = "Buffer", VmaAllocationCreateFlags vmaAllocFlags = 0);
		Holder<DescriptorPoolHandle> CreateDescriptorPool(std::vector<VkDescriptorPoolSize> poolSizes, const char* debugName = "DescriptorPool");

		Holder<ShaderModuleHandle>   CreateShaderModule(std::vector<char> code, const char* debugName = "ShaderModule");

		// SPIR-V readback: retained bytes + reflection (push ranges, spec consts).
		// CPU-only, safe to call any time after CreateShaderModule.
		std::span<const char> GetShaderModuleCode(ShaderModuleHandle handle) const;
		SpirvReflection ReflectShaderModule(ShaderModuleHandle handle) const;

		void* MapBuffer(BufferHandle handle);
		void UnmapBuffer(BufferHandle handle);

		void UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);
		// Table-driven bulk staged copy (RHI: ResourceManager::BulkCopyBuffer;
		// the load-time batched counterpart of UploadDataToBuffer). Content
		// bytes are staged during the call, so `content` only has to outlive
		// the call. Returns false only on an invalid dst/content (the table
		// itself cannot be validated against the caller's blob — caller
		// contract, like UploadDataToBuffer).
		bool BulkCopyBuffer(BufferHandle dst, const void* content,
			std::span<const BufferBulkCopy> copies,
			VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess);
		void UploadDataToImage2D(const Image& image, const void* data, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset);
		// GPU buffer -> CPU memory: one copy into a persistent grow-on-demand
		// staging buffer, a wait for that copy, then a memcpy. Render-thread
		// only (shares the graphics submitter + the shared staging buffer).
		// Returns false on zero size / bad handle / staging alloc failure.
		bool DownloadDataFromBuffer(BufferHandle src, void* out, size_t size, size_t srcOffset = 0);
		// Batched variant (see RHI/resource_manager.h): one submit + wait
		// for the whole list. The single-copy path delegates to it.
		bool DownloadDataFromBuffers(std::span<const BufferDownload> downloads);
		// ASYNC batched download — no CPU wait anywhere. Submit records the
		// copies into the dedicated async staging and returns a ticket (the
		// graphics-timeline value the copy signals); Poll checks the timeline
		// counter NON-BLOCKING each frame and, once signaled, memcpys staging
		// into the caller's buffers (same span layout as the submit) and
		// frees the single in-flight slot. Designed for the periodic exposure
		// histogram + texture-feedback tick: the render thread never stalls
		// on the graphics-queue backlog the way the sync paths do. A submit
		// while a ticket is still in flight returns 0 (caller skips/retries
		// next tick, so no backlog builds). Render-thread only.
		uint64_t SubmitAsyncDownloadDataFromBuffers(std::span<const BufferDownload> downloads);
		bool PollAsyncDownloadDataFromBuffers(uint64_t ticket, std::span<const BufferDownload> downloads);
		bool HasAsyncDownloadInFlight() const noexcept { return m_asyncReadbackTicket != 0; }
		// GPU color image (layer 0, mip 0, tightly packed) -> CPU memory.
		// currentLayout must be the image's ACTUAL layout (the rendergraph
		// does not write sync states back; Undefined would discard the
		// contents on transition). Transitions to TRANSFER_SRC and back.
		bool DownloadDataFromImage(ImageHandle src, ImageLayout currentLayout, void* out, size_t size);
		// Grows the persistent readback staging to cover `size` bytes
		// (power-of-two steps, 256 KiB floor). The old buffer retires via
		// DeferDestroy. Render-thread only, like the download itself.
		bool EnsureReadbackStaging(VkDeviceSize size);

		void UpdateDescriptors(uint32_t frameIndex = 0);

		// Engine globals (set 0 / kBinding_EngineGlobals): time / deltaTime /
		// frameIndex / fps, computed engine-side from the acquire rhythm and
		// uploaded once per frame. Called by the swapchain right after the
		// in-flight slot retires (AcquireFramebuffer). No app plumbing.
		void FlushEngineGlobals(uint32_t frameIndex);

		// Rewrites the set-0 kBinding_TextureFeedback descriptor to the
		// given flight slice. Call once per record before passes execute
		// (mirrors FlushEngineGlobals timing; update-after-bind makes it
		// visible to this frame's commands).
		void FlushTextureFeedback(uint32_t frameIndex);
		BufferHandle GetTextureFeedbackBuffer(uint32_t frameIndex);

		uint32_t GetBindlessIndex(BindlessImageHandle handle) const;
		uint32_t GetTextureBindlessIndex(TextureHandle handle) const;
		// True when the texture's backing image is cube-compatible with a
		// ViewCube / ViewCubeArray sampled view (manual isCubemap images
		// and KTX2 cubemap uploads). False for invalid handles.
		bool IsCubemapTexture(TextureHandle handle) const;
		// Backing-image array layers (6 = single cubemap, 6*N = cube
		// array) and base-extent edge length. 0 for invalid handles.
		uint32_t GetTextureArrayLayers(TextureHandle handle) const;
		uint32_t GetTextureCubemapSize(TextureHandle handle) const;
		uint32_t GetTextureMipLevels(TextureHandle handle) const;
		std::vector<GpuMemoryHeap> GetGpuMemoryInfo() const;

		std::unique_ptr<VulkanCore> core_;
		VmaAllocator m_allocator;
		Submitter* m_submitter = nullptr;
		Submitter* m_transferSubmitter = nullptr;
		// Async-compute submitter for EVSM prefiltering. Aliased to the
		// graphics submitter when the device has no dedicated compute
		// queue (emulated async: same-queue compute, still pipelined H->V).
		Submitter* m_computeSubmitter = nullptr;
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
		// In-place sampler replacement: destroys the VkSampler at `index`
		// and creates a new one from `ci`, keeping the descriptor-array
		// slot stable (shaders hardcode slots 0/1/2). Marks samplers dirty
		// so UpdateDescriptors rewrites the bindless sampler array.
		// Used by the texture-filtering toggle (bilinear <-> trilinear).
		// Returns false when index is out of range.
		bool RecreateSampler(uint32_t index, const SamplerCreateInfo& ci);
		Submitter* GetTransferSubmitter() const noexcept { return m_transferSubmitter; }
		Submitter* GetGraphicsSubmitter() const noexcept { return m_submitter; }
		Submitter* GetComputeSubmitter() const noexcept { return m_computeSubmitter != nullptr ? m_computeSubmitter : m_submitter; }
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
		// Texture-streaming feedback ring (Skull & Bones style GPU
		// readback): one uint demand per bindless texture slot, written by
		// fragment shaders via InterlockedMin. Ringed per flight like the
		// exposure histogram so the record-time clear can never clobber a
		// still-accumulating slice; the set-0 binding is rewritten every
		// frame to the current slice (see FlushTextureFeedback).
		// Fixed MAX_TEXTURES slots (32 KiB/slice); always valid, always
		// bound, so shaders need no enable toggle.
		BufferHandle m_feedbackBuffers[MAX_FRAMES_IN_FLIGHT]{};		std::chrono::steady_clock::time_point m_globalsStartTime{};
		std::chrono::steady_clock::time_point m_globalsLastAcquireTime{};
		uint32_t m_globalsFrameCounter = 0;
		float m_globalsSmoothedFps = 0.0f;

		// Persistent readback staging (DownloadDataFromBuffer): grow-on-
		// demand, reused across calls (each call waits for its own copy
		// before returning, so no in-flight overlap), freed in teardown.
		// Replaces the old per-call staging create + deferred destroy,
		// which churned allocations per frame and re-entered the
		// deferred-task queue from inside the drain loop.
		BufferHandle m_readbackStaging{};
		VkDeviceSize m_readbackStagingSize = 0;
		// Dedicated staging for the async download path: while a ticket is
		// in flight (up to a few frames), the sync readbacks (DS stats,
		// screenshots) must not clobber these bytes.
		BufferHandle m_asyncStaging{};
		VkDeviceSize m_asyncStagingSize = 0;
		uint64_t m_asyncReadbackTicket = 0; // 0 = slot free
		VkSemaphore m_asyncReadbackSemaphore = VK_NULL_HANDLE;
		VkDeviceSize m_asyncReadbackBytes = 0;
		bool EnsureAsyncStaging(VkDeviceSize size);

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
			, SDL_Window* window
		);
		// HOSTED render target: an offscreen color image instead of a
		// window surface. Acquire/submit run unchanged (frame command
		// pools, timeline retirement); "present" transitions the target
		// to SHADER_READ and skips the queue present entirely — the
		// image is consumed by an external compositor (the editor's
		// WYSIWYG viewport). One image, one view, fixed extent
		// (the compositor scales; Recreate is a no-op).
		VulkanSwapchain(
			VulkanResourceManager& context
			, VkExtent2D hostedExtent
			, Format colorFormat
		);
		bool hosted() const { return hosted_; }
		// Bindless-composition access: the stable handle of the hosted
		// target view (registered like a swapchain view so rendergraph
		// resolves work unchanged).
		ImageViewHandle HostedViewHandle() const {
			return imageViewHandles_.empty() ? ImageViewHandle{}
				: imageViewHandles_[0];
		}
		// The hosted target IMAGE (readback/dump paths want the image,
		// not the view).
		ImageHandle HostedImageHandle() const { return hostedImage_; }
		VulkanSwapchain(const VulkanSwapchain&) = delete;
		VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;
		VulkanSwapchain(VulkanSwapchain&&) = delete;
		VulkanSwapchain& operator=(VulkanSwapchain&&) = delete;
		~VulkanSwapchain();

		void Create() {
			// A minimized window reports a 0x0 client size; the swapchain
			// and every derived target cannot exist at extent 0 (the very
			// first depth image dies with VK_ERROR_INITIALIZATION_FAILED).
			WaitForNonMinimizedExtent();
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
		// VSync / presentation-mode control. The requested mode takes effect
		// on the next Create/Recreate; the active mode is what the running
		// swapchain was actually created with (fallback when unsupported).
		// Defaults to MAILBOX (no VSync, no tearing).
		void SetRequestedPresentMode(VkPresentModeKHR mode) { requestedPresentMode_ = mode; }
		VkPresentModeKHR GetRequestedPresentMode() const { return requestedPresentMode_; }
		VkPresentModeKHR GetActivePresentMode() const { return activePresentMode_; }
		std::vector<VkPresentModeKHR> GetAvailablePresentModes() const;
		// Frame GPU time (ms) of the last retired frame, measured with
		// per-frame timestamp queries around the whole command buffer.
		// Valid once a full frame has retired (HasGpuTiming); until then the
		// overlay should show CPU time only.
		double GetLastGpuMs() const { return lastGpuMs_; }
		bool HasGpuTiming() const { return gpuTimingValid_; }
		// Last vkQueuePresentKHR wall time (ms). Under FIFO VSync the
		// present blocks for the vertical blank — idle wait, not CPU work.
		double GetLastPresentWaitMs() const { return lastPresentWaitMs_; }
		VkSemaphore GetImageAvailableSemaphore() const { return imageAvailableSemaphores_.at(currentFrame_); }
		VkFence GetInFlightFence() const { return inFlightFences_.at(currentFrame_); }
		VkSemaphore GetRenderFinishedSemaphore(uint32_t imageIndex) const { return renderFinishedSemaphores_.at(imageIndex); }
		// 

		void AdvanceFrame() { currentFrame_ = (currentFrame_ + 1) % framesInFlight_; }
		void SetCurrentImageIndex(uint32_t imageIndex) { currentImageIndex_ = imageIndex; }

		//private:
		VulkanResourceManager& ctx_;
		SDL_Window* window_ = nullptr;
		VkSwapchainKHR vk_swapchain_ = VK_NULL_HANDLE;

		// Hosted render-target mode (see the hosted ctor): the offscreen
		// color image backing the "surface". Holder-owned through the
		// context (in-place recreate support); raw views live in
		// imageViews_ exactly like swapchain views.
		bool hosted_ = false;
		Holder<ImageHandle> hostedImage_;

		Format surfaceFormat{};
		Format depthFormat{};

#pragma region ===[ Frame Commands List ]===
		VkCommandPool perFrameCommandPools[MAX_FRAMES_IN_FLIGHT]{};
		VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT]{};
		// True-async-compute split (EVSM SameFrame): G1 (graphics, through
		// the moment passes) -> C (compute-queue blur) -> G2 (graphics,
		// lighting onward). Chained with per-flight timeline semaphores
		// (g2c, c2g); submission order G1->C->G2 makes every wait
		// signal-before-wait legal. Created lazily on first split use
		// (needs the compute family), reset in Acquire (slot retired),
		// destroyed in dtor. Monotonic values per flight.
		VkCommandPool splitPools[MAX_FRAMES_IN_FLIGHT]{};      // graphics family (G2)
		VkCommandBuffer splitBuffers[MAX_FRAMES_IN_FLIGHT]{};  // G2
		VkCommandPool asyncComputePools[MAX_FRAMES_IN_FLIGHT]{}; // compute family (C)
		VkCommandBuffer asyncComputeBuffers[MAX_FRAMES_IN_FLIGHT]{};
		VkSemaphore asyncG2C_[MAX_FRAMES_IN_FLIGHT]{}; // G1 -> C
		VkSemaphore asyncC2G_[MAX_FRAMES_IN_FLIGHT]{}; // C -> G2
		uint64_t asyncG2CValue_[MAX_FRAMES_IN_FLIGHT] = {};
		uint64_t asyncC2GValue_[MAX_FRAMES_IN_FLIGHT] = {};
		bool asyncSplitReady_[MAX_FRAMES_IN_FLIGHT] = {};
		bool EnsureAsyncSplit(uint32_t flight); // false = no dedicated compute
		// Shared frame-command setup (windowed + hosted ctors): pool
		// per flight on the given graphics family.
		void InitFrameCommands(uint32_t graphicsFamily);
		// Hosted-mode "surface": the single offscreen color image.
		void CreateHostedImage();
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

		// Presentation mode: requested (next Create/Recreate) vs active.
		// Immediate default: uncapped, lowest latency. Fallback chain on
		// unsupported: mailbox (tear-free, no FIFO halving) -> relaxed ->
		// spec-guaranteed FIFO.
		VkPresentModeKHR requestedPresentMode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
		VkPresentModeKHR activePresentMode_ = VK_PRESENT_MODE_FIFO_KHR;

		// Per-frame timestamp query pools (2 queries each: frame start/end)
		// for the CPU/GPU time split overlay. Indexed by frame slot
		// (currentFrame_ < MAX_FRAMES_IN_FLIGHT), so they survive Recreate.
		VkQueryPool timestampPools_[MAX_FRAMES_IN_FLIGHT]{};
		bool timestampWritten_[MAX_FRAMES_IN_FLIGHT] = {};
		float timestampPeriod_ = 0.0f; // ns per timestamp tick
		double lastGpuMs_ = 0.0;
		bool gpuTimingValid_ = false;
		double lastPresentWaitMs_ = 0.0;
		void CreateTimestampPools();
		void DestroyTimestampPools();

		void CreateSwapchain();
		void CreateImageViews();
		void CreateDepthResources();
		void CreateSyncObjects();
		void DestroySyncObjects();
		void CleanupSwapchain(bool fullDestroy = false);
		// Blocks while the window is minimized (0x0 client size): the OS
		// queue is pumped via SDL_WaitEvent so restore/resize events can
		// arrive while we wait. Used by Create() and Recreate() before any
		// extent-dependent resource is (re)built.
		void WaitForNonMinimizedExtent();

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
		HashValue(seed, key.msaa.alphaToCoverage);
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

		// In-place replacement at an existing slot (see
		// VulkanResourceManager::RecreateSampler). Keeps the slot index
		// stable; the new description is ALSO mapped to the slot (the old
		// mapping is kept, so per-frame GetOrCreate(defaultCI) calls keep
		// deduping here instead of leaking new slots).
		bool Recreate(uint32_t index, const SamplerCreateInfo& ci);

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
	VkQueue          GetVkComputeQueue();
	VkSurfaceKHR     GetVkSurface();
	uint32_t         GetVkGraphicsQueueFamily();
	uint32_t         GetVkComputeQueueFamily();
	bool             HasDedicatedComputeQueue();
	// True when the device was created with multiDrawIndirect enabled.
	// CommandBuffer::DrawIndexedIndirect uses the single-call fast path
	// then, and falls back to one-by-one emulation otherwise.
	bool             SupportsMultiDrawIndirect();
	uint32_t         GetVkTransferQueueFamily();

}; // namespace smvk

