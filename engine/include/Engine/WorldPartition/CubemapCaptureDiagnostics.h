// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Diagnostics for offline cubemap capture planning. Keep measurement separate
// from capture selection: equal probe counts are NOT proof that tier sets are
// equivalent, and requested density is NOT a guaranteed achieved density.
#include <Engine/WorldPartition/CubemapCapturePlanning.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace engine::worldpartition {

struct CubemapCaptureQuality {
    size_t probes = 0;
    float minInfluenceMeters = 0.0f;
    float medianInfluenceMeters = 0.0f;
    float maxInfluenceMeters = 0.0f;
    size_t matchingPreviousPositions = 0;
    bool hasPrevious = false;
    bool equivalentToPrevious = false;
};

// Compare *positions* across the same cell in consecutive tiers using an
// injective matching. A probe in the previous tier cannot satisfy multiple
// current probes. Equivalence also tests influence/fade radii and depths.
// These are placement diagnostics, not reflection quality metrics.
inline CubemapCaptureQuality AnalyzeCubemapCaptureQuality(
    const CubemapTileCaptureSet& current,
    const CubemapTileCaptureSet* previous = nullptr,
    float toleranceMeters = 0.01f) {
    CubemapCaptureQuality result;
    result.probes = current.probes.size();
    std::vector<float> radii;
    radii.reserve(current.probes.size());
    for (const auto& probe : current.probes)
        radii.push_back(probe.influenceRadius);
    if (!radii.empty()) {
        std::sort(radii.begin(), radii.end());
        result.minInfluenceMeters = radii.front();
        result.medianInfluenceMeters = radii[radii.size() / 2];
        result.maxInfluenceMeters = radii.back();
    }
    if (!previous)
        return result;
    result.hasPrevious = true;
    result.equivalentToPrevious = current.probes.size() == previous->probes.size();
    std::vector<bool> used(previous->probes.size(), false);
    const float tolerance = std::max(0.0f, toleranceMeters);
    const float toleranceSquared = tolerance * tolerance;
    for (const auto& probe : current.probes) {
        bool matched = false;
        for (size_t i = 0; i < previous->probes.size(); ++i) {
            if (used[i])
                continue;
            const auto& other = previous->probes[i];
            const glm::vec3 delta = probe.position - other.position;
            if (glm::dot(delta, delta) > toleranceSquared)
                continue;
            used[i] = true;
            matched = true;
            ++result.matchingPreviousPositions;
            if (probe.depth != other.depth ||
                std::abs(probe.influenceRadius - other.influenceRadius) > tolerance ||
                std::abs(probe.fadeRadius - other.fadeRadius) > tolerance)
                result.equivalentToPrevious = false;
            break;
        }
        if (!matched)
            result.equivalentToPrevious = false;
    }
    return result;
}

} // namespace engine::worldpartition
