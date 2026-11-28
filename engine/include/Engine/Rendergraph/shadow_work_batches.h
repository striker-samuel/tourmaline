// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace tour::shadow_work {
inline constexpr uint32_t kExperimentalBatches = 3;

[[nodiscard]] constexpr bool Enabled(const char* value) {
    return value && value[0] == '1' && value[1] == '\0';
}

[[nodiscard]] constexpr uint32_t BatchCount(bool enabled, bool evsm, bool allGpuReady) {
    return enabled && !evsm && allGpuReady ? kExperimentalBatches : 1u;
}

struct Range {
    uint32_t begin, end;
};

// Partition each raster-class stride without overlap or omission. All input
// commands, including zero-count padding, retain their original addresses.
// Precondition: count > 0 and index < count.
[[nodiscard]] constexpr Range Slice(uint32_t stride, uint32_t index, uint32_t count) {
    return {uint32_t(uint64_t(stride) * index / count),
            uint32_t(uint64_t(stride) * (index + 1u) / count)};
}

struct PassStep {
    uint32_t cascade, batch;
    bool first, last;
};

// Precondition: cascades > 0 and pass < cascades * batches.
[[nodiscard]] constexpr PassStep Step(uint32_t pass, uint32_t cascades, uint32_t batches) {
    const uint32_t batch = pass / cascades;
    return {pass % cascades, batch, batch == 0u, batch + 1u == batches};
}
} // namespace tour::shadow_work
