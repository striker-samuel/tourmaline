#pragma once
// Procedural terrain: analytic heightfield shared by the placement Terrain
// domain, the editor Create > Terrain flow, and the cooker (all three
// consume identical samples — preview, viewport mesh and baked mesh
// agree by construction).
//
// Convention: heights are sampled at ABSOLUTE world XZ (origin + local),
// while mesh vertices are emitted LOCAL ([0,size] on x/z). The scene node
// transform carries the origin (translate(ox, 0, oz)), so world space
// matches the placement domain exactly.

#include <cstdint>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_domain.h>
#include <Engine/Renderer/mesh.h>

namespace smvk::placement {

struct TerrainParams {
	glm::vec2 origin{ 0.0f, 0.0f }; // world XZ of texel (0,0)
	float size = 64.0f;             // square extent in meters
	uint32_t res = 64;              // samples per side (clamped 2..512)
	float height = 8.0f;            // max hill height in meters
	float freq = 0.02f;             // noise frequency (1/m)
	uint64_t seed = 1;              // noise seed
};

// Builds the heightfield described by params. Deterministic: same params
// -> identical samples (two value-noise octaves, output in [0, height]).
HeightfieldData BuildTerrainHeights(const TerrainParams& params);

// Builds a renderable grid mesh from the same field: local-space verts
// ([0,size] x/z, y up), smooth normals, planar 0..1 UVs, one indexed
// surface, valid AABB. Returns false when res is degenerate.
bool BuildTerrainUpload(const TerrainParams& params, StaticMeshUpload& out);

} // namespace smvk::placement
