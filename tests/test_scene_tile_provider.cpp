// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless scene tile residency tests; integrator runs these via CTest.

#include <Engine/WorldPartition/SceneTileProvider.h>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace wp = engine::worldpartition;
static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::cerr << "FAIL " << what << "\n"; }
}
int main() {
    wp::SceneTileProvider tiles;
    check(!tiles.SetManifest({{{0,0}, 1}, {{0,0}, 2}}),
        "duplicate cell rejected");
    check(!tiles.SetManifest({{{0,0}, 0}}), "missing content id rejected");
    check(tiles.SetManifest({{{-1,0}, 101}, {{0,0}, 102},
        {{1,0}, 103}, {{3,0}, 104}}), "manifest accepted");
    std::vector<uint64_t> requested, retired, tickets;
    tiles.SetCallbacks(
        [&](const wp::SceneTileDescriptor& d, wp::DesiredResidency, uint64_t ticket) {
            requested.push_back(d.contentId);
            tickets.push_back(ticket);
        },
        [&](const wp::SceneTileDescriptor& d) { retired.push_back(d.contentId); }
    );
    wp::WorldPartition partition;
    partition.AddProvider(&tiles);
    wp::StreamingInterest interest{};
    interest.position = glm::dvec3(128, 0, 128);
    interest.radiusMeters = 0;
    interest.layerMask = wp::StreamLayerBit(wp::StreamLayer::Scene);
    partition.SetInterests(&interest, 1);
    partition.UpdateFrame(1);
    check(requested.size() == 1 && requested[0] == 102,
        "only the containing tile requested");
    check(partition.RequestCount() == 1
        && partition.Requests()[0].layer == wp::StreamLayer::Scene,
        "published scene layer request");
    partition.UpdateFrame(2);
    check(requested.size() == 1, "pending requests deduplicated");
    check(!tiles.Activate({-1,0}, tickets[0]), "unrequested cell cannot activate");
    check(tiles.Activate({0,0}, tickets[0]) && tiles.IsResident({0,0}),
        "acknowledged cell activates");
    check(!tiles.SetManifest({{{2,0}, 201}}),
        "manifest replacement refused while active");
    interest.position = glm::dvec3(-128,0,128);
    partition.SetInterests(&interest, 1);
    partition.UpdateFrame(3);
    check(requested.size() == 2 && requested[1] == 101,
        "negative tile selection uses floor-based grid");
    check(retired.size() == 1 && retired[0] == 102
        && !tiles.IsResident({0,0}), "unwanted tile retires");
    partition.SetInterests(nullptr, 0);
    partition.UpdateFrame(4);
    check(tiles.PendingCount() == 0 && !tiles.Activate({-1,0}, tickets[1]),
        "pending request canceled on interest loss");
    // A late completion after demand drops must not resurrect the cell.
    check(!tiles.Activate({-1,0}, tickets[1]),
        "late completion from canceled IO rejected");
    check(tiles.SetManifest({{{5,5}, 401}}),
        "manifest replacement succeeds after retirement");
    interest.position = glm::dvec3(128,0,128);
    interest.radiusMeters = 1000000;
    interest.layerMask = wp::StreamLayerBit(wp::StreamLayer::Terrain);
    partition.SetInterests(&interest, 1);
    partition.UpdateFrame(5);
    check(partition.RequestCount() == 0, "scene layer mask honored");
    interest.layerMask = wp::StreamLayerBit(wp::StreamLayer::Scene);
    interest.radiusMeters = -1;
    partition.SetInterests(&interest, 1);
    partition.UpdateFrame(6);
    check(partition.RequestCount() == 0, "negative radius rejected");
    std::cout << "scene_tile_provider failures=" << failures << "\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
