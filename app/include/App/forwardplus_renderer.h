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

namespace app {
struct AppFrameContext;

struct ForwardPlusRenderer : public smvk::IFrameRenderer {
	smvk::ForwardPlus* fp = nullptr; // owned by ViewportResources
	smvk::ForwardPlusDesc desc{};
	smvk::ShaderProgram prepassMaskedProgram{}; // main-owned, init-stable

	void Configure(smvk::ForwardPlus* builder, const smvk::ForwardPlusDesc& d,
		const smvk::ShaderProgram& maskedProg) {
		fp = builder;
		desc = d;
		prepassMaskedProgram = maskedProg;
	}

	const char* Name() const override { return "ForwardPlus"; }
	bool Ensure(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) override;
	void Destroy(smvk::RenderGraph& rg) override;
	void AppendPasses(smvk::FrameContext& fc) override;
	// Split for the temporal pipeline (T5): the prepass is skipped when the
	// seed primed depth, and the tile cull runs after depth pass 2 so it
	// sees complete depth. AppendPasses = Prepass + TileCull (legacy order).
	void AppendPrepass(smvk::FrameContext& fc);
	void AppendTileCull(smvk::FrameContext& fc);

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

} // namespace app
