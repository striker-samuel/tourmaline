#pragma once
// SSGI: the post-Main screen-space subsystem (see the writeup in
// ssao_compose.frag.hlsl + world_renderer.cpp's frame block).
//
// Owns:
//   - the FINAL-DEPTH MAX-HZB (hzb_max.cs.hlsl): the nearest-surface
//     pyramid over the COMPLETE renderDepth (reversed-Z MAX), built after
//     every depth-writing pass — a shared screen-space primitive for the
//     SSR trace (mip walk), the SSR blur/compose block reps (mip1 =
//     nearest-of-2x2) and the AO chain (mip0 center depth + mip1 reps).
//     NOT the gfx-filter's MIN culling HZB (that one reduces MIN over the
//     seed's partial set / Option-A occluders before the depth passes
//     complete — wrong values and wrong reduction for everything here).
//   - the AO chain: raw (SSAO/SAO at half res, depth-only inputs) ->
//     bilateral blur -> full-res joint-bilateral (depth + normal) compose
//     that applies AO to the HDR through Main's exported aoTerm (MRT).
//
// The AO half-res targets carry a self-consistent (ao, repDist, oct
// repNormal) triple in RGBA16_UNorm (see ao.hlsli) — no stage
// re-derives a block's depth or normal from a corner texel, and the
// blur runs with ZERO depth loads. The compose reads:
//   1 center depth (HZB mip0), 1 center normal (shared full-res
//   G-buffer), 1 aoTerm, 4 half-res taps -> hdr - aoTerm*(1-aoEff).
//
// AO placement moved post-Main (this subsystem runs after Main +
// particles, before MotionBlur) with NO temporal lag: Main exports the
// pre-AO ambient sum through a second MRT target (aoTerm), every
// AO-sensitive lighting term of the lit shader flows through it, and
// the compose subtracts aoTerm*(1-aoEff) — an exact reconstruction
// (all factors between Main's store and the compose read are linear:
// occlusion, emissive, fog, alpha blending, the 2x MSAA resolve).
// Main itself no longer samples AO at all.
//
// Usage (world_renderer.cpp):
//   smvk::Ssgi ssgi;
//   ssgi.Ensure(ctx, rg, renderExtent, desc);      // targets + programs
//   ssgi.hzb.Ensure(ctx, rg, w, h);                 // MAX pyramid
//   if (hzbBuilt) ssgi.AddHzbMipPasses(rg, hzbProgram, depthView, depthBindless);
//   if (aoOn) ssgi.AddAoPasses(rg, ctx, mode, depthView, depthBindless, ...);
//   ... ssgi.OutputView() / OutputBindless() -> the AO-applied scene HDR ...

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Renderer/gfx_filter.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace smvk {

// ---- push mirrors (keep the HLSL twins in lockstep; static_asserts) ----

// Must match ssao.frag.hlsl SsaoPush (48 bytes) — unchanged from the old
// pre-Main chain (the raw pass keeps its own inputs; only its OUTPUT
// became the (ao, encRepDist, octN) triple).
struct SsaoPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t sampleCount = 16;
	float radius = 0.5f;
	float bias = 0.025f;
	float intensity = 1.0f; // reserved (composite lives in the SSGI compose)
	float maxDistance = 25.0f;
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f; // 1 / full-res depth height
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsaoPush) == 48);

// Must match sao.frag.hlsl SaoPush (48 bytes) — unchanged from the old
// pre-Main chain (same note as SsaoPush).
struct SaoPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t depthTextureIndex = UINT32_MAX;
	uint32_t sampleCount = 16; // 1..32 spiral taps
	float radius = 0.5f; // view-space spiral extent (m)
	float bias = 0.025f; // surface-offset bias (view units)
	float intensity = 1.0f; // reserved (composite lives in the SSGI compose)
	float maxDistance = 25.0f; // AO fade-out distance (m) + the .g encode range
	float invDepthW = 0.0f;
	float invDepthH = 0.0f;
	uint32_t spiralTurns = 7; // 1..16 extra spiral twist
	uint32_t pad0 = 0;
};
static_assert(sizeof(SaoPush) == 48);

// Must match ssao_blur.frag.hlsl SsgiAoBlurPush (32 bytes).
struct SsgiAoBlurPush {
	uint32_t aoTextureIndex = UINT32_MAX; // raw half-res (ao, encDist, octN)
	uint32_t samplerIndex = 0; // linear clamp (taps)
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
	float sharpness = 8.0f; // depth edge weight (higher = tighter)
	float maxDistance = 25.0f; // rep-dist decode (matches the raw pass)
	float texelX = 0.0f; // 1 / half-res width
	float texelY = 0.0f; // 1 / half-res height
};
static_assert(sizeof(SsgiAoBlurPush) == 32);

// Must match ssao_compose.frag.hlsl SsgiAoComposePush (64 bytes).
// uint64 BDAs lead; scalars after (see SsrClassifyPush's note).
struct SsgiAoComposePush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (nearPlane)
	uint64_t hzb = 0; // BDA of the MAX-HZB (mip0 center depth, mip1 reps)
	uint32_t aoTextureIndex = UINT32_MAX; // half-res blurred (ao, encDist, octN)
	uint32_t normalTextureIndex = UINT32_MAX; // shared full-res oct normal (UINT32_MAX = depth-only)
	uint32_t aoTermTextureIndex = UINT32_MAX; // Main MRT export (pre-AO ambient)
	uint32_t hdrTextureIndex = UINT32_MAX; // full-res HDR (the image to rewrite)
	uint32_t fullW = 0; // full-res width (HZB mip0 row stride)
	uint32_t fullH = 0;
	uint32_t hzbMip1Offset = 0; // float offset of mip1 in the HZB buffer
	uint32_t hzbMip1Stride = 0; // mip1 row stride (floats)
	float sharpness = 8.0f; // depth edge weight
	float intensity = 1.0f; // aoEff = lerp(1, ao, intensity) (the old hot.aoIntensity)
	float maxDistance = 25.0f; // rep-dist decode (matches raw/blur)
	uint32_t pad0 = 0;
};
static_assert(sizeof(SsgiAoComposePush) == 64);

// ---- desc ----

struct SsgiDesc {
	Format aoFormat = Format::RGBA16_UNorm; // half-res (ao, encRepDist, octN)
	Format hdrFormat = Format::RGBA16_Float; // aoTerm / composed (matches the scene HDR)
	float radius = 0.5f; // view-space sample radius (m)
	float bias = 0.025f; // self-occlusion / horizon bias
	float intensity = 1.0f; // compose-side strength (0 = off)
	float maxDistance = 25.0f; // AO fade-out distance (m) + the .g encoding range
	uint32_t sampleCount = 16; // SSAO hemisphere taps / SAO spiral taps 1..32
	float blurSharpness = 8.0f; // depth edge weight (blur + compose)
	float nearPlane = 0.01f; // must match FrameUniforms.nearPlane
	uint32_t spiralTurns = 7; // SAO spiral twist 1..16
	const char* name = "ssgi";
	const char* aoRawName = "ssgiAoRaw";
	const char* aoBlurName = "ssgiAoBlur";
	const char* aoTermName = "ssgiAoTerm";
	const char* aoTermMsaaName = "ssgiAoTermMsaa";
	const char* composedName = "ssgiAoHdr";
};

// Raw-pass selector (the app's AoMode minus None/Gtao).
enum class SsgiAoMode : int { Ssao = 0, Sao = 1 };

// Default SPVs (fullscreen-triangle VS shared with tonemap).
inline constexpr const char* kSsgiVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kSsgiSsaoPsPath = "content/shaders/ssao.frag.hlsl.spv";
inline constexpr const char* kSsgiSaoPsPath = "content/shaders/sao.frag.hlsl.spv";
inline constexpr const char* kSsgiBlurPsPath = "content/shaders/ssao_blur.frag.hlsl.spv";
inline constexpr const char* kSsgiComposePsPath = "content/shaders/ssao_compose.frag.hlsl.spv";
inline constexpr const char* kSsgiHzbCsPath = "content/shaders/hzb_max.cs.hlsl.spv";

struct Ssgi {

	// ---- final-depth MAX-HZB (shared screen-space primitive) ----
	struct Hzb {
		Holder<BufferHandle> buffer;
		LogicalBufferHandle logical{};
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t mipCount = 0;
		uint32_t mipOffsets[gfx::kHizMaxMips] = {};

		[[nodiscard]] bool Valid() const {
			return !buffer.empty() && !logical.empty() && mipCount != 0;
		}
		[[nodiscard]] uint64_t Address(ResourceManager& ctx) const {
			return buffer.empty() ? 0 : ctx.GetBufferDeviceAddress(buffer);
		}
		[[nodiscard]] uint32_t Mip1Offset() const {
			return mipCount > 1 ? mipOffsets[1] : 0;
		}
		[[nodiscard]] uint32_t Mip1Stride() const {
			if (mipCount < 2)
				return 0;
			uint32_t dw = 0, dh = 0;
			gfx::HzbMipDims(width, height, 1, dw, dh);
			return dw;
		}

		void Destroy(RenderGraph& rg) {
			if (!logical.empty())
				rg.FreeBuffer(logical);
			logical = {};
			buffer = {};
			width = height = mipCount = 0;
		}

		// Recreate only on extent drift (same lifecycle as the old
		// WorldRenderer::EnsureSsrHzb): free the logical, zero-fill the
		// fresh buffer (0 = far/sky at every MAX block — reads as "no
		// data" until the build pass runs), register it with the graph.
		bool Ensure(ResourceManager& ctx, RenderGraph& rg, uint32_t w, uint32_t h) {
			if (w == 0 || h == 0)
				return false;
			const uint32_t wantMips = gfx::HzbMipCount(w, h);
			if (width == w && height == h && mipCount == wantMips
				&& !buffer.empty() && !logical.empty())
				return true;
			Destroy(rg);
			width = w;
			height = h;
			mipCount = wantMips;
			for (uint32_t m = 0; m < wantMips; ++m)
				mipOffsets[m] = gfx::HzbMipOffsetFloats(w, h, m);
			for (uint32_t m = wantMips; m < gfx::kHizMaxMips; ++m)
				mipOffsets[m] = 0;
			const uint32_t totalFloats = gfx::HzbTotalFloats(w, h);
			// Shader-side array cap (hzb.hlsli HZB_MAX_FLOATS): 4K-class
			// targets fit; beyond it the shader reads/writes OOB
			// (diagnosed loudly).
			if (totalFloats > 12582912u)
				std::cerr << "[Ssgi] hzb " << w << "x" << h
					<< " needs " << totalFloats << " floats (shader cap 12582912): "
					<< "the HZB stays off for this extent\n";
			buffer = ctx.CreateBuffer(
				(size_t)totalFloats * sizeof(float),
				BufferType_Storage, "ssgi hzb max");
			if (buffer.empty()) {
				width = height = mipCount = 0;
				return false;
			}
			{
				std::vector<uint8_t> zeros((size_t)totalFloats * sizeof(float), 0);
				if (!zeros.empty()) {
					ctx.UploadDataToBuffer(buffer, zeros.data(), zeros.size(),
						PipelineStage(PipelineStageBit_Compute),
						AccessFlags::ShaderRead | AccessFlags::ShaderWrite);
				}
			}
			logical = rg.AddExternalBuffer(buffer, "ssgi hzb max");
			if (logical.empty()) {
				buffer = {};
				width = height = mipCount = 0;
				return false;
			}
			return true;
		}
	};

	Hzb hzb;

	// One compute pass per mip level (rendergraph barriers between; no
	// intra-pass barrier API): mip 0 copies the depth texture 1:1, mips 1+
	// MAX-reduce the level below. Verbatim of the old WorldRenderer SSR
	// block (moved: the pyramid is a shared primitive now — SSR's trace
	// and the AO chain consume it; the SsrHzb name became SsgiHzb).
	// hzbProgram: hzb_max.cs.hlsl (mainCS, compute). hzbAddr: the BDA this
	// graph's passes dereference (resolve via hzb.Address(ctx) at CALL
	// time — captures only copyable values, the repo's pass discipline).
	void AddHzbMipPasses(RenderGraph& rg, const ShaderProgram& hzbProgram,
		uint64_t hzbAddr,
		LogicalImageViewHandle depthView, uint32_t depthBindlessIndex) const {
		assert(hzbProgram.shaderCount == 1);
		assert(hzb.Valid());
		assert(hzbAddr != 0);
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		const uint32_t mipCount = hzb.mipCount;
		const uint32_t width = hzb.width;
		const uint32_t height = hzb.height;
		for (uint32_t m = 0; m < mipCount; ++m) {
			const bool fromDepth = (m == 0);
			RenderPass& hzbPass = rg.AddComputePass("SsgiHzb");
			{
				BufferSyncRequest hzbRW{};
				hzbRW.barrierMasks.accessMask =
					AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
				hzbRW.barrierMasks.stageMask = PipelineStageBit_Compute;
				hzbPass.AddBufferHint(hzb.logical, hzbRW);
			}
			if (fromDepth) {
				// Depth sampled in compute (DepthStencilReadOnlyOptimal:
				// matches the bindless descriptor layout — see gfx's
				// AddHizPass note).
				ImageSyncRequest depthReq{};
				depthReq.barrierMasks.accessMask = AccessFlags::ShaderRead;
				depthReq.barrierMasks.stageMask = PipelineStageBit_Compute;
				depthReq.layout = ImageLayout::DepthStencilReadOnlyOptimal;
				hzbPass.AddImageHint(depthView, depthReq);
			}
			const uint32_t mip = m;
			// (mipOffsets captured by value — kHizMaxMips is small)
			uint32_t offsets[gfx::kHizMaxMips];
			for (uint32_t k = 0; k < gfx::kHizMaxMips; ++k)
				offsets[k] = hzb.mipOffsets[k];
			ShaderProgram hzbProgCopy = hzbProgram; // copyable (handles only)
			hzbPass.SetRecordingFunc(
				[hzbProgCopy, hzbAddr, mip, depthBindlessIndex, fromDepth, width,
				height, offsets](CommandBuffer& cmd) mutable {
				uint32_t dw = 0, dh = 0, sw = 0, sh = 0;
				gfx::HzbMipDims(width, height, mip, dw, dh);
				gfx::HzbMipDims(width, height, fromDepth ? 0u : mip - 1u, sw, sh);
				gfx::HzbPush push{};
				push.hzb = hzbAddr;
				push.dstMip = mip;
				push.dstWidth = dw;
				push.dstHeight = dh;
				push.dstOffset = offsets[mip];
				push.depthTextureIndex = depthBindlessIndex;
				if (fromDepth) {
					push.srcMip = gfx::kHizSrcDepth;
					push.srcWidth = width;
					push.srcHeight = height;
					push.srcOffset = 0;
				} else {
					push.srcMip = mip - 1;
					push.srcWidth = sw;
					push.srcHeight = sh;
					push.srcOffset = offsets[mip - 1];
				}
				cmd.BindComputePipeline(hzbProgCopy);
				cmd.PushConstants(0, sizeof(push), &push,
					ShaderStage::Compute);
				cmd.Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
			});
		}
	}

	// ---- targets ----

	struct Target {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical{};
		[[nodiscard]] bool Valid() const { return !image.empty() && !logical.empty(); }
	};

	Target raw;      // half-res (ao, encRepDist, octN)
	Target blurred;  // half-res, same representation
	Target aoTerm;   // FULL-res RGBA16F: Main's MRT export (pre-AO ambient)
	Target aoTermMsaa; // FULL-res 2x RGBA16F: Main's MSAA twin (resolve -> aoTerm)
	Target composed; // FULL-res RGBA16F: the AO-applied scene HDR (compose output)
	Extent2D fullExtent{ 0, 0 };
	Extent2D halfExtent{ 0, 0 };
	Format aoFormat = Format::RGBA16_UNorm;
	Format hdrFormat = Format::RGBA16_Float;
	SsgiDesc desc{};

	// ---- programs ----
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> ssaoFragModule;
	Holder<ShaderModuleHandle> saoFragModule;
	Holder<ShaderModuleHandle> blurFragModule;
	Holder<ShaderModuleHandle> composeFragModule;
	ShaderProgram ssaoRawProgram{};
	ShaderProgram saoRawProgram{};
	ShaderProgram blurProgram{};
	ShaderProgram composeProgram{};

	[[nodiscard]] bool TargetsValid() const {
		return raw.Valid() && blurred.Valid() && aoTerm.Valid()
			&& aoTermMsaa.Valid() && composed.Valid();
	}
	[[nodiscard]] bool ShadersValid() const {
		return ssaoRawProgram.shaderCount != 0 && saoRawProgram.shaderCount != 0
			&& blurProgram.shaderCount != 0 && composeProgram.shaderCount != 0
			&& !vsModule.empty() && !ssaoFragModule.empty() && !saoFragModule.empty()
			&& !blurFragModule.empty() && !composeFragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetsValid() && ShadersValid(); }

	// Consumer output: the AO-applied FULL-res HDR (safe to sample from any
	// later pass; the scene HDR chain pointer moves here when the AO chain ran).
	[[nodiscard]] LogicalImageViewHandle OutputView() const { return composed.logical; }
	[[nodiscard]] uint32_t OutputBindless() const { return composed.bindlessIndex; }

	static Extent2D HalfExtentFor(Extent2D full) {
		return {
			std::max(1u, full.width / 2),
			std::max(1u, full.height / 2),
		};
	}

	static bool CreateTarget(ResourceManager& ctx, RenderGraph& rg,
		Extent2D extent, Format format, const char* name, Target& out) {
		if (extent.width == 0 || extent.height == 0)
			return false;
		ImageCreateInfo ci{};
		ci.extent = { extent.width, extent.height, 1 };
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

	// The MSAA aoTerm twin: 2x RGBA16F color attachment (resolve target of
	// Main's SetColorWithResolve, no Sampled usage — it is never read
	// directly; the resolved 1x aoTerm is), view + logical only.
	static bool CreateMsaaTarget(ResourceManager& ctx, RenderGraph& rg,
		Extent2D extent, Format format, const char* name, Target& out) {
		if (extent.width == 0 || extent.height == 0)
			return false;
		ImageCreateInfo ci{};
		ci.extent = { extent.width, extent.height, 1 };
		ci.format = format;
		ci.usage = ImageUsage::ColorAttachment;
		ci.samples = SampleCount::Count2;
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

		LogicalImageViewHandle newLogical =
			rg.AddExternalImageView(newView, format, name);
		if (newLogical.empty())
			return false;

		Target t;
		t.image = std::move(newImage);
		t.view = std::move(newView);
		t.logical = newLogical;
		out = std::move(t);
		return true;
	}

	static void DestroyTarget(RenderGraph& rg, Target& t) {
		if (!t.logical.empty())
			rg.FreeImageView(t.logical);
		t = Target{};
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kSsgiVsPath,
		const char* ssaoPath = kSsgiSsaoPsPath,
		const char* saoPath = kSsgiSaoPsPath,
		const char* blurPath = kSsgiBlurPsPath,
		const char* composePath = kSsgiComposePsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kSsgiVsPath;
			const std::string ssao = ssaoPath ? ssaoPath : kSsgiSsaoPsPath;
			const std::string sao = saoPath ? saoPath : kSsgiSaoPsPath;
			const std::string blur = blurPath ? blurPath : kSsgiBlurPsPath;
			const std::string compose = composePath ? composePath : kSsgiComposePsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "ssgiVert");
			if (ssaoFragModule.empty())
				ssaoFragModule = ctx.CreateShaderModule(readFile(ssao), "ssgiSsaoRawFrag");
			if (saoFragModule.empty())
				saoFragModule = ctx.CreateShaderModule(readFile(sao), "ssgiSaoRawFrag");
			if (blurFragModule.empty())
				blurFragModule = ctx.CreateShaderModule(readFile(blur), "ssgiBlurFrag");
			if (composeFragModule.empty())
				composeFragModule = ctx.CreateShaderModule(readFile(compose), "ssgiComposeFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || ssaoFragModule.empty() || saoFragModule.empty()
			|| blurFragModule.empty() || composeFragModule.empty())
			return false;
		Shader vsS{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader ssaoPS{ .module = ssaoFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader saoPS{ .module = saoFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader blurPS{ .module = blurFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader composePS{ .module = composeFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		ssaoRawProgram = { .shaders = { vsS, ssaoPS }, .shaderCount = 2 };
		saoRawProgram = { .shaders = { vsS, saoPS }, .shaderCount = 2 };
		blurProgram = { .shaders = { vsS, blurPS }, .shaderCount = 2 };
		composeProgram = { .shaders = { vsS, composePS }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		ssaoRawProgram = ShaderProgram{};
		saoRawProgram = ShaderProgram{};
		blurProgram = ShaderProgram{};
		composeProgram = ShaderProgram{};
		vsModule = {};
		ssaoFragModule = {};
		saoFragModule = {};
		blurFragModule = {};
		composeFragModule = {};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const SsgiDesc& d,
		const char* vsPath = kSsgiVsPath,
		const char* ssaoPath = kSsgiSsaoPsPath,
		const char* saoPath = kSsgiSaoPsPath,
		const char* blurPath = kSsgiBlurPsPath,
		const char* composePath = kSsgiComposePsPath) {
		if (full.width == 0 || full.height == 0)
			return false;
		DestroyTargets(rg);
		const Extent2D half = HalfExtentFor(full);
		Target newRaw, newBlur, newTerm, newTermMsaa, newComposed;
		if (!CreateTarget(ctx, rg, half, d.aoFormat, d.aoRawName, newRaw))
			return false;
		if (!CreateTarget(ctx, rg, half, d.aoFormat, d.aoBlurName, newBlur)) {
			DestroyTarget(rg, newRaw);
			return false;
		}
		if (!CreateTarget(ctx, rg, full, d.hdrFormat, d.aoTermName, newTerm)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			return false;
		}
		if (!CreateMsaaTarget(ctx, rg, full, d.hdrFormat, d.aoTermMsaaName, newTermMsaa)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newTerm);
			return false;
		}
		if (!CreateTarget(ctx, rg, full, d.hdrFormat, d.composedName, newComposed)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newTerm);
			DestroyTarget(rg, newTermMsaa);
			return false;
		}
		raw = std::move(newRaw);
		blurred = std::move(newBlur);
		aoTerm = std::move(newTerm);
		aoTermMsaa = std::move(newTermMsaa);
		composed = std::move(newComposed);
		fullExtent = full;
		halfExtent = half;
		aoFormat = d.aoFormat;
		hdrFormat = d.hdrFormat;
		desc = d;
		return EnsureShaders(ctx, vsPath, ssaoPath, saoPath, blurPath, composePath);
	}

	void DestroyTargets(RenderGraph& rg) {
		DestroyTarget(rg, raw);
		DestroyTarget(rg, blurred);
		DestroyTarget(rg, aoTerm);
		DestroyTarget(rg, aoTermMsaa);
		DestroyTarget(rg, composed);
		fullExtent = { 0, 0 };
		halfExtent = { 0, 0 };
	}

	// Recreate only when extent/format drift. Live-updates desc (radius /
	// bias / samples need no realloc).
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D full, const SsgiDesc& d,
		const char* vsPath = kSsgiVsPath,
		const char* ssaoPath = kSsgiSsaoPsPath,
		const char* saoPath = kSsgiSaoPsPath,
		const char* blurPath = kSsgiBlurPsPath,
		const char* composePath = kSsgiComposePsPath) {
		if (TargetsValid() &&
			fullExtent.width == full.width && fullExtent.height == full.height &&
			aoFormat == d.aoFormat && hdrFormat == d.hdrFormat) {
			desc = d;
			return EnsureShaders(ctx, vsPath, ssaoPath, saoPath, blurPath, composePath);
		}
		return Create(ctx, rg, full, d, vsPath, ssaoPath, saoPath, blurPath, composePath);
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyShaders();
	}

	// ---- pass emission ----

	// Emits raw (depth -> half-res AO triple) + blur (triple -> blurred
	// triple, zero depth loads) + compose (full-res joint bilateral +
	// the aoTerm HDR reconstruction). depthView/depthBindlessIndex: the
	// 1x prepass depth (valid in 1x + MSAA modes). hdrView/hdrBindless:
	// the post-Main scene HDR this pass rewrites. normalView /
	// normalBindlessIndex: the shared full-res thin G-buffer (oct normal —
	// written by the prepass when SSR OR AO wants it); empty / invalid =
	// depth-only edge weights. hzbProgramHandle is not needed here — the
	// HZB build is emitted separately (AddHzbMipPasses) so SSR can share
	// it. Requires hzb.Valid() (the compose dereferences it).
	// Captures only copyable handles/indices/addresses.
	void AddAoPasses(RenderGraph& rg,
		ResourceManager& ctx,
		SsgiAoMode mode,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle normalView,
		uint32_t normalBindlessIndex,
		const SsgiDesc& overrideDesc,
		const char* rawName = "SsgiAo",
		const char* blurName = "SsgiAoBlur",
		const char* composeName = "SsgiAoCompose") const {
		assert(Valid());
		assert(mode == SsgiAoMode::Ssao || mode == SsgiAoMode::Sao);
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(hzb.Valid());
		assert(hzb.Mip1Stride() != 0);

		const uint64_t hzbAddr = hzb.Address(ctx);
		assert(hzbAddr != 0);
		const uint32_t hzbMip1Offset = hzb.Mip1Offset();
		const uint32_t hzbMip1Stride = hzb.Mip1Stride();
		const bool normalOn =
			!normalView.empty() && normalBindlessIndex != UINT32_MAX;

		// ---- Raw: full-res depth -> half-res (ao, encRepDist, octN) ----
		{
			RenderPass& pass = rg.AddPass(rawName ? rawName : "SsgiAo");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ depthView })
				.SetColor({ raw.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			if (mode == SsgiAoMode::Ssao) {
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
				ShaderProgram progCopy = ssaoRawProgram; // copyable (handles only)
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
			} else {
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
				ShaderProgram progCopy = saoRawProgram; // copyable (handles only)
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
		}

		// ---- Blur: raw triple -> blurred triple (ZERO depth loads) ----
		{
			SsgiAoBlurPush push{};
			push.aoTextureIndex = raw.bindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.pad0 = 0;
			push.pad1 = 0;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.maxDistance = (std::max)(1.0f, overrideDesc.maxDistance);
			push.texelX = 1.0f / static_cast<float>(std::max(1u, halfExtent.width));
			push.texelY = 1.0f / static_cast<float>(std::max(1u, halfExtent.height));

			RenderPass& pass = rg.AddPass(blurName ? blurName : "SsgiAoBlur");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ raw.logical })
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
				cmd.PushConstants(0, sizeof(SsgiAoBlurPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Compose: blurred half-res + HZB depth + shared normal +
		// aoTerm -> the AO-applied FULL-res HDR. Reads the HZB buffer
		// (mip0 center, mip1 reps): barrier the SsgiHzb compute writes
		// before these fragment reads.
		{
			SsgiAoComposePush push{};
			push.frameUniforms = frameUniformsAddr;
			push.hzb = hzbAddr;
			push.aoTextureIndex = blurred.bindlessIndex;
			push.normalTextureIndex = normalOn ? normalBindlessIndex : UINT32_MAX;
			push.aoTermTextureIndex = aoTerm.bindlessIndex;
			push.hdrTextureIndex = hdrBindlessIndex;
			push.fullW = fullExtent.width;
			push.fullH = fullExtent.height;
			push.hzbMip1Offset = hzbMip1Offset;
			push.hzbMip1Stride = hzbMip1Stride;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.intensity = (std::max)(0.0f, overrideDesc.intensity);
			push.maxDistance = (std::max)(1.0f, overrideDesc.maxDistance);
			push.pad0 = 0;

			std::vector<LogicalImageViewHandle> composeInputs =
				{ blurred.logical, hdrView, aoTerm.logical };
			if (normalOn)
				composeInputs.push_back(normalView);
			RenderPass& pass = rg.AddPass(composeName ? composeName : "SsgiAoCompose");
			pass.SetOffset(0, 0)
				.SetExtent(fullExtent)
				.SetInputImages(composeInputs)
				.SetColor({ composed.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			{
				BufferSyncRequest hzbRead{};
				hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				hzbRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
				pass.AddBufferHint(hzb.logical, hzbRead);
			}
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
				cmd.PushConstants(0, sizeof(SsgiAoComposePush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}
	}
};

} // namespace smvk
