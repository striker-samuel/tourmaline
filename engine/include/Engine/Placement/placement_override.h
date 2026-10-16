// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// OverrideLayer: nondestructive art direction on top of procedural results.
// Artists NEVER edit generated points; overrides key on StableID so forest
// regens (density changes, tile streaming) keep the moved/deleted tree.
//
// Evaluation order: Procedural Result → OverrideLayer → Final Placement.

#include <cstddef>
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
	std::optional<uint32_t> customB;
	// NOTE: RenderInstance::stableLow is identity (low 32 bits of the
	// StableID), not an overridable attribute — rewriting it would
	// break override/debug identity. Every other RenderInstance field
	// (position, yaw, scale, prototypeId, customB) is covered above.
};

// ---- Node-filtered (bulk) rules --------------------------------------
// A per-StableID PlacementOverride is exact but tedious for art
// direction across a whole scatter node ("make every oak on this node
// 20% bigger"). Node rules apply a shared payload to every instance
// whose tile context matches the scope, in rule order (later wins);
// an exact PlacementOverride for the same StableID still wins over
// all bulk rules (more specific wins, Clarisse-style).
struct OverrideScope {
	std::optional<uint64_t> graphSeed; // empty = any seed
	std::optional<uint32_t> nodeId;    // empty = any placement node
	bool isGlobal() const { return !graphSeed && !nodeId; }
	bool matches(uint64_t seed, uint32_t node) const {
		return (!graphSeed || *graphSeed == seed)
			&& (!nodeId || *nodeId == node);
	}
};

struct NodeOverride {
	OverrideScope scope; // empty scope = every tile (global layer)
	bool deleted = false; // true = clear all in-scope instances
	std::optional<glm::vec3> position;
	std::optional<float> yaw;
	std::optional<glm::vec3> scale;
	std::optional<uint32_t> prototype;
	std::optional<uint32_t> customB;
};

class OverrideLayer {
public:
	void clear() { rules_.clear(); nodeRules_.clear(); }
	size_t size() const { return rules_.size(); }
	void set(PlacementOverride o) { rules_[o.target] = o; }
	void remove(const StableID& id) { rules_.erase(id); }
	const PlacementOverride* find(const StableID& id) const {
		auto it = rules_.find(id);
		return it != rules_.end() ? &it->second : nullptr;
	}

	// Bulk node-filtered rules (ordered: later wins). A rule whose
	// scope is global (no seed/node set) applies to every tile,
	// including through the legacy apply() below; scoped rules need
	// the tile context overload to be evaluated.
	void addNodeRule(NodeOverride r) { nodeRules_.push_back(std::move(r)); }
	bool removeNodeRule(size_t index) {
		if (index >= nodeRules_.size()) return false;
		nodeRules_.erase(nodeRules_.begin() + (ptrdiff_t)index);
		return true;
	}
	void clearNodeRules() { nodeRules_.clear(); }
	size_t nodeRuleCount() const { return nodeRules_.size(); }
	const NodeOverride* nodeRule(size_t i) const {
		return i < nodeRules_.size() ? &nodeRules_[i] : nullptr;
	}

	// Applies overrides to a generated tile in place. instances[i]
	// corresponds to ids[i]. Deleted targets are compacted out; moved
	// targets keep their StableID (physics/promotion unaffected).
	// The 2-arg form applies exact rules plus GLOBAL node rules
	// (scoped rules need tile context, so they are skipped here).
	void apply(std::vector<RenderInstance>& instances, std::vector<StableID>& ids) const;
	// Context-aware form: additionally applies every node rule whose
	// scope matches (graphSeed, nodeId). Precedence per instance:
	// matching node rules in order, then the exact StableID rule.
	void apply(std::vector<RenderInstance>& instances, std::vector<StableID>& ids,
		uint64_t graphSeed, uint32_t nodeId) const;

private:
	void applyImpl(std::vector<RenderInstance>& instances, std::vector<StableID>& ids,
		bool hasContext, uint64_t graphSeed, uint32_t nodeId) const;

	std::unordered_map<StableID, PlacementOverride, StableIDHash> rules_;
	std::vector<NodeOverride> nodeRules_;
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
