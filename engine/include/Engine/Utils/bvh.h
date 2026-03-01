#pragma once

#include <print>
#include <vector>
#include <cassert>
#include <limits>
#include <algorithm>
#include <cstdint>

//#include <Engine/Shared/common.h>
#include <Engine/Renderer/context.h>
#include <Engine/Scene/scene.h>


static inline glm::vec3 minVec(const glm::vec3& a, const glm::vec3& b) {
	return { std::min(a.x,b.x), std::min(a.y,b.y), std::min(a.z,b.z) };
}
static inline glm::vec3 maxVec(const glm::vec3& a, const glm::vec3& b) {
	return { std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z) };
}

struct Aabb {
	glm::vec3 mn, mx;

	static Aabb empty() {
		float inf = std::numeric_limits<float>::infinity();
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

struct PrimitiveInfo {
	Aabb bounds;
	glm::vec3 centroid;
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

struct BvhTree {
	std::vector<BvhNode> nodes;
	std::vector<uint32_t> primIndices; // permutation of primitives (triangles)
	std::vector<uint32_t> primToNode; // mapping to scene hierarchy slot
};

static Aabb rangeBounds(const std::vector<PrimitiveInfo>& prims,
	const std::vector<uint32_t>& primIndices,
	uint32_t first, uint32_t count)
{
	Aabb b = Aabb::empty();
	for (uint32_t i = 0; i < count; ++i)
		b.expand(prims[primIndices[first + i]].bounds);
	return b;
}

static Aabb rangeCentroidBounds(const std::vector<PrimitiveInfo>& prims,
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
    const std::vector<PrimitiveInfo>& prims,
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

    auto axisMin = [&](int ax) { return ax==0 ? cb.mn.x : ax==1 ? cb.mn.y : cb.mn.z; };
    auto axisMax = [&](int ax) { return ax==0 ? cb.mx.x : ax==1 ? cb.mx.y : cb.mx.z; };

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

            uint32_t leftChild  = buildRecursive(tree, prims, first, mid - first, maxLeafSize);
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

    uint32_t leftChild  = buildRecursive(tree, prims, first, mid - first, maxLeafSize);
    uint32_t rightChild = buildRecursive(tree, prims, mid, first + count - mid, maxLeafSize);

    tree.nodes[nodeIdx].leftFirst = leftChild;
    tree.nodes[nodeIdx].right = rightChild;

    return nodeIdx;
}


BvhTree treeFromScene(const Context* ctx, const Scene& scene) {
	BvhTree tree;

	std::vector<PrimitiveInfo> prims;
	prims.reserve(scene.hierarchy.size());
	tree.primToNode.reserve(scene.hierarchy.size());

	auto centroid = [](const glm::vec3& p1, const glm::vec3& p2) -> glm::vec3 {
		return {
			(p1.x + p2.x) * 0.5f,
			(p1.y + p2.y) * 0.5f,
			(p1.z + p2.z) * 0.5f
		};
		};

	printf("hierarchy.size=%zu meshForNode.size=%zu\n",
       scene.hierarchy.size(), scene.meshForNode.size());

	for (int i = 0; i < (int)scene.hierarchy.size(); i++) {
		auto it = scene.meshForNode.find(i);
		if (it == scene.meshForNode.end())
			continue;

		uint32_t m = it->second;
		const auto& mesh = ctx->meshes_.objects_[m];
		const glm::mat4& glob = scene.globalTransform[i];

		const glm::vec3 localMin = mesh.obj_.aabbMin;
		const glm::vec3 localMax = mesh.obj_.aabbMax;

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

		prims.push_back(PrimitiveInfo{
			{ wMin, wMax },
			centroid(wMin, wMax)
			});

		tree.primToNode.push_back((uint32_t)i);
	}

	const uint32_t N = (uint32_t)prims.size();
	tree.primIndices.resize(N);
	for (uint32_t i = 0; i < N; ++i) tree.primIndices[i] = i;

	tree.nodes.reserve(N * 2); // rough
	if (N > 0) {
		buildRecursive(tree, prims, 0, N, /*maxLeafSize=*/4);
	}

	return tree;
}

void getFrustumPlanes(glm::mat4 vp, glm::vec4* planes) {
	using glm::vec4;
	vp = glm::transpose(vp);
	planes[0] = vec4(vp[3] + vp[0]); // left
	planes[1] = vec4(vp[3] - vp[0]); // right
	planes[2] = vec4(vp[3] + vp[1]); // bottom
	planes[3] = vec4(vp[3] - vp[1]); // top
	planes[4] = vec4(vp[3] + vp[2]); // near
	planes[5] = vec4(vp[3] - vp[2]); // far
}

void getFrustumCorners(
	glm::mat4 vp, glm::vec4* points)
{
	using glm::vec4;
	const vec4 corners[] = {
	vec4(-1, -1, -1, 1), vec4(1, -1, -1, 1),
	vec4(1, 1, -1, 1), vec4(-1, 1, -1, 1),
	vec4(-1, -1, 1, 1), vec4(1, -1, 1, 1),
	vec4(1, 1, 1, 1), vec4(-1, 1, 1, 1)
	};
	const glm::mat4 invVP = glm::inverse(vp);
	for (int i = 0; i != 8; i++) {
		const vec4 q = invVP * corners[i];
		points[i] = q / q.w;
	}
}

static inline bool aabbOutsidePlane(const Aabb& b, const glm::vec4& pl)
{
	// Pick vertex farthest along plane normal
	glm::vec3 v;
	v.x = (pl.x >= 0.0f) ? b.mx.x : b.mn.x;
	v.y = (pl.y >= 0.0f) ? b.mx.y : b.mn.y;
	v.z = (pl.z >= 0.0f) ? b.mx.z : b.mn.z;

	// If the farthest point is still outside, box is fully outside
	float s = pl.x * v.x + pl.y * v.y + pl.z * v.z + pl.z;
	return s < 0.0f;
}

inline bool aabbInFrustum(
	const glm::vec4* frustumPlanes,
	const glm::vec4* frustumCorners,
	const Aabb& box)
{
	using glm::dot;

	for (int i = 0; i < 6; i++) {
		int r = 0;
		r += (dot(frustumPlanes[i], glm::vec4(box.mn.x, box.mn.y, box.mn.z, 1.0f)) < 0.0) ? 1 : 0;
		r += (dot(frustumPlanes[i], glm::vec4(box.mx.x, box.mn.y, box.mn.z, 1.0f)) < 0.0) ? 1 : 0;
		r += (dot(frustumPlanes[i], glm::vec4(box.mn.x, box.mx.y, box.mn.z, 1.0f)) < 0.0) ? 1 : 0;
		r += (dot(frustumPlanes[i], glm::vec4(box.mx.x, box.mx.y, box.mn.z, 1.0f)) < 0.0) ? 1 : 0;
		r += (dot(frustumPlanes[i], glm::vec4(box.mn.x, box.mn.y, box.mx.z, 1.0f)) < 0.0) ? 1 : 0;
		r += (dot(frustumPlanes[i], glm::vec4(box.mx.x, box.mn.y, box.mx.z, 1.0f)) < 0.0) ? 1 : 0;
		r += (dot(frustumPlanes[i], glm::vec4(box.mn.x, box.mx.y, box.mx.z, 1.0f)) < 0.0) ? 1 : 0;
		r += (dot(frustumPlanes[i], glm::vec4(box.mx.x, box.mx.y, box.mx.z, 1.0f)) < 0.0) ? 1 : 0;
		if (r == 8)
			return false;
	}

	// check frustum outside/inside box
	int r = 0;
	r = 0;
	for (int i = 0; i < 8; i++)
		r += ((frustumCorners[i].x > box.mx.x) ? 1 : 0);
	if (r == 8)
		return false;
	r = 0;
	for (int i = 0; i < 8; i++)
		r += ((frustumCorners[i].x < box.mn.x) ? 1 : 0);
	if (r == 8)
		return false;
	r = 0;
	for (int i = 0; i < 8; i++)
		r += ((frustumCorners[i].y > box.mx.y) ? 1 : 0);
	if (r == 8)
		return false;
	r = 0;
	for (int i = 0; i < 8; i++)
		r += ((frustumCorners[i].y < box.mn.y) ? 1 : 0);
	if (r == 8)
		return false;
	r = 0;
	for (int i = 0; i < 8; i++)
		r += ((frustumCorners[i].z > box.mx.z) ? 1 : 0);
	if (r == 8)
		return false;
	r = 0;
	for (int i = 0; i < 8; i++)
		r += ((frustumCorners[i].z < box.mn.z) ? 1 : 0);
	if (r == 8)
		return false;

	return true;
}

void frustumCullBVH(
	const BvhTree& tree,
    int rootIndex,
    const glm::vec4 planes[6],
    const glm::vec4 corners[8],
    //std::vector<int>& outVisibleNodeIndices // Out
	std::vector<int>& outVisiblePrimIndices
){
    outVisiblePrimIndices.clear();
    if (rootIndex < 0) return;

    std::vector<int> stack;
    stack.reserve(128);
    stack.push_back(rootIndex);

    while (!stack.empty()) {
        int ni = stack.back();
        stack.pop_back();

        const BvhNode& n = tree.nodes[ni];
        if (!aabbInFrustum(planes, corners, n.bounds)) continue;

        if (n.isLeaf()) {
			for (uint32_t i = 0; i < n.count; ++i) {
				uint32_t primId = tree.primIndices[n.leftFirst + i];
				outVisiblePrimIndices.push_back(primId);
				//uint32_t nodeId = tree.primToNode[primId];
				//outVisibleNodeIndices.push_back(nodeId);
			}
        } else {
            if (n.leftFirst  >= 0) stack.push_back(n.leftFirst);
            if (n.right >= 0) stack.push_back(n.right);
        }
    }
}
