// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include <Engine/RHI/table.h>
#include <Engine/Renderer/gpu_struct_name.h>
// GPUIR-inspired GPU-scene database: persistent rows and dirty-row publication.
// This is CPU-owned storage; an RHI adapter publishes the byte-identical rows.
// No camera-dependent positions, render passes, or draw candidates live here.
//
// Producer contract: single render/scene thread. Publish only on a flight slot
// whose previous GPU work has retired; frame slots keep independent dirty logs.

#include <Engine/Core/double_float.h>
#include <Engine/Memory/arena.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace tour::gpu_scene {

inline constexpr uint32_t InvalidIndex = smvk::table::InvalidIndex;

template <typename Payload> using Row = smvk::table::Row<Payload>;

// Maximum authored surfaces (indexed ranges) a single LOD may carry. Mirrors
// GS_MAX_SUBMESHES in gpu_scene_work_select.cs.hlsl and the 16-LOD chain cap
// in PublishMesh. A multi-surface mesh (one World::StaticMesh whose geometry
// holds several primitive ranges) expands to one Submesh row per surface, all
// sharing the instance material.
inline constexpr uint32_t kMaxSubmeshesPerLod = 16;

// Every table has persistent SLOT indices. Generation + live status travel
// with each GPU row so future consumers can reject stale references.
GPU_STRUCT_NAME(GpuSceneMesh)
struct alignas(16) Mesh {
    uint32_t firstLod = 0;
    uint32_t lodCount = 0;
    uint32_t residentMask = 0; // advisory until the geometry publisher is wired
    uint32_t flags = 0;
};

// Flags: keep physical payload residency separate from GPU meshlet registration.
inline constexpr uint32_t LodFlagResident = 1u << 0;
inline constexpr uint32_t LodFlagBoundsKnown = 1u << 1;
inline constexpr uint32_t LodFlagFilterable = 1u << 2;
// GPU instance culling must require this flag on the referenced mesh.
inline constexpr uint32_t MeshFlagCompleteBounds = 1u << 0;
// Extra GPU-scene raster classification bit in Instance.refs.w; World
// iterator flags currently occupy bits 0..11. Never encode this on a
// shared Mesh/Lod row: instances of one mesh can use distinct materials.
inline constexpr uint32_t InstanceFlagDoubleSided = 1u << 12;
// World::IteratorFlagBit_Masked = 1u << 10; preserve the source's
// material alpha-mode classification on the persistent instance.
inline constexpr uint32_t InstanceFlagMasked = 1u << 10;
// TransformHierarchy::ditherCloseup is subtree-inherited. Keep this independent of the
// MASK bit: the fragment shader needs to know whether to screen-door, while
// the depth and main passes must choose MASK pipelines even for opaque mats.
inline constexpr uint32_t InstanceFlagDitherCloseup = 1u << 13;
// Material shader family (World::SetMaterialShaderFamily). ProjectLit rides
// the SAME GPU-scene culling/LOD/work stream as StandardLit; only the Main
// fragment shader and its material payload change. This bit extends the four
// coverage/culling raster classes to eight (bit 2 = family).
inline constexpr uint32_t InstanceFlagProjectLit = 1u << 14;
// Skinned instances fetch their positions from the frame's DEFORMED heap
// (the Northlight compute path) instead of the bind-pose vertex stream, and
// skip bind-pose sphere/cone/HZB culling (the animated pose can leave the
// cooked bounds). Mirrored as GPU_SCENE_INSTANCE_SKINNED in gpu_scene.hlsli.
inline constexpr uint32_t InstanceFlagSkinned = 1u << 15;

// Per-instance render overrides (bits 16..18 of Instance.refs.w). These are
// PERSISTENT, cull-derivable facts the work-select reads, so an override
// survives the survivor compaction and needs no per-frame patch pass. The
// producer (cull) honors them; the consumer (draw) never sees them. Keep the
// line between persistent facts (here) and per-frame/view conditions (the
// cull's frame inputs) clean: nothing camera-relative belongs on this row.
inline constexpr uint32_t InstanceFlagForcedDirect = 1u << 16; // cull omits -> direct leg
inline constexpr uint32_t InstanceFlagForceLod0 = 1u << 17;    // LOD select clamps to finest
inline constexpr uint32_t InstanceFlagHighlight = 1u << 18;    // debug: rasterClass override

// Combined override mask (the "is any override set" query).
inline constexpr uint32_t kInstanceOverrideMask =
    InstanceFlagForcedDirect | InstanceFlagForceLod0 | InstanceFlagHighlight;

// The dither-closeup flag set: the authored subtree-inherited bit plus the
// MASK pipeline bit the screen-door path requires. Expressed as flags (not a
// bool) so every flag edit is a plain mask operation.
inline constexpr uint32_t kDitherCloseupFlags = InstanceFlagDitherCloseup | InstanceFlagMasked;

[[nodiscard]] constexpr uint32_t SetInstanceOverride(uint32_t flags, uint32_t mask) noexcept {
    return flags | (mask & kInstanceOverrideMask);
}

[[nodiscard]] constexpr uint32_t ClearInstanceOverride(uint32_t flags, uint32_t mask) noexcept {
    return flags & ~(mask & kInstanceOverrideMask);
}

[[nodiscard]] constexpr bool HasInstanceOverride(uint32_t flags,
                                                 uint32_t mask = kInstanceOverrideMask) noexcept {
    return (flags & (mask & kInstanceOverrideMask)) != 0u;
}

// One-call flag assembly: base flags | the bits to set. Set bits are the
// existing InstanceFlag* constants (no per-feature bools): dither-closeup is
// kDitherCloseupFlags, overrides are InstanceFlagForcedDirect/ForceLod0/
// Highlight.
[[nodiscard]] constexpr uint32_t MakeInstanceFlags(uint32_t baseFlags, uint32_t setMask) noexcept {
    return baseFlags | setMask;
}

GPU_STRUCT_NAME(GpuSceneLod)
struct alignas(16) Lod {
    uint32_t firstSubmesh = 0;
    uint32_t submeshCount = 0;
    float simplifyError = 0.0f; // mesh-local error; scale per instance
    uint32_t flags = 0;
};

GPU_STRUCT_NAME(GpuSceneSubmesh)
struct alignas(16) Submesh {
    uint32_t geometryId = InvalidIndex;
    uint32_t materialId = InvalidIndex;
    uint32_t pipelineGroup = InvalidIndex;
    uint32_t passMask = 0;
};

GPU_STRUCT_NAME(GpuSceneInstance)
struct alignas(16) Instance {
    tour::math::DoubleFloat3 worldPosition{};
    glm::vec4 basisX{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 basisY{0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 basisZ{0.0f, 0.0f, 1.0f, 0.0f};
    glm::vec4 localSphere{0.0f};   // xyz mesh-local center, w radius
    glm::uvec4 refs{InvalidIndex}; // mesh slot, mesh generation, World material, flags
    // Resolved 32-bit material-instance userData (World::ResolvedItemUserData),
    // forwarded to the lit fragment input at location 9. ProjectLit hooks read
    // it for effects (wetness/damage). Zero for StandardLit no-op materials.
    uint32_t userData = 0;
    uint32_t reserved[3] = {};
};

// A leaf is cullable only if EVERY member has a valid, conservative
// full-LOD-chain bound. Otherwise the future GPU pass must bypass leaf cull.
inline constexpr uint32_t LeafFlagBoundsComplete = 1u << 0;

// A leaf contains a contiguous span of LeafMember rows; each member
// points at an Instance slot+generation rather than a frame-local draw.
GPU_STRUCT_NAME(GpuSceneLeafMember)
struct alignas(16) LeafMember {
    glm::uvec4 ref{InvalidIndex, 0u, 0u, 0u};
};

GPU_STRUCT_NAME(GpuSceneLeaf)
struct alignas(16) Leaf {
    tour::math::DoubleFloat3 worldCenter{};
    glm::vec4 halfExtents{0.0f}; // xyz conservative; w reserved
    glm::uvec4 range{0u};        // x first member, y member count, z/w reserved
    // Membership list / spatial traversal is a later PR. A leaf range is
    // not valid for GPU traversal until a separate list has been published.
};

static_assert(sizeof(Mesh) == 16 && sizeof(Lod) == 16);
static_assert(sizeof(Submesh) == 16 && sizeof(Instance) == 128);
static_assert(sizeof(Leaf) == 64 && sizeof(LeafMember) == 16);
static_assert(sizeof(Row<Mesh>) == 32);
static_assert(sizeof(Row<Lod>) == 32);
static_assert(sizeof(Row<Submesh>) == 32);
static_assert(sizeof(Row<Instance>) == 144);
static_assert(sizeof(Row<Leaf>) == 80);
static_assert(sizeof(Row<LeafMember>) == 32);
static_assert(offsetof(Row<Instance>, identity) == 128);
static_assert(offsetof(Instance, userData) == 112);
static_assert(std::is_trivially_copyable_v<Row<Instance>>);
static_assert(std::is_standard_layout_v<Row<Instance>>);

// Scene code retains the old API and shader ABI; storage and publication
// mechanics are implemented in smvk for reuse by other RHI-owned tables.
template <typename Payload> using Handle = smvk::table::Handle<Payload>;
template <typename Payload, uint32_t FrameCount>
using Table = smvk::table::Table<Payload, FrameCount>;

// Stable indices belong to the table, not to the material-sorted CPU
// draw list or a merged scene node. No World association in this first
// PR: registration of authored/runtime instances happens separately.
template <uint32_t FrameCount> struct Database {
    Table<Mesh, FrameCount> meshes;
    Table<Lod, FrameCount> lods;
    Table<Submesh, FrameCount> submeshes;
    Table<Instance, FrameCount> instances;
    Table<Leaf, FrameCount> leaves;
    Table<LeafMember, FrameCount> leafMembers;
};

} // namespace tour::gpu_scene
