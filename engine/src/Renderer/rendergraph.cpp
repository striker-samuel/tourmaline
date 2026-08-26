#include <Engine/Renderer/rendergraph.h>

// this is just a stop gap, needing this is a design errors
/*static inline*/ smvk::ImageCreateInfo GetImageCreateInfoFromImage(smvk::Image* image)
{
	return {
		.vk_extent = image->vk_extent,
		.vk_format = image->vk_format,
		.vk_usage = image->vk_usage,
		.levels = image->levels,
		.arrayLayers = image->arrayLayers
	};
}


[[nodiscard]]
smvk::LogicalImageHandle smvk::RenderGraph::AddImage(smvk::ImageHandle imageHandle, const char* name
)
{
	assert(!imageHandle.empty());

	auto* image = ctx_.images_.get(imageHandle);
	assert(image);

	LogicalImage logical{
		.type = Type::External,
		.key = GetImageCreateInfoFromImage(image),
		.resolvedImageHandle = imageHandle,
		.name = name,
	};

	return images_.create(std::move(logical));
}


void smvk::CommandBuffer::BindRenderPipeline(
	ShaderProgram& shaderProgram,
	VertexFormat vertexFormat,
	const DepthSettings& depthSettings,
	const StencilSettings& stencilSettings,
	const BlendSettings& blendSettings,
	const MSAASettings& msaaSettings
)
{
	assert(graph_ != nullptr);
	assert(renderPass_ != nullptr);

	switch (vertexFormat)
	{
	case VertexFormat::StaticVertexFormat:
		smvk::BindStaticVertexFmtBuffers(this->graph_->ctx_, this->cmd_);
		break;
	case VertexFormat::Empty:
		break;
	default:
		break;
	}
	VertexFormatDescription formatDescription{};
	formatDescription = GetVertexFormatDescription(vertexFormat);

	std::vector<smvk::Format> colorAttachmentFormats;
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
		// I dont like this.
		if (FormatHasDepth(toVkFormat(format)))
			depthFormat = format;
		if (FormatHasStencil(toVkFormat(format)))
			stencilFormat = format;
	}

	auto bundle = graph_->pipelineCachePimpl->GetRenderPipeline(
		shaderProgram,
		formatDescription,
		colorAttachmentFormats.data(),
		static_cast<uint32_t>(colorAttachmentFormats.size()),
		depthFormat,
		stencilFormat,
		PrimitiveTopology::TriangleList,
		smvk::RasterSettings{},
		depthSettings,
		stencilSettings,
		blendSettings,
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

void smvk::RenderGraph::Execute(smvk::FrameBuffer& fb)
{
	ResolveImages();
	ResolveImageViews();
	ResolveBuffers();

	for (auto& pass : passes_) {
		// update bindings for storageImages and else associated to this pass

		std::vector<VkRenderingAttachmentInfo> colorInfos;
		colorInfos.reserve(pass.colorAttachments_.size());

		for (auto& att : pass.colorAttachments_) {
			auto view = imageViews_.get(att.view);

			VkRenderingAttachmentInfo info{};
			info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
			info.imageView = view->resolvedImageView;
			info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			info.loadOp = VkAttachmentLoadOp(att.loadOp);
			info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			info.clearValue = toVkClearValue(att.clearColor);

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
			FormatHasStencil(
				toVkFormat(
					depthView->resolvedFormat
				));

		if (hasDepth) {
			depthInfo.imageView = depthView->resolvedImageView;
			depthInfo.imageLayout =
				hasStencil
				? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
				: VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
			depthInfo.loadOp = toVkAttachmentLoadOp(pass.depthAttachment_.loadOp);
			depthInfo.storeOp = toVkAttachmentStoreOp(pass.depthAttachment_.storeOp);
			depthInfo.clearValue = toVkClearValue(pass.depthAttachment_.clear);

			VkRenderingInfo renderInfo{
				.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
				.renderArea = {
					.offset = {pass.offsetX_, pass.offsetY_},
					.extent = {pass.extent_.width, pass.extent_.height},
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
			scissor.extent = { pass.extent_.width, pass.extent_.height };

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
}

void smvk::RenderGraph::ResolveImages() {
	images_.forEach(
		[this](smvk::LogicalImage& logicalImage) {
		if (logicalImage.resolvedImageHandle.empty()) {
			if (logicalImage.type == smvk::LogicalImage::Type::External) { assert(false && "external image unresolved. shouldnt be possible"); };
			//CreateTransientImaeg(key, sync_req);
			logicalImage.resolvedImageHandle = ctx_.CreateImage(logicalImage.key, nullptr).release();
		};
	}
	);
};
void smvk::RenderGraph::ResolveImageViews() {
	// needs resolved images first
	imageViews_.forEach(
		[this](LogicalImageView& logicalView) {
		if (logicalView.resolvedImageView == VK_NULL_HANDLE) {
			auto logicalImage = images_.get(logicalView.key.image);
			auto img = ctx_.images_.get(logicalImage->resolvedImageHandle);
			logicalView.resolvedFormat = img->vk_format;
			logicalView.resolvedImageView = img->sampleView;

		};
	}
	);
};
void smvk::RenderGraph::ResolveBuffers()
{
	//	buffers.forEach(
	//		[this](LogicalBuffer& logicalBuffer) {
	//		if (logicalBuffer.resolvedBufferHandle.empty()) {
	//			const auto& key = logicalBuffer.key;
	//			int ringMultiplier = key.ring ? 4 : 1;
	//			logicalBuffer.resolvedBufferHandle = ctx_.CreateBuffer(
	//				key.elementCount * key.elementSize * ringMultiplier,
	//				smvk::BufferType_Storage,
	//				"resolved buffer"
	//			).release();
	//		}
	//	}
	//	);
}
