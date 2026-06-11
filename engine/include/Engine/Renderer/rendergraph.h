#pragma once

#include <vector>
//#include <Engine/Vulkan/core.h>
#include <Engine/Renderer/context.h>

struct RenderGraph{};

struct ImageKey {
	int texWidth;
	int texHeight;
	VkFormat format;
	VkImageUsageFlags usage;
	uint32_t levels;
	uint32_t arrayLayers;
};

struct BufferKey {
};

struct ToyFrameBuffer{
	//Swapchain& swapchain;
	// We are going with stupid simple only one swapchain
	uint32_t frameIndex = 0; // Maybe end os uint32_ to catch for unitialized? this value can only be ip to SWAPCHAIN_IMAGE_COUNT
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
};

struct ToyRenderPass {};
struct ToyRenderPassContext {
	Context& ctx;
	ToyRenderPass renderpass;
};
using RecordingFunction = std::function<void(ToyRenderPassContext)>;

enum PassType {
	Rasterize,
	Compute,
	//TransferPass,
	//ImagePresent,
	//FrameSyncBegin,
	//FrameSyncEnd
};

struct VirtualImageId {
	enum struct ImageTypes {
		Transient, // Wont outlive the frame
		External // External like swapIamge for expample
	};

	size_t id;
	ImageTypes imageType;
	// if external provide handle

	// Remember! Handles are per context
	ImageHandle externalImage{};
};

struct ToyRenderPass final {

	//RenderGraph& rendergraph;

	PassType type;
	VkExtent2D extent_;
	VkOffset2D offset_ = { 0,0 };

	RecordingFunction recordingFunction_;

	// Data
	//std::vector<BufferKey> uniformBuffers_;
	//std::vector<BufferKey> storageBuffers_;
	std::vector<BufferKey> vertexBuffers_;

	// Attachments
	struct Attachment {
		ImageKey image;
		VkAttachmentLoadOp loadOp;
		VkAttachmentStoreOp storeOp;
		VkClearValue clearValue;
	};

	std::vector<Attachment> colorAttachments_;
	Attachment depthAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = { 1.0f } /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE */ };
	Attachment stencilAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = {0.0f} /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE*/ };

	// Builder
	ToyRenderPass& setColor(
		std::vector<VirtualImageId> images,
		VkAttachmentLoadOp _loadOp,
		VkAttachmentStoreOp _storeOp,
		VkClearValue _clearValue
	) {
		this->colorAttachments_.resize(images.size());
		for (size_t index = 0; index < images.size(); index++)
		{
			this->colorAttachments_[index] = { images[index], _loadOp, _storeOp, _clearValue };
		}
		return *this;
	}

	// stencil is baked into depth! this can cause problems here!
	// protect agains invariant or make SetStencil only enable
	ToyRenderPass& setDepth(VirtualImageId _imageId) {};
	ToyRenderPass& setStencil(VirtualImageId depthAttachent) /*{ this->depthAttachment_ = depthAttachent; return *this; }*/;

	ToyRenderPass& serVertexBuffers(std::vector<BufferKey> vertexBuffers) { this->vertexBuffers_ = vertexBuffers; return *this; }
	ToyRenderPass& setRecordingFunc(RecordingFunction recordingFunction) { this->recordingFunction_ = recordingFunction; return *this; };
	ToyRenderPass& setExtent(VkExtent2D extent) { this->extent_ = extent; return *this; };
	ToyRenderPass& setOffset(VkOffset2D offset) { this->offset_ = offset; return *this; };

};

struct RenderGraph {
	Context* ctx;
	std::vector<ToyRenderPass> passes;
	void Clear() { passes.clear(); }

	std::vector<ImageKey> images;

	ToyRenderPass& AddPass(ToyRenderPass _renderPass) {
		return passes.emplace_back(_renderPass);
	}

	VirtualImageId GetImage(ImageKey _imageKey) {
		VirtualImageId proxy{
			.id = images.size()
		};
		images.push_back(_imageKey);

	}

	// TODO: Implement ImageKey → ImageHandle resolution
	ImageHandle ResolveImageKey(const ImageKey& key) {
		(void)key;
		return {};
	}

	// Resolves all ImageKeys, then begins/ends dynamic rendering for each pass
	void Execute(
	) {
		for (auto& pass : passes) {
			std::vector<VkRenderingAttachmentInfo> colorInfos;
			colorInfos.reserve(pass.colorAttachments_.size());


			for (auto& att : pass.colorAttachments_) {
				VkRenderingAttachmentInfo info{};
				info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
				ImageHandle h = ResolveImageKey(att.image);
				if (h.valid()) {
					info.imageView = ctx->images_.get(h)->renderView;
				}
				info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				info.loadOp = att.loadOp;
				info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				info.clearValue = att.clearValue;
				colorInfos.push_back(info);
			}

			VkRenderingAttachmentInfo depthInfo{};
			depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
			{
				ImageHandle h = ResolveImageKey(pass.depthAttachment_.image);
				if (h.valid()) {
					depthInfo.imageView = ctx->images_.get(h)->renderView;
				}
			}
			depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
			depthInfo.loadOp = pass.depthAttachment_.loadOp;
			depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthInfo.clearValue = pass.depthAttachment_.clearValue;

			VkRenderingInfo renderInfo{};
			renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
			//renderInfo.renderArea.offset = { 0, 0 };
			renderInfo.renderArea.offset = pass.offset_;
			renderInfo.renderArea.extent = pass.extent_;
			renderInfo.layerCount = 1;
			renderInfo.colorAttachmentCount = (uint32_t)colorInfos.size();
			renderInfo.pColorAttachments = colorInfos.data();
			renderInfo.pDepthAttachment = &depthInfo;

			vkCmdBeginRendering(cb.commandBuffer, &renderInfo);

			ToyRenderPassContext rpc{
				.ctx = *ctx,
				.renderpass = pass
			};

			pass.recordingFunction_(
				rpc
			);

			vkCmdEndRendering(cb.commandBuffer);
		}
	}
};

