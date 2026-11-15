// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// CPU-only tests: no pak payloads, Vulkan device or renderer required.
#include <Engine/Renderer/pak_linear_pso_bins.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
int failures = 0;
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
} } while (0)
}

int main() {
    using namespace tour::gpu_scene;
    static_assert(kRasterPipelineClassCount == 8);

    const std::array<PakDrawClass, 6> possible{{
        {3, 10}, {0, 11}, {7, 12}, {3, 13}, {0, 14}, {4, 15}
    }};
    PakPsoCapacityPlan plan{};
    CHECK(PrecountPakPsoBins(possible, plan));
    CHECK(plan.drawCapacity == 6);
    CHECK(plan.bins[0].offset == 0 && plan.bins[0].count == 2);
    CHECK(plan.bins[3].offset == 2 && plan.bins[3].count == 2);
    CHECK(plan.bins[4].offset == 4 && plan.bins[4].count == 1);
    CHECK(plan.bins[7].offset == 5 && plan.bins[7].count == 1);

    std::array<uint32_t, 6> storage{};
    FrameLinearSceneDesc published{};
    CHECK(PackFramePsoBins(possible, plan, storage, published, 3, 41));
    CHECK(ValidFramePsoBins(published));
    CHECK(published.generation == 41 && published.meshCount == 3);
    CHECK((storage == std::array<uint32_t, 6>{11, 14, 10, 13, 15, 12}));

    // Streaming/LOD means the active draw set may be a strict subset.
    const std::array<PakDrawClass, 3> subset{{
        {3, 10}, {7, 12}, {0, 14}
    }};
    CHECK(PackFramePsoBins(subset, plan, storage, published, 3, 42));
    CHECK(ValidFramePsoBins(published));
    CHECK(published.drawCount == 3 && published.drawCapacity == 6);
    CHECK(storage[0] == 14 && storage[1] == 10 && storage[2] == 12);

    // A malformed request never publishes a partial snapshot.
    const auto old = published;
    const auto oldStorage = storage;
    std::array<PakDrawClass, 1> invalid{{{8, 99}}};
    CHECK(!PackFramePsoBins(invalid, plan, storage, published, 3, 43));
    CHECK(published.generation == old.generation);
    CHECK(storage == oldStorage);
    CHECK(!PrecountPakPsoBins(invalid, plan));
    CHECK(plan.drawCapacity == 6);

    // Reject invalid capacity layouts and per-class overflow.
    auto broken = plan;
    broken.bins[1].offset = 77;
    CHECK(!PackFramePsoBins(subset, broken, storage, published, 3, 44));
    CHECK(published.generation == old.generation);
    const std::array<PakDrawClass, 3> overClass{{
        {7, 1}, {7, 2}, {7, 3}
    }};
    CHECK(!PackFramePsoBins(overClass, plan, storage, published, 3, 45));
    CHECK(published.generation == old.generation);

    const std::array<PakDrawClass, 0> empty{};
    PakPsoCapacityPlan emptyPlan{};
    CHECK(PrecountPakPsoBins(empty, emptyPlan));
    FrameLinearSceneDesc emptyScene{};
    CHECK(PackFramePsoBins(empty, emptyPlan, std::span<uint32_t>{},
                           emptyScene, 0, 1));
    CHECK(ValidFramePsoBins(emptyScene));

    std::fprintf(stderr, "PakLinearPsoBinsTests: %d checks, %d failures\n",
                 checks, failures);
    return failures ? 1 : 0;
}
