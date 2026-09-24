#pragma once
// PhysicsView: answers "what procedural mesh lives here" WITHOUT Scene
// nodes and WITHOUT mirroring the forest as CPU entities.
//
// Two tiers:
//   Far field — query InstanceDB directly (cover checks, broadphase,
//   placement-vs-placement). No entity, no physics body.
//   Near field — Promote(StableID) creates ONE gameplay entity and
//   suppresses the procedural copy; Demote() restores it (persisting an
//   override when the entity changed the world, e.g. stump left behind).
//
// Engine-agnostic: EntityId is an opaque caller handle (physics SDK,
// gameplay object id, whatever owns bodies). This header never includes
// a physics engine.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_ids.h>
#include <Engine/Placement/placement_override.h>
#include <Engine/Placement/placement_point.h>
#include <Engine/Placement/placement_prototype.h>

namespace smvk::placement {

using EntityId = uint64_t;
inline constexpr EntityId kInvalidEntity = 0;

struct PlacementHit {
	StableID id;
	uint32_t prototypeId = 0;
	glm::vec3 position{ 0.0f };
	float distance = 0.0f; // ray t, or center distance for AABB queries
};

// Flat query source: caller gathers resident instances (+ids) from
// PlacementSystem tiles. Decoupled so physics never touches tile state.
struct PlacementQuerySource {
	const std::vector<RenderInstance>* instances = nullptr;
	const std::vector<StableID>* ids = nullptr; // parallel, may be null
	const PrototypeTable* prototypes = nullptr; // for collisionRadius, may be null
	float defaultRadius = 0.5f;
};

// Nearest sphere hit along ray (instance = sphere at position with
// radius collisionRadius*maxScale). Returns nullopt on miss.
std::optional<PlacementHit> RaycastPlacements(const PlacementQuerySource& src,
	const glm::vec3& origin, const glm::vec3& dir, float maxDist);

// All instances whose sphere intersects the AABB.
std::vector<PlacementHit> QueryPlacementsAABB(const PlacementQuerySource& src,
	const glm::vec3& boxMin, const glm::vec3& boxMax);

// Promotion registry: StableID <-> EntityId with suppression set.
// The renderer skips suppressed StableIDs; the spawner creates bodies.
class PromotionRegistry {
public:
	// Returns existing entity when already promoted (idempotent).
	EntityId promote(const StableID& id, EntityId fresh);
	// Releases back to procedural. Returns false when unknown.
	bool demote(const StableID& id);
	bool demoteByEntity(EntityId e);
	bool isSuppressed(const StableID& id) const;
	std::optional<EntityId> entityFor(const StableID& id) const;
	size_t promotedCount() const { return idToEntity_.size(); }
	void clear();

private:
	std::unordered_map<StableID, EntityId, StableIDHash> idToEntity_;
	std::unordered_map<EntityId, StableID> entityToId_;
};

} // namespace smvk::placement
