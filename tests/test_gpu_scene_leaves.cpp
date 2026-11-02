// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Headless GPUIR leaf membership, coarse bounds and dirty-range contracts.
#include <Engine/Renderer/gpu_scene_world.h>
#include <Engine/Renderer/gpu_scene_leaf.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <span>
#include <vector>

using namespace tour::gpu_scene;

namespace {
using Id = Handle<Instance>;
void Apply(SceneRegistry& source, LeafRegistry& leaves, RenderDatabase& db) {
    auto deltas = source.TakeDeltas();
    for (Id id : deltas.retired) leaves.Remove(id);
    for (Id id : deltas.changed) leaves.Upsert(id, db);
    assert(leaves.Flush(db));
}

void Ack(RenderDatabase& db, uint32_t flight = 0) {
    const auto write = [](size_t, const void*, size_t) { return true; };
    assert(db.leaves.Publish(flight, true, write));
    assert(db.leafMembers.Publish(flight, true, write));
}

void CheckMembership() {
    RenderDatabase db{};
    SceneRegistry source{};
    LeafRegistry leaves{};
    SourceRecord a{}, b{};
    a.instance = {101, 1, 1};
    a.mesh = {101, 3, 1};
    a.localSphere = glm::vec4(0, 0, 0, 2);
    b = a;
    b.instance = {101, 2, 1};
    b.globalModel[3] = glm::vec4(10.0f, 0, 0, 1);
    const std::array<SourceRecord, 2> initial{a, b};
    const auto added = source.Reconcile(initial, db);
    assert(added.created == 2 && added.meshesCreated == 1);
    const Id aId = source.Find(a.instance), bId = source.Find(b.instance);
    assert(aId && bId);
    const std::array<LevelSource, 1> levels{{LevelSource{0, 9, true, true}}};
    assert(source.PublishMesh(a.mesh, levels, true, db));
    Apply(source, leaves, db);
    assert(leaves.LeafCount() == 1);
    const auto tickets = leaves.Tickets(db);
    assert(tickets.size() == 1);
    assert(tickets[0].cell == (LeafCell{0, 0}));
    assert(tickets[0].memberCount == 2 && tickets[0].boundsComplete);
    const auto leafId = tickets[0].leaf;
    const Leaf* leaf = db.leaves.Get(leafId);
    assert(leaf && leaf->range.x == tickets[0].firstMember);
    assert(leaf->halfExtents.x >= 7.0f);
    // Frobenius transform bound is deliberately larger than the model's
    // exact spectral norm. It is conservative for shear/nonuniform scale.
    const auto& first = db.leafMembers.Rows()[leaf->range.x];
    assert(first.value.ref.x == aId.slot);
    assert(first.value.ref.y == aId.generation);
    const auto& second = db.leafMembers.Rows()[leaf->range.x + 1];
    assert(second.value.ref.x == bId.slot);

    Ack(db);
    assert(db.leaves.PendingRows(0) == 0);
    assert(db.leafMembers.PendingRows(0) == 0);
    // No change: even a redundant upsert doesn't enqueue any row.
    leaves.Upsert(aId, db);
    assert(leaves.Flush(db));
    assert(db.leaves.PendingRows(0) == 0);
    assert(db.leafMembers.PendingRows(0) == 0);

    // Motion inside a cell changes only that cell's conservative bound.
    glm::mat4 moved(1.0f);
    moved[3] = glm::vec4(15.0f, 1.0f, 0.0f, 1.0f);
    assert(source.UpdateGlobal(b.instance, moved, db));
    Apply(source, leaves, db);
    assert(leaves.Tickets(db)[0].leaf == leafId);
    assert(db.leaves.PendingRows(0) == 1);
    assert(db.leafMembers.PendingRows(0) == 0);
    Ack(db);

    // Crossing zero exercises floor() for negative X, creates a new
    // leaf and removes a member from the previous contiguous range.
    moved[3] = glm::vec4(-0.5f, 0, 0, 1);
    assert(source.UpdateGlobal(b.instance, moved, db));
    Apply(source, leaves, db);
    assert(leaves.LeafCount() == 2);
    const auto split = leaves.Tickets(db);
    assert(split.size() == 2);
    assert(split[0].cell == (LeafCell{-1, 0}));
    assert(split[1].cell == (LeafCell{0, 0}));
    assert(split[0].memberCount == 1 && split[1].memberCount == 1);
    assert(split[1].leaf == leafId);
    assert(db.leafMembers.Rows()[split[0].firstMember].value.ref.y == bId.generation);

    // Missing full-LOD-chain bounds forbids whole-leaf culling even
    // though an instance still has a populated local sphere.
    assert(source.PublishMesh(a.mesh, levels, false, db));
    leaves.RefreshSafety();
    assert(leaves.Flush(db));
    for (const auto& t : leaves.Tickets(db)) assert(!t.boundsComplete);
    assert(source.PublishMesh(a.mesh, levels, true, db));
    leaves.RefreshSafety();
    assert(leaves.Flush(db));
    for (const auto& t : leaves.Tickets(db)) assert(t.boundsComplete);

    // Destroyed source is removed and cannot alias a reused instance slot.
    auto state = source.Reconcile(std::span<const SourceRecord>(&b, 1), db);
    assert(state.retired == 1);
    Apply(source, leaves, db);
    assert(!db.instances.Contains(aId));
    assert(leaves.LeafCount() == 1);
    SourceRecord c = a;
    c.instance = {102, 1, 1};
    const std::array<SourceRecord, 2> afterReload{b, c};
    assert(source.Reconcile(afterReload, db).created == 1);
    const Id cId = source.Find(c.instance);
    assert(cId && cId.slot == aId.slot && cId.generation != aId.generation);
    Apply(source, leaves, db);
    const auto expanded = leaves.Tickets(db);
    assert(expanded.size() == 2);
    const auto inZero = expanded[1];
    const auto& active = db.leafMembers.Rows()[inZero.firstMember];
    assert(active.value.ref.x == cId.slot);
    assert(active.value.ref.y == cId.generation);

    leaves.Clear(db);
    source.Clear(db);
    assert(db.leaves.LiveCount() == 0);
    assert(db.leafMembers.LiveCount() == 0);
}

void CheckLargeWorldAndShear() {
    RenderDatabase db{};
    LeafRegistry leaves{};
    const auto mesh = db.meshes.Insert(Mesh{0, 0, 0, MeshFlagCompleteBounds});
    assert(mesh);
    Instance instance{};
    instance.worldPosition = tour::math::SplitDoubleFloat(
        glm::dvec3(-1e12 + 0.125, 12.0, 1e12 + 0.5));
    instance.localSphere = glm::vec4(0, 0, 0, 4);
    instance.refs = glm::uvec4(mesh.slot, mesh.generation, 0, 0);
    // Sheared basis: Frobenius norm must include off-axis terms.
    instance.basisX = glm::vec4(1.0f, 2.0f, 0.0f, 0.0f);
    const auto id = db.instances.Insert(instance);
    leaves.Upsert(id, db);
    assert(leaves.Flush(db));
    const auto tickets = leaves.Tickets(db);
    assert(tickets.size() == 1);
    const auto expected = LeafRegistry::CellFor(instance);
    assert(tickets[0].cell == expected);
    assert(tickets[0].boundsComplete);
    const Leaf* bounds = db.leaves.Get(tickets[0].leaf);
    assert(bounds);
    assert(bounds->halfExtents.x >= 4.0f);
    assert(bounds->halfExtents.y >= 4.0f);
    const glm::dvec3 center = tour::math::JoinDoubleFloat(bounds->worldCenter);
    assert(center.x < -9e11 && center.z > 9e11);

    // A destroyed mesh referenced by a live instance makes the leaf
    // non-cullable, not invisible.
    assert(db.meshes.Erase(mesh));
    leaves.RefreshSafety();
    assert(leaves.Flush(db));
    assert(!leaves.Tickets(db)[0].boundsComplete);
}
} // namespace

int main() {
    CheckMembership();
    CheckLargeWorldAndShear();
    std::puts("gpu scene spatial leaves: PASS");
}
