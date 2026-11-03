// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Shared, headless validation for values typed or dragged in the inspector.
// ImGui can yield NaN/Inf (or be handed one by a corrupted asset); never
// propagate those values into a scene, material, or undo command.
#include <cmath>
#include <cstddef>

namespace editor::numeric {

inline bool Finite(float value) { return std::isfinite(value); }

inline float Display(float value, float fallback = 0.0f) {
    return Finite(value) ? value : fallback;
}

inline bool AllFinite(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        if (!Finite(values[i]))
            return false;
    return true;
}

} // namespace editor::numeric
