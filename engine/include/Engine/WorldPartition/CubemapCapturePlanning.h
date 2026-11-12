// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// CPU-only, world-partition-aligned planning for baked reflection captures.
// WorldPartition owns 256 m XZ cells; the volume limits X/Y/Z placement.
// Each non-sky cascade is baked offline. The final cascade intentionally
// has no captures: rendering falls back to the existing environment IBL.

#include <Engine/Renderer/cubemap_placement.h>
#include <Engine/WorldPartition/WorldPartition.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace engine::worldpartition {

struct CubemapCascade {
    float outerRadiusMeters = 64.0f;
    float densityMeters = 2.0f;
};

struct CubemapCaptureVolume {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    // Cascades are ordered near-to-far; the sky/IBL tier is implicit.
    std::vector<CubemapCascade> cascades{{64.0f, 2.0f},
                                         {256.0f, 3.0f},
                                         {512.0f, 5.0f}};
    uint32_t maxDepth = 12;
    uint32_t maxProbesPerCell = 128;
};

struct CubemapTileCaptureSet {
    WorldCellKey cell{};
    uint32_t cascade = 0;
    std::vector<smvk::CubemapPlacedProbe> probes;
};

// Returns cascades.size() for the sky/IBL fallback (no cubemap capture).
inline size_t SelectCubemapCascade(const CubemapCaptureVolume& volume, double distanceMeters) {
    if (!std::isfinite(distanceMeters) || distanceMeters < 0.0)
        return volume.cascades.size();
    for (size_t i = 0; i < volume.cascades.size(); ++i)
        if (distanceMeters < volume.cascades[i].outerRadiusMeters)
            return i;
    return volume.cascades.size();
}

// A tile may have multiple pre-baked density tiers. Runtime requests only
// the selected tier, never rebuilds the octree when the camera moves.
// The output is replaced only on success, and sorted cell/cascade order.
inline bool PlanCubemapTileCaptures(const CubemapCaptureVolume& volume,
                                   const smvk::CubemapOcclusionRay& occluded,
                                   std::vector<CubemapTileCaptureSet>& output,
                                   std::string& error) {
    error.clear();
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(volume.min[a]) || !std::isfinite(volume.max[a]) ||
            volume.min[a] >= volume.max[a]) {
            error = "cubemap capture bounds must be finite and nonempty";
            return false;
        }
    }
    if (!volume.maxProbesPerCell || !volume.maxDepth || volume.maxDepth > 24 ||
        volume.cascades.empty()) {
        error = "cubemap capture settings invalid";
        return false;
    }
    float prev = 0.0f;
    for (const auto& cascade : volume.cascades) {
        if (!std::isfinite(cascade.outerRadiusMeters) ||
            !std::isfinite(cascade.densityMeters) ||
            cascade.outerRadiusMeters <= prev || cascade.densityMeters <= 0.0f) {
            error = "cubemap cascades require increasing finite ranges and positive densities";
            return false;
        }
        prev = cascade.outerRadiusMeters;
    }

    // A half-open upper bound prevents duplicate cells on exact XZ seams.
    const double x0 = std::floor(double(volume.min.x) / kRootCellSize);
    const double x1 = std::floor(
        std::nextafter(double(volume.max.x), -std::numeric_limits<double>::infinity()) /
        kRootCellSize);
    const double z0 = std::floor(double(volume.min.z) / kRootCellSize);
    const double z1 = std::floor(
        std::nextafter(double(volume.max.z), -std::numeric_limits<double>::infinity()) /
        kRootCellSize);
    const double limit = double(std::numeric_limits<int64_t>::max()) / 2.0;
    if (x0 < -limit || x1 > limit || z0 < -limit || z1 > limit ||
        (x1 - x0 + 1.0) * (z1 - z0 + 1.0) > 4096.0) {
        error = "cubemap capture volume covers too many world cells";
        return false;
    }

    std::vector<CubemapTileCaptureSet> next;
    for (double x = x0; x <= x1; ++x) {
        for (double z = z0; z <= z1; ++z) {
            const glm::vec3 mn =
                glm::max(volume.min, glm::vec3(float(x * kRootCellSize),
                                               volume.min.y, float(z * kRootCellSize)));
            const glm::vec3 mx =
                glm::min(volume.max, glm::vec3(float((x + 1.0) * kRootCellSize),
                                               volume.max.y, float((z + 1.0) * kRootCellSize)));
            // Bounds can become zero-width after float conversion at large
            // coordinates; do not accidentally emit a degenerate fallback.
            if (mx.x <= mn.x || mx.z <= mn.z) continue;
            for (uint32_t tier = 0; tier < volume.cascades.size(); ++tier) {
                smvk::CubemapOctreeOptions opts{};
                opts.maxCellSize = volume.cascades[tier].densityMeters;
                opts.minCellSize = volume.cascades[tier].densityMeters;
                opts.maxDepth = volume.maxDepth;
                opts.maxProbes = volume.maxProbesPerCell;
                CubemapTileCaptureSet set{};
                set.cell = {static_cast<int64_t>(x), static_cast<int64_t>(z)};
                set.cascade = tier;
                std::string log;
                smvk::BuildOctreeProbePositions(mn, mx, opts, occluded, set.probes, log);
                next.push_back(std::move(set));
            }
        }
    }
    output = std::move(next);
    return true;
}

} // namespace engine::worldpartition
