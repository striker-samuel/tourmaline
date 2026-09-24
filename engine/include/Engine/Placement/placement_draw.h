#pragma once
// Instanced draw feed: turns resident tiles into per-(tile, prototype)
// runs for DrawIndexedIndirect, with zero per-instance CPU work per frame.
//
// Contract with the GPU (see placement_instanced.vert.hlsl):
//   - tile instances are SORTED by (prototypeId, StableID) at generation,
//     so each prototype's instances form one contiguous run;
//   - the indirect command's firstInstance = run start in the tile's
//     instance buffer (SV_InstanceID already includes it under SPIR-V);
//   - per-instance frustum/alive culling happens in the VS (degenerate
//     collapse); the CPU only coarse-culls whole tiles via cached bounds.
// The app owns GPU buffers and LOD/material resolution: it uploads each
// tile once, adds its buffer base to run starts, picks LOD per run by
// range, and records one indirect command per run. Placement stays
// World-free; Scene is never touched.

#include <cstdint>
#include <cmath>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_ids.h>
#include <Engine/Placement/placement_point.h>
#include <Engine/Placement/placement_tile.h>

namespace smvk::placement {

// VS tail push offset: the main pass' unused prevModel scratch
// (bytes 192..224), same overlay precedent as gfx::kFilteredVsPushOffset.
inline constexpr uint32_t kPlacementVsPushOffset = 192;
inline constexpr uint32_t kPlacementCullFrustum = 1;
inline constexpr uint32_t kPlacementCullAlive = 2;

// Must match placement_instanced.vert.hlsl PlacementVsMainPush tail (32B).
struct PlacementVsPush {
	uint64_t instances = 0;   // BDA of the tile's PlacementInstanceBuffer
	uint64_t alive = 0;       // BDA of flag2 buffer, 0 = skip alive test
	uint64_t renderViews = 0; // BDA of the RenderView slab
	uint32_t renderViewIndex = 0;
	uint32_t cullFlags = kPlacementCullFrustum | kPlacementCullAlive;
};
static_assert(sizeof(PlacementVsPush) == 32, "PlacementVsPush moved — update placement_instanced.vert.hlsl");

inline constexpr const char* kPlacementInstancedVsPath =
	"content/shaders/placement_instanced.vert.hlsl.spv";

// One contiguous prototype run inside a tile's sorted instance buffer.
struct ProtoRun {
	uint32_t prototypeId = 0;
	uint32_t start = 0; // first index in the tile buffer
	uint32_t count = 0; // includes VS-culled instances (degenerate collapse)
};

// Everything the app needs to emit indirect commands for one tile.
struct TileDrawSet {
	TileKey key;
	glm::vec3 center{ 0.0f };
	float radius = 0.0f;
	uint64_t aliveBDA = 0; // 0 = CPU tile (all alive), else GPU flag2 buffer
	std::vector<ProtoRun> runs;
};

// Model matrix convention shared with the VS (T * RotY * S, column-major).
// CPU-side uses: exact range/LOD math, debug views, transient draws.
inline glm::mat4 ComposeInstanceMatrix(const glm::vec3& pos, float yaw, const glm::vec3& scale) {
	const float c = std::cos(yaw), s = std::sin(yaw);
	const float sx = scale.x; // uniform by construction
	return glm::mat4(
		glm::vec4(c * sx, 0.0f, -s * sx, 0.0f),
		glm::vec4(0.0f, scale.y, 0.0f, 0.0f),
		glm::vec4(s * sx, 0.0f, c * sx, 0.0f),
		glm::vec4(pos, 1.0f));
}

// Collects coarse-visible tiles (bounds test only) with their runs.
// Empty tiles are skipped. Output order is tile-map order (unspecified);
// runs within a set are prototype-ascending.
void CollectTileDrawSets(const PlacementSystem& sys, uint64_t programId,
	const std::function<bool(const glm::vec3&, float)>& frustumTest,
	std::vector<TileDrawSet>& out);

} // namespace smvk::placement
