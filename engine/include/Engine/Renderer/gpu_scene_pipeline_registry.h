// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Dynamic pipeline-key extension to pak_linear_pso_bins.h.
// CPU pak registration owns worst-case capacities; the GPU owns visibility
// counts. Neither a mesh nor a material directly owns a Vulkan PSO.
// IDs are snapshot-local and must be remapped on registry rebuild.
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace tour::gpu_scene {

enum class PipelineCoverage : uint8_t { Opaque, Masked };
enum class PipelineCull : uint8_t { Back, None, Front };

// renderStateId identifies the complete remaining compatibility state:
// pass, attachments/formats, depth/stencil, blend, MSAA, shader layout,
// topology and specialization. Do not conflate different render passes.
struct PipelineKey {
    uint64_t vertexShader = 0;
    uint64_t fragmentShader = 0;
    uint64_t renderStateId = 0;
    PipelineCoverage coverage = PipelineCoverage::Opaque;
    PipelineCull cull = PipelineCull::Back;

    bool operator==(const PipelineKey&) const = default;
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const noexcept {
        uint64_t h = 1469598103934665603ull;
        const uint64_t fields[] = {k.vertexShader, k.fragmentShader,
                                   k.renderStateId, uint64_t(k.coverage),
                                   uint64_t(k.cull)};
        for (const uint64_t value : fields) {
            h ^= value;
            h *= 1099511628211ull;
        }
        return static_cast<size_t>(h);
    }
};

class PipelineRegistry {
public:
    // Register at pak enumeration / shader revision time, not per GPU draw.
    // The calling renderer must retain actual pipelines across flight fences.
    [[nodiscard]] std::optional<uint32_t> Intern(const PipelineKey& key) {
        if (!key.vertexShader || !key.fragmentShader || !key.renderStateId ||
            uint32_t(key.coverage) > uint32_t(PipelineCoverage::Masked) ||
            uint32_t(key.cull) > uint32_t(PipelineCull::Front))
            return std::nullopt;
        if (const auto it = lookup_.find(key); it != lookup_.end())
            return it->second;
        if (keys_.size() >= std::numeric_limits<uint32_t>::max())
            return std::nullopt;
        const uint32_t id = static_cast<uint32_t>(keys_.size());
        keys_.push_back(key);
        try {
            lookup_.emplace(key, id);
        } catch (...) {
            keys_.pop_back();
            throw;
        }
        return id;
    }

    [[nodiscard]] const PipelineKey* Lookup(uint32_t id) const noexcept {
        return id < keys_.size() ? &keys_[id] : nullptr;
    }
    [[nodiscard]] uint32_t Size() const noexcept {
        return static_cast<uint32_t>(keys_.size());
    }
    void Clear() {
        lookup_.clear();
        keys_.clear();
    }
private:
    std::vector<PipelineKey> keys_;
    std::unordered_map<PipelineKey, uint32_t, PipelineKeyHash> lookup_;
};

struct RegisteredPipelineDraw {
    uint32_t pipelineId = UINT32_MAX;
    uint32_t sourceIndex = 0; // stable pak/cooked candidate identity
};

struct PipelineBinRange {
    uint32_t offset = 0;   // reserved command/index-array offset
    uint32_t capacity = 0; // CPU-proven maximum for this pipeline
    uint32_t count = 0;    // exact active count, never above capacity
};

struct PipelineCapacityPlan {
    std::vector<PipelineBinRange> bins;
    uint32_t capacity = 0;
};

struct PipelineFlightLayout {
    std::vector<PipelineBinRange> bins;
    uint32_t drawCount = 0;
    uint32_t drawCapacity = 0;
    uint32_t generation = 0;
};

// Pak/cooked enumeration includes each potentially drawable LOD and surface;
// a shared mesh asset is not one draw. Transactional: out unchanged on error.
[[nodiscard]] inline bool PrecountPipelineBins(
    std::span<const RegisteredPipelineDraw> registered,
    const PipelineRegistry& registry, PipelineCapacityPlan& out) {
    if (registered.size() > UINT32_MAX)
        return false;
    PipelineCapacityPlan next;
    next.bins.resize(registry.Size());
    for (const auto& draw : registered) {
        if (!registry.Lookup(draw.pipelineId))
            return false;
        ++next.bins[draw.pipelineId].capacity; // <= registered.size() <= UINT32_MAX
    }
    uint64_t offset = 0;
    for (auto& bin : next.bins) {
        bin.offset = static_cast<uint32_t>(offset);
        offset += bin.capacity;
        if (offset > UINT32_MAX)
            return false;
    }
    next.capacity = static_cast<uint32_t>(offset);
    out = std::move(next);
    return true;
}

// CPU-side reference packer, used for publication validation and future GPU
// scatter ABI. Writes only after preflight passes; no silent class overflow.
// Destination spans the full RESERVED capacity, not merely active count.
// Stable ordering is preserved within each class; unused tails aren't drawn.
[[nodiscard]] inline bool PackPipelineFlight(
    std::span<const RegisteredPipelineDraw> active,
    const PipelineRegistry& registry, const PipelineCapacityPlan& capacity,
    std::span<uint32_t> outputSourceIndices, PipelineFlightLayout& published,
    uint32_t generation) {
    if (capacity.bins.size() != registry.Size() ||
        outputSourceIndices.size() < capacity.capacity ||
        active.size() > capacity.capacity)
        return false;
    uint64_t end = 0;
    for (const auto& bin : capacity.bins) {
        if (bin.offset != end || bin.count != 0)
            return false;
        end += bin.capacity;
        if (end > UINT32_MAX)
            return false;
    }
    if (end != capacity.capacity)
        return false;

    std::vector<uint32_t> counts(capacity.bins.size(), 0);
    for (const auto& draw : active) {
        if (!registry.Lookup(draw.pipelineId) ||
            counts[draw.pipelineId] >= capacity.bins[draw.pipelineId].capacity)
            return false;
        ++counts[draw.pipelineId];
    }

    PipelineFlightLayout next;
    next.bins = capacity.bins;
    next.drawCapacity = capacity.capacity;
    next.drawCount = static_cast<uint32_t>(active.size());
    next.generation = generation;
    for (size_t i = 0; i < counts.size(); ++i)
        next.bins[i].count = counts[i];

    counts.assign(counts.size(), 0);
    for (const auto& draw : active) {
        const auto id = draw.pipelineId;
        outputSourceIndices[capacity.bins[id].offset + counts[id]++] =
            draw.sourceIndex;
    }
    published = std::move(next);
    return true;
}

[[nodiscard]] inline bool ValidPipelineFlight(const PipelineFlightLayout& layout) {
    if (layout.drawCount > layout.drawCapacity)
        return false;
    uint64_t end = 0, active = 0;
    for (const auto& bin : layout.bins) {
        if (bin.offset != end || bin.count > bin.capacity)
            return false;
        end += bin.capacity;
        active += bin.count;
        if (end > UINT32_MAX)
            return false;
    }
    return end == layout.drawCapacity && active == layout.drawCount;
}
} // namespace tour::gpu_scene
