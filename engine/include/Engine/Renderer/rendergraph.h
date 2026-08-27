#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <Engine/Vulkan/impl.h>
#include <Engine/Renderer/mesh.h>

namespace smvk
{
	// Need to add these for convenience
	//struct Mipped{};
	//struct UnMipped{};

	struct RenderGraph;
	struct CommandBuffer;

	using LogicalBufferHandle =
		Handle<struct LogicalBuffer>;

	using LogicalImageHandle =
		Handle<struct LogicalImage>;

	using LogicalImageViewHandle =
		Handle<struct LogicalImageView>;


	// ============================================================
	// Logical resource descriptions
	// ============================================================

	using BufferKey = BufferCreateInfo;
	using ImageKey = ImageCreateInfo;

	//
	// IMPORTANT:
	//
	// This describes a logical view into an image.
	//
	// It is NOT a VkImageView.
	//
	// A physical VkImageView is resolved/cached later from:
	//
	//     resolved image
	//     + this key
	//
	// This means transient and external images follow exactly the
	// same path once the underlying ImageHandle has been resolved.
	//
	struct ImageViewKey
	{
		LogicalImageHandle image{};

		ImageViewType viewType =
			ImageViewType::View2D;

		//
		// Undefined = inherit the underlying image format.
		//
		Format format =
			Format::Undefined;

		ImageSubresourceRange subresourceRange{};

		bool operator==(const ImageViewKey&) const = default;
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
		ResourceType type =
			ResourceType::Transient;

		BufferKey key{};

		//
		// External:
		//     supplied by AddExternalBuffer().
		//
		// Transient:
		//     filled during ResolveBuffers().
		//
		BufferHandle resolvedBufferHandle{};

		const char* name = "";
	};


	struct LogicalImage
	{
		ResourceType type =
			ResourceType::Transient;

		ImageKey key{};

		//
		// External:
		//     supplied by AddExternalImage().
		//
		// Transient:
		//     filled during ResolveImages().
		//
		ImageHandle resolvedImageHandle{};

		const char* name = "";
	};


	struct LogicalImageView
	{
		enum class Type : uint8_t
		{
			Logical,
			ExternalPhysical
		};

		Type type = Type::Logical;

		ImageViewKey key{};

		VkImageView externalView = VK_NULL_HANDLE;
		Format externalFormat = Format::Undefined;

		const char* name = "";
	};


	// ============================================================
	// Render attachments
	// ============================================================

	struct RenderAttachment
	{
		LogicalImageViewHandle view{};

		AttachmentLoadOp loadOp =
			AttachmentLoadOp::DontCare;

		AttachmentStoreOp storeOp =
			AttachmentStoreOp::Store;

		ClearColor clearColor{};
	};


	struct DepthAttachment
	{
		LogicalImageViewHandle view{};

		AttachmentLoadOp loadOp =
			AttachmentLoadOp::DontCare;

		AttachmentStoreOp storeOp =
			AttachmentStoreOp::Store;

		ClearDepthStencil clear{};
	};


	// ============================================================
	// Render pass
	// ============================================================

	using RecordingFunction =
		std::function<void(CommandBuffer&)>;


	struct RenderPass final
	{
		explicit RenderPass(
			const char* name = nullptr)
			: name_(name)
		{
		}


		// --------------------------------------------------------
		// Attachments
		// --------------------------------------------------------

		RenderPass& SetColor(
			const std::vector<LogicalImageViewHandle>& views,
			AttachmentLoadOp loadOp =
				AttachmentLoadOp::Load,
			AttachmentStoreOp storeOp =
				AttachmentStoreOp::Store,
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
			AttachmentLoadOp loadOp =
				AttachmentLoadOp::Load,
			AttachmentStoreOp storeOp =
				AttachmentStoreOp::Store,
			ClearDepthStencil clearValue = {})
		{
			assert(!view.empty());

			depthAttachment_ = {
				.view = view,
				.loadOp = loadOp,
				.storeOp = storeOp,
				.clear = clearValue,
			};

			hasDepthAttachment_ = true;

			return *this;
		}


		// --------------------------------------------------------
		// Shader resources
		// --------------------------------------------------------

		RenderPass& SetInputImages(
			std::vector<LogicalImageViewHandle> images)
		{
			inputImages_ = std::move(images);
			return *this;
		}


		RenderPass& SetStorageImages(
			std::vector<LogicalImageViewHandle> images)
		{
			storageImages_ = std::move(images);
			return *this;
		}


		RenderPass& SetStorageBuffers(
			std::vector<LogicalBufferHandle> buffers)
		{
			storageBuffers_ = std::move(buffers);
			return *this;
		}


		// --------------------------------------------------------
		// Render area
		// --------------------------------------------------------

		RenderPass& SetExtent(
			Extent2D extent)
		{
			extent_ = extent;
			return *this;
		}


		RenderPass& SetOffset(
			int32_t x,
			int32_t y)
		{
			offsetX_ = x;
			offsetY_ = y;
			return *this;
		}


		RenderPass& SetRecordingFunc(
			RecordingFunction fn)
		{
			recordingFunction_ =
				std::move(fn);

			return *this;
		}


	private:
		friend RenderGraph;
		friend CommandBuffer;

		//
		// Shader-readable resources.
		//
		std::vector<LogicalImageViewHandle>
			inputImages_;

		//
		// Read/write shader resources.
		//
		std::vector<LogicalImageViewHandle>
			storageImages_;

		std::vector<LogicalBufferHandle>
			storageBuffers_;


		//
		// Render targets.
		//
		std::vector<RenderAttachment>
			colorAttachments_;

		DepthAttachment
			depthAttachment_{};

		bool hasDepthAttachment_ =
			false;

		Extent2D extent_{};

		int32_t offsetX_ = 0;
		int32_t offsetY_ = 0;

		RecordingFunction
			recordingFunction_{};

		const char* name_ = nullptr;
	};



	// ============================================================
	// Render graph
	// ============================================================

	struct RenderGraph final
	{
		explicit RenderGraph(
			VulkanResourceManager& ctx)
			: ctx_(ctx)
			, pipelineCachePimpl(
				std::make_unique<PipelineCache>(&ctx_))
		{
		}


		// --------------------------------------------------------
		// Frame
		// --------------------------------------------------------

		void NewFrame()
		{
			passes_.clear();
		}


		RenderPass& AddPass(
			const char* name = nullptr)
		{
			return passes_.emplace_back(name);
		}


		// ========================================================
		// Images
		// ========================================================

		[[nodiscard]]
		LogicalImageHandle AddImage(
			const ImageCreateInfo& description,
			const char* name = "")
		{
			LogicalImage image{
				.type =
					ResourceType::Transient,

				.key =
					description,

				.resolvedImageHandle = {},

				.name =
					name,
			};

			return images_.create(
				std::move(image));
		}


		//
		// Import an already-existing physical image.
		//
		// This is the important external-resource API.
		//
		// External images are imported as IMAGES, not VkImageViews.
		//
		[[nodiscard]]
		LogicalImageHandle AddExternalImage(
			ImageHandle imageHandle,
			const char* name = "")
		{
			assert(!imageHandle.empty());

			const Image* image =
				ctx_.images_.get(imageHandle);

			assert(image != nullptr);

			LogicalImage logicalImage{
				.type = ResourceType::External,

				.key = {
					.type = image->type,
					.extent = image->extent,
					.format = image->format,
					.usage = image->usage,
					.samples = image->samples,
					.mipLevels = image->mipLevels,
					.arrayLayers = image->arrayLayers,
					.name = name,
				},

				.resolvedImageHandle = imageHandle,

				.name = name,
			};

			return images_.create(
				std::move(logicalImage)
			);
		}


		// ========================================================
		// Image views
		// ========================================================

		[[nodiscard]]
		LogicalImageViewHandle AddImageView(
			LogicalImageHandle image,
			ImageViewType type =
				ImageViewType::View2D,
			Format format =
				Format::Undefined,
			ImageSubresourceRange range = {},
			const char* name = "")
		{
			assert(!image.empty());

			LogicalImageView view{
				.key = {
					.image =
						image,

					.viewType =
						type,

					.format =
						format,

					.subresourceRange =
						range,
				},

				.name =
					name,
			};

			return imageViews_.create(
				std::move(view));
		}

		LogicalImageViewHandle AddExternalImageView(
			VkImageView imageView,
			Format format,
			const char* name = "")
		{
			assert(imageView != VK_NULL_HANDLE);

			LogicalImageView view{
				.type = LogicalImageView::Type::ExternalPhysical,
				.externalView = imageView,
				.externalFormat = format,
				.name = name,
			};

			return imageViews_.create(std::move(view));
		}
		//
		// Convenience function:
		//
		// import physical image, then create logical view.
		//
		[[nodiscard]]
		LogicalImageViewHandle AddExternalImageView(
			ImageHandle image,
			ImageViewType type =
				ImageViewType::View2D,
			Format format =
				Format::Undefined,
			ImageSubresourceRange range = {},
			const char* name = "")
		{
			auto logicalImage =
				AddExternalImage(
					image,
					name);

			return AddImageView(
				logicalImage,
				type,
				format,
				range,
				name);
		}


		// ========================================================
		// Buffers
		// ========================================================

		template<typename T>
		[[nodiscard]]
		LogicalBufferHandle AddBuffer(
			uint32_t count,
			BufferType usage =
				BufferType_Storage,
			const char* name = "")
		{
			LogicalBuffer buffer{
				.type =
					ResourceType::Transient,

				.key = {
					.size =
						sizeof(T) * count,

					.bufferType =
						usage,

					.name =
						name,
				},

				.resolvedBufferHandle = {},

				.name =
					name,
			};

			return buffers_.create(
				std::move(buffer));
		}


		[[nodiscard]]
		LogicalBufferHandle AddExternalBuffer(
			BufferHandle bufferHandle,
			const char* name = "")
		{
			assert(!bufferHandle.empty());

			LogicalBuffer buffer{
				.type =
					ResourceType::External,

				.resolvedBufferHandle =
					bufferHandle,

				.name =
					name,
			};

			return buffers_.create(
				std::move(buffer));
		}


		// ========================================================
		// Resolution
		// ========================================================

		[[nodiscard]]
		BufferHandle GetResolvedBufferHandle(
			LogicalBufferHandle handle) const
		{
			assert(!handle.empty());

			auto* buffer =
				buffers_.get(handle);

			assert(buffer != nullptr);

			return buffer->resolvedBufferHandle;
		}


		[[nodiscard]]
		ImageHandle GetResolvedImageHandle(
			LogicalImageHandle handle) const
		{
			assert(!handle.empty());

			auto* image =
				images_.get(handle);

			assert(image != nullptr);

			return image->resolvedImageHandle;
		}


		//
		// THIS is where VkImageView belongs.
		//
		// LogicalImageView does not permanently own one.
		//
		[[nodiscard]]
		VkImageView GetResolvedImageView(
			LogicalImageViewHandle handle);


		[[nodiscard]]
		Format GetResolvedImageViewFormat(
			LogicalImageViewHandle handle) const
		{
			assert(!handle.empty());

			const auto* view = imageViews_.get(handle);
			assert(view != nullptr);

			if (view->type == LogicalImageView::Type::ExternalPhysical)
			{
				assert(view->externalFormat != Format::Undefined);
				return view->externalFormat;
			}

			if (view->key.format != Format::Undefined)
			{
				return view->key.format;
			}

			const auto* image =
				images_.get(view->key.image);

			assert(image != nullptr);
			assert(image->key.format != Format::Undefined);

			return image->key.format;
		}


		// ========================================================
		// Execution
		// ========================================================

		void Execute(FrameBuffer& fb);


	private:
		friend CommandBuffer;


		// --------------------------------------------------------
		// Resource resolution
		// --------------------------------------------------------

		void ResolveImages();
		void ResolveBuffers();


		//
		// Resolve/create a physical VkImageView from:
		//
		//     ImageHandle
		//     ImageViewType
		//     Format
		//     ImageSubresourceRange
		//
		// This should eventually be backed by an image-view cache.
		//
		[[nodiscard]]
		VkImageView ResolveImageView(
			const ImageViewKey& key);


		// --------------------------------------------------------
		// Graph storage
		// --------------------------------------------------------

		Pool<LogicalImage, LogicalImage>
			images_;

		Pool<LogicalImageView, LogicalImageView>
			imageViews_;

		Pool<LogicalBuffer, LogicalBuffer>
			buffers_;

		std::vector<RenderPass>
			passes_;


		VulkanResourceManager& ctx_;

		std::unique_ptr<PipelineCache>
			pipelineCachePimpl;
	};



	// ============================================================
	// Command buffer
	// ============================================================

	struct CommandBuffer final
	{
		CommandBuffer() = delete;

		void BindRenderPipeline(
			ShaderProgram& shaderProgram,
			VertexFormat vertexFormat =
				VertexFormat::Empty,
			const DepthSettings& depthSettings =
				DepthSettings::DepthTest(),
			const StencilSettings& stencilSettings =
				StencilSettings::DefaultDisabled(),
			const BlendSettings& blendSettings =
				BlendSettings::Opaque(),
			const MSAASettings& msaaSettings =
				MSAASettings::Count1()
		);

		void PushConstants(
			uint32_t offset,
			uint32_t size,
			const void* data,
			ShaderStage stageMask =
			ShaderStage::AllGraphics
			//ShaderStage::Vertex |
			//ShaderStage::Fragment
		);
	
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
		}

		void DrawIndexed(
			uint32_t indexCount,
			uint32_t instanceCount = 1,
			uint32_t firstIndex = 0,
			uint32_t baseVertex = 0,
			uint32_t firstInstance = 0)
		{
			vkCmdDrawIndexed(
				cmd_,
				indexCount,
				instanceCount,
				firstIndex,
				static_cast<int32_t>(
					baseVertex),
				firstInstance);
		}


	private:
		friend RenderGraph;


		CommandBuffer(
			VkCommandBuffer cmd,
			RenderGraph& graph,
			RenderPass& pass)
			: cmd_(cmd)
			, graph_(&graph)
			, renderPass_(&pass)
		{
		}


		VkCommandBuffer cmd_ =
			VK_NULL_HANDLE;

		RenderGraph* graph_ =
			nullptr;

		RenderPass* renderPass_ =
			nullptr;

		VkPipeline currentPipeline_ =
			VK_NULL_HANDLE;

		VkPipelineLayout currentPipelineLayout_ =
			VK_NULL_HANDLE;
	};

}
