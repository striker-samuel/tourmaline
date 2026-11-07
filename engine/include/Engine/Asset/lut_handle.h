// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include <cstdint>

// A LOADED color-grading LUT. The compiled project::luts key names the
// asset; this handle names the runtime-loaded resource.
namespace app {

struct LutHandle {
    std::uint64_t value = 0;
    constexpr bool operator==(const LutHandle&) const = default;
    [[nodiscard]] constexpr explicit operator bool() const {
        return value != 0;
    }
};

} // namespace app
