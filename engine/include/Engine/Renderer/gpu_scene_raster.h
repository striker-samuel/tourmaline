// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Indexed raster ABI for GPU-scene-generated indirect graphics commands.
// This is distinct from transient meshlet geometryId. A publisher must
// resolve the actual index/vertex ranges before a raster work item can use it.
// ABI mirrors engine/shaders/gpu_scene_raster_drawbuild.cs.hlsl.

#include <Engine/Core/double_float.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace tour::gpu_scene {

// World-level raster draw-group budget (2^18). The old 65535 ceiling
// rejected large scenes; the per-flight publisher buffers double to the
// actual geometry count, so this is an admission ceiling, not a reservation.
// HLSL mirror: GS_MAX_GEO (gpu_scene_task_mesh.hlsli).
inline constexpr uint32_t kMaxRasterDrawGroups = 262144;
// Eight fixed PSO classes: bit 0 = double-sided, bit 1 = alpha-mask,
// bit 2 = material shader family (ProjectLit). Classes 0-3 are StandardLit,
// classes 4-7 are the same coverage/culling matrix with the ProjectLit
// fragment shader. Each class occupies one contiguous indirect range; each
// work record selects exactly one class, preserving its original
// firstInstance slot. Depth/G-buffer/motion need no family distinction
// (ProjectLit hooks are color-only), so those passes draw the family bit as
// an extra bin with the SAME program.
inline constexpr uint32_t kRasterPipelineClassCount = 8;
inline constexpr uint32_t kRasterPipelineDoubleSided = 1;
inline constexpr uint32_t kRasterPipelineMasked = 2;
inline constexpr uint32_t kRasterPipelineMaskedDoubleSided = 3;
inline constexpr uint32_t kRasterPipelineProjectLit = 4;

// Data-driven pipeline-class key (the "bin description"): the raster /
// coverage / culling matrix that selects one PSO class. The fixed 8-class
// encoding above is the CURRENT manifest; the key form lets the class set
// grow (more cull modes, raster modes, blend modes, shader families) without
// renumbering the hot path. rasterMode is reserved for the future
// indexed-vs-meshlet / blend split.
struct RasterPipelineKey {
    uint32_t doubleSided = 0; // 0 = back-culled, 1 = no cull
    uint32_t masked = 0;      // 0 = opaque, 1 = alpha-tested cutout
    uint32_t projectLit = 0;  // 0 = StandardLit, 1 = ProjectLit family
    uint32_t rasterMode = 0;  // reserved (future bins)
    bool operator==(const RasterPipelineKey&) const = default;
};

// The current manifest: 8 classes = (doubleSided, masked, projectLit).
// Extensible: a future manifest appends rasterMode / blendMode bits and
// grows kRasterPipelineClassCount without renumbering the low 8.
[[nodiscard]] inline constexpr uint32_t RasterPipelineClass(RasterPipelineKey k) {
    return (k.doubleSided ? kRasterPipelineDoubleSided : 0u) |
           (k.masked ? kRasterPipelineMasked : 0u) |
           (k.projectLit ? kRasterPipelineProjectLit : 0u);
}

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
    uint32_t reserved = 0;        // per-class command stride (work capacity)
    uint64_t geometryAliases = 0; // revision-local ID -> stable descriptor slot
    uint32_t aliasCount = 0;
    uint32_t padding = 0;
};

// CPU oracle for GPU Scene's camera-relative dither input. Subtract the
// real double-precision eye before rounding to shader float: a float32
// absolute-world position cannot express the 0.5m..3m dissolve at 1e12.
[[nodiscard]] inline float GpuSceneCloseupDitherCoverage(const glm::dvec3& worldPosition,
                                                         const glm::dvec3& cameraWorld) {
    constexpr float hideM = 0.5f;
    constexpr float solidM = 3.0f;
    const glm::vec3 relative(worldPosition - cameraWorld);
    const float distance = glm::length(relative);
    return std::clamp((distance - hideM) / (solidM - hideM), 0.0f, 1.0f);
}

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
    // Skinned-instance tail (mirrors gpu_scene_raster.vert.hlsl): the frame's
    // deformed heap (5 words/vertex) + the skinned span's global base. A
    // skinned instance fetches (vertexID - skinDeformedBase) * 5 instead of
    // the bind-pose POSITION. skinValid = 1 only when the frame ran the
    // skinning pass (a null BDA/unknown base must fall back to bind pose,
    // never fault).
    uint64_t skinDeformed = 0;
    uint32_t skinDeformedBase = 0;
    uint32_t skinValid = 0;
};
static_assert(sizeof(RasterViewData) == 96);
static_assert(sizeof(RasterVsPush) == 48);

// Previous-frame instance transform for the batched motion-vector VS.
// Indexed by PERSISTENT instance slot; generation matches the instance
// row's identity.x at snapshot time so a recycled slot never ghosts
// motion. The translation stays DoubleFloat3 (double-precision high/low)
// so moving objects retain the same camera-relative precision as the
// current GPU Scene raster path.
struct alignas(16) GpuScenePrevTransform {
    tour::math::DoubleFloat3 worldPosition{};
    glm::vec4 basisX{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 basisY{0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 basisZ{0.0f, 0.0f, 1.0f, 0.0f};
    uint32_t generation = 0;
    uint32_t pad[3] = {};
};
static_assert(sizeof(GpuScenePrevTransform) == 96);

// Batched motion-vector VS push: the RasterVsPush tail extended with the
// previous-transform buffer, the previous camera-relative view and a
// validity flag. Reuses the identical work/instance/indirect streams as the
// lit raster VS, so the motion pass submits the same visibility set.
struct RasterMotionVsPush {
    uint64_t work = 0;
    uint64_t instances = 0;
    uint64_t view = 0;
    uint32_t instanceSlots = 0;
    uint32_t workSlots = 0;
    uint64_t prevTransforms = 0; // GpuScenePrevTransform[] by slot
    uint64_t prevView = 0;       // RasterViewData (previous origin + relative VP)
    uint32_t prevValid = 0;      // 1 = previous history is usable
    // Number of entries the prevTransforms buffer actually holds (the previous
    // frame's instance count). Streaming can grow the instance set between the
    // snapshot and this frame, so the VS must bound the prev read by THIS, not
    // the (larger) current instanceSlots — an out-of-bounds read faults the GPU.
    uint32_t prevSlots = 0;
};
static_assert(sizeof(RasterMotionVsPush) == 56);

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

// CPU oracle for the fixed eight-bin GPU indirect builder. A command slot
// belongs to exactly one bin; firstInstance always denotes the ORIGINAL
// work index, not the PSO-sorted command index.
[[nodiscard]] inline RasterIndirectCommand MakeRasterBinnedCommand(const IndexedGeometry& geometry,
                                                                   uint32_t workIndex,
                                                                   uint32_t rasterClass,
                                                                   uint32_t bin) noexcept {
    if (rasterClass >= kRasterPipelineClassCount || rasterClass != bin || geometry.indexCount == 0u)
        return EmptyRasterIndirectCommand();
    return RasterIndirectCommand{
        geometry.indexCount, 1u, geometry.firstIndex, geometry.vertexOffset, workIndex};
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
