// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Placement/placement_draw.h>

#include <cmath>

namespace smvk::placement {

void CollectTileDrawSets(const PlacementSystem& sys, uint64_t programId,
	const std::function<bool(const glm::vec3&, float)>& frustumTest,
	std::vector<TileDrawSet>& out) {
	sys.forEachResidentTile(programId, [&](const TileKey& key, const PlacementTile& tile) {
		if (tile.instances.empty()) return;
		glm::vec3 center = (tile.boundsMin + tile.boundsMax) * 0.5f;
		float radius = glm::length(tile.boundsMax - center);
		if (frustumTest && !frustumTest(center, radius)) return;
		TileDrawSet set;
		set.key = key;
		set.center = center;
		set.radius = radius;
		set.aliveBDA = tile.aliveBDA;
		// Instances are proto-sorted at generation: single linear run scan.
		uint32_t runStart = 0;
		for (uint32_t i = 1; i <= (uint32_t)tile.instances.size(); ++i) {
			if (i < tile.instances.size()
				&& tile.instances[i].prototypeId == tile.instances[runStart].prototypeId)
				continue;
			ProtoRun r;
			r.prototypeId = tile.instances[runStart].prototypeId;
			r.start = runStart;
			r.count = i - runStart;
			set.runs.push_back(r);
			runStart = i;
		}
		out.push_back(std::move(set));
	});
}

} // namespace smvk::placement
