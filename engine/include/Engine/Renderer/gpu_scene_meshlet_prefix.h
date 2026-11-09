// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Reference for GPU Scene meshlet candidate expansion before #301 PSO
// compaction. The key is a DENSE candidate stream: no four-bin fixed lanes.
#include <Engine/Renderer/gpu_scene_pso_prefix.h>
#include <Engine/Renderer/gpu_scene_raster.h>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace tour::gpu_scene {
inline constexpr uint32_t kMeshletCandidateLimit = kPsoPrefixMaxWork;
struct MeshletRange {
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    // One whole-submesh fallback command if meshlet metadata is unavailable.
    bool indexedFallback = false;
};
struct MeshletCandidate {
    uint32_t workIndex = 0;
    uint32_t clusterIndex = kPsoPrefixInvalid;
    uint32_t rasterClass = kPsoPrefixInvalid;
    bool fullSubmesh = false;
};
struct MeshletPrefixPlan {
    std::vector<uint32_t> offsets;
    std::vector<MeshletCandidate> candidates;

    // Count -> exclusive scan -> scatter with transactional overflow.
    // Every original work item contributes either all its meshlets, or
    // exactly one fallback indexed draw. Never truncate a mesh's tail.
    [[nodiscard]] static bool Build(std::span<const MeshletRange> ranges,
                                    std::span<const uint32_t> rasterClasses,
                                    uint32_t capacity,
                                    MeshletPrefixPlan& destination) {
        if (ranges.size() != rasterClasses.size() ||
            ranges.size() > kMeshletCandidateLimit ||
            capacity > kMeshletCandidateLimit)
            return false;
        MeshletPrefixPlan next;
        next.offsets.resize(ranges.size() + 1, 0u);
        uint64_t total = 0;
        for (size_t i = 0; i < ranges.size(); ++i) {
            const auto& r = ranges[i];
            const bool usable = r.clusterCount != 0 && !r.indexedFallback &&
                r.firstCluster <= std::numeric_limits<uint32_t>::max() - r.clusterCount;
            const uint32_t count = usable ? r.clusterCount : 1u;
            total += count;
            if (total > capacity || total > kMeshletCandidateLimit)
                return false;
            next.offsets[i + 1] = uint32_t(total);
        }
        next.candidates.reserve(size_t(total));
        for (size_t i = 0; i < ranges.size(); ++i) {
            const auto& r = ranges[i];
            const bool usable = r.clusterCount != 0 && !r.indexedFallback &&
                r.firstCluster <= std::numeric_limits<uint32_t>::max() - r.clusterCount;
            const uint32_t count = next.offsets[i + 1] - next.offsets[i];
            for (uint32_t j = 0; j < count; ++j) {
                next.candidates.push_back({
                    uint32_t(i),
                    usable ? r.firstCluster + j : kPsoPrefixInvalid,
                    rasterClasses[i] < kPsoPrefixClasses
                        ? rasterClasses[i] : kPsoPrefixInvalid,
                    !usable
                });
            }
        }
        destination = std::move(next);
        return true;
    }
};
} // namespace tour::gpu_scene
