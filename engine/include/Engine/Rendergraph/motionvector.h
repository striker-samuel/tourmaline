#pragma once
// Motion vector target + pass (per-pixel prevUV - currUV, RG16F).
//
// Ports the HdrTarget (target lifecycle) + Tonemapper (program ownership)
// patterns into one place. The pass re-rasterizes opaque scene geometry
// depth-tested READ-ONLY against the 1x depth-prepass buffer (reversed-Z
// GreaterEqual, no writes), so it works identically in MSAA and non-MSAA
// modes – Main's transient 2x depth is never touched.
//
// Caller sets the recording func (bind program + draw + per-draw push
// constants: model + prevModel), same contract as AddCascadePass.
//
// Usage (app/src/main.cpp):
//   smvk::MotionVectors motionVectors;
//   motionVectors.Ensure(ctx, rg, renderExtent);   // target + programs
//   motionVectors.AddMotionVectorPass(rg, depthView, extent, "MotionVectors")
//       .SetRecordingFunc(...);
//   ... motionVectors.target.logicalView / .bindlessIndex ...

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

struct MotionVectorDesc {
	Format format = Format::RG16_Float; // 2x f16 UV delta, plenty for |v| < 1
	const char* name = "motion";
	const char* viewName = "motionView";
	const char* logicalName = "motionLogical";
};

struct MotionVectorTarget {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView;
	Extent2D extent{ 0, 0 };
	Format format = Format::RG16_Float;

	[[nodiscard]] bool Valid() const { return !image.empty(); }

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const MotionVectorDesc& d = {}) {
		if (targetExtent.width == 0 || targetExtent.height == 0)
			return false;
		Destroy(rg);

		ImageCreateInfo ci{};
		ci.extent = { targetExtent.width, targetExtent.height, 1 };
		ci.format = d.format;
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
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
		extent = targetExtent;
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

	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const MotionVectorDesc& d = {}) {
		if (Valid() &&
			extent.width == targetExtent.width && extent.height == targetExtent.height &&
			format == d.format) {
			return true;
		}
		return Create(ctx, rg, targetExtent, d);
	}
};

// Default SPVs (StaticVertexFormat VS + motion PS).
inline constexpr const char* kMotionVectorVsPath = "content/shaders/motionvector.vert.hlsl.spv";
inline constexpr const char* kMotionVectorPsPath = "content/shaders/motionvector.frag.hlsl.spv";

struct MotionVectors {
	MotionVectorTarget target;
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fragModule;
	ShaderProgram program{};

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
	}

	[[nodiscard]] bool Valid() const {
		return target.Valid() && ShadersValid();
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kMotionVectorVsPath,
		const char* psPath = kMotionVectorPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kMotionVectorVsPath;
			const std::string ps = psPath ? psPath : kMotionVectorPsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "motionVectorVert");
			if (fragModule.empty())
				fragModule = ctx.CreateShaderModule(readFile(ps), "motionVectorFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || fragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader ps{ .module = fragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		program = { .shaders = { vs, ps }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		program = ShaderProgram{};
		vsModule = {};
		fragModule = {};
	}

	// Idempotent target + program creation. Returns true when ready.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent,
		const MotionVectorDesc& d = {}) {
		bool ok = target.Ensure(ctx, rg, targetExtent, d);
		ok = EnsureShaders(ctx) && ok;
		return ok;
	}

	void Destroy(RenderGraph& rg) {
		target.Destroy(rg);
		DestroyShaders();
	}

	// Emits the motion pass: RG16F color (clear 0) + 1x depth loaded
	// read-only (prepass filled it) and STORED (Main, SSAO, the tile
	// cull and deferred shadows all consume it downstream — DontCare
	// here discards it out from under them: fullscreen corruption with
	// Main falling back to draw-order (far wall covers the hall) plus
	// depth-fighting speckles). Caller records draws.
	RenderPass& AddMotionVectorPass(RenderGraph& rg,
		LogicalImageViewHandle depthView,
		LogicalImageViewHandle motionView,
		Extent2D passExtent,
		const char* name = nullptr) const {
		assert(ShadersValid());
		assert(!depthView.empty());
		assert(!motionView.empty());

		RenderPass& pass = rg.AddPass(name ? name : "MotionVectors");
		std::vector<LogicalImageViewHandle> colors;
		colors.push_back(motionView);
		pass.SetOffset(0, 0)
			.SetExtent(passExtent)
			.SetColor(colors,
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::Store,
				ClearColor(0, 0, 0, 0))
			.SetDepth(depthView,
				AttachmentLoadOp::Load,
				AttachmentStoreOp::Store,
				ClearDepthStencil(0, 0));
		return pass;
	}
};

} // namespace smvk
