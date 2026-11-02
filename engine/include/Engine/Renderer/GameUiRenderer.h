// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Separate from WorldRenderer and World2DRenderer: pixel-space game HUD/UI
// rendering, a single bindless instanced draw, nested CPU-only layout.
#include <Engine/Renderer/Renderer.h>
#include <Engine/Renderer/GameUiLayout.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/rendergraph.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tour {

struct UiDrawRect2D {
	UiRect2D bounds{};
	glm::vec4 tint{1.0f}; // straight alpha; solid fill when texture is empty
	smvk::TextureHandle texture{}; // host-owned sampled 2D texture
	glm::vec2 uvMin{0.0f};
	glm::vec2 uvMax{1.0f};
	std::optional<UiRect2D> clip{}; // null: viewport; e.g. layoutItem.clip
};

// GPU layout must mirror engine/shaders/gameui.hlsli GameUiRectInstance.
// Packed to 72 bytes, one texture index PER instance -> one draw for
// mixed textured/solid rectangles without changing atlas descriptors.
struct GameUiRectInstance {
	float origin[2]{};
	float size[2]{};
	float uvMin[2]{};
	float uvMax[2]{};
	float tint[4]{};
	float clipMin[2]{};
	float clipMax[2]{};
	uint32_t textureIndex = UINT32_MAX; // no texture => solid fill
	uint32_t pad = 0;
};
static_assert(sizeof(GameUiRectInstance) == 72);

struct GameUiPreparedRect {
	GameUiRectInstance instance{};
	smvk::TextureHandle texture{};
};

// Testable without a GPU: filters invalid/invisible rectangles, retains
// submission order, leaves texture-index resolution for AppendPass.
[[nodiscard]] std::vector<GameUiPreparedRect> PrepareGameUiRects(
	std::span<const UiDrawRect2D> rects, smvk::Extent2D viewport);

class GameUiRenderer final : public engine::IRenderer {
public:
	const char* RendererName() const override { return "GameUI"; }

	// Draws are appended in painter's order (last is on top). Caller
	// rebuilds every frame, or may reuse its own retained widget tree.
	void BeginFrame() { m_rects.clear(); }
	void Reserve(size_t count) { m_rects.reserve(count); }
	void DrawRect(const UiDrawRect2D& rect) { m_rects.push_back(rect); }

	// Called on the render thread. Shaders are packed automatically by
	// the existing engine shaders build. Textures are owned by the host.
	[[nodiscard]] bool EnsureShaders(smvk::ResourceManager& ctx);

	// Appends AFTER the scene/world pass to render on top. The host owns
	// rendergraph execution and texture upload/synchronization. The
	// frameSlot must be acquired/fenced; only one append per slot/frame.
	[[nodiscard]] bool AppendPass(
		smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
		smvk::LogicalImageViewHandle colorTarget,
		smvk::Extent2D viewport, uint32_t frameSlot,
		bool clear = false);

	// Call before destroying the rendergraph and ResourceManager.
	void Destroy(smvk::RenderGraph& rg);

private:
	struct Flight {
		smvk::Holder<smvk::BufferHandle> instances;
		smvk::LogicalBufferHandle logical{};
		size_t capacity = 0;
	};
	struct Push {
		uint64_t instances = 0;
		float viewport[2]{};
		uint32_t samplerIndex = UINT32_MAX;
		uint32_t pad = 0;
	};
	static_assert(sizeof(Push) == 24);

	std::vector<UiDrawRect2D> m_rects;
	std::array<Flight, smvk::limits::MaxFramesInFlight> m_flights{};
	smvk::Holder<smvk::ShaderModuleHandle> m_vs;
	smvk::Holder<smvk::ShaderModuleHandle> m_fs;
	smvk::ShaderProgram m_program{};
	uint32_t m_sampler = UINT32_MAX;
};

} // namespace tour
