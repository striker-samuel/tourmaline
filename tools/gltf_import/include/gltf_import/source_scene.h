// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Format-independent, CPU-owned source scene. Source adapters decode into this
// representation; no glTF/OBJ/USD parser types or packed GPU structures live here.
// Phase 1: the live glTF geometry path consumes Primitive through ToMeshData.
// Materials, animation and source dispatch migrate in follow-up changes.
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace smvk::source {

inline constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec4 color{1.0f};
};

struct Primitive {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    int32_t material = -1;
    glm::vec3 aabbMin{0.0f};
    glm::vec3 aabbMax{0.0f};
    bool hasNormals = false;
    bool hasTangents = false;
    bool hasUv0 = false;
    bool hasUv1 = false;
    bool hasColors = false;
};

struct Mesh {
    std::string stableKey;
    std::string name;
    std::vector<Primitive> primitives;
};

struct Node {
    std::string stableKey;
    std::string name;
    uint32_t parent = InvalidIndex;
    std::vector<uint32_t> children;
    glm::mat4 localTransform{1.0f};
    int32_t mesh = -1;
    int32_t skin = -1;
    int32_t light = -1;
    bool staticRoot = false;
    bool noLightmap = false;
    bool ditherCloseup = false;
    bool staticLight = false;
};

struct Dependency {
    std::string relativePath;
};

struct Scene {
    std::vector<Node> nodes;
    std::vector<uint32_t> roots;
    std::vector<Mesh> meshes;
    std::vector<Dependency> dependencies;
};

// Validation is explicit and non-mutating: adapters must not silently
// publish truncated triangles, dangling node links or invalid indices.
// Returns false and fills error with the first reported invariant violation.
bool Validate(const Scene& scene, std::string& error);

} // namespace smvk::source
