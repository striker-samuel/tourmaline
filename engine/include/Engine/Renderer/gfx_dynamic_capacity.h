// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Runtime capacity rounding for the legacy GfxFilter's per-view buffers.
// A GPU allocation must be sized from the actual per-view counts, never the
// shader's historical maximum. Pure CPU so boundary/overflow cases can run
// in the headless test suite.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>

namespace tour::gfx {

inline constexpr size_t kDynamicPageBytes = 64u * 1024u;

[[nodiscard]] inline constexpr bool RoundCapacity(size_t entries,
                                                  size_t bytesPerEntry,
                                                  size_t& capacity) noexcept {
    capacity = 0;
    if (entries == 0)
        return true;
    if (bytesPerEntry == 0 || bytesPerEntry > kDynamicPageBytes)
        return false;
    // LCM(pageBytes, stride) / stride: the byte size really is page-aligned
    // even for strides such as 24 that do not divide 64 KiB.
    const size_t pageRows = kDynamicPageBytes / std::gcd(kDynamicPageBytes, bytesPerEntry);
    const size_t pages = 1 + (entries - 1) / pageRows;
    if (pages > std::numeric_limits<uint32_t>::max() / pageRows)
        return false;
    capacity = pages * pageRows;
    return true;
}

} // namespace tour::gfx
