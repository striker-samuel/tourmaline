// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// High/low float32 world-position transport for future persistent GPU scenes.
//
// This is NOT a replacement for CPU double-precision scene transforms.
// Split an absolute dvec3 before uploading it, and subtract a similarly
// split render origin on the GPU BEFORE converting to local float32 math.
// The resulting relative position must feed BOTH culling and rasterization.
//
// Mirror: engine/shaders/double_float.hlsli (two float4s, 32-byte stride).
// The w components are padding and MUST be zero. No render path uses this
// type yet: switching a scene/draw buffer over is a separate change.

#include <cassert>
#include <cmath>
#include <cstddef>
#include <type_traits>

#include <glm/glm.hpp>

namespace tour::math {

struct alignas(16) DoubleFloat3 {
    glm::vec4 high{0.0f};
    glm::vec4 low{0.0f};
};

static_assert(std::is_standard_layout_v<DoubleFloat3>);
static_assert(std::is_trivially_copyable_v<DoubleFloat3>);
static_assert(sizeof(DoubleFloat3) == 32);
static_assert(alignof(DoubleFloat3) == 16);
static_assert(offsetof(DoubleFloat3, high) == 0);
static_assert(offsetof(DoubleFloat3, low) == 16);

// Precondition: finite coordinates inside float32's finite range. The
// residual subtraction is performed in double, never after a float cast.
[[nodiscard]] inline DoubleFloat3 SplitDoubleFloat(const glm::dvec3& position) {
    assert(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z));

    const glm::vec3 high(position);
    assert(std::isfinite(high.x) && std::isfinite(high.y) && std::isfinite(high.z));
    const glm::vec3 low(position - glm::dvec3(high));
    return {glm::vec4(high, 0.0f), glm::vec4(low, 0.0f)};
}

// Useful for validation/serialization tests. CPU scene state should retain
// its original doubles; reconstructed values may differ by split rounding.
[[nodiscard]] inline glm::dvec3 JoinDoubleFloat(const DoubleFloat3& position) {
    return glm::dvec3(position.high) + glm::dvec3(position.low);
}

// Mirrors the shader's arithmetic. Intended for NEARBY world positions; a
// far-away result need not retain subpixel precision in a single float3.
[[nodiscard]] inline glm::vec3 DoubleFloatRelative(const DoubleFloat3& world,
                                                   const DoubleFloat3& origin) {
    const glm::vec3 hi = glm::vec3(world.high) - glm::vec3(origin.high);
    const glm::vec3 lo = glm::vec3(world.low) - glm::vec3(origin.low);
    return hi + lo;
}

// Cheapest current CPU -> per-view draw conversion: subtract while still
// in double, then round the small, local result to float32.
[[nodiscard]] inline glm::vec3 RelativeFloat(const glm::dvec3& world, const glm::dvec3& origin) {
    return glm::vec3(world - origin);
}

// For an AFFINE model matrix: preserve its double-precision translation
// until the view origin has been subtracted. The basis may be rounded to
// float32 because it carries no large absolute world-space translation.
[[nodiscard]] inline glm::mat4 RelativeModel(const glm::dmat4& worldModel,
                                             const glm::dvec3& origin) {
    glm::dmat4 relative = worldModel;
    relative[3] = glm::dvec4(glm::dvec3(worldModel[3]) - origin, worldModel[3].w);
    return glm::mat4(relative);
}

// Optional stable origin for temporal rendering (NOT per-object snapping).
// Negative positions use floor semantics, matching WorldPartition cells.
// Every view matrix consuming relative models must use the SAME origin.
[[nodiscard]] inline glm::dvec3 SnapRenderOrigin(const glm::dvec3& cameraWorld,
                                                 double cellMeters = 4096.0) {
    assert(std::isfinite(cellMeters) && cellMeters > 0.0);
    return glm::floor(cameraWorld / cellMeters) * cellMeters;
}

} // namespace tour::math
