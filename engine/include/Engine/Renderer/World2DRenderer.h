// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Independent, sprite-specialized frame renderer. No 3D scene/material
// pipeline dependency. See docs/world2d_renderer.md.
#include <Engine/Renderer/Renderer.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/rendergraph.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace tour {

inline constexpr uint32_t kInvalidSprite2D = UINT32_MAX;

struct SpriteRegion2D {
	uint32_t page = 0;
	glm::vec2 uvMin{0.0f};
	glm::vec2 uvMax{1.0f};
	glm::uvec2 size{0u};
};

// A deterministic shelf-packed, multi-page RGBA8 atlas. Source pixels are
// copied at insertion time; one extruded texel on every side prevents linear
// sampling from bleeding into adjacent sprites. Regions never move.
class SpriteAtlas2D {
public:
	explicit SpriteAtlas2D(uint32_t pageSize = 2048);
	[[nodiscard]] uint32_t Insert(uint32_t width, uint32_t height,
		std::span<const uint8_t> rgba8);
	[[nodiscard]] const SpriteRegion2D* Region(uint32_t id) const;
	[[nodiscard]] size_t PageCount() const { return m_pages.size(); }
	[[nodiscard]] uint32_t PageSize() const { return m_pageSize; }
	[[nodiscard]] uint64_t Revision() const { return m_revision; }
	[[nodiscard]] std::span<const uint8_t> PagePixels(uint32_t page) const;
	void Clear();

private:
	struct Page {
		std::vector<uint8_t> pixels;
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t rowHeight = 0;
	};
	uint32_t m_pageSize;
	uint64_t m_revision = 0;
	std::vector<Page> m_pages;
	std::vector<SpriteRegion2D> m_regions;
};

struct Sprite2D {
	uint32_t region = kInvalidSprite2D;
	glm::vec2 position{0.0f}; // center in world units; Y points downward
	glm::vec2 size{1.0f};
	float z = 0.0f;           // full-precision float; larger = foreground
	float rotation = 0.0f;    // radians, around center
	glm::vec4 color{1.0f};    // linear tint (straight alpha)
};

// 56-byte, scalar-layout-compatible CPU/HLSL instance contract.
struct SpriteInstance2D {
	float position[2];
	float size[2];
	float uvMin[2];
	float uvMax[2];
	float color[4];
	float rotation;
	float z;
};
static_assert(sizeof(SpriteInstance2D) == 56);

struct SpriteRun2D {
	uint32_t page = 0;
	uint32_t firstInstance = 0;
	uint32_t count = 0;
};

struct SpriteBatch2D {
	std::vector<SpriteInstance2D> instances;
	std::vector<SpriteRun2D> runs;
	float minZ = 0.0f;
	float maxZ = 0.0f;
};

// CPU-only preparation. Stable Z sort and consecutive-page batching retain
// correct straight-alpha ordering even when atlas pages are interleaved.
[[nodiscard]] SpriteBatch2D BuildSpriteBatch2D(
	const SpriteAtlas2D& atlas, std::span<const Sprite2D> sprites);

class World2DRenderer final : public engine::IRenderer {
public:
	const char* RendererName() const override { return "World2D"; }

	SpriteAtlas2D& Atlas() { return m_atlas; }
	const SpriteAtlas2D& Atlas() const { return m_atlas; }

	// Call on the render thread before drawing sprites from newly inserted
	// atlas regions. Retires prior textures via RHI holders.
	[[nodiscard]] bool UploadAtlas(smvk::ResourceManager& ctx);
	[[nodiscard]] bool EnsureShaders(smvk::ResourceManager& ctx);

	// Queue API is per frame (not thread-safe). Submit in arbitrary order;
	// increasing float Z draws in front, with stable order for equal Z.
	// kMaxSprites mirrors the HLSL WORLD2D_MAX_SPRITES array bound
	// (world2d.hlsli; DXC requires an explicit struct-member dimension —
	// the draw itself is bounded by the recorded instanceCount). Submit
	// fails loudly past it; 64k on-screen sprites is far past any leg.
	static constexpr size_t kMaxSprites = 65536;
	void BeginFrame() { m_sprites.clear(); }
	void Submit(const Sprite2D& sprite) {
		if (m_sprites.size() >= kMaxSprites) {
			if (!m_overflowLogged) {
				m_overflowLogged = true;
				std::fprintf(stderr, "[World2D] sprite overflow: >%zu "
					"(the shader table capacity); excess dropped\n", kMaxSprites);
			}
			return;
		}
		m_sprites.push_back(sprite);
	}
	void Reserve(size_t count) { m_sprites.reserve(count); }
	void SetCamera(glm::vec2 topLeftWorld, float pixelsPerUnit = 1.0f) {
		m_camera = topLeftWorld;
		m_pixelsPerUnit = pixelsPerUnit;
	}

	// Appends a pass to the host's existing rendergraph. Host owns target,
	// frame synchronization, graph execution and presentation. frameSlot
	// must be the acquired/fenced flight slot (not an unbounded frame id).
	// Only one AppendPass per flight slot is supported between fences.
	// false: missing resources, invalid extent, or allocation failure.
	[[nodiscard]] bool AppendPass(
		smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
		smvk::LogicalImageViewHandle colorTarget, smvk::Extent2D extent,
		uint32_t frameSlot, bool clear = false);

	// Destroy BEFORE the supplied graph/resource manager. No device wait;
	// holders use the RHI's deferred retirement.
	void Destroy(smvk::RenderGraph& rg);

private:
	struct PageGpu {
		smvk::Holder<smvk::TextureHandle> texture;
		uint32_t bindless = UINT32_MAX;
	};
	struct Flight {
		smvk::Holder<smvk::BufferHandle> instances;
		smvk::LogicalBufferHandle logical{};
		size_t capacity = 0;
	};
	struct Push {
		uint64_t instances = 0;
		float camera[2]{};
		float viewport[2]{};
		float pixelsPerUnit = 1.0f;
		float minZ = 0.0f;
		float zRange = 1.0f;
		uint32_t textureIndex = UINT32_MAX;
		uint32_t samplerIndex = UINT32_MAX;
		uint32_t pad = 0;
	};
	static_assert(sizeof(Push) == 48);

	SpriteAtlas2D m_atlas;
	std::vector<Sprite2D> m_sprites;
	bool m_overflowLogged = false;
	std::vector<PageGpu> m_pages;
	uint64_t m_uploadedRevision = UINT64_MAX;
	smvk::Holder<smvk::ShaderModuleHandle> m_vs;
	smvk::Holder<smvk::ShaderModuleHandle> m_fs;
	smvk::ShaderProgram m_program{};
	std::array<Flight, smvk::limits::MaxFramesInFlight> m_flights{};
	glm::vec2 m_camera{0.0f};
	float m_pixelsPerUnit = 1.0f;
	uint32_t m_sampler = UINT32_MAX;
};

} // namespace tour
