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

} // namespace smvk::placement
