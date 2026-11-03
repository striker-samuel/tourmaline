// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/DebugDraw.h>
#include <Engine/Renderer/ShapeDebugDraw.h>

#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

namespace { int checks = 0, failures = 0; }
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #expr); } } while (0)

int main() {
    tour::DebugDrawList draw;
    const glm::vec3 green(0, 1, 0);
    const glm::vec3 yellow(1, 1, 0);
    CHECK(draw.Empty());
    draw.Line({0,0,0}, {1,0,0}, green, tour::DebugDepthMode::Always);
    CHECK(draw.Size() == 1);
    CHECK(draw.Lines()[0].color == green);
    CHECK(draw.Lines()[0].depth == tour::DebugDepthMode::Always);

    draw.Point({0,1,2}, yellow, tour::DebugDepthMode::Always);
    CHECK(!draw.Empty());
    CHECK(draw.PointCount() == 1);
    CHECK(draw.Points()[0].position == glm::vec3(0,1,2));
    CHECK(draw.Points()[0].depth == tour::DebugDepthMode::Always);
    draw.Clear();
    CHECK(draw.PointCount() == 0 && draw.DroppedPoints() == 0);
    {
        std::string error;
        auto cloud = tour::asset::DefaultShapeAsset(tour::asset::ShapeKind::PointCloud);
        glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(3,4,5));
        CHECK(tour::renderer::AppendShapeDebug(cloud, moved, green, draw, error));
        CHECK(draw.PointCount() == cloud.points.size());
        CHECK(draw.Points()[0].position == glm::vec3(3,4,5));
        CHECK(draw.Size() == 0);
        draw.Clear();
        auto spline = tour::asset::DefaultShapeAsset(tour::asset::ShapeKind::Spline);
        spline.segments = 4;
        CHECK(tour::renderer::AppendShapeDebug(spline, moved, green, draw, error));
        CHECK(draw.Size() == 8);
        CHECK(draw.Lines()[0].a == glm::vec3(2,4,5));
        CHECK(draw.Lines()[7].b == glm::vec3(4,4,5));
        draw.Clear();
        CHECK(!tour::renderer::AppendShapeDebug(
            tour::asset::DefaultShapeAsset(tour::asset::ShapeKind::Box),
            moved, green, draw, error));
        CHECK(draw.Empty());
    }
    draw.Clear();
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(3, 4, 5));
    draw.Box(transform, {1, 2, 3}, green);
    CHECK(draw.Size() == 12);
    // 12 unique cube edges, never face diagonals.
    for (auto& line : draw.Lines()) {
        glm::vec3 delta = glm::abs(line.b - line.a);
        CHECK((delta.x > 0) + (delta.y > 0) + (delta.z > 0) == 1);
        CHECK(line.depth == tour::DebugDepthMode::Tested);
    }
    draw.Clear();
    draw.Aabb({2,2,2}, {-2,-2,-2}, green);
    CHECK(draw.Empty());

    draw.Sphere({0,0,0}, 2.0f, yellow);
    CHECK(draw.Size() == 72); // Three 24-segment great circles.
    for (auto& line : draw.Lines()) {
        CHECK(std::abs(glm::length(line.a) - 2.0f) < 0.0001f);
        CHECK(std::abs(glm::length(line.b) - 2.0f) < 0.0001f);
    }
    draw.Sphere({0,0,0}, -1.0f, yellow);
    CHECK(draw.Size() == 72);

    draw.Clear();
    draw.Capsule({0,0,0}, {0,2,0}, 1.0f, green);
    CHECK(draw.Size() == 2 * 24 + 4 + 4 * 12);
    draw.Clear();
    draw.Capsule({0,0,0}, {0,0,0}, 1.0f, green);
    CHECK(draw.Size() == 72); // Degenerate capsule becomes sphere.
    draw.Clear();
    draw.Axes(glm::mat4(1.0f));
    CHECK(draw.Size() == 3);
    CHECK(draw.Lines()[0].color == glm::vec3(1,0,0));
    CHECK(draw.Lines()[1].color == glm::vec3(0,1,0));
    CHECK(draw.Lines()[2].color == glm::vec3(0,0,1));
    CHECK(draw.Lines()[0].depth == tour::DebugDepthMode::Always);
    draw.Clear();

    for (size_t i = 0; i < tour::DebugDrawList::MaxLines + 3; ++i)
        draw.Line({0,0,0}, {1,0,0}, green);
    CHECK(draw.Size() == tour::DebugDrawList::MaxLines);
    CHECK(draw.DroppedLines() == 3);
    draw.Clear();
    CHECK(draw.Empty() && draw.DroppedLines() == 0);
    for (size_t i = 0; i < tour::DebugDrawList::MaxPoints + 2; ++i)
        draw.Point({0,0,0}, yellow);
    CHECK(draw.PointCount() == tour::DebugDrawList::MaxPoints);
    CHECK(draw.DroppedPoints() == 2);
    CHECK(!draw.Empty());
    draw.Clear();
    CHECK(draw.PointCount() == 0 && draw.DroppedPoints() == 0);
    std::printf("debug draw: %d checks, %d failures\\n", checks, failures);
    return failures ? 1 : 0;
}
