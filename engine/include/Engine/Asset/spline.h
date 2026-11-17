// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Engine-owned, CPU-only spline data and runtime sampling.
// No renderer, scene, asset registry, or editor dependency.
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace tour::asset {

// Uniform Catmull-Rom spline. Points are in caller-defined local/world space.
// Open curves clamp their end control points; closed curves wrap around.
struct SplineAsset {
    std::vector<glm::vec3> points;
    bool closed = false;
};

struct SplineSample {
    glm::vec3 position{0.0f};
    glm::vec3 tangent{0.0f}; // unit direction, or zero for degenerate spans
};

// Validation is independent from rendering and keeps malformed data out of
// callers that persist authored spline assets.
[[nodiscard]] inline bool ValidSpline(const SplineAsset& spline, std::string& error) {
    if (spline.points.size() < (spline.closed ? 3u : 2u)) {
        error = spline.closed ? "closed spline requires at least 3 points"
                              : "open spline requires at least 2 points";
        return false;
    }
    if (spline.points.size() > 100000u) {
        error = "spline has too many control points";
        return false;
    }
    for (const glm::vec3& p : spline.points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            error = "spline has non-finite control points";
            return false;
        }
    }
    error.clear();
    return true;
}

// Normalized parameter: [0, 1]. Invalid curves return a zero sample.
// For closed splines, t=1 returns the same position as t=0.
// For open splines, t=0 and t=1 return the exact endpoints.
[[nodiscard]] inline SplineSample SampleSpline(const SplineAsset& spline, float t) {
    const size_t count = spline.points.size();
    if (count < (spline.closed ? 3u : 2u) || !std::isfinite(t))
        return {};

    const size_t spans = spline.closed ? count : count - 1;
    const float scaled = std::clamp(t, 0.0f, 1.0f) * float(spans);
    const size_t span = std::min(size_t(scaled), spans - 1);
    const float u = scaled - float(span);
    const auto point = [&](long long index) -> const glm::vec3& {
        if (spline.closed) {
            const long long n = static_cast<long long>(count);
            return spline.points[size_t((index % n + n) % n)];
        }
        return spline.points[size_t(std::clamp(index, 0LL,
                                               static_cast<long long>(count - 1)))];
    };
    const long long i = static_cast<long long>(span);
    const glm::vec3& a = point(i - 1);
    const glm::vec3& b = point(i);
    const glm::vec3& c = point(i + 1);
    const glm::vec3& d = point(i + 2);
    const glm::vec3 v1 = -a + c;
    const glm::vec3 v2 = 2.0f * a - 5.0f * b + 4.0f * c - d;
    const glm::vec3 v3 = -a + 3.0f * b - 3.0f * c + d;
    SplineSample result;
    result.position = 0.5f * (2.0f * b + v1 * u + v2 * (u * u) +
                              v3 * (u * u * u));
    const glm::vec3 derivative =
        0.5f * (v1 + 2.0f * v2 * u + 3.0f * v3 * (u * u));
    const float len2 = glm::dot(derivative, derivative);
    if (len2 > 1e-12f)
        result.tangent = derivative / std::sqrt(len2);
    return result;
}

// Polyline approximation in the spline's coordinate units. 'steps' controls
// accuracy, not the parameterization of SampleSpline.
[[nodiscard]] inline float ApproximateSplineLength(const SplineAsset& spline,
                                                    unsigned steps = 128) {
    if (steps == 0 || spline.points.size() < (spline.closed ? 3u : 2u))
        return 0.0f;
    glm::vec3 previous = SampleSpline(spline, 0.0f).position;
    float length = 0.0f;
    for (unsigned i = 1; i <= steps; ++i) {
        const glm::vec3 next =
            SampleSpline(spline, float(i) / float(steps)).position;
        length += glm::length(next - previous);
        previous = next;
    }
    return length;
}

} // namespace tour::asset
