// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only reflected-camera tests. No Vulkan device or shader compiler.
#include <Engine/Rendergraph/planar_reflection.h>

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <iostream>

namespace {
int checks = 0;
int failures = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL " << label << "\n";
    }
}
bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-4f) {
    return glm::length(a - b) < epsilon;
}
void TestReflectionCamera() {
    const auto view = glm::lookAt(glm::vec3(2, 3, 5),
        glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const glm::mat4 projection(1.0f); // projection choice is irrelevant to reflection math
    smvk::PlanarReflectionCamera reflected{};
    const glm::vec4 plane(0, 2, 0, -4); // y=2 (unnormalized)
    Check(smvk::MakePlanarReflectionCamera(view, projection,
        glm::vec3(2, 3, 5), plane, reflected), "valid plane");
    Check(Near(reflected.eye, glm::vec3(2, 1, 5)), "eye mirrored over y=2");
    Check(Near(glm::vec3(reflected.view * glm::vec4(2, 1, 5, 1)),
        glm::vec3(view * glm::vec4(2, 3, 5, 1))),
        "reflected camera view matches original eye");
    Check(Near(glm::vec3(reflected.view * glm::vec4(3, 4, -2, 1)),
        glm::vec3(view * glm::vec4(3, 0, -2, 1))),
        "reflected view reverses one half-space");
    Check(reflected.reverseWinding, "reflection reverses winding");

    smvk::PlanarReflectionCamera reversed{};
    Check(smvk::MakePlanarReflectionCamera(view, projection,
        glm::vec3(2, 3, 5), -plane, reversed), "reverse normal accepted");
    Check(Near(reflected.eye, reversed.eye), "plane sign is irrelevant");
}
void TestInvalidPlane() {
    smvk::PlanarReflectionCamera camera{};
    Check(!smvk::MakePlanarReflectionCamera(glm::mat4(1), glm::mat4(1),
        glm::vec3(1), glm::vec4(0), camera), "zero plane rejected");
    Check(!smvk::MakePlanarReflectionCamera(glm::mat4(1), glm::mat4(1),
        glm::vec3(1), glm::vec4(0, 0, 0, 1), camera),
        "zero normal rejected");
    glm::vec4 normalized{};
    Check(smvk::NormalizePlanarPlane(glm::vec4(0, 3, 0, -6),
        normalized), "plane normalization succeeds");
    Check(std::abs(normalized.y - 1) < 1e-5f &&
        std::abs(normalized.w + 2) < 1e-5f, "plane offset normalizes");
}
} // namespace

int main() {
    TestReflectionCamera();
    TestInvalidPlane();
    std::cout << "checks=" << checks << " failures=" << failures << "\n";
    return failures ? 1 : 0;
}
