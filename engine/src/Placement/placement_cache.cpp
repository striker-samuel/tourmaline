#include <Engine/Placement/placement_cache.h>

#include <cstring>

namespace smvk::placement {

uint64_t HashCombine64(uint64_t acc, uint64_t lane) {
	acc ^= lane + 0x9e3779b97f4a7c15ull + (acc << 6) + (acc >> 2);
	return acc;
}

std::vector<uint8_t> SerializeCache(uint64_t graphHash, uint64_t resourceHash,
	TileID tile, const std::vector<RenderInstance>& instances,
	const std::vector<StableID>& ids) {
	std::vector<uint8_t> out;
	if (instances.size() != ids.size()) return out;
	PlacementCacheHeader h;
	h.graphHash = graphHash;
	h.resourceHash = resourceHash;
	h.tileX = tile.x; h.tileY = tile.y; h.tileZ = tile.z;
	h.instanceCount = static_cast<uint32_t>(instances.size());
	out.resize(sizeof(h) + instances.size() * sizeof(CachedInstance));
	std::memcpy(out.data(), &h, sizeof(h));
	CachedInstance* dst = reinterpret_cast<CachedInstance*>(out.data() + sizeof(h));
	for (size_t i = 0; i < instances.size(); ++i) {
		dst[i].in = instances[i];
		dst[i].id = ids[i];
	}
	return out;
}

bool DeserializeCache(const uint8_t* data, size_t size, uint64_t graphHash,
	uint64_t resourceHash, TileID tile, std::vector<RenderInstance>& instances,
	std::vector<StableID>& ids) {
	instances.clear();
	ids.clear();
	if (!data || size < sizeof(PlacementCacheHeader)) return false;
	PlacementCacheHeader h;
	std::memcpy(&h, data, sizeof(h));
	if (h.magic != kPlacementCacheMagic || h.version != kPlacementCacheVersion) return false;
	if (h.graphHash != graphHash || h.resourceHash != resourceHash) return false;
	if (h.tileX != tile.x || h.tileY != tile.y || h.tileZ != tile.z) return false;
	const size_t want = sizeof(h) + (size_t)h.instanceCount * sizeof(CachedInstance);
	if (size < want || h.instanceCount > (1u << 24)) return false; // sanity cap 16M
	const CachedInstance* src = reinterpret_cast<const CachedInstance*>(data + sizeof(h));
	instances.reserve(h.instanceCount);
	ids.reserve(h.instanceCount);
	for (uint32_t i = 0; i < h.instanceCount; ++i) {
		instances.push_back(src[i].in);
		ids.push_back(src[i].id);
	}
	return true;
}

} // namespace smvk::placement
