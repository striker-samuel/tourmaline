// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/gpu_scene_task_mesh.h>
#include <Engine/Renderer/gpu_scene_task_mesh_abi.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while(false)

int main() {
    using namespace tour::gpu_scene;
    TaskMeshletPlan plan;
    CHECK(TaskMeshletPlan::Build({}, {}, 0u, plan));
    std::array<MeshletRange, 6> ranges{{
        {100u, 32u, false}, {200u, 33u, false},
        {0u, 0u, true}, {400u, 1u, false},
        {0xffffffffu, 2u, false}, {900u, 1025u, false}
    }};
    std::array<uint32_t, 6> classes{{0u, 1u, 2u, 3u, 0u, 2u}};
    CHECK(TaskMeshletPlan::Build(ranges, classes, 37u, plan));
    CHECK(plan.classes[0].size() == 1u);
    CHECK(plan.classes[1].size() == 2u);
    CHECK(plan.classes[2].size() == 33u);
    CHECK(plan.classes[2].front().workSlot == 5u);
    CHECK(plan.classes[2].front().firstCluster == 900u);
    CHECK(plan.classes[2].back().firstCluster == 900u + 32u * 32u);
    CHECK(plan.classes[2].back().clusterCount == 1u);
    CHECK(plan.classes[3].size() == 1u);
    CHECK(plan.classes[0][0].workSlot == 0u);
    CHECK(plan.classes[0][0].firstCluster == 100u);
    CHECK(plan.classes[0][0].clusterCount == 32u);
    CHECK(plan.classes[1][0].workSlot == 1u);
    CHECK(plan.classes[1][0].clusterCount == 32u);
    CHECK(plan.classes[1][1].firstCluster == 232u);
    CHECK(plan.classes[1][1].clusterCount == 1u);
    CHECK(plan.classes[3][0].workSlot == 3u);
    CHECK((plan.indexedFallbackWork == std::vector<uint32_t>{2u,4u}));

    auto before = plan;
    CHECK(!TaskMeshletPlan::Build(ranges, classes, 36u, plan));
    CHECK(plan.classes[1].size() == before.classes[1].size());
    CHECK(plan.indexedFallbackWork == before.indexedFallbackWork);
    std::array<uint32_t, 5> wrongClassCount{{0u,1u,2u,3u,0u}};
    CHECK(!TaskMeshletPlan::Build(ranges, wrongClassCount, 10u, plan));
    CHECK(plan.classes[0].size() == before.classes[0].size());

    // Submeshes larger than 1024 meshlets remain eligible: 4096 / 32 groups.
    std::array<MeshletRange, 1> large{{{17u,4096u,false}}};
    std::array<uint32_t, 1> largeClass{{2u}};
    CHECK(TaskMeshletPlan::Build(large, largeClass, 128u, plan));
    CHECK(plan.classes[2].size() == 128u);
    CHECK(plan.classes[2].back().firstCluster == 17u+127u*32u);
    CHECK(plan.classes[2].back().clusterCount == 32u);
    CHECK(!TaskMeshletPlan::Build(large, largeClass, 127u, plan));
    CHECK(plan.classes[2].size() == 32u);
    // Large uint32 counts cannot overflow the ceil-div computation, and
    // capacity failure must leave the previous successful result intact.
    std::array<MeshletRange, 1> huge{{{0u, 0xffffffffu, false}}};
    CHECK(!TaskMeshletPlan::Build(huge, largeClass, 128u, plan));
    CHECK(plan.classes[2].size() == 128u);
    return 0;
}
