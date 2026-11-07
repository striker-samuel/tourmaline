// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Device-free GPU scene geometry/LOD metadata contracts.
#include <Engine/Renderer/gpu_scene_world.h>
#include <array>
#include <cstdint>
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
namespace {
template <typename T> void Ack(Table<T, smvk::limits::MaxFramesInFlight>& table, uint32_t flight) {
    CHECK(table.Publish(flight, true, [](size_t, const void*, size_t) { return true; }));
    CHECK(table.PendingRows(flight) == 0);
}
void CheckContiguousAllocations() {
    Table<Lod, smvk::limits::MaxFramesInFlight> table;
    const std::array<Lod, 4> four{{Lod{}, Lod{}, Lod{}, Lod{}}};
    auto a = table.InsertContiguous(four);
    CHECK(a.size() == 4);
    for (size_t i = 1; i < a.size(); ++i)
        CHECK(a[i].slot == a[0].slot + i);
    // Non-adjacent holes must never be mistaken for a contiguous run.
    CHECK(table.Erase(a[1]));
    CHECK(table.Erase(a[3]));
    const std::array<Lod, 2> two{{Lod{}, Lod{}}};
    auto b = table.InsertContiguous(two);
    CHECK(b.size() == 2 && b[1].slot == b[0].slot + 1);
    CHECK(b[0].slot >= 4);
    CHECK(table.Erase(a[0]));
    CHECK(table.Erase(a[2]));
    // The formerly fragmented first four slots are now free. Reuse a
    // whole contiguous range without changing either surviving row.
    auto c = table.InsertContiguous(four);
    CHECK(c.size() == 4 && c[0].slot == 0);
    for (size_t i = 0; i < c.size(); ++i) {
        CHECK(c[i].slot == i);
        CHECK(c[i].generation == 2);
        CHECK(!table.Contains(a[i]));
    }
    CHECK(table.Contains(b[0]) && table.Contains(b[1]));
}
void CheckMeshResidency() {
    RenderDatabase db{};
    SceneRegistry registry{};
    SourceRecord r{};
    r.instance = {22, 3, 1};
    r.mesh = {22, 51, 4};
    const std::array<SourceRecord, 1> src{r};
    auto stats = registry.Reconcile(src, db);
    CHECK(stats.created == 1 && stats.meshesCreated == 1);
    const auto meshId = registry.FindMesh(r.mesh);
    const auto instanceId = registry.Find(r.instance);
    CHECK(meshId && instanceId);
    CHECK(db.instances.Get(instanceId)->refs.y == meshId.generation);
    std::array<LevelSource, 3> chain{{
        {0.0f, 4u, true, true},             // LOD0: resident and filterable
        {0.25f, InvalidIndex, false, true}, // LOD1 deferred, metadata known
        {1.5f, InvalidIndex, true, true}    // LOD2 direct-draw only
    }};
    CHECK(registry.PublishMesh(r.mesh, chain, true, db));
    const Mesh* mesh = db.meshes.Get(meshId);
    CHECK(mesh && mesh->lodCount == 3 && mesh->residentMask == 0b101u);
    CHECK((mesh->flags & MeshFlagCompleteBounds) != 0);
    const uint32_t base = mesh->firstLod;
    const auto& lodRows = db.lods.Rows();
    const auto& subRows = db.submeshes.Rows();
    CHECK(lodRows[base].identity.y == 1);
    CHECK((lodRows[base].value.flags & LodFlagFilterable) != 0);
    CHECK((lodRows[base + 1].value.flags & LodFlagResident) == 0);
    CHECK((lodRows[base + 1].value.flags & LodFlagBoundsKnown) != 0);
    CHECK(lodRows[base + 1].value.simplifyError == 0.25f);
    CHECK((lodRows[base + 2].value.flags & LodFlagResident) != 0);
    CHECK((lodRows[base + 2].value.flags & LodFlagFilterable) == 0);
    CHECK(subRows[lodRows[base].value.firstSubmesh].value.geometryId == 4u);
    CHECK(subRows[lodRows[base + 1].value.firstSubmesh].value.geometryId == InvalidIndex);
    CHECK(subRows[lodRows[base + 2].value.firstSubmesh].value.geometryId == InvalidIndex);

    Ack(db.meshes, 0);
    Ack(db.lods, 0);
    Ack(db.submeshes, 0);
    // No material sort, camera, or time-driven dirty writes.
    CHECK(registry.PublishMesh(r.mesh, chain, true, db));
    CHECK(db.meshes.PendingRows(0) == 0);
    CHECK(db.lods.PendingRows(0) == 0);
    CHECK(db.submeshes.PendingRows(0) == 0);

    // Deferred sibling is promoted. Mesh + LOD1 + submesh1 change,
    // everything else retains identity and does not upload again.
    chain[1].resident = true;
    chain[1].gfxMesh = 17u;
    CHECK(registry.PublishMesh(r.mesh, chain, true, db));
    mesh = db.meshes.Get(meshId);
    CHECK(mesh->firstLod == base && mesh->residentMask == 0b111u);
    CHECK(db.meshes.PendingRows(0) == 1);
    CHECK(db.lods.PendingRows(0) == 1);
    CHECK(db.submeshes.PendingRows(0) == 1);
    Ack(db.meshes, 0);
    Ack(db.lods, 0);
    Ack(db.submeshes, 0);
    // A registration-table rebuild can remap geometry IDs without
    // changing physical residency or the persistent mesh handle.
    chain[0].gfxMesh = 77u;
    CHECK(registry.PublishMesh(r.mesh, chain, true, db));
    CHECK(db.meshes.PendingRows(0) == 0);
    CHECK(db.lods.PendingRows(0) == 0);
    CHECK(db.submeshes.PendingRows(0) == 1);
    Ack(db.submeshes, 0);

    // Geometry eviction publishes a false resident bit and disables
    // transient geometry reuse, including on a flight that lagged behind.
    chain[1].resident = false;
    chain[1].gfxMesh = InvalidIndex;
    CHECK(registry.PublishMesh(r.mesh, chain, true, db));
    CHECK(db.meshes.Get(meshId)->residentMask == 0b101u);
    CHECK(db.lods.Rows()[base + 1].value.firstSubmesh == lodRows[base + 1].value.firstSubmesh);
    CHECK(db.instances.Contains(instanceId));
    CHECK(db.instances.Get(instanceId)->refs.x == meshId.slot);

    // Missing bounds must never advertise a GPU-culling-safe mesh.
    chain[1].boundsKnown = false;
    CHECK(registry.PublishMesh(r.mesh, chain, false, db));
    CHECK((db.meshes.Get(meshId)->flags & MeshFlagCompleteBounds) == 0);
    CHECK((db.lods.Rows()[base + 1].value.flags & LodFlagBoundsKnown) == 0);

    // Reshaping a chain replaces its child rows but retains the mesh
    // slot/generation referenced by EVERY instance.
    const std::array<LevelSource, 2> shortChain{chain[0], chain[1]};
    CHECK(registry.PublishMesh(r.mesh, shortChain, false, db));
    mesh = db.meshes.Get(meshId);
    CHECK(mesh && mesh->lodCount == 2 && registry.FindMesh(r.mesh) == meshId);
    CHECK(db.instances.Get(instanceId)->refs.x == meshId.slot);
    CHECK(db.lods.LiveCount() == 2 && db.submeshes.LiveCount() == 2);

    stats = registry.Reconcile(std::span<const SourceRecord>{}, db);
    CHECK(stats.retired == 1 && stats.meshesRetired == 1);
    CHECK(!db.meshes.Contains(meshId));
    CHECK(db.lods.LiveCount() == 0 && db.submeshes.LiveCount() == 0);
    CHECK(db.meshes.PendingRows(0) > 0);
}
} // namespace
int main() {
    CheckContiguousAllocations();
    CheckMeshResidency();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
