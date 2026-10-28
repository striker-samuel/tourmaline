// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Instanced draw feed: turns resident tiles into GPU-visible draws with
// zero per-instance CPU work per frame.
//
// Contract with the GPU (the PR03 visible chain — see
// placement_instanced.vert.hlsl + placement_cull.hlsli):
//   - the scatter kernels write the persistent 40-byte instances ONCE
//     per tile generation (they never move per camera frame);
//   - per (tile slot, view), the cull chain (cull -> prefix ->
//     scatter -> drawbuild) builds the visible-index list + the proto
//     bins + the indirect commands' dynamic halves on the GPU — no
//     count readback, no CPU per-instance work;
//   - the CPU coarse-culls whole tiles via cached bounds, seeds the
//     immutable indirect halves (indexCount/firstIndex/vertexOffset)
//     once per prototype table, and records ONE DrawIndexedIndirect
//     per (tile, proto) — stride kPlacementIndirectStride, offset
//     proto * stride (instanceCount == 0 bins draw nothing);
//   - the VS fetches visible[SV_InstanceID] -> source slot -> the
//     instance (SV_InstanceID includes the command's firstInstance =
//     the bin's offset — validated by PlacementGpuTests' bin-range
//     identity checks).
// The app owns GPU buffers and LOD/material resolution. Placement stays
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

// The debug re-test bit (VS-only): re-run the legacy guard-based
// frustum test and collapse failures — a compare switch while the
// cull chain is validated. Production runs with cullFlags = 0 (the
// production bound is the per-proto cooked sphere in the cull kernel).
inline constexpr uint32_t kPlacementCullDebugRetest = 8;

// Must match placement_instanced.vert.hlsl PlacementVsMainPush tail (32B).
// PR03: the visible-chain fetch — visible[SV_InstanceID] -> source slot
// -> instance. The old `alive` BDA is gone (the cull kernel owns
// liveness now); the fetch itself is a load, no per-instance test.
struct PlacementVsPush {
	uint64_t visible = 0;     // BDA of the (slot, view) visible-index list
	uint64_t instances = 0;   // BDA of the tile's PlacementInstanceBuffer
	uint64_t renderViews = 0; // BDA of the RenderView slab
	uint32_t renderViewIndex = 0;
	uint32_t cullFlags = 0;   // 0 = trust the chain; DEBUG_RETEST = legacy compare
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
