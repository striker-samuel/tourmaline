#pragma once
// PlacementGraph domains: Grid + MeshSurface + Heightfield (MVP set).
// Each domain exposes deterministic sampling from (StableID, candidateIndex).
// CPU here is the reference oracle; the GPU kernels in Phase 3 mirror it
// (same hashes, same alias-walk, same bilinear filtering).

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_ids.h>

namespace smvk::placement {

enum class DomainKind : uint32_t { Grid = 0, MeshSurface = 1, Heightfield = 2 };

struct DomainSample {
	glm::vec3 p{ 0.0f };    // world position
	glm::vec3 n{ 0, 1, 0 }; // shading normal
	glm::vec2 uv{ 0.0f };
	uint32_t primitiveId = 0;
};

// ---- Grid domain ----------------------------------------------------
// Deterministic jittered lattice over an XZ rect at fixed height y.
// candidateIndex maps to cells row-major; jitter from POSITION channel
// (Random02Of, so x/z uncorrelated without spending extra channels).
struct GridDomain {
	glm::vec3 origin{ 0.0f }; // min corner (x, y, z); y = base height
	glm::vec2 extent{ 64.0f, 64.0f }; // size in x/z meters
	uint32_t cellsX = 8;
	uint32_t cellsZ = 8;

	uint32_t cellCount() const { return cellsX * cellsZ; }
	DomainSample sample(StableID id, uint32_t candidateIndex) const;
};

// ---- Mesh surface domain --------------------------------------------
// Area-weighted triangle sampling via Walker's alias method.
// Cook builds tables once (pak_cooker); runtime samples O(1).
struct PlacementMeshData {
	std::vector<glm::vec3> positions; // per-vertex world (or local + transform outside)
	std::vector<glm::vec3> normals;
	std::vector<glm::vec2> uvs;
	std::vector<uint32_t> indices;    // 3 per triangle
	// Alias table, one entry per triangle:
	std::vector<float> aliasProb;     // probability of keeping column triangle
	std::vector<uint32_t> aliasIndex; // fallback triangle when rejected

	size_t triangleCount() const { return indices.size() / 3; }
	bool hasAlias() const { return !aliasProb.empty() && aliasProb.size() == triangleCount(); }
};

float TriangleArea(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);

// Builds aliasProb/aliasIndex from current positions+indices.
// Degenerate/zero-area triangles get prob 0 and are never picked
// (unless the whole mesh is degenerate, then uniform fallback).
void BuildMeshAliasTable(PlacementMeshData& mesh);

// Area-weighted pick + barycentric point. r1 selects the column,
// r2 decides alias keep/fallback, (bx,by) place inside the triangle
// (all derived from the candidate's POSITION channel on GPU).
DomainSample SampleMeshSurface(const PlacementMeshData& mesh,
	float r1, float r2, float bx, float by);

// Convenience: derive r1/r2/bx/by from a StableID (CPU oracle path).
DomainSample SampleMeshSurface(const PlacementMeshData& mesh, StableID id);

// ---- Heightfield domain ---------------------------------------------
// Minimal heightmap: regular XZ grid, bilinear height + central-difference
// normal. GPU samples the height texture with the same math.
struct HeightfieldData {
	uint32_t width = 0;   // samples in x
	uint32_t height = 0;  // samples in z
	glm::vec3 origin{ 0.0f };  // world pos of texel (0,0)
	glm::vec2 spacing{ 1.0f, 1.0f }; // meters per texel in x/z
	std::vector<float> heights; // width*height, row-major (z*width+x)

	bool valid() const { return width >= 2 && height >= 2 && heights.size() == (size_t)width * height; }
	float sampleHeight(float x, float z) const; // bilinear, world XZ
	glm::vec3 sampleNormal(float x, float z) const;
};

struct HeightfieldDomain {
	const HeightfieldData* field = nullptr; // not owned
	glm::vec2 extentMin{ 0.0f };  // scatter rect in world XZ
	glm::vec2 extentMax{ 64.0f, 64.0f };

	// Uniform XZ pick from POSITION channel + height/normal lookup.
	DomainSample sample(StableID id) const;
};

} // namespace smvk::placement
