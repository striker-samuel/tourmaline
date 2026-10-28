// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// PlacementSystem tile runtime (CPU side).
// Scene holds ONE PlacementRef node per layer (params only). All instances
// live here, paged by TileCache, and reach the renderer as TRANSIENT draws
// (PushVisibleDraws) or GPU-resident buffers — never as Scene nodes.
//
// Pipeline per tile (mirrors the GPU kernels in placement_tile.cs.hlsl):
//   candidates → domain sample → field eval → accept → min-distance
//   → prototype + transform → resident instances.

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_domain.h>
#include <Engine/Placement/placement_eval.h>
#include <Engine/Placement/placement_ids.h>
#include <Engine/Placement/placement_point.h>
#include <Engine/Placement/placement_prototype.h>

namespace smvk::placement {

enum class TileState : uint8_t {
	Unloaded = 0,
	Generating,
	Resident,
	Dirty,
	Evicting,
};

struct TileKey {
	TileID tile;
	uint64_t programId = 0;
	bool operator==(const TileKey& o) const { return tile == o.tile && programId == o.programId; }
};
struct TileKeyHash {
	size_t operator()(const TileKey& k) const {
		uint32_t h = SplitMix32((uint32_t)k.tile.x + 0x9e3779b9u);
		h = HashMix32(h, (uint32_t)k.tile.y);
		h = HashMix32(h, (uint32_t)k.tile.z);
		h = HashMix32(h, (uint32_t)(k.programId & 0xFFFFFFFFull));
		h = HashMix32(h, (uint32_t)(k.programId >> 32));
		return (size_t)h;
	}
};

// What changed underneath a tile (roads, biomes, prototype sets...).
// Bump a version to dirty dependent tiles; tiles whose key doesn't
// include the resource keep serving.
struct DependencyVersions {
	uint64_t terrain = 0;
	uint64_t biome = 0;
	uint64_t prototypes = 0;
	uint64_t gameplay = 0;
	bool operator==(const DependencyVersions& o) const {
		return terrain == o.terrain && biome == o.biome
			&& prototypes == o.prototypes && gameplay == o.gameplay;
	}
};

struct PlacementTile {
	TileKey key;
	TileState state = TileState::Unloaded;
	std::vector<RenderInstance> instances; // resident output, SORTED by (prototypeId, StableID)
	DependencyVersions deps;               // versions at generation time
	uint64_t generation = 0;               // PR05: the activation token (bumped per
	                                       // (re)activation; a delayed publish with a
	                                       // stale token is refused)
	uint64_t lastAccess = 0;               // LRU stamp (PlacementSystem::tick_)
	glm::vec3 boundsMin{ 0.0f };           // cached instance bounds (coarse cull)
	glm::vec3 boundsMax{ 0.0f };
	uint64_t aliveBDA = 0; // GPU flag2 buffer for constraint-filtered tiles (0 = all alive)
};

// How to feed candidates into a tile (MVP: fixed-count stratified/grid).
struct TileGenConfig {
	uint32_t candidatesPerTile = 4096;
	float minSpacing = 0.0f;     // 0 = no min-distance pass
	float minScale = 0.8f;
	float maxScale = 1.2f;
	float densityScale = 1.0f;
	bool slopeToRock = false;    // slope rule demo: proto 1 when steep
	float slopeThresholdDeg = 35.0f;
};

// Renderer-agnostic transient draw (no RHI/gfx dependency here).
// The app layer maps these to gfx::PushDraw or direct draws per frame.
struct TransientDraw {
	glm::vec3 position{ 0.0f };
	float yaw = 0.0f;
	glm::vec3 scale{ 1.0f };
	uint32_t prototypeId = 0;
	uint32_t stableLow = 0;
};

class PlacementSystem {
public:
	// Registered evaluators (one layer = one program + domain binding).
	struct LayerBinding {
		uint64_t programId = 0;
		PlacementProgram program;
		DomainKind domainKind = DomainKind::Grid;
		const GridDomain* grid = nullptr;             // not owned (caller keeps alive)
		const PlacementMeshData* mesh = nullptr;      // not owned
		const HeightfieldDomain* heightfield = nullptr; // not owned
		const EcotopeMaskData* ecotope = nullptr;    // not owned (PR04: the
		                                              // painted layer the
		                                              // EcotopeMask op reads)
		PrototypeTable prototypes;
		TileGenConfig config;
	};
	void registerLayer(LayerBinding layer);
	bool hasLayer(uint64_t programId) const;

	// Generate (or refresh) one tile synchronously on CPU — the legacy
	// oracle form: generate + publish in one call (the token contract
	// does not apply: the result is complete when this returns).
	// Returns resident instance count.
	size_t generateTile(uint64_t programId, TileID tile, const DependencyVersions& deps);

	// The token-governed form (PR05): generate WITHOUT publishing (the
	// tile enters Generating with a fresh activation token). The caller
	// publishes via PublishActivation(key, generation) when the result is
	// complete — a stale token (the tile was re-activated, invalidated
	// or dropped meanwhile) refuses and the caller discards its result.
	// The CPU oracle path and the GPU scatter path share this flow.
	bool GenerateTilePending(uint64_t programId, TileID tile,
		const DependencyVersions& deps, uint64_t& generationOut);

	// Mark tiles whose key matches programId dirty (layer edit / dep bump).
	void invalidateLayer(uint64_t programId);
	void invalidateAll();

	// LRU: drop Resident tiles (except those in keep[]) beyond budget.
	void evictBeyond(size_t maxResident, const std::vector<TileKey>& keep = {});

	// ---- World-tile residency (PR05 — the plan's tile integration) ----
	// The CPU residency tick over WORLD tiles (the PlacementRef::tileSize
	// grid; the tile's world rect is HALF-OPEN
	// [tile*tileSize, (tile+1)*tileSize) — the canonical world-space
	// candidate ownership rule: neighbors never duplicate shared
	// borders, and the grid domain's jitter keeps every candidate inside
	// its own tile).
	//
	//   * ACTIVATE the tiles intersecting the interest rects (world XZ,
	//     half-open). Fresh tiles enter Generating; their Activation
	//     carries a GENERATION TOKEN bumped on every (re)assignment, so
	//     a delayed job can never publish stale contents after a rapid
	//     edit cycle (publish with a stale token is refused).
	//   * REGENERATE Resident tiles whose DependencyVersions went stale
	//     (an edit class bumped underneath them).
	//   * EVICT beyond budget, LRU (never the tiles activating this
	//     tick). Camera movement only changes the interest set — a
	//     resident tile that stays intersecting is NOT touched.
	//
	// The caller dispatches the returned activations (CPU oracle or the
	// GPU scatter path), then publishes with
	// PublishActivation(key, generation) — a stale generation is refused
	// and the tile stays dirty for the next tick.
	struct ActivationRequest {
		TileKey key;
		uint64_t generation = 0;
		DependencyVersions deps;      // the versions to record
		bool regenerate = false;      // true = was resident but stale
	};
	// rects: rectCount glm::vec4 world-XZ rects (minX, minZ, maxX, maxZ),
	// half-open. Every tile intersecting any rect activates; resident
	// tiles outside the interest set are NOT touched (camera movement
	// never regenerates — only the interest set changes).
	void UpdateResidency(uint64_t programId, float tileSize,
		const glm::vec4* rects, size_t rectCount,
		const DependencyVersions& deps, size_t maxResidentTiles,
		std::vector<ActivationRequest>& out);

	// Region invalidation (the editor paint path): mark every tile
	// intersecting [rectMin, rectMax] expanded by haloMeters Dirty. The
	// halo carries the cross-boundary influence — a min-spacing winner
	// or a central-difference normal sample reading a neighbouring
	// tile's edge data. Bounded by the layer's world placement.
	void InvalidateRegion(uint64_t programId, float tileSize,
		const glm::vec2& rectMin, const glm::vec2& rectMax,
		float haloMeters);

	// Publishes a completed activation. Refuses (returns false) when the
	// generation token went stale — the tile was re-activated or
	// invalidated while the job ran; the caller drops its result and the
	// next residency tick regenerates.
	bool PublishActivation(const TileKey& key, uint64_t generation);

	// Transient per-frame feed: visible resident tiles → draws.
	// frustumTest(center, radius) returns true when kept; LOD pick via callback.
	// Scene is NEVER touched — output is a scratch vector the caller maps
	// to gfx::PushDraw / direct draws and discards.
	void pushVisibleDraws(uint64_t programId,
		const std::function<bool(const glm::vec3&, float)>& frustumTest,
		std::vector<TransientDraw>& out) const;
	size_t residentTileCount() const;
	size_t residentInstanceCount(uint64_t programId) const;

	// Test/debug access.
	const PlacementTile* findTile(uint64_t programId, TileID tile) const;

	// Iteration over resident tiles of one layer (draw feed, streaming).
	template<typename Func>
	void forEachResidentTile(uint64_t programId, Func&& func) const {
		for (const auto& [key, tile] : tiles_)
			if (key.programId == programId && tile.state == TileState::Resident)
				func(key, tile);
	}

private:
	std::unordered_map<uint64_t, LayerBinding> layers_;
	std::unordered_map<TileKey, PlacementTile, TileKeyHash> tiles_;
	uint64_t tick_ = 0;
};

// Standalone (testable) tile evaluation used by generateTile and — via
// parity tests — by the GPU kernels. Runs: candidates → sample → eval →
// accept → optional min-distance → prototype + transform. When idsOut is
// non-null, receives the StableIDs parallel to the returned instances
// (post-constraint, same order).
std::vector<RenderInstance> EvaluateTile(const PlacementSystem::LayerBinding& layer,
	TileID tile, std::vector<StableID>* idsOut = nullptr);

// Draw-path order: sorts instances (+ids) by (prototypeId, StableID) so
// each prototype forms one contiguous run. Fully deterministic.
void SortInstancesForDraw(std::vector<RenderInstance>& instances, std::vector<StableID>& ids);

// Cached coarse-cull bounds for a tile (call after generation/sort).
void ComputeTileBounds(PlacementTile& tile);

// Deterministic min-distance filter (CPU mirror of the GPU cell-winner
// passes). Order-free snapshot rule, so every thread order / platform /
// backend agrees bit-exactly:
//
//   1. Elect: per cell, the argmax over (PriorityOf, tie -> smaller
//      candidate index) becomes the provisional winner (cell pre-election
//      is itself strongly deterministic).
//   2. Kill: a winner dies iff some OTHER winner in its 27-neighbourhood
//      conflicts (dist < spacing) and beats it under the same
//      (priority, tie -> smaller index) order.
//
// A dead candidate never kills (it lost its cell/support), and all kill
// tests read the elected-winner snapshot — no sequential dependence, so
// the GPU runs elect+kill as data-parallel kernels with barriers between.
// Survivors are compacted in ascending candidate-index order.
//
// Candidates outside `volume` (mesh triangles may overshoot their tile)
// skip the constraint entirely: they neither kill nor die. The GPU uses
// the same skip, so volume choice is part of the parity contract.
//
// NOTE on cell boundaries: positions within 1 ulp of an integer cell
// boundary may round to different cells on CPU (double floor) vs GPU
// (float floor). Measure-zero input, visual determinism still holds,
// strong determinism holds everywhere else.
struct ConstraintVolume {
	glm::vec3 origin{ 0.0f };
	uint32_t dimX = 0, dimY = 0, dimZ = 0; // cells per axis; 0 = unbounded
	float spacing = 1.0f;
};
void ApplyMinDistance(std::vector<RenderInstance>& instances,
	std::vector<StableID>& ids, float spacing, const ConstraintVolume& volume);

// Constraint volume for a layer's tile evaluation. Grid layers get a
// bounded volume covering the tile (+2 stagnant cells, 128m height band);
// everything else is unbounded (CPU exact-map path; GPU falls back to
// CPU constraint — see PlacementConstraint::VolumeFitsGpu).
ConstraintVolume ConstraintVolumeForLayer(const PlacementSystem::LayerBinding& layer);

} // namespace smvk::placement
