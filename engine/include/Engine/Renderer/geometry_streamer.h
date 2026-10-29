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
//        CONSTRUCTION: the picker never selects a deferred level), and
//        the LOGICAL half of stage 4: eviction back to deferred when
//        the promoted working set exceeds gpuBudgetBytes (the idlest
//        sibling past idleFramesBeforeEvict; a level touched this
//        frame - drawn or demanded - never evicts). Eviction frees
//        ARENA RANGES for reuse (DestroyMesh's deferred retirement) -
//        the monoblocks keep their committed size, which is the plan's
//        option 1; option 2 (sparse VkBuffer backing, physical page
//        release) stays out.
//   OUT: stage 5 (cascade-specific LOD demand) - RETIRED BY OWNER
//        DECISION, not deferred: the GPU-driven shadow path already
//        prunes per-view work at the MESLET/cluster level
//        (gfx::BuildShadowList culls each view's draw list against
//        that view's planes + the contribution sweep), and the node
//        LOD is error-driven, so far cascades naturally receive
//        mostly-far (already-coarse) geometry. Per-cascade LOD floors
//        would add four candidate lists + direct-leg plumbing + more
//        streaming demand sources, risk seam artifacts where cascades
//        overlap (a mesh at a cascade boundary drawn at different
//        LODs on each side), and buy little the meshlet visibility
//        does not already capture. Do NOT build it without a measured
//        shadow-raster cost case that the per-view culling leaves
//        unaddressed. Also out: stage 6 (disk streaming - the pak
//        image stays pinned; promotes are pure offset math into it),
//        skinned siblings (always resident - the deformed-heap parity
//        math must not see ranges disappear), physical sparse backing.
//
// The promote/evict paths are timeline-safe by construction: promotes
// allocate FRESH arena ranges; evicts ride DestroyMesh's
// DeferFreeStaticMeshRanges (the +MaxFramesInFlight retirement margin)
// - an in-flight frame drawing an evicted sibling's ranges finishes
// before the range returns to the free list.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

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

	// Idle frames before a demand-loaded sibling is EVICTION-ELIGIBLE
	// (nothing drawn or demanded it within the window). 0 = evictable
	// the frame after its last touch. Pins: TOURMALINE_GEOMETRY_EVICT_IDLE.
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

	// Eviction ON: the promoted working set is bounded by gpuBudgetBytes
	// (the idlest siblings past idleFramesBeforeEvict return to
	// deferred). The gfx-table rebuild (ResetStaticRegistrations + the
	// re-register walk skips destroyed pool entries) is the per-mesh
	// removal mechanism.
	bool enableEviction = true;
	// Stage-4's physical half - INERT in v1 (see the file header).
	bool enableSparseBacking = false;
};

// ---- per-frame streaming state -------------------------------------------

struct GeometryStreamingStats {
	uint64_t promotedCount = 0;      // siblings promoted since load
	uint64_t promotedBytes = 0;      // their payload bytes (vert+index)
	uint64_t evictedCount = 0;       // siblings evicted back to deferred
	uint64_t evictedBytes = 0;       // their freed payload bytes
	uint64_t deferredAtLoad = 0;     // the load-time census
	uint64_t deferredBytesAtLoad = 0; // payload bytes NOT uploaded at load
	uint64_t promotesLastUpdate = 0; // this tick's promotes
	uint64_t evictionsLastUpdate = 0; // this tick's evictions
	uint64_t demandMisses = 0;       // cumulative (mesh, level) demands
	                                 // that were not resident
};

// One entry per PROMOTED sibling (the eviction candidates): the payload
// accounting + the idle clock. Key = worldMesh<<32 | lodIndex.
struct PromotedLodEntry {
	uint64_t payloadBytes = 0;
	uint64_t lastTouchFrame = 0;   // last drawn OR demanded frame
};

// The streamer state the WorldRenderer ticks (the promote/evict
// machinery itself lives on Cluster::Promote/EvictDeferredMeshLod +
// World::Promote/EvictLodLevel - the asset owns its payload; this
// struct owns POLICY).
struct GeometryStreamer {
	GeometryStreamingConfig config{};
	GeometryStreamingStats stats{};
	std::unordered_map<uint64_t, PromotedLodEntry> promoted;

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

	void NotePromote(uint32_t mesh, uint32_t lod, uint64_t payloadBytes,
		uint64_t frame) {
		++stats.promotedCount;
		stats.promotedBytes += payloadBytes;
		++stats.promotesLastUpdate;
		promoted[(uint64_t(mesh) << 32) | lod] =
			PromotedLodEntry{ payloadBytes, frame };
	}

	void NoteTouch(uint32_t mesh, uint32_t lod, uint64_t frame) {
		auto it = promoted.find((uint64_t(mesh) << 32) | lod);
		if (it != promoted.end())
			it->second.lastTouchFrame = frame;
	}

	void NoteEvict(uint32_t mesh, uint32_t lod, uint64_t payloadBytes) {
		++stats.evictedCount;
		stats.evictedBytes += payloadBytes;
		++stats.evictionsLastUpdate;
		stats.promotedBytes -= (stats.promotedBytes >= payloadBytes)
			? payloadBytes : stats.promotedBytes;
		promoted.erase((uint64_t(mesh) << 32) | lod);
	}

	void BeginUpdate() {
		stats.promotesLastUpdate = 0;
		stats.evictionsLastUpdate = 0;
	}

	// The eviction candidate search: over budget, the IDLEST promoted
	// sibling (oldest touch) past the idle window. keyOut/bytesOut
	// filled on hit. A sibling touched THIS frame is never evictable
	// (it is currently drawn or demanded - the invariant that a level
	// in use stays resident).
	[[nodiscard]] bool FindEvictCandidate(uint64_t frame,
		uint64_t& keyOut, uint64_t& bytesOut) const {
		if (!config.enableEviction)
			return false;
		if (stats.promotedBytes <= config.gpuBudgetBytes)
			return false;
		uint64_t bestKey = 0;
		uint64_t bestBytes = 0;
		uint64_t bestTouch = ~uint64_t(0);
		for (const auto& [key, e] : promoted) {
			if (frame <= e.lastTouchFrame)
				continue; // touched this very frame
			if (frame - e.lastTouchFrame < config.idleFramesBeforeEvict)
				continue; // inside the idle window
			if (e.lastTouchFrame < bestTouch) {
				bestTouch = e.lastTouchFrame;
				bestKey = key;
				bestBytes = e.payloadBytes;
			}
		}
		if (bestTouch == ~uint64_t(0))
			return false;
		keyOut = bestKey;
		bytesOut = bestBytes;
		return true;
	}
};

// The config with the env overrides (the DebugImageDumpArmed pattern:
// static-cached, read once; the headless A/B pins for the eviction
// policy). TOURMALINE_GEOMETRY_BUDGET_MB clamps the working-set ceiling
// (small values force eviction churn for testing);
// TOURMALINE_GEOMETRY_EVICT_IDLE pins the idle window in frames (0 =
// evictable the frame after its last touch).
[[nodiscard]] inline const GeometryStreamingConfig&
GeometryStreamingRuntimeConfig() {
	static const GeometryStreamingConfig cfg = [] {
		GeometryStreamingConfig c{};
		if (const char* e = std::getenv("TOURMALINE_GEOMETRY_BUDGET_MB")) {
			const long mb = std::atol(e);
			if (mb > 0)
				c.gpuBudgetBytes = uint64_t(mb) << 20;
		}
		if (const char* e = std::getenv("TOURMALINE_GEOMETRY_EVICT_IDLE")) {
			const long f = std::atol(e);
			if (f >= 0)
				c.idleFramesBeforeEvict = uint32_t(f);
		}
		return c;
	}();
	return cfg;
}

} // namespace smvk
