// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Format-independent, CPU-owned source scene. Source adapters decode into this
// representation; no glTF/OBJ/USD parser types or packed GPU structures live here.
// Phase 1: the live glTF geometry path consumes Primitive through ToMeshData.
// Materials, animation and source dispatch migrate in follow-up changes.
#include <array>
#include <optional>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace smvk::source {

inline constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

enum class AlphaMode { Opaque, Mask, Blend };

// Resolved authoring texture reference. Image and texture tables are owned by
// the source adapter; these indices are not GPU texture handles.
struct TextureRef {
    int textureIndex = -1;
    int imageIndex = -1;
    int samplerIndex = -1;
    int texCoord = 0;
    float scaleOrStrength = 1.0f;
    [[nodiscard]] bool valid() const {
        return textureIndex >= 0 && imageIndex >= 0;
    }
};

struct Material {
    std::string name;
    std::array<double, 4> baseColorFactor{1.0, 1.0, 1.0, 1.0};
    double metallicFactor = 1.0;
    double roughnessFactor = 1.0;
    std::optional<TextureRef> baseColorTex;
    std::optional<TextureRef> metallicRoughnessTex;
    std::optional<TextureRef> normalTex;
    std::optional<TextureRef> occlusionTex;
    std::optional<TextureRef> emissiveTex;
    std::array<double, 3> emissiveFactor{0.0, 0.0, 0.0};
    double emissiveStrength = 1.0;
    bool unlit = false;
    AlphaMode alphaMode = AlphaMode::Opaque;
    double alphaCutoff = 0.5;
    bool doubleSided = false;
    bool castsShadows = true;
    bool receiveContactShadows = true;
    bool clothSheen = false;
};

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
    std::vector<Material> materials;
    std::vector<Dependency> dependencies;
};

// Validation is explicit and non-mutating: adapters must not silently
// publish truncated triangles, dangling node links or invalid indices.
// Returns false and fills error with the first reported invariant violation.
bool Validate(const Scene& scene, std::string& error);

} // namespace smvk::source
