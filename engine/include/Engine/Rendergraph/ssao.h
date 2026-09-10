#pragma once
// Half-res screen-space ambient occlusion (depth-only, Crytek-style).
//
// Ports the pattern used by MotionBlur / BloomChain into one place:
// resource creation (half-res raw + blurred AO targets) + default program
// ownership + rendergraph pass setup. Sampling is bindless (textures[]
// indexed by push), barriers come from SetColor (write) + SetInputImages.
//
// Pipeline (all at half res, i.e. full extent / 2):
//   raw:  depth (.Load, no sampler: D32 has no linear filtering) ->
//         view-space hemisphere test around a depth-derived normal,
//         IGN-rotated Hammersley kernel, range-checked -> R8 AO.
//         Blocks with ANY background tap (sky/clear/OOB) stay unoccluded:
//         averaging across a foreground/background discontinuity fabricates
//         a center that smears occlusion toward the sky.
//   blur: 5x5 depth-weighted (bilateral-ish) blur of the raw AO, depth
//         still .Load'ed at full res so edges don't bleed; background
//         blocks skip the gather entirely.
//   compose: FULL-res depth-guided upsample of the blurred AO (ao.hlsli
//         AoUpsample) into a full-res R8 target. The upsample runs in its
//         own pass because the depth image must only be a barriered
//         sampled input here — passes that ATTACH depth (motion vectors,
//         Main) can never also sample it (layout clash = UB).
// Composite happens in the opaque lighting pass (consumer-side): the app's
// default_lit.frag .Loads the composed AO and scales the ambient/indirect
// term; sky never runs that shader, so background can never receive AO.
//
// Depth conventions (must match app + forwardplus.cs.hlsl):
//   - infinite reversed-Z projection, clear 0 (far), GreaterEqual.
//   - view distance from NDC depth d: dist = near / max(d, eps).
//   - NDC mapping with the engine viewport (y down in framebuffer):
//       ndcX = (px / w) * 2 - 1, ndcY = (py / h) * 2 - 1.
//
// Placement: after every depth-writing pass (the Forward+ prepass fills
// the 1x depth; gfx remainder / motion vectors may follow) and BEFORE the
// opaque forward pass, so the lighting can sample the blurred AO. The 1x
// prepass depth is valid in both 1x and MSAA modes (same contract as the
// MotionVectors pass).
//
// Usage (app/src/main.cpp):
//   smvk::Ssao ssao;
//   ssao.Ensure(ctx, rg, renderExtent, desc);   // half-res targets + programs
//   if (enabled && ssao.Valid())
//     ssao.AddSsaoPasses(rg, depthView, depthBindless, frameUniformsAddr,
//                        linearSampler, desc);
//   ... ssao.OutputView() / ssao.OutputBindless() -> opaque pass AO input ...

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

struct SsaoDesc {
	Format format = Format::R8_UNorm; // single-channel AO, color-renderable
	float radius = 0.5f;              // view-space sample radius (m)
	float bias = 0.025f;              // self-occlusion bias (view units)
	float intensity = 1.0f;           // consumer-side strength (0 = off)
	float maxDistance = 25.0f;        // AO fade-out distance (m)
	uint32_t sampleCount = 16;        // hemisphere taps, 1..32
	float blurSharpness = 8.0f;       // depth edge weight (higher = tighter)
	float nearPlane = 0.01f;          // must match FrameUniforms.nearPlane
	const char* name = "ssao";
	const char* rawName = "ssaoRaw";
	const char* blurName = "ssaoBlur";
	const char* composeName = "ssaoComposed";
};

// Must match ssao.frag.hlsl SsaoPush (48 bytes).
struct SsaoPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t sampleCount = 16;
	float radius = 0.5f;
	float bias = 0.025f;
	float intensity = 1.0f; // reserved (composite lives in the opaque ambient)
	float maxDistance = 25.0f;
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f; // 1 / full-res depth height
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsaoPush) == 48);

// Must match ssao_blur.frag.hlsl SsaoBlurPush (32 bytes).
struct SsaoBlurPush {
	uint32_t aoTextureIndex = UINT32_MAX; // raw half-res AO
	uint32_t depthTextureIndex = UINT32_MAX; // full-res depth (edge stops)
	uint32_t samplerIndex = 0; // linear clamp (AO upscale taps only)
	uint32_t pad0 = 0;
	float sharpness = 8.0f;
	float nearPlane = 0.01f;
	float texelX = 0.0f; // 1 / half-res width
	float texelY = 0.0f; // 1 / half-res height
};
static_assert(sizeof(SsaoBlurPush) == 32);

// Must match ssao_compose.frag.hlsl SsaoComposePush (32 bytes).
// Upsamples the half-res blurred AO to full res with full-res depth
// authority (see ao.hlsli AoUpsample). Runs in its own pass so the depth
// image is only ever a sampled input there — passes that ATTACH depth
// (motion vectors, Main) must never also sample it bindlessly: the
// attachment/sampled layout clash is UB and reads garbage.
struct SsaoComposePush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (nearPlane)
	uint32_t aoTextureIndex = UINT32_MAX; // half-res blurred AO
	uint32_t depthTextureIndex = UINT32_MAX; // full-res depth (authority)
	float sharpness = 8.0f; // upsample edge weight
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f; // 1 / full-res depth height
	uint32_t pad0 = 0;
};
static_assert(sizeof(SsaoComposePush) == 32);

// Default SPVs (fullscreen-triangle VS shared with tonemap + SSAO PS).
inline constexpr const char* kSsaoVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kSsaoPsPath = "content/shaders/ssao.frag.hlsl.spv";
inline constexpr const char* kSsaoBlurPsPath = "content/shaders/ssao_blur.frag.hlsl.spv";
inline constexpr const char* kSsaoComposePsPath = "content/shaders/ssao_compose.frag.hlsl.spv";

struct Ssao {
	// Half-res target helper (raw + blurred AO share the layout).
	struct Target {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical{};
		[[nodiscard]] bool Valid() const { return !image.empty() && !logical.empty(); }
	};

	Target raw; // half-res raw AO (DontCare -> Store)
	Target blurred; // half-res depth-blurred AO (DontCare -> Store)
	Target composed; // FULL-res depth-guided upsample of blurred (DontCare -> Store)
	Extent2D fullExtent{ 0, 0 }; // depth/HDR extent the half-res derives from
	Extent2D halfExtent{ 0, 0 }; // max(1, full / 2) per axis
	Format format = Format::R8_UNorm;

	// Default programs (fullscreen VS + raw/blur/compose PS).
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> rawFragModule;
	Holder<ShaderModuleHandle> blurFragModule;
	Holder<ShaderModuleHandle> composeFragModule;
	ShaderProgram rawProgram{};
	ShaderProgram blurProgram{};
	ShaderProgram composeProgram{};
	SsaoDesc desc{};

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
		const char* vsPath = kSsaoVsPath,
		const char* rawPath = kSsaoPsPath,
		const char* blurPath = kSsaoBlurPsPath,
		const char* composePath = kSsaoComposePsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kSsaoVsPath;
			const std::string raw = rawPath ? rawPath : kSsaoPsPath;
			const std::string blur = blurPath ? blurPath : kSsaoBlurPsPath;
			const std::string compose = composePath ? composePath : kSsaoComposePsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "ssaoVert");
			if (rawFragModule.empty())
				rawFragModule = ctx.CreateShaderModule(readFile(raw), "ssaoRawFrag");
			if (blurFragModule.empty())
				blurFragModule = ctx.CreateShaderModule(readFile(blur), "ssaoBlurFrag");
			if (composeFragModule.empty())
				composeFragModule = ctx.CreateShaderModule(readFile(compose), "ssaoComposeFrag");
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

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const SsaoDesc& d,
		const char* vsPath = kSsaoVsPath,
		const char* rawPath = kSsaoPsPath,
		const char* blurPath = kSsaoBlurPsPath,
		const char* composePath = kSsaoComposePsPath) {
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
	// (radius / bias / samples need no realloc). Returns true when ready.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const SsaoDesc& d,
		const char* vsPath = kSsaoVsPath,
		const char* rawPath = kSsaoPsPath,
		const char* blurPath = kSsaoBlurPsPath,
		const char* composePath = kSsaoComposePsPath) {
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

	// Emits raw (depth -> half-res AO) + blur (raw -> half-res AO).
	// Captures only copyable handles/indices, so the passes are safe once
	// this returns. depthView is the 1x prepass depth (valid in 1x + MSAA).
	void AddSsaoPasses(RenderGraph& rg,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		const SsaoDesc& overrideDesc,
		const char* rawName = "Ssao",
		const char* blurName = "SsaoBlur",
		const char* composeName = "SsaoCompose") const {
		assert(Valid());
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);

		// ---- Raw: full-res depth -> half-res AO ----
		{
			SsaoPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.depthTextureIndex = depthBindlessIndex;
			push.sampleCount = std::clamp(overrideDesc.sampleCount, 1u, 32u);
			push.radius = (std::max)(0.0f, overrideDesc.radius);
			push.bias = (std::max)(0.0f, overrideDesc.bias);
			push.intensity = overrideDesc.intensity;
			push.maxDistance = (std::max)(1.0f, overrideDesc.maxDistance);
			push.invDepthW = 1.0f / static_cast<float>(std::max(1u, fullExtent.width));
			push.invDepthH = 1.0f / static_cast<float>(std::max(1u, fullExtent.height));
			push.pad0 = 0;
			push.pad1 = 0;

			RenderPass& pass = rg.AddPass(rawName ? rawName : "Ssao");
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
				cmd.PushConstants(0, sizeof(SsaoPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Blur: raw AO (+ full-res depth edges) -> blurred AO ----
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

			RenderPass& pass = rg.AddPass(blurName ? blurName : "SsaoBlur");
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
		// The depth-guided upsample (ao.hlsli AoUpsample) runs HERE, in a
		// pass that never attaches depth, so every depth read is a properly
		// barriered sampled input. Consumers (opaque ambient) then only
		// .Load the composed R8 — passes that attach depth (motion vectors,
		// Main) must never also sample it: the layout clash is UB.
		{
			SsaoComposePush push{};
			push.frameUniforms = frameUniformsAddr;
			push.aoTextureIndex = blurred.bindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.invDepthW = 1.0f / static_cast<float>(std::max(1u, fullExtent.width));
			push.invDepthH = 1.0f / static_cast<float>(std::max(1u, fullExtent.height));
			push.pad0 = 0;

			RenderPass& pass = rg.AddPass(composeName ? composeName : "SsaoCompose");
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
	void AddSsaoPasses(RenderGraph& rg,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		const char* rawName = "Ssao",
		const char* blurName = "SsaoBlur",
		const char* composeName = "SsaoCompose") const {
		AddSsaoPasses(rg, depthView, depthBindlessIndex, frameUniformsAddr,
			linearSamplerIndex, desc, rawName, blurName, composeName);
	}
};

} // namespace smvk
