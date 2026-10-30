// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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

#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace smvk::gi {

// Ray-tracing backend for the bake transport (primary/shadow rays): the
// vendored Embree (external/embree, packet/SIMD traversal over the same
// triangle soup — the fast production backend, THE backend: the builtin
// transport is DEPRECATED and marked for deletion) or the builtin
// binned-SAH BVH (the deterministic reference — deprecated for transport;
// it also owns ClosestDistance, which stays builtin either way — cold
// placement/relocation path).
enum class TraceBackend {
	Builtin, // DEPRECATED for the bake transport (deletion target)
	Embree,
};

class TriangleBvh {
public:
	// All five specials declared here, defined out-of-line (triangle_bvh.cpp):
	// the unique_ptr<EmbreeContext> member is incomplete in this header,
	// and an inline-defaulted special (MSVC instantiates a defaulted ctor
	// with its exception-cleanup path in every using TU) drags
	// ~unique_ptr — and its incomplete-type delete — into foreign TUs.
	TriangleBvh();
	~TriangleBvh();
	TriangleBvh(const TriangleBvh&) = delete;
	TriangleBvh& operator=(const TriangleBvh&) = delete;
	TriangleBvh(TriangleBvh&&) noexcept;
	TriangleBvh& operator=(TriangleBvh&&) noexcept;

	struct Hit {
		float t = 0.0f;
		uint32_t tri = 0;      // triangle index (into the build triangle list)
		glm::vec2 baryUV{};    // barycentric (v, w); u = 1 - v - w
	};

	// Triangles are consumed as a prebuilt vertex + index soup; `triCount`
	// is indices.size() / 3. Triangle k uses indices[3k..3k+2].
	void Build(std::span<const glm::vec3> positions, std::span<const uint32_t> indices,
		TraceBackend backend = TraceBackend::Builtin);

	TraceBackend Backend() const { return backend_; }

	bool Empty() const { return triCount_ == 0; }
	uint32_t TriangleCount() const { return triCount_; }

	// Vertex positions of triangle k (k = original triangle index into
	// the build soup; the BVH reorder table triIndices_ is resolved by
	// the CALLER — traversal passes triIndices_[slot], direct users pass
	// k itself. Never index triIndices_ here: doing so applies the
	// reorder twice and shades/traces the wrong triangle).
	void TriangleVertices(uint32_t k, glm::vec3& v0, glm::vec3& v1, glm::vec3& v2) const {
		const uint32_t i = k * 3;
		v0 = positions_[indices_[i + 0]];
		v1 = positions_[indices_[i + 1]];
		v2 = positions_[indices_[i + 2]];
	}

	glm::vec3 TriangleNormal(uint32_t k) const {
		glm::vec3 v0, v1, v2;
		TriangleVertices(k, v0, v1, v2);
		// Degenerate (zero-area) triangles normalize to NaN; one NaN
		// shading normal poisons the whole probe fit (rhs -> NaN amps),
		// the error metric (NaN error subdivides forever) and the
		// smoothing passes (NaN spreads to neighbors). Fall back to +Y.
		const glm::vec3 c = glm::cross(v1 - v0, v2 - v0);
		const float l2 = glm::dot(c, c);
		if (!(l2 > 1e-20f))
			return glm::vec3(0.0f, 1.0f, 0.0f);
		return c * (1.0f / std::sqrt(l2));
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
	// Embree traversal state (backend == Embree only): shares positions_/
	// indices_ zero-copy (rtcSetSharedGeometryBuffer — the vectors are
	// stable after Build, so the device pointers stay valid for the
	// BVH's lifetime). Forward-declared so this header never includes
	// embree.h (engine targets build without it).
	struct EmbreeContext;
	std::unique_ptr<EmbreeContext> embree_;
	TraceBackend backend_ = TraceBackend::Builtin;
};

} // namespace smvk::gi
