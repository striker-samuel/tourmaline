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
    budget.Clear();
    CHECK((budget.UpperBounds() == B::Counts{}));
    CHECK(budget.SourceCount() == 0);
}
