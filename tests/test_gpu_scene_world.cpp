// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless contracts for the GPUIR World -> GPU scene bridge.
#include <Engine/Renderer/gpu_scene_world.h>
#include <Engine/Scene/scene.h>
#include <cstdio>
#include <span>
#include <vector>
// House test convention (AGENTS.md): the CHECK macro ALWAYS evaluates its
// condition, in every build flavor. This file originally used CHECK(),
// whose NDEBUG no-op makes Release runs vacuous - and any side effect
// inside an assert (see test_gpu_scene_candidates.cpp:
// assert(leaves.Flush(db))) was silently skipped, crashing the run.
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } \
    while (0)
namespace { int g_checks = 0; int g_failures = 0; }

using namespace tour::gpu_scene;

int main() {
    RenderDatabase db{};
    SceneRegistry bridge{};
    SourceRecord a{};
    a.instance = {100, 4, 1};
    a.mesh = {100, 11, 1};
    a.material = 6;
    SourceRecord b = a;
    b.instance = {200, 7, 2};

    std::vector<SourceRecord> list{a, b};
    auto stats = bridge.Reconcile(list, db);
    CHECK(stats.created == 2 && stats.meshesCreated == 1);
    CHECK(db.instances.LiveCount() == 2 && db.meshes.LiveCount() == 1);
    const auto ida = bridge.Find(a.instance), idb = bridge.Find(b.instance);
    CHECK(ida && idb && ida != idb);

    // Material sort order and merged node enumeration are NOT identity.
    std::swap(list[0], list[1]);
    stats = bridge.Reconcile(list, db);
    CHECK(stats.created == 0 && stats.retired == 0 && stats.updated == 0);
    CHECK(bridge.Find(a.instance) == ida && bridge.Find(b.instance) == idb);

    // Only changed global transform creates an upload. The change is
    // coalesced with the original insert until the flight is published.
    glm::mat4 moved(1.0f);
    moved[3] = glm::vec4(42.0f, 3.0f, -12.0f, 1.0f);
    CHECK(bridge.UpdateGlobal(a.instance, moved, db));
    CHECK(!bridge.UpdateGlobal(a.instance, moved, db));
    CHECK(db.instances.Get(ida)->worldPosition.high.x == 42.0f);
    CHECK(db.instances.PendingRows(0) == 2);
    CHECK(db.instances.Publish(0, true,
        [](size_t, const void*, size_t) { return true; }));
    CHECK(db.instances.PendingRows(0) == 0);
    CHECK(!bridge.UpdateGlobal(a.instance, moved, db));
    CHECK(db.instances.PendingRows(0) == 0);

    // Unload a single object: the shared mesh remains for survivor.
    stats = bridge.Reconcile(std::span<const SourceRecord>(&b, 1), db);
    CHECK(stats.retired == 1 && stats.meshesRetired == 0);
    CHECK(!db.instances.Contains(ida) && db.instances.Contains(idb));

    // A new incarnation cannot alias the old GPU handle, even if its
    // cluster-local slot and (12-bit) node generation look identical.
    SourceRecord reloaded = a;
    reloaded.instance.registration = 300;
    std::vector<SourceRecord> next{b, reloaded};
    stats = bridge.Reconcile(next, db);
    CHECK(stats.created == 1);
    auto fresh = bridge.Find(reloaded.instance);
    CHECK(fresh && fresh != ida && !db.instances.Contains(ida));

    // A new mesh incarnation is separately allocated and the last
    // reference to an old mesh causes its tombstone to be published.
    reloaded.mesh.registration = 300;
    stats = bridge.Reconcile(std::span<const SourceRecord>(&reloaded, 1), db);
    CHECK(stats.meshesCreated == 1 && stats.meshesRetired == 1);
    CHECK(db.meshes.LiveCount() == 1);

    bridge.Clear(db);
    CHECK(db.instances.LiveCount() == 0 && db.meshes.LiveCount() == 0);

    // Double-authored transforms must reach the GPU scene without first
    // collapsing into float32 absolute-world matrix translation.
    RenderDatabase largeDb{};
    SceneRegistry largeBridge{};
    SourceRecord large{};
    large.instance = {900, 1, 1};
    large.mesh = {900, 2, 1};
    large.globalModel = glm::dmat4(1.0);
    const glm::dvec3 largeOrigin(1.0e12, -1.0e12, 1.0e12);
    large.globalModel[3] = glm::dvec4(
        largeOrigin + glm::dvec3(0.25, -0.125, 0.5), 1.0);
    const auto added = largeBridge.Reconcile(
        std::span<const SourceRecord>(&large, 1), largeDb);
    CHECK(added.created == 1);
    const auto largeId = largeBridge.Find(large.instance);
    CHECK(largeId);
    const auto worldPosition =
        tour::math::JoinDoubleFloat(largeDb.instances.Get(largeId)->worldPosition);
    CHECK(glm::length(worldPosition -
        glm::dvec3(large.globalModel[3])) < 0.01);
    glm::dmat4 movedDouble = large.globalModel;
    movedDouble[3].x += 0.125;
    CHECK(largeBridge.UpdateGlobal(large.instance, movedDouble, largeDb));
    const auto movedPosition =
        tour::math::JoinDoubleFloat(largeDb.instances.Get(largeId)->worldPosition);
    CHECK(std::abs(movedPosition.x - (largeOrigin.x + 0.375)) < 0.01);
    CHECK(!largeBridge.UpdateGlobal(large.instance, movedDouble, largeDb));

    // Recalculation clears the existing dirty-at-level queues, but the
    // GPU scene journal keeps the successfully recomputed parent + child.
    Scene scene{};
    int parent = addNode(scene, -1, 0);
    int child = addNode(scene, parent, 1);
    scene.localTransform[parent][3] = glm::vec4(1, 2, 3, 1);
    markAsChanged(scene, parent);
    CHECK(recalculateGlobalTransforms(scene));
    bool sawParent = false, sawChild = false;
    for (NodeId n : scene.gpuSceneGlobalChanges) {
        if (n == uint32_t(parent)) sawParent = true;
        if (n == uint32_t(child)) sawChild = true;
    }
    CHECK(sawParent && sawChild);
    const size_t pendingBefore = scene.gpuSceneGlobalChanges.size();
    markAsChanged(scene, parent);
    CHECK(recalculateGlobalTransforms(scene));
    CHECK(scene.gpuSceneGlobalChanges.size() == pendingBefore);
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
