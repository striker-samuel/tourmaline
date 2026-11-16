// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <gltf_import/source_scene.h>
#include <cassert>
#include <limits>
#include <string>

int main() {
    using namespace smvk::source;
    Scene s;
    s.nodes.emplace_back();
    s.roots.push_back(0);
    s.meshes.emplace_back();
    s.nodes[0].mesh = 0;
    auto& p = s.meshes[0].primitives.emplace_back();
    p.vertices.resize(3);
    p.indices = {0, 1, 2};
    std::string error;
    assert(Validate(s, error) && error.empty());
    p.indices[2] = 3;
    assert(!Validate(s, error) && !error.empty());
    p.indices[2] = 2;
    p.vertices[0].position.x = std::numeric_limits<float>::infinity();
    assert(!Validate(s, error));
    p.vertices[0].position.x = 0;
    s.nodes[0].children.push_back(0);
    s.nodes[0].parent = 0;
    assert(!Validate(s, error));
    s.nodes[0].children.clear();
    s.nodes[0].parent = InvalidIndex;
    assert(Validate(s, error));
    s.nodes.emplace_back();
    assert(!Validate(s, error)); // dangling/unreachable node
    return 0;
}
