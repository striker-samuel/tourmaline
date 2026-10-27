// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// ============================================================================
// WorldPartition — the top-level spatial streaming controller.
//
// ARCHITECTURE (docs/world_partition.md is the full reference):
//
//   WorldPartition (this module)  — WHAT parts of the world exist in
//     memory: world-cell spatial addressing, streaming interests,
//     priorities, lifecycle, budgets.
//   Resource providers            — HOW each subsystem stores/renders its
//     data: terrain (2D height/material atlases), scene geometry
//     (instances/meshlets/HLOD), world fields (sparse 3D bricks —
//     Engine/WorldFields), later collision/navigation/entities.
//   Specialized resident caches   — the physical allocators (2D terrain
//     atlases, 3D field atlas pools, geometry buffers, CPU collision
//     tiles). Fixed-slot 3D texture atlases and variable-sized geometry
//     allocators are fundamentally different; only the residency
//     ACCOUNTING is shared.
//
// The partition never forces one physical representation on a client:
// terrain is predominantly a 2D heightfield (2D pages + a quadtree),
// fog/wind are sparse 3D bricks. The SHARED elements are the world
// addressing (below) and the streaming interest/scheduler flow.
//
// ---- World grid (1 world unit = 1 metre) ----
//   Root streaming cell : 256 x 256 m (XZ) — scene streaming, object
//     ownership, coarse terrain data.
//   Subcell             :  64 x  64 m — terrain patches, vegetation
//     groups, local streaming.
//   Cells are 2D for terrain/ordinary scene content; OPTIONAL 3D
//     addressing exists for caves/stacked environments/world-field
//     bricks (a field brick IS a 3D "cell" at its own resolution — see
//     the alignment helpers below).
//
// Alignment relationships (NOT ownership): a 256 m root cell maps onto
// four 64 m fog-brick footprints per horizontal axis at 1 m/voxel
// (BricksPerRootCell(1.0) == 4), or exactly one 256 m wind-brick
// footprint at 4 m/voxel (BricksPerRootCell(4.0) == 1). A fog volume
// spanning several cells is still one logical field asset whose bricks
// stream independently.
//
// ---- CPU coordinates ----
// Interests carry double-precision world positions; cell keys are
// signed 64-bit grid indices (floor semantics — a cell boundary at -256
// is cell -1, exactly like the brick addressing's floor discipline).
//
// ---- v1 scope ----
// The provider seam + interest flow + the frame sequence below, all
// CPU-pure and testable. The request dedupe/priority/dependency engine
// and the per-layer CPU/GPU budget enforcement grow here next; the
// WorldFields provider (Engine/WorldFields/WorldFieldProvider.h) is the
// first live client, terrain/ecology/scene follow.
// ============================================================================

#include <cassert>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace engine::worldpartition {

// ---------------------------------------------------------------------------
// Cell constants (1 world unit = 1 metre).
// ---------------------------------------------------------------------------
inline constexpr double kRootCellSize = 256.0; // world cells (XZ)
inline constexpr double kSubcellSize = 64.0; // terrain patches / veg groups

// ---------------------------------------------------------------------------
// World cell keys — 2D (XZ) for terrain and ordinary scene content.
// ---------------------------------------------------------------------------
struct WorldCellKey
{
	int64_t x = 0;
	int64_t z = 0;

	bool operator==(const WorldCellKey& o) const { return x == o.x && z == o.z; }
	bool operator!=(const WorldCellKey& o) const { return !(*this == o); }
};

struct WorldCellKeyHash
{
	size_t operator()(const WorldCellKey& k) const noexcept
	{
		const uint64_t h = 1469598103934665603ull
			^ (static_cast<uint64_t>(static_cast<int64_t>(k.x)) * 1099511628211ull)
			^ (static_cast<uint64_t>(static_cast<int64_t>(k.z)) * 1099511628211ull >> 3);
		return static_cast<size_t>(h);
	}
};

// Floor discipline (NEVER truncation): -1 m is cell -1; exactly -256 m
// is cell -1; -256.001 is cell -2. Same rule as the WorldFields brick
// addressing — the two schemes must agree at their shared boundaries.
inline WorldCellKey GetWorldCell(glm::dvec3 pos, double cellSize = kRootCellSize)
{
	return WorldCellKey{
		int64_t(std::floor(pos.x / cellSize)),
		int64_t(std::floor(pos.z / cellSize)),
	};
}

// World-space bounds of a cell ([min, min+size) on XZ; unbounded Y).
inline void WorldCellBounds(const WorldCellKey& key, double cellSize,
	glm::dvec3& outMin, glm::dvec3& outMax)
{
	outMin = glm::dvec3(double(key.x) * cellSize, 0.0, double(key.z) * cellSize);
	outMax = outMin + glm::dvec3(cellSize, 0.0, cellSize);
}

// Subcell (64 m) key — terrain patch granularity. Distinct type so a
// cell and a subcell can never be silently mixed.
struct WorldSubcellKey
{
	int64_t x = 0;
	int64_t z = 0;
	bool operator==(const WorldSubcellKey& o) const { return x == o.x && z == o.z; }
};

inline WorldSubcellKey GetWorldSubcell(glm::dvec3 pos, double subcellSize = kSubcellSize)
{
	return WorldSubcellKey{
		int64_t(std::floor(pos.x / subcellSize)),
		int64_t(std::floor(pos.z / subcellSize)),
	};
}

inline WorldCellKey SubcellToCell(const WorldSubcellKey& s,
	double cellSize = kRootCellSize, double subcellSize = kSubcellSize)
{
	return WorldCellKey{
		int64_t(std::floor((double(s.x) * subcellSize) / cellSize)),
		int64_t(std::floor((double(s.z) * subcellSize) / cellSize)),
	};
}

// ---------------------------------------------------------------------------
// Field-brick alignment helpers (relationships, not ownership): how many
// 64-voxel brick footprints of a field with `voxelSize`-metre voxels tile
// one root cell per horizontal axis. 1 m/voxel fog -> 4 (a subcell per
// brick); 4 m/voxel wind -> 1 (a root cell per brick).
// ---------------------------------------------------------------------------
inline constexpr uint32_t BricksPerRootCell(double baseVoxelSizeMeters)
{
	const double brickFootprint = 64.0 * baseVoxelSizeMeters;
	return uint32_t(kRootCellSize / brickFootprint);
}

// ---------------------------------------------------------------------------
// Streaming model (the scheduler seam — v1 carries the flow; the dedupe/
// priority/dependency engine lands with the terrain client).
// ---------------------------------------------------------------------------
enum class StreamLayer : uint8_t
{
	Terrain = 0,
	Scene = 1,
	Geometry = 2,
	WorldField = 3,
	Collision = 4,
	Navigation = 5,
};
inline constexpr uint32_t kStreamLayerCount = 6;

inline constexpr uint32_t StreamLayerBit(StreamLayer l)
{
	return uint32_t(1) << uint32_t(l);
}

// The lifecycle a streamed resource moves through. "Requested -> IO/
// Generate -> CpuReady -> GPU upload -> GpuReady -> Active -> Retiring":
// a resource being loaded is NOT safe to use. CPU-only resources
// (collision, navigation) activate from CpuReady; GPU resources need
// upload completion, barriers and valid descriptor/page-table
// publication (the WorldFields publication contract is one instance).
enum class DesiredResidency : uint8_t
{
	Metadata = 0, // manifest/index bytes only
	CpuReady = 1, // decoded on the CPU, not yet uploaded
	GpuReady = 2, // uploaded + published, not yet activated
	Active = 3, // visible/simulatable
};

// One streaming-interest source: the player, camera, a vehicle, an editor
// viewport, a teleport destination. Providers translate the interest
// set into their own desired resources (a field provider centers its
// clipmaps on the primary interest and leads by velocity; a future
// terrain provider requests the covering height pages).
struct StreamingInterest
{
	glm::dvec3 position{ 0.0 };
	glm::dvec3 velocity{ 0.0 }; // m/s — lookahead direction (the prefetch
	// rule: d_prefetch >= v_max * t_worstReady + d_safety)
	double radiusMeters = 256.0;
	float priority = 1.0f; // higher wins (providers weight per layer)
	uint32_t layerMask = 0xFFFFFFFFu; // StreamLayerBit(...) set
	DesiredResidency desired = DesiredResidency::GpuReady;
};

// A generic (layer, cell, priority, desired) request — what a provider
// asked the scheduler for. Providers may carry richer private payloads;
// this is the shared currency the dedupe/priority engine will resolve.
struct StreamRequest
{
	StreamLayer layer = StreamLayer::WorldField;
	WorldCellKey cell{};
	DesiredResidency desired = DesiredResidency::GpuReady;
	float priority = 1.0f;
	uint64_t frameRequested = 0;
};

// Request sink handed to GatherRequests (pointer+count in, fixed
// capacity — the partition pre-sized buffer; overflow is a provider bug
// and asserts).
class StreamRequestSink
{
public:
	explicit StreamRequestSink(StreamRequest* storage, size_t capacity)
		: storage_(storage), capacity_(capacity)
	{
	}
	void Push(const StreamRequest& r)
	{
		assert(count_ < capacity_ && "StreamRequestSink overflow (provider over-produced)");
		if (count_ < capacity_)
			storage_[count_++] = r;
	}
	const StreamRequest* Requests() const { return storage_; }
	size_t Count() const { return count_; }
	void Clear() { count_ = 0; }

private:
	StreamRequest* storage_ = nullptr;
	size_t capacity_ = 0;
	size_t count_ = 0;
};

// ---------------------------------------------------------------------------
// The provider seam. A provider owns ONE subsystem's streaming response
// (what it wants resident, how it loads, how it retires). Pointer+count
// signatures per the AGENTS.md API convention.
//
// Threading: OnStreamingInterests/GatherRequests/CommitFrame/RetireFrame
// run on the RENDER thread inside UpdateFrame (well-defined sync points
// — the doc's frame sequence). CPU work (IO/decode/generation) may happen
// on workers a provider owns/manages, but GPU publication and scene
// mutation happen ONLY in these callbacks (resources finishing after
// this frame's publication point defer to the next frame).
// ---------------------------------------------------------------------------
class IWorldStreamProvider
{
public:
	virtual ~IWorldStreamProvider() = default;
	virtual const char* ProviderName() const = 0;
	// The resolved interest set (deduped by the partition; highest
	// priority first). Providers snapshot what they need.
	virtual void OnStreamingInterests(const StreamingInterest* interests,
		size_t count) = 0;
	// Optional (v1: the partition records requests for diagnostics; the
	// dedupe/priority engine grows here). Return the request count pushed.
	virtual size_t GatherRequests(StreamRequestSink* sink,
		uint64_t frameIndex) { (void)sink; (void)frameIndex; return 0; }
	// Commit this frame: publish completed resources (uploads + page
	// tables + activation deltas), inside the frame's budget.
	virtual void CommitFrame(uint64_t frameIndex) = 0;
	// Retire resources no longer required, respecting in-flight GPU
	// references (the deferred-retirement discipline).
	virtual void RetireFrame(uint64_t frameIndex) { (void)frameIndex; }
};

// ---------------------------------------------------------------------------
// WorldPartition — the orchestrator. Owns the interest set + provider
// list and drives the frame sequence (docs §8):
//   1) interests (SetInterests, before UpdateFrame)
//   2) providers gather desired resources
//   3) resolve duplicate requests + dependencies   (v1: sorted recording)
//   4) dispatch CPU work/IO/GPU uploads within budgets
//      (provider-internal in v1; the shared scheduler lands with the
//      terrain client)
//   5) publish only completed, safe-to-use resources (CommitFrame)
//   6) retire, respecting in-flight GPU references (RetireFrame)
// Run BEFORE constructing frame render data.
// ---------------------------------------------------------------------------
class WorldPartition
{
public:
	WorldPartition() = default;
	~WorldPartition() = default;
	WorldPartition(const WorldPartition&) = delete;
	WorldPartition& operator=(const WorldPartition&) = delete;

	void AddProvider(IWorldStreamProvider* provider);
	size_t ProviderCount() const { return providers_.size(); }

	// The frame's interest set (copied; render thread, before
	// UpdateFrame). Higher-priority-first sort is applied once here.
	void SetInterests(const StreamingInterest* interests, size_t count);

	// The frame sequence (see the class comment). frameIndex is the
	// monotonic frame counter providers key their clocks on.
	void UpdateFrame(uint64_t frameIndex);

	// ---- inspection (pointer + count) ----
	const StreamingInterest* Interests() const { return interests_.data(); }
	size_t InterestCount() const { return interests_.size(); }
	const StreamRequest* Requests() const { return requestBuffer_.data(); }
	size_t RequestCount() const { return requestCount_; }
	// Diagnostics: the distinct active root cells across all interests'
	// radii (the editor's residency-map backing data grows from here).
	size_t ActiveCellCount() const;

private:
	std::vector<IWorldStreamProvider*> providers_; // not owned
	std::vector<StreamingInterest> interests_; // sorted priority-first
	std::vector<StreamRequest> requestBuffer_; // gather scratch (grows)
	size_t requestCount_ = 0;
	StreamRequestSink MakeSink();
};

} // namespace engine::worldpartition
