// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// SgGiVolume: the baked SG-GI probe volume — data model, serialization
// (.sggi) and runtime lookup structures.
//
// Layout model (see the bake pipeline in GI/bake.h):
//  - The world is covered by an adaptive octree of bricks. Each leaf brick
//    owns ONE probe, placed at the (relocated) brick center, with a support
//    radius covering the brick. Dense areas (near surfaces, corners,
//    doorways, high-error regions) end up with small bricks/probes; open
//    empty space keeps a handful of coarse probes.
//  - Probes are grouped by octree LEVEL. Each level gets a uniform lookup
//    grid with cellSize = brick size. A probe sphere (R =
//    probeSupportFactor x brick; default 1.0) overlaps up to 2-3 cells per
//    axis, and a cell's probe list is a superset of everything that can
//    shade a point inside it. List length is scale-free (~27 at unit
//    support): fat supports stuff the 64/cell cap and silently drop
//    probes. Runtime sampling walks levels finest-first with an early-out
//    (fine probes supersede coarse) and blends with a partition-of-unity
//    weight.
//  - Wall awareness (baked, cheap): each probe stores 6 FP16 obstacle
//    distances (+x,-x,+y,-y,+z,-z). A shading point beyond a probe's
//    recorded obstacle is behind a wall from that probe's point of view
//    and the probe is downweighted to zero — classic baked-GI leak fix,
//    no runtime rays needed.
//
// Storage: 96 B per probe on the GPU (SgProbeGpu, half-float amplitudes).
// The .sggi file stores the same data LZ4-compressed (~64-80 B/probe) plus,
// since v2, the DENSE runtime layers (see DenseLevel): per octree level the
// visibility-weighted blend baked at every lookup-cell center into nine
// BC6H block payloads + one R16F weight volume — the runtime's ONLY probe
// payload (the shader trilinear-filters these; the per-pixel probe-list
// walk is gone).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/GI/sg.h>

namespace smvk::gi {

inline constexpr uint32_t kSgGiMaxLevels = 8;
inline constexpr uint32_t kSgGiMaxProbesPerCell = 64;

// ---------------------------------------------------------------------------
// CPU-side probe
// ---------------------------------------------------------------------------

struct SgProbe {
	glm::vec3 position{};
	float radius = 1.0f;            // support radius (Wendland falloff)
	glm::vec3 amps[kSgLobeCount]{}; // RGB amplitudes of the fixed basis
	float visibility[6] = {};       // +x,-x,+y,-y,+z,-z obstacle distances;
	                                // 0 = no known obstacle (unobstructed).
	                                // Never the trace cutoff: storing the
	                                // cutoff reads as a phantom wall.
	uint32_t level = 0;             // octree level (drives the lookup grid)
	bool valid = true;
};

// ---------------------------------------------------------------------------
// GPU grid descriptor (mirrors SgGiGrid / SgGiLevel in sggi.hlsli)
// ---------------------------------------------------------------------------

struct alignas(16) SgGiLevelGpu {
	glm::vec4 cellSize = {};        // x = cell size (m); yzw pad
	glm::vec4 volumeMin = {};       // level-local grid origin
	uint32_t dims[3] = {};          // cell counts per axis
	uint32_t pad0 = 0;
	uint32_t cellsOffset = 0;       // element offset into the uint2 cell buffer
	uint32_t indicesOffset = 0;     // element offset into the u32 index buffer
	uint32_t probeOffset = 0;       // first probe of the level (sorted array)
	uint32_t pad1 = 0;
};
static_assert(sizeof(SgGiLevelGpu) == 64);

struct alignas(16) SgGiGridGpu {
	glm::vec4 volumeMin_probeCount = {}; // xyz = global bounds min, w = probe count
	uint32_t levelCount = 0;
	uint32_t maxProbesPerCell = 0;
	uint32_t pad[2] = {};
	SgGiLevelGpu levels[kSgGiMaxLevels] = {};
};
static_assert(sizeof(SgGiGridGpu) == 544);

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

class SgGiVolume {
public:
	std::vector<SgProbe> probes; // sorted by level
	glm::vec3 boundsMin{ 0.0f };
	glm::vec3 boundsMax{ 0.0f };
	uint32_t levelCount = 0;
	float cellSize[kSgGiMaxLevels] = {}; // per lookup-level cell size (m)

	// Rebuilds the per-level lookup grids from `probes` (sorted by level).
	// Called after a bake and after a file load.
	void BuildGrids();

	// CPU-side sampling (unit tests + validation): blends probe amplitudes
	// at a world position with the same weights the GPU uses. Returns false
	// when nothing covers the point.
	bool Sample(const glm::vec3& pos, glm::vec3 outAmps[kSgLobeCount]) const;

	// Dense trilinear resample (the BC6H Texture3D runtime path): for one
	// level, evaluates the visibility-weighted partition-of-unity blend
	// at every grid cell center (the SAME per-cell math Sample applies —
	// ProbeWeight x VisibilityFactor over the cell's probe list) into
	// dense per-voxel (amps, weight). This is the genuinely nonlinear
	// part hardware trilinear cannot express from the raw lattice
	// (irregular per-cell probe selection + wall rejection + partition
	// weights); the runtime only interpolates the baked result. Returns
	// false for an empty/invalid level (the caller keeps it absent).
	bool ResampleLevelDense(uint32_t level,
		std::vector<glm::vec3>& outAmps, // dims^3 * kSgLobeCount (raw sums)
		std::vector<float>& outWeight,  // dims^3 (the partition weight sum)
		glm::vec3& outMin, uint32_t (&outDims)[3], float& outCell) const;

	// Dense runtime layers (baked by BakeDenseLayers, shipped in the
	// .sggi): per level nine BC6H payloads (SG-lobe RGB RAW SUMS — the
	// runtime divides by the weight volume's lerped sum, preserving
	// normalized weighted blending near validity boundaries) + one R16F
	// weight volume. The VkImage extent is EXACTLY dims on every axis —
	// no padding anywhere: BC6H blocks are 4x4x1, and Vulkan allows
	// partial edge blocks, so only the ENCODER sees padded staging
	// slices (the stored payloads hold ceil(w/4) x ceil(h/4) x d blocks).
	struct DenseLevel {
		bool present = false;          // false = sparse/failed level (absent)
		uint32_t dims[3] = { 1, 1, 1 }; // voxel grid per axis (the image extent)
		std::vector<uint8_t> lobeBlocks[kSgLobeCount]; // BC6H blocks per lobe
		std::vector<uint16_t> weight;                 // R16F halves (w*h*d)
		size_t LobeStride() const {
			return size_t((dims[0] + 3) / 4) * ((dims[1] + 3) / 4)
				* dims[2] * 16;
		}
		size_t Bytes() const {
			size_t b = size_t(dims[0]) * dims[1] * dims[2] * 2;
			for (uint32_t l = 0; l < kSgLobeCount; ++l)
				b += lobeBlocks[l].size();
			return b;
		}
	};
	std::vector<DenseLevel> denseLevels; // index = octree level

	// Bakes denseLevels from the built grids (call after a bake or a
	// load; GiBaker does this before WriteFile so the .sggi ships
	// finished payloads and the runtime never encodes). This is the
	// COOK step of the dense path — the bake cost (resample + BC6H
	// encode) belongs here, not at load. Returns true when at least one
	// level baked; sparse levels stay absent (present = false).
	bool BakeDenseLayers();

	// Packs the GPU payload: probe array, grid header, per-cell ranges and
	// probe index list. All vectors are cleared and filled.
	void PackGpu(std::vector<SgProbeGpu>& outProbes, SgGiGridGpu& outGrid,
		std::vector<uint32_t>& outCells, std::vector<uint32_t>& outIndices) const;

	// ---------------------------------------------------------------------------
	// .sggi serialization (LZ4-compressed payload)
	// ---------------------------------------------------------------------------

	bool WriteFile(const std::string& path, std::string& err) const;
	bool ReadFromMemory(std::span<const uint8_t> bytes, std::string& err);
	static bool ReadFile(const std::string& path, SgGiVolume& out, std::string& err);

	// Approximate GPU memory footprint (probes + cells + indices).
	size_t GpuBytes() const;

	struct LevelStats {
		uint32_t probeCount = 0;
		uint32_t cellCount = 0;
		uint32_t maxProbesPerCell = 0;
		uint32_t indexCount = 0;
	};
	std::vector<LevelStats> levelStats; // filled by BuildGrids

private:
	struct LevelGrid {
		std::vector<uint32_t> cellStart;   // dims^3 + 1 prefix sums
		std::vector<uint32_t> indices;     // probe indices (level-global)
		glm::vec3 volumeMin{};
		uint32_t dims[3] = { 1, 1, 1 };
	};
	std::vector<LevelGrid> levelGrids_; // rebuilt by BuildGrids
};

} // namespace smvk::gi
