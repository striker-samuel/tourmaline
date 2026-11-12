// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Shared mesh-local eligibility for costly spatial derivatives (SDF grids
// and hierarchical LOD preparation). Small objects are treated as props;
// the decision is based on geometry rather than unstable asset naming.
// Bounds are evaluated in the imported mesh's local units, not per-instance
// transforms, so shared geometry receives the same decision everywhere.
#include <algorithm>
#include <cmath>
#include <cstddef>

#include <glm/glm.hpp>

namespace smvk {

// Longest local AABB edge in model units. A 2-unit span separates
// architectural / large scene geometry from small prop-like primitives.
// Use one rule for all derivative producers to avoid divergent content.
inline constexpr float kSpatialDerivativeMinExtent = 2.0f;

inline bool EligibleForSpatialDerivatives(const glm::vec3& aabbMin,
                                          const glm::vec3& aabbMax,
                                          size_t indexCount,
                                          bool skinned = false) {
    if (skinned || indexCount < 3 || indexCount % 3 != 0)
        return false;
    const glm::vec3 extent = aabbMax - aabbMin;
    return std::isfinite(extent.x) && std::isfinite(extent.y) &&
           std::isfinite(extent.z) &&
           extent.x >= 0.0f && extent.y >= 0.0f && extent.z >= 0.0f &&
           std::max({extent.x, extent.y, extent.z}) >= kSpatialDerivativeMinExtent;
}

} // namespace smvk
