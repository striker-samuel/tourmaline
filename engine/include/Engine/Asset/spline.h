// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Engine-owned, CPU-only spline data and runtime sampling.
// No renderer, scene, asset registry, or editor dependency.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <glm/glm.hpp>
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

// The SHARED uniform Catmull-Rom scalar basis. This is the single kernel both
// the runtime spline sampler here and the editor `.shape` spline tessellation
// (shape_asset.h) reuse, so the polynomial lives in exactly one place.
// `t` is a segment parameter in [0, 1].
struct CatmullRomWeights {
    float w[4];  // position weights for (p[i-1], p[i], p[i+1], p[i+2])
    float dw[4]; // derivative weights (the unnormalized tangent)
};
[[nodiscard]] inline CatmullRomWeights CatmullRomWeightsAt(float t) {
    const float t2 = t * t, t3 = t2 * t;
    CatmullRomWeights c;
    c.w[0] = 0.5f * (-t + 2.0f * t2 - t3);
    c.w[1] = 0.5f * (2.0f - 5.0f * t2 + 3.0f * t3);
    c.w[2] = 0.5f * (t + 4.0f * t2 - 3.0f * t3);
    c.w[3] = 0.5f * (-t2 + t3);
    c.dw[0] = 0.5f * (-1.0f + 4.0f * t - 3.0f * t2);
    c.dw[1] = 0.5f * (-10.0f * t + 9.0f * t2);
    c.dw[2] = 0.5f * (1.0f + 8.0f * t - 9.0f * t2);
    c.dw[3] = 0.5f * (-2.0f * t + 3.0f * t2);
    return c;
}

// The SHARED control-point neighbor policy: open curves clamp their ends,
// closed curves wrap modulo. `count` must be >= 2 (>= 3 when closed).
[[nodiscard]] inline long long SplineWrapIndex(long long i, long long count, bool closed) {
    if (closed)
        return (i % count + count) % count;
    return std::clamp(i, 0LL, count - 1);
}

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

    const long long i = static_cast<long long>(span);
    const long long n = static_cast<long long>(count);
    const glm::vec3& a = spline.points[size_t(SplineWrapIndex(i - 1, n, spline.closed))];
    const glm::vec3& b = spline.points[size_t(SplineWrapIndex(i, n, spline.closed))];
    const glm::vec3& c = spline.points[size_t(SplineWrapIndex(i + 1, n, spline.closed))];
    const glm::vec3& d = spline.points[size_t(SplineWrapIndex(i + 2, n, spline.closed))];
    const CatmullRomWeights w = CatmullRomWeightsAt(u);

    SplineSample result;
    result.position = w.w[0] * a + w.w[1] * b + w.w[2] * c + w.w[3] * d;
    const glm::vec3 derivative = w.dw[0] * a + w.dw[1] * b + w.dw[2] * c + w.dw[3] * d;
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
        const glm::vec3 next = SampleSpline(spline, float(i) / float(steps)).position;
        length += glm::length(next - previous);
        previous = next;
    }
    return length;
}

} // namespace tour::asset
