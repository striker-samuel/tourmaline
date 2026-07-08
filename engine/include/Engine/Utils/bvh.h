#pragma once

#include <functional>
#include <vector>
#include <cassert>
#include <limits>
#include <algorithm>
#include <cstdint>

#include <Engine/Scene/scene.h>
#include <Engine/Vulkan/core.h>

// Dont like this. jsut used for bounds.
// maybe a fucntiopmn arg works better
#include <Engine/Renderer/ToyResourceManager.h>


static inline glm::vec3 minVec(const glm::vec3& a, const glm::vec3& b) {
	return { std::min(a.x,b.x), std::min(a.y,b.y), std::min(a.z,b.z) };
}
static inline glm::vec3 maxVec(const glm::vec3& a, const glm::vec3& b) {
	return { std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z) };
}

struct Aabb {
	glm::vec3 mn, mx;

	static Aabb empty() {
		constexpr float inf = std::numeric_limits<float>::infinity();
		return { { inf, inf, inf }, { -inf, -inf, -inf } };
	}

	void expand(const glm::vec3& p) {
		mn = minVec(mn, p);
		mx = maxVec(mx, p);
	}

	void expand(const Aabb& b) {
		mn = minVec(mn, b.mn);
		mx = maxVec(mx, b.mx);
	}

	glm::vec3 centroid() const {
		return { (mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f };
	}

	int longestAxis() const {
		glm::vec3 e{ mx.x - mn.x, mx.y - mn.y, mx.z - mn.z };
		if (e.x >= e.y && e.x >= e.z) return 0;
		if (e.y >= e.x && e.y >= e.z) return 1;
		return 2;
	}
};

struct Partition {
	Aabb bounds;
	glm::vec3 centroid;
	uint32_t node;
};

struct BvhNode {
	Aabb bounds{};

	// If leaf: count > 0, and leftFirst is first primitive index in primIndices
	// If interior: count == 0, and leftFirst is left child node index, right is leftFirst+1 OR store explicit right
	uint32_t leftFirst = 0;
	uint32_t count = 0; // 0 => interior, >0 => leaf

	// Optional if you prefer explicit children:
	uint32_t right = 0;
	bool isLeaf() const { return count > 0; }
};

// Dont like vector types..
struct BvhTree {
	std::vector<BvhNode> nodes;
	std::vector<uint32_t> primIndices; // permutation of primitives (triangles)
	std::vector<uint32_t> primToNode; // mapping to scene hierarchy slot
};

static Aabb rangeBounds(const std::vector<Partition>& prims,
	const std::vector<uint32_t>& primIndices,
	uint32_t first, uint32_t count)
{
	Aabb b = Aabb::empty();
	for (uint32_t i = 0; i < count; ++i)
		b.expand(prims[primIndices[first + i]].bounds);
	return b;
}

static Aabb rangeCentroidBounds(const std::vector<Partition>& prims,
	const std::vector<uint32_t>& primIndices,
	uint32_t first, uint32_t count)
{
	Aabb b = Aabb::empty();
	for (uint32_t i = 0; i < count; ++i)
		b.expand(prims[primIndices[first + i]].centroid);
	return b;
}

static uint32_t buildRecursive(
	BvhTree& tree,
	const std::vector<Partition>& prims,
	uint32_t first,
	uint32_t count,
	uint32_t maxLeafSize = 4)
{
	const uint32_t nodeIdx = (uint32_t)tree.nodes.size();
	tree.nodes.push_back(BvhNode{});

	// Always re-fetch by index (avoid dangling references if vector reallocates)
	tree.nodes[nodeIdx].bounds = rangeBounds(prims, tree.primIndices, first, count);

	// Leaf condition
	if (count <= maxLeafSize) {
		tree.nodes[nodeIdx].leftFirst = first;
		tree.nodes[nodeIdx].count = count;
		return nodeIdx;
	}

	// Choose split axis by centroid bounds
	Aabb cb = rangeCentroidBounds(prims, tree.primIndices, first, count);

	// Optional sanity: if cb is invalid, make leaf
	auto finite3 = [](const glm::vec3& v) {
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		};
	if (!finite3(cb.mn) || !finite3(cb.mx)) {
		tree.nodes[nodeIdx].leftFirst = first;
		tree.nodes[nodeIdx].count = count;
		return nodeIdx;
	}

	int axis = cb.longestAxis();

	auto axisMin = [&](int ax) { return ax == 0 ? cb.mn.x : ax == 1 ? cb.mn.y : cb.mn.z; };
	auto axisMax = [&](int ax) { return ax == 0 ? cb.mx.x : ax == 1 ? cb.mx.y : cb.mx.z; };

	float cmin = axisMin(axis);
	float cmax = axisMax(axis);

	// Use epsilon, not ==, and try other axes before giving up
	const float eps = 1e-6f;
	if (fabsf(cmax - cmin) < eps) {
		int ax2 = (axis + 1) % 3;
		int ax3 = (axis + 2) % 3;

		if (fabsf(axisMax(ax2) - axisMin(ax2)) >= eps) axis = ax2;
		else if (fabsf(axisMax(ax3) - axisMin(ax3)) >= eps) axis = ax3;
		else {
			// Completely degenerate centroid bounds: still split by index to avoid 1-node BVH
			uint32_t mid = first + count / 2;
			tree.nodes[nodeIdx].count = 0;

			uint32_t leftChild = buildRecursive(tree, prims, first, mid - first, maxLeafSize);
			uint32_t rightChild = buildRecursive(tree, prims, mid, first + count - mid, maxLeafSize);

			tree.nodes[nodeIdx].leftFirst = leftChild;
			tree.nodes[nodeIdx].right = rightChild;
			return nodeIdx;
		}

		cmin = axisMin(axis);
		cmax = axisMax(axis);
	}

	// Partition at median by centroid
	uint32_t mid = first + count / 2;

	auto centroidAxis = [&](uint32_t primId) -> float {
		const glm::vec3& c = prims[primId].centroid;
		return axis == 0 ? c.x : axis == 1 ? c.y : c.z;
		};

	std::nth_element(
		tree.primIndices.begin() + first,
		tree.primIndices.begin() + mid,
		tree.primIndices.begin() + first + count,
		[&](uint32_t a, uint32_t b) { return centroidAxis(a) < centroidAxis(b); }
	);

	// Create interior node
	tree.nodes[nodeIdx].count = 0;

	uint32_t leftChild = buildRecursive(tree, prims, first, mid - first, maxLeafSize);
	uint32_t rightChild = buildRecursive(tree, prims, mid, first + count - mid, maxLeafSize);

	tree.nodes[nodeIdx].leftFirst = leftChild;
	tree.nodes[nodeIdx].right = rightChild;

	return nodeIdx;
}

using MeshFromIndexFunc = std::function<Mesh(uint32_t meshIndex)>;

BvhTree treeFromScene(
	const Scene& scene,
	MeshFromIndexFunc meshFromMeshIndex
) {
	BvhTree tree;

	std::vector<Partition> prims;
	prims.reserve(scene.hierarchy.size());
	tree.primToNode.reserve(scene.hierarchy.size());

	auto centroid = [](const glm::vec3& p1, const glm::vec3& p2) -> glm::vec3 {
		return {
			(p1.x + p2.x) * 0.5f,
			(p1.y + p2.y) * 0.5f,
			(p1.z + p2.z) * 0.5f
		};
		};

	for (int i = 0; i < (int)scene.hierarchy.size(); i++) {

		auto it = scene.meshForNode.find(i);
		if (it == scene.meshForNode.end())
			continue;

		uint32_t m = it->second;
		Mesh mesh = meshFromMeshIndex(m);
		const glm::mat4& glob = scene.globalTransform[i];
		const glm::vec3 localMin = mesh.aabbMin;
		const glm::vec3 localMax = mesh.aabbMax;

		// Transform 8 corners to get correct world AABB under rotation/scale
		glm::vec3 worldMin(+FLT_MAX), worldMax(-FLT_MAX);

		auto expand = [&](const glm::vec3& p) {
			worldMin = glm::min(worldMin, p);
			worldMax = glm::max(worldMax, p);
			};

		const glm::vec3 corners[8] = {
			{localMin.x, localMin.y, localMin.z},
			{localMin.x, localMin.y, localMax.z},
			{localMin.x, localMax.y, localMin.z},
			{localMin.x, localMax.y, localMax.z},
			{localMax.x, localMin.y, localMin.z},
			{localMax.x, localMin.y, localMax.z},
			{localMax.x, localMax.y, localMin.z},
			{localMax.x, localMax.y, localMax.z},
		};

		for (const auto& c : corners) {
			glm::vec4 wp4 = glob * glm::vec4(c, 1.0f);
			expand(glm::vec3(wp4));
		}

		glm::vec3 wMin(worldMin.x, worldMin.y, worldMin.z);
		glm::vec3 wMax(worldMax.x, worldMax.y, worldMax.z);

		prims.push_back(
			Partition{
			{ wMin, wMax },
			centroid(wMin, wMax),
			(uint32_t)i
			}
			);


	}

	const uint32_t N = (uint32_t)prims.size();
	tree.primIndices.resize(N);
	for (uint32_t i = 0; i < N; ++i) tree.primIndices[i] = i;

	tree.nodes.reserve(N * 2); // rough
	if (N > 0) {
		buildRecursive(tree, prims, 0, N, /*maxLeafSize=*/4);
	}

	// Rebuild map
	tree.primToNode.resize(N);

	for (uint32_t slot = 0; slot < N; ++slot) {
		uint32_t originalPrimId = tree.primIndices[slot];
		tree.primToNode[slot] = prims[originalPrimId].node;
	}

	return tree;
}

static inline glm::vec4 normalizePlaneSafe(glm::vec4 p) {
	float len = glm::length(glm::vec3(p));
	return (len > 0.0f) ? (p / len) : p;
}

int getFrustumPlanes_ZO_InfiniteFar(
	const glm::mat4& VP,
	const glm::vec3& camPos,
	glm::vec4 planes[6]
)
{
	glm::vec4 r0 = glm::row(VP, 0);
	glm::vec4 r1 = glm::row(VP, 1);
	glm::vec4 r2 = glm::row(VP, 2);
	glm::vec4 r3 = glm::row(VP, 3);

	planes[0] = normalizePlaneSafe(r3 + r0); // left
	planes[1] = normalizePlaneSafe(r3 - r0); // right
	planes[2] = normalizePlaneSafe(r3 + r1); // bottom
	planes[3] = normalizePlaneSafe(r3 - r1); // top

	planes[4] = normalizePlaneSafe(r2);      // near: z >= 0 in ZO

	return 5; // infinite far => no far plane
}

inline bool aabbInFrustum(
	const glm::vec4* frustumPlanes,
	int frustumPlaneCount,
	const Aabb& box)
{
	for (int i = 0; i < frustumPlaneCount; ++i) {
		const glm::vec4 p = frustumPlanes[i];
		const glm::vec3 n(p.x, p.y, p.z);

		// pick the AABB vertex most in the direction of the plane normal
		glm::vec3 v = box.mn;
		if (n.x >= 0.0f) v.x = box.mx.x;
		if (n.y >= 0.0f) v.y = box.mx.y;
		if (n.z >= 0.0f) v.z = box.mx.z;

		// signed distance: dot(n, v) + d
		if (glm::dot(n, v) + p.w < 0.0f) {
			return false; // completely outside this plane
		}
	}
	return true; // inside/intersecting all planes
}


void frustumCullBVH(
	const BvhTree& tree,
	int rootIndex,
	const glm::vec4* planes,
	int planeCount,
	std::vector<int>& outVisiblePrims
) {
	outVisiblePrims.clear();
	if (rootIndex < 0) return;

	std::vector<int> stack;
	stack.reserve(128);
	stack.push_back(rootIndex);

	while (!stack.empty()) {
		int ni = stack.back();
		stack.pop_back();

		const BvhNode& n = tree.nodes[ni];

		if (!aabbInFrustum(planes, planeCount, n.bounds))
			continue;

		if (n.isLeaf()) {
			for (uint32_t i = 0; i < n.count; ++i) {
				uint32_t slot = n.leftFirst + i;
				outVisiblePrims.push_back((int)slot);
			}
		}
		else {
			stack.push_back((int)n.leftFirst);
			stack.push_back((int)n.right);
		}
	}
}

void frustumCullBVH(
	const BvhTree& tree,
	int rootIndex,
	const glm::mat4& viewProj,
	const glm::vec3 cameraPos,
	std::vector<int>& outVisiblePrims
) {

	glm::vec4 planes[5];

	auto planeCount = getFrustumPlanes_ZO_InfiniteFar(
		viewProj,
		cameraPos,
		planes
	);

	frustumCullBVH(
		tree,
		rootIndex,
		planes,
		planeCount,
		outVisiblePrims
	);

}

