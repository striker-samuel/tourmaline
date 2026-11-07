// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include <Engine/Scene/scene.h>
// #include <Engine/App/cluster.h>

// (namespace/RHI boundary audit, todo 8.4) the BVH/culling math layer
// is ENGINE-OWNED and lives in tour:: (flat — the utils convention).
// The GLOBAL re-export bridge at the bottom of this file follows the
// scene_node_id.h pattern: the whole tree's bare/global-qualified
// `Aabb`/`BvhTree`/`::tour::Aabb` spellings keep resolving through it while
// call sites migrate; retire the bridge as they do. (Scene references
// inside resolve through scene.h's own global compat alias.)
namespace tour {
static inline glm::vec3 minVec(const glm::vec3& a, const glm::vec3& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
static inline glm::vec3 maxVec(const glm::vec3& a, const glm::vec3& b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

struct Aabb {
    glm::vec3 mn, mx;

    static Aabb empty() {
        constexpr float inf = std::numeric_limits<float>::infinity();
        return {{inf, inf, inf}, {-inf, -inf, -inf}};
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
        return {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
    }

    int longestAxis() const {
        glm::vec3 e{mx.x - mn.x, mx.y - mn.y, mx.z - mn.z};
        if (e.x >= e.y && e.x >= e.z)
            return 0;
        if (e.y >= e.x && e.y >= e.z)
            return 1;
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
    // If interior: count == 0, and leftFirst is left child node index, right is leftFirst+1 OR
    // store explicit right
    uint32_t leftFirst = 0;
    uint32_t count = 0; // 0 => interior, >0 => leaf

    // Optional if you prefer explicit children:
    uint32_t right = 0;
    bool isLeaf() const {
        return count > 0;
    }
};

// Dont like vector types..
struct BvhTree {
    std::vector<BvhNode> nodes;
    std::vector<uint32_t> primIndices; // permutation of primitives (triangles)
    std::vector<uint32_t> primToNode;  // mapping to scene hierarchy slot
};

static Aabb rangeBounds(const std::vector<Partition>& prims,
                        const std::vector<uint32_t>& primIndices,
                        uint32_t first,
                        uint32_t count) {
    Aabb b = Aabb::empty();
    for (uint32_t i = 0; i < count; ++i)
        b.expand(prims[primIndices[first + i]].bounds);
    return b;
}

static Aabb rangeCentroidBounds(const std::vector<Partition>& prims,
                                const std::vector<uint32_t>& primIndices,
                                uint32_t first,
                                uint32_t count) {
    Aabb b = Aabb::empty();
    for (uint32_t i = 0; i < count; ++i)
        b.expand(prims[primIndices[first + i]].centroid);
    return b;
}

static uint32_t buildRecursive(BvhTree& tree,
                               const std::vector<Partition>& prims,
                               uint32_t first,
                               uint32_t count,
                               uint32_t maxLeafSize = 4) {
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

        if (fabsf(axisMax(ax2) - axisMin(ax2)) >= eps)
            axis = ax2;
        else if (fabsf(axisMax(ax3) - axisMin(ax3)) >= eps)
            axis = ax3;
        else {
            // Completely degenerate centroid bounds: still split by index to avoid 1-node BVH
            uint32_t mid = first + count / 2;
            tree.nodes[nodeIdx].count = 0;

            uint32_t leftChild = buildRecursive(tree, prims, first, mid - first, maxLeafSize);
            uint32_t rightChild =
                buildRecursive(tree, prims, mid, first + count - mid, maxLeafSize);

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

    std::nth_element(tree.primIndices.begin() + first,
                     tree.primIndices.begin() + mid,
                     tree.primIndices.begin() + first + count,
                     [&](uint32_t a, uint32_t b) { return centroidAxis(a) < centroidAxis(b); });

    // Create interior node
    tree.nodes[nodeIdx].count = 0;

    uint32_t leftChild = buildRecursive(tree, prims, first, mid - first, maxLeafSize);
    uint32_t rightChild = buildRecursive(tree, prims, mid, first + count - mid, maxLeafSize);

    tree.nodes[nodeIdx].leftFirst = leftChild;
    tree.nodes[nodeIdx].right = rightChild;

    return nodeIdx;
}

using MeshFromIndexFunc = std::function<smvk::StaticMesh(uint32_t meshIndex)>;
// Return false to exclude a node (e.g. static/dynamic split).
using BvhNodeFilter = std::function<bool(uint32_t node)>;

inline BvhTree treeFromScene(const Scene& scene,
                             MeshFromIndexFunc meshFromMeshIndex,
                             BvhNodeFilter filter = {}) {
    BvhTree tree;

    std::vector<Partition> prims;
    prims.reserve(scene.hierarchy.size());
    tree.primToNode.reserve(scene.hierarchy.size());

    auto centroid = [](const glm::vec3& p1, const glm::vec3& p2) -> glm::vec3 {
        return {(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f, (p1.z + p2.z) * 0.5f};
    };

    for (int i = 0; i < (int)scene.hierarchy.size(); i++) {

        if (filter && !filter((uint32_t)i))
            continue;

        const MeshRow* meshRow = FindMesh(scene, NodeId(i));
        if (!meshRow)
            continue;

        uint32_t m = meshRow->mesh;
        smvk::StaticMesh mesh = meshFromMeshIndex(m);
        const glm::mat4& glob = scene.globalTransform[i];
        const glm::vec3 localMin = mesh.geometry.aabbMin;
        const glm::vec3 localMax = mesh.geometry.aabbMax;

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

        prims.push_back(Partition{{wMin, wMax}, centroid(wMin, wMax), (uint32_t)i});
    }

    const uint32_t N = (uint32_t)prims.size();
    tree.primIndices.resize(N);
    for (uint32_t i = 0; i < N; ++i)
        tree.primIndices[i] = i;

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

// Static/dynamic split: the static tree is built once (frozen subtrees +
// collapsed mega-mesh nodes) and never rebuilt; the dynamic tree is rebuilt
// (or refit) when dynamic transforms change. Both share the same builder.
struct SplitBvh {
    BvhTree statik;
    BvhTree dynamic;
};

inline SplitBvh splitBvhFromScene(const Scene& scene, MeshFromIndexFunc meshFromMeshIndex) {
    SplitBvh out;
    const bool hasStatic = scene.staticNode.size() == scene.hierarchy.size();
    auto isStatic = [&](uint32_t node) -> bool {
        return hasStatic && node < scene.staticNode.size() && scene.staticNode[node] != 0;
    };
    out.statik =
        treeFromScene(scene, meshFromMeshIndex, [&](uint32_t node) { return isStatic(node); });
    out.dynamic =
        treeFromScene(scene, meshFromMeshIndex, [&](uint32_t node) { return !isStatic(node); });
    return out;
}

static inline glm::vec4 normalizePlaneSafe(glm::vec4 p) {
    float len = glm::length(glm::vec3(p));
    return (len > 0.0f) ? (p / len) : p;
}

inline int getFrustumPlanes_ZO_InfiniteFar(const glm::mat4& VP,
                                           const glm::vec3& camPos,
                                           glm::vec4 planes[6]) {
    glm::vec4 r0 = glm::row(VP, 0);
    glm::vec4 r1 = glm::row(VP, 1);
    glm::vec4 r2 = glm::row(VP, 2);
    glm::vec4 r3 = glm::row(VP, 3);

    planes[0] = normalizePlaneSafe(r3 + r0); // left
    planes[1] = normalizePlaneSafe(r3 - r0); // right
    planes[2] = normalizePlaneSafe(r3 + r1); // bottom
    planes[3] = normalizePlaneSafe(r3 - r1); // top

    planes[4] = normalizePlaneSafe(r2); // near: z >= 0 in ZO

    return 5; // infinite far => no far plane
}

inline bool aabbInFrustum(const glm::vec4* frustumPlanes, int frustumPlaneCount, const Aabb& box) {
    for (int i = 0; i < frustumPlaneCount; ++i) {
        const glm::vec4 p = frustumPlanes[i];
        const glm::vec3 n(p.x, p.y, p.z);

        // pick the AABB vertex most in the direction of the plane normal
        glm::vec3 v = box.mn;
        if (n.x >= 0.0f)
            v.x = box.mx.x;
        if (n.y >= 0.0f)
            v.y = box.mx.y;
        if (n.z >= 0.0f)
            v.z = box.mx.z;

        // signed distance: dot(n, v) + d
        if (glm::dot(n, v) + p.w < 0.0f) {
            return false; // completely outside this plane
        }
    }
    return true; // inside/intersecting all planes
}

inline void frustumCullBVH(const BvhTree& tree,
                           int rootIndex,
                           const glm::vec4* planes,
                           int planeCount,
                           std::vector<int>& outVisiblePrims) {
    outVisiblePrims.clear();
    if (rootIndex < 0)
        return;

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
        } else {
            stack.push_back((int)n.leftFirst);
            stack.push_back((int)n.right);
        }
    }
}

inline void frustumCullBVH(const BvhTree& tree,
                           int rootIndex,
                           const glm::mat4& viewProj,
                           const glm::vec3 cameraPos,
                           std::vector<int>& outVisiblePrims) {

    glm::vec4 planes[5];

    auto planeCount = getFrustumPlanes_ZO_InfiniteFar(viewProj, cameraPos, planes);

    frustumCullBVH(tree, rootIndex, planes, planeCount, outVisiblePrims);
}

// ---------------------------------------------------------------------------
// COOKED-TREE WIRE FORMAT (the scene blob's static-cull section; see
// cluster_serializer.cpp). Little-endian, byte-identical roundtrip:
//   u32 primCount, u32 nodeCount,
//   u32 primIndices[primCount],
//   u32 primToNode[primCount]        (SCENE SLOT ids, dense == loaded slots),
//   per node: 6 x f32 (aabb mn/mx), u32 leftFirst, u32 count, u32 right.
// The reader fails closed on any out-of-range index or non-finite bound.
// ---------------------------------------------------------------------------
inline void BvhPut32(std::vector<uint8_t>& out, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
inline void BvhPutF(std::vector<uint8_t>& out, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    BvhPut32(out, bits);
}
inline bool SerializeBvh(const BvhTree& tree, std::vector<uint8_t>& out) {
    if (tree.nodes.size() > UINT32_MAX || tree.primIndices.size() > UINT32_MAX)
        return false;
    const uint32_t primCount = static_cast<uint32_t>(tree.primIndices.size());
    const uint32_t nodeCount = static_cast<uint32_t>(tree.nodes.size());
    if (tree.primToNode.size() != tree.primIndices.size())
        return false;
    BvhPut32(out, primCount);
    BvhPut32(out, nodeCount);
    for (uint32_t p : tree.primIndices)
        BvhPut32(out, p);
    for (uint32_t n : tree.primToNode)
        BvhPut32(out, n);
    for (const BvhNode& n : tree.nodes) {
        BvhPutF(out, n.bounds.mn.x);
        BvhPutF(out, n.bounds.mn.y);
        BvhPutF(out, n.bounds.mn.z);
        BvhPutF(out, n.bounds.mx.x);
        BvhPutF(out, n.bounds.mx.y);
        BvhPutF(out, n.bounds.mx.z);
        BvhPut32(out, n.leftFirst);
        BvhPut32(out, n.count);
        BvhPut32(out, n.right);
    }
    return true;
}
inline bool DeserializeBvh(std::span<const uint8_t> payload,
                           uint32_t primToNodeBound,
                           BvhTree& out,
                           std::string& err) {
    out = BvhTree{};
    if (payload.size() < 8) {
        err = "bvh section truncated";
        return false;
    }
    size_t off = 0;
    auto u32 = [&]() -> uint32_t {
        uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i)
            v |= uint32_t(payload[off + i]) << (i * 8);
        off += 4;
        return v;
    };
    auto f32 = [&]() -> float {
        const uint32_t bits = u32();
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    const uint32_t primCount = u32();
    const uint32_t nodeCount = u32();
    // Per node: 6 x f32 (24) + u32 leftFirst + u32 count + u32 right (12)
    // = 36 bytes.
    const size_t fixed = 8ull + size_t(primCount) * 8ull + size_t(nodeCount) * 36ull;
    // Zero-prim zero-node is the LEGITIMATE empty tree (the writer always
    // emits the section; scenes without static meshes carry {0,0}). Any
    // other zero/nonzero mix, oversize or byte-count mismatch is corrupt.
    if (primCount > 64u * 1024u * 1024u || nodeCount > 2u * primCount + 1u ||
        (nodeCount == 0) != (primCount == 0) || fixed != payload.size()) {
        err = "bvh section shape invalid (prims=" + std::to_string(primCount) +
              " nodes=" + std::to_string(nodeCount) + " fixed=" + std::to_string(fixed) +
              " size=" + std::to_string(payload.size()) + ")";
        return false;
    }
    if (primCount == 0) {
        if (off != payload.size()) {
            err = "bvh section trailing bytes";
            return false;
        }
        return true; // the empty tree
    }
    out.primIndices.resize(primCount);
    for (uint32_t i = 0; i < primCount; ++i) {
        out.primIndices[i] = u32();
        if (out.primIndices[i] >= primCount) {
            err = "bvh prim index out of range";
            return false;
        }
    }
    out.primToNode.resize(primCount);
    for (uint32_t i = 0; i < primCount; ++i) {
        out.primToNode[i] = u32();
        if (out.primToNode[i] >= primToNodeBound) {
            err = "bvh prim node id out of range";
            return false;
        }
    }
    out.nodes.resize(nodeCount);
    for (uint32_t i = 0; i < nodeCount; ++i) {
        BvhNode& n = out.nodes[i];
        n.bounds.mn = {f32(), f32(), f32()};
        n.bounds.mx = {f32(), f32(), f32()};
        n.leftFirst = u32();
        n.count = u32();
        n.right = u32();
        const bool finiteBounds = std::isfinite(n.bounds.mn.x) && std::isfinite(n.bounds.mn.y) &&
                                  std::isfinite(n.bounds.mn.z) && std::isfinite(n.bounds.mx.x) &&
                                  std::isfinite(n.bounds.mx.y) && std::isfinite(n.bounds.mx.z);
        const bool refsOk = n.isLeaf() ? (uint64_t(n.leftFirst) + n.count <= primCount)
                                       : (n.leftFirst < nodeCount && n.right < nodeCount);
        if (!finiteBounds || !refsOk) {
            err = "bvh node invalid";
            return false;
        }
    }
    if (off != payload.size()) {
        err = "bvh section trailing bytes";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The ADOPTION GUARD hash: one fold, used IDENTICALLY by the cook (over the
// dense-remapped source scene) and by the World at resolve time (over the
// loaded scene) — the bit-exactness contract that makes adopting a cooked
// tree safe. Covers everything the baked tree depends on:
//   - node count + every node's parent + LOCAL transform bytes (any
//     reparent/move/structure edit flips it; locals roundtrip through the
//     blob bit-exactly, globals do not need to),
//   - the static mesh-row SET: per row (node id + the mesh's geometry AABB
//     bytes — the exact bounds the tree bakes).
// Row combining is order-independent (per-row fnv1a64 added mod 2^64), so
// the cook's row order need not match the loaded scene's.
// ---------------------------------------------------------------------------
struct StaticCullRow {
    uint32_t node = 0;
    glm::vec3 aabbMin{0.0f};
    glm::vec3 aabbMax{0.0f};
};
inline uint64_t StaticCullRowFnv(const StaticCullRow& r) {
    uint64_t h = 14695981039346656037ull;
    auto fold = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    fold(&r.node, sizeof(r.node));
    fold(glm::value_ptr(r.aabbMin), sizeof(glm::vec3));
    fold(glm::value_ptr(r.aabbMax), sizeof(glm::vec3));
    return h;
}
// parents/locals are DENSE-ORDERED (the blob's node order == the loaded
// scene's slots): the cook folds the same bytes it writes (denseLink'd
// parents + export-view locals), the World folds its own loaded arrays —
// identical by construction. Row combining is order-independent (per-row
// fnv1a64 summed mod 2^64).
inline uint64_t StaticCullStateHash(const int32_t* parents,
                                    const glm::mat4* locals,
                                    uint32_t nodeCount,
                                    const StaticCullRow* rows,
                                    uint32_t rowCount) {
    uint64_t h = 14695981039346656037ull;
    auto fold = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    fold(&nodeCount, sizeof(nodeCount));
    for (uint32_t i = 0; i < nodeCount; ++i) {
        fold(&parents[i], sizeof(int32_t));
        fold(glm::value_ptr(locals[i]), sizeof(glm::mat4));
    }
    fold(&rowCount, sizeof(rowCount));
    uint64_t rowAcc = 0; // order-independent: sum of per-row hashes
    for (uint32_t i = 0; i < rowCount; ++i)
        rowAcc += StaticCullRowFnv(rows[i]);
    fold(&rowAcc, sizeof(rowAcc));
    return h;
}

} // namespace tour
