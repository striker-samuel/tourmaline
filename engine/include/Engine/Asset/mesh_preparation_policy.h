// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// The same eligibility rule is used for SDF and LOD sibling derivation.
// Evaluate the local geometry AABB under an INSTANCE's complete world
// transform: authored model units alone do not represent scene size.
// The derived payload stays mesh-local and is shared across instances.
#include <algorithm>
#include <cmath>
#include <cstddef>

#include <glm/glm.hpp>

namespace smvk {

// World-space threshold (metres in metre-authored scenes).
inline constexpr float kSpatialDerivativeMinWorldExtent = 2.0f;

// Exact AABB of a transformed local AABB, including rotations, nonuniform
// and negative scales, and shear. Translation does not affect its size.
inline float SpatialDerivativeWorldExtent(const glm::vec3& aabbMin,
                                          const glm::vec3& aabbMax,
                                          const glm::mat4& world) {
    const glm::vec3 extent = aabbMax - aabbMin;
    if (!std::isfinite(extent.x) || !std::isfinite(extent.y) || !std::isfinite(extent.z) ||
        extent.x < 0.0f || extent.y < 0.0f || extent.z < 0.0f)
        return 0.0f;
    glm::vec3 worldExtent(0.0f);
    for (int axis = 0; axis < 3; ++axis) {
        for (int component = 0; component < 3; ++component) {
            const float value = world[component][axis];
            if (!std::isfinite(value))
                return 0.0f;
            worldExtent[axis] += std::abs(value) * extent[component];
        }
        if (!std::isfinite(worldExtent[axis]))
            return 0.0f;
    }
    return std::max({worldExtent.x, worldExtent.y, worldExtent.z});
}

inline bool EligibleForSpatialDerivatives(const glm::vec3& aabbMin,
                                          const glm::vec3& aabbMax,
                                          size_t indexCount,
                                          const glm::mat4& world,
                                          bool skinned = false) {
    return !skinned && indexCount >= 3 && indexCount % 3 == 0 &&
           SpatialDerivativeWorldExtent(aabbMin, aabbMax, world) >=
               kSpatialDerivativeMinWorldExtent;
}

// Triangle-count floor for the GPU-driven mesh derivations (the meshlet bake
// and the LOD sibling chain). One meshlet holds up to kMeshletMaxTriangles,
// so a mesh at or under this budget is a single draw regardless — the
// GPU-driven path exists to CULL many meshlets — and a multi-level simplify
// chain over a handful of triangles yields nothing the per-candidate filter
// (below lodMinTriangles) would not already drop. Skipping both for such
// meshes is a pure cook-time win with no runtime difference (a chair leg
// does not need four LOD levels).
//
// LOWERED to 1: the task/mesh backend is ALL-OR-NOTHING — it requires every
// mesh to be registered (meshleted), and a sub-threshold mesh has no meshlet
// AND no runtime retained upload (that is import-time only), so it maps to a
// native-suffix geometry id that flips m_gpuSceneTaskGeometryIdsCompatible and
// disables the whole meshlet path. Mesheting every non-degenerate mesh keeps
// the task/mesh path engageable. (The proper follow-up is a MIXED path — mesh-
// leted meshes via task/mesh, sub-threshold meshes via indexed raster — instead
// of this blanket floor.)
inline constexpr size_t kGpuDrivenMeshMinTriangles = 1;

inline bool EligibleForGpuDrivenMesh(size_t triangleCount) {
    return triangleCount >= kGpuDrivenMeshMinTriangles;
}

} // namespace smvk
