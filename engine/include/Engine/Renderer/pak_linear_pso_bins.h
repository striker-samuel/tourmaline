// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// CPU-only staging contract for pak-precounted, linear PSO-class draw bins.
// Does not replace the active gfxfilter/GPU-scene raster ownership path.
// The caller classifies each potential draw (including LOD/surface variants)
// during pak metadata enumeration. No allocations occur in these functions.
#include <Engine/Renderer/gpu_scene_raster.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace tour::gpu_scene {

struct LinearDrawRange {
    uint32_t offset = 0;
    uint32_t count = 0;
};

// One class per possible draw candidate, not one class per mesh. This
// distinction matters for multi-surface assets and different LOD materials.
struct PakDrawClass {
    uint32_t psoClass = 0;
    uint32_t sourceIndex = 0; // stable caller-defined pak/cooked draw ID
};

struct PakPsoCapacityPlan {
    std::array<LinearDrawRange, kRasterPipelineClassCount> bins{};
    uint32_t drawCapacity = 0;
};

// Immutable, pointer-free frame-flight snapshot header. All ranges are
// element offsets into the named typed arrays, NOT byte offsets. A published
// header and all referenced arrays must remain alive until that flight retires.
struct FrameLinearSceneDesc {
    std::array<LinearDrawRange, kRasterPipelineClassCount> psoBins{};
    LinearDrawRange instances{};
    LinearDrawRange skinRanges{};
    uint32_t drawCount = 0;
    uint32_t drawCapacity = 0;
    uint32_t meshCount = 0;
    uint32_t generation = 0;
};

static_assert(std::is_trivially_copyable_v<FrameLinearSceneDesc>);

// Precount a pak's complete possible draw set. Do not publish partial counts
// on malformed input or when uint32 addressing cannot represent the result.
[[nodiscard]] inline bool PrecountPakPsoBins(
    std::span<const PakDrawClass> candidates, PakPsoCapacityPlan& out) {
    if (candidates.size() > std::numeric_limits<uint32_t>::max())
        return false;
    PakPsoCapacityPlan next{};
    std::array<uint64_t, kRasterPipelineClassCount> counts{};
    for (const auto& candidate : candidates) {
        if (candidate.psoClass >= kRasterPipelineClassCount)
            return false;
        ++counts[candidate.psoClass];
    }
    uint64_t offset = 0;
    for (uint32_t c = 0; c < kRasterPipelineClassCount; ++c) {
        if (counts[c] > UINT32_MAX || offset + counts[c] > UINT32_MAX)
            return false;
        next.bins[c] = {static_cast<uint32_t>(offset),
                        static_cast<uint32_t>(counts[c])};
        offset += counts[c];
    }
    next.drawCapacity = static_cast<uint32_t>(offset);
    out = next;
    return true;
}

// Pack a single flight from a stable classified set. Caller supplies exactly
// one contiguous array, e.g. bump-allocated after acquiring a retired flight.
// Preflight validation is all-or-nothing: a failure leaves output unchanged.
// Layout is packed by class and retains source order within each class.
// A changing scene must recompute a new plan before publishing that flight.
[[nodiscard]] inline bool PackFramePsoBins(
    std::span<const PakDrawClass> active,
    const PakPsoCapacityPlan& capacity,
    std::span<uint32_t> outputSourceIndices,
    FrameLinearSceneDesc& published,
    uint32_t meshCount,
    uint32_t generation) {
    if (active.size() > capacity.drawCapacity ||
        outputSourceIndices.size() < capacity.drawCapacity ||
        active.size() > UINT32_MAX)
        return false;

    std::array<uint32_t, kRasterPipelineClassCount> counts{};
    for (const auto& candidate : active) {
        if (candidate.psoClass >= kRasterPipelineClassCount ||
            counts[candidate.psoClass] == capacity.bins[candidate.psoClass].count)
            return false;
        ++counts[candidate.psoClass];
    }
    // Capacity layout must itself be contiguous and internally consistent.
    uint64_t capEnd = 0;
    for (const auto& bin : capacity.bins) {
        if (bin.offset != capEnd)
            return false;
        capEnd += bin.count;
    }
    if (capEnd != capacity.drawCapacity)
        return false;

    FrameLinearSceneDesc next{};
    uint32_t offset = 0;
    for (uint32_t c = 0; c < kRasterPipelineClassCount; ++c) {
        next.psoBins[c] = {offset, counts[c]};
        offset += counts[c];
    }
    next.drawCount = offset;
    next.drawCapacity = capacity.drawCapacity;
    next.meshCount = meshCount;
    next.generation = generation;

    // Reuse the local histogram as per-bin scatter cursors. No temporary
    // heap allocation, no per-bin vector growth.
    counts.fill(0);
    for (const auto& candidate : active) {
        const auto c = candidate.psoClass;
        const auto dst = next.psoBins[c].offset + counts[c]++;
        outputSourceIndices[dst] = candidate.sourceIndex;
    }
    published = next;
    return true;
}

[[nodiscard]] inline bool ValidFramePsoBins(const FrameLinearSceneDesc& scene) {
    if (scene.drawCount > scene.drawCapacity)
        return false;
    uint64_t end = 0;
    for (const auto& bin : scene.psoBins) {
        if (bin.offset != end)
            return false;
        end += bin.count;
    }
    return end == scene.drawCount && end <= scene.drawCapacity;
}

} // namespace tour::gpu_scene
