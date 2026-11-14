// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Experimental, device-free CPU LOD oracle. Not wired to raster admission.
// The caller owns the shallow metadata and publishes a validated selection
// only after checking the current geometry generation and flight snapshot.
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace tour::gpu_scene::cpu_lod {
inline constexpr uint32_t InvalidLod = 0xffffffffu;

// Ordered finest (0) to coarsest (N-1), matching GPU-scene LOD rows.
// Residency is a physical property, not a promise of GPU raster eligibility.
struct ShallowLevel {
    float simplifyError = 0.0f;
    bool resident = false;
    bool boundsKnown = false;
    bool rasterEligible = false;
};

struct Selection {
    uint32_t desired = InvalidLod;  // requested by projected error, even if not resident
    uint32_t resident = InvalidLod; // safe candidate after residency/eligibility filtering
    bool valid() const noexcept { return resident != InvalidLod; }
};

// projectedPixelsPerWorldUnit = viewportHeight / (2*tan(verticalFov/2)*distance)
// supplied by the caller after conservative depth/distance treatment.
// A zero/invalid scale is NOT interpreted as proof a coarser LOD is safe.
[[nodiscard]] inline Selection Select(std::span<const ShallowLevel> levels,
                                       float projectedPixelsPerWorldUnit,
                                       float maxErrorPixels) noexcept {
    Selection result{};
    if (levels.empty() || levels.size() > 16 ||
        !std::isfinite(projectedPixelsPerWorldUnit) ||
        projectedPixelsPerWorldUnit <= 0.0f ||
        !std::isfinite(maxErrorPixels) || maxErrorPixels < 0.0f)
        return result;

    // Untrusted / incomplete metadata must not drive a visibility decision.
    for (const ShallowLevel& lod : levels)
        if (!lod.boundsKnown || !std::isfinite(lod.simplifyError) ||
            lod.simplifyError < 0.0f)
            return result;

    // Prefer the coarsest level meeting the error budget. If none meet it,
    // the finest level is the demand target.
    result.desired = 0;
    for (uint32_t i = 0; i < levels.size(); ++i)
        if (double(levels[i].simplifyError) *
                double(projectedPixelsPerWorldUnit) <= double(maxErrorPixels))
            result.desired = i;

    // Prefer an available equal/finer representation (never exceeds desired
    // error), then fall back to the closest coarser one to avoid holes.
    for (uint32_t i = result.desired + 1; i > 0; --i) {
        const uint32_t index = i - 1;
        if (levels[index].resident && levels[index].rasterEligible) {
            result.resident = index;
            return result;
        }
    }
    for (uint32_t i = result.desired + 1; i < levels.size(); ++i)
        if (levels[i].resident && levels[i].rasterEligible) {
            result.resident = i;
            return result;
        }
    return result; // no admissible geometry: caller MUST retain legacy fallback
}
} // namespace tour::gpu_scene::cpu_lod
