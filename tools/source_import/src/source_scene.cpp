// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <source_import/source_scene.h>
#include <unordered_map>
#include <unordered_set>

namespace smvk::source {
namespace {
bool Finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool Finite(const glm::vec4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
} // namespace

bool Validate(const Scene& scene, std::string& error) {
    error.clear();
    auto fail = [&](const std::string& reason) {
        error = reason;
        return false;
    };
    std::vector<uint8_t> color(scene.nodes.size(), 0);
    std::vector<uint8_t> reached(scene.nodes.size(), 0);
    for (uint32_t root : scene.roots) {
        if (root >= scene.nodes.size() || scene.nodes[root].parent != InvalidIndex)
            return fail("invalid root");
    }
    std::function<bool(uint32_t)> visit = [&](uint32_t i) {
        if (color[i] == 1)
            return fail("node cycle");
        if (color[i] == 2)
            return fail("duplicate node reference");
        color[i] = 1;
        reached[i] = 1;
        const auto& n = scene.nodes[i];
        if (n.mesh < -1 || (n.mesh >= 0 && size_t(n.mesh) >= scene.meshes.size()))
            return fail("invalid mesh reference");
        for (unsigned c = 0; c < 4; ++c)
            if (!Finite(n.localTransform[c]))
                return fail("nonfinite node transform");
        std::unordered_set<uint32_t> children;
        for (uint32_t child : n.children) {
            if (child >= scene.nodes.size() || scene.nodes[child].parent != i)
                return fail("invalid child or parent link");
            if (!children.insert(child).second)
                return fail("duplicate child");
            if (!visit(child))
                return false;
        }
        color[i] = 2;
        return true;
    };
    for (uint32_t root : scene.roots)
        if (!visit(root))
            return false;
    for (size_t i = 0; i < scene.nodes.size(); ++i)
        if (!reached[i])
            return fail("unreachable node");

    for (const auto& mesh : scene.meshes)
        for (const auto& prim : mesh.primitives) {
            if (prim.indices.size() % 3 != 0)
                return fail("non-triangle index stream");
            if (!Finite(prim.aabbMin) || !Finite(prim.aabbMax) ||
                glm::any(glm::greaterThan(prim.aabbMin, prim.aabbMax)))
                return fail("invalid primitive bounds");
            for (const auto& v : prim.vertices) {
                if (!Finite(v.position) || !Finite(v.normal) || !Finite(v.tangent) ||
                    !Finite(v.color) || !std::isfinite(v.uv0.x) || !std::isfinite(v.uv0.y) ||
                    !std::isfinite(v.uv1.x) || !std::isfinite(v.uv1.y))
                    return fail("nonfinite vertex attribute");
            }
            for (uint32_t index : prim.indices)
                if (index >= prim.vertices.size())
                    return fail("index out of bounds");
        }
    return true;
}

ScenePartition PartitionScene(const Scene& scene, size_t maxMeshesPerPart, float cellSizeMeters) {
    ScenePartition result;
    if (maxMeshesPerPart == 0 || scene.meshes.size() <= maxMeshesPerPart) {
        result.parts.push_back(scene);
        result.unchanged = true;
        return result;
    }

    // World transforms, top-down (parent before child). The scene is validated
    // (no cycles), so a single DFS per root covers the reachable hierarchy.
    std::vector<glm::mat4> world(scene.nodes.size(), glm::mat4(1.0f));
    {
        std::vector<uint8_t> visited(scene.nodes.size(), 0);
        std::function<void(uint32_t, const glm::mat4&)> dfs = [&](uint32_t i,
                                                                  const glm::mat4& parentWorld) {
            if (visited[i])
                return;
            visited[i] = 1;
            world[i] = parentWorld * scene.nodes[i].localTransform;
            for (uint32_t c : scene.nodes[i].children)
                dfs(c, world[i]);
        };
        const auto& roots = scene.activeRoots.empty() ? scene.roots : scene.activeRoots;
        for (uint32_t r : roots)
            dfs(r, glm::mat4(1.0f));
        for (size_t i = 0; i < scene.nodes.size(); ++i)
            if (!visited[i])
                dfs(uint32_t(i), glm::mat4(1.0f));
    }

    // Pack the 3D cell of a point into one 64-bit sort key (21 signed bits per
    // axis — a 256m grid spans ±268,435 km, far beyond any authored world).
    const float cell = cellSizeMeters > 0.0f ? cellSizeMeters : 256.0f;
    auto cellKey = [&](const glm::vec3& p) -> uint64_t {
        const int64_t x = int64_t(std::floor(double(p.x) / double(cell)));
        const int64_t y = int64_t(std::floor(double(p.y) / double(cell)));
        const int64_t z = int64_t(std::floor(double(p.z) / double(cell)));
        return (uint64_t(x) & 0x1FFFFFu) | ((uint64_t(y) & 0x1FFFFFu) << 21) |
               ((uint64_t(z) & 0x1FFFFFu) << 42);
    };
    auto meshCenter = [&](size_t mi) -> glm::vec3 {
        glm::vec3 mn(std::numeric_limits<float>::max());
        glm::vec3 mx(std::numeric_limits<float>::lowest());
        bool any = false;
        for (const auto& prim : scene.meshes[mi].primitives) {
            mn = glm::min(mn, prim.aabbMin);
            mx = glm::max(mx, prim.aabbMax);
            any = true;
        }
        return any ? (mn + mx) * 0.5f : glm::vec3(0.0f);
    };
    std::vector<uint64_t> meshCell(scene.meshes.size());
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi)
        meshCell[mi] = cellKey(meshCenter(mi));

    // Order meshes by cell (stable), then chunk to the capacity bound. Meshes
    // are never duplicated: each appears in exactly one chunk.
    std::vector<size_t> order(scene.meshes.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(
        order.begin(), order.end(), [&](size_t a, size_t b) { return meshCell[a] < meshCell[b]; });
    std::vector<std::vector<size_t>> chunks;
    for (size_t i = 0; i < order.size(); i += maxMeshesPerPart) {
        const size_t end = std::min(order.size(), i + maxMeshesPerPart);
        chunks.emplace_back(order.begin() + ptrdiff_t(i), order.begin() + ptrdiff_t(end));
    }

    result.parts.resize(chunks.size());
    for (size_t ci = 0; ci < chunks.size(); ++ci) {
        const std::vector<size_t>& chunk = chunks[ci];
        Scene& part = result.parts[ci];
        // Shared tables copied verbatim (stable indices across parts).
        part.materials = scene.materials;
        part.images = scene.images;
        part.textures = scene.textures;
        part.samplers = scene.samplers;
        part.skins = scene.skins;
        part.animations = scene.animations;
        part.lights = scene.lights;
        part.dependencies = scene.dependencies;
        // The part's meshes (full index -> local index).
        std::unordered_map<size_t, uint32_t> meshLocal;
        meshLocal.reserve(chunk.size() * 2);
        part.meshes.reserve(chunk.size());
        for (size_t li = 0; li < chunk.size(); ++li) {
            part.meshes.push_back(scene.meshes[chunk[li]]);
            meshLocal[chunk[li]] = uint32_t(li);
        }
        // A synthetic root + the flattened instances (world transform).
        part.nodes.emplace_back();
        part.nodes[0].name = "part" + std::to_string(ci);
        part.nodes[0].parent = InvalidIndex;
        part.nodes[0].localTransform = glm::mat4(1.0f);
        part.roots = {0};
        part.activeRoots = {0};
        for (size_t ni = 0; ni < scene.nodes.size(); ++ni) {
            const Node& src = scene.nodes[ni];
            if (src.mesh < 0)
                continue;
            const auto it = meshLocal.find(size_t(src.mesh));
            if (it == meshLocal.end())
                continue; // the mesh lives in a different part
            Node inst;
            inst.name = src.name;
            inst.mesh = int32_t(it->second);
            inst.skin = src.skin;
            inst.localTransform = world[ni];
            inst.parent = 0;
            part.nodes[0].children.push_back(uint32_t(part.nodes.size()));
            part.nodes.push_back(std::move(inst));
        }
    }
    return result;
}
} // namespace smvk::source
