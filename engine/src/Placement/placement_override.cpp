// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Placement/placement_override.h>

namespace smvk::placement {
namespace {

// Shared payload application (exact and bulk rules carry the same
// attribute set: every mutable RenderInstance field).
template<typename Rule>
void ApplyPayload(RenderInstance& in, const Rule& o) {
	if (o.position) in.position = *o.position;
	if (o.yaw) in.yaw = *o.yaw;
	if (o.scale) in.scale = *o.scale;
	if (o.prototype) in.prototypeId = *o.prototype;
	if (o.customB) in.customB = *o.customB;
}

} // namespace

void OverrideLayer::apply(std::vector<RenderInstance>& instances, std::vector<StableID>& ids) const {
	applyImpl(instances, ids, false, 0, 0);
}

void OverrideLayer::apply(std::vector<RenderInstance>& instances, std::vector<StableID>& ids,
	uint64_t graphSeed, uint32_t nodeId) const {
	applyImpl(instances, ids, true, graphSeed, nodeId);
}

void OverrideLayer::applyImpl(std::vector<RenderInstance>& instances, std::vector<StableID>& ids,
	bool hasContext, uint64_t graphSeed, uint32_t nodeId) const {
	if ((rules_.empty() && nodeRules_.empty()) || instances.empty()) return;
	std::vector<RenderInstance> kept;
	kept.reserve(instances.size());
	std::vector<StableID> keptIds;
	keptIds.reserve(ids.size());
	for (size_t i = 0; i < instances.size(); ++i) {
		// 1. Bulk node-filtered rules first, in order (later wins).
		// Without tile context only global (unscoped) rules can be
		// evaluated; scoped rules are skipped (never misapplied).
		RenderInstance in = instances[i];
		bool dropped = false;
		for (const NodeOverride& nr : nodeRules_) {
			const bool inScope = hasContext
				? nr.scope.matches(graphSeed, nodeId)
				: nr.scope.isGlobal();
			if (!inScope) continue;
			if (nr.deleted) { dropped = true; break; }
			ApplyPayload(in, nr);
		}
		if (dropped) continue;
		// 2. Exact StableID rule wins over every bulk rule.
		const PlacementOverride* o = find(ids[i]);
		if (o && o->deleted) continue;
		if (o) ApplyPayload(in, *o);
		kept.push_back(in);
		keptIds.push_back(ids[i]);
	}
	instances.swap(kept);
	ids.swap(keptIds);
}

size_t ApplyExclusions(std::vector<RenderInstance>& instances, std::vector<StableID>& ids,
	const std::vector<ExclusionDisc>& discs) {
	if (discs.empty() || instances.empty()) return instances.size();
	size_t w = 0;
	for (size_t r = 0; r < instances.size(); ++r) {
		bool killed = false;
		for (const ExclusionDisc& d : discs) {
			if (d.radius <= 0.0f) continue;
			float dx = instances[r].position.x - d.center.x;
			float dz = instances[r].position.z - d.center.y;
			if (dx * dx + dz * dz < d.radius * d.radius) { killed = true; break; }
		}
		if (!killed) {
			if (w != r) { instances[w] = instances[r]; ids[w] = ids[r]; }
			++w;
		}
	}
	instances.resize(w);
	ids.resize(w);
	return w;
}

} // namespace smvk::placement
