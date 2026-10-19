// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Cook-time meshlet path (meshoptimizer). Cooked once in the pak cooker from
// the optimized `StaticMeshUpload`, persisted in the Mesh blob, loaded into
// `StaticMesh::meshlets` at runtime. Nothing in the renderer consumes this
// yet — it is the data foundation for a future indirect/mesh-shader pass.
//
// Layout mirrors meshoptimizer: each meshlet references `vertexCount`
// entries in `vertices` (global vertex ids) starting at `vertexOffset`, and
// `triangleCount * 3` bytes in `triangles` (local ids into the meshlet's
// vertex window) starting at `triangleOffset`.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace smvk {

struct StaticMeshUpload;

// THE meshlet size limits (kMax constants — the one place the number lives).
// 64v/64t is an ENGINE invariant, not a tunable: one mesh-shader workgroup
// (64 threads, <= 192 = 64*3 verts — see gfxmesh.hlsli GFX_MESH_MAX_TRIS/
// GFX_MESH_MAX_VERTS) covers a WHOLE meshlet with no dedup hash, and the
// meshlet-cluster HZB cull (gfxfilter) assumes that coverage. Everything
// defaults to these (MeshletConfig, TourCook, the cooker CLI) and
// BuildMeshletsForUpload CLAMPS over-limit configs (stale authored cooks
// carrying e.g. meshletTris 124) down to them — a bigger meshlet than the
// workgroup can draw would drop geometry.
inline constexpr uint32_t kMeshletMaxVertices = 64;
inline constexpr uint32_t kMeshletMaxTriangles = 64;

struct MeshletConfig {
	bool enabled = false; // off by default; cooker opts in via CLI
	uint32_t maxVertices = kMeshletMaxVertices;
	uint32_t maxTriangles = kMeshletMaxTriangles;
	// Normal-cone pressure for meshopt's partition scoring. 0 (meshopt's
	// default, the canon): the facing-split repair (BuildMeshletsForUpload)
	// owns cone quality — the cone weight proved unable to stop meshopt's
	// layer mixing (its vertex-budget priority outranks the cone score,
	// and the kd-tree fallback ignores facing entirely; the knight stayed
	// ~97% degenerate at 0 / 0.5 / 1.0).
	float coneWeight = 0.0f;
};

struct MeshletStats {
	uint32_t meshletCount = 0;
	uint32_t totalTriangles = 0;
	uint32_t maxVertsUsed = 0;
	uint32_t maxTrisUsed = 0;
	// Shape-quality dashboard (the "are these meshlets well-formed" print):
	// radius is MESH-LOCAL model units; the cone cutoff is meshopt's
	// cos(half-angle) — 1 = razor-tight planar cone (best culling),
	// <=0 = hemisphere-or-worse (the cone test never fires), and a
	// degenerate cone (zero axis) keeps unconditionally (GfxConeCulled's
	// len2 < 1e-12). Ill-formed sets: big mean/max radius + mean cutoff
	// near 0 (meshlets straddling disjoint body parts). splitMeshlets =
	// how many were facing-split (the degenerate-cone repair; see
	// BuildMeshletsForUpload's block comment).
	float maxRadius = 0.0f;
	float meanRadius = 0.0f;
	float meanCutoff = 0.0f;
	uint32_t degenerateCones = 0;
	uint32_t splitMeshlets = 0;
};

struct Meshlet {
	uint32_t vertexOffset = 0;
	uint32_t vertexCount = 0;
	uint32_t triangleOffset = 0; // byte offset into CookedMeshlets::triangles
	uint32_t triangleCount = 0;

	glm::vec3 boundCenter{ 0.0f };
	float boundRadius = 0.0f;
	glm::vec3 coneApex{ 0.0f };
	glm::vec3 coneAxis{ 0.0f, 1.0f, 0.0f };
	float coneCutoff = 1.0f;
};

struct CookedMeshlets {
	std::vector<Meshlet> meshlets;
	std::vector<uint32_t> vertices; // global vertex ids
	std::vector<uint8_t> triangles; // 3 bytes per triangle (local ids)

	bool empty() const { return meshlets.empty(); }
	size_t byteSize() const;
};

// Builds meshlets from an (already vertex-cache-optimized) upload.
// Returns false on degenerate input / disabled config / builder failure,
// leaving `out` empty. `err` describes the failure when non-null.
bool BuildMeshletsForUpload(
	const StaticMeshUpload& upload,
	const MeshletConfig& config,
	CookedMeshlets* out,
	MeshletStats* outStats = nullptr,
	std::string* err = nullptr);

// ---------------------------------------------------------------------------
// Indirect-draw staging (CPU only, no renderer wiring yet).
//
// Expands each meshlet into global-vertex-id triangles appended to
// `outIndices`, and emits one DrawIndexedIndirect-style command per meshlet
// over that buffer. A future pass can upload `outIndices` once and issue the
// commands via indirect draws (one per meshlet, or multi-draw indirect).
// ---------------------------------------------------------------------------

struct MeshletDrawCommand {
	uint32_t indexCount = 0;
	uint32_t instanceCount = 1;
	uint32_t firstIndex = 0;
	int32_t baseVertex = 0; // always 0: indices are global vertex ids
	uint32_t meshletIndex = 0;
};

void BuildMeshletIndirectCommands(
	const CookedMeshlets& cooked,
	std::vector<uint32_t>& outIndices,
	std::vector<MeshletDrawCommand>& outCommands);

} // namespace smvk
