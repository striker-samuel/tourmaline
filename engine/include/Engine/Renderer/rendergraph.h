#pragma once

#include <vector>
//#include <Engine/Vulkan/core.h>
#include <Engine/Renderer/context.h>

// Lets just alias creation info to the key
using ImageKey = ImageCreateInfo;
using ImageKey = ImageCreateInfo;
using DeviceBufferKey = BufferCreateInfo;

struct RenderGraph;
struct ToyRenderPass;

struct ToyFrameBuffer {
	uint32_t frameIndex = 0; // Maybe end os uint32_ to catch for unitialized? this value can only be ip to SWAPCHAIN_IMAGE_COUNT
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VulkanSwapchain& swapchain;
};

//right now jsut mapping to single buffer resource, and not being more granular.
struct LogicalBuffer
{
	size_t id;
	DeviceBufferKey key;
	DeviceBufferHandle resolvedBufferHandle;
};


// hmmmm
struct LogicalImageView
{
	size_t id;
	ImageHandle externalImage{};
	VkImageView resolvedImageView = VK_NULL_HANDLE;
	VkFormat resolvedFormat = VK_FORMAT_UNDEFINED;
};

struct LogicalImage {
	enum struct ImageTypes {
		Transient, // Wont outlive the frame
		External // External like swapIamge for expample
	};

	size_t id;
	ImageTypes imageType; // transient/external
	ImageKey key; // May not be needed really
	// this may be collapsed into resolvedImage
	ImageHandle externalImage{};
	ImageHandle resolvedImage{};
};

struct ToyRenderPassContext {
	Context& ctx;
	ToyRenderPass& renderpass;
};
using RecordingFunction = std::function<void(ToyRenderPassContext)>;
struct ToyRenderPass final {
	RenderGraph& rg_;

	enum struct PassType { Rasterize, Compute };

	PassType type;
	VkExtent2D extent_;
	VkOffset2D offset_ = { 0,0 };
	RecordingFunction recordingFunction_;


	// Instrospect form shader or define here
	//std::vector<LogicalBinding> bindings_;
	//std::vector<LogicalBinding> bindings_;
	//std::vector<LogicalBinding> bindings_;

	struct Attachment {
		LogicalImageView view;
		VkAttachmentLoadOp loadOp;
		VkAttachmentStoreOp storeOp;
		VkClearValue clearValue;
	};

	std::vector<Attachment> colorAttachments_;
	Attachment depthAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = { 1.0f } /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE */ };
	Attachment stencilAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = {0.0f} /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE*/ };

	std::vector<LogicalBuffer> uniformBuffers_;
	std::vector<LogicalBuffer> storageBuffers_;
	std::vector<LogicalBuffer> vertexBuffers_;

	// Pipeline
	ShaderModuleHandle vert_;
	ShaderModuleHandle frag_;
	ToyRenderPass& setVertShader(ShaderModuleHandle vert) { vert_ = vert; };
	ToyRenderPass& setFragShader(ShaderModuleHandle frag) { frag_ = frag; };
	PipelineCreateInfo::RasterSettings rasterSettings_;
	ToyRenderPass& setRasterSettings(PipelineCreateInfo::RasterSettings rasterSettings) { rasterSettings_ = rasterSettings; };
	//
	//ToyRenderPass& setBinding(PipelineCreateInfo::BindingDescription desc) {};

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

	// stencil is baked into depth! this can cause problems here!
	// protect agains invariant or make SetStencil only enable
	ToyRenderPass& setDepth(LogicalImageView viewId,
		VkAttachmentLoadOp _loadOp,
		VkAttachmentStoreOp _storeOp,
		VkClearValue _clearValue
	) {
		this->depthAttachment_ = { viewId, _loadOp, _storeOp, _clearValue };
		return *this;
	};
	ToyRenderPass& setStencil(LogicalImageView depthAttachent) /*{ this->depthAttachment_ = depthAttachent; return *this; }*/;

	// this needs rework
	//struct VertexFormatDesc {
	//	//binding desc!
	//	} vertexFormat_;
	//ToyRenderPass& setVertexFormat(VertexFormatDesc vertexFormat) { this->vertexFormat_ = vertexFormat; return *this; }
	ToyRenderPass& setVertexBuffers(std::vector<LogicalBuffer> vertexBuffers) { this->vertexBuffers_ = vertexBuffers; return *this; }

	//
	//ToyRenderPass& setStorageBuffers(std::vector<LogicalBuffer> storageBuffers) { this->storageBuffers_ = storageBuffers; return *this; }
	//ToyRenderPass& setUniformBuffers(std::vector<LogicalBuffer> uniformBuffers) { this->uniformBuffers_ = uniformBuffers; return *this; }
	//

	ToyRenderPass& setRecordingFunc(RecordingFunction recordingFunction) { this->recordingFunction_ = recordingFunction; return *this; };
	ToyRenderPass& setExtent(VkExtent2D extent) { this->extent_ = extent; return *this; };
	ToyRenderPass& setOffset(VkOffset2D offset) { this->offset_ = offset; return *this; };

	RenderGraph& GetRenderGraph() { return this->rg_; };
};

struct RenderGraph {

	RenderGraph(Context& ctx) : ctx_(ctx) { pipelineBuilder_ = std::make_unique<PipelineBuilder>(ctx_); };
	Context& ctx_;
	std::unique_ptr<PipelineBuilder> pipelineBuilder_;

	std::vector<ToyRenderPass> passes;
	void Clear() { passes.clear(); }

	std::vector<LogicalImage> images;
	std::vector<LogicalImageView> views;
	std::vector<LogicalBuffer> buffers;

	ToyRenderPass& AddPass(ToyRenderPass _renderPass) {
		return passes.emplace_back(_renderPass);
	}

	//LogicalImage AddImage(VkImageCreateInfo imageDescription) {};
	LogicalImage AddImage(ImageHandle imageHandle) {
		auto image = ctx_.images_.get(imageHandle);
		LogicalImage proxy{
			.id = images.size(),
			.resolvedImage = imageHandle
		};
		images.push_back(proxy);
		return proxy;
	}
	LogicalImageView AddImageView(VkImageView view, VkFormat format) {
		LogicalImageView proxy{
			.id = views.size(),
			.externalImage = {},
			.resolvedImageView = view,
			.resolvedFormat = format
		};
		views.push_back(proxy);
		return proxy;
	}
	std::vector<LogicalBuffer> AddBuffers(std::vector<DeviceBufferHandle> bufferHandles) {
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
			colorAttachmentFormats.push_back(cattach.view.resolvedFormat);
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


	PipelineBuilder& GetPipelineBuilder() {
		return	*pipelineBuilder_;
	};
	void BindPipeline(const ToyRenderPassContext& rpc) {

		GetPipelineBuilder()
			.setRasterSettings(rpc.renderpass.rasterSettings_)
			.setDynamicRenderingInfo(GetPipelineRenderingInfo(rpc.renderpass))
			.buildPipeline();

	};

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

			// Vertex Bind
			// this should check for pipeline compat and coherence with having no vertex buffers
			//if (builder.GetPipepline(pass.pipeline_).vertexFormat) {}
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
				.ctx = ctx_,
				.renderpass = pass
			};
			BindPipeline(rpc);

			pass.recordingFunction_(
				rpc
			);

			vkCmdEndRendering(fb.commandBuffer);
		}
	}
};