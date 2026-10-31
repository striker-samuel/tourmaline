// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// PlacementCache: versioned binary snapshot of baked tiles.
// Modes: FullyProcedural (no cache) → BakedBase+RuntimeModifiers
// (offline quality placement + GPU runtime: seasons, destruction, LOD).
// Key = Hash(program, resources, tile); stale keys regenerate.

#include <cstdint>
#include <vector>

#include <Engine/Placement/placement_ids.h>
#include <Engine/Placement/placement_point.h>

namespace tour::placement {

inline constexpr uint32_t kPlacementCacheMagic = 0x504C4348u; // 'PLCH'
inline constexpr uint32_t kPlacementCacheVersion = 1;

struct PlacementCacheHeader {
	uint32_t magic = kPlacementCacheMagic;
	uint32_t version = kPlacementCacheVersion;
	uint64_t graphHash = 0;    // hash of PlacementProgram + seed
	uint64_t resourceHash = 0; // hash of deps (terrain/biome/proto versions)
	int32_t tileX = 0, tileY = 0, tileZ = 0;
	uint32_t instanceCount = 0;
};

// Cache record: RenderInstance (40B) + full StableID (16B) per instance
// so overrides/promotion survive a bake round-trip.
struct CachedInstance {
	RenderInstance in;
	StableID id;
};

// Serializes header + records. Returns empty on invalid input.
std::vector<uint8_t> SerializeCache(uint64_t graphHash, uint64_t resourceHash,
	TileID tile, const std::vector<RenderInstance>& instances,
	const std::vector<StableID>& ids);

// Parses + validates (magic, version, count). Returns false on mismatch;
// never throws, never partially fills.
bool DeserializeCache(const uint8_t* data, size_t size, uint64_t graphHash,
	uint64_t resourceHash, TileID tile, std::vector<RenderInstance>& instances,
	std::vector<StableID>& ids);

// Cache key ingredient: FNV-style fold over program bytes + dep versions.
uint64_t HashCombine64(uint64_t acc, uint64_t lane);

} // namespace tour::placement


// COMPAT (the namespace/RHI boundary audit, todo item 8.4): this
// placement header's declarations live in tour::placement; the smvk::
// placement aliases keep the existing call sites compiling through the
// consumer migration.
namespace smvk::placement {
	using ::tour::placement::CachedInstance;
	using ::tour::placement::DeserializeCache;
	using ::tour::placement::HashCombine64;
	using ::tour::placement::kPlacementCacheMagic;
	using ::tour::placement::kPlacementCacheVersion;
	using ::tour::placement::PlacementCacheHeader;
	using ::tour::placement::SerializeCache;
}
