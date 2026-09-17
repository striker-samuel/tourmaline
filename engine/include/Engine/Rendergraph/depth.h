#pragma once
// Scene depth target (internal render resolution).
//
// Same builder pattern as HdrTarget: image + view + bindless + rendergraph
// alias, recreated on extent/format drift. DepthStencilAttachment|Sampled
// lets the Forward+ cull pass sample it bindless (registered in
// DepthStencilReadOnlyOptimal, matching the graph's depth read barriers).
//
// Usage (app/src/main.cpp):
//   smvk::DepthTarget renderDepth;
//   renderDepth.Ensure(ctx, rg, renderExtent, depthDesc); // renderExtent =
//                                                          swapExtent * renderScale
//   ... renderDepth.logicalView (prepass / main depth attachment)
//   ... renderDepth.view (ForwardPlus::Ensure depth bindless)

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>

#include <cstdint>

namespace smvk {

struct DepthDesc {
	Format format = Format::D32_Float;
	const char* name = "renderDepth";
	const char* viewName = "renderDepthView";
	const char* logicalName = "renderDepthLogical";
};

struct DepthTarget {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView;
	Extent2D extent{ 0, 0 };
	Format format = Format::D32_Float;

	[[nodiscard]] bool Valid() const { return !image.empty(); }

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const DepthDesc& d = {}) {
		if (targetExtent.width == 0 || targetExtent.height == 0)
			return false;
		Destroy(rg);

		ImageCreateInfo ci{};
		ci.extent = { targetExtent.width, targetExtent.height, 1 };
		ci.format = d.format;
		// TransferSrc: debug stage dumps (VULKANTOY_SSR_DUMP) — armed dumps
		// only (DownloadDataFromImage needs it on the source image).
		ci.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled;
		if (DebugImageDumpArmed())
			ci.usage |= ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = d.name;

		Holder<ImageHandle> newImage = ctx.CreateImage(ci, nullptr);
		if (newImage.empty())
			return false;

		ImageViewCreateInfo vci{};
		vci.viewType = ImageViewType::View2D;
		vci.format = Format::Undefined; // inherit
		vci.subresourceRange = { ImageAspect::Depth, 0, 1, 0, 1 };
		vci.name = d.viewName;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::DepthStencilReadOnlyOptimal);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newView, d.format, d.logicalName);
		if (newLogical.empty())
			return false;

		image = std::move(newImage);
		view = std::move(newView);
		bindless = std::move(newBindless);
		bindlessIndex = newBindlessIndex;
		logicalView = newLogical;
		extent = targetExtent;
		format = d.format;
		return true;
	}

	void Destroy(RenderGraph& rg) {
		if (!logicalView.empty())
			rg.FreeImageView(logicalView);
		logicalView = {};
		bindlessIndex = UINT32_MAX;
		bindless = {};
		view = {};
		image = {};
		extent = { 0, 0 };
	}

	// Recreate only when extent / format drift.
	// Returns true when the target is valid afterwards.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const DepthDesc& d = {}) {
		if (Valid() &&
			extent.width == targetExtent.width && extent.height == targetExtent.height &&
			format == d.format) {
			return true;
		}
		return Create(ctx, rg, targetExtent, d);
	}
};

} // namespace smvk
