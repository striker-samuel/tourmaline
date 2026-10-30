// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Raymarched volumetric fog lit by the baked SG-GI probe volume.
//
// Single-scatter participating media composited in HDR:
//   - Raymarch camera -> scene depth (reversed-Z, exact invProj unproject).
//   - Beer-Lambert extinction + isotropic GI ambient from SgGiSample +
//     analytic sun single-scatter with Henyey-Greenstein phase.
//   - The GI probes carry the COMPLETE baked field (sky + sun/spot bounce),
//     so in-scatter ambient is probe-driven where covered, sky-IBL fallback
//     where not -- same discipline as default_lit.
// Placement: after SSR, before Exposure/Bloom/Tonemap so fog meters/blooms/
// tonemaps with the scene. Input = scene HDR (+ depth), output = same-extent
// HDR with fog composited (no copy when disabled).
//
// Quality model (adjustable):
//   - VolumetricQuality preset -> march steps. Low=8, Medium=16, High=32,
//     Ultra=64 steps.
//   - The composite ALWAYS runs at full render resolution (like SSR): a
//     half-res fog target breaks downstream consumers — the exposure meter
//     .Load()s the scene HDR at full-res pixel coords (out-of-bounds reads
//     on a smaller image), DLSS takes colorIn at renderExtent, and the
//     depth subsample alternates ray lengths at silhouettes (checkered
//     halos after the bilinear upscale). resolutionDivisor is kept at 1;
//     quality scales with steps only.

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>

namespace smvk {

// ---------------------------------------------------------------------------
// Quality presets — STEP COUNTS ONLY: the composite always runs at
// full resolution (ApplyVolumetricQuality pins resolutionDivisor to 1
// regardless of the preset; see the desc's MUST-stay-1 note and the
// header's half-res-history note — the old half-res presets are gone).
// Custom = a hand-edited step count (the UI sets this automatically
// when the steps slider diverges from a preset).
// ---------------------------------------------------------------------------
enum class VolumetricQuality : int {
	Low = 0,     // 8 steps
	Medium = 1,  // 16 steps
	High = 2,    // 32 steps
	Ultra = 3,   // 64 steps
	Custom = 4,  // hand-tuned step count
};

inline const char* VolumetricQualityName(VolumetricQuality q) {
	switch (q) {
	case VolumetricQuality::Low: return "Low (8 steps)";
	case VolumetricQuality::Medium: return "Medium (16 steps)";
	case VolumetricQuality::High: return "High (32 steps)";
	case VolumetricQuality::Ultra: return "Ultra (64 steps)";
	case VolumetricQuality::Custom: return "Custom";
	}
	return "?";
}

struct VolumetricDesc {
	Format format = Format::RGBA16_Float; // must match the HDR target
	uint32_t steps = 16;                  // march steps, 4..128
	uint32_t resolutionDivisor = 1;       // MUST stay 1 (full-res composite;
	                                      // see the quality-model note above)
	float maxDistance = 80.0f;            // ray clamp (m)
	float density = 0.015f;               // extinction coefficient (1/m)
	float intensity = 1.0f;               // scattering multiplier
	float anisotropy = 0.4f;              // HG g, -0.9..0.9 (forward+)
	float sunScale = 1.0f;                // sun single-scatter strength
	float giScale = 1.0f;                 // probe ambient strength
	uint32_t useGi = 1;                   // 1 = sample SG-GI probes
	uint32_t useSun = 1;                  // 1 = analytic sun scatter
	uint32_t useGiVolumes = 1;             // 1 = GI volumes stack into the
	                                       // fog GI (0 = the quality bypass:
	                                       // NO volume walks — the
	                                       // world-field/sky base still
	                                       // samples, independent sources;
	                                       // only the convex-volume
	                                       // composite skips)
	float jitterStrength = 1.0f;          // 0 = off, 1 = full IGN dither
	float heightFalloff = 0.0f;           // extra height density falloff (1/m, 0=off)
	float heightBase = 0.0f;              // reference height (m)
	// The perf-plan cadence knobs (see the push pair's notes): the
	// baked-GI base refresh interval and the world-field density
	// sampling interval (m).
	float giRefreshDist = 1.5f;
	float fieldCadence = 1.0f;
	const char* name = "volumetric";
	const char* viewName = "volumetricView";
	const char* logicalName = "volumetricLogical";
};

// Preset -> steps. The divisor stays 1 (full-res; see above).
inline void ApplyVolumetricQuality(VolumetricDesc& d, VolumetricQuality q) {
	switch (q) {
	case VolumetricQuality::Low:
		d.steps = 8;
		break;
	case VolumetricQuality::Medium:
		d.steps = 16;
		break;
	case VolumetricQuality::High:
		d.steps = 32;
		break;
	case VolumetricQuality::Ultra:
		d.steps = 64;
		break;
	case VolumetricQuality::Custom:
		break;
	}
	d.resolutionDivisor = 1;
}

// Must match volumetric.frag.hlsl VolumetricPush (132 B of members +
// 4 B tail alignment). The three repurposed slots were DEAD fields the
// shader never read (the plan's no-enlargement rule):
//   - texelX/texelY (24): the output texel — the shader derives its
//     sizes from depthTexelSize; the outputs are 1:1 anyway (divisor
//     pinned to 1) -> the CSM cascade-buffer BDA (8-aligned, mid-
//     struct like the fogTile* BDAs below).
//   - useGi (64): the octree probes toggle, retired with .sggi (the
//     HLSL side already carried it as pad0) -> the raw CSM depth
//     atlas bindless index. VolumetricDesc::useGi STAYS (the desc
//     field other code reads); only the push slot is repurposed.
//   - time (88): the march dither is deliberately static (see the
//     shader's jitter note) -> the cascade count (0 = no CSM fog
//     shadowing: the unshadowed sun leg, the original behavior).
struct VolumetricPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	// DEAD in the integrate shader (the HDR read moved to the
	// composite pass; kept for the layout's stability - the field
	// ordering is the HLSL mirror contract).
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	uint32_t steps = 16;
	// The CSM cascade buffer (GpuDirectionalLightDescription slice) —
	// the god-ray leg's SelectCascadeGpu/lightVP/uv*/bias source.
	uint64_t cascadeBuffer = 0;
	float depthTexelX = 0.0f;   // full-res depth texel (1/depthExtent)
	float depthTexelY = 0.0f;
	float maxDistance = 80.0f;
	float density = 0.015f;
	float intensity = 1.0f;
	float anisotropy = 0.4f;
	float sunScale = 1.0f;
	float giScale = 1.0f;
	// The fog sun-shadow depth atlas (whichever leg's - the raw MAIN CSM
	// atlas for the cascade tap, or the DEDICATED volumetric atlas for
	// the coarse tile; NEVER the EVSM blurred moments). UINT32_MAX = no
	// fog sun shadowing (the coarse leg MUST re-check this before
	// sampling - an unbound feed must never read an out-of-range
	// descriptor: undefined garbage that can change per frame).
	uint32_t shadowDepthIndex = UINT32_MAX;
	uint32_t useSun = 1;
	uint32_t useGiVolumes = 1; // 1 = GI volumes stack into the fog GI
	float jitterStrength = 1.0f;
	float heightFalloff = 0.0f;
	float heightBase = 0.0f;
	// 0 = the sun leg stays unshadowed (no CSM / shadows off).
	uint32_t numCascades = 0;
	uint32_t fogGiSource = 0; // 0=worldfield (default), 1=octree (A/B), 2=sky-only
	// Forward+ light feed for fog in-scatter (the forward tiled lights
	// contribute to the march, not just the surfaces): the DEDICATED fog
	// tile lists (march-range candidacy — no depth-slab cuts, see
	// ForwardPlusCullPush::fogTile*; the surface lists' slab culling is
	// wrong for march samples, which live in front of the nearest opaque
	// surface). BDAs ride the same freshness contract as the GI-volume
	// tile masks (address + grid publish together under TileCullWillRun,
	// zeros together — the shader falls back to a direct [0, fpLightCount)
	// walk when the cull didn't run). fpLights/fpLightCount publish
	// whenever the light buffer is uploaded (independent of the cull).
	uint64_t fogTileCounts = 0;   // per-tile fog light lists (0 = stale)
	uint64_t fogTileIndices = 0;  // (0 = stale)
	uint64_t fpLights = 0;       // ForwardPlusLight buffer (this flight)
	uint32_t fpLightCount = 0;   // lights-only bound (probes sort after)
	uint32_t fogMaxPerTile = 16; // fog list stride (matches the cull push)
	uint32_t fogGiDebug = 0;     // 0 = off, 1 = the BC4 coverage view, 2 = the
	                              //     transmittance view (throughput.xxx),
	                              //     3 = the god-ray sun-visibility view
	                              //     (the CSM taps), 4 = the coarse-tile
	                              //     raw depth view, 5 = the coarse-tile
	                              //     fog-shadow visibility view
	// The perf-plan cadence knobs (the two TAIL additions - the block
	// grows 136 -> 144, the old C++ tail pad consumed):
	float giRefreshDist = 1.5f;  // the baked-GI base refresh interval (m;
	                              //     was the hardcoded 0.75 - ambient
	                              //     radiance, coarser is optically safe)
	float fieldCadence = 1.0f;   // the world-field DENSITY sampling interval
	                              //     (m; two cached samples, lerped per
	                              //     segment - extinction-grade accuracy)
};
// 140 B of members + 4 B tail alignment (the uint64_t BDAs): the HLSL
// push block reads through 140; the padded C++ tail is dead bytes.
static_assert(sizeof(VolumetricPush) == 144);
// The repurposed slots (the no-enlargement contract): the cascade BDA
// lands on the old texelSize float2, the atlas index on the retired
// useGi, the count on the dead time — all three 8/4-aligned exactly as
// the HLSL mirror packs them (see the struct's header note).
static_assert(offsetof(VolumetricPush, cascadeBuffer) == 24,
	"VolumetricPush cascadeBuffer mirror drift (was texelSize)");
static_assert(offsetof(VolumetricPush, shadowDepthIndex) == 64,
	"VolumetricPush shadowDepthIndex mirror drift (was useGi)");
static_assert(offsetof(VolumetricPush, numCascades) == 88,
	"VolumetricPush numCascades mirror drift (was time)");

// The full-resolution composite half of the fog split (see the
// integrate push's header note): the depth-aware bilateral upsample of
// the half-res (scatter, throughput) + the HDR reconstruction. Must
// match volumetric_composite.frag.hlsl VolumetricCompositePush
// (52 B of members + 4 B tail alignment - the HLSL block reads
// through 52). uint64 BDA leads; scalars after.
struct VolumetricCompositePush {
	uint64_t frameUniforms = 0; // BDA (invProj for the view distances)
	uint32_t hdrTextureIndex = UINT32_MAX; // the FROZEN scene HDR (pre-fog)
	uint32_t fogTextureIndex = UINT32_MAX; // half-res (scatter, throughput)
	uint32_t fogDepthTextureIndex = UINT32_MAX; // half-res rep view dist (R32F)
	uint32_t depthTextureIndex = UINT32_MAX; // full-res scene depth
	uint32_t samplerIndex = 0; // linear clamp (the HDR tap)
	float invDepthW = 0.0f; // 1 / full-res depth extent
	float invDepthH = 0.0f;
	float maxDistance = 80.0f; // the sky clamp (matches the integrate)
	float depthFalloff = 0.02f; // the bilateral band scale (2% of z)
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(VolumetricCompositePush) == 56);

// Default SPVs (fullscreen-triangle VS shared with tonemap; the split
// PS pair - the old combined shader is retired).
inline constexpr const char* kVolumetricVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kVolumetricIntegratePsPath =
	"content/shaders/volumetric_integrate.frag.hlsl.spv";
inline constexpr const char* kVolumetricCompositePsPath =
	"content/shaders/volumetric_composite.frag.hlsl.spv";

struct VolumetricFog {
	// The half-resolution integration targets (the perf split): the
	// (scatter, throughput) fog + the representative view distance the
	// composite's bilateral weights key on (R32F, LINEAR distance).
	Holder<ImageHandle> fogImage;
	Holder<ImageViewHandle> fogView;
	Holder<BindlessImageHandle> fogBindless;
	uint32_t fogBindlessIndex = UINT32_MAX;
	LogicalImageViewHandle fogLogical{};
	Holder<ImageHandle> fogDepthImage;
	Holder<ImageViewHandle> fogDepthView;
	Holder<BindlessImageHandle> fogDepthBindless;
	uint32_t fogDepthBindlessIndex = UINT32_MAX;
	LogicalImageViewHandle fogDepthLogical{};
	Extent2D halfExtent{ 0, 0 };  // the integration extent (max(1, full/2))
	// The FULL-resolution composite output (the same 1:1 HDR target the
	// old combined pass wrote - exposure/DLSS/tonemap downstream are
	// untouched, the whole point of the split).
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView{};
	Extent2D extent{ 0, 0 };       // output extent (== renderExtent, divisor is 1)
	Extent2D renderExtent{ 0, 0 }; // full-res reference (drift detection)
	Format format = Format::RGBA16_Float;

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> integrateFragModule;
	Holder<ShaderModuleHandle> compositeFragModule;
	ShaderProgram integrateProgram{};
	ShaderProgram compositeProgram{};
	VolumetricDesc desc{};

	[[nodiscard]] bool TargetValid() const {
		return !image.empty() && !logicalView.empty()
			&& !fogImage.empty() && !fogLogical.empty()
			&& !fogDepthImage.empty() && !fogDepthLogical.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return integrateProgram.shaderCount != 0
			&& compositeProgram.shaderCount != 0
			&& !vsModule.empty() && !integrateFragModule.empty()
			&& !compositeFragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetValid() && ShadersValid(); }

	[[nodiscard]] LogicalImageViewHandle OutputView() const { return logicalView; }
	[[nodiscard]] uint32_t OutputBindless() const { return bindlessIndex; }

	static Extent2D OutputExtentFor(Extent2D render, uint32_t divisor) {
		divisor = std::clamp(divisor, 1u, 4u);
		return {
			std::max(1u, render.width / divisor),
			std::max(1u, render.height / divisor),
		};
	}
	// Half extent, the CEIL form (n+1)/2 (the odd-extent column/row fix
	// - see ssgi.h's note; every volumetric shader receives its extents
	// via pushes, so the single definition stays authoritative).
	static Extent2D HalfExtentFor(Extent2D render) {
		return { std::max(1u, (render.width + 1u) / 2u),
			std::max(1u, (render.height + 1u) / 2u) };
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kVolumetricVsPath,
		const char* integratePath = kVolumetricIntegratePsPath,
		const char* compositePath = kVolumetricCompositePsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kVolumetricVsPath;
			const std::string integrate =
				integratePath ? integratePath : kVolumetricIntegratePsPath;
			const std::string composite =
				compositePath ? compositePath : kVolumetricCompositePsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "volumetricVert");
			if (integrateFragModule.empty())
				integrateFragModule = ctx.CreateShaderModule(readFile(integrate),
					"volumetricIntegrateFrag");
			if (compositeFragModule.empty())
				compositeFragModule = ctx.CreateShaderModule(readFile(composite),
					"volumetricCompositeFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || integrateFragModule.empty()
			|| compositeFragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader integratePS{ .module = integrateFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader compositePS{ .module = compositeFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		integrateProgram = { .shaders = { vs, integratePS }, .shaderCount = 2 };
		compositeProgram = { .shaders = { vs, compositePS }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		integrateProgram = ShaderProgram();
		compositeProgram = ShaderProgram();
		vsModule = {};
		integrateFragModule = {};
		compositeFragModule = {};
	}

	bool CreateTarget(ResourceManager& ctx, RenderGraph& rg, Extent2D renderExtentIn, const VolumetricDesc& d) {
		if (renderExtentIn.width == 0 || renderExtentIn.height == 0)
			return false;
		DestroyTarget(rg);

		const Extent2D out = OutputExtentFor(renderExtentIn, d.resolutionDivisor);
		const Extent2D half = HalfExtentFor(renderExtentIn);

		// ---- The half-res integration pair (created FIRST; the failure
		// blocks unwind in reverse): the (scatter, throughput) fog +
		// the representative view distance. ----
		{
			ImageCreateInfo ci{};
			ci.extent = { half.width, half.height, 1 };
			ci.format = d.format;
			ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
			ci.mipLevels = 1;
			ci.arrayLayers = 1;
			ci.name = "volumetricFog";
			Holder<ImageHandle> newFogImage = ctx.CreateImage(ci, nullptr);
			Holder<ImageViewHandle> newFogView;
			Holder<BindlessImageHandle> newFogBindless;
			uint32_t newFogBindlessIndex = UINT32_MAX;
			LogicalImageViewHandle newFogLogical;
			if (!newFogImage.empty()) {
				ImageViewCreateInfo vci{};
				vci.viewType = ImageViewType::View2D;
				vci.format = Format::Undefined; // inherit
				vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
				vci.name = "volumetricFogView";
				newFogView = ctx.CreateImageView(newFogImage, vci);
				if (!newFogView.empty()) {
					newFogBindless = ctx.CreateBindlessImage(newFogView,
						ImageLayout::ShaderReadOnlyOptimal);
					if (!newFogBindless.empty()) {
						newFogBindlessIndex = ctx.GetBindlessIndex(newFogBindless);
						newFogLogical =
							rg.AddExternalImageView(newFogView, d.format,
								"volumetricFogLogical");
					}
				}
			}
			if (newFogLogical.empty()) {
				return false;
			}
			fogImage = std::move(newFogImage);
			fogView = std::move(newFogView);
			fogBindless = std::move(newFogBindless);
			fogBindlessIndex = newFogBindlessIndex;
			fogLogical = newFogLogical;
		}
		{
			ImageCreateInfo ci{};
			ci.extent = { half.width, half.height, 1 };
			ci.format = Format::R32_Float; // LINEAR view distance (the key)
			ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
			ci.mipLevels = 1;
			ci.arrayLayers = 1;
			ci.name = "volumetricFogDepth";
			Holder<ImageHandle> newDepthImage = ctx.CreateImage(ci, nullptr);
			Holder<ImageViewHandle> newDepthView;
			Holder<BindlessImageHandle> newDepthBindless;
			uint32_t newDepthBindlessIndex = UINT32_MAX;
			LogicalImageViewHandle newDepthLogical;
			if (!newDepthImage.empty()) {
				ImageViewCreateInfo vci{};
				vci.viewType = ImageViewType::View2D;
				vci.format = Format::Undefined; // inherit
				vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
				vci.name = "volumetricFogDepthView";
				newDepthView = ctx.CreateImageView(newDepthImage, vci);
				if (!newDepthView.empty()) {
					newDepthBindless = ctx.CreateBindlessImage(newDepthView,
						ImageLayout::ShaderReadOnlyOptimal);
					if (!newDepthBindless.empty()) {
						newDepthBindlessIndex =
							ctx.GetBindlessIndex(newDepthBindless);
						newDepthLogical =
							rg.AddExternalImageView(newDepthView,
								Format::R32_Float,
								"volumetricFogDepthLogical");
					}
				}
			}
			if (newDepthLogical.empty()) {
				DestroyFogTargets(rg);
				return false;
			}
			fogDepthImage = std::move(newDepthImage);
			fogDepthView = std::move(newDepthView);
			fogDepthBindless = std::move(newDepthBindless);
			fogDepthBindlessIndex = newDepthBindlessIndex;
			fogDepthLogical = newDepthLogical;
		}

		// ---- The full-resolution composite output (the SAME 1:1 target
		// the old combined pass wrote - downstream untouched). ----
		ImageCreateInfo ci{};
		ci.extent = { out.width, out.height, 1 };
		ci.format = d.format;
		// TransferSrc: debug stage dumps (TOURMALINE_SSR_DUMP) — armed dumps
		// only (DownloadDataFromImage needs it on the source image).
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
		if (DebugImageDumpArmed())
			ci.usage |= ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = d.name;

		Holder<ImageHandle> newImage = ctx.CreateImage(ci, nullptr);
		if (newImage.empty()) {
			DestroyFogTargets(rg);
			return false;
		}

		ImageViewCreateInfo vci{};
		vci.viewType = ImageViewType::View2D;
		vci.format = Format::Undefined; // inherit
		vci.subresourceRange = { ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = d.viewName;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty()) {
			DestroyFogTargets(rg);
			return false;
		}

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
		if (newBindless.empty()) {
			DestroyFogTargets(rg);
			return false;
		}
		const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newView, d.format, d.logicalName);
		if (newLogical.empty()) {
			DestroyFogTargets(rg);
			return false;
		}

		image = std::move(newImage);
		view = std::move(newView);
		bindless = std::move(newBindless);
		bindlessIndex = newBindlessIndex;
		logicalView = newLogical;
		extent = out;
		halfExtent = half;
		renderExtent = renderExtentIn;
		format = d.format;
		ctx.GetOrCreateSampler(SamplerCreateInfo{});
		return true;
	}

	void DestroyFogTargets(RenderGraph& rg) {
		if (!fogLogical.empty())
			rg.FreeImageView(fogLogical);
		fogLogical = {};
		fogBindlessIndex = UINT32_MAX;
		fogBindless = {};
		fogView = {};
		fogImage = {};
		if (!fogDepthLogical.empty())
			rg.FreeImageView(fogDepthLogical);
		fogDepthLogical = {};
		fogDepthBindlessIndex = UINT32_MAX;
		fogDepthBindless = {};
		fogDepthView = {};
		fogDepthImage = {};
		halfExtent = { 0, 0 };
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
		renderExtent = { 0, 0 };
		DestroyFogTargets(rg);
	}

	// Idempotent output + program creation. Live-updates desc (lighting
	// knobs need no realloc; steps/divisor realloc only on drift).
	// Returns true when ready.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D renderExtentIn,
		const VolumetricDesc& d,
		const char* vsPath = kVolumetricVsPath,
		const char* integratePath = kVolumetricIntegratePsPath,
		const char* compositePath = kVolumetricCompositePsPath) {
		desc = d;
		const Extent2D want = OutputExtentFor(renderExtentIn, d.resolutionDivisor);
		const Extent2D wantHalf = HalfExtentFor(renderExtentIn);
		bool ok = true;
		if (!(TargetValid() &&
				extent.width == want.width && extent.height == want.height &&
				halfExtent.width == wantHalf.width
				&& halfExtent.height == wantHalf.height &&
				renderExtent.width == renderExtentIn.width &&
				renderExtent.height == renderExtentIn.height &&
				format == d.format)) {
			ok = CreateTarget(ctx, rg, renderExtentIn, d);
		}
		ok = EnsureShaders(ctx, vsPath, integratePath, compositePath) && ok;
		return ok;
	}

	void Destroy(RenderGraph& rg) {
		DestroyTarget(rg);
		DestroyShaders();
	}

	// Emits the HDR -> HDR volumetric composite. Captures only copyable
	// handles/indices, so the pass is safe once this call returns.
	// depthExtent = full-res depth size (for .Load mapping when divisor > 1).
	// The four trailing args are the fog sun-shadow feed (appended to
	// preserve existing callers; defaults = the original unshadowed sun
	// leg), PER MODE (the shader picks ONE leg - coarse tile or CSM
	// tap, never both): cascadeBufferAddr = the
	// GpuDirectionalLightDescription slice BDA (the scene push's
	// pc.cascadeBuffer) + numCascadesIn = pc.numCascades feed the CSM
	// TAP leg (PCF mode only - under EVSM the raw atlas holds stale
	// data, callers gate this off); shadowDepthView/Index = the depth
	// atlas the march samples - the RAW main atlas for the CSM tap, or
	// the DEDICATED coarse volumetric atlas for the tile leg (mode 2 -
	// its own raw pass renders under EVSM too, making that leg the EVSM
	// fallback). numCascades = 0 with a bound view+index is the coarse
	// mode's shape: the CSM leg is off, the tile leg runs. NEVER the
	// EVSM blurred moments (light.shadow_map_index flips to those when
	// EVSM is on, but the fog's single-tap depth compare needs raw D32
	// values). The atlas rides SetInputImages so the graph orders the
	// last shadow write before this read.
	RenderPass& AddVolumetricPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		Extent2D depthExtent,
		float frameTime,
		const VolumetricDesc& overrideDesc,
		const char* name = nullptr,
		// Forward+ light feed (fog in-scatter): the DEDICATED fog tile
		// lists' addresses ride the cull freshness contract (0 = stale ->
		// direct walk), the light buffer + count publish whenever
		// uploaded.
		uint64_t fogTileCountsAddr = 0,
		uint64_t fogTileIndicesAddr = 0,
		uint64_t fpLightsAddr = 0,
		uint32_t fpLightCountIn = 0,
		uint32_t fogMaxPerTileIn = 16,
		// The CSM god-ray feed (see the signature note above).
		uint64_t cascadeBufferAddr = 0,
		uint32_t numCascadesIn = 0,
		LogicalImageViewHandle shadowDepthView = LogicalImageViewHandle{},
		uint32_t shadowDepthBindlessIndex = UINT32_MAX) const {
		assert(Valid());
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);
		assert(extent.width != 0 && extent.height != 0);
		(void)frameTime; // signature stability (the time slot is numCascades now)

		VolumetricPush push{};
		push.frameUniforms = frameUniformsAddr;
		push.hdrTextureIndex = hdrBindlessIndex;
		push.depthTextureIndex = depthBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.steps = std::clamp(overrideDesc.steps, 4u, 128u);
		push.cascadeBuffer = cascadeBufferAddr;
		push.depthTexelX = 1.0f / static_cast<float>(std::max(1u, depthExtent.width));
		push.depthTexelY = 1.0f / static_cast<float>(std::max(1u, depthExtent.height));
		push.maxDistance = (std::max)(1.0f, overrideDesc.maxDistance);
		push.density = (std::max)(0.0f, overrideDesc.density);
		push.intensity = (std::max)(0.0f, overrideDesc.intensity);
		push.anisotropy = std::clamp(overrideDesc.anisotropy, -0.9f, 0.9f);
		push.sunScale = (std::max)(0.0f, overrideDesc.sunScale);
		push.giScale = (std::max)(0.0f, overrideDesc.giScale);
		// The sun-shadow FEED gate: the atlas view + index must be bound
		// (the view is what orders the last shadow write before this
		// read; an index alone would race the shadow pass) — for EITHER
		// leg. The CSM leg ADDITIONALLY needs the cascade count + buffer
		// (numCascades = 0 with a bound atlas = the coarse tile's
		// shape: the CSM loop never runs, SampleVolumetricShadow does).
		// useSun stays the artistic master (off = no sun leg, so no
		// taps either). A caller that binds NOTHING gets the original
		// unshadowed sun leg (index stays UINT32_MAX — the shader's
		// coarse gate re-checks it; an unbound feed must never sample
		// an out-of-range descriptor).
		const bool shadowFeedBound = shadowDepthBindlessIndex != UINT32_MAX
			&& !shadowDepthView.empty();
		const bool csmLegEnabled = shadowFeedBound
			&& numCascadesIn > 0
			&& cascadeBufferAddr != 0;
		push.shadowDepthIndex = shadowFeedBound
			? shadowDepthBindlessIndex : UINT32_MAX;
		push.numCascades = csmLegEnabled ? numCascadesIn : 0u;
		push.useSun = overrideDesc.useSun ? 1u : 0u;
		push.useGiVolumes = overrideDesc.useGiVolumes ? 1u : 0u;
		push.fogTileCounts = fogTileCountsAddr;
		push.fogTileIndices = fogTileIndicesAddr;
		push.fpLights = fpLightsAddr;
		push.fpLightCount = fpLightCountIn;
		push.fogMaxPerTile = fogMaxPerTileIn;
		push.jitterStrength = std::clamp(overrideDesc.jitterStrength, 0.0f, 1.0f);
		push.heightFalloff = (std::max)(0.0f, overrideDesc.heightFalloff);
		push.heightBase = overrideDesc.heightBase;
		// (frameTime stays a parameter for signature stability; the old
		// push.time slot it fed is repurposed as numCascades — the march
		// dither is deliberately static, see the shader's jitter note.)
		// The fog-GI A/B source toggle (TOURMALINE_FOG_GI_SOURCE):
		// 0/unset = worldfield (the default BC6H mean field — the shader
		// falls to the sky mean wherever the field is not resident, so
		// the default never loses the loaded probe GI to the subsystem
		// being off), [1 RESERVED — the legacy octree walk is retired
		// with the .sggi runtime; the value reads as the default],
		// 2 = sky-only (the floor baseline for perf A/B). Parsed once
		// like the other env pins.
		push.fogGiSource = [] {
			const char* e = std::getenv("TOURMALINE_FOG_GI_SOURCE");
			return e ? uint32_t(std::clamp(std::atoi(e), 0, 2)) : 0u;
		}();
		// The fog-GI coverage debug view (TOURMALINE_FOG_GI_DEBUG).
		// Mode 1 (coverage): resident data displays in GRAYSCALE —
		// red marks a missing radiance brick, green marks coverage
		// data that is still streaming (the readable map of what the
		// fog's GI base consumes). Mode 2 (transmittance): the march's
		// surviving throughput in grayscale — the optical-density
		// diagnostic: white = no extinction, black = complete (the
		// review's prescribed density probe, replacing the composite).
		push.fogGiDebug = [] {
			const char* e = std::getenv("TOURMALINE_FOG_GI_DEBUG");
			return e ? uint32_t(std::clamp(std::atoi(e), 0, 5)) : 0u;
		}();
		// The perf-plan cadence knobs (the desc's pair - see the push
		// struct's note).
		push.giRefreshDist = (std::max)(0.1f, overrideDesc.giRefreshDist);
		push.fieldCadence = (std::max)(0.1f, overrideDesc.fieldCadence);

		// ---- Pass 1: the INTEGRATION (half res) - the march only, no
		// HDR read. MRT: the (scatter, throughput) fog + the rep view
		// distance. The returned pass is THIS one (the caller's read
		// hints - the GI-volume tile masks, the Forward+ fog tile
		// lists - attach to the pass that reads those buffers).
		RenderPass& pass = rg.AddPass(name ? name : "Volumetric");
		pass.SetOffset(0, 0)
			.SetExtent(halfExtent);
		// The shadow atlas (whichever leg's - the main raw CSM atlas or
		// the dedicated volumetric atlas) is a sampled input only while
		// the feed is bound (the graph owns the last-shadow-write ->
		// this-read ordering through the logical view; the Main pass
		// binds the same view for its surface shadows).
		if (shadowFeedBound)
			pass.SetInputImages({ depthView, shadowDepthView });
		else
			pass.SetInputImages({ depthView });
		pass.SetColor({ fogLogical, fogDepthLogical },
				AttachmentLoadOp::DontCare,
				AttachmentStoreOp::Store);
		{
			ShaderProgram progCopy = integrateProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(VolumetricPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Pass 2: the COMPOSITE (full res) - the depth-aware
		// bilateral upsample of the half-res fog + the HDR
		// reconstruction into the SAME 1:1 output target the old
		// combined pass wrote (exposure/DLSS/tonemap downstream are
		// untouched - the whole point of the split).
		{
			VolumetricCompositePush cpush{};
			cpush.frameUniforms = frameUniformsAddr;
			cpush.hdrTextureIndex = hdrBindlessIndex;
			cpush.fogTextureIndex = fogBindlessIndex;
			cpush.fogDepthTextureIndex = fogDepthBindlessIndex;
			cpush.depthTextureIndex = depthBindlessIndex;
			cpush.samplerIndex = linearSamplerIndex;
			cpush.invDepthW = 1.0f
				/ static_cast<float>(std::max(1u, depthExtent.width));
			cpush.invDepthH = 1.0f
				/ static_cast<float>(std::max(1u, depthExtent.height));
			cpush.maxDistance = (std::max)(1.0f, overrideDesc.maxDistance);
			cpush.depthFalloff = 0.02f;
			cpush.pad0 = 0;
			cpush.pad1 = 0;

			RenderPass& cpass = rg.AddPass("VolumetricComposite");
			cpass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages({ hdrView, depthView, fogLogical,
					fogDepthLogical })
				.SetColor({ logicalView },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = compositeProgram; // copyable (handles only)
			cpass.SetRecordingFunc([progCopy, cpush](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(VolumetricCompositePush), &cpush,
					ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}
		return pass;
	}

	// Uses the stored desc from Ensure().
	RenderPass& AddVolumetricPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		Extent2D depthExtent,
		float frameTime,
		const char* name = nullptr,
		uint64_t cascadeBufferAddr = 0,
		uint32_t numCascadesIn = 0,
		LogicalImageViewHandle shadowDepthView = LogicalImageViewHandle{},
		uint32_t shadowDepthBindlessIndex = UINT32_MAX) const {
		return AddVolumetricPass(rg, hdrView, hdrBindlessIndex, depthView, depthBindlessIndex,
			frameUniformsAddr, linearSamplerIndex, depthExtent, frameTime, desc, name,
			0, 0, 0, 0, 16, cascadeBufferAddr, numCascadesIn,
			shadowDepthView, shadowDepthBindlessIndex);
	}
};

} // namespace smvk
