// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Device-free oracle for the optional task/mesh temporal phase gate.
// Stable instance slot + generation is the identity; task/work IDs are NOT.
#include <cstdint>
#include <span>

namespace tour::gpu_scene {
enum class TaskVisibilityPhase : uint32_t { Full = 0, Seed = 1, Remainder = 2 };
struct TaskVisibilityIdentity {
    uint32_t slot = UINT32_MAX;
    uint32_t generation = 0;
};
[[nodiscard]] inline bool TaskVisibilityInPhase(
    TaskVisibilityIdentity current, std::span<const uint32_t> previous,
    TaskVisibilityPhase phase, bool live = true) noexcept {
    if (!live || current.generation == 0 || current.slot == UINT32_MAX)
        return false;
    if (phase == TaskVisibilityPhase::Full) return true;
    // Host contract: invalid/missing prior history MUST use Full mode.
    if (current.slot >= previous.size()) return false;
    const bool seen = previous[current.slot] == current.generation;
    if (phase == TaskVisibilityPhase::Seed) return seen;
    if (phase == TaskVisibilityPhase::Remainder) return !seen;
    return false;
}
} // namespace tour::gpu_scene
