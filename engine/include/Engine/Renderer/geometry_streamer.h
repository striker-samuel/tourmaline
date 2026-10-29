// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU geometry LOD streaming + sparse residency (the LOD streaming plan,
// stages 1-3 of 6; see AGENTS.md's TOURMALINE_GEOMETRY_STREAMING entry).
//
// THE THREE-LOD SEPARATION (the plan's core model):
//   Desired LOD  - what the camera would pick over the FULL cooked chain
//                  (every sibling's simplify error is known CPU-side even
//                  when its payload is not in VRAM).
//   Resident LOD - which geometry payloads actually sit in the static
//                  vertex/index heaps (a SPARSE, non-contiguous subset:
//                  LOD0 + LOD3 resident while LOD1/LOD2 are absent is a
//                  first-class state).
//   Drawn LOD    - the best ACCEPTABLE RESIDENT level the picker serves
//                  without stalling: World::SelectLodIndex selects over
//                  the resident subset ONLY, so a missing middle LOD
//                  degrades to whatever neighbor is in error budget -
//                  the existing SelectLodIndexForDistance math already
//                  has exactly these semantics over a sparse level list.
//
// HOW SPARSITY IS EXPRESSED (zero format/data changes): a deferred LOD
// sibling keeps its LodInfo REGISTERED in the cluster (GetMeshLod finds
// it - RebuildLodChains can see the full chain) but has NO pool mesh
// (FindMeshByUri misses). RebuildLodChains appends it as a
// resident=false level and CONTINUES (the old code broke the chain at
// the first unresolvable sibling - that would strand a resident coarse
// LOD behind a deferred middle one). Selection compacts the resident
// levels; demand compares against the full chain.
//
// Per SHARED MESH ASSET, not per instance: the deferred record + the
// promoted pool mesh live on the Cluster that cooked the uri; 100 nodes
// sharing one LOD3 sibling upload it once (the pool + uri registry are
// per-asset today - the streaming rides that).
//
// v1 SCOPE (gated OFF by default - the world-fields precedent):
//   IN : stage 1 (load without uploading every sibling; LOD0 bases +
//        the coarsest sibling per group PINNED resident, everything
//        between deferred), stage 2 (demand-driven promote of
//        individual LODs), stage 3 (residency-aware selection - every
//        submitted draw references only resident geometry BY
//        CONSTRUCTION: the picker never selects a deferred level).
//   OUT: stage 4 (physical sparse VkBuffer backing - eviction frees
//        RANGES for reuse, the monoblocks keep their committed size;
//        enableEviction is OFF because the gfx filter has NO per-mesh
//        removal path today - RegisterWorldMeshes is append/dedup and
//        gfx::UploadStaticData is a full rebuild, so an evicted mesh
//        would leave a live gfx entry pointing at a freed range - the
//        classic stale-reference hazard the plan warns about), stage 5
//        (cascade-specific demand - cascades keep sharing the camera
//        chain), stage 6 (disk streaming - the pak image stays pinned;
//        promotes are pure offset math into it), skinned siblings
//        (always resident - the deformed-heap parity math must not see
//        ranges disappear).
//
// The promote path is timeline-safe by construction: UploadStaticMeshes
// Direct allocates FRESH arena ranges and copies through the RHI upload
// streamer (retired by its batch); nothing is freed in v1, so the
// DeferFreeStaticMeshRanges retirement discipline is not yet exercised
// - it becomes load-bearing when eviction lands.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace smvk {

// The env gate (the DebugImageDumpArmed pattern: static-cached, read
// once; ships in all builds - the env is the only switch). Parsed by
// RunConfig for the warn/log/known-list side; the SERIALIZER reads this
// directly because cluster load happens before any renderer state
// exists (the gate must decide at parse time what gets uploaded).
inline bool GeometryStreamingEnabled() {
	static const bool enabled = [] {
		const char* e = std::getenv("TOURMALINE_GEOMETRY_STREAMING");
		return e && (std::strcmp(e, "1") == 0 || std::strcmp(e, "on") == 0
			|| std::strcmp(e, "true") == 0);
	}();
	return enabled;
}

// ---- config ---------------------------------------------------------------
//
// The plan's starting values (profiling starting points, not measured
// optima). v1-live fields: uploadBytesPerFrame (the promote throttle),
// pinCoarsestLod, prefetchLevels. The rest is the policy surface the
// later stages consume - present, documented, and inert until then.

struct GeometryStreamingConfig {
	// Working-set ceiling for DEMAND-LOADED siblings (the pinned LOD0
	// bases + coarsest fallbacks ride OUTSIDE this - they are load-time
	// residents like today). v1: accounting only (promotes stop when the
	// promoted bytes exceed the budget); no eviction exists to enforce
	// it downward.
	uint64_t gpuBudgetBytes = 512ull << 20;
	// Promote throttle: sum of promoted payload bytes per frame (the
	// texture streamer's maxUpgradesPerFrame discipline, in bytes -
	// mesh payloads are far chunkier than mip tails).
	uint64_t uploadBytesPerFrame = 16ull << 20;
	// Promote throttle: max sibling promotes per frame (bounds the
	// per-promote registration work + the gfx table rebuild cadence).
	uint32_t maxPromotesPerFrame = 4;

	// Idle frames before a demand-loaded sibling is EVICTION-ELIGIBLE.
	// INERT in v1 (enableEviction false) - tracked here so the policy
	// constants ship with the accounting.
	uint32_t idleFramesBeforeEvict = 60;
	// Levels finer than the desired one to prefetch (hides the promote
	// latency when the camera approaches). 0 = demand only. v1-live:
	// the demand window includes desired - prefetchLevels FINER
	// neighbors of the drawn level.
	uint32_t prefetchLevels = 1;

	// The coarsest sibling per cooked group stays resident from load
	// (the plan's pinned fallback: whatever happens, a drawable level
	// beyond LOD0 exists).
	bool pinCoarsestLod = true;

	// Stage-4+ surfaces - INERT in v1 (see the file header).
	bool enableEviction = false;
	bool enableSparseBacking = false;
};

// ---- per-frame streaming state -------------------------------------------

struct GeometryStreamingStats {
	uint64_t promotedCount = 0;      // siblings promoted since load
	uint64_t promotedBytes = 0;      // their payload bytes (vert+index)
	uint64_t deferredAtLoad = 0;     // the load-time census
	uint64_t deferredBytesAtLoad = 0; // payload bytes NOT uploaded at load
	uint64_t promotesLastUpdate = 0; // this tick's promotes
	uint64_t demandMisses = 0;       // cumulative (mesh, level) demands
	                                 // that were not resident
};

// The streamer state the WorldRenderer ticks (the promote machinery
// itself lives on Cluster::PromoteDeferredMeshLod / World::PromoteLodLevel
// - the asset owns its payload; this struct owns POLICY).
struct GeometryStreamer {
	GeometryStreamingConfig config{};
	GeometryStreamingStats stats{};

	// Budget/throttle decisions for one promote candidate.
	[[nodiscard]] bool CanPromoteThisFrame(uint64_t bytesThisFrame,
		uint32_t promotesThisFrame) const {
		if (promotesThisFrame >= config.maxPromotesPerFrame)
			return false;
		if (bytesThisFrame >= config.uploadBytesPerFrame)
			return false;
		if (stats.promotedBytes >= config.gpuBudgetBytes)
			return false;
		return true;
	}

	void NotePromote(uint64_t payloadBytes) {
		++stats.promotedCount;
		stats.promotedBytes += payloadBytes;
		++stats.promotesLastUpdate;
	}

	void BeginUpdate() { stats.promotesLastUpdate = 0; }
};

} // namespace smvk
