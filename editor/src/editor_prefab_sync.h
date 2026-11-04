// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Headless prefab definition -> reference-instance live preview policy.
// The SOURCE stays untouched on the parent side; all parent changes are
// reversible and never enter the parent's .tour command history.
// Local instance overrides ALWAYS win.
#include <glm/mat4x4.hpp>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <span>
#include <utility>

namespace editor::prefab_sync {

// Map legacy UUID-less nodes using explicit CLUSTER-LOCAL IDs rather than
// assuming the merged World node index is local+1 (holes and reference
// composition make that assumption unsafe).
template<typename NeedsUuid>
inline std::vector<std::pair<uint32_t, uint32_t>>
LegacyIdentityCandidates(std::span<const uint32_t> localToMerged,
                         NeedsUuid shouldMint) {
    std::vector<std::pair<uint32_t, uint32_t>> result;
    for (uint32_t local=0;local<localToMerged.size();++local) {
        const uint32_t merged=localToMerged[local];
        if (merged!=UINT32_MAX && shouldMint(merged))
            result.emplace_back(local,merged);
    }
    return result;
}

// Resolve exactly one node by a stable identity within an occurrence.
// Never choose the first match when records are ambiguous or missing.
template<typename Predicate>
inline uint32_t ResolveUnique(uint32_t count, Predicate matches) {
    uint32_t found = UINT32_MAX;
    for (uint32_t n = 0; n < count; ++n) {
        if (!matches(n)) continue;
        if (found != UINT32_MAX) return UINT32_MAX;
        found = n;
    }
    return found;
}

inline bool Near(const glm::mat4& a, const glm::mat4& b,
    float tolerance = 0.0005f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(a[c][r]) || !std::isfinite(b[c][r])
                || std::abs(a[c][r] - b[c][r]) > tolerance)
                return false;
    return true;
}

// Apply one source node to one reference occurrence, preserving its
// authored instance placement, transform overrides and concurrently
// edited parent state. No-op when the source has no unsaved delta.
// Returns true only when the caller should mark the node changed.
inline bool Apply(
    std::unordered_map<uint32_t, glm::mat4>& originals,
    std::unordered_map<uint32_t, glm::mat4>& applied,
    uint32_t parentNode,
    const glm::mat4& sourceSaved,
    const glm::mat4& sourceCurrent,
    glm::mat4& parentCurrent,
    bool instanceOverride) {
    auto previous = applied.find(parentNode);
    if (instanceOverride) {
        // The parent owns this node now. Do not undo another editor's
        // modification when the prefab window closes.
        originals.erase(parentNode);
        if (previous != applied.end()) applied.erase(previous);
        return false;
    }
    const bool edited = !Near(sourceSaved, sourceCurrent);
    auto original = originals.find(parentNode);
    if (original == originals.end()) {
        if (!edited || !Near(parentCurrent, sourceSaved))
            return false; // custom instance transform already present
        original = originals.emplace(parentNode, parentCurrent).first;
    }
    if (previous != applied.end() &&
        !Near(parentCurrent, previous->second)) {
        // Parent changed this occurrence since the last preview update.
        originals.erase(parentNode);
        applied.erase(previous);
        return false;
    }
    const glm::mat4 next = edited ? sourceCurrent : original->second;
    const bool touched = !Near(parentCurrent, next);
    if (touched) parentCurrent = next;
    if (edited) applied[parentNode] = next;
    else {
        originals.erase(parentNode);
        applied.erase(parentNode);
    }
    return touched;
}

// Restore a tracked occurrence after Cancel/Close. Only restore nodes
// still exactly at the pose OUR preview wrote.
inline bool RestoreOne(
    std::unordered_map<uint32_t, glm::mat4>& originals,
    std::unordered_map<uint32_t, glm::mat4>& applied,
    uint32_t parentNode, glm::mat4& parentCurrent) {
    auto old = originals.find(parentNode);
    auto expected = applied.find(parentNode);
    if (old == originals.end() || expected == applied.end()) return false;
    const bool safe = Near(parentCurrent, expected->second);
    const bool changed = safe && !Near(parentCurrent, old->second);
    if (changed) parentCurrent = old->second;
    originals.erase(old);
    applied.erase(expected);
    return changed;
}

// Successful Save commits the current source pose as the new baseline
// for any later unsaved edit/revert sequence.
inline void Commit(
    std::unordered_map<uint32_t, glm::mat4>& originals,
    const std::unordered_map<uint32_t, glm::mat4>& applied) {
    for (auto& [node, original] : originals) {
        const auto it = applied.find(node);
        if (it != applied.end()) original = it->second;
    }
}

} // namespace editor::prefab_sync
