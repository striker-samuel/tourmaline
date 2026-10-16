// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Tiled-terrain height field (CPU side): the source of truth for sampling,
// G-block baking, and collision/raycast queries.
//
// Layout: 4x4 H-tiles of 1024 RGBA8 (RG16 height, B/A ignored), covering
// worldSize x worldSize from (originX, originZ). Tile (tx,tz) covers
// [ox + tx*hTileSize, ox + (tx+1)*hTileSize). Inside a tile, pixel (px,py)
// centers sit at ox + (px+0.5)*texel, oz + (py+0.5)*texel with py=0 on the
// -Z edge (matches the generator + terrain_pack.h normal convention).
//
// Header-only over caller-owned pixel buffers (std::vector per tile);
// GPU upload lives in the renderer (T2), never here. Sampling clamps to
// the world edge.

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include <Engine/Rendergraph/terrain_meta.h>
#include <Engine/Rendergraph/terrain_pack.h>

namespace smvk::terrain {

struct TerrainHField {
	TerrainMeta meta{};
	// 16 tiles, row-major (tz * 4 + tx), each hRes*hRes RGBA8.
	std::vector<std::vector<uint8_t>> tiles{};

	[[nodiscard]] uint32_t tilesPerAxis() const noexcept {
		return static_cast<uint32_t>(meta.worldSize / meta.hTileSize + 0.5f);
	}
	[[nodiscard]] bool valid() const noexcept {
		const uint32_t n = tilesPerAxis();
		if (tiles.size() != size_t(n) * size_t(n))
			return false;
		const size_t want = size_t(meta.hRes) * size_t(meta.hRes) * 4;
		for (const auto& t : tiles) {
			if (t.size() != want)
				return false;
		}
		return true;
	}
};

// Pixel RG at integer texel (clamped) of one tile's RGBA8 buffer.
[[nodiscard]] inline float TileH01At(const std::vector<uint8_t>& px,
	uint32_t res, int32_t ix, int32_t iy) noexcept {
	ix = std::clamp(ix, int32_t(0), int32_t(res) - 1);
	iy = std::clamp(iy, int32_t(0), int32_t(res) - 1);
	const size_t o = (size_t(iy) * size_t(res) + size_t(ix)) * 4;
	return DecodeH01(px[o], px[o + 1]);
}

// Normalized height at world XZ: tile select + bilinear over texel centers,
// world-edge clamped. Returns h01; use HeightToWorld(meta-less baseY/range)
// via the overload below for meters.
[[nodiscard]] inline float SampleH01(
	const TerrainHField& f, float x, float z) noexcept {
	const uint32_t n = f.tilesPerAxis();
	const float tile = f.meta.hTileSize;
	const float texel = tile / float(f.meta.hRes);
	// Float tile coords; clamp the sample point into the world first.
	float lx = std::clamp(x - f.meta.originX, 0.0f, f.meta.worldSize - 1e-4f);
	float lz = std::clamp(z - f.meta.originZ, 0.0f, f.meta.worldSize - 1e-4f);
	uint32_t tx = std::min(uint32_t(lx / tile), n - 1);
	uint32_t tz = std::min(uint32_t(lz / tile), n - 1);
	const auto& px = f.tiles[size_t(tz) * size_t(n) + size_t(tx)];
	const uint32_t res = f.meta.hRes;
	// Position within the tile in texel units, relative to texel centers:
	// center of texel i sits at (i+0.5)*texel, so u = local/texel - 0.5.
	const float u = (lx - float(tx) * tile) / texel - 0.5f;
	const float v = (lz - float(tz) * tile) / texel - 0.5f;
	int32_t x0 = static_cast<int32_t>(floorf(u));
	int32_t y0 = static_cast<int32_t>(floorf(v));
	const float fx = u - float(x0);
	const float fz = v - float(y0);
	// NOTE: bilinear stays tile-local (1-texel seam step at H-borders is
	// below the 1m texel and hidden by the shared-edge G-block skirts in
	// T2; cross-tile filtering arrives with the atlas in phase 4).
	const float h00 = TileH01At(px, res, x0, y0);
	const float h10 = TileH01At(px, res, x0 + 1, y0);
	const float h01 = TileH01At(px, res, x0, y0 + 1);
	const float h11 = TileH01At(px, res, x0 + 1, y0 + 1);
	const float hx0 = h00 + (h10 - h00) * fx;
	const float hx1 = h01 + (h11 - h01) * fx;
	return hx0 + (hx1 - hx0) * fz;
}

[[nodiscard]] inline float SampleHeight(
	const TerrainHField& f, float x, float z) noexcept {
	return f.meta.baseY + SampleH01(f, x, z) * f.meta.range;
}

[[nodiscard]] inline glm::vec3 SampleNormal(
	const TerrainHField& f, float x, float z) noexcept {
	const float texel = f.meta.hTileSize / float(f.meta.hRes);
	const float hL = SampleHeight(f, x - texel, z);
	const float hR = SampleHeight(f, x + texel, z);
	const float hD = SampleHeight(f, x, z - texel);
	const float hU = SampleHeight(f, x, z + texel);
	return ComputeNormalH(hL, hR, hD, hU, texel);
}

// Exact min/max world-Y over one tile (for G-block AABBs at bake time).
inline void TileMinMax(const std::vector<uint8_t>& px, uint32_t res,
	float baseY, float range, float& minY, float& maxY) noexcept {
	minY = baseY + range;
	maxY = baseY;
	for (size_t i = 0; i < size_t(res) * size_t(res); ++i) {
		const float h = DecodeH01(px[i * 4], px[i * 4 + 1]);
		const float y = baseY + h * range;
		if (y < minY)
			minY = y;
		if (y > maxY)
			maxY = y;
	}
}

} // namespace smvk::terrain
