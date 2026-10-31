// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Placement/placement_physics.h>

#include <algorithm>

namespace tour::placement {
using namespace ::smvk;
namespace {

float InstanceRadius(const PlacementQuerySource& src, size_t i) {
	float r = src.defaultRadius;
	if (src.prototypes && !src.prototypes->empty() && i < src.instances->size()) {
		uint32_t p = (*src.instances)[i].prototypeId;
		if (p < src.prototypes->size()) r = src.prototypes->at(p).collisionRadius;
	}
	const glm::vec3& s = (*src.instances)[i].scale;
	return r * std::max(s.x, std::max(s.y, s.z));
}

} // namespace

std::optional<PlacementHit> RaycastPlacements(const PlacementQuerySource& src,
	const glm::vec3& origin, const glm::vec3& dir, float maxDist) {
	if (!src.instances || src.instances->empty() || maxDist <= 0.0f) return std::nullopt;
	std::optional<PlacementHit> best;
	for (size_t i = 0; i < src.instances->size(); ++i) {
		const RenderInstance& in = (*src.instances)[i];
		const float r = InstanceRadius(src, i);
		glm::vec3 oc = in.position - origin;
		float t = glm::dot(oc, dir);
		if (t < 0.0f || t > maxDist + r) continue;
		glm::vec3 closest = origin + dir * t;
		glm::vec3 d = in.position - closest;
		float d2 = glm::dot(d, d);
		if (d2 > r * r) continue;
		float tHit = t - std::sqrt(std::max(r * r - d2, 0.0f));
		if (tHit < 0.0f) tHit = 0.0f;
		if (!best || tHit < best->distance) {
			PlacementHit h;
			h.prototypeId = in.prototypeId;
			h.position = in.position;
			h.distance = tHit;
			if (src.ids && i < src.ids->size()) h.id = (*src.ids)[i];
			else h.id = StableID{ { in.stableLow, 0, 0, 0 } };
			best = h;
		}
	}
	return best;
}

std::vector<PlacementHit> QueryPlacementsAABB(const PlacementQuerySource& src,
	const glm::vec3& boxMin, const glm::vec3& boxMax) {
	std::vector<PlacementHit> hits;
	if (!src.instances) return hits;
	for (size_t i = 0; i < src.instances->size(); ++i) {
		const RenderInstance& in = (*src.instances)[i];
		const float r = InstanceRadius(src, i);
		if (in.position.x + r < boxMin.x || in.position.x - r > boxMax.x ||
			in.position.y + r < boxMin.y || in.position.y - r > boxMax.y ||
			in.position.z + r < boxMin.z || in.position.z - r > boxMax.z)
			continue;
		PlacementHit h;
		h.prototypeId = in.prototypeId;
		h.position = in.position;
		h.distance = 0.0f;
		if (src.ids && i < src.ids->size()) h.id = (*src.ids)[i];
		else h.id = StableID{ { in.stableLow, 0, 0, 0 } };
		hits.push_back(h);
	}
	return hits;
}

EntityId PromotionRegistry::promote(const StableID& id, EntityId fresh) {
	auto it = idToEntity_.find(id);
	if (it != idToEntity_.end()) return it->second;
	idToEntity_[id] = fresh;
	entityToId_[fresh] = id;
	return fresh;
}

bool PromotionRegistry::demote(const StableID& id) {
	auto it = idToEntity_.find(id);
	if (it == idToEntity_.end()) return false;
	entityToId_.erase(it->second);
	idToEntity_.erase(it);
	return true;
}

bool PromotionRegistry::demoteByEntity(EntityId e) {
	auto it = entityToId_.find(e);
	if (it == entityToId_.end()) return false;
	idToEntity_.erase(it->second);
	entityToId_.erase(it);
	return true;
}

bool PromotionRegistry::isSuppressed(const StableID& id) const {
	return idToEntity_.find(id) != idToEntity_.end();
}

std::optional<EntityId> PromotionRegistry::entityFor(const StableID& id) const {
	auto it = idToEntity_.find(id);
	if (it == idToEntity_.end()) return std::nullopt;
	return it->second;
}

void PromotionRegistry::clear() {
	idToEntity_.clear();
	entityToId_.clear();
}

} // namespace tour::placement
