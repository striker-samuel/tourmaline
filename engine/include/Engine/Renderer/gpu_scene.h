// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
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

inline constexpr uint32_t InvalidIndex = UINT32_MAX;

// Every table has persistent SLOT indices. Generation + live status travel
// with each GPU row so future consumers can reject stale references.
template <typename Payload> struct alignas(16) Row {
    Payload value{};
    glm::uvec4 identity{0u}; // x = generation, y = alive (0/1), z/w reserved
};

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
// Scene::ditherCloseup is subtree-inherited. Keep this independent of the
// MASK bit: the fragment shader needs to know whether to screen-door, while
// the depth and main passes must choose MASK pipelines even for opaque mats.
inline constexpr uint32_t InstanceFlagDitherCloseup = 1u << 13;

[[nodiscard]] constexpr uint32_t WithCloseupDitherRasterFlags(
    uint32_t flags, bool ditherCloseup) noexcept {
    return ditherCloseup
        ? flags | InstanceFlagDitherCloseup | InstanceFlagMasked
        : flags;
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
static_assert(sizeof(Submesh) == 16 && sizeof(Instance) == 112);
static_assert(sizeof(Leaf) == 64 && sizeof(LeafMember) == 16);
static_assert(sizeof(Row<Mesh>) == 32);
static_assert(sizeof(Row<Lod>) == 32);
static_assert(sizeof(Row<Submesh>) == 32);
static_assert(sizeof(Row<Instance>) == 128);
static_assert(sizeof(Row<Leaf>) == 80);
static_assert(sizeof(Row<LeafMember>) == 32);
static_assert(offsetof(Row<Instance>, identity) == 112);
static_assert(std::is_trivially_copyable_v<Row<Instance>>);
static_assert(std::is_standard_layout_v<Row<Instance>>);

template <typename Payload> struct Handle {
    uint32_t slot = InvalidIndex;
    uint32_t generation = 0;
    [[nodiscard]] explicit operator bool() const noexcept {
        return slot != InvalidIndex && generation != 0;
    }
    [[nodiscard]] bool operator==(const Handle&) const = default;
};

// FrameCount is the number of independently synchronized GPU copies.
// Each changed slot is queued at most once PER flight, even if edited many
// times before that flight is reused. This avoids an O(scene-size) scan.
template <typename Payload, uint32_t FrameCount> class Table {
    static_assert(FrameCount > 0 && FrameCount <= 32);
    static_assert(std::is_standard_layout_v<Payload>);
    static_assert(std::is_trivially_copyable_v<Payload>);
    static_assert(alignof(Payload) == 16);
    static_assert(sizeof(Payload) % 16 == 0);

  public:
    using Value = Payload;
    using Entry = Row<Payload>;
    using Id = Handle<Payload>;

    [[nodiscard]] uint32_t Size() const noexcept {
        return static_cast<uint32_t>(rows_.size()); // includes tombstones
    }
    [[nodiscard]] uint32_t LiveCount() const noexcept {
        return liveCount_;
    }
    [[nodiscard]] const std::vector<Entry>& Rows() const noexcept {
        return rows_;
    }

    [[nodiscard]] Id Insert(const Payload& value) {
        uint32_t slot;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
        } else {
            if (rows_.size() >= InvalidIndex)
                return {};
            slot = static_cast<uint32_t>(rows_.size());
            rows_.push_back({});
            pendingMasks_.push_back(0);
            rows_.back().identity.x = 1;
        }
        Entry& row = rows_[slot];
        assert(row.identity.x != 0 && row.identity.y == 0);
        row.value = value;
        row.identity.y = 1;
        ++liveCount_;
        Touch(slot);
        return Id{slot, row.identity.x};
    }

    // One mesh's LOD records are accessed as firstLod + level on the GPU.
    // The normal generational Insert() cannot guarantee adjacency when
    // free slots are recycled. Allocate a contiguous group instead.
    // This runs at mesh registration/chain reshaping, NOT per frame.
    // Allocation-free caller-facing contiguous insertion. The output span is
    // supplied by the caller (stack for <=16 mesh LODs; FrameArena for leaves).
    // The compatibility vector overload below is retained for older callers.
    [[nodiscard]] bool InsertContiguous(std::span<const Payload> payloads,
                                        std::span<Id> ids) {
        if (payloads.empty() || payloads.size() > InvalidIndex ||
            ids.size() < payloads.size())
            return false;
        const uint32_t count = static_cast<uint32_t>(payloads.size());
        uint32_t first = InvalidIndex;
        if (free_.size() >= count) {
            // Scratch belongs to FrameArena; no malloc-backed temporary sort.
            auto* ordered = static_cast<uint32_t*>(
                smvk::FrameArena().Allocate(free_.size() * sizeof(uint32_t), 16));
            std::copy(free_.begin(), free_.end(), ordered);
            std::sort(ordered, ordered + free_.size());
            for (size_t i = 0; i + count <= free_.size(); ++i) {
                if (ordered[i + count - 1] - ordered[i] == count - 1) {
                    first = ordered[i];
                    break;
                }
            }
        }
        if (first != InvalidIndex) {
            const uint64_t end = uint64_t(first) + count;
            free_.erase(std::remove_if(free_.begin(), free_.end(),
                                       [&](uint32_t slot) { return slot >= first && slot < end; }),
                        free_.end());
        } else {
            if (payloads.size() > size_t(InvalidIndex) - rows_.size())
                return false;
            first = static_cast<uint32_t>(rows_.size());
            rows_.resize(rows_.size() + count);
            pendingMasks_.resize(rows_.size(), 0);
            for (uint32_t i = 0; i < count; ++i)
                rows_[first + i].identity.x = 1;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t slot = first + i;
            Entry& row = rows_[slot];
            assert(row.identity.x != 0 && row.identity.y == 0);
            row.value = payloads[i];
            row.identity.y = 1;
            ++liveCount_;
            Touch(slot);
            ids[i] = Id{slot, row.identity.x};
        }
        return true;
    }

    // Backward compatibility for CPU-only tooling/tests; production scene
    // publication uses the caller-buffer overload above.
    [[nodiscard]] std::vector<Id> InsertContiguous(std::span<const Payload> payloads) {
        if (payloads.empty() || payloads.size() > InvalidIndex)
            return {};
        std::vector<Id> ids(payloads.size());
        if (!InsertContiguous(payloads, std::span<Id>(ids)))
            return {};
        return ids;
    }

    [[nodiscard]] bool Contains(Id id) const noexcept {
        return id && id.slot < rows_.size() && rows_[id.slot].identity.x == id.generation &&
               rows_[id.slot].identity.y == 1;
    }

    [[nodiscard]] const Payload* Get(Id id) const noexcept {
        return Contains(id) ? &rows_[id.slot].value : nullptr;
    }

    [[nodiscard]] bool Update(Id id, const Payload& value) {
        if (!Contains(id))
            return false;
        rows_[id.slot].value = value;
        Touch(id.slot);
        return true;
    }

    [[nodiscard]] bool Erase(Id id) {
        if (!Contains(id))
            return false;
        Entry& row = rows_[id.slot];
        row.value = {}; // tombstone must be GPU-visible
        row.identity.y = 0;
        --liveCount_;
        // Avoid ABA on uint32 generation wrap; permanently retire the slot
        // instead of allowing a decades-old handle to become valid again.
        if (row.identity.x != UINT32_MAX) {
            ++row.identity.x;
            free_.push_back(id.slot);
        }
        Touch(id.slot);
        return true;
    }

    // Writer signature: bool(size_t destinationByteOffset,
    //                         const void* source, size_t byteCount).
    // Return false to keep ALL changes pending so a failed/partial upload
    // can be retried. No GPU calls here: headless tests use byte mirrors.
    // forceFull is mandatory on a newly allocated/resized flight buffer.
    template <typename Writer>
    [[nodiscard]] bool Publish(uint32_t flight, bool forceFull, Writer&& write) {
        if (flight >= FrameCount)
            return false;
        auto& indices = pending_[flight];
        if (forceFull) {
            if (!rows_.empty() && !write(0, rows_.data(), rows_.size() * sizeof(Entry)))
                return false;
        } else if (!indices.empty()) {
            std::sort(indices.begin(), indices.end());
            size_t first = indices[0];
            size_t end = first + 1;
            const auto flush = [&](size_t lo, size_t hi) {
                return write(lo * sizeof(Entry), rows_.data() + lo, (hi - lo) * sizeof(Entry));
            };
            for (size_t k = 1; k < indices.size(); ++k) {
                if (indices[k] == end) {
                    ++end;
                } else {
                    if (!flush(first, end))
                        return false;
                    first = indices[k];
                    end = first + 1;
                }
            }
            if (!flush(first, end))
                return false;
        }
        for (uint32_t index : indices)
            pendingMasks_[index] &= ~(uint32_t(1) << flight);
        indices.clear();
        return true;
    }

    [[nodiscard]] size_t PendingRows(uint32_t flight) const noexcept {
        return flight < FrameCount ? pending_[flight].size() : 0;
    }

  private:
    void Touch(uint32_t slot) {
        for (uint32_t f = 0; f < FrameCount; ++f) {
            const uint32_t bit = uint32_t(1) << f;
            if (!(pendingMasks_[slot] & bit)) {
                pendingMasks_[slot] |= bit;
                pending_[f].push_back(slot);
            }
        }
    }

    std::vector<Entry> rows_;
    std::vector<uint32_t> pendingMasks_;
    std::vector<uint32_t> free_;
    std::array<std::vector<uint32_t>, FrameCount> pending_{};
    uint32_t liveCount_ = 0;
};

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
