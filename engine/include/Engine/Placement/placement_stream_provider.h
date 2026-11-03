// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU placement's WorldPartition adapter. WorldPartition owns interests and
// calls CommitFrame at its common publication point; the placement runtime
// still owns instance buffers, tile activation tokens, and GPU slot retirement.
//
// The callback is intentionally renderer-owned: this CPU-only header does not
// pull Vulkan/RenderGraph types into the partition or tests. The renderer
// binds its PlacementWorldRuntime::BeginFrame as the commit callback.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/WorldPartition/WorldPartition.h>

namespace tour::placement {

class PlacementStreamProvider final : public engine::worldpartition::IWorldStreamProvider {
public:
    using CommitCallback = std::function<bool(const glm::vec4*, size_t, uint64_t)>;

    void SetCommitCallback(CommitCallback callback) {
        commit_ = std::move(callback);
    }

    // A disabled/missing placement layer must never publish stale requests.
    // Bounds are the authoritative world heightfield's XZ rectangle.
    void SetFrameSource(bool enabled, const glm::vec4& worldBounds) {
        enabled_ = enabled && worldBounds.z > worldBounds.x
            && worldBounds.w > worldBounds.y;
        bounds_ = worldBounds;
    }

    const char* ProviderName() const override { return "Placement"; }

    void OnStreamingInterests(
        const engine::worldpartition::StreamingInterest* interests,
        size_t count) override {
        rects_.clear();
        metadata_.clear();
        committed_ = false;
        if (!enabled_) return;
        for (size_t i = 0; i < count; ++i) {
            const auto& in = interests[i];
            if (!(in.layerMask & engine::worldpartition::StreamLayerBit(
                    engine::worldpartition::StreamLayer::Placement)))
                continue;
            if (!(in.radiusMeters > 0.0) || !std::isfinite(in.radiusMeters)
                || !std::isfinite(in.position.x)
                || !std::isfinite(in.position.z))
                continue;
            const double r = in.radiusMeters;
            // Clip in double precision before converting to the placement
            // runtime's float rectangles. WorldPartition retains the
            // authoritative double-precision interest coordinates.
            const glm::vec4 rect(
                float(std::max(double(bounds_.x), in.position.x - r)),
                float(std::max(double(bounds_.y), in.position.z - r)),
                float(std::min(double(bounds_.z), in.position.x + r)),
                float(std::min(double(bounds_.w), in.position.z + r)));
            if (!(rect.z > rect.x && rect.w > rect.y)) continue;
            rects_.push_back(rect); // order is partition priority order
            metadata_.push_back({ in.priority, in.desired });
        }
    }

    size_t GatherRequests(engine::worldpartition::StreamRequestSink* sink,
        uint64_t frameIndex) override {
        if (!sink) return 0;
        size_t pushed = 0;
        for (size_t i = 0; i < rects_.size(); ++i) {
            const glm::vec4 rect = rects_[i];
            // Half-open upper bound: [0, 256) requests root cell 0 only.
            const double s = engine::worldpartition::kRootCellSize;
            const int64_t x0 = int64_t(std::floor(double(rect.x) / s));
            const int64_t z0 = int64_t(std::floor(double(rect.y) / s));
            const int64_t x1 = int64_t(std::ceil(double(rect.z) / s)) - 1;
            const int64_t z1 = int64_t(std::ceil(double(rect.w) / s)) - 1;
            for (int64_t z = z0; z <= z1; ++z)
                for (int64_t x = x0; x <= x1; ++x) {
                    engine::worldpartition::StreamRequest req{};
                    req.layer = engine::worldpartition::StreamLayer::Placement;
                    req.cell = { x, z };
                    req.priority = metadata_[i].priority;
                    req.desired = metadata_[i].desired;
                    req.frameRequested = frameIndex;
                    sink->Push(req);
                    ++pushed;
                }
        }
        return pushed;
    }

    void CommitFrame(uint64_t frameIndex) override {
        committed_ = false;
        if (enabled_ && commit_ && !rects_.empty())
            committed_ = commit_(rects_.data(), rects_.size(), frameIndex);
    }

    // The renderer may issue per-view cull and indirect passes only after
    // this provider recorded placement generation at the partition commit.
    bool CommittedThisFrame() const { return committed_; }
    size_t InterestRectCount() const { return rects_.size(); }

private:
    struct RequestMetadata {
        float priority = 0.0f;
        engine::worldpartition::DesiredResidency desired =
            engine::worldpartition::DesiredResidency::GpuReady;
    };
    bool enabled_ = false;
    bool committed_ = false;
    glm::vec4 bounds_{ 0.0f };
    std::vector<glm::vec4> rects_;
    std::vector<RequestMetadata> metadata_;
    CommitCallback commit_;
};

} // namespace tour::placement
