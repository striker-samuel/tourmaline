#pragma once
// App-side frame context: engine FrameContext plus the per-frame addresses
// and handles frame renderers need (filled by main per frame, read by
// IFrameRenderer::AppendPasses implementations).

#include <Engine/App/world.h>
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
	// Option A occluder depth: HZB source (big solid occluders, crudest
	// LOD). Empty handles = legacy path (HZB over depthView above).
	smvk::LogicalImageViewHandle hizDepthView{};
	smvk::ImageViewHandle hizDepthViewHandle{};
	uint32_t hizDepthBindless = UINT32_MAX;
	smvk::LogicalBufferHandle fpLights{};     // per-frame light buffer
	uint32_t fpLightCount = 0;
	// Decal ring (per-frame ForwardPlusDecal upload; hints ride the tile
	// cull + Main passes like fpLights).
	smvk::LogicalBufferHandle decals{};
	uint32_t decalCount = 0;
	// T3: the GfxRenderer seeded depth this frame (prepass must Load the
	// seed instead of Clearing). Set by main after gfx AppendPasses.
	bool seedDepthPrimed = false;
	// Error-driven LOD (see Engine/Asset/mesh_optimizer.h): selection
	// inputs shared by every consumer this frame (prepass, main, gfx
	// filter). World::LodIndexForNode keys its halfrate parity cache on
	// lodInputs.frameId; all passes therefore observe identical per-node
	// decisions within a frame.
	bool lodEnabled = false;
	smvk::LodFrameInputs lodInputs{};
	// Shader LODs: the Main pass draws instances whose selected level is
	// World::LodShader::VertexLit with the cheap vertex-lit program
	// (direct leg); the gfx filter excludes them from the indirect
	// ranges accordingly (see GfxRenderer::FilterMainView). Mirrors
	// SceneRenderer::ShaderLodActive() — both sides must agree per frame.
	bool lodShaderEnabled = false;
	// Dithered transparency (default) vs classic alpha blend for
	// AlphaMode::Blend materials. Mirrors SceneRenderer::m_transparentMode.
	// The depth prepass intentionally ignores it: transparent is always
	// excluded from depth so SSAO/HZB/tile-cull see opaque only.
	// (Main + MotionVectors branch on the mode directly today; kept here
	// for frame renderers that need it without reaching into SceneRenderer.)
	bool ditheredTransparency = true;
};

} // namespace app
