#pragma once
// Sparse adaptive-octree reflection-probe placement for cubemap captures.
//
// The uniform grid (spacing^3 probes over the scene bounds) wastes the
// 32-probe runtime budget in open air and leaves coverage seams wherever
// the lattice misses (e.g. an atrium center between rows). This module
// grows an occupancy octree over the scene bounds instead:
//
//   - Open cells larger than maxCellSize subdivide (coverage density).
//   - Cells near surfaces (any of 6 center rays hits within reach)
//     subdivide down to minCellSize/maxDepth (reflection detail where the
//     pixels are).
//   - Solid-embedded leaf centers are dropped (same 6-axial-rays rule as
//     the grid occupancy cull).
//   - Growth is budget-aware: subdividing a mixed leaf nets +7 leaves, so
//     subdivision stops when the budget (maxProbes, the runtime scan
//     budget) is exhausted — remaining mixed leaves emit coarse, which
//     preserves full coverage while detail fills whatever budget is left.
//     Emission order is breadth-first (coarse coverage first), so any
//     truncation keeps coverage, never detail-only.
//
// Geometry enters ONLY through the occlusion predicate (raycast lambda),
// so the header depends on nothing but glm/STL: unit tests drive it with
// stub lambdas, the cookers pass TriangleBvh::Raycast. Per-probe radii
// scale with the emitting cell (influence = half-diagonal, fade = half),
// so coarse air cells cover volumes and fine surface cells stay tight —
// the lit shader's per-pixel scan stays proportional to overlap, and the
// tile cull buckets the rest.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace smvk {

struct CubemapOctreeOptions {
	float maxCellSize = 8.0f;   // open cells larger than this subdivide (coverage)
	float minCellSize = 2.0f;   // never subdivide at/below this (detail floor)
	uint32_t maxDepth = 4;      // root = depth 0 (safety cap)
	float embedDist = 0.3f;     // placement occupancy test (axial rays, meters)
	uint32_t maxProbes = 32;    // hard cap: the runtime tile/global scan budget
};

struct CubemapPlacedProbe {
	glm::vec3 position{ 0.0f };
	float influenceRadius = 10.0f;
	float fadeRadius = 5.0f;
	uint32_t depth = 0;         // emitting cell depth (0 = coarsest)
};

// Occlusion predicate: true when the ray hits anything within maxDist.
using CubemapOcclusionRay =
	std::function<bool(const glm::vec3& origin, const glm::vec3& dir, float maxDist)>;

// Grows the octree over [bmin, bmax] and emits placed probes (breadth
// first, budget-capped). Returns the emitted count; log receives a short
// human-readable summary (depth histogram + drops). Degenerate bounds
// emit a single centered fallback probe.
size_t BuildOctreeProbePositions(const glm::vec3& bmin, const glm::vec3& bmax,
	const CubemapOctreeOptions& opts, const CubemapOcclusionRay& occluded,
	std::vector<CubemapPlacedProbe>& out, std::string& log);

} // namespace smvk
