// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Rendergraph/static_graph_compiler.h>
#include <iostream>
using namespace smvk::rg;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::cerr << "FAIL " << name << '\n'; ++failures; }
    };
    auto r = [](Id id, uint64_t stage = 1, uint32_t layout = 1) {
        return Access{id, Mode::Read, stage, 1, layout};
    };
    auto w = [](Id id) { return Access{id, Mode::Write, 1, 2, 1}; };
    auto p = [](std::vector<Access> a, Id s = 0, Queue q = Queue::Graphics) {
        return Pass{s, q, a};
    };
    auto plan = Compile({p({w(0)}),p({r(0)}),p({w(1)}),p({r(1)})},2,1);
    check(plan.passBatch == std::vector<Id>{0,1,0,1}, "independent chains");
    check(plan.sections[0].graphics[1].barriers.size()==2, "batched barriers");
    plan = Compile({p({w(0)}),p({r(0),w(1)}),p({r(0,2),r(1)}),p({w(0)})},2,1);
    check(plan.passBatch == std::vector<Id>{0,1,2,3}, "read group spans batches");
    check(plan.timelines[0][1].access.stages==3, "reader union");
    check(plan.timelines[0][1].firstBatch==1 && plan.timelines[0][1].lastBatch==2, "group interval");
    check(plan.barriers[1].before.stages==3, "WAR source union");
    plan = Compile({p({r(0),w(0)}),p({w(0)})},1,1);
    check(plan.timelines[0][0].access.mode==Mode::ReadWrite && plan.passBatch[1]==1, "duplicate normalization WAW");
    plan = Compile({p({r(0)}),p({r(0,1,2)})},1,1);
    check(plan.passBatch[1]==1 && plan.barriers.size()==1, "layout change");
    plan = Compile({p({w(0)}),p({r(0)},1,Queue::Compute)},1,2);
    check(plan.barriers.size()==1 && plan.barriers[0].sourceQueue==Queue::Graphics &&
          plan.barriers[0].destinationQueue==Queue::Compute, "cross-section handoff");
    check(!CanAlias(plan.lifetimes[0],plan.lifetimes[0]), "cross-stream lifetime");
    auto rejects = [&](std::vector<Pass> a) {
        try { (void)Compile(a,1,2); } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    check(rejects({p({w(0)}),p({r(0)},0,Queue::Compute)}), "same-section shared resource");
    check(rejects({p({r(0),r(0,1,2)})}), "same-pass conflicting layouts");
    check(rejects({p({r(0)},1),p({w(0)},0)}), "section order");
    check(rejects({p({r(1)})}), "resource bounds");
    plan = Compile({p({w(0)}),p({r(0),w(1)}),p({r(1),w(2)})},3,1);
    check(!CanAlias(plan.lifetimes[0],plan.lifetimes[1]), "inclusive overlap");
    check(CanAlias(plan.lifetimes[0],plan.lifetimes[2]), "disjoint batch lifetime");
    check(Compile({},0,0).sections.empty(), "empty graph");
    if (!failures) std::cout << "Static render graph compiler tests passed\n";
    return failures ? 1 : 0;
}
