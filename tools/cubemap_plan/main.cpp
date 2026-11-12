// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Cascaded world-tile cubemap capture planner (the #338 planning API)
// applied to the bistro scene AABB. Prints the per-world-cell / per-cascade
// capture sets and their probe counts so a bake can be scoped before the
// capture pipeline is wired in.
#include <Engine/WorldPartition/CubemapCapturePlanning.h>

#include <cstdio>
#include <string>
#include <vector>

namespace wp = engine::worldpartition;

int main(int argc, char** argv) {
    // The bistro scene AABB (GiBaker --print-bounds trace bounds).
    wp::CubemapCaptureVolume volume{};
    volume.min = {-58.6003f, -4.72616f, -72.3586f};
    volume.max = {50.463f, 27.2062f, 42.9172f};
    volume.cascades = {{64.0f, 2.0f}, {256.0f, 3.0f}, {512.0f, 5.0f}};

    // No-op occlusion (no BVH loaded): every cell reads open, so the
    // planner emits the full density-grid probe sets. A production bake
    // passes the scene's Embree/BVH ray test here to skip inside-geometry
    // cells.
    const smvk::CubemapOcclusionRay occluded{}; // empty = never occluded

    std::vector<wp::CubemapTileCaptureSet> sets;
    std::string error;
    if (!wp::PlanCubemapTileCaptures(volume, occluded, sets, error)) {
        std::fprintf(stderr, "plan failed: %s\n", error.c_str());
        return 1;
    }

    std::printf("bistro cascaded cubemap plan: %zu capture set(s) over %u cascade tiers\n",
                sets.size(),
                (unsigned)volume.cascades.size());
    size_t totalProbes = 0;
    for (const auto& s : sets) {
        totalProbes += s.probes.size();
        std::printf("  cell (%lld,%lld) cascade %u: %zu probes\n",
                    (long long)s.cell.x,
                    (long long)s.cell.z,
                    s.cascade,
                    s.probes.size());
        for (size_t i = 0; i < s.probes.size() && i < 4; ++i) {
            const auto& p = s.probes[i];
            std::printf(
                "      probe %zu: pos=(%.2f, %.2f, %.2f) influence=%.1f fade=%.1f depth=%u\n",
                i,
                p.position.x,
                p.position.y,
                p.position.z,
                p.influenceRadius,
                p.fadeRadius,
                p.depth);
        }
    }
    std::printf("total probes: %zu\n", totalProbes);
    (void)argc;
    (void)argv;
    return 0;
}
