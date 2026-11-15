// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/gpu_scene_meshlet_prefix.h>
#include <Engine/Renderer/gpu_scene_meshlet_compact_abi.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while(false)
int main() {
    using namespace tour::gpu_scene;
    MeshletPrefixPlan plan;
    CHECK(MeshletPrefixPlan::Build({}, {}, 0u, plan));
    CHECK(plan.offsets == std::vector<uint32_t>{0u});
    std::array<MeshletRange, 4> ranges{{
        {5u, 3u, false}, {0u, 0u, true}, {200u, 2u, false},
        {0xffffffffu, 2u, false} // wrap -> one whole-submesh fallback
    }};
    std::array<uint32_t, 4> classes{{0u, 1u, 3u, 2u}};
    CHECK(MeshletPrefixPlan::Build(ranges, classes, 7u, plan));
    CHECK((plan.offsets == std::vector<uint32_t>{0,3,4,6,7}));
    CHECK(plan.candidates.size() == 7);
    CHECK(plan.candidates[0].clusterIndex == 5u);
    CHECK(plan.candidates[1].clusterIndex == 6u);
    CHECK(plan.candidates[2].clusterIndex == 7u);
    CHECK(plan.candidates[3].fullSubmesh && plan.candidates[3].workIndex == 1u);
    CHECK(plan.candidates[4].clusterIndex == 200u);
    CHECK(plan.candidates[5].clusterIndex == 201u);
    CHECK(plan.candidates[6].fullSubmesh && plan.candidates[6].workIndex == 3u);
    PsoPrefixPlan compact;
    std::vector<uint32_t> candidateClasses;
    for (auto c : plan.candidates) candidateClasses.push_back(c.rasterClass);
    CHECK(PsoPrefixPlan::Build(candidateClasses, {{3,1,1,2}}, compact));
    CHECK((compact.counts == PsoPrefixPlan::Counts{3,1,1,2}));
    // Count and scatter must reject exactly the same records. A zero-index
    // candidate in a valid raster class previously inflated the GPU class
    // count without emitting a command, leaving an unwritten indirect slot.
    std::array<CompactMeshletCandidate, 7> emitted{};
    for (uint32_t i = 0; i < emitted.size(); ++i) {
        emitted[i].rasterClass = candidateClasses[i];
        emitted[i].indexCount = 3u;
    }
    emitted[1].indexCount = 0u; // classified, but no raster command
    emitted[4].rasterClass = 0xffffffffu; // culled/invalid
    emitted[5].rasterClass = 4u; // unsupported class
    std::vector<uint32_t> survivors;
    for (const auto& candidate : emitted)
        survivors.push_back(CompactMeshletSurvivorClass(candidate));
    CHECK(PsoPrefixPlan::Build(survivors, {{3,1,1,2}}, compact));
    CHECK((compact.counts == PsoPrefixPlan::Counts{2,0,0,2}));
    CHECK(compact.destinations[1] == kPsoPrefixInvalid);
    CHECK(compact.destinations[4] == kPsoPrefixInvalid);
    CHECK(compact.destinations[5] == kPsoPrefixInvalid);
    // The shader uses four nonoverlapping worst-case bins. Its command
    // address space must accommodate bin 3 at the maximum candidate bound.
    constexpr uint64_t maxCommands = 4ull * kMeshletCandidateLimit;
    constexpr uint64_t lastBinDestination = 3ull * kMeshletCandidateLimit +
                                             (kMeshletCandidateLimit - 1u);
    static_assert(lastBinDestination < maxCommands);
    static_assert(lastBinDestination >= kMeshletCandidateLimit);
    auto old = plan.candidates;
    CHECK(!MeshletPrefixPlan::Build(ranges, classes, 6u, plan));
    CHECK(plan.candidates.size() == old.size());
    for (size_t i = 0; i < old.size(); ++i)
        CHECK(plan.candidates[i].clusterIndex == old[i].clusterIndex);
    std::vector<MeshletRange> many(1024, {0, 1024, false});
    std::vector<uint32_t> manyClasses(many.size(), 0);
    CHECK(MeshletPrefixPlan::Build(many, manyClasses, 1048576u, plan));
    CHECK(plan.candidates.size() == 1048576u);
    CHECK(!MeshletPrefixPlan::Build(many, manyClasses, 1048575u, plan));
    return 0;
}
