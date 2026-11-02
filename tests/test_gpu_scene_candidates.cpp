// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only oracle for GPUIR coarse leaves -> GPU candidate ranges.
#include <Engine/Renderer/gpu_scene_candidates.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <span>
#include <vector>

using namespace tour::gpu_scene;

namespace {
RelativeFrustum Cube(glm::dvec3 origin, double radius = 10.0) {
    RelativeFrustum view{};
    view.originWorld = origin;
    // Six inward, normalized planes in view-origin-relative coordinates.
    view.planes = {{
        { 1,  0,  0, radius},
        {-1,  0,  0, radius},
        { 0,  1,  0, radius},
        { 0, -1,  0, radius},
        { 0,  0,  1, radius},
        { 0,  0, -1, radius},
    }};
    return view;
}
Handle<Instance> Add(RenderDatabase& db, LeafRegistry& leaves,
                     Handle<Mesh> mesh, glm::dvec3 where,
                     float localRadius = 0.5f) {
    Instance value{};
    value.worldPosition = math::SplitDoubleFloat(where);
    value.localSphere = glm::vec4(0, 0, 0, localRadius);
    value.refs = glm::uvec4(mesh.slot, mesh.generation, 0, 0);
    const auto id = db.instances.Insert(value);
    assert(id);
    leaves.Upsert(id, db);
    return id;
}
void VerifyRanges(const CandidateSelection& result,
                  const RenderDatabase& db) {
    assert(result.Valid());
    uint32_t count = 0;
    for (const auto& range : result.ranges) {
        assert(range.members.z == count);
        assert(range.members.y > 0);
        count += range.members.y;
        const Handle<Leaf> leaf{range.leaf.x, range.leaf.y};
        const Leaf* row = db.leaves.Get(leaf);
        assert(row);
        assert(row->range.x == range.members.x);
        assert(row->range.y == range.members.y);
        for (uint32_t i = 0; i < range.members.y; ++i) {
            const auto& m = db.leafMembers.Rows()[range.members.x+i];
            assert(m.identity.y == 1);
            assert(db.instances.Contains({m.value.ref.x, m.value.ref.y}));
        }
    }
    assert(count == result.candidateCount);
}
void CheckCoarse() {
    RenderDatabase db{};
    LeafRegistry leaves{};
    const auto complete = db.meshes.Insert(
        Mesh{0, 1, 1, MeshFlagCompleteBounds});
    const auto unknown = db.meshes.Insert(Mesh{0, 1, 1, 0});
    assert(complete && unknown);

    const auto near = Add(db, leaves, complete, glm::dvec3(2,0,0));
    const auto minus = Add(db, leaves, complete, glm::dvec3(-1,0,0));
    const auto far = Add(db, leaves, complete, glm::dvec3(512,0,0));
    const auto failOpen = Add(db, leaves, unknown, glm::dvec3(2048,0,0));
    assert(leaves.Flush(db));
    const auto tickets = leaves.Tickets(db);
    assert(tickets.size() == 4);
    const auto view = Cube(glm::dvec3(0.0));

    const auto selection = SelectPassedInstanceRanges(db, tickets, view);
    VerifyRanges(selection, db);
    assert(selection.ranges.size() == 3);
    assert(selection.rejectedLeaves == 1);
    assert(selection.unculledLeaves == 1);
    assert(selection.candidateCount == 3);
    assert(ReferenceInstanceVisible(db, near, view));
    assert(ReferenceInstanceVisible(db, minus, view));
    assert(!ReferenceInstanceVisible(db, far, view));
    assert(ReferenceInstanceVisible(db, failOpen, view));

    const auto passAll = SelectPassedInstanceRanges(
        db, tickets, view, CandidateMode::PassAll);
    VerifyRanges(passAll, db);
    assert(passAll.ranges.size() == 4);
    assert(passAll.candidateCount == 4);

    // Overflow invalidates the complete selection, no partial output.
    auto overflow = SelectPassedInstanceRanges(
        db, tickets, view, CandidateMode::PassAll, 8, 3);
    assert(overflow.status == CandidateStatus::CapacityExceeded);
    assert(overflow.ranges.empty() && overflow.candidateCount == 0);
    overflow = SelectPassedInstanceRanges(
        db, tickets, view, CandidateMode::PassAll, 2, 8);
    assert(overflow.status == CandidateStatus::CapacityExceeded);
    assert(overflow.ranges.empty() && overflow.candidateCount == 0);

    auto badTickets = tickets;
    badTickets[0].memberCount += 1;
    const auto invalid = SelectPassedInstanceRanges(db, badTickets, view);
    assert(invalid.status == CandidateStatus::InvalidSnapshot);
    assert(invalid.ranges.empty());
    badTickets = tickets;
    badTickets.push_back(tickets.front());
    assert(SelectPassedInstanceRanges(db, badTickets, view).status ==
           CandidateStatus::InvalidSnapshot);

    auto invalidView = view;
    invalidView.planes[0] = glm::dvec4(0);
    assert(SelectPassedInstanceRanges(db, tickets, invalidView).status ==
           CandidateStatus::InvalidView);
    assert(SelectPassedInstanceRanges(db, tickets, invalidView,
            CandidateMode::PassAll).Valid());

    // Within-cell motion retains leaf identities; camera motion cannot
    // dirty the persistent scene registry.
    const auto before = leaves.Tickets(db);
    auto changed = *db.instances.Get(near);
    changed.worldPosition = math::SplitDoubleFloat(glm::dvec3(3, 0, 0));
    assert(db.instances.Update(near, changed));
    leaves.Upsert(near, db);
    assert(leaves.Flush(db));
    const auto after = leaves.Tickets(db);
    assert(before.size() == after.size());
    for (size_t i = 0; i < after.size(); ++i)
        assert(before[i].leaf == after[i].leaf);

    // Destroy/reuse a slot. Membership must refer to a NEW generation.
    leaves.Remove(far);
    assert(db.instances.Erase(far));
    const auto reincarnated = Add(db, leaves, complete, glm::dvec3(600,0,0));
    assert(reincarnated.slot == far.slot);
    assert(reincarnated.generation != far.generation);
    assert(leaves.Flush(db));
    VerifyRanges(SelectPassedInstanceRanges(db, leaves.Tickets(db), view,
        CandidateMode::PassAll), db);
    assert(!db.instances.Contains(far));
}
void CheckLargeWorld() {
    RenderDatabase db{};
    LeafRegistry leaves{};
    const auto complete = db.meshes.Insert(
        Mesh{0, 1, 1, MeshFlagCompleteBounds});
    const glm::dvec3 eye(1.0e12, 0, -1.0e12);
    const auto visible = Add(db, leaves, complete,
                             eye + glm::dvec3(0.25, 0, -2.0));
    (void)Add(db, leaves, complete, eye + glm::dvec3(256, 0, 0));
    assert(leaves.Flush(db));
    const auto view = Cube(eye);
    const auto out = SelectPassedInstanceRanges(db, leaves.Tickets(db), view);
    assert(out.Valid() && out.candidateCount == 1);
    assert(out.rejectedLeaves == 1);
    assert(ReferenceInstanceVisible(db, visible, view));
    assert(math::JoinDoubleFloat(db.instances.Get(visible)->worldPosition).x >
           9.0e11);
}
}
int main() {
    CheckCoarse();
    CheckLargeWorld();
    std::puts("gpu scene candidate selection: PASS");
    return 0;
}
