// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace tour {

// Optional CPU-side frame cap, separate from Vulkan present modes.
// Call BeginFrame before the CPU frame and EndFrame after presentation.
// Deadline cadence is absolute (not sleep-for-after-work), and skipped
// deadlines are discarded after a hitch rather than run as catch-up frames.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    void SetTargetFps(uint32_t fps) {
        if (fps != targetFps_) {
            targetFps_ = fps;
            Reset();
        }
    }
    uint32_t GetTargetFps() const { return targetFps_; }
    void Reset() { armed_ = false; }

    // Leave room below the VRR ceiling so FIFO VSync does not start
    // queueing frames at the fixed-refresh boundary. Unknown refresh
    // rates deliberately yield no automatic cap.
    static uint32_t VrrTargetFps(double refreshHz) {
        if (!(refreshHz >= 30.0 && refreshHz <= 1000.0))
            return 0;
        const auto rounded = static_cast<uint32_t>(refreshHz + 0.5);
        return rounded > 2 ? rounded - 2 : 0;
    }

    static Duration PeriodForFps(uint32_t fps) {
        if (fps == 0)
            return Duration::zero();
        return std::max(Duration{1}, std::chrono::duration_cast<Duration>(
            std::chrono::duration<double>(1.0 / double(fps))));
    }

    // Return the first deadline strictly after 'now', or the outstanding
    // deadline when it has not been reached yet. Skip missed intervals.
    static TimePoint NextDeadline(TimePoint deadline, TimePoint now, Duration period) {
        if (period <= Duration::zero())
            return now;
        if (now < deadline)
            return deadline;
        return deadline + period * ((now - deadline) / period + 1);
    }

    void BeginFrame() {
        if (targetFps_ != 0 && !armed_) {
            nextDeadline_ = Clock::now() + PeriodForFps(targetFps_);
            armed_ = true;
        }
    }

    void EndFrame() {
        if (!armed_)
            return;
        // Sleep for the bulk of the interval and spin only over the final
        // ~200 us to avoid a full scheduler tick of late wake-up jitter.
        constexpr auto spinWindow = std::chrono::microseconds(200);
        const auto coarseDeadline = nextDeadline_ - spinWindow;
        if (Clock::now() < coarseDeadline)
            std::this_thread::sleep_until(coarseDeadline);
        while (Clock::now() < nextDeadline_) { }
        nextDeadline_ = NextDeadline(nextDeadline_, Clock::now(), PeriodForFps(targetFps_));
    }

private:
    uint32_t targetFps_ = 0; // 0 = no CPU cap; swapchain may still throttle.
    bool armed_ = false;
    TimePoint nextDeadline_{};
};

} // namespace tour
