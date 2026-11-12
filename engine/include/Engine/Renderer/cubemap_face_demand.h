// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Conservative CPU demand for independently streamed cubemap faces.
// This is a *request* mask, not permission to sample unresident faces:
// the renderer must use a complete coarse fallback (or defer publication)
// until all directions used by its shaders are covered.
//
// Unlike a visible cube's front three sides, IBL reflections can sample
// any direction. The caller must provide a bound on the actual sample cone;
// diffuse irradiance and unbounded rough-specular sampling request all six.

#include <Engine/Renderer/cubemap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace smvk {

using CubemapFaceMask = uint8_t;
inline constexpr CubemapFaceMask kAllCubemapFaces = 0x3fu;

[[nodiscard]] inline constexpr CubemapFaceMask CubemapFaceBit(CubemapFace face) {
    return CubemapFaceMask(1u << static_cast<uint8_t>(face));
}

// Returns a superset of the cube faces intersected by the spherical cap
// centered on 'direction' with angular radius 'halfAngleRadians'.
// A cube face covers at most acos(1/sqrt(3)) radians from its center
// (the four corners), so center separation > cone + corner radius
// proves the face cannot be sampled. False positives are intentional;
// false negatives would yield missing reflection samples.
//
// The direction is in cubemap lookup space (e.g. a *reflection* ray),
// NOT necessarily the camera-to-probe vector. An invalid cone or
// direction fails open to six faces. Zero radius is valid.
[[nodiscard]] inline CubemapFaceMask CubemapFacesForCone(
    const glm::vec3& direction, float halfAngleRadians) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kFaceCornerRadius = 0.9553166181245093f; // acos(1/sqrt(3))
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z) || !std::isfinite(halfAngleRadians) ||
        halfAngleRadians < 0.0f || halfAngleRadians > kPi)
        return kAllCubemapFaces;

    const float len2 = glm::dot(direction, direction);
    if (!std::isfinite(len2) || len2 <= 0.0f)
        return kAllCubemapFaces;
    const glm::vec3 axis = direction / std::sqrt(len2);
    if (halfAngleRadians + kFaceCornerRadius >= kPi)
        return kAllCubemapFaces;

    const float minDot = std::cos(halfAngleRadians + kFaceCornerRadius);
    CubemapFaceMask mask = 0;
    for (uint8_t i = 0; i < 6; ++i) {
        const auto c = CubemapFaceCenter(static_cast<CubemapFace>(i));
        if (glm::dot(axis, glm::vec3(c.x, c.y, c.z)) >= minDot - 1e-6f)
            mask |= CubemapFaceBit(static_cast<CubemapFace>(i));
    }
    return mask;
}

} // namespace smvk
