#pragma once
// SMAA - Enhanced Subpixel Morphological Antialiasing (SMAA 1x + SMAA T2x).
//
// Port of the official implementation (https://github.com/iryoku/smaa, MIT -
// see the license headers in engine/shaders/smaa.hlsli and
// engine/include/Engine/Rendergraph/smaa_area_tex.h, plus the entry in
// THIRD_PARTY_NOTICES.md) onto this engine's rendergraph conventions.
//
// Pass chain (fullscreen triangle per pass, bindless sampling, barriers from
// SetColor (write) + SetInputImages (read)):
//
//   Tonemapper -> |ldr|            (app retargets the tonemap pass onto the
//                                  offscreen LDR target owned here - the
//                                  swapchain cannot be sampled)
//   SmaaEdge:         ldr                     -> edges    (RGBA8, cleared)
//   SmaaWeights:      edges + areaTex + searchTex
//                                          -> weights  (RGBA8, cleared)
//   SmaaNeighborhood: ldr + weights + motion
//                                          -> history[slot] (T2x, RGBA,
//                                             alpha = packed velocity)
//                                          -> or swapchain (SMAA 1x)
//   SmaaResolve:      history pair + motion  -> swapchain  (T2x only)
//
// The UI composite then loads the swapchain as usual.
//
// Formats: the whole chain is UNORM 8-bit on purpose. Official guidance:
// "all texture reads and buffer writes must be non-sRGB" for the edge
// detection color input. The swapchain is sRGB, so the tonemapper encodes
// manually into the UNORM ldr target (gammaEncode), the chain works in
// gamma space (explicitly OK'd upstream for the neighborhood blending
// pass), and the swapchain-writing passes (neighborhood in 1x, the T2x
// resolve) decode back to linear so the hardware store encode restores the
// same bytes the tonemapper would have written directly (outputLinear).
// ldr/history match the swapchain channel order (BGRA8 on this backend),
// edges/weights are RGBA8 (official recommendation over RG on NVIDIA).
//
// T2x jitter + subsample indices (official table; positions assume a
// bottom-to-top y axis, this engine is y-down after the projection Y-flip,
// so the y sign flips while the physical corner pair is unchanged):
//   phase 0: jitter (+0.25, +0.25) px y-down  -> float4(1, 1, 1, 0)
//   phase 1: jitter (-0.25, -0.25) px y-down  -> float4(2, 2, 2, 0)
// Pixels here are CHAIN (swap/output resolution) pixels: the two phases
// are combined by the resolve at swapExtent, so the quarter-pixel is
// measured there (SceneRenderer::UpdateFrameUniforms) and the subsample
// indices pair exactly at any renderScale. In render pixels the shift is
// 0.25*renderScale; reprojection stays exact regardless because the
// motion-vector pass inherits the true offset through prevViewProj.
// SMAA 1x passes float4(0, 0, 0, 0) and no jitter.
//
// Phase ownership (record cadence, NOT per swapchain image): the T2x phase
// is a property of the recorded-frame sequence - a single global toggle
// advanced once per T2x record. Every record writes history[parity] and
// blends against history[parity^1], which then always holds the
// immediately preceding record: exactly the frame the motion-vector pass
// diffs against (prevViewProj is likewise last-record), so each record is
// self-consistent whatever image it targets and whatever the present mode.
// Under VSync FIFO this displays strict P0,P1,P0,P1. Under
// Mailbox/Immediate a replaced present only skips a phase on screen
// (P0,P1,P1,P0...); the resolve itself stays correct because it always
// blends consecutive records. (The previous per-image keying made each
// image alternate with itself, but the screen cycles images, so FIFO
// displayed P0,P0,P1,P1 - holds plus 0.5px jumps - and served 2-record-old
// colors to 1-record-old velocities. Deliberately not guarded by image
// order: any acquire order is record-consistent, so order-based
// invalidation would only flash passthrough frames.)
// See SmaaT2xState below (pure CPU, headless-testable).
//
// Velocity buffer: MotionVectors' RG16F target (prevUV - currUV in UV
// units - resolution independent, so the chain running at swap extent
// while the scene renders at renderExtent is fine). SMAA_DECODE_VELOCITY
// is the default .rg; the resolve negates it for reprojection. The
// vectors INCLUDE the T2x jitter (prevViewProj is jittered) - required
// for exact reprojection; the neighborhood pass strips the known jitter
// delta (sp.jitterDelta) from the magnitude it packs into alpha so the
// resolve's history attenuation reacts to true motion only.
//
// History: two ping-pong targets written by the neighborhood pass on
// alternating frames; the resolve combines the current and previous
// subsample (no recursive accumulation - T2x is a two-sample resolve).
// historyValid=false on the first frame (or after resize / mode switch)
// makes the resolve pass the current subsample through instead of blending
// against never-written/stale history.
//
// Usage:
//   smvk::Smaa smaa;
//   smaa.EnsureLuts(ctx);                       // once, before FlushUploads
//   smaa.Ensure(ctx, rg, swapExtent, desc);      // targets + programs
//   tonemap.AddTonemapPass(rg, ..., smaa.ldr.logicalView, swapExtent, ...,
//                          /*gammaEncodeOutput=*/true);
//   if (smaaActive)
//     smaa.AddSmaaPasses(rg, motionView, motionBindless,
//                       linearSampler, pointSampler,
//                       t2x, historyValid, subsample, parity,
//                       colors, swapExtent, swapIsSrgb);

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/smaa_area_tex.h>
#include <Engine/Rendergraph/smaa_search_tex.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

// Global T2x temporal state: one per renderer, advanced once per recorded
// frame (CPU-sequential in RenderFrame, before Execute). parity is the T2x
// phase == the history slot the next record writes; historyValid gates the
// resolve blend (false = passthrough of the current subsample).
struct SmaaT2xState {
	uint32_t parity = 0;
	bool historyValid = false;
};

// Record-time snapshot: jitter sign, history slot and subsample indices
// all derive from parity; historyValid allows the resolve blend.
struct SmaaFrameParams {
	uint32_t parity = 0;
	bool historyValid = false;
};

// Record-time snapshot (runs before Execute). Zeros when T2x is off:
// SMAA 1x subsample + passthrough.
inline SmaaFrameParams SmaaBeginFrame(const SmaaT2xState& st, bool t2xActive) {
	SmaaFrameParams p{};
	if (!t2xActive)
		return p;
	p.parity = st.parity & 1u;
	p.historyValid = st.historyValid;
	return p;
}

// End-of-record step (runs after Execute). ranT2x mirrors the
// chain-emission gate: a non-T2x record freezes the phase and invalidates,
// so the next T2x record warms up instead of blending against stale
// history.
inline void SmaaEndFrame(SmaaT2xState& st, bool ranT2x) {
	if (!ranT2x) {
		st.historyValid = false;
		return;
	}
	st.historyValid = true;
	st.parity ^= 1u;
}

// Explicit invalidation for history-content loss the toggle cannot see:
// swapchain rebuild (targets recreated) and AA-mode changes (stale data
// from an older T2x run). Resets parity so the jitter restarts at phase 0.
inline void SmaaInvalidateHistory(SmaaT2xState& st) {
	st.historyValid = false;
	st.parity = 0;
}

// Must match SmaaPush in engine/shaders/smaa_push.hlsli (88 bytes,
// all scalars). One shared layout for every SMAA pass; each pass fills
// only the fields it reads.
struct SmaaPush {
	uint32_t colorTexIndex = UINT32_MAX;      // edge/nb: LDR color input
	uint32_t edgesTexIndex = UINT32_MAX;      // weights: edges output
	uint32_t areaTexIndex = UINT32_MAX;       // weights: area LUT
	uint32_t searchTexIndex = UINT32_MAX;     // weights: search LUT
	uint32_t blendTexIndex = UINT32_MAX;       // nb: blend weights
	uint32_t currentTexIndex = UINT32_MAX;     // resolve: history[slot]
	uint32_t previousTexIndex = UINT32_MAX;   // resolve: history[slot^1]
	uint32_t velocityTexIndex = UINT32_MAX;   // nb/resolve: motion vectors
	uint32_t linearSamplerIndex = 0;
	uint32_t pointSamplerIndex = 0;
	uint32_t historyValid = 0;                // resolve: 0 = passthrough
	float subsampleIdxX = 0.0f;               // weights: subsample indices
	float subsampleIdxY = 0.0f;
	float subsampleIdxZ = 0.0f;
	float subsampleIdxW = 0.0f;
	float rtMetricsX = 0.0f;                   // float4(1/w, 1/h, w, h)
	float rtMetricsY = 0.0f;
	float rtMetricsZ = 0.0f;
	float rtMetricsW = 0.0f;
	uint32_t outputLinear = 0;                 // nb/resolve: 1 = decode to linear for the sRGB swapchain store, 0 = keep encoded bytes (UNORM history)
	// nb only: per-frame jitter delta between the previous and the current
	// record's sample positions, in UV (prevUV - currUV contribution of
	// the jitter alone). The pass packs the DEJITTERED velocity magnitude
	// into alpha so the resolve's attenuation reacts to true motion only
	// (the raw jittered velocity is still used for reprojection there).
	// Zero when no jitter is active (SMAA 1x / MSAA / off).
	float jitterDeltaX = 0.0f;
	float jitterDeltaY = 0.0f;
};
static_assert(sizeof(SmaaPush) == 88);

struct SmaaDesc {
	// UNORM sibling of the swapchain surface format: the chain works in
	// gamma-encoded bytes (channel order must match the swapchain so the
	// decode-on-write restores the tonemapper's values).
	Format ldrFormat = Format::BGRA8_UNorm;
	const char* name = "smaa";
};

// Default SPVs (compiled from engine/shaders by the CMake shader step).
inline constexpr const char* kSmaaEdgeVsPath = "content/shaders/smaa_edge.vert.hlsl.spv";
inline constexpr const char* kSmaaEdgePsPath = "content/shaders/smaa_edge.frag.hlsl.spv";
inline constexpr const char* kSmaaWeightsVsPath = "content/shaders/smaa_weights.vert.hlsl.spv";
inline constexpr const char* kSmaaWeightsPsPath = "content/shaders/smaa_weights.frag.hlsl.spv";
inline constexpr const char* kSmaaNeighborhoodVsPath = "content/shaders/smaa_neighborhood.vert.hlsl.spv";
inline constexpr const char* kSmaaNeighborhoodPsPath = "content/shaders/smaa_neighborhood.frag.hlsl.spv";
inline constexpr const char* kSmaaResolveVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kSmaaResolvePsPath = "content/shaders/smaa_resolve.frag.hlsl.spv";

// One color target: image + view + bindless + rendergraph logical alias
// (the lifecycle used by MotionBlur / HdrTarget, extracted for the five
// SMAA targets).
struct SmaaImageTarget {
	Holder<ImageHandle> image;
	Holder<ImageViewHandle> view;
	Holder<BindlessImageHandle> bindless;
	uint32_t bindlessIndex = UINT32_MAX;
	LogicalImageViewHandle logicalView{};
	Extent2D extent{ 0, 0 };
	Format format = Format::Undefined;

	[[nodiscard]] bool Valid() const { return !image.empty() && !logicalView.empty(); }

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D targetExtent,
		Format targetFormat, const char* name, const char* viewName, const char* logicalName) {
		if (targetExtent.width == 0 || targetExtent.height == 0)
			return false;
		Destroy(rg);

		ImageCreateInfo ci{};
		ci.extent = { targetExtent.width, targetExtent.height, 1 };
		ci.format = targetFormat;
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
		vci.name = viewName;

		Holder<ImageViewHandle> newView = ctx.CreateImageView(newImage, vci);
		if (newView.empty())
			return false;

		Holder<BindlessImageHandle> newBindless =
			ctx.CreateBindlessImage(newView, ImageLayout::ShaderReadOnlyOptimal);
		if (newBindless.empty())
			return false;
		const uint32_t newBindlessIndex = ctx.GetBindlessIndex(newBindless);

		LogicalImageViewHandle newLogical = rg.AddExternalImageView(newView, targetFormat, logicalName);
		if (newLogical.empty())
			return false;

		image = std::move(newImage);
		view = std::move(newView);
		bindless = std::move(newBindless);
		bindlessIndex = newBindlessIndex;
		logicalView = newLogical;
		extent = targetExtent;
		format = targetFormat;
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
		format = Format::Undefined;
	}
};

struct Smaa {
	// Persistent targets (swap extent):
	//   ldr        - tonemapper output when SMAA is active (input of edge/nb)
	//   edges      - edge detection output (RGBA8, cleared every frame)
	//   weights    - blend weights output (RGBA8, cleared every frame)
	//   history[2] - neighborhood-blending outputs, ping-ponged by `parity`;
	//                RGBA (alpha = sqrt(5*|velocity|) for the resolve)
	SmaaImageTarget ldr;
	SmaaImageTarget edges;
	SmaaImageTarget weights;
	SmaaImageTarget history[2];

	// Official precomputed LUTs (smaa_area_tex.h / smaa_search_tex.h).
	Holder<TextureHandle> areaTex;
	Holder<TextureHandle> searchTex;
	uint32_t areaBindless = UINT32_MAX;
	uint32_t searchBindless = UINT32_MAX;

	// Programs (edge/weights/nb have dedicated vertex shaders; the resolve
	// reuses the tonemapper's fullscreen vertex shader).
	Holder<ShaderModuleHandle> edgeVsModule, edgePsModule;
	Holder<ShaderModuleHandle> weightsVsModule, weightsPsModule;
	Holder<ShaderModuleHandle> nbVsModule, nbPsModule;
	Holder<ShaderModuleHandle> resolveVsModule, resolvePsModule;
	ShaderProgram edgeProgram{};
	ShaderProgram weightsProgram{};
	ShaderProgram nbProgram{};
	ShaderProgram resolveProgram{};

	Extent2D extent{ 0, 0 };
	Format ldrFormat = Format::BGRA8_UNorm;

	[[nodiscard]] bool ShadersValid() const {
		return edgeProgram.shaderCount != 0 && weightsProgram.shaderCount != 0
			&& nbProgram.shaderCount != 0 && resolveProgram.shaderCount != 0;
	}
	[[nodiscard]] bool LutsValid() const {
		return !areaTex.empty() && !searchTex.empty()
			&& areaBindless != UINT32_MAX && searchBindless != UINT32_MAX;
	}
	[[nodiscard]] bool TargetsValid() const {
		return ldr.Valid() && edges.Valid() && weights.Valid()
			&& history[0].Valid() && history[1].Valid();
	}
	// LUTs are part of validity: the weights pass samples them, so a
	// failed upload must gate the whole chain off (assert in AddSmaaPasses).
	[[nodiscard]] bool Valid() const { return TargetsValid() && ShadersValid() && LutsValid(); }

	// One-time upload of the official area (160x560 RG8) + search (64x16
	// R8) tables. Call before ResourceManager::FlushUploads (staged
	// uploads join that flush).
	bool EnsureLuts(ResourceManager& ctx) {
		if (LutsValid())
			return true;

		ImageCreateInfo ci{};
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.usage = ImageUsage::Sampled | ImageUsage::TransferDst;

		ci.format = Format::RG8_UNorm;
		ci.extent = { smaa::kAreaTexWidth, smaa::kAreaTexHeight, 1 };
		ci.name = "smaaAreaTex";
		Holder<TextureHandle> newArea = ctx.CreateTexture(ci, smaa::kAreaTexBytes);
		if (newArea.empty())
			return false;

		ci.format = Format::R8_UNorm;
		ci.extent = { smaa::kSearchTexWidth, smaa::kSearchTexHeight, 1 };
		ci.name = "smaaSearchTex";
		Holder<TextureHandle> newSearch = ctx.CreateTexture(ci, smaa::kSearchTexBytes);
		if (newSearch.empty())
			return false;

		areaTex = std::move(newArea);
		searchTex = std::move(newSearch);
		areaBindless = ctx.GetTextureBindlessIndex(areaTex);
		searchBindless = ctx.GetTextureBindlessIndex(searchTex);
		return LutsValid();
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* edgeVsPath = kSmaaEdgeVsPath, const char* edgePsPath = kSmaaEdgePsPath,
		const char* weightsVsPath = kSmaaWeightsVsPath, const char* weightsPsPath = kSmaaWeightsPsPath,
		const char* nbVsPath = kSmaaNeighborhoodVsPath, const char* nbPsPath = kSmaaNeighborhoodPsPath,
		const char* resolveVsPath = kSmaaResolveVsPath, const char* resolvePsPath = kSmaaResolvePsPath) {
		if (ShadersValid())
			return true;
		try {
			if (edgeVsModule.empty())
				edgeVsModule = ctx.CreateShaderModule(readFile(edgeVsPath), "smaaEdgeVert");
			if (edgePsModule.empty())
				edgePsModule = ctx.CreateShaderModule(readFile(edgePsPath), "smaaEdgeFrag");
			if (weightsVsModule.empty())
				weightsVsModule = ctx.CreateShaderModule(readFile(weightsVsPath), "smaaWeightsVert");
			if (weightsPsModule.empty())
				weightsPsModule = ctx.CreateShaderModule(readFile(weightsPsPath), "smaaWeightsFrag");
			if (nbVsModule.empty())
				nbVsModule = ctx.CreateShaderModule(readFile(nbVsPath), "smaaNeighborhoodVert");
			if (nbPsModule.empty())
				nbPsModule = ctx.CreateShaderModule(readFile(nbPsPath), "smaaNeighborhoodFrag");
			if (resolveVsModule.empty())
				resolveVsModule = ctx.CreateShaderModule(readFile(resolveVsPath), "smaaResolveVert");
			if (resolvePsModule.empty())
				resolvePsModule = ctx.CreateShaderModule(readFile(resolvePsPath), "smaaResolveFrag");
		} catch (...) {
			return false;
		}
		if (edgeVsModule.empty() || edgePsModule.empty() || weightsVsModule.empty()
			|| weightsPsModule.empty() || nbVsModule.empty() || nbPsModule.empty()
			|| resolveVsModule.empty() || resolvePsModule.empty())
			return false;
		Shader vs{ .module = edgeVsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader ps{ .module = edgePsModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		edgeProgram = { .shaders = { vs, ps }, .shaderCount = 2 };
		vs = { .module = weightsVsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		ps = { .module = weightsPsModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		weightsProgram = { .shaders = { vs, ps }, .shaderCount = 2 };
		vs = { .module = nbVsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		ps = { .module = nbPsModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		nbProgram = { .shaders = { vs, ps }, .shaderCount = 2 };
		vs = { .module = resolveVsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		ps = { .module = resolvePsModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		resolveProgram = { .shaders = { vs, ps }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		edgeProgram = ShaderProgram{};
		weightsProgram = ShaderProgram{};
		nbProgram = ShaderProgram{};
		resolveProgram = ShaderProgram{};
		edgeVsModule = {}; edgePsModule = {};
		weightsVsModule = {}; weightsPsModule = {};
		nbVsModule = {}; nbPsModule = {};
		resolveVsModule = {}; resolvePsModule = {};
	}

	void DestroyTargets(RenderGraph& rg) {
		ldr.Destroy(rg);
		edges.Destroy(rg);
		weights.Destroy(rg);
		history[0].Destroy(rg);
		history[1].Destroy(rg);
		extent = { 0, 0 };
	}

	// Idempotent target + program creation (recreates only on extent or
	// ldr-format drift; e.g. resize). History content is lost on recreate -
	// the app must invalidate its historyValid flag at the same time.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D swapExtent,
		const SmaaDesc& d) {
		ldrFormat = d.ldrFormat;
		if (TargetsValid() && extent.width == swapExtent.width
			&& extent.height == swapExtent.height
			&& ldr.format == d.ldrFormat) {
			return EnsureShaders(ctx);
		}
		bool ok = true;
		ok = ldr.Create(ctx, rg, swapExtent, d.ldrFormat,
			"smaaLdr", "smaaLdrView", "smaaLdrLogical") && ok;
		ok = edges.Create(ctx, rg, swapExtent, Format::RGBA8_UNorm,
			"smaaEdges", "smaaEdgesView", "smaaEdgesLogical") && ok;
		ok = weights.Create(ctx, rg, swapExtent, Format::RGBA8_UNorm,
			"smaaWeights", "smaaWeightsView", "smaaWeightsLogical") && ok;
		ok = history[0].Create(ctx, rg, swapExtent, d.ldrFormat,
			"smaaHistory0", "smaaHistory0View", "smaaHistory0Logical") && ok;
		ok = history[1].Create(ctx, rg, swapExtent, d.ldrFormat,
			"smaaHistory1", "smaaHistory1View", "smaaHistory1Logical") && ok;
		extent = swapExtent;
		ctx.GetOrCreateSampler(SamplerCreateInfo{});
		return EnsureShaders(ctx) && ok;
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyShaders();
		areaBindless = UINT32_MAX;
		searchBindless = UINT32_MAX;
		areaTex = {};
		searchTex = {};
	}

	// Emits the SMAA pass chain after the tonemap pass wrote ldr:
	//   SmaaEdge -> SmaaWeights -> SmaaNeighborhood [-> SmaaResolve].
	// t2x selects the temporal mode: the neighborhood pass writes
	// history[historySlot] and the resolve combines it with
	// history[historySlot^1] (historyValid gates the blend - see
	// smaa_resolve.frag.hlsl). With t2x == false (SMAA 1x) the neighborhood
	// pass writes the swapchain directly and no resolve is emitted.
	// jitterDeltaX/Y: the jitter component of the motion vectors
	// ((prevJitter - currJitter) in UV). The neighborhood pass strips it
	// from the packed velocity magnitude so the resolve's history
	// attenuation reacts to true motion only; zero for SMAA 1x.
	// swapTargetsAreSrgb: set when the swapchain surface format is an _SRGB
	// variant - the swapchain-writing passes then decode their gamma-space
	// result to linear (outputLinear) so the hardware store encode restores
	// it; with a non-sRGB surface the encoded bytes pass through raw.
	// Captures only copyable handles/indices, so the passes are safe once
	// this call returns.
	// Layer sub-rect (see Tonemapper::AddTonemapPass): offsetX/Y confine
	// the SWAP-WRITING passes (SMAA 1x neighborhood + T2x resolve) to the
	// element's rect. Both already use DontCare (no sibling wipe); the
	// edge/weights/history passes target internal images at passExtent and
	// stay full-extent. rtMetrics still describe passExtent.
	void AddSmaaPasses(RenderGraph& rg,
		LogicalImageViewHandle motionView,
		uint32_t motionBindless,
		uint32_t linearSamplerIndex,
		uint32_t pointSamplerIndex,
		bool t2x,
		bool historyValid,
		const float subsampleIndices[4],
		uint32_t historySlot,
		const std::vector<LogicalImageViewHandle>& swapViews,
		Extent2D passExtent,
		bool swapTargetsAreSrgb = false,
		float jitterDeltaX = 0.0f,
		float jitterDeltaY = 0.0f,
		const char* name = nullptr,
		uint32_t offsetX = 0,
		uint32_t offsetY = 0) const {
		assert(Valid());
		assert(LutsValid());
		assert(!ldr.logicalView.empty());
		assert(!motionView.empty() && motionBindless != UINT32_MAX);
		assert(!swapViews.empty());
		assert(passExtent.width != 0 && passExtent.height != 0);
		assert(historySlot < 2);

		const SmaaImageTarget& historyCurrent = history[historySlot];
		const SmaaImageTarget& historyPrevious = history[historySlot ^ 1];

		SmaaPush edgePush{};
		edgePush.colorTexIndex = ldr.bindlessIndex;
		edgePush.pointSamplerIndex = pointSamplerIndex;
		edgePush.rtMetricsX = 1.0f / float(passExtent.width);
		edgePush.rtMetricsY = 1.0f / float(passExtent.height);
		edgePush.rtMetricsZ = float(passExtent.width);
		edgePush.rtMetricsW = float(passExtent.height);

		{
			RenderPass& pass = rg.AddPass(name ? name : "SmaaEdge");
			pass.SetOffset(0, 0)
				.SetExtent(passExtent)
				.SetInputImages({ ldr.logicalView })
				.SetColor({ edges.logicalView },
					AttachmentLoadOp::Clear,
					AttachmentStoreOp::Store,
					ClearColor(0, 0, 0, 0)); // edges (and alpha) must be cleared
			ShaderProgram progCopy = edgeProgram;
			pass.SetRecordingFunc([progCopy, edgePush](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(progCopy, VertexFormat::Empty,
					DepthSettings::Disabled(), StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(), MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SmaaPush), &edgePush, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		SmaaPush weightsPush{};
		weightsPush.edgesTexIndex = edges.bindlessIndex;
		weightsPush.areaTexIndex = areaBindless;
		weightsPush.searchTexIndex = searchBindless;
		weightsPush.linearSamplerIndex = linearSamplerIndex;
		weightsPush.subsampleIdxX = subsampleIndices[0];
		weightsPush.subsampleIdxY = subsampleIndices[1];
		weightsPush.subsampleIdxZ = subsampleIndices[2];
		weightsPush.subsampleIdxW = subsampleIndices[3];
		weightsPush.rtMetricsX = edgePush.rtMetricsX;
		weightsPush.rtMetricsY = edgePush.rtMetricsY;
		weightsPush.rtMetricsZ = edgePush.rtMetricsZ;
		weightsPush.rtMetricsW = edgePush.rtMetricsW;

		{
			RenderPass& pass = rg.AddPass(name ? name : "SmaaWeights");
			pass.SetOffset(0, 0)
				.SetExtent(passExtent)
				.SetInputImages({ edges.logicalView })
				.SetColor({ weights.logicalView },
					AttachmentLoadOp::Clear,
					AttachmentStoreOp::Store,
					ClearColor(0, 0, 0, 0)); // weights (and alpha) must be cleared
			ShaderProgram progCopy = weightsProgram;
			pass.SetRecordingFunc([progCopy, weightsPush](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(progCopy, VertexFormat::Empty,
					DepthSettings::Disabled(), StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(), MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SmaaPush), &weightsPush, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0);
			});
		}

		SmaaPush nbPush{};
		nbPush.colorTexIndex = ldr.bindlessIndex;
		nbPush.blendTexIndex = weights.bindlessIndex;
		nbPush.velocityTexIndex = motionBindless;
		nbPush.linearSamplerIndex = linearSamplerIndex;
		nbPush.rtMetricsX = edgePush.rtMetricsX;
		nbPush.rtMetricsY = edgePush.rtMetricsY;
		nbPush.rtMetricsZ = edgePush.rtMetricsZ;
		nbPush.rtMetricsW = edgePush.rtMetricsW;
		nbPush.jitterDeltaX = jitterDeltaX;
		nbPush.jitterDeltaY = jitterDeltaY;
		nbPush.outputLinear = (!t2x && swapTargetsAreSrgb) ? 1u : 0u;

		{
			// T2x: write this frame's subsample into the history ping-pong
			// slot (alpha carries the packed velocity for the resolve).
			// SMAA 1x: straight to the swapchain (alpha ignored).
			RenderPass& pass = rg.AddPass(name ? name : "SmaaNeighborhood");
			const bool toSwap = !t2x;
			pass.SetOffset(toSwap ? offsetX : 0, toSwap ? offsetY : 0)
				.SetExtent(passExtent)
				.SetInputImages({ ldr.logicalView, weights.logicalView, motionView })
				.SetColor({ t2x ? historyCurrent.logicalView : swapViews[0] },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = nbProgram;
			pass.SetRecordingFunc([progCopy, nbPush](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(progCopy, VertexFormat::Empty,
					DepthSettings::Disabled(), StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(), MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SmaaPush), &nbPush, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0);
			});
		}

		if (t2x) {
			SmaaPush resolvePush{};
			resolvePush.currentTexIndex = historyCurrent.bindlessIndex;
			resolvePush.previousTexIndex = historyPrevious.bindlessIndex;
			resolvePush.velocityTexIndex = motionBindless;
			resolvePush.pointSamplerIndex = pointSamplerIndex;
			resolvePush.historyValid = historyValid ? 1u : 0u;
			resolvePush.outputLinear = swapTargetsAreSrgb ? 1u : 0u;

			RenderPass& pass = rg.AddPass(name ? name : "SmaaResolve");
			pass.SetOffset(offsetX, offsetY)
				.SetExtent(passExtent)
				.SetInputImages({ historyCurrent.logicalView,
					historyPrevious.logicalView, motionView })
				.SetColor(swapViews,
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = resolveProgram;
			pass.SetRecordingFunc([progCopy, resolvePush](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(progCopy, VertexFormat::Empty,
					DepthSettings::Disabled(), StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(), MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SmaaPush), &resolvePush, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0);
			});
		}
	}
};

} // namespace smvk
