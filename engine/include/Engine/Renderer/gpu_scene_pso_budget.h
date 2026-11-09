// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Conservative raster command capacities, maintained from published scene
// sources rather than GPU readback. A source is one renderable instance (not
// one unique mesh asset). Each class bound must cover every LOD that the GPU
// may choose for that source, including all submeshes of the chosen LOD.
//
// This is a CPU-only accounting primitive. Mutation/publication ordering is
// owned by the scene registry; do not publish a budget from a newer scene
// revision alongside an older GPU-scene work snapshot.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>

namespace tour::gpu_scene {

template <typename SourceId = uint64_t, typename SourceHash = std::hash<SourceId>>
class RasterPsoBudgetT {
public:
    static constexpr uint32_t ClassCount = 4;
    using Counts = std::array<uint32_t, ClassCount>;

    // The caller supplies a stable identity (including source generation).
    // Re-publishing an existing source replaces its contribution, making
    // streamed tile updates / material changes idempotent.
    [[nodiscard]] bool Upsert(const SourceId& source, Counts worstCasePerLod) {
        const auto it = sources_.find(source);
        const Counts previous = it == sources_.end() ? Counts{} : it->second;
        Counts next{};
        for (uint32_t i = 0; i < ClassCount; ++i) {
            const uint64_t withoutOld = totals_[i] - previous[i];
            if (withoutOld > uint64_t(std::numeric_limits<uint32_t>::max()) -
                                 worstCasePerLod[i])
                return false; // Reject atomically; don't publish an unsafe bound.
            next[i] = static_cast<uint32_t>(withoutOld + worstCasePerLod[i]);
        }
        if (it == sources_.end())
            sources_.emplace(source, worstCasePerLod);
        else
            it->second = worstCasePerLod;
        totals_ = next;
        return true;
    }

    [[nodiscard]] bool Erase(const SourceId& source) {
        const auto it = sources_.find(source);
        if (it == sources_.end())
            return false;
        for (uint32_t i = 0; i < ClassCount; ++i)
            totals_[i] -= it->second[i];
        sources_.erase(it);
        return true;
    }

    void Clear() {
        sources_.clear();
        totals_ = {};
    }

    [[nodiscard]] Counts UpperBounds() const noexcept { return totals_; }
    [[nodiscard]] size_t SourceCount() const noexcept { return sources_.size(); }

    // Maximize each class independently across mutually exclusive LODs,
    // then add across instances. This overestimates safely when LOD material
    // distribution changes, unlike counting only the current CPU LOD.
    template <typename Range>
    [[nodiscard]] static Counts WorstLod(const Range& lodClassCounts) {
        Counts out{};
        for (const Counts& lod : lodClassCounts)
            for (uint32_t i = 0; i < ClassCount; ++i)
                if (lod[i] > out[i])
                    out[i] = lod[i];
        return out;
    }

private:
    std::unordered_map<SourceId, Counts, SourceHash> sources_;
    Counts totals_{};
};

using RasterPsoBudget = RasterPsoBudgetT<>;

// Snapshot-friendly, tightly packed regions. CPU upper bounds determine
// capacity, while GPU counts determine how many entries actually get drawn.
// Return false if the declared work ceiling cannot hold all source bounds;
// callers must retain legacy rendering rather than silently truncate.
struct RasterPsoRegions {
    RasterPsoBudget::Counts offsets{};
    RasterPsoBudget::Counts capacities{};
    uint32_t totalCapacity = 0;

    [[nodiscard]] static bool Build(RasterPsoBudget::Counts bounds,
                                    uint32_t workCeiling,
                                    RasterPsoRegions& out) noexcept {
        RasterPsoRegions next{};
        uint64_t total = 0;
        for (uint32_t i = 0; i < RasterPsoBudget::ClassCount; ++i) {
            if (total > workCeiling || bounds[i] > workCeiling - total)
                return false;
            next.offsets[i] = static_cast<uint32_t>(total);
            next.capacities[i] = bounds[i];
            total += bounds[i];
        }
        next.totalCapacity = static_cast<uint32_t>(total);
        out = next;
        return true;
    }
};

} // namespace tour::gpu_scene
