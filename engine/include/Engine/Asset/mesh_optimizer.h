#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Memory/arena.h>
#include <Engine/Memory/string_view.h>
#include <Engine/Renderer/mesh.h>

namespace smvk {

struct Cluster;

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
	// Triangle-count ratios vs LOD0, coarsest-last. Default 60/30/12%.
	std::vector<float> lodRatios{ 0.6f, 0.3f, 0.12f };
	// Relative error budget per meshopt_simplify call (fraction of extents,
	// [0..1]). Generous on purpose: the ratio is the binding constraint,
	// the *result* error is recorded and used to filter/select.
	float lodTargetError = 0.02f;
	// Bitmask of meshopt_SimplifyX options (0 = safe default).
	unsigned lodSimplifyOptions = 0;
	// Error-driven filter ("drop tiny + broken"): a candidate is kept only
	// when it has >= lodMinTriangles tris, finite error, and reduces the
	// triangle count vs the last kept LOD by >= lodMinReduction.
	uint32_t lodMinTriangles = 8;
	float lodMinReduction = 0.10f;
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
bool BuildLodChainForUpload(
	const StaticMeshUpload& lod0,
	const MeshOptConfig& config,
	std::vector<CookedLod>& out,
	bool preserveCoverage = false);

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

// Pick the coarsest LOD whose projected error is within budget.
// `errorsByLod` is ordered LOD0..N (LOD0 error must be 0); entries with
// non-finite/negative error are treated as unusable (skipped, never picked
// beyond the last good one). Returns 0 when nothing coarser is in budget.
inline uint32_t SelectLodIndexForDistance(const float* errorsByLod, uint32_t count,
	float distance, float viewportHeightPx, float fovYRadians, float maxPixelError) {
	if (!errorsByLod || count == 0)
		return 0;
	if (!(distance > 0.0f) || !(maxPixelError >= 0.0f))
		return 0;
	uint32_t best = 0;
	for (uint32_t i = 1; i < count; ++i) {
		const float e = errorsByLod[i];
		if (!(e >= 0.0f) || !std::isfinite(e))
			break; // broken chain: stop at the last good LOD
		if (e <= 0.0f) {
			best = i; // zero-error sibling: always safe, keep going coarser
			continue;
		}
		const float px = LodProjectedPixelError(e, distance, viewportHeightPx, fovYRadians);
		if (px <= maxPixelError)
			best = i;
		else
			break; // errors grow coarser; further LODs only project larger
	}
	return (best < count) ? best : count - 1;
}

// Cook-time post-pass: after the scene's global transforms are final,
// scales every LOD sibling's locally-recorded simplifyError by the max
// world scale over all nodes instancing its base mesh (meshes shared by
// several nodes use the max — conservative, mirrors gltfpack quality).
// LOD0 entries keep simplifyError == 0. Also stamps candidateCount on the
// whole chain (kept size incl. LOD0). No-op when no LODs were cooked.
void FinalizeLodWorldErrors(Cluster& cluster);

} // namespace smvk
