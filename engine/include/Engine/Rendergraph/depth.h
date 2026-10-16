// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Scene depth target (internal render resolution).
//
// Same builder pattern as HdrTarget: image + view + bindless + rendergraph
// alias, recreated on extent/format drift. DepthStencilAttachment|Sampled
// lets the Forward+ cull pass sample it bindless (registered in
// DepthStencilReadOnlyOptimal, matching the graph's depth read barriers).
//
// Stencil aspect (D32S8/D24S8 devices — the supported depth formats all
// carry one): the SSR receiver-material layer lives in the stencil bits
// (see Rendergraph/stencil_bits.h). The target therefore owns a second
// pair of views next to the depth-only sampled view:
//   - dsView (Depth|Stencil aspects): the ATTACHMENT view registered as
//     `logicalView` — every SetDepth site (prepass, Main, motion, panes)
//     attaches it so stencil writes land (a depth-aspect-only attachment
//     would silently drop them). Sampling barriers route through the
//     same physical image, so input consumers see no difference.
//   - stencilView (Stencil aspect, S8_UInt): the bindless SAMPLING view
//     (stencilTextureIndex + stencilLogicalView) consumed by the SSR
//     trace/composite through the uintTextures[] bindless alias.
// Both are empty for stencil-less formats (pure D32_Float targets): SSR
// falls back to the legacy uniform-F0 behavior via its sentinel gates.
//
// Usage (app/src/main.cpp):
//   smvk::DepthTarget renderDepth;
//   renderDepth.Ensure(ctx, rg, renderExtent, depthDesc); // renderExtent =
//                                                          swapExtent * renderScale
//   ... renderDepth.logicalView (prepass / main depth attachment, DS aspects)
//   ... renderDepth.view (ForwardPlus::Ensure depth bindless)
//   ... renderDepth.stencilBindlessIndex (SSR receiver-material stencil)

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>

#include <cstdint>

namespace smvk {

struct DepthDesc {
	Format format = Format::D32_Float;
	const char* name = "renderDepth";
	const char* viewName = "renderDepthView";
	const char* logicalName = "renderDepthLogical";
	const char* stencilViewName = "renderDepthStencilView";
	const char* stencilLogicalName = "renderDepthStencilLogical";
	// Target-heap use window (design-A interval aliasing, see
	// ClaimTargetHeapRange + TargetPhase): same-shape targets with
	// DISJOINT windows share one packed memory range. DEFAULTS are the
	// full frame (never shares - always CORRECT, just no win); the two
	// real depth targets declare their true windows at the call site:
	//   renderDepth: [Seed .. Tonemap] (seed/prepass write, post reads)
	//   hizDepth:    [OccluderPrepass .. Hzb] (fill, pyramid build read)
	// The pair is exactly disjoint - the two full-screen depths share one
	// range (the biggest aliasing win in the target set). A too-narrow
	// window CORRUPTS (two live targets on one range) - widen, never
	// narrow, when unsure.
	TargetPhase firstPhase = TargetPhase::Shadows;
	TargetPhase lastPhase = TargetPhase::Composite;
};

struct DepthTarget {
	// Target-heap claim (FIRST member: destruction order - the claim
	// releases BEHIND the image's deferred destroy; the range free must
	// land behind the destroy in the deferred FIFO). Empty = unclaimed
	// (the plain VMA path or pre-conversion targets).
	Holder<TargetHeapClaimHandle> claim;
	Holder<ImageHandle> image;
	// Depth-aspect sampled view (bindless depth reads — unchanged).
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	// Depth+Stencil attachment view — the view behind `logicalView`
	// (see header comment). Not bound into the bindless array: only
	// the aspect-isolated views above are ever sampled.
	Holder<ImageViewHandle> dsView;
	// Stencil-aspect sampled view (S8_UInt) + bindless — the SSR
	// receiver-material stencil reader.
	Holder<ImageViewHandle> stencilView;
	Holder<BindlessImageHandle> stencilBindless;
	uint32_t stencilBindlessIndex = UINT32_MAX;
	// Attachment alias (backs the DS view) — SetDepth sites attach this.
	LogicalImageViewHandle logicalView;
	// Stencil sampling alias for input barriers of the stencil view.
	LogicalImageViewHandle stencilLogicalView;
	Extent2D extent{ 0, 0 };
	Format format = Format::D32_Float;

	[[nodiscard]] bool Valid() const { return !image.empty(); }
	[[nodiscard]] bool StencilValid() const {
		return stencilBindlessIndex != UINT32_MAX
			&& !stencilLogicalView.empty();
	}
	[[nodiscard]] bool HasStencilAspect() const {
		return FormatHasStencilAspect(format);
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const DepthDesc& d = {}) {
		if (targetExtent.width == 0 || targetExtent.height == 0)
			return false;
		Destroy(rg);

		ImageCreateInfo ci{};
		ci.extent = { targetExtent.width, targetExtent.height, 1 };
		ci.format = d.format;
		// TransferSrc: debug stage dumps (TOURMALINE_SSR_DUMP) — armed dumps
		// only (DownloadDataFromImage needs it on the source image).
		ci.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled;
		if (DebugImageDumpArmed())
			ci.usage |= ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = d.name;

		// LOCAL DECLARATION ORDER MATTERS: newClaim declared FIRST
		// destroys LAST on the fail path's scope exit - the range free
		// must land BEHIND the image's deferred destroy in the FIFO.
		Holder<TargetHeapClaimHandle> newClaim;
		// Design-A interval aliasing: the target is an UNBOUND heap image
		// (CreateHeapImage - no VmaAllocation), then claimed + bound at a
		// packed range (same-shape disjoint-window targets share ranges -
		// see the desc's window comment).
		Holder<ImageHandle> newImage = ctx.CreateHeapImage(ci);
		if (newImage.empty())
			return false;
		newClaim = ctx.ClaimTargetHeapRange(newImage, d.firstPhase, d.lastPhase);
		if (newClaim.empty())
			return false;

		// Depth-only sampled view (bindless depth reads — the pre-change
		// behavior; the Forward+ cull pass consumes this one).
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

		// Depth+Stencil attachment view: the SetDepth attachment for every
		// pass using this target. Stencil writes (SSR receiver-material
		// bits) need the stencil aspect in the attachment view; the format
		// still resolves to the parent DS format, so pipeline keys and
		// barriers are unchanged from the depth-only view era.
		ImageViewCreateInfo dsVci{};
		dsVci.viewType = ImageViewType::View2D;
		dsVci.format = Format::Undefined; // inherit (combined DS format)
		dsVci.subresourceRange = {
			ImageAspect::Depth | ImageAspect::Stencil, 0, 1, 0, 1 };
		dsVci.name = d.name;

		Holder<ImageViewHandle> newDsView = ctx.CreateImageView(newImage, dsVci);
		if (newDsView.empty())
			return false;

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newDsView, d.format, d.logicalName);
		if (newLogical.empty())
			return false;

		// Stencil-aspect sampling view + bindless. Format INHERITS the
		// combined DS parent (D32S8/D24S8): a subset-aspect view of the
		// parent format — the exact shape of the depth-only view above,
		// and the only legal form without VK_IMAGE_CREATE_MUTABLE_FORMAT
		// (an explicit S8_UINT view is VUID-VkImageViewCreateInfo-image-
		// 12397: drivers accept the vkCreateImageView call but sample
		// garbage). Stencil readers consume it through the uintTextures[]
		// bindless alias (.Load — the stencil byte as uint; unfilterable).
		// Empty for stencil-less formats: those targets simply have no
		// stencil reader.
		Holder<ImageViewHandle> newStencilView;
		Holder<BindlessImageHandle> newStencilBindless;
		uint32_t newStencilBindlessIndex = UINT32_MAX;
		LogicalImageViewHandle newStencilLogical;
		if (FormatHasStencilAspect(d.format)) {
			ImageViewCreateInfo stVci{};
			stVci.viewType = ImageViewType::View2D;
			stVci.format = Format::Undefined; // inherit (combined DS format)
			stVci.subresourceRange = { ImageAspect::Stencil, 0, 1, 0, 1 };
			stVci.name = d.stencilViewName;

			newStencilView = ctx.CreateImageView(newImage, stVci);
			if (newStencilView.empty())
				return false;
			newStencilBindless = ctx.CreateBindlessImage(
				newStencilView, ImageLayout::DepthStencilReadOnlyOptimal);
			if (newStencilBindless.empty())
				return false;
			newStencilBindlessIndex = ctx.GetBindlessIndex(newStencilBindless);
			newStencilLogical = rg.AddExternalImageView(
				newStencilView, d.format, d.stencilLogicalName);
			if (newStencilLogical.empty())
				return false;
		}

		claim = std::move(newClaim);
		image = std::move(newImage);
		view = std::move(newView);
		bindless = std::move(newBindless);
		bindlessIndex = newBindlessIndex;
		dsView = std::move(newDsView);
		stencilView = std::move(newStencilView);
		stencilBindless = std::move(newStencilBindless);
		stencilBindlessIndex = newStencilBindlessIndex;
		logicalView = newLogical;
		stencilLogicalView = newStencilLogical;
		extent = targetExtent;
		format = d.format;
		return true;
	}

	void Destroy(RenderGraph& rg) {
		if (!logicalView.empty())
			rg.FreeImageView(logicalView);
		if (!stencilLogicalView.empty())
			rg.FreeImageView(stencilLogicalView);
		logicalView = {};
		stencilLogicalView = {};
		stencilBindlessIndex = UINT32_MAX;
		stencilBindless = {};
		stencilView = {};
		dsView = {};
		bindlessIndex = UINT32_MAX;
		bindless = {};
		view = {};
		// ORDER: the image Holder drops first (deferred destroy @ P),
		// THEN the claim Holder (its range free queues @ P, behind -
		// see the member-order note at the top of the struct).
		image = {};
		claim = {};
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
