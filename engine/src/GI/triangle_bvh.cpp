// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/GI/triangle_bvh.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>

#include <embree4/rtcore.h>

namespace smvk::gi {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr uint32_t kLeafSize = 8;
constexpr uint32_t kMaxDepth = 32;

inline float SafeSqrt(float v) { return v > 0.0f ? std::sqrt(v) : 0.0f; }

// Moeller-Trumbore, two-sided (returns true for backfaces; negative
// determinant handled by swapping the edge terms).
inline bool IntersectTriangle(
	const glm::vec3& origin, const glm::vec3& dir,
	const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
	float& outT, glm::vec2& outBaryUV) {
	const glm::vec3 e1 = v1 - v0;
	const glm::vec3 e2 = v2 - v0;
	const glm::vec3 p = glm::cross(dir, e2);
	const float det = glm::dot(e1, p);
	if (std::abs(det) < 1e-12f)
		return false;
	const float invDet = 1.0f / det;
	const glm::vec3 tv = origin - v0;
	const float u = glm::dot(tv, p) * invDet;
	if (u < 0.0f || u > 1.0f)
		return false;
	const glm::vec3 q = glm::cross(tv, e1);
	const float v = glm::dot(dir, q) * invDet;
	if (v < 0.0f || u + v > 1.0f)
		return false;
	const float t = glm::dot(e2, q) * invDet;
	if (t < 1e-5f)
		return false;
	outT = t;
	outBaryUV = glm::vec2(u, v);
	return true;
}

// Ericson, Real-Time Collision Detection: closest point on triangle.
inline glm::vec3 ClosestPointOnTriangle(
	const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
	const glm::vec3 ab = b - a;
	const glm::vec3 ac = c - a;
	const glm::vec3 ap = p - a;
	const float d1 = glm::dot(ab, ap);
	const float d2 = glm::dot(ac, ap);
	if (d1 <= 0.0f && d2 <= 0.0f)
		return a;

	const glm::vec3 bp = p - b;
	const float d3 = glm::dot(ab, bp);
	const float d4 = glm::dot(ac, bp);
	if (d3 >= 0.0f && d4 <= d3)
		return b;

	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
		const float denom = 1.0f / (d1 - d3);
		return a + ab * (d1 * denom);
	}

	const glm::vec3 cp = p - c;
	const float d5 = glm::dot(ab, cp);
	const float d6 = glm::dot(ac, cp);
	if (d6 >= 0.0f && d5 <= d6)
		return c;

	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
		const float denom = 1.0f / (d2 - d6);
		return a + ac * (d2 * denom);
	}

	const float va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
		const float denom = 1.0f / ((d4 - d3) + (d5 - d6));
		return b + (c - b) * ((d4 - d3) * denom);
	}

	const float denom = 1.0f / (va + vb + vc);
	const float v = vb * denom;
	const float w = vc * denom;
	return a + ab * v + ac * w;
}

inline bool SlabTest(
	const glm::vec3& origin, const glm::vec3& invDir,
	const float bmin[4], const float bmax[4], float maxDist) {
	float tmin = 0.0f;
	float tmax = maxDist;
	for (int a = 0; a < 3; ++a) {
		const float d0 = bmin[a] - origin[a];
		const float d1 = bmax[a] - origin[a];
		float t0 = d0 * invDir[a];
		float t1 = d1 * invDir[a];
		if (t0 > t1)
			std::swap(t0, t1);
		tmin = std::max(tmin, t0);
		tmax = std::min(tmax, t1);
		if (tmin > tmax)
			return false;
	}
	return true;
}

inline float PointAabbDistanceSq(const glm::vec3& p, const float bmin[4], const float bmax[4]) {
	float d = 0.0f;
	for (int a = 0; a < 3; ++a) {
		const float e = std::max(bmin[a] - p[a], std::max(0.0f, p[a] - bmax[a]));
		d += e * e;
	}
	return d;
}

} // namespace

// ---------------------------------------------------------------------------
// Embree backend (vendored external/embree — the production backend for the
// bake transport): packet/SIMD traversal over the same triangle soup
// (zero-copy shared buffers). Two-sided (no cull flag — matches the builtin
// Moeller-Trumbore convention), tnear = 1e-5 to match the builtin near
// cutoff, primID == build triangle index (single regular triangle
// geometry, order preserved). Thread-safe for concurrent queries (the
// bakers trace from thread pools); build once, then trace.
// ClosestDistance stays builtin (cold path: placement/relocation only —
// Embree has no point-distance query).
// ---------------------------------------------------------------------------

struct TriangleBvh::EmbreeContext {
	RTCDevice device = nullptr;
	RTCScene scene = nullptr;
	~EmbreeContext() {
		if (scene != nullptr)
			rtcReleaseScene(scene);
		if (device != nullptr)
			rtcReleaseDevice(device);
	}
};

TriangleBvh::TriangleBvh() = default;
TriangleBvh::~TriangleBvh() = default;
TriangleBvh::TriangleBvh(TriangleBvh&&) noexcept = default;
TriangleBvh& TriangleBvh::operator=(TriangleBvh&&) noexcept = default;

void TriangleBvh::Build(std::span<const glm::vec3> positions, std::span<const uint32_t> indices,
	TraceBackend backend) {
	positions_ = std::vector<glm::vec3>(positions.begin(), positions.end());
	indices_ = std::vector<uint32_t>(indices.begin(), indices.end());
	triCount_ = uint32_t(indices_.size() / 3);
	backend_ = TraceBackend::Builtin;
	embree_.reset();
	if (triCount_ == 0) {
		nodes_.clear();
		return;
	}
	// The builtin structures ALWAYS build (they back TriangleVertices,
	// TriangleNormal, ClosestDistance and bounds regardless of backend).
	triIndices_.resize(triCount_);
	for (uint32_t k = 0; k < triCount_; ++k)
		triIndices_[k] = k;
	nodes_.resize(std::max<size_t>(2 * triCount_, 2));
	usedNodes_ = 1;
	boundsMin_ = glm::vec3(kInf);
	boundsMax_ = glm::vec3(-kInf);
	BuildNode(0, 0, triCount_, 0);
	nodes_.resize(usedNodes_);
	if (backend != TraceBackend::Embree)
		return;
	// Embree is the production backend (vendored, always compiled in) —
	// the only fallback left is a runtime device/scene failure (driver
	// trouble), which keeps the builtin traversal live.
	auto ctx = std::make_unique<EmbreeContext>();
	ctx->device = rtcNewDevice(nullptr);
	if (ctx->device == nullptr || rtcGetDeviceError(ctx->device) != RTC_ERROR_NONE) {
		std::fprintf(stderr, "TriangleBvh: rtcNewDevice failed — builtin fallback\n");
		return;
	}
	RTCGeometry geom = rtcNewGeometry(ctx->device, RTC_GEOMETRY_TYPE_TRIANGLE);
	rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
		positions_.data(), 0, sizeof(glm::vec3), positions_.size());
	rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
		indices_.data(), 0, 3 * sizeof(uint32_t), triCount_);
	rtcCommitGeometry(geom);
	ctx->scene = rtcNewScene(ctx->device);
	rtcSetSceneBuildQuality(ctx->scene, RTC_BUILD_QUALITY_HIGH);
	rtcAttachGeometry(ctx->scene, geom);
	rtcReleaseGeometry(geom);
	rtcCommitScene(ctx->scene);
	if (rtcGetDeviceError(ctx->device) != RTC_ERROR_NONE) {
		std::fprintf(stderr, "TriangleBvh: Embree scene build failed — builtin fallback\n");
		return;
	}
	embree_ = std::move(ctx);
	backend_ = TraceBackend::Embree;
}

void TriangleBvh::UpdateNodeBounds(Node& node, uint32_t first, uint32_t count) {
	for (int a = 0; a < 3; ++a) {
		node.aabbMin[a] = kInf;
		node.aabbMax[a] = -kInf;
	}
	glm::vec3 v0, v1, v2;
	for (uint32_t k = 0; k < count; ++k) {
		TriangleVertices(triIndices_[first + k], v0, v1, v2);
		const glm::vec3 mn = glm::min(v0, glm::min(v1, v2));
		const glm::vec3 mx = glm::max(v0, glm::max(v1, v2));
		for (int a = 0; a < 3; ++a) {
			node.aabbMin[a] = std::min(node.aabbMin[a], mn[a]);
			node.aabbMax[a] = std::max(node.aabbMax[a], mx[a]);
		}
	}
	for (int a = 0; a < 3; ++a) {
		boundsMin_[a] = std::min(boundsMin_[a], node.aabbMin[a]);
		boundsMax_[a] = std::max(boundsMax_[a], node.aabbMax[a]);
	}
}

bool TriangleBvh::Split(uint32_t first, uint32_t count, uint32_t& axis, float& splitPos, uint32_t& mid) {
	// Binned SAH over the dominant extent (16 bins per axis). Returns the
	// spatial median when the SAH gain is not worth the cost model.
	glm::vec3 v0, v1, v2;
	glm::vec3 cmin(kInf), cmax(-kInf);
	std::vector<glm::vec3> centroids(count);
	for (uint32_t k = 0; k < count; ++k) {
		TriangleVertices(triIndices_[first + k], v0, v1, v2);
		const glm::vec3 c = (v0 + v1 + v2) * (1.0f / 3.0f);
		centroids[k] = c;
		cmin = glm::min(cmin, c);
		cmax = glm::max(cmax, c);
	}
	const glm::vec3 extent = cmax - cmin;
	axis = 0;
	if (extent.y > extent[axis])
		axis = 1;
	if (extent.z > extent[axis])
		axis = 2;
	if (extent[axis] < 1e-6f)
		return false;

	constexpr uint32_t kBins = 16;
	struct Bin {
		uint32_t count = 0;
		glm::vec3 bmin{ kInf, kInf, kInf };
		glm::vec3 bmax{ -kInf, -kInf, -kInf };
	};
	Bin bins[kBins];
	const float lo = cmin[axis];
	const float scale = float(kBins) / extent[axis];
	for (uint32_t k = 0; k < count; ++k) {
		const glm::vec3& c = centroids[k];
		uint32_t b = uint32_t((c[axis] - lo) * scale);
		b = std::min(b, kBins - 1);
		Bin& bin = bins[b];
		bin.count++;
		TriangleVertices(triIndices_[first + k], v0, v1, v2);
		bin.bmin = glm::min(bin.bmin, glm::min(v0, glm::min(v1, v2)));
		bin.bmax = glm::max(bin.bmax, glm::max(v0, glm::max(v1, v2)));
	}

	float bestCost = kInf;
	uint32_t bestSplit = 0;
	glm::vec3 nodeMin(kInf), nodeMax(-kInf);
	for (uint32_t k = 0; k < count; ++k) {
		TriangleVertices(triIndices_[first + k], v0, v1, v2);
		nodeMin = glm::min(nodeMin, glm::min(v0, glm::min(v1, v2)));
		nodeMax = glm::max(nodeMax, glm::max(v0, glm::max(v1, v2)));
	}
	const glm::vec3 nodeExtent = nodeMax - nodeMin;
	const float nodeArea = 2.0f
		* (nodeExtent.x * nodeExtent.y + nodeExtent.y * nodeExtent.z
			+ nodeExtent.z * nodeExtent.x);

	for (uint32_t s = 1; s < kBins; ++s) {
		glm::vec3 lmin(kInf), lmax(-kInf), rmin(kInf), rmax(-kInf);
		uint32_t lc = 0, rc = 0;
		for (uint32_t b = 0; b < s; ++b) {
			if (!bins[b].count)
				continue;
			lmin = glm::min(lmin, bins[b].bmin);
			lmax = glm::max(lmax, bins[b].bmax);
			lc += bins[b].count;
		}
		for (uint32_t b = s; b < kBins; ++b) {
			if (!bins[b].count)
				continue;
			rmin = glm::min(rmin, bins[b].bmin);
			rmax = glm::max(rmax, bins[b].bmax);
			rc += bins[b].count;
		}
		if (lc == 0 || rc == 0)
			continue;
		auto area = [](const glm::vec3& mn, const glm::vec3& mx) {
			const glm::vec3 e = glm::max(mx - mn, glm::vec3(1e-6f));
			return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
		};
		const float cost = 0.25f + (area(lmin, lmax) * float(lc) + area(rmin, rmax) * float(rc)) / std::max(nodeArea, 1e-6f);
		if (cost < bestCost) {
			bestCost = cost;
			bestSplit = s;
		}
	}
	if (bestSplit == 0)
		return false;

	const float split = lo + (float(bestSplit) / scale);
	auto midIter = std::partition(
		triIndices_.begin() + first, triIndices_.begin() + first + count,
		[&](uint32_t tri) {
			glm::vec3 a, b, c;
			TriangleVertices(tri, a, b, c);
			return ((a[axis] + b[axis] + c[axis]) * (1.0f / 3.0f)) < split;
		});
	mid = uint32_t(midIter - (triIndices_.begin() + first));
	if (mid == 0 || mid == count)
		return false;
	splitPos = split;
	return true;
}

void TriangleBvh::BuildNode(uint32_t nodeIndex, uint32_t first, uint32_t count, uint32_t depth) {
	Node& node = nodes_[nodeIndex];
	UpdateNodeBounds(node, first, count);
	node.leftFirst = first;
	node.triCount = count;

	if (count <= kLeafSize || depth >= kMaxDepth)
		return;

	uint32_t axis = 0, mid = 0;
	float splitPos = 0.0f;
	if (!Split(first, count, axis, splitPos, mid))
		return;

	// Inner node: allocate children then re-write the header (the node
	// vector never reallocates during recursion — it is pre-sized to
	// 2 * triCount, the worst case for a binary tree with single-tri
	// leaves).
	const uint32_t leftChild = usedNodes_++;
	const uint32_t rightChild = usedNodes_++;
	Node& hdr = nodes_[nodeIndex];
	hdr.leftFirst = leftChild;
	hdr.triCount = 0;

	BuildNode(leftChild, first, mid, depth + 1);
	BuildNode(rightChild, first + mid, count - mid, depth + 1);
}

bool TriangleBvh::Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist, Hit& out) const {
	if (triCount_ == 0)
		return false;
	if (backend_ == TraceBackend::Embree && embree_ != nullptr && embree_->scene != nullptr) {
		// Defensive finite check (matches the builtin's graceful miss on
		// degenerate input; Embree behavior on NaN rays is undefined).
		if (!std::isfinite(origin.x + origin.y + origin.z + dir.x + dir.y + dir.z))
			return false;
		RTCRayHit rh{};
		rh.ray.org_x = origin.x;
		rh.ray.org_y = origin.y;
		rh.ray.org_z = origin.z;
		rh.ray.dir_x = dir.x;
		rh.ray.dir_y = dir.y;
		rh.ray.dir_z = dir.z;
		rh.ray.tnear = 1e-5f; // builtin near cutoff
		rh.ray.tfar = maxDist;
		rh.ray.mask = 0xFFFFFFFFu;
		rh.ray.flags = 0u; // no culling: two-sided like the builtin
		rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
		rh.hit.primID = RTC_INVALID_GEOMETRY_ID;
		rtcIntersect1(embree_->scene, &rh);
		if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID
			|| rh.hit.primID == RTC_INVALID_GEOMETRY_ID)
			return false;
		if (!(rh.ray.tfar < maxDist))
			return false;
		out.t = rh.ray.tfar;
		out.tri = rh.hit.primID;
		out.baryUV = glm::vec2(rh.hit.u, rh.hit.v);
		return true;
	}
	glm::vec3 invDir(0.0f);
	invDir.x = std::abs(dir.x) > 1e-8f ? 1.0f / dir.x : kInf * (dir.x >= 0.0f ? 1.0f : -1.0f);
	invDir.y = std::abs(dir.y) > 1e-8f ? 1.0f / dir.y : kInf * (dir.y >= 0.0f ? 1.0f : -1.0f);
	invDir.z = std::abs(dir.z) > 1e-8f ? 1.0f / dir.z : kInf * (dir.z >= 0.0f ? 1.0f : -1.0f);

	float bestT = maxDist;
	uint32_t bestTri = 0;
	glm::vec2 bestBary(0.0f);
	glm::vec3 v0, v1, v2;

	uint32_t stack[64];
	int sp = 0;
	stack[sp++] = 0;
	while (sp > 0) {
		const uint32_t nodeIndex = stack[--sp];
		const Node& node = nodes_[nodeIndex];
		if (node.triCount > 0) {
			for (uint32_t k = 0; k < node.triCount; ++k) {
				const uint32_t tri = triIndices_[node.leftFirst + k];
				TriangleVertices(tri, v0, v1, v2);
				float t;
				glm::vec2 bary;
				if (IntersectTriangle(origin, dir, v0, v1, v2, t, bary) && t < bestT) {
					bestT = t;
					bestTri = tri;
					bestBary = bary;
				}
			}
			continue;
		}
		const uint32_t left = node.leftFirst;
		const uint32_t right = left + 1;
		const bool hitL = SlabTest(origin, invDir, nodes_[left].aabbMin, nodes_[left].aabbMax, bestT);
		const bool hitR = SlabTest(origin, invDir, nodes_[right].aabbMin, nodes_[right].aabbMax, bestT);
		// Push the farther child first so the nearer one is processed first.
		if (hitL && hitR) {
			stack[sp++] = right;
			stack[sp++] = left;
		} else if (hitL) {
			stack[sp++] = left;
		} else if (hitR) {
			stack[sp++] = right;
		}
	}
	if (bestT >= maxDist)
		return false;
	out.t = bestT;
	out.tri = bestTri;
	out.baryUV = bestBary;
	return true;
}

bool TriangleBvh::AnyHit(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const {
	if (triCount_ == 0)
		return false;
	if (backend_ == TraceBackend::Embree && embree_ != nullptr && embree_->scene != nullptr) {
		if (!std::isfinite(origin.x + origin.y + origin.z + dir.x + dir.y + dir.z))
			return false;
		RTCRay r{};
		r.org_x = origin.x;
		r.org_y = origin.y;
		r.org_z = origin.z;
		r.dir_x = dir.x;
		r.dir_y = dir.y;
		r.dir_z = dir.z;
		r.tnear = 1e-5f; // builtin near cutoff
		r.tfar = maxDist;
		r.mask = 0xFFFFFFFFu;
		r.flags = 0u; // no culling: two-sided like the builtin
		rtcOccluded1(embree_->scene, &r);
		// Occlusion sets tfar to -inf; untouched (>= 0) means no hit.
		return r.tfar < 0.0f;
	}
	glm::vec3 invDir(0.0f);
	invDir.x = std::abs(dir.x) > 1e-8f ? 1.0f / dir.x : kInf * (dir.x >= 0.0f ? 1.0f : -1.0f);
	invDir.y = std::abs(dir.y) > 1e-8f ? 1.0f / dir.y : kInf * (dir.y >= 0.0f ? 1.0f : -1.0f);
	invDir.z = std::abs(dir.z) > 1e-8f ? 1.0f / dir.z : kInf * (dir.z >= 0.0f ? 1.0f : -1.0f);

	glm::vec3 v0, v1, v2;
	uint32_t stack[64];
	int sp = 0;
	stack[sp++] = 0;
	while (sp > 0) {
		const uint32_t nodeIndex = stack[--sp];
		const Node& node = nodes_[nodeIndex];
		if (node.triCount > 0) {
			for (uint32_t k = 0; k < node.triCount; ++k) {
				const uint32_t tri = triIndices_[node.leftFirst + k];
				TriangleVertices(tri, v0, v1, v2);
				float t;
				glm::vec2 bary;
				if (IntersectTriangle(origin, dir, v0, v1, v2, t, bary) && t < maxDist)
					return true;
			}
			continue;
		}
		const uint32_t left = node.leftFirst;
		const uint32_t right = left + 1;
		if (SlabTest(origin, invDir, nodes_[left].aabbMin, nodes_[left].aabbMax, maxDist))
			stack[sp++] = left;
		if (SlabTest(origin, invDir, nodes_[right].aabbMin, nodes_[right].aabbMax, maxDist))
			stack[sp++] = right;
	}
	return false;
}

float TriangleBvh::ClosestDistance(const glm::vec3& point, glm::vec3* outClosestPoint) const {
	if (triCount_ == 0)
		return kInf;

	struct QueueItem {
		float distSq;
		uint32_t node;
		bool operator<(const QueueItem& o) const { return distSq > o.distSq; }
	};
	std::priority_queue<QueueItem> queue;
	queue.push({ PointAabbDistanceSq(point, nodes_[0].aabbMin, nodes_[0].aabbMax), 0 });

	float bestDistSq = kInf;
	glm::vec3 bestPoint(0.0f);
	glm::vec3 v0, v1, v2;

	while (!queue.empty()) {
		const QueueItem item = queue.top();
		queue.pop();
		if (item.distSq >= bestDistSq)
			break;
		const Node& node = nodes_[item.node];
		if (node.triCount > 0) {
			for (uint32_t k = 0; k < node.triCount; ++k) {
				const uint32_t tri = triIndices_[node.leftFirst + k];
				TriangleVertices(tri, v0, v1, v2);
				const glm::vec3 cp = ClosestPointOnTriangle(point, v0, v1, v2);
				const float dSq = glm::dot(cp - point, cp - point);
				if (dSq < bestDistSq) {
					bestDistSq = dSq;
					bestPoint = cp;
				}
			}
		} else {
			const uint32_t left = node.leftFirst;
			const uint32_t right = left + 1;
			const float dl = PointAabbDistanceSq(point, nodes_[left].aabbMin, nodes_[left].aabbMax);
			if (dl < bestDistSq)
				queue.push({ dl, left });
			const float dr = PointAabbDistanceSq(point, nodes_[right].aabbMin, nodes_[right].aabbMax);
			if (dr < bestDistSq)
				queue.push({ dr, right });
		}
	}
	if (outClosestPoint)
		*outClosestPoint = bestPoint;
	return SafeSqrt(bestDistSq);
}

} // namespace smvk::gi
