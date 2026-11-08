// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless contracts for the GPUIR World -> GPU scene bridge.
#include <Engine/Renderer/gpu_scene_instancing.h>
#include <Engine/Renderer/gpu_scene_raster.h>
#include <Engine/Renderer/gpu_scene_raster_geometry.h>
#include <Engine/Renderer/gpu_scene_raster_ownership.h>
#include <Engine/Renderer/gpu_scene_validation.h>
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
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)
namespace {
int g_checks = 0;
int g_failures = 0;
} // namespace

using namespace tour::gpu_scene;

int main() {
    // Every compute invocation writes a verdict header, even when it
    // rejects an invalid leaf/member. Zero-filled readback is not a valid
    // visibility verdict; the slot is either unwritten or misaddressed.
    {
        InstanceCullVerdict empty{};
        empty.value = glm::uvec4(0u);
        CHECK(IsUnwrittenCullVerdict(empty));
        InstanceCullVerdict invalid{};
        invalid.value = glm::uvec4(InvalidIndex, 0u, 0u, 0u);
        CHECK(!IsUnwrittenCullVerdict(invalid));
        InstanceCullVerdict visible{};
        visible.value = glm::uvec4(0u, 1u, 1u, 0u);
        CHECK(!IsUnwrittenCullVerdict(visible));
        InstanceCullVerdict culled{};
        culled.value = glm::uvec4(0u, 1u, 0u, 0u);
        CHECK(!IsUnwrittenCullVerdict(culled));
    }
    // GPU-generated indexed draw commands must exactly match Vulkan's ABI.
    {
        const IndexedGeometry geo{231, 400, -12, 0};
        const RasterGroup group{4, 12345, 17, 0};
        const auto cmd = MakeRasterCommand(geo, group);
        CHECK(cmd.indexCount == 231 && cmd.firstIndex == 400 &&
              cmd.vertexOffset == -12 && cmd.instanceCount == 17 &&
              cmd.firstInstance == 12345);
        const RasterGroup empty{4, 0, 0, 0};
        CHECK(MakeRasterCommand(geo, empty).instanceCount == 0);
    }
    // A Gfx mesh registration is only legal for one indexed command when
    // its cooked cluster index intervals are contiguous and in order.
    {
        std::vector<tour::gfx::Mesh> meshes;
        std::vector<tour::gfx::Cluster> clusters;
        meshes.emplace_back(tour::gfx::Mesh{0, 2});
        tour::gfx::Cluster first{};
        first.mesh = 0;
        first.indexOffset = 0;
        first.triCount = 2;
        tour::gfx::Cluster second{};
        second.mesh = 0;
        second.indexOffset = 6;
        second.triCount = 3;
        clusters.push_back(first);
        clusters.push_back(second);
        IndexedGeometryTables tables{};
        tables.meshes = meshes.data();
        tables.meshCount = uint32_t(meshes.size());
        tables.clusters = clusters.data();
        tables.clusterCount = uint32_t(clusters.size());
        tables.indexBase = 16; // absolute firstIndex = base + cluster offset
        IndexedGeometry indexed{};
        CHECK(ResolveIndexedGeometry(tables, 0, indexed));
        CHECK(indexed.indexCount == 15 && indexed.firstIndex == 16 &&
              indexed.vertexOffset == 0);
        CHECK(!ResolveIndexedGeometry(tables, 1, indexed));
        clusters[1].indexOffset = 7; // not a contiguous index interval
        CHECK(!ResolveIndexedGeometry(tables, 0, indexed));
        CHECK(indexed.indexCount == 0);
    }
    // Geometry-only revisions must not revisit CPU drawItems or reconcile
    // the complete instance registry. The empty World tests the control
    // path without requiring Vulkan or a cooked cluster.
    {
        smvk::World world{};
        RenderDatabase snapshot{};
        WorldSynchronizer sync{};
        const auto first = sync.Synchronize(world, snapshot, 1);
        CHECK(first.topologyReconciled && first.geometryRefreshed);
        const auto unchanged = sync.Synchronize(world, snapshot, 1);
        CHECK(!unchanged.topologyReconciled && !unchanged.geometryRefreshed);
        const auto geometryOnly = sync.Synchronize(world, snapshot, 2);
        CHECK(!geometryOnly.topologyReconciled && geometryOnly.geometryRefreshed);
        CHECK(geometryOnly.created == 0 && geometryOnly.updated == 0 &&
              geometryOnly.retired == 0 && geometryOnly.geometryMeshesVisited == 0);
        ++world.gpuSceneTopologyRevision;
        const auto topology = sync.Synchronize(world, snapshot, 2);
        CHECK(topology.topologyReconciled);
    }
    // Canonical static-source ownership, independent of drawItems sort order.
    // Admission never commits ownership until the GPU work is prepared.
    {
        RasterOwnershipEntry entries[] = {
            {{22, 5, 1}, false},
            {{11, 2, 1}, true},
            {{11, 8, 1}, true},
        };
        CanonicalRasterOwnership owners;
        CHECK(owners.Publish(entries));
        CHECK(owners.Valid() && owners.Size() == 3);
        CHECK(owners.OwnsLegacy({11, 2, 1}));
        owners.CommitGpuPrepared();
        CHECK(owners.OwnsGpu({11, 2, 1}));
        CHECK(owners.OwnsGpu({11, 8, 1}));
        CHECK(owners.OwnsLegacy({22, 5, 1}));
        CHECK(owners.OwnsLegacy({11, 2, 2})); // generation changed
        CHECK(owners.OwnsLegacy({99, 2, 1})); // unknown registration
        owners.BeginFrame(); // publication / pipeline failed on next flight
        CHECK(!owners.GpuActive() && owners.OwnsLegacy({11, 2, 1}));
        owners.CommitGpuPrepared();
        CHECK(owners.OwnsGpu({11, 2, 1}));
        // A dirty topology publication takes ownership back immediately.
        RasterOwnershipEntry next[] = {
            {{11, 2, 2}, true},
            {{22, 5, 1}, false},
        };
        CHECK(owners.Publish(next));
        CHECK(owners.OwnsLegacy({11, 2, 2})); // until next GPU commit
        owners.CommitGpuPrepared();
        CHECK(owners.OwnsGpu({11, 2, 2}));
        CHECK(owners.OwnsLegacy({11, 2, 1}));
        CHECK(owners.OwnsLegacy({22, 5, 1}));
        RasterOwnershipEntry duplicates[] = {
            {{33, 1, 1}, true}, {{33, 1, 1}, false},
        };
        CHECK(!owners.Publish(duplicates));
        owners.CommitGpuPrepared();
        CHECK(!owners.GpuActive() && owners.OwnsLegacy({33, 1, 1}));
        RasterOwnershipEntry invalid[] = {{{0, 1, 1}, true}};
        CHECK(!owners.Publish(invalid));
        CHECK(owners.OwnsLegacy({0, 1, 1}));
        owners.Reset();
        CHECK(!owners.Valid() && owners.Size() == 0);
    }
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
    CHECK(db.instances.Publish(0, true, [](size_t, const void*, size_t) { return true; }));
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

    // Provisional mesh registrations must never be treated as draw ready.
    const auto current = bridge.Find(reloaded.instance);
    const auto meshKey = bridge.FindMesh(reloaded.mesh);
    CHECK(!InstanceDrawReady(db, current));
    const LevelSource missingBounds{0.0f, 7u, true, false};
    CHECK(bridge.PublishMesh(reloaded.mesh, std::span(&missingBounds, 1), false, db));
    CHECK(!InstanceDrawReady(db, current));
    const LevelSource readyLevel{0.0f, 7u, true, true};
    CHECK(bridge.PublishMesh(reloaded.mesh, std::span(&readyLevel, 1), true, db));
    CHECK(InstanceDrawReady(db, current));
    auto corrupted = *db.instances.Get(current);
    corrupted.refs.y += 1; // stale mesh generation
    CHECK(db.instances.Update(current, corrupted));
    CHECK(!InstanceDrawReady(db, current));
    corrupted.refs.y = meshKey.generation;
    CHECK(db.instances.Update(current, corrupted));
    CHECK(InstanceDrawReady(db, current));
    const LevelSource evicted{0.0f, InvalidIndex, false, true};
    CHECK(bridge.PublishMesh(reloaded.mesh, std::span(&evicted, 1), true, db));
    CHECK(!InstanceDrawReady(db, current));

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
    large.globalModel[3] = glm::dvec4(largeOrigin + glm::dvec3(0.25, -0.125, 0.5), 1.0);
    const auto added = largeBridge.Reconcile(std::span<const SourceRecord>(&large, 1), largeDb);
    CHECK(added.created == 1);
    const auto largeId = largeBridge.Find(large.instance);
    CHECK(largeId);
    const auto worldPosition =
        tour::math::JoinDoubleFloat(largeDb.instances.Get(largeId)->worldPosition);
    CHECK(glm::length(worldPosition - glm::dvec3(large.globalModel[3])) < 0.01);
    glm::dmat4 movedDouble = large.globalModel;
    movedDouble[3].x += 0.125;
    CHECK(largeBridge.UpdateGlobal(large.instance, movedDouble, largeDb));
    const auto movedPosition =
        tour::math::JoinDoubleFloat(largeDb.instances.Get(largeId)->worldPosition);
    CHECK(std::abs(movedPosition.x - (largeOrigin.x + 0.375)) < 0.01);
    CHECK(!largeBridge.UpdateGlobal(large.instance, movedDouble, largeDb));

    // The instancing planner uses ONLY GPU-scene generational identities;
    // selected LOD, material, and flags remain distinct draw keys.
    RenderDatabase planDb{};
    SceneRegistry planRegistry{};
    SourceRecord i0{};
    i0.instance = {1001, 1, 1};
    i0.mesh = {1001, 5, 1};
    i0.material = 11;
    SourceRecord i1 = i0;
    i1.instance = {1001, 2, 1};
    SourceRecord i2 = i0;
    i2.instance = {1001, 3, 1};
    i2.material = 12;
    std::vector<SourceRecord> instanceSources{i0, i1, i2};
    const auto planSync = planRegistry.Reconcile(instanceSources, planDb);
    CHECK(planSync.created == 3);
    const auto h0 = planRegistry.Find(i0.instance);
    const auto h1 = planRegistry.Find(i1.instance);
    const auto h2 = planRegistry.Find(i2.instance);
    const LevelSource twoLods[2] = {{0.0f, 31u, true, true}, {0.5f, 32u, true, true}};
    CHECK(planRegistry.PublishMesh(i0.mesh, std::span(twoLods), true, planDb));
    const InstanceDrawCandidate chosen[] = {{h1, 0u},
                                            {h2, 0u},
                                            {h0, 0u},
                                            {h0, 1u},
                                            {Handle<Instance>{h0.slot, h0.generation + 1u}, 0u},
                                            {h1, 9u}};
    const auto plan = BuildInstanceDrawPlan(planDb, chosen);
    CHECK(plan.rejected == 2u);
    CHECK(plan.refs.size() == 4u && plan.ranges.size() == 3u);
    CHECK(plan.ranges[0].instanceCount == 2u);
    CHECK(plan.ranges[0].firstInstance == 0u);
    CHECK(plan.ranges[0].key.lod == 0u && plan.ranges[0].key.material == 11u);
    CHECK(plan.refs[0] == h1 && plan.refs[1] == h0);
    CHECK(plan.ranges[1].instanceCount == 1u && plan.ranges[1].key.material == 12u);
    CHECK(plan.ranges[2].instanceCount == 1u && plan.ranges[2].key.lod == 1u);
    // Eviction and stale generations are rejected; the direct fallback
    // continues to own anything that is not ready for GPU-scene raster.
    const LevelSource oneEvicted[2] = {{0.0f, 31u, true, true}, {0.5f, InvalidIndex, false, true}};
    CHECK(planRegistry.PublishMesh(i0.mesh, std::span(oneEvicted), true, planDb));
    const auto evictedPlan = BuildInstanceDrawPlan(planDb, chosen);
    CHECK(evictedPlan.rejected == 3u);
    CHECK(evictedPlan.refs.size() == 3u);
    planRegistry.Clear(planDb);
    const auto retiredPlan = BuildInstanceDrawPlan(planDb, chosen);
    CHECK(retiredPlan.refs.empty() && retiredPlan.rejected == 6u);

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
        if (n == uint32_t(parent))
            sawParent = true;
        if (n == uint32_t(child))
            sawChild = true;
    }
    CHECK(sawParent && sawChild);
    const size_t pendingBefore = scene.gpuSceneGlobalChanges.size();
    markAsChanged(scene, parent);
    CHECK(recalculateGlobalTransforms(scene));
    CHECK(scene.gpuSceneGlobalChanges.size() == pendingBefore);
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
