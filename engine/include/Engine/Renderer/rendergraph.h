#pragma once

#include <vector>
#include <Engine/Vulkan/core.h>

struct RenderGraph;
struct ToyRenderPass;

using ImageKey = ImageCreateInfo;
using ImageViewKey = ImageViewCreateInfo;
struct BufferKey {
	size_t elementSize;
	size_t elementCount;
};
//using DeviceBufferKey = BufferCreateInfo;

/// 
struct ToyFrameBuffer {
	uint32_t frameIndex = 0; // Maybe end os uint32_ to catch for unitialized? this value can only be ip to SWAPCHAIN_IMAGE_COUNT
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VulkanSwapchain& swapchain;
};

//right now jsut mapping to single buffer resource, and not being more granular.
struct LogicalBuffer
{
	enum struct Type {
		External,
		Transient
	} type;
	size_t id;
	BufferKey key;
	ToyBufferHandle resolvedBufferHandle; // Should be holder?
};

struct LogicalImageView
{
	enum struct Type {
		External,
		Transient
	} type;
	size_t id;
	ImageViewKey key; //VkFormat resolvedFormat = VK_FORMAT_UNDEFINED;
	ImageHandle resolvedImage{};
	VkImageView resolvedImageView = VK_NULL_HANDLE;
};

struct LogicalImage {
	enum struct ImageTypes {
		Transient,
		External
	} type;
	size_t id;
	ImageKey key;
	ImageHandle resolvedImage{};
};

struct ToyRenderPassContext {
	//Context& ctx;// maybe nort needed
	ToyRenderPass& renderpass;
	VkCommandBuffer cmd;
};

using RecordingFunction = std::function<void(ToyRenderPassContext)>;
struct ToyRenderPass final {
	RenderGraph& rg_;
	enum struct PassType { Rasterize, Compute } type_;

	struct Attachment {
		LogicalImageView view;
		VkAttachmentLoadOp loadOp;
		VkAttachmentStoreOp storeOp;
		VkClearValue clearValue;
	};

	//std::vector<LogicalBuffer> uniformBuffers_; // dont really care about supporten uniform path. just abuse storage.
	std::vector<LogicalBuffer> storageBuffers_;
	// 0 is index. 1...N are vertex
	std::vector<LogicalBuffer> vertexBuffers_;

	ToyRenderPass& setColor(
		std::vector<LogicalImageView>& views, // may be better to do views. 
		VkAttachmentLoadOp _loadOp,
		VkAttachmentStoreOp _storeOp,
		VkClearValue _clearValue
	) {
		this->colorAttachments_.resize(views.size());
		for (size_t index = 0; index < views.size(); index++)
		{
			this->colorAttachments_[index] = { views[index], _loadOp, _storeOp, _clearValue };
		}
		return *this;
	}

	ToyRenderPass& setDepth(LogicalImageView viewId,
		VkAttachmentLoadOp _loadOp,
		VkAttachmentStoreOp _storeOp,
		VkClearValue _clearValue
	) {
		this->depthAttachment_ = { viewId, _loadOp, _storeOp, _clearValue };
		return *this;
	};
	//ToyRenderPass& setStencil(LogicalImageView depthAttachent) /*{ this->depthAttachment_ = depthAttachent; return *this; }*/;

	// Not supported yet
	struct VertexFormatDesc {
		VkPipelineVertexInputStateCreateInfo vertexDescription{};
	} vertexFormat_;
	ToyRenderPass& setVertexFormat(VertexFormatDesc vertexFormat) { this->vertexFormat_ = vertexFormat; return *this; }
	ToyRenderPass& setVertexBuffers(std::vector<LogicalBuffer> vertexBuffers) { this->vertexBuffers_ = vertexBuffers; return *this; }

	//
	ToyRenderPass& setRecordingFunc(RecordingFunction recordingFunction) { this->recordingFunction_ = recordingFunction; return *this; };
	ToyRenderPass& setExtent(VkExtent2D extent) { this->extent_ = extent; return *this; };
	ToyRenderPass& setOffset(VkOffset2D offset) { this->offset_ = offset; return *this; };
	// 
	RenderGraph& GetRenderGraph() { return this->rg_; };


private:
	VkExtent2D extent_;
	VkOffset2D offset_ = { 0,0 };
	std::vector<Attachment> colorAttachments_;
	Attachment depthAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = { 1.0f } /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE */ };
	PassType type;
	RecordingFunction recordingFunction_;

	friend RenderGraph;

};
struct RenderGraph {
	Context& ctx_;

	std::vector<ToyRenderPass> passes;
	void Clear() { 
		passes.clear(); 
	}

	std::vector<LogicalImage> images;
	std::vector<LogicalImageView> views;
	std::vector<LogicalBuffer> buffers;

	// Renderpass boundary is now attachament flush
	ToyRenderPass& AddPass(ToyRenderPass _renderPass) {
		return passes.emplace_back(_renderPass);
	}

	LogicalImage AddImage() {};
	LogicalImageView AddImageView() {};

	template<typename DataType>
	LogicalBuffer AddBuffer(uint32_t count) {

		BufferKey key = {
				.elementSize = sizeof(DataType),
				.elementCount = count
		};

		return LogicalBuffer{
			.id = buffers.size(),
			.type = LogicalBuffer::Type::Transient,
			.key = key,
		};
	};

	LogicalImage AddExternalImage(ImageHandle imageHandle) {
		auto image = ctx_.images_.get(imageHandle);
		LogicalImage proxy{
			.id = images.size(),
			.resolvedImage = imageHandle
		};
		images.push_back(proxy);
		return proxy;
	}
	//LogicalImageView AddExternalImageView(ImageViewHandle handle) {
	//	LogicalImageView proxy{
	//		.id = views.size(),
	//		.externalImage = {},
	//		.resolvedImageView = view,
	//		.resolvedFormat = format
	//	};
	//	views.push_back(proxy);
	//	return proxy;
	//}
	std::vector<LogicalBuffer> AddExternalBuffers(std::vector<ToyBufferHandle> bufferHandles) {
		size_t start = buffers.size();

		for (auto b : bufferHandles) {
			LogicalBuffer proxy{
				.id = buffers.size(),
				.resolvedBufferHandle = b
			};
			buffers.push_back(proxy);
		}

		return std::vector<LogicalBuffer>(
			buffers.begin() + start,
			buffers.end()
		);
	}

	VkPipelineRenderingCreateInfo GetPipelineRenderingInfo(
		const ToyRenderPass& pass
	) {
		std::vector<VkFormat> colorAttachmentFormats;
		for (auto cattach : pass.colorAttachments_) {
			colorAttachmentFormats.push_back(cattach.view.key.format);
		}

		return VkPipelineRenderingCreateInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
				.pNext = VK_NULL_HANDLE,
				.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentFormats.size()),
				.pColorAttachmentFormats = colorAttachmentFormats.data(),
				.depthAttachmentFormat = pass.depthAttachment_.view.resolvedImageView ? pass.depthAttachment_.view.resolvedFormat : VK_FORMAT_UNDEFINED,
				.stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
		};
	};

	void ResolveImages() {};
	void ResolveImageViews() {};
	void ResolveBuffers() {};

	void TopoSort() {};
	void Resolve() {};

	void Execute(ToyFrameBuffer& fb) {

		Resolve();

		for (auto& pass : passes) {

			std::vector<VkRenderingAttachmentInfo> colorInfos;
			colorInfos.reserve(pass.colorAttachments_.size());
			for (auto& att : pass.colorAttachments_) {
				VkRenderingAttachmentInfo info{};
				info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
				info.imageView = att.view.resolvedImageView;
				info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				info.loadOp = att.loadOp;
				info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				info.clearValue = att.clearValue;
				colorInfos.push_back(info);
			}

			VkRenderingAttachmentInfo depthInfo{};
			depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
			{
				depthInfo.imageView = pass.depthAttachment_.view.resolvedImageView;
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
			if (pass.vertexBuffers_.size() > 0) {
				auto indexBuffer = ctx_.buffers_.get(pass.vertexBuffers_[0].resolvedBufferHandle);
				vkCmdBindIndexBuffer(fb.commandBuffer, indexBuffer->buffer, 0, VK_INDEX_TYPE_UINT32);

				size_t v_bufs = pass.vertexBuffers_.size() - 1;
				std::vector<VkBuffer> bufs;
				bufs.reserve(2);
				for (size_t i = 1; i < pass.vertexBuffers_.size(); i++) {
					bufs.push_back(ctx_.buffers_.get(pass.vertexBuffers_[i].resolvedBufferHandle)->buffer);
				}
				VkDeviceSize offs[] = { 0, 0 };
				vkCmdBindVertexBuffers(fb.commandBuffer, 0, v_bufs, bufs.data(), offs);
			}

			vkCmdBeginRendering(fb.commandBuffer, &renderInfo);

			ToyRenderPassContext rpc{
				//.ctx = ctx_,
				.renderpass = pass,
				.cmd = fb.commandBuffer
			};

			pass.recordingFunction_(
				rpc
			);

			vkCmdEndRendering(fb.commandBuffer);
		}
	}
};