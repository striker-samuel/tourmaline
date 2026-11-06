// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Scene residency provider: a CPU-only streaming front-end for the existing
// WorldPartition. It does NOT mutate World or assume a pak-per-tile layout.
// The host supplies asynchronous request/cancel callbacks and acknowledges
// safe activation/retirement at its own render-thread publication boundary.

#include <Engine/WorldPartition/WorldPartition.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace engine::worldpartition {

struct SceneTileDescriptor {
    WorldCellKey cell{};
    uint64_t contentId = 0; // logical cooked tile ID, NOT a pak file offset
};

// Stable lexicographic ordering, including negative cell coordinates.
struct SceneTileKeyLess {
    bool operator()(WorldCellKey a, WorldCellKey b) const {
        return a.x < b.x || (a.x == b.x && a.z < b.z);
    }
};

class SceneTileProvider final : public IWorldStreamProvider {
public:
    using RequestCallback = std::function<void(const SceneTileDescriptor&, DesiredResidency, uint64_t ticket)>;
    using RetireCallback = std::function<void(const SceneTileDescriptor&)>;

    const char* ProviderName() const override { return "SceneTiles"; }

    // Manifest replaces previous availability, never runtime identity.
    // Do not swap a manifest while cells are resident: retire first.
    bool SetManifest(std::vector<SceneTileDescriptor> tiles) {
        if (!resident_.empty() || !pending_.empty()) return false;
        std::map<WorldCellKey, SceneTileDescriptor, SceneTileKeyLess> next;
        for (const auto& tile : tiles) {
            if (!tile.contentId || !next.emplace(tile.cell, tile).second)
                return false;
        }
        manifest_ = std::move(next);
        return true;
    }

    void SetCallbacks(RequestCallback request, RetireCallback retire) {
        request_ = std::move(request);
        retire_ = std::move(retire);
    }

    // Successful asynchronous completion must be acknowledged at the host's
    // safe publication point; failed requests can be retried by ResetPending.
    bool Activate(WorldCellKey cell, uint64_t ticket) {
        const auto it = pending_.find(cell);
        // Stale completion from canceled/evicted load cannot adopt a fresh
        // request for the same tile. Reject demand removed this frame too.
        if (it == pending_.end() || it->second != ticket
            || !desired_.count(cell)) return false;
        pending_.erase(it);
        resident_.insert(cell);
        return true;
    }

    bool ResetPending(WorldCellKey cell, uint64_t ticket) {
        const auto it = pending_.find(cell);
        if (it == pending_.end() || it->second != ticket) return false;
        pending_.erase(it);
        return true;
    }

    bool IsResident(WorldCellKey cell) const {
        return resident_.find(cell) != resident_.end();
    }
    size_t ResidentCount() const { return resident_.size(); }
    size_t PendingCount() const { return pending_.size(); }

    void OnStreamingInterests(const StreamingInterest* interests, size_t count) override {
        desired_.clear();
        if (!interests) return;
        // Iterate the actual manifest rather than expanding unbounded interest
        // rectangles. Selection is independent from camera frustum visibility.
        for (size_t i = 0; i < count; ++i) {
            const auto& interest = interests[i];
            if (!(interest.layerMask & StreamLayerBit(StreamLayer::Scene))
                || !std::isfinite(interest.radiusMeters)
                || interest.radiusMeters < 0.0
                || !std::isfinite(interest.position.x)
                || !std::isfinite(interest.position.z))
                continue;
            const double radius = interest.radiusMeters;
            for (const auto& [key, tile] : manifest_) {
                // XZ distance from interest position to tile's closest point.
                const double x0 = double(key.x) * kRootCellSize;
                const double z0 = double(key.z) * kRootCellSize;
                const double dx = std::max({x0 - interest.position.x,
                    0.0, interest.position.x - (x0 + kRootCellSize)});
                const double dz = std::max({z0 - interest.position.z,
                    0.0, interest.position.z - (z0 + kRootCellSize)});
                if (std::hypot(dx, dz) > radius) continue;
                auto [it, inserted] = desired_.try_emplace(key,
                    std::make_pair(interest.priority, interest.desired));
                if (!inserted) {
                    it->second.first = std::max(it->second.first, interest.priority);
                    if (uint8_t(interest.desired) > uint8_t(it->second.second))
                        it->second.second = interest.desired;
                }
            }
        }
    }

    size_t GatherRequests(StreamRequestSink* sink, uint64_t frameIndex) override {
        if (!sink) return 0;
        size_t count = 0;
        for (const auto& [key, policy] : desired_) {
            StreamRequest req{};
            req.layer = StreamLayer::Scene;
            req.cell = key;
            req.priority = policy.first;
            req.desired = policy.second;
            req.frameRequested = frameIndex;
            sink->Push(req);
            ++count;
        }
        return count;
    }

    void CommitFrame(uint64_t) override {
        if (!request_) return;
        for (const auto& [key, policy] : desired_) {
            if (resident_.count(key) || pending_.count(key)) continue;
            const auto it = manifest_.find(key);
            if (it == manifest_.end()) continue;
            const uint64_t ticket = nextTicket_++;
            if (ticket == 0) continue; // exhausted monotonic identity
            pending_.emplace(key, ticket);
            request_(it->second, policy.second, ticket);
        }
    }

    void RetireFrame(uint64_t) override {
        // Retire callbacks must defer GPU/scene destruction until safe;
        // the host owns the actual retirement fence and asset release.
        for (auto it = resident_.begin(); it != resident_.end();) {
            if (desired_.count(*it)) { ++it; continue; }
            auto old = it++;
            const auto desc = manifest_.find(*old);
            if (retire_ && desc != manifest_.end()) retire_(desc->second);
            resident_.erase(old);
        }
        // A canceled IO request must not be treated as a newly resident tile.
        // Backend cancellation is the caller's responsibility; subsequent
        // asynchronous completion will be rejected by Activate().
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (desired_.count(it->first)) { ++it; continue; }
            it = pending_.erase(it);
        }
    }

private:
    std::map<WorldCellKey, SceneTileDescriptor, SceneTileKeyLess> manifest_;
    std::map<WorldCellKey, std::pair<float, DesiredResidency>, SceneTileKeyLess> desired_;
    std::map<WorldCellKey, uint64_t, SceneTileKeyLess> pending_;
    uint64_t nextTicket_ = 1;
    std::set<WorldCellKey, SceneTileKeyLess> resident_;
    RequestCallback request_;
    RetireCallback retire_;
};

} // namespace engine::worldpartition
