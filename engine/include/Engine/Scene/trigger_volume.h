// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Per-actor runtime trigger evaluation on the SAME oriented box used by
// GI and audio rooms. Call Update once per actor per simulation tick.
// It emits transitions, not per-frame stay events; no global listener state.
#include <Engine/Scene/scene.h>
#include <Engine/Scene/spatial_volume.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace tour::scene {

inline bool TriggerContains(const SpatialVolumeBox& box,
		const glm::vec3& worldPos) {
	if (!box.Valid()) return false;
	const glm::vec3 delta = worldPos - box.center;
	for (int axis = 0; axis < 3; ++axis)
		if (std::abs(glm::dot(delta, box.axes[axis])) > box.half[axis])
			return false;
	return true;
}

struct TriggerEvent {
	enum class Type { Enter, Exit };
	Type type;
	uint64_t actorId;
	NodeId node;
	std::string tag;
};

// State is per world/scene. Reset/ResetActor on unload, teleport or actor
// destruction; the owner drives updates with actor positions.
class TriggerVolumeTracker {
public:
	template <typename Callback>
	void Update(const Scene& scene, uint64_t actorId,
			const glm::vec3& worldPosition, Callback&& onEvent) {
		auto& active = active_[actorId];
		auto& fired = fired_[actorId];
		std::unordered_set<NodeId> next;
		for (const TriggerVolumeRow& row : scene.tables.triggerVolumes) {
			if (!row.trigger.enabled
					|| row.node >= scene.globalTransform.size())
				continue;
			if (!TriggerContains(
					VolumeBoxFromGlobalTransform(scene.globalTransform[row.node]),
					worldPosition))
				continue;
			// The once flag applies to the actor's lifetime: a second
			// visit emits neither enter nor a spurious exit.
			if (row.trigger.once && !active.contains(row.node)
					&& fired.contains(row.node))
				continue;
			next.insert(row.node);
			if (!active.contains(row.node)) {
				onEvent(TriggerEvent{TriggerEvent::Type::Enter,
					actorId, row.node, row.trigger.tag});
				if (row.trigger.once)
					fired.insert(row.node);
			}
		}
		for (NodeId node : active) {
			if (next.contains(node)) continue;
			const TriggerVolumeRow* row = FindTriggerVolume(scene, node);
			if (row)
				onEvent(TriggerEvent{TriggerEvent::Type::Exit,
					actorId, node, row->trigger.tag});
		}
		active = std::move(next);
	}
	void ResetActor(uint64_t actorId) {
		active_.erase(actorId);
		fired_.erase(actorId);
	}
	void Reset() {
		active_.clear();
		fired_.clear();
	}
private:
	std::unordered_map<uint64_t, std::unordered_set<NodeId>> active_;
	std::unordered_map<uint64_t, std::unordered_set<NodeId>> fired_;
};

} // namespace tour::scene
