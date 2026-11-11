// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Device-free oracle for the optional task/mesh temporal phase gate.
// Stable instance slot + generation is the identity; task/work IDs are NOT.
#include <algorithm>
#include <cstdint>
#include <span>

namespace tour::gpu_scene {
enum class TaskVisibilityPhase : uint32_t { Full = 0, Seed = 1, Remainder = 2 };
// Selectable by the task/mesh host when temporal rendering is implemented.
enum class TaskVisibilityGranularity : uint32_t { Instance = 0, Meshlet = 1 };
// Every entry keys a single meshlet IN A SINGLE INSTANCE. Must be sorted
// lexicographically, with no duplicates, and invalidated on geometry republish.
struct TaskMeshletHistoryEntry {
    uint32_t instanceSlot = UINT32_MAX;
    uint32_t instanceGeneration = 0;
    uint32_t meshletId = UINT32_MAX;
    uint32_t reserved = 0;
};
static_assert(sizeof(TaskMeshletHistoryEntry) == 16);
[[nodiscard]] inline bool ValidMeshletHistory(
    std::span<const TaskMeshletHistoryEntry> history) noexcept {
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& cur = history[i];
        if (cur.instanceSlot == UINT32_MAX || cur.instanceGeneration == 0 ||
            cur.meshletId == UINT32_MAX || cur.reserved != 0)
            return false;
        if (i) {
            const auto& prev = history[i - 1];
            const bool ordered = prev.instanceSlot < cur.instanceSlot ||
                (prev.instanceSlot == cur.instanceSlot &&
                 (prev.instanceGeneration < cur.instanceGeneration ||
                  (prev.instanceGeneration == cur.instanceGeneration &&
                   prev.meshletId < cur.meshletId)));
            if (!ordered) return false;
        }
    }
    return true;
}

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
// Exact, collision-free meshlet membership oracle; mirrors the task-stage
// binary search over a read-only sorted GPU snapshot.
[[nodiscard]] inline bool TaskMeshletInPhase(
    TaskVisibilityIdentity current, uint32_t meshletId,
    std::span<const TaskMeshletHistoryEntry> previous,
    TaskVisibilityPhase phase, bool live = true) noexcept {
    if (!live || current.generation == 0 || current.slot == UINT32_MAX ||
        meshletId == UINT32_MAX) return false;
    if (phase == TaskVisibilityPhase::Full) return true;
    const auto less = [](const TaskMeshletHistoryEntry& a,
                         const TaskMeshletHistoryEntry& b) noexcept {
        if (a.instanceSlot != b.instanceSlot) return a.instanceSlot < b.instanceSlot;
        if (a.instanceGeneration != b.instanceGeneration)
            return a.instanceGeneration < b.instanceGeneration;
        return a.meshletId < b.meshletId;
    };
    const TaskMeshletHistoryEntry key{current.slot, current.generation, meshletId, 0};
    const auto it = std::lower_bound(previous.begin(), previous.end(), key, less);
    const bool seen = it != previous.end() &&
        it->instanceSlot == key.instanceSlot &&
        it->instanceGeneration == key.instanceGeneration &&
        it->meshletId == key.meshletId;
    if (phase == TaskVisibilityPhase::Seed) return seen;
    if (phase == TaskVisibilityPhase::Remainder) return !seen;
    return false;
}
} // namespace tour::gpu_scene
