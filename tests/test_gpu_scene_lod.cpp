// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Device-free GPU scene geometry/LOD metadata contracts.
#include <Engine/Renderer/gpu_scene_world.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace tour::gpu_scene;
namespace {
template<typename T>
void Ack(Table<T, smvk::limits::MaxFramesInFlight>& table, uint32_t flight) {
    assert(table.Publish(flight, true,
        [](size_t, const void*, size_t) { return true; }));
    assert(table.PendingRows(flight) == 0);
}
void CheckContiguousAllocations() {
    Table<Lod, smvk::limits::MaxFramesInFlight> table;
    const std::array<Lod, 4> four{{Lod{}, Lod{}, Lod{}, Lod{}}};
    auto a = table.InsertContiguous(four);
    assert(a.size() == 4);
    for (size_t i = 1; i < a.size(); ++i)
        assert(a[i].slot == a[0].slot + i);
    // Non-adjacent holes must never be mistaken for a contiguous run.
    assert(table.Erase(a[1]));
    assert(table.Erase(a[3]));
    const std::array<Lod, 2> two{{Lod{}, Lod{}}};
    auto b = table.InsertContiguous(two);
    assert(b.size() == 2 && b[1].slot == b[0].slot + 1);
    assert(b[0].slot >= 4);
    assert(table.Erase(a[0]));
    assert(table.Erase(a[2]));
    // The formerly fragmented first four slots are now free. Reuse a
    // whole contiguous range without changing either surviving row.
    auto c = table.InsertContiguous(four);
    assert(c.size() == 4 && c[0].slot == 0);
    for (size_t i = 0; i < c.size(); ++i) {
        assert(c[i].slot == i);
        assert(c[i].generation == 2);
        assert(!table.Contains(a[i]));
    }
    assert(table.Contains(b[0]) && table.Contains(b[1]));
}
void CheckMeshResidency() {
    RenderDatabase db{};
    SceneRegistry registry{};
    SourceRecord r{};
    r.instance = {22, 3, 1};
    r.mesh = {22, 51, 4};
    const std::array<SourceRecord, 1> src{r};
    auto stats = registry.Reconcile(src, db);
    assert(stats.created == 1 && stats.meshesCreated == 1);
    const auto meshId = registry.FindMesh(r.mesh);
    const auto instanceId = registry.Find(r.instance);
    assert(meshId && instanceId);
    std::array<LevelSource, 3> chain{{
        {0.0f, 4u, true, true},              // LOD0: resident and filterable
        {0.25f, InvalidIndex, false, true},  // LOD1 deferred, metadata known
        {1.5f, InvalidIndex, true, true}     // LOD2 direct-draw only
    }};
    assert(registry.PublishMesh(r.mesh, chain, true, db));
    const Mesh* mesh = db.meshes.Get(meshId);
    assert(mesh && mesh->lodCount == 3 && mesh->residentMask == 0b101u);
    assert((mesh->flags & MeshFlagCompleteBounds) != 0);
    const uint32_t base = mesh->firstLod;
    const auto& lodRows = db.lods.Rows();
    const auto& subRows = db.submeshes.Rows();
    assert(lodRows[base].identity.y == 1);
    assert((lodRows[base].value.flags & LodFlagFilterable) != 0);
    assert((lodRows[base+1].value.flags & LodFlagResident) == 0);
    assert((lodRows[base+1].value.flags & LodFlagBoundsKnown) != 0);
    assert(lodRows[base+1].value.simplifyError == 0.25f);
    assert((lodRows[base+2].value.flags & LodFlagResident) != 0);
    assert((lodRows[base+2].value.flags & LodFlagFilterable) == 0);
    assert(subRows[lodRows[base].value.firstSubmesh].value.geometryId == 4u);
    assert(subRows[lodRows[base+1].value.firstSubmesh].value.geometryId == InvalidIndex);
    assert(subRows[lodRows[base+2].value.firstSubmesh].value.geometryId == InvalidIndex);

    Ack(db.meshes, 0); Ack(db.lods, 0); Ack(db.submeshes, 0);
    // No material sort, camera, or time-driven dirty writes.
    assert(registry.PublishMesh(r.mesh, chain, true, db));
    assert(db.meshes.PendingRows(0) == 0);
    assert(db.lods.PendingRows(0) == 0);
    assert(db.submeshes.PendingRows(0) == 0);

    // Deferred sibling is promoted. Mesh + LOD1 + submesh1 change,
    // everything else retains identity and does not upload again.
    chain[1].resident = true;
    chain[1].gfxMesh = 17u;
    assert(registry.PublishMesh(r.mesh, chain, true, db));
    mesh = db.meshes.Get(meshId);
    assert(mesh->firstLod == base && mesh->residentMask == 0b111u);
    assert(db.meshes.PendingRows(0) == 1);
    assert(db.lods.PendingRows(0) == 1);
    assert(db.submeshes.PendingRows(0) == 1);
    Ack(db.meshes, 0); Ack(db.lods, 0); Ack(db.submeshes, 0);
    // A registration-table rebuild can remap geometry IDs without
    // changing physical residency or the persistent mesh handle.
    chain[0].gfxMesh = 77u;
    assert(registry.PublishMesh(r.mesh, chain, true, db));
    assert(db.meshes.PendingRows(0) == 0);
    assert(db.lods.PendingRows(0) == 0);
    assert(db.submeshes.PendingRows(0) == 1);
    Ack(db.submeshes, 0);

    // Geometry eviction publishes a false resident bit and disables
    // transient geometry reuse, including on a flight that lagged behind.
    chain[1].resident = false;
    chain[1].gfxMesh = InvalidIndex;
    assert(registry.PublishMesh(r.mesh, chain, true, db));
    assert(db.meshes.Get(meshId)->residentMask == 0b101u);
    assert(db.lods.Rows()[base+1].value.firstSubmesh ==
           lodRows[base+1].value.firstSubmesh);
    assert(db.instances.Contains(instanceId));
    assert(db.instances.Get(instanceId)->refs.x == meshId.slot);

    // Missing bounds must never advertise a GPU-culling-safe mesh.
    chain[1].boundsKnown = false;
    assert(registry.PublishMesh(r.mesh, chain, false, db));
    assert((db.meshes.Get(meshId)->flags & MeshFlagCompleteBounds) == 0);
    assert((db.lods.Rows()[base+1].value.flags & LodFlagBoundsKnown) == 0);

    // Reshaping a chain replaces its child rows but retains the mesh
    // slot/generation referenced by EVERY instance.
    const std::array<LevelSource, 2> shortChain{chain[0], chain[1]};
    assert(registry.PublishMesh(r.mesh, shortChain, false, db));
    mesh = db.meshes.Get(meshId);
    assert(mesh && mesh->lodCount == 2 && registry.FindMesh(r.mesh) == meshId);
    assert(db.instances.Get(instanceId)->refs.x == meshId.slot);
    assert(db.lods.LiveCount() == 2 && db.submeshes.LiveCount() == 2);

    stats = registry.Reconcile(std::span<const SourceRecord>{}, db);
    assert(stats.retired == 1 && stats.meshesRetired == 1);
    assert(!db.meshes.Contains(meshId));
    assert(db.lods.LiveCount() == 0 && db.submeshes.LiveCount() == 0);
    assert(db.meshes.PendingRows(0) > 0);
}
}
int main() {
    CheckContiguousAllocations();
    CheckMeshResidency();
    std::puts("gpu scene LOD metadata: PASS");
}
