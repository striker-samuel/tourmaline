#pragma once
// Centralized default allocation-chunk sizes (Phase 0 infrastructure).
//
// SINGLE PLACE TO RESIZE: every default chunk size in the linear-allocator
// migration lives here as a constexpr. To retune memory usage, edit the
// values below — no other file hard-codes a default size. Call sites may
// still pass explicit overrides, but the defaults always come from here.
//
// Lifetime model (see ALLOCATION_ANALYSIS.md):
//   - Process   : reserved once at startup, never reset (VulkanCore,
//                 Submitter, AppArena blocks, resource pools).
//   - Frame     : bumped per frame, reset at frame start (FrameArena
//                 blocks, RenderGraph temporaries).
//   - Transient : bumped sub-frame, reset after GPU sync (staging rings,
//                 readback, ThreadPool payloads).
//
// OOM policy (global): crash + report. LinearAllocator calls
// mem::ReportOomAndCrash() on exhaustion — there are no survival,
// fallback, or compaction strategies by design.

#include <cstddef>

namespace smvk::mem::config {

inline constexpr size_t kKiB = size_t(1) << 10;
inline constexpr size_t kMiB = size_t(1) << 20;
inline constexpr size_t kGiB = size_t(1) << 30;

// -- Arena block sizes (engine/src/Memory/arena.cpp) -----------------------
// AppArena: process-lifetime strings/URIs. FrameArena: per-frame scratch.
// Both default to 1 MiB (matches the pre-migration Arena behavior).
inline constexpr size_t kAppArenaBlock = 1 * kMiB;
inline constexpr size_t kFrameArenaBlock = 1 * kMiB;

// -- Linear allocator virtual-address reservations --------------------------
// Reserved (not committed) at startup via VirtualAlloc MEM_RESERVE on
// Windows. Reservation costs address space only — physical pages commit
// on demand in kCommitQuantum chunks.
inline constexpr size_t kProcessReserve = 4 * kGiB;
inline constexpr size_t kFrameReserve = 256 * kMiB;
inline constexpr size_t kTransientReserve = 1 * kGiB;

// -- Upload staging (UploadStreamer rings, VMA host-visible buffers) ---------
// Total streamer budget split across shards at init (init divides by 8 and
// clamps to the per-thread floor); shard rings lazily create on first
// upload so unused shards cost nothing. GPU-side VMA memory — the sizes
// live here so CPU and GPU budgets resize in one place.
inline constexpr size_t kStreamerTotalBudget = 256 * kMiB;
inline constexpr size_t kStagingRingPerThread = 32 * kMiB;
inline constexpr size_t kBulkRingCapacity = 512 * kMiB;
// Batch submit threshold: a shard submits its transfer batch once staged
// bytes since flush exceed this.
inline constexpr size_t kUploadFlushThreshold = 256 * kMiB;

// -- Readback staging (DownloadDataFromBuffer, VMA host-cached buffer) ------
// Grow-on-demand doubling from the floor; requests above the cap and VMA
// allocation failures crash (no graceful degradation — see OOM policy).
inline constexpr size_t kReadbackStagingMin = 256 * kKiB;
inline constexpr size_t kReadbackStagingMax = 256 * kMiB;

// -- ThreadPool task ring ----------------------------------------------------
// Bounded task queue: ring slots pre-allocated in transient memory at pool
// construction. Enqueue blocks when full (backpressure — the queue never
// grows and never OOMs). Sized for worst-case fan-out (pak decompress +
// GI bakes enqueue in the low thousands); costs ~160 KB of transient VA.
inline constexpr size_t kThreadPoolQueueSlots = 4096;

// -- Page-commit granularity ------------------------------------------------
// Physical pages commit in multiples of this quantum as the bump pointer
// advances. Larger = fewer syscalls, coarser working set. Must be a
// multiple of the OS page size (4 KiB).
inline constexpr size_t kCommitQuantum = 16 * kMiB;

// -- Default bump alignment --------------------------------------------------
inline constexpr size_t kDefaultAlign = 16;

} // namespace smvk::mem::config
