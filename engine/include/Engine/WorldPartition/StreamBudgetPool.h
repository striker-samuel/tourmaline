// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Shared admission accounting for streaming IO and GPU memory. This is a
// resource reservation ledger, not a replacement for the Vulkan allocators.
// All mutations occur at the WorldPartition frame/publication boundary.
#include <Engine/WorldPartition/WorldPartition.h>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>

namespace engine::worldpartition {

struct StreamBudgetLimits {
    uint64_t maxInFlightIoBytes = UINT64_MAX;
    uint64_t maxResidentGpuBytes = UINT64_MAX;
    uint64_t maxInFlightRequests = UINT64_MAX;
};

struct StreamBudgetKey {
    WorldId world = 0;
    StreamLayer layer = StreamLayer::WorldField;
    WorldCellKey cell{};
    WorldSubcellKey subcell{};
    bool operator<(const StreamBudgetKey& b) const {
        return std::tie(world, layer, cell.x, cell.z, subcell.x, subcell.z) <
               std::tie(b.world, b.layer, b.cell.x, b.cell.z, b.subcell.x, b.subcell.z);
    }
};

class StreamBudgetPool {
public:
    // Do not discard live resource accounting when limits are changed.
    void SetLimits(StreamBudgetLimits limits) { limits_ = limits; }
    StreamBudgetLimits Limits() const { return limits_; }

    bool Reserve(StreamBudgetKey key, uint64_t ioBytes, uint64_t gpuBytes) {
        if (entries_.count(key)) return false;
        // Never pretend that a byte ceiling is enforceable with unknown costs.
        if ((limits_.maxInFlightIoBytes != UINT64_MAX && !ioBytes) ||
            (limits_.maxResidentGpuBytes != UINT64_MAX && !gpuBytes))
            return false;
        if (!Fits(inFlightIoBytes_, ioBytes, limits_.maxInFlightIoBytes) ||
            !Fits(residentGpuBytes_, gpuBytes, limits_.maxResidentGpuBytes) ||
            !Fits(inFlightRequests_, 1, limits_.maxInFlightRequests))
            return false;
        entries_.emplace(key, Entry{ioBytes, gpuBytes, true});
        inFlightIoBytes_ += ioBytes;
        residentGpuBytes_ += gpuBytes; // reserve prospective VRAM before IO
        ++inFlightRequests_;
        return true;
    }

    // A completed IO no longer consumes an in-flight IO slot, but its
    // prospective GPU allocation remains reserved until retirement.
    bool CompleteIo(StreamBudgetKey key) {
        auto it = entries_.find(key);
        if (it == entries_.end() || !it->second.inFlight) return false;
        it->second.inFlight = false;
        inFlightIoBytes_ -= it->second.ioBytes;
        --inFlightRequests_;
        return true;
    }

    bool Release(StreamBudgetKey key) {
        const auto it = entries_.find(key);
        if (it == entries_.end()) return false;
        if (it->second.inFlight) {
            inFlightIoBytes_ -= it->second.ioBytes;
            --inFlightRequests_;
        }
        residentGpuBytes_ -= it->second.gpuBytes;
        entries_.erase(it);
        return true;
    }

    bool Contains(StreamBudgetKey key) const { return entries_.count(key) != 0; }
    uint64_t InFlightIoBytes() const { return inFlightIoBytes_; }
    uint64_t ReservedGpuBytes() const { return residentGpuBytes_; }
    uint64_t InFlightRequests() const { return inFlightRequests_; }
    size_t ReservationCount() const { return entries_.size(); }

private:
    static bool Fits(uint64_t used, uint64_t amount, uint64_t maximum) {
        return amount <= maximum && used <= maximum - amount;
    }
    struct Entry {
        uint64_t ioBytes;
        uint64_t gpuBytes;
        bool inFlight;
    };
    StreamBudgetLimits limits_{};
    std::map<StreamBudgetKey, Entry> entries_;
    uint64_t inFlightIoBytes_ = 0;
    uint64_t residentGpuBytes_ = 0;
    uint64_t inFlightRequests_ = 0;
};

} // namespace engine::worldpartition
