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
    check(!tiles.SetManifest({{{11,{0,0}}, 1}, {{11,{0,0}}, 2}}),
        "duplicate cell rejected");
    check(!tiles.SetManifest({{{11,{0,0}}, 0}}), "missing content id rejected");
    check(tiles.SetManifest({{{11,{-1,0}}, 101}, {{11,{0,0}}, 102},
        {{11,{1,0}}, 103}, {{11,{3,0}}, 104}}), "manifest accepted");
    std::vector<uint64_t> requested, retired, tickets;
    tiles.SetCallbacks(
        [&](const wp::SceneTileDescriptor& d, wp::DesiredResidency, uint64_t ticket) {
            requested.push_back(d.contentId);
            tickets.push_back(ticket);
        },
        [&](const wp::SceneTileDescriptor& d) {
            retired.push_back(d.contentId);
            return true;
        }
    );
    wp::WorldPartition partition;
    partition.AddProvider(&tiles);
    wp::StreamingInterest interest{};
    interest.world = 11;
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
    check(!tiles.Activate({11,{-1,0}}, tickets[0]), "unrequested cell cannot activate");
    check(tiles.Activate({11,{0,0}}, tickets[0]) && tiles.IsResident({11,{0,0}}),
        "acknowledged cell activates");
    check(!tiles.SetManifest({{{11,{2,0}}, 201}}),
        "manifest replacement refused while active");
    interest.position = glm::dvec3(-128,0,128);
    partition.SetInterests(&interest, 1);
    partition.UpdateFrame(3);
    check(requested.size() == 2 && requested[1] == 101,
        "negative tile selection uses floor-based grid");
    check(retired.size() == 1 && retired[0] == 102
        && !tiles.IsResident({11,{0,0}}), "unwanted tile retires");
    partition.SetInterests(nullptr, 0);
    partition.UpdateFrame(4);
    check(tiles.PendingCount() == 0 && !tiles.Activate({11,{-1,0}}, tickets[1]),
        "pending request canceled on interest loss");
    // A late completion after demand drops must not resurrect the cell.
    check(!tiles.Activate({11,{-1,0}}, tickets[1]),
        "late completion from canceled IO rejected");
    check(!tiles.SetManifest({{{11,{5,5}}, 401}, {{22,{5,5}}, 402}}),
        "mixed-world manifest rejected");
    check(tiles.SetManifest({{{22,{5,5}}, 402}}),
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
    // Two worlds own the same cell coordinate but never share request,
    // residency or resource lifetime.
    interest.radiusMeters = 0;
    interest.position = glm::dvec3(5*256+128, 0, 5*256+128);
    interest.world = 22;
    partition.SetInterests(&interest, 1);
    partition.UpdateFrame(7);
    check(requested.back() == 402
        && partition.Requests()[0].world == 22,
        "world-scoped demand selects other world's tile");
    check(!tiles.Activate({11,{5,5}}, tickets.back()),
        "retired world cannot adopt newer world ticket");
    check(tiles.Activate({22,{5,5}}, tickets.back())
        && tiles.IsResident({22,{5,5}})
        && !tiles.IsResident({11,{5,5}}),
        "world-scoped residency isolated");

    {
        wp::SceneTileProvider budget;
        check(budget.SetManifest(77, {}), "empty Map still retains identity");
        check(budget.ActiveWorld() == 77, "empty Map active ID retained");
        check(budget.SetManifest({{{44,{-1,0}}, 1}, {{44,{0,0}}, 2},
            {{44,{1,0}}, 3}}), "priority fixture installed");
        budget.SetMaxNewRequestsPerFrame(1);
        std::vector<uint64_t> loadOrder;
        budget.SetCallbacks(
            [&](const wp::SceneTileDescriptor& d, wp::DesiredResidency,
                uint64_t) { loadOrder.push_back(d.contentId); },
            [](const wp::SceneTileDescriptor&) { return true; });
        wp::WorldPartition p;
        p.AddProvider(&budget);
        wp::StreamingInterest interests[2]{};
        interests[0].world = 44;
        interests[0].radiusMeters = 1024.0;
        interests[0].priority = 1.0f;
        interests[0].layerMask = wp::StreamLayerBit(wp::StreamLayer::Scene);
        interests[1] = interests[0];
        interests[1].position = glm::dvec3(384,0,128);
        interests[1].radiusMeters = 0;
        interests[1].priority = 10.0f;
        p.SetInterests(interests, 2);
        p.UpdateFrame(20);
        check(loadOrder.size() == 1 && loadOrder.front() == 3,
            "highest-priority tile requested first");
        p.UpdateFrame(21);
        check(loadOrder.size() == 2, "per-frame request budget enforced");
    }
    std::cout << "scene_tile_provider failures=" << failures << "\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
