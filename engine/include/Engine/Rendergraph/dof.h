// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Depth of field (low-quality single-pass + HQ bokeh gather).
//
// Ports the pattern used by MotionBlur / Ssao into one place: resource
// creation (HDR output + CoC target + bindless + rendergraph alias) +
// default program ownership + rendergraph pass setup. Sampling is bindless
// (textures[] indexed by push), barriers come from SetColor (write) +
// SetInputImages (read).
//
// Placement: after Main + MotionBlur, before Exposure/Bloom/Tonemap.
// Input is the scene HDR (possibly motion-blurred), output is a
// same-extent/format DOF HDR image. Downstream passes sample the DOF
// output when the pass ran, else the input directly (no copy disabled).
//
// Depth conventions (must match ssao.h / forwardplus.cs.hlsl):
//   - reversed-Z projection, clear 0 (far), GreaterEqual.
//   - view positions are unprojected EXACTLY via FrameUniforms.invProj
//     (flip / finite-far agnostic), never via near/d formulas.
//   - background (sky/clear, d <= 1e-6) stays sharp: no depth = no blur.
//
// Modes:
//   Low  (single pass, dof_low.frag.hlsl): per-pixel CoC radius from the
//        center depth, 12-tap Poisson gather scaled by that radius.
//        Cheap, leaks across silhouettes (background bleeds onto
//        foreground) — the documented low-quality tradeoff.
//   HQ bokeh (two passes):
//        coc   (dof_coc.frag.hlsl):  full-res R8 signed CoC
//              (0.5 = in focus, 0 = near-blur max, 1 = far-blur max).
//        bokeh (dof_bokeh.frag.hlsl): 48-tap hexagonal bokeh gather,
//              per-tap CoC + depth-aware foreground weight + luminance
//              highlight boost for disc-shaped highlights.
//
// Usage (app/src/world_renderer.cpp):
//   smvk::DepthOfField dof;
//   dof.Ensure(ctx, rg, renderExtent, desc);   // output + CoC + programs
//   if (enabled)
//     dof.AddDofPasses(rg, hdrView, hdrBindless, depthView, depthBindless,
//                      frameUniformsAddr, linearSampler, renderExtent, desc);

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

struct DofDesc {
	Format format = Format::RGBA16_Float; // must match the HDR target
	Format cocFormat = Format::R8_UNorm;  // packed signed CoC target
	bool hqBokeh = false;                // false = low single-pass, true = CoC + bokeh
	float focusDistance = 8.0f;          // in-focus plane (m, view distance)
	float focusRange = 2.0f;             // sharp half-depth around focus (m)
	float maxRadiusPx = 12.0f;           // low-pass clamp (px)
	float bokehMaxRadiusPx = 24.0f;      // HQ bokeh clamp (px)
	float highlightBoost = 1.5f;         // HQ: bright-tap weight multiplier
	float highlightThreshold = 1.0f;     // HQ: pre-exposure luminance knee for boost
	const char* name = "dof";
	const char* viewName = "dofView";
	const char* logicalName = "dofLogical";
	const char* cocName = "dofCoc";
};

// Must match dof_low.frag.hlsl DofLowPush (56 bytes).
struct DofLowPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t pad0 = 0;
	float focusDistance = 8.0f;
	float focusRange = 2.0f;
	float maxRadiusPx = 12.0f;
	float texelX = 0.0f;
	float texelY = 0.0f;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
	uint32_t pad3 = 0;
};
static_assert(sizeof(DofLowPush) == 56);

// Must match dof_coc.frag.hlsl DofCocPush (48 bytes with tail pad).
struct DofCocPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t pad0 = 0;
	float focusDistance = 8.0f;
	float focusRange = 2.0f;
	float maxRadiusPx = 24.0f;
	float invDepthW = 0.0f; // 1 / full-res depth width (kept for parity)
	float invDepthH = 0.0f;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
	uint32_t pad3 = 0; // tail pad: sizeof rounds to 8-byte struct alignment
};
static_assert(sizeof(DofCocPush) == 48);

// Must match dof_bokeh.frag.hlsl DofBokehPush (56 bytes).
struct DofBokehPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t cocTextureIndex = UINT32_MAX;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	float texelX = 0.0f;
	float texelY = 0.0f;
	float maxRadiusPx = 24.0f;
	float highlightBoost = 1.5f;
	float highlightThreshold = 1.0f;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
};
static_assert(sizeof(DofBokehPush) == 56);

// Default SPVs (fullscreen-triangle VS shared with tonemap + DOF PS).
inline constexpr const char* kDofVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kDofLowPsPath = "content/shaders/dof_low.frag.hlsl.spv";
inline constexpr const char* kDofCocPsPath = "content/shaders/dof_coc.frag.hlsl.spv";
inline constexpr const char* kDofBokehPsPath = "content/shaders/dof_bokeh.frag.hlsl.spv";

struct DepthOfField {
	struct Target {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical{};
		[[nodiscard]] bool Valid() const { return !image.empty() && !logical.empty(); }
	};

	Target output; // HDR DOF result (same extent/format as HDR)
	Target coc;    // full-res packed signed CoC (HQ only, R8)
	Extent2D extent{ 0, 0 };
	Format format = Format::RGBA16_Float;
	Format cocFormat = Format::R8_UNorm;

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> lowFragModule;
	Holder<ShaderModuleHandle> cocFragModule;
	Holder<ShaderModuleHandle> bokehFragModule;
	ShaderProgram lowProgram{};
	ShaderProgram cocProgram{};
	ShaderProgram bokehProgram{};
	DofDesc desc{};

	[[nodiscard]] bool TargetsValid() const {
		// CoC is HQ-only: low mode needs just the HDR output.
		return output.Valid() && (desc.hqBokeh ? coc.Valid() : true);
	}
	[[nodiscard]] bool ShadersValid() const {
		if (vsModule.empty())
			return false;
		if (desc.hqBokeh)
			return cocProgram.shaderCount != 0 && bokehProgram.shaderCount != 0 &&
				!cocFragModule.empty() && !bokehFragModule.empty();
		return lowProgram.shaderCount != 0 && !lowFragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetsValid() && ShadersValid(); }

	[[nodiscard]] LogicalImageViewHandle OutputView() const { return output.logical; }
	[[nodiscard]] uint32_t OutputBindless() const { return output.bindlessIndex; }

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kDofVsPath,
		const char* lowPath = kDofLowPsPath,
		const char* cocPath = kDofCocPsPath,
		const char* bokehPath = kDofBokehPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kDofVsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "dofVert");
			if (desc.hqBokeh) {
				const std::string coc = cocPath ? cocPath : kDofCocPsPath;
				const std::string bok = bokehPath ? bokehPath : kDofBokehPsPath;
				if (cocFragModule.empty())
					cocFragModule = ctx.CreateShaderModule(readFile(coc), "dofCocFrag");
				if (bokehFragModule.empty())
					bokehFragModule = ctx.CreateShaderModule(readFile(bok), "dofBokehFrag");
			} else {
				const std::string low = lowPath ? lowPath : kDofLowPsPath;
				if (lowFragModule.empty())
					lowFragModule = ctx.CreateShaderModule(readFile(low), "dofLowFrag");
			}
		} catch (...) {
			return false;
		}
		if (vsModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		if (desc.hqBokeh) {
			if (cocFragModule.empty() || bokehFragModule.empty())
				return false;
			Shader cocPS{ .module = cocFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
			Shader bokPS{ .module = bokehFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
			cocProgram = { .shaders = { vs, cocPS }, .shaderCount = 2 };
			bokehProgram = { .shaders = { vs, bokPS }, .shaderCount = 2 };
		} else {
			if (lowFragModule.empty())
				return false;
			Shader lowPS{ .module = lowFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
			lowProgram = { .shaders = { vs, lowPS }, .shaderCount = 2 };
		}
		return true;
	}

	void DestroyShaders() {
		lowProgram = ShaderProgram{};
		cocProgram = ShaderProgram{};
		bokehProgram = ShaderProgram{};
		vsModule = {};
		lowFragModule = {};
		cocFragModule = {};
		bokehFragModule = {};
	}

	static bool CreateTarget(ResourceManager& ctx, RenderGraph& rg,
		Extent2D ext, Format format, const char* name, Target& out) {
		if (ext.width == 0 || ext.height == 0)
			return false;
		ImageCreateInfo ci{};
		ci.extent = { ext.width, ext.height, 1 };
		ci.format = format;
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = name;

		Holder<ImageHandle> newImage = ctx.CreateImage(ci, nullptr);
		if (newImage.empty())
			return false;

		ImageViewCreateInfo vci{};
		vci.viewType = ImageViewType::View2D;
		vci.format = Format::Undefined; // inherit
		vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = name;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newView, format, name);
		if (newLogical.empty())
			return false;

		Target t;
		t.image = std::move(newImage);
		t.view = std::move(newView);
		t.bindless = std::move(newBindless);
		t.bindlessIndex = newBindlessIndex;
		t.logical = newLogical;
		out = std::move(t);
		ctx.GetOrCreateSampler(SamplerCreateInfo{});
		return true;
	}

	static void DestroyTarget(RenderGraph& rg, Target& t) {
		if (!t.logical.empty())
			rg.FreeImageView(t.logical);
		t = Target{};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const DofDesc& d,
		const char* vsPath = kDofVsPath,
		const char* lowPath = kDofLowPsPath,
		const char* cocPath = kDofCocPsPath,
		const char* bokehPath = kDofBokehPsPath) {
		if (full.width == 0 || full.height == 0)
			return false;
		DestroyTargets(rg);
		desc = d;
		DestroyShaders(); // mode switch (low <-> HQ) needs the other program set
		Target newOut, newCoc;
		if (!CreateTarget(ctx, rg, full, d.format, d.name, newOut))
			return false;
		if (d.hqBokeh) {
			if (!CreateTarget(ctx, rg, full, d.cocFormat, d.cocName, newCoc)) {
				DestroyTarget(rg, newOut);
				return false;
			}
		}
		output = std::move(newOut);
		coc = std::move(newCoc);
		extent = full;
		format = d.format;
		cocFormat = d.cocFormat;
		return EnsureShaders(ctx, vsPath, lowPath, cocPath, bokehPath);
	}

	void DestroyTargets(RenderGraph& rg) {
		DestroyTarget(rg, output);
		DestroyTarget(rg, coc);
		extent = { 0, 0 };
	}

	// Recreate on extent / format / mode drift. Live-updates focus params.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const DofDesc& d,
		const char* vsPath = kDofVsPath,
		const char* lowPath = kDofLowPsPath,
		const char* cocPath = kDofCocPsPath,
		const char* bokehPath = kDofBokehPsPath) {
		if (TargetsValid() &&
			extent.width == full.width && extent.height == full.height &&
			format == d.format && cocFormat == d.cocFormat &&
			desc.hqBokeh == d.hqBokeh) {
			desc = d;
			return EnsureShaders(ctx, vsPath, lowPath, cocPath, bokehPath);
		}
		return Create(ctx, rg, full, d, vsPath, lowPath, cocPath, bokehPath);
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyShaders();
	}

	// Emits low or HQ passes per desc.hqBokeh. Captures only copyable
	// handles/indices. depthView is the 1x prepass depth (valid in 1x + MSAA).
	void AddDofPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		Extent2D passExtent,
		const DofDesc& overrideDesc,
		const char* lowName = "DofLow",
		const char* cocName = "DofCoc",
		const char* bokehName = "DofBokeh") const {
		assert(Valid());
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);

		const float focusDist = (std::max)(0.05f, overrideDesc.focusDistance);
		const float focusRange = (std::max)(0.0f, overrideDesc.focusRange);

		if (!overrideDesc.hqBokeh) {
			DofLowPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.hdrTextureIndex = hdrBindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.pad0 = 0;
			push.focusDistance = focusDist;
			push.focusRange = focusRange;
			push.maxRadiusPx = (std::max)(0.0f, overrideDesc.maxRadiusPx);
			push.texelX = 1.0f / static_cast<float>(std::max(1u, passExtent.width));
			push.texelY = 1.0f / static_cast<float>(std::max(1u, passExtent.height));
			push.pad1 = 0;
			push.pad2 = 0;

			RenderPass& pass = rg.AddPass(lowName ? lowName : "DofLow");
			pass.SetOffset(0, 0)
				.SetExtent(passExtent)
				.SetInputImages({ hdrView, depthView })
				.SetColor({ output.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = lowProgram;
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexFormat::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(DofLowPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0);
			});
			return;
		}

		// ---- HQ: CoC prepass (depth -> packed signed CoC) ----
		{
			DofCocPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.depthTextureIndex = depthBindlessIndex;
			push.pad0 = 0;
			push.focusDistance = focusDist;
			push.focusRange = focusRange;
			push.maxRadiusPx = (std::max)(0.0f, overrideDesc.bokehMaxRadiusPx);
			push.invDepthW = 1.0f / static_cast<float>(std::max(1u, passExtent.width));
			push.invDepthH = 1.0f / static_cast<float>(std::max(1u, passExtent.height));
			push.pad1 = 0;

			RenderPass& pass = rg.AddPass(cocName ? cocName : "DofCoc");
			pass.SetOffset(0, 0)
				.SetExtent(passExtent)
				.SetInputImages({ depthView })
				.SetColor({ coc.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = cocProgram;
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexFormat::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(DofCocPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0);
			});
		}

		// ---- HQ: bokeh gather (HDR + CoC + depth -> HDR) ----
		{
			DofBokehPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.hdrTextureIndex = hdrBindlessIndex;
			push.cocTextureIndex = coc.bindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.texelX = 1.0f / static_cast<float>(std::max(1u, passExtent.width));
			push.texelY = 1.0f / static_cast<float>(std::max(1u, passExtent.height));
			push.maxRadiusPx = (std::max)(0.0f, overrideDesc.bokehMaxRadiusPx);
			push.highlightBoost = (std::max)(1.0f, overrideDesc.highlightBoost);
			push.highlightThreshold = (std::max)(0.0f, overrideDesc.highlightThreshold);
			push.pad0 = 0;
			push.pad1 = 0;

			RenderPass& pass = rg.AddPass(bokehName ? bokehName : "DofBokeh");
			pass.SetOffset(0, 0)
				.SetExtent(passExtent)
				.SetInputImages({ hdrView, coc.logical, depthView })
				.SetColor({ output.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = bokehProgram;
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexFormat::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(DofBokehPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0);
			});
		}
	}

	// Uses the stored desc from Ensure().
	void AddDofPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		Extent2D passExtent,
		const char* lowName = "DofLow",
		const char* cocName = "DofCoc",
		const char* bokehName = "DofBokeh") const {
		AddDofPasses(rg, hdrView, hdrBindlessIndex, depthView, depthBindlessIndex,
			frameUniformsAddr, linearSamplerIndex, passExtent, desc,
			lowName, cocName, bokehName);
	}
};

} // namespace smvk
