#pragma once

#include <vector>
#include <string_view> // Entry point 
#include <cstring>     // compare and hash

#include <Engine/Vulkan/impl.h> //
//#include <Engine/smvk.h> // -> this should be enough
#include <Engine/Renderer/mesh.h>

namespace smvk {
	struct RenderGraph;
	struct CommandBuffer;
	//
	using LogicalBufferHandle = Handle<struct LogicalBuffer>;
	using LogicalImageHandle = Handle<struct LogicalImage>;
	using LogicalImageViewHandle = Handle<struct LogicalImageView>;
	using LogicalStorageImageHandle = Handle<struct LogicalStorageImage>;
};

namespace smvk {

	struct BufferKey {
		size_t elementSize;
		size_t elementCount;
		// Maybe use and type flags could be better?
		// to support uniform/storage and dynamic or not, things like that
		// right now just assuminng a lot of things to get a implementation
		// not that configurable. that could prove to be a good thing.
		// perf gains from mangling all that could be negligible on modern hw.
		bool ring; // each frame has its own size*count
	};

	using ImageKey = ImageCreateInfo;
	struct ImageViewKey {
		LogicalImageHandle image;
		VkImageViewCreateFlags     flags{};
		VkImageViewType            viewType{};
		VkImageSubresourceRange    subResourceRange{};
	};

	struct LogicalBuffer
	{
		enum struct Type {
			External,
			Transient
		} type;
		BufferKey key{};
		BufferHandle resolvedBufferHandle;
		const char* name = "";
	};

	struct LogicalImageView
	{
		enum struct Type {
			External,
			Transient
		} type;
		ImageViewKey key{};
		// We are just rawdogging VkImageView opaque handle here because my Image resource
		// owns the views directly. so no ImageViewHandle really.
		VkImageView resolvedImageView = VK_NULL_HANDLE;
		VkFormat resolvedFormat = VK_FORMAT_UNDEFINED;
		const char* name = "";
	};

	struct LogicalImage {
		enum struct Type {
			Transient,
			External
		} type;
		ImageKey key{};
		ImageHandle resolvedImageHandle{};
	};

	//struct AttachmentBlendSetup {};
	using AttachmentColorBlendSetup = VkPipelineColorBlendAttachmentState;
	//using AttachmentDepthBlendSetup = VkPipelineColorBlendAttachmentState;

	struct RenderPassDescription {

		struct ColorAttachment {
			VkFormat vkColorFormat = VK_FORMAT_UNDEFINED;
			AttachmentColorBlendSetup blend;
		} color[6]{};
		VkFormat vkDepthFormat = VK_FORMAT_UNDEFINED;
		VkPrimitiveTopology vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		bool depthTestEnable = VK_TRUE;
		bool depthWriteEnable = VK_TRUE;
		VkCompareOp depthCompareOp = VK_COMPARE_OP_GREATER;
		bool stencilTestEnable = VK_FALSE;
		bool stencilWriteEnable = VK_FALSE;
		uint32_t frontStencilCompareOp = VK_COMPARE_OP_ALWAYS;
		uint32_t backStencilCompareOp = VK_COMPARE_OP_ALWAYS;
		uint32_t frontStencilFailOp = VK_STENCIL_OP_KEEP;
		uint32_t backStencilFailOp = VK_STENCIL_OP_KEEP;
		uint32_t frontStencilPassOp = VK_STENCIL_OP_KEEP;
		uint32_t backStencilPassOp = VK_STENCIL_OP_KEEP;
		VkStencilFaceFlags frontStencilFace = VK_STENCIL_FACE_FRONT_BIT;
		VkStencilFaceFlags backStencilFace = VK_STENCIL_FACE_BACK_BIT;
		//uint32_t patchControlPoints = 0;
		//uint32_t samplesCount = 1u;
		//float minSampleShading = 0.0f;
	};

	using RecordingFunction = std::function<void(CommandBuffer&)>;
	struct RenderPass final {
		explicit RenderPass(const char* name = nullptr) { name_ = name; }
		struct Attachment {
			LogicalImageViewHandle view;
			VkAttachmentLoadOp loadOp;
			VkAttachmentStoreOp storeOp;
			VkClearValue clearValue;
		};
		//struct AttachmentBlendSetup {
		//	VkPipelineColorBlendAttachmentState state;
		//};

		RenderPass& setColor(
			std::vector<LogicalImageViewHandle>& views,
			VkAttachmentLoadOp loadOp,
			VkAttachmentStoreOp storeOp,
			VkClearValue clearValue
		) {
			this->colorAttachments_.resize(views.size());
			for (size_t index = 0; index < views.size(); index++)
			{
				assert(!views[index].empty() && "setColor() called with empty image view");
				this->colorAttachments_[index] = { views[index], loadOp, storeOp, clearValue };
			}
			return *this;

		}
		RenderPass& setDepth(
			LogicalImageViewHandle logicalImageViewHandle,
			VkAttachmentLoadOp loadOp,
			VkAttachmentStoreOp storeOp,
			VkClearValue clearValue
		) {
			assert(!logicalImageViewHandle.empty() && "setDepth() called with empty image view");
			this->depthAttachment_ = { logicalImageViewHandle, loadOp, storeOp, clearValue };
			return *this;
		};

		// Resources
		RenderPass& setInputImages(std::vector<LogicalImageHandle> inputImages) { this->inputImages_ = std::move(inputImages); return *this; };
		RenderPass& setStorageBuffers(std::vector<LogicalBufferHandle> storageBuffers) { this->storageBuffers_ = std::move(storageBuffers); return *this; };
		RenderPass& setStorageImages(std::vector<LogicalStorageImageHandle> storageImages) { this->storageImages_ = std::move(storageImages); return *this; };

		// Settings
		RenderPass& setExtent(VkExtent2D extent) { this->extent_ = extent; return *this; };
		RenderPass& setOffset(VkOffset2D offset) { this->offset_ = offset; return *this; };
		//RenderPass& setVertexFormat(VertexFormatDesc vertexFormat) { this->vertexFormat_ = vertexFormat; return *this; }
		// 
		RenderPass& setRecordingFunc(RecordingFunction recordingFunction) { this->recordingFunction_ = recordingFunction; return *this; };


	private:
		friend RenderGraph;
		friend struct CommandBuffer;
		//
		std::vector<LogicalImageHandle> inputImages_;
		std::vector<LogicalBufferHandle> storageBuffers_;
		std::vector<LogicalStorageImageHandle> storageImages_;
		VkExtent2D extent_ = { 0,0 };
		VkOffset2D offset_ = { 0,0 };
		std::vector<Attachment> colorAttachments_{};
		std::vector<AttachmentColorBlendSetup> colorAttachmentsBlendSetups;  // should either be {} or match colors size.
		Attachment depthAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = { 1.0f } /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE */ };
		//
		RecordingFunction recordingFunction_{};
		const char* name_;
	};

	struct CommandBuffer /*: ICommandBuffer*/ final {
	public:
		CommandBuffer() = delete;

		void BindRenderPipeline(
			ShaderProgram& shaderProgram, 
			VertexFormat vertexFormat,
			const smvk::DepthSettings& depthSettings,
			const smvk::StencilSettings& stencilSettings,
			const smvk::BlendSettings& blendSettings,
			const smvk::MSAASettings& msaaSettings
		);
		void PushConstants(uint32_t offset, uint32_t size, const void* data, PipelineStage stageMask = PipelineStageBit_AllGraphics);

		void SetViewport(const VkViewport& viewport);
		void SetScissor(const VkRect2D& scissor);

		void Draw(
			uint32_t vertexCount,
			uint32_t instanceCount = 1,
			uint32_t firstVertex = 0,
			uint32_t firstInstance = 0);
		void DrawIndexed(
			uint32_t indexCount,
			uint32_t instanceCount = 1,
			uint32_t firstIndex = 0,
			uint32_t baseVertex = 0,
			uint32_t firstInstance = 0
		);

		VkCommandBuffer GetVkCommandBuffer() const { return cmd_; }

	private:
		friend struct RenderGraph;

		CommandBuffer(
			VkCommandBuffer cmd,
			RenderGraph& graph,
			RenderPass& renderPass);



		VkCommandBuffer cmd_ = VK_NULL_HANDLE;
		RenderGraph* graph_ = nullptr;
		RenderPass* renderPass_ = nullptr;

		VkPipeline currentPipeline_ = VK_NULL_HANDLE;
		VkPipelineLayout currentPipelineLayout_ = VK_NULL_HANDLE;
	};

	struct RenderGraph final {
		explicit RenderGraph(VulkanResourceManager& ctx) : ctx_(ctx) {
			pipelineCachePimpl = std::make_unique<PipelineCache>(&ctx_);
		};
		void NewFrame() { m_passes.clear(); }
		RenderPass& AddPass(const char* name = nullptr) {
			return m_passes.emplace_back(RenderPass(name));
		}
		[[nodiscard]] LogicalImageHandle AddImage(ImageCreateInfo description) { assert(false); };
		[[nodiscard]] LogicalImageHandle AddImage(ImageHandle imageHandle) {
			assert(!imageHandle.empty());
			auto img = ctx_.images_.get(imageHandle);
			LogicalImage proxyImage{
				.type = LogicalImage::Type::External,
				.key = img->getImageCreateInfo(),
				.resolvedImageHandle = imageHandle
			};
			return inputImages_.create(std::move(proxyImage));
		};
		[[nodiscard]] LogicalImageViewHandle AddImageView(ImageViewCreateInfo description) { assert(false); };
		[[nodiscard]] LogicalImageViewHandle AddImageView(VkImageView imageView, VkFormat vkFormat, const char* debugName = nullptr) {
			LogicalImageView proxyView{};
			proxyView.type = LogicalImageView::Type::External;
			proxyView.resolvedImageView = imageView;
			proxyView.resolvedFormat = vkFormat;
			proxyView.name = debugName;
			return imageViews_.create(std::move(proxyView));
		};
		[[nodiscard]] LogicalImageViewHandle AddImageView(ImageHandle imageHandle, const char* debugName = nullptr) {
			auto img = ctx_.images_.get(imageHandle);
			assert(img != nullptr);
			LogicalImageView proxyView{};
			proxyView.type = LogicalImageView::Type::External;
			proxyView.key.image = AddImage(imageHandle);
			//proxyView.key.viewType; 
			//proxyView.key.subResourceRange;
			proxyView.resolvedImageView = img->sampleView;
			proxyView.resolvedFormat = img->vk_format;
			proxyView.name = debugName;
			return imageViews_.create(std::move(proxyView));
		};
		template<typename DataType>
		[[nodiscard]] LogicalBufferHandle AddRingBuffer(uint32_t count, const char* name = "") {
			BufferKey key = {
					.elementSize = sizeof(DataType),
					.elementCount = count,
					.ring = true,
			};
			auto buf = LogicalBuffer{
				.type = LogicalBuffer::Type::Transient,
				.key = key,
				.name = name
			};
			return storageBuffers_.create(std::move(buf));
		}
		template<typename DataType>
		[[nodiscard]] LogicalBufferHandle AddBuffer(uint32_t count, const char* name = "") {
			BufferKey key = {
					.elementSize = sizeof(DataType),
					.elementCount = count
			};
			LogicalBuffer proxyBuffer = {
			   .type = LogicalBuffer::Type::Transient,
			   .key = key,
			   .name = name
			};
			return storageBuffers_.create(std::move(proxyBuffer));
		};
		[[nodiscard]] std::vector<LogicalBufferHandle> AddExternalBuffers(
			std::vector<BufferHandle> bufferHandles
			//, BufferKey* keys, // could be used to mark them as rings but .... maybe not.
			//int keyCount
		) {
			std::vector<LogicalBufferHandle> handles;
			for (auto b : bufferHandles) {
				LogicalBuffer proxy{
					.type = LogicalBuffer::Type::External,
					.resolvedBufferHandle = b
				};
				handles.push_back(storageBuffers_.create(std::move(proxy)));
			}
			return handles;
		}

		smvk::BufferHandle GetResolvedBufferHandle(LogicalBufferHandle handle) const {
			return storageBuffers_.get(handle)->resolvedBufferHandle;
		};
		smvk::ImageHandle GetResolvedImageHandle(LogicalImageHandle handle) const {
			return inputImages_.get(handle)->resolvedImageHandle;
		};

		void Execute(FrameBuffer& fb);

	private:
		friend struct CommandBuffer;

		void ResolveImages() {
			inputImages_.forEach(
				[this](LogicalImage& logicalImage) {
				if (logicalImage.resolvedImageHandle.empty()) {
					if (logicalImage.type == LogicalImage::Type::External) { assert(false && "external image unresolved. shouldnt be possible"); };
					//CreateTransientImaeg(key, sync_req);
					logicalImage.resolvedImageHandle = ctx_.CreateImage(logicalImage.key, nullptr).release();
				};
			}
			);

			inputImages_.forEach(
				[this](LogicalImage& logicalImage) {
				if (logicalImage.resolvedImageHandle.empty()) {
					if (logicalImage.type == LogicalImage::Type::External) { assert(false && "external image unresolved. shouldnt be possible"); };
					//CreateTransientImaeg(key, sync_req);
					logicalImage.resolvedImageHandle = ctx_.CreateImage(logicalImage.key, nullptr).release();
				};
			}
			);

		};
		void ResolveImageViews() {
			// needs resolved images first
			imageViews_.forEach(
				[this](LogicalImageView& logicalView) {
				if (logicalView.resolvedImageView == VK_NULL_HANDLE) {
					auto logicalImage = inputImages_.get(logicalView.key.image);
					auto img = ctx_.images_.get(logicalImage->resolvedImageHandle);
					logicalView.resolvedFormat = img->vk_format;
					logicalView.resolvedImageView = img->sampleView;

				};
			}
			);
		};
		void ResolveBuffers() {
			storageBuffers_.forEach(
				[this](LogicalBuffer& logicalBuffer) {
				if (logicalBuffer.resolvedBufferHandle.empty()) {
					const auto& key = logicalBuffer.key;
					int ringMultiplier = key.ring ? 4 : 1;
					logicalBuffer.resolvedBufferHandle = ctx_.CreateBuffer(
						key.elementCount * key.elementSize * ringMultiplier,
						smvk::BufferType_Storage,
						"resolved buffer"
					).release();
				}
			}
			);
		}

		//
		smvk::Pool<LogicalImage, LogicalImage> inputImages_;
		smvk::Pool<LogicalStorageImage, LogicalImage> storageImages_;
		smvk::Pool<LogicalImageView, LogicalImageView> imageViews_;
		smvk::Pool<LogicalBuffer, LogicalBuffer> storageBuffers_;
		//
		std::vector<RenderPass> m_passes;
		std::unique_ptr<PipelineCache> pipelineCachePimpl;
		smvk::VulkanResourceManager& ctx_;

	};

	CommandBuffer::CommandBuffer(
		VkCommandBuffer cmd,
		RenderGraph& graph,
		RenderPass& renderPass)
		: cmd_(cmd)
		, graph_(&graph)
		, renderPass_(&renderPass)
	{}

	void CommandBuffer::BindRenderPipeline(
		ShaderProgram& shaderProgram,
		VertexFormat vertexFormat = VertexFormat::Empty,
		const smvk::DepthSettings& depthSettings = DepthSettings::DepthTest(),
		const smvk::StencilSettings& stencilSettings = StencilSettings::DefaultDisabled(),
		const smvk::BlendSettings& blendSettings = BlendSettings::Opaque(),
		const smvk::MSAASettings& msaaSettings = MSAASettings::Count1()
	)
	{
		assert(graph_ != nullptr);
		assert(renderPass_ != nullptr);

		VertexFormatDescription formatDescription{};
		switch (vertexFormat)
		{
		case VertexFormat::StaticVertexFormat:
			smvk::BindStaticVertexFmtBuffers(this->graph_->ctx_, this->cmd_);
			formatDescription = StaticVertexFormat;
			break;
			//case VertexFormat::Empty:
			//	break;
		default:
			break;
		}

		std::vector<VkFormat> colorAttachmentFormats;
		colorAttachmentFormats.reserve(renderPass_->colorAttachments_.size());

		for (auto& attachment : renderPass_->colorAttachments_) {
			colorAttachmentFormats.push_back(
				graph_->imageViews_.get(attachment.view)->resolvedFormat);
		}

		smvk::Format depthFormat = smvk::Format::Undefined;
		smvk::Format stencilFormat = smvk::Format::Undefined;
		const bool hasDepth = !renderPass_->depthAttachment_.view.empty();

		if (hasDepth) {
			smvk::Format format = graph_->imageViews_
				.get(renderPass_->depthAttachment_.view)
				->resolvedFormat;

			if (FormatHasDepth(format))
				depthFormat = format;

			if (FormatHasStencil(format))
				stencilFormat = format;
		}

		// Map DepthSettings to pipeline state fields
		bool depthTestEnable = depthSettings.depthTestEnable;
		bool depthWriteEnable = depthSettings.writeEnable ? VK_TRUE : VK_FALSE;
		VkCompareOp depthCompareOp = smvk::toVkCompareOp(depthSettings.depthFunc);
		bool stencilTestEnable = stencilSettings.frontTestEnable || stencilSettings.backTestEnable;
		bool stencilWriteEnable = stencilSettings.frontWriteEnable || stencilSettings.backWriteEnable;
		VkCompareOp frontStencilCompareOp = smvk::toVkCompareOp(stencilSettings.frontCompareOp);
		VkCompareOp backStencilCompareOp = smvk::toVkCompareOp(stencilSettings.backCompareOp);
		uint32_t frontStencilFailOp = stencilSettings.frontFailOp;
		uint32_t backStencilFailOp = stencilSettings.backFailOp;
		uint32_t frontStencilPassOp = stencilSettings.frontPassOp;
		uint32_t backStencilPassOp = stencilSettings.backPassOp;
		VkStencilFaceFlags stencilFace = stencilSettings.frontFace;
		auto bundle = graph_->pipelineCachePimpl->GetRenderPipeline(
			shaderProgram,
			formatDescription,
			colorAttachmentFormats.data(),
			static_cast<uint32_t>(colorAttachmentFormats.size()),
			smvk::toVkFormat(depthFormat),
			smvk::toVkFormat(stencilFormat),
			VK_POLYGON_MODE_FILL,
			VK_CULL_MODE_BACK_BIT,
			VK_FRONT_FACE_COUNTER_CLOCKWISE,
			depthTestEnable,
			depthWriteEnable,
			smvk::toVkCompareOp(depthCompareOp),
			stencilSettings,
			msaaSettings
		);
		assert(bundle.pipeline != VK_NULL_HANDLE && "Invalid pipeline handle");

		if (bundle.pipeline == currentPipeline_) {
			return;
		}

		currentPipeline_ = bundle.pipeline;
		currentPipelineLayout_ = bundle.layout;

		vkCmdBindPipeline(
			cmd_,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			currentPipeline_);

		vkCmdBindDescriptorSets(
			cmd_,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			graph_->ctx_.defaultPipeLayout,
			0,
			1,
			graph_->ctx_.defaultDescriptorSets_,
			0,
			nullptr);
	}

	inline void CommandBuffer::PushConstants(
		uint32_t offset,
		uint32_t size,
		const void* data,
		PipelineStage stageMask
	)
	{
		assert(currentPipelineLayout_ != VK_NULL_HANDLE);

		vkCmdPushConstants(
			cmd_,
			currentPipelineLayout_,
			VK_SHADER_STAGE_ALL, //may be really bad
			offset,
			size,
			data);
	}

	inline void CommandBuffer::SetViewport(const VkViewport& viewport)
	{
		vkCmdSetViewport(cmd_, 0, 1, &viewport);
	}

	inline void CommandBuffer::SetScissor(const VkRect2D& scissor)
	{
		vkCmdSetScissor(cmd_, 0, 1, &scissor);
	}

	inline void CommandBuffer::Draw(
		uint32_t vertexCount,
		uint32_t instanceCount,
		uint32_t firstVertex,
		uint32_t firstInstance)
	{
		vkCmdDraw(
			cmd_,
			vertexCount,
			instanceCount,
			firstVertex,
			firstInstance);
	}
	inline void CommandBuffer::DrawIndexed(
		uint32_t indexCount,
		uint32_t instanceCount,
		uint32_t firstIndex,
		uint32_t baseVertex,
		uint32_t firstInstance
	)
	{
		vkCmdDrawIndexed(
			cmd_,
			indexCount,
			instanceCount,
			firstIndex,
			baseVertex,
			firstInstance
		);
	}

	inline void RenderGraph::Execute(FrameBuffer& fb)
	{
		ResolveImages();
		ResolveImageViews();
		ResolveBuffers();

		for (auto& pass : m_passes) {
			// update bindings for storageImages and else associated to this pass

			std::vector<VkRenderingAttachmentInfo> colorInfos;
			colorInfos.reserve(pass.colorAttachments_.size());

			for (auto& att : pass.colorAttachments_) {
				auto view = imageViews_.get(att.view);

				VkRenderingAttachmentInfo info{};
				info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
				info.imageView = view->resolvedImageView;
				info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				info.loadOp = att.loadOp;
				info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				info.clearValue = att.clearValue;

				colorInfos.push_back(info);
			}

			VkRenderingAttachmentInfo depthInfo{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
			};

			const bool hasDepth = !pass.depthAttachment_.view.empty();
			auto depthView = hasDepth
				? imageViews_.get(pass.depthAttachment_.view)
				: nullptr;
			const bool hasStencil =
				hasDepth &&
				FormatHasStencil(depthView->resolvedFormat);

			if (hasDepth) {
				depthInfo.imageView = depthView->resolvedImageView;
				depthInfo.imageLayout =
					hasStencil
					? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
					: VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
				depthInfo.loadOp = pass.depthAttachment_.loadOp;
				depthInfo.storeOp = pass.depthAttachment_.storeOp;
				depthInfo.clearValue = pass.depthAttachment_.clearValue;
			}

			VkRenderingInfo renderInfo{
				.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
				.renderArea = {
					.offset = pass.offset_,
					.extent = pass.extent_,
				},
				.layerCount = 1,
				.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size()),
				.pColorAttachments = colorInfos.data(),
			};
			renderInfo.pDepthAttachment =
				hasDepth ? &depthInfo : nullptr;
			renderInfo.pStencilAttachment =
				hasStencil ? &depthInfo : nullptr;

			VkViewport vp{};
			vp.x = 0.0f;
			vp.y = 0.0f;
			vp.width = static_cast<float>(pass.extent_.width);
			vp.height = static_cast<float>(pass.extent_.height);
			vp.minDepth = 0.0f;
			vp.maxDepth = 1.0f;

			VkRect2D scissor{};
			scissor.offset = { 0, 0 };
			scissor.extent = pass.extent_;

			vkCmdSetViewport(fb.commandBuffer, 0, 1, &vp);
			vkCmdSetScissor(fb.commandBuffer, 0, 1, &scissor);

			vkCmdBeginRendering(fb.commandBuffer, &renderInfo);

			CommandBuffer cmd{
				fb.commandBuffer,
				*this,
				pass
			};

			if (pass.recordingFunction_) {
				pass.recordingFunction_(cmd);
			}

			vkCmdEndRendering(fb.commandBuffer);
		}
	};

};
