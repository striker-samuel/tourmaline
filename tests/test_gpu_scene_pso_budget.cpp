// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/gpu_scene_pso_budget.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    using B = tour::gpu_scene::RasterPsoBudget;
    B budget;
    const std::array<B::Counts, 3> lods{{
        {{3, 0, 1, 0}}, {{1, 2, 0, 0}}, {{2, 0, 2, 1}}
    }};
    const auto worst = B::WorstLod(lods);
    assert((worst == B::Counts{3, 2, 2, 1}));

    assert(budget.Upsert(1001, worst));
    assert(budget.Upsert(1002, worst)); // two instances of one mesh
    assert((budget.UpperBounds() == B::Counts{6, 4, 4, 2}));
    assert(budget.SourceCount() == 2);

    // Idempotent publication and material class migration.
    assert(budget.Upsert(1001, worst));
    assert((budget.UpperBounds() == B::Counts{6, 4, 4, 2}));
    assert(budget.Upsert(1001, {{0, 0, 5, 0}}));
    assert((budget.UpperBounds() == B::Counts{3, 2, 7, 1}));

    // Failed oversized mutations must not change previously safe totals.
    assert(!budget.Upsert(1003, {{0, 0, UINT32_MAX, 0}}));
    assert((budget.UpperBounds() == B::Counts{3, 2, 7, 1}));
    assert(budget.SourceCount() == 2);

    assert(budget.Erase(1002));
    assert(!budget.Erase(1002));
    assert((budget.UpperBounds() == B::Counts{0, 0, 5, 0}));
    budget.Clear();
    assert((budget.UpperBounds() == B::Counts{}));
    assert(budget.SourceCount() == 0);
}
