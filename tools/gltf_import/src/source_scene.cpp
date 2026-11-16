// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <gltf_import/source_scene.h>
#include <cmath>
#include <functional>
#include <unordered_set>

namespace smvk::source {
namespace {
bool Finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool Finite(const glm::vec4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) &&
           std::isfinite(v.z) && std::isfinite(v.w);
}
}

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
        if (color[i] == 1) return fail("node cycle");
        if (color[i] == 2) return fail("duplicate node reference");
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
            if (!visit(child)) return false;
        }
        color[i] = 2;
        return true;
    };
    for (uint32_t root : scene.roots)
        if (!visit(root)) return false;
    for (size_t i = 0; i < scene.nodes.size(); ++i)
        if (!reached[i]) return fail("unreachable node");

    for (const auto& mesh : scene.meshes)
        for (const auto& prim : mesh.primitives) {
            if (prim.indices.size() % 3 != 0)
                return fail("non-triangle index stream");
            if (!Finite(prim.aabbMin) || !Finite(prim.aabbMax) ||
                glm::any(glm::greaterThan(prim.aabbMin, prim.aabbMax)))
                return fail("invalid primitive bounds");
            for (const auto& v : prim.vertices) {
                if (!Finite(v.position) || !Finite(v.normal) ||
                    !Finite(v.tangent) || !Finite(v.color) ||
                    !std::isfinite(v.uv0.x) || !std::isfinite(v.uv0.y) ||
                    !std::isfinite(v.uv1.x) || !std::isfinite(v.uv1.y))
                    return fail("nonfinite vertex attribute");
            }
            for (uint32_t index : prim.indices)
                if (index >= prim.vertices.size())
                    return fail("index out of bounds");
        }
    return true;
}
} // namespace smvk::source
