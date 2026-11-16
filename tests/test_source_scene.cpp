// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <gltf_import/source_scene.h>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

static void Require(bool condition) {
    if (!condition) {
        std::cerr << "SourceScene validation test failed\n";
        std::exit(1);
    }
}

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
    Require(Validate(s, error) && error.empty());
    p.indices[2] = 3;
    Require(!Validate(s, error) && !error.empty());
    p.indices[2] = 2;
    p.vertices[0].position.x = std::numeric_limits<float>::infinity();
    Require(!Validate(s, error));
    p.vertices[0].position.x = 0;
    s.nodes[0].children.push_back(0);
    s.nodes[0].parent = 0;
    Require(!Validate(s, error));
    s.nodes[0].children.clear();
    s.nodes[0].parent = InvalidIndex;
    Require(Validate(s, error));
    s.nodes.emplace_back();
    Require(!Validate(s, error)); // dangling/unreachable node
    return 0;
}
