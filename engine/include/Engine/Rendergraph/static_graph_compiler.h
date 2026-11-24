// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace smvk::rg {
using Id = uint32_t;
inline constexpr Id Invalid = std::numeric_limits<Id>::max();
enum class Queue : uint8_t { Graphics, Compute };
enum class Mode : uint8_t { Read, Write, ReadWrite };

// Backend-neutral synchronization masks/layout tokens. Resource IDs must
// already be canonicalized by the declaration layer (whole resource).
struct Access {
    Id resource;
    Mode mode;
    uint64_t stages;
    uint64_t mask;
    uint32_t layout;
};
struct Pass {
    Id section;
    Queue queue;
    std::vector<Access> accesses;
};
struct Group {
    Access access;
    Id section;
    Queue queue;
    std::vector<Id> passes;
    Id firstBatch = Invalid;
    Id lastBatch = 0;
};
struct Barrier {
    Id resource;
    Access before, after;
    Id sourceSection, destinationSection;
    Queue sourceQueue, destinationQueue;
    Id destinationBatch;
};
struct Batch {
    std::vector<Id> passes;
    std::vector<Id> barriers;
};
struct Section {
    std::vector<Batch> graphics, compute;
};
struct Lifetime {
    Id firstSection = Invalid, lastSection = 0;
    Id firstBatch = Invalid, lastBatch = 0;
    Queue queue = Queue::Graphics;
    bool singleStream = true;
};
struct Plan {
    std::vector<Section> sections;
    std::vector<std::vector<Group>> timelines;
    std::vector<Barrier> barriers;
    std::vector<Lifetime> lifetimes;
    std::vector<Id> passBatch;   // per pass (compile order) -> queue-local batch
    std::vector<Id> passSection; // per pass (compile order) -> section
};

inline bool Writes(Mode mode) { return mode != Mode::Read; }

// CPU-only compiler. Sections are full submission boundaries; queue-local
// batches can overlap. Imported initial/final state and physical alias
// handoffs are deliberately left to the Vulkan lowering, not inferred here.
inline Plan Compile(const std::vector<Pass>& passes, Id resourceCount,
                    Id sectionCount) {
    if (passes.size() >= Invalid)
        throw std::invalid_argument("too many render passes");
    Plan plan;
    plan.sections.resize(sectionCount);
    plan.timelines.resize(resourceCount);
    plan.lifetimes.resize(resourceCount);
    plan.passBatch.resize(passes.size());
    plan.passSection.resize(passes.size());
    Id previousSection = 0;
    // Each pass is normalized first so duplicate declarations cannot
    // create intra-pass dependencies.
    for (Id p = 0; p < passes.size(); ++p) {
        const auto& pass = passes[p];
        if (pass.section >= sectionCount || pass.section < previousSection)
            throw std::invalid_argument("invalid render graph section order");
        if (pass.queue != Queue::Graphics && pass.queue != Queue::Compute)
            throw std::invalid_argument("invalid render graph queue");
        previousSection = pass.section;
        std::map<Id, Access> normalized;
        for (const auto& a : pass.accesses) {
            if (a.resource >= resourceCount || !a.stages || !a.mask)
                throw std::invalid_argument("invalid resource access");
            if (a.mode != Mode::Read && a.mode != Mode::Write && a.mode != Mode::ReadWrite)
                throw std::invalid_argument("invalid resource access mode");
            auto [it, inserted] = normalized.emplace(a.resource, a);
            if (!inserted) {
                auto& old = it->second;
                if (old.layout != a.layout)
                    throw std::invalid_argument("conflicting whole-resource layouts");
                if (old.mode != a.mode) old.mode = Mode::ReadWrite;
                old.stages |= a.stages;
                old.mask |= a.mask;
            }
        }
        Id batch = 0;
        for (const auto& [id, a] : normalized) {
            auto& timeline = plan.timelines[id];
            if (!timeline.empty()) {
                const auto& last = timeline.back();
                if (last.section == pass.section && last.queue != pass.queue)
                    throw std::invalid_argument("resource shared by queues in one section");
            }
            const bool join = !timeline.empty() &&
                timeline.back().section == pass.section &&
                !Writes(a.mode) && !Writes(timeline.back().access.mode) &&
                timeline.back().access.layout == a.layout;
            // A reader joining a group depends on the preceding group,
            // not its sibling readers. Incompatible layouts form a new
            // ordered group until common-layout lowering is supported.
            const size_t prior = timeline.size() - (join ? 1u : 0u);
            if (prior && timeline[prior - 1].section == pass.section)
                batch = std::max(batch, timeline[prior - 1].lastBatch + 1);
        }
        plan.passBatch[p] = batch;
        plan.passSection[p] = pass.section;
        auto& section = plan.sections[pass.section];
        auto& batches = pass.queue == Queue::Graphics ? section.graphics : section.compute;
        if (batches.size() <= batch) batches.resize(static_cast<size_t>(batch) + 1);
        batches[batch].passes.push_back(p);
        for (const auto& [id, a] : normalized) {
            auto& timeline = plan.timelines[id];
            const bool join = !timeline.empty() &&
                timeline.back().section == pass.section &&
                !Writes(a.mode) && !Writes(timeline.back().access.mode) &&
                timeline.back().access.layout == a.layout;
            if (!join) timeline.push_back(Group{a, pass.section, pass.queue, {}, Invalid, 0});
            auto& group = timeline.back();
            group.access.stages |= a.stages;
            group.access.mask |= a.mask;
            group.passes.push_back(p);
            group.firstBatch = std::min(group.firstBatch, batch);
            group.lastBatch = std::max(group.lastBatch, batch);
            auto& life = plan.lifetimes[id];
            if (life.firstSection == Invalid) {
                life.firstSection = pass.section;
                life.firstBatch = batch;
                life.queue = pass.queue;
                life.lastSection = pass.section;
                life.lastBatch = batch;
            } else {
                if (life.queue != pass.queue || life.firstSection != pass.section)
                    life.singleStream = false;
                if (life.firstSection == pass.section)
                    life.firstBatch = std::min(life.firstBatch, batch);
                if (life.lastSection != pass.section) life.lastBatch = batch;
                else life.lastBatch = std::max(life.lastBatch, batch);
                life.lastSection = pass.section;
            }
        }
    }
    for (Id id = 0; id < resourceCount; ++id) {
        const auto& timeline = plan.timelines[id];
        for (size_t i = 1; i < timeline.size(); ++i) {
            const auto& a = timeline[i-1];
            const auto& b = timeline[i];
            // Cross-section transitions are retained even for reads:
            // lowering owns the semaphore and queue boundary contracts.
            if (!Writes(a.access.mode) && !Writes(b.access.mode) &&
                a.access.layout == b.access.layout && a.section == b.section)
                continue;
            auto& section = plan.sections[b.section];
            auto& batches = b.queue == Queue::Graphics ? section.graphics : section.compute;
            batches[b.firstBatch].barriers.push_back(static_cast<Id>(plan.barriers.size()));
            plan.barriers.push_back({id, a.access, b.access, a.section, b.section,
                                     a.queue, b.queue, b.firstBatch});
        }
    }
    return plan;
}

// Conservative first implementation: only alias within one section and
// queue when inclusive batch lifetimes are disjoint. Cross-section
// aliasing needs explicit submission happens-before and memory handoffs.
inline bool CanAlias(const Lifetime& a, const Lifetime& b) {
    return a.firstSection != Invalid && b.firstSection != Invalid &&
           a.singleStream && b.singleStream && a.firstSection == b.firstSection &&
           a.queue == b.queue &&
           (a.lastBatch < b.firstBatch || b.lastBatch < a.firstBatch);
}

// Structural identity of one compiled permutation: everything that changes
// the compiled plan (pass structure, resource shapes, access patterns, queue
// sections). Camera matrices, draw counts and swapchain image indices are
// deliberately NOT part of the key (they only patch per-flight bindings).
struct GraphKey {
    uint64_t passStructure = 0;     // pass names, queue, compute, section
    uint64_t resourceStructure = 0; // resource shapes / formats / usage
    uint64_t accessStructure = 0;   // access modes, stages, masks, layouts
    bool operator==(const GraphKey&) const = default;
};

// FNV-1a hash mix (deterministic across builds; the key is a compile-time
// identity, never persisted).
inline uint64_t GraphHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

// The immutable result of compiling one permutation: the dependency/timeline
// plan plus the flattened, canonical resource order the executor binds
// per-flight handles into. Recording functions and barrier templates are
// layered by the Vulkan lowering (they need backend types), not here.
struct CompiledGraph {
    GraphKey key;
    Plan plan;
    // Dense resource id -> packed declaration-side identity (kind<<32 | index,
    // kind 0 = image, 1 = buffer), so the executor can resolve the actual
    // per-flight image/buffer handles and patch imported boundary states
    // without re-canonicalizing.
    std::vector<uint64_t> resourceIdentities;
    // Dense resource id -> raw handle key ((gen<<32) | index), parallel to
    // resourceIdentities. The executor reconstructs the Handle from these bits
    // (Handle is two uint32s, trivially copyable) to dereference the pool.
    std::vector<uint64_t> resourceHandles;
};
} // namespace smvk::rg
