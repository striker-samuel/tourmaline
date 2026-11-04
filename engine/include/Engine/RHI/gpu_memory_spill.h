// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>

namespace smvk {

// Advisory state only: a nonlocal heap increase is NOT proof of OS eviction.
// VMA sees allocations and estimated heap budgets, not WDDM residency moves.
enum class GpuMemoryWarning : uint8_t {
    Unavailable, Healthy, Pressure, Critical, SpillSuspected
};

struct GpuMemoryPressureSample {
    uint64_t localUsageBytes = 0;
    uint64_t localBudgetBytes = 0;
    uint64_t nonlocalUsageBytes = 0;
    bool nonlocalAvailable = false;
};

struct GpuMemoryWarningResult {
    GpuMemoryWarning state = GpuMemoryWarning::Unavailable;
    bool changed = false;
    double localRatio = 0.0;
    uint64_t nonlocalGrowthBytes = 0;
};

class GpuMemorySpillDetector {
public:
    // Call at a low fixed cadence (shipping: roughly once a second).
    // seconds is time elapsed since the previous observation.
    GpuMemoryWarningResult Update(const GpuMemoryPressureSample& sample,
                                  double seconds) noexcept {
        if (sample.localBudgetBytes == 0) {
            Reset();
            return {GpuMemoryWarning::Unavailable, false, 0.0, 0};
        }
        const double ratio = static_cast<double>(sample.localUsageBytes) /
                             static_cast<double>(sample.localBudgetBytes);
        if (!baselineSet_ && sample.nonlocalAvailable) {
            baselineSet_ = true;
            nonlocalBaseline_ = sample.nonlocalUsageBytes;
        }
        // Track the low-water mark; pre-existing host allocations are not spills.
        if (sample.nonlocalAvailable && baselineSet_)
            nonlocalBaseline_ = std::min(nonlocalBaseline_, sample.nonlocalUsageBytes);
        const uint64_t growth = sample.nonlocalAvailable && baselineSet_ &&
                                sample.nonlocalUsageBytes > nonlocalBaseline_
            ? sample.nonlocalUsageBytes - nonlocalBaseline_ : 0;

        const double dt = seconds > 0.0 && seconds < 60.0 ? seconds : 0.0;
        pressureSeconds_ = ratio >= 0.85 ? pressureSeconds_ + dt : 0.0;
        clearSeconds_ = ratio < 0.80 ? clearSeconds_ + dt : 0.0;

        GpuMemoryWarning next = state_;
        if (ratio >= 0.85 && growth >= 64ull * 1024ull * 1024ull &&
            pressureSeconds_ >= 2.0) {
            next = GpuMemoryWarning::SpillSuspected;
        } else if (ratio >= 0.95) {
            next = GpuMemoryWarning::Critical;
        } else if (pressureSeconds_ >= 2.0 && next != GpuMemoryWarning::SpillSuspected) {
            next = GpuMemoryWarning::Pressure;
        } else if (ratio < 0.80 && clearSeconds_ >= 5.0) {
            next = GpuMemoryWarning::Healthy;
        } else if (next == GpuMemoryWarning::Unavailable) {
            next = GpuMemoryWarning::Healthy;
        }

        // De-escalate suspected spill when the growth disappears even if
        // local usage remains pressured; never claim a confirmed eviction.
        if (next == GpuMemoryWarning::SpillSuspected && growth < 64ull * 1024ull * 1024ull) {
            next = ratio >= 0.95 ? GpuMemoryWarning::Critical :
                   ratio >= 0.85 ? GpuMemoryWarning::Pressure : GpuMemoryWarning::Healthy;
        }
        const bool changed = next != state_;
        state_ = next;
        return {next, changed, ratio, growth};
    }

    void Reset() noexcept {
        state_ = GpuMemoryWarning::Unavailable;
        baselineSet_ = false;
        nonlocalBaseline_ = 0;
        pressureSeconds_ = clearSeconds_ = 0.0;
    }

private:
    GpuMemoryWarning state_ = GpuMemoryWarning::Unavailable;
    uint64_t nonlocalBaseline_ = 0;
    bool baselineSet_ = false;
    double pressureSeconds_ = 0.0;
    double clearSeconds_ = 0.0;
};

} // namespace smvk
