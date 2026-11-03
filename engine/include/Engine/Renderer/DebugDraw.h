// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Immediate-mode, CPU-only world-space debug drawing. This layer has no
// renderer, physics, scene-node, or GPU resource dependencies. Callers own
// the meaning of the geometry; renderers decide whether to consume it.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace tour {

enum class DebugDepthMode : uint8_t {
    Tested, // Occluded by scene geometry
    Always  // X-ray overlay, still rendered as wireframe
};

struct DebugPoint {
    glm::vec3 position{ 0.0f };
    glm::vec3 color{ 1.0f };
    DebugDepthMode depth = DebugDepthMode::Tested;
};

struct DebugLine {
    glm::vec3 a{ 0.0f };
    glm::vec3 b{ 0.0f };
    glm::vec3 color{ 1.0f };
    DebugDepthMode depth = DebugDepthMode::Tested;
};

class DebugDrawList {
public:
    // Bounds memory and GPU upload work if a debug producer misbehaves.
    static constexpr size_t MaxLines = 65536;
    static constexpr size_t MaxPoints = 65536;

    void Clear() {
        m_lines.clear();
        m_points.clear();
        m_dropped = 0;
        m_droppedPoints = 0;
    }
    [[nodiscard]] bool Empty() const { return m_lines.empty() && m_points.empty(); }
    [[nodiscard]] size_t Size() const { return m_lines.size(); }
    [[nodiscard]] size_t DroppedLines() const { return m_dropped; }
    [[nodiscard]] size_t PointCount() const { return m_points.size(); }
    [[nodiscard]] size_t DroppedPoints() const { return m_droppedPoints; }
    [[nodiscard]] std::span<const DebugPoint> Points() const { return m_points; }

    void Point(glm::vec3 position, glm::vec3 color,
               DebugDepthMode depth = DebugDepthMode::Tested) {
        if (m_points.size() == MaxPoints) {
            ++m_droppedPoints;
            return;
        }
        m_points.push_back({position, color, depth});
    }
    [[nodiscard]] std::span<const DebugLine> Lines() const { return m_lines; }

    void Line(glm::vec3 a, glm::vec3 b, glm::vec3 color,
              DebugDepthMode depth = DebugDepthMode::Tested) {
        if (m_lines.size() == MaxLines) {
            ++m_dropped;
            return;
        }
        m_lines.push_back({ a, b, color, depth });
    }

    void Box(const glm::mat4& transform, glm::vec3 halfExtents,
             glm::vec3 color, DebugDepthMode depth = DebugDepthMode::Tested) {
        glm::vec3 corners[8];
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 p(
                (i & 1) ? halfExtents.x : -halfExtents.x,
                (i & 2) ? halfExtents.y : -halfExtents.y,
                (i & 4) ? halfExtents.z : -halfExtents.z);
            corners[i] = glm::vec3(transform * glm::vec4(p, 1.0f));
        }
        static constexpr int edges[12][2] = {
            {0,1}, {2,3}, {4,5}, {6,7},
            {0,2}, {1,3}, {4,6}, {5,7},
            {0,4}, {1,5}, {2,6}, {3,7}
        };
        for (const auto& e : edges)
            Line(corners[e[0]], corners[e[1]], color, depth);
    }

    void Aabb(glm::vec3 min, glm::vec3 max, glm::vec3 color,
              DebugDepthMode depth = DebugDepthMode::Tested) {
        if (glm::any(glm::greaterThan(min, max)))
            return;
        glm::mat4 transform(1.0f);
        transform[3] = glm::vec4((min + max) * 0.5f, 1.0f);
        Box(transform, (max - min) * 0.5f, color, depth);
    }

    void Sphere(glm::vec3 center, float radius, glm::vec3 color,
                DebugDepthMode depth = DebugDepthMode::Tested,
                uint32_t segments = 24) {
        if (!(radius > 0.0f) || !std::isfinite(radius))
            return;
        segments = std::clamp(segments, 8u, 128u);
        Circle(center, { 1, 0, 0 }, { 0, 1, 0 }, radius, color, depth, segments);
        Circle(center, { 0, 1, 0 }, { 0, 0, 1 }, radius, color, depth, segments);
        Circle(center, { 0, 0, 1 }, { 1, 0, 0 }, radius, color, depth, segments);
    }

    // Endpoints are the centers of the two hemispheres (not the tips).
    void Capsule(glm::vec3 a, glm::vec3 b, float radius, glm::vec3 color,
                 DebugDepthMode depth = DebugDepthMode::Tested,
                 uint32_t segments = 24) {
        if (!(radius > 0.0f) || !std::isfinite(radius))
            return;
        glm::vec3 axis = b - a;
        const float len = glm::length(axis);
        if (len < 1e-6f) {
            Sphere(a, radius, color, depth, segments);
            return;
        }
        axis /= len;
        glm::vec3 reference = std::abs(axis.y) < 0.9f
            ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 u = glm::normalize(glm::cross(axis, reference));
        const glm::vec3 v = glm::cross(axis, u);
        segments = std::clamp(segments, 8u, 128u);
        Circle(a, u, v, radius, color, depth, segments);
        Circle(b, u, v, radius, color, depth, segments);
        for (glm::vec3 radial : { u, -u, v, -v })
            Line(a + radial * radius, b + radial * radius, color, depth);
        const uint32_t halfSteps = std::max(4u, segments / 2u);
        constexpr float pi = 3.14159265358979323846f;
        for (glm::vec3 radial : { u, v }) {
            for (uint32_t i = 0; i < halfSteps; ++i) {
                const float t0 = pi * float(i) / float(halfSteps);
                const float t1 = pi * float(i + 1) / float(halfSteps);
                for (float sign : { -1.0f, 1.0f }) {
                    const glm::vec3 center = sign < 0 ? a : b;
                    const glm::vec3 capAxis = axis * sign;
                    const auto capPoint = [&](float t) {
                        return center + radius * (radial * std::cos(t)
                            + capAxis * std::sin(t));
                    };
                    Line(capPoint(t0), capPoint(t1), color, depth);
                }
            }
        }
    }

    void Ray(glm::vec3 origin, glm::vec3 direction, float length,
             glm::vec3 color, DebugDepthMode depth = DebugDepthMode::Tested) {
        if (length > 0.0f && std::isfinite(length))
            Line(origin, origin + direction * length, color, depth);
    }

    void Axes(const glm::mat4& transform, float scale = 1.0f,
              DebugDepthMode depth = DebugDepthMode::Always) {
        const glm::vec3 o(transform[3]);
        for (int i = 0; i < 3; ++i) {
            glm::vec3 color(0.0f);
            color[i] = 1.0f;
            Line(o, o + glm::vec3(transform[i]) * scale, color, depth);
        }
    }

private:
    void Circle(glm::vec3 center, glm::vec3 u, glm::vec3 v, float radius,
                glm::vec3 color, DebugDepthMode depth, uint32_t segments) {
        constexpr float tau = 6.28318530717958647692f;
        for (uint32_t i = 0; i < segments; ++i) {
            const float t0 = tau * float(i) / float(segments);
            const float t1 = tau * float(i + 1) / float(segments);
            Line(center + radius * (u * std::cos(t0) + v * std::sin(t0)),
                 center + radius * (u * std::cos(t1) + v * std::sin(t1)),
                 color, depth);
        }
    }

    std::vector<DebugLine> m_lines;
    std::vector<DebugPoint> m_points;
    size_t m_dropped = 0;
    size_t m_droppedPoints = 0;
};

} // namespace tour
