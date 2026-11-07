// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Gizmo rotation deltas are distinct from an object's absolute rotation.
// Apply the DELTA about the pivot in the parent coordinate frame; applying
// the total quaternion changes the position even with zero mouse movement.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace editor::gizmo_rotation {

inline glm::quat Orientation(const glm::vec3& eulerDegrees) {
    return glm::normalize(glm::quat(glm::radians(eulerDegrees)));
}

inline glm::quat LocalDelta(const glm::vec3& fromDegrees, const glm::vec3& toDegrees) {
    return glm::normalize(Orientation(toDegrees) * glm::inverse(Orientation(fromDegrees)));
}

inline glm::quat InFrame(const glm::quat& worldDelta, const glm::quat& parentWorld) {
    return glm::normalize(glm::inverse(parentWorld) * worldDelta * parentWorld);
}

inline glm::vec3 RotatePosition(const glm::vec3& baseTranslation,
                                const glm::vec3& pivotInParent,
                                const glm::quat& parentLocalDelta) {
    return pivotInParent + parentLocalDelta * (baseTranslation - pivotInParent);
}

} // namespace editor::gizmo_rotation
