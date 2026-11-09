// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/gpu_scene_pso_budget.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "CHECK failed: %s:%d: %s\\n", __FILE__, __LINE__, #cond); \
    std::abort(); \
} } while (false)

int main() {
    using B = tour::gpu_scene::RasterPsoBudget;
    B budget;
    const std::array<B::Counts, 3> lods{{
        {{3, 0, 1, 0}}, {{1, 2, 0, 0}}, {{2, 0, 2, 1}}
    }};
    const auto worst = B::WorstLod(lods);
    CHECK((worst == B::Counts{3, 2, 2, 1}));

    CHECK(budget.Upsert(1001, worst));
    CHECK(budget.Upsert(1002, worst)); // two instances of one mesh
    CHECK((budget.UpperBounds() == B::Counts{6, 4, 4, 2}));
    CHECK(budget.SourceCount() == 2);

    // Idempotent publication and material class migration.
    CHECK(budget.Upsert(1001, worst));
    CHECK((budget.UpperBounds() == B::Counts{6, 4, 4, 2}));
    CHECK(budget.Upsert(1001, {{0, 0, 5, 0}}));
    CHECK((budget.UpperBounds() == B::Counts{3, 2, 7, 1}));

    // Failed oversized mutations must not change previously safe totals.
    CHECK(!budget.Upsert(1003, {{0, 0, UINT32_MAX, 0}}));
    CHECK((budget.UpperBounds() == B::Counts{3, 2, 7, 1}));
    CHECK(budget.SourceCount() == 2);

    CHECK(budget.Erase(1002));
    CHECK(!budget.Erase(1002));
    CHECK((budget.UpperBounds() == B::Counts{0, 0, 5, 0}));
    // Region offsets are exclusive prefixes of the conservative capacity.
    tour::gpu_scene::RasterPsoRegions regions{};
    CHECK(tour::gpu_scene::RasterPsoRegions::Build({{4, 2, 3, 1}}, 10, regions));
    CHECK((regions.offsets == B::Counts{0, 4, 6, 9}));
    CHECK((regions.capacities == B::Counts{4, 2, 3, 1}));
    CHECK(regions.totalCapacity == 10);
    const auto oldRegions = regions;
    CHECK(!tour::gpu_scene::RasterPsoRegions::Build({{4, 2, 3, 2}}, 10, regions));
    CHECK(regions.totalCapacity == oldRegions.totalCapacity);
    CHECK((regions.offsets == oldRegions.offsets));
    CHECK(tour::gpu_scene::RasterPsoRegions::Build({{0, 0, 0, 0}}, 0, regions));
    CHECK(regions.totalCapacity == 0);

    // Same cluster-local slot reused at a new generation is a separate
    // identity. The old generation must retire without affecting the new.
    struct Source {
        uint32_t cluster, slot, generation;
        bool operator==(const Source&) const = default;
    };
    struct Hash {
        size_t operator()(const Source& k) const {
            return (size_t(k.cluster) << 32) ^ (size_t(k.slot) << 16) ^ k.generation;
        }
    };
    tour::gpu_scene::RasterPsoBudgetT<Source, Hash> keyed;
    CHECK(keyed.Upsert({1, 7, 21}, {{1, 0, 0, 0}}));
    CHECK(keyed.Upsert({1, 7, 22}, {{0, 1, 0, 0}}));
    CHECK(keyed.Erase({1, 7, 21}));
    CHECK((keyed.UpperBounds() == B::Counts{0, 1, 0, 0}));

    budget.Clear();
    CHECK((budget.UpperBounds() == B::Counts{}));
    CHECK(budget.SourceCount() == 0);
}
