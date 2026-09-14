#pragma once
// Screen-space reflections (plus pass, depth/normal-reconstructed).
//
// Ports the pattern used by MotionBlur / DepthOfField into one place:
// resource creation (HDR output + bindless + rendergraph alias) + default
// program ownership + rendergraph pass setup. Sampling is bindless
// (textures[] indexed by push), barriers come from SetColor (write) +
// SetInputImages (read).
//
// Placement: after Main + MotionBlur + DOF, before Exposure/Bloom/Tonemap.
// Input is the scene HDR (post-DOF), output is the same-extent HDR with
// reflections composited additively. Downstream passes sample the SSR
// output when the pass ran, else the input directly (no copy disabled).
//
// No GBuffer exists (forward+ path writes color + depth only), so the
// pass reconstructs everything it needs from the 1x prepass depth:
//   - view positions via FrameUniforms.invProj (exact, flip/finite-far
//     agnostic — same discipline as ssao.h / dof.h),
//   - normals from depth-derived view-position derivatives,
//   - Fresnel from a uniform dielectric F0 (glass default 0.04).
// Every dielectric surface reflects with the same F0; there is no
// per-material roughness/metalness mask, so the intensity knob is the
// artistic gate. Bistro windows (large vertical glass planes at grazing
// street-view angles) are the calibration case: aim the default Bistro
// camera down the road at the shopfront glass and raise intensity until
// the opposite facades read in the panes.
//
// Raymarch: fixed-step view-space march (maxDistance / steps) with depth
// compare + thickness acceptance and a short binary refine. Misses (sky,
// off-screen, max distance) keep the base color.

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

struct SsrDesc {
	Format format = Format::RGBA16_Float; // must match the HDR target
	uint32_t steps = 32;                 // raymarch steps, 1..128
	uint32_t refineSteps = 4;            // binary refine iterations, 0..8
	float maxDistance = 50.0f;           // ray length (m)
	float thickness = 0.5f;              // depth acceptance band (m, view units)
	float intensity = 1.0f;              // reflection strength multiplier
	float f0 = 0.04f;                    // dielectric base reflectance (glass)
	const char* name = "ssr";
	const char* viewName = "ssrView";
	const char* logicalName = "ssrLogical";
};

// Must match ssr.frag.hlsl SsrPush (56 bytes).
struct SsrPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t steps = 32;
	float texelX = 0.0f;
	float texelY = 0.0f;
	float maxDistance = 50.0f;
	float thickness = 0.5f;
	float intensity = 1.0f;
	float f0 = 0.04f;
	uint32_t refineSteps = 4;
	uint32_t pad0 = 0;
};
static_assert(sizeof(SsrPush) == 56);

// Default SPVs (fullscreen-triangle VS shared with tonemap + SSR PS).
inline constexpr const char* kSsrVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kSsrPsPath = "content/shaders/ssr.frag.hlsl.spv";

struct ScreenSpaceReflections {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView{};
	Extent2D extent{ 0, 0 };
	Format format = Format::RGBA16_Float;

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fragModule;
	ShaderProgram program{};
	SsrDesc desc{};

	[[nodiscard]] bool TargetValid() const { return !image.empty() && !logicalView.empty(); }
	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetValid() && ShadersValid(); }

	[[nodiscard]] LogicalImageViewHandle OutputView() const { return logicalView; }
	[[nodiscard]] uint32_t OutputBindless() const { return bindlessIndex; }

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kSsrVsPath,
		const char* psPath = kSsrPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kSsrVsPath;
			const std::string ps = psPath ? psPath : kSsrPsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "ssrVert");
			if (fragModule.empty())
				fragModule = ctx.CreateShaderModule(readFile(ps), "ssrFrag");
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

	bool CreateTarget(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent, const SsrDesc& d) {
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

	// Idempotent output + program creation. Live-updates desc (steps /
	// distance / intensity need no realloc). Returns true when ready.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent,
		const SsrDesc& d,
		const char* vsPath = kSsrVsPath,
		const char* psPath = kSsrPsPath) {
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

	// Emits the HDR -> HDR reflection composite. Captures only copyable
	// handles/indices, so the pass is safe once this call returns.
	RenderPass& AddSsrPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		Extent2D passExtent,
		const SsrDesc& overrideDesc,
		const char* name = nullptr) const {
		assert(Valid());
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);
		assert(passExtent.width != 0 && passExtent.height != 0);

		SsrPush push{};
		push.frameUniforms = frameUniformsAddr;
		push.hdrTextureIndex = hdrBindlessIndex;
		push.depthTextureIndex = depthBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.steps = std::clamp(overrideDesc.steps, 1u, 128u);
		push.texelX = 1.0f / static_cast<float>(passExtent.width);
		push.texelY = 1.0f / static_cast<float>(passExtent.height);
		push.maxDistance = (std::max)(0.1f, overrideDesc.maxDistance);
		push.thickness = (std::max)(0.01f, overrideDesc.thickness);
		push.intensity = (std::max)(0.0f, overrideDesc.intensity);
		push.f0 = std::clamp(overrideDesc.f0, 0.0f, 1.0f);
		push.refineSteps = std::min(overrideDesc.refineSteps, 8u);
		push.pad0 = 0;

		RenderPass& pass = rg.AddPass(name ? name : "SSR");
		pass.SetOffset(0, 0)
			.SetExtent(passExtent)
			.SetInputImages({ hdrView, depthView })
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
			cmd.PushConstants(0, sizeof(SsrPush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
		return pass;
	}

	// Uses the stored desc from Ensure().
	RenderPass& AddSsrPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		Extent2D passExtent,
		const char* name = nullptr) const {
		return AddSsrPass(rg, hdrView, hdrBindlessIndex, depthView, depthBindlessIndex,
			frameUniformsAddr, linearSamplerIndex, passExtent, desc, name);
	}
};

} // namespace smvk
