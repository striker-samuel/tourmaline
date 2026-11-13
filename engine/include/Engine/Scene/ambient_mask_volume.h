// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// CPU reference for an ambient-mask volume. This intentionally reuses the
// shared SpatialVolumeBox convention; it does not define a second shape.
// Visibility is a baked, world-static field, independent of sky radiance.
// GPU textures, scene associations and streaming are separate consumers.

#include "Engine/Scene/spatial_volume.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tour::scene {

// Six signed LOCAL axes (+X,-X,+Y,-Y,+Z,-Z). Each value is in [0,1]:
// 1 = unoccluded sky, 0 = fully blocked. These are visibility factors,
// NOT bounced radiance or a replacement for the baked SG GI contribution.
struct AmbientMaskSample {
    float axis[6] = {1, 1, 1, 1, 1, 1};
};

// X-major tightly packed field, one sample per grid point; dimensions
// must each be >= 2. The caller owns 'samples' and keeps it alive.
struct AmbientMaskField {
    uint32_t nx = 0, ny = 0, nz = 0;
    std::span<const AmbientMaskSample> samples{};

    [[nodiscard]] bool Valid() const {
        if (nx < 2 || ny < 2 || nz < 2)
            return false;
        const size_t x = nx, y = ny, z = nz;
        if (x > SIZE_MAX / y || x * y > SIZE_MAX / z)
            return false;
        return samples.size() == x * y * z;
    }

    [[nodiscard]] const AmbientMaskSample& At(uint32_t x, uint32_t y, uint32_t z) const {
        return samples[(size_t(z) * ny + y) * nx + x];
    }
};

struct AmbientMaskVolume {
    SpatialVolumeBox box;
    AmbientMaskField field;
    float insideFade = 0.0f;
    float outsideFade = 0.0f;
    float intensity = 1.0f;
};

// Samples in the OBB's local coordinate frame. Values beyond the volume
// are handled by SpatialVolumeWeight (not extrapolation). Broken fields
// and invalid/non-finite inputs conservatively return full visibility.
[[nodiscard]] inline float AmbientMaskVisibility(const AmbientMaskVolume& volume,
                                                  const glm::vec3& worldPos,
                                                  const glm::vec3& worldDirection) {
    if (!volume.box.Valid() || !volume.field.Valid() ||
        !std::isfinite(worldPos.x) || !std::isfinite(worldPos.y) ||
        !std::isfinite(worldPos.z) || !std::isfinite(worldDirection.x) ||
        !std::isfinite(worldDirection.y) || !std::isfinite(worldDirection.z))
        return 1.0f;
    const float weight = SpatialVolumeWeight(volume.box, worldPos,
                                             volume.insideFade, volume.outsideFade);
    if (!(weight > 0.0f))
        return 1.0f;
    const float dirLen = glm::length(worldDirection);
    if (!(dirLen > 1e-6f) || !std::isfinite(dirLen))
        return 1.0f;

    const glm::vec3 offset = worldPos - volume.box.center;
    glm::vec3 uv;
    glm::vec3 localDirection;
    for (int i = 0; i < 3; ++i) {
        uv[i] = std::clamp(0.5f + 0.5f * glm::dot(offset, volume.box.axes[i]) /
                                    volume.box.half[i], 0.0f, 1.0f);
        localDirection[i] = glm::dot(worldDirection / dirLen, volume.box.axes[i]);
    }
    const glm::vec3 grid = uv * glm::vec3(float(volume.field.nx - 1),
                                          float(volume.field.ny - 1),
                                          float(volume.field.nz - 1));
    const glm::uvec3 lo = glm::uvec3(grid);
    const glm::uvec3 hi = glm::min(lo + glm::uvec3(1),
                                   glm::uvec3(volume.field.nx - 1,
                                              volume.field.ny - 1,
                                              volume.field.nz - 1));
    const glm::vec3 t = grid - glm::vec3(lo);
    float axes[6] = {};
    for (unsigned k = 0; k < 6; ++k) {
        const auto fetch = [&](unsigned x, unsigned y, unsigned z) {
            const float v = volume.field.At(x, y, z).axis[k];
            return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f;
        };
        const float a = glm::mix(glm::mix(fetch(lo.x, lo.y, lo.z),
                                            fetch(hi.x, lo.y, lo.z), t.x),
                                  glm::mix(fetch(lo.x, hi.y, lo.z),
                                            fetch(hi.x, hi.y, lo.z), t.x), t.y);
        const float b = glm::mix(glm::mix(fetch(lo.x, lo.y, hi.z),
                                            fetch(hi.x, lo.y, hi.z), t.x),
                                  glm::mix(fetch(lo.x, hi.y, hi.z),
                                            fetch(hi.x, hi.y, hi.z), t.x), t.y);
        axes[k] = glm::mix(a, b, t.z);
    }

    // Project the direction onto the three positive/negative axes.
    // Absolute weights sum to one, so a constant field stays constant.
    float visibility = 0.0f;
    const float denom = std::abs(localDirection.x) +
                        std::abs(localDirection.y) + std::abs(localDirection.z);
    if (!(denom > 1e-6f))
        return 1.0f;
    for (int i = 0; i < 3; ++i) {
        const unsigned axis = unsigned(i * 2 + (localDirection[i] < 0.0f ? 1 : 0));
        visibility += std::abs(localDirection[i]) * axes[axis] / denom;
    }
    const float influence = weight * std::clamp(
        std::isfinite(volume.intensity) ? volume.intensity : 0.0f, 0.0f, 1.0f);
    return glm::mix(1.0f, visibility, influence);
}

// Blend multiple independent masks conservatively: the minimum visibility
// wins. Never multiply overlapping volumes (which double-darkens overlaps).
// Apply only to ambient SKY fallback, not already-occluded baked GI or AO.
[[nodiscard]] inline float AmbientMaskCombine(float visibilityA, float visibilityB) {
    const float a = std::isfinite(visibilityA) ? std::clamp(visibilityA, 0.0f, 1.0f) : 1.0f;
    const float b = std::isfinite(visibilityB) ? std::clamp(visibilityB, 0.0f, 1.0f) : 1.0f;
    return std::min(a, b);
}

} // namespace tour::scene
