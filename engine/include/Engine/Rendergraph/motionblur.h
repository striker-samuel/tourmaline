#pragma once
// HDR motion blur (velocity gather along motion vectors, pre-tonemap).
//
// Ports the pattern used by BloomChain / Tonemapper into one place:
// resource creation (output image + view + bindless + rendergraph alias) +
// default program ownership + rendergraph pass setup. Sampling is bindless
// (textures[] indexed by push), barriers come from SetColor (write) +
// SetInputImages (read).
//
// Placement: after Main, before Bloom/Tonemap. Input is the HDR scene
// target, output is a same-extent/format blurred HDR image. Bloom and the
// tonemapper sample the blurred output when the pass ran, else the sharp
// HDR directly (no copy when disabled).
//
// Blur: per-pixel gather along the motion vector (prevUV - currUV from the
// MotionVectors pass, RG16F). The vector is scaled (shutter strength),
// clamped to maxRadiusPx, then sampleCount box taps centered on the pixel
// (t in [-0.5, +0.5]). Sub-pixel motion (< 0.5px) early-outs to the center
// tap. No depth awareness (background bleeds onto foreground silhouettes);
// a tile-dilate / depth-weighted gather is the upgrade path.
//
// Usage (app/src/main.cpp):
//   smvk::MotionBlur motionBlur;
//   motionBlur.Ensure(ctx, rg, renderExtent, desc);   // output + programs
//   if (enabled)
//     motionBlur.AddMotionBlurPass(rg, hdrView, hdrBindless,
//                                  motionView, motionBindless,
//                                  linearSampler, renderExtent, desc);
//   ... motionBlur.OutputView() / motionBlur.OutputBindless() ...

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

struct MotionBlurDesc {
	Format format = Format::RGBA16_Float; // must match the HDR target
	uint32_t sampleCount = 12;            // gather taps, 1..32 (1 = copy)
	float maxRadiusPx = 24.0f;            // per-pixel blur clamp (px)
	float velocityScale = 1.0f;           // shutter strength on motion vectors
	const char* name = "motionBlur";
	const char* viewName = "motionBlurView";
	const char* logicalName = "motionBlurLogical";
};

// Must match motionblur.frag.hlsl MotionBlurPush (32 bytes).
struct MotionBlurPush {
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t motionTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t sampleCount = 12;
	float maxRadiusPx = 24.0f;
	float velocityScale = 1.0f;
	float texelX = 0.0f;
	float texelY = 0.0f;
};
static_assert(sizeof(MotionBlurPush) == 32);

// Default SPVs (fullscreen-triangle VS shared with tonemap + blur PS).
inline constexpr const char* kMotionBlurVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kMotionBlurPsPath = "content/shaders/motionblur.frag.hlsl.spv";

struct MotionBlur {
	// Blurred HDR output (same extent/format as the HDR target).
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView{};
	Extent2D extent{ 0, 0 };
	Format format = Format::RGBA16_Float;

	// Default program (tonemap.vert mainVS + motionblur.frag mainPS).
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fragModule;
	ShaderProgram program{};
	MotionBlurDesc desc{};

	[[nodiscard]] bool TargetValid() const { return !image.empty() && !logicalView.empty(); }
	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetValid() && ShadersValid(); }

	[[nodiscard]] LogicalImageViewHandle OutputView() const { return logicalView; }
	[[nodiscard]] uint32_t OutputBindless() const { return bindlessIndex; }

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kMotionBlurVsPath,
		const char* psPath = kMotionBlurPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kMotionBlurVsPath;
			const std::string ps = psPath ? psPath : kMotionBlurPsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "motionBlurVert");
			if (fragModule.empty())
				fragModule = ctx.CreateShaderModule(readFile(ps), "motionBlurFrag");
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

	bool CreateTarget(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const MotionBlurDesc& d) {
		if (targetExtent.width == 0 || targetExtent.height == 0)
			return false;
		DestroyTarget(rg);

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

	void DestroyTarget(RenderGraph& rg) {
		if (!logicalView.empty())
			rg.FreeImageView(logicalView);
		logicalView = {};
		bindlessIndex = UINT32_MAX;
		bindless = {};
		view = {};
		image = {};
		extent = { 0, 0 };
	}

	// Idempotent output + program creation. Live-updates desc (sample
	// count / radius / strength need no realloc). Returns true when ready.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent,
		const MotionBlurDesc& d,
		const char* vsPath = kMotionBlurVsPath,
		const char* psPath = kMotionBlurPsPath) {
		desc = d;
		bool ok = true;
		if (!(TargetValid() &&
				extent.width == targetExtent.width && extent.height == targetExtent.height &&
				format == d.format)) {
			ok = CreateTarget(ctx, rg, targetExtent, d);
		}
		ok = EnsureShaders(ctx, vsPath, psPath) && ok;
		return ok;
	}

	void Destroy(RenderGraph& rg) {
		DestroyTarget(rg);
		DestroyShaders();
	}

	// Emits the HDR -> blurred-HDR fullscreen gather. Captures only
	// copyable handles/indices, so the pass is safe once this returns.
	RenderPass& AddMotionBlurPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle motionView,
		uint32_t motionBindlessIndex,
		uint32_t linearSamplerIndex,
		Extent2D passExtent,
		const MotionBlurDesc& overrideDesc,
		const char* name = nullptr) const {
		assert(Valid());
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(!motionView.empty() && motionBindlessIndex != UINT32_MAX);
		assert(passExtent.width != 0 && passExtent.height != 0);

		MotionBlurPush push{};
		push.hdrTextureIndex = hdrBindlessIndex;
		push.motionTextureIndex = motionBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.sampleCount = std::clamp(overrideDesc.sampleCount, 1u, 32u);
		push.maxRadiusPx = (std::max)(0.0f, overrideDesc.maxRadiusPx);
		push.velocityScale = overrideDesc.velocityScale;
		push.texelX = 1.0f / static_cast<float>(passExtent.width);
		push.texelY = 1.0f / static_cast<float>(passExtent.height);

		RenderPass& pass = rg.AddPass(name ? name : "MotionBlur");
		pass.SetOffset(0, 0)
			.SetExtent(passExtent)
			.SetInputImages({ hdrView, motionView })
			.SetColor({ logicalView },
				AttachmentLoadOp::DontCare,
				AttachmentStoreOp::Store);
		ShaderProgram progCopy = program; // ShaderProgram is copyable (handles only)
		pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindRenderPipeline(
				progCopy,
				VertexFormat::Empty,
				DepthSettings::Disabled(),
				StencilSettings::DefaultDisabled(),
				BlendSettings::Opaque(),
				MSAASettings::Count1(),
				RasterSettings{ .cullMode = CullMode::None });
			cmd.PushConstants(0, sizeof(MotionBlurPush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
		return pass;
	}

	// Uses the stored desc from Ensure().
	RenderPass& AddMotionBlurPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle motionView,
		uint32_t motionBindlessIndex,
		uint32_t linearSamplerIndex,
		Extent2D passExtent,
		const char* name = nullptr) const {
		return AddMotionBlurPass(rg, hdrView, hdrBindlessIndex,
			motionView, motionBindlessIndex, linearSamplerIndex,
			passExtent, desc, name);
	}
};

} // namespace smvk
