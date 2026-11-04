// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Viewport surface-sketch strokes. Points are stored in the hit node's
// LOCAL frame, keyed by its persisted scene UUID rather than a transient
// world-node index. This is editor-only data until explicitly baked as
// scene locators; a spline is a connected guide through the same points.

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace editor::surface {

enum class Mode : int { Points = 0, Spline = 1 };
inline constexpr size_t kMaxPointsPerStroke = 2048;
inline constexpr size_t kMaxStrokes = 512;

struct Stroke {
    std::string parentUuid;
    Mode mode = Mode::Spline;
    std::vector<glm::vec3> points; // parent-local coordinates
};

// The singular-scale guard matters: inverse(parent) with a zero scale
// otherwise poisons an entire saved stroke with NaNs.
inline bool AppendWorldHit(Stroke& s, const glm::vec3& world,
                          const glm::mat4& parentWorld) {
    if (s.points.size() >= kMaxPointsPerStroke)
        return false;
    const float determinant = glm::determinant(parentWorld);
    if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-9f)
        return false;
    const glm::vec4 local4 = glm::inverse(parentWorld) * glm::vec4(world, 1.0f);
    if (!std::isfinite(local4.x) || !std::isfinite(local4.y) ||
        !std::isfinite(local4.z) || !std::isfinite(local4.w) ||
        std::fabs(local4.w) < 1e-7f)
        return false;
    const glm::vec3 local = glm::vec3(local4) / local4.w;
    if (!s.points.empty() &&
        glm::length(parentWorld * glm::vec4(s.points.back(), 1.0f) -
                    glm::vec4(world, 1.0f)) < 0.001f)
        return false; // overlapping samples waste slots, especially stationary LMB
    s.points.push_back(local);
    return true;
}

inline std::string SerializeMeta(const Stroke& s) {
    return "v1 " + std::to_string(int(s.mode)) + " " + s.parentUuid;
}
inline bool ParseMeta(const std::string& value, Stroke& s) {
    std::istringstream in(value);
    std::string version, uuid, extra;
    int mode = -1;
    if (!(in >> version >> mode >> uuid) || (in >> extra) ||
        version != "v1" || mode < 0 || mode > 1 || uuid.empty())
        return false;
    s.mode = Mode(mode);
    s.parentUuid = std::move(uuid);
    s.points.clear();
    return true;
}
inline std::string SerializePoint(const glm::vec3& p) {
    std::ostringstream out;
    out << std::setprecision(9) << p.x << " " << p.y << " " << p.z;
    return out.str();
}
inline bool ParsePoint(const std::string& value, glm::vec3& p) {
    std::istringstream in(value);
    glm::vec3 candidate{};
    std::string extra;
    if (!(in >> candidate.x >> candidate.y >> candidate.z) || (in >> extra) ||
        !std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
        !std::isfinite(candidate.z))
        return false;
    p = candidate;
    return true;
}

} // namespace editor::surface
