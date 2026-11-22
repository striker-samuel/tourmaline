// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace smvk {

enum class UploadStatus : uint8_t {
    Invalid, Rejected, Recorded, Submitted, Completed, Failed
};

struct UploadTicket {
    uint32_t shard = 0;
    uint64_t sequence = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return sequence != 0; }
};

struct UploadRequest {
    UploadStatus status = UploadStatus::Rejected;
    UploadTicket ticket{};
    [[nodiscard]] bool accepted() const noexcept {
        return status == UploadStatus::Recorded ||
               status == UploadStatus::Submitted;
    }
};

// A shard owns its writer. Pollers can read these atomics without holding
// a shard lock. Published sequence is written AFTER the timeline value.
// The newest successful batch covers all earlier sequences on the same
// ordered transfer timeline; an earlier ticket may conservatively remain
// Submitted until the newest batch finishes.
struct UploadSequence {
    std::atomic<uint64_t> issued{0};
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> submittedTimeline{0};
    std::atomic<uint64_t> failedFrom{std::numeric_limits<uint64_t>::max()};

    [[nodiscard]] uint64_t Issue() noexcept {
        return issued.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    void Submitted(uint64_t through, uint64_t timeline) noexcept {
        submittedTimeline.store(timeline, std::memory_order_relaxed);
        submitted.store(through, std::memory_order_release);
    }
    void Failed(uint64_t startingAt) noexcept {
        uint64_t old = failedFrom.load(std::memory_order_relaxed);
        while (startingAt < old &&
               !failedFrom.compare_exchange_weak(old, startingAt,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {}
    }
    [[nodiscard]] UploadStatus Poll(uint64_t seq, uint64_t completedTimeline) const noexcept {
        if (seq == 0 || seq > issued.load(std::memory_order_acquire))
            return UploadStatus::Invalid;
        if (seq >= failedFrom.load(std::memory_order_acquire))
            return UploadStatus::Failed;
        if (seq > submitted.load(std::memory_order_acquire))
            return UploadStatus::Recorded;
        return completedTimeline >= submittedTimeline.load(std::memory_order_relaxed)
                   ? UploadStatus::Completed : UploadStatus::Submitted;
    }
};
} // namespace smvk
