#pragma once
// Half-res Scalable Ambient Obscurance (SAO, depth-only spiral-tap volume
// obscurance, after McGuire et al. "Scalable Ambient Obscurance").
//
// Shares the pipeline shape with Ssao/Gtao (see ssao.h) so the consumer path
// is identical: raw (half-res) -> depth-weighted blur (half-res) ->
// depth-guided compose (full-res, ao.hlsli AoUpsample). Only the RAW pass
// differs:
//   SSAO raw: Crytek-style view-space hemisphere sample test around a
//             depth-derived normal (ssao.frag.hlsl).
//   GTAO raw: horizon search per slice direction (gtao.frag.hlsl).
//   SAO raw:  spiral-tap volume obscurance (this header + sao.frag.hlsl):
//             each pixel reconstructs view position + normal from depth, then
//             gathers `sampleCount` taps on a golden-angle spiral whose world
//             radius grows linearly to `radius`. Each tap contributes
//             normal-weighted obscurance faded by the spherical radius shell,
//             so nearby above-surface geometry occludes and distant /
//             below-surface taps do not. Background taps (sky/clear/OOB) are
//             skipped, same invariant as SSAO/GTAO: background never occludes
//             and never receives AO.
//
// The blur + compose passes reuse the SSAO shaders/push layouts
// (ssao_blur.frag.hlsl SsaoBlurPush, ssao_compose.frag.hlsl
// SsaoComposePush) — they are AO-algorithm agnostic (R8 in, R8 out).
//
// Depth conventions, placement, and consumer contract are identical to Ssao:
//   - infinite reversed-Z, clear 0 (far), GreaterEqual.
//   - runs after all depth writers, before Main; Main's opaque ambient only
//     .Loads the composed full-res R8 (never samples depth itself).
//
// Usage mirrors Ssao:
//   smvk::Sao sao;
//   sao.Ensure(ctx, rg, renderExtent, desc);
//   if (enabled && sao.Valid())
//     sao.AddSaoPasses(rg, depthView, depthBindless, frameUniformsAddr,
//                      linearSampler, desc);

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Rendergraph/ssao.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

struct SaoDesc {
	Format format = Format::R8_UNorm; // single-channel AO, color-renderable
	float radius = 0.5f;              // view-space spiral extent (m)
	float bias = 0.025f;              // surface-offset bias (view units)
	float intensity = 1.0f;           // consumer-side strength (0 = off)
	float maxDistance = 25.0f;        // AO fade-out distance (m)
	uint32_t sampleCount = 11;        // spiral taps per pixel, 1..32
	uint32_t spiralTurns = 7;         // extra spiral twist over the radius, 1..16
	float blurSharpness = 8.0f;       // depth edge weight (higher = tighter)
	float nearPlane = 0.01f;          // must match FrameUniforms.nearPlane
	const char* name = "sao";
	const char* rawName = "saoRaw";
	const char* blurName = "saoBlur";
	const char* composeName = "saoComposed";
};

// Must match sao.frag.hlsl SaoPush (48 bytes).
struct SaoPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t sampleCount = 11;  // 1..32 spiral taps
	float radius = 0.5f;
	float bias = 0.025f;
	float intensity = 1.0f; // reserved (composite lives in the opaque ambient)
	float maxDistance = 25.0f;
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f; // 1 / full-res depth height
	uint32_t spiralTurns = 7; // 1..16 extra spiral twist
	uint32_t pad0 = 0;
};
static_assert(sizeof(SaoPush) == 48);

// Default SPVs (fullscreen-triangle VS shared with tonemap + SAO raw PS;
// blur/compose shared with the SSAO chain — same push layouts).
inline constexpr const char* kSaoVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kSaoPsPath = "content/shaders/sao.frag.hlsl.spv";
inline constexpr const char* kSaoBlurPsPath = kSsaoBlurPsPath;
inline constexpr const char* kSaoComposePsPath = kSsaoComposePsPath;

struct Sao {
	// Half-res target helper (raw + blurred AO share the layout).
	struct Target {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical{};
		[[nodiscard]] bool Valid() const { return !image.empty() && !logical.empty(); }
	};

	Target raw;      // half-res raw SAO (DontCare -> Store)
	Target blurred;  // half-res depth-blurred SAO (DontCare -> Store)
	Target composed; // FULL-res depth-guided upsample of blurred (DontCare -> Store)
	Extent2D fullExtent{ 0, 0 }; // depth/HDR extent the half-res derives from
	Extent2D halfExtent{ 0, 0 }; // max(1, full / 2) per axis
	Format format = Format::R8_UNorm;

	// Default programs (fullscreen VS + SAO raw / shared blur+compose PS).
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> rawFragModule;
	Holder<ShaderModuleHandle> blurFragModule;
	Holder<ShaderModuleHandle> composeFragModule;
	ShaderProgram rawProgram{};
	ShaderProgram blurProgram{};
	ShaderProgram composeProgram{};
	SaoDesc desc{};

	[[nodiscard]] bool TargetsValid() const {
		return raw.Valid() && blurred.Valid() && composed.Valid();
	}
	[[nodiscard]] bool ShadersValid() const {
		return rawProgram.shaderCount != 0 && blurProgram.shaderCount != 0 &&
			composeProgram.shaderCount != 0 &&
			!vsModule.empty() && !rawFragModule.empty() && !blurFragModule.empty() &&
			!composeFragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetsValid() && ShadersValid(); }

	// Consumer output: the FULL-res composed AO (safe to .Load from any
	// pass that only samples it; never requires depth).
	[[nodiscard]] LogicalImageViewHandle OutputView() const { return composed.logical; }
	[[nodiscard]] uint32_t OutputBindless() const { return composed.bindlessIndex; }

	static Extent2D HalfExtentFor(Extent2D full) {
		return {
			std::max(1u, full.width / 2),
			std::max(1u, full.height / 2),
		};
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kSaoVsPath,
		const char* rawPath = kSaoPsPath,
		const char* blurPath = kSaoBlurPsPath,
		const char* composePath = kSaoComposePsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kSaoVsPath;
			const std::string raw = rawPath ? rawPath : kSaoPsPath;
			const std::string blur = blurPath ? blurPath : kSaoBlurPsPath;
			const std::string compose = composePath ? composePath : kSaoComposePsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "saoVert");
			if (rawFragModule.empty())
				rawFragModule = ctx.CreateShaderModule(readFile(raw), "saoRawFrag");
			if (blurFragModule.empty())
				blurFragModule = ctx.CreateShaderModule(readFile(blur), "saoBlurFrag");
			if (composeFragModule.empty())
				composeFragModule = ctx.CreateShaderModule(readFile(compose), "saoComposeFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || rawFragModule.empty() || blurFragModule.empty() ||
			composeFragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader rawPS{ .module = rawFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader blurPS{ .module = blurFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader composePS{ .module = composeFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		rawProgram = { .shaders = { vs, rawPS }, .shaderCount = 2 };
		blurProgram = { .shaders = { vs, blurPS }, .shaderCount = 2 };
		composeProgram = { .shaders = { vs, composePS }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		rawProgram = ShaderProgram{};
		blurProgram = ShaderProgram{};
		composeProgram = ShaderProgram{};
		vsModule = {};
		rawFragModule = {};
		blurFragModule = {};
		composeFragModule = {};
	}

	static bool CreateTarget(ResourceManager& ctx, RenderGraph& rg,
		Extent2D half, Format format, const char* name, Target& out) {
		if (half.width == 0 || half.height == 0)
			return false;
		ImageCreateInfo ci{};
		ci.extent = { half.width, half.height, 1 };
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

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const SaoDesc& d,
		const char* vsPath = kSaoVsPath,
		const char* rawPath = kSaoPsPath,
		const char* blurPath = kSaoBlurPsPath,
		const char* composePath = kSaoComposePsPath) {
		if (full.width == 0 || full.height == 0)
			return false;
		DestroyTargets(rg);
		const Extent2D half = HalfExtentFor(full);
		Target newRaw, newBlur, newComposed;
		if (!CreateTarget(ctx, rg, half, d.format, d.rawName, newRaw))
			return false;
		if (!CreateTarget(ctx, rg, half, d.format, d.blurName, newBlur)) {
			DestroyTarget(rg, newRaw);
			return false;
		}
		if (!CreateTarget(ctx, rg, full, d.format, d.composeName, newComposed)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			return false;
		}
		raw = std::move(newRaw);
		blurred = std::move(newBlur);
		composed = std::move(newComposed);
		fullExtent = full;
		halfExtent = half;
		format = d.format;
		desc = d;
		return EnsureShaders(ctx, vsPath, rawPath, blurPath, composePath);
	}

	void DestroyTargets(RenderGraph& rg) {
		DestroyTarget(rg, raw);
		DestroyTarget(rg, blurred);
		DestroyTarget(rg, composed);
		fullExtent = { 0, 0 };
		halfExtent = { 0, 0 };
	}

	// Recreate only when full extent / format drift. Live-updates desc
	// (radius / samples / turns need no realloc). Returns true when ready.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const SaoDesc& d,
		const char* vsPath = kSaoVsPath,
		const char* rawPath = kSaoPsPath,
		const char* blurPath = kSaoBlurPsPath,
		const char* composePath = kSaoComposePsPath) {
		if (TargetsValid() &&
			fullExtent.width == full.width && fullExtent.height == full.height &&
			format == d.format) {
			desc = d;
			return EnsureShaders(ctx, vsPath, rawPath, blurPath, composePath);
		}
		return Create(ctx, rg, full, d, vsPath, rawPath, blurPath, composePath);
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyShaders();
	}

	// Emits raw (depth -> half-res SAO) + blur (raw -> half-res AO) +
	// compose (blurred half-res -> full-res AO). Captures only copyable
	// handles/indices, so the passes are safe once this returns. depthView
	// is the 1x prepass depth (valid in 1x + MSAA).
	void AddSaoPasses(RenderGraph& rg,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		const SaoDesc& overrideDesc,
		const char* rawName = "Sao",
		const char* blurName = "SaoBlur",
		const char* composeName = "SaoCompose") const {
		assert(Valid());
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);

		// ---- Raw: full-res depth -> half-res spiral volume obscurance ----
		{
			SaoPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.depthTextureIndex = depthBindlessIndex;
			push.sampleCount = std::clamp(overrideDesc.sampleCount, 1u, 32u);
			push.radius = (std::max)(0.0f, overrideDesc.radius);
			push.bias = (std::max)(0.0f, overrideDesc.bias);
			push.intensity = overrideDesc.intensity;
			push.maxDistance = (std::max)(1.0f, overrideDesc.maxDistance);
			push.invDepthW = 1.0f / static_cast<float>(std::max(1u, fullExtent.width));
			push.invDepthH = 1.0f / static_cast<float>(std::max(1u, fullExtent.height));
			push.spiralTurns = std::clamp(overrideDesc.spiralTurns, 1u, 16u);
			push.pad0 = 0;

			RenderPass& pass = rg.AddPass(rawName ? rawName : "Sao");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ depthView })
				.SetColor({ raw.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = rawProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexFormat::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SaoPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Blur: raw AO (+ full-res depth edges) -> blurred AO ----
		// Same shader/push as the SSAO chain (algorithm-agnostic).
		{
			SsaoBlurPush push{};
			push.aoTextureIndex = raw.bindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.pad0 = 0;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.nearPlane = (std::max)(1e-4f, overrideDesc.nearPlane);
			push.texelX = 1.0f / static_cast<float>(std::max(1u, halfExtent.width));
			push.texelY = 1.0f / static_cast<float>(std::max(1u, halfExtent.height));

			RenderPass& pass = rg.AddPass(blurName ? blurName : "SaoBlur");
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
				cmd.PushConstants(0, sizeof(SsaoBlurPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Compose: blurred half-res AO + full-res depth -> FULL-res AO ----
		// Same depth-guided upsample as SSAO (ao.hlsli AoUpsample): runs in
		// a pass that never attaches depth; consumers only .Load the R8.
		{
			SsaoComposePush push{};
			push.frameUniforms = frameUniformsAddr;
			push.aoTextureIndex = blurred.bindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.invDepthW = 1.0f / static_cast<float>(std::max(1u, fullExtent.width));
			push.invDepthH = 1.0f / static_cast<float>(std::max(1u, fullExtent.height));
			push.pad0 = 0;

			RenderPass& pass = rg.AddPass(composeName ? composeName : "SaoCompose");
			pass.SetOffset(0, 0)
				.SetExtent(fullExtent)
				.SetInputImages({ blurred.logical, depthView })
				.SetColor({ composed.logical },
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
				cmd.PushConstants(0, sizeof(SsaoComposePush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}
	}

	// Uses the stored desc from Ensure().
	void AddSaoPasses(RenderGraph& rg,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		const char* rawName = "Sao",
		const char* blurName = "SaoBlur",
		const char* composeName = "SaoCompose") const {
		AddSaoPasses(rg, depthView, depthBindlessIndex, frameUniformsAddr,
			linearSamplerIndex, desc, rawName, blurName, composeName);
	}
};

} // namespace smvk
