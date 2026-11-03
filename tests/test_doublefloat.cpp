// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Core/double_float.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
// House test convention (AGENTS.md): the CHECK macro ALWAYS evaluates its
// condition, in every build flavor. This file originally used CHECK(),
// whose NDEBUG no-op makes Release runs vacuous - and any side effect
// inside an assert (see test_gpu_scene_candidates.cpp:
// assert(leaves.Flush(db))) was silently skipped, crashing the run.
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } \
    while (0)
namespace { int g_checks = 0; int g_failures = 0; }

namespace {
using tour::math::DoubleFloat3;
using tour::math::DoubleFloatRelative;
using tour::math::JoinDoubleFloat;
using tour::math::RelativeFloat;
using tour::math::RelativeModel;
using tour::math::SnapRenderOrigin;
using tour::math::SplitDoubleFloat;

void CheckNear(float actual, double expected, double tolerance) {
    if (std::abs(double(actual) - expected) > tolerance) {
        std::fprintf(stderr, "DoubleFloat: expected %.12g, got %.12g (tol %.4g)\n",
                     expected, double(actual), tolerance);
        std::abort();
    }
}

void CheckRelative(const glm::dvec3& world, const glm::dvec3& eye,
                   double tolerance) {
    const DoubleFloat3 gpuWorld = SplitDoubleFloat(world);
    const DoubleFloat3 gpuEye = SplitDoubleFloat(eye);
    CHECK(gpuWorld.high.w == 0.0f && gpuWorld.low.w == 0.0f);
    CHECK(gpuEye.high.w == 0.0f && gpuEye.low.w == 0.0f);
    const glm::vec3 delta = DoubleFloatRelative(gpuWorld, gpuEye);
    const glm::dvec3 truth = world - eye;
    for (int i = 0; i != 3; ++i)
        CheckNear(delta[i], truth[i], tolerance);
    const glm::vec3 cpuDelta = RelativeFloat(world, eye);
    for (int i = 0; i != 3; ++i)
        CheckNear(cpuDelta[i], truth[i], 0.0002);
}
} // namespace

int main() {
    // Conversion layout and exact zeros.
    const DoubleFloat3 zero = SplitDoubleFloat(glm::dvec3(0.0));
    CHECK(zero.high == glm::vec4(0.0f));
    CHECK(zero.low == glm::vec4(0.0f));

    // A small offset at one trillion world units is invisible if absolute
    // world and eye positions are independently converted to float32.
    const glm::dvec3 eye(1.0e12, -1.0e12, 1.0e12);
    const glm::dvec3 world = eye + glm::dvec3(0.125, -3.25, 7.5);
    const glm::vec3 broken = glm::vec3(world) - glm::vec3(eye);
    CHECK(broken == glm::vec3(0.0f));
    CheckRelative(world, eye, 0.02);

    const DoubleFloat3 packed = SplitDoubleFloat(world);
    const glm::dvec3 restored = JoinDoubleFloat(packed);
    for (int i = 0; i != 3; ++i)
        CHECK(std::abs(restored[i] - world[i]) < 0.02);

    // Exercise both signs, the high-part rounding boundary and hundreds
    // of nearby offsets. 2 cm is a conservative bound at 1e12 units,
    // where the residual float itself can have millimetre-scale ULPs.
    for (const double base : {0.0, 1.0e6, -1.0e9, 1.0e12, -1.0e12}) {
        const glm::dvec3 camera(base + 17.0, base - 13.0, base + 5.0);
        for (int i = 0; i < 128; ++i) {
            const double t = double(i) * 0.137;
            const glm::dvec3 point(base + t, base - 0.5 * t,
                                   base + 0.25 * t);
            CheckRelative(point, camera, 0.02);
        }
    }

    // Rebase in double BEFORE casting to the float32 GfxDraw model.
    glm::dmat4 model(1.0);
    model[3] = glm::dvec4(world, 1.0);
    const glm::mat4 relativeModel = RelativeModel(model, eye);
    for (int i = 0; i != 3; ++i)
        CheckNear(relativeModel[3][i], (world - eye)[i], 0.0002);
    CHECK(relativeModel[3].w == 1.0f);

    // Snapping must be stable around positive and negative grid edges.
    const glm::dvec3 snapped = SnapRenderOrigin(
        glm::dvec3(-0.5, 4096.0, -4096.125));
    CHECK(snapped == glm::dvec3(-4096.0, 4096.0, -8192.0));
    CHECK(SnapRenderOrigin(glm::dvec3(4095.99, 0.0, 8191.0))
           == glm::dvec3(0.0, 0.0, 4096.0));

    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
