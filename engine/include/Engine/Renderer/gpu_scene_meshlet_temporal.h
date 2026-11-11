// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace tour::gpu_scene {
enum class MeshletTemporalPhase : uint32_t {
    None=0u, Seed=1u, Remainder=2u, Main=3u
};
struct MeshletTemporalKey {
    uint32_t slot, generation, clusterId;
    bool operator==(const MeshletTemporalKey&) const = default;
};
[[nodiscard]] constexpr bool TemporalMeshletSurvives(
    MeshletTemporalPhase phase, bool previousVisible, bool currentVisible,
    bool frustumConeVisible, bool occludedBySeedHzb,
    bool currentHistoryOverflow=false) noexcept {
    if(!frustumConeVisible) return false;
    switch(phase) {
    case MeshletTemporalPhase::Seed: return previousVisible;
    case MeshletTemporalPhase::Remainder:
        return !previousVisible && !occludedBySeedHzb;
    case MeshletTemporalPhase::Main:
        return currentVisible || currentHistoryOverflow;
    default: return !occludedBySeedHzb;
    }
}
} // namespace tour::gpu_scene
