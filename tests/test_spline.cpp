// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Asset/spline.h>
#include <cassert>
#include <cmath>
#include <limits>
#include <string>

using namespace tour::asset;

static bool Near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return glm::length(a - b) < eps;
}

int main() {
    std::string error;
    SplineAsset line{{{0, 0, 0}, {2, 0, 0}}, false};
    assert(ValidSpline(line, error));
    assert(Near(SampleSpline(line, 0.0f).position, {0, 0, 0}));
    assert(Near(SampleSpline(line, 1.0f).position, {2, 0, 0}));
    assert(Near(SampleSpline(line, 0.5f).position, {1, 0, 0}));
    assert(Near(SampleSpline(line, 0.5f).tangent, {1, 0, 0}));
    assert(std::abs(ApproximateSplineLength(line) - 2.0f) < 1e-3f);

    SplineAsset curve{{{0, 0, 0}, {1, 2, 0}, {2, 0, 0}, {3, 1, 0}}, false};
    assert(ValidSpline(curve, error));
    assert(Near(SampleSpline(curve, 0.0f).position, curve.points.front()));
    assert(Near(SampleSpline(curve, 1.0f).position, curve.points.back()));
    assert(Near(SampleSpline(curve, -1.0f).position, curve.points.front()));
    assert(Near(SampleSpline(curve, 2.0f).position, curve.points.back()));
    assert(ApproximateSplineLength(curve) > 3.0f);

    curve.closed = true;
    assert(ValidSpline(curve, error));
    assert(Near(SampleSpline(curve, 0.0f).position, SampleSpline(curve, 1.0f).position));
    assert(Near(SampleSpline(curve, 0.0f).tangent, SampleSpline(curve, 1.0f).tangent));

    curve.points[0].x = std::numeric_limits<float>::infinity();
    assert(!ValidSpline(curve, error));
    curve.points[0].x = 0.0f;
    curve.points.resize(2);
    assert(!ValidSpline(curve, error));
    assert(ApproximateSplineLength(curve) == 0.0f);
    assert(Near(SampleSpline(curve, 0.5f).position, {0, 0, 0}));
}
