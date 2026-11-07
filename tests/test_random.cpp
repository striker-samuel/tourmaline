// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only contract tests for the versioned, deterministic PCG32 stream.
#include <Engine/Core/Random.h>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
int failures = 0;
void Check(bool pass, const char* name) {
    if (!pass) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}
} // namespace

int main() {
    // PCG32 reference vector: seed=42, sequence=54.
    tour::RandomStream reference(42, 54);
    constexpr uint32_t expected[] = {
        0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu};
    for (uint32_t value : expected)
        Check(reference.NextU32() == value, "rng.pcg32-golden-vector");

    tour::RandomService a(123456789), b(123456789);
    auto& aLoot = a.Stream("loot");
    auto& aCombat = a.Stream("combat");
    auto& bCombat = b.Stream("combat");
    auto& bLoot = b.Stream("loot");
    for (int i = 0; i < 32; ++i) {
        Check(aLoot.NextU32() == bLoot.NextU32(), "rng.name-order-independent-loot");
        Check(aCombat.NextU32() == bCombat.NextU32(), "rng.name-order-independent-combat");
    }
    tour::RandomService different(987654321), original(123456789);
    Check(different.Stream("loot").NextU32() != original.Stream("loot").NextU32(),
          "rng.different-master-seeds");
    Check(a.MakeStream("loot").NextU32() == original.MakeStream("loot").NextU32(),
          "rng.fresh-stream-independent-of-consumption");

    tour::RandomStream stream(42, 54);
    for (int i = 0; i < 9; ++i)
        (void)stream.NextU32();
    const auto checkpoint = stream.Save();
    const uint32_t next = stream.NextU32();
    stream.Restore(checkpoint);
    Check(stream.NextU32() == next, "rng.save-restore");
    try {
        stream.Restore({123, 2});
        Check(false, "rng.reject-even-increment");
    } catch (const std::invalid_argument&) {
        Check(true, "rng.reject-even-increment");
    }

    tour::RandomStream bounded(2026);
    for (int i = 0; i < 10000; ++i) {
        Check(bounded.NextBounded(3) < 3, "rng.bounded-range");
        const float f = bounded.NextFloat01();
        Check(f >= 0.0f && f < 1.0f, "rng.float-half-open-range");
    }
    Check(bounded.NextBounded(1) == 0, "rng.bound-one");
    try {
        (void)bounded.NextBounded(0);
        Check(false, "rng.reject-zero-bound");
    } catch (const std::invalid_argument&) {
        Check(true, "rng.reject-zero-bound");
    }
    a.Reseed(123456789);
    Check(a.Gameplay().NextU32() == b.Gameplay().NextU32(), "rng.reseed-resets-streams");
    Check(a.Seed() == 123456789, "rng.exposes-effective-seed");

    std::cout << "rng failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
