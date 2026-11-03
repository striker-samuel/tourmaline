// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/ShapeScene.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

using namespace tour;

int main() {
    renderer::ShapeScene scene;
    std::string error;
    auto spline = asset::DefaultShapeAsset(asset::ShapeKind::Spline);
    spline.segments = 4;
    const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(1,2,3));
    const auto first = scene.Add(spline, transform, {1,0,0}, error);
    assert(first && scene.Size() == 1);
    assert(scene.Get(first));
    assert(scene.Get(first)->geometry.topology == asset::ShapeGeometry::Topology::Lines);
    DebugDrawList draw;
    assert(scene.AppendDraws(draw) == 1);
    assert(draw.Size() == 8 && draw.PointCount() == 0);
    assert(draw.Lines().front().a == glm::vec3(0,2,3));
    assert(draw.Lines().back().b == glm::vec3(2,2,3));
    assert(scene.SetTransform(first, glm::mat4(1.0f)));
    draw.Clear();
    assert(scene.AppendDraws(draw) == 1);
    assert(draw.Lines().front().a == glm::vec3(-1,0,0));
    assert(scene.SetColor(first, {0,1,0}));
    draw.Clear();
    assert(scene.AppendDraws(draw) == 1);
    assert(draw.Lines().front().color == glm::vec3(0,1,0));

    const auto pointCloud = asset::DefaultShapeAsset(asset::ShapeKind::PointCloud);
    auto second = scene.Add(pointCloud, transform, {0,0,1}, error,
                            DebugDepthMode::Always);
    assert(second && scene.Size() == 2);
    draw.Clear();
    assert(scene.AppendDraws(draw) == 2);
    assert(draw.PointCount() == 4);
    assert(draw.Points().front().position == glm::vec3(1,2,3));
    assert(draw.Points().front().depth == DebugDepthMode::Always);

    const auto rejected = scene.Add(asset::DefaultShapeAsset(asset::ShapeKind::Box),
                                    transform, {1,1,1}, error);
    assert(!rejected && scene.Size() == 2);
    assert(!error.empty());
    assert(scene.Remove(first));
    assert(!scene.Remove(first));
    assert(!scene.Get(first));
    const auto reuse = scene.Add(spline, glm::mat4(1.0f), {1,1,0}, error);
    assert(reuse && reuse.index == first.index);
    assert(reuse.generation != first.generation);
    assert(!scene.SetTransform(first, transform));
    assert(scene.Get(reuse));
    scene.Clear();
    assert(scene.Size() == 0);
    assert(!scene.Get(reuse) && !scene.Get(second));
    draw.Clear();
    assert(scene.AppendDraws(draw) == 0 && draw.Empty());

    // Authored .shape file becomes a retained instance, with no callback
    // and with no dependency on the editor or a Vulkan device.
    const auto temp = std::filesystem::temp_directory_path()
        / "tourmaline_shape_scene_test.shape";
    {
        std::ofstream file(temp, std::ios::binary);
        file << asset::SerializeShapeAsset(
            asset::DefaultShapeAsset(asset::ShapeKind::PointCloud));
        assert(file.good());
    }
    const auto loaded = scene.LoadFile(temp, glm::mat4(1.0f), {0,1,1}, error);
    assert(loaded && scene.Get(loaded));
    assert(scene.Get(loaded)->geometry.vertices.size() == 4);
    std::filesystem::remove(temp);
    assert(scene.Get(loaded)); // source is owned after load
    const auto textLoaded = scene.LoadText(
        asset::SerializeShapeAsset(asset::DefaultShapeAsset(
            asset::ShapeKind::Spline)), transform, {1,1,1}, error);
    assert(textLoaded && scene.Get(textLoaded));
    assert(scene.Size() == 2);
    const auto bad = scene.LoadText("shape 1\\nkind bogus\\n",
        transform, {1,1,1}, error);
    assert(!bad && !error.empty() && scene.Size() == 2);
    const auto absent = scene.LoadFile(temp, transform, {1,1,1}, error);
    assert(!absent && !error.empty());
    assert(scene.Size() == 2);
    scene.Clear();

    // Admission is not partial: a dataset that exceeds the per-pass budget
    // is retained as an instance but skipped by this bounded draw stream.
    auto cloud = asset::DefaultShapeAsset(asset::ShapeKind::PointCloud);
    cloud.points.resize(DebugDrawList::MaxPoints + 1);
    const auto large = scene.Add(cloud, transform, {1,1,1}, error);
    assert(large && scene.Size() == 1);
    assert(scene.AppendDraws(draw) == 0 && draw.Empty());
}
