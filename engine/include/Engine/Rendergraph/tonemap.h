// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// HDR -> LDR tonemapper (fullscreen triangle, ACES / Reinhard / Uncharted2 / GT7).
//
// Ports the pattern used by BloomChain / CascadedShadowAtlas into one place:
// default shader program ownership + push layout + rendergraph pass setup.
// Sampling is bindless (textures[] indexed by push), barriers come from
// SetColor (write) + SetInputImages (read).
//
// Composite: hdr += bloom * intensity (pre-tonemap), then Tonemap(hdr * exposure)
// then optional artistic LUT (TPP-style 32^3 strip, linear) then straight
// linear LDR into the sRGB swapchain (the hardware encodes on store).
// gammaEncode=1 is the exception: the pass then writes sRGB-encoded
// bytes itself, for a UNORM LDR intermediate (the SMAA chain - see smaa.h) or
// when the surface fell back to a non-sRGB format. Bloom disabled when
// bloomTextureIndex == UINT32_MAX.
//
// Usage (app/src/main.cpp):
//   smvk::Tonemapper tonemap;
//   tonemap.Ensure(ctx, desc);                        // default programs
//   tonemap.AddTonemapPass(rg, hdrView, hdrBindless,  // HDR + optional bloom
//                          bloomView, bloomBindless, bloomIntensity,
//                          linearSampler, swapViews, swapExtent);

#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

// Must match tonemap.frag.hlsl TonemapPush (64 bytes).
struct TonemapPush {
	uint32_t hdrTextureIndex = UINT32_MAX;
	uint32_t samplerIndex = 0;
	float exposure = 0.02f;
	uint32_t tonemapMode = 1; // 0=Reinhard, 1=ACES, 2=Uncharted2, 3=GT7
	uint32_t bloomTextureIndex = UINT32_MAX; // UINT32_MAX = disabled
	float bloomIntensity = 0.0f;
	uint32_t debugView = 0; // 0=scene, 1=motion vectors (motionTextureIndex)
	uint32_t motionTextureIndex = UINT32_MAX; // UINT32_MAX = disabled
	float motionDebugScale = 16.0f; // velocity amplification for debugView 1
	uint32_t gammaEncode = 0; // 1 = write encoded bytes (UNORM LDR target), 0 = linear into the sRGB swapchain
	// Half-res SSAO composite (pre-bloom, pre-tonemap):
	// hdr *= lerp(1, ao, aoIntensity). UINT32_MAX = disabled.
	uint32_t aoTextureIndex = UINT32_MAX;
	float aoIntensity = 0.0f;
	// Artistic grading: TPP-style 32^3 LUT strip (1024x32) sampled in
	// linear post-tonemap, pre-encode. UINT32_MAX = disabled.
	uint32_t lutTextureIndex = UINT32_MAX;
	float lutIntensity = 0.0f;
	// debugView 1 only: jitter component of the motion vectors
	// ((prevJitter - currJitter) in UV), stripped so the view shows TRUE
	// motion - with temporal jitter the raw vectors are non-zero on a
	// still camera (they must carry the offset for SMAA T2x / DLSS).
	float jitterDeltaX = 0.0f;
	float jitterDeltaY = 0.0f;
	// Sub-rect source window (element stacking): the HDR image can be
	// larger than this pass's rect (a shared full-window HDR with
	// per-viewport layers); xy = min uv, zw = max uv - the fullscreen
	// triangle's 0..1 uv remaps into this window before every sample.
	// Default (0,0,1,1) = the full image (identity). Callers pass TEXEL
	// CENTERS of the source rect so linear taps never bleed the content
	// just outside the rect: (x + 0.5)/W .. (x + w - 0.5)/W etc.
	float srcUvMinX = 0.0f;
	float srcUvMinY = 0.0f;
	float srcUvMaxX = 1.0f;
	float srcUvMaxY = 1.0f;
	// Clipping debug views (debugView 2..4): the display-encoded SDR luma
	// thresholds — shadow at/below, highlight at/above. Must stay
	// layout-identical to the HLSL mirror (the appended tail).
	float shadowClipThreshold = 0.01f;
	float highlightClipThreshold = 0.99f;
};
static_assert(sizeof(TonemapPush) == 88);

inline constexpr uint32_t kTonemapReinhard = 0;
inline constexpr uint32_t kTonemapACES = 1;
inline constexpr uint32_t kTonemapUncharted2 = 2;
inline constexpr uint32_t kTonemapGT7 = 3;

struct TonemapDesc {
	float exposure = 0.02f;
	uint32_t tonemapMode = kTonemapACES;
	// Clipping debug views (debugView 2..4): display-encoded SDR luma
	// thresholds, carried into the push by AddTonemapPass.
	float shadowClipThreshold = 0.01f;
	float highlightClipThreshold = 0.99f;
	const char* name = "Tonemap";
};

// Default SPVs (fullscreen-triangle VS + tonemap PS).
inline constexpr const char* kTonemapVsPath = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kTonemapPsPath = "content/shaders/tonemap.frag.hlsl.spv";

struct Tonemapper {
	// Default program (tonemap.vert mainVS + tonemap.frag mainPS). Created by
	// EnsureShaders/Ensure so apps don't own tonemap modules.
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> fragModule;
	ShaderProgram program{};
	TonemapDesc desc{};

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount != 0 && !vsModule.empty() && !fragModule.empty();
	}

	// Idempotent default program creation. Returns true when ready afterwards.
	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kTonemapVsPath,
		const char* psPath = kTonemapPsPath) {
		if (ShadersValid())
			return true;
		try {
			const std::string vs = vsPath ? vsPath : kTonemapVsPath;
			const std::string ps = psPath ? psPath : kTonemapPsPath;
			if (vsModule.empty())
				vsModule = ctx.CreateShaderModule(readFile(vs), "tonemapVert");
			if (fragModule.empty())
				fragModule = ctx.CreateShaderModule(readFile(ps), "tonemapFrag");
		} catch (...) {
			return false;
		}
		if (vsModule.empty() || fragModule.empty())
			return false;
		Shader vs{ .module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader ps{ .module = fragModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		program = { .shaders = { vs, ps }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		program = ShaderProgram();
		vsModule = {};
		fragModule = {};
	}

	// No viewport resources – just stores desc + internalizes EnsureShaders.
	bool Ensure(ResourceManager& ctx, const TonemapDesc& d,
		const char* vsPath = kTonemapVsPath,
		const char* psPath = kTonemapPsPath) {
		desc = d;
		return EnsureShaders(ctx, vsPath, psPath);
	}

	// Emits the HDR -> swapchain pass with recording. Captures only copyable
	// handles/indices, so the pass is safe once this call returns.
	// bloomView may be empty (or bloomBindless UINT32_MAX) to disable bloom.
	// debugView 1 samples motionView/motionBindless instead of compositing
	// the scene (motion vector debug visualization). debugView 2..4 are the
	// CLIPPING views: the scene composites NORMALLY (exposure/bloom/
	// tonemap/LUT all live) and the final mapped color is annotated —
	// 2 = combined (blue shadows + red highlights), 3 = shadows only,
	// 4 = highlights only; the thresholds ride the desc. Modes 2..4 need
	// NO motion texture (only mode 1 validates against it).
	// gammaEncodeOutput: set when the color targets are NOT hardware-sRGB
	// (the SMAA ldr intermediate, or a non-sRGB surface fallback) - the
	// shader then writes sRGB-encoded bytes itself.
	// lutBindlessIndex: bindless slot of a 1024x32 LUT strip (32^3, RGBA8,
	// sRGB view) applied in linear post-tonemap when lutIntensity > 0. The
	// strip is immutable (like the SMAA area/search LUTs), so it needs no
	// input-image barrier declaration.
	// jitterDeltaX/Y: the jitter component of the motion vectors in UV,
	// stripped by the debugView 1 motion visualizer so it shows TRUE
	// motion (zero when no temporal jitter is active).
	// Layer sub-rect (nvpro-style element stacking): offsetX/Y + the
	// swapExtent above confine the write to the element's rect of the swap
	// image. Full-swap layers keep offset 0,0 + Clear (today's behavior);
	// sub-rect layers MUST pass Load so sibling layers outside the rect
	// survive (Clear would wipe the whole attachment).
	// srcUv window (sub-rect SOURCE sampling): the matching sub-rect of the
	// HDR image when the HDR is larger than this pass's rect (a shared
	// full-window HDR with per-rect layers) - texel centers, min/max uv.
	// Default (0,0,1,1) samples the full image (the HDR == the pass rect,
	// today's behavior). Must stay layout-identical to the HLSL mirror
	// (tonemap.frag.hlsl TonemapPush srcUvWindow).
	RenderPass& AddTonemapPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle bloomView,
		uint32_t bloomBindlessIndex,
		float bloomIntensity,
		uint32_t linearSamplerIndex,
		const std::vector<LogicalImageViewHandle>& swapViews,
		Extent2D swapExtent,
		const TonemapDesc& overrideDesc,
		const char* name = nullptr,
		LogicalImageViewHandle motionView = {},
		uint32_t motionBindlessIndex = UINT32_MAX,
		uint32_t debugView = 0,
		float motionDebugScale = 16.0f,
		LogicalImageViewHandle aoView = {},
		uint32_t aoBindlessIndex = UINT32_MAX,
		float aoIntensity = 0.0f,
		bool gammaEncodeOutput = false,
		uint32_t lutBindlessIndex = UINT32_MAX,
		float lutIntensity = 0.0f,
		float jitterDeltaX = 0.0f,
		float jitterDeltaY = 0.0f,
		uint32_t offsetX = 0,
		uint32_t offsetY = 0,
		AttachmentLoadOp colorLoad = AttachmentLoadOp::Clear,
		float srcUvMinX = 0.0f,
		float srcUvMinY = 0.0f,
		float srcUvMaxX = 1.0f,
		float srcUvMaxY = 1.0f) const {
		assert(ShadersValid());
		assert(!hdrView.empty());
		assert(!swapViews.empty());

		const bool bloomOn = !bloomView.empty() && bloomBindlessIndex != UINT32_MAX && bloomIntensity > 0.0f;
		// Mode-specific validation: ONLY the motion view (1) needs a motion
		// texture. The clipping views (2..4) annotate the normal composite
		// and must stay armed without one (the old any-nonzero-mode check
		// silently disabled them whenever motion vectors were absent).
		const bool motionDebugOn = debugView == 1 &&
			!motionView.empty() && motionBindlessIndex != UINT32_MAX;
		const bool aoOn = !aoView.empty() && aoBindlessIndex != UINT32_MAX && aoIntensity > 0.0f;
		TonemapPush push{};
		push.hdrTextureIndex = hdrBindlessIndex;
		push.samplerIndex = linearSamplerIndex;
		push.exposure = overrideDesc.exposure;
		push.tonemapMode = overrideDesc.tonemapMode;
		push.bloomTextureIndex = bloomOn ? bloomBindlessIndex : UINT32_MAX;
		push.bloomIntensity = bloomOn ? bloomIntensity : 0.0f;
		// Mode 1 without a valid motion texture falls back to the scene
		// (the old any-mode safety, narrowed to the mode that needs it);
		// the clipping views (2..4) pass through unconditionally.
		push.debugView = (debugView == 1u && !motionDebugOn) ? 0u : debugView;
		push.motionTextureIndex = motionDebugOn ? motionBindlessIndex : UINT32_MAX;
		push.motionDebugScale = motionDebugScale;
		push.gammaEncode = gammaEncodeOutput ? 1u : 0u;
		push.aoTextureIndex = aoOn ? aoBindlessIndex : UINT32_MAX;
		push.aoIntensity = aoOn ? aoIntensity : 0.0f;
		const bool lutOn = lutBindlessIndex != UINT32_MAX && lutIntensity > 0.0f;
		push.lutTextureIndex = lutOn ? lutBindlessIndex : UINT32_MAX;
		push.lutIntensity = lutOn ? lutIntensity : 0.0f;
		push.jitterDeltaX = jitterDeltaX;
		push.jitterDeltaY = jitterDeltaY;
		push.srcUvMinX = srcUvMinX;
		push.srcUvMinY = srcUvMinY;
		push.srcUvMaxX = srcUvMaxX;
		push.srcUvMaxY = srcUvMaxY;
		push.shadowClipThreshold = overrideDesc.shadowClipThreshold;
		push.highlightClipThreshold = overrideDesc.highlightClipThreshold;

		std::vector<LogicalImageViewHandle> inputs;
		inputs.reserve((bloomOn ? 1 : 0) + (motionDebugOn ? 1 : 0) + (aoOn ? 1 : 0) + 1);
		inputs.push_back(hdrView);
		if (bloomOn)
			inputs.push_back(bloomView);
		if (motionDebugOn)
			inputs.push_back(motionView);
		if (aoOn)
			inputs.push_back(aoView);

		RenderPass& pass = rg.AddPass(name ? name : overrideDesc.name);
		pass.SetOffset(offsetX, offsetY)
			.SetExtent(swapExtent)
			.SetInputImages(inputs)
			.SetColor(swapViews,
				colorLoad,
				AttachmentStoreOp::Store,
				ClearColor(0, 0, 0, 1));
		ShaderProgram progCopy = program; // ShaderProgram is copyable (handles only)
		pass.SetRecordingFunc([progCopy, push](CommandBuffer& cmd) mutable {
			cmd.BindRenderPipeline(
				progCopy,
				VertexLayoutId::Empty,
				DepthSettings::Disabled(),
				StencilSettings::DefaultDisabled(),
				BlendSettings::Opaque(),
				MSAASettings::Count1(),
				RasterSettings{ .cullMode = CullMode::None });
			cmd.PushConstants(0, sizeof(TonemapPush), &push, ShaderStage::All);
			cmd.Draw(3, 1, 0, 0); // fullscreen triangle
		});
		return pass;
	}

	// Uses the stored desc from Ensure().
	RenderPass& AddTonemapPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView,
		uint32_t hdrBindlessIndex,
		LogicalImageViewHandle bloomView,
		uint32_t bloomBindlessIndex,
		float bloomIntensity,
		uint32_t linearSamplerIndex,
		const std::vector<LogicalImageViewHandle>& swapViews,
		Extent2D swapExtent,
		const char* name = nullptr,
		LogicalImageViewHandle motionView = {},
		uint32_t motionBindlessIndex = UINT32_MAX,
		uint32_t debugView = 0,
		float motionDebugScale = 16.0f,
		LogicalImageViewHandle aoView = {},
		uint32_t aoBindlessIndex = UINT32_MAX,
		float aoIntensity = 0.0f,
		bool gammaEncodeOutput = false,
		uint32_t lutBindlessIndex = UINT32_MAX,
		float lutIntensity = 0.0f,
		float jitterDeltaX = 0.0f,
		float jitterDeltaY = 0.0f) const {
		return AddTonemapPass(rg, hdrView, hdrBindlessIndex, bloomView, bloomBindlessIndex,
			bloomIntensity, linearSamplerIndex, swapViews, swapExtent, desc, name,
			motionView, motionBindlessIndex, debugView, motionDebugScale,
			aoView, aoBindlessIndex, aoIntensity, gammaEncodeOutput,
			lutBindlessIndex, lutIntensity, jitterDeltaX, jitterDeltaY);
	}
};

} // namespace smvk
