// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/WorldFields/WorldFieldProvider.h>

namespace engine::worldfields {

size_t WorldFieldProvider::GatherRequests(worldpartition::StreamRequestSink* sink,
	uint64_t frameIndex)
{
	// v1: one diagnostic request per ACTIVE ROOT CELL under the
	// WorldField-layer interests (the editor's residency map / the
	// scheduler's request currency — the real per-brick demand stays
	// inside the system's windows, which the interests drive). The
	// shared dedupe/priority engine grows on this exact seam with the
	// terrain client.
	if (sink == nullptr || lastInterests_.empty())
		return 0;
	size_t pushed = 0;
	for (const worldpartition::StreamingInterest& in : lastInterests_) {
		if ((in.layerMask & worldpartition::StreamLayerBit(
				worldpartition::StreamLayer::WorldField)) == 0)
			continue;
		const glm::dvec3 min = in.position
			- glm::dvec3(in.radiusMeters, 0.0, in.radiusMeters);
		const glm::dvec3 max = in.position
			+ glm::dvec3(in.radiusMeters, 0.0, in.radiusMeters);
		const worldpartition::WorldCellKey lo = worldpartition::GetWorldCell(min);
		const worldpartition::WorldCellKey hi = worldpartition::GetWorldCell(max);
		for (int64_t z = lo.z; z <= hi.z; ++z)
			for (int64_t x = lo.x; x <= hi.x; ++x) {
				worldpartition::StreamRequest r{};
				r.layer = worldpartition::StreamLayer::WorldField;
				r.cell = worldpartition::WorldCellKey{ x, z };
				r.desired = in.desired;
				r.priority = in.priority;
				r.frameRequested = frameIndex;
				sink->Push(r);
				++pushed;
			}
	}
	return pushed;
}

} // namespace engine::worldfields
