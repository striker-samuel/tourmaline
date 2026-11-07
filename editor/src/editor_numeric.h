// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Shared, headless validation for values typed or dragged in the inspector.
// ImGui can yield NaN/Inf (or be handed one by a corrupted asset); never
// propagate those values into a scene, material, or undo command.
#include <cmath>
#include <cstddef>

namespace editor::numeric {

inline bool Finite(float value) {
    return std::isfinite(value);
}

inline float Display(float value, float fallback = 0.0f) {
    return Finite(value) ? value : fallback;
}

// Canonical display value for a fixed-decimal editor control. A small
// negative number that rounds to zero must show +0.000, never -0.000.
// Only the *candidate* sent to ImGui is normalized; scene data is not
// quantized merely by drawing an inspector row.
inline float DisplayFixed(float value, unsigned decimals = 3, float fallback = 0.0f) {
    value = Display(value, fallback);
    const float halfUnit = 0.5f * std::pow(10.0f, -float(decimals));
    return std::abs(value) < halfUnit ? 0.0f : value;
}

inline bool AllFinite(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        if (!Finite(values[i]))
            return false;
    return true;
}

} // namespace editor::numeric
