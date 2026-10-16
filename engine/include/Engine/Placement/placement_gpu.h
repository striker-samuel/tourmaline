// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU tile-evaluation backend (HZD-style runtime path).
// C++ mirror of engine/shaders/placement_tile.cs.hlsl push constants.
// The kernel evaluates: candidate → StableID → grid sample → density field
// (base + value-noise) → quantized accept → weighted prototype → transform,
// compacted via atomic append into a persistent per-tile instance buffer.
//
// Mesh-surface / heightfield domains + GPU min-distance arrive next;
// the CPU EvaluateTile() in placement_tile.h is the oracle for all paths.

#include <cstdint>

#include <glm/glm.hpp>

namespace smvk::placement {

// Output buffer views (HLSL: vk::BufferPointer, see placement_tile.cs.hlsl).
struct PlacementInstanceBuffer {
	RenderInstance items[1]; // variable-length
};
struct PlacementCountBuffer {
	uint32_t count = 0;
};

// Must match HLSL PlacementTilePush (144 bytes, 16-byte rows after the
// two 8-byte BDAs; compiled with -fvk-use-dx-layout).
struct PlacementTilePush {
	uint64_t outInstances = 0; // BDA of PlacementInstanceBuffer
	uint64_t outCount = 0;     // BDA of PlacementCountBuffer (single uint)
	uint32_t seedLo = 0;
	uint32_t seedHi = 0;
	uint32_t nodeId = 0;
	uint32_t candidateCount = 0;
	int32_t tileX = 0;
	int32_t tileY = 0;
	int32_t tileZ = 0;
	uint32_t pad0 = 0;
	glm::vec3 gridOrigin{ 0.0f };
	uint32_t cellsX = 8;
	glm::vec2 gridExtent{ 64.0f, 64.0f };
	uint32_t cellsZ = 8;
	uint32_t pad1 = 0;
	float density = 0.5f;
	float noiseAmp = 0.0f;  // 0 = constant density; else d = saturate(base + amp*(n-0.5)*2)
	float noiseFreq = 0.02f;
	uint32_t pad2 = 0;
	float minScale = 0.8f;
	float maxScale = 1.2f;
	uint32_t protoCount = 1; // <= 8
	uint32_t pad3 = 0;
	glm::vec4 weightsA{ 1, 0, 0, 0 }; // prototypes 0..3
	glm::vec4 weightsB{ 0, 0, 0, 0 }; // prototypes 4..7
};
static_assert(sizeof(PlacementTilePush) == 144, "PlacementTilePush size changed — update placement_tile.cs.hlsl");

inline constexpr uint32_t kPlacementTileThreads = 64;
inline uint32_t PlacementDispatchGroups(uint32_t candidateCount) {
	return (candidateCount + kPlacementTileThreads - 1u) / kPlacementTileThreads;
}

inline constexpr const char* kPlacementTileCsPath = "content/shaders/placement_tile.cs.hlsl.spv";
inline constexpr const char* kPlacementMeshTileCsPath = "content/shaders/placement_mesh_tile.cs.hlsl.spv";
inline constexpr const char* kPlacementHfTileCsPath = "content/shaders/placement_hf_tile.cs.hlsl.spv";
inline constexpr const char* kPlacementConBuildCsPath = "content/shaders/placement_con_build.cs.hlsl.spv";
inline constexpr const char* kPlacementConElectCsPath = "content/shaders/placement_con_elect.cs.hlsl.spv";
inline constexpr const char* kPlacementConKillCsPath = "content/shaders/placement_con_kill.cs.hlsl.spv";

inline constexpr uint32_t kPlacementConMaxInstances = 65536;
inline constexpr uint32_t kPlacementConMaxCells = 262144; // 64^3

// Must match placement_constraint.hlsli PlacementConParams (64 bytes).
struct PlacementConParams {
	glm::vec3 origin{ 0.0f };
	float spacing = 1.0f;
	uint32_t dimX = 0, dimY = 0, dimZ = 0;
	uint32_t pad0 = 0;
	uint32_t seedLo = 0, seedHi = 0, nodeId = 0;
	uint32_t pad1 = 0;
	int32_t tileX = 0, tileY = 0, tileZ = 0;
	uint32_t pad2 = 0;
};
static_assert(sizeof(PlacementConParams) == 64, "PlacementConParams moved — update placement_constraint.hlsli");

// Must match the constraint kernels' push (64 bytes).
struct PlacementConPush {
	uint64_t instances = 0;
	uint64_t head = 0;
	uint64_t next = 0;
	uint64_t flag2 = 0;
	uint64_t params = 0;
	uint64_t countBuf = 0;
	uint32_t pad[4] = {};
};
static_assert(sizeof(PlacementConPush) == 64, "PlacementConPush moved — update constraint kernels");

// ---- Mesh / heightfield scatter path ----------------------------------
// The grid kernel carries everything inline in its 144B push (engine
// precedent: FilterPush is 200B). Mesh/heightfield descriptors don't fit
// that budget, so they live in a device-side params struct addressed by
// a compact 64B push. Domain data layouts are padded for unambiguous
// CPU/GPU agreement: positions/normals as float4 arrays (never float3,
// whose HLSL array stride is layout-dependent), uvs float2, the rest
// scalars.

inline constexpr uint32_t kPlacementScatterDomain_Mesh = 1;
inline constexpr uint32_t kPlacementScatterDomain_Heightfield = 2;

inline constexpr uint32_t kPlacementFlag_HasNormals = 1u << 0;
inline constexpr uint32_t kPlacementFlag_HasUVs = 1u << 1;

// Must match placement_domains.hlsli PlacementDomainParams (176 bytes).
struct PlacementDomainParams {
	float density = 0.5f;
	float noiseAmp = 0.0f;
	float noiseFreq = 0.02f;
	float minScale = 0.8f;
	float maxScale = 1.0f;
	uint32_t protoCount = 1;
	uint32_t padA[2] = {};
	float weights[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
	uint32_t domainKind = 0;
	uint32_t triCount = 0;
	uint32_t vertCount = 0;
	uint32_t flags = 0;
	uint64_t posBDA = 0;   // float4[vertCount]
	uint64_t nrmBDA = 0;   // float4[vertCount], 0 when absent
	uint64_t uvBDA = 0;    // float2[vertCount], 0 when absent
	uint64_t idxBDA = 0;   // uint32[triCount*3]
	uint64_t probBDA = 0;  // float[triCount] alias keep-probability
	uint64_t aliasBDA = 0; // uint32[triCount] alias fallback
	uint64_t heightsBDA = 0; // float[hfW*hfH], heightfield only
	uint32_t hfW = 0;
	uint32_t hfH = 0;
	float hfOX = 0.0f, hfOZ = 0.0f; // world origin of texel (0,0)
	float hfSX = 1.0f, hfSZ = 1.0f; // meters per texel
	float extMinX = 0.0f, extMinZ = 0.0f; // scatter rect (world XZ)
	float extMaxX = 64.0f, extMaxZ = 64.0f;
};
static_assert(sizeof(PlacementDomainParams) == 176, "PlacementDomainParams moved — update placement_domains.hlsli");

// Must match the scatter kernels' push (64 bytes).
struct PlacementScatterPush {
	uint64_t outInstances = 0;
	uint64_t outCount = 0;
	uint64_t params = 0; // BDA of PlacementDomainParams
	uint64_t pad0 = 0;
	uint32_t seedLo = 0;
	uint32_t seedHi = 0;
	uint32_t nodeId = 0;
	uint32_t candidateCount = 0;
	int32_t tileX = 0, tileY = 0, tileZ = 0;
	uint32_t flags = 0;
};
static_assert(sizeof(PlacementScatterPush) == 64, "PlacementScatterPush moved — update scatter kernels");

} // namespace smvk::placement
