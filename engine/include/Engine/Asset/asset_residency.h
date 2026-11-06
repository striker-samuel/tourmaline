// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Device/host-owned asset residency independent of a Scene, Cluster or pak.
// A lease pins one resource backing by durable content ID. The last holder
// releases it; stale weak cache entries do not keep physical assets alive.
// This is a CPU-side ownership primitive; the loader is responsible for
// safely deferring GPU destruction until relevant submission fences retire.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace tour::asset {

struct SharedAssetRecord {
    uint64_t contentId = 0;
    std::shared_ptr<void> backing;
};

using AssetLease = std::shared_ptr<const SharedAssetRecord>;

class AssetResidency {
public:
    using Loader = std::function<std::shared_ptr<void>(uint64_t)>;

    explicit AssetResidency(Loader loader) : loader_(std::move(loader)) {}

    // Fails closed (nullptr) if missing ID, loader, or backing. Existing
    // leases always refer to the SAME asset record for the same content ID.
    AssetLease Acquire(uint64_t id) {
        if (!id || !loader_) return {};
        auto it = entries_.find(id);
        if (it != entries_.end()) {
            if (auto existing = it->second.lock()) return existing;
        }
        std::shared_ptr<void> backing = loader_(id);
        if (!backing) return {};
        auto record = std::make_shared<SharedAssetRecord>(
            SharedAssetRecord{id, std::move(backing)});
        entries_[id] = record;
        return record;
    }

    // All-or-nothing ownership: if one dependency fails, acquired leases
    // are discarded, including those created in this call. No partial
    // instance may publish with only some of its material/mesh assets.
    bool AcquireAll(std::span<const uint64_t> ids,
                    std::vector<AssetLease>& out) {
        std::vector<AssetLease> next;
        next.reserve(ids.size());
        for (const uint64_t id : ids) {
            auto lease = Acquire(id);
            if (!lease) return false;
            next.push_back(std::move(lease));
        }
        out = std::move(next);
        return true;
    }

    [[nodiscard]] bool IsHeld(uint64_t id) const {
        auto it = entries_.find(id);
        return it != entries_.end() && !it->second.expired();
    }

    // Optional weak-key cleanup; no resource is released by Prune itself.
    void Prune() {
        for (auto it = entries_.begin(); it != entries_.end();)
            it = it->second.expired() ? entries_.erase(it) : ++it;
    }

private:
    Loader loader_;
    std::map<uint64_t, std::weak_ptr<const SharedAssetRecord>> entries_;
};

} // namespace tour::asset
