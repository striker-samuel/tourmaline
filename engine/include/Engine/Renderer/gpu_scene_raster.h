// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Indexed raster ABI for GPU-scene-generated indirect graphics commands.
// This is distinct from transient meshlet geometryId. A publisher must
// resolve the actual index/vertex ranges before a raster work item can use it.
// ABI mirrors engine/shaders/gpu_scene_raster_drawbuild.cs.hlsl.

#include <Engine/Core/double_float.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace tour::gpu_scene {

inline constexpr uint32_t kMaxRasterDrawGroups = 65535;

// Immutable, GPU-addressable indexed-geometry descriptor. One per compatible
// raster geometry registration; managed separately from per-frame visibility.
struct alignas(16) IndexedGeometry {
    uint32_t indexCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t flags = 0; // reserved; not a residency/generation proof
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

// The direct GPU-scene work-to-raster path: each GPU-produced submesh work
// record maps to one indirect command and one SV_InstanceID work slot.
// Work rows already carry the persistent instance (slot,generation) pair.
// This intermediate form does not require CPU per-instance grouping.
struct RasterWorkDrawBuildPush {
    uint64_t work = 0;        // SceneSubmeshWork[] generated on GPU
    uint64_t workOffsets = 0; // workOffsets[candidateCount] = GPU work count
    uint64_t indexedGeometry = 0;
    uint64_t indirect = 0;
    uint32_t candidateCount = 0;
    uint32_t geometryCount = 0;
    uint32_t commandCapacity = 0;
    uint32_t reserved = 0;
};

// Static-opaque raster VS input: world position is rebased against this
// double-float origin before the relative VP is evaluated. The lit fragment
// still uses float world positions like the legacy raster shaders.
struct alignas(16) RasterViewData {
    glm::mat4 relativeViewProjection{1.0f};
    tour::math::DoubleFloat3 origin{};
};

// HLSL RasterVsPush begins at byte offset 192 of the shared 256B
// scene push. Shader locations 0..10 match gfxmesh_lit.frag.hlsl.
inline constexpr uint32_t kRasterVsPushOffset = 192;
struct RasterVsPush {
    uint64_t work = 0;
    uint64_t instances = 0;
    uint64_t view = 0;
    uint32_t instanceSlots = 0;
    uint32_t workSlots = 0;
};
static_assert(sizeof(RasterViewData) == 96);
static_assert(sizeof(RasterVsPush) == 32);

// CPU reference for the GPU seed shader. A work-slot index is NOT a
// visibility identity; only the persistent instance (slot,generation)
// from the current work row may match the previous survivor snapshot.
[[nodiscard]] inline bool RasterTemporalSeedMember(std::span<const uint32_t> previousGenerations,
                                                   InstanceReference instance) noexcept {
    return instance.generation != 0u && instance.slot < previousGenerations.size() &&
           previousGenerations[instance.slot] == instance.generation;
}

// GPU-created temporal commands preserve ALL per-work-slot identities.
// The seed is prev-visible intersect current-frustum-visible, and the
// remainder is its exact complement. Neither list is built on the CPU.
struct RasterSeedPush {
    uint64_t commands = 0;
    uint64_t work = 0;
    uint64_t previous = 0;
    uint64_t seed = 0;
    uint64_t remainder = 0;
    uint32_t commandCapacity = 0;
    uint32_t instanceSlots = 0;
    uint32_t historySlots = 0;
    uint32_t historyValid = 0;
};
static_assert(sizeof(RasterSeedPush) == 56);

struct RasterHistoryPush {
    uint64_t commands = 0;
    uint64_t work = 0;
    uint64_t history = 0;
    uint32_t commandCapacity = 0;
    uint32_t instanceSlots = 0;
    uint32_t historySlots = 0;
    // 0 = clear this flight's snapshot, 1 = commit survivors. The two legs
    // share ONE mainCS entry point (the DXC single-entry-point rule) and
    // the host binds the same pipeline twice with different pushes.
    uint32_t mode = 0;
};
static_assert(sizeof(RasterHistoryPush) == 40);

// Same-frame GPU-scene Hi-Z gate. The raster work builder first publishes
// frustum-visible commands; an occluder depth pass builds the reversed-Z
// MIN pyramid; this compute pass zeros occluded command instance counts.
// The view BDA is the same camera-relative RasterViewData as the raster VS.
struct RasterHizCullPush {
    uint64_t commands = 0;
    uint64_t remainder = 0;
    uint64_t work = 0;
    uint64_t instances = 0;
    uint64_t meshes = 0;
    uint64_t view = 0;
    uint64_t hiz = 0;
    uint64_t previous = 0;
    uint32_t commandCapacity = 0;
    uint32_t instanceSlots = 0;
    uint32_t meshSlots = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 0;
    uint32_t historySlots = 0;
    uint32_t historyValid = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(RasterHizCullPush) == 104);

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
static_assert(sizeof(RasterWorkDrawBuildPush) == 48);
static_assert(offsetof(RasterIndirectCommand, firstInstance) == 16);
static_assert(std::is_trivially_copyable_v<RasterIndirectCommand>);

[[nodiscard]] inline RasterIndirectCommand MakeRasterCommand(const IndexedGeometry& geometry,
                                                             const RasterGroup& group) noexcept {
    return RasterIndirectCommand{geometry.indexCount,
                                 group.instanceCount,
                                 geometry.firstIndex,
                                 geometry.vertexOffset,
                                 group.firstInstance};
}
} // namespace tour::gpu_scene
