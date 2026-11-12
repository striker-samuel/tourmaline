// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Device-free GPU Scene producer boundary for GPU-resident procedural batches.
// Does NOT own GPU allocations or submit draws. Snapshot() copies metadata;
// callers must pin the referenced resources until all GPU flight users retire.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace tour::gpu_scene {

struct ProducerHandle {
    uint64_t value = 0;
    explicit operator bool() const { return value != 0; }
};
struct BatchKey {
    uint64_t producer = 0;
    uint64_t local = 0; // stable within this producer (e.g. encoded tile key)
    friend bool operator<(BatchKey a, BatchKey b) {
        return a.producer < b.producer || (a.producer == b.producer && a.local < b.local);
    }
    friend bool operator==(BatchKey a, BatchKey b) {
        return a.producer == b.producer && a.local == b.local;
    }
};
enum class BatchInstanceFormat : uint32_t {
    Placement40 = 1, // Engine/Placement/placement_point.h, RenderInstance
    GpuScene128 = 2, // Engine/Renderer/gpu_scene.h, Row<Instance>
};
struct BatchBounds {
    double min[3] = {};
    double max[3] = {};
    bool Valid() const {
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(min[axis]) || !std::isfinite(max[axis]) ||
                min[axis] > max[axis])
                return false;
        return true;
    }
};
struct BatchPublication {
    BatchKey key{};
    uint64_t revision = 0; // strictly increasing for a given key, even across retire
    uint64_t instancesBda = 0; // GPU address, never a CPU pointer
    uint64_t bufferBytes = 0; // accessible bytes starting at instancesBda
    uint64_t prototypeTable = 0; // opaque resolver-owned resource identity
    uint32_t count = 0;
    uint32_t capacity = 0;
    uint32_t stride = 0;
    BatchInstanceFormat format = BatchInstanceFormat::Placement40;
    BatchBounds bounds{};
};
struct PublishedBatch {
    BatchPublication data{};
    uint64_t incarnation = 0; // changes on every successful publication
};

// Single-writer, device-free publication registry. It is not a second
// per-instance database: it tracks batches, not generated instance IDs.
class ProceduralBatchRegistry {
  public:
    ProducerHandle RegisterProducer() {
        if (nextProducer_ == 0) return {}; // prevent wraparound alias
        const uint64_t id = nextProducer_++;
        producers_.emplace(id, true);
        return {id};
    }
    // Retires all batches atomically from future snapshots. Does NOT free
    // buffers; renderer/producer must defer resource release past GPU fences.
    bool RetireProducer(ProducerHandle producer) {
        if (!producer || producers_.erase(producer.value) == 0) return false;
        for (auto it = live_.begin(); it != live_.end();) {
            if (it->first.producer == producer.value)
                it = live_.erase(it);
            else ++it;
        }
        return true;
    }
    bool Publish(const BatchPublication& batch) {
        if (!Valid(batch) || !producers_.contains(batch.key.producer))
            return false;
        auto last = revisions_.find(batch.key);
        if (last != revisions_.end() && batch.revision <= last->second)
            return false; // stale async activation or duplicate publication
        if (nextIncarnation_ == 0) return false;
        const uint64_t incarnation = nextIncarnation_++;
        // No GPU resource is released or modified by metadata publication.
        revisions_[batch.key] = batch.revision;
        live_[batch.key] = {batch, incarnation};
        return true;
    }
    // A retire is versioned too: an old completion cannot undo a newer tile.
    bool Retire(BatchKey key, uint64_t revision) {
        if (!key.producer || !key.local || !producers_.contains(key.producer) ||
            revision == 0)
            return false;
        auto last = revisions_.find(key);
        if (last != revisions_.end() && revision <= last->second)
            return false;
        revisions_[key] = revision;
        live_.erase(key);
        return true;
    }
    const PublishedBatch* Find(BatchKey key) const {
        auto it = live_.find(key);
        return it == live_.end() ? nullptr : &it->second;
    }
    // Freeze metadata for one acquired flight. Snapshot is NOT GPU admission:
    // the rendering adapter must resolve prototypes, validate residency and
    // bind resource holders before committing exactly-once raster ownership.
    std::vector<PublishedBatch> Snapshot() const {
        std::vector<PublishedBatch> result;
        result.reserve(live_.size());
        for (const auto& [key, value] : live_) result.push_back(value);
        return result;
    }
    size_t BatchCount() const { return live_.size(); }

    static bool Valid(const BatchPublication& b) {
        if (!b.key.producer || !b.key.local || !b.revision || !b.bounds.Valid())
            return false;
        const uint32_t expected = b.format == BatchInstanceFormat::Placement40 ? 40u :
                                  b.format == BatchInstanceFormat::GpuScene128 ? 128u : 0u;
        if (!expected || b.stride != expected || b.count > b.capacity)
            return false;
        if (b.count == 0) return true; // an empty published tile is valid
        if (!b.instancesBda || !b.prototypeTable ||
            b.count > std::numeric_limits<uint64_t>::max() / b.stride)
            return false;
        return uint64_t(b.count) * b.stride <= b.bufferBytes;
    }
  private:
    uint64_t nextProducer_ = 1;
    uint64_t nextIncarnation_ = 1;
    std::map<uint64_t, bool> producers_;
    std::map<BatchKey, uint64_t> revisions_; // tombstones prevent stale republish
    std::map<BatchKey, PublishedBatch> live_;
};
} // namespace tour::gpu_scene
