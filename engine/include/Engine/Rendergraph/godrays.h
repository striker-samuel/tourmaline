// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Screen-space god rays (the Dishonored-2-style sun occlusion +
// radial-scattering post chain - an ADDITIONAL, view-dependent effect
// on top of the CSM fog shadowing in volumetric.frag.hlsl, never a
// replacement: the CSM leg occludes the march's sun term per segment;
// this chain adds a separate scattering approximation that reads ONLY
// the scene depth, so it also runs under EVSM where the CSM fog gate
// closes - the fog sun-occlusion FALLBACK in that mode).
//
// The chain (all fullscreen-triangle FRAGMENT passes - the engine's
// post shape; the plan's compute forms carried no workgroup structure
// to exploit):
//   mask (quarter res): the sun's GLOW region where the depth is SKY -
//      geometry in front of the sun blanks it, and the radial gather
//      through those blanks is what makes the shafts.
//   radial (1/16): per pixel, a decayed 32-sample march toward the
//      projected sun through the mask (IGN start dither - the ring
//      breaker), normalized by the sample count.
//   filter (1/16): a tight 5x5 {6,4,1} Gaussian - knock the march's
//      residual noise down, NOT the shaft structure (aggressive
//      filtering here erases the thin beams the effect exists for).
//   composite (full res): base + rays * sunHdr * intensity *
//      fogFront, the fog in front of the visible depth
//      (1 - exp(-authoredDensity * viewDist), clamped at the fog max
//      distance) so foreground geometry shortens the contributing fog
//      interval. ADDITIVE - multiplying the CSM visibility by this
//      would darken the fog twice (both are sun-occlusion forms).
//
// Placement: immediately AFTER the volumetric composite re-points the
// scene HDR and BEFORE the HDR-tail overlays + exposure - the rays
// participate in bloom/tonemap exactly like the fog beneath them.
//
// Toggles: independent of the CSM fog-shadow toggle (the A/B matrix:
// CSM only / screen-space only / combined). Debug modes
// (TOURMALINE_GODRAYS_DEBUG, env-only - the FOG_GI_DEBUG discipline):
// 1 = the raw mask, 2 = the unfiltered radial rays, 3 = the filtered
// fog-shadow field, 4 = the final contribution term; each replaces the
// composite (the view IS the diagnostic).

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

namespace smvk {

// ---- push mirrors (keep the HLSL twins in lockstep; static_asserts) ----

// Must match godrays_mask.frag.hlsl GodraysMaskPush (32 bytes).
struct GodraysMaskPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t depthTextureIndex = UINT32_MAX; // full-res scene depth
	float invDepthW = 0.0f; // 1 / full-res depth extent
	float invDepthH = 0.0f;
	float glowInner = 0.01f; // uv radius of the full-strength glow
	float glowOuter = 0.12f; // uv radius where the glow fades to 0
	uint32_t pad0 = 0;
};
static_assert(sizeof(GodraysMaskPush) == 32);

// Must match godrays_radial.frag.hlsl GodraysRadialPush (40 bytes).
// All-scalar layout on purpose (the spirv-val straddling-vector hazard).
struct GodraysRadialPush {
	uint64_t frameUniforms = 0; // BDA (the sun Uv derivation)
	uint32_t maskTextureIndex = UINT32_MAX; // the quarter-res mask
	uint32_t samplerIndex = 0; // linear clamp
	uint32_t sampleCount = 32; // the kernel length, 4..64
	float decay = 0.97f; // per-sample illumination decay
	float reach = 1.0f; // march length (fraction of the sun distance)
	float weight = 1.0f; // the normalization pre-scale
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(GodraysRadialPush) == 40);

// Must match godrays_filter.frag.hlsl GodraysFilterPush (24 bytes).
struct GodraysFilterPush {
	uint32_t raysTextureIndex = UINT32_MAX; // the raw 1/16 radial output
	uint32_t samplerIndex = 0; // linear clamp
	float texelX = 0.0f; // 1 / sixteenth-res width
	float texelY = 0.0f; // 1 / sixteenth-res height
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(GodraysFilterPush) == 24);

// Must match godrays_composite.frag.hlsl GodraysCompositePush
// (64 bytes). uint64 BDA leads; scalars after.
struct GodraysCompositePush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t hdrTextureIndex = UINT32_MAX; // the scene HDR (frozen source)
	uint32_t depthTextureIndex = UINT32_MAX; // full-res scene depth
	uint32_t raysTextureIndex = UINT32_MAX; // the FILTERED 1/16 rays
	uint32_t raysRawTextureIndex = UINT32_MAX; // unfiltered (debug 2)
	uint32_t maskTextureIndex = UINT32_MAX; // the mask (debug 1)
	uint32_t samplerIndex = 0; // linear clamp
	float invDepthW = 0.0f; // 1 / full-res depth extent
	float invDepthH = 0.0f;
	float intensity = 0.5f; // the artistic strength (subtle default)
	float fogDensity = 0.015f; // the AUTHORED base-zone extinction
	float maxDistance = 80.0f; // the fog distance clamp (sky pixels)
	uint32_t debugMode = 0; // 0 off, 1 mask, 2 raw, 3 filtered, 4 term
	uint32_t useSun = 1; // the volumetric sun master
	uint32_t pad0 = 0;
};
static_assert(sizeof(GodraysCompositePush) == 64);

// ---- desc ----

struct GodraysDesc {
	Format format = Format::RGBA16_Float; // the targets + composite
	float intensity = 0.5f; // subtle by design (an approximation)
	float decay = 0.97f; // the radial kernel's per-sample decay
	float reach = 1.0f; // march length (fraction of the sun distance)
	float glowInner = 0.01f; // uv radius of the full-strength glow
	float glowOuter = 0.12f; // uv radius where the glow fades to 0
	uint32_t sampleCount = 32; // the radial kernel length, 4..64
	const char* name = "godrays";
};

// Default SPVs (the fullscreen-triangle VS shared with the post family).
inline constexpr const char* kGodraysVsPath =
	"content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kGodraysMaskPsPath =
	"content/shaders/godrays_mask.frag.hlsl.spv";
inline constexpr const char* kGodraysRadialPsPath =
	"content/shaders/godrays_radial.frag.hlsl.spv";
inline constexpr const char* kGodraysFilterPsPath =
	"content/shaders/godrays_filter.frag.hlsl.spv";
inline constexpr const char* kGodraysCompositePsPath =
	"content/shaders/godrays_composite.frag.hlsl.spv";

struct ScreenSpaceGodRays {
	// The target helper (the house Target: image + view + bindless +
	// logical, all lifecycle-managed together).
	struct Target {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical{};
		[[nodiscard]] bool Valid() const { return !image.empty() && !logical.empty(); }
	};

	Target mask;          // QUARTER res RGBA16F: the sun occlusion mask
	Target rays;          // 1/16 res RGBA16F: the raw radial gather
	Target raysFiltered;  // 1/16 res RGBA16F: the smoothed fog shadows
	Target composed;      // FULL res RGBA16F: the ray-composited HDR (the output)

	Extent2D extent{ 0, 0 }; // full-res extent (= depth/HDR extent)
	Extent2D quarterExtent{ 0, 0 }; // max(1, full / 4) per axis
	Extent2D sixteenthExtent{ 0, 0 }; // max(1, full / 16) per axis
	Format format = Format::RGBA16_Float;

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> maskFragModule;
	Holder<ShaderModuleHandle> radialFragModule;
	Holder<ShaderModuleHandle> filterFragModule;
	Holder<ShaderModuleHandle> compositeFragModule;
	ShaderProgram maskProgram{};
	ShaderProgram radialProgram{};
	ShaderProgram filterProgram{};
	ShaderProgram compositeProgram{};
	GodraysDesc desc{};

	[[nodiscard]] bool TargetsValid() const {
		return mask.Valid() && rays.Valid() && raysFiltered.Valid()
			&& composed.Valid();
	}
	[[nodiscard]] bool ShadersValid() const {
		return maskProgram.shaderCount != 0 && radialProgram.shaderCount != 0
			&& filterProgram.shaderCount != 0 && compositeProgram.shaderCount != 0
			&& !vsModule.empty() && !maskFragModule.empty()
			&& !radialFragModule.empty() && !filterFragModule.empty()
			&& !compositeFragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetsValid() && ShadersValid(); }

	// Consumer output: the ray-composited FULL-res HDR (the scene HDR
	// chain pointer moves here when the chain ran).
	[[nodiscard]] LogicalImageViewHandle OutputView() const { return composed.logical; }
	[[nodiscard]] uint32_t OutputBindless() const { return composed.bindlessIndex; }

	static Extent2D QuarterExtentFor(Extent2D full) {
		return { std::max(1u, full.width / 4), std::max(1u, full.height / 4) };
	}
	static Extent2D SixteenthExtentFor(Extent2D full) {
		return { std::max(1u, full.width / 16), std::max(1u, full.height / 16) };
	}

	bool EnsureShaders(ResourceManager& ctx) {
		if (ShadersValid())
			return true;
		try {
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(kGodraysVsPath),
					"godraysVert");
			if (maskFragModule.empty())
				maskFragModule = ctx.CreateShaderModule(readFile(kGodraysMaskPsPath),
					"godraysMaskFrag");
			if (radialFragModule.empty())
				radialFragModule = ctx.CreateShaderModule(readFile(kGodraysRadialPsPath),
					"godraysRadialFrag");
			if (filterFragModule.empty())
				filterFragModule = ctx.CreateShaderModule(readFile(kGodraysFilterPsPath),
					"godraysFilterFrag");
			if (compositeFragModule.empty())
				compositeFragModule = ctx.CreateShaderModule(
					readFile(kGodraysCompositePsPath), "godraysCompositeFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || maskFragModule.empty() || radialFragModule.empty()
			|| filterFragModule.empty() || compositeFragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader maskPS{ .module = maskFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader radialPS{ .module = radialFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader filterPS{ .module = filterFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader compositePS{ .module = compositeFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		maskProgram = { .shaders = { vs, maskPS }, .shaderCount = 2 };
		radialProgram = { .shaders = { vs, radialPS }, .shaderCount = 2 };
		filterProgram = { .shaders = { vs, filterPS }, .shaderCount = 2 };
		compositeProgram = { .shaders = { vs, compositePS }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		maskProgram = ShaderProgram();
		radialProgram = ShaderProgram();
		filterProgram = ShaderProgram();
		compositeProgram = ShaderProgram();
		vsModule = {};
		maskFragModule = {};
		radialFragModule = {};
		filterFragModule = {};
		compositeFragModule = {};
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

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D full,
		const GodraysDesc& d) {
		if (full.width == 0 || full.height == 0)
			return false;
		DestroyTargets(rg);
		const Extent2D quarter = QuarterExtentFor(full);
		const Extent2D sixteenth = SixteenthExtentFor(full);

		Target newMask, newRays, newRaysFiltered, newComposed;
		if (!CreateTarget(ctx, rg, quarter, d.format, "godraysMask", newMask))
			return false;
		if (!CreateTarget(ctx, rg, sixteenth, d.format, "godraysRays", newRays)) {
			DestroyTarget(rg, newMask);
			return false;
		}
		if (!CreateTarget(ctx, rg, sixteenth, d.format, "godraysRaysFiltered",
				newRaysFiltered)) {
			DestroyTarget(rg, newMask);
			DestroyTarget(rg, newRays);
			return false;
		}
		if (!CreateTarget(ctx, rg, full, d.format, "godraysComposed",
				newComposed)) {
			DestroyTarget(rg, newMask);
			DestroyTarget(rg, newRays);
			DestroyTarget(rg, newRaysFiltered);
			return false;
		}
		mask = std::move(newMask);
		rays = std::move(newRays);
		raysFiltered = std::move(newRaysFiltered);
		composed = std::move(newComposed);
		extent = full;
		quarterExtent = quarter;
		sixteenthExtent = sixteenth;
		format = d.format;
		desc = d;
		std::fprintf(stderr,
			"[Godrays] targets: full %ux%u -> 1/4 %ux%u, 1/16 %ux%u (%s)\n",
			full.width, full.height, quarter.width, quarter.height,
			sixteenth.width, sixteenth.height, d.name);
		return EnsureShaders(ctx);
	}

	void DestroyTargets(RenderGraph& rg) {
		DestroyTarget(rg, mask);
		DestroyTarget(rg, rays);
		DestroyTarget(rg, raysFiltered);
		DestroyTarget(rg, composed);
		extent = { 0, 0 };
		quarterExtent = { 0, 0 };
		sixteenthExtent = { 0, 0 };
	}

	// Recreate only on extent/format drift (live-updates desc).
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D full,
		const GodraysDesc& d) {
		if (TargetsValid() && ShadersValid()
			&& extent.width == full.width && extent.height == full.height
			&& format == d.format) {
			desc = d;
			return true;
		}
		if (TargetsValid()
			&& extent.width == full.width && extent.height == full.height
			&& format == d.format) {
			desc = d;
			return EnsureShaders(ctx);
		}
		return Create(ctx, rg, full, d);
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyShaders();
	}

	// ---- pass emission ----
	//
	// mask (quarter) -> radial (1/16) -> filter (1/16) -> composite
	// (full, the scene HDR pointer re-points to OutputView()). The
	// chain reads ONLY the scene depth (never the CSM atlas - it runs
	// identically in PCF and EVSM modes, the fog sun-occlusion fallback
	// under EVSM). Captures only copyable handles/indices.
	//
	// fogDensityIn: the AUTHORED base-zone extinction (the env table's
	// value - the panel slider is retired); maxDistanceIn: the fog
	// march range (the sky clamp); useSunIn: the volumetric sun master.
	// debugMode: the compose shader's view selector (0 off).
	void AddPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		float fogDensityIn,
		float maxDistanceIn,
		uint32_t useSunIn,
		const GodraysDesc& overrideDesc,
		uint32_t debugMode,
		const char* maskName = "GodraysMask",
		const char* radialName = "GodraysRadial",
		const char* filterName = "GodraysFilter",
		const char* composeName = "GodraysComposite") const {
		assert(Valid());
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);
		assert(extent.width != 0 && extent.height != 0);

		const float fullW = static_cast<float>(std::max(1u, extent.width));
		const float fullH = static_cast<float>(std::max(1u, extent.height));

		// ---- Mask: the sun glow region over visible sky (quarter res).
		{
			GodraysMaskPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.depthTextureIndex = depthBindlessIndex;
			push.invDepthW = 1.0f / fullW;
			push.invDepthH = 1.0f / fullH;
			push.glowInner = (std::max)(0.0f, overrideDesc.glowInner);
			push.glowOuter = (std::max)(overrideDesc.glowInner + 1e-4f,
				overrideDesc.glowOuter);
			push.pad0 = 0;
			RenderPass& pass = rg.AddPass(maskName ? maskName : "GodraysMask");
			pass.SetOffset(0, 0)
				.SetExtent(quarterExtent)
				.SetInputImages({ depthView })
				.SetColor({ mask.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = maskProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(GodraysMaskPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Radial: the decayed march toward the sun (1/16 res).
		{
			GodraysRadialPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.maskTextureIndex = mask.bindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.sampleCount = std::clamp(overrideDesc.sampleCount, 4u, 64u);
			push.decay = std::clamp(overrideDesc.decay, 0.5f, 1.0f);
			push.reach = std::clamp(overrideDesc.reach, 0.1f, 1.0f);
			push.weight = 1.0f;
			push.pad0 = 0;
			push.pad1 = 0;
			RenderPass& pass = rg.AddPass(radialName ? radialName : "GodraysRadial");
			pass.SetOffset(0, 0)
				.SetExtent(sixteenthExtent)
				.SetInputImages({ mask.logical })
				.SetColor({ rays.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = radialProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(GodraysRadialPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Filter: the tight 5x5 smoothing (1/16 res).
		{
			GodraysFilterPush push{};
			push.raysTextureIndex = rays.bindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.texelX = 1.0f
				/ static_cast<float>(std::max(1u, sixteenthExtent.width));
			push.texelY = 1.0f
				/ static_cast<float>(std::max(1u, sixteenthExtent.height));
			push.pad0 = 0;
			push.pad1 = 0;
			RenderPass& pass = rg.AddPass(filterName ? filterName : "GodraysFilter");
			pass.SetOffset(0, 0)
				.SetExtent(sixteenthExtent)
				.SetInputImages({ rays.logical })
				.SetColor({ raysFiltered.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = filterProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(GodraysFilterPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Composite: base + rays*sunHdr*intensity*fogFront (full).
		{
			GodraysCompositePush push{};
			push.frameUniforms = frameUniformsAddr;
			push.hdrTextureIndex = hdrBindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.raysTextureIndex = raysFiltered.bindlessIndex;
			push.raysRawTextureIndex = rays.bindlessIndex; // debug 2
			push.maskTextureIndex = mask.bindlessIndex; // debug 1
			push.samplerIndex = linearSamplerIndex;
			push.invDepthW = 1.0f / fullW;
			push.invDepthH = 1.0f / fullH;
			push.intensity = (std::max)(0.0f, overrideDesc.intensity);
			push.fogDensity = (std::max)(0.0f, fogDensityIn);
			push.maxDistance = (std::max)(1.0f, maxDistanceIn);
			push.debugMode = debugMode;
			push.useSun = useSunIn;
			push.pad0 = 0;
			RenderPass& pass = rg.AddPass(composeName ? composeName : "GodraysComposite");
			pass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages({ hdrView, depthView, mask.logical,
					rays.logical, raysFiltered.logical })
				.SetColor({ composed.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = compositeProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(GodraysCompositePush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}
	}
};

} // namespace smvk
