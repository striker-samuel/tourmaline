// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// PlacementGraph point storage: structure-of-arrays on CPU, SoA GPU mirror.
// A point is cheap: placement references prototype geometry, never duplicates it.
// Scene NEVER stores these (see docs below) — they live in PlacementSystem's
// TileCache and reach the renderer as transient draws / GPU instance buffers.

#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Engine/Placement/placement_ids.h>

namespace tour::placement {

// CPU authoring/bake representation. Parallel arrays, same length.
struct PlacementPoints {
    std::vector<glm::vec3> position;
    std::vector<glm::quat> rotation;
    std::vector<glm::vec3> scale = {};
    std::vector<uint32_t> prototypeId;
    std::vector<StableID> stableId;
    std::vector<glm::vec4> customData; // biome/age/tint/flags packed per graph
    std::vector<uint32_t> flags;

    size_t size() const {
        return position.size();
    }
    void clear() {
        position.clear();
        rotation.clear();
        scale.clear();
        prototypeId.clear();
        stableId.clear();
        customData.clear();
        flags.clear();
    }
    void reserve(size_t n) {
        position.reserve(n);
        rotation.reserve(n);
        scale.reserve(n);
        prototypeId.reserve(n);
        stableId.reserve(n);
        customData.reserve(n);
        flags.reserve(n);
    }
    void push(const glm::vec3& p,
              const glm::quat& q,
              const glm::vec3& s,
              uint32_t proto,
              StableID id,
              const glm::vec4& custom = glm::vec4(0.0f),
              uint32_t f = 0) {
        position.push_back(p);
        rotation.push_back(q);
        scale.push_back(s);
        prototypeId.push_back(proto);
        stableId.push_back(id);
        customData.push_back(custom);
        flags.push_back(f);
    }
};

// Yaw (rotation about +Y, radians) -> normalized quaternion (xyzw). Matches the
// existing sincos(yaw) rotation convention in placement_instanced.vert.hlsl.
inline glm::vec4 QuatFromYaw(float yaw) {
    return glm::vec4(0.0f, -std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f));
}

// GPU instance record fed to the renderer (culling/LOD/indirect).
// Must match placement_common.hlsli PlacementRenderInstance (56 bytes):
// pos(12) pad(4) rotation(16) scale(12) proto(4) stableLow(4) customB(4).
// rotation is a full quaternion (xyzw) so baked vegetation keeps its
// terrain-aligned tilt; the procedural scatter path writes QuatFromYaw(yaw).
struct RenderInstance {
    glm::vec3 position{0.0f};                   // 12
    float padRotAlign = 0.0f;                   // 4  (aligns rotation to 16 in the GPU view)
    glm::vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f}; // 16 quaternion (xyzw)
    glm::vec3 scale{1.0f};                      // 12
    uint32_t prototypeId = 0;                   // 4
    uint32_t stableLow = 0; // 4  low 32 bits of StableID.lo (identity for overrides/debug)
    uint32_t customB = 0;   // 4  original candidateIndex (GPU constraint passes
                            //    recompute the full StableID without an ID buffer)
};
static_assert(sizeof(RenderInstance) == 56,
              "RenderInstance size changed — update placement_common.hlsli");

// Single scatter layer reference stored on ONE Scene node (params only).
// The Scene holds this; individual instances NEVER become Scene nodes.
struct PlacementRef {
    uint64_t programId = 0;    // which PlacementProgram to evaluate
    uint64_t seedOverride = 0; // 0 = use program seed
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    float densityScale = 1.0f;
    float tileSize = 64.0f; // world meters per runtime tile
};

} // namespace tour::placement

// COMPAT (the namespace/RHI boundary audit, todo item 8.4): this
// placement header's declarations live in tour::placement; the smvk::
// placement aliases keep the existing call sites compiling through the
// consumer migration.
namespace smvk::placement {
using ::tour::placement::PlacementPoints;
using ::tour::placement::PlacementRef;
using ::tour::placement::QuatFromYaw;
using ::tour::placement::RenderInstance;
} // namespace smvk::placement
