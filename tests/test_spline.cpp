// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Asset/spline.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

using namespace tour::asset;

// Hand-rolled runner (runs in every build config — plain `assert` is compiled
// out under NDEBUG, which is exactly how the Release test tree is built).
static int g_checks = 0;
static int g_failures = 0;
static void Check(bool cond, const char* name) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

static bool Near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return glm::length(a - b) < eps;
}

static bool Near4(const CatmullRomWeights& a,
                  float b0,
                  float b1,
                  float b2,
                  float b3,
                  float eps = 1e-4f) {
    return std::abs(a.w[0] - b0) < eps && std::abs(a.w[1] - b1) < eps &&
           std::abs(a.w[2] - b2) < eps && std::abs(a.w[3] - b3) < eps;
}

static void KernelTests() {
    // The shared basis reproduces the exact segment endpoints and is a
    // partition of unity (the weights sum to 1 for every t).
    Check(Near4(CatmullRomWeightsAt(0.0f), 0.0f, 1.0f, 0.0f, 0.0f), "kernel.w.t0");
    Check(Near4(CatmullRomWeightsAt(1.0f), 0.0f, 0.0f, 1.0f, 0.0f), "kernel.w.t1");
    const CatmullRomWeights mid = CatmullRomWeightsAt(0.5f);
    Check(std::abs((mid.w[0] + mid.w[1] + mid.w[2] + mid.w[3]) - 1.0f) < 1e-6f,
          "kernel.w.partition-of-unity");
    Check(std::abs((mid.dw[0] + mid.dw[1] + mid.dw[2] + mid.dw[3])) < 1e-6f, "kernel.dw.sum-zero");
    // Neighbor policy: open clamps to the ends, closed wraps modulo.
    Check(SplineWrapIndex(-1, 4, false) == 0, "wrap.open.lo");
    Check(SplineWrapIndex(5, 4, false) == 3, "wrap.open.hi");
    Check(SplineWrapIndex(-1, 4, true) == 3, "wrap.closed.lo");
    Check(SplineWrapIndex(4, 4, true) == 0, "wrap.closed.hi");
    Check(SplineWrapIndex(6, 4, true) == 2, "wrap.closed.mod");
}

static void SamplingTests() {
    std::string error;
    SplineAsset line{{{0, 0, 0}, {2, 0, 0}}, false};
    Check(ValidSpline(line, error), "valid.open-2pt");
    Check(Near(SampleSpline(line, 0.0f).position, {0, 0, 0}), "sample.line.t0");
    Check(Near(SampleSpline(line, 1.0f).position, {2, 0, 0}), "sample.line.t1");
    Check(Near(SampleSpline(line, 0.5f).position, {1, 0, 0}), "sample.line.mid");
    Check(Near(SampleSpline(line, 0.5f).tangent, {1, 0, 0}), "sample.line.tangent");
    Check(std::abs(ApproximateSplineLength(line) - 2.0f) < 1e-3f, "sample.line.length");

    SplineAsset curve{{{0, 0, 0}, {1, 2, 0}, {2, 0, 0}, {3, 1, 0}}, false};
    Check(ValidSpline(curve, error), "valid.open-4pt");
    Check(Near(SampleSpline(curve, 0.0f).position, curve.points.front()), "sample.curve.t0");
    Check(Near(SampleSpline(curve, 1.0f).position, curve.points.back()), "sample.curve.t1");
    Check(Near(SampleSpline(curve, -1.0f).position, curve.points.front()), "sample.curve.clamp-lo");
    Check(Near(SampleSpline(curve, 2.0f).position, curve.points.back()), "sample.curve.clamp-hi");
    Check(ApproximateSplineLength(curve) > 3.0f, "sample.curve.length");

    curve.closed = true;
    Check(ValidSpline(curve, error), "valid.closed-4pt");
    Check(Near(SampleSpline(curve, 0.0f).position, SampleSpline(curve, 1.0f).position),
          "sample.closed.pos-loop");
    Check(Near(SampleSpline(curve, 0.0f).tangent, SampleSpline(curve, 1.0f).tangent),
          "sample.closed.tangent-loop");
}

static void ValidationTests() {
    std::string error;
    SplineAsset curve{{{0, 0, 0}, {1, 2, 0}, {2, 0, 0}, {3, 1, 0}}, true};
    curve.points[0].x = std::numeric_limits<float>::infinity();
    Check(!ValidSpline(curve, error), "valid.reject-inf");
    curve.points[0].x = 0.0f;
    curve.points.resize(2);
    Check(!ValidSpline(curve, error), "valid.reject-2pt-closed");
    Check(ApproximateSplineLength(curve) == 0.0f, "invalid.length-zero");
    Check(Near(SampleSpline(curve, 0.5f).position, {0, 0, 0}), "invalid.sample-zero");
}

int main() {
    KernelTests();
    SamplingTests();
    ValidationTests();
    std::cout << "checks=" << g_checks << " failures=" << g_failures << '\n';
    return g_failures ? 1 : 0;
}
