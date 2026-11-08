// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Runtime capacity planning for the legacy GfxFilter cutover. A GPU
// allocation must be sized from the actual per-view counts, never the
// shader's historical maximum. The plan is deliberately pure CPU so
// boundary/overflow cases can run in the headless test suite.
#include <cstddef>
#include <cstdint>
#include <limits>

namespace tour::gfx {

struct DynamicCapacity {
    uint32_t draws = 0;
    uint32_t candidates = 0;
    uint32_t survivors = 0;
};

inline constexpr size_t kDynamicPageBytes = 64u * 1024u;

[[nodiscard]] inline constexpr bool RoundCapacity(size_t entries,
                                                  size_t bytesPerEntry,
                                                  size_t& capacity) noexcept {
    capacity = 0;
    if (entries == 0)
        return true;
    if (bytesPerEntry == 0 || bytesPerEntry > kDynamicPageBytes)
        return false;
    const size_t pageRows = kDynamicPageBytes / bytesPerEntry;
    const size_t pages = 1 + (entries - 1) / pageRows;
    if (pages > std::numeric_limits<uint32_t>::max() / pageRows)
        return false;
    capacity = pages * pageRows;
    return true;
}

// Coupled allocations must use one coherent snapshot: each candidate
// requires one Batch, indexed-indirect and temporal seed/remainder command,
// plus two kept-bit parity regions. Draw capacity also determines the
// cull-state counter/diagnostic offsets and GPU-skinning bounds tail.
[[nodiscard]] inline constexpr bool PlanDynamicCapacity(size_t drawCount,
                                                        size_t candidateCount,
                                                        size_t drawBytes,
                                                        size_t candidateBytes,
                                                        DynamicCapacity& result) noexcept {
    size_t drawCapacity = 0;
    size_t candidateCapacity = 0;
    if (!RoundCapacity(drawCount, drawBytes, drawCapacity) ||
        !RoundCapacity(candidateCount, candidateBytes, candidateCapacity))
        return false;
    result.draws = uint32_t(drawCapacity);
    result.candidates = uint32_t(candidateCapacity);
    // Survivor/index-monoblock capacity is independent; don't silently
    // enlarge the static heap when allocating cheap candidate scratch.
    result.survivors = 65536u;
    return true;
}

// Vulkan 1.3 guarantees at least 65535 workgroups in both X and Y.
// Flatten as gid.y * 65535 + gid.x in the matching compute shader.
struct DispatchGrid {
    uint32_t x = 0;
    uint32_t y = 0;
};
inline constexpr uint32_t kDispatchAxisGroups = 65535u;

[[nodiscard]] inline constexpr bool PlanDispatchGrid(size_t groups,
                                                      DispatchGrid& result) noexcept {
    result = {};
    if (groups == 0)
        return true;
    const size_t y = 1 + (groups - 1) / kDispatchAxisGroups;
    if (y > kDispatchAxisGroups)
        return false;
    result.x = uint32_t(groups < kDispatchAxisGroups ? groups : kDispatchAxisGroups);
    result.y = uint32_t(y);
    return true;
}

} // namespace tour::gfx
