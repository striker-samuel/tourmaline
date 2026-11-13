// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless scene tile residency tests; integrator runs these via CTest.

#include <Engine/WorldPartition/SceneTileProvider.h>
#include <Engine/WorldPartition/CubemapTileProvider.h>
#include <Engine/WorldPartition/CubemapCapturePlanning.h>
#include <Engine/WorldPartition/CubemapCaptureDiagnostics.h>
#include <Engine/WorldPartition/StreamTicketLedger.h>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace wp = engine::worldpartition;
static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL " << what << "\n";
    }
}
int main() {
    wp::SceneTileProvider tiles;
    check(!tiles.SetManifest({{{11, {0, 0}}, 1}, {{11, {0, 0}}, 2}}), "duplicate cell rejected");
    check(!tiles.SetManifest({{{11, {0, 0}}, 0}}), "missing content id rejected");
    check(
        tiles.SetManifest(
            {{{11, {-1, 0}}, 101}, {{11, {0, 0}}, 102}, {{11, {1, 0}}, 103}, {{11, {3, 0}}, 104}}),
        "manifest accepted");
    std::vector<uint64_t> requested, retired, tickets;
    tiles.SetCallbacks(
        [&](const wp::SceneTileDescriptor& d, wp::DesiredResidency, uint64_t ticket) {
            requested.push_back(d.contentId);
            tickets.push_back(ticket);
        },
        [&](const wp::SceneTileDescriptor& d) {
            retired.push_back(d.contentId);
            return true;
        });
    wp::WorldPartition partition;
    partition.AddProvider(&tiles);
    wp::StreamingInterest interest{};
    interest.world = 11;
    interest.position = glm::dvec3(128, 0, 128);
    interest.radiusMeters = 0;
    interest.layerMask = wp::StreamLayerBit(wp::StreamLayer::Scene);
    partition.SetInterests(&interest, 1);
    partition.UpdateFrame(1);
    check(requested.size() == 1 && requested[0] == 102, "only the containing tile requested");
    check(partition.RequestCount() == 1 && partition.Requests()[0].layer == wp::StreamLayer::Scene,
          "published scene layer request");
    partition.UpdateFrame(2);
    check(requested.size() == 1, "pending requests deduplicated");
    check(!tiles.Activate({11, {-1, 0}}, tickets[0]), "unrequested cell cannot activate");
    check(tiles.Activate({11, {0, 0}}, tickets[0]) && tiles.IsResident({11, {0, 0}}),
          "acknowledged cell activates");
    check(!tiles.SetManifest({{{11, {2, 0}}, 201}}), "manifest replacement refused while active");
    interest.position = glm::dvec3(-128, 0, 128);
    partition.SetInterests(&interest, 1);
    partition.UpdateFrame(3);
    check(requested.size() == 2 && requested[1] == 101,
          "negative tile selection uses floor-based grid");
    check(retired.size() == 1 && retired[0] == 102 && !tiles.IsResident({11, {0, 0}}),
          "unwanted tile retires");
    partition.SetInterests(nullptr, 0);
    partition.UpdateFrame(4);
    check(tiles.PendingCount() == 0 && !tiles.Activate({11, {-1, 0}}, tickets[1]),
          "pending request canceled on interest loss");
    // A late completion after demand drops must not resurrect the cell.
    check(!tiles.Activate({11, {-1, 0}}, tickets[1]), "late completion from canceled IO rejected");
    check(!tiles.SetManifest({{{11, {5, 5}}, 401}, {{22, {5, 5}}, 402}}),
          "mixed-world manifest rejected");
    check(tiles.SetManifest({{{22, {5, 5}}, 402}}),
          "manifest replacement succeeds after retirement");
    interest.position = glm::dvec3(128, 0, 128);
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
    interest.position = glm::dvec3(5 * 256 + 128, 0, 5 * 256 + 128);
    interest.world = 22;
    partition.SetInterests(&interest, 1);
    partition.UpdateFrame(7);
    check(requested.back() == 402 && partition.Requests()[0].world == 22,
          "world-scoped demand selects other world's tile");
    check(!tiles.Activate({11, {5, 5}}, tickets.back()),
          "retired world cannot adopt newer world ticket");
    check(tiles.Activate({22, {5, 5}}, tickets.back()) && tiles.IsResident({22, {5, 5}}) &&
              !tiles.IsResident({11, {5, 5}}),
          "world-scoped residency isolated");

    {
        wp::SceneTileProvider budget;
        check(budget.SetManifest(77, {}), "empty Map still retains identity");
        check(budget.ActiveWorld() == 77, "empty Map active ID retained");
        check(budget.SetManifest({{{44, {-1, 0}}, 1}, {{44, {0, 0}}, 2}, {{44, {1, 0}}, 3}}),
              "priority fixture installed");
        budget.SetMaxNewRequestsPerFrame(1);
        std::vector<uint64_t> loadOrder;
        budget.SetCallbacks([&](const wp::SceneTileDescriptor& d,
                                wp::DesiredResidency,
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
        interests[1].position = glm::dvec3(384, 0, 128);
        interests[1].radiusMeters = 0;
        interests[1].priority = 10.0f;
        p.SetInterests(interests, 2);
        p.UpdateFrame(20);
        check(loadOrder.size() == 1 && loadOrder.front() == 3,
              "highest-priority tile requested first");
        p.UpdateFrame(21);
        check(loadOrder.size() == 2, "per-frame request budget enforced");
    }
    {
        // Real SceneTileProvider participates in the partition-owned budget.
        wp::WorldPartition p;
        p.Budgets().SetLimits({80, 200, 1});
        wp::SceneTileProvider budgeted;
        check(budgeted.SetManifest({{{88, {0, 0}}, 1}, {{88, {1, 0}}, 2}}),
              "budgeted scene fixture accepted");
        budgeted.SetCostEstimator([](const wp::SceneTileDescriptor&) {
            return std::pair<uint64_t,uint64_t>{80, 200};
        });
        std::vector<uint64_t> loaded;
        std::vector<uint64_t> tickets;
        budgeted.SetCallbacks(
            [&](const wp::SceneTileDescriptor& d, wp::DesiredResidency, uint64_t ticket) {
                loaded.push_back(d.contentId);
                tickets.push_back(ticket);
            },
            [](const wp::SceneTileDescriptor&) { return true; });
        p.AddProvider(&budgeted);
        wp::StreamingInterest interest{};
        interest.world = 88;
        interest.position = glm::dvec3(128, 0, 128);
        interest.radiusMeters = 512;
        interest.layerMask = wp::StreamLayerBit(wp::StreamLayer::Scene);
        p.SetInterests(&interest, 1);
        p.UpdateFrame(1);
        check(loaded.size() == 1 && p.Budgets().InFlightRequests() == 1,
              "global budget admits one scene IO request");
        check(budgeted.Activate({88, {0, 0}}, tickets[0]),
              "scene first request activated");
        p.UpdateFrame(2);
        check(loaded.size() == 1 && p.Budgets().InFlightRequests() == 0 &&
              p.Budgets().ReservedGpuBytes() == 200,
              "resident scene GPU budget blocks second tile");
        interest.position = glm::dvec3(384, 0, 128);
        interest.radiusMeters = 0;
        p.SetInterests(&interest, 1);
        p.UpdateFrame(3); // old tile retires after CommitFrame
        p.UpdateFrame(4);
        check(loaded.size() == 2 && loaded.back() == 2,
              "second tile admitted after old reservation retires");
        check(budgeted.ResetPending({88, {1, 0}}, tickets.back()) &&
              p.Budgets().ReservationCount() == 0,
              "failed tile IO releases shared reservation");
        p.ClearProviders();
    }
    {
        struct Request { int id; float priority; };
        std::vector<Request> work{{1, 1.0f}, {2, 3.0f}, {3, 3.0f}, {4, -1.0f}};
        wp::SelectStreamRequests(work, 2, [](const Request& a, const Request& b) {
            return a.priority > b.priority;
        });
        check(work.size() == 2 && work[0].id == 2 && work[1].id == 3,
              "shared request selector respects budget and stable ties");
        wp::SelectStreamRequests(work, 0, [](const Request& a, const Request& b) {
            return a.priority > b.priority;
        });
        check(work.empty(), "shared request selector honors zero budget");
    }
    {
        // Both scene and environment providers use these shared ticket rules.
        wp::StreamTicketLedger<int> ledger;
        const auto first = ledger.Issue(7, 1);
        check(first.has_value() && ledger.PendingCount() == 1,
              "ticket issue creates one pending request");
        check(!ledger.Issue(7, 1).has_value(), "duplicate issue rejected");
        check(first.has_value() && !ledger.Accept(7, *first + 1),
              "wrong ticket cannot activate");
        ledger.CancelUndesired([](int) { return false; });
        check(first.has_value() && !ledger.Accept(7, *first),
              "cancelled ticket cannot activate");
        const auto second = ledger.Issue(7, 2);
        check(second.has_value() && first.has_value() && *second != *first,
              "reissue after cancellation has new generation");
        check(second.has_value() && ledger.Fail(7, *second, 2, 120),
              "failure records retry cooldown");
        check(!ledger.CanIssue(7, 121) && ledger.CanIssue(7, 122),
              "retry waits through complete cooldown");
        const auto third = ledger.Issue(7, 122);
        check(third.has_value(), "request resumes after cooldown");
        ledger.Clear();
        check(ledger.PendingCount() == 0 && third.has_value() &&
              !ledger.Accept(7, *third), "source change rejects stale completion");
        const auto fourth = ledger.Issue(7, 123);
        check(fourth.has_value() && third.has_value() && *fourth > *third,
              "clear never resets ticket generations");
        check(fourth.has_value() && ledger.Accept(7, *fourth) &&
              ledger.PendingCount() == 0, "valid completion clears pending");
    }
    {
        // Bounded octree capture planning follows the WorldPartition grid,
        // supports negative coordinates, and has an implicit sky tier.
        wp::CubemapCaptureVolume v{};
        v.min = {-2.0f, 1.0f, 0.0f};
        v.max = {2.0f, 5.0f, 2.0f};
        v.cascades = {{64.0f, 2.0f}, {256.0f, 3.0f}, {512.0f, 5.0f}};
        std::vector<wp::CubemapTileCaptureSet> sets;
        std::string err;
        check(wp::PlanCubemapTileCaptures(v, {}, sets, err) &&
                  sets.size() == 6 && sets.front().cell.x == -1 &&
                  sets.back().cell.x == 0 && sets.back().cascade == 2,
              "cubemap tile planning splits at negative/positive world-cell seam");
        check(wp::SelectCubemapCascade(v, 1.0) == 0 &&
                  wp::SelectCubemapCascade(v, 100.0) == 1 &&
                  wp::SelectCubemapCascade(v, 300.0) == 2 &&
                  wp::SelectCubemapCascade(v, 512.0) == 3,
              "cubemap distance tiers terminate at IBL/sky without baking");
        // Per-tier budgets are independent: a dense near tier can be
        // granted more probes without multiplying every far-tier cost.
        v.cascades[0].maxProbesPerCell = 8;
        v.cascades[1].maxProbesPerCell = 16;
        v.cascades[2].maxProbesPerCell = 32;
        std::vector<wp::CubemapTileCaptureSet> tiered;
        check(wp::PlanCubemapTileCaptures(v, {}, tiered, err) &&
                  tiered.size() == 6 &&
                  tiered[0].probes.size() <= 8 &&
                  tiered[1].probes.size() <= 16 &&
                  tiered[2].probes.size() <= 32 &&
                  tiered[3].probes.size() <= 8 &&
                  tiered[4].probes.size() <= 16 &&
                  tiered[5].probes.size() <= 32,
              "cubemap cascade budgets bound each tile and tier separately");
        v.cascades[1].outerRadiusMeters = 32.0f;
        const size_t oldSize = sets.size();
        check(!wp::PlanCubemapTileCaptures(v, {}, sets, err) &&
                  sets.size() == oldSize,
              "invalid cascade config leaves existing captures untouched");
    }
    {
        // Diagnostics must not conflate equal counts with identical
        // placement. Compare tiers by one-to-one position matching.
        wp::CubemapTileCaptureSet first{};
        smvk::CubemapPlacedProbe a{};
        a.position = {0.0f, 0.0f, 0.0f};
        a.influenceRadius = 3.0f;
        a.fadeRadius = 1.5f;
        a.depth = 2;
        smvk::CubemapPlacedProbe b = a;
        b.position = {5.0f, 0.0f, 0.0f};
        b.influenceRadius = 7.0f;
        b.fadeRadius = 3.5f;
        first.probes = {a, b};

        const auto base = wp::AnalyzeCubemapCaptureQuality(first);
        check(base.probes == 2 && !base.hasPrevious &&
                  base.minInfluenceMeters == 3.0f &&
                  base.medianInfluenceMeters == 7.0f &&
                  base.maxInfluenceMeters == 7.0f,
              "cubemap diagnostics measure probe radius distribution");

        wp::CubemapTileCaptureSet copy = first;
        const auto same = wp::AnalyzeCubemapCaptureQuality(copy, &first);
        check(same.hasPrevious && same.matchingPreviousPositions == 2 &&
                  same.equivalentToPrevious,
              "cubemap diagnostics identify equivalent tiers");

        copy.probes[1].position.x += 1.0f; // equal count, different geometry
        const auto shifted = wp::AnalyzeCubemapCaptureQuality(copy, &first);
        check(shifted.probes == 2 && shifted.matchingPreviousPositions == 1 &&
                  !shifted.equivalentToPrevious,
              "cubemap diagnostics distinguish equal-count shifted tiers");

        copy = first;
        copy.probes[0].influenceRadius += 2.0f;
        const auto changedRadius = wp::AnalyzeCubemapCaptureQuality(copy, &first);
        check(changedRadius.matchingPreviousPositions == 2 &&
                  !changedRadius.equivalentToPrevious,
              "cubemap diagnostics distinguish equal-position radius changes");

        copy.probes = {a, a}; // two current probes cannot match one prior
        const auto duplicate = wp::AnalyzeCubemapCaptureQuality(copy, &first);
        check(duplicate.matchingPreviousPositions == 1 &&
                  !duplicate.equivalentToPrevious,
              "cubemap diagnostics one-to-one match prevents false overlap");

        wp::CubemapTileCaptureSet empty{};
        const auto emptyPair = wp::AnalyzeCubemapCaptureQuality(empty, &empty);
        check(emptyPair.probes == 0 && emptyPair.equivalentToPrevious,
              "cubemap diagnostics handle empty captures");
    }
    {
        // Cubemap capture tiles participate in the shared partition frame
        // lifecycle, but retain independent, ticket-checked GPU residency.
        wp::CubemapTileProvider cubes;
        wp::CubemapCaptureTile one{};
        one.tile.world = 99;
        one.tile.cell = {0, 0};
        one.captureSetId = 123;
        check(cubes.SetManifest(99, {one}), "cubemap capture manifest accepted");
        uint64_t ticket = 0;
        int requestCount = 0, retireCount = 0;
        cubes.SetCallbacks(
            [&](const wp::CubemapCaptureTile&, uint64_t issued) {
                ticket = issued;
                ++requestCount;
            },
            [&](const wp::CubemapCaptureTile&) {
                ++retireCount;
                return true;
            });
        wp::WorldPartition part;
        part.AddProvider(&cubes);
        wp::StreamingInterest interest{};
        interest.world = 99;
        interest.layerMask = wp::StreamLayerBit(wp::StreamLayer::Environment);
        interest.position = {100.0, 0.0, 100.0};
        interest.radiusMeters = 0;
        part.SetInterests(&interest, 1);
        part.UpdateFrame(1);
        part.UpdateFrame(2);
        check(requestCount == 1 && cubes.PendingCount() == 1,
              "cubemap tile request is deduplicated");
        check(!cubes.Activate(one.tile, ticket + 1) &&
                  cubes.Activate(one.tile, ticket) && cubes.ResidentCount() == 1,
              "cubemap tile rejects incorrect completion ticket");
        interest.position = {600.0, 0.0, 600.0};
        part.SetInterests(&interest, 1);
        part.UpdateFrame(3);
        check(retireCount == 1 && cubes.ResidentCount() == 0,
              "cubemap tile retires after losing streaming interest");
    }
    std::cout << "scene_tile_provider failures=" << failures << "\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
