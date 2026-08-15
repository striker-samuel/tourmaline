#pragma once

#include <vector>
#include <string_view> // Entry point 
#include <cstring>     // compare and hash

#include <Engine/Vulkan/core.h>

namespace smvk {
	struct RenderGraph;
	struct CommandBuffer;
	using LogicalBufferHandle = Handle<struct LogicalBuffer>;
	using LogicalImageHandle = Handle<struct LogicalImage>;
	using LogicalImageViewHandle = Handle<struct LogicalImageView>;
	using LogicalStorageImageHandle = Handle<struct LogicalStorageImage>;
};


namespace smvk {

	struct VertexFormat
	{
		std::vector<VkVertexInputBindingDescription> bindingDescriptions{};
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions{};
	};

	struct PipelineBundle {
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
	};

	struct PipelineKey {

		ShaderProgram& program;
		std::vector<VkVertexInputBindingDescription> bindingDescriptions{};
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions{};
		std::vector<VkFormat> colorFormats;
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
		VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
		VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
		VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		bool operator==(const PipelineKey& other) const noexcept
		{
			return program == other.program &&
				colorFormats == other.colorFormats &&
				depthFormat == other.depthFormat &&
				stencilFormat == other.stencilFormat &&
				polygonMode == other.polygonMode &&
				cullMode == other.cullMode &&
				frontFace == other.frontFace;
		}
	};

	struct PipelineKeyHash {
		static void HashCombine(std::size_t& seed, std::size_t value) noexcept
		{
			// Standard-ish hash combine.
			seed ^= value + 0x9e3779b97f4a7c15ull +
				(seed << 6) + (seed >> 2);
		}

		template<typename T>
		static void HashValue(std::size_t& seed, const T& value) noexcept
		{
			HashCombine(seed, std::hash<T>{}(value));
		}

		std::size_t operator()(const PipelineKey& key) const noexcept
		{
			std::size_t seed = 0;

			HashValue(seed, key.program.shaderCount);

			for (size_t i = 0; i < key.program.shaderCount; ++i)
			{
				const Shader& shader = key.program.shaders[i];

				HashValue(seed, shader.vkShaderModule.index());
				HashValue(seed, static_cast<uint32_t>(shader.stage));
				HashValue(seed, std::string_view(shader.entryPoint));
			}
			HashValue(seed, key.colorFormats.size());
			for (VkFormat format : key.colorFormats) {
				HashValue(seed, static_cast<uint32_t>(format));
			}

			HashValue(seed, static_cast<uint32_t>(key.depthFormat));
			HashValue(seed, static_cast<uint32_t>(key.stencilFormat));
			HashValue(seed, static_cast<uint32_t>(key.polygonMode));
			HashValue(seed, static_cast<uint32_t>(key.cullMode));
			HashValue(seed, static_cast<uint32_t>(key.frontFace));

			return seed;
		}
	};

	class PipelineCache {
	public:
		explicit PipelineCache(ResourceContext* ctx)
			: ctx_(ctx)
		{}

		~PipelineCache()
		{
			Clear();
		}

		PipelineBundle GetRenderPipeline(
			ShaderProgram& program,
			VertexFormat vertexFormat,
			const VkFormat* colorFormats,
			uint32_t nColorAttachments,
			VkFormat depthFormat,
			VkFormat stencilFormat,
			VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
			VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT,
			VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE)
		{
			PipelineKey key{
				.program = program,
				.bindingDescriptions = vertexFormat.bindingDescriptions,
				.attributeDescriptions = vertexFormat.attributeDescriptions,
				.colorFormats = std::vector<VkFormat>(
					colorFormats,
					colorFormats + nColorAttachments),
				.depthFormat = depthFormat,
				.stencilFormat = stencilFormat,
				.polygonMode = polygonMode,
				.cullMode = cullMode,
				.frontFace = frontFace,
			};

			if (auto it = pipelines_.find(key); it != pipelines_.end()) {
				return it->second;
			}

			PipelineBundle bundle = BuildGraphicsPipeline(key);

			auto [it, inserted] =
				pipelines_.emplace(std::move(key), bundle);

			return it->second;
		}

		void Clear()
		{
			vkDeviceWaitIdle(ctx_->core_.GetDevice());
			for (auto& [key, bundle] : pipelines_) {
				if (bundle.pipeline != VK_NULL_HANDLE) {
					ctx_->core_.DestroyPipeline(bundle.pipeline);
				}
			}
			pipelines_.clear();
		}

	private:
		PipelineBundle BuildGraphicsPipeline(const PipelineKey& key)
		{
			std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
			shaderStages.reserve(key.program.shaderCount);

			for (size_t i = 0; i < key.program.shaderCount; ++i)
			{
				const Shader& shader = key.program.shaders[i];

				auto shaderModule = ctx_->shaderModules_.get(shader.vkShaderModule);

				VkPipelineShaderStageCreateInfo stageInfo{};
				stageInfo.sType =
					VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;

				stageInfo.stage = shader.stage;
				stageInfo.module = *shaderModule;
				stageInfo.pName = shader.entryPoint;

				shaderStages.push_back(stageInfo);
			}

			// ---------------------------------------------------------
			// VERTEX INPUT
			// ---------------------------------------------------------

			auto bindingDescriptions = key.bindingDescriptions;
			auto attributeDescriptions = key.attributeDescriptions;
			VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
			vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescriptions.size());
			vertexInputInfo.pVertexBindingDescriptions = bindingDescriptions.data();
			vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
			vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

			// ---------------------------------------------------------
			// INPUT ASSEMBLY
			// ---------------------------------------------------------

			VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
			inputAssembly.sType =
				VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
			inputAssembly.topology =
				VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			inputAssembly.primitiveRestartEnable =
				VK_FALSE;

			// ---------------------------------------------------------
			// VIEWPORT
			// ---------------------------------------------------------

			VkPipelineViewportStateCreateInfo viewportState{};
			viewportState.sType =
				VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			viewportState.viewportCount = 1;
			viewportState.scissorCount = 1;

			// ---------------------------------------------------------
			// RASTERIZATION
			// ---------------------------------------------------------

			VkPipelineRasterizationStateCreateInfo rasterizationState{
				.sType =
					VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,

				.pNext = nullptr,
				.flags = 0,

				.depthClampEnable = VK_FALSE,
				.rasterizerDiscardEnable = VK_FALSE,

				.polygonMode = key.polygonMode,
				.cullMode = key.cullMode,
				.frontFace = key.frontFace,

				.depthBiasEnable = VK_FALSE,
				.depthBiasConstantFactor = 0.0f,
				.depthBiasClamp = 0.0f,
				.depthBiasSlopeFactor = 0.0f,

				.lineWidth = 1.0f,
			};

			// ---------------------------------------------------------
			// MULTISAMPLING
			// ---------------------------------------------------------

			VkPipelineMultisampleStateCreateInfo multisampling{};
			multisampling.sType =
				VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
			multisampling.sampleShadingEnable = VK_FALSE;
			multisampling.rasterizationSamples =
				VK_SAMPLE_COUNT_1_BIT;

			// ---------------------------------------------------------
			// DEPTH / STENCIL
			// ---------------------------------------------------------

			VkPipelineDepthStencilStateCreateInfo depthStencil{};
			depthStencil.sType =
				VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			depthStencil.depthTestEnable =
				key.depthFormat != VK_FORMAT_UNDEFINED;
			depthStencil.depthWriteEnable =
				key.depthFormat != VK_FORMAT_UNDEFINED;
			depthStencil.depthCompareOp =
				VK_COMPARE_OP_GREATER;
			depthStencil.depthBoundsTestEnable =
				VK_FALSE;
			depthStencil.stencilTestEnable =
				VK_FALSE;

			// ---------------------------------------------------------
			// COLOR BLENDING
			// ---------------------------------------------------------

			VkPipelineColorBlendAttachmentState colorBlendAttachment{};
			colorBlendAttachment.colorWriteMask =
				VK_COLOR_COMPONENT_R_BIT |
				VK_COLOR_COMPONENT_G_BIT |
				VK_COLOR_COMPONENT_B_BIT |
				VK_COLOR_COMPONENT_A_BIT;

			colorBlendAttachment.blendEnable =
				VK_FALSE;

			// One attachment state per color target.
			std::vector<VkPipelineColorBlendAttachmentState>
				colorBlendAttachments(
					key.colorFormats.size(),
					colorBlendAttachment);

			VkPipelineColorBlendStateCreateInfo colorBlending{};
			colorBlending.sType =
				VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

			colorBlending.logicOpEnable = VK_FALSE;
			colorBlending.logicOp = VK_LOGIC_OP_COPY;

			colorBlending.attachmentCount =
				static_cast<uint32_t>(colorBlendAttachments.size());

			colorBlending.pAttachments =
				colorBlendAttachments.data();

			// ---------------------------------------------------------
			// DYNAMIC STATE
			// ---------------------------------------------------------

			constexpr VkDynamicState dynamicStates[] = {
				VK_DYNAMIC_STATE_VIEWPORT,
				VK_DYNAMIC_STATE_SCISSOR,
				//VK_DYNAMIC_STATE_DEPTH_BOUNDS,
				//VK_DYNAMIC_STATE_DEPTH_BIAS
			};

			VkPipelineDynamicStateCreateInfo dynamicState{};
			dynamicState.sType =
				VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;

			dynamicState.dynamicStateCount =
				static_cast<uint32_t>(std::size(dynamicStates));

			dynamicState.pDynamicStates =
				dynamicStates;

			// ---------------------------------------------------------
			// PIPELINE LAYOUT
			// ---------------------------------------------------------

			VkPipelineLayout layout =
				ctx_->defaultPipeLayout;

			// ---------------------------------------------------------
			// DYNAMIC RENDERING
			// ---------------------------------------------------------

			VkPipelineRenderingCreateInfo renderingInfo{};
			renderingInfo.sType =
				VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;

			renderingInfo.colorAttachmentCount =
				static_cast<uint32_t>(key.colorFormats.size());

			renderingInfo.pColorAttachmentFormats =
				key.colorFormats.data();

			renderingInfo.depthAttachmentFormat =
				key.depthFormat;

			renderingInfo.stencilAttachmentFormat =
				key.stencilFormat;

			// ---------------------------------------------------------
			// GRAPHICS PIPELINE
			// ---------------------------------------------------------

			VkGraphicsPipelineCreateInfo pipelineInfo{};
			pipelineInfo.sType =
				VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;

			pipelineInfo.pNext =
				&renderingInfo;

			pipelineInfo.stageCount =
				static_cast<uint32_t>(std::size(shaderStages));

			pipelineInfo.pStages =
				shaderStages.data();

			pipelineInfo.pVertexInputState =
				&vertexInputInfo;

			pipelineInfo.pInputAssemblyState =
				&inputAssembly;

			pipelineInfo.pViewportState =
				&viewportState;

			pipelineInfo.pRasterizationState =
				&rasterizationState;

			pipelineInfo.pMultisampleState =
				&multisampling;

			pipelineInfo.pDepthStencilState =
				&depthStencil;

			pipelineInfo.pColorBlendState =
				&colorBlending;

			pipelineInfo.pDynamicState =
				&dynamicState;

			pipelineInfo.layout =
				layout;

			PipelineBundle bundle{};

			ctx_->core_.CreateGraphicsPipeline(
				pipelineInfo,
				bundle.pipeline);

			bundle.layout =
				layout;

			return bundle;
		}

	private:
		ResourceContext* ctx_ = nullptr;

		std::unordered_map<
			PipelineKey,
			PipelineBundle,
			PipelineKeyHash
		> pipelines_;
	};

} // namespace smvk


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
		//lvk::VertexInput vertexInput;
		//StencilState backFaceStencil = {};
		//StencilState frontFaceStencil = {};
		//uint32_t patchControlPoints = 0;
		//uint32_t samplesCount = 1u;
		//float minSampleShading = 0.0f;
	};

	using RecordingFunction = std::function<void(CommandBuffer&)>;
	struct RenderPass final {
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
		// Description
		// topology.
		// stencil
		// msaa
		VkExtent2D extent_ = { 0,0 };
		VkOffset2D offset_ = { 0,0 };
		std::vector<Attachment> colorAttachments_{};
		std::vector<AttachmentColorBlendSetup> colorAttachmentsBlendSetups;  // should either be {} or match colors size.
		Attachment depthAttachment_{ .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .clearValue = { 1.0f } /*.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE */ };
		//
		RecordingFunction recordingFunction_{};
	};

	struct CommandBuffer final {
	public:
		CommandBuffer() = delete;

		void BindRenderPipeline(ShaderProgram& shaderProgram, VertexFormat vertexFormat);
		void PushConstants(uint32_t offset, uint32_t size, const void* data);

		void SetViewport(const VkViewport& viewport);
		void SetScissor(const VkRect2D& scissor);

		void Draw(
			uint32_t vertexCount,
			uint32_t instanceCount = 1,
			uint32_t firstVertex = 0,
			uint32_t firstInstance = 0);

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
		explicit RenderGraph(ResourceContext& ctx) : ctx_(ctx) {
			pipelineCachePimpl = std::make_unique<PipelineCache>(&ctx_);
		};
		// could be called NewFrame really
		void NewFrame() { m_passes.clear(); }
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

		// 
		smvk::Pool<LogicalImage, LogicalImage> inputImages_;
		smvk::Pool<LogicalStorageImage, LogicalImage> storageImages_;
		smvk::Pool<LogicalImageView, LogicalImageView> imageViews_;
		smvk::Pool<LogicalBuffer, LogicalBuffer> storageBuffers_;
		//
		std::vector<RenderPass> m_passes;
		std::unique_ptr<PipelineCache> pipelineCachePimpl;
		smvk::ResourceContext& ctx_;

	};

	inline CommandBuffer::CommandBuffer(
		VkCommandBuffer cmd,
		RenderGraph& graph,
		RenderPass& renderPass)
		: cmd_(cmd)
		, graph_(&graph)
		, renderPass_(&renderPass)
	{}

	inline void CommandBuffer::BindRenderPipeline(ShaderProgram& shaderProgram, VertexFormat vertexFormat = VertexFormat())
	{
		assert(graph_ != nullptr);
		assert(renderPass_ != nullptr);

		std::vector<VkFormat> colorAttachmentFormats;
		colorAttachmentFormats.reserve(renderPass_->colorAttachments_.size());

		for (auto& attachment : renderPass_->colorAttachments_) {
			colorAttachmentFormats.push_back(
				graph_->imageViews_.get(attachment.view)->resolvedFormat);
		}

		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		const bool hasDepth = !renderPass_->depthAttachment_.view.empty();

		if (hasDepth) {
			depthFormat = graph_->imageViews_
				.get(renderPass_->depthAttachment_.view)
				->resolvedFormat;
		}

		auto bundle = graph_->pipelineCachePimpl->GetRenderPipeline(
			shaderProgram,
			vertexFormat,
			colorAttachmentFormats.data(),
			static_cast<uint32_t>(colorAttachmentFormats.size()),
			depthFormat,
			depthFormat
			// polygonmode = pass.polygonMode.
			// blend = pass.blend
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
		const void* data
		//stageMask stageMask
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

			if (hasDepth) {
				auto depthView = imageViews_.get(pass.depthAttachment_.view);

				depthInfo.imageView = depthView->resolvedImageView;
				depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
				depthInfo.loadOp = pass.depthAttachment_.loadOp;
				depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
				.pDepthAttachment = hasDepth ? &depthInfo : nullptr,
			};

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
