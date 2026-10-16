// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Separated UI pass (internal UI resolution).
//
// UiTarget: offscreen color target ImGui renders into (cleared transparent),
// sized independently of both the scene (render scale) and the swapchain
// (ui scale). Format is the UNORM sibling of the swapchain surface format:
// ImGui authors sRGB-encoded colors and fonts, which must be stored raw
// (an _SRGB target would double-encode them on store); the composite pass
// decodes for the sRGB swapchain (see UiCompositePush::decodeOutput).
//
// UiComposite: fullscreen-triangle pass that samples the UI target and
// alpha-blends it over the tonemapped swapchain (srcAlpha / oneMinusSrcAlpha).
// Barriers come from SetColor (write) + SetInputImages (read).
//
// Usage (app/src/main.cpp):
//   smvk::UiTarget ui;              // ui.Ensure(ctx, rg, uiExtent, uiDesc)
//   smvk::UiComposite uiComposite;  // uiComposite.Ensure(ctx)
//   imgui->NewFrame(ui.extent);     // io.DisplayFramebufferScale = target
//   imgui->AddPass(rg, ui.logicalView, ui.extent, Clear, {0,0,0,0});
//   uiComposite.AddCompositePass(rg, ui.logicalView, ui.bindlessIndex,
//                                linearSampler, swapViews, swapExtent,
//                                swapIsSrgb);

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

struct UiDesc {
	Format format = Format::BGRA8_UNorm; // UNORM sibling of the swapchain surface format
	const char* name = "ui";
	const char* viewName = "uiView";
	const char* logicalName = "uiLogical";
};

struct UiTarget {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView;
	Extent2D extent{ 0, 0 };
	Format format = Format::BGRA8_UNorm;

	[[nodiscard]] bool Valid() const { return !image.empty(); }

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const UiDesc& d = {}) {
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

	// Recreate only when extent / format drift (ui scale changes, resize).
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const UiDesc& d = {}) {
		if (Valid() &&
			extent.width == targetExtent.width && extent.height == targetExtent.height &&
			format == d.format) {
			return true;
		}
		return Create(ctx, rg, targetExtent, d);
	}
};

// Must match ui_composite.frag.hlsl UiCompositePush (16 bytes).
struct UiCompositePush {
	uint32_t uiTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t decodeOutput = 0; // 1 = decode to linear for the sRGB swapchain store
	uint32_t pad1 = 0;
};
static_assert(sizeof(UiCompositePush) == 16);

// Default SPVs (fullscreen-triangle VS shared with tonemap/bloom + composite PS).
inline constexpr const char* kUiCompositeVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kUiCompositePsPath = "content/shaders/ui_composite.frag.hlsl.spv";

struct UiComposite {
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fragModule;
	ShaderProgram program{};

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kUiCompositeVsPath,
		const char* psPath = kUiCompositePsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kUiCompositeVsPath;
			const std::string ps = psPath ? psPath : kUiCompositePsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "uiCompositeVert");
			if (fragModule.empty())
				fragModule = ctx.CreateShaderModule(readFile(ps), "uiCompositeFrag");
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

	bool Ensure(ResourceManager& ctx,
		const char* vsPath = kUiCompositeVsPath,
		const char* psPath = kUiCompositePsPath) {
		return EnsureShaders(ctx, vsPath, psPath);
	}

	// Emits the UI target -> swapchain alpha-blend pass. Loads the
	// tonemapped scene (written by the previous swap write) and composites
	// the UI on top. decodeOutput: set when the swapchain surface format is
	// an _SRGB variant - the shader decodes the sampled (encoded) UI bytes
	// to linear so the hardware store encode restores them; with a
	// non-sRGB surface they pass through raw. Captures only copyable
	// handles/indices.
	// Layer sub-rect (see Tonemapper::AddTonemapPass): offsetX/Y confine
	// the composite to the element's rect. Load is unconditional here so
	// sub-rect layers always preserve siblings.
	RenderPass& AddCompositePass(RenderGraph& rg,
		LogicalImageViewHandle uiView,
		uint32_t uiBindlessIndex,
		uint32_t linearSamplerIndex,
		const std::vector<LogicalImageViewHandle>& swapViews,
		Extent2D swapExtent,
		bool decodeOutput = false,
		const char* name = nullptr,
		uint32_t offsetX = 0,
		uint32_t offsetY = 0) const {
		assert(ShadersValid());
		assert(!uiView.empty());
		assert(!swapViews.empty());

		UiCompositePush push{};
		push.uiTextureIndex = uiBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.decodeOutput = decodeOutput ? 1u : 0u;

		RenderPass& pass = rg.AddPass(name ? name : "UiComposite");
		pass.SetOffset(offsetX, offsetY)
			.SetExtent(swapExtent)
			.SetInputImages({ uiView })
			.SetColor(swapViews,
				AttachmentLoadOp::Load,
				AttachmentStoreOp::Store,
				ClearColor(0, 0, 0, 1));
		ShaderProgram progCopy = program; // ShaderProgram is copyable (handles only)
		pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindRenderPipeline(
				progCopy,
				VertexFormat::Empty,
				DepthSettings::Disabled(),
				StencilSettings::DefaultDisabled(),
				BlendSettings::AlphaBlend(), // src.rgb*src.a + dst.rgb*(1-src.a)
				MSAASettings::Count1(),
				RasterSettings{ .cullMode = CullMode::None });
			cmd.PushConstants(0, sizeof(UiCompositePush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
		return pass;
	}
};

} // namespace smvk
