#pragma once

#include <cstdint>
#include <string>

#include <Engine/Renderer/mesh.h>

namespace smvk {

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
	bool generateLodMeshes = false; // forced false in v1; no LOD1+ output
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
// v1 policy: the cooker writes lodIndex = 0 and never emits LOD1+ geometry.
// recordLodCandidates fills the measurement fields (triCount, acmr,
// simplifyError estimate) so a future LOD policy can be designed from real
// cooker logs without changing the format. The renderer MUST NOT branch on
// lodIndex / screenThreshold yet — the selection hook is intentionally
// unplugged (no runtime LOD switching, no meshopt_simplify output).
// ---------------------------------------------------------------------------

struct LodInfo {
	uint32_t groupId = 0;        // source mesh index (future LOD grouping key)
	uint32_t lodIndex = 0;       // always 0 in v1
	uint32_t candidateCount = 0; // hypothetical LOD count considered (info only)
	float simplifyError = 0.0f;  // measured/estimated geometric error (info only)
	float screenThreshold = -1.0f; // reserved selection hook, renderer ignores
	uint32_t triCount = 0;
	float acmr = 0.0f;
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

} // namespace smvk
