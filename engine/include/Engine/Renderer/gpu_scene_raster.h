// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Indexed raster ABI for GPU-scene-generated indirect graphics commands.
// This is distinct from transient meshlet geometryId. A publisher must
// resolve the actual index/vertex ranges before a raster work item can use it.
// ABI mirrors engine/shaders/gpu_scene_raster_drawbuild.cs.hlsl.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tour::gpu_scene {

inline constexpr uint32_t kMaxRasterDrawGroups = 65535;

// Immutable, GPU-addressable indexed-geometry descriptor. One per compatible
// raster geometry registration; managed separately from per-frame visibility.
struct alignas(16) IndexedGeometry {
    uint32_t indexCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t flags = 0;  // reserved; not a residency/generation proof
};

// One group of compacted visible references, after GPU selection/grouping.
// firstInstance is an offset into the per-flight visible-reference stream.
struct alignas(16) RasterGroup {
    uint32_t geometryIndex = 0;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t flags = 0;
};

// VkDrawIndexedIndirectCommand exact ABI; written in full by compute.
struct RasterIndirectCommand {
    uint32_t indexCount = 0;
    uint32_t instanceCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0;
};

// Stable identity carried in a transient visible-reference list.
struct InstanceReference {
    uint32_t slot = 0xffffffffu;
    uint32_t generation = 0;
};

// Push constants for the independent indirect raster command build kernel.
struct RasterDrawBuildPush {
    uint64_t indexedGeometry = 0;
    uint64_t groups = 0;
    uint64_t indirect = 0;
    uint32_t groupCount = 0;
    uint32_t geometryCount = 0;
};

static_assert(sizeof(IndexedGeometry) == 16 && alignof(IndexedGeometry) == 16);
static_assert(sizeof(RasterGroup) == 16 && alignof(RasterGroup) == 16);
static_assert(sizeof(RasterIndirectCommand) == 20);
static_assert(sizeof(InstanceReference) == 8);
static_assert(sizeof(RasterDrawBuildPush) == 32);
static_assert(offsetof(RasterIndirectCommand, firstInstance) == 16);
static_assert(std::is_trivially_copyable_v<RasterIndirectCommand>);

[[nodiscard]] inline RasterIndirectCommand MakeRasterCommand(
    const IndexedGeometry& geometry, const RasterGroup& group) noexcept {
    return RasterIndirectCommand{
        geometry.indexCount, group.instanceCount, geometry.firstIndex,
        geometry.vertexOffset, group.firstInstance
    };
}
} // namespace tour::gpu_scene
