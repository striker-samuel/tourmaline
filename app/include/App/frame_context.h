#pragma once
// App-side frame context: engine FrameContext plus the per-frame addresses
// and handles frame renderers need (filled by main per frame, read by
// IFrameRenderer::AppendPasses implementations).

#include <Engine/Rendergraph/renderer.h>

#include <App/push_constants.h>

namespace app {

struct AppFrameContext : public smvk::FrameContext {
	// Full scene push (value-copied into recording lambdas).
	PushConstants pc{};
	// Forward+:
	smvk::LogicalImageViewHandle depthView{}; // 1x prepass depth target
	smvk::ImageViewHandle depthViewHandle{};  // physical view (bindless)
	smvk::LogicalBufferHandle fpLights{};     // per-frame light buffer
	uint32_t fpLightCount = 0;
	// T3: the GfxRenderer seeded depth this frame (prepass must Load the
	// seed instead of Clearing). Set by main after gfx AppendPasses.
	bool seedDepthPrimed = false;
};

} // namespace app
