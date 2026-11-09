// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Flight-safe publisher for the indexed ranges of GfxFilter's registered
// cluster-ordered index heap. Gfx mesh IDs and GPU-scene geometryId use the
// same revision-local numbering. This does NOT publish material/pass state.
//
// The descriptor is *not* a general mesh upload: only a contiguous run of
// registered clusters can be represented by one VkDrawIndexedIndirectCommand.
// All other entries publish indexCount=0 so an indirect consumer cannot read
// unrelated index data. Geometry changes invalidate all flight snapshots.

#include <Engine/Memory/arena.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/gfx_filter.h>
#include <Engine/Renderer/gpu_scene.h>
#include <Engine/Renderer/gpu_scene_raster.h>
#include <Engine/Renderer/gpu_scene_upload.h> // RenderDatabase (Database<MaxFramesInFlight>) for Synchronize

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tour::gpu_scene {

// The static GfxFilter geometry tables this publisher reads: a narrow
// pointer+count VIEW, never the whole gfx::State. Engine-facing headers do not
// carry RHI implementation state (the R8 boundary guard in
// tests/test_architecture.cpp); the WorldRenderer narrows its live state here.
struct IndexedGeometryTables {
    const tour::gfx::Mesh* meshes = nullptr;
    uint32_t meshCount = 0;
    const tour::gfx::Cluster* clusters = nullptr;
    uint32_t clusterCount = 0;
    uint32_t indexBase = 0; // absolute firstIndex base of the index heap
};

// Admission guard for streamed LOD metadata. The persistent instance sphere
// is built during topology reconciliation, but geometry-only updates can
// publish newer LOD bounds without rebuilding that sphere. Before allowing
// GPU-scene to own a multi-LOD draw, verify the published sphere contains
// the current AABB of EVERY LOD (resident or deferred). A failed check
// routes the mesh back through the legacy path instead of risking a cull
// from stale bounds.
[[nodiscard]] inline bool RasterSphereContainsLodBounds(glm::vec4 sphere,
                                                        glm::vec3 boundsMin,
                                                        glm::vec3 boundsMax) noexcept {
    if (!std::isfinite(sphere.x) || !std::isfinite(sphere.y) || !std::isfinite(sphere.z) ||
        !std::isfinite(sphere.w) || sphere.w < 0.0f)
        return false;
    const glm::dvec3 center(sphere);
    const glm::dvec3 lo(boundsMin), hi(boundsMax);
    const glm::dvec3 distance = glm::max(glm::abs(lo - center), glm::abs(hi - center));
    // Invalid AABBs/NaNs also fail closed.
    if (!(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z))
        return false;
    return glm::dot(distance, distance) <= double(sphere.w) * double(sphere.w);
}

// Returns false rather than guessing at a possibly discontinuous index range.
[[nodiscard]] inline bool ResolveIndexedGeometry(const IndexedGeometryTables& tables,
                                                 uint32_t meshIndex,
                                                 IndexedGeometry& out) noexcept {
    out = {};
    if (meshIndex >= tables.meshCount)
        return false;
    const tour::gfx::Mesh& mesh = tables.meshes[meshIndex];
    if (mesh.clusterCount == 0 || mesh.clusterStart > tables.clusterCount ||
        mesh.clusterCount > tables.clusterCount - mesh.clusterStart)
        return false;
    uint64_t first = 0;
    uint64_t next = 0;
    for (uint32_t i = 0; i < mesh.clusterCount; ++i) {
        const auto& cluster = tables.clusters[mesh.clusterStart + i];
        if (cluster.mesh != meshIndex || cluster.triCount == 0)
            return false;
        if (i == 0) {
            first = cluster.indexOffset;
            next = first;
        }
        if (cluster.indexOffset != next)
            return false;
        next += uint64_t(cluster.triCount) * 3u;
        if (next > std::numeric_limits<uint32_t>::max())
            return false;
    }
    const uint64_t absolute = uint64_t(tables.indexBase) + first;
    if (absolute > std::numeric_limits<uint32_t>::max())
        return false;
    out.indexCount = uint32_t(next - first);
    out.firstIndex = uint32_t(absolute);
    out.vertexOffset = 0; // cluster-ordered indices already contain heap-global IDs
    return out.indexCount != 0;
}

// Permanent raster geometry identity derives from the GPU-scene mesh's
// GENERATIONAL slot and selected LOD, not the revision-local GfxFilter ID.
// A retired key is never assigned to another mesh: indirect buffers from
// older flights must never alias a different geometry when reused.
struct PersistentGeometryKey {
    uint32_t meshSlot = InvalidIndex;
    uint32_t meshGeneration = 0;
    uint32_t lod = 0;
    bool operator==(const PersistentGeometryKey&) const = default;
};
struct PersistentGeometryKeyHash {
    size_t operator()(const PersistentGeometryKey& key) const noexcept {
        uint64_t x = uint64_t(key.meshSlot) | (uint64_t(key.meshGeneration) << 32);
        x ^= uint64_t(key.lod) * 0x9e3779b97f4a7c15ull;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        return size_t(x ^ (x >> 33));
    }
};

// CPU authority for indexed geometry descriptors. A transient GfxFilter
// registration is only an INPUT for discovering the currently resident
// vertex/index range. It is not the identity of this table, and resetting
// the registrations cannot renumber an existing live persistent key.
//
// Invalid, evicted or missing geometry becomes a zero-index tombstone.
// Rows are never recycled in this first cutover stage: this is conservative
// for in-flight commands, and a future generational reference ABI can add
// flight-fenced recycling. Exhaustion fails closed to legacy rendering.
class PersistentGeometryRegistry {
  public:
    [[nodiscard]] bool Synchronize(const RenderDatabase& db, const IndexedGeometryTables& source) {
        if ((source.meshCount && !source.meshes) || (source.clusterCount && !source.clusters))
            return false;
        auto nextKeys = ids_;
        auto nextRows = rows_;
        // Retire all previous descriptors unless a live DB mesh/LOD
        // explicitly republishes them in this revision.
        std::fill(nextRows.begin(), nextRows.end(), IndexedGeometry{});
        std::vector<uint32_t> nextAliases(source.meshCount, InvalidIndex);
        const auto& meshes = db.meshes.Rows();
        const auto& lods = db.lods.Rows();
        const auto& submeshes = db.submeshes.Rows();
        for (uint32_t meshSlot = 0; meshSlot < meshes.size(); ++meshSlot) {
            const auto& meshRow = meshes[meshSlot];
            if (meshRow.identity.y != 1u)
                continue;
            const Mesh& mesh = meshRow.value;
            if (mesh.lodCount > 16u || mesh.firstLod > lods.size() ||
                mesh.lodCount > lods.size() - mesh.firstLod)
                return false;
            for (uint32_t level = 0; level < mesh.lodCount; ++level) {
                const auto& lodRow = lods[mesh.firstLod + level];
                if (lodRow.identity.y != 1u || lodRow.value.submeshCount != 1u ||
                    lodRow.value.firstSubmesh >= submeshes.size())
                    continue; // not a rasterizable LOD
                const auto& subRow = submeshes[lodRow.value.firstSubmesh];
                if (subRow.identity.y != 1u || (lodRow.value.flags & LodFlagResident) == 0u ||
                    subRow.value.geometryId == InvalidIndex)
                    continue;
                IndexedGeometry descriptor{};
                if (!ResolveIndexedGeometry(source, subRow.value.geometryId, descriptor))
                    continue;
                const PersistentGeometryKey key{meshSlot, meshRow.identity.x, level};
                auto it = nextKeys.find(key);
                uint32_t id = InvalidIndex;
                if (it == nextKeys.end()) {
                    if (nextRows.size() >= kMaxRasterDrawGroups)
                        return false;
                    id = static_cast<uint32_t>(nextRows.size());
                    nextKeys.emplace(key, id);
                    nextRows.emplace_back();
                } else {
                    id = it->second;
                }
                nextRows[id] = descriptor;
                uint32_t& alias = nextAliases[subRow.value.geometryId];
                // One transient registration must not resolve to two
                // different draw ranges. Its alias may be shared by DB
                // LOD records that deliberately point at the same mesh.
                if (alias == InvalidIndex)
                    alias = id;
                else if (nextRows[alias].indexCount != descriptor.indexCount ||
                         nextRows[alias].firstIndex != descriptor.firstIndex ||
                         nextRows[alias].vertexOffset != descriptor.vertexOffset)
                    return false;
            }
        }
        if (nextAliases != transientAliases_)
            ++aliasRevision_;
        if (nextRows.size() != rows_.size() ||
            !std::equal(nextRows.begin(),
                        nextRows.end(),
                        rows_.begin(),
                        rows_.end(),
                        [](const IndexedGeometry& a, const IndexedGeometry& b) {
                            return a.indexCount == b.indexCount && a.firstIndex == b.firstIndex &&
                                   a.vertexOffset == b.vertexOffset && a.flags == b.flags;
                        })) {
            ++revision_;
            if (revision_ == 0)
                ++revision_;
        }
        ids_.swap(nextKeys);
        rows_.swap(nextRows);
        transientAliases_.swap(nextAliases);
        return true;
    }

    [[nodiscard]] uint32_t Find(PersistentGeometryKey key) const noexcept {
        const auto it = ids_.find(key);
        return it == ids_.end() ? InvalidIndex : it->second;
    }
    [[nodiscard]] uint32_t FromTransient(uint32_t meshId) const noexcept {
        return meshId < transientAliases_.size() ? transientAliases_[meshId] : InvalidIndex;
    }
    [[nodiscard]] std::span<const IndexedGeometry> Descriptors() const noexcept {
        return rows_;
    }
    [[nodiscard]] uint64_t Revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] uint64_t AliasRevision() const noexcept {
        return aliasRevision_;
    }
    [[nodiscard]] std::span<const uint32_t> TransientAliases() const noexcept {
        return transientAliases_;
    }

  private:
    std::unordered_map<PersistentGeometryKey, uint32_t, PersistentGeometryKeyHash> ids_;
    std::vector<IndexedGeometry> rows_;
    std::vector<uint32_t> transientAliases_;
    uint64_t revision_ = 1;
    uint64_t aliasRevision_ = 1;
};

// Per-acquired-flight GPU snapshot of the stable descriptor slots.
// Unlike the compatibility IndexedGeometryPublisher below, descriptor
// indices here do not change when the filter registration order changes.
class PersistentIndexedGeometryPublisher {
  public:
    static constexpr uint32_t Flights = smvk::limits::MaxFramesInFlight;
    [[nodiscard]] bool Publish(smvk::ResourceManager& ctx,
                               const PersistentGeometryRegistry& registry,
                               uint32_t flight) {
        if (flight >= Flights)
            return false;
        const auto rows = registry.Descriptors();
        const auto aliases = registry.TransientAliases();
        if (rows.empty() || aliases.empty()) {
            counts_[flight] = 0;
            aliasCounts_[flight] = 0;
            return true;
        }
        if (revisions_[flight] == registry.Revision() &&
            aliasRevisions_[flight] == registry.AliasRevision() && counts_[flight] == rows.size() &&
            aliasCounts_[flight] == aliases.size() && !buffers_[flight].empty() &&
            !aliasBuffers_[flight].empty())
            return true;
        if (capacity_[flight] < rows.size() || buffers_[flight].empty()) {
            size_t count = std::max<size_t>(16, capacity_[flight]);
            while (count < rows.size())
                count *= 2;
            auto next = ctx.CreateBuffer(count * sizeof(IndexedGeometry),
                                         smvk::BufferType_Storage,
                                         "GPU scene persistent indexed geometry");
            if (next.empty())
                return false;
            buffers_[flight] = std::move(next); // Holder retires old physical buffer
            capacity_[flight] = uint32_t(count);
        }
        if (aliasCapacity_[flight] < aliases.size() || aliasBuffers_[flight].empty()) {
            size_t count = std::max<size_t>(16, aliasCapacity_[flight]);
            while (count < aliases.size())
                count *= 2;
            auto next = ctx.CreateBuffer(count * sizeof(uint32_t),
                                         smvk::BufferType_Storage,
                                         "GPU scene transient geometry alias map");
            if (next.empty())
                return false;
            aliasBuffers_[flight] = std::move(next);
            aliasCapacity_[flight] = uint32_t(count);
        }
        ctx.UploadDataToBuffer(aliasBuffers_[flight],
                               aliases.data(),
                               aliases.size_bytes(),
                               smvk::PipelineStage(smvk::PipelineStageBit_Compute),
                               smvk::AccessFlags::ShaderRead);
        ctx.UploadDataToBuffer(buffers_[flight],
                               rows.data(),
                               rows.size_bytes(),
                               smvk::PipelineStage(smvk::PipelineStageBit_Compute),
                               smvk::AccessFlags::ShaderRead);
        counts_[flight] = static_cast<uint32_t>(rows.size());
        aliasCounts_[flight] = static_cast<uint32_t>(aliases.size());
        revisions_[flight] = registry.Revision();
        aliasRevisions_[flight] = registry.AliasRevision();
        return true;
    }
    [[nodiscard]] uint64_t DeviceAddress(const smvk::ResourceManager& ctx, uint32_t flight) const {
        if (flight >= Flights || !counts_[flight] || buffers_[flight].empty())
            return 0;
        return ctx.GetBufferDeviceAddress(buffers_[flight]);
    }
    [[nodiscard]] uint32_t Count(uint32_t flight) const noexcept {
        return flight < Flights ? counts_[flight] : 0u;
    }
    [[nodiscard]] uint32_t AliasCount(uint32_t flight) const noexcept {
        return flight < Flights ? aliasCounts_[flight] : 0u;
    }
    [[nodiscard]] uint64_t AliasAddress(const smvk::ResourceManager& ctx, uint32_t flight) const {
        if (flight >= Flights || !aliasCounts_[flight] || aliasBuffers_[flight].empty())
            return 0;
        return ctx.GetBufferDeviceAddress(aliasBuffers_[flight]);
    }

  private:
    std::array<smvk::Holder<smvk::BufferHandle>, Flights> buffers_{};
    std::array<smvk::Holder<smvk::BufferHandle>, Flights> aliasBuffers_{};
    std::array<uint32_t, Flights> capacity_{};
    std::array<uint32_t, Flights> aliasCapacity_{};
    std::array<uint32_t, Flights> counts_{};
    std::array<uint32_t, Flights> aliasCounts_{};
    std::array<uint64_t, Flights> revisions_{};
    std::array<uint64_t, Flights> aliasRevisions_{};
};

class IndexedGeometryPublisher {
  public:
    static constexpr uint32_t Flights = smvk::limits::MaxFramesInFlight;
    IndexedGeometryPublisher() = default;
    IndexedGeometryPublisher(const IndexedGeometryPublisher&) = delete;
    IndexedGeometryPublisher& operator=(const IndexedGeometryPublisher&) = delete;

    // Invoke only after acquiring the flight's fence. ResourceManager Holder
    // replacement uses deferred GPU retirement, as in GPU-scene Uploader.
    [[nodiscard]] bool Publish(smvk::ResourceManager& ctx,
                               const IndexedGeometryTables& tables,
                               uint64_t geometryRevision,
                               uint32_t flight) {
        if (flight >= Flights || tables.meshCount > kMaxRasterDrawGroups)
            return false;
        const uint32_t count = tables.meshCount;
        if (count == 0) {
            counts_[flight] = 0;
            revisions_[flight] = geometryRevision;
            return true;
        }
        if (revisions_[flight] == geometryRevision && counts_[flight] == count &&
            !buffers_[flight].empty())
            return true;

        // Build complete immutable geometry snapshot in FrameArena, never
        // heap-backed std::vector scratch. It is uploaded before frame reset.
        auto* rows = static_cast<IndexedGeometry*>(smvk::FrameArena().Allocate(
            size_t(count) * sizeof(IndexedGeometry), alignof(IndexedGeometry)));
        std::uninitialized_value_construct_n(rows, count);
        for (uint32_t i = 0; i < count; ++i)
            (void)ResolveIndexedGeometry(tables, i, rows[i]);

        if (buffers_[flight].empty() || capacity_[flight] < count) {
            size_t next = std::max<size_t>(16, capacity_[flight]);
            while (next < count)
                next *= 2;
            auto buffer = ctx.CreateBuffer(next * sizeof(IndexedGeometry),
                                           smvk::BufferType_Storage,
                                           "GPU scene indexed geometry");
            if (buffer.empty()) {
                std::destroy_n(rows, count);
                return false;
            }
            buffers_[flight] = std::move(buffer);
            capacity_[flight] = uint32_t(next);
        }
        ctx.UploadDataToBuffer(buffers_[flight],
                               rows,
                               size_t(count) * sizeof(IndexedGeometry),
                               smvk::PipelineStage(smvk::PipelineStageBit_Compute),
                               smvk::AccessFlags::ShaderRead);
        std::destroy_n(rows, count);
        counts_[flight] = count;
        revisions_[flight] = geometryRevision;
        return true;
    }

    [[nodiscard]] uint64_t DeviceAddress(const smvk::ResourceManager& ctx, uint32_t flight) const {
        if (flight >= Flights || buffers_[flight].empty() || counts_[flight] == 0)
            return 0;
        return ctx.GetBufferDeviceAddress(buffers_[flight]);
    }
    [[nodiscard]] uint32_t Count(uint32_t flight) const {
        return flight < Flights ? counts_[flight] : 0;
    }

  private:
    std::array<smvk::Holder<smvk::BufferHandle>, Flights> buffers_{};
    std::array<uint32_t, Flights> capacity_{};
    std::array<uint32_t, Flights> counts_{};
    std::array<uint64_t, Flights> revisions_ = [] {
        std::array<uint64_t, Flights> r{};
        r.fill(~uint64_t(0));
        return r;
    }();
};

// Per-view rebase data used by the GPU-scene VS. The renderer's existing
// absolute VP is translated into the same camera-relative space used by
// DoubleFloatRelative. Rendering far beyond float camera authority still
// requires a genuine double-precision camera construction.
class RasterViewPublisher {
  public:
    static constexpr uint32_t Flights = smvk::limits::MaxFramesInFlight;

    [[nodiscard]] bool Publish(smvk::ResourceManager& ctx,
                               uint32_t flight,
                               const glm::mat4& worldViewProjection,
                               const glm::dvec3& cameraWorld) {
        if (flight >= Flights)
            return false;
        if (buffers_[flight].empty()) {
            buffers_[flight] = ctx.CreateBuffer(sizeof(RasterViewData),
                                                smvk::BufferType_Storage,
                                                "GPU scene camera-relative raster view");
            if (buffers_[flight].empty())
                return false;
        }
        glm::dmat4 translated(1.0);
        translated[3] = glm::dvec4(cameraWorld, 1.0);
        RasterViewData view{};
        view.relativeViewProjection = glm::mat4(glm::dmat4(worldViewProjection) * translated);
        view.origin = tour::math::SplitDoubleFloat(cameraWorld);
        ctx.UploadDataToBuffer(buffers_[flight],
                               &view,
                               sizeof(view),
                               smvk::PipelineStage(smvk::PipelineStageBit_Vertex),
                               smvk::AccessFlags::ShaderRead);
        return true;
    }

    [[nodiscard]] uint64_t DeviceAddress(const smvk::ResourceManager& ctx, uint32_t flight) const {
        if (flight >= Flights || buffers_[flight].empty())
            return 0;
        return ctx.GetBufferDeviceAddress(buffers_[flight]);
    }

  private:
    std::array<smvk::Holder<smvk::BufferHandle>, Flights> buffers_{};
};

} // namespace tour::gpu_scene
