// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Procedural terrain: analytic heightfield shared by the placement Terrain
// domain, the editor Create > Terrain flow, and the cooker (all three
// consume identical samples — preview, viewport mesh and baked mesh
// agree by construction).
//
// Convention: heights are sampled at ABSOLUTE world XZ (origin + local),
// while mesh vertices are emitted LOCAL ([0,size] on x/z). The scene node
// transform carries the origin (translate(ox, 0, oz)), so world space
// matches the placement domain exactly.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_domain.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Rendergraph/terrain_hfield.h>

namespace smvk::placement {

struct TerrainParams {
	glm::vec2 origin{ 0.0f, 0.0f }; // world XZ of texel (0,0)
	float size = 64.0f;             // square extent in meters
	uint32_t res = 64;              // samples per side (clamped 2..512)
	float height = 8.0f;            // max hill height in meters
	float freq = 0.02f;             // noise frequency (1/m)
	uint64_t seed = 1;              // noise seed
};

// Builds the heightfield described by params. Deterministic: same params
// -> identical samples (two value-noise octaves, output in [0, height]).
HeightfieldData BuildTerrainHeights(const TerrainParams& params);

// Builds a renderable grid mesh from the same field: local-space verts
// ([0,size] x/z, y up), smooth normals, planar 0..1 UVs, one indexed
// surface, valid AABB. Returns false when res is degenerate.
bool BuildTerrainUpload(const TerrainParams& params, StaticMeshUpload& out);

// ---------------------------------------------------------------------------
// Terrain groups: tile split + height painting.
//
// A group is ONE logical terrain (params + a GLOBAL height grid of res x
// res samples) rendered as tilesX x tilesZ child meshes. Tiles sample
// inclusive sub-rects of the global grid, so edges between tiles share
// samples and heights stay continuous across borders. The group node's
// transform carries the world origin; each tile node adds its local
// offset. Painting edits the GLOBAL grid, then affected tile meshes are
// rebuilt — every consumer (viewport mesh, baked mesh, placement
// sampling) sees identical heights by construction.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kTerrainMaxTiles = 16; // per axis

struct TerrainGroupData {
	TerrainParams params;      // res/size describe the GLOBAL grid
	uint32_t tilesX = 1;
	uint32_t tilesZ = 1;
	HeightfieldData heights;    // global grid (res x res), world-relative
};

// Resolves the inclusive sample range of tile (tx,tz) in the global grid:
// [x0..x1] x [z0..z1] with shared edge samples between neighbours.
void TerrainTileRange(const TerrainGroupData& g, uint32_t tx, uint32_t tz,
	uint32_t& x0, uint32_t& x1, uint32_t& z0, uint32_t& z1);

// The tile's group-local XZ position: its ACTUAL first sample,
// (x0 * spacing, z0 * spacing). The tile node transform MUST be
// groupOrigin + this offset — NOT the even metric split
// tx * (size / tilesX): the integer sample split and the even metric
// split only agree when tilesX divides the interval count exactly.
// (64 samples over 64 m, 4 tiles: tile 0 ends at sample 15 = 15.238 m
// while the even split puts tile 1 at 16 m — a 0.76 m GAP. Positioning
// by first sample makes tile tx's last column coincide EXACTLY with
// tile tx+1's first column: x1 * spacing == (tx+1)'s x0 * spacing.)
glm::vec2 TerrainTileOrigin(const TerrainGroupData& g, uint32_t tx, uint32_t tz);

// Builds one tile's mesh. Local space: [0, (x1-x0)*spacing] x
// [0, (z1-z0)*spacing] (the tile's sample span), y from the global grid.
// The caller positions the tile node at groupOrigin + TerrainTileOrigin
// (see above — the seam-correct offset). Returns false on degenerate
// params: res < 2, an out-of-range tile index, or a tile split finer
// than one sample interval per axis (tilesX > res-1 / tilesZ > res-1 —
// a one-sample tile would divide by w-1 == 0 for its UVs). UVs are
// per-tile [0,1] (tile-local planar, standard for per-tile
// material/layer assignment).
bool BuildTerrainTileUpload(const TerrainGroupData& g, uint32_t tx, uint32_t tz,
	StaticMeshUpload& out);

// ---- Height painting (deterministic, no RNG) -----------------------------
enum class TerrainBrushOp : uint8_t {
	Raise = 0,
	Lower = 1,
	Flatten = 2, // pull towards `target` height
	Smooth = 3,  // 3x3 box blur
};

// Applies one brush stamp at WORLD XZ to the global grid. Falloff is a
// smoothstep from the stamp center; strength is the per-stamp amount in
// meters (Raise/Lower) or the blend factor 0..1 (Flatten/Smooth).
void TerrainBrushStamp(TerrainGroupData& g, TerrainBrushOp op,
	float wx, float wz, float radius, float strength, float target);

// Inclusive sample-box of a stamp (for before-patch capture): clamped to
// the grid, rounded outward so undo restores every touched sample.
void TerrainBrushBox(const TerrainGroupData& g, float wx, float wz,
	float radius, uint32_t& x0, uint32_t& x1, uint32_t& z0, uint32_t& z1);

// Which tiles a stamp's box touches (may be empty on a miss). Tiles are
// packed tx | (tz << 16), ascending, deterministic.
void TerrainBrushTouchedTiles(uint32_t x0, uint32_t x1, uint32_t z0,
	uint32_t z1, uint32_t res, uint32_t tilesX, uint32_t tilesZ,
	std::vector<uint32_t>& outTiles);

// Raycast against the heightfield ( marched fixed steps + bisection
// refine). o + t*d = hit point; returns false on no hit within maxDist.
bool TerrainRaycast(const HeightfieldData& hf,
	const glm::vec3& o, const glm::vec3& d, float maxDist, float& t);

// Undo helper: copy a sample box out / write one back (row-major, x-major
// within rows: (z - z0) * boxW + (x - x0)).
void TerrainCopyBox(const TerrainGroupData& g,
	uint32_t x0, uint32_t x1, uint32_t z0, uint32_t z1,
	std::vector<float>& outBox, uint32_t& boxW, uint32_t& boxH);
void TerrainWriteBox(TerrainGroupData& g,
	uint32_t x0, uint32_t x1, uint32_t z0, uint32_t z1,
	const std::vector<float>& box, uint32_t boxW);

// ---------------------------------------------------------------------------
// WORLD-TILE BRIDGE (the ecotope + terrain "ride" — the PR05 foundation).
//
// THE ANSWER to "can ecotope ride the world tiles' heightfield": YES —
// both ride the SAME world texel grid. The tiled world carries:
//   * H-tiles — RG16 heights (RGBA8, R=MSB/G=LSB, h01 -> meters via
//     baseY/range), texel centers at (px+0.5)*texel (TerrainHField's
//     contract);
//   * S-tiles — RGBA8 MATERIAL MASKS (the paint layers: 4 channels per
//     texel), 1m/texel, the same world texel pitch as the heights.
// The ecotope FieldOp's EcotopeMaskData maps straight onto the S-tile
// channels (channel index = paint layer), and the heightfield domain
// reuses the decoded heights — THE CONVERSION happens HERE, once per
// harvested window, not per sample.
//
// CONVENTION NORMALIZATION (the single documented bridge): the world
// samples TEXEL CENTERS; the placement contracts (HeightfieldData +
// EcotopeMaskData) use grid-k (origin + k*spacing, grid k == texel k).
// Each harvested field's origin is offset by +half a texel, so the
// placement field's texel i IS the world texel (windowBase+i)'s center
// — the placement bilinear then interpolates exactly the world's texel
// pairs, and both CPU and GPU sample ONE contract afterwards. Windows
// clamp at the world edge (the world sampler's own clamp).
//
// The height window adds a `halo` texel border on every side so the
// placement normal's central differences never read outside the window.
// ---------------------------------------------------------------------------

// The world S-tile material-mask grid (the ecotope paint source): a
// tilesPerAxis x tilesPerAxis grid of tileRes x tileRes RGBA8 tiles
// (production: 16x16 x 256 — the world's harvested `masks`).
struct WorldMaskGrid {
	const std::vector<std::vector<uint8_t>>* tiles = nullptr; // [tz*nS+tx]
	uint32_t tilesPerAxis = 0;
	uint32_t tileRes = 0;
	[[nodiscard]] bool Valid() const {
		return tiles && tilesPerAxis > 0 && tileRes > 0
			&& tiles->size() >= size_t(tilesPerAxis) * size_t(tilesPerAxis);
	}
};

// The cross-boundary halo (meters) every world tile's harvest AND
// invalidation must carry: the min-distance winners read ±minSpacing
// across borders, the central-difference normals read ±1 heightfield
// texel. Pass the SAME value to PlacementSystem::InvalidateRegion.
inline float WorldTileHaloMeters(float minSpacing, float heightTexel) {
	return std::max(minSpacing > 0.0f ? minSpacing : 0.0f, 2.0f * heightTexel);
}

// The per-tile harvested world data (the residency entry owns it; the
// layer binding REPOINTS at it per tile — heights + domain + eco).
struct WorldTileData {
	HeightfieldData heights;   // the tile window (+ halo), meters
	HeightfieldDomain domain; // extent = the HALF-OPEN tile rect
	EcotopeMaskData eco;      // the tile's paint window (+ the same halo)
};

// Builds one world placement tile's data: the heightfield window
// (+ halo texels), the domain extent (the tile's half-open rect), and
// the ecotope paint window. The tile rect is
// [tile*tileSize, (tile+1)*tileSize) on world XZ — the canonical
// candidate-ownership rule. Returns false when the tile lies outside
// the world or the sources are invalid.
bool BuildWorldTileDomain(const smvk::terrain::TerrainHField& field,
	const WorldMaskGrid& masks, float tileSize,
	int32_t tileX, int32_t tileZ, float haloMeters,
	WorldTileData& out);

// Harvests the [rectMin, rectMax) world-XZ window (+ halo texels) of the
// tiled heights into a placement HeightfieldData (meters). Returns false
// on an invalid field/rect.
bool HarvestWorldHeightfield(const smvk::terrain::TerrainHField& field,
	const glm::vec2& rectMin, const glm::vec2& rectMax, uint32_t halo,
	HeightfieldData& out);

// Harvests the same window (no halo — the mask is a direct read) of the
// S-tile paint channels into a placement EcotopeMaskData (4 channels,
// 0..255 -> 0..1). Returns false on an invalid grid/rect.
bool HarvestWorldEcotope(const smvk::terrain::TerrainMeta& meta,
	const WorldMaskGrid& masks,
	const glm::vec2& rectMin, const glm::vec2& rectMax,
	EcotopeMaskData& out);

} // namespace smvk::placement
