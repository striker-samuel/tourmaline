// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// P0 baseline + GPU visibility ABI for the GPUScene sphere-tree path (#372,
// docs/gpu_scene_visibility_hierarchy_proposal.md). This DEFINES the forward
// GPU-output interface; it is NOT wired into production yet — the CPU
// candidate selector (SelectPassedInstanceRangesFrame) and
// CandidateValidator::Prepare remain authoritative. The GPU path lands in
// P1 (flat multi-view selector) and P2 (balanced sphere tree).
//
// Lifetime split (the load-bearing invariant from the RFC):
//   persistent  — instances + static sphere topology (published on change)
//   per-flight  — traversal frontier + leaf view masks (scratch, reset/frame)
//   per-view    — compact ranges / verdicts / indirect commands (GPU output)
//
// Counters are GPU-WRITTEN and consumed by GPU dispatch-indirect; a normal
// frame NEVER reads them back to the CPU (a readback would reintroduce the
// stall this design removes). Readback exists only under test/debug. The
// overflow bit is the all-or-safe-fallback gate: a set bit means that view
// must not consume the compact result — use a conservative GPU fallback, not
// a post-hoc CPU readback.

#include <cstdint>

namespace tour::gpu_scene {

// Device-side, GPU-written frame counters. The CPU publishes capacity + epoch
// (GpuVisibilityFrame); the GPU fills these and the per-view outputs.
struct alignas(16) GpuVisibilityCounters {
    uint32_t activeViewMask; // bitmask of views culled this frame
    uint32_t frontierPeak;   // peak traversal frontier entries (debug)
    uint32_t overflowBits;   // per-view overflow flags (all-or-safe-fallback)
    uint32_t epoch;          // tree-snapshot generation this frame consumed
};
static_assert(sizeof(GpuVisibilityCounters) == 16);

// Per-view GPU visibility result. The range/candidate streams are compact
// (GPU-prefix-scattered), never maximum-stride reservations.
struct alignas(16) GpuVisibilityView {
    uint32_t firstRange;     // base into the shared compact range stream
    uint32_t rangeCount;     // emitted PassedInstanceRange count
    uint32_t candidateCount; // emitted instance-candidate count
    uint32_t viewId;         // RenderViewSet index (identity, not order)
};
static_assert(sizeof(GpuVisibilityView) == 16);

// CPU-published frame header: capacity ceilings + the snapshot epoch. The
// GPU fills the counters + per-view outputs from these published capacities,
// so per-view scratch can be reserved without a CPU round-trip.
struct alignas(16) GpuVisibilityFrame {
    uint32_t epoch;             // tree-snapshot generation
    uint32_t viewCount;         // active views this frame
    uint32_t rangeCapacity;     // max compact ranges per view
    uint32_t candidateCapacity; // max candidates per view
};
static_assert(sizeof(GpuVisibilityFrame) == 16);

inline constexpr uint32_t GpuVisibilityMaxViews = 32; // activeViewMask width

} // namespace tour::gpu_scene
