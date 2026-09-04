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

struct MeshletConfig {
	bool enabled = false; // off by default; cooker opts in via CLI
	uint32_t maxVertices = 64;
	uint32_t maxTriangles = 124;
	float coneWeight = 0.0f;
};

struct MeshletStats {
	uint32_t meshletCount = 0;
	uint32_t totalTriangles = 0;
	uint32_t maxVertsUsed = 0;
	uint32_t maxTrisUsed = 0;
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
