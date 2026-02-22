//#pragma once
//
//#include <vector>
//#include <limits>
//#include <algorithm>
//#include <cstdint>
//
//#include <Engine/Shared/common.h>
//
//struct PrimitiveInfo {
//    Aabb bounds;
//    Vec3 centroid;
//};
//
//using Vec3 = glm::vec3;
//
//static inline Vec3 minVec(const Vec3& a, const Vec3& b) {
//    return { std::min(a.x,b.x), std::min(a.y,b.y), std::min(a.z,b.z) };
//}
//static inline Vec3 maxVec(const Vec3& a, const Vec3& b) {
//    return { std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z) };
//}
//
//struct Aabb {
//    Vec3 mn, mx;
//
//    static Aabb empty() {
//        float inf = std::numeric_limits<float>::infinity();
//        return { { inf, inf, inf }, { -inf, -inf, -inf } };
//    }
//
//    void expand(const Vec3& p) {
//        mn = minVec(mn, p);
//        mx = maxVec(mx, p);
//    }
//
//    void expand(const Aabb& b) {
//        mn = minVec(mn, b.mn);
//        mx = maxVec(mx, b.mx);
//    }
//
//    Vec3 centroid() const {
//        return { (mn.x + mx.x)*0.5f, (mn.y + mx.y)*0.5f, (mn.z + mx.z)*0.5f };
//    }
//
//    int longestAxis() const {
//        Vec3 e{ mx.x - mn.x, mx.y - mn.y, mx.z - mn.z };
//        if (e.x >= e.y && e.x >= e.z) return 0;
//        if (e.y >= e.x && e.y >= e.z) return 1;
//        return 2;
//    }
//};
//
//struct BvhNode {
//    Aabb bounds{};
//
//    // If leaf: count > 0, and leftFirst is first primitive index in primIndices
//    // If interior: count == 0, and leftFirst is left child node index, right is leftFirst+1 OR store explicit right
//    uint32_t leftFirst = 0;
//    uint32_t count = 0; // 0 => interior, >0 => leaf
//
//    // Optional if you prefer explicit children:
//    uint32_t right = 0;
//    bool isLeaf() const { return count > 0; }
//};
//
//struct BvhTree {
//    std::vector<BvhNode> nodes;
//    std::vector<uint32_t> primIndices; // permutation of primitives (triangles)
//};
//
//
//static Aabb rangeBounds(const std::vector<PrimitiveInfo>& prims,
//                        const std::vector<uint32_t>& primIndices,
//                        uint32_t first, uint32_t count)
//{
//    Aabb b = Aabb::empty();
//    for (uint32_t i = 0; i < count; ++i)
//        b.expand(prims[primIndices[first + i]].bounds);
//    return b;
//}
//
//static Aabb rangeCentroidBounds(const std::vector<PrimitiveInfo>& prims,
//                                const std::vector<uint32_t>& primIndices,
//                                uint32_t first, uint32_t count)
//{
//    Aabb b = Aabb::empty();
//    for (uint32_t i = 0; i < count; ++i)
//        b.expand(prims[primIndices[first + i]].centroid);
//    return b;
//}
//
//static uint32_t buildRecursive(
//    BvhTree& tree,
//    const std::vector<PrimitiveInfo>& prims,
//    uint32_t first,
//    uint32_t count,
//    uint32_t maxLeafSize = 4)
//{
//    uint32_t nodeIdx = (uint32_t)tree.nodes.size();
//    tree.nodes.push_back(BvhNode{});
//    BvhNode& node = tree.nodes[nodeIdx];
//
//    node.bounds = rangeBounds(prims, tree.primIndices, first, count);
//
//    // Leaf condition
//    if (count <= maxLeafSize) {
//        node.leftFirst = first;
//        node.count = count;
//        return nodeIdx;
//    }
//
//    // Choose split axis by centroid bounds
//    Aabb cb = rangeCentroidBounds(prims, tree.primIndices, first, count);
//    int axis = cb.longestAxis();
//
//    // If centroids are all the same on this axis, just make a leaf (prevents bad splits)
//    float cmin = (axis == 0 ? cb.mn.x : axis == 1 ? cb.mn.y : cb.mn.z);
//    float cmax = (axis == 0 ? cb.mx.x : axis == 1 ? cb.mx.y : cb.mx.z);
//    if (cmin == cmax) {
//        node.leftFirst = first;
//        node.count = count;
//        return nodeIdx;
//    }
//
//    // Partition at median by centroid
//    uint32_t mid = first + count / 2;
//
//    auto centroidAxis = [&](uint32_t primId) -> float {
//        const Vec3& c = prims[primId].centroid;
//        return axis == 0 ? c.x : axis == 1 ? c.y : c.z;
//    };
//
//    std::nth_element(
//        tree.primIndices.begin() + first,
//        tree.primIndices.begin() + mid,
//        tree.primIndices.begin() + first + count,
//        [&](uint32_t a, uint32_t b) { return centroidAxis(a) < centroidAxis(b); }
//    );
//
//    // Create interior node
//    node.count = 0;
//    node.leftFirst = 0;
//    node.right = 0;
//
//    uint32_t leftChild  = buildRecursive(tree, prims, first, mid - first, maxLeafSize);
//    uint32_t rightChild = buildRecursive(tree, prims, mid, first + count - mid, maxLeafSize);
//
//    node.leftFirst = leftChild;
//    node.right = rightChild;
//
//    return nodeIdx;
//}
//
//#include <Engine/Renderer/context.h>
//#include <Engine/Scene/scene.h>
//
//BvhTree treeFromScene(Context* ctx, const Scene& scene) {
//    BvhTree tree;
//    assert(scene->changedAtThisFrame.size() == 0 && "needs to have global transforms updated for bvh build");
//
//    // 1) Extract primitives (triangles / instances / etc.)
//    //    You need: N primitives, each has bounds + centroid.
//    std::vector<PrimitiveInfo> prims;
//
//    for (int i = 0; i < scene.hierarchy.size(); i++) {
//        auto m = scene.meshForNode[i] ? scene.meshForNode[i] : continue;
//    }
//
//
//    // --- TODO: adapt to your Scene ---
//    // Example pseudo:
//    //
//    // for each mesh in scene:
//    //   for each triangle:
//    //      Vec3 p0,p1,p2 in world space
//    //      Aabb b = Aabb::empty(); b.expand(p0); b.expand(p1); b.expand(p2);
//    //      prims.push_back({ b, b.centroid() });
//    //
//    // Also store mapping from primId -> (meshId, triId) somewhere if needed.
//    // --------------------------------
//
//    const uint32_t N = (uint32_t)prims.size();
//    tree.primIndices.resize(N);
//    for (uint32_t i = 0; i < N; ++i) tree.primIndices[i] = i;
//
//    tree.nodes.reserve(N * 2); // rough
//    if (N > 0) {
//        buildRecursive(tree, prims, 0, N, /*maxLeafSize=*/4);
//    }
//
//    return tree;
//}
