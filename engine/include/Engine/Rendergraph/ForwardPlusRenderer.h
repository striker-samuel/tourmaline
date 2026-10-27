// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Forward+ frame renderer: depth prepass (fills depth for tile min/max)
// + tile light cull, moved out of main.cpp behind IFrameRenderer.
//
// Depth is opaque-only (solid + MASK cutout). AlphaMode::Blend transparent
// never writes depth — even dithered screen-door — so SSAO/HZB/tile-cull
// observe the background behind glass instead of the glass surface.
// Tradeoff: no prepass ordering/HZB/cull benefits for transparent (it
// tests against opaque depth, writes none), matching classic forward.
//
// References the existing smvk::ForwardPlus builder owned by the viewport
// resources (tile buffers, programs stay where the main pass expects them)
// and adds sizing config. Per-frame inputs (depth target, light buffer,
// addresses) arrive via AppFrameContext. The main opaque pass still samples
// the tile lists itself (see ForwardPlus::AddTileReadHints).

#include <Engine/Rendergraph/renderer.h>
#include <Engine/Rendergraph/forwardplus.h>

namespace engine {
struct AppFrameContext;

// Direct-path skin tail (default.vert.hlsl's 208..240 overlay scratch):
// the prepass/occluder legs push it per draw so the direct VS can fetch
// the frame's deformed words instead of the bind-pose vertex inputs.
// The layout mirrors DefaultVsPush's skin tail byte-for-byte.
struct alignas(8) DepthSkinTail {
	uint32_t skinned = 0;           // 208: 1 = fetch from the deformed heap
	uint32_t deformedBase = 0xFFFFFFFFu; // 212: the skinned span's heap-id base
	uint64_t vsDeformed = 0;         // 216: the frame's deformed heap BDA
	uint32_t pad[4] = {};            // 224..240: layout filler
};
static_assert(sizeof(DepthSkinTail) == 32,
	"DepthSkinTail must be 32 bytes (default.vert.hlsl's 208..240 overlay)");
inline constexpr uint32_t kDepthSkinTailOffset = 208;

struct ForwardPlusRenderer : public smvk::IFrameRenderer {
	smvk::ForwardPlus* fp = nullptr; // owned by ViewportResources
	smvk::ForwardPlusDesc desc{};
	smvk::ShaderProgram prepassMaskedProgram{}; // main-owned, init-stable
	// SSSR-style thin G-buffer writers (see gbuffer.frag.hlsl): bound by
	// the prepass phases instead of the VS-only depthProgram +
	// prepassMaskedProgram when fc.ssrGbufferWanted (the G-buffer FS is
	// trivial — no textures, no lighting; the prepass already rasterizes
	// every opaque surface). Empty programs = legacy VS-only path.
	smvk::ShaderProgram gbufferProgram{};
	smvk::ShaderProgram gbufferMaskedProgram{};
	// Option A occluder prepass tuning (see AppendOccluderPrepass): items
	// whose world bound-radius < occluderMinRadius OR whose projected
	// bound diameter < occluderMinPixels are skipped (poor occluders).
	float occluderMinPixels = 24.0f;
	float occluderMinRadius = 0.5f;

	void Configure(smvk::ForwardPlus* builder, const smvk::ForwardPlusDesc& d,
		const smvk::ShaderProgram& maskedProg) {
		fp = builder;
		desc = d;
		prepassMaskedProgram = maskedProg;
	}
	void ConfigureGbuffer(const smvk::ShaderProgram& gbufferProg,
		const smvk::ShaderProgram& gbufferMaskedProg) {
		gbufferProgram = gbufferProg;
		gbufferMaskedProgram = gbufferMaskedProg;
	}

	const char* Name() const override { return "ForwardPlus"; }
	bool Ensure(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) override;
	void Destroy(smvk::RenderGraph& rg) override;
	void AppendPasses(smvk::FrameContext& fc) override;
	// Split for the temporal pipeline (T5): the prepass is skipped when the
	// seed primed depth, and the tile cull runs after depth pass 2 so it
	// sees complete depth. AppendPasses = Prepass + TileCull (legacy order).
	// fallbackOnly (Northlight depth split): skip items whose selected LOD
	// the gfx filter owns (fc.gfxLodFilterable) — the seed pass covers
	// their prev∩curr clusters and the remainder pass the newly visible
	// curr−prev ones (each cluster depthed exactly once, masked with the
	// real alpha test). The remaining direct-fallback items (unregistered
	// meshes/LODs, unknown-material clusters, shader-LOD vertex-lit
	// instances) keep their depth coverage, preserving the 1x Main
	// read-only contract.
	void AppendPrepass(smvk::FrameContext& fc, bool fallbackOnly = false);
	// lightCountOverride: UINT32_MAX (default) = cull fc.fpLightCount
	// lights; any other value limits the light leg (the deferred-shadow
	// pipeline passes 0 — its shading consumes the DS lists, but the decal
	// leg of the cull still refreshes the decal tile lists).
	// fogMaxDistance: the volumetric march range the fog lists are culled
	// against (the cull builds the per-tile FOG lists unconditionally —
	// march-range candidacy, see ForwardPlusCullPush::fogTile* — so the
	// app just feeds the pass's maxDistance every frame).
	void AppendTileCull(smvk::FrameContext& fc,
		uint32_t lightCountOverride = UINT32_MAX,
		float fogMaxDistance = 80.0f);
	// Option A occluder prepass: fills fc.hizDepthView (Clear, VS-only,
	// solid opaque only) with big occluders at their SELECTED LOD for the
	// HZB build — the occluder rasterizes the same surface Main draws, so
	// occluder coverage can never be nearer than the rendered geometry.
	// MASK cutout is skipped (holes = poor occluders + FS cost); small
	// items are skipped via the occluderMin* thresholds.
	// Missing occluders only cost culling efficiency (false visibles),
	// never correctness: Main/SSAO/tile-cull read the full depth.
	void AppendOccluderPrepass(smvk::FrameContext& fc);

	// Resize-safe sizing outside pass building (frame-top, so tile counts
	// are fresh for consumers that read them before AppendPasses).
	// Idempotent; AppendPasses re-checks anyway.
	bool EnsureSized(smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
		smvk::Extent2D extent, smvk::ImageViewHandle depthView) {
		if (!fp)
			return false;
		return fp->Ensure(ctx, rg, extent, desc, depthView);
	}
};

} // namespace engine
