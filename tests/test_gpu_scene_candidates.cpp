// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only oracle for GPUIR coarse leaves -> GPU candidate ranges.
// House test convention: a hand-rolled Check() runner with a
// checks=/failures= summary. NEVER put side effects in the conditions:
// this file originally used raw assert(), which NDEBUG compiles to
// nothing in Release builds - assert(leaves.Flush(db)) skipped Flush
// entirely and the run walked garbage into a segfault.
#include <Engine/Renderer/gpu_scene_candidates.h>

#include <cstdio>
#include <span>
#include <vector>

using namespace tour::gpu_scene;

namespace {
int g_checks = 0;
int g_failures = 0;
void Check(bool cond, const char* name) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("FAIL %s\n", name);
    }
}

RelativeFrustum Cube(glm::dvec3 origin, double radius = 10.0) {
    RelativeFrustum view{};
    view.originWorld = origin;
    // Six inward, normalized planes in view-origin-relative coordinates.
    view.planes = {{
        {1, 0, 0, radius},
        {-1, 0, 0, radius},
        {0, 1, 0, radius},
        {0, -1, 0, radius},
        {0, 0, 1, radius},
        {0, 0, -1, radius},
    }};
    return view;
}
Handle<Instance> Add(RenderDatabase& db,
                     LeafRegistry& leaves,
                     Handle<Mesh> mesh,
                     glm::dvec3 where,
                     float localRadius = 0.5f) {
    Instance value{};
    value.worldPosition = tour::math::SplitDoubleFloat(where);
    value.localSphere = glm::vec4(0, 0, 0, localRadius);
    value.refs = glm::uvec4(mesh.slot, mesh.generation, 0, 0);
    const auto id = db.instances.Insert(value);
    Check(bool(id), "cand.add-instance");
    leaves.Upsert(id, db);
    return id;
}
bool VerifyRanges(const CandidateSelection& result, const RenderDatabase& db) {
    if (!result.Valid()) {
        Check(false, "cand.verify-valid");
        return false;
    }
    uint32_t count = 0;
    for (const auto& range : result.ranges) {
        if (range.members.z != count || range.members.y == 0) {
            Check(false, "cand.verify-range-contiguous");
            return false;
        }
        count += range.members.y;
        const Handle<Leaf> leaf{range.leaf.x, range.leaf.y};
        const Leaf* row = db.leaves.Get(leaf);
        if (!row || row->range.x != range.members.x || row->range.y != range.members.y) {
            Check(false, "cand.verify-range-matches-leaf");
            return false;
        }
        for (uint32_t i = 0; i < range.members.y; ++i) {
            const auto& m = db.leafMembers.Rows()[range.members.x + i];
            if (m.identity.y != 1 || !db.instances.Contains({m.value.ref.x, m.value.ref.y})) {
                Check(false, "cand.verify-member-live");
                return false;
            }
        }
    }
    Check(count == result.candidateCount, "cand.verify-count");
    return true;
}
void CheckCoarse() {
    RenderDatabase db{};
    LeafRegistry leaves{};
    const auto complete = db.meshes.Insert(Mesh{0, 1, 1, MeshFlagCompleteBounds});
    const auto unknown = db.meshes.Insert(Mesh{0, 1, 1, 0});
    Check(bool(complete) && bool(unknown), "cand.mesh-inserts");

    const auto near = Add(db, leaves, complete, glm::dvec3(2, 0, 0));
    const auto minus = Add(db, leaves, complete, glm::dvec3(-1, 0, 0));
    const auto far = Add(db, leaves, complete, glm::dvec3(512, 0, 0));
    const auto failOpen = Add(db, leaves, unknown, glm::dvec3(2048, 0, 0));
    Check(leaves.Flush(db), "cand.flush");
    const auto tickets = leaves.Tickets(db);
    Check(tickets.size() == 4, "cand.ticket-count");
    const auto view = Cube(glm::dvec3(0.0));

    const auto selection = SelectPassedInstanceRanges(db, tickets, view);
    Check(VerifyRanges(selection, db), "cand.selection-verifies");
    const auto frameTickets = leaves.TicketsFrame(db);
    const auto frameSelection = SelectPassedInstanceRangesFrame(db, frameTickets, view);
    Check(frameSelection.Valid(), "cand.frame-arena-selection-valid");
    Check(frameSelection.candidateCount == selection.candidateCount &&
              frameSelection.ranges.size() == selection.ranges.size(),
          "cand.frame-arena-counts-match");
    if (frameSelection.ranges.size() == selection.ranges.size()) {
        for (size_t i = 0; i < selection.ranges.size(); ++i) {
            const auto& a = frameSelection.ranges[i];
            const auto& b = selection.ranges[i];
            Check(glm::all(glm::equal(a.members, b.members)) &&
                      glm::all(glm::equal(a.leaf, b.leaf)),
                  "cand.frame-arena-range-matches");
        }
    }
    const auto frameOverflow = SelectPassedInstanceRangesFrame(
        db, frameTickets, view, CandidateMode::PassAll, 2, 8);
    Check(frameOverflow.status == CandidateStatus::CapacityExceeded &&
              frameOverflow.ranges.empty(), "cand.frame-arena-overflow-fails");
    if (!frameTickets.empty()) {
        const LeafTicket duplicates[2] = {frameTickets.front(), frameTickets.front()};
        Check(SelectPassedInstanceRangesFrame(db, duplicates, view).status ==
                  CandidateStatus::InvalidSnapshot,
              "cand.frame-arena-duplicate-fails");
    }
    Check(selection.ranges.size() == 3, "cand.three-ranges");
    Check(selection.rejectedLeaves == 1, "cand.one-rejected-leaf");
    Check(selection.unculledLeaves == 1, "cand.one-unculled-leaf");
    Check(selection.candidateCount == 3, "cand.three-candidates");
    Check(ReferenceInstanceVisible(db, near, view), "cand.near-visible");
    Check(ReferenceInstanceVisible(db, minus, view), "cand.minus-visible");
    Check(!ReferenceInstanceVisible(db, far, view), "cand.far-rejected");
    Check(ReferenceInstanceVisible(db, failOpen, view), "cand.unknown-fails-open");

    const auto passAll = SelectPassedInstanceRanges(db, tickets, view, CandidateMode::PassAll);
    Check(VerifyRanges(passAll, db), "cand.passall-verifies");
    Check(passAll.ranges.size() == 4, "cand.passall-four-ranges");
    Check(passAll.candidateCount == 4, "cand.passall-four-candidates");

    // Overflow invalidates the complete selection, no partial output.
    auto overflow = SelectPassedInstanceRanges(db, tickets, view, CandidateMode::PassAll, 8, 3);
    Check(overflow.status == CandidateStatus::CapacityExceeded, "cand.overflow-ranges");
    Check(overflow.ranges.empty() && overflow.candidateCount == 0, "cand.overflow-no-partial");
    overflow = SelectPassedInstanceRanges(db, tickets, view, CandidateMode::PassAll, 2, 8);
    Check(overflow.status == CandidateStatus::CapacityExceeded, "cand.overflow-members");
    Check(overflow.ranges.empty() && overflow.candidateCount == 0,
          "cand.overflow-members-no-partial");

    auto badTickets = tickets;
    badTickets[0].memberCount += 1;
    const auto invalid = SelectPassedInstanceRanges(db, badTickets, view);
    Check(invalid.status == CandidateStatus::InvalidSnapshot, "cand.bad-ticket-count");
    Check(invalid.ranges.empty(), "cand.bad-ticket-no-ranges");
    badTickets = tickets;
    badTickets.push_back(tickets.front());
    Check(SelectPassedInstanceRanges(db, badTickets, view).status ==
              CandidateStatus::InvalidSnapshot,
          "cand.duplicate-ticket");

    auto invalidView = view;
    invalidView.planes[0] = glm::dvec4(0);
    Check(SelectPassedInstanceRanges(db, tickets, invalidView).status ==
              CandidateStatus::InvalidView,
          "cand.degenerate-view");
    Check(SelectPassedInstanceRanges(db, tickets, invalidView, CandidateMode::PassAll).Valid(),
          "cand.passall-ignores-view");

    // Within-cell motion retains leaf identities; camera motion cannot
    // dirty the persistent scene registry.
    const auto before = leaves.Tickets(db);
    auto changed = *db.instances.Get(near);
    changed.worldPosition = tour::math::SplitDoubleFloat(glm::dvec3(3, 0, 0));
    Check(db.instances.Update(near, changed), "cand.instance-update");
    leaves.Upsert(near, db);
    Check(leaves.Flush(db), "cand.reflush");
    const auto after = leaves.Tickets(db);
    Check(before.size() == after.size(), "cand.leaf-count-stable");
    bool leavesStable = true;
    for (size_t i = 0; i < after.size(); ++i)
        leavesStable = leavesStable && before[i].leaf == after[i].leaf;
    Check(leavesStable, "cand.leaf-identity-stable");

    // Destroy/reuse a slot. Membership must refer to a NEW generation.
    leaves.Remove(far);
    Check(db.instances.Erase(far), "cand.instance-erase");
    const auto reincarnated = Add(db, leaves, complete, glm::dvec3(600, 0, 0));
    Check(reincarnated.slot == far.slot, "cand.slot-reused");
    Check(reincarnated.generation != far.generation, "cand.generation-bumped");
    Check(leaves.Flush(db), "cand.reflush-reincarnated");
    const auto newTickets = leaves.Tickets(db);
    Check(
        VerifyRanges(SelectPassedInstanceRanges(db, newTickets, view, CandidateMode::PassAll), db),
        "cand.reincarnated-verifies");
    Check(!db.instances.Contains(far), "cand.old-handle-dead");
}
void CheckLargeWorld() {
    RenderDatabase db{};
    LeafRegistry leaves{};
    const auto complete = db.meshes.Insert(Mesh{0, 1, 1, MeshFlagCompleteBounds});
    const glm::dvec3 eye(1.0e12, 0, -1.0e12);
    const auto visible = Add(db, leaves, complete, eye + glm::dvec3(0.25, 0, -2.0));
    (void)Add(db, leaves, complete, eye + glm::dvec3(256, 0, 0));
    Check(leaves.Flush(db), "cand.large-flush");
    const auto view = Cube(eye);
    const auto out = SelectPassedInstanceRanges(db, leaves.Tickets(db), view);
    Check(out.Valid() && out.candidateCount == 1, "cand.large-one-candidate");
    Check(out.rejectedLeaves == 1, "cand.large-one-rejected");
    Check(ReferenceInstanceVisible(db, visible, view), "cand.large-visible");
    Check(tour::math::JoinDoubleFloat(db.instances.Get(visible)->worldPosition).x > 9.0e11,
          "cand.large-precision-kept");
}
} // namespace

int main() {
    CheckCoarse();
    CheckLargeWorld();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
