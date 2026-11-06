// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// No Vulkan context required. Exercise the direct World instance path,
// non-Cluster leases, unchanged survivor node handles and failure gates.
#include <Engine/App/world.h>
#include <Engine/Asset/asset_residency.h>
#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <limits>
#include <string>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main() {
    int loads = 0;
    int unloads = 0;
    tour::asset::AssetResidency store([&](uint64_t id) -> std::shared_ptr<void> {
        if (id == 999) return {};
        ++loads;
        return std::shared_ptr<void>(new uint64_t(id), [&](void* p) {
            ++unloads;
            delete static_cast<uint64_t*>(p);
        });
    });
    std::vector<tour::asset::AssetLease> leaseA, leaseB;
    const std::vector<uint64_t> deps{1234, 5678};
    CHECK(store.AcquireAll(deps, leaseA));
    CHECK(store.AcquireAll(deps, leaseB));
    CHECK(loads == 2 && store.IsHeld(1234) && store.IsHeld(5678));
    CHECK(leaseA[0] == leaseB[0]);
    std::vector<tour::asset::AssetLease> unchanged = leaseB;
    CHECK(!store.AcquireAll(std::vector<uint64_t>{5, 999}, unchanged));
    CHECK(unchanged.size() == 2 && !store.IsHeld(5));
    CHECK(loads == 3 && unloads == 1);

    Scene source;
    const int sr = addNode(source, -1, 0);
    const int child = addNode(source, sr, 1);
    setNodeName(source, sr, "Building");
    setNodeName(source, child, "Room");
    glm::dmat4 local(1.0);
    local[3][0] = 3.0;
    CHECK(tour::scene::SetLocalTransform(
        source, MakeNodeHandle(source, NodeId(child)), local));

    tour::World world;
    CHECK(world.SetActiveMapIdentity(100));
    std::string error;
    const uint64_t a = world.InstantiateSceneHierarchy(
        source, 9001, glm::dmat4(1.0), std::move(leaseA), error);
    CHECK(a != 0 && error.empty());
    const auto* ai = world.worldSceneInstances.Find(a);
    CHECK(ai && ai->assets.size() == 2);
    CHECK(world.regions.Find(ai->region, world.scene) != nullptr);
    const NodeHandle firstRoot = ai->root;
    const NodeHandle firstChild = ai->sourceNodes[size_t(child)];
    CHECK(NodeHandleValid(world.scene, firstChild));
    CHECK(world.scene.hierarchy[NodeHandleSlot(firstChild)].parent ==
        int(NodeHandleSlot(ai->sourceNodes[size_t(sr)])));

    glm::dmat4 secondFrame(1.0);
    secondFrame[3][0] = 128.0;
    const uint64_t b = world.InstantiateSceneHierarchy(
        source, 9001, secondFrame, std::move(leaseB), error);
    CHECK(b && b != a && world.worldSceneInstances.Count() == 2);
    CHECK(!world.SetActiveMapIdentity(101));
    const auto* bi = world.worldSceneInstances.Find(b);
    const NodeHandle surviving = bi->sourceNodes[size_t(child)];
    CHECK(NodeHandleValid(world.scene, surviving));
    CHECK(world.RetireSceneHierarchy(a));
    CHECK(!NodeHandleValid(world.scene, firstRoot));
    CHECK(!NodeHandleValid(world.scene, firstChild));
    CHECK(NodeHandleValid(world.scene, surviving));
    CHECK(store.IsHeld(1234) && unloads == 1);

    // Refuse unknown/stale instance IDs and incomplete component publication.
    CHECK(!world.RetireSceneHierarchy(a));
    CHECK(!world.SetActiveMapIdentity(101));
    Scene drawable;
    int meshNode = addNode(drawable, -1, 0);
    CHECK(SetMesh(drawable, NodeId(meshNode), 0));
    const size_t before = world.scene.hierarchy.size();
    CHECK(!world.InstantiateSceneHierarchy(
        drawable, 9010, glm::dmat4(1.0), {}, error));
    CHECK(error.find("binding") != std::string::npos);
    CHECK(world.scene.hierarchy.size() == before);
    {
        // Exercise the generic Scene -> World row remapper without a GPU.
        // World::InstantiateSceneHierarchy additionally verifies that each
        // World index resolves to an actually resident mesh/material.
        Scene mappedTarget;
        tour::RegionRegistry regionStore;
        tour::scene::WorldSceneInstances direct;
        CHECK(direct.SetActiveMap(200));
        tour::scene::WorldSceneResourceMap remap;
        remap.mesh = {77};
        Scene drawable2;
        const int node = addNode(drawable2, -1, 0);
        CHECK(SetMesh(drawable2, NodeId(node), 0));
        const uint64_t instance = direct.Instantiate(
            mappedTarget, regionStore, drawable2, 9020, glm::dmat4(1.0),
            {}, error, &remap);
        CHECK(instance != 0 && error.empty());
        if (const auto* inst = direct.Find(instance)) {
            const NodeId mappedNode = NodeHandleSlot(inst->sourceNodes[size_t(node)]);
            const auto* binding = FindMesh(mappedTarget, mappedNode);
            CHECK(binding && binding->mesh == 77);
        } else CHECK(false);
        CHECK(direct.Retire(mappedTarget, regionStore, instance));
        CHECK(mappedTarget.tables.meshes.empty());
    }
    glm::dmat4 bad(1.0);
    bad[3][0] = std::numeric_limits<double>::infinity();
    CHECK(!world.InstantiateSceneHierarchy(source, 9001, bad, {}, error));
    CHECK(error.find("nonfinite") != std::string::npos);

    CHECK(world.RetireSceneHierarchy(b));
    CHECK(unloads == 1); // `unchanged` test vector still holds both leases
    unchanged.clear();
    CHECK(unloads == 3 && !store.IsHeld(1234));
    store.Prune();
    CHECK(world.SetActiveMapIdentity(101));
    CHECK(world.ActiveMapIdentity() == 101);

    // Deleted slot reuse cannot resurrect a stale generation-checked handle.
    const uint64_t c = world.InstantiateSceneHierarchy(
        source, 9001, glm::dmat4(1.0), {}, error);
    CHECK(c != 0);
    CHECK(!NodeHandleValid(world.scene, firstChild));
    CHECK(world.RetireSceneHierarchy(c));

    std::printf("world_scene_instance_tests: %d checks, %d failures\n",
        checks, failures);
    return failures ? 1 : 0;
}
