// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include "rendergraph.h"
#include <Engine/RHI/resource_manager.h>

#include <algorithm>
#include <cstdint>

namespace smvk {

	// ------------------------------------------------------------------
	// Mip extent helpers (shared by LogicalMipChain, MipBuilder, bloom)
	// ------------------------------------------------------------------
	inline Extent2D MipExtentFor(Extent2D base, uint32_t mip) {
		Extent2D out{
			std::max(1u, base.width >> mip),
			std::max(1u, base.height >> mip),
		};
		return out;
	}

	// Full mip pyramid depth for base (incl. mip 0), clamped to maxMips.
	inline uint32_t CalcMipLevels(Extent2D base, uint32_t maxMips = 32) {
		assert(base.width != 0 && base.height != 0);
		uint32_t levels = 1;
		uint32_t w = base.width, h = base.height;
		while ((w > 1 || h > 1) && levels < maxMips) {
			w = std::max(1u, w >> 1);
			h = std::max(1u, h >> 1);
			++levels;
		}
		return std::min(levels, maxMips);
	}

	struct LogicalMipChain
	{
		// Single-image pyramid (one VkImage, N views). Cheap, but note:
		// the rendergraph tracks barriers per *physical image* with a whole-
		// image layout, so rendering + sampling different mips of the SAME
		// image in overlapping passes aliases layouts. Fine for auto-mipgen
		// via blits; for bloom (sample prev mip while rendering next, then
		// additive upsample) prefer MipBuilder::BuildSeparated below, which
		// gives each mip its own physical image.
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

		[[nodiscard]] smvk::Extent2D MipExtent(uint32_t mip) const {
			assert(mip < mipViews.size());
			return MipExtentFor(extent_, mip);
		}

		smvk::Extent2D extent_;
		LogicalImageHandle image;
		LogicalImageViewHandle view;
		std::vector<LogicalImageViewHandle> mipViews;
	};

	// ------------------------------------------------------------------
	// MipBuilder – separated per-mip chain (one physical image per mip).
	//
	// Why separated? RenderGraph::CollectPassUsages tracks barriers per
	// PHYSICAL image with a single whole-image layout. A single-image
	// pyramid (LogicalMipChain) therefore aliases layouts when pass N
	// samples mip N-1 while rendering mip N. Separate images give each
	// mip independent barriers/layouts, which is what bloom needs:
	//   down:  mip[i] = downsample(mip[i-1])   (or HDR for i == 0)
	//   up:    mip[i] += tentUpsample(mip[i+1]) via additive blending
	//
	// Each mip owns: physical image + view + bindless + rendergraph alias
	// (external logical view), mirroring CascadedShadowAtlas / HdrResources.
	// ------------------------------------------------------------------
	struct MipBuilder {
		struct BuiltMip {
			Holder<ImageHandle> image;
			Holder<ImageViewHandle> view;
			Holder<BindlessImageHandle> bindless;
			uint32_t bindlessIndex = UINT32_MAX;
			LogicalImageViewHandle logical{};
			Extent2D extent{ 0, 0 };
		};

		struct BuiltChain {
			std::vector<BuiltMip> mips;
			Extent2D baseExtent{ 0, 0 };
			Format format = Format::Undefined;

			[[nodiscard]] bool Valid() const { return !mips.empty(); }
			[[nodiscard]] uint32_t MipCount() const { return static_cast<uint32_t>(mips.size()); }
			[[nodiscard]] const BuiltMip& Mip(uint32_t i) const {
				assert(i < mips.size());
				return mips[i];
			}
		};

		// Builds mipCount separated images, mip 0 = baseExtent, each next
		// halved (clamped to 1). Returns empty chain on failure.
		// usage must include ColorAttachment | Sampled for bloom, plus
		// Storage when a compute downsampler (SPD) writes the mips.
		// bindlessLayout selects the descriptor registration: General for
		// images a compute pass stores into (EVSM blurTemp precedent —
		// sampling a General-registered image works the same).
		static BuiltChain BuildSeparated(
			ResourceManager& ctx,
			RenderGraph& rg,
			Extent2D baseExtent,
			uint32_t mipCount,
			Format format,
			ImageUsage usage,
			const char* name = "mip",
			ImageLayout bindlessLayout = ImageLayout::ShaderReadOnlyOptimal) {
			BuiltChain out{};
			if (mipCount == 0 || baseExtent.width == 0 || baseExtent.height == 0)
				return out;
			out.mips.reserve(mipCount);
			out.baseExtent = baseExtent;
			out.format = format;

			for (uint32_t i = 0; i < mipCount; ++i) {
				BuiltMip m{};
				m.extent = MipExtentFor(baseExtent, i);

				ImageCreateInfo ci{};
				ci.type = ImageType::Image2D;
				ci.extent = { m.extent.width, m.extent.height, 1 };
				ci.format = format;
				ci.usage = usage;
				ci.samples = SampleCount::Count1;
				ci.mipLevels = 1;
				ci.arrayLayers = 1;
				ci.name = name;

				m.image = ctx.CreateImage(ci, nullptr);
				if (m.image.empty())
					return BuiltChain{};

				ImageViewCreateInfo vci{};
				vci.viewType = ImageViewType::View2D;
				vci.format = Format::Undefined; // inherit
				vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
				vci.name = name;

				m.view = ctx.CreateImageView(m.image, vci);
				if (m.view.empty())
					return BuiltChain{};

				m.bindless = ctx.CreateBindlessImage(m.view, bindlessLayout);
				if (m.bindless.empty())
					return BuiltChain{};
				m.bindlessIndex = ctx.GetBindlessIndex(m.bindless);

				m.logical = rg.AddExternalImageView(m.view, format, name);
				if (m.logical.empty())
					return BuiltChain{};

				out.mips.push_back(std::move(m));
			}
			return out;
		}

		// Frees rendergraph aliases; physical holders die with the chain.
		static void DestroySeparated(RenderGraph& rg, BuiltChain& chain) {
			for (auto& m : chain.mips) {
				if (!m.logical.empty())
					rg.FreeImageView(m.logical);
				m.logical = {};
				m.bindlessIndex = UINT32_MAX;
				m.bindless = {};
				m.view = {};
				m.image = {};
			}
			chain.mips.clear();
			chain.baseExtent = { 0, 0 };
		}
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