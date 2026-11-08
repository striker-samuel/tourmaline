// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Canonical, per-source raster ownership contract. The source identity is the
// cluster registration + local node slot + generation; drawItems order and
// merged World node indexes are deliberately not identities.
//
// This CPU-only table is a safety boundary for the incremental cutover. The
// caller supplies persistent storage (AppArena) and publishes a complete,
// immutable snapshot after topology/compatibility changes. No allocations,
// GPU work, or legacy draw submissions occur here. A GPU submission can only
// take ownership after ALL required GPU resources and pass variants are ready.
#include <Engine/Renderer/gpu_scene_world.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <tuple>

namespace tour::gpu_scene {

// GPU-scene work selection already chooses the finest registered resident
// LOD. Admit multi-LOD static meshes only when ALL cooked levels have known
// bounds: the instance/leaf sphere is their union, so frustum rejection
// remains conservative even if the selector falls back to a coarser LOD.
// Single-level meshes retain their historical fail-open bound behavior.
[[nodiscard]] inline bool RasterIndexedInstanceReady(
    const RenderDatabase& db, Handle<Instance> id) noexcept {
    const Instance* instance = db.instances.Get(id);
    if (!instance)
        return false;
    const Mesh* mesh = db.meshes.Get({instance->refs.x, instance->refs.y});
    if (!mesh || mesh->lodCount == 0 || mesh->lodCount > 16 ||
        mesh->firstLod >= db.lods.Size() ||
        mesh->lodCount > db.lods.Size() - mesh->firstLod ||
        (mesh->lodCount > 1 && !(mesh->flags & MeshFlagCompleteBounds)))
        return false;

    bool hasResidentGeometry = false;
    for (uint32_t level = 0; level < mesh->lodCount; ++level) {
        const auto& lod = db.lods.Rows()[mesh->firstLod + level];
        if (lod.identity.y != 1)
            return false;
        const bool resident = (mesh->residentMask & (1u << level)) != 0;
        if (resident != ((lod.value.flags & LodFlagResident) != 0))
            return false;
        if (mesh->lodCount > 1 && !(lod.value.flags & LodFlagBoundsKnown))
            return false;
        if (!resident)
            continue;
        if (!(lod.value.flags & LodFlagFilterable) ||
            lod.value.submeshCount != 1 ||
            lod.value.firstSubmesh >= db.submeshes.Size())
            return false;
        const auto& submesh = db.submeshes.Rows()[lod.value.firstSubmesh];
        if (submesh.identity.y != 1 || submesh.value.geometryId == InvalidIndex)
            return false;
        hasResidentGeometry = true;
    }
    return hasResidentGeometry;
}

enum class RasterOwner : uint8_t { Legacy, Gpu };

struct RasterOwnershipEntry {
    SourceKey source{};
    // Compatibility is an admission *candidate*, never an ownership grant.
    bool staticOpaqueCompatible = false;
};

class CanonicalRasterOwnership {
  public:
    // Sort caller-owned AppArena records once on topology or compatibility
    // revision changes. Duplicate/invalid source identities invalidate the
    // WHOLE publication: a partial ownership map cannot be used safely.
    [[nodiscard]] bool Publish(std::span<RasterOwnershipEntry> entries) {
        Reset();
        const auto less = [](const RasterOwnershipEntry& a,
                             const RasterOwnershipEntry& b) {
            return std::tie(a.source.registration, a.source.slot, a.source.generation) <
                   std::tie(b.source.registration, b.source.slot, b.source.generation);
        };
        std::sort(entries.begin(), entries.end(), less);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].source.registration ||
                entries[i].source.slot == InvalidIndex ||
                (i && entries[i - 1].source == entries[i].source))
                return false;
        }
        entries_ = entries;
        valid_ = true;
        return true;
    }

    // Called at each flight boundary BEFORE raster preparation. If compute,
    // geometry publication or graphics setup fails, legacy retains every
    // source, including ones that were GPU-owned on an earlier flight.
    void BeginFrame() noexcept { gpuPrepared_ = false; }
    void CommitGpuPrepared() noexcept { gpuPrepared_ = valid_; }
    void Reset() noexcept {
        entries_ = {};
        valid_ = false;
        gpuPrepared_ = false;
    }

    [[nodiscard]] bool Valid() const noexcept { return valid_; }
    [[nodiscard]] bool GpuActive() const noexcept { return valid_ && gpuPrepared_; }
    [[nodiscard]] size_t Size() const noexcept { return entries_.size(); }

    // Unknown/stale sources ALWAYS remain legacy-owned. This deliberately
    // fails closed when a dirty journal has not yet published a new identity.
    [[nodiscard]] RasterOwner Owner(SourceKey source) const noexcept {
        if (!GpuActive() || !source.registration)
            return RasterOwner::Legacy;
        const auto less = [](const RasterOwnershipEntry& a, const SourceKey& key) {
            return std::tie(a.source.registration, a.source.slot, a.source.generation) <
                   std::tie(key.registration, key.slot, key.generation);
        };
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), source, less);
        return it != entries_.end() && it->source == source && it->staticOpaqueCompatible
                   ? RasterOwner::Gpu
                   : RasterOwner::Legacy;
    }
    [[nodiscard]] bool OwnsGpu(SourceKey source) const noexcept {
        return Owner(source) == RasterOwner::Gpu;
    }
    [[nodiscard]] bool OwnsLegacy(SourceKey source) const noexcept {
        return Owner(source) == RasterOwner::Legacy;
    }
    // Build a per-flight GPU admission mask indexed by persistent instance
    // slots. A value is the CURRENT generation, or zero for legacy-owned
    // slots. Upload the whole mask before dispatch; the shader must compare
    // both slot and generation before emitting a visible verdict.
    // Compatibility is frozen at snapshot publication, while the caller
    // decides whether this frame actually commits GPU ownership.
    template <typename Finder>
    [[nodiscard]] bool BuildGenerationMask(std::span<uint32_t> mask, Finder&& find) const {
        std::fill(mask.begin(), mask.end(), 0u);
        if (!valid_)
            return false;
        for (const auto& entry : entries_) {
            if (!entry.staticOpaqueCompatible)
                continue;
            const Handle<Instance> id = find(entry.source);
            if (!id || id.slot >= mask.size() || mask[id.slot] != 0) {
                // A failed build must not leak a partly admitted mask
                // even if a future caller mistakenly uploads its storage.
                std::fill(mask.begin(), mask.end(), 0u);
                return false;
            }
            mask[id.slot] = id.generation;
        }
        return true;
    }

  private:
    std::span<const RasterOwnershipEntry> entries_{};
    bool valid_ = false;
    bool gpuPrepared_ = false;
};

} // namespace tour::gpu_scene
