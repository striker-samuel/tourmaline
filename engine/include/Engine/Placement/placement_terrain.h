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

// Builds one tile's mesh. Local space: [0, tileW] x [0, tileH] (tileW/H =
// size/tiles), y from the global grid. The caller positions the tile node
// at groupOrigin + (tx*tileW, 0, tz*tileH). Returns false on degenerate
// params. UVs are per-tile [0,1] (tile-local planar, standard for
// per-tile material/layer assignment).
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

} // namespace smvk::placement
