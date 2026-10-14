#pragma once
// EVSM (Exponential Variance Shadow Maps) atlas with async-compute prefilter.
//
// Design (32-bit depth, 2x MSAA, async prefilter, pipelined):
//  - Casters render DEPTH ONLY at 2x MSAA into a D32 target — the SAME
//    depth-only programs the PCF shadow passes use (shadow.vert + the
//    depth-only opaque leg / alpha-tested shadow.frag leg), no MSAA color
//    attachment at all. The 2x MSAA DEPTH carries the whole MSAA payload:
//    at silhouette edges each sample holds its own coverage/depth, exactly
//    the distribution information EVSM wants.
//  - evsm_resolve.cs.hlsl then reconstructs the moments PER SAMPLE: load
//    both depth samples, warp EACH (exp is nonlinear — warp-then-average,
//    never average-then-warp), average into a single-sample RGBA32F
//    moments atlas (E[exp(+c*d)], E[exp(+2c*d)], E[exp(-c*d)], E[exp(-2c*d)]
//    — the seconds are squares at bake time, see evsm.hlsli). Replacing the
//    old 2x MSAA RGBA32F color + hardware resolve with 2x4B depth reads +
//    16B write per texel. Four moments are what the dual Cantelli
//    visibility needs: a single first moment per warp cannot separate
//    "mass in front of the receiver" from "mass at/behind it", which is
//    exactly the leak/acne failure of the old 2-channel form.
//  - Prefilter is a separable Gaussian (H then V) in compute, marked
//    RenderPass::QueueHint::AsyncCompute. EvsmAsyncPrefilter routes those
//    dispatches onto the dedicated compute queue; on single-queue devices
//    it degrades to same-queue compute (emulated async, still pipelined
//    H->V).
//  - Pipelining: default is ONE-FRAME LAG. Graphics renders depth +
//    resolves moments for frame N; async compute blurs frame N-1's
//    moments while graphics works; lighting samples the blurred result of
//    frame N-1. Set PipelineMode::SameFrame to blur + sample within one
//    frame (the transition is a single enum flip, no layout change).
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

// The moment pass reuses the PCF depth-only shadow programs (shadow.vert +
// shadow.frag — see RecordShadowCastersDirect); this atlas only owns the
// resolve + blur compute shaders.
inline constexpr const char* kEvsmResolveCsPath = "content/shaders/evsm_resolve.cs.hlsl.spv";
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

// Push constants for evsm_resolve.cs.hlsl. Must mirror the HLSL
// EvsmResolvePush (16 bytes, DX layout).
struct EvsmResolvePush {
	uint32_t srcIndex;    // bindless texturesMS[] index of the 2x MSAA depth view
	uint32_t dstIndex;    // bindless storage index of the slot image (moments-then-blurred, see EvsmAtlas)
	uint32_t atlasWidth;  // atlas extent in texels (one dispatch covers it)
	uint32_t atlasHeight;
};
static_assert(sizeof(EvsmResolvePush) == 16, "EVSM resolve push must mirror the HLSL EvsmResolvePush (16 bytes)");

// Per-frame resolved + blurred views. Ringed by flight slot; with OneFrameLag
// the sampler reads slot (write+FLIGHTS-1)%FLIGHTS.
struct EvsmAtlas {
	// 2x MSAA D32 depth target (transient per frame): the ONLY caster
	// attachment — the moment pass is depth-only; the per-sample resolve
	// reads it right after (texturesMS[] bindless + DepthReadOnly layout).
	Holder<ImageHandle> msaaDepth;
	Holder<ImageViewHandle> msaaDepthView;
	Holder<BindlessImageHandle> msaaDepthBindless;
	LogicalImageViewHandle msaaDepthLogical{};
	// ---- The SEPARABLE slot ping-pong (merged moments + blurred) -----
	// PER FLIGHT: ONE image per slot that carries moments (resolve
	// write, H read) and then, IN PLACE, the blurred output (V write
	// after H consumed the moments via the temp dependency). The old
	// per-flight x3 (moments + blurTemp + blurred) was 6 RGBA32F images
	// per atlas; moments and blurred never coexist on the same slot
	// (moments dies at H's last read, blurred is born at V's first
	// write), so the merge is lifetime-safe: 6 -> 2 slot images + 1
	// shared scratch = 3 images (~50% of the old atlas cost — the
	// evsmDir set alone drops ~768 MiB at 2048-res x4 cascades).
	Holder<ImageHandle> slotBuf[MAX_FRAMES_IN_FLIGHT]{};
	Holder<ImageViewHandle> slotView[MAX_FRAMES_IN_FLIGHT]{};
	Holder<BindlessImageHandle> slotBindless[MAX_FRAMES_IN_FLIGHT]{};
	LogicalImageViewHandle slotLogical[MAX_FRAMES_IN_FLIGHT]{};
	// ONE shared horizontal-blur scratch (compute General): dead
	// between frames, the rendergraph orders every use within a frame
	// (H writes it, V reads it; the slot dependency chains tiles). No
	// per-flight copies — the scratch has no cross-frame state.
	Holder<ImageHandle> blurTemp;
	Holder<ImageViewHandle> blurTempView;
	Holder<BindlessImageHandle> blurTempBindless;
	LogicalImageViewHandle blurTempLogical{};

	Holder<ShaderModuleHandle> resolveModule;
	Holder<ShaderModuleHandle> blurHModule;
	Holder<ShaderModuleHandle> blurVModule;
	ShaderProgram resolveProgram{};  // CS: per-sample MSAA depth -> moments
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
		return !msaaDepth.empty() && tileCount != 0 && !slotBuf[0].empty()
			&& !msaaDepthLogical.empty() && !msaaDepthBindless.empty()
			&& !blurTempBindless.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return resolveProgram.shaderCount == 1
			&& blurHProgram.shaderCount == 1 && blurVProgram.shaderCount == 1;
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

	// Bindless index of the slot image (moments-when-just-resolved /
	// blurred-when-lighting-samples — the SAME image, the lifetime
	// separability is the point; see the struct comment).
	int32_t SlotBindlessIndex(ResourceManager& ctx, uint32_t flight) const {
		if (slotBindless[flight % MAX_FRAMES_IN_FLIGHT].empty())
			return -1;
		return static_cast<int32_t>(ctx.GetBindlessIndex(slotBindless[flight % MAX_FRAMES_IN_FLIGHT]));
	}
	// The lighting side: same slot image, SampleSlot resolves the lag.
	int32_t BlurredBindlessIndex(ResourceManager& ctx, uint32_t sampleFlight) const {
		return SlotBindlessIndex(ctx, sampleFlight);
	}
	// Shared scratch (no flight param — it has no per-flight state).
	int32_t TempBindlessIndex(ResourceManager& ctx) const {
		if (blurTempBindless.empty())
			return -1;
		return static_cast<int32_t>(ctx.GetBindlessIndex(blurTempBindless));
	}
	// Bindless texturesMS[] index of the transient 2x MSAA depth view (the
	// per-sample resolve's source). -1 until the atlas is created.
	int32_t MsaaDepthBindlessIndex(ResourceManager& ctx) const {
		if (msaaDepthBindless.empty())
			return -1;
		return static_cast<int32_t>(ctx.GetBindlessIndex(msaaDepthBindless));
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* resolvePath = kEvsmResolveCsPath,
		const char* csHPath = kEvsmBlurHPath,
		const char* csVPath = kEvsmBlurVPath) {
		if (ShadersValid())
			return true;
		try {
			if (resolveModule.empty())
				resolveModule = ctx.CreateShaderModule(readFile(resolvePath ? resolvePath : kEvsmResolveCsPath), "evsmResolveCS");
			if (blurHModule.empty())
				blurHModule = ctx.CreateShaderModule(readFile(csHPath ? csHPath : kEvsmBlurHPath), "evsmBlurHCS");
			if (blurVModule.empty())
				blurVModule = ctx.CreateShaderModule(readFile(csVPath ? csVPath : kEvsmBlurVPath), "evsmBlurVCS");
		} catch (...) {
			return false;
		}
		if (resolveModule.empty() || blurHModule.empty() || blurVModule.empty())
			return false;
		Shader csResolve{ .module = resolveModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		Shader csH{ .module = blurHModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		Shader csV{ .module = blurVModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		resolveProgram = { .shaders = { csResolve }, .shaderCount = 1 };
		blurHProgram = { .shaders = { csH }, .shaderCount = 1 };
		blurVProgram = { .shaders = { csV }, .shaderCount = 1 };
		return true;
	}

	void Destroy(RenderGraph& rg) {
		if (!msaaDepthLogical.empty())
			rg.FreeImageView(msaaDepthLogical);
		msaaDepthLogical = {};
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			if (!slotLogical[f].empty())
				rg.FreeImageView(slotLogical[f]);
			slotLogical[f] = {};
			slotBindless[f] = {};
			slotView[f] = {};
			slotBuf[f] = {};
		}
		if (!blurTempLogical.empty())
			rg.FreeImageView(blurTempLogical);
		blurTempLogical = {};
		blurTempBindless = {};
		blurTempView = {};
		blurTemp = {};
		msaaDepthBindless = {};
		msaaDepthView = {};
		msaaDepth = {};
		atlasExtent = { 0, 0 };
		tileExtent = { 0, 0 };
		tileCount = 0;
		writeSlot = 0;
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, const EvsmDesc& desc) {
		if (desc.tileCount == 0 || desc.tileResolution == 0)
			return false;
		if (desc.msaaSamples != 2) {
			// evsm_resolve.cs.hlsl loads exactly two samples per texel.
			return false;
		}
		const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(desc.tileCount))));
		const uint32_t rows = (desc.tileCount + cols - 1) / cols;
		const Extent2D atlas{ desc.tileResolution * cols, desc.tileResolution * rows };
		const SampleCount samples = SampleCount::Count2;

		// The ONLY caster attachment: 2x MSAA D32 depth. Depth-only raster
		// (the PCF shadow programs), and the per-sample resolve reads it
		// right after through the bindless MSAA array — hence Sampled
		// usage too.
		ImageCreateInfo depthCI{};
		depthCI.format = desc.depthFormat;
		depthCI.extent = { atlas.width, atlas.height, 1u };
		depthCI.samples = samples;
		depthCI.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled;
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

		// ---- The SEPARABLE slot ping-pong (see the struct comment) ----
		// Per flight: ONE image (moments-then-blurred, in place). Storage
		// so the resolve + the async compute blur can write them (General),
		// sampled so lighting + the next blur tap can read them,
		// TransferSrc for the opt-in atlas dump (VULKANTOY_EVSM_DUMP) —
		// armed dumps only. CONCURRENT graphics+compute: the resolve
		// (graphics) and the blur (compute queue) ping-pong these images
		// every frame, so no ownership transfers — events + layout
		// barriers order the queues.
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
			ImageCreateInfo slotCI{};
			slotCI.format = desc.momentsFormat;
			slotCI.extent = { atlas.width, atlas.height, 1u };
			slotCI.usage = ImageUsage::Sampled | ImageUsage::Storage | ImageUsage::TransferDst;
			if (DebugImageDumpArmed())
				slotCI.usage |= ImageUsage::TransferSrc;
			slotCI.concurrentCompute = true;
			slotCI.name = desc.name;
			slotBuf[f] = ctx.CreateImage(slotCI, nullptr);
			if (slotBuf[f].empty()) {
				Destroy(rg);
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
			ImageViewCreateInfo vci{};
			vci.viewType = ImageViewType::View2D;
			vci.subresourceRange = { .aspect = ImageAspect::Color };
			slotView[f] = ctx.CreateImageView(slotBuf[f], vci);
			if (slotView[f].empty()) {
				Destroy(rg);
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
			slotBindless[f] = ctx.CreateBindlessImage(slotView[f], ImageLayout::ShaderReadOnlyOptimal);
			if (slotBindless[f].empty()) {
				Destroy(rg);
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
			slotLogical[f] = rg.AddExternalImageView(slotView[f], desc.momentsFormat, "evsmSlot");
			if (slotLogical[f].empty()) {
				Destroy(rg);
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
		}

		// ONE shared horizontal-blur scratch (compute General; the blur
		// writes it during H, V reads it — dead between frames, the
		// rendergraph chains every use within a frame).
		{
			ImageCreateInfo tempCI{};
			tempCI.format = desc.momentsFormat;
			tempCI.extent = { atlas.width, atlas.height, 1u };
			tempCI.usage = ImageUsage::Storage;
			tempCI.concurrentCompute = true;
			tempCI.name = "evsmTemp";
			blurTemp = ctx.CreateImage(tempCI, nullptr);
			if (blurTemp.empty()) {
				Destroy(rg);
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
			ImageViewCreateInfo tvci{};
			tvci.viewType = ImageViewType::View2D;
			tvci.subresourceRange = { .aspect = ImageAspect::Color };
			blurTempView = ctx.CreateImageView(blurTemp, tvci);
			if (blurTempView.empty()) {
				Destroy(rg);
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
			blurTempBindless = ctx.CreateBindlessImage(blurTempView, ImageLayout::General);
			if (blurTempBindless.empty()) {
				Destroy(rg);
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
			blurTempLogical = rg.AddExternalImageView(blurTempView, desc.momentsFormat, "evsmTemp");
			if (blurTempLogical.empty()) {
				Destroy(rg);
				msaaDepthView = {};
				msaaDepth = {};
				return false;
			}
		}

		msaaDepth = std::move(newDepth);
		msaaDepthView = std::move(newDepthView);
		msaaDepthBindless = ctx.CreateBindlessImage(msaaDepthView, ImageLayout::DepthReadOnlyOptimal);
		msaaDepthLogical = rg.AddExternalImageView(msaaDepthView, desc.depthFormat, "evsmMsaaDepth");
		if (msaaDepthBindless.empty() || msaaDepthLogical.empty()) {
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
	// Moment (caster) pass for one tile: DEPTH ONLY at 2x MSAA — the PCF
	// depth-only shadow programs (the caller's recording func binds them
	// with 2x MSAA + draws casters; see RecordShadowCastersDirect). Every
	// tile clears its own depth area to the far plane; the STORE matters:
	// the per-sample resolve (AddResolvePass) reads this depth right
	// after all moment passes. The far-plane DEPTH clear IS the moment
	// clear through the resolve's warp: uncovered/cleared samples read
	// z = 1.0 and resolve to warp(1) = the far-plane moment set (the old
	// EvsmClearColor color-clear convention, for free).
	static RenderPass& AddMomentPass(RenderGraph& rg, const EvsmAtlas& atlas,
		uint32_t tileIndex, uint32_t flight, const char* name = nullptr) {
		assert(atlas.Valid());
		assert(tileIndex < atlas.tileCount);
		int32_t offX = 0, offY = 0;
		atlas.TileOrigin(tileIndex, offX, offY);
		(void)flight;
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(offX, offY)
			.SetExtent(atlas.tileExtent)
			.SetDepth(
				atlas.msaaDepthLogical,
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::Store,
				ClearDepthStencil(1.0f, 0));
		return pass;
	}

	// Per-sample resolve (ONE per atlas per frame, after ALL moment
	// passes, before the blurs): evsm_resolve.cs.hlsl loads BOTH depth
	// samples of every atlas texel, warps each (warp-then-average — exp
	// is nonlinear), averages into the single-sample moments — which
	// LIVE IN the merged slot image (see the struct comment; the blur
	// V pass later overwrites the same slot with the blurred output).
	// Graphics-queue compute pass (not AsyncCompute-marked): it sorts
	// after every moment pass (reads the whole depth) and before every
	// blur (writes every slot texel), keeping the async blur window
	// free of graphics passes exactly as before.
	static void AddResolvePass(RenderGraph& rg, const EvsmAtlas& atlas,
		ResourceManager& ctx, uint32_t flight, const char* name = nullptr) {
		assert(atlas.Valid());
		const uint32_t slot = flight % MAX_FRAMES_IN_FLIGHT;
		const int32_t srcIdx = atlas.MsaaDepthBindlessIndex(ctx);
		const int32_t dstIdx = atlas.SlotBindlessIndex(ctx, slot);
		assert(srcIdx >= 0 && dstIdx >= 0);

		ImageSyncRequest depthRead{};
		depthRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		depthRead.barrierMasks.stageMask = PipelineStageBit_Compute;
		depthRead.layout = ImageLayout::DepthReadOnlyOptimal;
		ImageSyncRequest slotWrite{};
		slotWrite.barrierMasks.accessMask = AccessFlags::ShaderWrite;
		slotWrite.barrierMasks.stageMask = PipelineStageBit_Compute;
		slotWrite.layout = ImageLayout::General;

		EvsmResolvePush push{};
		push.srcIndex = static_cast<uint32_t>(srcIdx);
		push.dstIndex = static_cast<uint32_t>(dstIdx);
		push.atlasWidth = atlas.atlasExtent.width;
		push.atlasHeight = atlas.atlasExtent.height;

		RenderPass& pass = rg.AddComputePass(name ? name : "EvsmResolve");
		pass.AddImageHint(atlas.msaaDepthLogical, depthRead);
		pass.AddImageHint(atlas.slotLogical[slot], slotWrite);
		ShaderProgram* resolveProg = const_cast<ShaderProgram*>(&atlas.resolveProgram);
		const uint32_t aw = atlas.atlasExtent.width, ah = atlas.atlasExtent.height;
		pass.SetRecordingFunc([resolveProg, push, aw, ah](CommandBuffer& cmd) {
			// GPU stage timer (kGpuStageEvsmResolve): begin before the
			// dispatch, end after. One resolve per atlas per frame: with
			// BOTH atlases active the SECOND resolve's pair overwrites
			// the first's (identical-cost dispatches; the report shows
			// one resolve's cost, not their sum).
			cmd.WriteStageTimestamp(kGpuStageEvsmResolve, /*begin=*/true);
			cmd.BindComputePipeline(*resolveProg);
			cmd.PushConstants(0, sizeof(push), &push, ShaderStage::Compute);
			cmd.Dispatch((aw + 7) / 8, (ah + 7) / 8, 1);
			cmd.WriteStageTimestamp(kGpuStageEvsmResolve, /*begin=*/false);
		});
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

	// Full-tile blur pair for one tile: H (slot moments -> temp) then V
	// (temp -> slot blurred, IN PLACE — the same slot image H just read,
	// lifetime-safe because H fully consumed the moments before V's
	// first write; the temp dependency chains them), both
	// AsyncCompute-marked, one entry point per leg (atlas.blurHProgram /
	// blurVProgram — the legs share evsm_blur.hlsli). Recording funcs
	// are set here (bind program + push + dispatch); the caller only
	// supplies the rendergraph + context. Barriers come from the image
	// hints: H reads slotLogical, V WRITES slotLogical (the rendergraph
	// orders V after H via the shared temp dependency AND sees the
	// slot read-write for cross-tile ordering).
	static void AddTileBlurPasses(RenderGraph& rg, const EvsmAtlas& atlas,
		ResourceManager& ctx, uint32_t flight, uint32_t tileIndex,
		const char* nameH = nullptr, const char* nameV = nullptr) {
		assert(atlas.Valid());
		const uint32_t slot = flight % MAX_FRAMES_IN_FLIGHT;
		// H reads the slot's moments, V writes the slot's blurred — the
		// SAME bindless index (the same image, the merge).
		const int32_t slotIdx = atlas.SlotBindlessIndex(ctx, slot);
		const int32_t tmpIdx = atlas.TempBindlessIndex(ctx);
		assert(slotIdx >= 0 && tmpIdx >= 0);

		ImageSyncRequest srcR{};
		srcR.barrierMasks.accessMask = AccessFlags::ShaderRead;
		srcR.barrierMasks.stageMask = PipelineStageBit_Compute;
		srcR.layout = ImageLayout::ShaderReadOnlyOptimal;
		ImageSyncRequest dstW{};
		dstW.barrierMasks.accessMask = AccessFlags::ShaderWrite;
		dstW.barrierMasks.stageMask = PipelineStageBit_Compute;
		dstW.layout = ImageLayout::General;

		const EvsmBlurPush pushH = MakeBlurPush(atlas, tileIndex,
			static_cast<uint32_t>(slotIdx), static_cast<uint32_t>(tmpIdx));
		RenderPass& hPass = rg.AddComputePass(nameH ? nameH : "EvsmBlurH");
		hPass.MarkAsAsyncCompute();
		hPass.AddImageHint(atlas.slotLogical[slot], srcR);
		hPass.AddImageHint(atlas.blurTempLogical, dstW);
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

		// V reads the shared temp, writes the slot image IN PLACE (the
		// moments are dead after H; the blurred output takes their bytes).
		const EvsmBlurPush pushV = MakeBlurPush(atlas, tileIndex,
			static_cast<uint32_t>(tmpIdx), static_cast<uint32_t>(slotIdx));
		RenderPass& vPass = rg.AddComputePass(nameV ? nameV : "EvsmBlurV");
		vPass.MarkAsAsyncCompute();
		vPass.AddImageHint(atlas.blurTempLogical, srcR);
		vPass.AddImageHint(atlas.slotLogical[slot], dstW);
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
