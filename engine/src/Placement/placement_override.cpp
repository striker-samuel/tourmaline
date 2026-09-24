#include <Engine/Placement/placement_override.h>

namespace smvk::placement {

void OverrideLayer::apply(std::vector<RenderInstance>& instances, std::vector<StableID>& ids) const {
	if (rules_.empty() || instances.empty()) return;
	std::vector<RenderInstance> kept;
	kept.reserve(instances.size());
	std::vector<StableID> keptIds;
	keptIds.reserve(ids.size());
	for (size_t i = 0; i < instances.size(); ++i) {
		const PlacementOverride* o = find(ids[i]);
		if (o && o->deleted) continue;
		RenderInstance in = instances[i];
		if (o) {
			if (o->position) in.position = *o->position;
			if (o->yaw) in.yaw = *o->yaw;
			if (o->scale) in.scale = *o->scale;
			if (o->prototype) in.prototypeId = *o->prototype;
		}
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
