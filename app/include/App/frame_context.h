#pragma once
// App-side frame context: engine FrameContext plus the per-frame addresses
// and handles frame renderers need (filled by main per frame, read by
// IFrameRenderer::AppendPasses implementations).

#include <Engine/Rendergraph/renderer.h>

#include <App/push_constants.h>

#include <glm/glm.hpp>

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
	// Error-driven LOD (see Engine/Asset/mesh_optimizer.h): the prepass
	// runs inside ForwardPlusRenderer, so it reads selection inputs here
	// instead of SceneRenderer members. Distance = node world pos to eye.
	bool lodEnabled = false;
	glm::vec3 lodEye{ 0.0f };
	float lodViewportHeight = 1080.0f;
	float lodFovY = 0.7853982f; // 45 deg
	float lodPixelBudget = 1.0f;
};

} // namespace app
