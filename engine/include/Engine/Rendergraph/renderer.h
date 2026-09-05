#pragma once
// Whole-frame renderer abstraction.
//
// A frame renderer owns its persistent resources (buffers, programs —
// configured once, e.g. ForwardPlusDesc / GfxConfig) and appends its passes
// to the rendergraph every frame from a FrameContext. The app builds one
// context per frame and drives a list of renderers, instead of hand-rolling
// per-pass setup in one ever-growing function:
//
//   AppFrameContext fc{...}; // engine FrameContext + app addresses
//   for (IFrameRenderer* r : frameRenderers) r->AppendPasses(fc);
//
// Migration is incremental: passes that still need app-local push structs
// stay in main until their push data moves into the context.

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/App/world.h>
#include <Engine/Renderer/renderview.h>

#include <glm/glm.hpp>

namespace smvk {

// Whole-frame input. Persistent resources live on the renderer object;
// everything here is per-frame. App extends this (see App/frame_context.h)
// with pass-specific addresses/handles; renderers static_cast to what they
// need.
struct FrameContext {
	ResourceManager* ctx = nullptr;
	RenderGraph* rg = nullptr;
	World* world = nullptr;
	const RenderViewSet* views = nullptr;
	uint32_t frameIndex = 0;
	uint32_t mainViewIndex = RenderViewSet::kInvalidIndex;
	glm::vec3 eye{ 0.0f }; // camera world position
	Extent2D extent{ 0, 0 }; // internal render extent (px)
};

struct IFrameRenderer {
	virtual ~IFrameRenderer() = default;
	virtual const char* Name() const = 0;
	// Persistent resources (buffers, programs). Called once; must be
	// idempotent. Per-frame resizing happens in AppendPasses.
	virtual bool Ensure(ResourceManager& ctx, RenderGraph& rg) = 0;
	virtual void Destroy(RenderGraph& rg) = 0;
	// CPU work + pass shells for this frame. Recording lambdas capture
	// frame data by value (frameIndex-ringed buffers make this safe).
	virtual void AppendPasses(FrameContext& fc) = 0;
};

} // namespace smvk
