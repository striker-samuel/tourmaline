#pragma once

#include <vector>
#include <Engine/Vulkan/core.h>



#include <Engine/Renderer/vertex.h> // TODO:: Refactor this out!


namespace smvk {
	struct ShaderProgram {
		Holder<ShaderModuleHandle> vert{};
		Holder<ShaderModuleHandle> frag{};
	};
	struct RenderGraph;

	using LogicalBufferHandle = Handle<struct LogicalBuffer>;
	using LogicalImageHandle = Handle<struct LogicalImage>;
	using LogicalImageViewHandle = Handle<struct LogicalImageView>;
	using LogicalStorageImageHandle = Handle<struct LogicalStorageImage>;
};

namespace smvk {
	// This is not needed. We are handling a global default
	struct PipelineBundle {
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
	};

	struct PipelineCache {
		PipelineCache(Context* ctx) : ctx_(ctx) {};
		PipelineBundle GetRenderPipeline(
			ShaderProgram& program,
			// pass
			const VkFormat* colorFormats,
			uint32_t nColorAttachments,
			VkFormat depthFormat,
			VkFormat stencilFormat, //this right now shoyld match depthFormat since is same image.
			//rasterizerState
			VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
			VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT,
			VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE
		) {

			// TODO:: check for matching pipeline before building new one

			auto newPipe = BuildGraphicsPipeline(
				ctx_,
				program,
				colorFormats,
				nColorAttachments,
				depthFormat,
				stencilFormat,
				polygonMode,
				cullMode,
				frontFace
			);
			return newPipe;
		};
		PipelineBundle BuildGraphicsPipeline(
			Context* ctx,
			// shaders
			ShaderProgram& program,
			// pass
			const VkFormat* colorFormats,
			uint32_t nColorAttachments,
			VkFormat depthFormat,
			VkFormat stencilFormat, //this right now shoyld match depthFormat since is same image.
			//rasterizerState
			VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
			VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT,
			VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE
		)
		{
			auto v = ctx->shaderModules_.get(program.vert);
			auto f = ctx->shaderModules_.get(program.frag);

			// SHADER
			VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
			vertShaderStageInfo.sType =
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
			vertShaderStageInfo.module = *v;
			vertShaderStageInfo.pName = "mainVS";
			//vertShaderStageInfo.pSpecializationInfo = getSpecInfo()


			VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
			fragShaderStageInfo.sType =
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			fragShaderStageInfo.module = *f;
			fragShaderStageInfo.pName = "mainPS";

			VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo,fragShaderStageInfo };

			// VERTEX FORMAT. This is fixed and set beforehand
			// this should be obatined from description
			auto bindingDescription = Vertex::getBindingDescription();
			auto attributeDescriptions = Vertex::getAttributeDescriptions();

			VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
			vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescription.size());
			vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
			vertexInputInfo.pVertexBindingDescriptions = bindingDescription.data();
			vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

			VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
			inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
			inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			inputAssembly.primitiveRestartEnable = VK_FALSE;

			// VIEWPORT

			VkPipelineViewportStateCreateInfo viewportState{};
			viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			viewportState.viewportCount = 1;
			viewportState.scissorCount = 1;

			// RASTERIZER
			// Hardcoded fornow
			VkPipelineRasterizationStateCreateInfo rasterizationState_{
				  .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
				  .flags = 0,
				  .depthClampEnable = VK_FALSE, //
				  .rasterizerDiscardEnable = VK_FALSE,
				  .polygonMode = polygonMode,
				  .cullMode = cullMode,
				  .frontFace = frontFace,

				  .depthBiasEnable = VK_FALSE,
				  .depthBiasConstantFactor = 0.0f,
				  .depthBiasClamp = 0.0f,
				  .depthBiasSlopeFactor = 0.0f,

				  .lineWidth = 1.0f,
			};

			// MULTISAMPLING
			VkPipelineMultisampleStateCreateInfo multisampling{};
			multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
			multisampling.sampleShadingEnable = VK_FALSE;
			multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			// STENCIL
			VkPipelineDepthStencilStateCreateInfo depthStencil{};
			depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			depthStencil.depthTestEnable = VK_TRUE;
			depthStencil.depthWriteEnable = VK_TRUE;
			depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER;
			depthStencil.depthBoundsTestEnable = VK_FALSE;
			depthStencil.stencilTestEnable = VK_FALSE;

			// Blending should be configurable too!
			// COLORBLEND
			VkPipelineColorBlendAttachmentState colorBlendAttachment{};
			colorBlendAttachment.colorWriteMask =
				VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
				VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			colorBlendAttachment.blendEnable = VK_FALSE;

			VkPipelineColorBlendStateCreateInfo colorBlending{};
			colorBlending.sType =
				VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			colorBlending.logicOpEnable = VK_FALSE;
			colorBlending.logicOp = VK_LOGIC_OP_COPY;
			colorBlending.attachmentCount = 1;
			colorBlending.pAttachments = &colorBlendAttachment;
			colorBlending.blendConstants[0] = 0.0f;
			colorBlending.blendConstants[1] = 0.0f;
			colorBlending.blendConstants[2] = 0.0f;
			colorBlending.blendConstants[3] = 0.0f;

			// DYNAMIC STATES
			std::vector<VkDynamicState> dynamicStates = {
				VK_DYNAMIC_STATE_VIEWPORT, // critical
				VK_DYNAMIC_STATE_SCISSOR, // critical
				//VK_DYNAMIC_STATE_BLEND_CONSTANTS,
				//VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
				//VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
				//VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
				//VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE
			};

			VkPipelineDynamicStateCreateInfo dynamicState{};
			dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
			dynamicState.dynamicStateCount =
				static_cast<uint32_t>(dynamicStates.size());
			dynamicState.pDynamicStates = dynamicStates.data();

			VkPipelineLayout lyt = ctx_->defaultPipeLayout;
			VkPipelineRenderingCreateInfo dynamic_rendering_pipeline_info{
				VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR
			};
			dynamic_rendering_pipeline_info.pNext = VK_NULL_HANDLE;
			dynamic_rendering_pipeline_info.colorAttachmentCount = nColorAttachments;
			dynamic_rendering_pipeline_info.pColorAttachmentFormats = colorFormats;
			dynamic_rendering_pipeline_info.depthAttachmentFormat = depthFormat;
			dynamic_rendering_pipeline_info.stencilAttachmentFormat = stencilFormat;

			VkGraphicsPipelineCreateInfo pipelineInfo{};
			pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			pipelineInfo.stageCount = 2;
			pipelineInfo.pStages = shaderStages;
			pipelineInfo.pVertexInputState = &vertexInputInfo;
			pipelineInfo.pInputAssemblyState = &inputAssembly;
			pipelineInfo.pViewportState = &viewportState;
			pipelineInfo.pRasterizationState = &rasterizationState_;
			pipelineInfo.pMultisampleState = &multisampling;
			pipelineInfo.pColorBlendState = &colorBlending;
			pipelineInfo.pDynamicState = &dynamicState;
			pipelineInfo.pDepthStencilState = &depthStencil;
			pipelineInfo.layout = lyt;
			pipelineInfo.pNext = &dynamic_rendering_pipeline_info;

			PipelineBundle bundle{};
			ctx->core_.CreateGraphicsPipeline(pipelineInfo, bundle.pipeline);
			bundle.layout = lyt;
			return bundle;
		}
		//

	private:
		smvk::Context* ctx_;
	};


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
	};

	struct LogicalImage {
		enum struct Type {
			Transient,
			External
		} type;
		ImageKey key{};
		ImageHandle resolvedImageHandle{};
	};

	struct RenderPassContext; // Refactor this struct to oblvion

	struct RenderPassDescription {
		struct ColorAttachment {
			VkFormat vkColorFormat = VK_FORMAT_UNDEFINED;
			bool blend = false;
			//BlendState blendState;
		} color[6]{};
		VkFormat vkDepthFormat = VK_FORMAT_UNDEFINED;
		//Topology topology = Topology_Triangle;
		//lvk::VertexInput vertexInput;
		//StencilState backFaceStencil = {};
		//StencilState frontFaceStencil = {};
		//uint32_t patchControlPoints = 0;
		//uint32_t samplesCount = 1u;
		//float minSampleShading = 0.0f;
	};

	using RecordingFunction = std::function<void(RenderPassContext)>;
	struct RenderPass final {
		struct Attachment {
			LogicalImageViewHandle view;
			VkAttachmentLoadOp loadOp;
			VkAttachmentStoreOp storeOp;
			VkClearValue clearValue;
		};
		struct AttachmentBlendSetup {
			VkPipelineColorBlendAttachmentState state;
		};

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
			LogicalImageViewHandle logicalImageViewHandle,
			VkAttachmentLoadOp loadOp,
			VkAttachmentStoreOp storeOp,
			VkClearValue clearValue
		) {
			this->depthAttachment_ = { logicalImageViewHandle, loadOp, storeOp, clearValue };
			return *this;
		};

		//struct VertexFormatDesc {
		//	VkPipelineVertexInputStateCreateInfo vertexDescription{};
		//} vertexFormat_;

		// Resources
		RenderPass& setInputImages(std::vector<LogicalImageHandle> inputImages) { this->inputImages_ = inputImages; };
		RenderPass& setStorageBuffers(std::vector<LogicalBufferHandle> storageBuffers) { this->storageBuffers_ = storageBuffers; };
		RenderPass& setStorageImages(std::vector<LogicalStorageImageHandle> storageImages) { this->storageImages_ = storageImages; };

		// Settings
		RenderPass& setExtent(VkExtent2D extent) { this->extent_ = extent; return *this; };
		RenderPass& setOffset(VkOffset2D offset) { this->offset_ = offset; return *this; };
		//RenderPass& setVertexFormat(VertexFormatDesc vertexFormat) { this->vertexFormat_ = vertexFormat; return *this; }
		// 
		RenderPass& setRecordingFunc(RecordingFunction recordingFunction) { this->recordingFunction_ = recordingFunction; return *this; };


	private:
		friend RenderGraph;

		std::vector<LogicalImageHandle> inputImages_;
		std::vector<LogicalBufferHandle> storageBuffers_;
		std::vector<LogicalStorageImageHandle> storageImages_;


		VkExtent2D extent_ = { 0,0 };
		VkOffset2D offset_ = { 0,0 };
		std::vector<Attachment> colorAttachments_{};
		std::vector<AttachmentBlendSetup> colorAttachmentsBlendSetups;  // should either be {} or match colors size.
		Attachment depthAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = { 1.0f } /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE */ };
		RecordingFunction recordingFunction_{};
	};


	struct RenderPassContext {
		VkCommandBuffer cmd;
		RenderPass& renderpass;
		//RenderGraph* rendergraph = nullptr;
		//smvk::BufferHandle GetResolvedBuffer(LogicalBufferHandle handle) {
		//	return rendergraph->GetResolvedBufferHandle(handle);
		//}
		//smvk::ImageHandle GetResolvedImage(LogicalImageHandle handle) {
		//	return rendergraph->GetResolvedImageHandle(handle);
		//}
	};

	struct RenderGraph final {
		explicit RenderGraph(Context& ctx) : ctx_(ctx) {
			pipelineCachePimpl = std::make_unique<PipelineCache>(&ctx_);
		};
		void Clear() { m_passes.clear(); }



		// Rendergraph must resolve resouerces before this gettin called
		RenderPassDescription GetPassDescription(RenderPass& pass) {
			RenderPassDescription desc{};
			int i = 0;
			for (auto c : pass.colorAttachments_) {
				auto view = imageViews_.get(c.view);
				assert(view->resolvedFormat != VK_FORMAT_UNDEFINED);
				desc.color[i].vkColorFormat = view->resolvedFormat;
				desc.color[i].blend = false; // Pending blendsate implementation
			}
			auto depth = imageViews_.get(pass.depthAttachment_.view);
			assert(depth->resolvedFormat != VK_FORMAT_UNDEFINED);
			desc.vkDepthFormat = depth->resolvedFormat;
			//pending vertex assembly
			//pendibg blend
			//pending stencil
			return desc;
		};
		RenderPass& AddPass() {
			return m_passes.emplace_back(RenderPass());
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
		[[nodiscard]] LogicalImageViewHandle AddImageView(VkImageView view, VkFormat format) {
			LogicalImageView proxyView{};
			proxyView.type = LogicalImageView::Type::External;
			proxyView.resolvedImageView = view;
			proxyView.resolvedFormat = format;
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

		void ResolveImages() {
			inputImages_.forEach(
				[this](LogicalImage& logicalImage) {
				if (logicalImage.resolvedImageHandle.empty()) {
					if (logicalImage.type == LogicalImage::Type::External) { assert(false && "external image unresolved. shouldnt be possible"); };
					logicalImage.resolvedImageHandle = ctx_.CreateImage(logicalImage.key, nullptr).release();
				};
			}
			);

			inputImages_.forEach(
				[this](LogicalImage& logicalImage) {
				if (logicalImage.resolvedImageHandle.empty()) {
					if (logicalImage.type == LogicalImage::Type::External) { assert(false && "external image unresolved. shouldnt be possible"); };
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
					// if outImage"" we need to know if we are renderting to, sampling from or both
					// didnt distinguish before!. ill just assume render.
					// also swaphcain image is abstracted from the rendergraph for the moment...
					// LegitEngine does the present ass a pass. maybe its a good idea.
					logicalView.resolvedImageView = img->sampleView;

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
					logicalBuffer.resolvedBufferHandle = ctx_.CreateBuffer(
						BufferCreateInfo{
							.size = key.elementCount * key.elementSize * ringMultiplier,
							.vma_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
							.usage_flags =
							  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
							| VK_BUFFER_USAGE_TRANSFER_DST_BIT
						}
					).release();
				}
			}
			);
		}

		smvk::BufferHandle GetResolvedBufferHandle(LogicalBufferHandle handle) const {
			return storageBuffers_.get(handle)->resolvedBufferHandle;
		};
		smvk::ImageHandle GetResolvedImageHandle(LogicalImageHandle handle) const {
			return inputImages_.get(handle)->resolvedImageHandle;
		};

		void PushConstants(VkCommandBuffer cmd, uint32_t offset, uint32_t size, void* data) {
			assert(vkCurrentPipelineLayout_ != VK_NULL_HANDLE);
			vkCmdPushConstants(
				cmd,
				vkCurrentPipelineLayout_,
				VK_SHADER_STAGE_ALL, // crude
				//VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, /// This should depend on type of pipeline bound!
				offset,
				size,
				data
			);

		}

		void BindRenderPipeline(VkCommandBuffer cmd, ShaderProgram& shaderProgram) {
			std::vector<VkFormat> colorAttachmentFormats;
			for (auto& cattach : currentPass_->colorAttachments_) {
				colorAttachmentFormats.push_back(imageViews_.get(cattach.view)->resolvedFormat);
			}
			auto depthFormat = imageViews_.get(currentPass_->depthAttachment_.view)->resolvedFormat;
			VkPipelineRenderingCreateInfo info{
					.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
					.pNext = VK_NULL_HANDLE,
					.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentFormats.size()),
					.pColorAttachmentFormats = colorAttachmentFormats.data(),
					.depthAttachmentFormat = depthFormat,
					.stencilAttachmentFormat = depthFormat /*if depthFormat has stencil dadda*/,
			};

			auto bundle = pipelineCachePimpl->GetRenderPipeline(shaderProgram,
				info.pColorAttachmentFormats,
				info.colorAttachmentCount,
				info.depthAttachmentFormat,
				info.stencilAttachmentFormat
				//polygonmode =pass.polygonMode.
				//blend = pass.blend
			);

			if (bundle.pipeline == VK_NULL_HANDLE) {
				assert(false && "Invalid pipeline handle");
			}
			if (bundle.pipeline != vkCurrentPipeline_) {
				vkCurrentPipeline_ = bundle.pipeline;
				vkCurrentPipelineLayout_ = bundle.layout;
			}
			else {
				return;
			}
			vkCmdBindPipeline(
				cmd,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				vkCurrentPipeline_
			);

			vkCmdBindDescriptorSets(
				cmd,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				//vkCurrentPipelineLayout_,
				ctx_.defaultPipeLayout,
				0,
				1,
				ctx_.defaultDescriptorSets_,
				0,
				0
			);
		}

		void Execute(FrameBuffer& fb) {

			ResolveImages();
			ResolveImageViews();
			ResolveBuffers();

			for (auto& pass : m_passes) {

				// Reset current pipeline bound
				vkCurrentPipelineLayout_ = VK_NULL_HANDLE;
				vkCurrentPipeline_ = VK_NULL_HANDLE;
				currentPass_ = &pass;

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

				vkCmdBeginRendering(fb.commandBuffer, &renderInfo);
				RenderPassContext rpc{
					.cmd = fb.commandBuffer,
					.renderpass = pass
				};
				pass.recordingFunction_(rpc);

				assert(vkCurrentPipeline_ != VK_NULL_HANDLE && "no current pipeline");

				vkCmdEndRendering(fb.commandBuffer);
			}
		}

		RenderPass* currentPass_;
		VkPipeline vkCurrentPipeline_;
		VkPipelineLayout vkCurrentPipelineLayout_;
		// 
		smvk::Pool<LogicalImage, LogicalImage> inputImages_;
		smvk::Pool<LogicalStorageImage, LogicalImage> storageImages_;
		smvk::Pool<LogicalImageView, LogicalImageView> imageViews_;
		smvk::Pool<LogicalBuffer, LogicalBuffer> storageBuffers_;
		//
		std::vector<RenderPass> m_passes;
		//
		std::unique_ptr<PipelineCache> pipelineCachePimpl;
		smvk::Context& ctx_;

	};


};