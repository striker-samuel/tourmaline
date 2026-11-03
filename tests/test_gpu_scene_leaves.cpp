// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Headless GPUIR leaf membership, coarse bounds and dirty-range contracts.
#include <Engine/Renderer/gpu_scene_world.h>
#include <Engine/Renderer/gpu_scene_leaf.h>

#include <array>
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

namespace {
using Id = Handle<Instance>;
void Apply(SceneRegistry& source, LeafRegistry& leaves, RenderDatabase& db) {
    auto deltas = source.TakeDeltas();
    for (Id id : deltas.retired) leaves.Remove(id);
    for (Id id : deltas.changed) leaves.Upsert(id, db);
    CHECK(leaves.Flush(db));
}

void Ack(RenderDatabase& db, uint32_t flight = 0) {
    const auto write = [](size_t, const void*, size_t) { return true; };
    CHECK(db.leaves.Publish(flight, true, write));
    CHECK(db.leafMembers.Publish(flight, true, write));
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
    CHECK(added.created == 2 && added.meshesCreated == 1);
    const Id aId = source.Find(a.instance), bId = source.Find(b.instance);
    CHECK(aId && bId);
    const std::array<LevelSource, 1> levels{{LevelSource{0, 9, true, true}}};
    CHECK(source.PublishMesh(a.mesh, levels, true, db));
    Apply(source, leaves, db);
    CHECK(leaves.LeafCount() == 1);
    const auto tickets = leaves.Tickets(db);
    CHECK(tickets.size() == 1);
    CHECK(tickets[0].cell == (LeafCell{0, 0}));
    CHECK(tickets[0].memberCount == 2 && tickets[0].boundsComplete);
    const auto leafId = tickets[0].leaf;
    const Leaf* leaf = db.leaves.Get(leafId);
    CHECK(leaf && leaf->range.x == tickets[0].firstMember);
    CHECK(leaf->halfExtents.x >= 7.0f);
    // Frobenius transform bound is deliberately larger than the model's
    // exact spectral norm. It is conservative for shear/nonuniform scale.
    const auto& first = db.leafMembers.Rows()[leaf->range.x];
    CHECK(first.value.ref.x == aId.slot);
    CHECK(first.value.ref.y == aId.generation);
    const auto& second = db.leafMembers.Rows()[leaf->range.x + 1];
    CHECK(second.value.ref.x == bId.slot);

    Ack(db);
    CHECK(db.leaves.PendingRows(0) == 0);
    CHECK(db.leafMembers.PendingRows(0) == 0);
    // No change: even a redundant upsert doesn't enqueue any row.
    leaves.Upsert(aId, db);
    CHECK(leaves.Flush(db));
    CHECK(db.leaves.PendingRows(0) == 0);
    CHECK(db.leafMembers.PendingRows(0) == 0);

    // Motion inside a cell changes only that cell's conservative bound.
    glm::mat4 moved(1.0f);
    moved[3] = glm::vec4(15.0f, 1.0f, 0.0f, 1.0f);
    CHECK(source.UpdateGlobal(b.instance, moved, db));
    Apply(source, leaves, db);
    CHECK(leaves.Tickets(db)[0].leaf == leafId);
    CHECK(db.leaves.PendingRows(0) == 1);
    CHECK(db.leafMembers.PendingRows(0) == 0);
    Ack(db);

    // Crossing zero exercises floor() for negative X, creates a new
    // leaf and removes a member from the previous contiguous range.
    moved[3] = glm::vec4(-0.5f, 0, 0, 1);
    CHECK(source.UpdateGlobal(b.instance, moved, db));
    // Keep the local source record in sync with the move: the later
    // afterReload reconcile would otherwise see the STALE (10,0,0) pose,
    // move b back into cell {0,0}, and collapse the two leaves (this
    // desync was invisible while the asserts were NDEBUG no-ops).
    b.globalModel = moved;
    Apply(source, leaves, db);
    CHECK(leaves.LeafCount() == 2);
    const auto split = leaves.Tickets(db);
    CHECK(split.size() == 2);
    CHECK(split[0].cell == (LeafCell{-1, 0}));
    CHECK(split[1].cell == (LeafCell{0, 0}));
    CHECK(split[0].memberCount == 1 && split[1].memberCount == 1);
    CHECK(split[1].leaf == leafId);
    CHECK(db.leafMembers.Rows()[split[0].firstMember].value.ref.y == bId.generation);

    // Missing full-LOD-chain bounds forbids whole-leaf culling even
    // though an instance still has a populated local sphere.
    CHECK(source.PublishMesh(a.mesh, levels, false, db));
    leaves.RefreshSafety();
    CHECK(leaves.Flush(db));
    for (const auto& t : leaves.Tickets(db)) CHECK(!t.boundsComplete);
    CHECK(source.PublishMesh(a.mesh, levels, true, db));
    leaves.RefreshSafety();
    CHECK(leaves.Flush(db));
    for (const auto& t : leaves.Tickets(db)) CHECK(t.boundsComplete);

    // Destroyed source is removed and cannot alias a reused instance slot.
    auto state = source.Reconcile(std::span<const SourceRecord>(&b, 1), db);
    CHECK(state.retired == 1);
    Apply(source, leaves, db);
    CHECK(!db.instances.Contains(aId));
    CHECK(leaves.LeafCount() == 1);
    SourceRecord c = a;
    c.instance = {102, 1, 1};
    const std::array<SourceRecord, 2> afterReload{b, c};
    CHECK(source.Reconcile(afterReload, db).created == 1);
    const Id cId = source.Find(c.instance);
    CHECK(cId && cId.slot == aId.slot && cId.generation != aId.generation);
    Apply(source, leaves, db);
    const auto expanded = leaves.Tickets(db);
    CHECK(expanded.size() == 2);
    const auto inZero = expanded[1];
    const auto& active = db.leafMembers.Rows()[inZero.firstMember];
    CHECK(active.value.ref.x == cId.slot);
    CHECK(active.value.ref.y == cId.generation);

    leaves.Clear(db);
    source.Clear(db);
    CHECK(db.leaves.LiveCount() == 0);
    CHECK(db.leafMembers.LiveCount() == 0);
}

void CheckLargeWorldAndShear() {
    RenderDatabase db{};
    LeafRegistry leaves{};
    const auto mesh = db.meshes.Insert(Mesh{0, 0, 0, MeshFlagCompleteBounds});
    CHECK(mesh);
    Instance instance{};
    instance.worldPosition = tour::math::SplitDoubleFloat(
        glm::dvec3(-1e12 + 0.125, 12.0, 1e12 + 0.5));
    instance.localSphere = glm::vec4(0, 0, 0, 4);
    instance.refs = glm::uvec4(mesh.slot, mesh.generation, 0, 0);
    // Sheared basis: Frobenius norm must include off-axis terms.
    instance.basisX = glm::vec4(1.0f, 2.0f, 0.0f, 0.0f);
    const auto id = db.instances.Insert(instance);
    leaves.Upsert(id, db);
    CHECK(leaves.Flush(db));
    const auto tickets = leaves.Tickets(db);
    CHECK(tickets.size() == 1);
    const auto expected = LeafRegistry::CellFor(instance);
    CHECK(tickets[0].cell == expected);
    CHECK(tickets[0].boundsComplete);
    const Leaf* bounds = db.leaves.Get(tickets[0].leaf);
    CHECK(bounds);
    CHECK(bounds->halfExtents.x >= 4.0f);
    CHECK(bounds->halfExtents.y >= 4.0f);
    const glm::dvec3 center = tour::math::JoinDoubleFloat(bounds->worldCenter);
    CHECK(center.x < -9e11 && center.z > 9e11);

    // A destroyed mesh referenced by a live instance makes the leaf
    // non-cullable, not invisible.
    CHECK(db.meshes.Erase(mesh));
    leaves.RefreshSafety();
    CHECK(leaves.Flush(db));
    CHECK(!leaves.Tickets(db)[0].boundsComplete);
}
} // namespace

int main() {
    CheckMembership();
    CheckLargeWorldAndShear();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
