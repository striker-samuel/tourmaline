// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Headless contracts for the GPUIR World -> GPU scene bridge.
#include <Engine/Renderer/gpu_scene_instancing.h>
#include <Engine/Renderer/gpu_scene_pages.h>
#include <Engine/Renderer/gpu_scene_raster.h>
#include <Engine/Renderer/gpu_scene_raster_geometry.h>
#include <Engine/Renderer/gpu_scene_raster_ownership.h>
#include <Engine/Renderer/gpu_scene_validation.h>
#include <Engine/Renderer/gpu_scene_world.h>
#include <Engine/Scene/scene.h>
#include <array>
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
    // Page sizing is a pure CPU contract: no Vulkan device required.
    // BDA rows remain physically contiguous across page boundaries.
    {
        static_assert(TablePageLayout<Mesh>::RowsPerPage == 2048);
        static_assert(TablePageLayout<Lod>::RowsPerPage == 2048);
        static_assert(TablePageLayout<Submesh>::RowsPerPage == 2048);
        static_assert(TablePageLayout<Instance>::RowsPerPage == 512);
        static_assert(TablePageLayout<Leaf>::RowsPerPage == 819);
        static_assert(TablePageLayout<LeafMember>::RowsPerPage == 2048);
        size_t cap = 123, bytes = 123;
        CHECK(TablePageLayout<Instance>::Capacity(0, cap) && cap == 0);
        CHECK(TablePageLayout<Instance>::Bytes(0, bytes) && bytes == 0);
        CHECK(TablePageLayout<Instance>::Capacity(1, cap) && cap == 512);
        CHECK(TablePageLayout<Instance>::Bytes(1, bytes) && bytes == 65536);
        CHECK(TablePageLayout<Instance>::Capacity(512, cap) && cap == 512);
        CHECK(TablePageLayout<Instance>::Capacity(513, cap) && cap == 1024);
        CHECK(TablePageLayout<Leaf>::Capacity(819, cap) && cap == 819);
        CHECK(TablePageLayout<Leaf>::Bytes(819, bytes) && bytes == 65520);
        CHECK(TablePageLayout<Leaf>::Capacity(820, cap) && cap == 1638);
        CHECK(TablePageLayout<LeafMember>::Capacity(2049, cap) && cap == 4096);
        CHECK(!TablePageLayout<Instance>::Capacity(SIZE_MAX, cap));
        CHECK(!TablePageLayout<Leaf>::Bytes(SIZE_MAX, bytes));
    }
    // Streamed-LOD admission must fail closed when a geometry-only
    // metadata update expands an AABB beyond the cached instance sphere.
    {
        const glm::vec4 sphere(0.0f, 0.0f, 0.0f, 2.0f);
        CHECK(RasterSphereContainsLodBounds(
            sphere, glm::vec3(-1.0f), glm::vec3(1.0f)));
        CHECK(!RasterSphereContainsLodBounds(
            sphere, glm::vec3(-2.0f), glm::vec3(2.0f)));
        CHECK(!RasterSphereContainsLodBounds(
            sphere, glm::vec3(2.0f), glm::vec3(3.0f)));
        CHECK(!RasterSphereContainsLodBounds(
            sphere, glm::vec3(1.0f), glm::vec3(-1.0f)));
        CHECK(!RasterSphereContainsLodBounds(
            glm::vec4(0.0f, 0.0f, 0.0f, -1.0f),
            glm::vec3(-1.0f), glm::vec3(1.0f)));
    }
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
    // Indexed geometry is keyed by persistent mesh slot + generation + LOD,
    // not by a transient filter mesh number. Re-registration must not
    // renumber the descriptor, and retirement must tombstone its draw range.
    {
        RenderDatabase db{};
        const auto sub = db.submeshes.Insert(
            Submesh{0u, InvalidIndex, InvalidIndex, 0u});
        const auto lod = db.lods.Insert(
            Lod{sub.slot, 1u, 0.0f, LodFlagResident | LodFlagFilterable});
        const auto mesh = db.meshes.Insert(
            Mesh{lod.slot, 1u, 1u, MeshFlagCompleteBounds});
        CHECK(bool(sub) && bool(lod) && bool(mesh));
        std::array<tour::gfx::Mesh, 2> gfxMeshes{
            tour::gfx::Mesh{0, 1}, tour::gfx::Mesh{1, 1}};
        std::array<tour::gfx::Cluster, 2> clusters{};
        clusters[0].mesh = 0;
        clusters[0].indexOffset = 0;
        clusters[0].triCount = 2;
        clusters[1].mesh = 1;
        clusters[1].indexOffset = 6;
        clusters[1].triCount = 2;
        IndexedGeometryTables tables{
            gfxMeshes.data(), 2u, clusters.data(), 2u, 20u};
        PersistentGeometryRegistry registry{};
        CHECK(registry.Synchronize(db, tables));
        const PersistentGeometryKey key{mesh.slot, mesh.generation, 0u};
        const uint32_t stable = registry.Find(key);
        CHECK(stable != InvalidIndex);
        CHECK(registry.FromTransient(0u) == stable);
        CHECK(registry.TransientAliases().size() == 2u);
        CHECK(registry.TransientAliases()[0] == stable);
        CHECK(registry.TransientAliases()[1] == InvalidIndex);
        CHECK(registry.Descriptors()[stable].firstIndex == 20u);
        const uint64_t originalRevision = registry.Revision();
        const uint64_t originalAliasRevision = registry.AliasRevision();
        CHECK(registry.Synchronize(db, tables));
        CHECK(registry.Revision() == originalRevision);
        CHECK(registry.AliasRevision() == originalAliasRevision); // no redundant uploads

        // GfxFilter re-registers this same logical LOD under a new
        // transient ID. The persistent key and descriptor slot survive.
        CHECK(db.submeshes.Update(sub,
            Submesh{1u, InvalidIndex, InvalidIndex, 0u}));
        CHECK(registry.Synchronize(db, tables));
        CHECK(registry.Find(key) == stable);
        CHECK(registry.FromTransient(1u) == stable);
        CHECK(registry.FromTransient(0u) == InvalidIndex);
        CHECK(registry.AliasRevision() > originalAliasRevision);
        CHECK(registry.TransientAliases()[1] == stable);
        CHECK(registry.Descriptors()[stable].firstIndex == 26u);

        // Physical eviction clears the GPU descriptor instead of leaving
        // stale index offsets accessible to the next indirect work build.
        CHECK(db.lods.Update(lod, Lod{sub.slot, 1u, 0.0f, 0u}));
        CHECK(registry.Synchronize(db, tables));
        CHECK(registry.Find(key) == stable);
        CHECK(registry.Descriptors()[stable].indexCount == 0u);
        CHECK(registry.FromTransient(1u) == InvalidIndex);

        // Re-residency reuses the same identity, but a retired mesh
        // generation must NEVER inherit that descriptor slot.
        CHECK(db.lods.Update(lod, Lod{sub.slot, 1u, 0.0f,
                                        LodFlagResident | LodFlagFilterable}));
        CHECK(registry.Synchronize(db, tables));
        CHECK(registry.Find(key) == stable);
        CHECK(registry.Descriptors()[stable].indexCount == 6u);
        CHECK(db.meshes.Erase(mesh));
        const auto recycled = db.meshes.Insert(
            Mesh{lod.slot, 1u, 1u, MeshFlagCompleteBounds});
        CHECK(recycled.slot == mesh.slot && recycled.generation != mesh.generation);
        CHECK(registry.Synchronize(db, tables));
        CHECK(registry.Descriptors()[stable].indexCount == 0u);
        const auto newer = registry.Find(
            {recycled.slot, recycled.generation, 0u});
        CHECK(newer != InvalidIndex && newer != stable);
        CHECK(registry.Descriptors()[newer].indexCount == 6u);

        // A malformed published chain must fail closed without mutating
        // the last committed geometry descriptors.
        const auto beforeFailure = registry.Revision();
        CHECK(db.meshes.Update(recycled, Mesh{UINT32_MAX, 1u, 1u, 0u}));
        CHECK(!registry.Synchronize(db, tables));
        CHECK(registry.Revision() == beforeFailure);
        CHECK(registry.Descriptors()[newer].indexCount == 6u);
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
        CHECK(!sync.SourceForNode(0).registration);
        CHECK(sync.SourceForNode(UINT32_MAX).slot == InvalidIndex);

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
    // Gameplay-owned nodes are not in clusterMergedNodes, but must be
    // reconciled into GPU Scene using stable World-owned identities.
    // The fixture uses the resolved World mesh row directly; gameplay's
    // AssetManager-backed SetMesh path writes the same Scene mesh row.
    {
        smvk::World world{};
        auto owned = std::make_unique<smvk::Cluster>(nullptr);
        owned->SetOfflineMode(true);
        const int authored = addNode(owned->scene, -1, 0);
        const auto mesh = owned->staticMeshes.create(smvk::StaticMesh{});
        CHECK(SetMesh(owned->scene, uint32_t(authored), mesh.index()));
        world.RegisterCluster(std::move(owned));
        world.ResolveAndFixup_clusters();
        CHECK(!world.meshLocations.empty());
        const uint32_t worldMesh = 0;
        const NodeHandle first = world.AddNode();
        const NodeHandle second = world.AddNode();
        CHECK(world.IsGameNode(first) && world.IsGameNode(second));
        CHECK(SetMesh(world.scene, NodeHandleSlot(first), worldMesh));
        CHECK(SetMesh(world.scene, NodeHandleSlot(second), worldMesh));
        world.RefreshRuntimeSceneCaches();
        RenderDatabase snapshot{};
        WorldSynchronizer sync{};
        const auto spawned = sync.Synchronize(world, snapshot);
        CHECK(spawned.created == 3 && spawned.meshesCreated == 1);
        const SourceKey key1 = sync.SourceForNode(NodeHandleSlot(first));
        const SourceKey key2 = sync.SourceForNode(NodeHandleSlot(second));
        const SourceKey authoredKey = sync.SourceForNode(uint32_t(authored));
        CHECK(key1.registration != 0 && key1.registration != authoredKey.registration);
        CHECK(key1.registration == key2.registration && key1.slot != key2.slot);
        CHECK(key1.generation == NodeGenerationOf(world.scene, NodeHandleSlot(first)));
        CHECK(sync.Find(key1) && sync.Find(key2));
        const auto steady = sync.Synchronize(world, snapshot);
        CHECK(!steady.topologyReconciled && steady.created == 0);
        CHECK(world.RemoveNode(first));
        const auto removed = sync.Synchronize(world, snapshot);
        CHECK(removed.retired == 1 && removed.meshesRetired == 0);
        CHECK(!sync.Find(key1) && sync.Find(key2));
        const NodeHandle replacement = world.AddNode();
        CHECK(world.IsGameNode(replacement));
        CHECK(SetMesh(world.scene, NodeHandleSlot(replacement), worldMesh));
        world.RefreshRuntimeSceneCaches();
        const auto respawned = sync.Synchronize(world, snapshot);
        const SourceKey replacementKey = sync.SourceForNode(NodeHandleSlot(replacement));
        CHECK(respawned.created == 1 && respawned.meshesCreated == 0);
        CHECK(replacementKey != key1 && replacementKey.registration == key2.registration);
        CHECK(sync.Find(replacementKey) && !sync.Find(key1));
    }
    // Temporal GPU-scene seed is keyed by persistent slot + generation,
    // not by work slot, draw order or last frame's geometry ID.
    {
        std::array<uint32_t, 5> previous{{0u, 10u, 22u, 0u, 31u}};
        CHECK(RasterTemporalSeedMember(previous, {1u, 10u}));
        CHECK(RasterTemporalSeedMember(previous, {2u, 22u}));
        CHECK(!RasterTemporalSeedMember(previous, {2u, 23u}));
        CHECK(!RasterTemporalSeedMember(previous, {0u, 0u}));
        CHECK(!RasterTemporalSeedMember(previous, {3u, 1u}));
        CHECK(!RasterTemporalSeedMember(previous, {5u, 31u}));
        CHECK(!RasterTemporalSeedMember(previous, {UINT32_MAX, 31u}));
        // Work reordering may change indirect firstInstance, but never
        // the identity used for seed/remainder membership.
        std::array<InstanceReference, 3> ordered{{
            {2u, 22u}, {1u, 10u}, {4u, 31u}}};
        std::array<InstanceReference, 3> shuffled{{
            ordered[2], ordered[0], ordered[1]}};
        for (const auto& id : shuffled)
            CHECK(RasterTemporalSeedMember(previous, id));
        // No previous frame: bootstrap with a full remainder, no seed.
        CHECK(!RasterTemporalSeedMember({}, {1u, 10u}));
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
        // The GPU mask is indexed by *persistent instance slot*, not
        // draw-order. Only compatible rows carry their exact generation.
        std::array<uint32_t, 9> mask{};
        const auto find = [](SourceKey key) -> Handle<Instance> {
            if (key == SourceKey{11, 2, 1}) return {7, 19};
            if (key == SourceKey{11, 8, 1}) return {2, 31};
            if (key == SourceKey{22, 5, 1}) return {4, 9};
            return {};
        };
        CHECK(owners.BuildGenerationMask(mask, find));
        CHECK(mask[7] == 19 && mask[2] == 31);
        CHECK(mask[4] == 0 && mask[0] == 0);
        mask[7] = 100;
        CHECK(owners.BuildGenerationMask(mask, find) && mask[7] == 19);
        CHECK(!owners.BuildGenerationMask(
            std::span<uint32_t>(mask.data(), 3), find));
        CHECK(std::all_of(mask.begin(), mask.begin() + 3,
                          [](uint32_t generation) { return generation == 0; }));
        // The caller must refuse publication after a failed build.

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
        CHECK(!owners.BuildGenerationMask(mask, find));
        CHECK(std::all_of(mask.begin(), mask.end(),
                          [](uint32_t generation) { return generation == 0; }));
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
    CHECK(!RasterIndexedInstanceReady(db, current));
    const LevelSource missingBounds{0.0f, 7u, true, false};
    CHECK(bridge.PublishMesh(reloaded.mesh, std::span(&missingBounds, 1), false, db));
    CHECK(!InstanceDrawReady(db, current));
    CHECK(RasterIndexedInstanceReady(db, current)); // bounds conservatively bypassed
    const LevelSource readyLevel{0.0f, 7u, true, true};
    CHECK(bridge.PublishMesh(reloaded.mesh, std::span(&readyLevel, 1), true, db));
    CHECK(InstanceDrawReady(db, current));
    CHECK(RasterIndexedInstanceReady(db, current));

    // Multi-LOD raster admission: the GPU selector chooses the finest
    // registered resident sibling. The source's conservative bounds must
    // cover ALL cooked levels, including nonresident ones.
    const std::array<LevelSource, 3> levels{{
        {0.0f, 7u, true, true},
        {1.0f, 8u, true, true},
        {2.0f, InvalidIndex, false, true},
    }};
    CHECK(bridge.PublishMesh(reloaded.mesh, levels, true, db));
    CHECK(RasterIndexedInstanceReady(db, current));
    // LOD0 eviction is allowed: the GPU selector falls back to LOD1.
    auto fallbackLevels = levels;
    fallbackLevels[0] = {0.0f, InvalidIndex, false, true};
    CHECK(bridge.PublishMesh(reloaded.mesh, fallbackLevels, true, db));
    CHECK(RasterIndexedInstanceReady(db, current));
    // Unknown bounds invalidate the union sphere: never GPU-own a
    // multi-LOD mesh while any sibling can escape the published bounds.
    auto unknownBounds = levels;
    unknownBounds[2] = {2.0f, InvalidIndex, false, false};
    CHECK(bridge.PublishMesh(reloaded.mesh, unknownBounds, false, db));
    CHECK(!RasterIndexedInstanceReady(db, current));
    // A resident level with no registered filterable geometry is not
    // safe to draw, even when another LOD is usable.
    auto missingGeometry = levels;
    missingGeometry[1] = {1.0f, InvalidIndex, true, true};
    CHECK(bridge.PublishMesh(reloaded.mesh, missingGeometry, true, db));
    CHECK(!RasterIndexedInstanceReady(db, current));
    // No resident geometry must never result in GPU ownership.
    auto noResidency = levels;
    noResidency[0].resident = false;
    noResidency[1].resident = false;
    CHECK(bridge.PublishMesh(reloaded.mesh, noResidency, true, db));
    CHECK(!RasterIndexedInstanceReady(db, current));
    // Restore the original single-LOD fixture for generation checks.
    CHECK(bridge.PublishMesh(reloaded.mesh, std::span(&readyLevel, 1), true, db));
    CHECK(RasterIndexedInstanceReady(db, current));

    auto corrupted = *db.instances.Get(current);
    corrupted.refs.y += 1; // stale mesh generation
    CHECK(db.instances.Update(current, corrupted));
    CHECK(!InstanceDrawReady(db, current));
    CHECK(!RasterIndexedInstanceReady(db, current));
    corrupted.refs.y = meshKey.generation;
    CHECK(db.instances.Update(current, corrupted));
    CHECK(InstanceDrawReady(db, current));
    const LevelSource evicted{0.0f, InvalidIndex, false, true};
    CHECK(bridge.PublishMesh(reloaded.mesh, std::span(&evicted, 1), true, db));
    CHECK(!InstanceDrawReady(db, current));
    CHECK(!RasterIndexedInstanceReady(db, current));

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
