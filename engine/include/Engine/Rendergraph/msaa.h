#pragma once
// Transient MSAA 2x targets for the Main scene pass.
//
// The multisampled color image resolves (AVERAGE) into the single-sampled
// HDR intermediate; the multisampled depth image is transient (Clear ->
// DontCare, no resolve). Prepass/cull keep using the 1x swapchain depth.
// Both images are recreated when the swap extent or formats drift, mirroring
// HdrTarget::Ensure.
//
// Usage (app/src/main.cpp):
//   smvk::MsaaTarget msaa;
//   msaa.Ensure(ctx, rg, swapExtent, hdrFormat, depthFormat);
//   ... msaa.colorLogical / msaa.depthLogical ...

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>

#include <cstdint>

namespace smvk {

struct MsaaDesc {
	Format colorFormat = Format::RGBA16_Float;
	Format depthFormat = Format::D32_Float;
	const char* colorName = "msaaColor";
	const char* colorViewName = "msaaColorView";
	const char* colorLogicalName = "msaaColorLogical";
	const char* depthName = "msaaDepth";
	const char* depthViewName = "msaaDepthView";
	const char* depthLogicalName = "msaaDepthLogical";
};

struct MsaaTarget {
	Holder<ImageHandle> colorImage;
	Holder<ImageViewHandle> colorView;
	LogicalImageViewHandle colorLogical{};
	Holder<ImageHandle> depthImage;
	Holder<ImageViewHandle> depthView;
	LogicalImageViewHandle depthLogical{};
	Extent2D extent{ 0, 0 };
	Format colorFormat = Format::RGBA16_Float;
	Format depthFormat = Format::D32_Float;

	[[nodiscard]] bool Valid() const {
		return !colorImage.empty() && !colorView.empty() &&
			!colorLogical.empty() && !depthImage.empty() &&
			!depthView.empty() && !depthLogical.empty();
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent, const MsaaDesc& d = {}) {
		if (swapExtent.width == 0 || swapExtent.height == 0)
			return false;
		Destroy(rg);

		ImageCreateInfo colorCi{};
		colorCi.extent = { swapExtent.width, swapExtent.height, 1 };
		colorCi.format = d.colorFormat;
		colorCi.usage = ImageUsage::ColorAttachment;
		colorCi.samples = SampleCount::Count2;
		colorCi.mipLevels = 1;
		colorCi.arrayLayers = 1;
		colorCi.name = d.colorName;

		Holder<ImageHandle> newColorImage = ctx.CreateImage(colorCi, nullptr);
		if (newColorImage.empty())
			return false;

		ImageViewCreateInfo colorVci{};
		colorVci.viewType = ImageViewType::View2D;
		colorVci.format = Format::Undefined; // inherit
		colorVci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		colorVci.name = d.colorViewName;

		Holder<ImageViewHandle> newColorView = ctx.CreateImageView(newColorImage, colorVci);
		if (newColorView.empty())
			return false;

		LogicalImageViewHandle newColorLogical =
			rg.AddExternalImageView(newColorView, d.colorFormat, d.colorLogicalName);
		if (newColorLogical.empty())
			return false;

		ImageCreateInfo depthCi{};
		depthCi.extent = { swapExtent.width, swapExtent.height, 1 };
		depthCi.format = d.depthFormat;
		depthCi.usage = ImageUsage::DepthStencilAttachment;
		depthCi.samples = SampleCount::Count2;
		depthCi.mipLevels = 1;
		depthCi.arrayLayers = 1;
		depthCi.name = d.depthName;

		Holder<ImageHandle> newDepthImage = ctx.CreateImage(depthCi, nullptr);
		if (newDepthImage.empty())
			return false;

		ImageViewCreateInfo depthVci{};
		depthVci.viewType = ImageViewType::View2D;
		depthVci.format = Format::Undefined; // inherit
		depthVci.subresourceRange = { ImageAspect::Depth, 0, 1, 0, 1 };
		depthVci.name = d.depthViewName;

		Holder<ImageViewHandle> newDepthView = ctx.CreateImageView(newDepthImage, depthVci);
		if (newDepthView.empty())
			return false;

		LogicalImageViewHandle newDepthLogical =
			rg.AddExternalImageView(newDepthView, d.depthFormat, d.depthLogicalName);
		if (newDepthLogical.empty())
			return false;

		colorImage = std::move(newColorImage);
		colorView = std::move(newColorView);
		colorLogical = newColorLogical;
		depthImage = std::move(newDepthImage);
		depthView = std::move(newDepthView);
		depthLogical = newDepthLogical;
		extent = swapExtent;
		colorFormat = d.colorFormat;
		depthFormat = d.depthFormat;
		return true;
	}

	void Destroy(RenderGraph& rg) {
		if (!colorLogical.empty())
			rg.FreeImageView(colorLogical);
		if (!depthLogical.empty())
			rg.FreeImageView(depthLogical);
		colorLogical = {};
		depthLogical = {};
		colorView = {};
		colorImage = {};
		depthView = {};
		depthImage = {};
		extent = { 0, 0 };
	}

	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent, const MsaaDesc& d = {}) {
		if (Valid() &&
			extent.width == swapExtent.width && extent.height == swapExtent.height &&
			colorFormat == d.colorFormat && depthFormat == d.depthFormat) {
			return true;
		}
		return Create(ctx, rg, swapExtent, d);
	}
};

} // namespace smvk
