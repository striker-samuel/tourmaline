#pragma once
// Screen-space reflections: half-res raymarch + depth-aware bilateral
// blur + depth-aware full-res composite.
//
// Ports the SSAO raw/blur/compose discipline into one place:
// resource creation (half-res raw + blurred reflection targets, full-res
// composite) + default program ownership + rendergraph pass setup.
// Sampling is bindless (textures[] indexed by push), barriers come from
// SetColor (write) + SetInputImages (read).
//
// Pipeline (trace + blur at half res, i.e. full extent / 2):
//   trace:     full-res depth 2x2 block -> nearest valid tap is the
//              receiver (reverse-Z: max depth = nearest; never fabricate
//              a center across a silhouette) -> depth-derived normal
//              from full-res neighbors one half-step over (same-depth
//              fallback is sign-safe by construction) -> view-space
//              march with thickness acceptance + binary refine.
//              Output RGBA16F: rgb = reflected HDR x Fresnel x intensity
//              (unfaded), a = edge x distance confidence (1 on the sky
//              fallback). Rays that hit nothing sample the analytic sky
//              specular IBL along the world-space R instead of black, so
//              isolated mirrors (chrome probe balls, open atriums) read
//              as chrome; honors the IBL toggle.
//   blur:      5x5 bilateral blur of (rgb, a) at half res, depth-weighted
//              with exact view distances (flip / finite-far agnostic via
//              invProj, same discipline as dof.hlsli) so edges don't bleed.
//   composite: FULL-res base HDR + depth-guided 2x2 upsample of the
//              blurred (rgb, a) with full-res depth authority. Background
//              pixels keep the base color unconditionally.
// Placement: after Main + MotionBlur + DOF, before Exposure/Bloom/Tonemap.
// Input is the scene HDR (post-DOF), output is the same-extent HDR with
// reflections composited additively. Downstream passes sample the SSR
// output when the pass ran, else the input directly (no copy disabled).
//
// No GBuffer exists (forward+ path writes color + depth only), so the
// trace reconstructs everything it needs from the 1x prepass depth:
//   - view positions via FrameUniforms.invProj (exact, flip/finite-far
//     agnostic — same discipline as ssao.h / dof.h),
//   - normals from depth-derived view-position derivatives,
//   - Fresnel from a uniform dielectric F0 (glass default 0.04).
// Every dielectric surface reflects with the same F0; there is no
// per-material roughness/metalness mask, so the intensity knob is the
// artistic gate. Bistro windows (large vertical glass planes at grazing
// street-view angles, double-sided) are the calibration case: aim the
// default Bistro camera down the road at the shopfront glass and raise
// intensity until the opposite facades read in the panes. reflect() is
// invariant under N -> -N, so the viewer-facing normal flip is safe for
// double-sided glass by construction.
//
// Coordinate discipline (matches ssao.frag.hlsl exactly, so a Y-flip /
// reverse-Z audit only has to happen once):
//   - framebuffer y-down NDC: ndc = px * invDepth * 2 - 1 (top = -1),
//     because the main view bakes proj[1][1] *= -1 for Vulkan.
//   - NDC -> UV: uv = ndc * 0.5 + 0.5 (top-left = (0,0)).
//   - reverse-Z: depth 1 = near, 0 = far/sky (clear 0); background is
//     d <= 1e-6; view distance comes only from invProj unprojection.

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
	float thickness = 0.3f;              // depth acceptance band (m, view units)
	float intensity = 1.0f;              // reflection strength multiplier
	float f0 = 0.04f;                    // dielectric base reflectance (glass)
	float blurSharpness = 10.0f;         // bilateral depth edge weight (higher = tighter)
	const char* name = "ssr";
	const char* viewName = "ssrView";
	const char* logicalName = "ssrLogical";
};

// Must match ssr.frag.hlsl SsrTracePush (56 bytes).
struct SsrTracePush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t steps = 32;
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f; // 1 / full-res depth height
	float maxDistance = 50.0f;
	float thickness = 0.3f;
	float intensity = 1.0f;
	float f0 = 0.04f;
	uint32_t refineSteps = 4;
	uint32_t pad0 = 0;
};
static_assert(sizeof(SsrTracePush) == 56);

// Must match ssr_blur.frag.hlsl SsrBlurPush (48 bytes).
struct SsrBlurPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (exact view distances)
	uint32_t reflTextureIndex = UINT32_MAX; // half-res raw reflection
	uint32_t depthTextureIndex = UINT32_MAX; // full-res depth (edge stops)
	uint32_t samplerIndex = 0; // linear clamp (reflection taps only; depth uses Load)
	uint32_t pad0 = 0;
	float sharpness = 10.0f;
	float texelX = 0.0f; // 1 / half-res width
	float texelY = 0.0f; // 1 / half-res height
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f; // 1 / full-res depth height
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsrBlurPush) == 48);

// Must match ssr_compose.frag.hlsl SsrComposePush (48 bytes).
struct SsrComposePush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (exact view distances)
	uint32_t blurTextureIndex = UINT32_MAX; // half-res blurred reflection
	uint32_t depthTextureIndex = UINT32_MAX; // full-res depth (authority)
	uint32_t hdrTextureIndex = UINT32_MAX; // full-res base HDR
	uint32_t samplerIndex = 0; // linear clamp (base HDR tap)
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f; // 1 / full-res depth height
	float sharpness = 10.0f; // upsample edge weight
	float pad0 = 0.0f;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
};
static_assert(sizeof(SsrComposePush) == 48);

// Default SPVs (fullscreen-triangle VS shared with tonemap + SSR PS trio).
inline constexpr const char* kSsrVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kSsrTracePsPath = "content/shaders/ssr.frag.hlsl.spv";
inline constexpr const char* kSsrBlurPsPath = "content/shaders/ssr_blur.frag.hlsl.spv";
inline constexpr const char* kSsrComposePsPath = "content/shaders/ssr_compose.frag.hlsl.spv";

struct ScreenSpaceReflections {
	// Half-res target helper (raw + blurred reflection share the layout).
	struct Target {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical{};
		[[nodiscard]] bool Valid() const { return !image.empty() && !logical.empty(); }
	};

	Target raw; // half-res trace output (rgb = refl x fresnel x intensity, a = confidence)
	Target blurred; // half-res bilateral blur of raw
	// Full-res composite output (base HDR + upsampled reflection). These
	// keep the historic field names so downstream code (DLSS color input,
	// status UI) is untouched.
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView{};
	Extent2D extent{ 0, 0 }; // full-res composite extent (= depth/HDR extent)
	Extent2D halfExtent{ 0, 0 }; // max(1, full / 2) per axis
	Format format = Format::RGBA16_Float;

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> traceFragModule;
	Holder<ShaderModuleHandle> blurFragModule;
	Holder<ShaderModuleHandle> composeFragModule;
	ShaderProgram traceProgram{};
	ShaderProgram blurProgram{};
	ShaderProgram composeProgram{};
	SsrDesc desc{};

	[[nodiscard]] bool TargetsValid() const {
		return raw.Valid() && blurred.Valid() && !image.empty() && !logicalView.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return traceProgram.shaderCount != 0 && blurProgram.shaderCount != 0 &&
			composeProgram.shaderCount != 0 &&
			!vsModule.empty() && !traceFragModule.empty() && !blurFragModule.empty() &&
			!composeFragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetsValid() && ShadersValid(); }

	[[nodiscard]] LogicalImageViewHandle OutputView() const { return logicalView; }
	[[nodiscard]] uint32_t OutputBindless() const { return bindlessIndex; }

	static Extent2D HalfExtentFor(Extent2D full) {
		return {
			std::max(1u, full.width / 2),
			std::max(1u, full.height / 2),
		};
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kSsrVsPath,
		const char* tracePath = kSsrTracePsPath,
		const char* blurPath = kSsrBlurPsPath,
		const char* composePath = kSsrComposePsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kSsrVsPath;
			const std::string trace = tracePath ? tracePath : kSsrTracePsPath;
			const std::string blur = blurPath ? blurPath : kSsrBlurPsPath;
			const std::string compose = composePath ? composePath : kSsrComposePsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "ssrVert");
			if (traceFragModule.empty())
				traceFragModule = ctx.CreateShaderModule(readFile(trace), "ssrTraceFrag");
			if (blurFragModule.empty())
				blurFragModule = ctx.CreateShaderModule(readFile(blur), "ssrBlurFrag");
			if (composeFragModule.empty())
				composeFragModule = ctx.CreateShaderModule(readFile(compose), "ssrComposeFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || traceFragModule.empty() || blurFragModule.empty() ||
			composeFragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader tracePS{ .module = traceFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader blurPS{ .module = blurFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader composePS{ .module = composeFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		traceProgram = { .shaders = { vs, tracePS }, .shaderCount = 2 };
		blurProgram = { .shaders = { vs, blurPS }, .shaderCount = 2 };
		composeProgram = { .shaders = { vs, composePS }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		traceProgram = ShaderProgram{};
		blurProgram = ShaderProgram{};
		composeProgram = ShaderProgram{};
		vsModule = {};
		traceFragModule = {};
		blurFragModule = {};
		composeFragModule = {};
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

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const SsrDesc& d,
		const char* vsPath = kSsrVsPath,
		const char* tracePath = kSsrTracePsPath,
		const char* blurPath = kSsrBlurPsPath,
		const char* composePath = kSsrComposePsPath) {
		if (full.width == 0 || full.height == 0)
			return false;
		DestroyTargets(rg);
		const Extent2D half = HalfExtentFor(full);
		Target newRaw, newBlur;
		if (!CreateTarget(ctx, rg, half, d.format, "ssrRaw", newRaw))
			return false;
		if (!CreateTarget(ctx, rg, half, d.format, "ssrBlur", newBlur)) {
			DestroyTarget(rg, newRaw);
			return false;
		}
		if (!CreateTarget(ctx, rg, full, d)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			return false;
		}
		raw = std::move(newRaw);
		blurred = std::move(newBlur);
		halfExtent = half;
		format = d.format;
		desc = d;
		return EnsureShaders(ctx, vsPath, tracePath, blurPath, composePath);
	}

	void DestroyTargets(RenderGraph& rg) {
		DestroyTarget(rg, raw);
		DestroyTarget(rg, blurred);
		DestroyTarget(rg);
		halfExtent = { 0, 0 };
	}

	// Recreate only when full extent / format drift. Live-updates desc
	// (steps / distance / intensity need no realloc). Returns true when ready.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent,
		const SsrDesc& d,
		const char* vsPath = kSsrVsPath,
		const char* tracePath = kSsrTracePsPath,
		const char* blurPath = kSsrBlurPsPath,
		const char* composePath = kSsrComposePsPath) {
		if (TargetsValid() &&
				extent.width == targetExtent.width && extent.height == targetExtent.height &&
				format == d.format) {
			desc = d;
			return EnsureShaders(ctx, vsPath, tracePath, blurPath, composePath);
		}
		return Create(ctx, rg, targetExtent, d, vsPath, tracePath, blurPath, composePath);
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyShaders();
	}

	// Emits trace (HDR + full-res depth -> half-res reflection) + blur
	// (half-res reflection + full-res depth -> half-res) + composite
	// (half-res reflection + full-res depth + full-res HDR -> full-res HDR).
	// Captures only copyable handles/indices, so the passes are safe once
	// this call returns.
	void AddSsrPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		const SsrDesc& overrideDesc,
		const char* traceName = "SSR",
		const char* blurName = "SSRBlur",
		const char* composeName = "SSRComposite") const {
		assert(Valid());
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);
		assert(extent.width != 0 && extent.height != 0);
		assert(halfExtent.width != 0 && halfExtent.height != 0);

		const float fullW = static_cast<float>(std::max(1u, extent.width));
		const float fullH = static_cast<float>(std::max(1u, extent.height));
		const float halfW = static_cast<float>(std::max(1u, halfExtent.width));
		const float halfH = static_cast<float>(std::max(1u, halfExtent.height));

		// ---- Trace: HDR + full-res depth -> half-res reflection ----
		{
			SsrTracePush push{};
			push.frameUniforms = frameUniformsAddr;
			push.hdrTextureIndex = hdrBindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.steps = std::clamp(overrideDesc.steps, 1u, 128u);
			push.invDepthW = 1.0f / fullW;
			push.invDepthH = 1.0f / fullH;
			push.maxDistance = (std::max)(0.1f, overrideDesc.maxDistance);
			push.thickness = (std::max)(0.01f, overrideDesc.thickness);
			push.intensity = (std::max)(0.0f, overrideDesc.intensity);
			push.f0 = std::clamp(overrideDesc.f0, 0.0f, 1.0f);
			push.refineSteps = std::min(overrideDesc.refineSteps, 8u);
			push.pad0 = 0;

			RenderPass& pass = rg.AddPass(traceName ? traceName : "SSR");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ hdrView, depthView })
				.SetColor({ raw.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = traceProgram; // ShaderProgram is copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexFormat::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsrTracePush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Blur: raw reflection (+ full-res depth edges) -> blurred ----
		{
			SsrBlurPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.reflTextureIndex = raw.bindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.pad0 = 0;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.texelX = 1.0f / halfW;
			push.texelY = 1.0f / halfH;
			push.invDepthW = 1.0f / fullW;
			push.invDepthH = 1.0f / fullH;
			push.pad1 = 0;

			RenderPass& pass = rg.AddPass(blurName ? blurName : "SSRBlur");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ raw.logical, depthView })
				.SetColor({ blurred.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = blurProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexFormat::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsrBlurPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Composite: blurred half-res + full-res depth + HDR -> HDR ----
		// The depth-guided upsample runs HERE, in a pass that never
		// attaches depth, so every depth read is a properly barriered
		// sampled input.
		{
			SsrComposePush push{};
			push.frameUniforms = frameUniformsAddr;
			push.blurTextureIndex = blurred.bindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.hdrTextureIndex = hdrBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.invDepthW = 1.0f / fullW;
			push.invDepthH = 1.0f / fullH;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.pad0 = 0.0f;
			push.pad1 = 0;
			push.pad2 = 0;

			RenderPass& pass = rg.AddPass(composeName ? composeName : "SSRComposite");
			pass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages({ blurred.logical, depthView, hdrView })
				.SetColor({ logicalView },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = composeProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexFormat::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsrComposePush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}
	}

	// Uses the stored desc from Ensure().
	void AddSsrPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		const char* traceName = "SSR",
		const char* blurName = "SSRBlur",
		const char* composeName = "SSRComposite") const {
		AddSsrPasses(rg, hdrView, hdrBindlessIndex, depthView, depthBindlessIndex,
			frameUniformsAddr, linearSamplerIndex, desc, traceName, blurName, composeName);
	}
};

} // namespace smvk
