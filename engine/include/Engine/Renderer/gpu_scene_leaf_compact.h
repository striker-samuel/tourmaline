// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// P1 leaf work queue ABI (#372). Consumes the per-leaf mask from
// gpu_scene_leaf_classify and compacts ACTIVE leaves (mask != 0) into a
// bounded work list, one entry per active leaf carrying its stable instance
// range ONCE. Compaction is block-aggregated (one InterlockedAdd per
// workgroup), never per-instance global atomics. No production consumer yet —
// the CPU SelectPassedInstanceRangesFrame stays authoritative until the fused
// instance pass and the safe-overflow policy are in place.

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

namespace tour::gpu_scene {

// HLSL: GpuLeafWorkEntry. x leafIndex, y viewMask, z firstMember, w memberCount.
struct alignas(16) LeafWorkEntry {
    glm::uvec4 value{0u};
};
static_assert(sizeof(LeafWorkEntry) == 16);

// HLSL: GpuLeafCounters. The single GPU-written active-leaf count, consumed by
// the fused instance pass's dispatch-indirect (never read back on a normal
// frame).
struct alignas(16) LeafCounters {
    uint32_t activeLeafCount = 0;
    uint32_t reserved[3] = {};
};
static_assert(sizeof(LeafCounters) == 16);
static_assert(offsetof(LeafCounters, activeLeafCount) == 0);

// HLSL: GpuLeafCompactPush. Four BDA pointers then the counts uint4 (packed at
// offset 32 — see the InstanceCullGpuPush packing note).
struct LeafCompactPush {
    uint64_t leaves = 0;
    uint64_t masks = 0;
    uint64_t queue = 0;
    uint64_t counters = 0;
    glm::uvec4 counts{0u}; // x leafCount, y/w reserved, z zero-counters (1 = reset)
};
static_assert(sizeof(LeafCompactPush) == 48);
static_assert(offsetof(LeafCompactPush, counts) == 32);

} // namespace tour::gpu_scene
