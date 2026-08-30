#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <Engine/smvk.h>
#include <Engine/Renderer/mesh.h>

namespace smvk
{
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
	struct ImageViewKey
	{
		// cant reuse ImageCreateInfo beacuse it lacks handle property. Otherwise should be the same as this
		LogicalImageHandle image{};
		ImageViewType viewType =
			ImageViewType::View2D;
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
		BufferHandle resolvedBufferHandle{};
		const char* name = "";
	};


	struct LogicalImage
	{
		ResourceType type =
			ResourceType::Transient;
		ImageKey key{};
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
		ImageViewHandle resolvedViewHandle{};
		ImageViewHandle externalViewHandle{};
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
		{}

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

		// Shader-readable resources.
		std::vector<LogicalImageViewHandle>
			inputImages_;

		// Read/write shader resources.
		std::vector<LogicalImageViewHandle>
			storageImages_;

		std::vector<LogicalBufferHandle>
			storageBuffers_;

		// Render targets.
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
		{}

		~RenderGraph()
		{
			ReleaseResolvedResources();
		}

		// Targeted teardown of a single external view/image. Used to refresh
		// swapchain color views on Recreate() without clearing every external
		// resource the graph holds.
		void FreeImageView(
			LogicalImageViewHandle handle)
		{
			if (handle.empty())
				return;

			auto* view = imageViews_.get(handle);
			assert(view != nullptr);

			if (view->type !=
				LogicalImageView::Type::ExternalPhysical &&
				!view->resolvedViewHandle.empty())
			{
				destroy(&ctx_, view->resolvedViewHandle);
			}

			imageViews_.destroy(handle);
		}

		void FreeImage(
			LogicalImageHandle handle)
		{
			if (handle.empty())
				return;

			auto* image = images_.get(handle);
			assert(image != nullptr);

			if (image->type != ResourceType::External &&
				!image->resolvedImageHandle.empty())
			{
				destroy(&ctx_, image->resolvedImageHandle);
			}

			images_.destroy(handle);
		}

		// Destroys resolved views then transient images so each parent image's
		// liveViewCount reaches zero before the image is released. External
		// resources are owned by the caller and are left untouched.
		void ReleaseResolvedResources()
		{
			imageViews_.forEach(
				[this](LogicalImageView& view)
			{
				if (view.type ==
					LogicalImageView::Type::ExternalPhysical)
				{
					return;
				}

				if (!view.resolvedViewHandle.empty())
				{
					destroy(&ctx_, view.resolvedViewHandle);
				}
			}
			);

			images_.forEach(
				[this](LogicalImage& image)
			{
				if (image.type == ResourceType::External)
				{
					return;
				}

				if (!image.resolvedImageHandle.empty())
				{
					destroy(&ctx_, image.resolvedImageHandle);
				}
			}
			);
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

		RenderPass& AddComputePass(
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


		// Import an already-existing physical image. Needed for swapchain
		// This is the important external-resource API.
		// External images are imported as IMAGES, not VkImageViews.
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
			ImageViewHandle imageView,
			Format format,
			const char* name = "")
		{
			assert(!imageView.empty());

			LogicalImageView view{
				.type = LogicalImageView::Type::ExternalPhysical,
				.externalViewHandle = imageView,
				.externalFormat = format,
				.name = name,
			};

			return imageViews_.create(std::move(view));
		}

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

		//private:
		//	friend CommandBuffer;

		// --------------------------------------------------------
		// Resource resolution
		// --------------------------------------------------------

		void ResolveImages();
		void ResolveBuffers();
		void ResolveImageViews();

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


} // namespace smvk
