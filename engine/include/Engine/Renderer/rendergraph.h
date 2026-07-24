#pragma once

#include <vector>
#include <Engine/Vulkan/core.h>


namespace smvk {
	struct RenderGraph;
	struct RenderPass;
	using LogicalBufferHandle = Handle<struct LogicalBuffer>;
	using LogicalImageHandle = Handle<struct LogicalImage>;
	using LogicalImageViewHandle = Handle<struct LogicalImageView>;
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
		VkFormat resolvedFormat = VK_FORMAT_UNDEFINED; // we need to know format too.
		// no layout state tracking here. may be done later. need to study nicegraf.
	};

	struct LogicalImage {
		enum struct Type {
			Transient,
			External
		} type;
		ImageKey key{};
		ImageHandle resolvedImageHandle{};
	};

	struct RenderGraph;
	struct RenderPassContext {
		smvk::BufferHandle GetResolvedBuffer(LogicalBufferHandle handle) {
			return rendergraph.GetResolvedBufferHandle(handle);
		};
		smvk::ImageHandle GetResolvedImage(LogicalImageHandle handle) {
			return rendergraph.GetResolvedImageHandle(handle)
		};
		VkCommandBuffer cmd;
	private:
		RenderGraph& rendergraph;
		RenderPass& renderpass;
		friend RenderGraph;
	};

	using RecordingFunction = std::function<void(RenderPassContext)>;
	struct RenderPass final {

		explicit RenderPass(RenderGraph& rg)
			: rg_(rg) {}
		RenderGraph& rg_;

		struct Attachment {
			LogicalImageViewHandle view;
			VkAttachmentLoadOp loadOp;
			VkAttachmentStoreOp storeOp;
			VkClearValue clearValue;
		};

		std::vector<LogicalImageViewHandle> views;
		std::vector<LogicalImageHandle> inputImages_;
		std::vector<LogicalBufferHandle> storageBuffers_;

		RenderPass& setColor(
			std::vector<LogicalImageViewHandle>& views,
			VkAttachmentLoadOp loadOp,
			VkAttachmentStoreOp storeOp,
			VkClearValue clearValue
		) {
			this->colorAttachments_.resize(views.size());
			for (size_t index = 0; index < views.size(); index++)
			{
				this->colorAttachments_[index] = { views[index], loadOp, storeOp, clearValue };
			}
			return *this;
		}
		RenderPass& setDepth(
			LogicalImageViewHandle viewId,
			VkAttachmentLoadOp _loadOp,
			VkAttachmentStoreOp _storeOp,
			VkClearValue _clearValue
		) {
			this->depthAttachment_ = { viewId, _loadOp, _storeOp, _clearValue };
			return *this;
		};
		//ToyRenderPass& setStencil(LogicalImageView depthAttachent) /*{ this->depthAttachment_ = depthAttachent; return *this; }*/;

		struct VertexFormatDesc {
			VkPipelineVertexInputStateCreateInfo vertexDescription{};
		} vertexFormat_;

		//
		RenderPass& setVertexFormat(VertexFormatDesc vertexFormat) { this->vertexFormat_ = vertexFormat; return *this; }
		RenderPass& setInputImages(std::vector<LogicalImageHandle> inputImages) { this->inputImages_ = inputImages; };
		RenderPass& setStorageBuffers(std::vector<LogicalBufferHandle> storageBuffers) { this->storageBuffers_ = storageBuffers; };
		RenderPass& setRecordingFunc(RecordingFunction recordingFunction) { this->recordingFunction_ = recordingFunction; return *this; };
		RenderPass& setExtent(VkExtent2D extent) { this->extent_ = extent; return *this; };
		RenderPass& setOffset(VkOffset2D offset) { this->offset_ = offset; return *this; };

		RenderGraph& GetRenderGraph() { return this->rg_; };

	private:
		friend RenderGraph;
		VkExtent2D extent_ = { 0,0 };
		VkOffset2D offset_ = { 0,0 };
		std::vector<Attachment> colorAttachments_{};
		Attachment depthAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = { 1.0f } /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE */ };
		//PassType type = PassType::Rasterize;
		RecordingFunction recordingFunction_{};
	};


	struct RenderGraph final {
		explicit RenderGraph(Context& ctx) : ctx_(ctx) {};
		void Clear() { m_passes.clear(); }

		RenderPass& AddPass() {
			return m_passes.emplace_back(RenderPass(*this));
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
			return images_.create(std::move(proxyImage));
		};
		[[nodiscard]] LogicalImageViewHandle AddImageView(ImageViewCreateInfo description) { assert(false); };
		[[nodisacrd]] LogicalImageHandle AddImage(VkImage vk_image) { assert(false); };
		[[nodiscard]] LogicalImageViewHandle AddImageView(VkImageView view) {
			LogicalImageView proxyView{};
			proxyView.type = LogicalImageView::Type::External;
			proxyView.resolvedImageView = view;
			return imageViews_.create(std::move(proxyView));
		};

		template<typename DataType>
		[[nodicard]] LogicalBufferHandle AddRingBuffer(uint32_t count, const char* name = "") {
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

		[[nodiscard]] std::vector<LogicalBufferHandle> AddExternalBuffers(std::vector<BufferHandle> bufferHandles) {
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

		VkPipelineRenderingCreateInfo GetPipelineRenderingInfo(
			const RenderPass& pass
		) {
			std::vector<VkFormat> colorAttachmentFormats;
			for (auto& cattach : pass.colorAttachments_) {

				colorAttachmentFormats.push_back(imageViews_.get(cattach.view)->resolvedFormat);
			}
			auto depthFormat = imageViews_.get(pass.depthAttachment_.view)->resolvedFormat;
			return VkPipelineRenderingCreateInfo{
					.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
					.pNext = VK_NULL_HANDLE,
					.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentFormats.size()),
					.pColorAttachmentFormats = colorAttachmentFormats.data(),
					.depthAttachmentFormat = depthFormat,
					.stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
			};
		};

		// No aliasing ftm
		//void TopoSort() {}; 
		// so resolved is just creating the resources/getting them from cache.
		void ResolveImages() {
			images_.forEach(
				[this](LogicalImage* logicalImage) {
				if (logicalImage->resolvedImageHandle.empty()) {
					//auto logicalImage = images_.get(logicalView->key.image);
					//auto img = ctx_.images_.get(logicalImage->resolvedImageHandle);
					logicalImage->resolvedImageHandle = ctx_.CreateImage(logicalImage->key, nullptr);
					// TODO THIS IS A HOLDER. RAII WILL AUTODESTROY IT
				};
			}
			);
		};

		void ResolveImageViews() {
			// needs resolved images first
			imageViews_.forEach(
				[this](LogicalImageView* logicalView) {
				if (logicalView->resolvedImageView == VK_NULL_HANDLE) {
					auto logicalImage = images_.get(logicalView->key.image);
					auto img = ctx_.images_.get(logicalImage->resolvedImageHandle);
					logicalView->resolvedFormat = img->vk_format;
					// if outImage"" we need to know if we are renderting to, sampling from or both
					// didnt distinguish before!. ill just assume render.
					// also swaphcain image is abstracted from the rendergraph for the moment...
					// LegitEngine does the present ass a pass. maybe its a good idea.
					logicalView->resolvedImageView = img->renderView;

				};
			}
			);
		};

		void ResolveBuffers() {
			storageBuffers_.forEach(
				[this](LogicalBuffer& logicalBuffer) { // Pass by reference, not pointer
				if (logicalBuffer.resolvedBufferHandle.empty()) {
					const auto& key = logicalBuffer.key;
					int ringMultiplier = key.ring ? 4 : 1;
					logicalBuffer.resolvedBufferHandle = ctx_.CreateBufferNoHolder(
						BufferCreateInfo{
							.size = key.elementCount * key.elementSize * ringMultiplier,
							.vma_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
							.usage_flags =
							  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
							| VK_BUFFER_USAGE_TRANSFER_DST_BIT
						}
					);
				}
			}
			);
		}


		smvk::BufferHandle GetResolvedBufferHandle(LogicalBufferHandle handle) const {
			return storageBuffers_.get(handle)->resolvedBufferHandle;
		};
		smvk::ImageHandle GetResolvedImageHandle(LogicalImageHandle handle) const {
			return images_.get(handle)->resolvedImageHandle;
		};

		void Execute(FrameBuffer& fb) {

			ResolveImages();
			ResolveImageViews();
			ResolveBuffers();

			for (auto& pass : m_passes) {

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

				auto depthView = imageViews_.get(pass.depthAttachment_.view);
				VkRenderingAttachmentInfo depthInfo{};
				depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
				{
					depthInfo.imageView = depthView->resolvedImageView;
				}
				depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
				depthInfo.loadOp = pass.depthAttachment_.loadOp;
				depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
				depthInfo.clearValue = pass.depthAttachment_.clearValue;

				VkRenderingInfo renderInfo{};
				renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
				renderInfo.renderArea.offset = pass.offset_;
				renderInfo.renderArea.extent = pass.extent_;
				renderInfo.layerCount = 1;
				renderInfo.colorAttachmentCount = (uint32_t)colorInfos.size();
				renderInfo.pColorAttachments = colorInfos.data();
				renderInfo.pDepthAttachment = &depthInfo;

				//// VIEWPORT
				VkViewport vp{};
				vp.x = 0.0f;
				vp.y = 0.0f;
				vp.width = (float)pass.extent_.width;
				vp.height = (float)pass.extent_.height;
				vp.minDepth = 0.0f;
				vp.maxDepth = 1.0f;
				vkCmdSetViewport(fb.commandBuffer, 0, 1, &vp);

				// SCISSOR.
				VkRect2D scissor{};
				scissor.offset = { 0, 0 };
				scissor.extent = pass.extent_;
				vkCmdSetScissor(fb.commandBuffer, 0, 1, &scissor);

				// this should adapt to pass vertex declaration.

				vkCmdBeginRendering(fb.commandBuffer, &renderInfo);

				//PushConstants(fb.commandBuffer);
				//PushDescriptors(fb.commandBuffer);

				RenderPassContext rpc{
					.rendergraph = *this,
					.renderpass = pass,
					.cmd = fb.commandBuffer
				};

				pass.recordingFunction_(
					rpc
				);

				vkCmdEndRendering(fb.commandBuffer);
			}
		}

		//private:
		smvk::Context& ctx_;
		smvk::Pool<LogicalImage, LogicalImage> images_;
		smvk::Pool<LogicalImageView, LogicalImageView> imageViews_;
		smvk::Pool<LogicalBuffer, LogicalBuffer> storageBuffers_;
		std::vector<RenderPass> m_passes;
	};

};