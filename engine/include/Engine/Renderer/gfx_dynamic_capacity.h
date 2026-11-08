// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Pure, CPU-only capacity arithmetic for per-flight GeometryFX/GfxFilter
// storage. Capacity is independent of draw and candidate *counts*.
// Buffers are one contiguous BDA apiece; VMA handles the underlying bins.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace smvk::gfx {

inline constexpr uint32_t kGfxCapacityPageBytes = 64u * 1024u;

struct GrowableCapacity {
    uint32_t draws = 0;
    uint32_t candidates = 0;
};

// A page contains a whole number of rows. Preserve the old allocation
// until replacement is fully live; an acquired flight may grow, but must
// never shrink while a previous flight can read its old buffer.
[[nodiscard]] constexpr bool GrowCapacity(uint32_t used,
                                           uint32_t old,
                                           size_t elementBytes,
                                           uint32_t& output) noexcept {
    output = old;
    if (used <= old)
        return true;
    if (elementBytes == 0 || elementBytes > std::numeric_limits<uint32_t>::max())
        return false;
    const uint64_t rowsPerPage =
        (std::max)(uint64_t{1}, uint64_t(kGfxCapacityPageBytes) / elementBytes);
    const uint64_t pages = (uint64_t(used) + rowsPerPage - 1u) / rowsPerPage;
    const uint64_t capacity = pages * rowsPerPage;
    if (capacity > std::numeric_limits<uint32_t>::max() ||
        capacity > std::numeric_limits<size_t>::max() / elementBytes)
        return false;
    output = uint32_t(capacity);
    return true;
}

[[nodiscard]] constexpr bool GrowGfxCapacities(
    uint32_t usedDraws,
    uint32_t usedCandidates,
    uint32_t oldDraws,
    uint32_t oldCandidates,
    size_t drawBytes,
    size_t candidateBytes,
    GrowableCapacity& out) noexcept {
    uint32_t draws = 0;
    uint32_t candidates = 0;
    if (!GrowCapacity(usedDraws, oldDraws, drawBytes, draws) ||
        !GrowCapacity(usedCandidates, oldCandidates, candidateBytes, candidates))
        return false;
    out.draws = draws;
    out.candidates = candidates;
    return true;
}

} // namespace smvk::gfx
