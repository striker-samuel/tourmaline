// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Scene residency provider: a CPU-only streaming front-end for the existing
// WorldPartition. It does NOT mutate World or assume a pak-per-tile layout.
// The host supplies asynchronous request/cancel callbacks and acknowledges
// safe activation/retirement at its own render-thread publication boundary.

#include <Engine/WorldPartition/WorldPartition.h>
#include <Engine/WorldPartition/StreamTicketLedger.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace engine::worldpartition {

struct SceneTileDescriptor {
    WorldTileKey tile{};
    uint64_t contentId = 0; // logical cooked tile ID, NOT a pak file offset
};

// Stable lexicographic ordering, including negative cell coordinates.
using SceneTileKeyLess = WorldTileKeyLess;

class SceneTileProvider final : public IWorldStreamProvider {
  public:
    using RequestCallback =
        std::function<void(const SceneTileDescriptor&, DesiredResidency, uint64_t ticket)>;
    using RetireCallback = std::function<bool(const SceneTileDescriptor&)>;

    const char* ProviderName() const override {
        return "SceneTiles";
    }

    void SetMaxNewRequestsPerFrame(size_t limit) {
        maxNewRequestsPerFrame_ = limit;
    }
    size_t MaxNewRequestsPerFrame() const {
        return maxNewRequestsPerFrame_;
    }

    // Manifest replaces previous availability, never runtime identity.
    // Do not swap a manifest while cells are resident: retire first.
    bool SetManifest(std::vector<SceneTileDescriptor> tiles) {
        const WorldId world = tiles.empty() ? 0 : tiles.front().tile.world;
        return SetManifest(world, std::move(tiles));
    }
    // Explicit identity supports Maps containing only persistent placements
    // or no cells yet. A nonzero world may have an empty tile manifest.
    bool SetManifest(WorldId world, std::vector<SceneTileDescriptor> tiles) {
        if (!resident_.empty() || !tickets_.Empty())
            return false;
        std::map<WorldTileKey, SceneTileDescriptor, SceneTileKeyLess> next;
        for (const auto& tile : tiles) {
            if (!world || tile.tile.world != world || !tile.contentId ||
                !next.emplace(tile.tile, tile).second)
                return false;
        }
        manifest_ = std::move(next);
        desired_.clear();
        activeWorld_ = world;
        return true;
    }

    // Only one Map owns this provider at a time; zero means no active Map.
    WorldId ActiveWorld() const {
        return activeWorld_;
    }

    void SetCallbacks(RequestCallback request, RetireCallback retire) {
        request_ = std::move(request);
        retire_ = std::move(retire);
    }

    // Successful asynchronous completion must be acknowledged at the host's
    // safe publication point; failed requests can be retried by ResetPending.
    bool Activate(WorldTileKey cell, uint64_t ticket) {
        // A late completion may not resurrect a tile no longer desired.
        if (!desired_.count(cell) || !tickets_.Accept(cell, ticket))
            return false;
        resident_.insert(cell);
        return true;
    }

    bool ResetPending(WorldTileKey cell, uint64_t ticket) {
        return tickets_.Fail(cell, ticket);
    }

    bool IsResident(WorldTileKey cell) const {
        return resident_.find(cell) != resident_.end();
    }
    size_t ResidentCount() const {
        return resident_.size();
    }
    size_t PendingCount() const {
        return tickets_.PendingCount();
    }

    void OnStreamingInterests(const StreamingInterest* interests, size_t count) override {
        desired_.clear();
        if (!interests)
            return;
        // Iterate the actual manifest rather than expanding unbounded interest
        // rectangles. Selection is independent from camera frustum visibility.
        for (size_t i = 0; i < count; ++i) {
            const auto& interest = interests[i];
            if (!activeWorld_ || interest.world != activeWorld_ ||
                !(interest.layerMask & StreamLayerBit(StreamLayer::Scene)) ||
                !std::isfinite(interest.radiusMeters) || interest.radiusMeters < 0.0 ||
                !std::isfinite(interest.priority) || !std::isfinite(interest.position.x) ||
                !std::isfinite(interest.position.z))
                continue;
            const double radius = interest.radiusMeters;
            for (const auto& [key, tile] : manifest_) {
                if (key.world != interest.world)
                    continue;
                // XZ distance from interest position to tile's closest point.
                const double x0 = double(key.cell.x) * kRootCellSize;
                const double z0 = double(key.cell.z) * kRootCellSize;
                const double dx = std::max(
                    {x0 - interest.position.x, 0.0, interest.position.x - (x0 + kRootCellSize)});
                const double dz = std::max(
                    {z0 - interest.position.z, 0.0, interest.position.z - (z0 + kRootCellSize)});
                if (std::hypot(dx, dz) > radius)
                    continue;
                auto [it, inserted] =
                    desired_.try_emplace(key, std::make_pair(interest.priority, interest.desired));
                if (!inserted) {
                    it->second.first = std::max(it->second.first, interest.priority);
                    if (uint8_t(interest.desired) > uint8_t(it->second.second))
                        it->second.second = interest.desired;
                }
            }
        }
    }

    size_t GatherRequests(StreamRequestSink* sink, uint64_t frameIndex) override {
        if (!sink)
            return 0;
        size_t count = 0;
        for (const auto& [key, policy] : desired_) {
            StreamRequest req{};
            req.layer = StreamLayer::Scene;
            req.world = key.world;
            req.cell = key.cell;
            req.priority = policy.first;
            req.desired = policy.second;
            req.frameRequested = frameIndex;
            sink->Push(req);
            ++count;
        }
        return count;
    }

    void CommitFrame(uint64_t) override {
        if (!request_ || maxNewRequestsPerFrame_ == 0)
            return;
        struct Candidate {
            WorldTileKey tile;
            float priority;
            DesiredResidency desired;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(desired_.size());
        for (const auto& [key, policy] : desired_) {
            if (resident_.count(key) || tickets_.IsPending(key) ||
                manifest_.find(key) == manifest_.end())
                continue;
            candidates.push_back({key, policy.first, policy.second});
        }
        std::stable_sort(
            candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                if (a.priority != b.priority)
                    return a.priority > b.priority;
                return SceneTileKeyLess{}(a.tile, b.tile);
            });
        size_t issued = 0;
        for (const auto& candidate : candidates) {
            if (issued >= maxNewRequestsPerFrame_)
                break;
            const auto it = manifest_.find(candidate.tile);
            if (it == manifest_.end())
                continue;
            const auto ticket = tickets_.Issue(candidate.tile);
            if (!ticket) break; // exhausted ticket IDs, fail closed
            ++issued;
            request_(it->second, candidate.desired, *ticket);
        }
    }

    void RetireFrame(uint64_t) override {
        // Retire callbacks must defer GPU/scene destruction until safe;
        // the host owns the actual retirement fence and asset release.
        for (auto it = resident_.begin(); it != resident_.end();) {
            if (desired_.count(*it)) {
                ++it;
                continue;
            }
            const auto desc = manifest_.find(*it);
            if (retire_ && desc != manifest_.end() && !retire_(desc->second)) {
                ++it; // failed retirement must remain resident for retry
                continue;
            }
            it = resident_.erase(it);
        }
        // A canceled IO request must not be treated as a newly resident tile.
        // Backend cancellation is the caller's responsibility; subsequent
        // asynchronous completion will be rejected by Activate().
        tickets_.CancelUndesired([&](const WorldTileKey& key) {
            return desired_.count(key) != 0;
        });
    }

  private:
    WorldId activeWorld_ = 0;
    std::map<WorldTileKey, SceneTileDescriptor, SceneTileKeyLess> manifest_;
    std::map<WorldTileKey, std::pair<float, DesiredResidency>, SceneTileKeyLess> desired_;
    StreamTicketLedger<WorldTileKey, SceneTileKeyLess> tickets_;
    size_t maxNewRequestsPerFrame_ = 8;
    std::set<WorldTileKey, SceneTileKeyLess> resident_;
    RequestCallback request_;
    RetireCallback retire_;
};

} // namespace engine::worldpartition
