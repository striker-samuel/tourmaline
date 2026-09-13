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
//    grid with cellSize = 2x brick size. A probe sphere (R = 1.25x brick,
//    diameter 2.5x brick) overlaps up to 3 cells per axis (27 total), and
//    a cell's probe list is a superset of everything that can shade a
//    point inside it. Runtime sampling gathers from every level's
//    containing cell and blends with a partition-of-unity weight; the
//    finest nearby probes naturally dominate.
//  - Wall awareness (baked, cheap): each probe stores 6 FP16 obstacle
//    distances (+x,-x,+y,-y,+z,-z). A shading point beyond a probe's
//    recorded obstacle is behind a wall from that probe's point of view
//    and the probe is downweighted to zero — classic baked-GI leak fix,
//    no runtime rays needed.
//
// Storage: 96 B per probe on the GPU (SgProbeGpu, half-float amplitudes).
// The .sggi file stores the same data LZ4-compressed (~64-80 B/probe).

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
	float visibility[6] = {};       // +x,-x,+y,-y,+z,-z obstacle distances
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
