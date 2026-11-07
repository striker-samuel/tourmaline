// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Shared, CPU-only request ticket lifecycle for WorldPartition providers.
// Providers retain ownership of demand selection, content, and GPU retirement.
// Ticket numbers are deliberately NOT reset by Clear(): a late completion
// must never match a new request after a world or source revision swap.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace engine::worldpartition {

template<class Key, class Less = std::less<Key>>
class StreamTicketLedger {
public:
    // No duplicate dispatch, cooldown bypass, or ticket-number reuse.
    std::optional<uint64_t> Issue(const Key& key, uint64_t frame = 0) {
        if (!CanIssue(key, frame) || nextTicket_ == 0) return std::nullopt;
        const uint64_t ticket = nextTicket_++;
        pending_.emplace(key, ticket);
        return ticket;
    }

    bool Matches(const Key& key, uint64_t ticket) const {
        const auto it = pending_.find(key);
        return it != pending_.end() && it->second == ticket;
    }

    bool Accept(const Key& key, uint64_t ticket) {
        if (!Matches(key, ticket)) return false;
        pending_.erase(key);
        retryAfter_.erase(key);
        return true;
    }

    bool Fail(const Key& key, uint64_t ticket, uint64_t frame = 0,
              uint64_t retryDelayFrames = 0) {
        if (!Matches(key, ticket)) return false;
        pending_.erase(key);
        if (retryDelayFrames) {
            constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
            retryAfter_[key] = frame > max - retryDelayFrames ? max
                                                              : frame + retryDelayFrames;
        } else {
            retryAfter_.erase(key);
        }
        return true;
    }

    bool CanIssue(const Key& key, uint64_t frame = 0) const {
        if (pending_.count(key)) return false;
        const auto it = retryAfter_.find(key);
        return it == retryAfter_.end() || frame >= it->second;
    }
    bool IsPending(const Key& key) const { return pending_.count(key) != 0; }
    bool Empty() const { return pending_.empty(); }
    size_t PendingCount() const { return pending_.size(); }

    template<class Wanted>
    void CancelUndesired(Wanted wanted) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (wanted(it->first)) ++it;
            else it = pending_.erase(it);
        }
        for (auto it = retryAfter_.begin(); it != retryAfter_.end();) {
            if (wanted(it->first)) ++it;
            else it = retryAfter_.erase(it);
        }
    }

    void Clear() {
        pending_.clear();
        retryAfter_.clear();
    }

private:
    uint64_t nextTicket_ = 1;
    std::map<Key, uint64_t, Less> pending_;
    std::map<Key, uint64_t, Less> retryAfter_;
};

} // namespace engine::worldpartition
