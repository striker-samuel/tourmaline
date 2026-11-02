// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless contracts for the GPUIR World -> GPU scene bridge.
#include <Engine/Renderer/gpu_scene_world.h>
#include <Engine/Scene/scene.h>
#include <cassert>
#include <cstdio>
#include <span>
#include <vector>

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
    assert(stats.created == 2 && stats.meshesCreated == 1);
    assert(db.instances.LiveCount() == 2 && db.meshes.LiveCount() == 1);
    const auto ida = bridge.Find(a.instance), idb = bridge.Find(b.instance);
    assert(ida && idb && ida != idb);

    // Material sort order and merged node enumeration are NOT identity.
    std::swap(list[0], list[1]);
    stats = bridge.Reconcile(list, db);
    assert(stats.created == 0 && stats.retired == 0 && stats.updated == 0);
    assert(bridge.Find(a.instance) == ida && bridge.Find(b.instance) == idb);

    // Only changed global transform creates an upload. The change is
    // coalesced with the original insert until the flight is published.
    glm::mat4 moved(1.0f);
    moved[3] = glm::vec4(42.0f, 3.0f, -12.0f, 1.0f);
    assert(bridge.UpdateGlobal(a.instance, moved, db));
    assert(!bridge.UpdateGlobal(a.instance, moved, db));
    assert(db.instances.Get(ida)->worldPosition.high.x == 42.0f);
    assert(db.instances.PendingRows(0) == 2);
    assert(db.instances.Publish(0, true,
        [](size_t, const void*, size_t) { return true; }));
    assert(db.instances.PendingRows(0) == 0);
    assert(!bridge.UpdateGlobal(a.instance, moved, db));
    assert(db.instances.PendingRows(0) == 0);

    // Unload a single object: the shared mesh remains for survivor.
    stats = bridge.Reconcile(std::span<const SourceRecord>(&b, 1), db);
    assert(stats.retired == 1 && stats.meshesRetired == 0);
    assert(!db.instances.Contains(ida) && db.instances.Contains(idb));

    // A new incarnation cannot alias the old GPU handle, even if its
    // cluster-local slot and (12-bit) node generation look identical.
    SourceRecord reloaded = a;
    reloaded.instance.registration = 300;
    std::vector<SourceRecord> next{b, reloaded};
    stats = bridge.Reconcile(next, db);
    assert(stats.created == 1);
    auto fresh = bridge.Find(reloaded.instance);
    assert(fresh && fresh != ida && !db.instances.Contains(ida));

    // A new mesh incarnation is separately allocated and the last
    // reference to an old mesh causes its tombstone to be published.
    reloaded.mesh.registration = 300;
    stats = bridge.Reconcile(std::span<const SourceRecord>(&reloaded, 1), db);
    assert(stats.meshesCreated == 1 && stats.meshesRetired == 1);
    assert(db.meshes.LiveCount() == 1);

    bridge.Clear(db);
    assert(db.instances.LiveCount() == 0 && db.meshes.LiveCount() == 0);

    // Recalculation clears the existing dirty-at-level queues, but the
    // GPU scene journal keeps the successfully recomputed parent + child.
    Scene scene{};
    int parent = addNode(scene, -1, 0);
    int child = addNode(scene, parent, 1);
    scene.localTransform[parent][3] = glm::vec4(1, 2, 3, 1);
    markAsChanged(scene, parent);
    assert(recalculateGlobalTransforms(scene));
    bool sawParent = false, sawChild = false;
    for (NodeId n : scene.gpuSceneGlobalChanges) {
        if (n == uint32_t(parent)) sawParent = true;
        if (n == uint32_t(child)) sawChild = true;
    }
    assert(sawParent && sawChild);
    const size_t pendingBefore = scene.gpuSceneGlobalChanges.size();
    markAsChanged(scene, parent);
    assert(recalculateGlobalTransforms(scene));
    assert(scene.gpuSceneGlobalChanges.size() == pendingBefore);
    std::puts("gpu scene world synchronization: PASS");
    return 0;
}
