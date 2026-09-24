#pragma once
// OverrideLayer: nondestructive art direction on top of procedural results.
// Artists NEVER edit generated points; overrides key on StableID so forest
// regens (density changes, tile streaming) keep the moved/deleted tree.
//
// Evaluation order: Procedural Result → OverrideLayer → Final Placement.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_ids.h>
#include <Engine/Placement/placement_point.h>

namespace smvk::placement {

struct StableIDHash {
	size_t operator()(const StableID& id) const {
		uint32_t h = SplitMix32(id.w[0] + 0x9e3779b9u);
		h = HashMix32(h, id.w[1]);
		h = HashMix32(h, id.w[2]);
		h = HashMix32(h, id.w[3]);
		return static_cast<size_t>(h);
	}
};

struct PlacementOverride {
	StableID target;
	bool deleted = false;
	std::optional<glm::vec3> position;
	std::optional<float> yaw;
	std::optional<glm::vec3> scale;
	std::optional<uint32_t> prototype;
};

class OverrideLayer {
public:
	void clear() { rules_.clear(); }
	size_t size() const { return rules_.size(); }
	void set(PlacementOverride o) { rules_[o.target] = o; }
	void remove(const StableID& id) { rules_.erase(id); }
	const PlacementOverride* find(const StableID& id) const {
		auto it = rules_.find(id);
		return it != rules_.end() ? &it->second : nullptr;
	}

	// Applies overrides to a generated tile in place. instances[i]
	// corresponds to ids[i]. Deleted targets are compacted out; moved
	// targets keep their StableID (physics/promotion unaffected).
	void apply(std::vector<RenderInstance>& instances, std::vector<StableID>& ids) const;

private:
	std::unordered_map<StableID, PlacementOverride, StableIDHash> rules_;
};

// ---- Runtime exclusion volumes (dynamic masks, MVP) -------------------
// Player-built houses, cleared foliage, explosion craters: discs in XZ
// that suppress procedural instances. Only affected tiles regenerate
// (caller maps disc → dirty tiles via PlacementSystem::invalidateLayer
// or per-tile keys).
struct ExclusionDisc {
	glm::vec2 center{ 0.0f }; // world XZ
	float radius = 1.0f;
};

// Removes instances whose XZ falls inside any disc. Operates on the
// procedural result BEFORE overrides (an artist override can still
// re-add a hero tree inside a crater).
size_t ApplyExclusions(std::vector<RenderInstance>& instances, std::vector<StableID>& ids,
	const std::vector<ExclusionDisc>& discs);

} // namespace smvk::placement
