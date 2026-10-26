// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Bake-input hashing: the invalidation model that keeps stale lighting
// out of paks and drives partial re-bakes.
//
// Every bake UNIT (a convex volume, a cubemap capture, the world field)
// hashes its own inputs; a re-bake skips units whose recorded hash still
// matches, and the PAK ASSEMBLY GATE refuses to embed any artifact whose
// recorded hash does not match the freshly computed one — an old bake
// can never ride into a pak silently (the incident: ten-day-old daylight
// cubemaps embedding into a dusk-cooked pak).
//
// The unit hash:
//   unitHash = H( envHash, staticLightsHash, unitDefHash,
//                 bakeConfigHash, reachHash )
//
//   - ENV BLOCK (sun az/el/I + color, sky I + color): a GLOBAL input,
//     folded into every unit — any env change invalidates every capture,
//     volume and the world field.
//   - STATIC-MARKED LIGHTS (the bake light set): also GLOBAL (a light
//     reaches arbitrarily far through bounces).
//   - The GEOMETRY MERKLE: the scene's traced static content hashed into
//     1 m voxels. A cell's hash = the ordered fold over its member leaf
//     hashes; a leaf = H(retained mesh bytes, node global transform
//     bits, material field bits + texture ids) for every (node, mesh,
//     material) the trace scene includes — the hash tracks exactly
//     what bakes trace. A mesh edit/move dirties exactly the cells its
//     world AABB covers (old AND new cells on moves).
//   - REACH: the capture folds its OBSERVED cells (the cells its own
//     deterministic ray set traverses — RecordCaptureReach), so an edit
//     dirties exactly the captures that see it and new content in a
//     formerly-empty traversed cell dirties too. A volume folds its OBB
//     + a 2-cell margin ring (the bounce approximation: the immediate
//     surroundings carry the dominant bounced energy into the region;
//     deeper paths are multi-bounce attenuated — a per-volume observed
//     sweep costs ~the bake itself, so the margin stands). The world
//     field folds the whole root (its bake path-traces globally).
//
// The 1 m cell lower bound (the case): the finest useful cell is bound
// by the re-bake units' REACH and the content's ATOMICITY. Captures
// carry 1–8 m influence radii and the cvol lattice is authored at
// 0.25–1 m, so sub-meter cells cannot shrink any re-bake set — a 2 m
// capture covers the same cells at 10 cm or 1 m; the borderline ring is
// rounding noise. The MESH is the hash atom: a 10 cm banner move flips
// the leaf's transform hash, so the dirty set is already minimal at any
// cell size — cells finer than the mesh feature buy nothing but diff
// cost (~1000x the leaves for the same scene). Coarser cells spread the
// dirty region wider than the change and over-invalidate captures on
// its border. 1 m also maps 1:1 onto the authored probe lattice.
//
// Determinism: same scene bytes => same hashes (sorted members, sorted
// lights, fixed cell order, raw float bits) — the bake and the pak gate
// replay the same tour and must agree bit-for-bit.
//
// NO COMPAT: hashless records/manifest rows are REFUSED by the pak gate
// (an unverifiable bake is a stale bake). Old projects re-bake once.

#pragma once

#include <Engine/Renderer/mesh.h> // StaticMeshUpload
#include <Engine/App/cluster.h>
#include <Engine/GI/bake.h>       // GiTraceScene (the reach sweep)
#include <Engine/Scene/scene.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace smvk::gi {

// FNV-1a 64 with a length finish (the artifact-cache grade, 16 lowercase
// hex). Self-contained: the engine does not link the cache tool lib.
std::string BakeInputHash(const void* data, size_t size);
inline std::string BakeInputHash(const std::string& s) {
	return BakeInputHash(s.data(), s.size());
}

// ---------------------------------------------------------------------------
// The geometry merkle
// ---------------------------------------------------------------------------

struct BakeCellGrid {
	// The tile lower bound (see the header comment for the case).
	static constexpr float kCell = 1.0f;
	// The reach margin ring (cells beyond a unit's region; the bounce
	// approximation — see the header comment).
	static constexpr int32_t kMargin = 2;

	// (x,y,z) cell index -> the member-set hash.
	std::map<std::array<int32_t, 3>, std::string> cells;
	// The global fold over every cell (the world-field view — ANY
	// geometry change flips it, matching the octree bake's global path
	// trace).
	std::string root;
	size_t meshCount = 0; // members included (the trace-scene set)
};

// Hashes the scene's traced static geometry into the grid. Mirrors
// BuildTraceSceneFromCluster's membership (the retained uploads + node
// globals + materials — the superset incl. masked cutouts + decal
// overlays, so the hash covers everything any bake opts may trace).
// Recalculates global transforms like the trace builder does.
BakeCellGrid BuildBakeCellGrid(Cluster& cluster);

// ---------------------------------------------------------------------------
// The global inputs
// ---------------------------------------------------------------------------

// The resolved ACTIVE env block's bake-relevant fields (sun az/el/I +
// color, sky I + color — the EnvForBake set; exposure is a view setting,
// not a bake input; fog is not traced by bakes). Folded into EVERY unit
// hash.
std::string BakeEnvHash(const Scene& scene);

// Every static-marked light (the bake light set), sorted by node id:
// flags/type/transform/color/intensity/range/cones/cookie. Also folded
// into every unit hash — a light reaches arbitrarily far through
// bounces, so a light change is a global invalidate. (The lights live
// in the cluster pool; scene.lightForNode maps the nodes.)
std::string BakeLightsHash(const Cluster& cluster);

// ---------------------------------------------------------------------------
// Unit reach + config hashes
// ---------------------------------------------------------------------------

// The ordered fold over the cells overlapping an AABB (empty geometry
// cells fold a stable marker, so new content in a formerly-empty region
// changes the fold).
std::string BakeRegionHash(const BakeCellGrid& grid,
	const glm::vec3& mn, const glm::vec3& mx);

// A convex-volume unit hash: env + lights + the volume definition's
// bake-relevant fields (priority/spacing/fades — the shape rides the
// node global, folded) + the bake config scalar set + the cells
// overlapping the volume's OBB (+ the margin ring).
struct CvolBakeConfigHashInputs {
	uint32_t captureDirs = 0;    // 0 = the engine default (CvBakeConfig{})
	uint32_t maxBounces = 0;     // 0 = default
	float captureSpacing = 0.0f; // 0 = auto (spec-derived)
	uint32_t smoothingPasses = 0;
	bool maskTransmission = false;
};
std::string CvolBakeUnitHash(const BakeCellGrid& grid,
	const Cluster& cluster, const SceneCvol& def,
	const glm::mat4& nodeGlobal, const CvolBakeConfigHashInputs& cfg);

// A cubemap-capture unit hash: env + lights + the capture definition
// (position/influence/fade/box/pinned) + the bake config scalar set +
// the capture's OBSERVED reach — the cells its own ray set traverses
// (RecordCaptureReach), recorded at bake time as the cell id list. The
// gate recomputes with the CURRENT grid hashes at the recorded ids, so
// both a changed seen cell AND new content in a formerly-empty
// traversed cell dirty the capture.
struct CubemapBakeConfigHashInputs {
	uint32_t size = 128;
	uint32_t rays = 128;
	bool maskTransmission = false;
};
using CellId = std::array<int32_t, 3>;
std::string CubemapBakeUnitHash(const BakeCellGrid& grid,
	const Cluster& cluster, const glm::vec3& position,
	float influenceRadius, float fadeRadius, const glm::vec3& boxMin,
	const glm::vec3& boxMax, bool pinned,
	const CubemapBakeConfigHashInputs& cfg,
	const std::vector<CellId>& reachCells);

// The observed-reach sweep for one capture: casts a deterministic
// Fibonacci-sphere ray set from the position through the trace scene
// and records every 1 m cell each ray TRAVERSES up to its hit (plus the
// hit cell + a one-cell ring — the bounce neighborhood), sorted unique.
// One BVH traversal per ray; the sweep is the cheap part of a capture
// bake (the bake itself shades thousands of rays per face).
std::vector<CellId> RecordCaptureReach(const GiTraceScene& trace,
	const glm::vec3& position, uint32_t dirCount);

// The .reach sidecar (the manifest-adjacent bake bookkeeping, named
// from the BOUND set uuid — not a stem guess): the per-capture recorded
// cell id lists. Binary: magic + version + per capture (rowIndex, count,
// cell ids). Empty rows are legal (a capture that sees nothing move).
struct CaptureReachFile {
	static constexpr uint32_t kMagic = 0x52454143u; // 'REAC'
	static constexpr uint32_t kVersion = 1;
	struct Entry {
		uint32_t rowIndex = 0;
		std::vector<CellId> cells;
	};
	std::vector<Entry> entries;
};
bool SerializeCaptureReach(const CaptureReachFile& reach,
	std::vector<uint8_t>& out);
bool ParseCaptureReach(const uint8_t* data, size_t size,
	CaptureReachFile& out);

// The world-field unit hash: env + lights + the bake config + the
// WHOLE grid root (the octree bake path-traces globally; per-node
// partiality is phase 2). NOTE: the probe BUDGET (--max-probes) is a
// coverage knob, not a semantic one — it is deliberately NOT folded
// (a converged bake is budget-independent; the budget-hit warning
// stays loud).
struct WorldBakeConfigHashInputs {
	uint32_t maxBounces = 0;     // 0 = default
	bool maskTransmission = false;
};
std::string WorldBakeUnitHash(const BakeCellGrid& grid,
	const Cluster& cluster, const WorldBakeConfigHashInputs& cfg);

// ---------------------------------------------------------------------------
// The canonical config derivations (the ONE source both sides use)
// ---------------------------------------------------------------------------
// The bake owner folds the config it EFFECTIVELY ran with; the pak gate
// resolves the canonical values. Both go through these so an un-flagged
// (node-authored) run's records match the gate's recomputation —
// CLI-OVERRIDDEN semantic knobs mismatch by design and the pak refuses
// (author the exportSettings node instead; the mask/transmission node
// field is the authoring surface).

// The cvol bake config an un-flagged GiBaker run uses (Args defaults:
// 1536 capture dirs, 3 bounces, auto capture spacing, 3 smoothing
// passes).
inline CvolBakeConfigHashInputs CanonicalCvolCfgHash(
	bool maskTransmission) {
	CvolBakeConfigHashInputs c{};
	c.captureDirs = 1536;
	c.maxBounces = 3;
	c.captureSpacing = 0.0f;
	c.smoothingPasses = 3;
	c.maskTransmission = maskTransmission;
	return c;
}

// The world-field bake config (3 bounces default; the capture-dirs knob
// does not exist on the octree bake).
inline WorldBakeConfigHashInputs CanonicalWorldCfgHash(
	bool maskTransmission) {
	WorldBakeConfigHashInputs c{};
	c.maxBounces = 3;
	c.maskTransmission = maskTransmission;
	return c;
}

// The cubemap capture config — node-derived (the exportSettings'
// cubemapSize/cubemapRays are the authoring surface; the cook's capture
// stage bakes with the merged node values).
inline CubemapBakeConfigHashInputs CanonicalCubemapCfgHash(
	const SceneExportSettings& es) {
	CubemapBakeConfigHashInputs c{};
	c.size = es.cubemapSize;
	c.rays = es.cubemapRays;
	c.maskTransmission = es.maskTransmission;
	return c;
}

} // namespace smvk::gi
