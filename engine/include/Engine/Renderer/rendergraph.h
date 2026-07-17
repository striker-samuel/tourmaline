#pragma once

#include <vector>
#include <Engine/Vulkan/core.h>

namespace smvk {
	//
	struct RenderGraph;
	struct RenderPass;
	//

	struct BufferKey {
		size_t elementSize;
		size_t elementCount;
		bool ring; // each frame has its own size*count
	};
	using ImageKey = ImageCreateInfo;
	using ImageViewKey = ImageViewCreateInfo;

	struct LogicalBuffer
	{
		enum struct Type {
			External,
			Transient
		} type;
		size_t id = 0;
		BufferKey key{};
		BufferHandle resolvedBufferHandle; // Should be holder?
	};

	struct LogicalImageView
	{
		enum struct Type {
			External,
			Transient
		} type;
		size_t id = 0;
		ImageViewKey key{}; //VkFormat resolvedFormat = VK_FORMAT_UNDEFINED;
		ImageHandle resolvedImage{};
		VkImageView resolvedImageView = VK_NULL_HANDLE;
		// we are not managing subresource here... Should be done for proper aliasing later.
		//VkImageSubresourceRange subResource;
	};

	struct LogicalImage {
		enum struct ImageTypes {
			Transient,
			External
		} type;
		size_t id = 0;
		ImageKey key{};
		ImageHandle resolvedImage{};
	};

	struct RenderPassContext {
		//Context& ctx;// maybe nort needed
		RenderPass& renderpass;
		VkCommandBuffer cmd;
	};

	using RecordingFunction = std::function<void(RenderPassContext)>;
	struct RenderPass final {

		explicit RenderPass(RenderGraph& rg)
			: rg_(rg) {}
		RenderGraph& rg_;
		//enum struct PassType { Rasterize, Compute } type_;

		struct Attachment {
			LogicalImageView view;
			VkAttachmentLoadOp loadOp;
			VkAttachmentStoreOp storeOp;
			VkClearValue clearValue;
		};

		std::vector<LogicalImage> inputImages_;
		//std::vector<LogicalBuffer> uniformBuffers_; // dont really care about supporten uniform path. just abuse storage.
		std::vector<LogicalBuffer> storageBuffers_;
		// 0 is index. 1...N are vertex // this is bad and shoul be reworked to a vertexdecl type or something
		std::vector<LogicalBuffer> vertexBuffers_;

		RenderPass& setColor(
			std::vector<LogicalImageView>& views,
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

		RenderPass& setDepth(LogicalImageView viewId,
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
		RenderPass& setVertexFormat(VertexFormatDesc vertexFormat) { this->vertexFormat_ = vertexFormat; return *this; }
		//RenderPass& setUniformBuffers(std::vector<LogicalBuffer> uniformBuffers) { this->uniformBuffers_ = uniformBuffers};
		RenderPass& setInputImages(std::vector<LogicalImage> inputImages) { this->inputImages_ = inputImages; };
		RenderPass& setStorageBuffers(std::vector<LogicalBuffer> storageBuffers) { this->storageBuffers_ = storageBuffers; };
		RenderPass& setVertexBuffers(std::vector<LogicalBuffer> vertexBuffers) { this->vertexBuffers_ = vertexBuffers; return *this; }
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
		smvk::Context& ctx_;
		explicit RenderGraph(Context& ctx) : ctx_(ctx) {};
		void Clear() { m_passes.clear(); }

		RenderPass& AddPass() {
			return m_passes.emplace_back(RenderPass(*this));
		}

		LogicalImage AddImage(ImageCreateInfo description) { assert(false); };
		LogicalImage AddImage(VkImage vk_image) { assert(false); };
		LogicalImageView AddImageView(ImageViewCreateInfo description);
		LogicalImageView AddImageView(VkImageView view) {
			LogicalImageView proxyView{};
			proxyView.type = LogicalImageView::Type::External;
			proxyView.id = m_views.size();
			proxyView.resolvedImageView = view;
			m_views.push_back(proxyView);
			return proxyView;

		};

		template<typename DataType>
		LogicalBuffer AddRingBuffer(uint32_t count) {
			BufferKey key = {
					.elementSize = sizeof(DataType),
					.elementCount = count,
					.ring = true
			};

			return LogicalBuffer{
				.type = LogicalBuffer::Type::Transient,
				.id = m_storageBuffers.size(),
				.key = key,
			};

		}

		template<typename DataType>
		LogicalBuffer AddBuffer(uint32_t count) {

			BufferKey key = {
					.elementSize = sizeof(DataType),
					.elementCount = count
			};

			return LogicalBuffer{
				.type = LogicalBuffer::Type::Transient,
				.id = m_storageBuffers.size(),
				.key = key,
			};
		};

		LogicalImage AddExternalImage(ImageHandle imageHandle) {
			auto vk_image = ctx_.images_.get(imageHandle);
			LogicalImage proxy{
				.id = m_images.size(),
				.resolvedImage = imageHandle
			};
			m_images.push_back(proxy);
			return proxy;
		};
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
		std::vector<LogicalBuffer> AddExternalBuffers(std::vector<BufferHandle> bufferHandles) {
			size_t start = m_storageBuffers.size();

			for (auto b : bufferHandles) {
				LogicalBuffer proxy{
					.id = m_storageBuffers.size(),
					.resolvedBufferHandle = b
				};
				m_storageBuffers.push_back(proxy);
			}

			return std::vector<LogicalBuffer>(
				m_storageBuffers.begin() + start,
				m_storageBuffers.end()
			);
		}

		VkPipelineRenderingCreateInfo GetPipelineRenderingInfo(
			const RenderPass& pass
		) {
			std::vector<VkFormat> colorAttachmentFormats;
			for (auto cattach : pass.colorAttachments_) {
				colorAttachmentFormats.push_back(cattach.view.key.vk_format);
			}

			return VkPipelineRenderingCreateInfo{
					.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
					.pNext = VK_NULL_HANDLE,
					.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentFormats.size()),
					.pColorAttachmentFormats = colorAttachmentFormats.data(),
					//.depthAttachmentFormat = pass.depthAttachment_.view.resolvedImageView ? pass.depthAttachment_.view.resolvedFormat : VK_FORMAT_UNDEFINED,
					.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT, // TODO:: Hardcoded for now
					.stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
			};
		};

		void ResolveImages() {};
		void ResolveImageViews() {};
		void ResolveBuffers() {};
		void TopoSort() {};
		void Resolve() {};
		VkPipelineLayout GetLayout() {
			return VkPipelineLayout{};
		};

		void PushConstants(
			VkCommandBuffer cmd
		) {
			vkCmdPushConstants(
				cmd,
				GetLayout(),
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0,
				0,/* 	sizeof(pushBlock),*/ // TODO
				//&pushBlock
				nullptr
			);
		}

		void PushDescriptors(
			VkCommandBuffer cmd
		) {



		}

		void Execute(FrameBuffer& fb) {

			Resolve();

			for (auto& pass : m_passes) {

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
					vkCmdBindIndexBuffer(fb.commandBuffer, indexBuffer->vk_buffer, 0, VK_INDEX_TYPE_UINT32);

					size_t v_bufs = pass.vertexBuffers_.size() - 1;
					std::vector<VkBuffer> bufs;
					bufs.reserve(2);
					for (size_t i = 1; i < pass.vertexBuffers_.size(); i++) {
						bufs.push_back(ctx_.buffers_.get(pass.vertexBuffers_[i].resolvedBufferHandle)->vk_buffer);
					}
					VkDeviceSize offs[] = { 0, 0 };
					vkCmdBindVertexBuffers(fb.commandBuffer, 0, v_bufs, bufs.data(), offs);
				}

				vkCmdBeginRendering(fb.commandBuffer, &renderInfo);

				PushConstants(fb.commandBuffer);
				PushDescriptors(fb.commandBuffer);

				RenderPassContext rpc{
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

	private:
		std::vector<RenderPass> m_passes;


		// reosurces
		std::vector<LogicalImage> m_images;
		std::vector<LogicalImageView> m_views;
		std::vector<LogicalBuffer> m_storageBuffers;
		//std::vector<LogicalBuffer> m_uniformBuffers; // we dont care for uniform buffers yet. lets bruteforce with storage
	};

};