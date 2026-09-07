#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

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

	// LOD instrumentation (UNPLUGGED — see LodInfo below).
	bool recordLodCandidates = true;

	// Cooked LOD chain (meshopt_simplify, cook-time only). When true,
	// the importer emits sibling blobs "<base>/lodN" (N >= 1) sharing the
	// base groupId; the scene keeps referencing LOD0. The renderer still
	// ignores lodIndex/screenThreshold (selection hook unplugged).
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
// The renderer MUST NOT branch on lodIndex / screenThreshold yet — the
// selection hook is intentionally unplugged (no runtime LOD switching).
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
// meshopt_simplify (positions only) + vertex-cache/fetch cleanup. Each
// lodRatios entry is attempted in order against LOD0 with lodTargetError;
// the result error (relative) is converted to a local absolute error via
// meshopt_simplifyScale. Candidates are FILTERED by that error + reduction:
// dropped when broken (0 indices, non-triangle count, NaN/inf error),
// tiny (< lodMinTriangles tris), or not coarser enough (< lodMinReduction
// fewer tris than the last kept LOD). Returns true when the input was valid
// (out may still be empty when every candidate was filtered); false on
// degenerate input. Never touches the LOD0 upload.
bool BuildLodChainForUpload(
	const StaticMeshUpload& lod0,
	const MeshOptConfig& config,
	std::vector<CookedLod>& out);

// Max world scale of a node matrix: max length of the three basis columns.
// Matches gltfpack's getScale() (max abs scale axis); conservative for
// non-uniform scale — world error = local error * this.
float MaxScaleOfMatrix(const glm::mat4& m);

// Sibling URI helpers: "<base>/lodN" for N >= 1; base extraction strips
// that suffix (returns the input unchanged when it is not a LOD URI).
std::string LodSiblingUri(const std::string& baseUri, uint32_t lodIndex);
std::string BaseMeshUriForLod(const std::string& uri);

// Cook-time post-pass: after the scene's global transforms are final,
// scales every LOD sibling's locally-recorded simplifyError by the max
// world scale over all nodes instancing its base mesh (meshes shared by
// several nodes use the max — conservative, mirrors gltfpack quality).
// LOD0 entries keep simplifyError == 0. Also stamps candidateCount on the
// whole chain (kept size incl. LOD0). No-op when no LODs were cooked.
void FinalizeLodWorldErrors(Cluster& cluster);

} // namespace smvk
