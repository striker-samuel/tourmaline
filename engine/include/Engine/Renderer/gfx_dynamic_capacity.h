// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Runtime capacity planning for the legacy GfxFilter cutover. A GPU
// allocation must be sized from the actual per-view counts, never the
// shader's historical maximum. The plan is deliberately pure CPU so
// boundary/overflow cases can run in the headless test suite.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>

namespace tour::gfx {

struct DynamicCapacity {
    uint32_t draws = 0;
    uint32_t candidates = 0;
    uint32_t survivors = 0;
};

inline constexpr size_t kDynamicPageBytes = 64u * 1024u;

// DECLARED shader-array bounds (the BDA-pointer generous-declared
// pattern): the fixed HLSL arrays behind every GPU_PTR (vk::BufferPointer
// needs complete fixed-size types) are declared at these sizes, so the
// RUNTIME capacity must never exceed them or a shader load falls outside
// its declared array. They are therefore also the fail-closed CPU
// ceilings — the successor contract of the retired fixed caps
// (kMaxDraws=65535 / kMaxBatchSlots=262144): a scene needing more is a
// loud fatal, never a silent drop. HLSL mirrors: GFX_MAX_DRAWS /
// GFX_MAX_BATCH_SLOTS (gfxfilter.hlsli) — keep all three in lockstep.
inline constexpr uint32_t kDeclaredMaxDraws = 1u << 20;      // 1,048,576 draws per view
inline constexpr uint32_t kDeclaredMaxCandidates = 1u << 22; // 4,194,304 candidate slots per view

[[nodiscard]] inline constexpr bool RoundCapacity(size_t entries,
                                                  size_t bytesPerEntry,
                                                  size_t& capacity) noexcept {
    capacity = 0;
    if (entries == 0)
        return true;
    if (bytesPerEntry == 0 || bytesPerEntry > kDynamicPageBytes)
        return false;
    // LCM(pageBytes, stride) / stride: the byte size really is page-aligned
    // even for strides such as 24 that do not divide 64 KiB.
    const size_t pageRows = kDynamicPageBytes / std::gcd(kDynamicPageBytes, bytesPerEntry);
    const size_t pages = 1 + (entries - 1) / pageRows;
    if (pages > std::numeric_limits<uint32_t>::max() / pageRows)
        return false;
    capacity = pages * pageRows;
    return true;
}

// Coupled allocations must use one coherent snapshot: each candidate
// requires one Batch, indexed-indirect and temporal seed/remainder command,
// plus two kept-bit parity regions. Draw capacity also determines the
// cull-state counter/diagnostic offsets and GPU-skinning bounds tail.
[[nodiscard]] inline constexpr bool PlanDynamicCapacity(size_t drawCount,
                                                        size_t candidateCount,
                                                        size_t drawBytes,
                                                        size_t candidateBytes,
                                                        DynamicCapacity& result) noexcept {
    size_t drawCapacity = 0;
    size_t candidateCapacity = 0;
    if (!RoundCapacity(drawCount, drawBytes, drawCapacity) ||
        !RoundCapacity(candidateCount, candidateBytes, candidateCapacity))
        return false;
    result.draws = uint32_t(drawCapacity);
    result.candidates = uint32_t(candidateCapacity);
    // Survivor/index-monoblock capacity is independent; don't silently
    // enlarge the static heap when allocating cheap candidate scratch.
    result.survivors = 65536u;
    return true;
}

// The RUNTIME entry point of the dynamic-capacity cutover: one coupled
// (view, flight) plan from the frame's exact counts, page-rounded,
// overflow-checked AND bounded by the declared shader arrays. Pure CPU
// so the boundary cases run headless (test_gfxfilter). False = the frame
// exceeds a declared bound or overflows — a fatal contract violation at
// the caller, never a silent drop.
[[nodiscard]] inline constexpr bool PlanViewCapacity(size_t drawCount,
                                                     size_t candidateCount,
                                                     size_t drawBytes,
                                                     size_t candidateBytes,
                                                     DynamicCapacity& result) noexcept {
    if (drawCount > kDeclaredMaxDraws || candidateCount > kDeclaredMaxCandidates)
        return false;
    return PlanDynamicCapacity(drawCount, candidateCount, drawBytes, candidateBytes, result);
}

// Checked byte budget for the coupled per-view/per-flight allocations.
// Strides are arguments so this stays independent of RHI and GPU headers.
struct DynamicBytes {
    size_t draws = 0;
    size_t batches = 0;
    size_t indirect = 0;
    size_t temporalCommands = 0; // each of seed and remainder
    size_t kept = 0;             // two candidate parity regions
    size_t cullState = 0;        // draw verdicts + five counters
    size_t skinBounds = 0;       // candidate clusters + draw union tail
    size_t coneDebug = 0;        // two packed float3 lines per candidate
    size_t meshBatchList = 0;
};

[[nodiscard]] inline constexpr bool CheckedBytes(size_t count,
                                                 size_t stride,
                                                 size_t& bytes) noexcept {
    bytes = 0;
    if ((count != 0 && stride == 0) ||
        (stride && count > std::numeric_limits<size_t>::max() / stride))
        return false;
    bytes = count * stride;
    return true;
}

[[nodiscard]] inline constexpr bool PlanDynamicBytes(const DynamicCapacity& cap,
                                                     size_t drawStride,
                                                     size_t batchStride,
                                                     size_t indirectStride,
                                                     size_t temporalStride,
                                                     size_t clusterStride,
                                                     DynamicBytes& out) noexcept {
    DynamicBytes plan{};
    size_t tail = 0;
    if (size_t(cap.draws) > std::numeric_limits<size_t>::max() - 5u)
        return false;
    if (!CheckedBytes(cap.draws, drawStride, plan.draws) ||
        !CheckedBytes(cap.candidates, batchStride, plan.batches) ||
        !CheckedBytes(cap.candidates, indirectStride, plan.indirect) ||
        !CheckedBytes(cap.candidates, temporalStride, plan.temporalCommands) ||
        !CheckedBytes(cap.candidates, 2u * sizeof(uint32_t), plan.kept) ||
        !CheckedBytes(size_t(cap.draws) + 5u, sizeof(uint32_t), plan.cullState) ||
        !CheckedBytes(cap.candidates, clusterStride, plan.skinBounds) ||
        !CheckedBytes(cap.draws, 4u * sizeof(uint32_t), tail) ||
        tail > std::numeric_limits<size_t>::max() - plan.skinBounds ||
        !CheckedBytes(cap.candidates, 6u * sizeof(uint32_t), plan.coneDebug) ||
        !CheckedBytes(cap.candidates, sizeof(uint32_t), plan.meshBatchList))
        return false;
    plan.skinBounds += tail;
    out = plan; // no partial plan on failure
    return true;
}

// Vulkan 1.3 guarantees at least 65535 workgroups in both X and Y.
// Flatten as gid.y * 65535 + gid.x in the matching compute shader.
struct DispatchGrid {
    uint32_t x = 0;
    uint32_t y = 0;
};
inline constexpr uint32_t kDispatchAxisGroups = 65535u;

[[nodiscard]] inline constexpr bool PlanDispatchGrid(size_t groups,
                                                      DispatchGrid& result) noexcept {
    result = {};
    if (groups == 0)
        return true;
    const size_t y = 1 + (groups - 1) / kDispatchAxisGroups;
    if (y > kDispatchAxisGroups)
        return false;
    result.x = uint32_t(groups < kDispatchAxisGroups ? groups : kDispatchAxisGroups);
    result.y = uint32_t(y);
    return true;
}

} // namespace tour::gfx
