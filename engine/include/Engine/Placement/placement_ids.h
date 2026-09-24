#pragma once
// PlacementGraph stable IDs + deterministic channels (CPU mirror).
// Contract: integer hashing only (32-bit lanes — GPU-safe, no Int64).
// No sequential RNG. The same (graphSeed, node, tile, prim, candidate)
// tuple MUST produce the same StableID on every thread order, every tile
// streaming order, CPU or GPU. The HLSL mirror lives in
// engine/shaders/placement_common.hlsli — keep the two bit-identical
// (parity covered by CPU-side property tests; GPU readback in Phase 3).

#include <cstdint>

namespace smvk::placement {

// Random channels derived from one StableID. Separate channels so e.g.
// changing rotation variation never reshuffles prototype selection.
enum class RandomChannel : uint32_t {
	Position = 0,
	Rotation = 1,
	Scale = 2,
	Variant = 3,
	Density = 4,
};

// 128-bit stable identity for one candidate/instance (4x32 lanes).
struct StableID {
	uint32_t w[4] = { 0, 0, 0, 0 };
	bool operator==(const StableID& o) const {
		return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2] && w[3] == o.w[3];
	}
	bool operator!=(const StableID& o) const { return !(*this == o); }
};

// World-streaming tile coordinate. Participates in hashing so tiles
// regenerate identically regardless of activation order.
struct TileID {
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	constexpr bool operator==(const TileID& o) const { return x == o.x && y == o.y && z == o.z; }
};

// ---- Integer hash primitives (32-bit, splitmix/murmur family) ----
inline uint32_t SplitMix32(uint32_t x) {
	x += 0x9e3779b9u;
	x = (x ^ (x >> 16)) * 0x21f0aaadu;
	x = (x ^ (x >> 15)) * 0x735a2d97u;
	return x ^ (x >> 15);
}

// Mix one lane into a running state.
inline uint32_t HashMix32(uint32_t state, uint32_t lane) {
	state ^= SplitMix32(lane + 0x9e3779b9u);
	state *= 0x01000193u;
	return SplitMix32(state);
}

StableID MakeStableID(uint64_t graphSeed, uint32_t nodeId, TileID tile,
	uint32_t primitiveId, uint32_t candidateIndex);

// 64-bit channel hash as two 32-bit lanes (no uint64_t needed on GPU).
struct ChannelHash { uint32_t lo = 0; uint32_t hi = 0; };
ChannelHash HashChannel(StableID id, RandomChannel channel);

// Deterministic [0,1): top 24 bits of the lo lane over 2^24.
// 24-bit keeps CPU/GPU float parity exact (representable in f32)
// and makes quantize-before-threshold decisions stable.
inline float Random01(StableID id, RandomChannel channel) {
	const ChannelHash h = HashChannel(id, channel);
	return (float)(h.lo >> 8) * (1.0f / 16777216.0f);
}

// Two uncorrelated [0,1) values from ONE channel hash: x = top 24 bits
// of lo, y = top 24 bits of hi. Used for 2D jitter (grid cells,
// barycentrics) without burning a second semantic channel.
struct Random02 { float x, y; };
inline Random02 Random02Of(StableID id, RandomChannel channel) {
	const ChannelHash h = HashChannel(id, channel);
	return Random02{
		(float)(h.lo >> 8) * (1.0f / 16777216.0f),
		(float)(h.hi >> 8) * (1.0f / 16777216.0f),
	};
}

// Integer priority for deterministic min-distance winners (higher wins).
// Pure integer — no float reduction, safe for GPU atomics compare
// (compare hi, then lo).
inline uint64_t PriorityOf(StableID id) {
	const ChannelHash h = HashChannel(id, RandomChannel::Variant);
	return (static_cast<uint64_t>(h.hi) << 32) | h.lo;
}

} // namespace smvk::placement
