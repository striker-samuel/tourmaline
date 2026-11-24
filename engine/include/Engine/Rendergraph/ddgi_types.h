// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace smvk::ddgi {

// Initial fixed-grid implementation. Flat buffers avoid octahedral atlas borders.
inline constexpr uint32_t kIrrResolution = 8;
inline constexpr uint32_t kDistanceResolution = 16;
inline constexpr uint32_t kIrrBins = kIrrResolution * kIrrResolution;
inline constexpr uint32_t kDistanceBins = kDistanceResolution * kDistanceResolution;
inline constexpr uint32_t kMaxProbes = 32768;
inline constexpr uint32_t kMaxRays = 512;

struct Grid {
    float origin[3] = {};
    float spacing = 2.0f;
    uint32_t dims[3] = {16, 8, 16};
    uint32_t rays = 128;
    float maxDistance = 20.0f;
    float hysteresis = 0.95f;
};

struct alignas(16) RaySample {
    float direction[3];
    float distance;
    float radiance[3];
    uint32_t flags; // 0 miss, 1 front hit, 2 back hit
};
struct alignas(16) Irradiance {
    float rgb[3];
    float valid;
};
struct DistanceMoments {
    float mean, second;
};
static_assert(sizeof(RaySample) == 32 && offsetof(RaySample, flags) == 28);
static_assert(sizeof(Irradiance) == 16 && sizeof(DistanceMoments) == 8);

struct Sizes {
    uint32_t probes = 0;
    size_t scratch = 0, irradiance = 0, distance = 0;
};

// Reject malformed grids and allocations above the caller's device/budget limit.
inline bool CalculateSizes(const Grid& g, size_t byteBudget, Sizes& out) {
    out = {};
    if (!std::isfinite(g.spacing) || g.spacing <= 0 ||
        !std::isfinite(g.maxDistance) || g.maxDistance <= 0.001f ||
        g.maxDistance > 1.0e8f || !std::isfinite(g.hysteresis) ||
        g.hysteresis < 0 || g.hysteresis >= 1 ||
        g.rays == 0 || g.rays > kMaxRays)
        return false;
    uint64_t count = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(g.origin[i]) || g.dims[i] < 2 ||
            g.dims[i] > kMaxProbes || count > kMaxProbes / g.dims[i] ||
            !std::isfinite(g.origin[i] + (g.dims[i] - 1) * g.spacing))
            return false;
        count *= g.dims[i];
    }
    const uint64_t scratch = count * g.rays * sizeof(RaySample);
    const uint64_t irr = count * kIrrBins * sizeof(Irradiance);
    const uint64_t dist = count * kDistanceBins * sizeof(DistanceMoments);
    const uint64_t total = scratch + 2 * (irr + dist);
    if (total > byteBudget || total > std::numeric_limits<size_t>::max())
        return false;
    out = {static_cast<uint32_t>(count), static_cast<size_t>(scratch),
           static_cast<size_t>(irr), static_cast<size_t>(dist)};
    return true;
}

// Push mirrors use scalar members to avoid vector alignment surprises.
struct TracePush {
    uint64_t frameUniforms = 0, scratch = 0;
    float originX = 0, originY = 0, originZ = 0, spacing = 0;
    uint32_t dimX = 0, dimY = 0, dimZ = 0, rays = 0;
    float maxDistance = 0;
    uint32_t updateIndex = 0;
};
struct BlendPush {
    uint64_t scratch = 0, oldIrr = 0, oldDistance = 0, newIrr = 0, newDistance = 0;
    uint32_t probes = 0, rays = 0, reset = 1;
    float hysteresis = 0, maxDistance = 0;
    uint32_t pad = 0;
};
static_assert(sizeof(TracePush) == 56 && offsetof(TracePush, maxDistance) == 48);
static_assert(sizeof(BlendPush) == 64 && offsetof(BlendPush, probes) == 40);

} // namespace smvk::ddgi
