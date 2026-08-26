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
		.type = LogicalResourceType::External,
		.key = GetImageCreateInfoFromImage(image),
		.resolvedImageHandle = imageHandle,
		.name = name,
	};

	return images_.create(std::move(logical));
}


void smvk::CommandBuffer::BindRenderPipeline(
	ShaderProgram& shaderProgram,
	VertexFormatDescription vertexFormatDescription,
	const DepthSettings& depthSettings ,
	const StencilSettings& stencilSettings,
	const BlendSettings& blendSettings,
	const MSAASettings& msaaSettings 
)
{
	assert(graph_ != nullptr);
	assert(renderPass_ != nullptr);

	// Fix this!
	//smvk::BindStaticVertexFmtBuffers(this->graph_->ctx_, this->cmd_);
	// 
	//VertexFormatDescription formatDescription{};
	//switch (vertexFormat)
	//{
	//case VertexFormat::StaticVertexFormat:
	//	smvk::BindStaticVertexFmtBuffers(this->graph_->ctx_, this->cmd_);
	//	formatDescription = StaticVertexFormat;
	//	break;
	//	//case VertexFormat::Empty:
	//	//	break;
	//default:
	//	break;
	//}

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
		vertexFormatDescription,
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
