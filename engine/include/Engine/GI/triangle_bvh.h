#pragma once
// Triangle-level BVH for the GI baker: raycasting (primary/shadow rays) and
// closest-point distance queries (probe placement / relocation / brick
// refinement). The engine's Utils/bvh.h is a mesh-AABB BVH for CPU frustum
// culling; GI needs triangle-accurate intersection, so this is a separate,
// CPU-only acceleration structure (binned-SAH build, no GPU use).
//
// Conventions:
//  - Rays are two-sided (no backface culling): rooms and thin geometry must
//    be hit from both sides; the caller flips the geometric normal to face
//    the incoming ray for shading.
//  - ClosestDistance returns the unsigned distance to the closest point on
//    any triangle (0 when inside solid geometry; a signed test is the
//    caller's job, e.g. via ray-crossing parity).

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace smvk::gi {

class TriangleBvh {
public:
	struct Hit {
		float t = 0.0f;
		uint32_t tri = 0;      // triangle index (into the build triangle list)
		glm::vec2 baryUV{};    // barycentric (v, w); u = 1 - v - w
	};

	// Triangles are consumed as a prebuilt vertex + index soup; `triCount`
	// is indices.size() / 3. Triangle k uses indices[3k..3k+2].
	void Build(std::span<const glm::vec3> positions, std::span<const uint32_t> indices);

	bool Empty() const { return triCount_ == 0; }
	uint32_t TriangleCount() const { return triCount_; }

	// Vertex positions of triangle k (post-reorder indirection resolved).
	void TriangleVertices(uint32_t k, glm::vec3& v0, glm::vec3& v1, glm::vec3& v2) const {
		const uint32_t i = triIndices_[k] * 3;
		v0 = positions_[indices_[i + 0]];
		v1 = positions_[indices_[i + 1]];
		v2 = positions_[indices_[i + 2]];
	}

	glm::vec3 TriangleNormal(uint32_t k) const {
		glm::vec3 v0, v1, v2;
		TriangleVertices(k, v0, v1, v2);
		return glm::normalize(glm::cross(v1 - v0, v2 - v0));
	}

	// Closest hit along the ray (origin + t * dir). Returns false on miss.
	bool Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist, Hit& out) const;

	// Occlusion query with early-out: true when any triangle is hit before
	// maxDist. Cheaper than Raycast (no closest tracking, no bookkeeping).
	bool AnyHit(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const;

	// Unsigned distance from point to the closest triangle surface, plus the
	// closest point itself (optional out). Returns a large value when the
	// BVH is empty. Best-first traversal over node AABB lower bounds.
	float ClosestDistance(const glm::vec3& point, glm::vec3* outClosestPoint = nullptr) const;

private:
	// 40-byte node: padded float[4] AABB rows. Leaf: leftFirst = first
	// triangle index, triCount > 0. Inner: leftFirst = left child index
	// (right = left + 1), triCount = 0.
	struct Node {
		float aabbMin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float aabbMax[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		uint32_t leftFirst = 0;
		uint32_t triCount = 0;
	};
	static_assert(sizeof(Node) == 40);

	void BuildNode(uint32_t nodeIndex, uint32_t first, uint32_t count, uint32_t depth);
	bool Split(uint32_t first, uint32_t count, uint32_t& axis, float& splitPos, uint32_t& mid);
	void UpdateNodeBounds(Node& node, uint32_t first, uint32_t count);

	std::vector<glm::vec3> positions_;
	std::vector<uint32_t> indices_;   // original soup
	std::vector<uint32_t> triIndices_; // BVH-ordered triangle indirections
	std::vector<Node> nodes_;
	uint32_t usedNodes_ = 0;
	uint32_t triCount_ = 0;
	glm::vec3 boundsMin_{ 0.0f };
	glm::vec3 boundsMax_{ 0.0f };
};

} // namespace smvk::gi
