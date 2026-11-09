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
#include <algorithm>
#include <span>
#include <type_traits>

namespace tour::gpu_scene {

inline constexpr uint32_t kMaxRasterDrawGroups = 65535;
// Four fixed PSO classes: bit 0 = double-sided, bit 1 = alpha-mask.
// Each class occupies one contiguous indirect range; each work record
// selects exactly one class, preserving its original firstInstance slot.
inline constexpr uint32_t kRasterPipelineClassCount = 4;
inline constexpr uint32_t kRasterPipelineDoubleSided = 1;
inline constexpr uint32_t kRasterPipelineMasked = 2;
inline constexpr uint32_t kRasterPipelineMaskedDoubleSided = 3;


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
    uint32_t reserved = 0; // per-class command stride (work capacity)
    uint64_t geometryAliases = 0; // revision-local ID -> stable descriptor slot
    uint32_t aliasCount = 0;
    uint32_t padding = 0;
};

// Optional meshlet draw builder: the 64-byte prefix is identical to
// RasterWorkDrawBuildPush, so indexed fallback retains its ABI.
inline constexpr uint32_t kRasterMeshletMaxLanes = 1024u;
// Fixed indirect buffers are bounded by the existing 1M work slots per
// raster class. Prefer individual meshlets whenever there is command room;
// an oversized registration is rendered whole in lane zero.
[[nodiscard]] inline uint32_t RasterMeshletLaneCount(
    uint32_t candidateCount, uint32_t maxClusters) noexcept {
    if (candidateCount == 0u || maxClusters == 0u)
        return 1u;
    const uint32_t budget = 1048576u / candidateCount;
    return std::max(1u, std::min({maxClusters, kRasterMeshletMaxLanes, budget}));
}
struct RasterMeshletWorkDrawBuildPush {
    RasterWorkDrawBuildPush raster{};
    uint64_t meshes = 0;     // gfx::MeshEntry[] (transient registered IDs)
    uint64_t clusters = 0;   // gfx::ClusterConstants[] (64-byte bound records)
    uint64_t instances = 0;  // persistent GPU-scene instance table
    uint64_t cullView = 0;   // origin-relative instance-cull view
    uint32_t meshCount = 0;
    uint32_t clusterCount = 0;
    uint32_t indexBase = 0;  // cluster-ordered heap index base
    uint32_t instanceSlots = 0;
    uint32_t meshletLanes = 1;
    uint32_t cullViewValid = 0;
};
static_assert(sizeof(RasterMeshletWorkDrawBuildPush) == 120);
static_assert(offsetof(RasterMeshletWorkDrawBuildPush, meshes) == 64);

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
static_assert(sizeof(RasterWorkDrawBuildPush) == 64);
static_assert(offsetof(RasterIndirectCommand, firstInstance) == 16);
static_assert(std::is_trivially_copyable_v<RasterIndirectCommand>);

// Match the opt-in HLSL debug permutation; never poison indexCount or
// instanceCount. Both must remain zero to keep empty commands harmless.
[[nodiscard]] inline RasterIndirectCommand EmptyRasterIndirectCommand() noexcept {
    RasterIndirectCommand command{};
#if defined(GS_RASTER_DEBUG_EMPTY_COMMAND_SENTINEL) && GS_RASTER_DEBUG_EMPTY_COMMAND_SENTINEL
    command.firstIndex = 0xDEADBEEFu;
    command.vertexOffset = 0x7BADBEEF;
    command.firstInstance = 0xDEADBEEFu;
#endif
    return command;
}

// CPU oracle for the fixed four-bin GPU indirect builder. A command slot
// belongs to exactly one bin; firstInstance always denotes the ORIGINAL
// work index, not the PSO-sorted command index.
[[nodiscard]] inline RasterIndirectCommand MakeRasterBinnedCommand(
    const IndexedGeometry& geometry,
    uint32_t workIndex,
    uint32_t rasterClass,
    uint32_t bin) noexcept {
    if (rasterClass >= kRasterPipelineClassCount || rasterClass != bin ||
        geometry.indexCount == 0u)
        return EmptyRasterIndirectCommand();
    return RasterIndirectCommand{geometry.indexCount, 1u, geometry.firstIndex,
                                 geometry.vertexOffset, workIndex};
}

[[nodiscard]] inline RasterIndirectCommand MakeRasterCommand(const IndexedGeometry& geometry,
                                                             const RasterGroup& group) noexcept {
    return RasterIndirectCommand{geometry.indexCount,
                                 group.instanceCount,
                                 geometry.firstIndex,
                                 geometry.vertexOffset,
                                 group.firstInstance};
}
} // namespace tour::gpu_scene
