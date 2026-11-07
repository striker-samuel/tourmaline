// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// A matrix with a zero scale axis is singular. glm::decompose cannot
// extract its quaternion reliably (and can produce NaNs), so the inspector
// needs an explicit fallback for those perfectly valid edit states.
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace editor::transform {

inline bool Finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline bool Finite(const glm::mat4& m) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(m[c][r]))
                return false;
    return true;
}

inline void ReadLocalTRS(const glm::mat4& m,
                         glm::vec3& translation,
                         glm::vec3& rotationDeg,
                         glm::vec3& scale) {
    translation = glm::vec3(0.0f);
    rotationDeg = glm::vec3(0.0f);
    scale = glm::vec3(1.0f);

    // Even a previously poisoned transform should remain editable. A
    // subsequent valid edit writes a fresh finite matrix.
    for (int i = 0; i < 3; ++i)
        if (std::isfinite(m[3][i]))
            translation[i] = m[3][i];
    if (!Finite(m))
        return;

    glm::vec3 axes[3] = {glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2])};
    bool present[3] = {};
    int count = 0;
    for (int i = 0; i < 3; ++i) {
        const float magnitude = glm::length(axes[i]);
        if (!std::isfinite(magnitude))
            return;
        scale[i] = magnitude; // Retain exact zero; never substitute epsilon.
        present[i] = magnitude > 1e-7f;
        if (present[i]) {
            axes[i] /= magnitude;
            ++count;
        }
    }

    // Preserve the existing decomposition (including signed scales) on
    // nonsingular matrices; only zero/degenerate matrices need recovery.
    if (count == 3) {
        glm::vec3 t, s, skew;
        glm::vec4 perspective;
        glm::quat q;
        if (glm::decompose(m, s, q, t, skew, perspective)) {
            const glm::vec3 angles = glm::degrees(glm::eulerAngles(q));
            if (Finite(t) && Finite(s) && Finite(angles)) {
                translation = t;
                rotationDeg = angles;
                scale = s;
                return;
            }
        }
    }

    // Recover the missing basis direction from the remaining columns.
    // When two or three axes are collapsed, rotation is underdetermined:
    // choose a stable perpendicular frame rather than inventing NaNs.
    if (count == 2) {
        if (!present[0])
            axes[0] = glm::cross(axes[1], axes[2]);
        if (!present[1])
            axes[1] = glm::cross(axes[2], axes[0]);
        if (!present[2])
            axes[2] = glm::cross(axes[0], axes[1]);
        for (int i = 0; i < 3; ++i) {
            const float magnitude = glm::length(axes[i]);
            if (!(magnitude > 1e-7f) || !std::isfinite(magnitude))
                return;
            axes[i] /= magnitude;
        }
    } else if (count == 1) {
        const int known = present[0] ? 0 : (present[1] ? 1 : 2);
        const glm::vec3 ref =
            std::abs(axes[known].x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 next = glm::normalize(glm::cross(ref, axes[known]));
        axes[(known + 1) % 3] = next;
        axes[(known + 2) % 3] = glm::cross(axes[known], axes[(known + 1) % 3]);
    } else if (count == 0) {
        // No basis directions survive; a zero-scale node can still be
        // selected and given a new scale.
        return;
    }

    const glm::quat q = glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2]));
    const glm::vec3 angles = glm::degrees(glm::eulerAngles(q));
    if (Finite(angles))
        rotationDeg = angles;
}

} // namespace editor::transform
