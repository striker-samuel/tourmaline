// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// WorldEnvTiles — environment zones as WORLD TILES (the runtime rework of
// the old OBB-volume zone system).
//
// The AUTHORING surface is unchanged (env entities as scene nodes whose
// transforms define OBBs + EnvVolume inf faces — the editor's volume
// panel, the tour/pak keys). The RUNTIME is replaced: instead of packing
// up to 64 GPU zone records every frame and resolving the covering zone
// by PER-PIXEL OBB intersection, the zones are BAKED onto the partition's
// 64 m subcell grid (one world tile per cell) whenever the zone tables
// change (load / edit — a cheap hash, not every frame), and the shaders
// resolve a zone by TILE ADDRESS:
//
//   worldPos -> floor((pos.xz - gridMin) / 64) -> cell ->
//     (dominant record below/above splitY, weights) -> records[rec]
//
// A cell carries TWO records split at a Y plane — the vertical half-space
// zones (underwater y<=0, an interior floor) need a different look above
// vs below within one tile column. lower==upper + splitY = -FLT_MAX is
// the no-split common case. Each record index is into the records array
// where [0] is ALWAYS the base (the default environment block, the old
// zone-zero semantic: outside every zone the base look applies at weight
// 1; out-of-grid positions fall to it too).
//
// FIDELITY CONTRACT vs the retired per-pixel walk: the winner selection
// is the SAME resolution (priority desc -> innermost bounded volume ->
// node id; the first influencing zone wins) and the weight is the same
// blendWeight * blendRadius falloff — evaluated at the tile's SAMPLE
// positions instead of continuously. The trade (accepted, documented):
// a tile is 64 m, so fade bands quantize to tile granularity (a 256 m
// blend radius still fades across 4 cells; a 10 m radius becomes one
// tile's step). Zones coarser than a tile — the design target — are
// faithful; sub-tile detail is not representable by design.
//
// Layout: the records (48 B each, EnvTileRecord) + the cells (32 B each,
// EnvTileCell) ride a plain storage buffer each; the shader reads by
// BDA + offset (the probePremixSel raw-BDA pattern). HLSL mirrors in
// common.hlsli; the FrameUniforms env block carries the BDAs + the grid
// header (see the pinned envZone* fields).

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Scene/scene.h>

namespace engine::worldpartition {

// The env-tile record: one zone's blendable LOOK (the old EnvZoneGpu's
// sky/fog/vol payload — the OBB fields are gone, the tile carries the
// shape). HLSL mirror: EnvTileRecord (common.hlsli), 48 B.
struct EnvTileRecord
{
	glm::vec4 sky{}; // rgb skyTint, a ambientI
	glm::vec4 fog{}; // rgb fogColor, a fogDensity
	glm::vec4 vol{}; // x volumetrics ratio, y volumetricDensity, zw reserved
};
static_assert(sizeof(EnvTileRecord) == 48, "EnvTileRecord: HLSL mirror");

// One 64 m world tile (a subcell column). HLSL mirror: EnvTileCell,
// 32 B. lower/upperWeight = the zone's blendWeight*falloff at the slot's
// sample heights; the base (record 0) tiles carry weight 1.
struct EnvTileCell
{
	uint32_t lowerRecord = 0;
	uint32_t upperRecord = 0;
	float splitY = -3.402823466e+38f; // -FLT_MAX: no vertical split
	float lowerWeight = 0.0f;
	float upperWeight = 0.0f;
	uint32_t reserved[3] = { 0, 0, 0 }; // 32 B total
};
static_assert(sizeof(EnvTileCell) == 32, "EnvTileCell: HLSL mirror");

inline constexpr uint32_t kEnvTileMaxRecords = 64; // [0]=base + 63 zones
inline constexpr uint32_t kEnvTileMaxDim = 256; // 256 tiles * 64 m = 16 km edge
inline constexpr float kEnvTileSize = 64.0f; // == kSubcellSize (one subcell per tile)

// The bake. Rebuildable; the caller drives dirtiness (the renderer hashes
// the zone tables + transforms per frame — cheap — and rebuilds on
// change). CPU queries via At() are the shader mirror's reference (the
// tests pin both sides of the contract).
class WorldEnvTiles
{
public:
	// Grid extent: gridMin = the min corner (XZ, tile-aligned); dimX/dimZ
	// the tile counts (clamped to kEnvTileMaxDim). yMin/yMax frame the
	// vertical split search (the scene bounds). Returns false for invalid
	// extents; a scene with no env entities bakes the pure-base grid.
	bool Build(const Scene& scene, glm::vec2 gridMin,
		uint32_t dimX, uint32_t dimZ, float yMin, float yMax);

	// The shader mirror's reference (tests/editor/tooling).
	struct Lookup
	{
		uint32_t record = 0; // into Records(); 0 = the base
		float weight = 1.0f; // blendWeight*falloff at the slot sample
		bool valid = false; // false = the plain env path (no tiles built)
	};
	Lookup At(glm::vec3 pos) const;

	// ---- pointer + count accessors (the FrameUniforms publication) ----
	const EnvTileRecord* Records() const { return records_.data(); }
	uint32_t RecordCount() const { return recordCount_; }
	const EnvTileCell* Cells() const { return cells_.data(); }
	uint32_t CellCount() const { return uint32_t(cells_.size()); }
	glm::vec2 GridMin() const { return gridMin_; }
	uint32_t DimX() const { return dimX_; }
	uint32_t DimZ() const { return dimZ_; }
	bool Valid() const { return built_; }

private:
	struct Candidate
	{
		smvk::envzones::EnvZoneGpu zone{}; // the sanitized OBB + look
		uint32_t record = 0; // the assigned record index
		double boundedVolume = 0.0; // finite product; +inf for any unbounded face
		uint32_t nodeId = 0;
		float priority = 0.0f;
	};
	// The winner (record + weight) among the sorted candidates at a
	// position; record 0 (the base, an all-infinite candidate) at weight
	// 1 when nothing covers.
	Lookup Evaluate(const glm::vec3& p) const;

	std::vector<EnvTileRecord> records_; // [0] = the base
	uint32_t recordCount_ = 0;
	std::vector<EnvTileCell> cells_; // dimX * dimZ
	std::vector<Candidate> candidates_; // sorted, includes the base at [0]
	glm::vec2 gridMin_{ 0.0f, 0.0f };
	uint32_t dimX_ = 0;
	uint32_t dimZ_ = 0;
	bool built_ = false;
};

} // namespace engine::worldpartition
