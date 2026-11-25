// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only reflected-camera tests. No Vulkan device or shader compiler.
#include <Engine/Rendergraph/planar_reflection.h>

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
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
    const auto view = glm::lookAt(glm::vec3(2, 3, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const glm::mat4 projection(1.0f); // projection choice is irrelevant to reflection math
    smvk::PlanarReflectionCamera reflected{};
    const glm::vec4 plane(0, 2, 0, -4); // y=2 (unnormalized)
    Check(smvk::MakePlanarReflectionCamera(view, projection, glm::vec3(2, 3, 5), plane, reflected),
          "valid plane");
    Check(Near(reflected.eye, glm::vec3(2, 1, 5)), "eye mirrored over y=2");
    Check(Near(glm::vec3(reflected.view * glm::vec4(2, 1, 5, 1)),
               glm::vec3(view * glm::vec4(2, 3, 5, 1))),
          "reflected camera view matches original eye");
    Check(Near(glm::vec3(reflected.view * glm::vec4(3, 4, -2, 1)),
               glm::vec3(view * glm::vec4(3, 0, -2, 1))),
          "reflected view reverses one half-space");
    Check(reflected.reverseWinding, "reflection reverses winding");

    smvk::PlanarReflectionCamera reversed{};
    Check(smvk::MakePlanarReflectionCamera(view, projection, glm::vec3(2, 3, 5), -plane, reversed),
          "reverse normal accepted");
    Check(Near(reflected.eye, reversed.eye), "plane sign is irrelevant");
}
void TestObliqueProjection() {
    glm::mat4 projection(0.0f);
    projection[0][0] = 1.0f;
    projection[1][1] = -1.0f;
    projection[2][3] = -1.0f;
    projection[3][2] = 0.1f; // infinite reversed-Z perspective
    glm::mat4 clipped(1.0f);
    Check(smvk::MakePlanarObliqueProjection(glm::mat4(1), projection,
          glm::vec4(0, 0, -1, -2), clipped), "oblique projection succeeds");
    const auto on = clipped * glm::vec4(0, 0, -2, 1);
    const auto behind = clipped * glm::vec4(0, 0, -1, 1);
    const auto ahead = clipped * glm::vec4(0.5f, 0.5f, -4, 1);
    const auto original = projection * glm::vec4(0.5f, 0.5f, -4, 1);
    Check(std::abs(on.w - on.z) < 1e-5f, "receiver is near clip");
    Check(behind.z > behind.w, "geometry behind receiver clipped");
    Check(ahead.z >= 0 && ahead.z <= ahead.w, "retained geometry visible");
    Check(ahead.x == original.x && ahead.y == original.y && ahead.w == original.w,
          "capture UV unchanged");
    Check(!smvk::MakePlanarObliqueProjection(glm::mat4(1), projection,
          glm::vec4(0, 0, 1, 2), clipped), "wrong half-space rejected");
}
void TestCaptureBudget() {
    const auto half = smvk::PlanarCaptureExtent({1920, 1080}, 2);
    const auto quarter = smvk::PlanarCaptureExtent({1920, 1080}, 4);
    const auto tiny = smvk::PlanarCaptureExtent({1, 1}, 100);
    const auto clamped = smvk::PlanarCaptureExtent({1919, 1079}, 0);
    Check(half.width == 960 && half.height == 540, "half resolution");
    Check(quarter.width == 480 && quarter.height == 270, "quarter resolution");
    Check(tiny.width == 1 && tiny.height == 1, "nonzero tiny target");
    Check(clamped.width == 959 && clamped.height == 539, "odd dimensions and divisor clamp");
    const smvk::PlanarReflectionSettings settings{};
    Check(settings.maxDraws == 512 && settings.resolutionDivisor == 2 &&
          !settings.includeMasked, "cheap defaults");
}
void TestReceiverCrop() {
    // Identity clip space: a small front-facing receiver must shrink both axes.
    smvk::PlanarReceiverCrop crop{};
    Check(smvk::MakePlanarReceiverCrop(glm::mat4(1), glm::vec4(0,0,1,-0.5f),
        glm::vec3(0,0,0.5f), 0.1f, {960,540}, crop), "small receiver visible");
    Check(crop.extent.width < 960 && crop.extent.height < 540, "small receiver crops target");
    const auto left = crop.transform * glm::vec4(-0.1f,0,0.5f,1);
    Check(left.x >= -left.w && left.x <= left.w, "receiver edge retained");
    Check(!smvk::MakePlanarReceiverCrop(glm::mat4(1), glm::vec4(0,0,1,-0.5f),
        glm::vec3(3,0,0.5f), 0.1f, {960,540}, crop), "offscreen receiver rejected");
    Check(smvk::MakePlanarReceiverCrop(glm::mat4(1), glm::vec4(0,0,1,-0.5f),
        glm::vec3(0,0,0.5f), 0, {960,540}, crop) &&
        crop.extent.width == 960 && crop.extent.height == 540, "unbounded fallback");
    Check(smvk::MakePlanarReceiverCrop(glm::mat4(1), glm::vec4(0,1,0,0),
        glm::vec3(0,0,0.5f), 0.1f, {960,540}, crop) &&
        crop.extent.height <= 32, "edge-on receiver conservatively shrinks height");
}
void TestInvalidPlane() {
    smvk::PlanarReflectionCamera camera{};
    Check(!smvk::MakePlanarReflectionCamera(
              glm::mat4(1), glm::mat4(1), glm::vec3(1), glm::vec4(0), camera),
          "zero plane rejected");
    Check(!smvk::MakePlanarReflectionCamera(
              glm::mat4(1), glm::mat4(1), glm::vec3(1), glm::vec4(0, 0, 0, 1), camera),
          "zero normal rejected");
    glm::vec4 normalized{};
    Check(smvk::NormalizePlanarPlane(glm::vec4(0, 3, 0, -6), normalized),
          "plane normalization succeeds");
    Check(std::abs(normalized.y - 1) < 1e-5f && std::abs(normalized.w + 2) < 1e-5f,
          "plane offset normalizes");
}
} // namespace

int main() {
    TestReflectionCamera();
    TestInvalidPlane();
    TestObliqueProjection();
    TestCaptureBudget();
    TestReceiverCrop();
    std::cout << "checks=" << checks << " failures=" << failures << "\n";
    return failures ? 1 : 0;
}
