// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Prototype table: instances reference entries, never duplicate geometry.
// Selection happens AFTER accept/reject so editing asset weights never
// reshuffles the forest layout (distribution independent from assets).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace smvk::placement {

struct Prototype {
	std::string assetUri;          // resolves via World::meshLocations / Cluster URIs
	float weight = 1.0f;           // weighted-random share (must be >= 0)
	float collisionRadius = 0.5f;  // runtime min-distance + physics broadphase
	glm::vec3 boundsMin{ -0.5f };
	glm::vec3 boundsMax{ 0.5f };
	uint32_t flags = 0;
	float maxDrawDistance = 0.0f; // species distance cull; 0 = unlimited
	                               // (grass short, trees/rocks far)
};

class PrototypeTable {
public:
	void clear() { protos_.clear(); prefix_.clear(); total_ = 0.0f; }
	bool empty() const { return protos_.empty(); }
	size_t size() const { return protos_.size(); }

	void add(Prototype p);
	const Prototype& at(uint32_t i) const { return protos_[i]; }
	const std::vector<Prototype>& all() const { return protos_; }

	// Weighted-random pick driven by the VARIANT channel (r in [0,1)).
	// Returns nullopt when table empty or total weight <= 0.
	std::optional<uint32_t> select(float r) const;

	float totalWeight() const { return total_; }

private:
	std::vector<Prototype> protos_;
	std::vector<float> prefix_; // cumulative weights, same length
	float total_ = 0.0f;
};

// Slope rule helper (MVP): slopeDeg > threshold picks from rockSet,
// else from vegSet. Operates on prototype INDICES into the same table.
uint32_t SelectBySlope(const PrototypeTable& table, float slopeDeg, float thresholdDeg,
	uint32_t vegProto, uint32_t rockProto);

} // namespace smvk::placement
