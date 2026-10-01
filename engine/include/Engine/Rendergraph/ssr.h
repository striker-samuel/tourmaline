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
//              a center across a silhouette) ->
//              depth-derived normal
//              from CENTRAL differences over a ±3 full-res px baseline
//              (piecewise-planar depth makes one-sided 2px slopes step
//              at every triangle edge — tessellated mirrors read their
//              own wireframe; per-tap background same-depth fallback is
//              sign-safe by construction) -> screen-stride
//              adaptive view-space march (~6 px strides: dense where
//              footprints are small so strides can't straddle occluders,
//              long where the ray drills in depth) with slab acceptance
//              PLUS front-to-behind segment-crossing bisect + binary
//              refine + hit backface rejection (wrong-side hits show
//              occluded geometry through surfaces, so the march rejects
//              and continues instead of sampling them).
//              Output RGBA16F: rgb = reflected HDR x Fresnel x intensity,
//              a = edge x distance confidence (0 on a miss). Rays that
//              hit nothing contribute nothing: the base HDR already
//              carries the analytic sky specular IBL, so isolated mirrors
//              (chrome probe balls, open atriums) keep their chrome read
//              without double-counting the sky.
//   blur:      5x5 bilateral blur of (rgb, a) at half res, depth-weighted
//              with exact view distances (flip / finite-far agnostic via
//              invProj, same discipline as dof.hlsli) so edges don't bleed.
//   temporal:  (optional TAA) motion-vector-reprojected accumulation of
//              the blurred layer at half res, scoped to SSR only — the
//              base HDR is never filtered. History lives in two ping-pong
//              targets; rejection (off-screen reproject, disocclusion,
//              invalid history) falls back to the current blurred tap.
//   composite: FULL-res base HDR + depth-guided 2x2 upsample of the
//              blurred (rgb, a) with full-res depth authority. Background
//              pixels keep the base color unconditionally. An NDC
//              vignette on the target pixel fades the added reflection
//              over the outer 15% of the NDC range (screen-edge
//              contributions are the smear band).
// Placement: after Main + MotionBlur + DOF, before Exposure/Bloom/Tonemap.
// Input is the scene HDR (post-DOF), output is the same-extent HDR with
// reflections composited additively. Downstream passes sample the SSR
// output when the pass ran, else the input directly (no copy disabled).
//
// Receiver-material layer: the SSSR-style THIN G-BUFFER (full-res
// RGBA8: RG = octahedral world normal, B = roughness, A = metal — see
// gbuffer.frag.hlsl), written by the Forward+ DEPTH PREPASS's
// G-buffer FS pair (the prepass already rasterizes every opaque
// surface; the FS is trivial — no textures, no lighting — so the
// G-buffer costs a few ALU ops + 4 bytes/px on a pass that exists,
// and MAIN IS UNTOUCHED: no MRT, no stencil classification. The
// short-lived Main MRT roughness write and the stencil-byte roughness
// encoding are both GONE — the MRT made Main measurably more
// expensive, the stencil carried no normals).
//   - trace: reads REAL interpolated normals from the G-buffer (the
//     depth-derived receiver normal — piecewise-planar per triangle,
//     the wireframe source — is GONE by construction), continuous
//     roughness for the (1-r)^2 gloss fade + the r^2 cone jitter
//     (rough receivers smear the lobe — the screen-space diffuse-GI
//     read), metal raises F0 to ~1. Unbound G-buffer (SSR off /
//     MSAA / legacy) = perfect-mirror uniform-F0 legacy behavior.
//   - composite: NDC vignette fades edge contributions.
//
// Positions still come from the 1x prepass depth (no GBuffer for
// world positions — the thin G-buffer carries normal + material
// only, exactly the SSSR input set):
//   - view positions via FrameUniforms.invProj (exact, flip/finite-far
//     agnostic — same discipline as ssao.h / dof.h),
//   - receiver normals from the G-buffer (oct-decoded, world space),
//   - Fresnel from F0 = lerp(uniform dielectric F0, 1, metal)
//     (glass default 0.04). Every dielectric surface reflects with the
//     same base F0; the per-receiver G-buffer values (continuous
//     roughness + metal) are the material gate — rough receivers fade
//     the lobe (1-r)^2 and jitter the ray cone r^2, mirrors keep it
//     sharp. Bistro windows (large vertical glass planes at grazing
//     street-view angles, double-sided) are the calibration case: aim
//     the default Bistro camera down the road at the shopfront glass
//     and raise intensity until the opposite facades read in the panes.
//     reflect() is invariant under N -> -N, so the viewer-facing
//     normal flip is safe for double-sided glass by construction.
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

// Must match ssr.frag.hlsl SsrTracePush (80 bytes — the uint64 frame
// BDA + the trailing uint64 HZB BDA keep every 8-byte member aligned;
// pad0 lands the block on a multiple of 8).
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
	uint32_t frameIndex = 0; // dither epoch (0 = static legacy; temporal path passes the record counter)
	uint32_t gbufferTextureIndex = UINT32_MAX; // thin G-buffer (RGBA8 bindless); UINT32_MAX = legacy mirror
	uint32_t pad0 = 0;        // offset 60..64 (pad before the 8-aligned HZB BDA)
	uint64_t ssrHzbAddr = 0;  // BDA of the MAX-reduce trace Hi-Z (HzbBuffer layout); 0 = linear march
	uint32_t ssrHzbMipCount = 0; // mips in the pyramid; 0 = linear march
	uint32_t pad1 = 0;
};
static_assert(sizeof(SsrTracePush) == 80);

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
	uint32_t paneDepthTextureIndex = UINT32_MAX; // pane receiver depth (R16_UNorm bindless); UINT32_MAX = sky early-out unconditional
	uint32_t pad2 = 0;
};
static_assert(sizeof(SsrComposePush) == 48);

// Temporal (TAA) state for the SSR reflection layer: one per renderer,
// advanced once per recorded frame (record cadence, never per-image —
// same discipline as SmaaT2xState). parity selects the history slot the
// next record writes; historyValid gates the blend (false = warmup
// passthrough of the current blurred tap, which still fills the slot).
struct SsrTaaState {
	uint32_t parity = 0;
	bool historyValid = false;
};

struct SsrTaaFrameParams {
	uint32_t parity = 0;
	bool historyValid = false;
};

// Record-time snapshot (runs before Execute). Zeros when TAA is off.
inline SsrTaaFrameParams SsrTaaBeginFrame(const SsrTaaState& st, bool taaActive) {
	SsrTaaFrameParams p{};
	if (!taaActive)
		return p;
	p.parity = st.parity & 1u;
	p.historyValid = st.historyValid;
	return p;
}

// End-of-record step (runs after Execute). ranTemporal mirrors the
// temporal-emission gate: a non-temporal record freezes the phase and
// invalidates, so the next temporal record warms up instead of blending
// against stale history.
inline void SsrTaaEndFrame(SsrTaaState& st, bool ranTemporal) {
	if (!ranTemporal) {
		st.historyValid = false;
		return;
	}
	st.historyValid = true;
	st.parity ^= 1u;
}

// Explicit invalidation for history-content loss the toggle cannot see:
// extent/format drift (targets recreated) and motion-vector loss. Resets
// parity so the ping-pong restarts at slot 0.
inline void SsrTaaInvalidateHistory(SsrTaaState& st) {
	st.historyValid = false;
	st.parity = 0;
}

// Must match ssr_temporal.frag.hlsl SsrTemporalPush (64 bytes).
// All-scalar layout on purpose: float2 members would land at offsets
// that are not 8-byte aligned — spirv-val rejects the module
// ("improperly straddling vector"), same hazard as SsrBlurPush.
struct SsrTemporalPush {
	uint64_t frameUniforms = 0; // BDA of the FrameUniforms slice (exact view distances)
	uint32_t currTextureIndex = UINT32_MAX; // half-res blurred reflection (this frame)
	uint32_t historyTextureIndex = UINT32_MAX; // half-res temporal history (previous record)
	uint32_t motionTextureIndex = UINT32_MAX; // render-extent RG16F motion (prevUV - currUV)
	uint32_t depthTextureIndex = UINT32_MAX; // full-res depth (authority + disocclusion)
	uint32_t samplerIndex = 0; // linear clamp
	uint32_t historyValid = 0; // 0 = warmup passthrough of curr
	float jitterDeltaX = 0.0f; // (prevJitter - currJitter) UV (attenuation only)
	float jitterDeltaY = 0.0f;
	float texelX = 0.0f; // 1 / half-res width
	float texelY = 0.0f; // 1 / half-res height
	float invDepthW = 0.0f; // 1 / full-res depth width
	float invDepthH = 0.0f; // 1 / full-res depth height
	float blend = 0.9f; // history weight 0..0.98 (before attenuation)
	float sharpness = 10.0f; // depth edge weight (higher = tighter)
};
static_assert(sizeof(SsrTemporalPush) == 64);

// Default SPVs (fullscreen-triangle VS shared with tonemap + SSR PS trio).
inline constexpr const char* kSsrVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kSsrTracePsPath = "content/shaders/ssr.frag.hlsl.spv";
inline constexpr const char* kSsrBlurPsPath = "content/shaders/ssr_blur.frag.hlsl.spv";
inline constexpr const char* kSsrTemporalPsPath = "content/shaders/ssr_temporal.frag.hlsl.spv";
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
	// SSSR-style thin G-buffer (see gbuffer.frag.hlsl): full-res RGBA8
	// (RG = octahedral world normal, B = roughness, A = metal), written
	// by the APP's Forward+ depth prepass (the G-buffer FS pair rides
	// a pass that already rasterizes every opaque surface — Main is
	// untouched). The trace reads REAL interpolated normals (no
	// depth-derived receiver normals — the piecewise-planar wireframe
	// source is gone by construction) + continuous roughness + metal.
	// Clear (0.5, 0.5, 1, 0) = "no data" decodes as +Z/matte/non-metal
	// (a safe non-reflector); temporal-split frames Load last frame's
	// values (static normals never change).
	Target gbuffer;
	// Half-res temporal history ping-pong (TAA of the reflection layer).
	// history[parity] is written by the temporal pass each temporal
	// record; the composite samples the just-written slot. historyValid
	// (SsrTaaState, owned by the app) gates the blend — a warmup record
	// fills the slot with the current blurred tap instead.
	Target history[2];
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
	Holder<ShaderModuleHandle> temporalFragModule;
	Holder<ShaderModuleHandle> composeFragModule;
	ShaderProgram traceProgram{};
	ShaderProgram blurProgram{};
	ShaderProgram temporalProgram{};
	ShaderProgram composeProgram{};
	SsrDesc desc{};

	[[nodiscard]] bool TargetsValid() const {
		return raw.Valid() && blurred.Valid() && gbuffer.Valid() &&
			history[0].Valid() && history[1].Valid() &&
			!image.empty() && !logicalView.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return traceProgram.shaderCount != 0 && blurProgram.shaderCount != 0 &&
			temporalProgram.shaderCount != 0 && composeProgram.shaderCount != 0 &&
			!vsModule.empty() && !traceFragModule.empty() && !blurFragModule.empty() &&
			!temporalFragModule.empty() && !composeFragModule.empty();
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
		const char* temporalPath = kSsrTemporalPsPath,
		const char* composePath = kSsrComposePsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kSsrVsPath;
			const std::string trace = tracePath ? tracePath : kSsrTracePsPath;
			const std::string blur = blurPath ? blurPath : kSsrBlurPsPath;
			const std::string temporal = temporalPath ? temporalPath : kSsrTemporalPsPath;
			const std::string compose = composePath ? composePath : kSsrComposePsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "ssrVert");
			if (traceFragModule.empty())
				traceFragModule = ctx.CreateShaderModule(readFile(trace), "ssrTraceFrag");
			if (blurFragModule.empty())
				blurFragModule = ctx.CreateShaderModule(readFile(blur), "ssrBlurFrag");
			if (temporalFragModule.empty())
				temporalFragModule = ctx.CreateShaderModule(readFile(temporal), "ssrTemporalFrag");
			if (composeFragModule.empty())
				composeFragModule = ctx.CreateShaderModule(readFile(compose), "ssrComposeFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || traceFragModule.empty() || blurFragModule.empty() ||
			temporalFragModule.empty() || composeFragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader tracePS{ .module = traceFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader blurPS{ .module = blurFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader temporalPS{ .module = temporalFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		Shader composePS{ .module = composeFragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		traceProgram = { .shaders = { vs, tracePS }, .shaderCount = 2 };
		blurProgram = { .shaders = { vs, blurPS }, .shaderCount = 2 };
		temporalProgram = { .shaders = { vs, temporalPS }, .shaderCount = 2 };
		composeProgram = { .shaders = { vs, composePS }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		traceProgram = ShaderProgram{};
		blurProgram = ShaderProgram{};
		temporalProgram = ShaderProgram{};
		composeProgram = ShaderProgram{};
		vsModule = {};
		traceFragModule = {};
		blurFragModule = {};
		temporalFragModule = {};
		composeFragModule = {};
	}

	static bool CreateTarget(ResourceManager& ctx, RenderGraph& rg,
		Extent2D ext, Format format, const char* name, Target& out) {
		if (ext.width == 0 || ext.height == 0)
			return false;
		ImageCreateInfo ci{};
		ci.extent = { ext.width, ext.height, 1 };
		ci.format = format;
		// TransferSrc: debug stage dumps (VULKANTOY_SSR_DUMP) copy the
		// targets back through DownloadDataFromImage — armed dumps only.
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
		if (DebugImageDumpArmed())
			ci.usage |= ImageUsage::TransferSrc;
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
		// TransferSrc: debug stage dumps (VULKANTOY_SSR_DUMP) — armed dumps
		// only (DownloadDataFromImage needs it on the source image).
		ci.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
		if (DebugImageDumpArmed())
			ci.usage |= ImageUsage::TransferSrc;
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
		const char* temporalPath = kSsrTemporalPsPath,
		const char* composePath = kSsrComposePsPath) {
		if (full.width == 0 || full.height == 0)
			return false;
		DestroyTargets(rg);
		const Extent2D half = HalfExtentFor(full);
		Target newRaw, newBlur, newGbuffer, newHist0, newHist1;
		if (!CreateTarget(ctx, rg, half, d.format, "ssrRaw", newRaw))
			return false;
		if (!CreateTarget(ctx, rg, half, d.format, "ssrBlur", newBlur)) {
			DestroyTarget(rg, newRaw);
			return false;
		}
		// SSSR thin G-buffer: full-res RGBA8 (oct normal + roughness +
		// metal — see gbuffer.frag.hlsl). The depth prepass attaches it.
		if (!CreateTarget(ctx, rg, full, Format::RGBA8_UNorm,
				"ssrGbuffer", newGbuffer)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			return false;
		}
		if (!CreateTarget(ctx, rg, half, d.format, "ssrHist0", newHist0)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			return false;
		}
		if (!CreateTarget(ctx, rg, half, d.format, "ssrHist1", newHist1)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newHist0);
			return false;
		}
		if (!CreateTarget(ctx, rg, full, d)) {
			DestroyTarget(rg, newRaw);
			DestroyTarget(rg, newBlur);
			DestroyTarget(rg, newGbuffer);
			DestroyTarget(rg, newHist0);
			DestroyTarget(rg, newHist1);
			return false;
		}
		raw = std::move(newRaw);
		blurred = std::move(newBlur);
		gbuffer = std::move(newGbuffer);
		history[0] = std::move(newHist0);
		history[1] = std::move(newHist1);
		halfExtent = half;
		format = d.format;
		desc = d;
		return EnsureShaders(ctx, vsPath, tracePath, blurPath, temporalPath, composePath);
	}

	void DestroyTargets(RenderGraph& rg) {
		DestroyTarget(rg, raw);
		DestroyTarget(rg, blurred);
		DestroyTarget(rg, gbuffer);
		DestroyTarget(rg, history[0]);
		DestroyTarget(rg, history[1]);
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
		const char* temporalPath = kSsrTemporalPsPath,
		const char* composePath = kSsrComposePsPath) {
		if (TargetsValid() &&
				extent.width == targetExtent.width && extent.height == targetExtent.height &&
				format == d.format) {
			desc = d;
			return EnsureShaders(ctx, vsPath, tracePath, blurPath, temporalPath, composePath);
		}
		return Create(ctx, rg, targetExtent, d, vsPath, tracePath, blurPath, temporalPath, composePath);
	}

	void Destroy(RenderGraph& rg) {
		DestroyTargets(rg);
		DestroyShaders();
	}

// Emits trace (HDR + full-res depth [+ stencil] -> half-res reflection)
// + blur (half-res reflection + full-res depth -> half-res) + composite
// (half-res reflection + full-res depth + full-res HDR -> full-res HDR).
// gbufferView/gbufferBindlessIndex: the thin G-buffer (RGBA8 — oct
// normal + roughness + metal, see gbuffer.frag.hlsl) written by the
// app's Forward+ depth prepass. Empty view / invalid index = legacy
// mirror (perfect-mirror uniform-F0).
// hzbAddr/hzbMips/hzbLogical: the MAX-reduce trace Hi-Z (see
// hzb_max.cs.hlsl — the NEAREST-surface pyramid over the complete
// renderDepth, built by the app AFTER all depth passes). 0/0/empty =
// the plain adaptive-stride linear march (the empty-space skip stays
// off). hzbLogical carries the buffer READ dependency so the graph
// barriers the SsrHzb compute writes before the trace's fragment
// reads — without it there is no ordering/visibility edge and the
// trace races the pyramid build.
// Captures only copyable handles/indices, so the passes are safe once
// this call returns.
void AddSsrPasses(RenderGraph& rg,
	LogicalImageViewHandle hdrView,
	uint32_t hdrBindlessIndex,
	LogicalImageViewHandle depthView,
	uint32_t depthBindlessIndex,
	LogicalImageViewHandle gbufferView = LogicalImageViewHandle{},
	uint32_t gbufferBindlessIndex = UINT32_MAX,
	uint64_t hzbAddr = 0,
	uint32_t hzbMips = 0,
	LogicalBufferHandle hzbLogical = LogicalBufferHandle{},
	uint64_t frameUniformsAddr = 0,
	uint32_t linearSamplerIndex = 0,
	const SsrDesc& overrideDesc = SsrDesc{},
	const char* traceName = "SSR",
	const char* blurName = "SSRBlur",
	const char* composeName = "SSRComposite",
	uint32_t frameIndex = 0) const {
	assert(Valid());
	assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
	assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
	assert(frameUniformsAddr != 0);
	assert(extent.width != 0 && extent.height != 0);
	assert(halfExtent.width != 0 && halfExtent.height != 0);

	const bool gbufferOn = !gbufferView.empty() && gbufferBindlessIndex != UINT32_MAX;
	const bool hzbOn = hzbAddr != 0 && hzbMips != 0 && !hzbLogical.empty();
	const float fullW = static_cast<float>(std::max(1u, extent.width));
	const float fullH = static_cast<float>(std::max(1u, extent.height));
	const float halfW = static_cast<float>(std::max(1u, halfExtent.width));
	const float halfH = static_cast<float>(std::max(1u, halfExtent.height));

	// ---- Trace: HDR + full-res depth [+ gbuffer] -> half-res reflection ----
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
	push.frameIndex = frameIndex;
	push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;
	push.ssrHzbAddr = hzbOn ? hzbAddr : 0;
	push.ssrHzbMipCount = hzbOn ? hzbMips : 0u;

	std::vector<LogicalImageViewHandle> traceInputs = { hdrView, depthView };
	if (gbufferOn)
		traceInputs.push_back(gbufferView);
	RenderPass& pass = rg.AddPass(traceName ? traceName : "SSR");
	if (hzbOn) {
		// The trace Hi-Z read dependency: barriers the app's per-mip
		// SsrHzb compute writes (RW, compute stage) before these
		// fragment reads — the BDA alone carries no ordering.
		BufferSyncRequest hzbRead{};
		hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		hzbRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
		pass.AddBufferHint(hzbLogical, hzbRead);
	}
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages(traceInputs)
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

	// ---- Composite: blurred half-res + full-res depth + HDR ----
	// -> HDR. The depth-guided upsample runs HERE, in a pass that never
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

		std::vector<LogicalImageViewHandle> composeInputs = { blurred.logical, depthView, hdrView };
		RenderPass& pass = rg.AddPass(composeName ? composeName : "SSRComposite");
			pass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages(composeInputs)
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

// Temporal (TAA) variant: trace + blur (identical pushes to the
// spatial path above, stencil included), then the
// temporal pass (blurred + history[parity^1] + motion + depth ->
// history[parity]), then the composite sampling the just-written
// history slot instead of the blurred target. The base HDR path is
// untouched — only the reflection layer is temporally filtered.
// historyValid == false warms up: the temporal pass copies curr into
// the slot so the next record has something to blend against.
// Captures only copyable handles/indices, so the passes are safe once
// this call returns.
void AddSsrTemporalPasses(RenderGraph& rg,
	LogicalImageViewHandle hdrView,
	uint32_t hdrBindlessIndex,
	LogicalImageViewHandle depthView,
	uint32_t depthBindlessIndex,
	LogicalImageViewHandle gbufferView,
	uint32_t gbufferBindlessIndex,
	uint64_t hzbAddr,
	uint32_t hzbMips,
	LogicalBufferHandle hzbLogical,
	LogicalImageViewHandle motionView,
	uint32_t motionBindlessIndex,
	uint64_t frameUniformsAddr,
	uint32_t linearSamplerIndex,
	const SsrDesc& overrideDesc,
	float jitterDeltaX,
	float jitterDeltaY,
	float taaBlend,
	bool historyValid,
	uint32_t parity,
	const char* traceName = "SSR",
	const char* blurName = "SSRBlur",
	const char* temporalName = "SSRTemporal",
	const char* composeName = "SSRComposite",
	uint32_t frameIndex = 0) const {
	assert(Valid());
	assert(!hdrView.empty() && hdrBindlessIndex != UINT32_MAX);
	assert(!depthView.empty() && depthBindlessIndex != UINT32_MAX);
	assert(!motionView.empty() && motionBindlessIndex != UINT32_MAX);
	assert(frameUniformsAddr != 0);
	assert(extent.width != 0 && extent.height != 0);
	assert(halfExtent.width != 0 && halfExtent.height != 0);
	assert(parity < 2);

	const bool gbufferOn = !gbufferView.empty() && gbufferBindlessIndex != UINT32_MAX;
	const bool hzbOn = hzbAddr != 0 && hzbMips != 0 && !hzbLogical.empty();
	const float fullW = static_cast<float>(std::max(1u, extent.width));
	const float fullH = static_cast<float>(std::max(1u, extent.height));
	const float halfW = static_cast<float>(std::max(1u, halfExtent.width));
	const float halfH = static_cast<float>(std::max(1u, halfExtent.height));

	// ---- Trace: HDR + full-res depth [+ gbuffer] -> half-res reflection ----
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
	push.frameIndex = frameIndex;
	push.gbufferTextureIndex = gbufferOn ? gbufferBindlessIndex : UINT32_MAX;
	push.ssrHzbAddr = hzbOn ? hzbAddr : 0;
	push.ssrHzbMipCount = hzbOn ? hzbMips : 0u;

	std::vector<LogicalImageViewHandle> traceInputs = { hdrView, depthView };
	if (gbufferOn)
		traceInputs.push_back(gbufferView);
	RenderPass& pass = rg.AddPass(traceName ? traceName : "SSR");
	if (hzbOn) {
		// The trace Hi-Z read dependency (see AddSsrPasses).
		BufferSyncRequest hzbRead{};
		hzbRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		hzbRead.barrierMasks.stageMask = PipelineStageBit_Fragment;
		pass.AddBufferHint(hzbLogical, hzbRead);
	}
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages(traceInputs)
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

		// ---- Temporal: blurred + history[parity^1] + motion + depth ----
		// -> history[parity]. The base HDR is not an input here: only
		// the reflection layer accumulates.
		const Target& histPrev = history[(parity ^ 1u) & 1u];
		const Target& histCurr = history[parity & 1u];
		{
			SsrTemporalPush push{};
			push.frameUniforms = frameUniformsAddr;
			push.currTextureIndex = blurred.bindlessIndex;
			push.historyTextureIndex = histPrev.bindlessIndex;
			push.motionTextureIndex = motionBindlessIndex;
			push.depthTextureIndex = depthBindlessIndex;
			push.samplerIndex = linearSamplerIndex;
			push.historyValid = historyValid ? 1u : 0u;
			push.jitterDeltaX = jitterDeltaX;
			push.jitterDeltaY = jitterDeltaY;
			push.texelX = 1.0f / halfW;
			push.texelY = 1.0f / halfH;
			push.invDepthW = 1.0f / fullW;
			push.invDepthH = 1.0f / fullH;
			push.blend = std::clamp(taaBlend, 0.0f, 0.98f);
			push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);

			RenderPass& pass = rg.AddPass(temporalName ? temporalName : "SSRTemporal");
			pass.SetOffset(0, 0)
				.SetExtent(halfExtent)
				.SetInputImages({ blurred.logical, histPrev.logical, motionView, depthView })
				.SetColor({ histCurr.logical },
					AttachmentLoadOp::DontCare,
					AttachmentStoreOp::Store);
			ShaderProgram progCopy = temporalProgram; // copyable (handles only)
			pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
				cmd.BindRenderPipeline(
					progCopy,
					VertexFormat::Empty,
					DepthSettings::Disabled(),
					StencilSettings::DefaultDisabled(),
					BlendSettings::Opaque(),
					MSAASettings::Count1(),
					RasterSettings{ .cullMode = CullMode::None });
				cmd.PushConstants(0, sizeof(SsrTemporalPush), &push, ShaderStage::All);
				cmd.Draw(3, 1, 0, 0); // fullscreen triangle
			});
		}

	// ---- Composite: temporal half-res + full-res depth + HDR ----
	// -> HDR. Same depth-guided upsample as the spatial path, but the
	// reflection source is the just-written history slot.
	{
		SsrComposePush push{};
		push.frameUniforms = frameUniformsAddr;
		push.blurTextureIndex = histCurr.bindlessIndex;
		push.depthTextureIndex = depthBindlessIndex;
		push.hdrTextureIndex = hdrBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.invDepthW = 1.0f / fullW;
		push.invDepthH = 1.0f / fullH;
		push.sharpness = (std::max)(0.0f, overrideDesc.blurSharpness);
	push.pad0 = 0.0f;

		std::vector<LogicalImageViewHandle> composeInputs = { histCurr.logical, depthView, hdrView };
		RenderPass& pass = rg.AddPass(composeName ? composeName : "SSRComposite");
			pass.SetOffset(0, 0)
				.SetExtent(extent)
				.SetInputImages(composeInputs)
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

	// Temporal variant using the stored desc from Ensure().
	void AddSsrTemporalPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		LogicalImageViewHandle gbufferView,
		uint32_t gbufferBindlessIndex,
		uint64_t hzbAddr,
		uint32_t hzbMips,
		LogicalBufferHandle hzbLogical,
		LogicalImageViewHandle motionView,
		uint32_t motionBindlessIndex,
		uint64_t frameUniformsAddr,
		uint32_t linearSamplerIndex,
		float jitterDeltaX,
		float jitterDeltaY,
		float taaBlend,
		bool historyValid,
		uint32_t parity,
		const char* traceName = "SSR",
		const char* blurName = "SSRBlur",
		const char* temporalName = "SSRTemporal",
		const char* composeName = "SSRComposite",
		uint32_t frameIndex = 0) const {
		AddSsrTemporalPasses(rg, hdrView, hdrBindlessIndex, depthView, depthBindlessIndex,
			gbufferView, gbufferBindlessIndex,
			hzbAddr, hzbMips, hzbLogical,
			motionView, motionBindlessIndex,
			frameUniformsAddr, linearSamplerIndex, desc,
			jitterDeltaX, jitterDeltaY, taaBlend, historyValid, parity,
			traceName, blurName, temporalName, composeName, frameIndex);
	}

	// Uses the stored desc from Ensure().
	void AddSsrPasses(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle depthView,
		uint32_t depthBindlessIndex,
		LogicalImageViewHandle gbufferView = LogicalImageViewHandle{},
		uint32_t gbufferBindlessIndex = UINT32_MAX,
		uint64_t hzbAddr = 0,
		uint32_t hzbMips = 0,
		LogicalBufferHandle hzbLogical = LogicalBufferHandle{},
		uint64_t frameUniformsAddr = 0,
		uint32_t linearSamplerIndex = 0,
		const char* traceName = "SSR",
		const char* blurName = "SSRBlur",
		const char* composeName = "SSRComposite") const {
		AddSsrPasses(rg, hdrView, hdrBindlessIndex, depthView, depthBindlessIndex,
			gbufferView, gbufferBindlessIndex,
			hzbAddr, hzbMips, hzbLogical,
			frameUniformsAddr, linearSamplerIndex, desc,
			traceName, blurName, composeName);
	}
};

} // namespace smvk
