// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/gpu_scene_pso_prefix.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #x); \
    std::abort(); \
} } while (false)

int main() {
    using namespace tour::gpu_scene;
    using Counts = PsoPrefixPlan::Counts;
    PsoPrefixPlan plan;
    CHECK(PsoPrefixPlan::Build({}, {{0,0,0,0}}, plan));
    CHECK(plan.destinations.empty());
    CHECK((plan.counts == Counts{0,0,0,0}));

    // Three block boundaries; mostly opaque, multiple invalid work records.
    std::vector<uint32_t> cls(700, 0u);
    for (size_t i = 0; i < cls.size(); ++i) {
        if (i % 17 == 0) cls[i] = 1;
        if (i % 43 == 0) cls[i] = 2;
        if (i % 89 == 0) cls[i] = 3;
        if (i % 101 == 0) cls[i] = kPsoPrefixInvalid;
    }
    Counts expected{};
    for (uint32_t c : cls)
        if (c < kPsoPrefixClasses) ++expected[c];
    CHECK(PsoPrefixPlan::Build(cls, expected, plan));
    CHECK(plan.blockCounts.size() == 3);
    CHECK(plan.blockBases.size() == 3);
    CHECK((plan.counts == expected));

    Counts seen{};
    std::vector<uint8_t> occupied(700, 0);
    for (size_t i = 0; i < cls.size(); ++i) {
        if (cls[i] == kPsoPrefixInvalid) {
            CHECK(plan.destinations[i] == kPsoPrefixInvalid);
            continue;
        }
        const uint32_t c = cls[i];
        const uint32_t expectedDestination = plan.offsets[c] + seen[c]++;
        CHECK(plan.destinations[i] == expectedDestination);
        CHECK(plan.destinations[i] < occupied.size());
        CHECK(!occupied[plan.destinations[i]]);
        occupied[plan.destinations[i]] = 1;
    }
    CHECK((seen == expected));

    // All-opaque congestion: no global atomic append and stable ordering.
    std::vector<uint32_t> skew(1024, 0u);
    CHECK(PsoPrefixPlan::Build(skew, {{1024,0,0,0}}, plan));
    CHECK(plan.blockCounts.size() == 4);
    for (uint32_t i = 0; i < 1024; ++i)
        CHECK(plan.destinations[i] == i);

    // Class capacities may include unused headroom; offset prefix still
    // includes that headroom, never overlapping the following classes.
    std::array<uint32_t, 6> tiny{{0,1,1,3,2,0}};
    CHECK(PsoPrefixPlan::Build(tiny, {{8,3,2,1}}, plan));
    CHECK((plan.offsets == Counts{0,8,11,13}));
    CHECK((plan.counts == Counts{2,2,1,1}));
    CHECK((plan.destinations == std::vector<uint32_t>{0,8,9,13,11,1}));

    // Insufficient per-class capacity must reject without mutating a
    // previously valid publication, including class-specific overflow.
    const auto saved = plan.destinations;
    CHECK(!PsoPrefixPlan::Build(tiny, {{1,3,2,1}}, plan));
    CHECK(plan.destinations == saved);
    CHECK(!PsoPrefixPlan::Build(tiny,
        {{kPsoPrefixMaxWork,3,2,1}}, plan));
    CHECK(plan.destinations == saved);
    return 0;
}
