// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Cheap, CPU-only inspection of the EXACT candidate deduplication key used
// by VulkanResourceManager::BuildRayTracingWorld. No GPU reads or ray dispatch.
#include <Engine/RHI/resource_manager.h>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace smvk {

struct RayTracingDebugSummary {
    struct Instance {
        uint32_t sourceIndex = 0;
        uint32_t blasIndex = 0;
        uint32_t triangles = 0;
    };
    struct Blas {
        uint32_t triangles = 0;
        uint32_t references = 0;
        bool alphaMasked = false;
    };
    std::vector<Instance> instances;
    std::vector<Blas> blases;
    uint32_t rejected = 0;
    uint64_t instancedTriangles = 0;
    uint64_t uniqueTriangles = 0;
};

// Candidate counts, not Vulkan AS memory sizes or internal BVH node counts.
// A failed GPU build can leave this list populated while the TLAS is unready.
inline RayTracingDebugSummary SummarizeRayTracingCandidates(
    std::span<const RayTracingInstance> input) {
    RayTracingDebugSummary out;
    std::map<std::array<uint32_t, 8>, uint32_t> keys;
    for (size_t i = 0; i < input.size(); ++i) {
        const auto& inst = input[i];
        if (inst.baseVertex < 0 || inst.vertexCount < 3 || inst.indexCount < 3 ||
            inst.indexCount % 3 != 0) {
            ++out.rejected;
            continue;
        }
        const std::array<uint32_t, 8> key{inst.firstIndex,
                                          inst.indexCount,
                                          uint32_t(inst.baseVertex),
                                          inst.vertexCount,
                                          inst.opacityMode,
                                          inst.opacityTextureIndex,
                                          std::bit_cast<uint32_t>(inst.opacityFactor),
                                          std::bit_cast<uint32_t>(inst.opacityCutoff)};
        const uint32_t proposed = uint32_t(out.blases.size());
        const auto [it, inserted] = keys.emplace(key, proposed);
        const uint32_t triangles = inst.indexCount / 3;
        if (inserted) {
            out.blases.push_back({triangles, 0, inst.opacityMode == 1u});
            out.uniqueTriangles += triangles;
        }
        ++out.blases[it->second].references;
        out.instances.push_back({uint32_t(i), it->second, triangles});
        out.instancedTriangles += triangles;
    }
    return out;
}

} // namespace smvk
