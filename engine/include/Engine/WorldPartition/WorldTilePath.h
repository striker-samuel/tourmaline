// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <bit>
#include <cstdint>

namespace engine::worldpartition {

// A binary subdivision path relative to a WorldCellKey root.
// 1 is the root sentinel; each following bit chooses a binary child.
// For 2D tiles, each pair of bits splits X then Z (quadtree order).
// This retains the entire ancestry in one uint64_t, with up to 31
// complete XZ levels (62 path bits). Zero is the invalid sentinel.
// Root-cell coordinates remain separate so the world grid is unbounded.
struct WorldTilePath {
    uint64_t bits = 1;

    static constexpr uint32_t kMaxLevels = 31;

    constexpr bool Valid() const { return bits != 0; }
    constexpr bool IsRoot() const { return bits == 1; }
    constexpr uint32_t Depth() const {
        return bits ? std::bit_width(bits) - 1u : 0u;
    }
    constexpr bool IsXZTile() const { return Valid() && (Depth() % 2u == 0u); }
    constexpr uint32_t Level() const { return Depth() / 2u; }

    constexpr WorldTilePath Parent() const {
        return bits > 1 ? WorldTilePath{bits >> 1} : WorldTilePath{0};
    }
    constexpr WorldTilePath Child(uint32_t side) const {
        return Valid() && Depth() < 63 && side < 2
            ? WorldTilePath{(bits << 1) | side}
            : WorldTilePath{0};
    }

    // Local XZ coordinates at a given quadtree level, not world-cell indices.
    // Returns invalid (bits == 0) on an out-of-range level or coordinate.
    static constexpr WorldTilePath FromXY(uint32_t x, uint32_t z, uint32_t level) {
        if (level > kMaxLevels || x >= (uint32_t{1} << level)
            || z >= (uint32_t{1} << level))
            return {0};
        WorldTilePath result{};
        for (uint32_t i = level; i != 0; --i) {
            result = result.Child((x >> (i - 1)) & 1u);
            result = result.Child((z >> (i - 1)) & 1u);
        }
        return result;
    }

    struct XY {
        uint32_t x = 0;
        uint32_t z = 0;
    };

    // For an XZ (even-depth) path, recover local tile coordinates.
    // Invalid/odd-depth paths return {0,0}; check IsXZTile() first.
    constexpr XY ToXY() const {
        XY result{};
        if (!IsXZTile()) return result;
        const uint32_t level = Level();
        for (uint32_t i = 0; i < level; ++i) {
            const uint32_t shift = 2u * (level - i - 1u);
            result.x = (result.x << 1) | uint32_t((bits >> (shift + 1u)) & 1u);
            result.z = (result.z << 1) | uint32_t((bits >> shift) & 1u);
        }
        return result;
    }

    constexpr bool operator==(const WorldTilePath&) const = default;
};

static_assert(WorldTilePath::FromXY(0, 0, 0).bits == 1);
static_assert(WorldTilePath::FromXY(3, 2, 2).ToXY().x == 3);
static_assert(WorldTilePath::FromXY(3, 2, 2).ToXY().z == 2);
static_assert(WorldTilePath::FromXY(3, 2, 2).Level() == 2);
static_assert(WorldTilePath::FromXY(3, 2, 2).Parent().Parent()
    == WorldTilePath::FromXY(1, 1, 1));
static_assert(!WorldTilePath::FromXY(4, 0, 2).Valid());
static_assert(!WorldTilePath::FromXY(0, 0, 32).Valid());

} // namespace engine::worldpartition
