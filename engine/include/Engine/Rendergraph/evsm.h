#pragma once
// EVSM (Exponential Variance Shadow Maps) atlas with async-compute prefilter.
//
// Design (matches the request: 32-bit depth, 2x MSAA, async prefilter, pipelined):
//  - Moments are rendered as RGBA32F color (4x 32-bit float: first AND second
//    moments of both exponential warps — E[exp(+c*d)], E[exp(+2c*d)],
//    E[exp(-c*d)], E[exp(-2c*d)]; the seconds are squares at bake time, see
//    evsm.hlsli) with 2x MSAA + resolve. Depth testing still uses a 2x MSAA
//    D32 depth buffer; the pixel shader writes the warped moments. This gives
//    real MSAA on the depth signal (unlike a post-resolve depth read) and
//    keeps the single-sample resolved moments filterable. Four moments are
//    what the dual Cantelli visibility needs: a single first moment per warp
//    cannot separate "mass in front of the receiver" from "mass at/behind
//    it", which is exactly the leak/acne failure of the old 2-channel form.
//  - Prefilter is a separable Gaussian (H then V) in compute, marked
//    RenderPass::QueueHint::AsyncCompute. EvsmAsyncPrefilter routes those
//    dispatches onto the dedicated compute queue (RHI compute submitter) with
//    queue-family ownership transfers; on single-queue devices it degrades to
//    same-queue compute (emulated async, still pipelined H->V).
//  - Pipelining: default is ONE-FRAME LAG. Graphics renders moments for frame
//    N and resolves them; async compute blurs frame N-1's moments while
//    graphics works; lighting samples the blurred result of frame N-1.
//    Set PipelineMode::SameFrame to blur + sample within one frame (the
//    transition is a single enum flip: blur reads momentsCurrent instead of
//    momentsHistory, no layout change).
//
// One EvsmAtlas backs either the directional CSM tiles or the spot tiles —
// the tile UV convention (uvScale/uvBias/uvClamp) is identical to
// CascadedShadowAtlas / SpotShadowAtlas so existing lightVP math is reused.
// Both atlases can be EVSM at once (two instances).
//
// Frame indexing: moments + blurred outputs are ringed by MAX_FRAMES_IN_FLIGHT.
// With OneFrameLag, sample index = (writeIndex + FLIGHTS - 1) % FLIGHTS.

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/smvk.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace smvk {

// EVSM warp coefficient (Lauritzen & McCool 2008, Sec. 3): depth is warped as
// exp(+-c*d) with c ~ 40-80 for [0,1] NDC depth. Larger c tightens light
// bleeding but risks fp32 overflow in the blur; with second moments the
// ceiling is the positive M2: exp(2c) must fit fp32 (exp(84) ~ 3.3e36 at
// c=42, comfortable headroom under FLT_MAX 3.4e38).
inline constexpr float kEvsmExponent = 42.0f;
// Separable Gaussian taps for the prefilter (9-tap dense, sigma ~ radius/2).
// Blur runs in tile-local TEXEL space with clamp-to-tile so cascades/tiles
// never bleed into siblings.
inline constexpr uint32_t kEvsmBlurRadius = 2u;
// Blur workgroup edge — matches EVSM_BLUR_BLOCK / [numthreads(16,16,1)] in
// evsm_blur.hlsli; the dispatch tiling below rounds tile extents up to it.
inline constexpr uint32_t kEvsmBlurBlock = 16u;

enum class EvsmPipelineMode : uint8_t {
	// Sample previous frame's blurred moments while blurring the current
	// frame's moments on the async queue (max overlap, 1 frame of lag on
	// the shadow filter only — geometry/depth stay current).
	OneFrameLag = 0,
	// Blur + sample the same frame's moments (more graphics idle, no lag).
	// Flip this enum to transition; resource layout is identical.
	SameFrame = 1,
};

struct EvsmDesc {
	uint32_t tileResolution = 1024; // per-tile res (square)
	uint32_t tileCount = 4;         // cascades or spot slots in this atlas
	Format momentsFormat = Format::RGBA32_Float; // 4x32-bit moments
	Format depthFormat = Format::D32_Float;    // MSAA depth for the moment pass
	uint32_t msaaSamples = 2;       // 2x MSAA for depth (+moments)
	float exponent = kEvsmExponent; // warp coefficient c
	uint32_t blurRadius = kEvsmBlurRadius;
	EvsmPipelineMode pipeline = EvsmPipelineMode::OneFrameLag;
	const char* name = "evsmAtlas";
};

inline constexpr const char* kEvsmMomentsVsPath = "content/shaders/shadow.vert.hlsl.spv";
inline constexpr const char* kEvsmMomentsFsPath = "content/shaders/shadow_evsm.frag.hlsl.spv";
inline constexpr const char* kEvsmMomentsOpaqueFsPath = "content/shaders/shadow_evsm_opaque.frag.hlsl.spv";
inline constexpr const char* kEvsmBlurHPath = "content/shaders/evsm_blur_h.cs.hlsl.spv";
inline constexpr const char* kEvsmBlurVPath = "content/shaders/evsm_blur_v.cs.hlsl.spv";

// Push constants for the EVSM blur legs (evsm_blur_h/v.cs.hlsl share
// evsm_blur.hlsli). Must mirror the HLSL EvsmBlurPush (48 bytes, DX layout).
struct EvsmBlurPush {
	uint32_t srcIndex;      // bindless textures[] index this leg Load()s from
	uint32_t dstIndex;      // bindless storage-image index this leg writes to
	uint32_t tileOffsetX;   // tile origin in atlas texels (edge clamp min)
	uint32_t tileOffsetY;
	uint32_t tileWidth;     // tile extent in texels (early-out + edge clamp max)
	uint32_t tileHeight;
	float w0;               // CPU-baked normalized symmetric Gaussian weights:
	float w1;               // sigma = clamp(blurRadius/2, 1, 3),
	float w2;               // w_k = exp(-k^2 / (2 sigma^2)) / wSum over taps
	float w3;               // -4..+4 — the shader never evaluates exp()
	float w4;
	uint32_t pad0;
};
static_assert(sizeof(EvsmBlurPush) == 48, "EVSM blur push must mirror the HLSL EvsmBlurPush (48 bytes)");

// Per-frame resolved + blurred views. Ringed by flight slot; with OneFrameLag
// the sampler reads slot (write+FLIGHTS-1)%FLIGHTS.
struct EvsmAtlas {
	// MSAA moment + depth targets (transient per frame, resolved each tile).
	Holder<ImageHandle> msaaMoments;
	Holder<ImageViewHandle> msaaMomentsView;
	Holder<ImageHandle> msaaDepth;
	Holder<ImageViewHandle> msaaDepthView;
	LogicalImageViewHandle msaaMomentsLogical{};
	LogicalImageViewHandle msaaDepthLogical{};
	// Single-sample moments (resolve dest, blur source). One per flight.
	Holder<ImageHandle> moments[MAX_FRAMES_IN_FLIGHT]{};
	Holder<ImageViewHandle> momentsView[MAX_FRAMES_IN_FLIGHT]{};
	Holder<BindlessImageHandle> momentsBindless[MAX_FRAMES_IN_FLIGHT]{};
	LogicalImageViewHandle momentsLogical[MAX_FRAMES_IN_FLIGHT]{};
	// Horizontal-blur scratch (compute General) + final blurred output.
	Holder<ImageHandle> blurTemp[MAX_FRAMES_IN_FLIGHT]{};
	Holder<ImageViewHandle> blurTempView[MAX_FRAMES_IN_FLIGHT]{};
	Holder<BindlessImageHandle> blurTempBindless[MAX_FRAMES_IN_FLIGHT]{};
	LogicalImageViewHandle blurTempLogical[MAX_FRAMES_IN_FLIGHT]{};
	Holder<ImageHandle> blurred[MAX_FRAMES_IN_FLIGHT]{};
	Holder<ImageViewHandle> blurredView[MAX_FRAMES_IN_FLIGHT]{};
	Holder<BindlessImageHandle> blurredBindless[MAX_FRAMES_IN_FLIGHT]{};
	LogicalImageViewHandle blurredLogical[MAX_FRAMES_IN_FLIGHT]{};

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fsModule;
	Holder<ShaderModuleHandle> fsOpaqueModule;
	Holder<ShaderModuleHandle> blurHModule;
	Holder<ShaderModuleHandle> blurVModule;
	ShaderProgram momentsProgram{};      // VS+FS full: alpha-tested (MASK casters)
	ShaderProgram momentsOpaqueProgram{}; // VS+FS cheap: opaque casters (no material path)
	ShaderProgram blurHProgram{};   // CS: horizontal leg (moments -> temp)
	ShaderProgram blurVProgram{};   // CS: vertical leg (temp -> blurred)

	Extent2D atlasExtent{ 0, 0 };
	Extent2D tileExtent{ 0, 0 };
	uint32_t tileCount = 0;
	Format momentsFormat = Format::RG32_Float;
	EvsmPipelineMode pipeline = EvsmPipelineMode::OneFrameLag;
	float exponent = kEvsmExponent;
	uint32_t blurRadius = kEvsmBlurRadius;
	uint32_t writeSlot = 0; // flight slot moments were last resolved into

	[[nodiscard]] bool Valid() const {
		return !msaaMoments.empty() && tileCount != 0 && !moments[0].empty()
			&& !msaaMomentsLogical.empty() && !blurTempBindless[0].empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return momentsProgram.shaderCount == 2
			&& blurHProgram.shaderCount == 1 && blurVProgram.shaderCount == 1
			&& momentsOpaqueProgram.shaderCount == 2;
	}

	// Frame-lag helpers: write slot = current flight; sample slot trails by
	// one when pipelined. SameFrame collapses both to the current slot so
	// the caller transition is a single mode flip.
	[[nodiscard]] uint32_t SampleSlot(uint32_t flight) const {
		if (pipeline == EvsmPipelineMode::SameFrame)
			return flight % MAX_FRAMES_IN_FLIGHT;
		return (flight + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
	}

	[[nodiscard]] uint32_t Columns() const {
		assert(tileCount != 0);
		return static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(tileCount))));
	}

	// Tile origin in texels for tile i (same column-major convention as the
	// CascadedShadowAtlas / SpotShadowAtlas this EVSM atlas shadows).
	void TileOrigin(uint32_t index, int32_t& outX, int32_t& outY) const {
		assert(index < tileCount);
		const uint32_t cols = Columns();
		outX = static_cast<int32_t>((index % cols) * tileExtent.width);
		outY = static_cast<int32_t>((index / cols) * tileExtent.height);
	}

	int32_t MomentsBindlessIndex(ResourceManager& ctx, uint32_t flight) const {
		if (momentsBindless[flight % MAX_FRAMES_IN_FLIGHT].empty())
			return -1;
		return static_cast<int32_t>(ctx.GetBindlessIndex(momentsBindless[flight % MAX_FRAMES_IN_FLIGHT]));
	}
	int32_t BlurredBindlessIndex(ResourceManager& ctx, uint32_t sampleFlight) const {
		const uint32_t s = SampleSlot(sampleFlight);
		if (blurredBindless[s].empty())
			return -1;
		return static_cast<int32_t>(ctx.GetBindlessIndex(blurredBindless[s]));
	}
	int32_t TempBindlessIndex(ResourceManager& ctx, uint32_t flight) const {
		if (blurTempBindless[flight % MAX_FRAMES_IN_FLIGHT].empty())
			return -1;
		return static_cast<int32_t>(ctx.GetBindlessIndex(blurTempBindless[flight % MAX_FRAMES_IN_FLIGHT]));
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kEvsmMomentsVsPath,
		const char* fsPath = kEvsmMomentsFsPath,
		const char* csHPath = kEvsmBlurHPath,
		const char* csVPath = kEvsmBlurVPath,
		const char* fsOpaquePath = kEvsmMomentsOpaqueFsPath) {
		if (ShadersValid())
			return true;
		try {
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vsPath ? vsPath : kEvsmMomentsVsPath), "evsmMomentsVS");
			if (fsModule.empty())
				fsModule = ctx.CreateShaderModule(readFile(fsPath ? fsPath : kEvsmMomentsFsPath), "evsmMomentsFS");
			if (fsOpaqueModule.empty())
				fsOpaqueModule = ctx.CreateShaderModule(readFile(fsOpaquePath ? fsOpaquePath : kEvsmMomentsOpaqueFsPath), "evsmMomentsOpaqueFS");
			if (blurHModule.empty())
				blurHModule = ctx.CreateShaderModule(readFile(csHPath ? csHPath : kEvsmBlurHPath), "evsmBlurHCS");
			if (blurVModule.empty())
				blurVModule = ctx.CreateShaderModule(readFile(csVPath ? csVPath : kEvsmBlurVPath), "evsmBlurVCS");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || fsModule.empty() || fsOpaqueModule.empty()
			|| blurHModule.empty() || blurVModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader fs{ .module = fsModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader fsOpaque{ .module = fsOpaqueModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader csH{ .module = blurHModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		Shader csV{ .module = blurVModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		momentsProgram = { .shaders = { vs, fs }, .shaderCount = 2 };
		momentsOpaqueProgram = { .shaders = { vs, fsOpaque }, .shaderCount = 2 };
		blurHProgram = { .shaders = { csH }, .shaderCount = 1 };
		blurVProgram = { .shaders = { csV }, .shaderCount = 1 };
		return true;
	}

	void Destroy(RenderGraph& rg) {
		if (!msaaMomentsLogical.empty())
			rg.FreeImageView(msaaMomentsLogical);
		if (!msaaDepthLogical.empty())
			rg.FreeImageView(msaaDepthLogical);
		msaaMomentsLogical = {};
		msaaDepthLogical = {};
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			if (!blurredLogical[f].empty())
				rg.FreeImageView(blurredLogical[f]);
			if (!momentsLogical[f].empty())
				rg.FreeImageView(momentsLogical[f]);
			if (!blurTempLogical[f].empty())
				rg.FreeImageView(blurTempLogical[f]);
			blurTempLogical[f] = {};
			blurredLogical[f] = {};
			momentsLogical[f] = {};
			blurredBindless[f] = {};
			blurredView[f] = {};
			blurred[f] = {};
			blurTempBindless[f] = {};
			blurTempView[f] = {};
			blurTemp[f] = {};
			momentsBindless[f] = {};
			momentsView[f] = {};
			moments[f] = {};
		}
		msaaDepthView = {};
		msaaDepth = {};
		msaaMomentsView = {};
		msaaMoments = {};
		atlasExtent = { 0, 0 };
		tileExtent = { 0, 0 };
		tileCount = 0;
		writeSlot = 0;
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, const EvsmDesc& desc) {
		if (desc.tileCount == 0 || desc.tileResolution == 0)
			return false;
		const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(desc.tileCount))));
		const uint32_t rows = (desc.tileCount + cols - 1) / cols;
		const Extent2D atlas{ desc.tileResolution * cols, desc.tileResolution * rows };
		const SampleCount samples = (desc.msaaSamples == 2) ? SampleCount::Count2 : SampleCount::Count1;

		ImageCreateInfo msaaCI{};
		msaaCI.format = desc.momentsFormat;
		msaaCI.extent = { atlas.width, atlas.height, 1u };
		msaaCI.samples = samples;
		msaaCI.usage = ImageUsage::ColorAttachment;
		if (DebugImageDumpArmed()) // atlas dump readback (VULKANTOY_EVSM_DUMP)
			msaaCI.usage |= ImageUsage::TransferSrc;
		msaaCI.name = desc.name;
		Holder<ImageHandle> newMsaa = ctx.CreateImage(msaaCI, nullptr);
		if (newMsaa.empty())
			return false;
		ImageViewCreateInfo msaaViewCI{};
		msaaViewCI.viewType = ImageViewType::View2D;
		msaaViewCI.subresourceRange = { .aspect = ImageAspect::Color };
		Holder<ImageViewHandle> newMsaaView = ctx.CreateImageView(newMsaa, msaaViewCI);
		if (newMsaaView.empty())
			return false;

		ImageCreateInfo depthCI{};
		depthCI.format = desc.depthFormat;
		depthCI.extent = { atlas.width, atlas.height, 1u };
		depthCI.samples = samples;
		depthCI.usage = ImageUsage::DepthStencilAttachment;
		depthCI.name = "evsm msaa depth";
		Holder<ImageHandle> newDepth = ctx.CreateImage(depthCI, nullptr);
		if (newDepth.empty())
			return false;
		ImageViewCreateInfo depthViewCI{};
		depthViewCI.viewType = ImageViewType::View2D;
		depthViewCI.subresourceRange = { .aspect = ImageAspect::Depth };
		Holder<ImageViewHandle> newDepthView = ctx.CreateImageView(newDepth, depthViewCI);
		if (newDepthView.empty())
			return false;

		// Per-flight single-sample moments + blur ping-pong. Storage-capable
		// so the async compute prefilter can write them (General), sampled
		// so lighting + the next blur tap can read them, TransferSrc so the
		// opt-in atlas dump (VULKANTOY_EVSM_DUMP, EvsmDumpAtlas) can copy
		// them back — armed dumps only. CONCURRENT graphics+compute: the
		// moment resolve (graphics) and the blur (compute queue) ping-pong
		// these images every frame, so no ownership transfers — events +
		// layout barriers order the queues.
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			ImageCreateInfo singleCI{};
			singleCI.format = desc.momentsFormat;
			singleCI.extent = { atlas.width, atlas.height, 1u };
			singleCI.usage = ImageUsage::Sampled | ImageUsage::Storage | ImageUsage::TransferDst | ImageUsage::ColorAttachment;
			if (DebugImageDumpArmed())
				singleCI.usage |= ImageUsage::TransferSrc;
			singleCI.concurrentCompute = true;
			singleCI.name = desc.name;
			moments[f] = ctx.CreateImage(singleCI, nullptr);
			blurTemp[f] = ctx.CreateImage(singleCI, nullptr);
			blurred[f] = ctx.CreateImage(singleCI, nullptr);
			if (moments[f].empty() || blurTemp[f].empty() || blurred[f].empty()) {
				Destroy(rg);
				msaaMomentsView = {};
				msaaMoments = {};
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
			ImageViewCreateInfo vci{};
			vci.viewType = ImageViewType::View2D;
			vci.subresourceRange = { .aspect = ImageAspect::Color };
			momentsView[f] = ctx.CreateImageView(moments[f], vci);
			blurTempView[f] = ctx.CreateImageView(blurTemp[f], vci);
			blurredView[f] = ctx.CreateImageView(blurred[f], vci);
			if (momentsView[f].empty() || blurTempView[f].empty() || blurredView[f].empty()) {
				Destroy(rg);
				msaaMomentsView = {};
				msaaMoments = {};
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
			momentsBindless[f] = ctx.CreateBindlessImage(momentsView[f], ImageLayout::ShaderReadOnlyOptimal);
			blurTempBindless[f] = ctx.CreateBindlessImage(blurTempView[f], ImageLayout::General);
			blurredBindless[f] = ctx.CreateBindlessImage(blurredView[f], ImageLayout::ShaderReadOnlyOptimal);
			if (momentsBindless[f].empty() || blurTempBindless[f].empty() || blurredBindless[f].empty()) {
				Destroy(rg);
				msaaMomentsView = {};
				msaaMoments = {};
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
			momentsLogical[f] = rg.AddExternalImageView(momentsView[f], desc.momentsFormat, "evsmMoments");
			blurTempLogical[f] = rg.AddExternalImageView(blurTempView[f], desc.momentsFormat, "evsmTemp");
			blurredLogical[f] = rg.AddExternalImageView(blurredView[f], desc.momentsFormat, "evsmBlurred");
			if (momentsLogical[f].empty() || blurTempLogical[f].empty() || blurredLogical[f].empty()) {
				Destroy(rg);
				msaaMomentsView = {};
				msaaMoments = {};
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
		}

		msaaMoments = std::move(newMsaa);
		msaaMomentsView = std::move(newMsaaView);
		msaaDepth = std::move(newDepth);
		msaaDepthView = std::move(newDepthView);
		msaaMomentsLogical = rg.AddExternalImageView(msaaMomentsView, desc.momentsFormat, "evsmMsaaMoments");
		msaaDepthLogical = rg.AddExternalImageView(msaaDepthView, desc.depthFormat, "evsmMsaaDepth");
		if (msaaMomentsLogical.empty() || msaaDepthLogical.empty()) {
			Destroy(rg);
			return false;
		}
		atlasExtent = atlas;
		tileExtent = { desc.tileResolution, desc.tileResolution };
		tileCount = desc.tileCount;
		momentsFormat = desc.momentsFormat;
		pipeline = desc.pipeline;
		exponent = desc.exponent;
		blurRadius = desc.blurRadius;
		return EnsureShaders(ctx);
	}

	bool Ensure(ResourceManager& ctx, RenderGraph& rg, const EvsmDesc& desc) {
		if (Valid() && tileCount == desc.tileCount
			&& tileExtent.width == desc.tileResolution
			&& momentsFormat == desc.momentsFormat) {
			pipeline = desc.pipeline;
			exponent = desc.exponent;
			blurRadius = desc.blurRadius;
			return EnsureShaders(ctx);
		}
		Destroy(rg);
		return Create(ctx, rg, desc);
	}
};

// Async prefilter routing (pipelined, AsyncCompute-marked).
// The rendergraph records moment passes on graphics; the blur H/V passes are
// marked AsyncCompute and Execute() routes them onto the dedicated compute
// queue as the C leg of the G1/C/G2 split (timeline-chained). The C stream is
// ONE SUBMIT PER ASYNC PASS on the asyncG2C timeline: pass k waits pass k-1's
// chain value (G1's signal for pass 0) and signals its own, so H's temp
// stores fully land before V's fetches — the hard dependency chained compute
// tasks need without a rendergraph-wide dependency solve. With OneFrameLag
// the blur reads the just-resolved current slot and lighting samples the
// previous slot; SameFrame collapses both.
struct EvsmAsyncPrefilter {
	// Moment pass for one tile: MSAA RG32F moments + MSAA D32 depth, color
	// resolved into moments[flight]. The caller sets the recording func
	// (bind momentsProgram with 2x MSAA + draw casters). Every tile clears
	// its own MSAA area; the resolve stores into the single-sample image.
	static RenderPass& AddMomentPass(RenderGraph& rg, const EvsmAtlas& atlas,
		uint32_t tileIndex, uint32_t flight, const char* name = nullptr) {
		assert(atlas.Valid());
		assert(tileIndex < atlas.tileCount);
		int32_t offX = 0, offY = 0;
		atlas.TileOrigin(tileIndex, offX, offY);
		const uint32_t slot = flight % MAX_FRAMES_IN_FLIGHT;
		// Uncleared texels must read as "far occluder" (moments of d=1),
		// not lit — matches EvsmClearColor() in evsm.hlsli. All four
		// moments of the far-plane warp: (e^{+c}, e^{+2c}, e^{-c}, e^{-2c}).
		const float c = atlas.exponent;
		const float wp = std::exp(c);
		const float wn = std::exp(-c);
		ClearColor momentsClear{};
		momentsClear.r = wp;
		momentsClear.g = wp * wp;
		momentsClear.b = wn;
		momentsClear.a = wn * wn;
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(offX, offY)
			.SetExtent(atlas.tileExtent)
			.SetColorWithResolve(
				{ atlas.msaaMomentsLogical },
				{ atlas.momentsLogical[slot] },
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::DontCare,
				momentsClear)
			.SetDepth(
				atlas.msaaDepthLogical,
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::DontCare,
				ClearDepthStencil(1.0f, 0));
		return pass;
	}

	// Builds the blur push for one tile/leg. Edge clamping lives in TEXEL
	// space — [tileOffset, tileOffset + tileExtent - 1] — bit-identical to
	// the old half-texel-inset UV clamp (the center of the first/last
	// texel), so sibling tiles still never bleed. Weights: the Gaussian is
	// dispatch-uniform, so bake the five normalized symmetric weights here
	// (sigma mapping unchanged from the old shader-side derivation —
	// radius/2 clamped to [1, 3]; only WHERE it evaluates moved, CPU exp
	// vs 9 per-tap GPU exp per leg).
	static EvsmBlurPush MakeBlurPush(const EvsmAtlas& atlas,
		uint32_t tileIndex, uint32_t srcBindless, uint32_t dstBindless) {
		assert(tileIndex < atlas.tileCount);
		int32_t offX = 0, offY = 0;
		atlas.TileOrigin(tileIndex, offX, offY);
		EvsmBlurPush p{};
		p.srcIndex = srcBindless;
		p.dstIndex = dstBindless;
		p.tileOffsetX = static_cast<uint32_t>(offX);
		p.tileOffsetY = static_cast<uint32_t>(offY);
		p.tileWidth = atlas.tileExtent.width;
		p.tileHeight = atlas.tileExtent.height;
		const float sigma = std::clamp(atlas.blurRadius * 0.5f, 1.0f, 3.0f);
		const float inv2s2 = 0.5f / (sigma * sigma);
		float w[5] = {};
		float wSum = 0.0f;
		for (uint32_t k = 0; k < 5; ++k) {
			w[k] = std::exp(-static_cast<float>(k * k) * inv2s2);
			wSum += (k == 0) ? w[0] : 2.0f * w[k];
		}
		for (float& wk : w)
			wk /= wSum;
		p.w0 = w[0];
		p.w1 = w[1];
		p.w2 = w[2];
		p.w3 = w[3];
		p.w4 = w[4];
		p.pad0 = 0;
		return p;
	}

	// Full-tile blur pair for one tile: H (moments -> temp) then V
	// (temp -> blurred), both AsyncCompute-marked, one entry point per leg
	// (atlas.blurHProgram / blurVProgram — the legs share evsm_blur.hlsli).
	// Recording funcs are set here (bind program + push + dispatch); the
	// caller only supplies the rendergraph + context. Barriers come from
	// the image hints.
	static void AddTileBlurPasses(RenderGraph& rg, const EvsmAtlas& atlas,
		ResourceManager& ctx, uint32_t flight, uint32_t tileIndex,
		const char* nameH = nullptr, const char* nameV = nullptr) {
		assert(atlas.Valid());
		const uint32_t slot = flight % MAX_FRAMES_IN_FLIGHT;
		const int32_t srcIdx = atlas.MomentsBindlessIndex(ctx, slot);
		const int32_t tmpIdx = atlas.TempBindlessIndex(ctx, slot);
		const int32_t dstIdx = static_cast<int32_t>(ctx.GetBindlessIndex(atlas.blurredBindless[slot]));
		assert(srcIdx >= 0 && tmpIdx >= 0 && dstIdx >= 0);

		ImageSyncRequest srcR{};
		srcR.barrierMasks.accessMask = AccessFlags::ShaderRead;
		srcR.barrierMasks.stageMask = PipelineStageBit_Compute;
		srcR.layout = ImageLayout::ShaderReadOnlyOptimal;
		ImageSyncRequest dstW{};
		dstW.barrierMasks.accessMask = AccessFlags::ShaderWrite;
		dstW.barrierMasks.stageMask = PipelineStageBit_Compute;
		dstW.layout = ImageLayout::General;

		const EvsmBlurPush pushH = MakeBlurPush(atlas, tileIndex,
			static_cast<uint32_t>(srcIdx), static_cast<uint32_t>(tmpIdx));
		RenderPass& hPass = rg.AddComputePass(nameH ? nameH : "EvsmBlurH");
		hPass.MarkAsAsyncCompute();
		hPass.AddImageHint(atlas.momentsLogical[slot], srcR);
		hPass.AddImageHint(atlas.blurTempLogical[slot], dstW);
		// Capture the program pointer + push by value: Execute() runs the
		// lambda after this function returns (per-frame record).
		ShaderProgram* blurHProg = const_cast<ShaderProgram*>(&atlas.blurHProgram);
		const uint32_t tw = atlas.tileExtent.width, th = atlas.tileExtent.height;
		hPass.SetRecordingFunc([blurHProg, pushH, tw, th](CommandBuffer& cmd) {
			cmd.BindComputePipeline(*blurHProg);
			cmd.PushConstants(0, sizeof(pushH), &pushH, ShaderStage::Compute);
			cmd.Dispatch((tw + kEvsmBlurBlock - 1) / kEvsmBlurBlock,
				(th + kEvsmBlurBlock - 1) / kEvsmBlurBlock, 1);
		});

		const EvsmBlurPush pushV = MakeBlurPush(atlas, tileIndex,
			static_cast<uint32_t>(tmpIdx), static_cast<uint32_t>(dstIdx));
		RenderPass& vPass = rg.AddComputePass(nameV ? nameV : "EvsmBlurV");
		vPass.MarkAsAsyncCompute();
		vPass.AddImageHint(atlas.blurTempLogical[slot], srcR);
		vPass.AddImageHint(atlas.blurredLogical[slot], dstW);
		ShaderProgram* blurVProg = const_cast<ShaderProgram*>(&atlas.blurVProgram);
		vPass.SetRecordingFunc([blurVProg, pushV, tw, th](CommandBuffer& cmd) {
			cmd.BindComputePipeline(*blurVProg);
			cmd.PushConstants(0, sizeof(pushV), &pushV, ShaderStage::Compute);
			cmd.Dispatch((tw + kEvsmBlurBlock - 1) / kEvsmBlurBlock,
				(th + kEvsmBlurBlock - 1) / kEvsmBlurBlock, 1);
		});
	}
};

} // namespace smvk
