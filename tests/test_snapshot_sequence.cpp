// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only PNG chunk + keyframe/delta codec + RNG checkpoint tests.
// No renderer, GPU or SDL initialization.
#include <Engine/App/input_tape.h>
#include <Engine/App/snapshot_sequence.h>
#include <Engine/App/world_snapshot.h>
#include <Engine/Core/Random.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::cerr << "CHECK failed: " << #condition << " line " << __LINE__ << "\n";           \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

using tour::snapshot::Json;
using tour::snapshot::SequenceWriter;

int main() {
    tour::RandomService service(42);
    const uint32_t first = service.Stream("combat").NextU32();
    (void)first;
    const auto checkpoint = service.Capture();
    const auto next = service.Stream("combat").NextU32();
    (void)service.Stream("other").NextU32();
    service.Restore(checkpoint);
    CHECK(service.Seed() == 42);
    CHECK(service.Stream("combat").NextU32() == next);
    CHECK(service.Capture().streams.size() == 1);

    // The snapshot used by the renderer is stable, typed, and includes
    // engine RNG plus the scene's generational identity, not pointer data.
    {
        tour::World world;
        const int node = tour::scene::addNode(world.scene, -1, 0);
        CHECK(node >= 0);
        auto before = tour::snapshot::WorldState(
            world, service, glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
        CHECK(before["scope"] == "engine-scene-graph-v1");
        CHECK(before["rng"]["seed"] == 42);
        CHECK(before["nodes"].size() == 1);
        CHECK(before["scene"]["node_free_list"].is_array());
        CHECK(before["scene"]["node_slot_generations"].is_array());
        CHECK(before["asset_references"]["meshes"].is_array());
        world.scene.localTransform[static_cast<size_t>(node)][3][0] = 7.0f;
        auto after = tour::snapshot::WorldState(
            world, service, glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
        auto patch = Json::diff(before, after);
        CHECK(!patch.empty());
        CHECK(before.patch(patch) == after);
        CHECK(tour::snapshot::Fnv64(before.dump()) != tour::snapshot::Fnv64(after.dump()));
    }

    const auto root = std::filesystem::temp_directory_path() /
                      ("tour_snapshot_test_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const uint8_t png[] = {137, 80,  78, 71,  13,  10,  26, 10, 0,  0,  0,   13,  73, 72,
                           68,  82,  0,  0,   0,   1,   0,  0,  0,  1,  8,   6,   0,  0,
                           0,   31,  21, 196, 137, 0,   0,  0,  13, 73, 68,  65,  84, 120,
                           156, 99,  16, 84,  50,  118, 1,  0,  1,  89, 0,   171, 19, 42,
                           37,  171, 0,  0,   0,   0,   73, 69, 78, 68, 174, 66,  96, 130};
    std::string error;
    SequenceWriter writer;
    CHECK(writer.Configure(root, 2, 0, 3, "sequence-test", error));
    CHECK(!writer.Configure(root, 0, 0, 3, "invalid", error));

    const std::vector<uint8_t> rgba{0x11, 0x22, 0x33, 0x44};
    for (uint64_t i = 0; i < 3; ++i) {
        Json state = {{"scope", "engine-scene-graph-v1"},
                      {"nodes", {{"0:0", {{"x", i}, {"alive", true}}}}},
                      {"rng", {{"seed", 42}}}};
        Json events = Json::array();
        if (i == 0)
            events.push_back({{"type", 3}, {"key", 26}});
        CHECK(writer.Stage(i, 16666667, state, events, Json{{"code_identity", "test"}}, error));
        CHECK(writer.Written() == i); // only a successful PNG advances
        const auto target = writer.FramePath();
        const auto pending = target.string() + ".pending.png";
        {
            std::ofstream output(pending, std::ios::binary);
            CHECK(bool(output));
            output.write(reinterpret_cast<const char*>(png), sizeof(png));
            CHECK(bool(output));
        }
        CHECK(writer.Complete(pending, rgba, error));
        std::filesystem::rename(pending, target);
        // FramePath already advanced: the stored output was i+1.
        const auto expected = root / ("Frame_000" + std::to_string(i + 1) + ".png");
        CHECK(std::filesystem::is_regular_file(expected));
        const auto size = std::filesystem::file_size(expected);
        CHECK(size > sizeof(png) + 50);
        std::ifstream input(expected, std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(input)), {});
        CHECK(contents.find("tsNp") != std::string::npos);
        CHECK(contents.find(i == 1 ? "delta" : "key") != std::string::npos);
    }
    CHECK(writer.Written() == 3);
    CHECK(writer.FramePath().filename() == "Frame_0004.png");

    // Raw RGBA + JSON sidecars preserve the key/delta chain without PNG
    // compression. Writing an existing raw sequence must fail closed.
    const auto rawRoot = root / "raw";
    std::filesystem::create_directories(rawRoot);
    SequenceWriter rawWriter;
    CHECK(rawWriter.Configure(rawRoot, 2, 0, 3, "raw-sequence", error));
    for (uint64_t i = 0; i < 3; ++i) {
        Json state{{"scope", "engine-scene-graph-v1"}, {"value", i}};
        CHECK(rawWriter.Stage(i,
                              16666667,
                              state,
                              Json::array(),
                              Json{{"image_width", 1}, {"image_height", 1}},
                              error));
        CHECK(rawWriter.CompleteRaw(rgba, error));
        CHECK(std::filesystem::file_size(
                  rawRoot / ("Frame_000" + std::to_string(i + 1) + ".rgba")) == rgba.size());
        std::ifstream meta(rawRoot / ("Frame_000" + std::to_string(i + 1) + ".json"));
        Json record;
        meta >> record;
        CHECK(record["frame"] == i + 1);
        CHECK(record["pixels_fnv64"] == tour::snapshot::Fnv64(rgba));
        CHECK(record["kind"] == (i == 0 || i == 2 ? "key" : "delta"));
        if (i == 1)
            CHECK(record["previous_payload_fnv64"].is_string());
    }
    CHECK(rawWriter.Written() == 3);
    SequenceWriter noOverwrite;
    CHECK(!noOverwrite.Configure(rawRoot, 2, 0, 3, "other", error));
    std::filesystem::remove_all(root);
    std::cout << "snapshot_sequence_tests: passed\n";
    return 0;
}
