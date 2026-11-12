// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Tile-owned cubemap capture demand. WorldPartition decides residency, not
// the current view frustum. The host owns KTX IO, GPU publication and fences.
// This provider only requests/retire groups with stable WorldTileKey identity.
// Capture payloads remain in their existing cubemap assets; no duplicate
// image bytes or pak-per-tile requirement is introduced here.

#include <Engine/WorldPartition/WorldPartition.h>
#include <Engine/WorldPartition/SceneTileProvider.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace engine::worldpartition {

struct CubemapCaptureTile {
    WorldTileKey tile{};
    // Host-defined stable capture-set identifier (not a GPU texture handle).
    uint64_t captureSetId = 0;
};

class CubemapTileProvider final : public IWorldStreamProvider {
  public:
    using RequestCallback = std::function<void(const CubemapCaptureTile&, uint64_t ticket)>;
    // Must return false until the GPU no longer references this tile.
    using RetireCallback = std::function<bool(const CubemapCaptureTile&)>;

    const char* ProviderName() const override { return "CubemapTiles"; }

    bool SetManifest(WorldId world, std::vector<CubemapCaptureTile> tiles) {
        if (!resident_.empty() || !pending_.empty()) return false;
        std::map<WorldTileKey, CubemapCaptureTile, WorldTileKeyLess> next;
        for (const auto& tile : tiles) {
            if (!world || tile.tile.world != world || !tile.tile.path.Valid() ||
                !tile.captureSetId || !next.emplace(tile.tile, tile).second)
                return false;
        }
        tiles_ = std::move(next);
        world_ = world;
        desired_.clear();
        return true;
    }

    void SetCallbacks(RequestCallback request, RetireCallback retire) {
        request_ = std::move(request);
        retire_ = std::move(retire);
    }
    void SetMaxNewRequestsPerFrame(size_t max) { maxNewRequests_ = max; }
    WorldId ActiveWorld() const { return world_; }
    size_t ResidentCount() const { return resident_.size(); }
    size_t PendingCount() const { return pending_.size(); }

    // Tickets make late IO completions after teleport/map switch harmless.
    bool Activate(WorldTileKey key, uint64_t ticket) {
        const auto it = pending_.find(key);
        if (it == pending_.end() || it->second != ticket || !desired_.count(key))
            return false;
        pending_.erase(it);
        resident_.insert(key);
        return true;
    }
    bool Fail(WorldTileKey key, uint64_t ticket) {
        const auto it = pending_.find(key);
        if (it == pending_.end() || it->second != ticket) return false;
        pending_.erase(it);
        return true;
    }

    void OnStreamingInterests(const StreamingInterest* interests, size_t count) override {
        desired_.clear();
        if (!interests) return;
        for (size_t i = 0; i < count; ++i) {
            const auto& interest = interests[i];
            if (interest.world != world_ ||
                !(interest.layerMask & StreamLayerBit(StreamLayer::Environment)) ||
                !std::isfinite(interest.radiusMeters) || interest.radiusMeters < 0 ||
                !std::isfinite(interest.position.x) || !std::isfinite(interest.position.z))
                continue;
            for (const auto& [key, tile] : tiles_) {
                // Tile is a root-cell-owned asset. Distance to cell AABB,
                // not camera direction; 3D captures can span floors.
                const double x0 = double(key.cell.x) * kRootCellSize;
                const double z0 = double(key.cell.z) * kRootCellSize;
                const double dx = std::max({x0 - interest.position.x, 0.0,
                                            interest.position.x - (x0 + kRootCellSize)});
                const double dz = std::max({z0 - interest.position.z, 0.0,
                                            interest.position.z - (z0 + kRootCellSize)});
                if (std::hypot(dx, dz) <= interest.radiusMeters)
                    desired_.insert(key);
            }
        }
    }

    size_t GatherRequests(StreamRequestSink* sink, uint64_t frame) override {
        if (!sink) return 0;
        for (const auto& key : desired_) {
            StreamRequest request{};
            request.world = key.world;
            request.cell = key.cell;
            request.layer = StreamLayer::Environment;
            request.desired = DesiredResidency::GpuReady;
            request.frameRequested = frame;
            sink->Push(request);
        }
        return desired_.size();
    }

    void CommitFrame(uint64_t) override {
        if (!request_) return;
        size_t issued = 0;
        for (const auto& key : desired_) {
            if (issued >= maxNewRequests_) break;
            if (resident_.count(key) || pending_.count(key)) continue;
            // Never recycle a ticket; if exhausted, fail closed.
            if (nextTicket_ == UINT64_MAX) break;
            const uint64_t ticket = ++nextTicket_;
            pending_.emplace(key, ticket);
            request_(tiles_.at(key), ticket);
            ++issued;
        }
    }

    void RetireFrame(uint64_t) override {
        for (auto it = resident_.begin(); it != resident_.end();) {
            if (desired_.count(*it) || (retire_ && !retire_(tiles_.at(*it)))) {
                ++it;
            } else {
                it = resident_.erase(it);
            }
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (!desired_.count(it->first)) it = pending_.erase(it);
            else ++it;
        }
    }

  private:
    WorldId world_ = 0;
    uint64_t nextTicket_ = 0;
    size_t maxNewRequests_ = 4;
    std::map<WorldTileKey, CubemapCaptureTile, WorldTileKeyLess> tiles_;
    std::set<WorldTileKey, WorldTileKeyLess> desired_;
    std::set<WorldTileKey, WorldTileKeyLess> resident_;
    std::map<WorldTileKey, uint64_t, WorldTileKeyLess> pending_;
    RequestCallback request_;
    RetireCallback retire_;
};

} // namespace engine::worldpartition
