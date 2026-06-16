#pragma once

#include <vector>
//#include <Engine/Vulkan/core.h>
#include <Engine/Renderer/context.h>

// Lets just alias creation info to the key
using ImageKey = ImageCreateInfo;
using DeviceBufferKey = DeviceBufferCreateInfo;
//using PipelineKey = PipelineCreateInfo;

// forward declarations
struct RenderGraph;
struct ToyRenderPass;

struct ToyRenderPassContext {
	Context& ctx;
	ToyRenderPass& renderpass;
};

struct ToyFrameBuffer{
	uint32_t frameIndex = 0; // Maybe end os uint32_ to catch for unitialized? this value can only be ip to SWAPCHAIN_IMAGE_COUNT
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VulkanSwapchain& swapchain;
};

struct VirtualBufferId
{
	size_t id;
	DeviceBufferKey key;
	DeviceBufferHandle buffer;
};

struct VirtualImageViewId
{
	size_t id;
	VkImageView resolvedImageView = VK_NULL_HANDLE;
};

struct VirtualImageId {
	enum struct ImageTypes {
		Transient, // Wont outlive the frame
		External // External like swapIamge for expample
	};

	size_t id;
	ImageTypes imageType; // transient/external
	ImageKey key;
	// this may be collapsed into resolvedImage
	ImageHandle externalImage{};
	ImageHandle resolvedImage{};
};

/// <summary>
/// 
/// 
/// </summary>
using RecordingFunction = std::function<void(ToyRenderPassContext)>;
struct ToyRenderPass final {
	//RenderGraph& rendergraph;

	enum struct PassType { Rasterize, Compute };
	PassType type;

	VkExtent2D extent_;
	VkOffset2D offset_ = { 0,0 };

	RecordingFunction recordingFunction_;

	struct Attachment {
		VirtualImageViewId view;
		VkAttachmentLoadOp loadOp;
		VkAttachmentStoreOp storeOp;
		VkClearValue clearValue;
	};

	std::vector<Attachment> colorAttachments_;
	Attachment depthAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = { 1.0f } /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE */ };
	Attachment stencilAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = {0.0f} /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE*/ };

	std::vector<VirtualBufferId> uniformBuffers_;
	std::vector<VirtualBufferId> storageBuffers_;
	std::vector<VirtualBufferId> vertexBuffers_;

	ToyRenderPass& setColor(
		std::vector<VirtualImageViewId>& views, // may be better to do views. 
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

	// stencil is baked into depth! this can cause problems here!
	// protect agains invariant or make SetStencil only enable
	ToyRenderPass& setDepth(VirtualImageViewId viewId,
		VkAttachmentLoadOp _loadOp,
		VkAttachmentStoreOp _storeOp,
		VkClearValue _clearValue
	) {
		this->depthAttachment_ = { viewId, _loadOp, _storeOp, _clearValue };
		return *this;
	};
	ToyRenderPass& setStencil(VirtualImageViewId depthAttachent) /*{ this->depthAttachment_ = depthAttachent; return *this; }*/;

	ToyRenderPass& setVertexBuffers(std::vector<VirtualBufferId> vertexBuffers) { this->vertexBuffers_ = vertexBuffers; return *this; }
	ToyRenderPass& setStorageBuffers(std::vector<VirtualBufferId> storageBuffers) { this->storageBuffers_ = storageBuffers; return *this; }
	ToyRenderPass& setUniformBuffers(std::vector<VirtualBufferId> uniformBuffers) { this->uniformBuffers_ = uniformBuffers; return *this; }

	ToyRenderPass& setRecordingFunc(RecordingFunction recordingFunction) { this->recordingFunction_ = recordingFunction; return *this; };

	ToyRenderPass& setExtent(VkExtent2D extent) { this->extent_ = extent; return *this; };
	ToyRenderPass& setOffset(VkOffset2D offset) { this->offset_ = offset; return *this; };

};

struct RenderGraph {
	Context* ctx;
	std::vector<ToyRenderPass> passes;
	void Clear() { passes.clear(); }

	std::vector<VirtualImageId> images;
	std::vector<VirtualImageViewId> views;
	//std::map<ImageKey, VirtualImageId> images;
	//std::map<BufferKey, VirtualImageId> buffers;

	ToyRenderPass& AddPass(ToyRenderPass _renderPass) {
		return passes.emplace_back(_renderPass);
	}


	//VirtualImageId AddImage(ImageHandle imageHandle) {
	//	auto image = ctx->images_.get(imageHandle);
	//	VirtualImageId proxy{
	//		.id = images.size(),
	//	};
	//	images.push_back(proxy);
	//	return proxy;
	//}
	VirtualImageViewId AddImageView(VkImageView view) {
		VirtualImageViewId proxy{
			.id = views.size(),
			.resolvedImageView = view
		};
		views.push_back(proxy);
		return proxy;
	}

	void ResolveImages() {};
	void ResolveImageViews() {};
	void ResolveBuffers() {};

	void Execute(ToyFrameBuffer& fb) {

	  ResolveImages();
	  ResolveImageViews();
	  ResolveBuffers();

		for (auto& pass : passes) {
			std::vector<VkRenderingAttachmentInfo> colorInfos;
			colorInfos.reserve(pass.colorAttachments_.size());

			for (auto& att : pass.colorAttachments_) {
				VkRenderingAttachmentInfo info{};
				info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
				//ImageHandle h = att.image.resolvedImage;
				//if (h.valid()) {
				//	info.imageView = ctx->images_.get(h)->renderView;
				//}
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
				//ImageHandle h = pass.depthAttachment_.image.resolvedImage;
				//if (h.valid()) {
				//	depthInfo.imageView = ctx->images_.get(h)->renderView;
				//}
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

			vkCmdBeginRendering(fb.commandBuffer, &renderInfo);

			ToyRenderPassContext rpc{
				.ctx = *ctx,
				.renderpass = pass
			};

			pass.recordingFunction_(
				rpc
			);

			vkCmdEndRendering(fb.commandBuffer);
		}
	}
};

class Cache {

	Context& ctx_;
	Cache(Context& ctx) : ctx_(ctx) {}
	~Cache() {};
	std::map<ImageKey, ImageHandle> images;
	//std::map<PipelineKey, PipelineHandle> pipelines;

	// This should be split into unofmr, storage and else
	std::map<DeviceBufferKey, DeviceBufferHandle> buffers;

	ImageHandle GetImage(ImageKey key) {
		// Obviusly not correct
		auto cacheHit = images[key];
		return cacheHit;
	};

};
