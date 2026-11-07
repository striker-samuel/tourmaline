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
#include <Engine/Renderer/gpu_scene_raster.h>
#include <Engine/Renderer/gfx_filter.h>
#include <Engine/RHI/resource_manager.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace tour::gpu_scene {

// Returns false rather than guessing at a possibly discontinuous index range.
[[nodiscard]] inline bool ResolveIndexedGeometry(const smvk::gfx::State& gfx,
                                                 uint32_t meshIndex,
                                                 IndexedGeometry& out) noexcept {
    out = {};
    if (meshIndex >= gfx.meshes.size())
        return false;
    const smvk::gfx::Mesh& mesh = gfx.meshes[meshIndex];
    if (mesh.clusterCount == 0 ||
        mesh.clusterStart > gfx.clusters.size() ||
        mesh.clusterCount > gfx.clusters.size() - mesh.clusterStart)
        return false;
    uint64_t first = 0;
    uint64_t next = 0;
    for (uint32_t i = 0; i < mesh.clusterCount; ++i) {
        const auto& cluster = gfx.clusters[mesh.clusterStart + i];
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
    const uint64_t absolute = uint64_t(smvk::gfx::ClusterIndexBase(gfx)) + first;
    if (absolute > std::numeric_limits<uint32_t>::max())
        return false;
    out.indexCount = uint32_t(next - first);
    out.firstIndex = uint32_t(absolute);
    out.vertexOffset = 0; // cluster-ordered indices already contain heap-global IDs
    return out.indexCount != 0;
}

class IndexedGeometryPublisher {
  public:
    static constexpr uint32_t Flights = smvk::limits::MaxFramesInFlight;
    IndexedGeometryPublisher() = default;
    IndexedGeometryPublisher(const IndexedGeometryPublisher&) = delete;
    IndexedGeometryPublisher& operator=(const IndexedGeometryPublisher&) = delete;

    // Invoke only after acquiring the flight's fence. ResourceManager Holder
    // replacement uses deferred GPU retirement, as in GPU-scene Uploader.
    [[nodiscard]] bool Publish(smvk::ResourceManager& ctx,
                               const smvk::gfx::State& gfx,
                               uint64_t geometryRevision,
                               uint32_t flight) {
        if (flight >= Flights || gfx.meshes.size() > kMaxRasterDrawGroups)
            return false;
        const uint32_t count = uint32_t(gfx.meshes.size());
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
        auto* rows = static_cast<IndexedGeometry*>(
            smvk::FrameArena().Allocate(size_t(count) * sizeof(IndexedGeometry),
                                        alignof(IndexedGeometry)));
        std::uninitialized_value_construct_n(rows, count);
        for (uint32_t i = 0; i < count; ++i)
            (void)ResolveIndexedGeometry(gfx, i, rows[i]);

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

    [[nodiscard]] uint64_t DeviceAddress(const smvk::ResourceManager& ctx,
                                         uint32_t flight) const {
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

} // namespace tour::gpu_scene
