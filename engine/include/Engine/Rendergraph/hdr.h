#pragma once
// HDR intermediate target (scene color before tonemap).
//
// Ports the pattern used by BloomChain / CascadedShadowAtlas into one place:
// resource creation (image + view + bindless + rendergraph alias) + resize
// handling. Sampling is bindless (textures[] indexed by push), barriers come
// from SetColor (write) + SetInputImages (read).
//
// Usage (app/src/main.cpp):
//   smvk::HdrTarget hdr;
//   hdr.Ensure(ctx, rg, swapExtent);   // create / resize
//   ... hdr.logicalView / hdr.bindlessIndex ...

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>

#include <cstdint>

namespace smvk {

struct HdrDesc {
	Format format = Format::RGBA16_Float; // HDR, must be color-renderable
	const char* name = "hdr";
	const char* viewName = "hdrView";
	const char* logicalName = "hdrLogical";
};

struct HdrTarget {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView;
	Extent2D extent{ 0, 0 };
	Format format = Format::RGBA16_Float;

	[[nodiscard]] bool Valid() const { return !image.empty(); }

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent, const HdrDesc& d = {}) {
		if (swapExtent.width == 0 || swapExtent.height == 0)
			return false;
		Destroy(rg);

		ImageCreateInfo ci{};
		ci.extent = { swapExtent.width, swapExtent.height, 1 };
		ci.format = d.format;
		// TransferSrc: debug stage dumps (TOURMALINE_SSR_DUMP) — armed dumps
		// only (DownloadDataFromImage needs it on the source image).
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
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
		vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = d.viewName;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
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
		extent = swapExtent;
		format = d.format;
		ctx.GetOrCreateSampler(SamplerCreateInfo{});
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

	// Recreate only when swap extent / format drift.
	// Returns true when the target is valid afterwards.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent, const HdrDesc& d = {}) {
		if (Valid() &&
			extent.width == swapExtent.width && extent.height == swapExtent.height &&
			format == d.format) {
			return true;
		}
		return Create(ctx, rg, swapExtent, d);
	}
};

} // namespace smvk
