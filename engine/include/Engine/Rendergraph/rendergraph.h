#pragma once

#include <Engine/smvk.h>
#include <Engine/Renderer/mesh.h>

#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace smvk
{

	struct RenderGraph;
	struct RenderPass;

	struct CommandBuffer final
	{
		CommandBuffer(void* cmdOpaque, RenderGraph& graph, RenderPass& pass);
		void BindRenderPipeline(
			ShaderProgram& shaderProgram,
			VertexFormat vertexFormat = VertexFormat::Empty,
			const DepthSettings& depthSettings =
			DepthSettings::DepthTest(),
			const StencilSettings& stencilSettings =
			StencilSettings::DefaultDisabled(),
			const BlendSettings& blendSettings =
			BlendSettings::Opaque(),
			const MSAASettings& msaaSettings =
			MSAASettings::Count1(),
			const RasterSettings& rasterSettings = {},
			PrimitiveTopology topology = PrimitiveTopology::TriangleList);

		void PushConstants(
			uint32_t offset,
			uint32_t size,
			const void* data,
			ShaderStage stages =
			ShaderStage::AllGraphics);

		void Draw(
			uint32_t vertexCount,
			uint32_t instanceCount = 1,
			uint32_t firstVertex = 0,
			uint32_t firstInstance = 0);

		void DrawIndexed(
			uint32_t indexCount,
			uint32_t instanceCount = 1,
			uint32_t firstIndex = 0,
			int32_t vertexOffset = 0,
			uint32_t firstInstance = 0
		);

		// Binds an arbitrary RHI buffer as the index buffer (UINT32).
		// All index streams (mesh, cluster-ordered, filtered) live in the
		// static index monoblock with absolute firstIndex, so in practice
		// the offset-0 monoblock binding from BindStaticVertexFmt is the
		// only one ever needed (see BindStaticVertexFmt).
		void BindIndexBuffer(BufferHandle buffer, uint64_t offset = 0);

		// Indirect indexed draws consuming VkDrawIndexedIndirectCommand
		// entries produced by the filter compute (see Engine/Renderer/gfx_filter.h).
		// The indirect buffer must have been created with BufferType_Indirect.
		void DrawIndexedIndirect(
			BufferHandle buffer,
			uint64_t offset,
			uint32_t drawCount,
			uint32_t stride);

		void BindComputePipeline(
			ShaderProgram& shaderProgram);

		void Dispatch(
			uint32_t groupCountX,
			uint32_t groupCountY,
			uint32_t groupCountZ);

		void BindStaticVertexFmt();

		// Binds an arbitrary RHI buffer as vertex binding N (pos-only debug
		// streams use binding 0 with VertexFormat::DebugLine).
		void BindVertexBuffer(BufferHandle buffer, uint32_t binding = 0,
			uint64_t offset = 0);

		// Access native VkCommandBuffer for integrations like ImGui.
		void* GetNativeHandle() const { return cmdOpaque_; }

	private:
		void* cmdOpaque_ = nullptr;
		void* currentPipeline_ = nullptr;
		void* currentLayout_ = nullptr;
#ifndef NDEBUG
		// Debug-only: last index buffer bound on this command buffer.
		// DrawIndexed* wrappers assert it is set, catching leaked/stale
		// index bindings across passes (the exploded-vertices class).
		BufferHandle boundIndexBuffer_{};
#endif
		RenderGraph* graph_ = nullptr;
		RenderPass* renderPass_ = nullptr;

		// ---- Graphics-bind fast path -------------------------------
		// BindRenderPipeline used to pay the full pipeline-cache lookup
		// (format copy + several vector allocs + a ~100-way hash combine)
		// even when the requested pipeline was already bound. Recorders
		// rebind per phase and per material, so the redundant case is
		// common. A CommandBuffer is per-pass (RenderGraph::Execute) and
		// attachment formats are pass-invariants, so remembering the raw
		// input state of the last graphics bind in this pass is enough to
		// skip the lookup and the vkCmdBind* re-issue entirely.
		bool graphicsBound_ = false;
		ShaderProgram boundProgram_{};
		VertexFormat boundVertexFormat_ = VertexFormat::Empty;
		DepthSettings boundDepth_{};
		StencilSettings boundStencil_{};
		BlendSettings boundBlend_{};
		MSAASettings boundMsaa_{};
		RasterSettings boundRaster_{};
		PrimitiveTopology boundTopology_ = PrimitiveTopology::TriangleList;
		// Last graphics bundle: a redundant bind after a compute bind
		// (which overwrites currentPipeline_) restores the cached
		// pointers without re-recording vkCmdBindPipeline — the graphics
		// bind point still holds the right pipeline.
		void* graphicsPipeline_ = nullptr;
		void* graphicsLayout_ = nullptr;
	};

	using LogicalBufferHandle =
		Handle<struct LogicalBuffer>;

	using LogicalImageHandle =
		Handle<struct LogicalImage>;

	using LogicalImageViewHandle =
		Handle<struct LogicalImageView>;

	// ============================================================
	// Logical resource descriptions
	// ============================================================

	using BufferKey = BufferCreateInfo;
	using ImageKey = ImageCreateInfo;
	struct ImageViewKey
	{
		// cant reuse ImageCreateInfo beacuse it lacks handle property. Otherwise should be the same as this
		LogicalImageHandle image{};
		ImageViewType viewType =
			ImageViewType::View2D;
		Format format =
			Format::Undefined;
		ImageSubresourceRange subresourceRange{};
		bool operator==(const ImageViewKey&) const = default;
	};


	// ============================================================
	// Logical resources
	// ============================================================

	enum class ResourceType : uint8_t
	{
		Transient,
		External
	};


	struct LogicalBuffer
	{
		ResourceType type =
			ResourceType::Transient;
		BufferKey key{};
		BufferHandle resolvedBufferHandle{};
		const char* name = "";
	};


	struct LogicalImage
	{
		ResourceType type =
			ResourceType::Transient;
		ImageKey key{};
		ImageHandle resolvedImageHandle{};
		const char* name = "";
	};


	struct LogicalImageView
	{
		enum class Type : uint8_t
		{
			Logical,
			ExternalPhysical
		};
		Type type = Type::Logical;
		ImageViewKey key{};
		ImageViewHandle resolvedViewHandle{};
		ImageViewHandle externalViewHandle{};
		Format externalFormat = Format::Undefined;
		const char* name = "";
	};


	// ============================================================
	// Render attachments
	// ============================================================

	struct RenderAttachment
	{
		LogicalImageViewHandle view{};
		AttachmentLoadOp loadOp =
			AttachmentLoadOp::DontCare;
		AttachmentStoreOp storeOp =
			AttachmentStoreOp::Store;
		ClearColor clearColor{};
		// Optional MSAA resolve destination (single-sampled). When set,
		// Execute() fills VkRenderingAttachmentInfo::resolveImageView with
		// resolveMode AVERAGE. view is then the multisampled image whose
		// storeOp should typically be DontCare.
		LogicalImageViewHandle resolveView{};
	};


	struct DepthAttachment
	{
		LogicalImageViewHandle view{};
		AttachmentLoadOp loadOp =
			AttachmentLoadOp::DontCare;
		AttachmentStoreOp storeOp =
			AttachmentStoreOp::Store;
		ClearDepthStencil clear{};
	};


	// ============================================================
	// Sync hints for bindless / explicit resources
	// ============================================================
	struct ImageHint {
		LogicalImageViewHandle view{};
		ImageSyncRequest request{};
	};
	struct BufferHint {
		LogicalBufferHandle buffer{};
		BufferSyncRequest request{};
	};

	// ============================================================
	// Render pass
	// ============================================================

	using RecordingFunction =
		std::function<void(CommandBuffer&)>;

	struct RenderPass final
	{
		explicit RenderPass(
			const char* name = nullptr)
			: name_(name)
		{}

		// --------------------------------------------------------
		// Attachments
		// --------------------------------------------------------

		RenderPass& SetColor(
			const std::vector<LogicalImageViewHandle>& views,
			AttachmentLoadOp loadOp =
			AttachmentLoadOp::Load,
			AttachmentStoreOp storeOp =
			AttachmentStoreOp::Store,
			ClearColor clearValue = {})
		{
			colorAttachments_.resize(views.size());

			for (size_t i = 0; i < views.size(); ++i)
			{
				assert(!views[i].empty());

				colorAttachments_[i] = {
					.view = views[i],
					.loadOp = loadOp,
					.storeOp = storeOp,
					.clearColor = clearValue,
				};
			}

			return *this;
		}

		// MSAA variant: views are multisampled, resolveViews are the
		// single-sampled resolve destinations (same count, may be empty
		// per-index for no-resolve). resolveViews must match views in size
		// when provided.
		RenderPass& SetColorWithResolve(
			const std::vector<LogicalImageViewHandle>& views,
			const std::vector<LogicalImageViewHandle>& resolveViews,
			AttachmentLoadOp loadOp =
			AttachmentLoadOp::Clear,
			AttachmentStoreOp storeOp =
			AttachmentStoreOp::DontCare,
			ClearColor clearValue = {})
		{
			assert(resolveViews.empty() || resolveViews.size() == views.size());

			colorAttachments_.resize(views.size());

			for (size_t i = 0; i < views.size(); ++i)
			{
				assert(!views[i].empty());

				colorAttachments_[i] = {
					.view = views[i],
					.loadOp = loadOp,
					.storeOp = storeOp,
					.clearColor = clearValue,
					.resolveView = resolveViews.empty() ? LogicalImageViewHandle{} : resolveViews[i],
				};
			}

			return *this;
		}


		RenderPass& SetDepth(
			LogicalImageViewHandle view,
			AttachmentLoadOp loadOp =
			AttachmentLoadOp::Load,
			AttachmentStoreOp storeOp =
			AttachmentStoreOp::Store,
			ClearDepthStencil clearValue = {})
		{
			assert(!view.empty());

			depthAttachment_ = {
				.view = view,
				.loadOp = loadOp,
				.storeOp = storeOp,
				.clear = clearValue,
			};

			hasDepthAttachment_ = true;

			return *this;
		}


		// --------------------------------------------------------
		// Shader resources (legacy sugar – derives sync hints)
		// --------------------------------------------------------

		RenderPass& SetInputImages(
			std::vector<LogicalImageViewHandle> images)
		{
			inputImages_ = std::move(images);
			// Also populate sync hints with derived stage/layout if not already hinted.
			for (auto v : inputImages_) {
				bool exists = false;
				for (auto &h : imageHints_) if (h.view == v) { exists = true; break; }
				if (!exists) {
					ImageSyncRequest req{};
					req.barrierMasks.accessMask = AccessFlags::ShaderRead;
					req.barrierMasks.stageMask = DerivePipelineStage(req.barrierMasks.accessMask);
					req.layout = ImageLayout::ShaderReadOnlyOptimal;
					imageHints_.push_back({ v, req });
				}
			}
			return *this;
		}


		RenderPass& SetStorageImages(
			std::vector<LogicalImageViewHandle> images)
		{
			storageImages_ = std::move(images);
			for (auto v : storageImages_) {
				bool exists = false;
				for (auto &h : imageHints_) if (h.view == v) { exists = true; break; }
				if (!exists) {
					ImageSyncRequest req{};
					req.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
					req.barrierMasks.stageMask = DerivePipelineStage(req.barrierMasks.accessMask);
					req.layout = ImageLayout::General;
					imageHints_.push_back({ v, req });
				}
			}
			return *this;
		}


		RenderPass& SetStorageBuffers(
			std::vector<LogicalBufferHandle> buffers)
		{
			storageBuffers_ = std::move(buffers);
			for (auto b : storageBuffers_) {
				bool exists = false;
				for (auto &h : bufferHints_) if (h.buffer == b) { exists = true; break; }
				if (!exists) {
					BufferSyncRequest req{};
					req.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
					req.barrierMasks.stageMask = DerivePipelineStage(req.barrierMasks.accessMask);
					bufferHints_.push_back({ b, req });
				}
			}
			return *this;
		}

		// --------------------------------------------------------
		// Explicit sync hints (bindless model)
		// --------------------------------------------------------
		RenderPass& AddImageHint(LogicalImageViewHandle view, ImageSyncRequest request)
		{
			assert(!view.empty());
			if (request.barrierMasks.stageMask == PipelineStageBit_None)
				request.barrierMasks.stageMask = DerivePipelineStage(request.barrierMasks.accessMask);
			// deduplicate – replace if exists
			for (auto &h : imageHints_) if (h.view == view) { h.request = request; return *this; }
			imageHints_.push_back({ view, request });
			return *this;
		}
		RenderPass& AddBufferHint(LogicalBufferHandle buffer, BufferSyncRequest request)
		{
			assert(!buffer.empty());
			if (request.barrierMasks.stageMask == PipelineStageBit_None)
				request.barrierMasks.stageMask = DerivePipelineStage(request.barrierMasks.accessMask);
			for (auto &h : bufferHints_) if (h.buffer == buffer) { h.request = request; return *this; }
			bufferHints_.push_back({ buffer, request });
			return *this;
		}
		// Batch variants
		RenderPass& AddImageHints(const std::vector<std::pair<LogicalImageViewHandle, ImageSyncRequest>>& hints)
		{
			for (auto &p : hints) AddImageHint(p.first, p.second);
			return *this;
		}
		RenderPass& AddBufferHints(const std::vector<std::pair<LogicalBufferHandle, BufferSyncRequest>>& hints)
		{
			for (auto &p : hints) AddBufferHint(p.first, p.second);
			return *this;
		}


		// --------------------------------------------------------
		// Render area
		// --------------------------------------------------------

		RenderPass& SetExtent(
			Extent2D extent)
		{
			extent_ = extent;
			return *this;
		}


		RenderPass& SetOffset(
			int32_t x,
			int32_t y)
		{
			offsetX_ = x;
			offsetY_ = y;
			return *this;
		}


		RenderPass& SetRecordingFunc(
			RecordingFunction fn)
		{
			recordingFunction_ =
				std::move(fn);

			return *this;
		}

		// --------------------------------------------------------
		// Compute support (Forward+ light culling, etc.)
		// --------------------------------------------------------
		// Marks this pass as compute: Execute() skips BeginRendering
		// and runs the recording func directly (barriers still emitted
		// from image/buffer hints). Compute passes must have no color
		// or depth attachments.
		RenderPass& MarkAsCompute()
		{
			isCompute_ = true;
			return *this;
		}

		[[nodiscard]] bool IsCompute() const { return isCompute_; }


	private:
		friend RenderGraph;
		friend CommandBuffer;

		// Shader-readable resources (legacy, also drives imageHints_ defaults).
		std::vector<LogicalImageViewHandle>
			inputImages_;

		// Read/write shader resources.
		std::vector<LogicalImageViewHandle>
			storageImages_;

		std::vector<LogicalBufferHandle>
			storageBuffers_;

		// Explicit sync hints – primary source for barrier generation.
		std::vector<ImageHint>
			imageHints_;
		std::vector<BufferHint>
			bufferHints_;

		// Render targets.
		std::vector<RenderAttachment>
			colorAttachments_;

		DepthAttachment
			depthAttachment_{};

		bool hasDepthAttachment_ =
			false;

		Extent2D extent_{};

		int32_t offsetX_ = 0;
		int32_t offsetY_ = 0;

		RecordingFunction
			recordingFunction_{};

		bool isCompute_ = false;

		const char* name_ = nullptr;
	};



	// ============================================================
	// Render graph
	// ============================================================

	class ResourceManager;
	struct Framebuffer;
	struct RenderGraph final
	{
		explicit RenderGraph(
			ResourceManager& ctx);

		~RenderGraph();

		// Targeted teardown of a single external view/image. Used to refresh
		// swapchain color views on Recreate() without clearing every external
		// resource the graph holds.
		void FreeImageView(
			LogicalImageViewHandle handle);

		void FreeImage(
			LogicalImageHandle handle);

		void FreeBuffer(
			LogicalBufferHandle handle);

		// Destroys resolved views then transient images so each parent image's
		// liveViewCount reaches zero before the image is released. External
		// resources are owned by the caller and are left untouched.
		void ReleaseResolvedResources();

		// --------------------------------------------------------
		// Frame
		// --------------------------------------------------------

		void NewFrame()
		{
			passes_.clear();
		}


		RenderPass& AddPass(
			const char* name = nullptr)
		{
			return passes_.emplace_back(name);
		}

		RenderPass& AddComputePass(
			const char* name = nullptr)
		{
			RenderPass& pass = passes_.emplace_back(name);
			pass.MarkAsCompute();
			return pass;
		}


		// ========================================================
		// Images
		// ========================================================

		[[nodiscard]]
		LogicalImageHandle AddImage(
			const ImageCreateInfo& description,
			const char* name = "");

		[[nodiscard]]
		LogicalImageHandle AddExternalImage(
			ImageHandle imageHandle,
			const char* name = "");


		// ========================================================
		// Image views
		// ========================================================

		[[nodiscard]]
		LogicalImageViewHandle AddImageView(
			LogicalImageHandle image,
			ImageViewType type =
			ImageViewType::View2D,
			Format format =
			Format::Undefined,
			ImageSubresourceRange range = {},
			const char* name = "")
		{
			assert(!image.empty());

			LogicalImageView view{
				.key = {
					.image =
						image,

					.viewType =
						type,

					.format =
						format,

					.subresourceRange =
						range,
				},

				.name =
					name,
			};

			return imageViews_.create(
				std::move(view));
		}

		LogicalImageViewHandle AddExternalImageView(
			ImageViewHandle imageView,
			Format format,
			const char* name = "")
		{
			assert(!imageView.empty());

			LogicalImageView view{
				.type = LogicalImageView::Type::ExternalPhysical,
				.externalViewHandle = imageView,
				.externalFormat = format,
				.name = name,
			};

			return imageViews_.create(std::move(view));
		}

		[[nodiscard]]
		LogicalImageViewHandle AddExternalImageView(
			ImageHandle image,
			ImageViewType type =
			ImageViewType::View2D,
			Format format =
			Format::Undefined,
			ImageSubresourceRange range = {},
			const char* name = "")
		{
			auto logicalImage =
				AddExternalImage(
					image,
					name);

			return AddImageView(
				logicalImage,
				type,
				format,
				range,
				name);
		}


		// ========================================================
		// Buffers
		// ========================================================

		template<typename T>
		[[nodiscard]]
		LogicalBufferHandle AddBuffer(
			uint32_t count,
			BufferType usage =
			BufferType_Storage,
			const char* name = "")
		{
			LogicalBuffer buffer{
				.type =
					ResourceType::Transient,

				.key = {
					.size =
						sizeof(T) * count,

					.bufferType =
						usage,

					.name =
						name,
				},

				.resolvedBufferHandle = {},

				.name =
					name,
			};

			return buffers_.create(
				std::move(buffer));
		}


		[[nodiscard]]
		LogicalBufferHandle AddExternalBuffer(
			BufferHandle bufferHandle,
			const char* name = "")
		{
			assert(!bufferHandle.empty());

			LogicalBuffer buffer{
				.type =
					ResourceType::External,

				.resolvedBufferHandle =
					bufferHandle,

				.name =
					name,
			};

			return buffers_.create(
				std::move(buffer));
		}


		// ========================================================
		// Resolution
		// ========================================================

		[[nodiscard]]
		BufferHandle GetResolvedBufferHandle(
			LogicalBufferHandle handle) const
		{
			assert(!handle.empty());

			auto* buffer =
				buffers_.get(handle);

			assert(buffer != nullptr);

			return buffer->resolvedBufferHandle;
		}


		[[nodiscard]]
		ImageHandle GetResolvedImageHandle(
			LogicalImageHandle handle) const
		{
			assert(!handle.empty());

			auto* image =
				images_.get(handle);

			assert(image != nullptr);

			return image->resolvedImageHandle;
		}

		[[nodiscard]]
		Format GetResolvedImageViewFormat(
			LogicalImageViewHandle handle) const
		{
			assert(!handle.empty());

			const auto* view = imageViews_.get(handle);
			assert(view != nullptr);

			if (view->type == LogicalImageView::Type::ExternalPhysical)
			{
				assert(view->externalFormat != Format::Undefined);
				return view->externalFormat;
			}

			if (view->key.format != Format::Undefined)
			{
				return view->key.format;
			}

			const auto* image =
				images_.get(view->key.image);

			assert(image != nullptr);
			assert(image->key.format != Format::Undefined);

			return image->key.format;
		}


		// ========================================================
		// Execution
		// ========================================================

		void Execute(Framebuffer& fb);

		// --------------------------------------------------------
		// Resource resolution
		// --------------------------------------------------------

		void ResolveImages();
		void ResolveBuffers();
		void ResolveImageViews();

		// --------------------------------------------------------
		// Sync – sorting & barrier generation (local bookkeeping)
		// --------------------------------------------------------
	private:
		// Collect per-pass physical image/buffer usage with derived layout/access.
		struct PassUsage {
			std::vector<std::pair<ImageHandle, ImageSyncRequest>> images; // resolved physical
			std::vector<std::pair<BufferHandle, BufferSyncRequest>> buffers;
		};
		std::vector<PassUsage> CollectPassUsages() const;
		static bool IsWriteImage(const ImageSyncRequest& r) { return IsWriteAccess(r.barrierMasks.accessMask); }
		static bool IsWriteBuffer(const BufferSyncRequest& r) { return IsWriteAccess(r.barrierMasks.accessMask); }
		// Returns an internally-owned order (reused across frames): the
		// sort used to rebuild its adjacency, indegree and frontier
		// buffers — and re-sort the whole ready set on every release —
		// once per Execute.
		const std::vector<size_t>& TopologicalSort(const std::vector<PassUsage>& usages) const;
		void EmitBarriers(void* cmdOpaque, const std::vector<size_t>& sortedOrder, const std::vector<PassUsage>& usages);

		// TopologicalSort scratch (mutable: reused across frames).
		mutable std::vector<size_t> topoSorted_;
		mutable std::vector<size_t> topoHeap_;
		mutable std::vector<std::vector<size_t>> topoAdj_;
		mutable std::vector<int> topoIndeg_;
		mutable std::unordered_map<uint64_t, size_t> topoLastWriterImage_;
		mutable std::unordered_map<uint64_t, std::vector<size_t>> topoLastReadersImage_;
		mutable std::unordered_map<uint64_t, size_t> topoLastWriterBuffer_;
		mutable std::unordered_map<uint64_t, std::vector<size_t>> topoLastReadersBuffer_;

	public:

		// --------------------------------------------------------
		// Graph storage
		// --------------------------------------------------------

		Pool<LogicalImage, LogicalImage>
			images_;
		Pool<LogicalImageView, LogicalImageView>
			imageViews_;
		Pool<LogicalBuffer, LogicalBuffer>
			buffers_;
		std::vector<RenderPass>
			passes_;

		ResourceManager& ctx_;
		struct PipelineCacheOpaque;
		std::unique_ptr<PipelineCacheOpaque> pipelineCacheOpaque_;
	};


} // namespace smvk
