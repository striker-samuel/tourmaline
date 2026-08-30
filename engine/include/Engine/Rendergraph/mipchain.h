#pragma once
#include "rendergraph.h"

namespace smvk {

	struct LogicalMipChain
	{
		LogicalMipChain(
			RenderGraph* rg,
			smvk::Format format,
			smvk::Extent2D extent,
			smvk::ImageUsage usage,
			uint32_t mipLevels = 0,
			smvk::ImageAspect aspect = smvk::ImageAspect::None,
			const char* name = nullptr)
			: extent_(extent)
		{
			assert(extent.width != 0);
			assert(extent.height != 0);

			ImageCreateInfo info{
				.type = ImageType::Image2D,
				.extent = { extent.width, extent.height, 1 },
				.format = format,
				.usage = usage,
				.samples = SampleCount::Count1,
				.mipLevels = mipLevels,
				.arrayLayers = 1,
				.name = name
			};

			image = rg->AddImage(info, name);

			if (aspect == ImageAspect::None)
				aspect = DefaultViewAspect(format);

			view = rg->AddImageView(
				image,
				ImageViewType::View2D,
				format,
				{
					.aspect = aspect,
					.baseMipLevel = 0,
					.mipLevelCount = RemainingSubresources,
					.baseArrayLayer = 0,
					.arrayLayerCount = 1
				}
			);

			mipViews.reserve(mipLevels);
			for (uint32_t mipIndex = 0; mipIndex < mipLevels; ++mipIndex)
			{
				mipViews.push_back(
					rg->AddImageView(
						image,
						ImageViewType::View2D,
						format,
						{
							.aspect = aspect,
							.baseMipLevel = mipIndex,
							.mipLevelCount = 1,
							.baseArrayLayer = 0,
							.arrayLayerCount = 1
						}
					)
				);
			}
		}

		[[nodiscard]] uint32_t MipCount() const
		{
			return static_cast<uint32_t>(mipViews.size());
		}

		[[nodiscard]] LogicalImageViewHandle MipView(uint32_t mip) const
		{
			assert(mip < mipViews.size());
			return mipViews[mip];
		}

		smvk::Extent2D extent_;
		LogicalImageHandle image;
		LogicalImageViewHandle view;
		std::vector<LogicalImageViewHandle> mipViews;
	};

	enum struct MipFilter
	{
		Average,
		Linear,
		None
	};

	// TODO:: Compelte implementation. but needs compute support
	//void BuildMipChain(
	//	RenderGraph* rg, // this can lead to bugs if called with a differnet rg from the one the mipchain was created of hmmm
	//	const LogicalMipChain& mipChain,
	//	MipFilter filter = MipFilter::Average
	//) {
	//	Extent2D size = mipChain.extent_;
	//	for (size_t mipIndex = 1; mipIndex < mipChain.MipCount(); mipIndex++)
	//	{
	//		size.width /= 2;
	//		size.height /= 2;
	//		auto src = mipChain.mipViews[mipIndex - 1];
	//		auto dst = mipChain.mipViews[mipIndex];
	//		rg->AddComputePass()
	//			.SetColor({ dst }, AttachmentLoadOp::DontCare)
	//			.SetExtent(size)
	//			.SetRecordingFunc(
	//				[](CommandBuffer& cmd) {
	//			//BindCompute and pushConstatns for filter values
	//			//Run
	//		});
	//	}
	//}

}; // namespace smvk