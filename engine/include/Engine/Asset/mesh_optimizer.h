// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Memory/arena.h>
#include <Engine/Memory/string_view.h>
#include <Engine/Renderer/mesh.h>

namespace smvk {

struct Cluster;
struct MeshletConfig;

// ---------------------------------------------------------------------------
// Lightmap chart-seam protection during sibling simplification (see
// CookLodSiblings / BuildLodChainForUpload). Chart seams are POSITION-
// IDENTICAL vertex duplicates (the bake's per-corner stamping) differing
// only in texCoord2/chart, so a collapse across the seam has ZERO
// position error — protection has to come from elsewhere.
//
//   Attribute (default): the chart id + atlas uv2 ride
//   meshopt_simplifyWithAttributes as error terms; the chart term is the
//   only rejector for the twin collapse (weight 1.0 puts a one-chart
//   delta far over the squared error budget). Seam vertices still
//   collapse ALONG the seam (meshopt Kind_Seam rules forbid drifting
//   into manifold vertices, so no cracks).
//
//   Hard: every vertex with a position twin under a DIFFERENT chart is
//   passed via vertex_lock — cross-seam collapses become impossible, at
//   the cost of locked vertices (less reduction).
//
// NOTE: meshopt_SimplifyLockBorder is useless here: chart seams classify
// as Kind_Seam (collapsible), not Kind_Border (locked).
// ---------------------------------------------------------------------------
enum class LodSeamLock {
	Attribute,
	Hard,
};


// ---------------------------------------------------------------------------
// Cook-time mesh optimization (meshoptimizer). Runs offline in the pak
// cooker, never on the runtime load path.
// ---------------------------------------------------------------------------

struct MeshOptConfig {
	bool enabled = true;
	bool weld = true;        // vertex dedup (generateVertexRemap)
	bool vertexCache = true; // optimizeVertexCache
	bool overdraw = true;    // optimizeOverdraw (auto-skipped for Blend/Mask)
	bool vertexFetch = true; // optimizeVertexFetch (runs last)
	float overdrawThreshold = 1.05f;

	// LOD instrumentation (see LodInfo below).
	bool recordLodCandidates = true;

	// Cooked LOD chain (meshopt_simplify, cook-time only). When true,
	// the importer emits sibling blobs "<base>/lodN" (N >= 1) sharing the
	// base groupId; the scene keeps referencing LOD0. The renderer selects
	// siblings at draw time from LodInfo::simplifyError (see below).
	bool generateLods = false;
	// Triangle-count ratios vs LOD0, coarsest-last. Default
	// 75/50/30/15%: gentle steps + one extra level (the runtime pick is
	// ERROR-driven per pixel budget, so the chain wants many fine
	// siblings, not a few coarse jumps — the old 60/30/12 steps lost
	// visible ornament detail one full level early).
	std::vector<float> lodRatios{ 0.75f, 0.5f, 0.3f, 0.15f };
	// Relative error budget per meshopt_simplify call (fraction of extents,
	// [0..1]). 1%: the recorded sibling error feeds the runtime's projected
	// pixel-error pick, so a 2% budget let siblings drift two pixels of
	// shape before the picker ever got a say — tightened to 1% (the ratio
	// chain stays the binding constraint for reduction).
	float lodTargetError = 0.01f;
	// Bitmask of meshopt_SimplifyX options (0 = safe default).
	unsigned lodSimplifyOptions = 0;
	// Error-driven filter ("drop tiny + broken"): a candidate is kept only
	// when it has >= lodMinTriangles tris, finite error, and reduces the
	// triangle count vs the last kept LOD by >= lodMinReduction.
	uint32_t lodMinTriangles = 8;
	float lodMinReduction = 0.10f;
	// Chart-seam protection for lightmapped sibling simplify (see
	// LodSeamLock above; only consulted when a lightmap chart table is
	// passed to BuildLodChainForUpload).
	LodSeamLock lodSeamLock = LodSeamLock::Attribute;
	// Albedo-UV awareness for the default sibling path (see
	// BuildLodChainForUpload). Carrying texCoord in the error metric
	// protects UV-seam topology (split verts at identical positions, where
	// the field genuinely disagrees) and penalizes parameterization
	// distortion there; smooth mappings are largely unaffected by design
	// (the metric preserves the attribute function). Untextured meshes
	// (zero UVs) are unaffected. Set false to force positions-only.
	bool lodUvAware = true;
};

struct MeshOptStats {
	uint32_t inVertices = 0;
	uint32_t outVertices = 0;
	uint32_t triangles = 0;
	float acmrBefore = 0.0f; // average cache miss ratio, worst case 3.0
	float acmrAfter = 0.0f;
	float overdrawBefore = 0.0f; // shaded/covered px, best case 1.0
	float overdrawAfter = 0.0f;
	uint32_t lodGroupId = 0;
};

// ---------------------------------------------------------------------------
// Per-asset LOD marker. Stored in every MeshBlob and exposed via
// World::GetLodInfo() for tooling/ImGui only.
//
// LOD0 is the full (optimized) mesh referenced by the scene; LOD1+ are
// cooked siblings "<baseUri>/lodN" sharing the base groupId. simplifyError
// is the WORLD-space absolute geometric error in the same units as vertex
// positions (local meshoptimizer error scaled by meshopt_simplifyScale and
// by the max node world scale of all instances — see FinalizeLodWorldErrors).
// The renderer selects a LOD per instance from simplifyError via
// LodProjectedPixelError() below (screen-space error budget); lodIndex /
// screenThreshold stay as informational markers (screenThreshold reserved).
// ---------------------------------------------------------------------------

struct LodInfo {
	uint32_t groupId = 0;        // source mesh index (LOD grouping key)
	uint32_t lodIndex = 0;       // 0 = full mesh, N >= 1 = cooked siblings
	uint32_t candidateCount = 0; // kept chain size incl. LOD0 (siblings agree)
	float simplifyError = 0.0f;  // world-space absolute error (0 for LOD0)
	float screenThreshold = -1.0f; // reserved selection hook, renderer ignores
	uint32_t triCount = 0;
	float acmr = 0.0f;
};

// One kept LOD candidate: compacted upload + error metrics. localError is
// in mesh-local units (relativeError * meshopt_simplifyScale); the caller
// scales it by node world scale to obtain LodInfo::simplifyError.
struct CookedLod {
	StaticMeshUpload upload;
	float relativeError = 0.0f;
	float localError = 0.0f;
	uint32_t triCount = 0;
};

// ---------------------------------------------------------------------------
// Tangent-frame synthesis (shared by the importer and the LOD cooker).
//
// meshopt_simplify only tracks positions, so decimated vertices keep the
// pre-simplification frame: interpolating those normals/tangents across
// the new, larger triangles shades wrongly (even back-facing -> black).
// ComputeSmoothNormals re-derives area-weighted smooth normals from the
// upload's own indices; GenerateTangents re-derives UV-gradient tangents
// (Lengyel) Gram-Schmidt orthogonalized against N with a handedness sign.
// Creases survive because crease vertices are duplicated. Call
// ComputeSmoothNormals first (GenerateTangents consumes its normals).
// Inversion repair: a smoothed normal that disagrees with its biggest
// contributor (dot <= 0 — cancellation across opposing faces on thin
// decimated topology) falls back to that dominant face normal, so every
// repaired vertex agrees with at least one real triangle (double-sided
// materials shade either side; single-sided ones match a true face
// instead of pointing backwards into black).
// ---------------------------------------------------------------------------
void ComputeSmoothNormals(StaticMeshUpload& upload);
void GenerateTangents(StaticMeshUpload& upload);

// Optimizes upload in place. The caller gates config.overdraw for surfaces
// that need it (the importer disables overdraw for Blend/Mask materials:
// overdraw optimization can hurt cutout rendering). lodGroupId stamps stats
// + outLod for the MeshBlob. Returns false when the input is degenerate
// (0 verts / <3 indices / non-triangle count) and leaves the upload
// untouched (still fills outStats/outLod markers).
bool OptimizeStaticMeshUpload(
	StaticMeshUpload& upload,
	const MeshOptConfig& config,
	uint32_t lodGroupId,
	MeshOptStats* outStats = nullptr,
	LodInfo* outLod = nullptr);

// Builds the cooked LOD chain for an already-optimized LOD0 upload via
// meshopt_simplify (+ vertex-cache/fetch cleanup). Each lodRatios entry is
// attempted in order against LOD0 with lodTargetError; the result error
// (relative) is converted to a local absolute error via
// meshopt_simplifyScale. Candidates are FILTERED by that error + reduction:
// dropped when broken (0 indices, non-triangle count, NaN/inf error),
// tiny (< lodMinTriangles tris), or not coarser enough (< lodMinReduction
// fewer tris than the last kept LOD). Returns true when the input was valid
// (out may still be empty when every candidate was filtered); false on
// degenerate input. Never touches the LOD0 upload.
//
// preserveCoverage must be true for alpha-tested (MASK) / blended cutout
// foliage: the simplifier then uses meshopt_simplifyWithAttributes on the
// UVs plus meshopt_SimplifyLockBorder so quad borders can't collapse and
// UV distortion (which drives the alpha-test) is part of the error metric.
// Positions-only simplify destroys cutout silhouettes: borders shrink and
// stretched UVs flip the alpha test, flashing background/sky through the
// leaves on every LOD switch.
//
// lmVertexChart (optional, parallel to lod0.vertices; 0xFFFFFFFF =
// uncharted) enables the lightmap-aware path for STAMPED meshes (the
// bake ran before sibling generation): texCoord2 rides the vertex remap
// for free — surviving vertices keep exact LOD0 atlas UVs — and the
// chart id + uv2 enter the error metric so collapses stay inside one
// lightmap chart (see LodSeamLock). Mutually exclusive with
// preserveCoverage in practice (Blend/MASK meshes are never lightmapped;
// coverage wins if both are set).
//
 // Otherwise — the common case, including the frostbite pre-bake cook where
 // lightmap charts don't exist yet — the albedo texCoord rides the error
 // metric when MeshOptConfig::lodUvAware (default true), protecting UV-seam
 // topology and biasing collapses toward UV-preserving ones. Untextured
 // (zero-UV) meshes are unaffected.
bool BuildLodChainForUpload(
	const StaticMeshUpload& lod0,
	const MeshOptConfig& config,
	std::vector<CookedLod>& out,
	bool preserveCoverage = false,
	const uint32_t* lmVertexChart = nullptr);

// Max world scale of a node matrix: max length of the three basis columns.
// Matches gltfpack's getScale() (max abs scale axis); conservative for
// non-uniform scale — world error = local error * this.
float MaxScaleOfMatrix(const glm::mat4& m);

// Sibling URI helpers: "<base>/lodN" for N >= 1 (composed into `scratch`,
// default the frame arena — never stored); base extraction strips that
// suffix as a pure view slice (returns the input unchanged otherwise).
[[nodiscard]] StringViewT LodSiblingUri(StringViewT baseUri, uint32_t lodIndex,
	Arena& scratch = FrameArena());
[[nodiscard]] StringViewT BaseMeshUriForLod(StringViewT uri);

// ---------------------------------------------------------------------------
// Error-driven LOD transitions (runtime selection from the simplifier error).
//
// meshopt_simplify reports a *geometric* error in world units
// (LodInfo::simplifyError). A world-space deviation `err` at view distance
// `d` projects to roughly
//
//   pixels = err * (viewportHeightPx / (2 * tan(fovY/2))) / d
//
// (perspective projection of a view-perpendicular segment; matches gltfpack
// screen-error semantics). The renderer picks the coarsest LOD whose
// projected error stays within a pixel budget (typically ~1px), so
// transitions are driven directly by the simplifier's error rate — no baked
// distance thresholds, no per-mesh tuning.
//
// Helpers are pure/free so cook tooling, tests and the renderer share one
// formula.
// ---------------------------------------------------------------------------

// Projected screen-space error in pixels for a world-space geometric error.
inline float LodProjectedPixelError(float simplifyErrorWorld, float distance,
	float viewportHeightPx, float fovYRadians) {
	// LOD0 (zero error) never projects; degenerate views never select.
	if (!(simplifyErrorWorld > 0.0f))
		return 0.0f;
	if (!(distance > 0.0f) || !(viewportHeightPx > 0.0f) || !(fovYRadians > 0.0f))
		return 0.0f;
	const float half = fovYRadians * 0.5f;
	const float t = tanf(half);
	if (!(t > 0.0f))
		return 0.0f;
	return simplifyErrorWorld * viewportHeightPx / (2.0f * t * distance);
}

// View distance at which `simplifyErrorWorld` projects to exactly
// `maxPixelError`. Beyond this distance the LOD is within budget.
inline float LodSwitchDistance(float simplifyErrorWorld, float viewportHeightPx,
	float fovYRadians, float maxPixelError) {
	if (!(simplifyErrorWorld > 0.0f) || !(maxPixelError > 0.0f))
		return 0.0f;
	if (!(viewportHeightPx > 0.0f) || !(fovYRadians > 0.0f))
		return 0.0f;
	const float half = fovYRadians * 0.5f;
	const float t = tanf(half);
	if (!(t > 0.0f))
		return 0.0f;
	return simplifyErrorWorld * viewportHeightPx / (2.0f * t * maxPixelError);
}

// Hysteresis band around the pixel budget for LOD switching: coarsen only
// onto a level whose projected error is under budget*(1-h), refine only off
// a current level whose error is over budget*(1+h). Without the band, a
// node sitting at a switch distance flips between siblings on every
// refresh (visible flicker while the camera moves — worst on cutout
// foliage, whose coarse levels lose alpha coverage). 0 = plain threshold.
inline constexpr float kLodSelectHysteresis = 0.2f;

// Pick the coarsest LOD whose projected error is within budget.
// `errorsByLod` is ordered LOD0..N (LOD0 error must be 0); entries with
// non-finite/negative error are treated as unusable (skipped, never picked
// beyond the last good one). Returns 0 when nothing coarser is in budget.
// currentLod + hysteresis add a sticky band anchored on the caller's live
// level: inside the band the current level is served; coarsening needs the
// tight budget, refining needs the loose one. hysteresis = 0 (any
// currentLod) is bit-identical to the stateless threshold walk, so cook
// tooling and one-shot queries keep the old semantics by default.
inline uint32_t SelectLodIndexForDistance(const float* errorsByLod, uint32_t count,
	float distance, float viewportHeightPx, float fovYRadians, float maxPixelError,
	uint32_t currentLod = 0, float hysteresis = 0.0f) {
	if (!errorsByLod || count == 0)
		return 0;
	if (!(distance > 0.0f) || !(maxPixelError >= 0.0f))
		return 0;
	if (currentLod >= count)
		currentLod = 0; // stale level beyond the chain (post-edit): fresh pick
	const float tightBudget = maxPixelError * (1.0f - hysteresis);
	const float looseBudget = maxPixelError * (1.0f + hysteresis);
	uint32_t tight = 0; // last level safely inside the coarsen budget
	uint32_t loose = 0; // last level inside the refine budget
	for (uint32_t i = 1; i < count; ++i) {
		const float e = errorsByLod[i];
		if (!(e >= 0.0f) || !std::isfinite(e))
			break; // broken chain: stop at the last good LOD
		if (e <= 0.0f) {
			tight = i; // zero-error sibling: always safe, keep going coarser
			loose = i;
			continue;
		}
		const float px = LodProjectedPixelError(e, distance, viewportHeightPx, fovYRadians);
		if (px <= tightBudget) {
			tight = i;
			loose = i;
		}
		else if (px <= looseBudget) {
			loose = i;
		}
		else
			break; // errors grow coarser; further LODs only project larger
	}
	if (loose < currentLod)
		return loose;   // current clearly out of budget: refine off it
	if (tight > currentLod)
		return tight;   // a strictly safer coarse level exists: coarsen onto it
	return currentLod; // inside the hysteresis band (or no move): sticky
}

// Cook-time post-pass: after the scene's global transforms are final,
// scales every LOD sibling's locally-recorded simplifyError by the max
// world scale over all nodes instancing its base mesh (meshes shared by
// several nodes use the max — conservative, mirrors gltfpack quality).
// LOD0 entries keep simplifyError == 0. Also stamps candidateCount on the
// whole chain (kept size incl. LOD0). No-op when no LODs were cooked.
// Idempotent: re-running divides out the previously applied scale first
// (tracked per sibling on the Cluster, cook-time only), so pipelines that
// finalize more than once keep single-scaled errors.
void FinalizeLodWorldErrors(Cluster& cluster);

// ---------------------------------------------------------------------------
// Post-bake LOD sibling cooking (PakCooker path; never on the runtime
// load path). The importer's in-pass chain generation runs BEFORE the
// lightmap bake, so siblings would simplify from UNSTAMPED uploads
// (garbage chart uvs, no chart knowledge). CookLodSiblings runs AFTER
// the bake instead: lightmapped meshes simplify from their stamped
// uploads, siblings share LOD0's chart list + atlas transforms, and a
// per-level safety audit quantifies how far the simplified
// parameterization drifts from the LOD0 surface (see AuditLodLightmap).
// ---------------------------------------------------------------------------

struct CookLodSiblingsOptions {
	// Lightmap provenance from a completed bake: mesh pool index ->
	// per-vertex chart id over the STAMPED retained upload (0xFFFFFFFF =
	// uncharted). Meshes without an entry (or without a valid lmChunk)
	// take the positions-only path. Null/empty = no lightmaps at all.
	const std::unordered_map<uint32_t, std::vector<uint32_t>>* vertexCharts
		= nullptr;
	// Per-level lightmap-safety audit for lightmapped meshes (log-only
	// unless auditGate). Requires non-zero atlas dims.
	bool audit = false;
	uint32_t atlasWidth = 0;
	uint32_t atlasHeight = 0;
	// Texel-error threshold for the over-threshold counter (and the gate).
	float auditThresholdTexels = 2.0f;
	// false (default) = audit failures only log; true = the chain is
	// truncated at the first failing level (coarser levels only
	// interpolate more, so they cannot recover).
	bool auditGate = false;
	// Per-level triangle subsample cap for the projection audit.
	uint32_t auditMaxTris = 1024;
};

// Generates + registers the cooked LOD siblings ("<base>/lodN") for every
// node-referenced base mesh with a retained upload (mirrors the
// importer's sibling registration so World::RebuildLodChains resolves
// them identically), then FinalizeLodWorldErrors. Idempotent: meshes that
// already carry siblings are skipped. Returns false only on invalid
// input arguments (individual mesh failures are skipped with a log).
// Prints per-mesh chain lines + per-level audit lines to stdout.
bool CookLodSiblings(Cluster& cluster, const MeshOptConfig& config,
	const MeshletConfig& meshletConfig,
	const CookLodSiblingsOptions& options = {});

// ---------------------------------------------------------------------------
// Lightmap-safety audit: would this simplification visibly damage the
// baked lighting? For sampled interior points of the sibling's
// triangles, compares the barycentrically interpolated (candidate)
// atlas uv against the TRUE LOD0-surface atlas uv obtained by projecting
// the point onto a world-space BVH of LOD0 (normal rays both directions,
// scored by distance + normal similarity; closest-point fallback via the
// BVH). Both uploads carry chart-local uvs + chart ids (resolved here).
// Reports the error in atlas TEXELS, plus a full-mesh cross-chart sweep
// count (triangles whose corners disagree on chart id or whose atlas
// corners span more than one chart rect — the catastrophic atlas-smear
// case no texel number can describe).
// ---------------------------------------------------------------------------

// Cross-chart uv2 span threshold (max |du|+|dv| over corner pairs).
// Within one 256-texel chart of a ~2k atlas a triangle spans at most
// ~0.42; 0.45 flags only genuine cross-rect sweeps (same math as the
// pak dump's uv2span detector).
inline constexpr float kLodCrossChartUvSpan = 0.45f;

struct LodLightmapAuditReport {
	uint32_t trisTotal = 0;     // sibling triangles
	uint32_t trisAudited = 0;   // sampled by the projection audit
	uint32_t samples = 0;       // resolved interior samples
	uint32_t unresolved = 0;    // projection failed (counted, not scored)
	float maxTexels = 0.0f;
	float meanTexels = 0.0f;
	float p95Texels = 0.0f;
	uint32_t overThreshold = 0; // resolved samples over thresholdTexels
	uint32_t crossChartTris = 0; // chart-id mismatch or atlas span > kLodCrossChartUvSpan (all tris)
};

// `world` places both uploads in the same space (siblings share LOD0's
// node transform). `chartXforms` is LOD0's per-chart atlas transforms
// (parallel to chart ids); both uploads carry CHART-LOCAL uvs + chart
// ids, resolved to atlas space here with the same math as the VS
// unpack. `searchDist` is the normal-ray reach (caller: ~4x the level's
// world error with an extent floor; the closest-point fallback is exact
// regardless). `maxTris` caps the audited triangles via a deterministic
// stride (cross-chart counting always covers all tris).
// Returns false on degenerate input (report zeroed).
bool AuditLodLightmap(const StaticMeshUpload& lod0,
	const StaticMeshUpload& sibling, const glm::mat4& world,
	const std::vector<glm::vec4>& chartXforms,
	uint32_t atlasWidth, uint32_t atlasHeight,
	float thresholdTexels, float searchDist, uint32_t maxTris,
	LodLightmapAuditReport& out);

} // namespace smvk
