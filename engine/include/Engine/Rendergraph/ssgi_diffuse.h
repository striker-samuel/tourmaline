// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Diffuse screen-space GI (one bounce, half resolution — the plan
// doc's "diffuse SSGI"): a SEPARATE diffuse-indirect pipeline that
// reuses the SSR infrastructure (the shared reverse-Z MAX-HZB trace,
// the thin G-buffer, Main's HDR + MRT ambient exports, the
// motion-vector temporal reprojection, the depth-guided bilateral
// filtering) WITHOUT converting SSR into a combined shader — SSR keeps
// the specular-indirect slot, this chain owns the diffuse slot.
//
// The pipeline (half-res trace + filters, full-res compose):
//   trace:     ONE cosine-weighted hemisphere ray per half-res pixel
//              per frame (pdf = cos/pi, so the MC estimate of the
//              diffuse integral is albedo * Li — no extra cosine),
//              marched through the shared final-depth MAX-HZB by the
//              SSR intersect's cell-exit mip walk (copied into the
//              shader verbatim; extraction to a shared
//              screen_space_trace.hlsli waits for field validation).
//              Output RGBA16F: rgb = RAW incoming radiance (NO
//              albedo/kd/Fresnel — receiver reflectance is applied at
//              the compose so it never smears across filter
//              boundaries), a = hit confidence (0 on every miss).
//   copy:      the radiance BDA buffer -> the raw target (the SSR
//              compute-chain pattern; reuses ssr_copy.frag.hlsl).
//   blur:      fixed 5x5 joint-bilateral (HZB mip1 depth + packed
//              receiver normals) — geometric similarity only, no
//              roughness buckets (diffuse spread is not a mirror
//              lobe), confidence-aware accumulation.
//   temporal:  motion-reprojected accumulation of the GI layer with
//              STRENGTHENED rejection: previous depth (the R32F
//              mirror) AND previous normals (the RGBA8 oct mirror —
//              the diffuse addition; a same-depth different-surface
//              ghost survives depth alone), the current-neighborhood
//              AABB clamp, premultiplied confidence blending.
//   compose:   FULL-res energy-conserving integration —
//              L = base + s*c*(albedo*kd*Li - diffuseAmbient), the
//              SIGNED replacement of the diffuse-ambient slice (NOT
//              additive over the baked GI — that would double-count),
//              c=0 keeps the baked GI/probe fallback exactly (the
//              off-screen safety), the baseline sourced from the AO
//              compose's third output (AO on) or Main's raw
//              aoTerm/envSpec exports (AO off, aoEff=1).
//
// Placement: after the SSGI/AO compose, BEFORE SSR (WorldRenderer's
// frame block). The trace source is the FROZEN pre-compose HDR (the
// post-AO scene color) — the compose writes its OWN full-res target
// and re-points the scene HDR pointer, so the trace never samples the
// just-composited result (no recursive feedback, no read/write hazard
// in the graph); SSR then runs on the GI-adjusted HDR. MSAA is not
// supported (the thin G-buffer has no stable single-sample companion
// under it — the app gates the chain off; support needs a consistent
// depth/material resolve first).
//
// Resource lifecycle: ssr.h's discipline — targets/buffers recreated
// only on extent drift (Ensure), temporal history invalidated at the
// same moment (the app owns the invalidation call), captures only
// copyable handles/indices. The radiance/receiver buffers are fully
// rewritten by the full half-res dispatch every frame — no
// InterlockedAdd, no creation zero-fill contract (unlike the SSR ray
// counter).

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

// Must match ssgi_diffuse_trace.cs.hlsl SsgiDiffuseTracePush (96
// bytes). uint64 BDAs lead (scalars never precede a BDA).
struct SsgiDiffuseTracePush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint64_t radianceAddr = 0; // BDA: float4 per half-res pixel
	uint64_t receiverDataAddr = 0; // BDA: packed receiver per half-res pixel
	uint64_t hzbAddr = 0; // BDA of the shared final-depth MAX-HZB
	uint32_t hdrTextureIndex = UINT32_MAX; // the FROZEN pre-compose HDR
	uint32_t depthTextureIndex = UINT32_MAX; // full-res final depth
	uint32_t gbufferTextureIndex = UINT32_MAX; // thin G-buffer (oct normal + metal)
	uint32_t samplerIndex = 0; // linear clamp (the HDR tap)
	uint32_t halfW = 0; // half-res extent (the slot domain)
	uint32_t halfH = 0;
	float invDepthW = 0.0f; // 1 / full-res depth extent
	float invDepthH = 0.0f;
	float maxDistance = 5.0f; // ray length budget (m)
	float thickness = 0.3f; // depth acceptance band -> confidence
	uint32_t frameIndex = 0; // IGN dither epoch (temporal convergence)
	uint32_t maxIntersections = 64; // traversal cap (exhaustion = miss)
	uint32_t hzbMipCount = 0;
	uint32_t mostDetailedMip = 0; // the walk's finest level: 0 = full-res refinement (the thickness confidence needs the real depth texel — mip 1's block quantization collapsed it)
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsgiDiffuseTracePush) == 96);

// Must match ssgi_diffuse_blur.frag.hlsl SsgiDiffuseBlurPush (64
// bytes). All-scalar layout on purpose (the spirv-val
// straddling-vector hazard — see SsrBlurPush's note).
struct SsgiDiffuseBlurPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (nearPlane)
	uint64_t hzb = 0; // BDA of the MAX-HZB (mip1 reads)
	uint64_t receiverData = 0; // BDA of the packed receiver metadata
	uint32_t giTextureIndex = UINT32_MAX; // half-res raw GI layer
	uint32_t samplerIndex = 0; // linear clamp
	float sharpness = 10.0f; // depth edge weight
	float texelX = 0.0f; // 1 / half-res width
	float texelY = 0.0f; // 1 / half-res height
	uint32_t hzbMip1Offset = 0; // float offset of mip1 in the HZB buffer
	uint32_t hzbMip1Stride = 0; // mip1 row stride (floats)
	uint32_t halfW = 0; // half-res extent (BDA tap clamp domain)
	uint32_t halfH = 0;
	uint32_t pad0 = 0;
};
static_assert(sizeof(SsgiDiffuseBlurPush) == 64);

// Must match ssgi_diffuse_temporal.frag.hlsl SsgiDiffuseTemporalPush
// (80 bytes). All-scalar layout on purpose (same hazard).
struct SsgiDiffuseTemporalPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint32_t currTextureIndex = UINT32_MAX; // half-res blurred GI (this frame)
	uint32_t historyTextureIndex = UINT32_MAX; // half-res history (previous record)
	uint32_t motionTextureIndex = UINT32_MAX; // render-extent RG16F motion
	uint32_t depthTextureIndex = UINT32_MAX; // full-res depth
	uint32_t prevDepthTextureIndex = UINT32_MAX; // previous record's R32F mirror
	uint32_t prevNormalTextureIndex = UINT32_MAX; // previous record's RGBA8 oct mirror
	uint32_t gbufferTextureIndex = UINT32_MAX; // the CURRENT receiver normal
	uint32_t samplerIndex = 0;
	uint32_t historyValid = 0; // 0 = warmup passthrough of curr
	float jitterDeltaX = 0.0f; // (prevJitter - currJitter) UV
	float jitterDeltaY = 0.0f;
	float texelX = 0.0f; // 1 / half-res width
	float texelY = 0.0f; // 1 / half-res height
	float invDepthW = 0.0f; // 1 / full-res depth extent
	float invDepthH = 0.0f;
	float blend = 0.9f; // history weight (before attenuation)
	float sharpness = 10.0f; // depth disocclusion edge weight
	uint32_t pad0 = 0;
};
static_assert(sizeof(SsgiDiffuseTemporalPush) == 80);

// Must match ssgi_diffuse_prev.frag.hlsl SsgiDiffusePrevPush (16
// bytes) — the depth + oct-normal history mirrors.
struct SsgiDiffusePrevPush {
	uint32_t depthTextureIndex = UINT32_MAX; // full-res final depth
	uint32_t gbufferTextureIndex = UINT32_MAX; // thin G-buffer (oct normal .xy)
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsgiDiffusePrevPush) == 16);

// Must match ssgi_diffuse_compose.frag.hlsl SsgiDiffuseComposePush
// (104 bytes). uint64 BDAs lead; scalars after.
struct SsgiDiffuseComposePush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice
	uint64_t hzb = 0; // BDA of the MAX-HZB (mip0 center, mip1 reps)
	uint64_t receiverData = 0; // BDA of the packed receiver metadata
	uint32_t giTextureIndex = UINT32_MAX; // half-res GI layer (temporal/blurred)
	uint32_t hdrTextureIndex = UINT32_MAX; // full-res base HDR (frozen source)
	uint32_t samplerIndex = 0;
	uint32_t gbufferTextureIndex = UINT32_MAX; // metal + center normal
	uint32_t gbuffer2TextureIndex = UINT32_MAX; // albedo (baseColor texel)
	uint32_t ambientTextureIndex = UINT32_MAX; // AO-compose baseline; MAX = inline
	uint32_t aoTermTextureIndex = UINT32_MAX; // Main MRT export (AO-off path)
	uint32_t envSpecTextureIndex = UINT32_MAX; // env-spec export (AO-off path)
	uint32_t rawTextureIndex = UINT32_MAX; // half-res raw (debug view 2)
	float invDepthW = 0.0f; // 1 / full-res depth extent
	float invDepthH = 0.0f;
	float sharpness = 10.0f; // upsample edge weight
	float intensity = 1.0f; // s (the user SSGI strength)
	uint32_t fullW = 0; // full-res width (HZB mip0 row stride)
	uint32_t fullH = 0;
	uint32_t hzbMip1Offset = 0; // float offset of mip1 in the HZB buffer
	uint32_t hzbMip1Stride = 0; // mip1 row stride (floats)
	// 0 off, 1 confidence, 2 raw, 3 layer, 4 baseline, 5 contribution
	// (the ViewMode Ssgi* debug entries — see the compose shader's
	// header for what each shows).
	uint32_t debugView = 0;
	// Views 2..5: the DIAGNOSTIC EXPOSURE GAIN (exp2(+EV) — the Debug
	// panel's SSGI debug slider / TOURMALINE_SSGI_DEBUG_EV; 1.0f =
	// none). Physical-unit scenes meter at ~0.001 exposure and dim
	// indirect readouts tonemap to near-black; this puts them in a
	// readable range WITHOUT touching the scene exposure. The
	// confidence view is excluded (it self-normalizes via 1/exposure).
	float debugGain = 1.0f;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsgiDiffuseComposePush) == 104);

// The copy pass's push: layout-identical to SsrCopyPush (24 bytes) —
// the pass reuses ssr_copy.frag.hlsl (the same BDA radiance ->
// half-res RGBA16F blit contract).
struct SsgiDiffuseCopyPush {
	uint64_t radianceAddr = 0;
	uint32_t halfW = 0;
	uint32_t halfH = 0;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsgiDiffuseCopyPush) == 24);

// ---- desc ----

struct SsgiDiffuseDesc {
	Format format = Format::RGBA16_Float; // the GI layer + the composed HDR
	float maxDistance = 5.0f; // ray length budget (m) — the LOCAL bounce
	float thickness = 0.3f; // depth acceptance band (m) -> confidence
	float intensity = 1.0f; // compose-side replacement strength (0 = off)
	float temporalBlend = 0.9f; // history weight 0..0.98 (before attenuation)
	float blurSharpness = 10.0f; // bilateral depth edge weight
	uint32_t maxIntersections = 64; // traversal loop cap (exhaustion = miss)
	uint32_t raysPerPixel = 1; // RESERVED (v1 is the fixed 1-ray dispatch)
	const char* name = "ssgiDiffuse";
	const char* viewName = "ssgiDiffuseView";
	const char* logicalName = "ssgiDiffuseLogical";
};

// ---- temporal (TAA) state, the SsrTaaState discipline ----

struct SsgiDiffuseTaaState {
	uint32_t parity = 0;
	bool historyValid = false;
};

struct SsgiDiffuseTaaFrameParams {
	uint32_t parity = 0;
	bool historyValid = false;
};

// Record-time snapshot (runs before Execute). Zeros when TAA is off.
inline SsgiDiffuseTaaFrameParams SsgiDiffuseTaaBeginFrame(
	const SsgiDiffuseTaaState& st, bool taaActive) {
	SsgiDiffuseTaaFrameParams p{};
	if (!taaActive)
		return p;
	p.parity = st.parity & 1u;
	p.historyValid = st.historyValid;
	return p;
}

// End-of-record step (runs after Execute). A non-temporal record
// freezes the phase and invalidates, so the next temporal record warms
// up instead of blending against stale history.
inline void SsgiDiffuseTaaEndFrame(SsgiDiffuseTaaState& st, bool ranTemporal) {
	if (!ranTemporal) {
		st.historyValid = false;
		return;
	}
	st.historyValid = true;
	st.parity ^= 1u;
}

// Explicit invalidation for history-content loss the toggle cannot
// see: extent/format drift (targets recreated) and motion-vector loss.
inline void SsgiDiffuseTaaInvalidateHistory(SsgiDiffuseTaaState& st) {
	st.historyValid = false;
	st.parity = 0;
}

// ---- default SPVs ----
// The fullscreen-triangle VS shared with tonemap + the whole SSGI/SSR
// FS family; the copy PS is SSR's (the identical BDA blit contract).
inline constexpr const char* kSsgiDiffuseVsPath =
	"content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kSsgiDiffuseTraceCsPath =
	"content/shaders/ssgi_diffuse_trace.cs.hlsl.spv";
inline constexpr const char* kSsgiDiffuseBlurPsPath =
	"content/shaders/ssgi_diffuse_blur.frag.hlsl.spv";
inline constexpr const char* kSsgiDiffuseTemporalPsPath =
	"content/shaders/ssgi_diffuse_temporal.frag.hlsl.spv";
inline constexpr const char* kSsgiDiffusePrevPsPath =
	"content/shaders/ssgi_diffuse_prev.frag.hlsl.spv";
inline constexpr const char* kSsgiDiffuseComposePsPath =
	"content/shaders/ssgi_diffuse_compose.frag.hlsl.spv";
inline constexpr const char* kSsgiDiffuseCopyPsPath =
	"content/shaders/ssr_copy.frag.hlsl.spv";

struct ScreenSpaceDiffuseGi {
	// Half/full-res target helper (the ssr.h Target pattern: image +
	// view + bindless + logical, all lifecycle-managed together).
	struct Target {
		Holder<ImageHandle> image;
		Holder<ImageViewHandle> view;
		Holder<BindlessImageHandle> bindless;
		uint32_t bindlessIndex = UINT32_MAX;
		LogicalImageViewHandle logical{};
		[[nodiscard]] bool Valid() const { return !image.empty() && !logical.empty(); }
	};

	// Compute-chain buffer helper (BDA storage, barriered via logical
	// hints — the ssr.h CBuf pattern).
	struct CBuf {
		Holder<BufferHandle> buffer;
		LogicalBufferHandle logical{};
		[[nodiscard]] bool Valid() const { return !buffer.empty() && !logical.empty(); }
	};

	Target raw;         // half-res RGBA16F: (raw radiance, confidence)
	Target blurred;     // half-res RGBA16F: the spatial blur of raw
	Target history[2];  // half-res RGBA16F: temporal ping-pong (this chain's OWN
	                    // history — never shared with SSR's)
	Target composed;    // FULL-res RGBA16F: the GI-replaced scene HDR (the output)
	// The temporal disocclusion test's history mirrors (written by the
	// SsgiDiffuseHistory pass at the end of every TEMPORAL record —
	// depth AND normal, the diffuse rejection's strengthening):
	Target prevDepth;   // full-res R32F (the ssr_depth_history pattern)
	Target prevNormal;  // full-res RGBA8 (oct world normal, .xy)
	// Compute-chain handoff buffers: radiance (float4 per half-res
	// pixel — rewritten by the full dispatch every frame) + receiverData
	// (the packed thin-G-buffer receiver per half-res pixel, the trace's
	// selection — the blur/compose tap normals + the filters' domain).
	CBuf radiance;
	CBuf receiverData;

	Extent2D extent{ 0, 0 }; // full-res extent (= depth/HDR extent)
	Extent2D halfExtent{ 0, 0 }; // max(1, full / 2) per axis
	Format format = Format::RGBA16_Float;

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> traceCsModule;
	Holder<ShaderModuleHandle> blurFragModule;
	Holder<ShaderModuleHandle> temporalFragModule;
	Holder<ShaderModuleHandle> prevFragModule;
	Holder<ShaderModuleHandle> composeFragModule;
	Holder<ShaderModuleHandle> copyFragModule;
	ShaderProgram traceProgram{}; // compute, 8x8 groups (full half-res grid)
	ShaderProgram blurProgram{};
	ShaderProgram temporalProgram{};
	ShaderProgram prevProgram{};
	ShaderProgram composeProgram{};
	ShaderProgram copyProgram{}; // the SSR copy blit (layout-identical push)
	SsgiDiffuseDesc desc{};

	[[nodiscard]] bool TargetsValid() const {
		return raw.Valid() && blurred.Valid() &&
			history[0].Valid() && history[1].Valid() &&
			composed.Valid() && prevDepth.Valid() && prevNormal.Valid() &&
			radiance.Valid() && receiverData.Valid();
	}
	[[nodiscard]] bool ShadersValid() const {
		return traceProgram.shaderCount != 0 && blurProgram.shaderCount != 0 &&
			temporalProgram.shaderCount != 0 && prevProgram.shaderCount != 0 &&
			composeProgram.shaderCount != 0 && copyProgram.shaderCount != 0 &&
			!vsModule.empty() && !traceCsModule.empty() && !blurFragModule.empty() &&
			!temporalFragModule.empty() && !prevFragModule.empty() &&
			!composeFragModule.empty() && !copyFragModule.empty();
	}
	[[nodiscard]] bool Valid() const { return TargetsValid() && ShadersValid(); }

	// Consumer output: the GI-replaced FULL-res HDR (the scene HDR
	// chain pointer moves here when the chain ran).
	[[nodiscard]] LogicalImageViewHandle OutputView() const { return composed.logical; }
	[[nodiscard]] uint32_t OutputBindless() const { return composed.bindlessIndex; }

	// Half extent, the CEIL form (n+1)/2 (the odd-extent column/row fix
	// - see ssgi.h's note; ssgi_diffuse_compose.frag.hlsl's in-shader
	// twin uses the same ceil form, every other shader receives the
	// extent via its push).
	static Extent2D HalfExtentFor(Extent2D full) {
		return {
			std::max(1u, (full.width + 1u) / 2u),
			std::max(1u, (full.height + 1u) / 2u),
		};
	}

	// ---- shader lifecycle (the ssr.h pattern) ----

	bool EnsureShaders(ResourceManager& ctx) {
		if (ShadersValid())
			return true;
		try {
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(kSsgiDiffuseVsPath),
					"ssgiDiffuseVert");
			if (traceCsModule.empty())
				traceCsModule = ctx.CreateShaderModule(readFile(kSsgiDiffuseTraceCsPath),
					"ssgiDiffuseTraceCS");
			if (blurFragModule.empty())
				blurFragModule = ctx.CreateShaderModule(readFile(kSsgiDiffuseBlurPsPath),
					"ssgiDiffuseBlurFrag");
			if (temporalFragModule.empty())
				temporalFragModule = ctx.CreateShaderModule(
					readFile(kSsgiDiffuseTemporalPsPath), "ssgiDiffuseTemporalFrag");
			if (prevFragModule.empty())
				prevFragModule = ctx.CreateShaderModule(readFile(kSsgiDiffusePrevPsPath),
					"ssgiDiffusePrevFrag");
			if (composeFragModule.empty())
				composeFragModule = ctx.CreateShaderModule(
					readFile(kSsgiDiffuseComposePsPath), "ssgiDiffuseComposeFrag");
			// The copy blit: SSR's shader, the identical BDA contract
			// (SsgiDiffuseCopyPush is layout-identical to SsrCopyPush).
			if (copyFragModule.empty())
				copyFragModule = ctx.CreateShaderModule(readFile(kSsgiDiffuseCopyPsPath),
					"ssgiDiffuseCopyFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || traceCsModule.empty() || blurFragModule.empty() ||
			temporalFragModule.empty() || prevFragModule.empty() ||
			composeFragModule.empty() || copyFragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader traceCS{ .module = traceCsModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		Shader blurPS{ .module = blurFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader temporalPS{ .module = temporalFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader prevPS{ .module = prevFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader composePS{ .module = composeFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader copyPS{ .module = copyFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		traceProgram = { .shaders = { traceCS }, .shaderCount = 1 };
		blurProgram = { .shaders = { vs, blurPS }, .shaderCount = 2 };
		temporalProgram = { .shaders = { vs, temporalPS }, .shaderCount = 2 };
		prevProgram = { .shaders = { vs, prevPS }, .shaderCount = 2 };
		composeProgram = { .shaders = { vs, composePS }, .shaderCount = 2 };
		copyProgram = { .shaders = { vs, copyPS }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		traceProgram = ShaderProgram();
		blurProgram = ShaderProgram();
		temporalProgram = ShaderProgram();
		prevProgram = ShaderProgram();
		composeProgram = ShaderProgram();
		copyProgram = ShaderProgram();
		vsModule = {};
		traceCsModule = {};
		blurFragModule = {};
		temporalFragModule = {};
		prevFragModule = {};
		composeFragModule = {};
		copyFragModule = {};
	}

	// ---- target lifecycle (the ssr.h pattern) ----

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

	static bool CreateBuffer(ResourceManager& ctx, RenderGraph& rg,
		size_t bytes, const char* name, CBuf& out) {
		if (bytes == 0)
			return false;
		Holder<BufferHandle> newBuffer =
			ctx.CreateBuffer(bytes, BufferType_Storage, name);
		if (newBuffer.empty())
			return false;
		LogicalBufferHandle newLogical = rg.AddExternalBuffer(newBuffer, name);
		if (newLogical.empty())
			return false;
		CBuf b;
		b.buffer = std::move(newBuffer);
		b.logical = newLogical;
		out = std::move(b);
		return true;
	}

	static void DestroyBuffer(RenderGraph& rg, CBuf& b) {
		if (!b.logical.empty())
			rg.FreeBuffer(b.logical);
		b = CBuf{};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, Extent2D full,
		const SsgiDiffuseDesc& d) {
		if (full.width == 0 || full.height == 0)
			return false;
		DestroyTargets(rg);
		DestroyBuffers(rg);
		const Extent2D half = HalfExtentFor(full);
		const size_t pixels = (size_t)std::max(1u, half.width)
			* (size_t)std::max(1u, half.height);

		Target newRaw, newBlur, newHist0, newHist1, newComposed;
		Target newPrevDepth, newPrevNormal;
		if (!CreateTarget(ctx, rg, half, d.format, "ssgiDiffuseRaw", newRaw))
			return false;
		if (!CreateTarget(ctx, rg, half, d.format, "ssgiDiffuseBlur", newBlur)) {
			DestroyTarget(rg, newRaw);
			return false;
		}
		if (!CreateTarget(ctx, rg, half, d.format, "ssgiDiffuseHist0", newHist0)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			return false;
		}
		if (!CreateTarget(ctx, rg, half, d.format, "ssgiDiffuseHist1", newHist1)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newHist0);
			return false;
		}
		if (!CreateTarget(ctx, rg, full, d.format, "ssgiDiffuseComposed",
				newComposed)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			return false;
		}
		// The temporal history mirrors (depth R32F + oct normal RGBA8 —
		// the SSR prevDepth pattern + the diffuse normal strengthening).
		if (!CreateTarget(ctx, rg, full, Format::R32_Float,
				"ssgiDiffusePrevDepth", newPrevDepth)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg, newComposed);
			return false;
		}
		if (!CreateTarget(ctx, rg, full, Format::RGBA8_UNorm,
				"ssgiDiffusePrevNormal", newPrevNormal)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg, newComposed);
			DestroyTarget(rg, newPrevDepth);
			return false;
		}
		// Compute-chain handoff buffers: fully rewritten by the full
		// half-res dispatch every frame (no counter, no zero-fill
		// contract — see the header note).
		CBuf newRadiance, newReceiverData;
		if (!CreateBuffer(ctx, rg, pixels * 4u * sizeof(float),
				"ssgiDiffuseRadiance", newRadiance)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg, newComposed);
			DestroyTarget(rg, newPrevDepth);
			DestroyTarget(rg, newPrevNormal);
			return false;
		}
		if (!CreateBuffer(ctx, rg, pixels * sizeof(uint32_t),
				"ssgiDiffuseReceiver", newReceiverData)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			DestroyTarget(rg, newComposed);
			DestroyTarget(rg, newPrevDepth);
			DestroyTarget(rg, newPrevNormal);
			DestroyBuffer(rg, newRadiance);
			return false;
		}
		raw = std::move(newRaw);
		blurred = std::move(newBlur);
		history[0] = std::move(newHist0);
		history[1] = std::move(newHist1);
		composed = std::move(newComposed);
		prevDepth = std::move(newPrevDepth);
		prevNormal = std::move(newPrevNormal);
		radiance = std::move(newRadiance);
		receiverData = std::move(newReceiverData);
		extent = full;
		halfExtent = half;
		format = d.format;
		desc = d;
		std::fprintf(stderr,
			"[SSGI-diffuse] targets: full %ux%u -> half %ux%u (%s)\n",
			full.width, full.height, half.width, half.height, d.name);
		return EnsureShaders(ctx);
	}

	void DestroyTargets(RenderGraph& rg) {
		DestroyTarget(rg, raw);
		DestroyTarget(rg, blurred);
		DestroyTarget(rg, history[0]);
		DestroyTarget(rg, history[1]);
		DestroyTarget(rg, composed);
		DestroyTarget(rg, prevDepth);
		DestroyTarget(rg, prevNormal);
		extent = { 0, 0 };
		halfExtent = { 0, 0 };
	}

	void DestroyBuffers(RenderGraph& rg) {
		DestroyBuffer(rg, radiance);
		DestroyBuffer(rg, receiverData);
	}

	// Recreate only when full extent / format drift (live-updates desc
	// — the quality knobs need no realloc).
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D full,
		const SsgiDiffuseDesc& d) {
		if (TargetsValid() &&
				extent.width == full.width && extent.height == full.height &&
				format == d.format) {
			desc = d;
			return EnsureShaders(ctx);
		}
		return Create(ctx, rg, full, d);
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyBuffers(rg);
		DestroyShaders();
	}

	// ---- pass emission ----
	//
	// trace (compute, full half-res grid) -> copy (radiance BDA ->
	// raw) -> blur (raw -> blurred) -> [temporal (blurred + history +
	// motion + the mirrors -> history[parity])] -> compose (the GI
	// layer + the frozen HDR + the baseline -> the composed full-res
	// target) -> [history mirrors (depth + oct normal), temporal
	// records only — AFTER the temporal pass so this record's reads
	// see the previous copy].
	//
	// hdrView/hdrBindlessIndex: the FROZEN pre-compose scene HDR (the
	// post-AO source) — BOTH the trace's radiance source and the
	// compose's base; the compose writes its own target, so the graph
	// never sees a read/write hazard on the source.
	// gbuffer/gbuffer2: the shared thin G-buffer pair (SSR's targets —
	// the prepass wrote them because this chain OR'd its want into
	// fc.ssrGbufferWanted).
	// ambientView: the AO compose's diffuse-ambient baseline output —
	// empty when AO is off (the compose then derives the baseline
	// inline from aoTerm/envSpec with aoEff = 1).
	// temporal: false = the spatial-only record (compose samples the
	// blur; the caller's EndFrame step invalidates the history).
	// debugView: the compose shader's visualization selector (0 off).
	// debugGain: the views-2..5 diagnostic exposure gain (exp2(+EV),
	// 1.0f = none — see SsgiDiffuseComposePush::debugGain).
	void AddPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		LogicalImageViewHandle gbufferView,
		uint32_t gbufferBindlessIndex,
		LogicalImageViewHandle gbuffer2View,
		uint32_t gbuffer2BindlessIndex,
		LogicalImageViewHandle motionView,
		uint32_t motionBindlessIndex,
		uint64_t radianceAddr,
		uint64_t receiverDataAddr,
		uint64_t hzbAddr,
		uint32_t hzbMips,
		LogicalBufferHandle hzbLogical,
		uint32_t hzbMip1Offset,
		uint32_t hzbMip1Stride,
		LogicalImageViewHandle ambientView,
		uint32_t ambientBindlessIndex,
		LogicalImageViewHandle aoTermView,
		uint32_t aoTermBindlessIndex,
		LogicalImageViewHandle envSpecView,
		uint32_t envSpecBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		const SsgiDiffuseDesc& overrideDesc,
		float jitterDeltaX,
		float jitterDeltaY,
		bool historyValid,
		uint32_t parity,
		bool temporal,
		uint32_t frameIndex,
		uint32_t debugView,
		float debugGain,
		const char* traceName = "SsgiDiffuseTrace",
		const char* blurName = "SsgiDiffuseBlur",
		const char* temporalName = "SsgiDiffuseTemporal",
		const char* composeName = "SsgiDiffuseCompose") const {
		assert(Valid());
		assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
		assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
		assert(!gbufferView.empty() && gbufferBindlessIndex != UINT32_MAX);
		assert(frameUniformsAddr != 0);
		assert(radianceAddr != 0 && receiverDataAddr != 0);
		assert(hzbAddr != 0 && hzbMips != 0 && !hzbLogical.empty()
			&& hzbMip1Stride != 0);
		// EXTENT INVARIANTS (the dark-triangle lesson, the ssr.h pair):
		assert(halfExtent.width == std::max(1u, extent.width / 2u));
		assert(halfExtent.height == std::max(1u, extent.height / 2u));
		assert(hzbMip1Stride >= halfExtent.width);
		assert(parity < 2);

		const bool gbuffer2On = !gbuffer2View.empty()
			&& gbuffer2BindlessIndex != UINT32_MAX;
		const float fullW = static_cast<float>(std::max(1u, extent.width));
		const float fullH = static_cast<float>(std::max(1u, extent.height));

		// ---- Trace: the full half-res dispatch (one cosine ray per
		// pixel; every slot + receiver entry written every frame).
		{
			SsgiDiffuseTracePush push{};
			push.frameUniforms = frameUniformsAddr;
			push.radianceAddr = radianceAddr;
			push.receiverDataAddr = receiverDataAddr;
			push.hzbAddr = hzbAddr;
			push.hdrTextureIndex = hdrBindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.gbufferTextureIndex = gbufferBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.halfW = halfExtent.width;
			push.halfH = halfExtent.height;
			push.invDepthW = 1.0f / fullW;
			push.invDepthH = 1.0f / fullH;
			push.maxDistance = (std::max)(0.1f, overrideDesc.maxDistance);
			push.thickness = (std::max)(0.01f, overrideDesc.thickness);
			push.frameIndex = frameIndex; // the dither EPOCH (temporal convergence)
			push.maxIntersections = std::clamp(overrideDesc.maxIntersections, 4u, 128u);
			push.hzbMipCount = hzbMips;
			// The walk's finest level: 0 (FULL-RES refinement — the SSR
			// mirror path). SSR's rough rays stop at mip 1 as a perf
			// choice, but a diffuse hemisphere grazes surfaces at every
			// angle, and the thickness confidence measures the crossing
			// gap against the depth texel — block-level quantization
			// (mip 1) inflated that gap systematically and collapsed the
			// confidence to a few percent. Refining to mip 0 prices the
			// confidence against the real surface.
			push.mostDetailedMip = 0;
			push.pad0 = 0;
			push.pad1 = 0;
			RenderPass& pass = rg.AddComputePass(traceName ? traceName : "SsgiDiffuseTrace");
			{
				BufferSyncRequest w{};
				w.barrierMasks.accessMask = AccessFlags::ShaderWrite;
				w.barrierMasks.stageMask = PipelineStageBit_Compute;
				pass.AddBufferHint(radiance.logical, w);
				pass.AddBufferHint(receiverData.logical, w);
				BufferSyncRequest hzbRead{};
				hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				hzbRead.barrierMasks.stageMask = PipelineStageBit_Compute;
				pass.AddBufferHint(hzbLogical, hzbRead);
			}
			// The radiance/receiver buffers are read back by the copy
			// (fragment BDA) + the blur/compose (receiver BDA) — the
			// graph orders those via their own read hints.
			pass.SetInputImages({ hdrView, depthView, gbufferView });
			const uint32_t hw = halfExtent.width;
			const uint32_t hh = halfExtent.height;
			ShaderProgram progCopy = traceProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push, hw, hh](CommandBuffer& cmd) mutable {
				cmd.BindComputePipeline(progCopy);
				cmd.PushConstants(0, sizeof(SsgiDiffuseTracePush), &push,
					ShaderStage::Compute);
				cmd.Dispatch((hw + 7) / 8, (hh + 7) / 8, 1);
			});
		}

		// ---- Copy: the radiance buffer -> the raw target (the blur
		// reads it bilinearly, so it stays an image; SSR's blit).
		{
			SsgiDiffuseCopyPush push{};
			push.radianceAddr = radianceAddr;
			push.halfW = halfExtent.width;
			push.halfH = halfExtent.height;
			push.pad0 = 0;
			push.pad1 = 0;
			RenderPass& pass = rg.AddPass("SsgiDiffuseCopy");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent);
			{
				BufferSyncRequest radRead{};
				radRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				radRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
				pass.AddBufferHint(radiance.logical, radRead);
			}
			pass.SetColor({ raw.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = copyProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsgiDiffuseCopyPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Blur: raw -> blurred (fixed 5x5 joint-bilateral).
		{
			SsgiDiffuseBlurPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.hzb = hzbAddr;
			push.receiverData = receiverDataAddr;
			push.giTextureIndex = raw.bindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.texelX = 1.0f / static_cast<float>(std::max(1u, halfExtent.width));
			push.texelY = 1.0f / static_cast<float>(std::max(1u, halfExtent.height));
			push.hzbMip1Offset = hzbMip1Offset;
			push.hzbMip1Stride = hzbMip1Stride;
			push.halfW = halfExtent.width;
			push.halfH = halfExtent.height;
			push.pad0 = 0;
			RenderPass& pass = rg.AddPass(blurName ? blurName : "SsgiDiffuseBlur");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ raw.logical })
				.SetColor({ blurred.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			{
				BufferSyncRequest hzbRead{};
				hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				hzbRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
				pass.AddBufferHint(hzbLogical, hzbRead);
				BufferSyncRequest recvRead{};
				recvRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				recvRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
				pass.AddBufferHint(receiverData.logical, recvRead);
			}
			ShaderProgram progCopy = blurProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsgiDiffuseBlurPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Temporal (the optional TAA record): blurred + history
		// + motion + the mirrors -> history[parity]. The base HDR is
		// not an input: only the GI layer accumulates.
		const Target& giSource = temporal
			? history[parity & 1u] : blurred;
		if (temporal) {
			const Target& histPrev = history[(parity ^ 1u) & 1u];
			const Target& histCurr = history[parity & 1u];
			SsgiDiffuseTemporalPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.currTextureIndex = blurred.bindlessIndex;
			push.historyTextureIndex = histPrev.bindlessIndex;
			push.motionTextureIndex = motionBindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.prevDepthTextureIndex = prevDepth.bindlessIndex;
			push.prevNormalTextureIndex = prevNormal.bindlessIndex;
			push.gbufferTextureIndex = gbufferBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.historyValid = historyValid ? 1u : 0u;
			push.jitterDeltaX = jitterDeltaX;
			push.jitterDeltaY = jitterDeltaY;
			push.texelX = 1.0f / static_cast<float>(std::max(1u, halfExtent.width));
			push.texelY = 1.0f / static_cast<float>(std::max(1u, halfExtent.height));
			push.invDepthW = 1.0f / fullW;
			push.invDepthH = 1.0f / fullH;
			push.blend = std::clamp(overrideDesc.temporalBlend, 0.0f, 0.98f);
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.pad0 = 0;
			RenderPass& pass = rg.AddPass(temporalName ? temporalName : "SsgiDiffuseTemporal");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ blurred.logical, histPrev.logical, motionView,
					depthView, prevDepth.logical, prevNormal.logical,
					gbufferView })
				.SetColor({ histCurr.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = temporalProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsgiDiffuseTemporalPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- Compose: the GI layer + the frozen HDR + the baseline
		// -> the composed FULL-res target (the energy-conserving
		// replacement; the scene HDR pointer moves to OutputView()).
		{
			SsgiDiffuseComposePush push{};
			push.frameUniforms = frameUniformsAddr;
			push.hzb = hzbAddr;
			push.receiverData = receiverDataAddr;
			push.giTextureIndex = giSource.bindlessIndex;
			push.hdrTextureIndex = hdrBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.gbufferTextureIndex = gbufferBindlessIndex;
			push.gbuffer2TextureIndex = gbuffer2On
				? gbuffer2BindlessIndex : UINT32_MAX;
			// The baseline's sourcing mode (see the compose shader's
			// header): the AO compose's third output when bound (AO
			// ran), else Main's raw exports with aoEff = 1 (AO off).
			const bool ambientOn = !ambientView.empty()
				&& ambientBindlessIndex != UINT32_MAX;
			push.ambientTextureIndex =
				ambientOn ? ambientBindlessIndex : UINT32_MAX;
			push.aoTermTextureIndex =
				ambientOn ? UINT32_MAX : aoTermBindlessIndex;
			push.envSpecTextureIndex =
				ambientOn ? UINT32_MAX : envSpecBindlessIndex;
			push.rawTextureIndex = raw.bindlessIndex; // debug view 2
			push.invDepthW = 1.0f / fullW;
			push.invDepthH = 1.0f / fullH;
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
			push.intensity = (std::max)(0.0f, overrideDesc.intensity);
			push.fullW = extent.width;
			push.fullH = extent.height;
			push.hzbMip1Offset = hzbMip1Offset;
			push.hzbMip1Stride = hzbMip1Stride;
			push.debugView = debugView;
			push.debugGain = debugGain;
			push.pad1 = 0;

			std::vector<LogicalImageViewHandle> composeInputs =
				{ giSource.logical, hdrView, gbufferView, raw.logical };
			if (gbuffer2On)
				composeInputs.push_back(gbuffer2View);
			if (ambientOn)
				composeInputs.push_back(ambientView);
			else {
				composeInputs.push_back(aoTermView);
				composeInputs.push_back(envSpecView);
			}
			RenderPass& pass = rg.AddPass(composeName ? composeName : "SsgiDiffuseCompose");
			pass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages(composeInputs)
				.SetColor({ composed.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			{
				BufferSyncRequest hzbRead{};
				hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				hzbRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
				pass.AddBufferHint(hzbLogical, hzbRead);
				BufferSyncRequest recvRead{};
				recvRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
				recvRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
				pass.AddBufferHint(receiverData.logical, recvRead);
			}
			ShaderProgram progCopy = composeProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsgiDiffuseComposePush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

		// ---- The history mirrors (TEMPORAL records only): depth +
		// oct normal, one copy pass, AFTER the temporal pass's reads
		// (this record refreshes them for the NEXT record's test —
		// the ssr_depth_history discipline; the warmup record never
		// reads them, so a stale first copy is harmless).
		if (temporal) {
			SsgiDiffusePrevPush push{};
			push.depthTextureIndex = depthBindlessIndex;
			push.gbufferTextureIndex = gbufferBindlessIndex;
			push.pad0 = 0;
			push.pad1 = 0;
			RenderPass& pass = rg.AddPass("SsgiDiffuseHistory");
			pass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages({ depthView, gbufferView })
				.SetColor({ prevDepth.logical, prevNormal.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = prevProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexLayoutId::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsgiDiffusePrevPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}
	}
};

} // namespace smvk
