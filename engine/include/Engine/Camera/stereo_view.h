// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// CPU stereo camera contract for large worlds. Both eyes share ONE render
// origin; eye offsets are applied in double precision before conversion.
// This is a preparation layer, not an OpenXR swapchain/session implementation.

#include <Engine/Core/double_float.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <array>
#include <cassert>
#include <cmath>

namespace tour::math {

struct StereoEyeView {
    glm::dvec3 worldPosition{0.0};
    // View and VP consume positions rebased against originWorld, NEVER
    // absolute-world float32 positions.
    glm::mat4 relativeView{1.0f};
    glm::mat4 relativeViewProjection{1.0f};
    DoubleFloat3 origin{};
};

struct StereoViewPair {
    glm::dvec3 originWorld{0.0};
    std::array<StereoEyeView, 2> eyes{}; // [0] left, [1] right
};

// The runtime supplies eye offsets in head-local metres and independent
// (potentially asymmetric) projection matrices. OpenXR's per-eye poses can
// also be expressed as a head pose plus local eye poses; use the overload
// below if each eye has a different orientation.
// A single snapped origin prevents left/right precision or temporal
// discontinuities when a head crosses a render-origin cell boundary.
[[nodiscard]] inline StereoViewPair MakeStereoViewPair(
    const glm::dvec3& headWorld,
    const glm::dquat& headOrientation,
    const std::array<glm::dvec3, 2>& eyeOffsets,
    const std::array<glm::dquat, 2>& localEyeOrientations,
    const std::array<glm::mat4, 2>& projections,
    double originCellMeters = 4096.0) {
    assert(std::isfinite(headWorld.x) && std::isfinite(headWorld.y) &&
           std::isfinite(headWorld.z));
    assert(glm::length(headOrientation) > 0.0);
    const glm::dquat headRotation = glm::normalize(headOrientation);

    StereoViewPair pair;
    pair.originWorld = SnapRenderOrigin(headWorld, originCellMeters);
    const DoubleFloat3 packedOrigin = SplitDoubleFloat(pair.originWorld);
    for (size_t eye = 0; eye < 2; ++eye) {
        assert(glm::length(localEyeOrientations[eye]) > 0.0);
        const glm::dquat rotation =
            glm::normalize(headRotation * glm::normalize(localEyeOrientations[eye]));
        StereoEyeView& out = pair.eyes[eye];
        out.worldPosition = headWorld + headRotation * eyeOffsets[eye];
        out.origin = packedOrigin;

        // Build rotation and eye-to-origin translation in double. Rounding
        // absolute eye positions to float before subtraction loses IPD at 1e12.
        const glm::dmat4 relativeViewD =
            glm::mat4_cast(glm::conjugate(rotation)) *
            glm::translate(glm::dmat4(1.0), -(out.worldPosition - pair.originWorld));
        out.relativeView = glm::mat4(relativeViewD);
        out.relativeViewProjection = projections[eye] * out.relativeView;
    }
    return pair;
}

[[nodiscard]] inline StereoViewPair MakeStereoViewPair(
    const glm::dvec3& headWorld,
    const glm::dquat& headOrientation,
    const std::array<glm::dvec3, 2>& eyeOffsets,
    const std::array<glm::mat4, 2>& projections,
    double originCellMeters = 4096.0) {
    return MakeStereoViewPair(headWorld,
                              headOrientation,
                              eyeOffsets,
                              {glm::dquat(1.0, 0.0, 0.0, 0.0),
                               glm::dquat(1.0, 0.0, 0.0, 0.0)},
                              projections,
                              originCellMeters);
}

} // namespace tour::math
