// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// CPU reference for the proposed GPU Scene task -> mesh raster work mapping.
// This is a headless oracle, not a Vulkan draw consumer.
#include <Engine/Renderer/gpu_scene_meshlet_prefix.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace tour::gpu_scene {
inline constexpr uint32_t kTaskMeshletGroupSize = 32u;
struct TaskMeshletGroup {
    uint32_t workSlot = 0;
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    uint32_t rasterClass = 0;
};
struct TaskMeshletPlan {
    std::array<std::vector<TaskMeshletGroup>, kRasterPipelineClassCount> classes{};
    std::vector<uint32_t> indexedFallbackWork;
    // Build a lossless task-work plan: no invalid meshlet ranges are
    // submitted to mesh stages. Such work is retained for indexed fallback.
    // The capacity gate is transactional: destination is unchanged on failure.
    [[nodiscard]] static bool Build(std::span<const MeshletRange> ranges,
                                    std::span<const uint32_t> rasterClasses,
                                    uint32_t maxGroups,
                                    TaskMeshletPlan& destination) {
        if (ranges.size() != rasterClasses.size() ||
            ranges.size() > kMeshletCandidateLimit)
            return false;
        TaskMeshletPlan next{};
        uint64_t groups = 0;
        for (size_t i = 0; i < ranges.size(); ++i) {
            const MeshletRange& range = ranges[i];
            const uint32_t cls = rasterClasses[i];
            const bool valid = cls < kRasterPipelineClassCount &&
                !range.indexedFallback && range.clusterCount != 0u &&
                range.firstCluster <=
                    std::numeric_limits<uint32_t>::max() - range.clusterCount;
            if (!valid) {
                next.indexedFallbackWork.push_back(uint32_t(i));
                continue;
            }
            const uint64_t count =
                (uint64_t(range.clusterCount) + kTaskMeshletGroupSize - 1u) /
                kTaskMeshletGroupSize;
            groups += count;
            if (groups > maxGroups)
                return false;
            for (uint64_t begin = 0; begin < range.clusterCount;
                 begin += kTaskMeshletGroupSize) {
                next.classes[cls].push_back({
                    uint32_t(i), uint32_t(uint64_t(range.firstCluster) + begin),
                    uint32_t(std::min<uint64_t>(kTaskMeshletGroupSize,
                                                uint64_t(range.clusterCount) - begin)),
                    cls
                });
            }
        }
        destination = std::move(next);
        return true;
    }
};
} // namespace tour::gpu_scene
