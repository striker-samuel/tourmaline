#pragma once

#include <functional>
#include <vector>

#include <Engine/Vulkan/impl.h>
#include <Engine/Renderer/mesh.h>


namespace smvk {

	struct RenderGraph;
	struct CommandBuffer;

	using LogicalBufferHandle = Handle<struct LogicalBuffer>;
	using LogicalImageHandle = Handle<struct LogicalImage>;
	using LogicalImageViewHandle = Handle<struct LogicalImageView>;

	// ============================================================
	// Logical Resoruce Keys
	// ============================================================

	using BufferKey = BufferCreateInfo;
	using ImageKey = ImageCreateInfo;
	struct ImageViewKey // this neesds rework
	{
		LogicalImageHandle image;
		ImageViewType viewType = ImageViewType::View2D;
		Format format = Format::Undefined;
		ImageSubresourceRange subresourceRange{};
	};


	// ============================================================
	// Logical resources
	// ============================================================

	enum class ResourceType : uint8_t
	{
		Transient,
		External
	};

	struct LogicalBuffer
	{
		ResourceType type = ResourceType::Transient;
		BufferKey key{};
		BufferHandle resolvedBufferHandle{};
		const char* name = "";
	};

	struct LogicalImage
	{
		ResourceType type = ResourceType::Transient;
		ImageKey key{};
		ImageHandle resolvedImageHandle{};
		const char* name = "";
	};

	struct LogicalImageView
	{
		ResourceType type = ResourceType::Transient;
		ImageViewKey key{};
		VkImageView resolvedImageView = VK_NULL_HANDLE; // this is bad
		Format resolvedFormat = Format::Undefined;
		const char* name = "";
	};

	// ============================================================
	// Render attachments
	// ============================================================

	struct RenderAttachment
	{
		LogicalImageViewHandle view;
		AttachmentLoadOp loadOp = AttachmentLoadOp::DontCare;
		AttachmentStoreOp storeOp = AttachmentStoreOp::Store;
		ClearColor clearColor{};
	};

	struct DepthAttachment
	{
		LogicalImageViewHandle view;
		AttachmentLoadOp loadOp = AttachmentLoadOp::DontCare;
		AttachmentStoreOp storeOp = AttachmentStoreOp::Store;
		ClearDepthStencil clear{};
	};


	// ============================================================
	// Render pass
	// ============================================================

	using RecordingFunction =
		std::function<void(CommandBuffer&)>;

	struct RenderPass final
	{
		explicit RenderPass(const char* name = nullptr)
			: name_(name)
		{}

		RenderPass& SetColor(
			const std::vector<LogicalImageViewHandle>& views,
			AttachmentLoadOp loadOp = AttachmentLoadOp::Load,
			AttachmentStoreOp storeOp = AttachmentStoreOp::Store,
			ClearColor clearValue = {})
		{
			colorAttachments_.resize(views.size());

			for (size_t i = 0; i < views.size(); ++i)
			{
				assert(!views[i].empty());

				colorAttachments_[i] = {
					.view = views[i],
					.loadOp = loadOp,
					.storeOp = storeOp,
					.clearColor = clearValue,
				};
			}

			return *this;
		}

		RenderPass& SetDepth(
			LogicalImageViewHandle view,
			AttachmentLoadOp loadOp = AttachmentLoadOp::Load,
			AttachmentStoreOp storeOp = AttachmentStoreOp::Store,
			ClearDepthStencil clearValue = {})
		{
			assert(!view.empty());

			depthAttachment_ = {
				.view = view,
				.loadOp = loadOp,
				.storeOp = storeOp,
				.clear = clearValue,
			};

			return *this;
		}

		RenderPass& SetInputImages(
			std::vector<LogicalImageHandle> images)
		{
			inputImages_ = std::move(images);
			return *this;
		}

		RenderPass& SetStorageBuffers(
			std::vector<LogicalBufferHandle> buffers)
		{
			storageBuffers_ = std::move(buffers);
			return *this;
		}

		RenderPass& SetExtent(Extent2D extent)
		{
			extent_ = extent;
			return *this;
		}

		RenderPass& SetOffset(int32_t x, int32_t y)
		{
			offsetX_ = x;
			offsetY_ = y;
			return *this;
		}

		RenderPass& SetRecordingFunc(
			RecordingFunction fn)
		{
			recordingFunction_ = std::move(fn);
			return *this;
		}

	private:
		friend RenderGraph;
		friend CommandBuffer;
		std::vector<LogicalImageHandle> inputImages_;
		std::vector<LogicalBufferHandle> storageBuffers_;
		std::vector<RenderAttachment> colorAttachments_;
		DepthAttachment depthAttachment_{};
		Extent2D extent_{};
		int32_t offsetX_ = 0;
		int32_t offsetY_ = 0;
		RecordingFunction recordingFunction_{};
		const char* name_ = nullptr;
	};



	// ============================================================
	// Render graph
	// ============================================================

	struct RenderGraph final
	{
		explicit RenderGraph(VulkanResourceManager& ctx)
			: ctx_(ctx)
			, pipelineCachePimpl(
				std::make_unique<PipelineCache>(&ctx_))
		{}

		void NewFrame()
		{
			passes_.clear();
		}

		RenderPass& AddPass(const char* name = nullptr)
		{
			return passes_.emplace_back(name);
		}


		// --------------------------------------------------------
		// Images
		// --------------------------------------------------------

		[[nodiscard]]
		LogicalImageHandle AddImage(
			const ImageCreateInfo& description,
			const char* name = "")
		{
			LogicalImage image{
				.type = ResourceType::Transient,
				.key = description,
				.name = name,
			};

			return images_.create(std::move(image));
		}

		[[nodiscard]]
		LogicalImageHandle AddImage(ImageHandle imageHandle, const char* name = "");

		// --------------------------------------------------------
		// Image views
		// --------------------------------------------------------

		[[nodiscard]]
		LogicalImageViewHandle AddImageView(
			LogicalImageHandle image,
			ImageViewType type = ImageViewType::View2D,
			Format format = Format::Undefined,
			ImageSubresourceRange range = {},
			const char* name = "")
		{
			assert(!image.empty());

			LogicalImageView view{
				.type = ResourceType::Transient,
				.key = {
					.image = image,
					.viewType = type,
					.format = format,
					.subresourceRange = range,
				},
				.name = name,
			};

			return imageViews_.create(std::move(view));
		}

		[[nodiscard]]
		LogicalImageViewHandle AddExternalImageView(
			VkImageView imageView,
			Format format,
			const char* name = "")
		{
			assert(imageView != VK_NULL_HANDLE);
			LogicalImageView view{
				.type = ResourceType::External,
				.resolvedImageView = imageView,
				.resolvedFormat = format,
				.name = name,
			};

			return imageViews_.create(std::move(view));
		}


		// --------------------------------------------------------
		// Buffers
		// --------------------------------------------------------

		template<typename T>
		[[nodiscard]]
		LogicalBufferHandle AddBuffer(
			uint32_t count,
			BufferType usage = BufferType::Storage,
			const char* name = "")
		{
			LogicalBuffer buffer{
				.type = ResourceType::Transient,
				.key = {
					.elementSize = sizeof(T),
					.elementCount = count,
					.usage = usage,
					.ring = false,
				},
				.name = name,
			};
		}

		[[nodiscard]]
		BufferHandle GetResolvedBufferHandle(
			LogicalBufferHandle handle) const
		{
			return buffers_.get(handle)->resolvedBufferHandle;
		}

		[[nodiscard]]
		ImageHandle GetResolvedImageHandle(
			LogicalImageHandle handle) const
		{
			return images_.get(handle)->resolvedImageHandle;
		}

		void Execute(FrameBuffer& fb);

	private:
		friend CommandBuffer;

		void ResolveImages();
		void ResolveImageViews();
		void ResolveBuffers();

		Pool<LogicalImage, LogicalImage> images_;
		//Pool<LogicalImage, LogicalImage> storageImages_;
		Pool<LogicalImageView, LogicalImageView> imageViews_;
		Pool<LogicalBuffer, LogicalBuffer> buffers_;
		std::vector<RenderPass> passes_;

		VulkanResourceManager& ctx_;
		std::unique_ptr<PipelineCache> pipelineCachePimpl;
	};



	// ============================================================
	// Command buffer
	// ============================================================

	struct CommandBuffer final
	{
		CommandBuffer() = delete;

		void BindRenderPipeline(
			ShaderProgram& shaderProgram,
			VertexFormat vertexFormat = VertexFormat::Empty,
			const DepthSettings& depthSettings = DepthSettings::DepthTest(),
			const StencilSettings& stencilSettings = StencilSettings::DefaultDisabled(),
			const BlendSettings& blendSettings = BlendSettings::Opaque(),
			const MSAASettings& msaaSettings = MSAASettings::Count1()
		);

		void PushConstants(
			uint32_t offset,
			uint32_t size,
			const void* data,
			ShaderStage stageMask = ShaderStage::Vertex | ShaderStage::Fragment
		)
		{
			assert(currentPipelineLayout_ != VK_NULL_HANDLE);
			vkCmdPushConstants(
				cmd_,
				currentPipelineLayout_,
				VK_SHADER_STAGE_ALL_GRAPHICS,
				//toVkShaderStage(stageMask), //may be really bad
				offset,
				size,
				data
			);
		};

		void Draw(
			uint32_t vertexCount,
			uint32_t instanceCount = 1,
			uint32_t firstVertex = 0,
			uint32_t firstInstance = 0)
		{
			vkCmdDraw(
				cmd_,
				vertexCount,
				instanceCount,
				firstVertex,
				firstInstance);
		};

		void DrawIndexed(
			uint32_t indexCount,
			uint32_t instanceCount = 1,
			uint32_t firstIndex = 0,
			uint32_t baseVertex = 0,
			uint32_t firstInstance = 0
		)
		{
			vkCmdDrawIndexed(
				cmd_,
				indexCount,
				instanceCount,
				firstIndex,
				static_cast<int32_t>(baseVertex),
				firstInstance);
		};

	private:
		friend RenderGraph;

		CommandBuffer(
			VkCommandBuffer cmd,
			RenderGraph& graph,
			RenderPass& pass)
			: cmd_(cmd)
			, graph_(&graph)
			, renderPass_(&pass)
		{}

		VkCommandBuffer cmd_ = VK_NULL_HANDLE;
		RenderGraph* graph_ = nullptr;
		RenderPass* renderPass_ = nullptr;
		VkPipeline currentPipeline_ = VK_NULL_HANDLE;
		VkPipelineLayout currentPipelineLayout_ = VK_NULL_HANDLE;
	};



}

