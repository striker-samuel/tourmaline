#include <Engine/Placement/placement_ids.h>

namespace smvk::placement {

StableID MakeStableID(uint64_t graphSeed, uint32_t nodeId, TileID tile,
	uint32_t primitiveId, uint32_t candidateIndex) {
	const uint32_t seedLo = static_cast<uint32_t>(graphSeed & 0xFFFFFFFFull);
	const uint32_t seedHi = static_cast<uint32_t>(graphSeed >> 32);
	const uint32_t tx = static_cast<uint32_t>(tile.x);
	const uint32_t ty = static_cast<uint32_t>(tile.y);
	const uint32_t tz = static_cast<uint32_t>(tile.z);

	// Lane 0: forward absorption.
	uint32_t w0 = seedLo ^ 0x243f6a88u;
	w0 = HashMix32(w0, nodeId);
	w0 = HashMix32(w0, tx);
	w0 = HashMix32(w0, ty);
	w0 = HashMix32(w0, tz);
	w0 = HashMix32(w0, primitiveId);
	w0 = HashMix32(w0, candidateIndex);

	// Lane 1: reverse absorption, different salt (uncorrelated to lane 0
	// even for adjacent candidate indices).
	uint32_t w1 = seedHi ^ 0x452821e6u;
	w1 = HashMix32(w1, candidateIndex);
	w1 = HashMix32(w1, primitiveId);
	w1 = HashMix32(w1, tz);
	w1 = HashMix32(w1, ty);
	w1 = HashMix32(w1, tx);
	w1 = HashMix32(w1, nodeId);

	// Lane 2: folded fields, third salt.
	uint32_t w2 = (seedLo ^ seedHi) ^ 0x38d01377u;
	w2 = HashMix32(w2, nodeId ^ candidateIndex);
	w2 = HashMix32(w2, tx ^ (tz << 11));
	w2 = HashMix32(w2, ty ^ (tx >> 7));
	w2 = HashMix32(w2, primitiveId ^ (ty << 5));

	// Lane 3: folded fields, reverse, fourth salt.
	uint32_t w3 = (seedLo + seedHi) ^ 0xda879e0bu;
	w3 = HashMix32(w3, primitiveId ^ (tz >> 3));
	w3 = HashMix32(w3, ty ^ candidateIndex);
	w3 = HashMix32(w3, tx ^ nodeId);
	w3 = HashMix32(w3, tz ^ (candidateIndex >> 4));

	return StableID{ { w0, w1, w2, w3 } };
}

ChannelHash HashChannel(StableID id, RandomChannel channel) {
	const uint32_t ch = static_cast<uint32_t>(channel);
	uint32_t lo = id.w[0] ^ SplitMix32(id.w[1] + 0x9e3779b9u);
	lo = HashMix32(lo, id.w[2]);
	lo = HashMix32(lo, ch);
	uint32_t hi = id.w[3] ^ SplitMix32(id.w[2] + 0x85ebca6bu);
	hi = HashMix32(hi, id.w[1]);
	hi = HashMix32(hi, ch ^ 0x9e3779b9u);
	return ChannelHash{ lo, hi };
}

} // namespace smvk::placement
