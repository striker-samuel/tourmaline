// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only reference + parity for the P1 flat GPU multi-view leaf
// classifier chain (#372, docs/gpu_scene_visibility_hierarchy_proposal.md):
//
//   gpu_scene_leaf_classify.cs.hlsl  -> per-leaf view mask
//   gpu_scene_leaf_compact.cs.hlsl   -> active-leaf work queue
//   gpu_scene_fused_cull.cs.hlsl     -> per-instance fused view mask
//
// This test dispatches no GPU work. It re-implements the three shaders'
// semantics with the existing CPU oracles (PossibleLeafInFrustum /
// ReferenceInstanceVisible) and pins the load-bearing correctness contract
// the GPU leaf traversal must not violate: the leaf-level frustum cull can
// never reject a leaf that holds an instance the per-instance reference cull
// considers visible in that view (no false negatives). It also pins the
// fail-open policy for incomplete bounds, multi-view mask folding, and the
// fused verdict generation check against recycled slots.
//
// House test convention (see test_gpu_scene_candidates.cpp): a hand-rolled
// Check() runner; never put side effects inside a Check condition.
#include <Engine/Renderer/gpu_scene_candidates.h>
#include <Engine/Renderer/gpu_scene_leaf.h>

#include <cstdint>
#include <cstdio>
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

// Inward, normalized cube planes in view-origin-relative coordinates,
// matching the RelativeFrustum contract (dot(n, relative) + w >= 0).
RelativeFrustum Cube(glm::dvec3 origin, double radius = 10.0) {
    RelativeFrustum view{};
    view.originWorld = origin;
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
    Check(bool(id), "leafclass.add-instance");
    leaves.Upsert(id, db);
    return id;
}

// Mirror of gpu_scene_leaf_classify.cs.hlsl mainCS: one view bit per leaf.
uint32_t ClassifyLeafMask(const Leaf* leaf, const RelativeFrustum* views, uint32_t viewCount) {
    uint32_t mask = 0u;
    for (uint32_t v = 0; v < viewCount; ++v)
        if (PossibleLeafInFrustum(*leaf, views[v]))
            mask |= (1u << v);
    return mask;
}

// Mirror of gpu_scene_fused_cull.cs.hlsl InstanceInView: the existing
// ReferenceInstanceVisible already expresses it exactly.
bool InstanceInViewMirror(const RenderDatabase& db,
                          Handle<Instance> id,
                          const RelativeFrustum& view) {
    return ReferenceInstanceVisible(db, id, view);
}

// Finds the ticket (leaf) that owns an instance by member-slot match.
const Leaf* LeafOwning(const RenderDatabase& db,
                       const std::vector<LeafTicket>& tickets,
                       Handle<Instance> id) {
    for (const LeafTicket& ticket : tickets) {
        for (uint32_t i = 0; i < ticket.memberCount; ++i) {
            const auto& member = db.leafMembers.Rows()[ticket.firstMember + i];
            if (member.identity.y == 1 && member.value.ref.x == id.slot &&
                member.value.ref.y == id.generation)
                return db.leaves.Get(ticket.leaf);
        }
    }
    return nullptr;
}

void CheckNoFalseNegatives() {
    RenderDatabase db{};
    LeafRegistry leaves{};
    const auto complete = db.meshes.Insert(Mesh{0, 1, 1, MeshFlagCompleteBounds});
    const auto unknown = db.meshes.Insert(Mesh{0, 1, 1, 0});
    Check(bool(complete) && bool(unknown), "leafclass.mesh-inserts");

    // 6 instances -> 5 leaves. (2,0,0) and (63.9,0,0) share cell (0,0); the
    // 64m cell border splits (63.9) from (64.1); far/failOpen are isolated.
    const auto near0 = Add(db, leaves, complete, glm::dvec3(2, 0, 0));
    const auto near1 = Add(db, leaves, complete, glm::dvec3(-3, 0, 4));
    const auto cellEdgeA = Add(db, leaves, complete, glm::dvec3(63.9, 0, 0));
    const auto cellEdgeB = Add(db, leaves, complete, glm::dvec3(64.1, 0, 0));
    const auto far = Add(db, leaves, complete, glm::dvec3(2048, 0, 0));
    const auto failOpen = Add(db, leaves, unknown, glm::dvec3(4096, 0, 0));
    Check(leaves.Flush(db), "leafclass.flush");

    const auto tickets = leaves.Tickets(db);
    Check(tickets.size() == 5, "leafclass.ticket-count");

    // Main + two disjoint cascade-like frustums.
    const RelativeFrustum views[3] = {
        Cube(glm::dvec3(0.0)),
        Cube(glm::dvec3(64.0, 0, 0), 6.0),
        Cube(glm::dvec3(2048.0, 0, 0), 50.0),
    };
    const uint32_t viewCount = 3;

    // The load-bearing contract: for every view, every instance the
    // per-instance cull reports visible must belong to a leaf the
    // leaf-level cull did not reject.
    for (uint32_t v = 0; v < viewCount; ++v) {
        for (const LeafTicket& ticket : tickets) {
            const Leaf* leaf = db.leaves.Get(ticket.leaf);
            Check(leaf != nullptr, "leafclass.leaf-live");
            if (!leaf)
                continue;
            const bool leafVisible = PossibleLeafInFrustum(*leaf, views[v]);
            for (uint32_t i = 0; i < ticket.memberCount; ++i) {
                const auto& member = db.leafMembers.Rows()[ticket.firstMember + i];
                if (member.identity.y != 1)
                    continue;
                const Handle<Instance> id{member.value.ref.x, member.value.ref.y};
                if (InstanceInViewMirror(db, id, views[v]))
                    Check(leafVisible, "leafclass.no-false-negative");
            }
        }
    }

    // Fail-open: unknown-bounds instance is visible in every view, and its
    // leaf must never be culled.
    for (uint32_t v = 0; v < viewCount; ++v)
        Check(ReferenceInstanceVisible(db, failOpen, views[v]),
              "leafclass.unknown-fails-open-instance");
    const Leaf* failOpenLeaf = LeafOwning(db, tickets, failOpen);
    Check(failOpenLeaf != nullptr, "leafclass.unknown-leaf-found");
    if (failOpenLeaf)
        for (uint32_t v = 0; v < viewCount; ++v)
            Check(PossibleLeafInFrustum(*failOpenLeaf, views[v]),
                  "leafclass.unknown-fails-open-leaf");

    // Sanity: far (2048,0,0) is rejected by Main (radius 10 at origin) but
    // visible in the third cascade (radius 50 centered at 2048).
    Check(!ReferenceInstanceVisible(db, far, views[0]), "leafclass.far-rejected-v0");
    Check(ReferenceInstanceVisible(db, far, views[2]), "leafclass.far-visible-v2");

    // Multi-view mask: the far leaf's mask must be exactly bit 2 (visible
    // only in the third view). Its bounds are complete so it can be culled.
    const Leaf* farLeaf = LeafOwning(db, tickets, far);
    Check(farLeaf != nullptr, "leafclass.far-leaf-found");
    if (farLeaf) {
        Check((farLeaf->range.z & LeafFlagBoundsComplete) != 0,
              "leafclass.far-leaf-bounds-complete");
        const uint32_t mask = ClassifyLeafMask(farLeaf, views, viewCount);
        Check(mask == (1u << 2), "leafclass.far-leaf-mask-bit2-only");
    }

    // cellEdgeB (64.1) is inside view 1 (centered 64, radius 6) but outside
    // Main (radius 10 at origin). Its leaf is shared only with itself, so its
    // mask should contain bit 1 and exclude bit 0.
    const Leaf* edgeBLeaf = LeafOwning(db, tickets, cellEdgeB);
    Check(edgeBLeaf != nullptr, "leafclass.edgeb-leaf-found");
    if (edgeBLeaf) {
        const uint32_t mask = ClassifyLeafMask(edgeBLeaf, views, viewCount);
        Check((mask & (1u << 1)) != 0 && (mask & (1u << 0)) == 0,
              "leafclass.edgeb-leaf-mask-v1-only");
    }
}

void CheckGenerationRejection() {
    RenderDatabase db{};
    LeafRegistry leaves{};
    const auto complete = db.meshes.Insert(Mesh{0, 1, 1, MeshFlagCompleteBounds});
    Check(bool(complete), "leafclass.gen-mesh");
    const auto id = Add(db, leaves, complete, glm::dvec3(1, 0, 0));
    Check(leaves.Flush(db), "leafclass.gen-flush");

    // A stale generation reference must not resolve to a live instance:
    // ReferenceInstanceVisible uses Handle{slot,generation} and the DB's
    // generation check rejects the recycled-slot alias.
    const Handle<Instance> stale{id.slot, id.generation + 1u};
    Check(!ReferenceInstanceVisible(db, stale, Cube(glm::dvec3(0.0))),
          "leafclass.stale-generation-rejected");

    // Erase + reuse the slot bumps generation; the old handle stays dead.
    Check(db.instances.Erase(id), "leafclass.gen-erase");
    leaves.Remove(id);
    Check(leaves.Flush(db), "leafclass.gen-reflush");
    const auto reincarnated = Add(db, leaves, complete, glm::dvec3(2, 0, 0));
    Check(reincarnated.slot == id.slot && reincarnated.generation != id.generation,
          "leafclass.slot-reused-generation-bumped");
}

void CheckDenseLeaves() {
    RenderDatabase db{};
    LeafRegistry leaves{};
    const auto complete = db.meshes.Insert(Mesh{0, 1, 1, MeshFlagCompleteBounds});
    Check(bool(complete), "leafclass.dense-mesh");

    // 4 instances -> 3 cells (2,0), (63.9,0) share cell (0,0); (100,0) is
    // cell (1,0); (200,0) is cell (3,0). Dense order is signed cell order.
    (void)Add(db, leaves, complete, glm::dvec3(2, 0, 0));
    (void)Add(db, leaves, complete, glm::dvec3(200, 0, 0));
    (void)Add(db, leaves, complete, glm::dvec3(63.9, 0, 0));
    (void)Add(db, leaves, complete, glm::dvec3(100, 0, 0));
    Check(leaves.Flush(db), "leafclass.dense-flush");

    const auto tickets = leaves.Tickets(db);
    Check(tickets.size() == 3, "leafclass.dense-ticket-count");

    DenseLeaves dense{};
    Check(BuildDenseLeaves(db, tickets, dense), "leafclass.dense-build");
    Check(dense.count == tickets.size(), "leafclass.dense-count");
    for (uint32_t i = 0; i < dense.count; ++i) {
        Check(dense.denseToSlot[i] == tickets[i].leaf.slot, "leafclass.dense-slot-map");
        const auto& src = db.leaves.Rows()[tickets[i].leaf.slot];
        Check(dense.rows[i].identity == src.identity, "leafclass.dense-identity");
        Check(dense.rows[i].value.range == src.value.range, "leafclass.dense-range");
    }

    // Fail-closed: a stale ticket generation aborts with an empty result.
    std::vector<LeafTicket> stale = tickets;
    stale[0].leaf.generation += 1u;
    DenseLeaves bad{};
    Check(!BuildDenseLeaves(db, stale, bad), "leafclass.dense-stale-fails");
    Check(bad.count == 0 && bad.rows.empty(), "leafclass.dense-stale-empty");

    // Empty ticket set is a valid zero-leaf result, not an error.
    DenseLeaves none{};
    Check(BuildDenseLeaves(db, {}, none) && none.count == 0, "leafclass.dense-empty-ok");
}

} // namespace

int main() {
    CheckNoFalseNegatives();
    CheckGenerationRejection();
    CheckDenseLeaves();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
