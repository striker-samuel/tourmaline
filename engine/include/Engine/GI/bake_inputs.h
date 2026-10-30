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
// carry 1–8 m influence radii and the GI-volume lattice is authored at
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
// The bake algorithm revision
// ---------------------------------------------------------------------------
// Folded into EVERY unit hash (volume / capture / world): bake BEHAVIOR
// changes (tracing rules, sampling, the SG fit, transmission handling,
// texel decode caps) alter bake output without touching any scene INPUT,
// so the input hashes alone would keep validating pre-change artifacts.
// Bump this whenever a code change alters what a bake produces; every
// recorded hash then mismatches its re-computation and the pak gate
// refuses the old records wholesale (the no-compat contract — re-bake).
// It is deliberately NOT an input: it rides the unit definition side so
// env/light/geometry hashes stay byte-stable for debugging.
// v2: the FULL-BSDF PBR transport (GGX specular NEE + one-sample-MIS
// lobe sampling — metals reflect instead of absorbing; every prior
// bake under-reports indirect near metallic or glossy surfaces).
inline constexpr uint32_t kBakeAlgorithmRevision = 2;

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
struct GiVolumeBakeConfigHashInputs {
	uint32_t captureDirs = 0;    // 0 = the engine default (GiVolumeBakeConfig{})
	uint32_t maxBounces = 0;     // 0 = default
	float captureSpacing = 0.0f; // 0 = auto (spec-derived)
	uint32_t smoothingPasses = 0;
	bool maskTransmission = false;
	// The firefly clamp ratio (GiVolumeBakeConfig::clampRatio — MUST
	// mirror the bake default so an unset input hashes like a default
	// bake). 0 = the off A/B.
	float clampRatio = 8.0f;
	// The trace scene's albedo mode (GiSceneBuildOptions): the
	// --uniform-albedo white-world override changes every traced value,
	// so it must refuse stale records (the gap the commit review found:
	// a 0.7 <-> off flip kept matching old volumes — the A/B silently
	// compared a stale white-world bake). Defaults mirror the canonical
	// un-flagged GiBaker run (Args.uniformAlbedo = "0.7").
	bool albedoUniform = true;
	glm::vec3 albedoValue{ 0.7f };
};
std::string GiVolumeBakeUnitHash(const BakeCellGrid& grid,
	const Cluster& cluster, const SceneGiVolume& def,
	const glm::mat4& nodeGlobal, const GiVolumeBakeConfigHashInputs& cfg);

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
	uint32_t spp = 32; // transport paths per texel (bumping this re-captures)
	bool maskTransmission = false;
	// The capture transport depth (--cubemap-bounces; the review-found
	// gap: the fold missed it, so a bounce change left stale captures).
	// Default = the cook's capture-stage default.
	uint32_t maxBounces = 5;
	// The trace scene's albedo mode (GiVolumeBakeConfigHashInputs's
	// twin; the capture bake bakes TEXTURED by design — the canonical
	// value is false).
	bool albedoUniform = false;
	glm::vec3 albedoValue{ 0.7f };
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
// cell id lists. Binary: magic + per capture (rowIndex, count,
// cell ids). Empty rows are legal (a capture that sees nothing move).
// NO VERSION SCHEME (the repo data rule) - the magic is the identity;
// the old versioned layout refused at the magic, re-record.
struct CaptureReachFile {
	static constexpr uint32_t kMagic = 0x52454158u; // 'REAX'
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

// (The WORLD-FIELD unit hash + its config inputs
// (WorldBakeConfigHashInputs / WorldBakeUnitHash) are DELETED with the
// legacy .sggi octree bake — stage 3 of the octree retirement: nothing
// hashes the world field anymore. The live per-product unit hashes are
// GiVolumeBakeUnitHash / CubemapBakeUnitHash / BakeRegionHash above.)

// ---------------------------------------------------------------------------
// The canonical config derivations (the ONE source both sides use)
// ---------------------------------------------------------------------------
// The bake owner folds the config it EFFECTIVELY ran with; the pak gate
// resolves the canonical values. Both go through these so an un-flagged
// (node-authored) run's records match the gate's recomputation —
// CLI-OVERRIDDEN semantic knobs mismatch by design and the pak refuses
// (author the exportSettings node instead; the mask/transmission node
// field is the authoring surface).

// The GI-volume bake config an un-flagged GiBaker run uses (Args
// defaults: 1536 capture dirs, 3 bounces, auto capture spacing, 3
// smoothing passes, clamp 8x, white-world 0.7 — the SG-fit convention).
inline GiVolumeBakeConfigHashInputs CanonicalGiVolumeCfgHash(
	bool maskTransmission) {
	GiVolumeBakeConfigHashInputs c{};
	c.captureDirs = 1536;
	c.maxBounces = 3;
	c.captureSpacing = 0.0f;
	c.smoothingPasses = 3;
	c.maskTransmission = maskTransmission;
	c.clampRatio = 8.0f;
	c.albedoUniform = true;   // Args.uniformAlbedo = "0.7" (the tool default)
	c.albedoValue = glm::vec3(0.7f);
	return c;
}

// (CanonicalWorldCfgHash — the world-field config derivation — is deleted
// with WorldBakeUnitHash: stage 3 of the octree retirement, nothing folds a
// world-field hash anymore.)

// The cubemap capture config — node-derived (the exportSettings'
// cubemapSize/cubemapRays are the authoring surface; the cook's capture
// stage bakes with the merged node values). Bounces + albedo are the
// capture-stage defaults (textured transport, --cubemap-bounces 5).
inline CubemapBakeConfigHashInputs CanonicalCubemapCfgHash(
	const SceneExportSettings& es) {
	CubemapBakeConfigHashInputs c{};
	c.size = es.cubemapSize;
	c.rays = es.cubemapRays;
	c.maskTransmission = es.maskTransmission;
	c.maxBounces = 5;
	c.albedoUniform = false; // captures bake textured (the cook's design)
	c.albedoValue = glm::vec3(0.7f);
	return c;
}

} // namespace smvk::gi
